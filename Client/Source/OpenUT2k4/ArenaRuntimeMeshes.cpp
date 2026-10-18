// Runtime static meshes: cache glTF / generated geometry -> UStaticMesh (fast build, no Nanite, no lightmap
// UVs, no distance field) with the editor importer's collision setup (Tools/ImportUT2004.py setup_mesh):
// one material slot per (material, collides) pair, complex-as-simple collision from the colliding sections,
// double-sided collision geometry.
//
// Collision cooking: cooked builds have no cooked collision for these meshes, so Chaos cooks them at runtime.
// UBodySetup::CreatePhysicsMeshes only does that for bodies whose outer chain reaches a game world, which is
// why runtime assets live in the game instance (AssetOuter). CookCollision cooks many meshes in parallel.

#include "ArenaRuntimeContentPrivate.h"
#include "ArenaRuntimeGltf.h"
#include "OpenUT2k4.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/ScopeLock.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace ArenaRuntimeMeshes
{
	using namespace ArenaRuntimePrivate;

	/**
	 * UStaticMesh::BuildFromMeshDescriptions, also in headless cooked runs (the packaged dedicated server, -nullrhi):
	 * there FStaticMeshRenderData::InitResources asserts ("RenderData should not initialize resources in headless cooked
	 * runs"), so the CPU side of the render data is built without it. That is all collision needs; nothing renders.
	 */
	bool BuildStaticMeshHeadlessSafe(UStaticMesh* SM, const FMeshDescription& MD, const UStaticMesh::FBuildMeshDescriptionsParams& Params)
	{
		if (FApp::CanEverRender() || !FPlatformProperties::RequiresCookedData())
		{
			return SM->BuildFromMeshDescriptions({ &MD }, Params);
		}
		SM->NeverStream = true;
		SM->bAllowCPUAccess = Params.bAllowCpuAccess;
		SM->SetRenderData(MakeUnique<FStaticMeshRenderData>());
		SM->GetRenderData()->AllocateLODResources(1);
		FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
		LOD.IndexBuffer.TrySetAllowCPUAccess(Params.bAllowCpuAccess);
		SM->BuildFromMeshDescription(MD, LOD);
		SM->GetRenderData()->Bounds = MD.GetBounds();
		SM->CalculateExtendedBounds();
		SM->GetRenderData()->ScreenSize[0].Default = 1.f;
		SM->CreateBodySetup();
		SM->GetBodySetup()->InvalidatePhysicsData();
		return true;
	}
	using ArenaRuntimeContent::FMeshSection;
	using ArenaRuntimeContent::EMaterialVariant;

	/** Parsed cache mesh: one FMeshSection per material slot, plus the glTF section -> slot map. */
	struct FMeshData
	{
		bool bValid = false;
		TArray<FMeshSection> Slots;
		TMap<int32, int32> SectionSlot;
		/** UE2 collision model ("collision_model" primitive; empty without one) and who collides with it. */
		FMeshSection CollisionModel;
		bool bModelBox = false;
		bool bModelLine = false;
	};
	using FMeshDataPtr = TSharedPtr<FMeshData, ESPMode::ThreadSafe>;

	FCriticalSection DataLock;
	TMap<FString, FMeshDataPtr> DataCache;

	bool IsFinite(const FVector3f& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); }

	FMeshDataPtr Parse(const FString& ObjectPath)
	{
		FMeshDataPtr Out = MakeShared<FMeshData, ESPMode::ThreadSafe>();
		const FString Base = FindByObjectPath(TEXT("staticmeshes"), ObjectPath);
		FArenaGltf G;
		if (Base.IsEmpty() || !G.Load(Base + TEXT(".gltf")))
		{
			return Out;
		}
		const TSharedPtr<FJsonObject> Mesh = G.Item(TEXT("meshes"), 0);
		const TArray<TSharedPtr<FJsonValue>>* Prims = nullptr;
		if (!Mesh.IsValid() || !Mesh->TryGetArrayField(TEXT("primitives"), Prims))
		{
			return Out;
		}
		TMap<TPair<FString, bool>, int32> SlotOf;
		TArray<float> Pos, Nrm, UV, UV1, Col;
		TArray<uint32> Idx;
		for (const TSharedPtr<FJsonValue>& PV : *Prims)
		{
			const TSharedPtr<FJsonObject> P = PV->AsObject();
			const TSharedPtr<FJsonObject>* Attrs = nullptr;
			int32 IndicesAcc = -1, PosAcc = -1;
			if (!P->TryGetObjectField(TEXT("attributes"), Attrs) || !(*Attrs)->TryGetNumberField(TEXT("POSITION"), PosAcc) ||
				!P->TryGetNumberField(TEXT("indices"), IndicesAcc))
			{
				continue;
			}
			int32 MatIndex = -1;
			const FString Material = P->TryGetNumberField(TEXT("material"), MatIndex) ? G.MaterialName(MatIndex) : FString(TEXT("None"));
			const TSharedPtr<FJsonObject>* Extras = nullptr;
			bool bCollides = true;
			int32 Section = 0;
			bool bModel = false;
			if (P->TryGetObjectField(TEXT("extras"), Extras))
			{
				(*Extras)->TryGetBoolField(TEXT("enable_collision"), bCollides);
				(*Extras)->TryGetNumberField(TEXT("section"), Section);
				(*Extras)->TryGetBoolField(TEXT("collision_model"), bModel);
			}
			if (bModel)
			{
				// collision only: kept apart from the drawn slots
				int32 PC = 0;
				if (G.ReadFloats(PosAcc, Pos, &PC) && PC == 3 && G.ReadUInts(IndicesAcc, Idx))
				{
					FMeshSection& C = Out->CollisionModel;
					C.Material = TEXT("None");
					const int32 NumV = Pos.Num() / 3;
					for (int32 v = 0; v < NumV; ++v)
					{
						const FVector3f P3(Pos[v * 3], Pos[v * 3 + 1], Pos[v * 3 + 2]);
						C.Positions.Add(IsFinite(P3) ? P3 : FVector3f::ZeroVector);
					}
					C.Normals.SetNumZeroed(NumV);
					C.UV0.SetNumZeroed(NumV);
					for (uint32 I : Idx)
					{
						C.Indices.Add(I < uint32(NumV) ? I : 0);
					}
					(*Extras)->TryGetBoolField(TEXT("box"), Out->bModelBox);
					(*Extras)->TryGetBoolField(TEXT("line"), Out->bModelLine);
				}
				continue;
			}
			const TPair<FString, bool> Key(Material, bCollides);
			int32* Found = SlotOf.Find(Key);
			if (!Found)
			{
				Found = &SlotOf.Add(Key, Out->Slots.Num());
				FMeshSection& S = Out->Slots.AddDefaulted_GetRef();
				S.Material = Material;
				S.bCollides = bCollides;
			}
			Out->SectionSlot.FindOrAdd(Section, *Found);
			FMeshSection& S = Out->Slots[*Found];

			int32 NAcc = -1, TAcc = -1, T1Acc = -1, CAcc = -1;
			(*Attrs)->TryGetNumberField(TEXT("NORMAL"), NAcc);
			(*Attrs)->TryGetNumberField(TEXT("TEXCOORD_0"), TAcc);
			(*Attrs)->TryGetNumberField(TEXT("TEXCOORD_1"), T1Acc);
			(*Attrs)->TryGetNumberField(TEXT("COLOR_0"), CAcc);
			int32 PC = 0, NC = 0, TC = 0, T1C = 0, CC = 0;
			if (!G.ReadFloats(PosAcc, Pos, &PC) || PC != 3 || !G.ReadUInts(IndicesAcc, Idx))
			{
				continue;
			}
			const bool bN = NAcc >= 0 && G.ReadFloats(NAcc, Nrm, &NC) && NC == 3 && Nrm.Num() == Pos.Num();
			const bool bT = TAcc >= 0 && G.ReadFloats(TAcc, UV, &TC) && TC == 2 && UV.Num() / 2 == Pos.Num() / 3;
			const bool bT1 = T1Acc >= 0 && G.ReadFloats(T1Acc, UV1, &T1C) && T1C == 2 && UV1.Num() / 2 == Pos.Num() / 3;
			const bool bC = CAcc >= 0 && G.ReadFloats(CAcc, Col, &CC) && (CC == 3 || CC == 4) && Col.Num() / CC == Pos.Num() / 3;
			const int32 NumV = Pos.Num() / 3;
			const uint32 First = uint32(S.Positions.Num());
			for (int32 v = 0; v < NumV; ++v)
			{
				FVector3f P3(Pos[v * 3], Pos[v * 3 + 1], Pos[v * 3 + 2]);
				S.Positions.Add(IsFinite(P3) ? P3 : FVector3f::ZeroVector);
				FVector3f N3 = bN ? FVector3f(Nrm[v * 3], Nrm[v * 3 + 1], Nrm[v * 3 + 2]) : FVector3f::ZeroVector;
				S.Normals.Add(IsFinite(N3) ? N3 : FVector3f::ZeroVector);
				FVector2f T2 = bT ? FVector2f(UV[v * 2], UV[v * 2 + 1]) : FVector2f::ZeroVector;
				S.UV0.Add(FMath::IsFinite(T2.X) && FMath::IsFinite(T2.Y) ? T2 : FVector2f::ZeroVector);
				if (bT1)
				{
					S.UV1.Add(FVector2f(UV1[v * 2], UV1[v * 2 + 1]));
				}
				if (bC)
				{
					const float A = CC == 4 ? Col[v * 4 + 3] : 1.f;
					S.Colors.Add(FLinearColor(Col[v * CC], Col[v * CC + 1], Col[v * CC + 2], A).ToFColor(true));
				}
			}
			// keep optional streams consistent across merged primitives
			if (S.UV1.Num() && S.UV1.Num() != S.Positions.Num()) S.UV1.SetNumZeroed(S.Positions.Num());
			if (S.Colors.Num() && S.Colors.Num() != S.Positions.Num()) S.Colors.SetNum(S.Positions.Num());
			for (uint32 I : Idx)
			{
				S.Indices.Add(I < uint32(NumV) ? First + I : First);
			}
		}
		Out->bValid = Out->Slots.Num() > 0;
		return Out;
	}

	FMeshDataPtr GetMeshData(const FString& ObjectPath)
	{
		const FString Key = ObjectPath.ToLower();
		{
			FScopeLock Lock(&DataLock);
			if (FMeshDataPtr* Found = DataCache.Find(Key))
			{
				return *Found;
			}
		}
		FMeshDataPtr D = Parse(ObjectPath);
		FScopeLock Lock(&DataLock);
		return DataCache.FindOrAdd(Key, D);
	}

	/** Per-vertex tangents from UV derivatives, orthogonalised against the normal (fallback: any perpendicular). */
	void ComputeTangents(const FMeshSection& S, TArray<FVector3f>& OutTangents, TArray<FVector3f>& OutNormals)
	{
		const int32 NumV = S.Positions.Num();
		OutTangents.Init(FVector3f::ZeroVector, NumV);
		OutNormals = S.Normals;
		OutNormals.SetNumZeroed(NumV);
		TArray<FVector3f> FaceN;
		const bool bNeedNormals = OutNormals.ContainsByPredicate([](const FVector3f& N) { return N.IsNearlyZero(); });
		if (bNeedNormals)
		{
			FaceN.Init(FVector3f::ZeroVector, NumV);
		}
		for (int32 t = 0; t + 2 < S.Indices.Num(); t += 3)
		{
			const uint32 I0 = S.Indices[t], I1 = S.Indices[t + 1], I2 = S.Indices[t + 2];
			const FVector3f E1 = S.Positions[I1] - S.Positions[I0], E2 = S.Positions[I2] - S.Positions[I0];
			const FVector2f D1 = S.UV0[I1] - S.UV0[I0], D2 = S.UV0[I2] - S.UV0[I0];
			const float Det = D1.X * D2.Y - D2.X * D1.Y;
			if (FMath::Abs(Det) > 1e-12f)
			{
				const FVector3f T = (E1 * D2.Y - E2 * D1.Y) / Det;
				OutTangents[I0] += T; OutTangents[I1] += T; OutTangents[I2] += T;
			}
			if (bNeedNormals)
			{
				// Unreal winding: the face normal is the opposite of cross(e1, e2)
				const FVector3f N = FVector3f::CrossProduct(E2, E1);
				FaceN[I0] += N; FaceN[I1] += N; FaceN[I2] += N;
			}
		}
		for (int32 v = 0; v < NumV; ++v)
		{
			FVector3f N = OutNormals[v];
			if (N.IsNearlyZero())
			{
				N = bNeedNormals ? FaceN[v] : FVector3f::UpVector;
			}
			N = N.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
			OutNormals[v] = N;
			FVector3f T = OutTangents[v] - N * FVector3f::DotProduct(N, OutTangents[v]);
			if (T.IsNearlyZero())
			{
				T = FVector3f::CrossProduct(N, FMath::Abs(N.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector);
			}
			OutTangents[v] = T.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::ForwardVector);
		}
	}

	struct FSlotKey
	{
		FString Material;
		EMaterialVariant Variant;
		bool bCollides;
		bool operator==(const FSlotKey& O) const { return bCollides == O.bCollides && Variant == O.Variant && Material.Equals(O.Material, ESearchCase::IgnoreCase); }
		friend uint32 GetTypeHash(const FSlotKey& K) { return HashCombine(GetTypeHash(K.Material.ToLower()), uint32(K.Variant) * 2 + K.bCollides); }
	};

	/** Sections must already be one per slot (same key merged). */
	UStaticMesh* Build(const FString& Name, const TArray<const FMeshSection*>& Slots)
	{
		FScopeUs Timer(Stats().MeshUs);
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VTan = Attr.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VSign = Attr.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> PGNames = Attr.GetPolygonGroupMaterialSlotNames();

		int32 NumV = 0, NumT = 0;
		bool bUV1 = false;
		for (const FMeshSection* S : Slots)
		{
			NumV += S->Positions.Num();
			NumT += S->Indices.Num() / 3;
			bUV1 = bUV1 || S->UV1.Num() > 0;
		}
		if (NumT == 0)
		{
			return nullptr;
		}
		VUV.SetNumChannels(bUV1 ? 2 : 1);
		MD.ReserveNewVertices(NumV);
		MD.ReserveNewVertexInstances(NumV);
		MD.ReserveNewTriangles(NumT);
		MD.ReserveNewPolygons(NumT);
		MD.ReserveNewEdges(NumT * 2);

		UObject* Outer = AssetOuter();
		UStaticMesh* SM = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(*Name)), RF_Transient);
		SM->bAllowCPUAccess = true; // runtime collision cooking reads the render data
		TArray<bool> SlotCollides;
		TArray<FVector3f> Tangents, Normals;
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			const FMeshSection& S = *Slots[SlotIndex];
			const FName SlotName(*FString::Printf(TEXT("S%d"), SlotIndex));
			const FPolygonGroupID PG = MD.CreatePolygonGroup();
			PGNames[PG] = SlotName;
			SM->GetStaticMaterials().Add(FStaticMaterial(ArenaRuntimeContent::LoadMaterialByObjectPath(S.Material, S.Variant), SlotName));
			SlotCollides.Add(S.bCollides);

			ComputeTangents(S, Tangents, Normals);
			TArray<FVertexInstanceID> Inst;
			Inst.SetNumUninitialized(S.Positions.Num());
			for (int32 v = 0; v < S.Positions.Num(); ++v)
			{
				const FVertexID V = MD.CreateVertex();
				VPos[V] = S.Positions[v];
				const FVertexInstanceID I = MD.CreateVertexInstance(V);
				VNrm[I] = Normals[v];
				VTan[I] = Tangents[v];
				VSign[I] = 1.f;
				VUV.Set(I, 0, S.UV0.IsValidIndex(v) ? S.UV0[v] : FVector2f::ZeroVector);
				if (bUV1)
				{
					VUV.Set(I, 1, S.UV1.IsValidIndex(v) ? S.UV1[v] : FVector2f::ZeroVector);
				}
				VCol[I] = S.Colors.IsValidIndex(v) ? FVector4f(FLinearColor(S.Colors[v])) : FVector4f(1.f, 1.f, 1.f, 1.f);
				Inst[v] = I;
			}
			for (int32 t = 0; t + 2 < S.Indices.Num(); t += 3)
			{
				const uint32 A = S.Indices[t], B = S.Indices[t + 1], C = S.Indices[t + 2];
				if (A == B || B == C || A == C || A >= uint32(Inst.Num()) || B >= uint32(Inst.Num()) || C >= uint32(Inst.Num()))
				{
					continue;
				}
				if (S.Positions[A] == S.Positions[B] || S.Positions[B] == S.Positions[C] || S.Positions[A] == S.Positions[C])
				{
					continue; // degenerate (the importer removed these too)
				}
				MD.CreateTriangle(PG, { Inst[A], Inst[B], Inst[C] });
			}
		}

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bUseHashAsGuid = false;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		Params.bAllowCpuAccess = true;
		SM->NeverStream = true;
		if (!BuildStaticMeshHeadlessSafe(SM, MD, Params))
		{
			return nullptr;
		}
		// Texture streaming data is computed by the editor only; cooked builds ensure() on uninitialised entries.
		// Our textures never stream (transient), so the default density is enough.
		for (FStaticMaterial& Material : SM->GetStaticMaterials())
		{
			if (!Material.UVChannelData.bInitialized)
			{
				Material.UVChannelData = FMeshUVChannelInfo(1.f);
			}
		}

		// per-section collision (sections follow the polygon groups that have triangles)
		bool bAnyCollision = false;
		FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			FStaticMeshSection& Section = LOD.Sections[SectionIndex];
			const bool bCol = SlotCollides.IsValidIndex(Section.MaterialIndex) ? SlotCollides[Section.MaterialIndex] : true;
			Section.bEnableCollision = bCol;
			bAnyCollision = bAnyCollision || bCol;
#if WITH_EDITORONLY_DATA
			FMeshSectionInfo Info = SM->GetSectionInfoMap().Get(0, SectionIndex);
			Info.bEnableCollision = bCol;
			SM->GetSectionInfoMap().Set(0, SectionIndex, Info);
#endif
		}
		UBodySetup* Body = SM->GetBodySetup();
		if (Body)
		{
			Body->CollisionTraceFlag = bAnyCollision ? CTF_UseComplexAsSimple : CTF_UseDefault;
			// UE2 collides against solid space, not one-sided polygons (see ImportUT2004.py setup_mesh)
			Body->bDoubleSidedGeometry = true;
			Body->bNeverNeedsCookedCollisionData = !bAnyCollision;
			Body->InvalidatePhysicsData();
		}
		++Stats().Meshes;
		return SM;
	}
}

UStaticMesh* ArenaRuntimePrivate::BuildCacheStaticMesh(const FString& ObjectPath)
{
	using namespace ArenaRuntimeMeshes;
	FMeshDataPtr D;
	{
		FScopeUs Timer(Stats().MeshUs);
		D = GetMeshData(ObjectPath);
	}
	if (!D->bValid)
	{
		return nullptr;
	}
	TArray<const FMeshSection*> Slots;
	for (const FMeshSection& S : D->Slots)
	{
		Slots.Add(&S);
	}
	FString Pkg, Rest;
	if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
	{
		Rest = ObjectPath;
	}
	UStaticMesh* SM = Build(AssetName(Rest), Slots);
	// the cached geometry is not needed once the mesh exists (slot info stays)
	{
		FScopeLock Lock(&DataLock);
		if (FMeshDataPtr* Found = DataCache.Find(ObjectPath.ToLower()))
		{
			FMeshDataPtr Slim = MakeShared<FMeshData, ESPMode::ThreadSafe>();
			Slim->bValid = (*Found)->bValid;
			Slim->SectionSlot = (*Found)->SectionSlot;
			// the collision model mesh is built on its own request (LoadStaticMeshCollisionModel)
			Slim->CollisionModel = MoveTemp((*Found)->CollisionModel);
			Slim->bModelBox = (*Found)->bModelBox;
			Slim->bModelLine = (*Found)->bModelLine;
			for (const FMeshSection& S : (*Found)->Slots)
			{
				FMeshSection& O = Slim->Slots.AddDefaulted_GetRef();
				O.Material = S.Material;
				O.bCollides = S.bCollides;
			}
			*Found = Slim;
		}
	}
	return SM;
}

UStaticMesh* ArenaRuntimePrivate::BuildCacheCollisionModel(const FString& ObjectPath)
{
	using namespace ArenaRuntimeMeshes;
	FMeshDataPtr D = GetMeshData(ObjectPath);
	if (!D->bValid || D->CollisionModel.Indices.Num() < 3 || !(D->bModelBox || D->bModelLine))
	{
		return nullptr;
	}
	FString Pkg, Rest;
	if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
	{
		Rest = ObjectPath;
	}
	return Build(AssetName(Rest + TEXT("_UE2Collision")), { &D->CollisionModel });
}

bool ArenaRuntimePrivate::CollisionModelFlags(const FString& ObjectPath, bool& bOutBox, bool& bOutLine)
{
	using namespace ArenaRuntimeMeshes;
	FMeshDataPtr D = GetMeshData(ObjectPath);
	bOutBox = D->bModelBox;
	bOutLine = D->bModelLine;
	return D->CollisionModel.Indices.Num() >= 3;
}

int32 ArenaRuntimePrivate::SlotForSection(const FString& ObjectPath, int32 Section)
{
	using namespace ArenaRuntimeMeshes;
	FMeshDataPtr D = GetMeshData(ObjectPath);
	const int32* Slot = D->SectionSlot.Find(Section);
	return Slot ? *Slot : -1;
}

void ArenaRuntimePrivate::PrefetchMeshData(const TArray<FString>& ObjectPaths)
{
	using namespace ArenaRuntimeMeshes;
	FScopeUs Timer(Stats().MeshUs);
	ParallelFor(ObjectPaths.Num(), [&](int32 i) { GetMeshData(ObjectPaths[i]); });
}

TArray<FString> ArenaRuntimePrivate::MeshMaterials(const TArray<FString>& MeshPaths)
{
	using namespace ArenaRuntimeMeshes;
	TArray<FString> Out;
	for (const FString& P : MeshPaths)
	{
		for (const FMeshSection& S : GetMeshData(P)->Slots)
		{
			Out.AddUnique(S.Material);
		}
	}
	return Out;
}

void ArenaRuntimePrivate::TrimMeshData()
{
	using namespace ArenaRuntimeMeshes;
	FScopeLock Lock(&DataLock);
	DataCache.Reset();
}

UStaticMesh* ArenaRuntimeContent::BuildStaticMesh(const FString& Name, const TArray<FMeshSection>& Sections)
{
	using namespace ArenaRuntimeMeshes;
	// merge sections with the same (material, variant, collides) into one slot
	TMap<FSlotKey, int32> SlotOf;
	TArray<FMeshSection> Merged;
	TArray<const FMeshSection*> Direct;
	bool bNeedMerge = false;
	for (const FMeshSection& S : Sections)
	{
		const FSlotKey K{ S.Material, S.Variant, S.bCollides };
		if (SlotOf.Contains(K))
		{
			bNeedMerge = true;
			break;
		}
		SlotOf.Add(K, SlotOf.Num());
	}
	if (!bNeedMerge)
	{
		for (const FMeshSection& S : Sections)
		{
			Direct.Add(&S);
		}
		return Build(AssetName(Name), Direct);
	}
	SlotOf.Reset();
	for (const FMeshSection& S : Sections)
	{
		const FSlotKey K{ S.Material, S.Variant, S.bCollides };
		int32* Found = SlotOf.Find(K);
		if (!Found)
		{
			Found = &SlotOf.Add(K, Merged.Num());
			FMeshSection& M = Merged.AddDefaulted_GetRef();
			M.Material = S.Material;
			M.Variant = S.Variant;
			M.bCollides = S.bCollides;
		}
		FMeshSection& M = Merged[*Found];
		const uint32 First = uint32(M.Positions.Num());
		const bool bUV1 = M.UV1.Num() > 0 || S.UV1.Num() > 0;
		const bool bCol = M.Colors.Num() > 0 || S.Colors.Num() > 0;
		if (bUV1) M.UV1.SetNumZeroed(M.Positions.Num());
		if (bCol) M.Colors.SetNum(M.Positions.Num());
		M.Positions.Append(S.Positions);
		M.Normals.Append(S.Normals);
		M.Normals.SetNumZeroed(M.Positions.Num());
		M.UV0.Append(S.UV0);
		M.UV0.SetNumZeroed(M.Positions.Num());
		if (bUV1) { M.UV1.Append(S.UV1); M.UV1.SetNumZeroed(M.Positions.Num()); }
		if (bCol) { M.Colors.Append(S.Colors); M.Colors.SetNum(M.Positions.Num()); }
		for (uint32 I : S.Indices)
		{
			M.Indices.Add(First + I);
		}
	}
	for (const FMeshSection& S : Merged)
	{
		Direct.Add(&S);
	}
	return Build(AssetName(Name), Direct);
}

void ArenaRuntimeContent::CookCollision(const TArray<UStaticMesh*>& Meshes)
{
	check(IsInGameThread());
	const double T0 = FPlatformTime::Seconds();
	int32 Pending = 0, Started = 0;
	for (UStaticMesh* SM : Meshes)
	{
		UBodySetup* Body = SM ? SM->GetBodySetup() : nullptr;
		if (!Body || Body->bCreatedPhysicsMeshes || Body->bNeverNeedsCookedCollisionData || Body->CollisionTraceFlag != CTF_UseComplexAsSimple)
		{
			continue;
		}
		++Pending;
		++Started;
		Body->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateLambda([&Pending](bool) { --Pending; }));
	}
	// the completion callbacks run as game thread tasks
	while (Pending > 0)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (Pending > 0)
		{
			FPlatformProcess::SleepNoStats(0.0005f);
		}
	}
	if (Started > 0)
	{
		int32 WithTriMesh = 0;
		for (UStaticMesh* SM : Meshes)
		{
			const UBodySetup* Body = SM ? SM->GetBodySetup() : nullptr;
			WithTriMesh += Body && Body->TriMeshGeometries.Num() > 0 ? 1 : 0;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: cooked collision of %d meshes in %.2fs (%d with a collision mesh)"), Started, FPlatformTime::Seconds() - T0, WithTriMesh);
	}
}

#include "ArenaRuntimeSkeletal.h"
#include "OpenUT2k4.h"
#include "ArenaHitchLog.h"
#include "ArenaContent.h"
#include "ArenaRuntimeContent.h"
#include "Algo/BinarySearch.h"
#include "Algo/StableSort.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#endif

// =================================================================================================
// Animation sequence
// =================================================================================================

void UArenaRuntimeAnimSequence::InitRuntime(USkeleton* InSkeleton, float InLength, float InFrameRate, TArray<TArray<float>>&& InTimes, TArray<FTrack>&& InTracks)
{
	SetSkeleton(InSkeleton);
	Length = InLength;
	FrameRate = InFrameRate > 0.f ? InFrameRate : 30.f;
	Times = MoveTemp(InTimes);
	Tracks = MoveTemp(InTracks);
	bEnableRootMotion = false;
}

namespace ArenaRuntimeSkeletalPrivate
{
	/** Key interval for time T in a sorted key time array: Index and blend Alpha towards Index + 1. */
	void FindKey(const TArray<float>& KeyTimes, float T, int32& OutIndex, float& OutAlpha)
	{
		const int32 Num = KeyTimes.Num();
		if (Num <= 1 || T <= KeyTimes[0])
		{
			OutIndex = 0;
			OutAlpha = 0.f;
			return;
		}
		if (T >= KeyTimes[Num - 1])
		{
			OutIndex = Num - 1;
			OutAlpha = 0.f;
			return;
		}
		const int32 Upper = Algo::UpperBound(KeyTimes, T);
		OutIndex = FMath::Clamp(Upper - 1, 0, Num - 2);
		const float Span = KeyTimes[OutIndex + 1] - KeyTimes[OutIndex];
		OutAlpha = Span > UE_SMALL_NUMBER ? FMath::Clamp((T - KeyTimes[OutIndex]) / Span, 0.f, 1.f) : 0.f;
	}

	template <typename T>
	bool SampleLinear(const TArray<float>* KeyTimes, const TArray<T>& Keys, float Time, T& Out)
	{
		if (Keys.Num() == 0)
		{
			return false;
		}
		if (!KeyTimes || Keys.Num() == 1)
		{
			Out = Keys[0];
			return true;
		}
		int32 I;
		float A;
		FindKey(*KeyTimes, Time, I, A);
		I = FMath::Min(I, Keys.Num() - 1);
		const int32 J = FMath::Min(I + 1, Keys.Num() - 1);
		Out = (A <= 0.f || I == J) ? Keys[I] : FMath::Lerp(Keys[I], Keys[J], A);
		return true;
	}

	bool SampleRotation(const TArray<float>* KeyTimes, const TArray<FQuat4f>& Keys, float Time, FQuat4f& Out)
	{
		if (Keys.Num() == 0)
		{
			return false;
		}
		if (!KeyTimes || Keys.Num() == 1)
		{
			Out = Keys[0];
			return true;
		}
		int32 I;
		float A;
		FindKey(*KeyTimes, Time, I, A);
		I = FMath::Min(I, Keys.Num() - 1);
		const int32 J = FMath::Min(I + 1, Keys.Num() - 1);
		Out = (A <= 0.f || I == J) ? Keys[I] : FQuat4f::FastLerp(Keys[I], Keys[J], A).GetNormalized();
		return true;
	}
}

void UArenaRuntimeAnimSequence::GetAnimationPose(FAnimationPoseData& OutAnimationPoseData, const FAnimExtractContext& ExtractionContext) const
{
	using namespace ArenaRuntimeSkeletalPrivate;
	FCompactPose& Pose = OutAnimationPoseData.GetPose();
	Pose.ResetToRefPose();
	if (Tracks.Num() == 0)
	{
		return;
	}
	float T = float(ExtractionContext.CurrentTime);
	if (ExtractionContext.bLooping && Length > 0.f && (T < 0.f || T > Length))
	{
		T = FMath::Fmod(T, Length);
		if (T < 0.f)
		{
			T += Length;
		}
	}
	T = FMath::Clamp(T, 0.f, Length);

	const FBoneContainer& Bones = Pose.GetBoneContainer();
	const FReferenceSkeleton& RefSkel = Bones.GetReferenceSkeleton();
	for (const FTrack& Track : Tracks)
	{
		const int32 MeshBone = RefSkel.FindBoneIndex(Track.Bone);
		if (MeshBone == INDEX_NONE)
		{
			continue;
		}
		const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBone));
		if (Index.GetInt() == INDEX_NONE)
		{
			continue; // not required at this LOD
		}
		FTransform& Bone = Pose[Index];
		FVector3f P;
		if (SampleLinear(Times.IsValidIndex(Track.PosTimes) ? &Times[Track.PosTimes] : nullptr, Track.Pos, T, P))
		{
			Bone.SetTranslation(FVector(P));
		}
		FQuat4f R;
		if (SampleRotation(Times.IsValidIndex(Track.RotTimes) ? &Times[Track.RotTimes] : nullptr, Track.Rot, T, R))
		{
			Bone.SetRotation(FQuat(R));
		}
		FVector3f S;
		if (SampleLinear(Times.IsValidIndex(Track.ScaleTimes) ? &Times[Track.ScaleTimes] : nullptr, Track.Scale, T, S))
		{
			Bone.SetScale3D(FVector(S));
		}
	}
}

// =================================================================================================
// Cache index and glTF reading
// =================================================================================================

namespace ArenaRuntimeSkeletalPrivate
{
	/** Same as the editor importer's asset_name / asset_folder: anything but [A-Za-z0-9_-] -> '_'. */
	FString AssetName(const FString& In)
	{
		FString Out = In;
		for (TCHAR& C : Out)
		{
			if (!FChar::IsAlnum(C) || C > 127)
			{
				if (C != TEXT('_') && C != TEXT('-'))
				{
					C = TEXT('_');
				}
			}
		}
		return Out;
	}

	/** "Pkg.Group.Name" -> ("Pkg", "Group.Name"). */
	void SplitObjectPath(const FString& Path, FString& OutPkg, FString& OutRest)
	{
		if (!Path.Split(TEXT("."), &OutPkg, &OutRest))
		{
			OutPkg = Path;
			OutRest = Path;
		}
	}

	/** "<Folder>/<Name>" key (lower case) of an object path in the importer's naming. */
	FString RelKey(const FString& ObjectPath, const FString& Suffix = FString())
	{
		FString Pkg, Rest;
		SplitObjectPath(ObjectPath, Pkg, Rest);
		return (AssetName(Pkg) + TEXT("/") + AssetName(Rest) + Suffix).ToLower();
	}

	/** Bone names as the game sees them (Interchange turned spaces into '-'). */
	FName BoneName(const FString& In)
	{
		return FName(*In.Replace(TEXT(" "), TEXT("-")));
	}

	/** Normalised name for socket bone matching (importer's _norm: lower case alphanumerics only). */
	FString NormName(const FString& In)
	{
		FString Out;
		for (TCHAR C : In.ToLower())
		{
			if ((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')))
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s is not valid json"), *Path);
			return nullptr;
		}
		return Root;
	}

	const TArray<TSharedPtr<FJsonValue>>& Arr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
		return (Obj.IsValid() && Obj->TryGetArrayField(Field, Out) && Out) ? *Out : Empty;
	}

	TSharedPtr<FJsonObject> ObjAt(const TArray<TSharedPtr<FJsonValue>>& A, int32 Index)
	{
		return A.IsValidIndex(Index) && A[Index].IsValid() ? A[Index]->AsObject() : nullptr;
	}

	int32 IntField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, int32 Default = INDEX_NONE)
	{
		int32 V = Default;
		return (Obj.IsValid() && Obj->TryGetNumberField(Field, V)) ? V : Default;
	}

	FString StrField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString V;
		return (Obj.IsValid() && Obj->TryGetStringField(Field, V)) ? V : FString();
	}

	/** Numbers of a json array field (missing -> empty). */
	TArray<double> Numbers(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<double> Out;
		for (const TSharedPtr<FJsonValue>& V : Arr(Obj, Field))
		{
			Out.Add(V.IsValid() ? V->AsNumber() : 0.0);
		}
		return Out;
	}

	/** A glTF file of the exporter (one external buffer). Data stays in Unreal space, centimetres. */
	struct FGltf
	{
		TSharedPtr<FJsonObject> Doc;
		TArray<uint8> Bin;
		FString Path;

		bool Load(const FString& InPath)
		{
			Path = InPath;
			Doc = LoadJson(InPath);
			if (!Doc.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Buffer = ObjAt(Arr(Doc, TEXT("buffers")), 0);
			const FString Uri = StrField(Buffer, TEXT("uri"));
			if (Uri.IsEmpty() || !FFileHelper::LoadFileToArray(Bin, *(FPaths::GetPath(InPath) / Uri)))
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: buffer %s missing"), *InPath, *Uri);
				return false;
			}
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>& Nodes() const { return Arr(Doc, TEXT("nodes")); }

		/** Reads any accessor as floats (integers converted, normalized types scaled to 0..1). */
		bool Read(int32 AccessorIndex, TArray<float>& Out, int32& OutComponents) const
		{
			Out.Reset();
			OutComponents = 0;
			const TSharedPtr<FJsonObject> Acc = ObjAt(Arr(Doc, TEXT("accessors")), AccessorIndex);
			if (!Acc.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject> View = ObjAt(Arr(Doc, TEXT("bufferViews")), IntField(Acc, TEXT("bufferView")));
			if (!View.IsValid())
			{
				return false;
			}
			const FString Type = StrField(Acc, TEXT("type"));
			const int32 NumComp = Type == TEXT("SCALAR") ? 1 : Type == TEXT("VEC2") ? 2 : Type == TEXT("VEC3") ? 3 : Type == TEXT("VEC4") ? 4 : Type == TEXT("MAT4") ? 16 : 0;
			const int32 CompType = IntField(Acc, TEXT("componentType"));
			const int32 CompSize = (CompType == 5120 || CompType == 5121) ? 1 : (CompType == 5122 || CompType == 5123) ? 2 : (CompType == 5125 || CompType == 5126) ? 4 : 0;
			const int32 Count = IntField(Acc, TEXT("count"), 0);
			bool bNormalized = false;
			Acc->TryGetBoolField(TEXT("normalized"), bNormalized);
			if (NumComp == 0 || CompSize == 0 || Count <= 0)
			{
				return false;
			}
			const int64 Offset = int64(IntField(View, TEXT("byteOffset"), 0)) + IntField(Acc, TEXT("byteOffset"), 0);
			const int32 ElemSize = NumComp * CompSize;
			const int32 Stride = FMath::Max(IntField(View, TEXT("byteStride"), 0), ElemSize);
			if (Offset < 0 || Offset + int64(Stride) * (Count - 1) + ElemSize > Bin.Num())
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: accessor %d out of range"), *Path, AccessorIndex);
				return false;
			}
			Out.SetNumUninitialized(Count * NumComp);
			const uint8* Base = Bin.GetData() + Offset;
			for (int32 i = 0; i < Count; ++i)
			{
				const uint8* E = Base + int64(i) * Stride;
				for (int32 c = 0; c < NumComp; ++c)
				{
					const uint8* P = E + c * CompSize;
					float V = 0.f;
					switch (CompType)
					{
					case 5126: { float F; FMemory::Memcpy(&F, P, 4); V = F; break; }
					case 5125: { uint32 U; FMemory::Memcpy(&U, P, 4); V = float(U); break; }
					case 5123: { uint16 U; FMemory::Memcpy(&U, P, 2); V = bNormalized ? U / 65535.f : float(U); break; }
					case 5122: { int16 S; FMemory::Memcpy(&S, P, 2); V = bNormalized ? FMath::Max(S / 32767.f, -1.f) : float(S); break; }
					case 5121: V = bNormalized ? *P / 255.f : float(*P); break;
					case 5120: { const int8 S = int8(*P); V = bNormalized ? FMath::Max(S / 127.f, -1.f) : float(S); break; }
					default: break;
					}
					Out[i * NumComp + c] = V;
				}
			}
			OutComponents = NumComp;
			return true;
		}

		/** Integer accessor (indices, joints) read exactly. */
		bool ReadUInts(int32 AccessorIndex, TArray<uint32>& Out) const
		{
			TArray<float> F;
			int32 N = 0;
			if (!Read(AccessorIndex, F, N))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Acc = ObjAt(Arr(Doc, TEXT("accessors")), AccessorIndex);
			if (IntField(Acc, TEXT("componentType")) == 5125)
			{
				// 32-bit indices: re-read without the float round trip
				const TSharedPtr<FJsonObject> View = ObjAt(Arr(Doc, TEXT("bufferViews")), IntField(Acc, TEXT("bufferView")));
				const int64 Offset = int64(IntField(View, TEXT("byteOffset"), 0)) + IntField(Acc, TEXT("byteOffset"), 0);
				const int32 Stride = FMath::Max(IntField(View, TEXT("byteStride"), 0), 4 * N);
				const int32 Count = F.Num() / N;
				Out.SetNumUninitialized(F.Num());
				for (int32 i = 0; i < Count; ++i)
				{
					FMemory::Memcpy(&Out[i * N], Bin.GetData() + Offset + int64(i) * Stride, 4 * N);
				}
				return true;
			}
			Out.SetNumUninitialized(F.Num());
			for (int32 i = 0; i < F.Num(); ++i)
			{
				Out[i] = uint32(F[i]);
			}
			return true;
		}

		static FTransform NodeTransform(const TSharedPtr<FJsonObject>& Node)
		{
			const TArray<double> T = Numbers(Node, TEXT("translation"));
			const TArray<double> R = Numbers(Node, TEXT("rotation"));
			const TArray<double> S = Numbers(Node, TEXT("scale"));
			FTransform X = FTransform::Identity;
			if (T.Num() == 3)
			{
				X.SetTranslation(FVector(T[0], T[1], T[2]));
			}
			if (R.Num() == 4)
			{
				X.SetRotation(FQuat(R[0], R[1], R[2], R[3]).GetNormalized());
			}
			if (S.Num() == 3)
			{
				X.SetScale3D(FVector(S[0], S[1], S[2]));
			}
			return X;
		}
	};

	struct FMeshInfo
	{
		FString Json;
		FString ObjectPath;
		/** Animation set object path from the json (lower case), empty if none. */
		FString Animation;
		bool bVertMesh = false;
	};

	struct FSetInfo
	{
		FString Json;
		FString ObjectPath;
		FString RepresentativeMesh;
		bool bVertMesh = false;
	};

	struct FSeqRef
	{
		int32 Set = INDEX_NONE;
		FString Sequence;
	};

	struct FState
	{
		bool bIndexed = false;
		FString CacheDir;
		TMap<FString, FMeshInfo> MeshByObject;        // lower object path
		TMap<FString, FString> MeshObjectByRel;        // lower "<Folder>/<Name>" -> lower object path
		TArray<FSetInfo> Sets;
		TMap<FString, int32> SetByObject;              // lower object path
		TMap<FString, FSeqRef> SeqByRel;               // lower "<Folder>/<Set>_<Seq>"

		TMap<FString, TStrongObjectPtr<USkeletalMesh>> Meshes;   // lower object path (nullptr = failed)
		TMap<FString, TStrongObjectPtr<USkeleton>> Skeletons;    // group key
		TMap<FString, FString> MeshGroup;                        // lower object path -> group key actually used
		TMap<int32, TMap<FString, TStrongObjectPtr<UArenaRuntimeAnimSequence>>> SetSequences; // set -> lower seq name
		TSet<int32> FailedSets;

		int32 NumMeshes = 0;
		int32 NumSequences = 0;
		double MeshSeconds = 0.0;
		double AnimSeconds = 0.0;
		double IndexSeconds = 0.0;
	};

	FState& State()
	{
		static FState S;
		return S;
	}

	/** Lists the json files in <cache>/<Kind>/<Pkg>/. */
	TArray<FString> CacheJsonFiles(const FString& CacheDir, const TCHAR* Kind)
	{
		TArray<FString> Out;
		const FString Root = CacheDir / Kind;
		TArray<FString> Packages;
		IFileManager::Get().FindFiles(Packages, *(Root / TEXT("*")), false, true);
		Packages.Sort();
		for (const FString& Pkg : Packages)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Root / Pkg / TEXT("*.json")), true, false);
			Files.Sort();
			for (const FString& F : Files)
			{
				Out.Add(Root / Pkg / F);
			}
		}
		return Out;
	}

	void AddSequences(FState& S, int32 SetIndex, const TSharedPtr<FJsonObject>& Meta, const FString& SetObjectPath)
	{
		FString Pkg, Rest;
		SplitObjectPath(SetObjectPath, Pkg, Rest);
		const FString Prefix = AssetName(Pkg) + TEXT("/") + AssetName(Rest) + TEXT("_");
		for (const TSharedPtr<FJsonValue>& V : Arr(Meta, TEXT("sequences")))
		{
			const FString Name = StrField(V.IsValid() ? V->AsObject() : nullptr, TEXT("name"));
			if (!Name.IsEmpty())
			{
				S.SeqByRel.FindOrAdd((Prefix + AssetName(Name)).ToLower(), FSeqRef{ SetIndex, Name.ToLower() });
			}
		}
	}

	/** Reads every skeletal / vertex mesh and animation json of the cache once (paths, names, sequences). */
	FState& Index()
	{
		FState& S = State();
		const FString CacheDir = ArenaContent::GetCacheDir();
		if (S.bIndexed && S.CacheDir == CacheDir)
		{
			return S;
		}
		const double T0 = FPlatformTime::Seconds();
		S = FState();
		S.bIndexed = true;
		S.CacheDir = CacheDir;

		// Vertex meshes first so that a skeletal mesh of the same name wins (the importer did the same).
		for (const TCHAR* Kind : { TEXT("vertmeshes"), TEXT("skeletalmeshes") })
		{
			const bool bVert = FCString::Strcmp(Kind, TEXT("vertmeshes")) == 0;
			for (const FString& Json : CacheJsonFiles(CacheDir, Kind))
			{
				const TSharedPtr<FJsonObject> Meta = LoadJson(Json);
				const FString Type = StrField(Meta, TEXT("type"));
				const FString Path = StrField(Meta, TEXT("path"));
				if (Path.IsEmpty() || StrField(Meta, TEXT("gltf")).IsEmpty() || Type != (bVert ? TEXT("VertMesh") : TEXT("SkeletalMesh")))
				{
					continue;
				}
				FMeshInfo Info;
				Info.Json = Json;
				Info.ObjectPath = Path;
				Info.Animation = StrField(Meta, TEXT("animation")).ToLower();
				Info.bVertMesh = bVert;
				S.MeshByObject.Add(Path.ToLower(), Info);
				S.MeshObjectByRel.Add(RelKey(Path), Path.ToLower());
				if (bVert && Arr(Meta, TEXT("sequences")).Num() > 0)
				{
					FSetInfo Set;
					Set.Json = Json;
					Set.ObjectPath = Path;
					Set.RepresentativeMesh = Path;
					Set.bVertMesh = true;
					const int32 Index = S.Sets.Add(Set);
					S.SetByObject.Add(Path.ToLower(), Index);
					AddSequences(S, Index, Meta, Path);
				}
			}
		}
		for (const FString& Json : CacheJsonFiles(CacheDir, TEXT("animations")))
		{
			const TSharedPtr<FJsonObject> Meta = LoadJson(Json);
			const FString Path = StrField(Meta, TEXT("path"));
			if (Path.IsEmpty() || StrField(Meta, TEXT("type")) != TEXT("MeshAnimation") || StrField(Meta, TEXT("gltf")).IsEmpty()
				|| StrField(Meta, TEXT("representative_mesh")).IsEmpty())
			{
				continue;
			}
			FSetInfo Set;
			Set.Json = Json;
			Set.ObjectPath = Path;
			Set.RepresentativeMesh = StrField(Meta, TEXT("representative_mesh"));
			const int32 Index = S.Sets.Add(Set);
			S.SetByObject.Add(Path.ToLower(), Index);
			AddSequences(S, Index, Meta, Path);
		}
		S.IndexSeconds = FPlatformTime::Seconds() - T0;
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeSkeletal: indexed %d meshes, %d animation sets, %d sequences in %s (%.2fs)"),
			S.MeshByObject.Num(), S.Sets.Num(), S.SeqByRel.Num(), *CacheDir, S.IndexSeconds);
		return S;
	}

	/** Skeleton group of a mesh: its animation set when the cache has it, else the mesh itself. */
	FString GroupKeyFor(const FState& S, const FMeshInfo& Info)
	{
		if (!Info.Animation.IsEmpty() && S.SetByObject.Contains(Info.Animation))
		{
			return TEXT("set:") + Info.Animation;
		}
		return TEXT("mesh:") + Info.ObjectPath.ToLower();
	}

	FString GroupKeyForSet(const FSetInfo& Set)
	{
		return Set.bVertMesh ? TEXT("mesh:") + Set.ObjectPath.ToLower() : TEXT("set:") + Set.ObjectPath.ToLower();
	}

	// ---------------------------------------------------------------------------------------------
	// Mesh building
	// ---------------------------------------------------------------------------------------------

	struct FVertexKey
	{
		FVector3f P;
		bool operator==(const FVertexKey& O) const { return P == O.P; }
		friend uint32 GetTypeHash(const FVertexKey& K) { return HashCombine(HashCombine(GetTypeHash(K.P.X), GetTypeHash(K.P.Y)), GetTypeHash(K.P.Z)); }
	};

	USkeletalMesh* BuildMesh(FState& S, const FMeshInfo& Info, const FString& GroupKey)
	{
		const double T0 = FPlatformTime::Seconds();
		const TSharedPtr<FJsonObject> Meta = LoadJson(Info.Json);
		FGltf G;
		if (!Meta.IsValid() || !G.Load(FPaths::GetPath(Info.Json) / StrField(Meta, TEXT("gltf"))))
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>& Nodes = G.Nodes();
		const TSharedPtr<FJsonObject> Skin = ObjAt(Arr(G.Doc, TEXT("skins")), 0);
		const TSharedPtr<FJsonObject> GMesh = ObjAt(Arr(G.Doc, TEXT("meshes")), 0);
		if (!Skin.IsValid() || !GMesh.IsValid())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s has no skin / mesh"), *G.Path);
			return nullptr;
		}

		// ---- skeleton: skin joints in order, parents from the node hierarchy ----
		TArray<int32> Joints;
		for (const TSharedPtr<FJsonValue>& V : Arr(Skin, TEXT("joints")))
		{
			Joints.Add(int32(V->AsNumber()));
		}
		TMap<int32, int32> NodeParent;
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			for (const TSharedPtr<FJsonValue>& C : Arr(ObjAt(Nodes, n), TEXT("children")))
			{
				NodeParent.Add(int32(C->AsNumber()), n);
			}
		}
		TMap<int32, int32> JointOfNode;
		for (int32 j = 0; j < Joints.Num(); ++j)
		{
			JointOfNode.Add(Joints[j], j);
		}
		const int32 NumBones = Joints.Num();
		if (NumBones == 0 || NumBones >= MAX_uint16)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: unsupported joint count %d"), *G.Path, NumBones);
			return nullptr;
		}

		FString Pkg, Rest;
		SplitObjectPath(Info.ObjectPath, Pkg, Rest);
		const FName ObjName = MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(*AssetName(Rest)));
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage(), ObjName, RF_Transient);
		Mesh->NeverStream = true;
		{
			FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
			for (int32 j = 0; j < NumBones; ++j)
			{
				const TSharedPtr<FJsonObject> Node = ObjAt(Nodes, Joints[j]);
				int32 Parent = INDEX_NONE;
				for (const int32* P = NodeParent.Find(Joints[j]); P; P = NodeParent.Find(*P))
				{
					if (const int32* PJ = JointOfNode.Find(*P))
					{
						Parent = *PJ;
						break;
					}
				}
				if (Parent >= j || (Parent == INDEX_NONE && j != 0))
				{
					UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: joint order not supported (joint %d parent %d)"), *G.Path, j, Parent);
					return nullptr;
				}
				const FName Name = BoneName(StrField(Node, TEXT("name")));
				if (Modifier.FindBoneIndex(Name) != INDEX_NONE)
				{
					UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: duplicate bone %s"), *G.Path, *Name.ToString());
					return nullptr;
				}
				Modifier.Add(FMeshBoneInfo(Name, Name.ToString(), Parent), FGltf::NodeTransform(Node));
			}
		}

		// ---- material slots: distinct section materials in first-use order (importer: S0, S1, ...) ----
		// Sections without a material (None: the skin comes from the character record, xPawn Skins[0] / Skins[1])
		// keep a slot each, so slot = UE2 material index and body and face can be skinned separately.
		const TArray<TSharedPtr<FJsonValue>>& Sections = Arr(Meta, TEXT("sections"));
		TArray<FString> SectionKeys;
		for (const TSharedPtr<FJsonValue>& V : Sections)
		{
			const TSharedPtr<FJsonObject> Sec = V.IsValid() ? V->AsObject() : nullptr;
			const FString M = StrField(Sec, TEXT("material"));
			SectionKeys.Add(M.IsEmpty() || M == TEXT("None") ? FString::Printf(TEXT("None#%d"), IntField(Sec, TEXT("index"))) : M);
		}
		TArray<FString> SlotMaterials;
		for (const FString& Key : SectionKeys)
		{
			SlotMaterials.AddUnique(Key);
		}
		const TArray<TSharedPtr<FJsonValue>>& GMaterials = Arr(G.Doc, TEXT("materials"));

		// ---- geometry ----
		struct FPrim
		{
			int32 Slot = 0;
			int32 BaseVertex = 0;
			int32 NumVertices = 0;
			int32 BaseIndex = 0;
			int32 NumIndices = 0;
		};
		TArray<FPrim> Prims;
		TArray<FVector3f> Positions;
		TArray<FVector2f> UVs;
		TArray<FSkinWeightInfo> Weights;
		TArray<uint32> Indices;
		// Primitives sharing a material become one render section (as the editor import does): emit them in slot order
		// and extend the previous section while the slot stays the same.
		const TArray<TSharedPtr<FJsonValue>>& GPrims = Arr(GMesh, TEXT("primitives"));
		TArray<TPair<int32, int32>> PrimOrder; // (slot, primitive)
		for (int32 p = 0; p < GPrims.Num(); ++p)
		{
			// the converter writes one primitive per section, in section order
			if (GPrims.Num() == SectionKeys.Num())
			{
				PrimOrder.Add({ SlotMaterials.AddUnique(SectionKeys[p]), p });
				continue;
			}
			const TSharedPtr<FJsonObject> P = GPrims[p].IsValid() ? GPrims[p]->AsObject() : nullptr;
			FString MatName = StrField(ObjAt(GMaterials, IntField(P, TEXT("material"))), TEXT("name"));
			PrimOrder.Add({ SlotMaterials.AddUnique(MatName.IsEmpty() ? FString(TEXT("None")) : MatName), p });
		}
		Algo::StableSortBy(PrimOrder, [](const TPair<int32, int32>& O) { return O.Key; });
		for (const TPair<int32, int32>& Order : PrimOrder)
		{
			const TSharedPtr<FJsonObject> P = GPrims[Order.Value].IsValid() ? GPrims[Order.Value]->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> Attr = P.IsValid() ? P->GetObjectField(TEXT("attributes")) : nullptr;
			TArray<float> Pos, UV, Wt;
			TArray<uint32> Jt, Idx;
			int32 NPos = 0, NUV = 0, NWt = 0;
			if (!Attr.IsValid() || !G.Read(IntField(Attr, TEXT("POSITION")), Pos, NPos) || NPos != 3 || !G.ReadUInts(IntField(P, TEXT("indices")), Idx))
			{
				continue;
			}
			const int32 NumV = Pos.Num() / 3;
			if (!G.Read(IntField(Attr, TEXT("TEXCOORD_0")), UV, NUV) || NUV != 2 || UV.Num() / 2 != NumV)
			{
				UV.SetNumZeroed(NumV * 2);
			}
			const bool bSkin = G.ReadUInts(IntField(Attr, TEXT("JOINTS_0")), Jt) && G.Read(IntField(Attr, TEXT("WEIGHTS_0")), Wt, NWt)
				&& NWt == 4 && Jt.Num() == NumV * 4 && Wt.Num() == NumV * 4;
			if (!bSkin)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: primitive without skin weights, bound to the root"), *G.Path);
			}
			FPrim Prim;
			Prim.Slot = Order.Key;
			Prim.BaseVertex = Positions.Num();
			Prim.NumVertices = NumV;
			Prim.BaseIndex = Indices.Num();
			for (int32 v = 0; v < NumV; ++v)
			{
				Positions.Add(FVector3f(Pos[v * 3], Pos[v * 3 + 1], Pos[v * 3 + 2]));
				UVs.Add(FVector2f(UV[v * 2], UV[v * 2 + 1]));
				FSkinWeightInfo W;
				FMemory::Memzero(W);
				if (bSkin)
				{
					// 16-bit weights summing to exactly 65535; leftovers go to the heaviest influence
					int32 Sum = 0, Heaviest = 0;
					for (int32 k = 0; k < 4; ++k)
					{
						const uint32 Bone = Jt[v * 4 + k];
						const float F = FMath::Clamp(Wt[v * 4 + k], 0.f, 1.f);
						W.InfluenceBones[k] = FBoneIndexType(Bone < uint32(NumBones) ? Bone : 0);
						W.InfluenceWeights[k] = uint16(FMath::RoundToInt(F * 65535.f));
						Sum += W.InfluenceWeights[k];
						if (W.InfluenceWeights[k] > W.InfluenceWeights[Heaviest])
						{
							Heaviest = k;
						}
					}
					if (Sum == 0)
					{
						W.InfluenceWeights[0] = 65535;
					}
					else if (Sum != 65535)
					{
						W.InfluenceWeights[Heaviest] = uint16(FMath::Clamp(int32(W.InfluenceWeights[Heaviest]) + 65535 - Sum, 0, 65535));
					}
				}
				else
				{
					W.InfluenceWeights[0] = 65535;
				}
				Weights.Add(W);
			}
			for (int32 i = 0; i + 2 < Idx.Num(); i += 3)
			{
				if (Idx[i] < uint32(NumV) && Idx[i + 1] < uint32(NumV) && Idx[i + 2] < uint32(NumV))
				{
					Indices.Add(Prim.BaseVertex + Idx[i]);
					Indices.Add(Prim.BaseVertex + Idx[i + 1]);
					Indices.Add(Prim.BaseVertex + Idx[i + 2]);
				}
			}
			Prim.NumIndices = Indices.Num() - Prim.BaseIndex;
			if (Prims.Num() > 0 && Prims.Last().Slot == Prim.Slot)
			{
				Prims.Last().NumVertices += Prim.NumVertices;
				Prims.Last().NumIndices += Prim.NumIndices;
			}
			else if (Prim.NumIndices > 0)
			{
				Prims.Add(Prim);
			}
		}
		if (Prims.Num() == 0)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s has no triangles"), *G.Path);
			return nullptr;
		}
		const int32 NumVerts = Positions.Num();

		// ---- normals (smooth, welded by position: the files carry none, the importer recomputed them) and tangents ----
		// Winding is Unreal native: cross(v1 - v0, v2 - v0) points away from the face normal.
		TArray<FVector3f> Normals, TanX, TanY;
		Normals.SetNumZeroed(NumVerts);
		TanX.SetNumZeroed(NumVerts);
		TanY.SetNumZeroed(NumVerts);
		{
			TMap<FVertexKey, int32> Weld;
			TArray<int32> WeldOf;
			WeldOf.SetNumUninitialized(NumVerts);
			for (int32 v = 0; v < NumVerts; ++v)
			{
				WeldOf[v] = Weld.FindOrAdd(FVertexKey{ Positions[v] }, Weld.Num());
			}
			TArray<FVector3f> WeldNormals;
			WeldNormals.SetNumZeroed(Weld.Num());
			for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
			{
				const uint32 A = Indices[i], B = Indices[i + 1], C = Indices[i + 2];
				const FVector3f E1 = Positions[B] - Positions[A];
				const FVector3f E2 = Positions[C] - Positions[A];
				const FVector3f FaceN = FVector3f::CrossProduct(E2, E1); // area weighted
				WeldNormals[WeldOf[A]] += FaceN;
				WeldNormals[WeldOf[B]] += FaceN;
				WeldNormals[WeldOf[C]] += FaceN;
				const FVector2f D1 = UVs[B] - UVs[A];
				const FVector2f D2 = UVs[C] - UVs[A];
				const float Det = D1.X * D2.Y - D2.X * D1.Y;
				if (FMath::Abs(Det) > UE_SMALL_NUMBER)
				{
					const float R = 1.f / Det;
					const FVector3f T = (E1 * D2.Y - E2 * D1.Y) * R;
					const FVector3f Bt = (E2 * D1.X - E1 * D2.X) * R;
					for (uint32 V : { A, B, C })
					{
						TanX[V] += T;
						TanY[V] += Bt;
					}
				}
			}
			for (int32 v = 0; v < NumVerts; ++v)
			{
				FVector3f N = WeldNormals[WeldOf[v]].GetSafeNormal();
				if (N.IsNearlyZero())
				{
					N = FVector3f::UpVector;
				}
				FVector3f X = TanX[v] - N * FVector3f::DotProduct(N, TanX[v]);
				if (!X.Normalize())
				{
					X = FMath::Abs(N.X) < 0.9f ? FVector3f::CrossProduct(N, FVector3f::ForwardVector).GetSafeNormal() : FVector3f::CrossProduct(N, FVector3f::RightVector).GetSafeNormal();
				}
				const FVector3f YDir = FVector3f::CrossProduct(N, X);
				const float Sign = FVector3f::DotProduct(YDir, TanY[v]) < 0.f ? -1.f : 1.f;
				Normals[v] = N;
				TanX[v] = X;
				TanY[v] = YDir * Sign;
			}
		}

		// ---- LOD info and materials ----
		FSkeletalMeshLODInfo& LODInfo = Mesh->AddLODInfo();
		LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.f;
		LODInfo.ReductionSettings.NumOfVertPercentage = 1.f;
		LODInfo.ReductionSettings.MaxDeviationPercentage = 0.f;
		LODInfo.LODHysteresis = 0.02f;
		LODInfo.BuildSettings.bRecomputeNormals = false;
		LODInfo.BuildSettings.bRecomputeTangents = false;
		LODInfo.BuildSettings.bUseFullPrecisionUVs = true;
		TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		for (int32 s = 0; s < SlotMaterials.Num(); ++s)
		{
			UMaterialInterface* Mat = ArenaRuntimeContent::LoadMaterialByObjectPath(SlotMaterials[s].StartsWith(TEXT("None#")) ? FString(TEXT("None")) : SlotMaterials[s]);
			FSkeletalMaterial& SM = Materials.Add_GetRef(FSkeletalMaterial(Mat, FName(*FString::Printf(TEXT("S%d"), s))));
			SM.UVChannelData.bInitialized = true;
		}

		// ---- render data (one LOD, one render section per primitive, every bone in each section's bone map) ----
		Mesh->AllocateResourceForRendering();
		FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		FSkeletalMeshLODRenderData* LOD = new FSkeletalMeshLODRenderData();
		RenderData->LODRenderData.Add(LOD);
		RenderData->NumInlinedLODs = 1;
		RenderData->NumNonOptionalLODs = 1;

		LOD->StaticVertexBuffers.PositionVertexBuffer.Init(NumVerts);
		LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
		LOD->StaticVertexBuffers.StaticMeshVertexBuffer.Init(NumVerts, 1);
		FBox3f Box(ForceInit);
		for (int32 v = 0; v < NumVerts; ++v)
		{
			LOD->StaticVertexBuffers.PositionVertexBuffer.VertexPosition(v) = Positions[v];
			LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(v, TanX[v], TanY[v], Normals[v]);
			LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(v, 0, UVs[v]);
			Box += Positions[v];
		}
		TArray<FBoneIndexType> AllBones;
		AllBones.SetNumUninitialized(NumBones);
		for (int32 b = 0; b < NumBones; ++b)
		{
			AllBones[b] = FBoneIndexType(b);
		}
		LOD->ActiveBoneIndices = AllBones;
		LOD->RequiredBones = AllBones;
		for (const FPrim& Prim : Prims)
		{
			FSkelMeshRenderSection& Section = LOD->RenderSections.AddDefaulted_GetRef();
			Section.MaterialIndex = uint16(Prim.Slot);
			Section.BaseIndex = uint32(Prim.BaseIndex);
			Section.NumTriangles = uint32(Prim.NumIndices / 3);
			Section.BaseVertexIndex = uint32(Prim.BaseVertex);
			Section.NumVertices = uint32(Prim.NumVertices);
			Section.MaxBoneInfluences = 4;
			Section.BoneMap = AllBones;
			Section.DuplicatedVerticesBuffer.Init(Prim.NumVertices, TMap<int32, TArray<int32>>());
		}
		LOD->MultiSizeIndexContainer.RebuildIndexBuffer(NumVerts > MAX_uint16 ? sizeof(uint32) : sizeof(uint16), Indices);
		LOD->SkinWeightVertexBuffer.SetNeedsCPUAccess(true);
		LOD->SkinWeightVertexBuffer.SetMaxBoneInfluences(4);
		LOD->SkinWeightVertexBuffer.SetUse16BitBoneIndex(NumBones > MAX_uint8);
		LOD->SkinWeightVertexBuffer.SetUse16BitBoneWeight(true);
		LOD->SkinWeightVertexBuffer = Weights;

#if WITH_EDITOR
		// The editor checks that the source model sections mirror the render sections.
		if (FSkeletalMeshModel* Imported = Mesh->GetImportedModel())
		{
			Imported->LODModels.Empty();
			FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();
			Imported->LODModels.Add(LODModel);
			for (int32 s = 0; s < Prims.Num(); ++s)
			{
				FSkelMeshSection& Sec = LODModel->Sections.AddDefaulted_GetRef();
				Sec.OriginalDataSectionIndex = s;
				Sec.MaterialIndex = uint16(Prims[s].Slot);
				LODModel->UserSectionsData.Add(s);
			}
		}
#endif

		Mesh->CalculateInvRefMatrices();
		Mesh->SetImportedBounds(FBoxSphereBounds(FBox(Box)));
		Mesh->SetHasVertexColors(false);

		// ---- skeleton (shared per animation set) ----
		USkeleton* Skeleton = nullptr;
		// UE2 meshes can name another skeleton's animation set as theirs (the Paladin's ShockTankMesh names the SPMA
		// cannon's ArtilleryCannonAnims): the set's own mesh (the converter's representative_mesh) owns the set's skeleton
		const int32* SetIndex = Info.Animation.IsEmpty() ? nullptr : S.SetByObject.Find(Info.Animation);
		const bool bSetsOwnMesh = SetIndex && S.Sets.IsValidIndex(*SetIndex) && S.Sets[*SetIndex].RepresentativeMesh.Equals(Info.ObjectPath, ESearchCase::IgnoreCase);
		bool bTakeOver = false;
		if (TStrongObjectPtr<USkeleton>* Shared = S.Skeletons.Find(GroupKey))
		{
			Skeleton = Shared->Get();
			if (Skeleton && !Skeleton->MergeAllBonesToBoneTree(Mesh, false))
			{
				if (bSetsOwnMesh && GroupKey.StartsWith(TEXT("set:")))
				{
					// a mesh that only borrowed the set's name built the skeleton first: it keeps that skeleton as its own
					const TStrongObjectPtr<USkeleton> Borrowed = *Shared;
					for (TPair<FString, FString>& Owner : S.MeshGroup)
					{
						if (Owner.Value == GroupKey)
						{
							Owner.Value = TEXT("mesh:") + Owner.Key;
							S.Skeletons.Add(Owner.Value, Borrowed);
						}
					}
					UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeSkeletal: %s owns the skeleton of its animation set %s (a mesh that only names the set built one first)"),
						*Info.ObjectPath, *Info.Animation);
					bTakeOver = true;
				}
				else
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeSkeletal: %s does not fit the skeleton of its animation set %s (%s): it uses its own"), *Info.ObjectPath,
						*Info.Animation, *Skeleton->GetName());
				}
				Skeleton = nullptr;
			}
		}
		FString UsedGroup = GroupKey;
		if (!Skeleton)
		{
			Skeleton = NewObject<USkeleton>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USkeleton::StaticClass(),
				FName(*(AssetName(Rest) + TEXT("_Skeleton")))), RF_Transient);
			Skeleton->MergeAllBonesToBoneTree(Mesh, false);
			if (bTakeOver)
			{
				S.Skeletons.Add(GroupKey, TStrongObjectPtr<USkeleton>(Skeleton));
			}
			else if (!S.Skeletons.Contains(GroupKey))
			{
				S.Skeletons.Add(GroupKey, TStrongObjectPtr<USkeleton>(Skeleton));
			}
			else
			{
				UsedGroup = TEXT("mesh:") + Info.ObjectPath.ToLower();
				S.Skeletons.Add(UsedGroup, TStrongObjectPtr<USkeleton>(Skeleton));
			}
		}
		Mesh->SetSkeleton(Skeleton);
		S.MeshGroup.Add(Info.ObjectPath.ToLower(), UsedGroup);

		// ---- sockets (mesh-only, as the importer adds them) ----
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		TMap<FString, FName> BoneByNorm;
		for (int32 b = 0; b < Ref.GetNum(); ++b)
		{
			BoneByNorm.FindOrAdd(NormName(Ref.GetBoneName(b).ToString()), Ref.GetBoneName(b));
		}
		for (const TSharedPtr<FJsonValue>& V : Arr(Meta, TEXT("sockets")))
		{
			const TSharedPtr<FJsonObject> So = V.IsValid() ? V->AsObject() : nullptr;
			const FString Name = StrField(So, TEXT("name"));
			const FName* Bone = BoneByNorm.Find(NormName(StrField(So, TEXT("bone"))));
			if (!Bone)
			{
				Bone = BoneByNorm.Find(NormName(StrField(So, TEXT("bone_original"))));
			}
			if (Name.IsEmpty() || !Bone)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: %s: socket %s: bone %s not found"), *Info.ObjectPath, *Name, *StrField(So, TEXT("bone")));
				continue;
			}
			const TArray<double> L = Numbers(So, TEXT("location"));
			const TArray<double> Q = Numbers(So, TEXT("rotation_quat"));
			USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
			Socket->SocketName = FName(*Name);
			Socket->BoneName = *Bone;
			Socket->RelativeLocation = L.Num() == 3 ? FVector(L[0], L[1], L[2]) : FVector::ZeroVector;
			Socket->RelativeRotation = Q.Num() == 4 ? FQuat(Q[0], Q[1], Q[2], Q[3]).GetNormalized().Rotator() : FRotator::ZeroRotator;
			Mesh->GetMeshOnlySocketList().Add(Socket);
		}
		Mesh->RebuildSocketMap();
		Mesh->InitResources();

		++S.NumMeshes;
		const double Dt = FPlatformTime::Seconds() - T0;
		S.MeshSeconds += Dt;
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeSkeletal: mesh %s: %d bones, %d verts, %d tris, %d sections, %d sockets, skeleton %s (%.1f ms)"),
			*Info.ObjectPath, NumBones, NumVerts, Indices.Num() / 3, Prims.Num(), Mesh->GetMeshOnlySocketList().Num(), *Skeleton->GetName(), Dt * 1000.0);
		ArenaHitchLog::Note(FString::Printf(TEXT("skeletal mesh %s (%.0f ms)"), *Info.ObjectPath, Dt * 1000.0));
		return Mesh;
	}

	USkeletalMesh* GetMesh(FState& S, const FString& ObjectPathLower, const FString* ForceGroup = nullptr)
	{
		if (const TStrongObjectPtr<USkeletalMesh>* Found = S.Meshes.Find(ObjectPathLower))
		{
			return Found->Get();
		}
		const FMeshInfo* Info = S.MeshByObject.Find(ObjectPathLower);
		if (!Info)
		{
			return nullptr;
		}
		USkeletalMesh* Mesh = BuildMesh(S, *Info, ForceGroup ? *ForceGroup : GroupKeyFor(S, *Info));
		S.Meshes.Add(ObjectPathLower, TStrongObjectPtr<USkeletalMesh>(Mesh));
		return Mesh;
	}

	// ---------------------------------------------------------------------------------------------
	// Animation building
	// ---------------------------------------------------------------------------------------------

	/** The skeleton of a set: its representative mesh's (built on demand into the set's group). */
	USkeleton* SkeletonForSet(FState& S, const FSetInfo& Set)
	{
		const FString Group = GroupKeyForSet(Set);
		if (const TStrongObjectPtr<USkeleton>* Found = S.Skeletons.Find(Group))
		{
			return Found->Get();
		}
		FString Rep = Set.RepresentativeMesh.ToLower();
		if (!S.MeshByObject.Contains(Rep))
		{
			// Representative mesh missing from the cache: any mesh that names the set.
			Rep.Reset();
			for (const TPair<FString, FMeshInfo>& M : S.MeshByObject)
			{
				if (M.Value.Animation == Set.ObjectPath.ToLower())
				{
					Rep = M.Key;
					break;
				}
			}
		}
		if (Rep.IsEmpty())
		{
			return nullptr;
		}
		if (!S.Meshes.Contains(Rep))
		{
			GetMesh(S, Rep, &Group);
		}
		const TStrongObjectPtr<USkeleton>* Found = S.Skeletons.Find(Group);
		if (!Found)
		{
			// The representative was built earlier into another group: use its skeleton.
			const TStrongObjectPtr<USkeletalMesh>* RepMesh = S.Meshes.Find(Rep);
			return (RepMesh && RepMesh->Get()) ? RepMesh->Get()->GetSkeleton() : nullptr;
		}
		return Found->Get();
	}

	bool BuildSet(FState& S, int32 SetIndex)
	{
		if (S.SetSequences.Contains(SetIndex))
		{
			return true;
		}
		if (S.FailedSets.Contains(SetIndex))
		{
			return false;
		}
		const double T0 = FPlatformTime::Seconds();
		const FSetInfo& Set = S.Sets[SetIndex];
		USkeleton* Skeleton = SkeletonForSet(S, Set);
		const TSharedPtr<FJsonObject> Meta = LoadJson(Set.Json);
		FGltf G;
		if (!Skeleton || !Meta.IsValid() || !G.Load(FPaths::GetPath(Set.Json) / StrField(Meta, TEXT("gltf"))))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeSkeletal: animation set %s unavailable (skeleton %s)"), *Set.ObjectPath, Skeleton ? TEXT("ok") : TEXT("missing"));
			S.FailedSets.Add(SetIndex);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Nodes = G.Nodes();
		const TArray<TSharedPtr<FJsonValue>>& GAnims = Arr(G.Doc, TEXT("animations"));
		const TArray<TSharedPtr<FJsonValue>>& Seqs = Arr(Meta, TEXT("sequences"));
		FString Pkg, Rest;
		SplitObjectPath(Set.ObjectPath, Pkg, Rest);
		const FString SetName = AssetName(Rest);

		TMap<FString, TStrongObjectPtr<UArenaRuntimeAnimSequence>>& Out = S.SetSequences.Add(SetIndex);
		for (int32 a = 0; a < Seqs.Num(); ++a)
		{
			const TSharedPtr<FJsonObject> SeqMeta = Seqs[a].IsValid() ? Seqs[a]->AsObject() : nullptr;
			const FString SeqName = StrField(SeqMeta, TEXT("name"));
			// glTF animation i is sequence i (also matched by name when the counts differ)
			TSharedPtr<FJsonObject> Anim = ObjAt(GAnims, a);
			if (!Anim.IsValid() || StrField(Anim, TEXT("name")) != SeqName)
			{
				Anim.Reset();
				for (const TSharedPtr<FJsonValue>& V : GAnims)
				{
					if (V.IsValid() && StrField(V->AsObject(), TEXT("name")) == SeqName)
					{
						Anim = V->AsObject();
						break;
					}
				}
			}
			if (SeqName.IsEmpty() || !Anim.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>& Samplers = Arr(Anim, TEXT("samplers"));
			TArray<TArray<float>> Times;
			TMap<int32, int32> TimesOfAccessor;
			TMap<int32, int32> TrackOfNode;
			TArray<UArenaRuntimeAnimSequence::FTrack> Tracks;
			float MaxTime = 0.f;
			for (const TSharedPtr<FJsonValue>& CV : Arr(Anim, TEXT("channels")))
			{
				const TSharedPtr<FJsonObject> Ch = CV.IsValid() ? CV->AsObject() : nullptr;
				const TSharedPtr<FJsonObject> Target = Ch.IsValid() ? Ch->GetObjectField(TEXT("target")) : nullptr;
				const TSharedPtr<FJsonObject> Sampler = ObjAt(Samplers, IntField(Ch, TEXT("sampler")));
				const int32 NodeIndex = IntField(Target, TEXT("node"));
				const FString PathName = StrField(Target, TEXT("path"));
				if (!Sampler.IsValid() || !Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				const int32 InputAcc = IntField(Sampler, TEXT("input"));
				int32 TimesIndex = INDEX_NONE;
				if (const int32* Found = TimesOfAccessor.Find(InputAcc))
				{
					TimesIndex = *Found;
				}
				else
				{
					TArray<float> T;
					int32 N = 0;
					if (!G.Read(InputAcc, T, N) || N != 1)
					{
						continue;
					}
					for (int32 k = 1; k < T.Num(); ++k)
					{
						T[k] = FMath::Max(T[k], T[k - 1]);
					}
					if (T.Num() > 0)
					{
						MaxTime = FMath::Max(MaxTime, T.Last());
					}
					TimesIndex = Times.Add(MoveTemp(T));
					TimesOfAccessor.Add(InputAcc, TimesIndex);
				}
				TArray<float> V;
				int32 N = 0;
				if (!G.Read(IntField(Sampler, TEXT("output")), V, N))
				{
					continue;
				}
				int32& TrackIndex = TrackOfNode.FindOrAdd(NodeIndex, INDEX_NONE);
				if (TrackIndex == INDEX_NONE)
				{
					TrackIndex = Tracks.AddDefaulted();
					Tracks[TrackIndex].Bone = BoneName(StrField(ObjAt(Nodes, NodeIndex), TEXT("name")));
				}
				UArenaRuntimeAnimSequence::FTrack& Track = Tracks[TrackIndex];
				const int32 NumKeys = FMath::Min(V.Num() / FMath::Max(N, 1), Times[TimesIndex].Num());
				if (PathName == TEXT("translation") && N == 3)
				{
					Track.PosTimes = TimesIndex;
					for (int32 k = 0; k < NumKeys; ++k)
					{
						Track.Pos.Add(FVector3f(V[k * 3], V[k * 3 + 1], V[k * 3 + 2]));
					}
				}
				else if (PathName == TEXT("rotation") && N == 4)
				{
					Track.RotTimes = TimesIndex;
					for (int32 k = 0; k < NumKeys; ++k)
					{
						FQuat4f Q(V[k * 4], V[k * 4 + 1], V[k * 4 + 2], V[k * 4 + 3]);
						Q.Normalize();
						// keep consecutive keys in the same hemisphere so the lerp takes the short way
						if (Track.Rot.Num() > 0 && (Track.Rot.Last() | Q) < 0.f)
						{
							Q = -Q;
						}
						Track.Rot.Add(Q);
					}
				}
				else if (PathName == TEXT("scale") && N == 3)
				{
					Track.ScaleTimes = TimesIndex;
					for (int32 k = 0; k < NumKeys; ++k)
					{
						Track.Scale.Add(FVector3f(V[k * 3], V[k * 3 + 1], V[k * 3 + 2]));
					}
				}
			}
			double Rate = 30.0;
			if (SeqMeta.IsValid())
			{
				SeqMeta->TryGetNumberField(TEXT("rate"), Rate);
			}
			const float FrameRate = Rate > 0.0 ? float(Rate) : 30.f;
			// Single-frame sequences (poses) still need a positive length to be playable.
			const float Length = MaxTime > UE_KINDA_SMALL_NUMBER ? MaxTime : 1.f / FrameRate;

			const FName ObjName = MakeUniqueObjectName(GetTransientPackage(), UArenaRuntimeAnimSequence::StaticClass(), FName(*(SetName + TEXT("_") + AssetName(SeqName))));
			UArenaRuntimeAnimSequence* Seq = NewObject<UArenaRuntimeAnimSequence>(GetTransientPackage(), ObjName, RF_Transient);
			Seq->InitRuntime(Skeleton, Length, FrameRate, MoveTemp(Times), MoveTemp(Tracks));
			Out.Add(SeqName.ToLower(), TStrongObjectPtr<UArenaRuntimeAnimSequence>(Seq));
			++S.NumSequences;
		}
		const double Dt = FPlatformTime::Seconds() - T0;
		S.AnimSeconds += Dt;
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeSkeletal: animation set %s: %d sequences, skeleton %s (%.1f ms)"),
			*Set.ObjectPath, Out.Num(), *Skeleton->GetName(), Dt * 1000.0);
		ArenaHitchLog::Note(FString::Printf(TEXT("animation set %s (%.0f ms)"), *Set.ObjectPath, Dt * 1000.0));
		return true;
	}

	UAnimSequence* GetSequence(FState& S, int32 SetIndex, const FString& SeqLower)
	{
		if (!S.Sets.IsValidIndex(SetIndex) || !BuildSet(S, SetIndex))
		{
			return nullptr;
		}
		const TStrongObjectPtr<UArenaRuntimeAnimSequence>* Found = S.SetSequences[SetIndex].Find(SeqLower);
		return Found ? Found->Get() : nullptr;
	}

	/** Strips "<Kind>/" from a relative path; false if it does not start with it. */
	bool StripKind(const FString& RelativePath, const TCHAR* Kind, FString& OutRest)
	{
		const FString Prefix = FString(Kind) + TEXT("/");
		if (!RelativePath.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return false;
		}
		OutRest = RelativePath.Mid(Prefix.Len()).ToLower();
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Registration with the runtime content provider
	// ---------------------------------------------------------------------------------------------

	// Registered as soon as UObjects exist (runs right away if the module loads later): the startup map, and the
	// players spawned in it, load before EndOfEngineInit.
	static FDelayedAutoRegisterHelper GRegisterLoaders(EDelayedRegisterRunPhase::ObjectSystemReady, []
	{
		ArenaRuntimeContent::RegisterLoader(TEXT("SkeletalMeshes"), [](UClass* Class, const FString& Rel) { return ArenaRuntimeSkeletal::LoadByRelativePath(Class, Rel); });
		ArenaRuntimeContent::RegisterLoader(TEXT("Animations"), [](UClass* Class, const FString& Rel) { return ArenaRuntimeSkeletal::LoadByRelativePath(Class, Rel); });
	});

	static FDelayedAutoRegisterHelper GCompareOnStart(EDelayedRegisterRunPhase::EndOfEngineInit, []
	{

		// -CompareRuntimeSkeletal=<rel>+<rel>...: development comparison with the cooked imports, run once after start-up.
		FString Compare;
		if (FParse::Value(FCommandLine::Get(), TEXT("CompareRuntimeSkeletal="), Compare, false))
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Compare](float)
			{
				TArray<FString> Paths;
				Compare.ParseIntoArray(Paths, TEXT("+"));
				ArenaRuntimeSkeletal::CompareWithImported(Paths);
				return false;
			}), 1.f);
		}
	});

	static FAutoConsoleCommand GCompareCommand(
		TEXT("arena.CompareRuntimeSkeletal"),
		TEXT("Builds skeletal meshes / animations from the content cache and compares them with the /Game/Imported assets. Args: relative paths (SkeletalMeshes/<Pkg>/<Mesh>, Animations/<Pkg>/<Set>_<Seq>)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { ArenaRuntimeSkeletal::CompareWithImported(Args); }));
}

// =================================================================================================
// Public API
// =================================================================================================

USkeletalMesh* ArenaRuntimeSkeletal::LoadSkeletalMesh(const FString& RelativePath)
{
	using namespace ArenaRuntimeSkeletalPrivate;
	FString Rest;
	if (!StripKind(RelativePath, TEXT("SkeletalMeshes"), Rest))
	{
		return nullptr;
	}
	FState& S = Index();
	const FString* Obj = S.MeshObjectByRel.Find(Rest);
	return Obj ? GetMesh(S, *Obj) : nullptr;
}

USkeletalMesh* ArenaRuntimeSkeletal::LoadSkeletalMeshByObjectPath(const FString& ObjectPath)
{
	using namespace ArenaRuntimeSkeletalPrivate;
	return GetMesh(Index(), ObjectPath.ToLower());
}

USkeleton* ArenaRuntimeSkeletal::LoadSkeleton(const FString& RelativePath)
{
	FString MeshPath = RelativePath;
	MeshPath.RemoveFromEnd(TEXT("_Skeleton"), ESearchCase::IgnoreCase);
	USkeletalMesh* Mesh = LoadSkeletalMesh(MeshPath);
	return Mesh ? Mesh->GetSkeleton() : nullptr;
}

UAnimSequence* ArenaRuntimeSkeletal::LoadAnimation(const FString& RelativePath)
{
	using namespace ArenaRuntimeSkeletalPrivate;
	FString Rest;
	if (!StripKind(RelativePath, TEXT("Animations"), Rest))
	{
		return nullptr;
	}
	FState& S = Index();
	const FSeqRef* Ref = S.SeqByRel.Find(Rest);
	return Ref ? GetSequence(S, Ref->Set, Ref->Sequence) : nullptr;
}

UAnimSequence* ArenaRuntimeSkeletal::LoadAnimationByObjectPath(const FString& SetObjectPath, const FString& Sequence)
{
	using namespace ArenaRuntimeSkeletalPrivate;
	FState& S = Index();
	const int32* Set = S.SetByObject.Find(SetObjectPath.ToLower());
	return Set ? GetSequence(S, *Set, Sequence.ToLower()) : nullptr;
}

UObject* ArenaRuntimeSkeletal::LoadByRelativePath(UClass* Class, const FString& RelativePath)
{
	UObject* Result = nullptr;
	if (RelativePath.StartsWith(TEXT("Animations/"), ESearchCase::IgnoreCase))
	{
		Result = LoadAnimation(RelativePath);
	}
	else if (Class && Class->IsChildOf(USkeleton::StaticClass()))
	{
		Result = LoadSkeleton(RelativePath);
	}
	else
	{
		Result = LoadSkeletalMesh(RelativePath);
		if (!Result && RelativePath.EndsWith(TEXT("_Skeleton"), ESearchCase::IgnoreCase))
		{
			Result = LoadSkeleton(RelativePath);
		}
	}
	return (Result && (!Class || Result->IsA(Class))) ? Result : nullptr;
}

FString ArenaRuntimeSkeletal::GetStats()
{
	using namespace ArenaRuntimeSkeletalPrivate;
	const FState& S = State();
	return FString::Printf(TEXT("skeletal: %d meshes (%.2fs), %d sequences (%.2fs), %d skeletons, index %.2fs"),
		S.NumMeshes, S.MeshSeconds, S.NumSequences, S.AnimSeconds, S.Skeletons.Num(), S.IndexSeconds);
}

// =================================================================================================
// Development comparison with the cooked imports
// =================================================================================================

namespace ArenaRuntimeSkeletalPrivate
{
	/** Local bone transforms of a sequence at time T on a mesh, by bone name. */
	TMap<FName, FTransform> SamplePose(const UAnimSequence* Seq, USkeletalMesh* Mesh, float T)
	{
		TMap<FName, FTransform> Out;
		FMemMark Mark(FMemStack::Get()); // pose evaluation allocates from the mem stack
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		TArray<FBoneIndexType> Required;
		for (int32 b = 0; b < Ref.GetNum(); ++b)
		{
			Required.Add(FBoneIndexType(b));
		}
		FBoneContainer Bones(Required, UE::Anim::FCurveFilterSettings(), *Mesh);
		FCompactPose Pose;
		Pose.SetBoneContainer(&Bones);
		FBlendedCurve Curve;
		Curve.InitFrom(Bones);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData Data(Pose, Curve, Attributes);
		Seq->GetAnimationPose(Data, FAnimExtractContext(double(T), false, FDeltaTimeRecord(), false));
		for (FCompactPoseBoneIndex B : Pose.ForEachBoneIndex())
		{
			Out.Add(Ref.GetBoneName(Bones.MakeMeshPoseIndex(B).GetInt()), Pose[B]);
		}
		return Out;
	}

	float RotDiffDeg(const FQuat& A, const FQuat& B)
	{
		return FMath::RadiansToDegrees(A.AngularDistance(B));
	}

	void CompareMesh(const FString& Rel)
	{
		const FString Name = FPaths::GetBaseFilename(Rel);
		USkeletalMesh* Cooked = LoadObject<USkeletalMesh>(nullptr, *(TEXT("/Game/Imported/") + Rel + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		USkeletalMesh* Runtime = ArenaRuntimeSkeletal::LoadSkeletalMesh(Rel);
		if (!Cooked || !Runtime)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("SkelCompare %s: cooked %s runtime %s"), *Rel, Cooked ? TEXT("ok") : TEXT("missing"), Runtime ? TEXT("ok") : TEXT("missing"));
			return;
		}
		const FReferenceSkeleton& RC = Cooked->GetRefSkeleton();
		const FReferenceSkeleton& RR = Runtime->GetRefSkeleton();
		int32 Missing = 0, ParentDiff = 0;
		double MaxPos = 0.0, MaxRot = 0.0;
		FString Worst;
		const TArray<FTransform> CookedCS = [&]{ TArray<FTransform> A; FAnimationRuntime::FillUpComponentSpaceTransforms(RC, RC.GetRefBonePose(), A); return A; }();
		TArray<FTransform> RuntimeCS;
		FAnimationRuntime::FillUpComponentSpaceTransforms(RR, RR.GetRefBonePose(), RuntimeCS);
		for (int32 b = 0; b < RC.GetNum(); ++b)
		{
			const FName Bone = RC.GetBoneName(b);
			const int32 r = RR.FindBoneIndex(Bone);
			if (r == INDEX_NONE)
			{
				++Missing;
				if (Missing <= 3)
				{
					Worst += FString::Printf(TEXT(" missing:%s"), *Bone.ToString());
				}
				continue;
			}
			const int32 PC = RC.GetParentIndex(b), PR = RR.GetParentIndex(r);
			if ((PC == INDEX_NONE) != (PR == INDEX_NONE) || (PC != INDEX_NONE && RC.GetBoneName(PC) != RR.GetBoneName(PR)))
			{
				++ParentDiff;
			}
			const double DP = FVector::Dist(CookedCS[b].GetLocation(), RuntimeCS[r].GetLocation());
			const double DR = RotDiffDeg(CookedCS[b].GetRotation(), RuntimeCS[r].GetRotation());
			if (DP > MaxPos || DR > MaxRot)
			{
				MaxPos = FMath::Max(MaxPos, DP);
				MaxRot = FMath::Max(MaxRot, DR);
			}
		}
		// Sockets: component-space reference transforms
		int32 SocketMissing = 0;
		double SocketPos = 0.0, SocketRot = 0.0;
		for (int32 i = 0; i < Cooked->NumSockets(); ++i)
		{
			const USkeletalMeshSocket* SC = Cooked->GetSocketByIndex(i);
			const USkeletalMeshSocket* SR = SC ? Runtime->FindSocket(SC->SocketName) : nullptr;
			const int32 BC = SC ? RC.FindBoneIndex(SC->BoneName) : INDEX_NONE;
			const int32 BR = SR ? RR.FindBoneIndex(SR->BoneName) : INDEX_NONE;
			if (!SR || BC == INDEX_NONE || BR == INDEX_NONE)
			{
				++SocketMissing;
				continue;
			}
			const FTransform TC = FTransform(SC->RelativeRotation, SC->RelativeLocation, SC->RelativeScale) * CookedCS[BC];
			const FTransform TR = FTransform(SR->RelativeRotation, SR->RelativeLocation, SR->RelativeScale) * RuntimeCS[BR];
			SocketPos = FMath::Max(SocketPos, FVector::Dist(TC.GetLocation(), TR.GetLocation()));
			SocketRot = FMath::Max(SocketRot, RotDiffDeg(TC.GetRotation(), TR.GetRotation()));
		}
		const FSkeletalMeshRenderData* DC = Cooked->GetResourceForRendering();
		const FSkeletalMeshRenderData* DR = Runtime->GetResourceForRendering();
		const int32 VC = DC && DC->LODRenderData.Num() ? DC->LODRenderData[0].GetNumVertices() : -1;
		const int32 VR = DR && DR->LODRenderData.Num() ? DR->LODRenderData[0].GetNumVertices() : -1;
		const int32 TC = DC && DC->LODRenderData.Num() ? DC->LODRenderData[0].GetTotalFaces() : -1;
		const int32 TR = DR && DR->LODRenderData.Num() ? DR->LODRenderData[0].GetTotalFaces() : -1;
		const int32 SecC = DC && DC->LODRenderData.Num() ? DC->LODRenderData[0].RenderSections.Num() : -1;
		const int32 SecR = DR && DR->LODRenderData.Num() ? DR->LODRenderData[0].RenderSections.Num() : -1;
		FString Mats;
		for (int32 m = 0; m < FMath::Max(Cooked->GetMaterials().Num(), Runtime->GetMaterials().Num()); ++m)
		{
			const UMaterialInterface* MC = Cooked->GetMaterials().IsValidIndex(m) ? Cooked->GetMaterials()[m].MaterialInterface.Get() : nullptr;
			const UMaterialInterface* MR = Runtime->GetMaterials().IsValidIndex(m) ? Runtime->GetMaterials()[m].MaterialInterface.Get() : nullptr;
			Mats += FString::Printf(TEXT(" [%d %s|%s]"), m, MC ? *MC->GetName() : TEXT("-"), MR ? *MR->GetName() : TEXT("-"));
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("SkelCompare mesh %s: bones %d/%d missing %d parentdiff %d refpose maxpos %.4f maxrot %.4f%s | sockets %d/%d missing %d maxpos %.4f maxrot %.3f | verts %d/%d tris %d/%d sections %d/%d | bounds %s / %s | mats%s"),
			*Rel, RC.GetNum(), RR.GetNum(), Missing, ParentDiff, MaxPos, MaxRot, *Worst, Cooked->NumSockets(), Runtime->NumSockets(), SocketMissing, SocketPos, SocketRot,
			VC, VR, TC, TR, SecC, SecR, *Cooked->GetImportedBounds().GetBox().ToString(), *Runtime->GetImportedBounds().GetBox().ToString(), *Mats);
	}

	void CompareAnim(const FString& Rel)
	{
		const FString Name = FPaths::GetBaseFilename(Rel);
		UAnimSequence* Cooked = LoadObject<UAnimSequence>(nullptr, *(TEXT("/Game/Imported/") + Rel + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UAnimSequence* Runtime = ArenaRuntimeSkeletal::LoadAnimation(Rel);
		if (!Cooked || !Runtime)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("SkelCompare %s: cooked %s runtime %s"), *Rel, Cooked ? TEXT("ok") : TEXT("missing"), Runtime ? TEXT("ok") : TEXT("missing"));
			return;
		}
		// Evaluate each on a mesh of its own skeleton: the skeleton's preview mesh, else any mesh built for it.
		USkeletalMesh* CookedMesh = Cooked->GetSkeleton() ? Cooked->GetSkeleton()->GetPreviewMesh(true) : nullptr;
		USkeletalMesh* RuntimeMesh = nullptr;
		for (const TPair<FString, TStrongObjectPtr<USkeletalMesh>>& M : State().Meshes)
		{
			if (M.Value.IsValid() && M.Value->GetSkeleton() == Runtime->GetSkeleton())
			{
				RuntimeMesh = M.Value.Get();
				if (!CookedMesh)
				{
					FString Pkg, Rest;
					SplitObjectPath(M.Key, Pkg, Rest);
					CookedMesh = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("/Game/Imported/SkeletalMeshes/%s/%s.%s"), *AssetName(Pkg), *AssetName(Rest), *AssetName(Rest)), nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				break;
			}
		}
		if (!CookedMesh || !RuntimeMesh)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("SkelCompare anim %s: no mesh to evaluate on (cooked %d runtime %d)"), *Rel, CookedMesh != nullptr, RuntimeMesh != nullptr);
			return;
		}
		const float LC = Cooked->GetPlayLength(), LR = Runtime->GetPlayLength();
		double MaxPos = 0.0, MaxRot = 0.0;
		FString WorstBone;
		const int32 Steps = 8;
		for (int32 i = 0; i <= Steps; ++i)
		{
			const float T = FMath::Min(LC, LR) * i / Steps;
			const TMap<FName, FTransform> PC = SamplePose(Cooked, CookedMesh, T);
			const TMap<FName, FTransform> PR = SamplePose(Runtime, RuntimeMesh, T);
			for (const TPair<FName, FTransform>& B : PC)
			{
				const FTransform* R = PR.Find(B.Key);
				if (!R)
				{
					continue;
				}
				const double DP = FVector::Dist(B.Value.GetLocation(), R->GetLocation());
				const double DR = RotDiffDeg(B.Value.GetRotation(), R->GetRotation());
				if (DP > MaxPos || DR > MaxRot)
				{
					WorstBone = B.Key.ToString();
				}
				MaxPos = FMath::Max(MaxPos, DP);
				MaxRot = FMath::Max(MaxRot, DR);
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("SkelCompare anim %s: length %.4f/%.4f keys %d/%d rate %s/%s | sampled local maxpos %.4f maxrot %.3f (worst %s) on %s / %s"),
			*Rel, LC, LR, Cooked->GetNumberOfSampledKeys(), Runtime->GetNumberOfSampledKeys(), *Cooked->GetSamplingFrameRate().ToPrettyText().ToString(),
			*Runtime->GetSamplingFrameRate().ToPrettyText().ToString(), MaxPos, MaxRot, *WorstBone, *CookedMesh->GetName(), *RuntimeMesh->GetName());
	}
}

void ArenaRuntimeSkeletal::CompareWithImported(const TArray<FString>& RelativePaths)
{
	using namespace ArenaRuntimeSkeletalPrivate;
	for (const FString& Rel : RelativePaths)
	{
		if (Rel.StartsWith(TEXT("Animations/"), ESearchCase::IgnoreCase))
		{
			CompareAnim(Rel);
		}
		else
		{
			CompareMesh(Rel);
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("SkelCompare done: %s"), *GetStats());

	// -SkelShowcase: also place each mesh twice in front of the player (cooked on the left, runtime on the right),
	// playing the animation listed right after it, for -AutoScreenshot comparisons.
	if (!FParse::Param(FCommandLine::Get(), TEXT("SkelShowcase")))
	{
		return;
	}
	UWorld* World = nullptr;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE)
		{
			World = Ctx.World();
		}
	}
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("SkelShowcase: no player"));
		return;
	}
	FVector ViewLoc;
	FRotator ViewRot;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	ViewRot.Pitch = ViewRot.Roll = 0.f;
	const FVector Fwd = ViewRot.Vector();
	const FVector Right = FRotationMatrix(ViewRot).GetUnitAxis(EAxis::Y);
	int32 Row = 0;
	for (int32 i = 0; i < RelativePaths.Num(); ++i)
	{
		const FString& Rel = RelativePaths[i];
		if (!Rel.StartsWith(TEXT("SkeletalMeshes/"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString AnimRel = RelativePaths.IsValidIndex(i + 1) && RelativePaths[i + 1].StartsWith(TEXT("Animations/"), ESearchCase::IgnoreCase) ? RelativePaths[i + 1] : FString();
		const FString Name = FPaths::GetBaseFilename(Rel);
		USkeletalMesh* Cooked = LoadObject<USkeletalMesh>(nullptr, *(TEXT("/Game/Imported/") + Rel + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		USkeletalMesh* Runtime = LoadSkeletalMesh(Rel);
		UAnimSequence* CookedAnim = AnimRel.IsEmpty() ? nullptr : LoadObject<UAnimSequence>(nullptr, *(TEXT("/Game/Imported/") + AnimRel + TEXT(".") + FPaths::GetBaseFilename(AnimRel)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UAnimSequence* RuntimeAnim = AnimRel.IsEmpty() ? nullptr : LoadAnimation(AnimRel);
		const float Spacing = FMath::Max(Runtime ? float(Runtime->GetImportedBounds().SphereRadius) : 100.f, 60.f) * 1.2f;
		const FVector Base = ViewLoc + Fwd * (250.f + Row * 250.f) - FVector(0.f, 0.f, 60.f);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			USkeletalMesh* M = Side == 0 ? Cooked : Runtime;
			UAnimSequence* A = Side == 0 ? CookedAnim : RuntimeAnim;
			if (!M)
			{
				continue;
			}
			ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(Base + Right * (Side == 0 ? -Spacing : Spacing), FRotator(0.f, ViewRot.Yaw + 180.f, 0.f));
			if (!Actor)
			{
				continue;
			}
			USkeletalMeshComponent* C = Actor->GetSkeletalMeshComponent();
			C->SetSkeletalMeshAsset(M);
			if (A)
			{
				C->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				C->PlayAnimation(A, true);
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("SkelShowcase row %d: %s (%s) cooked %d runtime %d"), Row, *Rel, *AnimRel, Cooked != nullptr, Runtime != nullptr);
		++Row;
	}
}

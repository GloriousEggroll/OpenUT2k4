#include "ArenaRuntimeMap.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaRuntimeContent.h"
#include "ArenaRuntimeContentPrivate.h"
#include "ArenaLevelBuilder.h"
#include "ArenaUE2World.h"
#include "Async/ParallelFor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/SkyAtmosphereComponent.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ArenaZoneFog.h"
#include "ArenaProjectile.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "TimerManager.h"

namespace ArenaRuntimeMapLocal
{
	using ArenaRuntimeContent::EMaterialVariant;
	using ArenaRuntimeContent::FMeshSection;

	// Same constants as Tools/ImportUT2004.py
	constexpr double ChunkSize = 6000.0;     // cm, XY grid used to split the BSP into separate meshes
	constexpr int32 ChunkMinTris = 4000;     // maps below this triangle count keep a single BSP mesh
	constexpr int32 ChunkMergeTris = 200;    // smaller chunks are merged into their largest neighbour
	constexpr float LightIntensityScale = 1.f;
	constexpr float LightFalloffExponent = 2.f;
	constexpr float ExposureBias = 0.f;

	/** Actors with these classes in their chain are gameplay markers handled by BuildImported. */
	const TCHAR* const GameplayClasses[] = {
		TEXT("pickup"), TEXT("xpickupbase"), TEXT("xweaponbase"), TEXT("weaponlocker"), TEXT("xbombspawn"), TEXT("xbombdelivery"),
		TEXT("gameobjective"), TEXT("ctfbase"), TEXT("navigationpoint"), TEXT("projectile"), TEXT("pawn"),
	};

	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	const TArray<TSharedPtr<FJsonValue>>& ArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Field, Arr) ? *Arr : Empty;
	}

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		return Obj.IsValid() && Obj->TryGetObjectField(Field, Out) && Out ? *Out : nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Out;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Field, Out);
		}
		return Out;
	}

	double NumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Default)
	{
		double Out = Default;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(Field, Out);
		}
		return Out;
	}

	bool BoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		bool bOut = false;
		return Obj.IsValid() && Obj->TryGetBoolField(Field, bOut) && bOut;
	}

	FVector VectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& Default)
	{
		FVector Out = Default;
		AArenaLevelBuilder::ReadJsonVector(Obj, Field, Out);
		return Out;
	}

	FRotator RotatorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FRotator Out = FRotator::ZeroRotator;
		AArenaLevelBuilder::ReadJsonRotator(Obj, Field, Out);
		return Out;
	}

	/** Integer array property (UE2 bitmaps are signed int32 words in the json). */
	TArray<uint32> BitmapProperty(const TSharedPtr<FJsonObject>& Props, const TCHAR* Field)
	{
		TArray<uint32> Out;
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Props, Field))
		{
			Out.Add(uint32(int64(V->AsNumber())));
		}
		return Out;
	}

	bool IsGameplayActor(const TSharedPtr<FJsonObject>& Actor)
	{
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Actor, TEXT("class_chain")))
		{
			const FString Class = V->AsString();
			for (const TCHAR* G : GameplayClasses)
			{
				if (Class.Equals(G, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool ActorCollides(const TSharedPtr<FJsonObject>& Actor)
	{
		const TSharedPtr<FJsonObject> Props = ObjectField(Actor, TEXT("properties"));
		bool bValue = true;
		if (Props.IsValid() && ((Props->TryGetBoolField(TEXT("bCollideActors"), bValue) && !bValue) || (Props->TryGetBoolField(TEXT("bBlockActors"), bValue) && !bValue)))
		{
			return false;
		}
		return true;
	}

	/**
	 * UE2 bBlockNonZeroExtentTraces / bBlockZeroExtentTraces (default true): False lets players (extent traces) or
	 * shots and sight (zero extent traces) through, e.g. the charger ring around a jump pad, or a grate players
	 * cannot pass but shots can.
	 */
	bool BlocksTraces(const TSharedPtr<FJsonObject>& Actor, const TCHAR* Flag)
	{
		const TSharedPtr<FJsonObject> Props = ObjectField(Actor, TEXT("properties"));
		bool bValue = true;
		return !(Props.IsValid() && Props->TryGetBoolField(Flag, bValue) && !bValue);
	}

	/** True for a real static mesh reference ("None" / null / empty are none). */
	bool HasStaticMesh(const TSharedPtr<FJsonObject>& Actor, FString& OutPath)
	{
		OutPath = StringField(Actor, TEXT("static_mesh"));
		return !OutPath.IsEmpty() && OutPath != TEXT("None");
	}

	// ---- world-space geometry (BSP / terrain) ----

	/** One primitive of world geometry; Key = material + variant + collision (one material slot). */
	struct FWorldPrim
	{
		TArray<FVector3f> Pos;
		TArray<FVector3f> Nrm;
		TArray<FVector2f> UV;
		TArray<uint32> Idx;
		FString Material;
		EMaterialVariant Variant = EMaterialVariant::None;
		bool bCollides = true;
		/** Seen from a SkyZoneInfo's zone (drawn by the 3D sky box). */
		bool bSky = false;
		/** Invisible but solid UE2 surfaces (invisible walls / floors, the sky): collision, never drawn. */
		bool bCollisionOnly = false;
		/** The level's sky surfaces (FakeBackdrop): solid like walls in UE2, projectiles hit them too. */
		bool bFakeBackdrop = false;
		/** Terrain only: how far the pawn collision copy lowers each vertex, for a capsule (X) and a box (Y) (PawnTerrain). */
		TArray<FVector2f> PawnDrop;
		bool bInverted = false;

		int32 NumTris() const { return Idx.Num() / 3; }
	};

	/** Minimal glTF reader for the exporter's bsp.gltf (tightly packed float / uint accessors, one buffer). */
	struct FGltf
	{
		TSharedPtr<FJsonObject> Doc;
		TArray<uint8> Bin;

		bool Load(const FString& Path)
		{
			Doc = LoadJson(Path);
			if (!Doc.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Buffers = ArrayField(Doc, TEXT("buffers"));
			if (Buffers.Num() < 1)
			{
				return false;
			}
			const FString Uri = StringField(Buffers[0]->AsObject(), TEXT("uri"));
			return FFileHelper::LoadFileToArray(Bin, *(FPaths::GetPath(Path) / Uri));
		}

		/** Accessor data as floats (VEC2/VEC3) or indices. False on a malformed accessor. */
		template <typename T>
		bool Read(int32 Index, int32 Components, TArray<T>& Out) const
		{
			const TArray<TSharedPtr<FJsonValue>>& Accessors = ArrayField(Doc, TEXT("accessors"));
			const TArray<TSharedPtr<FJsonValue>>& Views = ArrayField(Doc, TEXT("bufferViews"));
			if (!Accessors.IsValidIndex(Index))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Acc = Accessors[Index]->AsObject();
			const int32 ViewIndex = int32(NumberField(Acc, TEXT("bufferView"), -1));
			if (!Views.IsValidIndex(ViewIndex))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> View = Views[ViewIndex]->AsObject();
			const int64 Offset = int64(NumberField(View, TEXT("byteOffset"), 0)) + int64(NumberField(Acc, TEXT("byteOffset"), 0));
			const int32 Count = int32(NumberField(Acc, TEXT("count"), 0));
			const int32 Type = int32(NumberField(Acc, TEXT("componentType"), 0));
			const int32 Size = Type == 5126 || Type == 5125 ? 4 : Type == 5123 ? 2 : Type == 5121 ? 1 : 0;
			const int64 Num = int64(Count) * Components;
			if (Size == 0 || Offset < 0 || Offset + Num * Size > Bin.Num())
			{
				return false;
			}
			Out.SetNumUninitialized(std::is_same_v<T, uint32> ? Num : Count);
			const uint8* Src = Bin.GetData() + Offset;
			if constexpr (std::is_same_v<T, uint32>)
			{
				for (int64 i = 0; i < Num; ++i)
				{
					Out[i] = Type == 5125 ? reinterpret_cast<const uint32*>(Src)[i] : Type == 5123 ? reinterpret_cast<const uint16*>(Src)[i] : Src[i];
				}
			}
			else
			{
				if (Type != 5126 || sizeof(T) != Components * sizeof(float))
				{
					return false;
				}
				FMemory::Memcpy(Out.GetData(), Src, Num * sizeof(float));
			}
			return true;
		}
	};

	/** bsp.gltf -> primitives keyed like ImportUT2004.import_bsp: (material, TwoSided, Unlit, !NotSolid). */
	bool ReadBsp(const FString& Path, TArray<FWorldPrim>& OutPrims)
	{
		FGltf Gltf;
		if (!Gltf.Load(Path))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Materials = ArrayField(Gltf.Doc, TEXT("materials"));
		for (const TSharedPtr<FJsonValue>& MeshValue : ArrayField(Gltf.Doc, TEXT("meshes")))
		{
			for (const TSharedPtr<FJsonValue>& PrimValue : ArrayField(MeshValue->AsObject(), TEXT("primitives")))
			{
				const TSharedPtr<FJsonObject> P = PrimValue->AsObject();
				const TSharedPtr<FJsonObject> Attrs = ObjectField(P, TEXT("attributes"));
				FWorldPrim& Prim = OutPrims.AddDefaulted_GetRef();
				const int32 PosIndex = int32(NumberField(Attrs, TEXT("POSITION"), -1));
				const int32 NrmIndex = int32(NumberField(Attrs, TEXT("NORMAL"), -1));
				const int32 UVIndex = int32(NumberField(Attrs, TEXT("TEXCOORD_0"), -1));
				if (!Gltf.Read(PosIndex, 3, Prim.Pos) || !Gltf.Read(int32(NumberField(P, TEXT("indices"), -1)), 1, Prim.Idx))
				{
					OutPrims.Pop();
					continue;
				}
				if (NrmIndex < 0 || !Gltf.Read(NrmIndex, 3, Prim.Nrm))
				{
					Prim.Nrm.Reset();
				}
				if (UVIndex < 0 || !Gltf.Read(UVIndex, 2, Prim.UV))
				{
					Prim.UV.Init(FVector2f::ZeroVector, Prim.Pos.Num());
				}
				const int32 MatIndex = int32(NumberField(P, TEXT("material"), -1));
				Prim.Material = Materials.IsValidIndex(MatIndex) ? StringField(Materials[MatIndex]->AsObject(), TEXT("name")) : FString(TEXT("None"));
				TSet<FString> Flags;
				for (const TSharedPtr<FJsonValue>& F : ArrayField(ObjectField(P, TEXT("extras")), TEXT("poly_flag_names")))
				{
					Flags.Add(F->AsString());
				}
				if (Flags.Contains(TEXT("TwoSided")))
				{
					Prim.Variant |= EMaterialVariant::TwoSided;
				}
				if (Flags.Contains(TEXT("Unlit")))
				{
					Prim.Variant |= EMaterialVariant::Unlit;
				}
				Prim.bCollides = !Flags.Contains(TEXT("NotSolid"));
				Prim.bSky = BoolField(ObjectField(P, TEXT("extras")), TEXT("sky_zone"));
				Prim.bCollisionOnly = BoolField(ObjectField(P, TEXT("extras")), TEXT("collision_only"));
				Prim.bFakeBackdrop = BoolField(ObjectField(P, TEXT("extras")), TEXT("fake_backdrop"));
			}
		}
		return true;
	}

	/** 16-bit (or 8-bit) grayscale heightmap PNG -> samples. */
	bool ReadHeightmap(const FString& Path, int32& OutW, int32& OutH, TArray<uint16>& OutSamples)
	{
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *Path))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Data.GetData(), Data.Num()))
		{
			return false;
		}
		OutW = int32(Wrapper->GetWidth());
		OutH = int32(Wrapper->GetHeight());
		TArray64<uint8> Raw;
		if (Wrapper->GetBitDepth() == 16)
		{
			if (!Wrapper->GetRaw(ERGBFormat::Gray, 16, Raw) || Raw.Num() < int64(OutW) * OutH * 2)
			{
				return false;
			}
			OutSamples.SetNumUninitialized(OutW * OutH);
			FMemory::Memcpy(OutSamples.GetData(), Raw.GetData(), OutSamples.Num() * sizeof(uint16));
		}
		else
		{
			if (!Wrapper->GetRaw(ERGBFormat::Gray, 8, Raw) || Raw.Num() < int64(OutW) * OutH)
			{
				return false;
			}
			// 8 bit heightmaps: same scale as 16 bit (UE2 stores G16; the exporter only writes 8 bit for flat maps)
			OutSamples.SetNumUninitialized(OutW * OutH);
			for (int32 i = 0; i < OutSamples.Num(); ++i)
			{
				OutSamples[i] = uint16(Raw[i]) << 8;
			}
		}
		return OutW > 1 && OutH > 1;
	}

	/**
	 * UE2 TerrainInfo -> one world-space primitive (ImportUT2004.terrain_primitive):
	 * vertex(x, y) = location + ((x - W/2) * sx, (y - H/2) * sy, (h - 32768) * sz / 256), first layer's material.
	 */
	bool TerrainPrimitive(const TSharedPtr<FJsonObject>& Terrain, const TSharedPtr<FJsonObject>& Actor, FWorldPrim& Out)
	{
		const FString File = StringField(Terrain, TEXT("heightmap_file"));
		if (File.IsEmpty())
		{
			return false;
		}
		int32 W = 0, H = 0;
		TArray<uint16> Hm;
		if (!ReadHeightmap(ArenaContent::GetCacheDir() / File, W, H, Hm))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeMap: terrain %s: could not read %s"), *StringField(Terrain, TEXT("actor")), *File);
			return false;
		}
		const TSharedPtr<FJsonObject> Props = ObjectField(Actor, TEXT("properties"));
		const FVector Scale = VectorField(Terrain, TEXT("terrain_scale"), FVector(1.0));
		const FVector Loc = VectorField(Terrain, TEXT("location"), FVector::ZeroVector);
		const bool bInverted = BoolField(Props, TEXT("Inverted"));
		const TArray<uint32> Visible = BitmapProperty(Props, TEXT("QuadVisibilityBitmap"));
		const TArray<uint32> Turn = BitmapProperty(Props, TEXT("EdgeTurnBitmap"));
		auto Bit = [](const TArray<uint32>& Bits, int32 I) -> bool
		{
			const int32 K = I >> 5;
			return K < Bits.Num() && ((Bits[K] >> (I & 31)) & 1u) != 0;
		};
		auto Height = [&](int32 X, int32 Y) -> double
		{
			// UE2's terrain vertices sit one height step above (h - 32768) x scale (checked against the vertices
			// UT2004 saves in the .ut2: every terrain of DM-DesertIsle, DM-Antalus, ONS-Torlan)
			return Loc.Z + (double(Hm[Y * W + X]) - 32767.0) * Scale.Z / 256.0;
		};

		const TArray<TSharedPtr<FJsonValue>>& Layers = ArrayField(Terrain, TEXT("layers"));
		const TSharedPtr<FJsonObject> Layer = Layers.Num() > 0 ? Layers[0]->AsObject() : nullptr;
		Out.Material = Layer.IsValid() ? StringField(Layer, TEXT("texture")) : FString(TEXT("None"));
		double US = NumberField(Layer, TEXT("u_scale"), 1.0), VS = NumberField(Layer, TEXT("v_scale"), 1.0);
		US = US == 0.0 ? 1.0 : US;
		VS = VS == 0.0 ? 1.0 : VS;
		Out.Variant = EMaterialVariant::None;
		Out.bCollides = true;
		Out.bInverted = bInverted;

		Out.Pos.Reserve(W * H);
		Out.Nrm.Reserve(W * H);
		Out.UV.Reserve(W * H);
		const float NSign = bInverted ? -1.f : 1.f;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				Out.Pos.Add(FVector3f(float(Loc.X + (X - W / 2.0) * Scale.X), float(Loc.Y + (Y - H / 2.0) * Scale.Y), float(Height(X, Y))));
				const double DX = Height(FMath::Min(X + 1, W - 1), Y) - Height(FMath::Max(X - 1, 0), Y);
				const double DY = Height(X, FMath::Min(Y + 1, H - 1)) - Height(X, FMath::Max(Y - 1, 0));
				const FVector3f N = FVector3f(float(-DX / (2.0 * Scale.X)), float(-DY / (2.0 * Scale.Y)), 1.f).GetSafeNormal();
				Out.Nrm.Add(N * NSign);
				Out.UV.Add(FVector2f(float(X / US), float(Y / VS)));
			}
		}
		// pawn collision copy: a vertex is lowered by what the pawn shape adds on the slope at the vertex (central
		// differences, at most as on a 50 degree slope), but not by more than on the walkable quads around it, so a
		// steep bank does not pull the floor at its foot down
		{
			constexpr double R = 50.0; // UT2004 pawn CollisionRadius 25 UU
			const double MaxTan = FMath::Tan(FMath::DegreesToRadians(50.0));
			const double WalkTan = FMath::Tan(FMath::DegreesToRadians(45.6)); // UE2 walkable: floor normal Z >= 0.7
			auto ShapeDrop = [&](double GX, double GY)
			{
				GX = FMath::Abs(GX);
				GY = FMath::Abs(GY);
				const double Tan = FMath::Sqrt(GX * GX + GY * GY);
				if (Tan > MaxTan)
				{
					GX *= MaxTan / Tan;
					GY *= MaxTan / Tan;
				}
				const double T = FMath::Min(Tan, MaxTan);
				return FVector2f(float(R * (FMath::Sqrt(1.0 + T * T) - 1.0)), float(R * (GX + GY)));
			};
			TArray<FVector2f> Limit;
			Limit.Init(FVector2f(TNumericLimits<float>::Max()), W * H);
			TArray<bool> bLimited;
			bLimited.Init(false, W * H);
			for (int32 Y = 0; Y < H - 1; ++Y)
			{
				for (int32 X = 0; X < W - 1; ++X)
				{
					const double A = Height(X, Y), B = Height(X + 1, Y), C = Height(X, Y + 1), D = Height(X + 1, Y + 1);
					const double GX = ((B - A) + (D - C)) / (2.0 * Scale.X), GY = ((C - A) + (D - B)) / (2.0 * Scale.Y);
					if (FMath::Sqrt(GX * GX + GY * GY) > WalkTan)
					{
						continue;
					}
					const FVector2f Q = ShapeDrop(GX, GY);
					for (const int32 V : { Y * W + X, Y * W + X + 1, (Y + 1) * W + X, (Y + 1) * W + X + 1 })
					{
						Limit[V] = bLimited[V] ? FVector2f(FMath::Max(Limit[V].X, Q.X), FMath::Max(Limit[V].Y, Q.Y)) : Q;
						bLimited[V] = true;
					}
				}
			}
			Out.PawnDrop.SetNumUninitialized(W * H);
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const double DX = Height(FMath::Min(X + 1, W - 1), Y) - Height(FMath::Max(X - 1, 0), Y);
					const double DY = Height(X, FMath::Min(Y + 1, H - 1)) - Height(X, FMath::Max(Y - 1, 0));
					const int32 V = Y * W + X;
					const FVector2f Drop = ShapeDrop(DX / (2.0 * Scale.X), DY / (2.0 * Scale.Y));
					Out.PawnDrop[V] = FVector2f(FMath::Min(Drop.X, Limit[V].X), FMath::Min(Drop.Y, Limit[V].Y));
				}
			}
		}
		for (int32 Y = 0; Y < H - 1; ++Y)
		{
			for (int32 X = 0; X < W - 1; ++X)
			{
				const int32 Q = Y * W + X;
				if (Visible.Num() > 0 && !Bit(Visible, Q))
				{
					continue;
				}
				const uint32 A = Q, B = Q + 1, C = Q + W, D = Q + W + 1;
				// UE2 splits a quad along (x, y) - (x + 1, y + 1), the other diagonal where EdgeTurnBitmap is set
				// (measured: a UT2004 server's pawn heights on DM-DesertIsle fit it to 0.2 UU, the other split is off
				// by up to 10). cross(b - a, c - a) points up (+Z); Unreal winding wants the opposite
				uint32 Tris[6];
				if (!Bit(Turn, Q))
				{
					const uint32 T[6] = { A, D, B, A, C, D };
					FMemory::Memcpy(Tris, T, sizeof(Tris));
				}
				else
				{
					const uint32 T[6] = { A, C, B, B, C, D };
					FMemory::Memcpy(Tris, T, sizeof(Tris));
				}
				for (int32 t = 0; t < 2; ++t)
				{
					Out.Idx.Add(Tris[t * 3]);
					Out.Idx.Add(bInverted ? Tris[t * 3 + 2] : Tris[t * 3 + 1]);
					Out.Idx.Add(bInverted ? Tris[t * 3 + 1] : Tris[t * 3 + 2]);
				}
			}
		}
		return Out.Idx.Num() > 0;
	}

	/**
	 * UE2 pawns stand on terrain at the height under their centre (a UT2004 server's pawn floats 2.5 UU above it on
	 * any slope), while a capsule or box rests on the slope higher up. Pawns therefore collide with a copy of the
	 * terrain lowered by what their shape adds on the local slope (TerrainPrimitive's PawnDrop): R (sec - 1) for a
	 * capsule (R the pawn radius), R (|dZ/dX| + |dZ/dY|) for an axis-aligned box (the UE2 net pawn).
	 */
	FWorldPrim PawnTerrain(const FWorldPrim& T, bool bBox)
	{
		FWorldPrim P;
		P.Material = T.Material;
		P.bCollides = true;
		P.Pos = T.Pos;
		P.Nrm = T.Nrm;
		P.UV = T.UV;
		P.Idx = T.Idx;
		if (T.bInverted)
		{
			return P; // a ceiling: pawns do not stand on it
		}
		for (int32 i = 0; i < P.Pos.Num() && i < T.PawnDrop.Num(); ++i)
		{
			P.Pos[i].Z -= bBox ? T.PawnDrop[i].Y : T.PawnDrop[i].X;
		}
		return P;
	}

	/** Triangles of one primitive that fall into a chunk. */
	struct FChunkPart
	{
		int32 Prim = 0;
		TArray<int32> Tris;
	};

	/**
	 * Splits world geometry into XY chunks (ImportUT2004.import_world_meshes): maps below ChunkMinTris stay one
	 * mesh; small chunks are merged into their largest neighbour to keep the actor count low.
	 */
	TArray<TArray<FChunkPart>> ChunkWorldGeometry(const TArray<FWorldPrim>& Prims)
	{
		int32 Total = 0;
		for (const FWorldPrim& P : Prims)
		{
			Total += P.NumTris();
		}
		TMap<FIntPoint, TArray<FChunkPart>> Chunks;
		if (Total < ChunkMinTris)
		{
			TArray<FChunkPart>& Parts = Chunks.Add(FIntPoint(0, 0));
			for (int32 p = 0; p < Prims.Num(); ++p)
			{
				FChunkPart& Part = Parts.AddDefaulted_GetRef();
				Part.Prim = p;
				for (int32 t = 0; t < Prims[p].NumTris(); ++t)
				{
					Part.Tris.Add(t);
				}
			}
		}
		else
		{
			for (int32 p = 0; p < Prims.Num(); ++p)
			{
				const FWorldPrim& P = Prims[p];
				TMap<FIntPoint, TArray<int32>> Cells;
				for (int32 t = 0; t < P.NumTris(); ++t)
				{
					const FVector3f& A = P.Pos[P.Idx[t * 3]];
					const FVector3f& B = P.Pos[P.Idx[t * 3 + 1]];
					const FVector3f& C = P.Pos[P.Idx[t * 3 + 2]];
					const double CX = (double(A.X) + B.X + C.X) / 3.0;
					const double CY = (double(A.Y) + B.Y + C.Y) / 3.0;
					Cells.FindOrAdd(FIntPoint(int32(FMath::FloorToDouble(CX / ChunkSize)), int32(FMath::FloorToDouble(CY / ChunkSize)))).Add(t);
				}
				for (TPair<FIntPoint, TArray<int32>>& Cell : Cells)
				{
					FChunkPart& Part = Chunks.FindOrAdd(Cell.Key).AddDefaulted_GetRef();
					Part.Prim = p;
					Part.Tris = MoveTemp(Cell.Value);
				}
			}
		}

		// merge tiny chunks into their largest neighbour
		TMap<FIntPoint, int32> Sizes;
		for (const TPair<FIntPoint, TArray<FChunkPart>>& C : Chunks)
		{
			int32 N = 0;
			for (const FChunkPart& Part : C.Value)
			{
				N += Part.Tris.Num();
			}
			Sizes.Add(C.Key, N);
		}
		TArray<FIntPoint> Order;
		Sizes.GenerateKeyArray(Order);
		Order.Sort([&Sizes](const FIntPoint& A, const FIntPoint& B)
		{
			const int32 SA = Sizes[A], SB = Sizes[B];
			return SA != SB ? SA < SB : (A.X != B.X ? A.X < B.X : A.Y < B.Y);
		});
		for (const FIntPoint& C : Order)
		{
			const int32* Size = Sizes.Find(C);
			if (!Size || *Size >= ChunkMergeTris || Chunks.Num() <= 1)
			{
				continue;
			}
			FIntPoint Target(MAX_int32, MAX_int32);
			int32 TargetSize = -1;
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					const FIntPoint N(C.X + DX, C.Y + DY);
					const int32* NSize = (DX || DY) ? Sizes.Find(N) : nullptr;
					if (NSize && *NSize > TargetSize)
					{
						TargetSize = *NSize;
						Target = N;
					}
				}
			}
			if (TargetSize < 0)
			{
				continue;
			}
			TArray<FChunkPart> Moved = Chunks.FindAndRemoveChecked(C);
			Chunks[Target].Append(MoveTemp(Moved));
			Sizes[Target] += Sizes.FindAndRemoveChecked(C);
		}

		TArray<FIntPoint> Keys;
		Chunks.GenerateKeyArray(Keys);
		Keys.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
		TArray<TArray<FChunkPart>> Out;
		for (const FIntPoint& K : Keys)
		{
			Out.Add(MoveTemp(Chunks[K]));
		}
		return Out;
	}

	/** Chunk -> provider mesh sections (one per primitive part; the provider merges equal keys into one slot). */
	void MakeSections(const TArray<FWorldPrim>& Prims, const TArray<FChunkPart>& Parts, TArray<FMeshSection>& Out)
	{
		for (const FChunkPart& Part : Parts)
		{
			const FWorldPrim& P = Prims[Part.Prim];
			FMeshSection& S = Out.AddDefaulted_GetRef();
			S.Material = P.Material;
			S.Variant = P.Variant;
			S.bCollides = P.bCollides;
			TMap<uint32, uint32> Remap;
			Remap.Reserve(Part.Tris.Num() * 2);
			S.Indices.Reserve(Part.Tris.Num() * 3);
			for (const int32 T : Part.Tris)
			{
				for (int32 k = 0; k < 3; ++k)
				{
					const uint32 V = P.Idx[T * 3 + k];
					uint32* Existing = Remap.Find(V);
					uint32 NV;
					if (Existing)
					{
						NV = *Existing;
					}
					else
					{
						NV = uint32(S.Positions.Num());
						Remap.Add(V, NV);
						S.Positions.Add(P.Pos[V]);
						S.Normals.Add(P.Nrm.IsValidIndex(V) ? P.Nrm[V] : FVector3f::UpVector);
						S.UV0.Add(P.UV.IsValidIndex(V) ? P.UV[V] : FVector2f::ZeroVector);
					}
					S.Indices.Add(NV);
				}
			}
		}
	}

	/**
	 * UE2 collision that is never drawn (invisible BSP surfaces, bHidden static mesh actors): it blocks like the
	 * visible geometry but has no render, shadow or ray tracing presence.
	 */
	void MakeCollisionOnly(AStaticMeshActor* Actor)
	{
		UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
		Comp->SetVisibility(false);
		Comp->SetHiddenInGame(true);
		Comp->SetCastShadow(false);
		Comp->bVisibleInRayTracing = false;
		Comp->bVisibleInReflectionCaptures = false;
		Comp->bAffectDistanceFieldLighting = false;
		Actor->Tags.Add(TEXT("ArenaCollisionOnly"));
	}

	AStaticMeshActor* SpawnMeshActor(UWorld* World, UStaticMesh* Mesh, const FTransform& Transform, bool bCollides)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
		if (!Actor)
		{
			return nullptr;
		}
		UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
		Comp->SetMobility(EComponentMobility::Static);
		Actor->SetActorScale3D(Transform.GetScale3D());
		Comp->SetStaticMesh(Mesh);
		if (bCollides)
		{
			Comp->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		}
		else
		{
			Comp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		}
		Comp->SetGenerateOverlapEvents(false);
		Comp->SetCanEverAffectNavigation(false);
		return Actor;
	}

	/** World geometry (BSP or terrain) -> chunk meshes (spawned later, after collision cooking). */
	TArray<UStaticMesh*> BuildWorldGeometry(const FString& MapName, const TCHAR* Kind, const TArray<FWorldPrim>& Prims)
	{
		TArray<UStaticMesh*> Meshes;
		if (Prims.Num() == 0)
		{
			return Meshes;
		}
		const TArray<TArray<FChunkPart>> Chunks = ChunkWorldGeometry(Prims);
		TArray<TArray<FMeshSection>> Sections;
		Sections.SetNum(Chunks.Num());
		ParallelFor(Chunks.Num(), [&](int32 i) { MakeSections(Prims, Chunks[i], Sections[i]); });
		for (int32 i = 0; i < Sections.Num(); ++i)
		{
			const FString Name = FString::Printf(TEXT("%s_%s_%02d"), Kind, *MapName.Replace(TEXT("-"), TEXT("_")), i);
			if (UStaticMesh* Mesh = ArenaRuntimeContent::BuildStaticMesh(Name, Sections[i]))
			{
				Meshes.Add(Mesh);
			}
		}
		return Meshes;
	}

	// ---- actors ----

	/** Lifts stay at their bottom key; other movers (doors...) are placed at their last (open) key. */
	void MoverPose(const TSharedPtr<FJsonObject>& Markers, const TSharedPtr<FJsonObject>& Actor, FVector& Loc, FRotator& Rot)
	{
		const int32 Index = int32(NumberField(Actor, TEXT("index"), -1));
		TSharedPtr<FJsonObject> Mover;
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Markers, TEXT("movers")))
		{
			const TSharedPtr<FJsonObject> M = V->AsObject();
			if (int32(NumberField(M, TEXT("actor_index"), -2)) == Index)
			{
				Mover = M;
				break;
			}
		}
		if (!Mover.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>& Keys = ArrayField(Mover, TEXT("key_pos"));
		const FString Tag = StringField(Actor, TEXT("tag"));
		bool bLift = false;
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Markers, TEXT("lifts")))
		{
			const TSharedPtr<FJsonObject> L = V->AsObject();
			if (StringField(L, TEXT("class")) == TEXT("LiftCenter") && StringField(L, TEXT("lift_tag")) == Tag)
			{
				bLift = true;
				break;
			}
		}
		if (Keys.Num() == 0 || bLift)
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Key = nullptr;
		if (Keys.Last()->TryGetArray(Key) && Key->Num() >= 3)
		{
			Loc += FVector((*Key)[0]->AsNumber(), (*Key)[1]->AsNumber(), (*Key)[2]->AsNumber());
		}
		const TArray<TSharedPtr<FJsonValue>>& KeyRots = ArrayField(Mover, TEXT("key_rot"));
		if (KeyRots.Num() == Keys.Num())
		{
			const TSharedPtr<FJsonObject> KR = KeyRots.Last()->AsObject();
			Rot += FRotator(NumberField(KR, TEXT("pitch"), 0.0), NumberField(KR, TEXT("yaw"), 0.0), NumberField(KR, TEXT("roll"), 0.0));
		}
	}

	/** world = location + R * (scale * (vertex - pre_pivot)) (exporter's transform note). */
	FTransform MeshActorTransform(const TSharedPtr<FJsonObject>& Actor, FVector Loc, const FRotator& Rot)
	{
		const double DrawScale = NumberField(Actor, TEXT("draw_scale"), 1.0);
		const FVector Scale = VectorField(Actor, TEXT("draw_scale3d"), FVector(1.0)) * (DrawScale == 0.0 ? 1.0 : DrawScale);
		const FVector PrePivot = VectorField(Actor, TEXT("pre_pivot"), FVector::ZeroVector);
		if (!PrePivot.IsNearlyZero(1e-4))
		{
			Loc -= Rot.RotateVector(Scale * PrePivot);
		}
		return FTransform(Rot, Loc, Scale);
	}

	int32 SpawnLights(UWorld* World, const TSharedPtr<FJsonObject>& Markers)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Markers, TEXT("lights")))
		{
			const TSharedPtr<FJsonObject> L = V->AsObject();
			if (BoolField(L, TEXT("sunlight")))
			{
				continue;
			}
			const double Brightness = NumberField(L, TEXT("brightness"), 0.0);
			if (Brightness <= 0.0)
			{
				continue;
			}
			const float Radius = float(FMath::Clamp(NumberField(L, TEXT("radius_world"), 1000.0), 200.0, 12000.0));
			// UE2 lights fall off gently over their whole radius: UE's non inverse-squared falloff, where the
			// unitless intensity is a plain brightness multiplier (UE2 brightness 64 ~ 1.0).
			const float Intensity = LightIntensityScale * float(FMath::Min(Brightness, 512.0) / 64.0);
			const FString Effect = StringField(L, TEXT("light_effect"));
			const bool bSpot = Effect == TEXT("Spotlight") || Effect == TEXT("StaticSpot") || BoolField(L, TEXT("directional"));
			FVector RGB = VectorField(L, TEXT("color_rgb"), FVector(1.0));
			const FColor Color(uint8(FMath::Clamp(FMath::RoundToInt(RGB.X * 255.0), 0, 255)), uint8(FMath::Clamp(FMath::RoundToInt(RGB.Y * 255.0), 0, 255)),
				uint8(FMath::Clamp(FMath::RoundToInt(RGB.Z * 255.0), 0, 255)), 255);
			const FVector Loc = VectorField(L, TEXT("location"), FVector::ZeroVector);

			ULocalLightComponent* Comp = nullptr;
			if (bSpot)
			{
				const FRotator SpotRot = RotatorField(L, TEXT("rotation"));
				ASpotLight* Spot = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Loc, SpotRot, Params);
				USpotLightComponent* SC = Spot ? Cast<USpotLightComponent>(Spot->GetLightComponent()) : nullptr;
				if (SC)
				{
					SC->UnregisterComponent();
					SC->SetMobility(EComponentMobility::Movable);
					// ASpotLight's root has a default relative pitch of -90 that SpawnActor composes with the spawn rotation
					Spot->SetActorRotation(SpotRot);
					const float Cone = float(FMath::Clamp(NumberField(L, TEXT("cone"), 128.0) / 256.0 * 90.0, 5.0, 80.0));
					SC->OuterConeAngle = Cone;
					SC->InnerConeAngle = Cone * 0.6f;
				}
				Comp = SC;
			}
			else
			{
				APointLight* Point = World->SpawnActor<APointLight>(APointLight::StaticClass(), Loc, FRotator::ZeroRotator, Params);
				Comp = Point ? Cast<UPointLightComponent>(Point->GetLightComponent()) : nullptr;
				if (Comp)
				{
					Comp->UnregisterComponent();
				}
			}
			UPointLightComponent* PC = Cast<UPointLightComponent>(Comp);
			if (!PC)
			{
				continue;
			}
			PC->SetMobility(EComponentMobility::Movable);
			PC->IntensityUnits = ELightUnits::Unitless;
			PC->Intensity = Intensity;
			PC->LightColor = Color;
			PC->AttenuationRadius = Radius;
			PC->SourceRadius = 10.f;
			PC->CastShadows = false;
			PC->bUseInverseSquaredFalloff = false;
			PC->LightFalloffExponent = LightFalloffExponent;
			PC->RegisterComponent();
			++Count;
		}
		return Count;
	}

	/** Sun, sky light, atmosphere, height fog and the unbound post process volume (ImportUT2004.add_sky). */
	void SpawnSky(UWorld* World, const TSharedPtr<FJsonObject>& Markers)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		TSharedPtr<FJsonObject> Sun;
		for (const TSharedPtr<FJsonValue>& V : ArrayField(Markers, TEXT("lights")))
		{
			if (BoolField(V->AsObject(), TEXT("sunlight")))
			{
				Sun = V->AsObject();
				break;
			}
		}
		FRotator SunRot = Sun.IsValid() ? RotatorField(Sun, TEXT("rotation")) : FRotator(-50.f, 35.f, 0.f);
		// UE2 sun rotators can have |pitch| > 90 (e.g. ONS-Torlan: -148): use the direction they point in, as a
		// normalized rotator, so the spawn transform keeps it (only the direction matters for a directional light).
		SunRot = SunRot.Vector().Rotation();
		if (SunRot.Pitch > -10.f)
		{
			SunRot.Pitch = -50.f;
		}
		// UE2 lights a map with the sun only when it has a Sunlight actor: without one there is none (night and space
		// maps such as DM-Big_City-Final-(Eisbaer) or CTF-FaceClassic stay dark and keep their sky box instead of
		// the atmosphere's lit blue sky; daylight maps without a sun are lit by their own lights and zone ambient)
		if (ADirectionalLight* D = Sun.IsValid() ? World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector(0.f, 0.f, 5000.f), SunRot, Params) : nullptr)
		{
			if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(D->GetLightComponent()))
			{
				C->UnregisterComponent();
				C->SetMobility(EComponentMobility::Movable);
				// ADirectionalLight's root has a default relative pitch of -46 that SpawnActor composes with the spawn
				// rotation: set the wanted direction explicitly.
				D->SetActorRotation(SunRot);
				C->Intensity = 4.f;
				C->bAtmosphereSunLight = true;
				C->AtmosphereSunLightIndex = 0;
				C->CastShadows = true;
				if (Sun.IsValid())
				{
					const FVector RGB = VectorField(Sun, TEXT("color_rgb"), FVector(1.0));
					C->LightColor = FColor(uint8(FMath::Clamp(FMath::RoundToInt(RGB.X * 255.0), 0, 255)), uint8(FMath::Clamp(FMath::RoundToInt(RGB.Y * 255.0), 0, 255)),
						uint8(FMath::Clamp(FMath::RoundToInt(RGB.Z * 255.0), 0, 255)), 255);
				}
				C->RegisterComponent();
				UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeMap: sun %s intensity %.1f color %s (map sunlight)"), *D->GetActorRotation().ToCompactString(), C->Intensity,
					*C->LightColor.ToString());
			}
		}
		if (ASkyLight* S = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FVector(0.f, 0.f, 3000.f), FRotator::ZeroRotator, Params))
		{
			if (USkyLightComponent* C = S->GetLightComponent())
			{
				C->UnregisterComponent();
				C->SetMobility(EComponentMobility::Movable);
				C->SourceType = SLS_CapturedScene;
				C->bRealTimeCapture = true;
				C->bLowerHemisphereIsBlack = false;
				C->Intensity = 1.f;
				C->RegisterComponent();
				C->SetCaptureIsDirty();
			}
		}
		World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FVector(0.f, 0.f, -200.f), FRotator::ZeroRotator, Params))
		{
			if (UExponentialHeightFogComponent* C = Fog->GetComponent())
			{
				C->SetFogDensity(0.004f);
				C->SetFogHeightFalloff(0.1f);
				C->SetStartDistance(1500.f);
			}
		}
		if (APostProcessVolume* PP = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params))
		{
			PP->bUnbound = true;
			PP->Priority = 10.f;
			PP->BlendWeight = 1.f;
			FPostProcessSettings& S = PP->Settings;
			S.bOverride_AutoExposureMethod = 1;
			S.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
			S.bOverride_AutoExposureApplyPhysicalCameraExposure = 1;
			S.AutoExposureApplyPhysicalCameraExposure = 0;
			S.bOverride_AutoExposureBias = 1;
			S.AutoExposureBias = ExposureBias;
			S.bOverride_BloomIntensity = 1;
			S.BloomIntensity = 0.6f;
			S.bOverride_VignetteIntensity = 1;
			S.VignetteIntensity = 0.25f;
		}
	}
}

#if !UE_BUILD_SHIPPING
namespace ArenaRuntimeMapLocal
{
	/**
	 * -TerrainPawnTest: drops a UE2-size capsule and box (radius 50, half height 88) onto random points of the
	 * terrain (ECC_Pawn sweeps, i.e. onto the pawn copies) and logs, per slope, how far above the true surface under
	 * the centre the shape's bottom rests. UE2 pawns rest with their centre point on the surface (0).
	 */
	void TerrainPawnTest(UWorld* World)
	{
		TArray<UPrimitiveComponent*> Visual;
		FBox Box(ForceInit);
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			UStaticMeshComponent* C = It->GetStaticMeshComponent();
			if (C && C->GetStaticMesh() && C->GetStaticMesh()->GetName().StartsWith(TEXT("Terrain_")))
			{
				Visual.Add(C);
				Box += C->Bounds.GetBox();
			}
		}
		if (!Visual.Num())
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("TerrainPawnTest: no terrain"));
			return;
		}
		const FCollisionShape Shapes[2] = { FCollisionShape::MakeCapsule(50.f, 88.f), FCollisionShape::MakeBox(FVector(50.f, 50.f, 88.f)) };
		TArray<AActor*> Other[2];
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(TEXT("ArenaTerrainBox")))
			{
				Other[0].Add(*It);
			}
			else if (It->ActorHasTag(TEXT("ArenaTerrainCapsule")))
			{
				Other[1].Add(*It);
			}
		}
		constexpr int32 Buckets = 5;
		const float Edges[Buckets + 1] = { 0.f, 5.f, 15.f, 25.f, 35.f, 46.f };
		TArray<float> Res[2][Buckets];
		FRandomStream Rng(7);
		for (int32 i = 0; i < 4000; ++i)
		{
			const FVector P(Rng.FRandRange(Box.Min.X, Box.Max.X), Rng.FRandRange(Box.Min.Y, Box.Max.Y), Box.Max.Z + 400.f);
			FHitResult Surf;
			const FVector End(P.X, P.Y, Box.Min.Z - 400.f);
			bool bSurf = false;
			for (UPrimitiveComponent* C : Visual)
			{
				FHitResult H;
				if (C->LineTraceComponent(H, P, End, FCollisionQueryParams()) && (!bSurf || H.ImpactPoint.Z > Surf.ImpactPoint.Z))
				{
					Surf = H;
					bSurf = true;
				}
			}
			if (!bSurf || Surf.ImpactNormal.Z < 0.7f)
			{
				continue;
			}
			const float Slope = FMath::RadiansToDegrees(FMath::Acos(float(Surf.ImpactNormal.Z)));
			int32 B = 0;
			while (B < Buckets - 1 && Slope >= Edges[B + 1])
			{
				++B;
			}
			for (int32 S = 0; S < 2; ++S)
			{
				// start just above the surface so nothing else (overhangs) is in the way
				const FVector From(P.X, P.Y, Surf.ImpactPoint.Z + 88.f + 120.f), To(P.X, P.Y, Surf.ImpactPoint.Z - 200.f);
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTerrainPawnTest));
				Q.AddIgnoredActors(Other[S]); // as UArenaCharacterMovement: each shape uses its own copy
				if (World->SweepSingleByChannel(Hit, From, To, FQuat::Identity, ECC_Pawn, Shapes[S], Q) && Hit.GetActor()
					&& Hit.GetActor()->ActorHasTag(TEXT("ArenaUE2Terrain")))
				{
					Res[S][B].Add(float(Hit.Location.Z - 88.f - Surf.ImpactPoint.Z) / 2.f);
				}
			}
		}
		for (int32 S = 0; S < 2; ++S)
		{
			FString Line;
			for (int32 B = 0; B < Buckets; ++B)
			{
				TArray<float>& R = Res[S][B];
				R.Sort();
				Line += FString::Printf(TEXT(" | %.0f-%.0f deg: n %d median %.1f p90 %.1f"), Edges[B], Edges[B + 1], R.Num(),
					R.Num() ? R[R.Num() / 2] : 0.f, R.Num() ? R[R.Num() * 9 / 10] : 0.f);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("TerrainPawnTest: %s rests above the surface under its centre (UU)%s"), S ? TEXT("box") : TEXT("capsule"), *Line);
		}
	}

	/**
	 * -SkyShotTest: rockets fired straight up from the player starts that have the level's sky (FakeBackdrop) above
	 * them; logs where each one ends against where the sky surface is (UE2 Projectile.HitWall: they explode there).
	 */
	void SkyShotTest(UWorld* World)
	{
		struct FShot { TWeakObjectPtr<AArenaProjectile> Proj; FVector Last; float SkyZ; };
		TSharedRef<TArray<FShot>> Shots = MakeShared<TArray<FShot>>();
		FArenaProjectileParams P;
		P.Kind = EArenaProjKind::Rocket;
		P.Speed = P.MaxSpeed = 2700.f;
		P.Damage = 90.f;
		P.DamageRadius = 440.f;
		P.LifeSpan = 30.f;
		P.CollisionRadius = 0.1f;
		P.WeaponName = TEXT("Rocket Launcher");
		int32 NoSky = 0;
		for (TActorIterator<APlayerStart> It(World); It && Shots->Num() < 6; ++It)
		{
			const FVector Start = It->GetActorLocation() + FVector(0.f, 0.f, 100.f);
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Start, Start + FVector(0.f, 0.f, 60000.f), ECC_Projectile) || !Hit.GetActor()
				|| !Hit.GetActor()->Tags.Contains(FName(TEXT("ArenaFakeBackdrop"))))
			{
				++NoSky;
				continue;
			}
			const FTransform T(FRotator(90.f, 0.f, 0.f), Start);
			if (AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
			{
				Proj->Init(P, nullptr, 1.f);
				Proj->FinishSpawning(T);
				Shots->Add({ Proj, Start, float(Hit.ImpactPoint.Z) });
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("SkyShotTest: %d rockets fired at the sky (%d player starts without sky above)"), Shots->Num(), NoSky);
		TSharedRef<FTimerHandle> Handle = MakeShared<FTimerHandle>();
		TWeakObjectPtr<UWorld> WeakWorld(World);
		World->GetTimerManager().SetTimer(*Handle, FTimerDelegate::CreateLambda([Shots, Handle, WeakWorld]()
		{
			int32 Live = 0;
			for (FShot& S : *Shots)
			{
				if (S.Proj.IsValid())
				{
					S.Last = S.Proj->GetActorLocation();
					++Live;
				}
				else if (S.SkyZ != MAX_flt)
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("SkyShotTest: rocket ended at Z=%.0f, sky surface at Z=%.0f: %s"), S.Last.Z, S.SkyZ,
						FMath::Abs(S.Last.Z - S.SkyZ) < 150.f ? TEXT("OK (exploded on the sky)") : TEXT("MISMATCH"));
					S.SkyZ = MAX_flt;
				}
			}
			if (Live == 0 && WeakWorld.IsValid())
			{
				WeakWorld->GetTimerManager().ClearTimer(*Handle);
			}
		}), 0.02f, true);
	}
}
#endif

void ArenaRuntimeMap::ApplyUE2MeshCollision(UStaticMeshComponent* Visual, UStaticMeshComponent* Model, bool bModelBox, bool bModelLine,
	bool bBlockNonZeroExtent, bool bBlockZeroExtent)
{
	// players and what moves with an extent like them; physics bodies (vehicles, ragdolls; UE2 Karma); shots and sight
	static const ECollisionChannel ExtentChannels[] = { ECC_Pawn, ECC_WorldStatic, ECC_WorldDynamic };
	static const ECollisionChannel PhysicsChannels[] = { ECC_Vehicle, ECC_PhysicsBody, ECC_Destructible };
	static const ECollisionChannel LineChannels[] = { ECC_Visibility, ECC_Camera, ECC_Weapon, ECC_Projectile };
	const UBodySetup* Body = Visual && Visual->GetStaticMesh() ? Visual->GetStaticMesh()->GetBodySetup() : nullptr;
	const bool bVisualHasCollision = Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple;
	if (!Model)
	{
		bModelBox = bModelLine = false;
	}
	UStaticMeshComponent* ExtentOwner = !bBlockNonZeroExtent ? nullptr : bModelBox ? Model : Visual;
	UStaticMeshComponent* LineOwner = !bBlockZeroExtent ? nullptr : bModelLine ? Model : Visual;
	// Karma collides with the mesh (approximated by its colliding sections), or the model when there are none
	UStaticMeshComponent* PhysicsOwner = Model && !bVisualHasCollision ? Model : Visual;
	for (UStaticMeshComponent* Comp : { Visual, Model })
	{
		if (!Comp)
		{
			continue;
		}
		bool bAny = false;
		auto Set = [Comp, &bAny](TConstArrayView<ECollisionChannel> Channels, bool bBlock)
		{
			for (const ECollisionChannel C : Channels)
			{
				Comp->SetCollisionResponseToChannel(C, bBlock ? ECR_Block : ECR_Ignore);
			}
			bAny |= bBlock;
		};
		Set(ExtentChannels, ExtentOwner == Comp);
		Set(PhysicsChannels, PhysicsOwner == Comp);
		Set(LineChannels, LineOwner == Comp);
		if (LineOwner == Comp && ExtentOwner != Comp)
		{
			Comp->ComponentTags.AddUnique(ARENA_SHOT_BLOCKER_TAG);
		}
		if (!bAny)
		{
			Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
}

FString ArenaRuntimeMap::GetMapDir(const FString& MapName)
{
	// the converter names the folder with the package name's file-safe form (FConvExporter::Safe): UT2004 map names
	// may contain "[", "]", "{", "(", "!", spaces...
	return ArenaContent::GetCacheDir() / TEXT("maps") / ArenaRuntimePrivate::CacheSafe(MapName);
}

bool ArenaRuntimeMap::HasMap(const FString& MapName)
{
	return !MapName.IsEmpty() && FPaths::FileExists(GetMapDir(MapName) / TEXT("map.json"));
}

bool ArenaRuntimeMap::BuildWorld(UWorld* World, const FString& MapName)
{
	using namespace ArenaRuntimeMapLocal;

	if (!World || !ArenaRuntimeContent::IsActive())
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("RuntimeMap: runtime content is not active, cannot build %s"), *MapName);
		return false;
	}
	const double T0 = FPlatformTime::Seconds();
	// Assets of the previous map are no longer referenced by any world.
	ArenaRuntimeContent::TrimCache();

	const FString Dir = GetMapDir(MapName);
	const TSharedPtr<FJsonObject> Doc = LoadJson(Dir / TEXT("map.json"));
	if (!Doc.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("RuntimeMap: could not read %s/map.json"), *Dir);
		return false;
	}
	const TSharedPtr<FJsonObject> Markers = ObjectField(Doc, TEXT("markers"));
	const TArray<TSharedPtr<FJsonValue>>& Actors = ArrayField(Doc, TEXT("actors"));
	// actors the native UE2 world classes take over (movers place their own meshes)
	const TSet<int32> NativeActors = ArenaUE2World::GetHandledActorIndices(Doc);
	const double TJson = FPlatformTime::Seconds();

	// ---- what the map needs (ImportUT2004.map_assets): prefetch on worker threads ----
	TArray<TSharedPtr<FJsonObject>> MeshActors;
	TSet<FString> MeshPaths, MaterialPaths;
	for (const TSharedPtr<FJsonValue>& V : Actors)
	{
		const TSharedPtr<FJsonObject> A = V->AsObject();
		FString Mesh;
		// bHidden mesh actors are not drawn but still collide in UE2 (CTF-Maul's tower collision hulls)
		const bool bHidden = A.IsValid() && BoolField(A, TEXT("hidden"));
		if (!A.IsValid() || !HasStaticMesh(A, Mesh) || IsGameplayActor(A) || (bHidden && !ActorCollides(A)))
		{
			continue;
		}
		if (NativeActors.Contains(int32(NumberField(A, TEXT("index"), -1))))
		{
			MeshPaths.Add(Mesh); // still prefetched
			continue;
		}
		MeshActors.Add(A);
		MeshPaths.Add(Mesh);
		if (bHidden)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Skin : ArrayField(A, TEXT("skins")))
		{
			FString S;
			if (Skin.IsValid() && Skin->TryGetString(S) && !S.IsEmpty() && S != TEXT("None"))
			{
				MaterialPaths.Add(S);
			}
		}
	}
	for (const TSharedPtr<FJsonValue>& V : ArrayField(ObjectField(Doc, TEXT("bsp")), TEXT("materials")))
	{
		MaterialPaths.Add(V->AsString());
	}
	for (const TSharedPtr<FJsonValue>& V : ArrayField(Markers, TEXT("terrains")))
	{
		const TArray<TSharedPtr<FJsonValue>>& Layers = ArrayField(V->AsObject(), TEXT("layers"));
		if (Layers.Num() > 0)
		{
			MaterialPaths.Add(StringField(Layers[0]->AsObject(), TEXT("texture")));
		}
	}
	ArenaRuntimeContent::PrefetchStaticMeshes(MeshPaths.Array());
	ArenaRuntimeContent::PrefetchMaterials(MaterialPaths.Array());
	const double TPrefetch = FPlatformTime::Seconds();

	// ---- BSP and terrain ----
	TArray<UStaticMesh*> BspMeshes, SkyBspMeshes, BspCollisionMeshes, BackdropMeshes, TerrainMeshes, TerrainPawnMeshes, TerrainBoxMeshes;
	int32 BspTris = 0;
	{
		TArray<FWorldPrim> Prims;
		const FString BspFile = Dir / TEXT("bsp.gltf");
		if (!FPaths::FileExists(BspFile))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeMap: %s has no bsp.gltf"), *MapName);
		}
		else if (!ReadBsp(BspFile, Prims))
		{
			UE_LOG(LogOpenUT2k4, Error, TEXT("RuntimeMap: could not read %s"), *BspFile);
		}
		for (const FWorldPrim& P : Prims)
		{
			BspTris += P.NumTris();
		}
		// the sky zone's surfaces become separate chunks for the 3D sky box
		TArray<FWorldPrim> SkyPrims;
		for (int32 i = Prims.Num() - 1; i >= 0; --i)
		{
			if (Prims[i].bSky)
			{
				SkyPrims.Add(MoveTemp(Prims[i]));
				Prims.RemoveAt(i);
			}
		}
		// invisible solid surfaces become separate, never drawn collision chunks
		TArray<FWorldPrim> CollisionPrims, BackdropPrims;
		for (int32 i = Prims.Num() - 1; i >= 0; --i)
		{
			if (Prims[i].bCollisionOnly)
			{
				(Prims[i].bFakeBackdrop ? BackdropPrims : CollisionPrims).Add(MoveTemp(Prims[i]));
				Prims.RemoveAt(i);
			}
		}
		BackdropMeshes = BuildWorldGeometry(MapName, TEXT("SkyBackdrop"), BackdropPrims);
		BspMeshes = BuildWorldGeometry(MapName, TEXT("BSP"), Prims);
		BspCollisionMeshes = BuildWorldGeometry(MapName, TEXT("BSPCollision"), CollisionPrims);
		SkyBspMeshes = BuildWorldGeometry(MapName, TEXT("SkyBSP"), SkyPrims);
	}
	{
		TMap<int32, TSharedPtr<FJsonObject>> ActorByIndex;
		for (const TSharedPtr<FJsonValue>& V : Actors)
		{
			const TSharedPtr<FJsonObject> A = V->AsObject();
			ActorByIndex.Add(int32(NumberField(A, TEXT("index"), -1)), A);
		}
		const TArray<TSharedPtr<FJsonValue>>& Terrains = ArrayField(Markers, TEXT("terrains"));
		TArray<FWorldPrim> Prims;
		Prims.SetNum(Terrains.Num());
		TArray<bool> bOk;
		bOk.Init(false, Terrains.Num());
		if (Terrains.Num() > 0)
		{
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")); // not thread-safe: load here
		}
		ParallelFor(Terrains.Num(), [&](int32 i)
		{
			const TSharedPtr<FJsonObject> T = Terrains[i]->AsObject();
			const TSharedPtr<FJsonObject>* Actor = ActorByIndex.Find(int32(NumberField(T, TEXT("actor_index"), -1)));
			bOk[i] = TerrainPrimitive(T, Actor ? *Actor : nullptr, Prims[i]);
		});
		for (int32 i = Prims.Num() - 1; i >= 0; --i)
		{
			if (!bOk[i])
			{
				Prims.RemoveAt(i);
			}
		}
		TerrainMeshes = BuildWorldGeometry(MapName, TEXT("Terrain"), Prims);
		TArray<FWorldPrim> CapsulePrims, BoxPrims;
		for (const FWorldPrim& P : Prims)
		{
			CapsulePrims.Add(PawnTerrain(P, false));
			BoxPrims.Add(PawnTerrain(P, true));
		}
		TerrainPawnMeshes = BuildWorldGeometry(MapName, TEXT("TerrainPawn"), CapsulePrims);
		TerrainBoxMeshes = BuildWorldGeometry(MapName, TEXT("TerrainPawnBox"), BoxPrims);
	}
	const double TWorld = FPlatformTime::Seconds();

	// ---- meshes of the static mesh actors, then all collision cooked in parallel ----
	TMap<FString, UStaticMesh*> LoadedMeshes;
	struct FCollisionModel { UStaticMesh* Mesh = nullptr; bool bBox = false; bool bLine = false; };
	TMap<FString, FCollisionModel> CollisionModels;
	TArray<UStaticMesh*> CookList = BspMeshes;
	CookList.Append(SkyBspMeshes);
	CookList.Append(BspCollisionMeshes);
	CookList.Append(BackdropMeshes);
	CookList.Append(TerrainMeshes);
	CookList.Append(TerrainPawnMeshes);
	CookList.Append(TerrainBoxMeshes);
	for (const FString& Path : MeshPaths)
	{
		UStaticMesh* Mesh = ArenaRuntimeContent::LoadStaticMeshByObjectPath(Path);
		LoadedMeshes.Add(Path, Mesh);
		if (Mesh)
		{
			CookList.Add(Mesh);
			FCollisionModel Model;
			Model.Mesh = ArenaRuntimeContent::LoadStaticMeshCollisionModel(Path, Model.bBox, Model.bLine);
			if (Model.Mesh)
			{
				CookList.Add(Model.Mesh);
				CollisionModels.Add(Path, Model);
			}
		}
	}
	const double TLoad = FPlatformTime::Seconds();
	ArenaRuntimeContent::CookCollision(CookList);
	const double TCook = FPlatformTime::Seconds();

	// ---- actors: BSP / terrain chunks, static mesh actors ----
	int32 NumBsp = 0, NumTerrain = 0;
	for (UStaticMesh* Mesh : BspMeshes)
	{
		NumBsp += SpawnMeshActor(World, Mesh, FTransform::Identity, true) ? 1 : 0;
	}
	for (UStaticMesh* Mesh : BackdropMeshes)
	{
		if (AStaticMeshActor* Col = SpawnMeshActor(World, Mesh, FTransform::Identity, true))
		{
			MakeCollisionOnly(Col);
			// sky probes (ArenaImportedLighting's openness) look through it
			Col->Tags.Add(TEXT("ArenaFakeBackdrop"));
			++NumBsp;
		}
	}
	for (UStaticMesh* Mesh : BspCollisionMeshes)
	{
		if (AStaticMeshActor* Col = SpawnMeshActor(World, Mesh, FTransform::Identity, true))
		{
			MakeCollisionOnly(Col);
			++NumBsp;
		}
	}
	for (UStaticMesh* Mesh : SkyBspMeshes)
	{
		if (AStaticMeshActor* Sky = SpawnMeshActor(World, Mesh, FTransform::Identity, true))
		{
			Sky->Tags.Add(TEXT("ArenaSkyZone"));
			++NumBsp;
		}
	}
	for (UStaticMesh* Mesh : TerrainMeshes)
	{
		if (AStaticMeshActor* Terrain = SpawnMeshActor(World, Mesh, FTransform::Identity, true))
		{
			// pawns collide with the lowered copies below (PawnTerrain); everything else with the true surface
			Terrain->GetStaticMeshComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			Terrain->GetStaticMeshComponent()->ComponentTags.Add(ARENA_SHOT_BLOCKER_TAG);
			++NumTerrain;
		}
	}
	for (int32 Shape = 0; Shape < 2; ++Shape)
	{
		for (UStaticMesh* Mesh : Shape == 0 ? TerrainPawnMeshes : TerrainBoxMeshes)
		{
			if (AStaticMeshActor* Terrain = SpawnMeshActor(World, Mesh, FTransform::Identity, true))
			{
				MakeCollisionOnly(Terrain);
				UStaticMeshComponent* Comp = Terrain->GetStaticMeshComponent();
				Comp->SetCollisionResponseToAllChannels(ECR_Ignore);
				Comp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
				// UArenaCharacterMovement: UE2 terrain only blocks pawns from above; a pawn uses the copy for its shape
				Terrain->Tags.Add(TEXT("ArenaUE2Terrain"));
				Terrain->Tags.Add(Shape == 0 ? TEXT("ArenaTerrainCapsule") : TEXT("ArenaTerrainBox"));
			}
		}
	}
	int32 Placed = 0, Skipped = 0;
	for (const TSharedPtr<FJsonObject>& A : MeshActors)
	{
		FString MeshPath;
		HasStaticMesh(A, MeshPath);
		UStaticMesh* Mesh = LoadedMeshes.FindRef(MeshPath);
		if (!Mesh)
		{
			++Skipped;
			continue;
		}
		FVector Loc = VectorField(A, TEXT("location"), FVector::ZeroVector);
		FRotator Rot = RotatorField(A, TEXT("rotation"));
		bool bMover = false;
		for (const TSharedPtr<FJsonValue>& C : ArrayField(A, TEXT("class_chain")))
		{
			bMover |= C->AsString() == TEXT("Mover");
		}
		if (bMover)
		{
			MoverPose(Markers, A, Loc, Rot);
		}
		const bool bCollides = ActorCollides(A);
		AStaticMeshActor* Actor = SpawnMeshActor(World, Mesh, MeshActorTransform(A, Loc, Rot), bCollides);
		if (!Actor)
		{
			++Skipped;
			continue;
		}
		if (bCollides)
		{
			const FCollisionModel* Model = CollisionModels.Find(MeshPath);
			UStaticMeshComponent* ColComp = nullptr;
			if (Model)
			{
				if (AStaticMeshActor* ColActor = SpawnMeshActor(World, Model->Mesh, Actor->GetActorTransform(), true))
				{
					MakeCollisionOnly(ColActor);
					ColComp = ColActor->GetStaticMeshComponent();
				}
			}
			ArenaRuntimeMap::ApplyUE2MeshCollision(Actor->GetStaticMeshComponent(), ColComp, Model && Model->bBox, Model && Model->bLine,
				BlocksTraces(A, TEXT("bBlockNonZeroExtentTraces")), BlocksTraces(A, TEXT("bBlockZeroExtentTraces")));
		}
		if (BoolField(A, TEXT("hidden")))
		{
			MakeCollisionOnly(Actor);
			++Placed;
			continue;
		}
		// map static mesh (the UE2 world code checks which ones are in the sky zone)
		Actor->Tags.Add(TEXT("ArenaMapMesh"));
		const TArray<TSharedPtr<FJsonValue>>& Skins = ArrayField(A, TEXT("skins"));
		for (int32 i = 0; i < Skins.Num(); ++i)
		{
			FString Skin;
			if (!Skins[i].IsValid() || !Skins[i]->TryGetString(Skin) || Skin.IsEmpty() || Skin == TEXT("None"))
			{
				continue;
			}
			const int32 Slot = ArenaRuntimeContent::GetStaticMeshSlotForSection(MeshPath, i);
			if (Slot >= 0)
			{
				Actor->GetStaticMeshComponent()->SetMaterial(Slot, ArenaRuntimeContent::LoadMaterialByObjectPath(Skin));
			}
		}
		++Placed;
	}
	const double TMeshes = FPlatformTime::Seconds();

	// ---- lights and sky ----
	const int32 NumLights = SpawnLights(World, Markers);
	SpawnSky(World, Markers);
	// UE2 LevelInfo distance fog: the fog of zones without a ZoneInfo actor (UArenaZoneFogSubsystem)
	for (const TSharedPtr<FJsonValue>& V : ArrayField(Doc, TEXT("actors")))
	{
		const TSharedPtr<FJsonObject> A = V.IsValid() ? V->AsObject() : nullptr;
		if (A.IsValid() && StringField(A, TEXT("class")) == TEXT("LevelInfo"))
		{
			if (UArenaZoneFogSubsystem* ZoneFog = World->GetSubsystem<UArenaZoneFogSubsystem>())
			{
				ZoneFog->SetLevelFog(ObjectField(A, TEXT("properties")));
			}
			break;
		}
	}

	// Bodies created now only reach the scene query structure with the next physics tick; BuildImported traces
	// for floors right away, so push them through now.
	if (FPhysScene* Scene = World->GetPhysicsScene())
	{
		Scene->Flush();
	}
	// UE2 world behaviour: zones, volumes, movers, triggers, teleporters, emitters, ambient sounds, sky box
	{
		ArenaUE2World::SpawnWorldActors(World, Doc, Dir);
		if (FPhysScene* Scene = World->GetPhysicsScene())
		{
			Scene->Flush();
		}
	}
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("TerrainPawnTest")))
	{
		TWeakObjectPtr<UWorld> WeakWorld(World);
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			if (WeakWorld.IsValid())
			{
				ArenaRuntimeMapLocal::TerrainPawnTest(WeakWorld.Get());
			}
		}), 3.f, false);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SkyShotTest")))
	{
		TWeakObjectPtr<UWorld> WeakWorld(World);
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			if (WeakWorld.IsValid())
			{
				ArenaRuntimeMapLocal::SkyShotTest(WeakWorld.Get());
			}
		}), 6.f, false);
	}
#endif
	const double TEnd = FPlatformTime::Seconds();

	UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeMap: built %s in %.2f s (json %.2f, prefetch %.2f [%d meshes, %d materials], bsp/terrain %.2f [%d BSP chunk(s) %d tris, %d terrain chunk(s)], mesh load %.2f, collision cook %.2f, actors %.2f [%d placed, %d skipped], lights+sky+flush %.2f [%d lights])"),
		*MapName, TEnd - T0, TJson - T0, TPrefetch - TJson, MeshPaths.Num(), MaterialPaths.Num(), TWorld - TPrefetch, NumBsp, BspTris, NumTerrain,
		TLoad - TWorld, TCook - TLoad, TMeshes - TCook, Placed, Skipped, TEnd - TMeshes, NumLights);
	UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeMap: provider %s"), *ArenaRuntimeContent::GetStats());
	return NumBsp + NumTerrain + Placed > 0;
}

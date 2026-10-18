#include "ArenaAssets.h"
#include "OpenUT2k4.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	template <typename T>
	T* LoadCached(TStrongObjectPtr<T>& Cache, bool& bTried, const TCHAR* Path, const TCHAR* FallbackPath = nullptr)
	{
		if (!bTried)
		{
			bTried = true;
			T* Obj = LoadObject<T>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Obj && FallbackPath)
			{
				Obj = LoadObject<T>(nullptr, FallbackPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Obj)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaAssets: missing asset %s"), Path);
			}
			Cache.Reset(Obj);
		}
		return Cache.Get();
	}

	bool IsSurfaceGenerated()
	{
		UMaterialInterface* M = ArenaAssets::SurfaceMaterial();
		return M && M->GetPathName().StartsWith(TEXT("/Game/"));
	}

	bool IsGlowGenerated()
	{
		UMaterialInterface* M = ArenaAssets::GlowMaterial();
		return M && M->GetPathName().StartsWith(TEXT("/Game/"));
	}
}

#define ARENA_CACHED(Type, Name, Path, Fallback) \
	Type* ArenaAssets::Name() \
	{ \
		static TStrongObjectPtr<Type> Cache; \
		static bool bTried = false; \
		return LoadCached<Type>(Cache, bTried, TEXT(Path), Fallback); \
	}

ARENA_CACHED(UStaticMesh, Cube, "/Engine/BasicShapes/Cube.Cube", nullptr)
ARENA_CACHED(UStaticMesh, Sphere, "/Engine/BasicShapes/Sphere.Sphere", nullptr)
ARENA_CACHED(UStaticMesh, Cylinder, "/Engine/BasicShapes/Cylinder.Cylinder", nullptr)
ARENA_CACHED(UStaticMesh, Cone, "/Engine/BasicShapes/Cone.Cone", nullptr)
ARENA_CACHED(UMaterialInterface, SurfaceMaterial, "/Game/Materials/M_ArenaSurface.M_ArenaSurface", TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"))
ARENA_CACHED(UMaterialInterface, GlowMaterial, "/Game/Materials/M_ArenaGlow.M_ArenaGlow", TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"))

#undef ARENA_CACHED

UMaterialInstanceDynamic* ArenaAssets::MakeSurface(UObject* Outer, const FLinearColor& Color, float Roughness, float Metallic, const FLinearColor& Emissive, float GridScale)
{
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SurfaceMaterial(), Outer);
	if (MID)
	{
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		if (IsSurfaceGenerated())
		{
			MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
			MID->SetScalarParameterValue(TEXT("Roughness"), Roughness);
			MID->SetScalarParameterValue(TEXT("Metallic"), Metallic);
			MID->SetScalarParameterValue(TEXT("GridScale"), GridScale);
		}
	}
	return MID;
}

UMaterialInstanceDynamic* ArenaAssets::MakeGlow(UObject* Outer, const FLinearColor& Color, float Intensity, float Opacity)
{
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(GlowMaterial(), Outer);
	if (MID)
	{
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		if (IsGlowGenerated())
		{
			MID->SetScalarParameterValue(TEXT("Intensity"), Intensity);
			MID->SetScalarParameterValue(TEXT("Opacity"), Opacity);
		}
	}
	return MID;
}

USoundBase* ArenaAssets::Sound(FName Name)
{
	if (Name.IsNone())
	{
		return nullptr;
	}
	static TMap<FName, TStrongObjectPtr<USoundBase>> Cache;
	if (TStrongObjectPtr<USoundBase>* Found = Cache.Find(Name))
	{
		return Found->Get();
	}
	const FString Path = FString::Printf(TEXT("/Game/Audio/%s.%s"), *Name.ToString(), *Name.ToString());
	USoundBase* Snd = LoadObject<USoundBase>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Snd)
	{
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("ArenaAssets: missing sound %s"), *Path);
	}
	Cache.Add(Name, TStrongObjectPtr<USoundBase>(Snd));
	return Snd;
}

void ArenaAssets::PlaySoundAt(const UObject* WorldContext, FName Name, const FVector& Location, float Volume, float Pitch)
{
	if (USoundBase* Snd = Sound(Name))
	{
		UGameplayStatics::PlaySoundAtLocation(WorldContext, Snd, Location, Volume, Pitch);
	}
}

void ArenaAssets::PlaySound2D(const UObject* WorldContext, FName Name, float Volume, float Pitch)
{
	if (USoundBase* Snd = Sound(Name))
	{
		UGameplayStatics::PlaySound2D(WorldContext, Snd, Volume, Pitch);
	}
}

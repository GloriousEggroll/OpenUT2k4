#include "ArenaZoneFog.h"
#include "OpenUT2k4.h"
#include "ArenaWorldZones.h"
#include "ArenaUE2World.h"
#include "Dom/JsonObject.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	/** Seconds to blend between zone fogs (UE2 switched instantly; this only hides the pop). */
	constexpr float ZoneFogBlendTime = 0.25f;
	/**
	 * Fog opacity at DistanceFogEnd. UE2 fog was linear (half way: 50%); exponential fog dense enough to be opaque at
	 * End would already be ~86% opaque half way, so match the middle instead (55% there, 80% at End, ~96% at 2x End).
	 */
	constexpr float OpacityAtEnd = 0.8f;
	/** Height falloff for the zone fog: practically uniform (UE2 distance fog does not depend on height). */
	constexpr float ZoneFogFalloff = 0.001f;
	/** Scale from the zone's linear fog colour to inscattering luminance (tuned against the auto exposure). */
	constexpr float FogLuminanceScale = 1.0f;
#if !UE_BUILD_SHIPPING
	float LuminanceScale()
	{
		static float Scale = [] { float V = FogLuminanceScale; FParse::Value(FCommandLine::Get(), TEXT("ZoneFogScale="), V); return V; }();
		return Scale;
	}
#else
	float LuminanceScale() { return FogLuminanceScale; }
#endif

	/** FogDensity (UE: per 1000 cm) so that the fog is OpacityAtEnd opaque at (End - Start) past the start. */
	float DensityFor(float Start, float End)
	{
		const float Span = FMath::Max(End - Start, 100.f);
		return FMath::Clamp(-FMath::Loge(1.f - OpacityAtEnd) / Span * 1000.f, 0.0005f, 20.f);
	}
}

bool UArenaZoneFogSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UArenaZoneFogSubsystem::SetLevelFog(const TSharedPtr<FJsonObject>& Props)
{
	bLevelFog = false;
	if (!Props.IsValid())
	{
		return;
	}
	Props->TryGetBoolField(TEXT("bDistanceFog"), bLevelFog);
	// UE2 ZoneInfo defaults where the level does not set them
	double Start = 3000.0, End = 8000.0;
	Props->TryGetNumberField(TEXT("DistanceFogStart"), Start);
	Props->TryGetNumberField(TEXT("DistanceFogEnd"), End);
	LevelFogStart = float(Start * ArenaUE2World::Scale);
	LevelFogEnd = float(End * ArenaUE2World::Scale);
	const TSharedPtr<FJsonObject>* Color = nullptr;
	if (Props->TryGetObjectField(TEXT("DistanceFogColor"), Color) && Color && Color->IsValid())
	{
		LevelFogColor = FColor(uint8((*Color)->GetIntegerField(TEXT("R"))), uint8((*Color)->GetIntegerField(TEXT("G"))), uint8((*Color)->GetIntegerField(TEXT("B"))));
	}
	bAnyFogZone |= bLevelFog;
}

TStatId UArenaZoneFogSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaZoneFogSubsystem, STATGROUP_Tickables);
}

void UArenaZoneFogSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay())
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("NoZoneFog"));
	if (bDisabled)
	{
		return;
	}
#endif
	// Only levels with distance fog zones need any of this (checked until the level's zones exist).
	if (!bAnyFogZone)
	{
		if (World->GetTimeSeconds() < NextZoneCheck)
		{
			return;
		}
		NextZoneCheck = World->GetTimeSeconds() + 1.f;
		for (TActorIterator<AArenaZoneInfo> It(World); It; ++It)
		{
			bAnyFogZone |= It->bDistanceFog && !It->IsSkyZone();
		}
		if (!bAnyFogZone)
		{
			return;
		}
	}
	UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World);
	APlayerController* PC = World->GetFirstPlayerController();
	if (!Zones || !PC || !PC->PlayerCameraManager)
	{
		return;
	}
	UExponentialHeightFogComponent* C = FogComp.Get();
	if (!C)
	{
		TActorIterator<AExponentialHeightFog> It(World);
		C = It ? It->GetComponent() : nullptr;
		FogComp = C;
		bHaveBase = false;
		if (!C)
		{
			return;
		}
	}
	// Water / lava / fog volumes drive the fog themselves while the camera is inside (UArenaWorldZoneSubsystem).
	if (Zones->IsViewFogActive())
	{
		bWritten = false;
		return;
	}
	if (!bHaveBase)
	{
		BaseDensity = C->FogDensity;
		BaseFalloff = C->FogHeightFalloff;
		BaseStart = C->StartDistance;
		BaseColor = C->FogInscatteringLuminance;
		BaseMaxOpacity = C->FogMaxOpacity;
		BaseDirectional = C->DirectionalInscatteringLuminance;
		BaseSkyAmbient = C->SkyAtmosphereAmbientContributionColorScale;
		bHaveBase = true;
	}

	AArenaZoneInfo* Zone = Zones->GetZoneAt(PC->PlayerCameraManager->GetCameraLocation());
	// zones without a ZoneInfo actor are the LevelInfo's (UE2: LevelInfo is the default zone's ZoneInfo)
	const bool bFogZone = Zone ? Zone->bDistanceFog && !Zone->IsSkyZone() : bLevelFog;
	if (bFogZone)
	{
		ZoneStart = FMath::Max(Zone ? Zone->DistanceFogStart : LevelFogStart, 0.f);
		ZoneEnd = FMath::Max(Zone ? Zone->DistanceFogEnd : LevelFogEnd, ZoneStart + 100.f);
		ZoneColor = FLinearColor(Zone ? Zone->DistanceFogColor : LevelFogColor) * LuminanceScale(); // FColor -> linear (sRGB decode)
	}
	const bool bZoneChanged = LastZone.Get() != Zone;
#if !UE_BUILD_SHIPPING
	if (bZoneChanged && FParse::Param(FCommandLine::Get(), TEXT("ZoneFogReport")))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ZoneFog: camera in %s: %s"), Zone ? *Zone->UE2Name : TEXT("no zone"),
			bFogZone ? *FString::Printf(TEXT("distance fog %.0f-%.0f cm color %s density %.4f"), ZoneStart, ZoneEnd, *(Zone ? Zone->DistanceFogColor : LevelFogColor).ToString(), DensityFor(ZoneStart, ZoneEnd)) : TEXT("level fog"));
	}
#endif
	LastZone = Zone;

	const float Target = bFogZone ? 1.f : 0.f;
	const float NewWeight = FMath::FInterpConstantTo(Weight, Target, DeltaTime, 1.f / ZoneFogBlendTime);
	if (bWritten && NewWeight == Weight && !bZoneChanged)
	{
		return; // nothing changes
	}
	Weight = NewWeight;
	bWritten = true;
	const float W = Weight;
	C->SetFogDensity(FMath::Lerp(BaseDensity, DensityFor(ZoneStart, ZoneEnd), W));
	C->SetFogHeightFalloff(FMath::Lerp(BaseFalloff, ZoneFogFalloff, W));
	C->SetStartDistance(FMath::Lerp(BaseStart, ZoneStart, W));
	C->SetFogInscatteringColor(FMath::Lerp(BaseColor, ZoneColor, W));
	C->SetFogMaxOpacity(FMath::Lerp(BaseMaxOpacity, 1.f, W));
	// UE2 fog is the flat zone colour: no sun glow or sky atmosphere light added on top
	C->SetDirectionalInscatteringColor(FMath::Lerp(BaseDirectional, FLinearColor::Black, W));
	C->SetSkyAtmosphereAmbientContributionColorScale(FMath::Lerp(BaseSkyAmbient, FLinearColor::Black, W));
}

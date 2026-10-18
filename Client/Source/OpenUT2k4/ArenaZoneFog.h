#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaZoneFog.generated.h"

class UExponentialHeightFogComponent;

/**
 * UE2 zone distance fog (ZoneInfo bDistanceFog / DistanceFogStart / DistanceFogEnd / DistanceFogColor), per camera
 * zone like UE2: the level's exponential height fog takes the fog of the zone the camera is in (AArenaZoneInfo via
 * UArenaWorldZoneSubsystem::GetZoneAt) and goes back to the level's own fog in zones without distance fog.
 *
 * UE2's linear fog (clear at Start, fully fog coloured at End) becomes uniform exponential fog (no height falloff)
 * from Start, about as opaque as the linear fog half way (80% at End). The colour is the zone's sRGB colour as emissive inscattering.
 * Zone changes blend over ZoneFogBlendTime (UE2 switched instantly). While the camera's physics volume has its own
 * fog (water, lava; UArenaWorldZoneSubsystem::IsViewFogActive) that takes over and this waits.
 * The 3D sky box keeps its own look (AArenaSkybox sets FogCutoffDistance). Test hooks (non-shipping): -ZoneFogReport
 * logs every zone change with the applied values, -NoZoneFog turns it off.
 */
UCLASS()
class OPENUT2K4_API UArenaZoneFogSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** LevelInfo's distance fog (its "properties"), used in zones without a ZoneInfo actor. */
	void SetLevelFog(const TSharedPtr<class FJsonObject>& Props);

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	TWeakObjectPtr<UExponentialHeightFogComponent> FogComp;
	/** The level's own fog, restored in zones without distance fog. */
	float BaseDensity = 0.f, BaseFalloff = 0.f, BaseStart = 0.f, BaseMaxOpacity = 1.f;
	FLinearColor BaseColor = FLinearColor::Black;
	FLinearColor BaseDirectional = FLinearColor::Black;
	FLinearColor BaseSkyAmbient = FLinearColor::White;
	bool bHaveBase = false;
	/** Fog of the zone the camera was last in with distance fog. */
	float ZoneStart = 0.f, ZoneEnd = 0.f;
	FLinearColor ZoneColor = FLinearColor::Black;
	/** 0 = level fog, 1 = zone fog. */
	float Weight = 0.f;
	bool bWritten = false;
	bool bAnyFogZone = false;
	float NextZoneCheck = 0.f;
	TWeakObjectPtr<class AArenaZoneInfo> LastZone;
	/** LevelInfo distance fog (UE2 units converted to cm). */
	bool bLevelFog = false;
	float LevelFogStart = 0.f, LevelFogEnd = 0.f;
	FColor LevelFogColor = FColor(128, 128, 128);
};

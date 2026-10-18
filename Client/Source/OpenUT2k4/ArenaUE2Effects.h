#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaUE2World.h"
#include "ArenaUE2Effects.generated.h"

class AArenaEmitter;
class UArenaSpriteComponent;
class UPointLightComponent;
class UWorld;

/**
 * UT2004's own effect classes (XEffects.RedeemerExplosion, NewIonEffect, ONSGrenadeExplosionEffect ...) spawned at run
 * time from the stock script packages in the content cache (scripts/System/<Pkg>.u): the class's merged default
 * properties and its inline ParticleEmitter subobjects (SpriteEmitter, SparkEmitter, BeamEmitter, MeshEmitter) are read
 * with the converter's package reader (Convert/ArenaConvPackage.h), turned into the same description a map Emitter
 * gets (FArenaUE2ActorDesc: properties + "subobjects" + class defaults) and simulated by AArenaEmitter
 * (ArenaEmitters.h). Nothing is shipped: without the cache (or with arena.UE2Effects 0) every call returns null and
 * the caller keeps its own effect.
 *
 * Effect actors without an Emitter body are native here, following their script: AArenaIonEffect is
 * XEffects.IonEffect (IonCore falling from the cannon, the IonShaft and IonLightBeam xEmitter beams, then
 * NewIonEffect where it lands).
 */
namespace ArenaUE2Effects
{
	/** False without a content cache with script packages, or with arena.UE2Effects 0. */
	OPENUT2K4_API bool IsAvailable();
	/**
	 * The default object of a UE2 class ("XEffects.RedeemerExplosion") as an actor description: Props are the merged
	 * class defaults, Json holds the "subobjects" (emitters) and ClassDefaults the defaults of their classes. Cached.
	 */
	OPENUT2K4_API bool GetClassDesc(const FString& PkgClass, FArenaUE2ActorDesc& Out);
	/**
	 * Spawns an Emitter (or xEmitter) class at Location / Rotation (UE coordinates). Its life ends when its particles
	 * are done (UE2 AutoDestroy; at most MaxLife seconds), or after the class's LifeSpan. Null when unavailable.
	 */
	OPENUT2K4_API AArenaEmitter* SpawnEmitter(UWorld* World, const FString& PkgClass, const FVector& Location, const FRotator& Rotation, float MaxLife = 12.f);
	/** "Textures/<Pkg>/<Group>_<Name>" (ArenaArtFX sprite path) of a UE2 texture or of a material's base texture. */
	OPENUT2K4_API FString SpriteTexturePath(const FString& UE2Path);
	/** UE2 rotator units (65536 per turn) to degrees: Rot(0, 16384, 0) is FRotator(0, 90, 0). */
	inline FRotator UE2Rot(int32 Pitch, int32 Yaw, int32 Roll) { return FRotator(Pitch * 360.f / 65536.f, Yaw * 360.f / 65536.f, Roll * 360.f / 65536.f); }
	/**
	 * A one-particle sprite xEmitter class (XEffects.IonCore ...): its texture (Skins), mSizeRange, mColorRange, Style
	 * and LifeSpan, facing the camera, fading out when mAttenuate. Null when unavailable.
	 */
	OPENUT2K4_API AActor* SpawnSpriteXEmitter(UWorld* World, const FString& PkgClass, const FVector& Location);
	/** Loads the descriptions of every stock effect class the game uses (UArenaUE2EffectsSubsystem). */
	OPENUT2K4_API void Preload();
	/** Effects spawned since start (tests). */
	OPENUT2K4_API int32 GetSpawnCount(const FString& PkgClass);
}

/**
 * XEffects.IonEffect: the ion cannon's strike from the cannon (Location) to the mark (HitLocation). IonCore (shock_core
 * sprite) drops to the mark over DropTime (0.5 s) with the IonShaft beams following it from the top and the IonLightBeam
 * from the top to the mark; then the core goes, both beams fade (mAttenuate) and NewIonEffect is spawned 100 UU above
 * the mark. Blue dynamic light (LightHue 160, LightRadius 12) for its 2 s LifeSpan.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AArenaIonEffect : public AActor
{
	GENERATED_BODY()

public:
	AArenaIonEffect();
	/** IonEffect.AimAt(HitLocation, HitNormal) (UE coordinates). False when the stock classes are unavailable. */
	bool AimAt(const FVector& InHitLocation, const FVector& InHitNormal);
	virtual void Tick(float DeltaSeconds) override;
	bool HasLanded() const { return bLanded; }

	UPROPERTY() TObjectPtr<UPointLightComponent> Light;
	UPROPERTY() TObjectPtr<UArenaSpriteComponent> Core;
	UPROPERTY() TArray<TObjectPtr<UArenaSpriteComponent>> Shaft;
	UPROPERTY() TArray<TObjectPtr<UArenaSpriteComponent>> LightBeam;

protected:
	FVector Top = FVector::ZeroVector;
	FVector HitLocation = FVector::ZeroVector;
	FVector HitNormal = FVector::UpVector;
	float StartTime = 0.f;
	float DropTime = 1.f;
	bool bLanded = false;
	float LandTime = 0.f;
	float ShaftWidth = 140.f;
	float LightBeamWidth = 44.f;
	float FadeTime = 1.3f;
};

/**
 * Reads the stock effect classes the game spawns (Redeemer, ion cannon, ONS weapon and vehicle explosions) when a match
 * world starts, so the first blast does not stall the frame while their script packages are parsed.
 */
UCLASS()
class OPENUT2K4_API UArenaUE2EffectsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
};

/**
 * Test hook (non-shipping): -UE2FXTest=nuke|ion|both|warhead [-UE2FXTestTime=<s>] fires the real Redeemer blast (warhead: an
 * unguided warhead flying in, for the bots that shoot it down)
 * (AArenaWarheadShell::BlowUp) and / or ion strike (AArenaIonCannon) on the ground about 30 m in front of the local view
 * at that match time (default 8 s; the ion strike 6 s later with "both") and takes screenshots
 * (ue2fx_<kind>_<n>.png) through the effect. Logs "UE2FXTest: ..." with the spawned effect classes.
 */
UCLASS()
class OPENUT2K4_API UArenaUE2FXTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	FString Kind;
	float StartTime = 8.f;
	int32 Step = 0;
	float NextTime = 0.f;
	FVector Point = FVector::ZeroVector;
	int32 ShotIndex = 0;
	bool bIon = false;
	void Fire(bool bIonStrike);
	TMap<FName, FVector> RestPositions;
	float WalkYaw = 0.f;
	float MarkTime = 0.f;
};

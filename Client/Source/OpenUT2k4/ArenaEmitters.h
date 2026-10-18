#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "ArenaUE2World.h"
#include "ArenaEmitters.generated.h"

class UAudioComponent;
class UMaterialInterface;
class UStaticMesh;
class USoundBase;
class UTexture;

/**
 * UE2 particle systems, native (no Niagara): AArenaEmitter simulates the Emitter actor's ParticleEmitter
 * subobjects (SpriteEmitter, SparkEmitter, BeamEmitter, MeshEmitter as sprites / streaks / beams / meshes) from
 * their UE2 parameters (MaxParticles, LifetimeRange, StartLocation*, StartVelocity*, Acceleration, VelocityLoss,
 * ColorScale / SizeScale curves, FadeIn / FadeOut, SpinParticles, SubdivisionScale / TextureUSubdivisions,
 * DrawStyle, ParticlesPerSecond / RespawnDeadParticles / InitialParticlesPerSecond, AutoReset, AutomaticInitialSpawning,
 * Trigger, CoordinateSystem ...) and draws every emitter's particles as one dynamic mesh per emitter
 * (UArenaParticleComponent, camera facing quads built per view on the render thread) with the generated
 * /Game/Materials/M_ArenaParticle{Add,Alpha,Mod} materials (vertex colour x texture). xEmitter (the older UT2003
 * emitter) maps onto one sprite emitter.
 *
 * AArenaAmbientSound plays a UE2 actor's AmbientSound (AmbientSound actors and any actor with an ambient sound)
 * with UE2's SoundRadius / SoundVolume / SoundPitch.
 */

/** UE2 ParticleEmitter parameters (subset that matters for visuals), in UE coordinates (cm). */
struct OPENUT2K4_API FArenaParticleParams
{
	enum class EKind : uint8 { Sprite, Spark, Beam, Mesh };
	enum class EStyle : uint8 { Additive, Alpha, Modulate };
	EKind Kind = EKind::Sprite;
	EStyle Style = EStyle::Alpha;
	FString TexturePath;
	FString MeshPath;
	int32 MaxParticles = 10;
	bool bRespawnDeadParticles = true;
	bool bAutomaticInitialSpawning = true;
	float InitialParticlesPerSecond = 0.f;
	float ParticlesPerSecond = 0.f;
	bool bAutoDestroy = false;
	bool bAutoReset = false;
	FVector2D AutoResetTimeRange = FVector2D::ZeroVector;
	bool bTriggerDisabled = true;
	bool bResetOnTrigger = false;
	bool bDisabled = false;
	FVector2D LifetimeRange = FVector2D(4.f, 4.f);
	FVector2D InitialDelayRange = FVector2D::ZeroVector;
	/** Local space: relative to the emitter; else world. */
	bool bLocalSpace = false;
	/** 0 box, 1 sphere, 2 polar. */
	uint8 StartLocationShape = 0;
	FVector StartLocationOffset = FVector::ZeroVector;
	FVector StartLocationMin = FVector::ZeroVector, StartLocationMax = FVector::ZeroVector;
	FVector2D SphereRadiusRange = FVector2D::ZeroVector;
	FVector StartLocationPolarMin = FVector::ZeroVector, StartLocationPolarMax = FVector::ZeroVector;
	FVector StartVelocityMin = FVector::ZeroVector, StartVelocityMax = FVector::ZeroVector;
	FVector2D StartVelocityRadialRange = FVector2D::ZeroVector;
	bool bGetVelocityDirectionFrom = false;
	FVector Acceleration = FVector::ZeroVector;
	FVector VelocityLossMin = FVector::ZeroVector, VelocityLossMax = FVector::ZeroVector;
	FVector MaxAbsVelocity = FVector::ZeroVector;
	FVector StartSizeMin = FVector(100.f), StartSizeMax = FVector(100.f);
	bool bUniformSize = false;
	/** Curve points (RelativeTime 0..1). */
	TArray<TPair<float, FLinearColor>> ColorScale;
	TArray<TPair<float, FVector>> SizeScale;
	bool bUseColorScale = false;
	bool bUseSizeScale = false;
	bool bUseRegularSizeScale = true;
	float ColorScaleRepeats = 0.f;
	float SizeScaleRepeats = 0.f;
	FVector ColorMultiplierMin = FVector::OneVector, ColorMultiplierMax = FVector::OneVector;
	float Opacity = 1.f;
	bool bFadeOut = false, bFadeIn = false;
	float FadeOutStartTime = 0.f, FadeInEndTime = 0.f;
	FVector4f FadeOutFactor = FVector4f(1, 1, 1, 1), FadeInFactor = FVector4f(1, 1, 1, 1);
	bool bSpinParticles = false;
	FVector2D StartSpinRange = FVector2D::ZeroVector;
	FVector2D SpinsPerSecondRange = FVector2D::ZeroVector;
	float SpinCCWorCW = 0.5f;
	/** Sprite facing: 0 camera, 1 up (horizontal, normal Z), 2 along velocity (streak), 3 perpendicular to the camera along velocity. */
	uint8 Facing = 0;
	/** Texture sheet. */
	int32 TextureUSubdivisions = 1, TextureVSubdivisions = 1;
	bool bBlendBetweenSubdivisions = false;
	bool bUseRandomSubdivision = false;
	TArray<float> SubdivisionScale;
	int32 SubdivisionStart = 0, SubdivisionEnd = 0;
	/** Spark: line length from velocity * LineSegments ... */
	FVector2D TimeBetweenSegmentsRange = FVector2D(0.1f, 0.1f);
	/** Beam: end point offset / noise. */
	FVector BeamEndMin = FVector::ZeroVector, BeamEndMax = FVector::ZeroVector;
	FVector BeamNoiseMin = FVector::ZeroVector, BeamNoiseMax = FVector::ZeroVector;
	int32 BeamSegments = 1;
	float BeamTextureUScale = 1.f;
	/** Mesh: scale range is StartSize / 100 (UE2 mesh particles use StartSizeRange as a scale). */
	bool bUseMeshBlendMode = true;
	float DrawScale = 1.f;
	float SecondsBeforeInactive = 1.f;

	/** From UE2 properties (raw), Scale = UE2 unit -> cm, DrawScale = the Emitter actor's. */
	void Load(const FString& UE2Class, const TArray<FString>& Chain, const FArenaUE2Props& P, float InDrawScale);
	/** xEmitter (UT2003 style). */
	void LoadXEmitter(const FArenaUE2Props& P, const FString& Texture, float InDrawScale);
};

/** One particle (simulation state). */
struct FArenaParticle
{
	FVector Location = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	FVector VelocityLoss = FVector::ZeroVector;
	FVector StartSize = FVector(100.f);
	FLinearColor ColorMul = FLinearColor::White;
	float Age = 0.f;
	float Life = 1.f;
	float Spin = 0.f;
	float SpinRate = 0.f;
	int32 Subdivision = 0;
	FVector BeamEnd = FVector::ZeroVector;
	FVector PrevLocation = FVector::ZeroVector;
	bool bAlive = false;
};

/** Draws one emitter's particles (camera facing quads built per view). */
UCLASS()
class OPENUT2K4_API UArenaParticleComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UArenaParticleComponent();

	/** Render data of one particle (world space). */
	struct FDrawParticle
	{
		FVector3f Location;
		/** Velocity direction * length for streaks / beam end - start for beams. */
		FVector3f Axis;
		FVector2f Size;
		float Rotation = 0.f;
		FColor Color;
		FVector4f UV;
	};

	void SetParticleMaterial(UMaterialInterface* InMaterial);
	/** Replace the particles to draw (game thread; pushed to the render thread). */
	void SetParticles(TArray<FDrawParticle>&& InParticles, uint8 InFacing, const FBox& InBounds);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override { return Material; }
	virtual int32 GetNumMaterials() const override { return 1; }

	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Material;

private:
	FBox WorldBounds = FBox(ForceInit);
};

/** UE2 Emitter (and xEmitter). */
UCLASS()
class OPENUT2K4_API AArenaEmitter : public AActor, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaEmitter();

	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bAutoReset = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector2D TimeTillResetRange = FVector2D::ZeroVector;
	FString UE2Name;

	/** Adds a ParticleEmitter (before BeginPlay). */
	void AddEmitter(const FArenaParticleParams& Params);
	int32 NumEmitters() const { return Emitters.Num(); }
	int32 NumLiveParticles() const;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
	/** UE2: Trigger toggles emitters with TriggerDisabled = false (and resets those with ResetOnTrigger). */
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;

	/** Global quality: particle count scale and the distance beyond which emitters pause (arena.EmitterDistance). */
	static float GetMaxDistance();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	struct FEmitterState
	{
		FArenaParticleParams P;
		TArray<FArenaParticle> Particles;
		TObjectPtr<UArenaParticleComponent> Comp;
		TArray<TObjectPtr<UStaticMeshComponent>> MeshComps;
		float SpawnAccum = 0.f;
		float Delay = 0.f;
		int32 Spawned = 0;
		bool bActive = true;
		bool bFinished = false;
		bool bInitialBurstDone = false;
		/** Where its particles can be (world), so it counts as visible before it has particles. */
		FBox Reach = FBox(ForceInit);
	};
	TArray<FEmitterState> Emitters;
	UPROPERTY() TArray<TObjectPtr<UArenaParticleComponent>> ParticleComps;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> MeshParticleComps;
	float ResetTimer = -1.f;
	/** UE2 SecondsBeforeInactive: emitters not drawn for this long pause (simulation and mesh building). */
	float SecondsBeforeInactive = 1.f;
	float PausedTime = 0.f;
	/** True when one of the particle components was rendered recently (or the emitter just started). */
	bool IsRecentlyVisible() const;
	FRandomStream Rng;

	void SpawnParticle(FEmitterState& E, FArenaParticle& Pt);
	void Simulate(FEmitterState& E, float DeltaSeconds);
	void Draw(FEmitterState& E);
	void ResetEmitter(FEmitterState& E);
};

/** UE2 AmbientSound (and AmbientSound on other actors). */
UCLASS()
class OPENUT2K4_API AArenaAmbientSound : public AActor, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaAmbientSound();

	UPROPERTY(VisibleAnywhere) TObjectPtr<UAudioComponent> Audio;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> AmbientSound;
	/** UE2 units: audible radius = 25 * (SoundRadius + 1) UE2 units. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float SoundRadius = 64.f;
	/** 0..255 (128 = full UE2 ambient volume). */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 SoundVolume = 128;
	/** 64 = normal pitch. */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 SoundPitch = 64;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;

	/** Configure and start (Sound may be null: nothing plays). */
	void Setup(USoundBase* Sound, float InRadius, uint8 InVolume, uint8 InPitch);
	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
};

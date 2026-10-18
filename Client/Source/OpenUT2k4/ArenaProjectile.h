#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "Engine/TimerHandle.h"
#include "ArenaProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class UPrimitiveComponent;
class APawn;
class AController;
class USkeletalMeshComponent;
namespace ArenaArtFX { struct FProjectileArt; }

/**
 * Generic projectile configured entirely by FArenaProjectileParams.
 * Collision object type is ECC_Projectile; it blocks ECC_Weapon traces only when bComboCore.
 * Spawn with SpawnActorDeferred, call Init(), then FinishSpawning.
 */
UCLASS()
class OPENUT2K4_API AArenaProjectile : public AActor
{
	GENERATED_BODY()

public:
	AArenaProjectile();

	/** Must be called before FinishSpawning. Instigator/Owner should be set via spawn params. */
	void Init(const FArenaProjectileParams& InParams, AController* InInstigatorController, float InDamageScale);

	/** Explode at the current location (radial or direct damage as configured). */
	virtual void Explode(const FVector& HitLocation, const FVector& HitNormal, AActor* DirectHitActor);

	/** Pulse rifle combo: detonate with ComboDamage / ComboRadius. Returns true if this is a combo core. */
	virtual bool TriggerCombo(AController* ComboInstigator);

	const FArenaProjectileParams& GetParams() const { return Params; }
	/** Rocket flocking (RocketMultiFire tight spread): the other rockets of the volley and the curl direction. */
	void SetFlock(const TArray<AArenaProjectile*>& Mates, bool bInCurl);
	/** BioGlob: merge another glob's goop into this one (on the ground). */
	void BioMergeWithGlob(int32 AdditionalGoopLevel);
	bool IsBioOnGround() const { return Params.Kind == EArenaProjKind::BioGlob && bStuck; }
	int32 GetGoopLevel() const { return Params.GoopLevel; }
	/** Damage dealt to this projectile (shock core: combo for the shock beam; goop: detonates). */
	void TakeProjectileDamage(AController* DamageInstigator, bool bShockBeam);

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Collision;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UProjectileMovementComponent> Movement;
	/** Where the last projectile exploded and where its instigator was at that moment (-MoveTest checks). */
	static FVector LastExplodeLocation;
	static FVector LastExplodeInstigatorLocation;
	/** Seconds of flight simulated so far (sum of its ticks: game time, for the -MoveTest checks). */
	float GetFlightTime() const { return FlightTime; }
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Visual;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

protected:
	virtual void BeginPlay() override;
	virtual void LifeSpanExpired() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	FArenaProjectileParams Params;
	TWeakObjectPtr<AController> InstigatorController;
	float DamageScale = 1.f;
	int32 BounceCount = 0;
	bool bExploded = false;

	// ---- internals (ArenaProjectile.cpp) ----
	UFUNCTION() void OnCollisionHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);
	UFUNCTION() void OnCollisionOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	UFUNCTION() void OnProjectileBounced(const FHitResult& ImpactResult, const FVector& ImpactVelocity);
	UFUNCTION() void OnProjectileStopped(const FHitResult& ImpactResult);

	/** Stop ignoring the instigator pawn after the launch grace period. */
	void EndInstigatorGrace();
	/** True while the instigator pawn should be ignored. */
	bool IsInGrace(const AActor* Other) const;
	/** Sticky goo: settle on the surface and wait for a pawn. */
	void StickTo(const FHitResult& Hit);
	/** Destroy quietly (no damage) with a small fizzle effect. */
	void Fizzle(const FVector& Location, const FVector& Normal);

	// ---- UT2004 behaviours (EArenaProjKind) ----
	void TickBehaviour(float DeltaSeconds);
	void BioLanded(const FHitResult& Hit);
	void BioSetGoopLevel(int32 NewLevel);
	void BioSplashGlobs(int32 NumGloblings);
	void BioBlowUp();
	void BioDrip();
	void FlakShellExplode(const FVector& HitLocation, const FVector& HitNormal);
	/** Radius damage, effects and sound at Loc (no destroy). */
	void HurtRadiusAt(const FVector& Loc, const FVector& Normal, float Damage, float Radius, float Momentum, const FString& Name);
	int32 BouncesLeft = 0;
	bool bFuseSet = false;
	FTimerHandle KindTimer;
	FVector SurfaceNormal = FVector::UpVector;
	int32 BioRand3 = 0;
	float NextSteerTime = 0.f;
	FVector InitialDir = FVector::ForwardVector;
	TArray<TWeakObjectPtr<AArenaProjectile>> Flock;
	bool bCurl = false;
	float SpawnTime = 0.f;
	FVector SteerAccel = FVector::ZeroVector;
	float ArtBaseScale = 1.f;
public:
	/** Flak shell: the chunks it throws when it explodes (set by the weapon). */
	FArenaProjectileParams ChunkParams;
protected:

	TWeakObjectPtr<APawn> GracePawn;
	/** UE2 ProcessTouch ignores Instigator and Owner: a Shield Gun reflected copy ignores the original shooter and the
	 * reflecting pawn (radius damage still hits both). */
	TWeakObjectPtr<AActor> TouchIgnore[2];
	/** UE2 Shock/LinkProjectile.ProcessTouch: a raised Shield Gun sends a copy back along its holder's view. */
	bool TryShieldReflect(AActor* Other, const FVector& HitLocation);
public:
	/** Times this projectile (or its ancestors) was reflected by a Shield Gun. */
	int32 ReflectGeneration = 0;
protected:
	float GraceEndTime = 0.f;
	bool bStuck = false;
	FVector StuckNormal = FVector::UpVector;
	FTimerHandle GraceTimer;

	// ---- imported art (ArenaArtFX projectile table); the procedural Visual is hidden while it is shown ----
	virtual void Tick(float DeltaSeconds) override;
	void BuildArt();
	FName GetArtEffect(bool bExplosion) const;
	const ArenaArtFX::FProjectileArt* ArtDesc = nullptr;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> ArtSkel;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ArtMeshes;
	TArray<float> ArtMeshSpin;
	float FlightTime = 0.f;
};

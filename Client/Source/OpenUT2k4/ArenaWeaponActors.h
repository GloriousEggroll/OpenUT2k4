#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "ArenaWeaponActors.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;
class UPointLightComponent;
class UCameraComponent;
class UAudioComponent;
class AArenaCharacter;
class AArenaWeapon;

/**
 * Actors of the UT2004 super / Onslaught weapons (ArenaWeaponsExtra.h). Values are UT2004's in UE2 units (UU) where
 * noted, converted with 1 UU = 2 cm; momentum is the launch speed of a standard pawn (MomentumTransfer / 100 x 2 cm/s).
 */
namespace ArenaWeaponActors
{
	constexpr float UU = 2.f;
	/** UE2 PHYS_Falling gravity (-950 UU/s^2). */
	constexpr float FallGravity = -1900.f;
	/** UE2 team of an actor's instigator (255 = none). */
	OPENUT2K4_API int32 TeamOf(const AController* C);
	OPENUT2K4_API bool SameTeam(const UWorld* World, const AController* A, const AController* B);
}

/**
 * UE2 RedeemerProjectile / RedeemerWarhead "Dying" state code and IonCannon "FireSequence": HurtRadius(Damage,
 * Radius x fraction) at stage times, the rings growing outwards (the same damage at every stage).
 */
UCLASS()
class OPENUT2K4_API AArenaStagedBlast : public AActor
{
	GENERATED_BODY()
public:
	AArenaStagedBlast();
	struct FStage
	{
		float Time = 0.f;
		float RadiusFrac = 1.f;
	};
	/** Starts the sequence at Location (Damage at every stage; Radius and momentum in cm and cm/s). */
	void Start(const FVector& Location, float InDamage, float InRadius, float InMomentum, AController* InInstigator, const FString& InWeaponName,
		const TArray<FStage>& InStages, bool bInNuke);
	virtual void Tick(float DeltaSeconds) override;
	int32 GetStagesDone() const { return NextStage; }
	/** The stock effect classes show the blast (ArenaUE2Effects): no procedural rings. */
	void SetUE2Effect(bool b) { bUE2Effect = b; }
protected:
	TArray<FStage> Stages;
	int32 NextStage = 0;
	float StartTime = 0.f;
	float Damage = 0.f;
	float Radius = 0.f;
	float Momentum = 0.f;
	bool bNuke = false;
	bool bUE2Effect = false;
	TWeakObjectPtr<AController> InstigatorController;
	FString WeaponName;
};

/** UT2004 RedeemerProjectile: the unguided warhead (1000 UU/s, shootable, nukes on impact). */
UCLASS()
class OPENUT2K4_API AArenaWarheadShell : public AActor
{
	GENERATED_BODY()
public:
	AArenaWarheadShell();
	void Launch(AController* InInstigator, const FString& InWeaponName, const FVector& Dir);
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void Tick(float DeltaSeconds) override;
	/** RedeemerProjectile.BlowUp: the nuke (staged blast). */
	void BlowUp(const FVector& At);
	bool HasBlownUp() const { return bBlown; }
	/**
	 * The nuke's look and view shake (RedeemerProjectile / RedeemerWarhead BlowUp + Dying): XEffects.RedeemerExplosion
	 * 100 UU back along Velocity, IonCore, and ShakeView of every player within 2 x DamageRadius. True when the stock
	 * effect classes were spawned.
	 */
	static bool SpawnNukeEffects(UWorld* World, const FVector& At, const FVector& Velocity);
	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UPointLightComponent> Light;
	UPROPERTY() TObjectPtr<UAudioComponent> FlightSound;
	virtual FVector GetVelocity() const override { return Velocity; }
	/** RedeemerProjectile.Timer (every 0.5 s): enemy bots of skill 2+ with nothing else to shoot at fire at it. */
	void WarnBots();
	/** Bots told to shoot at it (tests). */
	int32 NumBotsWarned = 0;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
protected:
	UFUNCTION() void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	TWeakObjectPtr<AController> InstigatorController;
	TWeakObjectPtr<APawn> InstigatorPawn;
	FString WeaponName;
	FVector Velocity = FVector::ZeroVector;
	bool bBlown = false;
	float NextBotWarnTime = 0.f;
	/** XEffects.NewRedeemerTrail (PostBeginPlay, 40 UU behind, based on the warhead). */
	TWeakObjectPtr<AActor> SmokeTrail;
};

/** UT2004 RedeemerWarhead: the guided warhead, flown by its player (PlayerController.PlayerRocketing). */
UCLASS()
class OPENUT2K4_API AArenaGuidedWarhead : public APawn
{
	GENERATED_BODY()
public:
	AArenaGuidedWarhead();
	/** RedeemerGuidedFire.SpawnProjectile: the player leaves Old (it stays where it is) and flies this. */
	void Launch(AArenaCharacter* Old, AController* Flyer, const FString& InWeaponName);
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void BlowUp(const FVector& At);
	/** RedeemerWarhead.DrawHUD: the scope overlay; static noise after the explosion. */
	bool IsStaticScreen() const { return bStaticScreen; }
	/** Roll of the warhead (banking), degrees: the scope's outer ring turns with it. */
	float GetBankingDeg() const;
	/** Vertical speed (cm/s): the altitude strips pan with it. */
	float GetClimbRate() const { return VelUU.Z * ArenaWeaponActors::UU; }
	AArenaCharacter* GetOldPawn() const { return OldPawn.Get(); }
	/** Net play (Net/UE2NetPlay.h): the UT2004 server's RedeemerWarhead we possess, flown here like PlayerRocketing and
	 *  corrected by the server; it does no damage and has no blast of its own (the server's RedeemerExplosion). */
	void LaunchNet(AController* InFlyer, const FVector& InVelUU);
	/** Net play: the server's location and velocity (UU), after replaying our moves since. */
	void SetNetFlight(const FVector& LocUU, const FVector& InVelUU);
	/** Net play: the server blew it up (bStaticScreen): the static screen until it gives us our pawn back. */
	void NetBlownUp();
	bool IsNetFlight() const { return bNetFlight; }
	/** UE2 Acceleration / Velocity (UU). */
	const FVector& GetAccelUU() const { return AccelUU; }
	const FVector& GetVelUU() const { return VelUU; }
	/** PHYS_Flying's velocity step (UE2 CalcVelocity with the default volume's FluidFriction 0.3, AirSpeed 1000). */
	static void FlyVelocity(FVector& InOutVelUU, const FVector& InAccelUU, float DeltaTime);
	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UCameraComponent> Camera;
	UPROPERTY() TObjectPtr<UAudioComponent> FlightSound;
protected:
	/** RedeemerWarhead.UpdateRocketAcceleration (UE2 units: aTurn / aLookUp, rotator units). */
	void UpdateRocketAcceleration(float DeltaTime, float YawChange, float PitchChange);
	/** RedeemerWarhead.RelinquishController: back to the old pawn, or no pawn (respawn) when it died. */
	void RelinquishController();
	TWeakObjectPtr<AArenaCharacter> OldPawn;
	TWeakObjectPtr<AController> Flyer;
	FString WeaponName;
	/** UE2 Velocity / Acceleration in UU. */
	FVector VelUU = FVector::ZeroVector;
	FVector AccelUU = FVector::ZeroVector;
	float YawAccel = 0.f;
	float PitchAccel = 0.f;
	float Banking = 0.f;
	float BankingVelocity = 0.f;
	FRotator LastSetRotation = FRotator::ZeroRotator;
	bool bFireWasDown = true;
	bool bBlown = false;
	bool bNetFlight = false;
	bool bStaticScreen = false;
	float RelinquishTime = -1.f;
	bool bRelinquished = false;
	/** XEffects.RedeemerTrail (RedeemerWarhead.PostBeginPlay). */
	TWeakObjectPtr<AActor> SmokeTrail;
};

/** UT2004 IonCannon: one per level, 2000 UU above where it was first needed; fires its FireSequence at a painted mark. */
UCLASS()
class OPENUT2K4_API AArenaIonCannon : public AActor
{
	GENERATED_BODY()
public:
	AArenaIonCannon();
	/** Painter.CheckMark: the level's cannon (spawned at Near + 2000 UU). */
	static AArenaIonCannon* FindOrSpawn(UWorld* World, const FVector& Near);
	/** IonCannon.Ready.CheckMark: the sky above Mark is open up to the cannon; with bFire the strike starts. */
	bool CheckMark(AController* Aimer, const FVector& Mark, bool bFire, const FString& InWeaponName);
	bool IsFiring() const { return bFiring; }
	virtual void Tick(float DeltaSeconds) override;
	int32 GetStrikes() const { return Strikes; }
protected:
	/** FireSequence.SpawnEffect: the IonEffects (AArenaIonEffect) from the sky to the mark. */
	void SpawnEffect();
	bool bUE2Effect = false;
	bool bFiring = false;
	float FireStart = 0.f;
	int32 Stage = 0;
	FVector MarkLocation = FVector::ZeroVector;
	TWeakObjectPtr<AController> Aimer;
	FString WeaponName;
	int32 Strikes = 0;
};

/** UT2004 ONSAVRiLRocket: accelerating, homes while the launcher keeps its lock (vehicles). */
UCLASS()
class OPENUT2K4_API AArenaSeekerRocket : public AActor
{
	GENERATED_BODY()
public:
	AArenaSeekerRocket();
	void Launch(class AWeap_SeekerLauncher* InLauncher, AController* InInstigator, const FString& InWeaponName, const FVector& Dir);
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	void Explode(const FVector& At);
	FVector GetVelocityCm() const { return VelUU * ArenaWeaponActors::UU; }
	/** Seconds of flight integrated so far (tests: the speed curve is 550 + 375 t^2 UU/s). */
	float GetFlightTime() const { return FlightTime; }
	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UPointLightComponent> Light;
protected:
	/** ONSAVRiLRocket.Timer (every 0.1 s): steering towards the launcher's HomingTarget. */
	void Home();
	TWeakObjectPtr<class AWeap_SeekerLauncher> Launcher;
	TWeakObjectPtr<AController> InstigatorController;
	TWeakObjectPtr<APawn> InstigatorPawn;
	FString WeaponName;
	FVector VelUU = FVector::ZeroVector;
	FVector AccelUU = FVector::ZeroVector;
	bool bAccelerating = true;
	float NextHome = 0.f;
	float LeadTargetStartTime = 0.f;
	bool bExploded = false;
	float FlightTime = 0.f;
};

/** UT2004 ONSGrenadeProjectile: bounces, sticks to players and vehicles, waits for the launcher's detonation. */
UCLASS()
class OPENUT2K4_API AArenaStickyGrenade : public AActor
{
	GENERATED_BODY()
public:
	AArenaStickyGrenade();
	void Launch(class AWeap_StickyLauncher* InLauncher, AController* InInstigator, const FString& InWeaponName, const FVector& Dir, float SpeedCm);
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void Explode();
	bool IsStuck() const { return StuckTo.IsValid(); }
	AActor* GetStuckTo() const { return StuckTo.Get(); }
	bool IsResting() const { return bResting; }
	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UPointLightComponent> Beacon;
protected:
	void Stick(AActor* Other);
	void UnStick();
	TWeakObjectPtr<class AWeap_StickyLauncher> Launcher;
	TWeakObjectPtr<AController> InstigatorController;
	TWeakObjectPtr<APawn> InstigatorPawn;
	FString WeaponName;
	FVector Velocity = FVector::ZeroVector;
	TWeakObjectPtr<AActor> StuckTo;
	TWeakObjectPtr<AActor> IgnoreActor;
	bool bResting = false;
	bool bExploded = false;
};

/** UT2004 ONSMineProjectile: a spider mine that lands, waits, hunts enemies within 750 UU and leaps at them. */
UCLASS()
class OPENUT2K4_API AArenaCrawlerMine : public AActor
{
	GENERATED_BODY()
public:
	AArenaCrawlerMine();
	enum class EState : uint8 { Flying, OnGround, Scurrying, ScurryToTargetLoc };
	void Launch(class AWeap_CrawlerLayer* InLayer, AController* InInstigator, const FString& InWeaponName, const FVector& VelocityCm);
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** ONSMineLayerAltFire: walk to the laser spot (+ up to TargetLocFuzz 250 UU), then look for targets there. */
	void SetScurryTarget(const FVector& Location);
	void BlowUp();
	EState GetState() const { return State; }
	AActor* GetTargetPawn() const { return TargetPawn.Get(); }
	/** Where the last mine blew up (tests: the damage falloff is measured from there). */
	static FVector LastBlowUpLocation;
	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Body;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Fallback;
protected:
	UFUNCTION() void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	void GotoState(EState NewState);
	/** ONSMineProjectile.AcquireTarget: the nearest visible enemy pawn within 750 UU. */
	void AcquireTarget();
	void PlayAnim(const TCHAR* Name, bool bLoop, float Rate = 1.f);
	/** One walking step: horizontal velocity, follow the floor; false when there is no floor (falls). */
	bool WalkStep(float DeltaSeconds);
	TWeakObjectPtr<class AWeap_CrawlerLayer> Layer;
	TWeakObjectPtr<AController> InstigatorController;
	TWeakObjectPtr<APawn> InstigatorPawn;
	FString WeaponName;
	int32 TeamNum = 255;
	EState State = EState::Flying;
	FVector Velocity = FVector::ZeroVector;
	bool bBounce = true;
	TWeakObjectPtr<AActor> TargetPawn;
	FVector TargetLoc = FVector::ZeroVector;
	float NextTimer = 0.f;
	float StateTime = 0.f;
	bool bExploded = false;
};

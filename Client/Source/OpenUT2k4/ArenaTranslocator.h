#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaWeapon.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaTranslocator.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class UAudioComponent;
class UArenaSpriteComponent;
class AArenaCharacter;
class AArenaTransBeacon;
class AArenaBotController;
struct FArenaDraw;

/**
 * The Translocator: UT2004's XWeapons.TransLauncher (TransFire / TransRecall) and its beacon (TransBeacon, RedBeacon /
 * BlueBeacon), with the stock class defaults and script logic (read from the player's XWeapons.u / UnrealGame.u).
 * UE2 units are converted with 1 UU = 2 cm.
 *
 * - Fire throws the beacon at 1200 UU/s along the view (no inherited velocity); it falls, bounces off walls and pawns
 *   with Velocity = 0.3 x the mirrored velocity and comes to rest under 20 UU/s on a floor (HitWall).
 * - Fire again recalls it; alt fire translocates to it (TransRecall.Translocate), dropping a carried flag or ball,
 *   telefragging enemies at the destination (gibbed: XWeapons.DamTypeTeleFrag) and failing on team mates and
 *   spawn-protected players. Translocating to a disrupted beacon gibs the owner instead (gibbedBy the disruptor).
 * - Charges: AmmoChargeF 6 of AmmoChargeMax 6, regaining AmmoChargeRate 0.4 per second; a throw needs 1 (it is not
 *   used up), a successful translocation uses 1. Bombing Run's bBallDrainsTransloc empties it when the ball is shot.
 * - Damage disrupts the beacon (enemies only in team games): above DisruptionThreshold 65 it sparks and kills
 *   whoever translocates to it.
 * - Pressing the Translocator key again while it is held looks through the beacon's camera (TransLauncher.Reselect:
 *   ViewCamera / ViewPlayer); firing, recalling or putting the weapon down returns the view.
 */
namespace ArenaTranslocator
{
	/** TransLauncher / TransFire / TransRecall / TransBeacon defaults (UE2 units where noted). */
	constexpr float AmmoChargeMax = 6.f;
	constexpr float AmmoChargeRate = 0.4f;
	constexpr float FireRate = 0.25f;
	constexpr float BeaconSpeedUU = 1200.f;
	constexpr float BeaconRadiusUU = 10.f;
	constexpr float DisruptionThreshold = 65.f;
	constexpr float MaxCamDistUU = 4000.f;
	/** TransFire.ProjSpawnOffset (forward, right, up). */
	const FVector ProjSpawnOffsetUU(25.f, 8.f, -10.f);
	/** UE2 Bot.TranslocUse of an average character. */
	constexpr float BotTranslocUse = 1.f;

	/** Weapon names used for the death messages (ArenaLocalMessages maps them to the stock damage types). */
	OPENUT2K4_API const FString& TelefraggedName();
	OPENUT2K4_API const FString& GibbedName();

	/** The beacon camera overlay (TransLauncher.RenderOverlays) when the local player looks through a beacon. */
	OPENUT2K4_API void DrawCameraOverlay(const FArenaDraw& D, const APlayerController* PC);

	/** UE2 Controller.AdjustToss: the throw velocity (cm/s) from Start that lands on End (horizontal speed TSpeed). */
	OPENUT2K4_API FVector AdjustToss(float TSpeedCm, const FVector& Start, const FVector& End, float GravityZ, bool bNormalize);

	/** Gravity (cm/s^2, negative) of the physics volume at a point (UE2 PhysicsVolume.Gravity.Z on our scale). */
	OPENUT2K4_API float GravityZAt(const UWorld* World, const FVector& Location);

	/** Counters for the -MoveTest checks and the bot match logs. */
	struct FStats
	{
		int32 Throws = 0;
		int32 Translocations = 0;
		int32 BotTranslocations = 0;
		int32 BotThrows = 0;
		int32 Telefrags = 0;
		int32 Gibs = 0;
		int32 Failed = 0;
		int32 Overlays = 0;
		int32 BeaconsShotAt = 0;
	};
	OPENUT2K4_API FStats& Stats();
}

/** UT2004 TransLauncher. */
UCLASS()
class OPENUT2K4_API AWeap_Translocator : public AArenaWeapon
{
	GENERATED_BODY()

public:
	AWeap_Translocator();

	// ---- AArenaWeapon ----
	virtual void GiveTo(AArenaCharacter* NewOwner) override;
	virtual void Equip() override;
	virtual void Unequip() override;
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	virtual bool HasAmmo() const override { return true; }
	virtual bool HasAmmoFor(uint8 Mode) const override { return true; }
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override { return 0; }
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- TransLauncher ----
	/** AmmoChargeF: whole charges plus the one recharging. */
	float GetCharge() const { return AmmoChargeF; }
	void SetCharge(float InCharge);
	/** TransLauncher.ChargeBar: the recharging part (0 when full). */
	float GetChargeBar() const { return AmmoChargeF - FMath::FloorToFloat(AmmoChargeF); }
	AArenaTransBeacon* GetBeacon() const { return Beacon.Get(); }
	/** TransFire: throw (TransFire.SpawnProjectile with no beacon out) or recall. Returns true if something happened. */
	bool ThrowOrRecall();
	/** TransRecall.ModeDoFire: translocate to the beacon (or gib on a disrupted one). True when the owner moved. */
	bool Translocate();
	/** TransLauncher.DrainCharges (BallShoot with bBallDrainsTransloc). */
	void DrainCharges();
	/** TransLauncher.Reselect: the translocator key pressed while held: look through the beacon / back. */
	void Reselect();
	void ViewCamera();
	void ViewPlayer();
	bool IsViewingBeacon() const;
	/** The weapon in use before the translocator (bPrevWeaponSwitch). */
	AArenaWeapon* GetPreviousWeapon() const { return PrevWeapon.Get(); }

	// ---- bots (TransLauncher.FireHack / BotFire; bots never press the buttons) ----
	/** FireHack(0): throw a beacon from the pawn at TargetLoc (AdjustToss), monitored for Target. */
	AArenaTransBeacon* BotThrow(const FVector& TargetLoc, AActor* MonitorTarget, const FVector& MonitorLoc, float MonitorRadius, float MonitorHeight,
		bool bGameObjectTarget);
	/** Recall without any sound (FireHack destroying an old beacon). */
	void DestroyBeacon();
	/** Brought up and past its refire time (TransLauncher.Timer runs FireHack after the bring-up). */
	bool IsReadyForBotThrow() const { return bEquipped && GetWorldTime() >= NextFireTime; }
	/** A bot translocation in progress set this (TranslocationTarget / bPreparingMove) so GetAIRating rates it 10. */
	bool bBotPreparing = false;

	/** Bot counters for the logs. */
	int32 NumTranslocations = 0;

protected:
	virtual void BuildViewModel() override;
	void ReduceAmmo();
	/** UE2 WeaponFire.ModeDoFire timing: NextFireTime += FireRate. */
	bool ReadyToFire(uint8 Mode) const;
	/** TransRecall.Translocate body: the destination, the pawns there and the move. */
	bool DoTranslocate(AArenaTransBeacon* B);
	void ApplyTeamSkin();
	/** UE2 Pawn.gibbedBy(Other) for Victim. */
	static void GibbedBy(AArenaCharacter* Victim, AArenaCharacter* Other);

	float AmmoChargeF = ArenaTranslocator::AmmoChargeMax;
	bool bDrained = false;
	TWeakObjectPtr<AArenaTransBeacon> Beacon;
	TWeakObjectPtr<AArenaWeapon> PrevWeapon;
	bool bPendingFire[2] = { false, false };
	int32 AppliedSkinTeam = -100;
	UPROPERTY() TObjectPtr<UMaterialInterface> BlueSkin;
	UPROPERTY() TObjectPtr<UMaterialInterface> RedSkin;
	TWeakObjectPtr<USkeletalMeshComponent> SkinnedThirdPerson;
};

/** UT2004 TransBeacon (RedBeacon / BlueBeacon). */
UCLASS()
class OPENUT2K4_API AArenaTransBeacon : public AActor
{
	GENERATED_BODY()

public:
	AArenaTransBeacon();

	/** TransBeacon.PostBeginPlay: velocity Speed along Dir (AimUp off), yaw only. */
	void Launch(AWeap_Translocator* InLauncher, AArenaCharacter* InOwner, const FVector& Dir, const FVector& InVelocity = FVector::ZeroVector);

	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;

	bool Disrupted() const { return Disruption > ArenaTranslocator::DisruptionThreshold; }
	float GetDisruption() const { return Disruption; }
	AArenaCharacter* GetDisruptor() const { return Disruptor.Get(); }
	/** PHYS_None: lying still. */
	bool IsResting() const { return bResting; }
	FVector GetVelocity() const { return Velocity; }
	AArenaCharacter* GetBeaconOwner() const { return OwnerPawn.Get(); }
	int32 GetTeam() const { return Team; }
	int32 NumBounces = 0;
	/** Where / when / how fast it was thrown, and the world time its flight was simulated up to (tests). */
	FVector LaunchLocation = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;
	float LaunchTime = 0.f;
	float LastMoveTime = 0.f;

	/** TransBeacon.SetTranslocationTarget: MonitoringThrow for a bot (target location, radius and height in cm). */
	void SetTranslocationTarget(AActor* Target, const FVector& TargetLoc, float Radius, float HalfHeight, bool bGameObject, bool bPreparingMove);
	bool IsMonitoring() const { return bMonitoring; }
	/** Set by FireHack before destroying an old beacon: no bot callbacks. */
	bool bNoAI = false;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Collision;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	UPROPERTY() TObjectPtr<UAudioComponent> Ambient;

	/** Ambient volume while its owner looks through it (TransLauncher.ViewBeaconVolume 40 of 255). */
	void SetCameraView(bool bViewing);

protected:
	/** TransBeacon.HitWall. */
	void HitWall(const FVector& HitNormal, AActor* Wall, bool bWorld);
	/** TransBeacon.ProcessTouch with a pawn. */
	void TouchPawn(APawn* Other);
	/** MonitoringThrow.Tick / HitWall / Touch. */
	void MonitorTick();
	void MonitorHitWall(const FVector& HitNormal);
	void EndMonitoring(bool bFailedMove, float FearCost);
	void BotTranslocate();
	void SpawnTrailPuff();
	void UpdateFlare(float DeltaSeconds);

	TWeakObjectPtr<AWeap_Translocator> Launcher;
	TWeakObjectPtr<AArenaCharacter> OwnerPawn;
	TWeakObjectPtr<AController> OwnerController;
	TWeakObjectPtr<AArenaCharacter> Disruptor;
	int32 Team = 255;
	FVector Velocity = FVector::ZeroVector;
	bool bResting = false;
	bool bCanHitOwner = false;
	float Disruption = 0.f;
	float NextSparkCheck = 0.f;
	bool bSparks = false;
	TSet<TWeakObjectPtr<AActor>> TouchingPawns;

	// MonitoringThrow (bots)
	bool bMonitoring = false;
	TWeakObjectPtr<AActor> MonitorActor;
	FVector MonitorLoc = FVector::ZeroVector;
	float MonitorRadius = 80.f;
	float MonitorHeight = 86.f;
	bool bMonitorGameObject = false;
	bool bMonitorPreparing = false;

	// visuals
	float NextPuffTime = 0.f;
	UPROPERTY() TArray<TObjectPtr<UArenaSpriteComponent>> Puffs;
	int32 NextPuff = 0;
	UPROPERTY() TObjectPtr<UArenaSpriteComponent> Flare;
	UPROPERTY() TObjectPtr<UArenaSpriteComponent> SparkGlow;
	FLinearColor TeamColor = FLinearColor(0.31f, 0.59f, 1.f);
};

/** Short translocation effects (xPawn TransEffects / TransOutEffect): a team coloured flash and ring. */
UCLASS()
class OPENUT2K4_API AArenaTransEffect : public AActor
{
	GENERATED_BODY()
public:
	AArenaTransEffect();
	static void Spawn(UWorld* World, const FVector& Location, int32 Team, bool bOut);
};

/**
 * Test hook (non-shipping): -TranslocShots once the match runs: the local player takes the translocator and screenshots
 * are taken of it in first person, a thrown and a resting beacon, the beacon camera, third person and a translocation
 * (Saved/Screenshots/<platform>/transloc_*.png), then the game quits.
 */
UCLASS()
class OPENUT2K4_API UArenaTranslocShotSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
private:
	float Start = -1.f;
	int32 Step = 0;
};

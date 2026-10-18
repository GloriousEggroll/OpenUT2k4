#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AIController.h"
#include "ArenaTypes.h"
#include "ArenaAS_Turret.generated.h"

class UStaticMeshComponent;
class USkeletalMeshComponent;
class UCapsuleComponent;
class AArenaCharacter;
class USoundBase;

/**
 * One Assault turret as UT2004 places them with its ASVehicleFactory_* actors (UArenaRules_AS reads the map and the
 * stock class defaults through the UScript loader, ArenaAS_Turret.cpp): sentinels (ASVehicle_Sentinel_Floor /
 * _Ceiling: automatic), the minigun, link, energy (ball) turrets and the ion cannon (manned; the link and energy
 * turrets also fire on their own while empty: UE2 bAutoTurret). Numbers come from the stock classes: health, fire
 * rate, projectile speed / damage / splash, instant hit range, pitch limits, model parts.
 */
struct FArenaASTurretDesc
{
	/** Short UT2004 class name ("ASTurret_Minigun") and the name players see ("Minigun Turret"). */
	FString ClassName;
	FString DisplayName;
	int32 Team = 1;
	float Health = 500.f;
	/** Fires at enemies by itself while nobody mans it (sentinels, bAutoTurret turrets). */
	bool bAuto = false;
	/** Players and bots can man it (UE2 bNonHumanControl false). */
	bool bManned = true;
	/** Model: the gun (skeletal), the base (static) and the swivel (static or skeletal), with their DrawScales. */
	FString GunMesh;
	FString BaseMesh;
	bool bBaseSkeletal = false;
	float BaseScale = 1.f;
	/** UE2 PrePivot of the base / swivel (cm). */
	FVector BaseOffset = FVector::ZeroVector;
	FVector SwivelOffset = FVector::ZeroVector;
	/** Sentinels (UE2 ASVehicle_Sentinel): closed until an enemy shows up, then Open / IdleOpen / Fire, Close again
	 *  a while after the last one (the gun, base and swivel meshes' own animations), with OpenCloseSound. */
	bool bDeploys = false;
	FString OpenCloseSound;
	FString SwivelMesh;
	bool bSwivelSkeletal = false;
	float SwivelScale = 1.f;
	// weapon (primary fire mode)
	float FireInterval = 0.25f;
	bool bHitscan = false;
	float Damage = 20.f;
	float DamageRadius = 0.f;
	float Momentum = 0.f;
	float ProjSpeed = 8000.f;
	float ProjLifeSpan = 3.f;
	float Range = 20000.f;
	/** Random spread (radians, UE2 Spread). */
	float Spread = 0.f;
	/** Muzzle relative to the gun (cm). */
	FVector MuzzleOffset = FVector(200.f, 0.f, 0.f);
	FLinearColor Color = FLinearColor(0.3f, 0.6f, 1.f);
	FString FireSound;
	/** The stock damage type of its shots ("UT2k4Assault.DamTypeSentinelLaser": the kill message text). */
	FString DamageType;
	/** Aim limits (degrees) and turn rate (deg / s). */
	float PitchUp = 99.f;
	float PitchDown = -90.f;
	float TurnRate = 110.f;
	/** Bots / automatic aim error (cm at the target, UE2 aimerror / 2). */
	float AimError = 150.f;
	/** Seen enemies within this range are attacked (cm). */
	float SightRange = 9000.f;
	/** Driver view: camera offset relative to the gun (cm). */
	FVector CameraOffset = FVector(-50.f, 0.f, 180.f);
	/** UE2 EntryRadius (cm). */
	float EntryRadius = 240.f;
};

/**
 * The controller of an automatic turret (UE2 TurretController / ASSentinelController): the instigator of its shots, so
 * they follow the team rules (UArenaGameRules::AreOnSameTeam, ArenaONS::TeamOfController) and kills are its own.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AArenaAS_TurretController : public AAIController
{
	GENERATED_BODY()

public:
	int32 Team = 1;
	/** The turret it fires for (its name in kill messages). */
	TWeakObjectPtr<class AArenaAS_Turret> Turret;

	/** The team of a turret controller, or ArenaTeam::None for any other controller. */
	static int32 TeamOf(const AController* C);
	/**
	 * The console kill line for a kill by an automatic turret, as UT2004 words it: its damage type's DeathString
	 * ("%o was served an extra helping of %k's lasers.") with the turret's name as the killer. Empty when C is not a
	 * turret controller. OutKillerName: "Floor Sentinel".
	 */
	static FString KillText(const AController* C, const FString& Victim, FString& OutKillerName);
};

UCLASS()
class OPENUT2K4_API AArenaAS_Turret : public APawn
{
	GENERATED_BODY()

public:
	AArenaAS_Turret();

	/** Server: before FinishSpawning. */
	void InitTurret(const FArenaASTurretDesc& InDesc);
	const FArenaASTurretDesc& GetTurretDesc() const { return Desc; }
	int32 GetTeam() const { return Team; }
	void SetTeam(int32 InTeam);
	bool IsDestroyedTurret() const { return bWrecked; }
	float GetHealth() const { return Health; }
	float GetHealthFraction() const { return Desc.Health > 0.f ? FMath::Clamp(Health / Desc.Health, 0.f, 1.f) : 0.f; }
	/** Turned off (UE2 factories disabled by the map's scripts / a passed stage). */
	void SetActiveTurret(bool bInActive) { bActiveTurret = bInActive; }

	// ---- manning ----
	bool CanBeEnteredBy(const AArenaCharacter* Char) const;
	bool IsManned() const { return Gunner.IsValid(); }
	AArenaCharacter* GetGunner() const { return Gunner.Get(); }
	bool EnterTurret(AArenaCharacter* Char);
	/** The gunner gets out (bForced: the turret was destroyed / reset). */
	AArenaCharacter* LeaveTurret(bool bForced);

	/** Round reset: full health, empty, back in place. */
	void ResetTurret();
	/**
	 * A DestroyVehicleObjective's vehicle (AS-MotherShip's sentinels): damage to the turret is the objective's
	 * (ApplyAttackDamage), and the turret is wrecked when the objective is completed.
	 */
	void SetLinkedObjective(class AArenaAS_Objective* InObjective);

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;
	virtual FRotator GetViewRotation() const override;
	virtual FVector GetPawnViewLocation() const override;

	/** Statistics for the test log: shots fired by all turrets, hits on pawns, kills. */
	static int32 NumShots;
	static int32 NumDestroyed;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void BuildModel();
	/** Picks the nearest seen enemy (characters, vehicles) within the sight range. */
	AActor* FindTarget() const;
	bool CanSee(const AActor* Target, FVector& OutAimPoint) const;
	void AimAt(const FRotator& Desired, float DeltaSeconds);
	void Fire(AController* InstigatorController);
	FVector GetMuzzle() const;
	void Wreck(AController* Killer);

	FArenaASTurretDesc Desc;
	int32 Team = 1;
	float Health = 500.f;
	bool bWrecked = false;
	bool bActiveTurret = true;
	FRotator AimRotation = FRotator::ZeroRotator;
	FRotator RestRotation = FRotator::ZeroRotator;
	float NextFireTime = 0.f;
	float NextTargetTime = 0.f;
	TWeakObjectPtr<AActor> Target;
	FVector TargetAim = FVector::ZeroVector;
	TWeakObjectPtr<AArenaCharacter> Gunner;
	TMap<TWeakObjectPtr<AActor>, bool> GunnerHidden;
	float EnterTime = 0.f;
	FTransform HomeTransform;
	TWeakObjectPtr<class AArenaAS_Objective> LinkedObjective;

	UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Solid;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> BaseComp;
	UPROPERTY() TObjectPtr<USceneComponent> SwivelRoot;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> SwivelStatic;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> SwivelSkel;
	UPROPERTY() TObjectPtr<USceneComponent> GunRoot;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> GunComp;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> StandIn;
	UPROPERTY() TObjectPtr<USoundBase> FireSoundAsset;
	UPROPERTY() TObjectPtr<USoundBase> OpenCloseSoundAsset;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> BaseSkel;
	UPROPERTY() TObjectPtr<AArenaAS_TurretController> AutoController;

	// sentinel deployment: 0 closed, 1 opening, 2 open, 3 closing
	int32 DeployState = 0;
	float DeployStateTime = 0.f;
	float DeployEndTime = 0.f;
	float LastTargetTime = -100.f;
	void SetDeployState(int32 NewState);
	/** Plays a sequence of the turret's meshes' own animation sets ("Open", "IdleOpen" ...) on gun, base and swivel. */
	float PlayTurretAnim(const TCHAR* Seq, bool bLoop);
};

namespace ArenaASTurret
{
	/**
	 * The turret a UT2004 Assault vehicle factory places ("ASVehicleFactory_SentinelFloor" ...; VehicleClass /
	 * VehicleClassStr resolved through the UScript loader), from the stock class defaults. False for factories of
	 * other vehicles (ONS vehicles: UArenaRules_AS spawns those as our vehicles) or when the classes are missing.
	 */
	OPENUT2K4_API bool DescribeFactory(const FString& FactoryClass, const FString& VehicleClassOverride, FArenaASTurretDesc& Out);
	/** The stock kill line of the turret's damage type, the turret named as the killer ("%o was killed by %k." without one). */
	OPENUT2K4_API FString KillText(const FArenaASTurretDesc& Desc, const FString& Victim);
}

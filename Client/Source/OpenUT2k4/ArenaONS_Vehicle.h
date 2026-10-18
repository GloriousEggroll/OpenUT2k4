#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "ArenaTypes.h"
#include "ArenaArtVehicles.h"
#include "ArenaONS_Vehicle.generated.h"

class UBoxComponent;
class USceneComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class USpringArmComponent;
class UCameraComponent;
class UMaterialInstanceDynamic;
class AArenaCharacter;
class AArenaONS_VehicleSeat;
class AArenaONS_VehicleFactory;
class AArenaONS_PowerNode;
class AArenaNavGraph;

/** The Onslaught vehicle roster (original designs). */
UENUM()
enum class EArenaONS_VehicleType : uint8
{
	Wisp,        // light hover scout
	Dustrunner,  // 4-wheel buggy
	Bulwark,     // heavy tank with a turret
	Kestrel,     // flying gunship
	Caravan,     // multi-seat transport with a passenger turret
	Num UMETA(Hidden)
};

/** How a vehicle moves. */
enum class EArenaONS_DriveMode : uint8
{
	Wheeled,  // raycast suspension, steered wheels, lateral grip
	Tracked,  // raycast suspension, pivot steering
	Hover,    // hover thrusters, yaw follows the view, strafing
	Flyer     // anti-gravity lift, free 3D movement
};

/** One vehicle gun (static tuning data). */
struct FArenaONS_GunSpec
{
	FString Name;
	/** 0 = driver, 1 = passenger seat. */
	int32 Seat = 0;
	/** 0 = primary fire, 1 = alt fire. */
	uint8 FireMode = 0;
	bool bHitscan = false;
	/** Alt "gun" that is a speed boost instead of a weapon. */
	bool bBoost = false;
	/** Fires along the turret barrel (tank / passenger turret) instead of the hull. */
	bool bTurret = false;
	float RefireTime = 0.3f;
	/** Heat added per shot (1 = overheated). */
	float HeatPerShot = 0.1f;
	/** Heat lost per second. */
	float CoolRate = 0.35f;
	/** Hitscan */
	float Damage = 20.f;
	float Range = 9000.f;
	float Spread = 0.f;
	float Momentum = 0.f;
	float BeamWidth = 3.f;
	/** Projectile */
	FArenaProjectileParams Projectile;
	/** Muzzles (hull or turret-barrel local space); shots alternate between them. */
	TArray<FVector> Muzzles;
	/** Max angle (deg) between the hull forward and the shot direction for fixed guns. */
	float AimCone = 30.f;
	FLinearColor Color = FLinearColor(0.3f, 0.8f, 1.f);
	FName Sound = NAME_None;
	/** UE2 ONSWeapon.ShakeRotMag ... of the matching UT2004 gun (ONSWeapon.ShakeView on every shot of its gunner). */
	FArenaViewShake Shake;
};

/** Static description of a vehicle type. */
struct FArenaONS_VehicleSpec
{
	FString Name;
	FString Role;
	EArenaONS_DriveMode Mode = EArenaONS_DriveMode::Wheeled;
	float Mass = 700.f;
	/** Collision box half extents. */
	FVector HalfExtent = FVector(150.f, 85.f, 30.f);
	float MaxHealth = 300.f;
	/** Suspension / thruster ray origins (body local) */
	TArray<FVector> SuspensionPoints;
	float RestLength = 40.f;
	float WheelRadius = 35.f;
	/** Rest compression as a fraction of RestLength. */
	float RestCompression = 0.45f;
	float DampingRatio = 0.55f;
	/** Total gravity multiplier (world gravity * this). */
	float GravityScale = 1.5f;
	float MaxSpeed = 2000.f;
	float ReverseSpeed = 800.f;
	float Accel = 1500.f;
	float Brake = 2800.f;
	float LateralGrip = 7.f;
	/** deg / s */
	float YawRate = 100.f;
	float JumpSpeed = 0.f;
	float VerticalSpeed = 0.f;
	float CameraDistance = 600.f;
	float CameraHeight = 180.f;
	float RespawnTime = 15.f;
	int32 NumSeats = 1;
	/** Turret slew rate (deg / s) and pitch limits. */
	float TurretRate = 80.f;
	float TurretMinPitch = -8.f;
	float TurretMaxPitch = 30.f;
	TArray<FArenaONS_GunSpec> Guns;
	/** Damage dealt to enemies run over. */
	float RoadkillSpeed = 750.f;
	/** Bots: how close they must get to the destination before leaving the vehicle. */
	float BotArriveRadius = 900.f;
	/** Bots stay inside and fight at the objective. */
	bool bBotStaysAtObjective = false;
	/** UE2 KarmaParams.KBuoyancy (0 for the stock vehicles: they sink) x the water volume's KBuoyancy, of gravity. */
	float KBuoyancy = 0.f;
	/**
	 * ONSVehicle bEnableProximityViewShake (ONSHoverTank: ViewShakeRadius 600 UU, ViewShakeOffsetMag (0.5, 0, 2) UU,
	 * ViewShakeOffsetFreq 7): players near the driven vehicle get a sine view offset. Radius in cm, 0 = off.
	 */
	float ProximityShakeRadius = 0.f;
	FVector ProximityShakeOffsetMag = FVector::ZeroVector;
	float ProximityShakeFreq = 0.f;
};

/** Per-tick control input for a vehicle seat (from the player's keys or the vehicle AI). */
struct FArenaONS_VehicleInput
{
	/** Forward (+) / back (-) */
	float Throttle = 0.f;
	/** Right (+) / left (-): steering for ground vehicles, strafing for hover / flyer */
	float Steer = 0.f;
	/** Up (+) / down (-) for the flyer */
	float Rise = 0.f;
	bool bFire = false;
	bool bAltFire = false;
	bool bJump = false;
	bool bUse = false;
	/** Yaw the hull should face (hover / flyer). */
	float DesiredYaw = 0.f;
	bool bHasDesiredYaw = false;
	/** World point the weapons aim at. */
	FVector AimPoint = FVector::ZeroVector;
	/** View rotation (camera / turret aim). */
	FRotator AimRotation = FRotator::ZeroRotator;
};

/**
 * Hosting for UT2004 clients (Net/UE2Server*): input of a remote player driving a vehicle or manning a seat (their
 * ServerDrive / VehicleFire calls). Returns true when it filled the input (then the vehicle uses it instead of the
 * player's keys or the AI). Unset outside hosting.
 */
OPENUT2K4_API extern TFunction<bool(const APawn* Seat, AController* Controller, FArenaONS_VehicleInput& OutInput)> GArenaONSExternalInput;

/**
 * Onslaught vehicle: a physics-simulated box with arcade raycast suspension / hover thrusters / lift, driven by
 * the possessing controller (players: keys polled through the player controller; bots: built-in driving AI).
 * The driver's character is hidden and attached while driving. Seat 1 (Caravan) is an AArenaONS_VehicleSeat pawn.
 */
UCLASS()
class OPENUT2K4_API AArenaONS_Vehicle : public APawn
{
	GENERATED_BODY()

public:
	AArenaONS_Vehicle();

	/** Tuning of a type; fitted to the imported / replacement model when that model is available (ArenaArtVehicles). */
	static const FArenaONS_VehicleSpec& GetSpec(EArenaONS_VehicleType Type);
	/** Tuning of a type for the procedural (false) or art (true) model. */
	static const FArenaONS_VehicleSpec& GetSpec(EArenaONS_VehicleType Type, bool bArtModel);
	static FString GetTypeName(EArenaONS_VehicleType Type);

	/** Call right after SpawnActorDeferred (before FinishSpawning). */
	void InitVehicle(EArenaONS_VehicleType InType, int32 InTeam, AArenaONS_VehicleFactory* InFactory);

	const FArenaONS_VehicleSpec& GetSpec() const { return GetSpec(VehicleType, UsesArtModel()); }
	/** True when this vehicle shows the imported / replacement model (decided once, at spawn). */
	bool UsesArtModel() const;
	EArenaONS_VehicleType GetVehicleType() const { return VehicleType; }
	FString GetVehicleName() const { return GetSpec().Name; }
	int32 GetTeam() const { return Team; }
	float GetHealth() const { return Health; }
	float GetHealthFraction() const;
	bool IsDestroyedVehicle() const { return bWrecked; }
	/** Heat (0..1) of the seat's primary (0) / alt (1) gun; -1 if the seat has none. */
	float GetGunHeat(int32 Seat, uint8 FireMode) const;
	const FArenaONS_GunSpec* FindGun(int32 Seat, uint8 FireMode, int32* OutIndex = nullptr) const;
	bool IsLocked() const { return bLocked; }
	float GetSpeed() const;
	AArenaONS_VehicleFactory* GetFactory() const { return Factory.Get(); }

	// ---- seats ----
	AArenaCharacter* GetDriverCharacter() const { return DriverChar.Get(); }
	AArenaCharacter* GetSeatCharacter(int32 Seat) const;
	AController* GetSeatController(int32 Seat) const;
	bool HasDriver() const { return DriverChar.IsValid(); }
	bool IsEmpty() const;
	int32 GetNumSeats() const { return GetSpec().NumSeats; }
	/** First free seat (driver first), -1 if full. */
	int32 GetFreeSeat() const;
	/** Team rules: own team always; enemies only once the vehicle was driven (unlocked) and it is empty. */
	bool CanBeEnteredBy(const AArenaCharacter* Char) const;
	/** Put a character into a seat (its controller possesses the vehicle / seat pawn). */
	bool EnterSeat(AArenaCharacter* Char, int32 Seat);
	/** Leave a seat: the character is placed safely beside the vehicle and repossessed. Returns it. */
	AArenaCharacter* LeaveSeat(int32 Seat, bool bForced = false);
	void EjectAll();
	/** Seat pawn (1) access. */
	AArenaONS_VehicleSeat* GetSeatPawn() const { return SeatPawn; }

	/** Destroy with an explosion; occupants die with kill credit to Killer. */
	void Explode(AController* Killer, const FString& WeaponName);

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual FVector GetPawnViewLocation() const override;
	virtual FRotator GetViewRotation() const override;

	// ---- used by the seat pawn ----
	/** Fire a gun if ready. AimPoint in world space. */
	bool TryFireGun(int32 GunIndex, AController* Instigator, const FVector& AimPoint);
	void UpdateTurretAim(const FRotator& AimRotation, float DeltaSeconds, bool bPassengerTurret);
	FVector GetTurretPivot(bool bPassengerTurret) const;
	/** Turret aim relative to the hull (pitch, yaw), the main or the rear / passenger turret (hosting: the guns' aim for UE2 clients). */
	FRotator GetTurretAim(bool bPassengerTurret) const { return bPassengerTurret ? FRotator(RearTurretPitch, RearTurretYaw, 0.f) : FRotator(TurretPitch, TurretYaw, 0.f); }
	/** Computes the world point under the crosshair of a camera. */
	FVector TraceAimPoint(const FVector& ViewLoc, const FRotator& ViewRot) const;
	/** Collision/visibility ignore list (self, seat pawn, riders). */
	void AddIgnoredActors(struct FCollisionQueryParams& Params) const;

	/** Debug: let the AI drive for a human driver (-ONSAutoDrive). */
	bool bDebugAutoDrive = false;

	// ---- components ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> Body;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> VisualRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> TurretRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> BarrelRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> RearTurretRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> RearBarrelRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USpringArmComponent> SpringArm;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;

	void BuildModel();
	UStaticMeshComponent* AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Loc, const FRotator& Rot, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow = true);
	void UpdateTeamLook();
	/** Imported / replacement model (ArenaArtVehicles); false keeps the procedural model. */
	bool BuildArtModel();
	void PlayArtSound(const TCHAR* Path, const FVector& Location, float Volume = 1.f);
	/** Test harness (-ONSArtShowcase): camera cycling through the vehicle types. */
	void TickArtShowcase();

	// ---- simulation ----
	void GatherPlayerInput(APlayerController* PC, float DeltaSeconds);
	void GatherAIInput(AController* C, float DeltaSeconds);
	void SimulatePhysics(float DeltaSeconds);
	void UpdateWeapons(float DeltaSeconds);
	void UpdateRoadkill(float DeltaSeconds);
	void UpdateVisuals(float DeltaSeconds);
	void CheckFlipped(float DeltaSeconds);

	// ---- AI ----
	bool FindAITarget(AActor*& OutTarget, FVector& OutAimPoint) const;
	void RepathAI(const FVector& Destination);
	FVector GetAISteerTarget();

	UPROPERTY(Replicated) EArenaONS_VehicleType VehicleType = EArenaONS_VehicleType::Dustrunner;
	UPROPERTY(Replicated) int32 Team = ArenaTeam::None;
	UPROPERTY(Replicated) float Health = 300.f;
	UPROPERTY(Replicated) float Heat[4] = { 0.f, 0.f, 0.f, 0.f };
	UPROPERTY(Replicated) bool bLocked = true;
	UPROPERTY(Replicated) TObjectPtr<AArenaONS_VehicleSeat> SeatPawn;
	/** Replicated so the HUD knows who drives. */
	UPROPERTY(Replicated) TObjectPtr<AArenaCharacter> ReplicatedDriver;

	TWeakObjectPtr<AArenaCharacter> DriverChar;
	TWeakObjectPtr<AArenaONS_VehicleFactory> Factory;
	/** Hidden state of the rider's attached actors before entering (restored on exit). */
	TMap<TWeakObjectPtr<AActor>, bool> DriverAttachedHidden;

	FArenaONS_VehicleInput Input;
	float NextFireTime[4] = { 0.f, 0.f, 0.f, 0.f };
	int32 MuzzleIndex[4] = { 0, 0, 0, 0 };
	bool bOverheated[4] = { false, false, false, false };
	float TurretYaw = 0.f;     // relative to hull
	float TurretPitch = 0.f;
	float RearTurretYaw = 180.f;
	float RearTurretPitch = 0.f;
	float EnterTime = -100.f;
	float LastBoostTime = -100.f;
	float LastJumpTime = -100.f;
	float GroundedFraction = 0.f;
	FVector AvgGroundNormal = FVector::UpVector;
	float FlippedTime = 0.f;
	float SteerVisual = 0.f;
	float WheelSpin = 0.f;
	TArray<float> WheelCompression;
	bool bWrecked = false;
	bool bModelBuilt = false;
	TWeakObjectPtr<AController> LastDamager;
	FString LastDamageWeapon;
	float LastDamageTime = -100.f;
	bool bWasDriven = false;

	// ---- AI state ----
	FVector AIDestination = FVector::ZeroVector;
	TWeakObjectPtr<AArenaONS_PowerNode> AITargetNode;
	bool bAIAttackNode = false;
	/** Brake hold (StopThreshold): where the vehicle came to rest. */
	FVector HoldAnchor = FVector::ZeroVector;
	bool bHoldAnchorSet = false;
	float AINextDestTime = 0.f;
	float AINextRepathTime = 0.f;
	TArray<FVector> AIPath;
	int32 AIPathIndex = 0;
	float AIStuckTime = 0.f;
	float AIReverseUntil = 0.f;
	float AIReverseSteer = 0.f;
	int32 AIStuckCount = 0;
	float AINextTargetTime = 0.f;
	TWeakObjectPtr<AActor> AITarget;
	FVector AITargetAim = FVector::ZeroVector;
	FRotator AIAimRotation = FRotator::ZeroRotator;
	float AIDriveStartTime = 0.f;
	float AINextLogTime = 0.f;
	FVector AILastPos = FVector::ZeroVector;
	float AIProgressCheckTime = 0.f;
	float AISpaceClimbUntil = 0.f;
	int32 AIOrbitIndex = -1;
	float AINextSightTime = 0.f;
	bool bAINodeVisible = false;
	float AIOrbitBestDist = 0.f;
	float AIOrbitTime = 0.f;

	// ---- visuals ----
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	UPROPERTY() TArray<TObjectPtr<USceneComponent>> WheelRoots;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> TeamGlowParts;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> TeamPaintParts;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ThrusterParts;
	int32 BuiltTeam = -2;
	/** Imported / replacement model (valid only when UsesArtModel()). */
	UPROPERTY() FArenaVehicleArtRig ArtRig;
	/** -1 undecided, 0 procedural, 1 art model. */
	mutable int8 ArtModelState = -1;
	/** Turret aims (yaw, pitch, rear yaw, rear pitch) compressed for clients. */
	UPROPERTY(Replicated) uint16 RepTurretAim[4] = { 0, 0, 32768, 0 };
	float LastGunSoundTime[4] = { -100.f, -100.f, -100.f, -100.f };
	bool bArtEngineOn = false;
	bool bArtDriverSeated = false;

	friend class AArenaONS_VehicleSeat;
	friend class UArenaRules_ONS;
};

/**
 * Passenger seat with its own turret (Caravan). Attached to the vehicle; the passenger's controller possesses it.
 */
UCLASS()
class OPENUT2K4_API AArenaONS_VehicleSeat : public APawn
{
	GENERATED_BODY()

public:
	AArenaONS_VehicleSeat();

	void InitSeat(AArenaONS_Vehicle* InVehicle, int32 InSeatIndex);
	AArenaONS_Vehicle* GetVehicle() const { return Vehicle; }
	int32 GetSeatIndex() const { return SeatIndex; }
	AArenaCharacter* GetRider() const { return Rider.Get(); }

	virtual void Tick(float DeltaSeconds) override;
	virtual FVector GetPawnViewLocation() const override;
	virtual FRotator GetViewRotation() const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USpringArmComponent> SpringArm;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;

protected:
	UPROPERTY(Replicated) TObjectPtr<AArenaONS_Vehicle> Vehicle;
	int32 SeatIndex = 1;
	TWeakObjectPtr<AArenaCharacter> Rider;
	TMap<TWeakObjectPtr<AActor>, bool> RiderAttachedHidden;
	float EnterTime = -100.f;
	float AINextTargetTime = 0.f;
	TWeakObjectPtr<AActor> AITarget;
	FVector AITargetAim = FVector::ZeroVector;
	FRotator AIAim = FRotator::ZeroRotator;

	friend class AArenaONS_Vehicle;
};

/**
 * Burnt-out hull left by a destroyed vehicle when its model has an imported / replacement wreck mesh.
 * Tossed by the blast, settles under physics and is removed after a while.
 */
UCLASS()
class OPENUT2K4_API AArenaONS_VehicleWreck : public AActor
{
	GENERATED_BODY()

public:
	AArenaONS_VehicleWreck();

	/** Server: leave a wreck where the vehicle is (nullptr if its model has no wreck mesh). */
	static AArenaONS_VehicleWreck* SpawnFor(AArenaONS_Vehicle* Vehicle);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> Box;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;

protected:
	virtual void BeginPlay() override;
	void BuildWreck();

	UPROPERTY(Replicated) EArenaONS_VehicleType VehicleType = EArenaONS_VehicleType::Dustrunner;
	FVector LaunchVelocity = FVector::ZeroVector;
};

/** Helpers shared by the ONS files. */
namespace ArenaONS
{
	/** Team of a controller's player state (-1 if none). */
	OPENUT2K4_API int32 TeamOfController(const AController* C);
	/** Effective team of a character: its controller's team, or the team of the vehicle it rides. */
	OPENUT2K4_API int32 TeamOfCharacter(const AArenaCharacter* Char);
	/** The vehicle (or seat's vehicle) a hidden rider character sits in, if any. */
	OPENUT2K4_API AArenaONS_Vehicle* GetRiddenVehicle(const AArenaCharacter* Char);
	/** Vehicle the controller currently drives or rides (vehicle pawn or seat pawn). */
	OPENUT2K4_API AArenaONS_Vehicle* GetControlledVehicle(const AController* C);
	/** Seat index the controller occupies (0 driver, 1 passenger), -1 if none. */
	OPENUT2K4_API int32 GetControlledSeat(const AController* C);
	/** Hide / attach a rider, remembering the hidden state of its attached actors. */
	void HideRider(AArenaCharacter* Char, USceneComponent* AttachTo, const FVector& RelLocation, TMap<TWeakObjectPtr<AActor>, bool>& OutHidden);
	/** Restore a rider at a world location. */
	void RestoreRider(AArenaCharacter* Char, const FVector& Location, float Yaw, const TMap<TWeakObjectPtr<AActor>, bool>& Hidden);
	/** Safe exit location next to a vehicle box (falls back to above it). */
	FVector FindExitLocation(const AArenaONS_Vehicle* Vehicle, const AActor* IgnoreActor);
	/** Gun firing from a muzzle location (projectile or hitscan). */
	void FireGunShot(AArenaONS_Vehicle* Vehicle, const FArenaONS_GunSpec& Gun, AController* Instigator, const FVector& Muzzle, const FVector& Dir);
	/** True if the -ONS<Flag> debug switch is on the command line. */
	bool HasDebugFlag(const TCHAR* Flag);
}

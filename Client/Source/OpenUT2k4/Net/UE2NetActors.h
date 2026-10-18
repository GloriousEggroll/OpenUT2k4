#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../ArenaArtVehicles.h"
#include "../ArenaCharacter.h"
#include "UE2NetActors.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;
class USkeletalMeshComponent;
class UPoseableMeshComponent;
class UMeshComponent;
class UAudioComponent;
class USoundBase;
namespace ArenaArtFX { struct FProjectileArt; }
namespace ArenaArt { struct FWeaponArt; struct FCharacterModel; }

namespace UE2NetActors
{
	/**
	 * Our weapon class for a UE2 weapon or attachment class ("XWeapons.RocketAttachment", "RocketLauncher",
	 * "TransLauncher" ...), matched by the short name's prefix; null when we have none.
	 */
	OPENUT2K4_API UClass* WeaponClassFor(const FString& UE2Class);
}

/**
 * Native stand-ins for actors a UT2004 server replicates (step 3 of Docs/UE2Net.md). They only show what the server
 * says: no gameplay, no collision with the world's gameplay actors (pickups, flags, triggers), no damage. The world
 * layer (UE2NetWorld.h) creates them and feeds them the decoded properties.
 */

/**
 * A replicated pawn (xPawn / UnrealPawn): our character body (player model from the PRI's CharacterName, team skin,
 * native animation) moved by the server's PawnPosition with client-side extrapolation and smoothing, the weapon
 * attachment in its hand, and the ragdoll / gibs when the server tears it off.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetPawn : public AArenaCharacter
{
	GENERATED_BODY()

public:
	AUE2NetPawn();

	/** Player model for a UT2004 character name (UPL record: mesh + body skin); falls back to a pick by player id. */
	void SetCharacter(const FString& CharacterName, int32 PlayerId);
	/** A new PawnPosition (UE2 units: location, velocity; rotation in UE2 units, pitch from ViewPitch). */
	void SetNetPosition(const FVector& LocationUU, const FVector& VelocityUU, int32 YawUU, bool bTeleport);
	/** Replicated movement state: UE2 EPhysics, crouching, collision height (UU). */
	void SetNetPhysics(uint8 InPhysics);
	void SetNetCrouched(bool bCrouched, float CollisionHeightUU);
	/** Pawn.ViewPitch (one byte: pitch / 256). */
	void SetNetViewPitch(uint8 InViewPitch) { ViewPitchByte = InViewPitch; }
	/** Third-person weapon from the attachment class ("XWeapons.RocketAttachment"); empty hides it. */
	void SetNetWeapon(const FString& AttachmentClass);
	/** The attachment fired (FlashCount changed): fire animation and a muzzle flash. */
	void NetFired(uint8 FiringMode);
	/** Pawn.AnimAction ("Weapon_Switch", taunts ...). */
	void NetAnimAction(FName Action);
	/** HitFxTicker changed: a hit reaction (bone name from HitFx). */
	void NetHit(FName Bone);
	/** bTearOff: the server killed this pawn. Momentum / hit location in UE2 units; bGib for gibbing damage. */
	void NetDie(const FVector& MomentumUU, const FVector& HitLocationUU, bool bGib);
	bool IsNetDead() const { return bNetDead; }
	/** Driving a vehicle (Vehicle.Driver): hidden, carried along with the vehicle, the camera behind the vehicle. */
	void SetNetVehicle(AActor* Vehicle);
	AActor* GetNetVehicle() const { return NetVehicle.Get(); }
	/** Animation set prefix of the player model in use ("Animations/Pkg/Set_"), null before it is applied. */
	const TCHAR* GetAnimPrefix() const;
	/** World location of the weapon muzzle (third-person weapon tip, else the eyes). */
	FVector GetMuzzleLocation() const;
	/** Camera: behind the pawn (the server's ClientSetBehindView) or first person. */
	static bool bBehindView;
	/** Statistics: PawnPosition updates and the distance between the shown and the received location at each one. */
	static int32 NumCorrections;
	static double SumCorrection;
	static float MaxCorrection;

	virtual FRotator GetViewRotation() const override;
	virtual FRotator GetBaseAimRotation() const override;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;
	/** Never hurt locally: the server decides. */
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override { return 0.f; }
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override {}

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PossessedBy(AController* NewController) override {}

	void ApplyModel();
	void ApplyCharacterSkin();

	const ArenaArt::FCharacterModel* Model = nullptr;
	const ArenaArt::FCharacterModel* AppliedModel = nullptr;
	int32 ModelPlayerId = 0;
	int32 LastTeam = -100;
	FString CharSkin;
	FString CharSkinPkg;
	FString FaceSkin;
	bool bCharacterKnown = false;
	bool bModelApplied = false;
	const ArenaArt::FWeaponArt* WeaponArt = nullptr;
	FString WeaponClassName;

	// network movement (centimetres)
	FVector NetLocation = FVector::ZeroVector;
	FVector NetVelocity = FVector::ZeroVector;
	double NetTime = 0.0;
	float NetYaw = 0.f;
	bool bHasNetPosition = false;
	uint8 NetPhysics = 1;
	uint8 ViewPitchByte = 0;
	float SmoothYaw = 0.f;
	bool bNetDead = false;
	double DeadTime = 0.0;
	float CollisionHalfHeight = 88.f;
	TWeakObjectPtr<AActor> NetVehicle;
};

/** A replicated projectile: the art of our projectile table, flown along the replicated velocity, exploding on close. */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetProjectile : public AActor
{
	GENERATED_BODY()

public:
	AUE2NetProjectile();

	/** Style of ArenaArtFX's projectile table ("Rocket", "FlakChunk" ...), size scale. */
	void Setup(FName InStyle, float InScale, const FLinearColor& InGlow);
	void SetNetVelocity(const FVector& VelocityUU);
	void SetNetLocation(const FVector& LocationUU);
	void SetNetPhysics(uint8 InPhysics) { Physics = InPhysics; }
	/**
	 * UE2 bNetTemporary projectiles (rockets, flak, link plasma ...): the server sends them once and closes the
	 * channel; the client flies them itself until they hit something (explode there) or their LifeSpan ends.
	 * Bounces: flak chunks / grenades bounce off walls MaxBounces times with Damping.
	 */
	void SetLocalSimulation(bool bInTemporary, int32 InMaxBounces, float InDamping, float InLifeSpan);
	bool IsTemporary() const { return bTemporary; }
	/** Statistics: projectiles gone (exploded, life span over) and the distance they flew. */
	static int32 NumEnded;
	static double SumDistance;
	/** The server closed the channel: the explosion / impact effect, then gone. */
	void NetExplode();
	/** Centimetres flown since it was spawned (statistics). */
	float GetFlightDistance() const { return FlightDistance; }

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

protected:
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	FName Style;
	float Scale = 1.f;
	const ArenaArtFX::FProjectileArt* Art = nullptr;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Meshes;
	TArray<float> Spin;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Glow;
	FVector Velocity = FVector::ZeroVector;
	uint8 Physics = 6;
	bool bStopped = false;
	bool bExploded = false;
	FLinearColor GlowColor = FLinearColor::White;
	float FlightDistance = 0.f;
	bool bTemporary = false;
	int32 BouncesLeft = 0;
	float Damping = 0.5f;
	/** Hit test against the replicated pawns (they have no collision): explode on contact. */
	bool HitsPawn(const FVector& From, const FVector& To, FVector& OutHit) const;
};

/**
 * Anything else worth showing that has no native counterpart yet (ONS vehicles, the bombing run ball when no
 * native ball exists ...): a team-tinted box of the replicated collision size, or a static mesh, at the replicated
 * location.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetPlaceholder : public AActor
{
	GENERATED_BODY()

public:
	AUE2NetPlaceholder();

	/** Mesh: a static mesh ("StaticMeshes/Pkg/Name"), else a skeletal mesh ("SkeletalMeshes/Pkg/Name"), else a box. */
	void Setup(const FString& Label, float RadiusUU, float HeightUU, int32 Team, const FString& StaticMeshPath = FString(),
		const FString& SkeletalMeshPath = FString());
	/** Vehicles: the KRigidBody state (UE2 units; linear velocity as replicated). */
	void SetNetRigidBody(const FVector& LocationUU, const FQuat& Rotation, const FVector& VelocityUU);
	void SetNetLocation(const FVector& LocationUU, const FVector& VelocityUU, const FRotator& Rotation);
	void SetTeam(int32 Team);

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USkeletalMeshComponent> Skel;

protected:
	virtual void Tick(float DeltaSeconds) override;
	FQuat NetRotation = FQuat::Identity;
	bool bRigidBody = false;
	FVector NetLocation = FVector::ZeroVector;
	FVector NetVelocity = FVector::ZeroVector;
	double NetTime = 0.0;
	bool bHasNet = false;
	bool bArtMesh = false;
};

/**
 * A replicated vehicle (UE2 Vehicle: ONS vehicles, turrets, Assault vehicles; also a stationary ONSWeaponPawn turret
 * without a vehicle base). The chassis is the class's own skeletal mesh; when it is the mesh of one of our vehicle
 * types (ArenaArtVehicles.h) our art rig is used (team skins, wheels, rollers, thruster glows). It is moved by the
 * KRigidBody state (or Location / Rotation), its weapons (UE2 ONSWeapon actors: their own meshes on the chassis
 * bones) aim at the replicated CurrentHitLocation, fire flashes and tracers follow FlashCount / HitCount, the driver
 * and gunners sit in their seats when the class draws them (bDrawDriverInTP), and bDestroyAppearance leaves the wreck.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetVehicle : public AActor
{
	GENERATED_BODY()

public:
	AUE2NetVehicle();

	/**
	 * Mesh paths are content paths ("SkeletalMeshes/Pkg/Name", "StaticMeshes/Pkg/Name"); skins UE2 object paths
	 * ("Pkg.Group.Name", empty for none). StaticMeshPath: a static mesh base when the class has no skeletal mesh.
	 */
	void Setup(const FString& Label, const FString& SkeletalMeshPath, const FString& StaticMeshPath, const FString& RedSkin, const FString& BlueSkin,
		const FString& DestroyedMeshPath, float RadiusUU, float HeightUU);
	void SetTeam(int32 InTeam);
	/** The KRigidBody state (UE2 units; velocity in UU/s). */
	void SetNetRigidBody(const FVector& LocationUU, const FQuat& Rotation, const FVector& VelocityUU);
	/** Vehicles without a rigid body state: Location / Velocity / Rotation. */
	void SetNetLocation(const FVector& LocationUU, const FVector& VelocityUU, const FRotator& Rotation, bool bTeleport);
	/** Vehicle.bDriving (engine sound, thruster glow). */
	void SetDriving(bool bInDriving) { bDriving = bInDriving; }
	/** Actor.AmbientSound (engine loop); null stops it. Volume 0..1. */
	void SetAmbientSound(USoundBase* Sound, float Volume);

	/**
	 * A weapon (UE2 ONSWeapon on channel Key) on this vehicle: its mesh on AttachBone of the chassis (else at the
	 * chassis origin). The bones turn to the aim; FireBone + FireOffsetUU (along the aim) is the muzzle.
	 */
	void SetWeapon(int32 Key, const FString& MeshPath, FName AttachBone, FName YawBone, FName PitchBone, FName FireBone, float FireOffsetUU,
		const FString& RedSkin, const FString& BlueSkin);
	void RemoveWeapon(int32 Key);
	bool HasWeapon(int32 Key) const { return Guns.Contains(Key); }
	/** ONSWeapon turning limits (degrees, relative to the weapon's rest pose): PitchDownLimit .. PitchUpLimit, and the
	 *  yaw range YawStartConstraint .. YawEndConstraint when bLimitYaw. */
	void SetWeaponLimits(int32 Key, float PitchMin, float PitchMax, bool bLimitYaw, float YawMin, float YawMax);
	/** CurrentHitLocation of the weapon (UE2 units): its turret turns towards it. */
	void SetWeaponAim(int32 Key, const FVector& TargetUU);
	/** FlashCount changed: muzzle flash; bInstantHit: a tracer and an impact at HitUU. */
	void WeaponFired(int32 Key, const FVector& HitUU, bool bInstantHit, bool bHasHit);
	/** HitCount changed: an impact at HitUU (instant hit weapons). */
	void WeaponHit(int32 Key, const FVector& HitUU);

	/**
	 * A pawn in a seat (Key: the vehicle's or the weapon pawn's channel). bShow: drawn in the seat (UE2
	 * bDrawDriverInTP) at DrivePosUU / DriveRot of the seat's class, relative to the weapon WeaponKey (gunners) or
	 * the vehicle. Null pawn: empty seat.
	 */
	void SetSeat(int32 Key, class AUE2NetPawn* Pawn, bool bShow, const FVector& DrivePosUU, const FRotator& DriveRot, int32 WeaponKey);
	/** A wheel of the class (UE2 SVehicleWheel: BoneName, WheelRadius in UU, SteerType 0 fixed / 1 steered / 2 inverted). */
	struct FNetWheel
	{
		FName Bone;
		float RadiusUU = 0.f;
		uint8 SteerType = 0;
	};
	/** Wheels of a vehicle without our art rig (the rig has its own table): they spin, steer and follow the ground. */
	void SetWheels(const TArray<FNetWheel>& InWheels);
	/** bDestroyAppearance: the wreck (DestroyedVehicleMesh) with an explosion; bDisintegrate: gone in a big one. */
	void SetDestroyed(bool bInDestroyed, bool bDisintegrate);
	bool IsDestroyedAppearance() const { return bDestroyed; }
	/** ONSVehicle.ExplosionCount changed (a damage explosion). */
	void NetExplosion();
	/** Statistics for the summary log: weapons aimed / fired, seats shown. */
	static int32 NumAimUpdates;
	static int32 NumFlashes;
	static int32 NumSeatsShown;
	static int32 NumWrecks;
	/** Turret aim check: angle between the barrel bone's X axis and the direction to the replicated aim point. */
	static int32 NumAimChecks;
	static double SumAimError;
	static float MaxAimError;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;

protected:
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	struct FGun
	{
		TWeakObjectPtr<UPoseableMeshComponent> Comp;
		/** Our art rig's gun this weapon drives (its yaw / pitch slot), else INDEX_NONE (Comp is ours). */
		int32 RigGun = INDEX_NONE;
		int32 RigAim = 0;
		int32 YawBone = INDEX_NONE;
		int32 PitchBone = INDEX_NONE;
		FName FireBone;
		float FireOffset = 0.f;
		FVector Target = FVector::ZeroVector;
		bool bHasTarget = false;
		float Yaw = 0.f;
		float Pitch = 0.f;
		int32 Shots = 0;
		FString RedSkin, BlueSkin;
		float PitchMin = -60.f;
		float PitchMax = 80.f;
		bool bLimitYaw = false;
		float YawMin = -180.f;
		float YawMax = 180.f;
		/** The aim point is outside the limits (the barrel stops at them); the barrel has caught up with the aim. */
		bool bClamped = false;
		bool bSettled = false;
	};
	struct FSeat
	{
		TWeakObjectPtr<class AUE2NetPawn> Pawn;
		TWeakObjectPtr<USkeletalMeshComponent> Body;
		int32 WeaponKey = 0;
		FVector Pos = FVector::ZeroVector;
		FRotator Rot = FRotator::ZeroRotator;
		bool bShow = false;
		bool bNoPose = false;
		TWeakObjectPtr<USkeletalMesh> ShownMesh;
	};

	/** A wheel bone posed by us: spin, steering, suspension (the ground under it, traced each frame). */
	struct FWheelState
	{
		int32 BoneIndex = INDEX_NONE;
		float Radius = 40.f;
		float Travel = 30.f;
		int32 SteerSign = 0;
		/** Our rig's suspension point (rig wheels), else INDEX_NONE. */
		int32 RigSuspension = INDEX_NONE;
		float Offset = 0.f;
	};
	TArray<FWheelState> WheelStates;
	double NextAimCheck = 0.0;
	float SteerDegrees = 0.f;
	/** Traces the ground under each wheel and eases its offset (along the chassis up axis, within its travel). */
	void UpdateSuspension(UPoseableMeshComponent* C, float DeltaSeconds);
	USceneComponent* GetChassisComponent() const;
	UPoseableMeshComponent* GetGunComponent(const FGun& G) const;
	/** Yaw / pitch (degrees) that point a gun's yaw bone at a world point, in the gun component's frame. */
	bool AimAngles(const UPoseableMeshComponent* C, int32 YawBone, const FVector& WorldTarget, float& OutYaw, float& OutPitch) const;
	void PoseGun(UPoseableMeshComponent* C, int32 YawBone, int32 PitchBone, float Yaw, float Pitch) const;
	FVector GetMuzzle(const FGun& G, FVector& OutDir) const;
	void UpdateSeat(FSeat& Seat);
	void ApplySkins(UMeshComponent* C, const FString& RedSkin, const FString& BlueSkin);

	UPROPERTY() TObjectPtr<USceneComponent> RigRoot;
	UPROPERTY() FArenaVehicleArtRig Rig;
	UPROPERTY() TObjectPtr<UPoseableMeshComponent> Chassis;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> BaseMesh;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Wreck;
	UPROPERTY() TObjectPtr<UAudioComponent> Ambient;
	UPROPERTY() TArray<TObjectPtr<USceneComponent>> GunComps;
	UPROPERTY() TArray<TObjectPtr<USkeletalMeshComponent>> SeatBodies;
	TMap<int32, FGun> Guns;
	TMap<int32, FSeat> Seats;
	TSet<int32> RigGunsUsed;
	FString ChassisRedSkin, ChassisBlueSkin, DestroyedMesh;
	FString Label;
	int32 Team = -2;
	bool bRig = false;

	FQuat NetRotation = FQuat::Identity;
	bool bRigidBody = false;
	FVector NetLocation = FVector::ZeroVector;
	FVector NetVelocity = FVector::ZeroVector;
	double NetTime = 0.0;
	bool bHasNet = false;
	float WheelSpin = 0.f;
	float LastYaw = 0.f;
	float YawRate = 0.f;
	bool bDriving = false;
	bool bDestroyed = false;
	float AmbientVolume = 1.f;
	TWeakObjectPtr<USoundBase> AmbientSound;
};

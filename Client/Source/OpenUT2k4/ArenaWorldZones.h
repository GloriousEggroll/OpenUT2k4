#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "GameFramework/PhysicsVolume.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaUE2World.h"
#include "ArenaWorldZones.generated.h"

class USoundBase;
class UStaticMeshComponent;
class UStaticMesh;

/**
 * UE2 zones and volumes, native.
 *
 * Zones: UE2 zones are BSP regions (separated by zone portals), one ZoneInfo each. The converter exports the BSP
 * tree (bsptree.bin); UArenaWorldZoneSubsystem::GetZoneAt walks it like UModel::PointRegion.
 *
 * Physics volumes (PhysicsVolume, WaterVolume, LavaVolume, PressureVolume, xFallingVolume ...): AArenaPhysicsVolume
 * is an engine APhysicsVolume, so UCharacterMovementComponent and UProjectileMovementComponent use its gravity
 * (UE2 Gravity, relative to LevelInfo.DefaultGravity), TerminalVelocity, FluidFriction and bWaterVolume (swimming)
 * natively. Membership follows UE2 (the actor's location inside the volume brush, highest Priority wins) and is
 * assigned by the subsystem every tick to characters, projectiles and vehicles (no overlap queries). The subsystem
 * also runs the UE2 PhysicsVolume behaviour: PainTimer (DamagePerSec / DamageType, bPainCausing), bDestructive,
 * bMoveProjectiles (ZoneVelocity added to entering projectiles), the horizontal part of Gravity for projectiles,
 * entry / exit sounds, drowning, and the view fog / tint of the camera's volume. The full Gravity vector,
 * TerminalVelocity, ZoneVelocity and bBounceVelocity of pawns are UArenaCharacterMovement's; thrown flags / balls
 * use them in AArenaCarryable::TickFlight.
 *
 * Blocking volumes: invisible collision (players, vehicles, physics objects; never weapon traces or projectiles,
 * like UE2's zero-extent rule), built from the brush faces; bClassBlocker / BlockedClasses limit them to vehicles.
 */

/** A UE2 brush (volume shape) in world space: faces for collision and the brush BSP for UE2 point checks. */
struct OPENUT2K4_API FArenaBrushShape
{
	TArray<FPlane> Planes;
	TArray<int32> Back;
	TArray<int32> Front;
	TArray<TArray<FVector>> Faces;
	FBox Bounds = FBox(ForceInit);
	bool bConvex = true;

	/** From a volume marker ("brush" faces / planes, else "bounds_world" as a box). */
	bool Load(const TSharedPtr<FJsonObject>& Marker);
	bool IsValid() const { return Bounds.IsValid != 0; }
	/** UE2 Brush->PointCheck: inside the brush (Radius > 0: within Radius of it, conservative). */
	bool Encompasses(const FVector& Point, float Radius = 0.f) const;
	/** Collision mesh of the faces (no material), or null. */
	UStaticMesh* BuildMesh(const FString& Name) const;
};

/** UE2 ZoneInfo: a BSP zone's settings. */
UCLASS()
class OPENUT2K4_API AArenaZoneInfo : public AInfo, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaZoneInfo();

	// ---- UE2 ZoneInfo ----
	UPROPERTY(VisibleAnywhere, Category = UE2) FName ZoneTag;
	/** Centimetres. Actors below it die (bSoftKillZ: only when falling). Not set: -1e9. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float KillZ = -1.0e9f;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bSoftKillZ = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bTerrainZone = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bDistanceFog = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bClearToFogColor = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) FColor DistanceFogColor = FColor(128, 128, 128);
	/** Centimetres. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float DistanceFogStart = 6000.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DistanceFogEnd = 16000.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString LocationName;
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 AmbientBrightness = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 AmbientHue = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 AmbientSaturation = 255;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Event;
	/** BSP zone number (index into the model's zone table), -1 if none. */
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 ZoneNumber = INDEX_NONE;
	FString UE2Name;

	virtual bool IsSkyZone() const { return false; }
	/** UE2 ZoneInfo.ActorEntered / ActorLeaving (pawns, tracked by the subsystem). */
	virtual void ActorEntered(AActor* Other) {}
	virtual void ActorLeaving(AActor* Other) {}

	// IArenaUE2Actor
	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
	virtual FName GetUE2Event() const override { return Event; }
};

/** UE2 SkyZoneInfo: the zone the 3D sky box is rendered from (AArenaSkybox). */
UCLASS()
class OPENUT2K4_API AArenaSkyZoneInfo : public AArenaZoneInfo
{
	GENERATED_BODY()

public:
	virtual bool IsSkyZone() const override { return true; }
};

/** UE2 PhysicsVolume (base of WaterVolume, LavaVolume ...). Units: UE2 values are converted to centimetres on load. */
UCLASS()
class OPENUT2K4_API AArenaPhysicsVolume : public APhysicsVolume, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaPhysicsVolume(const FObjectInitializer& ObjectInitializer);

	// ---- UE2 PhysicsVolume (TerminalVelocity, FluidFriction, Priority, bWaterVolume: APhysicsVolume's) ----
	/** UE2 units (raw, full vector); GetGravityVector scales it relative to UE2DefaultGravity onto our world gravity. */
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector Gravity = FVector(0.f, 0.f, -950.f);
	/**
	 * cm/s. UE2 native physics adds it to the move of pawns inside (walking: horizontal part only), not to their
	 * velocity; bMoveProjectiles adds it once to the velocity of projectiles entering (PhysicsVolume.Touch) and
	 * pushes falling dropped items (flags, the ball). bBounceVelocity: a pawn landing inside bounces off with it.
	 */
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector ZoneVelocity = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = UE2) float GroundFriction = 8.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bPainCausing = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DamagePerSec = 0.f;
	/** UE2 damage type path ("Engine.FellLava"). */
	UPROPERTY(VisibleAnywhere, Category = UE2) FString DamageType;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bDestructive = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bNoInventory = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bMoveProjectiles = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bBounceVelocity = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bNeutralZone = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bDamagesVehicles = true;
	/** Kills on entry (UE2 kill volumes / xFallingVolume with huge damage). */
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bKillVolume = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString LocationName;
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 LocationPriority = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> EntrySound;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> ExitSound;
	/** View tint (UE2 ViewFog: 0..1 RGB added fog) and flash. */
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector ViewFog = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector ViewFlash = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bDistanceFog = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) FColor DistanceFogColor = FColor(128, 128, 128);
	/** Centimetres. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float DistanceFogStart = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DistanceFogEnd = 4000.f;
	/** UE2 Karma damping added to bodies inside (vehicles; WaterVolume 2.5 / 0.4, LavaVolume 0.8 / 0.1), per second. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float KExtraLinearDamping = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float KExtraAngularDamping = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float KBuoyancy = 1.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Event;
	FString UE2Name;
	FArenaBrushShape Shape;

	/** UE2 Volume.Encompasses (point inside the brush). */
	bool EncompassesUE2(const FVector& Point) const { return Shape.Encompasses(Point); }
	/** Gravity in cm/s^2 (our world gravity scaled by Gravity.Z / UE2DefaultGravity). */
	virtual float GetGravityZ() const override;
	/** Full UE2 gravity vector in cm/s^2 (Gravity / UE2DefaultGravity x our world gravity); Z == GetGravityZ(). */
	FVector GetGravityVector() const;
	/** True when Gravity has a horizontal part (tubes, wind): GetGravityZ alone does not describe it. */
	bool HasLateralGravity() const { return Gravity.X != 0.f || Gravity.Y != 0.f; }
	/**
	 * UE2 TerminalVelocity: falling may not go faster than this downwards (cm/s; the Z part only, so a sideways
	 * tube with TerminalVelocity 200 still carries pawns at full speed, like BR-Colossus in UT2004).
	 */
	FVector ClampToTerminalVelocity(const FVector& Velocity) const;
	virtual void ActorEnteredVolume(AActor* Other) override;
	virtual void ActorLeavingVolume(AActor* Other) override;
	/** UE2 PhysicsVolume.CausePainTo (every second while inside, when bPainCausing). */
	virtual void CausePainTo(AActor* Other);
	/** Kill message weapon name for DamageType ("Lava", "Fell" ...). */
	FString GetDamageName() const;

	// IArenaUE2Actor
	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
	virtual FName GetUE2Event() const override { return Event; }
	/** UE2: triggering a damaging volume toggles bPainCausing. */
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};

UCLASS()
class OPENUT2K4_API AArenaWaterVolume : public AArenaPhysicsVolume
{
	GENERATED_BODY()
public:
	AArenaWaterVolume(const FObjectInitializer& ObjectInitializer);
};

UCLASS()
class OPENUT2K4_API AArenaLavaVolume : public AArenaPhysicsVolume
{
	GENERATED_BODY()
public:
	AArenaLavaVolume(const FObjectInitializer& ObjectInitializer);
};

UCLASS()
class OPENUT2K4_API AArenaPressureVolume : public AArenaPhysicsVolume
{
	GENERATED_BODY()
public:
	AArenaPressureVolume(const FObjectInitializer& ObjectInitializer);
};

/** xFallingVolume: deadly fall area (100 damage per second, "fell"). */
UCLASS()
class OPENUT2K4_API AArenaFallingVolume : public AArenaPhysicsVolume
{
	GENERATED_BODY()
public:
	AArenaFallingVolume(const FObjectInitializer& ObjectInitializer);
};

/** UE2 BlockingVolume: invisible wall. */
UCLASS()
class OPENUT2K4_API AArenaBlockingVolume : public AActor, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaBlockingVolume();

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Collision;
	// ---- UE2 ----
	/** Blocks only BlockedClasses (UE2 class names, e.g. "Vehicle"). */
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bClassBlocker = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<FString> BlockedClasses;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bBlockZeroExtentTraces = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bBlockNonZeroExtentTraces = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bBlockActors = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bBlockKarma = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	FString UE2Name;
	FArenaBrushShape Shape;

	/** True when only vehicles are blocked (bClassBlocker with vehicle classes). */
	bool BlocksOnlyVehicles() const;
	/** Applies the collision responses for the UE2 flags (call after the mesh is set). */
	void ApplyCollision();

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
};

/**
 * Zones and volume membership of a world (see the file comment). Also the place for "what is at this point"
 * queries from other systems (bots, vehicles, the sky box, the HUD location name).
 */
UCLASS()
class OPENUT2K4_API UArenaWorldZoneSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UArenaWorldZoneSubsystem* Get(const UObject* WorldContext);

	/** bsptree.bin + the bsp "tree" json (zone table); false when missing. */
	bool LoadBspTree(const FString& File, const TSharedPtr<FJsonObject>& TreeJson);
	bool HasBspTree() const { return Nodes.Num() > 0; }
	/** UE2 PointRegion: BSP zone number at a point (-1 without a tree). */
	int32 GetZoneNumberAt(const FVector& Point) const;
	/** Zone actor at a point (null for zones without one). */
	AArenaZoneInfo* GetZoneAt(const FVector& Point) const;
	/** Zone actor by BSP zone number. */
	AArenaZoneInfo* GetZone(int32 ZoneNumber) const;
	/** True when the BSP zone at Point is a SkyZoneInfo's. */
	bool IsInSkyZone(const FVector& Point) const;

	void RegisterZone(AArenaZoneInfo* Zone);
	void RegisterVolume(AArenaPhysicsVolume* Volume);
	void UnregisterVolume(AArenaPhysicsVolume* Volume);
	/** Highest priority physics volume containing Point (UE2 rules), or null (the level's default). */
	AArenaPhysicsVolume* GetPhysicsVolumeAt(const FVector& Point) const;
	/** Gravity (cm/s^2, negative) at a point: the volume's, else the world's. */
	float GetGravityZAt(const FVector& Point) const;
	/** Full gravity vector (cm/s^2) at a point: the volume's UE2 Gravity vector, else (0, 0, world gravity). */
	FVector GetGravityAt(const FVector& Point) const;
	/** Location name for the HUD (volume, else zone), empty if none. */
	FString GetLocationNameAt(const FVector& Point) const;
	const TArray<TWeakObjectPtr<AArenaPhysicsVolume>>& GetVolumes() const { return Volumes; }
	bool HasVolumes() const { return Volumes.Num() > 0; }
	/** True when a point is inside a pain causing or kill volume (bots avoid these). */
	bool IsHazardAt(const FVector& Point) const;
	/** True while the camera's volume fog (water, lava, fog volumes) drives the level's height fog. */
	bool IsViewFogActive() const { return bFogApplied || ViewBlend > 0.f; }

	// UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	struct FNode
	{
		FPlane Plane;
		int32 Back = -1;
		int32 Front = -1;
		uint8 Zone[2] = { 0, 0 };
	};
	TArray<FNode> Nodes;
	TArray<FString> ZoneActorNames;
	TArray<bool> ZoneIsSky;
	TArray<TWeakObjectPtr<AArenaZoneInfo>> ZoneActors;
	TArray<TWeakObjectPtr<AArenaPhysicsVolume>> Volumes;

	/** Per tracked actor: current volume, pain timer, breath. */
	struct FTracked
	{
		TWeakObjectPtr<AArenaPhysicsVolume> Volume;
		TWeakObjectPtr<AArenaZoneInfo> Zone;
		float PainTime = 0.f;
		float UnderWaterTime = 0.f;
		float NextBreathDamage = 0.f;
		bool bHeadInWater = false;
	};
	TMap<TWeakObjectPtr<AActor>, FTracked> Tracked;
	FDelegateHandle SpawnHandle;
	float ViewBlend = 0.f;
	TWeakObjectPtr<class APostProcessVolume> ViewPP;
	TWeakObjectPtr<class AExponentialHeightFog> HeightFog;
	float SavedFogDensity = 0.f, SavedFogFalloff = 0.f, SavedFogStart = 0.f, SavedFogMaxOpacity = 1.f;
	FLinearColor SavedFogColor = FLinearColor::Black;
	FLinearColor FogColor = FLinearColor::Black;
	float FogStart = 0.f, FogEnd = 4000.f;
	bool bFogApplied = false;
	TWeakObjectPtr<AArenaPhysicsVolume> LastViewVolume;

	void OnActorSpawned(AActor* Actor);
	/** Test hooks (non-shipping): -UE2View=x,y,z,pitch,yaw[;...] (-UE2ViewEvery=sec) and -UE2Report. */
	void TickTestHooks(float DeltaTime);
	TWeakObjectPtr<class ACameraActor> TestCamera;
	float TestViewTime = 0.f;
	float ReportTime = 0.f;
	struct FJumpTrack { float StartZ = 0.f; float MaxZ = 0.f; bool bAir = false; };
	TMap<TWeakObjectPtr<AActor>, FJumpTrack> Jumps;
	void TrackActor(AActor* Actor);
	void UpdateActor(AActor* Actor, FTracked& T, float DeltaTime);
	void UpdateView(float DeltaTime);
};

/** UE2 falling physics shared by actors that move themselves (projectiles without a movement component, thrown items). */
namespace ArenaWorldZones
{
	/**
	 * UE2 AActor.NewFallVelocity + physFalling's TerminalVelocity for an actor at Location: Velocity x (1 - FluidFriction x
	 * dt) in a water volume, + the volume's full Gravity vector (else the world's) x GravityScale x dt, the downward speed
	 * limited by the volume's TerminalVelocity (else the level's DefaultPhysicsVolume's). cm, cm/s.
	 */
	OPENUT2K4_API FVector NewFallVelocity(const UWorld* World, const FVector& Location, const FVector& Velocity, float GravityScale, float DeltaTime);
	/** TerminalVelocity of the physics volume at Location (else the level's DefaultPhysicsVolume), cm/s. */
	OPENUT2K4_API float GetTerminalVelocityAt(const UWorld* World, const FVector& Location);
}

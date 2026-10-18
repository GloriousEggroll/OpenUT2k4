#pragma once

#include "CoreMinimal.h"
#include "UScriptCore.h"
#include "UScriptWorld.h"
#include "Tickable.h"

class UWorld;
class AActor;
class AController;
class APawn;
class APlayerState;
class AArenaGameMode;
class AArenaCharacter;

/**
 * Cached property locations of the stock Engine classes (resolved once Engine/UnrealGame/XGame are loaded).
 * All values are UE2 units (1 UE2 unit = ArenaUE2World::Scale cm).
 */
struct FUSEngineProps
{
	bool bResolved = false;
	// Actor
	FUSPropRef Location, Rotation, Velocity, Acceleration, Owner, Tag, Event, Level, Instigator, Base, Physics, bDeleteMe,
		TimerRate, TimerCounter, bTimerLoop, Role, RemoteRole, CollisionRadius, CollisionHeight, bHidden, bStatic, bNoDelete,
		DrawScale, PhysicsVolume, LifeSpan, bGameRelevant, bScriptInitialized, XLevel;
	// LevelInfo
	FUSPropRef TimeSeconds, Game, ControllerList, PawnList, NetMode, TimeDilation, DefaultGravity, LevelTitle, GRI, bBegunPlay, NavigationPointList;
	// GameInfo
	FUSPropRef BaseMutator, GameRulesModifiers, GameReplicationInfo, bTeamGame, GoalScore, TimeLimit, MaxLives, NumBots,
		MutatorClass, bAllowWeaponThrowing, bWeaponStay, GameDifficulty, bGameEnded, GameSpeed, DefaultEnemyRosterClass;
	// Mutator
	FUSPropRef NextMutator, MutatorFriendlyName;
	// GameRules
	FUSPropRef NextGameRules;
	// Pawn
	FUSPropRef Health, HealthMax, SuperHealthMax, PawnController, PawnPRI, NextPawn, JumpZ, GroundSpeed, AirSpeed, WaterSpeed,
		AirControl, MaxFallSpeed, bCanDoubleJump, MultiJumpRemaining, MaxMultiJump, MultiJumpBoost, HeadScale, RequiredEquipment,
		ShieldStrength, DamageScaling;
	// Controller
	FUSPropRef ControllerPawn, ControllerPRI, NextController, bIsPlayer, bAdrenalineEnabled, Adrenaline, PlayerReplicationInfoClass;
	// PlayerReplicationInfo
	FUSPropRef PlayerName, Score, Deaths, PRITeam, PlayerID, bBot, Kills;
	// PhysicsVolume
	FUSPropRef Gravity;
	// Inventory / Pickup / xWeaponBase
	FUSPropRef InventoryType, WeaponType, PickupClass;

	void Resolve();
};

FUSEngineProps& USProps();

/** Link between a VM actor and a game actor (FUSObject::Link). */
struct FUSActorLink
{
	enum class EKind : uint8 { None, Pawn, Controller, PlayerState, Pickup, PhysicsVolume, GameInfo, LevelInfo, Weapon, ScriptActor };
	EKind Kind = EKind::None;
	TWeakObjectPtr<AActor> Actor;
	/** Values last written to the VM (push only what script changed). */
	int32 LastHealth = 0;
	float LastScore = 0.f;
	float LastGravityZ = 0.f;
	int32 LastMaxMultiJump = 0;
	float LastJumpZ = 0.f;
	float LastGroundSpeed = 0.f;
	float LastHeadScale = 1.f;
	float LastShield = 0.f;
	bool bLastHidden = false;
	/** The VM object runs its own script (map script actors); proxies only mirror game actors. */
	bool bRunsScript = false;
	FUSVector LastLocation;
	/** Pickup proxies: our pickup type (EArenaPickupType) and weapon class. */
	int32 PickupType = -1;
	UClass* WeaponClass = nullptr;
};

/**
 * The script world of one game world: LevelInfo, a shadow GameInfo of the stock class matching our mode, script
 * actors (mutators, game rules, map script actors) and proxies for our controllers, pawns, player states and
 * pickups. Ticked on the game thread (timers, Tick events, state code).
 */
class FUSGameWorld : public FUSWorld, public FTickableGameObject
{
public:
	FUSGameWorld(UWorld* InWorld, AArenaGameMode* InGameMode);
	virtual ~FUSGameWorld() override;

	/** Creates Level / Game / GRI. False if the runtime or the stock classes are missing. */
	bool Init(const FString& ModeId, FString& OutError);
	/** UT2004 GameInfo.AddMutator for "Package.Class" (ignored when already present). */
	bool AddMutator(const FString& ClassPath);

	// FUSWorld
	virtual FUSGameWorld* AsGameWorld() override { return this; }
	virtual FUSObject* SpawnActor(FUSClass* Class, FUSObject* Owner, FName Tag, const FUSVector* Location, const FUSRotator* Rotation, FUSObject* Spawner) override;
	virtual bool DestroyActor(FUSObject* Actor) override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FUSGameWorld, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return bReady; }
	virtual UWorld* GetTickableGameObjectWorld() const override { return World.Get(); }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

	// proxies
	FUSObject* ProxyForController(AController* C, bool bCreate = true);
	FUSObject* ProxyForPawn(APawn* P, bool bCreate = true);
	FUSObject* ProxyForPlayerState(APlayerState* PS, bool bCreate = true);
	/** The game actor behind a VM actor (null for pure script actors). */
	AActor* GetLinkedActor(FUSObject* Obj) const;
	FUSActorLink* GetLink(FUSObject* Obj) const;
	/** Refreshes VM values from the game (location, health, score ...). */
	void Pull(FUSObject* Obj);
	/** Applies VM changes to the game (health, gravity, speeds ...). */
	void Push(FUSObject* Obj);
	void PullAll();
	void PushAll();

	/** Every live VM actor (snapshot for iterators). */
	TArray<FUSObject*> GetActors() const;

	UWorld* GetUWorld() const { return World.Get(); }
	AArenaGameMode* GetGameMode() const { return GameMode.Get(); }
	FUSObject* Level = nullptr;
	FUSObject* Game = nullptr;
	FUSObject* GRI = nullptr;
	FUSObject* DefaultPhysicsVolume = nullptr;
	/** Script actors spawned this frame by Spawn (for pickup replacement). */
	TArray<FUSObject*> RecentSpawns;
	/** Mutator class paths that were added. */
	TArray<FString> MutatorNames;
	/** Time spent in script this match (seconds) and calls, for the performance report. */
	double ScriptSeconds = 0.0;
	uint64 TickCount = 0;
	struct FStats { int32 ModifyPlayer = 0, NetDamage = 0, HealthChangedByRules = 0, ScoreKill = 0, Timers = 0, HealthPushed = 0; } Stats;

	/** Instantiates the map's own script actors (classes defined by the map or custom packages). Returns how many. */
	int32 SpawnMapScriptActors(const FString& InMapName);
	/** VM object of a game actor (proxy or script actor body). */
	FUSObject* ObjectForActor(AActor* A) const;
	/** Level.ControllerList / PawnList maintenance. */
	void LinkController(FUSObject* C, bool bAdd);
	void LinkPawn(FUSObject* P, bool bAdd);
	/** Map name (Level.Title / GetURLMap). */
	FString MapName;

	/** Calls an event by name on an object if it exists (no parameters). */
	void CallEvent(FUSObject* Obj, const TCHAR* EventName);

	static FUSGameWorld* Find(const UWorld* World);
	static FUSGameWorld* GetOrCreate(UWorld* World, AArenaGameMode* GM, const FString& ModeId);

private:
	FUSObject* MakeProxy(FUSClass* Class, AActor* Actor, FUSActorLink::EKind Kind);
	void RemoveProxy(FUSObject* Obj);
	void BeginPlayEvents(FUSObject* Actor);
	FUSClass* LoadClass(const TCHAR* Path);

	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AArenaGameMode> GameMode;
	TMap<TWeakObjectPtr<AActor>, FUSObject*> Proxies;
	TMap<FUSObject*, FUSActorLink*> Links;
	bool bReady = false;
	float LastWorldGravity = 0.f;
	float LastTimeDilation = 0.f;
	float DefaultTimeDilation = 1.1f;
	float BaseWorldGravityZ = 0.f;
};

/** UE2 unit conversions (VM values are UE2 units). */
namespace USUnits
{
	FUSVector ToUE2(const FVector& V);
	FVector FromUE2(const FUSVector& V);
	FUSRotator ToUE2(const FRotator& R);
	FRotator FromUE2(const FUSRotator& R);
}

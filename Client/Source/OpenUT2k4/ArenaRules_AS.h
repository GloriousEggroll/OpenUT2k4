#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaAS_Turret.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaAS_Objective.h"
#include "ArenaRules_AS.generated.h"

class AArenaAS_Info;
class APlayerStart;

/**
 * Assault: one team attacks a chain of objectives (reach / hold / destroy / use) under a round time limit
 * while the other defends. Then the sides swap and the second team must beat the first team's time (or,
 * if the first team failed, just complete the chain). The better attack wins the pair; team score = pairs
 * won. A pair where both attacks fail is decided by how far each team got; a full draw plays one overtime
 * pair, then ends tied.
 *
 * AS- maps read raw_markers.assault_objectives (or game_objectives; else the builder's Objective0..N
 * anchors), spawn groups from raw_markers.player_starts + spawn_managers.
 *
 * Options: ?RoundTime=<seconds> overrides the round length (default: ?Time= minutes, 5 by default);
 * ?StartStage=<n> (testing) starts every attack at stage n.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_AS : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// identity / defaults
	virtual bool IsTeamGame() const override { return true; }
	virtual float GetKillAdrenaline() const override { return 2.f; }
	virtual float GetSpawnProtectionTime() const override { return 3.f; }
	virtual int32 GetDefaultGoalScore() const override { return 1; }
	virtual int32 GetDefaultNumBots() const override { return 7; }
	virtual int32 GetDefaultTimeLimit() const override { return 10; }
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:AS-Convoy"); }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;
	virtual FString GetGoalLabel() const override { return TEXT("Rounds to Win"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 1; OutMax = 5; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;

	// lifecycle
	virtual void InitRules(AArenaGameMode* InGameMode, const FString& Options) override;
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void InitGameState(AArenaGameState* GS) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void OnStartPlay() override;
	virtual void OnRoundStart(int32 Round) override;
	virtual void OnRoundReset(int32 Round) override;
	virtual bool ShouldSwapSides(int32 Round) const override { return Round >= 2; }
	virtual void Tick(float DeltaSeconds) override;

	// spawning
	virtual AActor* ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates) override;

	// scoring
	virtual void CheckEndConditions() override {}
	virtual void OnTimeLimitReached() override;
	virtual bool OnPlayerUse(AController* Controller) override;

	// bots
	virtual bool AreScriptedDoorsPassable() const override { return true; }
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// HUD
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const override;
	virtual FString GetScoreboardHeaderExtra(const AArenaGameState* GS) const override;

	/** Bot objective tags: Tag_Attack + index, Tag_Guard + index, Tag_Intercept + index. */
	enum EObjectiveTag : int32
	{
		Tag_Attack = 1000,
		Tag_Guard = 2000,
		Tag_Intercept = 3000,
		/** Board an Assault vehicle (+ its index in AssaultVehicles) / man a turret. */
		Tag_Vehicle = 4000,
		Tag_Turret = 5000,
		/** Open the door / call the mover that keeps the attackers from the objective (see FMoverStall). */
		Tag_Mover = 6000
	};
	/** Drivers of our vehicles on an Assault map: the current objective of their team (UArenaGameRules hook). */
	virtual bool GetVehicleDriveDestination(AController* Driver, const FVector& From, FVector& OutDestination) override;
	virtual bool GetVehicleSiegeTarget(int32 Team, const FVector& Eye, float Range, AActor*& OutTarget, FVector& OutPoint) override;
	virtual void OnPawnSpawned(AArenaCharacter* Character) override;

	// Team attacking this round (UT2004 Trigger_ASTeam asks it too).
	int32 GetAttackingTeam() const;

protected:
	/** One objective as read from a map, before spawning. */
	struct FObjectiveDef
	{
		EArenaASObjectiveType Type = EArenaASObjectiveType::Touch;
		FVector Location = FVector::ZeroVector;
		float Yaw = 0.f;
		int32 Stage = 0;
		bool bOptional = false;
		float Radius = 200.f;
		float Duration = 5.f;
		float Health = 600.f;
		FString SourceClass;
		/** UT2004 actor name of the objective (its mesh / placement in the map json). */
		FString SourceActor;
		/** UT2004 needs a vehicle to get there (bMustBoardVehicleFirst, a vehicle path). */
		bool bNeedsVehicle = false;
	};
	/** Objectives (by index) that need a vehicle in UT2004 (we spawn none): dropped when the paths do not reach them. */
	TSet<int32> VehicleObjectives;

	/** A set of spawn points that belong together (one side, one forward position, one imported spawn group). */
	struct FSpawnGroup
	{
		TArray<TWeakObjectPtr<APlayerStart>> Starts;
		FVector Centroid = FVector::ZeroVector;
		/** 0 = attackers, 1 = defenders, -1 = either. */
		int32 Role = -1;
		/** Enabled when the round starts (imported spawn managers / the attackers' home side). */
		bool bInitial = false;
		FString Name;
		/** UE2 PlayerSpawnManager ForcedPawnClass (AS-MotherShip's space fighters): spawn in one of our flying vehicles. */
		bool bSpawnInFighter = false;
		/** The spawn manager's Tag: the map's script toggles it (PlayerSpawnManager.Trigger flips bEnabled). */
		FName Tag;
		/** Enabled right now (bInitial at round start, then toggled by the map's events). */
		bool bEnabledNow = false;
	};
	/** Fighters made for players spawning in space (removed when left empty). */
	TArray<TWeakObjectPtr<AArenaONS_Vehicle>> SpawnedFighters;
	/** Player starts made for the space spawn points (our level builder places starts on floors only). */
	UPROPERTY() TArray<TObjectPtr<APlayerStart>> FighterStarts;

	bool ReadImportedObjectives(AArenaLevelBuilder* Builder, TArray<FObjectiveDef>& OutDefs);
	/** Fallback without usable assault markers: the builder's Objective0..N anchors (+ a bonus objective at ControlA). */
	void BuildAnchorObjectives(AArenaLevelBuilder* Builder, TArray<FObjectiveDef>& OutDefs);
	void SpawnObjectives(const TArray<FObjectiveDef>& Defs);
	FString MakeDescription(EArenaASObjectiveType Type, int32 Stage, bool bOptional, int32 Salt) const;

	/** Spawn groups and the group used per stage (lazy: needs the nav graph). */
	void EnsureSpawnGroups();
	void BuildImportedSpawnGroups();
	void AssignStageSpawnGroups();
	/** Imported maps: drop stages no attacker spawn can reach (they depend on movers / scripts we do not run). */
	void DropUnreachableStages();
	/** bDoorsOpen: through the doors the map's script opens later in the round (closed now). */
	bool IsReachable(const FVector& From, const FVector& To, bool bDoorsOpen = false);
	FVector GetStageLocation(int32 InStage) const;

	void ApplyRolesAndStage();
	void SetStage(int32 NewStage);
	void OnObjectiveCompleted(AArenaAS_Objective* Obj);
	/** UE2 events (ArenaUE2World): TriggeredObjectives complete when their Tag fires. */
	void OnUE2Event(FName Event, AActor* Other, APawn* EventInstigator, bool bTrigger);
	FDelegateHandle UE2EventHandle;
	/** Attack over (completed / time out). */
	void FinishAttack(bool bSuccess);
	/** Completed stages + partial progress on the current stage. */
	float GetAttackProgressScore() const;
	void UpdateBotDestroyFire(float DeltaSeconds);
	void StopBotFire(AArenaBotController* Bot);
	AArenaAS_Info* FindInfo() const;
	void GatherObjectives(TArray<AArenaAS_Objective*>& Out) const;

	UPROPERTY() TArray<TObjectPtr<AArenaAS_Objective>> Objectives;

	/**
	 * The map's Assault vehicle factories (ASVehicleFactory*): turrets / sentinels (AArenaAS_Turret, from the stock
	 * class defaults) and vehicles (our AArenaONS_Vehicle types for the ONS classes the factories name). RelTeam: 1 the
	 * defenders (UE2 VehicleTeam), 0 the attackers. A sentinel whose VehicleEvent is a DestroyVehicleObjective's tag
	 * is that objective's vehicle (damage to it is the objective's).
	 */
	struct FAssaultVehicle
	{
		FTransform Transform;
		bool bTurret = true;
		FArenaASTurretDesc Turret;
		EArenaONS_VehicleType Type = EArenaONS_VehicleType::Bulwark;
		int32 RelTeam = 1;
		FString VehicleEvent;
		FString Source;
		TWeakObjectPtr<APawn> Pawn;
		float RespawnAt = 0.f;
		TWeakObjectPtr<AArenaAS_Objective> Objective;
	};
	TArray<FAssaultVehicle> AssaultVehicles;

	/** UE2 UseTrigger (no native actor of its own): Use within its radius fires Event. */
	struct FUseTrigger
	{
		FVector Location = FVector::ZeroVector;
		float Radius = 150.f;
		FName Event;
	};
	TArray<FUseTrigger> UseTriggers;
	bool FireUseTrigger(const FVector& At, AController* By);

	/**
	 * The attackers make no progress towards the current objective (no attacker got 3 m nearer in 20 s): the path from
	 * the nearest of them crosses a closed mover. What opens it (UE2 bots do the same): a proximity trigger to touch, a
	 * UseTrigger to use, a shootable trigger to shoot; else a spot before the mover to wait at until it opens.
	 */
	struct FMoverStall
	{
		int32 ObjectiveIndex = INDEX_NONE;
		float BestDist = TNumericLimits<float>::Max();
		float LastProgress = 0.f;
		/** 0 none, 1 touch, 2 use, 3 shoot, 4 wait */
		int32 Kind = 0;
		FVector GoalLocation = FVector::ZeroVector;
		TWeakObjectPtr<AActor> Opener;
		TWeakObjectPtr<class AArenaMover> Mover;
		float Since = 0.f;
	};
	FMoverStall Stall;
	void UpdateMoverStall(float Now);
	void ReadAssaultVehicles(const FString& MapName);
	/** (Re)spawns every vehicle / turret, with the current roles. */
	void SpawnAssaultVehicle(int32 Index);
	void ResetAssaultVehicles();
	void TickAssaultVehicles(float Now);
	bool HasAssaultVehicles(bool bForAttackers) const;
	void GetBotVehicleObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals);
	UPROPERTY() TObjectPtr<AArenaAS_Info> Info;

	/** Map side (0 / 1) whose team attacks in the first round of a pair. */
	int32 AttackSide = 0;
	int32 NumStages = 0;
	int32 CurrentStage = 0;
	int32 RoundSeconds = 300;
	/** Testing: ?StartStage=N skips the first N stages each round. */
	int32 DebugStartStage = 0;
	float RoundStartTime = 0.f;
	bool bAttackFinished = false;

	// pair bookkeeping (mirrored to Info)
	int32 PairRound = 0;
	int32 PairNumber = 1;
	int32 FirstAttackTeam = -1;
	bool bFirstSucceeded = false;
	float FirstTime = 0.f;
	float FirstProgress = 0.f;
	int32 FirstStages = 0;
	bool bOvertimeUsed = false;
	bool bOvertimePair = false;

	// spawns
	TArray<FSpawnGroup> SpawnGroups;
	TArray<int32> AttackerGroupForStage;
	TArray<int32> DefenderGroupForStage;
	bool bSpawnGroupsBuilt = false;
	bool bSpawnStagesAssigned = false;
	TMap<FString, bool> ReachCache;
	// Flyers (AS-MotherShip's fighters), per destroy objective: spots in the open with a clear shot at it (the first
	// NumSiege points) and approach points on wider shells, with each point's straight-line flight distance to a spot.
	struct FSiegeGraph
	{
		TArray<FVector> Points;
		TArray<float> CostToSpot;
		int32 NumSiege = 0;
	};
	TMap<int32, FSiegeGraph> SiegeSpots;
	/** UT2004 objective MoverTag by the objective's map actor: the mover the objective works, triggered when it completes. */
	TMap<FString, FName> ObjectiveMoverTags;
	/** The map's script has toggled a spawn manager this round (spawns then follow the managers, not the stage table). */
	bool bSpawnsToggled = false;
	bool FindSiegeSpot(const class AArenaAS_Objective* Obj, const FVector& From, const AActor* Flyer, FVector& OutSpot);

	/** Imported: original markers we could not map, for the log. */
	TArray<FString> SkippedNotes;

	/** Bots currently firing at a destroy objective because we told them to. */
	TSet<TWeakObjectPtr<AArenaBotController>> FiringBots;
	TMap<TWeakObjectPtr<AArenaBotController>, float> BotAttackTime;
	float StatusAccum = 0.f;

	// HUD cache (client side too)
	mutable TArray<TWeakObjectPtr<AArenaAS_Objective>> HudObjectives;
	mutable TWeakObjectPtr<AArenaAS_Info> HudInfo;
	mutable float HudRefreshTime = -100.f;
};

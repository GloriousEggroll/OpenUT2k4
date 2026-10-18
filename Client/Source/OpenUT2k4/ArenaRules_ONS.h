#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaRules_ONS.generated.h"

class AArenaONS_PowerNode;
class AArenaONS_PowerLink;
class AArenaONS_VehicleFactory;
class AArenaONS_Vehicle;
class APlayerStart;

/**
 * Onslaught: two power cores joined by a network of power nodes. Build nodes linked to your network, destroy the
 * enemy's, and bring down the enemy core once one of its neighbours is yours. Vehicles spawn at team bays.
 * Team score = cores destroyed (goal 1 by default; higher goals play rounds).
 */
UCLASS()
class OPENUT2K4_API UArenaRules_ONS : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// ---- identity ----
	virtual bool IsTeamGame() const override { return true; }
	virtual float GetKillAdrenaline() const override { return 2.f; }
	virtual int32 GetDefaultGoalScore() const override { return 3; }
	virtual int32 GetDefaultNumBots() const override { return 9; }
	virtual int32 GetDefaultTimeLimit() const override { return 20; }
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:ONS-Torlan"); }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;
	virtual FString GetGoalLabel() const override { return TEXT("Points to Win"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 1; OutMax = 10; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;

	// ---- lifecycle ----
	virtual void InitRules(AArenaGameMode* InGameMode, const FString& Options) override;
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void OnStartPlay() override;
	virtual void OnMatchStart() override;
	virtual void OnRoundEnd(int32 Round, int32 WinningTeam) override;
	virtual void OnRoundReset(int32 Round) override;
	virtual bool ShouldSwapSides(int32 Round) const override { return Round >= 2; }
	virtual void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPlayerLeft(AController* Controller) override;

	// ---- spawning / combat ----
	virtual AActor* ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates) override;
	virtual void ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const override;
	virtual void CheckEndConditions() override;
	virtual void OnTimeLimitReached() override;
	virtual bool OnPlayerUse(AController* Controller) override;

	// ---- bots ----
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// ---- HUD ----
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	/** Health meters over the power nodes / cores in view (HUD art; the art nodes have no world-space bar). */
	void DrawNodeHealth(AArenaHUD* HUD, UCanvas* Canvas);
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const override;
	virtual FString GetDeadScreenMessage(const AArenaPlayerState* Player) const override;

	// ---- network events (from the node actors, server) ----
	void NotifyConstructionStarted(AArenaONS_PowerNode* Node, AController* By);
	void NotifyNodeBuilt(AArenaONS_PowerNode* Node, AController* By);
	void NotifyNodeDestroyed(AArenaONS_PowerNode* Node, int32 OldTeam, AController* By);
	void NotifyCoreDamaged(AArenaONS_PowerNode* Core, float Damage, AController* By);
	void NotifyCoreDestroyed(AArenaONS_PowerNode* Core, AController* By);
	/** Recompute attackable / buildable flags of every node. */
	void RefreshNetwork();

	// ---- queries ----
	const TArray<TObjectPtr<AArenaONS_PowerNode>>& GetNodes() const { return Nodes; }
	AArenaONS_PowerNode* GetCore(int32 Team) const;
	bool IsNodeAttackableBy(const AArenaONS_PowerNode* Node, int32 Team) const;
	bool IsNodeBuildableBy(const AArenaONS_PowerNode* Node, int32 Team) const;
	/** Hops from the node to the nearest node not owned by Team (0 = the node itself is not owned). */
	int32 GetFrontDistance(const AArenaONS_PowerNode* Node, int32 Team) const;

	/** Destination for a vehicle driver (bots): the node objective of its team. Returns false if none. */
	bool GetDriveDestination(AController* Driver, const FVector& From, FVector& OutDest, AArenaONS_PowerNode*& OutNode, bool& bOutAttack);
	/** Queue a character to enter a vehicle on the next rules tick (bots). */
	void QueueVehicleEntry(AArenaCharacter* Char, AArenaONS_Vehicle* Vehicle);
	/** A bot left a vehicle: don't send it back to one for a while. */
	void NotifyLeftVehicle(AController* C, AArenaONS_Vehicle* Vehicle, bool bGaveUp);
	/** Vehicle registry (factories and vehicles call these). */
	void RegisterVehicle(AArenaONS_Vehicle* Vehicle);
	const TArray<TWeakObjectPtr<AArenaONS_Vehicle>>& GetVehicles() const { return Vehicles; }
	float GetKillZ() const;

protected:
	/** One parsed objective (core / node) before actors exist. */
	struct FONSNodeDef
	{
		FString Name;
		FVector Location = FVector::ZeroVector;
		float Yaw = 0.f;
		bool bCore = false;
		int32 Team = -1;
	};
	struct FONSFactoryDef
	{
		EArenaONS_VehicleType Type = EArenaONS_VehicleType::Dustrunner;
		FVector Location = FVector::ZeroVector;
		float Yaw = 0.f;
		float RespawnTime = 15.f;
		/** Explicit owner node name (from the importer) or empty. */
		FString OwnerName;
		FString SourceClass;
	};

	bool ParseImported(AArenaLevelBuilder* Builder, TArray<FONSNodeDef>& OutNodes, TArray<TPair<FString, FString>>& OutLinks, TArray<FONSFactoryDef>& OutFactories);
	void BuildLinksByProximity(TArray<TPair<int32, int32>>& OutLinks) const;
	void ComputeNodePoints();
	void SpawnFactories();
	void ResetNetwork();
	void UpdateStatus();
	void ProcessVehicleEntries();
	void TickBotSiege();
	void TickDebugView();
	/** Best node objective for a team member at Location. */
	bool ChooseNodeObjective(int32 Team, const FVector& Location, uint32 Seed, bool bDefender, FArenaBotObjective& Out) const;
	AArenaONS_Vehicle* FindVehicleForBot(AArenaBotController* Bot, const FVector& Location, float MaxDist, const FVector& Destination) const;
	AArenaONS_Vehicle* FindUsableVehicle(AArenaCharacter* Char, float MaxDist) const;
	void EndWithCoreDestroyed(int32 WinnerTeam, AArenaONS_PowerNode* Core);
	/** Core team after side swaps: the team that plays on the core's side. */
	int32 GetCoreTeamForSide(int32 Side) const;
	void LogNetwork() const;

	UPROPERTY() TArray<TObjectPtr<AArenaONS_PowerNode>> Nodes;
	UPROPERTY() TArray<TObjectPtr<AArenaONS_PowerLink>> LinkActors;
	UPROPERTY() TArray<TObjectPtr<AArenaONS_VehicleFactory>> Factories;
	UPROPERTY() TArray<TObjectPtr<APlayerStart>> SpawnStarts;
	TArray<TWeakObjectPtr<AArenaONS_Vehicle>> Vehicles;
	TArray<FONSFactoryDef> FactoryDefs;
	/** Side (0 / 1) of each core node index. */
	TMap<int32, int32> CoreSide;
	TMap<TObjectKey<APlayerStart>, int32> StartNode;
	mutable bool bPointsComputed = false;
	FString LinkSource;

	struct FPendingEntry
	{
		TWeakObjectPtr<AArenaCharacter> Char;
		TWeakObjectPtr<AArenaONS_Vehicle> Vehicle;
	};
	TArray<FPendingEntry> PendingEntries;
	/** Vehicle -> reserving bot and expiry time. */
	TMap<TWeakObjectPtr<AArenaONS_Vehicle>, TPair<TWeakObjectPtr<AController>, float>> Reservations;
	/** Bot -> time until which it walks. */
	TMap<TWeakObjectPtr<AController>, float> NoVehicleUntil;
	/** Bots currently firing at a node: last siege tick. */
	TMap<TWeakObjectPtr<AArenaBotController>, float> SiegeBots;
	float NextStatusTime = 0.f;
	float NextNetworkLog = 0.f;
	float NextDebugViewTime = 0.f;
	TWeakObjectPtr<AActor> DebugViewTarget;
	bool bCoreDestroyedThisRound = false;
	/** Stats for the test log */
	int32 NumNodesBuilt = 0;
	int32 NumNodesDestroyed = 0;
	int32 NumVehicleEntries = 0;
};

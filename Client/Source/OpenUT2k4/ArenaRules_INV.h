#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaINV_Monster.h"
#include "ArenaRules_INV.generated.h"

class AArenaINV_Monster;
class AArenaINV_MonsterController;
class AArenaINV_WaveState;

/**
 * Invasion: every human and bot is on one side against waves of AI monsters (AArenaINV_Monster).
 * - Waves grow in size and toughness; a countdown separates them. Goal = number of waves (default 16).
 * - One life per wave: the fallen spectate a living team mate and return for the next wave.
 *   If the whole team is down, the invasion wins.
 * - Points per monster kill, scaled by monster type. Team total in GS->TeamScores[0].
 * - The monsters of each wave follow UT2004's Invasion wave table (ArenaINV::GetWave); the last waves bring
 *   Cindermaw bosses, the 16th only them.
 * - Monsters spawn at hidden, far-away path nodes / player starts / nav nodes.
 *
 * URL options: ?WaveDelay=<seconds between waves> (default 12), ?StartWave=<first wave> (default 1).
 */
UCLASS()
class OPENUT2K4_API UArenaRules_INV : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// ---- identity ----
	virtual bool IsTeamGame() const override { return false; }
	virtual int32 GetDefaultGoalScore() const override { return 16; }
	virtual int32 GetDefaultNumBots() const override { return 3; }
	virtual int32 GetDefaultTimeLimit() const override { return 0; }
	virtual FString GetGoalLabel() const override { return TEXT("Waves"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 1; OutMax = 40; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;
	virtual FString GetFallbackModeId() const override { return TEXT("DM"); }
	/** An open outdoor arena gives the monster waves room (any DM-capable map works). */
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:DM-Antalus"); }

	// ---- lifecycle ----
	virtual void InitRules(AArenaGameMode* InGameMode, const FString& Options) override;
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void InitGameState(AArenaGameState* GS) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void OnStartPlay() override;
	virtual void OnMatchStart() override;
	virtual void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPlayerJoined(AController* Controller) override;

	// ---- spawning ----
	virtual AActor* ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates) override;
	virtual bool CanRespawn(AController* Controller) const override;
	virtual float GetForcedRespawnDelay(AController* Controller) const override;
	virtual void ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const override;
	virtual bool CanPickUp(AArenaCharacter* Character, AArenaPickup* Pickup) const override;

	// ---- combat ----
	virtual float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim) const override;
	virtual bool AreOnSameTeam(const AController* A, const AController* B) const override;
	virtual bool ScoreKill(AController* Killer, AController* Victim, bool bHeadshot) override;
	virtual void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot) override;
	virtual void CheckEndConditions() override;
	virtual void OnTimeLimitReached() override;

	// ---- bots ----
	virtual bool GetBotObjective(AArenaBotController* Bot, FArenaBotObjective& OutGoal) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// ---- HUD ----
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const override;
	virtual FString GetScoreboardHeaderExtra(const AArenaGameState* GS) const override;
	virtual FString GetDeadScreenMessage(const AArenaPlayerState* Player) const override;

	static bool IsMonsterController(const AController* C);

protected:
	enum class EWavePhase : uint8 { PreMatch, Countdown, Active, Done };

	void StartWave(int32 NewWave);
	void FinishWave();
	void BuildWaveQueue(int32 InWave);
	bool TrySpawnMonster(EArenaINVMonsterType Type);
	/** Best spawn floor point for a monster of this size (hidden from and far from players when possible). */
	bool PickSpawnPoint(EArenaINVMonsterType Type, FVector& OutFloor, float& OutYaw);
	void GatherSpawnCandidates();
	void RespawnFallenPlayers();
	void UpdateSpectators(float Now);
	void RecallStragglers(float Now);
	void UpdateWaveState();
	/** Controllers of the human side (humans and bots), with player states. */
	void GetTeamControllers(TArray<AController*>& Out) const;
	int32 CountAlivePlayers(int32* OutTotal = nullptr) const;
	TArray<FVector> GetLivingPlayerEyes() const;
	int32 GetNumWaves() const;

	EWavePhase Phase = EWavePhase::PreMatch;
	int32 Wave = 0;
	float NextWaveTime = 0.f;
	float WaveDelay = 12.f;
	/** ?StartWave=N (testing / practice): first wave played. */
	int32 StartWaveOption = 1;
	float NextSpawnTime = 0.f;
	float LastKillTime = 0.f;
	float WaveStartTime = 0.f;
	int32 WaveTotal = 0;
	int32 MaxAlive = 12;
	TArray<EArenaINVMonsterType> SpawnQueue;
	TArray<TWeakObjectPtr<AArenaINV_Monster>> Monsters;
	int32 MonsterSerial = 0;
	int32 TotalKills = 0;
	float LastCountdownSecond = -1.f;

	/** Floor points monsters may appear at. */
	TArray<FVector> SpawnFloors;
	bool bCandidatesGathered = false;

	/** Dead humans: when they died (spectator switch delay). */
	TMap<TWeakObjectPtr<AController>, float> DeathTimes;

	UPROPERTY() TObjectPtr<AArenaINV_WaveState> WaveState;
};

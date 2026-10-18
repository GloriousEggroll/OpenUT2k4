#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ArenaTypes.h"
#include "ArenaGameMode.generated.h"

class AArenaCharacter;
class AArenaPlayerState;
class AArenaGameState;
class AArenaLevelBuilder;
class AArenaNavGraph;
class AArenaFlag;
class AArenaWeapon;
class AArenaBotController;
class UArenaGameRules;

/**
 * The one game mode class of the arena. The mode-specific behaviour lives in a UArenaGameRules object
 * created in InitGame from ?Mode=<ModeId> (see ArenaGameRules.h and Docs/GameModes.md).
 * URL options:
 *   ?Mode=<ModeId> ?Layout=<id> ?Bots=N ?Skill=0..7 ?Goal=N ?Time=<minutes> ?Instagib=1 ?Team=0|1
 * The arena geometry is generated at runtime by AArenaLevelBuilder on /Game/Maps/Arena; imported maps
 * (/Game/Imported/Maps/<Map> with Content/ImportedData/<Map>.json) keep their own geometry and only get
 * our gameplay actors (AArenaLevelBuilder::BuildImported).
 */
UCLASS()
class OPENUT2K4_API AArenaGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AArenaGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void InitGameState() override;
	virtual void StartPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	/** Reads the player's ?Character= login option (UT2004 URL option) into the player state. */
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal = TEXT("")) override;
	virtual void Logout(AController* Exiting) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual bool ShouldSpawnAtStartSpot(AController* Player) override { return false; }
	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
	/** Spawns (or respawns) a controller's pawn. Ignored after the match ended. */
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- queries ----
	/** The active rules (never null after InitGame). */
	UArenaGameRules* GetRules() const { return Rules; }
	/** Registered id of the active mode ("DM", "CTF", ...). */
	FString GetModeId() const;
	/** Legacy enum of the active mode (compatibility only; use GetModeId / GetRules). */
	EArenaGameType GetGameType() const;
	bool IsTeamGame() const;
	bool IsInstagib() const { return bInstagib; }
	/** UE2 DeathMatch.bAllowTrans (?Translocator=0|1, default the rules' GetDefaultTranslocator): the translocator is in
	 *  the default inventory. */
	bool AllowsTranslocator() const { return bAllowTrans; }
	bool IsPlaying() const;
	bool AreOnSameTeam(const AController* A, const AController* B) const;
	AArenaLevelBuilder* GetLevelBuilder() const { return LevelBuilder; }
	/** Bot navigation graph (built in StartPlay; null before). */
	AArenaNavGraph* GetNavGraph() const { return NavGraph; }
	AArenaGameState* GetArenaGameState() const;
	int32 GetBotSkill() const { return BotSkill; }
	int32 GetNumBots() const { return NumBots; }
	int32 GetGoalScore() const { return GoalScore; }
	int32 GetTimeLimitMinutes() const { return TimeLimitMinutes; }
	const FString& GetLayoutId() const { return LayoutId; }
	/** The URL options of this match (without the leading '?'). */
	const FString& GetMatchOptions() const { return MatchOptions; }
	const TArray<TObjectPtr<AController>>& GetBots() const { return Bots; }
	/** Current round (1-based). */
	int32 GetRound() const;

	/** Starting inventory for a newly spawned character (defaults, then UArenaGameRules::ModifyDefaultInventory). */
	void GetDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& OutWeapons) const;

	// ---- events ----
	/** Called by AArenaCharacter on death. Killer may be null or == Victim (suicide). */
	void Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot);
	/** Respawn request (fire pressed when dead, or bot timer). */
	void RequestRespawn(AController* Controller);
	/** Damage scaling hook (forwards to the rules). Returns final damage. */
	float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim) const;
	/** Use key / bot use request (forwards to the rules). */
	bool PlayerUse(AController* Controller);

	/** CTF hooks from AArenaFlag (forwarded to UArenaRules_CTF when active) */
	void FlagTaken(AArenaFlag* Flag, AController* By);
	void FlagDropped(AArenaFlag* Flag, AController* By);
	void FlagReturned(AArenaFlag* Flag, AController* By);
	void FlagCaptured(AArenaFlag* Flag, AController* By);

	// ---- API for game rules ----
	/** End the match: phase Ended, winner text / team / player, back to the menu after 20 s. */
	void EndMatch(const FString& Winner, int32 WinTeam, APlayerState* WinPlayer);
	/** Ask the rules whether the match (or round) is over. Call after anything that changes scores. */
	void CheckEndConditions();
	/** Add to a team's score (0 / 1). The result never goes below 0. */
	void AddTeamScore(int32 Team, int32 Delta);
	void SetTeamScore(int32 Team, int32 Score);
	/** Default kill scoring (frags, suicides, team kills, sprees, multi kills, first blood, team score if KillsAddTeamScore). */
	void ScoreKillDefault(AController* Killer, AController* Victim, bool bHeadshot);
	/** Broadcast a centre-screen message to all human players. */
	void BroadcastCenterMessage(const FString& Text, const FLinearColor& Color, float Duration = 3.f, FName Sound = NAME_None);
	/** Centre-screen message for one team's human players. */
	void BroadcastTeamMessage(int32 Team, const FString& Text, const FLinearColor& Color, float Duration = 3.f, FName Sound = NAME_None);
	/** Centre-screen message for one controller (ignored for bots). */
	void SendCenterMessage(AController* To, const FString& Text, const FLinearColor& Color, float Duration = 3.f);
	/** Move a controller to a team (0 / 1, or ArenaTeam::None) and recolour its pawn. */
	void SetPlayerTeam(AController* Controller, int32 Team);
	/** Side (0 = red base, 1 = blue base) a team currently plays on; differs from Team after a side swap. */
	int32 GetTeamSide(int32 Team) const;
	/** Add a bot (Index selects the name). PreferredTeam -1 = balance. Returns the new bot (already spawned). */
	AArenaBotController* AddBot(int32 Index, int32 PreferredTeam = -1);
	/** Respawn timer: may respawn after DelaySeconds (bots, and humans after the forced delay, respawn automatically). */
	void ScheduleRespawn(AController* Controller, float DelaySeconds);
	/** Drop a pending respawn (e.g. eliminated players). */
	void CancelRespawn(AController* Controller);
	/** Destroy the controller's pawn (if any) without scoring it; carried objects are dropped. */
	void RemovePawn(AController* Controller);
	/**
	 * End the current round: phase RoundOver for IntermissionSeconds, then all pawns are removed,
	 * UArenaGameRules::ShouldSwapSides / OnRoundReset run, everyone respawns and a 3 s countdown starts the next round.
	 */
	void EndRound(int32 WinTeam, const FString& Message, float IntermissionSeconds = 6.f);
	/** Start the next round now (normally called automatically after EndRound's intermission). */
	void StartNextRound();
	/** Replicated mode status lines (drawn by the default UArenaGameRules::DrawHUD). */
	void SetModeStatusLines(const TArray<FString>& Lines);

	/** Return everyone to the main menu. */
	void ReturnToMenu();
	/** Restart the same match with the same options. */
	void RestartMatch();

protected:
	void BalanceTeam(AArenaPlayerState* PS, int32 PreferredTeam);
	void StartMatch();
	void CheckTimeLimit();
	void TickAutomation(float Now);
	/** Create Rules for ModeId and call InitRules. */
	void CreateRules(const FString& ModeId, const FString& Options);

	UPROPERTY() TObjectPtr<UArenaGameRules> Rules;
	FString LayoutId;
	FString MatchOptions;
	int32 NumBots = 5;
	/** Set by InitGame when this level cannot host the match (cooked map level to open instead, or the menu). */
	FString PendingTravelLevel;
	FString PendingTravelOptions;
	int32 BotSkill = 3;
	int32 GoalScore = 25;
	int32 TimeLimitMinutes = 15;
	bool bInstagib = false;
	bool bAllowTrans = false;
	int32 PreferredHumanTeam = 0;
	float PhaseTimer = 0.f;
	float SecondAccum = 0.f;
	float EndTime = 0.f;
	float RoundOverEndTime = 0.f;
	bool bMatchStarted = false;

	UPROPERTY() TObjectPtr<AArenaLevelBuilder> LevelBuilder;
	UPROPERTY() TObjectPtr<AArenaNavGraph> NavGraph;
	UPROPERTY() TArray<TObjectPtr<AController>> Bots;

	/** Controllers waiting to respawn and when they may/must. */
	TMap<TWeakObjectPtr<AController>, float> RespawnTimes;
	/** Extra wait after RespawnTimes before a waiting controller is respawned automatically. */
	TMap<TWeakObjectPtr<AController>, float> ForcedRespawnDelays;
};

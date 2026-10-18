#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_LMS.generated.h"

class AArenaLMS_Beacon;
class APlayerController;

/**
 * Last Man Standing: free-for-all with a fixed number of lives (the goal value). Everyone spawns fully armed
 * with 100 health / 100 shield; pickups are off except adrenaline (?LMSPickups=1 keeps all of them). Running out
 * of lives eliminates you (spectate the survivors, FIRE cycles). The last player alive wins; at the time limit
 * the most lives (then kills) wins. Players staying within CampRadius for CampTime seconds are exposed with a
 * beacon and hunted by bots (?CampTime=<s>, ?CampRadius=<cm>, ?CampTime=0 disables it).
 *
 * PlayerState use: Lives, bEliminated, ModeFlags & FlagExposed.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_LMS : public UArenaGameRules
{
	GENERATED_BODY()

public:
	/** ModeFlags bit: currently exposed for camping. */
	static constexpr int32 FlagExposed = 1 << 4;

	// ---- identity ----
	virtual EArenaGameType GetLegacyGameType() const override { return EArenaGameType::Deathmatch; }
	virtual int32 GetDefaultGoalScore() const override { return 3; }
	virtual int32 GetDefaultTimeLimit() const override { return 0; }
	virtual FString GetGoalLabel() const override { return TEXT("Lives"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 1; OutMax = 20; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;

	// ---- lifecycle ----
	virtual void InitRules(AArenaGameMode* InGameMode, const FString& Options) override;
	virtual void OnMatchStart() override;
	virtual void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPlayerJoined(AController* Controller) override;
	virtual void OnPlayerLeft(AController* Controller) override;

	// ---- spawning ----
	virtual bool CanRespawn(AController* Controller) const override;
	virtual void OnPawnSpawned(AArenaCharacter* Character) override;
	virtual void ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const override;
	virtual bool AllowPickup(EArenaPickupType Type, TSubclassOf<AArenaWeapon> WeaponClass) const override;

	// ---- combat ----
	virtual void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot) override;
	virtual void CheckEndConditions() override;
	virtual void OnTimeLimitReached() override;

	// ---- bots ----
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// ---- HUD ----
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const override;
	virtual FString GetScoreboardHeaderExtra(const AArenaGameState* GS) const override;
	virtual FString GetDeadScreenMessage(const AArenaPlayerState* Player) const override;
	virtual bool ShouldDrawDeadScreen(APlayerController* PC) const override;

protected:
	/** Lives everyone starts with (goal value, at least 1). */
	int32 GetStartLives() const;
	/** Players taking part (have a lives count, not pure spectators). */
	TArray<AArenaPlayerState*> GetParticipants() const;
	/** Best by lives, then kills, then fewer deaths. */
	static AArenaPlayerState* PickLeader(const TArray<AArenaPlayerState*>& Players);
	void UpdateCamping(float Now);
	void SetExposed(AArenaCharacter* Character, AArenaPlayerState* PS, bool bExposed);
	void UpdateSpectators(float Now);
	AArenaCharacter* FindNextAlive(const AArenaCharacter* After) const;

	struct FCampState
	{
		FVector Anchor = FVector::ZeroVector;
		float AnchorTime = 0.f;
		bool bWarned = false;
		bool bExposed = false;
		TWeakObjectPtr<AArenaPlayerState> PlayerState;
		TWeakObjectPtr<AArenaLMS_Beacon> Beacon;
	};
	TMap<TWeakObjectPtr<AArenaCharacter>, FCampState> CampStates;
	/** When each eliminated human was knocked out (spectator camera switch delay). */
	TMap<TWeakObjectPtr<AController>, float> EliminatedAt;

	bool bAllowAllPickups = false;
	float CampTime = 30.f;
	float CampRadius = 600.f;
	float StatusAccum = 0.f;
};

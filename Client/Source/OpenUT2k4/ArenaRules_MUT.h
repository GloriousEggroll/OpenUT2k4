#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_MUT.generated.h"

class AArenaMUT_Aura;

/**
 * Mutant: free-for-all where the first kill makes the killer the Mutant (full health and shield, double damage,
 * extra speed, regeneration, every weapon, a glowing aura, shown to everyone on the HUD). Killing the Mutant makes
 * you the Mutant. The Mutant scores a point per kill; everyone else only scores by killing the Mutant, except the
 * Bottom Feeder (lowest score) who may hit and score off anyone. Non-mutants can't hurt each other unless one of
 * them is the Bottom Feeder (AreOnSameTeam / ModifyDamage).
 *
 * PlayerState use: ModeFlags (FlagMutant / FlagBottomFeeder), ObjectiveScore = kills made as the Mutant.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_MUT : public UArenaGameRules
{
	GENERATED_BODY()

public:
	static constexpr int32 FlagMutant = 1 << 0;
	static constexpr int32 FlagBottomFeeder = 1 << 1;

	// ---- identity ----
	virtual EArenaGameType GetLegacyGameType() const override { return EArenaGameType::Deathmatch; }
	virtual int32 GetDefaultGoalScore() const override { return 20; }
	virtual FString GetGoalLabel() const override { return TEXT("Score Limit"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 5; OutMax = 100; OutStep = 5; }
	virtual FString GetGoalText(int32 Goal) const override;

	// ---- lifecycle ----
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPlayerLeft(AController* Controller) override;
	virtual void OnPawnSpawned(AArenaCharacter* Character) override;
	virtual void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer) override;

	// ---- combat / scoring ----
	virtual float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim) const override;
	virtual bool AreOnSameTeam(const AController* A, const AController* B) const override;
	virtual bool ScoreKill(AController* Killer, AController* Victim, bool bHeadshot) override;
	virtual void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot) override;

	// ---- bots ----
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// ---- HUD ----
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const override;
	virtual FString GetScoreboardHeaderExtra(const AArenaGameState* GS) const override;

	// ---- queries (replicated state only, usable on clients) ----
	static bool HasFlag(const AController* C, int32 Flag);
	static AArenaPlayerState* FindFlagged(const AArenaGameState* GS, int32 Flag);

protected:
	/** Turn Controller into the Mutant (the previous one loses the status). */
	void MakeMutant(AController* Controller);
	/** Nobody is the Mutant any more. */
	void ClearMutant();
	/** Recompute the Bottom Feeder (lowest score among non-mutants). */
	void UpdateBottomFeeder();
	void ApplyMutantBuffs(AArenaCharacter* Pawn, bool bInitial);

	TWeakObjectPtr<AController> Mutant;
	TWeakObjectPtr<AArenaMUT_Aura> Aura;
	float RegenAccum = 0.f;
	float FeederCheckAccum = 0.f;
};

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "ArenaPlayerState.generated.h"

UCLASS()
class OPENUT2K4_API AArenaPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Team = -1;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Kills = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Deaths = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Suicides = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Captures = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Returns = 0;
	/** Kills since last death (killing spree tracking) */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Spree = 0;
	/** Bot skill 0..7, -1 for humans */
	UPROPERTY(Replicated, BlueprintReadOnly) float BotSkill = -1.f;

	// ---- generic mode fields (meaning defined by the active game rules) ----
	/** Remaining lives (lives-based modes); -1 = unlimited / unused. */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Lives = -1;
	/** Out of the match / round (e.g. no lives left). */
	UPROPERTY(Replicated, BlueprintReadOnly) bool bEliminated = false;
	/** Objective counter (goals scored, points controlled, objectives completed, nodes built ...). */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 ObjectiveScore = 0;
	/** Mode-defined role bits (e.g. mutant / bottom feeder). */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 ModeFlags = 0;
	/** The player's character (a .upl DefaultName, UT2004's ?Character= login option); empty = a model from the pool. */
	UPROPERTY(Replicated, BlueprintReadOnly) FString CharacterName;

	/** Multi kill tracking */
	float LastKillTime = -100.f;
	int32 MultiKillCount = 0;

	void AddScore(float Delta) { SetScore(GetScore() + Delta); }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Reset() override;
};

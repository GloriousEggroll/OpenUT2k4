#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "ArenaAS_Info.generated.h"

/** Replicated Assault match state for the HUD (spawned by UArenaRules_AS). */
UCLASS()
class OPENUT2K4_API AArenaAS_Info : public AInfo
{
	GENERATED_BODY()

public:
	AArenaAS_Info();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Team attacking this round (0 / 1). */
	UPROPERTY(Replicated) int32 AttackingTeam = 0;
	/** Current stage of the objective chain (0-based) and the number of stages. */
	UPROPERTY(Replicated) int32 CurrentStage = 0;
	UPROPERTY(Replicated) int32 NumStages = 0;
	/** 0 = first attack of a pair, 1 = second (sides swapped). */
	UPROPERTY(Replicated) int32 PairRound = 0;
	/** Pair number (1-based); > 1 after a draw or with a higher goal. */
	UPROPERTY(Replicated) int32 PairNumber = 1;
	/** Second attack: seconds to beat (the first team's time); < 0 = the first team failed (just complete it). */
	UPROPERTY(Replicated) float TimeToBeat = -1.f;
	/** First attack of this pair: team, success, time, stages completed. */
	UPROPERTY(Replicated) int32 FirstAttackTeam = -1;
	UPROPERTY(Replicated) bool bFirstSucceeded = false;
	UPROPERTY(Replicated) float FirstTime = 0.f;
	UPROPERTY(Replicated) int32 FirstStages = 0;
	/** This pair is an overtime pair (after a draw). */
	UPROPERTY(Replicated) bool bOvertimePair = false;
};

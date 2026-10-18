#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_BR.generated.h"

class AArenaBR_Ball;
class AArenaBR_Goal;

/**
 * Bombing Run: one neutral ball at the centre, a goal hoop at each base. Carry the ball into the enemy hoop
 * (7 points, the carrier is sacrificed) or throw it through (3 points). Dropped balls return after 25 s.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_BR : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// identity / defaults
	virtual bool IsTeamGame() const override { return true; }
	virtual float GetKillAdrenaline() const override { return 2.f; }
	virtual int32 GetDefaultGoalScore() const override { return 15; }
	/** xBombingRun.bDefaultTranslocator */
	virtual bool GetDefaultTranslocator() const override { return true; }
	virtual int32 GetDefaultNumBots() const override { return 7; }
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:BR-Colossus"); }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;
	virtual FString GetGoalLabel() const override { return TEXT("Score Limit"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 0; OutMax = 100; OutStep = 5; }
	virtual FString GetGoalText(int32 Goal) const override;

	// lifecycle
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void OnMatchStart() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ScoreKill(AController* Killer, AController* Victim, bool bHeadshot) override;

	// bots
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;
	virtual float GetBotHuntPriority(AArenaBotController* Bot, AArenaCharacter* Enemy, const FVector& LastSeen) const override;

	// HUD
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;

	// ball events (from AArenaBR_Ball)
	void OnBallPickedUp(AArenaBR_Ball* Ball, AArenaCharacter* By);
	void OnBallDropped(AArenaBR_Ball* Ball, AController* By, bool bThrown);
	void OnBallReturned(AArenaBR_Ball* Ball);
	/** The ball flew from From to To this tick: score if it went through a hoop. */
	void OnBallFlightSegment(AArenaBR_Ball* Ball, const FVector& From, const FVector& To);

	AArenaBR_Ball* GetBall() const;
	AArenaBR_Goal* GetGoal(int32 Team) const;

	enum EObjectiveTag : int32
	{
		Tag_Ball = 1,     // go get the loose ball
		Tag_Deliver = 2,  // carry the ball to the enemy goal
		Tag_Escort = 3,   // stay ahead of our carrier
		Tag_Attack = 4,   // hunt the enemy carrier
		Tag_Defend = 5,   // guard our goal
		Tag_Push = 6      // move up towards the enemy goal
	};

protected:
	void ScoreGoal(int32 ScoringTeam, AController* Scorer, int32 Points, bool bCarried);
	bool FindPositions(AArenaLevelBuilder* Builder, FVector& OutCenter, FVector OutFloor[2], FVector OutHoop[2]) const;
	void BotCarrierThink(AArenaBotController* Bot, AArenaCharacter* P);
	/** Where carriers score at each goal (a jump-in spot level with the ring, or the floor under a walk-in ring). */
	void UpdateGoalSpots();
	FVector PickDefendSpot(AArenaBotController* Bot, int32 Team) const;
	FVector GetEscortSpot(const AArenaCharacter* Carrier) const;

	UPROPERTY() TObjectPtr<AArenaBR_Ball> Ball;
	UPROPERTY() TObjectPtr<AArenaBR_Goal> Goals[2];
	TWeakObjectPtr<AController> SacrificeController;
	TMap<TWeakObjectPtr<AController>, float> NextBotThinkTime;
	float ScoreLockUntil = 0.f;
	/** Time the current carrier got the ball (bots don't pass right after a catch). */
	float CarrierSince = 0.f;
	/** UT2004 BombingRunSquadAI.ScoringRand: rolled when the ball is picked up (low = this carrier prefers shooting). */
	float CarrierScoringRand = 1.f;
	float NextPassTime[2] = { 0.f, 0.f };
	FVector ApproachSpot[2] = { FVector::ZeroVector, FVector::ZeroVector };
	bool bDiveGoal[2] = { false, false };
	/** The jump-in spot is a jump pad that throws its rider through the ring. */
	bool bPadGoal[2] = { false, false };
	bool bGoalSpotsDone = false;
};

#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_DOM.generated.h"

class AArenaDOM_ControlPoint;

/**
 * Double Domination: two control points, A and B. Touch a point to claim it. Holding both for 10 seconds
 * scores a point; then both reset to neutral and stay locked for 5 seconds.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_DOM : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// identity / defaults
	virtual bool IsTeamGame() const override { return true; }
	virtual float GetKillAdrenaline() const override { return 2.f; }
	virtual int32 GetDefaultGoalScore() const override { return 3; }
	virtual int32 GetDefaultNumBots() const override { return 7; }
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:DOM-SunTemple"); }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;
	virtual FString GetGoalLabel() const override { return TEXT("Score Limit"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 0; OutMax = 20; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;

	// lifecycle
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void OnStartPlay() override;
	virtual void OnMatchStart() override;
	virtual void Tick(float DeltaSeconds) override;

	// bots
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;

	// HUD
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas) override;
	/** UT2004 HUD only (HudCDeathmatch.DrawHudPassA team pass): HudCDoubleDomination's points; false otherwise. */
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;

	AArenaDOM_ControlPoint* GetPoint(int32 Index) const;

	static constexpr float HoldTime = 10.f;
	static constexpr float LockTime = 5.f;

	enum EObjectiveTag : int32
	{
		Tag_Capture = 1, // touch a point we don't own
		Tag_Hold = 2,    // guard a point we own (or wait at a locked one)
	};

protected:
	void ProcessTouches();
	void ScorePoint(int32 Team);
	bool FindPositions(AArenaLevelBuilder* Builder, FVector OutPos[2]) const;
	AArenaDOM_ControlPoint* SpawnPoint(const FVector& Floor, const FString& InLabel);
	float ServerTime() const;
	/** Bots must be able to walk to both points: move a point the nav graph can't reach to the closest reachable spot. */
	void EnsureReachable();

	UPROPERTY() TObjectPtr<AArenaDOM_ControlPoint> Points[2];
	int32 HoldTeam = -1;
	float HoldStart = 0.f;
	bool bLocked = false;
	float LastAnnouncedSecond = -1.f;
};

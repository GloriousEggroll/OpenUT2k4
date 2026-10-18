#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_CTF.generated.h"

class AArenaFlag;

/**
 * Capture the Flag: each team has a flag at its base (anchors "TeamBase0" / "TeamBase1"). Take the enemy
 * flag and touch your own flag at home to capture. AArenaFlag reports to AArenaGameMode::Flag*, which
 * forwards to the On* functions below.
 */
UCLASS()
class OPENUT2K4_API UArenaRules_CTF : public UArenaGameRules
{
	GENERATED_BODY()

public:
	// identity / defaults
	virtual bool IsTeamGame() const override { return true; }
	virtual float GetKillAdrenaline() const override { return 2.f; }
	virtual EArenaGameType GetLegacyGameType() const override { return EArenaGameType::CaptureTheFlag; }
	virtual int32 GetDefaultGoalScore() const override { return 3; }
	/** CTFGame.bDefaultTranslocator */
	virtual bool GetDefaultTranslocator() const override { return true; }
	virtual int32 GetDefaultNumBots() const override { return 7; }
	virtual FString GetDefaultLayout() const override { return TEXT("Imported:CTF-Face3"); }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;
	virtual FString GetGoalLabel() const override { return TEXT("Capture Limit"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 0; OutMax = 20; OutStep = 1; }
	virtual FString GetGoalText(int32 Goal) const override;

	// lifecycle
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void InitGameState(AArenaGameState* GS) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;

	// bots
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals) override;
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective) override;
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const override;
	virtual float GetBotHuntPriority(AArenaBotController* Bot, AArenaCharacter* Enemy, const FVector& LastSeen) const override;

	// HUD
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) override;

	// flag events (forwarded by AArenaGameMode::Flag*)
	virtual void OnFlagTaken(AArenaFlag* Flag, AController* By);
	virtual void OnFlagDropped(AArenaFlag* Flag, AController* By);
	virtual void OnFlagReturned(AArenaFlag* Flag, AController* By);
	virtual void OnFlagCaptured(AArenaFlag* Flag, AController* By);

	AArenaFlag* GetFlag(int32 Team) const { return (Team == 0 || Team == 1) ? Flags[Team].Get() : nullptr; }

	/** Bot objective tags */
	enum EObjectiveTag : int32
	{
		Tag_FlagHome = 1,     // carry the enemy flag to our base
		Tag_FlagLocation = 2, // go to a flag (take / return)
		Tag_Chase = 3,        // chase a flag carrier
		Tag_Defend = 4,       // guard our base
		Tag_Via = 5,          // the squad's alternate route (UE2 AssaultPath) on the way to the flag or home
		Tag_Heal = 6          // a hurt carrier picks up health / armour on the way
	};

protected:
	AArenaFlag* SpawnFlag(AArenaLevelBuilder* Builder, const FVector& FloorLocation, int32 Team);
	/**
	 * UE2 SquadAI alternate paths: a random AssaultPath (weighted by its Priority) leading to the objective at
	 * ObjectiveLoc that is not much of a detour from From to To. bReturn: a carrier going home (bReturnOnly paths
	 * allowed, bNoReturn ones not); otherwise the other way round.
	 */
	const class AArenaNavGraph* GetBotNavGraph() const;
	bool PickAltPath(const FVector& From, const FVector& To, const FVector& ObjectiveLoc, bool bReturn, FRandomStream& Rng, FVector& OutVia) const;

	/** The alternate route a bot took for its current trip (to the enemy flag, or home with it). */
	struct FBotRoute
	{
		TWeakObjectPtr<APawn> Pawn;
		bool bCarry = false;
		bool bPicked = false;
		bool bDone = true;
		FVector Via = FVector::ZeroVector;
	};
	TMap<TWeakObjectPtr<AController>, FBotRoute> Routes;

	UPROPERTY() TObjectPtr<AArenaFlag> Flags[2];
};

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ArenaTypes.h"
#include "ArenaNavGraph.h"
#include "ArenaGameRules.h"
#include "ArenaBotController.generated.h"

class AArenaCharacter;
class AArenaWeapon;

/** What the bot is currently moving towards (internal). */
enum class EArenaBotGoal : uint8
{
	None,
	Wander,
	Pickup,
	Objective,     // a game rules objective (UArenaGameRules::GetBotObjectives), see CurrentObjective
	Hunt           // go to the last seen enemy position
};

/**
 * Bot AI: target selection, aiming with skill-based error and projectile leading, strafing / dodging,
 * weapon choice, item collection, game mode objectives (UArenaGameRules::GetBotObjectives), path following
 * over AArenaNavGraph.
 * Moves the pawn with AddMovementInput (no navmesh needed).
 */
UCLASS()
class OPENUT2K4_API AArenaBotController : public AAIController
{
	GENERATED_BODY()

public:
	AArenaBotController();

	/** 0 (novice) .. 7 (godlike) */
	void SetSkill(float InSkill);
	float GetSkill() const { return Skill; }

	/** Called by the game mode after the pawn dies. */
	void NotifyPawnDied();
	/** Damage notification (turn towards attacker). */
	void NotifyTookDamage(AController* InstigatedBy, const FVector& FromLocation);
	/**
	 * UE2 Pawn.MakeNoise: Maker made a noise of Loudness (weapon fire 1, picking something up 0.2) at Location. Every bot
	 * that hears it (Bot.HearNoise: within HearingThreshold x sqrt(Loudness) with a clear line, a third of that through
	 * walls) takes Maker as its enemy when it has none in sight and turns to it.
	 */
	static void ReportNoise(APawn* Maker, const FVector& Location, float Loudness);
	void HearNoise(APawn* Maker, const FVector& Location, float Loudness);
	/** UE2 Bot.LostContact: how long an enemy out of sight stays known (7 s, 4 s for defenders in team games). */
	float GetEnemyMemoryTime() const;

	// ---- API for game rules ----
	AArenaCharacter* GetArenaPawn() const;
	AArenaNavGraph* GetNavGraph() const;
	int32 GetTeam() const { return GetMyTeam(); }
	/** Roughly one bot in three is a defender (stable per bot); modes may change it. */
	bool IsDefender() const { return bDefender; }
	void SetDefender(bool bInDefender) { bDefender = bInDefender; }
	/** Current target (may be null / not visible right now). */
	AArenaCharacter* GetEnemy() const { return Enemy; }
	bool IsEnemyVisible() const { return bEnemyVisible; }
	FVector GetLastSeenEnemyLocation() const { return LastSeenEnemyLocation; }
	/** The objective being pursued, or null when the goal is not a rules objective. */
	const FArenaBotObjective* GetCurrentObjective() const { return GoalKind == EArenaBotGoal::Objective ? &CurrentObjective : nullptr; }
	/** Where the bot is currently heading. */
	FVector GetGoalLocation() const { return GoalLocation; }
	/** True if the bot reached its goal location in the last goal evaluation. */
	bool HasReachedGoal() const { return bGoalReached; }
	/** Force an objective now (replaces the current goal; normal evaluation resumes after ~1 s). */
	void SetGoal(const FArenaBotObjective& Objective);
	/** Re-evaluate goals on the next tick. */
	void ForceGoalUpdate() { NextGoalTime = 0.f; GoalKind = EArenaBotGoal::None; }
	/**
	 * Run straight at Target for Duration seconds, ignoring the nav graph and the edge guard (e.g. a dive into a Bombing Run
	 * hoop over a pit). bJump: jump at once (double jumping at the apex when needed).
	 */
	void SetDirectMove(const FVector& Target, float Duration, bool bJump);
	bool IsDirectMoving() const;
	/** A jump pad launched us (UE2 JumpPad.PostTouch: MoveTarget = JumpTarget): steer for its target until we land. */
	void NotifyJumpPadLaunch(const FVector& JumpTarget);
	/** Remaining path points (next first). */
	const TArray<FVector>& GetPathPoints() const { return Path; }
	/** Press "use" (forwarded to UArenaGameRules::OnPlayerUse). */
	bool RequestUse();
	/** Pawn carries a CTF flag / any carryable. */
	bool HasFlag() const;
	bool IsCarryingObjective() const;
	/** The bot's deterministic random stream. */
	FRandomStream& GetRandomStream() { return Rng; }

	// ---- UT2004 Bot combat (ArenaBotCombat.cpp) ----
	/** UE2 Bot.AdjustAim: the direction a shot of Weapon's Mode leaves ProjStart in (lead, splash, yaw aim error from
	 * AdjustAimError); the view snaps to it like UE2's SetRotation(FireRotation). */
	FVector AdjustAimUE2(const AArenaWeapon* Weapon, uint8 Mode, const FVector& ProjStart, const FVector& ViewDir);
	/** UE2 Bot.ReceiveWarning: an instant-hit shot aimed at us (ProjSpeed -1). */
	void ReceiveWarning(APawn* Shooter, float ProjSpeed, const FVector& FireDir);
	/** UE2 Bot.ReceiveProjectileWarning: a projectile fired at us; may dodge after WarningDelay. */
	void ReceiveProjectileWarning(class AArenaProjectile* Proj);
	/** UE2 ShockRifle.SetComboTarget: a core this bot fired at its enemy, to detonate with the beam. */
	void SetComboCore(class AArenaProjectile* Core);
	/**
	 * UE2 C.Focus = Target; C.FireWeaponAt(Target) for a non-pawn target (RedeemerProjectile.Timer: enemy bots shoot
	 * the incoming warhead). For Duration seconds, while the bot has no enemy in sight, it faces Target and fires its
	 * current weapon at it (leading it). Ends early when Target is gone.
	 */
	void SetShootTarget(AActor* Target, float Duration);
	/** The shoot target while it applies (valid, not expired, no enemy in sight), else null. */
	AActor* GetShootTarget() const;
	/** Shots fired at shoot targets (tests). */
	int32 NumShootTargetShots = 0;
	/** UE2 Pawn.IsFiring: the current weapon's fire is held. */
	bool IsBotFiring() const { return bFiring; }
	/** UE2 Bot.ResetSkill RotationRate.Yaw in degrees per second. */
	float GetRotationRateYaw() const;
	/** UE2 RosterEntry profile values (0 = an average character). */
	float Accuracy = 0.f;
	float ReactionTimeAdjust = 0.f;
	float Jumpiness = 0.f;
	float StrafingAbility = 0.f;
	/**
	 * UT2004 .upl personality (-1..1, 0 = an average character):
	 * Aggressiveness: how readily the bot closes in and hunts (hunt priority +0.3 x it, shorter preferred range) and how
	 * late it backs off when hurt (it backs off below 40 health when Aggressiveness + CombatStyle / 2 < 0.3).
	 * CombatStyle: preferred fighting distance, x (1 - 0.3 x CombatStyle): above 0 closer, below 0 further away.
	 * Tactics: squad play, the range at which it takes on enemies its squadmates fight (40 m x (1 + 0.5 x Tactics)).
	 * FavoriteWeapon: rated 0.1 higher when choosing a weapon, and the bot is proficient with it (shock combos: skill + 2).
	 */
	float Aggressiveness = 0.f;
	float CombatStyle = 0.f;
	float Tactics = 0.f;
	TSubclassOf<class AArenaWeapon> FavoriteWeapon;
	void SetAccuracy(float V) { Accuracy = FMath::Clamp(V, -1.f, 1.f); }
	void SetReactionTime(float V) { ReactionTimeAdjust = FMath::Clamp(V, -1.f, 1.f); }
	void SetJumpiness(float V) { Jumpiness = FMath::Clamp(V, -1.f, 1.f); }
	void SetStrafingAbility(float V) { StrafingAbility = FMath::Clamp(V, -1.f, 1.f); }
	void SetAggressiveness(float V) { Aggressiveness = FMath::Clamp(V, -1.f, 1.f); }
	void SetCombatStyle(float V) { CombatStyle = FMath::Clamp(V, -1.f, 1.f); }
	void SetTactics(float V) { Tactics = FMath::Clamp(V, -1.f, 1.f); }
	void SetFavoriteWeapon(TSubclassOf<class AArenaWeapon> W) { FavoriteWeapon = W; }
	/** The .upl FavoriteWeapon string ("XWeapons.ShockRifle", "Onslaught.ONSAVRiL" ...) mapped to our weapon; false if unknown. */
	bool SetFavoriteWeaponUE2(const FString& UE2Class);
	bool IsFavoriteWeapon(const class AArenaWeapon* W) const;
	/** Counters for the -MoveTest bot checks. */
	int32 NumWarnings = 0;
	int32 NumDucks = 0;
	int32 NumTacticalJumps = 0;
	int32 NumGoalDodges = 0;
	int32 NumCombos = 0;
	int32 NumLockedRockets = 0;
	float LastAimErrorYaw = 0.f;
	/** Jump pad launches while the path's next link was a JumpPad link (planned pad use). */
	int32 NumPadLaunches = 0;
	/** UE2 ForcedPaths jumps taken (JumpDest.DoJump). */
	int32 NumForcedJumps = 0;
	/** True while the path's next or second link is a JumpPad link (heading onto a pad on purpose: the trigger is
	 *  usually touched before the pad's own node counts as reached). */
	bool IsFollowingPadLink() const
	{
		return (PathLinks.Num() > 0 && PathLinks[0] == EArenaNavLinkType::JumpPad) || (PathLinks.Num() > 1 && PathLinks[1] == EArenaNavLinkType::JumpPad);
	}

	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void UpdateControlRotation(float DeltaTime, bool bUpdatePawn = true) override;

protected:
	// sub-behaviours (implemented in ArenaBotController.cpp)
	void UpdateEnemy(float DeltaSeconds);
	void UpdateGoal(float DeltaSeconds);
	void UpdateMovement(float DeltaSeconds);
	void UpdateAimAndFire(float DeltaSeconds);
	void ChooseWeapon();
	bool CanSee(const AActor* Other) const;

	float Skill = 3.f;
	UPROPERTY() TObjectPtr<AArenaCharacter> Enemy;
	FVector LastSeenEnemyLocation = FVector::ZeroVector;
	float LastSeenEnemyTime = -100.f;

	FVector GoalLocation = FVector::ZeroVector;
	TWeakObjectPtr<AActor> GoalActor;
	TArray<FVector> Path;
	TArray<EArenaNavLinkType> PathLinks;
	float NextGoalTime = 0.f;
	float NextRepathTime = 0.f;
	float StuckTime = 0.f;
	FVector LastPosition = FVector::ZeroVector;

	FRotator DesiredRotation = FRotator::ZeroRotator;
	FVector AimError = FVector::ZeroVector;
	float NextAimErrorTime = 0.f;
	float StrafeDir = 1.f;
	float NextStrafeChange = 0.f;
	float NextDodgeTime = 0.f;
	float ReactionTimer = 0.f;
	bool bFiring = false;
	TWeakObjectPtr<AActor> ShootTarget;
	float ShootTargetUntil = -1.f;
	/** UpdateAimAndFire for the shoot target; true when it handled this tick. */
	bool UpdateShootTarget(float DeltaSeconds);
	uint8 FiringMode = 0;
	FRandomStream Rng;

	// ---- internal helpers ----
	void StartFiring(uint8 Mode);
	void StopFiring();
	void Repath();
	void StartJump(bool bAllowDoubleJump);
	void UpdateJumpState(const FVector& MoveTarget, bool bJumpLink);
	FVector ComputeAimPoint(const AArenaWeapon* Weapon, uint8 Mode, float Distance) const;
	float GetPreferredDistance(const AArenaWeapon* Weapon) const;
	FVector AvoidObstacles(const FVector& Dir, bool bAvoidPits) const;
	bool HasFloorAt(const FVector& Location, float MaxDrop) const;
	/**
	 * Void check: true if there is level geometry below Location (pawn centre height) within MaxDrop that is
	 * above the world's kill Z. Long drops onto a lower floor are safe; open chasms / falling below kill Z are not.
	 */
	bool HasSafeFloorAt(const FVector& Location, float MaxDrop = 1600.f, float* OutFloorZ = nullptr) const;
	/** Every sample along Dir (from the pawn, every ~100 cm up to Distance, stopping at walls) has safe floor. */
	bool IsSafeMove(const FVector& Dir, float Distance) const;
	/** Steer a ground move away from the void: Dir if safe, else the closest safe rotation, else brake. */
	FVector GuardEdge(const FVector& Dir) const;
	bool IsBlockedAhead(const FVector& Dir, float Distance) const;
	bool IsEnemyAimingAtUs() const;
	float GetReactionTime() const;
	int32 GetMyTeam() const;
	void ResetState();

	// ---- internal state ----
	EArenaBotGoal GoalKind = EArenaBotGoal::None;
	/** Valid when GoalKind == Objective. */
	FArenaBotObjective CurrentObjective;
	bool bGoalReached = false;
	float GoalScore = 0.f;
	FVector PathGoal = FVector::ZeroVector;
	FVector PrevPathPoint = FVector::ZeroVector;
	/** Lift link (UE2 mover): steers to / onto the lift; true while the bot should stand still (waiting or riding). */
	bool UpdateLiftRide(const FVector& Loc, FVector& InOutMoveTarget);
	float PathTime = -100.f;
	bool bPathFailed = false;
	bool bEnemyVisible = false;
	float NextSightCheckTime = 0.f;
	float LookAtUntil = 0.f;
	float NextWeaponSwitchTime = 0.f;
	int32 JumpPhase = 0;
	float JumpPhaseTime = 0.f;
	bool bAllowDoubleJump = false;
	float ChargeReleaseTime = 0.f;
	float FireStartTime = 0.f;
	float LastAimOkTime = -100.f;
	bool bAimHead = false;
	int32 StuckCount = 0;
	bool bDefender = false;
	bool bRngSeeded = false;
	TWeakObjectPtr<AActor> IgnoredGoalActor;
	float IgnoreGoalUntil = 0.f;
	FVector LastMoveDir = FVector::ZeroVector;
	/** Last place we stood on safe ground (air recovery steers back towards it when knocked over the void). */
	FVector LastSafeGround = FVector::ZeroVector;
	bool bHasSafeGround = false;
	/** -BotDebug state dump timer. */
	FVector DirectMoveTarget = FVector::ZeroVector;
	float DirectMoveUntil = -1.f;
	bool bDirectMoveJump = false;
	FVector PadFlightTarget = FVector::ZeroVector;
	float PadFlightStart = -1.f;
	float PadFlightMaxZ = 0.f;
	FVector PadLaunchLoc = FVector::ZeroVector;
	float NextDebugTime = 0.f;
	/** Squad enemy sharing (UE2 SquadAI.Enemies): next check for a teammate's enemy. */
	float NextSquadEnemyTime = 0.f;
	float LastHeardTime = -100.f;

	// ---- UT2004 Bot combat state (ArenaBotCombat.cpp) ----
	float AdjustAimErrorUE2(float InAimError, float TargetDist, bool bDefendMelee, bool bInstantProj, bool bLeadTargetNow);
	bool TryToDuck(FVector DuckDir, bool bReversed);
	void DelayedWarning();
	/** UE2 Bot.FaceActor: while moving along a path with an enemy around, face the enemy (strafe) or the path. */
	bool ShouldFaceEnemyUE2(const FVector& MoveTarget);
	/** UE2 Bot.MayDodgeToMoveTarget (DodgeToGoalPct): dodge towards a far path node (then double jump). */
	void MayDodgeToMoveTarget(const FVector& MoveTarget);
public:
	/** UE2 Bot.SetCombatTimer period: 1.2 - 0.09 x min(10, skill + reaction time). */
	float GetCombatTimerPeriod() const;
protected:
	bool IsStoppedUE2() const { return bStoppedUE2; }
	TWeakObjectPtr<class AArenaProjectile> WarningProjectile;
	float WarningDelay = 0.f;
	float AcquireTime = -100.f;
	float StopStartTime = 0.f;
	bool bStoppedUE2 = false;
	float LastUnderFire = -100.f;
	TWeakObjectPtr<class AArenaProjectile> ComboCore;
	bool bWaitForCombo = false;
	FVector FaceDecisionPoint = FVector::ZeroVector;
	bool bFaceEnemy = true;
	FVector LastDodgeGoalPoint = FVector::ZeroVector;
	float NextTacticalTime = 0.f;
	bool bRegisterComboTarget = false;
	bool bComboTriggered = false;
	/** Set by UpdateMovement: following a path to an important goal with an enemy around (Bot.FaceActor applies). */
	bool bPathFacing = false;
	FVector CurMoveTarget = FVector::ZeroVector;
	/** Double jump at the apex of the current jump / dodge (UE2 bNotifyApex + bPendingDoubleJump / bTacticalDoubleJump). */
	bool bApexDoubleJump = false;
	float NextShockModeTime = 0.f;
	uint8 ShockMode = 0;
public:
	float GetAcquireTime() const { return AcquireTime; }
	float GetLastSeenEnemyTime() const { return LastSeenEnemyTime; }

	// ---- UT2004 translocator use (ArenaTranslocatorBot.cpp; Bot.TranslocationTarget / bPreparingMove) ----
	/** Bot.CanUseTranslocator: has one, skill 2+, NextTranslocTime passed, the squad allows it (not carrying the flag / ball). */
	bool CanUseTranslocator() const;
	/** TransLauncher.ReduceAmmo: TranslocFreq = 3 + max(TranslocFreq, now). */
	void NotifyTranslocated();
	/** TransLauncher.DrainCharges: NextTranslocTime = now + 3.5. */
	void NotifyTranslocatorDrained();
	/** TransRecall.BotTranslocation: the RealTranslocationTarget while preparing a move. */
	bool GetRealTranslocationTarget(FVector& OutLocation) const;
	/** MonitoringThrow.Tick's checks: the target is still where the bot is going. */
	bool IsTranslocationTargetValid(const class AArenaTransBeacon* Beacon) const;
	/** MonitoringThrow.EndState / BotTranslocate (bMoved). bFailedMove: the prepared move failed (UE2 MoveTimer -1). */
	void OnBeaconMonitorEnd(class AArenaTransBeacon* Beacon, bool bFailedMove, float FearCost, bool bMoved);
	/** A hop's beacon at BeaconLoc has safe floor below it (no void / KillZ); translocator routes are the map author's. */
	bool IsSafeTranslocation(const FVector& BeaconLoc) const;
	/** Translocations done / beacons thrown by this bot (logs, tests). */
	int32 NumTranslocations = 0;
	int32 NumTranslocThrows = 0;
protected:
	/** Bot.FaceActor's translocator hop and JumpSpot.TryTranslocator on Translocate links; true while it drives the bot. */
	bool UpdateTranslocator(float DeltaSeconds, const FVector& Loc);
	/** JumpSpot.TryTranslocator for the path's Translocate link (From = PrevPathPoint, To = Path[0]). */
	bool TryTranslocatorLink(const FVector& From, const FVector& To);
	void ClearTranslocation(bool bSwitchBack);
	class AWeap_Translocator* GetTranslocator() const;
	/** Bot.TranslocationTarget (location / actor) and RealTranslocationTarget. */
	TWeakObjectPtr<AActor> TranslocTargetActor;
	FVector TranslocTargetLoc = FVector::ZeroVector;
	FVector RealTranslocTargetLoc = FVector::ZeroVector;
	float TranslocAimZ = 0.f;
	float TranslocTargetRadius = 80.f;
	float TranslocTargetHeight = 86.f;
	bool bHasTranslocTarget = false;
	bool bPreparingMove = false;
	bool bTranslocatorHop = false;
	bool bTranslocGameObject = false;
	bool bTranslocThrown = false;
	float TranslocStartTime = 0.f;
	float NextTranslocTime = 0.f;
	float TranslocFreq = 0.f;
	float NextHopCheck = 0.f;
	/** SquadAI.ShouldDestroyTranslocator: an enemy beacon in sight within 1500 UU is shot (SetShootTarget). */
	void ShouldDestroyTranslocator();
	float NextBeaconShootCheck = 0.f;
	/** Bot.NotifyMissedJump / the JumpSpot's FearCost: links that failed are avoided for a while. */
	TMap<FIntPoint, float> FailedTranslocLinks;
public:
};

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ArenaNavGraph.h"
#include "ArenaINV_MonsterController.generated.h"

class AArenaINV_Monster;
class AArenaCharacter;
struct FArenaProjectileParams;

/**
 * Invasion monster brain. Picks the nearest living player (humans and bots), paths to it over
 * AArenaNavGraph (flyers steer directly with simple obstacle avoidance) and runs the attack of the type's
 * behaviour (EArenaINVBehavior) with its UT2004 projectile (FArenaINVShot):
 *   Swarmer    - sprint and bite
 *   Leaper     - stalks, then leaps in a ballistic arc and rakes on landing; twin bolts in between (Skaarj)
 *   Spitter    - keeps its distance and fires its bolts, melee up close (Krall)
 *   Brute      - closes in, winds up and slams the ground; arm rockets at range (Brute, Behemoth)
 *   Flyer      - orbits above its target and fires (Gasbag)
 *   FlyerMelee - dives at its target and bites / stings (Razorfly, Manta)
 *   Boss       - rocket volleys at range, stomp up close (WarLord)
 * Attacks play the monster's action animations and sounds (AArenaINV_Monster::PlayAction / PlayMonsterSound).
 * All damage goes through ArenaDamage with this controller as instigator.
 */
UCLASS()
class OPENUT2K4_API AArenaINV_MonsterController : public AAIController
{
	GENERATED_BODY()

public:
	AArenaINV_MonsterController();

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void UpdateControlRotation(float DeltaTime, bool bUpdatePawn = true) override;

	AArenaINV_Monster* GetMonster() const;
	AArenaCharacter* GetTarget() const { return Target.Get(); }
	/** Last time this monster damaged a player (stall detection by the rules). */
	float GetLastAttackTime() const { return LastDamageDealtTime; }
	/** Last time the monster made real progress (moved / attacked). */
	float GetLastProgressTime() const { return LastProgressTime; }
	/** Called by the rules after it teleported the monster. */
	void ResetAfterTeleport();

protected:
	void UpdateTarget(float Now);
	void UpdateGroundMovement(float DeltaSeconds, float Now);
	void UpdateFlyingMovement(float DeltaSeconds, float Now);
	void UpdateAttack(float DeltaSeconds, float Now);

	/** Move toward Goal on the nav graph (or directly when in clear view). Returns the steering direction. */
	FVector GetPathDirection(const FVector& Goal, float Now, bool& bOutJumpLink);
	void Repath(const FVector& Goal, float Now);
	bool HasClearShot(const FVector& From, const AActor* Other) const;
	bool IsWalkableStraight(const FVector& From, const FVector& To) const;
	FVector AvoidObstacles3D(const FVector& Dir, float LookAhead) const;

	void DoMelee(float Damage, float Now);
	void FireProjectile(const FVector& Start, const FVector& Velocity, const FArenaProjectileParams& Params);
	/** Launch velocity for a projectile of Speed / GravityScale to hit Target (low arc); false if out of range. */
	bool SolveBallistic(const FVector& Start, const FVector& TargetLoc, float Speed, float GravityScale, FVector& OutVelocity) const;
	/** Predict where Target will be in Time seconds (ground clamped). */
	FVector PredictTarget(float Time) const;

	TWeakObjectPtr<AArenaCharacter> Target;
	float NextTargetTime = 0.f;
	bool bTargetVisible = false;
	float LastSeenTargetTime = -100.f;

	TArray<FVector> Path;
	TArray<EArenaNavLinkType> PathLinks;
	FVector PathGoal = FVector::ZeroVector;
	float NextRepathTime = 0.f;
	bool bPathFailed = false;

	FRotator DesiredRotation = FRotator::ZeroRotator;
	FVector LastPosition = FVector::ZeroVector;
	float StuckTime = 0.f;
	float LastProgressTime = 0.f;
	float LastDamageDealtTime = -100.f;
	float NextMeleeTime = 0.f;
	float NextSpecialTime = 0.f;
	float NextJumpTime = 0.f;
	float NextShotTime = 0.f;
	float LastChallengeTime = -100.f;

	// Attack state
	int32 AttackPhase = 0;       // 0 idle, 1 wind-up, 2 active (leap in air)
	int32 PendingAttack = 0;     // which special is winding up (type specific)
	bool bJumpHeld = false;
	bool bWantDoubleJump = false;
	float AttackPhaseTime = 0.f;
	bool bLeapHit = false;
	float OrbitAngle = 0.f;
	float OrbitDir = 1.f;
	float NextOrbitFlip = 0.f;
	FVector StrafeOffset = FVector::ZeroVector;
	FRandomStream Rng;
};

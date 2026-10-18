#pragma once

#include "CoreMinimal.h"
#include "ArenaWeapon.h"
#include "ArenaBR_BallLauncher.generated.h"

class AArenaBR_Ball;
class AArenaCharacter;

/**
 * Bombing Run ball launcher. Given automatically to whoever picks up the ball (no pickup in the level) and
 * removed again as soon as the ball leaves the carrier. While it is held no other weapon can be selected.
 *  Primary: throw the ball (a pass). If a teammate is locked, the pass homes in on them.
 *  Alt:     lock on to the teammate closest to the crosshair in front of you.
 * Bots don't use the fire buttons; UArenaRules_BR drives their passes and shots through ThrowAt / PassTo.
 */
UCLASS()
class OPENUT2K4_API AArenaBR_BallLauncher : public AArenaWeapon
{
	GENERATED_BODY()

public:
	AArenaBR_BallLauncher();

	virtual void StartFire(uint8 Mode) override;
	virtual bool HasAmmo() const override;
	virtual bool HasAmmoFor(uint8 Mode) const override;
	virtual float GetBotRating(float DistanceToEnemy) const override { return 100.f; }

	/** The ball the owner carries (null if none). */
	AArenaBR_Ball* GetCarriedBall() const;
	AArenaCharacter* GetLockedTarget() const { return LockedTarget.Get(); }

	/** Throw along the aim (with homing if a teammate is locked). Returns true if the ball left. */
	bool ThrowPass();
	/** Find and lock the best teammate in front. Returns the target or null. */
	AArenaCharacter* TryLock();
	/** Bot API: homing pass to a teammate. */
	bool PassTo(AArenaCharacter* Teammate);
	/** Bot API: lob the ball so it reaches a world point (ballistic, e.g. through a hoop). */
	bool ThrowAt(const FVector& TargetPoint);

	/** Remove the launcher from a character's inventory (no-op if it has none). */
	static void RemoveFrom(AArenaCharacter* Character);
	/** Give the launcher to a character and select it. */
	static AArenaBR_BallLauncher* GiveTo_BR(AArenaCharacter* Character);

	static constexpr float PassSpeed = 1900.f;

protected:
	virtual void Tick(float DeltaSeconds) override;
	virtual void BuildViewModel() override;
	bool Launch(const FVector& Velocity, AArenaCharacter* HomingTarget);
	FVector GetThrowStart() const;

	TWeakObjectPtr<AArenaCharacter> LockedTarget;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> LockGlow;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> BallGlow;
	float NextActionTime = 0.f;
	bool bRemoving = false;
};

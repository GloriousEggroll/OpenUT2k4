#pragma once

#include "CoreMinimal.h"
#include "ArenaCarryable.h"
#include "ArenaBR_Ball.generated.h"

class AArenaCharacter;
class UMaterialInstanceDynamic;

/**
 * Bombing Run ball: a neutral carryable that spawns at the arena centre. Picking it up arms the carrier
 * with the ball launcher (AArenaBR_BallLauncher); throwing it makes it a bouncy projectile that can home in
 * on a locked teammate. Scores are detected by UArenaRules_BR (carry-in in the rules tick, throw-through
 * from the flight segment reported here).
 */
UCLASS()
class OPENUT2K4_API AArenaBR_Ball : public AArenaCarryable
{
	GENERATED_BODY()

public:
	AArenaBR_Ball();

	/** Throw from a character's hands. HomingTarget (optional) is a teammate the ball curves towards. */
	void LaunchFrom(AArenaCharacter* Thrower, const FVector& Start, const FVector& Velocity, AArenaCharacter* HomingTarget);

	/** Team of the last character that threw it (-1 if none / reset). */
	int32 GetLastThrowerTeam() const { return LastThrowerTeam; }
	AController* GetLastThrower() const { return LastThrower.Get(); }
	AArenaCharacter* GetHomingTarget() const { return HomingTarget.Get(); }
	/** Seconds left until an untouched dropped ball returns (0 if not dropped). */
	float GetReturnCountdown() const;
	/** Clears thrower info (after a score). */
	void ClearThrowInfo();
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): the ball's own replicated static mesh and DrawScale (xBombFlag:
	 * E_Pickups.BombBall.FullBomb while loose); null restores our ball model (carried).
	 */
	void SetNetMesh(class UStaticMesh* Mesh, float DrawScale);

	// AArenaCarryable
	virtual bool CanBePickedUpBy(AArenaCharacter* Character) const override;
	virtual void OnPickedUp(AArenaCharacter* Character) override;
	virtual void OnDropped(AController* By) override;
	virtual void OnReturned(AController* By) override;
	virtual void OnLanded() override;
	virtual void BuildModel() override;
	virtual FString GetCarrierHudText() const override;

protected:
	virtual void Tick(float DeltaSeconds) override;
	void UpdateBeaconColor();

	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BeaconMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> CoreGlowMaterial;
	TWeakObjectPtr<AArenaCharacter> HomingTarget;
	TWeakObjectPtr<AController> LastThrower;
	int32 LastThrowerTeam = -1;
	int32 BeaconTeam = -2;
	float HomingUntil = 0.f;
	UPROPERTY() TObjectPtr<class UStaticMesh> OwnBallMesh;
	FTransform OwnBallTransform;
	/** Imported ball spawn pad at the home spot (not part of the moving model). */
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> SpawnPad;
};

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaJumpPad.generated.h"

class UStaticMeshComponent;

/**
 * UE2 JumpPad (UTJumpPad): launches pawns that touch it. The trigger is the UE2 actor's collision cylinder
 * (CollisionRadius / CollisionHeight around the UE2 location, UE2 Touch: the pawn's cylinder overlaps it), tested
 * along each pawn's path every frame so fast pawns (BR-Colossus tube rides) can not skip it. The launch is the
 * UE2 JumpVelocity of the map when it has one (PostTouch: Velocity = JumpVelocity), else a ballistic arc to Target.
 * Like UE2 only pawns are launched; a ball or flag flying through is not (JumpPad.Touch ignores non-pawns).
 */
UCLASS()
class OPENUT2K4_API AArenaJumpPad : public AActor
{
	GENERATED_BODY()

public:
	AArenaJumpPad();

	/** World-space landing target. Call before or after spawn. */
	void SetTarget(const FVector& InTarget, float InApexHeight = 400.f);
	FVector GetTarget() const { return Target; }
	/** Launch velocity for a character standing on the pad. */
	FVector ComputeLaunchVelocity(const FVector& From) const;
	/**
	 * UE2 trigger cylinder: Center is the UE2 actor location, Radius / HalfHeight its CollisionRadius /
	 * CollisionHeight (cm). Without a call: a 80 x 86 cm cylinder (UE2 JumpPad defaults 40 / 43) above the floor.
	 */
	void SetUE2Collision(const FVector& Center, float Radius, float HalfHeight);
	/** UE2 JumpVelocity (cm/s) from the map; zero: the ballistic launch to Target. */
	void SetUE2JumpVelocity(const FVector& InVelocity) { UE2JumpVelocity = InVelocity; }
	FVector GetTriggerCenter() const;
	float GetTriggerRadius() const { return TriggerRadius; }
	float GetTriggerHalfHeight() const { return TriggerHalfHeight; }
	/** UE2 name of the pad (logs), empty for generated pads. */
	FString UE2Name;

protected:
	virtual void BeginPlay() override;

	FVector Target = FVector::ZeroVector;
	float ApexHeight = 400.f;
	TMap<TWeakObjectPtr<AActor>, float> LastLaunch;
	FVector UE2JumpVelocity = FVector::ZeroVector;
	bool bHasUE2Center = false;
	FVector UE2Center = FVector::ZeroVector;
	float TriggerRadius = 80.f;
	float TriggerHalfHeight = 86.f;
	/** Per pawn: last position and whether its cylinder was touching the pad (UE2 Touch fires on entering). */
	struct FToucher { FVector LastLocation = FVector::ZeroVector; bool bTouching = false; };
	TMap<TWeakObjectPtr<AActor>, FToucher> Touchers;
	/** UE2 cylinder overlap of a pawn cylinder at Location with the pad's. */
	bool Touches(const FVector& Location, float Radius, float HalfHeight) const;

	virtual void Tick(float DeltaSeconds) override;
	/** Ballistic solution for a given (negative) gravity Z. */
	FVector ComputeLaunchVelocityWithGravity(const FVector& From, float GravityZ) const;
	/** Launch if the per-actor cooldown allows. Returns true if launched. */
	bool TryLaunch(class AArenaCharacter* Character);

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> PadMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> GlowMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> ArrowMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UPointLightComponent> Light;
};

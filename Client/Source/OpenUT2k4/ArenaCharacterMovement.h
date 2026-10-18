#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ArenaCharacterMovement.generated.h"

/**
 * Character movement with UE2 (UT2004) physics rules on top of the engine's collision, floor and step handling:
 *  - walking (UE2 APawn::calcVelocity): input changes direction with ground friction (the physics volume's UE2
 *    GroundFriction), no input brakes with 2 x friction, and the speed is clamped hard to GroundSpeed (landing from
 *    a fast boost drops to running speed at once, which is why UT2004 has no bunny hop);
 *  - falling (UE2 physFalling): input accelerates with AccelRate * AirControl and may only steer: the horizontal
 *    speed never rises above max(current horizontal speed, AirSpeed), so air strafing builds no speed;
 *  - swimming: UE2 fluid friction (the volume's FluidFriction) and WaterSpeed, the UE2 water jump out of a pool;
 *  - crouched / walking speed = GroundSpeed * CrouchedPct / WalkingPct, no crouching in the air, no walking off
 *    ledges while crouched or walking (UE2 bCanWalkOffLedges);
 *  - moving bases (lifts) do not impart velocity on their own: AArenaCharacter's jump adds the lift's vertical
 *    speed like UE2 Pawn.DoJump;
 *  - UE2 PhysicsVolume (AArenaPhysicsVolume): the full Gravity vector while falling (sideways / upward tubes),
 *    TerminalVelocity (falling speed limit), FluidFriction (swimming, and falling in water), GroundFriction
 *    (walking), ZoneVelocity added to the move (not to Velocity; walking: its horizontal part) and bBounceVelocity
 *    (landing inside bounces off with ZoneVelocity). A pawn can not stand in a volume whose Gravity.Z points up.
 * Before each physics step the owner gets AArenaCharacter::PreMovementUpdate (UE2 PlayerController.PlayerMove /
 * ProcessMove: double-click dodges, jumps, crouch).
 */
UCLASS()
class OPENUT2K4_API UArenaCharacterMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UArenaCharacterMovement();

	/** UE2 Pawn.bIsWalking (the walk key): WalkingPct speed, no walking off ledges, jumps use the default JumpZ. */
	bool bUE2Walking = false;
	/** UE2 Pawn.bStopAtLedges (set by bot AI): a walking pawn stops at ledges instead of walking off them. */
	bool bStopAtLedges = false;
	/**
	 * Net play (Net/UE2NetPlay.h): brake without input exactly as UE2 APawn::calcVelocity does (0.03 s steps, the
	 * average velocity of the steps, zero below 10 UU/s), so a UT2004 server's walking pawn and ours stop alike.
	 */
	bool bUE2ServerBraking = false;
	/**
	 * Net play: UE2 ULevel::MoveActor stops a moving actor 2 UU short of what it hits (the trace is extended by 2 UU
	 * along the move and the hit pulled back by as much; a hit within 2 UU does not move it at all), so a UT2004
	 * server's pawn keeps up to 2 UU off walls. Applied to falling moves and to mostly horizontal walking ones
	 * (walking floors keep their own offsets).
	 */
	bool bUE2MoveBackoff = false;
	/** MOVE_Custom mode: climbing a UE2 ladder (PHYS_Ladder). Net play sets it when the server says we climb. */
	static constexpr uint8 CustomLadder = 1;
	/** Ladder (UE2 LadderVolume.ClimbDir, Pawn.LadderSpeed in cm/s). */
	FVector LadderClimbDir = FVector::UpVector;
	float LadderSpeed = 400.f;
	bool IsOnLadder() const { return MovementMode == MOVE_Custom && CustomMovementMode == CustomLadder; }
	/** UE2 APawn::physLadder (simplified): along ClimbDir at LadderSpeed while the forward input is held, up or down. */
	virtual void PhysCustom(float DeltaTime, int32 Iterations) override;
	virtual bool MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& NewRotation, bool bSweep, FHitResult* OutHit = nullptr, ETeleportType Teleport = ETeleportType::None) override;
	/** UE2 PhysicsVolume.GroundFriction when the volume is not an AArenaPhysicsVolume. */
	float DefaultGroundFriction = 8.f;
	/** Velocity of the current movement base (lift), measured from its motion. */
	FVector GetBaseVelocityUE2() const { return BaseVelocity; }

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual float GetMaxSpeed() const override;
	/**
	 * UE2 APawn::stepUp: a walking pawn that runs into any barrier it cannot walk on (walls, slanted door frames, 47
	 * degree ramps, corners) steps up MaxStepHeight ("treat as vertical wall") even where UE5's own step up refuses, moves on (sliding along the barrier as if it were vertical) and drops back MaxStepHeight to the floor;
	 * when that lands on the steep face, or on nothing, it falls (UE2 physWalking). UE5 refuses such steps, so pawns
	 * stuck against them. Used when the engine's own step up fails.
	 */
	virtual bool StepUp(const FVector& GravDir, const FVector& Delta, const FHitResult& Hit, FStepDownResult* OutStepDownResult = nullptr) override;
	/** Steps taken by the UE2 rule above (tests). */
	int32 NumUE2StepUps = 0;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual bool CanCrouchInCurrentState() const override;
	virtual bool CanWalkOffLedges() const override;
	virtual void PhysicsVolumeChanged(class APhysicsVolume* NewVolume) override;
	/** UE2 physFalling: air control drops to 0.05 while it would push the pawn into a walkable slope. */
	virtual FVector GetAirControl(float DeltaTime, float TickAirControl, const FVector& FallAcceleration) override;
	/** UE2 physFalling gravity: the volume's full Gravity vector, fluid friction in water, TerminalVelocity. */
	virtual FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;
	/** UE2 processLanded: in a bBounceVelocity volume the pawn bounces off with ZoneVelocity instead of landing. */
	virtual void ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations) override;
	/**
	 * Net play's box collision (Net/UE2NetPlay.h UUE2NetBoxCollision: UE2's pawn against the world is an axis-aligned
	 * box of half size (R, R, H)): floors are found with that box, and a contact anywhere under its bottom face counts
	 * (UE2 physWalking / physFalling). The engine's capsule rules (a contact within the radius, the flat base inside
	 * the capsule) rejected the box's corners: the pawn walked off ledges a UE2 pawn still stands on, and hung in the
	 * falling state at the floor after a landing on a corner contact (often next to a wall).
	 */
	bool HasBoxCollision() const;
	virtual bool IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const override;
	virtual bool FloorSweepTest(FHitResult& OutHit, const FVector& Start, const FVector& End, ECollisionChannel TraceChannel, const struct FCollisionShape& CollisionShape,
		const struct FCollisionQueryParams& Params, const struct FCollisionResponseParams& ResponseParam) const override;
	/** Test hook (non-shipping): -UE2LogImpacts logs what a fast falling pawn (> 1500 cm/s) runs into. */
	virtual void HandleImpact(const FHitResult& Hit, float TimeSlice = 0.f, const FVector& MoveDelta = FVector::ZeroVector) override;
	/**
	 * Net play (Net/UE2NetPlay.h): one physics step outside the tick, as TickComponent runs it, with InputVector as the
	 * movement input (UE2 MoveAutonomous). Used to replay saved moves after a server correction.
	 */
	void SimulateNetStep(float DeltaTime, const FVector& InputVector);
	/** Last water jump-out (tests). */
	float LastWaterJumpVz = 0.f;
	int32 WaterJumps = 0;
	/** Air control used by the last falling step (tests). */
	float LastTickAirControl = 0.f;

private:
	/** UE2 ground friction of the current physics volume. */
	float GetUE2GroundFriction() const;
	void UpdateBaseVelocity(float DeltaTime);
	/** UE2 PhysicsVolume rules that are not part of the engine's phys modes (see the class comment). */
	void ApplyVolumePreMove(float DeltaTime);
	void ApplyZoneVelocity(float DeltaTime);
	/**
	 * UE2 terrain collides from its top side only (ATerrainInfo checks hit the surface's front): a pawn under a terrain,
	 * e.g. rising out of a pit through a hole in it (BR-Colossus' pit pads), passes through its underside. Terrain
	 * chunks above the pawn are ignored while it moves.
	 */
	void UpdateTerrainCollision();
	TArray<TWeakObjectPtr<UPrimitiveComponent>> IgnoredTerrain;

	FVector BaseVelocity = FVector::ZeroVector;
	TWeakObjectPtr<UPrimitiveComponent> TrackedBase;
	FVector TrackedBaseLocation = FVector::ZeroVector;
};

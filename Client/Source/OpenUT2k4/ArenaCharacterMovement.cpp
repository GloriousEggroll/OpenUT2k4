#include "ArenaCharacterMovement.h"
#include "ArenaCharacter.h"
#include "ArenaTypes.h"
#include "ArenaUE2World.h"
#include "ArenaWorldZones.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/CommandLine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "OpenUT2k4.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PhysicsVolume.h"

UArenaCharacterMovement::UArenaCharacterMovement()
{
	GravityScale = ArenaTuning::GravityScale;
	MaxWalkSpeed = ArenaTuning::GroundSpeed;
	MaxWalkSpeedCrouched = ArenaTuning::GroundSpeed * ArenaTuning::CrouchedPct;
	MaxSwimSpeed = ArenaTuning::WaterSpeed;
	// PHYS_Flying uses AirSpeed (xPawn 440 UU/s, as GroundSpeed); flying monsters set their own
	MaxFlySpeed = ArenaTuning::GroundSpeed;
	MaxAcceleration = ArenaTuning::AccelRate;
	// UE2 braking: Velocity -= 2 * Velocity * dt * GroundFriction, no constant deceleration.
	BrakingDecelerationWalking = 0.f;
	BrakingDecelerationFalling = 0.f;
	BrakingDecelerationSwimming = 0.f;
	BrakingFrictionFactor = 2.f;
	GroundFriction = DefaultGroundFriction;
	AirControl = ArenaTuning::AirControl;
	AirControlBoostMultiplier = 1.f;
	AirControlBoostVelocityThreshold = 0.f;
	FallingLateralFriction = 0.f;
	JumpZVelocity = ArenaTuning::JumpZ;
	MaxStepHeight = ArenaTuning::MaxStepHeight;
	SetWalkableFloorZ(ArenaTuning::WalkableFloorZ);
	// UE2 pawns are cylinders: stand fully on ledges.
	bUseFlatBaseForFloorChecks = true;
	PerchRadiusThreshold = 0.f;
	bCanWalkOffLedges = true;
	bCanWalkOffLedgesWhenCrouching = false;
	// Lifts: UE2 adds only the base's vertical speed, and only when jumping (AArenaCharacter::DoJumpUE2).
	bImpartBaseVelocityX = false;
	bImpartBaseVelocityY = false;
	bImpartBaseVelocityZ = false;
	bImpartBaseAngularVelocity = false;
	// UE2 Pawn Buoyancy 99 / Mass 100; water jump: PlayerSwimming.NotifyPhysicsVolumeChange.
	Buoyancy = 0.99f;
	JumpOutOfWaterPitch = 11.25f;
	MaxOutOfWaterStepHeight = 1.1f * ArenaTuning::MaxStepHeight;
	OutofWaterZ = 840.f + 2.f * ArenaTuning::CapsuleRadius;
	bOrientRotationToMovement = false;
}

void UArenaCharacterMovement::UpdateBaseVelocity(float DeltaTime)
{
	UPrimitiveComponent* Base = CharacterOwner ? Cast<UPrimitiveComponent>(CharacterOwner->GetMovementBaseObject()) : nullptr;
	if (Base && Base->Mobility != EComponentMobility::Movable)
	{
		Base = nullptr;
	}
	const FVector Loc = Base ? Base->GetComponentLocation() : FVector::ZeroVector;
	// The base's own velocity when it reports one (AArenaMover), else its motion since the last physics step.
	const FVector Reported = Base ? Base->GetComponentVelocity() : FVector::ZeroVector;
	BaseVelocity = !Reported.IsNearlyZero() ? Reported
		: (Base && Base == TrackedBase.Get() && DeltaTime > UE_KINDA_SMALL_NUMBER) ? (Loc - TrackedBaseLocation) / DeltaTime : FVector::ZeroVector;
	TrackedBase = Base;
	TrackedBaseLocation = Loc;
}

void UArenaCharacterMovement::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	UpdateBaseVelocity(DeltaTime);
	if (AArenaCharacter* Char = Cast<AArenaCharacter>(CharacterOwner))
	{
		Char->PreMovementUpdate(DeltaTime);
	}
	ApplyVolumePreMove(DeltaTime);
	UpdateTerrainCollision();
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	ApplyZoneVelocity(DeltaTime);
}

bool UArenaCharacterMovement::MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& NewRotation, bool bSweep, FHitResult* OutHit, ETeleportType Teleport)
{
	const double Size = Delta.Size();
	// walking: mostly horizontal moves only (the floor checks and step downs keep the floor distance); falling: all
	// (UE2 lands on a floor within 2 UU along the move, one step sooner than a plain sweep)
	if (!bUE2MoveBackoff || !bSweep || !UpdatedComponent || Size < UE_KINDA_SMALL_NUMBER || (MovementMode != MOVE_Falling && FMath::Abs(Delta.Z) > Delta.Size2D()))
	{
		return Super::MoveUpdatedComponentImpl(Delta, NewRotation, bSweep, OutHit, Teleport);
	}
	// UE2 ULevel::MoveActor: TestAdjust = 2; trace Delta + 2 along it; a hit: Time = ((Size + 2) * Time - 2) / Size, or 0
	const double Adjust = 2.0 * ArenaUE2World::Scale;
	const FVector Dir = Delta / Size;
	const FVector Start = UpdatedComponent->GetComponentLocation();
	FHitResult Hit(1.f);
	const bool bMoved = Super::MoveUpdatedComponentImpl(Delta + Dir * Adjust, NewRotation, true, &Hit, Teleport);
	if (Hit.bStartPenetrating)
	{
		if (OutHit)
		{
			*OutHit = Hit;
		}
		return bMoved;
	}
	double Travel = Size;
	if (Hit.bBlockingHit)
	{
		Travel = FMath::Max(0.0, double(Hit.Time) * (Size + Adjust) - Adjust);
		Hit.Time = float(Travel / Size);
		Hit.Distance = float(Travel);
	}
	// to Start + Dir * Travel (a short step back along the swept path: nothing to sweep)
	const FVector Back = Start + Dir * Travel - UpdatedComponent->GetComponentLocation();
	if (!Back.IsNearlyZero())
	{
		Super::MoveUpdatedComponentImpl(Back, NewRotation, false, nullptr, ETeleportType::None);
	}
	if (Hit.bBlockingHit)
	{
		Hit.Location = UpdatedComponent->GetComponentLocation();
		Hit.TraceEnd = Start + Delta;
	}
	if (OutHit)
	{
		*OutHit = Hit;
	}
	return bMoved;
}

void UArenaCharacterMovement::PhysCustom(float DeltaTime, int32 Iterations)
{
	if (CustomMovementMode != CustomLadder || !CharacterOwner || DeltaTime <= 0.f)
	{
		Super::PhysCustom(DeltaTime, Iterations);
		return;
	}
	// UE2 PlayerClimbing: Acceleration = aForward * ClimbDir; physLadder climbs at LadderSpeed, up when the acceleration
	// points along ClimbDir, down otherwise, and stops without input
	const FRotator Yaw(0.f, CharacterOwner->GetControlRotation().Yaw, 0.f);
	// the forward input (live), or the climb acceleration itself (net play replays moves with Accel = aForward * ClimbDir)
	const float Forward = float(FVector::DotProduct(Acceleration, Yaw.Vector()) + FVector::DotProduct(Acceleration, LadderClimbDir.GetSafeNormal()));
	const float Sign = FMath::Abs(Forward) < 1.f ? 0.f : FMath::Sign(Forward);
	Velocity = LadderClimbDir.GetSafeNormal() * LadderSpeed * Sign;
	if (Velocity.IsNearlyZero())
	{
		return;
	}
	FHitResult Hit;
	const FVector Delta = Velocity * DeltaTime;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Velocity.Z < 0.f && Hit.ImpactNormal.Z >= GetWalkableFloorZ())
		{
			// climbed down onto the floor
			SetMovementMode(MOVE_Walking);
			return;
		}
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
}

void UArenaCharacterMovement::SimulateNetStep(float DeltaTime, const FVector& InputVector)
{
	if (!HasValidData() || DeltaTime <= 0.f)
	{
		return;
	}
	UpdateBaseVelocity(DeltaTime);
	if (AArenaCharacter* Char = Cast<AArenaCharacter>(CharacterOwner))
	{
		Char->PreMovementUpdate(DeltaTime);
	}
	ApplyVolumePreMove(DeltaTime);
	UpdateTerrainCollision();
	// UCharacterMovementComponent::TickComponent for a locally controlled pawn: the input vector to an acceleration,
	// then PerformMovement
	ConsumeInputVector();
	ControlledCharacterMove(InputVector, DeltaTime);
	ApplyZoneVelocity(DeltaTime);
}

namespace ArenaTerrainLocal
{
	TAutoConsoleVariable<int32> CVarTerrainOneSided(TEXT("arena.UE2TerrainOneSided"), 1,
		TEXT("1: UE2 terrain blocks pawns from above only (UE2 behaviour); 0: from both sides."));

	struct FTerrainList
	{
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Comps;
		double CheckedAt = -100.0;
	};
	TMap<TWeakObjectPtr<UWorld>, FTerrainList>& Lists()
	{
		static TMap<TWeakObjectPtr<UWorld>, FTerrainList> L;
		return L;
	}
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& TerrainOf(UWorld* World)
	{
		TMap<TWeakObjectPtr<UWorld>, FTerrainList>& L = Lists();
		for (auto It = L.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}
		FTerrainList& T = L.FindOrAdd(World);
		const double Now = World->GetTimeSeconds();
		if (T.Comps.Num() == 0 && Now - T.CheckedAt > 5.0)
		{
			T.CheckedAt = Now;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->ActorHasTag(TEXT("ArenaUE2Terrain")))
				{
					TInlineComponentArray<UPrimitiveComponent*> Prims(*It);
					for (UPrimitiveComponent* C : Prims)
					{
						T.Comps.Add(C);
					}
				}
			}
		}
		return T.Comps;
	}
}

void UArenaCharacterMovement::UpdateTerrainCollision()
{
	UPrimitiveComponent* Me = UpdatedPrimitive;
	UWorld* World = GetWorld();
	if (!Me || !World || !CharacterOwner)
	{
		return;
	}
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Terrain = ArenaTerrainLocal::TerrainOf(World);
	const bool bOneSided = ArenaTerrainLocal::CVarTerrainOneSided.GetValueOnGameThread() != 0;
	if (Terrain.Num() == 0 && IgnoredTerrain.Num() == 0)
	{
		return;
	}
	const FVector Loc = UpdatedComponent->GetComponentLocation();
	const float R = CharacterOwner->GetSimpleCollisionRadius();
	const float H = CharacterOwner->GetSimpleCollisionHalfHeight();
	// the runtime map's pawn terrain comes in a copy per pawn shape (ArenaRuntimeMap PawnTerrain): skip the other one
	static const FName CapsuleTag(TEXT("ArenaTerrainCapsule")), BoxTag(TEXT("ArenaTerrainBox"));
	const FName OtherShape = Me->GetCollisionShape().IsBox() ? CapsuleTag : BoxTag;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> Below;
	for (const TWeakObjectPtr<UPrimitiveComponent>& Ptr : Terrain)
	{
		UPrimitiveComponent* T = Ptr.Get();
		if (!T)
		{
			continue;
		}
		if (T->GetOwner() && T->GetOwner()->ActorHasTag(OtherShape))
		{
			Below.Add(T);
			continue;
		}
		if (!bOneSided)
		{
			continue;
		}
		const FBox B = T->Bounds.GetBox().ExpandBy(FVector(R + 50.f, R + 50.f, 0.f));
		if (Loc.X < B.Min.X || Loc.X > B.Max.X || Loc.Y < B.Min.Y || Loc.Y > B.Max.Y || Loc.Z - H > B.Max.Z)
		{
			continue;
		}
		// the surface over the pawn's cylinder: above its head somewhere and nowhere under its feet = under the terrain
		if (Loc.Z + H < B.Min.Z - 200.f)
		{
			continue;
		}
		bool bOver = false, bUnder = false;
		static const FVector2D Offsets[] = { FVector2D(0, 0), FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1) };
		for (const FVector2D& O : Offsets)
		{
			const FVector P(Loc.X + O.X * (R + 20.f), Loc.Y + O.Y * (R + 20.f), 0.f);
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTerrainSide), true);
			if (T->LineTraceComponent(Hit, FVector(P.X, P.Y, B.Max.Z + 10.f), FVector(P.X, P.Y, Loc.Z - H - 2.f * MaxStepHeight), Q))
			{
				if (Hit.ImpactPoint.Z > Loc.Z + H - 5.f)
				{
					bOver = true;
				}
				else if (Hit.ImpactPoint.Z < Loc.Z - H + 5.f)
				{
					bUnder = true; // standing on it, or all the way through
				}
			}
		}
		// only in the air (a pawn walking under an overhang or into a bank keeps colliding as before); once passing through,
		// keep going until the surface is under the feet (a pawn half way through must not get stuck in it)
		if (IsFalling() && !bUnder && ((bOver && Velocity.Z > 0.f) || IgnoredTerrain.Contains(Ptr)))
		{
			Below.Add(T);
		}
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& Old : IgnoredTerrain)
	{
		if (UPrimitiveComponent* T = Old.Get(); T && !Below.Contains(Old))
		{
			Me->IgnoreComponentWhenMoving(T, false);
		}
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& New : Below)
	{
		if (UPrimitiveComponent* T = New.Get())
		{
			Me->IgnoreComponentWhenMoving(T, true);
		}
	}
#if !UE_BUILD_SHIPPING
	static const bool bLog = FParse::Param(FCommandLine::Get(), TEXT("UE2LogImpacts"));
	if (bLog && Below.Num() != IgnoredTerrain.Num())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: %s passes through %d terrain chunk(s) from below at %s (%d chunks)"), *CharacterOwner->GetName(), Below.Num(),
			*Loc.ToCompactString(), Terrain.Num());
	}
#endif
	IgnoredTerrain = MoveTemp(Below);
}

void UArenaCharacterMovement::ApplyVolumePreMove(float DeltaTime)
{
	const AArenaPhysicsVolume* V = Cast<AArenaPhysicsVolume>(GetPhysicsVolume());
	if (!V || !HasValidData() || (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		return;
	}
	// Gravity pointing up (BR-Colossus tubes: Gravity.Z = +1, BloodRun's wind: +0.1): nothing holds the pawn on the
	// floor, it floats and the rest of the gravity (or ZoneVelocity) carries it.
	if (IsMovingOnGround() && V->Gravity.Z > 0.f)
	{
		SetMovementMode(MOVE_Falling);
	}
	// UE2 physSwimming: Velocity += Gravity * dt * (1 - Buoyancy); the engine's swimming applies only the Z part
	else if (IsSwimming() && V->HasLateralGravity() && DeltaTime > 0.f)
	{
		const FVector G = V->GetGravityVector() * GravityScale * (1.f - FMath::Clamp(Buoyancy, 0.f, 1.f));
		Velocity += FVector(G.X, G.Y, 0.f) * DeltaTime;
	}
}

void UArenaCharacterMovement::ApplyZoneVelocity(float DeltaTime)
{
	const AArenaPhysicsVolume* V = Cast<AArenaPhysicsVolume>(GetPhysicsVolume());
	if (!V || V->ZoneVelocity.IsNearlyZero() || V->bBounceVelocity || DeltaTime <= 0.f || !HasValidData()
		|| !(IsMovingOnGround() || IsFalling() || IsSwimming()) || (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		return;
	}
	// UE2 physWalking / physFalling / physSwimming move by (Velocity + ZoneVelocity) * dt; Velocity itself is
	// unchanged (so no friction, speed clamp or air control acts on the zone's part)
	FVector Delta = V->ZoneVelocity * DeltaTime;
	if (IsMovingOnGround())
	{
		Delta.Z = 0.f;
	}
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
}

FVector UArenaCharacterMovement::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
	const AArenaPhysicsVolume* V = Cast<AArenaPhysicsVolume>(GetPhysicsVolume());
	if (!V || DeltaTime <= 0.f)
	{
		return Super::NewFallVelocity(InitialVelocity, Gravity, DeltaTime);
	}
	// UE2 NewFallVelocity: Velocity * (1 - FluidFriction * dt) in water + (Acceleration + PhysicsVolume.Gravity) * dt.
	// Gravity's Z comes from GetGravityZ (volume, GravityScale, test overrides); the volume adds its X / Y.
	FVector G = Gravity;
	if (V->HasLateralGravity())
	{
		const FVector Lateral = V->GetGravityVector() * GravityScale;
		G.X += Lateral.X;
		G.Y += Lateral.Y;
	}
	FVector Result = InitialVelocity;
	if (V->bWaterVolume)
	{
		Result *= 1.f - FMath::Min(V->FluidFriction * DeltaTime, 1.f);
	}
	Result += G * DeltaTime;
	return V->ClampToTerminalVelocity(Result);
}

void UArenaCharacterMovement::ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations)
{
	const AArenaPhysicsVolume* V = Cast<AArenaPhysicsVolume>(GetPhysicsVolume());
	if (V && V->bBounceVelocity && !V->ZoneVelocity.IsNearlyZero() && CharacterOwner && CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy)
	{
		// UE2 processLanded: no landing in a bounce zone, the pawn keeps falling with ZoneVelocity added (its speed
		// into the floor is dropped so it leaves the surface)
		Velocity = FVector::VectorPlaneProject(Velocity, Hit.ImpactNormal) + V->ZoneVelocity;
		return;
	}
	Super::ProcessLanded(Hit, remainingTime, Iterations);
}

bool UArenaCharacterMovement::HasBoxCollision() const
{
	return UpdatedPrimitive && UpdatedPrimitive->GetCollisionShape().IsBox();
}

bool UArenaCharacterMovement::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	if (!HasBoxCollision())
	{
		return Super::IsWithinEdgeTolerance(CapsuleLocation, TestImpactPoint, CapsuleRadius);
	}
	// the box's bottom face: a square of half size R (the box does not turn), less the engine's edge margin
	const FVector D = ProjectToGravityFloor(TestImpactPoint - CapsuleLocation);
	const double Reduced = FMath::Max(double(SWEEP_EDGE_REJECT_DISTANCE + UE_KINDA_SMALL_NUMBER), double(CapsuleRadius - SWEEP_EDGE_REJECT_DISTANCE));
	return FMath::Abs(D.X) < Reduced && FMath::Abs(D.Y) < Reduced;
}

bool UArenaCharacterMovement::FloorSweepTest(FHitResult& OutHit, const FVector& Start, const FVector& End, ECollisionChannel TraceChannel, const FCollisionShape& CollisionShape,
	const FCollisionQueryParams& Params, const FCollisionResponseParams& ResponseParam) const
{
	if (!HasBoxCollision() || !CollisionShape.IsCapsule())
	{
		return Super::FloorSweepTest(OutHit, Start, End, TraceChannel, CollisionShape, Params, ResponseParam);
	}
	// the engine's (shrunk) capsule as the box it stands for: half size (radius, radius, half height), axis-aligned.
	// A walkable contact of the box is the floor (also under a corner that the capsule's flat base does not reach);
	// otherwise (the box catches a step's edge, whose contact normal is not walkable) the engine's own floor sweep
	// decides, as before: with the box alone the pawn slid down stairs it pressed against.
	const FCollisionShape Box = FCollisionShape::MakeBox(FVector(CollisionShape.GetCapsuleRadius(), CollisionShape.GetCapsuleRadius(), CollisionShape.GetCapsuleHalfHeight()));
	FHitResult BoxHit(1.f);
	const bool bBoxHit = GetWorld()->SweepSingleByChannel(BoxHit, Start, End, FQuat::Identity, TraceChannel, Box, Params, ResponseParam);
	if (bBoxHit && !BoxHit.bStartPenetrating && IsWalkable(BoxHit))
	{
		OutHit = BoxHit;
		return true;
	}
	if (Super::FloorSweepTest(OutHit, Start, End, TraceChannel, CollisionShape, Params, ResponseParam))
	{
		return true;
	}
	OutHit = BoxHit;
	return bBoxHit;
}

float UArenaCharacterMovement::GetMaxSpeed() const
{
	switch (MovementMode)
	{
	case MOVE_Walking:
	case MOVE_NavWalking:
		if (IsCrouching())
		{
			return MaxWalkSpeed * ArenaTuning::CrouchedPct;
		}
		return bUE2Walking ? MaxWalkSpeed * ArenaTuning::WalkingPct : MaxWalkSpeed;
	case MOVE_Falling:
		// xPawn AirSpeed == GroundSpeed
		return MaxWalkSpeed;
	default:
		return Super::GetMaxSpeed();
	}
}

bool UArenaCharacterMovement::StepUp(const FVector& GravDir, const FVector& Delta, const FHitResult& InHit, FStepDownResult* OutStepDownResult)
{
	if (Super::StepUp(GravDir, Delta, InHit, OutStepDownResult))
	{
		return true;
	}
	if (!IsMovingOnGround() || MaxStepHeight <= 0.f || GravDir.IsZero() || !UpdatedComponent || !CharacterOwner)
	{
		return false;
	}
	// UE2 stepUp steps against every barrier the pawn walks into (UE5 refuses where the step would end on a steep face
	// or in a corner); walkable slopes are the engine's ramp moves, overhangs facing down are left alone
	const float NormalZ = FVector::DotProduct(InHit.Normal, -GravDir);
	if (NormalZ < -0.08f || IsWalkable(InHit))
	{
		return false;
	}
	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FQuat Rot = UpdatedComponent->GetComponentQuat();
	FScopedMovementUpdate ScopedStep(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// step up - treat as vertical wall
	FHitResult UpHit(1.f);
	MoveUpdatedComponent(-GravDir * MaxStepHeight, Rot, true, &UpHit);
	if (UpHit.bStartPenetrating || FVector::DotProduct(UpdatedComponent->GetComponentLocation() - OldLocation, -GravDir) < 1.f)
	{
		ScopedStep.RevertMove();
		return false;
	}
	// forward, then (UE2) slide along the barrier as if it were vertical
	FHitResult Hit(1.f);
	MoveUpdatedComponent(Delta, Rot, true, &Hit);
	if (Hit.bStartPenetrating)
	{
		ScopedStep.RevertMove();
		return false;
	}
	if (Hit.IsValidBlockingHit())
	{
		// UE2: "Hit.Normal.Z = 0; treat barrier as vertical", then slide along it
		const FVector N = (Hit.Normal - (-GravDir) * FVector::DotProduct(Hit.Normal, -GravDir)).GetSafeNormal();
		FVector Slide = (Delta - N * FVector::DotProduct(Delta, N)) * (1.f - Hit.Time);
		if (FVector::DotProduct(Slide, Delta) >= 0.f && !Slide.IsNearlyZero())
		{
			FHitResult SlideHit(1.f);
			MoveUpdatedComponent(Slide, Rot, true, &SlideHit);
			if (SlideHit.IsValidBlockingHit() && !SlideHit.bStartPenetrating)
			{
				TwoWallAdjust(Slide, SlideHit, N);
				MoveUpdatedComponent(Slide, Rot, true, &SlideHit);
			}
		}
	}
	// physWalking: drop to the floor (MaxStepHeight + a little); on a steep face the pawn falls (FindFloor / CheckFall)
	const FVector DropStart = UpdatedComponent->GetComponentLocation();
	FHitResult DownHit(1.f);
	MoveUpdatedComponent(GravDir * (MaxStepHeight + 2.f * MAX_FLOOR_DIST), Rot, true, &DownHit);
	++NumUE2StepUps;
	if (!DownHit.IsValidBlockingHit() || !IsWalkable(DownHit))
	{
		// UE2 physWalking: no floor, or one too steep (Hit.Normal.Z < MINFLOORZ), within the step: the drop is undone
		// and the pawn falls from the stepped-up height (a UT2004 server's pawn rises a full step and goes airborne)
		FHitResult BackHit(1.f);
		MoveUpdatedComponent(DropStart - UpdatedComponent->GetComponentLocation(), Rot, true, &BackHit);
		SetMovementMode(MOVE_Falling);
	}
	return true;
}

float UArenaCharacterMovement::GetUE2GroundFriction() const
{
	if (const AArenaPhysicsVolume* V = Cast<AArenaPhysicsVolume>(GetPhysicsVolume()))
	{
		return V->GroundFriction;
	}
	return DefaultGroundFriction;
}

void UArenaCharacterMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	const bool bWalkingMode = IsMovingOnGround();
	if (!HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME || bHasRequestedVelocity || bForceMaxAccel
		|| !(bWalkingMode || IsFalling() || IsSwimming())
		|| (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		// UE2 calcVelocity (also PHYS_Flying) has no analog speed cap: a partial input still reaches the full speed
		if (IsFlying() && !Acceleration.IsZero())
		{
			AnalogInputModifier = 1.f;
		}
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
		return;
	}

	const bool bZeroAcceleration = Acceleration.IsZero();
	// UE2 APawn::calcVelocity has no analog speed cap (UE5's AnalogInputModifier): a partial input (gamepad stick)
	// accelerates more slowly (Acceleration scales with the input) but reaches the full speed, offline and in net play.
	const float MaxSpeed = GetMaxSpeed();

	if (IsFalling())
	{
		// UE2 physFalling: Acceleration is AccelRate * AirControl (GetFallingLateralAcceleration) and Velocity.Z was
		// zeroed by the caller. Air control steers; a horizontal speed above the maximum is bound to itself (BoundSpeed),
		// one at or below it is not bound: one step of air control may take it past the maximum (a 3369 server's
		// pawn jumping forward at 440 UU/s goes on at 440 + AccelRate * AirControl * dt).
		if (!bZeroAcceleration)
		{
			const float Speed = Velocity.Size();
			Velocity += Acceleration * DeltaTime;
			if (Speed > MaxSpeed)
			{
				Velocity = Velocity.GetClampedToMaxSize(Speed);
			}
		}
		return;
	}

	// UE2 APawn::calcVelocity (walking: bBrake; swimming: bFluid with the volume's FluidFriction).
	if (IsSwimming())
	{
		Friction = GetPhysicsVolume() ? GetPhysicsVolume()->FluidFriction : Friction;
	}
	else
	{
		Friction = GetUE2GroundFriction();
	}
	Friction = FMath::Max(0.f, Friction);

	if (bZeroAcceleration)
	{
		if (!bFluid && bUE2ServerBraking && bWalkingMode)
		{
			// UE2 APawn::calcVelocity, braking (read from the 3369 server binary): 0.03 s steps of
			// V -= 2 * V * step * Friction, and the velocity is the time average of the steps' velocities (only those
			// still pointing the old way); zero below 10 UU/s
			const FVector Old = Velocity;
			FVector Average = FVector::ZeroVector;
			float Remaining = DeltaTime;
			while (Remaining > 0.03f)
			{
				Velocity -= Velocity * (0.06f * Friction);
				if ((Velocity | Old) > 0.f)
				{
					Average += Velocity * (0.03f / DeltaTime);
				}
				Remaining -= 0.03f;
			}
			Velocity -= (Velocity + Velocity) * Friction * Remaining;
			if ((Velocity | Old) > 0.f)
			{
				Average += Velocity * (Remaining / DeltaTime);
			}
			Velocity = Average;
			if ((Velocity | Old) < 0.f || Velocity.SizeSquared() < FMath::Square(20.f))
			{
				Velocity = FVector::ZeroVector;
			}
		}
		else if (!bFluid)
		{
			const FVector OldVelocity = Velocity;
			Velocity -= (2.f * Velocity) * FMath::Min(DeltaTime * Friction, 0.5f);
			if ((OldVelocity | Velocity) <= 0.f || Velocity.SizeSquared() < UE_KINDA_SMALL_NUMBER)
			{
				Velocity = FVector::ZeroVector;
			}
		}
	}
	else
	{
		const float VelSize = Velocity.Size();
		if (VelSize > 0.f)
		{
			const FVector AccelDir = Acceleration.GetSafeNormal();
			Velocity -= (Velocity - AccelDir * VelSize) * FMath::Min(DeltaTime * Friction, 1.f);
		}
	}

	if (bFluid)
	{
		Velocity *= 1.f - FMath::Min(Friction * DeltaTime, 1.f);
	}
	Velocity += Acceleration * DeltaTime;
	Velocity = Velocity.GetClampedToMaxSize(MaxSpeed);
}

bool UArenaCharacterMovement::CanCrouchInCurrentState() const
{
	if (!Super::CanCrouchInCurrentState())
	{
		return false;
	}
	// UE2 PlayerWalking.ProcessMove only changes crouching when not falling (a crouched pawn stays crouched in the air).
	return !IsFalling() || IsCrouching();
}

bool UArenaCharacterMovement::CanWalkOffLedges() const
{
	if (bUE2Walking || bStopAtLedges)
	{
		return false;
	}
	return Super::CanWalkOffLedges();
}

void UArenaCharacterMovement::PhysicsVolumeChanged(APhysicsVolume* NewVolume)
{
	if (!HasValidData() || (NewVolume && NewVolume->bWaterVolume) || !IsSwimming())
	{
		Super::PhysicsVolumeChanged(NewVolume);
		return;
	}
	// Left the water (UE2 PlayerController.PlayerSwimming.NotifyPhysicsVolumeChange): falling; swimming forwards and
	// up (or looking up) against a low ledge jumps out with max(JumpZ, 420) + 2 * CollisionRadius.
	SetMovementMode(MOVE_Falling);
	const AController* C = CharacterOwner->GetController();
	const FRotator ViewRot = C ? C->GetControlRotation() : UpdatedComponent->GetComponentRotation();
	const FVector X = ViewRot.Vector();
	const bool bUpAndOut = (X | Acceleration) > 0.f && (Acceleration.Z > 0.f || FRotator::NormalizeAxis(ViewRot.Pitch) > JumpOutOfWaterPitch);
	FVector WallNormal(0.f);
	if (Velocity.Z > 0.f && bUpAndOut && CheckWaterJump(X, WallNormal))
	{
		const float Radius = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius();
		Velocity.Z = FMath::Max(JumpZVelocity, 840.f) + 2.f * Radius;
		LastWaterJumpVz = Velocity.Z;
		++WaterJumps;
	}
}


FVector UArenaCharacterMovement::GetAirControl(float DeltaTime, float TickAirControl, const FVector& FallAcceleration)
{
	// UE2 APawn::physFalling: TestWalk = (AirControl * AccelRate * AccelDir + Velocity) * dt (horizontal); when that
	// move would hit a walkable floor (normal Z >= MINFLOORZ), air control is cut to 0.05 so it can't climb slopes.
	// When it would hit a wall, there is no air control at all for the step: a UT2004 server's pawn that jumps or
	// dodges while pressing into a wall keeps its horizontal velocity exactly (measured with -UE2NetPlayProbe against
	// a 3369 server: no drift towards the wall, no air control along it).
	if (TickAirControl > 0.05f && UpdatedComponent && CharacterOwner)
	{
		FVector TestWalk = (TickAirControl * GetMaxAcceleration() * FallAcceleration.GetSafeNormal() + Velocity) * DeltaTime;
		TestWalk.Z = 0.f;
		if (!TestWalk.IsNearlyZero())
		{
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaAirControlSlope), false, CharacterOwner);
			FCollisionResponseParams ResponseParam;
			InitCollisionParams(Params, ResponseParam);
			const FVector Start = UpdatedComponent->GetComponentLocation();
			// the pawn's own collision shape (net play: the UE2 box, Net/UE2NetPlay.h UUE2NetBoxCollision)
			const FCollisionShape Shape = UpdatedPrimitive ? UpdatedPrimitive->GetCollisionShape() : GetPawnCapsuleCollisionShape(SHRINK_None);
			if (GetWorld()->SweepSingleByChannel(Hit, Start, Start + TestWalk, UpdatedComponent->GetComponentQuat(), UpdatedComponent->GetCollisionObjectType(),
				Shape, Params, ResponseParam) && !Hit.bStartPenetrating)
			{
				TickAirControl = Hit.ImpactNormal.Z >= GetWalkableFloorZ() ? 0.05f : 0.f;
			}
		}
	}
	LastTickAirControl = TickAirControl;
	return TickAirControl * FallAcceleration;
}

void UArenaCharacterMovement::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);
#if !UE_BUILD_SHIPPING
	static const bool bLog = FParse::Param(FCommandLine::Get(), TEXT("UE2LogImpacts"));
	if (bLog && IsFalling() && Velocity.Size() > 1500.f && CharacterOwner)
	{
		const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Hit.GetComponent());
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: %s hit %s/%s (%s) at %s normal %s, speed %.0f"), *CharacterOwner->GetName(), *GetNameSafe(Hit.GetActor()),
			*GetNameSafe(Hit.GetComponent()), SMC ? *GetNameSafe(SMC->GetStaticMesh()) : TEXT("-"), *Hit.ImpactPoint.ToCompactString(),
			*Hit.ImpactNormal.ToCompactString(), Velocity.Size());
	}
#endif
}

#include "ArenaINV_MonsterController.h"
#include "OpenUT2k4.h"
#include "ArenaINV_Monster.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaNavGraph.h"
#include "ArenaDamage.h"
#include "ArenaProjectile.h"
#include "ArenaAssets.h"
#include "ArenaFX.h"
#include "ArenaTypes.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"

namespace ArenaINVAI
{
	constexpr float TargetInterval = 0.4f;
	constexpr float RepathInterval = 2.5f;
	/** Nav graph nodes sit at floor + standard capsule half height. */
	constexpr float NavNodeHeight = ArenaTuning::CapsuleHalfHeight;

}

AArenaINV_MonsterController::AArenaINV_MonsterController()
{
	PrimaryActorTick.bCanEverTick = true;
	bWantsPlayerState = false; // the Invasion rules create a hidden (inactive) player state for kill messages
	bAttachToPawn = false;
}

AArenaINV_Monster* AArenaINV_MonsterController::GetMonster() const
{
	return Cast<AArenaINV_Monster>(GetPawn());
}

void AArenaINV_MonsterController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	Rng.Initialize(GetUniqueID() * 7919 + 17);
	const float Now = GetWorld()->GetTimeSeconds();
	LastProgressTime = Now;
	NextSpecialTime = Now + Rng.FRandRange(1.f, 2.5f);
	OrbitAngle = Rng.FRandRange(0.f, 2.f * PI);
	OrbitDir = Rng.FRand() < 0.5f ? -1.f : 1.f;
	if (InPawn)
	{
		LastPosition = InPawn->GetActorLocation();
		DesiredRotation = InPawn->GetActorRotation();
		SetControlRotation(DesiredRotation);
	}
}

void AArenaINV_MonsterController::OnUnPossess()
{
	Super::OnUnPossess();
	// Monsters live for one pawn: the controller (and its hidden player state) goes away shortly after.
	SetLifeSpan(0.5f);
}

void AArenaINV_MonsterController::ResetAfterTeleport()
{
	Path.Reset();
	PathLinks.Reset();
	NextRepathTime = 0.f;
	StuckTime = 0.f;
	AttackPhase = 0;
	PendingAttack = 0;
	const float Now = GetWorld()->GetTimeSeconds();
	LastProgressTime = Now;
	if (APawn* P = GetPawn())
	{
		LastPosition = P->GetActorLocation();
	}
}

void AArenaINV_MonsterController::UpdateControlRotation(float DeltaTime, bool bUpdatePawn)
{
	APawn* P = GetPawn();
	if (!P)
	{
		return;
	}
	const AArenaINV_Monster* M = GetMonster();
	const float TurnRate = (M && M->IsBoss()) ? 110.f : ((M && M->GetDef().Health >= 300.f) ? 200.f : 540.f);
	FRotator Current = GetControlRotation().GetNormalized();
	const FRotator Wanted = DesiredRotation.GetNormalized();
	const float DeltaYaw = FRotator::NormalizeAxis(Wanted.Yaw - Current.Yaw);
	Current.Yaw = FRotator::NormalizeAxis(Current.Yaw + FMath::Clamp(DeltaYaw, -TurnRate * DeltaTime, TurnRate * DeltaTime));
	Current.Pitch = FMath::FInterpTo(Current.Pitch, FMath::Clamp(Wanted.Pitch, -80.f, 80.f), DeltaTime, 6.f);
	Current.Roll = 0.f;
	SetControlRotation(Current);
	if (bUpdatePawn)
	{
		P->FaceRotation(Current, DeltaTime);
	}
}

// ---------------------------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------------------------

void AArenaINV_MonsterController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AArenaINV_Monster* M = GetMonster();
	if (!M || M->IsDead())
	{
		return;
	}
	UWorld* World = GetWorld();
	const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
	if (GM && !GM->IsPlaying())
	{
		M->SetChargeGlow(0.f);
		AttackPhase = 0;
		return;
	}
	const float Now = World->GetTimeSeconds();

	if (bJumpHeld)
	{
		M->StopJumping();
		bJumpHeld = false;
	}

	UpdateTarget(Now);
	if (M->IsFlyer())
	{
		UpdateFlyingMovement(DeltaSeconds, Now);
	}
	else
	{
		UpdateGroundMovement(DeltaSeconds, Now);
	}
	UpdateAttack(DeltaSeconds, Now);

	// Progress tracking (the rules recall monsters that make none for a long time)
	const FVector Loc = M->GetActorLocation();
	if (FVector::DistSquared(Loc, LastPosition) > FMath::Square(150.f))
	{
		LastPosition = Loc;
		LastProgressTime = Now;
	}
}

// ---------------------------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------------------------

bool AArenaINV_MonsterController::HasClearShot(const FVector& From, const AActor* Other) const
{
	if (!Other)
	{
		return false;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(INVMonsterSight), false, GetPawn());
	Q.AddIgnoredActor(Other);
	FCollisionResponseParams RP;
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	RP.CollisionResponse.SetResponse(ECC_PhysicsBody, ECR_Ignore);
	FHitResult Hit;
	return !GetWorld()->LineTraceSingleByChannel(Hit, From, Other->GetActorLocation() + FVector(0.f, 0.f, 30.f), ECC_Visibility, Q, RP);
}

void AArenaINV_MonsterController::UpdateTarget(float Now)
{
	AArenaINV_Monster* M = GetMonster();
	AArenaCharacter* Current = Target.Get();
	if (Current && (!IsValid(Current) || Current->IsDead()))
	{
		Target = nullptr;
		Current = nullptr;
		NextTargetTime = 0.f;
	}
	if (Now < NextTargetTime)
	{
		return;
	}
	NextTargetTime = Now + ArenaINVAI::TargetInterval * Rng.FRandRange(0.8f, 1.2f);

	const FVector Eye = M->GetPawnViewLocation();
	AArenaCharacter* Best = nullptr;
	bool bBestVisible = false;
	float BestScore = TNumericLimits<float>::Max();
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (!IsValid(C) || C->IsDead() || C->IsA<AArenaINV_Monster>() || !C->GetController())
		{
			continue;
		}
		const float Dist = FVector::Dist(C->GetActorLocation(), M->GetActorLocation());
		if (C->IsInvisible() && Dist > 700.f && C != Current)
		{
			continue;
		}
		const bool bVisible = Dist < 6000.f && HasClearShot(Eye, C);
		float Score = Dist * (bVisible ? 0.5f : 1.f);
		if (C == Current)
		{
			Score *= 0.75f;
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = C;
			bBestVisible = bVisible;
		}
	}
	if (Best != Current)
	{
		Path.Reset();
		PathLinks.Reset();
		NextRepathTime = 0.f;
	}
	if (Best && Best != Current && bBestVisible && Now - LastChallengeTime > 6.f)
	{
		LastChallengeTime = Now;
		M->PlayMonsterSound(M->GetDef().Sounds.Challenge, 0.9f);
	}
	Target = Best;
	bTargetVisible = Best && bBestVisible;
	if (bTargetVisible)
	{
		LastSeenTargetTime = Now;
	}
}

// ---------------------------------------------------------------------------------------------
// Ground movement
// ---------------------------------------------------------------------------------------------

bool AArenaINV_MonsterController::IsWalkableStraight(const FVector& From, const FVector& To) const
{
	const AArenaINV_Monster* M = GetMonster();
	const float HalfHeight = M->GetDef().CapsuleHalfHeight;
	const FVector FromFeet = From - FVector(0.f, 0.f, HalfHeight);
	if (FMath::Abs(To.Z - From.Z) > 140.f + HalfHeight)
	{
		return false;
	}
	FCollisionObjectQueryParams Obj(ECC_WorldStatic);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(INVMonsterWalk), false, GetPawn());
	const FVector Knee(0.f, 0.f, 50.f);
	if (GetWorld()->LineTraceTestByObjectType(FromFeet + Knee, FVector(To.X, To.Y, FromFeet.Z) + Knee, Obj, Q))
	{
		return false;
	}
	// Floor at the midpoint and three quarters (no pits in between)
	for (const float Alpha : { 0.5f, 0.8f })
	{
		const FVector P = FMath::Lerp(FromFeet, FVector(To.X, To.Y, FromFeet.Z), Alpha) + FVector(0.f, 0.f, 40.f);
		if (!GetWorld()->LineTraceTestByObjectType(P, P - FVector(0.f, 0.f, 260.f), Obj, Q))
		{
			return false;
		}
	}
	return true;
}

void AArenaINV_MonsterController::Repath(const FVector& Goal, float Now)
{
	AArenaINV_Monster* M = GetMonster();
	const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	AArenaNavGraph* Nav = GM ? GM->GetNavGraph() : nullptr;
	NextRepathTime = Now + ArenaINVAI::RepathInterval * Rng.FRandRange(0.8f, 1.2f);
	PathGoal = Goal;
	Path.Reset();
	PathLinks.Reset();
	if (!Nav || Nav->NumNodes() == 0)
	{
		bPathFailed = true;
		return;
	}
	// The graph lives at standard capsule height: query from our feet + that height.
	const FVector Start = M->GetFeetLocation() + FVector(0.f, 0.f, ArenaINVAI::NavNodeHeight);
	bPathFailed = !Nav->FindPath(Start, Goal, Path, PathLinks);
}

FVector AArenaINV_MonsterController::GetPathDirection(const FVector& Goal, float Now, bool& bOutJumpLink)
{
	bOutJumpLink = false;
	AArenaINV_Monster* M = GetMonster();
	const FVector Loc = M->GetActorLocation();

	if (bTargetVisible && IsWalkableStraight(Loc, Goal))
	{
		Path.Reset();
		PathLinks.Reset();
		return (Goal - Loc).GetSafeNormal2D();
	}

	const bool bGoalMoved = FVector::DistSquared(Goal, PathGoal) > FMath::Square(400.f);
	if ((Path.Num() == 0 || bGoalMoved || bPathFailed) && Now >= NextRepathTime)
	{
		Repath(Goal, Now);
	}

	const float NavZ = M->GetFeetLocation().Z + ArenaINVAI::NavNodeHeight;
	const float Reach = FMath::Max(90.f, M->GetDef().CapsuleRadius + 50.f);
	while (Path.Num() > 0)
	{
		const FVector& Pt = Path[0];
		const float D2 = FVector::Dist2D(Loc, Pt);
		const float Dz = NavZ - Pt.Z;
		if (D2 < Reach && Dz > -60.f && Dz < 160.f)
		{
			Path.RemoveAt(0);
			PathLinks.RemoveAt(0);
			continue;
		}
		break;
	}
	if (Path.Num() > 0)
	{
		bOutJumpLink = PathLinks[0] == EArenaNavLinkType::Jump;
		return (Path[0] - Loc).GetSafeNormal2D();
	}
	return (Goal - Loc).GetSafeNormal2D();
}

void AArenaINV_MonsterController::UpdateGroundMovement(float DeltaSeconds, float Now)
{
	AArenaINV_Monster* M = GetMonster();
	UCharacterMovementComponent* Move = M->GetCharacterMovement();
	AArenaCharacter* T = Target.Get();
	const FVector Loc = M->GetActorLocation();
	const bool bOnGround = Move->IsMovingOnGround();

	// Rooted while winding up an attack or flying through a leap
	if (AttackPhase != 0)
	{
		if (T)
		{
			DesiredRotation = (T->GetActorLocation() - Loc).Rotation();
		}
		return;
	}

	if (!T)
	{
		// Nothing to hunt: idle in place (waves only spawn while players are alive)
		return;
	}

	const FVector TLoc = T->GetActorLocation();
	const float Dist2D = FVector::Dist2D(Loc, TLoc);
	const float SurfaceDist = Dist2D - M->GetDef().CapsuleRadius - ArenaTuning::CapsuleRadius;
	FVector Dir = FVector::ZeroVector;
	bool bJumpLink = false;

	if (M->GetBehavior() == EArenaINVBehavior::Spitter && bTargetVisible && Dist2D < 1500.f && SurfaceDist > M->GetDef().MeleeReach)
	{
		// Keep a comfortable lobbing distance: back off when crowded, sidestep otherwise
		const FVector Away = (Loc - TLoc).GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Away) * OrbitDir;
		if (Now > NextOrbitFlip)
		{
			OrbitDir = -OrbitDir;
			NextOrbitFlip = Now + Rng.FRandRange(1.5f, 3.f);
		}
		Dir = Dist2D < 700.f ? (Away * 0.9f + Side * 0.4f) : Side * 0.6f;
		// Don't back off a ledge
		const FVector Probe = M->GetFeetLocation() + Dir.GetSafeNormal() * 120.f + FVector(0.f, 0.f, 40.f);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(INVMonsterLedge), false, M);
		if (!GetWorld()->LineTraceTestByObjectType(Probe, Probe - FVector(0.f, 0.f, 200.f), FCollisionObjectQueryParams(ECC_WorldStatic), Q))
		{
			Dir = -Dir;
		}
	}
	else if (SurfaceDist > M->GetDef().MeleeReach * 0.6f)
	{
		Dir = GetPathDirection(TLoc, Now, bJumpLink);
	}

	if (!Dir.IsNearlyZero())
	{
		M->AddMovementInput(Dir.GetSafeNormal2D(), 1.f);
	}

	// Face the target when close and visible, else where we go
	if (bTargetVisible && Dist2D < 2500.f)
	{
		DesiredRotation = (TLoc - Loc).Rotation();
	}
	else if (!Dir.IsNearlyZero())
	{
		DesiredRotation = Dir.Rotation();
	}

	// Jumps: path jump links, and getting unstuck
	const float Speed2D = M->GetVelocity().Size2D();
	const bool bTryingToMove = !Dir.IsNearlyZero();
	if (bTryingToMove && bOnGround && Speed2D < M->GetDef().Speed * 0.15f)
	{
		StuckTime += DeltaSeconds;
	}
	else if (Speed2D > M->GetDef().Speed * 0.4f)
	{
		StuckTime = FMath::Max(0.f, StuckTime - DeltaSeconds);
	}

	if (bOnGround && Now >= NextJumpTime)
	{
		const bool bNeedJumpLink = bJumpLink && Path.Num() > 0 && FVector::Dist2D(Loc, Path[0]) < 350.f;
		if (bNeedJumpLink || StuckTime > 0.8f)
		{
			M->Jump();
			bJumpHeld = true;
			bWantDoubleJump = true;
			NextJumpTime = Now + 0.9f;
			if (StuckTime > 3.f)
			{
				// Really stuck: forget the path and try again
				Path.Reset();
				PathLinks.Reset();
				NextRepathTime = 0.f;
				StuckTime = 0.f;
			}
		}
	}
	else if (bWantDoubleJump && Move->IsFalling() && M->GetVelocity().Z < 60.f)
	{
		bWantDoubleJump = false;
		const bool bNeedHeight = Path.Num() > 0 && Path[0].Z > M->GetFeetLocation().Z + ArenaINVAI::NavNodeHeight + 40.f;
		if (bNeedHeight || StuckTime > 0.8f)
		{
			M->Jump();
			bJumpHeld = true;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Flying movement
// ---------------------------------------------------------------------------------------------

FVector AArenaINV_MonsterController::AvoidObstacles3D(const FVector& Dir, float LookAhead) const
{
	const APawn* P = GetPawn();
	if (!P || Dir.IsNearlyZero())
	{
		return Dir;
	}
	const FVector Loc = P->GetActorLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(INVFlyerAvoid), false, P);
	FCollisionObjectQueryParams Obj(ECC_WorldStatic);
	FHitResult Hit;
	FVector Result = Dir;
	if (GetWorld()->SweepSingleByObjectType(Hit, Loc, Loc + Dir * LookAhead, FQuat::Identity, Obj, FCollisionShape::MakeSphere(40.f), Q))
	{
		// Slide along the obstacle and climb a little
		FVector Slide = FVector::VectorPlaneProject(Dir, Hit.ImpactNormal);
		if (Slide.IsNearlyZero())
		{
			Slide = FVector::CrossProduct(Hit.ImpactNormal, FVector::UpVector);
		}
		Result = (Slide.GetSafeNormal() + Hit.ImpactNormal * 0.5f + FVector(0.f, 0.f, 0.4f)).GetSafeNormal();
	}
	// Keep off the floor and the ceiling
	if (GetWorld()->LineTraceSingleByObjectType(Hit, Loc, Loc - FVector(0.f, 0.f, 140.f), Obj, Q))
	{
		Result.Z = FMath::Max(Result.Z, 0.5f);
	}
	if (GetWorld()->LineTraceSingleByObjectType(Hit, Loc, Loc + FVector(0.f, 0.f, 90.f), Obj, Q))
	{
		Result.Z = FMath::Min(Result.Z, -0.3f);
	}
	return Result.GetSafeNormal();
}

void AArenaINV_MonsterController::UpdateFlyingMovement(float DeltaSeconds, float Now)
{
	AArenaINV_Monster* M = GetMonster();
	AArenaCharacter* T = Target.Get();
	const FVector Loc = M->GetActorLocation();
	if (!T)
	{
		// hover in place
		M->AddMovementInput(AvoidObstacles3D(FVector(0.f, 0.f, 0.1f), 100.f), 0.2f);
		return;
	}
	const FVector TLoc = T->GetActorLocation();

	if (Now > NextOrbitFlip)
	{
		OrbitDir = Rng.FRand() < 0.35f ? -OrbitDir : OrbitDir;
		NextOrbitFlip = Now + Rng.FRandRange(2.f, 4.f);
	}
	OrbitAngle += DeltaSeconds * 0.7f * OrbitDir;

	FVector Desired;
	const bool bLostSight = Now - LastSeenTargetTime > 2.f;
	if (bLostSight || FVector::Dist(Loc, TLoc) > 2800.f)
	{
		// Follow the ground route (raised) towards the target
		bool bJump = false;
		const FVector PathDir = GetPathDirection(TLoc, Now, bJump);
		const float WantZ = (Path.Num() > 0 ? Path[0].Z : TLoc.Z) + 140.f;
		Desired = Loc + PathDir * 400.f;
		Desired.Z = WantZ;
	}
	else if (M->GetBehavior() == EArenaINVBehavior::FlyerMelee)
	{
		// Dive at the target (Razorfly / Manta), slightly above its centre; swerve around it between bites.
		const bool bJustBit = Now < NextMeleeTime - M->GetDef().MeleeInterval * 0.4f;
		const FVector Side = FVector::CrossProduct(FVector::UpVector, (TLoc - Loc).GetSafeNormal2D()) * OrbitDir;
		Desired = TLoc + FVector(0.f, 0.f, 40.f) + (bJustBit ? Side * 260.f + FVector(0.f, 0.f, 160.f) : FVector::ZeroVector);
	}
	else
	{
		// Strafe in a circle above the target
		const float Radius = 620.f;
		Desired = TLoc + FVector(FMath::Cos(OrbitAngle) * Radius, FMath::Sin(OrbitAngle) * Radius, 300.f);
		// Don't orbit into walls: if the orbit point is out of sight of the target, tighten the circle
		FCollisionQueryParams Q(SCENE_QUERY_STAT(INVFlyerOrbit), false, M);
		Q.AddIgnoredActor(T);
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByObjectType(Hit, TLoc, Desired, FCollisionObjectQueryParams(ECC_WorldStatic), Q))
		{
			Desired = FMath::Lerp(TLoc, Hit.Location, 0.7f);
		}
	}

	const FVector ToDesired = Desired - Loc;
	const float Strength = FMath::Clamp(ToDesired.Size() / 250.f, 0.15f, 1.f);
	const FVector Dir = AvoidObstacles3D(ToDesired.GetSafeNormal(), 260.f);
	M->AddMovementInput(Dir, Strength);

	DesiredRotation = (TLoc - Loc).Rotation();
	if (Now - LastSeenTargetTime > 1.f && !Dir.IsNearlyZero())
	{
		DesiredRotation = Dir.Rotation();
	}
}

// ---------------------------------------------------------------------------------------------
// Attacks
// ---------------------------------------------------------------------------------------------

FVector AArenaINV_MonsterController::PredictTarget(float Time) const
{
	const AArenaCharacter* T = Target.Get();
	if (!T)
	{
		return FVector::ZeroVector;
	}
	FVector V = T->GetVelocity();
	V.Z = FMath::Clamp(V.Z, -300.f, 300.f);
	return T->GetActorLocation() + V * FMath::Clamp(Time, 0.f, 1.5f) * 0.8f;
}

bool AArenaINV_MonsterController::SolveBallistic(const FVector& Start, const FVector& TargetLoc, float Speed, float GravityScale, FVector& OutVelocity) const
{
	const float G = -GetWorld()->GetGravityZ() * GravityScale;
	const FVector Delta = TargetLoc - Start;
	const float Dx = FVector(Delta.X, Delta.Y, 0.f).Size();
	const float Dy = Delta.Z;
	if (G < 1.f || Dx < 1.f)
	{
		OutVelocity = Delta.GetSafeNormal() * Speed;
		return true;
	}
	const float V2 = Speed * Speed;
	const float Disc = V2 * V2 - G * (G * Dx * Dx + 2.f * Dy * V2);
	if (Disc < 0.f)
	{
		return false;
	}
	const float Angle = FMath::Atan((V2 - FMath::Sqrt(Disc)) / (G * Dx));
	const FVector Flat = FVector(Delta.X, Delta.Y, 0.f) / Dx;
	OutVelocity = Flat * Speed * FMath::Cos(Angle) + FVector(0.f, 0.f, Speed * FMath::Sin(Angle));
	return true;
}

void AArenaINV_MonsterController::FireProjectile(const FVector& Start, const FVector& Velocity, const FArenaProjectileParams& Params)
{
	AArenaINV_Monster* M = GetMonster();
	UWorld* World = GetWorld();
	const FTransform SpawnTM(Velocity.Rotation(), Start);
	AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), SpawnTM, M, M,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Proj)
	{
		return;
	}
	Proj->Init(Params, this, 1.f);
	Proj->FinishSpawning(SpawnTM);
}

void AArenaINV_MonsterController::DoMelee(float Damage, float Now)
{
	AArenaINV_Monster* M = GetMonster();
	AArenaCharacter* T = Target.Get();
	if (!T)
	{
		return;
	}
	FArenaHitInfo Info;
	Info.WeaponName = M->GetDef().MeleeName;
	Info.Momentum = (T->GetActorLocation() - M->GetActorLocation()).GetSafeNormal2D() * 250.f;
	const float Applied = ArenaDamage::ApplyDamage(T, Damage * M->GetMonsterDamageScale(), this, M, Info);
	M->PlayAction(AArenaINV_Monster::EAction::Melee);
	if (M->GetDef().Sounds.Melee && M->HasImportedBody())
	{
		M->PlayMonsterSound(M->GetDef().Sounds.Melee);
	}
	else
	{
		ArenaAssets::PlaySoundAt(M, TEXT("S_Hit"), T->GetActorLocation(), 0.8f, 0.7f);
	}
	if (Applied > 0.f)
	{
		LastDamageDealtTime = Now;
		LastProgressTime = Now;
	}
	UE_LOG(LogOpenUT2k4, Verbose, TEXT("Invasion: %s hits %s for %.0f"), M->GetDef().Name, *T->GetName(), Applied);
}

void AArenaINV_MonsterController::UpdateAttack(float DeltaSeconds, float Now)
{
	AArenaINV_Monster* M = GetMonster();
	AArenaCharacter* T = Target.Get();
	const FArenaINVMonsterDef& Def = M->GetDef();
	UWorld* World = GetWorld();
	UCharacterMovementComponent* Move = M->GetCharacterMovement();

	if (!T)
	{
		if (AttackPhase == 1)
		{
			AttackPhase = 0;
			M->SetChargeGlow(0.f);
		}
		return;
	}

	const FVector Loc = M->GetActorLocation();
	const FVector TLoc = T->GetActorLocation();
	const float Dist = FVector::Dist(Loc, TLoc);
	const float SurfaceDist = FVector::Dist2D(Loc, TLoc) - Def.CapsuleRadius - ArenaTuning::CapsuleRadius;
	const bool bVerticalOverlap = FMath::Abs(TLoc.Z - Loc.Z) < Def.CapsuleHalfHeight + ArenaTuning::CapsuleHalfHeight + 20.f;
	const bool bInMelee = SurfaceDist < Def.MeleeReach && bVerticalOverlap;
	const FVector Fwd = M->GetActorForwardVector();

	// Plain melee for everyone (the boss and the brute use their slams instead when ready)
	if (bInMelee && AttackPhase == 0 && Now >= NextMeleeTime && bTargetVisible)
	{
		NextMeleeTime = Now + Def.MeleeInterval * Rng.FRandRange(0.9f, 1.1f);
		DoMelee(Def.MeleeDamage, Now);
	}

	// Ranged shots of the type (UT2004 projectile), fanned out a little when several.
	const FArenaINVShot& Shot = Def.Shot;
	auto FireShots = [&](float Spread)
	{
		FArenaProjectileParams P;
		P.Speed = Shot.Speed;
		P.MaxSpeed = Shot.Speed;
		P.GravityScale = Shot.GravityScale;
		P.Damage = Shot.Damage * M->GetMonsterDamageScale();
		P.DamageRadius = Shot.DamageRadius;
		P.Momentum = Shot.DamageRadius > 0.f ? 400.f : 150.f;
		P.LifeSpan = 6.f;
		P.Size = Shot.Size;
		P.Color = Def.GlowColor;
		P.WeaponName = Shot.Name;
		P.Style = Shot.Style;
		P.ExplosionSound = TEXT("S_Explosion");
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		for (int32 i = 0; i < FMath::Max(1, Shot.Count); ++i)
		{
			const float Side = Shot.Count > 1 ? (i % 2 == 0 ? 1.f : -1.f) : 0.f;
			const FVector Start = Loc + Fwd * (Def.CapsuleRadius + 30.f) + Right * Side * Def.CapsuleRadius * 0.6f + FVector(0.f, 0.f, Def.CapsuleHalfHeight * 0.35f);
			const FVector Aim = PredictTarget(Dist / P.Speed) - FVector(0.f, 0.f, P.DamageRadius > 0.f ? 40.f : 0.f);
			FVector Vel;
			if (P.GravityScale <= 0.f || !SolveBallistic(Start, Aim, P.Speed, P.GravityScale, Vel))
			{
				Vel = (Aim - Start).GetSafeNormal() * P.Speed;
			}
			if (Shot.Count > 1)
			{
				Vel = Vel.RotateAngleAxis(-Spread * 0.5f + Spread * (float(i) / float(Shot.Count - 1)), FVector::UpVector);
			}
			FireProjectile(Start, Vel, P);
		}
		if (Shot.Sound && M->HasImportedBody())
		{
			M->PlayMonsterSound(Shot.Sound, 0.8f);
		}
		else
		{
			ArenaAssets::PlaySoundAt(M, TEXT("S_JumpPad"), Loc, 0.5f, 0.6f);
		}
		M->PlayAction(AArenaINV_Monster::EAction::Fire);
	};
	const bool bHasShot = Shot.Speed > 0.f;

	switch (Def.Behavior)
	{
	case EArenaINVBehavior::Leaper:
	{
		if (AttackPhase == 0 && Move->IsMovingOnGround() && bTargetVisible && Dist > 350.f && Dist < 1300.f && Now >= NextSpecialTime)
		{
			AttackPhase = 1; // crouch
			AttackPhaseTime = Now;
			M->SetChargeGlow(1.f);
		}
		else if (AttackPhase == 1 && Now - AttackPhaseTime > 0.35f)
		{
			// Leap: ballistic arc onto the predicted spot
			const float Gravity = -Move->GetGravityZ();
			const float Time = FMath::Clamp(Dist / 1150.f, 0.45f, 1.f);
			const FVector Aim = PredictTarget(Time);
			FVector Vel = (Aim - Loc) / Time;
			Vel.Z = (Aim.Z - Loc.Z) / Time + 0.5f * Gravity * Time;
			M->LaunchCharacter(Vel, true, true);
			M->PlayAction(AArenaINV_Monster::EAction::Leap);
			ArenaAssets::PlaySoundAt(M, TEXT("S_JumpPad"), Loc, 0.6f, 1.4f);
			AttackPhase = 2;
			AttackPhaseTime = Now;
			bLeapHit = false;
			M->SetChargeGlow(0.4f);
		}
		else if (AttackPhase == 2)
		{
			if (!bLeapHit && Dist < Def.CapsuleRadius + ArenaTuning::CapsuleRadius + 70.f)
			{
				bLeapHit = true;
				FArenaHitInfo Info;
				Info.WeaponName = TEXT("Pounce");
				Info.Momentum = M->GetVelocity().GetSafeNormal2D() * 500.f;
				const float Applied = ArenaDamage::ApplyDamage(T, 18.f * M->GetMonsterDamageScale(), this, M, Info);
				M->PlayAction(AArenaINV_Monster::EAction::Melee);
				M->PlayMonsterSound(Def.Sounds.Melee);
				if (Applied > 0.f)
				{
					LastDamageDealtTime = Now;
					LastProgressTime = Now;
				}
			}
			if ((Move->IsMovingOnGround() && Now - AttackPhaseTime > 0.25f) || Now - AttackPhaseTime > 2.5f)
			{
				AttackPhase = 0;
				NextSpecialTime = Now + Rng.FRandRange(2.2f, 3.5f);
				M->SetChargeGlow(0.f);
			}
		}
		// Skaarj: twin bolts between leaps
		if (AttackPhase == 0 && bHasShot && bTargetVisible && Dist > 400.f && Dist < 3000.f && Now >= NextShotTime)
		{
			NextShotTime = Now + Shot.Interval * Rng.FRandRange(0.8f, 1.3f);
			FireShots(6.f);
		}
		break;
	}
	case EArenaINVBehavior::Spitter:
	{
		if (AttackPhase == 0 && bHasShot && bTargetVisible && Dist > 250.f && Dist < 2600.f && Now >= NextSpecialTime)
		{
			AttackPhase = 1;
			AttackPhaseTime = Now;
			M->PlayAction(AArenaINV_Monster::EAction::Fire);
		}
		if (AttackPhase == 1)
		{
			M->SetChargeGlow((Now - AttackPhaseTime) / 0.35f);
			if (Now - AttackPhaseTime > 0.35f)
			{
				FireShots(0.f);
				AttackPhase = 0;
				NextSpecialTime = Now + Shot.Interval * Rng.FRandRange(0.9f, 1.5f);
				M->SetChargeGlow(0.f);
			}
		}
		break;
	}
	case EArenaINVBehavior::Brute:
	{
		if (AttackPhase == 0 && SurfaceDist < 170.f && bVerticalOverlap && Now >= NextSpecialTime)
		{
			AttackPhase = 1;
			AttackPhaseTime = Now;
			M->PlayAction(AArenaINV_Monster::EAction::Melee);
		}
		if (AttackPhase == 1)
		{
			M->SetChargeGlow((Now - AttackPhaseTime) / 0.7f);
			if (Now - AttackPhaseTime > 0.7f)
			{
				const FVector Origin = M->GetFeetLocation() + Fwd * 70.f + FVector(0.f, 0.f, 30.f);
				ArenaDamage::ApplyRadialDamage(World, Origin, 42.f * M->GetMonsterDamageScale(), 380.f, 750.f, this, M, TEXT("Ground Slam"), 0.f, M);
				ArenaFX::SpawnExplosion(World, Origin, Def.GlowColor, 260.f, TEXT("Explosion.Grenade"));
				ArenaAssets::PlaySoundAt(M, TEXT("S_ExplosionBig"), Origin, 0.9f, 0.7f);
				if (FVector::Dist(Origin, TLoc) < 420.f)
				{
					LastDamageDealtTime = Now;
					LastProgressTime = Now;
				}
				AttackPhase = 0;
				NextSpecialTime = Now + Rng.FRandRange(2.2f, 3.f);
				NextMeleeTime = Now + 0.8f;
				M->SetChargeGlow(0.f);
			}
		}
		// Brute rockets from both arms at range
		else if (AttackPhase == 0 && bHasShot && bTargetVisible && Dist > 500.f && Dist < 3500.f && Now >= NextShotTime)
		{
			NextShotTime = Now + Shot.Interval * Rng.FRandRange(0.8f, 1.3f);
			FireShots(4.f);
		}
		break;
	}
	case EArenaINVBehavior::Flyer:
	{
		if (bHasShot && bTargetVisible && Dist < 3200.f && Now >= NextSpecialTime)
		{
			FireShots(0.f);
			NextSpecialTime = Now + Shot.Interval * Rng.FRandRange(0.8f, 1.3f);
		}
		break;
	}
	case EArenaINVBehavior::Boss:
	{
		if (AttackPhase == 0)
		{
			if (SurfaceDist < 230.f && bVerticalOverlap && Now >= NextMeleeTime)
			{
				AttackPhase = 1;
				PendingAttack = 1; // stomp
				AttackPhaseTime = Now;
				M->PlayAction(AArenaINV_Monster::EAction::Melee);
			}
			else if (bTargetVisible && Dist > 450.f && Dist < 5000.f && Now >= NextSpecialTime)
			{
				AttackPhase = 1;
				PendingAttack = 2; // volley
				AttackPhaseTime = Now;
				M->PlayMonsterSound(Def.Sounds.Challenge);
				M->PlayAction(AArenaINV_Monster::EAction::Fire);
			}
		}
		if (AttackPhase == 1)
		{
			const float WindUp = PendingAttack == 1 ? 0.55f : 0.6f;
			M->SetChargeGlow((Now - AttackPhaseTime) / WindUp);
			if (Now - AttackPhaseTime > WindUp)
			{
				if (PendingAttack == 1)
				{
					const FVector Origin = M->GetFeetLocation() + FVector(0.f, 0.f, 30.f);
					ArenaDamage::ApplyRadialDamage(World, Origin, 55.f * M->GetMonsterDamageScale(), 500.f, 1100.f, this, M, TEXT("Titan Stomp"), 0.f, M);
					ArenaFX::SpawnExplosion(World, Origin, Def.GlowColor, 400.f, TEXT("Explosion.Grenade"));
					ArenaAssets::PlaySoundAt(M, TEXT("S_ExplosionBig"), Origin, 1.f, 0.5f);
					LastDamageDealtTime = Now;
					NextMeleeTime = Now + 3.f;
				}
				else if (bHasShot)
				{
					FireShots(30.f);
					NextSpecialTime = Now + Shot.Interval * Rng.FRandRange(0.9f, 1.2f);
				}
				AttackPhase = 0;
				PendingAttack = 0;
				M->SetChargeGlow(0.f);
			}
		}
		break;
	}
	default:
		break;
	}
}

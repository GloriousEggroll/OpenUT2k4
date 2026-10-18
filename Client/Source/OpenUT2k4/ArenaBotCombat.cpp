// UT2004 bot combat (UnrealGame.Bot): AdjustAim / AdjustAimError, ReceiveWarning / ReceiveProjectileWarning /
// DelayedWarning / TryToDuck, FaceActor strafing, MayDodgeToMoveTarget, combat timer, shock combos, rocket lock.
// UE2 distances are converted with 1 UU = 2 cm; rotator units 65536 = 360 deg.
#include "ArenaBotController.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaProjectile.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

namespace ArenaBotUE2
{
	constexpr float RotToDeg = 360.f / 65536.f;
	constexpr float RotToRad = 9.58738e-05f;

	bool FastTrace(const UWorld* World, const FVector& To, const FVector& From)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotFastTrace), false);
		return !World->LineTraceTestByChannel(From, To, ECC_Visibility, Q);
	}

	bool WorldTrace(const UWorld* World, const FVector& From, const FVector& To, FHitResult& Hit)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotTrace), false);
		FCollisionResponseParams R;
		R.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q, R);
	}
}

float AArenaBotController::GetRotationRateYaw() const
{
	// Bot.ResetSkill: RotationRate.Yaw 90000 above skill + ReactionTime 7, 7000 + 10000 x (skill + RT) from 4,
	// else 30000 + 4000 x (skill + RT)
	const float S = Skill + ReactionTimeAdjust;
	const float Yaw = S > 7.f ? 90000.f : (S >= 4.f ? 7000.f + 10000.f * S : 30000.f + 4000.f * S);
	return Yaw * ArenaBotUE2::RotToDeg;
}

float AArenaBotController::GetCombatTimerPeriod() const
{
	return 1.2f - 0.09f * FMath::Min(10.f, Skill + ReactionTimeAdjust);
}

float AArenaBotController::AdjustAimErrorUE2(float InAimError, float TargetDist, bool bDefendMelee, bool bInstantProj, bool bLeadTargetNow)
{
	using namespace ArenaBotUE2;
	const APawn* P = GetPawn();
	const AArenaCharacter* T = Enemy;
	if (!P || !T)
	{
		return 0.f;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	float AimErr = InAimError;
	if (T->IsInvisible())
	{
		AimErr *= 2.5f;
	}
	// harder to hit a target moving across the line of fire
	const FVector TLoc = T->GetActorLocation();
	const FVector PLoc = P->GetActorLocation();
	const float Across = FVector::DotProduct((TLoc - PLoc).GetSafeNormal(), ((TLoc + 1.2f * T->GetVelocity()) - (PLoc + P->GetVelocity())).GetSafeNormal());
	AimErr *= FMath::Min(5.f, 12.f - 11.f * Across);
	if (bDefendMelee)
	{
		AimErr *= 0.5f;
	}
	if (T->GetVelocity().IsNearlyZero())
	{
		AimErr *= 0.2f + 0.1f * (7.f - FMath::Min(7.f, Skill));
	}
	else if (Skill + Accuracy > 5.f)
	{
		FVector VelDir = T->GetVelocity();
		VelDir.Z = 0.f;
		const UCharacterMovementComponent* TCM = T->GetCharacterMovement();
		FVector AccelDir = TCM ? TCM->GetCurrentAcceleration() : FVector::ZeroVector;
		AccelDir.Z = 0.f;
		if (AccelDir.IsNearlyZero() || FVector::DotProduct(VelDir.GetSafeNormal(), AccelDir.GetSafeNormal()) > 0.95f)
		{
			AimErr *= 0.8f;
		}
	}
	if (bStoppedUE2 && Now > StopStartTime)
	{
		if (Skill + Accuracy > 4.f)
		{
			AimErr *= 0.75f;
		}
		const AArenaCharacter* Me = GetArenaPawn();
		if (Me && Cast<AWeap_RailRifle>(Me->GetCurrentWeapon()))
		{
			AimErr *= FMath::Clamp((1.5f - 0.08f * FMath::Min(Skill, 7.f) - 0.8f * Rng.FRand()) / (Now - StopStartTime + 0.4f), 0.3f, 1.f);
		}
		else
		{
			AimErr *= FMath::Clamp((2.f - 0.08f * FMath::Min(Skill, 7.f) - Rng.FRand()) / (Now - StopStartTime + 0.4f), 0.7f, 1.f);
		}
	}
	if (!bDefendMelee)
	{
		AimErr *= 3.3f - 0.38f * (FMath::Clamp(Skill + Accuracy, 0.f, 8.f) + 0.5f * Rng.FRand());
	}
	const AArenaCharacter* Me = GetArenaPawn();
	if ((Skill < 6.f || Rng.FRand() < 0.5f) && Me && Now - Me->LastHitTime < 0.2f)
	{
		AimErr *= 1.3f;
	}
	const UCharacterMovementComponent* MyCM = Me ? Me->GetCharacterMovement() : nullptr;
	const UCharacterMovementComponent* TCM = T->GetCharacterMovement();
	if ((MyCM && MyCM->IsFalling()) || (TCM && TCM->IsFalling()))
	{
		AimErr *= 1.6f - 0.04f * (Skill + Accuracy);
	}
	if (AcquireTime > Now - 0.5f - 0.5f * (7.f - FMath::Min(7.f, Skill)))
	{
		AimErr *= 1.5f;
		if (bInstantProj)
		{
			AimErr *= 1.5f;
		}
	}
	AimErr = 2.f * AimErr * Rng.FRand() - AimErr;
	if (FMath::Abs(AimErr) > 700.f)
	{
		// never miss by more than 100 UU (instant) / 320 UU plus the target's radius
		const float DesiredDist = (bInstantProj ? 200.f : 640.f) + T->GetSimpleCollisionRadius();
		const float AimDist = FMath::Tan(RotToRad * AimErr) * TargetDist;
		if (FMath::Abs(AimDist) > DesiredDist)
		{
			const float NewAngle = FMath::Atan2(DesiredDist, TargetDist) / RotToRad;
			AimErr = AimErr > 0.f ? NewAngle : -NewAngle;
		}
	}
	return AimErr;
}

FVector AArenaBotController::AdjustAimUE2(const AArenaWeapon* Weapon, uint8 Mode, const FVector& ProjStart, const FVector& ViewDir)
{
	using namespace ArenaBotUE2;
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	if (!P || !Weapon || Mode > 1 || !World)
	{
		return ViewDir;
	}
	const FArenaFireMode& FM = Weapon->FireModes[Mode];
	const float Now = World->GetTimeSeconds();

	// ShockBeamFire.AdjustAim: the combo shot goes straight at the core
	if (Mode == 0 && Cast<AWeap_PulseRifle>(Weapon) && ComboCore.IsValid())
	{
		const FVector Dir = (ComboCore->GetActorLocation() - ProjStart).GetSafeNormal();
		SetControlRotation(Dir.Rotation());
		ComboCore.Reset();
		bWaitForCombo = false;
		bComboTriggered = false;
		NumCombos++;
		return Dir;
	}
	// Focus on a non-pawn target (an incoming warhead, SetShootTarget): lead it, with the UE2 aim error left out
	if (const AActor* ST = GetShootTarget())
	{
		FVector Aim = ST->GetActorLocation();
		const float Speed = FM.FireType == EArenaFireType::Projectile ? Weapon->GetProjectileSpeed(Mode) : 0.f;
		if (Speed > 1.f)
		{
			Aim += ST->GetVelocity() * (FVector::Dist(ProjStart, Aim) / Speed);
		}
		const FVector Dir = (Aim - ProjStart).GetSafeNormal();
		SetControlRotation(Dir.Rotation());
		return Dir;
	}
	if (!Enemy || Enemy->IsDead())
	{
		return ViewDir;
	}
	const AArenaCharacter* T = Enemy;
	const FVector TLoc = T->GetActorLocation();
	const float CH = T->GetSimpleCollisionHalfHeight();
	const float TargetDist = FVector::Dist(TLoc, P->GetActorLocation());
	const bool bInstant = FM.FireType != EArenaFireType::Projectile;
	const float ProjSpeed = FMath::Max(1.f, Weapon->GetProjectileSpeed(Mode));
	const UCharacterMovementComponent* TCM = T->GetCharacterMovement();
	const bool bTargetFalling = TCM && TCM->IsFalling();
	// Bot.ResetSkill: bLeadTarget = skill >= 4 (ProjectileFire.bLeadTarget)
	bool bLeadNow = !bInstant && Skill >= 4.f;
	// Bot.DefendMelee: the enemy holds a melee weapon (Shield Gun) within 1000 UU
	const bool bDefendMelee = Cast<AWeap_Gauntlet>(T->GetCurrentWeapon()) && TargetDist < 2000.f;
	const float AimErr = AdjustAimErrorUE2(FM.BotAimError, TargetDist, bDefendMelee, bInstant, bLeadNow);
	LastAimErrorYaw = AimErr;

	FVector FireSpot = TLoc;
	if (bLeadNow)
	{
		FVector TV = T->GetVelocity();
		const float TravelTime = TargetDist / ProjSpeed;
		if (bTargetFalling)
		{
			const float GZ = (TCM ? TCM->GetGravityZ() : -1900.f);
			if (GZ <= -1899.f)
			{
				TV.Z = FMath::Min(TV.Z + FMath::Max(-800.f, GZ * FMath::Min(1.f, TargetDist / ProjSpeed)), 0.f);
			}
			else
			{
				TV.Z += 0.5f * TravelTime * GZ;
			}
			FireSpot = TLoc + TravelTime * TV;
			FHitResult Hit;
			bLeadNow = false;
			if (WorldTrace(World, TLoc, FireSpot, Hit))
			{
				FireSpot = Hit.Location + FVector(0.f, 0.f, 4.f);
			}
		}
		if (bLeadNow)
		{
			FireSpot += FMath::Min(1.f, 0.7f + 0.6f * Rng.FRand()) * TV * TravelTime;
			FireSpot.Z = FMath::Min(TLoc.Z, FireSpot.Z);
		}
		bool bCleanLead;
		if (!bTargetFalling && Rng.FRand() < 0.55f && FVector::Dist(FireSpot, ProjStart) > 2000.f)
		{
			FRotator Look = T->GetActorRotation();
			if (TCM && TCM->IsMovingOnGround())
			{
				Look.Pitch = 0.f;
			}
			bCleanLead = FVector::DotProduct(Look.Vector(), T->GetVelocity().GetSafeNormal()) >= 0.71f && FastTrace(World, FireSpot, ProjStart);
		}
		else
		{
			bCleanLead = FastTrace(World, FireSpot, ProjStart);
		}
		if (!bCleanLead)
		{
			FireSpot = Rng.FRand() < 0.3f ? TLoc : 0.5f * (FireSpot + TLoc);
		}
	}

	bool bClean = false;
	const FVector PLoc = P->GetActorLocation();
	if (FM.bRecommendSplash && (Skill >= 4.f || bDefendMelee)
		&& ((bTargetFalling && PLoc.Z + 160.f >= TLoc.Z) || (PLoc.Z + 38.f >= TLoc.Z && (bDefendMelee || Skill > 6.5f * Rng.FRand() - 0.5f))))
	{
		// splash: aim at the floor under the target
		FHitResult Hit;
		bClean = !WorldTrace(World, FireSpot, FireSpot - FVector(0.f, 0.f, CH + 12.f), Hit);
		if (!bClean)
		{
			FireSpot = Hit.Location + FVector(0.f, 0.f, 6.f);
			bClean = FastTrace(World, FireSpot, ProjStart);
		}
		else
		{
			bClean = bTargetFalling && FastTrace(World, FireSpot, ProjStart);
		}
	}
	if (Cast<AWeap_RailRifle>(Weapon) && bStoppedUE2 && Skill > 5.f + 6.f * Rng.FRand())
	{
		// sniping: go for the head
		FireSpot.Z = TLoc.Z + 0.9f * CH;
		bClean = FastTrace(World, FireSpot, ProjStart);
	}
	if (!bClean)
	{
		FireSpot.Z = TLoc.Z;
		bClean = FastTrace(World, FireSpot, ProjStart);
	}
	if (FM.bTossed && !bClean)
	{
		FireSpot = LastSeenEnemyLocation;
		FHitResult Hit;
		if (WorldTrace(World, ProjStart, FireSpot, Hit))
		{
			FireSpot += 2.f * CH * Hit.ImpactNormal;
		}
		bClean = true;
	}
	if (!bClean)
	{
		FireSpot.Z = TLoc.Z + 0.9f * CH;
		bClean = FastTrace(World, FireSpot, ProjStart);
	}
	if (!bClean)
	{
		FireSpot = LastSeenEnemyLocation;
		if (PLoc.Z >= LastSeenEnemyLocation.Z)
		{
			FireSpot.Z -= 0.4f * CH;
		}
		FHitResult Hit;
		if (WorldTrace(World, ProjStart, FireSpot, Hit))
		{
			FireSpot = LastSeenEnemyLocation + 2.f * CH * Hit.ImpactNormal;
		}
	}

	FVector FireDir = FireSpot - ProjStart;
	if (FM.bTossed)
	{
		// AdjustToss: a launch direction for ProjSpeed under gravity
		FVector TossVel;
		const float GravScale = FM.Projectile.GravityScale > 0.f ? FM.Projectile.GravityScale : 1.f;
		const float GZ = World->GetGravityZ() * GravScale;
		const FVector Delta = FireSpot - ProjStart;
		const float D2 = Delta.Size2D();
		const float V2 = ProjSpeed * ProjSpeed;
		const float Disc = V2 * V2 + GZ * (-GZ * D2 * D2 + 2.f * Delta.Z * V2);
		if (Disc >= 0.f && D2 > 1.f)
		{
			const float Angle = FMath::Atan((V2 - FMath::Sqrt(Disc)) / (-GZ * D2));
			TossVel = Delta.GetSafeNormal2D() * FMath::Cos(Angle) + FVector(0.f, 0.f, FMath::Sin(Angle));
			FireDir = TossVel;
		}
	}
	FRotator FireRot = FireDir.Rotation();
	const float RealYaw = FireRot.Yaw;
	FireRot.Yaw = RealYaw + AimErr * RotToDeg;
	FireDir = FireRot.Vector();
	// keep the error away from a nearby wall (UE2 traces 400 UU along the fire direction)
	const float FireDist = FMath::Min(FVector::Dist(FireSpot, ProjStart), 800.f);
	FVector Spot = ProjStart + FireDist * FireDir;
	FHitResult Hit;
	if (WorldTrace(World, ProjStart, Spot, Hit))
	{
		if (Hit.ImpactNormal.Z < 0.7f)
		{
			FireRot.Yaw = RealYaw - AimErr * RotToDeg;
			FireDir = FireRot.Vector();
			Spot = ProjStart + FireDist * FireDir;
		}
		FHitResult Hit2;
		if (WorldTrace(World, ProjStart, Spot, Hit2))
		{
			Spot += Hit2.ImpactNormal * 2.f * CH;
			FHitResult Hit3;
			if (Skill >= 4.f && WorldTrace(World, ProjStart, Spot, Hit3))
			{
				Spot += CH * Hit3.ImpactNormal;
			}
			FireDir = (Spot - ProjStart).GetSafeNormal();
			FireRot = FireDir.Rotation();
		}
	}
	SetControlRotation(FRotator(FireRot.Pitch, FireRot.Yaw, 0.f));
	return FireDir.GetSafeNormal();
}

void AArenaBotController::ReceiveWarning(APawn* Shooter, float ProjSpeed, const FVector& FireDir)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	if (!P || P->IsDead() || !Shooter || !World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	LastUnderFire = Now;
	NumWarnings++;
	if (!Enemy)
	{
		if (AArenaCharacter* S = Cast<AArenaCharacter>(Shooter))
		{
			Enemy = S;
			LastSeenEnemyLocation = S->GetActorLocation();
			LastSeenEnemyTime = Now;
			AcquireTime = Now;
		}
		return;
	}
	const UCharacterMovementComponent* CM = P->GetCharacterMovement();
	if (Skill < 4.f || (CM && (CM->IsFalling() || CM->IsSwimming())) || Rng.FRand() > 0.2f * Skill - 0.33f)
	{
		return;
	}
	const float EnemyDist = FVector::Dist(Shooter->GetActorLocation(), P->GetActorLocation());
	if (EnemyDist > 4000.f && !bStoppedUE2)
	{
		return;
	}
	FVector X = P->GetActorRotation().Vector();
	X.Z = 0.f;
	FVector EnemyDir = Shooter->GetActorLocation() - P->GetActorLocation();
	EnemyDir.Z = 0.f;
	if (FVector::DotProduct(EnemyDir.GetSafeNormal(), X.GetSafeNormal()) < 0.7f)
	{
		return;
	}
	if (ProjSpeed > 0.f && EnemyDist / ProjSpeed < 0.11f + 0.15f * Rng.FRand())
	{
		return;
	}
	if (Rng.FRand() * (Skill + 4.f) < 4.f)
	{
		return;
	}
	FVector Y = FRotationMatrix(P->GetActorRotation()).GetUnitAxis(EAxis::Y);
	if (FVector::DotProduct(FireDir, Y) > 0.f)
	{
		TryToDuck(-Y, true);
	}
	else
	{
		TryToDuck(Y, false);
	}
}

void AArenaBotController::ReceiveProjectileWarning(AArenaProjectile* Proj)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	if (!P || P->IsDead() || !Proj || !World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	LastUnderFire = Now;
	const UCharacterMovementComponent* CM = P->GetCharacterMovement();
	if (Skill < 2.f || !Enemy || (CM && CM->IsSwimming()))
	{
		return;
	}
	NumWarnings++;
	const float Speed = Proj->GetVelocity().Size();
	if (Speed <= 0.f)
	{
		return;
	}
	const float S = Skill + ReactionTimeAdjust;
	const float ProjTime = FVector::Dist(Proj->GetActorLocation(), P->GetActorLocation()) / Speed;
	if (ProjTime < 0.35f - 0.03f * S)
	{
		return;
	}
	if (ProjTime < 2.f - (0.265f + Rng.FRand() * 0.2f) * S)
	{
		return;
	}
	AArenaProjectile* Old = WarningProjectile.Get();
	if (Old && FVector::Dist(Old->GetActorLocation(), P->GetActorLocation()) / FMath::Max(1.f, Old->GetVelocity().Size()) < ProjTime)
	{
		return;
	}
	if (ProjTime < 1.2f || Old)
	{
		FVector X = GetControlRotation().Vector();
		X.Z = 0.f;
		FVector Dir = Proj->GetActorLocation() - P->GetActorLocation();
		Dir.Z = 0.f;
		if (FVector::DotProduct(Dir.GetSafeNormal(), X.GetSafeNormal()) < 0.7f)
		{
			return;
		}
	}
	const float K = S >= 7.f ? 0.025f : 0.02f;
	WarningDelay = Now + FMath::Max(0.08f, FMath::Max(0.35f - K * S * (1.f + Rng.FRand()), ProjTime - 0.65f));
	WarningProjectile = Proj;
}

void AArenaBotController::DelayedWarning()
{
	AArenaCharacter* P = GetArenaPawn();
	AArenaProjectile* Proj = WarningProjectile.Get();
	WarningProjectile.Reset();
	UWorld* World = GetWorld();
	if (!P || !Proj || !World || Proj->GetVelocity().IsNearlyZero())
	{
		return;
	}
	if (!Enemy)
	{
		Enemy = Cast<AArenaCharacter>(Proj->GetInstigator());
		return;
	}
	const FVector PV = Proj->GetVelocity();
	const FVector Dir = PV.GetSafeNormal();
	const FVector PLoc = P->GetActorLocation();
	const FVector FuturePos = PLoc + P->GetVelocity() * FVector::Dist(Proj->GetActorLocation(), PLoc) / PV.Size();
	const FVector LineDist = FuturePos - (Proj->GetActorLocation() + FVector::DotProduct(Dir, FuturePos - Proj->GetActorLocation()) * Dir);
	const float Dist = LineDist.Size();
	const float CH = P->GetSimpleCollisionHalfHeight();
	if (Dist > 460.f + P->GetSimpleCollisionRadius())
	{
		return;
	}
	if (Dist > 1.2f * CH)
	{
		if (Proj->GetParams().Damage <= 40.f)
		{
			return;
		}
		if (Proj->GetParams().GravityScale > 0.f && Dist > CH + 200.f && Proj->GetParams().Kind != EArenaProjKind::ShockBall)
		{
			FHitResult Hit;
			if (!ArenaBotUE2::WorldTrace(World, Proj->GetActorLocation(), Proj->GetActorLocation() + PV, Hit))
			{
				return;
			}
		}
	}
	FVector X = P->GetActorRotation().Vector();
	X.Z = 0.f;
	FVector ToProj = Proj->GetActorLocation() - PLoc;
	ToProj.Z = 0.f;
	if (FVector::DotProduct(ToProj.GetSafeNormal(), X.GetSafeNormal()) < 0.7f)
	{
		return;
	}
	const FVector Y = FRotationMatrix(P->GetActorRotation()).GetUnitAxis(EAxis::Y);
	if (FVector::DotProduct(PV, Y) > 0.f)
	{
		TryToDuck(-Y, true);
	}
	else
	{
		TryToDuck(Y, false);
	}
}

bool AArenaBotController::TryToDuck(FVector DuckDir, bool bReversed)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	if (!P || !World)
	{
		return false;
	}
	const UCharacterMovementComponent* CM = P->GetCharacterMovement();
	if (Skill < 3.f || !CM || CM->IsSwimming() || CM->IsFalling() || CM->GetGravityZ() > -1899.f)
	{
		return false;
	}
	if (P->bIsCrouched || !CM->IsMovingOnGround() || JumpPhase != 0)
	{
		return false;
	}
	DuckDir.Z = 0.f;
	DuckDir = DuckDir.GetSafeNormal() * 670.f;
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(P->GetSimpleCollisionRadius(), P->GetSimpleCollisionHalfHeight());
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotDuck), false, P);
	const FVector Start = P->GetActorLocation() + FVector(0.f, 0.f, 50.f);
	auto Probe = [&](const FVector& D, float MinDist) -> bool
	{
		FHitResult Hit;
		const bool bHit = World->SweepSingleByChannel(Hit, Start, Start + D, FQuat::Identity, ECC_Pawn, Shape, Q);
		if (bHit && FVector::Dist(Hit.Location, P->GetActorLocation()) <= MinDist)
		{
			return false;
		}
		const FVector End = bHit ? FVector(Hit.Location) : Start + D;
		FHitResult Floor;
		return World->SweepSingleByChannel(Floor, End, End - FVector(0.f, 0.f, 175.f), FQuat::Identity, ECC_Pawn, Shape, Q) && Floor.ImpactNormal.Z >= 0.7f;
	};
	FHitResult First;
	const bool bFirstHit = World->SweepSingleByChannel(First, Start, Start + DuckDir, FQuat::Identity, ECC_Pawn, Shape, Q);
	const float FirstDist = bFirstHit ? FVector::Dist(First.Location, P->GetActorLocation()) : 1e9f;
	bool bSuccess = Probe(DuckDir, 300.f);
	float MinDist = 300.f;
	if (bFirstHit && FirstDist <= 300.f)
	{
		MinDist = 60.f + 300.f - FirstDist;
	}
	if (!bSuccess)
	{
		DuckDir = -DuckDir;
		bSuccess = Probe(DuckDir, MinDist);
	}
	// never dodge into the void (our safety on top of UE2's floor check)
	if (!bSuccess || !IsSafeMove(DuckDir.GetSafeNormal(), 700.f))
	{
		return false;
	}
	if (!P->DodgeWorld(DuckDir.GetSafeNormal()))
	{
		return false;
	}
	NumDucks++;
	JumpPhase = 2;
	JumpPhaseTime = World->GetTimeSeconds();
	bAllowDoubleJump = false;
	return true;
}

bool AArenaBotController::ShouldFaceEnemyUE2(const FVector& MoveTarget)
{
	// Bot.FaceActor (StrafingModifier 0), decided once per path node like UE2's MoveToward focus
	const AArenaCharacter* P = GetArenaPawn();
	if (!P || !Enemy)
	{
		return false;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (MoveTarget.Equals(FaceDecisionPoint, 1.f))
	{
		return bFaceEnemy;
	}
	FaceDecisionPoint = MoveTarget;
	const float SinceSeen = Now - LastSeenEnemyTime;
	const AArenaWeapon* W = P->GetCurrentWeapon();
	const bool bMelee = Cast<AWeap_Gauntlet>(W) != nullptr;
	const FVector Loc = P->GetActorLocation();
	if (SinceSeen > 6.f)
	{
		return bFaceEnemy = false;
	}
	if ((Skill + StrafingAbility >= 6.f && !bMelee) || FVector::Dist(MoveTarget, Loc) < 4.f * P->GetSimpleCollisionRadius())
	{
		return bFaceEnemy = true;
	}
	if (SinceSeen > 4.f)
	{
		return bFaceEnemy = false;
	}
	const float RelativeDir = FVector::DotProduct((Enemy->GetActorLocation() - Loc).GetSafeNormal2D(), (MoveTarget - Loc).GetSafeNormal2D());
	if (RelativeDir > 0.85f)
	{
		return bFaceEnemy = true;
	}
	if (Skill + StrafingAbility < 2.f + Rng.FRand())
	{
		return bFaceEnemy = false;
	}
	if ((bMelee && RelativeDir < 0.3f) || Skill + StrafingAbility < 5.f * Rng.FRand() || 0.4f * RelativeDir + 0.8f < Rng.FRand())
	{
		return bFaceEnemy = false;
	}
	return bFaceEnemy = true;
}

void AArenaBotController::MayDodgeToMoveTarget(const FVector& MoveTarget)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	if (!P || !World || JumpPhase != 0)
	{
		return;
	}
	// Bot.ResetSkill: DodgeToGoalPct = 0 below skill 3, else 3 x Jumpiness + skill / 6
	const float DodgeToGoalPct = Skill < 3.f ? 0.f : 3.f * Jumpiness + Skill / 6.f;
	if (Rng.FRand() >= DodgeToGoalPct)
	{
		return;
	}
	const UCharacterMovementComponent* CM = P->GetCharacterMovement();
	if (!CM || !CM->IsMovingOnGround() || Rng.FRand() > 0.85f)
	{
		return;
	}
	if (Enemy && bEnemyVisible && bFaceEnemy && Skill + Jumpiness < 6.f)
	{
		return;
	}
	if (CM->GetGravityZ() > -1899.f)
	{
		return;
	}
	const FVector Dir = MoveTarget - P->GetActorLocation();
	if (Dir.Size() < 1600.f || Dir.Z > 70.f)
	{
		return;
	}
	const FVector D2 = Dir.GetSafeNormal2D();
	if (!IsSafeMove(D2, 700.f) || !P->DodgeWorld(D2))
	{
		return;
	}
	// then double jump at the apex (bPendingDoubleJump)
	NumGoalDodges++;
	JumpPhase = 2;
	JumpPhaseTime = World->GetTimeSeconds();
	bAllowDoubleJump = Skill >= 3.f;
	bApexDoubleJump = bAllowDoubleJump;
}

void AArenaBotController::SetComboCore(AArenaProjectile* Core)
{
	// ShockRifle.SetComboTarget (bRegisterTarget set by BestMode)
	if (!bRegisterComboTarget || !Enemy || !Core)
	{
		return;
	}
	bRegisterComboTarget = false;
	ComboCore = Core;
	bWaitForCombo = true;
}

// UT2004 bot translocator use: Bot.FaceActor's translocator hops (catching up with the flag / ball / an enemy carrier
// or just moving faster), JumpSpot.TryTranslocator on the nav graph's Translocate links, TransLauncher.FireHack /
// ShouldTranslocatorHop, and the beacon's MonitoringThrow callbacks (ArenaTranslocator.cpp).
#include "ArenaBotController.h"
#include "OpenUT2k4.h"
#include "ArenaCarryable.h"
#include "ArenaCharacter.h"
#include "ArenaFlag.h"
#include "ArenaNavGraph.h"
#include "ArenaTranslocator.h"
#include "ArenaWeaponActors.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace ArenaTranslocatorBotLocal
{
	constexpr float UU = 2.f;

	bool IsGameObject(const AActor* A)
	{
		return A && (A->IsA<AArenaFlag>() || A->IsA<AArenaCarryable>());
	}

	bool BotLog()
	{
		static const bool b = FParse::Param(FCommandLine::Get(), TEXT("TranslocLog")) || FCString::Strifind(FCommandLine::Get(), TEXT("-BotDebug=")) != nullptr;
		return b;
	}
}

using namespace ArenaTranslocatorBotLocal;

AWeap_Translocator* AArenaBotController::GetTranslocator() const
{
	const AArenaCharacter* P = GetArenaPawn();
	if (!P)
	{
		return nullptr;
	}
	for (AArenaWeapon* W : P->GetWeapons())
	{
		if (AWeap_Translocator* T = Cast<AWeap_Translocator>(W))
		{
			return T;
		}
	}
	return nullptr;
}

bool AArenaBotController::CanUseTranslocator() const
{
	// Bot.CanUseTranslocator: bHasTranslocator && Skill >= 2 && Level.TimeSeconds > NextTranslocTime &&
	// Squad.AllowTranslocationBy (CTFSquadAI / BombingRunSquadAI: not the flag / ball carrier)
	const UWorld* World = GetWorld();
	return World && Skill >= 2.f && World->GetTimeSeconds() > NextTranslocTime && !IsCarryingObjective() && GetTranslocator() != nullptr;
}

void AArenaBotController::NotifyTranslocated()
{
	// TransLauncher.ReduceAmmo: TranslocFreq = 3 + FMax(TranslocFreq, Level.TimeSeconds)
	const float Now = GetWorld()->GetTimeSeconds();
	TranslocFreq = 3.f + FMath::Max(TranslocFreq, Now);
	NumTranslocations++;
}

void AArenaBotController::NotifyTranslocatorDrained()
{
	// TransLauncher.DrainCharges: NextTranslocTime = Level.TimeSeconds + 3.5
	NextTranslocTime = GetWorld()->GetTimeSeconds() + 3.5f;
}

bool AArenaBotController::GetRealTranslocationTarget(FVector& OutLocation) const
{
	// TransRecall.BotTranslocation: only while preparing a move (JumpSpot routes)
	const AArenaCharacter* P = GetArenaPawn();
	if (!bHasTranslocTarget || !bPreparingMove || !P)
	{
		return false;
	}
	OutLocation = RealTranslocTargetLoc + FVector(0.f, 0.f, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - TranslocTargetHeight);
	return true;
}

bool AArenaBotController::IsTranslocationTargetValid(const AArenaTransBeacon* Beacon) const
{
	// MonitoringThrow.Tick: TranslocationTarget set, the squad still allows it
	const AArenaCharacter* P = GetArenaPawn();
	return bHasTranslocTarget && P && P->IsAlive() && !IsCarryingObjective();
}

void AArenaBotController::ShouldDestroyTranslocator()
{
	// SquadAI.ShouldDestroyTranslocator: no vehicle, no enemy, skill 2+; the first beacon of an enemy (not disrupted yet)
	// within 1500 UU and in sight: GoalString "Destroy Translocator", DoRangedAttackOn(beacon)
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	if (!P || Skill < 2.f || (Enemy && !Enemy->IsDead() && Now - LastSeenEnemyTime < 2.f) || GetShootTarget())
	{
		return;
	}
	const FVector Eye = P->GetPawnViewLocation();
	for (TActorIterator<AArenaTransBeacon> It(World); It; ++It)
	{
		AArenaTransBeacon* B = *It;
		const AArenaCharacter* Owner = B->GetBeaconOwner();
		if (B->IsActorBeingDestroyed() || B->Disrupted() || !Owner || Owner == P || !Owner->GetController()
			|| ArenaWeaponActors::SameTeam(World, this, Owner->GetController()) || FVector::Dist(Eye, B->GetActorLocation()) > 1500.f * UU)
		{
			continue;
		}
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotBeaconLOS), false, P);
		Q.AddIgnoredActor(B);
		if (World->LineTraceTestByChannel(Eye, B->GetActorLocation(), ECC_Visibility, Q))
		{
			continue;
		}
		SetShootTarget(B, 1.5f);
		ArenaTranslocator::Stats().BeaconsShotAt++;
		if (BotLog())
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("BotTransloc: %s shoots the beacon of %s (%.0f cm)"), *GetName(), *Owner->GetName(), FVector::Dist(Eye, B->GetActorLocation()));
		}
		return;
	}
}

bool AArenaBotController::IsSafeTranslocation(const FVector& BeaconLoc) const
{
	// our addition to UT2004's MonitoringThrow: hops never end over the void (UE2 bots rely on the path network)
	return bPreparingMove || HasSafeFloorAt(BeaconLoc, 1600.f);
}

void AArenaBotController::ClearTranslocation(bool bSwitchBack)
{
	// MonitoringThrow.EndState: TranslocationTarget / RealTranslocationTarget = None, bPreparingMove = false,
	// SwitchToBestWeapon
	bHasTranslocTarget = false;
	bPreparingMove = false;
	bTranslocatorHop = false;
	bTranslocThrown = false;
	TranslocTargetActor.Reset();
	if (AWeap_Translocator* T = GetTranslocator())
	{
		T->bBotPreparing = false;
	}
	if (bSwitchBack && GetArenaPawn())
	{
		NextWeaponSwitchTime = 0.f;
		ChooseWeapon();
	}
}

void AArenaBotController::OnBeaconMonitorEnd(AArenaTransBeacon* Beacon, bool bFailedMove, float FearCost, bool bMoved)
{
	if (!bHasTranslocTarget)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const bool bWasPreparing = bPreparingMove;
	if (BotLog())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("BotTransloc: %s %s (%s)%s"), *GetName(), bMoved ? TEXT("translocated") : TEXT("beacon done"),
			bWasPreparing ? TEXT("translocator route") : (bTranslocGameObject ? TEXT("catching the flag / ball") : TEXT("hop")),
			bFailedMove ? TEXT(", the move failed") : TEXT(""));
	}
	if (bFailedMove && bWasPreparing)
	{
		// UE2: MoveTimer = -1 and JumpSpot.FearCost += FearCost; here the route is avoided for a few seconds
		NextTranslocTime = FMath::Max(NextTranslocTime, Now + 3.f);
	}
	ClearTranslocation(true);
	if (bMoved || bFailedMove)
	{
		// re-plan from the new spot
		Path.Reset();
		PathLinks.Reset();
		bPathFailed = false;
		NextRepathTime = 0.f;
		PathTime = -100.f;
	}
}

bool AArenaBotController::TryTranslocatorLink(const FVector& From, const FVector& To)
{
	// JumpSpot.TryTranslocator: stop, bPreparingMove, TranslocationTarget = the JumpSpot; the translocator comes up (or
	// its Timer fires FireHack in 0.2 s when already held)
	if (!CanUseTranslocator())
	{
		return false;
	}
	const AArenaNavGraph* Nav = GetNavGraph();
	const AArenaNavGraph::FTranslocLink* L = Nav ? Nav->FindTranslocLink(From, To) : nullptr;
	if (!L)
	{
		return false;
	}
	bHasTranslocTarget = true;
	bPreparingMove = true;
	bTranslocatorHop = false;
	bTranslocGameObject = false;
	bTranslocThrown = false;
	TranslocTargetActor.Reset();
	TranslocTargetLoc = L->Target;
	RealTranslocTargetLoc = L->Target;
	TranslocAimZ = L->AimZ;
	TranslocTargetRadius = L->Radius;
	TranslocTargetHeight = L->Height;
	TranslocStartTime = GetWorld()->GetTimeSeconds();
	if (BotLog())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("BotTransloc: %s takes a translocator route %s -> %s (TranslocZOffset %.0f)"), *GetName(), *From.ToCompactString(),
			*L->Target.ToCompactString(), L->AimZ);
	}
	return true;
}

bool AArenaBotController::UpdateTranslocator(float DeltaSeconds, const FVector& Loc)
{
	AArenaCharacter* P = GetArenaPawn();
	AWeap_Translocator* T = GetTranslocator();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	if (!P || !T)
	{
		if (bHasTranslocTarget)
		{
			ClearTranslocation(false);
		}
		return false;
	}
	if (Now >= NextBeaconShootCheck && !bHasTranslocTarget)
	{
		NextBeaconShootCheck = Now + 0.5f;
		ShouldDestroyTranslocator();
	}
	const UCharacterMovementComponent* CMC = P->GetCharacterMovement();
	const bool bOnGround = CMC && CMC->IsMovingOnGround();
	auto LineOfSightTo = [&](const FVector& Point)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotTranslocLOS), false, P);
		return !World->LineTraceTestByChannel(P->GetPawnViewLocation(), Point, ECC_Visibility, Q);
	};

	if (bHasTranslocTarget)
	{
		if (IsCarryingObjective() || (bTranslocGameObject && !TranslocTargetActor.IsValid()))
		{
			ClearTranslocation(true);
			return false;
		}
		if (AActor* A = TranslocTargetActor.Get())
		{
			TranslocTargetLoc = A->GetActorLocation();
			RealTranslocTargetLoc = TranslocTargetLoc;
		}
		if (!bTranslocThrown)
		{
			if (P->GetCurrentWeapon() != T)
			{
				StopFiring();
				P->SwitchToWeapon(T);
			}
			T->bBotPreparing = true;
			const FVector Aim = TranslocTargetLoc + FVector(0.f, 0.f, TranslocAimZ);
			const FVector ToTarget = TranslocTargetLoc - P->GetActorLocation();
			// Focus = the target (FaceActor / TryTranslocator)
			if (!Enemy || !bEnemyVisible || bPreparingMove)
			{
				DesiredRotation = (Aim - P->GetPawnViewLocation()).Rotation();
				DesiredRotation.Roll = 0.f;
				LookAtUntil = Now + 0.3f;
			}
			// TransLauncher.Timer after the bring-up (or 0.2 s when already held): FireHack(0), for a hop only when
			// ShouldTranslocatorHop (facing the target: Vector(Rotation) dot direction >= 0.5, else again in 0.1 s)
			bool bReady = T->IsReadyForBotThrow() && Now >= TranslocStartTime + 0.2f;
			if (bReady && bTranslocatorHop)
			{
				const float Dist = ToTarget.Size();
				if (Dist < 200.f * UU)
				{
					ClearTranslocation(true);
					return false;
				}
				bReady = FVector::DotProduct(GetControlRotation().Vector(), ToTarget.GetSafeNormal()) >= 0.5f;
			}
			if (bReady)
			{
				// FireHack: JumpSpot targets get TranslocZOffset; a moving target is led by 0.3 s (half of it up); a
				// bot falling below its target in normal gravity throws 128 UU higher
				FVector TTargetLoc = Aim;
				const AActor* A = TranslocTargetActor.Get();
				const FVector TargetVel = A ? A->GetVelocity() : FVector::ZeroVector;
				if (!TargetVel.IsNearlyZero())
				{
					TTargetLoc += 0.3f * TargetVel;
					TTargetLoc.Z = 0.5f * (TTargetLoc.Z + TranslocTargetLoc.Z);
				}
				else if (CMC && CMC->IsFalling() && P->GetActorLocation().Z < TranslocTargetLoc.Z
					&& ArenaTranslocator::GravityZAt(World, P->GetActorLocation()) > -800.f * UU)
				{
					TTargetLoc.Z += 128.f * UU;
				}
				if (T->BotThrow(TTargetLoc, TranslocTargetActor.Get(), TranslocTargetLoc, TranslocTargetRadius, TranslocTargetHeight, bTranslocGameObject))
				{
					bTranslocThrown = true;
					NumTranslocThrows++;
					TranslocStartTime = Now;
					if (BotLog())
					{
						UE_LOG(LogOpenUT2k4, Log, TEXT("BotTransloc: %s throws at %s (%.0f cm away)"), *GetName(), *TTargetLoc.ToCompactString(), ToTarget.Size());
					}
				}
			}
			if (!bTranslocThrown && Now - TranslocStartTime > 3.f)
			{
				ClearTranslocation(true);
				return false;
			}
		}
		else if (!T->GetBeacon() || Now - TranslocStartTime > 4.f)
		{
			// the beacon went without a callback (recalled, destroyed) or never reached the target
			ClearTranslocation(true);
			return false;
		}
		// JumpSpot.TryTranslocator: Acceleration = 0 while preparing the move; a hop keeps running
		return bPreparingMove;
	}

	// the path's next link needs the translocator: throw once at its start
	if (PathLinks.Num() > 0 && PathLinks[0] == EArenaNavLinkType::Translocate && Path.Num() > 0)
	{
		if (bOnGround && FVector::Dist2D(Loc, PrevPathPoint) < 90.f && TryTranslocatorLink(PrevPathPoint, Path[0]))
		{
			return true;
		}
		return false;
	}

	// Bot.FaceActor's translocator hop, decided when the bot picks its next move target
	if (Now < NextHopCheck || Path.Num() < 2 || !bOnGround || !CanUseTranslocator())
	{
		return false;
	}
	NextHopCheck = Now + 1.f;
	// our nav graph is denser than UE2's path nodes: MoveTarget / RouteCache[1] / [2] are the path points about every
	// 1200 cm ahead (walking links only; a lift, pad or teleporter ends the run like LiftCenter)
	TArray<FVector, TInlineAllocator<4>> Route;
	{
		float Acc = 0.f;
		FVector Prev = Loc;
		for (int32 i = 0; i < Path.Num() && Route.Num() < 3; ++i)
		{
			const EArenaNavLinkType K = PathLinks[i];
			if (K != EArenaNavLinkType::Walk && K != EArenaNavLinkType::Jump && K != EArenaNavLinkType::Drop)
			{
				break;
			}
			Acc += FVector::Dist(Prev, Path[i]);
			Prev = Path[i];
			if (Acc >= 1200.f * (Route.Num() + 1) || i == Path.Num() - 1)
			{
				Route.Add(Path[i]);
			}
		}
	}
	if (Route.Num() == 0)
	{
		return false;
	}
	AActor* RouteGoal = GoalActor.Get();
	const AArenaCharacter* GoalPawn = Cast<AArenaCharacter>(RouteGoal);
	const bool bGameObjectGoal = IsGameObject(RouteGoal);
	const bool bEnemyNotEngaged = !Enemy || Now - LastSeenEnemyTime > 1.f;
	const bool bCatchup = (GoalPawn && GoalPawn != P && GoalPawn->GetController() && !ArenaWeaponActors::SameTeam(World, this, GoalPawn->GetController()))
		|| bGameObjectGoal;
	bool bTranslocTactics = false;
	if (bEnemyNotEngaged)
	{
		bTranslocTactics = bCatchup ? (Skill + Tactics > 2.f + 2.f * Rng.FRand()) : (Skill + Tactics > 4.f);
	}
	bTranslocTactics = bTranslocTactics || (Skill + Tactics > 2.5f + 3.f * Rng.FRand());
	const FVector MoveTarget = Route[0];
	const bool bFarEnemy = Enemy && FVector::Dist(Enemy->GetActorLocation(), Loc) > 1200.f * UU * (1.f + Rng.FRand());
	const bool bCatchupNow = bCatchup && Rng.FRand() < 0.65f && (bGameObjectGoal || !LineOfSightTo(RouteGoal ? RouteGoal->GetActorLocation() : GoalLocation));
	if (!(bTranslocTactics && ArenaTranslocator::BotTranslocUse > Rng.FRand() && TranslocFreq < Now + 6.f + 9.f * Rng.FRand()
		&& (bEnemyNotEngaged || bFarEnemy || bCatchupNow)))
	{
		return false;
	}
	// target: MoveTarget, the flag / ball when it is close and in sight, else maybe AlternateTranslocDest
	FVector Target = MoveTarget;
	AActor* TargetActor = nullptr;
	float Dist = FVector::Dist(Loc, Target);
	const float MinDist = (300.f + 40.f * FMath::Max(0.f, TranslocFreq - Now)) * UU;
	if (bGameObjectGoal && FVector::Dist(Loc, RouteGoal->GetActorLocation()) < (1000.f + 1200.f * Rng.FRand()) * UU && LineOfSightTo(RouteGoal->GetActorLocation()))
	{
		Target = RouteGoal->GetActorLocation();
		TargetActor = RouteGoal;
		Dist = FVector::Dist(Loc, Target);
	}
	else if ((MinDist + (200.f + 1000.f * Rng.FRand()) * UU) > Dist)
	{
		// Bot.AlternateTranslocDest: RouteCache[2] (30%) or RouteCache[1] when in sight
		FVector N = FVector::ZeroVector;
		bool bN = false;
		if (Route.Num() > 2 && Rng.FRand() < 0.3f && LineOfSightTo(Route[2]))
		{
			N = Route[2];
			bN = true;
		}
		else if (Route.Num() > 1 && LineOfSightTo(Route[1]))
		{
			N = Route[1];
			bN = true;
		}
		if (bN)
		{
			Target = N;
			Dist = FVector::Dist(Loc, Target);
		}
	}
	const bool bHolding = Cast<AWeap_Translocator>(P->GetCurrentWeapon()) != nullptr;
	if (!HasSafeFloorAt(Target, 400.f))
	{
		return false;
	}
	if (Dist < MinDist || (Dist < MinDist + 150.f * UU && !bHolding) || (!TargetActor && !LineOfSightTo(Target)))
	{
		return false;
	}
	bHasTranslocTarget = true;
	bPreparingMove = false;
	bTranslocatorHop = true;
	bTranslocGameObject = TargetActor != nullptr;
	bTranslocThrown = false;
	TranslocTargetActor = TargetActor;
	TranslocTargetLoc = Target;
	RealTranslocTargetLoc = Target;
	TranslocAimZ = 0.f;
	// NavigationPoint collision 40 x 43 UU; the flag / ball's own size is about the same
	TranslocTargetRadius = 40.f * UU;
	TranslocTargetHeight = 43.f * UU;
	TranslocStartTime = Now;
	if (BotLog())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("BotTransloc: %s hops to %s (%.0f cm, %s)"), *GetName(), *Target.ToCompactString(), Dist,
			TargetActor ? TEXT("the flag / ball") : (bCatchup ? TEXT("catching up") : TEXT("path")));
	}
	return false;
}

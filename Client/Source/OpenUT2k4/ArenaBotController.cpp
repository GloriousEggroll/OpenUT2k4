#include "ArenaBotController.h"
#include "ArenaMover.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaProjectile.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaPickup.h"
#include "ArenaFlag.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaWorldZones.h"
#include "ArenaJumpPad.h"
#include "ArenaCharacterMovement.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"

namespace ArenaBotTrace
{
	/**
	 * Movement probes (floors, ledges, walls): what blocks pawns. A WorldStatic channel trace that ignores pawns, like the
	 * nav graph's; object type queries would also hit meshes pawns pass through (UE2 bBlockNonZeroExtentTraces=False
	 * decoration, e.g. CTF-Citadel's planes over the canyon) and take them for floor.
	 */
	const FCollisionResponseParams& WorldOnly()
	{
		static const FCollisionResponseParams R = []()
		{
			FCollisionResponseParams P(ECR_Ignore);
			P.CollisionResponse.SetResponse(ECC_WorldStatic, ECR_Block);
			return P;
		}();
		return R;
	}
}

namespace ArenaBotTuning
{
	constexpr float SightInterval = 0.1f;
	constexpr float RepathInterval = 0.5f;
	constexpr float WeaponSwitchInterval = 2.f;
	constexpr float ReachRadius = 120.f;
	constexpr float StuckSpeed = 90.f;
	constexpr float StuckTimeLimit = 1.f;
	constexpr float InvisibleNoticeRange = 800.f;
	// Void safety: a dodge covers ~600 cm (1320 cm/s for ~0.45 s), a hop at run speed ~650 cm
	constexpr float DodgeCheckDist = 700.f;
	constexpr float HopCheckDist = 700.f;
	constexpr float SafeSampleStep = 100.f;
}

AArenaBotController::AArenaBotController()
{
	PrimaryActorTick.bCanEverTick = true;
	bWantsPlayerState = true;
	bSetControlRotationFromPawnOrientation = false;
	Rng.Initialize(0x5EED);
}

void AArenaBotController::SetSkill(float InSkill)
{
	Skill = FMath::Clamp(InSkill, 0.f, 7.f);
}

AArenaCharacter* AArenaBotController::GetArenaPawn() const
{
	return Cast<AArenaCharacter>(GetPawn());
}

AArenaNavGraph* AArenaBotController::GetNavGraph() const
{
	UWorld* World = GetWorld();
	AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	return GM ? GM->GetNavGraph() : nullptr;
}

int32 AArenaBotController::GetMyTeam() const
{
	const AArenaPlayerState* PS = GetPlayerState<AArenaPlayerState>();
	return PS ? PS->Team : ArenaTeam::None;
}

float AArenaBotController::GetReactionTime() const
{
	// UE2 bots decide to fire on their combat timer (Bot.SetCombatTimer): 1.2 - 0.09 x min(10, skill + ReactionTime) s
	return GetCombatTimerPeriod();
}

void AArenaBotController::ResetState()
{
	Enemy = nullptr;
	bEnemyVisible = false;
	LastSeenEnemyTime = -100.f;
	GoalActor.Reset();
	GoalKind = EArenaBotGoal::None;
	CurrentObjective = FArenaBotObjective();
	bGoalReached = false;
	GoalScore = 0.f;
	Path.Reset();
	PathLinks.Reset();
	bPathFailed = false;
	PathTime = -100.f;
	NextGoalTime = 0.f;
	NextRepathTime = 0.f;
	NextSightCheckTime = 0.f;
	StuckTime = 0.f;
	StuckCount = 0;
	JumpPhase = 0;
	bFiring = false;
	FiringMode = 0;
	ReactionTimer = 0.f;
	LookAtUntil = 0.f;
	NextWeaponSwitchTime = 0.f;
	AimError = FVector::ZeroVector;
	NextAimErrorTime = 0.f;
	LastMoveDir = FVector::ZeroVector;
	bHasSafeGround = false;
	DirectMoveUntil = -1.f;
	bDirectMoveJump = false;
	PadFlightStart = -1.f;
}

void AArenaBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	ResetState();

	if (!bRngSeeded)
	{
		bRngSeeded = true;
		const AArenaPlayerState* PS = GetPlayerState<AArenaPlayerState>();
		const int32 Id = PS ? PS->GetPlayerId() : int32(GetUniqueID());
		Rng.Initialize(0x2A51 + Id * 7919);
		// Roughly one bot in three defends in CTF
		bDefender = (Id % 3) == 0;
	}

	if (InPawn)
	{
		DesiredRotation = InPawn->GetActorRotation();
		DesiredRotation.Pitch = 0.f;
		DesiredRotation.Roll = 0.f;
		SetControlRotation(DesiredRotation);
		LastPosition = InPawn->GetActorLocation();
	}
}

void AArenaBotController::OnUnPossess()
{
	StopFiring();
	if (ACharacter* C = Cast<ACharacter>(GetPawn()))
	{
		C->StopJumping();
	}
	Super::OnUnPossess();
	ResetState();
}

void AArenaBotController::NotifyPawnDied()
{
	bFiring = false;
	ResetState();
}

void AArenaBotController::NotifyTookDamage(AController* InstigatedBy, const FVector& FromLocation)
{
	AArenaCharacter* P = GetArenaPawn();
	if (!P || P->IsDead())
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();

	if (InstigatedBy && InstigatedBy != this && !(GM && GM->AreOnSameTeam(this, InstigatedBy)))
	{
		AArenaCharacter* Attacker = Cast<AArenaCharacter>(InstigatedBy->GetPawn());
		if (Attacker && Attacker->IsAlive() && (!Enemy || !bEnemyVisible || Enemy == Attacker))
		{
			if (Enemy != Attacker)
			{
				ReactionTimer = GetReactionTime() * 0.7f;
			}
			Enemy = Attacker;
			LastSeenEnemyLocation = Attacker->GetActorLocation();
			LastSeenEnemyTime = Now;
		}
	}

	// "Hear" the hit: turn towards the source unless we are already looking at a visible enemy
	if (!bEnemyVisible)
	{
		DesiredRotation = (FromLocation - P->GetPawnViewLocation()).Rotation();
		DesiredRotation.Roll = 0.f;
		LookAtUntil = Now + 1.f;
	}
	NextSightCheckTime = 0.f;
}

void AArenaBotController::UpdateControlRotation(float DeltaTime, bool bUpdatePawn)
{
	APawn* P = GetPawn();
	if (!P)
	{
		return;
	}

	const FRotator Current = GetControlRotation().GetNormalized();
	const FRotator Target = DesiredRotation.GetNormalized();
	const FRotator Delta = (Target - Current).GetNormalized();

	// Turn rate (deg/s) and responsiveness scale with skill
	// UnrealGame.Bot.ResetSkill RotationRate.Yaw (65536 = 360 deg): 30000 + 4000 x skill, 7000 + 10000 x skill from skill 4
	const float MaxRate = GetRotationRateYaw();
	const float Responsiveness = FMath::Lerp(5.f, 14.f, Skill / 7.f);
	const float MinRate = 40.f;

	auto Step = [&](float D) -> float
	{
		const float AbsD = FMath::Abs(D);
		float Amount = FMath::Max(AbsD * Responsiveness * DeltaTime, MinRate * DeltaTime);
		Amount = FMath::Min(Amount, MaxRate * DeltaTime);
		Amount = FMath::Min(Amount, AbsD);
		return FMath::Sign(D) * Amount;
	};

	FRotator NewRot = Current;
	NewRot.Yaw += Step(Delta.Yaw);
	NewRot.Pitch = FMath::Clamp(NewRot.Pitch + Step(Delta.Pitch), -88.f, 88.f);
	NewRot.Roll = 0.f;
	NewRot.Yaw = FRotator::NormalizeAxis(NewRot.Yaw);

	SetControlRotation(NewRot);
	if (bUpdatePawn)
	{
		P->FaceRotation(NewRot, DeltaTime);
	}
}

void AArenaBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds); // calls UpdateControlRotation

	AArenaCharacter* P = GetArenaPawn();
	if (!P || P->IsDead())
	{
		bFiring = false;
		return;
	}

	UWorld* World = GetWorld();
	AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	if (GM && !GM->IsPlaying())
	{
		StopFiring();
		return;
	}

	// a rules objective may make the bot use something that possesses another pawn (an Assault turret) in the middle of
	// the tick: stop as soon as the character is no longer ours
	UpdateEnemy(DeltaSeconds);
	UpdateGoal(DeltaSeconds);
	if (GetArenaPawn() != P)
	{
		return;
	}
	UpdateMovement(DeltaSeconds);
	if (GetArenaPawn() != P)
	{
		return;
	}
	UpdateAimAndFire(DeltaSeconds);
	if (GetArenaPawn() != P)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	// -BotDebug=<seconds>: log every bot's state at that interval (goal, path, enemy, weapon)
	static float DebugInterval = -1.f;
	if (DebugInterval < 0.f)
	{
		DebugInterval = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("BotDebug="), DebugInterval);
	}
	if (DebugInterval > 0.f && Now >= NextDebugTime)
	{
		NextDebugTime = Now + DebugInterval;
		static const TCHAR* KindNames[] = { TEXT("None"), TEXT("Wander"), TEXT("Pickup"), TEXT("Objective"), TEXT("Hunt") };
		const AArenaPlayerState* PS = GetPlayerState<AArenaPlayerState>();
		const AArenaWeapon* W = P->GetCurrentWeapon();
		UE_LOG(LogOpenUT2k4, Log, TEXT("BotDebug: %s t%d at %s goal=%s tag=%d score=%.2f dist=%.0f path=%d%s enemy=%s%s weapon=%s firing=%d hp=%d mode=%d speed=%.0f pad=%d direct=%d jump=%d stuck=%d link0=%d mt=%s dir=%s"),
			PS ? *PS->GetPlayerName() : TEXT("?"), GetMyTeam(), *P->GetActorLocation().ToCompactString(), KindNames[FMath::Min<int32>(int32(GoalKind), 4)],
			GoalKind == EArenaBotGoal::Objective ? CurrentObjective.Tag : -1, GoalScore, FVector::Dist(P->GetActorLocation(), GoalLocation), Path.Num(),
			bPathFailed ? TEXT(" FAILED") : TEXT(""), Enemy ? *Enemy->GetName() : TEXT("-"), bEnemyVisible ? TEXT("(vis)") : TEXT(""),
			W ? *W->GetClass()->GetName() : TEXT("-"), bFiring ? 1 : 0, int32(P->Health), P->GetCharacterMovement() ? int32(P->GetCharacterMovement()->MovementMode) : -1,
			P->GetVelocity().Size(), PadFlightStart >= 0.f ? 1 : 0, Now < DirectMoveUntil ? 1 : 0, JumpPhase, StuckCount, PathLinks.Num() > 0 ? int32(PathLinks[0]) : -1,
			*(CurMoveTarget - P->GetActorLocation()).ToCompactString(), *LastMoveDir.ToCompactString());
	}
	// Bot.Stopped (bPreparingMove): standing still; StopStartTime feeds AdjustAimError
	const bool bStoppedNow = P->GetVelocity().Size2D() < 20.f && P->GetCharacterMovement() && P->GetCharacterMovement()->IsMovingOnGround();
	if (bStoppedNow && !bStoppedUE2)
	{
		StopStartTime = Now;
	}
	bStoppedUE2 = bStoppedNow;
	// Bot.DelayedWarning after ReceiveProjectileWarning's WarningDelay
	if (WarningProjectile.IsValid() && Now >= WarningDelay)
	{
		DelayedWarning();
	}
	else if (!WarningProjectile.IsValid())
	{
		WarningProjectile.Reset();
	}
	// ShockProjectile.WaitForCombo.Tick: detonate when the core is within ComboRadius / 2 of the enemy or passes it
	if (bWaitForCombo)
	{
		AArenaProjectile* Core = ComboCore.Get();
		if (!Core || !Enemy || Enemy->IsDead() || !Cast<AWeap_PulseRifle>(P->GetCurrentWeapon()))
		{
			if (bComboTriggered)
			{
				StopFiring();
			}
			bWaitForCombo = false;
			bComboTriggered = false;
			ComboCore.Reset();
		}
		else
		{
			const FVector ToEnemy = Enemy->GetActorLocation() - Core->GetActorLocation();
			if (!bComboTriggered && (ToEnemy.Size() <= 275.f + Enemy->GetSimpleCollisionRadius() || FVector::DotProduct(Core->GetVelocity(), ToEnemy) <= 0.f))
			{
				// ShockRifle.DoCombo: StartFire(0), held until the beam fires (it aims at the core: ShockBeamFire.AdjustAim)
				bComboTriggered = true;
				StopFiring();
				StartFiring(0);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------------------------

bool AArenaBotController::CanSee(const AActor* Other) const
{
	const APawn* P = GetPawn();
	if (!P || !Other)
	{
		return false;
	}
	const FVector Eye = P->GetPawnViewLocation();
	const FVector ToOther = Other->GetActorLocation() - Eye;
	const float Dist = ToOther.Size();
	if (Dist > 15000.f)
	{
		return false;
	}
	if (Dist > 250.f)
	{
		// Bot.SetPeripheralVision: cos of the half view angle 0.7 below skill 2, -0.2 above 6, else 1 - 0.2 x skill
		const float Peripheral = Skill < 2.f ? 0.7f : (Skill > 6.f ? -0.2f : 1.f - 0.2f * Skill);
		const FVector Fwd = GetControlRotation().Vector();
		if (FVector::DotProduct(Fwd, ToOther / Dist) < Peripheral)
		{
			return false;
		}
	}
	return LineOfSightTo(Other, Eye);
}

void AArenaBotController::UpdateEnemy(float DeltaSeconds)
{
	using namespace ArenaBotTuning;

	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();

	if (Enemy && (!IsValid(Enemy) || Enemy->IsDead()))
	{
		Enemy = nullptr;
		bEnemyVisible = false;
	}

	if (Now < NextSightCheckTime)
	{
		return;
	}
	NextSightCheckTime = Now + SightInterval;

	AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
	UArenaGameRules* Rules = GM ? GM->GetRules() : nullptr;
	const FVector MyLoc = P->GetActorLocation();

	AArenaCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (TActorIterator<AArenaCharacter> It(World); It; ++It)
	{
		AArenaCharacter* Other = *It;
		if (Other == P || !IsValid(Other) || Other->IsDead())
		{
			continue;
		}
		AController* OtherController = Other->GetController();
		if (GM && OtherController && GM->AreOnSameTeam(this, OtherController))
		{
			continue;
		}
		const float Dist = FVector::Dist(MyLoc, Other->GetActorLocation());
		if (Other->IsInvisible() && Dist > InvisibleNoticeRange && Other != Enemy)
		{
			// Hard to spot: better bots notice more often
			if (Rng.FRand() > 0.05f + 0.04f * Skill)
			{
				continue;
			}
		}
		if (!CanSee(Other))
		{
			continue;
		}
		float Score = Dist;
		if (Other == Enemy)
		{
			Score *= 0.7f; // stick with the current target
		}
		if (Rules)
		{
			Score *= Rules->GetBotEnemyWeight(this, Other); // e.g. the carrier of our flag is the priority
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Other;
		}
	}

	if (Best)
	{
		if (Best != Enemy || !bEnemyVisible)
		{
			// New target (or reacquired): reaction delay before shooting
			if (Best != Enemy || Now - LastSeenEnemyTime > 0.5f)
			{
				// the first shot waits for the next tick of the repeating combat timer
				ReactionTimer = GetReactionTime() * Rng.FRand();
			}
			if (Best != Enemy)
			{
				AcquireTime = Now; // Bot.AcquireTime: aim error x1.5 for a while after a new enemy
				NextWeaponSwitchTime = 0.f; // Bot.SetEnemy -> WhatToDoNext: SwitchToBestWeapon
			}
			NextAimErrorTime = 0.f;
		}
		Enemy = Best;
		bEnemyVisible = true;
		LastSeenEnemyLocation = Best->GetActorLocation();
		LastSeenEnemyTime = Now;
	}
	else
	{
		bEnemyVisible = false;
		if (Enemy && Now - LastSeenEnemyTime > GetEnemyMemoryTime())
		{
			Enemy = nullptr;
		}
		// UE2 SquadAI.Enemies: a squad shares what its members see (FindNewEnemyFor picks among them). A bot with no
		// enemy of its own takes on the nearest enemy a squadmate (same team and role) is fighting close by.
		if (!Enemy && Now >= NextSquadEnemyTime && GM && GM->IsTeamGame())
		{
			NextSquadEnemyTime = Now + 0.5f;
			float BestDist = FMath::Square(4000.f * (1.f + 0.5f * Tactics));
			for (TActorIterator<AArenaBotController> It(World); It; ++It)
			{
				const AArenaBotController* Mate = *It;
				AArenaCharacter* MateEnemy = Mate->Enemy;
				if (Mate == this || !Mate->bEnemyVisible || !MateEnemy || !IsValid(MateEnemy) || MateEnemy->IsDead()
					|| Mate->IsDefender() != bDefender || !GM->AreOnSameTeam(this, Mate) || !Mate->GetPawn())
				{
					continue;
				}
				const float D = FVector::DistSquared(MyLoc, Mate->GetPawn()->GetActorLocation());
				if (D < BestDist)
				{
					BestDist = D;
					Enemy = MateEnemy;
					LastSeenEnemyLocation = Mate->LastSeenEnemyLocation;
					LastSeenEnemyTime = Mate->LastSeenEnemyTime - 1.f;
				}
			}
		}
	}
}

float AArenaBotController::GetEnemyMemoryTime() const
{
	// Bot.ExecuteWhatToDoNext: LostContact(4) while defending, LostContact(7) otherwise
	const AArenaGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr;
	return (bDefender && GM && GM->IsTeamGame()) ? 4.f : 7.f;
}

void AArenaBotController::ReportNoise(APawn* Maker, const FVector& Location, float Loudness)
{
	UWorld* World = Maker ? Maker->GetWorld() : nullptr;
	if (!World || Loudness <= 0.f)
	{
		return;
	}
	for (TActorIterator<AArenaBotController> It(World); It; ++It)
	{
		if (It->GetPawn() && It->GetPawn() != Maker)
		{
			It->HearNoise(Maker, Location, Loudness);
		}
	}
}

void AArenaBotController::HearNoise(APawn* Maker, const FVector& Location, float Loudness)
{
	AArenaCharacter* P = GetArenaPawn();
	AArenaCharacter* Other = Cast<AArenaCharacter>(Maker);
	UWorld* World = GetWorld();
	if (!P || P->IsDead() || !Other || Other->IsDead() || !World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	// busy with an enemy in sight, or heard something a moment ago (Bot.HearNoise runs WhatToDoNext at most so often)
	if ((bEnemyVisible && Enemy) || Now - LastHeardTime < 0.3f)
	{
		return;
	}
	AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
	if (GM && Other->GetController() && GM->AreOnSameTeam(this, Other->GetController()))
	{
		return;
	}
	// UE2 Pawn.HearingThreshold 2800 UU (5600 cm); loudness scales the squared distance
	const FVector Ear = P->GetPawnViewLocation();
	const float Range = 5600.f * FMath::Sqrt(Loudness);
	const float Dist = FVector::Dist(Ear, Location);
	if (Dist > Range)
	{
		return;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBotHear), false, P);
	Q.AddIgnoredActor(Maker);
	const bool bClear = !World->LineTraceTestByObjectType(Ear, Location, FCollisionObjectQueryParams(ECC_WorldStatic), Q);
	// bLOSHearing with bMuffledHearing: through walls only from much closer
	if (!bClear && Dist > Range * 0.35f)
	{
		return;
	}
	LastHeardTime = Now;
	if (Enemy != Other)
	{
		AcquireTime = Now;
	}
	Enemy = Other;
	LastSeenEnemyLocation = Other->GetActorLocation();
	LastSeenEnemyTime = Now;
	// turn towards it: the sight check sees it as soon as it is in the view cone
	DesiredRotation = (Location - Ear).Rotation();
	DesiredRotation.Roll = 0.f;
	LookAtUntil = Now + 1.f;
	NextSightCheckTime = 0.f;
	NextWeaponSwitchTime = 0.f;
}

// ---------------------------------------------------------------------------------------------
// Goals
// ---------------------------------------------------------------------------------------------

void AArenaBotController::UpdateGoal(float DeltaSeconds)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	const FVector MyLoc = P->GetActorLocation();
	AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
	UArenaGameRules* Rules = GM ? GM->GetRules() : nullptr;

	// ---- refresh the current goal ----
	bool bForce = GoalKind == EArenaBotGoal::None;
	AActor* CurGoalActor = GoalActor.Get();
	switch (GoalKind)
	{
	case EArenaBotGoal::Pickup:
		if (const AArenaPickup* Pickup = Cast<AArenaPickup>(CurGoalActor))
		{
			bForce |= !Pickup->IsAvailable();
		}
		else
		{
			bForce = true;
		}
		break;
	case EArenaBotGoal::Objective:
	{
		const bool bValid = Rules && Rules->UpdateBotObjective(this, CurrentObjective);
		GoalLocation = CurrentObjective.Location;
		bForce |= !bValid;
		break;
	}
	case EArenaBotGoal::Hunt:
		bForce |= Enemy == nullptr || bEnemyVisible;
		if (Enemy)
		{
			GoalLocation = LastSeenEnemyLocation;
		}
		break;
	default:
		break;
	}

	const bool bReached = FVector::Dist2D(MyLoc, GoalLocation) < 100.f && FMath::Abs(MyLoc.Z - GoalLocation.Z) < 150.f;
	bGoalReached = bReached;
	const bool bCompletesOnReach = GoalKind != EArenaBotGoal::Objective || CurrentObjective.bCompleteOnReach;
	if (bReached && GoalKind != EArenaBotGoal::None && bCompletesOnReach)
	{
		if (GoalKind == EArenaBotGoal::Pickup)
		{
			// Avoid fixating on an item we could not take
			IgnoredGoalActor = GoalActor;
			IgnoreGoalUntil = Now + 5.f;
		}
		bForce = true;
	}

	if (!bForce && Now < NextGoalTime)
	{
		return;
	}
	NextGoalTime = Now + Rng.FRandRange(0.8f, 1.4f);

	// ---- evaluate candidates ----
	EArenaBotGoal BestKind = EArenaBotGoal::None;
	float BestScore = 0.f;
	FVector BestLoc = FVector::ZeroVector;
	AActor* BestActor = nullptr;
	int32 BestObjective = INDEX_NONE;

	auto Consider = [&](EArenaBotGoal Kind, float Score, const FVector& Loc, AActor* Actor, const FArenaBotObjective* Objective = nullptr, int32 ObjectiveIndex = INDEX_NONE)
	{
		// Hysteresis: prefer to keep the current goal
		bool bSame = Kind == GoalKind && Actor == GoalActor.Get() && !bForce;
		if (bSame && Objective)
		{
			bSame = Objective->Tag == CurrentObjective.Tag && Objective->Action == CurrentObjective.Action;
		}
		if (bSame)
		{
			Score *= 1.2f;
		}
		if (Score > BestScore)
		{
			BestScore = Score;
			BestKind = Kind;
			BestLoc = Loc;
			BestActor = Actor;
			BestObjective = ObjectiveIndex;
		}
	};

	// Pickups
	if (GM && GM->GetLevelBuilder())
	{
		for (AArenaPickup* Pickup : GM->GetLevelBuilder()->GetPickups())
		{
			if (!Pickup || !Pickup->IsAvailable())
			{
				continue;
			}
			if (Pickup == IgnoredGoalActor.Get() && Now < IgnoreGoalUntil)
			{
				continue;
			}
			if (Rules && !Rules->CanPickUp(P, Pickup))
			{
				continue;
			}
			const float Desire = Pickup->GetBotDesire(P);
			if (Desire <= 0.01f)
			{
				continue;
			}
			const float Dist = FVector::Dist(MyLoc, Pickup->GetActorLocation());
			if (bEnemyVisible && Dist > 1500.f)
			{
				continue; // only grab nearby items in a fight
			}
			Consider(EArenaBotGoal::Pickup, Desire * 2000.f / (Dist + 1000.f), Pickup->GetActorLocation(), Pickup);
		}
	}

	// Game mode objectives
	TArray<FArenaBotObjective> Objectives;
	if (Rules)
	{
		Rules->GetBotObjectives(this, Objectives);
		for (int32 i = 0; i < Objectives.Num(); ++i)
		{
			const FArenaBotObjective& O = Objectives[i];
			Consider(EArenaBotGoal::Objective, O.Priority, O.Location, O.Actor.Get(), &O, i);
		}
	}

	// Hunt a recently seen enemy
	if (Enemy && !bEnemyVisible && Now - LastSeenEnemyTime < GetEnemyMemoryTime())
	{
		const float RulesHunt = Rules ? Rules->GetBotHuntPriority(this, Enemy, LastSeenEnemyLocation) : -1.f;
		const float HuntScore = RulesHunt < 0.f ? FMath::Max(0.05f, 0.3f + 0.05f * Skill + 0.3f * Aggressiveness) : RulesHunt;
		if (HuntScore > 0.f)
		{
			Consider(EArenaBotGoal::Hunt, HuntScore, LastSeenEnemyLocation, Enemy);
		}
	}

	// Wander
	if (BestKind == EArenaBotGoal::None)
	{
		if (GoalKind == EArenaBotGoal::Wander && !bReached && !bForce)
		{
			BestLoc = GoalLocation;
		}
		else if (AArenaNavGraph* Nav = GetNavGraph(); Nav && Nav->NumNodes() > 0)
		{
			BestLoc = Nav->GetRandomNodeLocation(Rng);
		}
		else
		{
			const FVector Off = Rng.GetUnitVector() * 1500.f;
			BestLoc = MyLoc + FVector(Off.X, Off.Y, 0.f);
		}
		BestKind = EArenaBotGoal::Wander;
		BestScore = 0.05f;
		BestActor = nullptr;
	}

	const bool bChanged = BestKind != GoalKind || BestActor != GoalActor.Get() || FVector::DistSquared(BestLoc, GoalLocation) > FMath::Square(200.f);
	GoalKind = BestKind;
	GoalScore = BestScore;
	GoalActor = BestActor;
	GoalLocation = BestLoc;
	CurrentObjective = Objectives.IsValidIndex(BestObjective) ? Objectives[BestObjective] : FArenaBotObjective();
	if (bChanged)
	{
		Path.Reset();
		PathLinks.Reset();
		bPathFailed = false;
		NextRepathTime = FMath::Min(NextRepathTime, Now);
		StuckCount = 0;
	}
}

void AArenaBotController::SetGoal(const FArenaBotObjective& Objective)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	GoalKind = EArenaBotGoal::Objective;
	CurrentObjective = Objective;
	GoalScore = Objective.Priority;
	GoalActor = Objective.Actor;
	GoalLocation = Objective.Location;
	NextGoalTime = Now + Rng.FRandRange(0.8f, 1.4f);
	Path.Reset();
	PathLinks.Reset();
	bPathFailed = false;
	NextRepathTime = Now;
	StuckCount = 0;
}

void AArenaBotController::SetDirectMove(const FVector& Target, float Duration, bool bJump)
{
	DirectMoveTarget = Target;
	DirectMoveUntil = GetWorld()->GetTimeSeconds() + Duration;
	bDirectMoveJump = bJump;
}

void AArenaBotController::NotifyJumpPadLaunch(const FVector& JumpTarget)
{
	PadFlightTarget = JumpTarget;
	PadLaunchLoc = GetPawn() ? GetPawn()->GetActorLocation() : FVector::ZeroVector;
	PadFlightMaxZ = -1.0e9f;
	PadFlightStart = GetWorld()->GetTimeSeconds();
	DirectMoveUntil = -1.f;
	JumpPhase = 0;
}

bool AArenaBotController::IsDirectMoving() const
{
	return GetWorld() && GetWorld()->GetTimeSeconds() < DirectMoveUntil;
}

bool AArenaBotController::RequestUse()
{
	AArenaCharacter* P = GetArenaPawn();
	return P && P->IsAlive() && P->Use();
}

bool AArenaBotController::HasFlag() const
{
	const AArenaCharacter* P = GetArenaPawn();
	return P && P->HasFlag();
}

bool AArenaBotController::IsCarryingObjective() const
{
	const AArenaCharacter* P = GetArenaPawn();
	return P && P->IsCarryingObjective();
}

// ---------------------------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------------------------

void AArenaBotController::Repath()
{
	APawn* P = GetPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	Path.Reset();
	PathLinks.Reset();
	PathGoal = GoalLocation;
	PathTime = Now;
	NextRepathTime = Now + ArenaBotTuning::RepathInterval;
	PrevPathPoint = P ? P->GetActorLocation() : FVector::ZeroVector;

	AArenaNavGraph* Nav = GetNavGraph();
	bPathFailed = !(P && Nav && Nav->NumNodes() > 0 && Nav->FindPath(P->GetActorLocation(), GoalLocation, Path, PathLinks, CanUseTranslocator()));

	if (bPathFailed || Path.Num() != PathLinks.Num())
	{
		Path.Reset();
		PathLinks.Reset();
		bPathFailed = true;
	}
}

bool AArenaBotController::HasFloorAt(const FVector& Location, float MaxDrop) const
{
	const APawn* P = GetPawn();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaBotFloor), false, P);
	const float HalfHeight = ArenaTuning::CapsuleHalfHeight;
	return GetWorld()->LineTraceTestByChannel(Location, Location - FVector(0, 0, HalfHeight + MaxDrop), ECC_WorldStatic, Params,
		ArenaBotTrace::WorldOnly());
}

bool AArenaBotController::HasSafeFloorAt(const FVector& Location, float MaxDrop, float* OutFloorZ) const
{
	const UWorld* World = GetWorld();
	const AWorldSettings* WS = World ? World->GetWorldSettings() : nullptr;
	const float KillZ = WS ? WS->KillZ : -1.0e6f;
	// UE2 zones may have their own, higher kill Z (BR-Skyline: the holes under the goals); it kills when the
	// pawn's centre goes below it
	float ZoneKillZ = -1.0e9f;
	if (const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World))
	{
		if (const AArenaZoneInfo* Zone = Zones->GetZoneAt(Location))
		{
			ZoneKillZ = Zone->KillZ;
		}
	}
	const float HalfHeight = ArenaTuning::CapsuleHalfHeight;
	const float BottomZ = FMath::Max<float>(Location.Z - HalfHeight - MaxDrop, FMath::Max(KillZ, ZoneKillZ - HalfHeight));
	if (BottomZ >= Location.Z)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaBotSafeFloor), false, GetPawn());
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Location, FVector(Location.X, Location.Y, BottomZ), ECC_WorldStatic, Params,
		ArenaBotTrace::WorldOnly()))
	{
		return false;
	}
	// a floor only just above kill Z is no better than the void, nor is one in a lava / pain / kill volume (CTF-Face3's
	// fall volumes under the bridges)
	if (const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World))
	{
		if (Zones->HasVolumes() && Zones->IsHazardAt(Hit.ImpactPoint + FVector(0.f, 0.f, 20.f)))
		{
			return false;
		}
		// the zone the pawn lands in decides too: a canyon whose zone has a KillZ (CTF-Citadel's ZoneInfo1, -5600) kills a
		// pawn falling into it although the zone it jumps from has none
		if (const AArenaZoneInfo* FloorZone = Zones->GetZoneAt(Hit.ImpactPoint + FVector(0.f, 0.f, HalfHeight)))
		{
			if (Hit.ImpactPoint.Z + HalfHeight <= FloorZone->KillZ + 20.f)
			{
				return false;
			}
		}
	}
	if (OutFloorZ)
	{
		*OutFloorZ = Hit.ImpactPoint.Z;
	}
	return Hit.ImpactPoint.Z > KillZ + HalfHeight * 2.f && Hit.ImpactPoint.Z + HalfHeight > ZoneKillZ + 20.f;
}

bool AArenaBotController::IsSafeMove(const FVector& Dir, float Distance) const
{
	const APawn* P = GetPawn();
	const FVector D = Dir.GetSafeNormal2D();
	if (!P || D.IsNearlyZero())
	{
		return true;
	}
	const FVector Loc = P->GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaBotSafeMove), false, P);
	FHitResult Wall;
	// a wall ends the move early (we stop or slide along it)
	if (GetWorld()->LineTraceSingleByChannel(Wall, Loc, Loc + D * Distance, ECC_WorldStatic, Params, ArenaBotTrace::WorldOnly()))
	{
		Distance = FMath::Max(0.f, Wall.Distance - ArenaTuning::CapsuleRadius);
	}
	// running off a ledge carries the pawn on: a lower floor only counts where the fall actually lands
	const float FeetZ = Loc.Z - ArenaTuning::CapsuleHalfHeight;
	const float Speed = FMath::Max(float(P->GetVelocity().Size2D()), ArenaTuning::GroundSpeed);
	const UCharacterMovementComponent* CMC = Cast<ACharacter>(P) ? Cast<ACharacter>(P)->GetCharacterMovement() : nullptr;
	const float G = CMC ? FMath::Max(-CMC->GetGravityZ(), 100.f) : 1900.f;
	for (float T = FMath::Min(Distance, ArenaBotTuning::SafeSampleStep); T <= Distance + 1.f; T += ArenaBotTuning::SafeSampleStep)
	{
		float FloorZ = FeetZ;
		if (!HasSafeFloorAt(Loc + D * T, 1600.f, &FloorZ))
		{
			return false;
		}
		const float Drop = FeetZ - FloorZ;
		if (Drop > ArenaTuning::MaxStepHeight * 1.5f)
		{
			const float Carry = Speed * FMath::Sqrt(2.f * Drop / G);
			if (!HasSafeFloorAt(Loc + D * (T + Carry), Drop + 400.f))
			{
				return false;
			}
			break; // beyond the ledge the samples are the fall's
		}
	}
	return true;
}

FVector AArenaBotController::GuardEdge(const FVector& Dir) const
{
	const APawn* P = GetPawn();
	const FVector D = Dir.GetSafeNormal2D();
	if (!P || D.IsNearlyZero())
	{
		return Dir;
	}
	// Look further ahead the faster we run: the character needs ~0.25 s to brake from run speed
	const FVector Vel = P->GetVelocity();
	const float Look = FMath::Clamp(Vel.Size2D() * 0.3f, 100.f, 300.f) + ArenaTuning::CapsuleRadius;
	// the pawn's own momentum first: running at a ledge, turning takes longer than the ledge is away (UE2 walking keeps
	// the speed while the direction changes), so brake until the way it is sliding is safe
	const FVector VelDir = Vel.GetSafeNormal2D();
	if (Vel.Size2D() > 300.f && !IsSafeMove(VelDir, Vel.Size2D() * 0.2f + ArenaTuning::CapsuleRadius))
	{
		return -VelDir;
	}
	if (IsSafeMove(D, Look))
	{
		return Dir;
	}
	static const float Angles[] = { 30.f, -30.f, 60.f, -60.f, 90.f, -90.f, 135.f, -135.f };
	const float Side = StrafeDir >= 0.f ? 1.f : -1.f;
	for (const float A : Angles)
	{
		const FVector Test = D.RotateAngleAxis(A * Side, FVector::UpVector);
		if (IsSafeMove(Test, Look))
		{
			return Test;
		}
	}
	// Nothing safe ahead or sideways: push against our own motion to stop
	const FVector Back = -Vel.GetSafeNormal2D();
	return Back.IsNearlyZero() ? FVector::ZeroVector : Back;
}

bool AArenaBotController::IsBlockedAhead(const FVector& Dir, float Distance) const
{
	const APawn* P = GetPawn();
	if (!P)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaBotAhead), false, P);
	const FVector Loc = P->GetActorLocation();
	const FVector Reach = Dir.GetSafeNormal2D() * (Distance + ArenaTuning::CapsuleRadius);
	UWorld* World = GetWorld();
	// Chest height and just above step height
	const FVector Knee = Loc - FVector(0, 0, ArenaTuning::CapsuleHalfHeight - ArenaTuning::MaxStepHeight - 8.f);
	return World->LineTraceTestByChannel(Loc, Loc + Reach, ECC_WorldStatic, Params, ArenaBotTrace::WorldOnly()) ||
		World->LineTraceTestByChannel(Knee, Knee + Reach, ECC_WorldStatic, Params, ArenaBotTrace::WorldOnly());
}

FVector AArenaBotController::AvoidObstacles(const FVector& Dir, bool bAvoidPits) const
{
	const APawn* P = GetPawn();
	FVector D = Dir.GetSafeNormal2D();
	if (!P || D.IsNearlyZero())
	{
		return D;
	}
	const FVector Loc = P->GetActorLocation();
	auto IsOk = [&](const FVector& Test) -> bool
	{
		if (IsBlockedAhead(Test, 110.f))
		{
			return false;
		}
		return !bAvoidPits || HasFloorAt(Loc + Test * 120.f, 300.f);
	};
	if (IsOk(D))
	{
		return D;
	}
	static const float Angles[] = { 40.f, -40.f, 80.f, -80.f, 120.f, -120.f };
	// Alternate which side is tried first so bots don't all hug the same way
	const float Side = StrafeDir >= 0.f ? 1.f : -1.f;
	for (const float A : Angles)
	{
		const FVector Test = D.RotateAngleAxis(A * Side, FVector::UpVector);
		if (IsOk(Test))
		{
			return Test;
		}
	}
	// every direction is blocked or a pit: keep going only if it is not the void
	return (!bAvoidPits || HasSafeFloorAt(Loc + D * 120.f)) ? D : FVector::ZeroVector;
}

void AArenaBotController::StartJump(bool bInAllowDoubleJump)
{
	AArenaCharacter* P = GetArenaPawn();
	if (!P || JumpPhase != 0)
	{
		return;
	}
	P->Jump();
	JumpPhase = 1;
	JumpPhaseTime = GetWorld()->GetTimeSeconds();
	bAllowDoubleJump = bInAllowDoubleJump;
}

void AArenaBotController::UpdateJumpState(const FVector& MoveTarget, bool bJumpLink)
{
	AArenaCharacter* P = GetArenaPawn();
	if (!P || JumpPhase == 0)
	{
		return;
	}
	const UCharacterMovementComponent* CMC = P->GetCharacterMovement();
	const float Now = GetWorld()->GetTimeSeconds();
	const FVector Loc = P->GetActorLocation();
	const FVector Vel = P->GetVelocity();

	switch (JumpPhase)
	{
	case 1: // first jump held
		if (Now - JumpPhaseTime > 0.2f)
		{
			P->StopJumping();
			JumpPhase = 2;
		}
		break;
	case 2: // airborne: double jump near the apex if we still need height / are over a hole
		if (bAllowDoubleJump && CMC && CMC->IsFalling() && Vel.Z < 80.f)
		{
			const bool bNeedHeight = bJumpLink && MoveTarget.Z > Loc.Z - 30.f;
			const bool bOverHole = !HasFloorAt(Loc, 250.f);
			if (bNeedHeight || bOverHole || bApexDoubleJump)
			{
				P->Jump();
				JumpPhase = 3;
				JumpPhaseTime = Now;
			}
			else
			{
				JumpPhase = 4;
			}
		}
		break;
	case 3:
		if (Now - JumpPhaseTime > 0.15f)
		{
			P->StopJumping();
			JumpPhase = 4;
		}
		break;
	default:
		break;
	}

	// Landed: ready for the next jump
	if (JumpPhase != 1 && JumpPhase != 3 && CMC && CMC->IsMovingOnGround() && Now - JumpPhaseTime > 0.25f)
	{
		P->StopJumping();
		JumpPhase = 0;
		bApexDoubleJump = false;
	}
	// Safety: never get stuck in a jump phase
	if (Now - JumpPhaseTime > 3.f)
	{
		P->StopJumping();
		JumpPhase = 0;
		bApexDoubleJump = false;
	}
}

float AArenaBotController::GetPreferredDistance(const AArenaWeapon* Weapon) const
{
	if (!Weapon)
	{
		return 1000.f;
	}
	const FArenaFireMode& FM = Weapon->FireModes[0];
	switch (FM.FireType)
	{
	case EArenaFireType::Melee:
		return 0.f;
	case EArenaFireType::Beam:
		return FMath::Min(FM.Range * 0.6f, 800.f);
	case EArenaFireType::Projectile:
		if (FM.Projectile.DamageRadius > 0.f)
		{
			return FMath::Max(900.f, FM.Projectile.DamageRadius * 3.f);
		}
		if (FM.Projectile.bShard || FM.NumShots > 1)
		{
			return 500.f;
		}
		return 900.f;
	case EArenaFireType::Hitscan:
		if (FM.HeadshotMultiplier > 0.f)
		{
			return 2500.f;
		}
		if (FM.Spread > 2.f || FM.NumShots > 1)
		{
			return 600.f;
		}
		return 1200.f;
	default:
		return 1000.f;
	}
}

bool AArenaBotController::UpdateLiftRide(const FVector& Loc, FVector& InOutMoveTarget)
{
	// the lift whose bottom stop is the path point we just reached
	AArenaMover* Lift = nullptr;
	float Best = FMath::Square(400.f);
	for (TActorIterator<AArenaMover> It(GetWorld()); It; ++It)
	{
		const float D = FVector::DistSquared2D(It->GetKeyLocation(0), PrevPathPoint);
		if (D < Best)
		{
			Best = D;
			Lift = *It;
		}
	}
	const ACharacter* C = Cast<ACharacter>(GetPawn());
	if (!Lift || !C)
	{
		return false;
	}
	const bool bOnLift = C->GetMovementBaseObject() == Lift->Mesh;
	if (bOnLift)
	{
		// ride: stand still until the lift is up, then walk off towards the exit
		if (!Lift->IsOpen())
		{
			InOutMoveTarget = Lift->GetKeyLocation(Lift->KeyNum);
			return FVector::Dist2D(Loc, Lift->GetKeyLocation(0)) < 90.f;
		}
		return false;
	}
	// not on it: walk onto it when it is down, otherwise wait next to the shaft
	InOutMoveTarget = PrevPathPoint;
	if (!Lift->IsClosed())
	{
		return FVector::Dist2D(Loc, PrevPathPoint) < 260.f && Loc.Z < Lift->GetKeyLocation(0).Z + 150.f;
	}
	return false;
}

bool AArenaBotController::IsEnemyAimingAtUs() const
{
	const APawn* P = GetPawn();
	if (!Enemy || !P)
	{
		return false;
	}
	const AController* EC = Enemy->GetController();
	const AArenaWeapon* EW = Enemy->GetCurrentWeapon();
	if (!EC || !EW || !(EW->IsFiring(0) || EW->IsFiring(1)))
	{
		return false;
	}
	const FVector ToUs = (P->GetActorLocation() - Enemy->GetPawnViewLocation()).GetSafeNormal();
	return FVector::DotProduct(EC->GetControlRotation().Vector(), ToUs) > 0.985f;
}

void AArenaBotController::UpdateMovement(float DeltaSeconds)
{
	using namespace ArenaBotTuning;

	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	const FVector Loc = P->GetActorLocation();
	const UCharacterMovementComponent* CMC = P->GetCharacterMovement();
	const bool bOnGround = CMC && CMC->IsMovingOnGround();

	// ---- jump pad flight: air control towards the pad's target until we land (UE2 JumpPad.PostTouch) ----
	if (PadFlightStart >= 0.f)
	{
		const float FlightTime = Now - PadFlightStart;
		if ((bOnGround && FlightTime > 0.3f) || FlightTime > 6.f)
		{
			PadFlightStart = -1.f;
			if (FCString::Strifind(FCommandLine::Get(), TEXT("-BotDebug=")))
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("BotDebug: %s landed %.0f from the jump target (%.0f up) after %.1f s, max height %.0f"), *GetName(),
					FVector::Dist2D(Loc, PadFlightTarget), Loc.Z - PadFlightTarget.Z, FlightTime, PadFlightMaxZ - PadFlightTarget.Z);
			}
		}
		else
		{
			PadFlightMaxZ = FMath::Max(PadFlightMaxZ, Loc.Z);
			// a direct move set in flight (e.g. a Bombing Run carrier aiming for the ring) wins over the pad target
			const FVector Target = Now < DirectMoveUntil ? DirectMoveTarget : PadFlightTarget;
			const FVector ToTarget = Target - Loc;
			// air control: the horizontal velocity that arrives over the target when the fall reaches its height
			const FVector Vel = P->GetVelocity();
			const float G = CMC ? FMath::Max(-CMC->GetGravityZ(), 1.f) : 1900.f;
			const float Disc = Vel.Z * Vel.Z - 2.f * G * (ToTarget.Z);
			FVector Steer = ToTarget.GetSafeNormal2D();
			float Strength = ToTarget.Size2D() > 60.f ? 1.f : 0.f;
			if (Disc > 0.f)
			{
				const float TimeLeft = FMath::Max((Vel.Z + FMath::Sqrt(Disc)) / G, 0.15f);
				const FVector Wanted = FVector(ToTarget.X, ToTarget.Y, 0.f) / TimeLeft;
				const FVector Diff = Wanted - FVector(Vel.X, Vel.Y, 0.f);
				Steer = Diff.GetSafeNormal2D();
				Strength = FMath::Clamp(Diff.Size2D() / 150.f, 0.f, 1.f);
			}
			// a failed flight (bumped into something, falling back well below the target): drift towards the target so
			// we don't land on the pad again
			if (Vel.Z < 0.f && Loc.Z < PadFlightTarget.Z - 300.f && PadFlightMaxZ < PadFlightTarget.Z - 100.f && ToTarget.Size2D() > 60.f)
			{
				Steer = ToTarget.GetSafeNormal2D();
				Strength = 1.f;
			}
			// a ceiling over the rising pawn (BR-Colossus' pit pads launch into narrow shafts): move to where the way up is
			// open, preferring the pad's centre and then the side of the target
			if (Vel.Z > 100.f && Loc.Z < Target.Z)
			{
				FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBotPadCeiling), false, P);
				const FCollisionShape Shape = FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
				const float Rise = FMath::Min(Vel.Z * 0.4f, 500.f);
				auto OpenAbove = [&](const FVector& At) { return !GetWorld()->SweepTestByChannel(At, At + FVector(0.f, 0.f, Rise), FQuat::Identity, ECC_Pawn, Shape, Query); };
				if (!OpenAbove(Loc))
				{
					FVector Best = Loc;
					float BestScore = BIG_NUMBER;
					TArray<FVector, TInlineAllocator<10>> Cands;
					Cands.Add(FVector(PadLaunchLoc.X, PadLaunchLoc.Y, Loc.Z));
					for (int32 k = 0; k < 8; ++k)
					{
						Cands.Add(Loc + FRotator(0.f, 45.f * k, 0.f).Vector() * 70.f);
					}
					for (const FVector& C : Cands)
					{
						const float Score = FVector::Dist2D(C, Loc) - 0.2f * FVector::DotProduct((C - Loc).GetSafeNormal2D(), ToTarget.GetSafeNormal2D()) * 70.f;
						if (Score < BestScore && OpenAbove(C))
						{
							BestScore = Score;
							Best = C;
						}
					}
					const FVector Want = (Best - Loc) / 0.25f;
					const FVector Diff = FVector(Want.X - Vel.X, Want.Y - Vel.Y, 0.f);
					Steer = Diff.GetSafeNormal2D();
					Strength = FMath::Clamp(Diff.Size2D() / 100.f, 0.f, 1.f);
				}
			}
			if (Strength > 0.05f)
			{
				P->AddMovementInput(Steer, Strength);
				LastMoveDir = Steer;
			}
			if (!Enemy || !bEnemyVisible)
			{
				DesiredRotation = FRotator(0.f, ToTarget.Rotation().Yaw, 0.f);
			}
			LastPosition = Loc;
			StuckTime = 0.f;
			return;
		}
	}

	// ---- direct move (SetDirectMove): straight at the target, jumping when asked ----
	if (Now < DirectMoveUntil)
	{
		const FVector Dir = (DirectMoveTarget - Loc).GetSafeNormal2D();
		if (bDirectMoveJump && bOnGround && JumpPhase == 0)
		{
			StartJump(true);
			bApexDoubleJump = DirectMoveTarget.Z > Loc.Z - 40.f;
			bDirectMoveJump = false;
		}
		UpdateJumpState(DirectMoveTarget, true);
		if (!Dir.IsNearlyZero())
		{
			P->AddMovementInput(Dir, 1.f);
			LastMoveDir = Dir;
		}
		if (!Enemy || !bEnemyVisible)
		{
			DesiredRotation = (DirectMoveTarget - P->GetPawnViewLocation()).Rotation();
			DesiredRotation.Roll = 0.f;
		}
		LastPosition = Loc;
		StuckTime = 0.f;
		return;
	}

	// ---- translocator (Bot.FaceActor hops, JumpSpot.TryTranslocator on Translocate links): holds the bot while it prepares ----
	if (UpdateTranslocator(DeltaSeconds, Loc))
	{
		LastPosition = Loc;
		StuckTime = 0.f;
		return;
	}

	// ---- path maintenance (throttled) ----
	const bool bGoalMoved = FVector::DistSquared(GoalLocation, PathGoal) > FMath::Square(250.f);
	const bool bNeedPath = (Path.Num() == 0 && !bPathFailed) || bGoalMoved || Now - PathTime > 5.f || (bPathFailed && Now - PathTime > 2.f);
	if (GoalKind != EArenaBotGoal::None && bNeedPath && Now >= NextRepathTime && (bOnGround || Path.Num() == 0))
	{
		Repath();
	}

	// ---- advance along the path ----
	FVector MoveTarget = GoalLocation;
	EArenaNavLinkType Link = EArenaNavLinkType::Walk;
	bool bOnPath = false;
	bool bWaitForLift = false;
	bool bPathAdvanced = false;
	while (Path.Num() > 0)
	{
		const FVector& Pt = Path[0];
		const EArenaNavLinkType PtLink = PathLinks[0];
		const float D2 = FVector::Dist2D(Loc, Pt);
		const float Dz = Loc.Z - Pt.Z;
		bool bReachedPt = false;
		switch (PtLink)
		{
		case EArenaNavLinkType::Jump:
			bReachedPt = D2 < ReachRadius && Dz > -50.f && Dz < 150.f;
			break;
		case EArenaNavLinkType::JumpPad:
			bReachedPt = D2 < ReachRadius * 1.5f && FMath::Abs(Dz) < 150.f;
			break;
		case EArenaNavLinkType::Lift:
			bReachedPt = D2 < ReachRadius * 2.f && Dz > -70.f && Dz < 150.f;
			break;
		case EArenaNavLinkType::Teleport:
			bReachedPt = D2 < ReachRadius * 2.f && FMath::Abs(Dz) < 200.f;
			break;
		case EArenaNavLinkType::ForcedJump:
		case EArenaNavLinkType::Translocate:
			bReachedPt = D2 < ReachRadius * 1.5f && FMath::Abs(Dz) < 150.f;
			break;
		default:
			bReachedPt = D2 < ReachRadius && FMath::Abs(Dz) < 120.f;
			break;
		}
		if (!bReachedPt)
		{
			break;
		}
		PrevPathPoint = Pt;
		Path.RemoveAt(0);
		PathLinks.RemoveAt(0);
		StuckCount = 0;
		bPathAdvanced = true;
	}
	if (Path.Num() > 0)
	{
		MoveTarget = Path[0];
		Link = PathLinks[0];
		bOnPath = true;
		if (Link == EArenaNavLinkType::JumpPad && bOnGround)
		{
			// Step right onto the pad until it launches us. Like a player (and UE2's path into the pad) come in from the
			// side of its target: the trigger fires at the pad's edge and the flight starts clear of anything that
			// overhangs the far side (BR-Colossus' pit pads)
			MoveTarget = PrevPathPoint;
			const FVector TargetDir = (Path[0] - PrevPathPoint).GetSafeNormal2D();
			const FVector FromPad = Loc - PrevPathPoint;
			if (!TargetDir.IsNearlyZero() && FromPad.Size2D() > 120.f && FVector::DotProduct(FromPad.GetSafeNormal2D(), TargetDir) < 0.3f
				&& FMath::Abs(FromPad.Z) < 150.f)
			{
				const FVector Lead = PrevPathPoint + TargetDir * 220.f;
				if (HasFloorAt(Lead, 150.f) && !IsBlockedAhead(Lead - Loc, FMath::Min(FVector::Dist2D(Lead, Loc), 300.f)))
				{
					MoveTarget = Lead;
				}
			}
		}
		else if (Link == EArenaNavLinkType::Teleport || Link == EArenaNavLinkType::Translocate)
		{
			// walk into the teleporter / to the spot the translocator throw starts from (UpdateTranslocator throws there)
			MoveTarget = PrevPathPoint;
		}
		else if (Link == EArenaNavLinkType::ForcedJump && bOnGround && JumpPhase == 0)
		{
			// UE2 JumpDest.DoJump: from the takeoff node, leave with the velocity that lands on the JumpDest
			if (FVector::Dist2D(Loc, PrevPathPoint) > 90.f)
			{
				MoveTarget = PrevPathPoint;
			}
			else
			{
				const float G = CMC ? -CMC->GetGravityZ() : 1900.f;
				const float D = FVector::Dist2D(Loc, Path[0]);
				const float Dz = Path[0].Z - Loc.Z;
				float Speed = ArenaTuning::DodgeSpeed * 1.05f;
				for (float S = ArenaTuning::GroundSpeed; S <= ArenaTuning::DodgeSpeed * 1.05f; S += 50.f)
				{
					const float T = D / S;
					if ((Dz + 0.5f * G * T * T) / T <= 1100.f)
					{
						Speed = S;
						break;
					}
				}
				const float T = FMath::Max(D / Speed, 0.1f);
				const FVector V = (Path[0] - Loc).GetSafeNormal2D() * Speed + FVector(0.f, 0.f, (Dz + 0.5f * G * T * T) / T);
				P->LaunchCharacter(V, true, true);
				NotifyJumpPadLaunch(Path[0]);
				NumForcedJumps++;
				return;
			}
		}
		else if (Link == EArenaNavLinkType::Lift)
		{
			bWaitForLift = UpdateLiftRide(Loc, MoveTarget);
		}
		else if (bPathAdvanced && Link == EArenaNavLinkType::Walk && bOnGround)
		{
			// UE2 (native MoveToward / NotifyPostLanded): with DodgeToGoalPct, dodge towards a far path node. Our nav
			// graph is denser than UE2's path nodes: use the furthest node straight ahead (same direction, same height).
			FVector Far = MoveTarget;
			const FVector Dir0 = (MoveTarget - Loc).GetSafeNormal2D();
			for (int32 i = 1; i < Path.Num() && i < 8; ++i)
			{
				if (PathLinks[i] != EArenaNavLinkType::Walk || FMath::Abs(Path[i].Z - Loc.Z) > 70.f
					|| FVector::DotProduct((Path[i] - Loc).GetSafeNormal2D(), Dir0) < 0.97f)
				{
					break;
				}
				Far = Path[i];
			}
			MayDodgeToMoveTarget(Far);
		}
	}
	else if (!bPathFailed && PathTime > 0.f && GoalKind == EArenaBotGoal::Wander)
	{
		// Finished a wander path
		NextGoalTime = 0.f;
	}
	CurMoveTarget = MoveTarget;
	bPathFacing = false;

	FVector Dir = (MoveTarget - Loc).GetSafeNormal2D();
	if (bOnPath && Link == EArenaNavLinkType::Lift && bWaitForLift)
	{
		Dir = FVector::ZeroVector;
	}
	if (!bOnPath)
	{
		// No graph / no path: steer directly with simple obstacle avoidance
		Dir = AvoidObstacles(Dir, true);
	}

	// ---- combat manoeuvres ----
	const bool bFighting = Enemy && bEnemyVisible;
	if (bFighting)
	{
		const FVector ToEnemy = Enemy->GetActorLocation() - Loc;
		const float EnemyDist = ToEnemy.Size2D();
		const FVector ToEnemyDir = ToEnemy.GetSafeNormal2D();
		bool bNewTacticalMove = false;
		if (Now >= NextStrafeChange)
		{
			StrafeDir = Rng.FRand() < 0.5f ? -1.f : 1.f;
			NextStrafeChange = Now + Rng.FRandRange(0.4f, FMath::Max(0.8f, 1.8f - Skill * 0.12f));
			bNewTacticalMove = true;
		}
		FVector Strafe = FVector::CrossProduct(FVector::UpVector, ToEnemyDir) * StrafeDir;
		if (bOnGround && (IsBlockedAhead(Strafe, 80.f) || !HasFloorAt(Loc + Strafe * 120.f, 300.f) || !IsSafeMove(Strafe, 300.f)))
		{
			StrafeDir = -StrafeDir;
			Strafe = -Strafe;
		}

		const bool bImportantGoal = GoalScore >= 0.9f && GoalKind != EArenaBotGoal::Hunt;
		// a drop or jump link of the path is taken at full speed even in a fight: braking or strafing half way over a gap
		// is a fall
		const bool bCommitted = bOnPath && (Link == EArenaNavLinkType::Drop || Link == EArenaNavLinkType::Jump);
		bPathFacing = (bImportantGoal || bCommitted) && bOnPath;
		if (bImportantGoal || bCommitted)
		{
			// UE2 strafing on the way to an objective: the bot keeps moving to its MoveTarget at full speed and only
			// faces (and shoots at) the enemy (Bot.FaceActor); no sideways drift that slows a flag / ball carrier
		}
		else
		{
			// personality: CombatStyle and Aggressiveness move the preferred range; a hurt, cautious bot backs off
			const float Pref = GetPreferredDistance(P->GetCurrentWeapon()) * FMath::Clamp(1.f - 0.3f * CombatStyle - 0.15f * Aggressiveness, 0.4f, 1.6f);
			float Approach = 0.f;
			if (P->Health < 40.f && Aggressiveness + 0.5f * CombatStyle < 0.3f)
			{
				Approach = -0.8f;
			}
			else if (EnemyDist > Pref + 250.f)
			{
				Approach = 1.f;
			}
			else if (EnemyDist < Pref - 250.f)
			{
				Approach = -0.8f;
			}
			Dir = AvoidObstacles(ToEnemyDir * Approach + Strafe, true);
		}

		// Dodging shots is driven by UE2's warnings (ReceiveWarning / ReceiveProjectileWarning -> TryToDuck).
		// Each new tactical move (UE2 FightEnemy / TacticalMove.EngageDirection):
		if (bNewTacticalMove && bOnGround && JumpPhase == 0)
		{
			AArenaWeapon* CurW = P->GetCurrentWeapon();
			const bool bSplashWeapon = CurW && (CurW->FireModes[0].bRecommendSplash || CurW->FireModes[1].bRecommendSplash);
			if (!bSplashWeapon && Rng.FRand() < 0.7f && 3.f * Jumpiness + Rng.FRand() * Skill > 3.f)
			{
				// FightEnemy: "Try to Duck" to a random side
				const FVector Y = FRotationMatrix(P->GetActorRotation()).GetUnitAxis(EAxis::Y);
				TryToDuck(Rng.FRand() < 0.5f ? -Y : Y, Rng.FRand() < 0.5f);
			}
			else if (Rng.FRand() < 0.05f * Skill + 0.6f * Jumpiness
				&& Enemy->GetActorLocation().Z - Enemy->GetSimpleCollisionHalfHeight() <= Loc.Z + 70.f - P->GetSimpleCollisionHalfHeight()
				&& IsSafeMove(Strafe, ArenaBotTuning::HopCheckDist))
			{
				// EngageDirection bWantJump: a strafe jump, double jumping at the apex (bTacticalDoubleJump)
				StartJump(Skill >= 3.f);
				bApexDoubleJump = Skill >= 3.f;
				NumTacticalJumps++;
			}
		}
	}

	// ---- path link handling ----
	if (bOnPath && Link == EArenaNavLinkType::Jump && bOnGround && JumpPhase == 0)
	{
		const float D2 = FVector::Dist2D(Loc, MoveTarget);
		const bool bNeedUp = MoveTarget.Z > Loc.Z + ArenaTuning::MaxStepHeight;
		const FVector Ahead = Loc + (MoveTarget - Loc).GetSafeNormal2D() * 70.f;
		const bool bEdge = !HasFloorAt(Ahead, 100.f);
		const bool bWall = IsBlockedAhead(MoveTarget - Loc, 60.f);
		if ((bNeedUp && D2 < 260.f) || bEdge || bWall || D2 < 150.f)
		{
			StartJump(true);
		}
	}
	UpdateJumpState(MoveTarget, bOnPath && Link == EArenaNavLinkType::Jump);

	// ---- void safety ----
	if (bOnGround)
	{
		// never run into the void; deliberate jump links over gaps are left to the jump logic, and drop links (the nav
		// graph only links drops onto floors it found, out of hazard volumes) may be longer than the void check looks
		const bool bJumpingGap = bOnPath && (Link == EArenaNavLinkType::Jump || Link == EArenaNavLinkType::JumpPad
			|| Link == EArenaNavLinkType::Drop);
		if (!bJumpingGap && !Dir.IsNearlyZero())
		{
			Dir = GuardEdge(Dir);
		}
		if (HasSafeFloorAt(Loc, 200.f))
		{
			LastSafeGround = Loc;
			bHasSafeGround = true;
		}
	}
	else if (bHasSafeGround && (!bOnPath || Link == EArenaNavLinkType::Walk) && !HasSafeFloorAt(Loc, 100000.f))
	{
		// Airborne over the void (knocked off, or a hop went wrong): steer back and use the double jump
		Dir = (LastSafeGround - Loc).GetSafeNormal2D();
		if ((JumpPhase == 0 || JumpPhase == 2 || JumpPhase == 4) && P->CanJump() && P->GetVelocity().Z < 100.f)
		{
			P->Jump();
			JumpPhase = 3;
			JumpPhaseTime = Now;
		}
	}

	// ---- jump pads the path doesn't use: walk around them (UE2 bots touch pads only on their way through) ----
	const AArenaGameMode* PadGM = World->GetAuthGameMode<AArenaGameMode>();
	if (bOnGround && !Dir.IsNearlyZero() && !IsFollowingPadLink() && PadGM && PadGM->GetLevelBuilder())
	{
		const float R = P->GetSimpleCollisionRadius();
		const float H = P->GetSimpleCollisionHalfHeight();
		for (const AArenaJumpPad* Pad : PadGM->GetLevelBuilder()->GetJumpPads())
		{
			if (!Pad || FVector::DistSquared2D(Pad->GetTriggerCenter(), Loc) > FMath::Square(Pad->GetTriggerRadius() + 400.f))
			{
				continue;
			}
			const FVector Ahead = Loc + Dir.GetSafeNormal2D() * 120.f;
			const FVector PadD = Ahead - Pad->GetTriggerCenter();
			if (FVector2D(PadD.X, PadD.Y).Size() <= Pad->GetTriggerRadius() + R + 15.f && FMath::Abs(PadD.Z) <= Pad->GetTriggerHalfHeight() + H)
			{
				// sidestep: the perpendicular that leads away from the pad's centre
				const FVector Away = (Loc - Pad->GetTriggerCenter()).GetSafeNormal2D();
				FVector Side = FVector::CrossProduct(FVector::UpVector, Dir).GetSafeNormal2D();
				if (FVector::DotProduct(Side, Away) < 0.f)
				{
					Side = -Side;
				}
				Dir = (Side + Away * 0.5f).GetSafeNormal2D();
				break;
			}
		}
	}

	// ---- UE2 Pawn.bStopAtLedges: walking bots stop at ledges unless the path takes them off on purpose ----
	if (UArenaCharacterMovement* AMC = Cast<UArenaCharacterMovement>(P->GetCharacterMovement()))
	{
		const bool bLeaveLedge = bOnPath && (Link == EArenaNavLinkType::Drop || Link == EArenaNavLinkType::Jump || Link == EArenaNavLinkType::JumpPad
			|| Link == EArenaNavLinkType::ForcedJump || Link == EArenaNavLinkType::Lift || Link == EArenaNavLinkType::Teleport);
		AMC->bStopAtLedges = !bLeaveLedge && !bPathFailed && GoalKind != EArenaBotGoal::None;
	}

	// ---- swimming: straight at the path point in 3D (UE2 bots swim to their MoveTarget; flooded tunnels) ----
	if (CMC && CMC->IsSwimming() && bOnPath)
	{
		const FVector Swim = (MoveTarget - Loc).GetSafeNormal();
		if (!Swim.IsNearlyZero())
		{
			Dir = Swim;
		}
	}

	// ---- apply movement ----
	if (!Dir.IsNearlyZero())
	{
		P->AddMovementInput(Dir, 1.f);
		LastMoveDir = Dir;
	}

	// ---- look direction when not fighting ----
	if ((!Enemy || (!bEnemyVisible && Now - LastSeenEnemyTime > 1.5f)) && Now >= LookAtUntil && !Dir.IsNearlyZero())
	{
		FRotator R = Dir.Rotation();
		// Look slightly up/down towards the next waypoint
		const FVector ToTarget = MoveTarget - P->GetPawnViewLocation();
		R.Pitch = FMath::Clamp(ToTarget.Rotation().Pitch * 0.5f, -30.f, 30.f);
		R.Roll = 0.f;
		DesiredRotation = R;
	}

	// ---- stuck detection ----
	const bool bWantsMove = !Dir.IsNearlyZero() && !(bFighting && GoalScore < 0.9f);
	// by the distance moved: velocity alone can stay high while pushing into a ledge
	if (bWantsMove && bOnGround && FVector::Dist2D(Loc, LastPosition) < StuckSpeed * DeltaSeconds + 1.f)
	{
		StuckTime += DeltaSeconds;
	}
	else
	{
		StuckTime = FMath::Max(0.f, StuckTime - DeltaSeconds * 0.5f);
	}
	LastPosition = Loc;

	if (StuckTime > StuckTimeLimit)
	{
		StuckTime = 0.f;
		++StuckCount;
		// a stuck bot jumps, but not off a ledge it stopped at (Pawn.bStopAtLedges) into the void
		const bool bSafeHop = IsSafeMove(Dir, ArenaBotTuning::HopCheckDist);
		if (StuckCount % 2 == 1)
		{
			if (bSafeHop)
			{
				StartJump(true);
			}
		}
		else
		{
			FVector Side = FVector::CrossProduct(FVector::UpVector, Dir) * (Rng.FRand() < 0.5f ? -1.f : 1.f);
			if (IsBlockedAhead(Side, 150.f))
			{
				Side = -Side;
			}
			const FVector DodgeDir = (Side - Dir * 0.5f).GetSafeNormal2D();
			if (IsSafeMove(DodgeDir, ArenaBotTuning::DodgeCheckDist))
			{
				P->DodgeWorld(DodgeDir);
			}
			else if (bSafeHop)
			{
				StartJump(false);
			}
		}
		// Repath from where we are now
		Path.Reset();
		PathLinks.Reset();
		bPathFailed = false;
		NextRepathTime = Now + 0.25f;
		if (StuckCount >= 4)
		{
			// Give up on this goal
			IgnoredGoalActor = GoalActor;
			IgnoreGoalUntil = Now + 8.f;
			GoalKind = EArenaBotGoal::None;
			GoalActor.Reset();
			NextGoalTime = 0.f;
			StuckCount = 0;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------------------------

void AArenaBotController::ChooseWeapon()
{
	AArenaCharacter* P = GetArenaPawn();
	if (!P)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	NextWeaponSwitchTime = Now + ArenaBotTuning::WeaponSwitchInterval;

	const float Dist = Enemy ? FVector::Dist(P->GetActorLocation(), Enemy->GetActorLocation()) : 1500.f;
	AArenaWeapon* Current = P->GetCurrentWeapon();
	AArenaWeapon* Best = nullptr;
	float BestRating = -TNumericLimits<float>::Max();
	for (AArenaWeapon* W : P->GetWeapons())
	{
		if (!W || !W->HasAmmo())
		{
			continue;
		}
		float Rating = W->GetBotRating(Dist);
		// a splash weapon whose blast would reach us (the warhead launcher up close) can't be fired at this enemy
		const FArenaFireMode& FM0 = W->FireModes[W->GetBotFireMode(Dist) > 0 ? 1 : 0];
		if (Enemy && FM0.FireType == EArenaFireType::Projectile && FM0.Projectile.DamageRadius > 0.f && Dist < FM0.Projectile.DamageRadius + 60.f)
		{
			Rating *= 0.05f;
		}
		if (W == Current)
		{
			Rating += 0.05f;
		}
		if (IsFavoriteWeapon(W))
		{
			Rating += 0.1f;
		}
		if (Rating > BestRating)
		{
			BestRating = Rating;
			Best = W;
		}
	}
	if (Best && Best != Current)
	{
		StopFiring();
		P->SwitchToWeapon(Best);
	}
}

void AArenaBotController::StartFiring(uint8 Mode)
{
	AArenaCharacter* P = GetArenaPawn();
	if (!P)
	{
		return;
	}
	if (bFiring && FiringMode == Mode)
	{
		return;
	}
	if (bFiring)
	{
		StopFiring();
	}
	P->StartFire(Mode);
	bFiring = true;
	FiringMode = Mode;
	FireStartTime = GetWorld()->GetTimeSeconds();
}

void AArenaBotController::StopFiring()
{
	if (bFiring)
	{
		if (AArenaCharacter* P = GetArenaPawn())
		{
			P->StopFire(FiringMode);
		}
	}
	bFiring = false;
}

FVector AArenaBotController::ComputeAimPoint(const AArenaWeapon* Weapon, uint8 Mode, float Distance) const
{
	if (!Enemy)
	{
		return LastSeenEnemyLocation;
	}
	const FVector EnemyLoc = Enemy->GetActorLocation();
	FVector Aim = EnemyLoc;
	if (!Weapon || Mode > 1)
	{
		return Aim;
	}

	const FArenaFireMode& FM = Weapon->FireModes[Mode];
	const UCharacterMovementComponent* ECMC = Enemy->GetCharacterMovement();
	const bool bGrounded = ECMC && ECMC->IsMovingOnGround();

	if ((FM.FireType == EArenaFireType::Hitscan) && FM.HeadshotMultiplier > 0.f && bAimHead)
	{
		Aim = Enemy->GetHeadLocation();
	}

	const float Speed = Weapon->GetProjectileSpeed(Mode);
	if (FM.FireType == EArenaFireType::Projectile && Speed > 0.f)
	{
		const bool bSplash = FM.Projectile.DamageRadius > 0.f;
		if (bSplash && bGrounded && Skill >= 2.f)
		{
			// Aim at the feet so splash damage still connects on a near miss
			Aim.Z = EnemyLoc.Z - Enemy->GetSimpleCollisionHalfHeight() + 15.f;
		}
		FVector Vel = Enemy->GetVelocity();
		if (bGrounded)
		{
			Vel.Z = 0.f;
		}
		// Bot.ResetSkill: bLeadTarget = Skill >= 4
		const float LeadSkill = Skill >= 4.f ? 1.f : 0.f;
		const float T = Distance / Speed;
		Aim += Vel * T * LeadSkill;
		if (FM.Projectile.GravityScale > 0.f)
		{
			const float G = -GetWorld()->GetGravityZ() * FM.Projectile.GravityScale;
			Aim.Z += 0.5f * G * T * T;
		}
	}
	return Aim;
}

void AArenaBotController::UpdateAimAndFire(float DeltaSeconds)
{
	AArenaCharacter* P = GetArenaPawn();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	ReactionTimer = FMath::Max(0.f, ReactionTimer - DeltaSeconds);

	// UE2 Focus / FireWeaponAt on a non-pawn target (an incoming warhead) while no enemy is in sight
	if (UpdateShootTarget(DeltaSeconds))
	{
		return;
	}

	if (!Enemy || !IsValid(Enemy) || Enemy->IsDead())
	{
		StopFiring();
		AArenaWeapon* W = P->GetCurrentWeapon();
		if (Now >= NextWeaponSwitchTime || !W || !W->HasAmmo())
		{
			ChooseWeapon();
		}
		return;
	}

	AArenaWeapon* W = P->GetCurrentWeapon();
	if (Now >= NextWeaponSwitchTime || !W || !W->HasAmmo())
	{
		ChooseWeapon();
		W = P->GetCurrentWeapon();
	}

	const FVector Eye = P->GetPawnViewLocation();
	const float Dist = FVector::Dist(Eye, Enemy->GetActorLocation());

	// Pick the fire mode
	uint8 Mode = 0;
	if (W)
	{
		Mode = FMath::Min<uint8>(W->GetBotFireMode(Dist), 1);
		auto IsUsable = [W](uint8 M)
		{
			const EArenaFireType T = W->FireModes[M].FireType;
			return T != EArenaFireType::Zoom && T != EArenaFireType::Shield && W->HasAmmoFor(M);
		};
		const uint8 OtherMode = static_cast<uint8>(1 - Mode);
		if (!IsUsable(Mode) && IsUsable(OtherMode))
		{
			Mode = OtherMode;
		}
	}

	// The view tracks the target; the aim error is UE2's, applied to each shot (Bot.AdjustAim / AdjustAimError).
	AimError = FVector::ZeroVector;
	bAimHead = false;
	// ShockRifle.BestMode: set up a shock combo (core first, then the beam when it reaches the enemy)
	if (W && Cast<AWeap_PulseRifle>(W) && !bWaitForCombo && !bComboTriggered && (Now >= NextShockModeTime || !bFiring))
	{
		if (Now >= NextShockModeTime)
		{
			NextShockModeTime = Now + GetCombatTimerPeriod();
			bRegisterComboTarget = false;
			const AArenaCharacter* Me = P;
			const bool bStopped = bStoppedUE2;
			const UCharacterMovementComponent* MyCM = Me->GetCharacterMovement();
			// Bot.CanCombo: stopped, or not falling and (not moving or moving at the enemy) or CanComboMoving
			bool bCanCombo = bStopped;
			if (!bCanCombo && MyCM && !MyCM->IsFalling())
			{
				bCanCombo = MyCM->GetCurrentAcceleration().IsNearlyZero() || (Skill >= 7.f ? Rng.FRand() < 0.9f : Skill - 3.f > 4.f * Rng.FRand());
			}
			// Bot.ProficientWithWeapon: skill + 2 with the favourite weapon
			const bool bProficient = Skill + (IsFavoriteWeapon(W) ? 2.f : 0.f) > 2.f + Rng.FRand() * 4.f;
			const float MaxDist = (Skill > 5.f ? 4.f : 3.f) * FMath::Max(1.f, W->GetProjectileSpeed(1));
			ShockMode = 0;
			if (!bEnemyVisible)
			{
				if (bCanCombo && bProficient)
				{
					bRegisterComboTarget = true;
					ShockMode = 1;
				}
			}
			else if (Dist > MaxDist || Dist < 300.f)
			{
				ShockMode = 0;
			}
			else if (Dist > 5000.f && Rng.FRand() < 0.5f)
			{
				ShockMode = 0;
			}
			else if (bCanCombo && bProficient)
			{
				bRegisterComboTarget = true;
				ShockMode = 1;
			}
			else
			{
				ShockMode = Rng.FRand() < 0.7f ? 0 : 1;
			}
		}
		Mode = W->HasAmmoFor(ShockMode) ? ShockMode : Mode;
	}

	if (!bEnemyVisible)
	{
		// Keep looking where we last saw (or heard) them for a moment, then along the way again
		StopFiring();
		if (Now - LastSeenEnemyTime < 1.5f && Now >= LookAtUntil)
		{
			DesiredRotation = (LastSeenEnemyLocation - Eye).Rotation();
			DesiredRotation.Roll = 0.f;
		}
		return;
	}

	FVector AimPoint = ComputeAimPoint(W, Mode, Dist) + AimError;
	if (bPathFacing && !ShouldFaceEnemyUE2(CurMoveTarget))
	{
		// Bot.FaceActor chose the path: look where we go (shots only when the enemy happens to be in front)
		AimPoint = Eye + (CurMoveTarget - P->GetActorLocation()).GetSafeNormal2D() * 1000.f;
	}
	DesiredRotation = (AimPoint - Eye).Rotation();
	DesiredRotation.Roll = 0.f;

	if (bWaitForCombo || bComboTriggered)
	{
		// ShockRifle.StartFire: a bot waiting for its combo doesn't fire anything else
		if (!bComboTriggered)
		{
			StopFiring();
		}
		return;
	}

	if (!W || ReactionTimer > 0.f || !W->HasAmmoFor(Mode))
	{
		StopFiring();
		return;
	}

	const FArenaFireMode& FM = W->FireModes[Mode];
	if (FM.FireType == EArenaFireType::Zoom || FM.FireType == EArenaFireType::Shield)
	{
		StopFiring();
		return;
	}

	// Is the crosshair close enough?
	const FVector AimDir = (AimPoint - Eye).GetSafeNormal();
	const FVector ViewDir = GetControlRotation().Vector();
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(AimDir, ViewDir), -1.f, 1.f)));
	float Tolerance = FMath::Max(3.f, FMath::RadiansToDegrees(FMath::Atan2(70.f, FMath::Max(Dist, 1.f))));
	if (FM.FireType == EArenaFireType::Beam || FM.FireType == EArenaFireType::Melee)
	{
		Tolerance += 10.f;
	}
	if (FM.Spread > 0.f)
	{
		Tolerance += FM.Spread;
	}
	const bool bAimOk = AngleDeg <= Tolerance;
	if (bAimOk)
	{
		LastAimOkTime = Now;
	}

	bool bInRange = Dist <= FM.Range * 1.05f;
	// Don't blow ourselves up
	if (FM.FireType == EArenaFireType::Projectile && FM.Projectile.DamageRadius > 0.f && Dist < FM.Projectile.DamageRadius + 60.f)
	{
		bInRange = false;
	}

	const bool bCharge = FM.FireType == EArenaFireType::Melee || FM.MaxChargeTime > 0.f;
	if (bCharge)
	{
		if (!bFiring || FiringMode != Mode)
		{
			// Start charging when lined up (melee: when closing in)
			const bool bStart = FM.FireType == EArenaFireType::Melee ? (Dist < FMath::Max(FM.Range, 150.f) + 600.f) : (bAimOk && bInRange);
			if (bStart)
			{
				StartFiring(Mode);
				const float MaxCharge = FMath::Max(FM.MaxChargeTime, 0.3f);
				ChargeReleaseTime = Now + MaxCharge * Rng.FRandRange(0.4f, 1.f);
			}
		}
		else
		{
			bool bRelease = false;
			if (FM.FireType == EArenaFireType::Melee)
			{
				bRelease = (Dist < FMath::Max(FM.Range, 120.f) + 40.f && bAimOk) || Now - FireStartTime > FM.MaxChargeTime + 2.f;
			}
			else
			{
				bRelease = (Now >= ChargeReleaseTime && bAimOk && bInRange) || Now - FireStartTime > FM.MaxChargeTime + 1.5f;
			}
			if (bRelease)
			{
				StopFiring();
			}
		}
		return;
	}

	// Hold modes (hitscan / projectile auto fire / beam)
	if (bAimOk && bInRange)
	{
		StartFiring(Mode);
	}
	else if (bFiring)
	{
		// Short grace period for continuous fire so jitter doesn't toggle the trigger
		const bool bContinuous = FM.FireType == EArenaFireType::Beam || FM.RefireTime < 0.25f;
		if (!bContinuous || Now - LastAimOkTime > 0.15f || !bInRange)
		{
			StopFiring();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Shoot targets (UE2 Focus = Target; FireWeaponAt(Target) on non-pawns: RedeemerProjectile.Timer)
// ---------------------------------------------------------------------------------------------

void AArenaBotController::SetShootTarget(AActor* Target, float Duration)
{
	ShootTarget = Target;
	ShootTargetUntil = Target ? GetWorld()->GetTimeSeconds() + Duration : -1.f;
}

AActor* AArenaBotController::GetShootTarget() const
{
	AActor* T = ShootTarget.Get();
	if (!T || !IsValid(T) || T->IsActorBeingDestroyed() || !GetWorld() || GetWorld()->GetTimeSeconds() > ShootTargetUntil)
	{
		return nullptr;
	}
	if (Enemy && IsValid(Enemy) && !Enemy->IsDead() && bEnemyVisible)
	{
		return nullptr;
	}
	return T;
}

bool AArenaBotController::UpdateShootTarget(float DeltaSeconds)
{
	AActor* T = GetShootTarget();
	AArenaCharacter* P = GetArenaPawn();
	AArenaWeapon* W = P ? P->GetCurrentWeapon() : nullptr;
	if (!T || !P || !W)
	{
		if (ShootTarget.IsValid() && !T)
		{
			ShootTarget.Reset();
			StopFiring();
		}
		return false;
	}
	const FVector Eye = P->GetPawnViewLocation();
	const FVector TLoc = T->GetActorLocation();
	const float Dist = FVector::Dist(Eye, TLoc);
	// a mode that can fire at it (no zoom / shield / melee / charging mode, in range)
	auto Usable = [W, Dist](uint8 M)
	{
		const FArenaFireMode& FM = W->FireModes[M];
		return FM.FireType != EArenaFireType::Zoom && FM.FireType != EArenaFireType::Shield && FM.FireType != EArenaFireType::Melee
			&& FM.MaxChargeTime <= 0.f && W->HasAmmoFor(M) && Dist <= FM.Range * 1.05f;
	};
	uint8 Mode = FMath::Min<uint8>(W->GetBotFireMode(Dist), 1);
	if (!Usable(Mode))
	{
		Mode = uint8(1 - Mode);
	}
	if (!Usable(Mode))
	{
		ShootTarget.Reset();
		StopFiring();
		return false;
	}
	// lead it with the projectile's speed (Bot.AdjustAim leads moving targets)
	FVector Aim = TLoc;
	const float Speed = W->FireModes[Mode].FireType == EArenaFireType::Projectile ? W->GetProjectileSpeed(Mode) : 0.f;
	if (Speed > 1.f)
	{
		Aim += T->GetVelocity() * (Dist / Speed);
	}
	DesiredRotation = (Aim - Eye).Rotation();
	DesiredRotation.Roll = 0.f;
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((Aim - Eye).GetSafeNormal(), GetControlRotation().Vector()), -1.f, 1.f)));
	if (AngleDeg <= FMath::Max(4.f, FMath::RadiansToDegrees(FMath::Atan2(150.f, FMath::Max(Dist, 1.f)))))
	{
		if (!bFiring || FiringMode != Mode)
		{
			++NumShootTargetShots;
		}
		StartFiring(Mode);
	}
	return true;
}

bool AArenaBotController::IsFavoriteWeapon(const AArenaWeapon* W) const
{
	return W && FavoriteWeapon && W->IsA(FavoriteWeapon);
}

bool AArenaBotController::SetFavoriteWeaponUE2(const FString& UE2Class)
{
	// UT2004 weapon classes (package prefix optional) -> ours
	FString Name = UE2Class;
	int32 Dot = INDEX_NONE;
	if (Name.FindLastChar(TEXT('.'), Dot))
	{
		Name.RightChopInline(Dot + 1);
	}
	Name.ToLowerInline();
	static const TMap<FString, UClass*> Map = {
		{ TEXT("shieldgun"), AWeap_Gauntlet::StaticClass() }, { TEXT("assaultrifle"), AWeap_Carbine::StaticClass() },
		{ TEXT("biorifle"), AWeap_GooLauncher::StaticClass() }, { TEXT("shockrifle"), AWeap_PulseRifle::StaticClass() },
		{ TEXT("linkgun"), AWeap_ArcLinker::StaticClass() }, { TEXT("minigun"), AWeap_RotaryGun::StaticClass() },
		{ TEXT("flakcannon"), AWeap_ShardCannon::StaticClass() }, { TEXT("rocketlauncher"), AWeap_RocketLauncher::StaticClass() },
		{ TEXT("sniperrifle"), AWeap_RailRifle::StaticClass() }, { TEXT("classicsniperrifle"), AWeap_MarksmanRifle::StaticClass() },
		{ TEXT("redeemer"), AWeap_WarheadLauncher::StaticClass() }, { TEXT("painter"), AWeap_OrbitalDesignator::StaticClass() },
		{ TEXT("onsavril"), AWeap_SeekerLauncher::StaticClass() }, { TEXT("onsgrenadelauncher"), AWeap_StickyLauncher::StaticClass() },
		{ TEXT("onsminelayer"), AWeap_CrawlerLayer::StaticClass() } };
	UClass* const* Found = Map.Find(Name);
	FavoriteWeapon = Found ? *Found : nullptr;
	return Found != nullptr;
}

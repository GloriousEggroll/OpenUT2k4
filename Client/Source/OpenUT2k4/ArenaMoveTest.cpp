#include "ArenaMoveTest.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaCharacterMovement.h"
#include "ArenaBotController.h"
#include "ArenaGameMode.h"
#include "ArenaNavGraph.h"
#include "ArenaProjectile.h"
#include "ArenaWeaponActors.h"
#include "ArenaTypes.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaWorldZones.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "UnrealClient.h"

namespace ArenaMoveTestLocal
{
	constexpr float G = 1900.f;
	constexpr float HalfHeight = ArenaTuning::CapsuleHalfHeight;
	constexpr float Radius = ArenaTuning::CapsuleRadius;
	/** Rig layout (relative to the floor centre). */
	constexpr float WallX = -2000.f;
	const FVector StepPos(0.f, 2500.f, 0.f);
	const FVector HighStepPos(0.f, 3300.f, 0.f);
	const FVector LiftPos(2000.f, -2500.f, 0.f);
	const FVector Ramp44Pos(0.f, -1000.f, 0.f);
	const FVector Ramp47Pos(0.f, -1800.f, 0.f);
	constexpr float LiftHeight = 40.f;

	const TCHAR* AllTests[] = {
		TEXT("jump"), TEXT("doublejump"), TEXT("doublejump_early"), TEXT("doublejump_late"), TEXT("dodge"), TEXT("dodge_lockout"),
		TEXT("dodgejump"), TEXT("dodge_land_jump"), TEXT("walldodge"), TEXT("walldodge_far"), TEXT("walldodge_twice"),
		TEXT("walldodge_after_doublejump"), TEXT("aircontrol"), TEXT("strafejump"), TEXT("bunnyhop"), TEXT("dodgechain"),
		TEXT("rocketjump"), TEXT("rocketjump_jump"), TEXT("shieldjump"), TEXT("shieldjump_jump"), TEXT("lift"), TEXT("crouch"),
		TEXT("step"), TEXT("slope"), TEXT("falldamage"), TEXT("lowgrav"), TEXT("view"), TEXT("airslope"), TEXT("swim"), TEXT("drown")
	};
	/** Water pool: a water volume from the floor up to 260 cm, edge wall (top 280 cm) facing -X at PoolEdgeX. */
	const FVector PoolPos(-1000.f, -3000.f, 0.f);
	constexpr float PoolDepth = 260.f;
	constexpr float PoolEdgeX = -500.f;
}

bool UArenaMoveTestSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	FString Value;
	return FParse::Value(FCommandLine::Get(), TEXT("-MoveTest="), Value, false) && Super::ShouldCreateSubsystem(Outer);
#endif
}

bool UArenaMoveTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UArenaMoveTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaMoveTestSubsystem, STATGROUP_Tickables);
}

void UArenaMoveTestSubsystem::Tick(float DeltaTime)
{
#if !UE_BUILD_SHIPPING
	if (bDone)
	{
		return;
	}
	UWorld* World = GetWorld();
	AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	if (!GM)
	{
		return;
	}
	FString Spec;
	FParse::Value(FCommandLine::Get(), TEXT("-MoveTest="), Spec, false);
	if (Spec.Equals(TEXT("botwatch"), ESearchCase::IgnoreCase))
	{
		// Bots: count the UT2004 moves the bots make (use with ?Bots=N and -AutoSpectate).
		WaitTime += DeltaTime;
		if (WaitTime > 60.f)
		{
			int32 Bots = 0, J = 0, DJ = 0, D = 0, WD = 0;
			for (TActorIterator<AArenaCharacter> It(World); It; ++It)
			{
				if (!It->IsPlayerControlled())
				{
					const AArenaCharacter::FMoveRecord& R = It->GetMoveRecord();
					++Bots;
					J += R.Jumps;
					DJ += R.DoubleJumps;
					D += R.Dodges;
					WD += R.WallDodges;
				}
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: NOTE botwatch %d bot pawns alive (counts since their last spawn): %d jumps, %d double jumps, %d dodges, %d wall dodges"),
				Bots, J, DJ, D, WD);
			// UT2004 Bot combat counters (whole match) and the skill knobs of each bot against UnrealGame.Bot's formulas
			int32 Warn = 0, Duck = 0, TJ = 0, GD = 0, Cmb = 0, Lock = 0;
			for (TActorIterator<AArenaBotController> It(World); It; ++It)
			{
				AArenaBotController* B = *It;
				Warn += B->NumWarnings;
				Duck += B->NumDucks;
				TJ += B->NumTacticalJumps;
				GD += B->NumGoalDodges;
				Cmb += B->NumCombos;
				Lock += B->NumLockedRockets;
				const float S = B->GetSkill();
				const float RotUU = S > 7.f ? 90000.f : (S >= 4.f ? 7000.f + 10000.f * S : 30000.f + 4000.f * S);
				const FString Name = FString::Printf(TEXT("skill_%.1f"), S);
				UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: RESULT botwatch %s_rotation_rate expected=%.1f measured=%.1f deg/s %s"), *Name, RotUU * 360.f / 65536.f,
					B->GetRotationRateYaw(), FMath::IsNearlyEqual(RotUU * 360.f / 65536.f, B->GetRotationRateYaw(), 0.1f) ? TEXT("OK") : TEXT("MISMATCH"));
				const float Combat = 1.2f - 0.09f * FMath::Min(10.f, S);
				UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: RESULT botwatch %s_combat_timer expected=%.3f measured=%.3f s %s"), *Name, Combat,
					B->GetCombatTimerPeriod(), FMath::IsNearlyEqual(Combat, B->GetCombatTimerPeriod(), 0.001f) ? TEXT("OK") : TEXT("MISMATCH"));
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: NOTE botwatch bot combat: %d shot warnings, %d ducks (dodges away from shots), %d tactical strafe jumps, %d dodges towards path nodes, %d shock combos, %d locked rockets"),
				Warn, Duck, TJ, GD, Cmb, Lock);
			bDone = true;
			if (FParse::Param(FCommandLine::Get(), TEXT("MoveTestQuit")))
			{
				FPlatformMisc::RequestExit(false);
			}
		}
		return;
	}
	if (!bRigBuilt)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		AArenaCharacter* Char = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
		WaitTime += DeltaTime;
		if (!Char || WaitTime < 4.f || !GM->IsPlaying())
		{
			return;
		}
		Pawn = Char;
		FString List;
		FParse::Value(FCommandLine::Get(), TEXT("-MoveTest="), List, false);
		// "all" = every movement test, "weapons" = every w_* test; lists may mix them with test names
		TArray<FString> Parts;
		List.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Part : Parts)
		{
			if (Part.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				for (const TCHAR* T : ArenaMoveTestLocal::AllTests)
				{
					Tests.Add(T);
				}
			}
			else if (Part.Equals(TEXT("weapons"), ESearchCase::IgnoreCase))
			{
				for (const TCHAR* T : { TEXT("w_rates"), TEXT("w_damage"), TEXT("w_proj"), TEXT("w_combo"), TEXT("w_bio"), TEXT("w_rocketload"),
					TEXT("w_switch"), TEXT("w_shield"), TEXT("w_reflect"), TEXT("w_dual"), TEXT("w_extra"), TEXT("w_armor"), TEXT("w_pickups"), TEXT("w_combos"), TEXT("w_transloc"),
					TEXT("w_shake"), TEXT("w_fall"), TEXT("w_vehrest"), TEXT("w_analog"), TEXT("w_stepup") })
				{
					Tests.Add(T);
				}
			}
			else
			{
				Tests.Add(Part);
			}
		}
		// Far outside the map, so the map's zones and volumes don't apply (default gravity).
		BuildRig(Char->GetActorLocation() + FVector(400000.f, 0.f, 20000.f));
		bRigBuilt = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: rig at %s, %d tests, gravity %.0f cm/s^2"), *Origin.ToCompactString(), Tests.Num(),
			Char->GetCharacterMovement()->GetGravityZ());
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("arena.BehindView")))
		{
			CV->Set(1, ECVF_SetByCode);
		}
		StartTest(0);
		return;
	}
	if (!Pawn.IsValid() || Pawn->IsDead())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("MoveTest: pawn lost, stopping"));
		bDone = true;
		return;
	}
	if (AStaticMeshActor* L = Lift.Get())
	{
		L->GetStaticMeshComponent()->ComponentVelocity = FVector(0.f, 0.f, LiftSpeed);
		if (LiftSpeed != 0.f)
		{
			L->SetActorLocation(L->GetActorLocation() + FVector(0.f, 0.f, LiftSpeed * DeltaTime));
		}
	}
	TickTest(DeltaTime);
#endif
}

#if !UE_BUILD_SHIPPING

AStaticMeshActor* UArenaMoveTestSubsystem::SpawnBox(const FVector& Center, const FVector& SizeCm, const FRotator& Rot, bool bMovable)
{
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* A = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Center, Rot, P);
	if (!A)
	{
		return nullptr;
	}
	UStaticMeshComponent* C = A->GetStaticMeshComponent();
	C->SetMobility(EComponentMobility::Movable);
	C->SetStaticMesh(ArenaAssets::Cube());
	C->SetWorldScale3D(SizeCm / 100.f);
	C->SetCollisionProfileName(TEXT("BlockAll"));
	if (!bMovable)
	{
		C->SetMobility(EComponentMobility::Static);
	}
	return A;
}

void UArenaMoveTestSubsystem::BuildRig(const FVector& InOrigin)
{
	using namespace ArenaMoveTestLocal;
	Origin = InOrigin;
	SpawnBox(Origin - FVector(0.f, 0.f, 50.f), FVector(9000.f, 9000.f, 100.f), FRotator::ZeroRotator, false);
	// Wall facing +X at Origin.X + WallX
	SpawnBox(Origin + FVector(WallX - 100.f, 0.f, 1000.f), FVector(200.f, 3000.f, 2000.f), FRotator::ZeroRotator, false);
	// Steps: 70 cm (UE2 MAXSTEPHEIGHT 35 UU) and 76 cm, front faces at X = -200
	SpawnBox(Origin + StepPos + FVector(0.f, 0.f, 35.f), FVector(400.f, 400.f, 70.f), FRotator::ZeroRotator, false);
	SpawnBox(Origin + HighStepPos + FVector(0.f, 0.f, 38.f), FVector(400.f, 400.f, 76.f), FRotator::ZeroRotator, false);
	// Ramps of 44 and 47 degrees, 10 m long, low end at X = 0
	for (const TPair<FVector, float>& R : { TPair<FVector, float>(Ramp44Pos, 44.f), TPair<FVector, float>(Ramp47Pos, 47.f) })
	{
		const float A = FMath::DegreesToRadians(R.Value);
		const FVector C = Origin + R.Key + FVector(500.f * FMath::Cos(A), 0.f, 500.f * FMath::Sin(A) - 25.f * FMath::Cos(A));
		SpawnBox(C, FVector(1000.f, 400.f, 50.f), FRotator(R.Value, 0.f, 0.f), false);
	}
	// Water pool and its edge
	{
		const FVector P = Origin + PoolPos;
		SpawnBox(Origin + FVector(PoolEdgeX + 100.f, PoolPos.Y, 140.f), FVector(200.f, 1000.f, 280.f), FRotator::ZeroRotator, false);
		TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Bw = MakeShared<FJsonObject>();
		auto Arr = [](const FVector& V) { TArray<TSharedPtr<FJsonValue>> A; A.Add(MakeShared<FJsonValueNumber>(V.X)); A.Add(MakeShared<FJsonValueNumber>(V.Y)); A.Add(MakeShared<FJsonValueNumber>(V.Z)); return A; };
		Bw->SetArrayField(TEXT("min"), Arr(FVector(P.X - 600.f, P.Y - 500.f, P.Z - 50.f)));
		Bw->SetArrayField(TEXT("max"), Arr(FVector(Origin.X + PoolEdgeX, P.Y + 500.f, P.Z + PoolDepth)));
		Marker->SetObjectField(TEXT("bounds_world"), Bw);
		const FTransform TM(FRotator::ZeroRotator, P);
		if (AArenaWaterVolume* W = GetWorld()->SpawnActorDeferred<AArenaWaterVolume>(AArenaWaterVolume::StaticClass(), TM))
		{
			W->Shape.Load(Marker);
			W->UE2Name = TEXT("MoveTestPool");
			W->FinishSpawning(TM);
		}
	}
	// Target dummy for the weapon tests (spawned early so its spawn protection is over)
	SpawnDummy();
	// Lift platform (moved by Tick)
	LiftHome = Origin + LiftPos + FVector(0.f, 0.f, LiftHeight * 0.5f);
	Lift = SpawnBox(LiftHome, FVector(400.f, 400.f, LiftHeight), FRotator::ZeroRotator, true);
}

void UArenaMoveTestSubsystem::ResetPawn(const FVector& FeetLocation, float Yaw)
{
	AArenaCharacter* C = Pawn.Get();
	UCharacterMovementComponent* CM = C->GetCharacterMovement();
	C->SetActorLocation(FeetLocation + FVector(0.f, 0.f, ArenaMoveTestLocal::HalfHeight + 2.f), false, nullptr, ETeleportType::TeleportPhysics);
	C->SetActorRotation(FRotator(0.f, Yaw, 0.f));
	if (AController* Ctrl = C->GetController())
	{
		Ctrl->SetControlRotation(FRotator(0.f, Yaw, 0.f));
	}
	CM->Velocity = FVector::ZeroVector;
	CM->SetMovementMode(MOVE_Falling);
	C->SetDuckInput(false);
	C->SetWalkInput(false);
	C->StopJumping();
	C->Health = 100.f;
	C->Shield = 0.f;
}

void UArenaMoveTestSubsystem::StartTest(int32 Index)
{
	using namespace ArenaMoveTestLocal;
	TestIndex = Index;
	if (!Tests.IsValidIndex(Index))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: done, %d checks passed, %d failed"), Passes, Failures);
		bDone = true;
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("arena.BehindView")))
		{
			CV->Set(0, ECVF_SetByCode);
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("MoveTestQuit")))
		{
			FPlatformMisc::RequestExit(false);
		}
		return;
	}
	TestTime = 0.f;
	NextSampleTime = 0.f;
	SubIndex = 0;
	SubPhase = 0;
	SubTime = 0.f;
	ShotTimes.Reset();
	TrackedActor.Reset();
	Phase = 0;
	PhaseTime = 0.f;
	MaxZ = -1e9f;
	MaxHSpeed = 0.f;
	LandTime = -1.f;
	TakeoffTime = -1.f;
	Counter = 0;
	MarkF = MarkF2 = 0.f;
	bWasFalling = false;
	LiftSpeed = 0.f;
	TrackedProjectile.Reset();
	if (AStaticMeshActor* L = Lift.Get())
	{
		L->SetActorLocation(LiftHome);
	}

	const FString& T = Tests[Index];
	FVector Feet = Origin;
	float Yaw = 0.f;
	if (T.StartsWith(TEXT("walldodge")))
	{
		const float Gap = T == TEXT("walldodge_far") ? 80.f : 20.f;
		Feet = Origin + FVector(WallX + Radius + Gap, 0.f, 0.f);
	}
	else if (T == TEXT("step"))
	{
		Feet = Origin + StepPos + FVector(-500.f, 0.f, 0.f);
	}
	else if (T == TEXT("slope"))
	{
		Feet = Origin + Ramp44Pos + FVector(-300.f, 0.f, 0.f);
	}
	else if (T == TEXT("lift"))
	{
		Feet = LiftHome + FVector(0.f, 0.f, LiftHeight * 0.5f);
	}
	else if (T == TEXT("falldamage"))
	{
		Feet = Origin + FVector(0.f, 1000.f, 2000.f);
	}
	else if (T == TEXT("swim") || T == TEXT("drown"))
	{
		Feet = Origin + PoolPos + FVector(-400.f, 0.f, 20.f);
	}
	else if (T == TEXT("airslope"))
	{
		Feet = Origin + Ramp44Pos + FVector(-150.f, 0.f, 0.f);
	}
	else if (T == TEXT("swimmap"))
	{
		// a free spot inside a water volume of the loaded map (the capsule fits, the centre is in the brush)
		bool bFound = false;
		WadeDepth = -1.f;
		if (UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(GetWorld()))
		{
			const FCollisionShape Cap = FCollisionShape::MakeCapsule(Radius, HalfHeight);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(MoveTestWater), false, Pawn.Get());
			// First choice: a bot nav node (always on a reachable floor) that lies in water.
			for (TActorIterator<AArenaNavGraph> It(GetWorld()); It && !bFound; ++It)
			{
				for (const FArenaNavNode& N : It->GetNodes())
				{
					const AArenaPhysicsVolume* NV = Zones->GetPhysicsVolumeAt(N.Location);
					if (NV && NV->bWaterVolume && NV->UE2Name != TEXT("MoveTestPool")
						&& !GetWorld()->OverlapBlockingTestByChannel(N.Location + FVector(0.f, 0.f, 4.f), FQuat::Identity, ECC_Pawn, Cap, Q))
					{
						Feet = N.Location - FVector(0.f, 0.f, HalfHeight - 4.f);
						bFound = true;
						break;
					}
				}
			}
			for (const TWeakObjectPtr<AArenaPhysicsVolume>& V : Zones->GetVolumes())
			{
				if (bFound || !V.IsValid() || !V->bWaterVolume || V->UE2Name == TEXT("MoveTestPool"))
				{
					continue;
				}
				const FBox B = V->Shape.Bounds;
				int32 NIn = 0, NZone = 0, NUp = 0;
				for (int32 i = 0; i < 4096 && !bFound; ++i)
				{
					const FVector P(FMath::Lerp(B.Min.X, B.Max.X, FMath::FRand()), FMath::Lerp(B.Min.Y, B.Max.Y, FMath::FRand()), FMath::Lerp(B.Min.Z, B.Max.Z, FMath::FRand()));
					if (Zones->GetPhysicsVolumeAt(P) != V.Get())
					{
						continue;
					}
					++NIn;
					if (Zones->HasBspTree() && Zones->GetZoneNumberAt(P) <= 0)
					{
						continue;
					}
					++NZone;
					// UE2 swims when the pawn's centre is in the water (shallow pools: the head may stick out); prefer
					// spots with head room for the first half of the samples.
					if (i < 2048 && Zones->GetPhysicsVolumeAt(P + FVector(0.f, 0.f, HalfHeight + 20.f)) != V.Get())
					{
						continue;
					}
					++NUp;
					// stand on the pool floor below the sample (shallow water: the centre must stay in the water)
					FVector C = P;
					FHitResult Floor;
					if (GetWorld()->LineTraceSingleByChannel(Floor, P, P - FVector(0.f, 0.f, 2000.f), ECC_Pawn, Q))
					{
						C.Z = Floor.ImpactPoint.Z + HalfHeight + 2.f;
					}
					if (Floor.bBlockingHit && Zones->GetPhysicsVolumeAt(C) != V.Get() && !GetWorld()->OverlapBlockingTestByChannel(C, FQuat::Identity, ECC_Pawn, Cap, Q))
					{
						// water shallower than the pawn's centre height: UE2 wades through it (walking)
						const float Depth = B.Max.Z - Floor.ImpactPoint.Z;
						if (Depth > WadeDepth)
						{
							WadeDepth = Depth;
							WadeFeet = C - FVector(0.f, 0.f, HalfHeight + 2.f);
						}
					}
					if (Zones->GetPhysicsVolumeAt(C) == V.Get() && !GetWorld()->OverlapBlockingTestByChannel(C, FQuat::Identity, ECC_Pawn, Cap, Q))
					{
						Feet = C - FVector(0.f, 0.f, HalfHeight + 2.f);
						bFound = true;
					}
				}
				Note(FString::Printf(TEXT("water volume %s bounds %s: %d samples inside, %d in open zones, %d with head room, found %d"),
					*V->UE2Name, *B.ToString(), NIn, NZone, NUp, bFound ? 1 : 0));
			}
		}
		if (!bFound && WadeDepth > 0.f)
		{
			Feet = WadeFeet;
			Note(FString::Printf(TEXT("no swimmable water: deepest %.0f cm, below the pawn's centre (%.0f cm above its feet): wading"), WadeDepth, HalfHeight));
		}
		else
		{
			WadeDepth = -1.f;
		}
		Note(FString::Printf(TEXT("map water volume spot %s"), *Feet.ToCompactString()));
	}
	else if (T == TEXT("strafejump") || T == TEXT("bunnyhop") || T == TEXT("dodgechain") || T == TEXT("aircontrol"))
	{
		Feet = Origin + FVector(-3800.f, -3800.f, 0.f);
		Yaw = 45.f;
	}
	StartLoc = Feet;
	StartFeetZ = Feet.Z;
	ResetPawn(Feet, Yaw);
	UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: ---- %s ----"), *T);
}

void UArenaMoveTestSubsystem::FinishTest()
{
	if (Lift.IsValid())
	{
		LiftSpeed = 0.f;
	}
	if (UWorld* World = GetWorld())
	{
		if (AWorldSettings* WS = World->GetWorldSettings())
		{
			if (Tests.IsValidIndex(TestIndex) && Tests[TestIndex] == TEXT("lowgrav"))
			{
				WS->bWorldGravitySet = false;
				WS->bGlobalGravitySet = false;
			}
		}
	}
	if (AArenaCharacter* C = Pawn.Get())
	{
		C->StopFire(0);
		C->StopFire(1);
		C->SetDuckInput(false);
		C->ActivateCombo(EArenaCombo::None);
		C->Health = 100.f;
	}
	for (const TWeakObjectPtr<AActor>& A : TestActors)
	{
		if (A.IsValid())
		{
			A->Destroy();
		}
	}
	TestActors.Reset();
	// mines, sticky grenades and rockets of one test must not hit the dummy in the next
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (Cast<AArenaSeekerRocket>(*It) || Cast<AArenaCrawlerMine>(*It) || Cast<AArenaStickyGrenade>(*It) || Cast<AArenaStagedBlast>(*It)
			|| Cast<AArenaWarheadShell>(*It) || Cast<AArenaProjectile>(*It))
		{
			It->Destroy();
		}
	}
	if (AArenaCharacter* D = Dummy.Get())
	{
		D->SetActorLocation(Origin + FVector(-3500.f, -3500.f, 300.f));
	}
	StartTest(TestIndex + 1);
}

void UArenaMoveTestSubsystem::Result(const TCHAR* Metric, double Expected, double Measured, const TCHAR* Unit, double Tolerance)
{
	const FString U(Unit);
	const bool bCount = U.IsEmpty() || U == TEXT("dodges") || U == TEXT("frames") || U == TEXT("flag");
	const double Tol = Tolerance > 0.0 ? Tolerance : bCount ? 0.1 : FMath::Max(2.0, FMath::Abs(Expected) * 0.03);
	const bool bOk = FMath::Abs(Expected - Measured) <= Tol;
	(bOk ? Passes : Failures)++;
	UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: RESULT %s %s expected=%.1f measured=%.1f %s %s"), *Tests[TestIndex], Metric, Expected, Measured, Unit,
		bOk ? TEXT("OK") : TEXT("MISMATCH"));
}

void UArenaMoveTestSubsystem::Note(const FString& Text)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: NOTE %s %s"), *Tests[TestIndex], *Text);
}

void UArenaMoveTestSubsystem::Shot(const FString& Tag)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("MoveTestShots")))
	{
		FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("movetest_%s_%s"), *Tests[TestIndex], *Tag), false, false);
	}
}

void UArenaMoveTestSubsystem::TickTest(float DeltaTime)
{
	using namespace ArenaMoveTestLocal;
	AArenaCharacter* C = Pawn.Get();
	UArenaCharacterMovement* CM = Cast<UArenaCharacterMovement>(C->GetCharacterMovement());
	// the body left behind while flying a guided warhead still runs its test (w_extra)
	AController* Ctrl = C->GetController() ? C->GetController() : C->GetRemoteController();
	if (!CM || !Ctrl)
	{
		return;
	}
	const FString& T = Tests[TestIndex];
	TestTime += DeltaTime;
	PhaseTime += DeltaTime;
	const FVector Loc = C->GetActorLocation();
	const float FeetZ = Loc.Z - C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Vel = CM->Velocity;
	const float HSpeed = Vel.Size2D();
	const AArenaCharacter::FMoveRecord& Rec = C->GetMoveRecord();
	const bool bFalling = CM->IsFalling();
	const bool bLanded = bWasFalling && CM->IsMovingOnGround();
	const bool bTookOff = !bWasFalling && bFalling;
	bWasFalling = bFalling;

	if (TestTime >= NextSampleTime)
	{
		NextSampleTime = TestTime + 0.05f;
		UE_LOG(LogOpenUT2k4, Log, TEXT("MoveTest: %s t=%.3f x=%.1f y=%.1f z=%.1f vz=%.1f hspeed=%.1f mode=%d dclick=%d mj=%d hp=%.0f"), *T, TestTime,
			Loc.X - Origin.X, Loc.Y - Origin.Y, FeetZ - Origin.Z, Vel.Z, HSpeed, int32(CM->MovementMode.GetValue()), C->GetDoubleClickDir(),
			C->GetMultiJumpRemaining(), C->Health);
	}

	auto NextPhase = [this]() { ++Phase; PhaseTime = 0.f; };
	auto Fwd = [Ctrl]() { return FRotator(0.f, Ctrl->GetControlRotation().Yaw, 0.f).Vector(); };
	auto Right = [Ctrl]() { return FRotationMatrix(FRotator(0.f, Ctrl->GetControlRotation().Yaw, 0.f)).GetUnitAxis(EAxis::Y); };
	// Settle on the floor first (and let any dodge lockout from the previous test expire).
	if (Phase == 0)
	{
		if (PhaseTime > 0.8f && (CM->IsMovingOnGround() || (CM->IsSwimming() && (T == TEXT("swim") || T == TEXT("drown") || T == TEXT("swimmap")))))
		{
			NextPhase();
			StartLoc = Loc;
			StartFeetZ = FeetZ;
			MaxZ = FeetZ;
		}
		else if (TestTime > 6.f && T != TEXT("falldamage") && T != TEXT("view"))
		{
			Note(TEXT("never landed"));
			FinishTest();
		}
		if (T == TEXT("falldamage"))
		{
			MarkF = FMath::Min(MarkF, Vel.Z);
			if (bLanded)
			{
				const float Impact = -Rec.LandImpactVelocity.Z;
				const int32 Expected = Impact > ArenaTuning::MaxFallSpeed ? int32(100.f * (Impact - ArenaTuning::MaxFallSpeed) / ArenaTuning::MaxFallSpeed) : 0;
				Result(TEXT("impact_speed"), FMath::Sqrt(2.f * G * 2000.f), Impact, TEXT("cm/s"));
				Result(TEXT("fall_damage"), Expected, 100.f - C->Health, TEXT("hp"));
				Note(FString::Printf(TEXT("UE2 rule: damage = 100 * (impact - 2400) / 2400 above MaxFallSpeed 1200 UU/s (2400 cm/s); a 2000 cm drop is %.0f cm/s"), FMath::Sqrt(2.f * G * 2000.f)));
				FinishTest();
			}
		}
		return;
	}
	MaxZ = FMath::Max(MaxZ, FeetZ);
	MaxHSpeed = FMath::Max(MaxHSpeed, HSpeed);

	if (TickWeaponTest(T, DeltaTime))
	{
		return;
	}

	// ---------------------------------------------------------------------------------- jumps
	if (T == TEXT("jump") || T == TEXT("lowgrav"))
	{
		if (Phase == 1)
		{
			if (T == TEXT("lowgrav"))
			{
				AWorldSettings* WS = GetWorld()->GetWorldSettings();
				WS->WorldGravityZ = UPhysicsSettings::Get()->DefaultGravityZ * (300.f / 950.f);
				WS->bWorldGravitySet = true;
				WS->bGlobalGravitySet = true;
				WS->GlobalGravityZ = WS->WorldGravityZ;
			}
			C->Jump();
			NextPhase();
		}
		else if (Phase == 2 && bLanded)
		{
			const float Grav = T == TEXT("lowgrav") ? G * 300.f / 950.f : G;
			Result(TEXT("apex"), FMath::Square(ArenaTuning::JumpZ) / (2.f * Grav), MaxZ - StartFeetZ);
			Result(TEXT("airtime"), 2.f * ArenaTuning::JumpZ / Grav, PhaseTime, TEXT("s"));
			if (T == TEXT("lowgrav"))
			{
				Note(FString::Printf(TEXT("gravity during test %.0f cm/s^2 (UE2 MutLowGrav -300 UU/s^2)"), CM->GetGravityZ()));
			}
			FinishTest();
		}
		return;
	}
	if (T.StartsWith(TEXT("doublejump")))
	{
		if (Phase == 1)
		{
			C->Jump();
			NextPhase();
		}
		else if (Phase == 2 && bFalling)
		{
			const float PressVz = T == TEXT("doublejump_early") ? 400.f : (T == TEXT("doublejump_late") ? -400.f : 0.f);
			if (Vel.Z <= PressVz)
			{
				MarkF = Vel.Z;
				MarkF2 = FeetZ - StartFeetZ;
				C->Jump();
				NextPhase();
			}
		}
		else if (Phase == 3 && Counter == 0 && (Vel.Z > 0.f || T != TEXT("doublejump")))
		{
			// first frame after the press was processed (velocity after one physics step of gravity)
			Counter = 1;
			MarkF = Vel.Z + (T == TEXT("doublejump") ? G * GetWorld()->GetDeltaSeconds() : 0.f);
		}
		else if (Phase == 3 && bLanded)
		{
			const bool bWindow = T == TEXT("doublejump");
			const float JumpApex = FMath::Square(ArenaTuning::JumpZ) / (2.f * G);
			if (bWindow)
			{
				const float DJ = ArenaTuning::JumpZ + ArenaTuning::MultiJumpBoost;
				// apex from where the double jump started (velocity right after it is JumpZ + MultiJumpBoost)
				Result(TEXT("doublejump_velocity"), DJ, Rec.TakeoffVelocity.Z, TEXT("cm/s"));
				Result(TEXT("apex"), MarkF2 + FMath::Square(DJ) / (2.f * G), MaxZ - StartFeetZ);
				Note(FString::Printf(TEXT("UE2 double jump apex range %.1f (pressed at |vz| = 100 UU/s) to %.1f (at the apex)"),
					(FMath::Square(ArenaTuning::JumpZ) - FMath::Square(200.f)) / (2.f * G) + FMath::Square(DJ) / (2.f * G), JumpApex + FMath::Square(DJ) / (2.f * G)));
			}
			else
			{
				Result(TEXT("apex_no_double_jump"), JumpApex, MaxZ - StartFeetZ);
			}
			FinishTest();
		}
		return;
	}

	// ---------------------------------------------------------------------------------- dodges
	if (T == TEXT("dodge") || T == TEXT("dodgejump") || T == TEXT("dodge_land_jump") || T == TEXT("dodge_lockout"))
	{
		if (Phase == 1)
		{
			C->OnDirectionTap(EArenaDir::Forward);
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime >= 0.1f)
		{
			C->OnDirectionTap(EArenaDir::Forward);
			NextPhase();
		}
		else if (Phase == 3)
		{
			if (bTookOff || (bFalling && TakeoffTime < 0.f))
			{
				TakeoffTime = TestTime;
				Mark = Loc;
				MarkF = Vel.Z;
				MarkF2 = HSpeed;
			}
			if (TakeoffTime >= 0.f && T == TEXT("dodgejump") && Counter == 0 && Vel.Z <= 0.f)
			{
				Counter = 1;
				MarkF2 = FeetZ - StartFeetZ;
				Mark = Rec.TakeoffLocation;
				C->Jump();
			}
			if (TakeoffTime >= 0.f && T == TEXT("dodge") && Counter == 0 && Vel.Z <= 0.f)
			{
				Counter = 1;
				Shot(TEXT("apex"));
			}
			if (bLanded)
			{
				const float Air = TestTime - TakeoffTime;
				// take-off of the dodge to the landing point (Mark holds the dodge take-off when a double jump followed)
				const float Dist = FVector::Dist2D(Rec.LandLocation, T == TEXT("dodgejump") ? Mark : Rec.TakeoffLocation);
				const float DodgeT = 2.f * ArenaTuning::DodgeSpeedZ / G;
				if (T == TEXT("dodge"))
				{
					Result(TEXT("takeoff_vz"), ArenaTuning::DodgeSpeedZ, Rec.TakeoffVelocity.Z, TEXT("cm/s"));
					Result(TEXT("takeoff_hspeed"), ArenaTuning::DodgeSpeed, Rec.TakeoffVelocity.Size2D(), TEXT("cm/s"));
					Result(TEXT("apex"), FMath::Square(ArenaTuning::DodgeSpeedZ) / (2.f * G), MaxZ - StartFeetZ);
					Result(TEXT("airtime"), DodgeT, Air, TEXT("s"));
					Result(TEXT("distance"), ArenaTuning::DodgeSpeed * DodgeT, Dist);
					Result(TEXT("landing_hspeed"), ArenaTuning::DodgeSpeed * 0.1f, C->GetLastLandedVelocity().Size2D(), TEXT("cm/s"));
					FinishTest();
				}
				else if (T == TEXT("dodgejump"))
				{
					const float DJ = ArenaTuning::JumpZ + ArenaTuning::MultiJumpBoost;
					const float H0 = MarkF2;
					const float TUp = ArenaTuning::DodgeSpeedZ / G;
					const float TDown = (DJ + FMath::Sqrt(DJ * DJ + 2.f * G * H0)) / G;
					Result(TEXT("apex"), H0 + DJ * DJ / (2.f * G), MaxZ - StartFeetZ);
					Result(TEXT("distance"), ArenaTuning::DodgeSpeed * (TUp + TDown), Dist);
					Note(FString::Printf(TEXT("ideal (double jump at the dodge apex): apex %.1f cm, distance %.1f cm, airtime %.3f s (measured %.3f)"),
						FMath::Square(ArenaTuning::DodgeSpeedZ) / (2.f * G) + DJ * DJ / (2.f * G), ArenaTuning::DodgeSpeed * (TUp + TDown), TUp + TDown, Air));
					FinishTest();
				}
				else
				{
					Mark = Loc;
					LandLoc = FVector(HSpeed, 0.f, 0.f);
					Result(TEXT("landing_hspeed"), ArenaTuning::DodgeSpeed * 0.1f, C->GetLastLandedVelocity().Size2D(), TEXT("cm/s"));
					NextPhase();
					if (T == TEXT("dodge_land_jump"))
					{
						C->Jump();
					}
					else
					{
						C->OnDirectionTap(EArenaDir::Forward);
					}
				}
			}
		}
		else if (Phase == 4 && T == TEXT("dodge_land_jump"))
		{
			if (TakeoffTime >= 0.f && bFalling && Counter < 2)
			{
				Counter = 2;
				MarkF = Vel.Z;
				MarkF2 = HSpeed;
			}
			if (bLanded)
			{
				Result(TEXT("jump_vz"), ArenaTuning::JumpZ, Rec.TakeoffVelocity.Z, TEXT("cm/s"));
				Result(TEXT("jump_hspeed_vs_landing_speed"), LandLoc.X, Rec.TakeoffVelocity.Size2D(), TEXT("cm/s"),
					LandLoc.X * 0.15f);
				Result(TEXT("jump_distance"), Rec.TakeoffVelocity.Size2D() * 2.f * ArenaTuning::JumpZ / G, FVector::Dist2D(Rec.LandLocation, Rec.TakeoffLocation));
				Note(TEXT("UE2: landing from a dodge keeps 10% of the speed, so jumping at the landing gives a plain jump"));
				FinishTest();
			}
		}
		else if (Phase == 4 && T == TEXT("dodge_lockout"))
		{
			// taps at landing + 0 / 0.1 s: inside the 0.35 lockout (timer at 1.1x), no dodge
			if (Counter == 0 && PhaseTime >= 0.1f)
			{
				Counter = 1;
				C->OnDirectionTap(EArenaDir::Forward);
			}
			if (PhaseTime >= 0.25f)
			{
				Result(TEXT("dodge_within_lockout"), 0.f, bFalling ? 1.f : 0.f, TEXT("dodges"));
				NextPhase();
			}
		}
		else if (Phase == 5)
		{
			if (Counter == 1 && PhaseTime >= 0.3f)
			{
				Counter = 2;
				C->OnDirectionTap(EArenaDir::Forward);
			}
			else if (Counter == 2 && PhaseTime >= 0.4f)
			{
				Counter = 3;
				C->OnDirectionTap(EArenaDir::Forward);
			}
			else if (Counter == 3 && PhaseTime >= 0.5f)
			{
				Result(TEXT("dodge_after_lockout"), 1.f, bFalling && Vel.Z > 0.f ? 1.f : 0.f, TEXT("dodges"));
				Note(FString::Printf(TEXT("lockout after a dodge landing: %.3f s real time (0.35 at 1.1x)"), ArenaTuning::DodgeCooldown / 1.1f));
				FinishTest();
			}
		}
		return;
	}

	// ---------------------------------------------------------------------------------- wall dodges
	if (T.StartsWith(TEXT("walldodge")))
	{
		const bool bAfterDJ = T == TEXT("walldodge_after_doublejump");
		if (Phase == 1)
		{
			C->Jump();
			NextPhase();
		}
		else if (Phase == 2 && bFalling)
		{
			if (bAfterDJ && Counter == 0 && Vel.Z <= 0.f)
			{
				Counter = 1;
				C->Jump();
			}
			else if ((!bAfterDJ && Vel.Z <= 100.f) || (bAfterDJ && Counter == 1 && Vel.Z > 0.f && Vel.Z < 400.f && C->GetMultiJumpRemaining() == 0))
			{
				C->OnDirectionTap(EArenaDir::Forward);
				NextPhase();
			}
		}
		else if (Phase == 3 && PhaseTime >= 0.05f)
		{
			C->OnDirectionTap(EArenaDir::Forward);
			NextPhase();
		}
		else if (Phase == 4 && PhaseTime > 0.f && Counter < 5)
		{
			Counter = 5;
			MarkF = Rec.Takeoffs > 0 && Rec.TakeoffVelocity.X > 1000.f ? Rec.TakeoffVelocity.X : Vel.X;
			MarkF2 = Rec.TakeoffVelocity.Z;
			Mark = FVector(PhaseTime, Vel.Z, 0.f);
			Shot(TEXT("kick"));
			if (T == TEXT("walldodge_twice"))
			{
				// second wall dodge at once, while the wall is still within reach
				C->OnDirectionTap(EArenaDir::Forward);
			}
		}
		else if (Phase == 4 && Counter == 5 && T == TEXT("walldodge_twice"))
		{
			Counter = 6;
			C->OnDirectionTap(EArenaDir::Forward);
		}
		else if (Phase == 4 && PhaseTime >= 0.15f)
		{
			const bool bKick = MarkF > 1000.f;
			if (T == TEXT("walldodge_far"))
			{
				Result(TEXT("wall_dodge_happened"), 0.f, bKick ? 1.f : 0.f, TEXT("dodges"));
				Note(TEXT("wall 80 cm from the capsule; UE2 traces 32 UU (64 cm) from the feet"));
				FinishTest();
				return;
			}
			Result(TEXT("kick_speed_away_from_wall"), ArenaTuning::DodgeSpeed, MarkF, TEXT("cm/s"));
			Result(TEXT("kick_vz"), ArenaTuning::DodgeSpeedZ, MarkF2, TEXT("cm/s"));
			if (T == TEXT("walldodge_twice"))
			{
				// a second kick would have reset vz to 420
				const float NoSecond = Mark.Y - G * (PhaseTime - Mark.X);
				Result(TEXT("vz_0.15s_after_kick_no_second_dodge"), NoSecond, Vel.Z, TEXT("cm/s"));
				Note(TEXT("UE2: one dodge (ground or wall) per air time; DoubleClickDir stays 5 (active) until landing"));
			}
			if (bAfterDJ)
			{
				Note(TEXT("UE2 allows a wall dodge after a double jump (and a double jump after a wall dodge while MultiJumpRemaining > 0)"));
			}
			FinishTest();
		}
		return;
	}

	// ---------------------------------------------------------------------------------- air control / speed limits
	if (T == TEXT("aircontrol"))
	{
		// run to full speed, jump, hold back: UE2 air control 0.35 * AccelRate brakes 1434 cm/s^2
		if (Phase == 1)
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (PhaseTime > 1.f)
			{
				Result(TEXT("run_speed"), ArenaTuning::GroundSpeed, HSpeed, TEXT("cm/s"));
				C->Jump();
				NextPhase();
			}
		}
		else if (Phase == 2)
		{
			if (bFalling && TakeoffTime < 0.f)
			{
				TakeoffTime = TestTime;
				MarkF = HSpeed;
			}
			C->AddMovementInput(TakeoffTime >= 0.f ? -Fwd() : Fwd(), 1.f);
			if (TakeoffTime >= 0.f && TestTime - TakeoffTime >= 0.3f)
			{
				const float A = ArenaTuning::AirControl * ArenaTuning::AccelRate;
				Result(TEXT("hspeed_after_0.3s_reverse"), MarkF - A * (TestTime - TakeoffTime), HSpeed, TEXT("cm/s"));
				NextPhase();
			}
		}
		else if (Phase == 3)
		{
			if (bLanded)
			{
				FinishTest();
			}
		}
		return;
	}
	if (T == TEXT("strafejump") || T == TEXT("bunnyhop") || T == TEXT("dodgechain"))
	{
		if (T == TEXT("strafejump"))
		{
			// Quake style: forward + strafe while turning into the strafe, jumping on every landing
			FRotator R = Ctrl->GetControlRotation();
			const float Side = FMath::Fmod(PhaseTime, 2.f) < 1.f ? 1.f : -1.f;
			R.Yaw += Side * 120.f * DeltaTime;
			Ctrl->SetControlRotation(R);
			C->AddMovementInput((Fwd() + Side * Right()).GetSafeNormal(), 1.f);
			if (CM->IsMovingOnGround())
			{
				C->Jump();
			}
		}
		else if (T == TEXT("bunnyhop"))
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (CM->IsMovingOnGround() && PhaseTime > 0.5f)
			{
				C->Jump();
			}
		}
		else
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (CM->IsMovingOnGround() && C->GetDoubleClickDir() == 0 && PhaseTime > 0.5f)
			{
				C->OnDirectionTap(EArenaDir::Forward);
				Counter = 1;
			}
			else if (Counter == 1 && C->GetDoubleClickDir() == 3)
			{
				C->OnDirectionTap(EArenaDir::Forward);
				Counter = 0;
			}
		}
		if (PhaseTime > 0.5f)
		{
			MarkF += HSpeed * DeltaTime;
			MarkF2 += DeltaTime;
		}
		else
		{
			MaxHSpeed = 0.f;
		}
		if (PhaseTime > 5.5f)
		{
			const float Limit = T == TEXT("dodgechain") ? ArenaTuning::DodgeSpeed : ArenaTuning::GroundSpeed;
			Result(TEXT("max_hspeed"), Limit, MaxHSpeed, TEXT("cm/s"));
			Note(FString::Printf(TEXT("average horizontal speed %.1f cm/s over %.1f s"), MarkF / FMath::Max(MarkF2, 0.01f), MarkF2));
			FinishTest();
		}
		return;
	}

	// ---------------------------------------------------------------------------------- weapon boosts
	if (T.StartsWith(TEXT("rocketjump")) || T.StartsWith(TEXT("shieldjump")))
	{
		const bool bRocket = T.StartsWith(TEXT("rocketjump"));
		const bool bWithJump = T.EndsWith(TEXT("_jump"));
		const int32 Skill = GetWorld()->GetAuthGameMode<AArenaGameMode>()->GetBotSkill();
		const float SelfScale = Skill <= 3 ? 0.5f : 1.f;
		if (Phase == 1)
		{
			TSubclassOf<AArenaWeapon> WC = bRocket ? TSubclassOf<AArenaWeapon>(AWeap_RocketLauncher::StaticClass()) : TSubclassOf<AArenaWeapon>(AWeap_Gauntlet::StaticClass());
			C->GiveWeapon(WC);
			C->SwitchToWeapon(C->FindWeapon(WC));
			Ctrl->SetControlRotation(FRotator(-89.f, 0.f, 0.f));
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime > 1.5f)
		{
			Ctrl->SetControlRotation(FRotator(-89.f, 0.f, 0.f));
			C->Health = 100.f;
			if (bRocket)
			{
				if (bWithJump)
				{
					C->Jump();
				}
				NextPhase();
			}
			else
			{
				C->StartFire(0);
				NextPhase();
			}
		}
		else if (Phase == 3)
		{
			if (bRocket)
			{
				// fire on the frame the jump was taken (or at once)
				if (!bWithJump || bFalling)
				{
					MarkF2 = Vel.Z;
					C->StartFire(0);
					NextPhase();
				}
			}
			else if (PhaseTime >= 2.7f)
			{
				if (bWithJump && Counter == 0)
				{
					Counter = 1;
					C->Jump();
				}
				else if (!bWithJump || bFalling)
				{
					MarkF2 = Vel.Z;
					C->StopFire(0);
					NextPhase();
				}
			}
		}
		else if (Phase == 4)
		{
			if (bRocket)
			{
				C->StopFire(0);
				if (!TrackedProjectile.IsValid() && Counter < 10)
				{
					for (TActorIterator<AArenaProjectile> It(GetWorld()); It; ++It)
					{
						if (It->GetInstigator() == C)
						{
							TrackedProjectile = *It;
							Counter = 10;
						}
					}
				}
				if (AActor* P = TrackedProjectile.Get())
				{
					LastProjectileLoc = P->GetActorLocation();
					Mark = Loc;
					MarkF2 = Vel.Z;
				}
				else if (Counter == 10)
				{
					// exploded this frame: the momentum is in the velocity now. The blast and the pawn are taken where they were
					// when it went off (not a frame earlier), so the frame rate does not matter
					Counter = 11;
					Mark = AArenaProjectile::LastExplodeInstigatorLocation;
					LastProjectileLoc = AArenaProjectile::LastExplodeLocation;
					const float Dist = FMath::Max(0.f, FVector::Dist(Mark, LastProjectileLoc) - Radius);
					const float Scale = FMath::Max(0.f, 1.f - Dist / 440.f);
					FVector M = (Mark - LastProjectileLoc).GetSafeNormal() * 1000.f * Scale;
					if (!bWithJump)
					{
						M.Z = FMath::Max(M.Z, 0.4f * M.Size());
					}
					M *= 0.6f;
					MarkF2 = Rec.PushVelocityBefore.Z;
					float Add = M.Z;
					if (MarkF2 > 760.f && Add > 0.f)
					{
						Add *= 0.5f;
					}
					const float ExpectedVz = MarkF2 + Add;
					MarkF = Vel.Z;
					Result(TEXT("boost_vz"), ExpectedVz, Rec.PushVelocityAfter.Z, TEXT("cm/s"));
					Result(TEXT("self_damage"), FMath::FloorToFloat(FMath::FloorToFloat(90.f * Scale) * SelfScale), 100.f - C->Health, TEXT("hp"));
					Note(FString::Printf(TEXT("explosion %.1f cm from the centre (falloff %.3f), vz before %.1f; UE2 rocket alone from standing: %.1f cm/s"), Dist + Radius, Scale, MarkF2,
						0.6f * 1000.f * Scale));
					StartFeetZ = FeetZ;
					MaxZ = FeetZ;
					NextPhase();
				}
				else if (PhaseTime > 2.f)
				{
					Note(TEXT("no rocket seen"));
					FinishTest();
				}
			}
			else
			{
				// shield: the boost is applied at the release
				MarkF = Vel.Z;
				MarkF2 = Rec.PushVelocityBefore.Z;
				const float Force = 2000.f * 0.6f;
				const float Add = (MarkF2 > 760.f) ? Force * 0.5f : Force;
				Result(TEXT("boost_vz"), MarkF2 + Add, Rec.PushVelocityAfter.Z, TEXT("cm/s"));
				Result(TEXT("self_damage"), FMath::FloorToFloat((8.f + 0.3f * 150.f) * SelfScale), 100.f - C->Health, TEXT("hp"));
				Note(FString::Printf(TEXT("full charge (2.5 s): force 100000 -> %.0f cm/s self (x0.6), self damage 8 + 0.3 x 150 = 53 (x%.1f at skill %d)"), Force, SelfScale, Skill));
				StartFeetZ = FeetZ;
				MaxZ = FeetZ;
				NextPhase();
			}
		}
		else if (Phase == 5)
		{
			if (Counter != 99 && Vel.Z <= 0.f)
			{
				Counter = 99;
				Shot(TEXT("apex"));
			}
			if (bLanded)
			{
				Result(TEXT("apex_from_boost"), FMath::Square(MarkF) / (2.f * G), MaxZ - StartFeetZ);
				FinishTest();
			}
		}
		return;
	}

	// ---------------------------------------------------------------------------------- lift
	if (T == TEXT("lift"))
	{
		if (Phase == 1)
		{
			LiftSpeed = 400.f;
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime >= 1.f)
		{
			MarkF2 = CM->GetBaseVelocityUE2().Z;
			C->Jump();
			NextPhase();
		}
		else if (Phase == 3 && bFalling && Counter == 0)
		{
			// first frame in the air: undo the gravity of this frame's physics step to get the take-off speed
			Counter = 1;
			MarkF = Rec.TakeoffVelocity.Z;
			StartFeetZ = Rec.TakeoffLocation.Z;
			MaxZ = FeetZ;
			LiftSpeed = 0.f;
			Shot(TEXT("takeoff"));
		}
		else if (Phase == 3 && Counter == 1 && (bLanded || Vel.Z < -600.f))
		{
			Result(TEXT("lift_velocity_seen"), 400.f, MarkF2, TEXT("cm/s"));
			Result(TEXT("jump_vz"), ArenaTuning::JumpZ + 400.f, MarkF, TEXT("cm/s"));
			Result(TEXT("apex_above_takeoff"), FMath::Square(MarkF) / (2.f * G), MaxZ - StartFeetZ);
			Note(TEXT("UE2 Pawn.DoJump adds Base.Velocity.Z of a non-world-geometry base; no horizontal base velocity"));
			FinishTest();
		}
		return;
	}

	// ---------------------------------------------------------------------------------- crouch, steps, slopes
	if (T == TEXT("crouch"))
	{
		C->SetDuckInput(true);
		C->AddMovementInput(Fwd(), 1.f);
		if (Phase == 1 && PhaseTime > 1.f)
		{
			Result(TEXT("crouched_speed"), ArenaTuning::GroundSpeed * ArenaTuning::CrouchedPct, HSpeed, TEXT("cm/s"));
			Result(TEXT("crouched_half_height"), ArenaTuning::CrouchedHalfHeight, C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			MaxZ = FeetZ;
			StartFeetZ = FeetZ;
			C->Jump();
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime > 0.5f)
		{
			Result(TEXT("crouch_jump_height"), 0.f, MaxZ - StartFeetZ);
			Note(TEXT("UE2 Pawn.DoJump refuses while crouched; no crouching in the air"));
			C->SetDuckInput(false);
			FinishTest();
		}
		return;
	}
	if (T == TEXT("step") || T == TEXT("slope"))
	{
		C->AddMovementInput(Fwd(), 1.f);
		if (Phase == 1 && PhaseTime > (T == TEXT("step") ? 1.5f : 2.5f))
		{
			if (T == TEXT("step"))
			{
				Result(TEXT("climbed_70cm_step"), 70.f, MaxZ - StartFeetZ);
				ResetPawn(Origin + HighStepPos + FVector(-500.f, 0.f, 0.f), 0.f);
			}
			else
			{
				Result(TEXT("height_on_44deg_ramp"), 1000.f * FMath::Sin(FMath::DegreesToRadians(44.f)), MaxZ - StartFeetZ);
				ResetPawn(Origin + Ramp47Pos + FVector(-300.f, 0.f, 0.f), 0.f);
			}
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime > 0.5f && CM->IsMovingOnGround())
		{
			StartFeetZ = FeetZ;
			MaxZ = FeetZ;
			NextPhase();
		}
		else if (Phase == 3 && PhaseTime > 2.f)
		{
			if (T == TEXT("step"))
			{
				Result(TEXT("climbed_76cm_step"), 0.f, MaxZ - StartFeetZ);
				Note(TEXT("UE2 MAXSTEPHEIGHT 35 UU = 70 cm"));
			}
			else
			{
				Result(TEXT("height_on_47deg_ramp_max_step"), 0.f, MaxZ - StartFeetZ > ArenaTuning::MaxStepHeight + 5.f ? MaxZ - StartFeetZ : 0.f);
				Note(FString::Printf(TEXT("47 degree ramp: highest feet %.1f cm (walkable floor Z 0.7 = 45.57 degrees)"), MaxZ - StartFeetZ));
			}
			FinishTest();
		}
		return;
	}

	// ---------------------------------------------------------------------------------- view: eye height, crouch, landing dip, bob
	if (T == TEXT("view"))
	{
		const float Eye = C->GetViewEyeHeight();
		const FVector Bob = C->GetViewWalkBob();
		if (Phase == 1 && PhaseTime > 0.5f)
		{
			Result(TEXT("standing_eye_height"), ArenaTuning::EyeHeight, Eye);
			C->SetDuckInput(true);
			NextPhase();
		}
		else if (Phase == 2 && PhaseTime > 1.5f)
		{
			Result(TEXT("crouched_eye_height"), ArenaTuning::CrouchedEyeHeight, Eye);
			Result(TEXT("crouched_eye_above_feet"), ArenaTuning::CrouchedHalfHeight + ArenaTuning::CrouchedEyeHeight, Loc.Z + Eye - FeetZ);
			C->SetDuckInput(false);
			NextPhase();
		}
		else if (Phase == 3 && PhaseTime > 1.f)
		{
			Result(TEXT("eye_after_uncrouch"), ArenaTuning::EyeHeight, Eye);
			ResetPawn(Origin + FVector(0.f, 1000.f, 300.f), 0.f);
			MarkF = 1000.f;
			NextPhase();
		}
		else if (Phase == 4)
		{
			MarkF = FMath::Min(MarkF, Eye);
			if (PhaseTime > 1.5f)
			{
				// UE2: the eye drops fast after a landing over 200 UU/s and recovers from 0.25 x BaseEyeHeight + 1
				Result(TEXT("landing_dip_lowest_eye"), 0.25f * ArenaTuning::EyeHeight + 2.f, MarkF);
				Result(TEXT("eye_recovered"), ArenaTuning::EyeHeight, Eye);
				MarkF = MarkF2 = 0.f;
				NextPhase();
			}
		}
		else if (Phase == 5)
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (PhaseTime > 0.6f)
			{
				MarkF = FMath::Max(MarkF, FMath::Abs(Bob.Y));
				MarkF2 = FMath::Max(MarkF2, FMath::Abs(Bob.Z));
			}
			if (PhaseTime > 2.5f)
			{
				// Pawn.CheckBob: Y = Bob x speed x sin(8t), Z = 0.75 x Bob x speed x sin(16t), Bob 0.006, speed 880
				Result(TEXT("bob_side_amplitude"), ArenaTuning::ViewBob * ArenaTuning::GroundSpeed, MarkF, TEXT("cm"), 0.3);
				Result(TEXT("bob_up_amplitude"), 0.75f * ArenaTuning::ViewBob * ArenaTuning::GroundSpeed, MarkF2, TEXT("cm"), 0.3);
				FinishTest();
			}
		}
		return;
	}
	// ---------------------------------------------------------------------------------- air control into a walkable slope
	if (T == TEXT("airslope"))
	{
		if (Phase == 1)
		{
			C->Jump();
			NextPhase();
		}
		else if (Phase == 2)
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (bFalling)
			{
				MarkF = FMath::Max(MarkF, CM->LastTickAirControl < 0.1f ? 1.f : 0.f);
				MarkF2 = FMath::Max(MarkF2, CM->LastTickAirControl);
			}
			if (bLanded || PhaseTime > 1.5f)
			{
				Result(TEXT("air_control_cut_to_0.05_at_slope"), 1.f, MarkF, TEXT("flag"));
				Result(TEXT("normal_air_control"), ArenaTuning::AirControl, MarkF2, TEXT("ratio"), 0.01);
				FinishTest();
			}
		}
		return;
	}
	// ---------------------------------------------------------------------------------- swimming / water jump / drowning
	if (T == TEXT("swim") || T == TEXT("swimmap"))
	{
		if (Phase == 1)
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (PhaseTime > 1.0f)
			{
				if (T == TEXT("swimmap") && WadeDepth > 0.f)
				{
					// UE2: the pawn's Location is not in the water, so it walks (GroundSpeed 440 UU)
					Result(TEXT("wade_speed"), 880.0, Vel.Size(), TEXT("cm/s"), 8.0);
					Result(TEXT("wade_walking"), 1.0, CM->IsMovingOnGround() ? 1.0 : 0.0, TEXT("bool"), 0.1);
				}
				else
				{
					Result(TEXT("swim_speed"), ArenaTuning::WaterSpeed, Vel.Size(), TEXT("cm/s"), 8.0);
				}
				Note(FString::Printf(TEXT("mode %d (4 = swimming), volume %s"), int32(CM->MovementMode.GetValue()), *GetNameSafe(CM->GetPhysicsVolume())));
				if (T == TEXT("swimmap"))
				{
					FinishTest();
					return;
				}
				MarkF = float(CM->WaterJumps);
				NextPhase();
			}
		}
		else if (Phase == 2)
		{
			// swim to the pool edge, then forwards and up (jump held): UE2 jumps out at max(JumpZ, 420) + 2 x CollisionRadius
			C->AddMovementInput(Fwd(), 1.f);
			if (Counter == 0 && Loc.X - Origin.X > PoolEdgeX - Radius - 30.f)
			{
				Counter = 1;
				C->Jump();
			}
			if (CM->WaterJumps > int32(MarkF))
			{
				Result(TEXT("water_jump_out_vz"), 840.f + 2.f * Radius, CM->LastWaterJumpVz, TEXT("cm/s"));
				NextPhase();
			}
			else if (PhaseTime > 6.f)
			{
				Result(TEXT("water_jump_out_vz"), 840.f + 2.f * Radius, 0.f, TEXT("cm/s"));
				FinishTest();
			}
		}
		else if (Phase == 3)
		{
			C->AddMovementInput(Fwd(), 1.f);
			if (PhaseTime > 1.5f)
			{
				Result(TEXT("cleared_the_pool_edge"), 1.f, (MaxZ - Origin.Z > 280.f && Loc.X - Origin.X > PoolEdgeX) ? 1.f : 0.f, TEXT("flag"));
				C->StopJumping();
				FinishTest();
			}
		}
		return;
	}
	if (T == TEXT("drown"))
	{
		// stay under water: UE2 UnderWaterTime 20 s, then 5 damage every 2 s
		const float Hurt = 100.f - C->Health;
		if (Counter == 0 && Hurt > 0.f)
		{
			Counter = 1;
			MarkF = TestTime;
			MarkF2 = Hurt;
		}
		else if (Counter == 1 && Hurt > MarkF2)
		{
			Counter = 2;
			Result(TEXT("first_drown_damage_after"), 20.f, MarkF, TEXT("s"), 0.15);
			Result(TEXT("drown_damage"), 5.f, MarkF2, TEXT("hp"));
			Result(TEXT("next_drown_damage_interval"), 2.f, TestTime - MarkF, TEXT("s"), 0.15);
			FinishTest();
		}
		else if (PhaseTime > 30.f)
		{
			Note(TEXT("no drowning damage"));
			FinishTest();
		}
		return;
	}

	Note(TEXT("unknown test"));
	FinishTest();
}

#endif

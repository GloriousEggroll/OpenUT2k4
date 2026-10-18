#include "ArenaPadShaftTest.h"
#include "OpenUT2k4.h"
#include "ArenaJumpPad.h"
#include "ArenaGameMode.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"

bool UArenaPadShaftTestSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	FString Value;
	return FParse::Value(FCommandLine::Get(), TEXT("-PadShaftTest="), Value, false) && Super::ShouldCreateSubsystem(Outer);
#endif
}

bool UArenaPadShaftTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UArenaPadShaftTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaPadShaftTestSubsystem, STATGROUP_Tickables);
}

void UArenaPadShaftTestSubsystem::StartTrial()
{
	ACharacter* C = Pawn.Get();
	AArenaJumpPad* P = Pad.Get();
	if (!C || !P)
	{
		Phase = 3;
		return;
	}
	const FVector Center = P->GetTriggerCenter();
	const float Angle = 360.f * Trial / NumTrials + 7.f;
	RunDir = -FRotator(0.f, Angle, 0.f).Vector();
	FVector Start = Center - RunDir * 350.f;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaPadShaftTest), false, C);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start + FVector(0, 0, 100.f), Start - FVector(0, 0, 600.f), ECC_Pawn, Q))
	{
		Start.Z = Hit.ImpactPoint.Z + C->GetSimpleCollisionHalfHeight() + 2.f;
	}
	C->TeleportTo(Start, RunDir.Rotation());
	C->GetCharacterMovement()->StopMovementImmediately();
	Phase = 1;
	PhaseTime = 0.f;
	MaxZ = Start.Z;
}

void UArenaPadShaftTestSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || Phase == 3)
	{
		return;
	}
	if (PadName.IsEmpty())
	{
		FString Spec;
		FParse::Value(FCommandLine::Get(), TEXT("-PadShaftTest="), Spec, false);
		FString Count;
		if (!Spec.Split(TEXT(","), &PadName, &Count))
		{
			PadName = Spec;
		}
		else
		{
			NumTrials = FMath::Max(1, FCString::Atoi(*Count));
		}
	}
	const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
	if (!Pad.IsValid() || !Pawn.IsValid())
	{
		if (!GM || !GM->IsPlaying() || World->GetTimeSeconds() < 3.f)
		{
			return;
		}
		for (TActorIterator<AArenaJumpPad> It(World); It; ++It)
		{
			if (It->UE2Name.Equals(PadName, ESearchCase::IgnoreCase))
			{
				Pad = *It;
			}
		}
		for (TActorIterator<ACharacter> It(World); It && !Pawn.IsValid(); ++It)
		{
			if (!It->IsPlayerControlled() && It->GetController())
			{
				Pawn = *It;
				It->GetController()->SetActorTickEnabled(false);
			}
		}
		if (!Pad.IsValid() || !Pawn.IsValid())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("PadShaftTest: no pad %s or no bot"), *PadName);
			Phase = 3;
		}
		return;
	}
	ACharacter* C = Pawn.Get();
	AArenaJumpPad* P = Pad.Get();
	UCharacterMovementComponent* CM = C->GetCharacterMovement();
	PhaseTime += DeltaTime;
	switch (Phase)
	{
	case 0:
		WaitTime -= DeltaTime;
		if (WaitTime <= 0.f)
		{
			if (++Trial >= NumTrials)
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("PadShaftTest: %d of %d flights reached the target"), NumOk, NumTrials);
				Phase = 3;
				return;
			}
			StartTrial();
		}
		break;
	case 1:
		C->AddMovementInput(RunDir, 1.f);
		if (C->GetVelocity().Z > 1000.f)
		{
			TouchLoc = C->GetActorLocation();
			Phase = 2;
			PhaseTime = 0.f;
		}
		else if (PhaseTime > 4.f)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("PadShaftTest: trial %d: never launched"), Trial);
			Phase = 0;
			WaitTime = 0.5f;
		}
		break;
	case 2:
	{
		MaxZ = FMath::Max(MaxZ, float(C->GetActorLocation().Z));
		// steer for the target like a player holding the direction (air control)
		C->AddMovementInput((P->GetTarget() - C->GetActorLocation()).GetSafeNormal2D(), 1.f);
		if ((CM->IsMovingOnGround() && PhaseTime > 0.3f) || PhaseTime > 6.f)
		{
			const FVector Target = P->GetTarget();
			const float Miss = FVector::Dist2D(C->GetActorLocation(), Target);
			const bool bOk = Miss < 400.f && FMath::Abs(C->GetActorLocation().Z - Target.Z) < 200.f;
			NumOk += bOk ? 1 : 0;
			const FVector Rel = TouchLoc - P->GetTriggerCenter();
			UE_LOG(LogOpenUT2k4, Log, TEXT("PadShaftTest: trial %d from %.0f deg: touched at %+.0f,%+.0f from the centre, apex %.0f (target %.0f), landed %.0f from the target at %s: %s"),
				Trial, 360.f * Trial / NumTrials + 7.f, Rel.X, Rel.Y, MaxZ, Target.Z, Miss, *C->GetActorLocation().ToCompactString(), bOk ? TEXT("OK") : TEXT("FAIL"));
			Phase = 0;
			WaitTime = 0.6f;
		}
		break;
	}
	default:
		break;
	}
}

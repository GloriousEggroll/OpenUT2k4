#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaUserSettings.h"
#include "ArenaWeapon.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace ArenaViewShakeLocal
{
	TAutoConsoleVariable<int32> CVarLogShake(TEXT("arena.LogViewShake"), 0, TEXT("Log every view shake started on a local player (1 = on)."));

	constexpr float UU = 2.f;
	constexpr float RotToDeg = 360.f / 65536.f;

	AArenaPlayerCameraManager* CameraOf(const AController* C)
	{
		const APlayerController* PC = Cast<APlayerController>(C);
		return PC && PC->IsLocalController() ? Cast<AArenaPlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
	}

	/** UE2 PlayerController.CheckShake. */
	void CheckShake(double& MaxOffset, double& Offset, double& Rate, double& Time, float Dt)
	{
		if (FMath::Abs(Offset) < FMath::Abs(MaxOffset))
		{
			return;
		}
		Offset = MaxOffset;
		if (Time > 1.0)
		{
			// Rate 0 gives an infinite MaxOffset / Rate in UE2 (the test fails)
			if (Rate != 0.0 && Time * FMath::Abs(MaxOffset / Rate) <= 1.0)
			{
				MaxOffset = MaxOffset * (1.0 / Time - 1.0);
			}
			else
			{
				MaxOffset *= -1.0;
			}
			Time -= Dt;
			Rate *= -1.0;
		}
		else
		{
			MaxOffset = 0.0;
			Offset = 0.0;
			Rate = 0.0;
		}
	}

	/** UE2 PlayerController.UpdateShakeRotComponent (the rotator component is an int, wrapped to +-32768). */
	void UpdateShakeRotComponent(double& Max, double& Current, double& Rate, double& Time, float Dt)
	{
		int32 Cur = (int32(Current) & 65535) + int32(Rate * Dt);
		Cur &= 65535;
		if (Cur > 32768)
		{
			Cur -= 65536;
		}
		double F = double(Cur);
		CheckShake(Max, F, Rate, Time, Dt);
		Current = double(int32(F));
	}
}

// ---------------------------------------------------------------------------------------------
// Camera manager
// ---------------------------------------------------------------------------------------------

void AArenaPlayerCameraManager::ShakeView(const FArenaViewShake& S)
{
	if (S.RotMag.Size() > ShakeRotMax.Size())
	{
		ShakeRotMax = S.RotMag;
		ShakeRotRate = S.RotRate;
		ShakeRotTime = FVector(S.RotTime);
	}
	if (S.OffsetMag.Size() > ShakeOffsetMax.Size())
	{
		ShakeOffsetMax = S.OffsetMag;
		ShakeOffsetRate = S.OffsetRate;
		ShakeOffsetTime = FVector(S.OffsetTime);
	}
}

void AArenaPlayerCameraManager::StopViewShaking()
{
	ShakeRotMax = ShakeRotRate = ShakeRotTime = FVector::ZeroVector;
	ShakeOffsetMax = ShakeOffsetRate = ShakeOffsetTime = FVector::ZeroVector;
}

void AArenaPlayerCameraManager::ViewShake(float DeltaTime)
{
	using namespace ArenaViewShakeLocal;
	if (!ShakeOffsetRate.IsZero())
	{
		for (int32 i = 0; i < 3; ++i)
		{
			ShakeOffsetUU[i] += DeltaTime * ShakeOffsetRate[i];
			CheckShake(ShakeOffsetMax[i], ShakeOffsetUU[i], ShakeOffsetRate[i], ShakeOffsetTime[i], DeltaTime);
		}
	}
	if (!ShakeRotRate.IsZero())
	{
		for (int32 i = 0; i < 3; ++i)
		{
			UpdateShakeRotComponent(ShakeRotMax[i], ShakeRotUU[i], ShakeRotRate[i], ShakeRotTime[i], DeltaTime);
		}
	}
}

void AArenaPlayerCameraManager::AddAmbientOffset(const FVector& OffsetUU)
{
	if (OffsetUU.SizeSquared() > AmbientOffsetUU.SizeSquared())
	{
		AmbientOffsetUU = OffsetUU;
	}
}

void AArenaPlayerCameraManager::UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime)
{
	using namespace ArenaViewShakeLocal;
	Super::UpdateViewTarget(OutVT, DeltaTime);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// UE2 runs ViewShake in PlayerTick: once per unpaused game frame
	const double Now = World->GetTimeSeconds();
	if (!World->IsPaused() && Now != LastShakeWorldTime)
	{
		const float Dt = LastShakeWorldTime < 0.0 ? 0.f : float(Now - LastShakeWorldTime);
		LastShakeWorldTime = Now;
		if (Dt > 0.f)
		{
			ViewShake(FMath::Min(Dt, 0.1f));
		}
	}
	const FVector Ambient = AmbientOffsetUU;
	AmbientOffsetUU = FVector::ZeroVector;
	if (ShakeRotUU.IsZero() && ShakeOffsetUU.IsZero() && Ambient.IsZero())
	{
		return;
	}
	// CalcFirstPersonView / Vehicle.SpecialCalc*View: only the player's own pawn or vehicle
	const APlayerController* PC = GetOwningPlayerController();
	const APawn* Own = PC ? PC->GetPawn() : nullptr;
	AActor* Target = OutVT.Target.Get();
	const bool bOwnView = Target && (Target == Own || (PC && Target == ArenaONS::GetControlledVehicle(PC)));
	const AArenaCharacter* Char = Cast<AArenaCharacter>(Own);
	if (!bOwnView || (Char && Char->IsDead()))
	{
		return;
	}
	const FRotator Base = OutVT.POV.Rotation;
	const FRotationMatrix M(Base);
	const FVector Off = ShakeOffsetUU + Ambient;
	OutVT.POV.Location += (M.GetUnitAxis(EAxis::X) * Off.X + M.GetUnitAxis(EAxis::Y) * Off.Y + M.GetUnitAxis(EAxis::Z) * Off.Z) * UU;
	OutVT.POV.Rotation = (Base + FRotator(ShakeRotUU.X * RotToDeg, ShakeRotUU.Y * RotToDeg, ShakeRotUU.Z * RotToDeg)).GetNormalized();
}

// ---------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------

void ArenaViewShake::ShakeView(AController* Controller, const FArenaViewShake& Shake)
{
	using namespace ArenaViewShakeLocal;
	if (AArenaPlayerCameraManager* Cam = CameraOf(Controller))
	{
		if (CVarLogShake.GetValueOnGameThread() != 0)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("ViewShake: %s rot %s rate %s time %.2f offset %s rate %s time %.2f"), *GetNameSafe(Controller),
				*Shake.RotMag.ToCompactString(), *Shake.RotRate.ToCompactString(), Shake.RotTime, *Shake.OffsetMag.ToCompactString(),
				*Shake.OffsetRate.ToCompactString(), Shake.OffsetTime);
		}
		Cam->ShakeView(Shake);
	}
}

void ArenaViewShake::WeaponShakeView(AController* Controller, const FArenaViewShake& Shake)
{
	if (Shake.IsZero() || !UArenaUserSettings::Get()->bWeaponViewShake || !ArenaViewShakeLocal::CameraOf(Controller))
	{
		return;
	}
	// ZoomLevel: UE2 DesiredFOV = DefaultFOV - ZoomLevel x 88
	float ZoomLevel = 0.f;
	if (const AArenaCharacter* Char = Cast<AArenaCharacter>(Controller->GetPawn()))
	{
		if (const AArenaWeapon* W = Char->GetCurrentWeapon())
		{
			const float Zoom = W->GetZoomFOV();
			if (Zoom > 0.f)
			{
				ZoomLevel = FMath::Clamp((90.f - Zoom) / 88.f, 0.f, 1.f);
			}
		}
	}
	ShakeView(Controller, Shake.Scaled(1.f - ZoomLevel));
}

void ArenaViewShake::DamageShake(AController* Controller, int32 Damage)
{
	if (Damage <= 0)
	{
		return;
	}
	const float D = float(Damage);
	ShakeView(Controller, FArenaViewShake(FVector(30.f * D, 0.f, 0.f), FVector(120000.f, 0.f, 0.f), 0.15f + 0.005f * D,
		FVector(0.f, 0.f, 0.03f * D), FVector(1.f, 1.f, 1.f), 0.2f));
}

int32 ArenaViewShake::ShakeViewersNear(UWorld* World, const FVector& Location, float Radius, float Falloff, const FArenaViewShake& Shake, bool bRequireBase)
{
	int32 N = 0;
	if (!World || Radius <= 0.f)
	{
		return 0;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		const AActor* VT = PC ? PC->GetViewTarget() : nullptr;
		if (!VT || !PC->IsLocalController())
		{
			continue;
		}
		if (bRequireBase)
		{
			const ACharacter* C = Cast<ACharacter>(VT);
			if (!C || !Cast<UPrimitiveComponent>(C->GetMovementBaseObject()))
			{
				continue;
			}
		}
		const float Dist = FVector::Dist(Location, VT->GetActorLocation());
		if (Dist < Radius * Falloff)
		{
			const float Scale = Dist < Radius ? 1.f : (Radius * Falloff - Dist) / Radius;
			ShakeView(PC, Shake.Scaled(Scale));
			++N;
		}
	}
	return N;
}

void ArenaViewShake::ProximityShake(UWorld* World, const AActor* Source, const FVector& Location, float Radius, const FVector& OffsetMag, float Freq)
{
	using namespace ArenaViewShakeLocal;
	if (!World || Radius <= 0.f)
	{
		return;
	}
	const float S = FMath::Sin(float(World->GetTimeSeconds()) * Freq * 2.f * PI);
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		const AActor* VT = PC ? PC->GetViewTarget() : nullptr;
		AArenaPlayerCameraManager* Cam = PC && PC->IsLocalController() ? Cast<AArenaPlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
		if (!VT || !Cam || VT == Source || (PC->GetPawn() && ArenaONS::GetControlledVehicle(PC) == Source))
		{
			continue;
		}
		const float Dist = FVector::Dist(VT->GetActorLocation(), Location);
		if (Dist < Radius)
		{
			Cam->AddAmbientOffset(OffsetMag * S * (1.f - Dist / Radius));
		}
	}
}

bool ArenaViewShake::GetCurrentShake(const AController* Controller, FRotator& OutRot, FVector& OutOffset)
{
	using namespace ArenaViewShakeLocal;
	const AArenaPlayerCameraManager* Cam = CameraOf(Controller);
	if (!Cam)
	{
		return false;
	}
	OutRot = FRotator(Cam->ShakeRotUU.X * RotToDeg, Cam->ShakeRotUU.Y * RotToDeg, Cam->ShakeRotUU.Z * RotToDeg);
	OutOffset = Cam->ShakeOffsetUU * UU;
	return true;
}

// UT2004 defaults (read from the stock XWeapons / Onslaught / UTClassic class defaults)
FArenaViewShake ArenaViewShake::Blast()
{
	return FArenaViewShake(FVector(0, 0, 250), FVector(0, 0, 2500), 6.f, FVector(0, 0, 10), FVector(0, 0, 200), 10.f);
}
FArenaViewShake ArenaViewShake::KickBack()
{
	return FArenaViewShake(FVector::ZeroVector, FVector::ZeroVector, 2.f, FVector(-20, 0, 0), FVector(-1000, 0, 0), 2.f);
}
FArenaViewShake ArenaViewShake::AssaultFire()
{
	return FArenaViewShake(FVector(50, 50, 50), FVector(10000, 10000, 10000), 2.f, FVector(1, 1, 1), FVector(1000, 1000, 1000), 2.f);
}
FArenaViewShake ArenaViewShake::BioFire()
{
	return FArenaViewShake(FVector(70, 0, 0), FVector(1000, 0, 0), 1.8f, FVector(0, 0, -2), FVector(0, 0, 1000), 1.8f);
}
FArenaViewShake ArenaViewShake::BioChargedFire()
{
	return FArenaViewShake(FVector(100, 0, 0), FVector(1000, 0, 0), 2.f, FVector(-4, 0, -4), FVector(1000, 0, 1000), 2.f);
}
FArenaViewShake ArenaViewShake::ShockBeamFire()
{
	return FArenaViewShake(FVector::ZeroVector, FVector::ZeroVector, 2.f, FVector(-8, 0, 0), FVector(-600, 0, 0), 3.2f);
}
FArenaViewShake ArenaViewShake::ShockProjFire()
{
	return FArenaViewShake(FVector(60, 20, 0), FVector(1000, 1000, 0), 2.f, FVector::ZeroVector, FVector::ZeroVector, 0.f);
}
FArenaViewShake ArenaViewShake::LinkAltFire()
{
	return FArenaViewShake(FVector(40, 0, 0), FVector(2000, 0, 0), 2.f, FVector(0, 1, 0), FVector(0, -2000, 0), 4.f);
}
FArenaViewShake ArenaViewShake::LinkFire()
{
	return FArenaViewShake(FVector(0, 0, 60), FVector(0, 0, 4000), 6.f, FVector(0, 1, 1), FVector(1000, 1000, 1000), 3.f);
}
FArenaViewShake ArenaViewShake::SniperFire()
{
	return FArenaViewShake(FVector::ZeroVector, FVector::ZeroVector, 2.f, FVector(-15, 0, 10), FVector(-4000, 0, 4000), 1.6f);
}

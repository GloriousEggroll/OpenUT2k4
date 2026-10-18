// View shake, falling physics and vehicle volume checks of the -MoveTest hook (non-shipping): w_shake and w_fall.
// Expected values are UT2004's (Engine.PlayerController ShakeView / ViewShake / CheckShake / ClientDamageShake,
// XWeapons RedeemerProjectile / RocketFire shake defaults, PhysicsVolume TerminalVelocity 2500, KarmaParams gravity).
#include "ArenaMoveTest.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaProjectile.h"
#include "ArenaTypes.h"
#include "ArenaUserSettings.h"
#include "ArenaViewShake.h"
#include "ArenaWeaponActors.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaWorldZones.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"

#if !UE_BUILD_SHIPPING

namespace ArenaMoveTestGapsLocal
{
	constexpr float UU = 2.f;
	constexpr float RotToDeg = 360.f / 65536.f;

	AArenaPlayerCameraManager* Cam(AArenaCharacter* C)
	{
		const APlayerController* PC = C ? Cast<APlayerController>(C->GetController()) : nullptr;
		return PC ? Cast<AArenaPlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
	}

	/** A physics volume box (UE2 values: Gravity in UU/s^2, ZoneVelocity in UU/s) for the vehicle checks. */
	AArenaPhysicsVolume* SpawnVolume(UWorld* World, const FVector& Center, const FVector& HalfSize)
	{
		TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Bw = MakeShared<FJsonObject>();
		auto Arr = [](const FVector& V)
		{
			TArray<TSharedPtr<FJsonValue>> A;
			A.Add(MakeShared<FJsonValueNumber>(V.X));
			A.Add(MakeShared<FJsonValueNumber>(V.Y));
			A.Add(MakeShared<FJsonValueNumber>(V.Z));
			return A;
		};
		Bw->SetArrayField(TEXT("min"), Arr(Center - HalfSize));
		Bw->SetArrayField(TEXT("max"), Arr(Center + HalfSize));
		Marker->SetObjectField(TEXT("bounds_world"), Bw);
		const FTransform TM(FRotator::ZeroRotator, Center);
		AArenaPhysicsVolume* V = World->SpawnActorDeferred<AArenaPhysicsVolume>(AArenaPhysicsVolume::StaticClass(), TM);
		if (V)
		{
			V->Shape.Load(Marker);
			V->UE2Name = TEXT("MoveTestVehicleVolume");
			V->Priority = 10;
			V->FinishSpawning(TM);
		}
		return V;
	}
}

bool UArenaMoveTestSubsystem::TickGapsTest(const FString& T, float DeltaTime)
{
	using namespace ArenaMoveTestGapsLocal;
	AArenaCharacter* C = Pawn.Get();
	UWorld* World = GetWorld();
	auto NextSub = [this]() { ++SubPhase; SubTime = 0.f; };
	AArenaPlayerCameraManager* PCM = Cam(C);
	FRotator SRot;
	FVector SOff;
	const bool bHaveShake = ArenaViewShake::GetCurrentShake(C->GetController(), SRot, SOff);

	// ---------------------------------------------------------------- view shake
	if (T == TEXT("w_shake"))
	{
		if (!PCM || !bHaveShake)
		{
			Result(TEXT("camera_manager_runs_ue2_view_shake"), 1.0, 0.0, TEXT(""));
			FinishTest();
			return true;
		}
		switch (SubPhase)
		{
		case 0:
			ResetPawn(Origin + FVector(0.f, 0.f, 0.f), 0.f);
			UArenaUserSettings::Get()->bWeaponViewShake = true;
			PCM->StopViewShaking();
			PCM->ShakeRotUU = PCM->ShakeOffsetUU = FVector::ZeroVector;
			NextSub();
			break;
		case 1:
			if (SubTime > 0.5f)
			{
				// RedeemerProjectile.Dying.ShakeView at point blank: ShakeRotMag (0,0,250) rate 2500 time 6, offset (0,0,10) rate 200 time 10
				ArenaViewShake::ShakeView(C->GetController(), ArenaViewShake::Blast());
				MarkF = 0.f;      // max roll
				MarkF2 = 0.f;     // min roll
				Mark = FVector::ZeroVector; // x: max offset z, y: max camera roll difference
				NextSub();
			}
			break;
		case 2:
			MarkF = FMath::Max(MarkF, float(SRot.Roll));
			MarkF2 = FMath::Min(MarkF2, float(SRot.Roll));
			Mark.X = FMath::Max(Mark.X, SOff.Z);
			if (APlayerController* PC = Cast<APlayerController>(C->GetController()))
			{
				const float CamRoll = FRotator::NormalizeAxis(PCM->GetCameraRotation().Roll - PC->GetControlRotation().Roll);
				if (FMath::Abs(SRot.Roll) > 0.3f)
				{
					Mark.Y = FMath::Max(Mark.Y, FMath::Abs(CamRoll - float(SRot.Roll)) < 0.05f ? 1.0 : 0.0);
				}
			}
			if (SubTime > 0.4f)
			{
				// first swing to +ShakeRotMag, then CheckShake: Time x |Max / Rate| = 0.6 <= 1 -> Max x (1/Time - 1) = -208.3
				Result(TEXT("blast_roll_first_peak"), 250.f * RotToDeg, MarkF, TEXT("deg"), 0.02);
				Result(TEXT("blast_roll_second_peak"), 250.f * (1.f / 6.f - 1.f) * RotToDeg, MarkF2, TEXT("deg"), 0.03);
				Result(TEXT("blast_offset_z_peak"), 10.f * UU, Mark.X, TEXT("cm"), 0.5);
				Result(TEXT("camera_view_rolls_with_the_shake"), 1.0, Mark.Y, TEXT(""));
				NextSub();
			}
			break;
		case 3:
			// the shake runs out (Time counts down by dt at every swing; offset Z lasts longest: about 10 s)
			if (SRot.IsNearlyZero(1e-4) && SOff.IsNearlyZero(1e-3))
			{
				Result(TEXT("blast_shake_ends"), 1.0, 1.0, TEXT(""));
				Note(FString::Printf(TEXT("blast shake lasted %.1f s"), SubTime + 0.4f));
				NextSub();
			}
			else if (SubTime > 20.f)
			{
				Result(TEXT("blast_shake_ends"), 1.0, 0.0, TEXT(""));
				NextSub();
			}
			break;
		case 4:
		{
			// RocketFire: ShakeOffsetMag (-20,0,0) rate (-1000,0,0) time 2 (WeaponShakeView)
			C->GiveWeapon(AWeap_RocketLauncher::StaticClass());
			AArenaWeapon* W = C->FindWeapon(AWeap_RocketLauncher::StaticClass());
			if (W)
			{
				W->AddAmmo(W->MaxAmmo);
				C->SwitchToWeapon(W);
			}
			C->GetController()->SetControlRotation(FRotator(30.f, 45.f, 0.f));
			Mark = FVector::ZeroVector;
			NextSub();
			break;
		}
		case 5:
			if (SubTime > 1.2f)
			{
				C->StartFire(0);
				C->StopFire(0);
				NextSub();
			}
			break;
		case 6:
			Mark.X = FMath::Min(Mark.X, SOff.X);
			if (SubTime > 0.3f)
			{
				Result(TEXT("rocket_fire_kick_offset_x"), -20.f * UU, Mark.X, TEXT("cm"), 0.5);
				// "Weapons Shake View" off: no weapon shake
				UArenaUserSettings::Get()->bWeaponViewShake = false;
				PCM->StopViewShaking();
				PCM->ShakeRotUU = PCM->ShakeOffsetUU = FVector::ZeroVector;
				Mark = FVector::ZeroVector;
				NextSub();
			}
			break;
		case 7:
			if (SubTime > 1.f)
			{
				C->StartFire(0);
				C->StopFire(0);
				NextSub();
			}
			break;
		case 8:
			Mark.X = FMath::Min(Mark.X, SOff.X);
			if (SubTime > 0.3f)
			{
				Result(TEXT("setting_off_no_weapon_shake"), 0.0, Mark.X, TEXT("cm"), 0.01);
				UArenaUserSettings::Get()->bWeaponViewShake = true;
				// UnrealPlayer.NotifyTakeHit -> ClientDamageShake(10): offset (0,0,0.3) UU at 1 UU/s (time 0.2), the 300
				// unit pitch is reached and dropped in the same update (ShakeRotTime 0.2 < 1)
				FArenaHitInfo Info;
				Info.WeaponName = TEXT("MoveTest");
				C->Health = 100.f;
				C->ReceiveArenaDamage(10.f, nullptr, nullptr, Info);
				Mark = FVector::ZeroVector;
				MarkF = 0.f;
				NextSub();
			}
			break;
		case 9:
			Mark.X = FMath::Max(Mark.X, SOff.Z);
			MarkF = FMath::Max(MarkF, FMath::Abs(float(SRot.Pitch)));
			if (SubTime > 0.6f)
			{
				// it creeps up at 1 UU/s and is dropped in the frame it reaches 0.3 UU: the last value seen is up to one frame short
				Result(TEXT("damage_shake_offset_z_peak"), 0.3f * UU, Mark.X, TEXT("cm"), 0.25);
				Result(TEXT("damage_shake_no_visible_pitch"), 0.0, MarkF, TEXT("deg"), 0.01);
				PCM->StopViewShaking();
				PCM->ShakeRotUU = PCM->ShakeOffsetUU = FVector::ZeroVector;
				// blast 1.5 x DamageRadius away: Scale (2R - 1.5R) / R = 0.5
				const float R = 2000.f * UU;
				Counter = ArenaViewShake::ShakeViewersNear(World, C->GetActorLocation() + FVector(1.5f * R, 0.f, 0.f), R, 2.f, ArenaViewShake::Blast());
				MarkF = 0.f;
				NextSub();
			}
			break;
		case 10:
			MarkF = FMath::Max(MarkF, float(SRot.Roll));
			if (SubTime > 0.3f)
			{
				Result(TEXT("blast_at_1.5_radius_half_roll"), 125.f * RotToDeg, MarkF, TEXT("deg"), 0.02);
				PCM->StopViewShaking();
				PCM->ShakeRotUU = PCM->ShakeOffsetUU = FVector::ZeroVector;
				const float R = 2000.f * UU;
				Counter = ArenaViewShake::ShakeViewersNear(World, C->GetActorLocation() + FVector(2.1f * R, 0.f, 0.f), R, 2.f, ArenaViewShake::Blast());
				Result(TEXT("blast_beyond_2_radius_no_shake"), 0.0, Counter, TEXT(""));
				FinishTest();
			}
			break;
		default:
			FinishTest();
			break;
		}
		return true;
	}

	// ---------------------------------------------------------------- terminal velocity, vehicles in volumes
	if (T == TEXT("w_fall"))
	{
		const float TV = ArenaWorldZones::GetTerminalVelocityAt(World, Origin + FVector(0.f, 0.f, 5000.f));
		switch (SubPhase)
		{
		case 0:
		{
			// the level's DefaultPhysicsVolume: TerminalVelocity 2500 UU/s
			Result(TEXT("default_terminal_velocity"), 2500.f * UU, TV, TEXT("cm/s"), 1.0);
			ResetPawn(Origin + FVector(1500.f, 1500.f, 0.f), 0.f);
			C->GiveWeapon(AWeap_GooLauncher::StaticClass());
			if (AArenaWeapon* W = C->FindWeapon(AWeap_GooLauncher::StaticClass()))
			{
				W->AddAmmo(W->MaxAmmo);
				C->SwitchToWeapon(W);
			}
			C->GetController()->SetControlRotation(FRotator(80.f, 0.f, 0.f));
			TrackedActor.Reset();
			NextSub();
			break;
		}
		case 1:
			if (SubTime > 1.2f)
			{
				C->StartFire(0);
				C->StopFire(0);
				NextSub();
			}
			break;
		case 2:
			// a bio glob (UE2 PHYS_Falling) dropped from high up at 4000 cm/s: one second later it falls at the terminal
			// velocity, not 4000 + 1900 cm/s
			if (!TrackedActor.IsValid())
			{
				for (TActorIterator<AArenaProjectile> It(World); It; ++It)
				{
					if (It->GetInstigator() == C && It->Movement && It->Movement->ProjectileGravityScale != 0.f)
					{
						TrackedActor = *It;
						It->SetActorLocation(Origin + FVector(-2500.f, 2500.f, 30000.f), false, nullptr, ETeleportType::TeleportPhysics);
						It->Movement->Velocity = FVector(0.f, 0.f, -4000.f);
						It->SetLifeSpan(10.f);
						SubTime = 0.f;
						break;
					}
				}
				if (!TrackedActor.IsValid() && SubTime > 2.f)
				{
					Result(TEXT("bio_glob_found"), 1.0, 0.0, TEXT(""));
					NextSub();
				}
			}
			else if (SubTime > 1.f)
			{
				const AArenaProjectile* P = Cast<AArenaProjectile>(TrackedActor.Get());
				Result(TEXT("bio_glob_terminal_velocity"), -2500.f * UU, P ? float(P->Movement->Velocity.Z) : 0.f, TEXT("cm/s"), 20.0);
				TrackedActor->Destroy();
				TrackedActor.Reset();
				NextSub();
			}
			break;
		case 3:
		{
			C->GiveWeapon(AWeap_StickyLauncher::StaticClass());
			if (AArenaWeapon* W = C->FindWeapon(AWeap_StickyLauncher::StaticClass()))
			{
				W->AddAmmo(W->MaxAmmo);
				C->SwitchToWeapon(W);
			}
			NextSub();
			break;
		}
		case 4:
			if (SubTime > 1.2f)
			{
				C->StartFire(0);
				C->StopFire(0);
				NextSub();
			}
			break;
		case 5:
			// an ONS grenade (moves itself, PHYS_Falling) thrown up and dropped from high up: the fall is limited too
			if (!TrackedActor.IsValid())
			{
				TActorIterator<AArenaStickyGrenade> It(World);
				if (It)
				{
					TrackedActor = *It;
					It->SetActorLocation(Origin + FVector(2500.f, -2500.f, 30000.f));
					TrackedLoc = It->GetActorLocation();
					SubTime = 0.f;
				}
				if (!TrackedActor.IsValid() && SubTime > 2.f)
				{
					Result(TEXT("ons_grenade_found"), 1.0, 0.0, TEXT(""));
					NextSub();
				}
			}
			else
			{
				const FVector L = TrackedActor->GetActorLocation();
				const float Vz = DeltaTime > 0.f ? float(L.Z - TrackedLoc.Z) / DeltaTime : 0.f;
				TrackedLoc = L;
				if (SubTime > 5.f)
				{
					// thrown up at ~2400 cm/s: unlimited it would fall at 2400 - 1900 x 5 = -7100 cm/s
					Result(TEXT("ons_grenade_terminal_velocity"), -2500.f * UU, Vz, TEXT("cm/s"), 40.0);
					TrackedActor->Destroy();
					TrackedActor.Reset();
					NextSub();
				}
			}
			break;
		case 6:
		{
			// a vehicle in a volume with sideways gravity (UE2 Gravity (300, 0, -950)): the whole vector pulls it
			const FVector Center = Origin + FVector(0.f, 0.f, 60000.f);
			AArenaPhysicsVolume* V = SpawnVolume(World, Center, FVector(20000.f, 20000.f, 20000.f));
			if (!V)
			{
				Result(TEXT("vehicle_volume_spawned"), 1.0, 0.0, TEXT(""));
				FinishTest();
				break;
			}
			V->Gravity = FVector(300.f, 0.f, -950.f);
			TestActors.Add(V);
			const FTransform Xf(FRotator::ZeroRotator, Center);
			AArenaONS_Vehicle* Veh = World->SpawnActorDeferred<AArenaONS_Vehicle>(AArenaONS_Vehicle::StaticClass(), Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Veh)
			{
				Veh->InitVehicle(EArenaONS_VehicleType::Dustrunner, 0, nullptr);
				Veh->FinishSpawning(Xf);
				TestActors.Add(Veh);
			}
			TrackedActor = Veh;
			NextSub();
			break;
		}
		case 7:
		{
			AArenaONS_Vehicle* Veh = Cast<AArenaONS_Vehicle>(TrackedActor.Get());
			if (!Veh || !Veh->Body)
			{
				Result(TEXT("vehicle_spawned"), 1.0, 0.0, TEXT(""));
				FinishTest();
				break;
			}
			if (SubTime < 0.3f)
			{
				Mark = Veh->Body->GetPhysicsLinearVelocity();
				MarkF = SubTime;
			}
			else if (SubTime > 1.3f)
			{
				const FVector V1 = Veh->Body->GetPhysicsLinearVelocity();
				const float Dt = SubTime - MarkF;
				const float Ax = float(V1.X - Mark.X) / FMath::Max(Dt, 0.01f);
				const float Az = float(V1.Z - Mark.Z) / FMath::Max(Dt, 0.01f);
				// GetGravityVector: Gravity x (980 / 950) cm/s^2, x the vehicle's KActorGravScale (spec GravityScale). The
				// physics step is capped at 1/30 s, so under 30 fps the body moves slower than world time: the check is
				// the sideways pull against the downward one (UE2 Gravity X / Z = 300 / 950), which does not depend on that
				const float Expected = 300.f * (980.f / 950.f) * Veh->GetSpec().GravityScale;
				Note(FString::Printf(TEXT("vehicle accel x %.0f z %.0f cm/s2 (expected x %.0f at full frame rate)"), Ax, Az, Expected));
				Result(TEXT("vehicle_sideways_to_down_gravity_ratio"), 300.f / 950.f, Az < -1.f ? Ax / -Az : 0.f, TEXT(""), 0.03);
				// then a volume with ZoneVelocity (500, 0, 0) UU/s and normal gravity: carried along X
				for (const TWeakObjectPtr<AActor>& A : TestActors)
				{
					if (AArenaPhysicsVolume* PV = Cast<AArenaPhysicsVolume>(A.Get()))
					{
						PV->Gravity = FVector(0.f, 0.f, -950.f);
						PV->ZoneVelocity = FVector(500.f * UU, 0.f, 0.f);
					}
				}
				Veh->Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
				Veh->SetActorLocation(Origin + FVector(0.f, 0.f, 60000.f), false, nullptr, ETeleportType::TeleportPhysics);
				Veh->Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
				TrackedLoc = Veh->GetActorLocation();
				NextSub();
			}
			break;
		}
		case 8:
		{
			AArenaONS_Vehicle* Veh = Cast<AArenaONS_Vehicle>(TrackedActor.Get());
			if (!Veh)
			{
				FinishTest();
				break;
			}
			if (SubTime > 1.f)
			{
				// the X move comes from the zone velocity alone (no gravity along X): 1000 cm/s x 1 s, the velocity stays 0
				const float Moved = float(Veh->GetActorLocation().X - TrackedLoc.X);
				Result(TEXT("vehicle_zone_velocity_move"), 500.f * UU * SubTime, Moved, TEXT("cm"), 500.f * UU * SubTime * 0.1f);
				FinishTest();
			}
			break;
		}
		default:
			FinishTest();
			break;
		}
		return true;
	}
	// ---------------------------------------------------------------- vehicles at rest (flat and on a 15 degree slope)
	if (T == TEXT("w_vehrest"))
	{
		// UE2 ONSWheeledCraft / ONSHoverCraft StopThreshold: an empty vehicle on the ground stays put
		static const EArenaONS_VehicleType Types[] = { EArenaONS_VehicleType::Dustrunner, EArenaONS_VehicleType::Caravan, EArenaONS_VehicleType::Bulwark,
			EArenaONS_VehicleType::Wisp };
		const int32 NumCases = UE_ARRAY_COUNT(Types) * 2;
		if (SubIndex >= NumCases)
		{
			FinishTest();
			return true;
		}
		const EArenaONS_VehicleType Type = Types[SubIndex / 2];
		const bool bSlope = (SubIndex % 2) == 1;
		const FVector FlatSpot = Origin + FVector(3000.f, 1000.f, 0.f);
		const FVector SlopeCenter = Origin + FVector(3000.f, 3300.f, 300.f);
		const FRotator SlopeRot(15.f, 0.f, 0.f);
		if (SubPhase == 0)
		{
			if (bSlope && !TrackedActor.IsValid() && SubIndex == 1)
			{
				TestActors.Add(SpawnBox(SlopeCenter, FVector(1600.f, 1000.f, 50.f), SlopeRot, false));
			}
			const FVector Up = SlopeRot.RotateVector(FVector::UpVector);
			const FVector At = bSlope ? SlopeCenter + Up * 200.f : FlatSpot + FVector(0.f, 0.f, 200.f);
			const FTransform Xf(bSlope ? SlopeRot : FRotator::ZeroRotator, At);
			AArenaONS_Vehicle* Veh = World->SpawnActorDeferred<AArenaONS_Vehicle>(AArenaONS_Vehicle::StaticClass(), Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Veh)
			{
				Veh->InitVehicle(Type, 0, nullptr);
				Veh->FinishSpawning(Xf);
			}
			TrackedActor = Veh;
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 3.f)
		{
			TrackedLoc = TrackedActor.IsValid() ? TrackedActor->GetActorLocation() : FVector::ZeroVector;
			NextSub();
		}
		else if (SubPhase == 2 && SubTime > 3.f)
		{
			AArenaONS_Vehicle* Veh = Cast<AArenaONS_Vehicle>(TrackedActor.Get());
			const float Drift = Veh ? float(FVector::Dist2D(Veh->GetActorLocation(), TrackedLoc)) / SubTime : 999.f;
			Result(*FString::Printf(TEXT("%s_%s_rest_drift"), Veh ? *Veh->GetVehicleName().ToLower() : TEXT("vehicle"), bSlope ? TEXT("slope15") : TEXT("flat")), 0.0, Drift,
				TEXT("cm/s"), 2.0);
			if (Veh)
			{
				Veh->Destroy();
			}
			TrackedActor.Reset();
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}
	// ---------------------------------------------------------------- partial (analog) input: UE2 has no speed cap
	if (T == TEXT("w_analog"))
	{
		// UE2 APawn::calcVelocity: a stick held at 60% accelerates more slowly but reaches GroundSpeed / WaterSpeed /
		// AirSpeed (UE5's AnalogInputModifier would cap it at 60%)
		UCharacterMovementComponent* CM = C->GetCharacterMovement();
		static const TCHAR* Names[] = { TEXT("walk"), TEXT("swim"), TEXT("fly") };
		if (SubIndex >= 3)
		{
			CM->SetMovementMode(MOVE_Walking);
			FinishTest();
			return true;
		}
		if (SubPhase == 0)
		{
			if (SubIndex == 0)
			{
				ResetPawn(Origin + FVector(1500.f, -500.f, 0.f), 90.f);
			}
			else if (SubIndex == 1)
			{
				// the rig's water pool (X -1600 .. -500, Y -3500 .. -2500, 260 cm deep): swim along +X at mid depth
				ResetPawn(Origin + FVector(-1550.f, -3000.f, 40.f), 0.f);
				CM->SetMovementMode(MOVE_Swimming);
			}
			else
			{
				ResetPawn(Origin + FVector(1500.f, 2000.f, 1500.f), 0.f);
				CM->SetMovementMode(MOVE_Flying);
			}
			CM->Velocity = FVector::ZeroVector;
			MarkF = 0.f;
			NextSub();
		}
		else if (SubPhase == 1)
		{
			const FVector Dir = FRotator(0.f, SubIndex == 0 ? 90.f : 0.f, 0.f).Vector();
			C->AddMovementInput(Dir, 0.6f);
			if (SubIndex == 1 && !CM->IsSwimming())
			{
				CM->SetMovementMode(MOVE_Swimming);
			}
			if (SubIndex == 2 && CM->MovementMode != MOVE_Flying)
			{
				CM->SetMovementMode(MOVE_Flying);
			}
			MarkF = FMath::Max(MarkF, float(CM->Velocity.Size2D()));
			if (SubTime > (SubIndex == 1 ? 1.6f : 2.f))
			{
				Result(*FString::Printf(TEXT("%s_top_speed_at_60pct_input"), Names[SubIndex]), CM->GetMaxSpeed(), MarkF, TEXT("cm/s"), CM->GetMaxSpeed() * 0.02);
				CM->SetMovementMode(MOVE_Falling);
				++SubIndex;
				SubPhase = 0;
				SubTime = 0.f;
			}
		}
		return true;
	}
	// ---------------------------------------------------------------- UE2 stepUp against a steep, not vertical face
	if (T == TEXT("w_stepup"))
	{
		// a face leaning back 8.6 degrees (normal Z 0.15, like DM-Rankin's door frame legs): UE2 physWalking steps up
		// MaxStepHeight (35 UU), cannot stand on the face and falls (a UT2004 server's pawn rises 36 UU and goes airborne)
		UCharacterMovementComponent* CM = C->GetCharacterMovement();
		const FVector BoxCenter = Origin + FVector(-3600.f, 3000.f, 200.f);
		if (SubPhase == 0)
		{
			TestActors.Add(SpawnBox(BoxCenter, FVector(200.f, 800.f, 400.f), FRotator(8.63f, 0.f, 0.f), false));
			ResetPawn(Origin + FVector(-3300.f, 3000.f, 0.f), 180.f);
			MarkF = 0.f;
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 0.5f)
		{
			StartFeetZ = C->GetActorLocation().Z;
			NextSub();
		}
		else if (SubPhase == 2)
		{
			C->AddMovementInput(FVector(-1.f, 0.f, 0.f), 1.f);
			MarkF = FMath::Max(MarkF, float(C->GetActorLocation().Z - StartFeetZ));
			Counter |= CM->IsFalling() ? 1 : 0;
			if (SubTime > 1.5f)
			{
				Result(TEXT("rise_at_steep_face"), ArenaTuning::MaxStepHeight, MarkF, TEXT("cm"), 20.0);
				Result(TEXT("goes_airborne_at_steep_face"), 1.0, Counter, TEXT(""));
				FinishTest();
			}
		}
		return true;
	}
	return false;
}

#endif

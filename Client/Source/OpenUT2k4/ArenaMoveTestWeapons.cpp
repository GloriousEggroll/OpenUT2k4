// Weapon / item / combo fidelity tests of the -MoveTest hook (non-shipping): w_* test names.
#include "ArenaMoveTest.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaCharacterMovement.h"
#include "ArenaDamage.h"
#include "ArenaGameMode.h"
#include "ArenaPickup.h"
#include "ArenaProjectile.h"
#include "ArenaTypes.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaWeaponActors.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/ProjectileMovementComponent.h"

#if !UE_BUILD_SHIPPING

namespace ArenaWeaponTestLocal
{
	struct FRateCase
	{
		const TCHAR* Name;
		UClass* (*Class)();
		uint8 Mode;
		float Period;
		float AmmoPerShot;
		float PreFire;
	};

	template <typename T> UClass* Cls() { return T::StaticClass(); }

	const FRateCase RateCases[] = {
		{ TEXT("assault_rifle_primary"), &Cls<AWeap_Carbine>, 0, 0.16f, 1.f, 0.f },
		{ TEXT("bio_primary"), &Cls<AWeap_GooLauncher>, 0, 0.33f, 1.f, 0.f },
		{ TEXT("shock_beam"), &Cls<AWeap_PulseRifle>, 0, 0.7f, 1.f, 0.f },
		{ TEXT("shock_core"), &Cls<AWeap_PulseRifle>, 1, 0.6f, 1.f, 0.f },
		{ TEXT("link_plasma"), &Cls<AWeap_ArcLinker>, 0, 0.2f, 2.f, 0.f },
		{ TEXT("link_beam"), &Cls<AWeap_ArcLinker>, 1, 0.12f, 1.f, 0.f },
		{ TEXT("minigun_primary"), &Cls<AWeap_RotaryGun>, 0, 1.f / 15.f, 1.f, 0.27f },
		{ TEXT("minigun_alt"), &Cls<AWeap_RotaryGun>, 1, 0.2f, 1.f, 0.15f },
		{ TEXT("flak_primary"), &Cls<AWeap_ShardCannon>, 0, 0.8947f, 1.f, 0.f },
		{ TEXT("flak_alt"), &Cls<AWeap_ShardCannon>, 1, 1.11f, 1.f, 0.f },
		{ TEXT("rocket_primary"), &Cls<AWeap_RocketLauncher>, 0, 0.9f, 1.f, 0.f },
		{ TEXT("lightning_primary"), &Cls<AWeap_RailRifle>, 0, 1.6f, 1.f, 0.f },
		{ TEXT("instagib"), &Cls<AWeap_InstaRifle>, 0, 1.1f, 0.f, 0.f },
		{ TEXT("classic_sniper"), &Cls<AWeap_MarksmanRifle>, 0, 1.33f, 1.f, 0.f },
		{ TEXT("ons_grenade"), &Cls<AWeap_StickyLauncher>, 0, 0.65f, 1.f, 0.f },
		{ TEXT("ons_mine"), &Cls<AWeap_CrawlerLayer>, 0, 1.1f, 1.f, 0.f },
	};

	struct FDamageCase
	{
		const TCHAR* Name;
		UClass* (*Class)();
		uint8 Mode;
		float Distance;
		bool bHead;
		float Expected;
		float Tolerance;
	};

	const FDamageCase DamageCases[] = {
		{ TEXT("assault_rifle_bullet"), &Cls<AWeap_Carbine>, 0, 400.f, false, 7.f, 0.1f },
		{ TEXT("bio_glob"), &Cls<AWeap_GooLauncher>, 0, 500.f, false, 38.f, 1.1f },
		{ TEXT("shock_beam"), &Cls<AWeap_PulseRifle>, 0, 800.f, false, 45.f, 0.1f },
		{ TEXT("shock_core_direct"), &Cls<AWeap_PulseRifle>, 1, 800.f, false, 42.f, 1.1f },
		{ TEXT("link_plasma"), &Cls<AWeap_ArcLinker>, 0, 800.f, false, 30.f, 0.1f },
		{ TEXT("minigun_bullet"), &Cls<AWeap_RotaryGun>, 0, 250.f, false, 7.5f, 0.6f },
		{ TEXT("minigun_alt_bullet"), &Cls<AWeap_RotaryGun>, 1, 250.f, false, 15.f, 1.1f },
		{ TEXT("flak_9_chunks_point_blank"), &Cls<AWeap_ShardCannon>, 0, 250.f, false, 117.f, 14.f },
		{ TEXT("flak_shell_direct_plus_its_chunks"), &Cls<AWeap_ShardCannon>, 1, 500.f, false, 112.5f, 33.f },
		{ TEXT("rocket_direct"), &Cls<AWeap_RocketLauncher>, 0, 800.f, false, 90.f, 1.1f },
		{ TEXT("lightning_body"), &Cls<AWeap_RailRifle>, 0, 1500.f, false, 70.f, 0.1f },
		{ TEXT("lightning_headshot"), &Cls<AWeap_RailRifle>, 0, 1500.f, true, 140.f, 0.1f },
		{ TEXT("instagib"), &Cls<AWeap_InstaRifle>, 0, 1500.f, false, 1000.f, 0.1f },
		{ TEXT("classic_sniper_body"), &Cls<AWeap_MarksmanRifle>, 0, 1500.f, false, 60.f, 0.1f },
		{ TEXT("classic_sniper_headshot"), &Cls<AWeap_MarksmanRifle>, 0, 1500.f, true, 120.f, 0.1f },
	};
}

AArenaCharacter* UArenaMoveTestSubsystem::SpawnDummy()
{
	if (Dummy.IsValid())
	{
		return Dummy.Get();
	}
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaCharacter* D = GetWorld()->SpawnActor<AArenaCharacter>(AArenaCharacter::StaticClass(), Origin + FVector(-3500.f, -3500.f, 300.f), FRotator::ZeroRotator, P);
	Dummy = D;
	return D;
}

void UArenaMoveTestSubsystem::AimAt(const FVector& Target)
{
	AArenaCharacter* C = Pawn.Get();
	if (!C || !C->GetController())
	{
		return;
	}
	const FVector Eye = C->Camera ? C->Camera->GetComponentLocation() : C->GetPawnViewLocation();
	C->GetController()->SetControlRotation((Target - Eye).Rotation());
}

bool UArenaMoveTestSubsystem::TickWeaponTest(const FString& T, float DeltaTime)
{
	using namespace ArenaWeaponTestLocal;
	if (!T.StartsWith(TEXT("w_")))
	{
		return false;
	}
	AArenaCharacter* C = Pawn.Get();
	AArenaCharacter* D = SpawnDummy();
	const float Now = GetWorld()->GetTimeSeconds();
	SubTime += DeltaTime;
	auto NextSub = [this]() { ++SubPhase; SubTime = 0.f; };
	auto Equip = [C](UClass* Class) -> AArenaWeapon*
	{
		C->GiveWeapon(Class);
		AArenaWeapon* W = C->FindWeapon(Class);
		if (W)
		{
			W->AddAmmo(W->MaxAmmo);
			W->AddAltAmmo(W->AltMaxAmmo);
			C->SwitchToWeapon(W);
		}
		return W;
	};
	auto PlaceDummy = [this, D, C](float Distance)
	{
		const FVector Fwd = FRotator(0.f, 0.f, 0.f).Vector();
		D->SetActorLocation(C->GetActorLocation() + Fwd * Distance, false, nullptr, ETeleportType::TeleportPhysics);
		D->Health = 10000.f;
		D->Shield = 0.f;
	};
	if (T == TEXT("w_transloc"))
	{
		return TickTranslocTest(DeltaTime);
	}
	if (T == TEXT("w_shake") || T == TEXT("w_fall") || T == TEXT("w_vehrest") || T == TEXT("w_analog") || T == TEXT("w_stepup"))
	{
		return TickGapsTest(T, DeltaTime);
	}
	const int32 FireCount = C->GetAnimEvents().FireCount;
	const bool bShot = FireCount != LastFireCount;
	LastFireCount = FireCount;

	// ---------------------------------------------------------------- fire rates, ammo per shot, wind-up
	if (T == TEXT("w_rates"))
	{
		if (SubIndex >= UE_ARRAY_COUNT(RateCases))
		{
			FinishTest();
			return true;
		}
		const FRateCase& R = RateCases[SubIndex];
		AArenaWeapon* W = C->FindWeapon(R.Class());
		if (SubPhase == 0)
		{
			C->Health = 100.f;
			W = Equip(R.Class());
			C->GetController()->SetControlRotation(FRotator(45.f, 0.f, 0.f));
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			ShotTimes.Reset();
			AmmoAtStart = W ? float(W->GetAmmoFor(R.Mode)) : 0.f;
			MarkF = Now;
			C->StartFire(R.Mode);
			NextSub();
		}
		else if (SubPhase == 2)
		{
			if (bShot)
			{
				ShotTimes.Add(Now);
			}
			if (SubTime > 3.f)
			{
				C->StopFire(R.Mode);
				const int32 N = ShotTimes.Num();
				const float Period = N > 1 ? (ShotTimes.Last() - ShotTimes[0]) / float(N - 1) : 0.f;
				const float Used = W ? AmmoAtStart - float(W->GetAmmoFor(R.Mode)) : 0.f;
				Result(*FString::Printf(TEXT("%s_fire_period"), R.Name), R.Period, Period, TEXT("s"), FMath::Max(0.01, R.Period * 0.03));
				Result(*FString::Printf(TEXT("%s_ammo_per_shot"), R.Name), R.AmmoPerShot, N > 0 ? Used / float(N) : 0.f, TEXT("ammo"), 0.05);
				if (R.PreFire > 0.f && N > 0)
				{
					Result(*FString::Printf(TEXT("%s_wind_up"), R.Name), R.PreFire, ShotTimes[0] - MarkF, TEXT("s"), 0.02);
				}
				NextSub();
			}
		}
		else if (SubPhase == 3 && SubTime > 0.5f)
		{
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}

	// ---------------------------------------------------------------- damage per hit
	if (T == TEXT("w_damage"))
	{
		if (SubIndex >= UE_ARRAY_COUNT(DamageCases))
		{
			FinishTest();
			return true;
		}
		const FDamageCase& R = DamageCases[SubIndex];
		if (SubPhase == 0)
		{
			C->Health = 100.f;
			Equip(R.Class());
			PlaceDummy(R.Distance);
			NextSub();
		}
		else if (SubPhase == 1)
		{
			AimAt(R.bHead ? D->GetHeadLocation() : D->GetActorLocation());
			if (SubTime > 1.2f)
			{
				D->Health = 10000.f;
				C->StartFire(R.Mode);
				NextSub();
			}
		}
		else if (SubPhase == 2)
		{
			AimAt(R.bHead ? D->GetHeadLocation() : D->GetActorLocation());
			if (bShot || SubTime > 1.f)
			{
				C->StopFire(R.Mode);
				NextSub();
			}
		}
		else if (SubPhase == 3 && SubTime > 1.2f)
		{
			Result(*FString::Printf(TEXT("%s_damage"), R.Name), R.Expected, 10000.f - D->Health, TEXT("hp"), R.Tolerance);
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}

	// ---------------------------------------------------------------- projectile speed, acceleration, gravity, lifespan
	if (T == TEXT("w_proj"))
	{
		struct FProjCase { const TCHAR* Name; UClass* (*Class)(); uint8 Mode; float Speed; float SpeedAfterHalfSecond; float LifeSpan; float GravityZ; };
		static const FProjCase Cases[] = {
			{ TEXT("bio_glob"), &Cls<AWeap_GooLauncher>, 0, 4000.f, -1.f, -1.f, -1900.f },
			{ TEXT("grenade_min_hold"), &Cls<AWeap_Carbine>, 1, 1700.f, -1.f, -1.f, -1900.f },
			{ TEXT("shock_core"), &Cls<AWeap_PulseRifle>, 1, 2300.f, 2300.f, 10.f, 0.f },
			{ TEXT("link_plasma"), &Cls<AWeap_ArcLinker>, 0, 2000.f, 5000.f, 3.f, 0.f },
			{ TEXT("flak_chunk"), &Cls<AWeap_ShardCannon>, 0, 5000.f, 5000.f, 2.7f, 0.f },
			{ TEXT("flak_shell"), &Cls<AWeap_ShardCannon>, 1, 2443.f, -1.f, -1.f, -1900.f },
			{ TEXT("rocket"), &Cls<AWeap_RocketLauncher>, 0, 2700.f, 2700.f, 8.f, 0.f },
		};
		if (SubIndex >= UE_ARRAY_COUNT(Cases))
		{
			FinishTest();
			return true;
		}
		const FProjCase& R = Cases[SubIndex];
		if (SubPhase == 0)
		{
			Equip(R.Class());
			C->GetController()->SetControlRotation(FRotator(8.f, 45.f, 0.f));
			TrackedActor.Reset();
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			C->StartFire(R.Mode);
			NextSub();
		}
		else if (SubPhase == 2)
		{
			if (Counter == 0 && R.Mode == 1 && R.Class() == AWeap_Carbine::StaticClass() && SubTime > 0.02f)
			{
				C->StopFire(1);   // the grenade leaves on release
			}
			if (Counter == 0)
			{
				for (TActorIterator<AArenaProjectile> It(GetWorld()); It; ++It)
				{
					if (It->GetInstigator() == C && Now - It->CreationTime < 0.2f)
					{
						TrackedActor = *It;
						TrackedSpawnTime = It->CreationTime;
						const FVector V = It->Movement->Velocity;
						// the velocity read here is after the projectile's own first steps (link plasma accelerates 3000 UU/s^2
						// for its flight time; game time, so the frame rate does not matter)
						const float Expected = R.Name == FString(TEXT("link_plasma")) ? R.Speed + 6000.f * It->GetFlightTime() : R.Speed;
						Result(*FString::Printf(TEXT("%s_launch_speed"), R.Name), Expected, V.Size(), TEXT("cm/s"), FMath::Max(10.0, R.Speed * 0.03));
						MarkF = V.Z;
						MarkF2 = It->GetFlightTime();
						Mark = V;
						C->StopFire(R.Mode);
						Counter = 1;
						break;
					}
				}
				if (SubTime > 1.f && Counter == 0)
				{
					C->StopFire(R.Mode);
					Note(FString::Printf(TEXT("%s: no projectile"), R.Name));
					++SubIndex;
					SubPhase = 0;
					SubTime = 0.f;
				}
				return true;
			}
			AArenaProjectile* P = Cast<AArenaProjectile>(TrackedActor.Get());
			const float Age = Now - TrackedSpawnTime;
			// the projectile's own flight time (game time it has moved), not the wall-clock age
			const float Flight = P ? P->GetFlightTime() : 0.f;
			if (P && Counter == 1 && Flight >= 0.3f)
			{
				Counter = 2;
				if (R.GravityZ != 0.f && Flight - MarkF2 > 0.01f)
				{
					Result(*FString::Printf(TEXT("%s_gravity"), R.Name), R.GravityZ, (P->Movement->Velocity.Z - MarkF) / (Flight - MarkF2), TEXT("cm/s2"), 60.0);
				}
			}
			if (P && Counter == 2 && Flight >= 0.5f)
			{
				Counter = 3;
				if (R.SpeedAfterHalfSecond > 0.f)
				{
					// link plasma: 2000 + 6000 x t cm/s up to MaxSpeed 8000, at the flight time it is read at
					const float Expected = R.Name == FString(TEXT("link_plasma")) ? FMath::Min(8000.f, R.Speed + 6000.f * Flight) : R.SpeedAfterHalfSecond;
					Result(*FString::Printf(TEXT("%s_speed_after_0.5s"), R.Name), Expected, P->Movement->Velocity.Size(), TEXT("cm/s"), FMath::Max(30.0, Expected * 0.03));
				}
			}
			if (!P || Age > 16.f)
			{
				if (R.LifeSpan > 0.f)
				{
					Result(*FString::Printf(TEXT("%s_lifespan"), R.Name), R.LifeSpan, Age, TEXT("s"), 0.1);
				}
				Counter = 0;
				++SubIndex;
				SubPhase = 0;
				SubTime = 0.f;
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- shock combo
	if (T == TEXT("w_combo"))
	{
		AArenaWeapon* W = C->FindWeapon(AWeap_PulseRifle::StaticClass());
		if (SubPhase == 0)
		{
			W = Equip(AWeap_PulseRifle::StaticClass());
			PlaceDummy(1900.f);
			AimAt(D->GetActorLocation());
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			W->Ammo = 50;
			D->Health = 10000.f;
			AimAt(D->GetActorLocation());
			C->StartFire(1);
			TrackedActor.Reset();
			NextSub();
		}
		else if (SubPhase == 2)
		{
			C->StopFire(1);
			for (TActorIterator<AArenaProjectile> It(GetWorld()); It && !TrackedActor.IsValid(); ++It)
			{
				if (It->GetInstigator() == C)
				{
					TrackedActor = *It;
				}
			}
			if (TrackedActor.IsValid() && SubTime > 0.3f && Now >= 0.f)
			{
				NextSub();
			}
		}
		else if (SubPhase == 3)
		{
			// beam at the core once the rifle can fire again (FireRate 0.6 after the core)
			if (AActor* Core = TrackedActor.Get())
			{
				AimAt(Core->GetActorLocation());
				TrackedLoc = Core->GetActorLocation();
				if (SubTime > 0.35f)
				{
					C->StartFire(0);
					NextSub();
				}
			}
			else
			{
				Note(TEXT("core gone"));
				FinishTest();
			}
		}
		else if (SubPhase == 4)
		{
			C->StopFire(0);
			if (SubTime > 0.5f)
			{
				const float Dist = FMath::Max(0.f, FVector::Dist(TrackedLoc, D->GetActorLocation()) - 50.f);
				const float Scale = FMath::Max(0.f, 1.f - Dist / 550.f);
				Result(TEXT("combo_damage"), FMath::FloorToFloat(200.f * Scale), 10000.f - D->Health, TEXT("hp"), 3.0);
				Result(TEXT("combo_ammo_used_core_beam_plus_3"), 5.f, 50.f - float(W->Ammo), TEXT("ammo"), 0.1);
				Note(FString::Printf(TEXT("combo %.0f cm from the dummy's edge (ComboDamage 200, ComboRadius 275 UU = 550 cm)"), Dist));
				FinishTest();
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- bio charge
	if (T == TEXT("w_bio"))
	{
		AWeap_GooLauncher* W = Cast<AWeap_GooLauncher>(C->FindWeapon(AWeap_GooLauncher::StaticClass()));
		if (SubPhase == 0)
		{
			W = Cast<AWeap_GooLauncher>(Equip(AWeap_GooLauncher::StaticClass()));
			PlaceDummy(450.f);
			NextSub();
		}
		else if (SubPhase == 1)
		{
			AimAt(D->GetActorLocation() + FVector(0.f, 0.f, 20.f));
			if (SubTime > 1.2f)
			{
				W->Ammo = 50;
				D->Health = 10000.f;
				C->StartFire(1);
				NextSub();
			}
		}
		else if (SubPhase == 2 && SubTime >= 1.01f)
		{
			Result(TEXT("goop_load_after_1s"), 5.f, float(W->GetGoopLoad()), TEXT("globs"), 0.1);
			Result(TEXT("ammo_taken_while_loading"), 5.f, 50.f - float(W->Ammo), TEXT("ammo"), 0.1);
			C->StopFire(1);
			NextSub();
		}
		else if (SubPhase == 3 && SubTime > 1.5f)
		{
			Result(TEXT("level5_glob_damage"), 20.f + 19.f * 5.f, 10000.f - D->Health, TEXT("hp"), 3.0);
			FinishTest();
		}
		return true;
	}

	// ---------------------------------------------------------------- rocket load
	if (T == TEXT("w_rocketload"))
	{
		AArenaWeapon* W = C->FindWeapon(AWeap_RocketLauncher::StaticClass());
		if (SubPhase == 0)
		{
			W = Equip(AWeap_RocketLauncher::StaticClass());
			C->GetController()->SetControlRotation(FRotator(30.f, 180.f, 0.f));
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			W->Ammo = 30;
			C->StartFire(1);
			MarkF = Now;
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 2)
		{
			if (bShot && Counter == 0)
			{
				Counter = 1;
				int32 Rockets = 0;
				for (TActorIterator<AArenaProjectile> It(GetWorld()); It; ++It)
				{
					Rockets += It->GetInstigator() == C && Now - It->CreationTime < 0.1f ? 1 : 0;
				}
				Result(TEXT("auto_release_after"), 2.3f, Now - MarkF, TEXT("s"), 0.03);
				Result(TEXT("rockets_fired"), 3.f, float(Rockets), TEXT("rockets"), 0.1);
				Result(TEXT("ammo_used"), 3.f, 30.f - float(W->Ammo), TEXT("ammo"), 0.1);
			}
			if (SubTime > 3.f)
			{
				C->StopFire(1);
				FinishTest();
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- weapon switch timing
	if (T == TEXT("w_switch"))
	{
		static UClass* (*Targets[])() = { &Cls<AWeap_RocketLauncher>, &Cls<AWeap_RailRifle> };
		static const float Expected[] = { 0.33f + 0.33f, 0.33f + 0.36f };
		if (SubIndex >= 2)
		{
			FinishTest();
			return true;
		}
		if (SubPhase == 0)
		{
			Equip(AWeap_Carbine::StaticClass());
			C->GiveWeapon(Targets[SubIndex]());
			C->FindWeapon(Targets[SubIndex]())->AddAmmo(100);
			C->GetController()->SetControlRotation(FRotator(45.f, 0.f, 0.f));
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.5f)
		{
			C->SwitchToWeapon(C->FindWeapon(Targets[SubIndex]()));
			MarkF = Now;
			NextSub();
		}
		else if (SubPhase == 2)
		{
			// hold fire through the switch: the first shot comes after PutDownTime + BringUpTime
			C->StartFire(0);
			if (bShot)
			{
				Result(*FString::Printf(TEXT("switch_to_%s_first_shot"), SubIndex == 0 ? TEXT("rocket") : TEXT("lightning")), Expected[SubIndex], Now - MarkF, TEXT("s"), 0.03);
				C->StopFire(0);
				NextSub();
			}
		}
		else if (SubPhase == 3 && SubTime > 2.f)
		{
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}

	// ---------------------------------------------------------------- shield gun: auto release, shield absorption, energy
	if (T == TEXT("w_shield"))
	{
		AWeap_Gauntlet* W = Cast<AWeap_Gauntlet>(C->FindWeapon(AWeap_Gauntlet::StaticClass()));
		if (SubPhase == 0)
		{
			W = Cast<AWeap_Gauntlet>(Equip(AWeap_Gauntlet::StaticClass()));
			PlaceDummy(500.f);
			AimAt(D->GetActorLocation());
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			// charge while the target is out of range, then bring it within ShieldRange: auto release (0.15 s checks)
			D->Health = 10000.f;
			C->StartFire(0);
			NextSub();
		}
		else if (SubPhase == 2)
		{
			AimAt(D->GetActorLocation());
			if (SubTime > 1.5f && Counter == 0)
			{
				Counter = 1;
				D->SetActorLocation(C->GetActorLocation() + FVector(150.f, 0.f, 0.f));
				MarkF = Now;
			}
			if (bShot)
			{
				const float Hold = Now - MarkF + 1.5f;
				const float Scale = (FMath::Clamp(Hold, 0.4f, 2.5f) - 0.4f) / 2.1f;
				// ShieldFire checks every 0.15 s, and the shot is seen in the frame it happens: 0 .. 0.15 s + one frame
				Result(TEXT("auto_release_delay"), 0.075f + 0.5f * DeltaTime, Now - MarkF, TEXT("s"), 0.08 + 0.5 * DeltaTime);
				Result(TEXT("auto_release_damage"), FMath::FloorToFloat(FMath::Lerp(40.f, 150.f, Scale)), 10000.f - D->Health, TEXT("hp"), 3.0);
				C->StopFire(0);
				Counter = 0;
				NextSub();
			}
			else if (SubTime > 4.f)
			{
				Result(TEXT("auto_release_delay"), 0.075f, 99.f, TEXT("s"), 0.08);
				C->StopFire(0);
				NextSub();
			}
		}
		else if (SubPhase == 3 && SubTime > 1.f)
		{
			// shield up: frontal damage drained (2 x energy, max 100), half of it in energy, momentum x1.25
			W->Ammo = 100;
			AimAt(D->GetActorLocation());
			C->StartFire(1);
			NextSub();
		}
		else if (SubPhase == 4 && SubTime > 0.05f)
		{
			C->Health = 100.f;
			C->Shield = 0.f;
			W->Ammo = 100;
			FArenaHitInfo Info;
			Info.WeaponName = TEXT("Test");
			Info.HitLocation = C->GetActorLocation() + C->GetViewRotation().Vector() * 50.f;
			ArenaDamage::ApplyDamage(C, 80.f, nullptr, D, Info);
			Result(TEXT("front_80_damage_taken"), 0.f, 100.f - C->Health, TEXT("hp"), 0.1);
			Result(TEXT("front_80_energy_used"), 40.f, 100.f - float(W->Ammo), TEXT("ammo"), 0.1);
			C->Health = 100.f;
			Info.HitLocation = C->GetActorLocation() - C->GetViewRotation().Vector() * 50.f;
			ArenaDamage::ApplyDamage(C, 80.f, nullptr, D, Info);
			Result(TEXT("back_80_damage_taken"), 80.f, 100.f - C->Health, TEXT("hp"), 0.1);
			C->Health = 100.f;
			W->Ammo = 100;
			MarkF = 100.f;
			NextSub();
		}
		else if (SubPhase == 5 && SubTime >= 2.f)
		{
			Result(TEXT("shield_energy_used_in_2s"), 10.f, MarkF - float(W->Ammo), TEXT("ammo"), 1.1);
			C->StopFire(1);
			MarkF = float(W->Ammo);
			NextSub();
		}
		else if (SubPhase == 6 && SubTime >= 2.f)
		{
			Result(TEXT("shield_energy_regained_in_2s"), 10.f, float(W->Ammo) - MarkF, TEXT("ammo"), 1.1);
			FinishTest();
		}
		return true;
	}

	// ---------------------------------------------------------------- Shield Gun reflection (xPawn.CheckReflect)
	if (T == TEXT("w_reflect"))
	{
		AWeap_Gauntlet* G = D ? Cast<AWeap_Gauntlet>(D->FindWeapon(AWeap_Gauntlet::StaticClass())) : nullptr;
		auto FaceShooter = [C, D]()
		{
			const FRotator R = (C->GetActorLocation() - D->GetActorLocation()).Rotation();
			D->SetActorRotation(FRotator(0.f, R.Yaw, 0.f));
			if (D->GetController())
			{
				D->GetController()->SetControlRotation(R);
			}
		};
		auto FindReflected = [this](FVector& OutDir) -> bool
		{
			for (TActorIterator<AArenaProjectile> It(GetWorld()); It; ++It)
			{
				if (It->ReflectGeneration == 1 && !It->IsActorBeingDestroyed())
				{
					OutDir = It->GetVelocity().GetSafeNormal();
					return true;
				}
			}
			return false;
		};
		const FVector Back = (C->GetActorLocation() - D->GetActorLocation()).GetSafeNormal2D();
		if (SubPhase == 0)
		{
			Equip(AWeap_PulseRifle::StaticClass());
			PlaceDummy(600.f);
			D->GiveWeapon(AWeap_Gauntlet::StaticClass());
			D->SwitchToWeapon(D->FindWeapon(AWeap_Gauntlet::StaticClass()));
			FaceShooter();
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			FaceShooter();
			D->StartFire(1);
			NextSub();
		}
		else if (SubPhase == 2 && SubTime > 0.3f && G)
		{
			// shock beam (ShockBeamFire.bReflective): goes back along the holder's view and hits the shooter
			FaceShooter();
			G->Ammo = 100;
			G->ReflectCount = 0;
			C->Health = 100.f;
			C->Shield = 0.f;
			D->Health = 10000.f;
			AimAt(D->GetActorLocation() + FVector(0.f, 0.f, 20.f));
			C->StartFire(0);
			NextSub();
		}
		else if (SubPhase == 3 && SubTime > 0.25f && G)
		{
			C->StopFire(0);
			Result(TEXT("beam_reflections"), 1.0, G->ReflectCount, TEXT("x"), 0.1);
			Result(TEXT("beam_damage_to_shield_holder"), 0.0, 10000.f - D->Health, TEXT("hp"), 0.1);
			Result(TEXT("beam_shield_energy_used"), 11.0, 100.f - float(G->Ammo), TEXT("ammo"), 1.5);
			{
				// the reflected beam is still the shooter's (self damage: halved offline at skill 3 and below)
				const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
				const bool bHalf = GM && C->IsPlayerControlled() && GM->GetBotSkill() <= 3 && GetWorld()->GetNetMode() == NM_Standalone;
				Result(TEXT("reflected_beam_damage_to_shooter"), bHalf ? 22.0 : 45.0, 100.f - C->Health, TEXT("hp"), 0.1);
			}
			C->Health = 100.f;
			NextSub();
		}
		else if (SubPhase == 4 && SubTime > 0.8f && G)
		{
			// shock core: a copy flies straight along the holder's view (it ignores its instigator on touch)
			FaceShooter();
			G->Ammo = 100;
			G->ReflectCount = 0;
			D->Health = 10000.f;
			AimAt(D->GetActorLocation() + FVector(0.f, 0.f, 20.f));
			C->StartFire(1);
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 5 && G)
		{
			C->StopFire(1);
			FVector Dir;
			if (Counter == 0 && FindReflected(Dir))
			{
				Counter = 1;
				Result(TEXT("core_reflected_dir_dot_view"), 1.0, FVector::DotProduct(Dir, Back), TEXT("dot"), 0.05);
			}
			if (SubTime > 1.2f)
			{
				if (Counter == 0)
				{
					Result(TEXT("core_reflected_dir_dot_view"), 1.0, -1.0, TEXT("dot"), 0.05);
				}
				Result(TEXT("core_reflections"), 1.0, G->ReflectCount, TEXT("x"), 0.1);
				Result(TEXT("core_damage_to_shield_holder"), 0.0, 10000.f - D->Health, TEXT("hp"), 0.1);
				Equip(AWeap_ArcLinker::StaticClass());
				NextSub();
			}
		}
		else if (SubPhase == 6 && SubTime > 1.5f && G)
		{
			// link plasma: mirrored about the holder's view (X - 2N(X.N))
			FaceShooter();
			G->Ammo = 100;
			G->ReflectCount = 0;
			D->Health = 10000.f;
			AimAt(D->GetActorLocation() + FVector(0.f, 0.f, 20.f));
			C->StartFire(0);
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 7 && G)
		{
			if (SubTime > 0.1f)
			{
				C->StopFire(0);
			}
			FVector Dir;
			if (Counter == 0 && FindReflected(Dir))
			{
				Counter = 1;
				Result(TEXT("link_reflected_dir_dot_back"), 1.0, FVector::DotProduct(Dir, Back), TEXT("dot"), 0.05);
			}
			if (SubTime > 1.2f)
			{
				if (Counter == 0)
				{
					Result(TEXT("link_reflected_dir_dot_back"), 1.0, -1.0, TEXT("dot"), 0.05);
				}
				Result(TEXT("link_damage_to_shield_holder"), 0.0, 10000.f - D->Health, TEXT("hp"), 0.1);
				Result(TEXT("link_reflections_at_least"), 1.0, FMath::Min(1, G->ReflectCount), TEXT("x"), 0.1);
				D->StopFire(1);
				FinishTest();
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- dual assault rifles (AssaultRifle.bDualMode)
	if (T == TEXT("w_dual"))
	{
		AWeap_Carbine* W = Cast<AWeap_Carbine>(C->FindWeapon(AWeap_Carbine::StaticClass()));
		IConsoleVariable* Behind = IConsoleManager::Get().FindConsoleVariable(TEXT("arena.BehindView"));
		if (SubPhase == 0)
		{
			W = Cast<AWeap_Carbine>(Equip(AWeap_Carbine::StaticClass()));
			if (Behind)
			{
				Behind->Set(0, ECVF_SetByCode); // the test rig runs in behind view; this check needs the first-person view
			}
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.f && W)
		{
			Result(TEXT("dual_before_pickup"), 0.0, W->IsDualMode() ? 1.0 : 0.0, TEXT("bool"), 0.1);
			W->OnRepeatPickup(true);
			Result(TEXT("dual_after_second_rifle"), 1.0, W->IsDualMode() ? 1.0 : 0.0, TEXT("bool"), 0.1);
			NextSub();
		}
		else if (SubPhase == 2 && SubTime > 1.f && W)
		{
			const bool bFirst = W->GetOffhandViewMesh() ? W->GetOffhandViewMesh()->IsVisible() && !W->GetOffhandViewMesh()->bHiddenInGame : W->HasMirroredViewParts();
			Result(TEXT("first_person_left_gun_shown"), 1.0, bFirst ? 1.0 : 0.0, TEXT("bool"), 0.1);
			if (USkeletalMeshComponent* O = W->GetOffhandViewMesh())
			{
				Note(FString::Printf(TEXT("left view gun visible %d hidden %d at %s"), O->IsVisible() ? 1 : 0, O->bHiddenInGame ? 1 : 0, *O->GetComponentLocation().ToCompactString()));
			}
			if (W->GetOffhandViewMesh())
			{
				Result(TEXT("first_person_left_gun_mirrored_scale_y"), -1.0, FMath::Sign(W->GetOffhandViewMesh()->GetRelativeScale3D().Y), TEXT("sign"), 0.1);
			}
			Shot(TEXT("first_person"));
			NextSub();
		}
		else if (SubPhase == 3 && SubTime > 0.3f && Counter == 0)
		{
			if (Behind)
			{
				Behind->Set(1, ECVF_SetByCode);
			}
			Counter = 1;
			SubTime = 0.f;
		}
		else if (SubPhase == 3 && SubTime > 0.5f && Counter == 1)
		{
			Counter = 0;
			USkeletalMeshComponent* Off = C->GetOffhandWeapon();
			if (C->IsUsingSkeletalBody())
			{
				Result(TEXT("third_person_left_gun_shown"), 1.0, Off && Off->IsVisible() ? 1.0 : 0.0, TEXT("bool"), 0.1);
				if (Off)
				{
					Note(FString::Printf(TEXT("left gun on %s at %s"), *Off->GetAttachSocketName().ToString(), *Off->GetComponentLocation().ToCompactString()));
				}
			}
			else
			{
				Note(TEXT("no player model art: no third-person weapons"));
			}
			if (C->GetMesh())
			{
				FString Bones;
				for (int32 B = 0; B < C->GetMesh()->GetNumBones(); ++B)
				{
					const FString N = C->GetMesh()->GetBoneName(B).ToString();
					if (N.Contains(TEXT("hand")) || N.Contains(TEXT("arm")))
					{
						Bones += N + TEXT(", ");
					}
				}
				Note(FString::Printf(TEXT("hand / arm bones: %s"), *Bones));
			}
			Note(FString::Printf(TEXT("skeletal body %d, left hand bone %d, right gun %d"), C->IsUsingSkeletalBody() ? 1 : 0,
				C->GetMesh() && C->GetMesh()->DoesSocketExist(TEXT("bip01-l-hand")) ? 1 : 0, C->GetThirdPersonWeapon() && C->GetThirdPersonWeapon()->IsVisible() ? 1 : 0));
			Shot(TEXT("third_person"));
			NextSub();
		}
		else if (SubPhase == 4 && SubTime > 0.5f)
		{
			FinishTest(); // the rig's behind view stays on
		}
		return true;
	}

	// ---------------------------------------------------------------- the super / Onslaught / classic weapons
	if (T == TEXT("w_extra"))
	{
		UWorld* World = GetWorld();
		const float CapR = D->GetSimpleCollisionRadius();
		// UE2 HurtRadius at every stage of a staged blast (independent re-computation for the expected values)
		auto StagedExpected = [CapR](const FVector& Blast, const FVector& Victim, float Dmg, float R, const TArray<float>& Fracs)
		{
			const float Dist = FMath::Max(0.f, FVector::Dist(Blast, Victim) - CapR);
			float Sum = 0.f;
			for (float F : Fracs)
			{
				Sum += Dist < R * F ? Dmg * (1.f - Dist / (R * F)) : 0.f;
			}
			return Sum;
		};
		const TArray<float> NukeFracs = { 0.125f, 0.3f, 0.475f, 0.65f, 0.825f, 1.f };
		APlayerController* PC = Cast<APlayerController>(C->GetController() ? C->GetController() : C->GetRemoteController());
		if (SubPhase == 0)
		{
			// ---- warhead launcher, primary: the shell at 2000 cm/s and the six-ring nuke
			Equip(AWeap_WarheadLauncher::StaticClass());
			PlaceDummy(3000.f);
			AimAt(D->GetActorLocation());
			C->Health = 100000.f;
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 1.2f)
		{
			D->Health = 100000.f;
			AimAt(D->GetActorLocation());
			C->StartFire(0);
			C->StopFire(0);
			Counter = 0;
			TrackedProjectile.Reset();
			NextSub();
		}
		else if (SubPhase == 2)
		{
			if (!TrackedProjectile.IsValid() && Counter == 0)
			{
				for (TActorIterator<AArenaWarheadShell> It(World); It; ++It)
				{
					TrackedProjectile = *It;
					LastProjectileLoc = It->GetActorLocation();
					MarkF = SubTime;
				}
			}
			else if (AArenaWarheadShell* S = Cast<AArenaWarheadShell>(TrackedProjectile.Get()))
			{
				if (Counter == 0 && SubTime - MarkF > 0.2f && !S->HasBlownUp())
				{
					Result(TEXT("warhead_speed"), 2000.0, FVector::Dist(S->GetActorLocation(), LastProjectileLoc) / (SubTime - MarkF), TEXT("cm/s"), 30.0);
					Counter = 1;
				}
			}
			for (TActorIterator<AArenaStagedBlast> It(World); It; ++It)
			{
				Mark = It->GetActorLocation();
				Counter = 2;
			}
			if (Counter == 2 && SubTime > 3.f)
			{
				const float Expect = StagedExpected(Mark, D->GetActorLocation(), 250.f, 4000.f, NukeFracs);
				Result(TEXT("nuke_damage_to_target_6_rings"), Expect, 100000.f - D->Health, TEXT("hp"), FMath::Max(3.f, Expect * 0.02f));
				NextSub();
			}
			else if (SubTime > 6.f)
			{
				Result(TEXT("nuke_damage_to_target_6_rings"), 1400.0, 0.0, TEXT("hp"), 1.0);
				NextSub();
			}
		}
		else if (SubPhase == 3 && SubTime > 0.5f)
		{
			Counter = 0;
			// ---- warhead launcher, alt: fly it (the player's controller possesses the warhead)
			AArenaWeapon* W = C->FindWeapon(AWeap_WarheadLauncher::StaticClass());
			C->Health = 100000.f;
			if (W)
			{
				W->AddAmmo(1);
				C->SwitchToWeapon(W);
			}
			NextSub();
		}
		else if (SubPhase == 4 && SubTime > 1.2f)
		{
			AimAt(C->GetActorLocation() + FVector(3000.f, 0.f, 400.f));
			C->StartFire(1);
			NextSub();
		}
		else if (SubPhase == 5)
		{
			AArenaGuidedWarhead* WH = PC ? Cast<AArenaGuidedWarhead>(PC->GetPawn()) : nullptr;
			if (SubTime > 0.1f && Counter < 5)
			{
				Result(TEXT("guided_player_flies_warhead"), 1.0, WH ? 1.0 : 0.0, TEXT("bool"), 0.1);
				Counter = 5;
				if (WH)
				{
					Mark = WH->GetActorLocation();
					MarkF = SubTime;
				}
				else
				{
					NextSub();
					SubPhase = 7;
				}
			}
			else if (WH && SubTime > 0.6f && Counter == 5)
			{
				Result(TEXT("guided_speed"), 2000.0, FVector::Dist(WH->GetActorLocation(), Mark) / (SubTime - MarkF), TEXT("cm/s"), 40.0);
				Shot(TEXT("guided_scope"));
				Counter = 6;
			}
			else if (WH && SubTime > 0.9f && Counter == 6)
			{
				WH->BlowUp(WH->GetActorLocation());
				Counter = 7;
				NextSub();
			}
			else if (SubTime > 3.f)
			{
				NextSub();
			}
		}
		else if (SubPhase == 6 && SubTime > 0.4f && Counter == 7)
		{
			Shot(TEXT("guided_static"));
			Counter = 8;
		}
		else if (SubPhase == 6 && SubTime > 1.5f)
		{
			Result(TEXT("guided_control_returns_after_blast"), 1.0, PC && PC->GetPawn() == C ? 1.0 : 0.0, TEXT("bool"), 0.1);
			NextSub();
		}
		else if (SubPhase == 7 && SubTime > 0.5f)
		{
			// ---- orbital designator: hold on a spot under open sky; the strike engages after PaintDuration 1.2 s
			Equip(AWeap_OrbitalDesignator::StaticClass());
			PlaceDummy(1500.f);
			C->Health = 100000.f;
			NextSub();
		}
		else if (SubPhase == 8 && SubTime > 0.8f)
		{
			D->Health = 100000.f;
			Mark = C->GetActorLocation() + FVector(1300.f, 0.f, -C->GetSimpleCollisionHalfHeight());
			AimAt(Mark);
			C->StartFire(0);
			MarkF = SubTime;
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 9)
		{
			AimAt(Mark);
			AArenaIonCannon* Cannon = nullptr;
			for (TActorIterator<AArenaIonCannon> It(World); It; ++It)
			{
				Cannon = *It;
			}
			if (Counter == 0 && SubTime > 0.6f && SubTime < 0.7f)
			{
				Shot(TEXT("painting"));
			}
			if (Counter == 0 && Cannon && Cannon->GetStrikes() > 0)
			{
				Counter = 1;
				Result(TEXT("ion_strike_engaged_after"), 1.25, SubTime, TEXT("s"), 0.26);
				C->StopFire(0);
				MarkF2 = SubTime;
			}
			if (Counter == 1 && SubTime - MarkF2 > 3.f)
			{
				const AWeap_OrbitalDesignator* W = Cast<AWeap_OrbitalDesignator>(C->FindWeapon(AWeap_OrbitalDesignator::StaticClass()));
				Result(TEXT("ion_painter_charge_used"), 0.0, W ? W->Ammo : -1, TEXT("ammo"), 0.1);
				const FVector DamageLoc = Mark + FVector(0.f, 0.f, 400.f);
				const float Expect = StagedExpected(DamageLoc, D->GetActorLocation(), 150.f, 4000.f, NukeFracs);
				Result(TEXT("ion_damage_to_target_6_rings"), Expect, 100000.f - D->Health, TEXT("hp"), FMath::Max(3.f, Expect * 0.03f));
				NextSub();
			}
			else if (Counter == 0 && SubTime > 4.f)
			{
				Result(TEXT("ion_strike_engaged_after"), 1.25, -1.0, TEXT("s"), 0.26);
				C->StopFire(0);
				NextSub();
			}
		}
		else if (SubPhase == 10 && SubTime > 0.5f)
		{
			// ---- sticky launcher: sticks to the target, alt detonates (100, 175 UU)
			Equip(AWeap_StickyLauncher::StaticClass());
			PlaceDummy(600.f);
			NextSub();
		}
		else if (SubPhase == 11 && SubTime > 0.8f)
		{
			D->Health = 100000.f;
			AimAt(D->GetActorLocation() + FVector(0.f, 0.f, 50.f));
			C->StartFire(0);
			C->StopFire(0);
			NextSub();
		}
		else if (SubPhase == 12 && SubTime > 1.f)
		{
			AArenaStickyGrenade* G = nullptr;
			for (TActorIterator<AArenaStickyGrenade> It(World); It; ++It)
			{
				G = *It;
			}
			Result(TEXT("grenade_sticks_to_target"), 1.0, G && G->GetStuckTo() == D ? 1.0 : 0.0, TEXT("bool"), 0.1);
			Mark = G ? G->GetActorLocation() : D->GetActorLocation();
			Shot(TEXT("sticky"));
			C->StartFire(1);
			C->StopFire(1);
			NextSub();
		}
		else if (SubPhase == 13 && SubTime > 0.3f)
		{
			const float Dist = FMath::Max(0.f, FVector::Dist(Mark, D->GetActorLocation()) - CapR);
			Result(TEXT("grenade_detonation_damage"), 100.f * FMath::Max(0.f, 1.f - Dist / 350.f), 100000.f - D->Health, TEXT("hp"), 1.0);
			NextSub();
		}
		else if (SubPhase == 14 && SubTime > 0.3f)
		{
			// ---- crawler layer: the mine lands, finds the target within 750 UU, runs at 525 UU/s and leaps
			Equip(AWeap_CrawlerLayer::StaticClass());
			PlaceDummy(1000.f);
			NextSub();
		}
		else if (SubPhase == 15 && SubTime > 0.8f)
		{
			D->Health = 100000.f;
			AimAt(C->GetActorLocation() + FVector(250.f, 0.f, -C->GetSimpleCollisionHalfHeight()));
			C->StartFire(0);
			C->StopFire(0);
			Counter = 0;
			MarkF = 0.f;
			TrackedProjectile.Reset();
			NextSub();
		}
		else if (SubPhase == 16)
		{
			AArenaCrawlerMine* M = Cast<AArenaCrawlerMine>(TrackedProjectile.Get());
			if (!M)
			{
				for (TActorIterator<AArenaCrawlerMine> It(World); It; ++It)
				{
					M = *It;
					TrackedProjectile = M;
				}
			}
			if (M)
			{
				Mark = M->GetActorLocation();
				if (M->GetState() == AArenaCrawlerMine::EState::Scurrying)
				{
					const FVector V = (M->GetActorLocation() - LastProjectileLoc) / FMath::Max(DeltaTime, 0.001f);
					if (Counter < 1 && FVector(V.X, V.Y, 0.f).Size() > 100.f)
					{
						MarkF = FMath::Max(MarkF, float(FVector(V.X, V.Y, 0.f).Size()));
						Counter = 1;
						Shot(TEXT("crawler"));
					}
				}
				LastProjectileLoc = M->GetActorLocation();
			}
			else if (Counter >= 1 || SubTime > 1.f)
			{
				Counter = Counter >= 1 ? Counter : 1;
			}
			if ((!M && SubTime > 1.f) || SubTime > 10.f)
			{
				Result(TEXT("mine_run_speed"), 1050.0, MarkF, TEXT("cm/s"), 40.0);
				// from where the mine blew up (it can move a long way in its last frame at a low frame rate)
				const float Dist = FMath::Max(0.f, FVector::Dist(AArenaCrawlerMine::LastBlowUpLocation, D->GetActorLocation()) - CapR);
				Result(TEXT("mine_damage"), 95.f * FMath::Max(0.f, 1.f - Dist / 500.f), 100000.f - D->Health, TEXT("hp"), 6.0);
				NextSub();
			}
		}
		else if (SubPhase == 17 && SubTime > 0.3f)
		{
			// ---- seeker launcher: 550 UU/s, the acceleration grows 750 UU/s^2 per second (v = 550 + 375 t^2)
			Equip(AWeap_SeekerLauncher::StaticClass());
			D->SetActorLocation(Origin + FVector(-3500.f, -3500.f, 300.f));
			NextSub();
		}
		else if (SubPhase == 18 && SubTime > 1.f)
		{
			AimAt(C->GetActorLocation() + FVector(3000.f, 0.f, 600.f));
			C->StartFire(0);
			C->StopFire(0);
			MarkF = SubTime;
			MarkF2 = C->GetVelocity().Z;
			TrackedProjectile.Reset();
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 19)
		{
			if (Counter == 0 && SubTime > 0.f)
			{
				// KickMomentum Z 175 UU/s, then gravity for the time since the shot
				Result(TEXT("seeker_kick_up_velocity"), 350.0 - 1900.0 * SubTime, C->GetVelocity().Z, TEXT("cm/s"), 25.0);
				Counter = 1;
			}
			AArenaSeekerRocket* R = Cast<AArenaSeekerRocket>(TrackedProjectile.Get());
			if (!R)
			{
				for (TActorIterator<AArenaSeekerRocket> It(World); It; ++It)
				{
					R = *It;
					TrackedProjectile = R;
					MarkF = SubTime;
				}
			}
			if (R && Counter == 1 && SubTime - MarkF >= 1.f)
			{
				// the rocket's own flight time (it can be found a frame or more after it left)
				const float Tt = R->GetFlightTime();
				Result(TEXT("seeker_speed_after_1s"), (550.f + 375.f * Tt * Tt) * 2.f, R->GetVelocityCm().Size(), TEXT("cm/s"), 40.0);
				Counter = 2;
			}
			if (Counter >= 2 || SubTime > 3.f)
			{
				C->Health = 100.f;
				// leave nothing flying / lying around for the next tests
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (Cast<AArenaSeekerRocket>(*It) || Cast<AArenaCrawlerMine>(*It) || Cast<AArenaStickyGrenade>(*It) || Cast<AArenaStagedBlast>(*It)
						|| Cast<AArenaWarheadShell>(*It))
					{
						It->Destroy();
					}
				}
				FinishTest();
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- armour (xPawn.ShieldAbsorb)
	if (T == TEXT("w_armor"))
	{
		struct FArmorCase { int32 Pack1; int32 Pack2; float Damage; float ExpectHealthLoss; float ExpectShield; };
		static const FArmorCase Cases[] = {
			{ 50, 0, 40.f, 20.f, 30.f },     // small shield: 50%
			{ 100, 0, 40.f, 10.f, 70.f },    // big shield: 75%
			{ 100, 50, 40.f, 0.f, 110.f },   // 150: the part above 100 takes everything
			{ 100, 0, 200.f, 100.f, 0.f },   // big shield runs out: the 100 interval is soaked, the rest goes through
		};
		for (const FArmorCase& A : Cases)
		{
			C->Health = 199.f;
			C->Shield = 0.f;
			C->SmallShield = 0.f;
			C->AddShieldStrengthUE2(A.Pack1);
			if (A.Pack2 > 0)
			{
				C->AddShieldStrengthUE2(A.Pack2);
			}
			FArenaHitInfo Info;
			Info.WeaponName = TEXT("Test");
			ArenaDamage::ApplyDamage(C, A.Damage, nullptr, D, Info);
			const FString Name = FString::Printf(TEXT("shield_%d%s_damage_%.0f"), A.Pack1, A.Pack2 > 0 ? TEXT("+50") : TEXT(""), A.Damage);
			Result(*(Name + TEXT("_health_loss")), A.ExpectHealthLoss, 199.f - C->Health, TEXT("hp"), 0.1);
			Result(*(Name + TEXT("_shield_left")), A.ExpectShield, C->Shield, TEXT("shield"), 0.6);
		}
		C->Health = 100.f;
		C->Shield = 0.f;
		C->SmallShield = 0.f;
		C->AddShieldStrengthUE2(50);
		C->AddShieldStrengthUE2(50);
		Result(TEXT("two_small_packs_shield"), 50.f, C->Shield, TEXT("shield"), 0.1);
		C->AddShieldStrengthUE2(100);
		C->AddShieldStrengthUE2(100);
		Result(TEXT("shield_cap"), 150.f, C->Shield, TEXT("shield"), 0.1);
		C->Shield = 0.f;
		C->SmallShield = 0.f;
		FinishTest();
		return true;
	}

	// ---------------------------------------------------------------- pickups
	if (T == TEXT("w_pickups"))
	{
		struct FPickCase { EArenaPickupType Type; float Health; float Expected; float Respawn; const TCHAR* Name; };
		static const FPickCase Cases[] = {
			{ EArenaPickupType::HealthVial, 100.f, 105.f, 30.f, TEXT("vial_at_100") },
			{ EArenaPickupType::HealthVial, 197.f, 199.f, 30.f, TEXT("vial_at_197") },
			{ EArenaPickupType::HealthPack, 60.f, 85.f, 30.f, TEXT("health_pack_at_60") },
			{ EArenaPickupType::HealthPack, 95.f, 100.f, 30.f, TEXT("health_pack_at_95") },
			{ EArenaPickupType::SuperHealth, 100.f, 199.f, 60.f, TEXT("keg_at_100") },
			{ EArenaPickupType::ShieldSmall, 100.f, 50.f, 30.f, TEXT("shield_pack") },
			{ EArenaPickupType::ShieldLarge, 100.f, 100.f, 60.f, TEXT("super_shield_pack") },
			{ EArenaPickupType::DoubleDamage, 100.f, 30.f, 90.f, TEXT("udamage") },
			{ EArenaPickupType::Adrenaline, 100.f, 2.f, 30.f, TEXT("adrenaline") },
		};
		if (SubIndex >= UE_ARRAY_COUNT(Cases))
		{
			// health pack not taken at full health (stays available)
			FinishTest();
			return true;
		}
		const FPickCase& R = Cases[SubIndex];
		if (SubPhase == 0)
		{
			C->Health = R.Health;
			C->Shield = 0.f;
			C->SmallShield = 0.f;
			C->Adrenaline = 0.f;
			FTransform TM(FRotator::ZeroRotator, C->GetActorLocation() + FVector(300.f, 0.f, 0.f));
			AArenaPickup* P = GetWorld()->SpawnActorDeferred<AArenaPickup>(AArenaPickup::StaticClass(), TM, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			P->Setup(R.Type);
			P->FinishSpawning(TM);
			TrackedActor = P;
			TestActors.Add(P);
			NextSub();
		}
		else if (SubPhase == 1 && SubTime > 0.3f)
		{
			C->AddMovementInput(FVector::ForwardVector, 1.f);
			AArenaPickup* P = Cast<AArenaPickup>(TrackedActor.Get());
			if ((P && !P->IsAvailable()) || SubTime > 2.f)
			{
				float Got = C->Health;
				if (R.Type == EArenaPickupType::ShieldSmall || R.Type == EArenaPickupType::ShieldLarge) Got = C->Shield;
				if (R.Type == EArenaPickupType::DoubleDamage) Got = C->GetDoubleDamageRemaining();
				if (R.Type == EArenaPickupType::Adrenaline) Got = C->Adrenaline;
				Result(*FString::Printf(TEXT("%s_result"), R.Name), R.Expected, Got, TEXT(""), 0.2);
				Result(*FString::Printf(TEXT("%s_respawn_time"), R.Name), R.Respawn, P ? P->GetRespawnRemaining() : 0.f, TEXT("s"), 0.2);
				if (P)
				{
					P->Destroy();
				}
				ResetPawn(Origin, 0.f);
				NextSub();
			}
		}
		else if (SubPhase == 2 && SubTime > 0.8f)
		{
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}

	// ---------------------------------------------------------------- adrenaline combos
	if (T == TEXT("w_combos"))
	{
		static const EArenaDir Keys[4][4] = {
			{ EArenaDir::Forward, EArenaDir::Forward, EArenaDir::Forward, EArenaDir::Forward },
			{ EArenaDir::Back, EArenaDir::Back, EArenaDir::Back, EArenaDir::Back },
			{ EArenaDir::Forward, EArenaDir::Forward, EArenaDir::Back, EArenaDir::Back },
			{ EArenaDir::Right, EArenaDir::Right, EArenaDir::Left, EArenaDir::Left },
		};
		static const EArenaCombo Combos[4] = { EArenaCombo::Speed, EArenaCombo::Booster, EArenaCombo::Berserk, EArenaCombo::Invisible };
		static const TCHAR* Names[4] = { TEXT("speed"), TEXT("booster"), TEXT("berserk"), TEXT("invisible") };
		if (SubIndex >= 4)
		{
			FinishTest();
			return true;
		}
		if (SubPhase == 0)
		{
			C->ActivateCombo(EArenaCombo::None);
			C->Adrenaline = 100.f;
			C->Health = 100.f;
			C->Shield = 0.f;
			Counter = 0;
			NextSub();
		}
		else if (SubPhase == 1)
		{
			// one tap every 0.1 s (xPlayer: within 0.35 s of each other)
			if (SubTime >= 0.1f * float(Counter + 1) && Counter < 4)
			{
				C->OnDirectionTap(Keys[SubIndex][Counter]);
				++Counter;
			}
			if (Counter >= 4 && (C->GetActiveCombo() != EArenaCombo::None || SubTime > 1.f))
			{
				Result(*FString::Printf(TEXT("%s_activated"), Names[SubIndex]), 1.f, C->GetActiveCombo() == Combos[SubIndex] ? 1.f : 0.f, TEXT("flag"));
				MarkF = Now;
				MarkF2 = C->Adrenaline;
				NextSub();
			}
		}
		else if (SubPhase == 2 && SubTime >= 4.f)
		{
			const float Duration = AArenaCharacter::GetComboDuration(Combos[SubIndex]);
			Result(*FString::Printf(TEXT("%s_adrenaline_drain_4s"), Names[SubIndex]), 100.f * 4.f / Duration, MarkF2 - C->Adrenaline, TEXT("adrenaline"), 1.0);
			UCharacterMovementComponent* CM = C->GetCharacterMovement();
			if (Combos[SubIndex] == EArenaCombo::Speed)
			{
				Result(TEXT("speed_ground_speed"), ArenaTuning::GroundSpeed * 1.4f, CM->MaxWalkSpeed, TEXT("cm/s"), 0.5);
				Result(TEXT("speed_jumpz"), ArenaTuning::JumpZ * 1.5f, CM->JumpZVelocity, TEXT("cm/s"), 0.5);
				Result(TEXT("speed_air_control"), ArenaTuning::AirControl * 1.4f, CM->AirControl, TEXT("ratio"), 0.001);
			}
			else if (Combos[SubIndex] == EArenaCombo::Booster)
			{
				// +5 at once (ComboDefensive.StartEffect), then every 0.9 s
				Result(TEXT("booster_health_after_4s"), 100.f + 5.f * (1.f + FMath::FloorToFloat((Now - MarkF) / 0.9f)), C->Health, TEXT("hp"), 5.1);
			}
			else if (Combos[SubIndex] == EArenaCombo::Berserk)
			{
				Result(TEXT("berserk_refire_scale"), 0.75f, C->GetRefireScale(), TEXT("ratio"), 0.001);
			}
			C->ActivateCombo(EArenaCombo::None);
			++SubIndex;
			SubPhase = 0;
			SubTime = 0.f;
		}
		return true;
	}

	Note(TEXT("unknown weapon test"));
	FinishTest();
	return true;
}

#endif

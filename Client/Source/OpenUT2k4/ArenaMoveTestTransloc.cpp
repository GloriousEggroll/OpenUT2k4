// Translocator checks of the -MoveTest hook (non-shipping): w_transloc. Expected values are UT2004's (XWeapons
// TransLauncher / TransFire / TransRecall / TransBeacon defaults and script), measured on the test rig.
#include "ArenaMoveTest.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaFlag.h"
#include "ArenaLocalMessages.h"
#include "ArenaTranslocator.h"
#include "ArenaTypes.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"

#if !UE_BUILD_SHIPPING

namespace ArenaMoveTestTranslocLocal
{
	constexpr float UU = 2.f;
	AArenaTransBeacon* FindBeacon(UWorld* World, const AArenaCharacter* Owner)
	{
		for (TActorIterator<AArenaTransBeacon> It(World); It; ++It)
		{
			if (It->GetBeaconOwner() == Owner && !It->IsActorBeingDestroyed())
			{
				return *It;
			}
		}
		return nullptr;
	}
}

bool UArenaMoveTestSubsystem::TickTranslocTest(float DeltaTime)
{
	using namespace ArenaMoveTestTranslocLocal;
	AArenaCharacter* C = Pawn.Get();
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	auto NextSub = [this]() { ++SubPhase; SubTime = 0.f; };
	AWeap_Translocator* T = Cast<AWeap_Translocator>(C->FindWeapon(AWeap_Translocator::StaticClass()));
	AArenaCharacter* Victim = Cast<AArenaCharacter>(TrackedActor.Get());
	auto Press = [C](uint8 Mode) { C->StartFire(Mode); C->StopFire(Mode); };
	auto SpawnChar = [this, World](const FVector& At) -> AArenaCharacter*
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AArenaCharacter* A = World->SpawnActor<AArenaCharacter>(AArenaCharacter::StaticClass(), At, FRotator::ZeroRotator, P);
		if (A)
		{
			TestActors.Add(A);
		}
		return A;
	};

	if (SubPhase == 0)
	{
		C->GiveWeapon(AWeap_Translocator::StaticClass());
		T = Cast<AWeap_Translocator>(C->FindWeapon(AWeap_Translocator::StaticClass()));
		if (!T)
		{
			Result(TEXT("has_translocator"), 1.0, 0.0, TEXT("bool"), 0.1);
			FinishTest();
			return true;
		}
		C->SwitchToWeapon(T);
		T->SetCharge(ArenaTranslocator::AmmoChargeMax);
		// the telefrag victim exists from now on (its 2 s spawn protection must be over)
		TrackedActor = SpawnChar(Origin + FVector(1500.f, 1500.f, 120.f));
		ResetPawn(Origin, 0.f);
		C->Health = 1000.f;
		Note(TEXT("UT2004: TransLauncher AmmoChargeF 6 / AmmoChargeMax 6, AmmoChargeRate 0.4/s; TransFire / TransRecall FireRate 0.25; TransBeacon Speed 1200 UU/s, HitWall: v' = 0.3 x mirrored v, rests under 20 UU/s; DisruptionThreshold 65"));
		NextSub();
	}
	else if (SubPhase == 1 && SubTime > 1.0f)
	{
		Result(TEXT("start_charge"), ArenaTranslocator::AmmoChargeMax, T->GetCharge(), TEXT(""), 0.01);
		// ---- throw level along +X
		C->GetController()->SetControlRotation(FRotator(0.f, 0.f, 0.f));
		Press(0);
		Counter = 0;
		TrackedProjectile.Reset();
		NextSub();
	}
	else if (SubPhase == 2)
	{
		AArenaTransBeacon* B = FindBeacon(World, C);
		if (B && !TrackedProjectile.IsValid())
		{
			TrackedProjectile = B;
			Mark = B->GetActorLocation();
			MarkF = Now;
			LastProjectileLoc = B->GetVelocity();
			Result(TEXT("throw_speed"), ArenaTranslocator::BeaconSpeedUU * UU, B->GetVelocity().Size(), TEXT("cm/s"), 5.0);
			Result(TEXT("charge_after_throw_unchanged"), ArenaTranslocator::AmmoChargeMax, T->GetCharge(), TEXT(""), 0.01);
		}
		else if (B && Counter == 0 && Now - MarkF >= 0.2f && B->NumBounces == 0)
		{
			// from the launch: horizontal distance / flight time, and the drop below the straight line (z = vz0 t + g t^2 / 2)
			const float Tf = B->LastMoveTime - B->LaunchTime;
			const FVector D = B->GetActorLocation() - B->LaunchLocation;
			Result(TEXT("throw_horizontal_speed"), ArenaTranslocator::BeaconSpeedUU * UU, FVector(D.X, D.Y, 0.f).Size() / Tf, TEXT("cm/s"), 10.0);
			Result(TEXT("beacon_gravity"), -950.f * UU, 2.f * (D.Z - B->LaunchVelocity.Z * Tf) / (Tf * Tf), TEXT("cm/s2"), 20.0);
			Counter = 1;
			Shot(TEXT("beacon_flight"));
		}
		if (B)
		{
			if (B->NumBounces == 1 && Counter == 1)
			{
				// HitWall on the floor: v' = 0.3 x (v mirrored about the normal); MarkF2 holds the speed just before
				const FVector Pre = TrackedLoc;
				const FVector Mirrored(Pre.X, Pre.Y, -Pre.Z);
				Result(TEXT("first_bounce_speed_ratio"), 0.3, B->GetVelocity().Size() / FMath::Max(1.f, Mirrored.Size()), TEXT(""), 0.03);
				Counter = 2;
			}
			TrackedLoc = B->GetVelocity();
			if (B->IsResting() && Counter >= 1)
			{
				Result(TEXT("beacon_comes_to_rest"), 1.0, 1.0, TEXT("bool"), 0.1);
				Note(FString::Printf(TEXT("rest after %d bounces, %.2f s, %.0f cm from the throw"), B->NumBounces, Now - MarkF, FVector::Dist2D(B->GetActorLocation(), Mark)));
				Shot(TEXT("beacon_resting"));
				NextSub();
			}
		}
		if (SubTime > 5.f)
		{
			Result(TEXT("beacon_comes_to_rest"), 1.0, 0.0, TEXT("bool"), 0.1);
			NextSub();
		}
	}
	else if (SubPhase == 3 && SubTime > 0.4f)
	{
		// ---- fire again: recall (no charge used)
		Press(0);
		NextSub();
	}
	else if (SubPhase == 4 && SubTime > 0.2f)
	{
		Result(TEXT("recall_removes_beacon"), 1.0, FindBeacon(World, C) ? 0.0 : 1.0, TEXT("bool"), 0.1);
		Result(TEXT("charge_after_recall"), ArenaTranslocator::AmmoChargeMax, T->GetCharge(), TEXT(""), 0.01);
		// ---- throw, carry a flag, translocate
		AArenaFlag* Flag = World->SpawnActor<AArenaFlag>(AArenaFlag::StaticClass(), C->GetActorLocation(), FRotator::ZeroRotator);
		if (Flag)
		{
			TestActors.Add(Flag);
			Flag->SetTeam(1);
			Flag->SetNetState(EArenaFlagState::Carried, C, C->GetActorLocation());
		}
		C->GetController()->SetControlRotation(FRotator(20.f, 0.f, 0.f));
		Press(0);
		NextSub();
	}
	else if (SubPhase == 5)
	{
		AArenaTransBeacon* B = FindBeacon(World, C);
		if (B && B->IsResting() && SubTime > 0.3f)
		{
			Mark = B->GetActorLocation();
			StartLoc = C->GetActorLocation();
			Press(1);
			MarkF = Now;
			NextSub();
		}
		else if (SubTime > 5.f)
		{
			Result(TEXT("teleport_to_beacon"), 0.0, 1.0, TEXT("cm"));
			NextSub();
		}
	}
	else if (SubPhase == 6 && SubTime > 0.05f && Counter < 10)
	{
		const float Expected = ArenaTranslocator::AmmoChargeMax - 1.f;
		Result(TEXT("teleport_distance_to_beacon_2d"), 0.0, FVector::Dist2D(C->GetActorLocation(), Mark), TEXT("cm"), 5.0);
		Result(TEXT("teleport_height_above_beacon"), C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), C->GetActorLocation().Z - Mark.Z, TEXT("cm"), 6.0);
		Result(TEXT("charge_after_translocate"), Expected, T->GetCharge(), TEXT(""), 0.05);
		Result(TEXT("beacon_gone_after_translocate"), 1.0, FindBeacon(World, C) ? 0.0 : 1.0, TEXT("bool"), 0.1);
		bool bDropped = false;
		float DropDist = 1e9f;
		for (TActorIterator<AArenaFlag> It(World); It; ++It)
		{
			bDropped = It->GetState() == EArenaFlagState::Dropped;
			DropDist = FVector::Dist2D(It->GetFlagLocation(), StartLoc);
		}
		Result(TEXT("flag_dropped_on_translocate"), 1.0, bDropped && !C->CarriedFlag ? 1.0 : 0.0, TEXT("flag"), 0.1);
		Result(TEXT("flag_left_at_the_start"), 0.0, DropDist, TEXT("cm"), 60.0);
		Counter = 10;
		MarkF2 = T->GetCharge();
		MarkF = Now;
	}
	else if (SubPhase == 6 && Counter == 10)
	{
		// ---- recharge: AmmoChargeRate 0.4/s -> one charge in 2.5 s
		if (T->GetCharge() >= ArenaTranslocator::AmmoChargeMax - 0.001f)
		{
			Result(TEXT("recharge_one_charge_time"), 1.f / ArenaTranslocator::AmmoChargeRate, (Now - MarkF) + (MarkF2 - (ArenaTranslocator::AmmoChargeMax - 1.f)) / ArenaTranslocator::AmmoChargeRate,
				TEXT("s"), 0.05);
			Shot(TEXT("charge_full"));
			Counter = 0;
			NextSub();
		}
		else if (SubTime > 1.2f && SubTime < 1.25f)
		{
			Shot(TEXT("charge_bar"));
		}
		else if (SubTime > 6.f)
		{
			Result(TEXT("recharge_one_charge_time"), 2.5, SubTime, TEXT("s"), 0.05);
			NextSub();
		}
	}
	else if (SubPhase == 7)
	{
		// ---- the throw needs a whole charge (TransFire.AllowFire)
		T->SetCharge(0.5f);
		Press(0);
		NextSub();
	}
	else if (SubPhase == 8 && SubTime > 0.3f)
	{
		Result(TEXT("no_throw_below_one_charge"), 0.0, FindBeacon(World, C) ? 1.0 : 0.0, TEXT("bool"), 0.1);
		T->SetCharge(ArenaTranslocator::AmmoChargeMax);
		// ---- telefrag: the victim stands where the beacon lands (a clear part of the rig)
		ResetPawn(Origin + FVector(-1500.f, 1200.f, 0.f), 0.f);
		C->Health = 1000.f;
		NextSub();
	}
	else if (SubPhase == 9 && SubTime > 0.6f)
	{
		C->GetController()->SetControlRotation(FRotator(20.f, 0.f, 0.f));
		Press(0);
		NextSub();
	}
	else if (SubPhase == 10)
	{
		AArenaTransBeacon* B = FindBeacon(World, C);
		if (B && B->IsResting() && SubTime > 0.3f && Victim)
		{
			Victim->SetActorLocation(B->GetActorLocation() + FVector(30.f, 0.f, Victim->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.f), false, nullptr,
				ETeleportType::TeleportPhysics);
			Victim->Health = 100.f;
			MarkF = float(ArenaTranslocator::Stats().Telefrags);
			NextSub();
		}
		else if (SubTime > 5.f)
		{
			Result(TEXT("telefrag_victim_dies"), 1.0, 0.0, TEXT("bool"), 0.1);
			SubPhase = 12;
		}
	}
	else if (SubPhase == 11 && SubTime > 0.4f)
	{
		Note(FString::Printf(TEXT("victim spawned %.1f s ago (spawn protection 2 s)"), Now - (Victim ? Victim->GetSpawnTime() : 0.f)));
		Press(1);
		NextSub();
	}
	else if (SubPhase == 12 && SubTime > 0.3f)
	{
		Result(TEXT("telefrag_victim_dies"), 1.0, Victim && Victim->IsDead() ? 1.0 : 0.0, TEXT("bool"), 0.1);
		Result(TEXT("telefrag_count"), MarkF + 1.0, ArenaTranslocator::Stats().Telefrags, TEXT(""));
		Result(TEXT("translocator_alive_after_telefrag"), 1.0, C->IsDead() ? 0.0 : 1.0, TEXT("bool"), 0.1);
		// ---- disruption: a second translocator owner's beacon takes 60, then 10 more damage; translocating gibs it
		AArenaCharacter* Subject = SpawnChar(Origin + FVector(-1000.f, 1500.f, 120.f));
		TrackedActor = Subject;
		if (Subject)
		{
			Subject->GiveWeapon(AWeap_Translocator::StaticClass());
			if (AArenaWeapon* W = Subject->FindWeapon(AWeap_Translocator::StaticClass()))
			{
				Subject->SwitchToWeapon(W);
			}
		}
		NextSub();
	}
	else if (SubPhase == 13 && SubTime > 1.f)
	{
		AArenaCharacter* Subject = Victim;
		AWeap_Translocator* ST = Subject ? Cast<AWeap_Translocator>(Subject->FindWeapon(AWeap_Translocator::StaticClass())) : nullptr;
		if (!ST)
		{
			Result(TEXT("disrupted_beacon_gibs_owner"), 1.0, 0.0, TEXT("bool"), 0.1);
			SubPhase = 16;
			return true;
		}
		ST->BotThrow(Subject->GetActorLocation() + FVector(600.f, 0.f, -60.f), nullptr, FVector::ZeroVector, 0.f, 0.f, false);
		NextSub();
	}
	else if (SubPhase == 14)
	{
		AArenaTransBeacon* B = Victim ? FindBeacon(World, Victim) : nullptr;
		if (B && B->IsResting() && SubTime > 0.2f)
		{
			ArenaDamage::ApplyDamage(B, 60.f, C->GetController(), C, FArenaHitInfo());
			Result(TEXT("disrupted_after_60_damage"), 0.0, B->Disrupted() ? 1.0 : 0.0, TEXT("bool"), 0.1);
			ArenaDamage::ApplyDamage(B, 10.f, C->GetController(), C, FArenaHitInfo());
			Result(TEXT("disrupted_after_70_damage"), 1.0, B->Disrupted() ? 1.0 : 0.0, TEXT("bool"), 0.1);
			Shot(TEXT("disrupted_beacon"));
			MarkF = float(ArenaTranslocator::Stats().Gibs);
			NextSub();
		}
		else if (SubTime > 5.f)
		{
			Result(TEXT("disrupted_beacon_gibs_owner"), 1.0, 0.0, TEXT("bool"), 0.1);
			SubPhase = 16;
		}
	}
	else if (SubPhase == 15 && SubTime > 0.7f)
	{
		AWeap_Translocator* ST = Victim ? Cast<AWeap_Translocator>(Victim->FindWeapon(AWeap_Translocator::StaticClass())) : nullptr;
		const FVector Before = Victim ? Victim->GetActorLocation() : FVector::ZeroVector;
		if (ST)
		{
			ST->Translocate();
		}
		Result(TEXT("disrupted_beacon_gibs_owner"), 1.0, Victim && Victim->IsDead() ? 1.0 : 0.0, TEXT("bool"), 0.1);
		Result(TEXT("gibbed_where_it_stood"), 0.0, Victim ? FVector::Dist(Victim->GetActorLocation(), Before) : 1e6, TEXT("cm"), 5.0);
		Result(TEXT("gib_count"), MarkF + 1.0, ArenaTranslocator::Stats().Gibs, TEXT(""));
		NextSub();
	}
	else if (SubPhase == 16 && SubTime > 0.5f)
	{
		// death messages: the stock damage types' strings (not Engine.DamageType's generic one)
		const FString Generic = ArenaLocalMessages::GetDeathString(TEXT("K"), TEXT("V"), TEXT("no such weapon"), false);
		const FString Tele = ArenaLocalMessages::GetDeathString(TEXT("K"), TEXT("V"), GetDefault<AWeap_Translocator>()->WeaponName, false);
		const FString Telefragged = ArenaLocalMessages::GetDeathString(TEXT("K"), TEXT("V"), ArenaTranslocator::TelefraggedName(), false);
		const FString Gib = ArenaLocalMessages::GetDeathString(FString(), TEXT("V"), ArenaTranslocator::GibbedName(), false);
		const FString SelfGib = ArenaLocalMessages::GetDeathString(FString(), TEXT("V"), GetDefault<AWeap_Translocator>()->WeaponName, false);
		Note(FString::Printf(TEXT("death messages: telefrag \"%s\", disrupted (other weapon) \"%s\", nobody \"%s\", own disrupted beacon \"%s\", generic \"%s\""),
			*Tele, *Telefragged, *Gib, *SelfGib, *Generic));
		Result(TEXT("telefrag_message_is_the_damage_types"), 1.0, Tele != Generic && !Tele.IsEmpty() ? 1.0 : 0.0, TEXT("bool"), 0.1);
		Result(TEXT("telefragged_message_is_the_damage_types"), 1.0, Telefragged != Generic && !Telefragged.IsEmpty() ? 1.0 : 0.0, TEXT("bool"), 0.1);
		if (AArenaWeapon* W = C->FindWeapon(AWeap_Translocator::StaticClass()))
		{
			C->RemoveWeapon(W);
			W->Destroy();
		}
		FinishTest();
	}
	return true;
}

#endif

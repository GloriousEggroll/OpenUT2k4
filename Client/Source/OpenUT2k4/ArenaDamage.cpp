#include "ArenaDamage.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaPlayerController.h"
#include "ArenaPlayerState.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "Engine/OverlapResult.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Controller.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/Pawn.h"

namespace
{
	/** True if A and B are on opposite sides (or teams don't apply). */
	bool AreEnemies(UWorld* World, AController* A, AController* B)
	{
		if (!A || !B || A == B)
		{
			return false;
		}
		if (World)
		{
			if (const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>())
			{
				return !GM->AreOnSameTeam(A, B);
			}
		}
		const AArenaPlayerState* PA = A->GetPlayerState<AArenaPlayerState>();
		const AArenaPlayerState* PB = B->GetPlayerState<AArenaPlayerState>();
		if (PA && PB && PA->Team >= 0 && PA->Team == PB->Team)
		{
			return false;
		}
		return true;
	}

	/** Line of sight from Origin to Target ignoring pawns and projectiles. */
	bool HasRadialLOS(UWorld* World, const FVector& Origin, const FVector& Target, const AActor* Victim, const AActor* DamageCauser)
	{
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaRadialLOS), false);
		if (DamageCauser)
		{
			QP.AddIgnoredActor(DamageCauser);
		}
		if (Victim)
		{
			QP.AddIgnoredActor(Victim);
		}
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		RP.CollisionResponse.SetResponse(ECC_Projectile, ECR_Ignore);
		RP.CollisionResponse.SetResponse(ECC_PhysicsBody, ECR_Ignore);
		FHitResult Hit;
		return !World->LineTraceSingleByChannel(Hit, Origin, Target, ECC_Visibility, QP, RP);
	}
}

float ArenaDamage::ApplyDamage(AActor* Victim, float Damage, AController* InstigatorController, AActor* DamageCauser, const FArenaHitInfo& Info)
{
	if (!IsValid(Victim) || Damage <= 0.f)
	{
		return 0.f;
	}

	if (AArenaCharacter* Char = Cast<AArenaCharacter>(Victim))
	{
		if (Char->IsDead())
		{
			return 0.f;
		}
		AController* VictimController = Char->GetController();
		const bool bEnemy = AreEnemies(Char->GetWorld(), InstigatorController, VictimController)
			|| (InstigatorController && !VictimController && InstigatorController->GetPawn() != Char);
		const bool bProtected = Char->HasSpawnProtection();

		const float Applied = Char->ReceiveArenaDamage(Damage, InstigatorController, DamageCauser, Info);

		if (bEnemy && !bProtected)
		{
			if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(InstigatorController))
			{
				PC->NotifyHitEnemy(Applied > 0.f ? Applied : Damage, Info.bHeadshot);
			}
		}
		return Applied;
	}

	// Generic actor: plain point damage event.
	FPointDamageEvent Event;
	Event.Damage = Damage;
	Event.DamageTypeClass = UDamageType::StaticClass();
	Event.ShotDirection = Info.Momentum.GetSafeNormal();
	Event.HitInfo.Location = Victim->GetActorLocation();
	Event.HitInfo.ImpactPoint = Victim->GetActorLocation();
	return Victim->TakeDamage(Damage, Event, InstigatorController, DamageCauser);
}

void ArenaDamage::ApplyRadialDamage(UWorld* World, const FVector& Origin, float Damage, float Radius, float Momentum,
	AController* InstigatorController, AActor* DamageCauser, const FString& WeaponName, float SelfDamageScale, AActor* IgnoreActor)
{
	if (!World || Radius <= 0.f || (Damage <= 0.f && Momentum <= 0.f))
	{
		return;
	}

	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjParams.AddObjectTypesToQuery(ECC_PhysicsBody);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaRadialDamage), false);
	if (DamageCauser)
	{
		QP.AddIgnoredActor(DamageCauser);
	}
	if (IgnoreActor)
	{
		QP.AddIgnoredActor(IgnoreActor);
	}

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, Origin, FQuat::Identity, ObjParams, FCollisionShape::MakeSphere(Radius), QP);

	// Dedupe actors, keep the component closest to the origin for each.
	TMap<AActor*, TPair<UPrimitiveComponent*, float>> Victims;
	for (const FOverlapResult& O : Overlaps)
	{
		AActor* A = O.GetActor();
		UPrimitiveComponent* C = O.GetComponent();
		if (!IsValid(A) || A == DamageCauser || A == IgnoreActor || !A->CanBeDamaged())
		{
			continue;
		}
		float Dist = -1.f;
		if (C)
		{
			FVector Closest;
			Dist = C->GetDistanceToCollision(Origin, Closest);
		}
		if (Dist < 0.f)
		{
			Dist = FVector::Dist(Origin, A->GetActorLocation());
		}
		if (TPair<UPrimitiveComponent*, float>* Existing = Victims.Find(A))
		{
			if (Dist < Existing->Value)
			{
				*Existing = TPair<UPrimitiveComponent*, float>(C, Dist);
			}
		}
		else
		{
			Victims.Add(A, TPair<UPrimitiveComponent*, float>(C, Dist));
		}
	}

	APawn* InstigatorPawn = InstigatorController ? InstigatorController->GetPawn() : nullptr;
	if (!InstigatorPawn && DamageCauser)
	{
		InstigatorPawn = DamageCauser->GetInstigator();
	}

	for (const TPair<AActor*, TPair<UPrimitiveComponent*, float>>& V : Victims)
	{
		AActor* Victim = V.Key;
		if (!IsValid(Victim))
		{
			continue;
		}

		// Line of sight to the centre, then to the upper body (feet behind a ledge still get hit).
		const FVector Centre = Victim->GetActorLocation();
		bool bVisible = HasRadialLOS(World, Origin, Centre, Victim, DamageCauser);
		if (!bVisible)
		{
			const FVector Box = Victim->GetComponentsBoundingBox().GetExtent();
			bVisible = HasRadialLOS(World, Origin, Centre + FVector(0.f, 0.f, Box.Z * 0.75f), Victim, DamageCauser);
		}
		if (!bVisible)
		{
			continue;
		}

		// UE2 Actor.HurtRadius: damageScale = 1 - max(0, (dist - CollisionRadius) / DamageRadius) with dist to the
		// victim's centre (pawns) and the push along the line from the explosion to that centre.
		const bool bSelf = InstigatorPawn && Victim == InstigatorPawn;
		float Dist = FMath::Max(0.f, V.Value.Value);
		if (const ACharacter* VictimChar = Cast<ACharacter>(Victim))
		{
			Dist = FMath::Max(0.f, FVector::Dist(Centre, Origin) - VictimChar->GetCapsuleComponent()->GetScaledCapsuleRadius());
		}
		const float Falloff = 1.f - FMath::Min(1.f, Dist / Radius);
		if (Falloff <= 0.f)
		{
			continue;
		}

		FVector Dir = Centre - Origin;
		Dir = Dir.SizeSquared() < 1.f ? FVector::UpVector : Dir.GetSafeNormal();

		FArenaHitInfo Info;
		Info.WeaponName = WeaponName;
		Info.bRadial = true;
		Info.bHeadshot = false;
		// Launch velocity for a standard pawn (UE2 MomentumTransfer / Mass); the pawn applies the UE2 rules.
		Info.Momentum = Dir * Momentum * Falloff;
		{
			float R = 50.f, H = 88.f;
			if (const ACharacter* VC = Cast<ACharacter>(Victim))
			{
				VC->GetCapsuleComponent()->GetScaledCapsuleSize(R, H);
			}
			Info.HitLocation = Centre - 0.5f * (H + R) * Dir;
		}

		float Dmg = FMath::FloorToFloat(Damage * Falloff);
		if (bSelf)
		{
			Dmg *= SelfDamageScale;
		}

		if (Dmg > 0.f)
		{
			ApplyDamage(Victim, Dmg, InstigatorController, DamageCauser, Info);
		}
		else if (AArenaCharacter* Char = Cast<AArenaCharacter>(Victim))
		{
			// Pure momentum (e.g. zero self damage): still push the pawn.
			if (!Char->IsDead() && !Info.Momentum.IsNearlyZero())
			{
				Char->TakeMomentumUE2(Info.Momentum, bSelf);
			}
		}
	}
}

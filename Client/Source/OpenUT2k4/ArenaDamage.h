#pragma once

#include "CoreMinimal.h"
#include "ArenaDamage.generated.h"

class AActor;
class AController;

/** Extra info attached to damage dealt by arena weapons (kill messages, headshots, momentum). */
USTRUCT()
struct FArenaHitInfo
{
	GENERATED_BODY()

	UPROPERTY() FString WeaponName;
	UPROPERTY() FVector Momentum = FVector::ZeroVector;
	/** Where the damage hit the victim (UE2 TakeDamage HitLocation); zero = unknown. */
	UPROPERTY() FVector HitLocation = FVector::ZeroVector;
	UPROPERTY() bool bHeadshot = false;
	UPROPERTY() bool bRadial = false;
};

namespace ArenaDamage
{
	/**
	 * Apply damage to an actor. For AArenaCharacter victims, routes to AArenaCharacter::ReceiveArenaDamage
	 * (handles shields, momentum, team damage, kill credit). Other actors get a plain TakeDamage call.
	 */
	OPENUT2K4_API float ApplyDamage(AActor* Victim, float Damage, AController* InstigatorController, AActor* DamageCauser, const FArenaHitInfo& Info);

	/**
	 * Radial damage like UE2 Actor.HurtRadius: scale = 1 - (distance to the victim's centre - its radius) / Radius,
	 * line-of-sight check (ECC_Visibility against world statics). Momentum (launch speed in cm/s for a standard pawn,
	 * UE2 MomentumTransfer / Mass x 2) is applied from Origin towards the victim's centre, times the scale. The
	 * instigator takes self damage (rocket jumps) times SelfDamageScale (UE2: full; AArenaCharacter applies the
	 * DeathMatch rule for human players and the 0.6 self momentum).
	 */
	OPENUT2K4_API void ApplyRadialDamage(UWorld* World, const FVector& Origin, float Damage, float Radius, float Momentum,
		AController* InstigatorController, AActor* DamageCauser, const FString& WeaponName, float SelfDamageScale = 1.f,
		AActor* IgnoreActor = nullptr);
}

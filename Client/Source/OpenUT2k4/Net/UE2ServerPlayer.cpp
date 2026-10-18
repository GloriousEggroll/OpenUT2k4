#include "UE2ServerPlayer.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/SceneComponent.h"

AUE2RemoteController::AUE2RemoteController()
{
	SetReplicates(false);
	PrimaryActorTick.bCanEverTick = false;
}

void AUE2RemoteController::GetPlayerViewPoint(FVector& OutLocation, FRotator& OutRotation) const
{
	if (const APawn* P = GetPawn())
	{
		OutLocation = P->GetPawnViewLocation();
	}
	else
	{
		OutLocation = GetRootComponent() ? GetRootComponent()->GetComponentLocation() : FVector::ZeroVector;
	}
	OutRotation = GetControlRotation();
}

void AUE2RemoteController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	// the client's moves drive the pawn (one physics step each, FUE2ServerWorld); no movement of its own in between
	if (ACharacter* C = Cast<ACharacter>(InPawn))
	{
		if (UCharacterMovementComponent* Move = C->GetCharacterMovement())
		{
			Move->SetComponentTickEnabled(false);
		}
	}
}

void AUE2RemoteController::OnUnPossess()
{
	if (ACharacter* C = Cast<ACharacter>(GetPawn()))
	{
		if (UCharacterMovementComponent* Move = C->GetCharacterMovement())
		{
			Move->SetComponentTickEnabled(true);
		}
	}
	Super::OnUnPossess();
}

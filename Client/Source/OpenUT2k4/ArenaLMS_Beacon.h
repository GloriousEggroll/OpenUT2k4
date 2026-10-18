#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaLMS_Beacon.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UPointLightComponent;

/**
 * Last Man Standing: the "exposed" beacon put on a player who stayed in one spot for too long. A tall light
 * column and a spinning pointer above the head, attached to the camper's pawn by UArenaRules_LMS (replicated).
 */
UCLASS()
class OPENUT2K4_API AArenaLMS_Beacon : public AActor
{
	GENERATED_BODY()

public:
	AArenaLMS_Beacon();

	virtual void Tick(float DeltaSeconds) override;

	static FLinearColor BeaconColor();

protected:
	virtual void BeginPlay() override;

	UStaticMeshComponent* AddPart(USceneComponent* Parent, class UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm,
		const FRotator& Rotation, class UMaterialInterface* Material, bool bOwnerNoSee);

	UPROPERTY() TObjectPtr<USceneComponent> BeaconRoot;
	UPROPERTY() TObjectPtr<USceneComponent> Spinner;
	UPROPERTY() TObjectPtr<UPointLightComponent> BeaconLight;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Column;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	float Age = 0.f;
};

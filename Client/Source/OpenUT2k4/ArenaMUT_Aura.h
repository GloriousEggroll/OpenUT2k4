#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaMUT_Aura.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UPointLightComponent;

/**
 * Mutant mode: the glowing aura around the current Mutant. Spawned by UArenaRules_MUT on the server with the
 * Mutant's pawn as owner and attached to it (replicated, so clients see it too). The parts are hidden for the
 * owner's own view (first person), everyone else sees a pulsing shell, a floor ring and orbiting motes.
 */
UCLASS()
class OPENUT2K4_API AArenaMUT_Aura : public AActor
{
	GENERATED_BODY()

public:
	AArenaMUT_Aura();

	virtual void Tick(float DeltaSeconds) override;

	/** Aura colour (shared with the HUD marker). */
	static FLinearColor AuraColor();

protected:
	virtual void BeginPlay() override;

	UStaticMeshComponent* AddPart(USceneComponent* Parent, class UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm,
		const FRotator& Rotation, class UMaterialInterface* Material);

	UPROPERTY() TObjectPtr<USceneComponent> AuraRoot;
	UPROPERTY() TObjectPtr<USceneComponent> OrbitRoot;
	UPROPERTY() TObjectPtr<UPointLightComponent> AuraLight;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Shell;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Ring;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;

	float Age = 0.f;
};

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaBR_Goal.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;

/**
 * Bombing Run goal hoop at a team's base. The team that owns it defends it; the other team scores by
 * carrying the ball into it (the carrier is sacrificed) or by throwing the ball through the ring.
 * Purely visual + geometry queries; the rules do the scoring. No collision (bots walk under / through it).
 */
UCLASS()
class OPENUT2K4_API AArenaBR_Goal : public AActor
{
	GENERATED_BODY()

public:
	AArenaBR_Goal();

	/** Configure before / right after spawning. FloorLocation is where bots walk to; HoopCenter may float. */
	void Setup(int32 InTeam, const FVector& InFloorLocation, const FVector& InHoopCenter, float FacingYaw);

	int32 GetTeam() const { return Team; }
	FVector GetFloorLocation() const { return FloorLocation; }
	FVector GetHoopCenter() const { return HoopCenter; }
	float GetHoopRadius() const { return HoopRadius; }
	/** The hoop is high above the floor: carriers cannot walk into it, they have to throw. */
	bool IsElevated() const { return HoopCenter.Z - FloorLocation.Z > 600.f; }

	/** Is a character (actor location = capsule centre) inside the scoring zone? */
	bool IsCharacterInside(const FVector& CharacterLocation) const;
	/** Does a ball flight segment pass through the ring? */
	bool SegmentPassesThrough(const FVector& From, const FVector& To) const;

	/** Short flash after a score. */
	void PlayScoreEffect();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void BuildVisuals();
	UStaticMeshComponent* AddPart(class UStaticMesh* Mesh, const FVector& RelLocation, const FRotator& RelRotation, const FVector& SizeCm, class UMaterialInterface* Material, bool bShadow);

	UPROPERTY() TObjectPtr<USceneComponent> Hoop;
	UPROPERTY() TObjectPtr<UPointLightComponent> Light;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> RingMaterial;

	int32 Team = 0;
	FVector FloorLocation = FVector::ZeroVector;
	FVector HoopCenter = FVector::ZeroVector;
	float HoopRadius = 170.f;
	float Facing = 0.f;
	float FlashUntil = 0.f;
	bool bBuilt = false;
};

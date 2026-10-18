#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaDOM_ControlPoint.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;
class AArenaCharacter;

/**
 * Double Domination control point. Touch it to claim it for your team. Owner, enabled state and the timers
 * the HUD shows are replicated. Colours follow the owner (neutral white, team colours, grey while locked).
 */
UCLASS()
class OPENUT2K4_API AArenaDOM_ControlPoint : public AActor
{
	GENERATED_BODY()

public:
	AArenaDOM_ControlPoint();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** "A" / "B" */
	UPROPERTY(Replicated) FString Label;
	/** -1 neutral, 0 / 1 team */
	UPROPERTY(ReplicatedUsing = OnRep_State) int32 OwnerTeam = -1;
	UPROPERTY(ReplicatedUsing = OnRep_State) bool bEnabled = true;
	/** Server time when a disabled point becomes live again (HUD countdown). */
	UPROPERTY(Replicated) float EnableTime = 0.f;
	/** Server time when the team holding both points scores (0 = nobody holds both). */
	UPROPERTY(Replicated) float ScoreTime = 0.f;
	/** Who touched it last (for credit), server only. */
	TWeakObjectPtr<AController> LastToucher;

	// ---- server ----
	void SetOwnerTeam(int32 Team, AController* By);
	void SetDisabled(float UntilServerTime);
	void SetEnabled();
	/** Living characters standing on the point: first one found per team (null if none). */
	void GetTouching(AArenaCharacter* OutFirst[2]) const;
	static constexpr float TouchRadius = 170.f;

	FVector GetFloorLocation() const { return GetActorLocation(); }

	UFUNCTION() void OnRep_State();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void BuildVisuals();
	void UpdateVisuals();
	UStaticMeshComponent* AddPart(USceneComponent* Parent, class UStaticMesh* Mesh, const FVector& RelLocation, const FRotator& RelRotation, const FVector& SizeCm, class UMaterialInterface* Material, bool bShadow);

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Emblem;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> LabelText;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GlowMat;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> SurfaceMat;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BeamMat;
	bool bBuilt = false;
	/** Imported letter emblem and ring (materials follow the state). */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ArtLetter;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> ArtRing;
};

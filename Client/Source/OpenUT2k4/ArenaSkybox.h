#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaSkybox.generated.h"

class UPrimitiveComponent;
class USceneCaptureComponent2D;
class UStaticMeshComponent;
class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;

/**
 * UE2 3D sky box: UE2 draws the SkyZoneInfo's zone (a small separate room of the map) from the SkyZoneInfo's
 * location with the player's view rotation behind everything, wherever the BSP shows "fake backdrop" surfaces.
 *
 * Here: six 90 degree scene captures at the SkyZoneInfo render only the sky zone's primitives (its BSP chunks,
 * static meshes, emitters; registered with AddSkyPrimitive) into six render targets, refreshed at a low rate
 * (arena.SkyboxRefresh: seconds per full refresh, one face at a time, default 1; 0 = once), and a camera-centred cube of six unlit quads far away shows them,
 * so the sky has no parallax like UE2's. The sky zone's own primitives are hidden from the normal view (they would
 * float somewhere outside the playable area). Height fog stops before the cube (FogCutoffDistance).
 */
UCLASS()
class OPENUT2K4_API AArenaSkybox : public AActor
{
	GENERATED_BODY()

public:
	AArenaSkybox();

	/** A primitive of the sky zone: drawn into the sky box, hidden from the normal view. */
	void AddSkyPrimitive(UPrimitiveComponent* Comp);
	/** Size of each face render target (arena.SkyboxResolution, default 512). */
	static int32 GetResolution();
	/** Half size of the camera-centred cube (cm). */
	static constexpr float CubeHalfSize = 900000.f;
	int32 NumSkyPrimitives() const { return SkyPrimitives.Num(); }

	/** Spawns the sky box for a SkyZoneInfo location (only one per world). */
	static AArenaSkybox* Spawn(UWorld* World, const FVector& SkyViewLocation);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY() TArray<TObjectPtr<USceneCaptureComponent2D>> Captures;
	UPROPERTY() TArray<TObjectPtr<UTextureRenderTarget2D>> Targets;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Faces;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> FaceMaterials;
	UPROPERTY() TArray<TWeakObjectPtr<UPrimitiveComponent>> SkyPrimitives;
	UPROPERTY() TObjectPtr<USceneComponent> CubeRoot;
	FVector SkyLocation = FVector::ZeroVector;
	float CaptureTimer = 0.f;
	int32 CapturesDone = 0;
	int32 NextFace = 0;
	bool bBuilt = false;

	void BuildCube();
	void CaptureAll();
};

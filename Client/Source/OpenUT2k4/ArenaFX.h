#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaFX.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UPointLightComponent;
class UMaterialInstanceDynamic;
class USceneComponent;

/**
 * Short-lived procedural effect: one glowing shape mesh (optionally with a point light) that
 * scales between two sizes and fades out, then destroys itself. No collision, no shadows.
 * Created through the ArenaFX helpers below.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AArenaFXActor : public AActor
{
	GENERATED_BODY()

public:
	AArenaFXActor();

	/**
	 * Configure the effect (call right after spawning).
	 * @param InMesh            shape mesh to show (nullptr = light only)
	 * @param InStartScale      relative mesh scale at birth
	 * @param InEndScale        relative mesh scale at death
	 * @param InGlowIntensity   glow material intensity at birth
	 * @param InLightIntensity  point light intensity at birth (0 = no light)
	 * @param InLightRadius     point light attenuation radius
	 */
	void InitFX(UStaticMesh* InMesh, const FLinearColor& InColor, float InLifeTime, const FVector& InStartScale, const FVector& InEndScale,
		float InGlowIntensity, float InLightIntensity, float InLightRadius, bool bOnlyOwnerSee = false);

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

	/** Fade curve exponent (>1 = stays bright longer). */
	float FadePower = 1.f;

protected:
	void ApplyAlpha(float Alpha);

	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> MID;
	float LifeTime = 0.2f;
	float Age = 0.f;
	float GlowIntensity = 8.f;
	float LightIntensity = 0.f;
	FVector StartScale = FVector::OneVector;
	FVector EndScale = FVector::OneVector;
};

/**
 * Fire-and-forget visual effects built from shape meshes and the generated glow material.
 * With a Style (effect name from ArenaArtFX.cpp) the imported-art version is spawned instead when its
 * textures are available; otherwise, or with arena.ImportedArt 0, the procedural shapes below are used.
 */
namespace ArenaFX
{
	/** Glowing cylinder stretched from Start to End that thins and fades over LifeTime. */
	OPENUT2K4_API AArenaFXActor* SpawnBeam(UWorld* World, const FVector& Start, const FVector& End, const FLinearColor& Color, float Width = 4.f, float LifeTime = 0.2f,
		FName Style = NAME_None);

	/** Small expanding glow sphere plus a short point light. */
	OPENUT2K4_API AArenaFXActor* SpawnImpact(UWorld* World, const FVector& Location, const FVector& Normal, const FLinearColor& Color, float Radius = 20.f,
		FName Style = NAME_None);

	/** Large expanding fireball + hot core + bright light. Radius ~ damage radius. */
	OPENUT2K4_API AArenaFXActor* SpawnExplosion(UWorld* World, const FVector& Location, const FLinearColor& Color, float Radius = 300.f,
		FName Style = NAME_None, const FVector& Normal = FVector::UpVector);

	/** Quick flash attached to a component (e.g. a weapon view model). Only the owner sees the mesh when bOnlyOwnerSee. */
	OPENUT2K4_API AArenaFXActor* SpawnMuzzleFlash(USceneComponent* AttachTo, const FLinearColor& Color, const FVector& RelativeOffset = FVector::ZeroVector,
		float Scale = 1.f, bool bOnlyOwnerSee = true, FName Style = NAME_None);

	/** Place/scale a (100 cm, Z-aligned) cylinder mesh component in world space so it spans Start..End. */
	OPENUT2K4_API void OrientBeamMesh(UStaticMeshComponent* BeamMesh, const FVector& Start, const FVector& End, float Width);
}

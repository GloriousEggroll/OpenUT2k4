#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaRagdoll.generated.h"

class UPhysicsAsset;
class USkeletalMesh;
class UStaticMeshComponent;
class USphereComponent;

/**
 * Ragdoll deaths and gibs.
 *
 * UT2004 used Karma ragdolls with per-skeleton collision data (the KarmaData .ka files are not converted). Here the physics
 * asset is built at runtime from the skeleton itself, so it works the same for meshes built from the content cache
 * (no physics asset) and for cooked imports, and needs no editor code (Shipping-safe: bodies are plain capsules and
 * spheres, which need no collision cooking):
 *  - bodies on the major bones of a biped (pelvis, spine, head, upper arm, forearm, hand, thigh, calf, foot; bone
 *    names matched case-insensitively, e.g. "bip01-l-forearm"), a capsule along each limb segment, the torso as
 *    capsules across the hips / chest, a sphere for the head; sizes are proportional to the skeleton's height,
 *  - a constraint from every body to the nearest ancestor body (frames at the child bone, cone / twist limits per
 *    joint type), no collision between jointed, sibling and grandparent bodies,
 *  - skeletons that are not bipeds (fewer than six matching bones, e.g. vertex-animated monsters) get none.
 * The asset is cached per skeletal mesh for the session.
 *
 * Gibs (AArenaGib): UT2004's xPawn.SpawnGibs pieces (XEffects Gib<Organic|Bot><Part> static meshes, fitted to
 * body-part sizes) thrown from the body with the killing momentum, a blood (or spark) burst and a gib sound.
 */
namespace ArenaRagdoll
{
	/** Physics asset for a mesh (built on first use and cached). nullptr when the skeleton is not a biped. */
	OPENUT2K4_API UPhysicsAsset* GetPhysicsAsset(USkeletalMesh* Mesh);

	enum class EGibKind : uint8
	{
		Organic,     // red blood
		OrganicAlt,  // green blood (aliens)
		Robot        // metal parts and sparks
	};

	/**
	 * Throws a full set of gibs (UT2004: 2 torso, 2 head, 2 upper arm, 2 forearm, plus calves / hands when detailed)
	 * from Location with Velocity (body velocity + killing momentum). Returns false when no gib meshes are available.
	 */
	OPENUT2K4_API bool SpawnGibs(UWorld* World, const FVector& Location, const FVector& Velocity, EGibKind Kind, float SizeScale = 1.f);

	/**
	 * Test hook (non-shipping), -AutoDeathCam[=<count>]: the first <count> deaths (default 6) are each filmed by a
	 * camera that follows the body, with screenshots at 0.05, 0.5, 1.5 and 4 s (Saved/Screenshots/death_<n>_<step>.png).
	 */
	OPENUT2K4_API void DebugNotifyDeath(AActor* Body, const FString& Label);
}

/**
 * One flying body part that disappears after a few seconds: a simulated sphere (the gib meshes have no simple
 * collision, and converted meshes only complex collision) carrying the gib mesh.
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AArenaGib : public AActor
{
	GENERATED_BODY()

public:
	AArenaGib();

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Body;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
};

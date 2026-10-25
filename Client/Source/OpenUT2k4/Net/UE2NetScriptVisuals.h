#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UE2NetScriptVisuals.generated.h"

struct FUSObject;
class USceneComponent;

/**
 * What the server's client-side script shows (Docs/UE2Net.md "Mod client-side code"): the actors a mod's simulated code
 * spawns on our side in the UE2 client mirror (an Odin's ion cannon attachment, a weapon's muzzle flash emitter, a
 * projectile's trail ...). Each frame the script actors' drawing state is mirrored to a native actor:
 *
 *  - DrawType DT_Mesh (its skeletal mesh, playing what PlayAnim / LoopAnim ask: UScript/UScriptPresentation.h),
 *    DT_StaticMesh (its static mesh), Emitter / xEmitter classes (ArenaUE2Effects: the class's own emitters)
 *  - Skins, DrawScale x DrawScale3D, bHidden
 *  - where: on its Base's bone (AttachToBone: AttachmentBone, RelativeLocation / RelativeRotation), its Base being a
 *    replicated actor (the native component that shows it) or another script actor; else its Location / Rotation
 *
 * Sprites, projectors and the actors' own dynamic emitter changes (a charge beam's length set by script) are not drawn.
 */
class FUE2NetScriptVisuals
{
public:
	explicit FUE2NetScriptVisuals(UWorld* InWorld);
	~FUE2NetScriptVisuals();

	/**
	 * Mirrors Actors (live script actors spawned on our side). Mirrored(O) is the native component of a replicated actor's
	 * mirror, null for others.
	 */
	void Sync(const TArray<FUSObject*>& Actors, TFunctionRef<USceneComponent*(FUSObject*)> Mirrored);
	/** Emitter actors whose particles are done (UE2 Emitter.AutoDestroy): the caller destroys their script actors. */
	TArray<FUSObject*> TakeFinished() { return MoveTemp(Finished); }
	/** The native actor showing a script actor, if any. */
	AActor* Find(FUSObject* Obj) const;
	int32 Num() const { return Visuals.Num(); }
	/** For the summary log: visuals made by kind. */
	FString Describe() const;

private:
	struct FVisual;
	TWeakObjectPtr<UWorld> World;
	TMap<FUSObject*, TSharedPtr<FVisual>> Visuals;
	TArray<FUSObject*> Finished;
	int32 MadeMeshes = 0, MadeStaticMeshes = 0, MadeEmitters = 0, AnimsPlayed = 0;
};

/** Native body of a client-side script actor (FUE2NetScriptVisuals). */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetScriptVisual : public AActor
{
	GENERATED_BODY()

public:
	AUE2NetScriptVisual();

	UPROPERTY() TObjectPtr<USceneComponent> Root;
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> Skeletal;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> Static;
	/** The class's emitter actor (attached to this one). */
	UPROPERTY() TObjectPtr<AActor> Emitter;

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
};

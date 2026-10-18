#pragma once

#include "CoreMinimal.h"
#include "../ArenaTriggers.h"
#include "ArenaScriptActor.generated.h"

class UCapsuleComponent;

/**
 * Game-side body of an UnrealScript actor placed in a map (a class defined by the map or a custom package).
 * It joins the UE2 event system with the script actor's Tag, reports Touch / UnTouch of pawns (UE2 collision
 * cylinder) and weapon damage to the script VM, which runs the actor's own UnrealScript (see Docs/UnrealScript.md).
 */
UCLASS()
class OPENUT2K4_API AArenaScriptActor : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	/** Sets the UE2 values (Tag, Event, collision in cm) before BeginPlay. */
	void SetupScript(FName InTag, FName InEvent, float InRadius, float InHeight, bool bInCollide, bool bShootable);

	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	virtual void UnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void Touch(APawn* Other) override;
	virtual void UnTouch(APawn* Other) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** Shows a static mesh for the script actor (DrawType DT_StaticMesh). */
	void SetMesh(class UStaticMesh* Mesh, const FVector& Scale3D, const FRotator& Rotation);
	/** UE2 bHidden. */
	void SetScriptHidden(bool bHide);

	/** Opaque VM object (FUSObject*), owned by the script world. */
	void* ScriptObject = nullptr;

protected:
	UPROPERTY() TObjectPtr<UCapsuleComponent> ShootCollision;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> MeshComponent;
};

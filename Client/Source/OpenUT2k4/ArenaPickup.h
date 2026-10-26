#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaPickup.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class AArenaCharacter;
class AArenaWeapon;

UENUM(BlueprintType)
enum class EArenaPickupType : uint8
{
	HealthVial,      // +5 up to 199
	HealthPack,      // +25 up to 100
	SuperHealth,     // +100 up to 199
	ShieldSmall,     // +50 shield
	ShieldLarge,     // +100 shield
	DoubleDamage,
	Adrenaline,      // +2 adrenaline
	Weapon,          // WeaponClass, stays (weapon stay rules)
	Ammo             // ammo for WeaponClass
};

/**
 * Generic floating pickup with a procedural model, respawn timer and bot desirability.
 * Spawn with SpawnActorDeferred, call Setup(), then FinishSpawning (or call Setup before BeginPlay).
 */
UCLASS()
class OPENUT2K4_API AArenaPickup : public AActor
{
	GENERATED_BODY()

public:
	AArenaPickup();

	void Setup(EArenaPickupType InType, TSubclassOf<AArenaWeapon> InWeaponClass = nullptr);
	/** Whether the item sits on a pickup base / charger (imported maps know this; otherwise it depends on the type). */
	void SetOnBase(bool bOnBase);

	EArenaPickupType GetPickupType() const { return PickupType; }
	TSubclassOf<AArenaWeapon> GetWeaponClass() const { return WeaponClass; }
	bool IsAvailable() const { return bAvailable; }
	/** Seconds until available again (0 if available). */
	float GetRespawnRemaining() const;
	/** 0..1+ how much this bot would like this item right now (0 = useless). */
	float GetBotDesire(const AArenaCharacter* Bot) const;
	FString GetDisplayName() const;
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): the server's state of this item. Shown / hidden only; no local
	 * respawn timer runs afterwards.
	 */
	void SetNetAvailable(bool bNewAvailable);
	/** Net play: a mod pickup's own StaticMesh in place of our item mesh, fitted to its size. */
	void SetNetItemMesh(class UStaticMesh* Mesh);
	/** Net play: a mod pickup's PrePivot (cm, unscaled): its item is drawn at -PrePivot in the item's own frame. */
	void SetNetItemOffset(const FVector& Offset);

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Trigger;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> ModelRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	UFUNCTION() void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/** Apply to character. Returns true if consumed. */
	bool GiveTo(AArenaCharacter* Character);
	void SetAvailable(bool bNewAvailable);
	void BuildModel();
	float GetRespawnTime() const;

	/** Re-test characters already standing in the trigger (after respawn). */
	void CheckOverlappingCharacters();
	/** Adds one shape to the procedural model under ModelRoot. */
	UStaticMeshComponent* AddModelPart(class UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& SizeCm, class UMaterialInterface* Material);

	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ModelParts;
	/** Imported pickup base (stays when the item is taken). */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> BaseMesh;
	bool bModelBuilt = false;
	/** SetNetItemOffset: what was added to the item's place. */
	FVector NetItemOffset = FVector::ZeroVector;
	/** The imported item mesh replaced the procedural parts (AddModelPart adds nothing). */
	bool bArtModel = false;
	/** -1 = by type, 0 = no base, 1 = base. */
	int8 BaseHint = -1;

	EArenaPickupType PickupType = EArenaPickupType::HealthPack;
	UPROPERTY() TSubclassOf<AArenaWeapon> WeaponClass;
	bool bAvailable = true;
	float RespawnAt = 0.f;
	float BobPhase = 0.f;
};

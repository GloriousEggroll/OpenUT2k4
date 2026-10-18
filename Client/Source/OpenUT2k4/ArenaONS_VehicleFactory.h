#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaONS_VehicleFactory.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class AArenaONS_PowerNode;

/**
 * Vehicle bay: spawns one vehicle of its type for the team that owns its node / core, and respawns it
 * RespawnTime seconds after it was destroyed. Inactive while the owning node is not built.
 */
UCLASS()
class OPENUT2K4_API AArenaONS_VehicleFactory : public AActor
{
	GENERATED_BODY()

public:
	AArenaONS_VehicleFactory();

	void InitFactory(EArenaONS_VehicleType InType, AArenaONS_PowerNode* InOwnerNode, float InRespawnTime);
	EArenaONS_VehicleType GetVehicleType() const { return VehicleType; }
	AArenaONS_PowerNode* GetOwnerNode() const { return OwnerNode.Get(); }
	AArenaONS_Vehicle* GetVehicle() const { return Vehicle.Get(); }
	/** Team that may currently use this factory (-1 when inactive). */
	int32 GetActiveTeam() const;

	/** Server: spawn now if possible (clear spot found). */
	AArenaONS_Vehicle* SpawnVehicle();
	/** Server: destroy the current vehicle (if unoccupied) and spawn again (round reset). */
	void ResetFactory();
	/** Called by the vehicle when it is destroyed. */
	void NotifyVehicleDestroyed(AArenaONS_Vehicle* InVehicle);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	void UpdateLook();
	/** Find a clear floor spot near the factory for the vehicle's box. */
	bool FindSpawnTransform(FTransform& Out) const;

	EArenaONS_VehicleType VehicleType = EArenaONS_VehicleType::Dustrunner;
	TWeakObjectPtr<AArenaONS_PowerNode> OwnerNode;
	TWeakObjectPtr<AArenaONS_Vehicle> Vehicle;
	float RespawnTime = 15.f;
	float NextSpawnTime = 0.f;
	int32 LookTeam = -2;
	int32 SpawnFailures = 0;

	UPROPERTY() TObjectPtr<UStaticMeshComponent> Pad;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Ring;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RingMat;
	/** Hologram of the vehicle model shown on the bay while it is being rebuilt (art models only). */
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> Hologram;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> HologramMat;
	void UpdateHologram();
};

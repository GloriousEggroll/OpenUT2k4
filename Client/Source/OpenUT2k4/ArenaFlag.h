#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaFlag.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class AArenaCharacter;

UENUM(BlueprintType)
enum class EArenaFlagState : uint8
{
	Home,
	Carried,
	Dropped
};

/**
 * CTF flag. Placed by AArenaLevelBuilder at its team's base (base visual is part of the flag actor).
 * Enemy touch = take; own team touch while dropped = return; carrier touching own flag at home = capture.
 * Dropped flags return automatically after 20 s. Reports to AArenaGameMode::Flag* hooks.
 */
UCLASS()
class OPENUT2K4_API AArenaFlag : public AActor
{
	GENERATED_BODY()

public:
	AArenaFlag();

	void SetTeam(int32 InTeam);
	int32 GetTeam() const { return Team; }
	EArenaFlagState GetState() const { return State; }
	AArenaCharacter* GetHolder() const { return Holder; }
	FVector GetHomeLocation() const { return HomeLocation; }
	/** Where the flag currently is in the world (holder location if carried). */
	FVector GetFlagLocation() const;
	bool IsHome() const { return State == EArenaFlagState::Home; }

	/** Called by AArenaCharacter when its holder dies or is destroyed. */
	void Drop(AController* By);
	/** Send back to base (auto-return, own-team touch, match reset). */
	void ReturnHome(AController* By);
	/** Re-applies the team symbol (AArenaGameState::TeamSymbols changed). */
	void RefreshTeamSymbol();
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): show the server's state (home, carried by NewHolder, dropped at
	 * Location). From then on the flag runs no rules of its own (no touches, drops or auto returns).
	 */
	void SetNetState(EArenaFlagState NewState, AArenaCharacter* NewHolder, const FVector& Location);

	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Trigger;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	UFUNCTION() void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	void TryTouch(AArenaCharacter* Character);
	void BuildModel();

	int32 Team = 0;
	EArenaFlagState State = EArenaFlagState::Home;
	UPROPERTY() TObjectPtr<AArenaCharacter> Holder;
	UPROPERTY() TObjectPtr<USceneComponent> FlagModel;
	UPROPERTY() TObjectPtr<USceneComponent> BaseModel;
	FVector HomeLocation = FVector::ZeroVector;
	float DroppedTime = 0.f;

	/** Put the flag back at its base without notifying the game mode (used after a capture). */
	void SendHome();
	/** Reattach the flag model to this actor at a world location (home or dropped spot). */
	void PlaceFlagModel(const FVector& WorldLocation, float Yaw);
	bool IsMatchPlaying() const;
	class AArenaGameMode* GetArenaGameMode() const;
	UStaticMeshComponent* AddPart(USceneComponent* Parent, class UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, class UMaterialInterface* Material, bool bShadow);

	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	/** Imported waving flag (vertex-animated mesh) replacing the procedural pole and cloth. */
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> FlagSkel;
	/** Team symbol layer on the cloth: same mesh following FlagSkel's pose, translucent symbol material. */
	UPROPERTY() TObjectPtr<class USkeletalMeshComponent> FlagSymbol;
	void AddFlagSymbol(class USkeletalMesh* Mesh, int32 TeamIndex);
	/** Match symbol path the symbol layer was built with. */
	FString AppliedSymbol;
	UPROPERTY() TObjectPtr<UPointLightComponent> FlagLight;
	UPROPERTY() TObjectPtr<UPointLightComponent> BaseLight;
	bool bModelBuilt = false;
	bool bCheckOverlaps = false;
	/** Driven by a UT2004 server (SetNetState). */
	bool bNetDriven = false;
};

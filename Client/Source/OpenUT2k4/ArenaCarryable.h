#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaCarryable.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class AArenaCharacter;

UENUM(BlueprintType)
enum class EArenaCarryState : uint8
{
	Home,     // resting at its home spot
	Carried,  // attached to a character
	Dropped,  // lying where it fell / landed (auto-returns after GetAutoReturnTime)
	InFlight  // thrown (Throw), can be caught by touching it
};

/**
 * Reusable base for objects a character carries (bombing run ball, relics, ...). Handles the common
 * mechanics so a mode only writes its rules in the virtual hooks:
 *   - touch to pick up (CanBePickedUpBy / OnTouched), attach to the carrier's BodyRoot,
 *   - drop on the carrier's death or removal (AArenaCharacter::Die / EndPlay), fall to the floor,
 *   - throw with simple ballistic flight and bounces (Throw), catchable in flight,
 *   - auto-return home after GetAutoReturnTime() seconds on the ground, return when lost (no floor / kill Z).
 * Spawn it at its home floor location (the actor location is home), e.g. from UArenaGameRules::OnLevelBuilt.
 * Server-side logic; the model is built locally in BeginPlay. (AArenaFlag predates this class and keeps its own code.)
 */
UCLASS()
class OPENUT2K4_API AArenaCarryable : public AActor
{
	GENERATED_BODY()

public:
	AArenaCarryable();

	// ---- state ----
	EArenaCarryState GetCarryState() const { return CarryState; }
	bool IsHome() const { return CarryState == EArenaCarryState::Home; }
	bool IsCarried() const { return CarryState == EArenaCarryState::Carried; }
	AArenaCharacter* GetCarrier() const { return Carrier; }
	/** Team that owns the object (-1 = neutral, e.g. the ball). */
	int32 GetTeam() const { return Team; }
	void SetTeam(int32 InTeam);
	FVector GetHomeLocation() const { return HomeLocation; }
	/** Move the home spot (floor point); the object is sent there if it is home. */
	void SetHomeLocation(const FVector& FloorLocation);
	/** Where the object is now (carrier location while carried). */
	FVector GetObjectLocation() const;
	/** Seconds since it was dropped / landed (0 unless Dropped). */
	float GetTimeOnGround() const;

	// ---- actions (authority) ----
	/** Give it to a character (no CanBePickedUpBy check). Returns false if the character already carries something. */
	bool PickUp(AArenaCharacter* Character);
	/** Leave the carrier and fall to the floor below. By = who caused it (killer) or null. */
	void Drop(AController* By);
	/** Leave the carrier (if any) and fly from Start with Velocity. The thrower can't catch it for a moment. */
	void Throw(const FVector& Start, const FVector& Velocity, AController* By);
	/** Send it home and call OnReturned(By). */
	void ReturnHome(AController* By);
	/** Send it home silently (after a score, round reset ...). */
	void SendHome();
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): the server's state of its game object. No pickups, auto returns
	 * or rule hooks from then on. Carried: on NewCarrier; Dropped: resting at Location (floor point); InFlight: flying
	 * from Location with Velocity (simulated locally between the server's updates). OnNetStateChanged follows.
	 */
	void SetNetState(EArenaCarryState NewState, AArenaCharacter* NewCarrier, const FVector& Location, const FVector& Velocity);
	bool IsNetDriven() const { return bNetDriven; }

	// ---- rules (override in the mode's subclass) ----
	/** May this character take it now? Default: alive, team 0/1 or any when neutral, not carrying another object, match playing, not its thrower right after the throw. */
	virtual bool CanBePickedUpBy(AArenaCharacter* Character) const;
	/** A living character touched it while not carried. Default: PickUp when CanBePickedUpBy. */
	virtual void OnTouched(AArenaCharacter* Character);
	virtual void OnPickedUp(AArenaCharacter* Character) {}
	/** Dropped or thrown by its carrier (By = killer for deaths, the carrier for throws). */
	virtual void OnDropped(AController* By) {}
	/** Returned home (By = player who returned it, null for automatic returns). */
	virtual void OnReturned(AController* By) {}
	/** Came to rest after a throw. */
	virtual void OnLanded() {}
	/** SetNetState changed the state (visuals only: no rules). */
	virtual void OnNetStateChanged() {}
	/** Seconds on the ground before auto return; 0 = never. */
	virtual float GetAutoReturnTime() const { return AutoReturnTime; }
	/** Builds the visual model under Model (default: a glowing sphere in the team colour). */
	virtual void BuildModel();
	/** HUD line shown to the carrier, e.g. "YOU HAVE THE BALL". */
	virtual FString GetCarrierHudText() const;

	// ---- tuning ----
	UPROPERTY(EditAnywhere) FString DisplayName = TEXT("Objective");
	UPROPERTY(EditAnywhere) float AutoReturnTime = 20.f;
	/** Model placement relative to the carrier's BodyRoot (which sits at the feet). */
	UPROPERTY(EditAnywhere) FVector CarryOffset = FVector(30.f, 25.f, 95.f);
	UPROPERTY(EditAnywhere) FRotator CarryRotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere) float CarryScale = 0.8f;
	/** Height of the model's origin above the floor when resting. */
	UPROPERTY(EditAnywhere) float RestHeight = 50.f;
	UPROPERTY(EditAnywhere) float FlightGravityScale = 1.f;
	UPROPERTY(EditAnywhere) float Bounciness = 0.45f;
	UPROPERTY(EditAnywhere) float FlightRadius = 20.f;
	/** Thrower can't catch it for this long. */
	UPROPERTY(EditAnywhere) float RepickDelay = 0.6f;

	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Model;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Trigger;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	UFUNCTION() void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	void TryTouch(AArenaCharacter* Character);
	/** Detach from the carrier and clear its CarriedObject. */
	void ReleaseFromCarrier();
	/** Reattach the model to this actor at a world location (resting). */
	void PlaceModel(const FVector& WorldLocation, float Yaw);
	void TickFlight(float DeltaSeconds);
	bool IsMatchPlaying() const;
	/** Adds one shape under Model; for BuildModel overrides. */
	UStaticMeshComponent* AddModelPart(class UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, class UMaterialInterface* Material, bool bShadow = true);

	UPROPERTY() TObjectPtr<AArenaCharacter> Carrier;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ModelParts;
	EArenaCarryState CarryState = EArenaCarryState::Home;
	int32 Team = -1;
	FVector HomeLocation = FVector::ZeroVector;
	float DroppedTime = 0.f;
	FVector FlightVelocity = FVector::ZeroVector;
	float FlightStartTime = 0.f;
	TWeakObjectPtr<AArenaCharacter> LastCarrier;
	float NoRepickUntil = 0.f;
	bool bCheckOverlaps = false;
	bool bModelBuilt = false;
	/** Driven by a UT2004 server's state (SetNetState). */
	bool bNetDriven = false;
};

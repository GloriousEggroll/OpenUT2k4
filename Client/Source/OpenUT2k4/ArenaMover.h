#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaUE2World.h"
#include "ArenaMover.generated.h"

class UStaticMeshComponent;
class UAudioComponent;
class USoundBase;
class APawn;

/** UE2 Mover.InitialState. */
UENUM()
enum class EArenaMoverState : uint8
{
	BumpOpenTimed,
	BumpButton,
	StandOpenTimed,
	TriggerOpenTimed,
	TriggerToggle,
	TriggerControl,
	TriggerPound,
	ConstantLoop,
	LeadInOutLooper,
	/** Moves only when told to (RotatingMover style, ServerIdle) */
	None,
	/** UT2004 TriggerAdvance: each trigger moves it on to the next key; it never returns on its own */
	TriggerAdvance
};

/**
 * UE2 Mover: a static mesh interpolated between up to 24 keyframes (BasePos + KeyPos[i], BaseRot + KeyRot[i]).
 * Each segment takes MoveTime (MoverGlideType: GlideByTime = smooth start / stop). States as UE2 InitialState:
 * BumpOpenTimed / BumpButton (touched by a pawn, BumpType), StandOpenTimed (lifts: a pawn stands on it),
 * TriggerOpenTimed, TriggerToggle, TriggerControl, TriggerPound, ConstantLoop. Opened movers wait StayOpenTime and
 * return (timed states). Encroachment (MoverEncroachType): StopWhenEncroach, ReturnWhenEncroach, CrushWhenEncroach
 * (EncroachDamage), IgnoreWhenEncroach. Sounds: OpeningSound / OpenedSound / ClosingSound / ClosedSound and the
 * looping MoveAmbientSound. Events: Event when fully open (and OpeningEvent / OpenedEvent / ClosingEvent /
 * ClosedEvent), movers sharing a ReturnGroup return together. Physics=PHYS_Rotating movers (fans) spin by
 * RotationRate. Characters standing on a mover ride it (the engine's moving base).
 */
UCLASS()
class OPENUT2K4_API AArenaMover : public AActor, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaMover();

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	/** The mesh's UE2 collision model (never drawn), when it has one: players collide with it instead of Mesh. */
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> CollisionModel;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UAudioComponent> AmbientAudio;

	// ---- UE2 Mover ----
	UPROPERTY(VisibleAnywhere, Category = UE2) EArenaMoverState InitialState = EArenaMoverState::BumpOpenTimed;
	/** 0 Stop, 1 Return, 2 Crush, 3 Ignore. */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 MoverEncroachType = 1;
	/** 0 MoveByTime (linear), 1 GlideByTime (ease in / out). */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 MoverGlideType = 1;
	/** 0 PlayerBump, 1 PawnBump, 2 AnyBump. */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 BumpType = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 NumKeys = 2;
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 KeyNum = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) float MoveTime = 1.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float StayOpenTime = 4.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float OtherTime = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DelayTime = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 EncroachDamage = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bTriggerOnceOnly = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bDamageTriggered = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DamageThreshold = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bToggleDirection = true;
	/** cm, relative to BasePos. */
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<FVector> KeyPos;
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<FRotator> KeyRot;
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector BasePos = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = UE2) FRotator BaseRot = FRotator::ZeroRotator;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> OpeningSound;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> OpenedSound;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> ClosingSound;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> ClosedSound;
	UPROPERTY(VisibleAnywhere, Category = UE2) TObjectPtr<USoundBase> MoveAmbientSound;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Event;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName OpeningEvent;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName OpenedEvent;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName ClosingEvent;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName ClosedEvent;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName ReturnGroup;
	/** PHYS_Rotating: degrees per second. */
	UPROPERTY(VisibleAnywhere, Category = UE2) FRotator RotationRate = FRotator::ZeroRotator;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bRotating = false;
	FString UE2Name;

	// ---- state ----
	bool IsMoving() const { return bInterpolating; }
	/** True when at rest at key 0 (closed / lift at the bottom). */
	bool IsClosed() const { return !bInterpolating && KeyNum == 0; }
	bool IsOpen() const { return !bInterpolating && KeyNum == NumKeys - 1; }
	/** World location of key I's top centre (for bots: where to stand on a lift). */
	FVector GetKeyLocation(int32 I) const;
	/** Top of the mesh bounds at key I (world Z). */
	float GetKeyTopZ(int32 I) const;
	/**
	 * Watching a UT2004 server (Net/UE2NetWorld.h): the server's Mover.SimInterpolate (a move from PrevKey to NewKey,
	 * at Alpha 0..1, Rate per second) or its resting pose (RealPosition / RealRotation, cm / degrees). From then on
	 * the mover runs no state of its own (no bumps, timers, events or encroachment).
	 */
	void SetNetMove(int32 NewKey, int32 PrevKey, float InAlpha, float InRate);
	void SetNetRest(const FVector& Location, const FRotator& Rotation);
	/**
	 * Hosting for UT2004 clients (Net/UE2Server*): the move in progress as UE2's Mover replicates it (SimOldPos /
	 * SimOldRot = where it started, SimInterpolate = alpha, rate and keys). OutSerial counts the moves started.
	 */
	void GetNetMoveState(FVector& OutFromLoc, FRotator& OutFromRot, int32& OutKey, int32& OutPrevKey, float& OutAlpha, float& OutRate, int32& OutSerial) const
	{
		OutFromLoc = FromLoc;
		OutFromRot = FromRot;
		OutKey = bInterpolating ? TargetKey : KeyNum;
		OutPrevKey = KeyNum;
		OutAlpha = Alpha;
		OutRate = Rate;
		OutSerial = NetMoveSerial;
	}
	FVector GetNetLocation() const { return CurLoc; }
	FRotator GetNetRotation() const { return CurRot; }
	/** Open (all keys forward) / close (back to key 0). */
	void DoOpen();
	void DoClose();
	/** A pawn bumped / stands on it (called from the overlap scan). */
	virtual void Bump(APawn* Other);
	virtual void StandOn(APawn* Other);

	// IArenaUE2Actor
	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void PostInitUE2() override;
	virtual FName GetUE2Tag() const override { return Tag; }
	virtual FName GetUE2Event() const override { return Event; }
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	virtual void UnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void ResetUE2() override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	void InterpolateTo(int32 NewKey, float Time);
	void FinishedInterpolation();
	void PlayMoverSound(USoundBase* Sound);
	FTransform PoseAt(const FVector& Loc, const FRotator& Rot) const;
	void ApplyPose(const FVector& Loc, const FRotator& Rot);
	/** True when the pose at Loc / Rot would push into a pawn that does not ride the mover. */
	APawn* FindEncroached(const FVector& Loc, const FRotator& Rot) const;
	/** The component players collide with and stand on (Mesh, or the collision model). */
	UStaticMeshComponent* PawnCollision() const;
	bool IsBaseOf(const class ACharacter* C) const;
	void HandleEncroach(APawn* Other);
	void ScanPawns();

	FArenaUE2ActorDesc MeshDesc;
	FVector CurLoc = FVector::ZeroVector;
	FRotator CurRot = FRotator::ZeroRotator;
	FVector FromLoc = FVector::ZeroVector;
	FRotator FromRot = FRotator::ZeroRotator;
	int32 TargetKey = 0;
	float Alpha = 0.f;
	float Rate = 1.f;
	bool bInterpolating = false;
	bool bOpening = false;
	bool bTriggeredOnce = false;
	bool bTriggerOn = false;
	/** Seconds until the timed close / the delayed open. */
	float WaitTime = -1.f;
	bool bWaitToClose = false;
	bool bWaitToOpen = false;
	TWeakObjectPtr<APawn> LastInstigator;
	int32 InitialKeyNum = 0;
	TArray<TWeakObjectPtr<AArenaMover>> ReturnGroupMovers;
	TSet<TWeakObjectPtr<APawn>> TouchingPawns;
	/** Driven by a UT2004 server (SetNetMove / SetNetRest). */
	bool bNetDriven = false;
	/** Moves started (GetNetMoveState). */
	int32 NetMoveSerial = 0;
};

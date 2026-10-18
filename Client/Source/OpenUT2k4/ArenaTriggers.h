#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaUE2World.h"
#include "ArenaTriggers.generated.h"

class APawn;
class UCapsuleComponent;
class USoundBase;

/**
 * UE2 Triggers and event relays, native: Trigger, Dispatcher, Counter, TriggeredCondition, Teleporter and the basic
 * ScriptedTrigger actions. Touch is UE2's collision cylinder (CollisionRadius / CollisionHeight around Location),
 * tested against pawns every tick.
 */

/** Base for UE2 actors with a Tag / Event and a touch cylinder. */
UCLASS(Abstract)
class OPENUT2K4_API AArenaUE2EventActor : public AActor, public IArenaUE2Actor
{
	GENERATED_BODY()

public:
	AArenaUE2EventActor();

	UPROPERTY(VisibleAnywhere, Category = UE2) FName Tag;
	UPROPERTY(VisibleAnywhere, Category = UE2) FName Event;
	/** cm. */
	UPROPERTY(VisibleAnywhere, Category = UE2) float CollisionRadius = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float CollisionHeight = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bCollideActors = true;
	FString UE2Name;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual FName GetUE2Tag() const override { return Tag; }
	virtual FName GetUE2Event() const override { return Event; }

	/** UE2 Touch / UnTouch (pawns inside the cylinder). */
	virtual void Touch(APawn* Other) {}
	virtual void UnTouch(APawn* Other) {}
	/** True when Other's cylinder overlaps ours. */
	bool IsTouching(const APawn* Other) const;
	/** Fires Event through the event system. */
	void FireEvent(FName Name, APawn* EventInstigator, bool bTrigger = true);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	/** Touch scanning on/off (Tick runs only when needed). */
	bool bWantsTouch = false;
	TSet<TWeakObjectPtr<APawn>> Touching;
};

/** UE2 Trigger. */
UCLASS()
class OPENUT2K4_API AArenaTrigger : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	/** 0 PlayerProximity, 1 PawnProximity, 2 ClassProximity, 3 AnyProximity, 4 Shoot, 5 HumanPlayerProximity, 6 LivePlayerProximity. */
	UPROPERTY(VisibleAnywhere, Category = UE2) uint8 TriggerType = 0;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString Message;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bTriggerOnceOnly = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bInitiallyActive = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString ClassProximityType;
	UPROPERTY(VisibleAnywhere, Category = UE2) float RepeatTriggerTime = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float ReTriggerDelay = 0.f;
	UPROPERTY(VisibleAnywhere, Category = UE2) float DamageThreshold = 0.f;
	/** NormalTrigger, OtherTriggerToggles, OtherTriggerTurnsOn, OtherTriggerTurnsOff. */
	UPROPERTY(VisibleAnywhere, Category = UE2) FName InitialState;
	// UT2004 Trigger_ASTeam: only pawns of one Assault side touch it (0 the attackers, its default; 1 the defenders).
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 AssaultTeamOnly = INDEX_NONE;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void Touch(APawn* Other) override;
	virtual void UnTouch(APawn* Other) override;
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void ResetUE2() override { bActive = bInitiallyActive; TriggerTime = -100.f; }
	bool IsRelevant(const APawn* Other) const;

protected:
	virtual void Tick(float DeltaSeconds) override;
	UPROPERTY() TObjectPtr<UCapsuleComponent> ShootCollision;
	bool bActive = true;
	float TriggerTime = -100.f;
	float RepeatTimer = 0.f;
};

/** UE2 Dispatcher: on Trigger fires OutEvents[i] after OutDelays[i] (in sequence). */
UCLASS()
class OPENUT2K4_API AArenaDispatcher : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<FName> OutEvents;
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<float> OutDelays;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;

protected:
	virtual void Tick(float DeltaSeconds) override;
	int32 Step = -1;
	float StepTime = 0.f;
	TWeakObjectPtr<APawn> Instigator;
};

/** UE2 Counter: fires Event after NumToCount triggers. */
UCLASS()
class OPENUT2K4_API AArenaCounter : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = UE2) int32 NumToCount = 2;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bShowMessage = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString CountMessage;
	UPROPERTY(VisibleAnywhere, Category = UE2) FString CompleteMessage;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	virtual void ResetUE2() override { Remaining = NumToCount; }

protected:
	int32 Remaining = 2;
};

/** UE2 TriggeredCondition: a flag other actors (scripted triggers, objectives) test. */
UCLASS()
class OPENUT2K4_API AArenaTriggeredCondition : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bToggled = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bEnabled = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bTriggerControlled = false;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	virtual void UnTrigger(AActor* Other, APawn* EventInstigator) override;
	bool IsEnabled() const { return bEnabled; }
	virtual void ResetUE2() override { bEnabled = bInitialValue; }

protected:
	bool bInitialValue = false;
};

/**
 * UE2 Teleporter: touching it moves the pawn to a teleporter whose Tag equals our URL (random one if several),
 * keeping the view relative to the exit (bChangesYaw) and the velocity (or TargetVelocity with bChangesVelocity).
 */
UCLASS()
class OPENUT2K4_API AArenaTeleporter : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = UE2) FString URL;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bEnabled = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bChangesVelocity = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bChangesYaw = true;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bReversesX = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bReversesY = false;
	UPROPERTY(VisibleAnywhere, Category = UE2) bool bReversesZ = false;
	/** cm/s. */
	UPROPERTY(VisibleAnywhere, Category = UE2) FVector TargetVelocity = FVector::ZeroVector;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void Touch(APawn* Other) override;
	/** UE2: triggering a teleporter toggles bEnabled. */
	virtual void Trigger(AActor* Other, APawn* EventInstigator) override;
	/** Teleporters with Tag == URL. */
	TArray<AArenaTeleporter*> GetDestinations() const;
	/** UE2 Teleporter.Accept: place Incoming here. */
	bool Accept(APawn* Incoming, AArenaTeleporter* Source);
	/** Floor point at the teleporter (for the nav graph). */
	FVector GetFloorLocation() const;
};

/** One UE2 ScriptedAction (the supported subset). */
USTRUCT()
struct FArenaScriptedAction
{
	GENERATED_BODY()
	/** UE2 class ("ACTION_TriggerEvent", "ACTION_WaitForTimer" ...). */
	UPROPERTY() FString Class;
	UPROPERTY() FName EventName;
	UPROPERTY() float Duration = 0.f;
	UPROPERTY() int32 JumpIndex = 0;
	UPROPERTY() TObjectPtr<USoundBase> Sound;
	UPROPERTY() float Volume = 1.f;
	UPROPERTY() FString Text;
};

/**
 * UE2 ScriptedTrigger (basics): runs its Actions in order. Supported: ACTION_TriggerEvent, ACTION_UnTriggerEvent,
 * ACTION_WaitForEvent, ACTION_WaitForTimer, ACTION_PlaySound / PlayLocalSound / PlayAmbientSound,
 * ACTION_GotoAction, ACTION_DisplayMessage, ACTION_DestroyActor (by tag); others are skipped (logged once).
 */
UCLASS()
class OPENUT2K4_API AArenaScriptedTrigger : public AArenaUE2EventActor
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = UE2) TArray<FArenaScriptedAction> Actions;

	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) override;
	virtual void ResetUE2() override;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void OnAnyEvent(FName Name, AActor* Other, APawn* EventInstigator, bool bTrigger);
	void Run();
	int32 Current = 0;
	float WaitUntil = -1.f;
	FName WaitEvent;
	bool bDone = false;
	int32 Steps = 0;
	FDelegateHandle EventHandle;
};

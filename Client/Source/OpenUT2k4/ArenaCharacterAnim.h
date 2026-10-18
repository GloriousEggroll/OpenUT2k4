#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "ArenaCharacterAnim.generated.h"

class UAnimSequence;
class AArenaCharacter;

/** Sequences the character animation uses. Names are appended to the model's animation prefix. */
enum class EArenaAnim : uint8
{
	IdleRifle, IdleHeavy,
	RunF, RunB, RunL, RunR,
	CrouchIdle, CrouchF, CrouchB, CrouchL, CrouchR,
	JumpTakeoff, JumpMid, JumpLand,
	DoubleJumpF, DoubleJumpB, DoubleJumpL, DoubleJumpR,
	DodgeF, DodgeB, DodgeL, DodgeR,
	SwimTread, SwimF, SwimB, SwimL, SwimR,
	DeathF, DeathB, DeathL, DeathR,
	HitF, HitB, HitL, HitR, HitHead,
	FireRifle, FireHeavy, FireCrouch,
	WeaponSwitch,
	Num
};

/**
 * Gameplay events the animation reacts to. AArenaCharacter bumps the counters; the animation proxy
 * compares them with the values it saw last frame. Directions are world space.
 */
struct FArenaAnimEvents
{
	int32 FireCount = 0;
	int32 SwitchCount = 0;
	int32 HitCount = 0;
	/** Unit vector from the character towards where the last hit came from. */
	FVector HitFromDir = FVector::ForwardVector;
	bool bHitHead = false;
	int32 DodgeCount = 0;
	FVector DodgeDir = FVector::ForwardVector;
	int32 DeathCount = 0;
	/** Direction the killing blow pushed the body. */
	FVector DeathPushDir = -FVector::ForwardVector;
	bool bHeavyWeapon = false;
};

/**
 * Native character animation: no AnimBlueprint and no anim graph. The proxy gathers the pawn's state on the game
 * thread (PreUpdate), runs a small state machine with cross-fades on the worker thread (Update) and samples and
 * blends the sequences itself (Evaluate):
 *  - base pose: idle / 4-way run (8 directions by blending), crouch, jump & double jump, dodge, landing, swim, death
 *  - upper body (spine and up): fire, weapon switch and hit reactions
 *  - aim pitch spread over the spine bones
 */
UCLASS(Transient, NotBlueprintable)
class OPENUT2K4_API UArenaCharacterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/**
	 * Loads "<AnimPrefix><Sequence>" for every EArenaAnim via ArenaArt (Replacements first), keeping only
	 * sequences that use the mesh's skeleton. Missing optional sequences fall back to related ones.
	 * Returns false if the basic locomotion set is not available.
	 */
	bool LoadAnimSet(const FString& AnimPrefix);

	bool HasAnimSet() const { return bHasAnimSet; }
	const UAnimSequence* GetSequence(EArenaAnim Anim) const;

	UPROPERTY(Transient) TArray<TObjectPtr<UAnimSequence>> Sequences;

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

	bool bHasAnimSet = false;
};

/** Worker-thread side of UArenaCharacterAnimInstance. */
struct FArenaCharacterAnimProxy : public FAnimInstanceProxy
{
	FArenaCharacterAnimProxy() = default;
	explicit FArenaCharacterAnimProxy(UAnimInstance* InInstance) : FAnimInstanceProxy(InInstance) {}

protected:
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual void Update(float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	enum class EState : uint8 { Ground, Crouch, Air, Dodge, Land, Swim, Death, Num };

	struct FInputs
	{
		FVector LocalVelocity = FVector::ZeroVector;
		float Speed2D = 0.f;
		float VelZ = 0.f;
		float AimPitch = 0.f;
		/** UE2 Pawn.HeadScale (xPawn.SetHeadScale: SetBoneScale on the head bone). */
		float HeadScale = 1.f;
		bool bOnGround = true;
		bool bFalling = false;
		bool bSwimming = false;
		bool bCrouched = false;
		bool bDead = false;
		int32 JumpCount = 0;
		FVector HitFromLocal = FVector::ForwardVector;
		FVector DodgeLocal = FVector::ForwardVector;
		FVector DeathPushLocal = -FVector::ForwardVector;
		FArenaAnimEvents Events;
	};

	struct FLayer
	{
		const UAnimSequence* Seq = nullptr;
		float Time = 0.f;
		float Weight = 0.f;
		bool bLoop = true;
	};

	struct FOverlay
	{
		int32 Anim = -1;
		float Time = 0.f;
		float Weight = 0.f;
		float Rate = 1.f;
		/** Peak weight; single-frame reactions use less than 1 and decay. */
		float Peak = 1.f;
		float DecayTime = 0.f;
		bool IsActive() const { return Anim >= 0; }
	};

	const UAnimSequence* Seq(EArenaAnim A) const { return Sequences[static_cast<int32>(A)]; }
	float Len(EArenaAnim A) const;
	static EArenaAnim DirAnim(EArenaAnim First, const FVector& LocalDir);
	void StartOverlay(FOverlay& O, EArenaAnim Anim, float Rate, float Peak, float DecayTime);
	void TickOverlay(FOverlay& O, float DeltaSeconds);
	void AddDirectionalLayers(TArray<FLayer, TInlineAllocator<12>>& Out, float StateWeight, EArenaAnim Idle, float IdleTime,
		EArenaAnim FirstDir, float MoveAlpha, float Phase) const;
	void EvaluateLayer(const FLayer& Layer, FPoseContext& Into) const;

	const UAnimSequence* Sequences[static_cast<int32>(EArenaAnim::Num)] = {};
	bool bHasAnims = false;
	FInputs In;

	// state machine (worker thread)
	bool bInitialized = false;
	float StateWeight[static_cast<int32>(EState::Num)] = {};
	EState Current = EState::Ground;
	bool bWasOnGround = true;
	float AirTime = 0.f;
	int32 AirJumpCount = 0;

	float IdleTime = 0.f;
	float MovePhase = 0.f;
	float MoveAlpha = 0.f;
	float CrouchPhase = 0.f;
	float CrouchMoveAlpha = 0.f;
	float CrouchIdleTime = 0.f;
	float DirWeights[4] = { 1.f, 0.f, 0.f, 0.f }; // F B L R
	float SwimPhase = 0.f;
	float SwimMoveAlpha = 0.f;
	float SwimIdleTime = 0.f;

	EArenaAnim AirOneShot = EArenaAnim::Num;
	float AirOneShotTime = 0.f;
	float AirMidTime = 0.f;
	EArenaAnim DodgeAnim = EArenaAnim::DodgeF;
	float DodgeTime = 100.f;
	float LandTime = 100.f;
	EArenaAnim DeathAnim = EArenaAnim::DeathB;
	float DeathTime = 0.f;

	FOverlay FireOverlay;
	FOverlay SwitchOverlay;
	FOverlay HitOverlay;
	float AimPitch = 0.f;

	int32 SeenFire = 0;
	int32 SeenSwitch = 0;
	int32 SeenHit = 0;
	int32 SeenDodge = 0;
	int32 SeenDeath = 0;
};

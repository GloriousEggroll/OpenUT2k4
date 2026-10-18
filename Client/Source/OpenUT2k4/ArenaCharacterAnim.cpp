#include "ArenaCharacterAnim.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaCharacter.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace ArenaCharAnim
{
	/** Sequence names per EArenaAnim, appended to the model's prefix. */
	static const TCHAR* const SequenceNames[] = {
		TEXT("Idle_Rifle"), TEXT("Idle_Biggun"),
		TEXT("RunF"), TEXT("RunB"), TEXT("RunL"), TEXT("RunR"),
		TEXT("Crouch"), TEXT("CrouchF"), TEXT("CrouchB"), TEXT("CrouchL"), TEXT("CrouchR"),
		TEXT("Jump_Takeoff"), TEXT("Jump_Mid"), TEXT("Jump_Land"),
		TEXT("DoubleJumpF"), TEXT("DoubleJumpB"), TEXT("DoubleJumpL"), TEXT("DoubleJumpR"),
		TEXT("DodgeF"), TEXT("DodgeB"), TEXT("DodgeL"), TEXT("DodgeR"),
		TEXT("Swim_Tread"), TEXT("SwimF"), TEXT("SwimB"), TEXT("SwimL"), TEXT("SwimR"),
		TEXT("DeathF"), TEXT("DeathB"), TEXT("DeathL"), TEXT("DeathR"),
		TEXT("HitF"), TEXT("HitB"), TEXT("HitL"), TEXT("HitR"), TEXT("Hit_Head"),
		TEXT("Rifle_Burst"), TEXT("Biggun_Burst"), TEXT("Crouch_Burst"),
		TEXT("Weapon_Switch"),
	};
	static_assert(UE_ARRAY_COUNT(SequenceNames) == static_cast<int32>(EArenaAnim::Num), "SequenceNames out of sync with EArenaAnim");

	/** Fallback when a sequence is missing (same index = no fallback). */
	EArenaAnim Fallback(EArenaAnim A)
	{
		switch (A)
		{
		case EArenaAnim::IdleHeavy:   return EArenaAnim::IdleRifle;
		case EArenaAnim::CrouchIdle:  return EArenaAnim::IdleRifle;
		case EArenaAnim::CrouchF:     return EArenaAnim::RunF;
		case EArenaAnim::CrouchB:     return EArenaAnim::RunB;
		case EArenaAnim::CrouchL:     return EArenaAnim::RunL;
		case EArenaAnim::CrouchR:     return EArenaAnim::RunR;
		case EArenaAnim::JumpTakeoff: return EArenaAnim::JumpMid;
		case EArenaAnim::JumpMid:     return EArenaAnim::IdleRifle;
		case EArenaAnim::JumpLand:    return EArenaAnim::JumpMid;
		case EArenaAnim::DoubleJumpF: case EArenaAnim::DoubleJumpB: case EArenaAnim::DoubleJumpL: case EArenaAnim::DoubleJumpR:
			return EArenaAnim::JumpTakeoff;
		case EArenaAnim::DodgeF: return EArenaAnim::DoubleJumpF;
		case EArenaAnim::DodgeB: return EArenaAnim::DoubleJumpB;
		case EArenaAnim::DodgeL: return EArenaAnim::DoubleJumpL;
		case EArenaAnim::DodgeR: return EArenaAnim::DoubleJumpR;
		case EArenaAnim::SwimTread: return EArenaAnim::JumpMid;
		case EArenaAnim::SwimF: return EArenaAnim::RunF;
		case EArenaAnim::SwimB: return EArenaAnim::RunB;
		case EArenaAnim::SwimL: return EArenaAnim::RunL;
		case EArenaAnim::SwimR: return EArenaAnim::RunR;
		case EArenaAnim::DeathF: case EArenaAnim::DeathL: case EArenaAnim::DeathR: return EArenaAnim::DeathB;
		case EArenaAnim::HitHead: return EArenaAnim::HitF;
		case EArenaAnim::FireHeavy:  return EArenaAnim::FireRifle;
		case EArenaAnim::FireCrouch: return EArenaAnim::FireRifle;
		default: return A;
		}
	}

	static const FName UpperBodyRoot(TEXT("bip01-spine"));
	/** The head bone of the converted UT2004 skeletons ("Bip01 Head"), and UE2's own name for it. */
	static const FName HeadBone(TEXT("bip01-head"));
	static const FName HeadBoneUE2(TEXT("head"));
	static const FName SpineBones[] = { FName(TEXT("bip01-spine")), FName(TEXT("bip01-spine1")), FName(TEXT("bip01-spine2")) };
	static const float SpineShare[] = { 0.25f, 0.35f, 0.4f };

	/** Reference speeds of the sequences (cm/s at play rate 1). */
	static constexpr float RunSpeed = 880.f;
	static constexpr float CrouchSpeed = 352.f;
	static constexpr float SwimSpeed = 500.f;

	void BlendTransform(FTransform& A, const FTransform& B, float W)
	{
		A.SetTranslation(FMath::Lerp(A.GetTranslation(), B.GetTranslation(), W));
		FQuat Q = FQuat::FastLerp(A.GetRotation(), B.GetRotation(), W);
		Q.Normalize();
		A.SetRotation(Q);
		A.SetScale3D(FMath::Lerp(A.GetScale3D(), B.GetScale3D(), W));
	}

	float ExpApproach(float Current, float Target, float Speed, float DeltaSeconds)
	{
		return FMath::Lerp(Target, Current, FMath::Exp(-Speed * DeltaSeconds));
	}
}

// =================================================================================================
// Anim instance (game thread)
// =================================================================================================

FAnimInstanceProxy* UArenaCharacterAnimInstance::CreateAnimInstanceProxy()
{
	return new FArenaCharacterAnimProxy(this);
}

const UAnimSequence* UArenaCharacterAnimInstance::GetSequence(EArenaAnim Anim) const
{
	const int32 Index = static_cast<int32>(Anim);
	return Sequences.IsValidIndex(Index) ? Sequences[Index].Get() : nullptr;
}

bool UArenaCharacterAnimInstance::LoadAnimSet(const FString& AnimPrefix)
{
	using namespace ArenaCharAnim;
	const USkeletalMeshComponent* Comp = GetSkelMeshComponent();
	const USkeletalMesh* Mesh = Comp ? Comp->GetSkeletalMeshAsset() : nullptr;
	const USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;

	Sequences.Reset();
	Sequences.SetNum(static_cast<int32>(EArenaAnim::Num));
	int32 Found = 0;
	for (int32 i = 0; i < Sequences.Num(); ++i)
	{
		UAnimSequence* S = ArenaArt::Load<UAnimSequence>(AnimPrefix + SequenceNames[i]);
		if (S && Skeleton && S->GetSkeleton() != Skeleton)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaCharacterAnim: %s uses skeleton %s, mesh uses %s - skipped"),
				*S->GetPathName(), S->GetSkeleton() ? *S->GetSkeleton()->GetName() : TEXT("none"), *Skeleton->GetName());
			S = nullptr;
		}
		Sequences[i] = S;
		Found += S ? 1 : 0;
	}
	// Resolve fallbacks (chains are short and acyclic)
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		for (int32 i = 0; i < Sequences.Num(); ++i)
		{
			if (!Sequences[i])
			{
				const int32 F = static_cast<int32>(Fallback(static_cast<EArenaAnim>(i)));
				if (F != i)
				{
					Sequences[i] = Sequences[F];
				}
			}
		}
	}
	bHasAnimSet = GetSequence(EArenaAnim::IdleRifle) && GetSequence(EArenaAnim::RunF) && GetSequence(EArenaAnim::RunB)
		&& GetSequence(EArenaAnim::RunL) && GetSequence(EArenaAnim::RunR);
	UE_LOG(LogOpenUT2k4, Log, TEXT("ArenaCharacterAnim: %s%s loaded %d/%d sequences%s"), *AnimPrefix, TEXT("*"), Found, Sequences.Num(),
		bHasAnimSet ? TEXT("") : TEXT(" (no locomotion set)"));
	return bHasAnimSet;
}

// =================================================================================================
// Proxy: game thread input gathering
// =================================================================================================

void FArenaCharacterAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	const UArenaCharacterAnimInstance* Inst = Cast<UArenaCharacterAnimInstance>(InAnimInstance);
	bHasAnims = Inst && Inst->HasAnimSet();
	for (int32 i = 0; i < static_cast<int32>(EArenaAnim::Num); ++i)
	{
		Sequences[i] = Inst ? Inst->GetSequence(static_cast<EArenaAnim>(i)) : nullptr;
	}

	const AArenaCharacter* Char = Cast<AArenaCharacter>(InAnimInstance->GetOwningActor());
	if (!Char)
	{
		return;
	}
	const FTransform ActorT = Char->GetActorTransform();
	const FVector Vel = Char->GetVelocity();
	In.LocalVelocity = ActorT.InverseTransformVectorNoScale(Vel);
	In.Speed2D = Vel.Size2D();
	In.VelZ = Vel.Z;
	In.bDead = Char->IsDead();
	In.bCrouched = Char->bIsCrouched;
	In.JumpCount = Char->JumpCurrentCount;
	if (const UCharacterMovementComponent* Move = Char->GetCharacterMovement())
	{
		In.bOnGround = Move->IsMovingOnGround();
		In.bFalling = Move->IsFalling();
		In.bSwimming = Move->IsSwimming();
		// Riding a vehicle disables movement: treat as standing.
		if (Move->MovementMode == MOVE_None)
		{
			In.bOnGround = true;
			In.bFalling = false;
		}
	}
	if (!In.bDead)
	{
		In.AimPitch = FRotator::NormalizeAxis(Char->GetBaseAimRotation().Pitch);
		In.HeadScale = Char->HeadScale;
	}
	In.Events = Char->GetAnimEvents();
	In.HitFromLocal = ActorT.InverseTransformVectorNoScale(In.Events.HitFromDir);
	In.DodgeLocal = ActorT.InverseTransformVectorNoScale(In.Events.DodgeDir);
	In.DeathPushLocal = ActorT.InverseTransformVectorNoScale(In.Events.DeathPushDir);
}

// =================================================================================================
// Proxy: state machine (worker thread)
// =================================================================================================

float FArenaCharacterAnimProxy::Len(EArenaAnim A) const
{
	const UAnimSequence* S = Seq(A);
	return S ? FMath::Max(S->GetPlayLength(), 1.f / 30.f) : 0.f;
}

EArenaAnim FArenaCharacterAnimProxy::DirAnim(EArenaAnim First, const FVector& LocalDir)
{
	// First is the F variant; B, L, R follow it in the enum.
	const int32 Base = static_cast<int32>(First);
	if (FMath::Abs(LocalDir.X) >= FMath::Abs(LocalDir.Y))
	{
		return static_cast<EArenaAnim>(Base + (LocalDir.X >= 0.f ? 0 : 1));
	}
	return static_cast<EArenaAnim>(Base + (LocalDir.Y < 0.f ? 2 : 3));
}

void FArenaCharacterAnimProxy::StartOverlay(FOverlay& O, EArenaAnim Anim, float Rate, float Peak, float DecayTime)
{
	if (!Seq(Anim))
	{
		return;
	}
	O.Anim = static_cast<int32>(Anim);
	O.Time = 0.f;
	O.Rate = Rate;
	O.Peak = Peak;
	O.DecayTime = DecayTime;
	if (DecayTime > 0.f)
	{
		O.Weight = Peak;
	}
}

void FArenaCharacterAnimProxy::TickOverlay(FOverlay& O, float DeltaSeconds)
{
	if (!O.IsActive())
	{
		return;
	}
	O.Time += DeltaSeconds * O.Rate;
	if (O.DecayTime > 0.f)
	{
		// Reaction pose: jump to the peak, decay linearly
		O.Weight = FMath::Max(0.f, O.Peak * (1.f - O.Time / (O.DecayTime * O.Rate)));
	}
	else
	{
		const float L = Len(static_cast<EArenaAnim>(O.Anim));
		if (O.Time < L)
		{
			O.Weight = FMath::Min(O.Peak, O.Weight + DeltaSeconds / 0.06f);
		}
		else
		{
			O.Weight = FMath::Max(0.f, O.Weight - DeltaSeconds / 0.15f);
		}
	}
	if (O.Weight <= 0.f && O.Time > 0.f)
	{
		O.Anim = -1;
		O.Weight = 0.f;
	}
}

void FArenaCharacterAnimProxy::Update(float DeltaSeconds)
{
	using namespace ArenaCharAnim;
	if (!bHasAnims)
	{
		return;
	}
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.25f);
	const FArenaAnimEvents& Ev = In.Events;

	if (!bInitialized)
	{
		bInitialized = true;
		SeenFire = Ev.FireCount;
		SeenSwitch = Ev.SwitchCount;
		SeenHit = Ev.HitCount;
		SeenDodge = Ev.DodgeCount;
		SeenDeath = Ev.DeathCount;
		for (float& W : StateWeight)
		{
			W = 0.f;
		}
		StateWeight[static_cast<int32>(In.bDead ? EState::Death : EState::Ground)] = 1.f;
		IdleTime = FMath::FRand() * 2.f;
	}

	// ---- events ----
	if (Ev.DeathCount != SeenDeath)
	{
		SeenDeath = Ev.DeathCount;
		DeathAnim = DirAnim(EArenaAnim::DeathF, In.DeathPushLocal);
		DeathTime = 0.f;
	}
	if (Ev.DodgeCount != SeenDodge)
	{
		SeenDodge = Ev.DodgeCount;
		DodgeAnim = DirAnim(EArenaAnim::DodgeF, In.DodgeLocal);
		DodgeTime = 0.f;
	}
	if (Ev.FireCount != SeenFire)
	{
		SeenFire = Ev.FireCount;
		const EArenaAnim Fire = In.bCrouched ? EArenaAnim::FireCrouch : (Ev.bHeavyWeapon ? EArenaAnim::FireHeavy : EArenaAnim::FireRifle);
		StartOverlay(FireOverlay, Fire, 1.f, 1.f, 0.f);
	}
	if (Ev.SwitchCount != SeenSwitch)
	{
		SeenSwitch = Ev.SwitchCount;
		StartOverlay(SwitchOverlay, EArenaAnim::WeaponSwitch, 1.6f, 0.9f, 0.f);
	}
	if (Ev.HitCount != SeenHit)
	{
		SeenHit = Ev.HitCount;
		const EArenaAnim Hit = Ev.bHitHead ? EArenaAnim::HitHead : DirAnim(EArenaAnim::HitF, In.HitFromLocal);
		StartOverlay(HitOverlay, Hit, 1.f, 0.55f, 0.3f);
	}

	// ---- ground / air transitions ----
	const bool bOnGround = In.bOnGround && !In.bFalling;
	if (In.bFalling && !In.bSwimming)
	{
		if (bWasOnGround)
		{
			// Left the ground: a jump plays the take-off, walking off a ledge goes straight to the fall pose
			AirTime = 0.f;
			AirMidTime = 0.f;
			AirJumpCount = In.JumpCount;
			AirOneShot = (In.VelZ > 100.f && DodgeTime > 0.1f) ? EArenaAnim::JumpTakeoff : EArenaAnim::Num;
			AirOneShotTime = 0.f;
		}
		AirTime += Dt;
		if (In.JumpCount > AirJumpCount && AirTime > 0.05f)
		{
			// Double jump: flip in the direction of travel
			AirOneShot = DirAnim(EArenaAnim::DoubleJumpF, In.LocalVelocity.SizeSquared2D() > FMath::Square(150.f) ? In.LocalVelocity : FVector::ForwardVector);
			AirOneShotTime = 0.f;
		}
		AirJumpCount = In.JumpCount;
	}
	else if (!bWasOnGround && bOnGround)
	{
		LandTime = 0.f;
		DodgeTime = 100.f;
	}
	bWasOnGround = bOnGround || In.bSwimming;

	// ---- target state ----
	EState Target;
	if (In.bDead)
	{
		Target = EState::Death;
	}
	else if (In.bSwimming)
	{
		Target = EState::Swim;
	}
	else if (In.bFalling && DodgeTime < Len(DodgeAnim) + 0.25f)
	{
		Target = EState::Dodge;
	}
	else if (In.bFalling)
	{
		Target = EState::Air;
	}
	else if (LandTime < 0.1f && !In.bCrouched && In.Speed2D < 400.f)
	{
		Target = EState::Land;
	}
	else
	{
		Target = In.bCrouched ? EState::Crouch : EState::Ground;
	}
	Current = Target;

	const float BlendTime = Target == EState::Death ? 0.1f : (Target == EState::Land || Current == EState::Dodge ? 0.08f : 0.15f);
	for (int32 s = 0; s < static_cast<int32>(EState::Num); ++s)
	{
		const float Goal = s == static_cast<int32>(Target) ? 1.f : 0.f;
		StateWeight[s] = FMath::FInterpConstantTo(StateWeight[s], Goal, Dt, 1.f / BlendTime);
	}

	// ---- per-state timers ----
	// Direction weights from velocity relative to facing (4-way blend gives 8 directions)
	{
		const FVector2D V(In.LocalVelocity.X, In.LocalVelocity.Y);
		float Target4[4] = { 1.f, 0.f, 0.f, 0.f };
		if (V.SizeSquared() > 25.f * 25.f)
		{
			const FVector2D D = V.GetSafeNormal();
			Target4[0] = FMath::Max(0.f, D.X);
			Target4[1] = FMath::Max(0.f, -D.X);
			Target4[2] = FMath::Max(0.f, -D.Y);
			Target4[3] = FMath::Max(0.f, D.Y);
			const float Sum = Target4[0] + Target4[1] + Target4[2] + Target4[3];
			for (float& W : Target4)
			{
				W /= Sum;
			}
		}
		else
		{
			for (int32 i = 0; i < 4; ++i)
			{
				Target4[i] = DirWeights[i];
			}
		}
		for (int32 i = 0; i < 4; ++i)
		{
			DirWeights[i] = ExpApproach(DirWeights[i], Target4[i], 14.f, Dt);
		}
	}

	IdleTime += Dt;
	CrouchIdleTime += Dt;
	SwimIdleTime += Dt;

	MoveAlpha = ExpApproach(MoveAlpha, FMath::Clamp(In.Speed2D / 220.f, 0.f, 1.f), 12.f, Dt);
	if (Len(EArenaAnim::RunF) > 0.f)
	{
		const float Rate = FMath::Clamp(In.Speed2D / RunSpeed, 0.5f, 1.7f);
		MovePhase = FMath::Fmod(MovePhase + Dt * Rate / Len(EArenaAnim::RunF), 1.f);
	}
	CrouchMoveAlpha = ExpApproach(CrouchMoveAlpha, FMath::Clamp(In.Speed2D / 100.f, 0.f, 1.f), 12.f, Dt);
	if (Len(EArenaAnim::CrouchF) > 0.f)
	{
		const float Rate = FMath::Clamp(In.Speed2D / CrouchSpeed, 0.5f, 1.7f);
		CrouchPhase = FMath::Fmod(CrouchPhase + Dt * Rate / Len(EArenaAnim::CrouchF), 1.f);
	}
	const float SwimSpeed3D = FVector(In.LocalVelocity).Size();
	SwimMoveAlpha = ExpApproach(SwimMoveAlpha, FMath::Clamp(SwimSpeed3D / 200.f, 0.f, 1.f), 8.f, Dt);
	if (Len(EArenaAnim::SwimF) > 0.f)
	{
		const float Rate = FMath::Clamp(SwimSpeed3D / SwimSpeed, 0.5f, 1.5f);
		SwimPhase = FMath::Fmod(SwimPhase + Dt * Rate / Len(EArenaAnim::SwimF), 1.f);
	}

	AirOneShotTime += Dt;
	AirMidTime += Dt;
	DodgeTime += Dt;
	LandTime += Dt;
	if (In.bDead)
	{
		DeathTime += Dt;
	}

	TickOverlay(FireOverlay, Dt);
	TickOverlay(SwitchOverlay, Dt);
	TickOverlay(HitOverlay, Dt);

	AimPitch = ExpApproach(AimPitch, In.bDead ? 0.f : FMath::Clamp(In.AimPitch, -80.f, 80.f), 15.f, Dt);
}

// =================================================================================================
// Proxy: evaluation (worker thread)
// =================================================================================================

void FArenaCharacterAnimProxy::AddDirectionalLayers(TArray<FLayer, TInlineAllocator<12>>& Out, float InStateWeight, EArenaAnim Idle,
	float InIdleTime, EArenaAnim FirstDir, float InMoveAlpha, float Phase) const
{
	if (InStateWeight <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	if (const UAnimSequence* IdleSeq = Seq(Idle))
	{
		Out.Add({ IdleSeq, FMath::Fmod(InIdleTime, Len(Idle)), InStateWeight * (1.f - InMoveAlpha), true });
	}
	for (int32 d = 0; d < 4; ++d)
	{
		const EArenaAnim A = static_cast<EArenaAnim>(static_cast<int32>(FirstDir) + d);
		if (const UAnimSequence* S = Seq(A))
		{
			Out.Add({ S, Phase * Len(A), InStateWeight * InMoveAlpha * DirWeights[d], true });
		}
	}
}

void FArenaCharacterAnimProxy::EvaluateLayer(const FLayer& Layer, FPoseContext& Into) const
{
	FAnimationPoseData PoseData(Into);
	const float L = Layer.Seq->GetPlayLength();
	const float T = Layer.bLoop ? Layer.Time : FMath::Clamp(Layer.Time, 0.f, L);
	Layer.Seq->GetAnimationPose(PoseData, FAnimExtractContext(double(T), false, FDeltaTimeRecord(), Layer.bLoop));
}

bool FArenaCharacterAnimProxy::Evaluate(FPoseContext& Output)
{
	using namespace ArenaCharAnim;
	if (!bHasAnims || !bInitialized)
	{
		Output.ResetToRefPose();
		return true;
	}

	auto W = [this](EState S) { return StateWeight[static_cast<int32>(S)]; };

	// ---- base layers ----
	TArray<FLayer, TInlineAllocator<12>> Layers;
	const EArenaAnim Idle = In.Events.bHeavyWeapon ? EArenaAnim::IdleHeavy : EArenaAnim::IdleRifle;
	AddDirectionalLayers(Layers, W(EState::Ground), Idle, IdleTime, EArenaAnim::RunF, MoveAlpha, MovePhase);
	AddDirectionalLayers(Layers, W(EState::Crouch), EArenaAnim::CrouchIdle, CrouchIdleTime, EArenaAnim::CrouchF, CrouchMoveAlpha, CrouchPhase);
	AddDirectionalLayers(Layers, W(EState::Swim), EArenaAnim::SwimTread, SwimIdleTime, EArenaAnim::SwimF, SwimMoveAlpha, SwimPhase);
	if (W(EState::Air) > KINDA_SMALL_NUMBER)
	{
		float OneShotAlpha = 0.f;
		if (AirOneShot != EArenaAnim::Num && Seq(AirOneShot))
		{
			const float L = Len(AirOneShot);
			OneShotAlpha = AirOneShotTime < L ? 1.f : FMath::Clamp(1.f - (AirOneShotTime - L) / 0.15f, 0.f, 1.f);
			if (OneShotAlpha > 0.f)
			{
				Layers.Add({ Seq(AirOneShot), AirOneShotTime, W(EState::Air) * OneShotAlpha, false });
			}
		}
		if (const UAnimSequence* Mid = Seq(EArenaAnim::JumpMid))
		{
			Layers.Add({ Mid, AirMidTime, W(EState::Air) * (1.f - OneShotAlpha), false });
		}
	}
	if (W(EState::Dodge) > KINDA_SMALL_NUMBER && Seq(DodgeAnim))
	{
		Layers.Add({ Seq(DodgeAnim), DodgeTime, W(EState::Dodge), false });
	}
	if (W(EState::Land) > KINDA_SMALL_NUMBER && Seq(EArenaAnim::JumpLand))
	{
		Layers.Add({ Seq(EArenaAnim::JumpLand), 0.f, W(EState::Land), false });
	}
	if (W(EState::Death) > KINDA_SMALL_NUMBER && Seq(DeathAnim))
	{
		Layers.Add({ Seq(DeathAnim), DeathTime, W(EState::Death), false });
	}

	Layers.RemoveAll([](const FLayer& L) { return !L.Seq || L.Weight <= 0.001f; });
	float Total = 0.f;
	for (const FLayer& L : Layers)
	{
		Total += L.Weight;
	}
	if (Layers.Num() == 0 || Total <= KINDA_SMALL_NUMBER)
	{
		Output.ResetToRefPose();
		return true;
	}

	if (Layers.Num() == 1)
	{
		EvaluateLayer(Layers[0], Output);
	}
	else
	{
		TArray<FCompactPose, TInlineAllocator<12>> Poses;
		TArray<FBlendedCurve, TInlineAllocator<12>> Curves;
		TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<12>> Attributes;
		TArray<float, TInlineAllocator<12>> Weights;
		for (const FLayer& L : Layers)
		{
			FPoseContext Ctx(Output);
			EvaluateLayer(L, Ctx);
			Poses.AddDefaulted_GetRef().MoveBonesFrom(Ctx.Pose);
			Curves.AddDefaulted_GetRef().MoveFrom(Ctx.Curve);
			Attributes.AddDefaulted_GetRef().MoveFrom(Ctx.CustomAttributes);
			Weights.Add(L.Weight / Total);
		}
		FAnimationPoseData OutData(Output);
		FAnimationRuntime::BlendPosesTogether(Poses, Curves, Attributes, Weights, OutData);
		Output.Pose.NormalizeRotations();
	}

	// ---- bone lookups ----
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FReferenceSkeleton& RefSkel = Bones.GetReferenceSkeleton();
	const int32 NumBones = Output.Pose.GetNumBones();
	TArray<bool, TInlineAllocator<64>> UpperMask;
	UpperMask.SetNumZeroed(NumBones);
	FCompactPoseBoneIndex Spine[UE_ARRAY_COUNT(SpineBones)] = { FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE) };
	FCompactPoseBoneIndex Head(INDEX_NONE);
	for (FCompactPoseBoneIndex B : Output.Pose.ForEachBoneIndex())
	{
		const FName Name = RefSkel.GetBoneName(Bones.MakeMeshPoseIndex(B).GetInt());
		if (Name == HeadBone || (Head.GetInt() < 0 && Name == HeadBoneUE2))
		{
			Head = B;
		}
		const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(B);
		UpperMask[B.GetInt()] = Name == UpperBodyRoot || (Parent.GetInt() >= 0 && UpperMask[Parent.GetInt()]);
		for (int32 s = 0; s < UE_ARRAY_COUNT(SpineBones); ++s)
		{
			if (Name == SpineBones[s])
			{
				Spine[s] = B;
			}
		}
	}

	// ---- upper body overlays (not while dead / swimming) ----
	const float UpperScale = FMath::Clamp(1.f - W(EState::Death) - W(EState::Swim), 0.f, 1.f);
	const FOverlay* Overlays[] = { &SwitchOverlay, &FireOverlay, &HitOverlay };
	for (const FOverlay* O : Overlays)
	{
		const float Alpha = O->Weight * UpperScale;
		if (!O->IsActive() || Alpha <= 0.001f)
		{
			continue;
		}
		const EArenaAnim A = static_cast<EArenaAnim>(O->Anim);
		FPoseContext Ctx(Output);
		EvaluateLayer({ Seq(A), O->Time, 1.f, false }, Ctx);
		for (FCompactPoseBoneIndex B : Output.Pose.ForEachBoneIndex())
		{
			if (UpperMask[B.GetInt()])
			{
				BlendTransform(Output.Pose[B], Ctx.Pose[B], Alpha);
			}
		}
	}

	// ---- aim pitch over the spine (component space, about the character's right axis) ----
	const float Pitch = AimPitch * UpperScale;
	if (FMath::Abs(Pitch) > 0.5f && Spine[0].GetInt() >= 0)
	{
		FCSPose<FCompactPose> CS;
		CS.InitPose(Output.Pose);
		for (int32 s = 0; s < UE_ARRAY_COUNT(SpineBones); ++s)
		{
			if (Spine[s].GetInt() < 0)
			{
				continue;
			}
			FTransform T = CS.GetComponentSpaceTransform(Spine[s]);
			// Positive pitch = looking up: rotate the torso back about +Y.
			const FQuat Delta(FRotator(Pitch * SpineShare[s], 0.f, 0.f));
			T.SetRotation((Delta * T.GetRotation()).GetNormalized());
			const FBoneTransform Mod(Spine[s], T);
			CS.SafeSetCSBoneTransforms(MakeArrayView(&Mod, 1));
		}
		FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(MoveTemp(CS), Output.Pose);
	}
	// ---- head scale (xPawn.SetHeadScale -> SetBoneScale(4, HeadScale, 'head'): the head and everything under it) ----
	if (Head.GetInt() >= 0 && !FMath::IsNearlyEqual(In.HeadScale, 1.f))
	{
		Output.Pose[Head].SetScale3D(Output.Pose[Head].GetScale3D() * In.HeadScale);
	}
	return true;
}

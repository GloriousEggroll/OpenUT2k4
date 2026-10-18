#include "ArenaMover.h"
#include "OpenUT2k4.h"
#include "ArenaDamage.h"
#include "ArenaRuntimeContent.h"
#include "ArenaRuntimeMap.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "ArenaGameMode.h"
#include "ArenaNavGraph.h"

namespace ArenaMoverLocal
{
	EArenaMoverState ParseState(const FString& S)
	{
		static const TPair<const TCHAR*, EArenaMoverState> Names[] = {
			{ TEXT("BumpOpenTimed"), EArenaMoverState::BumpOpenTimed }, { TEXT("BumpButton"), EArenaMoverState::BumpButton },
			{ TEXT("StandOpenTimed"), EArenaMoverState::StandOpenTimed }, { TEXT("TriggerOpenTimed"), EArenaMoverState::TriggerOpenTimed },
			{ TEXT("TriggerToggle"), EArenaMoverState::TriggerToggle }, { TEXT("TriggerControl"), EArenaMoverState::TriggerControl },
			{ TEXT("TriggerPound"), EArenaMoverState::TriggerPound }, { TEXT("ConstantLoop"), EArenaMoverState::ConstantLoop },
			{ TEXT("LeadInOutLooper"), EArenaMoverState::LeadInOutLooper }, { TEXT("TriggerAdvance"), EArenaMoverState::TriggerAdvance } };
		for (const auto& N : Names)
		{
			if (S.Equals(N.Key, ESearchCase::IgnoreCase))
			{
				return N.Value;
			}
		}
		return S.IsEmpty() ? EArenaMoverState::BumpOpenTimed : EArenaMoverState::None;
	}

	USoundBase* Sound(const FArenaUE2Props& P, const TCHAR* Key)
	{
		const FString S = P.Str(Key);
		return S.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadSoundByObjectPath(S);
	}

	bool IsTimed(EArenaMoverState S)
	{
		return S == EArenaMoverState::BumpOpenTimed || S == EArenaMoverState::BumpButton || S == EArenaMoverState::StandOpenTimed
			|| S == EArenaMoverState::TriggerOpenTimed;
	}
}

AArenaMover::AArenaMover()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->CanCharacterStepUpOn = ECB_Yes;
	AmbientAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("AmbientAudio"));
	AmbientAudio->SetupAttachment(Mesh);
	AmbientAudio->bAutoActivate = false;
}

void AArenaMover::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	using namespace ArenaMoverLocal;
	const FArenaUE2Props& P = Desc.Props;
	constexpr float S = ArenaUE2World::Scale;
	MeshDesc = Desc;
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	Event = Desc.Event;
	InitialState = ParseState(P.Str(TEXT("InitialState")));
	MoverEncroachType = uint8(P.Num(TEXT("MoverEncroachType"), 1));
	MoverGlideType = uint8(P.Num(TEXT("MoverGlideType"), 1));
	BumpType = uint8(P.Num(TEXT("BumpType"), 0));
	NumKeys = FMath::Clamp(int32(P.Num(TEXT("NumKeys"), 2)), 1, 24);
	KeyNum = FMath::Clamp(int32(P.Num(TEXT("KeyNum"), 0)), 0, NumKeys - 1);
	MoveTime = FMath::Max(0.01f, float(P.Num(TEXT("MoveTime"), 1.0)));
	StayOpenTime = float(P.Num(TEXT("StayOpenTime"), 4.0));
	OtherTime = float(P.Num(TEXT("OtherTime"), 0.0));
	DelayTime = float(P.Num(TEXT("DelayTime"), 0.0));
	EncroachDamage = int32(P.Num(TEXT("EncroachDamage"), 0));
	bTriggerOnceOnly = P.Bool(TEXT("bTriggerOnceOnly"));
	bDamageTriggered = P.Bool(TEXT("bDamageTriggered"));
	DamageThreshold = float(P.Num(TEXT("DamageThreshold"), 0.0));
	bToggleDirection = P.Bool(TEXT("bToggleDirection"), true);
	KeyPos.SetNum(NumKeys);
	KeyRot.SetNum(NumKeys);
	for (int32 i = 0; i < NumKeys; ++i)
	{
		KeyPos[i] = FArenaUE2Props::ToVector(P.Indexed(TEXT("KeyPos"), i)) * S;
		KeyRot[i] = FArenaUE2Props::ToRotator(P.Indexed(TEXT("KeyRot"), i));
	}
	BasePos = P.Has(TEXT("BasePos")) ? P.Vec(TEXT("BasePos")) * S : Desc.Location - KeyPos[KeyNum];
	BaseRot = P.Has(TEXT("BaseRot")) ? P.Rot(TEXT("BaseRot")) : Desc.Rotation - KeyRot[KeyNum];
	OpeningSound = Sound(P, TEXT("OpeningSound"));
	OpenedSound = Sound(P, TEXT("OpenedSound"));
	ClosingSound = Sound(P, TEXT("ClosingSound"));
	ClosedSound = Sound(P, TEXT("ClosedSound"));
	MoveAmbientSound = Sound(P, TEXT("MoveAmbientSound"));
	OpeningEvent = P.Name(TEXT("OpeningEvent"));
	OpenedEvent = P.Name(TEXT("OpenedEvent"));
	ClosingEvent = P.Name(TEXT("ClosingEvent"));
	ClosedEvent = P.Name(TEXT("ClosedEvent"));
	ReturnGroup = P.Name(TEXT("ReturnGroup"));
	// PHYS_Rotating with a fixed rotation direction: a spinning mover (fans)
	if (int32(P.Num(TEXT("Physics"), 8)) == 5)
	{
		bRotating = true;
		RotationRate = P.Rot(TEXT("RotationRate"));
		InitialState = EArenaMoverState::None;
	}

	// mesh, skins, collision
	if (!Desc.StaticMesh.IsEmpty())
	{
		if (UStaticMesh* SM = ArenaRuntimeContent::LoadStaticMeshByObjectPath(Desc.StaticMesh))
		{
			Mesh->SetStaticMesh(SM);
			for (int32 i = 0; i < Desc.Skins.Num(); ++i)
			{
				if (Desc.Skins[i].IsEmpty() || Desc.Skins[i] == TEXT("None"))
				{
					continue;
				}
				const int32 Slot = ArenaRuntimeContent::GetStaticMeshSlotForSection(Desc.StaticMesh, i);
				if (Slot >= 0)
				{
					Mesh->SetMaterial(Slot, ArenaRuntimeContent::LoadMaterialByObjectPath(Desc.Skins[i]));
				}
			}
		}
	}
	const bool bCollides = P.Bool(TEXT("bCollideActors"), true) && P.Bool(TEXT("bBlockActors"), true);
	if (!bCollides)
	{
		Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}
	else if (Mesh->GetStaticMesh())
	{
		// UE2 collision model and zero / non-zero extent trace flags (ArenaRuntimeMap::ApplyUE2MeshCollision)
		bool bBox = false, bLine = false;
		if (UStaticMesh* ModelMesh = ArenaRuntimeContent::LoadStaticMeshCollisionModel(Desc.StaticMesh, bBox, bLine))
		{
			CollisionModel = NewObject<UStaticMeshComponent>(this, TEXT("CollisionModel"));
			CollisionModel->SetMobility(EComponentMobility::Movable);
			CollisionModel->SetStaticMesh(ModelMesh);
			CollisionModel->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			CollisionModel->SetGenerateOverlapEvents(false);
			CollisionModel->SetCanEverAffectNavigation(false);
			CollisionModel->CanCharacterStepUpOn = ECB_Yes;
			CollisionModel->SetVisibility(false);
			CollisionModel->SetHiddenInGame(true);
			CollisionModel->SetCastShadow(false);
			CollisionModel->bVisibleInRayTracing = false;
			CollisionModel->SetupAttachment(Mesh);
			CollisionModel->RegisterComponent();
		}
		ArenaRuntimeMap::ApplyUE2MeshCollision(Mesh, CollisionModel, bBox, bLine, P.Bool(TEXT("bBlockNonZeroExtentTraces"), true),
			P.Bool(TEXT("bBlockZeroExtentTraces"), true));
	}
	if (Desc.bHidden || P.Bool(TEXT("bHidden")))
	{
		Mesh->SetVisibility(false);
	}
	InitialKeyNum = KeyNum;
	CurLoc = BasePos + KeyPos[KeyNum];
	CurRot = BaseRot + KeyRot[KeyNum];
	ApplyPose(CurLoc, CurRot);
	SetActorTickEnabled(true);
}

void AArenaMover::PostInitUE2()
{
	if (ReturnGroup.IsNone() && Tag.IsNone())
	{
		return;
	}
	const FName Group = ReturnGroup.IsNone() ? Tag : ReturnGroup;
	for (TActorIterator<AArenaMover> It(GetWorld()); It; ++It)
	{
		if (*It != this && ((!It->ReturnGroup.IsNone() && It->ReturnGroup == Group) || (It->ReturnGroup.IsNone() && It->Tag == Group && !ReturnGroup.IsNone())))
		{
			ReturnGroupMovers.Add(*It);
		}
	}
}

void AArenaMover::BeginPlay()
{
	Super::BeginPlay();
	if (MoveAmbientSound)
	{
		AmbientAudio->SetSound(MoveAmbientSound);
	}
	// watching / playing on a UT2004 server: only the server's SimInterpolate / RealPosition move it (SetNetMove /
	// SetNetRest), never our own pawns standing on it, bumping it or the map's local triggers
	bNetDriven = GetWorld() && GetWorld()->URL.HasOption(TEXT("UE2Net"));
	if (!bNetDriven && (InitialState == EArenaMoverState::ConstantLoop || InitialState == EArenaMoverState::LeadInOutLooper))
	{
		DoOpen();
	}
}

FTransform AArenaMover::PoseAt(const FVector& Loc, const FRotator& Rot) const
{
	return MeshDesc.MeshTransform(Loc, Rot);
}

void AArenaMover::ApplyPose(const FVector& Loc, const FRotator& Rot)
{
	const FTransform T = PoseAt(Loc, Rot);
	SetActorScale3D(T.GetScale3D());
	SetActorLocationAndRotation(T.GetLocation(), T.GetRotation(), false, nullptr, ETeleportType::None);
}

FVector AArenaMover::GetKeyLocation(int32 I) const
{
	I = FMath::Clamp(I, 0, NumKeys - 1);
	const FVector Delta = (BasePos + KeyPos[I]) - CurLoc;
	const FBox B = Mesh->Bounds.GetBox().ShiftBy(Delta);
	return FVector(B.GetCenter().X, B.GetCenter().Y, B.Max.Z);
}

float AArenaMover::GetKeyTopZ(int32 I) const
{
	return GetKeyLocation(I).Z;
}

void AArenaMover::PlayMoverSound(USoundBase* Sound)
{
	if (Sound && GetWorld() && GetWorld()->HasBegunPlay())
	{
		UGameplayStatics::PlaySoundAtLocation(this, Sound, Mesh->Bounds.Origin);
	}
}

void AArenaMover::InterpolateTo(int32 NewKey, float Time)
{
	NewKey = FMath::Clamp(NewKey, 0, NumKeys - 1);
	FromLoc = CurLoc;
	FromRot = CurRot;
	TargetKey = NewKey;
	Alpha = 0.f;
	Rate = 1.f / FMath::Max(Time, 0.01f);
	bInterpolating = true;
	++NetMoveSerial;
	if (MoveAmbientSound && !AmbientAudio->IsPlaying())
	{
		AmbientAudio->Play();
	}
}

void AArenaMover::DoOpen()
{
	if (NumKeys < 2)
	{
		return;
	}
	bOpening = true;
	bWaitToClose = false;
	if (DelayTime > 0.f && !bWaitToOpen && !bInterpolating && KeyNum == 0)
	{
		bWaitToOpen = true;
		WaitTime = DelayTime;
		return;
	}
	bWaitToOpen = false;
	if (UArenaEventSubsystem::IsLogging())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Mover: %s opens (key %d of %d, by %s)"), *UE2Name, KeyNum, NumKeys, *GetNameSafe(LastInstigator.Get()));
	}
	PlayMoverSound(OpeningSound);
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		Events->TriggerEvent(OpeningEvent, this, LastInstigator.Get());
	}
	InterpolateTo(FMath::Min(KeyNum + 1, NumKeys - 1), MoveTime);
}

void AArenaMover::DoClose()
{
	if (NumKeys < 2)
	{
		return;
	}
	bOpening = false;
	bWaitToClose = false;
	bWaitToOpen = false;
	if (UArenaEventSubsystem::IsLogging())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Mover: %s closes (key %d)"), *UE2Name, KeyNum);
	}
	PlayMoverSound(ClosingSound);
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		Events->TriggerEvent(ClosingEvent, this, LastInstigator.Get());
	}
	// from a partial move: back to the key we came from
	const int32 Target = bInterpolating ? KeyNum : FMath::Max(KeyNum - 1, 0);
	InterpolateTo(Target, MoveTime * (bInterpolating ? FMath::Max(Alpha, 0.05f) : 1.f));
}

void AArenaMover::FinishedInterpolation()
{
	bInterpolating = false;
	KeyNum = TargetKey;
	// a big scripted mover at rest (AS-Convoy's crane, a bridge): the bots' nav graph picks up the floor it brings
	if (HasAuthority() && Mesh && InitialState != EArenaMoverState::StandOpenTimed && InitialState != EArenaMoverState::BumpOpenTimed
		&& InitialState != EArenaMoverState::ConstantLoop && InitialState != EArenaMoverState::LeadInOutLooper && Mesh->Bounds.BoxExtent.GetMax() > 300.f)
	{
		if (const AArenaGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr)
		{
			if (AArenaNavGraph* Nav = GM->GetNavGraph())
			{
				Nav->RefreshAround(Mesh->Bounds.GetBox().ExpandBy(600.f), KeyNum != 0 ? Mesh.Get() : nullptr);
			}
		}
	}
	UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this);
	if (bOpening)
	{
		if (KeyNum < NumKeys - 1 && InitialState != EArenaMoverState::TriggerAdvance)
		{
			InterpolateTo(KeyNum + 1, MoveTime);
			return;
		}
		AmbientAudio->Stop();
		PlayMoverSound(OpenedSound);
		if (Events)
		{
			Events->TriggerEvent(OpenedEvent, this, LastInstigator.Get());
			Events->TriggerEvent(Event, this, LastInstigator.Get());
		}
		switch (InitialState)
		{
		case EArenaMoverState::TriggerPound:
			bWaitToClose = true;
			WaitTime = FMath::Max(OtherTime, 0.01f);
			break;
		case EArenaMoverState::ConstantLoop:
		case EArenaMoverState::LeadInOutLooper:
			bWaitToClose = true;
			WaitTime = 0.f;
			break;
		case EArenaMoverState::TriggerToggle:
		case EArenaMoverState::TriggerControl:
		case EArenaMoverState::TriggerAdvance:
		case EArenaMoverState::None:
			break;
		case EArenaMoverState::TriggerOpenTimed:
			// UE2 Mover state TriggerOpenTimed: after StayOpenTime a bTriggerOnceOnly mover goes to state '' and stays
			// open (AS-Glacier's / AS-Convoy's objective doors)
			if (!bTriggerOnceOnly)
			{
				bWaitToClose = true;
				WaitTime = StayOpenTime;
			}
			break;
		default:
			bWaitToClose = true;
			WaitTime = StayOpenTime;
			break;
		}
	}
	else
	{
		if (KeyNum > 0)
		{
			InterpolateTo(KeyNum - 1, MoveTime);
			return;
		}
		AmbientAudio->Stop();
		PlayMoverSound(ClosedSound);
		if (Events)
		{
			Events->TriggerEvent(ClosedEvent, this, LastInstigator.Get());
			Events->UntriggerEvent(Event, this, LastInstigator.Get());
		}
		if (InitialState == EArenaMoverState::ConstantLoop || InitialState == EArenaMoverState::LeadInOutLooper
			|| (InitialState == EArenaMoverState::TriggerPound && bTriggerOn))
		{
			bWaitToOpen = true;
			WaitTime = InitialState == EArenaMoverState::TriggerPound ? FMath::Max(StayOpenTime, 0.01f) : 0.f;
		}
	}
}

UStaticMeshComponent* AArenaMover::PawnCollision() const
{
	return CollisionModel && CollisionModel->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block ? CollisionModel.Get() : Mesh.Get();
}

bool AArenaMover::IsBaseOf(const ACharacter* C) const
{
	const UPrimitiveComponent* Base = C ? Cast<UPrimitiveComponent>(C->GetMovementBaseObject()) : nullptr;
	return Base && (Base == Mesh || (CollisionModel && Base == CollisionModel));
}

APawn* AArenaMover::FindEncroached(const FVector& Loc, const FRotator& Rot) const
{
	UStaticMeshComponent* Blocker = PawnCollision();
	if (!Blocker->IsCollisionEnabled() || !Blocker->GetStaticMesh() || Blocker->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block)
	{
		return nullptr;
	}
	const FTransform T = PoseAt(Loc, Rot);
	const FVector Delta = T.GetLocation() - GetActorLocation();
	const FBox Moved = Mesh->Bounds.GetBox().ShiftBy(Delta).ExpandBy(2.f);
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* P = *It;
		ACharacter* C = Cast<ACharacter>(P);
		if (!C || !Moved.Intersect(C->GetComponentsBoundingBox()))
		{
			continue;
		}
		// riders move with us
		if (IsBaseOf(C))
		{
			continue;
		}
		const UCapsuleComponent* Cap = C->GetCapsuleComponent();
		// test the capsule in the mover's frame after the move: place the capsule relative to the moved pose
		const FTransform Rel = FTransform(Cap->GetComponentQuat(), Cap->GetComponentLocation()).GetRelativeTransform(T);
		const FTransform Now = Rel * GetActorTransform();
		FCollisionShape Shape = FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius() - 2.f, Cap->GetScaledCapsuleHalfHeight() - 2.f);
		if (Blocker->OverlapComponent(Now.GetLocation(), Now.GetRotation(), Shape))
		{
			return P;
		}
	}
	return nullptr;
}

void AArenaMover::HandleEncroach(APawn* Other)
{
	if (bNetDriven)
	{
		return; // the server decides what a mover does to what it runs into
	}
	if (EncroachDamage > 0 && Other)
	{
		FArenaHitInfo Info;
		Info.WeaponName = TEXT("Crushed");
		ArenaDamage::ApplyDamage(Other, float(EncroachDamage), nullptr, this, Info);
	}
	if (MoverEncroachType == 1)
	{
		// return: reverse, with the return group
		auto Reverse = [](AArenaMover* M)
		{
			if (M->bOpening)
			{
				M->DoClose();
			}
			else
			{
				M->DoOpen();
			}
		};
		Reverse(this);
		for (const TWeakObjectPtr<AArenaMover>& M : ReturnGroupMovers)
		{
			if (M.IsValid() && M->bInterpolating)
			{
				Reverse(M.Get());
			}
		}
	}
}

void AArenaMover::SetNetMove(int32 NewKey, int32 PrevKey, float InAlpha, float InRate)
{
	bNetDriven = true;
	bWaitToClose = bWaitToOpen = false;
	if (NumKeys < 1 || KeyPos.Num() < NumKeys || KeyRot.Num() < NumKeys)
	{
		return;
	}
	NewKey = FMath::Clamp(NewKey, 0, NumKeys - 1);
	PrevKey = FMath::Clamp(PrevKey, 0, NumKeys - 1);
	FromLoc = BasePos + KeyPos[PrevKey];
	FromRot = BaseRot + KeyRot[PrevKey];
	TargetKey = NewKey;
	Alpha = FMath::Clamp(InAlpha, 0.f, 1.f);
	Rate = FMath::Max(InRate, 0.01f);
	bOpening = NewKey > PrevKey;
	bInterpolating = Alpha < 1.f;
	if (bInterpolating && MoveAmbientSound && !AmbientAudio->IsPlaying())
	{
		AmbientAudio->Play();
	}
}

void AArenaMover::SetNetRest(const FVector& Location, const FRotator& Rotation)
{
	bNetDriven = true;
	bWaitToClose = bWaitToOpen = false;
	if (bInterpolating)
	{
		return; // the move ends there anyway
	}
	CurLoc = Location;
	CurRot = Rotation;
	ApplyPose(CurLoc, CurRot);
}

void AArenaMover::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bNetDriven && !bRotating)
	{
		// the server's moves only: interpolate, nothing else
		if (bInterpolating && KeyPos.IsValidIndex(TargetKey) && KeyRot.IsValidIndex(TargetKey))
		{
			const FVector PrevNetLoc = CurLoc;
			Alpha = FMath::Min(1.f, Alpha + DeltaSeconds * Rate);
			const float A = MoverGlideType == 1 ? Alpha * Alpha * (3.f - 2.f * Alpha) : Alpha;
			CurLoc = FMath::Lerp(FromLoc, BasePos + KeyPos[TargetKey], A);
			CurRot = FromRot + ((BaseRot + KeyRot[TargetKey]) - FromRot) * A;
			ApplyPose(CurLoc, CurRot);
			if (Mesh && DeltaSeconds > UE_KINDA_SMALL_NUMBER)
			{
				Mesh->ComponentVelocity = (CurLoc - PrevNetLoc) / DeltaSeconds;
			}
			if (Alpha >= 1.f)
			{
				bInterpolating = false;
				KeyNum = TargetKey;
				AmbientAudio->Stop();
			}
		}
		return;
	}
	// UE2 Mover.Velocity while interpolating: pawns jumping off a rising lift add its vertical speed (Pawn.DoJump).
	const FVector PrevLoc = CurLoc;
	if (Mesh)
	{
		Mesh->ComponentVelocity = FVector::ZeroVector;
	}
	if (bRotating)
	{
		CurRot += RotationRate * DeltaSeconds;
		CurRot.Normalize();
		ApplyPose(CurLoc, CurRot);
		return;
	}
	if (InitialState != EArenaMoverState::None)
	{
		ScanPawns();
	}
	if (bInterpolating)
	{
		const float NewAlpha = FMath::Min(1.f, Alpha + DeltaSeconds * Rate);
		const float A = MoverGlideType == 1 ? NewAlpha * NewAlpha * (3.f - 2.f * NewAlpha) : NewAlpha;
		const FVector Loc = FMath::Lerp(FromLoc, BasePos + KeyPos[TargetKey], A);
		const FRotator Rot = FromRot + ((BaseRot + KeyRot[TargetKey]) - FromRot) * A;
		if (MoverEncroachType != 3)
		{
			if (APawn* Hit = FindEncroached(Loc, Rot))
			{
				if (MoverEncroachType == 0)
				{
					return; // stop
				}
				if (MoverEncroachType == 1)
				{
					HandleEncroach(Hit);
					return;
				}
				// crush: damage now and then, keep moving
				static float LastCrush = 0.f;
				if (GetWorld()->GetTimeSeconds() - LastCrush > 0.5f)
				{
					LastCrush = GetWorld()->GetTimeSeconds();
					HandleEncroach(Hit);
				}
			}
		}
		Alpha = NewAlpha;
		CurLoc = Loc;
		CurRot = Rot;
		ApplyPose(CurLoc, CurRot);
		if (Mesh && DeltaSeconds > UE_KINDA_SMALL_NUMBER)
		{
			Mesh->ComponentVelocity = (CurLoc - PrevLoc) / DeltaSeconds;
		}
		if (Alpha >= 1.f)
		{
			FinishedInterpolation();
		}
		return;
	}
	if (bWaitToClose || bWaitToOpen)
	{
		// lifts stay up while someone stands on them
		if (bWaitToClose && InitialState == EArenaMoverState::StandOpenTimed && TouchingPawns.Num() > 0)
		{
			WaitTime = FMath::Max(WaitTime, 0.5f);
		}
		WaitTime -= DeltaSeconds;
		if (WaitTime <= 0.f)
		{
			if (bWaitToClose)
			{
				DoClose();
			}
			else
			{
				DoOpen();
			}
		}
	}
}

void AArenaMover::ScanPawns()
{
	const bool bBump = InitialState == EArenaMoverState::BumpOpenTimed || InitialState == EArenaMoverState::BumpButton;
	const bool bStand = InitialState == EArenaMoverState::StandOpenTimed;
	if (!bBump && !bStand)
	{
		return;
	}
	const FBox Box = Mesh->Bounds.GetBox().ExpandBy(FVector(80.f, 80.f, 200.f));
	TSet<TWeakObjectPtr<APawn>> Now;
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		ACharacter* C = Cast<ACharacter>(*It);
		if (!C || !Box.IsInside(C->GetActorLocation()))
		{
			continue;
		}
		bool bTouch = false;
		if (bStand)
		{
			bTouch = IsBaseOf(C);
		}
		else
		{
			const UCapsuleComponent* Cap = C->GetCapsuleComponent();
			bTouch = PawnCollision()->OverlapComponent(Cap->GetComponentLocation(), Cap->GetComponentQuat(),
				FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius() + 6.f, Cap->GetScaledCapsuleHalfHeight() + 6.f));
		}
		if (bTouch)
		{
			Now.Add(C);
			if (!TouchingPawns.Contains(C))
			{
				if (bStand)
				{
					StandOn(C);
				}
				else
				{
					Bump(C);
				}
			}
		}
	}
	TouchingPawns = MoveTemp(Now);
}

void AArenaMover::Bump(APawn* Other)
{
	if (bNetDriven || !Other || (BumpType == 0 && !Other->GetController()))
	{
		return;
	}
	if (bTriggerOnceOnly && bTriggeredOnce)
	{
		return;
	}
	bTriggeredOnce = true;
	LastInstigator = Other;
	if (!bOpening || (!bInterpolating && KeyNum == 0))
	{
		DoOpen();
	}
	else if (bWaitToClose)
	{
		WaitTime = FMath::Max(WaitTime, StayOpenTime);
	}
}

void AArenaMover::StandOn(APawn* Other)
{
	Bump(Other);
}

void AArenaMover::Trigger(AActor* Other, APawn* EventInstigator)
{
	if (bNetDriven || (bTriggerOnceOnly && bTriggeredOnce))
	{
		return;
	}
	bTriggeredOnce = true;
	LastInstigator = EventInstigator;
	switch (InitialState)
	{
	case EArenaMoverState::TriggerOpenTimed:
	case EArenaMoverState::BumpOpenTimed:
	case EArenaMoverState::StandOpenTimed:
	case EArenaMoverState::BumpButton:
		if (!bOpening || (!bInterpolating && KeyNum == 0))
		{
			DoOpen();
		}
		break;
	case EArenaMoverState::TriggerToggle:
		if (bOpening || KeyNum > 0)
		{
			DoClose();
		}
		else
		{
			DoOpen();
		}
		break;
	case EArenaMoverState::TriggerControl:
		DoOpen();
		break;
	case EArenaMoverState::TriggerAdvance:
		// UT2004 Mover state TriggerAdvance: on to the next key, one step per trigger
		if (!bInterpolating && KeyNum < NumKeys - 1)
		{
			bOpening = true;
			bWaitToClose = false;
			PlayMoverSound(OpeningSound);
			InterpolateTo(KeyNum + 1, MoveTime);
		}
		break;
	case EArenaMoverState::TriggerPound:
		bTriggerOn = true;
		if (!bInterpolating && KeyNum == 0)
		{
			DoOpen();
		}
		break;
	default:
		break;
	}
}

void AArenaMover::ResetUE2()
{
	KeyNum = InitialKeyNum;
	bInterpolating = bOpening = bWaitToClose = bWaitToOpen = bTriggerOn = bTriggeredOnce = false;
	AmbientAudio->Stop();
	if (!bRotating)
	{
		CurLoc = BasePos + KeyPos[KeyNum];
		CurRot = BaseRot + KeyRot[KeyNum];
		ApplyPose(CurLoc, CurRot);
	}
	if (InitialState == EArenaMoverState::ConstantLoop || InitialState == EArenaMoverState::LeadInOutLooper)
	{
		DoOpen();
	}
}

void AArenaMover::UnTrigger(AActor* Other, APawn* EventInstigator)
{
	if (bNetDriven)
	{
		return;
	}
	if (InitialState == EArenaMoverState::TriggerControl)
	{
		DoClose();
	}
	else if (InitialState == EArenaMoverState::TriggerPound)
	{
		bTriggerOn = false;
	}
}

float AArenaMover::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	const float Out = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (bDamageTriggered && DamageAmount >= DamageThreshold)
	{
		Trigger(DamageCauser, EventInstigator ? EventInstigator->GetPawn() : nullptr);
	}
	return Out;
}

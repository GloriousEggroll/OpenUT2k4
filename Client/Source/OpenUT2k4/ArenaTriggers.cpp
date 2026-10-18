#include "ArenaTriggers.h"
#include "OpenUT2k4.h"
#include "ArenaGameMode.h"
#include "ArenaPlayerController.h"
#include "ArenaRules_AS.h"
#include "ArenaRuntimeContent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace ArenaTriggersLocal
{
	void ShowMessage(APawn* To, const FString& Text, bool bBroadcast = false)
	{
		if (Text.IsEmpty() || !To)
		{
			return;
		}
		UWorld* World = To->GetWorld();
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			AArenaPlayerController* PC = Cast<AArenaPlayerController>(It->Get());
			if (PC && (bBroadcast || PC->GetPawn() == To))
			{
				PC->AddCenterMessage(Text, FLinearColor::White, 3.f);
			}
		}
	}

	/** Pawn cylinder (radius, half height) for UE2 touch tests. */
	void PawnCylinder(const APawn* P, float& R, float& H)
	{
		R = 30.f;
		H = 50.f;
		if (const ACharacter* C = Cast<ACharacter>(P))
		{
			R = C->GetCapsuleComponent()->GetScaledCapsuleRadius();
			H = C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
		else if (P)
		{
			const FBox B = P->GetComponentsBoundingBox();
			if (B.IsValid)
			{
				const FVector E = B.GetExtent();
				R = float(FMath::Max(E.X, E.Y));
				H = float(E.Z);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaUE2EventActor
// ---------------------------------------------------------------------------------------------

AArenaUE2EventActor::AArenaUE2EventActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickInterval = 0.f;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetHidden(true);
}

void AArenaUE2EventActor::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	Event = Desc.Event;
	// UE2 collision cylinder (scaled by the converter); DrawScale3D does not scale it
	CollisionRadius = Desc.CollisionRadius;
	CollisionHeight = Desc.CollisionHeight;
	bCollideActors = Desc.Props.Bool(TEXT("bCollideActors"), true);
}

void AArenaUE2EventActor::BeginPlay()
{
	Super::BeginPlay();
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		Events->RegisterActor(this, Tag);
	}
	SetActorTickEnabled(bWantsTouch || PrimaryActorTick.bStartWithTickEnabled);
}

void AArenaUE2EventActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		Events->UnregisterActor(this);
	}
	Super::EndPlay(EndPlayReason);
}

bool AArenaUE2EventActor::IsTouching(const APawn* Other) const
{
	if (!Other || CollisionRadius <= 0.f)
	{
		return false;
	}
	float R, H;
	ArenaTriggersLocal::PawnCylinder(Other, R, H);
	const FVector D = Other->GetActorLocation() - GetActorLocation();
	return FVector2D(D.X, D.Y).SizeSquared() <= FMath::Square(R + CollisionRadius) && FMath::Abs(D.Z) <= H + CollisionHeight;
}

void AArenaUE2EventActor::FireEvent(FName Name, APawn* EventInstigator, bool bTrigger)
{
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		if (bTrigger)
		{
			Events->TriggerEvent(Name, this, EventInstigator);
		}
		else
		{
			Events->UntriggerEvent(Name, this, EventInstigator);
		}
	}
}

void AArenaUE2EventActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bWantsTouch || !bCollideActors)
	{
		return;
	}
	TSet<TWeakObjectPtr<APawn>> Now;
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* P = *It;
		if (IsValid(P) && IsTouching(P))
		{
			Now.Add(P);
			if (!Touching.Contains(P))
			{
				Touching.Add(P);
				Touch(P);
			}
		}
	}
	for (auto It = Touching.CreateIterator(); It; ++It)
	{
		if (!Now.Contains(*It))
		{
			APawn* P = It->Get();
			It.RemoveCurrent();
			if (P)
			{
				UnTouch(P);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaTrigger
// ---------------------------------------------------------------------------------------------

void AArenaTrigger::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	const FArenaUE2Props& P = Desc.Props;
	TriggerType = uint8(P.Num(TEXT("TriggerType"), 0));
	Message = P.Str(TEXT("Message"));
	bTriggerOnceOnly = P.Bool(TEXT("bTriggerOnceOnly"));
	bInitiallyActive = P.Bool(TEXT("bInitiallyActive"), true);
	ClassProximityType = P.Str(TEXT("ClassProximityType"));
	RepeatTriggerTime = float(P.Num(TEXT("RepeatTriggerTime"), 0.0));
	ReTriggerDelay = float(P.Num(TEXT("ReTriggerDelay"), 0.0));
	DamageThreshold = float(P.Num(TEXT("DamageThreshold"), 0.0));
	InitialState = P.Name(TEXT("InitialState"));
	if (InitialState.IsNone())
	{
		InitialState = TEXT("NormalTrigger");
	}
	if (Desc.IsA(TEXT("Trigger_ASTeam")))
	{
		AssaultTeamOnly = FMath::Clamp(int32(P.Num(TEXT("Team"), P.Num(TEXT("AssaultTeam"), 0))), 0, 1);
	}
	bActive = bInitiallyActive;
	bWantsTouch = TriggerType != 4;
	if (TriggerType == 4 && CollisionRadius > 0.f)
	{
		// shoot triggers take hits on a cylinder
		ShootCollision = NewObject<UCapsuleComponent>(this, TEXT("ShootCollision"));
		ShootCollision->InitCapsuleSize(CollisionRadius, FMath::Max(CollisionHeight, CollisionRadius));
		ShootCollision->SetupAttachment(RootComponent);
		ShootCollision->SetCollisionProfileName(TEXT("NoCollision"));
		ShootCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		ShootCollision->SetCollisionObjectType(ECC_WorldDynamic);
		ShootCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
		ShootCollision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Block); // Weapon
		ShootCollision->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block); // Projectile
		ShootCollision->RegisterComponent();
	}
}

bool AArenaTrigger::IsRelevant(const APawn* Other) const
{
	if (!bActive || !Other)
	{
		return false;
	}
	if (AssaultTeamOnly != INDEX_NONE)
	{
		// Trigger_ASTeam.IsRelevant: the pawn's side (AS-MotherShip's 'Corereached' is for the attackers entering the core,
		// not the defenders spawning there)
		const AArenaGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr;
		const UArenaRules_AS* AS = GM ? Cast<UArenaRules_AS>(GM->GetRules()) : nullptr;
		const int32 PawnTeam = UArenaGameRules::GetTeamOf(Other->GetController());
		if (AS && (PawnTeam == AS->GetAttackingTeam()) != (AssaultTeamOnly == 0))
		{
			return false;
		}
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("UE2Trigger: %s (Trigger_ASTeam) touched by %s (%s, team %d, attackers %d) at %s"), *GetName(), *Other->GetName(),
			*GetNameSafe(Other->GetController()), PawnTeam, AS ? AS->GetAttackingTeam() : -1, *Other->GetActorLocation().ToCompactString());
	}
	switch (TriggerType)
	{
	case 0: // PlayerProximity: players and bots
	case 6: // UT2004 LivePlayerProximity: the same, alive (dead pawns don't touch anything here)
		return Other->GetController() != nullptr && Other->IsA<ACharacter>();
	case 5: // HumanPlayerProximity
		return Other->IsPlayerControlled();
	case 1: // PawnProximity
	case 3: // AnyProximity
		return true;
	case 2: // ClassProximity
	{
		const FString C = ClassProximityType;
		if (C.Contains(TEXT("Vehicle")))
		{
			return !Other->IsA<ACharacter>();
		}
		return Other->IsA<ACharacter>();
	}
	default:
		return false;
	}
}

void AArenaTrigger::Touch(APawn* Other)
{
	if (!IsRelevant(Other))
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (ReTriggerDelay > 0.f && Now - TriggerTime < ReTriggerDelay)
	{
		return;
	}
	TriggerTime = Now;
	FireEvent(Event, Other, true);
	ArenaTriggersLocal::ShowMessage(Other, Message);
	if (bTriggerOnceOnly)
	{
		bActive = false;
	}
	RepeatTimer = RepeatTriggerTime;
}

void AArenaTrigger::UnTouch(APawn* Other)
{
	if (Other && (TriggerType != 4) && (bActive || bTriggerOnceOnly))
	{
		FireEvent(Event, Other, false);
	}
}

void AArenaTrigger::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// RepeatTriggerTime: fire again while something stays inside
	if (RepeatTriggerTime > 0.f && bActive && Touching.Num() > 0)
	{
		RepeatTimer -= DeltaSeconds;
		if (RepeatTimer <= 0.f)
		{
			RepeatTimer = RepeatTriggerTime;
			for (const TWeakObjectPtr<APawn>& P : Touching)
			{
				if (P.IsValid() && IsRelevant(P.Get()))
				{
					FireEvent(Event, P.Get(), true);
					break;
				}
			}
		}
	}
}

void AArenaTrigger::Trigger(AActor* Other, APawn* EventInstigator)
{
	static const FName Toggles(TEXT("OtherTriggerToggles"));
	static const FName TurnsOn(TEXT("OtherTriggerTurnsOn"));
	static const FName TurnsOff(TEXT("OtherTriggerTurnsOff"));
	if (InitialState == Toggles)
	{
		bActive = !bActive;
	}
	else if (InitialState == TurnsOn)
	{
		bActive = true;
	}
	else if (InitialState == TurnsOff)
	{
		bActive = false;
	}
	if (bActive)
	{
		// pawns already inside count as touching now
		for (const TWeakObjectPtr<APawn>& P : Touching)
		{
			if (P.IsValid() && IsRelevant(P.Get()))
			{
				Touch(P.Get());
				break;
			}
		}
	}
}

float AArenaTrigger::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (TriggerType == 4 && bActive && DamageAmount >= DamageThreshold)
	{
		APawn* P = EventInstigator ? EventInstigator->GetPawn() : nullptr;
		FireEvent(Event, P, true);
		ArenaTriggersLocal::ShowMessage(P, Message);
		if (bTriggerOnceOnly)
		{
			bActive = false;
		}
	}
	return DamageAmount;
}

// ---------------------------------------------------------------------------------------------
// AArenaDispatcher
// ---------------------------------------------------------------------------------------------

void AArenaDispatcher::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	for (int32 i = 0; i < 8; ++i)
	{
		OutEvents.Add(FName(*FArenaUE2Props::ToString(Desc.Props.Indexed(TEXT("OutEvents"), i))));
		const TSharedPtr<FJsonValue> D = Desc.Props.Indexed(TEXT("OutDelays"), i);
		OutDelays.Add(D.IsValid() ? float(D->AsNumber()) : 0.f);
	}
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void AArenaDispatcher::Trigger(AActor* Other, APawn* EventInstigator)
{
	if (Step >= 0)
	{
		return; // UE2: ignores triggers while dispatching
	}
	Instigator = EventInstigator;
	Step = 0;
	StepTime = OutDelays.IsValidIndex(0) ? OutDelays[0] : 0.f;
	SetActorTickEnabled(true);
}

void AArenaDispatcher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Step < 0)
	{
		SetActorTickEnabled(false);
		return;
	}
	StepTime -= DeltaSeconds;
	while (Step >= 0 && StepTime <= 0.f)
	{
		if (OutEvents.IsValidIndex(Step) && !OutEvents[Step].IsNone())
		{
			FireEvent(OutEvents[Step], Instigator.Get(), true);
		}
		++Step;
		if (Step >= OutEvents.Num())
		{
			Step = -1;
			break;
		}
		StepTime += OutDelays[Step];
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaCounter / AArenaTriggeredCondition
// ---------------------------------------------------------------------------------------------

void AArenaCounter::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	NumToCount = FMath::Max(1, int32(Desc.Props.Num(TEXT("NumToCount"), 2)));
	bShowMessage = Desc.Props.Bool(TEXT("bShowMessage"));
	CountMessage = Desc.Props.Str(TEXT("CountMessage"));
	CompleteMessage = Desc.Props.Str(TEXT("CompleteMessage"));
	Remaining = NumToCount;
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void AArenaCounter::Trigger(AActor* Other, APawn* EventInstigator)
{
	if (Remaining <= 0)
	{
		return;
	}
	--Remaining;
	if (Remaining == 0)
	{
		if (bShowMessage)
		{
			ArenaTriggersLocal::ShowMessage(EventInstigator, CompleteMessage);
		}
		FireEvent(Event, EventInstigator, true);
	}
	else if (bShowMessage)
	{
		ArenaTriggersLocal::ShowMessage(EventInstigator, CountMessage.Replace(TEXT("%i"), *FString::FromInt(Remaining)));
	}
}

void AArenaTriggeredCondition::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	bToggled = Desc.Props.Bool(TEXT("bToggled"));
	bEnabled = Desc.Props.Bool(TEXT("bEnabled"));
	bTriggerControlled = Desc.Props.Bool(TEXT("bTriggerControlled"));
	bInitialValue = bEnabled;
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void AArenaTriggeredCondition::Trigger(AActor* Other, APawn* EventInstigator)
{
	if (bToggled)
	{
		bEnabled = !bEnabled;
	}
	else if (bTriggerControlled)
	{
		bEnabled = !bInitialValue;
	}
	else
	{
		bEnabled = !bInitialValue;
	}
}

void AArenaTriggeredCondition::UnTrigger(AActor* Other, APawn* EventInstigator)
{
	if (bTriggerControlled)
	{
		bEnabled = bInitialValue;
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaTeleporter
// ---------------------------------------------------------------------------------------------

void AArenaTeleporter::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	const FArenaUE2Props& P = Desc.Props;
	URL = P.Str(TEXT("URL"));
	bEnabled = P.Bool(TEXT("bEnabled"), true);
	bChangesVelocity = P.Bool(TEXT("bChangesVelocity"));
	bChangesYaw = P.Bool(TEXT("bChangesYaw"), true);
	bReversesX = P.Bool(TEXT("bReversesX"));
	bReversesY = P.Bool(TEXT("bReversesY"));
	bReversesZ = P.Bool(TEXT("bReversesZ"));
	TargetVelocity = P.Vec(TEXT("TargetVelocity")) * ArenaUE2World::Scale;
	if (CollisionRadius <= 0.f)
	{
		CollisionRadius = 18.f * ArenaUE2World::Scale;
		CollisionHeight = 40.f * ArenaUE2World::Scale;
	}
	bWantsTouch = !URL.IsEmpty();
}

TArray<AArenaTeleporter*> AArenaTeleporter::GetDestinations() const
{
	TArray<AArenaTeleporter*> Out;
	if (URL.IsEmpty() || URL.Contains(TEXT("/")) || URL.Contains(TEXT("#")))
	{
		return Out; // other levels are not supported
	}
	const FName Want(*URL);
	for (TActorIterator<AArenaTeleporter> It(GetWorld()); It; ++It)
	{
		if (*It != this && It->Tag == Want)
		{
			Out.Add(*It);
		}
	}
	return Out;
}

void AArenaTeleporter::Touch(APawn* Other)
{
	if (!bEnabled || !Other)
	{
		return;
	}
	// just arrived here: not again until the pawn has left
	if (Other->Tags.Contains(FName(TEXT("ArenaJustTeleported"))))
	{
		return;
	}
	TArray<AArenaTeleporter*> Dest = GetDestinations();
	if (Dest.Num() == 0)
	{
		return;
	}
	AArenaTeleporter* D = Dest[FMath::RandHelper(Dest.Num())];
	D->Accept(Other, this);
}

bool AArenaTeleporter::Accept(APawn* Incoming, AArenaTeleporter* Source)
{
	if (!Incoming)
	{
		return false;
	}
	ACharacter* C = Cast<ACharacter>(Incoming);
	FRotator NewRot = Incoming->GetControlRotation();
	FVector Vel = Incoming->GetVelocity();
	if (bChangesYaw && Source)
	{
		// keep the view relative to the exit: yaw + (dest - source) + 180
		const float Delta = GetActorRotation().Yaw - Source->GetActorRotation().Yaw + 180.f;
		NewRot.Yaw += Delta;
		Vel = FRotator(0.f, Delta, 0.f).RotateVector(Vel);
	}
	if (bChangesVelocity)
	{
		Vel = TargetVelocity;
	}
	if (bReversesX) Vel.X = -Vel.X;
	if (bReversesY) Vel.Y = -Vel.Y;
	if (bReversesZ) Vel.Z = -Vel.Z;
	FVector Dest = GetActorLocation();
	if (C)
	{
		// stand on the floor below the teleporter
		Dest.Z = GetFloorLocation().Z + C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f;
	}
	if (!Incoming->TeleportTo(Dest, FRotator(0.f, NewRot.Yaw, 0.f)))
	{
		return false;
	}
	if (AController* Ctrl = Incoming->GetController())
	{
		Ctrl->SetControlRotation(NewRot);
	}
	if (C && C->GetCharacterMovement())
	{
		C->GetCharacterMovement()->Velocity = Vel;
	}
	// no bounce back while standing in the destination teleporter
	Touching.Add(Incoming);
	if (Source)
	{
		Source->Touching.Remove(Incoming);
	}
	FireEvent(Event, Incoming, true);
	if (UArenaEventSubsystem::IsLogging())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Teleporter: %s -> %s: %s"), Source ? *Source->UE2Name : TEXT("?"), *UE2Name, *Incoming->GetName());
	}
	return true;
}

FVector AArenaTeleporter::GetFloorLocation() const
{
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(TeleporterFloor), false, this);
	const FVector L = GetActorLocation();
	if (GetWorld()->LineTraceSingleByChannel(Hit, L + FVector(0, 0, 20.f), L - FVector(0, 0, 400.f), ECC_WorldStatic, Params))
	{
		return Hit.ImpactPoint;
	}
	return L - FVector(0, 0, CollisionHeight);
}

void AArenaTeleporter::Trigger(AActor* Other, APawn* EventInstigator)
{
	bEnabled = !bEnabled;
	if (bEnabled)
	{
		for (const TWeakObjectPtr<APawn>& P : Touching)
		{
			if (P.IsValid())
			{
				Touch(P.Get());
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaScriptedTrigger
// ---------------------------------------------------------------------------------------------

void AArenaScriptedTrigger::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Super::InitFromUE2(Desc);
	for (const TSharedPtr<FJsonValue>& V : Desc.Props.Array(TEXT("Actions")))
	{
		const FString Ref = FArenaUE2Props::ToString(V);
		FString Class;
		TArray<FString> Chain;
		FArenaUE2Props P;
		FArenaScriptedAction& A = Actions.AddDefaulted_GetRef();
		if (!Desc.GetSubobject(Ref, Class, Chain, P))
		{
			A.Class = TEXT("Missing");
			continue;
		}
		A.Class = Class;
		A.EventName = P.Name(TEXT("Event"));
		if (Class.Equals(TEXT("ACTION_WaitForEvent"), ESearchCase::IgnoreCase))
		{
			A.EventName = P.Name(TEXT("ExternalEvent"));
		}
		else if (Class.Equals(TEXT("ACTION_DestroyActor"), ESearchCase::IgnoreCase))
		{
			A.EventName = P.Name(TEXT("DestroyTag"));
		}
		A.Duration = float(P.Num(TEXT("PauseTime"), 0.0));
		A.JumpIndex = int32(P.Num(TEXT("JumpToAction"), 0));
		A.Volume = float(P.Num(TEXT("Volume"), 1.0));
		A.Text = P.Str(TEXT("Message"));
		const FString Snd = P.Str(TEXT("Sound"));
		if (!Snd.IsEmpty())
		{
			A.Sound = ArenaRuntimeContent::LoadSoundByObjectPath(Snd);
		}
	}
}

void AArenaScriptedTrigger::ResetUE2()
{
	Current = 0;
	WaitUntil = -1.f;
	WaitEvent = NAME_None;
	bDone = false;
	Run();
}

void AArenaScriptedTrigger::BeginPlay()
{
	Super::BeginPlay();
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		EventHandle = Events->OnEvent.AddUObject(this, &AArenaScriptedTrigger::OnAnyEvent);
	}
	Run();
}

void AArenaScriptedTrigger::OnAnyEvent(FName Name, AActor* Other, APawn* EventInstigator, bool bTrigger)
{
	if (bTrigger && !WaitEvent.IsNone() && Name == WaitEvent)
	{
		WaitEvent = NAME_None;
		++Current;
		Run();
	}
}

void AArenaScriptedTrigger::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (WaitUntil >= 0.f && GetWorld()->GetTimeSeconds() >= WaitUntil)
	{
		WaitUntil = -1.f;
		++Current;
		Run();
	}
}

void AArenaScriptedTrigger::Run()
{
	if (bDone)
	{
		return;
	}
	// actions run until one waits; a guard stops endless GotoAction loops within one frame
	for (int32 Guard = 0; Guard < 256; ++Guard)
	{
		if (!Actions.IsValidIndex(Current))
		{
			bDone = true;
			return;
		}
		const FArenaScriptedAction& A = Actions[Current];
		const FString& C = A.Class;
		if (C.Equals(TEXT("ACTION_WaitForEvent"), ESearchCase::IgnoreCase))
		{
			WaitEvent = A.EventName;
			if (!WaitEvent.IsNone())
			{
				return;
			}
		}
		else if (C.Equals(TEXT("ACTION_WaitForTimer"), ESearchCase::IgnoreCase))
		{
			if (A.Duration > 0.f)
			{
				WaitUntil = GetWorld()->GetTimeSeconds() + A.Duration;
				return;
			}
		}
		else if (C.Equals(TEXT("ACTION_TriggerEvent"), ESearchCase::IgnoreCase))
		{
			FireEvent(A.EventName, nullptr, true);
		}
		else if (C.Equals(TEXT("ACTION_UntriggerEvent"), ESearchCase::IgnoreCase))
		{
			FireEvent(A.EventName, nullptr, false);
		}
		else if (C.StartsWith(TEXT("ACTION_Play")) && C.Contains(TEXT("Sound")))
		{
			if (A.Sound)
			{
				UGameplayStatics::PlaySoundAtLocation(this, A.Sound, GetActorLocation(), A.Volume);
			}
		}
		else if (C.Equals(TEXT("ACTION_GotoAction"), ESearchCase::IgnoreCase))
		{
			Current = FMath::Clamp(A.JumpIndex, 0, Actions.Num());
			if (Guard > 0 && Current <= 0)
			{
				// a loop without a wait: continue next frame
				WaitUntil = GetWorld()->GetTimeSeconds();
				--Current;
				return;
			}
			continue;
		}
		else if (C.Equals(TEXT("ACTION_DisplayMessage"), ESearchCase::IgnoreCase))
		{
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				ArenaTriggersLocal::ShowMessage(PC->GetPawn(), A.Text, true);
			}
		}
		else if (C.Equals(TEXT("ACTION_DestroyActor"), ESearchCase::IgnoreCase))
		{
			if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
			{
				for (AActor* T : Events->GetTaggedActors(A.EventName))
				{
					T->Destroy();
				}
			}
		}
		else if (UArenaEventSubsystem::IsLogging())
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Event: %s skips unsupported %s"), *UE2Name, *C);
		}
		++Current;
	}
	WaitUntil = GetWorld()->GetTimeSeconds();
}

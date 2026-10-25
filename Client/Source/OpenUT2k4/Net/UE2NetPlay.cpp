#include "UE2NetPlay.h"
#include "UE2NetJoin.h"
#include "UE2NetModWeapons.h"
#include "UE2NetModContent.h"
#include "UE2NetScriptWeapon.h"
#include "UE2NetScriptHUD.h"
#include "../ArenaUE2Effects.h"
#include "../ArenaHitchLog.h"

#include "../ArenaCharacterMovement.h"
#include "../ArenaFlag.h"
#include "../ArenaGameMode.h"
#include "../ArenaMover.h"
#include "../ArenaWorldZones.h"
#include "../ArenaNavGraph.h"
#include "../ArenaPickup.h"
#include "../ArenaPlayerController.h"
#include "../ArenaPlayerState.h"
#include "../ArenaTranslocator.h"
#include "../ArenaTypes.h"
#include "../ArenaUE2World.h"
#include "../ArenaUserSettings.h"
#include "../ArenaWeapon.h"
#include "../ArenaWeaponActors.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraTypes.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UE2NetActors.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetModClient.h"
#include "UE2NetNewNet.h"
#include "UE2NetWorld.h"

/**
 * Step 4 of Docs/UE2Net.md. The UE2 rules followed here are those of the 3369 scripts (Engine.PlayerController,
 * UnrealGame.UnrealPlayer, XGame.xPlayer / xPawn, Engine.Weapon), read from the install's packages:
 *
 *   ReplicateMove / CallServerMove: a move is ServerMove(TimeStamp, InAccel = Acceleration * 10, ClientLoc, bRun,
 *   bDuck, bJumpStatus (toggles per jump press), bDoubleJump, DoubleClickMove, ClientRoll, View = (Pitch / 2 & 32767)
 *   * 32768 + (Yaw / 2 & 32767), [OldTimeDelta, OldAccel]); DualServerMove sends two moves (the first's flags and
 *   both bJumpStatus in PendingCompress); ShortServerMove a move without acceleration, dodge or double jump.
 *   OldTimeDelta / OldAccel repeat the last jump / dodge move (500 per second, 0.05 * accel + 0.5 in 8 bits per
 *   axis, flags 64 run, 32 double jump, 16 jump, 0-7 dodge) in case its packet was lost.
 *   The server simulates each move (MoveAutonomous with DeltaTime = TimeStamp - previous TimeStamp, at most
 *   MaxResponseTime) and answers a ClientLoc more than sqrt(3) UU off with Client / Short / VeryShort /
 *   LongClientAdjustPosition(TimeStamp, state, physics, location, velocity, base). The client then resets the pawn
 *   and replays its newer saved moves (ClientUpdatePosition). 3369 has no ClientAckGoodMove: saved moves older than
 *   an adjustment or than 2 s are dropped.
 *   ServerMove is ignored until AcknowledgedPawn == Pawn: the client sends ServerAcknowledgePossession(Pawn,
 *   Handedness, bAutoTaunt) when its Pawn changes.
 */
namespace UE2NetPlayLocal
{
	FUE2NetPlay* GInstance = nullptr;
	constexpr float S = ArenaUE2World::Scale;
	/** See AUE2NetLocalPawn::OnMovementModeChanged. */
	constexpr float NetWalkExtraHeight = 4.78f;

	FUE2NetValue VFloat(float F) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Float; V.F = F; return V; }
	FUE2NetValue VInt(int32 I) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Int; V.I = I; return V; }
	FUE2NetValue VByte(int32 I) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Byte; V.I = I & 255; return V; }
	FUE2NetValue VBool(bool B) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Bool; V.I = B ? 1 : 0; return V; }
	FUE2NetValue VVec(const FVector& In) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Vector; V.V = FVector3f(In); return V; }
	FUE2NetValue VStr(const FString& In) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Str; V.S = In; return V; }
	FUE2NetValue VActor(int32 Channel) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Object; V.Obj.Channel = Channel; return V; }
	FUE2NetValue VNone() { return FUE2NetValue(); }

	bool ChainHas(const FUE2NetActor& A, const TCHAR* Name)
	{
		for (FUSClass* C = A.Class; C; C = C->GetSuperClass())
		{
			if (C->Name == Name)
			{
				return true;
			}
		}
		return false;
	}

	/** A class default as text (bytes as numbers, objects as their path). */
	FString DefaultText(FUSClass* Class, const TCHAR* Key)
	{
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FUSProperty* Prop = Def ? Def->FindProperty(FName(Key)) : nullptr;
		if (!Prop || !Def->Data)
		{
			return FString();
		}
		const uint8* Value = Def->Data + Prop->Offset;
		if (Prop->Type == EUSType::Byte)
		{
			return FString::FromInt(*Value);
		}
		FString Out;
		Prop->ExportText(Out, Value, true);
		return Out;
	}

	const FUE2NetValue* Prop(const FUE2NetActor& A, const TCHAR* Key) { return A.Properties.Find(Key); }

	float GetFloat(const FUE2NetActor& A, const TCHAR* Key, float Default)
	{
		if (const FUE2NetValue* V = Prop(A, Key))
		{
			return V->Kind == FUE2NetValue::EKind::Float ? V->F : float(V->I);
		}
		const FString D = DefaultText(A.Class, Key);
		return D.IsEmpty() ? Default : FCString::Atof(*D);
	}

	int32 GetInt(const FUE2NetActor& A, const TCHAR* Key, int32 Default)
	{
		if (const FUE2NetValue* V = Prop(A, Key))
		{
			return V->Kind == FUE2NetValue::EKind::Float ? int32(V->F) : V->I;
		}
		const FString D = DefaultText(A.Class, Key);
		return D.IsEmpty() ? Default : (D.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? 1 : FCString::Atoi(*D));
	}

	int32 GetRef(const FUE2NetActor& A, const TCHAR* Key)
	{
		const FUE2NetValue* V = Prop(A, Key);
		return V && V->Obj.IsDynamic() ? V->Obj.Channel : 0;
	}

	const FUE2NetValue* Param(const TArray<TPair<FName, FUE2NetValue>>& Params, const TCHAR* Name)
	{
		for (const TPair<FName, FUE2NetValue>& P : Params)
		{
			if (P.Key == Name)
			{
				return &P.Value;
			}
		}
		return nullptr;
	}

	float ParamFloat(const TArray<TPair<FName, FUE2NetValue>>& Params, const TCHAR* Name)
	{
		const FUE2NetValue* V = Param(Params, Name);
		return V ? (V->Kind == FUE2NetValue::EKind::Float ? V->F : float(V->I)) : 0.f;
	}

	/** UE2 rotation units (65536 per turn) of an angle in degrees, 0..65535. */
	int32 ToUU(double Degrees) { return int32(FMath::RoundToInt64(Degrees * 65536.0 / 360.0)) & 65535; }

	EMovementMode MovementFor(uint8 Physics)
	{
		switch (Physics)
		{
		case 1: return MOVE_Walking;
		case 2: return MOVE_Falling;
		case 3: return MOVE_Swimming;
		case 4: return MOVE_Flying;
		case 11: return MOVE_Custom; // PHYS_Ladder: UArenaCharacterMovement::CustomLadder
		default: return MOVE_None;
		}
	}

	uint8 PhysicsFor(EMovementMode Mode)
	{
		switch (Mode)
		{
		case MOVE_Walking: case MOVE_NavWalking: return 1;
		case MOVE_Falling: return 2;
		case MOVE_Swimming: return 3;
		case MOVE_Flying: return 4;
		default: return 0;
		}
	}

	/** UE2 PlayerController.CompressAccel. */
	int32 CompressAccel(int32 C) { return C >= 0 ? FMath::Min(C, 127) : FMath::Min(FMath::Abs(C), 127) + 128; }

	/** Combo classes of xPlayer.ServerDoCombo. */
	const TCHAR* ComboClass(EArenaCombo Combo)
	{
		switch (Combo)
		{
		case EArenaCombo::Speed: return TEXT("XGame.ComboSpeed");
		case EArenaCombo::Berserk: return TEXT("XGame.ComboBerserk");
		case EArenaCombo::Booster: return TEXT("XGame.ComboDefensive");
		case EArenaCombo::Invisible: return TEXT("XGame.ComboInvis");
		default: return nullptr;
		}
	}

	bool IsLocalAddress(const FString& Host)
	{
		return Host == TEXT("127.0.0.1") || Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase);
	}
}

// =================================================================================================
// AUE2NetLocalPawn
// =================================================================================================

bool UUE2NetBoxCollision::UseBox()
{
	static const bool bCapsule = FParse::Param(FCommandLine::Get(), TEXT("UE2NetCapsuleCollision"));
	return !bCapsule;
}

void UUE2NetBoxCollision::UpdateBodySetup()
{
	if (!UseBox())
	{
		Super::UpdateBodySetup();
		return;
	}
	if (!ShapeBodySetup || ShapeBodySetup->AggGeom.BoxElems.Num() != 1 || ShapeBodySetup->AggGeom.SphylElems.Num() != 0)
	{
		ShapeBodySetup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
		ShapeBodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		ShapeBodySetup->AggGeom.BoxElems.Add(FKBoxElem());
		ShapeBodySetup->bNeverNeedsCookedCollisionData = true;
		bUseArchetypeBodySetup = false;
		BodyInstance.BodySetup = ShapeBodySetup;
	}
	FKBoxElem& Box = ShapeBodySetup->AggGeom.BoxElems[0];
	Box.Center = FVector::ZeroVector;
	Box.X = 2.f * GetUnscaledCapsuleRadius();
	Box.Y = 2.f * GetUnscaledCapsuleRadius();
	Box.Z = 2.f * GetUnscaledCapsuleHalfHeight();
}

FCollisionShape UUE2NetBoxCollision::GetCollisionShape(float Inflation) const
{
	if (!UseBox())
	{
		return Super::GetCollisionShape(Inflation);
	}
	return FCollisionShape::MakeBox(FVector(GetScaledCapsuleRadius(), GetScaledCapsuleRadius(), GetScaledCapsuleHalfHeight())).Inflate(Inflation);
}

AUE2NetLocalPawn::AUE2NetLocalPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UUE2NetBoxCollision>(ACharacter::CapsuleComponentName))
{
	bNetProxy = true;
	// the body (and its collision box) keeps facing +X; the view turns
	bUseControllerRotationYaw = !UUE2NetBoxCollision::UseBox();
}

void AUE2NetLocalPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	AActor* Vehicle = DrivenVehicle.Get();
	if (!Vehicle)
	{
		Super::CalcCamera(DeltaTime, OutResult);
		return;
	}
	// UE2 Vehicle.SpecialCalcBehindView: behind and above the vehicle, along our own view rotation
	const FRotator View = Controller ? Controller->GetControlRotation() : GetActorRotation();
	const FBoxSphereBounds B = Vehicle->GetRootComponent() ? Vehicle->GetRootComponent()->Bounds : FBoxSphereBounds(Vehicle->GetActorLocation(), FVector(200.f), 200.f);
	const FVector Pivot = Vehicle->GetActorLocation() + FVector(0.f, 0.f, 120.f);
	FVector Desired = Pivot - View.Vector() * FMath::Max(600.f, float(B.SphereRadius) * 2.2f);
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetDriveCamera), false, this);
	Q.AddIgnoredActor(Vehicle);
	if (GetWorld()->SweepSingleByChannel(Hit, Pivot, Desired, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(12.f), Q))
	{
		Desired = Hit.Location;
	}
	OutResult.Location = Desired;
	OutResult.Rotation = View;
}

float AUE2NetLocalPawn::GetMovementYaw() const
{
	return Controller ? float(Controller->GetControlRotation().Yaw) : Super::GetMovementYaw();
}

void AUE2NetLocalPawn::PossessedBy(AController* NewController)
{
	// ACharacter's possession only: the inventory comes from the server, not AArenaCharacter's default weapons
	ACharacter::PossessedBy(NewController);
	SetupSkeletalBody();
	UpdateTeamColors();
}

void AUE2NetLocalPawn::ActivateCombo(EArenaCombo Combo)
{
	if (bSettingNetCombo || Combo == EArenaCombo::None)
	{
		Super::ActivateCombo(Combo);
		return;
	}
	using namespace UE2NetPlayLocal;
	const TCHAR* Class = ComboClass(Combo);
	FUE2NetPlay* Play = FUE2NetPlay::Get();
	if (!Class || !Play)
	{
		return;
	}
	FUSClass* C = FUSRuntime::Get().FindClass(Class);
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: combo %s requested (xPlayer.ServerDoCombo)%s"), Class, C ? TEXT("") : TEXT(" - class not found"));
	if (C)
	{
		Play->DoCombo(C);
	}
}

void AUE2NetLocalPawn::SetNetCombo(EArenaCombo Combo)
{
	if (GetActiveCombo() == Combo)
	{
		return;
	}
	bSettingNetCombo = true;
	Super::ActivateCombo(Combo);
	bSettingNetCombo = false;
}

void AUE2NetLocalPawn::ApplyNetCollision()
{
	using namespace UE2NetPlayLocal;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (!Cap || !CM)
	{
		return;
	}
	const bool bWalking = CM->IsMovingOnGround();
	const float Extra = bWalking ? NetWalkExtraHeight : 0.f;
	const float Old = Cap->GetUnscaledCapsuleHalfHeight();
	const float Base = bIsCrouched ? 29.f : 44.f;
	const float New = (Base + Extra) * S;
	CM->SetCrouchedHalfHeight((29.f + Extra) * S);
	if (FMath::IsNearlyEqual(Old, New, 0.01f))
	{
		return;
	}
	Cap->SetCapsuleSize(25.f * S, New);
	if (New > Old && !bNoLandingLift)
	{
		// landed: the server's walking pawn rises above the point of contact
		AddActorWorldOffset(FVector(0.f, 0.f, New - Old), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AUE2NetLocalPawn::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	ApplyNetCollision();
}

void AUE2NetLocalPawn::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);
	ApplyNetCollision();
}

AArenaWeapon* AUE2NetLocalPawn::EnsureNetWeapon(UClass* WeaponClass, const FString& UE2Class)
{
	if (!WeaponClass || !WeaponClass->IsChildOf(AArenaWeapon::StaticClass()))
	{
		return nullptr;
	}
	// one weapon per server class: a mod weapon and the stock one it stands in for are two
	const UE2NetModWeapons::FModWeapon* Mod = UE2NetModWeapons::FindWeapon(UE2Class);
	const FString Key = Mod ? UE2Class : FString();
	for (AArenaWeapon* Have : Weapons)
	{
		if (Have && Have->GetClass() == WeaponClass && Have->GetNetStandIn().UE2Class.Equals(Key, ESearchCase::IgnoreCase))
		{
			return Have;
		}
	}
	FActorSpawnParameters P;
	P.Owner = this;
	P.Instigator = this;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaWeapon* W = GetWorld()->SpawnActor<AArenaWeapon>(WeaponClass, GetActorTransform(), P);
	if (W)
	{
		if (Mod)
		{
			FArenaNetStandIn In;
			In.UE2Class = UE2Class;
			In.Art = &Mod->Art;
			In.View = Mod->bView ? &Mod->View : nullptr;
			In.Skins = UE2NetModWeapons::LoadSkins(Mod->Skins);
			In.FireRate[0] = Mod->FireRate[0];
			In.FireRate[1] = Mod->FireRate[1];
			In.bIcon = Mod->bIcon;
			In.IconCoords = Mod->IconCoords;
			In.IconTexture = Mod->IconTexture;
			In.FlashEmitterClass[0] = Mod->FlashEmitterClass[0];
			In.FlashEmitterClass[1] = Mod->FlashEmitterClass[1];
			for (const FString& Flash : In.FlashEmitterClass)
			{
				FString Pkg;
				if (Flash.Split(TEXT("."), &Pkg, nullptr) && !UE2NetModWeapons::IsStockClass(Flash))
				{
					ArenaUE2Effects::AddPackageFile(UE2NetModContent::PackageFile(Pkg)); // its emitters are read from the mod's package
				}
			}
			In.SelectSound = Mod->SelectSound.IsEmpty() ? nullptr : UE2NetModContent::LoadSound(Mod->SelectSound);
			W->SetNetStandIn(In);
		}
		W->GiveTo(this);
		Weapons.Add(W);
	}
	return W;
}

// =================================================================================================
// FUE2NetPlay
// =================================================================================================

USceneComponent* FUE2NetPlay::GetScriptWeaponComponent(int32 WeaponChannel) const
{
	const AUE2NetLocalPawn* P = LocalPawn.Get();
	if (!P)
	{
		return nullptr;
	}
	for (AArenaWeapon* W : P->GetWeapons())
	{
		if (const AWeap_UE2Script* SW = Cast<AWeap_UE2Script>(W); SW && SW->GetScriptChannel() == WeaponChannel)
		{
			return SW->GetViewMesh();
		}
	}
	return nullptr;
}

FString FUE2NetPlay::GetServerAddress() const
{
	return Connection ? Connection->GetAddressString() : FString();
}

FUE2NetPlay* FUE2NetPlay::Get()
{
	return UE2NetPlayLocal::GInstance;
}

FUE2NetPlay::FUE2NetPlay(FUE2NetConnection& InConnection, UWorld* InWorld)
	: Connection(&InConnection), World(InWorld)
{
	UE2NetPlayLocal::GInstance = this;
	NewNet = MakeUnique<FUE2NetNewNet>(InConnection);
	ModClient = MakeUnique<FUE2NetModClient>(InConnection, InWorld);
	StartTime = FPlatformTime::Seconds();
	NextSummary = StartTime + 30.0;
	PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FUE2NetPlay::PostActorTick);
	PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FUE2NetPlay::PreActorTick);
	// UT2004 runs at Level.TimeDilation 1.1 (GameInfo.SetGameSpeed): our world seconds are the server's game seconds,
	// so moves, timestamps and the replicated velocities line up
	if (InWorld && InWorld->GetWorldSettings())
	{
		InWorld->GetWorldSettings()->SetTimeDilation(1.1f);
		// UE2 ReplicateMove: a move is at most MaxResponseTime (0.125 game seconds), and the server simulates no more:
		// a longer frame (a hitch) would put us ahead of the server, so the world's frames are capped the same way
		InWorld->GetWorldSettings()->MaxUndilatedFrameTime = 0.125f / 1.1f;
	}
	FString ScriptText;
	if (FParse::Value(FCommandLine::Get(), TEXT("UE2NetPlayScript="), ScriptText, false))
	{
		TArray<FString> Steps;
		ScriptText.ParseIntoArray(Steps, TEXT(";"));
		for (const FString& Step : Steps)
		{
			FString T, Cmd;
			if (Step.Split(TEXT(":"), &T, &Cmd))
			{
				Script.Add({ StartTime + FCString::Atod(*T), Cmd.TrimStartAndEnd() });
			}
		}
	}
	// test chat only on a server on this machine (never on public servers)
	bScriptCanChat = UE2NetPlayLocal::IsLocalAddress(InConnection.GetSettings().Host);
	if (Script.Num() > 0)
	{
		CorrectionLogBudget = 1000000; // test runs: every correction in the log
	}
#if !UE_BUILD_SHIPPING
	bProbe = FParse::Param(FCommandLine::Get(), TEXT("UE2NetPlayProbe")) && bScriptCanChat;
	bNoNewNet = FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoNewNet"));
#endif
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: %s on %s (%d script steps)"), InConnection.GetSettings().bSpectator ? TEXT("spectating") : TEXT("playing"),
		*InConnection.GetAddressString(), Script.Num());
}

FUE2NetPlay::~FUE2NetPlay()
{
	LogSummary();
	FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
	FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
	if (AUE2NetLocalPawn* P = LocalPawn.Get())
	{
		P->Destroy();
	}
	if (UE2NetPlayLocal::GInstance == this)
	{
		UE2NetPlayLocal::GInstance = nullptr;
	}
}

int32 FUE2NetPlay::PCChannel() const
{
	return Connection->FindPlayerControllerChannel();
}

AArenaPlayerController* FUE2NetPlay::GetPC() const
{
	UWorld* W = World.Get();
	return W ? Cast<AArenaPlayerController>(W->GetFirstPlayerController()) : nullptr;
}

bool FUE2NetPlay::Send(int32 Channel, const TCHAR* Function, const TArray<FUE2NetValue>& Params)
{
	return Channel && Connection->SendFunction(Channel, FName(Function), Params);
}

bool FUE2NetPlay::IsPlaying() const
{
	return !Connection->GetSettings().bSpectator;
}

bool FUE2NetPlay::IsWaitingForRound() const
{
	if (!IsPlaying() || LocalPawn.IsValid() || DrivingChannel)
	{
		return false;
	}
	if (ServerState == TEXT("Spectating"))
	{
		return true;
	}
	const FUE2NetActor* PRI = OwnPRIChannel ? Connection->GetActors().Find(OwnPRIChannel) : nullptr;
	return PRI && UE2NetPlayLocal::GetInt(*PRI, TEXT("bOutOfLives"), 0) != 0;
}

bool FUE2NetPlay::IsHeldAsSpectator() const
{
	if (!IsPlaying() || LocalPawn.IsValid() || DrivingChannel)
	{
		return false;
	}
	const FUE2NetActor* PRI = OwnPRIChannel ? Connection->GetActors().Find(OwnPRIChannel) : nullptr;
	return PRI && UE2NetPlayLocal::GetInt(*PRI, TEXT("bOnlySpectator"), 0) != 0;
}

int32 FUE2NetPlay::ViewInt(const FRotator& View) const
{
	using namespace UE2NetPlayLocal;
	const int32 Pitch = ToUU(View.Pitch);
	const int32 Yaw = ToUU(View.Yaw);
	return (32767 & (Pitch / 2)) * 32768 + (32767 & (Yaw / 2));
}

// -------------------------------------------------------------------------------------------------
// Replicated actors
// -------------------------------------------------------------------------------------------------

void FUE2NetPlay::ActorOpened(const FUE2NetActor& A)
{
	TArray<FString> All;
	A.Properties.GetKeys(All);
	bInActorOpen = true;
	ActorUpdated(A, All);
	bInActorOpen = false;
}

void FUE2NetPlay::ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed)
{
	using namespace UE2NetPlayLocal;
	if (A.bBroken || !A.Class)
	{
		return;
	}
	NewNet->ActorUpdated(A, Changed, World.IsValid() ? World->GetTimeSeconds() : 0.0);
	ModClient->ActorUpdated(A, Changed, bInActorOpen, OwnPRIChannel);
	if (A.Channel == PCChannel() && Changed.Contains(TEXT("PlayerReplicationInfo")))
	{
		OwnPRIChannel = GetRef(A, TEXT("PlayerReplicationInfo"));
		if (const FUE2NetActor* PRI = OwnPRIChannel ? Connection->GetActors().Find(OwnPRIChannel) : nullptr)
		{
			ApplyOwnName(*PRI);
		}
	}
	if (A.Channel != 0 && A.Channel == OwnPRIChannel && Changed.Contains(TEXT("PlayerName")))
	{
		ApplyOwnName(A);
		return;
	}
	if (A.Channel == PCChannel())
	{
		if (Changed.Contains(TEXT("Pawn")))
		{
			const int32 PawnCh = GetRef(A, TEXT("Pawn"));
			if (PawnCh && IsVehicleChannel(PawnCh))
			{
				EnterVehicle(PawnCh);
			}
			else if (PawnCh && DrivingChannel && PawnCh == OwnPawnChannel)
			{
				ExitVehicle(true);
			}
			else if (PawnCh)
			{
				SetOwnPawnChannel(PawnCh, TEXT("Controller.Pawn"));
			}
		}
		if (Changed.Contains(TEXT("Adrenaline")))
		{
			if (AUE2NetLocalPawn* P = LocalPawn.Get())
			{
				P->Adrenaline = GetFloat(A, TEXT("Adrenaline"), 0.f);
			}
		}
		return;
	}
	if (ChainHas(A, TEXT("Weapon")) && !ChainHas(A, TEXT("WeaponAttachment")))
	{
		// weapons of other pawns (the view target's at the end of a match) are not ours
		if (const int32 Inst = GetRef(A, TEXT("Instigator")); Inst && OwnPawnChannel && Inst != OwnPawnChannel && !Weapons.Contains(A.Channel))
		{
			return;
		}
		// weapons are replicated to their owner only: this is our inventory
		FWeaponInfo& W = Weapons.FindOrAdd(A.Channel);
		if (W.Channel == 0)
		{
			W.Channel = A.Channel;
			W.ClassPath = A.ClassPath;
			W.Native = UE2NetActors::WeaponClassFor(A.ClassPath);
			W.Group = GetInt(A, TEXT("InventoryGroup"), 0);
			W.Priority = GetFloat(A, TEXT("Priority"), 0.f);
			if (W.Native && W.Native->IsChildOf(AWeap_Translocator::StaticClass()) && !Prop(A, TEXT("RepAmmo")))
			{
				// TransLauncher.RepAmmo comes when the charges change: a new launcher is full (AmmoChargeMax)
				W.Ammo = FMath::FloorToInt32(ArenaTranslocator::AmmoChargeMax);
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: inventory + %s (group %d, ours %s)"), *A.ClassPath, W.Group, W.Native ? *W.Native->GetName() : TEXT("none"));
		}
		for (const TCHAR* Key : { TEXT("Ammo"), TEXT("Ammo[0]") })
		{
			if (const FUE2NetValue* Ammo0 = Prop(A, Key); Ammo0 && Ammo0->Obj.IsDynamic())
			{
				W.AmmoChannel = Ammo0->Obj.Channel;
			}
		}
		// TransLauncher.RepAmmo: its charges (AmmoChargeF) as the owner sees them
		for (const TCHAR* Key : { TEXT("AmmoCharge"), TEXT("AmmoCharge[0]"), TEXT("RepAmmo") })
		{
			if (Prop(A, Key))
			{
				W.Ammo = GetInt(A, Key, W.Ammo);
			}
		}
		ApplyWeapons();
		return;
	}
	if (ChainHas(A, TEXT("Ammunition")))
	{
		for (TPair<int32, FWeaponInfo>& W : Weapons)
		{
			if (W.Value.AmmoChannel == A.Channel)
			{
				W.Value.Ammo = GetInt(A, TEXT("AmmoAmount"), W.Value.Ammo);
			}
		}
		ApplyWeapons();
		return;
	}
	if (ChainHas(A, TEXT("Pawn")) && !ChainHas(A, TEXT("Vehicle")))
	{
		// our own pawn: the server's RemoteRole for it is AutonomousProxy (the value arrives in Role, see Docs)
		const bool bAutonomous = GetInt(A, TEXT("Role"), 0) == 3 || GetInt(A, TEXT("RemoteRole"), 0) == 3;
		if (bAutonomous && !OwnPawnChannel && !IsPlaying())
		{
			return;
		}
		if (bAutonomous && A.Channel != OwnPawnChannel && A.Channel != DeadPawnChannel && IsPlaying())
		{
			SetOwnPawnChannel(A.Channel, TEXT("autonomous pawn"));
		}
		if (A.Channel == OwnPawnChannel)
		{
			ApplyOwnPawn(A, &Changed);
		}
		return;
	}
}

void FUE2NetPlay::ActorClosed(const FUE2NetActor& A)
{
	NewNet->ActorClosed(A);
	if (A.Channel != 0 && A.Channel == DrivingChannel)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the vehicle we drove is gone (channel %d)"), A.Channel);
		ExitVehicle(true);
	}
	if (A.Channel == DeadPawnChannel)
	{
		DeadPawnChannel = 0; // the index may be reused for our next pawn
	}
	if (A.Channel != 0 && A.Channel == WarheadChannel)
	{
		EndWarhead();
	}
	if (A.Channel == OwnPawnChannel)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: our pawn's channel %d closed"), A.Channel);
		DestroyLocalPawn(false);
		OwnPawnChannel = 0;
		AckedPawnChannel = 0;
	}
	if (Weapons.Remove(A.Channel))
	{
		if (CurrentWeaponChannel == A.Channel)
		{
			CurrentWeaponChannel = 0;
		}
	}
}

void FUE2NetPlay::ApplyOwnName(const FUE2NetActor& PRI)
{
	// the server may rename us (a duplicate name becomes "Player3"): kill lines, scoreboard and HUD use its name
	const FUE2NetValue* Name = UE2NetPlayLocal::Prop(PRI, TEXT("PlayerName"));
	AArenaPlayerController* PC = GetPC();
	APlayerState* PS = PC ? PC->PlayerState : nullptr;
	if (Name && !Name->S.IsEmpty() && PS && PS->GetPlayerName() != Name->S)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the server calls us \"%s\""), *Name->S);
		PS->SetPlayerName(Name->S);
	}
}

void FUE2NetPlay::SetOwnPawnChannel(int32 Channel, const TCHAR* Why)
{
	if (Channel == OwnPawnChannel || Channel == DeadPawnChannel)
	{
		return;
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: our pawn is channel %d (%s)"), Channel, Why);
	if (LocalPawn.IsValid())
	{
		DestroyLocalPawn(false);
	}
	if (WarheadChannel && Channel != WarheadChannel)
	{
		EndWarhead(); // RedeemerWarhead.RelinquishController: back to our pawn
	}
	OwnPawnChannel = Channel;
	// the body the world layer showed for this pawn while it was not ours (a guided Redeemer warhead was): we are it again
	FUE2NetWorld* NW = FUE2NetWorld::Get();
	const FUE2NetActor* A = Connection->GetActors().Find(Channel);
	if (NW && A && !DrivingChannel && NW->GetNativeActor(Channel))
	{
		NW->ActorClosed(*A);
	}
	TrySpawnLocalPawn();
}

void FUE2NetPlay::TrySpawnLocalPawn()
{
	using namespace UE2NetPlayLocal;
	ArenaHitchLog::FScopedNote HitchNote(TEXT("our pawn spawned"));
	UWorld* W = World.Get();
	AArenaPlayerController* PC = GetPC();
	const FUE2NetActor* A = OwnPawnChannel ? Connection->GetActors().Find(OwnPawnChannel) : nullptr;
	if (!W || !PC || !A || LocalPawn.IsValid())
	{
		return;
	}
	if (ChainHas(*A, TEXT("RedeemerWarhead")))
	{
		// RedeemerGuidedFire: the controller possesses the warhead (PlayerRocketing), not a walking pawn
		if (WarheadChannel != OwnPawnChannel)
		{
			StartWarhead(*A);
		}
		return;
	}
	FVector Loc = FVector(A->SpawnLocation);
	if (const FUE2NetValue* L = Prop(*A, TEXT("Location")))
	{
		Loc = FVector(L->V);
	}
	Loc.Z -= ZOffset;
	int32 Yaw = A->SpawnRotation.Y;
	if (const FUE2NetValue* R = Prop(*A, TEXT("Rotation")))
	{
		Yaw = R->R.Y;
	}
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.ObjectFlags |= RF_Transient;
	const FRotator Rot(0.f, Yaw * 360.f / 65536.f, 0.f);
	AUE2NetLocalPawn* Pawn = W->SpawnActor<AUE2NetLocalPawn>(AUE2NetLocalPawn::StaticClass(), Loc * S,
		UUE2NetBoxCollision::UseBox() ? FRotator::ZeroRotator : Rot, P);
	if (!Pawn)
	{
		UE_LOG(LogUE2Net, Warning, TEXT("UE2NetPlay: could not spawn our pawn at %s"), *(Loc * S).ToCompactString());
		return;
	}
	LocalPawn = Pawn;
	Pawn->ApplyNetCollision();
	// UE2 physWalking computes the velocity once per move (calcVelocity over the whole DeltaTime) and only splits the
	// movement; the engine's substeps (0.05 s) would accelerate and brake in smaller steps than the server
	Pawn->GetCharacterMovement()->MaxSimulationTimeStep = 0.15f;
	if (UArenaCharacterMovement* ACM = Cast<UArenaCharacterMovement>(Pawn->GetCharacterMovement()))
	{
		ACM->bUE2ServerBraking = true;
		// UE2 MoveActor's 2 UU back-off from what the pawn hits (test hook -UE2NetNoBackoff: without, for A/B runs)
		ACM->bUE2MoveBackoff = !FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoBackoff"));
		ACM->LadderSpeed = 200.f * S; // Pawn.LadderSpeed
	}
	if (AArenaPlayerState* PS = PC->GetPlayerState<AArenaPlayerState>())
	{
		// the local login spectates (?SpectatorOnly=1); playing on the server, our player state plays too
		PS->SetIsOnlyASpectator(false);
		PS->SetIsSpectator(false);
	}
	PC->Possess(Pawn);
	PC->SetControlRotation(Rot);
	PC->SetViewTarget(Pawn);
	AUE2NetPawn::bBehindView = false;
	SavedMoves.Reset();
	LastAdjustTimeStamp = -1.f;
	++Spawns;
	LastSpawnRealTime = FPlatformTime::Seconds();
	// UE2 PlayerController.AcknowledgePossession: the server ignores our moves until it has this
	// Handedness: the settings' weapon hand (0 left .. 3 hidden) minus one, like UT2004's PlayerController.Handedness
	const float Handedness = float(FMath::Clamp(UArenaUserSettings::Get()->WeaponHand, 0, 3) - 1);
	Send(PCChannel(), TEXT("ServerAcknowledgePossession"), { VActor(OwnPawnChannel), VFloat(Handedness), VBool(false) });
	AckedPawnChannel = OwnPawnChannel;
	ApplyOwnPawn(*A, nullptr);
	ApplyWeapons();
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: spawned at %s (UU %s), yaw %d; ServerAcknowledgePossession sent"), *(Loc * S).ToCompactString(), *Loc.ToCompactString(), Yaw);
}

void FUE2NetPlay::DestroyLocalPawn(bool bDied)
{
	ArenaHitchLog::FScopedNote HitchNote(bDied ? TEXT("our pawn died (death camera body)") : TEXT("our pawn removed"));
	using namespace UE2NetPlayLocal;
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	LocalPawn.Reset();
	SavedMoves.Reset();
	for (bool& B : bFireHeld)
	{
		B = false;
	}
	if (!Pawn)
	{
		return;
	}
	AArenaPlayerController* PC = GetPC();
	UWorld* W = World.Get();
	if (bDied && W)
	{
		// the death camera: a body that ragdolls (or gibs) where we died, seen from behind (UE2 PlayerController.Dead)
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		AUE2NetPawn* Corpse = W->SpawnActor<AUE2NetPawn>(AUE2NetPawn::StaticClass(), Pawn->GetActorTransform(), P);
		if (Corpse)
		{
			const FUE2NetActor* A = Connection->GetActors().Find(OwnPawnChannel);
			FUE2NetWorld* NW = FUE2NetWorld::Get();
			const FUE2NetActor* PRI = NW ? Connection->GetActors().Find(NW->GetOwnPRIChannel()) : nullptr;
			if (PRI)
			{
				const FUE2NetValue* Char = Prop(*PRI, TEXT("CharacterName"));
				Corpse->SetCharacter(Char ? Char->S : FString(), GetInt(*PRI, TEXT("PlayerID"), 0));
			}
			const FVector LocUU = Pawn->GetActorLocation() / S;
			Corpse->SetNetPosition(LocUU, Pawn->GetVelocity() / S, ToUU(Pawn->GetActorRotation().Yaw), true);
			const FVector Momentum = A && Prop(*A, TEXT("TearOffMomentum")) ? FVector(Prop(*A, TEXT("TearOffMomentum"))->V) : FVector::ZeroVector;
			const FVector HitLoc = A && Prop(*A, TEXT("TakeHitLocation")) ? FVector(Prop(*A, TEXT("TakeHitLocation"))->V) : LocUU;
			// xPawn.PlayDying with the server's values: its TearOffMomentum, hit location, and gibs for a big overkill or a
			// damage type with bAlwaysGibs (HitDamageType)
			bool bGib = A && GetInt(*A, TEXT("Health"), 0) <= -60;
			if (const FUE2NetValue* Dam = A ? Prop(*A, TEXT("HitDamageType")) : nullptr; Dam && Dam->Obj.IsStatic())
			{
				FUSClass* DamClass = Connection->GetPackageMap().GetClass(Dam->Obj);
				bGib |= DefaultText(DamClass, TEXT("bAlwaysGibs")).Equals(TEXT("True"), ESearchCase::IgnoreCase);
			}
			Corpse->NetDie(Momentum, HitLoc, bGib);
			Corpse->SetLifeSpan(10.f);
			if (PC)
			{
				PC->UnPossess();
				AUE2NetPawn::bBehindView = true;
				PC->SetViewTarget(Corpse);
			}
		}
	}
	else if (PC && PC->GetPawn() == Pawn)
	{
		PC->UnPossess();
	}
	Pawn->Destroy();
}

void FUE2NetPlay::ApplyOwnPawn(const FUE2NetActor& A, const TArray<FString>* Changed)
{
	using namespace UE2NetPlayLocal;
	auto Changes = [Changed](const TCHAR* Key) { return !Changed || Changed->Contains(Key); };
	if (A.Channel == WarheadChannel)
	{
		// RedeemerWarhead.BlowUp: bStaticScreen until the server gives us our pawn back
		if (AArenaGuidedWarhead* WH = NetWarhead.Get(); WH && Changes(TEXT("bStaticScreen")) && GetInt(A, TEXT("bStaticScreen"), 0) != 0)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the warhead blew up (%d moves sent, %d corrections)"), RocketMovesSent, WarheadAdjustments);
			WH->NetBlownUp();
		}
		return;
	}
	if (Changes(TEXT("bTearOff")) && GetInt(A, TEXT("bTearOff"), 0) != 0 && DeadPawnChannel != A.Channel)
	{
		DeadPawnChannel = A.Channel;
		++Deaths;
		if (DrivingChannel)
		{
			// killed in a vehicle: the world layer has our body (it was seated); watch it
			DrivingChannel = 0;
			AUE2NetLocalPawn* P = LocalPawn.Get();
			AArenaPlayerController* PC = GetPC();
			if (P && PC)
			{
				PC->UnPossess();
				P->Destroy();
				LocalPawn.Reset();
				if (FUE2NetWorld* NW = FUE2NetWorld::Get())
				{
					if (AActor* Body = NW->GetNativeActor(A.Channel))
					{
						AUE2NetPawn::bBehindView = true;
						PC->SetViewTarget(Body);
					}
				}
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: we died in a vehicle"));
			OwnPawnChannel = 0;
			AckedPawnChannel = 0;
			return;
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: we died (health %d)"), GetInt(A, TEXT("Health"), 0));
		DestroyLocalPawn(true);
		OwnPawnChannel = 0;
		AckedPawnChannel = 0;
		return;
	}
	AUE2NetLocalPawn* P = LocalPawn.Get();
	if (!P)
	{
		TrySpawnLocalPawn();
		return;
	}
	if (Changes(TEXT("Health")))
	{
		P->Health = float(GetInt(A, TEXT("Health"), 100));
	}
	if (Changes(TEXT("ShieldStrength")))
	{
		P->Shield = GetFloat(A, TEXT("ShieldStrength"), 0.f);
	}
	if (Changes(TEXT("SmallShieldStrength")))
	{
		P->SmallShield = GetFloat(A, TEXT("SmallShieldStrength"), 0.f);
	}
	UArenaCharacterMovement* CM = Cast<UArenaCharacterMovement>(P->GetCharacterMovement());
	if (CM)
	{
		// the movement values the server may change (speed combo, mutators): UE2 replicates them to the owner
		if (Changes(TEXT("GroundSpeed")))
		{
			CM->MaxWalkSpeed = GetFloat(A, TEXT("GroundSpeed"), 440.f) * S;
			if (Changed)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the server sets our GroundSpeed to %.0f"), GetFloat(A, TEXT("GroundSpeed"), 440.f));
			}
		}
		if (Changes(TEXT("WaterSpeed")))
		{
			CM->MaxSwimSpeed = GetFloat(A, TEXT("WaterSpeed"), 220.f) * S;
		}
		if (Changes(TEXT("JumpZ")))
		{
			CM->JumpZVelocity = GetFloat(A, TEXT("JumpZ"), 340.f) * S;
		}
		if (Changes(TEXT("AirControl")))
		{
			CM->AirControl = GetFloat(A, TEXT("AirControl"), 0.35f);
		}
		if (Changes(TEXT("AccelRate")))
		{
			CM->MaxAcceleration = GetFloat(A, TEXT("AccelRate"), 2048.f) * S;
		}
	}
	if (Changes(TEXT("Weapon")))
	{
		const int32 W = GetRef(A, TEXT("Weapon"));
		if (W && W != CurrentWeaponChannel)
		{
			CurrentWeaponChannel = W;
			ApplyWeapons();
		}
	}
}

void FUE2NetPlay::ApplyWeapons()
{
	AUE2NetLocalPawn* P = LocalPawn.Get();
	if (!P)
	{
		return;
	}
	FUE2NetWorld* NW = FUE2NetWorld::Get();
	FUE2NetScriptHUD* ScriptHud = FUE2NetScriptHUD::Get();
	for (TPair<int32, FWeaponInfo>& W : Weapons)
	{
		if (!W.Value.Native && ScriptHud && ScriptHud->IsScriptWeapon(W.Key))
		{
			// no stock relative: the mod's own script runs it (Net/UE2NetScriptWeapon.h)
			const UE2NetModWeapons::FModWeapon* Mod = UE2NetModWeapons::FindWeapon(W.Value.ClassPath);
			W.Value.Native = Mod && Mod->Native == AWeap_UE2Script::StaticClass() ? Mod->Native : nullptr;
			UE_CLOG(W.Value.Native != nullptr, LogUE2Net, Log, TEXT("UE2NetPlay: %s runs as the mod's script"), *W.Value.ClassPath);
		}
		if (AArenaWeapon* Native = P->EnsureNetWeapon(W.Value.Native, W.Value.ClassPath))
		{
			if (AWeap_UE2Script* SW = Cast<AWeap_UE2Script>(Native))
			{
				SW->SetScriptChannel(W.Key);
				SW->Slot = W.Value.Group;
			}
			Native->Ammo = W.Value.Ammo;
			// what the weapon shows of the server's weapon: the AVRiL's lock, the translocator's beacon
			AArenaWeapon::FNetWeaponState& NS = Native->NetState;
			NS.bValid = true;
			if (const FUE2NetActor* WA = Connection->GetActors().Find(W.Key))
			{
				NS.bLockedOn = UE2NetPlayLocal::GetInt(*WA, TEXT("bLockedOn"), 0) != 0;
				const int32 Target = UE2NetPlayLocal::GetRef(*WA, TEXT("HomingTarget"));
				NS.HomingTarget = NW && Target ? NW->GetNativeActor(Target) : nullptr;
				NS.bBeaconOut = UE2NetPlayLocal::GetRef(*WA, TEXT("TransBeacon")) != 0;
			}
		}
	}
	if (const FWeaponInfo* Cur = Weapons.Find(CurrentWeaponChannel))
	{
		AArenaWeapon* Native = Cur->Native ? P->EnsureNetWeapon(Cur->Native, Cur->ClassPath) : nullptr;
		if (Native && P->GetCurrentWeapon() != Native)
		{
			P->SwitchToWeapon(Native);
		}
	}
}

void FUE2NetPlay::SwitchToWeapon(int32 WeaponChannel)
{
	const int32 PawnCh = OwnPawnChannel;
	if (!PawnCh || !LocalPawn.IsValid() || !Weapons.Contains(WeaponChannel) || WeaponChannel == CurrentWeaponChannel)
	{
		return;
	}
	// UE2 Pawn.ChangedWeapon on a client: ServerChangedWeapon(Weapon, PendingWeapon), then the new weapon comes up
	Send(PawnCh, TEXT("ServerChangedWeapon"), { CurrentWeaponChannel ? UE2NetPlayLocal::VActor(CurrentWeaponChannel) : UE2NetPlayLocal::VNone(),
		UE2NetPlayLocal::VActor(WeaponChannel) });
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: weapon %s -> %s (ServerChangedWeapon)"), Weapons.Contains(CurrentWeaponChannel) ? *Weapons[CurrentWeaponChannel].ClassPath : TEXT("none"),
		*Weapons[WeaponChannel].ClassPath);
	CurrentWeaponChannel = WeaponChannel;
	++WeaponSwitches;
	ApplyWeapons();
}

void FUE2NetPlay::CycleWeapon(int32 Dir)
{
	// UE2 Inventory.NextWeapon / PrevWeapon: by InventoryGroup, then priority
	TArray<const FWeaponInfo*> List;
	for (const TPair<int32, FWeaponInfo>& W : Weapons)
	{
		List.Add(&W.Value);
	}
	if (List.Num() == 0)
	{
		return;
	}
	List.Sort([](const FWeaponInfo& L, const FWeaponInfo& R) { return L.Group != R.Group ? L.Group < R.Group : L.Priority < R.Priority; });
	int32 Index = List.IndexOfByPredicate([this](const FWeaponInfo* W) { return W->Channel == CurrentWeaponChannel; });
	Index = Index == INDEX_NONE ? 0 : (Index + Dir + List.Num()) % List.Num();
	SwitchToWeapon(List[Index]->Channel);
}

void FUE2NetPlay::SelectGroup(int32 Group)
{
	const FWeaponInfo* Best = nullptr;
	const FWeaponInfo* Current = Weapons.Find(CurrentWeaponChannel);
	TArray<const FWeaponInfo*> InGroup;
	for (const TPair<int32, FWeaponInfo>& W : Weapons)
	{
		if (W.Value.Group == Group)
		{
			InGroup.Add(&W.Value);
		}
	}
	if (InGroup.Num() == 0)
	{
		return;
	}
	InGroup.Sort([](const FWeaponInfo& L, const FWeaponInfo& R) { return L.Priority > R.Priority; });
	Best = InGroup[0];
	if (Current && Current->Group == Group && InGroup.Num() > 1)
	{
		const int32 I = InGroup.IndexOfByKey(Current);
		Best = InGroup[(I + 1) % InGroup.Num()];
	}
	SwitchToWeapon(Best->Channel);
}

// -------------------------------------------------------------------------------------------------
// Function calls from the server
// -------------------------------------------------------------------------------------------------

bool FUE2NetPlay::FunctionCall(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	using namespace UE2NetPlayLocal;
	if (!Function)
	{
		return false;
	}
	const FName Name = Function->Name;
	if (NewNet->FunctionCall(A, Function, World.IsValid() ? World->GetTimeSeconds() : 0.0) || ModClient->FunctionCall(A, Function, Params, OwnPRIChannel))
	{
		return true;
	}
	if (A.Channel == PCChannel())
	{
		if (Name == TEXT("ClientCapBandwidth"))
		{
			// PlayerController.ClientCapBandwidth: the server's MaxClientRate caps our net speed (move cadence)
			if (const FUE2NetValue* Cap = Param(Params, TEXT("Cap")))
			{
				ClientCap = Cap->I;
			}
		}
		if (Name == TEXT("ClientAdjustPosition") || Name == TEXT("LongClientAdjustPosition"))
		{
			ClientAdjust(Params, true, false);
			return true;
		}
		if (Name == TEXT("ShortClientAdjustPosition"))
		{
			ClientAdjust(Params, false, false);
			return true;
		}
		if (Name == TEXT("VeryShortClientAdjustPosition"))
		{
			ClientAdjust(Params, false, true);
			return true;
		}
		if (Name == TEXT("ClientRestart") || Name == TEXT("ClientReStart") || Name == TEXT("GivePawn"))
		{
			const FUE2NetValue* P = Param(Params, TEXT("NewPawn"));
			if (P && P->Obj.IsDynamic())
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: %s(%d)"), *Name.ToString(), P->Obj.Channel);
				if (IsVehicleChannel(P->Obj.Channel))
				{
					EnterVehicle(P->Obj.Channel);
				}
				else if (DrivingChannel && P->Obj.Channel == OwnPawnChannel)
				{
					ExitVehicle(true);
				}
				else
				{
					SetOwnPawnChannel(P->Obj.Channel, *Name.ToString());
				}
			}
			ServerState = NAME_None;
			return false; // the world layer also resets its match state on ClientRestart
		}
		if (Name == TEXT("ClientSwitchToBestWeapon"))
		{
			// Controller.ClientSwitchToBestWeapon (the painter after its strike, its last charge going): the best other
			// weapon that has ammo
			const FWeaponInfo* Best = nullptr;
			for (const TPair<int32, FWeaponInfo>& W : Weapons)
			{
				const AArenaWeapon* Def = W.Value.Native ? W.Value.Native->GetDefaultObject<AArenaWeapon>() : nullptr;
				if (W.Key != CurrentWeaponChannel && (W.Value.Ammo > 0 || (Def && Def->bInfiniteAmmo)) && (!Best || W.Value.Priority > Best->Priority))
				{
					Best = &W.Value;
				}
			}
			if (Best)
			{
				SwitchToWeapon(Best->Channel);
			}
			return true;
		}
		if (Name == TEXT("ClientGotoState"))
		{
			if (const FUE2NetValue* S2 = Param(Params, TEXT("NewState")))
			{
				ServerState = S2->N;
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ClientGotoState(%s)"), *ServerState.ToString());
			}
			return true;
		}
		if (Name == TEXT("NewClientPlayTakeHit") || Name == TEXT("ClientPlayTakeHit"))
		{
			// UnrealPlayer.NewClientPlayTakeHit(AttackLoc, HitLoc, Damage, DamageType): the HUD's damage direction
			AArenaPlayerController* PC = GetPC();
			const FUE2NetValue* Attack = Param(Params, TEXT("AttackLoc"));
			const FUE2NetValue* Dmg = Param(Params, TEXT("Damage"));
			if (PC && LocalPawn.IsValid())
			{
				FVector From = Attack && !Attack->V.IsZero() ? FVector(Attack->V) * S : LocalPawn->GetActorLocation();
				if (!Attack || Attack->V.IsZero())
				{
					if (const FUE2NetValue* Hit = Param(Params, TEXT("HitLoc")))
					{
						From = LocalPawn->GetActorLocation() + FVector(Hit->V) * S * 4.f;
					}
				}
				PC->NotifyTookDamage(From, Dmg ? float(Dmg->I) : 0.f);
				LocalPawn->LastHitTime = World.IsValid() ? World->GetTimeSeconds() : 0.f;
			}
			return true;
		}
		if (Name == TEXT("TeamMessage"))
		{
			// chat as the server relays it (the world layer shows it): logged as evidence that Say / TeamSay went through
			const FUE2NetValue* Text = Param(Params, TEXT("S"));
			const FUE2NetValue* Type = Param(Params, TEXT("Type"));
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: chat from the server (%s): %s"), Type ? *Type->N.ToString() : TEXT("?"), Text ? *Text->S : TEXT(""));
			return false;
		}
		if (Name == TEXT("ClientSetLocation"))
		{
			// ServerViewSelf: the spectator was moved to the view target
			if (const FUE2NetValue* L = Param(Params, TEXT("NewLocation")))
			{
				ServerSpecLoc = FVector(L->V);
				bServerSpecKnown = true;
			}
			return false;
		}
		return false;
	}
	if (!Weapons.Contains(A.Channel) && ChainHas(A, TEXT("Weapon")) && !ChainHas(A, TEXT("WeaponAttachment")))
	{
		// a call in the weapon's first bunch can come before the actor is announced
		ActorOpened(A);
	}
	if (Weapons.Contains(A.Channel))
	{
		if (Name == TEXT("ClientWeaponSet"))
		{
			// Weapon.ClientWeaponSet: the first weapon is brought up; later ones when better and bPossiblySwitch
			const FUE2NetValue* B = Param(Params, TEXT("bPossiblySwitch"));
			const FWeaponInfo& W = Weapons[A.Channel];
			const FWeaponInfo* Cur = Weapons.Find(CurrentWeaponChannel);
			if (!Cur || (B && B->I != 0 && W.Priority > Cur->Priority && !bFireHeld[0] && !bFireHeld[1]))
			{
				SwitchToWeapon(A.Channel);
			}
			return true;
		}
		if (Name == TEXT("ClientForceAmmoUpdate"))
		{
			const FUE2NetValue* Amount = Param(Params, TEXT("NewAmount"));
			if (Amount)
			{
				Weapons[A.Channel].Ammo = Amount->I;
				ApplyWeapons();
			}
			return true;
		}
		return true;
	}
	return false;
}

// -------------------------------------------------------------------------------------------------
// Corrections (UE2 LongClientAdjustPosition / ClientUpdatePosition)
// -------------------------------------------------------------------------------------------------

void FUE2NetPlay::ClientAdjust(const TArray<TPair<FName, FUE2NetValue>>& Params, bool bHasVelocity, bool bVeryShort)
{
	using namespace UE2NetPlayLocal;
	++Adjustments;
	const float TimeStamp = ParamFloat(Params, TEXT("TimeStamp"));
	const FVector NewLoc(ParamFloat(Params, TEXT("NewLocX")), ParamFloat(Params, TEXT("NewLocY")), ParamFloat(Params, TEXT("NewLocZ")));
	const FVector NewVel = bHasVelocity ? FVector(ParamFloat(Params, TEXT("NewVelX")), ParamFloat(Params, TEXT("NewVelY")), ParamFloat(Params, TEXT("NewVelZ")))
		: FVector::ZeroVector;
	uint8 NewPhysics = 1;
	if (const FUE2NetValue* Ph = Param(Params, TEXT("newPhysics")))
	{
		NewPhysics = uint8(Ph->I);
	}
	if (!bVeryShort)
	{
		if (const FUE2NetValue* St = Param(Params, TEXT("NewState")))
		{
			ServerState = St->N;
		}
	}
	else
	{
		ServerState = TEXT("PlayerWalking");
	}
	if (TimeStamp <= LastAdjustTimeStamp)
	{
		return;
	}
	LastAdjustTimeStamp = TimeStamp;
	if (AArenaGuidedWarhead* WH = NetWarhead.Get(); WH && WarheadChannel == OwnPawnChannel)
	{
		// PlayerRocketing: the server's warhead at TimeStamp, then our moves since (ClientUpdatePosition)
		FVector Loc = NewLoc;
		FVector Vel = bHasVelocity ? NewVel : WH->GetVelUU();
		RocketMoves.RemoveAll([TimeStamp](const FRocketMove& M) { return M.TimeStamp <= TimeStamp; });
		float Last = TimeStamp;
		for (const FRocketMove& M : RocketMoves)
		{
			AArenaGuidedWarhead::FlyVelocity(Vel, M.AccelUU, M.Delta);
			Loc += Vel * M.Delta;
			Last = M.TimeStamp;
		}
		// the frames since the newest move sent
		const float Rest = FMath::Clamp(float(World->GetTimeSeconds()) - Last, 0.f, 0.125f);
		AArenaGuidedWarhead::FlyVelocity(Vel, WH->GetAccelUU(), Rest);
		Loc += Vel * Rest;
		const float Error = float(FVector::Dist(Loc, WH->GetActorLocation() / S));
		if (++WarheadAdjustments <= 20)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: warhead correction %d: %.1f UU off"), WarheadAdjustments, Error);
		}
		WH->SetNetFlight(Loc, Vel);
		return;
	}
	// a base that moves (lift): the location is relative to it
	FVector BaseOffset = FVector::ZeroVector;
	const AActor* BaseNative = nullptr;
	const FUE2NetValue* BaseParam = Param(Params, TEXT("NewBase"));
	if (BaseParam && BaseParam->Obj.IsDynamic())
	{
		// movers (and vehicles) are replicated actors: UE2 sends our location relative to them
		const FUE2NetActor* BaseActor = Connection->GetActors().Find(BaseParam->Obj.Channel);
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		const AActor* Native = NW ? NW->GetNativeActor(BaseParam->Obj.Channel) : nullptr;
		if (BaseActor && Native && (ChainHas(*BaseActor, TEXT("Mover")) || ChainHas(*BaseActor, TEXT("Vehicle"))))
		{
			BaseOffset = Native->GetActorLocation() / S;
			BaseNative = Native;
		}
	}
	if (const FUE2NetValue* Base = BaseParam; Base && Base->Obj.IsStatic())
	{
		const FString Path = Connection->GetPackageMap().GetObjectPath(Base->Obj);
		const FString ClassName = Connection->GetPackageMap().GetObjectClassName(Base->Obj);
		if (ClassName.Contains(TEXT("Mover")))
		{
			FString Pkg, Actor;
			if (Path.Split(TEXT("."), &Pkg, &Actor, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				if (const AActor* M = ArenaUE2World::FindActorByName(World.Get(), Actor))
				{
					BaseOffset = M->GetActorLocation() / S;
					BaseNative = M;
				}
			}
		}
	}
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	if (!Pawn)
	{
		// spectating: where the server's spectator is
		ServerSpecLoc = NewLoc;
		bServerSpecKnown = true;
		++SpectatorAdjustments;
		if (AArenaPlayerController* PC = GetPC(); PC && SpectatorAdjustments % 20 == 1)
		{
			FVector CamLoc;
			FRotator CamRot;
			PC->GetPlayerViewPoint(CamLoc, CamRot);
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the server's spectator is at %s, our camera at %s (%.0f UU apart)"), *NewLoc.ToCompactString(),
				*(CamLoc / S).ToCompactString(), FVector::Dist(NewLoc, CamLoc / S));
		}
		return;
	}
	// drop the moves the server has simulated; the one at TimeStamp tells how far off we were
	const FMove* Match = nullptr;
	int32 Keep = 0;
	for (; Keep < SavedMoves.Num() && SavedMoves[Keep].TimeStamp <= TimeStamp; ++Keep)
	{
		if (SavedMoves[Keep].TimeStamp == TimeStamp)
		{
			Match = &SavedMoves[Keep];
		}
	}
	const FVector ServerLoc = NewLoc + BaseOffset - FVector(0.f, 0.f, ZOffset + (Match ? Match->SlopeZ : 0.f));
	// on a lift: our location relative to our lift at the end of that move against the server's relative one (both
	// copies of the lift move on their own clocks, so absolute locations differ by the lift's travel in between)
	const FVector OurLoc = !Match ? FVector::ZeroVector
		: (BaseNative && Match->EndBase.Get() == BaseNative) ? (Match->EndLoc - Match->EndBaseLoc + BaseNative->GetActorLocation()) / S : Match->EndLoc / S;
	const float Error = Match ? float(FVector::Dist(OurLoc, ServerLoc)) : -1.f;
	const float VelError = Match ? float(FVector::Dist(Match->EndVel / S, NewVel)) : -1.f;
	const uint8 MatchPhysics = Match ? Match->In.DoubleClickMove : 0;
	(void)MatchPhysics;
#if !UE_BUILD_SHIPPING
	if (bProbe)
	{
		// test hook -UE2NetPlayProbe: every move is "wrong", so the server answers each with its own pawn state; logged, not applied
		if (Match)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlayProbe: t %.3f dt %.4f in(d%d j%d dj%d) ours %s vel %s %s | server %s vel %s phys %d | diff %s"), TimeStamp, Match->Delta,
				Match->In.DoubleClickMove, Match->In.bPressedJump, Match->In.bDoubleJump, *(Match->EndLoc / S).ToCompactString(), *(Match->EndVel / S).ToCompactString(),
				Match->bEndWalking ? TEXT("walk") : TEXT("air"), *ServerLoc.ToCompactString(), *NewVel.ToCompactString(), NewPhysics,
				*(ServerLoc - Match->EndLoc / S).ToCompactString());
		}
		SavedMoves.RemoveAt(0, Keep);
		return;
	}
#endif
	// learn the height difference of this floor (both walking, only the height differs): per floor surface and
	// steepness, as our converted level's floors (BSP, static meshes, terrain) differ from UE2's by their own amounts
	if (Match && Match->bEndWalking && NewPhysics == 1 && Match->SlopeBucket >= 0 && FVector::Dist2D(Match->EndLoc / S, ServerLoc) < 1.f)
	{
		const float Sample = float(ServerLoc.Z - Match->EndLoc.Z / S);
		if (FMath::Abs(Sample) < 30.f)
		{
			int32& N = FloorSamples.FindOrAdd(Match->FloorKey);
			float& Off = FloorOffsets.FindOrAdd(Match->FloorKey);
			Off += Sample * (N < 3 ? 0.7f : 0.3f);
			++N;
			const int32 B = Match->SlopeBucket;
			SlopeOffsets[B] += Sample * (SlopeSamples[B] < 5 ? 0.5f : 0.2f);
			++SlopeSamples[B];
			++ZOffsetSamples;
		}
	}
	if (Match && Error < 3.f && (VelError < 3.f || !bHasVelocity) && (NewPhysics == 0 || NewPhysics == 1 || NewPhysics == 2 || NewPhysics == 3 || NewPhysics == 11))
	{
		// UE2 LongClientAdjustPosition: already there (the server's adjustment crossed our move)
		++IgnoredAdjustments;
		SavedMoves.RemoveAt(0, Keep);
		return;
	}
	++Corrections;
	if (TimeStamp >= LastSpecialTime && TimeStamp - LastSpecialTime < 1.f)
	{
		++CorrectionsAfterSpecial;
	}
	if (Error >= 0.f)
	{
		SumCorrection += Error;
		MaxCorrection = FMath::Max(MaxCorrection, Error);
	}
	if (CorrectionLogBudget > 0 || Corrections % 25 == 0)
	{
		--CorrectionLogBudget;
		// diagnostics: how high above our floor the server's location is (UE2 walking pawns rest at a small height)
		float FloorGap = -1.f;
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetPlayFloor), false, Pawn);
			const FVector Start = ServerLoc * S;
			if (World->SweepSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, 400.f), FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(Pawn->GetCapsuleComponent()->GetScaledCapsuleRadius(), Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), Q))
			{
				FloorGap = float(Start.Z - Hit.Location.Z) / S;
			}
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: server location %.2f UU above our floor (z offset %.2f, %d samples)"), FloorGap + ZOffset, ZOffset, ZOffsetSamples);
		// diagnostics: a horizontal correction (pushing into a wall): what our world has in the direction of the move
		if (Match && Match->Accel.SizeSquared2D() > 1.f && FVector::Dist2D(Match->EndLoc / S, ServerLoc) > 1.5f)
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetPlayWall), false, Pawn);
			const FVector Start = ServerLoc * S;
			const FVector Dir = (Match->EndLoc / S - ServerLoc).GetSafeNormal2D();
			const UCapsuleComponent* Cap = Pawn->GetCapsuleComponent();
			const FCollisionShape Shape = Cap->GetCollisionShape();
			if (World->SweepSingleByChannel(Hit, Start, Start + Dir * 200.f, FQuat::Identity, ECC_Pawn, Shape, Q))
			{
				const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Hit.GetComponent());
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: wall probe: our blocker %s (%s) %.2f UU ahead of the server's location, normal %s"),
					Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("-"), SMC && SMC->GetStaticMesh() ? *SMC->GetStaticMesh()->GetPathName() : Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("-"),
					Hit.Distance / S, *Hit.ImpactNormal.ToCompactString());
			}
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: correction %d at %.3f: error %.1f UU (ours %s, server %s), velocity error %.1f, physics %d, state %s, move: accel %s dclick %d jump %d dj %d"),
			Corrections, TimeStamp, Error, Match ? *(Match->EndLoc / S).ToCompactString() : TEXT("-"), *ServerLoc.ToCompactString(), VelError, NewPhysics,
			*ServerState.ToString(), Match ? *(Match->Accel / S).ToCompactString() : TEXT("-"), Match ? Match->In.DoubleClickMove : -1, Match ? Match->In.bPressedJump : -1,
			Match ? Match->In.bDoubleJump : -1);
		if (Match)
		{
			const UPrimitiveComponent* Floor = Match->FloorComp.Get();
			const UStaticMeshComponent* FloorSM = Cast<UStaticMeshComponent>(Floor);
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: correction %d details: velocity ours %s server %s, dt %.4f, %s, floor %s (%s)"), Corrections,
				*(Match->EndVel / S).ToCompactString(), *NewVel.ToCompactString(), Match->Delta, Match->bEndWalking ? TEXT("walking") : TEXT("falling"),
				Floor && Floor->GetOwner() ? *Floor->GetOwner()->GetName() : TEXT("-"),
				FloorSM && FloorSM->GetStaticMesh() ? *FloorSM->GetStaticMesh()->GetName() : Floor ? *Floor->GetName() : TEXT("-"));
		}
	}
	SavedMoves.RemoveAt(0, Keep);
	UCharacterMovementComponent* CM = Pawn->GetCharacterMovement();
	// the server's physics first (our capsule height follows it, without the landing lift: the server's location
	// already is a walking one), then its location and velocity
	const EMovementMode Mode = MovementFor(NewPhysics);
	Pawn->bNoLandingLift = true;
	if (Mode == MOVE_Custom)
	{
		if (!Cast<UArenaCharacterMovement>(CM)->IsOnLadder())
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the server says we climb a ladder"));
			++LadderEntries;
		}
		CM->SetMovementMode(MOVE_Custom, UArenaCharacterMovement::CustomLadder);
	}
	else if (NewPhysics == 0 && !bVeryShort)
	{
		// PHYS_None: the server froze our pawn (Assault's RoundEnded, the end of the match): ours stops as well
		// instead of falling or walking on, which drew a correction every move until the next round
		CM->DisableMovement();
	}
	else if (Mode != MOVE_None && CM->MovementMode != Mode)
	{
		CM->SetMovementMode(Mode);
	}
	Pawn->ApplyNetCollision();
	Pawn->bNoLandingLift = false;
	Pawn->SetActorLocation(ServerLoc * S, false, nullptr, ETeleportType::TeleportPhysics);
	CM->Velocity = NewVel * S;
	CM->bForceNextFloorCheck = true;
	if (Mode == MOVE_Walking)
	{
		CM->FindFloor(CM->UpdatedComponent->GetComponentLocation(), CM->CurrentFloor, false);
	}
	ReplayMovesAfter(TimeStamp);
}

void FUE2NetPlay::ReplayMovesAfter(float TimeStamp)
{
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaPlayerController* PC = GetPC();
	UArenaCharacterMovement* CM = Pawn ? Cast<UArenaCharacterMovement>(Pawn->GetCharacterMovement()) : nullptr;
	if (!Pawn || !PC || !CM)
	{
		return;
	}
	const FRotator LiveView = PC->GetControlRotation();
	const FRotator LiveRot = Pawn->GetActorRotation();
	const float MaxAccel = FMath::Max(1.f, CM->GetMaxAcceleration());
	for (FMove& M : SavedMoves)
	{
		if (M.TimeStamp <= TimeStamp)
		{
			continue;
		}
		PC->SetControlRotation(M.View);
		if (!UUE2NetBoxCollision::UseBox())
		{
			Pawn->SetActorRotation(FRotator(0.f, M.View.Yaw, 0.f));
		}
		Pawn->StepInputOverride = &M.In;
		CM->SimulateNetStep(M.Delta, M.Accel / MaxAccel);
		Pawn->StepInputOverride = nullptr;
		M.EndLoc = Pawn->GetActorLocation();
		M.EndVel = CM->Velocity;
		M.bEndWalking = CM->IsMovingOnGround();
		ClassifyFloor(M);
		++ReplayedMoves;
	}
	PC->SetControlRotation(LiveView);
	if (!UUE2NetBoxCollision::UseBox())
	{
		Pawn->SetActorRotation(LiveRot);
	}
}

// -------------------------------------------------------------------------------------------------
// Moves (UE2 ReplicateMove / CallServerMove)
// -------------------------------------------------------------------------------------------------

void FUE2NetPlay::PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds)
{
	if (InWorld != World.Get() || Connection->GetState() != FUE2NetConnection::EState::Joined || DeltaSeconds <= 0.f)
	{
		return;
	}
	NewNet->Tick(InWorld->GetTimeSeconds(), DeltaSeconds, OwnPRIChannel);
	if (DrivingChannel && LocalPawn.IsValid())
	{
		DrivingTime += DeltaSeconds;
		SendDrive();
	}
	else if (WarheadChannel && WarheadChannel == OwnPawnChannel)
	{
		SendRocketMove();
	}
	else if (LocalPawn.IsValid() && OwnPawnChannel && AckedPawnChannel == OwnPawnChannel)
	{
		RecordMove(DeltaSeconds);
		SendMoves(false);
		TickNewNetFire();
		TickFireEffects();
	}
	else if (!IsPlaying() || (!LocalPawn.IsValid() && ServerState != TEXT("Dead") && ServerState != TEXT("GameEnded") && ServerState != TEXT("RoundEnded")))
	{
		// spectators, and players without a pawn that the server lets look around (PlayerWaiting, Spectating), move the
		// server's controller like UE2 BaseSpectating.PlayerMove does; its adjustments tell us its state. Dead players
		// stay where the server put them.
		SendSpectatorMove(DeltaSeconds);
	}
}

bool FUE2NetPlay::PlanFlagLeg(AUE2NetLocalPawn* Pawn, int32 Stage)
{
	UWorld* W = World.Get();
	AArenaGameMode* GM = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
	AArenaNavGraph* Nav = GM ? GM->GetNavGraph() : nullptr;
	const AArenaPlayerController* PC = GetPC();
	const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (!Nav || !Pawn || !PS)
	{
		return false;
	}
	FVector Goal = FVector::ZeroVector;
	bool bFound = false;
	for (TActorIterator<AArenaFlag> It(W); It; ++It)
	{
		if ((Stage == 1) == (It->GetTeam() != PS->Team))
		{
			Goal = It->GetHomeLocation();
			bFound = true;
		}
	}
	TArray<FVector> Path;
	TArray<EArenaNavLinkType> Links;
	if (!bFound || !Nav->FindPath(Pawn->GetActorLocation(), Goal, Path, Links))
	{
		return false;
	}
	ScriptRoute = Path;
	ScriptRouteLinks = Links;
	ScriptRouteIndex = 0;
	ScriptFlagStage = Stage;
	ScriptStuckCount = 0;
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: route flag: to the %s flag, %d way points"), Stage == 1 ? TEXT("enemy") : TEXT("own"), Path.Num());
	return true;
}

bool FUE2NetPlay::IsVehicleChannel(int32 Channel) const
{
	const FUE2NetActor* A = Channel ? Connection->GetActors().Find(Channel) : nullptr;
	return A && UE2NetPlayLocal::ChainHas(*A, TEXT("Vehicle"));
}

void FUE2NetPlay::EnterVehicle(int32 Channel)
{
	if (Channel == DrivingChannel)
	{
		return;
	}
	DrivingChannel = Channel;
	++VehicleEntries;
	LastDriveSend = -1.f;
	const FUE2NetActor* A = Connection->GetActors().Find(Channel);
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: driving %s (channel %d)"), A ? *A->ClassPath : TEXT("?"), Channel);
	SavedMoves.Reset();
	for (bool& B : bFireHeld)
	{
		B = false;
	}
	// UE2 PlayerController.AcknowledgePossession for the vehicle (else the server keeps calling GivePawn)
	const float Handedness = float(FMath::Clamp(UArenaUserSettings::Get()->WeaponHand, 0, 3) - 1);
	Send(PCChannel(), TEXT("ServerAcknowledgePossession"), { UE2NetPlayLocal::VActor(Channel), UE2NetPlayLocal::VFloat(Handedness), UE2NetPlayLocal::VBool(false) });
	if (AUE2NetLocalPawn* Pawn = LocalPawn.Get())
	{
		// UE2: the driver is attached to the vehicle (hidden or in its seat); input goes to ServerDrive
		Pawn->SetActorHiddenInGame(true);
		Pawn->SetActorEnableCollision(false);
		if (AArenaWeapon* W = Pawn->GetCurrentWeapon())
		{
			W->SetActorHiddenInGame(true);
		}
		Pawn->GetCharacterMovement()->DisableMovement();
		FUE2NetWorld* NW = FUE2NetWorld::Get();
		Pawn->DrivenVehicle = NW ? NW->GetNativeActor(Channel) : nullptr;
		// the world layer shows our xPawn in its seat while we drive
		if (const FUE2NetActor* Own = NW ? Connection->GetActors().Find(OwnPawnChannel) : nullptr)
		{
			NW->ActorOpened(*Own);
		}
	}
}

void FUE2NetPlay::ExitVehicle(bool bToFoot)
{
	if (!DrivingChannel)
	{
		return;
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: out of the vehicle (channel %d)"), DrivingChannel);
	// our xPawn back from the world layer
	FUE2NetWorld* NW = FUE2NetWorld::Get();
	if (const FUE2NetActor* Own = NW ? Connection->GetActors().Find(OwnPawnChannel) : nullptr)
	{
		NW->ActorClosed(*Own);
	}
	DrivingChannel = 0;
	for (bool& B : bFireHeld)
	{
		B = false;
	}
	if (OwnPawnChannel)
	{
		const float Handedness = float(FMath::Clamp(UArenaUserSettings::Get()->WeaponHand, 0, 3) - 1);
		Send(PCChannel(), TEXT("ServerAcknowledgePossession"), { UE2NetPlayLocal::VActor(OwnPawnChannel), UE2NetPlayLocal::VFloat(Handedness), UE2NetPlayLocal::VBool(false) });
	}
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	if (!Pawn)
	{
		return;
	}
	AActor* Vehicle = Pawn->DrivenVehicle.Get();
	Pawn->DrivenVehicle.Reset();
	Pawn->SetActorHiddenInGame(false);
	Pawn->SetActorEnableCollision(true);
	if (AArenaWeapon* W = Pawn->GetCurrentWeapon())
	{
		W->SetActorHiddenInGame(false);
	}
	if (bToFoot)
	{
		// the server puts the driver beside the vehicle; its first correction places us exactly
		if (Vehicle)
		{
			Pawn->SetActorLocation(Vehicle->GetActorLocation() + FVector(0.f, 0.f, 150.f), false, nullptr, ETeleportType::TeleportPhysics);
		}
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	}
	LastTimeStamp = float(World->GetTimeSeconds());
}

void FUE2NetPlay::SendDrive()
{
	using namespace UE2NetPlayLocal;
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaPlayerController* PC = GetPC();
	if (!Pawn || !PC)
	{
		return;
	}
	if (!Pawn->DrivenVehicle.IsValid())
	{
		if (FUE2NetWorld* NW = FUE2NetWorld::Get())
		{
			Pawn->DrivenVehicle = NW->GetNativeActor(DrivingChannel);
		}
	}
	if (AActor* V = Pawn->DrivenVehicle.Get())
	{
		// our hidden body rides along (sounds at our place)
		Pawn->SetActorLocation(V->GetActorLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	}
	// UE2 PlayerDriving.PlayerMove: ServerDrive(aForward, aStrafe, aUp, bPressedJump, View); Throttle = aForward / 5000,
	// Steering = -aStrafe / 5000, Rise = aUp / 5000 (keyboard input goes past the limits)
	const FRotator View = PC->GetControlRotation();
	const FVector In = Pawn->GetLastMovementInputVector();
	const FRotationMatrix R(FRotator(0.f, View.Yaw, 0.f));
	const float Forward = float(FVector::DotProduct(In, R.GetUnitAxis(EAxis::X))) * 6000.f;
	const float Strafe = float(FVector::DotProduct(In, R.GetUnitAxis(EAxis::Y))) * 6000.f;
	const float Up = (Pawn->IsJumpHeld() ? 6000.f : 0.f) - (Pawn->IsDuckHeld() ? 6000.f : 0.f);
	const bool bJump = Pawn->LastStepInput.bPressedJump;
	Pawn->LastStepInput = AArenaCharacter::FUE2StepInput();
	const int32 ViewBits = ViewInt(View);
	const float Now = float(World->GetTimeSeconds());
	const FVector4f Input(Forward, Strafe, Up, bJump ? 1.f : 0.f);
	// like UE2: no update while nothing changed, at most one per 1/45 s otherwise
	if (Now - LastDriveSend < 0.0222f || (Input == LastDriveInput && ViewBits == LastDriveView && Now - LastDriveSend < 0.5f))
	{
		return;
	}
	LastDriveInput = Input;
	LastDriveView = ViewBits;
	LastDriveSend = Now;
	Send(PCChannel(), TEXT("ServerDrive"), { VFloat(Forward), VFloat(Strafe), VFloat(Up), VBool(bJump), VInt(ViewBits) });
	++DriveUpdates;
}

void FUE2NetPlay::ClassifyFloor(FMove& M) const
{
	M.SlopeBucket = -1;
	M.SlopeZ = 0.f;
	M.EndBase = nullptr;
	M.FloorComp = nullptr;
	const AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	const UCharacterMovementComponent* CM = Pawn ? Pawn->GetCharacterMovement() : nullptr;
	if (!CM || !M.bEndWalking || !CM->CurrentFloor.bBlockingHit)
	{
		return;
	}
	M.FloorComp = CM->CurrentFloor.HitResult.GetComponent();
	if (const AArenaMover* Mover = Cast<AArenaMover>(CM->CurrentFloor.HitResult.GetActor()))
	{
		// on a lift: no floor height learning (the lift moves), the location is compared relative to it
		M.EndBase = Mover;
		M.EndBaseLoc = Mover->GetActorLocation();
		return;
	}
	static const bool bOff = FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoSlopeOffsets")); // test hook (A/B)
	if (bOff)
	{
		return;
	}
	// the floor surface (component) and its steepness class (0 flat, then steps of 0.02 of the normal's Z)
	const float NZ = float(CM->CurrentFloor.HitResult.ImpactNormal.Z);
	M.SlopeBucket = NZ > 0.995f ? 0 : FMath::Clamp(1 + FMath::RoundToInt((1.f - NZ) / 0.02f), 1, 15);
	M.FloorKey = HashCombine(GetTypeHash(CM->CurrentFloor.HitResult.GetComponent()), uint32(M.SlopeBucket));
	// a surface not seen yet: what the same steepness has needed elsewhere
	const float* Known = FloorOffsets.Find(M.FloorKey);
	M.SlopeZ = Known ? *Known : SlopeOffsets[M.SlopeBucket];
}

void FUE2NetPlay::StartWarhead(const FUE2NetActor& A)
{
	using namespace UE2NetPlayLocal;
	UWorld* W = World.Get();
	AArenaPlayerController* PC = GetPC();
	if (!W || !PC)
	{
		return;
	}
	FVector Loc = FVector(A.SpawnLocation);
	if (const FUE2NetValue* L = Prop(A, TEXT("Location")))
	{
		Loc = FVector(L->V);
	}
	FIntVector RotUU(A.SpawnRotation.X, A.SpawnRotation.Y, 0);
	if (const FUE2NetValue* R = Prop(A, TEXT("Rotation")))
	{
		RotUU = R->R;
	}
	const FRotator Rot(RotUU.X * 360.f / 65536.f, RotUU.Y * 360.f / 65536.f, 0.f);
	const FUE2NetValue* V = Prop(A, TEXT("Velocity"));
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.ObjectFlags |= RF_Transient;
	AArenaGuidedWarhead* WH = W->SpawnActor<AArenaGuidedWarhead>(AArenaGuidedWarhead::StaticClass(), Loc * S, Rot, P);
	if (!WH)
	{
		return;
	}
	WH->LaunchNet(PC, V ? FVector(V->V) : Rot.Vector() * 1000.f);
	if (FUE2NetWorld* NW = FUE2NetWorld::Get())
	{
		NW->DropNativeActor(A.Channel); // the missile the world layer may already show for it
	}
	NetWarhead = WH;
	WarheadChannel = A.Channel;
	RocketMoves.Reset();
	RocketMovesSent = 0;
	WarheadAdjustments = 0;
	LastAdjustTimeStamp = -1.f;
	LastSendTime = 0.f;
	// PlayerController.ClientRestart -> AcknowledgePossession, as for any pawn
	const float Handedness = float(FMath::Clamp(UArenaUserSettings::Get()->WeaponHand, 0, 3) - 1);
	Send(PCChannel(), TEXT("ServerAcknowledgePossession"), { VActor(A.Channel), VFloat(Handedness), VBool(false) });
	AckedPawnChannel = A.Channel;
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: flying the guided warhead (channel %d) from %s"), A.Channel, *Loc.ToCompactString());
}

void FUE2NetPlay::EndWarhead()
{
	if (AArenaGuidedWarhead* WH = NetWarhead.Get())
	{
		if (AArenaPlayerController* PC = GetPC(); PC && PC->GetPawn() == WH)
		{
			PC->UnPossess();
		}
		WH->Destroy();
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: the guided warhead is over (%d moves sent, %d corrections)"), RocketMovesSent, WarheadAdjustments);
	}
	NetWarhead.Reset();
	WarheadChannel = 0;
	RocketMoves.Reset();
}

void FUE2NetPlay::SendRocketMove()
{
	using namespace UE2NetPlayLocal;
	AArenaGuidedWarhead* WH = NetWarhead.Get();
	const int32 Ch = PCChannel();
	if (!WH || !Ch || WH->IsStaticScreen())
	{
		return;
	}
	// PlayerRocketing.PlayerMove -> ReplicateMove: RocketServerMove(TimeStamp, Acceleration * 10, Location, bDuck,
	// ClientRoll, View) at most every 1/45 s; the server flies the warhead with that acceleration since our last move
	const float Now = float(World->GetTimeSeconds());
	if (Now - LastSendTime < 0.0222f)
	{
		return;
	}
	FRocketMove M;
	M.TimeStamp = Now;
	M.Delta = FMath::Min(Now - FMath::Max(LastSendTime, LastTimeStamp), 0.125f);
	M.AccelUU = WH->GetAccelUU();
	LastSendTime = Now;
	LastTimeStamp = Now;
	RocketMoves.Add(M);
	while (RocketMoves.Num() > 200)
	{
		RocketMoves.RemoveAt(0);
	}
	const FRotator R = WH->GetActorRotation();
	const int32 Roll = (ToUU(R.Roll) >> 8) & 255;
	Send(Ch, TEXT("RocketServerMove"), { VFloat(Now), VVec(M.AccelUU * 10.f), VVec(WH->GetActorLocation() / S), VBool(false), VByte(Roll),
		VInt(ViewInt(FRotator(R.Pitch, R.Yaw, 0.f))) });
	++RocketMovesSent;
	++MovesSent;
}

void FUE2NetPlay::TickFireEffects()
{
	// UE2 clients play their own shots (Weapon.ClientStartFire -> the fire mode's ModeDoFire effects) while the server
	// fires them: sound, muzzle flash and the view animation at the fire rate while the button is held; charged modes
	// on release (bFireOnRelease), the shield, beams and laser sights while held (AArenaWeapon::SetNetFireHeld)
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaWeapon* W = Pawn ? Pawn->GetCurrentWeapon() : nullptr;
	const FWeaponInfo* Info = Weapons.Find(CurrentWeaponChannel);
	if (!Info || (W && Info->Native != W->GetClass()))
	{
		W = nullptr;
	}
	if (AArenaWeapon* Old = NetFireWeapon.Get(); Old && Old != W)
	{
		// switched away (or died): no release shot
		Old->SetNetFireHeld(0, false, false);
		Old->SetNetFireHeld(1, false, false);
	}
	NetFireWeapon = W;
	if (!W)
	{
		return;
	}
	const float Now = float(World->GetTimeSeconds());
	if (W->IsA<AWeap_UE2Script>())
	{
		return; // the mod's own script fires it (Net/UE2NetScriptWeapon.h)
	}
	// a mod weapon our stand-in plays: its fire modes' client code runs alongside, so its fire timing follows the mod's
	// (a spin-up in ModeTick, a FireRate set by ModeDoFire)
	FUE2NetScriptHUD* ScriptHud = W->GetNetStandIn().UE2Class.IsEmpty() ? nullptr : FUE2NetScriptHUD::Get();
	if (ScriptHud)
	{
		ScriptHud->TickStandInWeapon(CurrentWeaponChannel, bFireHeld[0], bFireHeld[1], FMath::Max(0.f, Now - LastStandInTick));
		for (uint8 Mode = 0; Mode < 2; ++Mode)
		{
			W->SetNetFireRate(Mode, ScriptHud->GetScriptFireRate(CurrentWeaponChannel, Mode));
		}
	}
	LastStandInTick = Now;
	// the translocator's presses count without charges too (TransRecall's failed sound)
	const bool bAmmo = Info->Ammo > 0 || W->bInfiniteAmmo || W->IsA<AWeap_Translocator>();
	for (uint8 Mode = 0; Mode < 2; ++Mode)
	{
		const float Refire = FMath::Max(0.05f, W->FireModes[Mode].RefireTime);
		if (!bFireHeld[Mode])
		{
			bNetWaitRelease[Mode] = false;
			if (W->FireModes[Mode].FireType == EArenaFireType::Melee)
			{
				NextMeleeTest = Now + 0.15f; // ShieldFire.ModeHoldFire: SetTimer(AutoFireTestFreq, true)
			}
		}
		// a charge keeps going when it took the last ammo (bio alt), the release fires it
		const bool bHold = bFireHeld[Mode] && !bNetWaitRelease[Mode] && (bAmmo || (W->IsNetFireHeld(Mode) && W->IsChargeMode(Mode)));
		bool bShot = W->SetNetFireHeld(Mode, bHold, Now >= NextFireEffect[Mode]);
		if (!bShot && bHold && W->FireModes[Mode].FireType == EArenaFireType::Melee && Now >= NextMeleeTest)
		{
			// ShieldFire.Timer (AutoFireTestFreq 0.15 s, also on the owning client): a pawn in front within ShieldRange of
			// the pawn's location releases the charge
			NextMeleeTest = Now + 0.15f;
			const FVector Dir = GetPC() ? GetPC()->GetControlRotation().Vector() : Pawn->GetActorForwardVector();
			if (Now >= NextFireEffect[Mode] && TraceShownPawn(Pawn->GetActorLocation(), Dir, W->FireModes[Mode].Range) != 0)
			{
				bShot = W->SetNetFireHeld(Mode, false, true);
				bNetWaitRelease[Mode] = true;
			}
		}
		if (!bShot && bFireHeld[Mode] && bAmmo && W->IsNetAutoFire(Mode) && Now >= NextFireEffect[Mode])
		{
			W->PlayNetFireEffects(Mode);
			bShot = true;
		}
		if (bShot)
		{
			if (ScriptHud)
			{
				ScriptHud->StandInFired(CurrentWeaponChannel, Mode);
			}
			NextFireEffect[Mode] = Now + Refire;
			if (W->bSharedFireMode)
			{
				NextFireEffect[1 - Mode] = NextFireEffect[Mode];
			}
		}
	}
}

void FUE2NetPlay::TickNewNetFire()
{
	using namespace UE2NetPlayLocal;
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaPlayerController* PC = GetPC();
	UWorld* W = World.Get();
	if (!Pawn || !PC || !W || (!bNewNetFirePending[0] && !bNewNetFirePending[1]))
	{
		return;
	}
	const float Now = float(W->GetTimeSeconds());
	for (uint8 Mode = 0; Mode < 2; ++Mode)
	{
		if (!bNewNetFirePending[Mode] || !bFireHeld[Mode] || !CurrentWeaponChannel)
		{
			bNewNetFirePending[Mode] = false;
			continue;
		}
		// Weapon.ReadyToFire: the fire mode's NextFireTime has passed (UE2 retries ClientStartFire every frame while
		// the button is held); started while the other (exclusive) mode is still busy: OldServerStartFire
		if (Now < NextFireEffect[Mode])
		{
			continue;
		}
		bNewNetFirePending[Mode] = false;
		FUE2NetNewNet::FFire F;
		F.Mode = Mode;
		const FRotator View = PC->GetControlRotation();
		F.Yaw = ToUU(View.Yaw);
		F.Pitch = ToUU(View.Pitch);
		// Pawn.Location + EyePosition(), in the frame of our ClientLoc (the learned floor offsets included)
		const FVector Eye = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : Pawn->GetPawnViewLocation();
		const float FloorZ = ZOffset + (SavedMoves.Num() > 0 ? SavedMoves.Last().SlopeZ : 0.f);
		F.EyeUU = Eye / S + FVector(0.f, 0.f, FloorZ);
		F.bOld = NextFireEffect[1 - Mode] > Now && NewNet->HasOldStartFire(CurrentWeaponChannel);
		// what our own trace along the view hits (NewNet's bBelievesHit / A: Trace(..., true) from the eye, 40000 UU):
		// the world first, then the players as we show them (their UE2 collision cylinders, a vehicle's bounds)
		F.HitChannel = TraceShownPawn(Eye, View.Vector(), 40000.f * S);
		if (NewNet->SendStartFire(CurrentWeaponChannel, F))
		{
			++FireCommands;
		}
		else
		{
			Send(CurrentWeaponChannel, TEXT("ServerStartFire"), { VByte(Mode) });
			++FireCommands;
		}
	}
}

int32 FUE2NetPlay::TraceShownPawn(const FVector& Start, const FVector& Dir, float Range) const
{
	using namespace UE2NetPlayLocal;
	UWorld* W = World.Get();
	FUE2NetWorld* NW = FUE2NetWorld::Get();
	if (!W || !NW)
	{
		return 0;
	}
	float BestT = Range;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetNewNetAim), false, LocalPawn.Get());
	if (W->LineTraceSingleByChannel(Hit, Start, Start + Dir * Range, ECC_Visibility, Q))
	{
		BestT = float(Hit.Distance);
	}
	int32 Best = 0;
	for (const TPair<int32, FUE2NetActor>& KV : Connection->GetActors())
	{
		const bool bPawn = ChainHas(KV.Value, TEXT("xPawn"));
		const bool bVehicle = !bPawn && ChainHas(KV.Value, TEXT("Vehicle"));
		const AActor* A = (bPawn || bVehicle) && KV.Key != OwnPawnChannel ? NW->GetNativeActor(KV.Key) : nullptr;
		if (!A || A->IsHidden())
		{
			continue;
		}
		if (const AUE2NetPawn* P = Cast<AUE2NetPawn>(A); P && P->IsNetDead())
		{
			continue;
		}
		float T = -1.f;
		if (bPawn)
		{
			// the pawn's cylinder (CollisionRadius 25, CollisionHeight 44 UU, crouched 34)
			const float R = 25.f * S;
			const float H = (GetInt(KV.Value, TEXT("bIsCrouched"), 0) ? 34.f : 44.f) * S;
			const FVector C = A->GetActorLocation();
			const FVector2D O(Start.X - C.X, Start.Y - C.Y), D2(Dir.X, Dir.Y);
			const float a = float(D2.SizeSquared()), b = 2.f * float(FVector2D::DotProduct(O, D2)), c = float(O.SizeSquared()) - R * R;
			const float Disc = b * b - 4.f * a * c;
			if (a > KINDA_SMALL_NUMBER && Disc >= 0.f)
			{
				const float T0 = (-b - FMath::Sqrt(Disc)) / (2.f * a), T1 = (-b + FMath::Sqrt(Disc)) / (2.f * a);
				for (float Cand : { T0, T1 })
				{
					const float Z = float(Start.Z + Dir.Z * Cand - C.Z);
					if (Cand >= 0.f && FMath::Abs(Z) <= H)
					{
						T = Cand;
						break;
					}
				}
			}
		}
		else
		{
			FVector HitLoc, HitNormal;
			float HitTime = 0.f;
			const FBox Box = A->GetComponentsBoundingBox(false);
			if (Box.IsValid && FMath::LineExtentBoxIntersection(Box, Start, Start + Dir * Range, FVector::ZeroVector, HitLoc, HitNormal, HitTime))
			{
				T = HitTime * Range;
			}
		}
		if (T >= 0.f && T < BestT)
		{
			BestT = T;
			Best = KV.Key;
		}
	}
	return Best;
}

void FUE2NetPlay::RecordMove(float DeltaSeconds)
{
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaPlayerController* PC = GetPC();
	if (!Pawn || !PC || PC->GetPawn() != Pawn)
	{
		return;
	}
	UCharacterMovementComponent* CM = Pawn->GetCharacterMovement();
	FMove M;
	M.TimeStamp = float(World->GetTimeSeconds());
	if (M.TimeStamp <= LastTimeStamp)
	{
		return;
	}
	M.Delta = M.TimeStamp - LastTimeStamp;
	// the first move after a pause in moves: the server clamps to MaxResponseTime (0.125 game seconds)
	M.Delta = FMath::Min(M.Delta, DeltaSeconds);
	LastTimeStamp = M.TimeStamp;
	M.Accel = CM->GetCurrentAcceleration();
	if (const UArenaCharacterMovement* ACM = Cast<UArenaCharacterMovement>(CM); ACM && ACM->IsOnLadder())
	{
		// UE2 PlayerClimbing: Acceleration = aForward * ClimbDir (up for forward, down for back)
		const float Forward = float(FVector::DotProduct(M.Accel, FRotator(0.f, PC->GetControlRotation().Yaw, 0.f).Vector()));
		M.Accel = FMath::Abs(Forward) < 1.f ? FVector::ZeroVector : ACM->LadderClimbDir * FMath::Sign(Forward) * ACM->GetMaxAcceleration();
	}
	M.In = Pawn->LastStepInput;
	Pawn->LastStepInput = AArenaCharacter::FUE2StepInput();
	M.View = PC->GetControlRotation();
	M.EndLoc = Pawn->GetActorLocation();
	M.EndVel = CM->Velocity;
	M.bEndWalking = CM->IsMovingOnGround();
	ClassifyFloor(M);
	if (M.In.bPressedJump)
	{
		bJumpStatus = !bJumpStatus;
	}
	M.bJumpStatus = bJumpStatus;
	if (M.In.bDoubleJump)
	{
		++NumDoubleJumps;
	}
	else if (M.In.bPressedJump)
	{
		++NumJumps;
	}
	if (M.In.DoubleClickMove >= 1 && M.In.DoubleClickMove <= 4)
	{
		++NumDodges;
	}
	if (M.IsImportant())
	{
		LastSpecialTime = M.TimeStamp;
	}
	SavedMoves.Add(M);
	// moves the server can no longer ask about
	while (SavedMoves.Num() > 0 && (SavedMoves.Num() > 400 || M.TimeStamp - SavedMoves[0].TimeStamp > 2.f))
	{
		SavedMoves.RemoveAt(0);
	}
}

void FUE2NetPlay::SendMoves(bool bForce)
{
	TArray<int32> Unsent;
	for (int32 i = 0; i < SavedMoves.Num(); ++i)
	{
		if (!SavedMoves[i].bSent)
		{
			Unsent.Add(i);
		}
	}
	if (Unsent.Num() == 0)
	{
		return;
	}
	const FMove& Newest = SavedMoves[Unsent.Last()];
	// UE2 ReplicateMove holds a move back (PendingMove, sent with the next one) while less than NetMoveDelta has passed
	// since the last send: OldUnreal 3374 NetMoveDelta = FMax(0.011, 2 * MoveRepSize (42) / CurrentNetSpeed), the net
	// speed capped by the server's ClientCapBandwidth; the elapsed game time counts * TimeDilation * 0.91. (3369 used
	// 0.0222 unless the net speed is above 10000 with at most 10 players.) A jump or dodge goes at once.
	const int32 NetSpeed = FMath::Max(1, ClientCap > 0 ? FMath::Min(ClientCap, Connection->GetSettings().NetSpeed) : Connection->GetSettings().NetSpeed);
	static const bool bLegacy = FParse::Param(FCommandLine::Get(), TEXT("UE2NetLegacyMoves")); // test hook (A/B): the cadence before 3374
	const float NetMoveDelta = FMath::Max(bLegacy ? 0.0222f : 0.011f, 2.f * 42.f / float(NetSpeed));
	const float Dilation = World.IsValid() ? World->GetWorldSettings()->TimeDilation : 1.f;
	if (!bForce && Unsent.Num() < 2 && !Newest.IsImportant() && (Newest.TimeStamp - LastSendTime) * Dilation * 0.91f < NetMoveDelta)
	{
		return;
	}
	// UE2 OldMove: the newest jump / dodge move of the moves not yet confirmed by an adjustment, else the newest one whose
	// acceleration turned (dot < 0.95), repeated with the next sends in case its packet was lost (OldTimeDelta covers
	// half a second)
	const FMove* Old = nullptr;
	{
		const FVector NewAccelDir = Newest.Accel.GetSafeNormal();
		for (int32 i = 0; i < SavedMoves.Num(); ++i)
		{
			const FMove& C = SavedMoves[i];
			if (!C.bSent || Newest.TimeStamp - C.TimeStamp >= 0.5f)
			{
				continue;
			}
			if (C.IsImportant())
			{
				Old = &C;
			}
			else if (!bLegacy && (!Old || !Old->IsImportant()))
			{
				const FVector Compare = Old ? Old->Accel.GetSafeNormal() : NewAccelDir;
				if (FVector::DotProduct(C.Accel.GetSafeNormal(), Compare) < 0.95f)
				{
					Old = &C;
				}
			}
		}
	}
	int32 i = 0;
	while (i < Unsent.Num())
	{
		const int32 Left = Unsent.Num() - i;
		if (Left >= 2)
		{
			FMove& A = SavedMoves[Unsent[i]];
			FMove& B = SavedMoves[Unsent[i + 1]];
			CallServerMove(&A, B, Old);
			A.bSent = B.bSent = true;
			i += 2;
		}
		else
		{
			FMove& A = SavedMoves[Unsent[i]];
			CallServerMove(nullptr, A, Old);
			A.bSent = true;
			i += 1;
		}
	}
	LastSendTime = Newest.TimeStamp;
}

void FUE2NetPlay::CallServerMove(const FMove* Pending, const FMove& Move, const FMove* Old)
{
	using namespace UE2NetPlayLocal;
	const int32 Ch = PCChannel();
	// the step's acceleration (AccelRate, 2048 UU/s^2, at full input), sent * 10; SavedMove.SetMoveFor caps it at 3072.
	// The server's walking physics adds Acceleration * DeltaTime as it is, so it must be the one we simulated.
	auto AccelUU = [](const FMove& M) -> FVector
	{
		const FVector A = M.Accel / S;
		return A.IsNearlyZero() ? FVector::ZeroVector : A.GetClampedToMaxSize(3072.f);
	};
	const FVector ClientLoc = Move.EndLoc / S + FVector(0.f, 0.f, ZOffset + Move.SlopeZ + (bProbe ? 500.f : 0.f));
	const int32 View = ViewInt(Move.View);
	int32 OldTimeDelta = 0, OldAccel = 0;
	if (Old && Old != &Move && Old != Pending)
	{
		OldTimeDelta = FMath::Min(255, int32((Move.TimeStamp - Old->TimeStamp) * 500.f));
		const FVector Build = 0.05f * AccelUU(*Old) + FVector(0.5f);
		OldAccel = (CompressAccel(int32(Build.X)) << 23) + (CompressAccel(int32(Build.Y)) << 15) + (CompressAccel(int32(Build.Z)) << 7);
		OldAccel += (Old->In.bRun ? 64 : 0) + (Old->In.bDoubleJump ? 32 : 0) + (Old->In.bPressedJump ? 16 : 0) + Old->In.DoubleClickMove;
		if (OldTimeDelta == 0)
		{
			OldAccel = 0;
		}
	}
	++PacketsWithMoves;
	if (Pending)
	{
		const bool bPendingJumpStatus = Pending->bJumpStatus;
		const int32 Compress = (Pending->In.bRun ? 1 : 0) | (Pending->In.bDuck ? 2 : 0) | (bPendingJumpStatus ? 4 : 0) | (Pending->In.bDoubleJump ? 8 : 0)
			| (Move.In.bRun ? 16 : 0) | (Move.In.bDuck ? 32 : 0) | (Move.bJumpStatus ? 64 : 0) | (Move.In.bDoubleJump ? 128 : 0);
		TArray<FUE2NetValue> P = { VFloat(Pending->TimeStamp), VVec(AccelUU(*Pending) * 10.f), VByte(Compress), VByte(Pending->In.DoubleClickMove), VInt(ViewInt(Pending->View)),
			VFloat(Move.TimeStamp), VVec(AccelUU(Move) * 10.f), VVec(ClientLoc.IsNearlyZero() ? FVector(0.1f, 0.f, 0.f) : ClientLoc), VByte(Move.In.DoubleClickMove), VByte(0), VInt(View) };
		if (OldTimeDelta)
		{
			P.Add(VByte(OldTimeDelta));
			P.Add(VInt(OldAccel));
		}
		Send(Ch, TEXT("DualServerMove"), P);
		MovesSent += 2;
		++DualMoves;
		return;
	}
	const FVector InAccel = AccelUU(Move) * 10.f;
	if (OldTimeDelta == 0 && InAccel.IsNearlyZero() && Move.In.DoubleClickMove == 0 && !Move.In.bDoubleJump)
	{
		Send(Ch, TEXT("ShortServerMove"), { VFloat(Move.TimeStamp), VVec(ClientLoc), VBool(Move.In.bRun), VBool(Move.In.bDuck), VBool(Move.bJumpStatus), VByte(0), VInt(View) });
		++ShortMoves;
	}
	else
	{
		TArray<FUE2NetValue> P = { VFloat(Move.TimeStamp), VVec(InAccel), VVec(ClientLoc), VBool(Move.In.bRun), VBool(Move.In.bDuck), VBool(Move.bJumpStatus),
			VBool(Move.In.bDoubleJump), VByte(Move.In.DoubleClickMove), VByte(0), VInt(View) };
		if (OldTimeDelta)
		{
			P.Add(VByte(OldTimeDelta));
			P.Add(VInt(OldAccel));
		}
		Send(Ch, TEXT("ServerMove"), P);
	}
	++MovesSent;
}

void FUE2NetPlay::SendSpectatorMove(float DeltaSeconds)
{
	using namespace UE2NetPlayLocal;
	AArenaPlayerController* PC = GetPC();
	const int32 Ch = PCChannel();
	if (!PC || !Ch)
	{
		return;
	}
	// only the free camera: following a player, the server's view target already decides what is relevant
	if (Cast<APawn>(PC->GetViewTarget()) && PC->GetViewTarget() != PC->GetSpectatorPawn())
	{
		return;
	}
	const float Now = float(World->GetTimeSeconds());
	static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoSpectatorMoves")); // test hook: the step 3 behaviour
	if (Now - LastSendTime < 0.0222f || bDisabled)
	{
		return;
	}
	const float Delta = FMath::Min(Now - FMath::Max(LastSendTime, LastTimeStamp), 0.125f);
	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector Target = CamLoc / S;
	if (!bServerSpecKnown)
	{
		// the server corrects us with where its spectator is
		ServerSpecLoc = Target;
	}
	// UE2 BaseSpectating.ProcessMove: the server moves its spectator SpectateSpeed * Normal(Acceleration) * DeltaTime
	const FVector ToTarget = Target - ServerSpecLoc;
	const float Step = 600.f * Delta;
	FVector Accel = FVector::ZeroVector;
	if (ToTarget.Size() > FMath::Max(Step * 0.5f, 2.f))
	{
		Accel = ToTarget.GetSafeNormal() * 30720.f;
		ServerSpecLoc += ToTarget.GetSafeNormal() * Step;
	}
	LastSendTime = Now;
	LastTimeStamp = Now;
	if (Accel.IsZero())
	{
		Send(Ch, TEXT("ShortServerMove"), { VFloat(Now), VVec(ServerSpecLoc), VBool(false), VBool(false), VBool(bJumpStatus), VByte(0), VInt(ViewInt(CamRot)) });
	}
	else
	{
		Send(Ch, TEXT("ServerMove"), { VFloat(Now), VVec(Accel), VVec(ServerSpecLoc), VBool(false), VBool(false), VBool(bJumpStatus), VBool(false), VByte(0), VByte(0),
			VInt(ViewInt(CamRot)) });
	}
	++MovesSent;
}

// -------------------------------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------------------------------

bool FUE2NetPlay::HandleInput(APlayerController* InPC, EInput Input, int32 Value)
{
	using namespace UE2NetPlayLocal;
	AArenaPlayerController* PC = Cast<AArenaPlayerController>(InPC);
	if (!PC || PC->GetWorld() != World.Get() || !IsPlaying())
	{
		return false;
	}
	if (PC->IsPauseMenuOpen() && (Input == EInput::Fire || Input == EInput::AltFire))
	{
		return false;
	}
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	switch (Input)
	{
	case EInput::Fire:
	case EInput::AltFire:
	{
		const uint8 Mode = Input == EInput::Fire ? 0 : 1;
		if (WarheadChannel && WarheadChannel == OwnPawnChannel)
		{
			// RedeemerWarhead.Fire: ServerBlowUp
			if (Send(WarheadChannel, TEXT("ServerBlowUp"), {}))
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ServerBlowUp"));
			}
			return true;
		}
		if (!Pawn && (IsWaitingForRound() || IsHeldAsSpectator()))
		{
			// the server only spawns us at the next round: its Spectating controls (Fire: next player, Alt-Fire: free camera)
			FUE2NetWorld* NW = FUE2NetWorld::Get();
			return NW && NW->SpectatorFire(PC, Mode == 1);
		}
		if (!Pawn || (ServerState == TEXT("PlayerWaiting") && !CurrentWeaponChannel))
		{
			// dead or waiting: fire restarts (UE2 Dead.Fire / PlayerWaiting.Fire -> ServerReStartPlayer). The server can
			// leave us a pawn without weapons while its controller waits (a player agreement answered after the match
			// began): PlayerWaiting.Fire restarts then too, and RestartPlayer gives the inventory
			if (Mode == 0 || ServerState == TEXT("Dead"))
			{
				RequestRestart();
			}
			return true;
		}
		if (DrivingChannel)
		{
			// UE2 Vehicle.Fire / AltFire -> VehicleFire(bWasAltFire) on the vehicle (or gunner seat) we drive
			bFireHeld[Mode] = true;
			Send(DrivingChannel, TEXT("VehicleFire"), { VBool(Mode == 1) });
			++FireCommands;
			return true;
		}
		bFireHeld[Mode] = true;
		if (FUE2NetScriptHUD* SH = FUE2NetScriptHUD::Get(); SH && CurrentWeaponChannel && SH->IsScriptWeapon(CurrentWeaponChannel)
			&& Cast<AWeap_UE2Script>(Pawn->GetCurrentWeapon()))
		{
			// a mod weapon running as script: its ClientStartFire (it calls the server itself)
			SH->ScriptWeaponCommand(CurrentWeaponChannel, FUE2NetScriptHUD::EWeaponCommand::StartFire, Mode);
			++FireCommands;
			return true;
		}
		NextFireEffect[Mode] = World.IsValid() ? FMath::Max(NextFireEffect[Mode], float(World->GetTimeSeconds())) : 0.f;
		if (CurrentWeaponChannel && !bNoNewNet && NewNet->IsNewNetWeapon(CurrentWeaponChannel))
		{
			// NewNet_ClientStartFire: sent once the weapon is ready (TickNewNetFire, this frame when it is)
			bNewNetFirePending[Mode] = true;
			TickNewNetFire();
		}
		else if (CurrentWeaponChannel)
		{
			Send(CurrentWeaponChannel, TEXT("ServerStartFire"), { VByte(Mode) });
			++FireCommands;
		}
		return true;
	}
	case EInput::FireRelease:
	case EInput::AltFireRelease:
	{
		const uint8 Mode = Input == EInput::FireRelease ? 0 : 1;
		if (bFireHeld[Mode] && DrivingChannel)
		{
			bFireHeld[Mode] = false;
			Send(DrivingChannel, TEXT("VehicleCeaseFire"), { VBool(Mode == 1) });
			return true;
		}
		bNewNetFirePending[Mode] = false;
		if (FUE2NetScriptHUD* SH = FUE2NetScriptHUD::Get(); bFireHeld[Mode] && SH && CurrentWeaponChannel && SH->IsScriptWeapon(CurrentWeaponChannel)
			&& LocalPawn.IsValid() && Cast<AWeap_UE2Script>(LocalPawn->GetCurrentWeapon()))
		{
			bFireHeld[Mode] = false;
			SH->ScriptWeaponCommand(CurrentWeaponChannel, FUE2NetScriptHUD::EWeaponCommand::StopFire, Mode);
			return true;
		}
		if (bFireHeld[Mode])
		{
			bFireHeld[Mode] = false;
			if (CurrentWeaponChannel)
			{
				Send(CurrentWeaponChannel, TEXT("ServerStopFire"), { VByte(Mode) });
			}
		}
		return true;
	}
	case EInput::NextWeapon: CycleWeapon(+1); return true;
	case EInput::PrevWeapon: CycleWeapon(-1); return true;
	case EInput::Slot: SelectGroup(Value == 0 ? 10 : Value); return true;
	case EInput::Use: Use(); return true;
	case EInput::Suicide: Suicide(); return true;
	default: return false;
	}
}

void FUE2NetPlay::Mutate(const FString& Text)
{
	Send(PCChannel(), TEXT("ServerMutate"), { UE2NetPlayLocal::VStr(Text) });
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ServerMutate %s"), *Text);
}

void FUE2NetPlay::RequestRestart()
{
	Send(PCChannel(), TEXT("ServerReStartPlayer"), {});
	RestartsRequested = Spawns + 1;
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ServerReStartPlayer (state %s)"), *ServerState.ToString());
}

void FUE2NetPlay::Use()
{
	Send(PCChannel(), TEXT("ServerUse"), {});
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ServerUse"));
}

void FUE2NetPlay::Suicide()
{
	Send(PCChannel(), TEXT("Suicide"), {});
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: Suicide"));
}

void FUE2NetPlay::ChangeTeam(int32 Team)
{
	if (Team < 0)
	{
		// PlayerController.SwitchTeam: the other team
		const AArenaPlayerController* PC = GetPC();
		const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		Team = PS && PS->Team == 0 ? 1 : 0;
	}
	Send(PCChannel(), TEXT("ServerChangeTeam"), { UE2NetPlayLocal::VInt(Team) });
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: ServerChangeTeam(%d)"), Team);
}

void FUE2NetPlay::Ready()
{
	// a mod's ready-up (UTComp warmup: UTComp_PRI.Ready) replaces the stock one, as its controller's exec Ready does
	if (ModClient->SetReady(true, OwnPRIChannel))
	{
		return;
	}
	// UnrealPlayer.ServerSetReadyToStart, then (PlayerWaiting) ServerReStartPlayer: ready while the match waits
	Send(PCChannel(), TEXT("ServerSetReadyToStart"), {});
	if (!LocalPawn.IsValid())
	{
		RequestRestart();
	}
}

void FUE2NetPlay::NotReady()
{
	if (!ModClient->SetReady(false, OwnPRIChannel))
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: NotReady: the server has no not-ready call"));
	}
}

FString FUE2NetPlay::GetModStatusText() const
{
	return ModClient ? ModClient->DescribeWarmup() : FString();
}

void FUE2NetPlay::Say(const FString& Text, bool bTeam)
{
	const FString Msg = Text.Left(128);
	if (Msg.IsEmpty())
	{
		return;
	}
	Send(PCChannel(), bTeam ? TEXT("ServerTeamSay") : TEXT("ServerSay"), { UE2NetPlayLocal::VStr(Msg) });
}

void FUE2NetPlay::DoCombo(FUSClass* ComboClass)
{
	FUE2NetValue V;
	V.Kind = FUE2NetValue::EKind::Object;
	V.Obj.PackageIndex = Connection->GetPackageMap().FindPackage(ComboClass->Linker ? ComboClass->Linker->PackageName.ToString() : FString(TEXT("XGame")));
	V.Obj.ExportIndex = ComboClass->ExportIndex;
	Send(PCChannel(), TEXT("ServerDoCombo"), { V });
}

FString FUE2NetPlay::DescribeScore() const
{
	const AArenaPlayerController* PC = GetPC();
	const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	return PS ? FString::Printf(TEXT("; our score %.0f, deaths %d"), PS->GetScore(), PS->Deaths) : FString();
}

FString FUE2NetPlay::DescribeState() const
{
	const AUE2NetLocalPawn* P = LocalPawn.Get();
	if (!P)
	{
		return FString::Printf(TEXT("no pawn (server state %s)%s"), *ServerState.ToString(), *DescribeScore());
	}
	const FWeaponInfo* W = Weapons.Find(CurrentWeaponChannel);
	if (DrivingChannel)
	{
		const AActor* V = P->DrivenVehicle.Get();
		return FString::Printf(TEXT("driving channel %d at %s, speed %.0f UU/s%s"), DrivingChannel, V ? *(V->GetActorLocation() / UE2NetPlayLocal::S).ToCompactString() : TEXT("?"),
			V ? V->GetVelocity().Size() / UE2NetPlayLocal::S : 0.0, *DescribeScore());
	}
	return FString::Printf(TEXT("health %.0f shield %.0f adrenaline %.0f weapon %s ammo %d (%d weapons)%s"), P->Health, P->Shield, P->Adrenaline,
		W ? *W->ClassPath : TEXT("none"), W ? W->Ammo : 0, Weapons.Num(), *DescribeScore());
}

// -------------------------------------------------------------------------------------------------
// Tick, test script
// -------------------------------------------------------------------------------------------------

void FUE2NetPlay::Tick(float DeltaSeconds)
{
	const double Now = FPlatformTime::Seconds();
	if (IsPlaying() && Connection->GetState() == FUE2NetConnection::EState::Joined && !bReadySent && PCChannel() && Now - StartTime > 1.0)
	{
		// UnrealPlayer.bReadyToStart (a UT2004 client sets it when its login menu closes), then join the match
		bReadySent = true;
		Send(PCChannel(), TEXT("ServerSetReadyToStart"), {});
		RequestRestart();
	}
	if (OwnPawnChannel && !LocalPawn.IsValid())
	{
		TrySpawnLocalPawn();
	}
	// no weapon brought up yet (ClientWeaponSet missed): the best one, like UE2 Pawn.SwitchToBestWeapon
	if (LocalPawn.IsValid() && !Weapons.Contains(CurrentWeaponChannel) && Weapons.Num() > 0 && Now - LastSpawnRealTime > 0.5)
	{
		const FWeaponInfo* Best = nullptr;
		for (const TPair<int32, FWeaponInfo>& W : Weapons)
		{
			if (!Best || W.Value.Priority > Best->Priority)
			{
				Best = &W.Value;
			}
		}
		CurrentWeaponChannel = 0;
		SwitchToWeapon(Best->Channel);
	}
	if (AUE2NetLocalPawn* Pawn = LocalPawn.Get(); Pawn && World.IsValid() && Now >= NextIgnoreRefresh)
	{
		// the other players are shown where the server last said (smoothed): our pawn does not bump into those copies
		// (nor into bodies); the server's own pawn collisions come back as corrections
		NextIgnoreRefresh = Now + 0.5;
		for (TActorIterator<AUE2NetPawn> It(World.Get()); It; ++It)
		{
			Pawn->GetCapsuleComponent()->IgnoreActorWhenMoving(*It, true);
		}
	}
	// test hook: respawn while hunting
	if ((bScriptHunt || !ScriptRouteKind.IsEmpty()) && !LocalPawn.IsValid() && OwnPawnChannel == 0 && Spawns > 0 && Now - LastRestartRequest > 2.5)
	{
		LastRestartRequest = Now;
		RequestRestart();
		if (!ScriptRouteKind.IsEmpty() && !bRouteRequeued)
		{
			// a new route from wherever we spawn next
			bRouteRequeued = true;
			Script.Add({ Now + 2.0, TEXT("route=") + ScriptRouteKind });
		}
	}
	TickScript(Now);
	if (Now >= NextSummary)
	{
		NextSummary = Now + 30.0;
		LogSummary();
	}
}

void FUE2NetPlay::LogSummary()
{
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay summary (%.0f s): moves sent %d (dual %d, short %d, packets %d); adjustments %d: corrections %d (average %.2f UU, max %.1f UU), already matching %d, spectator %d; replayed moves %d; jump presses %d, double jumps %d, dodges %d, corrections within 1 s after one %d; floor height offsets learned %d; spawns %d, deaths %d, fire commands %d, weapon switches %d; vehicle entries %d, drive updates %d (%.0f s driving); %s"),
		FPlatformTime::Seconds() - StartTime, MovesSent, DualMoves, ShortMoves, PacketsWithMoves, Adjustments, Corrections, Corrections ? SumCorrection / Corrections : 0.0,
		MaxCorrection, IgnoredAdjustments, SpectatorAdjustments, ReplayedMoves, NumJumps, NumDoubleJumps, NumDodges, CorrectionsAfterSpecial, FloorOffsets.Num(), Spawns, Deaths,
		FireCommands, WeaponSwitches, VehicleEntries, DriveUpdates, DrivingTime, *DescribeState());
	if (const FString N = NewNet ? NewNet->Describe() : FString(); !N.IsEmpty())
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay summary: %s"), *N);
	}
	if (ModClient)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay summary: %s"), *ModClient->Describe());
	}
}

void FUE2NetPlay::PreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds)
{
	if (InWorld != World.Get())
	{
		return;
	}
	AUE2NetLocalPawn* Pawn = LocalPawn.Get();
	AArenaPlayerController* PC = GetPC();
	if (!Pawn && PC && PC->GetSpectatorPawn() && (!ScriptMove.IsZero() || ScriptTurn != 0.f))
	{
		// scripted free camera (test hook)
		FRotator View = PC->GetControlRotation();
		View.Yaw += ScriptTurn * DeltaSeconds;
		PC->SetControlRotation(View);
		const FRotationMatrix R(FRotator(0.f, View.Yaw, 0.f));
		PC->GetSpectatorPawn()->AddMovementInput(R.GetUnitAxis(EAxis::X), ScriptMove.Y);
		PC->GetSpectatorPawn()->AddMovementInput(R.GetUnitAxis(EAxis::Y), ScriptMove.X);
		return;
	}
	if (PC && NetWarhead.IsValid() && ScriptTurn != 0.f)
	{
		// scripted steering of a guided warhead (test hook)
		FRotator View = PC->GetControlRotation();
		View.Yaw += ScriptTurn * DeltaSeconds;
		PC->SetControlRotation(View);
		return;
	}
	if (!Pawn || !PC || PC->GetPawn() != Pawn)
	{
		return;
	}
	// scripted input (test hook): movement keys, turning, the route
	if (bScriptDoubleJump && Pawn->GetCharacterMovement()->IsFalling() && FMath::Abs(Pawn->GetCharacterMovement()->Velocity.Z) < 150.f)
	{
		bScriptDoubleJump = false;
		Pawn->Jump();
		Pawn->StopJumping();
	}
	FRotator View = PC->GetControlRotation();
	if (ScriptTurn != 0.f)
	{
		View.Yaw += ScriptTurn * DeltaSeconds;
		PC->SetControlRotation(View);
	}
	FVector2D Move = ScriptMove;
	FVector RouteDir = FVector::ZeroVector;
	if (bScriptRoute && ScriptRoute.Num() > 0)
	{
		const FVector Loc = Pawn->GetActorLocation();
		FVector To = ScriptRoute[ScriptRouteIndex] - Loc;
		// goto=vehicle: close enough to get in (UE2 ServerUse finds vehicles within VehicleCheckRadius)
		if (ScriptVehicleChannel)
		{
			FUE2NetWorld* NW = FUE2NetWorld::Get();
			const AActor* V = NW ? NW->GetNativeActor(ScriptVehicleChannel) : nullptr;
			// UE2 Vehicle.FindEntryVehicle: within EntryRadius (about 100-200 UU) of the entry point; walk right up to it,
			// and Use every 1.5 s until the server puts us in
			if (V && FVector::Dist2D(V->GetActorLocation(), Loc) < 400.f && InWorld->GetTimeSeconds() - ScriptLastUseTime > 1.5)
			{
				ScriptLastUseTime = InWorld->GetTimeSeconds();
				Use();
			}
			if (V && FVector::Dist2D(V->GetActorLocation(), Loc) < 400.f)
			{
				RouteDirOverride = (V->GetActorLocation() - Loc).GetSafeNormal2D();
			}
			if (DrivingChannel)
			{
				bScriptRoute = false;
				ScriptRouteGoalStop = false;
				ScriptVehicleChannel = 0;
			}
		}
		// stuck on something (the map's collision differs a little, or a ledge): the next way point, with a jump
		if (FVector::Dist2D(Loc, ScriptLastProgressLoc) > 40.f)
		{
			ScriptLastProgressLoc = Loc;
			ScriptLastProgressTime = InWorld->GetTimeSeconds();
		}
		else if (InWorld->GetTimeSeconds() - ScriptLastProgressTime > 1.5)
		{
			// a jump first; then back off sideways and take the next way point
			ScriptLastProgressTime = InWorld->GetTimeSeconds();
			if (++ScriptStuckCount >= 2)
			{
				ScriptStuckCount = 0;
				ScriptRouteIndex = (ScriptRouteIndex + 1) % ScriptRoute.Num();
				ScriptBackOffUntil = InWorld->GetTimeSeconds() + 0.6;
				ScriptBackOffDir = FVector::CrossProduct(To.GetSafeNormal2D(), FVector::UpVector) * (FMath::RandBool() ? 1.f : -1.f) - To.GetSafeNormal2D();
				To = ScriptRoute[ScriptRouteIndex] - Loc;
			}
			else
			{
				Pawn->Jump();
				Pawn->StopJumping();
			}
		}
		if (To.Size2D() < 90.f)
		{
			if (ScriptRouteIndex + 1 >= ScriptRoute.Num() && ScriptRouteGoalStop && !ScriptVehicleChannel)
			{
				// goto: arrived
				bScriptRoute = false;
				ScriptRouteGoalStop = false;
			}
			else if (ScriptRouteIndex + 1 >= ScriptRoute.Num() && ScriptVehicleChannel)
			{
				// at the vehicle: keep walking into it (above) until we are in
			}
			else if (ScriptRouteIndex + 1 >= ScriptRoute.Num() && ScriptFlagStage > 0)
			{
				// CTF: at the enemy flag, plan the way home from here (and back out again after a capture)
				PlanFlagLeg(Pawn, ScriptFlagStage == 1 ? 2 : 1);
			}
			else
			{
				ScriptRouteIndex = (ScriptRouteIndex + 1) % ScriptRoute.Num();
			}
			To = ScriptRoute[ScriptRouteIndex] - Loc;
		}
		RouteDir = InWorld->GetTimeSeconds() < ScriptBackOffUntil ? ScriptBackOffDir.GetSafeNormal2D() : To.GetSafeNormal2D();
		if (!RouteDirOverride.IsZero())
		{
			RouteDir = RouteDirOverride;
			RouteDirOverride = FVector::ZeroVector;
		}
		View.Yaw = To.Rotation().Yaw;
		View.Pitch = 0.f;
		PC->SetControlRotation(View);
	}
	// hunting (test hook): aim at the nearest visible opponent and hold fire
	if (bScriptHunt)
	{
		const AUE2NetPawn* Best = nullptr;
		float BestDist = 4000.f;
		const FVector Eye = Pawn->GetPawnViewLocation();
		for (TActorIterator<AUE2NetPawn> It(InWorld); It; ++It)
		{
			if (It->IsNetDead() || It->IsHidden())
			{
				continue;
			}
			const float D = float(FVector::Dist(It->GetActorLocation(), Eye));
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetPlayHunt), false, Pawn);
			Q.AddIgnoredActor(*It);
			if (D < BestDist && !InWorld->LineTraceSingleByChannel(Hit, Eye, It->GetActorLocation(), ECC_Visibility, Q))
			{
				Best = *It;
				BestDist = D;
			}
		}
		if (Best && bScriptHuntTap)
		{
			// one press per shot: pressed when the weapon is ready, released once the start fire went out
			const FRotator Aim = (Best->GetActorLocation() - Eye).Rotation();
			View = FRotator(Aim.Pitch, Aim.Yaw, 0.f);
			PC->SetControlRotation(View);
			const double T = InWorld->GetTimeSeconds();
			if (!bFireHeld[0] && T >= NextFireEffect[0])
			{
				HandleInput(PC, EInput::Fire);
				ScriptTapRelease = T + 0.05;
			}
			else if (bFireHeld[0] && !bNewNetFirePending[0] && T >= ScriptTapRelease)
			{
				HandleInput(PC, EInput::FireRelease);
			}
		}
		else if (Best)
		{
			const FRotator Aim = (Best->GetActorLocation() - Eye).Rotation();
			View = FRotator(Aim.Pitch, Aim.Yaw, 0.f);
			PC->SetControlRotation(View);
			if (!bFireHeld[0])
			{
				HandleInput(PC, EInput::Fire);
			}
		}
		else if (bFireHeld[0])
		{
			HandleInput(PC, EInput::FireRelease);
		}
	}
	if (!RouteDir.IsZero())
	{
		Pawn->AddMovementInput(RouteDir, 1.f);
	}
	else if (!Move.IsZero())
	{
		const FRotationMatrix R(FRotator(0.f, View.Yaw, 0.f));
		Pawn->AddMovementInput(R.GetUnitAxis(EAxis::X), Move.Y);
		Pawn->AddMovementInput(R.GetUnitAxis(EAxis::Y), Move.X);
	}
}

void FUE2NetPlay::TickScript(double Now)
{
	while (Script.Num() > 0 && Now >= Script[0].Time)
	{
		const FString Cmd = Script[0].Cmd;
		Script.RemoveAt(0);
		FString Key = Cmd, Value;
		Cmd.Split(TEXT("="), &Key, &Value);
		Key = Key.ToLower();
		AUE2NetLocalPawn* Pawn = LocalPawn.Get();
		AArenaPlayerController* PC = GetPC();
		if (!Pawn && IsPlaying() && (Key == TEXT("route") || Key == TEXT("move") || Key == TEXT("goto")) && Now - StartTime < 600.0)
		{
			// not spawned yet (the match is starting): try again in a second
			Script.Add({ Now + 1.0, Cmd });
			Script.Sort([](const FScriptStep& A, const FScriptStep& B) { return A.Time < B.Time; });
			continue;
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: script %s (pawn %s at %s; %s)"), *Cmd, Pawn ? TEXT("yes") : TEXT("no"),
			Pawn ? *(Pawn->GetActorLocation() / UE2NetPlayLocal::S).ToCompactString() : TEXT("-"), *DescribeState());
		if (Key == TEXT("move"))
		{
			// move=<forward>,<right>
			FString F, R;
			Value.Split(TEXT(","), &F, &R);
			ScriptMove = FVector2D(FCString::Atof(*R), FCString::Atof(*F));
			bScriptRoute = false;
		}
		else if (Key == TEXT("stop"))
		{
			ScriptMove = FVector2D::ZeroVector;
			ScriptTurn = 0.f;
			bScriptRoute = false;
		}
		else if (Key == TEXT("turn"))
		{
			ScriptTurn = FCString::Atof(*Value);
		}
		else if (Key == TEXT("yaw") && PC)
		{
			FRotator V = PC->GetControlRotation();
			V.Yaw = FCString::Atof(*Value);
			PC->SetControlRotation(V);
		}
		else if (Key == TEXT("pitch") && PC)
		{
			FRotator V = PC->GetControlRotation();
			V.Pitch = FCString::Atof(*Value);
			PC->SetControlRotation(V);
		}
		else if (Key == TEXT("dj") && Pawn)
		{
			// a jump, then the double jump near the top (xPawn.DoJump: |Velocity.Z| < 100 UU/s)
			Pawn->Jump();
			Pawn->StopJumping();
			bScriptDoubleJump = true;
		}
		else if (Key == TEXT("jump") && Pawn)
		{
			Pawn->Jump();
			Pawn->StopJumping();
		}
		else if (Key == TEXT("dodge") && Pawn)
		{
			// a double tap: two taps of the direction key before the next physics step (UE2 PlayerInput)
			const EArenaDir Dir = Value == TEXT("b") ? EArenaDir::Back : Value == TEXT("l") ? EArenaDir::Left : Value == TEXT("r") ? EArenaDir::Right : EArenaDir::Forward;
			Pawn->OnDirectionTap(Dir);
			Script.Insert({ Now + 0.05, FString::Printf(TEXT("tap=%s"), *Value) }, 0);
		}
		else if (Key == TEXT("tap") && Pawn)
		{
			const EArenaDir Dir = Value == TEXT("b") ? EArenaDir::Back : Value == TEXT("l") ? EArenaDir::Left : Value == TEXT("r") ? EArenaDir::Right : EArenaDir::Forward;
			Pawn->OnDirectionTap(Dir);
		}
		else if (Key == TEXT("fire") || Key == TEXT("alt"))
		{
			HandleInput(PC, Key == TEXT("fire") ? EInput::Fire : EInput::AltFire);
		}
		else if (Key == TEXT("stopfire") || Key == TEXT("stopalt"))
		{
			HandleInput(PC, Key == TEXT("stopfire") ? EInput::FireRelease : EInput::AltFireRelease);
		}
		else if (Key == TEXT("next") || Key == TEXT("prev"))
		{
			HandleInput(PC, Key == TEXT("next") ? EInput::NextWeapon : EInput::PrevWeapon);
		}
		else if (Key == TEXT("slot"))
		{
			HandleInput(PC, EInput::Slot, FCString::Atoi(*Value));
		}
		else if (Key == TEXT("restart"))
		{
			RequestRestart();
		}
		else if (Key == TEXT("use"))
		{
			Use();
		}
		else if (Key == TEXT("suicide"))
		{
			Suicide();
		}
		else if (Key == TEXT("team"))
		{
			ChangeTeam(FCString::Atoi(*Value));
		}
		else if (Key == TEXT("ready"))
		{
			Ready();
		}
		else if (Key == TEXT("agree") || Key == TEXT("disagree"))
		{
			// the server's player agreement page (Net/UE2NetPlayerAgreement.h): "I agree!" / "I do not agree."
			ArenaNet::AnswerPlayerAgreement(Key == TEXT("agree"));
		}
		else if ((Key == TEXT("say") || Key == TEXT("teamsay")))
		{
			if (bScriptCanChat)
			{
				Say(Value, Key == TEXT("teamsay"));
			}
			else
			{
				UE_LOG(LogUE2Net, Warning, TEXT("UE2NetPlay: script chat is only allowed on a server on this machine"));
			}
		}
		else if (Key == TEXT("combo") && Pawn)
		{
			Pawn->ActivateCombo(Value == TEXT("berserk") ? EArenaCombo::Berserk : Value == TEXT("booster") ? EArenaCombo::Booster
				: Value == TEXT("invis") ? EArenaCombo::Invisible : EArenaCombo::Speed);
		}
		else if (Key == TEXT("route") && Pawn)
		{
			// route=pickups: every pickup of the map in nearest-neighbour order; route=flag: the enemy flag, then ours
			ScriptRoute.Reset();
			ScriptRouteIndex = 0;
			bScriptRoute = true;
			ScriptRouteKind = Value;
			bRouteRequeued = false;
			UWorld* W = World.Get();
			AArenaGameMode* GM = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
			AArenaNavGraph* Nav = GM ? GM->GetNavGraph() : nullptr;
			TArray<FVector> Goals;
			if (Value == TEXT("flag"))
			{
				const int32 MyTeam = PC && PC->GetPlayerState<AArenaPlayerState>() ? PC->GetPlayerState<AArenaPlayerState>()->Team : 0;
				for (TActorIterator<AArenaFlag> It(W); It; ++It)
				{
					if (It->GetTeam() != MyTeam)
					{
						Goals.Insert(It->GetHomeLocation(), 0);
					}
					else
					{
						Goals.Add(It->GetHomeLocation());
					}
				}
			}
			else
			{
				TArray<FVector> Items;
				for (TActorIterator<AArenaPickup> It(W); It; ++It)
				{
					Items.Add(It->GetActorLocation());
				}
				FVector From = Pawn->GetActorLocation();
				while (Items.Num() > 0 && Goals.Num() < 12)
				{
					int32 Best = 0;
					for (int32 i = 1; i < Items.Num(); ++i)
					{
						if (FVector::DistSquared(Items[i], From) < FVector::DistSquared(Items[Best], From))
						{
							Best = i;
						}
					}
					From = Items[Best];
					Goals.Add(From);
					Items.RemoveAt(Best);
				}
			}
			FVector From = Pawn->GetActorLocation();
			ScriptRouteLinks.Reset();
			ScriptFlagStage = 0;
			if (Value == TEXT("flag") && Goals.Num() >= 2 && Nav && PlanFlagLeg(Pawn, 1))
			{
				Goals.Reset();
			}
			if (Value == TEXT("flagreverse") && Goals.Num() >= 2 && Nav)
			{
				// to the enemy flag and back the same way (the way there is known to be walkable), then onto our flag
				TArray<FVector> Path;
				TArray<EArenaNavLinkType> Links;
				if (Nav->FindPath(From, Goals[0], Path, Links))
				{
					ScriptRoute = Path;
					for (int32 i = Path.Num() - 2; i >= 0; --i)
					{
						ScriptRoute.Add(Path[i]);
					}
					ScriptRoute.Add(From);
					ScriptRoute.Add(Goals.Last());
					Goals.Reset();
				}
			}
			for (const FVector& G : Goals)
			{
				TArray<FVector> Path;
				TArray<EArenaNavLinkType> Links;
				if (Nav && Nav->FindPath(From, G, Path, Links))
				{
					ScriptRoute.Append(Path);
				}
				else
				{
					ScriptRoute.Add(G);
				}
				From = G;
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: route %s: %d goals, %d way points (nav graph %s)"), *Value, Goals.Num(), ScriptRoute.Num(), Nav ? TEXT("yes") : TEXT("no"));
			ScriptStuckCount = 0;
		}
		else if (Key == TEXT("goto") && Pawn)
		{
			// goto=water (into the nearest water volume), goto=lift (onto the nearest mover), goto=vehicle:<class part>
			// (to the nearest free vehicle of that class, then Use it), goto=<x>,<y>,<z> (UE2 units)
			UWorld* W = World.Get();
			FVector Target = FVector::ZeroVector;
			bool bFound = false;
			const FVector From = Pawn->GetActorLocation();
			ScriptVehicleChannel = 0;
			if (Value == TEXT("water"))
			{
				// the nearest nav graph node inside a water volume (the pool's floor), else the nearest volume's centre
				AArenaGameMode* GM0 = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
				const AArenaNavGraph* Nav0 = GM0 ? GM0->GetNavGraph() : nullptr;
				for (TActorIterator<AArenaPhysicsVolume> It(W); It; ++It)
				{
					if (!It->bWaterVolume)
					{
						continue;
					}
					UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: water volume %s, bounds %s"), *It->GetName(), *(It->GetComponentsBoundingBox(true).ToString()));
					if (Nav0)
					{
						for (const FArenaNavNode& Node : Nav0->GetNodes())
						{
							if (It->EncompassesUE2(Node.Location) && (!bFound || FVector::Dist(Node.Location, From) < FVector::Dist(Target, From)))
							{
								Target = Node.Location;
								bFound = true;
							}
						}
					}
				}
				// deep water (a pawn swims when its centre is in the water): the deepest point whose floor lies at least
				// 100 UU under the surface, on a 64 UU grid around each water volume; its nav node goes first
				{
					bool bDeep = false;
					FVector Deep = FVector::ZeroVector;
					int32 WetFloors = 0;
					for (TActorIterator<AArenaPhysicsVolume> It(W); It; ++It)
					{
						if (!It->bWaterVolume)
						{
							continue;
						}
						const FBox B = It->Shape.Bounds;
						if (!It->Shape.IsValid())
						{
							continue;
						}
						const float Step = FMath::Max(64.f * UE2NetPlayLocal::S, float(FMath::Max(B.GetSize().X, B.GetSize().Y)) / 128.f);
						for (double X = B.Min.X; X <= B.Max.X; X += Step)
						{
							for (double Y = B.Min.Y; Y <= B.Max.Y; Y += Step)
							{
								const FVector Top(X, Y, B.Max.Z - 1.f);
								// the pawn's own collision (terrain and meshes that block only pawns count)
								FHitResult Hit;
								FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetPlayDeep), false, Pawn);
								FCollisionResponseParams Resp;
								const UCapsuleComponent* Cap = Pawn->GetCapsuleComponent();
								Cap->InitSweepCollisionParams(Q, Resp);
								if (!W->SweepSingleByChannel(Hit, Top, FVector(X, Y, B.Min.Z - 400.f), FQuat::Identity, Cap->GetCollisionObjectType(),
									FCollisionShape::MakeSphere(1.f), Q, Resp))
								{
									continue;
								}
								const FVector Swim = Hit.Location + FVector(0.f, 0.f, 100.f * UE2NetPlayLocal::S);
								if (It->EncompassesUE2(Hit.Location + FVector(0.f, 0.f, 10.f)))
								{
									++WetFloors;
								}
								if (It->EncompassesUE2(Swim) && It->EncompassesUE2(Hit.Location + FVector(0.f, 0.f, 10.f))
									&& (!bDeep || Swim.Z < Deep.Z - 1.f || (Swim.Z <= Deep.Z + 1.f && FVector::Dist(Swim, From) < FVector::Dist(Deep, From))))
								{
									Deep = Swim;
									bDeep = true;
								}
							}
						}
					}
					UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: goto water: %d floor points under water, deep enough: %s"), WetFloors, bDeep ? *(Deep / UE2NetPlayLocal::S).ToCompactString() : TEXT("none"));
					if (bDeep)
					{
						Target = Deep;
						bFound = true;
					}
				}
				if (!bFound)
				{
					for (TActorIterator<AArenaPhysicsVolume> It(W); It; ++It)
					{
						const FBox B = It->GetComponentsBoundingBox(true);
						if (It->bWaterVolume && B.IsValid && (!bFound || FVector::Dist(B.GetCenter(), From) < FVector::Dist(Target, From)))
						{
							Target = B.GetCenter();
							bFound = true;
						}
					}
				}
			}
			else if (Value == TEXT("lift"))
			{
				for (TActorIterator<AArenaMover> It(W); It; ++It)
				{
					const FBox B = It->GetComponentsBoundingBox(true);
					const FVector Top(B.GetCenter().X, B.GetCenter().Y, B.Max.Z + 100.f);
					if (B.IsValid && B.GetExtent().X > 60.f && B.GetExtent().Y > 60.f && (!bFound || FVector::Dist(Top, From) < FVector::Dist(Target, From)))
					{
						Target = Top;
						bFound = true;
					}
				}
			}
			else if (Value.StartsWith(TEXT("vehicle:")))
			{
				const FString Part = Value.Mid(8);
				FUE2NetWorld* NW = FUE2NetWorld::Get();
				for (const TPair<int32, FUE2NetActor>& KV : Connection->GetActors())
				{
					AActor* V = NW ? NW->GetNativeActor(KV.Key) : nullptr;
					if (V && KV.Value.ClassPath.Contains(Part) && UE2NetPlayLocal::ChainHas(KV.Value, TEXT("Vehicle")) && !UE2NetPlayLocal::GetRef(KV.Value, TEXT("Driver"))
						&& (!bFound || FVector::Dist(V->GetActorLocation(), From) < FVector::Dist(Target, From)))
					{
						Target = V->GetActorLocation();
						ScriptVehicleChannel = KV.Key;
						bFound = true;
					}
				}
			}
			else
			{
				TArray<FString> P;
				Value.ParseIntoArray(P, TEXT(","));
				if (P.Num() == 3)
				{
					Target = FVector(FCString::Atof(*P[0]), FCString::Atof(*P[1]), FCString::Atof(*P[2])) * UE2NetPlayLocal::S;
					bFound = true;
				}
			}
			ScriptRoute.Reset();
			ScriptRouteLinks.Reset();
			ScriptRouteIndex = 0;
			ScriptFlagStage = 0;
			ScriptRouteKind.Reset();
			if (bFound)
			{
				AArenaGameMode* GM = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
				AArenaNavGraph* Nav = GM ? GM->GetNavGraph() : nullptr;
				TArray<EArenaNavLinkType> Links;
				if (!Nav || !Nav->FindPath(From, Target, ScriptRoute, ScriptRouteLinks))
				{
					ScriptRoute = { Target };
				}
				// stop at the goal (no loop)
				ScriptRouteGoalStop = true;
				bScriptRoute = true;
			}
			ScriptGotoTarget = Target;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: goto %s: %s, %d way points"), *Value, bFound ? *(Target / UE2NetPlayLocal::S).ToCompactString() : TEXT("not found"), ScriptRoute.Num());
			for (int32 i = 0; i < ScriptRoute.Num(); ++i)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetPlay: goto way point %d: %s link %d"), i, *(ScriptRoute[i] / UE2NetPlayLocal::S).ToCompactString(), ScriptRouteLinks.IsValidIndex(i) ? int32(ScriptRouteLinks[i]) : -1);
			}
		}
		else if (Key == TEXT("face") && Pawn && PC)
		{
			// face the last goto target (e.g. into the water), level
			const FVector To = ScriptGotoTarget - Pawn->GetActorLocation();
			PC->SetControlRotation(FRotator(0.f, To.Rotation().Yaw, 0.f));
		}
		else if (Key == TEXT("aim") && Pawn && PC)
		{
			// look straight at the last goto target; aim=vehicle / aim=player: at the nearest vehicle / player shown (e.g.
			// one to lock on to, one to watch)
			FVector Target = ScriptGotoTarget;
			if (Value == TEXT("player"))
			{
				double Best = TNumericLimits<double>::Max();
				for (TActorIterator<AUE2NetPawn> It(World.Get()); It; ++It)
				{
					const double D = FVector::DistSquared(It->GetActorLocation(), Pawn->GetActorLocation());
					if (!It->IsNetDead() && D < Best)
					{
						Best = D;
						Target = It->GetActorLocation();
					}
				}
			}
			else if (Value == TEXT("vehicle"))
			{
				double Best = TNumericLimits<double>::Max();
				for (TActorIterator<AUE2NetVehicle> It(World.Get()); It; ++It)
				{
					const double D = FVector::DistSquared(It->GetActorLocation(), Pawn->GetActorLocation());
					if (D < Best)
					{
						Best = D;
						Target = It->GetActorLocation();
					}
				}
			}
			PC->SetControlRotation((Target - Pawn->GetPawnViewLocation()).Rotation());
		}
		else if (Key == TEXT("hunt"))
		{
			bScriptHunt = Value != TEXT("0");
			bScriptHuntTap = Value == TEXT("tap");
			if (!bScriptHunt && bFireHeld[0])
			{
				HandleInput(PC, EInput::FireRelease);
			}
		}
		else if (Key == TEXT("summary"))
		{
			LogSummary();
		}
#if !UE_BUILD_SHIPPING
		else if (Key == TEXT("exec") && PC)
		{
			// a console command (test runs: ModExec, ModMenuClick ...)
			PC->ConsoleCommand(Value.Replace(TEXT("+"), TEXT(" ")));
		}
#endif
	}
}

// =================================================================================================
// Console commands (typed by the player)
// =================================================================================================

namespace UE2NetPlayLocal
{
	// Say / TeamSay: AArenaPlayerController execs (ArenaChat::Send routes them to FUE2NetPlay::Say while playing here)
	FAutoConsoleCommand CmdSwitchTeam(TEXT("SwitchTeam"), TEXT("SwitchTeam [0|1]: change team on the UT2004 server (UE2 ServerChangeTeam)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (FUE2NetPlay* P = FUE2NetPlay::Get())
			{
				P->ChangeTeam(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1);
			}
		}));
	FAutoConsoleCommand CmdModExec(TEXT("ModExec"), TEXT("ModExec <command> [args]: run an exec command of the UT2004 server's mod code (its controller, pawn or weapon): a mod weapon's reload or special key, a mod's menu command."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			const FString Line = FString::Join(Args, TEXT(" "));
			if (!S || !S->ConsoleExec(Line))
			{
				UE_LOG(LogUE2Net, Log, TEXT("ModExec %s: no exec function of that name in the server's mod code"), *Line);
			}
		}));
	FAutoConsoleCommand CmdMutate(TEXT("Mutate"), TEXT("Mutate <text>: UT2004's Mutate console command (PlayerController.ServerMutate): mods' commands, e.g. UTComp's menu."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Line = FString::Join(Args, TEXT(" "));
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			if (S && S->ConsoleExec(TEXT("Mutate ") + Line))
			{
				return;
			}
			if (FUE2NetPlay* P = FUE2NetPlay::Get())
			{
				P->Mutate(Line);
			}
		}));
	FAutoConsoleCommand CmdReady(TEXT("Ready"), TEXT("Ready: tell the UT2004 server you are ready to start."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (FUE2NetPlay* P = FUE2NetPlay::Get())
			{
				P->Ready();
			}
		}));
	FAutoConsoleCommand CmdNotReady(TEXT("NotReady"), TEXT("NotReady: take back Ready during a mod's warmup (UTComp)."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (FUE2NetPlay* P = FUE2NetPlay::Get())
			{
				P->NotReady();
			}
		}));
}

namespace ArenaNet
{
	bool HandleNetPlayInput(APlayerController* PC, int32 Input, int32 Value)
	{
		FUE2NetPlay* P = FUE2NetPlay::Get();
		return P && PC && P->HandleInput(PC, FUE2NetPlay::EInput(Input), Value);
	}
}

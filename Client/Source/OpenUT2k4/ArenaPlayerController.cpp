#include "ArenaPlayerController.h"
#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaWeapon.h"
#include "ArenaTranslocator.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaUserSettings.h"
#include "ArenaAssets.h"
#include "ArenaArt.h"
#include "ArenaChat.h"
#include "ArenaDisplay.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/UE2NetWorld.h"
#include "Net/UE2NetPlay.h"

namespace ArenaPCLocal
{
	/** Kill lines: UT2004 console messages (xDeathMessage Lifetime, HUD ConsoleMessageCount). */
	float PCKillMessageLifetime() { return ArenaLocalMessages::GetStyle(EArenaLocalMessage::Death).Lifetime; }
	constexpr int32 PCMaxKillMessages = ArenaLocalMessages::ConsoleMessageCount;
	constexpr int32 PCMaxCenterMessages = 4;
	constexpr float PCDamageIndicatorLifetime = 1.5f;
	constexpr int32 PCMaxDamageIndicators = 8;
	/** Degrees of rotation per raw mouse count at sensitivity 1. */
	constexpr float PCMouseDegreesPerCount = 0.15f;
	/** Degrees per second at full stick deflection at sensitivity 1. */
	constexpr float PCGamepadYawRate = 200.f;
	constexpr float PCGamepadPitchRate = 140.f;

	UInputAction* PCMakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, FName(Name));
		Action->ValueType = Type;
		// Several actions share keys (W drives both Move and TapForward), so never swallow keys.
		Action->bConsumeInput = false;
		return Action;
	}

	FLinearColor PCPlayerColorFor(const APlayerState* PS, const APlayerState* Self, bool bTeamGame)
	{
		if (const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS))
		{
			if (bTeamGame && APS->Team >= 0)
			{
				return ArenaTeam::Color(APS->Team) * 0.5f + FLinearColor(0.5f, 0.5f, 0.5f, 0.5f);
			}
		}
		if (PS && PS == Self)
		{
			return FLinearColor(1.f, 0.85f, 0.25f);
		}
		return FLinearColor(0.92f, 0.94f, 1.f);
	}
}
using namespace ArenaPCLocal;

AArenaPlayerController::AArenaPlayerController()
{
	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
	// UE2 view shake (PlayerController.ShakeView / ViewShake): ArenaViewShake.h
	PlayerCameraManagerClass = AArenaPlayerCameraManager::StaticClass();
}

// ---------------------------------------------------------------------------------------------
// Input setup
// ---------------------------------------------------------------------------------------------

void AArenaPlayerController::CreateInputActions()
{
	if (MappingContext)
	{
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Arena"));
	UInputMappingContext* IMC = MappingContext;

	auto Add = [this](const TCHAR* Name, EInputActionValueType Type) -> UInputAction*
	{
		UInputAction* A = PCMakeAction(this, Name, Type);
		Actions.Add(FName(Name), A);
		return A;
	};

	auto Swizzle = [IMC]() -> UInputModifier*
	{
		UInputModifierSwizzleAxis* M = NewObject<UInputModifierSwizzleAxis>(IMC);
		M->Order = EInputAxisSwizzle::YXZ;
		return M;
	};
	auto Negate = [IMC]() -> UInputModifier*
	{
		return NewObject<UInputModifierNegate>(IMC);
	};
	auto DeadZone = [IMC]() -> UInputModifier*
	{
		UInputModifierDeadZone* M = NewObject<UInputModifierDeadZone>(IMC);
		M->LowerThreshold = 0.15f;
		return M;
	};

	// ---- movement ----
	UInputAction* Move = Add(TEXT("Move"), EInputActionValueType::Axis2D);
	{
		// D = +X (right), A = -X, W = +Y (forward), S = -Y
		IMC->MapKey(Move, EKeys::D);
		IMC->MapKey(Move, EKeys::A).Modifiers.Add(Negate());
		IMC->MapKey(Move, EKeys::W).Modifiers.Add(Swizzle());
		{
			FEnhancedActionKeyMapping& M = IMC->MapKey(Move, EKeys::S);
			M.Modifiers.Add(Swizzle());
			M.Modifiers.Add(Negate());
		}
		IMC->MapKey(Move, EKeys::Gamepad_Left2D).Modifiers.Add(DeadZone());
	}

	UInputAction* Look = Add(TEXT("Look"), EInputActionValueType::Axis2D);
	IMC->MapKey(Look, EKeys::Mouse2D);

	UInputAction* PadLook = Add(TEXT("GamepadLook"), EInputActionValueType::Axis2D);
	IMC->MapKey(PadLook, EKeys::Gamepad_Right2D).Modifiers.Add(DeadZone());

	UInputAction* Jump = Add(TEXT("Jump"), EInputActionValueType::Boolean);
	IMC->MapKey(Jump, EKeys::SpaceBar);
	IMC->MapKey(Jump, EKeys::Gamepad_FaceButton_Bottom);

	UInputAction* Crouch = Add(TEXT("Crouch"), EInputActionValueType::Boolean);
	IMC->MapKey(Crouch, EKeys::C);
	IMC->MapKey(Crouch, EKeys::LeftControl);
	IMC->MapKey(Crouch, EKeys::Gamepad_RightThumbstick);

	// UT2004 "Walking" (hold): WalkingPct speed, no walking off ledges
	UInputAction* Walk = Add(TEXT("Walk"), EInputActionValueType::Boolean);
	IMC->MapKey(Walk, EKeys::LeftShift);

	// Dodge key: dodge towards the held movement keys (same rules as a double tap)
	UInputAction* DodgeKey = Add(TEXT("Dodge"), EInputActionValueType::Boolean);
	IMC->MapKey(DodgeKey, EKeys::V);
	IMC->MapKey(DodgeKey, EKeys::Gamepad_FaceButton_Right);

	// ---- weapons ----
	UInputAction* Fire = Add(TEXT("Fire"), EInputActionValueType::Boolean);
	IMC->MapKey(Fire, EKeys::LeftMouseButton);
	IMC->MapKey(Fire, EKeys::Gamepad_RightTrigger);

	UInputAction* AltFire = Add(TEXT("AltFire"), EInputActionValueType::Boolean);
	IMC->MapKey(AltFire, EKeys::RightMouseButton);
	IMC->MapKey(AltFire, EKeys::Gamepad_LeftTrigger);

	UInputAction* NextWeapon = Add(TEXT("NextWeapon"), EInputActionValueType::Boolean);
	IMC->MapKey(NextWeapon, EKeys::MouseScrollUp);
	IMC->MapKey(NextWeapon, EKeys::Gamepad_RightShoulder);

	UInputAction* PrevWeapon = Add(TEXT("PrevWeapon"), EInputActionValueType::Boolean);
	IMC->MapKey(PrevWeapon, EKeys::MouseScrollDown);
	IMC->MapKey(PrevWeapon, EKeys::Gamepad_LeftShoulder);

	const FKey SlotKeys[10] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
		EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
	for (int32 i = 0; i < 10; ++i)
	{
		const FString Name = FString::Printf(TEXT("Slot%d"), i);
		UInputAction* SlotAction = Add(*Name, EInputActionValueType::Boolean);
		IMC->MapKey(SlotAction, SlotKeys[i]);
	}

	// ---- direction taps (dodge / combos) ----
	UInputAction* TapF = Add(TEXT("TapForward"), EInputActionValueType::Boolean);
	IMC->MapKey(TapF, EKeys::W);
	UInputAction* TapB = Add(TEXT("TapBack"), EInputActionValueType::Boolean);
	IMC->MapKey(TapB, EKeys::S);
	UInputAction* TapL = Add(TEXT("TapLeft"), EInputActionValueType::Boolean);
	IMC->MapKey(TapL, EKeys::A);
	UInputAction* TapR = Add(TEXT("TapRight"), EInputActionValueType::Boolean);
	IMC->MapKey(TapR, EKeys::D);

	// ---- misc ----
	UInputAction* Scoreboard = Add(TEXT("Scoreboard"), EInputActionValueType::Boolean);
	IMC->MapKey(Scoreboard, EKeys::Tab);
	IMC->MapKey(Scoreboard, EKeys::Gamepad_Special_Left);

	UInputAction* Pause = Add(TEXT("Pause"), EInputActionValueType::Boolean);
	IMC->MapKey(Pause, EKeys::Escape);
	IMC->MapKey(Pause, EKeys::Gamepad_Special_Right);

	UInputAction* Suicide = Add(TEXT("Suicide"), EInputActionValueType::Boolean);
	IMC->MapKey(Suicide, EKeys::K);

	UInputAction* Use = Add(TEXT("Use"), EInputActionValueType::Boolean);
	IMC->MapKey(Use, EKeys::E);
	IMC->MapKey(Use, EKeys::Gamepad_FaceButton_Left);

	UInputAction* TranslocatorKey = Add(TEXT("Translocator"), EInputActionValueType::Boolean);
	IMC->MapKey(TranslocatorKey, EKeys::Q);

	// chat (ArenaChat.h): DefUser.ini T=Talk, R=TeamTalk; Y also opens team chat (a common rebinding, unused otherwise)
	UInputAction* Talk = Add(TEXT("Talk"), EInputActionValueType::Boolean);
	IMC->MapKey(Talk, EKeys::T);
	UInputAction* TeamTalk = Add(TEXT("TeamTalk"), EInputActionValueType::Boolean);
	IMC->MapKey(TeamTalk, EKeys::R);
	IMC->MapKey(TeamTalk, EKeys::Y);

	UInputAction* MenuUp = Add(TEXT("MenuUp"), EInputActionValueType::Boolean);
	IMC->MapKey(MenuUp, EKeys::Up);
	IMC->MapKey(MenuUp, EKeys::Gamepad_DPad_Up);

	UInputAction* MenuDown = Add(TEXT("MenuDown"), EInputActionValueType::Boolean);
	IMC->MapKey(MenuDown, EKeys::Down);
	IMC->MapKey(MenuDown, EKeys::Gamepad_DPad_Down);

	UInputAction* Accept = Add(TEXT("Accept"), EInputActionValueType::Boolean);
	IMC->MapKey(Accept, EKeys::Enter);
	IMC->MapKey(Accept, EKeys::Gamepad_FaceButton_Bottom);

	UInputAction* MenuLeft = Add(TEXT("MenuLeft"), EInputActionValueType::Boolean);
	IMC->MapKey(MenuLeft, EKeys::Left);
	IMC->MapKey(MenuLeft, EKeys::Gamepad_DPad_Left);

	UInputAction* MenuRight = Add(TEXT("MenuRight"), EInputActionValueType::Boolean);
	IMC->MapKey(MenuRight, EKeys::Right);
	IMC->MapKey(MenuRight, EKeys::Gamepad_DPad_Right);

	// ---- quick sensitivity hotkeys ----
	UInputAction* SensDown = Add(TEXT("SensitivityDown"), EInputActionValueType::Boolean);
	IMC->MapKey(SensDown, EKeys::LeftBracket);
	UInputAction* SensUp = Add(TEXT("SensitivityUp"), EInputActionValueType::Boolean);
	IMC->MapKey(SensUp, EKeys::RightBracket);
}

void AArenaPlayerController::AddMappingContextToSubsystem()
{
	if (!MappingContext || !IsLocalController())
	{
		return;
	}
	if (ULocalPlayer* LP = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			// Adding the same context again only refreshes its priority.
			Subsystem->AddMappingContext(MappingContext, 0);
		}
	}
}

void AArenaPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	CreateInputActions();

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC)
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("AArenaPlayerController: InputComponent is not an UEnhancedInputComponent; check DefaultInput.ini"));
		return;
	}

	auto A = [this](const TCHAR* Name) -> UInputAction*
	{
		const TObjectPtr<UInputAction>* Found = Actions.Find(FName(Name));
		return Found ? Found->Get() : nullptr;
	};

	EIC->BindAction(A(TEXT("Move")), ETriggerEvent::Triggered, this, &AArenaPlayerController::OnMove);
	EIC->BindAction(A(TEXT("Look")), ETriggerEvent::Triggered, this, &AArenaPlayerController::OnLook);
	EIC->BindAction(A(TEXT("GamepadLook")), ETriggerEvent::Triggered, this, &AArenaPlayerController::OnGamepadLook);

	EIC->BindAction(A(TEXT("Jump")), ETriggerEvent::Started, this, &AArenaPlayerController::OnJumpPressed);
	EIC->BindAction(A(TEXT("Jump")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnJumpReleased);
	EIC->BindAction(A(TEXT("Crouch")), ETriggerEvent::Started, this, &AArenaPlayerController::OnCrouchPressed);
	EIC->BindAction(A(TEXT("Crouch")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnCrouchReleased);
	EIC->BindAction(A(TEXT("Walk")), ETriggerEvent::Started, this, &AArenaPlayerController::OnWalkPressed);
	EIC->BindAction(A(TEXT("Walk")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnWalkReleased);
	EIC->BindAction(A(TEXT("Dodge")), ETriggerEvent::Started, this, &AArenaPlayerController::OnDodgePressed);

	EIC->BindAction(A(TEXT("Fire")), ETriggerEvent::Started, this, &AArenaPlayerController::OnFirePressed);
	EIC->BindAction(A(TEXT("Fire")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnFireReleased);
	EIC->BindAction(A(TEXT("AltFire")), ETriggerEvent::Started, this, &AArenaPlayerController::OnAltFirePressed);
	EIC->BindAction(A(TEXT("AltFire")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnAltFireReleased);

	EIC->BindAction(A(TEXT("TapForward")), ETriggerEvent::Started, this, &AArenaPlayerController::OnTapForward);
	EIC->BindAction(A(TEXT("TapBack")), ETriggerEvent::Started, this, &AArenaPlayerController::OnTapBack);
	EIC->BindAction(A(TEXT("TapLeft")), ETriggerEvent::Started, this, &AArenaPlayerController::OnTapLeft);
	EIC->BindAction(A(TEXT("TapRight")), ETriggerEvent::Started, this, &AArenaPlayerController::OnTapRight);

	EIC->BindAction(A(TEXT("NextWeapon")), ETriggerEvent::Started, this, &AArenaPlayerController::OnNextWeapon);
	EIC->BindAction(A(TEXT("PrevWeapon")), ETriggerEvent::Started, this, &AArenaPlayerController::OnPrevWeapon);
	for (int32 i = 0; i < 10; ++i)
	{
		const FString Name = FString::Printf(TEXT("Slot%d"), i);
		EIC->BindAction(A(*Name), ETriggerEvent::Started, this, &AArenaPlayerController::OnSlot, i);
	}

	EIC->BindAction(A(TEXT("Scoreboard")), ETriggerEvent::Started, this, &AArenaPlayerController::OnScoreboardPressed);
	EIC->BindAction(A(TEXT("Scoreboard")), ETriggerEvent::Completed, this, &AArenaPlayerController::OnScoreboardReleased);
	EIC->BindAction(A(TEXT("Pause")), ETriggerEvent::Started, this, &AArenaPlayerController::OnPausePressed);
	EIC->BindAction(A(TEXT("Suicide")), ETriggerEvent::Started, this, &AArenaPlayerController::OnSuicide);
	EIC->BindAction(A(TEXT("Use")), ETriggerEvent::Started, this, &AArenaPlayerController::OnUse);
	EIC->BindAction(A(TEXT("Translocator")), ETriggerEvent::Started, this, &AArenaPlayerController::OnTranslocatorKey);
	EIC->BindAction(A(TEXT("Talk")), ETriggerEvent::Started, this, &AArenaPlayerController::Talk);
	EIC->BindAction(A(TEXT("TeamTalk")), ETriggerEvent::Started, this, &AArenaPlayerController::TeamTalk);
	EIC->BindAction(A(TEXT("MenuUp")), ETriggerEvent::Started, this, &AArenaPlayerController::OnMenuUp);
	EIC->BindAction(A(TEXT("MenuDown")), ETriggerEvent::Started, this, &AArenaPlayerController::OnMenuDown);
	EIC->BindAction(A(TEXT("Accept")), ETriggerEvent::Started, this, &AArenaPlayerController::OnMenuAccept);
	EIC->BindAction(A(TEXT("MenuLeft")), ETriggerEvent::Started, this, &AArenaPlayerController::OnMenuLeft);
	EIC->BindAction(A(TEXT("MenuRight")), ETriggerEvent::Started, this, &AArenaPlayerController::OnMenuRight);
	EIC->BindAction(A(TEXT("SensitivityDown")), ETriggerEvent::Started, this, &AArenaPlayerController::OnSensitivityDown);
	EIC->BindAction(A(TEXT("SensitivityUp")), ETriggerEvent::Started, this, &AArenaPlayerController::OnSensitivityUp);

	AddMappingContextToSubsystem();
}

void AArenaPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		CreateInputActions();
		AddMappingContextToSubsystem();
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
		UArenaUserSettings::Get()->ApplyAudio();
	}
}

void AArenaPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	CurrentFOV = 0.f; // snap to the desired FOV on the next tick
	if (IsLocalController() && !bPauseMenuOpen)
	{
		SetInputMode(FInputModeGameOnly());
	}
}

void AArenaPlayerController::OnUnPossess()
{
	Super::OnUnPossess();
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

AArenaCharacter* AArenaPlayerController::GetArenaCharacter() const
{
	return Cast<AArenaCharacter>(GetPawn());
}

bool AArenaPlayerController::IsPawnDead() const
{
	const AArenaCharacter* Char = GetArenaCharacter();
	if (Char)
	{
		return Char->IsDead();
	}
	return GetPawn() == nullptr;
}

bool AArenaPlayerController::IsPawnInputBlocked() const
{
	if (bPauseMenuOpen)
	{
		return true;
	}
	if (const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr)
	{
		if (GS->Phase == EArenaMatchPhase::Ended)
		{
			return true;
		}
	}
	return false;
}

bool AArenaPlayerController::IsScoreboardVisible() const
{
	if (bScoreboardHeld)
	{
		return true;
	}
	if (const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr)
	{
		return GS->Phase == EArenaMatchPhase::Ended;
	}
	return false;
}

float AArenaPlayerController::GetDesiredFOV() const
{
	return ArenaDisplay::GetCameraFOV(GetDesiredFOV43(), 16.f / 9.f);
}

float AArenaPlayerController::GetDesiredFOV43() const
{
	float Desired = FMath::Clamp(UArenaUserSettings::Get()->FOV, 60.f, 140.f);
	if (const AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			if (const AArenaWeapon* Weapon = Char->GetCurrentWeapon())
			{
				const float Zoom = Weapon->GetZoomFOV();
				if (Zoom > 0.f)
				{
					Desired = Zoom;
				}
			}
		}
	}
	return Desired;
}

void AArenaPlayerController::ExpireMessages()
{
	const float Now = GetWorld()->GetTimeSeconds();
	const float KillLife = PCKillMessageLifetime();
	KillMessages.RemoveAll([Now, KillLife](const FArenaKillMessage& M) { return Now - M.Time > KillLife; });
	CenterMessages.RemoveAll([Now](const FArenaCenterMessage& M) { return Now - M.Time > M.Duration; });
	LocalMessages.RemoveAll([Now](const FArenaCenterMessage& M) { return Now - M.Time > M.Duration; });
	DamageIndicators.RemoveAll([Now](const FDamageIndicator& D) { return Now - D.Time > PCDamageIndicatorLifetime; });
}

void AArenaPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	ExpireMessages();
	UpdateStartupMessages();
#if !UE_BUILD_SHIPPING
	TickTestMessages();
#endif

	// FOV: settings or weapon zoom, smoothly blended (in UT2004's 4:3 terms; converted per camera below)
	const float Desired = GetDesiredFOV43();
	if (CurrentFOV <= 0.f)
	{
		CurrentFOV = Desired;
	}
	else
	{
		CurrentFOV = FMath::FInterpTo(CurrentFOV, Desired, DeltaTime, 18.f);
		if (FMath::Abs(CurrentFOV - Desired) < 0.05f)
		{
			CurrentFOV = Desired;
		}
	}
	if (PlayerCameraManager)
	{
		// Views without a camera component (spectating) use the camera manager's DefaultFOV at its DefaultAspectRatio
		const float ManagerFOV = ArenaDisplay::GetCameraFOV(CurrentFOV, PlayerCameraManager->DefaultAspectRatio);
		PlayerCameraManager->DefaultFOV = ManagerFOV;
		PlayerCameraManager->SetFOV(ManagerFOV);
	}
	// The camera component's FOV wins over the camera manager's locked FOV, so set it too.
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->Camera)
		{
			Char->Camera->SetFieldOfView(ArenaDisplay::GetCameraFOV(CurrentFOV, Char->Camera->AspectRatio));
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Messages API
// ---------------------------------------------------------------------------------------------

void AArenaPlayerController::AddKillMessage(APlayerState* Killer, APlayerState* Victim, const FString& WeaponName, bool bHeadshot)
{
	const AArenaGameState* GS = GetWorld()->GetGameState<AArenaGameState>();
	const bool bTeamGame = GS && GS->IsTeamGame();

	FArenaKillMessage Msg;
	const bool bSuicide = (Killer == nullptr || Killer == Victim);
	Msg.Killer = bSuicide ? FString() : Killer->GetPlayerName();
	Msg.Victim = Victim ? Victim->GetPlayerName() : FString(TEXT("Someone"));
	Msg.Weapon = WeaponName;
	Msg.KillerColor = PCPlayerColorFor(bSuicide ? nullptr : Killer, PlayerState, bTeamGame);
	Msg.VictimColor = PCPlayerColorFor(Victim, PlayerState, bTeamGame);
	Msg.bHeadshot = bHeadshot;
	Msg.ConsoleText = ArenaLocalMessages::GetDeathString(Msg.Killer, Victim ? Victim->GetPlayerName() : FString(), WeaponName, bHeadshot);
	Msg.Time = GetWorld()->GetTimeSeconds();
	KillMessages.Add(Msg);
	while (KillMessages.Num() > PCMaxKillMessages)
	{
		KillMessages.RemoveAt(0);
	}

	// Personal confirmation when we score a frag (UT2004 xDeathMessage hands the killer xKillerMessagePlus)
	if (!bSuicide && Killer == PlayerState && Victim && Victim != PlayerState)
	{
		AddLocalMessage(EArenaLocalMessage::Killer, 0, Victim);
	}
}

void AArenaPlayerController::AddNamedKillMessage(const FString& KillerName, APlayerState* Victim, const FString& WeaponName, const FString& ConsoleText)
{
	const AArenaGameState* GS = GetWorld()->GetGameState<AArenaGameState>();
	const bool bTeamGame = GS && GS->IsTeamGame();

	FArenaKillMessage Msg;
	Msg.Killer = KillerName;
	Msg.Victim = Victim ? Victim->GetPlayerName() : FString(TEXT("Someone"));
	Msg.Weapon = WeaponName;
	Msg.KillerColor = PCPlayerColorFor(nullptr, PlayerState, bTeamGame);
	Msg.VictimColor = PCPlayerColorFor(Victim, PlayerState, bTeamGame);
	Msg.ConsoleText = ConsoleText;
	Msg.Time = GetWorld()->GetTimeSeconds();
	KillMessages.Add(Msg);
	while (KillMessages.Num() > PCMaxKillMessages)
	{
		KillMessages.RemoveAt(0);
	}
}

void AArenaPlayerController::AddConsoleLine(const FString& Text, const FColor& Color)
{
	if (Text.IsEmpty() || !GetWorld())
	{
		return;
	}
	FArenaKillMessage Msg;
	Msg.ConsoleText = Text;
	Msg.ConsoleColor = Color;
	Msg.Time = GetWorld()->GetTimeSeconds();
	KillMessages.Add(Msg);
	while (KillMessages.Num() > PCMaxKillMessages)
	{
		KillMessages.RemoveAt(0);
	}
}

void AArenaPlayerController::AddLocalMessage(EArenaLocalMessage Kind, int32 Switch, const APlayerState* Name1, const APlayerState* Name2)
{
	using namespace ArenaLocalMessages;
	if (Kind == EArenaLocalMessage::None || !GetWorld())
	{
		return;
	}
	const FStyle& Style = GetStyle(Kind);
	const bool bLocalIsName1 = Name1 && Name1 == PlayerState;
	FArenaCenterMessage Msg;
	Msg.Text = GetString(Kind, Switch, Name1 ? Name1->GetPlayerName() : FString(), Name2 ? Name2->GetPlayerName() : FString(), bLocalIsName1);
	if (Msg.Text.IsEmpty())
	{
		return;
	}
	Msg.Color = FLinearColor(Style.DrawColor);
	Msg.Duration = Style.Lifetime;
	Msg.Time = GetWorld()->GetTimeSeconds();
	Msg.Kind = Kind;
	Msg.Switch = Switch;
	Msg.FontSize = GetFontSize(Kind, Switch, bLocalIsName1, Name2 != nullptr);
	// HudBase.LocalizedMessage: a unique message takes the place of the one of its class, else a new slot (the oldest goes)
	const int32 Same = Style.bIsUnique ? LocalMessages.IndexOfByPredicate([Kind](const FArenaCenterMessage& M) { return M.Kind == Kind; }) : INDEX_NONE;
	if (Same != INDEX_NONE)
	{
		LocalMessages[Same] = Msg;
		return;
	}
	if (LocalMessages.Num() >= MaxLocalMessages)
	{
		LocalMessages.RemoveAt(0);
	}
	LocalMessages.Add(Msg);
}

void AArenaPlayerController::AddCustomLocalMessage(const FString& Text, const ArenaLocalMessages::FStyle& Style, int32 FontSize, FName ClassName)
{
	if (Text.IsEmpty() || !GetWorld())
	{
		return;
	}
	FArenaCenterMessage Msg;
	Msg.Text = Text;
	Msg.Color = FLinearColor(Style.DrawColor);
	Msg.Duration = Style.Lifetime;
	Msg.Time = GetWorld()->GetTimeSeconds();
	Msg.FontSize = FontSize;
	Msg.bCustomStyle = true;
	Msg.CustomStyle = Style;
	Msg.CustomClass = ClassName;
	// HudBase.LocalizedMessage: as AddLocalMessage, by class
	const int32 Same = Style.bIsUnique ? LocalMessages.IndexOfByPredicate([ClassName](const FArenaCenterMessage& M) { return M.bCustomStyle && M.CustomClass == ClassName; })
		: INDEX_NONE;
	if (Same != INDEX_NONE)
	{
		LocalMessages[Same] = Msg;
		return;
	}
	if (LocalMessages.Num() >= ArenaLocalMessages::MaxLocalMessages)
	{
		LocalMessages.RemoveAt(0);
	}
	LocalMessages.Add(Msg);
}

void AArenaPlayerController::TickTestMessages()
{
	// -AutoLocalMessages=<sec>: at that world time this player "kills" one bot and is "killed" by another 0.4 s later
	// (console kill lines, xKillerMessagePlus, xVictimMessage), for -AutoScreenshot checks of the message styles
	static float TestTime = -2.f;
	if (TestTime < -1.f)
	{
		TestTime = -1.f;
		FParse::Value(FCommandLine::Get(), TEXT("AutoLocalMessages="), TestTime);
	}
	const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	if (TestTime < 0.f || TestMessageStep >= 2 || !GS || !IsLocalController() || !PlayerState)
	{
		return;
	}
	TArray<APlayerState*> Others;
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (PS && PS != PlayerState)
		{
			Others.Add(PS);
		}
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (Others.Num() == 0 || Now < TestTime + 0.4f * TestMessageStep)
	{
		return;
	}
	if (TestMessageStep == 0)
	{
		AddKillMessage(PlayerState, Others[0], TEXT("Shard Cannon"), false);
	}
	else
	{
		APlayerState* Killer = Others[Others.Num() > 1 ? 1 : 0];
		AddKillMessage(Killer, PlayerState, TEXT("Rocket Launcher"), false);
		NotifyDied(Killer, TEXT("Rocket Launcher"));
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("AutoLocalMessages: step %d at %.2f"), TestMessageStep, Now);
	++TestMessageStep;
}

void AArenaPlayerController::UpdateStartupMessages()
{
	// UT2004 DeathMatch.PendingMatch: StartupMessage stage 5 - CountDown each second (beep), stage 5 when the match
	// starts ("Play" announcement). Driven by the replicated game state, so it shows on every client.
	const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	if (!GS || !IsLocalController())
	{
		return;
	}
	if (!bPhaseSeen)
	{
		// joined (or loaded) mid-match: nothing to announce for what already happened
		bPhaseSeen = true;
		LastPhaseSeen = GS->Phase;
		LastCountdownSeen = GS->Phase == EArenaMatchPhase::Countdown ? -1 : GS->CountdownRemaining;
	}
	if (GS->Phase == EArenaMatchPhase::Countdown)
	{
		const int32 N = GS->CountdownRemaining;
		if (N != LastCountdownSeen && N >= 1 && N <= 3)
		{
			AddLocalMessage(EArenaLocalMessage::Startup, 5 - N);
			// xPlayer.PlayBeepSound
			if (USoundBase* Beep = ArenaArt::LoadOneShotSound(TEXT("Sounds/MenuSounds/selectJ")))
			{
				UGameplayStatics::PlaySound2D(this, Beep);
			}
			else
			{
				ArenaAssets::PlaySound2D(this, TEXT("S_Countdown"));
			}
		}
		LastCountdownSeen = N;
	}
	else if (GS->Phase == EArenaMatchPhase::Playing && LastPhaseSeen == EArenaMatchPhase::Countdown)
	{
		AddLocalMessage(EArenaLocalMessage::Startup, 5);
		// PlayStatusAnnouncement('Play') with the stock status announcer (UnrealGame.FemaleAnnouncer: AnnouncerFemale2K4)
		if (USoundBase* Play = ArenaArt::LoadOneShotSound(TEXT("Sounds/AnnouncerFemale2K4/Generic_Play")))
		{
			UGameplayStatics::PlaySound2D(this, Play);
		}
		else
		{
			ArenaAssets::PlaySound2D(this, TEXT("S_Spree"));
		}
		LastCountdownSeen = -1;
	}
	LastPhaseSeen = GS->Phase;
}

void AArenaPlayerController::AddCenterMessage(const FString& Text, const FLinearColor& Color, float Duration)
{
	if (Text.IsEmpty())
	{
		return;
	}
	// Replace an identical message instead of stacking duplicates
	CenterMessages.RemoveAll([&Text](const FArenaCenterMessage& M) { return M.Text == Text; });

	FArenaCenterMessage Msg;
	Msg.Text = Text;
	Msg.Color = Color;
	Msg.Duration = FMath::Max(0.5f, Duration);
	Msg.Time = GetWorld()->GetTimeSeconds();
	CenterMessages.Add(Msg);
	while (CenterMessages.Num() > PCMaxCenterMessages)
	{
		CenterMessages.RemoveAt(0);
	}
}

void AArenaPlayerController::NotifyHitEnemy(float Damage, bool bHeadshot)
{
	const float Now = GetWorld()->GetTimeSeconds();
	// Multi-pellet weapons report many hits in one frame: only one sound per short window
	if (Now - LastHitEnemyTime > 0.06f || (bHeadshot && !bLastHitHeadshot))
	{
		ArenaAssets::PlaySound2D(this, TEXT("S_Hit"), 0.7f, bHeadshot ? 1.35f : 1.f);
	}
	LastHitEnemyTime = Now;
	bLastHitHeadshot = bHeadshot;
}

void AArenaPlayerController::NotifyTookDamage(const FVector& FromLocation, float Damage)
{
	FDamageIndicator Indicator;
	Indicator.From = FromLocation;
	Indicator.Time = GetWorld()->GetTimeSeconds();
	Indicator.Damage = Damage;
	DamageIndicators.Add(Indicator);
	while (DamageIndicators.Num() > PCMaxDamageIndicators)
	{
		DamageIndicators.RemoveAt(0);
	}
}

void AArenaPlayerController::NotifyPickup(const FString& ItemName)
{
	PickupMessage = ItemName;
	PickupMessageTime = GetWorld()->GetTimeSeconds();
}

FString AArenaPlayerController::GetPickupMessage(float& OutAge) const
{
	OutAge = GetWorld() ? GetWorld()->GetTimeSeconds() - PickupMessageTime : 1000.f;
	return PickupMessage;
}

void AArenaPlayerController::NotifyDied(APlayerState* Killer, const FString& WeaponName)
{
	// UT2004 xDeathMessage: the victim gets xVictimMessage; a suicide / environment death only has the console line
	if (Killer && Killer != PlayerState)
	{
		AddLocalMessage(EArenaLocalMessage::Victim, 0, Killer);
	}
	DamageIndicators.Reset();
}

// ---------------------------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------------------------

void AArenaPlayerController::OnMove(const FInputActionValue& Value)
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	APawn* P = GetPawn();
	if (!P || IsPawnDead())
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	// UE2 PlayerWalking moves along the yaw only; PlayerSwimming along the full view rotation.
	const ACharacter* Char = Cast<ACharacter>(P);
	const bool bSwimming = Char && Char->GetCharacterMovement() && Char->GetCharacterMovement()->IsSwimming();
	const FRotator YawRot = bSwimming ? GetControlRotation() : FRotator(0.f, GetControlRotation().Yaw, 0.f);
	const FRotationMatrix RotMatrix(YawRot);
	if (Axis.Y != 0.f)
	{
		P->AddMovementInput(RotMatrix.GetUnitAxis(EAxis::X), Axis.Y);
	}
	if (Axis.X != 0.f)
	{
		P->AddMovementInput(RotMatrix.GetUnitAxis(EAxis::Y), Axis.X);
	}
}

void AArenaPlayerController::OnLook(const FInputActionValue& Value)
{
	if (bPauseMenuOpen || IsLookInputIgnored())
	{
		return;
	}
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	const FVector2D Axis = Value.Get<FVector2D>();
	const float Scale = PCMouseDegreesPerCount * FMath::Max(0.01f, S->MouseSensitivity);
	// Applied straight to RotationInput so legacy input scale settings don't interfere.
	RotationInput.Yaw += Axis.X * Scale;
	RotationInput.Pitch += Axis.Y * Scale * (S->bInvertY ? -1.f : 1.f);
}

void AArenaPlayerController::OnGamepadLook(const FInputActionValue& Value)
{
	if (bPauseMenuOpen || IsLookInputIgnored())
	{
		return;
	}
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	const FVector2D Axis = Value.Get<FVector2D>();
	const float Dt = GetWorld()->GetDeltaSeconds();
	const float Sens = FMath::Max(0.01f, S->MouseSensitivity);
	RotationInput.Yaw += Axis.X * PCGamepadYawRate * Sens * Dt;
	// Right stick up should look up (non-inverted); the raw axis comes in the opposite sense to mouse Y
	RotationInput.Pitch -= Axis.Y * PCGamepadPitchRate * Sens * Dt * (S->bInvertY ? -1.f : 1.f);
}

void AArenaPlayerController::OnJumpPressed()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->Jump();
		}
	}
}

void AArenaPlayerController::OnJumpReleased()
{
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->StopJumping();
	}
}

void AArenaPlayerController::OnCrouchPressed()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->SetDuckInput(true);
		}
	}
}

void AArenaPlayerController::OnCrouchReleased()
{
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->SetDuckInput(false);
	}
}

void AArenaPlayerController::OnWalkPressed()
{
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->SetWalkInput(!IsPawnInputBlocked());
	}
}

void AArenaPlayerController::OnWalkReleased()
{
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->SetWalkInput(false);
	}
}

void AArenaPlayerController::OnDodgePressed()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->OnDodgeKey();
		}
	}
}

void AArenaPlayerController::OnFirePressed()
{
	if (ArenaNet::HandleSpectatorFire(this, false))
	{
		return; // spectating a UT2004 server: the server's spectator controls (Net/UE2NetWorld.h)
	}
	if (IsPawnInputBlocked())
	{
		return; // pause menu clicks are handled by HUD hit boxes
	}
	if (IsPawnDead())
	{
		if (AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>())
		{
			GM->RequestRespawn(this);
		}
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->StartFire(0);
	}
}

void AArenaPlayerController::OnFireReleased()
{
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::FireRelease)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->StopFire(0);
		}
	}
}

void AArenaPlayerController::OnAltFirePressed()
{
	if (ArenaNet::HandleSpectatorFire(this, true))
	{
		return; // spectating a UT2004 server: the server's spectator controls (Net/UE2NetWorld.h)
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (IsPawnDead())
	{
		if (AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>())
		{
			GM->RequestRespawn(this);
		}
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		Char->StartFire(1);
	}
}

void AArenaPlayerController::OnAltFireReleased()
{
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::AltFireRelease)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->StopFire(1);
		}
	}
}

void AArenaPlayerController::OnTapForward()
{
	if (bPauseMenuOpen)
	{
		OnMenuUp();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->OnDirectionTap(EArenaDir::Forward);
		}
	}
}

void AArenaPlayerController::OnTapBack()
{
	if (bPauseMenuOpen)
	{
		OnMenuDown();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->OnDirectionTap(EArenaDir::Back);
		}
	}
}

void AArenaPlayerController::OnTapLeft()
{
	if (bPauseMenuOpen)
	{
		OnMenuLeft();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->OnDirectionTap(EArenaDir::Left);
		}
	}
}

void AArenaPlayerController::OnTapRight()
{
	if (bPauseMenuOpen)
	{
		OnMenuRight();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->OnDirectionTap(EArenaDir::Right);
		}
	}
}

void AArenaPlayerController::OnNextWeapon()
{
	if (IsPauseSettingsOpen())
	{
		// mouse wheel adjusts the hovered / selected value
		ChangePauseSetting(PauseSettingsSelection, +1);
		return;
	}
	if (bPauseMenuOpen)
	{
		OnMenuUp();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::NextWeapon)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->NextWeapon();
		}
	}
}

void AArenaPlayerController::OnPrevWeapon()
{
	if (IsPauseSettingsOpen())
	{
		ChangePauseSetting(PauseSettingsSelection, -1);
		return;
	}
	if (bPauseMenuOpen)
	{
		OnMenuDown();
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::PrevWeapon)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->PrevWeapon();
		}
	}
}

void AArenaPlayerController::OnSlot(int32 Slot)
{
	if (bPauseMenuOpen)
	{
		if (bPauseSettingsOpen)
		{
			return;
		}
		const int32 Index = Slot - 1;
		if (GetPauseMenuItems().IsValidIndex(Index))
		{
			PauseMenuSelection = Index;
			ActivatePauseItem(Index);
		}
		return;
	}
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::Slot), Slot))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	AArenaCharacter* Char = GetArenaCharacter();
	if (!Char || !Char->IsAlive())
	{
		return;
	}
	if (Slot == 0)
	{
		// Key 0 selects (and cycles) the weapons of slot 0 / 10 (instagib rifle, warhead launcher, orbital designator)
		const AArenaWeapon* Before = Char->GetCurrentWeapon();
		Char->SwitchToSlot(0);
		if (Char->GetCurrentWeapon() == Before)
		{
			Char->SwitchToSlot(10);
		}
		return;
	}
	Char->SwitchToSlot(Slot);
}

void AArenaPlayerController::OnTranslocatorKey()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	AArenaCharacter* Char = GetArenaCharacter();
	if (!Char || !Char->IsAlive())
	{
		return;
	}
	// Weapon.Reselect when the group's weapon is already held (TransLauncher: beacon camera / back)
	if (AWeap_Translocator* T = Cast<AWeap_Translocator>(Char->GetCurrentWeapon()))
	{
		T->Reselect();
		return;
	}
	Char->SwitchToSlot(10);
}

void AArenaPlayerController::OnScoreboardPressed()
{
	bScoreboardHeld = true;
}

void AArenaPlayerController::OnScoreboardReleased()
{
	bScoreboardHeld = false;
}

void AArenaPlayerController::OnUse()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::Use)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive())
		{
			Char->Use();
		}
	}
}

void AArenaPlayerController::OnSuicide()
{
	if (IsPawnInputBlocked())
	{
		return;
	}
	if (ArenaNet::HandleNetPlayInput(this, int32(FUE2NetPlay::EInput::Suicide)))
	{
		return; // playing on a UT2004 server (Net/UE2NetPlay.h)
	}
	if (AArenaCharacter* Char = GetArenaCharacter())
	{
		if (Char->IsAlive() && Char->HasAuthority())
		{
			Char->Suicide();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Pause menu
// ---------------------------------------------------------------------------------------------

const TArray<FString>& AArenaPlayerController::GetPauseMenuItems()
{
	static const TArray<FString> Items = {
		TEXT("Resume"),
		TEXT("Settings"),
		TEXT("Restart Match"),
		TEXT("Leave to Main Menu"),
		TEXT("Quit Game")
	};
	return Items;
}

const TArray<FName>& AArenaPlayerController::GetPauseSettingsRows()
{
	static const TArray<FName> Rows = []()
	{
		TArray<FName> Result = UArenaUserSettings::GetAdjustableSettingIds();
		Result.Add(FName(TEXT("Back")));
		return Result;
	}();
	return Rows;
}

void AArenaPlayerController::OnPausePressed()
{
	if (IsPauseSettingsOpen())
	{
		OpenPauseSettings(false);
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f);
		return;
	}
	SetPauseMenuOpen(!bPauseMenuOpen);
}

void AArenaPlayerController::SetPauseMenuOpen(bool bOpen)
{
	if (bOpen == bPauseMenuOpen)
	{
		return;
	}
	bPauseMenuOpen = bOpen;
	bPauseSettingsOpen = false;
	PauseSettingsSelection = 0;

	if (bOpen)
	{
		PauseMenuSelection = 0;
		// Freeze our pawn: release held buttons so it does not keep firing / jumping
		if (AArenaCharacter* Char = GetArenaCharacter())
		{
			if (Char->IsAlive())
			{
				Char->StopFire(0);
				Char->StopFire(1);
			}
			Char->StopJumping();
		}
		bShowMouseCursor = true;
		bEnableClickEvents = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuSelect"));
	}
	else
	{
		bShowMouseCursor = false;
		bEnableClickEvents = false;
		SetInputMode(FInputModeGameOnly());
	}
}

void AArenaPlayerController::OpenPauseSettings(bool bOpen)
{
	if (!bPauseMenuOpen)
	{
		return;
	}
	bPauseSettingsOpen = bOpen;
	if (bOpen)
	{
		PauseSettingsSelection = 0;
	}
	else
	{
		PauseMenuSelection = 1; // back on the "Settings" entry
		UArenaUserSettings::Get()->Save();
	}
}

void AArenaPlayerController::SetPauseMenuSelection(int32 Index)
{
	if (bPauseMenuOpen && !bPauseSettingsOpen && GetPauseMenuItems().IsValidIndex(Index) && Index != PauseMenuSelection)
	{
		PauseMenuSelection = Index;
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f);
	}
}

void AArenaPlayerController::ClickPauseItem(int32 Index)
{
	if (bPauseMenuOpen && !bPauseSettingsOpen && GetPauseMenuItems().IsValidIndex(Index))
	{
		PauseMenuSelection = Index;
		ActivatePauseItem(Index);
	}
}

void AArenaPlayerController::SetPauseSettingsSelection(int32 Row)
{
	if (IsPauseSettingsOpen() && GetPauseSettingsRows().IsValidIndex(Row) && Row != PauseSettingsSelection)
	{
		PauseSettingsSelection = Row;
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f);
	}
}

void AArenaPlayerController::ClickPauseSetting(int32 Row, int32 Dir)
{
	if (!IsPauseSettingsOpen() || !GetPauseSettingsRows().IsValidIndex(Row))
	{
		return;
	}
	PauseSettingsSelection = Row;
	ChangePauseSetting(Row, Dir);
}

void AArenaPlayerController::ChangePauseSetting(int32 Row, int32 Dir)
{
	const TArray<FName>& Rows = GetPauseSettingsRows();
	if (!IsPauseSettingsOpen() || !Rows.IsValidIndex(Row))
	{
		return;
	}
	if (Rows[Row] == FName(TEXT("Back")))
	{
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuSelect"));
		OpenPauseSettings(false);
		return;
	}
	if (UArenaUserSettings::Get()->ChangeSetting(Rows[Row], Dir))
	{
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f, Dir >= 0 ? 1.08f : 0.94f);
	}
}

void AArenaPlayerController::OnMenuUp()
{
	if (!bPauseMenuOpen)
	{
		return;
	}
	if (bPauseSettingsOpen)
	{
		const int32 Num = GetPauseSettingsRows().Num();
		PauseSettingsSelection = (PauseSettingsSelection + Num - 1) % Num;
	}
	else
	{
		const int32 Num = GetPauseMenuItems().Num();
		PauseMenuSelection = (PauseMenuSelection + Num - 1) % Num;
	}
	ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f);
}

void AArenaPlayerController::OnMenuDown()
{
	if (!bPauseMenuOpen)
	{
		return;
	}
	if (bPauseSettingsOpen)
	{
		const int32 Num = GetPauseSettingsRows().Num();
		PauseSettingsSelection = (PauseSettingsSelection + 1) % Num;
	}
	else
	{
		const int32 Num = GetPauseMenuItems().Num();
		PauseMenuSelection = (PauseMenuSelection + 1) % Num;
	}
	ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.6f);
}

void AArenaPlayerController::OnMenuLeft()
{
	if (IsPauseSettingsOpen() && GetPauseSettingsRows()[PauseSettingsSelection] != FName(TEXT("Back")))
	{
		ChangePauseSetting(PauseSettingsSelection, -1);
	}
}

void AArenaPlayerController::OnMenuRight()
{
	if (IsPauseSettingsOpen() && GetPauseSettingsRows()[PauseSettingsSelection] != FName(TEXT("Back")))
	{
		ChangePauseSetting(PauseSettingsSelection, +1);
	}
}

void AArenaPlayerController::OnMenuAccept()
{
	if (!bPauseMenuOpen)
	{
		return;
	}
	if (bPauseSettingsOpen)
	{
		ChangePauseSetting(PauseSettingsSelection, +1);
		return;
	}
	ActivatePauseItem(PauseMenuSelection);
}

void AArenaPlayerController::OnSensitivityDown()
{
	AdjustSensitivityHotkey(-1);
}

void AArenaPlayerController::OnSensitivityUp()
{
	AdjustSensitivityHotkey(+1);
}

void AArenaPlayerController::AdjustSensitivityHotkey(int32 Dir)
{
	UArenaUserSettings* S = UArenaUserSettings::Get();
	S->ChangeSetting(FName(TEXT("Sensitivity")), Dir);

	// Replace any previous sensitivity readout instead of stacking them
	const FString Prefix = TEXT("Mouse sensitivity");
	CenterMessages.RemoveAll([&Prefix](const FArenaCenterMessage& M) { return M.Text.StartsWith(Prefix); });
	AddCenterMessage(FString::Printf(TEXT("%s %.2f"), *Prefix, S->MouseSensitivity), FLinearColor(0.6f, 0.85f, 1.f), 1.2f);
	ArenaAssets::PlaySound2D(this, TEXT("S_MenuMove"), 0.5f, Dir >= 0 ? 1.08f : 0.94f);
}

void AArenaPlayerController::ActivatePauseItem(int32 Index)
{
	ArenaAssets::PlaySound2D(this, TEXT("S_MenuSelect"));
	AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();

	switch (Index)
	{
	case 0: // Resume
		SetPauseMenuOpen(false);
		break;

	case 1: // Settings sub-page
		OpenPauseSettings(true);
		break;

	case 2: // Restart match
		SetPauseMenuOpen(false);
		if (GM)
		{
			GM->RestartMatch();
		}
		else
		{
			ConsoleCommand(TEXT("disconnect"));
		}
		break;

	case 3: // Leave to main menu
		SetPauseMenuOpen(false);
		if (GM)
		{
			GM->ReturnToMenu();
		}
		else
		{
			ClientTravel(TEXT("/Game/Maps/MainMenu"), TRAVEL_Absolute);
		}
		break;

	case 4: // Quit
		UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false);
		break;

	default:
		break;
	}
}

// ---- chat (ArenaChat.h) ----

void AArenaPlayerController::Say(const FString& Msg)
{
	ArenaChat::Send(this, Msg, false);
}

void AArenaPlayerController::TeamSay(const FString& Msg)
{
	ArenaChat::Send(this, Msg, true);
}

void AArenaPlayerController::Talk()
{
	if (!bPauseMenuOpen && !ArenaChat::IsTyping())
	{
		ArenaChat::OpenTyping(this, false);
	}
}

void AArenaPlayerController::TeamTalk()
{
	if (!bPauseMenuOpen && !ArenaChat::IsTyping())
	{
		ArenaChat::OpenTyping(this, true);
	}
}

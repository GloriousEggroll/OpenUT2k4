#include "ArenaMenuGameMode.h"
#include "OpenUT2k4.h"
#include "ArenaArtHUD.h"
#include "ArenaHUD.h"
#include "ArenaMenuScreens.h"
#include "ArenaUserSettings.h"

#include "Engine/Canvas.h"

// =============================================================================================
// AArenaMenuGameMode
// =============================================================================================

AArenaMenuGameMode::AArenaMenuGameMode()
{
	DefaultPawnClass = nullptr;
	PlayerControllerClass = AArenaMenuPlayerController::StaticClass();
	HUDClass = AArenaMenuHUD::StaticClass();
}

void AArenaMenuGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// Intentionally do not call RestartPlayer: the menu has no pawn.
}

// =============================================================================================
// AArenaMenuPlayerController
// =============================================================================================

AArenaMenuPlayerController::AArenaMenuPlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
}

void AArenaMenuPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController())
	{
		bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		UArenaUserSettings::Get()->ApplyAudio();
	}
}

// =============================================================================================
// AArenaMenuHUD
// =============================================================================================

AArenaMenuHUD::AArenaMenuHUD()
{
}

AArenaMenuHUD::~AArenaMenuHUD()
{
}

TFunction<void(AArenaMenuHUD*)>& AArenaMenuHUD::OnOpenContentSetup()
{
	static TFunction<void(AArenaMenuHUD*)> Hook;
	return Hook;
}

void AArenaMenuHUD::DrawMenuBackdrop(const FArenaDraw& D, float Time, int32 Variant)
{
	if (!ArenaArtHUD::DrawMenuBackground(D, Time, Variant))
	{
		FArenaMenuScreens::DrawProceduralBackground(D, Time);
	}
}

void AArenaMenuHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	if (!Screens)
	{
		Screens = MakeUnique<FArenaMenuScreens>(this);
	}
	Screens->Draw(Canvas);
}

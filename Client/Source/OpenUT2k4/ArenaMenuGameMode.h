#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "ArenaTypes.h"
#include "ArenaMenuScreens.h"
#include "ArenaMenuGameMode.generated.h"

struct FArenaDraw;

/** Game mode for the main menu map: no pawn, menu controller + canvas menu HUD. */
UCLASS()
class OPENUT2K4_API AArenaMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AArenaMenuGameMode();

	/** Players never get a pawn in the menu. */
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
};

/** Menu controller: mouse cursor and UI input mode (the menu GUI reads its own input, see ArenaGUI2K4.h). */
UCLASS()
class OPENUT2K4_API AArenaMenuPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AArenaMenuPlayerController();

	virtual void BeginPlay() override;
};

/**
 * Front end menus in UT2004's GUI2K4 look (FArenaMenuScreens, ArenaMenuScreens.h): main menu, Instant Action,
 * Settings. Test hooks: README "Menus".
 */
UCLASS()
class OPENUT2K4_API AArenaMenuHUD : public AHUD
{
	GENERATED_BODY()

public:
	AArenaMenuHUD();
	virtual ~AArenaMenuHUD() override;

	virtual void DrawHUD() override;

	/**
	 * Optional hook for the UT2004 content setup screen: when bound, the main page shows a "UT2004 Content"
	 * entry (always when no maps are available) that calls it. Bound by the content setup code.
	 */
	static TFunction<void(AArenaMenuHUD* Menu)>& OnOpenContentSetup();

	/** The menu backdrop: UT2004 menu art when available, the animated procedural scene otherwise (also used by the content setup screen). */
	static void DrawMenuBackdrop(const FArenaDraw& D, float Time, int32 Variant);

protected:
	TUniquePtr<FArenaMenuScreens> Screens;
};

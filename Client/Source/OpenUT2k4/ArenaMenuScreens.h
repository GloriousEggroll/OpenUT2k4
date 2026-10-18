#pragma once

#include "CoreMinimal.h"
#include "ArenaGUI2K4.h"
#include "ArenaGUI2K4Pages.h"

// Single Player (the "singleplayer" agent's ArenaSinglePlayer.h): the main menu's Single Player button opens it once it exists
#if __has_include("ArenaSinglePlayer.h")
#define ARENA_HAS_SINGLEPLAYER 1
#else
#define ARENA_HAS_SINGLEPLAYER 0
#endif

class AHUD;
class UCanvas;
class FArenaSinglePlayerPage;
struct FArenaRulesEntry;
class UArenaGameRules;
class FArenaServerBrowserPage;

/**
 * The front end menus in UT2004's GUI2K4 look, drawn by AArenaMenuHUD (ArenaGUI2K4.h):
 *  - main menu (UT2k4MainMenu): mmbgnd backdrop, the scrolling title bands (PanHuge / PanBig / PanSmall), a character
 *    render, the logo and the corner view sliding in, then the stock text buttons (STY2TextButton, Impact) Single
 *    Player, Join Game, Host Game, Instant Action, Community, Settings, Exit sliding in from the right with the
 *    highlight bar behind the focused one; Exit opens the quit page (UT2K4QuitPage)
 *  - Single Player: the tournament pages (ArenaSinglePlayer.h, when built in)
 *  - Instant Action (UT2K4GamePageSP): Gametype, Select Map (with preview), Game Rules, Mutators, Bot Config tabs,
 *    PLAY / SPECTATE / BACK footer
 *  - Host Game (UT2K4GamePageMP): the same tabs plus Server Rules (server name, players, spectators, password, port,
 *    LAN server, advertising); LISTEN / DEDICATED / BACK (ArenaGameSession.h)
 *  - Settings (UT2K4SettingsPage, ArenaGUI2K4Pages::FSettingsPage)
 *  - Join Game: the server browser for stock UT2004 servers (UT2K4ServerBrowser, Net/ArenaServerBrowserPage.h)
 * Before any content exists the same layout is drawn with procedural stand-ins.
 */
class OPENUT2K4_API FArenaMenuScreens
{
public:
	enum class EPage : uint8 { Main, InstantAction, Settings, ServerBrowser, HostGame, SinglePlayer };

	explicit FArenaMenuScreens(AHUD* InOwner);
	~FArenaMenuScreens();
	/** Draws and runs the menus for one frame. */
	void Draw(UCanvas* Canvas);
	/** The procedural backdrop (no content yet), also used by the content setup screen. */
	static void DrawProceduralBackground(const FArenaDraw& D, float Time);

	EPage GetPage() const { return Page; }

private:
	void SetPage(EPage NewPage);
	enum class EStart : uint8 { Local, Listen, Dedicated };
	void StartMatch(EStart How = EStart::Local);
	void LoadFromSettings();
	void SaveToSettings() const;
	TArray<int32> GetValidLayoutIndices() const;
	void ValidateLayout();
	bool HasMaps() const;
	FString GetModeKey() const;
	/** Instant Action "Translocator" of the selected mode (saved per mode, else the mode's default). */
	bool GetTranslocatorSetting() const;
	void SetTranslocatorSetting(bool bOn);
	const FArenaRulesEntry* GetModeEntry() const;
	const UArenaGameRules* GetModeDefaults() const;
	static int32 FindModeIndex(const FString& ModeId);
	void SetMode(int32 NewModeIndex);
	void ApplyTestCommand(const FString& Name, const FString& Value);
	void RunLegacyTestHooks();

	void DrawMainPage();
	void DrawQuitPage();
	/** Instant Action, or Host Game when Page is HostGame (the Server Rules tab and the LISTEN / DEDICATED footer). */
	void DrawInstantActionPage();
	void DrawServerRulesTab(const ArenaGUI2K4::FRect& Panel);
	void DrawGameTypeTab(const ArenaGUI2K4::FRect& Panel);
	void DrawMapTab(const ArenaGUI2K4::FRect& Panel);
	void DrawRulesTab(const ArenaGUI2K4::FRect& Panel);
	void DrawMutatorTab(const ArenaGUI2K4::FRect& Panel);
	void DrawBotTab(const ArenaGUI2K4::FRect& Panel);

	TWeakObjectPtr<AHUD> Owner;
	ArenaGUI2K4::FGUI G;
	ArenaGUI2K4Pages::FSettingsPage Settings;
	/** Join Game: the UT2004 server browser (Net/ArenaServerBrowserPage.h), created when first opened */
	TUniquePtr<FArenaServerBrowserPage> Browser;
	ArenaGUI2K4Pages::FAutomation Automation;
#if ARENA_HAS_SINGLEPLAYER
	/** Single Player (ArenaSinglePlayer.h), created when first opened */
	TUniquePtr<FArenaSinglePlayerPage> SinglePlayer;
#endif
	bool bCheckedSinglePlayerStart = false;

	EPage Page = EPage::Main;
	double PageOpenedAt = -1.0;
	/** UT2k4MainMenu.ButtonClick: the chosen button; the menu animates out, then MoveOn() */
	FName PendingAction;
	double ActionAt = 0.0;
	bool bQuitPage = false;
	int32 CharShot = 0;
	/** UT2k4MainMenu.MyOnDraw: button Tag (highlight alpha 200 while focused, -784 per second) */
	TMap<int32, float> Highlight;

	// instant action
	int32 IATab = 0;
	double IATabChangedAt = 0.0;
	// host game (the instant action tabs + Server Rules at index 3, like UT2K4GamePageMP)
	int32 HostTab = 0;
	double HostTabChangedAt = 0.0;
	/** Result of the last LISTEN / DEDICATED (shown in the footer area) */
	FString HostMessage;
	int32 ModeIndex = 0;
	FString LayoutId;
	int32 NumBots = 7;
	int32 BotSkill = 3;
	int32 GoalScore = 25;
	int32 TimeLimit = 15;
	bool bInstagib = false;
	int32 Team = 0;
	bool bOfficialMapsOnly = false;
	TArray<FString> Mutators;
	int32 AvailSel = 0;
	int32 ActiveSel = -1;
	bool bDescActive = false;
	FString MutatorMessage;
	bool bLoaded = false;
};

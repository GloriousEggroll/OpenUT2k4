#pragma once

#include "CoreMinimal.h"

class AHUD;
class UCanvas;
class AArenaPlayerController;
class AArenaGameState;

/**
 * The in-game menus in UT2004's GUI2K4 look (ArenaGUI2K4.h), drawn by AArenaHUD:
 *  - the mid-game menu (UT2K4PlayerLoginMenu: floating Display1 window, title bar with close box, Game / Rules tabs;
 *    the Game tab lists the players (team sections in team games) and has Resume, Restart Match (Map Voting on a
 *    UT2004 server that runs it, with its own tab), Leave Match, Settings and Exit Game buttons in the UT2K4Tab_PlayerLoginControls button grid)
 *  - the Settings page (UT2K4SettingsPage, shared with the main menu)
 *  - the end-of-match banner (winner line in the HUD's large font).
 * Input goes through the GUI's own input queue while the menu is open; Esc / the close box resume the game.
 */
namespace ArenaMidGameMenu
{
	/** Draws the pause menu or its settings page (bForceSettings / bForceMenu: test pages forced on). */
	OPENUT2K4_API void DrawPauseMenu(AHUD* HUD, UCanvas* Canvas, AArenaPlayerController* PC, bool bForceSettings);
	/** A UT2004 server opened map voting (end of the match): opens the menu on its Map Voting tab. */
	OPENUT2K4_API void OpenMapVote(AArenaPlayerController* PC);
	/** The server closed map voting: closes the menu when it shows the vote. */
	OPENUT2K4_API void CloseMapVote(AArenaPlayerController* PC);
	/** The end-of-match banner. */
	OPENUT2K4_API void DrawEndOfMatch(AHUD* HUD, UCanvas* Canvas, const AArenaGameState* GS);
}

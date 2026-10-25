#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class AHUD;
class UCanvas;

/**
 * The game side of mods' menus (their GUI2K4 / XInterface pages run as script: Net/UE2NetScriptHUD.h,
 * UScript/UScriptGUI.h; Docs/UE2Net.md "Mod menus").
 *
 *  - Drawing: AArenaHUD::DrawHUD draws an open mod menu over everything else, with UT2004's mouse cursor.
 *  - Input: while one is open, an input pre-processor gives it the mouse buttons, the wheel and the keys (as UE2
 *    EInputKey / EInputAction, typed characters as KeyType; the console key stays ours), the cursor position comes from
 *    the player controller each frame, the cursor is shown and our pawn stops firing. Escape closes the top menu
 *    (GUIController.CloseMenu).
 *  - Mod key binds: keys a mod's code bound for itself ("set input F5 MyMenu": UTComp's menu key) run their command
 *    while playing on a UT2004 server (kept in our per-user ini).
 *
 * Test hooks (console): ModMenuOpen <class>, ModMenuDump, ModMenuClick <component name or caption>, ModMenuKey <EInputKey>,
 * ModMenuType <text>, ModMenuClose.
 */
namespace UE2NetModMenus
{
	/** AArenaHUD::DrawHUD: draws an open mod menu (and switches the input to it). True when one is open. */
	bool DrawAndUpdate(AHUD* Hud, UCanvas* Canvas);
	/** A mod menu is open. */
	bool IsOpen();
	/** UE2 EInputKey of a key (0 for keys UE2 has no code for). */
	uint8 KeyCode(const FKey& Key);
}

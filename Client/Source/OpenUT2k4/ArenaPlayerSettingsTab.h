#pragma once

#include "CoreMinimal.h"
#include "ArenaGUI2K4.h"

/**
 * Settings > Player (UT2004's UT2K4Tab_PlayerSettings): the character with its 3D view (the converted mesh with its
 * idle animation and body skin, rendered by a scene capture) or portrait, the biography, Change Character (the
 * UT2K4ModelSelect window: the .upl characters by race, the locked ones shown locked until the tournament unlocks
 * them), and Misc.: name, small weapons, preferred team, weapon hand, voice type, field of view.
 * The choice is saved in UArenaUserSettings (PlayerCharacter, PlayerSex, PreferredTeam, VoiceType, WeaponHand,
 * bSmallWeapons) and used for the local player's model (?Character=) in every match.
 */
namespace ArenaPlayerSettingsTab
{
	/** Draws the tab into the settings page panel. */
	OPENUT2K4_API void Draw(ArenaGUI2K4::FGUI& G, const ArenaGUI2K4::FRect& Panel);
	/** DEFAULTS of the tab (UT2K4Tab_PlayerSettings.ResetClicked: Jakob, "Player", no team, right hand, FOV). */
	OPENUT2K4_API void ResetDefaults();
	/** True while the Change Character window is open (the page leaves Esc to it). */
	OPENUT2K4_API bool IsModalOpen();
	/** Test hooks (non-shipping): "PlayerChar=<name>", "PlayerSelect=1" (opens Change Character), "Player3D=0|1". */
	OPENUT2K4_API void ApplyTestCommand(const FString& Name, const FString& Value);
}

#pragma once

#include "CoreMinimal.h"

/**
 * Names shown to players for the game itself. Kept in one place so the project can be renamed
 * (menus, HUD and window texts read these instead of spelling the name out).
 */
namespace ArenaBranding
{
	/** Game title drawn by the main menu. */
	inline const TCHAR* GameTitle() { return TEXT("OpenUT2k4"); }
	/** Line under the title. */
	inline const TCHAR* GameSubtitle() { return TEXT("A R E N A   C O M B A T"); }
	/** Version tag in the menu corner. */
	inline const TCHAR* VersionTag() { return TEXT("v0.1"); }
	/** Not-affiliated notice shown on the main menu and the content setup screen (also in README.md and NOTICE.md). */
	inline const TCHAR* Disclaimer()
	{
		return TEXT("Unofficial fan project, not affiliated with or endorsed by Epic Games. Unreal and Unreal Tournament are trademarks of Epic Games, Inc.");
	}
}

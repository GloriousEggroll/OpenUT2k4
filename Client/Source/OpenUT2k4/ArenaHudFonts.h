#pragma once

#include "CoreMinimal.h"

class UFont;

/**
 * UT2004's HUD fonts, from the player's own content cache (ArenaRuntimeContent::LoadFont), with the HUD's font size
 * rules. The values come from the stock classes of the player's install, read once while this was written:
 *   XInterface.HudBase defaults:  FontArrayNames[0..8] = UT2003Fonts.FontEurostile37, 29, 24, 21, 17, 14, 12, 9,
 *                                 UT2003Fonts.FontSmallText; ConsoleFontSize = 5; FontScreenWidthMedium[0..8] =
 *                                 2048, 1600, 1280, 1024, 800, 640, 512, 400, 320
 *   Engine.HUD.GetFontSizeIndex:  FontSize + 1 for each of ClipX >= 512, 640, 800, 1024, 1280, 1600;
 *                                 font = FontArrayNames[Clamp(8 - FontSize, 0, 8)]
 *   Engine.HUD.GetConsoleFont:    ConsoleFontSize + 1 for each of ClipX < 640, 800, 1024, 1280, 1600; Min(8, size)
 *   Engine.HUD.GetMediumFontFor:  the first i with FontScreenWidthMedium[i] <= ClipX, else 8
 * Fonts are bitmap fonts drawn 1 texel = 1 pixel (UE2 Canvas), so the size only depends on the screen width; on big
 * screens they are drawn a whole multiple larger (FArenaDraw::UTFontScale), resampled to that size (GetScaled).
 * Without a cache with fonts (cooked import mode, a cache converted before fonts were exported) the HUD falls back
 * to the engine font scaled to the same height (NominalHeight).
 */
namespace ArenaHudFonts
{
	constexpr int32 NumFonts = 9;

	/** UT2004 Font object path of FontArrayNames[Index]. */
	OPENUT2K4_API const TCHAR* FontPath(int32 Index);
	/** Line height in pixels of FontArrayNames[Index] (the fonts' tallest glyph cell). */
	OPENUT2K4_API int32 NominalHeight(int32 Index);
	/** The runtime font of FontArrayNames[Index]; nullptr when the cache has no fonts (callers fall back). Game thread. */
	OPENUT2K4_API UFont* Get(int32 Index);
	/**
	 * FontArrayNames[Index] resampled to Scale times its height (ArenaRuntimeContent::LoadFontAtHeight), drawn at scale 1:
	 * smooth at 2x / 3x where point sampling made blocky text; also at 1x the page is the clean (uncompressed) copy.
	 */
	OPENUT2K4_API UFont* GetScaled(int32 Index, float Scale);

	/** HUD.GetFontSizeIndex: the font index for a LocalMessage FontSize at a canvas width. */
	OPENUT2K4_API int32 FontSizeIndex(float ClipX, int32 FontSize);
	/** HUD.GetConsoleFont (default ConsoleFontSize 5). */
	OPENUT2K4_API int32 ConsoleFontIndex(float ClipX);
	/** HUD.GetMediumFontFor. */
	OPENUT2K4_API int32 MediumFontIndex(float ClipX);
}

#pragma once

#include "CoreMinimal.h"

/**
 * Where the game gets UT2004 content from.
 *
 * Release builds ship no UT2004 data at all. Players point the game at their own UT2004 install; the
 * game converts it once into a content cache in the user's data folder (same layout as the developer
 * ImportCache/ produced by Tools/ut2k4_export.py) and builds textures, materials, meshes, sounds and
 * maps from that cache at runtime.
 *
 * Two modes:
 *   - Cooked imports (developer builds only): /Game/Imported assets made by Tools/ImportUT2004.py
 *     and Content/ImportedData json files exist in the build. Used as before.
 *   - Runtime content (release builds, or any build started with -RuntimeContent): nothing under
 *     /Game/Imported is used; everything comes from GetCacheDir().
 *
 * Cache layout (GetCacheDir()):
 *   textures/<Pkg>/<Group.Name>.png and .json, materials, staticmeshes/<Pkg>/<Name>.gltf/.bin/.json,
 *   skeletalmeshes, animations, vertmeshes, sounds/<Pkg>/<Name>.wav|.ogg + .json, music/<Name>.ogg,
 *   maps/<Map>/map.json + bsp.gltf, fonts/<Pkg>/<Name>.json (HUD bitmap fonts, ArenaConvFonts.h) + fonts/index.json,
 *   localization/<File>.int / .ucl + index.json (the install's System language files, ArenaConvLocalization.h),
 *   and data/ for the game-side json (marker files <Map>.json and the
 *   Assets json tables that the editor importer writes to Content/ImportedData).
 *   cache.json at the root records the format version and the install it was converted from.
 */
namespace ArenaContent
{
	/**
	 * Bumped whenever converted files change meaning; a cache with another version is converted again, every asset
	 * rewritten (FArenaConverter). 2: UE2 Color structs are B,G,R,A (red and blue were swapped in version 1).
	 */
	constexpr int32 CacheFormatVersion = 2;

	/**
	 * True when UT2004 content must come from the runtime cache instead of cooked /Game/Imported assets.
	 * -RuntimeContent forces it on, -NoRuntimeContent off; otherwise it is on when the build has no
	 * Content/ImportedData folder (release builds never stage it).
	 */
	OPENUT2K4_API bool UseRuntimeContent();

	/**
	 * The converted content cache. -ContentCache=<dir> overrides; otherwise
	 *   Linux:   $XDG_DATA_HOME/OpenUT2k4/ContentCache  (~/.local/share/OpenUT2k4/ContentCache)
	 *   Windows: %LOCALAPPDATA%/OpenUT2k4/ContentCache
	 *   Mac:     ~/Library/Application Support/OpenUT2k4/ContentCache
	 */
	OPENUT2K4_API const FString& GetCacheDir();

	/** Game-side json in the cache (marker files, asset tables). */
	OPENUT2K4_API FString GetCacheDataDir();

	/** True when GetCacheDir() holds a complete cache of the current format (cache.json, version matches). */
	OPENUT2K4_API bool IsCacheReady();

	/**
	 * The UT2004 install to convert from, or empty if none was found. Search order:
	 *   -UT2004Dir=<dir>, $OPENUT2K4_UT2004_DIR, the saved choice (UArenaUserSettings::UT2004Dir),
	 *   a "UT2004" folder next to the game (drop-in), then common install locations.
	 * A folder counts as an install when it contains System/ and Maps/ (case-insensitive on Linux).
	 */
	OPENUT2K4_API FString FindUT2004Install();

	/**
	 * A UT2004 localization file (e.g. "XInterface.int"): <cache>/localization/<File> (copied there by the converter,
	 * ArenaConvLocalization.h), else the install's System/<File>; matched case-insensitively. Empty when neither has it.
	 */
	OPENUT2K4_API FString FindLocalizationFile(const FString& FileName);

	/** True if Dir looks like a UT2004 install (System/ and Maps/ folders). */
	OPENUT2K4_API bool IsUT2004Install(const FString& Dir);

	/**
	 * The game's per-user data folder (parent of the default content cache):
	 * Linux $XDG_DATA_HOME/OpenUT2k4, Windows %LOCALAPPDATA%/OpenUT2k4, Mac ~/Library/Application Support/OpenUT2k4.
	 * GetUserDataDir()/UT2004 is one of the GetSearchLocations() and where the OldUnreal download installs to.
	 */
	OPENUT2K4_API FString GetUserDataDir();

	/**
	 * The player's custom content folder (community maps and their packages, see ArenaUserContent.h):
	 * GetUserDataDir()/UserContent, or -UserContent=<dir>. Read only: the game never writes into it.
	 */
	OPENUT2K4_API const FString& GetUserContentDir();

	/** Candidate folders FindUT2004Install checks after the explicit settings (for the setup screen). */
	OPENUT2K4_API TArray<FString> GetSearchLocations();
}

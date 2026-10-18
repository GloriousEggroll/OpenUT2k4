#pragma once

#include "CoreMinimal.h"

/**
 * UT2004's script code in the content cache, laid out like an install so the UnrealScript runtime (UScript/) can use
 * it as its game folder when the install is moved or deleted after the conversion:
 *   <cache>/scripts/System/<Pkg>.u            every script package of the install's System folder (about 31 MB)
 *   <cache>/scripts/System/<Name>.ini         Default.ini, DefUser.ini, UT2004.ini, User.ini (package paths, config)
 *   <cache>/scripts/Maps/<Map>.ut2            only the stock maps that define their own script classes
 *   <cache>/scripts/index.json                { "version": ArenaConverterVersions::ScriptsVersion, "packages", "maps" }
 * The full converter copies them on every run; the custom content pass at startup (ArenaConvUserContent) copies them
 * into caches converted before (stage "Updating script packages") when NeedsUpdate() says so and an install is there.
 * Custom packages stay in the UserContent folder. Nothing of it is shipped (it is the player's own cache).
 */
namespace ArenaConvScripts
{
	/** <cache>/scripts */
	OPENUT2K4_API FString GetDir(const FString& CacheDir);
	/** True when <cache>/scripts/index.json is missing or older than ArenaConverterVersions::ScriptsVersion. */
	OPENUT2K4_API bool NeedsUpdate(const FString& CacheDir);
	/** True when <cache>/scripts has Core.u (the runtime can start from it). */
	OPENUT2K4_API bool HasScripts(const FString& CacheDir);
	/** Copies the files (those newer than or different from the cached copy) and writes the index. Returns false on failure. */
	OPENUT2K4_API bool CopyAll(const FString& InstallDir, const FString& CacheDir, const TAtomic<bool>* Cancel = nullptr);
}

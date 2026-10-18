#pragma once

#include "CoreMinimal.h"

/**
 * UT2004's localization files in the content cache: every .int, .ucl and .upl file in the install's System folder is copied to
 * <cache>/localization/ (with index.json: { "version": ArenaConverterVersions::LocalizationVersion, "files": [...] }),
 * so the HUD, message, menu and script texts (ArenaContent::FindLocalizationFile) keep working when the install is
 * moved or deleted after the conversion. They stay in the player's cache; nothing of it is shipped.
 *
 * The full converter copies them on every run; the custom content pass at startup (ArenaConvUserContent) copies them
 * into caches converted before (stage "Updating language files") when NeedsUpdate() says so.
 */
namespace ArenaConvLocalization
{
	/** True when <cache>/localization/index.json is missing or older than ArenaConverterVersions::LocalizationVersion. */
	OPENUT2K4_API bool NeedsUpdate(const FString& CacheDir);

	/** Copies the install's System .int / .ucl / .upl files (newer than the cached copy) and writes the index. Returns the count, -1 on failure. */
	OPENUT2K4_API int32 CopyAll(const FString& InstallDir, const FString& CacheDir);
}

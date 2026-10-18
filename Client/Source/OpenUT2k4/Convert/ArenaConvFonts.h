#pragma once

#include "CoreMinimal.h"

class FConvExporter;
class FConvPackageManager;

/**
 * UE2 Font objects in the content cache (the HUD's bitmap fonts, e.g. UT2003Fonts.FontEurostile24, 2K4Fonts.Verdana20).
 *
 * FConvExporter::ExportFont writes fonts/<Pkg>/<Name>.json for every Font a package unit or a map exports:
 *   { "type": "Font", "path": "Pkg.Name", "kerning": int, "remapped": bool, "max_height": int,
 *     "pages": [ { "path": "Pkg.Name.Page", "file": "textures/Pkg/Name.Page.png", "width": w, "height": h } ],
 *     "chars": [ [StartU, StartV, USize, VSize, Page], ... ],      (index = character code unless remapped)
 *     "remap": [ [UnicodeChar, CharIndex], ... ] }                  (only for remapped fonts)
 * The pages are ordinary texture exports. ArenaRuntimeContent::LoadFont builds a UFont from it.
 *
 * Caches converted before fonts were exported get them through ExportAll: the full converter calls it at the end of
 * every run, and the custom content pass (ArenaConvUserContent, runs at startup) calls it when NeedsUpdate() says so
 * (<cache>/fonts/index.json missing or older than ArenaConverterVersions::FontsVersion).
 */
namespace ArenaConvFonts
{
	/** True when the cache has no fonts index of the current ArenaConverterVersions::FontsVersion. Cheap (one small file). */
	bool NeedsUpdate(const FString& CacheDir);

	/**
	 * Exports every Font object of the install's packages (the custom packages FConvPackageManager::AddPackageFiles added
	 * are left out), reusing files that are newer than their package, then writes <cache>/fonts/index.json.
	 * Returns the number of fonts in the index, or -1 when cancelled (no index written).
	 */
	int32 ExportAll(FConvPackageManager& Mgr, FConvExporter& Ex, const FString& CacheDir, const TAtomic<bool>* Cancel);
}

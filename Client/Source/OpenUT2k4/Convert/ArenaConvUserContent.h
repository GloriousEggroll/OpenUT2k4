#pragma once

#include "CoreMinimal.h"

/**
 * Incremental conversion of the player's custom content (ArenaUserContent.h has the game side and the folder rules).
 *
 * One Run():
 *   1. lists the custom content folder (any depth; files are sorted by extension, not by folder), unpacks new or
 *      changed .umod / .ut2mod / .zip archives into <cache>/usercontent/unpacked/<archive>/ and drops the unpacked
 *      folders of archives that are gone;
 *   2. fingerprints every usable file (size + mtime, MD5 only when those changed) against <cache>/usercontent/state.json;
 *   3. indexes the custom packages next to the install (FConvPackageManager::AddPackageFiles: a name the install has
 *      keeps the install's file), reads the import tables of the custom maps and packages and works out per map the
 *      custom packages it needs (transitively), the packages that are missing, and the actors whose class comes from a
 *      custom script package or the map itself;
 *   4. removes the cache entries of deleted / changed custom maps, packages and music, then converts only the maps whose
 *      fingerprint (map file + the custom packages it uses + its .int / .ucl metadata) changed, and new custom music;
 *   5. updates the cache-side data incrementally (combiners of the touched packages, texture_stats.json merge,
 *      data/Assets/Music.json, data/<Map>.json of the converted maps) and writes state.json.
 *
 * Also updates converted maps of an older map schema (ArenaConverterVersions::MapSchemaVersion; stage "Updating
 * converted maps"): any cached maps/<Map>/map.json with a lower schema_version is exported again from the install
 * (custom maps through their fingerprint), so earlier caches get new map features without a full conversion.
 *
 * state.json "maps" entries carry what the menu and the setup screen show: status ("ok" / "unplayable"), problems
 * (plain-language reasons), missing packages, script classes, title / author / description / players / screenshot.
 */
namespace ArenaConvUserContent
{
	/** Bumped when the state layout or the per-map conversion changes (every custom map is converted again). */
	constexpr int32 StateVersion = 1;
	/** Bumped when a converter fix may export what failed before: a custom map converted with errors by an older
	 *  revision is converted again (state.json "error_retry"; 1: struct arrays stored with a short size, e.g. a static
	 *  mesh's Materials). */
	constexpr int32 ErrorRetryVersion = 1;
	/** Bumped when more of a script package's assets are exported (state.json packages' "assets", and the signature of
	 *  a map with embedded classes; 1: every texture, material, mesh, animation and sound inside a .u, for mod weapons:
	 *  Net/UE2NetModWeapons.h; 2: also those it takes from other custom packages, and a map's embedded classes' assets,
	 *  for mod vehicles). */
	constexpr int32 ScriptAssetsVersion = 2;

	struct FOptions
	{
		FString InstallDir;
		FString CacheDir;
		FString UserDir;
		/** Convert every custom map again. */
		bool bForce = false;
		/**
		 * Folder under CacheDir for the state (state.json, errors.log, unpacked archives). Another name keeps a separate
		 * set of converted maps and packages, e.g. "netcache" for packages downloaded from servers (Net/UE2NetDownload.h).
		 */
		FString StateDirName = TEXT("usercontent");
		/** When not empty: exactly these files are the content (UserDir is not listed). */
		TArray<FString> Files;
		/** Package files indexed next to the install (found, not converted or removed here), e.g. the custom content folder's. */
		TArray<FString> ReferencePackages;
		/** Also update older caches (map schema, fonts, language files, script copies). Off for the download pass. */
		bool bMaintenance = true;
	};

	struct FProgress
	{
		FString Stage;
		FString Item;
		float Fraction = 0.f;
		int32 Done = 0;
		int32 Total = 0;
	};

	struct FStats
	{
		bool bOk = false;
		bool bCancelled = false;
		FString FatalError;
		double Seconds = 0.0;
		/** Seconds per stage ("scan", "unpack", "analyse", "convert", "data"). */
		TArray<TPair<FString, double>> StageSeconds;
		TArray<FString> ConvertedMaps;
		TArray<FString> RemovedMaps;
		int32 UnchangedMaps = 0;
		/** Script packages whose own assets were exported (ScriptAssetsVersion). */
		int32 ScriptPackagesExported = 0;
		int32 UnplayableMaps = 0;
		/** Converted maps (stock) that were exported again because their map schema was older. */
		TArray<FString> UpdatedMaps;
		int32 Errors = 0;
	};

	/** <cache>/usercontent/state.json */
	FString StatePath(const FString& CacheDir);
	FString StatePath(const FString& CacheDir, const FString& StateDirName);

	/** Converts what changed (blocking; any thread). Progress is called on the same thread. */
	bool Run(const FOptions& Options, TFunctionRef<void(const FProgress&)> OnProgress, const TAtomic<bool>* Cancel, FStats& OutStats);

	/**
	 * Cheap check (directory listing + file sizes / dates only): true when the custom content folder differs from what
	 * the last Run saw, the install changed, or a converted map has an older map schema. False when the folder is empty,
	 * nothing was converted before and every map is current.
	 */
	bool HasChanges(const FOptions& Options);
}

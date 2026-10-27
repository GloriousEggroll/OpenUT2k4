#pragma once

#include "CoreMinimal.h"

/**
 * In-game UT2004 content converter: turns the player's own UT2004 install into the content cache
 * (ArenaContent::GetCacheDir()) that the runtime asset provider reads. C++ port of Tools/ut2k4_export.py
 * (+ the cache-side parts of Tools/ut2k4_materials.py: texture_stats.json and baked Combiner textures)
 * and of the game-data part of Tools/ImportUT2004.py (data/<Map>.json marker files and data/Assets tables).
 *
 * Runs on its own thread (pixel decoding and file writes on the task graph), can be cancelled at any time
 * and resumed later: units (music, asset packages, maps) that an earlier run finished are skipped, and files
 * already written after their package last changed are reused. cache.json ({format_version, complete,
 * source install, counts}) is written last, so a cache is only "ready" (ArenaContent::IsCacheReady) once
 * everything is done.
 *
 * Output layout: see ArenaContent.h. Asset paths in data/ keep the editor importer's form
 * "/Game/Imported/<Kind>/<Pkg>/<Name>"; runtime consumers strip "/Game/Imported/" to get the provider's
 * relative path.
 *
 * Command line (developer / tests):
 *   -ConvertOnly                 run a conversion right after engine start, then quit (no menu, no map)
 *   -ConvertTo=<dir>             output folder (default: ArenaContent::GetCacheDir())
 *   -ConvertMaps=A,B,...         only these maps (+ everything they reference) instead of the whole install
 *   -ConvertForce                re-export everything (no resume)
 *   -ConvertCompare=<dir>        after converting, compare the output with a reference cache made by
 *                                Tools/ut2k4_export.py (json values, PNG pixels, glTF counts / bounds, wav sizes)
 *   -ConvertCompareOnly          skip the conversion, only compare the -ConvertTo folder
 *   -ConvertFontsOnly            only export the install's fonts (fonts/, ArenaConvFonts.h) into the cache, then quit
 *   -ConvertCompareData=<dir>    reference for data/ (default: <project>/Content/ImportedData)
 *   -UT2004Dir=<dir>             the install (else ArenaContent::FindUT2004Install())
 */
struct OPENUT2K4_API FArenaConvertProgress
{
	bool bRunning = false;
	bool bFinished = false;
	bool bSucceeded = false;
	bool bCancelled = false;
	/** Human readable stage ("Textures and meshes", "Maps", ...) and current item. */
	FString Stage;
	FString Item;
	/** 0..1 over the whole conversion. */
	float Fraction = 0.f;
	int32 UnitsDone = 0;
	int32 UnitsTotal = 0;
	int32 ErrorCount = 0;
	/** A few of the latest per-asset errors (the full list is in <cache>/convert_errors.log). */
	TArray<FString> RecentErrors;
	/** Fatal error (no install, cannot write the cache folder...). */
	FString FatalError;
	double ElapsedSeconds = 0.0;
};

namespace ArenaConverterVersions
{
	/**
	 * schema_version written to maps/<Map>/map.json. A cached map with a lower version lacks data the game uses
	 * (4: BSP zone tree, sky zone surfaces, volume brushes, emitter / scripted action subobjects, UE2 class defaults;
	 * 5: every texture / material / mesh / sound inside the map package is exported, also when only script code or
	 * class defaults use it; 6: invisible solid BSP surfaces as "collision_only" primitives in bsp.gltf; 7: the map's
	 * static meshes are exported again with their UE2 collision model, static mesh json tool_version 2; 8: sky
	 * (FakeBackdrop) surfaces as collision only "fake_backdrop" primitives; 9: textured zone portal surfaces such as
	 * water are drawn) and is exported again.
	 */
	constexpr int32 MapSchemaVersion = 9;

	/**
	 * "version" of <cache>/fonts/index.json (ArenaConvFonts.h). A cache whose fonts index is missing or older gets the
	 * Font objects of the install exported (fonts/<Pkg>/<Name>.json, 1: glyph table, remapping, kerning, pages) by
	 * the next conversion or the next custom content pass at startup, without converting anything else again.
	 */
	constexpr int32 FontsVersion = 1;

	/**
	 * "version" of <cache>/localization/index.json (ArenaConvLocalization.h; 1: the install's System .int / .ucl files, 2: also the .upl character lists).
	 * An older or missing index gets the files copied by the next conversion or the next startup content pass.
	 */
	constexpr int32 LocalizationVersion = 2;

	/**
	 * "version" of <cache>/scripts/index.json (ArenaConvScripts.h; 1: System .u / .ini files and the maps that define
	 * script classes). An older or missing index gets them copied by the next conversion or startup content pass.
	 */
	constexpr int32 ScriptsVersion = 1;
}

class OPENUT2K4_API FArenaConverter
{
public:
	struct FOptions
	{
		/** UT2004 install (System/, Maps/...). */
		FString InstallDir;
		/** Output folder; empty = ArenaContent::GetCacheDir(). */
		FString CacheDir;
		/** Only these maps (and what they reference); empty = the whole install. */
		TArray<FString> Maps;
		/** Re-export everything instead of resuming. */
		bool bForce = false;
		/** zlib level of the PNGs (0-9). */
		int32 PngLevel = 6;
	};

	static FArenaConverter& Get();

	/** Starts a background conversion; false if one is running or the options are invalid (see progress). */
	bool Start(const FOptions& Options);
	/** Converts on the calling thread (tests, -ConvertOnly). Returns true on success. */
	bool RunBlocking(const FOptions& Options);
	/** Asks a running conversion to stop (it stops between objects; the cache stays resumable). */
	void Cancel();
	bool IsRunning() const;
	/** Cancels and waits for a running conversion (engine exit). */
	static void Shutdown();
	FArenaConvertProgress GetProgress() const;

	/**
	 * Compares a converted cache with a reference (structural diff, see the command line help). Writes a
	 * report to <Converted>/compare_report.txt and the log; returns the number of mismatching files.
	 */
	static int32 CompareCaches(const FString& Converted, const FString& Reference, const FString& ReferenceData, const TArray<FString>& Maps);

	~FArenaConverter();

private:
	FArenaConverter() = default;
	bool Run(FOptions Options);
	void SetStage(const FString& Stage);

	mutable FCriticalSection Lock;
	FArenaConvertProgress Progress;
	TAtomic<bool> bCancel{ false };
	TAtomic<bool> bBusy{ false };
	class FRunnableThread* Thread = nullptr;
	class FArenaConvertRunnable* Runnable = nullptr;
	friend class FArenaConvertRunnable;
};

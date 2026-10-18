#pragma once

#include "CoreMinimal.h"

/**
 * Custom content: community maps (and the texture / mesh / sound / music / script packages they ship) that players add
 * in UT2004 format, without touching their UT2004 install.
 *
 * Folder: ArenaContent::GetUserContentDir() (GetUserDataDir()/UserContent, -UserContent=<dir> overrides). Files may sit
 * in the UE2 layout (Maps/, Textures/, StaticMeshes/, Sounds/, Music/, System/, Animations/, KarmaData/), loose at the
 * top level or in any subfolder: they are sorted by extension (.ut2 maps, .utx .usx .uax .ukx .u packages, .ogg music,
 * .int / .ucl map titles, .upl character lists, .umx UT99 music). .umod / .ut2mod installers and .zip archives are
 * unpacked into the content cache (<cache>/usercontent/unpacked); .7z / .rar must be unpacked by the player. The game
 * never changes the folder (it only creates it, empty, so players can find it).
 *
 * Conversion is incremental (Convert/ArenaConvUserContent.h): on startup (when the cache is ready and the folder
 * changed), after a full conversion, and from the setup screen ("Rescan custom content"), only new or changed maps -
 * and the maps that use a changed package - are converted into the same content cache; cache entries of deleted files
 * are removed. Custom packages are found before the install's, except that a package or map with the same name as a
 * UT2004 one never replaces it (the shared cache stays valid for the stock maps).
 *
 * Results (<cache>/usercontent/state.json) feed the Instant Action map list (FArenaLayoutInfo::bCustom, title, author,
 * players, screenshot, notice) and the setup screen's custom content page, which names the reason of every map that
 * cannot be played (missing packages by name, damaged files, no player starts, conversion errors) and of maps that
 * play with limits (custom UnrealScript classes until the script runtime supports them: ScriptClassSupport()).
 *
 * Command line: -UserContent=<dir>, -NoUserContent (no scans), -UserContentScan (scan once right after engine start,
 * log the timings and quit; -UserContentForce converts every custom map again), -UserContentShowList (test hook:
 * opens the setup screen's custom content page).
 */
namespace ArenaUserContent
{
	enum class EMapStatus : uint8
	{
		Ready,
		/** Plays, but parts are left out (Problems says what). */
		Limited,
		Unplayable
	};

	struct FMapInfo
	{
		/** Package name, e.g. "DM-Aerowalk". */
		FString Name;
		FString Title;
		FString Author;
		FString Description;
		int32 PlayersMin = 0;
		int32 PlayersMax = 0;
		/** Texture object path of the LevelInfo screenshot (ArenaRuntimeContent::LoadTextureByObjectPath), or empty. */
		FString Screenshot;
		/** The .ut2 file, and the archive it came from (if any). */
		FString File;
		FString Archive;
		EMapStatus Status = EMapStatus::Ready;
		/** Plain-language reasons (why it cannot be played / what is left out). */
		TArray<FString> Problems;
		/** Packages the map needs that were not found. */
		TArray<FString> MissingPackages;
		/** "Package.Class" of objects whose class comes from a custom script package or the map itself, with counts. */
		TArray<TPair<FString, int32>> ScriptClasses;
	};

	/** A file that could not be used, or a note about one (duplicates, unsupported formats...). */
	struct FProblem
	{
		FString File;
		FString Reason;
		bool bError = true;
	};

	struct FScanStatus
	{
		bool bRunning = false;
		bool bFinished = false;
		bool bOk = false;
		bool bCancelled = false;
		FString Stage;
		FString Item;
		float Fraction = 0.f;
		int32 Done = 0;
		int32 Total = 0;
		double ElapsedSeconds = 0.0;
		FString FatalError;
		/** Last finished scan. */
		TArray<FString> ConvertedMaps;
		TArray<FString> RemovedMaps;
		/** Converted UT2004 maps exported again because their map schema was older. */
		TArray<FString> UpdatedMaps;
		int32 UnchangedMaps = 0;
	};

	/** Scans run only with runtime content, a ready cache and a UT2004 install (and without -NoUserContent). */
	OPENUT2K4_API bool CanScan();
	/** Starts a background scan + incremental conversion; false if one runs, the full converter runs, or !CanScan(). */
	OPENUT2K4_API bool StartScan(bool bForce = false);
	OPENUT2K4_API void CancelScan();
	OPENUT2K4_API bool IsScanning();
	OPENUT2K4_API FScanStatus GetStatus();
	/** Cheap check whether the folder changed since the last scan (directory listing + sizes / dates). */
	OPENUT2K4_API bool HasChanges();

	/** Custom maps of the last scan (game thread; reloaded after every scan). */
	OPENUT2K4_API const TArray<FMapInfo>& GetMaps();
	/** Custom map by package name (case-insensitive), or null. */
	OPENUT2K4_API const FMapInfo* FindMap(const FString& MapName);
	/** Files that could not be used and notes. */
	OPENUT2K4_API const TArray<FProblem>& GetProblems();
	/**
	 * Custom script packages (.u) for the UnrealScript runtime: package name -> file (unpacked from archives when
	 * needed). Their .int files (localisation, class lists) are in the same list under "<Name>.int" when present.
	 */
	OPENUT2K4_API TMap<FString, FString> GetScriptPackages();

	/**
	 * Set by the UnrealScript runtime: true when it can run the class "Package.Class". While unset (or false for a
	 * class) a map with such objects is listed as Limited with "uses custom scripts (not supported yet)".
	 * Call RefreshMapStatus() after changing it.
	 */
	OPENUT2K4_API TFunction<bool(const FString& ClassPath)>& ScriptClassSupport();
	/** Re-reads the scan results and recomputes the map status (e.g. after ScriptClassSupport changed). */
	OPENUT2K4_API void RefreshMapStatus();
}

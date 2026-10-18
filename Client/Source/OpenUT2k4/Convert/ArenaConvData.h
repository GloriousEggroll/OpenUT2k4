#pragma once

#include "CoreMinimal.h"
#include "ArenaConvJson.h"

/**
 * Cache post-processing after the export: texture statistics and baked Combiner textures
 * (Tools/ut2k4_materials.py stats / bake) and the game-side data under <cache>/data
 * (Tools/ImportUT2004.py slim_markers -> data/<Map>.json, and the asset tables data/Assets json tables).
 */
namespace ArenaConvData
{
	struct FResult
	{
		TArray<FString> Errors;
		TMap<FString, int32> Counts;
	};

	/**
	 * Combiner Add / Add_With_Mask_Modulation of two plain textures -> textures/<Pkg>/<Group.Name>_Baked.png + .json.
	 * OnlyPackages (lower-case material folder names): only the combiners of these packages (custom content).
	 */
	void BakeCombiners(const FString& CacheDir, TMap<FString, FConvJson>& NewStats, FResult& Result, const TSet<FString>* OnlyPackages = nullptr);

	/** texture_stats.json: NewStats (object path -> stats) merged with up-to-date entries, the rest sampled from the PNGs. */
	void WriteTextureStats(const FString& CacheDir, const TMap<FString, FConvJson>& NewStats, FResult& Result, const TAtomic<bool>* Cancel);

	/**
	 * data/Assets/{Music,Sounds,SkeletalMeshes,Animations}.json and data/<Map>.json for every maps/<Map>/map.json.
	 * OnlyMaps (custom content): data/Assets/Music.json and data/<Map>.json of these maps only.
	 */
	void WriteGameData(const FString& CacheDir, FResult& Result, const TArray<FString>* OnlyMaps = nullptr);

	/**
	 * Incremental texture_stats.json update (custom content): adds NewStats (object path -> stats, mtime taken from the
	 * PNG) and drops every entry of RemovedPackages (lower-case package names), without sampling the rest of the cache.
	 */
	void MergeTextureStats(const FString& CacheDir, const TMap<FString, FConvJson>& NewStats, const TSet<FString>& RemovedPackages, FResult& Result);

	/** Map marker file from a map.json document (ImportUT2004.py slim_markers). Music is the parsed Music table's "assets". */
	FConvJson SlimMarkers(const FConvJson& Doc, const FConvJson& MusicAssets);

	/** Same sanitising as ImportUT2004.py asset_name / asset_folder ([^A-Za-z0-9_-] -> _). */
	FString AssetName(const FString& Name);
	/** "/Game/Imported/<Kind>/<Pkg>/<Group_Name>" of a UT2004 object path. */
	FString ImportedPath(const TCHAR* Kind, const FString& ObjectPath);
	/** Sorts every object's keys (Python sort_keys=True). */
	void SortKeys(FConvJson& J);
}

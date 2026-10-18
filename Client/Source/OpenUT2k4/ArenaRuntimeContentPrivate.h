#pragma once

#include "CoreMinimal.h"
#include "ArenaRuntimeContent.h"
#include "Dom/JsonObject.h"

class UMaterialInterface;
class UTexture2D;

/**
 * Internals shared by the ArenaRuntime*.cpp files (not for game code: use ArenaRuntimeContent.h).
 */
namespace ArenaRuntimePrivate
{
	/** Same sanitising as Tools/ImportUT2004.py asset_name(): anything but [A-Za-z0-9_-] -> '_'. */
	FString AssetName(const FString& In);
	/** Same as the exporter's safe(): anything but [A-Za-z0-9._-] -> '_'. */
	FString CacheSafe(const FString& In);

	/**
	 * Cache file lookup by kind folder ("textures", "materials", "staticmeshes", "sounds") and object path,
	 * or by relative asset name. Case-insensitive, thread-safe, built lazily per kind on first use.
	 * Returns the full path without extension ("<cache>/textures/Pkg/Group.Name"), empty when missing.
	 */
	FString FindByObjectPath(const TCHAR* Kind, const FString& ObjectPath);
	FString FindByAssetName(const TCHAR* Kind, const FString& PackageFolder, const FString& AssetName);

	/** Parsed json file (nullptr when missing or broken). Not cached. */
	TSharedPtr<FJsonObject> LoadJson(const FString& File);

	/** Lazily loaded texture_stats.json entry (lowercase object path) or nullptr. Thread-safe. */
	const TSharedPtr<FJsonObject>* FindTextureStats(const FString& ObjectPath);

	/** Outer for runtime assets: the game instance when there is one (runtime physics cooking needs a game
	 *  world reachable through the outer chain), else the transient package. */
	UObject* AssetOuter();

	void Keep(UObject* Object, bool bPermanent);

	// ---- per-kind builders (game thread) ----
	UTexture2D* BuildTexture(const FString& ObjectPath);
	UMaterialInterface* BuildMaterial(const FString& ObjectPath, ArenaRuntimeContent::EMaterialVariant Variant);
	UMaterialInterface* GetMaster(const FString& Name);
	UMaterialInterface* DefaultMaterial();
	UStaticMesh* BuildCacheStaticMesh(const FString& ObjectPath);
	int32 SlotForSection(const FString& ObjectPath, int32 Section);
	/** Collision-only mesh of the cache mesh's UE2 collision model, null without one. */
	UStaticMesh* BuildCacheCollisionModel(const FString& ObjectPath);
	/** Whether the cache mesh has a UE2 collision model, and which traces use it (box: players, line: shots). */
	bool CollisionModelFlags(const FString& ObjectPath, bool& bOutBox, bool& bOutLine);
	USoundBase* BuildSound(const FString& ObjectPath);
	USoundBase* BuildMusic(const FString& Name);

	// ---- worker-thread prefetch ----
	void PrefetchTextures(const TArray<FString>& ObjectPaths);
	void PrefetchMeshData(const TArray<FString>& ObjectPaths);
	/** Base texture object paths the given materials resolve to (thread-safe). */
	TArray<FString> MaterialBaseTextures(const TArray<FString>& MaterialPaths);
	/** Material object paths used by the given cache meshes (after PrefetchMeshData). */
	TArray<FString> MeshMaterials(const TArray<FString>& MeshPaths);

	void TrimTextures();
	/** Forget the parsed material graph nodes (cache rewritten). */
	void ResetMaterialGraph();
	void TrimMeshData();

	/** Timing / counters for GetStats(). */
	struct FStats
	{
		std::atomic<int32> Textures{ 0 }, Materials{ 0 }, Meshes{ 0 }, Sounds{ 0 };
		std::atomic<int64> TextureUs{ 0 }, MaterialUs{ 0 }, MeshUs{ 0 }, SoundUs{ 0 };
		std::atomic<int64> TextureBytes{ 0 };
		/** Sound generators created (= plays of runtime sounds and music). */
		std::atomic<int32> SoundPlays{ 0 };
	};
	FStats& Stats();

	struct FScopeUs
	{
		std::atomic<int64>& Target;
		double Start;
		explicit FScopeUs(std::atomic<int64>& InTarget) : Target(InTarget), Start(FPlatformTime::Seconds()) {}
		~FScopeUs() { Target += int64((FPlatformTime::Seconds() - Start) * 1e6); }
	};
}

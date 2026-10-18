#pragma once

#include "CoreMinimal.h"

class UObject;
class UClass;
class UTexture2D;
class UFont;
class UMaterialInterface;
class UStaticMesh;
class USoundBase;
class USoundWave;

/**
 * Runtime asset provider: builds engine assets from the user's converted UT2004 content cache
 * (ArenaContent::GetCacheDir()) instead of loading cooked /Game/Imported packages.
 *
 * Only used when ArenaContent::UseRuntimeContent() is true. Callers normally do not call this directly:
 * ArenaArt::LoadArt (and the other art loaders) resolve "/Game/Replacements/<rel>" first and then hand
 * the same relative path to LoadByRelativePath here instead of trying /Game/Imported/<rel>.
 *
 * Relative paths are the ones used everywhere in the game code and the asset tables, i.e. the
 * /Game/Imported path without its root:
 *     Textures/<Pkg>/<Group_Name>          (UT2004 "Pkg.Group.Name"; '.' and other unsafe chars -> '_')
 *     Materials/<Pkg>/<Group_Name>[_2S][_U] (material, or a plain texture used as a material;
 *                                           _2S / _U are the forced two-sided / unlit variants)
 *     StaticMeshes/<Pkg>/<Group_Name>
 *     Sounds/<Pkg>/<Group_Name>
 *     Music/<Name>
 *     SkeletalMeshes/..., Animations/...   (provided by loaders registered with RegisterLoader)
 * The map builder and the skeletal builder work with UT2004 object paths ("Pkg.Group.Name"), so every
 * loader also has an ...ByObjectPath form. Both forms resolve to the same cached object.
 *
 * Lifetime: everything built here is kept alive (rooted through a strong reference table) until
 * TrimCache() releases what is not referenced any more; the level builder calls it on map change.
 * All functions are game-thread only unless noted; expensive parsing (PNG decode, glTF read, wav/ogg
 * decode) can be done ahead of time on worker threads with the Prefetch* functions.
 *
 * Assets never touch disk and are not saved: they live in the transient package. Nothing here uses
 * editor-only APIs (works in cooked Development / Shipping builds).
 */
namespace ArenaRuntimeContent
{
	/** True when runtime content is in use and the cache folder exists (ArenaContent::UseRuntimeContent()). */
	OPENUT2K4_API bool IsActive();

	/**
	 * Generic entry point (used by ArenaArt::LoadArt): builds or returns the cached asset for a relative path.
	 * Class picks the kind when the path is ambiguous (e.g. UTexture vs UMaterialInterface); the first path
	 * segment picks the loader otherwise. Returns nullptr (silently) when the cache has no such asset.
	 */
	OPENUT2K4_API UObject* LoadByRelativePath(UClass* Class, const FString& RelativePath);

	// ---- textures ------------------------------------------------------------------------------

	/** "Textures/<Pkg>/<Name>" -> sRGB world texture with mips (normal maps / UI textures get their own settings). */
	OPENUT2K4_API UTexture2D* LoadTexture(const FString& RelativePath);
	/** "Pkg.Group.Name" (a UT2004 texture object path). */
	OPENUT2K4_API UTexture2D* LoadTextureByObjectPath(const FString& ObjectPath);

	// ---- materials -----------------------------------------------------------------------------

	enum class EMaterialVariant : uint8
	{
		None = 0,
		/** Force two-sided rendering (BSP TwoSided polys, mesh actors with bTwoSided...). */
		TwoSided = 1 << 0,
		/** Force unlit (BSP Unlit polys; makes lit masters fully emissive). */
		Unlit = 1 << 1,
	};
	ENUM_CLASS_FLAGS(EMaterialVariant)

	/**
	 * "Materials/<Pkg>/<Name>" (suffixes _2S/_U are understood) -> dynamic instance of a shipped master
	 * (/Game/Materials/UT2004/M_Imported*) with the same parent, Base texture and parameters the editor
	 * importer gives its material instances (Tools/ImportUT2004.py + Tools/ut2k4/matresolve.py rules).
	 * Never nullptr while active: unknown materials get the default material instance.
	 */
	OPENUT2K4_API UMaterialInterface* LoadMaterial(const FString& RelativePath, EMaterialVariant Variant = EMaterialVariant::None);
	/** "Pkg.Group.Name" of a material or texture; "None"/empty -> the default material. */
	OPENUT2K4_API UMaterialInterface* LoadMaterialByObjectPath(const FString& ObjectPath, EMaterialVariant Variant = EMaterialVariant::None);

	/**
	 * New per-object dynamic instance that looks like Source. Runtime materials are shared dynamic instances, and a
	 * dynamic instance cannot be the parent of another one: for those the new instance gets Source's parent and a copy
	 * of its parameters. Use this (never CreateDynamicMaterialInstance, which hands back a shared instance already in
	 * the slot) when changing parameters per actor. Works for any material, with or without runtime content.
	 */
	OPENUT2K4_API class UMaterialInstanceDynamic* CreateMaterialInstance(UMaterialInterface* Source, UObject* Outer);

	// ---- static meshes -------------------------------------------------------------------------

	/**
	 * "StaticMeshes/<Pkg>/<Name>" -> UStaticMesh built from the cached glTF: materials assigned per section,
	 * complex-as-simple collision on colliding sections (double-sided), as the editor importer does.
	 * No Nanite, no lightmap UVs, no distance fields (runtime builds cannot make them).
	 */
	OPENUT2K4_API UStaticMesh* LoadStaticMesh(const FString& RelativePath);
	OPENUT2K4_API UStaticMesh* LoadStaticMeshByObjectPath(const FString& ObjectPath);

	/**
	 * Material slot of a cache-mesh section (the glTF primitive "section" extra), for per-section skin
	 * overrides on map actors: one slot per (material, collides) pair in first-use order, as the editor
	 * importer's mesh_slots(). -1 if the mesh or the section is unknown. Loads the mesh if needed.
	 */
	OPENUT2K4_API int32 GetStaticMeshSlotForSection(const FString& ObjectPath, int32 SectionIndex);

	/**
	 * The UE2 collision model of a cache static mesh (StaticMesh.CollisionModel: hulls built from brushes) as a
	 * collision-only mesh (never drawn), or null when the mesh has none or a /Game/Replacements mesh replaces it.
	 * bOutBox: players and everything else that moves with an extent collide with the model instead of the mesh's
	 * colliding sections (UE2 UseSimpleBoxCollision); bOutLine: shots and sight traces do (UseSimpleLineCollision).
	 * Some meshes have no colliding section at all and get their whole player collision from the model.
	 */
	OPENUT2K4_API UStaticMesh* LoadStaticMeshCollisionModel(const FString& ObjectPath, bool& bOutBox, bool& bOutLine);

	/** Raw geometry for BuildStaticMesh (BSP chunks, terrain, anything the map builder generates). */
	struct FMeshSection
	{
		/** Material object path ("Pkg.Group.Name" or "None") and variant. */
		FString Material;
		EMaterialVariant Variant = EMaterialVariant::None;
		bool bCollides = true;
		/** Triangle list. Winding as in the cache glTF (Unreal native). */
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		/** UV0 (required, may be zeros) and optional UV1. */
		TArray<FVector2f> UV0;
		TArray<FVector2f> UV1;
		/** Optional vertex colours (sRGB bytes). */
		TArray<FColor> Colors;
		TArray<uint32> Indices;
	};

	/**
	 * Builds a transient static mesh from sections (sections with the same Material/Variant/bCollides are
	 * merged into one material slot). Any colliding section makes the body complex-as-simple; non-colliding
	 * sections are left out of the collision mesh. Safe to call for hundreds of meshes; the expensive part
	 * (mesh description + render data build) is single-threaded per mesh, so callers may prepare FMeshSection
	 * arrays on worker threads. Name is used for the object name (made unique).
	 */
	OPENUT2K4_API UStaticMesh* BuildStaticMesh(const FString& Name, const TArray<FMeshSection>& Sections);

	/**
	 * Cooks the collision of the given runtime meshes on worker threads (in parallel) and blocks until all are
	 * done. Optional: a mesh whose collision was not cooked yet is cooked synchronously when its first
	 * component creates its physics state. The map builder calls this once with every mesh it built.
	 */
	OPENUT2K4_API void CookCollision(const TArray<UStaticMesh*>& Meshes);

	// ---- sounds and music ----------------------------------------------------------------------

	/** "Sounds/<Pkg>/<Name>" -> PCM USoundWave (looping flag from the cache json). */
	OPENUT2K4_API USoundBase* LoadSound(const FString& RelativePath);
	OPENUT2K4_API USoundBase* LoadSoundByObjectPath(const FString& ObjectPath);
	/** "Music/<Name>" or just "<Name>" (UT2004 song name) -> looping music wave, decoded from ogg. */
	OPENUT2K4_API USoundBase* LoadMusic(const FString& Name);
	/** UT2004 song names available in the cache (the .ogg files in music/), sorted. */
	OPENUT2K4_API TArray<FString> GetMusicNames();

	// ---- fonts ---------------------------------------------------------------------------------

	/**
	 * "Fonts/<Pkg>/<Name>" -> offline (bitmap) UFont built from the cache's fonts/<Pkg>/<Name>.json (the UE2 Font's
	 * glyph table, remapping and kerning; ArenaConvFonts.h) with its page textures. Drawn by UCanvas / FCanvasTextItem
	 * like any offline font, 1 texel = 1 pixel at scale 1, as UE2's Canvas drew it. Kept for the whole session.
	 * nullptr (silently) when the cache has no such font, e.g. a cache converted before fonts were exported; a miss is
	 * retried after a while, so fonts added by the startup custom content pass are picked up. Game thread.
	 */
	OPENUT2K4_API UFont* LoadFont(const FString& RelativePath);
	/** "Pkg.Name" (a UT2004 Font object path, e.g. "UT2003Fonts.FontEurostile24"). */
	OPENUT2K4_API UFont* LoadFontByObjectPath(const FString& ObjectPath);
	/** Line height (tallest glyph cell, the cache's max_height) of a font in pixels; 0 when the cache has no such font. */
	OPENUT2K4_API int32 GetFontCellHeight(const FString& ObjectPath);
	/**
	 * The font resampled so its line height is PixelHeight, to draw at scale 1 (1 texel = 1 pixel, no sampling blur):
	 * the pages are resampled on the CPU in premultiplied alpha (bicubic, an area filter when shrinking; enlarged
	 * glyph edges get a contrast curve so they stay sharp instead of soft), uncompressed BGRA, bilinear filtering,
	 * clamped; glyph cells, offsets and kerning are scaled with them. PixelHeight <= 0 or equal to the cell height
	 * gives the font at its own size (but with the same clean texture: no DXT, no point sampling). Variants are cached
	 * per (font, height); the least recently used ones are dropped beyond a few dozen. nullptr when missing. Game thread.
	 */
	OPENUT2K4_API UFont* LoadFontAtHeight(const FString& ObjectPath, int32 PixelHeight);

	// ---- path helpers --------------------------------------------------------------------------

	/** "Pkg.Group.Name" -> "<Kind>/<Pkg>/<Group_Name>" (Kind: Textures, Materials, StaticMeshes, Sounds...). */
	OPENUT2K4_API FString ObjectPathToRelative(const TCHAR* Kind, const FString& ObjectPath);
	/**
	 * Cache file for a relative path, e.g. ("Textures/Foo/Bar_Baz", ".png") -> <cache>/textures/Foo/Bar.Baz.png,
	 * resolving the name sanitising through a per-package index. Empty if missing. Thread-safe.
	 */
	OPENUT2K4_API FString FindCacheFile(const FString& RelativePath, const TCHAR* Extension);
	/** Same for an object path in a cache kind folder ("textures", "materials", "staticmeshes", "sounds", ...). */
	OPENUT2K4_API FString FindCacheFileByObjectPath(const TCHAR* KindFolder, const FString& ObjectPath, const TCHAR* Extension);

	// ---- extension points ----------------------------------------------------------------------

	/**
	 * Loader for a top-level kind folder ("SkeletalMeshes", "Animations"...), called by LoadByRelativePath
	 * for paths starting with "<Kind>/". The skeletal builder registers its loaders at module startup.
	 * The provider caches results (also misses) by (Class, path), so loaders need no cache of their own
	 * for that, but they must return objects kept alive by themselves or pass them to KeepAlive.
	 */
	using FLoader = TFunction<UObject*(UClass* Class, const FString& RelativePath)>;
	OPENUT2K4_API void RegisterLoader(const FString& Kind, FLoader Loader);

	/** Keeps an object alive until the next TrimCache (or forever with bPermanent: HUD fonts etc.). */
	OPENUT2K4_API void KeepAlive(UObject* Object, bool bPermanent = false);

	// ---- caching and prefetch ------------------------------------------------------------------

	/**
	 * Decodes textures / reads meshes for the given UT2004 object paths on worker threads so the following
	 * Load* calls only create the engine objects. Blocks until done. Map loading calls this with every
	 * mesh and material the map uses.
	 */
	OPENUT2K4_API void PrefetchStaticMeshes(const TArray<FString>& ObjectPaths);
	OPENUT2K4_API void PrefetchMaterials(const TArray<FString>& ObjectPaths);

	/**
	 * Releases built assets that nothing else references any more (call after a map change).
	 * Assets marked permanent (and everything loaded by ArenaArt for players / weapons / HUD) stay.
	 */
	OPENUT2K4_API void TrimCache();

	/**
	 * Forgets the cache file index and every lookup result (also misses), e.g. after the converter has (re)written
	 * the cache. Assets already built stay valid. Game thread.
	 */
	OPENUT2K4_API void ResetCacheIndex();
	/** Files of one kind folder ("sounds", "textures"...) were added to the cache: index that folder again, forget misses. */
	OPENUT2K4_API void OnCacheFilesAdded(const TCHAR* Kind);

	/** Stats line for logs: counts and time spent per kind. */
	OPENUT2K4_API FString GetStats();
}

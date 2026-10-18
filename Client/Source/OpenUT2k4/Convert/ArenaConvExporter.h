#pragma once

#include "CoreMinimal.h"
#include "ArenaConvJson.h"
#include "ArenaConvPackage.h"
#include "Tasks/Task.h"

/**
 * Exports UT2004 objects into the content cache (C++ port of Tools/ut2k4/exporter.py, level.py, skeletal.py,
 * skelexport.py, staticmesh.py, sound.py, music.py and the material resolution of texture.py):
 *   textures/<Pkg>/<Group.Name>.png + .json, materials/..json, staticmeshes/..gltf/.bin/.json,
 *   sounds/..wav|ogg + .json, skeletalmeshes / animations / vertmeshes ..gltf + .json, music/<Name>.ogg + .json,
 *   maps/<Map>/map.json + bsp.gltf, fonts/<Pkg>/<Name>.json (glyph tables of UE2 Font objects).
 *
 * The object graph walk (what to export, dependencies of every map) runs on the calling thread like the
 * Python exporter; pixel decoding, PNG compression and the file copies run as background tasks.
 * Call Flush() before reading anything the tasks write.
 */
class FConvExporter
{
public:
	struct FOptions
	{
		FString OutDir;
		double Scale = 2.0;
		int32 PngLevel = 6;
		/** Resume: keep files an earlier run wrote after the package last changed. */
		bool bSkipExisting = false;
		/** In-flight budget of the background tasks (decoded pixels + compressed output). */
		int64 MaxPendingBytes = 1536ll << 20;
		/** Run the tasks on background workers (in-game conversion, keeps the menu responsive). */
		bool bBackgroundTasks = false;
	};

	FConvExporter(FConvPackageManager& InManager, const FOptions& InOptions);
	~FConvExporter();

	// ---- units of work ----
	void ExportPackage(const FString& Name);
	/** Map package name (or a .ut2 file path); returns false when the map was not exported. */
	bool ExportMap(const FString& Name);
	FString ExportMusicFile(const FString& Path);
	TArray<FString> MusicFiles() const;
	/** Waits for every background task. */
	void Flush();

	// ---- graph ----
	FString ExportRef(const FConvRef& Ref);
	FString ExportObject(FConvObject* Obj);
	/** Canonical full path of a reference (target package's own names). */
	FString Canon(const FConvRef& Ref);
	FString KindOf(FConvObject* Obj);
	/** Output file (cache-relative) of an exported object path, optionally of one kind folder. */
	FString FileFor(const FString& Path, const FString& Kind = FString()) const;
	void BeginCollect();
	/** {kind: [{'path', 'file'}]} of everything exported or reused since BeginCollect. */
	FConvJson EndCollect();

	// ---- json ----
	FConvJson Jsonify(const FConvProp& V);
	FConvJson JsonifyProps(const FConvProps& Props);
	void CollectRefs(const FConvProp& V, TArray<FConvRef>& Out) const;
	void CollectRefs(const FConvProps& Props, TArray<FConvRef>& Out) const;

	// ---- logging / progress ----
	void Error(const FString& What, const FString& Message);
	/** Errors since the last call (for the per-unit error log). */
	TArray<FString> TakeNewErrors();
	TArray<FString> Errors;
	/**
	 * Informational notes: data the original game cannot use either (dangling references, textures whose pixels
	 * were stripped from the package and get a flat stand-in). Not errors; written to convert_notes.log.
	 */
	void Note_(const FString& Message);
	TArray<FString> TakeNewNotes();
	TArray<FString> Notes;
	TMap<FString, int32> Counts;
	/** Set from another thread to stop between objects. */
	TAtomic<bool>* CancelFlag = nullptr;
	bool IsCancelled() const { return CancelFlag && CancelFlag->Load(); }
	/** Texture statistics produced by the texture tasks (object path -> stats json). */
	TMap<FString, FConvJson> TakeTextureStats();
	/** Current item for progress display. */
	FString CurrentItem;

	FConvPackageManager& Mgr;
	FOptions Opt;

	// ---- helpers shared by the export kinds ----
	FString OutPath(const TCHAR* Kind, FConvObject* Obj, const TCHAR* Ext);
	FString Rel(const FString& Path) const;
	bool WriteJson(const FString& Path, const FConvJson& Data);
	bool Fresh(FConvObject* Obj, const FString& MainPath, const FString& MetaPath = FString(), int32 Version = 1);
	static FString Safe(const FString& Name);

	/** texture.py resolve_material: the first texture of a material graph and the outermost blend flags. */
	struct FMatResolve
	{
		FConvObject* BaseTexture = nullptr;
		FString BlendMode;
		bool bTwoSided = false;
		bool bUnlit = false;
		FConvJson Chain = FConvJson::Array();
		FConvJson Emissive, Opacity, Specular, Detail;
		FConvJson ShaderOutputBlending, FrameBufferBlending, AlphaRef, ConstantColor;
		bool bHasShaderOutput = false, bHasFrameBuffer = false, bHasAlphaRef = false, bHasConstantColor = false;
	};
	FMatResolve ResolveMaterial(FConvObject* Obj);
	/** (USize, VSize) of a Texture from props or the first mip (256 fallback). */
	FIntPoint TextureSize(FConvObject* Tex);

private:
	friend class FConvMapExporter;
	FString ExportKind(FConvObject* Obj, const FString& Kind);
	FString ExportTexture(FConvObject* Obj);
	FString ExportMaterial(FConvObject* Obj);
	FString ExportStaticMesh(FConvObject* Obj);
	FString ExportSound(FConvObject* Obj);
	FString ExportSkeletalMesh(FConvObject* Obj);
	FString ExportAnimation(FConvObject* Obj);
	FString ExportVertMesh(FConvObject* Obj);
	/** fonts/<Pkg>/<Name>.json: glyph table, remapping and kerning of a UE2 Font, its pages exported as textures (ArenaConvFonts.cpp). */
	FString ExportFont(FConvObject* Obj);
	void Note(const FString& Key);

	/** Launches a background task; blocks while too much work is pending. */
	void Launch(int64 Bytes, TUniqueFunction<void()> Work);

	TMap<FString, FString> Done;
	TMap<FString, FString> Paths;
	TMap<FString, TArray<FString>> ByPath;
	TMap<FString, TSet<FString>> ObjDeps;
	TArray<TSet<FString>> Collect;
	TMap<FString, FString> CanonCache;
	int32 ErrorsTaken = 0;
	int32 NotesTaken = 0;
	TArray<FString> TaskNotes;

	// animation -> meshes index (exporter.py anim_mesh_index)
	TMap<FString, TArray<FString>>* AnimMeshes = nullptr;
	const TMap<FString, TArray<FString>>& AnimMeshIndex();

	// background tasks
	TArray<UE::Tasks::FTask> Tasks;
	TAtomic<int64> PendingBytes{ 0 };
	FCriticalSection TaskLock;
	TArray<FString> TaskErrors;
	TMap<FString, FConvJson> TextureStats;
};

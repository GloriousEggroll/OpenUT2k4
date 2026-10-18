#pragma once

#include "CoreMinimal.h"
#include "UScriptCore.h"

class FConvPackage;
class FConvPackageManager;
struct FConvReader;

/** Loads one package's script objects (UE2 ULinkerLoad). */
class FUSLinker
{
public:
	FUSLinker(FUSRuntime* InRuntime, FConvPackage* InPkg);

	FUSRuntime* Runtime = nullptr;
	FConvPackage* Pkg = nullptr;
	FName PackageName;
	FUSObject* PackageObject = nullptr;
	/** Package name table as FNames. */
	TArray<FName> Names;

	/** Object for a compact package index (>0 export, <0 import). Creates shells; loads field bodies when bLoad. */
	FUSObject* IndexToObject(int32 Index, bool bLoad = false);
	FUSObject* GetExport(int32 Ei, bool bLoad = false);
	FUSObject* GetImport(int32 Ii);
	/** Loads a field's body (class, struct, function, state, property, enum, const). */
	bool LoadField(FUSField* Field);
	/** Export index of a top-level class by name, or INDEX_NONE. */
	int32 FindClassExport(FName ClassName) const;
	/** Export by relative path ("Group.Name") and class name. */
	int32 FindExport(const FString& RelPath, const FString& ClassName) const;
	FName NameAt(int32 I) const { return Names.IsValidIndex(I) ? Names[I] : FName(); }
	int32 NumExports() const { return Exports.Num(); }
	/** True for exports that are top-level classes. */
	bool IsTopLevelClass(int32 Ei) const;
	/** Class object of an export (null for classes themselves or non-script classes). */
	FUSClass* GetExportClass(int32 Ei);
	FString GetExportName(int32 Ei) const;
	/** Makes references to export Ei resolve to Obj (map actors instantiated in a script world). */
	void SetExportObject(int32 Ei, FUSObject* Obj);
	/** Reads the export's saved properties (map actors) into an instance of its class. */
	bool LoadInstanceProperties(int32 Ei, FUSObject* Obj);

	/** Errors while loading (for diagnostics / CanRun). */
	TArray<FString> Errors;

private:
	FUSObject* CreateExport(int32 Ei);
	bool LoadStruct(FUSStruct* Struct, FConvReader& R);
	bool LoadProperty(FUSProperty* Prop);
	bool LoadClassBody(FUSClass* Class);
	bool SerializeScript(FConvReader& R, FUSStruct* Owner, int32 ScriptSize);
	bool SerializeExpr(FConvReader& R, TArray<uint8>& Out, int32 Depth);
	bool LinkStruct(FUSStruct* Struct);
	TArray<FUSObject*> Exports;
	TArray<FUSObject*> Imports;
	TArray<bool> ImportResolved;
};

/**
 * Process-wide UScript runtime: package/class cache, name and reference tables, native bindings.
 * Packages come from the player's UT2004 install (and UserContent); nothing is shipped.
 */
class FUSRuntime
{
public:
	static FUSRuntime& Get();
	~FUSRuntime();

	/** Opens the package search paths of a UT2004 install. Safe to call again (no-op when initialised). */
	bool Initialize(const FString& InGameDir, FString& OutError);
	bool IsInitialized() const { return Packages != nullptr; }
	/** Extra package folders (UserContent): searched after the install. */
	void AddPackageFolder(const FString& Folder);
	/** Extra package files (UserContent): "<Name>.int" files are used for localization. */
	void AddPackageFiles(const TArray<FString>& Files);
	const FString& GetGameDir() const { return GameDir; }
	/** Folders added with AddPackageFolder (the install when the runtime runs from the content cache's scripts). */
	const TArray<FString>& GetExtraFolders() const { return ExtraFolders; }

	/** Loads a package by name (no extension). Null when missing/unreadable (error in OutError). */
	FUSLinker* LoadPackage(FName PackageName, FString* OutError = nullptr);
	/** Opens a package from a file path (maps, user packages). */
	FUSLinker* LoadPackageFile(const FString& Path, FString* OutError = nullptr);
	/** "Package.Class" -> loaded class (null if missing / failed). */
	FUSClass* FindClass(const FString& PathName, FString* OutError = nullptr);
	/** "Package[.Group].Name" of any class, loading the package. */
	FUSObject* FindObject(const FString& PathName, FUSClass* RequiredClass = nullptr);
	/** Finds an already-loaded class by short name (any package). */
	FUSClass* FindLoadedClass(FName ClassName) const;

	// ---- tables used by bytecode ----
	int32 AddName(FName Name);
	FName GetName(int32 Index) const { return Names.IsValidIndex(Index) ? Names[Index] : FName(); }
	int32 AddRef(FUSLinker* Linker, int32 PackageIndex);
	int32 AddRefObject(FUSObject* Obj);
	FUSObject* ResolveRef(int32 Index);

	// ---- intrinsic classes ----
	FUSClass* GetIntrinsicClass(FName ClassName);
	FUSClass* ObjectClass() const { return CoreObject; }
	/** Engine classes (null until Engine is loaded). */
	FUSClass* ActorClass = nullptr;
	FUSClass* PawnClass = nullptr;
	FUSClass* ControllerClass = nullptr;

	/** Placeholder for a non-script object (texture, sound, mesh ...) by full path. */
	FUSObject* GetPlaceholder(const FString& PathName, FUSClass* ObjClass);
	/**
	 * Gives a placeholder whose class is a script class with properties (Engine.Texture, Shader ...) property memory from
	 * its class defaults, once, then lets PlaceholderInit fill what the game knows about the object (a texture's USize /
	 * VSize from the content cache). Called the first time script reads a variable of the placeholder.
	 */
	bool MaterializePlaceholder(FUSObject* Obj);
	/**
	 * Set by the game (Net/UE2NetScriptHUD): true when a content object (texture, font, sound ... "Pkg.Group.Name" of
	 * that UE2 class) exists in the player's content, so DynamicLoadObject / FindObject give its placeholder without
	 * opening the content package.
	 */
	TFunction<bool(const FString&, const FString&)> ContentObjectExists;
	/** Set by the game (Net/UE2NetScriptHUD): fills a materialized placeholder's values. */
	TFunction<void(FUSObject*)> PlaceholderInit;
	/** The package object (outermost) for a name. */
	FUSObject* GetPackageObject(FName PackageName);

	// ---- natives ----
	/** Declaration of native function by index (for missing natives / inference). */
	FUSFunction* NativeDecls[4096] = {};
	/** Records a native that is declared but not implemented ("Class.Function"). */
	void NoteMissingNative(FUSFunction* Function);
	const TSet<FString>& GetMissingNatives() const { return MissingNatives; }
	/** Missing natives per package name (lower case). */
	TMap<FString, TSet<FString>> MissingNativesByPackage;

	/** Registers a metadata object (owned by the runtime). */
	void AddMetadata(FUSObject* Obj);

	/** All loaded linkers by lower-case package name. */
	TMap<FName, FUSLinker*> Linkers;

	/** Reads a value from the player's UT2004 ini files (System/UT2004.ini then User.ini). */
	bool GetConfigValue(const FString& Section, const FString& Key, TArray<FString>& OutValues);
	/** Localized string from System/<Package>.int. */
	bool GetLocalized(const FString& Package, const FString& Section, const FString& Key, FString& OutValue);

private:
	FUSRuntime() = default;
	void CreateIntrinsics();
	struct FIniFile { TMap<FString, TMap<FString, TArray<FString>>> Sections; bool bLoaded = false; };
	FIniFile* LoadIni(const FString& Path);

	FString GameDir;
	TArray<FString> ExtraFolders;
	/** Lower-case "<package>.int" -> file. */
	TMap<FString, FString> ExtraIntFiles;
	FConvPackageManager* Packages = nullptr;
	TArray<FName> Names;
	TMap<FName, int32> NameLookup;
	struct FRef
	{
		FUSObject* Obj = nullptr;
		FUSLinker* Linker = nullptr;
		int32 Index = 0;
	};
	TArray<FRef> Refs;
	TMap<FUSObject*, int32> RefLookup;
	TMap<FName, FUSClass*> Intrinsics;
	FUSClass* CoreObject = nullptr;
	TMap<FString, FUSObject*> Placeholders;
	TMap<FName, FUSObject*> PackageObjects;
	TArray<FUSObject*> Metadata;
	TArray<FUSLinker*> OwnedLinkers;
	TSet<FString> MissingNatives;
	TMap<FString, FIniFile> IniFiles;
	friend class FUSLinker;
};

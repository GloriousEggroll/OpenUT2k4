#pragma once

#include "CoreMinimal.h"

class FUE2BitReader;
class FUE2BitWriter;
class FConvPackage;
class FConvPackageManager;
class FUE2NetConnection;
struct FUSClass;
struct FUSField;
struct FUSFunction;
struct FUSProperty;

/**
 * The UE2 package map of one server connection (UPackageMap / UPackageMapLevel). See Docs/UE2Net.md "Package map".
 *
 * The server lists its packages with USES (name, GUID, generation). Both sides then number every object by
 * concatenating the export tables of the listed packages in USES order, each cut to the export / name counts of the
 * generation both sides have (min of the server's GEN= and ours, which we tell the server with HAVE). Static objects
 * (classes, map actors, textures...) travel as an index into that numbering, names as an index into the concatenated
 * name tables, and dynamic actors as their actor channel index.
 */

/** Header fields of a UE2 package file that the network protocol needs. */
struct FUE2PackageHeader
{
	uint16 Version = 0;
	uint16 Licensee = 0;
	uint32 PackageFlags = 0;
	int32 NameCount = 0;
	int32 ExportCount = 0;
	FGuid Guid;
	/**
	 * UE1 style packages (version < 68) keep their GUID as the last heritage table entry, usually at the end of the
	 * file: UE2ParsePackageHeader sets this file offset when that entry lies past the bytes it was given (-1: Guid is set).
	 */
	int64 GuidOffset = -1;
	/** (ExportCount, NameCount) of each generation, oldest first. */
	TArray<TPair<int32, int32>> Generations;
};

/** Reads the header of a package file (only the first bytes). */
bool UE2ReadPackageHeader(const FString& Path, FUE2PackageHeader& Out, FString* OutError = nullptr);
bool UE2ParsePackageHeader(const uint8* Data, int64 Size, FUE2PackageHeader& Out, FString* OutError = nullptr);
/** UE2 GUID text (FGuid::String in UE2: four 8-digit upper-case hex words). */
FString UE2GuidToString(const FGuid& Guid);
bool UE2GuidFromString(const FString& Text, FGuid& Out);

/** One USES entry and the local package matched to it. */
struct FUE2PackageInfo
{
	FString Name;
	FString FileName;
	FGuid Guid;
	uint32 PackageFlags = 0;
	int32 FileSize = 0;
	/** GEN= of the server. */
	int32 RemoteGeneration = 0;
	/** Generations of our file (0: no matching local package). */
	int32 LocalGeneration = 0;

	/** Local file (empty when missing). */
	FString LocalPath;
	bool bFound = false;
	/** Our file has the same GUID (the same package build). */
	bool bGuidMatch = false;

	int32 ObjectBase = 0;
	int32 ObjectCount = 0;
	int32 NameBase = 0;
	int32 NameCount = 0;

	/** Opened lazily for names / export paths. */
	FConvPackage* Pkg = nullptr;
	bool bPkgTried = false;
};

/** A replicated object reference: a static object (package + export) or an actor channel. */
struct FUE2ObjectRef
{
	/** Index into the package map list, or INDEX_NONE. */
	int32 PackageIndex = INDEX_NONE;
	/** Export index within that package (0-based). */
	int32 ExportIndex = INDEX_NONE;
	/** Actor channel index for dynamic actors (0: none). */
	int32 Channel = 0;

	bool IsNone() const { return PackageIndex == INDEX_NONE && Channel == 0; }
	bool IsStatic() const { return PackageIndex != INDEX_NONE; }
	bool IsDynamic() const { return Channel != 0; }
	bool operator==(const FUE2ObjectRef& O) const { return PackageIndex == O.PackageIndex && ExportIndex == O.ExportIndex && Channel == O.Channel; }
};

/**
 * Replicated fields of one class (FClassNetCache): the class's own replicated properties (CPF_Net) and functions
 * (FUNC_Net), numbered after the super class's, in the order of the class's field list in the .u file.
 */
struct FUE2ClassNetCache
{
	FUSClass* Class = nullptr;
	const FUE2ClassNetCache* Super = nullptr;
	int32 FieldsBase = 0;
	TArray<FUSField*> Fields;

	int32 GetMaxIndex() const { return FieldsBase + Fields.Num(); }
	/** Field by replication index (searches the super classes). */
	FUSField* GetFromIndex(int32 Index) const;
	/** Replication index of a field of this class or a super class (INDEX_NONE if it does not replicate). */
	int32 GetIndexOf(const FUSField* Field) const;
	/** Finds a replicated function by name (this class first). */
	FUSFunction* FindFunction(FName Name) const;
};

class FUE2PackageMap
{
public:
	FUE2PackageMap();
	~FUE2PackageMap();

	/** Adds a USES line ("GUID=... PKG=... FLAGS=... SIZE=... GEN=... FNAME=..."). */
	void AddUses(const FString& Params);
	/** Finds local files for every entry (install, content cache scripts, custom content) and compares GUIDs. */
	void MatchLocalPackages();
	/** Drops entries (packages the client told the server to SKIP). */
	void RemovePackages(const TArray<FString>& Names);
	/** Computes the object / name numbering (call after MatchLocalPackages). */
	void Compute();

	const TArray<FUE2PackageInfo>& GetList() const { return List; }
	int32 GetMaxObjectIndex() const { return MaxObjectIndex; }
	int32 GetMaxNameIndex() const { return MaxNameIndex; }
	/** Entries without a usable local package (missing or different GUID). */
	TArray<FString> GetMissing() const;
	/** Entries whose local generation differs from the server's (both are fine; HAVE tells the server ours). */
	TArray<FString> GetGenerationDifferences() const;

	// ---- static objects ----
	/** Static object index -> package / export. False when out of range. */
	bool IndexToObject(int32 Index, FUE2ObjectRef& Out) const;
	/** "Package.Group.Name" of a static object. */
	FString GetObjectPath(const FUE2ObjectRef& Ref);
	/** Class name of a static object's export ("Class" for classes, e.g. "Mover" for a map actor). */
	FString GetObjectClassName(const FUE2ObjectRef& Ref);
	/** Static index of a package's export (INDEX_NONE when not in the map). */
	int32 ObjectToIndex(int32 PackageIndex, int32 ExportIndex) const;
	/** Static index of a script object (class, function...) by its package / export. */
	int32 ObjectToIndex(const FUSField* Field) const;
	/** Package map entry of a package name (INDEX_NONE if not listed). */
	int32 FindPackage(const FString& PackageName) const;

	// ---- names ----
	bool IndexToName(int32 Index, FName& Out);
	/** Wire index of a name (the first package that has it), or GetMaxNameIndex() (None) when no package has it. */
	int32 NameToIndex(FName Name);

	// ---- server side (Net/UE2Server*) ----
	/** The other side's HAVE GUID=... GEN=n: numbers that package by the older generation both have. Call before MatchLocalPackages. */
	bool SetRemoteGeneration(const FGuid& Guid, int32 Generation);
	/** Package map entry with this GUID, or INDEX_NONE. */
	int32 FindPackageByGuid(const FGuid& Guid) const;
	/** A static object by path ("XWeapons.DamTypeRocket", "DM-Rankin.Mover0", "WeaponSounds.AssaultRifle.AssaultRifleFire"). */
	bool FindObjectByPath(const FString& Path, FUE2ObjectRef& Out);

	// ---- script classes ----
	/** The script class of a static object that is a class (loads it through the UScript runtime), or null. */
	FUSClass* GetClass(const FUE2ObjectRef& Ref, FString* OutError = nullptr);
	/** The class of a static object that is an instance (a map actor), or null. */
	FUSClass* GetInstanceClass(const FUE2ObjectRef& Ref, FString* OutError = nullptr);
	/** Replicated fields of a class (null when the class is not in the package map). */
	const FUE2ClassNetCache* GetClassNetCache(FUSClass* Class);

private:
	FConvPackage* GetPkg(int32 PackageIndex);
	bool EnsureScriptRuntime(FString* OutError);
	FUSClass* LoadClassExport(int32 PackageIndex, int32 ExportIndex, FString* OutError);

	TArray<FUE2PackageInfo> List;
	int32 MaxObjectIndex = 0;
	int32 MaxNameIndex = 0;
	FConvPackageManager* Files = nullptr;
	TMap<FUSClass*, TUniquePtr<FUE2ClassNetCache>> ClassCaches;
	TMap<FName, int32> PackageByName;
	/** Script packages checked against the package map GUID (lower-case name -> ok). */
	TMap<FString, bool> ScriptGuidOk;
	/** NameToIndex lookup (built on first use after Compute). */
	TMap<FName, int32> NameIndices;
	bool bNameIndicesBuilt = false;
};

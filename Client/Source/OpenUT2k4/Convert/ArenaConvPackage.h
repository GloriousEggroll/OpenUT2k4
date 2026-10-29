#pragma once

#include "CoreMinimal.h"

/**
 * Unreal Engine 2.5 package reader (UT2004 .ut2/.utx/.usx/.uax/.ukx/.u), a C++ port of
 * Tools/ut2k4/archive.py, package.py, properties.py and objects.py.
 *
 * Packages are memory mapped (read whole when mapping is unavailable). Objects load lazily; tagged
 * properties are parsed into FConvProp values with the same rules as the Python reader (array element
 * types from class declarations, UE2 struct size quirks, class defaults found by scanning the class export).
 * Not thread-safe: one FConvPackageManager per thread. The raw package bytes (FConvPackageData) are
 * reference counted so worker jobs can keep decoding from them after the manager closed the package.
 */

class FConvPackage;
class FConvPackageManager;
struct FConvObject;

/** Thrown-free error handling: readers set this and return defaults. */
struct FConvReadError
{
	FString Message;
};

/** Raw bytes of a package file. */
struct FConvPackageData
{
	~FConvPackageData();
	const uint8* Data = nullptr;
	int64 Size = 0;
	TArray64<uint8> Owned;
	class IMappedFileHandle* MappedFile = nullptr;
	class IMappedFileRegion* MappedRegion = nullptr;
};

/** Bounded cursor over package bytes (archive.py Reader). Errors set bError and return zeros. */
struct FConvReader
{
	const uint8* Buf = nullptr;
	int64 Pos = 0;
	int64 End = 0;
	bool bError = false;
	FString Error;

	FConvReader() = default;
	FConvReader(const uint8* InBuf, int64 InPos, int64 InEnd) : Buf(InBuf), Pos(InPos), End(InEnd) {}

	bool Fail(const FString& Msg) { if (!bError) { bError = true; Error = Msg; } return false; }
	bool Check(int64 N) { if (bError) return false; if (N < 0 || Pos + N > End) { return Fail(FString::Printf(TEXT("read past end (pos=%lld need=%lld end=%lld)"), Pos, N, End)); } return true; }
	int64 Remaining() const { return End - Pos; }

	uint8 U8() { if (!Check(1)) return 0; return Buf[Pos++]; }
	int16 I16() { int16 V = 0; Raw(&V, 2); return V; }
	uint16 U16() { uint16 V = 0; Raw(&V, 2); return V; }
	int32 I32() { int32 V = 0; Raw(&V, 4); return V; }
	uint32 U32() { uint32 V = 0; Raw(&V, 4); return V; }
	uint64 U64() { uint64 V = 0; Raw(&V, 8); return V; }
	float F32() { float V = 0; Raw(&V, 4); return V; }
	void Raw(void* Out, int64 N) { if (!Check(N)) { FMemory::Memzero(Out, N); return; } FMemory::Memcpy(Out, Buf + Pos, N); Pos += N; }
	void Skip(int64 N) { if (Check(N)) Pos += N; }
	/** Pointer to N bytes (advances). Null on error. */
	const uint8* Bytes(int64 N) { if (!Check(N)) return nullptr; const uint8* P = Buf + Pos; Pos += N; return P; }
	void Vector(float Out[3]) { Raw(Out, 12); }
	/** FCompactIndex. */
	int32 Index();
	FString FString_();
	FString CString();
	/** TLazyArray<BYTE> span: (offset, length) without copying. */
	bool LazyBytesSpan(bool bHasSkip, int64& OutOffset, int64& OutLen);
};

/** A reference to an object as seen from a package (compact object index). */
struct FConvRef
{
	FConvPackage* Pkg = nullptr;
	int32 Index = 0;
	bool IsSet() const { return Index != 0 && Pkg != nullptr; }
	explicit operator bool() const { return IsSet(); }
	FString Path() const;
	FString Name() const;
	FString ClassName() const;
	/** The object this reference points at (loading packages as needed), or null. */
	FConvObject* Load() const;
};

/** A parsed property value (properties.py). */
struct FConvProp
{
	enum class EKind : uint8 { None, Bool, Int, Float, Str, Ref, Vector, Rotator, Struct, Array };
	EKind Kind = EKind::None;
	bool B = false;
	int32 I = 0;
	double F = 0.0;
	float V[3] = { 0, 0, 0 };
	int32 R[3] = { 0, 0, 0 };
	FString S;
	FConvRef Ref;
	TArray<FConvProp> A;
	TArray<TPair<FString, FConvProp>> M;

	static FConvProp MakeInt(int32 In) { FConvProp P; P.Kind = EKind::Int; P.I = In; return P; }
	static FConvProp MakeFloat(double In) { FConvProp P; P.Kind = EKind::Float; P.F = In; return P; }
	static FConvProp MakeStr(const FString& In) { FConvProp P; P.Kind = EKind::Str; P.S = In; return P; }
	static FConvProp MakeBool(bool In) { FConvProp P; P.Kind = EKind::Bool; P.B = In; return P; }
	static FConvProp MakeRef(FConvPackage* Pkg, int32 Index) { FConvProp P; P.Kind = EKind::Ref; P.Ref.Pkg = Pkg; P.Ref.Index = Index; return P; }
	static FConvProp MakeVector(const float In[3]) { FConvProp P; P.Kind = EKind::Vector; P.V[0] = In[0]; P.V[1] = In[1]; P.V[2] = In[2]; return P; }
	static FConvProp MakeStruct() { FConvProp P; P.Kind = EKind::Struct; return P; }
	static FConvProp MakeArray() { FConvProp P; P.Kind = EKind::Array; return P; }

	bool IsNone() const { return Kind == EKind::None; }
	bool IsRef() const { return Kind == EKind::Ref; }
	bool IsStruct() const { return Kind == EKind::Struct; }
	bool IsArray() const { return Kind == EKind::Array; }
	bool IsNumber() const { return Kind == EKind::Int || Kind == EKind::Float || Kind == EKind::Bool; }
	/** Python truthiness. */
	bool Truthy() const;
	double Num(double Default = 0.0) const { return Kind == EKind::Int ? double(I) : Kind == EKind::Float ? F : Kind == EKind::Bool ? (B ? 1.0 : 0.0) : Default; }
	int32 Int(int32 Default = 0) const { return Kind == EKind::Int ? I : Kind == EKind::Bool ? (B ? 1 : 0) : Kind == EKind::Float ? int32(F) : Default; }

	// struct members
	const FConvProp* Find(const FString& Key) const;
	const FConvProp& Get(const FString& Key) const;
	bool Has(const FString& Key) const { return Find(Key) != nullptr; }
	void Set(const FString& Key, FConvProp Value);
	/** True for {'_raw': ...} placeholders of unparsed data. */
	bool IsRaw() const { return Kind == EKind::Struct && Has(TEXT("_raw")); }

	static const FConvProp& NoneValue();
};

/** Ordered property dictionary of an object. */
using FConvProps = TArray<TPair<FString, FConvProp>>;
const FConvProp* ConvFindProp(const FConvProps& Props, const FString& Key);

struct FConvExport
{
	int32 ClassIndex = 0;
	int32 SuperIndex = 0;
	int32 PackageIndex = 0;
	FString Name;
	uint32 Flags = 0;
	int32 Size = 0;
	int32 Offset = 0;
};

struct FConvImport
{
	FString ClassPackage;
	FString ClassName;
	int32 PackageIndex = 0;
	FString Name;
};

/** Lazily loaded UObject (objects.py UObject). */
struct FConvObject
{
	FConvPackage* Pkg = nullptr;
	int32 Ei = 0;

	const FConvExport& Export() const;
	const FString& Name() const;
	FString ClassName() const;
	FString Path() const;
	/** Class names from the object's class up to Object. */
	const TArray<FString>& ClassChain();
	/** Case-insensitive membership of any of the names in the class chain. */
	bool IsA(const TCHAR* A, const TCHAR* B = nullptr, const TCHAR* C = nullptr, const TCHAR* D = nullptr);
	const FConvProps& Props();
	/** Error text when the properties could not be parsed. */
	const FString& LoadError() { Props(); return Error; }
	/** Reader positioned after the tagged properties; bError set when they could not be parsed. */
	FConvReader NativeReader();
	FConvReader Reader() const;
	/** Property value with class defaults fallback (objects.py get()); null when missing. */
	const FConvProp* GetProp(const FString& Name, bool bWithDefaults = true);
	const FConvProp* Prop(const FString& Name) { return ConvFindProp(Props(), Name); }

private:
	friend class FConvPackage;
	void Load();
	bool bLoaded = false;
	bool bChain = false;
	int64 NativePos = -1;
	FString Error;
	FConvProps PropsData;
	TArray<FString> Chain;
};

class FConvPackage
{
public:
	FConvPackage(const FString& InPath, FConvPackageManager* InManager);
	~FConvPackage();

	bool Open(FString& OutError);

	FString PathOnDisk;
	FString Name;
	FConvPackageManager* Manager = nullptr;
	TSharedPtr<FConvPackageData, ESPMode::ThreadSafe> Data;
	uint16 Version = 0;
	uint16 Licensee = 0;
	uint32 PackageFlags = 0;
	TArray<FString> Names;
	TArray<FConvExport> Exports;
	TArray<FConvImport> Imports;
	int32 NoneNameIndex = -1;

	const uint8* Buf() const { return Data.IsValid() ? Data->Data : nullptr; }
	int64 BufSize() const { return Data.IsValid() ? Data->Size : 0; }
	/**
	 * Bytes an FBspNode (UModel nodes: level BSP, brushes, static mesh collision models) has after ExclusiveSphereBound
	 * besides the stock layout. Packages older than version 126 (pre-release UT2004 builds that some custom maps were
	 * saved with, e.g. DM-(ED)-CarPark, DM-Bartokk, DM-ShudderCreek at 121) store 16 more bytes there (zero in every map
	 * seen); stock maps and packages (126 - 128) do not. bsp.py NODE_EXTRA_PRE126.
	 */
	int32 BspNodeExtraBytes() const { return Version < 126 ? 16 : 0; }

	bool NameAt(int32 I, FString& Out) const;
	const FString& NameAtOr(int32 I) const;
	FString RefName(int32 Index) const;
	FString RefClassName(int32 Index) const;
	FString ExportClassName(int32 Ei) const;
	FString RefPath(int32 Index) const;
	FString ExportPath(int32 Ei) const { return RefPath(Ei + 1); }
	/** Export by path relative to this package ('Group.Name'), preferring ClassName. -1 if missing. */
	int32 FindExport(const FString& RelPath, const FString& ClassName = FString());
	TArray<int32> ExportsOfClass(std::initializer_list<const TCHAR*> ClassNames) const;
	/** The only export with this name and class anywhere in the package (any group), or -1. */
	int32 FindUniqueExport(const FString& ObjectName, const FString& ClassName) const;
	/** (package, export index) of the object a reference points at. */
	bool ResolveRef(int32 Index, FConvPackage*& OutPkg, int32& OutEi);
	FConvObject* GetObject(int32 Ei);
	FConvObject* FindObject(const FString& RelPath, const FString& ClassName = FString());
	FConvReader ExportReader(int32 Ei) const;
	/** Children export indices of an export (exports whose outer is it). */
	const TArray<int32>* Children(int32 Ei);

	FConvRef MakeRef(int32 Index) { FConvRef R; R.Pkg = this; R.Index = Index; return R; }

	/** Class defaults reference this package: keep its tables alive after it is closed. */
	bool bPinned = false;
	/** Drops the file bytes and parsed objects (tables stay for names and paths). */
	void Release() { Objects.Reset(); Data.Reset(); bPathIndex = false; PathIndex.Reset(); }
	bool IsReleased() const { return !Data.IsValid(); }

private:
	void BuildPathIndex();
	TMap<FString, TArray<int32>> PathIndex;
	bool bPathIndex = false;
	TMap<int32, TArray<int32>> ChildIndex;
	bool bChildIndex = false;
	TMap<int32, TUniquePtr<FConvObject>> Objects;
};

/** Finds and caches packages using the game's UT2004.ini paths (package.py PackageManager). */
class FConvPackageManager
{
public:
	explicit FConvPackageManager(const FString& InGameDir);
	~FConvPackageManager();

	FString GameDir;
	/** Lower-case package name -> file, first search path wins. */
	TMap<FString, FString> FileIndex;
	/** Sorted (by lower name) list of package files with an extension filter ('.ut2'), or all. */
	TArray<TPair<FString, FString>> PackageFiles(const TCHAR* Ext = nullptr) const;
	FString FindFile(const FString& Name) const;
	/**
	 * Custom content (ArenaUserContent): indexes extra package files from any folder by their name, searched after
	 * the install's own paths. A name the install already has keeps the install's file (the shared content cache must
	 * not change under the stock maps); such files are returned in OutShadowed.
	 */
	void AddPackageFiles(const TArray<FString>& Files, TArray<FString>* OutShadowed = nullptr);
	/** Lower-case names of the packages AddPackageFiles indexed. */
	TSet<FString> ExtraPackages;

	FConvPackage* LoadPackage(const FString& Name);
	FConvPackage* OpenFile(const FString& Path);
	FConvObject* ResolvePath(const FString& FullPath, const FString& ClassName = FString());
	/** Class names from a class up to Object (cached by class path). */
	const TArray<FString>& ClassChain(FConvPackage* Pkg, int32 ClassIndex);

	/** Merged default properties of a class and its superclasses (objects.py class_defaults). */
	const FConvProps& ClassDefaults(FConvPackage* Pkg, int32 ClassIndex);
	/** Dynamic array property name (lower) -> element kind, for a class or struct (array_hints). */
	const TMap<FString, FString>& ArrayHints(FConvPackage* Pkg, int32 Ei, int32 Depth = 0);

	/** Closes every open package (keeps class caches keyed by path, which stay valid). */
	void CloseAll();
	/** Closes one package by name. */
	void ClosePackage(const FString& Name);
	int32 NumOpen() const { return Packages.Num(); }

	/** Warnings (unreadable packages). */
	TArray<FString> Warnings;

private:
	const TMap<FString, FString>& DeclaredProperties(FConvPackage* Pkg, int32 Ei, int32 Depth = 0);
	FConvProps ScanDefaults(FConvPackage* Pkg, int32 Ei);
	bool StructOf(FConvPackage* Pkg, int32 PropEi, FConvPackage*& OutPkg, int32& OutEi);

	TArray<TPair<FString, FString>> SearchPaths;
	TMap<FString, TUniquePtr<FConvPackage>> Packages;
	TSet<FString> Missing;
	TMap<FString, TArray<FString>> ClassChainCache;
	TMap<FString, FConvProps> DefaultsCache;
	TMap<FString, TMap<FString, FString>> HintsCache;
	TMap<FString, TMap<FString, FString>> DeclaredCache;
	/** Closed packages are kept here until CloseAll returns (objects may still point at them). */
	TArray<TUniquePtr<FConvPackage>> Graveyard;
};

namespace ArenaConvProps
{
	/** Reads a tagged property list up to 'None' (properties.py read_properties). */
	bool ReadProperties(FConvReader& R, FConvPackage* Pkg, FConvProps& Out, const TMap<FString, FString>* Hints,
		TMap<FString, TPair<int32, FString>>* OutTypes = nullptr, int64 Limit = -1);
}

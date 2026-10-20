#include "UE2NetPackageMap.h"

#include "../ArenaContent.h"
#include "../ArenaUserContent.h"
#include "../Convert/ArenaConvPackage.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "../UScript/ArenaUScript.h"
#include "../UScript/UScriptRuntime.h"
#include "UE2NetDownload.h"
#include "UE2NetLog.h"

// =================================================================================================
// Package headers
// =================================================================================================

namespace UE2NetPackageMapLocal
{
	constexpr uint32 PackageTag = 0x9E2A83C1;

	/** Value of KEY=VALUE in a control message parameter list (case-insensitive key; empty if missing). */
	FString ParseValue(const FString& Params, const TCHAR* Key)
	{
		FString Value;
		FParse::Value(*Params, Key, Value);
		return Value;
	}

	/** Value of KEY=..., with surrounding quotes stripped and bPresent set when the key existed (even if empty). */
	FString ParseValueQuoted(const FString& Params, const TCHAR* Key, bool& bPresent)
	{
		FString Value;
		bPresent = FParse::Value(*Params, Key, Value);
		return Value.TrimQuotes();
	}
}

bool UE2ParsePackageHeader(const uint8* Data, int64 Size, FUE2PackageHeader& Out, FString* OutError)
{
	auto Fail = [OutError](const TCHAR* Msg)
	{
		if (OutError)
		{
			*OutError = Msg;
		}
		return false;
	};
	auto U32 = [Data](int64 Pos) { return uint32(Data[Pos]) | (uint32(Data[Pos + 1]) << 8) | (uint32(Data[Pos + 2]) << 16) | (uint32(Data[Pos + 3]) << 24); };
	if (!Data || Size < 36 || U32(0) != UE2NetPackageMapLocal::PackageTag)
	{
		return Fail(TEXT("not an Unreal package"));
	}
	Out.Version = uint16(U32(4) & 0xFFFF);
	Out.Licensee = uint16(U32(4) >> 16);
	Out.PackageFlags = U32(8);
	Out.NameCount = int32(U32(12));
	Out.ExportCount = int32(U32(20));
	Out.Generations.Reset();
	Out.GuidOffset = -1;
	if (Out.Version < 68)
	{
		// UE1 style header (UT99-era packages): a heritage table instead of GUID + generations, and one generation. UE2's
		// linker takes the package GUID from the heritage table's last entry (it may lie past this buffer: GuidOffset).
		Out.Guid = FGuid();
		Out.Generations.Add(TPair<int32, int32>(Out.ExportCount, Out.NameCount));
		if (Size < 44)
		{
			return Fail(TEXT("truncated package header"));
		}
		const int64 HeritageCount = int64(U32(36));
		const int64 HeritageOffset = int64(U32(40));
		if (HeritageCount > 0)
		{
			Out.GuidOffset = HeritageOffset + (HeritageCount - 1) * 16;
			if (Out.GuidOffset + 16 <= Size)
			{
				Out.Guid = FGuid(U32(Out.GuidOffset), U32(Out.GuidOffset + 4), U32(Out.GuidOffset + 8), U32(Out.GuidOffset + 12));
				Out.GuidOffset = -1;
			}
		}
		return true;
	}
	if (Size < 56)
	{
		return Fail(TEXT("truncated package header"));
	}
	Out.Guid = FGuid(U32(36), U32(40), U32(44), U32(48));
	const int32 GenCount = int32(U32(52));
	if (GenCount < 0 || GenCount > 100000 || 56 + int64(GenCount) * 8 > Size)
	{
		return Fail(TEXT("bad generation table"));
	}
	for (int32 i = 0; i < GenCount; ++i)
	{
		Out.Generations.Add(TPair<int32, int32>(int32(U32(56 + i * 8)), int32(U32(60 + i * 8))));
	}
	return true;
}

bool UE2ReadPackageHeader(const FString& Path, FUE2PackageHeader& Out, FString* OutError)
{
	TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
	if (!Ar)
	{
		if (OutError)
		{
			*OutError = TEXT("cannot open");
		}
		return false;
	}
	// header + generation table: 64 KB covers any real package (each generation takes 8 bytes)
	const int64 Size = FMath::Min<int64>(Ar->TotalSize(), 65536);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(int32(Size));
	Ar->Serialize(Bytes.GetData(), Size);
	if (Ar->IsError())
	{
		if (OutError)
		{
			*OutError = TEXT("read error");
		}
		return false;
	}
	if (!UE2ParsePackageHeader(Bytes.GetData(), Size, Out, OutError))
	{
		return false;
	}
	if (Out.GuidOffset >= 0)
	{
		// UE1 style package: the heritage GUID lies past the bytes read
		uint32 G[4];
		if (Out.GuidOffset + int64(sizeof(G)) > Ar->TotalSize())
		{
			if (OutError)
			{
				*OutError = TEXT("bad heritage table");
			}
			return false;
		}
		Ar->Seek(Out.GuidOffset);
		Ar->Serialize(G, sizeof(G));
		if (Ar->IsError())
		{
			if (OutError)
			{
				*OutError = TEXT("read error");
			}
			return false;
		}
		Out.Guid = FGuid(INTEL_ORDER32(G[0]), INTEL_ORDER32(G[1]), INTEL_ORDER32(G[2]), INTEL_ORDER32(G[3]));
		Out.GuidOffset = -1;
	}
	return true;
}

FString UE2GuidToString(const FGuid& Guid)
{
	return FString::Printf(TEXT("%08X%08X%08X%08X"), Guid.A, Guid.B, Guid.C, Guid.D);
}

bool UE2GuidFromString(const FString& Text, FGuid& Out)
{
	if (Text.Len() != 32)
	{
		return false;
	}
	uint32 W[4];
	for (int32 i = 0; i < 4; ++i)
	{
		const FString Part = Text.Mid(i * 8, 8);
		for (TCHAR C : Part)
		{
			if (!FChar::IsHexDigit(C))
			{
				return false;
			}
		}
		W[i] = uint32(FCString::Strtoui64(*Part, nullptr, 16));
	}
	Out = FGuid(W[0], W[1], W[2], W[3]);
	return true;
}

// =================================================================================================
// Class net cache
// =================================================================================================

FUSField* FUE2ClassNetCache::GetFromIndex(int32 Index) const
{
	for (const FUE2ClassNetCache* C = this; C; C = C->Super)
	{
		if (Index >= C->FieldsBase && Index < C->FieldsBase + C->Fields.Num())
		{
			return C->Fields[Index - C->FieldsBase];
		}
	}
	return nullptr;
}

int32 FUE2ClassNetCache::GetIndexOf(const FUSField* Field) const
{
	for (const FUE2ClassNetCache* C = this; C; C = C->Super)
	{
		const int32 I = C->Fields.IndexOfByKey(const_cast<FUSField*>(Field));
		if (I != INDEX_NONE)
		{
			return C->FieldsBase + I;
		}
	}
	return INDEX_NONE;
}

FUSFunction* FUE2ClassNetCache::FindFunction(FName Name) const
{
	for (const FUE2ClassNetCache* C = this; C; C = C->Super)
	{
		for (FUSField* F : C->Fields)
		{
			if (F->Name == Name && F->Class && F->Class->Name == TEXT("Function"))
			{
				return static_cast<FUSFunction*>(F);
			}
		}
	}
	return nullptr;
}

// =================================================================================================
// Package map
// =================================================================================================

FUE2PackageMap::FUE2PackageMap() = default;

FUE2PackageMap::~FUE2PackageMap()
{
	delete Files;
	Files = nullptr;
}

void FUE2PackageMap::AddUses(const FString& Params)
{
	using namespace UE2NetPackageMapLocal;
	FUE2PackageInfo Info;
	UE2GuidFromString(ParseValue(Params, TEXT("GUID=")), Info.Guid);
	Info.Name = ParseValue(Params, TEXT("PKG=")).TrimQuotes();
	// OldUnreal 3374 quotes FNAME ("ONS-Torlan.ut2") and sends an empty FNAME="" for code packages that cannot be downloaded
	bool bHasFName = false;
	Info.FileName = ParseValueQuoted(Params, TEXT("FNAME="), bHasFName);
	Info.PackageFlags = uint32(FCString::Atoi64(*ParseValue(Params, TEXT("FLAGS="))));
	Info.FileSize = FCString::Atoi(*ParseValue(Params, TEXT("SIZE=")));
	Info.RemoteGeneration = FCString::Atoi(*ParseValue(Params, TEXT("GEN=")));
	// an absent FNAME falls back to the package name; an explicit empty FNAME="" stays empty (not downloadable)
	if (!bHasFName)
	{
		Info.FileName = Info.Name;
	}
	PackageByName.Add(FName(*Info.Name), List.Num());
	List.Add(MoveTemp(Info));
}

void FUE2PackageMap::MatchLocalPackages()
{
	const FString Install = ArenaContent::FindUT2004Install();
	if (!Files)
	{
		// the install's own search paths (UT2004.ini Paths=), then the content cache's script copies, then custom content
		Files = new FConvPackageManager(Install.IsEmpty() ? FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("scripts")) : Install);
		TArray<FString> Extra;
		const FString CacheScripts = FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("scripts"));
		IFileManager::Get().FindFilesRecursive(Extra, *CacheScripts, TEXT("*.u"), true, false, false);
		const FString UserDir = ArenaContent::GetUserContentDir();
		if (!UserDir.IsEmpty() && IFileManager::Get().DirectoryExists(*UserDir))
		{
			for (const TCHAR* Ext : { TEXT("*.u"), TEXT("*.ut2"), TEXT("*.utx"), TEXT("*.usx"), TEXT("*.uax"), TEXT("*.ukx") })
			{
				IFileManager::Get().FindFilesRecursive(Extra, *UserDir, Ext, true, false, false);
			}
		}
		Files->AddPackageFiles(Extra);
	}

	for (FUE2PackageInfo& Info : List)
	{
		Info.bFound = false;
		Info.bGuidMatch = false;
		Info.LocalGeneration = 0;
		Info.Pkg = nullptr;
		Info.bPkgTried = false;
		Info.LocalPath = Files->FindFile(Info.Name);
		FUE2PackageHeader Header;
		FString Err;
		bool bHeader = !Info.LocalPath.IsEmpty() && UE2ReadPackageHeader(Info.LocalPath, Header, &Err);
		if (!Info.LocalPath.IsEmpty() && !bHeader)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Package %s: %s (%s)"), *Info.Name, *Err, *Info.LocalPath);
		}
		if (!bHeader || Header.Guid != Info.Guid)
		{
			// a package downloaded from a server earlier (the net cache, by GUID: Net/UE2NetDownload.h)
			const FString Cached = UE2NetDownload::FindCachedPackage(Info.Guid);
			if (!Cached.IsEmpty() && UE2ReadPackageHeader(Cached, Header, &Err))
			{
				Info.LocalPath = Cached;
				bHeader = true;
			}
		}
		if (!bHeader)
		{
			Info.LocalPath.Reset();
			continue;
		}
		Info.bFound = true;
		Info.bGuidMatch = Header.Guid == Info.Guid;
		Info.LocalGeneration = Info.bGuidMatch ? Header.Generations.Num() : 0;
		if (Info.bGuidMatch)
		{
			// the counts are used by Compute()
			const int32 Gen = FMath::Clamp(FMath::Min(Info.RemoteGeneration > 0 ? Info.RemoteGeneration : Info.LocalGeneration, Info.LocalGeneration), 1, Header.Generations.Num());
			Info.ObjectCount = Header.Generations[Gen - 1].Key;
			Info.NameCount = Header.Generations[Gen - 1].Value;
		}
	}
}

void FUE2PackageMap::RemovePackages(const TArray<FString>& Names)
{
	if (Names.Num() == 0)
	{
		return;
	}
	List.RemoveAll([&Names](const FUE2PackageInfo& Info) { return Names.ContainsByPredicate([&Info](const FString& N) { return N.Equals(Info.Name, ESearchCase::IgnoreCase); }); });
	PackageByName.Reset();
	for (int32 i = 0; i < List.Num(); ++i)
	{
		PackageByName.Add(FName(*List[i].Name), i);
	}
}

void FUE2PackageMap::Compute()
{
	MaxObjectIndex = 0;
	MaxNameIndex = 0;
	NameIndices.Reset();
	bNameIndicesBuilt = false;
	for (FUE2PackageInfo& Info : List)
	{
		Info.ObjectBase = MaxObjectIndex;
		Info.NameBase = MaxNameIndex;
		if (!Info.bGuidMatch)
		{
			// unknown package: its objects cannot be numbered, so every index after it would be wrong
			Info.ObjectCount = 0;
			Info.NameCount = 0;
		}
		MaxObjectIndex += Info.ObjectCount;
		MaxNameIndex += Info.NameCount;
	}
}

TArray<FString> FUE2PackageMap::GetMissing() const
{
	TArray<FString> Out;
	for (const FUE2PackageInfo& Info : List)
	{
		if (!Info.bFound)
		{
			Out.Add(FString::Printf(TEXT("%s (not found)"), *Info.FileName));
		}
		else if (!Info.bGuidMatch)
		{
			Out.Add(FString::Printf(TEXT("%s (different version: GUID mismatch)"), *Info.FileName));
		}
	}
	return Out;
}

TArray<FString> FUE2PackageMap::GetGenerationDifferences() const
{
	TArray<FString> Out;
	for (const FUE2PackageInfo& Info : List)
	{
		if (Info.bGuidMatch && Info.LocalGeneration != Info.RemoteGeneration)
		{
			Out.Add(FString::Printf(TEXT("%s server %d local %d"), *Info.Name, Info.RemoteGeneration, Info.LocalGeneration));
		}
	}
	return Out;
}

int32 FUE2PackageMap::FindPackage(const FString& PackageName) const
{
	const int32* I = PackageByName.Find(FName(*PackageName));
	return I ? *I : INDEX_NONE;
}

bool FUE2PackageMap::IndexToObject(int32 Index, FUE2ObjectRef& Out) const
{
	Out = FUE2ObjectRef();
	if (Index < 0 || Index >= MaxObjectIndex)
	{
		return false;
	}
	for (int32 i = 0; i < List.Num(); ++i)
	{
		const FUE2PackageInfo& Info = List[i];
		if (Index < Info.ObjectBase + Info.ObjectCount)
		{
			Out.PackageIndex = i;
			Out.ExportIndex = Index - Info.ObjectBase;
			return true;
		}
	}
	return false;
}

int32 FUE2PackageMap::ObjectToIndex(int32 PackageIndex, int32 ExportIndex) const
{
	if (!List.IsValidIndex(PackageIndex))
	{
		return INDEX_NONE;
	}
	const FUE2PackageInfo& Info = List[PackageIndex];
	return (ExportIndex >= 0 && ExportIndex < Info.ObjectCount) ? Info.ObjectBase + ExportIndex : INDEX_NONE;
}

int32 FUE2PackageMap::ObjectToIndex(const FUSField* Field) const
{
	if (!Field || !Field->Linker)
	{
		return INDEX_NONE;
	}
	return ObjectToIndex(FindPackage(Field->Linker->PackageName.ToString()), Field->ExportIndex);
}

FConvPackage* FUE2PackageMap::GetPkg(int32 PackageIndex)
{
	if (!List.IsValidIndex(PackageIndex) || !Files)
	{
		return nullptr;
	}
	FUE2PackageInfo& Info = List[PackageIndex];
	if (!Info.bPkgTried)
	{
		Info.bPkgTried = true;
		Info.Pkg = Info.bFound ? Files->OpenFile(Info.LocalPath) : nullptr;
	}
	return Info.Pkg;
}

FString FUE2PackageMap::GetObjectPath(const FUE2ObjectRef& Ref)
{
	if (Ref.IsDynamic())
	{
		return FString::Printf(TEXT("<channel %d>"), Ref.Channel);
	}
	if (!Ref.IsStatic())
	{
		return TEXT("None");
	}
	FConvPackage* Pkg = GetPkg(Ref.PackageIndex);
	if (!Pkg || !Pkg->Exports.IsValidIndex(Ref.ExportIndex))
	{
		return FString::Printf(TEXT("%s.<export %d>"), *List[Ref.PackageIndex].Name, Ref.ExportIndex);
	}
	// the package's own name comes first in its export paths
	return Pkg->ExportPath(Ref.ExportIndex);
}

FString FUE2PackageMap::GetObjectClassName(const FUE2ObjectRef& Ref)
{
	FConvPackage* Pkg = Ref.IsStatic() ? GetPkg(Ref.PackageIndex) : nullptr;
	if (!Pkg || !Pkg->Exports.IsValidIndex(Ref.ExportIndex))
	{
		return FString();
	}
	return Pkg->ExportClassName(Ref.ExportIndex);
}

bool FUE2PackageMap::IndexToName(int32 Index, FName& Out)
{
	Out = NAME_None;
	if (Index < 0 || Index >= MaxNameIndex)
	{
		return false;
	}
	for (int32 i = 0; i < List.Num(); ++i)
	{
		const FUE2PackageInfo& Info = List[i];
		if (Index < Info.NameBase + Info.NameCount)
		{
			FConvPackage* Pkg = GetPkg(i);
			FString Text;
			if (Pkg && Pkg->NameAt(Index - Info.NameBase, Text))
			{
				Out = FName(*Text);
				return true;
			}
			return false;
		}
	}
	return false;
}

bool FUE2PackageMap::EnsureScriptRuntime(FString* OutError)
{
	return ArenaUScript::EnsureRuntime(OutError);
}

FUSClass* FUE2PackageMap::LoadClassExport(int32 PackageIndex, int32 ExportIndex, FString* OutError)
{
	if (!List.IsValidIndex(PackageIndex) || !EnsureScriptRuntime(OutError))
	{
		return nullptr;
	}
	FUE2PackageInfo& Info = List[PackageIndex];
	FUSRuntime& RT = FUSRuntime::Get();
	FUSLinker* L = RT.LoadPackage(FName(*Info.Name), OutError);
	if (!L && Info.bFound)
	{
		L = RT.LoadPackageFile(Info.LocalPath, OutError);
	}
	if (!L || !L->Pkg)
	{
		return nullptr;
	}
	// the script runtime may load another copy of the package (the content cache's): it must be the same build
	const FString Key = Info.Name.ToLower();
	if (!ScriptGuidOk.Contains(Key))
	{
		FUE2PackageHeader Header;
		const bool bOk = UE2ReadPackageHeader(L->Pkg->PathOnDisk, Header) && Header.Guid == Info.Guid;
		ScriptGuidOk.Add(Key, bOk);
		if (!bOk)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Script package %s (%s) is not the server's version: its replicated fields would not line up"),
				*Info.Name, *L->Pkg->PathOnDisk);
		}
	}
	if (!ScriptGuidOk[Key])
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("%s: different package version"), *Info.Name);
		}
		return nullptr;
	}
	FUSObject* Obj = L->GetExport(ExportIndex, false);
	if (!Obj || !(Obj->Flags & USF_Field) || !Obj->Class || Obj->Class->Name != TEXT("Class"))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("%s export %d is not a class"), *Info.Name, ExportIndex);
		}
		return nullptr;
	}
	FUSClass* C = static_cast<FUSClass*>(Obj);
	if (!C->EnsureLoaded())
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("%s: %s"), *C->GetPathName(), *C->LoadError);
		}
		return nullptr;
	}
	return C;
}

FUSClass* FUE2PackageMap::GetClass(const FUE2ObjectRef& Ref, FString* OutError)
{
	if (!Ref.IsStatic())
	{
		return nullptr;
	}
	return LoadClassExport(Ref.PackageIndex, Ref.ExportIndex, OutError);
}

FUSClass* FUE2PackageMap::GetInstanceClass(const FUE2ObjectRef& Ref, FString* OutError)
{
	FConvPackage* Pkg = Ref.IsStatic() ? GetPkg(Ref.PackageIndex) : nullptr;
	if (!Pkg || !Pkg->Exports.IsValidIndex(Ref.ExportIndex))
	{
		return nullptr;
	}
	const int32 ClassIndex = Pkg->Exports[Ref.ExportIndex].ClassIndex;
	if (ClassIndex > 0)
	{
		// a class defined in the same package (map script classes)
		return LoadClassExport(Ref.PackageIndex, ClassIndex - 1, OutError);
	}
	if (ClassIndex < 0 && EnsureScriptRuntime(OutError))
	{
		// "Package.Class" of the import
		return FUSRuntime::Get().FindClass(Pkg->RefPath(ClassIndex), OutError);
	}
	return nullptr;
}

const FUE2ClassNetCache* FUE2PackageMap::GetClassNetCache(FUSClass* Class)
{
	if (!Class)
	{
		return nullptr;
	}
	if (const TUniquePtr<FUE2ClassNetCache>* Hit = ClassCaches.Find(Class))
	{
		return Hit->Get();
	}
	if (ObjectToIndex(Class) == INDEX_NONE)
	{
		ClassCaches.Add(Class, nullptr);
		return nullptr;
	}
	Class->EnsureLoaded();
	TUniquePtr<FUE2ClassNetCache> Cache = MakeUnique<FUE2ClassNetCache>();
	Cache->Class = Class;
	if (FUSClass* Super = Class->GetSuperClass())
	{
		Cache->Super = GetClassNetCache(Super);
		Cache->FieldsBase = Cache->Super ? Cache->Super->GetMaxIndex() : 0;
	}
	// the class's own fields in field-list order (UE2 TFieldIterator stopped at the class itself)
	int32 Guard = 0;
	for (FUSField* F = Class->Children; F && Guard < 100000; F = F->Next, ++Guard)
	{
		if (!(F->Flags & USF_Field) || !F->Class)
		{
			break;
		}
		if (F->Linker)
		{
			F->Linker->LoadField(F);
		}
		const FString Kind = F->Class->Name.ToString();
		bool bNet = false;
		if (Kind == TEXT("Function"))
		{
			// only the first declaration of a replicated function (overrides in subclasses have a super function)
			bNet = (static_cast<FUSFunction*>(F)->FunctionFlags & USFlags::FUNC_Net) != 0 && !F->SuperField;
		}
		else if (Kind.EndsWith(TEXT("Property")))
		{
			bNet = (static_cast<FUSProperty*>(F)->PropertyFlags & USFlags::CPF_Net) != 0;
		}
		if (bNet && ObjectToIndex(F) != INDEX_NONE)
		{
			Cache->Fields.Add(F);
		}
	}
	// UE2 numbers a class's replicated fields in the order of their objects in the package (export table order),
	// not in the order of the class's field list
	Cache->Fields.Sort([](const FUSField& A, const FUSField& B) { return A.ExportIndex < B.ExportIndex; });
	if (FParse::Param(FCommandLine::Get(), TEXT("UE2NetTraceBits")))
	{
		for (int32 i = 0; i < Cache->Fields.Num(); ++i)
		{
			FUSField* F = Cache->Fields[i];
			const bool bProp = F->Class->Name.ToString().EndsWith(TEXT("Property"));
			FString Desc;
			if (bProp)
			{
				FUSProperty* P = static_cast<FUSProperty*>(F);
				Desc = FString::Printf(TEXT("prop type=%d dim=%d enum=%d struct=%s"), int32(P->Type), P->ArrayDim, P->Enum ? P->Enum->Names.Num() : -1, P->Struct ? *P->Struct->Name.ToString() : TEXT("-"));
			}
			else
			{
				FUSFunction* Fn = static_cast<FUSFunction*>(F);
				Desc = FString::Printf(TEXT("func flags=%08x"), Fn->FunctionFlags);
				for (FUSField* C = Fn->Children; C; C = C->Next)
				{
					if (C->Class->Name.ToString().EndsWith(TEXT("Property")))
					{
						FUSProperty* P = static_cast<FUSProperty*>(C);
						Desc += FString::Printf(TEXT(" %s:%d:%08x:%d"), *P->Name.ToString(), int32(P->Type), P->PropertyFlags, P->Enum ? P->Enum->Names.Num() : -1);
					}
				}
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetFields: %s %d %s %s"), *Class->GetName(), Cache->FieldsBase + i, *F->Name.ToString(), *Desc);
		}
	}
	FUE2ClassNetCache* Result = Cache.Get();
	ClassCaches.Add(Class, MoveTemp(Cache));
	return Result;
}

// =================================================================================================
// Server side (Net/UE2Server*): reverse lookups
// =================================================================================================

int32 FUE2PackageMap::NameToIndex(FName Name)
{
	if (Name.IsNone())
	{
		return MaxNameIndex;
	}
	if (!bNameIndicesBuilt)
	{
		bNameIndicesBuilt = true;
		NameIndices.Reset();
		for (int32 i = 0; i < List.Num(); ++i)
		{
			const FUE2PackageInfo& Info = List[i];
			FConvPackage* Pkg = Info.NameCount > 0 ? GetPkg(i) : nullptr;
			if (!Pkg)
			{
				continue;
			}
			for (int32 j = 0; j < Info.NameCount && j < Pkg->Names.Num(); ++j)
			{
				const FName N(*Pkg->Names[j]);
				if (!NameIndices.Contains(N))
				{
					NameIndices.Add(N, Info.NameBase + j);
				}
			}
		}
	}
	const int32* Found = NameIndices.Find(Name);
	return Found ? *Found : MaxNameIndex;
}

bool FUE2PackageMap::SetRemoteGeneration(const FGuid& Guid, int32 Generation)
{
	const int32 I = FindPackageByGuid(Guid);
	if (I == INDEX_NONE || Generation <= 0)
	{
		return false;
	}
	List[I].RemoteGeneration = Generation;
	return true;
}

int32 FUE2PackageMap::FindPackageByGuid(const FGuid& Guid) const
{
	return List.IndexOfByPredicate([&Guid](const FUE2PackageInfo& Info) { return Info.Guid == Guid; });
}

bool FUE2PackageMap::FindObjectByPath(const FString& Path, FUE2ObjectRef& Out)
{
	Out = FUE2ObjectRef();
	FString PackageName, Rel;
	if (!Path.Split(TEXT("."), &PackageName, &Rel))
	{
		return false;
	}
	const int32 PackageIndex = FindPackage(PackageName);
	FConvPackage* Pkg = PackageIndex != INDEX_NONE ? GetPkg(PackageIndex) : nullptr;
	if (!Pkg)
	{
		return false;
	}
	const int32 Export = Pkg->FindExport(Rel);
	if (Export < 0 || Export >= List[PackageIndex].ObjectCount)
	{
		return false;
	}
	Out.PackageIndex = PackageIndex;
	Out.ExportIndex = Export;
	return true;
}

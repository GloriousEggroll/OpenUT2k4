#include "ArenaRuntimeContent.h"
#include "ArenaRuntimeContentPrivate.h"
#include "ArenaContent.h"
#include "OpenUT2k4.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"
#include "UObject/StrongObjectPtr.h"

// =================================================================================================
// Names and the cache index
// =================================================================================================

namespace ArenaRuntimePrivate
{
	FString Sanitize(const FString& In, bool bKeepDot)
	{
		FString Out = In;
		for (TCHAR& C : Out)
		{
			const bool bOk = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-' || (bKeepDot && C == '.');
			if (!bOk)
			{
				C = '_';
			}
		}
		return Out;
	}

	/** Files of one cache kind folder: <cache>/<kind>/<Pkg>/<Group.Name>.json (music: <cache>/music/<Name>.json). */
	struct FKindIndex
	{
		bool bBuilt = false;
		/** lower("<PkgFolder>.<FileStem>") -> "<dir>/<FileStem>" (no extension) */
		TMap<FString, FString> ByObjectKey;
		/** lower("<AssetName(PkgFolder)>/<AssetName(FileStem)>") -> same */
		TMap<FString, FString> ByAssetName;
	};

	FCriticalSection IndexLock;
	TMap<FString, FKindIndex> Indices;

	FKindIndex& GetIndex(const TCHAR* Kind)
	{
		// caller holds IndexLock
		FKindIndex& Index = Indices.FindOrAdd(FString(Kind).ToLower());
		if (Index.bBuilt)
		{
			return Index;
		}
		Index.bBuilt = true;
		const double T0 = FPlatformTime::Seconds();
		const FString Root = ArenaContent::GetCacheDir() / Kind;
		auto AddDir = [&Index](const FString& Dir, const FString& Pkg)
		{
			IFileManager::Get().IterateDirectory(*Dir, [&](const TCHAR* Path, bool bIsDir)
			{
				const FString File(Path);
				if (!bIsDir && File.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
				{
					const FString Stem = FPaths::GetBaseFilename(File);
					const FString Base = FPaths::GetPath(File) / Stem;
					const FString PkgKey = Pkg.IsEmpty() ? Stem : Pkg;
					Index.ByObjectKey.Add((PkgKey + TEXT(".") + Stem).ToLower(), Base);
					Index.ByAssetName.Add((AssetName(PkgKey) + TEXT("/") + AssetName(Stem)).ToLower(), Base);
				}
				return true;
			});
		};
		IFileManager::Get().IterateDirectory(*Root, [&](const TCHAR* Path, bool bIsDir)
		{
			if (bIsDir)
			{
				AddDir(Path, FPaths::GetCleanFilename(Path));
			}
			return true;
		});
		AddDir(Root, FString()); // flat folders (music)
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: indexed %d %s in %.2fs"), Index.ByObjectKey.Num(), Kind, FPlatformTime::Seconds() - T0);
		return Index;
	}

	// ---- built objects ----

	struct FObjectCache
	{
		TMap<FString, TWeakObjectPtr<UObject>> Objects;
		TSet<FString> Misses;
		TArray<TStrongObjectPtr<UObject>> Session;
		TArray<TStrongObjectPtr<UObject>> Permanent;
		TMap<FString, ArenaRuntimeContent::FLoader> Loaders;
		/** Replacement package checks: package path -> exists */
		TMap<FString, bool> Replacements;
	};

	FObjectCache& Objects()
	{
		static FObjectCache C;
		return C;
	}

	FStats& Stats()
	{
		static FStats S;
		return S;
	}

	/** Cached object for Key, or build it with Make (misses are remembered too). */
	template <typename T, typename FMake>
	T* FindOrBuild(const FString& Key, FMake&& Make)
	{
		check(IsInGameThread());
		FObjectCache& C = Objects();
		if (const TWeakObjectPtr<UObject>* Found = C.Objects.Find(Key))
		{
			if (UObject* O = Found->Get())
			{
				return Cast<T>(O);
			}
		}
		if (C.Misses.Contains(Key))
		{
			return nullptr;
		}
		T* Result = Make();
		if (Result)
		{
			C.Objects.Add(Key, Result);
			Keep(Result, false);
		}
		else
		{
			C.Misses.Add(Key);
		}
		return Result;
	}

	/** /Game/Replacements/<Rel> when that package exists (same override as ArenaArt::LoadArt). */
	UObject* LoadReplacement(UClass* Class, const FString& Rel)
	{
		const FString Package = TEXT("/Game/Replacements/") + Rel;
		bool* Known = Objects().Replacements.Find(Package);
		const bool bExists = Known ? *Known : Objects().Replacements.Add(Package, FPackageName::DoesPackageExist(Package));
		if (!bExists)
		{
			return nullptr;
		}
		return StaticLoadObject(Class, nullptr, *(Package + TEXT(".") + FPaths::GetBaseFilename(Rel)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Object path ("path" in the cache json) of a cache file base; falls back to <Pkg>.<Stem>. */
	FString ObjectPathOf(const FString& Base)
	{
		if (const TSharedPtr<FJsonObject> J = LoadJson(Base + TEXT(".json")))
		{
			FString P;
			if (J->TryGetStringField(TEXT("path"), P) && !P.IsEmpty())
			{
				return P;
			}
		}
		return FPaths::GetCleanFilename(FPaths::GetPath(Base)) + TEXT(".") + FPaths::GetCleanFilename(Base);
	}

	/** "Kind/Pkg/Name" -> (Pkg, Name); false for other shapes. */
	bool SplitRelative(const FString& Rel, FString& Kind, FString& Pkg, FString& Name)
	{
		TArray<FString> Parts;
		Rel.ParseIntoArray(Parts, TEXT("/"));
		if (Parts.Num() < 2)
		{
			return false;
		}
		Kind = Parts[0];
		Name = Parts.Last();
		Pkg = Parts.Num() >= 3 ? Parts[1] : FString();
		return true;
	}

	/** Relative "Kind/Pkg/Name" -> UT2004 object path through the cache index of KindFolder; empty if missing. */
	FString RelativeToObjectPath(const TCHAR* KindFolder, const FString& Pkg, const FString& Name)
	{
		const FString Base = FindByAssetName(KindFolder, Pkg, Name);
		return Base.IsEmpty() ? FString() : ObjectPathOf(Base);
	}

	FCriticalSection StatsLock;
	TMap<FString, TSharedPtr<FJsonObject>> TextureStatsMap;
	bool bTextureStatsLoaded = false;
}

FString ArenaRuntimePrivate::AssetName(const FString& In) { return Sanitize(In, false); }
FString ArenaRuntimePrivate::CacheSafe(const FString& In) { return Sanitize(In, true); }

FString ArenaRuntimePrivate::FindByObjectPath(const TCHAR* Kind, const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
	{
		return FString();
	}
	FString Pkg, Rest;
	if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
	{
		Pkg = Rest = ObjectPath;
	}
	const FString Key = (CacheSafe(Pkg) + TEXT(".") + CacheSafe(Rest)).ToLower();
	FScopeLock Lock(&IndexLock);
	const FString* Found = GetIndex(Kind).ByObjectKey.Find(Key);
	return Found ? *Found : FString();
}

FString ArenaRuntimePrivate::FindByAssetName(const TCHAR* Kind, const FString& PackageFolder, const FString& Name)
{
	FScopeLock Lock(&IndexLock);
	const FString Key = ((PackageFolder.IsEmpty() ? Name : PackageFolder) + TEXT("/") + Name).ToLower();
	const FString* Found = GetIndex(Kind).ByAssetName.Find(Key);
	return Found ? *Found : FString();
}

TSharedPtr<FJsonObject> ArenaRuntimePrivate::LoadJson(const FString& File)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
}

const TSharedPtr<FJsonObject>* ArenaRuntimePrivate::FindTextureStats(const FString& ObjectPath)
{
	TMap<FString, TSharedPtr<FJsonObject>>& Map = TextureStatsMap;
	bool& bLoaded = bTextureStatsLoaded;
	{
		FScopeLock Lock(&StatsLock);
		if (!bLoaded)
		{
			bLoaded = true;
			if (const TSharedPtr<FJsonObject> Root = LoadJson(ArenaContent::GetCacheDir() / TEXT("texture_stats.json")))
			{
				const TSharedPtr<FJsonObject>* Textures = nullptr;
				if (Root->TryGetObjectField(TEXT("textures"), Textures))
				{
					for (const auto& Pair : (*Textures)->Values)
					{
						if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
						{
							Map.Add(FString(Pair.Key).ToLower(), Pair.Value->AsObject());
						}
					}
				}
			}
		}
	}
	return Map.Find(ObjectPath.ToLower());
}

UObject* ArenaRuntimePrivate::AssetOuter()
{
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.OwningGameInstance && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
			{
				return Ctx.OwningGameInstance;
			}
		}
	}
	return GetTransientPackage();
}

void ArenaRuntimePrivate::Keep(UObject* Object, bool bPermanent)
{
	if (Object)
	{
		(bPermanent ? Objects().Permanent : Objects().Session).Emplace(Object);
	}
}

// =================================================================================================
// Public API
// =================================================================================================

bool ArenaRuntimeContent::IsActive()
{
	// Only a positive answer is cached: on a first start the cache appears when the converter has run.
	static bool bActive = false;
	if (!bActive && ArenaContent::UseRuntimeContent() && IFileManager::Get().DirectoryExists(*ArenaContent::GetCacheDir()))
	{
		bActive = true;
		FCoreDelegates::OnEnginePreExit.AddLambda([]() { UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: session total: %s"), *GetStats()); });
	}
	return bActive;
}

void ArenaRuntimeContent::ResetCacheIndex()
{
	using namespace ArenaRuntimePrivate;
	{
		FScopeLock Lock(&IndexLock);
		Indices.Reset();
	}
	{
		FScopeLock Lock(&StatsLock);
		TextureStatsMap.Reset();
		bTextureStatsLoaded = false;
	}
	Objects().Misses.Reset();
	Objects().Replacements.Reset();
	ResetMaterialGraph();
	TrimTextures();
	TrimMeshData();
}

UObject* ArenaRuntimeContent::LoadByRelativePath(UClass* Class, const FString& RelativePath)
{
	using namespace ArenaRuntimePrivate;
	if (!Class || !IsActive())
	{
		return nullptr;
	}
	FString Kind, Pkg, Name;
	if (!SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return nullptr;
	}
	UObject* Result = nullptr;
	if (Kind.Equals(TEXT("Textures"), ESearchCase::IgnoreCase))
	{
		Result = Class->IsChildOf(UMaterialInterface::StaticClass())
			? static_cast<UObject*>(LoadMaterial(TEXT("Materials/") + Pkg + TEXT("/") + Name))
			: static_cast<UObject*>(LoadTexture(RelativePath));
	}
	else if (Kind.Equals(TEXT("Materials"), ESearchCase::IgnoreCase))
	{
		Result = LoadMaterial(RelativePath);
	}
	else if (Kind.Equals(TEXT("StaticMeshes"), ESearchCase::IgnoreCase))
	{
		Result = LoadStaticMesh(RelativePath);
	}
	else if (Kind.Equals(TEXT("Sounds"), ESearchCase::IgnoreCase))
	{
		Result = LoadSound(RelativePath);
	}
	else if (Kind.Equals(TEXT("Music"), ESearchCase::IgnoreCase))
	{
		Result = LoadMusic(Name);
	}
	else if (const FLoader* Loader = Objects().Loaders.Find(Kind.ToLower()))
	{
		const FString Key = FString::Printf(TEXT("ext|%s|%s"), *Class->GetName(), *RelativePath.ToLower());
		Result = FindOrBuild<UObject>(Key, [&]() { return (*Loader)(Class, RelativePath); });
	}
	return Result && Result->IsA(Class) ? Result : nullptr;
}

UTexture2D* ArenaRuntimeContent::LoadTexture(const FString& RelativePath)
{
	using namespace ArenaRuntimePrivate;
	FString Kind, Pkg, Name;
	if (!IsActive() || !SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return nullptr;
	}
	const FString ObjectPath = RelativeToObjectPath(TEXT("textures"), Pkg, Name);
	return ObjectPath.IsEmpty() ? nullptr : LoadTextureByObjectPath(ObjectPath);
}

UTexture2D* ArenaRuntimeContent::LoadTextureByObjectPath(const FString& ObjectPath)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive() || ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
	{
		return nullptr;
	}
	return FindOrBuild<UTexture2D>(TEXT("tex|") + ObjectPath.ToLower(), [&]() -> UTexture2D*
	{
		if (UTexture2D* Rep = Cast<UTexture2D>(LoadReplacement(UTexture2D::StaticClass(), ObjectPathToRelative(TEXT("Textures"), ObjectPath))))
		{
			return Rep;
		}
		return BuildTexture(ObjectPath);
	});
}

UMaterialInterface* ArenaRuntimeContent::LoadMaterial(const FString& RelativePath, EMaterialVariant Variant)
{
	using namespace ArenaRuntimePrivate;
	FString Kind, Pkg, Name;
	if (!IsActive() || !SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return nullptr;
	}
	if (Pkg.Equals(TEXT("Masters"), ESearchCase::IgnoreCase))
	{
		// the importer's masters: shipped generated materials
		return Name.Equals(TEXT("MI_ImportedDefault"), ESearchCase::IgnoreCase) ? DefaultMaterial() : GetMaster(Name);
	}
	// "<Name>_2S" / "<Name>_U" are the forced variants (unless the cache has an asset of that exact name)
	FString Base = Name;
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		FString ObjectPath = RelativeToObjectPath(TEXT("materials"), Pkg, Base);
		if (ObjectPath.IsEmpty())
		{
			ObjectPath = RelativeToObjectPath(TEXT("textures"), Pkg, Base);
		}
		if (!ObjectPath.IsEmpty())
		{
			return LoadMaterialByObjectPath(ObjectPath, Variant);
		}
		if (Base.EndsWith(TEXT("_U")))
		{
			Base.LeftChopInline(2);
			Variant |= EMaterialVariant::Unlit;
		}
		else if (Base.EndsWith(TEXT("_2S")))
		{
			Base.LeftChopInline(3);
			Variant |= EMaterialVariant::TwoSided;
		}
		else
		{
			break;
		}
	}
	return nullptr;
}

UMaterialInterface* ArenaRuntimeContent::LoadMaterialByObjectPath(const FString& ObjectPath, EMaterialVariant Variant)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive())
	{
		return nullptr;
	}
	const FString Key = FString::Printf(TEXT("mat|%s|%d"), *ObjectPath.ToLower(), int32(Variant));
	return FindOrBuild<UMaterialInterface>(Key, [&]() -> UMaterialInterface*
	{
		if (Variant == EMaterialVariant::None && !ObjectPath.IsEmpty() && ObjectPath != TEXT("None"))
		{
			if (UMaterialInterface* Rep = Cast<UMaterialInterface>(LoadReplacement(UMaterialInterface::StaticClass(), ObjectPathToRelative(TEXT("Materials"), ObjectPath))))
			{
				return Rep;
			}
		}
		return BuildMaterial(ObjectPath, Variant);
	});
}

UStaticMesh* ArenaRuntimeContent::LoadStaticMesh(const FString& RelativePath)
{
	using namespace ArenaRuntimePrivate;
	FString Kind, Pkg, Name;
	if (!IsActive() || !SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return nullptr;
	}
	const FString ObjectPath = RelativeToObjectPath(TEXT("staticmeshes"), Pkg, Name);
	return ObjectPath.IsEmpty() ? nullptr : LoadStaticMeshByObjectPath(ObjectPath);
}

UStaticMesh* ArenaRuntimeContent::LoadStaticMeshByObjectPath(const FString& ObjectPath)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive() || ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
	{
		return nullptr;
	}
	return FindOrBuild<UStaticMesh>(TEXT("mesh|") + ObjectPath.ToLower(), [&]() -> UStaticMesh*
	{
		if (UStaticMesh* Rep = Cast<UStaticMesh>(LoadReplacement(UStaticMesh::StaticClass(), ObjectPathToRelative(TEXT("StaticMeshes"), ObjectPath))))
		{
			return Rep;
		}
		return BuildCacheStaticMesh(ObjectPath);
	});
}

UStaticMesh* ArenaRuntimeContent::LoadStaticMeshCollisionModel(const FString& ObjectPath, bool& bOutBox, bool& bOutLine)
{
	using namespace ArenaRuntimePrivate;
	bOutBox = bOutLine = false;
	if (!IsActive() || ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
	{
		return nullptr;
	}
	UStaticMesh* Mesh = FindOrBuild<UStaticMesh>(TEXT("meshcol|") + ObjectPath.ToLower(), [&]() -> UStaticMesh*
	{
		if (LoadReplacement(UStaticMesh::StaticClass(), ObjectPathToRelative(TEXT("StaticMeshes"), ObjectPath)))
		{
			return nullptr; // a replacement brings its own collision
		}
		return BuildCacheCollisionModel(ObjectPath);
	});
	if (Mesh)
	{
		CollisionModelFlags(ObjectPath, bOutBox, bOutLine);
	}
	return Mesh;
}

int32 ArenaRuntimeContent::GetStaticMeshSlotForSection(const FString& ObjectPath, int32 SectionIndex)
{
	return IsActive() ? ArenaRuntimePrivate::SlotForSection(ObjectPath, SectionIndex) : -1;
}

USoundBase* ArenaRuntimeContent::LoadSound(const FString& RelativePath)
{
	using namespace ArenaRuntimePrivate;
	FString Kind, Pkg, Name;
	if (!IsActive() || !SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return nullptr;
	}
	const FString ObjectPath = RelativeToObjectPath(TEXT("sounds"), Pkg, Name);
	return ObjectPath.IsEmpty() ? nullptr : LoadSoundByObjectPath(ObjectPath);
}

USoundBase* ArenaRuntimeContent::LoadSoundByObjectPath(const FString& ObjectPath)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive() || ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
	{
		return nullptr;
	}
	return FindOrBuild<USoundBase>(TEXT("snd|") + ObjectPath.ToLower(), [&]() -> USoundBase*
	{
		if (USoundBase* Rep = Cast<USoundBase>(LoadReplacement(USoundBase::StaticClass(), ObjectPathToRelative(TEXT("Sounds"), ObjectPath))))
		{
			return Rep;
		}
		return BuildSound(ObjectPath);
	});
}

USoundBase* ArenaRuntimeContent::LoadMusic(const FString& InName)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive())
	{
		return nullptr;
	}
	FString Name = InName;
	Name.RemoveFromStart(TEXT("Music/"));
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	return FindOrBuild<USoundBase>(TEXT("music|") + Name.ToLower(), [&]() -> USoundBase*
	{
		if (USoundBase* Rep = Cast<USoundBase>(LoadReplacement(USoundBase::StaticClass(), TEXT("Music/") + AssetName(Name))))
		{
			return Rep;
		}
		return BuildMusic(Name);
	});
}

TArray<FString> ArenaRuntimeContent::GetMusicNames()
{
	TArray<FString> Out;
	if (!IsActive())
	{
		return Out;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(ArenaContent::GetCacheDir() / TEXT("music") / TEXT("*.ogg")), true, false);
	for (const FString& F : Files)
	{
		Out.Add(FPaths::GetBaseFilename(F));
	}
	Out.Sort();
	return Out;
}

FString ArenaRuntimeContent::ObjectPathToRelative(const TCHAR* Kind, const FString& ObjectPath)
{
	using namespace ArenaRuntimePrivate;
	FString Pkg, Rest;
	if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
	{
		Pkg = Rest = ObjectPath;
	}
	return FString::Printf(TEXT("%s/%s/%s"), Kind, *AssetName(Pkg), *AssetName(Rest));
}

FString ArenaRuntimeContent::FindCacheFile(const FString& RelativePath, const TCHAR* Extension)
{
	using namespace ArenaRuntimePrivate;
	FString Kind, Pkg, Name;
	if (!SplitRelative(RelativePath, Kind, Pkg, Name))
	{
		return FString();
	}
	const FString Base = FindByAssetName(*Kind.ToLower(), Pkg, Name);
	if (Base.IsEmpty())
	{
		return FString();
	}
	const FString File = Base + Extension;
	return FPaths::FileExists(File) ? File : FString();
}

FString ArenaRuntimeContent::FindCacheFileByObjectPath(const TCHAR* KindFolder, const FString& ObjectPath, const TCHAR* Extension)
{
	const FString Base = ArenaRuntimePrivate::FindByObjectPath(KindFolder, ObjectPath);
	if (Base.IsEmpty())
	{
		return FString();
	}
	const FString File = Base + Extension;
	return FPaths::FileExists(File) ? File : FString();
}

void ArenaRuntimeContent::RegisterLoader(const FString& Kind, FLoader Loader)
{
	ArenaRuntimePrivate::Objects().Loaders.Add(Kind.ToLower(), MoveTemp(Loader));
}

void ArenaRuntimeContent::KeepAlive(UObject* Object, bool bPermanent)
{
	ArenaRuntimePrivate::Keep(Object, bPermanent);
}

void ArenaRuntimeContent::PrefetchStaticMeshes(const TArray<FString>& ObjectPaths)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive() || ObjectPaths.Num() == 0)
	{
		return;
	}
	const double T0 = FPlatformTime::Seconds();
	PrefetchMeshData(ObjectPaths);
	PrefetchMaterials(MeshMaterials(ObjectPaths));
	UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: prefetched %d meshes in %.2fs"), ObjectPaths.Num(), FPlatformTime::Seconds() - T0);
}

void ArenaRuntimeContent::PrefetchMaterials(const TArray<FString>& ObjectPaths)
{
	using namespace ArenaRuntimePrivate;
	if (!IsActive() || ObjectPaths.Num() == 0)
	{
		return;
	}
	// only textures that are not built yet
	TArray<FString> Textures = MaterialBaseTextures(ObjectPaths);
	Textures.RemoveAll([](const FString& T)
	{
		const TWeakObjectPtr<UObject>* Found = Objects().Objects.Find(TEXT("tex|") + T.ToLower());
		return Found && Found->IsValid();
	});
	PrefetchTextures(Textures);
}

void ArenaRuntimeContent::TrimCache()
{
	using namespace ArenaRuntimePrivate;
	FObjectCache& C = Objects();
	const int32 Released = C.Session.Num();
	C.Session.Reset();
	C.Misses.Reset();
	for (auto It = C.Objects.CreateIterator(); It; ++It)
	{
		if (!It->Value.IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TrimTextures();
	TrimMeshData();
	UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: released %d session assets (%s)"), Released, *GetStats());
}

FString ArenaRuntimeContent::GetStats()
{
	using namespace ArenaRuntimePrivate;
	const FStats& S = Stats();
	return FString::Printf(TEXT("%d textures (%.1f MB, %.2fs), %d materials (%.2fs), %d meshes (%.2fs), %d sounds (%.2fs, %d plays)"),
		S.Textures.load(), S.TextureBytes.load() / (1024.0 * 1024.0), S.TextureUs.load() / 1e6,
		S.Materials.load(), S.MaterialUs.load() / 1e6, S.Meshes.load(), S.MeshUs.load() / 1e6, S.Sounds.load(), S.SoundUs.load() / 1e6,
		S.SoundPlays.load());
}

static FAutoConsoleCommand GRuntimeContentStats(
	TEXT("arena.RuntimeContentStats"),
	TEXT("Log what the runtime content provider has built so far"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UE_LOG(LogOpenUT2k4, Display, TEXT("RuntimeContent: %s"), *ArenaRuntimeContent::GetStats());
	}));

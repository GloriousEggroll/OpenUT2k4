#include "ArenaLevelBuilder.h"
#include "ArenaUE2World.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaUserContent.h"
#include "ArenaRuntimeMap.h"
#include "ArenaLocalMessages.h"
#include "ArenaPickup.h"
#include "ArenaJumpPad.h"
#include "ArenaFlag.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaTranslocator.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ArenaImportedLighting.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UScript/ArenaUScript.h"

namespace ArenaImported
{
	const TCHAR* const ImportedMapsDir = TEXT("/Game/Imported/Maps");
	const TCHAR* const ArenaLevel = TEXT("/Game/Maps/Arena");

	/**
	 * UT2004's map list text for maps whose LevelSummary.Description is empty (most stock maps): the cache
	 * records' Map=(MapName=...,TextName=XMaps.Face3) name a DecoText entry ([DecoText] Face3=... in XMaps.int,
	 * rows split by '|'). MapName (lower case) -> text; the localization folder of the cache, else the install's.
	 */
	TMap<FString, FString> LoadStockMapTexts()
	{
		TMap<FString, FString> Texts;
		FString Dir = ArenaContent::GetCacheDir() / TEXT("localization");
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.ucl")), true, false);
		if (Files.Num() == 0)
		{
			const FString Install = ArenaContent::FindUT2004Install();
			Dir = Install.IsEmpty() ? FString() : Install / TEXT("System");
			if (!Dir.IsEmpty())
			{
				IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.ucl")), true, false);
			}
		}
		for (const FString& File : Files)
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *(Dir / File));
			for (const FString& Line : Lines)
			{
				FString MapName, TextName;
				if (!Line.StartsWith(TEXT("Map=")) || !FParse::Value(*Line, TEXT("MapName="), MapName) || !FParse::Value(*Line, TEXT("TextName="), TextName)
					|| MapName.IsEmpty() || TextName.IsEmpty() || Texts.Contains(MapName.ToLower()))
				{
					continue;
				}
				// "XMaps.Face3"; a bare name is looked up in XMaps, where the stock entries live
				FString Package = TEXT("XMaps"), Key = TextName;
				TextName.Split(TEXT("."), &Package, &Key);
				FString Text;
				if (ArenaLocalMessages::Localize(Package, TEXT("DecoText"), Key, Text) && !Text.IsEmpty())
				{
					Text.ReplaceInline(TEXT("|"), TEXT("\n"));
					Text.TrimEndInline();
					Texts.Add(MapName.ToLower(), Text);
				}
			}
		}
		return Texts;
	}

	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	bool ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Field, Arr) || Arr->Num() < 3)
		{
			return false;
		}
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>& ArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Field, Arr) ? *Arr : Empty;
	}

	/** Our weapon class for the weapon names in the marker json. */
	TSubclassOf<AArenaWeapon> WeaponClass(const FString& Name)
	{
		if (Name == TEXT("PulseRifle"))     { return AWeap_PulseRifle::StaticClass(); }
		if (Name == TEXT("WarheadLauncher")) { return AWeap_WarheadLauncher::StaticClass(); }
		if (Name == TEXT("OrbitalDesignator")) { return AWeap_OrbitalDesignator::StaticClass(); }
		if (Name == TEXT("MarksmanRifle"))  { return AWeap_MarksmanRifle::StaticClass(); }
		if (Name == TEXT("SeekerLauncher")) { return AWeap_SeekerLauncher::StaticClass(); }
		if (Name == TEXT("StickyLauncher")) { return AWeap_StickyLauncher::StaticClass(); }
		if (Name == TEXT("CrawlerLayer"))   { return AWeap_CrawlerLayer::StaticClass(); }
		if (Name == TEXT("Translocator"))   { return AWeap_Translocator::StaticClass(); }
		if (Name == TEXT("ArcLinker"))      { return AWeap_ArcLinker::StaticClass(); }
		if (Name == TEXT("RailRifle"))      { return AWeap_RailRifle::StaticClass(); }
		if (Name == TEXT("RocketLauncher")) { return AWeap_RocketLauncher::StaticClass(); }
		if (Name == TEXT("ShardCannon"))    { return AWeap_ShardCannon::StaticClass(); }
		if (Name == TEXT("RotaryGun"))      { return AWeap_RotaryGun::StaticClass(); }
		if (Name == TEXT("GooLauncher"))    { return AWeap_GooLauncher::StaticClass(); }
		if (Name == TEXT("Carbine"))        { return AWeap_Carbine::StaticClass(); }
		return nullptr;
	}

	/** UT2004 class (weapon, its pickup or ammo pickup, lower case) of the super / Onslaught / classic weapons -> ours. */
	FString ExtraWeaponName(const FString& LowerClass)
	{
		static const TMap<FString, FString> M = {
			{ TEXT("redeemer"), TEXT("WarheadLauncher") }, { TEXT("redeemerpickup"), TEXT("WarheadLauncher") },
			{ TEXT("painter"), TEXT("OrbitalDesignator") }, { TEXT("painterpickup"), TEXT("OrbitalDesignator") },
			{ TEXT("onspainter"), TEXT("OrbitalDesignator") }, { TEXT("onspainterpickup"), TEXT("OrbitalDesignator") },
			{ TEXT("classicsniperrifle"), TEXT("MarksmanRifle") }, { TEXT("classicsniperriflepickup"), TEXT("MarksmanRifle") },
			{ TEXT("classicsniperammopickup"), TEXT("MarksmanRifle") },
			{ TEXT("onsavril"), TEXT("SeekerLauncher") }, { TEXT("onsavrilpickup"), TEXT("SeekerLauncher") }, { TEXT("onsavrilammopickup"), TEXT("SeekerLauncher") },
			{ TEXT("onsgrenadelauncher"), TEXT("StickyLauncher") }, { TEXT("onsgrenadepickup"), TEXT("StickyLauncher") },
			{ TEXT("onsgrenadeammopickup"), TEXT("StickyLauncher") },
			{ TEXT("onsminelayer"), TEXT("CrawlerLayer") }, { TEXT("onsminelayerpickup"), TEXT("CrawlerLayer") }, { TEXT("onsmineammopickup"), TEXT("CrawlerLayer") },
			{ TEXT("translauncher"), TEXT("Translocator") }, { TEXT("transpickup"), TEXT("Translocator") } };
		const FString* Hit = M.Find(LowerClass);
		return Hit ? *Hit : FString();
	}

	/**
	 * Map data written before these weapons existed stands them in with the rocket launcher / rail rifle: find the
	 * UT2004 class again (the pickup's own class, or the weapon base / locker at that spot in raw_markers).
	 */
	struct FRawWeaponSpot
	{
		FVector Location;
		FString Class;
	};

	TArray<FRawWeaponSpot> RawWeaponSpots(const TSharedPtr<FJsonObject>& Root)
	{
		TArray<FRawWeaponSpot> Out;
		const TSharedPtr<FJsonObject>* Raw = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("raw_markers"), Raw) || !Raw || !Raw->IsValid())
		{
			return Out;
		}
		auto ClassOf = [](const TSharedPtr<FJsonObject>& O) -> FString
		{
			const TSharedPtr<FJsonObject>* WC = nullptr;
			FString C;
			if (O.IsValid() && O->TryGetObjectField(TEXT("weapon_class"), WC) && WC && WC->IsValid())
			{
				(*WC)->TryGetStringField(TEXT("class"), C);
			}
			return C;
		};
		for (const TSharedPtr<FJsonValue>& V : ArrayField(*Raw, TEXT("weapon_bases")))
		{
			FRawWeaponSpot S;
			if (ReadVector(V->AsObject(), TEXT("location"), S.Location))
			{
				S.Class = ClassOf(V->AsObject());
				Out.Add(S);
			}
		}
		for (const TSharedPtr<FJsonValue>& V : ArrayField(*Raw, TEXT("weapon_lockers")))
		{
			const TSharedPtr<FJsonObject> L = V->AsObject();
			FVector Loc;
			if (!ReadVector(L, TEXT("location"), Loc))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Rot = nullptr;
			double YawDeg = 0.0;
			if (L->TryGetObjectField(TEXT("rotation"), Rot) && Rot && Rot->IsValid())
			{
				(*Rot)->TryGetNumberField(TEXT("yaw"), YawDeg);
			}
			const double Yaw = FMath::DegreesToRadians(YawDeg);
			const TArray<TSharedPtr<FJsonValue>>& Ws = ArrayField(L, TEXT("weapons"));
			const int32 N = Ws.Num();
			for (int32 i = 0; i < N; ++i)
			{
				const double Off = (i - (N - 1) * 0.5) * 70.0; // ArenaConvData: lockers spread their weapons 70 apart
				FRawWeaponSpot S;
				S.Location = FVector(Loc.X - FMath::Sin(Yaw) * Off, Loc.Y + FMath::Cos(Yaw) * Off, Loc.Z);
				S.Class = ClassOf(Ws[i]->AsObject());
				Out.Add(S);
			}
		}
		return Out;
	}

	FString UpgradeLegacyWeapon(const FString& Weapon, const FString& Source, const FVector& Loc, const TArray<FRawWeaponSpot>& Spots)
	{
		if (Weapon != TEXT("RocketLauncher") && Weapon != TEXT("RailRifle"))
		{
			return Weapon;
		}
		FString Mapped = ExtraWeaponName(Source.ToLower());
		if (Mapped.IsEmpty())
		{
			for (const FRawWeaponSpot& S : Spots)
			{
				if (FVector::DistSquared(S.Location, Loc) < 1.f)
				{
					Mapped = ExtraWeaponName(S.Class.ToLower());
					break;
				}
			}
		}
		return Mapped.IsEmpty() ? Weapon : Mapped;
	}

	struct FLayoutCache
	{
		bool bValid = false;
		TArray<FArenaLayoutInfo> Layouts;
	};

	FLayoutCache& Cache()
	{
		static FLayoutCache C;
		return C;
	}

	/**
	 * Maps that have a marker json and their geometry: the cache's maps/<Map>/map.json in runtime mode, the
	 * cooked level /Game/Imported/Maps/<Map> otherwise. Scanned once, then cached (RefreshImportedLayouts).
	 */
	const TArray<FArenaLayoutInfo>& GetLayouts()
	{
		FLayoutCache& C = Cache();
		if (C.bValid)
		{
			return C.Layouts;
		}
		C.bValid = true;
		C.Layouts.Reset();

		const bool bRuntime = ArenaContent::UseRuntimeContent();
		const FString Dir = AArenaLevelBuilder::GetImportedDataDir();
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
		Files.Sort();
		// The marker files are large (raw markers): parse them in parallel.
		TArray<TOptional<FArenaLayoutInfo>> Parsed;
		Parsed.SetNum(Files.Num());
		ParallelFor(Files.Num(), [&](int32 Index)
		{
			const FString& File = Files[Index];
			const FString MapName = FPaths::GetBaseFilename(File);
			if (bRuntime)
			{
				if (!ArenaRuntimeMap::HasMap(MapName))
				{
					UE_LOG(LogOpenUT2k4, Verbose, TEXT("Map %s has markers but no map.json in the content cache"), *MapName);
					return;
				}
			}
			const TSharedPtr<FJsonObject> Root = LoadJson(Dir / File);
			if (!Root.IsValid())
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("Map %s: could not parse %s"), *MapName, *File);
				return;
			}
			bool bFlag[2] = { false, false };
			for (const TSharedPtr<FJsonValue>& V : ArrayField(Root, TEXT("flag_bases")))
			{
				const int32 Team = int32(V->AsObject()->GetNumberField(TEXT("team")));
				if (Team == 0 || Team == 1)
				{
					bFlag[Team] = true;
				}
			}
			FString Prefix = Root->GetStringField(TEXT("game_type_prefix")).ToUpper();

			FArenaLayoutInfo L;
			L.Id = AArenaLevelBuilder::MakeLayoutId(MapName);
			L.MapName = MapName;
			L.GameTypePrefix = Prefix;
			if (bFlag[0] && bFlag[1])
			{
				L.Features.Add(TEXT("FlagBases"));
			}
			const TSharedPtr<FJsonObject>* Raw = nullptr;
			if (Root->TryGetObjectField(TEXT("raw_markers"), Raw) && Raw && Raw->IsValid())
			{
				for (const auto& Pair : (*Raw)->Values)
				{
					const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
					if (Pair.Value.IsValid() && Pair.Value->TryGetArray(List) && List->Num() > 0)
					{
						L.Features.Add(FString(TEXT("raw:")) + FString(*Pair.Key));
					}
				}
			}
			L.DisplayName = Root->GetStringField(TEXT("title"));
			if (L.DisplayName.IsEmpty())
			{
				L.DisplayName = MapName;
			}
			L.Author = Root->GetStringField(TEXT("author"));
			L.Description = Root->GetStringField(TEXT("description"));
			// Every map has player starts, so DM/TDM always works; CTF needs a CTF map with both flag bases.
			L.bSupportsDM = true;
			// Vehicle CTF maps ("VCTF-", xVehicleCTFGame) are CTF maps with vehicles
			L.bSupportsCTF = (Prefix == TEXT("CTF") || Prefix == TEXT("VCTF")) && bFlag[0] && bFlag[1];
			int32 Recommended = 0;
			for (const TSharedPtr<FJsonValue>& V : ArrayField(Root, TEXT("recommended_players")))
			{
				Recommended = FMath::Max(Recommended, int32(V->AsNumber()));
			}
			L.RecommendedPlayers = Recommended > 0 ? Recommended : 8;
			Parsed[Index] = MoveTemp(L);
		});
		const TMap<FString, FString> StockTexts = bRuntime ? LoadStockMapTexts() : TMap<FString, FString>();
		int32 NoDescription = 0;
		for (TOptional<FArenaLayoutInfo>& L : Parsed)
		{
			if (!L.IsSet())
			{
				continue;
			}
			if (L->Description.IsEmpty())
			{
				if (const FString* Text = StockTexts.Find(L->MapName.ToLower()))
				{
					L->Description = *Text;
				}
			}
			if (bRuntime)
			{
				// custom content: unplayable maps are left out, the rest is marked (title / author from the .int / .ucl files)
				if (const ArenaUserContent::FMapInfo* U = ArenaUserContent::FindMap(L->MapName))
				{
					if (U->Status == ArenaUserContent::EMapStatus::Unplayable)
					{
						continue;
					}
					L->bCustom = true;
					L->DisplayName = U->Title.IsEmpty() ? L->MapName : U->Title;
					L->Author = U->Author.IsEmpty() ? L->Author : U->Author;
					L->Description = U->Description.IsEmpty() ? L->Description : U->Description;
					L->Screenshot = U->Screenshot;
					L->Notice = FString::Join(U->Problems, TEXT(" "));
					if (U->PlayersMax > 0)
					{
						L->RecommendedPlayers = U->PlayersMax;
					}
				}
			}
			if (!bRuntime)
			{
				const FString Level = FString::Printf(TEXT("%s/%s"), ImportedMapsDir, *L->MapName);
				if (!FPackageName::DoesPackageExist(Level))
				{
					UE_LOG(LogOpenUT2k4, Verbose, TEXT("Map %s has markers but no level (%s)"), *L->MapName, *Level);
					continue;
				}
			}
			NoDescription += L->Description.IsEmpty() ? 1 : 0;
			C.Layouts.Add(MoveTemp(L.GetValue()));
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Found %d map(s) in %s (%s), %d without a description"), C.Layouts.Num(), *Dir,
			bRuntime ? TEXT("runtime content") : TEXT("cooked imports"), NoDescription);
		return C.Layouts;
	}
}

namespace
{
	constexpr float StartHeightAboveFloor = 92.f;
	constexpr float PickupFloatHeight = 60.f;
}

AArenaLevelBuilder::AArenaLevelBuilder()
{
	PrimaryActorTick.bCanEverTick = false;

	BuilderRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BuilderRoot"));
	BuilderRoot->SetMobility(EComponentMobility::Static);
	RootComponent = BuilderRoot;

}

// ---------------------------------------------------------------------------------------------
// Layout registry (UT2004 maps from the user's install)
// ---------------------------------------------------------------------------------------------

TArray<FArenaLayoutInfo> AArenaLevelBuilder::GetLayouts()
{
	return ArenaImported::GetLayouts();
}

FString AArenaLevelBuilder::MakeLayoutId(const FString& MapName)
{
	return FString(ImportedLayoutPrefix()) + MapName;
}

FString AArenaLevelBuilder::GetMapNameFromLayoutId(const FString& LayoutId)
{
	return LayoutId.StartsWith(ImportedLayoutPrefix()) ? LayoutId.RightChop(FCString::Strlen(ImportedLayoutPrefix())) : LayoutId;
}

bool AArenaLevelBuilder::FindLayout(const FString& LayoutId, FArenaLayoutInfo& OutInfo)
{
	// "Imported:DM-Rankin" and plain "DM-Rankin" both work
	const FString MapName = GetMapNameFromLayoutId(LayoutId);
	if (MapName.IsEmpty())
	{
		return false;
	}
	for (const FArenaLayoutInfo& Info : GetLayouts())
	{
		if (Info.MapName.Equals(MapName, ESearchCase::IgnoreCase))
		{
			OutInfo = Info;
			return true;
		}
	}
	return false;
}

FString AArenaLevelBuilder::ChooseLayout(const FString& Requested, const FString& Preferred, TFunctionRef<bool(const FArenaLayoutInfo&)> Supports)
{
	FArenaLayoutInfo Info;
	if (FindLayout(Requested, Info) && Supports(Info))
	{
		return Info.Id;
	}
	if (FindLayout(Preferred, Info) && Supports(Info))
	{
		return Info.Id;
	}
	for (const FArenaLayoutInfo& L : GetLayouts())
	{
		if (Supports(L))
		{
			return L.Id;
		}
	}
	return FString();
}

bool AArenaLevelBuilder::HasAnyLayouts()
{
	return GetLayouts().Num() > 0;
}

// ---------------------------------------------------------------------------------------------
// Imported maps
// ---------------------------------------------------------------------------------------------

FString AArenaLevelBuilder::GetImportedDataDir()
{
	const FString Cooked = FPaths::ProjectContentDir() / TEXT("ImportedData");
	if (ArenaContent::UseRuntimeContent())
	{
		const FString CacheData = ArenaContent::GetCacheDataDir();
		if (IFileManager::Get().DirectoryExists(*CacheData))
		{
			return CacheData;
		}
		// DEV FALLBACK ONLY: a developer cache made by Tools/ut2k4_export.py has no data/ folder (the editor
		// importer writes the marker json to Content/ImportedData). Release builds never stage that folder.
		if (IFileManager::Get().DirectoryExists(*Cooked))
		{
			return Cooked;
		}
		return CacheData;
	}
	return Cooked;
}

bool AArenaLevelBuilder::HasImportedData(const FString& MapName)
{
	return !MapName.IsEmpty() && FPaths::FileExists(GetImportedDataDir() / (MapName + TEXT(".json")));
}

FString AArenaLevelBuilder::GetLevelForLayout(const FString& LayoutId)
{
	// Runtime content: every map is built into the shipped base level.
	if (!ArenaContent::UseRuntimeContent())
	{
		const FString MapName = GetMapNameFromLayoutId(LayoutId);
		const FString Level = FString::Printf(TEXT("%s/%s"), ArenaImported::ImportedMapsDir, *MapName);
		if (HasImportedData(MapName) && FPackageName::DoesPackageExist(Level))
		{
			return Level;
		}
	}
	return ArenaImported::ArenaLevel;
}

FString AArenaLevelBuilder::GetBaseLevel()
{
	return ArenaImported::ArenaLevel;
}

FString AArenaLevelBuilder::GetCurrentMapName(const UWorld* World)
{
	if (!World)
	{
		return FString();
	}
	const FString Package = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	if (Package != ArenaImported::ArenaLevel)
	{
		const FString ShortName = FPackageName::GetShortName(Package);
		return HasImportedData(ShortName) ? ShortName : FString();
	}
	for (TActorIterator<AArenaLevelBuilder> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (It->IsImportedMap())
		{
			return GetMapNameFromLayoutId(It->GetBuiltLayoutId());
		}
	}
	return FString();
}

void AArenaLevelBuilder::RefreshImportedLayouts()
{
	ArenaImported::Cache().bValid = false;
}

FVector AArenaLevelBuilder::FindFloorBelow(const FVector& Location, float FallbackDrop, bool* bOutFound) const
{
	if (bOutFound)
	{
		*bOutFound = false;
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return Location - FVector(0.f, 0.f, FallbackDrop);
	}
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaImportFloor), true, this);
	for (const TObjectPtr<AActor>& Actor : SpawnedActors)
	{
		Query.AddIgnoredActor(Actor.Get());
	}
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Location + FVector(0.f, 0.f, 60.f), Location - FVector(0.f, 0.f, 600.f), ECC_WorldStatic, Query))
	{
		if (bOutFound)
		{
			*bOutFound = true;
		}
		return Hit.ImpactPoint;
	}
	return Location - FVector(0.f, 0.f, FallbackDrop);
}

FBox AArenaLevelBuilder::ComputeStaticGeometryBounds() const
{
	FBox Bounds(ForceInit);
	for (TActorIterator<AStaticMeshActor> It(GetWorld()); It; ++It)
	{
		const UStaticMeshComponent* Comp = It->GetStaticMeshComponent();
		if (Comp && Comp->GetStaticMesh() && Comp->IsCollisionEnabled())
		{
			Bounds += Comp->Bounds.GetBox();
		}
	}
	return Bounds;
}

bool AArenaLevelBuilder::BuildImported(const FString& MapName, bool bTeamGame)
{
	using namespace ArenaImported;

	if (!BuiltLayoutId.IsEmpty())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaLevelBuilder: already built, ignoring BuildImported"));
		return false;
	}
	const FString Path = GetImportedDataDir() / (MapName + TEXT(".json"));
	const TSharedPtr<FJsonObject> Root = LoadJson(Path);
	if (!Root.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("ArenaLevelBuilder: could not read %s"), *Path);
		return false;
	}
	const double StartTime = FPlatformTime::Seconds();
	ImportedJson = Root;

	// ---- bounds: colliding static geometry, clipped to the playable area so sky boxes stay out ----
	const FBox GeometryBounds = ComputeStaticGeometryBounds();
	FBox PlayBounds(ForceInit);
	const TSharedPtr<FJsonObject>* PlayObj = nullptr;
	if (Root->TryGetObjectField(TEXT("play_bounds"), PlayObj))
	{
		FVector Min, Max;
		if (ReadVector(*PlayObj, TEXT("min"), Min) && ReadVector(*PlayObj, TEXT("max"), Max))
		{
			PlayBounds = FBox(Min - FVector(1500.f, 1500.f, 800.f), Max + FVector(1500.f, 1500.f, 1500.f));
		}
	}
	if (GeometryBounds.IsValid && PlayBounds.IsValid && GeometryBounds.Intersect(PlayBounds))
	{
		ArenaBounds = GeometryBounds.Overlap(PlayBounds);
	}
	else
	{
		ArenaBounds = GeometryBounds.IsValid ? GeometryBounds : PlayBounds;
	}
	if (!ArenaBounds.IsValid)
	{
		ArenaBounds = FBox(FVector(-2000.f), FVector(2000.f));
	}
	double JsonKillZ = 0.0;
	KillZ = Root->TryGetNumberField(TEXT("kill_z"), JsonKillZ) ? float(JsonKillZ) : float(ArenaBounds.Min.Z - 500.f);

	// ---- player starts ----
	for (const TSharedPtr<FJsonValue>& V : ArrayField(Root, TEXT("player_starts")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVector Loc;
		if (!ReadVector(O, TEXT("location"), Loc))
		{
			continue;
		}
		const int32 Team = int32(O->GetNumberField(TEXT("team")));
		bool bFloor = false;
		const FVector Floor = FindFloorBelow(Loc, 80.f, &bFloor);
		if (!bFloor)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaLevelBuilder: no floor below player start %s, skipped"), *Loc.ToString());
			continue;
		}
		AddPlayerStart(Floor, float(O->GetNumberField(TEXT("yaw"))), bTeamGame && (Team == 0 || Team == 1) ? Team : ArenaTeam::None);
	}

	// ---- pickups ----
	const UEnum* PickupEnum = StaticEnum<EArenaPickupType>();
	int32 Unknown = 0;
	const TArray<FRawWeaponSpot> RawSpots = RawWeaponSpots(Root);
	TMap<FString, int32> ExtraPickups;
	for (const TSharedPtr<FJsonValue>& V : ArrayField(Root, TEXT("pickups")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVector Loc;
		const int64 Type = PickupEnum->GetValueByNameString(O->GetStringField(TEXT("type")));
		if (!ReadVector(O, TEXT("location"), Loc) || Type == INDEX_NONE)
		{
			++Unknown;
			continue;
		}
		FString Weapon;
		O->TryGetStringField(TEXT("weapon"), Weapon);
		{
			FString PickupSource;
			O->TryGetStringField(TEXT("source"), PickupSource);
			Weapon = UpgradeLegacyWeapon(Weapon, PickupSource, Loc, RawSpots);
			if (!ExtraWeaponName(Weapon.ToLower()).IsEmpty() || Weapon == TEXT("WarheadLauncher") || Weapon == TEXT("OrbitalDesignator")
				|| Weapon == TEXT("MarksmanRifle") || Weapon == TEXT("SeekerLauncher") || Weapon == TEXT("StickyLauncher") || Weapon == TEXT("CrawlerLayer"))
			{
				ExtraPickups.FindOrAdd(O->GetStringField(TEXT("type")) + TEXT(" ") + Weapon)++;
			}
		}
		const TSubclassOf<AArenaWeapon> WeaponClass = ArenaImported::WeaponClass(Weapon);
		if ((Type == int64(EArenaPickupType::Weapon) || Type == int64(EArenaPickupType::Ammo)) && !WeaponClass)
		{
			++Unknown;
			continue;
		}
		if (AArenaPickup* Pickup = AddPickup(FindFloorBelow(Loc, 40.f), int32(Type), WeaponClass))
		{
			// xWeaponBase / *Charger sources stood on a pickup base in the original map.
			FString Source;
			O->TryGetStringField(TEXT("source"), Source);
			Pickup->SetOnBase(Source.Contains(TEXT("Charger")) || Source.Contains(TEXT("WeaponBase")));
		}
	}

	for (const TPair<FString, int32>& E : ExtraPickups)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ArenaLevelBuilder: %d x %s (UT2004 super / Onslaught / classic weapon)"), E.Value, *E.Key);
	}

	// ---- jump pads (UT jump pads and converted lifts) ----
	// runtime maps with native movers have real lifts (ArenaMover): no stand-in pads
	const bool bNativeLifts = ArenaUE2World::HasNativeMovers(GetWorld());
	for (const TSharedPtr<FJsonValue>& V : ArrayField(Root, TEXT("jump_pads")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FString PadSource;
		if (bNativeLifts && O->TryGetStringField(TEXT("source"), PadSource) && PadSource.StartsWith(TEXT("Lift:")))
		{
			continue;
		}
		FVector Loc, Target;
		if (!ReadVector(O, TEXT("location"), Loc) || !ReadVector(O, TEXT("target"), Target))
		{
			continue;
		}
		// the UE2 pad (runtime maps: its collision cylinder; else the raw marker's JumpVelocity, default cylinder)
		ArenaUE2World::FJumpPadInfo RawPad;
		const ArenaUE2World::FJumpPadInfo* UE2Pad = ArenaUE2World::FindJumpPad(GetWorld(), Loc);
		if (!UE2Pad && !PadSource.StartsWith(TEXT("Lift:")))
		{
			const TSharedPtr<FJsonObject>* Raw = nullptr;
			if (Root->TryGetObjectField(TEXT("raw_markers"), Raw))
			{
				for (const TSharedPtr<FJsonValue>& RV : ArrayField(*Raw, TEXT("jump_pads")))
				{
					const TSharedPtr<FJsonObject> R = RV->AsObject();
					FVector RLoc;
					if (R.IsValid() && ReadVector(R, TEXT("location"), RLoc) && FVector::DistSquared(RLoc, Loc) < 50.f * 50.f)
					{
						RawPad.Location = RLoc;
						ReadVector(R, TEXT("jump_velocity"), RawPad.JumpVelocity);
						R->TryGetStringField(TEXT("actor"), RawPad.Name);
						RawPad.HalfHeight = 86.f;
						UE2Pad = &RawPad;
						break;
					}
				}
			}
		}
		bool bPadFloor = false;
		FVector PadFloor = FindFloorBelow(Loc, 40.f, &bPadFloor);
		if (UE2Pad && !bPadFloor)
		{
			PadFloor = UE2Pad->Location - FVector(0.f, 0.f, UE2Pad->HalfHeight);
		}
		const FVector TargetFloor = FindFloorBelow(Target, 80.f);
		const float Apex = FMath::Clamp(float(FVector::Dist2D(PadFloor, TargetFloor)) * 0.2f, 150.f, 600.f);
		AddJumpPad(PadFloor, TargetFloor, Apex, UE2Pad);
	}

	// ---- anchors (flag bases, raw markers) ----
	AddImportedAnchors();

	// ---- lighting: ambient floor, exposure (UT2004 zone ambient / lightmaps have no Lumen equivalent) ----
	ArenaImportedLighting::Apply(GetWorld(), Root);

	BuiltLayoutId = FString(ImportedLayoutPrefix()) + MapName;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ArenaLevelBuilder: imported map '%s' starts=%d pickups=%d (unmapped %d) pads=%d anchors=%d bounds=%s killz=%.0f (%.3f s)"),
		*MapName, PlayerStarts.Num(), Pickups.Num(), Unknown, JumpPads.Num(), Anchors.Num(),
		*ArenaBounds.ToString(), KillZ, FPlatformTime::Seconds() - StartTime);
	return PlayerStarts.Num() > 0;
}

// ---------------------------------------------------------------------------------------------
// Anchors and imported json access (for game rules)
// ---------------------------------------------------------------------------------------------

void AArenaLevelBuilder::AddAnchor(FName Name, const FVector& FloorLocation, float Yaw)
{
	Anchors.FindOrAdd(Name).Add(FTransform(FRotator(0.f, Yaw, 0.f), FloorLocation));
}

TArray<FTransform> AArenaLevelBuilder::GetAnchorPoints(FName Name) const
{
	const TArray<FTransform>* Found = Anchors.Find(Name);
	return Found ? *Found : TArray<FTransform>();
}

bool AArenaLevelBuilder::GetAnchorPoint(FName Name, FTransform& Out) const
{
	const TArray<FTransform>* Found = Anchors.Find(Name);
	if (!Found || Found->Num() == 0)
	{
		return false;
	}
	Out = (*Found)[0];
	return true;
}

TArray<FTransform> AArenaLevelBuilder::GetAnchorSeries(const FString& Prefix) const
{
	TArray<FTransform> Result;
	for (int32 i = 0; ; ++i)
	{
		const TArray<FTransform>* Found = Anchors.Find(FName(*FString::Printf(TEXT("%s%d"), *Prefix, i)));
		if (!Found || Found->Num() == 0)
		{
			break;
		}
		Result.Add((*Found)[0]);
	}
	return Result;
}

TArray<FName> AArenaLevelBuilder::GetAnchorNames() const
{
	TArray<FName> Names;
	Anchors.GetKeys(Names);
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Names;
}

bool AArenaLevelBuilder::ReadJsonVector(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& Out)
{
	return ArenaImported::ReadVector(Obj, *Field, Out);
}

bool AArenaLevelBuilder::ReadJsonRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FRotator& Out)
{
	if (!Obj.IsValid() || !Obj->HasField(Field))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Obj->TryGetArrayField(Field, Arr) && Arr->Num() >= 3)
	{
		Out = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (Obj->TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
	{
		double P = 0.0, Y = 0.0, R = 0.0;
		(*Sub)->TryGetNumberField(TEXT("pitch"), P);
		(*Sub)->TryGetNumberField(TEXT("yaw"), Y);
		(*Sub)->TryGetNumberField(TEXT("roll"), R);
		Out = FRotator(P, Y, R);
		return true;
	}
	double Yaw = 0.0;
	if (Obj->TryGetNumberField(Field, Yaw))
	{
		Out = FRotator(0.0, Yaw, 0.0);
		return true;
	}
	return false;
}

bool AArenaLevelBuilder::ParseRawMarker(const TSharedPtr<FJsonObject>& Obj, FArenaRawMarker& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	Out = FArenaRawMarker();
	Out.Json = Obj;
	Obj->TryGetStringField(TEXT("class"), Out.Class);
	const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
	if (Obj->TryGetArrayField(TEXT("class_chain"), Chain))
	{
		for (const TSharedPtr<FJsonValue>& V : *Chain)
		{
			FString Name;
			if (V.IsValid() && V->TryGetString(Name))
			{
				Out.ClassChain.Add(Name);
			}
		}
	}
	if (!ReadJsonVector(Obj, TEXT("location"), Out.Location))
	{
		return false;
	}
	if (!ReadJsonRotator(Obj, TEXT("rotation"), Out.Rotation))
	{
		ReadJsonRotator(Obj, TEXT("yaw"), Out.Rotation);
	}
	const TSharedPtr<FJsonObject>* Props = nullptr;
	if (Obj->TryGetObjectField(TEXT("properties"), Props) && Props)
	{
		Out.Properties = *Props;
	}
	return true;
}

TArray<FArenaRawMarker> AArenaLevelBuilder::GetRawMarkers(const FString& ListName) const
{
	TArray<FArenaRawMarker> Result;
	const TSharedPtr<FJsonObject>* Raw = nullptr;
	if (!ImportedJson.IsValid() || !ImportedJson->TryGetObjectField(TEXT("raw_markers"), Raw) || !Raw || !Raw->IsValid())
	{
		return Result;
	}
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (!(*Raw)->TryGetArrayField(ListName, List))
	{
		return Result;
	}
	for (const TSharedPtr<FJsonValue>& V : *List)
	{
		FArenaRawMarker M;
		if (V.IsValid() && V->Type == EJson::Object && ParseRawMarker(V->AsObject(), M))
		{
			Result.Add(MoveTemp(M));
		}
	}
	return Result;
}

TArray<FString> AArenaLevelBuilder::GetRawMarkerListNames() const
{
	TArray<FString> Names;
	const TSharedPtr<FJsonObject>* Raw = nullptr;
	if (ImportedJson.IsValid() && ImportedJson->TryGetObjectField(TEXT("raw_markers"), Raw) && Raw && Raw->IsValid())
	{
		for (const auto& Pair : (*Raw)->Values)
		{
			Names.Add(FString(*Pair.Key));
		}
		Names.Sort();
	}
	return Names;
}

bool FArenaRawMarker::IsA(const FString& ClassName) const
{
	if (Class.Equals(ClassName, ESearchCase::IgnoreCase))
	{
		return true;
	}
	for (const FString& C : ClassChain)
	{
		if (C.Equals(ClassName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

namespace
{
	const TSharedPtr<FJsonValue>* FindPropertyIgnoreCase(const TSharedPtr<FJsonObject>& Props, const FString& Key)
	{
		if (!Props.IsValid())
		{
			return nullptr;
		}
		for (const auto& Pair : Props->Values)
		{
			if (FCString::Stricmp(*Pair.Key, *Key) == 0)
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}
}

bool FArenaRawMarker::GetStringProperty(const FString& Key, FString& Out) const
{
	const TSharedPtr<FJsonValue>* V = FindPropertyIgnoreCase(Properties, Key);
	return V && V->IsValid() && (*V)->TryGetString(Out);
}

bool FArenaRawMarker::GetNumberProperty(const FString& Key, double& Out) const
{
	const TSharedPtr<FJsonValue>* V = FindPropertyIgnoreCase(Properties, Key);
	if (!V || !V->IsValid())
	{
		return false;
	}
	if ((*V)->TryGetNumber(Out))
	{
		return true;
	}
	FString Str;
	if ((*V)->TryGetString(Str) && Str.IsNumeric())
	{
		Out = FCString::Atod(*Str);
		return true;
	}
	return false;
}

int32 FArenaRawMarker::GetTeam() const
{
	static const TCHAR* const Keys[] = { TEXT("team"), TEXT("DefenderTeamIndex"), TEXT("TeamNum"), TEXT("TeamIndex") };
	for (const TCHAR* Key : Keys)
	{
		double Value = 0.0;
		if (GetNumberProperty(Key, Value))
		{
			const int32 Team = int32(Value);
			return (Team == 0 || Team == 1) ? Team : -1;
		}
	}
	if (Json.IsValid())
	{
		double Value = 0.0;
		if (Json->TryGetNumberField(TEXT("team"), Value))
		{
			const int32 Team = int32(Value);
			return (Team == 0 || Team == 1) ? Team : -1;
		}
	}
	return -1;
}

bool AArenaLevelBuilder::TraceFloor(const FVector& Location, float MaxDrop, FVector& OutFloor) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaTraceFloor), true);
	for (const TObjectPtr<AActor>& Actor : SpawnedActors)
	{
		Query.AddIgnoredActor(Actor.Get());
	}
	FHitResult Hit;
	if (World->LineTraceSingleByObjectType(Hit, Location + FVector(0.f, 0.f, 60.f), Location - FVector(0.f, 0.f, MaxDrop), FCollisionObjectQueryParams(ECC_WorldStatic), Query))
	{
		OutFloor = Hit.ImpactPoint;
		return true;
	}
	return false;
}

void AArenaLevelBuilder::AddImportedAnchors()
{
	using namespace ArenaImported;
	if (!ImportedJson.IsValid())
	{
		return;
	}
	auto Floor = [this](const FVector& Loc, float Drop) { return FindFloorBelow(Loc, Drop); };

	// Flag bases -> TeamBase0/1 (same floor placement the CTF flags always used)
	for (const TSharedPtr<FJsonValue>& V : ArrayField(ImportedJson, TEXT("flag_bases")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVector Loc;
		const int32 Team = O.IsValid() ? int32(O->GetNumberField(TEXT("team"))) : -1;
		const FName Name(*FString::Printf(TEXT("TeamBase%d"), Team));
		if (ReadVector(O, TEXT("location"), Loc) && (Team == 0 || Team == 1) && !Anchors.Contains(Name))
		{
			double Yaw = 0.0;
			O->TryGetNumberField(TEXT("yaw"), Yaw);
			AddAnchor(Name, Floor(Loc, 60.f), float(Yaw));
		}
	}

	// Raw markers (when the importer provides them). Lists that are not mapped here stay available
	// through GetRawMarkers().
	for (const FArenaRawMarker& M : GetRawMarkers(TEXT("bomb_deliveries")))
	{
		const int32 Team = M.GetTeam();
		const FName Goal(*FString::Printf(TEXT("Goal%d"), Team));
		if ((Team == 0 || Team == 1) && !Anchors.Contains(Goal))
		{
			AddAnchor(Goal, M.Location, M.Rotation.Yaw); // goals may float: raw location
			const FName Base(*FString::Printf(TEXT("TeamBase%d"), Team));
			if (!Anchors.Contains(Base))
			{
				AddAnchor(Base, Floor(M.Location, 0.f), M.Rotation.Yaw);
			}
		}
	}
	for (const FArenaRawMarker& M : GetRawMarkers(TEXT("bomb_spawns")))
	{
		if (!Anchors.Contains(TEXT("Center")))
		{
			AddAnchor(TEXT("Center"), Floor(M.Location, 0.f), M.Rotation.Yaw);
		}
	}
	{
		const TArray<FArenaRawMarker> Points = GetRawMarkers(TEXT("domination_points"));
		const TCHAR* const Names[2] = { TEXT("ControlA"), TEXT("ControlB") };
		for (int32 i = 0; i < Points.Num() && i < 2; ++i)
		{
			AddAnchor(Names[i], Floor(Points[i].Location, 0.f), Points[i].Rotation.Yaw);
		}
	}
	{
		int32 NumObjectives = 0, NumNodes = 0;
		for (const FArenaRawMarker& M : GetRawMarkers(TEXT("game_objectives")))
		{
			if (M.IsA(TEXT("ONSPowerCore")))
			{
				const int32 Team = M.GetTeam();
				if (Team == 0 || Team == 1)
				{
					AddAnchor(FName(*FString::Printf(TEXT("Core%d"), Team)), Floor(M.Location, 0.f), M.Rotation.Yaw);
				}
			}
			else if (M.IsA(TEXT("ONSPowerNode")))
			{
				AddAnchor(FName(*FString::Printf(TEXT("Node%d"), NumNodes++)), Floor(M.Location, 0.f), M.Rotation.Yaw);
			}
			else if (!M.IsA(TEXT("xDomPoint")) && !M.IsA(TEXT("DominationPoint")))
			{
				AddAnchor(FName(*FString::Printf(TEXT("Objective%d"), NumObjectives++)), M.Location, M.Rotation.Yaw);
			}
		}
	}
	{
		int32 Index = 0;
		for (const FArenaRawMarker& M : GetRawMarkers(TEXT("vehicle_factories")))
		{
			AddAnchor(FName(*FString::Printf(TEXT("VehicleSpawn%d"), Index++)), M.Location, M.Rotation.Yaw);
		}
	}
}

void AArenaLevelBuilder::ValidateAnchors() const
{
#if !UE_BUILD_SHIPPING
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
	for (const TPair<FName, TArray<FTransform>>& Pair : Anchors)
	{
		for (const FTransform& T : Pair.Value)
		{
			const FVector Loc = T.GetLocation();
			FVector FloorLoc;
			if (!TraceFloor(Loc, 150.f, FloorLoc) || FMath::Abs(FloorLoc.Z - Loc.Z) > 40.f)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaLevelBuilder: anchor %s at %s has no floor right below"), *Pair.Key.ToString(), *Loc.ToString());
			}
			// Only level geometry counts: mode actors (an AS objective, a flag base) stand on their anchor by design.
			FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaAnchorCheck), false);
			TArray<FOverlapResult> Overlaps;
			World->OverlapMultiByChannel(Overlaps, Loc + FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight + 5.f), FQuat::Identity, ECC_Pawn, Capsule, Query);
			const bool bBlocked = Overlaps.ContainsByPredicate([this](const FOverlapResult& O)
			{
				const AActor* A = O.GetActor();
				return O.bBlockingHit && (A == this || (A && A->IsA<AStaticMeshActor>()));
			});
			if (bBlocked)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaLevelBuilder: anchor %s at %s is blocked by geometry"), *Pair.Key.ToString(), *Loc.ToString());
			}
		}
	}
#endif
}

// ---------------------------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------------------------

APlayerStart* AArenaLevelBuilder::AddSpawnPoint(const FVector& FloorLocation, float Yaw, int32 Team)
{
	return AddPlayerStart(FloorLocation, Yaw, Team);
}

APlayerStart* AArenaLevelBuilder::AddPlayerStart(const FVector& Location, float Yaw, int32 Team)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FVector Loc = Location + FVector(0.f, 0.f, StartHeightAboveFloor);

	// The spawn capsule must be free of level geometry. Starts on slopes or against walls (imported maps,
	// ramps) are nudged to the nearest free spot that still stands on a floor at about the same height.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaStartCheck), false);
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
	auto IsFree = [&](const FVector& P) { return !World->OverlapBlockingTestByChannel(P, FQuat::Identity, ECC_Pawn, Capsule, Query); };
	if (!IsFree(Loc))
	{
		bool bMoved = false;
		const FVector Original = Loc;
		for (const float Up : { 0.f, 12.f, 25.f, 45.f })
		{
			for (const float R : { 0.f, 20.f, 40.f, 60.f })
			{
				for (int32 k = 0; k < (R > 0.f ? 8 : 1) && !bMoved; ++k)
				{
					const float A = PI * 0.25f * k;
					const FVector P = Original + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, Up);
					FVector Floor;
					if (IsFree(P) && TraceFloor(P - FVector(0.f, 0.f, StartHeightAboveFloor), 120.f, Floor) &&
						FMath::Abs(Floor.Z - Location.Z) < 60.f)
					{
						Loc = FVector(P.X, P.Y, FMath::Max(P.Z, Floor.Z + StartHeightAboveFloor));
						bMoved = IsFree(Loc);
					}
				}
				if (bMoved)
				{
					break;
				}
			}
			if (bMoved)
			{
				break;
			}
		}
		if (bMoved)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("ArenaLevelBuilder: player start at %s overlapped geometry, moved %.0f cm"), *Original.ToString(), FVector::Dist(Original, Loc));
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("ArenaLevelBuilder: player start at %s overlaps geometry"), *Loc.ToString());
		}
	}

	APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Loc, FRotator(0.f, Yaw, 0.f), Params);
	if (!Start)
	{
		return nullptr;
	}
	Start->PlayerStartTag = Team == ArenaTeam::Red ? FName(TEXT("Red")) : Team == ArenaTeam::Blue ? FName(TEXT("Blue")) : FName(TEXT("Any"));
	PlayerStarts.Add(Start);
	SpawnedActors.Add(Start);
	return Start;
}

bool AArenaLevelBuilder::IsPickupAllowed(int32 PickupType, TSubclassOf<AArenaWeapon> WeaponClass) const
{
	const UWorld* World = GetWorld();
	if (ArenaUE2World::IsUE2NetWorld(World))
	{
		return true; // a UT2004 server's world: every map pickup, hidden until the server shows it (AArenaPickup::BeginPlay)
	}
	const AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	const UArenaGameRules* Rules = GM ? GM->GetRules() : nullptr;
	if (!Rules)
	{
		return !(GM && GM->IsInstagib() && PickupType != int32(EArenaPickupType::Adrenaline));
	}
	return Rules->AllowPickups() && Rules->AllowPickup(static_cast<EArenaPickupType>(PickupType), WeaponClass);
}

AArenaPickup* AArenaLevelBuilder::AddPickup(const FVector& FloorLocation, int32 PickupType, TSubclassOf<AArenaWeapon> WeaponClass)
{
	UWorld* World = GetWorld();
	if (!World || PickupType < 0 || PickupType > int32(EArenaPickupType::Ammo))
	{
		return nullptr;
	}
	EArenaPickupType Type = static_cast<EArenaPickupType>(PickupType);
	// UnrealScript mutators: Mutator.CheckRelevance may remove or replace the pickup (no-op without script).
	if (!ArenaUScriptHooks::FilterPickup(World, FloorLocation, Type, WeaponClass))
	{
		return nullptr;
	}
	PickupType = int32(Type);
	if (!IsPickupAllowed(PickupType, WeaponClass))
	{
		return nullptr;
	}
	if ((Type == EArenaPickupType::Weapon || Type == EArenaPickupType::Ammo) && !WeaponClass)
	{
		return nullptr;
	}

	const FTransform T(FRotator::ZeroRotator, FloorLocation + FVector(0.f, 0.f, PickupFloatHeight));
	AArenaPickup* Pickup = World->SpawnActorDeferred<AArenaPickup>(AArenaPickup::StaticClass(), T, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Pickup)
	{
		return nullptr;
	}
	Pickup->Setup(Type, WeaponClass);
	Pickup->FinishSpawning(T);
	Pickups.Add(Pickup);
	SpawnedActors.Add(Pickup);
	return Pickup;
}

AArenaJumpPad* AArenaLevelBuilder::AddJumpPad(const FVector& FloorLocation, const FVector& Target, float ApexHeight, const ArenaUE2World::FJumpPadInfo* UE2Pad)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	const FTransform T(FRotator::ZeroRotator, FloorLocation);
	AArenaJumpPad* Pad = World->SpawnActorDeferred<AArenaJumpPad>(AArenaJumpPad::StaticClass(), T, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Pad)
	{
		return nullptr;
	}
	// Target is a floor point; the launch solves for the character centre (capsule half height above floor).
	Pad->SetTarget(Target + FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight), ApexHeight);
	if (UE2Pad)
	{
		Pad->UE2Name = UE2Pad->Name;
		Pad->SetUE2Collision(UE2Pad->Location, UE2Pad->Radius, UE2Pad->HalfHeight);
		Pad->SetUE2JumpVelocity(UE2Pad->JumpVelocity);
	}
	Pad->FinishSpawning(T);
	JumpPads.Add(Pad);
	SpawnedActors.Add(Pad);
	return Pad;
}

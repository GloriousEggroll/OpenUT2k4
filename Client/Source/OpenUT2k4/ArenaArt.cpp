#include "ArenaArt.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaRuntimeContent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ArenaArtPrivate
{
	static TAutoConsoleVariable<int32> CVarImportedArt(
		TEXT("arena.ImportedArt"), 1,
		TEXT("1 = use replacement / imported character and weapon art when present, 0 = always use the procedural visuals."),
		ECVF_Default);

	static const TCHAR* const Roots[] = { TEXT("/Game/Replacements/"), TEXT("/Game/Imported/") };

	struct FCacheKey
	{
		UClass* Class;
		FString Path;
		bool operator==(const FCacheKey& O) const { return Class == O.Class && Path == O.Path; }
		friend uint32 GetTypeHash(const FCacheKey& K) { return HashCombine(PointerHash(K.Class), GetTypeHash(K.Path)); }
	};

	TMap<FCacheKey, TStrongObjectPtr<UObject>>& Cache()
	{
		static TMap<FCacheKey, TStrongObjectPtr<UObject>> Map;
		return Map;
	}
}

bool ArenaArt::IsEnabled()
{
	return ArenaArtPrivate::CVarImportedArt.GetValueOnGameThread() != 0;
}

UObject* ArenaArt::LoadArt(UClass* Class, const FString& RelativePath)
{
	using namespace ArenaArtPrivate;
	if (!Class || RelativePath.IsEmpty() || !IsEnabled())
	{
		return nullptr;
	}
	const FCacheKey Key{ Class, RelativePath };
	if (TStrongObjectPtr<UObject>* Found = Cache().Find(Key))
	{
		return Found->Get();
	}

	UObject* Result = nullptr;
	const FString ObjectName = FPaths::GetBaseFilename(RelativePath);
	// Runtime content: /Game/Imported is not used; the provider builds the asset from the content cache.
	const bool bRuntime = ArenaContent::UseRuntimeContent();
	for (const TCHAR* Root : Roots)
	{
		if (bRuntime && Root != Roots[0])
		{
			Result = ArenaRuntimeContent::LoadByRelativePath(Class, RelativePath);
			break;
		}
		const FString Package = FString(Root) + RelativePath;
		// Checking the package first keeps missing optional art silent (no load warnings).
		if (!FPackageName::DoesPackageExist(Package))
		{
			continue;
		}
		const FString ObjectPath = Package + TEXT(".") + ObjectName;
		Result = StaticLoadObject(Class, nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Result)
		{
			break;
		}
	}
	UE_LOG(LogOpenUT2k4, Verbose, TEXT("ArenaArt: %s %s -> %s"), *Class->GetName(), *RelativePath, Result ? *Result->GetPathName() : TEXT("missing"));
	Cache().Add(Key, TStrongObjectPtr<UObject>(Result));
	return Result;
}

UObject* ArenaArt::LoadImportedPath(UClass* Class, const FString& ImportedPath)
{
	// "/Game/Imported/<rel>" or "/Game/Imported/<rel>.<Name>" -> LoadArt(<rel>)
	static const FString Prefix = TEXT("/Game/Imported/");
	if (!ImportedPath.StartsWith(Prefix))
	{
		return nullptr;
	}
	FString Rel = ImportedPath.RightChop(Prefix.Len());
	int32 Dot = INDEX_NONE;
	if (Rel.FindLastChar(TEXT('.'), Dot) && Dot > Rel.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		Rel.LeftInline(Dot);
	}
	return LoadArt(Class, Rel);
}

USoundBase* ArenaArt::LoadOneShotSound(const FString& RelativePath)
{
	USoundBase* Sound = Load<USoundBase>(RelativePath);
	if (const USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		if (Wave->bLooping)
		{
			return nullptr;
		}
	}
	return Sound;
}

// =================================================================================================
// Characters
// =================================================================================================

const TArray<ArenaArt::FCharacterModel>& ArenaArt::GetCharacterModels()
{
	static const TArray<FCharacterModel> Models = {
		{ TEXT("SkeletalMeshes/HumanMaleA/MercMaleA"),     TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("MercMaleABodyA"),     false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/MercFemaleA"), TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("MercFemaleABodyA"),   true },
		{ TEXT("SkeletalMeshes/Jugg/JuggMaleA"),           TEXT("Animations/Jugg/JuggA_"),                TEXT("JuggMaleABodyA"),     false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/NightFemaleA"),TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("NightFemaleABodyA"),  true },
		{ TEXT("SkeletalMeshes/HumanMaleA/NightMaleA"),    TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("NightMaleABodyA"),    false },
		{ TEXT("SkeletalMeshes/Jugg/JuggFemaleA"),         TEXT("Animations/Jugg/JuggA_"),                TEXT("JuggFemaleABodyA"),   true },
		{ TEXT("SkeletalMeshes/Bot/BotA"),                 TEXT("Animations/Bot/BotA_"),                  TEXT("BotABodyA"),          false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/EgyptFemaleA"),TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("EgyptFemaleABodyA"),  true },
		{ TEXT("SkeletalMeshes/HumanMaleA/MercMaleB"),     TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("MercMaleBBodyA"),     false },
		{ TEXT("SkeletalMeshes/Aliens/AlienFemaleA"),      TEXT("Animations/Aliens/AlienA_"),             TEXT("AlienFemaleABodyA"),  true },
		{ TEXT("SkeletalMeshes/Aliens/AlienMaleA"),        TEXT("Animations/Aliens/AlienA_"),             TEXT("AlienMaleABodyA"),    false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/MercFemaleB"), TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("MercFemaleBBodyA"),   true },
		{ TEXT("SkeletalMeshes/HumanMaleA/EgyptMaleA"),    TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("EgyptMaleABodyA"),    false },
		{ TEXT("SkeletalMeshes/Jugg/JuggFemaleB"),         TEXT("Animations/Jugg/JuggA_"),                TEXT("JuggFemaleBBodyA"),   true },
		{ TEXT("SkeletalMeshes/Jugg/JuggMaleB"),           TEXT("Animations/Jugg/JuggA_"),                TEXT("JuggMaleBBodyA"),     false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/NightFemaleB"),TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("NightFemaleBBodyA"),  true },
		{ TEXT("SkeletalMeshes/HumanMaleA/MercMaleC"),     TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("MercMaleCBodyA"),     false },
		{ TEXT("SkeletalMeshes/Aliens/AlienFemaleB"),      TEXT("Animations/Aliens/AlienA_"),             TEXT("AlienFemaleBBodyA"),  true },
		{ TEXT("SkeletalMeshes/Bot/BotB"),                 TEXT("Animations/Bot/BotA_"),                  TEXT("BotBBodyA"),          false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/MercFemaleC"), TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("MercFemaleCBodyA"),   true },
		{ TEXT("SkeletalMeshes/HumanMaleA/NightMaleB"),    TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("NightMaleBBodyA"),    false },
		{ TEXT("SkeletalMeshes/HumanFemaleA/EgyptFemaleB"),TEXT("Animations/HumanFemaleA/BipedFemaleA_"), TEXT("EgyptFemaleBBodyA"),  true },
		{ TEXT("SkeletalMeshes/HumanMaleA/MercMaleD"),     TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("MercMaleDBodyA"),     false },
		{ TEXT("SkeletalMeshes/Bot/BotC"),                 TEXT("Animations/Bot/BotA_"),                  TEXT("BotCBodyA"),          false },
		{ TEXT("SkeletalMeshes/Aliens/AlienMaleB"),        TEXT("Animations/Aliens/AlienA_"),             TEXT("AlienMaleBBodyA"),    false },
		{ TEXT("SkeletalMeshes/HumanMaleA/EgyptMaleB"),    TEXT("Animations/HumanMaleA/BipedMaleA_"),     TEXT("EgyptMaleBBodyA"),    false },
		{ TEXT("SkeletalMeshes/Bot/BotD"),                 TEXT("Animations/Bot/BotA_"),                  TEXT("BotDBodyA"),          false },
	};
	return Models;
}

const ArenaArt::FCharacterModel* ArenaArt::PickCharacterModel(int32 PlayerId)
{
	const TArray<FCharacterModel>& Models = GetCharacterModels();
	if (Models.Num() == 0)
	{
		return nullptr;
	}
	// Player ids are small consecutive numbers: start each match at a different point of the pool but keep
	// neighbours distinct.
	static const int32 Offset = FMath::RandRange(0, 1000);
	const int32 Index = (FMath::Abs(PlayerId) + Offset) % Models.Num();
	return &Models[Index];
}

namespace ArenaArtPrivate
{
	/** Player name -> model (pool entries or models built for meshes outside the pool). */
	TMap<FString, const ArenaArt::FCharacterModel*>& CharacterOverrides()
	{
		static TMap<FString, const ArenaArt::FCharacterModel*> Map;
		return Map;
	}

	/** Body skin name -> its UE2 package when that is not PlayerSkins (from the character records' skins). */
	TMap<FString, FString>& SkinPackages()
	{
		static TMap<FString, FString> Map;
		return Map;
	}

	/** A model for a converted mesh outside the pool: its animation set from the cache's mesh record. Strings live in the entry. */
	struct FDynCharacterModel
	{
		FString Mesh, Anim, Skin;
		ArenaArt::FCharacterModel Model;
	};

	const ArenaArt::FCharacterModel* DynamicModel(const FString& Pkg, const FString& Name, const FString& BodySkin)
	{
		static TMap<FString, TUniquePtr<FDynCharacterModel>> Dyn;
		const FString Key = (Pkg + TEXT(".") + Name + TEXT("|") + BodySkin).ToLower();
		if (TUniquePtr<FDynCharacterModel>* Found = Dyn.Find(Key))
		{
			return *Found ? &(*Found)->Model : nullptr;
		}
		TUniquePtr<FDynCharacterModel>& D = Dyn.Add(Key);
		FString Json;
		// through the cache index: record names and cache file names can differ in case (ThunderCrash.MercFemale1 / Mercfemale1)
		const FString File = ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("skeletalmeshes"), Pkg + TEXT(".") + Name, TEXT(".json"));
		if (File.IsEmpty() || !FFileHelper::LoadFileToString(Json, *File))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		FString AnimSet, AnimPkg, AnimName;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid() || !Root->TryGetStringField(TEXT("animation"), AnimSet)
			|| !AnimSet.Split(TEXT("."), &AnimPkg, &AnimName))
		{
			return nullptr;
		}
		D = MakeUnique<FDynCharacterModel>();
		D->Mesh = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Name);
		D->Anim = FString::Printf(TEXT("Animations/%s/%s_"), *AnimPkg, *AnimName);
		D->Skin = BodySkin.IsEmpty() ? Name : BodySkin;
		D->Model = { *D->Mesh, *D->Anim, *D->Skin, AnimName.Contains(TEXT("Female")) || Name.Contains(TEXT("Female")) };
		return &D->Model;
	}
}

const ArenaArt::FCharacterModel* ArenaArt::FindCharacterModelForMesh(const FString& UE2Mesh, const FString& UE2BodySkin)
{
	FString Pkg, Mesh;
	if (!UE2Mesh.Split(TEXT("."), &Pkg, &Mesh))
	{
		return nullptr;
	}
	// the body skin's name ("PlayerSkins.MercMaleDBodyA" -> "MercMaleDBodyA"; its team variants are <name>_0 / _1)
	FString Skin = UE2BodySkin, SkinPkg;
	if (UE2BodySkin.Split(TEXT("."), &SkinPkg, &Skin, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !SkinPkg.Equals(TEXT("PlayerSkins"), ESearchCase::IgnoreCase))
	{
		ArenaArtPrivate::SkinPackages().Add(Skin.ToLower(), SkinPkg);
	}
	const FString Want = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Mesh);
	const FCharacterModel* Best = nullptr;
	for (const FCharacterModel& M : GetCharacterModels())
	{
		if (Want.Equals(M.Mesh, ESearchCase::IgnoreCase) && (!Best || Skin.Equals(M.BodySkin, ESearchCase::IgnoreCase)))
		{
			Best = &M;
		}
	}
	if (Best && !Skin.IsEmpty() && !Skin.Equals(Best->BodySkin, ESearchCase::IgnoreCase))
	{
		// same mesh, another skin (most stock characters): a model entry with that skin
		if (const FCharacterModel* Dyn = ArenaArtPrivate::DynamicModel(Pkg, Mesh, Skin))
		{
			return Dyn;
		}
	}
	// a converted mesh outside the pool (Hellions, Thunder Crash, Skaarj, Xan's robots ...)
	return Best ? Best : ArenaArtPrivate::DynamicModel(Pkg, Mesh, Skin);
}

UTexture* ArenaArt::LoadBodySkin(const FString& SkinName)
{
	if (UTexture* T = Load<UTexture>(TEXT("Textures/PlayerSkins/") + SkinName))
	{
		return T;
	}
	// a team variant: the package of its base skin
	FString Base = SkinName;
	if (Base.Len() > 2 && Base[Base.Len() - 2] == TEXT('_') && FChar::IsDigit(Base[Base.Len() - 1]))
	{
		Base.LeftChopInline(2);
	}
	const FString* Pkg = ArenaArtPrivate::SkinPackages().Find(Base.ToLower());
	return Pkg ? Load<UTexture>(FString::Printf(TEXT("Textures/%s/%s"), **Pkg, *SkinName)) : nullptr;
}

void ArenaArt::ApplyFaceSkin(USkeletalMeshComponent* Comp, const FString& UE2FaceSkin, int32 Team)
{
	const USkeletalMesh* Mesh = Comp ? Comp->GetSkeletalMeshAsset() : nullptr;
	const int32 FaceSlot = 1;
	if (!Mesh || UE2FaceSkin.IsEmpty() || !Mesh->GetMaterials().IsValidIndex(FaceSlot)
		|| !ArenaRuntimeContent::IsDefaultMaterial(Mesh->GetMaterials()[FaceSlot].MaterialInterface.Get()))
	{
		return;
	}
	Comp->SetMaterial(FaceSlot, nullptr); // a previous team's face
	auto InCache = [](const FString& Path)
	{
		return !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("materials"), Path, TEXT(".json")).IsEmpty()
			|| !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("textures"), Path, TEXT(".json")).IsEmpty();
	};
	FString Path = (Team == 0 || Team == 1) ? FString::Printf(TEXT("%s_%d"), *UE2FaceSkin, Team) : FString();
	if (Path.IsEmpty() || !InCache(Path))
	{
		Path = UE2FaceSkin;
	}
	if (InCache(Path))
	{
		if (UMaterialInterface* Mat = ArenaRuntimeContent::LoadMaterialByObjectPath(Path))
		{
			Comp->SetMaterial(FaceSlot, Mat);
		}
	}
}

void ArenaArt::SetCharacterOverride(const FString& PlayerName, const FString& UE2Mesh, const FString& UE2BodySkin)
{
	if (PlayerName.IsEmpty())
	{
		return;
	}
	if (const FCharacterModel* M = FindCharacterModelForMesh(UE2Mesh, UE2BodySkin))
	{
		ArenaArtPrivate::CharacterOverrides().Add(PlayerName.ToLower(), M);
	}
}

void ArenaArt::ClearCharacterOverrides()
{
	ArenaArtPrivate::CharacterOverrides().Reset();
}

const ArenaArt::FCharacterModel* ArenaArt::FindCharacterOverride(const FString& PlayerName)
{
	const ArenaArt::FCharacterModel* const* M = ArenaArtPrivate::CharacterOverrides().Find(PlayerName.ToLower());
	return M ? *M : nullptr;
}

// =================================================================================================
// Weapons
// =================================================================================================

namespace ArenaArtPrivate
{
	/** Source rotation units (65536 per turn, pitch / yaw / roll) to degrees. */
	FRotator SourceRotation(const FIntVector& R)
	{
		const float S = 360.f / 65536.f;
		return FRotator(R.X * S, R.Y * S, R.Z * S);
	}
}

void ArenaArt::FWeaponArt::SetViewFromSource(const FVector& Offset, const FIntVector& Pivot, float DrawScale, float DisplayFOV)
{
	// The source game draws the view model at EyeLocation + Offset * (90 / DisplayFOV), rotated by the view.
	ViewOffset = Offset * (90.f / DisplayFOV) * 2.f;
	ViewRotation = ArenaArtPrivate::SourceRotation(Pivot);
	ViewScale = DrawScale;
	ViewFOV = DisplayFOV;
}

void ArenaArt::FWeaponArt::SetHandFromSource(const FVector& Offset, const FIntVector& Rotation, float DrawScale)
{
	HandOffset = Offset * 2.f;
	HandRotation = ArenaArtPrivate::SourceRotation(Rotation);
	HandScale = DrawScale;
}

namespace ArenaArtPrivate
{
	TArray<ArenaArt::FWeaponArt> BuildWeaponTable()
	{
		using ArenaArt::FWeaponArt;
		TArray<FWeaponArt> T;

		{	// Melee / shield
			FWeaponArt A;
			A.ClassName = TEXT("Weap_Gauntlet");
			A.SetViewFromSource(FVector(2.f, -0.7f, -2.7f), FIntVector(500, 500, 0), 0.4f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/ShieldGun_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/ShieldGun_1st_");
			A.FireAnim[0] = TEXT("Fire");
			A.FireAnim[1] = nullptr;
			A.HoldAnim[0] = TEXT("Charge");
			A.HoldAnim[1] = TEXT("Shield");
			A.MuzzleSocket = TEXT("bp1");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/ShieldGun_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BShieldGunFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BShield1");
			T.Add(A);
		}
		{	// Assault rifle with grenades
			FWeaponArt A;
			A.ClassName = TEXT("Weap_Carbine");
			A.SetViewFromSource(FVector(4.f, 5.5f, -6.f), FIntVector(400, 0, 0), 0.8f, 70.f);
			A.SetHandFromSource(FVector(-20.f, -5.f, 0.f), FIntVector(32768, 0, 0), 0.3f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/AssaultRifle");
			A.AnimPrefix = TEXT("Animations/NewWeapons2004/AssaultRifle_");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NewAssaultRifle_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BAssaultRifleFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BAssaultRifleAltFire");
			A.SoundInterval[0] = 0.14f;
			T.Add(A);
		}
		{	// Goo
			FWeaponArt A;
			A.ClassName = TEXT("Weap_GooLauncher");
			A.SetViewFromSource(FVector(7.f, 3.f, 0.f), FIntVector(0, 0, 0), 1.f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/BioRifle_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/BioRifle_1st_");
			A.HoldAnim[1] = TEXT("HOLD");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/BioRifle_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BBioRifleFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BBioRifleFire");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Shock-like
			FWeaponArt A;
			A.ClassName = TEXT("Weap_PulseRifle");
			A.SetViewFromSource(FVector(4.f, 8.f, -2.f), FIntVector(-1000, 0, 0), 0.7f, 70.f);
			A.SetHandFromSource(FVector(-3.f, -5.f, -10.f), FIntVector(32768, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/ShockRifle");
			A.AnimPrefix = TEXT("Animations/NewWeapons2004/ShockRifleAnims_");
			A.SelectAnim = TEXT("Pickup");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NewShockRifle_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BShockRifleFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BShockRifleAltFire");
			T.Add(A);
		}
		{	// Link-like
			FWeaponArt A;
			A.ClassName = TEXT("Weap_ArcLinker");
			A.SetViewFromSource(FVector(-5.f, -3.f, 0.f), FIntVector(0, 500, 0), 1.f, 60.f);
			A.SetHandFromSource(FVector(15.f, -5.f, -7.f), FIntVector(32768, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/FatLinkGun");
			A.AnimPrefix = TEXT("Animations/NewWeapons2004/LinkGun_");
			A.FireAnim[1] = nullptr;
			A.HoldAnim[1] = TEXT("AltFire");
			A.bLoopHold[1] = true;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NewLinkGun_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/LinkGun_BLinkedFire");
			A.SoundInterval[0] = 0.16f;
			T.Add(A);
		}
		{	// Minigun-like
			FWeaponArt A;
			A.ClassName = TEXT("Weap_RotaryGun");
			A.SetViewFromSource(FVector(2.f, -1.f, 0.f), FIntVector(0, 500, 0), 0.4f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/Minigun_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/Minigun_1st_");
			A.FireAnim[0] = nullptr;
			A.FireAnim[1] = nullptr;
			A.HoldAnim[0] = TEXT("FireLoop");
			A.HoldAnim[1] = TEXT("FireLoop");
			A.bLoopHold[0] = A.bLoopHold[1] = true;
			A.MuzzleSocket = TEXT("Flash");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/Minigun_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BMiniGunFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BMiniGunAltFire");
			A.SoundInterval[0] = 0.9f;
			A.SoundInterval[1] = 0.95f;
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Flak-like
			FWeaponArt A;
			A.ClassName = TEXT("Weap_ShardCannon");
			A.SetViewFromSource(FVector(-7.f, 8.f, 0.f), FIntVector(0, 500, 200), 1.f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/Flak_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/Flak_1st_");
			A.SelectAnim = TEXT("Pickup");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/Flak_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BFlakCannonFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BFlakCannonAltFire");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Rockets
			FWeaponArt A;
			A.ClassName = TEXT("Weap_RocketLauncher");
			A.SetViewFromSource(FVector(0.f, 8.f, 0.f), FIntVector(0, 500, 1000), 1.f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/RocketLauncher_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/RocketLauncher_1st_");
			A.SelectAnim = TEXT("Pickup");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/RocketLauncher_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BRocketLauncherFire");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BRocketLauncherAltFire");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Lightning / sniper
			FWeaponArt A;
			A.ClassName = TEXT("Weap_RailRifle");
			A.SetViewFromSource(FVector(0.f, 2.3f, 0.f), FIntVector(0, 0, 0), 0.4f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/Sniper_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/Sniper_1st_");
			A.SelectAnim = TEXT("Pickup");
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/Sniper_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BLightningGunFire");
			T.Add(A);
		}
		{	// Instagib
			FWeaponArt A;
			A.ClassName = TEXT("Weap_InstaRifle");
			A.SetViewFromSource(FVector(4.f, 8.f, -2.f), FIntVector(-1000, 0, 0), 0.7f, 70.f);
			A.SetHandFromSource(FVector(-3.f, -5.f, -10.f), FIntVector(32768, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/FinalShockRifle");
			A.AnimPrefix = TEXT("Animations/NewWeapons2004/ShockRifleAnims_");
			A.SelectAnim = TEXT("Pickup");
			A.FireAnim[1] = TEXT("Fire");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NewShockRifle_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/Misc_instagib_rifleshot");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/Misc_instagib_rifleshot");
			T.Add(A);
		}
		{	// Redeemer (XWeapons.Redeemer / RedeemerAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_WarheadLauncher");
			A.SetViewFromSource(FVector(14.f, 0.f, -28.f), FIntVector(1000, -400, 0), 1.2f, 60.f);
			A.SetHandFromSource(FVector(-35.f, 0.f, 0.f), FIntVector(0, 49152, 32768), 0.6f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/Redeemer_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/Redeemer_1st_");
			A.SelectAnim = TEXT("Pickup");
			A.FireAnim[1] = TEXT("Fire");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/Redeemer_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/Misc_redeemer_shoot");
			A.FireSound[1] = TEXT("Sounds/WeaponSounds/Misc_redeemer_shoot");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Translocator (XWeapons.TransLauncher / TransAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_Translocator");
			A.SetViewFromSource(FVector(28.5f, 12.f, -12.f), FIntVector(1000, 400, 0), 0.8f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NewTranslauncher_1st");
			A.AnimPrefix = TEXT("Animations/NewWeapons2004/NEWTranslauncherAnims_");
			A.FireAnim[1] = nullptr; // TransRecall's "Recall" is not in the 2004 mesh's animation set
			A.MuzzleSocket = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/NEWTranslauncher_3rd");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// Ion Painter (XWeapons.Painter / PainterAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_OrbitalDesignator");
			A.SetViewFromSource(FVector(25.f, 2.f, -1.f), FIntVector(0, 0, 0), 1.f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/Painter_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/Painter_1st_");
			A.FireAnim[0] = nullptr;
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/Painter_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/TAGRifle_TAGFireB");
			T.Add(A);
		}
		{	// Classic sniper rifle (UTClassic.ClassicSniperRifle / ClassicSniperAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_MarksmanRifle");
			A.SetViewFromSource(FVector(27.7f, 5.3f, -10.6f), FIntVector(0, 16384, 0), 0.48f, 60.f);
			A.SetHandFromSource(FVector(-30.f, 0.f, 4.f), FIntVector(32768, 0, 0), 0.16f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/SniperAnims/SniperRifle_1st");
			A.AnimPrefix = TEXT("Animations/SniperAnims/SniperRifleAnims_");
			A.SelectAnim = TEXT("Pickup");
			A.FireAnim[0] = TEXT("Fire1");
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/NewWeapons2004/Sniper3rd");
			A.FireSound[0] = TEXT("Sounds/NewWeaponSounds/NewSniperShot");
			T.Add(A);
		}
		{	// AVRiL (Onslaught.ONSAVRiL / ONSAVRiLAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_SeekerLauncher");
			A.SetViewFromSource(FVector(100.f, 35.5f, -32.5f), FIntVector(0, 0, 0), 1.f, 45.f);
			A.SetHandFromSource(FVector(35.f, 15.f, 0.f), FIntVector(0, 0, 0), 0.28f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/AVRiL_1st");
			A.AnimPrefix = TEXT("Animations/ONSWeapons-A/AVRiLanims_");
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/AVRiL_3rd");
			A.FireSound[0] = TEXT("Sounds/ONSVehicleSounds-S/AVRiL_AvrilFire01");
			A.bHeavy = true;
			T.Add(A);
		}
		{	// ONS grenade launcher (Onslaught.ONSGrenadeLauncher / ONSGrenadeAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_StickyLauncher");
			A.SetViewFromSource(FVector(150.f, 40.f, -46.f), FIntVector(0, 0, 0), 1.f, 45.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.18f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/GrenadeLauncher_1st");
			A.AnimPrefix = TEXT("Animations/ONSWeapons-A/GrenadeLauncherAnims_");
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/GrenadeLauncher_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BBioRifleFire");
			T.Add(A);
		}
		{	// ONS mine layer (Onslaught.ONSMineLayer / ONSMineLayerAttachment)
			FWeaponArt A;
			A.ClassName = TEXT("Weap_CrawlerLayer");
			A.SetViewFromSource(FVector(100.f, 35.5f, -32.5f), FIntVector(0, 0, 0), 1.f, 45.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.28f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/MineLayer_1st");
			A.AnimPrefix = TEXT("Animations/ONSWeapons-A/MineLayerAnims_");
			A.FireAnim[1] = nullptr;
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/ONSWeapons-A/MineLayer_3rd");
			A.FireSound[0] = TEXT("Sounds/ONSVehicleSounds-S/SpiderMines_SpiderMineFire01");
			T.Add(A);
		}
		{	// Bombing Run ball launcher
			FWeaponArt A;
			A.ClassName = TEXT("ArenaBR_BallLauncher");
			A.SetViewFromSource(FVector(11.f, 0.f, 0.f), FIntVector(0, 0, 0), 0.4f, 60.f);
			A.SetHandFromSource(FVector::ZeroVector, FIntVector(0, 0, 0), 0.4f);
			A.FirstPersonMesh = TEXT("SkeletalMeshes/Weapons/BallLauncher_1st");
			A.AnimPrefix = TEXT("Animations/Weapons/BallLauncher_1st_");
			A.FireAnim[1] = TEXT("Fire");
			A.ThirdPersonMesh = TEXT("SkeletalMeshes/Weapons/BallLauncher_3rd");
			A.FireSound[0] = TEXT("Sounds/WeaponSounds/Misc_ballgun_launch");
			T.Add(A);
		}
		return T;
	}
}

const TArray<ArenaArt::FWeaponArt>& ArenaArt::GetWeaponArtTable()
{
	static const TArray<FWeaponArt> Table = ArenaArtPrivate::BuildWeaponTable();
	return Table;
}

const ArenaArt::FWeaponArt* ArenaArt::FindWeaponArt(const UClass* WeaponClass)
{
	for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
	{
		const FString Name = C->GetName();
		for (const FWeaponArt& Art : GetWeaponArtTable())
		{
			if (Name == Art.ClassName)
			{
				return &Art;
			}
		}
	}
	return nullptr;
}

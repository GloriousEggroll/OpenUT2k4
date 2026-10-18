#include "ArenaArtItems.h"
#include "ArenaArt.h"
#include "ArenaPickup.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"
#include "OpenUT2k4.h"

// Asset paths are relative to /Game/Replacements or /Game/Imported (UT2004 package names, groups joined with '_').
// Sizes are the largest dimension in cm; the meshes are fitted by their bounds, so differently sized replacements
// keep the same footprint.

namespace ArenaArtItemsPrivate
{
	using namespace ArenaArtItems;

	FItemMesh Item(const TCHAR* Mesh, float Size, const TCHAR* Fallback = nullptr, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		FItemMesh M;
		M.Mesh = Mesh;
		M.Fallback = Fallback;
		M.Size = Size;
		M.Rotation = Rotation;
		return M;
	}

	FItemMesh Base(const TCHAR* Mesh, float Size)
	{
		FItemMesh M = Item(Mesh, Size);
		M.bBottom = true;
		return M;
	}

	const TCHAR* const WeaponBase = TEXT("StaticMeshes/2K4chargerMESHES/ChargerMeshes_WeaponChargerMesh-DS");
	const TCHAR* const HealthBase = TEXT("StaticMeshes/2K4chargerMESHES/ChargerMeshes_HealthChargerMESH-DS");
	const TCHAR* const ShieldBase = TEXT("StaticMeshes/XGame_rc/ShieldChargerMesh");
	const TCHAR* const PowerBase = TEXT("StaticMeshes/XGame_rc/WildcardChargerMesh");

	struct FWeaponPickupArt
	{
		const TCHAR* ClassName;
		FPickupArt Weapon;
		FPickupArt Ammo;
	};

	FWeaponPickupArt WeaponEntry(const TCHAR* ClassName, const TCHAR* Mesh, const TCHAR* Fallback, float Size, const TCHAR* AmmoMesh, float AmmoSize)
	{
		FWeaponPickupArt W;
		W.ClassName = ClassName;
		W.Weapon.Item = Item(Mesh, Size, Fallback);
		W.Weapon.Base = Base(WeaponBase, 150.f);
		W.Weapon.bBaseByDefault = true;
		if (AmmoMesh)
		{
			W.Ammo.Item = Item(AmmoMesh, AmmoSize);
		}
		return W;
	}

	const TArray<FWeaponPickupArt>& WeaponTable()
	{
		// Pickup meshes match the first / third-person art in ArenaArt.cpp (2004 models for the carbine,
		// pulse rifle and arc linker, the classic ones for the rest).
		static const TArray<FWeaponPickupArt> Table = {
			WeaponEntry(TEXT("Weap_Gauntlet"), TEXT("StaticMeshes/WeaponStaticMesh/ShieldGunPickup"), nullptr, 100.f, nullptr, 0.f),
			WeaponEntry(TEXT("Weap_Carbine"), TEXT("StaticMeshes/NewWeaponPickups/AssaultPickupSM"), TEXT("StaticMeshes/WeaponStaticMesh/AssaultRiflePickup"), 105.f,
				TEXT("StaticMeshes/WeaponStaticMesh/AssaultAmmoPickup"), 40.f),
			WeaponEntry(TEXT("Weap_GooLauncher"), TEXT("StaticMeshes/WeaponStaticMesh/BioRiflePickup"), nullptr, 95.f,
				TEXT("StaticMeshes/WeaponStaticMesh/BioAmmoPickup"), 42.f),
			WeaponEntry(TEXT("Weap_PulseRifle"), TEXT("StaticMeshes/NewWeaponPickups/ShockPickupSM"), TEXT("StaticMeshes/WeaponStaticMesh/ShockRiflePickup"), 120.f,
				TEXT("StaticMeshes/WeaponStaticMesh/ShockAmmoPickup"), 48.f),
			WeaponEntry(TEXT("Weap_ArcLinker"), TEXT("StaticMeshes/NewWeaponPickups/LinkPickupSM"), TEXT("StaticMeshes/WeaponStaticMesh/LinkGunPickup"), 100.f,
				TEXT("StaticMeshes/WeaponStaticMesh/LinkAmmoPickup"), 40.f),
			WeaponEntry(TEXT("Weap_RotaryGun"), TEXT("StaticMeshes/WeaponStaticMesh/MinigunPickup"), nullptr, 135.f,
				TEXT("StaticMeshes/WeaponStaticMesh/MinigunAmmoPickup"), 52.f),
			WeaponEntry(TEXT("Weap_ShardCannon"), TEXT("StaticMeshes/WeaponStaticMesh/FlakCannonPickup"), nullptr, 115.f,
				TEXT("StaticMeshes/WeaponStaticMesh/FlakAmmoPickup"), 46.f),
			WeaponEntry(TEXT("Weap_RocketLauncher"), TEXT("StaticMeshes/WeaponStaticMesh/RocketLauncherPickup"), nullptr, 125.f,
				TEXT("StaticMeshes/WeaponStaticMesh/RocketAmmoPickup"), 46.f),
			WeaponEntry(TEXT("Weap_RailRifle"), TEXT("StaticMeshes/WeaponStaticMesh/SniperRiflePickup"), nullptr, 135.f,
				TEXT("StaticMeshes/WeaponStaticMesh/SniperAmmoPickup"), 42.f),
			WeaponEntry(TEXT("Weap_InstaRifle"), TEXT("StaticMeshes/NewWeaponPickups/ShockPickupSM"), TEXT("StaticMeshes/WeaponStaticMesh/ShockRiflePickup"), 120.f,
				nullptr, 0.f),
			// pickup meshes of the UT2004 super / Onslaught / classic weapons (their Pickup and ammo classes)
			WeaponEntry(TEXT("Weap_WarheadLauncher"), TEXT("StaticMeshes/WeaponStaticMesh/RedeemerPickup"), nullptr, 140.f, nullptr, 0.f),
			WeaponEntry(TEXT("Weap_OrbitalDesignator"), TEXT("StaticMeshes/WeaponStaticMesh/PainterPickup"), nullptr, 110.f, nullptr, 0.f),
			WeaponEntry(TEXT("Weap_MarksmanRifle"), TEXT("StaticMeshes/NewWeaponStatic/newsniperpickup"), TEXT("StaticMeshes/WeaponStaticMesh/SniperRiflePickup"), 135.f,
				TEXT("StaticMeshes/NewWeaponStatic/ClassicSniperAmmoM"), 42.f),
			WeaponEntry(TEXT("Weap_SeekerLauncher"), TEXT("StaticMeshes/VMWeaponsSM/PlayerWeaponsGroup_AVRiLsm"), nullptr, 130.f,
				TEXT("StaticMeshes/ONSWeapons-SM/AVRILammo"), 46.f),
			WeaponEntry(TEXT("Weap_StickyLauncher"), TEXT("StaticMeshes/ONSWeapons-SM/GrenadeLauncherPickup"), nullptr, 110.f,
				TEXT("StaticMeshes/ONSWeapons-SM/GrenadeLauncherAmmo"), 40.f),
			WeaponEntry(TEXT("Weap_CrawlerLayer"), TEXT("StaticMeshes/ONSWeapons-SM/MineLayerPickup"), nullptr, 110.f,
				TEXT("StaticMeshes/ONSWeapons-SM/MineLayerAmmo"), 40.f),
			// TransPickup: NewWeaponPickups.TranslocatorCENTER at DrawScale 0.2
			WeaponEntry(TEXT("Weap_Translocator"), TEXT("StaticMeshes/NewWeaponPickups/TranslocatorCENTER"), nullptr, 98.f, nullptr, 0.f),
		};
		return Table;
	}

	FPickupArt ItemArt(const TCHAR* Mesh, float Size, const TCHAR* Fallback, const TCHAR* BaseMesh, float BaseSize, bool bBaseByDefault)
	{
		FPickupArt A;
		A.Item = Item(Mesh, Size, Fallback);
		if (BaseMesh)
		{
			A.Base = Base(BaseMesh, BaseSize);
		}
		A.bBaseByDefault = bBaseByDefault;
		return A;
	}

	const FPickupArt* ItemTable(EArenaPickupType Type)
	{
		static const FPickupArt Vial = ItemArt(TEXT("StaticMeshes/XPickups_rc/MiniHealthPack"), 46.f, nullptr, nullptr, 0.f, false);
		static const FPickupArt Health = ItemArt(TEXT("StaticMeshes/E_Pickups/Health_MidHealth"), 62.f, TEXT("StaticMeshes/XPickups_rc/HealthPack"), HealthBase, 170.f, false);
		static const FPickupArt Keg = ItemArt(TEXT("StaticMeshes/E_Pickups/Health_SuperKeg"), 95.f, nullptr, HealthBase, 170.f, true);
		static const FPickupArt ShieldSmall = ItemArt(TEXT("StaticMeshes/E_Pickups/General_RegShield"), 62.f, TEXT("StaticMeshes/XPickups_rc/ShieldPack"), ShieldBase, 135.f, false);
		static const FPickupArt ShieldLarge = ItemArt(TEXT("StaticMeshes/E_Pickups/General_SuperShield"), 105.f, TEXT("StaticMeshes/XPickups_rc/SuperShieldPack"), ShieldBase, 135.f, true);
		static const FPickupArt Damage = ItemArt(TEXT("StaticMeshes/E_Pickups/General_Udamage"), 80.f, TEXT("StaticMeshes/XPickups_rc/UDamagePack"), PowerBase, 140.f, true);
		static const FPickupArt Adrenaline = ItemArt(TEXT("StaticMeshes/XPickups_rc/AdrenalinePack"), 44.f, nullptr, nullptr, 0.f, false);
		switch (Type)
		{
		case EArenaPickupType::HealthVial:   return &Vial;
		case EArenaPickupType::HealthPack:   return &Health;
		case EArenaPickupType::SuperHealth:  return &Keg;
		case EArenaPickupType::ShieldSmall:  return &ShieldSmall;
		case EArenaPickupType::ShieldLarge:  return &ShieldLarge;
		case EArenaPickupType::DoubleDamage: return &Damage;
		case EArenaPickupType::Adrenaline:   return &Adrenaline;
		default: return nullptr;
		}
	}
}

const ArenaArtItems::FPickupArt* ArenaArtItems::FindPickupArt(EArenaPickupType Type, const UClass* WeaponClass)
{
	using namespace ArenaArtItemsPrivate;
	if (!ArenaArt::IsEnabled())
	{
		return nullptr;
	}
	if (Type != EArenaPickupType::Weapon && Type != EArenaPickupType::Ammo)
	{
		return ItemTable(Type);
	}
	for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
	{
		const FString Name = C->GetName();
		for (const FWeaponPickupArt& W : WeaponTable())
		{
			if (Name == W.ClassName)
			{
				const FPickupArt& A = Type == EArenaPickupType::Weapon ? W.Weapon : W.Ammo;
				return A.Item.Mesh ? &A : nullptr;
			}
		}
	}
	return nullptr;
}

UStaticMesh* ArenaArtItems::LoadMesh(const FItemMesh& M)
{
	UStaticMesh* Mesh = M.Mesh ? ArenaArt::Load<UStaticMesh>(M.Mesh) : nullptr;
	if (!Mesh && M.Fallback)
	{
		Mesh = ArenaArt::Load<UStaticMesh>(M.Fallback);
	}
	return Mesh;
}

UMaterialInterface* ArenaArtItems::LoadMaterial(const TCHAR* Path)
{
	return Path ? ArenaArt::Load<UMaterialInterface>(Path) : nullptr;
}

void ArenaArtItems::FitItemMesh(UStaticMeshComponent* Comp, const FItemMesh& M, float ScaleMul)
{
	const UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
	if (!Mesh)
	{
		return;
	}
	const FBoxSphereBounds B = Mesh->GetBounds();
	const float MaxDim = FMath::Max(1.f, float(B.BoxExtent.GetMax()) * 2.f);
	const float S = M.Size * ScaleMul / MaxDim;
	FVector Pivot = FVector(B.Origin);
	if (M.bBottom)
	{
		// Lowest point of the rotated bounds on the origin, centred in XY.
		const FBox Box = FBox(B.Origin - B.BoxExtent, B.Origin + B.BoxExtent).TransformBy(FTransform(M.Rotation));
		const FVector RotatedCentre = M.Rotation.RotateVector(FVector(B.Origin));
		Pivot = M.Rotation.UnrotateVector(FVector(RotatedCentre.X, RotatedCentre.Y, Box.Min.Z));
	}
	Comp->SetRelativeScale3D(FVector(S));
	Comp->SetRelativeRotation(M.Rotation);
	Comp->SetRelativeLocation(M.Offset * ScaleMul - M.Rotation.RotateVector(Pivot * S));
}

UStaticMeshComponent* ArenaArtItems::AddItemMesh(AActor* Owner, USceneComponent* Parent, const FItemMesh& M, float ScaleMul, bool bShadow)
{
	UStaticMesh* Mesh = LoadMesh(M);
	if (!Owner || !Parent || !Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
	C->SetMobility(EComponentMobility::Movable);
	C->SetStaticMesh(Mesh);
	C->SetupAttachment(Parent);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	C->SetGenerateOverlapEvents(false);
	C->SetCanEverAffectNavigation(false);
	C->SetCastShadow(bShadow);
	FitItemMesh(C, M, ScaleMul);
	C->RegisterComponent();
	return C;
}

// =================================================================================================
// Game objects
// =================================================================================================

const ArenaArtItems::FFlagArt& ArenaArtItems::GetFlagArt()
{
	static const FFlagArt Art = []
	{
		FFlagArt A;
		A.Mesh = TEXT("SkeletalMeshes/XGame_rc/FlagMesh");
		A.Anim = TEXT("Animations/XGame_rc/FlagMesh_flag");
		A.TeamMaterial[0] = TEXT("Materials/XGameShaders2004/CTFShaders_RedFlagShader");
		A.TeamMaterial[1] = TEXT("Materials/XGameShaders2004/CTFShaders_BlueFlagShader");
		A.TeamMaterialFallback[0] = TEXT("Materials/XGameShaders/RedFlagShader");
		A.TeamMaterialFallback[1] = TEXT("Materials/XGameShaders/BlueFlagShader");
		A.Height = 205.f;
		A.Base = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/XGame_rc/FlagBaseMesh"), 280.f);
		A.BaseMaterial[0] = TEXT("Materials/XGameTextures/FlagBaseTexR");
		A.BaseMaterial[1] = TEXT("Materials/XGameTextures/FlagBaseTexB");
		A.Symbol[0] = TEXT("Textures/TeamSymbols_UT2004/Team_phoenix");
		A.Symbol[1] = TEXT("Textures/TeamSymbols_UT2004/Team_RAPTOR");
		// XGameShaders.TexScaler0/1: UScale = VScale = 0.5, UOffset -110, VOffset -40 (of a 256 texel symbol)
		A.SymbolScale = 2.f;
		A.SymbolOffset = FVector2f(-110.f / 256.f, -40.f / 256.f);
		return A;
	}();
	return Art;
}

const TArray<FString>& ArenaArtItems::GetTeamSymbolList()
{
	// UT2004's list without the single-player entries, in its order (TeamSymbols_UT2003.int, then TeamSymbols_UT2004.int).
	// Names as the converter writes them (the texture's group is part of the asset name).
	static const TArray<FString> List = []
	{
		TArray<FString> L;
		const TCHAR* const UT2003[] = {
			TEXT("Apocalypse"), TEXT("BlackLegion"), TEXT("BloodFists"), TEXT("BoneCrushers"), TEXT("ch"), TEXT("ChildrenoftheSun"),
			TEXT("ColdSteel"), TEXT("Crusaders"), TEXT("DarkProvince"), TEXT("Demolition"), TEXT("DragonBreath"), TEXT("DuneRiders"),
			TEXT("FireStorm"), TEXT("GearHeads"), TEXT("IronGuard"), TEXT("IronSkull"), TEXT("LordsOfPain"), TEXT("NightStalkers"),
			TEXT("PainMachine"), TEXT("SunBlades"), TEXT("SuperNova"),
			TEXT("sym01"), TEXT("sym02"), TEXT("sym03"), TEXT("sym04"), TEXT("sym05"), TEXT("sym06"), TEXT("sym07"), TEXT("sym08"),
			TEXT("sym09"), TEXT("sym10"), TEXT("sym11"), TEXT("sym12"), TEXT("sym13"), TEXT("sym14"), TEXT("sym15"), TEXT("sym16"),
			TEXT("sym17"), TEXT("sym18"), TEXT("sym19"),
			TEXT("Turtle"), TEXT("Venom"), TEXT("Warcry"), TEXT("WrathofGod") };
		const TCHAR* const UT2004[] = {
			TEXT("design1"), TEXT("design2"), TEXT("design3"), TEXT("design4"), TEXT("design5"), TEXT("design6"),
			TEXT("HELLBENDER"), TEXT("LEVIATHAN"), TEXT("Manta"), TEXT("phoenix"), TEXT("RAPTOR"), TEXT("SCORPION"), TEXT("Spider") };
		for (const TCHAR* N : UT2003)
		{
			L.Add(FString(TEXT("Textures/TeamSymbols_UT2003/Teams_")) + N);
		}
		for (const TCHAR* N : UT2004)
		{
			L.Add(FString(TEXT("Textures/TeamSymbols_UT2004/Team_")) + N);
		}
		return L;
	}();
	return List;
}

FString ArenaArtItems::ResolveTeamSymbol(const FString& InValue)
{
	FString Value = InValue.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return FString();
	}
	// a converted texture path outside the random list (single player team symbols such as TeamSymbols_UT2004.Corrupt)
	if (Value.StartsWith(TEXT("Textures/"), ESearchCase::IgnoreCase) && !GetTeamSymbolList().Contains(Value))
	{
		return Value;
	}
	// "Pkg.Group.Name" / "Pkg.Name" / "Textures/Pkg/Group_Name" / "Name": compare the symbol name and, if given, the package.
	Value.ReplaceInline(TEXT("/"), TEXT("."));
	TArray<FString> Parts;
	Value.ParseIntoArray(Parts, TEXT("."));
	if (Parts.Num() == 0)
	{
		return FString();
	}
	FString Name = Parts.Last();
	FString Package = Parts.Num() >= 2 ? Parts[0] : FString();
	if (Package.Equals(TEXT("Textures"), ESearchCase::IgnoreCase) && Parts.Num() >= 3)
	{
		Package = Parts[1];
	}
	Name.RemoveFromStart(TEXT("Teams_"), ESearchCase::IgnoreCase);
	Name.RemoveFromStart(TEXT("Team_"), ESearchCase::IgnoreCase);
	for (const FString& Entry : GetTeamSymbolList())
	{
		const FString EntryName = Entry.RightChop(Entry.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
		const bool bPackageOk = Package.IsEmpty() || Entry.Contains(TEXT("/") + Package + TEXT("/"), ESearchCase::IgnoreCase);
		if (bPackageOk && EntryName.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Entry;
		}
	}
	return FString();
}

void ArenaArtItems::PickTeamSymbols(const FString& Options, FString OutSymbols[2])
{
	const TArray<FString>& List = GetTeamSymbolList();
	const TCHAR* const Keys[2] = { TEXT("RedTeamSymbol"), TEXT("BlueTeamSymbol") };
	for (int32 Team = 0; Team < 2; ++Team)
	{
		const FString Value = UGameplayStatics::ParseOption(Options, Keys[Team]);
		OutSymbols[Team] = ResolveTeamSymbol(Value);
		if (!Value.IsEmpty() && OutSymbols[Team].IsEmpty())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("%s=%s is not a known team symbol, picking one at random"), Keys[Team], *Value);
		}
	}
	// TeamGame.GetRandomTeamSymbol(base): base + Rand(Length - base), red base 1, blue base 10. UT2004 could give both
	// teams the same symbol (indices 10 and up); here the second pick skips the other team's symbol.
	auto Random = [&List](int32 Base, const FString& Avoid)
	{
		const int32 Num = List.Num();
		for (int32 Try = 0; Try < 16; ++Try)
		{
			const int32 Raw = FMath::RandRange(0, FMath::Max(0, Num - Base - 1));
			const int32 Index = Base + Raw < Num ? Base + Raw : Raw;
			if (List[Index] != Avoid)
			{
				return List[Index];
			}
		}
		return List[(List.IndexOfByKey(Avoid) + 1) % Num];
	};
	if (OutSymbols[0].IsEmpty())
	{
		OutSymbols[0] = Random(1, OutSymbols[1]);
	}
	if (OutSymbols[1].IsEmpty() || OutSymbols[1] == OutSymbols[0])
	{
		OutSymbols[1] = Random(10, OutSymbols[0]);
	}
}

const ArenaArtItems::FBombingRunArt& ArenaArtItems::GetBombingRunArt()
{
	static const FBombingRunArt Art = []
	{
		FBombingRunArt A;
		A.Ball = ArenaArtItemsPrivate::Item(TEXT("StaticMeshes/XGame_rc/BallMesh"), 44.f, TEXT("StaticMeshes/E_Pickups/BombBall_FullBomb"));
		// The hoop mesh lies in its XZ plane; our hoop faces the actor's X, so turn it a quarter.
		A.Goal = ArenaArtItemsPrivate::Item(TEXT("StaticMeshes/XGame_rc/BombDeliveryMesh"), 0.f, nullptr, FRotator(0.f, 90.f, 0.f));
		A.GoalPulseMaterial[0] = TEXT("Materials/XGameTextures/SuperPickups_BombgatePulseRC");
		A.GoalPulseMaterial[1] = TEXT("Materials/XGameTextures/SuperPickups_BombgatePulseBC");
		A.GoalWidthPerRadius = 4.4f;
		A.SpawnPad = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/XGame_rc/BombSpawnMesh"), 240.f);
		return A;
	}();
	return Art;
}

const ArenaArtItems::FDominationArt& ArenaArtItems::GetDominationArt()
{
	static const FDominationArt Art = []
	{
		FDominationArt A;
		A.Pad = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/XGame_rc/DominationPointMesh"), 380.f);
		A.Letter[0] = ArenaArtItemsPrivate::Item(TEXT("StaticMeshes/XGame_rc/DomAMesh"), 150.f, TEXT("StaticMeshes/XGame_StaticMeshes/DOMpointA"));
		A.Letter[1] = ArenaArtItemsPrivate::Item(TEXT("StaticMeshes/XGame_rc/DomBMesh"), 150.f, TEXT("StaticMeshes/XGame_StaticMeshes/DOMpointB"));
		A.Ring = ArenaArtItemsPrivate::Item(TEXT("StaticMeshes/XGame_rc/DomRing"), 230.f, TEXT("StaticMeshes/XGame_StaticMeshes/DOMpoint"));
		A.LetterMaterial[0] = TEXT("Materials/XGameTextures/SuperPickups_DOMabRs");
		A.LetterMaterial[1] = TEXT("Materials/XGameTextures/SuperPickups_DOMabBs");
		A.LetterMaterial[2] = TEXT("Materials/XGameTextures/SuperPickups_DOMabGs");
		A.RingMaterial[0] = TEXT("Materials/XGameTextures/SuperPickups_DOMRedS");
		A.RingMaterial[1] = TEXT("Materials/XGameTextures/SuperPickups_DOMBlueS");
		A.RingMaterial[2] = TEXT("Materials/XGameTextures/SuperPickups_DOMGreyS");
		return A;
	}();
	return Art;
}

const ArenaArtItems::FOnslaughtArt& ArenaArtItems::GetOnslaughtArt()
{
	static const FOnslaughtArt Art = []
	{
		FOnslaughtArt A;
		A.NodeBase = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/VMStructures/CoreGroup_powerNodeBaseSM"), 470.f);
		A.NodeFrame = TEXT("SkeletalMeshes/ONSWeapons-A/PowerNode");
		A.NodeFrameHeight = 400.f;
		A.NodeFrameMaterials[0][0] = TEXT("Materials/ONSstructureTextures/CoreGroup_powerNodeUpperFLAREredFinal");
		A.NodeFrameMaterials[0][1] = TEXT("Materials/ONSstructureTextures/CoreBreachGroup_powerNodeBeamREDfb");
		A.NodeFrameMaterials[0][2] = TEXT("Materials/ONSstructureTextures/CoreBreachGroup_PowerNodeREDfb");
		A.NodeFrameMaterials[1][0] = TEXT("Materials/ONSstructureTextures/CoreGroup_powerNodeUpperFLAREblueFinal");
		A.NodeFrameMaterials[1][1] = TEXT("Materials/ONSstructureTextures/CoreBreachGroup_PowerNodeBeamBLUEfb");
		A.NodeFrameMaterials[1][2] = TEXT("Materials/ONSstructureTextures/CoreBreachGroup_PowerNodeBlueFB");
		// The core and its shield at the source game's draw scale 1 (the node is 2/3 of its source size to keep
		// our pad radius).
		A.CoreBase = ArenaArtItems::FItemMesh();
		A.Core = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/VMStructures/CoreGroup_CoreDivided"), 810.f);
		A.CoreShield = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/VMStructures/CoreGroup_CoreShieldSM"), 990.f);
		A.NodeShield = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/VMStructures/CoreGroup_CoreShieldSM"), 600.f);
		A.CoreEnergy = ArenaArtItemsPrivate::Base(TEXT("StaticMeshes/VMStructures/CoreGroup_COREenergy"), 1300.f);
		A.Flare = TEXT("Textures/EpicParticles/Flares_SoftFlare");
		A.LinkBeam = TEXT("Textures/AW-2004Particles/Energy_PowerBeam");
		A.LinkBolt = TEXT("Textures/AW-2004Particles/Energy_BeamBolt1a");
		A.CoreShieldMaterial[0] = TEXT("Materials/ONSstructureTextures/CoreGroup_CoreEnergyShaderRed");
		A.CoreShieldMaterial[1] = TEXT("Materials/ONSstructureTextures/CoreEnergyShaderBlue");
		return A;
	}();
	return Art;
}

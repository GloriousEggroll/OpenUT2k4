#include "ArenaWeaponHand.h"
#include "ArenaBotController.h"
#include "ArenaSPData.h"
#include "ArenaUserSettings.h"
#include "GameFramework/PlayerController.h"

float ArenaWeaponHand::GetHandedness(const AController* Controller)
{
	if (const APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (PC->IsLocalController())
		{
			// Settings: 0 left, 1 center, 2 right, 3 hidden -> Handedness -1 .. 2
			return float(FMath::Clamp(UArenaUserSettings::Get()->WeaponHand, 0, 3) - 1);
		}
		return 1.f;
	}
	return Cast<AArenaBotController>(Controller) ? 0.f : 1.f;
}

bool ArenaWeaponHand::UseSmallWeapons(const AController* Controller)
{
	const APlayerController* PC = Cast<APlayerController>(Controller);
	return PC && PC->IsLocalController() && UArenaUserSettings::Get()->bSmallWeapons;
}

FString ArenaWeaponHand::UE2WeaponClass(const UClass* WeaponClass)
{
	struct FMap { const TCHAR* Ours; const TCHAR* UE2; };
	static const FMap Map[] = {
		{ TEXT("Weap_Gauntlet"), TEXT("XWeapons.ShieldGun") }, { TEXT("Weap_Carbine"), TEXT("XWeapons.AssaultRifle") },
		{ TEXT("Weap_GooLauncher"), TEXT("XWeapons.BioRifle") }, { TEXT("Weap_PulseRifle"), TEXT("XWeapons.ShockRifle") },
		{ TEXT("Weap_ArcLinker"), TEXT("XWeapons.LinkGun") }, { TEXT("Weap_RotaryGun"), TEXT("XWeapons.Minigun") },
		{ TEXT("Weap_ShardCannon"), TEXT("XWeapons.FlakCannon") }, { TEXT("Weap_RocketLauncher"), TEXT("XWeapons.RocketLauncher") },
		{ TEXT("Weap_RailRifle"), TEXT("XWeapons.SniperRifle") }, { TEXT("Weap_InstaRifle"), TEXT("XWeapons.SuperShockRifle") },
		{ TEXT("Weap_WarheadLauncher"), TEXT("XWeapons.Redeemer") }, { TEXT("Weap_OrbitalDesignator"), TEXT("XWeapons.Painter") },
		{ TEXT("Weap_MarksmanRifle"), TEXT("UTClassic.ClassicSniperRifle") }, { TEXT("Weap_SeekerLauncher"), TEXT("Onslaught.ONSAVRiL") },
		{ TEXT("Weap_StickyLauncher"), TEXT("Onslaught.ONSGrenadeLauncher") }, { TEXT("Weap_CrawlerLayer"), TEXT("Onslaught.ONSMineLayer") },
		{ TEXT("Weap_Translocator"), TEXT("XWeapons.TransLauncher") },
	};
	for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
	{
		const FString Name = C->GetName();
		for (const FMap& M : Map)
		{
			if (Name == M.Ours)
			{
				return M.UE2;
			}
		}
	}
	return FString();
}

namespace ArenaWeaponHandLocal
{
	FVector ParseVector(const FString& S, const FVector& Default)
	{
		if (S.IsEmpty())
		{
			return Default;
		}
		return FVector(FCString::Atof(*ArenaSP::StructField(S, TEXT("X"))), FCString::Atof(*ArenaSP::StructField(S, TEXT("Y"))),
			FCString::Atof(*ArenaSP::StructField(S, TEXT("Z"))));
	}
}

const ArenaWeaponHand::FViewParams& ArenaWeaponHand::GetViewParams(const UClass* WeaponClass)
{
	static TMap<const UClass*, FViewParams> Cache;
	if (const FViewParams* Hit = Cache.Find(WeaponClass))
	{
		return *Hit;
	}
	FViewParams P;
	const FString Cls = UE2WeaponClass(WeaponClass);
	if (!Cls.IsEmpty())
	{
		using namespace ArenaWeaponHandLocal;
		const FString Offset = ArenaSP::GetStockValue(Cls, TEXT("PlayerViewOffset"));
		if (!Offset.IsEmpty())
		{
			P.bValid = true;
			P.Offset = ParseVector(Offset, FVector::ZeroVector);
			const FVector Small = ParseVector(ArenaSP::GetStockValue(Cls, TEXT("SmallViewOffset")), FVector::ZeroVector);
			// Weapon.PostBeginPlay: an unset SmallViewOffset is the PlayerViewOffset
			P.SmallOffset = Small.IsZero() ? P.Offset : Small;
			P.CenteredOffsetY = FCString::Atof(*ArenaSP::GetStockValue(Cls, TEXT("CenteredOffsetY")));
			P.CenteredRoll = FCString::Atoi(*ArenaSP::GetStockValue(Cls, TEXT("CenteredRoll")));
			P.CenteredYaw = FCString::Atoi(*ArenaSP::GetStockValue(Cls, TEXT("CenteredYaw")));
			const float Fov = FCString::Atof(*ArenaSP::GetStockValue(Cls, TEXT("DisplayFOV")));
			P.DisplayFOV = Fov > 1.f ? Fov : 60.f;
		}
	}
	return Cache.Add(WeaponClass, P);
}

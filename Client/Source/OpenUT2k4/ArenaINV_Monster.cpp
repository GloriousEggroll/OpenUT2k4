#include "ArenaINV_Monster.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaAssets.h"
#include "ArenaFX.h"
#include "ArenaRagdoll.h"
#include "ArenaRuntimeContent.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/CommandLine.h"

// =============================================================================================
// Roster data
// =============================================================================================
//
// Values are UT2004's SkaarjPack class defaults (Health, GroundSpeed / AirSpeed, CollisionRadius / CollisionHeight,
// MeleeRange, ScoringValue, Mesh, Skins, sounds, projectile Speed / Damage / DamageRadius) converted to our units:
// 1 UT2004 unit = 2 cm (as the converted meshes), radii scaled like the player's (25 -> 34 cm). Values the class
// defaults leave to its parents use Monster / xPawn's (Health 100, GroundSpeed 440, 25 x 44 cylinder). Melee damage
// and the few projectile values set in script code are close approximations.

namespace ArenaINV
{
	constexpr float UnitCm = 2.f;
	constexpr float RadiusCm = 34.f / 25.f;

	static FArenaINVMonsterDef MakeDef(const TCHAR* Name, EArenaINVBehavior Behavior, float Health, float UTRadius, float UTHeight, float UTSpeed, int32 Points,
		bool bFlying, bool bBoss, float UTMeleeRange, float MeleeDmg, float MeleeInt, const TCHAR* MeleeName)
	{
		FArenaINVMonsterDef D;
		D.Name = Name;
		D.Behavior = Behavior;
		D.Health = Health;
		D.CapsuleRadius = UTRadius * RadiusCm;
		D.CapsuleHalfHeight = UTHeight * UnitCm;
		D.Speed = UTSpeed * UnitCm;
		D.Points = Points;
		D.bFlying = bFlying;
		D.bBoss = bBoss;
		// UT2004's MeleeRange is measured from the collision cylinder: the same here.
		D.MeleeReach = UTMeleeRange * UnitCm * 0.6f;
		D.MeleeDamage = MeleeDmg;
		D.MeleeInterval = MeleeInt;
		D.MeleeName = MeleeName;
		return D;
	}

	static FArenaINVShot MakeShot(float UTSpeed, float Damage, float UTRadius, float Size, int32 Count, float Interval, const TCHAR* Style, const TCHAR* Name,
		const TCHAR* Sound, float Gravity = 0.f)
	{
		FArenaINVShot S;
		S.Speed = UTSpeed * UnitCm;
		S.Damage = Damage;
		S.DamageRadius = UTRadius * UnitCm;
		S.Size = Size;
		S.Count = Count;
		S.Interval = Interval;
		S.Style = Style;
		S.Name = Name;
		S.Sound = Sound;
		S.GravityScale = Gravity;
		return S;
	}

	static TArray<FArenaINVMonsterDef> BuildDefs()
	{
		TArray<FArenaINVMonsterDef> L;
		L.SetNum(int32(EArenaINVMonsterType::Count));
		using B = EArenaINVBehavior;
		auto Snd = [](const TCHAR* S) { return S; };

		// SkaarjPupae: Health 60, GroundSpeed 300, 28 x 12, MeleeRange 25, crawls (Crawl), bites, lunges.
		{
			FArenaINVMonsterDef D = MakeDef(TEXT("Gnashling"), B::Swarmer, 60.f, 28.f, 12.f, 300.f, 1, false, false, 25.f, 8.f, 0.9f, TEXT("Gnash"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/Pupae1");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/Pupae1_");
			V.Idle = TEXT("Idle_Rest"); V.Walk = TEXT("Crawl"); V.Run = TEXT("Crawl"); V.Leap = TEXT("Lunge");
			V.Melee = { TEXT("Bite"), TEXT("Stab") };
			V.Hit = { TEXT("TakeHit") };
			V.Death = { TEXT("Dead"), TEXT("Dead2"), TEXT("Dead3") };
			V.RunAnimSpeed = 600.f; V.WalkAnimSpeed = 600.f;
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Pupae_injur1pp"), TEXT("Sounds/SkaarjPack_rc/Pupae_injur2pp") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Pupae_death1pp") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Pupae_hiss2pp"), TEXT("Sounds/SkaarjPack_rc/Pupae_hiss1pp"), TEXT("Sounds/SkaarjPack_rc/Pupae_roam1pp") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Pupae_bite1pp");
			D.BodyColor = FLinearColor(0.32f, 0.40f, 0.16f); D.GlowColor = FLinearColor(0.55f, 1.f, 0.1f);
			L[int32(EArenaINVMonsterType::Gnashling)] = D;
		}
		// Razorfly: Health 35, AirSpeed 300, 18 x 11, MeleeRange 40.
		{
			FArenaINVMonsterDef D = MakeDef(TEXT("Razorwing"), B::FlyerMelee, 35.f, 18.f, 11.f, 300.f, 1, true, false, 40.f, 6.f, 0.8f, TEXT("Razor Bite"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/FlyM");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/FlyM_");
			V.Idle = TEXT("Fly"); V.Fly = TEXT("Fly");
			V.Melee = { TEXT("Shoot1"), TEXT("Shoot2") };
			V.Hit = { TEXT("TakeHit") };
			V.Death = { TEXT("Dead") };
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Razorfly_injur1rf"), TEXT("Sounds/SkaarjPack_rc/Razorfly_injur2rf") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Razorfly_death1rf") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Razorfly_buzz3rf") };
			D.BodyColor = FLinearColor(0.20f, 0.23f, 0.27f); D.GlowColor = FLinearColor(0.1f, 0.9f, 1.f);
			L[int32(EArenaINVMonsterType::Razorwing)] = D;
		}
		// Manta: Health 100, AirSpeed 400, 25 x 12, MeleeRange 200 (its long tail), stings / whips.
		{
			FArenaINVMonsterDef D = MakeDef(TEXT("Skyray"), B::FlyerMelee, 100.f, 25.f, 12.f, 400.f, 1, true, false, 120.f, 14.f, 1.1f, TEXT("Tail Sting"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/Manta1");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/Manta1_");
			V.Idle = TEXT("Fly"); V.Fly = TEXT("Fly");
			V.Melee = { TEXT("Sting"), TEXT("Whip") };
			V.Hit = { TEXT("TakeHit") };
			V.Death = { TEXT("Death") };
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Manta_injur1m"), TEXT("Sounds/SkaarjPack_rc/Manta_injur2m") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Manta_death2m") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Manta_call1m"), TEXT("Sounds/SkaarjPack_rc/Manta_call2m") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Manta_sting1m");
			D.BodyColor = FLinearColor(0.25f, 0.2f, 0.3f); D.GlowColor = FLinearColor(0.9f, 0.3f, 1.f);
			L[int32(EArenaINVMonsterType::Skyray)] = D;
		}
		// Krall: Health 100, GroundSpeed 440, 25 x 44, ScoringValue 2, KrallBolt (LinkProjectile in yellow).
		auto Krall = [&](const TCHAR* Name, int32 Points, const TCHAR* Skin, const TCHAR* Style, float ShotDamage)
		{
			FArenaINVMonsterDef D = MakeDef(Name, B::Spitter, 100.f, 25.f, 44.f, 440.f, Points, false, false, 90.f, 15.f, 1.2f, TEXT("Krall Staff"));
			D.Shot = MakeShot(900.f, ShotDamage, 0.f, 10.f, 1, 1.6f, Style, TEXT("Krall Bolt"), TEXT("Sounds/WeaponSounds/ShockRifle_ShockRifleAltFire"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/KrallM");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/KrallM_");
			V.Skins[0] = Skin; V.Skins[1] = Skin;
			V.Idle = TEXT("Breath2"); V.Walk = TEXT("WalkF"); V.Run = TEXT("RunF"); V.Leap = TEXT("Jump");
			V.Melee = { TEXT("Strike1"), TEXT("Strike2"), TEXT("Strike3") };
			V.Fire = { TEXT("Shoot1"), TEXT("Shoot3") };
			V.Hit = { TEXT("HitF") };
			V.Death = { TEXT("DeathB"), TEXT("DeathF"), TEXT("DeathL"), TEXT("DeathR") };
			V.RunAnimSpeed = 880.f; V.WalkAnimSpeed = 300.f;
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Krall_injur1k"), TEXT("Sounds/SkaarjPack_rc/Krall_injur2k") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Krall_death1k"), TEXT("Sounds/SkaarjPack_rc/Krall_death2k") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Krall_chlng1k"), TEXT("Sounds/SkaarjPack_rc/Krall_chlng2k") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Krall_hit2k");
			D.BodyColor = FLinearColor(0.55f, 0.42f, 0.10f); D.GlowColor = FLinearColor(1.f, 0.75f, 0.05f);
			return D;
		};
		L[int32(EArenaINVMonsterType::Sputterpod)] = Krall(TEXT("Sputterpod"), 2, nullptr, TEXT("KrallBolt"), 20.f);
		// EliteKrall: ScoringValue 3, skin ekrall, EliteKrallBolt (Damage 25, blue trail).
		L[int32(EArenaINVMonsterType::SputterpodElite)] = Krall(TEXT("Elite Sputterpod"), 3, TEXT("SkaarjPackSkins/Skins_ekrall"), TEXT("EliteKrallBolt"), 25.f);
		// Gasbag: Health 150, AirSpeed 330, 47 x 36, ScoringValue 4, GasBagBelch fireballs, TwoPunch / Pound.
		{
			FArenaINVMonsterDef D = MakeDef(TEXT("Whirrsting"), B::Flyer, 150.f, 47.f, 36.f, 330.f, 4, true, false, 90.f, 20.f, 1.4f, TEXT("Gas Punch"));
			D.Shot = MakeShot(700.f, 30.f, 60.f, 22.f, 1, 2.2f, TEXT("GasbagBelch"), TEXT("Belch"), TEXT("Sounds/SkaarjPack_rc/Gasbag_yell3g"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/GasBagM");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/GasBagM_");
			V.Idle = TEXT("Float"); V.Fly = TEXT("Float");
			V.Melee = { TEXT("TwoPunch"), TEXT("Pound") };
			V.Fire = { TEXT("Belch") };
			V.Hit = { TEXT("TakeHit"), TEXT("Hit2") };
			V.Death = { TEXT("Deflate"), TEXT("Dead2") };
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Gasbag_injur1g"), TEXT("Sounds/SkaarjPack_rc/Gasbag_injur2g") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Gasbag_death1g") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Gasbag_yell2g"), TEXT("Sounds/SkaarjPack_rc/Gasbag_nearby1g") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Gasbag_twopunch1g");
			D.BodyColor = FLinearColor(0.45f, 0.38f, 0.30f); D.GlowColor = FLinearColor(1.f, 0.45f, 0.1f);
			L[int32(EArenaINVMonsterType::Whirrsting)] = D;
		}
		// Brute: Health 220, GroundSpeed 150, 47 x 52, MeleeRange 80, ScoringValue 5, BruteRocket (Speed 750, Damage 55,
		// DamageRadius 180) from both arms; Behemoth: Health 260, ScoringValue 6, skin jBrute2.
		auto Brute = [&](const TCHAR* Name, float Health, int32 Points, const TCHAR* Skin)
		{
			FArenaINVMonsterDef D = MakeDef(Name, B::Brute, Health, 47.f, 52.f, 150.f, Points, false, false, 80.f, 25.f, 1.4f, TEXT("Brute Fist"));
			D.Shot = MakeShot(750.f, 55.f, 180.f, 14.f, 2, 3.f, TEXT("Rocket"), TEXT("Brute Rocket"), TEXT("Sounds/WeaponSounds/RocketLauncher_RocketLauncherFire"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/Brute1");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/Brute1_");
			V.Skins[0] = Skin;
			V.Idle = TEXT("Idle_Rest"); V.Walk = TEXT("WalkF"); V.Run = TEXT("WalkF");
			V.Melee = { TEXT("Punch"), TEXT("PistolWhip") };
			V.Fire = { TEXT("StillFire") };
			V.Hit = { TEXT("HitF"), TEXT("HitB") };
			V.Death = { TEXT("DeathF"), TEXT("DeathB"), TEXT("DeathL"), TEXT("DeathR") };
			V.RunAnimSpeed = 300.f; V.WalkAnimSpeed = 300.f;
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Brute_injur1br"), TEXT("Sounds/SkaarjPack_rc/Brute_injur2br") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Brute_death1br"), TEXT("Sounds/SkaarjPack_rc/Brute_death2br") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Brute_yell1br"), TEXT("Sounds/SkaarjPack_rc/Brute_nearby2br"), TEXT("Sounds/SkaarjPack_rc/Brute_yell2br") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Brute_pwhip1br");
			D.BodyColor = FLinearColor(0.12f, 0.11f, 0.10f); D.GlowColor = FLinearColor(1.f, 0.35f, 0.02f);
			return D;
		};
		L[int32(EArenaINVMonsterType::Slagbrute)] = Brute(TEXT("Slagbrute"), 220.f, 5, nullptr);
		L[int32(EArenaINVMonsterType::Slagtitan)] = Brute(TEXT("Slagtitan"), 260.f, 6, TEXT("SkaarjPackSkins/Skins_jBrute2"));
		// Skaarj: Health 150, GroundSpeed 440, 25 x 44, MeleeRange 60, ScoringValue 6, two SkaarjProjectiles (Speed
		// 1000, Damage 30, DamageRadius 150); IceSkaarj (skin 2, Speed 1250, Damage 25), FireSkaarj (skin 3, Speed 900,
		// Damage 45, DamageRadius 200), ScoringValue 7.
		auto Skaarj = [&](const TCHAR* Name, int32 Points, const TCHAR* Skin, float UTShotSpeed, float ShotDamage, float UTShotRadius, const TCHAR* Style)
		{
			FArenaINVMonsterDef D = MakeDef(Name, B::Leaper, 150.f, 25.f, 44.f, 440.f, Points, false, false, 60.f, 15.f, 0.9f, TEXT("Skaarj Claws"));
			D.Shot = MakeShot(UTShotSpeed, ShotDamage, UTShotRadius, 12.f, 2, 2.4f, Style, TEXT("Skaarj Bolt"), TEXT("Sounds/WeaponSounds/ShockRifle_ShockRifleFire"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/Skaarjw");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/Skaarjw_");
			V.Skins[0] = Skin;
			V.Idle = TEXT("Breath"); V.Walk = TEXT("WalkF"); V.Run = TEXT("RunF"); V.Leap = TEXT("FullJump");
			V.Melee = { TEXT("Claw"), TEXT("Spin") };
			V.Fire = { TEXT("Firing") };
			V.Hit = { TEXT("HitF"), TEXT("HitB"), TEXT("HitL"), TEXT("HitR") };
			V.Death = { TEXT("Death"), TEXT("Death2"), TEXT("Death3"), TEXT("Death4") };
			V.RunAnimSpeed = 880.f; V.WalkAnimSpeed = 300.f;
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/Skaarj_injur1sk"), TEXT("Sounds/SkaarjPack_rc/Skaarj_injur2sk"), TEXT("Sounds/SkaarjPack_rc/Skaarj_injur3sk") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/Skaarj_death1sk"), TEXT("Sounds/SkaarjPack_rc/Skaarj_death2sk") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/Skaarj_chalnge1s"), TEXT("Sounds/SkaarjPack_rc/Skaarj_chalnge3s"), TEXT("Sounds/SkaarjPack_rc/Skaarj_roam11s") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Skaarj_clawhit1s");
			D.BodyColor = FLinearColor(0.22f, 0.10f, 0.30f); D.GlowColor = FLinearColor(1.f, 0.15f, 0.8f);
			return D;
		};
		L[int32(EArenaINVMonsterType::Vaultfang)] = Skaarj(TEXT("Vaultfang"), 6, nullptr, 1000.f, 30.f, 150.f, TEXT("SkaarjBolt"));
		L[int32(EArenaINVMonsterType::FrostFang)] = Skaarj(TEXT("Frost Vaultfang"), 7, TEXT("SkaarjPackSkins/Skins_Skaarjw2"), 1250.f, 25.f, 150.f, TEXT("IceSkaarjBolt"));
		L[int32(EArenaINVMonsterType::EmberFang)] = Skaarj(TEXT("Ember Vaultfang"), 7, TEXT("SkaarjPackSkins/Skins_Skaarjw3"), 900.f, 45.f, 200.f, TEXT("FireSkaarjBolt"));
		// WarLord: Health 500, GroundSpeed 400 / AirSpeed 500, 47 x 78, MeleeRange 80, ScoringValue 10, WarlordRocket
		// (seeking, MaxSpeed 1000, Damage 70, DamageRadius 200), two per FlyFire.
		{
			FArenaINVMonsterDef D = MakeDef(TEXT("Cindermaw"), B::Boss, 500.f, 47.f, 78.f, 400.f, 10, false, true, 80.f, 40.f, 2.f, TEXT("Warlord Strike"));
			D.Shot = MakeShot(900.f, 70.f, 200.f, 18.f, 3, 4.5f, TEXT("Rocket"), TEXT("Warlord Rocket"), TEXT("Sounds/WeaponSounds/RocketLauncher_RocketLauncherFire"));
			FArenaINVVisual& V = D.Visual;
			V.Mesh = TEXT("SkeletalMeshes/SkaarjPack_rc/WarlordM");
			V.AnimPrefix = TEXT("Animations/SkaarjPack_rc/WarlordM_");
			V.Idle = TEXT("Idle_Rest"); V.Walk = TEXT("WalkF"); V.Run = TEXT("RunF"); V.Fly = TEXT("Fly");
			V.Melee = { TEXT("Strike"), TEXT("GKick1"), TEXT("GPunch1") };
			V.Fire = { TEXT("Fire") };
			V.Hit = { TEXT("TakeHit") };
			V.Death = { TEXT("Dead1"), TEXT("Dead2B") };
			V.RunAnimSpeed = 800.f; V.WalkAnimSpeed = 350.f;
			D.Sounds.Hit = { TEXT("Sounds/SkaarjPack_rc/WarLord_injur1WL"), TEXT("Sounds/SkaarjPack_rc/WarLord_injur2WL") };
			D.Sounds.Death = { TEXT("Sounds/SkaarjPack_rc/WarLord_DeathCry1WL") };
			D.Sounds.Challenge = { TEXT("Sounds/SkaarjPack_rc/WarLord_acquire1WL"), TEXT("Sounds/SkaarjPack_rc/WarLord_threat1WL"), TEXT("Sounds/SkaarjPack_rc/WarLord_laugh1WL") };
			D.Sounds.Melee = TEXT("Sounds/SkaarjPack_rc/Brute_pwhip1br");
			D.BodyColor = FLinearColor(0.07f, 0.03f, 0.03f); D.GlowColor = FLinearColor(1.f, 0.22f, 0.02f);
			L[int32(EArenaINVMonsterType::Cindermaw)] = D;
		}
		(void)Snd;
		return L;
	}

	const FArenaINVMonsterDef& GetMonsterDef(EArenaINVMonsterType Type)
	{
		static const TArray<FArenaINVMonsterDef> Defs = BuildDefs();
		const int32 Index = FMath::Clamp(static_cast<int32>(Type), 0, Defs.Num() - 1);
		return Defs[Index];
	}

	void GetWave(int32 Wave, TArray<EArenaINVMonsterType>& OutTypes, int32& OutMaxMonsters)
	{
		// Invasion.MonsterClass[16] and Invasion.Waves[16] (WaveMask = bits into MonsterClass, WaveMaxMonsters).
		using T = EArenaINVMonsterType;
		static const T MonsterClass[16] = { T::Gnashling, T::Razorwing, T::Skyray, T::Sputterpod, T::SputterpodElite, T::Whirrsting, T::Slagbrute,
			T::Vaultfang, T::Slagtitan, T::FrostFang, T::EmberFang, T::Cindermaw, T::Gnashling, T::Gnashling, T::Razorwing, T::Razorwing };
		static const int32 WaveMask[16] = { 20491, 60, 105, 186, 225, 966, 4771, 917, 1689, 18260, 14340, 4021, 3729, 3972, 3712, 2048 };
		static const int32 WaveMax[16] = { 16, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 8 };
		const int32 W = FMath::Clamp(Wave - 1, 0, 15);
		OutTypes.Reset();
		for (int32 Bit = 0; Bit < 16; ++Bit)
		{
			if (WaveMask[W] & (1 << Bit))
			{
				OutTypes.Add(MonsterClass[Bit]); // duplicates (Pupae / Razorfly twice) weight the pick like UT2004
			}
		}
		OutMaxMonsters = WaveMax[W];
	}
}

// =============================================================================================
// AArenaINV_Monster
// =============================================================================================

AArenaINV_Monster::AArenaINV_Monster()
{
	// Monsters are driven by AArenaINV_MonsterController, possessed explicitly by the Invasion rules.
	AutoPossessAI = EAutoPossessAI::Disabled;
}

const FArenaINVMonsterDef& AArenaINV_Monster::GetDef() const
{
	return ArenaINV::GetMonsterDef(MonsterType);
}

bool AArenaINV_Monster::IsFlyer() const
{
	return GetDef().bFlying;
}

bool AArenaINV_Monster::IsBoss() const
{
	return GetDef().bBoss;
}

void AArenaINV_Monster::InitMonster(EArenaINVMonsterType InType, float InHealthScale, float InDamageScale)
{
	MonsterType = InType;
	HealthScale = FMath::Max(0.1f, InHealthScale);
	DamageScale = FMath::Max(0.1f, InDamageScale);
	bConfigured = true;
}

FVector AArenaINV_Monster::GetFeetLocation() const
{
	return GetActorLocation() - FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
}

void AArenaINV_Monster::BeginPlay()
{
	const FArenaINVMonsterDef& Def = GetDef();

	// Body size and movement for this type (before the base class looks at anything).
	GetCapsuleComponent()->SetCapsuleSize(Def.CapsuleRadius, Def.CapsuleHalfHeight);
	BodyRoot->SetRelativeLocation(FVector(0.f, 0.f, -Def.CapsuleHalfHeight));
	BaseEyeHeight = Def.CapsuleHalfHeight * 0.55f;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = Def.Speed;
	Move->MaxFlySpeed = Def.Speed;
	Move->MaxStepHeight = FMath::Min(ArenaTuning::MaxStepHeight, FMath::Max(20.f, Def.CapsuleHalfHeight * 0.8f));
	Move->bOrientRotationToMovement = false;
	Move->GetNavAgentPropertiesRef().bCanCrouch = false;
	if (Def.Health >= 200.f)
	{
		Move->MaxAcceleration = 2200.f;
		Move->JumpZVelocity = 600.f;
		Move->Mass = 400.f;
	}
	if (Def.bFlying)
	{
		Move->BrakingDecelerationFlying = 1400.f;
		Move->DefaultLandMovementMode = MOVE_Flying;
	}

	MaxHealth = Def.Health * HealthScale;
	Health = MaxHealth;
	// No spawn protection for monsters (AArenaCharacter::HasSpawnProtection checks this flag).
	bFiredSinceSpawn = true;

	// The UT2004 body, else our parts go into BodyParts first: AArenaCharacter::BuildBody then skips the humanoid.
	bCustomBody = SetupImportedBody();
	if (!bCustomBody)
	{
		BuildMonsterBody();
	}

	Super::BeginPlay();

	ApplyMonsterMaterials();
	if (Def.bFlying)
	{
		Move->SetMovementMode(MOVE_Flying);
	}
	AnimTime = FMath::FRandRange(0.f, 10.f);
}

void AArenaINV_Monster::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	ApplyMonsterMaterials();
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetOwnerNoSee(false);
		}
	}
	if (bUsingImportedMesh)
	{
		GetMesh()->SetOwnerNoSee(false);
	}
	if (IsFlyer())
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
}

// ---------------------------------------------------------------------------------------------
// UT2004 body
// ---------------------------------------------------------------------------------------------

bool AArenaINV_Monster::SetupImportedBody()
{
	const FArenaINVMonsterDef& Def = GetDef();
	const FArenaINVVisual& V = Def.Visual;
	USkeletalMesh* Mesh = V.Mesh ? ArenaArt::Load<USkeletalMesh>(V.Mesh) : nullptr;
	if (!Mesh)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: no art for %s (%s), using the procedural stand-in"), Def.Name, V.Mesh ? V.Mesh : TEXT("-"));
		return false;
	}
	USkeletalMeshComponent* Skel = GetMesh();
	Skel->SetSkeletalMeshAsset(Mesh);
	// UE2: the mesh origin is the collision cylinder's centre (the converted mesh has MeshOrigin / RotOrigin baked in).
	Skel->SetRelativeLocation(FVector::ZeroVector);
	Skel->SetRelativeRotation(FRotator(0.f, V.Yaw, 0.f));
	Skel->SetRelativeScale3D(FVector(V.DrawScale));
	Skel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Skel->SetCanEverAffectNavigation(false);
	Skel->SetGenerateOverlapEvents(false);
	Skel->SetOwnerNoSee(false);
	Skel->SetCastShadow(true);
	Skel->SetVisibility(true);
	// vertex meshes: bounds of the reference pose do not cover every frame
	Skel->SetBoundsScale(1.5f);
	Skel->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Skel->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	// Skins (UT2004 Skins[]): a converted material, else the slot's material with the skin texture as its Base.
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		const TCHAR* Skin = V.Skins[Slot];
		if (!Skin || Slot >= Mesh->GetMaterials().Num())
		{
			continue;
		}
		if (UMaterialInterface* Mat = ArenaArt::Load<UMaterialInterface>(FString(TEXT("Materials/")) + Skin))
		{
			Skel->SetMaterial(Slot, Mat);
		}
		else if (UTexture2D* Tex = ArenaArt::Load<UTexture2D>(FString(TEXT("Textures/")) + Skin))
		{
			if (UMaterialInstanceDynamic* MID = ArenaRuntimeContent::CreateMaterialInstance(Skel->GetMaterial(Slot), Skel))
			{
				MID->SetTextureParameterValue(TEXT("Base"), Tex);
				Skel->SetMaterial(Slot, MID);
			}
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Invasion: skin %s for %s not found"), Skin, Def.Name);
		}
	}
	bUsingImportedMesh = true;
	CurrentSeq = nullptr;
	static TSet<EArenaINVMonsterType> Logged;
	if (!Logged.Contains(MonsterType))
	{
		Logged.Add(MonsterType);
		FString Mats;
		for (int32 i = 0; i < Skel->GetNumMaterials(); ++i)
		{
			const UMaterialInterface* M = Skel->GetMaterial(i);
			Mats += FString::Printf(TEXT(" [%d] %s"), i, M ? *M->GetName() : TEXT("none"));
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: %s uses %s, materials%s"), Def.Name, *Mesh->GetName(), *Mats);
	}
	PlaySequence(LoadSequence(Def.bFlying && V.Fly ? V.Fly : V.Idle), true);
	return true;
}

UAnimSequence* AArenaINV_Monster::LoadSequence(const TCHAR* Name) const
{
	const FArenaINVVisual& V = GetDef().Visual;
	if (!Name || !V.AnimPrefix)
	{
		return nullptr;
	}
	return ArenaArt::Load<UAnimSequence>(FString(V.AnimPrefix) + Name);
}

UAnimSequence* AArenaINV_Monster::PickSequence(const TArray<const TCHAR*>& Names) const
{
	return Names.Num() > 0 ? LoadSequence(Names[FMath::RandRange(0, Names.Num() - 1)]) : nullptr;
}

void AArenaINV_Monster::PlaySequence(UAnimSequence* Seq, bool bLoop, float Rate)
{
	if (!Seq || !bUsingImportedMesh)
	{
		return;
	}
	USkeletalMeshComponent* Skel = GetMesh();
	if (Seq != CurrentSeq || !bLoop || !bCurrentLoops)
	{
		Skel->PlayAnimation(Seq, bLoop);
		CurrentSeq = Seq;
		bCurrentLoops = bLoop;
	}
	Skel->SetPlayRate(Rate);
}

void AArenaINV_Monster::PlayAction(EAction Action)
{
	if (!bUsingImportedMesh || IsDead())
	{
		return;
	}
	const FArenaINVVisual& V = GetDef().Visual;
	UAnimSequence* Seq = nullptr;
	switch (Action)
	{
	case EAction::Melee: Seq = PickSequence(V.Melee); break;
	case EAction::Fire:  Seq = PickSequence(V.Fire.Num() ? V.Fire : V.Melee); break;
	case EAction::Leap:  Seq = LoadSequence(V.Leap); break;
	}
	if (Seq)
	{
		PlaySequence(Seq, false);
		OneShotEnd = GetWorld()->GetTimeSeconds() + FMath::Max(0.15f, Seq->GetPlayLength());
	}
}

void AArenaINV_Monster::PlayMonsterSound(const TArray<const TCHAR*>& Sounds, float Volume) const
{
	if (Sounds.Num() > 0)
	{
		PlayMonsterSound(Sounds[FMath::RandRange(0, Sounds.Num() - 1)], Volume);
	}
}

void AArenaINV_Monster::PlayMonsterSound(const TCHAR* Sound, float Volume) const
{
	if (USoundBase* S = Sound ? ArenaArt::LoadOneShotSound(Sound) : nullptr)
	{
		UGameplayStatics::PlaySoundAtLocation(this, S, GetActorLocation(), Volume);
	}
}

void AArenaINV_Monster::TickAnimation(float DeltaSeconds)
{
	const FArenaINVMonsterDef& Def = GetDef();
	const FArenaINVVisual& V = Def.Visual;
	const float Now = GetWorld()->GetTimeSeconds();

	// Hit reactions (AArenaCharacter counts hits) and their sounds
	if (AnimEvents.HitCount != SeenHitCount)
	{
		SeenHitCount = AnimEvents.HitCount;
		if (Now >= OneShotEnd && V.Hit.Num() > 0)
		{
			if (UAnimSequence* Seq = PickSequence(V.Hit))
			{
				PlaySequence(Seq, false);
				OneShotEnd = Now + FMath::Clamp(Seq->GetPlayLength(), 0.12f, 0.5f);
			}
		}
		if (Now - LastHitSoundTime > 0.6f)
		{
			LastHitSoundTime = Now;
			PlayMonsterSound(Def.Sounds.Hit, 0.9f);
		}
	}
	if (Now < OneShotEnd)
	{
		return;
	}

	// Movement: flight loop, else idle / walk / run by ground speed with the play rate following the speed
	const float Speed = GetVelocity().Size2D();
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (Def.bFlying || (Move->IsFalling() && V.Fly))
	{
		PlaySequence(LoadSequence(V.Fly ? V.Fly : V.Idle), true, 1.f);
	}
	else if (Move->IsFalling() && V.Leap && GetVelocity().Z > 200.f)
	{
		PlaySequence(LoadSequence(V.Leap), true, 1.f);
	}
	else if (Speed < 40.f || (!V.Walk && !V.Run))
	{
		PlaySequence(LoadSequence(V.Idle), true, 1.f);
	}
	else if (Speed < Def.Speed * 0.45f && V.Walk)
	{
		PlaySequence(LoadSequence(V.Walk), true, FMath::Clamp(Speed / FMath::Max(1.f, V.WalkAnimSpeed), 0.5f, 2.f));
	}
	else
	{
		PlaySequence(LoadSequence(V.Run ? V.Run : V.Walk), true, FMath::Clamp(Speed / FMath::Max(1.f, V.RunAnimSpeed), 0.5f, 2.f));
	}
}

bool AArenaINV_Monster::HandleDeathBody(bool bGib, const FVector& Velocity)
{
	const FArenaINVMonsterDef& Def = GetDef();
	bDeathHandled = true;
	PlayMonsterSound(Def.Sounds.Death);
	if (!bUsingImportedMesh)
	{
		return false; // procedural stand-in: break apart like any procedural body
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (bGib)
	{
		// UT2004 monsters gib with the organic gib set; small ones throw small pieces.
		const float SizeScale = FMath::Clamp(Def.CapsuleHalfHeight / 88.f, 0.4f, 1.6f);
		if (ArenaRagdoll::SpawnGibs(GetWorld(), GetActorLocation(), Velocity, ArenaRagdoll::EGibKind::Organic, SizeScale))
		{
			Move->DisableMovement();
			Move->StopMovementImmediately();
			GetMesh()->SetVisibility(false);
			return true;
		}
	}
	// Vertex meshes have no bones to ragdoll: the death sequence plays and holds its last frame (as in UT2004),
	// flyers drop to the floor.
	PlaySequence(PickSequence(Def.Visual.Death), false);
	OneShotEnd = TNumericLimits<float>::Max();
	Move->bRunPhysicsWithNoController = true;
	Move->Velocity.X *= 0.3f;
	Move->Velocity.Y *= 0.3f;
	Move->SetMovementMode(MOVE_Falling);
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPose;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Procedural stand-in (no UT2004 art)
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaINV_Monster::AddPart(UStaticMesh* Mesh, const FVector& Loc, const FVector& SizeCm, int32 MatIndex, const FRotator& Rot)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(Mesh);
	C->SetupAttachment(BodyRoot);
	C->SetRelativeLocation(Loc);
	C->SetRelativeRotation(Rot);
	C->SetRelativeScale3D(SizeCm / 100.f);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCanEverAffectNavigation(false);
	C->SetCastShadow(MatIndex != 2);
	C->RegisterComponent();
	BodyParts.Add(C);
	PartMaterials.Add(MonsterMats.IsValidIndex(MatIndex) ? MonsterMats[MatIndex] : nullptr);
	return C;
}

void AArenaINV_Monster::BuildMonsterBody()
{
	if (BodyParts.Num() > 0)
	{
		return;
	}
	const FArenaINVMonsterDef& Def = GetDef();
	// The stand-in shapes were drawn for these heights per behaviour; fit them to the capsule.
	{
		float RefHalfHeight = 60.f;
		switch (Def.Behavior)
		{
		case EArenaINVBehavior::Swarmer:    RefHalfHeight = 42.f; break;
		case EArenaINVBehavior::Leaper:     RefHalfHeight = 60.f; break;
		case EArenaINVBehavior::Spitter:    RefHalfHeight = 70.f; break;
		case EArenaINVBehavior::Brute:      RefHalfHeight = 115.f; break;
		case EArenaINVBehavior::Flyer:
		case EArenaINVBehavior::FlyerMelee: RefHalfHeight = 34.f; break;
		case EArenaINVBehavior::Boss:       RefHalfHeight = 170.f; break;
		}
		BodyRoot->SetRelativeScale3D(FVector(Def.CapsuleHalfHeight / RefHalfHeight));
	}
	MonsterMats.Reset();
	MonsterMats.Add(ArenaAssets::MakeSurface(this, Def.BodyColor, 0.55f, 0.15f));                                   // 0 body
	MonsterMats.Add(ArenaAssets::MakeSurface(this, Def.BodyColor * 0.35f, 0.7f, 0.05f));                              // 1 dark
	MonsterMats.Add(ArenaAssets::MakeSurface(this, Def.GlowColor, 0.2f, 0.f, Def.GlowColor * 8.f));                   // 2 glow
	MonsterMats.Add(ArenaAssets::MakeSurface(this, FMath::Lerp(Def.BodyColor, Def.GlowColor, 0.35f), 0.35f, 0.5f));  // 3 accent

	UStaticMesh* Cube = ArenaAssets::Cube();
	UStaticMesh* Sphere = ArenaAssets::Sphere();
	UStaticMesh* Cyl = ArenaAssets::Cylinder();
	UStaticMesh* Cone = ArenaAssets::Cone();

	auto AddAnim = [this](UStaticMeshComponent* C, int32 Kind)
	{
		AnimParts.Add(C);
		AnimBaseLoc.Add(C->GetRelativeLocation());
		AnimBaseRot.Add(C->GetRelativeRotation());
		AnimBaseScale.Add(C->GetRelativeScale3D());
		AnimKind.Add(Kind);
	};

	switch (Def.Behavior)
	{
	case EArenaINVBehavior::Swarmer:
	{
		// Low, many-legged scuttler with an oversized split jaw.
		AddPart(Sphere, FVector(-6, 0, 40), FVector(95, 56, 42), 0);                             // body
		AddPart(Sphere, FVector(-40, 0, 46), FVector(50, 40, 34), 1);                            // abdomen
		AddPart(Cone, FVector(34, 0, 48), FVector(40, 46, 46), 3, FRotator(-80, 0, 0));          // upper jaw
		AddPart(Cone, FVector(32, 0, 34), FVector(30, 38, 34), 1, FRotator(-100, 0, 0));         // lower jaw
		AddPart(Sphere, FVector(26, -14, 58), FVector(11, 11, 11), 2);                           // eyes
		AddPart(Sphere, FVector(26, 14, 58), FVector(11, 11, 11), 2);
		AddPart(Sphere, FVector(16, 0, 64), FVector(8, 8, 8), 2);
		for (int32 i = 0; i < 3; ++i)
		{
			const float X = 18.f - i * 22.f;
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				UStaticMeshComponent* Leg = AddPart(Cyl, FVector(X, Side * 34.f, 20), FVector(7, 7, 46), 1, FRotator(0, 0, Side * 50.f));
				AddAnim(Leg, 0);
			}
		}
		AddPart(Cone, FVector(-60, 0, 52), FVector(14, 14, 30), 2, FRotator(80, 0, 0));          // glowing tail barb
		break;
	}
	case EArenaINVBehavior::Leaper:
	{
		// Lean, forward-tilted leaper with long spring legs and a crest of fins.
		AddPart(Cube, FVector(4, 0, 74), FVector(86, 40, 34), 0, FRotator(18, 0, 0));            // torso
		AddPart(Sphere, FVector(46, 0, 92), FVector(36, 30, 28), 0);                            // head
		AddPart(Cone, FVector(70, 0, 88), FVector(18, 22, 34), 3, FRotator(-90, 0, 0));          // snout
		AddPart(Cube, FVector(56, -10, 100), FVector(8, 6, 4), 2);                               // eye slits
		AddPart(Cube, FVector(56, 10, 100), FVector(8, 6, 4), 2);
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			// hind spring leg: thigh + shin
			UStaticMeshComponent* Thigh = AddPart(Cyl, FVector(-26, Side * 22.f, 54), FVector(14, 14, 50), 0, FRotator(-35, 0, 0));
			UStaticMeshComponent* Shin = AddPart(Cyl, FVector(-30, Side * 22.f, 20), FVector(9, 9, 46), 1, FRotator(30, 0, 0));
			AddAnim(Thigh, 0);
			AddAnim(Shin, 0);
			// short claw arm
			UStaticMeshComponent* Arm = AddPart(Cyl, FVector(34, Side * 20.f, 52), FVector(8, 8, 40), 1, FRotator(-25, 0, 0));
			AddAnim(Arm, 0);
			AddPart(Cone, FVector(42, Side * 20.f, 30), FVector(10, 10, 18), 2, FRotator(180, 0, 0));
		}
		for (int32 i = 0; i < 4; ++i)
		{
			AddPart(Cone, FVector(16.f - i * 18.f, 0, 98.f - i * 6.f), FVector(6, 18, 30.f - i * 4.f), 2, FRotator(-15, 0, 0)); // fins
		}
		AddPart(Cone, FVector(-62, 0, 70), FVector(14, 14, 60), 1, FRotator(100, 0, 0));        // tail
		break;
	}
	case EArenaINVBehavior::Spitter:
	{
		// Bloated sac on stubby legs with glowing pustules and a forward spout.
		UStaticMeshComponent* Sac = AddPart(Sphere, FVector(0, 0, 76), FVector(110, 100, 92), 0);
		AddAnim(Sac, 2);
		AddPart(Sphere, FVector(-8, 0, 118), FVector(70, 64, 40), 3);                            // crown
		AddPart(Cyl, FVector(52, 0, 104), FVector(22, 22, 52), 1, FRotator(-55, 0, 0));          // spout
		AddPart(Sphere, FVector(72, 0, 120), FVector(20, 20, 20), 2);                            // spout glow
		const FVector Pustules[] = { FVector(30, -42, 84), FVector(-20, -48, 70), FVector(30, 42, 84), FVector(-20, 48, 70), FVector(-46, 0, 96), FVector(10, -30, 120) };
		for (const FVector& P : Pustules)
		{
			UStaticMeshComponent* Pu = AddPart(Sphere, P, FVector(22, 22, 22), 2);
			AddAnim(Pu, 2);
		}
		for (int32 i = 0; i < 3; ++i)
		{
			const float A = FMath::DegreesToRadians(i * 120.f + 60.f);
			AddPart(Cyl, FVector(FMath::Cos(A) * 34.f, FMath::Sin(A) * 34.f, 18), FVector(16, 16, 40), 1);
		}
		break;
	}
	case EArenaINVBehavior::Brute:
	{
		// Hulking slab of cooled slag with glowing seams and knuckle-dragging fists.
		AddPart(Cube, FVector(0, 0, 150), FVector(80, 120, 90), 0, FRotator(10, 0, 0));          // chest
		AddPart(Cube, FVector(-4, 0, 96), FVector(64, 86, 50), 1);                               // belly
		AddPart(Sphere, FVector(0, -70, 178), FVector(70, 66, 62), 0);                           // shoulder boulders
		AddPart(Sphere, FVector(0, 70, 178), FVector(70, 66, 62), 0);
		AddPart(Sphere, FVector(30, 0, 196), FVector(40, 40, 34), 3);                            // sunken head
		AddPart(Cube, FVector(50, 0, 198), FVector(6, 26, 6), 2);                                // brow glow
		AddPart(Cube, FVector(40, 0, 150), FVector(4, 90, 8), 2, FRotator(0, 0, 12));            // seams
		AddPart(Cube, FVector(40, 0, 120), FVector(4, 70, 6), 2, FRotator(0, 0, -15));
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			UStaticMeshComponent* Arm = AddPart(Cyl, FVector(10, Side * 86.f, 120), FVector(34, 34, 90), 1, FRotator(-10, 0, 0));
			AddAnim(Arm, 0);
			UStaticMeshComponent* Fist = AddPart(Cube, FVector(18, Side * 88.f, 56), FVector(58, 52, 50), 0);
			AddAnim(Fist, 0);
			AddPart(Cube, FVector(46, Side * 88.f, 56), FVector(6, 40, 8), 2);                    // knuckle glow
			UStaticMeshComponent* Leg = AddPart(Cyl, FVector(-10, Side * 34.f, 34), FVector(40, 40, 70), 1);
			AddAnim(Leg, 0);
		}
		AddPart(Cone, FVector(-30, -40, 222), FVector(24, 24, 40), 2, FRotator(-20, 0, 0));      // vents
		AddPart(Cone, FVector(-30, 40, 222), FVector(24, 24, 40), 2, FRotator(-20, 0, 0));
		break;
	}
	case EArenaINVBehavior::Flyer:
	case EArenaINVBehavior::FlyerMelee:
	{
		// Hovering disc drone: central eye, three spinning rotor pods and a hanging stinger.
		AddPart(Cyl, FVector(0, 0, 36), FVector(84, 84, 16), 0);                                 // disc
		AddPart(Cyl, FVector(0, 0, 36), FVector(96, 96, 4), 2);                                  // rim glow
		AddPart(Sphere, FVector(0, 0, 50), FVector(46, 46, 30), 3);                              // dome
		AddPart(Sphere, FVector(22, 0, 42), FVector(20, 20, 20), 2);                             // eye
		for (int32 i = 0; i < 3; ++i)
		{
			const float A = FMath::DegreesToRadians(i * 120.f + 60.f);
			const FVector P(FMath::Cos(A) * 56.f, FMath::Sin(A) * 56.f, 44.f);
			AddPart(Cyl, P - FVector(0, 0, 6), FVector(20, 20, 18), 1);
			UStaticMeshComponent* Rotor = AddPart(Cube, P + FVector(0, 0, 6), FVector(52, 6, 2), 3);
			AddAnim(Rotor, 1);
		}
		AddPart(Cone, FVector(10, 0, 12), FVector(14, 14, 34), 2, FRotator(155, 0, 0));          // stinger
		break;
	}
	case EArenaINVBehavior::Boss:
	default:
	{
		// Towering obsidian titan: pillar legs, furnace maw in the chest, horn crown and arm cannons.
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			UStaticMeshComponent* Leg = AddPart(Cyl, FVector(0, Side * 50.f, 70), FVector(62, 62, 140), 1);
			AddAnim(Leg, 0);
			AddPart(Cube, FVector(16, Side * 50.f, 10), FVector(90, 70, 20), 0);                  // feet
		}
		AddPart(Sphere, FVector(0, 0, 220), FVector(170, 180, 170), 0);                         // torso
		AddPart(Sphere, FVector(70, 0, 210), FVector(60, 90, 70), 2);                            // furnace maw
		AddPart(Cube, FVector(84, 0, 238), FVector(20, 110, 14), 1);                             // upper teeth bar
		AddPart(Cube, FVector(84, 0, 182), FVector(20, 100, 14), 1);                             // lower teeth bar
		AddPart(Sphere, FVector(30, 0, 312), FVector(70, 70, 56), 3);                            // head
		AddPart(Cube, FVector(62, -16, 318), FVector(8, 14, 6), 2);                              // eyes
		AddPart(Cube, FVector(62, 16, 318), FVector(8, 14, 6), 2);
		for (int32 i = 0; i < 5; ++i)
		{
			const float Y = -40.f + i * 20.f;
			AddPart(Cone, FVector(20, Y, 352.f + (i == 2 ? 10.f : 0.f)), FVector(16, 16, 50.f + (i == 2 ? 20.f : 0.f)), 0, FRotator(0, 0, Y * -0.6f)); // horn crown
		}
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			AddPart(Sphere, FVector(0, Side * 100.f, 270), FVector(80, 70, 70), 1);              // shoulders
			UStaticMeshComponent* Cannon = AddPart(Cyl, FVector(40, Side * 118.f, 220), FVector(48, 48, 120), 0, FRotator(-75, 0, 0));
			AddAnim(Cannon, 0);
			AddPart(Cyl, FVector(100, Side * 118.f, 236), FVector(36, 36, 10), 2, FRotator(-75, 0, 0)); // muzzle glow
			AddPart(Cone, FVector(-60, Side * 60.f, 300), FVector(30, 30, 70), 2, FRotator(-30, 0, 0)); // back vents
		}
		break;
	}
	}
}

void AArenaINV_Monster::ApplyMonsterMaterials()
{
	for (int32 i = 0; i < BodyParts.Num(); ++i)
	{
		if (BodyParts[i] && PartMaterials.IsValidIndex(i) && PartMaterials[i])
		{
			BodyParts[i]->SetMaterial(0, PartMaterials[i]);
		}
	}
}

void AArenaINV_Monster::SetChargeGlow(float Alpha)
{
	Alpha = FMath::Clamp(Alpha, 0.f, 1.f);
	if (FMath::IsNearlyEqual(Alpha, ChargeGlow, 0.02f))
	{
		return;
	}
	ChargeGlow = Alpha;
	if (MonsterMats.IsValidIndex(2) && MonsterMats[2])
	{
		const FLinearColor G = GetDef().GlowColor;
		MonsterMats[2]->SetVectorParameterValue(TEXT("Emissive"), G * (8.f + 40.f * Alpha));
	}
}

void AArenaINV_Monster::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (IsDead())
	{
		return;
	}

	// Flyers: knockback (LaunchCharacter) switches to falling; keep hovering.
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (IsFlyer() && Move->MovementMode != MOVE_Flying)
	{
		Move->SetMovementMode(MOVE_Flying);
	}

	if (bUsingImportedMesh)
	{
		TickAnimation(DeltaSeconds);
		return;
	}

	// Procedural stand-in animation: legs swing with speed, rotors spin, sacs pulse.
	const float Speed = GetVelocity().Size2D();
	const float SpeedAlpha = FMath::Clamp(Speed / FMath::Max(100.f, GetDef().Speed), 0.f, 1.f);
	AnimTime += DeltaSeconds * (2.f + 10.f * SpeedAlpha);
	for (int32 i = 0; i < AnimParts.Num(); ++i)
	{
		UStaticMeshComponent* C = AnimParts[i];
		if (!C || C->GetAttachParent() != BodyRoot)
		{
			continue;
		}
		switch (AnimKind[i])
		{
		case 0: // swing
		{
			const float Phase = (i % 2 == 0) ? 0.f : PI;
			FRotator R = AnimBaseRot[i];
			R.Pitch += FMath::Sin(AnimTime + Phase) * 22.f * SpeedAlpha;
			C->SetRelativeRotation(R);
			break;
		}
		case 1: // spin
		{
			FRotator R = AnimBaseRot[i];
			R.Yaw += FMath::Fmod(GetWorld()->GetTimeSeconds() * 1440.f + i * 40.f, 360.f);
			C->SetRelativeRotation(R);
			break;
		}
		case 2: // pulse
		{
			const float Wave = FMath::Sin(GetWorld()->GetTimeSeconds() * 4.f + i);
			C->SetRelativeLocation(AnimBaseLoc[i] + FVector(0.f, 0.f, 2.f * Wave));
			C->SetRelativeScale3D(AnimBaseScale[i] * (1.f + 0.06f * Wave));
			break;
		}
		default:
			break;
		}
	}

	// Flyers bob gently.
	if (IsFlyer())
	{
		BodyRoot->SetRelativeLocation(FVector(0.f, 0.f, -GetDef().CapsuleHalfHeight + 6.f * FMath::Sin(GetWorld()->GetTimeSeconds() * 2.5f)));
	}
}

// ---------------------------------------------------------------------------------------------
// Showcase (test hook)
// ---------------------------------------------------------------------------------------------

void AArenaINV_Monster::DebugPlayRole(int32 Role)
{
	const FArenaINVVisual& V = GetDef().Visual;
	const float Now = GetWorld()->GetTimeSeconds();
	switch (Role)
	{
	case 0: PlaySequence(LoadSequence(GetDef().bFlying && V.Fly ? V.Fly : V.Idle), true); OneShotEnd = 0.f; break;
	case 1: PlaySequence(LoadSequence(V.Walk ? V.Walk : (V.Fly ? V.Fly : V.Idle)), true); OneShotEnd = Now + 2.f; break;
	case 2: PlaySequence(LoadSequence(V.Run ? V.Run : (V.Fly ? V.Fly : V.Idle)), true); OneShotEnd = Now + 2.f; break;
	case 3: PlayAction(EAction::Melee); break;
	case 4: PlayAction(EAction::Fire); break;
	case 5: PlaySequence(PickSequence(V.Hit), false); OneShotEnd = Now + 0.5f; break;
	default: PlaySequence(PickSequence(V.Death), false); OneShotEnd = TNumericLimits<float>::Max(); break;
	}
}

bool ArenaINV::IsShowcase()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("INVShowcase"));
	return bEnabled;
#endif
}

void ArenaINV::TickDebugShowcase(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!IsShowcase() || !World)
	{
		return;
	}
	static TArray<TWeakObjectPtr<AArenaINV_Monster>> Shown;
	static TWeakObjectPtr<ACameraActor> Cam;
	static float StartTime = -1.f;
	static int32 LastRoleStep = -1;
	static FVector Fwd = FVector::ForwardVector;
	const float Now = World->GetTimeSeconds();
	if (StartTime < 0.f)
	{
		if (Now < 3.f)
		{
			return;
		}
		StartTime = Now;
		TActorIterator<APlayerStart> It(World);
		APlayerStart* Start = It ? *It : nullptr;
		if (!Start)
		{
			return;
		}
		Fwd = FRotator(0.f, Start->GetActorRotation().Yaw, 0.f).Vector();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd);
		const FVector Base = Start->GetActorLocation();
		const int32 Num = int32(EArenaINVMonsterType::Count);
		for (int32 i = 0; i < Num; ++i)
		{
			const FArenaINVMonsterDef& Def = GetMonsterDef(EArenaINVMonsterType(i));
			FVector Spot = Base + Fwd * 500.f + Right * (i - Num / 2) * 260.f;
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(INVShowcase), false);
			if (World->LineTraceSingleByChannel(Hit, Spot + FVector(0.f, 0.f, 400.f), Spot - FVector(0.f, 0.f, 1500.f), ECC_WorldStatic, Q))
			{
				Spot = Hit.Location;
			}
			const FTransform TM(FRotator(0.f, (-Fwd).Rotation().Yaw, 0.f), Spot + FVector(0.f, 0.f, Def.CapsuleHalfHeight + 4.f + (Def.bFlying ? 80.f : 0.f)));
			AArenaINV_Monster* M = World->SpawnActorDeferred<AArenaINV_Monster>(AArenaINV_Monster::StaticClass(), TM, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (M)
			{
				M->InitMonster(EArenaINVMonsterType(i), 1.f, 1.f);
				M->FinishSpawning(TM);
				Shown.Add(M);
				UE_LOG(LogOpenUT2k4, Log, TEXT("INVShowcase: %s (%s) at %s, capsule %.0f x %.0f, art %d"), Def.Name, Def.Visual.Mesh ? Def.Visual.Mesh : TEXT("-"),
					*Spot.ToCompactString(), Def.CapsuleRadius, Def.CapsuleHalfHeight, M->HasImportedBody() ? 1 : 0);
			}
		}
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Cam = World->SpawnActor<ACameraActor>(Base, Fwd.Rotation(), P);
		if (Cam.IsValid())
		{
			Cam->GetCameraComponent()->SetFieldOfView(70.f);
			Cam->GetCameraComponent()->bConstrainAspectRatio = false;
		}
	}
	// Camera on monster k (2.5 s each, then the whole row), roles cycling every 0.8 s, deaths after one round.
	const float T = Now - StartTime;
	const int32 Num = Shown.Num();
	if (Num == 0 || !Cam.IsValid())
	{
		return;
	}
	const int32 K = int32(T / 2.5f);
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->GetViewTarget() != Cam.Get())
		{
			PC->SetViewTarget(Cam.Get());
		}
	}
	AArenaINV_Monster* Focus = Shown[K % Num].Get();
	if (K >= Num && K < Num + 2)
	{
		// whole row
		const FVector Mid = (Shown[0].IsValid() && Shown[Num - 1].IsValid()) ? (Shown[0]->GetActorLocation() + Shown[Num - 1]->GetActorLocation()) * 0.5f : Cam->GetActorLocation();
		Cam->SetActorLocationAndRotation(Mid - Fwd * 1500.f + FVector(0.f, 0.f, 350.f), (Fwd * 1500.f - FVector(0.f, 0.f, 350.f)).Rotation());
	}
	else if (Focus)
	{
		const float Size = FMath::Max(60.f, Focus->GetDef().CapsuleHalfHeight * 2.f);
		const FVector Target = Focus->GetActorLocation();
		// camera between the row and the start, slightly to the side
		const FVector From = Target + (Fwd * -1.f).RotateAngleAxis(30.f, FVector::UpVector) * (Size * 1.6f + 110.f) + FVector(0.f, 0.f, Size * 0.25f);
		Cam->SetActorLocationAndRotation(From, (Target - From).Rotation());
	}
	const int32 RoleStep = int32(T / 0.8f);
	if (RoleStep != LastRoleStep)
	{
		LastRoleStep = RoleStep;
		const bool bDeaths = K >= Num + 2;
		static const int32 Roles[] = { 0, 3, 1, 4, 2, 5 };
		for (int32 i = 0; i < Num; ++i)
		{
			if (AArenaINV_Monster* M = Shown[i].Get())
			{
				if (bDeaths)
				{
					static TSet<const AArenaINV_Monster*> Dead;
					if (!Dead.Contains(M))
					{
						Dead.Add(M);
						M->DebugPlayRole(6);
					}
				}
				else
				{
					M->DebugPlayRole(Roles[(RoleStep + i) % UE_ARRAY_COUNT(Roles)]);
				}
			}
		}
	}
#endif
}

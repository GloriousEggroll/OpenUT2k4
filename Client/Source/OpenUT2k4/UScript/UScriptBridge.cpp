// UScript bridge: the script world of a game world (LevelInfo, shadow GameInfo, mutators, proxies of our actors)
// and the game hooks (ArenaUScriptHooks) that run UT2004 mutator / game rules script at the UT2004 call points.

#include "UScriptBridge.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "ArenaUScript.h"
#include "../ArenaGameMode.h"
#include "../ArenaCharacter.h"
#include "../ArenaPlayerState.h"
#include "../ArenaPickup.h"
#include "../ArenaWeapons.h"
#include "../ArenaWeaponsExtra.h"
#include "../ArenaTranslocator.h"
#include "../ArenaTypes.h"
#include "../ArenaUE2World.h"
#include "../ArenaPlayerController.h"
#include "ArenaScriptActor.h"
#include "../ArenaRuntimeContent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	TMap<TWeakObjectPtr<UWorld>, TUniquePtr<FUSGameWorld>>& Worlds()
	{
		static TMap<TWeakObjectPtr<UWorld>, TUniquePtr<FUSGameWorld>> Map;
		return Map;
	}

	bool GCleanupRegistered = false;

	template <typename T> T* PropPtr(const FUSPropRef& R, FUSObject* O) { return R.Ptr<T>(O); }

	/** Our weapon <-> UT2004 weapon class names. */
	struct FWeaponMap { const TCHAR* UE2; TSubclassOf<AArenaWeapon> (*Ours)(); };
	const FWeaponMap* WeaponTable(int32& OutNum)
	{
		static const FWeaponMap Table[] = {
			{ TEXT("ShieldGun"), [] { return TSubclassOf<AArenaWeapon>(AWeap_Gauntlet::StaticClass()); } },
			{ TEXT("AssaultRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_Carbine::StaticClass()); } },
			{ TEXT("BioRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_GooLauncher::StaticClass()); } },
			{ TEXT("ShockRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_PulseRifle::StaticClass()); } },
			{ TEXT("LinkGun"), [] { return TSubclassOf<AArenaWeapon>(AWeap_ArcLinker::StaticClass()); } },
			{ TEXT("Minigun"), [] { return TSubclassOf<AArenaWeapon>(AWeap_RotaryGun::StaticClass()); } },
			{ TEXT("FlakCannon"), [] { return TSubclassOf<AArenaWeapon>(AWeap_ShardCannon::StaticClass()); } },
			{ TEXT("RocketLauncher"), [] { return TSubclassOf<AArenaWeapon>(AWeap_RocketLauncher::StaticClass()); } },
			{ TEXT("SniperRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_RailRifle::StaticClass()); } },
			{ TEXT("SuperShockRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_InstaRifle::StaticClass()); } },
			{ TEXT("Redeemer"), [] { return TSubclassOf<AArenaWeapon>(AWeap_WarheadLauncher::StaticClass()); } },
			{ TEXT("Painter"), [] { return TSubclassOf<AArenaWeapon>(AWeap_OrbitalDesignator::StaticClass()); } },
			{ TEXT("ClassicSniperRifle"), [] { return TSubclassOf<AArenaWeapon>(AWeap_MarksmanRifle::StaticClass()); } },
			{ TEXT("ONSAVRiL"), [] { return TSubclassOf<AArenaWeapon>(AWeap_SeekerLauncher::StaticClass()); } },
			{ TEXT("ONSGrenadeLauncher"), [] { return TSubclassOf<AArenaWeapon>(AWeap_StickyLauncher::StaticClass()); } },
			{ TEXT("ONSMineLayer"), [] { return TSubclassOf<AArenaWeapon>(AWeap_CrawlerLayer::StaticClass()); } },
			{ TEXT("TransLauncher"), [] { return TSubclassOf<AArenaWeapon>(AWeap_Translocator::StaticClass()); } },
		};
		OutNum = UE_ARRAY_COUNT(Table);
		return Table;
	}

	const TCHAR* UE2WeaponName(TSubclassOf<AArenaWeapon> W)
	{
		int32 N;
		const FWeaponMap* T = WeaponTable(N);
		for (int32 i = 0; i < N; ++i)
		{
			if (T[i].Ours() == W)
			{
				return T[i].UE2;
			}
		}
		return nullptr;
	}

	TSubclassOf<AArenaWeapon> OurWeapon(const FString& UE2Name)
	{
		int32 N;
		const FWeaponMap* T = WeaponTable(N);
		for (int32 i = 0; i < N; ++i)
		{
			if (UE2Name.Equals(T[i].UE2, ESearchCase::IgnoreCase))
			{
				return T[i].Ours();
			}
		}
		if (UE2Name.Equals(TEXT("ONSPainter"), ESearchCase::IgnoreCase))
		{
			return AWeap_OrbitalDesignator::StaticClass();
		}
		if (UE2Name.Equals(TEXT("ONSGrenade"), ESearchCase::IgnoreCase)) // ONSGrenadePickup
		{
			return AWeap_StickyLauncher::StaticClass();
		}
		return nullptr;
	}

	const TCHAR* AmmoName(const TCHAR* Weapon)
	{
		static const TMap<FString, FString> M = {
			{ TEXT("AssaultRifle"), TEXT("AssaultAmmoPickup") }, { TEXT("BioRifle"), TEXT("BioAmmoPickup") },
			{ TEXT("ShockRifle"), TEXT("ShockAmmoPickup") }, { TEXT("LinkGun"), TEXT("LinkAmmoPickup") },
			{ TEXT("Minigun"), TEXT("MinigunAmmoPickup") }, { TEXT("FlakCannon"), TEXT("FlakAmmoPickup") },
			{ TEXT("RocketLauncher"), TEXT("RocketAmmoPickup") }, { TEXT("SniperRifle"), TEXT("SniperAmmoPickup") },
			{ TEXT("ClassicSniperRifle"), TEXT("ClassicSniperAmmoPickup") }, { TEXT("ONSAVRiL"), TEXT("ONSAVRiLAmmoPickup") },
			{ TEXT("ONSGrenadeLauncher"), TEXT("ONSGrenadeAmmoPickup") }, { TEXT("ONSMineLayer"), TEXT("ONSMineAmmoPickup") } };
		const FString* S = M.Find(Weapon);
		return S ? **S : nullptr;
	}

	/** UE2 pickup class path for one of our pickups. */
	FString UE2PickupClass(EArenaPickupType Type, TSubclassOf<AArenaWeapon> Weapon)
	{
		switch (Type)
		{
		case EArenaPickupType::HealthVial: return TEXT("XPickups.MiniHealthPack");
		case EArenaPickupType::HealthPack: return TEXT("XPickups.HealthPack");
		case EArenaPickupType::SuperHealth: return TEXT("XPickups.SuperHealthPack");
		case EArenaPickupType::ShieldSmall: return TEXT("XPickups.ShieldPack");
		case EArenaPickupType::ShieldLarge: return TEXT("XPickups.SuperShieldPack");
		case EArenaPickupType::DoubleDamage: return TEXT("XPickups.UDamagePack");
		case EArenaPickupType::Adrenaline: return TEXT("XPickups.AdrenalinePickup");
		case EArenaPickupType::Weapon:
			if (const TCHAR* W = UE2WeaponName(Weapon))
			{
				const FString Name(W);
				if (Name == TEXT("ONSGrenadeLauncher"))
				{
					return TEXT("Onslaught.ONSGrenadePickup");
				}
				const TCHAR* Pkg = Name.StartsWith(TEXT("ONS")) ? TEXT("Onslaught") : (Name == TEXT("ClassicSniperRifle") ? TEXT("UTClassic") : TEXT("XWeapons"));
				return FString::Printf(TEXT("%s.%sPickup"), Pkg, W);
			}
			return FString();
		case EArenaPickupType::Ammo:
			if (const TCHAR* W = UE2WeaponName(Weapon))
			{
				if (const TCHAR* A = AmmoName(W))
				{
					const FString Name(W);
					const TCHAR* Pkg = Name.StartsWith(TEXT("ONS")) ? TEXT("Onslaught") : (Name == TEXT("ClassicSniperRifle") ? TEXT("UTClassic") : TEXT("XWeapons"));
					return FString::Printf(TEXT("%s.%s"), Pkg, A);
				}
			}
			return FString();
		default:
			return FString();
		}
	}

	/** Our pickup for a UE2 pickup class (walks the class chain). */
	bool OurPickup(FUSClass* C, EArenaPickupType& OutType, TSubclassOf<AArenaWeapon>& OutWeapon)
	{
		for (int32 Guard = 0; C && Guard < 32; C = C->GetSuperClass(), ++Guard)
		{
			const FString N = C->GetName();
			struct FItem { const TCHAR* Name; EArenaPickupType Type; };
			static const FItem Items[] = {
				{ TEXT("MiniHealthPack"), EArenaPickupType::HealthVial }, { TEXT("SuperHealthPack"), EArenaPickupType::SuperHealth },
				{ TEXT("HealthPack"), EArenaPickupType::HealthPack }, { TEXT("SuperShieldPack"), EArenaPickupType::ShieldLarge },
				{ TEXT("ShieldPack"), EArenaPickupType::ShieldSmall }, { TEXT("UDamagePack"), EArenaPickupType::DoubleDamage },
				{ TEXT("AdrenalinePickup"), EArenaPickupType::Adrenaline } };
			for (const FItem& I : Items)
			{
				if (N.Equals(I.Name, ESearchCase::IgnoreCase))
				{
					OutType = I.Type;
					OutWeapon = nullptr;
					return true;
				}
			}
			if (N.EndsWith(TEXT("AmmoPickup")))
			{
				int32 Num;
				const FWeaponMap* T = WeaponTable(Num);
				for (int32 i = 0; i < Num; ++i)
				{
					const TCHAR* A = AmmoName(T[i].UE2);
					if (A && N.Equals(A, ESearchCase::IgnoreCase))
					{
						OutType = EArenaPickupType::Ammo;
						OutWeapon = T[i].Ours();
						return true;
					}
				}
			}
			if (N.EndsWith(TEXT("Pickup")))
			{
				if (TSubclassOf<AArenaWeapon> W = OurWeapon(N.LeftChop(6)))
				{
					OutType = EArenaPickupType::Weapon;
					OutWeapon = W;
					return true;
				}
			}
		}
		return false;
	}

	FString GameClassForMode(const FString& ModeId)
	{
		static const TMap<FString, FString> M = {
			{ TEXT("DM"), TEXT("XGame.xDeathMatch") }, { TEXT("TDM"), TEXT("XGame.xTeamGame") }, { TEXT("CTF"), TEXT("XGame.xCTFGame") },
			{ TEXT("BR"), TEXT("XGame.xBombingRun") }, { TEXT("DOM"), TEXT("XGame.xDoubleDom") },
			{ TEXT("LMS"), TEXT("BonusPack.xLastManStandingGame") }, { TEXT("MUT"), TEXT("BonusPack.xMutantGame") },
			{ TEXT("INV"), TEXT("SkaarjPack.Invasion") }, { TEXT("AS"), TEXT("UT2k4Assault.ASGameInfo") },
			{ TEXT("ONS"), TEXT("Onslaught.ONSOnslaughtGame") } };
		const FString* S = M.Find(ModeId.ToUpper());
		return S ? *S : FString(TEXT("XGame.xDeathMatch"));
	}

	FUSObject* CallObjectFunction(FUSObject* Obj, const TCHAR* Name, TFunctionRef<void(FUSParms&)> Fill, TFunctionRef<void(FUSParms&)> Read)
	{
		FUSFunction* F = Obj ? USVM::FindFunction(Obj, Name) : nullptr;
		if (!F)
		{
			return nullptr;
		}
		FUSParms P(F);
		Fill(P);
		P.Call(Obj);
		Read(P);
		return Obj;
	}
}

// =============================================================================================
// Units
// =============================================================================================

FUSVector USUnits::ToUE2(const FVector& V) { return FUSVector(V * (1.0 / ArenaUE2World::Scale)); }
FVector USUnits::FromUE2(const FUSVector& V) { return V.ToFVector() * ArenaUE2World::Scale; }
FUSRotator USUnits::ToUE2(const FRotator& R)
{
	auto C = [](double Deg) { return int32(FMath::RoundToInt(Deg * 65536.0 / 360.0)) & 0xFFFF; };
	return FUSRotator(C(R.Pitch), C(R.Yaw), C(R.Roll));
}
FRotator USUnits::FromUE2(const FUSRotator& R)
{
	return FRotator(R.Pitch * (360.0 / 65536.0), R.Yaw * (360.0 / 65536.0), R.Roll * (360.0 / 65536.0));
}

// =============================================================================================
// Properties
// =============================================================================================

FUSEngineProps& USProps()
{
	static FUSEngineProps P;
	return P;
}

void FUSEngineProps::Resolve()
{
	if (bResolved)
	{
		return;
	}
	FUSRuntime& RT = FUSRuntime::Get();
	FUSClass* Actor = RT.FindClass(TEXT("Engine.Actor"));
	FUSClass* LevelC = RT.FindClass(TEXT("Engine.LevelInfo"));
	FUSClass* GameInfo = RT.FindClass(TEXT("Engine.GameInfo"));
	FUSClass* Mutator = RT.FindClass(TEXT("Engine.Mutator"));
	FUSClass* GameRules = RT.FindClass(TEXT("Engine.GameRules"));
	FUSClass* Pawn = RT.FindClass(TEXT("Engine.Pawn"));
	FUSClass* XPawn = RT.FindClass(TEXT("XGame.xPawn"));
	FUSClass* UPawn = RT.FindClass(TEXT("UnrealGame.UnrealPawn"));
	FUSClass* Controller = RT.FindClass(TEXT("Engine.Controller"));
	FUSClass* PRI = RT.FindClass(TEXT("Engine.PlayerReplicationInfo"));
	FUSClass* PV = RT.FindClass(TEXT("Engine.PhysicsVolume"));
	FUSClass* Pickup = RT.FindClass(TEXT("Engine.Pickup"));
	FUSClass* WeaponPickup = RT.FindClass(TEXT("Engine.WeaponPickup"));
	FUSClass* Weapon = RT.FindClass(TEXT("Engine.Weapon"));

	Location.Resolve(Actor, TEXT("Location")); Rotation.Resolve(Actor, TEXT("Rotation")); Velocity.Resolve(Actor, TEXT("Velocity"));
	Acceleration.Resolve(Actor, TEXT("Acceleration")); Owner.Resolve(Actor, TEXT("Owner")); Tag.Resolve(Actor, TEXT("Tag"));
	Event.Resolve(Actor, TEXT("Event")); this->Level.Resolve(Actor, TEXT("Level")); Instigator.Resolve(Actor, TEXT("Instigator"));
	Base.Resolve(Actor, TEXT("Base")); Physics.Resolve(Actor, TEXT("Physics")); bDeleteMe.Resolve(Actor, TEXT("bDeleteMe"));
	TimerRate.Resolve(Actor, TEXT("TimerRate")); TimerCounter.Resolve(Actor, TEXT("TimerCounter")); bTimerLoop.Resolve(Actor, TEXT("bTimerLoop"));
	Role.Resolve(Actor, TEXT("Role")); RemoteRole.Resolve(Actor, TEXT("RemoteRole")); CollisionRadius.Resolve(Actor, TEXT("CollisionRadius"));
	CollisionHeight.Resolve(Actor, TEXT("CollisionHeight")); bHidden.Resolve(Actor, TEXT("bHidden")); bStatic.Resolve(Actor, TEXT("bStatic"));
	bNoDelete.Resolve(Actor, TEXT("bNoDelete")); DrawScale.Resolve(Actor, TEXT("DrawScale")); PhysicsVolume.Resolve(Actor, TEXT("PhysicsVolume"));
	LifeSpan.Resolve(Actor, TEXT("LifeSpan")); bGameRelevant.Resolve(Actor, TEXT("bGameRelevant")); bScriptInitialized.Resolve(Actor, TEXT("bScriptInitialized"));

	TimeSeconds.Resolve(LevelC, TEXT("TimeSeconds")); Game.Resolve(LevelC, TEXT("Game")); ControllerList.Resolve(LevelC, TEXT("ControllerList"));
	PawnList.Resolve(LevelC, TEXT("PawnList")); NetMode.Resolve(LevelC, TEXT("NetMode")); TimeDilation.Resolve(LevelC, TEXT("TimeDilation"));
	DefaultGravity.Resolve(LevelC, TEXT("DefaultGravity")); LevelTitle.Resolve(LevelC, TEXT("Title")); GRI.Resolve(LevelC, TEXT("GRI"));
	bBegunPlay.Resolve(LevelC, TEXT("bBegunPlay"));

	BaseMutator.Resolve(GameInfo, TEXT("BaseMutator")); GameRulesModifiers.Resolve(GameInfo, TEXT("GameRulesModifiers"));
	GameReplicationInfo.Resolve(GameInfo, TEXT("GameReplicationInfo")); bTeamGame.Resolve(GameInfo, TEXT("bTeamGame"));
	GoalScore.Resolve(GameInfo, TEXT("GoalScore")); TimeLimit.Resolve(GameInfo, TEXT("TimeLimit")); MaxLives.Resolve(GameInfo, TEXT("MaxLives"));
	MutatorClass.Resolve(GameInfo, TEXT("MutatorClass")); bWeaponStay.Resolve(GameInfo, TEXT("bWeaponStay"));
	GameDifficulty.Resolve(GameInfo, TEXT("GameDifficulty")); bGameEnded.Resolve(GameInfo, TEXT("bGameEnded"));
	GameSpeed.Resolve(GameInfo, TEXT("GameSpeed"));

	NextMutator.Resolve(Mutator, TEXT("NextMutator")); MutatorFriendlyName.Resolve(Mutator, TEXT("FriendlyName"));
	NextGameRules.Resolve(GameRules, TEXT("NextGameRules"));

	Health.Resolve(Pawn, TEXT("Health")); HealthMax.Resolve(Pawn, TEXT("HealthMax")); SuperHealthMax.Resolve(Pawn, TEXT("SuperHealthMax"));
	PawnController.Resolve(Pawn, TEXT("Controller")); PawnPRI.Resolve(Pawn, TEXT("PlayerReplicationInfo")); NextPawn.Resolve(Pawn, TEXT("NextPawn"));
	JumpZ.Resolve(Pawn, TEXT("JumpZ")); GroundSpeed.Resolve(Pawn, TEXT("GroundSpeed")); AirSpeed.Resolve(Pawn, TEXT("AirSpeed"));
	WaterSpeed.Resolve(Pawn, TEXT("WaterSpeed")); AirControl.Resolve(Pawn, TEXT("AirControl")); MaxFallSpeed.Resolve(Pawn, TEXT("MaxFallSpeed"));
	DamageScaling.Resolve(Pawn, TEXT("DamageScaling"));
	if (XPawn)
	{
		MaxMultiJump.Resolve(XPawn, TEXT("MaxMultiJump")); MultiJumpBoost.Resolve(XPawn, TEXT("MultiJumpBoost"));
		MultiJumpRemaining.Resolve(XPawn, TEXT("MultiJumpRemaining"));
	}
	if (UPawn)
	{
		RequiredEquipment.Resolve(UPawn, TEXT("RequiredEquipment"));
		bCanDoubleJump.Resolve(UPawn, TEXT("bCanDoubleJump"));
	}
	HeadScale.Resolve(Pawn, TEXT("HeadScale"));
	ShieldStrength.Resolve(Pawn, TEXT("ShieldStrength"));

	ControllerPawn.Resolve(Controller, TEXT("Pawn")); ControllerPRI.Resolve(Controller, TEXT("PlayerReplicationInfo"));
	NextController.Resolve(Controller, TEXT("NextController")); bIsPlayer.Resolve(Controller, TEXT("bIsPlayer"));
	bAdrenalineEnabled.Resolve(Controller, TEXT("bAdrenalineEnabled")); Adrenaline.Resolve(Controller, TEXT("Adrenaline"));
	PlayerReplicationInfoClass.Resolve(Controller, TEXT("PlayerReplicationInfoClass"));

	PlayerName.Resolve(PRI, TEXT("PlayerName")); Score.Resolve(PRI, TEXT("Score")); Deaths.Resolve(PRI, TEXT("Deaths"));
	PRITeam.Resolve(PRI, TEXT("Team")); PlayerID.Resolve(PRI, TEXT("PlayerID")); bBot.Resolve(PRI, TEXT("bBot")); Kills.Resolve(PRI, TEXT("Kills"));
	Gravity.Resolve(PV, TEXT("Gravity"));
	InventoryType.Resolve(Pickup, TEXT("InventoryType"));
	if (Weapon)
	{
		PickupClass.Resolve(Weapon, TEXT("PickupClass"));
	}
	(void)WeaponPickup;
	bResolved = true;
}

// =============================================================================================
// FUSGameWorld
// =============================================================================================

FUSGameWorld::FUSGameWorld(UWorld* InWorld, AArenaGameMode* InGameMode)
	: World(InWorld), GameMode(InGameMode)
{
}

FUSGameWorld::~FUSGameWorld()
{
	for (TPair<FUSObject*, FUSActorLink*>& KV : Links)
	{
		delete KV.Value;
	}
	Links.Reset();
}

FUSGameWorld* FUSGameWorld::Find(const UWorld* InWorld)
{
	if (!InWorld)
	{
		return nullptr;
	}
	const TUniquePtr<FUSGameWorld>* W = Worlds().Find(TWeakObjectPtr<UWorld>(const_cast<UWorld*>(InWorld)));
	return W ? W->Get() : nullptr;
}

FUSGameWorld* FUSGameWorld::GetOrCreate(UWorld* InWorld, AArenaGameMode* GM, const FString& ModeId)
{
	if (FUSGameWorld* W = Find(InWorld))
	{
		return W;
	}
	if (!GCleanupRegistered)
	{
		GCleanupRegistered = true;
		FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* W, bool, bool)
		{
			if (TUniquePtr<FUSGameWorld>* Found = Worlds().Find(TWeakObjectPtr<UWorld>(W)))
			{
				if (Found->IsValid())
				{
					const FUSGameWorld& G = **Found;
					UE_LOG(LogUScript, Log, TEXT("UScript: world ended (%llu ticks, %.2f ms script in total, %.4f ms per tick; ModifyPlayer %d, NetDamage %d (health changed %d), ScoreKill %d, Timer %d, script health regen %d)"),
						G.TickCount, G.ScriptSeconds * 1000.0, G.TickCount ? G.ScriptSeconds * 1000.0 / G.TickCount : 0.0,
						G.Stats.ModifyPlayer, G.Stats.NetDamage, G.Stats.HealthChangedByRules, G.Stats.ScoreKill, G.Stats.Timers, G.Stats.HealthPushed);
				}
				Worlds().Remove(TWeakObjectPtr<UWorld>(W));
			}
		});
	}
	TUniquePtr<FUSGameWorld> New = MakeUnique<FUSGameWorld>(InWorld, GM);
	FString Err;
	if (!New->Init(ModeId, Err))
	{
		UE_LOG(LogUScript, Warning, TEXT("UScript: script world not available: %s"), *Err);
		return nullptr;
	}
	FUSGameWorld* Raw = New.Get();
	Worlds().Add(TWeakObjectPtr<UWorld>(InWorld), MoveTemp(New));
	return Raw;
}

FUSClass* FUSGameWorld::LoadClass(const TCHAR* Path)
{
	FString Err;
	FUSClass* C = FUSRuntime::Get().FindClass(Path, &Err);
	if (!C)
	{
		UE_LOG(LogUScript, Warning, TEXT("UScript: %s"), *Err);
	}
	return C;
}

bool FUSGameWorld::Init(const FString& ModeId, FString& OutError)
{
	if (!ArenaUScript::EnsureRuntime(&OutError))
	{
		return false;
	}
	FUSEngineProps& P = USProps();
	P.Resolve();
	FUSClass* LevelClass = LoadClass(TEXT("Engine.LevelInfo"));
	FUSClass* GameClass = LoadClass(*GameClassForMode(ModeId));
	if (!GameClass)
	{
		GameClass = LoadClass(TEXT("XGame.xDeathMatch"));
	}
	if (!LevelClass || !GameClass)
	{
		OutError = TEXT("stock Engine/XGame classes missing");
		return false;
	}
	Level = ConstructObject(LevelClass, nullptr, TEXT("LevelInfo0"));
	P.Level.Set<FUSObject*>(Level, Level);
	P.Role.Set<uint8>(Level, 4);
	P.NetMode.Set<uint8>(Level, 0);
	P.TimeSeconds.Set<float>(Level, 0.f);
	// the UE gravity of UE2's stock -950 (not the level's: a map's own gravity, e.g. BR-Skyline's -600, must not scale
	// a mutator's absolute value such as MutLowGrav's -300)
	BaseWorldGravityZ = UPhysicsSettings::Get()->DefaultGravityZ;

	Game = ConstructObject(GameClass, Level, NAME_None);
	P.Level.Set<FUSObject*>(Game, Level);
	P.Role.Set<uint8>(Game, 4);
	P.Game.Set<FUSObject*>(Level, Game);
	if (AArenaGameMode* GM = GameMode.Get())
	{
		P.GoalScore.Set<int32>(Game, GM->GetGoalScore());
		P.TimeLimit.Set<int32>(Game, GM->GetTimeLimitMinutes());
		P.bTeamGame.Set<uint32>(Game, GM->IsTeamGame() ? 1u : 0u);
		P.GameDifficulty.Set<float>(Game, float(GM->GetBotSkill()));
	}
	// GameReplicationInfo
	if (FUSProperty* GRIClassProp = Game->FindProperty(TEXT("GameReplicationInfoClass")))
	{
		FUSClass* GRIClass = *reinterpret_cast<FUSClass**>(Game->Data + GRIClassProp->Offset);
		if (GRIClass && GRIClass->EnsureLoaded())
		{
			GRI = ConstructObject(GRIClass, Level, NAME_None);
			P.Level.Set<FUSObject*>(GRI, Level);
			P.GameReplicationInfo.Set<FUSObject*>(Game, GRI);
			P.GRI.Set<FUSObject*>(Level, GRI);
		}
	}
	if (FUSClass* DPV = LoadClass(TEXT("Engine.DefaultPhysicsVolume")))
	{
		DefaultPhysicsVolume = ConstructObject(DPV, Level, NAME_None);
		P.Level.Set<FUSObject*>(DefaultPhysicsVolume, Level);
		FUSActorLink* L = new FUSActorLink();
		L->Kind = FUSActorLink::EKind::PhysicsVolume;
		if (FUSVector* G = P.Gravity.Ptr<FUSVector>(DefaultPhysicsVolume))
		{
			L->LastGravityZ = G->Z;
		}
		Links.Add(DefaultPhysicsVolume, L);
		DefaultPhysicsVolume->Link = L;
	}
	bReady = true;

	// UT2004 GameInfo.InitGame: BaseMutator = Spawn(MutatorClass) (UnrealGame.DMMutator for the stock games)
	FString MutClassName = P.MutatorClass.Get<FString>(Game);
	if (MutClassName.IsEmpty())
	{
		MutClassName = TEXT("UnrealGame.DMMutator");
	}
	if (FUSClass* MC = LoadClass(*MutClassName))
	{
		FUSObject* Base = SpawnActor(MC, nullptr, NAME_None, nullptr, nullptr, Game);
		P.BaseMutator.Set<FUSObject*>(Game, Base);
	}
	UE_LOG(LogUScript, Log, TEXT("UScript: script world for %s (%s, base mutator %s)"), *ModeId, *GameClass->GetPathName(), *MutClassName);
	return true;
}

bool FUSGameWorld::AddMutator(const FString& ClassPath)
{
	if (!Game || MutatorNames.Contains(ClassPath))
	{
		return false;
	}
	FString Reason;
	FUSClass* C = FUSRuntime::Get().FindClass(ClassPath, &Reason);
	if (!C)
	{
		UE_LOG(LogUScript, Warning, TEXT("UScript: mutator %s not loaded: %s"), *ClassPath, *Reason);
		return false;
	}
	const double S0 = FPlatformTime::Seconds();
	FUSFunction* F = USVM::FindFunction(Game, TEXT("AddMutator"));
	if (!F)
	{
		return false;
	}
	FUSParms Parms(F);
	Parms.Str(ClassPath).Bool(true);
	Parms.Call(Game);
	ScriptSeconds += FPlatformTime::Seconds() - S0;
	// verify it is in the chain
	bool bFound = false;
	int32 Guard = 0;
	for (FUSObject* M = USProps().BaseMutator.Get<FUSObject*>(Game); M && Guard < 64; M = USProps().NextMutator.Get<FUSObject*>(M), ++Guard)
	{
		if (M->Class == C)
		{
			bFound = true;
		}
	}
	if (bFound)
	{
		MutatorNames.Add(ClassPath);
		UE_LOG(LogUScript, Log, TEXT("UScript: mutator %s active (%s)"), *ClassPath,
			*USProps().MutatorFriendlyName.Get<FString>(C->GetDefaultObject()));
	}
	else
	{
		// e.g. MutVampire installs a GameRules and destroys itself, as in UT2004
		UE_LOG(LogUScript, Log, TEXT("UScript: mutator %s ran (not in the mutator chain afterwards)"), *ClassPath);
	}
	MutatorNames.AddUnique(ClassPath);
	return true;
}

void FUSGameWorld::CallEvent(FUSObject* Obj, const TCHAR* EventName)
{
	if (!Obj || Obj->IsPendingKill())
	{
		return;
	}
	if (FUSFunction* F = USVM::FindFunction(Obj, EventName))
	{
		USVM::ProcessEvent(Obj, F, nullptr);
	}
}

void FUSGameWorld::BeginPlayEvents(FUSObject* A)
{
	CallEvent(A, TEXT("Spawned"));
	if (A->IsPendingKill()) { return; }
	CallEvent(A, TEXT("PreBeginPlay"));
	if (A->IsPendingKill()) { return; }
	CallEvent(A, TEXT("BeginPlay"));
	if (A->IsPendingKill()) { return; }
	CallEvent(A, TEXT("PostBeginPlay"));
	if (A->IsPendingKill()) { return; }
	CallEvent(A, TEXT("SetInitialState"));
	if (A->IsPendingKill()) { return; }
	CallEvent(A, TEXT("PostNetBeginPlay"));
}

FUSObject* FUSGameWorld::SpawnActor(FUSClass* Class, FUSObject* Owner, FName Tag, const FUSVector* Location, const FUSRotator* Rotation, FUSObject* Spawner)
{
	FUSRuntime& RT = FUSRuntime::Get();
	if (!Class || !Class->EnsureLoaded() || (Class->ClassFlags & USFlags::CLASS_Abstract) || !RT.ActorClass || !Class->IsChildOf(RT.ActorClass))
	{
		return nullptr;
	}
	FUSEngineProps& P = USProps();
	FUSObject* A = ConstructObject(Class, Level, NAME_None);
	if (!A)
	{
		return nullptr;
	}
	P.Level.Set<FUSObject*>(A, Level);
	P.Owner.Set<FUSObject*>(A, Owner);
	P.Tag.Set<FName>(A, Tag != NAME_None ? Tag : Class->Name);
	if (Location)
	{
		P.Location.Set<FUSVector>(A, *Location);
	}
	if (Rotation)
	{
		P.Rotation.Set<FUSRotator>(A, *Rotation);
	}
	if (Spawner)
	{
		P.Instigator.Set<FUSObject*>(A, P.Instigator.Get<FUSObject*>(Spawner));
	}
	P.Role.Set<uint8>(A, 4);
	if (DefaultPhysicsVolume)
	{
		P.PhysicsVolume.Set<FUSObject*>(A, DefaultPhysicsVolume);
	}
	RecentSpawns.Add(A);
	// Pickups, weapons and other inventory are represented by our C++ actors: no script begin-play.
	static FUSClass* PickupClass = RT.FindClass(TEXT("Engine.Pickup"));
	static FUSClass* InventoryClass = RT.FindClass(TEXT("Engine.Inventory"));
	if ((PickupClass && Class->IsChildOf(PickupClass)) || (InventoryClass && Class->IsChildOf(InventoryClass)))
	{
		return A;
	}
	BeginPlayEvents(A);
	if (!A->IsPendingKill())
	{
		P.bScriptInitialized.Set<uint32>(A, 1u);
	}
	return A->IsPendingKill() ? nullptr : A;
}

bool FUSGameWorld::DestroyActor(FUSObject* A)
{
	if (!A || A->IsPendingKill())
	{
		return true;
	}
	FUSEngineProps& P = USProps();
	if (P.bStatic.Get<uint32>(A) || P.bNoDelete.Get<uint32>(A))
	{
		return false;
	}
	P.bDeleteMe.Set<uint32>(A, 1u);
	// proxies mirror game actors: their UT2004 Destroyed() (PawnDied, inventory cleanup ...) is the game's job
	if (!A->Link)
	{
		CallEvent(A, TEXT("Destroyed"));
	}
	if (FUSActorLink* L = GetLink(A))
	{
		if (L->Kind == FUSActorLink::EKind::Controller)
		{
			LinkController(A, false);
		}
		else if (L->Kind == FUSActorLink::EKind::Pawn)
		{
			LinkPawn(A, false);
		}
	}
	RemoveProxy(A);
	MarkPendingKill(A);
	return true;
}

FUSObject* FUSGameWorld::MakeProxy(FUSClass* Class, AActor* Actor, FUSActorLink::EKind Kind)
{
	if (!Class)
	{
		return nullptr;
	}
	FUSObject* O = ConstructObject(Class, Level, NAME_None);
	if (!O)
	{
		return nullptr;
	}
	FUSEngineProps& P = USProps();
	P.Level.Set<FUSObject*>(O, Level);
	P.Role.Set<uint8>(O, 4);
	P.Tag.Set<FName>(O, Class->Name);
	if (DefaultPhysicsVolume)
	{
		P.PhysicsVolume.Set<FUSObject*>(O, DefaultPhysicsVolume);
	}
	FUSActorLink* L = new FUSActorLink();
	L->Kind = Kind;
	L->Actor = Actor;
	Links.Add(O, L);
	O->Link = L;
	if (Actor)
	{
		Proxies.Add(Actor, O);
	}
	return O;
}

void FUSGameWorld::RemoveProxy(FUSObject* Obj)
{
	FUSActorLink* L = nullptr;
	if (Links.RemoveAndCopyValue(Obj, L) && L)
	{
		if (L->Actor.IsValid())
		{
			Proxies.Remove(L->Actor);
		}
		else
		{
			for (auto It = Proxies.CreateIterator(); It; ++It)
			{
				if (It.Value() == Obj)
				{
					It.RemoveCurrent();
				}
			}
		}
		delete L;
	}
	Obj->Link = nullptr;
}

FUSActorLink* FUSGameWorld::GetLink(FUSObject* Obj) const
{
	FUSActorLink* const* L = Obj ? Links.Find(Obj) : nullptr;
	return L ? *L : nullptr;
}

AActor* FUSGameWorld::GetLinkedActor(FUSObject* Obj) const
{
	FUSActorLink* L = GetLink(Obj);
	return L ? L->Actor.Get() : nullptr;
}

void FUSGameWorld::LinkController(FUSObject* C, bool bAdd)
{
	FUSEngineProps& P = USProps();
	FUSObject** Head = P.ControllerList.Ptr<FUSObject*>(Level);
	if (!Head)
	{
		return;
	}
	// unlink first (also used by add to avoid duplicates)
	FUSObject** Link = Head;
	for (int32 Guard = 0; *Link && Guard < 1024; ++Guard)
	{
		if (*Link == C)
		{
			*Link = P.NextController.Get<FUSObject*>(C);
			break;
		}
		Link = P.NextController.Ptr<FUSObject*>(*Link);
		if (!Link)
		{
			break;
		}
	}
	if (bAdd)
	{
		P.NextController.Set<FUSObject*>(C, *Head);
		*Head = C;
	}
}

void FUSGameWorld::LinkPawn(FUSObject* Pawn, bool bAdd)
{
	FUSEngineProps& P = USProps();
	FUSObject** Head = P.PawnList.Ptr<FUSObject*>(Level);
	if (!Head || !P.NextPawn.Prop)
	{
		return;
	}
	FUSObject** Link = Head;
	for (int32 Guard = 0; *Link && Guard < 1024; ++Guard)
	{
		if (*Link == Pawn)
		{
			*Link = P.NextPawn.Get<FUSObject*>(Pawn);
			break;
		}
		Link = P.NextPawn.Ptr<FUSObject*>(*Link);
		if (!Link)
		{
			break;
		}
	}
	if (bAdd)
	{
		P.NextPawn.Set<FUSObject*>(Pawn, *Head);
		*Head = Pawn;
	}
}

FUSObject* FUSGameWorld::ProxyForPlayerState(APlayerState* PS, bool bCreate)
{
	if (!PS)
	{
		return nullptr;
	}
	if (FUSObject** Found = Proxies.Find(PS))
	{
		return *Found;
	}
	if (!bCreate)
	{
		return nullptr;
	}
	static FUSClass* PRIClass = nullptr;
	if (!PRIClass)
	{
		PRIClass = LoadClass(TEXT("XGame.xPlayerReplicationInfo"));
	}
	FUSObject* O = MakeProxy(PRIClass, PS, FUSActorLink::EKind::PlayerState);
	if (O)
	{
		USProps().PlayerID.Set<int32>(O, PS->GetPlayerId());
		Pull(O);
	}
	return O;
}

FUSObject* FUSGameWorld::ProxyForController(AController* C, bool bCreate)
{
	if (!C)
	{
		return nullptr;
	}
	if (FUSObject** Found = Proxies.Find(C))
	{
		return *Found;
	}
	if (!bCreate)
	{
		return nullptr;
	}
	static FUSClass* PlayerClass = nullptr;
	static FUSClass* BotClass = nullptr;
	if (!PlayerClass)
	{
		PlayerClass = LoadClass(TEXT("XGame.xPlayer"));
		BotClass = LoadClass(TEXT("XGame.xBot"));
	}
	const bool bHuman = C->IsA<APlayerController>();
	FUSObject* O = MakeProxy(bHuman ? PlayerClass : BotClass, C, FUSActorLink::EKind::Controller);
	if (!O)
	{
		return nullptr;
	}
	FUSEngineProps& P = USProps();
	P.bIsPlayer.Set<uint32>(O, 1u);
	FUSObject* PRI = ProxyForPlayerState(C->PlayerState);
	P.ControllerPRI.Set<FUSObject*>(O, PRI);
	if (PRI)
	{
		P.bBot.Set<uint32>(PRI, bHuman ? 0u : 1u);
	}
	LinkController(O, true);
	Pull(O);
	return O;
}

FUSObject* FUSGameWorld::ProxyForPawn(APawn* Pawn, bool bCreate)
{
	if (!Pawn)
	{
		return nullptr;
	}
	if (FUSObject** Found = Proxies.Find(Pawn))
	{
		return *Found;
	}
	if (!bCreate)
	{
		return nullptr;
	}
	static FUSClass* PawnClass = nullptr;
	if (!PawnClass)
	{
		PawnClass = LoadClass(TEXT("XGame.xPawn"));
	}
	FUSObject* O = MakeProxy(PawnClass, Pawn, FUSActorLink::EKind::Pawn);
	if (!O)
	{
		return nullptr;
	}
	FUSEngineProps& P = USProps();
	FUSActorLink* L = GetLink(O);
	L->LastMaxMultiJump = P.MaxMultiJump.Get<int32>(O);
	L->LastJumpZ = P.JumpZ.Get<float>(O);
	L->LastGroundSpeed = P.GroundSpeed.Get<float>(O);
	L->LastHeadScale = P.HeadScale.Get<float>(O, 1.f);
	LinkPawn(O, true);
	if (AController* C = Pawn->GetController())
	{
		FUSObject* CP = ProxyForController(C);
		P.PawnController.Set<FUSObject*>(O, CP);
		P.ControllerPawn.Set<FUSObject*>(CP, O);
		FUSObject* PRI = P.ControllerPRI.Get<FUSObject*>(CP);
		P.PawnPRI.Set<FUSObject*>(O, PRI);
		P.Instigator.Set<FUSObject*>(O, O);
	}
	Pull(O);
	return O;
}

void FUSGameWorld::Pull(FUSObject* Obj)
{
	FUSActorLink* L = GetLink(Obj);
	if (!L)
	{
		return;
	}
	FUSEngineProps& P = USProps();
	AActor* A = L->Actor.Get();
	switch (L->Kind)
	{
	case FUSActorLink::EKind::Pawn:
	{
		AArenaCharacter* Ch = Cast<AArenaCharacter>(A);
		if (!Ch || Ch->IsDead())
		{
			DestroyActor(Obj);
			return;
		}
		P.Location.Set<FUSVector>(Obj, USUnits::ToUE2(Ch->GetActorLocation()));
		P.Rotation.Set<FUSRotator>(Obj, USUnits::ToUE2(Ch->GetActorRotation()));
		P.Velocity.Set<FUSVector>(Obj, USUnits::ToUE2(Ch->GetVelocity()));
		const int32 H = FMath::CeilToInt(Ch->Health);
		P.Health.Set<int32>(Obj, H);
		L->LastHealth = H;
		P.ShieldStrength.Set<float>(Obj, Ch->Shield);
		L->LastShield = Ch->Shield;
		break;
	}
	case FUSActorLink::EKind::Controller:
	{
		AController* C = Cast<AController>(A);
		if (!C)
		{
			DestroyActor(Obj);
			return;
		}
		AArenaCharacter* Ch = Cast<AArenaCharacter>(C->GetPawn());
		FUSObject* PawnProxy = (Ch && !Ch->IsDead()) ? ProxyForPawn(Ch, false) : nullptr;
		P.ControllerPawn.Set<FUSObject*>(Obj, PawnProxy);
		if (Ch)
		{
			P.Adrenaline.Set<float>(Obj, Ch->Adrenaline);
		}
		break;
	}
	case FUSActorLink::EKind::PlayerState:
	{
		AArenaPlayerState* PS = Cast<AArenaPlayerState>(A);
		if (!PS)
		{
			DestroyActor(Obj);
			return;
		}
		P.PlayerName.Set<FString>(Obj, PS->GetPlayerName());
		P.Score.Set<float>(Obj, PS->GetScore());
		L->LastScore = PS->GetScore();
		P.Deaths.Set<float>(Obj, float(PS->Deaths));
		P.Kills.Set<int32>(Obj, PS->Kills);
		break;
	}
	default:
		break;
	}
}

void FUSGameWorld::Push(FUSObject* Obj)
{
	FUSActorLink* L = GetLink(Obj);
	if (!L || Obj->IsPendingKill())
	{
		return;
	}
	FUSEngineProps& P = USProps();
	AActor* A = L->Actor.Get();
	switch (L->Kind)
	{
	case FUSActorLink::EKind::Pawn:
	{
		AArenaCharacter* Ch = Cast<AArenaCharacter>(A);
		if (!Ch || Ch->IsDead())
		{
			return;
		}
		const int32 H = P.Health.Get<int32>(Obj);
		if (H != L->LastHealth)
		{
			Ch->Health = float(FMath::Max(H, 1));
			L->LastHealth = H;
			++Stats.HealthPushed;
		}
		UCharacterMovementComponent* Move = Ch->GetCharacterMovement();
		const FUSObject* Def = Obj->Class->DefaultObject;
		const int32 MJ = P.MaxMultiJump.Get<int32>(Obj);
		if (P.MaxMultiJump.Prop && MJ != L->LastMaxMultiJump)
		{
			Ch->JumpMaxCount = FMath::Clamp(1 + MJ, 1, 16);
			L->LastMaxMultiJump = MJ;
		}
		const float JZ = P.JumpZ.Get<float>(Obj);
		if (Move && JZ != L->LastJumpZ)
		{
			const float DefJZ = P.JumpZ.Get<float>(const_cast<FUSObject*>(Def), 340.f);
			Move->JumpZVelocity = ArenaTuning::JumpZ * (DefJZ > 0.f ? JZ / DefJZ : 1.f);
			L->LastJumpZ = JZ;
		}
		const float GS = P.GroundSpeed.Get<float>(Obj);
		if (Move && GS != L->LastGroundSpeed)
		{
			const float DefGS = P.GroundSpeed.Get<float>(const_cast<FUSObject*>(Def), 440.f);
			Move->MaxWalkSpeed = ArenaTuning::GroundSpeed * (DefGS > 0.f ? GS / DefGS : 1.f);
			L->LastGroundSpeed = GS;
		}
		const float Sh = P.ShieldStrength.Get<float>(Obj);
		if (Sh != L->LastShield)
		{
			Ch->Shield = FMath::Max(0.f, Sh);
			L->LastShield = Sh;
		}
		const float HS = P.HeadScale.Get<float>(Obj, 1.f);
		if (HS != L->LastHeadScale)
		{
			L->LastHeadScale = HS;
			extern void USApplyHeadScale(AArenaCharacter * Ch, float Scale);
			USApplyHeadScale(Ch, HS);
		}
		break;
	}
	case FUSActorLink::EKind::PlayerState:
	{
		AArenaPlayerState* PS = Cast<AArenaPlayerState>(A);
		const float S = P.Score.Get<float>(Obj);
		if (PS && S != L->LastScore)
		{
			PS->SetScore(S);
			L->LastScore = S;
		}
		break;
	}
	case FUSActorLink::EKind::ScriptActor:
	{
		const bool bHidden = P.bHidden.Get<uint32>(Obj) != 0;
		if (bHidden != L->bLastHidden)
		{
			L->bLastHidden = bHidden;
			if (AArenaScriptActor* Body = Cast<AArenaScriptActor>(A))
			{
				Body->SetScriptHidden(bHidden);
			}
			static int32 Shown = 0;
			if (!bHidden && Shown++ < 3)
			{
				UE_LOG(LogUScript, Log, TEXT("UScript: %s visible again (state %s)"), *Obj->GetName(),
					Obj->StateFrame && Obj->StateFrame->StateNode ? *Obj->StateFrame->StateNode->GetName() : TEXT("-"));
			}
		}
		const FUSVector Loc = P.Location.Get<FUSVector>(Obj);
		if (A && (Loc.X != L->LastLocation.X || Loc.Y != L->LastLocation.Y || Loc.Z != L->LastLocation.Z))
		{
			L->LastLocation = Loc;
			A->SetActorLocation(USUnits::FromUE2(Loc));
		}
		break;
	}
	case FUSActorLink::EKind::PhysicsVolume:
	{
		const FUSVector* G = P.Gravity.Ptr<FUSVector>(Obj);
		if (G && G->Z != L->LastGravityZ && Obj == DefaultPhysicsVolume)
		{
			L->LastGravityZ = G->Z;
			if (AWorldSettings* WS = World.IsValid() ? World->GetWorldSettings() : nullptr)
			{
				const float Base = BaseWorldGravityZ != 0.f ? BaseWorldGravityZ : -980.f;
				WS->bGlobalGravitySet = true;
				WS->GlobalGravityZ = Base * (G->Z / ArenaUE2World::UE2DefaultGravity);
				// a map's own gravity (bWorldGravitySet, e.g. BR-Skyline's DefaultPhysicsVolume) would hide GlobalGravityZ
				if (WS->bWorldGravitySet)
				{
					WS->WorldGravityZ = WS->GlobalGravityZ;
				}
				UE_LOG(LogUScript, Log, TEXT("UScript: world gravity %.0f (UE2 %.0f)"), WS->GlobalGravityZ, G->Z);
			}
		}
		break;
	}
	default:
		break;
	}
}

void FUSGameWorld::PullAll()
{
	TArray<FUSObject*> Keys;
	Links.GetKeys(Keys);
	for (FUSObject* O : Keys)
	{
		if (!O->IsPendingKill())
		{
			Pull(O);
		}
	}
}

void FUSGameWorld::PushAll()
{
	// Level.TimeDilation (GameInfo.SetGameSpeed): UT2004 runs at 1.1 by default
	if (const float* TD = USProps().TimeDilation.Ptr<float>(Level))
	{
		if (LastTimeDilation == 0.f)
		{
			LastTimeDilation = *TD;
			DefaultTimeDilation = *TD > 0.f ? *TD : 1.1f;
		}
		else if (*TD != LastTimeDilation && *TD > 0.f)
		{
			LastTimeDilation = *TD;
			if (AWorldSettings* WS = World.IsValid() ? World->GetWorldSettings() : nullptr)
			{
				WS->SetTimeDilation(*TD / DefaultTimeDilation);
				UE_LOG(LogUScript, Log, TEXT("UScript: game speed %.2f (Level.TimeDilation %.2f)"), *TD / DefaultTimeDilation, *TD);
			}
		}
	}
	TArray<FUSObject*> Keys;
	Links.GetKeys(Keys);
	for (FUSObject* O : Keys)
	{
		Push(O);
	}
}

TArray<FUSObject*> FUSGameWorld::GetActors() const
{
	TArray<FUSObject*> Out;
	for (FUSObject* O : Objects)
	{
		if ((O->Flags & USF_Actor) && !O->IsPendingKill())
		{
			Out.Add(O);
		}
	}
	return Out;
}

void FUSGameWorld::Tick(float DeltaTime)
{
	if (!bReady)
	{
		return;
	}
	const double S0 = FPlatformTime::Seconds();
	++TickCount;
	FUSEngineProps& P = USProps();
	PullAll();
	if (float* T = P.TimeSeconds.Ptr<float>(Level))
	{
		*T += DeltaTime;
		TimeSeconds = *T;
	}
	// script actors only: proxies mirror game actors and do not run their own UT2004 behaviour
	TArray<FUSObject*> Actors = GetActors();
	for (FUSObject* A : Actors)
	{
		if (A->IsPendingKill() || A == Level || A == Game || A == GRI)
		{
			continue;
		}
		if (A->Link && !static_cast<FUSActorLink*>(A->Link)->bRunsScript)
		{
			continue;
		}
		float* Rate = P.TimerRate.Ptr<float>(A);
		float* Counter = P.TimerCounter.Ptr<float>(A);
		if (Rate && Counter && *Rate > 0.f)
		{
			*Counter += DeltaTime;
			if (*Counter >= *Rate)
			{
				const int32 Ticks = FMath::Max(1, int32(*Counter / *Rate));
				*Counter -= *Rate * Ticks;
				if (!P.bTimerLoop.Get<uint32>(A))
				{
					*Rate = 0.f;
				}
				CallEvent(A, TEXT("Timer"));
				++Stats.Timers;
			}
		}
		if (A->IsPendingKill())
		{
			continue;
		}
		if (A->Class->bHasTick)
		{
			if (FUSFunction* F = USVM::FindFunction(A, TEXT("Tick")))
			{
				FUSParms Parms(F);
				Parms.Float(DeltaTime);
				Parms.Call(A);
			}
		}
		USVM::ProcessState(A, DeltaTime);
		if (float* Life = P.LifeSpan.Ptr<float>(A))
		{
			if (*Life > 0.f)
			{
				*Life -= DeltaTime;
				if (*Life <= 0.f)
				{
					*Life = 0.f;
					CallEvent(A, TEXT("Expired"));
					DestroyActor(A);
				}
			}
		}
	}
	PushAll();
	CleanupDestroyed();
	ScriptSeconds += FPlatformTime::Seconds() - S0;

	// test hook: -UScriptShootTest=<seconds> shoots every map script actor once with the shock beam
	static float ShootAt = -2.f;
	if (ShootAt == -2.f)
	{
		ShootAt = -1.f;
		FParse::Value(FCommandLine::Get(), TEXT("UScriptShootTest="), ShootAt);
	}
	if (ShootAt > 0.f && TimeSeconds >= ShootAt)
	{
		ShootAt = -1.f;
		extern void USShootTest(FUSGameWorld* W);
		USShootTest(this);
	}
}

// =============================================================================================
// Hooks (called by the game)
// =============================================================================================

void USApplyHeadScale(AArenaCharacter* Ch, float Scale)
{
	// xPawn.SetHeadScale: HeadScale, and SetBoneScale(4, HeadScale, 'head') (the character's anim instance scales the head bone)
	if (Ch)
	{
		Ch->HeadScale = FMath::Clamp(Scale, 0.1f, 8.f);
	}
	UE_LOG(LogUScript, Log, TEXT("UScript: %s head scale %.2f"), Ch ? *Ch->GetName() : TEXT("?"), Scale);
}

namespace
{
	FUSObject* BaseMutatorOf(FUSGameWorld* W)
	{
		return (W && W->Game) ? USProps().BaseMutator.Get<FUSObject*>(W->Game) : nullptr;
	}

	FUSObject* GameRulesOf(FUSGameWorld* W)
	{
		return (W && W->Game) ? USProps().GameRulesModifiers.Get<FUSObject*>(W->Game) : nullptr;
	}

	/** Mutator.CheckRelevance(Other) on the base mutator. */
	bool CheckRelevance(FUSGameWorld* W, FUSObject* Other)
	{
		FUSObject* Base = BaseMutatorOf(W);
		FUSFunction* F = Base ? USVM::FindFunction(Base, TEXT("CheckRelevance")) : nullptr;
		if (!F)
		{
			return true;
		}
		FUSParms P(F);
		P.Object(Other);
		P.Call(Base);
		return P.ReturnBool();
	}

	int32 GetLinkHealth(FUSGameWorld* W, FUSObject* Proxy)
	{
		FUSActorLink* L = W->GetLink(Proxy);
		return L ? L->LastHealth : 0;
	}

	/** True when the map package defines script classes (custom map scripts). */
	bool MapHasScriptClasses(const FString& MapName)
	{
		FString Err;
		if (!ArenaUScript::EnsureRuntime(&Err))
		{
			return false;
		}
		FUSLinker* L = FUSRuntime::Get().LoadPackage(FName(*MapName));
		if (!L)
		{
			return false;
		}
		for (int32 Ei = 0; Ei < L->NumExports(); ++Ei)
		{
			if (L->IsTopLevelClass(Ei))
			{
				return true;
			}
		}
		return false;
	}

	struct FScriptTimer
	{
		FUSGameWorld* W;
		double S0;
		explicit FScriptTimer(FUSGameWorld* In) : W(In), S0(FPlatformTime::Seconds()) {}
		~FScriptTimer() { W->ScriptSeconds += FPlatformTime::Seconds() - S0; }
	};
}

void ArenaUScriptHooks::InitGame(AArenaGameMode* GM, const FString& Options)
{
	if (!GM)
	{
		return;
	}
	FString Mutators = UGameplayStatics::ParseOption(Options, TEXT("Mutator"));
	if (Mutators.IsEmpty())
	{
		return;
	}
	FUSGameWorld* W = FUSGameWorld::GetOrCreate(GM->GetWorld(), GM, GM->GetModeId());
	if (!W)
	{
		return;
	}
	W->MapName = GM->GetLayoutId();
	W->MapName.RemoveFromStart(TEXT("Imported:"));
	TArray<FString> Names;
	Mutators.ParseIntoArray(Names, TEXT(","));
	for (FString& N : Names)
	{
		N.TrimStartAndEndInline();
		if (!N.IsEmpty())
		{
			W->AddMutator(N);
		}
	}
	FString Chain, RulesChain;
	int32 Guard = 0;
	for (FUSObject* M = BaseMutatorOf(W); M && Guard < 64; M = USProps().NextMutator.Get<FUSObject*>(M), ++Guard)
	{
		Chain += M->Class->GetName() + TEXT(" ");
	}
	Guard = 0;
	for (FUSObject* R = GameRulesOf(W); R && Guard < 64; R = USProps().NextGameRules.Get<FUSObject*>(R), ++Guard)
	{
		RulesChain += R->Class->GetName() + TEXT(" ");
	}
	UE_LOG(LogUScript, Log, TEXT("UScript: mutators [%s] game rules [%s]"), *Chain.TrimEnd(), *RulesChain.TrimEnd());
}

void ArenaUScriptHooks::LevelBuilt(AArenaGameMode* GM)
{
	if (!GM)
	{
		return;
	}
	FString MapName = GM->GetLayoutId();
	MapName.RemoveFromStart(TEXT("Imported:"));
	FUSGameWorld* W = FUSGameWorld::Find(GM->GetWorld());
	if (!W && !MapName.IsEmpty() && MapHasScriptClasses(MapName))
	{
		W = FUSGameWorld::GetOrCreate(GM->GetWorld(), GM, GM->GetModeId());
	}
	if (!W)
	{
		return;
	}
	W->MapName = MapName;
	FScriptTimer T(W);
	W->SpawnMapScriptActors(MapName);
	if (W->DefaultPhysicsVolume)
	{
		CheckRelevance(W, W->DefaultPhysicsVolume);
		W->Push(W->DefaultPhysicsVolume);
	}
}

bool ArenaUScriptHooks::FilterPickup(UWorld* World, const FVector& Location, EArenaPickupType& Type, TSubclassOf<AArenaWeapon>& WeaponClass)
{
	FUSGameWorld* W = FUSGameWorld::Find(World);
	if (!W)
	{
		return true;
	}
	const FString ClassPath = UE2PickupClass(Type, WeaponClass);
	FUSClass* C = ClassPath.IsEmpty() ? nullptr : FUSRuntime::Get().FindClass(ClassPath);
	if (!C)
	{
		return true;
	}
	FScriptTimer T(W);
	const FUSVector Loc = USUnits::ToUE2(Location);
	W->RecentSpawns.Reset();
	FUSObject* Proxy = W->SpawnActor(C, nullptr, NAME_None, &Loc, nullptr, nullptr);
	if (!Proxy)
	{
		return true;
	}
	W->RecentSpawns.Reset();
	const bool bKeep = CheckRelevance(W, Proxy) && !Proxy->IsPendingKill();
	bool bResult = bKeep;
	if (!bKeep)
	{
		for (FUSObject* S : W->RecentSpawns)
		{
			EArenaPickupType NewType;
			TSubclassOf<AArenaWeapon> NewWeapon;
			if (!S->IsPendingKill() && OurPickup(S->Class, NewType, NewWeapon))
			{
				UE_LOG(LogUScript, Verbose, TEXT("UScript: pickup %s replaced by %s"), *ClassPath, *S->Class->GetPathName());
				Type = NewType;
				WeaponClass = NewWeapon;
				bResult = true;
				break;
			}
		}
	}
	for (FUSObject* S : W->RecentSpawns)
	{
		W->DestroyActor(S);
	}
	W->RecentSpawns.Reset();
	W->DestroyActor(Proxy);
	return bResult;
}

void ArenaUScriptHooks::ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons)
{
	FUSGameWorld* W = Controller ? FUSGameWorld::Find(Controller->GetWorld()) : nullptr;
	if (!W || !W->Game)
	{
		return;
	}
	FScriptTimer T(W);
	FUSRuntime& RT = FUSRuntime::Get();
	// UT2004: xPawn.RequiredEquipment + GameInfo.DefaultWeaponName; every inventory spawn passes CheckRelevance.
	TArray<TSubclassOf<AArenaWeapon>> Out;
	FUSObject* Base = BaseMutatorOf(W);
	auto TryWeaponClass = [&](FUSClass* C)
	{
		if (!C)
		{
			return;
		}
		FUSObject* Proxy = W->SpawnActor(C, nullptr, NAME_None, nullptr, nullptr, nullptr);
		if (!Proxy)
		{
			return;
		}
		if (CheckRelevance(W, Proxy) && !Proxy->IsPendingKill())
		{
			if (TSubclassOf<AArenaWeapon> Ours = OurWeapon(C->GetName()))
			{
				Out.AddUnique(Ours);
			}
		}
		W->DestroyActor(Proxy);
	};
	// UnrealPawn.AddDefaultInventory: CreateInventory(BaseMutator.GetInventoryClassOverride(RequiredEquipment[i]))
	FUSClass* PawnClass = RT.FindClass(TEXT("XGame.xPawn"));
	FUSObject* PawnDefaults = PawnClass ? PawnClass->GetDefaultObject() : nullptr;
	if (PawnDefaults && USProps().RequiredEquipment.Prop)
	{
		FUSProperty* RE = USProps().RequiredEquipment.Prop;
		for (int32 i = 0; i < RE->ArrayDim; ++i)
		{
			FString S = *reinterpret_cast<const FString*>(PawnDefaults->Data + RE->Offset + i * RE->ElementSize);
			if (S.IsEmpty())
			{
				continue;
			}
			if (FUSFunction* F = Base ? USVM::FindFunction(Base, TEXT("GetInventoryClassOverride")) : nullptr)
			{
				FUSParms P(F);
				P.Str(S);
				P.Call(Base);
				S = P.ReturnStr();
			}
			TryWeaponClass(RT.FindClass(S));
		}
	}
	// GameInfo.AddDefaultInventory: BaseMutator.GetDefaultWeapon()
	if (FUSFunction* F = Base ? USVM::FindFunction(Base, TEXT("GetDefaultWeapon")) : nullptr)
	{
		FUSParms P(F);
		P.Call(Base);
		FUSObject* C = P.ReturnObject();
		if (C && (C->Flags & USF_Field))
		{
			TryWeaponClass(static_cast<FUSClass*>(C));
		}
	}
	// DeathMatch.AddGameSpecificInventory: CreateInventory("XWeapons.TransLauncher") when AllowTransloc (a mutator such as
	// MutInstaGib can still turn it down in CheckRelevance)
	if (Weapons.Contains(TSubclassOf<AArenaWeapon>(AWeap_Translocator::StaticClass())))
	{
		TryWeaponClass(RT.FindClass(TEXT("XWeapons.TransLauncher")));
	}
	static int32 Reported = 0;
	if (Reported++ < 2)
	{
		FString Names;
		for (const TSubclassOf<AArenaWeapon>& Wc : Out)
		{
			Names += (*Wc ? Wc->GetName() : FString(TEXT("?"))) + TEXT(" ");
		}
		UE_LOG(LogUScript, Log, TEXT("UScript: default inventory after script: [%s]"), *Names.TrimEnd());
	}
	if (Out.Num() > 0)
	{
		Weapons = Out;
	}
}

void ArenaUScriptHooks::PawnSpawned(AArenaCharacter* Pawn)
{
	FUSGameWorld* W = Pawn ? FUSGameWorld::Find(Pawn->GetWorld()) : nullptr;
	if (!W)
	{
		return;
	}
	FScriptTimer T(W);
	FUSObject* Proxy = W->ProxyForPawn(Pawn);
	if (!Proxy)
	{
		return;
	}
	CheckRelevance(W, Proxy);
	if (FUSObject* Base = BaseMutatorOf(W))
	{
		if (FUSFunction* F = USVM::FindFunction(Base, TEXT("ModifyPlayer")))
		{
			FUSParms P(F);
			P.Object(Proxy);
			P.Call(Base);
		}
	}
	W->Push(Proxy);
	++W->Stats.ModifyPlayer;
	if (W->Stats.ModifyPlayer <= 2)
	{
		UE_LOG(LogUScript, Log, TEXT("UScript: ModifyPlayer(%s): MaxMultiJump=%d -> JumpMaxCount=%d, Health=%d, GroundSpeed=%.0f"), *Pawn->GetName(),
			USProps().MaxMultiJump.Get<int32>(Proxy), Pawn->JumpMaxCount, USProps().Health.Get<int32>(Proxy), USProps().GroundSpeed.Get<float>(Proxy));
	}
}

float ArenaUScriptHooks::ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim)
{
	FUSGameWorld* W = Victim ? FUSGameWorld::Find(Victim->GetWorld()) : nullptr;
	FUSObject* Rules = GameRulesOf(W);
	if (!Rules)
	{
		return Damage;
	}
	FScriptTimer T(W);
	FUSObject* VictimProxy = W->ProxyForPawn(Victim);
	FUSObject* InstProxy = Instigator ? W->ProxyForPawn(Instigator->GetPawn()) : nullptr;
	FUSFunction* F = USVM::FindFunction(Rules, TEXT("NetDamage"));
	if (!F || !VictimProxy)
	{
		return Damage;
	}
	W->Pull(VictimProxy);
	if (InstProxy)
	{
		W->Pull(InstProxy);
	}
	FUSParms P(F);
	const int32 D = FMath::RoundToInt(Damage);
	P.Int(D).Int(D).Object(VictimProxy).Object(InstProxy).Vector(USProps().Location.Get<FUSVector>(VictimProxy)).Vector(FUSVector());
	P.Object(FUSRuntime::Get().FindClass(TEXT("Engine.DamageType")));
	P.Call(Rules);
	++W->Stats.NetDamage;
	if (W->Stats.NetDamage <= 3)
	{
		UE_LOG(LogUScript, Log, TEXT("UScript: NetDamage(%d) on %s by %s: instigator health %d/%.0f pri %p/%p -> %d"), D, *VictimProxy->GetName(),
			InstProxy ? *InstProxy->GetName() : TEXT("None"), InstProxy ? USProps().Health.Get<int32>(InstProxy) : 0,
			InstProxy ? USProps().HealthMax.Get<float>(InstProxy) : 0.f, InstProxy ? USProps().PawnPRI.Get<FUSObject*>(InstProxy) : nullptr,
			USProps().PawnPRI.Get<FUSObject*>(VictimProxy), P.ReturnInt());
	}
	if (InstProxy)
	{
		const int32 Before = GetLinkHealth(W, InstProxy);
		W->Push(InstProxy);
		if (USProps().Health.Get<int32>(InstProxy) != Before)
		{
			++W->Stats.HealthChangedByRules;
			if (W->Stats.HealthChangedByRules <= 3)
			{
				UE_LOG(LogUScript, Log, TEXT("UScript: GameRules.NetDamage(%d) changed the instigator's health %d -> %d"), D, Before, USProps().Health.Get<int32>(InstProxy));
			}
		}
	}
	return float(P.ReturnInt());
}

void ArenaUScriptHooks::Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn)
{
	FUSGameWorld* W = Victim ? FUSGameWorld::Find(Victim->GetWorld()) : nullptr;
	if (!W)
	{
		return;
	}
	FScriptTimer T(W);
	FUSObject* K = Killer ? W->ProxyForController(Killer) : nullptr;
	FUSObject* V = W->ProxyForController(Victim);
	if (K)
	{
		W->Pull(K);
		W->Pull(USProps().ControllerPRI.Get<FUSObject*>(K));
	}
	W->Pull(USProps().ControllerPRI.Get<FUSObject*>(V));
	auto Call = [&](FUSObject* Target)
	{
		FUSFunction* F = Target ? USVM::FindFunction(Target, TEXT("ScoreKill")) : nullptr;
		if (F)
		{
			FUSParms P(F);
			P.Object(K).Object(V);
			P.Call(Target);
		}
	};
	Call(GameRulesOf(W));
	Call(BaseMutatorOf(W));
	++W->Stats.ScoreKill;
	if (K)
	{
		W->Push(USProps().ControllerPRI.Get<FUSObject*>(K));
	}
	W->Push(USProps().ControllerPRI.Get<FUSObject*>(V));
}

bool ArenaUScriptHooks::IsActive(const UWorld* World)
{
	return FUSGameWorld::Find(World) != nullptr;
}

// =============================================================================================
// Map script actors
// =============================================================================================

namespace
{
	/** Packages whose actor classes are UT2004's own (handled natively or not needed). */
	bool IsStockPackage(FName Pkg)
	{
		static const TSet<FName> Stock = { TEXT("Core"), TEXT("Engine"), TEXT("Editor"), TEXT("UnrealEd"), TEXT("Fire"), TEXT("IpDrv"),
			TEXT("UWeb"), TEXT("GamePlay"), TEXT("UnrealGame"), TEXT("XGame"), TEXT("XGame_rc"), TEXT("XEffects"), TEXT("XWeapons"),
			TEXT("XWeapons_rc"), TEXT("XPickups"), TEXT("XPickups_rc"), TEXT("XInterface"), TEXT("XAdmin"), TEXT("XWebAdmin"),
			TEXT("XVoting"), TEXT("GUI2K4"), TEXT("Onslaught"), TEXT("OnslaughtFull"), TEXT("OnslaughtBP"), TEXT("UT2k4Assault"),
			TEXT("UT2k4AssaultFull"), TEXT("Vehicles"), TEXT("BonusPack"), TEXT("SkaarjPack"), TEXT("SkaarjPack_rc"), TEXT("UTClassic"),
			TEXT("StreamlineFX"), TEXT("UTV2004c"), TEXT("UTV2004s"), TEXT("AssaultBP") };
		return Stock.Contains(Pkg);
	}

	/** UE2 damage type for one of our weapons / projectiles. */
	FUSClass* DamageTypeFor(AActor* Causer)
	{
		FString Name = TEXT("Engine.DamageType");
		const bool bProjectile = Causer && !Causer->IsA<AArenaWeapon>();
		AActor* WeaponActor = Causer;
		for (int32 Guard = 0; WeaponActor && !WeaponActor->IsA<AArenaWeapon>() && Guard < 4; ++Guard)
		{
			WeaponActor = WeaponActor->GetOwner();
		}
		if (WeaponActor && WeaponActor->IsA<AArenaWeapon>())
		{
			UClass* C = WeaponActor->GetClass();
			struct FMap { UClass* (*W)(); const TCHAR* Hit; const TCHAR* Proj; };
			static const FMap Map[] = {
				{ [] { return AWeap_PulseRifle::StaticClass(); }, TEXT("XWeapons.DamTypeShockBeam"), TEXT("XWeapons.DamTypeShockBall") },
				{ [] { return AWeap_InstaRifle::StaticClass(); }, TEXT("XWeapons.DamTypeSuperShockBeam"), TEXT("XWeapons.DamTypeSuperShockBeam") },
				{ [] { return AWeap_MarksmanRifle::StaticClass(); }, TEXT("UTClassic.DamTypeClassicSniper"), TEXT("UTClassic.DamTypeClassicSniper") },
				{ [] { return AWeap_RailRifle::StaticClass(); }, TEXT("XWeapons.DamTypeSniperShot"), TEXT("XWeapons.DamTypeSniperShot") },
				{ [] { return AWeap_Carbine::StaticClass(); }, TEXT("XWeapons.DamTypeAssaultBullet"), TEXT("XWeapons.DamTypeAssaultGrenade") },
				{ [] { return AWeap_RotaryGun::StaticClass(); }, TEXT("XWeapons.DamTypeMinigunBullet"), TEXT("XWeapons.DamTypeMinigunBullet") },
				{ [] { return AWeap_Gauntlet::StaticClass(); }, TEXT("XWeapons.DamTypeShieldImpact"), TEXT("XWeapons.DamTypeShieldImpact") },
				{ [] { return AWeap_ArcLinker::StaticClass(); }, TEXT("XWeapons.DamTypeLinkShaft"), TEXT("XWeapons.DamTypeLinkPlasma") },
				{ [] { return AWeap_RocketLauncher::StaticClass(); }, TEXT("XWeapons.DamTypeRocket"), TEXT("XWeapons.DamTypeRocket") },
				{ [] { return AWeap_ShardCannon::StaticClass(); }, TEXT("XWeapons.DamTypeFlakChunk"), TEXT("XWeapons.DamTypeFlakShell") },
				{ [] { return AWeap_GooLauncher::StaticClass(); }, TEXT("XWeapons.DamTypeBioGlob"), TEXT("XWeapons.DamTypeBioGlob") },
				{ [] { return AWeap_WarheadLauncher::StaticClass(); }, TEXT("XWeapons.DamTypeRedeemer"), TEXT("XWeapons.DamTypeRedeemer") },
				{ [] { return AWeap_OrbitalDesignator::StaticClass(); }, TEXT("XWeapons.DamTypeIonBlast"), TEXT("XWeapons.DamTypeIonBlast") },
				{ [] { return AWeap_SeekerLauncher::StaticClass(); }, TEXT("Onslaught.DamTypeONSAVRiLRocket"), TEXT("Onslaught.DamTypeONSAVRiLRocket") },
				{ [] { return AWeap_StickyLauncher::StaticClass(); }, TEXT("Onslaught.DamTypeONSGrenade"), TEXT("Onslaught.DamTypeONSGrenade") },
				{ [] { return AWeap_CrawlerLayer::StaticClass(); }, TEXT("Onslaught.DamTypeONSMine"), TEXT("Onslaught.DamTypeONSMine") },
				{ [] { return AWeap_Translocator::StaticClass(); }, TEXT("XWeapons.DamTypeTeleFrag"), TEXT("XWeapons.DamTypeTeleFrag") } };
			for (const FMap& M : Map)
			{
				if (C->IsChildOf(M.W()))
				{
					Name = bProjectile ? M.Proj : M.Hit;
					break;
				}
			}
		}
		FUSClass* DT = FUSRuntime::Get().FindClass(Name);
		return DT ? DT : FUSRuntime::Get().FindClass(TEXT("Engine.DamageType"));
	}

	/** DT_StaticMesh script actors show their mesh from the content cache. */
	void ApplyBodyMesh(AArenaScriptActor* Body, FUSObject* A)
	{
		FUSProperty* DT = A->FindProperty(TEXT("DrawType"));
		FUSProperty* SM = A->FindProperty(TEXT("StaticMesh"));
		if (!DT || !SM || A->Data[DT->Offset] != 8)
		{
			return;
		}
		FUSObject* Mesh = *reinterpret_cast<FUSObject**>(A->Data + SM->Offset);
		if (!Mesh)
		{
			return;
		}
		UStaticMesh* UM = ArenaRuntimeContent::LoadStaticMeshByObjectPath(Mesh->GetPathName());
		UE_LOG(LogUScript, Verbose, TEXT("UScript: %s mesh %s %s"), *A->GetName(), *Mesh->GetPathName(), UM ? TEXT("loaded") : TEXT("missing"));
		if (!UM)
		{
			static int32 Missing = 0;
			if (Missing++ < 5)
			{
				UE_LOG(LogUScript, Warning, TEXT("UScript: %s: static mesh %s is not in the content cache"), *A->GetName(), *Mesh->GetPathName());
			}
		}
		float Scale = USProps().DrawScale.Get<float>(A, 1.f);
		FVector Scale3D(Scale);
		if (FUSProperty* S3 = A->FindProperty(TEXT("DrawScale3D")))
		{
			Scale3D *= reinterpret_cast<FUSVector*>(A->Data + S3->Offset)->ToFVector();
		}
		Body->SetMesh(UM, Scale3D, USUnits::FromUE2(USProps().Rotation.Get<FUSRotator>(A)));
		Body->SetScriptHidden(USProps().bHidden.Get<uint32>(A) != 0);
	}

	AActor* GameActorOf(FUSGameWorld* W, FUSObject* O)
	{
		return O ? W->GetLinkedActor(O) : nullptr;
	}
}

FUSObject* FUSGameWorld::ObjectForActor(AActor* A) const
{
	if (!A)
	{
		return nullptr;
	}
	if (AArenaScriptActor* Body = Cast<AArenaScriptActor>(A))
	{
		return static_cast<FUSObject*>(Body->ScriptObject);
	}
	FUSObject* const* P = Proxies.Find(A);
	return P ? *P : nullptr;
}

int32 FUSGameWorld::SpawnMapScriptActors(const FString& InMapName)
{
	FUSRuntime& RT = FUSRuntime::Get();
	FUSLinker* L = RT.LoadPackage(FName(*InMapName));
	UWorld* UW = World.Get();
	if (!L || !UW || !RT.ActorClass)
	{
		return 0;
	}
	FUSEngineProps& P = USProps();
	// the map's LevelInfo is our Level
	for (int32 Ei = 0; Ei < L->NumExports(); ++Ei)
	{
		FUSClass* C = L->GetExportClass(Ei);
		if (C && C->Name == TEXT("LevelInfo"))
		{
			L->SetExportObject(Ei, Level);
		}
	}
	struct FNew { int32 Ei; FUSObject* Obj; };
	TArray<FNew> Created;
	TSet<FUSClass*> Skipped;
	for (int32 Ei = 0; Ei < L->NumExports(); ++Ei)
	{
		FUSClass* C = L->GetExportClass(Ei);
		if (!C || !C->EnsureLoaded() || !C->IsChildOf(RT.ActorClass) || IsStockPackage(C->GetPackageName()))
		{
			continue;
		}
		FString Reason;
		if (USGetClassSupport(C, Reason) != EUSClassSupport::RunsInVM)
		{
			// the game keeps its stand-in for the stock base class (pickups, movers, emitters ...)
			if (!Skipped.Contains(C))
			{
				Skipped.Add(C);
				UE_LOG(LogUScript, Log, TEXT("UScript: %s: %s"), *InMapName, *Reason);
			}
			continue;
		}
		FUSObject* A = ConstructObject(C, Level, FName(*L->GetExportName(Ei)));
		if (!A)
		{
			continue;
		}
		L->SetExportObject(Ei, A);
		Created.Add({ Ei, A });
	}
	for (FNew& N : Created)
	{
		FUSObject* A = N.Obj;
		L->LoadInstanceProperties(N.Ei, A);
		P.Level.Set<FUSObject*>(A, Level);
		P.Role.Set<uint8>(A, 4);
		if (DefaultPhysicsVolume)
		{
			P.PhysicsVolume.Set<FUSObject*>(A, DefaultPhysicsVolume);
		}
		// the world module may have spawned a native stand-in for the base class: the script version replaces it
		if (AActor* Native = ArenaUE2World::FindActorByName(UW, A->GetName()))
		{
			Native->Destroy();
		}
		const FUSVector Loc = P.Location.Get<FUSVector>(A);
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AArenaScriptActor* Body = UW->SpawnActor<AArenaScriptActor>(AArenaScriptActor::StaticClass(), USUnits::FromUE2(Loc), FRotator::ZeroRotator, SP);
		if (Body)
		{
			bool bCollide = false;
			if (FUSProperty* CP = A->FindProperty(TEXT("bCollideActors")))
			{
				bCollide = *reinterpret_cast<uint32*>(A->Data + CP->Offset) != 0;
			}
			const float R = P.CollisionRadius.Get<float>(A) * ArenaUE2World::Scale;
			const float H = P.CollisionHeight.Get<float>(A) * ArenaUE2World::Scale;
			Body->SetupScript(P.Tag.Get<FName>(A), P.Event.Get<FName>(A), R, H, bCollide, true);
			ApplyBodyMesh(Body, A);
			// a script pickup replaces the stock-equivalent pickup the level builder placed for it
			if (A->IsA(FName(TEXT("Pickup"))))
			{
				for (TActorIterator<AArenaPickup> It(UW); It; ++It)
				{
					if (FVector::Dist(It->GetActorLocation(), Body->GetActorLocation()) < 120.f)
					{
						It->Destroy();
						break;
					}
				}
			}
			Body->ScriptObject = A;
			FUSActorLink* Link = new FUSActorLink();
			Link->Kind = FUSActorLink::EKind::ScriptActor;
			Link->Actor = Body;
			Link->bRunsScript = true;
			Link->LastLocation = Loc;
			Links.Add(A, Link);
			A->Link = Link;
			Proxies.Add(Body, A);
		}
	}
	// UE2 level start: PreBeginPlay / BeginPlay / PostBeginPlay / SetInitialState for every placed actor
	for (FNew& N : Created)
	{
		if (!N.Obj->IsPendingKill())
		{
			BeginPlayEvents(N.Obj);
			P.bScriptInitialized.Set<uint32>(N.Obj, 1u);
		}
	}
	if (Created.Num())
	{
		TMap<FString, int32> ByClass;
		for (FNew& N : Created)
		{
			ByClass.FindOrAdd(N.Obj->Class->GetName())++;
		}
		FString List;
		for (const TPair<FString, int32>& KV : ByClass)
		{
			List += FString::Printf(TEXT("%s x%d "), *KV.Key, KV.Value);
		}
		UE_LOG(LogUScript, Log, TEXT("UScript: %s: %d map script actors [%s]"), *InMapName, Created.Num(), *List.TrimEnd());
	}
	return Created.Num();
}

void ArenaUScriptHooks::ScriptActorEvent(AActor* Body, AActor* Other, APawn* EventInstigator, int32 Kind)
{
	FUSGameWorld* W = Body ? FUSGameWorld::Find(Body->GetWorld()) : nullptr;
	FUSObject* Self = W ? W->ObjectForActor(Body) : nullptr;
	if (!Self || Self->IsPendingKill())
	{
		return;
	}
	FScriptTimer T(W);
	W->PullAll();
	FUSObject* OtherObj = W->ObjectForActor(Other);
	if (!OtherObj && Other && Other->IsA<APawn>())
	{
		OtherObj = W->ProxyForPawn(Cast<APawn>(Other));
	}
	FUSObject* InstObj = EventInstigator ? W->ProxyForPawn(EventInstigator) : nullptr;
	static const TCHAR* Names[] = { TEXT("Trigger"), TEXT("UnTrigger"), TEXT("Touch"), TEXT("UnTouch") };
	FUSFunction* F = USVM::FindFunction(Self, Names[FMath::Clamp(Kind, 0, 3)]);
	if (F)
	{
		FUSParms P(F);
		P.Object(OtherObj);
		if (Kind < 2)
		{
			P.Object(InstObj);
		}
		P.Call(Self);
	}
	W->PushAll();
}

void ArenaUScriptHooks::ScriptActorDamage(AActor* Body, float Damage, AController* EventInstigator, AActor* DamageCauser)
{
	FUSGameWorld* W = Body ? FUSGameWorld::Find(Body->GetWorld()) : nullptr;
	FUSObject* Self = W ? W->ObjectForActor(Body) : nullptr;
	if (!Self || Self->IsPendingKill())
	{
		return;
	}
	FScriptTimer T(W);
	FUSObject* Inst = (EventInstigator && EventInstigator->GetPawn()) ? W->ProxyForPawn(EventInstigator->GetPawn()) : nullptr;
	if (FUSFunction* F = USVM::FindFunction(Self, TEXT("TakeDamage")))
	{
		FUSParms P(F);
		P.Int(FMath::RoundToInt(Damage)).Object(Inst).Vector(USProps().Location.Get<FUSVector>(Self)).Vector(FUSVector());
		P.Object(DamageTypeFor(DamageCauser));
		P.Call(Self);
		UE_LOG(LogUScript, Log, TEXT("UScript: %s.TakeDamage(%d, %s)"), *Self->GetName(), FMath::RoundToInt(Damage), DamageCauser ? *DamageCauser->GetName() : TEXT("None"));
	}
	W->PushAll();
}

// ---------------------------------------------------------------------------------------------
// Script functions replaced by the bridge (registered like natives; the loader swaps them in)
// ---------------------------------------------------------------------------------------------

namespace USBridgeOverrides
{
	FUSGameWorld* GWOf(FUSObject* O)
	{
		return (O && O->World) ? O->World->AsGameWorld() : nullptr;
	}

	void EventImpl(FUSObject* Self, FUSFrame& Stack, bool bTrigger)
	{
		USP_GET_NAME(EventName);
		USP_GET_OBJECT(Other);
		USP_GET_OBJECT(Instigator);
		USP_FINISH;
		FUSGameWorld* W = GWOf(Self);
		if (!W || EventName == NAME_None)
		{
			return;
		}
		UE_LOG(LogUScript, Log, TEXT("UScript: %s %s event '%s'"), *Self->GetName(), bTrigger ? TEXT("triggers") : TEXT("untriggers"), *EventName.ToString());
		// VM actors without a game body (mutators, rules ...) with that Tag
		for (FUSObject* A : W->GetActors())
		{
			if (!A->Link && A != Self && USProps().Tag.Get<FName>(A) == EventName)
			{
				if (FUSFunction* F = USVM::FindFunction(A, bTrigger ? TEXT("Trigger") : TEXT("UnTrigger")))
				{
					FUSParms P(F);
					P.Object(Other).Object(Instigator);
					P.Call(A);
				}
			}
		}
		// the game's event system: native world actors and map script actors (through their bodies)
		UWorld* UW = W->GetUWorld();
		if (UArenaEventSubsystem* Events = UW ? UArenaEventSubsystem::Get(UW) : nullptr)
		{
			AActor* OtherActor = GameActorOf(W, Other);
			APawn* InstPawn = Cast<APawn>(GameActorOf(W, Instigator));
			if (bTrigger)
			{
				Events->TriggerEvent(EventName, OtherActor, InstPawn);
			}
			else
			{
				Events->UntriggerEvent(EventName, OtherActor, InstPawn);
			}
		}
	}
	US_NATIVE(TriggerEvent) { EventImpl(Self, Stack, true); }
	US_NATIVE(UntriggerEvent) { EventImpl(Self, Stack, false); }

	void ShowTo(FUSGameWorld* W, FUSObject* ControllerProxy, const FString& Text)
	{
		if (Text.IsEmpty() || !W)
		{
			return;
		}
		if (ControllerProxy)
		{
			if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(W->GetLinkedActor(ControllerProxy)))
			{
				PC->AddCenterMessage(Text, FLinearColor::White, 3.f);
			}
			return;
		}
		if (AArenaGameMode* GM = W->GetGameMode())
		{
			GM->BroadcastCenterMessage(Text, FLinearColor::White, 3.f);
		}
	}

	US_NATIVE(ClientMessage)
	{
		USP_GET_STR(S);
		USP_GET_NAME_OPTX(Type, NAME_None);
		USP_GET_FLOAT_OPTX(Life, 0.f);
		USP_FINISH;
		ShowTo(GWOf(Self), Self, S);
	}

	/** PlayerController.ResetInput: UE2 clears the PlayerInput buttons and axes; ours is releasing the pressed keys. */
	US_NATIVE(ResetInput)
	{
		USP_FINISH;
		FUSGameWorld* W = GWOf(Self);
		if (APlayerController* PC = W ? Cast<APlayerController>(W->GetLinkedActor(Self)) : nullptr)
		{
			PC->FlushPressedKeys();
		}
	}

	FString LocalMessageString(FUSObject* MessageClass, int32 Switch, FUSObject* PRI1, FUSObject* PRI2, FUSObject* Opt)
	{
		FUSClass* C = (MessageClass && (MessageClass->Flags & USF_Field)) ? static_cast<FUSClass*>(MessageClass) : nullptr;
		FUSObject* DO = C ? C->GetDefaultObject() : nullptr;
		FUSFunction* F = DO ? USVM::FindFunction(DO, TEXT("GetString")) : nullptr;
		if (!F)
		{
			return FString();
		}
		FUSParms P(F);
		P.Int(Switch).Object(PRI1).Object(PRI2).Object(Opt);
		P.Call(DO);
		return P.ReturnStr();
	}

	US_NATIVE(ReceiveLocalizedMessage)
	{
		USP_GET_OBJECT(MessageClass);
		USP_GET_INT_OPTX(Switch, 0);
		USP_GET_OBJECT_OPTX(PRI1, nullptr);
		USP_GET_OBJECT_OPTX(PRI2, nullptr);
		USP_GET_OBJECT_OPTX(Opt, nullptr);
		USP_FINISH;
		ShowTo(GWOf(Self), Self, LocalMessageString(MessageClass, Switch, PRI1, PRI2, Opt));
	}

	US_NATIVE(GetLocalPlayerController)
	{
		USP_FINISH;
		FUSGameWorld* W = GWOf(Self);
		if (!W)
		{
			US_RESULT(FUSObject*) = (Self && Self->World) ? Self->World->GetLocalPlayerController() : nullptr;
			return;
		}
		UWorld* UW = W->GetUWorld();
		APlayerController* PC = UW ? UW->GetFirstPlayerController() : nullptr;
		US_RESULT(FUSObject*) = PC ? W->ProxyForController(PC) : nullptr;
	}

	US_NATIVE(Broadcast)
	{
		USP_GET_OBJECT(Sender);
		USP_GET_STR(Msg);
		USP_GET_NAME_OPTX(Type, NAME_None);
		USP_FINISH;
		ShowTo(GWOf(Self), nullptr, Msg);
	}

	US_NATIVE(BroadcastLocalized)
	{
		USP_GET_OBJECT(Sender);
		USP_GET_OBJECT(MessageClass);
		USP_GET_INT_OPTX(Switch, 0);
		USP_GET_OBJECT_OPTX(PRI1, nullptr);
		USP_GET_OBJECT_OPTX(PRI2, nullptr);
		USP_GET_OBJECT_OPTX(Opt, nullptr);
		USP_FINISH;
		ShowTo(GWOf(Self), nullptr, LocalMessageString(MessageClass, Switch, PRI1, PRI2, Opt));
	}
}

void USRegisterBridgeOverrides()
{
	using namespace USBridgeOverrides;
	static const FUSNativeEntry Entries[] = {
		{ TEXT("Actor"), TEXT("TriggerEvent"), &TriggerEvent }, { TEXT("Actor"), TEXT("UntriggerEvent"), &UntriggerEvent },
		{ TEXT("PlayerController"), TEXT("ClientMessage"), &ClientMessage },
		{ TEXT("PlayerController"), TEXT("ReceiveLocalizedMessage"), &ReceiveLocalizedMessage },
		{ TEXT("PlayerController"), TEXT("ResetInput"), &ResetInput },
		{ TEXT("LevelInfo"), TEXT("GetLocalPlayerController"), &GetLocalPlayerController },
		{ TEXT("GameInfo"), TEXT("Broadcast"), &Broadcast }, { TEXT("GameInfo"), TEXT("BroadcastLocalized"), &BroadcastLocalized },
	};
	USNatives::Register(Entries, UE_ARRAY_COUNT(Entries));
}

void USShootTest(FUSGameWorld* W)
{
	UWorld* UW = W->GetUWorld();
	APlayerController* PC = UW ? UW->GetFirstPlayerController() : nullptr;
	FUSObject* Inst = (PC && PC->GetPawn()) ? W->ProxyForPawn(PC->GetPawn()) : nullptr;
	FUSClass* DT = FUSRuntime::Get().FindClass(TEXT("XWeapons.DamTypeShockBeam"));
	int32 N = 0;
	for (FUSObject* A : W->GetActors())
	{
		FUSActorLink* L = W->GetLink(A);
		if (!L || L->Kind != FUSActorLink::EKind::ScriptActor)
		{
			continue;
		}
		if (FUSFunction* F = USVM::FindFunction(A, TEXT("TakeDamage")))
		{
			FUSParms P(F);
			P.Int(45).Object(Inst).Vector(USProps().Location.Get<FUSVector>(A)).Vector(FUSVector());
			P.Object(DT);
			P.Call(A);
			++N;
		}
	}
	W->PushAll();
	UE_LOG(LogUScript, Log, TEXT("UScript: shoot test hit %d script actors with DamTypeShockBeam (instigator %s)"), N, Inst ? *Inst->GetName() : TEXT("None"));
	// touch test: the player (at 50 health, no shield) touches every script pickup
	AArenaCharacter* Ch = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
	if (!Ch || !Inst)
	{
		return;
	}
	for (FUSObject* A : W->GetActors())
	{
		FUSActorLink* L = W->GetLink(A);
		if (!L || L->Kind != FUSActorLink::EKind::ScriptActor || !A->IsA(FName(TEXT("Pickup"))))
		{
			continue;
		}
		Ch->Health = 50.f;
		Ch->Shield = 0.f;
		W->Pull(Inst);
		USProps().Location.Set<FUSVector>(Inst, USProps().Location.Get<FUSVector>(A)); // as if standing on it
		if (FUSFunction* F = USVM::FindFunction(A, TEXT("Touch")))
		{
			FUSParms P(F);
			P.Object(Inst);
			P.Call(A);
		}
		W->PushAll();
		const FName State = A->StateFrame && A->StateFrame->StateNode ? A->StateFrame->StateNode->Name : NAME_None;
		UE_LOG(LogUScript, Log, TEXT("UScript: touch test %s (%s): health 50 -> %.0f, shield 0 -> %.0f, state now %s, hidden %d"), *A->GetName(),
			*A->Class->GetName(), Ch->Health, Ch->Shield, *State.ToString(), USProps().bHidden.Get<uint32>(A));
	}
}

#endif // OPENUT2K4_USCRIPT_ENABLED

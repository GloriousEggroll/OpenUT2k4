#include "UE2ServerWorld.h"

#include "UE2Server.h"

#if OPENUT2K4_UE2SERVER_BUILD

#include "UE2ServerConnection.h"
#include "UE2ServerCodec.h"
#include "UE2ServerPlayer.h"
#include "UE2ServerUplink.h"
#include "UE2NetLog.h"
#include "../ArenaCharacter.h"
#include "../ArenaChat.h"
#include "../ArenaCharacterMovement.h"
#include "../ArenaAS_Info.h"
#include "../ArenaAS_Objective.h"
#include "../ArenaBR_Ball.h"
#include "../ArenaINV_Monster.h"
#include "../ArenaINV_WaveState.h"
#include "../ArenaContent.h"
#include "../ArenaDOM_ControlPoint.h"
#include "../ArenaFlag.h"
#include "../ArenaGameMode.h"
#include "../ArenaGameRules.h"
#include "../ArenaGameState.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaMover.h"
#include "../ArenaONS_PowerNode.h"
#include "../ArenaONS_Vehicle.h"
#include "../ArenaPickup.h"
#include "../ArenaPlayerController.h"
#include "../ArenaPlayerState.h"
#include "../ArenaProjectile.h"
#include "../ArenaSPData.h"
#include "../ArenaUE2World.h"
#include "../ArenaWeapon.h"
#include "../ArenaWeapons.h"
#include "../ArenaArtVehicles.h"
#include "../ArenaWorldZones.h"
#include "../UScript/UScriptCore.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameSession.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE2ServerWorldLocal
{
	constexpr float S = ArenaUE2World::Scale;
	/** How much higher (UU) a walking UE2 pawn rests above the floor than ours (Net/UE2NetPlay.h NetWalkExtraHeight). */
	constexpr float WalkZOffset = 4.78f;
	/** Client positions closer than this (UU) to ours are taken as they are (no correction): our movement is not
	 *  bit-identical to UE2's, and UE2's own threshold (error^2 > 3) would correct nearly every move. */
	constexpr float TrustDistance = 48.f;
	/** UE2 MaxResponseTime. */
	constexpr float MaxResponseTime = 0.125f;

	FVector3f UU(const FVector& Cm) { return FVector3f(Cm / S); }
	FVector Cm(const FVector& UUVec) { return UUVec * S; }
	int32 RotUU(double Degrees) { return int32(FMath::RoundToInt64(Degrees * 65536.0 / 360.0)) & 65535; }
	double RotDeg(int32 UUValue) { return double(UUValue & 65535) * 360.0 / 65536.0; }

	FUE2NetValue PawnPosition(const FVector& LocCm, double YawDeg, const FVector& VelCm)
	{
		FUE2NetValue V;
		V.Kind = FUE2NetValue::EKind::Struct;
		V.Members.Emplace(TEXT("Location"), UE2Codec::Vec(UU(LocCm)));
		V.Members.Emplace(TEXT("Rotation"), UE2Codec::Rot(FIntVector(0, RotUU(YawDeg), 0)));
		V.Members.Emplace(TEXT("Velocity"), UE2Codec::Vec(UU(VelCm)));
		return V;
	}

	/** UE2 EPhysics of a character. */
	uint8 PhysicsOf(const AArenaCharacter* C)
	{
		const UCharacterMovementComponent* M = C ? C->GetCharacterMovement() : nullptr;
		if (!M)
		{
			return 0;
		}
		switch (M->MovementMode)
		{
		case MOVE_Walking:
		case MOVE_NavWalking: return 1;
		case MOVE_Falling: return 2;
		case MOVE_Swimming: return 3;
		case MOVE_Flying: return 4;
		default: return 0;
		}
	}

	struct FWeaponNames
	{
		const TCHAR* Native;
		const TCHAR* Weapon;
		const TCHAR* Attachment;
		const TCHAR* Pickup;
		const TCHAR* AmmoPickup;
	};
	const FWeaponNames WeaponTable[] = {
		{ TEXT("Weap_Gauntlet"), TEXT("XWeapons.ShieldGun"), TEXT("XWeapons.ShieldAttachment"), TEXT(""), TEXT("") },
		{ TEXT("Weap_Carbine"), TEXT("XWeapons.AssaultRifle"), TEXT("XWeapons.AssaultAttachment"), TEXT("XWeapons.AssaultRiflePickup"), TEXT("XWeapons.AssaultAmmoPickup") },
		{ TEXT("Weap_GooLauncher"), TEXT("XWeapons.BioRifle"), TEXT("XWeapons.BioAttachment"), TEXT("XWeapons.BioRiflePickup"), TEXT("XWeapons.BioAmmoPickup") },
		{ TEXT("Weap_PulseRifle"), TEXT("XWeapons.ShockRifle"), TEXT("XWeapons.ShockAttachment"), TEXT("XWeapons.ShockRiflePickup"), TEXT("XWeapons.ShockAmmoPickup") },
		{ TEXT("Weap_ArcLinker"), TEXT("XWeapons.LinkGun"), TEXT("XWeapons.LinkAttachment"), TEXT("XWeapons.LinkGunPickup"), TEXT("XWeapons.LinkAmmoPickup") },
		{ TEXT("Weap_RotaryGun"), TEXT("XWeapons.Minigun"), TEXT("XWeapons.MinigunAttachment"), TEXT("XWeapons.MinigunPickup"), TEXT("XWeapons.MinigunAmmoPickup") },
		{ TEXT("Weap_ShardCannon"), TEXT("XWeapons.FlakCannon"), TEXT("XWeapons.FlakAttachment"), TEXT("XWeapons.FlakCannonPickup"), TEXT("XWeapons.FlakAmmoPickup") },
		{ TEXT("Weap_RocketLauncher"), TEXT("XWeapons.RocketLauncher"), TEXT("XWeapons.RocketAttachment"), TEXT("XWeapons.RocketLauncherPickup"), TEXT("XWeapons.RocketAmmoPickup") },
		{ TEXT("Weap_MarksmanRifle"), TEXT("UTClassic.ClassicSniperRifle"), TEXT("UTClassic.ClassicSniperAttachment"), TEXT("UTClassic.ClassicSniperRiflePickup"), TEXT("UTClassic.ClassicSniperAmmoPickup") },
		{ TEXT("Weap_RailRifle"), TEXT("XWeapons.SniperRifle"), TEXT("XWeapons.SniperAttachment"), TEXT("XWeapons.SniperRiflePickup"), TEXT("XWeapons.SniperAmmoPickup") },
		{ TEXT("Weap_InstaRifle"), TEXT("XWeapons.SuperShockRifle"), TEXT("XWeapons.SuperShockRifleAttachment"), TEXT("XWeapons.SuperShockRiflePickup"), TEXT("") },
		{ TEXT("Weap_Translocator"), TEXT("XWeapons.TransLauncher"), TEXT("XWeapons.TransAttachment"), TEXT(""), TEXT("") },
		{ TEXT("Weap_WarheadLauncher"), TEXT("XWeapons.Redeemer"), TEXT("XWeapons.RedeemerAttachment"), TEXT("XWeapons.RedeemerPickup"), TEXT("") },
		{ TEXT("Weap_OrbitalDesignator"), TEXT("XWeapons.Painter"), TEXT("XWeapons.PainterAttachment"), TEXT("XWeapons.PainterPickup"), TEXT("") },
		{ TEXT("Weap_SeekerLauncher"), TEXT("Onslaught.ONSAVRiL"), TEXT("Onslaught.ONSAVRiLAttachment"), TEXT("Onslaught.ONSAVRiLPickup"), TEXT("Onslaught.ONSAVRiLAmmoPickup") },
		{ TEXT("Weap_StickyLauncher"), TEXT("Onslaught.ONSGrenadeLauncher"), TEXT("Onslaught.ONSGrenadeAttachment"), TEXT("Onslaught.ONSGrenadePickup"), TEXT("Onslaught.ONSGrenadeAmmoPickup") },
		{ TEXT("Weap_CrawlerLayer"), TEXT("Onslaught.ONSMineLayer"), TEXT("Onslaught.ONSMineLayerAttachment"), TEXT("Onslaught.ONSMineLayerPickup"), TEXT("Onslaught.ONSMineAmmoPickup") },
		{ TEXT("ArenaBR_BallLauncher"), TEXT("XWeapons.BallLauncher"), TEXT("XWeapons.BallAttachment"), TEXT(""), TEXT("") },
	};

	/** HUD and scoreboard classes of a stock game (xPlayer.ClientSetHUD). */
	void HUDClasses(const FString& ModeId, FString& OutHUD, FString& OutScores)
	{
		const FString M = ModeId.ToUpper();
		OutScores = TEXT("XInterface.ScoreBoardTeamDeathMatch");
		if (M == TEXT("DM") || M == TEXT("LMS") || M == TEXT("MUT"))
		{
			OutHUD = M == TEXT("LMS") ? TEXT("XInterface.HudCDeathMatch") : TEXT("XInterface.HudCDeathMatch");
			OutScores = TEXT("XInterface.ScoreBoardDeathMatch");
		}
		else if (M == TEXT("TDM")) OutHUD = TEXT("XInterface.HudCTeamDeathMatch");
		else if (M == TEXT("CTF")) OutHUD = TEXT("XInterface.HudCCaptureTheFlag");
		else if (M == TEXT("BR")) OutHUD = TEXT("XInterface.HudCBombingRun");
		else if (M == TEXT("DOM")) OutHUD = TEXT("XInterface.HudCDoubleDomination");
		else if (M == TEXT("ONS")) { OutHUD = TEXT("Onslaught.ONSHUDOnslaught"); OutScores = TEXT("Onslaught.ONSScoreBoard"); }
		else if (M == TEXT("AS")) { OutHUD = TEXT("UT2k4Assault.HUD_Assault"); OutScores = TEXT("UT2k4Assault.ScoreBoard_Assault"); }
		else if (M == TEXT("INV")) { OutHUD = TEXT("SkaarjPack.HudInvasion"); OutScores = TEXT("SkaarjPack.ScoreBoardInvasion"); }
		else OutHUD = TEXT("XInterface.HudCDeathMatch");
	}

	FString GameNameOf(const FString& ModeId)
	{
		static const TMap<FString, FString> Names = {
			{ TEXT("DM"), TEXT("DeathMatch") }, { TEXT("TDM"), TEXT("Team DeathMatch") }, { TEXT("CTF"), TEXT("Capture the Flag") },
			{ TEXT("BR"), TEXT("Bombing Run") }, { TEXT("DOM"), TEXT("Double Domination") }, { TEXT("ONS"), TEXT("Onslaught") },
			{ TEXT("AS"), TEXT("Assault") }, { TEXT("INV"), TEXT("Invasion") }, { TEXT("LMS"), TEXT("Last Man Standing") }, { TEXT("MUT"), TEXT("Mutant") },
		};
		const FString* N = Names.Find(ModeId.ToUpper());
		return N ? *N : FString(TEXT("DeathMatch"));
	}

	FString PickupClass(const AArenaPickup* P)
	{
		switch (P->GetPickupType())
		{
		case EArenaPickupType::HealthVial: return TEXT("XPickups.MiniHealthPack");
		case EArenaPickupType::HealthPack: return TEXT("XPickups.HealthPack");
		case EArenaPickupType::SuperHealth: return TEXT("XPickups.SuperHealthPack");
		case EArenaPickupType::ShieldSmall: return TEXT("XPickups.ShieldPack");
		case EArenaPickupType::ShieldLarge: return TEXT("XPickups.SuperShieldPack");
		case EArenaPickupType::DoubleDamage: return TEXT("XPickups.UDamagePack");
		case EArenaPickupType::Adrenaline: return TEXT("XPickups.AdrenalinePickup");
		default: return FString();
		}
	}

	FString ProjectileClass(const AArenaProjectile* P)
	{
		const FArenaProjectileParams& Params = P->GetParams();
		switch (Params.Kind)
		{
		case EArenaProjKind::Grenade: return TEXT("XWeapons.Grenade");
		case EArenaProjKind::BioGlob: return TEXT("XWeapons.BioGlob");
		case EArenaProjKind::ShockBall: return TEXT("XWeapons.ShockProjectile");
		case EArenaProjKind::LinkBolt: return TEXT("XWeapons.LinkProjectile");
		case EArenaProjKind::FlakChunk: return TEXT("XWeapons.FlakChunk");
		case EArenaProjKind::FlakShell: return TEXT("XWeapons.FlakShell");
		case EArenaProjKind::Rocket: return TEXT("XWeapons.RocketProj");
		default: break;
		}
		const FString& W = Params.WeaponName;
		if (W.Contains(TEXT("Seeker"))) return TEXT("Onslaught.ONSAVRiLRocket");
		if (W.Contains(TEXT("Sticky"))) return TEXT("Onslaught.ONSGrenadeProjectile");
		if (W.Contains(TEXT("Crawler"))) return TEXT("Onslaught.ONSMineProjectile");
		if (W.Contains(TEXT("Warhead"))) return TEXT("XWeapons.RedeemerProjectile");
		if (W.Contains(TEXT("Translocator"))) return TEXT("XWeapons.TransBeacon");
		return FString();
	}

	/** The map's actors from the content cache (map.json "actors": name, class, location). */
	struct FMapActor
	{
		FString Name;
		FString Class;
		FVector Location = FVector::ZeroVector;
	};
	TArray<FMapActor> LoadMapActors(const FString& MapName)
	{
		TArray<FMapActor> Out;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("maps"), MapName, TEXT("map.json"))))
		{
			return Out;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Root->TryGetArrayField(TEXT("actors"), Actors))
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& V : *Actors)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid())
			{
				continue;
			}
			FMapActor A;
			O->TryGetStringField(TEXT("name"), A.Name);
			O->TryGetStringField(TEXT("class"), A.Class);
			const TArray<TSharedPtr<FJsonValue>>* L = nullptr;
			if (O->TryGetArrayField(TEXT("location"), L) && L->Num() == 3)
			{
				A.Location = FVector((*L)[0]->AsNumber(), (*L)[1]->AsNumber(), (*L)[2]->AsNumber());
			}
			Out.Add(MoveTemp(A));
		}
		return Out;
	}
}

using namespace UE2ServerWorldLocal;

// =================================================================================================
// Setup
// =================================================================================================

FUE2ServerWorld::FUE2ServerWorld(FUE2Server& InServer, UWorld* InWorld)
	: Server(InServer)
	, World(InWorld)
{
	MapName = Server.GetMapName();
	const AArenaGameMode* GM = InWorld ? InWorld->GetAuthGameMode<AArenaGameMode>() : nullptr;
	ModeId = GM ? GM->GetModeId() : FString(TEXT("DM"));
	StartTime = FPlatformTime::Seconds();

	// map actors the clients have from their own copy of the map: the level info and the flag bases
	const TArray<FMapActor> MapActors = LoadMapActors(MapName);
	FString LevelInfoName = TEXT("LevelInfo0");
	for (const FMapActor& A : MapActors)
	{
		if (A.Class == TEXT("LevelInfo"))
		{
			LevelInfoName = A.Name;
		}
		else if (A.Class == TEXT("xDomPointA") || A.Class == TEXT("xDomPointB"))
		{
			DomPointNames.Add(A.Class.Right(1), A.Name);
		}
		else if ((A.Class == TEXT("xRedFlagBase") || A.Class == TEXT("xBlueFlagBase")) && ModeId.Equals(TEXT("CTF"), ESearchCase::IgnoreCase))
		{
			const int32 Team = A.Class == TEXT("xRedFlagBase") ? 0 : 1;
			if (!FlagBaseActors[Team])
			{
				if (FUE2SvActor* Base = Server.CreateMapActor(MapName + TEXT(".") + A.Name, FString(), TEXT("FlagBase")))
				{
					FlagBaseActors[Team] = Base->Id;
					Base->Location = UU(A.Location * S);
				}
			}
		}
	}
	if (FUE2SvActor* LI = Server.CreateMapActor(MapName + TEXT(".") + LevelInfoName, TEXT("Engine.LevelInfo"), TEXT("LevelInfo")))
	{
		LevelInfoActor = LI->Id;
		LI->UpdateInterval = 1.f;
	}
	// remote players driving our vehicles: their ServerDrive / VehicleFire input (ArenaONS_Vehicle.h)
	GArenaONSExternalInput = [this](const APawn* Seat, AController* Controller, FArenaONS_VehicleInput& Out)
	{
		FRemotePlayer* P = FindPlayerByController(Controller);
		if (!P)
		{
			return false;
		}
		Out.Throttle = FMath::Clamp(P->DriveForward / 5000.f, -1.f, 1.f);
		Out.Steer = FMath::Clamp(P->DriveStrafe / 5000.f, -1.f, 1.f);
		Out.Rise = FMath::Clamp(P->DriveUp / 5000.f, -1.f, 1.f) + (P->bDriveJump ? 1.f : 0.f);
		Out.bJump = P->bDriveJump;
		Out.bFire = P->bVehicleFire[0];
		Out.bAltFire = P->bVehicleFire[1];
		Out.bUse = P->bVehicleUse;
		P->bVehicleUse = false;
		Out.AimRotation = P->View;
		Out.DesiredYaw = float(P->View.Yaw);
		Out.bHasDesiredYaw = true;
		return true;
	};
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: world %s (%s): level info %s, %d map actors read"), *MapName, *ModeId, LevelInfoActor ? *LevelInfoName : TEXT("missing"), MapActors.Num());
}

FUE2ServerWorld::~FUE2ServerWorld()
{
	GArenaONSExternalInput = nullptr;
	// remote players leave the match with the server
	for (TPair<int32, FRemotePlayer>& P : Players)
	{
		AUE2RemoteController* C = P.Value.Controller.Get();
		UWorld* CW = C ? C->GetWorld() : nullptr;
		if (C && CW && !CW->bIsTearingDown)
		{
			if (AArenaGameMode* GM = CW->GetAuthGameMode<AArenaGameMode>())
			{
				GM->RemovePawn(C);
				GM->Logout(C);
			}
			C->Destroy();
		}
	}
	Players.Reset();
}

FUE2SvActor* FUE2ServerWorld::ActorFor(UObject* Native, const FString& ClassPath, FName Kind, bool* bOutNew)
{
	if (bOutNew)
	{
		*bOutNew = false;
	}
	if (int32* Id = NativeActors.Find(Native))
	{
		FUE2SvActor* A = Server.FindActor(*Id);
		if (A && A->ClassPath == ClassPath)
		{
			A->SeenFrame = Server.Frame;
			return A;
		}
		if (A)
		{
			Server.DestroyActor(*Id);
		}
		NativeActors.Remove(Native);
	}
	if (ClassPath.IsEmpty())
	{
		return nullptr;
	}
	FUE2SvActor* A = Server.CreateActor(ClassPath, Kind);
	if (!A)
	{
		return nullptr;
	}
	A->Native = Native;
	A->SeenFrame = Server.Frame;
	NativeActors.Add(Native, A->Id);
	if (bOutNew)
	{
		*bOutNew = true;
	}
	return A;
}

int32 FUE2ServerWorld::PRIActorOf(const APlayerState* PS) const
{
	const int32* Id = PS ? NativeActors.Find(const_cast<APlayerState*>(PS)) : nullptr;
	return Id ? *Id : 0;
}

int32 FUE2ServerWorld::PawnActorOf(const AActor* Pawn) const
{
	const int32* Id = Pawn ? NativeActors.Find(const_cast<AActor*>(Pawn)) : nullptr;
	return Id ? *Id : 0;
}

int32 FUE2ServerWorld::GetControllerActor(int32 ConnId) const
{
	const FRemotePlayer* P = Players.Find(ConnId);
	return P ? P->PCActor : 0;
}

bool FUE2ServerWorld::GetViewLocation(int32 ConnId, FVector3f& OutLocation) const
{
	const FRemotePlayer* P = Players.Find(ConnId);
	if (!P)
	{
		return false;
	}
	const AUE2RemoteController* C = P->Controller.Get();
	if (const APawn* Pawn = C ? C->GetPawn() : nullptr)
	{
		// the eyes (UE2 relevancy traces from the viewer's eyes)
		OutLocation = UU(Pawn->GetPawnViewLocation());
	}
	else if (const FUE2SvActor* Target = P->ViewTargetActor ? Server.FindActor(P->ViewTargetActor) : nullptr)
	{
		OutLocation = Target->Location + FVector3f(0.f, 0.f, 30.f);
	}
	else
	{
		OutLocation = UU(P->SpectatorLocation);
	}
	return true;
}

FString FUE2ServerWorld::UE2WeaponClass(const UClass* WeaponClass, int32 Which)
{
	for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
	{
		const FString Name = C->GetName();
		for (const FWeaponNames& W : WeaponTable)
		{
			if (Name == W.Native)
			{
				const TCHAR* Out = Which == 0 ? W.Weapon : Which == 1 ? W.Attachment : Which == 2 ? W.Pickup : W.AmmoPickup;
				return Out;
			}
		}
	}
	return FString();
}

FString FUE2ServerWorld::UE2DamageType(const FString& WeaponName, bool bHeadshot)
{
	if (bHeadshot)
	{
		return TEXT("XWeapons.DamTypeSniperHeadShot");
	}
	static const TMap<FString, FString> Types = {
		{ TEXT("Gauntlet"), TEXT("XWeapons.DamTypeShieldImpact") }, { TEXT("Carbine"), TEXT("XWeapons.DamTypeAssaultBullet") },
		{ TEXT("Goo Launcher"), TEXT("XWeapons.DamTypeBioGlob") }, { TEXT("Pulse Rifle"), TEXT("XWeapons.DamTypeShockBeam") },
		{ TEXT("Arc Linker"), TEXT("XWeapons.DamTypeLinkPlasma") }, { TEXT("Rotary Gun"), TEXT("XWeapons.DamTypeMinigunBullet") },
		{ TEXT("Shard Cannon"), TEXT("XWeapons.DamTypeFlakChunk") }, { TEXT("Rocket Launcher"), TEXT("XWeapons.DamTypeRocket") },
		{ TEXT("Rail Rifle"), TEXT("XWeapons.DamTypeSniperShot") }, { TEXT("Insta Rifle"), TEXT("XWeapons.DamTypeSuperShockBeam") },
		{ TEXT("Translocator"), TEXT("XWeapons.DamTypeTelefragged") }, { TEXT("Warhead Launcher"), TEXT("XWeapons.DamTypeRedeemer") },
		{ TEXT("Orbital Designator"), TEXT("XWeapons.DamTypeIonBlast") }, { TEXT("Marksman Rifle"), TEXT("UTClassic.DamTypeClassicSniper") },
		{ TEXT("Seeker Launcher"), TEXT("Onslaught.DamTypeONSAVRiLRocket") }, { TEXT("Sticky Launcher"), TEXT("Onslaught.DamTypeONSGrenade") },
		{ TEXT("Crawler Layer"), TEXT("Onslaught.DamTypeONSMine") }, { TEXT("Fell"), TEXT("Engine.Fell") }, { TEXT("Drowned"), TEXT("Engine.Drowned") },
		{ TEXT("Crushed"), TEXT("Engine.Crushed") }, { TEXT("Ball Launcher"), TEXT("XWeapons.DamTypeShieldImpact") },
	};
	const FString* T = Types.Find(WeaponName);
	return T ? *T : FString(TEXT("Engine.DamageType"));
}

// =================================================================================================
// Refresh: the native game -> server actors
// =================================================================================================

void FUE2ServerWorld::Refresh(float DeltaSeconds)
{
	UWorld* W = World.Get();
	AArenaGameMode* GM = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
	AArenaGameState* GS = GM ? GM->GetArenaGameState() : nullptr;
	if (!W || !GM || !GS)
	{
		return;
	}
	if (FUE2SvActor* LI = Server.FindActor(LevelInfoActor))
	{
		LI->SeenFrame = Server.Frame;
		LI->bAlwaysRelevant = true;
	}
	RefreshGame(GM, GS);
	RefreshPlayerStates(GS);
#if !UE_BUILD_SHIPPING
	{
		// test hook -UE2ServerTestDrive=<seconds>: the remote players' pawns then enter the nearest free vehicle (they cannot
		// be steered there in tests)
		static float DriveAt = -1.f;
		static bool bParsed = false;
		if (!bParsed)
		{
			bParsed = true;
			FParse::Value(FCommandLine::Get(), TEXT("UE2ServerTestDrive="), DriveAt);
		}
		static double NextDriveTry = 0.0;
		if (DriveAt > 0.f && FPlatformTime::Seconds() - StartTime > DriveAt && FPlatformTime::Seconds() >= NextDriveTry)
		{
			// tried once a second until a remote player is in a vehicle (a pawn and a free vehicle are needed)
			NextDriveTry = FPlatformTime::Seconds() + 1.0;
			for (TPair<int32, FRemotePlayer>& P : Players)
			{
				AArenaCharacter* Ch = P.Value.Controller.IsValid() ? Cast<AArenaCharacter>(P.Value.Controller->GetPawn()) : nullptr;
				AArenaONS_Vehicle* Best = nullptr;
				double BestD = TNumericLimits<double>::Max();
				for (TActorIterator<AArenaONS_Vehicle> It(World.Get()); It && Ch; ++It)
				{
					const double D = FVector::DistSquared(It->GetActorLocation(), Ch->GetActorLocation());
					static const bool bMulti = FParse::Param(FCommandLine::Get(), TEXT("UE2ServerTestDriveSeats"));
					if (!It->HasDriver() && !It->IsDestroyedVehicle() && D < BestD && (!bMulti || It->GetNumSeats() > 1))
					{
						BestD = D;
						Best = *It;
					}
				}
				if (Best && Ch)
				{
					const bool bIn = Best->EnterSeat(Ch, 0);
					if (bIn)
					{
						DriveAt = -1.f;
					}
					UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTestDrive: %s into %s: %s"), *P.Value.Controller->PlayerState->GetPlayerName(), *Best->GetVehicleName(), bIn ? TEXT("entered") : TEXT("refused"));
				}
			}
		}
	}
	{
		// test hook -UE2ServerTestGunner=<seconds>: a bot takes the gunner seat of the free multi-seat vehicle nearest to the
		// first remote player (its turret then turns to its targets on the clients); retaken every second while free
		static float GunnerAt = -1.f;
		static bool bGunnerParsed = false;
		static double NextGunnerTry = 0.0;
		if (!bGunnerParsed)
		{
			bGunnerParsed = true;
			FParse::Value(FCommandLine::Get(), TEXT("UE2ServerTestGunner="), GunnerAt);
		}
		const double TNow = FPlatformTime::Seconds();
		if (GunnerAt > 0.f && TNow - StartTime > GunnerAt && TNow >= NextGunnerTry && Players.Num() > 0)
		{
			NextGunnerTry = TNow + 1.0;
			const APawn* Viewer = nullptr;
			for (TPair<int32, FRemotePlayer>& P : Players)
			{
				Viewer = P.Value.Controller.IsValid() ? P.Value.Controller->GetPawn() : nullptr;
				if (Viewer)
				{
					break;
				}
			}
			AArenaCharacter* Bot = nullptr;
			for (TActorIterator<AArenaCharacter> It(World.Get()); It && !Bot; ++It)
			{
				if (!It->IsDead() && It->GetController() && It->GetController()->GetPawn() == *It && !It->GetController()->IsPlayerController()
					&& !Cast<AUE2RemoteController>(It->GetController()))
				{
					Bot = *It;
				}
			}
			AArenaONS_Vehicle* Best = nullptr;
			double BestD = TNumericLimits<double>::Max();
			for (TActorIterator<AArenaONS_Vehicle> It(World.Get()); It && Viewer; ++It)
			{
				const double D = FVector::DistSquared(It->GetActorLocation(), Viewer->GetActorLocation());
				if (It->GetNumSeats() > 1 && !It->GetSeatCharacter(1) && !It->IsDestroyedVehicle() && D < BestD)
				{
					BestD = D;
					Best = *It;
				}
			}
			// kept up for the test: a bot takes the seat again whenever it is left
			if (Best && Bot && Best->EnterSeat(Bot, 1))
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTestGunner: %s in the gunner seat of %s"), Bot->GetPlayerState() ? *Bot->GetPlayerState()->GetPlayerName() : TEXT("?"), *Best->GetVehicleName());
			}
		}
	}
	{
		// test hook -UE2ServerTestMessages=<seconds>: from then on, one message every 2 s to each remote player through the
		// same path as the game's events (head shot, multi kills 1..3, then sprees: ours, another's, its end), for on-screen checks of the stock clients
		static float MsgAt = -1.f;
		static bool bMsgParsed = false;
		static int32 MsgStep = 0;
		if (!bMsgParsed)
		{
			bMsgParsed = true;
			FParse::Value(FCommandLine::Get(), TEXT("UE2ServerTestMessages="), MsgAt);
		}
		if (MsgAt > 0.f && FPlatformTime::Seconds() - StartTime > MsgAt + 2.f * MsgStep && MsgStep < 7)
		{
			for (TPair<int32, FRemotePlayer>& P : Players)
			{
				AUE2RemoteController* C = P.Value.Controller.Get();
				if (!C)
				{
					continue;
				}
				// another player for the spree lines (a bot)
				APlayerState* Other = nullptr;
				if (AGameStateBase* MGS = World.IsValid() ? World->GetGameState() : nullptr)
				{
					for (APlayerState* PS : MGS->PlayerArray)
					{
						if (PS && PS != C->PlayerState && !Other)
						{
							Other = PS;
						}
					}
				}
				if (MsgStep == 0)
				{
					OnLocalMessage(TEXT("XGame.SpecialKillMessage"), 0, nullptr, nullptr, C);
				}
				else if (MsgStep <= 3)
				{
					OnLocalMessage(TEXT("XGame.MultiKillMessage"), MsgStep, nullptr, nullptr, C);
				}
				else if (MsgStep == 4)
				{
					// KillingSpreeMessage as the game sends it (ArenaGameMode): our own spree, another's, its end
					OnLocalMessage(TEXT("UnrealGame.KillingSpreeMessage"), 0, C->PlayerState, nullptr, C);
				}
				else if (MsgStep == 5 && Other)
				{
					OnLocalMessage(TEXT("UnrealGame.KillingSpreeMessage"), 1, Other, nullptr, C);
				}
				else if (MsgStep == 6 && Other)
				{
					OnLocalMessage(TEXT("UnrealGame.KillingSpreeMessage"), 0, Other, C->PlayerState, C);
				}
			}
			UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTestMessages: step %d"), MsgStep);
			++MsgStep;
		}
	}
#endif
	RefreshVehicles();
	RefreshPawns(DeltaSeconds);
	RefreshControllers();
	RefreshProjectiles();
	RefreshPickups();
	RefreshMovers();
	RefreshFlags();
	RefreshObjectives();
	// actors whose native object is gone
	TArray<int32> Stale;
	for (auto It = NativeActors.CreateIterator(); It; ++It)
	{
		FUE2SvActor* A = Server.FindActor(It.Value());
		if (!A || A->SeenFrame != Server.Frame)
		{
			Stale.Add(It.Value());
			It.RemoveCurrent();
		}
	}
	for (int32 Id : Stale)
	{
		Server.DestroyActor(Id);
		Attachments.Remove(Id);
		DeadPawns.Remove(Id);
		FlashCounts.Remove(Id);
	}
	// beams and shields of pawns that are gone
	for (TMap<int32, int32>* Effects : { &BeamActors, &ShieldActors })
	{
		for (auto It = Effects->CreateIterator(); It; ++It)
		{
			FUE2SvActor* E = Server.FindActor(It.Value());
			if (!E || E->SeenFrame != Server.Frame)
			{
				Server.DestroyActor(It.Value());
				It.RemoveCurrent();
			}
		}
	}
	for (auto It = PawnStates.CreateIterator(); It; ++It)
	{
		if (!Server.FindActor(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	// shot effects: sent for a moment, then gone (their clients keep bNetTemporary ones)
	const double NowTemp = FPlatformTime::Seconds();
	for (auto It = TempActors.CreateIterator(); It; ++It)
	{
		if (NowTemp - It.Value() > 0.5)
		{
			Server.DestroyActor(It.Key());
			It.RemoveCurrent();
		}
	}
	for (auto It = Attachments.CreateIterator(); It; ++It)
	{
		FUE2SvActor* A = Server.FindActor(It.Value());
		if (!A || A->SeenFrame != Server.Frame)
		{
			Server.DestroyActor(It.Value());
			It.RemoveCurrent();
		}
	}
}

void FUE2ServerWorld::RefreshGame(AArenaGameMode* GM, AArenaGameState* GS)
{
	bool bNew = false;
	// the game's own GRI class (Assault: round time and objectives progress, Invasion: waves)
	FString GRIClass = TEXT("Engine.GameReplicationInfo");
	if (ModeId.Equals(TEXT("AS"), ESearchCase::IgnoreCase)) GRIClass = TEXT("UT2k4Assault.ASGameReplicationInfo");
	else if (ModeId.Equals(TEXT("INV"), ESearchCase::IgnoreCase)) GRIClass = TEXT("SkaarjPack.InvasionGameReplicationInfo");
	if (!Server.FindClass(GRIClass))
	{
		GRIClass = TEXT("Engine.GameReplicationInfo");
	}
	FUE2SvActor* GRI = ActorFor(GS, GRIClass, TEXT("GRI"), &bNew);
	if (!GRI)
	{
		return;
	}
	GRIActor = GRI->Id;
	GRI->bAlwaysRelevant = true;
	GRI->UpdateInterval = 0.25f;
	GRI->NetPriority = 1.f;
	const bool bTeams = GS->IsTeamGame();
	const bool bPlaying = GS->Phase == EArenaMatchPhase::Playing;
	GRI->Set(TEXT("bStopCountDown"), UE2Codec::Bool(!bPlaying || GS->TimeLimit <= 0 || GS->bOvertime));
	GRI->Set(TEXT("bMatchHasBegun"), UE2Codec::Bool(GS->Phase != EArenaMatchPhase::Countdown));
	GRI->Set(TEXT("GameName"), UE2Codec::Str(GameNameOf(ModeId)), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("GameClass"), UE2Codec::Str(Server.GetGameClass()), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("bTeamGame"), UE2Codec::Bool(bTeams), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("RemainingTime"), UE2Codec::Int(GS->RemainingTime), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("ElapsedTime"), UE2Codec::Int(int32(FPlatformTime::Seconds() - StartTime)), FUE2SvProp::InitialOnly);
	const FString Name = Server.GetSettings().ServerName;
	GRI->Set(TEXT("ServerName"), UE2Codec::Str(Name), FUE2SvProp::InitialOnly);
	// server info from the host's options (a dedicated server's ini): ?ShortName= ?AdminName= ?AdminEmail= ?MOTD=<lines|...>
	{
		const FString Opt = TEXT("?") + GM->GetMatchOptions();
		auto Decode = [](const FString& In)
		{
			FString Out;
			for (int32 i = 0; i < In.Len(); ++i)
			{
				if (In[i] == '%' && i + 2 < In.Len() && FChar::IsHexDigit(In[i + 1]) && FChar::IsHexDigit(In[i + 2]))
				{
					Out.AppendChar(TCHAR(FParse::HexNumber(*In.Mid(i + 1, 2))));
					i += 2;
				}
				else
				{
					Out.AppendChar(In[i]);
				}
			}
			return Out;
		};
		const FString Short = Decode(UGameplayStatics::ParseOption(Opt, TEXT("ShortName")));
		GRI->Set(TEXT("ShortName"), UE2Codec::Str(Short.IsEmpty() ? Name.Left(20) : Short.Left(20)), FUE2SvProp::InitialOnly);
		const FString Admin = Decode(UGameplayStatics::ParseOption(Opt, TEXT("AdminName")));
		const FString Email = Decode(UGameplayStatics::ParseOption(Opt, TEXT("AdminEmail")));
		const FString Motd = Decode(UGameplayStatics::ParseOption(Opt, TEXT("MOTD")));
		if (!Admin.IsEmpty())
		{
			GRI->Set(TEXT("AdminName"), UE2Codec::Str(Admin), FUE2SvProp::InitialOnly);
		}
		if (!Email.IsEmpty())
		{
			GRI->Set(TEXT("AdminEmail"), UE2Codec::Str(Email), FUE2SvProp::InitialOnly);
		}
		if (!Motd.IsEmpty())
		{
			// GameReplicationInfo.MessageOfTheDay: the lines as one text (UT2004's server info page shows it)
			GRI->Set(TEXT("MessageOfTheDay"), UE2Codec::Str(Motd.Replace(TEXT("|"), TEXT(" "))), FUE2SvProp::InitialOnly);
		}
	}
	GRI->Set(TEXT("GoalScore"), UE2Codec::Int(GS->GoalScore), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("TimeLimit"), UE2Codec::Int(GS->TimeLimit / 60), FUE2SvProp::InitialOnly);
	GRI->Set(TEXT("MinNetPlayers"), UE2Codec::Int(1), FUE2SvProp::InitialOnly);
	// RemainingMinute: the clients count down themselves; the server corrects them once a minute (GameInfo.Timer)
	const double Now = FPlatformTime::Seconds();
	if (bNew || Now - LastRemainingMinute >= 60.0 || !bPlaying)
	{
		LastRemainingMinute = Now;
		GRI->Set(TEXT("RemainingMinute"), UE2Codec::Int(GS->RemainingTime));
	}
	if (bTeams)
	{
		for (int32 T = 0; T < 2; ++T)
		{
			FUE2SvActor* Team = TeamActors[T] ? Server.FindActor(TeamActors[T]) : nullptr;
			if (!Team)
			{
				Team = Server.CreateActor(TEXT("XGame.xTeamRoster"), TEXT("Team"));
				TeamActors[T] = Team ? Team->Id : 0;
			}
			if (!Team)
			{
				continue;
			}
			Team->SeenFrame = Server.Frame;
			Team->bAlwaysRelevant = true;
			Team->UpdateInterval = 0.25f;
			Team->Set(TEXT("TeamIndex"), UE2Codec::Int(T), FUE2SvProp::InitialOnly);
			Team->Set(TEXT("Score"), UE2Codec::Float(float(GS->TeamScores[T])));
			if (FlagBaseActors[T])
			{
				Team->Set(TEXT("HomeBase"), UE2Codec::Actor(FlagBaseActors[T]));
			}
			GRI->Set(FString::Printf(TEXT("Teams[%d]"), T), UE2Codec::Actor(Team->Id));
		}
	}
	// last man standing: the HUD shows MaxLives - Deaths
	int32 MaxLives = 0;
	for (APlayerState* Base : GS->PlayerArray)
	{
		if (const AArenaPlayerState* PS = Cast<AArenaPlayerState>(Base))
		{
			if (PS->Lives >= 0)
			{
				MaxLives = FMath::Max(MaxLives, PS->Lives + PS->Deaths);
			}
		}
	}
	if (MaxLives > 0)
	{
		GRI->Set(TEXT("MaxLives"), UE2Codec::Int(MaxLives));
	}
	if (GRI->ClassPath == TEXT("UT2k4Assault.ASGameReplicationInfo"))
	{
		TActorIterator<AArenaAS_Info> It(GM->GetWorld());
		if (It)
		{
			GRI->Set(TEXT("bTeamZeroIsAttacking"), UE2Codec::Bool(It->AttackingTeam == 0));
			GRI->Set(TEXT("CurrentRound"), UE2Codec::Int(It->PairRound + 1));
			GRI->Set(TEXT("MaxRounds"), UE2Codec::Int(2));
			GRI->Set(TEXT("ObjectiveProgress"), UE2Codec::Byte(It->CurrentStage));
		}
	}
	else if (GRI->ClassPath == TEXT("SkaarjPack.InvasionGameReplicationInfo"))
	{
		if (const AArenaINV_WaveState* WS = AArenaINV_WaveState::Get(GM))
		{
			GRI->Set(TEXT("WaveNumber"), UE2Codec::Byte(FMath::Max(0, WS->Wave - 1)));
			// InvasionGameReplicationInfo replicates WaveNumber and BaseDifficulty (FinalWave is the game's, not replicated)
			GRI->Set(TEXT("BaseDifficulty"), UE2Codec::Byte(uint8(FMath::Clamp(GM ? GM->GetBotSkill() : 3, 0, 7))), FUE2SvProp::InitialOnly);
		}
	}
	RefreshAnnouncements(GS);
	if (GS->Phase == EArenaMatchPhase::Ended)
	{
		if (bTeams && (GS->WinningTeam == 0 || GS->WinningTeam == 1))
		{
			GRI->Set(TEXT("Winner"), UE2Codec::Actor(TeamActors[GS->WinningTeam]));
		}
		else if (GS->WinningPlayer)
		{
			GRI->Set(TEXT("Winner"), UE2Codec::Actor(PRIActorOf(GS->WinningPlayer)));
		}
	}
}

void FUE2ServerWorld::RefreshPlayerStates(AArenaGameState* GS)
{
	const bool bTeams = GS->IsTeamGame();
	for (APlayerState* Base : GS->PlayerArray)
	{
		AArenaPlayerState* PS = Cast<AArenaPlayerState>(Base);
		if (!PS)
		{
			continue;
		}
		FUE2SvActor* A = ActorFor(PS, ModeId.Equals(TEXT("ONS"), ESearchCase::IgnoreCase) ? TEXT("Onslaught.ONSPlayerReplicationInfo") : TEXT("XGame.xPlayerReplicationInfo"), TEXT("PRI"));
		if (!A)
		{
			continue;
		}
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.5f;
		A->NetPriority = 1.f;
		A->Set(TEXT("PlayerName"), UE2Codec::Str(PS->GetPlayerName()));
		A->Set(TEXT("Score"), UE2Codec::Float(PS->GetScore()));
		A->Set(TEXT("Deaths"), UE2Codec::Float(float(PS->Deaths)));
		A->Set(TEXT("CharacterName"), UE2Codec::Str(PS->CharacterName.IsEmpty() ? FString(TEXT("Jakob")) : PS->CharacterName));
		{
			// PlayerReplicationInfo.bIsFemale (xPawn.Setup from the record's Sex): female voices and taunts on the clients
			const ArenaSP::FPlayerRecord* Rec = ArenaSP::FindPlayer(PS->CharacterName.IsEmpty() ? FString(TEXT("Jakob")) : PS->CharacterName);
			A->Set(TEXT("bIsFemale"), UE2Codec::Bool(Rec && Rec->Sex.Equals(TEXT("Female"), ESearchCase::IgnoreCase)));
		}
		A->Set(TEXT("PlayerID"), UE2Codec::Int(PS->GetPlayerId()), FUE2SvProp::InitialOnly);
		A->Set(TEXT("bBot"), UE2Codec::Bool(PS->IsABot()), FUE2SvProp::InitialOnly);
		A->Set(TEXT("bNoTeam"), UE2Codec::Bool(!bTeams), FUE2SvProp::InitialOnly);
		A->Set(TEXT("bOnlySpectator"), UE2Codec::Bool(PS->IsOnlyASpectator()));
		A->Set(TEXT("bIsSpectator"), UE2Codec::Bool(PS->IsSpectator() || PS->IsOnlyASpectator()));
		A->Set(TEXT("bOutOfLives"), UE2Codec::Bool(PS->bEliminated));
		A->Set(TEXT("Ping"), UE2Codec::Byte(PS->IsABot() ? 0 : FMath::Clamp(int32(PS->GetPingInMilliseconds()) / 4, 0, 255)), FUE2SvProp::NotOwner);
		if (bTeams)
		{
			A->Set(TEXT("Team"), UE2Codec::Actor(TeamActor(PS->Team)));
			A->Set(TEXT("TeamID"), UE2Codec::Int(FMath::Max(0, PS->GetPlayerId() % 32)));
		}
		// the CTF flag the player carries (HUD flag icons)
		int32 FlagActor = 0;
		if (const AController* C = PS->GetOwningController())
		{
			if (const AArenaCharacter* Ch = Cast<AArenaCharacter>(C->GetPawn()))
			{
				FlagActor = Ch->CarriedFlag ? PawnActorOf(Ch->CarriedFlag) : 0;
			}
		}
		A->Set(TEXT("HasFlag"), UE2Codec::Actor(FlagActor));
		// PlayerReplicationInfo.UpdatePlayerLocation: PlayerZone / PlayerVolume of the pawn (map actors, static
		// references), for GetLocationName (team chat "Name (location): text", the scoreboard); none while dead
		FUE2NetValue Zone = UE2Codec::None(), Volume = UE2Codec::None();
		const APawn* LocPawn = PS->GetPawn();
		const UArenaWorldZoneSubsystem* Zones = LocPawn ? UArenaWorldZoneSubsystem::Get(LocPawn) : nullptr;
		if (Zones)
		{
			const FVector Loc = LocPawn->GetActorLocation();
			if (const AArenaZoneInfo* Z = Zones->GetZoneAt(Loc))
			{
				const FUE2ObjectRef Ref = Z->UE2Name.IsEmpty() ? FUE2ObjectRef() : Server.FindStatic(MapName + TEXT(".") + Z->UE2Name);
				if (Ref.IsStatic())
				{
					Zone = UE2Codec::Static(Ref);
				}
			}
			if (const AArenaPhysicsVolume* V = Zones->GetPhysicsVolumeAt(Loc))
			{
				const FUE2ObjectRef Ref = V->UE2Name.IsEmpty() || V->LocationName.IsEmpty() ? FUE2ObjectRef() : Server.FindStatic(MapName + TEXT(".") + V->UE2Name);
				if (Ref.IsStatic())
				{
					Volume = UE2Codec::Static(Ref);
				}
			}
		}
		A->Set(TEXT("PlayerZone"), Zone);
		A->Set(TEXT("PlayerVolume"), Volume);
	}
}

void FUE2ServerWorld::RefreshControllers()
{
	const double Now = FPlatformTime::Seconds();
	for (TPair<int32, FRemotePlayer>& KV : Players)
	{
		FRemotePlayer& P = KV.Value;
		FUE2SvActor* PC = Server.FindActor(P.PCActor);
		AUE2RemoteController* C = P.Controller.Get();
		if (!PC || !C)
		{
			continue;
		}
		PC->SeenFrame = Server.Frame;
		PC->OwnerConn = P.ConnId;
		if (const FUE2ServerConnection* Conn = Server.FindConnection(P.ConnId))
		{
			if (C->PlayerState)
			{
				C->PlayerState->SetCompressedPing(uint8(FMath::Clamp(Conn->PingMs / 4, 0, 255)));
			}
		}
		PC->bOnlyRelevantToOwner = true;
		PC->UpdateInterval = 0.1f;
		PC->NetPriority = 3.f;
		APawn* Pawn = C->GetPawn();
		AArenaCharacter* Ch = Cast<AArenaCharacter>(Pawn);
		const bool bAlive = Ch && !Ch->IsDead();
		const FVector Loc = Pawn ? Pawn->GetActorLocation() : P.SpectatorLocation;
		PC->Location = UU(Loc);
		PC->Set(TEXT("Role"), UE2Codec::Byte(3));
		PC->Set(TEXT("bNetOwner"), UE2Codec::Bool(true));
		PC->Set(TEXT("PlayerReplicationInfo"), UE2Codec::Actor(PRIActorOf(C->PlayerState)));
		PC->Set(TEXT("GameReplicationInfo"), UE2Codec::Actor(GRIActor));
		const bool bInVehicle = Pawn && !Ch && (Cast<AArenaONS_Vehicle>(Pawn) || Cast<AArenaONS_VehicleSeat>(Pawn));
		PC->Set(TEXT("Pawn"), UE2Codec::Actor((bAlive || bInVehicle) ? PawnActorOf(Pawn) : 0));
		PC->Set(TEXT("Adrenaline"), UE2Codec::Int(Ch ? int32(Ch->Adrenaline) : 0));
		UpdatePossession(P);
		(void)Now;
	}
}

void FUE2ServerWorld::RefreshPawns(float DeltaSeconds)
{
	UWorld* W = World.Get();
	const double Now = FPlatformTime::Seconds();
	for (TActorIterator<AArenaCharacter> It(W); It; ++It)
	{
		AArenaCharacter* Ch = *It;
		if (!IsValid(Ch))
		{
			continue;
		}
		bool bNew = false;
		// invasion monsters are the stock SkaarjPack monsters
		FString PawnClass = TEXT("XGame.xPawn");
		const AArenaINV_Monster* Monster = Cast<AArenaINV_Monster>(Ch);
		if (Monster)
		{
			static const TCHAR* MonsterClasses[] = { TEXT("SkaarjPack.SkaarjPupae"), TEXT("SkaarjPack.RazorFly"), TEXT("SkaarjPack.Manta"), TEXT("SkaarjPack.Krall"),
				TEXT("SkaarjPack.EliteKrall"), TEXT("SkaarjPack.GasBag"), TEXT("SkaarjPack.Brute"), TEXT("SkaarjPack.Behemoth"), TEXT("SkaarjPack.Skaarj"),
				TEXT("SkaarjPack.IceSkaarj"), TEXT("SkaarjPack.FireSkaarj"), TEXT("SkaarjPack.Warlord") };
			const int32 Type = int32(Monster->GetMonsterType());
			PawnClass = Type >= 0 && Type < int32(UE_ARRAY_COUNT(MonsterClasses)) ? MonsterClasses[Type] : TEXT("SkaarjPack.Skaarj");
		}
		FUE2SvActor* A = ActorFor(Ch, PawnClass, TEXT("Pawn"), &bNew);
		if (!A)
		{
			continue;
		}
		const AUE2RemoteController* RC = Cast<AUE2RemoteController>(Ch->GetController());
		if (RC)
		{
			A->OwnerConn = RC->ConnId;
		}
		else if (!Ch->IsDead())
		{
			A->OwnerConn = 0;
		}
		const FRemotePlayer* Owner = A->OwnerConn ? Players.Find(A->OwnerConn) : nullptr;
		const int32 OwnerPC = Owner ? Owner->PCActor : 0;
		const FVector Loc = Ch->GetActorLocation();
		const FVector Vel = Ch->GetVelocity();
		const double Yaw = Ch->GetActorRotation().Yaw;
		A->Location = UU(Loc);
		A->Rotation = FIntVector(0, RotUU(Yaw), 0);
		A->UpdateInterval = 1.f / 30.f;
		// DeathMatch.CheckEndGame: EndGameFocus.bAlwaysRelevant (every view follows the winner's pawn)
		A->bAlwaysRelevant = A->Id == EndGameFocusActor;
		A->CullDistance = 0.f;
		A->bUseLineOfSight = true;
		A->NetPriority = 3.f;

		// roles from the client's side: our pawn is autonomous to its owner, simulated to everyone else
		A->Set(TEXT("Role"), UE2Codec::Byte(2));
		A->SetOwnerValue(TEXT("Role"), UE2Codec::Byte(3));
		if (OwnerPC)
		{
			A->Set(TEXT("Owner"), UE2Codec::Actor(OwnerPC), FUE2SvProp::OwnerOnly);
			A->Set(TEXT("Controller"), UE2Codec::Actor(OwnerPC), FUE2SvProp::OwnerOnly);
		}
		// a walking UE2 pawn rests WalkZOffset higher above its floor than ours: where the clients expect it
		const bool bWalkingPawn = Ch->GetCharacterMovement() && Ch->GetCharacterMovement()->IsMovingOnGround();
		const FVector NetLoc = bWalkingPawn ? Loc + FVector(0.0, 0.0, WalkZOffset * S) : Loc;
		A->Set(TEXT("PawnPosition"), PawnPosition(NetLoc, Yaw, Vel), FUE2SvProp::NotOwner, true);
		A->Set(TEXT("Physics"), UE2Codec::Byte(PhysicsOf(Ch)), FUE2SvProp::InitialOnly);
		if (const int32* Vehicle = DriverVehicles.Find(Ch))
		{
			A->Set(TEXT("DrivenVehicle"), UE2Codec::Actor(*Vehicle));
		}
		else
		{
			A->Set(TEXT("DrivenVehicle"), UE2Codec::None());
		}
		const FRotator View = Ch->GetViewRotation();
		A->Set(TEXT("ViewPitch"), UE2Codec::Byte((RotUU(View.Pitch) >> 8) & 255), FUE2SvProp::NotOwner, true);
		const APlayerState* PS = Ch->GetPlayerState();
		if (PS)
		{
			A->Set(TEXT("PlayerReplicationInfo"), UE2Codec::Actor(PRIActorOf(PS)));
		}
		A->Set(TEXT("Health"), UE2Codec::Int(FMath::Max(0, FMath::RoundToInt(Ch->Health))));
		A->Set(TEXT("ShieldStrength"), UE2Codec::Float(Ch->Shield));
		A->Set(TEXT("bIsCrouched"), UE2Codec::Bool(Ch->bIsCrouched));
		if (!Monster)
		{
			A->Set(TEXT("bInvis"), UE2Codec::Bool(Ch->IsInvisible()));
		}
		PawnEventSounds(*A, Ch);

		if (Ch->IsDead())
		{
			if (!DeadPawns.Contains(A->Id))
			{
				DeadPawns.Add(A->Id, Now);
				A->Set(TEXT("TearOffMomentum"), UE2Codec::Vec(UU(Vel * 0.5f)));
				A->Set(TEXT("TakeHitLocation"), UE2Codec::Vec(UU(Loc)));
			}
			A->Set(TEXT("bTearOff"), UE2Codec::Bool(true));
			A->bTearOff = true;
			continue;
		}

		if (Monster)
		{
			continue; // monsters fight with their own (client side) attacks: no weapon or attachment
		}
		// inventory (owner only) and the third person weapon
		AArenaWeapon* Current = Ch->GetCurrentWeapon();
		int32 AttachId = 0;
		if (Current)
		{
			const FString AttachClass = UE2WeaponClass(Current->GetClass(), 1);
			int32* Existing = Attachments.Find(A->Id);
			FUE2SvActor* Att = Existing ? Server.FindActor(*Existing) : nullptr;
			if (Att && Att->ClassPath != AttachClass)
			{
				Server.DestroyActor(Att->Id);
				Attachments.Remove(A->Id);
				Att = nullptr;
			}
			if (!Att && !AttachClass.IsEmpty())
			{
				Att = Server.CreateActor(AttachClass, TEXT("Attachment"));
				if (Att)
				{
					Attachments.Add(A->Id, Att->Id);
					FlashCounts.Add(Att->Id, Current->NetShotCount);
					A->Set(TEXT("AnimAction"), UE2Codec::Name(TEXT("Weapon_Switch")));
				}
			}
			if (Att)
			{
				Att->SeenFrame = Server.Frame;
				Att->Location = A->Location;
				Att->UpdateInterval = 1.f / 30.f;
				Att->OwnerConn = A->OwnerConn;
				Att->RelevancyParent = A->Id;
				Att->NetPriority = 2.f;
				Att->Set(TEXT("Instigator"), UE2Codec::Actor(A->Id));
				Att->Set(TEXT("Base"), UE2Codec::Actor(A->Id));
				// FlashCount: changes with every shot (0 = not firing), the fire mode with it
				int32& Last = FlashCounts.FindOrAdd(Att->Id);
				const FUE2SvProp* FC = Att->Props.Find(TEXT("FlashCount"));
				int32 Flash = FC ? FC->Value.I : 0;
				if (Current->NetShotCount != Last)
				{
					Last = Current->NetShotCount;
					PlayFireSound(*A, UE2WeaponClass(Current->GetClass(), 0), Current->NetShotMode);
					int32& LastHit = HitscanCounts.FindOrAdd(Att->Id);
					if (Current->NetHitscanCount != LastHit)
					{
						LastHit = Current->NetHitscanCount;
						SpawnShotEffect(*A, *Att, Current);
					}
					Flash = (Flash % 254) + 1;
					Att->Set(TEXT("FiringMode"), UE2Codec::Byte(Current->NetShotMode), FUE2SvProp::NotOwner);
				}
				else if (!Current->IsFiring(0) && !Current->IsFiring(1))
				{
					Flash = 0;
				}
				Att->Set(TEXT("FlashCount"), UE2Codec::Byte(Flash), FUE2SvProp::NotOwner);
				AttachId = Att->Id;
			}
		}
		// weapon loops (minigun firing, shield gun charging): xPawn.AmbientSound, as the stock fire modes set it
		FUE2ObjectRef Ambient;
		if (Current && (Current->IsFiring(0) || Current->IsFiring(1)))
		{
			const uint8 Mode = Current->IsFiring(0) ? 0 : 1;
			const FString WeaponClass = UE2WeaponClass(Current->GetClass(), 0);
			const FUE2ObjectRef FireClass = WeaponClass.IsEmpty() ? FUE2ObjectRef() : DefaultObject(WeaponClass, TEXT("FireModeClass"), Mode);
			const FString FirePath = FireClass.IsStatic() ? Server.GetMasterMap().GetObjectPath(FireClass) : FString();
			if (!FirePath.IsEmpty())
			{
				Ambient = DefaultObject(FirePath, TEXT("FiringSound"));
				if (!Ambient.IsStatic())
				{
					Ambient = DefaultObject(FirePath, TEXT("ChargingSound"));
				}
			}
		}
		A->Set(TEXT("AmbientSound"), Ambient.IsStatic() ? UE2Codec::Static(Ambient) : UE2Codec::None());
		{
			const AArenaPlayerState* TeamPS = Ch->GetPlayerState<AArenaPlayerState>();
			RefreshWeaponEffects(*A, AttachId, Current, TeamPS ? TeamPS->Team : -1);
		}
		if (Owner)
		{
			// the owner's inventory: each weapon an owner-only actor, linked through Inventory like UE2's list
			const TArray<TObjectPtr<AArenaWeapon>>& Weapons = Ch->GetWeapons();
			TArray<FUE2SvActor*> List;
			for (AArenaWeapon* Wp : Weapons)
			{
				if (!IsValid(Wp))
				{
					continue;
				}
				const FString WeaponClass = UE2WeaponClass(Wp->GetClass(), 0);
				FUE2SvActor* WA = ActorFor(Wp, WeaponClass, TEXT("Weapon"));
				if (!WA)
				{
					continue;
				}
				WA->OwnerConn = A->OwnerConn;
				WA->bOnlyRelevantToOwner = true;
				WA->Location = A->Location;
				WA->UpdateInterval = 0.2f;
				WA->NetPriority = 1.f;
				WA->Set(TEXT("Owner"), UE2Codec::Actor(A->Id));
				WA->Set(TEXT("Instigator"), UE2Codec::Actor(A->Id));
				WA->Set(TEXT("AmmoCharge[0]"), UE2Codec::Int(Wp->bInfiniteAmmo ? 999 : Wp->Ammo));
				WA->Set(TEXT("AmmoCharge[1]"), UE2Codec::Int(Wp->bInfiniteAmmo ? 999 : (Wp->AltMaxAmmo > 0 ? Wp->AltAmmo : Wp->Ammo)));
				WA->Set(TEXT("ThirdPersonActor"), UE2Codec::Actor(Wp == Current ? AttachId : 0));
				List.Add(WA);
			}
			for (int32 i = 0; i < List.Num(); ++i)
			{
				List[i]->Set(TEXT("Inventory"), UE2Codec::Actor(i + 1 < List.Num() ? List[i + 1]->Id : 0));
			}
			A->Set(TEXT("Inventory"), UE2Codec::Actor(List.Num() > 0 ? List[0]->Id : 0), FUE2SvProp::OwnerOnly);
		}
	}
	(void)DeltaSeconds;
}

void FUE2ServerWorld::RefreshProjectiles()
{
	for (TActorIterator<AArenaProjectile> It(World.Get()); It; ++It)
	{
		AArenaProjectile* P = *It;
		if (!IsValid(P))
		{
			continue;
		}
		const FString Class = ProjectileClass(P);
		bool bNew = false;
		FUE2SvActor* A = ActorFor(P, Class, TEXT("Projectile"), &bNew);
		if (!A)
		{
			continue;
		}
		const FVector Vel = P->GetVelocity();
		A->Location = UU(P->GetActorLocation());
		const FRotator Rot = Vel.IsNearlyZero() ? P->GetActorRotation() : Vel.Rotation();
		A->Rotation = FIntVector(RotUU(Rot.Pitch), RotUU(Rot.Yaw), RotUU(Rot.Roll));
		A->UpdateInterval = 0.1f;
		A->bUseLineOfSight = true;
		A->NetPriority = 2.5f;
		if (APawn* Inst = P->GetInstigator())
		{
			A->Set(TEXT("Instigator"), UE2Codec::Actor(PawnActorOf(Inst)), FUE2SvProp::InitialOnly);
		}
		A->Set(TEXT("Velocity"), UE2Codec::Vec(UU(Vel)), A->bNetTemporary ? FUE2SvProp::InitialOnly : FUE2SvProp::All, !A->bNetTemporary);
		if (!A->bNetTemporary)
		{
			A->Set(TEXT("Location"), UE2Codec::Vec(A->Location), FUE2SvProp::All, true);
		}
	}
}

void FUE2ServerWorld::RefreshPickups()
{
	for (TActorIterator<AArenaPickup> It(World.Get()); It; ++It)
	{
		AArenaPickup* P = *It;
		if (!IsValid(P))
		{
			continue;
		}
		FString Class = PickupClass(P);
		if (P->GetPickupType() == EArenaPickupType::Weapon)
		{
			Class = UE2WeaponClass(P->GetWeaponClass(), 2);
		}
		else if (P->GetPickupType() == EArenaPickupType::Ammo)
		{
			Class = UE2WeaponClass(P->GetWeaponClass(), 3);
		}
		FUE2SvActor* A = ActorFor(P, Class, TEXT("Pickup"));
		if (!A)
		{
			continue;
		}
		A->Location = UU(P->GetActorLocation());
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.5f;
		A->NetPriority = 1.4f;
		A->Set(TEXT("bHidden"), UE2Codec::Bool(!P->IsAvailable()));
		bool& bWasAvailable = PickupStates.FindOrAdd(A->Id, P->IsAvailable());
		if (bWasAvailable && !P->IsAvailable())
		{
			// taken: the pickup's PickupSound for everyone near, its MessageClass to the taker (Pickup.AnnouncePickup)
			HearSound(DefaultObject(Class, TEXT("PickupSound")), A->Location, 1.f, 300.f, 0, A->Id * 16 + 7);
			AArenaCharacter* Taker = nullptr;
			double Best = FMath::Square(250.0);
			for (TActorIterator<AArenaCharacter> CIt(World.Get()); CIt; ++CIt)
			{
				const double D = FVector::DistSquared(CIt->GetActorLocation(), P->GetActorLocation());
				if (!CIt->IsDead() && D < Best)
				{
					Best = D;
					Taker = *CIt;
				}
			}
			const FRemotePlayer* TakerPlayer = Taker ? FindPlayerByController(Taker->GetController()) : nullptr;
			const FUE2ObjectRef Message = DefaultObject(Class, TEXT("MessageClass"));
			if (TakerPlayer && Message.IsStatic())
			{
				CallPC(TakerPlayer->ConnId, TEXT("ReceiveLocalizedMessage"), { UE2Codec::Static(Message), FUE2NetValue(), UE2Codec::None(), UE2Codec::None(),
					UE2Codec::Static(Server.FindStatic(Class)) });
			}
		}
		bWasAvailable = P->IsAvailable();
	}
}

void FUE2ServerWorld::RefreshMovers()
{
	struct FMoveSeen
	{
		int32 Serial = -1;
		FVector FromLoc = FVector::ZeroVector;
		FRotator FromRot = FRotator::ZeroRotator;
		float Alpha = 0.f;
		float Rate = 1.f;
		int32 Key = 0;
		int32 PrevKey = 0;
	};
	static TMap<int32, FMoveSeen> Moves;
	for (TActorIterator<AArenaMover> It(World.Get()); It; ++It)
	{
		AArenaMover* M = *It;
		if (!IsValid(M) || M->UE2Name.IsEmpty())
		{
			continue;
		}
		int32* Id = NativeActors.Find(M);
		FUE2SvActor* A = Id ? Server.FindActor(*Id) : nullptr;
		if (!A)
		{
			if (Id)
			{
				continue; // not in the map package (tried before)
			}
			A = Server.CreateMapActor(MapName + TEXT(".") + M->UE2Name, FString(), TEXT("Mover"));
			NativeActors.Add(M, A ? A->Id : 0);
			if (!A)
			{
				continue;
			}
			A->Native = M;
		}
		A->SeenFrame = Server.Frame;
		A->UpdateInterval = 0.05f;
		A->Location = UU(M->GetNetLocation());
		FVector FromLoc;
		FRotator FromRot;
		int32 Key = 0, PrevKey = 0, Serial = 0;
		float Alpha = 0.f, Rate = 1.f;
		M->GetNetMoveState(FromLoc, FromRot, Key, PrevKey, Alpha, Rate, Serial);
		FMoveSeen& Seen = Moves.FindOrAdd(A->Id);
		if (Seen.Serial != Serial)
		{
			// a move started: UE2 Mover.InterpolateTo replicates where it starts and how fast it goes
			Seen.Serial = Serial;
			Seen.FromLoc = FromLoc;
			Seen.FromRot = FromRot;
			Seen.Alpha = Alpha;
			Seen.Rate = Rate;
			Seen.Key = Key;
			Seen.PrevKey = PrevKey;
		}
		if (Serial > 0)
		{
			A->Set(TEXT("SimOldPos"), UE2Codec::Vec(UU(Seen.FromLoc)));
			A->Set(TEXT("SimOldRotPitch"), UE2Codec::Int(RotUU(Seen.FromRot.Pitch)));
			A->Set(TEXT("SimOldRotYaw"), UE2Codec::Int(RotUU(Seen.FromRot.Yaw)));
			A->Set(TEXT("SimOldRotRoll"), UE2Codec::Int(RotUU(Seen.FromRot.Roll)));
			A->Set(TEXT("SimInterpolate"), UE2Codec::Vec(FVector3f(100.f * Seen.Alpha, 100.f * FMath::Max(0.01f, Seen.Rate), float(256 * Seen.PrevKey + Seen.Key))));
		}
		if (!M->IsMoving())
		{
			const FRotator R = M->GetNetRotation();
			A->Set(TEXT("RealPosition"), UE2Codec::Vec(UU(M->GetNetLocation())));
			A->Set(TEXT("RealRotation"), UE2Codec::Rot(FIntVector(RotUU(R.Pitch), RotUU(R.Yaw), RotUU(R.Roll))));
		}
	}
}

void FUE2ServerWorld::RefreshFlags()
{
	for (TActorIterator<AArenaFlag> It(World.Get()); It; ++It)
	{
		AArenaFlag* F = *It;
		if (!IsValid(F))
		{
			continue;
		}
		const int32 Team = F->GetTeam();
		FUE2SvActor* A = ActorFor(F, Team == 0 ? TEXT("XGame.xRedFlag") : TEXT("XGame.xBlueFlag"), TEXT("Flag"));
		if (!A)
		{
			continue;
		}
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.1f;
		A->NetPriority = 2.5f;
		A->Location = UU(F->GetFlagLocation());
		const AArenaCharacter* Holder = F->GetHolder();
		const bool bHeld = F->GetState() == EArenaFlagState::Carried && Holder;
		A->Set(TEXT("Team"), UE2Codec::Actor(TeamActor(Team)));
		A->Set(TEXT("bHome"), UE2Codec::Bool(F->IsHome()));
		A->Set(TEXT("bHeld"), UE2Codec::Bool(bHeld));
		A->Set(TEXT("HolderPRI"), UE2Codec::Actor(bHeld ? PRIActorOf(Holder->GetPlayerState()) : 0));
		A->Set(TEXT("Base"), UE2Codec::Actor(bHeld ? PawnActorOf(Holder) : 0));
		A->Set(TEXT("bOnlyDrawIfAttached"), UE2Codec::Bool(bHeld));
		A->Set(TEXT("AttachmentBone"), UE2Codec::Name(bHeld ? FName(TEXT("FlagHand")) : NAME_None));
		A->Set(TEXT("bCollideActors"), UE2Codec::Bool(!bHeld));
		A->Set(TEXT("bHidden"), UE2Codec::Bool(false));
		if (!bHeld)
		{
			A->Set(TEXT("Location"), UE2Codec::Vec(A->Location));
		}
		if (FUE2SvActor* Base = Server.FindActor(FlagBaseActors[FMath::Clamp(Team, 0, 1)]))
		{
			Base->SeenFrame = Server.Frame;
			Base->UpdateInterval = 0.25f;
			Base->Set(TEXT("bHidden"), UE2Codec::Bool(!F->IsHome()));
		}
		if (FUE2SvActor* GRI = Server.FindActor(GRIActor))
		{
			// GRI.FlagState: 0 home, 1 held by its own team, 2 held by the enemy, 3 down
			const int32 State = F->IsHome() ? 0 : (bHeld ? 2 : 3);
			GRI->Set(FString::Printf(TEXT("FlagState[%d]"), FMath::Clamp(Team, 0, 1)), UE2Codec::Byte(State));
		}
	}
}

void FUE2ServerWorld::RefreshObjectives()
{
	UWorld* W = World.Get();
	// double domination: the map's xDomPointA / xDomPointB
	for (TActorIterator<AArenaDOM_ControlPoint> It(W); It; ++It)
	{
		AArenaDOM_ControlPoint* Point = *It;
		const FString* Name = IsValid(Point) ? DomPointNames.Find(Point->Label.ToUpper()) : nullptr;
		if (!Name)
		{
			continue;
		}
		int32* Id = NativeActors.Find(Point);
		FUE2SvActor* A = Id ? Server.FindActor(*Id) : nullptr;
		if (!A)
		{
			if (Id)
			{
				continue;
			}
			A = Server.CreateMapActor(MapName + TEXT(".") + *Name, FString(), TEXT("DomPoint"));
			NativeActors.Add(Point, A ? A->Id : 0);
			if (!A)
			{
				continue;
			}
			A->Native = Point;
		}
		A->SeenFrame = Server.Frame;
		A->UpdateInterval = 0.2f;
		A->Location = UU(Point->GetActorLocation());
		A->Set(TEXT("ControllingTeam"), UE2Codec::Actor(TeamActor(Point->OwnerTeam)));
		A->Set(TEXT("bControllable"), UE2Codec::Bool(Point->bEnabled));
	}
	// onslaught: power nodes and cores are the map's own ONSPowerNode / ONSPowerCore actors (their state only)
	for (TActorIterator<AArenaONS_PowerNode> It(W); It; ++It)
	{
		AArenaONS_PowerNode* Node = *It;
		if (!IsValid(Node) || Node->UE2SourceName.IsEmpty())
		{
			continue;
		}
		int32* Id = NativeActors.Find(Node);
		FUE2SvActor* A = Id ? Server.FindActor(*Id) : nullptr;
		if (!A)
		{
			if (Id)
			{
				continue;
			}
			A = Server.CreateMapActor(MapName + TEXT(".") + Node->UE2SourceName, FString(), TEXT("Objective"));
			NativeActors.Add(Node, A ? A->Id : 0);
			if (!A)
			{
				continue;
			}
			A->Native = Node;
		}
		A->SeenFrame = Server.Frame;
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.25f;
		const int32 Team = Node->GetTeam();
		// ONSPowerCore.CoreStage: 0 active, 1 destroyed, 2 constructing, 4 neutral; DefenderTeamIndex 2 = nobody
		uint8 Stage = Node->IsActive() ? 0 : (Node->IsConstructing() ? 2 : 4);
		if (Node->IsCore() && Node->GetHealth() <= 0.f)
		{
			Stage = 1;
		}
		A->Set(TEXT("CoreStage"), UE2Codec::Byte(Stage));
		A->Set(TEXT("DefenderTeamIndex"), UE2Codec::Byte((Team == 0 || Team == 1) && !Node->IsNeutral() ? Team : 2));
		// the stock HUD shows Health / DamageCapacity: our health as that fraction of the class's capacity
		{
			FUSPropRef Capacity;
			const int32 Cap = (A->Class && Capacity.Resolve(A->Class, TEXT("DamageCapacity"))) ? Capacity.Get<int32>(A->Class->GetDefaultObject()) : 0;
			const int32 Health = Cap > 0 ? FMath::RoundToInt(Node->GetHealthFraction() * Cap) : FMath::RoundToInt(Node->GetHealth());
			A->Set(TEXT("Health"), UE2Codec::Int(FMath::Max(0, Health)));
		}
		A->Set(TEXT("bPoweredByRed"), UE2Codec::Bool(Node->IsAttackableBy(0) || Node->IsBuildableBy(0)));
		A->Set(TEXT("bPoweredByBlue"), UE2Codec::Bool(Node->IsAttackableBy(1) || Node->IsBuildableBy(1)));
	}
	// assault: the map's objectives (GameObjective: active, disabled when done, the defending team, health)
	for (TActorIterator<AArenaAS_Objective> It(W); It; ++It)
	{
		AArenaAS_Objective* Obj = *It;
		if (!IsValid(Obj) || Obj->UE2SourceName.IsEmpty())
		{
			continue;
		}
		int32* Id = NativeActors.Find(Obj);
		FUE2SvActor* A = Id ? Server.FindActor(*Id) : nullptr;
		if (!A)
		{
			if (Id)
			{
				continue;
			}
			A = Server.CreateMapActor(MapName + TEXT(".") + Obj->UE2SourceName, FString(), TEXT("Objective"));
			NativeActors.Add(Obj, A ? A->Id : 0);
			if (!A)
			{
				continue;
			}
			A->Native = Obj;
		}
		A->SeenFrame = Server.Frame;
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.25f;
		A->Set(TEXT("bActive"), UE2Codec::Bool(Obj->IsActive()));
		A->Set(TEXT("bDisabled"), UE2Codec::Bool(Obj->IsCompleted()));
		A->Set(TEXT("DefenderTeamIndex"), UE2Codec::Byte(FMath::Clamp(Obj->GetDefendingTeam(), 0, 2)));
		A->Set(TEXT("bIsCritical"), UE2Codec::Bool(Obj->IsContested()));
		if (Obj->GetMaxHealth() > 0.f && A->Class && A->Class->FindPropertyByName(TEXT("Health")))
		{
			A->Set(TEXT("Health"), UE2Codec::Int(FMath::Max(0, FMath::RoundToInt(Obj->GetHealth()))));
		}
	}
	// bombing run: the ball (XGame.xBombFlag, a GameObject like the CTF flags)
	for (TActorIterator<AArenaBR_Ball> It(W); It; ++It)
	{
		AArenaBR_Ball* Ball = *It;
		if (!IsValid(Ball))
		{
			continue;
		}
		FUE2SvActor* A = ActorFor(Ball, TEXT("XGame.xBombFlag"), TEXT("Ball"));
		if (!A)
		{
			continue;
		}
		A->bAlwaysRelevant = true;
		A->UpdateInterval = 0.05f;
		const AArenaCharacter* Carrier = Ball->GetCarrier();
		const bool bHeld = Ball->IsCarried() && Carrier;
		const FVector Loc = Ball->GetObjectLocation();
		A->Location = UU(Loc);
		A->Set(TEXT("bHome"), UE2Codec::Bool(Ball->IsHome()));
		A->Set(TEXT("bHeld"), UE2Codec::Bool(bHeld));
		A->Set(TEXT("HolderPRI"), UE2Codec::Actor(bHeld ? PRIActorOf(Carrier->GetPlayerState()) : 0));
		A->Set(TEXT("Base"), UE2Codec::Actor(bHeld ? PawnActorOf(Carrier) : 0));
		A->Set(TEXT("bOnlyDrawIfAttached"), UE2Codec::Bool(bHeld));
		A->Set(TEXT("AttachmentBone"), UE2Codec::Name(bHeld ? FName(TEXT("FlagHand")) : NAME_None));
		A->Set(TEXT("bHidden"), UE2Codec::Bool(false));
		if (!bHeld)
		{
			// thrown or lying: simulated by the clients from location, velocity and physics (falling while it moves)
			const FVector Vel = Ball->GetVelocity();
			A->Set(TEXT("Location"), UE2Codec::Vec(A->Location), FUE2SvProp::All, true);
			A->Set(TEXT("Velocity"), UE2Codec::Vec(UU(Vel)), FUE2SvProp::All, true);
			A->Set(TEXT("Physics"), UE2Codec::Byte(Vel.IsNearlyZero(1.f) ? 0 : 2));
		}
	}
}

void FUE2ServerWorld::SpawnShotEffect(const FUE2SvActor& Pawn, FUE2SvActor& Attachment, const AArenaWeapon* Weapon)
{
	const FString Native = Weapon->GetClass()->GetName();
	const FVector3f End = UU(Weapon->NetShotEnd);
	// bullets: the attachment spawns the impacts from mHitLocation each time SpawnHitCount changes
	if (Native.Contains(TEXT("Carbine")) || Native.Contains(TEXT("RotaryGun")))
	{
		const FUE2SvProp* Count = Attachment.Props.Find(TEXT("SpawnHitCount"));
		Attachment.Set(TEXT("mHitLocation"), UE2Codec::Vec(End));
		Attachment.Set(TEXT("SpawnHitCount"), UE2Codec::Byte(((Count ? Count->Value.I : 0) + 1) & 255));
		return;
	}
	// beams: an effect actor from the start of the shot, aimed at where it hit (ShockBeamFire.SpawnBeamEffect)
	FString BeamClass;
	if (Native.Contains(TEXT("InstaRifle"))) BeamClass = TEXT("XWeapons.SuperShockBeamEffect");
	else if (Native.Contains(TEXT("PulseRifle"))) BeamClass = TEXT("XWeapons.ShockBeamEffect");
	else if (Native.Contains(TEXT("RailRifle")) && !Native.Contains(TEXT("Marksman"))) BeamClass = TEXT("XWeapons.NewLightningBolt");
	if (BeamClass.IsEmpty())
	{
		return;
	}
	FUE2SvActor* Beam = Server.CreateActor(BeamClass, TEXT("Effect"));
	if (!Beam)
	{
		return;
	}
	const FVector Start = Weapon->NetShotStart;
	const FRotator Dir = (Weapon->NetShotEnd - Start).Rotation();
	Beam->Location = UU(Start);
	Beam->Rotation = FIntVector(RotUU(Dir.Pitch), RotUU(Dir.Yaw), RotUU(Dir.Roll));
	Beam->UpdateInterval = 0.f;
	Beam->bUseLineOfSight = true;
	Beam->NetPriority = 3.f;
	Beam->Set(TEXT("mSpawnVecA"), UE2Codec::Vec(End));
	if (!Weapon->NetShotNormal.IsZero())
	{
		Beam->Set(TEXT("HitNormal"), UE2Codec::Vec(FVector3f(Weapon->NetShotNormal)));
	}
	Beam->Set(TEXT("Instigator"), UE2Codec::Actor(Pawn.Id));
	TempActors.Add(Beam->Id, FPlatformTime::Seconds());
}

TArray<TPair<FString, FName>> FUE2ServerWorld::DefaultVehicleWeapons(const FString& VehicleClass, const TCHAR* Array, const TCHAR* ClassMember)
{
	TArray<TPair<FString, FName>> Out;
	FUSClass* Class = Server.FindClass(VehicleClass);
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	FUSProperty* Prop = Def ? Def->FindProperty(FName(Array)) : nullptr;
	if (!Prop || !Def->Data || Prop->Type != EUSType::Array || !Prop->Inner || !Prop->Inner->Struct)
	{
		return Out;
	}
	FUSProperty* ClassProp = Prop->Inner->Struct->FindPropertyByName(FName(ClassMember));
	FUSProperty* BoneProp = Prop->Inner->Struct->FindPropertyByName(TEXT("WeaponBone"));
	const FUSScriptArray* Arr = reinterpret_cast<const FUSScriptArray*>(Def->Data + Prop->Offset);
	for (int32 i = 0; ClassProp && Arr->Data && i < Arr->Num && i < 8; ++i)
	{
		const uint8* Elem = Arr->Data + i * Prop->Inner->ElementSize;
		const FUSObject* WC = *reinterpret_cast<FUSObject* const*>(Elem + ClassProp->Offset);
		const FName Bone = BoneProp ? *reinterpret_cast<const FName*>(Elem + BoneProp->Offset) : NAME_None;
		if (WC)
		{
			Out.Emplace(WC->GetPathName(), Bone);
		}
	}
	return Out;
}

void FUE2ServerWorld::RefreshVehicles()
{
	DriverVehicles.Reset();
	for (TActorIterator<AArenaONS_Vehicle> It(World.Get()); It; ++It)
	{
		AArenaONS_Vehicle* V = *It;
		if (!IsValid(V))
		{
			continue;
		}
		// our vehicles as their stock counterparts (the same roles in ONS)
		static const TCHAR* Classes[] = { TEXT("Onslaught.ONSHoverBike"), TEXT("Onslaught.ONSRV"), TEXT("Onslaught.ONSHoverTank"), TEXT("Onslaught.ONSAttackCraft"),
			TEXT("Onslaught.ONSPRV") };
		const int32 Type = int32(V->GetVehicleType());
		const FString Class = Type >= 0 && Type < int32(UE_ARRAY_COUNT(Classes)) ? Classes[Type] : TEXT("Onslaught.ONSRV");
		bool bNew = false;
		FUE2SvActor* A = ActorFor(V, Class, TEXT("Vehicle"), &bNew);
		if (!A)
		{
			continue;
		}
		const FRotator Rot = V->GetActorRotation();
		// the stock vehicle's origin is its mesh's origin: our actor sits at the collision box centre, the mesh at
		// MeshOffset from it (ArenaArtVehicles). Without this the clients' Karma chassis floats above the ground (wheels
		// without contact: "Daredevil" stunt messages while standing still)
		const ArenaArtVehicles::FVehicleArt* Art = ArenaArtVehicles::Find(V->GetVehicleType());
		const FVector Loc = V->GetActorLocation() + (Art ? Rot.RotateVector(Art->MeshOffset) : FVector::ZeroVector);
		A->Location = UU(Loc);
		A->Rotation = FIntVector(RotUU(Rot.Pitch), RotUU(Rot.Yaw), RotUU(Rot.Roll));
		A->bUseLineOfSight = true;
		A->NetPriority = 3.f;
		A->UpdateInterval = 1.f / 20.f;
		const FRemotePlayer* Owner = FindPlayerByController(V->GetController());
		A->OwnerConn = Owner ? Owner->ConnId : 0;
		A->Set(TEXT("Role"), UE2Codec::Byte(2));
		A->SetOwnerValue(TEXT("Role"), UE2Codec::Byte(3));
		if (Owner)
		{
			A->Set(TEXT("Owner"), UE2Codec::Actor(Owner->PCActor), FUE2SvProp::OwnerOnly);
			A->Set(TEXT("Controller"), UE2Codec::Actor(Owner->PCActor), FUE2SvProp::OwnerOnly);
		}
		// the KRigidBody state struct of its kind: position, rotation, velocity (x10, unpacked with 0.1 by KUpdateState)
		static const TCHAR* StateNames[] = { TEXT("CarState"), TEXT("HoverState"), TEXT("TreadState"), TEXT("CopState") };
		for (const TCHAR* StateName : StateNames)
		{
			if (A->Class->FindPropertyByName(StateName))
			{
				const FQuat Q = Rot.Quaternion();
				FUE2NetValue Quat;
				Quat.Kind = FUE2NetValue::EKind::Struct;
				Quat.Members.Emplace(TEXT("X"), UE2Codec::Float(float(Q.X)));
				Quat.Members.Emplace(TEXT("Y"), UE2Codec::Float(float(Q.Y)));
				Quat.Members.Emplace(TEXT("Z"), UE2Codec::Float(float(Q.Z)));
				Quat.Members.Emplace(TEXT("W"), UE2Codec::Float(float(Q.W)));
				FUE2NetValue State;
				State.Kind = FUE2NetValue::EKind::Struct;
				State.Members.Emplace(TEXT("ChassisPosition"), UE2Codec::Vec(A->Location));
				State.Members.Emplace(TEXT("ChassisQuaternion"), MoveTemp(Quat));
				State.Members.Emplace(TEXT("ChassisLinVel"), UE2Codec::Vec(UU(V->GetVelocity()) * 10.f));
				// the driver's controls as range bytes (FloatToRangeByte: -1..1 -> 0..255, 128 neutral) and view
				const FRemotePlayer* Drv = FindPlayerByController(V->GetController());
				const float Throttle = Drv ? FMath::Clamp(Drv->DriveForward / 5000.f, -1.f, 1.f) : 0.f;
				const float Steer = Drv ? FMath::Clamp(Drv->DriveStrafe / 5000.f, -1.f, 1.f) : 0.f;
				const float Rise = Drv ? FMath::Clamp(Drv->DriveUp / 5000.f, -1.f, 1.f) : 0.f;
				auto RangeByte = [](float X) { return UE2Codec::Byte(uint8(FMath::Clamp(FMath::RoundToInt((X + 1.f) * 127.5f), 0, 255))); };
				const FRotator ViewRot = V->GetController() ? V->GetController()->GetControlRotation() : Rot;
				State.Members.Emplace(TEXT("ServerViewPitch"), UE2Codec::Int(RotUU(ViewRot.Pitch)));
				State.Members.Emplace(TEXT("ServerViewYaw"), UE2Codec::Int(RotUU(ViewRot.Yaw)));
				if (FCString::Strcmp(StateName, TEXT("CarState")) == 0)
				{
					State.Members.Emplace(TEXT("ServerGas"), RangeByte(Throttle));
					State.Members.Emplace(TEXT("ServerBrake"), RangeByte(FMath::Abs(Throttle) < 0.01f ? 1.f : 0.f));
					State.Members.Emplace(TEXT("ServerHandbrake"), RangeByte(0.f));
					State.Members.Emplace(TEXT("ServerSteering"), RangeByte(Steer));
					State.Members.Emplace(TEXT("ServerGear"), UE2Codec::Byte(Throttle < -0.01f ? 0 : 1));
				}
				else
				{
					State.Members.Emplace(TEXT("ServerThrust"), RangeByte(Throttle));
					State.Members.Emplace(TEXT("ServerStrafe"), RangeByte(Steer));
					State.Members.Emplace(TEXT("ServerTurn"), RangeByte(Steer));
					State.Members.Emplace(TEXT("ServerRise"), RangeByte(Rise));
				}
				A->Set(StateName, State, FUE2SvProp::All, true);
				break;
			}
		}
		AArenaCharacter* Driver = V->GetDriverCharacter();
		if (Driver)
		{
			DriverVehicles.Add(Driver, A->Id);
		}
		A->Set(TEXT("Team"), UE2Codec::Byte(V->GetTeam() == 0 || V->GetTeam() == 1 ? V->GetTeam() : 255));
		A->Set(TEXT("bDriving"), UE2Codec::Bool(Driver != nullptr));
		A->Set(TEXT("Driver"), UE2Codec::Actor(Driver ? PawnActorOf(Driver) : 0));
		A->Set(TEXT("Health"), UE2Codec::Int(FMath::Max(0, FMath::RoundToInt(V->GetHealth()))));
		A->Set(TEXT("bTeamLocked"), UE2Codec::Bool(V->IsLocked()));
		if (V->IsDestroyedVehicle())
		{
			A->Set(TEXT("bVehicleDestroyed"), UE2Codec::Bool(true));
			A->Set(TEXT("bDestroyAppearance"), UE2Codec::Bool(true));
			A->Set(TEXT("ExplosionCount"), UE2Codec::Byte(1));
		}
		// its guns (ONSWeapon actors on their bones), as the vehicle's DriverWeapons spawn them
		TArray<int32>& Guns = VehicleGuns.FindOrAdd(A->Id);
		if (bNew || Guns.Num() == 0)
		{
			for (const TPair<FString, FName>& W : DefaultVehicleWeapons(Class, TEXT("DriverWeapons"), TEXT("WeaponClass")))
			{
				if (FUE2SvActor* G = Server.CreateActor(W.Key, TEXT("VehicleWeapon")))
				{
					G->Set(TEXT("AttachmentBone"), UE2Codec::Name(W.Value), FUE2SvProp::All);
					Guns.Add(G->Id);
				}
			}
		}
		for (const int32 GunId : Guns)
		{
			if (FUE2SvActor* G = Server.FindActor(GunId))
			{
				G->SeenFrame = Server.Frame;
				G->Location = A->Location;
				G->RelevancyParent = A->Id;
				G->OwnerConn = A->OwnerConn;
				G->UpdateInterval = 0.1f;
				G->Set(TEXT("Base"), UE2Codec::Actor(A->Id));
				G->Set(TEXT("Owner"), UE2Codec::Actor(A->Id));
				G->Set(TEXT("Instigator"), UE2Codec::Actor(A->Id));
				G->Set(TEXT("Team"), UE2Codec::Byte(V->GetTeam() == 0 || V->GetTeam() == 1 ? V->GetTeam() : 255));
				// ONSWeapon.CurrentHitLocation (to everyone but the driver, who aims itself): the turret turns to it
				if (Driver)
				{
					const FQuat Aim = V->GetActorQuat() * V->GetTurretAim(false).Quaternion();
					const FVector Hit = V->GetTurretPivot(false) + Aim.GetForwardVector() * 5000.f;
					G->Set(TEXT("CurrentHitLocation"), UE2Codec::Vec(UU(Hit)), FUE2SvProp::NotOwner);
				}
			}
		}
		// a gunner seat (the Caravan's): the vehicle's PassengerWeapons pawn
		if (AArenaONS_VehicleSeat* Seat = V->GetSeatPawn())
		{
			const TArray<TPair<FString, FName>> Pawns = DefaultVehicleWeapons(Class, TEXT("PassengerWeapons"), TEXT("WeaponPawnClass"));
			FUE2SvActor* SA = Pawns.Num() ? ActorFor(Seat, Pawns[0].Key, TEXT("Vehicle")) : nullptr;
			if (SA)
			{
				SA->Location = A->Location;
				SA->RelevancyParent = A->Id;
				SA->UpdateInterval = 0.1f;
				const FRemotePlayer* Gunner = FindPlayerByController(Seat->GetController());
				SA->OwnerConn = Gunner ? Gunner->ConnId : 0;
				SA->Set(TEXT("Role"), UE2Codec::Byte(2));
				SA->SetOwnerValue(TEXT("Role"), UE2Codec::Byte(3));
				if (Gunner)
				{
					SA->Set(TEXT("Owner"), UE2Codec::Actor(Gunner->PCActor), FUE2SvProp::OwnerOnly);
					SA->Set(TEXT("Controller"), UE2Codec::Actor(Gunner->PCActor), FUE2SvProp::OwnerOnly);
				}
				SA->Set(TEXT("VehicleBase"), UE2Codec::Actor(A->Id));
				// ONSWeaponPawn.BeginPlay / AttachToVehicle: the seat's own gun (GunClass), on the vehicle's WeaponBone;
				// its CurrentHitLocation turns the passenger turret on the other clients
				TArray<int32>& SeatGuns = VehicleGuns.FindOrAdd(SA->Id);
				if (SeatGuns.Num() == 0)
				{
					FUSClass* SeatClass = Server.FindClass(Pawns[0].Key);
					FUSObject* SeatDef = SeatClass ? SeatClass->GetDefaultObject() : nullptr;
					FUSProperty* GunProp = SeatDef ? SeatDef->FindProperty(TEXT("GunClass")) : nullptr;
					const FUSObject* GunClass = GunProp && SeatDef->Data && (GunProp->Type == EUSType::Class || GunProp->Type == EUSType::Object)
						? *reinterpret_cast<FUSObject* const*>(SeatDef->Data + GunProp->Offset) : nullptr;
					if (FUE2SvActor* G = GunClass ? Server.CreateActor(GunClass->GetPathName(), TEXT("VehicleWeapon")) : nullptr)
					{
						G->Set(TEXT("AttachmentBone"), UE2Codec::Name(Pawns[0].Value), FUE2SvProp::All);
						SeatGuns.Add(G->Id);
					}
				}
				for (const int32 GunId : SeatGuns)
				{
					if (FUE2SvActor* G = Server.FindActor(GunId))
					{
						G->SeenFrame = Server.Frame;
						G->Location = A->Location;
						G->RelevancyParent = A->Id;
						G->OwnerConn = SA->OwnerConn;
						G->UpdateInterval = 0.1f;
						G->Set(TEXT("Base"), UE2Codec::Actor(A->Id));
						G->Set(TEXT("Owner"), UE2Codec::Actor(SA->Id));
						G->Set(TEXT("Instigator"), UE2Codec::Actor(SA->Id));
						G->Set(TEXT("Team"), UE2Codec::Byte(V->GetTeam() == 0 || V->GetTeam() == 1 ? V->GetTeam() : 255));
						if (Seat->GetRider())
						{
							const FQuat Aim = V->GetActorQuat() * V->GetTurretAim(true).Quaternion();
							G->Set(TEXT("CurrentHitLocation"), UE2Codec::Vec(UU(V->GetTurretPivot(true) + Aim.GetForwardVector() * 5000.f)), FUE2SvProp::NotOwner);
						}
						SA->Set(TEXT("Gun"), UE2Codec::Actor(GunId));
					}
				}
				AArenaCharacter* Rider = Seat->GetRider();
				SA->Set(TEXT("Driver"), UE2Codec::Actor(Rider ? PawnActorOf(Rider) : 0));
				SA->Set(TEXT("bDriving"), UE2Codec::Bool(Rider != nullptr));
				if (Rider)
				{
					DriverVehicles.Add(Rider, SA->Id);
				}
			}
		}
	}
	for (auto It = VehicleGuns.CreateIterator(); It; ++It)
	{
		if (!Server.FindActor(It.Key()) || Server.FindActor(It.Key())->SeenFrame != Server.Frame)
		{
			for (const int32 GunId : It.Value())
			{
				Server.DestroyActor(GunId);
			}
			It.RemoveCurrent();
		}
	}
}

// =================================================================================================
// Sounds (PlayerController.ClientHearSound: what the server's PlaySound / PlayOwnedSound sends)
// =================================================================================================

bool FUE2ServerWorld::GetFireSound(const FString& WeaponClass, int32 Mode, FUE2ObjectRef& OutSound, float& OutVolume, float& OutRadius)
{
	const FString Key = FString::Printf(TEXT("%s:%d"), *WeaponClass, Mode);
	if (const FFireSound* Hit = FireSounds.Find(Key))
	{
		OutSound = Hit->Sound;
		OutVolume = Hit->Volume;
		OutRadius = Hit->Radius;
		return Hit->Sound.IsStatic();
	}
	FFireSound F;
	// the weapon's fire mode class (Weapon.FireModeClass[Mode]) and its FireSound / TransientSound* defaults
	FUSClass* Weapon = Server.FindClass(WeaponClass);
	FUSObject* WeaponDef = Weapon ? Weapon->GetDefaultObject() : nullptr;
	FUSProperty* ModeProp = WeaponDef ? WeaponDef->FindProperty(TEXT("FireModeClass")) : nullptr;
	if (ModeProp && WeaponDef->Data && Mode >= 0 && Mode < ModeProp->ArrayDim)
	{
		FUSObject* FireClassObj = *reinterpret_cast<FUSObject**>(WeaponDef->Data + ModeProp->Offset + Mode * ModeProp->ElementSize);
		FUSClass* FireClass = FireClassObj ? Server.FindClass(FireClassObj->GetPathName()) : nullptr;
		FUSObject* FireDef = FireClass ? FireClass->GetDefaultObject() : nullptr;
		FUSPropRef SoundRef, VolumeRef, RadiusRef;
		if (FireDef && SoundRef.Resolve(FireClass, TEXT("FireSound")))
		{
			if (const FUSObject* Snd = SoundRef.Get<FUSObject*>(FireDef))
			{
				F.Sound = Server.FindStatic(Snd->GetPathName());
			}
			F.Volume = VolumeRef.Resolve(FireClass, TEXT("TransientSoundVolume")) ? VolumeRef.Get<float>(FireDef) : 1.f;
			F.Radius = RadiusRef.Resolve(FireClass, TEXT("TransientSoundRadius")) ? RadiusRef.Get<float>(FireDef) : 300.f;
		}
	}
	FireSounds.Add(Key, F);
	OutSound = F.Sound;
	OutVolume = F.Volume;
	OutRadius = F.Radius;
	return F.Sound.IsStatic();
}

void FUE2ServerWorld::PlayFireSound(const FUE2SvActor& Pawn, const FString& WeaponClass, int32 Mode)
{
	FUE2ObjectRef Sound;
	float Volume = 1.f, Radius = 300.f;
	if (WeaponClass.IsEmpty() || !GetFireSound(WeaponClass, Mode, Sound, Volume, Radius))
	{
		return;
	}
	// PlayOwnedSound: everyone in hearing range but the owner (whose own client plays it)
	HearSound(Sound, Pawn.Location, Volume, Radius, Pawn.OwnerConn, Pawn.Id * 16 + 3);
}

void FUE2ServerWorld::HearSound(const FUE2ObjectRef& Sound, const FVector3f& Location, float Volume, float Radius, int32 ExcludeConn, int32 Id)
{
	if (!Sound.IsStatic())
	{
		return;
	}
	const FVector3f Params(FMath::Clamp(Volume, 0.f, 4.f) * 1000.f, FMath::Max(Radius, 1.f), 1000.f);
	for (const TPair<int32, FRemotePlayer>& P : Players)
	{
		if (P.Key == ExcludeConn)
		{
			continue;
		}
		FVector3f View;
		if (GetViewLocation(P.Key, View) && FVector3f::DistSquared(View, Location) > FMath::Square(FMath::Max(4000.f, Radius * 10.f)))
		{
			continue;
		}
		CallPC(P.Key, TEXT("ClientHearSound"), { UE2Codec::None(), UE2Codec::Int(Id), UE2Codec::Static(Sound), UE2Codec::Vec(Location),
			UE2Codec::Vec(Params), UE2Codec::Bool(false) }, true);
		++SoundsSent;
	}
}

FUE2ObjectRef FUE2ServerWorld::DefaultObject(const FString& ClassPath, const TCHAR* Property, int32 Element)
{
	FUSClass* Class = Server.FindClass(ClassPath);
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	FUSProperty* Prop = Def ? Def->FindProperty(FName(Property)) : nullptr;
	if (!Prop || !Def->Data || Element < 0 || Element >= Prop->ArrayDim || (Prop->Type != EUSType::Object && Prop->Type != EUSType::Class))
	{
		return FUE2ObjectRef();
	}
	const FUSObject* O = *reinterpret_cast<FUSObject* const*>(Def->Data + Prop->Offset + Element * Prop->ElementSize);
	return O ? Server.FindStatic(O->GetPathName()) : FUE2ObjectRef();
}

TArray<FUE2ObjectRef> FUE2ServerWorld::DefaultObjectArray(const FString& ClassPath, const TCHAR* Property)
{
	TArray<FUE2ObjectRef> Out;
	FUSClass* Class = Server.FindClass(ClassPath);
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	FUSProperty* Prop = Def ? Def->FindProperty(FName(Property)) : nullptr;
	if (!Prop || !Def->Data || Prop->Type != EUSType::Array || !Prop->Inner)
	{
		return Out;
	}
	const FUSScriptArray* Arr = reinterpret_cast<const FUSScriptArray*>(Def->Data + Prop->Offset);
	for (int32 i = 0; Arr->Data && i < Arr->Num && i < 64; ++i)
	{
		const FUSObject* O = *reinterpret_cast<FUSObject* const*>(Arr->Data + i * Prop->Inner->ElementSize);
		Out.Add(O ? Server.FindStatic(O->GetPathName()) : FUE2ObjectRef());
	}
	return Out;
}

FName FUE2ServerWorld::DefaultName(const FString& ClassPath, const TCHAR* Property, int32 Element)
{
	FUSClass* Class = Server.FindClass(ClassPath);
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	FUSProperty* Prop = Def ? Def->FindProperty(FName(Property)) : nullptr;
	if (!Prop || !Def->Data || Prop->Type != EUSType::Name || Element < 0 || Element >= Prop->ArrayDim)
	{
		return NAME_None;
	}
	return *reinterpret_cast<const FName*>(Def->Data + Prop->Offset + Element * Prop->ElementSize);
}

FString FUE2ServerWorld::SoundGroupOf(const AArenaCharacter* Ch)
{
	// SpeciesType.Setup: the species' MaleSoundGroup / FemaleSoundGroup by the character record's Sex (the .upl's
	// Species=, e.g. xGame.SPECIES_Merc -> XGame.xMercFemaleSoundGroup; robots xBotSoundGroup, Skaarj, Juggernauts ...)
	const AArenaPlayerState* PS = Ch ? Ch->GetPlayerState<AArenaPlayerState>() : nullptr;
	const FString Character = PS && !PS->CharacterName.IsEmpty() ? PS->CharacterName : FString(TEXT("Jakob"));
	if (const FString* Known = SoundGroups.Find(Character))
	{
		return *Known;
	}
	FString Group = TEXT("XGame.xMercMaleSoundGroup"); // xPawn.SoundGroupClass default
	if (const ArenaSP::FPlayerRecord* Rec = ArenaSP::FindPlayer(Character))
	{
		const bool bFemale = Rec->Sex.Equals(TEXT("Female"), ESearchCase::IgnoreCase);
		const FString Species = Rec->Species.IsEmpty() ? FString(TEXT("XGame.SPECIES_Human")) : Rec->Species;
		FUSClass* Class = Server.FindClass(Species);
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FUSProperty* Prop = Def ? Def->FindProperty(FName(bFemale ? TEXT("FemaleSoundGroup") : TEXT("MaleSoundGroup"))) : nullptr;
		if (Prop && Def->Data && Prop->Type == EUSType::Str)
		{
			const FString Value = *reinterpret_cast<const FString*>(Def->Data + Prop->Offset);
			if (!Value.IsEmpty() && Server.FindClass(Value))
			{
				Group = Value;
			}
		}
		else if (bFemale)
		{
			Group = TEXT("XGame.xMercFemaleSoundGroup");
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s (%s, %s) sounds %s"), *Character, *Species, *Rec->Sex, *Group);
	}
	SoundGroups.Add(Character, Group);
	return Group;
}

void FUE2ServerWorld::PlayPawnSound(const FUE2SvActor& Pawn, const AArenaCharacter* Ch, const TCHAR* Array, int32 Index, float Volume, float Radius, int32 ExcludeConn)
{
	// xPawn.SoundGroupClass (an xPawnSoundGroup subclass): Sounds[ESoundType], DeathSounds[], PainSounds[]
	static TMap<FString, TArray<FUE2ObjectRef>> Cache;
	const FString Group = SoundGroupOf(Ch);
	const FString Key = Group + TEXT(".") + Array;
	TArray<FUE2ObjectRef>* Sounds = Cache.Find(Key);
	if (!Sounds)
	{
		Sounds = &Cache.Add(Key, DefaultObjectArray(Group, Array));
		if (Sounds->Num() == 0)
		{
			*Sounds = DefaultObjectArray(TEXT("XGame.xPawnSoundGroup"), Array);
		}
	}
	if (Sounds->Num() == 0)
	{
		return;
	}
	const int32 I = Index >= 0 ? Index : FMath::RandRange(0, Sounds->Num() - 1);
	if (Sounds->IsValidIndex(I))
	{
		HearSound((*Sounds)[I], Pawn.Location, Volume, Radius, ExcludeConn, Pawn.Id * 16 + 5);
	}
}

void FUE2ServerWorld::PawnEventSounds(FUE2SvActor& A, AArenaCharacter* Ch)
{
	FPawnState& St = PawnStates.FindOrAdd(A.Id);
	const uint8 Physics = PhysicsOf(Ch);
	const double Now = FPlatformTime::Seconds();
	if (St.Health >= 0.f && Ch->Health < St.Health - 1.f && !Ch->IsDead() && Now - St.LastPain > 0.5)
	{
		// xPawn.PlayHit: a pain sound (the stock server plays it for everyone)
		St.LastPain = Now;
		PlayPawnSound(A, Ch, TEXT("PainSounds"), -1, 1.f, 200.f, 0);
	}
	if (St.Physics == 2 && Physics == 1 && St.FallSpeed < -400.f)
	{
		// xPawn.Landed: EST_Land (0), to everyone but the owner
		PlayPawnSound(A, Ch, TEXT("Sounds"), 0, FMath::Clamp(-St.FallSpeed / 1000.f, 0.2f, 1.f), 300.f, A.OwnerConn);
	}
	else if (St.Physics == 1 && Physics == 2 && Ch->GetVelocity().Z / S > 200.f)
	{
		// xPawn.DoJump: EST_Jump (3)
		PlayPawnSound(A, Ch, TEXT("Sounds"), 3, 0.5f, 300.f, A.OwnerConn);
	}
	St.Health = Ch->Health;
	St.Physics = Physics;
	St.FallSpeed = float(Ch->GetVelocity().Z / S);
}

void FUE2ServerWorld::RefreshWeaponEffects(FUE2SvActor& Pawn, int32 AttachmentId, AArenaWeapon* Weapon, int32 Team)
{
	const double WorldNow = World.IsValid() ? World->GetTimeSeconds() : 0.0;
	const FString Native = Weapon ? Weapon->GetClass()->GetName() : FString();
	// link gun beam: an actor of its own while the beam fires (LinkFire spawns it, owned by the pawn)
	const bool bBeam = Weapon && Native.Contains(TEXT("ArcLinker")) && WorldNow - Weapon->NetBeamTime < 0.25;
	const AWeap_ArcLinker* Linker = Cast<AWeap_ArcLinker>(Weapon);
	const int32 Links = Linker ? Linker->GetLinks() : 0;
	const AArenaCharacter* LinkedTo = Linker && bBeam ? Linker->GetLinkedPawn() : nullptr;
	int32* BeamId = BeamActors.Find(Pawn.Id);
	FUE2SvActor* Beam = BeamId ? Server.FindActor(*BeamId) : nullptr;
	if (bBeam)
	{
		if (!Beam)
		{
			Beam = Server.CreateActor(TEXT("XWeapons.LinkBeamEffect"), TEXT("Effect"));
			if (Beam)
			{
				BeamActors.Add(Pawn.Id, Beam->Id);
			}
		}
		if (Beam)
		{
			Beam->SeenFrame = Server.Frame;
			Beam->Location = UU(Weapon->NetShotStart);
			Beam->OwnerConn = Pawn.OwnerConn;
			Beam->RelevancyParent = Pawn.Id;
			Beam->UpdateInterval = 0.05f;
			Beam->NetPriority = 3.f;
			Beam->Set(TEXT("Instigator"), UE2Codec::Actor(Pawn.Id));
			Beam->Set(TEXT("Owner"), UE2Codec::Actor(Pawn.Id));
			Beam->Set(TEXT("StartEffect"), UE2Codec::Vec(UU(Weapon->NetShotStart)), FUE2SvProp::NotOwner, true);
			Beam->Set(TEXT("EndEffect"), UE2Codec::Vec(UU(Weapon->NetShotEnd)), FUE2SvProp::NotOwner, true);
			Beam->Set(TEXT("bHitSomething"), UE2Codec::Bool(!Weapon->NetShotNormal.IsZero()));
			// LinkFire.ModeTick: Links (beam size and children), LinkColor 0 / team + 1 while linked to a team mate, LinkedPawn
			Beam->Set(TEXT("Links"), UE2Codec::Byte(uint8(FMath::Clamp(Links, 0, 255))));
			Beam->Set(TEXT("LinkColor"), UE2Codec::Byte(LinkedTo ? uint8(FMath::Clamp(Team, 0, 1) + 1) : 0));
			Beam->Set(TEXT("LinkedPawn"), LinkedTo ? UE2Codec::Actor(PawnActorOf(LinkedTo)) : UE2Codec::None());
			Beam->Set(TEXT("bLockedOn"), UE2Codec::Bool(LinkedTo != nullptr));
		}
	}
	else if (Beam)
	{
		Server.DestroyActor(Beam->Id);
		BeamActors.Remove(Pawn.Id);
	}
	if (Linker && AttachmentId)
	{
		if (FUE2SvActor* Att = Server.FindActor(AttachmentId))
		{
			// LinkGun: LinkAttachment.Links; SetLinkColor LC_Red / LC_Blue while linking a team mate, LC_Gold when linked, LC_Green
			const uint8 Color = bBeam && LinkedTo ? uint8(Team == 1 ? 2 : 1) : (Links > 0 ? 3 : 0);
			Att->Set(TEXT("Links"), UE2Codec::Int(Links));
			Att->Set(TEXT("LinkColor"), UE2Codec::Byte(Color), FUE2SvProp::NotOwner);
		}
	}
	// shield gun: the attachment's ShieldEffect3rd (team colour), shown while the shield is up (alt fire)
	const bool bShieldGun = Weapon && Native.Contains(TEXT("Gauntlet")) && AttachmentId;
	int32* ShieldId = ShieldActors.Find(Pawn.Id);
	FUE2SvActor* Shield = ShieldId ? Server.FindActor(*ShieldId) : nullptr;
	if (bShieldGun)
	{
		if (!Shield)
		{
			Shield = Server.CreateActor(Team == 0 ? TEXT("XWeapons.ShieldEffect3rdRED") : (Team == 1 ? TEXT("XWeapons.ShieldEffect3rdBLUE") : TEXT("XWeapons.ShieldEffect3rd")),
				TEXT("Effect"));
			if (Shield)
			{
				ShieldActors.Add(Pawn.Id, Shield->Id);
			}
		}
		if (Shield)
		{
			Shield->SeenFrame = Server.Frame;
			Shield->Location = Pawn.Location;
			Shield->RelevancyParent = Pawn.Id;
			Shield->UpdateInterval = 0.05f;
			Shield->Set(TEXT("Owner"), UE2Codec::Actor(Pawn.Id));
			Shield->Set(TEXT("Instigator"), UE2Codec::Actor(Pawn.Id));
			Shield->Set(TEXT("Base"), UE2Codec::Actor(Pawn.Id));
			Shield->Set(TEXT("bHidden"), UE2Codec::Bool(!Weapon->IsFiring(1)));
			if (FUE2SvActor* Att = Server.FindActor(AttachmentId))
			{
				Att->Set(TEXT("ShieldEffect3rd"), UE2Codec::Actor(Shield->Id));
			}
		}
	}
	else if (Shield)
	{
		Server.DestroyActor(Shield->Id);
		ShieldActors.Remove(Pawn.Id);
	}
}

void FUE2ServerWorld::RefreshAnnouncements(AArenaGameState* GS)
{
	const uint8 Phase = uint8(GS->Phase);
	// the countdown before the match: StartupMessage stages 2..4 (3, 2, 1), 5 when it begins (UnrealPlayer.PlayStartupMessage)
	if (GS->Phase == EArenaMatchPhase::Countdown && GS->CountdownRemaining != LastCountdown && GS->CountdownRemaining >= 1 && GS->CountdownRemaining <= 3)
	{
		for (const TPair<int32, FRemotePlayer>& P : Players)
		{
			CallPC(P.Key, TEXT("PlayStartupMessage"), { UE2Codec::Byte(5 - GS->CountdownRemaining) });
		}
	}
	LastCountdown = GS->CountdownRemaining;
	if (Phase != LastPhase && LastPhase != 255)
	{
		if (GS->Phase == EArenaMatchPhase::Playing && LastPhase == uint8(EArenaMatchPhase::Countdown))
		{
			for (const TPair<int32, FRemotePlayer>& P : Players)
			{
				CallPC(P.Key, TEXT("PlayStartupMessage"), { UE2Codec::Byte(5) });
			}
		}
		else if (GS->Phase == EArenaMatchPhase::Ended)
		{
			// DeathMatch / TeamGame.PlayEndOfMatchMessage: the game class's EndGameSoundName[0] to the winners, [1] to the rest
			const FName Won = DefaultName(Server.GetGameClass(), TEXT("EndGameSoundName"), 0);
			const FName Lost = DefaultName(Server.GetGameClass(), TEXT("EndGameSoundName"), 1);
			// DeathMatch.CheckEndGame: every view goes behind the winner's pawn (EndGameFocus)
			const APlayerState* WinPS = GS->WinningPlayer;
			const APawn* WinPawn = WinPS ? WinPS->GetPawn() : nullptr;
			const int32 FocusActor = WinPawn ? PawnActorOf(WinPawn) : 0;
			EndGameFocusActor = FocusActor;
			for (const TPair<int32, FRemotePlayer>& P : Players)
			{
				if (FocusActor)
				{
					CallPC(P.Key, TEXT("ClientSetViewTarget"), { UE2Codec::Actor(FocusActor) });
					CallPC(P.Key, TEXT("ClientSetBehindView"), { UE2Codec::Bool(true) });
				}
				const AUE2RemoteController* C = P.Value.Controller.Get();
				const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
				const bool bWinner = PS && (GS->IsTeamGame() ? PS->Team == GS->WinningTeam : GS->WinningPlayer == PS);
				const FName Sound = (bWinner || P.Value.bSpectator) ? Won : Lost;
				if (!Sound.IsNone())
				{
					CallPC(P.Key, TEXT("PlayStatusAnnouncement"), { UE2Codec::Name(Sound), UE2Codec::Byte(1), UE2Codec::Bool(true) });
				}
			}
		}
	}
	LastPhase = Phase;
}

// =================================================================================================
// Players
// =================================================================================================

FUE2ServerWorld::FRemotePlayer* FUE2ServerWorld::FindPlayerByController(const AController* C)
{
	for (TPair<int32, FRemotePlayer>& P : Players)
	{
		if (P.Value.Controller.Get() == C)
		{
			return &P.Value;
		}
	}
	return nullptr;
}

void FUE2ServerWorld::PlayerJoined(FUE2ServerConnection& Conn)
{
	FRemotePlayer& P = Players.Add(Conn.GetId());
	P.ConnId = Conn.GetId();
	P.bSpectator = Conn.bSpectatorOnly;
	SpawnRemotePlayer(P, Conn);
}

void FUE2ServerWorld::SpawnRemotePlayer(FRemotePlayer& P, FUE2ServerConnection& Conn)
{
	UWorld* W = World.Get();
	AArenaGameMode* GM = W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
	AArenaGameState* GS = GM ? GM->GetArenaGameState() : nullptr;
	if (!GM || !GS)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AUE2RemoteController* C = W->SpawnActor<AUE2RemoteController>(AUE2RemoteController::StaticClass(), FTransform::Identity, Params);
	if (!C)
	{
		Conn.Close(TEXT("cannot create the player"));
		return;
	}
	C->ConnId = P.ConnId;
	C->bRemoteSpectator = P.bSpectator;
	C->InitPlayerState();
	P.Controller = C;
	AArenaPlayerState* PS = C->GetPlayerState<AArenaPlayerState>();
	const FString Url = Conn.LoginOptions;
	auto Option = [&Url](const TCHAR* Key) { return UGameplayStatics::ParseOption(Url, Key); };
	if (PS)
	{
		if (GM->GameSession)
		{
			PS->SetPlayerId(GM->GameSession->GetNextPlayerID());
		}
		// unique names, like GameInfo.ChangeName
		FString Name = Conn.PlayerName;
		for (int32 Try = 2; Try < 100; ++Try)
		{
			bool bTaken = false;
			for (APlayerState* Other : GS->PlayerArray)
			{
				bTaken |= Other && Other != PS && Other->GetPlayerName().Equals(Name, ESearchCase::IgnoreCase);
			}
			if (!bTaken)
			{
				break;
			}
			Name = FString::Printf(TEXT("%s%d"), *Conn.PlayerName, Try);
		}
		PS->SetPlayerName(Name);
		// the connection goes by the name the game gave the player (the admin list, logs)
		Conn.PlayerName = Name;
		PS->CharacterName = Option(TEXT("Character"));
		if (P.bSpectator)
		{
			PS->SetIsSpectator(true);
			PS->SetIsOnlyASpectator(true);
		}
	}
	if (GS->IsTeamGame() && !P.bSpectator && PS)
	{
		int32 Team = FCString::Atoi(*Option(TEXT("Team")));
		if (Option(TEXT("Team")).IsEmpty() || (Team != 0 && Team != 1))
		{
			int32 Count[2] = { 0, 0 };
			for (APlayerState* Other : GS->PlayerArray)
			{
				if (const AArenaPlayerState* O = Cast<AArenaPlayerState>(Other))
				{
					if (O != PS && (O->Team == 0 || O->Team == 1))
					{
						++Count[O->Team];
					}
				}
			}
			Team = Count[1] < Count[0] ? 1 : 0;
		}
		GM->SetPlayerTeam(C, Team);
	}
	// the game info and the players first: the controller, its weapons and HUD refer to them (Weapon.PutDown reads the GRI)
	RefreshGame(GM, GS);
	RefreshPlayerStates(GS);
	for (int32 Id : { LevelInfoActor, GRIActor, TeamActors[0], TeamActors[1] })
	{
		if (FUE2SvActor* A = Server.FindActor(Id))
		{
			Conn.ReplicateActor(*A, Server.GetTime(), false);
		}
	}
	for (APlayerState* Other : GS->PlayerArray)
	{
		if (FUE2SvActor* A = Server.FindActor(PRIActorOf(Other)))
		{
			Conn.ReplicateActor(*A, Server.GetTime(), false);
		}
	}
	// the PlayerController this client controls (owner only)
	if (FUE2SvActor* PC = Server.CreateActor(TEXT("XGame.xPlayer"), TEXT("PC")))
	{
		P.PCActor = PC->Id;
		PC->OwnerConn = P.ConnId;
		PC->bOnlyRelevantToOwner = true;
		PC->Native = C;
		NativeActors.Add(C, PC->Id);
	}
	P.State = P.bSpectator ? FName(TEXT("Spectating")) : FName(TEXT("PlayerWaiting"));
	// a first view point: a player start
	if (AActor* Start = GM->ChoosePlayerStart(C))
	{
		P.SpectatorLocation = Start->GetActorLocation();
	}
	// what the stock server tells a new player first
	RefreshPlayerStates(GS);
	RefreshControllers();
	FString HUD, Scores;
	HUDClasses(ModeId, HUD, Scores);
	CallPC(P.ConnId, TEXT("ClientSetHUD"), { UE2Codec::Static(Server.FindStatic(HUD)), UE2Codec::Static(Server.FindStatic(Scores)) });
	if (!P.bSpectator && GS->Phase != EArenaMatchPhase::Ended)
	{
		GM->RestartPlayer(C);
	}
	if (UArenaGameRules* Rules = GM->GetRules())
	{
		Rules->OnPlayerJoined(C);
	}
	// "<name> entered the game." (GameMessage switch 1) to everyone
	BroadcastLocalized(TEXT("Engine.GameMessage"), 1, PS, nullptr, nullptr);
	for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
	{
		if (AArenaPlayerController* LocalPC = Cast<AArenaPlayerController>(It->Get()))
		{
			LocalPC->AddConsoleLine(FString::Printf(TEXT("%s entered the game."), PS ? *PS->GetPlayerName() : TEXT("A player")), FColor::White);
		}
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s is in the match (%s%s)"), PS ? *PS->GetPlayerName() : TEXT("?"), P.bSpectator ? TEXT("spectator") : TEXT("player"),
		PS && GS->IsTeamGame() ? *FString::Printf(TEXT(", team %d"), PS->Team) : TEXT(""));
}

void FUE2ServerWorld::PlayerLeft(FUE2ServerConnection& Conn)
{
	FRemotePlayer P;
	if (!Players.RemoveAndCopyValue(Conn.GetId(), P))
	{
		return;
	}
	AUE2RemoteController* C = P.Controller.Get();
	const APlayerState* PS = C ? C->PlayerState : nullptr;
	BroadcastLocalized(TEXT("Engine.GameMessage"), 4, PS, nullptr, nullptr);
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s left (moves %d, steps %d, adjustments %d, corrections %d, client positions taken %d, average error %.1f UU, max %.1f UU, fire %d, restarts %d)"),
		*Conn.PlayerName, P.Moves, P.Steps, P.Adjustments, P.Corrections, P.Trusted, P.Moves ? P.ErrorSum / P.Moves : 0.0, P.MaxError, P.FireCommands, P.Restarts);
	if (C)
	{
		if (AArenaGameMode* GM = C->GetWorld() ? C->GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr)
		{
			if (UArenaGameRules* Rules = GM->GetRules())
			{
				Rules->OnPlayerLeft(C);
			}
			GM->RemovePawn(C);
			GM->Logout(C);
		}
		NativeActors.Remove(C);
		C->Destroy();
	}
	Server.DestroyActor(P.PCActor);
}

void FUE2ServerWorld::UpdatePossession(FRemotePlayer& P)
{
	AUE2RemoteController* C = P.Controller.Get();
	if (!C)
	{
		return;
	}
	AArenaCharacter* Ch = Cast<AArenaCharacter>(C->GetPawn());
	const bool bAlive = Ch && !Ch->IsDead();
	APawn* Vehicle = (!Ch && C->GetPawn() && (Cast<AArenaONS_Vehicle>(C->GetPawn()) || Cast<AArenaONS_VehicleSeat>(C->GetPawn()))) ? C->GetPawn() : nullptr;
	const AArenaGameState* GS = C->GetWorld() ? C->GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	// driving: the client's controller drives the vehicle pawn (Vehicle.KDriverEnter / KDriverLeave)
	if (Vehicle != P.DrivenPawn.Get())
	{
		const int32 PCActor = P.PCActor;
		if (APawn* Old = P.DrivenPawn.Get())
		{
			Server.CallClient(P.ConnId, PawnActorOf(Old), TEXT("ClientKDriverLeave"), { UE2Codec::Actor(PCActor) });
		}
		P.DrivenPawn = Vehicle;
		if (Vehicle)
		{
			const int32 VehicleActor = PawnActorOf(Vehicle);
			if (FUE2SvActor* VA = Server.FindActor(VehicleActor))
			{
				if (FUE2ServerConnection* Conn = Server.FindConnection(P.ConnId))
				{
					Conn->ReplicateActor(*VA, Server.GetTime(), true);
				}
			}
			P.AcknowledgedPawn = 0;
			P.bVehicleFire[0] = P.bVehicleFire[1] = false;
			CallPC(P.ConnId, TEXT("ClientRestart"), { UE2Codec::Actor(VehicleActor) });
			Server.CallClient(P.ConnId, VehicleActor, TEXT("ClientKDriverEnter"), { UE2Codec::Actor(PCActor) });
			P.State = TEXT("PlayerDriving");
			P.LastSentState = P.State;
		}
		P.RestartedPawn.Reset();
	}
	if (Vehicle)
	{
		P.State = TEXT("PlayerDriving");
		return;
	}
	if (GS && GS->Phase == EArenaMatchPhase::Ended)
	{
		P.State = TEXT("GameEnded");
	}
	else if (bAlive)
	{
		P.State = (Ch->GetCharacterMovement() && Ch->GetCharacterMovement()->IsSwimming()) ? FName(TEXT("PlayerSwimming")) : FName(TEXT("PlayerWalking"));
	}
	else if (P.bSpectator)
	{
		P.State = TEXT("Spectating");
	}
	else if (P.RestartedPawn.IsValid() || P.State == TEXT("PlayerWalking") || P.State == TEXT("PlayerSwimming"))
	{
		P.State = TEXT("Dead");
	}
	if (bAlive && P.RestartedPawn.Get() != Ch)
	{
		// a new pawn: the client possesses it (ClientRestart), after the pawn's channel is open there
		P.RestartedPawn = Ch;
		P.AcknowledgedPawn = 0;
		P.CurrentTimeStamp = 0.f;
		P.WeaponsSet.Reset();
		++P.Restarts;
		const int32 PawnActor = PawnActorOf(Ch);
		FUE2ServerConnection* Conn = Server.FindConnection(P.ConnId);
		if (FUE2SvActor* PA = Server.FindActor(PawnActor))
		{
			if (Conn)
			{
				Conn->ReplicateActor(*PA, Server.GetTime(), true);
			}
		}
		CallPC(P.ConnId, TEXT("ClientRestart"), { UE2Codec::Actor(PawnActor) });
		// GameInfo.RestartPlayer: the view faces where the pawn spawned
		CallPC(P.ConnId, TEXT("ClientSetRotation"), { UE2Codec::Rot(FIntVector(0, RotUU(Ch->GetActorRotation().Yaw), 0)) });
		P.LastSentState = P.State;
	}
	else if (!bAlive && P.RestartedPawn.IsValid())
	{
		P.RestartedPawn.Reset();
		if (Ch)
		{
			P.SpectatorLocation = Ch->GetActorLocation();
		}
	}
	// PlayerController.GameHasEnded at the end of the match: GameEnded with or without a pawn
	if (P.State != P.LastSentState && (!bAlive || P.State == TEXT("GameEnded")))
	{
		// the client's controller state (UE2 also tells it through ClientAdjustPosition's newState)
		CallPC(P.ConnId, TEXT("ClientGotoState"), { UE2Codec::Name(P.State), UE2Codec::Name(NAME_None) });
		if (P.State == TEXT("GameEnded"))
		{
			CallPC(P.ConnId, TEXT("ClientGameEnded"), {});
		}
		P.LastSentState = P.State;
	}
	// weapons given: ClientWeaponSet once each (the client brings the first / a better one up)
	if (bAlive)
	{
		for (AArenaWeapon* Wp : Ch->GetWeapons())
		{
			const int32* WId = Wp ? NativeActors.Find(Wp) : nullptr;
			if (WId && !P.WeaponsSet.Contains(*WId))
			{
				P.WeaponsSet.Add(*WId);
				Server.CallClient(P.ConnId, *WId, TEXT("ClientWeaponSet"), { UE2Codec::Bool(Wp == Ch->GetCurrentWeapon()) });
			}
		}
	}
}

void FUE2ServerWorld::CallPC(int32 ConnId, FName Function, const TArray<FUE2NetValue>& Params, bool bForceUnreliable)
{
	const FRemotePlayer* P = Players.Find(ConnId);
	FUE2ServerConnection* Conn = Server.FindConnection(ConnId);
	if (!P || !Conn)
	{
		return;
	}
	// actors the call names go out first (a reference to an actor without a channel there arrives as None)
	for (const FUE2NetValue& V : Params)
	{
		if (V.Kind == FUE2NetValue::EKind::Object && V.Obj.Channel != 0 && !V.Obj.IsStatic() && !Conn->ChannelOfActor(V.Obj.Channel))
		{
			if (FUE2SvActor* A = Server.FindActor(V.Obj.Channel))
			{
				if (!A->bOnlyRelevantToOwner || A->OwnerConn == ConnId)
				{
					Conn->ReplicateActor(*A, Server.GetTime(), A->OwnerConn == ConnId);
				}
			}
		}
	}
	Server.CallClient(ConnId, P->PCActor, Function, Params, bForceUnreliable);
}

void FUE2ServerWorld::BroadcastLocalized(const FString& MessageClass, int32 Switch, const APlayerState* PRI1, const APlayerState* PRI2, const FUE2NetValue* Optional)
{
	const FUE2ObjectRef Class = Server.FindStatic(MessageClass);
	if (!Class.IsStatic())
	{
		return;
	}
	TArray<FUE2NetValue> Params = { UE2Codec::Static(Class), UE2Codec::Int(Switch), UE2Codec::Actor(PRIActorOf(PRI1)), UE2Codec::Actor(PRIActorOf(PRI2)),
		Optional ? *Optional : UE2Codec::None() };
	if (Switch == 0)
	{
		Params[1] = FUE2NetValue();
	}
	for (const TPair<int32, FRemotePlayer>& P : Players)
	{
		CallPC(P.Key, TEXT("ReceiveLocalizedMessage"), Params);
	}
}

// =================================================================================================
// Client calls
// =================================================================================================

void FUE2ServerWorld::ClientFunction(FUE2ServerConnection& Conn, int32 ActorId, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	FRemotePlayer* P = Players.Find(Conn.GetId());
	if (!P || !Function)
	{
		return;
	}
	AUE2RemoteController* C = P->Controller.Get();
	AArenaCharacter* Ch = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr;
	AArenaGameMode* GM = C && C->GetWorld() ? C->GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr;
	const FString Name = Function->Name.ToString();
	auto Arg = [&Params](int32 i) -> const FUE2NetValue& { static const FUE2NetValue Empty; return Params.IsValidIndex(i) ? Params[i].Value : Empty; };
	auto ArgF = [&Arg](int32 i) { const FUE2NetValue& V = Arg(i); return V.Kind == FUE2NetValue::EKind::Float ? V.F : float(V.I); };
	auto ArgV = [&Arg](int32 i) { return FVector(Arg(i).V); };
	if (Server.bTrace && !Name.Contains(TEXT("ServerMove")))
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] client call %s"), Conn.GetId(), *Name);
	}

	if (Name == TEXT("ServerMove"))
	{
		ServerMove(*P, ArgF(0), ArgV(1), ArgV(2), Arg(3).I != 0, Arg(4).I != 0, Arg(5).I != 0, Arg(6).I != 0, uint8(Arg(7).I), Arg(9).I,
			uint8(Arg(10).I), Arg(11).I);
	}
	else if (Name == TEXT("ShortServerMove"))
	{
		ServerMove(*P, ArgF(0), FVector::ZeroVector, ArgV(1), Arg(2).I != 0, Arg(3).I != 0, Arg(4).I != 0, false, 0, Arg(6).I, 0, 0);
	}
	else if (Name == TEXT("DualServerMove"))
	{
		const int32 Compress = Arg(2).I;
		ServerMove(*P, ArgF(0), ArgV(1), FVector::ZeroVector, (Compress & 1) != 0, (Compress & 2) != 0, (Compress & 4) != 0, (Compress & 8) != 0, uint8(Arg(3).I),
			Arg(4).I, 0, 0);
		FVector ClientLoc = ArgV(7);
		if (ClientLoc.IsZero())
		{
			ClientLoc = FVector(0.1f, 0.f, 0.f);
		}
		ServerMove(*P, ArgF(5), ArgV(6), ClientLoc, (Compress & 16) != 0, (Compress & 32) != 0, (Compress & 64) != 0, (Compress & 128) != 0, uint8(Arg(8).I),
			Arg(10).I, uint8(Arg(11).I), Arg(12).I);
	}
	else if (Name == TEXT("ServerAcknowledgePossession"))
	{
		P->AcknowledgedPawn = Arg(0).Obj.Channel;
	}
	else if (Name == TEXT("AskForPawn"))
	{
		const int32 PawnActor = Ch && !Ch->IsDead() ? PawnActorOf(Ch) : 0;
		if (PawnActor)
		{
			CallPC(P->ConnId, TEXT("GivePawn"), { UE2Codec::Actor(PawnActor) });
		}
		else
		{
			CallPC(P->ConnId, TEXT("ClientGotoState"), { UE2Codec::Name(P->State), UE2Codec::Name(NAME_None) });
		}
	}
	else if (Name == TEXT("ServerReStartPlayer") || Name == TEXT("ServerRestartPlayer"))
	{
		if (GM && C && !P->bSpectator && (!Ch || Ch->IsDead()))
		{
			GM->RequestRespawn(C);
		}
	}
	else if (Name == TEXT("ServerStartFire") || Name == TEXT("ServerStopFire"))
	{
		const FUE2SvActor* WA = Server.FindActor(ActorId);
		AArenaWeapon* Wp = WA ? Cast<AArenaWeapon>(WA->Native.Get()) : nullptr;
		const uint8 Mode = uint8(FMath::Clamp(Arg(0).I, 0, 1));
		if (Ch && !Ch->IsDead() && Wp && Wp->GetArenaOwner() == Ch)
		{
			if (Name == TEXT("ServerStartFire"))
			{
				if (Ch->GetCurrentWeapon() != Wp)
				{
					Ch->SwitchToWeapon(Wp);
				}
				Ch->StartFire(Mode);
				++P->FireCommands;
			}
			else
			{
				Ch->StopFire(Mode);
			}
		}
	}
	else if (Name == TEXT("ServerChangedWeapon"))
	{
		const FUE2SvActor* WA = Server.FindActor(Arg(1).Obj.Channel);
		AArenaWeapon* Wp = WA ? Cast<AArenaWeapon>(WA->Native.Get()) : nullptr;
		if (Ch && Wp && Wp->GetArenaOwner() == Ch && Ch->GetCurrentWeapon() != Wp)
		{
			Ch->SwitchToWeapon(Wp);
		}
	}
	else if (Name == TEXT("ServerUse"))
	{
		if (Ch && !Ch->IsDead())
		{
			Ch->Use();
		}
		else if (P->DrivenPawn.IsValid())
		{
			P->bVehicleUse = true; // leave the vehicle
		}
	}
	else if (Name == TEXT("Suicide"))
	{
		if (Ch && !Ch->IsDead())
		{
			Ch->Suicide();
		}
	}
	else if (Name == TEXT("ServerChangeTeam"))
	{
		const int32 Team = Arg(0).I;
		AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (GM && PS && GM->IsTeamGame() && (Team == 0 || Team == 1) && PS->Team != Team)
		{
			GM->SetPlayerTeam(C, Team);
			if (Ch && !Ch->IsDead())
			{
				Ch->Suicide();
			}
		}
	}
	else if (Name == TEXT("ServerSay") || Name == TEXT("ServerTeamSay"))
	{
		OnChat(C ? C->PlayerState : nullptr, Arg(0).S.Left(128), Name == TEXT("ServerTeamSay"));
	}
	else if (Name == TEXT("ServerDoCombo"))
	{
		const FString Combo = Server.GetMasterMap().GetObjectPath(Arg(0).Obj);
		EArenaCombo Which = EArenaCombo::None;
		if (Combo.Contains(TEXT("Speed"))) Which = EArenaCombo::Speed;
		else if (Combo.Contains(TEXT("Berserk"))) Which = EArenaCombo::Berserk;
		else if (Combo.Contains(TEXT("Defensive")) || Combo.Contains(TEXT("Booster"))) Which = EArenaCombo::Booster;
		else if (Combo.Contains(TEXT("Invis"))) Which = EArenaCombo::Invisible;
		if (Ch && !Ch->IsDead() && Which != EArenaCombo::None)
		{
			Ch->ActivateCombo(Which);
		}
	}
	else if (Name == TEXT("ServerViewNextPlayer") || Name == TEXT("ServerViewSelf") || Name == TEXT("ServerVerifyViewTarget"))
	{
		HandleViewChange(*P, Name != TEXT("ServerViewSelf"));
	}
	else if (Name == TEXT("ServerUpdatePing"))
	{
		if (C && C->PlayerState)
		{
			C->PlayerState->SetCompressedPing(uint8(FMath::Clamp(Arg(0).I / 4, 0, 255)));
		}
	}
	else if (Name == TEXT("ServerDrive"))
	{
		// PlayerController.ServerDrive(InForward, InStrafe, aUp, InJump, View): the vehicle's controls (ProcessDrive / 5000)
		P->DriveForward = ArgF(0);
		P->DriveStrafe = ArgF(1);
		P->DriveUp = ArgF(2);
		P->bDriveJump = Arg(3).I != 0;
		const int32 View = Arg(4).I;
		int32 ViewPitch = View / 32768;
		const int32 ViewYaw = 2 * (View - 32768 * ViewPitch);
		ViewPitch *= 2;
		P->View = FRotator(RotDeg(ViewPitch) > 180.0 ? RotDeg(ViewPitch) - 360.0 : RotDeg(ViewPitch), RotDeg(ViewYaw), 0.0);
		if (C)
		{
			C->SetControlRotation(P->View);
		}
		if (Server.bTrace && (++P->DriveCalls % 40) == 0)
		{
			const APawn* Driven = C ? C->GetPawn() : nullptr;
			UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: ServerDrive forward %.0f strafe %.0f up %.0f jump %d view %s; %s at %s speed %.0f"), P->DriveForward, P->DriveStrafe,
				P->DriveUp, P->bDriveJump ? 1 : 0, *P->View.ToCompactString(), Driven ? *Driven->GetName() : TEXT("-"),
				Driven ? *Driven->GetActorLocation().ToCompactString() : TEXT("-"), Driven ? Driven->GetVelocity().Size() : 0.0);
		}
	}
	else if (Name == TEXT("ServerChangeDriverPosition"))
	{
		// Vehicle.ServerChangeDriverPosition(F) (number keys while driving): 1 the driver's seat, 2.. the WeaponPawns
		AArenaONS_Vehicle* V = nullptr;
		int32 From = -1;
		APawn* Cur = C ? C->GetPawn() : nullptr;
		if (AArenaONS_Vehicle* AsVehicle = Cast<AArenaONS_Vehicle>(Cur))
		{
			V = AsVehicle;
			From = 0;
		}
		else if (AArenaONS_VehicleSeat* AsSeat = Cast<AArenaONS_VehicleSeat>(Cur))
		{
			V = AsSeat->GetVehicle();
			From = AsSeat->GetSeatIndex();
		}
		const int32 To = int32(Arg(0).I) - 1;
		if (V && From >= 0 && To >= 0 && To != From && To < V->GetNumSeats() && !V->GetSeatCharacter(To))
		{
			AArenaCharacter* Rider = V->LeaveSeat(From, true);
			const bool bIn = Rider && V->EnterSeat(Rider, To);
			UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s changes seat %d -> %d in %s: %s"), C->PlayerState ? *C->PlayerState->GetPlayerName() : TEXT("?"),
				From, To, *V->GetVehicleName(), bIn ? TEXT("done") : TEXT("refused"));
		}
	}
	else if (Name == TEXT("VehicleFire") || Name == TEXT("VehicleCeaseFire"))
	{
		P->bVehicleFire[Arg(0).I != 0 ? 1 : 0] = Name == TEXT("VehicleFire");
	}
	else if (Name == TEXT("ServerValidationResponse"))
	{
		if (FUE2MasterUplink* Uplink = Server.GetUplink())
		{
			Uplink->ClientValidationResponse(Conn.GetId(), Arg(0).S);
		}
	}
	else if (Name == TEXT("ChangeName"))
	{
		if (C && C->PlayerState && !Arg(0).S.IsEmpty())
		{
			C->PlayerState->SetPlayerName(Arg(0).S.Left(20));
		}
	}
}

void FUE2ServerWorld::HandleViewChange(FRemotePlayer& P, bool bNext)
{
	if (!P.bSpectator)
	{
		return;
	}
	TArray<int32> Pawns;
	for (const TPair<TWeakObjectPtr<UObject>, int32>& N : NativeActors)
	{
		const AArenaCharacter* Ch = Cast<AArenaCharacter>(N.Key.Get());
		if (Ch && !Ch->IsDead())
		{
			Pawns.Add(N.Value);
		}
	}
	Pawns.Sort();
	if (!bNext || Pawns.Num() == 0)
	{
		P.ViewTargetActor = 0;
		CallPC(P.ConnId, TEXT("ClientSetViewTarget"), { UE2Codec::Actor(P.PCActor) });
		CallPC(P.ConnId, TEXT("ClientSetBehindView"), { UE2Codec::Bool(false) });
		return;
	}
	int32 Index = Pawns.IndexOfByKey(P.ViewTargetActor);
	Index = (Index + 1) % Pawns.Num();
	P.ViewTargetActor = Pawns[Index];
	if (FUE2SvActor* A = Server.FindActor(P.ViewTargetActor))
	{
		P.SpectatorLocation = Cm(FVector(A->Location));
		if (FUE2ServerConnection* Conn = Server.FindConnection(P.ConnId))
		{
			Conn->ReplicateActor(*A, Server.GetTime(), false);
		}
	}
	CallPC(P.ConnId, TEXT("ClientSetViewTarget"), { UE2Codec::Actor(P.ViewTargetActor) });
	CallPC(P.ConnId, TEXT("ClientSetBehindView"), { UE2Codec::Bool(true) });
}

// =================================================================================================
// Movement (PlayerController.ServerMove / MoveAutonomous / SendClientAdjustment)
// =================================================================================================

void FUE2ServerWorld::ServerMove(FRemotePlayer& P, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, bool bRun, bool bDuck, bool bNewJumpStatus,
	bool bDoubleJump, uint8 DoubleClickMove, int32 View, uint8 OldTimeDelta, int32 OldAccel)
{
	if (P.CurrentTimeStamp >= TimeStamp)
	{
		return;
	}
	++P.Moves;
	AUE2RemoteController* C = P.Controller.Get();
	AArenaCharacter* Ch = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr;
	const bool bAlive = Ch && !Ch->IsDead();
	FVector Accel = InAccel * 0.1f;
	const int32 PawnActor = bAlive ? PawnActorOf(Ch) : 0;
	const double Now = FPlatformTime::Seconds();
	if (bAlive && P.AcknowledgedPawn != PawnActor)
	{
		OldTimeDelta = 0;
		Accel = FVector::ZeroVector;
		if (Now - P.LastGivePawn > 0.5)
		{
			P.LastGivePawn = Now;
			CallPC(P.ConnId, TEXT("GivePawn"), { UE2Codec::Actor(PawnActor) });
		}
	}
	// a lost move the client resent in compressed form
	if (OldTimeDelta != 0 && bAlive)
	{
		float OldTimeStamp = TimeStamp - float(OldTimeDelta) / 500.f - 0.001f;
		if (P.CurrentTimeStamp < OldTimeStamp - 0.001f)
		{
			auto Axis = [](int32 V) { V &= 255; return V > 127 ? -(V - 128) : V; };
			const FVector OldA = FVector(Axis(int32(uint32(OldAccel) >> 23)), Axis(int32(uint32(OldAccel) >> 15)), Axis(int32(uint32(OldAccel) >> 7))) * 20.f;
			const bool OldRun = (OldAccel & 64) != 0;
			const bool OldDoubleJump = (OldAccel & 32) != 0;
			const bool OldJump = (OldAccel & 16) != 0;
			if (OldJump)
			{
				P.bJumpStatus = bNewJumpStatus;
			}
			static const uint8 DClick[8] = { 0, 1, 2, 3, 4, 0, 0, 0 };
			OldTimeStamp = FMath::Min(OldTimeStamp, P.CurrentTimeStamp + MaxResponseTime);
			MoveAutonomous(P, OldTimeStamp - P.CurrentTimeStamp, OldRun, bDuck, OldJump, OldDoubleJump, DClick[OldAccel & 7], OldA);
			P.CurrentTimeStamp = OldTimeStamp;
		}
	}
	// view: pitch and yaw / 2, packed in 15 bits each
	int32 ViewPitch = View / 32768;
	const int32 ViewYaw = 2 * (View - 32768 * ViewPitch);
	ViewPitch *= 2;
	P.View = FRotator(RotDeg(ViewPitch), RotDeg(ViewYaw), 0.0);
	if (P.View.Pitch > 180.0)
	{
		P.View.Pitch -= 360.0;
	}
	const bool bPressedJump = P.bJumpStatus != bNewJumpStatus;
	P.bJumpStatus = bNewJumpStatus;
	const float DeltaTime = FMath::Min(MaxResponseTime, TimeStamp - P.CurrentTimeStamp);
	P.CurrentTimeStamp = TimeStamp;
	if (C)
	{
		C->SetControlRotation(P.View);
	}
	if (!bAlive)
	{
		// spectating / dead: the controller is where the client says
		if (P.bSpectator && !ClientLoc.IsZero())
		{
			P.SpectatorLocation = Cm(ClientLoc);
		}
		if (!ClientLoc.IsZero() && Now - P.LastUpdateTime > 0.3)
		{
			P.bPendingAdjustment = true;
			P.AdjustTimeStamp = TimeStamp;
			P.LastUpdateTime = Now;
		}
		return;
	}
	if (P.AcknowledgedPawn != PawnActor)
	{
		return;
	}
	Ch->SetActorRotation(FRotator(0.0, P.View.Yaw, 0.0));
	if (DeltaTime > 0.f)
	{
		MoveAutonomous(P, DeltaTime, bRun, bDuck, bPressedJump, bDoubleJump, DoubleClickMove, Accel);
	}
	if (ClientLoc.IsZero())
	{
		return; // first part of a DualServerMove
	}
	// the client's position against ours
	UCharacterMovementComponent* Move = Ch->GetCharacterMovement();
	const bool bWalking = Move && Move->IsMovingOnGround();
	FVector Ours = FVector(UU(Ch->GetActorLocation()));
	if (bWalking)
	{
		Ours.Z += WalkZOffset;
	}
	const FVector Diff = Ours - ClientLoc;
	const float Error = float(Diff.Size());
	P.ErrorSum += Error;
	P.MaxError = FMath::Max(P.MaxError, Error);
	if (Error <= TrustDistance)
	{
		// close enough: take the client's position (UE2 itself corrects anything above sqrt(3) UU). Walking with only a
		// small height difference (floors that our collision puts a little lower or higher): ours stays on its floor.
		const bool bFloorOnly = bWalking && FVector(Diff.X, Diff.Y, 0.0).Size() < 1.0 && FMath::Abs(Diff.Z) < 12.0;
		if (Error > 0.5f && !bFloorOnly)
		{
			FVector NewLoc = Cm(ClientLoc);
			if (bWalking)
			{
				NewLoc.Z -= WalkZOffset * S;
			}
			Ch->SetActorLocation(NewLoc, false, nullptr, ETeleportType::None);
			++P.Trusted;
		}
		// UE2 confirms the position at least every 0.3 s; a confirmation with our velocity would correct the client
		// whenever our velocity differs by more than 3 UU/s (UE2's own tolerance), so a trusted move is not confirmed.
		// The client keeps at most 100 saved moves (PlayerController.GetFreeMove) and drops older ones itself.
		return;
	}
	++P.Corrections;
	P.bPendingAdjustment = true;
	P.AdjustTimeStamp = TimeStamp;
	P.LastUpdateTime = Now;
}

void FUE2ServerWorld::MoveAutonomous(FRemotePlayer& P, float DeltaTime, bool bRun, bool bDuck, bool bPressedJump, bool bDoubleJump, uint8 DoubleClickMove, const FVector& AccelUU)
{
	AUE2RemoteController* C = P.Controller.Get();
	AArenaCharacter* Ch = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr;
	UArenaCharacterMovement* Move = Ch ? Cast<UArenaCharacterMovement>(Ch->GetCharacterMovement()) : nullptr;
	if (!Move || Ch->IsDead() || DeltaTime <= 0.f)
	{
		return;
	}
	AArenaCharacter::FUE2StepInput In;
	In.bRun = bRun;
	In.bDuck = bDuck;
	In.bPressedJump = bPressedJump;
	In.bDoubleJump = bDoubleJump;
	In.DoubleClickMove = DoubleClickMove;
	const float MaxAccel = FMath::Max(1.f, Move->GetMaxAcceleration());
	const FVector Input = (AccelUU * S / MaxAccel).GetClampedToMaxSize(1.5f);
	Ch->StepInputOverride = &In;
	Move->SimulateNetStep(DeltaTime, Input);
	Ch->StepInputOverride = nullptr;
	++P.Steps;
}

void FUE2ServerWorld::SendAdjustments()
{
	for (TPair<int32, FRemotePlayer>& KV : Players)
	{
		if (KV.Value.bPendingAdjustment)
		{
			SendAdjustment(KV.Value);
			KV.Value.bPendingAdjustment = false;
		}
	}
}

void FUE2ServerWorld::SendAdjustment(FRemotePlayer& P)
{
	AUE2RemoteController* C = P.Controller.Get();
	AArenaCharacter* Ch = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr;
	++P.Adjustments;
	if (!Ch || Ch->IsDead())
	{
		const FVector3f Loc = UU(P.SpectatorLocation);
		CallPC(P.ConnId, TEXT("ShortClientAdjustPosition"), { UE2Codec::Float(P.AdjustTimeStamp), UE2Codec::Name(P.State), UE2Codec::Byte(P.bSpectator ? 4 : 0),
			UE2Codec::Float(Loc.X), UE2Codec::Float(Loc.Y), UE2Codec::Float(Loc.Z), UE2Codec::None() }, true);
		return;
	}
	UCharacterMovementComponent* Move = Ch->GetCharacterMovement();
	const bool bWalking = Move && Move->IsMovingOnGround();
	FVector3f Loc = UU(Ch->GetActorLocation());
	if (bWalking)
	{
		Loc.Z += WalkZOffset;
	}
	const FVector3f Vel = UU(Ch->GetVelocity());
	const uint8 Physics = PhysicsOf(Ch);
	if (Vel.IsNearlyZero(0.01f))
	{
		CallPC(P.ConnId, TEXT("ShortClientAdjustPosition"), { UE2Codec::Float(P.AdjustTimeStamp), UE2Codec::Name(P.State), UE2Codec::Byte(Physics),
			UE2Codec::Float(Loc.X), UE2Codec::Float(Loc.Y), UE2Codec::Float(Loc.Z), UE2Codec::None() }, true);
	}
	else
	{
		CallPC(P.ConnId, TEXT("ClientAdjustPosition"), { UE2Codec::Float(P.AdjustTimeStamp), UE2Codec::Name(P.State), UE2Codec::Byte(Physics),
			UE2Codec::Float(Loc.X), UE2Codec::Float(Loc.Y), UE2Codec::Float(Loc.Z), UE2Codec::Float(Vel.X), UE2Codec::Float(Vel.Y), UE2Codec::Float(Vel.Z),
			UE2Codec::None() }, true);
	}
}

// =================================================================================================
// Game events
// =================================================================================================

void FUE2ServerWorld::OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
	const APlayerState* VictimPS = Victim ? Victim->PlayerState.Get() : (VictimPawn ? VictimPawn->GetPlayerState() : nullptr);
	const APlayerState* KillerPS = Killer ? Killer->PlayerState : nullptr;
	const bool bSuicide = !Killer || Killer == Victim;
	FString DamageType = UE2DamageType(WeaponName, bHeadshot);
	if (bSuicide && (WeaponName.IsEmpty() || WeaponName == TEXT("Suicide")))
	{
		DamageType = TEXT("Engine.Suicided");
	}
	const FUE2NetValue Dam = UE2Codec::Static(Server.FindStatic(DamageType));
	// the pawn is torn off now (ragdoll on the clients)
	if (VictimPawn)
	{
		if (FUE2SvActor* A = Server.FindActor(PawnActorOf(VictimPawn)))
		{
			A->Set(TEXT("HitDamageType"), Dam);
			// xPawn.PlayDyingSound
			PlayPawnSound(*A, VictimPawn, TEXT("DeathSounds"), -1, 1.f, 500.f, 0);
		}
	}
	if (bHeadshot && !bSuicide)
	{
		OnLocalMessage(TEXT("XGame.SpecialKillMessage"), 0, nullptr, nullptr, Killer);
	}
	// GameInfo.Killed: death messages only for players (Killed.bIsPlayer), not for monsters
	if (!VictimPS)
	{
		return;
	}
	// XGame.xDeathMessage (Invasion: SkaarjPack.InvasionDeathMessage, "a monster" as the killer without a PRI);
	// switch 1 = suicide (victim as PRI 2)
	const AArenaGameState* KGS = World.IsValid() ? World->GetGameState<AArenaGameState>() : nullptr;
	const TCHAR* DeathClass = KGS && KGS->ModeId == TEXT("INV") ? TEXT("SkaarjPack.InvasionDeathMessage") : TEXT("XGame.xDeathMessage");
	if (bSuicide)
	{
		BroadcastLocalized(DeathClass, 1, nullptr, VictimPS, &Dam);
	}
	else
	{
		BroadcastLocalized(DeathClass, 0, KillerPS, VictimPS, &Dam);
		// xDeathMessage.ClientReceive: "You killed <victim>" to the killer, "You were killed by <killer>" to the victim
		OnLocalMessage(TEXT("XGame.xKillerMessagePlus"), 0, const_cast<APlayerState*>(KillerPS), const_cast<APlayerState*>(VictimPS), Killer);
		OnLocalMessage(TEXT("XGame.xVictimMessage"), 0, const_cast<APlayerState*>(KillerPS), nullptr, Victim);
	}
	++KillMessages;
}

void FUE2ServerWorld::OnLocalMessage(const TCHAR* MessageClass, int32 Switch, APlayerState* PRI1, APlayerState* PRI2, AController* OnlyTo)
{
	if (!OnlyTo)
	{
		BroadcastLocalized(MessageClass, Switch, PRI1, PRI2, nullptr);
		return;
	}
	if (const FRemotePlayer* P = FindPlayerByController(OnlyTo))
	{
		const FUE2ObjectRef Class = Server.FindStatic(MessageClass);
		if (Class.IsStatic())
		{
			CallPC(P->ConnId, TEXT("ReceiveLocalizedMessage"), { UE2Codec::Static(Class), Switch ? UE2Codec::Int(Switch) : FUE2NetValue(),
				UE2Codec::Actor(PRIActorOf(PRI1)), UE2Codec::Actor(PRIActorOf(PRI2)), UE2Codec::None() });
		}
	}
}

bool FUE2ServerWorld::TravelToNextMap(AArenaGameMode* GM)
{
	if (!GM)
	{
		return false;
	}
	const FString Options = GM->GetMatchOptions();
	// the rotation: ?MapList=A,B,C (the Host Game map list), else every map of this game type, in name order
	TArray<FString> Rotation;
	UGameplayStatics::ParseOption(TEXT("?") + Options, TEXT("MapList")).ParseIntoArray(Rotation, TEXT(","));
	if (Rotation.Num() == 0)
	{
		FString Prefix;
		MapName.Split(TEXT("-"), &Prefix, nullptr);
		for (const FArenaLayoutInfo& L : AArenaLevelBuilder::GetLayouts())
		{
			// only maps the clients have: the install's (UT2004 clients load the map from their own install)
			if (!L.MapName.IsEmpty() && !L.bCustom && L.GameTypePrefix.Equals(Prefix, ESearchCase::IgnoreCase) && Server.HasPackageFile(L.MapName))
			{
				Rotation.AddUnique(L.MapName);
			}
		}
		Rotation.Sort();
	}
	if (Rotation.Num() == 0)
	{
		Rotation.Add(MapName);
	}
	const int32 Current = Rotation.IndexOfByPredicate([this](const FString& M) { return M.Equals(MapName, ESearchCase::IgnoreCase); });
	return TravelToMap(GM, Rotation[(Current + 1) % Rotation.Num()], Rotation.Num());
}

bool FUE2ServerWorld::TravelToMap(AArenaGameMode* GM, const FString& Next, int32 RotationSize)
{
	if (!GM || Next.IsEmpty())
	{
		return false;
	}
	const FString Options = GM->GetMatchOptions();
	const FString NextLayout = AArenaLevelBuilder::MakeLayoutId(Next);
	// the same options with the next map
	TArray<FString> Parts;
	Options.ParseIntoArray(Parts, TEXT("?"));
	bool bLayout = false;
	for (FString& Part : Parts)
	{
		if (Part.StartsWith(TEXT("Layout="), ESearchCase::IgnoreCase))
		{
			Part = TEXT("Layout=") + NextLayout;
			bLayout = true;
		}
	}
	if (!bLayout)
	{
		Parts.Add(TEXT("Layout=") + NextLayout);
	}
	const FString NextOptions = FString::Join(Parts, TEXT("?"));
	// as UE2's ServerTravel: every client is told to follow (ClientTravel(map, TRAVEL_Relative, false)) and reconnects
	for (const TPair<int32, FRemotePlayer>& P : Players)
	{
		CallPC(P.Key, TEXT("ClientTravel"), { UE2Codec::Str(Next.ToUpper()), UE2Codec::Byte(2), UE2Codec::Bool(false) });
	}
	for (const TUniquePtr<FUE2ServerConnection>& C : Server.GetConnections())
	{
		C->Flush(Server.GetTime());
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: next map %s (rotation of %d), %d clients told to follow"), *Next, RotationSize, Players.Num());
	const FString Level = AArenaLevelBuilder::GetLevelForLayout(NextLayout);
	UGameplayStatics::OpenLevel(GM, FName(*Level), true, NextOptions);
	return true;
}

void FUE2ServerWorld::OnFlagEvent(AArenaFlag* Flag, AController* By, int32 Switch)
{
	if (!Flag)
	{
		return;
	}
	const FUE2NetValue Team = UE2Codec::Actor(TeamActor(Flag->GetTeam()));
	BroadcastLocalized(TEXT("UnrealGame.CTFMessage"), Switch, By ? By->PlayerState : nullptr, nullptr, &Team);
}

void FUE2ServerWorld::OnChat(APlayerState* From, const FString& Text, bool bTeam)
{
	// PlayerController.ServerTeamSay: plain chat in a game without teams
	if (bTeam && World.IsValid())
	{
		const AArenaGameState* ChatGS = World->GetGameState<AArenaGameState>();
		bTeam = ChatGS && ChatGS->IsTeamGame();
	}
	if (Text.IsEmpty())
	{
		return;
	}
	const AArenaPlayerState* FromPS = Cast<AArenaPlayerState>(From);
	const int32 Team = FromPS ? FromPS->Team : -1;
	++ChatLines;
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server chat: %s%s: %s"), From ? *From->GetPlayerName() : TEXT("?"), bTeam ? TEXT(" (team)") : TEXT(""), *Text);
	for (const TPair<int32, FRemotePlayer>& P : Players)
	{
		const AUE2RemoteController* C = P.Value.Controller.Get();
		const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (bTeam && (!PS || PS->Team != Team))
		{
			continue;
		}
		CallPC(P.Key, TEXT("TeamMessage"), { UE2Codec::Actor(PRIActorOf(From)), UE2Codec::Str(Text), UE2Codec::Name(bTeam ? TEXT("TeamSay") : TEXT("Say")) });
	}
	// the listen server's own players see remote players' chat in their console
	if (UWorld* W = World.Get())
	{
		const bool bRemote = From && Cast<AUE2RemoteController>(From->GetOwningController()) != nullptr;
		if (bRemote)
		{
			for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
			{
				AArenaPlayerController* LocalPC = Cast<AArenaPlayerController>(It->Get());
				const AArenaPlayerState* LocalPS = LocalPC ? LocalPC->GetPlayerState<AArenaPlayerState>() : nullptr;
				if (LocalPC && (!bTeam || (LocalPS && LocalPS->Team == Team)))
				{
					ArenaChat::ShowChat(LocalPC, From, Text, bTeam);
				}
			}
		}
	}
}

FString FUE2ServerWorld::DescribeSummary() const
{
	int32 Moves = 0, Corrections = 0, Trusted = 0, Adjust = 0;
	double Err = 0.0;
	for (const TPair<int32, FRemotePlayer>& P : Players)
	{
		Moves += P.Value.Moves;
		Corrections += P.Value.Corrections;
		Trusted += P.Value.Trusted;
		Adjust += P.Value.Adjustments;
		Err += P.Value.ErrorSum;
	}
	return FString::Printf(TEXT("%d remote players, moves %d, adjustments %d, corrections %d, client positions taken %d, average error %.2f UU, kill messages %d, chat %d, sounds %d"),
		Players.Num(), Moves, Adjust, Corrections, Trusted, Moves ? Err / Moves : 0.0, KillMessages, ChatLines, SoundsSent);
}

#endif // OPENUT2K4_UE2SERVER_BUILD

// =================================================================================================
// Hooks (always compiled)
// =================================================================================================

void UE2ServerHooks::Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			W->OnKilled(Killer, Victim, VictimPawn, WeaponName, bHeadshot);
		}
	}
#endif
}

void UE2ServerHooks::FlagEvent(AArenaFlag* Flag, AController* By, int32 Switch)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			W->OnFlagEvent(Flag, By, Switch);
		}
	}
#endif
}

bool UE2ServerHooks::TravelToNextMap(AArenaGameMode* GM)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			return W->TravelToNextMap(GM);
		}
	}
#endif
	return false;
}

bool UE2ServerHooks::TravelToMap(AArenaGameMode* GM, const FString& MapName)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			return W->TravelToMap(GM, MapName, 1);
		}
	}
#endif
	return false;
}

void UE2ServerHooks::LocalMessage(const TCHAR* MessageClass, int32 Switch, APlayerState* PRI1, APlayerState* PRI2, AController* OnlyTo)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			W->OnLocalMessage(MessageClass, Switch, PRI1, PRI2, OnlyTo);
		}
	}
#endif
}

void UE2ServerHooks::Chat(APlayerState* From, const FString& Text, bool bTeam)
{
#if OPENUT2K4_UE2SERVER_BUILD
	if (FUE2Server* Srv = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = Srv->GetServerWorld())
		{
			W->OnChat(From, Text, bTeam);
		}
	}
#endif
}

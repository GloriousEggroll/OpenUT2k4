#include "UE2NetJoin.h"

#include "../ArenaGameMode.h"
#include "../ArenaGUI2K4.h"
#include "../ArenaContent.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaRuntimeMap.h"
#include "../ArenaUserSettings.h"
#include "GameFramework/GameModeBase.h"
#include "UnrealClient.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DelayedAutoRegister.h"
#include "UObject/UObjectGlobals.h"
#include "UE2NetConnection.h"
#include "UE2NetDownload.h"
#include "UE2NetWorld.h"
#include "UE2NetPlay.h"
#include "../UScript/UScriptCore.h"
#include "UE2NetLog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

/**
 * Joining flow (Docs/UE2Net.md "Joining"):
 *   JoinServer -> FUE2NetConnection handshake -> WELCOME names the map -> we open our converted copy of that map
 *   (?UE2Net=1: no bots, the local player spectates) -> once it is loaded, JOIN -> the server replicates actors.
 * Until the replicated actors drive the world (step 3), the player watches the map with a status line on screen.
 * Leaving the map (menu, quit) or a failure closes the connection.
 */
namespace UE2NetJoinLocal
{
#if !UE_BUILD_SHIPPING
	void AutomationFallback();
#endif
	TUniquePtr<FUE2NetConnection> GConnection;
	FTSTicker::FDelegateHandle GTickHandle;
	/** The map we opened for the connection; JOIN goes out when it has loaded. */
	bool bWaitingForMap = false;
	/** Last message to show after a failure (cleared by the next join). */
	FString GLastError;
	/** The server's FAILCODE of the last rejected join (e.g. "NEEDSTATS"), for the UI prompt. */
	FString GLastFailCode;
	double GJoinedTime = 0.0;
	bool bAutoConnectDone = false;
	/** The server announced a map change (ClientTravel): when it drops the connection, join it again. */
	bool bServerTravel = false;
	/** When to reconnect after a map change (0: not scheduled), and to where. */
	double GReconnectTime = 0.0;
	FString GReconnectAddress;
	/** The address / URL of the current connection, as given to JoinServer (with its options). */
	FString GCurrentURL;
	/** Package downloads and their conversion between WELCOME and loading the map (Net/UE2NetDownload.h). */
	TUniquePtr<FUE2NetDownloader> GDownloader;
	/** Step 3: the server's actors and game state in our world (created when the map has loaded and JOIN went out). */
	TUniquePtr<FUE2NetWorld> GNetWorld;
	/** Step 4: our own pawn, movement and input on the server (Net/UE2NetPlay.h); lives with GNetWorld. */
	TUniquePtr<FUE2NetPlay> GNetPlay;

	void ResetNetWorld()
	{
		GNetPlay.Reset();
		GNetWorld.Reset();
	}

	/**
	 * Parses "host[:port]", "ut2004://host[:port][/Map][?Option[=Value]...]" (also unreal://) into connection settings.
	 * ?Name= sets the player name (default: the settings' name), ?SpectatorOnly= the spectator flag, other options
	 * (?Password=, ?Team=, ?Character=, ...) go to the server's login URL unchanged.
	 */
	/**
	 * Fills the stats account in the connection settings. The defaults match UT2004: off and blank unless the player
	 * turned on Track Stats in the settings. Non-shipping -UE2TestStats generates a throw-away account for this run only
	 * (not persisted, never logged) so stats-required servers can be tested; it is compiled out of Shipping.
	 */
	void ApplyStatsSettings(FUE2NetConnection::FSettings& S)
	{
		const UArenaUserSettings* Settings = UArenaUserSettings::Get();
		S.bTrackStats = Settings->bTrackStats;
		S.StatsUsername = Settings->StatsUsername;
		S.StatsPassword = Settings->StatsPassword;
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("UE2TestStats")))
		{
			S.bTrackStats = true;
			S.StatsUsername = FString::Printf(TEXT("OpenUT2k4test%04u"), FMath::RandRange(0, 9999));
			S.StatsPassword = FString::Printf(TEXT("t%08x"), uint32(FMath::Rand()) ^ uint32(FPlatformTime::Cycles()));
			UE_LOG(LogUE2Net, Log, TEXT("UE2TestStats: using a generated throw-away stats username \"%s\" for this run"), *S.StatsUsername);
		}
#endif
	}

	bool ParseServerURL(const FString& InURL, FUE2NetConnection::FSettings& Out)
	{
		FString URL = InURL.TrimStartAndEnd();
		for (const TCHAR* Scheme : { TEXT("ut2004://"), TEXT("unreal://"), TEXT("ut2004:"), TEXT("unreal:") })
		{
			if (URL.StartsWith(Scheme, ESearchCase::IgnoreCase))
			{
				URL.RightChopInline(FCString::Strlen(Scheme));
				break;
			}
		}
		FString HostPart = URL, OptionPart;
		URL.Split(TEXT("?"), &HostPart, &OptionPart);
		// a map after the address ("host:port/DM-Rankin") is the client's travel URL: the server decides the map
		int32 Slash = INDEX_NONE;
		if (HostPart.FindChar(TEXT('/'), Slash))
		{
			HostPart.LeftInline(Slash);
		}
		int32 Port = 7777;
		int32 Colon = INDEX_NONE;
		if (HostPart.FindLastChar(TEXT(':'), Colon))
		{
			Port = FCString::Atoi(*HostPart.Mid(Colon + 1));
			HostPart.LeftInline(Colon);
		}
		if (HostPart.IsEmpty() || Port <= 0 || Port > 65535)
		{
			return false;
		}
		Out.Host = HostPart;
		Out.Port = Port;
		const FString DefaultName = UArenaUserSettings::Get()->PlayerName;
		Out.PlayerName = DefaultName.IsEmpty() ? FString(TEXT("Player")) : DefaultName;
		Out.ExtraOptions.Reset();
		Out.bSpectator = false;
		TArray<FString> Options;
		OptionPart.ParseIntoArray(Options, TEXT("?"));
		for (const FString& Option : Options)
		{
			FString Key = Option, Value;
			Option.Split(TEXT("="), &Key, &Value);
			if (Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
			{
				if (!Value.IsEmpty())
				{
					Out.PlayerName = Value;
				}
			}
			else if (Key.Equals(TEXT("SpectatorOnly"), ESearchCase::IgnoreCase))
			{
				// ?SpectatorOnly=1 watches the match; without it we join as a player (step 4: Net/UE2NetPlay.h)
				Out.bSpectator = Value.IsEmpty() || Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
			else if (!Key.IsEmpty() && !Option.Contains(TEXT(" ")))
			{
				Out.ExtraOptions += TEXT("?") + Option;
			}
		}
		// the player's settings, as UT2004's PlayerController.UpdateURL puts them in the login URL (options given in the
		// address win)
		const UArenaUserSettings* US = UArenaUserSettings::Get();
		auto AddDefault = [&Out, &Options](const TCHAR* Key, const FString& Value)
		{
			for (const FString& O : Options)
			{
				if (O.StartsWith(FString(Key) + TEXT("="), ESearchCase::IgnoreCase))
				{
					return;
				}
			}
			if (!Value.IsEmpty() && !Value.Contains(TEXT("?")) && !Value.Contains(TEXT(" ")))
			{
				Out.ExtraOptions += FString::Printf(TEXT("?%s=%s"), Key, *Value);
			}
		};
		AddDefault(TEXT("Character"), US->PlayerCharacter);
		AddDefault(TEXT("Sex"), US->PlayerSex);
		AddDefault(TEXT("Team"), FString::FromInt(US->PreferredTeam == 0 || US->PreferredTeam == 1 ? US->PreferredTeam : 255));
		AddDefault(TEXT("Voice"), US->VoiceType);
		return true;
	}

	/** Our game mode for a UT2004 game class (the classes of the stock game types). */
	FString ModeForGameClass(const FString& GameClass, const FString& MapName)
	{
		const FString C = GameClass.ToLower();
		struct FMap { const TCHAR* Suffix; const TCHAR* Mode; };
		static const FMap Table[] = {
			{ TEXT("xteamgame"), TEXT("TDM") }, { TEXT("xctfgame"), TEXT("CTF") }, { TEXT("xvehiclectfgame"), TEXT("CTF") }, { TEXT("xbombingrun"), TEXT("BR") },
			{ TEXT("xdoubledom"), TEXT("DOM") }, { TEXT("onsonslaughtgame"), TEXT("ONS") }, { TEXT("asgameinfo"), TEXT("AS") },
			{ TEXT("xlastmanstandinggame"), TEXT("LMS") }, { TEXT("xmutantgame"), TEXT("MUT") }, { TEXT("invasion"), TEXT("INV") },
			{ TEXT("xdeathmatch"), TEXT("DM") },
		};
		for (const FMap& M : Table)
		{
			if (C.EndsWith(M.Suffix))
			{
				return M.Mode;
			}
		}
		// wrappers and modded game types (3SPN ArenaMaster / TeamArenaMaster / Freon, UTComp, Jailbreak ...): the
		// closest base mode by the class name
		struct FPart { const TCHAR* Part; const TCHAR* Mode; };
		static const FPart Parts[] = {
			{ TEXT("ctf"), TEXT("CTF") }, { TEXT("bombingrun"), TEXT("BR") }, { TEXT("doubledom"), TEXT("DOM") }, { TEXT("domination"), TEXT("DOM") },
			{ TEXT("onslaught"), TEXT("ONS") }, { TEXT("assault"), TEXT("AS") }, { TEXT("invasion"), TEXT("INV") }, { TEXT("mutant"), TEXT("MUT") },
			{ TEXT("lastmanstanding"), TEXT("LMS") }, { TEXT("teamarenamaster"), TEXT("TDM") }, { TEXT("freon"), TEXT("TDM") },
			{ TEXT("jailbreak"), TEXT("TDM") }, { TEXT("clanarena"), TEXT("TDM") }, { TEXT("teamgame"), TEXT("TDM") },
			{ TEXT("arenamaster"), TEXT("DM") }, { TEXT("deathmatch"), TEXT("DM") },
		};
		for (const FPart& P : Parts)
		{
			if (C.Contains(P.Part))
			{
				return P.Mode;
			}
		}
		// unknown (modded) game types: by the map's prefix, else deathmatch
		const FString Prefix = MapName.Left(MapName.Find(TEXT("-"))).ToUpper();
		if (Prefix == TEXT("VCTF"))
		{
			return TEXT("CTF");
		}
		for (const TCHAR* Known : { TEXT("CTF"), TEXT("BR"), TEXT("DOM"), TEXT("ONS"), TEXT("AS") })
		{
			if (Prefix == Known)
			{
				return Known;
			}
		}
		return TEXT("DM");
	}

	UWorld* FindGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World() && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
			{
				return Ctx.World();
			}
		}
		return nullptr;
	}

	void Fail(const FString& Message)
	{
		GLastError = Message;
		UE_LOG(LogUE2Net, Warning, TEXT("%s"), *Message);
		if (GConnection && GConnection->IsOpen())
		{
			GConnection->Close(Message);
		}
	}

	void ContinueWelcome();

	void OnWelcome()
	{
		// missing packages are downloaded (and maps / resources converted) first; JOIN waits for them
		GDownloader = MakeUnique<FUE2NetDownloader>(*GConnection);
		if (GDownloader->Start())
		{
			return; // Tick continues with ContinueWelcome when the downloader is done
		}
		GDownloader.Reset();
		ContinueWelcome();
	}

	/**
	 * Is our converted copy of the map the server's map? The server names it by GUID (its USES entry); the converter
	 * writes the GUID it converted into map.json ("package_guid"). Caches from before that field: the install's file
	 * of that name is what was converted (downloaded maps are converted from the server's own version anyway).
	 */
	bool ConvertedMapMatches(FUE2NetConnection& C, FString& OutWhy)
	{
		const FString MapName = C.GetMapName();
		const FUE2PackageInfo* Server = nullptr;
		for (const FUE2PackageInfo& P : C.GetPackageMap().GetList())
		{
			if (P.Name.Equals(MapName, ESearchCase::IgnoreCase))
			{
				Server = &P;
			}
		}
		if (!Server || !Server->Guid.IsValid())
		{
			return true;
		}
		const FString ServerGuid = UE2GuidToString(Server->Guid);
		// package_guid is near the top of map.json: the first 4 KB are enough
		FString Head;
		{
			TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*(ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json"))));
			if (Ar)
			{
				TArray<uint8> Bytes;
				Bytes.SetNumZeroed(int32(FMath::Min<int64>(Ar->TotalSize(), 4096)));
				Ar->Serialize(Bytes.GetData(), Bytes.Num());
				FFileHelper::BufferToString(Head, Bytes.GetData(), Bytes.Num());
			}
		}
		FString Converted;
		const int32 At = Head.Find(TEXT("\"package_guid\""));
		if (At != INDEX_NONE)
		{
			FString Rest = Head.Mid(At + 14);
			int32 Q1 = INDEX_NONE, Q2 = INDEX_NONE;
			if (Rest.FindChar(TEXT('"'), Q1) && Rest.Mid(Q1 + 1).FindChar(TEXT('"'), Q2))
			{
				Converted = Rest.Mid(Q1 + 1, Q2);
			}
		}
		if (Converted.IsEmpty())
		{
			// an older cache: the install's map of that name (if any) is what it was converted from
			const FString Install = ArenaContent::FindUT2004Install();
			FUE2PackageHeader H;
			if (!Install.IsEmpty() && UE2ReadPackageHeader(Install / TEXT("Maps") / (MapName + TEXT(".ut2")), H))
			{
				Converted = UE2GuidToString(H.Guid);
			}
		}
		if (Converted.IsEmpty() || Converted.Equals(ServerGuid, ESearchCase::IgnoreCase))
		{
			return true;
		}
		OutWhy = FString::Printf(TEXT("The server's %s is another version than the converted one (GUID %s, ours %s)"), *MapName, *ServerGuid, *Converted);
		return false;
	}

	void ContinueWelcome()
	{
		FUE2NetConnection& C = *GConnection;
		FArenaLayoutInfo Info;
		FString Mismatch;
		if (AArenaLevelBuilder::FindLayout(C.GetMapName(), Info) && !ConvertedMapMatches(C, Mismatch))
		{
			// same name, another map: showing ours would put the player in the wrong level
			GLastError = Mismatch;
			UE_LOG(LogUE2Net, Warning, TEXT("%s: joining without a map (decode only)"), *Mismatch);
			bWaitingForMap = false;
			GJoinedTime = FPlatformTime::Seconds();
			C.SendJoin();
			return;
		}
		if (!AArenaLevelBuilder::FindLayout(C.GetMapName(), Info))
		{
			// no converted copy of the map (a custom map: it would have to be downloaded and converted first). Join anyway
			// without a world so the game state still arrives (decode only), and say why nothing is shown.
			GLastError = FString::Printf(TEXT("The server plays %s, which is not in the content cache"), *C.GetMapName());
			UE_LOG(LogUE2Net, Warning, TEXT("%s: joining without a map (decode only)"), *GLastError);
			bWaitingForMap = false;
			GJoinedTime = FPlatformTime::Seconds();
			C.SendJoin();
			return;
		}
		UWorld* World = FindGameWorld();
		if (!World)
		{
			Fail(TEXT("No game world to load the map into"));
			return;
		}
		const FString Mode = ModeForGameClass(C.GetGameClass(), C.GetMapName());
		const FString Options = FString::Printf(TEXT("Mode=%s?Layout=%s?Bots=0?Time=0?Goal=0?SpectatorOnly=1?UE2Net=1"), *Mode, *Info.Id);
		const FString Level = AArenaLevelBuilder::GetLevelForLayout(Info.Id);
		UE_LOG(LogUE2Net, Log, TEXT("Loading %s for %s: %s?%s"), *C.GetMapName(), *C.GetAddressString(), *Level, *Options);
		bWaitingForMap = true;
		UGameplayStatics::OpenLevel(World, FName(*Level), true, Options);
	}

	void OnPostLoadMap(UWorld* World)
	{
		if (!World || !GConnection || !GConnection->IsOpen())
		{
			return;
		}
		if (!World->URL.HasOption(TEXT("UE2Net")))
		{
			// the player left the server's map (menu, another match)
			bWaitingForMap = false;
			ResetNetWorld();
			GConnection->Close(TEXT(""));
			return;
		}
		if (bWaitingForMap)
		{
			bWaitingForMap = false;
			GJoinedTime = FPlatformTime::Seconds();
			GNetWorld = MakeUnique<FUE2NetWorld>(*GConnection, World);
			GNetPlay = MakeUnique<FUE2NetPlay>(*GConnection, World);
			GConnection->SendJoin();
		}
	}

	bool Tick(float DeltaTime)
	{
		// -UE2Connect=host:port: join once the game is up (test hook)
		if (!bAutoConnectDone && FindGameWorld())
		{
			bAutoConnectDone = true;
			FString Address;
			if (FParse::Value(FCommandLine::Get(), TEXT("UE2Connect="), Address))
			{
				ArenaNet::JoinServer(Address);
			}
		}
		if (GReconnectTime > 0.0 && FPlatformTime::Seconds() >= GReconnectTime)
		{
			GReconnectTime = 0.0;
			ArenaNet::JoinServer(GReconnectAddress);
		}
		if (GConnection)
		{
			GConnection->Tick(FPlatformTime::Seconds());
			if (GNetWorld)
			{
				if (!GNetWorld->GetWorld() || !GConnection->IsOpen())
				{
					ResetNetWorld();
				}
				else
				{
					GNetWorld->Tick(DeltaTime);
					if (GNetPlay)
					{
						GNetPlay->Tick(DeltaTime);
					}
				}
			}
			if (GDownloader)
			{
				if (!GConnection->IsOpen())
				{
					GDownloader.Reset();
				}
				else
				{
					GDownloader->Tick(FPlatformTime::Seconds());
					if (GDownloader->IsDone())
					{
						GDownloader.Reset();
						ContinueWelcome();
					}
				}
			}
			// -UE2NetViewPlayer: once joined, ask the server to let our spectator follow a player (test hook; the
			// followed pawn then stays relevant, and it exercises client -> server function calls)
			static double LastView = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (GConnection->GetState() == FUE2NetConnection::EState::Joined && GJoinedTime > 0.0 && Now - GJoinedTime > 3.0
				&& Now - LastView > 20.0 && FParse::Param(FCommandLine::Get(), TEXT("UE2NetViewPlayer")))
			{
				if (const int32 Ch = GConnection->FindPlayerControllerChannel())
				{
					LastView = Now;
					GConnection->SendFunction(Ch, TEXT("ServerViewNextPlayer"), {});
				}
			}
		}
#if !UE_BUILD_SHIPPING
		AutomationFallback();
#endif
		return true;
	}

#if !UE_BUILD_SHIPPING
	/**
	 * -AutoScreenshot / -AutoQuit from the net ticker, for test runs where no match world ever loads (a rejected join
	 * or a long download leaves us on the menu, where AArenaGameMode's automation never runs). Fires only while no
	 * match game mode is active, so it never doubles up with the match's own handling.
	 */
	void AutomationFallback()
	{
		static bool bInit = false;
		static double StartTime = 0.0;
		static TArray<float> ShotTimes;
		static float QuitTime = -1.f;
		if (!bInit)
		{
			bInit = true;
			StartTime = FPlatformTime::Seconds();
			FString Shots;
			if (FParse::Value(FCommandLine::Get(), TEXT("AutoScreenshot="), Shots, false))
			{
				TArray<FString> Parts;
				Shots.ParseIntoArray(Parts, TEXT(","));
				for (const FString& P : Parts)
				{
					ShotTimes.Add(FCString::Atof(*P));
				}
			}
			FParse::Value(FCommandLine::Get(), TEXT("AutoQuit="), QuitTime);
		}
		if (ShotTimes.Num() == 0 && QuitTime < 0.f)
		{
			return;
		}
		// let AArenaGameMode handle it whenever a match world is up
		UWorld* World = FindGameWorld();
		if (World && World->GetAuthGameMode() && World->GetAuthGameMode()->IsA(AArenaGameMode::StaticClass()))
		{
			return;
		}
		const float Now = float(FPlatformTime::Seconds() - StartTime);
		if (ShotTimes.Num() > 0 && Now >= ShotTimes[0])
		{
			ShotTimes.RemoveAt(0);
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("auto_%03d.png"), FMath::RoundToInt(Now)));
			UE_LOG(LogUE2Net, Log, TEXT("AutoScreenshot (no match world): %s"), *File);
			FScreenshotRequest::RequestScreenshot(File, false, false);
		}
		if (QuitTime > 0.f && Now >= QuitTime)
		{
			QuitTime = -1.f;
			UE_LOG(LogUE2Net, Log, TEXT("AutoQuit (no match world): fail code \"%s\", status \"%s\""), *GLastFailCode, *GLastError);
			if (GEngine)
			{
				GEngine->Exec(World, TEXT("quit"));
			}
			FPlatformMisc::RequestExit(false);
		}
	}
#endif

	void Register()
	{
		GTickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
		FCoreUObjectDelegates::PostLoadMapWithWorld.AddStatic(&OnPostLoadMap);
		FCoreDelegates::OnPreExit.AddStatic([]() { ArenaNet::LeaveServer(); });
	}

	FDelayedAutoRegisterHelper GRegister(EDelayedRegisterRunPhase::EndOfEngineInit, &Register);
}

namespace ArenaNet
{
	bool JoinServer(const FString& Address)
	{
		using namespace UE2NetJoinLocal;
		FUE2NetConnection::FSettings S;
		if (!ParseServerURL(Address, S))
		{
			UE_LOG(LogUE2Net, Warning, TEXT("JoinServer: bad address \"%s\""), *Address);
			return false;
		}
		LeaveServer();
		GLastError.Reset();
		UE2NetDownload::ClearJoinNotice();
		bServerTravel = false;
		GCurrentURL = Address;
		S.bTrace = FParse::Param(FCommandLine::Get(), TEXT("UE2NetTrace"));
		S.bTraceBits = FParse::Param(FCommandLine::Get(), TEXT("UE2NetTraceBits"));
		ApplyStatsSettings(S);
		GConnection = MakeUnique<FUE2NetConnection>();
		GConnection->OnWelcome = &OnWelcome;
		GConnection->OnClosed = [](const FString& Reason)
		{
			if (bServerTravel && GConnection)
			{
				// UE2 servers change maps by sending every client ClientTravel and closing the connections; clients then
				// connect again (the server needs a few seconds to load the next map: HELLO is resent until it answers)
				bServerTravel = false;
				GReconnectAddress = GCurrentURL;
				GReconnectTime = FPlatformTime::Seconds() + 2.0;
				UE_LOG(LogUE2Net, Log, TEXT("Map change: reconnecting to %s"), *GReconnectAddress);
				return;
			}
			if (!Reason.IsEmpty())
			{
				GLastError = Reason;
			}
			GLastFailCode = GConnection ? GConnection->GetCloseFailCode() : FString();
		};
		GConnection->OnFunctionCall = [](const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
		{
			if (Function && Function->Name == TEXT("ClientTravel"))
			{
				bServerTravel = true;
			}
			if (GNetPlay && GNetPlay->FunctionCall(Actor, Function, Params))
			{
				return; // our own pawn's movement, possession and weapons (Net/UE2NetPlay.h)
			}
			if (GNetWorld)
			{
				GNetWorld->FunctionCall(Actor, Function, Params);
			}
		};
		// step 3: the world layer turns the replicated actors into ours (Net/UE2NetWorld.h)
		GConnection->OnActorOpened = [](const FUE2NetActor& Actor)
		{
			if (GNetPlay)
			{
				GNetPlay->ActorOpened(Actor);
			}
			if (GNetWorld)
			{
				GNetWorld->ActorOpened(Actor);
			}
		};
		GConnection->OnActorUpdated = [](const FUE2NetActor& Actor, const TArray<FString>& Changed)
		{
			if (GNetPlay)
			{
				GNetPlay->ActorUpdated(Actor, Changed);
			}
			if (GNetWorld)
			{
				GNetWorld->ActorUpdated(Actor, Changed);
			}
		};
		GConnection->OnActorClosed = [](const FUE2NetActor& Actor)
		{
			if (GNetPlay)
			{
				GNetPlay->ActorClosed(Actor);
			}
			if (GNetWorld)
			{
				GNetWorld->ActorClosed(Actor);
			}
		};
		FString Err;
		if (!GConnection->Connect(S, Err))
		{
			Fail(FString::Printf(TEXT("Cannot connect to %s: %s"), *Address, *Err));
			GConnection.Reset();
			return false;
		}
		return true;
	}

	void LeaveServer()
	{
		using namespace UE2NetJoinLocal;
		bWaitingForMap = false;
		GReconnectTime = 0.0;
		UE2NetDownload::ClearJoinNotice();
		GDownloader.Reset();
		ResetNetWorld();
		if (GConnection)
		{
			if (GConnection->IsOpen())
			{
				GConnection->Close(TEXT(""));
			}
			GConnection.Reset();
		}
	}

	bool IsConnectedToServer()
	{
		return UE2NetJoinLocal::GConnection && UE2NetJoinLocal::GConnection->IsOpen();
	}

	FString GetSpectatorBarText(const UWorld* World)
	{
		using namespace UE2NetJoinLocal;
		if (!World || !World->URL.HasOption(TEXT("UE2Net")))
		{
			return FString();
		}
		if (GReconnectTime > 0.0)
		{
			return FString::Printf(TEXT("The server is changing maps, reconnecting to %s..."), *GReconnectAddress);
		}
		// packages left out of this join (missing or another version), on its own line so the bar stays readable
		const FString Notice = UE2NetDownload::GetJoinNotice();
		const FString NoticeLine = Notice.IsEmpty() ? FString() : TEXT("\n") + Notice;
		if (GConnection && GConnection->GetState() == FUE2NetConnection::EState::Joined && GNetPlay && GNetPlay->IsPlaying())
		{
			// playing: the HUD's own lines ("You died. Press Fire to respawn!"); before the first spawn, how to join
			return GNetPlay->HasSpawned() ? FString() : FString(TEXT("Press Fire to join the match")) + NoticeLine;
		}
		if (GConnection && GConnection->GetState() == FUE2NetConnection::EState::Joined)
		{
			// a plain dash: the UT2004 HUD fonts have no em dash. Following a player, the HUD shows "Now viewing" instead.
			const FString Server = GNetWorld && !GNetWorld->GetServerName().IsEmpty() ? GNetWorld->GetServerName() : GConnection->GetAddressString();
			return FString::Printf(TEXT("Spectating %s - Fire: view a player, Alt-Fire: free camera"), *Server) + NoticeLine;
		}
		if (GDownloader && GConnection && GConnection->IsOpen())
		{
			return GDownloader->DescribeStatus();
		}
		return (GConnection ? GConnection->DescribeStatus() : GLastError) + NoticeLine;
	}

	FString GetModStatusText(const UWorld* World)
	{
		using namespace UE2NetJoinLocal;
		if (!World || !World->URL.HasOption(TEXT("UE2Net")) || !GNetPlay)
		{
			return FString();
		}
		return GNetPlay->GetModStatusText();
	}

	bool GetJoinProgress(FString& OutText, float& OutFraction)
	{
		using namespace UE2NetJoinLocal;
		if (!GDownloader || !GConnection || !GConnection->IsOpen())
		{
			return false;
		}
		OutText = GDownloader->DescribeStatus();
		OutFraction = GDownloader->GetProgress();
		return !OutText.IsEmpty();
	}

	FString GetConnectionStatus()
	{
		using namespace UE2NetJoinLocal;
		const FString Notice = UE2NetDownload::GetJoinNotice();
		const FString NoticeSuffix = Notice.IsEmpty() ? FString() : TEXT(" | ") + Notice;
		if (GConnection && GConnection->IsOpen())
		{
			return (GDownloader ? GDownloader->DescribeStatus() : GConnection->DescribeStatus()) + NoticeSuffix;
		}
		return GLastError + NoticeSuffix;
	}

	FString GetLastFailCode()
	{
		return UE2NetJoinLocal::GLastFailCode;
	}

	bool NeedsStatsPrompt()
	{
		return UE2NetJoinLocal::GLastFailCode == TEXT("NEEDSTATS");
	}

	bool GetStatsPrompt(FString& OutHeader, FString& OutDescription, FString& OutYes, FString& OutNo)
	{
		// UT2004's own UT2K4StatsPrompt texts, from GUI2K4.int at runtime (not hard-coded)
		OutHeader = ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2K4StatsPrompt"), TEXT("PromptHeader.Caption"),
			TEXT("This server has UT2004 stats ENABLED!"));
		OutDescription = ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2K4StatsPrompt"), TEXT("PromptDesc.Caption"),
			TEXT("You will only be able to join this server by turning on \"Track Stats\" and setting a unique Stats Username and Password."));
		// UT2004 writes paragraph breaks as "||"
		OutDescription.ReplaceInline(TEXT("||"), TEXT("\n\n"));
		OutYes = ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2K4StatsPrompt"), TEXT("YesButton.Caption"), TEXT("YES"));
		OutNo = ArenaGUI2K4::Loc(TEXT("GUI2K4"), TEXT("UT2K4StatsPrompt"), TEXT("NoButton.Caption"), TEXT("NO"));
		return true;
	}

	bool RetryLastJoin()
	{
		using namespace UE2NetJoinLocal;
		if (GCurrentURL.IsEmpty())
		{
			return false;
		}
		return JoinServer(GCurrentURL);
	}
}

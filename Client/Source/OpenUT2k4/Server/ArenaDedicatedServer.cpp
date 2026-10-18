#include "ArenaDedicatedServer.h"
#include "../OpenUT2k4.h"
#include "../ArenaBotController.h"
#include "../ArenaBranding.h"
#include "../ArenaContent.h"
#include "../ArenaGameMode.h"
#include "../ArenaGameState.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaPlayerState.h"
#include "../Net/UE2Server.h"
#include "../Net/UE2ServerValidator.h"
#include "../Net/UE2ServerConnection.h"
#include "../Net/UE2ServerPlayer.h"
#include "../Net/UE2ServerWorld.h"
#include "ArenaServerConfig.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Package.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include <atomic>

#if PLATFORM_UNIX || PLATFORM_MAC
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#endif
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogArenaServer, Log, All);

namespace ArenaDedicatedServer
{
namespace Local
{
	FArenaServerConfig ServerCfg;
	bool bConfigLoaded = false;
	FString ConfigError;
	FString StateDir;
	/** The ban list the UE2 server uses (ArenaHost::SetUE2BanStore): "id|name". */
	TArray<FString> Bans;
	FTSTicker::FDelegateHandle TickHandle;
	double NextBotCheck = 0.0;
	int32 BotNameIndex = 0;
	/** Consecutive matches that could not be hosted (map not built, UE2 server not started). */
	int32 FailedMatches = 0;
	double WorldSince = 0.0;
	TWeakObjectPtr<UWorld> LastWorld;
	bool bReportedHosting = false;
	FString CurrentMap;
	double GStartTime = 0.0;
	/** Engine frames per second over the last seconds (status). */
	double MeasuredFps = 0.0;
	uint64 FpsFrames = 0;
	double FpsSince = 0.0;

	struct FQueued
	{
		FString Line;
		TFunction<void(const FString&)> OnReply;
	};
	FCriticalSection QueueLock;
	TArray<FQueued> Queue;

	bool IsServerModeImpl()
	{
#if OPENUT2K4_SERVER_BINARY || UE_SERVER
		return true;
#else
		// the OpenUT2k4Server Game target (installed engine) is this module compiled like the game's: its executable's
		// name tells them apart (OpenUT2k4Server, OpenUT2k4Server-Linux-Shipping, OpenUT2k4Server.exe ...)
		static const bool bServer = FString(FPlatformProcess::ExecutableName(false)).StartsWith(TEXT("OpenUT2k4Server"))
			|| FParse::Param(FCommandLine::Get(), TEXT("OpenUT2k4Server"));
		return bServer;
#endif
	}

	void LogLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (const FString& L : Lines)
		{
			UE_LOG(LogArenaServer, Display, TEXT("%s"), *L);
		}
	}

	UWorld* GetGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Game && Ctx.World())
			{
				return Ctx.World();
			}
		}
		return nullptr;
	}

	AArenaGameMode* GetGameMode()
	{
		UWorld* W = GetGameWorld();
		return W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
	}

	// ---- bans: [Engine.AccessControl] BannedIDs of the server ini + <state dir>/Bans.ini ----

	FString BansFile()
	{
		return StateDir / TEXT("Bans.ini");
	}

	void SaveBans()
	{
		FString Text = TEXT("; Bans of this OpenUT2k4 dedicated server (kick / ban from the admin console), merged at start with\n");
		Text += TEXT("; [Engine.AccessControl] BannedIDs of the server ini. One line per ban: BannedIDs=<player id> <name>\n");
		Text += TEXT("[Engine.AccessControl]\n");
		for (const FString& B : Bans)
		{
			FString Id = B, Name;
			B.Split(TEXT("|"), &Id, &Name);
			Text += FString::Printf(TEXT("BannedIDs=%s %s\n"), *Id, *Name);
		}
		IFileManager::Get().MakeDirectory(*StateDir, true);
		if (!FFileHelper::SaveStringToFile(Text, *BansFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogArenaServer, Warning, TEXT("Server: cannot write %s"), *BansFile());
		}
	}

	void LoadBans()
	{
		Bans = ServerCfg.BannedIDs;
		FArenaIniFile Ini;
		if (Ini.Load(BansFile()))
		{
			for (const FString& Line : Ini.GetAll(TEXT("Engine.AccessControl"), TEXT("BannedIDs")))
			{
				FString Id = Line.TrimStartAndEnd(), Name;
				Line.TrimStartAndEnd().Split(TEXT(" "), &Id, &Name);
				if (!Id.IsEmpty() && !Bans.ContainsByPredicate([&Id](const FString& B) { return B.StartsWith(Id + TEXT("|")); }))
				{
					Bans.Add(Id + TEXT("|") + Name.TrimStartAndEnd());
				}
			}
		}
		ArenaHost::SetUE2BanStore([]() -> TArray<FString>& { return Bans; }, []() { SaveBans(); });
	}

	// ---- bots: UT2004 MinPlayers ----

	int32 CountHumans()
	{
		int32 N = 0;
		for (const ArenaHost::FUE2HostPlayer& P : ArenaHost::GetUE2Players())
		{
			N += P.bSpectator ? 0 : 1;
		}
		return N;
	}

	bool AddOneBot(AArenaGameMode* GM)
	{
		return GM && GM->AddBot(BotNameIndex++, -1) != nullptr;
	}

	bool RemoveOneBot(AArenaGameMode* GM)
	{
		if (!GM || GM->GetBots().Num() == 0)
		{
			return false;
		}
		// the newest bot leaves first
		AController* Bot = GM->GetBots().Last();
		if (!Bot)
		{
			return false;
		}
		GM->RemovePawn(Bot);
		GM->Logout(Bot);
		Bot->Destroy();
		return true;
	}

	void BalanceBots(AArenaGameMode* GM)
	{
		const AArenaGameState* GS = GM ? GM->GetArenaGameState() : nullptr;
		if (!GS || GS->Phase != EArenaMatchPhase::Playing)
		{
			return;
		}
		const int32 Humans = CountHumans();
		const int32 Wanted = (Humans > 0 || ServerCfg.bBotsWhenEmpty) ? FMath::Max(0, ServerCfg.MinPlayers - Humans) : 0;
		const int32 Have = GM->GetBots().Num();
		// one at a time, as UT2004's GameInfo does
		if (Have < Wanted)
		{
			AddOneBot(GM);
		}
		else if (Have > Wanted)
		{
			RemoveOneBot(GM);
		}
	}

	// ---- travel ----

	/** The next match on MapName (clients follow when the UE2 server runs). */
	bool TravelTo(const FString& MapName, FString& OutError)
	{
		AArenaGameMode* GM = GetGameMode();
		FArenaLayoutInfo Info;
		if (!AArenaLevelBuilder::FindLayout(MapName, Info) || Info.MapName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("map %s is not in the content cache"), *MapName);
			return false;
		}
		if (GM && UE2ServerHooks::TravelToMap(GM, Info.MapName))
		{
			return true;
		}
		// not hosting (the last match failed): open the map with the server's own options
		const FString Url = ServerCfg.BuildMatchURL(Info.MapName);
		FString Level, Options;
		if (!Url.Split(TEXT("?"), &Level, &Options))
		{
			Level = Url;
		}
		UWorld* W = GetGameWorld();
		if (!W)
		{
			OutError = TEXT("no world");
			return false;
		}
		UGameplayStatics::OpenLevel(W, FName(*Level), true, Options);
		return true;
	}

	FString NextRotationMap()
	{
		if (ServerCfg.Maps.Num() == 0)
		{
			return ServerCfg.Map;
		}
		const int32 Cur = ServerCfg.Maps.IndexOfByPredicate([](const FString& M) { return M.Equals(CurrentMap, ESearchCase::IgnoreCase); });
		return ServerCfg.Maps[(Cur + 1) % ServerCfg.Maps.Num()];
	}

	// ---- the game thread tick ----

	void WatchWorld(double Now)
	{
		UWorld* W = GetGameWorld();
		if (W != LastWorld.Get())
		{
			LastWorld = W;
			WorldSince = Now;
			bReportedHosting = false;
			BotNameIndex = 0;
			if (W)
			{
				const FString Package = UWorld::RemovePIEPrefix(W->GetOutermost()->GetName());
				if (Package.EndsWith(TEXT("/MainMenu")))
				{
					// the game fell back to its menu (the map could not be built, the UE2 server did not start)
					++FailedMatches;
					UE_LOG(LogArenaServer, Error, TEXT("Server: the match on %s could not be hosted (%d in a row)"), *CurrentMap, FailedMatches);
					if (FailedMatches >= 3)
					{
						UE_LOG(LogArenaServer, Error, TEXT("Server: giving up after %d failed matches"), FailedMatches);
						FPlatformMisc::RequestExitWithStatus(false, ArenaDedicatedServer::ExitMatchFailed);
						return;
					}
					FString Error;
					const FString Next = NextRotationMap();
					CurrentMap = Next;
					if (!TravelTo(Next, Error))
					{
						UE_LOG(LogArenaServer, Error, TEXT("Server: %s"), *Error);
						FPlatformMisc::RequestExitWithStatus(false, ArenaDedicatedServer::ExitMatchFailed);
					}
					return;
				}
				if (const AArenaGameMode* GM = W->GetAuthGameMode<AArenaGameMode>())
				{
					CurrentMap = AArenaLevelBuilder::GetMapNameFromLayoutId(GM->GetLayoutId());
				}
			}
		}
		if (W && !bReportedHosting && Now - WorldSince > 2.0)
		{
			if (FUE2Server* S = FUE2Server::Get())
			{
				bReportedHosting = true;
				FailedMatches = 0;
				UE_LOG(LogArenaServer, Display, TEXT("Server: hosting %s (%s) on UDP %d, query port %d"), *S->GetMapName(), *ServerCfg.GameClass, ServerCfg.Port, ServerCfg.Port + 1);
			}
			else if (Now - WorldSince > 15.0)
			{
				// the UE2 server did not start (port in use, no UT2004 install ...): the service manager restarts us later
				UE_LOG(LogArenaServer, Error, TEXT("Server: the UE2 server is not running on %s (port %d in use? see the log above)"), *CurrentMap, ServerCfg.Port);
				FPlatformMisc::RequestExitWithStatus(false, ArenaDedicatedServer::ExitMatchFailed);
				bReportedHosting = true;
			}
		}
	}

	void RunQueuedCommands()
	{
		TArray<FQueued> Batch;
		{
			FScopeLock L(&QueueLock);
			Batch = MoveTemp(Queue);
			Queue.Reset();
		}
		for (FQueued& Q : Batch)
		{
			const FString Reply = ArenaDedicatedServer::ExecCommand(Q.Line);
			if (Q.OnReply)
			{
				Q.OnReply(Reply);
			}
		}
	}

	// ---- stdin ----

	class FStdinReader : public FRunnable
	{
	public:
		std::atomic<bool> bStop{ false };

		virtual uint32 Run() override
		{
			FString Pending;
			const auto Feed = [&Pending](const char* Data, int32 Len)
			{
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Data), Len);
				Pending += FString(Conv.Length(), Conv.Get());
				int32 Nl = INDEX_NONE;
				while (Pending.FindChar('\n', Nl))
				{
					FString Line = Pending.Left(Nl).TrimStartAndEnd();
					Pending.RightChopInline(Nl + 1);
					if (!Line.IsEmpty())
					{
						ArenaDedicatedServer::QueueCommand(Line, [](const FString& Reply) { LogLines(Reply); });
					}
				}
			};
#if PLATFORM_UNIX || PLATFORM_MAC
			char Buffer[1024];
			while (!bStop)
			{
				pollfd P = { 0, POLLIN, 0 };
				const int R = poll(&P, 1, 250);
				if (R < 0)
				{
					if (errno == EINTR)
					{
						continue;
					}
					break;
				}
				if (R == 0)
				{
					continue;
				}
				const ssize_t N = read(0, Buffer, sizeof(Buffer) - 1);
				if (N <= 0)
				{
					break; // end of input (/dev/null under a service manager)
				}
				Feed(Buffer, int32(N));
			}
#elif PLATFORM_WINDOWS
			HANDLE In = GetStdHandle(STD_INPUT_HANDLE);
			if (In == nullptr || In == INVALID_HANDLE_VALUE)
			{
				return 0;
			}
			const DWORD Type = GetFileType(In);
			char Buffer[1024];
			while (!bStop)
			{
				DWORD Read = 0;
				if (Type == FILE_TYPE_PIPE)
				{
					DWORD Avail = 0;
					if (!PeekNamedPipe(In, nullptr, 0, nullptr, &Avail, nullptr))
					{
						break;
					}
					if (Avail == 0)
					{
						Sleep(200);
						continue;
					}
				}
				else if (Type == FILE_TYPE_CHAR)
				{
					if (WaitForSingleObject(In, 250) != WAIT_OBJECT_0)
					{
						continue;
					}
				}
				else if (Type != FILE_TYPE_DISK)
				{
					break;
				}
				if (!ReadFile(In, Buffer, sizeof(Buffer) - 1, &Read, nullptr) || Read == 0)
				{
					if (Type == FILE_TYPE_CHAR)
					{
						continue;
					}
					break;
				}
				Feed(Buffer, int32(Read));
			}
#endif
			return 0;
		}

		virtual void Stop() override
		{
			bStop = true;
		}
	};
	FStdinReader* StdinReader = nullptr;
	FRunnableThread* StdinThread = nullptr;

	// ---- TCP admin console ----

	struct FAdminSession
	{
		FSocket* Socket = nullptr;
		FString Remote;
		FString In;
		bool bAuthed = false;
		int32 Failures = 0;
		bool bClose = false;
	};
	FSocket* AdminListen = nullptr;
	TArray<FAdminSession> AdminSessions;

	void AdminSend(FAdminSession& S, const FString& Text)
	{
		FString Out = Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\n"), TEXT("\r\n"));
		FTCHARToUTF8 Utf8(*Out);
		int32 Sent = 0;
		S.Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent);
	}

	void StartAdminConsole()
	{
		if (!ServerCfg.bAdminConsole)
		{
			return;
		}
		if (ServerCfg.AdminPassword.IsEmpty())
		{
			UE_LOG(LogArenaServer, Warning, TEXT("Server: [OpenUT2k4.AdminConsole] is enabled but AdminPassword is empty: the admin console is not started"));
			return;
		}
		ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedPtr<FInternetAddr> Addr = SS ? SS->GetAddressFromString(ServerCfg.AdminConsoleAddress) : nullptr;
		if (!Addr.IsValid() || !Addr->IsValid())
		{
			UE_LOG(LogArenaServer, Warning, TEXT("Server: admin console address %s is not valid"), *ServerCfg.AdminConsoleAddress);
			return;
		}
		Addr->SetPort(ServerCfg.AdminConsolePort);
		AdminListen = SS->CreateSocket(NAME_Stream, TEXT("OpenUT2k4 admin console"), Addr->GetProtocolType());
		if (!AdminListen || !AdminListen->SetReuseAddr(true) || !AdminListen->Bind(*Addr) || !AdminListen->Listen(4))
		{
			UE_LOG(LogArenaServer, Warning, TEXT("Server: admin console could not listen on %s"), *Addr->ToString(true));
			if (AdminListen)
			{
				SS->DestroySocket(AdminListen);
				AdminListen = nullptr;
			}
			return;
		}
		AdminListen->SetNonBlocking(true);
		UE_LOG(LogArenaServer, Display, TEXT("Server: admin console on TCP %s"), *Addr->ToString(true));
	}

	void StopAdminConsole()
	{
		ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		for (FAdminSession& S : AdminSessions)
		{
			AdminSend(S, TEXT("The server is shutting down.\n"));
			S.Socket->Close();
			SS->DestroySocket(S.Socket);
		}
		AdminSessions.Reset();
		if (AdminListen)
		{
			AdminListen->Close();
			SS->DestroySocket(AdminListen);
			AdminListen = nullptr;
		}
	}

	void TickAdminConsole()
	{
		if (!AdminListen)
		{
			return;
		}
		ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		bool bPending = false;
		while (AdminListen->HasPendingConnection(bPending) && bPending)
		{
			FSocket* C = AdminListen->Accept(TEXT("OpenUT2k4 admin session"));
			if (!C)
			{
				break;
			}
			C->SetNonBlocking(true);
			FAdminSession& S = AdminSessions.AddDefaulted_GetRef();
			S.Socket = C;
			TSharedRef<FInternetAddr> Peer = SS->CreateInternetAddr();
			C->GetPeerAddress(*Peer);
			S.Remote = Peer->ToString(true);
			UE_LOG(LogArenaServer, Display, TEXT("Server: admin console connection from %s"), *S.Remote);
			AdminSend(S, FString::Printf(TEXT("%s dedicated server admin console\nPassword: "), ArenaBranding::GameTitle()));
		}
		for (FAdminSession& S : AdminSessions)
		{
			uint8 Buf[1024];
			int32 Read = 0;
			while (!S.bClose && S.Socket->Recv(Buf, sizeof(Buf), Read, ESocketReceiveFlags::None) && Read > 0)
			{
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Buf), Read);
				S.In += FString(Conv.Length(), Conv.Get());
				if (S.In.Len() > 8192)
				{
					S.bClose = true;
				}
			}
			if (S.Socket->GetConnectionState() != SCS_Connected)
			{
				S.bClose = true;
			}
			int32 Nl = INDEX_NONE;
			while (!S.bClose && S.In.FindChar('\n', Nl))
			{
				const FString Line = S.In.Left(Nl).TrimStartAndEnd();
				S.In.RightChopInline(Nl + 1);
				if (!S.bAuthed)
				{
					if (Line == ServerCfg.AdminPassword)
					{
						S.bAuthed = true;
						UE_LOG(LogArenaServer, Display, TEXT("Server: admin console login from %s"), *S.Remote);
						AdminSend(S, TEXT("Logged in. Type help for the commands, logout to leave.\n> "));
					}
					else if (++S.Failures >= 3)
					{
						UE_LOG(LogArenaServer, Warning, TEXT("Server: admin console login failed 3 times from %s"), *S.Remote);
						AdminSend(S, TEXT("Wrong password.\n"));
						S.bClose = true;
					}
					else
					{
						AdminSend(S, TEXT("Wrong password.\nPassword: "));
					}
					continue;
				}
				if (Line.Equals(TEXT("logout"), ESearchCase::IgnoreCase) || Line.Equals(TEXT("exit"), ESearchCase::IgnoreCase) || Line.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
				{
					AdminSend(S, TEXT("Bye.\n"));
					S.bClose = true;
					continue;
				}
				if (!Line.IsEmpty())
				{
					UE_LOG(LogArenaServer, Display, TEXT("Server: admin %s: %s"), *S.Remote, *Line);
					AdminSend(S, ArenaDedicatedServer::ExecCommand(Line) + TEXT("\n"));
				}
				AdminSend(S, TEXT("> "));
			}
		}
		for (int32 i = AdminSessions.Num() - 1; i >= 0; --i)
		{
			if (AdminSessions[i].bClose)
			{
				AdminSessions[i].Socket->Close();
				SS->DestroySocket(AdminSessions[i].Socket);
				AdminSessions.RemoveAt(i);
			}
		}
	}

	/**
	 * A server needs no more frames than its clients get updates: t.MaxFPS = TickRate. Kept every second, because the
	 * game user settings write their own frame rate limit (0, unlimited) into it when they are applied.
	 */
	void KeepTickRate()
	{
		static IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
		if (MaxFPS && FMath::Abs(MaxFPS->GetFloat() - float(ServerCfg.TickRate)) > 0.5f)
		{
			MaxFPS->Set(float(ServerCfg.TickRate), ECVF_SetByConsole);
		}
	}

	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		RunQueuedCommands();
		TickAdminConsole();
		WatchWorld(Now);
		if (Now >= NextBotCheck)
		{
			NextBotCheck = Now + 1.0;
			BalanceBots(GetGameMode());
			KeepTickRate();
			if (FpsSince > 0.0 && Now - FpsSince >= 5.0)
			{
				MeasuredFps = double(GFrameCounter - FpsFrames) / (Now - FpsSince);
				FpsSince = 0.0;
			}
			if (FpsSince == 0.0)
			{
				FpsSince = Now;
				FpsFrames = GFrameCounter;
			}
		}
		return true;
	}

	/**
	 * A world starts tearing down: on exit (SIGTERM, service stop, quit) this comes before its EndPlay, where the game
	 * session would stop the UE2 server with "the match ended"; the players are told the server is shutting down.
	 */
	void OnWorldBeginTearDown(UWorld*)
	{
		if (IsEngineExitRequested() && FUE2Server::IsRunning())
		{
			UE_LOG(LogArenaServer, Display, TEXT("Server: stopping, players are told the server is shutting down"));
			FUE2Server::Stop(TEXT("the server is shutting down"));
		}
	}

	void OnPreExit()
	{
		UE_LOG(LogArenaServer, Display, TEXT("Server: shutting down"));
		if (FUE2Server::IsRunning())
		{
			// the clients are told (FAILURE / close) before the world goes away
			FUE2Server::Stop(TEXT("the server is shutting down"));
		}
		StopAdminConsole();
		if (StdinReader)
		{
			StdinReader->Stop();
#if PLATFORM_WINDOWS
			// a console read cannot be interrupted: the thread ends with the process
			StdinThread = nullptr;
			StdinReader = nullptr;
#else
			StdinThread->WaitForCompletion();
			delete StdinThread;
			delete StdinReader;
			StdinThread = nullptr;
			StdinReader = nullptr;
#endif
		}
		if (TickHandle.IsValid())
		{
			FTSTicker::RemoveTicker(TickHandle);
			TickHandle.Reset();
		}
	}

	/** Puts a path switch on the command line unless it is there already (ArenaContent reads them lazily). */
	void AddPathSwitch(const TCHAR* Switch, const FString& Value)
	{
		FString Existing;
		if (!Value.IsEmpty() && !FParse::Value(FCommandLine::Get(), *(FString(Switch) + TEXT("=")), Existing))
		{
			FCommandLine::Append(*FString::Printf(TEXT(" -%s=\"%s\""), Switch, *Value));
		}
	}

	/** "3", "Player", "pla" -> a joined connection id; error text otherwise. */
	int32 FindPlayer(const FString& Arg, FString& OutError)
	{
		const TArray<ArenaHost::FUE2HostPlayer> Players = ArenaHost::GetUE2Players();
		if (Arg.IsNumeric())
		{
			const int32 Id = FCString::Atoi(*Arg);
			if (Players.ContainsByPredicate([Id](const ArenaHost::FUE2HostPlayer& P) { return P.ConnId == Id; }))
			{
				return Id;
			}
		}
		for (const ArenaHost::FUE2HostPlayer& P : Players)
		{
			if (P.Name.Equals(Arg, ESearchCase::IgnoreCase))
			{
				return P.ConnId;
			}
		}
		int32 Found = INDEX_NONE;
		for (const ArenaHost::FUE2HostPlayer& P : Players)
		{
			if (P.Name.StartsWith(Arg, ESearchCase::IgnoreCase))
			{
				if (Found != INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("%s matches more than one player"), *Arg);
					return INDEX_NONE;
				}
				Found = P.ConnId;
			}
		}
		if (Found == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("no player %s (see status)"), *Arg);
		}
		return Found;
	}

	FString Status()
	{
		FString Out;
		UWorld* W = GetGameWorld();
		const AArenaGameMode* GM = GetGameMode();
		const AArenaGameState* GS = GM ? GM->GetArenaGameState() : nullptr;
		FUE2Server* S = FUE2Server::Get();
		Out += FString::Printf(TEXT("%s dedicated server \"%s\" %s, UDP %d (query %d)%s%s\n"), ArenaBranding::GameTitle(), *ServerCfg.ServerName, ArenaBranding::VersionTag(),
			ServerCfg.Port, ServerCfg.Port + 1, ServerCfg.bUplink ? TEXT(", master server uplink") : TEXT(""), ServerCfg.bLANServer ? TEXT(", LAN") : TEXT(""));
		if (!GS)
		{
			Out += FString::Printf(TEXT("no match running (%s)"), W ? *W->GetMapName() : TEXT("no world"));
			return Out;
		}
		static const TCHAR* Phases[] = { TEXT("countdown"), TEXT("playing"), TEXT("ended"), TEXT("between rounds") };
		const int32 Phase = FMath::Clamp(int32(GS->Phase), 0, 3);
		Out += FString::Printf(TEXT("map %s, %s (%s), %s, %d:%02d left, goal %d, uptime %.0f s, %.1f frames/s (max %d), hosting %s\n"), *CurrentMap, *GS->ModeName, *ServerCfg.GameClass, Phases[Phase],
			GS->RemainingTime / 60, GS->RemainingTime % 60, GM->GetGoalScore(), FPlatformTime::Seconds() - GStartTime, MeasuredFps, ServerCfg.TickRate, S ? TEXT("yes") : TEXT("NO"));
		if (GM->IsTeamGame())
		{
			Out += FString::Printf(TEXT("teams: red %d, blue %d\n"), GS->TeamScores[0], GS->TeamScores[1]);
		}
		int32 Humans = 0, Bots = 0, Specs = 0;
		Out += TEXT("  id  name                 score deaths  ping  address               player id\n");
		for (const APlayerState* PS : GS->PlayerArray)
		{
			const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS);
			if (!PS)
			{
				continue;
			}
			// remote players: their controller names the connection
			const AUE2RemoteController* RC = Cast<AUE2RemoteController>(PS->GetOwningController());
			const FUE2ServerConnection* C = (RC && S) ? S->FindConnection(RC->ConnId) : nullptr;
			const bool bBot = !RC && PS->IsABot();
			if (bBot)
			{
				++Bots;
			}
			else if (PS->IsOnlyASpectator() || (RC && RC->bRemoteSpectator))
			{
				++Specs;
			}
			else
			{
				++Humans;
			}
			Out += FString::Printf(TEXT("  %-3s %-20s %5d %6d %5s  %-21s %s%s\n"),
				C ? *FString::FromInt(C->GetId()) : TEXT("-"), *PS->GetPlayerName().Left(20), FMath::RoundToInt(PS->GetScore()), APS ? APS->Deaths : 0,
				C ? *FString::FromInt(C->PingMs) : TEXT("-"), C ? *C->GetAddrString() : (bBot ? TEXT("bot") : TEXT("local")),
				C ? *C->PlayerHashId.Left(8) : TEXT(""), (PS->IsOnlyASpectator() || (RC && RC->bRemoteSpectator)) ? TEXT(" (spectator)") : TEXT(""));
		}
		Out += FString::Printf(TEXT("%d players, %d bots (MinPlayers %d), %d spectators; max %d players / %d spectators"), Humans, Bots, ServerCfg.MinPlayers, Specs, ServerCfg.MaxPlayers, ServerCfg.MaxSpectators);
		return Out;
	}

}
}

namespace ArenaDedicatedServer
{
using namespace Local;

bool IsServerMode()
{
	return IsServerModeImpl();
}

const FArenaServerConfig& GetConfig()
{
	return ServerCfg;
}

void QueueCommand(const FString& Line, TFunction<void(const FString& Reply)> OnReply)
{
	FScopeLock L(&QueueLock);
	Queue.Add({ Line, MoveTemp(OnReply) });
}

void OnModuleStartup()
{
	if (!IsServerMode())
	{
		return;
	}
	GStartTime = FPlatformTime::Seconds();
	bConfigLoaded = ServerCfg.Load(FCommandLine::Get(), ConfigError);

	// paths from the ini: the content code reads them from the command line, on first use
	AddPathSwitch(TEXT("ContentCache"), ServerCfg.ContentCache);
	AddPathSwitch(TEXT("UT2004Dir"), ServerCfg.UT2004Dir);
	AddPathSwitch(TEXT("UserContent"), ServerCfg.UserContent);
	if (!ServerCfg.BindAddress.IsEmpty())
	{
		AddPathSwitch(TEXT("UE2ServerBind"), ServerCfg.BindAddress);
	}
	if (!ServerCfg.bUplink && !FParse::Param(FCommandLine::Get(), TEXT("UE2NoUplink")))
	{
		FCommandLine::Append(TEXT(" -UE2NoUplink"));
	}
	if (!FParse::Value(FCommandLine::Get(), TEXT("ServerStateDir="), StateDir))
	{
		StateDir = FPaths::ProjectSavedDir() / TEXT("Server");
	}
	StateDir = FPaths::ConvertRelativePathToFull(StateDir);

	// headless like a dedicated server: UGameEngine::Init creates no viewport, window or local player while GIsClient
	// is off (UArenaServerGameInstance turns it back on before the first map, so LoadMap does not start UE5's net driver)
	GIsClient = false;
	GIsServer = true;
	GetMutableDefault<UGameMapsSettings>()->GameInstanceClass = FSoftClassPath(TEXT("/Script/OpenUT2k4.ArenaServerGameInstance"));

	FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);
	FCoreDelegates::OnPreExit.AddStatic(&OnPreExit);
	ArenaServerService::Start();
}

void OnModuleShutdown()
{
	if (!IsServerMode())
	{
		return;
	}
	uint8 Code = 0;
	FPlatformMisc::HasOverriddenReturnCode(&Code);
	ArenaServerService::Stop(Code);
}

bool PrepareFirstMatch(FString& OutUrl, FString& OutError)
{
	UE_LOG(LogArenaServer, Display, TEXT("%s dedicated server %s starting"), ArenaBranding::GameTitle(), ArenaBranding::VersionTag());
	if (!bConfigLoaded)
	{
		OutError = ConfigError;
		return false;
	}
	for (const FString& F : ServerCfg.LoadedFiles)
	{
		UE_LOG(LogArenaServer, Display, TEXT("Server: config %s"), *F);
	}
	if (ServerCfg.LoadedFiles.Num() == 0)
	{
		UE_LOG(LogArenaServer, Display, TEXT("Server: no ini found, built-in defaults"));
	}
	if (ArenaContent::UseRuntimeContent() && !ArenaContent::IsCacheReady())
	{
		OutError = FString::Printf(TEXT("the content cache %s is not ready. Convert your UT2004 install once with: OpenUT2k4Server -ConvertOnly -UT2004Dir=<install> -ContentCache=%s (or start the game once)"),
			*ArenaContent::GetCacheDir(), *ArenaContent::GetCacheDir());
		return false;
	}
	const FString Install = ArenaContent::FindUT2004Install();
	if (Install.IsEmpty())
	{
		OutError = TEXT("no UT2004 install found: the server needs one for the clients' package list. Set [OpenUT2k4.Paths] UT2004Dir= or -UT2004Dir=<install>");
		return false;
	}
	UE_LOG(LogArenaServer, Display, TEXT("Server: content cache %s, UT2004 install %s, state %s"), *ArenaContent::GetCacheDir(), *Install, *StateDir);

	ServerCfg.ResolveMaps();
	for (const FString& W : ServerCfg.Warnings)
	{
		UE_LOG(LogArenaServer, Warning, TEXT("Server: %s"), *W);
	}
	if (ServerCfg.Map.IsEmpty())
	{
		OutError = FString::Printf(TEXT("no playable map for %s in the content cache"), *ServerCfg.GameClass);
		return false;
	}
	UE_LOG(LogArenaServer, Display, TEXT("Server: %s, map %s, rotation of %d (%s): %s"), *ServerCfg.GameClass, *ServerCfg.Map, ServerCfg.Maps.Num(), *ServerCfg.MapsSource, *FString::Join(ServerCfg.Maps, TEXT(" ")));
	UE_LOG(LogArenaServer, Display, TEXT("Server: \"%s\" UDP %d, max %d players / %d spectators, MinPlayers %d (skill %d), %s%s%s, uplink %s, %d ticks/s%s"),
		*ServerCfg.ServerName, ServerCfg.Port, ServerCfg.MaxPlayers, ServerCfg.MaxSpectators, ServerCfg.MinPlayers, ServerCfg.BotSkill,
		ServerCfg.Mutators.Num() ? *(TEXT("mutators ") + FString::Join(ServerCfg.Mutators, TEXT(","))) : TEXT("no mutators"),
		ServerCfg.GamePassword.IsEmpty() ? TEXT("") : TEXT(", game password"), ServerCfg.AdminPassword.IsEmpty() ? TEXT("") : TEXT(", admin password"),
		ServerCfg.bUplink ? TEXT("on") : TEXT("off"), ServerCfg.TickRate, ServerCfg.bLANServer ? TEXT(", LAN") : TEXT(""));
	if (ServerCfg.ServerPackages.Num() > 0)
	{
		UE_LOG(LogArenaServer, Display, TEXT("Server: %d ServerPackages in the ini %s"), ServerCfg.ServerPackages.Num(),
			ServerCfg.bUseServerPackages ? TEXT("replace the UT2004 install's list (UseServerPackages)") : TEXT("are informational (the UT2004 install's list is used; UseServerPackages=True to use them)"));
	}

	LoadBans();
	UE2ServerValidator::Configure(ServerCfg.Validator, StateDir);
	UE_LOG(LogArenaServer, Display, TEXT("Server: %d bans (%s)"), Bans.Num(), *BansFile());
	StartAdminConsole();
	if (!FParse::Param(FCommandLine::Get(), TEXT("NoServerConsole")) && !ArenaServerService::IsRunningAsService())
	{
		StdinReader = new FStdinReader();
		StdinThread = FRunnableThread::Create(StdinReader, TEXT("OpenUT2k4ServerStdin"), 64 * 1024, TPri_BelowNormal);
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);

	CurrentMap = ServerCfg.Map;
	OutUrl = ServerCfg.BuildMatchURL(ServerCfg.Map);
	const int32 PwAt = OutUrl.Find(TEXT("?GamePassword="), ESearchCase::IgnoreCase);
	UE_LOG(LogArenaServer, Display, TEXT("Server: first match %s"), *(PwAt >= 0 ? OutUrl.Left(PwAt) + TEXT("?GamePassword=(set)") : OutUrl));
	return true;
}

FString ExecCommand(const FString& InLine)
{
	FString Line = InLine.TrimStartAndEnd();
	FString Cmd = Line, Arg;
	Line.Split(TEXT(" "), &Cmd, &Arg);
	Arg.TrimStartAndEndInline();
	Cmd.ToLowerInline();
	// UT2004 "admin kick x" / "admin say y" spellings
	if (Cmd == TEXT("admin") && !Arg.IsEmpty())
	{
		return ExecCommand(Arg);
	}
	AArenaGameMode* GM = GetGameMode();
	FString Error;

	if (Cmd == TEXT("help") || Cmd == TEXT("?"))
	{
		return TEXT("status | players               the match and its players (ids for kick / ban)\n")
			TEXT("say <text>                      a message to every player\n")
			TEXT("kick <id|name>                  remove a player\n")
			TEXT("ban <id|name>                   remove a player and ban the player id\n")
			TEXT("banid <player id> [name]        ban an id (32 hex digits) without a connection\n")
			TEXT("bans / unban <n|player id>      the ban list / lift a ban\n")
			TEXT("map <MapName>                   change the map now (clients follow)\n")
			TEXT("nextmap / restartmap            the next map of the rotation / this map again\n")
			TEXT("maplist                         the rotation\n")
			TEXT("minplayers <n>                  bots fill the match up to n players\n")
			TEXT("addbot [n] / removebot [n]      bots now (MinPlayers moves them back over time)\n")
			TEXT("validator [status|ban|unban|whitelist|unwhitelist|export|import]   the OpenUT2k4 Validator (Server/Docs/Validator.md)\n")
			TEXT("quit | exit | shutdown          stop the server (TCP console: shutdown; logout leaves)");
	}
	if (Cmd == TEXT("status") || Cmd == TEXT("players") || Cmd == TEXT("listplayers"))
	{
		return Status();
	}
	if (Cmd == TEXT("say"))
	{
		if (Arg.IsEmpty())
		{
			return TEXT("say what?");
		}
		ArenaHost::UE2ServerSay(Arg);
		UE_LOG(LogArenaServer, Display, TEXT("Server: say %s"), *Arg);
		return TEXT("said: ") + Arg;
	}
	if (Cmd == TEXT("kick") || Cmd == TEXT("ban") || Cmd == TEXT("kickban"))
	{
		const int32 Id = FindPlayer(Arg, Error);
		if (Id == INDEX_NONE)
		{
			return Error;
		}
		const bool bBan = Cmd != TEXT("kick");
		return ArenaHost::KickUE2Player(Id, bBan) ? FString::Printf(TEXT("%s player %d"), bBan ? TEXT("banned") : TEXT("kicked"), Id) : FString(TEXT("could not remove that player"));
	}
	if (Cmd == TEXT("banid"))
	{
		FString Id = Arg, Name;
		Arg.Split(TEXT(" "), &Id, &Name);
		Id.TrimStartAndEndInline();
		if (Id.Len() != 32)
		{
			return TEXT("usage: banid <32 hex digit player id> [name]");
		}
		for (const TCHAR C : Id)
		{
			if (!FChar::IsHexDigit(C))
			{
				return TEXT("usage: banid <32 hex digit player id> [name]");
			}
		}
		Id.ToLowerInline();
		if (Bans.ContainsByPredicate([&Id](const FString& B) { return B.StartsWith(Id + TEXT("|"), ESearchCase::IgnoreCase); }))
		{
			return TEXT("already banned");
		}
		Bans.Add(Id + TEXT("|") + Name.TrimStartAndEnd().Replace(TEXT("|"), TEXT("")));
		SaveBans();
		return TEXT("banned ") + Id;
	}
	if (Cmd == TEXT("validator"))
	{
		return UE2ServerValidator::AdminCommand(Arg);
	}
	if (Cmd == TEXT("bans") || Cmd == TEXT("banlist"))
	{
		const TArray<FString> List = ArenaHost::GetUE2BanList();
		FString Out = FString::Printf(TEXT("%d bans"), List.Num());
		for (int32 i = 0; i < List.Num(); ++i)
		{
			Out += FString::Printf(TEXT("\n  %d: %s"), i, *List[i]);
		}
		return Out;
	}
	if (Cmd == TEXT("unban"))
	{
		int32 Index = Arg.IsNumeric() ? FCString::Atoi(*Arg) : Bans.IndexOfByPredicate([&Arg](const FString& B) { return !Arg.IsEmpty() && B.StartsWith(Arg, ESearchCase::IgnoreCase); });
		if (!Bans.IsValidIndex(Index))
		{
			return TEXT("usage: unban <number from bans | player id>");
		}
		const FString Entry = Bans[Index];
		ArenaHost::UnbanUE2(Index);
		return TEXT("unbanned ") + Entry.Replace(TEXT("|"), TEXT(" "));
	}
	if (Cmd == TEXT("map") || Cmd == TEXT("switch") || Cmd == TEXT("servertravel"))
	{
		FString MapName = Arg;
		int32 Q = INDEX_NONE;
		if (MapName.FindChar('?', Q))
		{
			MapName.LeftInline(Q);
		}
		if (MapName.IsEmpty())
		{
			return TEXT("usage: map <MapName>");
		}
		if (!TravelTo(MapName, Error))
		{
			return Error;
		}
		return TEXT("changing map to ") + MapName;
	}
	if (Cmd == TEXT("restartmap"))
	{
		return TravelTo(CurrentMap, Error) ? TEXT("restarting ") + CurrentMap : Error;
	}
	if (Cmd == TEXT("nextmap"))
	{
		const FString Next = NextRotationMap();
		return TravelTo(Next, Error) ? TEXT("changing map to ") + Next : Error;
	}
	if (Cmd == TEXT("maplist"))
	{
		return FString::Printf(TEXT("%s rotation (%s): %s"), *ServerCfg.GameClass, *ServerCfg.MapsSource, *FString::Join(ServerCfg.Maps, TEXT(" ")));
	}
	if (Cmd == TEXT("minplayers"))
	{
		if (!Arg.IsNumeric())
		{
			return FString::Printf(TEXT("MinPlayers %d"), ServerCfg.MinPlayers);
		}
		ServerCfg.MinPlayers = FMath::Clamp(FCString::Atoi(*Arg), 0, 64);
		return FString::Printf(TEXT("MinPlayers %d"), ServerCfg.MinPlayers);
	}
	if (Cmd == TEXT("addbot") || Cmd == TEXT("addbots") || Cmd == TEXT("removebot") || Cmd == TEXT("removebots") || Cmd == TEXT("killbots"))
	{
		const bool bAdd = Cmd.StartsWith(TEXT("add"));
		const int32 N = Cmd == TEXT("killbots") ? 32 : (Arg.IsNumeric() ? FMath::Clamp(FCString::Atoi(*Arg), 1, 32) : 1);
		int32 Done = 0;
		for (int32 i = 0; i < N; ++i)
		{
			Done += (bAdd ? AddOneBot(GM) : RemoveOneBot(GM)) ? 1 : 0;
		}
		// MinPlayers keeps its count: move it with the command so the balance does not undo it
		const int32 Humans = CountHumans();
		ServerCfg.MinPlayers = FMath::Clamp((GM ? GM->GetBots().Num() : 0) + Humans, 0, 32);
		if (Humans == 0)
		{
			ServerCfg.bBotsWhenEmpty = GM && GM->GetBots().Num() > 0;
		}
		return FString::Printf(TEXT("%s %d bots (MinPlayers now %d)"), bAdd ? TEXT("added") : TEXT("removed"), Done, ServerCfg.MinPlayers);
	}
	if (Cmd == TEXT("quit") || Cmd == TEXT("exit") || Cmd == TEXT("shutdown"))
	{
		UE_LOG(LogArenaServer, Display, TEXT("Server: shutdown requested by the admin"));
		FPlatformMisc::RequestExitWithStatus(false, 0);
		return TEXT("shutting down");
	}
	return FString::Printf(TEXT("unknown command %s (help lists them)"), *Cmd);
}
}

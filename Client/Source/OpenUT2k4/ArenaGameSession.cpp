#include "ArenaGameSession.h"
#include "OpenUT2k4.h"
#include "ArenaBranding.h"
#include "ArenaContent.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaLevelBuilder.h"
#include "ArenaPlayerState.h"
#include "ArenaUserSettings.h"
#include "Net/UE2Server.h"

#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace ArenaGameSessionLocal
{
	constexpr int32 LANQueryPort = 10777;
	/** The hosting session's query socket (ArenaHost::GetHostQuerySocket). */
	FSocket* GHostQuerySocket = nullptr;
	/** Original FURL default port while a Host Game travel is pending (-1 = none). */
	int32 SavedDefaultPort = -1;

	ISocketSubsystem* Sockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	/** URL-safe option values: % ? = # & space and quotes as %XX. */
	FString EncodeOption(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (C == '%' || C == '?' || C == '=' || C == '#' || C == '&' || C == ' ' || C == '"' || C == '\'' || C == '/' || C == '\\' || C < 32)
			{
				Out += FString::Printf(TEXT("%%%02X"), uint32(C) & 0xFF);
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	FString DecodeOption(const FString& In)
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
	}

	/** UT2004 query packets (Net/ArenaServerBrowser.h): little-endian ints, UE2 FStrings (compact index length with the terminator). */
	struct FWriter
	{
		TArray<uint8> Data;
		void U8(uint8 V) { Data.Add(V); }
		void I32(int32 V)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				Data.Add(uint8((uint32(V) >> (8 * i)) & 0xFF));
			}
		}
		void Index(int32 V)
		{
			uint32 A = uint32(FMath::Abs(V));
			uint8 B0 = uint8((V < 0 ? 0x80 : 0) | (A & 0x3F));
			A >>= 6;
			if (A) B0 |= 0x40;
			U8(B0);
			while (A)
			{
				uint8 B = uint8(A & 0x7F);
				A >>= 7;
				if (A) B |= 0x80;
				U8(B);
			}
		}
		void String(const FString& S)
		{
			const FString T = S.Left(250);
			Index(T.Len() + 1);
			for (const TCHAR C : T)
			{
				U8(uint8(C <= 0xFF ? C : '?'));
			}
			U8(0);
		}
		void Header(uint8 Type)
		{
			U8(0x80); U8(0); U8(0); U8(0); U8(Type);
		}
	};

	FString ShortMapName(const AArenaGameMode* GM)
	{
		const AArenaGameState* GS = GM ? GM->GetGameState<AArenaGameState>() : nullptr;
		if (GM && !GM->GetLayoutId().IsEmpty())
		{
			const FString Map = AArenaLevelBuilder::GetMapNameFromLayoutId(GM->GetLayoutId());
			if (!Map.IsEmpty())
			{
				return Map;
			}
		}
		return GS ? GS->LayoutName : FString();
	}
}

using namespace ArenaGameSessionLocal;

AArenaGameSession::AArenaGameSession()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AArenaGameSession::InitOptions(const FString& Options)
{
	Super::InitOptions(Options);
	ServerName = DecodeOption(UGameplayStatics::ParseOption(Options, TEXT("ServerName")));
	if (ServerName.IsEmpty())
	{
		ServerName = FString::Printf(TEXT("%s Server"), ArenaBranding::GameTitle());
	}
	GamePassword = DecodeOption(UGameplayStatics::ParseOption(Options, TEXT("GamePassword")));
	bLANServer = UGameplayStatics::GetIntOption(Options, TEXT("LANServer"), 0) != 0;
	bAdvertise = UGameplayStatics::GetIntOption(Options, TEXT("Advertise"), 1) != 0;
	bUE2Host = UGameplayStatics::GetIntOption(Options, TEXT("UE2Host"), 0) != 0;
	UE2Port = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("UE2Port"), 7777), 1, 65534);
	bUE2Dedicated = UGameplayStatics::GetIntOption(Options, TEXT("UE2Dedicated"), 0) != 0;
	OptionsString = Options;
}

FString AArenaGameSession::ApproveLogin(const FString& Options)
{
	FString Error = Super::ApproveLogin(Options);
	if (Error.IsEmpty() && !GamePassword.IsEmpty())
	{
		const FString Given = DecodeOption(UGameplayStatics::ParseOption(Options, TEXT("Password")));
		// the listen server's own player logs in with the server's URL (?GamePassword=): never refuse it (a failed
		// local login is fatal in UEngine::LoadMap)
		const bool bHost = DecodeOption(UGameplayStatics::ParseOption(Options, TEXT("GamePassword"))) == GamePassword;
		if (!bHost && Given != GamePassword)
		{
			// UT2004's failure codes: NEEDPW without one, WRONGPW for a wrong one
			Error = Given.IsEmpty() ? TEXT("NEEDPW") : TEXT("WRONGPW");
		}
	}
	if (!Error.IsEmpty())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Host: login refused (%s)"), *Error);
	}
	return Error;
}

void AArenaGameSession::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (SavedDefaultPort >= 0)
	{
		FURL::UrlConfig.DefaultPort = SavedDefaultPort;
		SavedDefaultPort = -1;
	}
	if (World && bUE2Host && World->GetNetMode() != NM_Client)
	{
		// Host Game: our UE2 server (UT2004 3369 / 3374 protocol) for UT2004 and OpenUT2k4 clients
		FUE2ServerSettings Settings;
		Settings.Port = UE2Port;
		Settings.ServerName = ServerName;
		Settings.Password = GamePassword;
		Settings.MaxPlayers = MaxPlayers;
		Settings.MaxSpectators = MaxSpectators;
		Settings.bLAN = bLANServer;
		// the master server uplink only when the player ticked "Show in Server Browsers"
		Settings.bAdvertise = UGameplayStatics::HasOption(OptionsString, TEXT("Advertise")) && bAdvertise && !FParse::Param(FCommandLine::Get(), TEXT("UE2NoUplink"));
		Settings.bListen = !bUE2Dedicated;
		FString Error;
		if (FUE2Server::Start(World, Settings, Error))
		{
			GamePort = UE2Port;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Host: UE2 server \"%s\" on port %d, max %d players / %d spectators%s%s"), *ServerName, GamePort, MaxPlayers, MaxSpectators,
				GamePassword.IsEmpty() ? TEXT("") : TEXT(", password"), bLANServer ? TEXT(", LAN") : TEXT(""));
			// LAN listing always (the LAN tab and Open IP); the internet uplink is the server's (bAdvertise)
			OpenQuerySockets();
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Host: the UE2 server could not start: %s"), *Error);
			SetActorTickEnabled(false);
		}
		return;
	}
	if (!World || World->GetNetMode() == NM_Standalone || World->GetNetMode() == NM_Client)
	{
		SetActorTickEnabled(false);
		return;
	}
	GamePort = World->URL.Port;
	if (UNetDriver* Driver = World->GetNetDriver())
	{
		if (bLANServer)
		{
			// UT2K4Tab_ServerRulesPanel "LAN Server" (-lanplay): no internet rate limit
			Driver->MaxInternetClientRate = Driver->MaxClientRate;
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Host: %s server \"%s\" on port %d, max %d players / %d spectators%s%s"),
		World->GetNetMode() == NM_DedicatedServer ? TEXT("dedicated") : TEXT("listen"), *ServerName, GamePort, MaxPlayers, MaxSpectators,
		GamePassword.IsEmpty() ? TEXT("") : TEXT(", password"), bLANServer ? TEXT(", LAN") : TEXT(""));
	if (bAdvertise)
	{
		OpenQuerySockets();
	}
}

void AArenaGameSession::EndPlay(const EEndPlayReason::Type Reason)
{
	if (bUE2Host)
	{
		FUE2Server::Stop(TEXT("the match ended"));
	}
	CloseQuerySockets();
	Super::EndPlay(Reason);
}

void AArenaGameSession::OpenQuerySockets()
{
	ISocketSubsystem* SS = Sockets();
	if (!SS || GamePort <= 0)
	{
		return;
	}
	const auto Open = [SS](int32 Port, bool bReuse, const TCHAR* What) -> FSocket*
	{
		FSocket* S = SS->CreateSocket(NAME_DGram, What, false);
		if (!S)
		{
			return nullptr;
		}
		S->SetNonBlocking(true);
		S->SetReuseAddr(bReuse);
		S->SetBroadcast(true);
		TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
		Addr->SetAnyAddress();
		Addr->SetPort(Port);
		if (!S->Bind(*Addr))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Host: cannot bind UDP %d for %s (another server?)"), Port, What);
			SS->DestroySocket(S);
			return nullptr;
		}
		return S;
	};
	QuerySocket = Open(GamePort + 1, false, TEXT("OpenUT2k4 server query"));
	GHostQuerySocket = QuerySocket;
	// UT2004 LAN servers all listen on 10777 for the browser's broadcast (shared with other servers on this machine)
	LANSocket = Open(LANQueryPort, true, TEXT("OpenUT2k4 LAN query"));
	UE_LOG(LogOpenUT2k4, Log, TEXT("Host: answering server queries on UDP %d%s"), GamePort + 1, LANSocket ? TEXT(" and LAN queries on UDP 10777") : TEXT(""));
}

void AArenaGameSession::CloseQuerySockets()
{
	if (GHostQuerySocket == QuerySocket)
	{
		GHostQuerySocket = nullptr;
	}
	ISocketSubsystem* SS = Sockets();
	for (FSocket** S : { &QuerySocket, &LANSocket })
	{
		if (*S && SS)
		{
			(*S)->Close();
			SS->DestroySocket(*S);
		}
		*S = nullptr;
	}
}

void AArenaGameSession::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (QuerySocket)
	{
		Receive(QuerySocket);
	}
	if (LANSocket)
	{
		Receive(LANSocket);
	}
}

void AArenaGameSession::Receive(FSocket* Socket)
{
	ISocketSubsystem* SS = Sockets();
	if (!SS)
	{
		return;
	}
	uint8 Buffer[256];
	for (int32 Guard = 0; Guard < 32; ++Guard)
	{
		uint32 Pending = 0;
		if (!Socket->HasPendingData(Pending))
		{
			return;
		}
		TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
		int32 Read = 0;
		if (!Socket->RecvFrom(Buffer, sizeof(Buffer), Read, *From) || Read < 5)
		{
			continue;
		}
		if (Buffer[0] != 0x80 || Buffer[1] != 0 || Buffer[2] != 0 || Buffer[3] != 0 || Buffer[4] > 2)
		{
			continue;
		}
		// queries from other machines (e.g. a master server checking the server) are logged once per address
		{
			static TSet<FString> Seen;
			const FString Who = From->ToString(false);
			if (!Who.StartsWith(TEXT("127.")) && !Seen.Contains(Who) && Seen.Num() < 64)
			{
				Seen.Add(Who);
				UE_LOG(LogOpenUT2k4, Log, TEXT("Host: server query (type %d) from %s on %s"), Buffer[4], *Who, Socket == LANSocket ? TEXT("the LAN port") : TEXT("the query port"));
			}
		}
		// the LAN broadcast only asks for the info; replies always come from the query port (the browser keys servers by it)
		Answer(Socket == LANSocket ? 0 : Buffer[4], From);
	}
}

void AArenaGameSession::Answer(uint8 Type, const TSharedRef<FInternetAddr>& To)
{
	FSocket* Out = QuerySocket ? QuerySocket : LANSocket;
	const AArenaGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr;
	const AArenaGameState* GS = GM ? GM->GetGameState<AArenaGameState>() : nullptr;
	if (!Out || !GS)
	{
		return;
	}
	int32 NumPlayers = 0;
	for (const APlayerState* PS : GS->PlayerArray)
	{
		if (PS && !PS->IsOnlyASpectator())
		{
			++NumPlayers;
		}
	}
	const bool bUE2 = bUE2Host && ArenaHost::IsUE2Hosting();
	FWriter W;
	W.Header(Type);
	if (Type == 0)
	{
		// a UE2 server is joined with the UE2 protocol (UT2004 and OpenUT2k4 clients alike): no OpenServerFlag then
		uint32 Flags = bUE2 ? 0u : ArenaHost::OpenServerFlag;
		Flags |= GamePassword.IsEmpty() ? 0u : 1u;
		Flags |= (bUE2 ? !bUE2Dedicated : GetWorld()->GetNetMode() == NM_ListenServer) ? 8u : 0u;
		Flags |= GS->bInstagib ? 16u : 0u;
		W.I32(0);                     // server id
		W.String(FString());          // ip (the browser uses the sender's)
		W.I32(GamePort);
		W.I32(GamePort + 1);
		W.String(ServerName);
		W.String(ShortMapName(GM));
		W.String(ArenaHost::UE2GameType(GM->GetModeId()));
		W.I32(NumPlayers);
		W.I32(MaxPlayers);
		W.I32(0);                     // ping
		W.I32(int32(Flags));
		W.String(FString());          // skill
	}
	else if (Type == 1)
	{
		const auto Rule = [&W](const FString& K, const FString& V) { W.String(K); W.String(V); };
		Rule(TEXT("ServerMode"), (bUE2 ? bUE2Dedicated : GetWorld()->GetNetMode() == NM_DedicatedServer) ? TEXT("dedicated") : TEXT("non-dedicated"));
		Rule(TEXT("ServerVersion"), FString::Printf(TEXT("%s %s"), ArenaBranding::GameTitle(), ArenaBranding::VersionTag()));
		Rule(TEXT("GameMode"), GS->ModeName);
		Rule(TEXT("GoalScore"), FString::FromInt(GS->GoalScore));
		Rule(TEXT("TimeLimit"), FString::FromInt(GS->TimeLimit));
		Rule(TEXT("MaxSpectators"), FString::FromInt(MaxSpectators));
		Rule(TEXT("GamePassword"), GamePassword.IsEmpty() ? TEXT("False") : TEXT("True"));
		Rule(TEXT("LANServer"), bLANServer ? TEXT("True") : TEXT("False"));
		Rule(TEXT("Engine"), bUE2 ? TEXT("OpenUT2k4 (UT2004 protocol)") : TEXT("Unreal Engine 5 (join with OpenUT2k4)"));
	}
	else
	{
		int32 Id = 0;
		for (const APlayerState* PS : GS->PlayerArray)
		{
			const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS);
			if (!PS || PS->IsOnlyASpectator())
			{
				continue;
			}
			W.I32(++Id);
			W.String(PS->GetPlayerName());
			W.I32(PS->IsABot() ? 0 : int32(PS->GetPingInMilliseconds()));
			W.I32(int32(PS->GetScore()));
			const int32 Team = APS ? APS->Team : -1;
			W.I32(Team == 0 ? int32(1u << 29) : (Team == 1 ? int32(1u << 30) : 0));
			if (W.Data.Num() > 1200)
			{
				break;
			}
		}
	}
	int32 Sent = 0;
	Out->SendTo(W.Data.GetData(), W.Data.Num(), Sent, *To);
}

// =================================================================================================
// ArenaHost
// =================================================================================================

FString ArenaHost::UE2GameType(const FString& ModeId)
{
	static const TMap<FString, FString> Types = {
		{ TEXT("DM"), TEXT("xDeathMatch") }, { TEXT("TDM"), TEXT("xTeamGame") }, { TEXT("CTF"), TEXT("xCTFGame") },
		{ TEXT("BR"), TEXT("xBombingRun") }, { TEXT("DOM"), TEXT("xDoubleDom") }, { TEXT("ONS"), TEXT("ONSOnslaughtGame") },
		{ TEXT("AS"), TEXT("ASGameInfo") }, { TEXT("INV"), TEXT("Invasion") }, { TEXT("LMS"), TEXT("xLastManStandingGame") },
		{ TEXT("MUT"), TEXT("xMutantGame") },
	};
	const FString* T = Types.Find(ModeId.ToUpper());
	return T ? *T : ModeId;
}

FSocket* ArenaHost::GetHostQuerySocket()
{
	return GHostQuerySocket;
}

void ArenaHost::SetListenPort(int32 Port)
{
	if (Port <= 0 || Port > 65534)
	{
		return;
	}
	if (SavedDefaultPort < 0)
	{
		SavedDefaultPort = FURL::UrlConfig.DefaultPort;
	}
	FURL::UrlConfig.DefaultPort = Port;
}

FString ArenaHost::BuildServerOptions(bool bListen)
{
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	FString Out;
	// our UE2 server (Net/UE2Server.h) hosts the match for UT2004 and OpenUT2k4 clients alike; UE5's net driver is not used
	Out += FString::Printf(TEXT("?UE2Host=1?UE2Port=%d"), FMath::Clamp(S->HostPort, 1, 65534));
	if (!bListen)
	{
		Out += TEXT("?UE2Dedicated=1");
	}
	Out += FString::Printf(TEXT("?MaxPlayers=%d?MaxSpectators=%d"), FMath::Clamp(S->HostMaxPlayers, 1, 32), FMath::Clamp(S->HostMaxSpectators, 0, 32));
	if (!S->HostServerName.IsEmpty())
	{
		Out += TEXT("?ServerName=") + EncodeOption(S->HostServerName.Left(64));
	}
	if (!S->HostPassword.IsEmpty())
	{
		Out += TEXT("?GamePassword=") + EncodeOption(S->HostPassword.Left(32));
	}
	Out += FString::Printf(TEXT("?LANServer=%d?Advertise=%d"), S->bHostLANServer ? 1 : 0, S->bHostAdvertise ? 1 : 0);
	return Out;
}

bool ArenaHost::LaunchDedicated(const FString& LevelAndOptions, FString& OutError)
{
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	const TCHAR* Cmd = FCommandLine::Get();
	// the dedicated server mode (-OpenUT2k4Server, Server/Docs/DedicatedServer.md): no local player, admin console, MinPlayers
	// bots. Its command line is UT2004's: <Map>?game=<class>?<option>=<value>... built from the Host Game options.
	FString Options;
	LevelAndOptions.Split(TEXT("?"), nullptr, &Options);
	Options = TEXT("?") + Options;
	auto Opt = [&Options](const TCHAR* Key) { return UGameplayStatics::ParseOption(Options, Key); };
	FString Map = AArenaLevelBuilder::GetMapNameFromLayoutId(Opt(TEXT("Layout")));
	if (Map.IsEmpty())
	{
		OutError = TEXT("The dedicated server can only host UT2004 maps.");
		return false;
	}
	FString Url = FString::Printf(TEXT("%s?game=%s"), *Map, *UE2GameType(Opt(TEXT("Mode"))));
	const auto Add = [&Url, &Opt](const TCHAR* From, const TCHAR* To)
	{
		const FString V = Opt(From);
		if (!V.IsEmpty())
		{
			Url += FString::Printf(TEXT("?%s=%s"), To, *V);
		}
	};
	Add(TEXT("MaxPlayers"), TEXT("MaxPlayers"));
	Add(TEXT("MaxSpectators"), TEXT("MaxSpectators"));
	Add(TEXT("ServerName"), TEXT("ServerName"));
	Add(TEXT("GamePassword"), TEXT("GamePassword"));
	Add(TEXT("LANServer"), TEXT("LANServer"));
	Add(TEXT("Bots"), TEXT("MinPlayers"));
	Add(TEXT("Goal"), TEXT("GoalScore"));
	Add(TEXT("Time"), TEXT("TimeLimit"));
	Add(TEXT("Skill"), TEXT("GameDifficulty"));
	Add(TEXT("Mutator"), TEXT("Mutator"));
	Add(TEXT("Translocator"), TEXT("Translocator"));
	Add(TEXT("Instagib"), TEXT("Instagib"));
	Add(TEXT("MapList"), TEXT("MapList"));
	if (UGameplayStatics::GetIntOption(Options, TEXT("Advertise"), 0) != 0)
	{
		Url += TEXT("?Advertise=1");
	}
	FString Args;
#if WITH_EDITOR
	Args = FString::Printf(TEXT("\"%s\" \"%s\" -game -OpenUT2k4Server -nullrhi -nosound"), *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Url);
#else
	Args = FString::Printf(TEXT("\"%s\" -OpenUT2k4Server -nullrhi -nosound"), *Url);
#endif
	Args += FString::Printf(TEXT(" -port=%d -unattended -log=OpenUT2k4Server.log"), FMath::Clamp(S->HostPort, 1, 65534));
	if (FParse::Param(Cmd, TEXT("RuntimeContent")))
	{
		Args += TEXT(" -RuntimeContent");
	}
	if (FParse::Param(Cmd, TEXT("UE2NoUplink")))
	{
		Args += TEXT(" -NoUplink -UE2NoUplink");
	}
	Args += FString::Printf(TEXT(" \"-ContentCache=%s\""), *ArenaContent::GetCacheDir());
	FString Value;
	if (FParse::Value(Cmd, TEXT("saveddirsuffix="), Value))
	{
		Args += TEXT(" -saveddirsuffix=") + Value + TEXT("_server");
	}
	if (FParse::Value(Cmd, TEXT("UT2004Dir="), Value))
	{
		Args += FString::Printf(TEXT(" \"-UT2004Dir=%s\""), *Value);
	}
	const FString Exe = FPlatformProcess::ExecutablePath();
	const int32 PwAt = Args.Find(TEXT("?GamePassword="), ESearchCase::IgnoreCase);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Host: starting a dedicated server: %s %s"), *Exe, PwAt >= 0 ? *(Args.Left(PwAt) + TEXT("?GamePassword=... (password set)")) : *Args);
	FProcHandle Proc = FPlatformProcess::CreateProc(*Exe, *Args, true, false, false, nullptr, 0, nullptr, nullptr);
	if (!Proc.IsValid())
	{
		OutError = TEXT("The dedicated server could not be started.");
		return false;
	}
	FPlatformProcess::CloseProc(Proc);
	return true;
}

bool ArenaHost::ProbeOpenServer(const FString& Host, int32 Port, float TimeoutSeconds)
{
	ISocketSubsystem* SS = Sockets();
	if (!SS || Host.IsEmpty() || Port <= 0)
	{
		return false;
	}
	TSharedPtr<FInternetAddr> Addr = SS->GetAddressFromString(Host);
	if (!Addr.IsValid() || !Addr->IsValid())
	{
		FAddressInfoResult Info = SS->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Datagram);
		if (Info.ReturnCode != SE_NO_ERROR || Info.Results.Num() == 0)
		{
			return false;
		}
		Addr = Info.Results[0].Address;
	}
	Addr->SetPort(Port + 1);
	FSocket* S = SS->CreateSocket(NAME_DGram, TEXT("OpenUT2k4 probe"), Addr->GetProtocolType());
	if (!S)
	{
		return false;
	}
	S->SetNonBlocking(true);
	FWriter W;
	W.Header(0);
	int32 Sent = 0;
	bool bOpen = false;
	S->SendTo(W.Data.GetData(), W.Data.Num(), Sent, *Addr);
	const double End = FPlatformTime::Seconds() + TimeoutSeconds;
	uint8 Buffer[1024];
	while (!bOpen && FPlatformTime::Seconds() < End)
	{
		uint32 Pending = 0;
		if (!S->HasPendingData(Pending))
		{
			FPlatformProcess::Sleep(0.005f);
			continue;
		}
		TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
		int32 Read = 0;
		if (!S->RecvFrom(Buffer, sizeof(Buffer), Read, *From) || Read < 9 || Buffer[0] != 0x80 || Buffer[4] != 0)
		{
			continue;
		}
		// the flags int sits after: id, ip string, ports, name, map, game type, players, max, ping. Find it by
		// walking the strings (single byte lengths: our names are short; longer ones fail the probe harmlessly).
		int32 Pos = 5 + 4;
		const auto SkipString = [&Pos, Read, &Buffer]() -> bool
		{
			if (Pos >= Read) return false;
			const uint8 B0 = Buffer[Pos];
			if (B0 & 0xC0) return false;
			Pos += 1 + (B0 & 0x3F);
			return Pos <= Read;
		};
		if (!SkipString()) continue;
		Pos += 8;
		if (!SkipString() || !SkipString() || !SkipString()) continue;
		Pos += 12;
		if (Pos + 4 > Read) continue;
		const uint32 Flags = uint32(Buffer[Pos]) | (uint32(Buffer[Pos + 1]) << 8) | (uint32(Buffer[Pos + 2]) << 16) | (uint32(Buffer[Pos + 3]) << 24);
		bOpen = IsOpenServer(Flags);
	}
	S->Close();
	SS->DestroySocket(S);
	return bOpen;
}

bool ArenaHost::JoinServer(const FString& Url)
{
	if (!GEngine)
	{
		return false;
	}
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		UWorld* World = Ctx.World();
		if (!World || !(Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
		{
			continue;
		}
		if (APlayerController* PC = GEngine->GetFirstLocalPlayerController(World))
		{
			const UArenaUserSettings* S = UArenaUserSettings::Get();
			FString Full = Url;
			if (!Full.Contains(TEXT("?Name="), ESearchCase::IgnoreCase) && !S->PlayerName.IsEmpty())
			{
				FString Name;
				for (const TCHAR C : S->PlayerName.Left(24))
				{
					if (C != '?' && C != '=' && C != '#' && C != '/' && C != '\\' && C != '"')
					{
						Name.AppendChar(FChar::IsWhitespace(C) ? TCHAR('_') : C);
					}
				}
				if (!Name.IsEmpty())
				{
					Full += TEXT("?Name=") + Name;
				}
			}
			const int32 PwAt = Full.Find(TEXT("?Password="), ESearchCase::IgnoreCase);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Joining OpenUT2k4 server %s"), *(PwAt >= 0 ? Full.Left(PwAt) : Full));
			PC->ClientTravel(Full, TRAVEL_Absolute, false);
			return true;
		}
	}
	return false;
}

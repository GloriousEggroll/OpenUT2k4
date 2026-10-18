#include "UE2ServerUplink.h"

#include "UE2Server.h"

#if OPENUT2K4_UE2SERVER_BUILD

#include "ArenaServerBrowser.h"
#include "UE2NetCDKey.h"
#include "UE2NetLog.h"
#include "UE2ServerConnection.h"
#include "UE2ServerWorld.h"
#include "../ArenaGameMode.h"
#include "../ArenaGameSession.h"
#include "../ArenaGameState.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaPlayerState.h"
#include "Async/Async.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/World.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

/*
 * The UT2004 master server uplink (IpDrv MasterServerUplink / FMasterServerUplinkLink), as read from the OldUnreal 3374
 * Linux IpDrv.so. See Docs/UE2Net.md "Master server uplink".
 *
 * TCP to the master (port 28902), every message framed as [int32 length][payload], UE2 archive encoding (little-endian
 * ints, compact index FStrings):
 *   master: challenge (FString)
 *   server: CDKeyHash, CDKeyResponse = MD5(key + challenge), "UT2K4SERVER", int version, byte platform, language,
 *           int MatchID (-1), byte bIgnoreUTANBans
 *   master: "APPROVED" (else DENIED / UPGRADE <int> / ...)       server: int bBehindNAT, int bGamespyLink
 *   master: { byte 0, byte HBType, int Code } heartbeat requests (answered by UDP from the matching socket:
 *           80 00 00 00, byte HBType, int Code, to the master's address at the TCP port number)
 *           then { byte 1, int HeartbeatPeriod, 3 dwords }
 *   server: byte 4 STM_MD5Version (int -1), then byte 1 STM_GameState every 60 s
 *   master: byte commands (client challenges, kicks, match id, ...)
 */
struct FUE2MasterUplink::FImpl
{
	enum class EState : uint8 { Idle, Resolving, Connecting, Challenge, Approval, Heartbeats, Ready, Failed };

	FUE2Server* Server = nullptr;
	EState State = EState::Idle;
	TArray<ArenaServerBrowser::FMasterServer> Masters;
	int32 MasterIndex = 0;
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> MasterAddr;
	TArray<uint8> InBuffer;
	double NextAttempt = 0.0;
	double StateTime = 0.0;
	double LastGameState = -100.0;
	int32 MatchId = -1;
	int32 GameStatesSent = 0;
	bool bNoReconnect = false;
	/** Resolution result from the worker thread. */
	TSharedPtr<FString, ESPMode::ThreadSafe> ResolvedIp;
	TSharedPtr<std::atomic<int32>, ESPMode::ThreadSafe> ResolveState;
	struct FChallenge { FString ClientId; double Time = 0.0; };
	TMap<int32, FChallenge> Challenges;

	// ---- archive helpers ----
	struct FWriter
	{
		TArray<uint8> Data;
		void U8(uint8 V) { Data.Add(V); }
		void I32(int32 V) { for (int32 i = 0; i < 4; ++i) { Data.Add(uint8((uint32(V) >> (8 * i)) & 0xFF)); } }
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
		void Str(const FString& S)
		{
			if (S.IsEmpty())
			{
				Index(0);
				return;
			}
			Index(S.Len() + 1);
			for (const TCHAR C : S)
			{
				U8(C < 0x100 ? uint8(C) : uint8(0x7F));
			}
			U8(0);
		}
	};
	struct FReader
	{
		const TArray<uint8>& Data;
		int32 Pos = 0;
		bool bError = false;
		explicit FReader(const TArray<uint8>& In) : Data(In) {}
		uint8 U8() { if (Pos >= Data.Num()) { bError = true; return 0; } return Data[Pos++]; }
		int32 I32() { uint32 V = 0; for (int32 i = 0; i < 4; ++i) { V |= uint32(U8()) << (8 * i); } return int32(V); }
		int32 Index()
		{
			const uint8 B0 = U8();
			int32 V = B0 & 0x3F;
			if (B0 & 0x40)
			{
				int32 Shift = 6;
				for (int32 i = 0; i < 4; ++i)
				{
					const uint8 B = U8();
					V |= int32(B & 0x7F) << Shift;
					Shift += 7;
					if (!(B & 0x80))
					{
						break;
					}
				}
			}
			return (B0 & 0x80) ? -V : V;
		}
		FString Str()
		{
			const int32 Len = Index();
			FString Out;
			if (Len > 0 && Len < 4096)
			{
				for (int32 i = 0; i < Len; ++i)
				{
					const uint8 C = U8();
					if (C)
					{
						Out.AppendChar(TCHAR(C));
					}
				}
			}
			else if (Len < 0 && Len > -4096)
			{
				for (int32 i = 0; i < -Len; ++i)
				{
					const uint16 C = uint16(U8()) | (uint16(U8()) << 8);
					if (C)
					{
						Out.AppendChar(TCHAR(C));
					}
				}
			}
			else if (Len != 0)
			{
				bError = true;
			}
			return Out;
		}
	};

	void Log(const FString& Text) const
	{
		const FString Host = Masters.Num() ? Masters[MasterIndex % Masters.Num()].Host : FString();
		UE_LOG(LogUE2Net, Log, TEXT("UE2Server uplink [%s]: %s"), *Host, *Text);
	}

	void CloseSocket()
	{
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
		InBuffer.Reset();
	}

	void Fail(const FString& Why, double Now, bool bTryNext)
	{
		Log(Why);
		CloseSocket();
		State = bNoReconnect ? EState::Failed : EState::Idle;
		// gentle: 30 s between attempts, the next master of the list after a failure
		NextAttempt = Now + 30.0;
		if (bTryNext && Masters.Num() > 0)
		{
			MasterIndex = (MasterIndex + 1) % Masters.Num();
		}
	}

	void Send(const FWriter& W)
	{
		if (!Socket)
		{
			return;
		}
		TArray<uint8> Frame;
		const int32 N = W.Data.Num();
		for (int32 i = 0; i < 4; ++i)
		{
			Frame.Add(uint8((uint32(N) >> (8 * i)) & 0xFF));
		}
		Frame.Append(W.Data);
		int32 Sent = 0, Total = 0;
		for (int32 Guard = 0; Total < Frame.Num() && Guard < 100; ++Guard)
		{
			if (!Socket->Send(Frame.GetData() + Total, Frame.Num() - Total, Sent))
			{
				break;
			}
			Total += Sent;
		}
	}

	void SendHeartbeat(uint8 Type, int32 Code)
	{
		FSocket* From = nullptr;
		if (Type == 0)
		{
			From = ArenaHost::GetHostQuerySocket();
		}
		else if (Type == 1)
		{
			From = Server->GetSocket();
		}
		if (!From || !MasterAddr.IsValid())
		{
			Log(FString::Printf(TEXT("heartbeat %d requested, no socket for it"), Type));
			return;
		}
		const uint8 Packet[9] = { 0x80, 0, 0, 0, Type, uint8(Code & 0xFF), uint8((Code >> 8) & 0xFF), uint8((Code >> 16) & 0xFF), uint8((uint32(Code) >> 24) & 0xFF) };
		int32 Sent = 0;
		From->SendTo(Packet, 9, Sent, *MasterAddr);
	}

	FString GameTypeName() const
	{
		FString Class = Server->GetGameClass();
		int32 Dot = INDEX_NONE;
		return Class.FindLastChar(TEXT('.'), Dot) ? Class.Mid(Dot + 1) : Class;
	}

	void SendGameState(double Now)
	{
		LastGameState = Now;
		if (++GameStatesSent == 1)
		{
			Log(TEXT("game state sent: the server is listed"));
		}
		UWorld* World = Server->GetWorld();
		const AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
		const AArenaGameState* GS = GM ? GM->GetArenaGameState() : nullptr;
		const FUE2ServerSettings& S = Server->GetSettings();
		FWriter W;
		W.U8(1); // STM_GameState
		// the addresses of the connected clients
		TArray<FString> Clients;
		for (const TUniquePtr<FUE2ServerConnection>& C : Server->GetConnections())
		{
			if (C->IsJoined())
			{
				Clients.Add(C->GetAddrString());
			}
		}
		W.Index(Clients.Num());
		for (const FString& C : Clients)
		{
			W.Str(C);
		}
		int32 NumPlayers = 0;
		TArray<const APlayerState*> Players;
		if (GS)
		{
			for (const APlayerState* PS : GS->PlayerArray)
			{
				if (PS && !PS->IsOnlyASpectator())
				{
					++NumPlayers;
					Players.Add(PS);
				}
			}
		}
		// FServerResponseLine
		W.I32(0);                       // ServerID
		W.Str(FString());               // IP (the master uses the connection's)
		W.I32(S.Port);
		W.I32(S.Port + 1);
		W.Str(S.ServerName);
		W.Str(Server->GetMapName());
		W.Str(GameTypeName());
		W.I32(NumPlayers);
		W.I32(S.MaxPlayers);
		W.I32(0);                       // Ping
		uint32 Flags = (S.Password.IsEmpty() ? 0u : 1u) | (S.bListen ? 8u : 0u) | (GS && GS->bInstagib ? 16u : 0u);
		W.I32(int32(Flags));
		W.Str(FString());               // SkillLevel
		TArray<TPair<FString, FString>> Rules = {
			{ TEXT("ServerMode"), S.bListen ? TEXT("non-dedicated") : TEXT("dedicated") },
			{ TEXT("ServerVersion"), TEXT("3369") },
			{ TEXT("GamePassword"), S.Password.IsEmpty() ? TEXT("False") : TEXT("True") },
			{ TEXT("MaxSpectators"), FString::FromInt(S.MaxSpectators) },
		};
		if (GS)
		{
			Rules.Add({ TEXT("GoalScore"), FString::FromInt(GS->GoalScore) });
			Rules.Add({ TEXT("TimeLimit"), FString::FromInt(GS->TimeLimit / 60) });
		}
		W.Index(Rules.Num());
		for (const TPair<FString, FString>& R : Rules)
		{
			W.Str(R.Key);
			W.Str(R.Value);
		}
		W.Index(Players.Num());
		int32 Num = 0;
		for (const APlayerState* PS : Players)
		{
			W.I32(Num++);
			W.Str(PS->GetPlayerName());
			W.I32(PS->IsABot() ? 0 : int32(PS->GetPingInMilliseconds()));
			W.I32(int32(PS->GetScore()));
			W.I32(0);                   // StatsID
		}
		Send(W);
	}

	FUE2ServerConnection* FindClient(const FString& ClientId) const
	{
		for (const TUniquePtr<FUE2ServerConnection>& C : Server->GetConnections())
		{
			if (C->IsJoined() && C->GetAddrString() == ClientId)
			{
				return C.Get();
			}
		}
		return nullptr;
	}

	void Kick(const FString& ClientId, const FString& Reason)
	{
		if (FUE2ServerConnection* C = FindClient(ClientId))
		{
			Log(FString::Printf(TEXT("the master server removes %s (%s)"), *C->PlayerName, *Reason));
			C->Close(Reason, FString(), Reason);
		}
		else
		{
			FWriter W;
			W.U8(3); // STM_ClientDisconnectFailed
			W.Str(ClientId);
			Send(W);
		}
	}

	void HandleMessage(const TArray<uint8>& Msg, double Now)
	{
		FReader R(Msg);
		switch (State)
		{
		case EState::Challenge:
		{
			const FString Challenge = R.Str();
			Log(TEXT("challenge received, answering (hash and response not logged)"));
			FWriter W;
			W.Str(UE2NetCDKey::ComputeAuthHash());
			W.Str(UE2NetCDKey::ComputeResponse(Challenge));
			W.Str(TEXT("UT2K4SERVER"));
			W.I32(3369);
#if PLATFORM_LINUX
			W.U8(8);
#else
			W.U8(0);
#endif
			W.Str(TEXT("int"));
			W.I32(MatchId);
			W.U8(0); // bIgnoreUTANBans
			Send(W);
			State = EState::Approval;
			break;
		}
		case EState::Approval:
		{
			const FString Reply = R.Str();
			Log(FString::Printf(TEXT("login answer: %s"), *Reply.Left(40)));
			if (Reply.Equals(TEXT("APPROVED"), ESearchCase::IgnoreCase))
			{
				FWriter W;
				W.I32(0); // not behind a NAT (no periodic UDP heartbeats)
				W.I32(0); // no GameSpy query link
				Send(W);
				State = EState::Heartbeats;
				Log(TEXT("approved"));
			}
			else if (Reply.Equals(TEXT("MSUPDATE"), ESearchCase::IgnoreCase))
			{
			}
			else if (Reply.Equals(TEXT("UPGRADE"), ESearchCase::IgnoreCase) || Reply.Equals(TEXT("DENIED"), ESearchCase::IgnoreCase))
			{
				bNoReconnect = true;
				Fail(FString::Printf(TEXT("the master server refused the server (%s)"), *Reply), Now, false);
			}
			else
			{
				Fail(FString::Printf(TEXT("the master server answered %s"), *Reply.Left(40)), Now, true);
			}
			break;
		}
		case EState::Heartbeats:
		{
			const uint8 Flag = R.U8();
			if (Flag == 0)
			{
				const uint8 Type = R.U8();
				const int32 Code = R.I32();
				Log(FString::Printf(TEXT("heartbeat request: type %d (%s)"), Type, Type == 0 ? TEXT("query port") : Type == 1 ? TEXT("game port") : TEXT("GameSpy port")));
				SendHeartbeat(Type, Code);
			}
			else
			{
				const int32 Period = R.I32(); // heartbeat period (only used behind a NAT)
				Log(FString::Printf(TEXT("heartbeats done (period %d s)"), Period));
				State = EState::Ready;
				FWriter W;
				W.U8(4);  // STM_MD5Version
				W.I32(-1);
				Send(W);
				SendGameState(Now);
				Log(TEXT("the server is listed"));
			}
			break;
		}
		case EState::Ready:
		{
			const uint8 Cmd = R.U8();
			switch (Cmd)
			{
			case 0: // MTS_ClientChallenge: the master checks a client's CD key through us
			{
				const FString ClientId = R.Str();
				const FString Challenge = R.Str();
				FUE2ServerConnection* C = FindClient(ClientId);
				FUE2ServerWorld* SW = Server->GetServerWorld();
				if (C && SW)
				{
					Challenges.Add(C->GetId(), { ClientId, Now });
					Server->CallClient(C->GetId(), SW->GetControllerActor(C->GetId()), TEXT("ClientValidate"), { UE2Codec::Str(Challenge) });
				}
				break;
			}
			case 1: Kick(R.Str(), TEXT("authentication failed")); break;
			case 2: Fail(TEXT("the master server closed the uplink"), Now, false); break;
			case 3: MatchId = R.I32(); LastGameState = -100.0; Log(FString::Printf(TEXT("match id %d"), MatchId)); break;
			case 4: break; // MTS_MD5Update: package checksums for client checks we do not make
			case 5: break; // MTS_UpdateOption: a master never changes our settings
			case 6: // MTS_CheckOption: answered with an empty value
			{
				const FString Section = R.Str();
				const FString Key = R.Str();
				FWriter W;
				W.U8(5);
				W.Str(Section);
				W.Str(Key);
				W.Str(FString());
				Send(W);
				break;
			}
			case 7: Kick(R.Str(), TEXT("modified client")); break;
			case 8: { const FString Id = R.Str(); Kick(Id, FString::Printf(TEXT("banned: %s"), *R.Str())); break; }
			case 9: Kick(R.Str(), TEXT("duplicate CD key")); break;
			case 10: Kick(R.Str(), TEXT("banned")); break;
			default: Log(FString::Printf(TEXT("unknown master command %d"), Cmd)); break;
			}
			break;
		}
		default:
			break;
		}
	}

	void Tick(double Now)
	{
		switch (State)
		{
		case EState::Failed:
			return;
		case EState::Idle:
		{
			if (Now < NextAttempt || Masters.Num() == 0)
			{
				return;
			}
			const ArenaServerBrowser::FMasterServer M = Masters[MasterIndex % Masters.Num()];
			State = EState::Resolving;
			StateTime = Now;
			ResolvedIp = MakeShared<FString, ESPMode::ThreadSafe>();
			ResolveState = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
			TSharedPtr<FString, ESPMode::ThreadSafe> Ip = ResolvedIp;
			TSharedPtr<std::atomic<int32>, ESPMode::ThreadSafe> Done = ResolveState;
			const FString Host = M.Host;
			Log(FString::Printf(TEXT("contacting %s:%d"), *M.Host, M.Port));
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Host, Ip, Done]()
			{
				ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
				FAddressInfoResult Res = SS->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, ESocketType::SOCKTYPE_Streaming);
				if (Res.ReturnCode == SE_NO_ERROR && Res.Results.Num() > 0)
				{
					*Ip = Res.Results[0].Address->ToString(false);
					Done->store(1);
				}
				else
				{
					Done->store(2);
				}
			});
			return;
		}
		case EState::Resolving:
		{
			const int32 R = ResolveState.IsValid() ? ResolveState->load() : 2;
			if (R == 0)
			{
				if (Now - StateTime > 15.0)
				{
					Fail(TEXT("master server address lookup timed out"), Now, true);
				}
				return;
			}
			if (R == 2)
			{
				Fail(TEXT("cannot resolve the master server"), Now, true);
				return;
			}
			ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			const ArenaServerBrowser::FMasterServer M = Masters[MasterIndex % Masters.Num()];
			MasterAddr = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			bool bValid = false;
			MasterAddr->SetIp(**ResolvedIp, bValid);
			MasterAddr->SetPort(M.Port);
			Socket = SS->CreateSocket(NAME_Stream, TEXT("UE2ServerUplink"), FNetworkProtocolTypes::IPv4);
			if (!bValid || !Socket)
			{
				Fail(TEXT("cannot create the uplink socket"), Now, true);
				return;
			}
			Socket->SetNonBlocking(true);
			Socket->Connect(*MasterAddr);
			State = EState::Connecting;
			StateTime = Now;
			return;
		}
		case EState::Connecting:
		{
			const ESocketConnectionState CS = Socket ? Socket->GetConnectionState() : SCS_ConnectionError;
			if (CS == SCS_Connected)
			{
				State = EState::Challenge;
				StateTime = Now;
			}
			else if (CS == SCS_ConnectionError || Now - StateTime > 10.0)
			{
				Fail(TEXT("cannot connect to the master server"), Now, true);
			}
			return;
		}
		default:
			break;
		}
		// connected: read frames
		uint8 Buffer[4096];
		for (int32 Guard = 0; Socket && Guard < 64; ++Guard)
		{
			int32 Read = 0;
			if (!Socket->Recv(Buffer, sizeof(Buffer), Read, ESocketReceiveFlags::None))
			{
				if (Socket->GetConnectionState() != SCS_Connected)
				{
					Fail(TEXT("the master server closed the connection"), Now, false);
					return;
				}
				break;
			}
			if (Read <= 0)
			{
				break;
			}
			InBuffer.Append(Buffer, Read);
		}
		while (InBuffer.Num() >= 4 && Socket)
		{
			const uint32 Len = uint32(InBuffer[0]) | (uint32(InBuffer[1]) << 8) | (uint32(InBuffer[2]) << 16) | (uint32(InBuffer[3]) << 24);
			if (Len > 1 << 20)
			{
				Fail(TEXT("bad message from the master server"), Now, true);
				return;
			}
			if (InBuffer.Num() < int32(4 + Len))
			{
				break;
			}
			TArray<uint8> Msg(InBuffer.GetData() + 4, int32(Len));
			InBuffer.RemoveAt(0, int32(4 + Len));
			HandleMessage(Msg, Now);
		}
		if (State == EState::Ready && Now - LastGameState > 60.0)
		{
			SendGameState(Now);
		}
		if (State != EState::Ready && State != EState::Idle && State != EState::Failed && Now - StateTime > 30.0)
		{
			Fail(TEXT("the master server handshake timed out"), Now, true);
		}
		// challenges the client did not answer in 20 s are dropped (the stock server kicks the client)
		for (auto It = Challenges.CreateIterator(); It; ++It)
		{
			if (Now - It.Value().Time > 20.0)
			{
				It.RemoveCurrent();
			}
		}
	}
};

FUE2MasterUplink::FUE2MasterUplink(FUE2Server& InServer)
{
	TArray<ArenaServerBrowser::FMasterServer> Masters = ArenaServerBrowser::GetMasterServers();
#if !UE_BUILD_SHIPPING
	// test hook: -UE2UplinkMaster=<host:port> (a local stand-in master)
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("UE2UplinkMaster="), Override))
	{
		ArenaServerBrowser::FMasterServer M;
		int32 Port = 28902;
		if (ArenaServerBrowser::ParseAddress(Override, M.Host, Port))
		{
			M.Port = Port;
			Masters = { M };
		}
	}
#endif
	// one link per group (UT2004's MasterServerList groups: masters of a group share their lists)
	TMap<int32, TArray<ArenaServerBrowser::FMasterServer>> Groups;
	for (const ArenaServerBrowser::FMasterServer& M : Masters)
	{
		Groups.FindOrAdd(M.Group).Add(M);
	}
	for (TPair<int32, TArray<ArenaServerBrowser::FMasterServer>>& G : Groups)
	{
		TUniquePtr<FImpl> L = MakeUnique<FImpl>();
		L->Server = &InServer;
		L->Masters = MoveTemp(G.Value);
		Links.Add(MoveTemp(L));
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server uplink: on (\"Show in Server Browsers\"), %d master servers in %d groups"), Masters.Num(), Links.Num());
}

FUE2MasterUplink::~FUE2MasterUplink()
{
	for (TUniquePtr<FImpl>& L : Links)
	{
		L->CloseSocket();
	}
}

void FUE2MasterUplink::Tick(double Now)
{
	for (TUniquePtr<FImpl>& L : Links)
	{
		L->Tick(Now);
	}
}

void FUE2MasterUplink::ClientValidationResponse(int32 ConnId, const FString& Response)
{
	FUE2ServerConnection* C = Links.Num() ? Links[0]->Server->FindConnection(ConnId) : nullptr;
	for (TUniquePtr<FImpl>& L : Links)
	{
		FImpl::FChallenge Challenge;
		if (!C || !L->Challenges.RemoveAndCopyValue(ConnId, Challenge) || L->State != FImpl::EState::Ready)
		{
			continue;
		}
		// STM_ClientResponse: the client's id, its AUTH hash and its answer, for the master to check
		FImpl::FWriter W;
		W.U8(0);
		W.Str(Challenge.ClientId);
		W.Str(C->AuthHash);
		W.Str(Response);
		W.Str(FString::ChrN(32, TEXT('0')));
		W.Str(C->PlayerName);
		L->Send(W);
	}
}

void FUE2MasterUplink::ConnectionClosed(int32 ConnId)
{
	for (TUniquePtr<FImpl>& L : Links)
	{
		L->Challenges.Remove(ConnId);
	}
}

#endif // OPENUT2K4_UE2SERVER_BUILD

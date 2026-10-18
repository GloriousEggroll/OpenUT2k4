#include "ArenaServerBrowser.h"
#include "../OpenUT2k4.h"
#include "../ArenaUserSettings.h"

#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include <atomic>

using namespace ArenaServerBrowser;

namespace ArenaServerBrowserLocal
{
	/** Windows-1252 characters of 0x80..0x9F (0 = undefined: kept as the Latin-1 code). */
	const uint16 Cp1252High[32] = {
		0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
		0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

	/**
	 * An 8-bit UE2 string as sent by a server: UE2 colour codes (0x1B R G B, whose bytes may be anything) are removed
	 * first, up to the terminator; then the bytes are UTF-8 when they form valid UTF-8 with at least one multi-byte
	 * sequence (servers whose names / MOTDs were typed in UTF-8), otherwise Windows-1252 (UE2's ANSI code page).
	 */
	FString DecodeBytes(const uint8* Bytes, int32 Num, bool bKeepColours = false)
	{
		struct FCode { int32 BytePos; TCHAR R, G, B; };
		TArray<FCode> Codes;
		TArray<uint8> Clean;
		Clean.Reserve(Num);
		for (int32 i = 0; i < Num; ++i)
		{
			const uint8 C = Bytes[i];
			if (C == 0)
			{
				break;
			}
			if (C == 0x1B)
			{
				if (bKeepColours && i + 3 < Num && Bytes[i + 1] && Bytes[i + 2] && Bytes[i + 3])
				{
					Codes.Add({ Clean.Num(), TCHAR(Bytes[i + 1]), TCHAR(Bytes[i + 2]), TCHAR(Bytes[i + 3]) });
				}
				i += 3;
				continue;
			}
			Clean.Add(C);
		}
		// UTF-8 validation (no overlong forms, no surrogates, at most U+10FFFF)
		bool bValid = true, bMulti = false;
		TArray<uint32> Code;
		TArray<int32> CodeStart;
		for (int32 i = 0; i < Clean.Num() && bValid;)
		{
			const uint8 C = Clean[i];
			if (C < 0x80)
			{
				Code.Add(C);
				CodeStart.Add(i);
				++i;
				continue;
			}
			int32 Extra = 0;
			uint32 V = 0, Min = 0;
			if ((C & 0xE0) == 0xC0) { Extra = 1; V = C & 0x1F; Min = 0x80; }
			else if ((C & 0xF0) == 0xE0) { Extra = 2; V = C & 0x0F; Min = 0x800; }
			else if ((C & 0xF8) == 0xF0) { Extra = 3; V = C & 0x07; Min = 0x10000; }
			else { bValid = false; break; }
			if (i + Extra >= Clean.Num()) { bValid = false; break; }
			for (int32 k = 1; k <= Extra; ++k)
			{
				const uint8 B = Clean[i + k];
				if ((B & 0xC0) != 0x80) { bValid = false; break; }
				V = (V << 6) | (B & 0x3F);
			}
			if (!bValid || V < Min || V > 0x10FFFF || (V >= 0xD800 && V <= 0xDFFF))
			{
				bValid = false;
				break;
			}
			Code.Add(V);
			CodeStart.Add(i);
			bMulti = true;
			i += Extra + 1;
		}
		FString Out;
		Out.Reserve(Clean.Num() + Codes.Num() * 4);
		// colour codes (kept as ESC R G B characters) go in before the character that followed them
		int32 NextCode = 0;
		auto EmitCodesUpTo = [&](int32 BytePos)
		{
			while (NextCode < Codes.Num() && Codes[NextCode].BytePos <= BytePos)
			{
				Out.AppendChar(TCHAR(0x1B));
				Out.AppendChar(Codes[NextCode].R);
				Out.AppendChar(Codes[NextCode].G);
				Out.AppendChar(Codes[NextCode].B);
				++NextCode;
			}
		};
		if (bValid && bMulti)
		{
			for (int32 k = 0; k < Code.Num(); ++k)
			{
				EmitCodesUpTo(CodeStart[k]);
				const uint32 V = Code[k];
				if (V > 0xFFFF && sizeof(TCHAR) == 2)
				{
					// UTF-16 TCHAR: a surrogate pair
					const uint32 U = V - 0x10000;
					Out.AppendChar(TCHAR(0xD800 + (U >> 10)));
					Out.AppendChar(TCHAR(0xDC00 + (U & 0x3FF)));
				}
				else
				{
					Out.AppendChar(TCHAR(V));
				}
			}
			EmitCodesUpTo(MAX_int32);
			return Out;
		}
		for (int32 k = 0; k < Clean.Num(); ++k)
		{
			EmitCodesUpTo(k);
			const uint8 C = Clean[k];
			const uint16 W = (C >= 0x80 && C < 0xA0 && Cp1252High[C - 0x80]) ? Cp1252High[C - 0x80] : uint16(C);
			Out.AppendChar(TCHAR(W));
		}
		EmitCodesUpTo(MAX_int32);
		return Out;
	}

	/** Little-endian reader of UE2 network data (bounds checked: a short packet sets bError and reads zeros). */
	struct FReader
	{
		const uint8* Data = nullptr;
		int32 Num = 0;
		int32 Pos = 0;
		bool bError = false;

		FReader(const uint8* InData, int32 InNum) : Data(InData), Num(InNum) {}
		bool AtEnd() const { return Pos >= Num; }
		uint8 U8()
		{
			if (Pos >= Num) { bError = true; return 0; }
			return Data[Pos++];
		}
		uint16 U16()
		{
			const uint16 A = U8();
			return uint16(A | (uint16(U8()) << 8));
		}
		int32 I32()
		{
			uint32 V = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				V |= uint32(U8()) << (8 * i);
			}
			return int32(V);
		}
		/** UE2 FArchive compact index */
		int32 Index()
		{
			const uint8 B0 = U8();
			int32 V = B0 & 0x3F;
			if (B0 & 0x40)
			{
				int32 Shift = 6;
				for (int32 i = 0; i < 4 && !bError; ++i)
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
		/** UE2 FString: length includes the terminator; negative = UTF-16 characters. */
		FString String(bool bKeepColours = false)
		{
			const int32 Len = Index();
			FString Out;
			if (Len > 0)
			{
				if (Len > Num - Pos) { bError = true; return Out; }
				Out = DecodeBytes(Data + Pos, Len, bKeepColours);
				Pos += Len;
			}
			else if (Len < 0)
			{
				const int32 Chars = -Len;
				if (Chars * 2 > Num - Pos) { bError = true; return Out; }
				for (int32 i = 0; i < Chars; ++i)
				{
					const TCHAR C = TCHAR(Data[Pos + i * 2] | (Data[Pos + i * 2 + 1] << 8));
					if (C == 0) break;
					Out.AppendChar(C);
				}
				Pos += Chars * 2;
			}
			return Out;
		}
	};

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
		/** ANSI FString with its terminator */
		void String(const FString& S)
		{
			Index(S.Len() + 1);
			for (const TCHAR C : S)
			{
				U8(uint8(C <= 0xFF ? C : '?'));
			}
			U8(0);
		}
	};

	FString IpToString(uint32 Ip)
	{
		return FString::Printf(TEXT("%u.%u.%u.%u"), (Ip >> 24) & 0xFF, (Ip >> 16) & 0xFF, (Ip >> 8) & 0xFF, Ip & 0xFF);
	}

	ISocketSubsystem* Sockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	/** Host name or dotted address -> IPv4 dotted address (blocking DNS; worker threads only for names). */
	bool Resolve(const FString& Host, FString& OutIp)
	{
		ISocketSubsystem* SS = Sockets();
		if (!SS || Host.IsEmpty())
		{
			return false;
		}
		bool bDotted = true;
		for (const TCHAR C : Host)
		{
			bDotted &= FChar::IsDigit(C) || C == TEXT('.');
		}
		TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		if (bDotted)
		{
			Addr->SetIp(*Host, bValid);
		}
		if (bValid)
		{
			uint32 Ip = 0;
			Addr->GetIp(Ip);
			OutIp = IpToString(Ip);
			return true;
		}
		const FAddressInfoResult Res = SS->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4);
		for (const FAddressInfoResultData& R : Res.Results)
		{
			if (R.Address->GetProtocolType() == FNetworkProtocolTypes::IPv4)
			{
				uint32 Ip = 0;
				R.Address->GetIp(Ip);
				OutIp = IpToString(Ip);
				return true;
			}
		}
		return false;
	}

	constexpr int32 LANServerPort = 10777;
	constexpr int32 DefaultGamePort = 7777;
	const uint8 QueryHeader[4] = { 0x80, 0x00, 0x00, 0x00 };

	/** Parses a server info reply body (after the 5 byte header) into S. */
	bool ParseInfo(FReader& R, FServer& S)
	{
		R.I32(); // server id
		const FString Ip = R.String();
		const int32 Port = R.I32();
		const int32 QueryPort = R.I32();
		const FString Name = StripColours(R.String(true), true);
		const FString Map = StripColours(R.String());
		const FString GameType = R.String();
		const int32 NumPlayers = R.I32();
		const int32 MaxPlayers = R.I32();
		if (R.bError)
		{
			return false;
		}
		R.I32(); // ping (unused)
		const uint32 Flags = uint32(R.I32());
		const FString Skill = R.String();
		if (Port > 0 && Port < 65536)
		{
			S.Port = Port;
		}
		(void)Ip;
		(void)QueryPort;
		S.Name = Name;
		S.Map = Map;
		S.GameType = GameType;
		S.NumPlayers = FMath::Max(0, NumPlayers);
		S.MaxPlayers = FMath::Max(0, MaxPlayers);
		if (!R.bError)
		{
			S.Flags = Flags;
			S.Skill = Skill;
		}
		S.bHasInfo = true;
		return true;
	}
}

using namespace ArenaServerBrowserLocal;

// =================================================================================================
// Helpers
// =================================================================================================

FString FServer::Address() const
{
	return FString::Printf(TEXT("%s:%d"), Ip.IsEmpty() ? *Host : *Ip, Port);
}

FString ArenaServerBrowser::DecodeBytes(const uint8* Bytes, int32 Num, bool bKeepColours)
{
	return ArenaServerBrowserLocal::DecodeBytes(Bytes, Num, bKeepColours);
}

FString ArenaServerBrowser::StripColours(const FString& In, bool bKeepColours)
{
	FString Out;
	Out.Reserve(In.Len());
	for (int32 i = 0; i < In.Len(); ++i)
	{
		const TCHAR C = In[i];
		if (C == 0x1B)
		{
			if (bKeepColours && i + 3 < In.Len())
			{
				// kept for the text drawing (UT2004 shows coloured names)
				Out.Append(In.Mid(i, 4));
			}
			i += 3; // colour code: ESC R G B
			continue;
		}
		if (C < 0x20)
		{
			continue;
		}
		// non-breaking spaces (common in server names) are not in the bitmap fonts
		Out.AppendChar(C == 0xA0 ? TCHAR(' ') : C);
	}
	return Out.TrimStartAndEnd();
}

bool ArenaServerBrowser::ParseAddress(const FString& In, FString& OutHost, int32& OutPort)
{
	FString S = In.TrimStartAndEnd();
	for (const TCHAR* Prefix : { TEXT("unreal://"), TEXT("ut2004://") })
	{
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			S.RightChopInline(FCString::Strlen(Prefix));
		}
	}
	int32 Slash = INDEX_NONE;
	if (S.FindChar(TEXT('/'), Slash))
	{
		S.LeftInline(Slash);
	}
	OutPort = DefaultGamePort;
	FString Host = S, PortText;
	if (S.Split(TEXT(":"), &Host, &PortText, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		if (!PortText.IsNumeric())
		{
			return false;
		}
		OutPort = FCString::Atoi(*PortText);
	}
	Host.TrimStartAndEndInline();
	if (Host.IsEmpty() || OutPort <= 0 || OutPort > 65534)
	{
		return false;
	}
	for (const TCHAR C : Host)
	{
		if (!(FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('-') || C == TEXT('_')))
		{
			return false;
		}
	}
	OutHost = Host;
	return true;
}

TArray<FMasterServer> ArenaServerBrowser::GetMasterServers()
{
	TArray<FMasterServer> Out;
	if (const UArenaUserSettings* U = UArenaUserSettings::Get())
	{
		for (const FString& Entry : U->MasterServers)
		{
			FString AddressText = Entry, GroupText;
			Entry.Split(TEXT(","), &AddressText, &GroupText);
			FMasterServer M;
			int32 Port = 28902;
			if (ParseAddress(AddressText, M.Host, Port))
			{
				M.Port = AddressText.Contains(TEXT(":")) ? Port : 28902;
				M.Group = GroupText.IsEmpty() ? 1 : FCString::Atoi(*GroupText);
				Out.Add(M);
			}
		}
	}
	if (Out.Num() == 0)
	{
		// [IpDrv.MasterServerLink] MasterServerList of the OldUnreal 3374 patch's Default.ini (Group 1 masters share
		// their lists; OpenSpy keeps its own)
		const TCHAR* Group1[] = { TEXT("ut2004master.333networks.com"), TEXT("ut2004master.gonespy.com"), TEXT("ut2004master.errorist.eu"),
			TEXT("master.frag-net.com"), TEXT("master.noccer.de") };
		for (const TCHAR* H : Group1)
		{
			Out.Add({ H, 28902, 1 });
		}
		Out.Add({ TEXT("utmaster.openspy.net"), 28902, 2 });
	}
	return Out;
}

// =================================================================================================
// Worker jobs: master server list, host name lookups
// =================================================================================================

struct ArenaServerBrowser::FJob
{
	std::atomic<bool> bCancel{ false };
	std::atomic<bool> bDone{ false };
	FCriticalSection Lock;
	FString Status;
	TArray<FServer> Results;

	void SetStatus(const FString& S)
	{
		FScopeLock L(&Lock);
		Status = S;
	}
	FString GetStatus()
	{
		FScopeLock L(&Lock);
		return Status;
	}
};

namespace ArenaServerBrowserLocal
{
	struct FTcp
	{
		FSocket* Socket = nullptr;
		FJob& Job;
		double Deadline = 0.0;
		explicit FTcp(FJob& InJob) : Job(InJob) {}
		~FTcp()
		{
			if (Socket)
			{
				Socket->Close();
				Sockets()->DestroySocket(Socket);
			}
		}
		bool Connect(const FString& Ip, int32 Port, double Timeout)
		{
			ISocketSubsystem* SS = Sockets();
			TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			bool bValid = false;
			Addr->SetIp(*Ip, bValid);
			Addr->SetPort(Port);
			if (!bValid)
			{
				return false;
			}
			Socket = SS->CreateSocket(NAME_Stream, TEXT("ArenaMasterServer"), FNetworkProtocolTypes::IPv4);
			if (!Socket)
			{
				return false;
			}
			Socket->SetNonBlocking(true);
			if (!Socket->Connect(*Addr))
			{
				return false;
			}
			const double End = FPlatformTime::Seconds() + Timeout;
			while (!Job.bCancel && FPlatformTime::Seconds() < End)
			{
				if (Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(200)))
				{
					return Socket->GetConnectionState() == SCS_Connected;
				}
			}
			return false;
		}
		bool ReadBytes(uint8* Out, int32 Num)
		{
			int32 Got = 0;
			while (Got < Num)
			{
				if (Job.bCancel || FPlatformTime::Seconds() > Deadline)
				{
					return false;
				}
				if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
				{
					continue;
				}
				int32 Read = 0;
				if (!Socket->Recv(Out + Got, Num - Got, Read))
				{
					return false; // closed or failed
				}
				Got += Read;
			}
			return true;
		}
		/** One length-prefixed message. */
		bool ReadMessage(TArray<uint8>& Out)
		{
			uint8 Len[4];
			if (!ReadBytes(Len, 4))
			{
				return false;
			}
			const uint32 N = uint32(Len[0]) | (uint32(Len[1]) << 8) | (uint32(Len[2]) << 16) | (uint32(Len[3]) << 24);
			if (N > 64 * 1024)
			{
				return false;
			}
			Out.SetNumUninitialized(int32(N));
			return N == 0 || ReadBytes(Out.GetData(), int32(N));
		}
		bool WriteMessage(const TArray<uint8>& Body)
		{
			TArray<uint8> Data;
			const uint32 N = uint32(Body.Num());
			Data.Add(uint8(N & 0xFF)); Data.Add(uint8((N >> 8) & 0xFF)); Data.Add(uint8((N >> 16) & 0xFF)); Data.Add(uint8((N >> 24) & 0xFF));
			Data.Append(Body);
			int32 Sent = 0;
			while (Sent < Data.Num())
			{
				if (Job.bCancel || FPlatformTime::Seconds() > Deadline)
				{
					return false;
				}
				int32 Now = 0;
				if (!Socket->Send(Data.GetData() + Sent, Data.Num() - Sent, Now))
				{
					if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(200)))
					{
						continue;
					}
					continue;
				}
				Sent += Now;
			}
			return true;
		}
	};

	/** One master server: handshake (no CD key: empty hash and response) and the list query. */
	bool QueryMaster(FJob& Job, const FMasterServer& M, const FString& GameType, TArray<FServer>& Out, FString& OutError)
	{
		FString Ip;
		if (!Resolve(M.Host, Ip))
		{
			OutError = TEXT("unknown host");
			return false;
		}
		FTcp Tcp(Job);
		if (!Tcp.Connect(Ip, M.Port, 5.0))
		{
			OutError = TEXT("no connection");
			return false;
		}
		Tcp.Deadline = FPlatformTime::Seconds() + 8.0;
		TArray<uint8> Msg;
		if (!Tcp.ReadMessage(Msg))
		{
			OutError = TEXT("no challenge");
			return false;
		}
		{
			// the challenge is only needed to answer with a CD key: listing works without one, so none is used
			FWriter W;
			W.String(FString());           // CD key hash
			W.String(FString());           // challenge response
			W.String(TEXT("UT2K4CLIENT"));
			W.I32(3369);                   // engine version
			W.U8(0);                       // platform
			W.String(TEXT("int"));         // language
			W.I32(0);                      // GPU device id
			W.I32(0);                      // GPU vendor id
			W.I32(0);                      // CPU speed
			W.U8(0);                       // CPU type
			if (!Tcp.WriteMessage(W.Data) || !Tcp.ReadMessage(Msg))
			{
				OutError = TEXT("no reply to the login");
				return false;
			}
			FReader R(Msg.GetData(), Msg.Num());
			const FString Reply = R.String();
			if (Reply != TEXT("APPROVED"))
			{
				OutError = FString::Printf(TEXT("login refused (%s)"), *Reply.Left(40));
				return false;
			}
		}
		{
			FWriter W;
			W.String(FString()); // verification
			if (!Tcp.WriteMessage(W.Data) || !Tcp.ReadMessage(Msg))
			{
				OutError = TEXT("no reply to the verification");
				return false;
			}
			FReader R(Msg.GetData(), Msg.Num());
			const FString Reply = R.String();
			if (Reply != TEXT("VERIFIED"))
			{
				OutError = FString::Printf(TEXT("verification refused (%s)"), *Reply.Left(40));
				return false;
			}
		}
		{
			FWriter W;
			W.U8(0); // query
			W.U8(GameType.IsEmpty() ? 0 : 1);
			if (!GameType.IsEmpty())
			{
				W.String(TEXT("gametype"));
				W.String(GameType);
				W.U8(0); // equals
			}
			Tcp.Deadline = FPlatformTime::Seconds() + 15.0;
			if (!Tcp.WriteMessage(W.Data) || !Tcp.ReadMessage(Msg))
			{
				OutError = TEXT("no list");
				return false;
			}
		}
		FReader Count(Msg.GetData(), Msg.Num());
		const int32 Num = Count.I32();
		if (Count.bError || Num < 0 || Num > 20000)
		{
			OutError = TEXT("bad list");
			return false;
		}
		for (int32 i = 0; i < Num; ++i)
		{
			if (!Tcp.ReadMessage(Msg))
			{
				OutError = FString::Printf(TEXT("list cut off after %d of %d servers"), i, Num);
				return Out.Num() > 0;
			}
			FReader R(Msg.GetData(), Msg.Num());
			FServer S;
			const uint8 A = R.U8(), B = R.U8(), C = R.U8(), D = R.U8();
			S.Ip = FString::Printf(TEXT("%u.%u.%u.%u"), A, B, C, D);
			S.Port = R.U16();
			S.QueryPort = R.U16();
			S.Name = StripColours(R.String(true), true);
			S.Map = StripColours(R.String());
			S.GameType = R.String();
			S.NumPlayers = R.U8();
			S.MaxPlayers = R.U8();
			S.Flags = uint32(R.I32());
			S.Skill = R.String();
			if (R.bError && S.Name.IsEmpty())
			{
				continue;
			}
			S.bHasInfo = true;
			if (S.Port > 0 && S.QueryPort > 0)
			{
				Out.Add(MoveTemp(S));
			}
		}
		return true;
	}

	void RunInternetJob(TSharedPtr<FJob, ESPMode::ThreadSafe> Job, TArray<FMasterServer> Masters, FString GameType)
	{
		TArray<int32> Groups;
		for (const FMasterServer& M : Masters)
		{
			Groups.AddUnique(M.Group);
		}
		TArray<FServer> All;
		TSet<FString> Seen;
		TArray<FString> Used, Failed;
		for (const int32 Group : Groups)
		{
			for (const FMasterServer& M : Masters)
			{
				if (M.Group != Group || Job->bCancel)
				{
					continue;
				}
				Job->SetStatus(FString::Printf(TEXT("Contacting the master server %s..."), *M.Host));
				TArray<FServer> List;
				FString Error;
				const double Start = FPlatformTime::Seconds();
				const bool bOk = QueryMaster(*Job, M, GameType, List, Error);
				UE_LOG(LogOpenUT2k4, Log, TEXT("Server browser: master %s:%d (group %d) %s: %d servers in %.1f s%s"), *M.Host, M.Port, M.Group,
					*GameType, List.Num(), FPlatformTime::Seconds() - Start, bOk ? TEXT("") : *(TEXT(", failed: ") + Error));
				for (FServer& S : List)
				{
					const FString Key = S.Address();
					if (!Seen.Contains(Key))
					{
						Seen.Add(Key);
						All.Add(MoveTemp(S));
					}
				}
				if (bOk)
				{
					Used.Add(M.Host);
					break;
				}
				Failed.Add(M.Host);
			}
		}
		{
			FScopeLock L(&Job->Lock);
			Job->Results = MoveTemp(All);
			Job->Status = Used.Num() > 0 ? FString::Printf(TEXT("%d servers from %s"), Job->Results.Num(), *FString::Join(Used, TEXT(", ")))
				: TEXT("No master server answered");
		}
		Job->bDone = true;
	}

	void RunResolveJob(TSharedPtr<FJob, ESPMode::ThreadSafe> Job, TArray<FString> Addresses)
	{
		TArray<FServer> Out;
		for (const FString& A : Addresses)
		{
			if (Job->bCancel)
			{
				break;
			}
			FServer S;
			int32 Port = DefaultGamePort;
			if (!ParseAddress(A, S.Host, Port))
			{
				continue;
			}
			S.Port = Port;
			S.QueryPort = Port + 1;
			S.Name = A;
			if (!Resolve(S.Host, S.Ip))
			{
				S.Ip.Reset();
				S.State = EServerState::NoResponse;
				S.Name = FString::Printf(TEXT("%s (unknown host)"), *A);
			}
			Out.Add(MoveTemp(S));
		}
		{
			FScopeLock L(&Job->Lock);
			Job->Results = MoveTemp(Out);
			Job->Status.Reset();
		}
		Job->bDone = true;
	}
}

// =================================================================================================
// Browser
// =================================================================================================

FBrowser::FBrowser() = default;

FBrowser::~FBrowser()
{
	if (Job)
	{
		Job->bCancel = true;
	}
	CloseSocket();
}

void FBrowser::Reset(EMode NewMode)
{
	if (Job)
	{
		Job->bCancel = true;
		Job.Reset();
	}
	Mode = NewMode;
	Servers.Reset();
	ByQueryAddress.Reset();
	Queue.Reset();
	InFlight.Reset();
	JobStatus.Reset();
	LANSentAt = -1.0;
	DetailsIndex = INDEX_NONE;
	DetailsSentAt = -1.0;
	++Revision;
}

void FBrowser::Stop()
{
	if (Job)
	{
		Job->bCancel = true;
		Job.Reset();
		JobStatus = TEXT("Stopped");
	}
	Queue.Reset();
	InFlight.Reset();
	++Revision;
}

void FBrowser::RefreshInternet(const FString& GameType)
{
	Reset(EMode::Internet);
	Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->SetStatus(TEXT("Contacting the master server..."));
	TSharedPtr<FJob, ESPMode::ThreadSafe> J = Job;
	TArray<FMasterServer> Masters = GetMasterServers();
	Async(EAsyncExecution::Thread, [J, Masters, GameType]() { RunInternetJob(J, Masters, GameType); });
}

void FBrowser::RefreshAddresses(const TArray<FString>& Addresses)
{
	Reset(EMode::Addresses);
	if (Addresses.Num() == 0)
	{
		return;
	}
	Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->SetStatus(TEXT("Looking up the servers..."));
	TSharedPtr<FJob, ESPMode::ThreadSafe> J = Job;
	Async(EAsyncExecution::Thread, [J, Addresses]() { RunResolveJob(J, Addresses); });
}

void FBrowser::RefreshLAN()
{
	Reset(EMode::LAN);
	if (!EnsureSocket())
	{
		JobStatus = TEXT("Could not open a network socket");
		return;
	}
	const uint8 Query[5] = { QueryHeader[0], QueryHeader[1], QueryHeader[2], QueryHeader[3], 0 };
	SendTo(TEXT("255.255.255.255"), LANServerPort, Query, 5);
	SendTo(TEXT("127.0.0.1"), LANServerPort, Query, 5);
	LANSentAt = FPlatformTime::Seconds();
}

bool FBrowser::EnsureSocket()
{
	if (Socket)
	{
		return true;
	}
	ISocketSubsystem* SS = Sockets();
	if (!SS)
	{
		return false;
	}
	Socket = SS->CreateSocket(NAME_DGram, TEXT("ArenaServerBrowser"), FNetworkProtocolTypes::IPv4);
	if (!Socket)
	{
		return false;
	}
	Socket->SetNonBlocking(true);
	Socket->SetBroadcast(true);
	int32 Size = 0;
	Socket->SetReceiveBufferSize(512 * 1024, Size);
	TSharedRef<FInternetAddr> Any = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Any->SetAnyAddress();
	Any->SetPort(0);
	if (!Socket->Bind(*Any))
	{
		CloseSocket();
		return false;
	}
	Scratch = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	return true;
}

void FBrowser::CloseSocket()
{
	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* SS = Sockets())
		{
			SS->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
}

bool FBrowser::SendTo(const FString& Ip, int32 Port, const uint8* Data, int32 Num)
{
	if (!Socket || !Scratch.IsValid())
	{
		return false;
	}
	bool bValid = false;
	Scratch->SetIp(*Ip, bValid);
	Scratch->SetPort(Port);
	int32 Sent = 0;
	return bValid && Socket->SendTo(Data, Num, Sent, *Scratch);
}

void FBrowser::SendQuery(int32 Index, uint8 Type)
{
	if (!Servers.IsValidIndex(Index) || Servers[Index].Ip.IsEmpty())
	{
		return;
	}
	FServer& S = Servers[Index];
	const uint8 Query[5] = { QueryHeader[0], QueryHeader[1], QueryHeader[2], QueryHeader[3], Type };
	SendTo(S.Ip, S.QueryPort, Query, 5);
	if (Type == 0)
	{
		FPending* P = InFlight.FindByPredicate([Index](const FPending& X) { return X.Server == Index && X.Type == 0; });
		if (!P)
		{
			P = &InFlight.AddDefaulted_GetRef();
			P->Server = Index;
			P->Type = 0;
		}
		P->SentAt = FPlatformTime::Seconds();
		++P->Tries;
		if (S.State == EServerState::Waiting)
		{
			S.State = EServerState::Querying;
		}
	}
}

int32 FBrowser::FindByQueryAddress(const FString& Ip, int32 QueryPort) const
{
	const int32* Found = ByQueryAddress.Find(FString::Printf(TEXT("%s:%d"), *Ip, QueryPort));
	return Found ? *Found : INDEX_NONE;
}

void FBrowser::MergeJob()
{
	if (!Job)
	{
		return;
	}
	if (!Job->bDone)
	{
		JobStatus = Job->GetStatus();
		return;
	}
	TArray<FServer> Results;
	{
		FScopeLock L(&Job->Lock);
		Results = MoveTemp(Job->Results);
		JobStatus = Job->Status;
	}
	Job.Reset();
	for (FServer& S : Results)
	{
		const int32 Index = Servers.Add(MoveTemp(S));
		const FServer& Added = Servers[Index];
		if (!Added.Ip.IsEmpty())
		{
			ByQueryAddress.Add(FString::Printf(TEXT("%s:%d"), *Added.Ip, Added.QueryPort), Index);
			Queue.Add(Index);
		}
	}
	++Revision;
}

void FBrowser::Receive()
{
	if (!Socket)
	{
		return;
	}
	ISocketSubsystem* SS = Sockets();
	TSharedRef<FInternetAddr> From = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	uint8 Buffer[8192];
	for (int32 n = 0; n < 512; ++n)
	{
		int32 Read = 0;
		if (!Socket->RecvFrom(Buffer, sizeof(Buffer), Read, *From) || Read <= 0)
		{
			break;
		}
		uint32 Ip = 0;
		From->GetIp(Ip);
		HandlePacket(Buffer, Read, Ip, From->GetPort());
	}
}

void FBrowser::HandlePacket(const uint8* Data, int32 Num, uint32 FromIp, int32 FromPort)
{
	if (Num < 5 || FMemory::Memcmp(Data, QueryHeader, 4) != 0)
	{
		return;
	}
	const uint8 Type = Data[4];
	const double Now = FPlatformTime::Seconds();
	const FString Ip = IpToString(FromIp);
	int32 Index = FindByQueryAddress(Ip, FromPort);
	if (Index == INDEX_NONE)
	{
		if (Mode != EMode::LAN || Type != 0)
		{
			return;
		}
		// a LAN server answering the broadcast
		FServer S;
		S.Ip = Ip;
		S.QueryPort = FromPort;
		S.Port = FromPort - 1;
		Index = Servers.Add(MoveTemp(S));
		ByQueryAddress.Add(FString::Printf(TEXT("%s:%d"), *Ip, FromPort), Index);
	}
	FServer& S = Servers[Index];
	FReader R(Data + 5, Num - 5);
	++Revision;
	if (Type == 0)
	{
		if (!ParseInfo(R, S))
		{
			return;
		}
		const int32 P = InFlight.IndexOfByPredicate([Index](const FPending& X) { return X.Server == Index && X.Type == 0; });
		if (P != INDEX_NONE)
		{
			S.Ping = FMath::Clamp(int32((Now - InFlight[P].SentAt) * 1000.0), 0, 9999);
			InFlight.RemoveAtSwap(P);
		}
		else if (Mode == EMode::LAN && LANSentAt > 0.0 && S.Ping < 0)
		{
			S.Ping = FMath::Clamp(int32((Now - LANSentAt) * 1000.0), 0, 9999);
		}
		S.State = EServerState::Responded;
	}
	else if (Type == 1)
	{
		// rules: the first packet after a request replaces the old rules, later ones (long rule lists) append
		if (Index == DetailsIndex && bRulesFresh)
		{
			S.Rules.Reset();
			bRulesFresh = false;
		}
		while (!R.AtEnd())
		{
			const FString Key = R.String();
			const FString Value = R.String();
			if (R.bError)
			{
				break;
			}
			if (!Key.IsEmpty())
			{
				S.Rules.Emplace(StripColours(Key), StripColours(Value));
			}
		}
		S.DetailsAt = Now;
	}
	else if (Type == 2)
	{
		if (Index == DetailsIndex && bPlayersFresh)
		{
			S.Players.Reset();
			bPlayersFresh = false;
		}
		while (!R.AtEnd())
		{
			FPlayer P;
			P.Id = R.I32();
			P.Name = StripColours(R.String(true), true);
			P.Ping = R.I32();
			P.Score = R.I32();
			const uint32 Stats = uint32(R.I32());
			if (R.bError)
			{
				break;
			}
			// 3369: the team is in the stats id (1 << 29 red, 1 << 30 blue)
			P.Team = (Stats & (1u << 29)) ? 0 : ((Stats & (1u << 30)) ? 1 : -1);
			S.Players.Add(MoveTemp(P));
		}
		S.DetailsAt = Now;
	}
}

void FBrowser::WantDetails(int32 Index)
{
	if (Index != DetailsIndex)
	{
		DetailsIndex = Index;
		DetailsSentAt = -1.0;
	}
}

void FBrowser::Tick()
{
	MergeJob();
	const bool bNeedSocket = Queue.Num() > 0 || InFlight.Num() > 0 || Servers.IsValidIndex(DetailsIndex) || Mode == EMode::LAN;
	if (!bNeedSocket || !EnsureSocket())
	{
		return;
	}
	Receive();
	const double Now = FPlatformTime::Seconds();

	// rate limit: a token bucket of MaxPerSecond (a quarter second of burst), at most MaxInFlight outstanding
	if (TokenTime <= 0.0)
	{
		TokenTime = Now;
	}
	Tokens = FMath::Min(double(MaxPerSecond) / 4.0, Tokens + (Now - TokenTime) * MaxPerSecond);
	TokenTime = Now;

	// timeouts: one retry, then the server counts as not responding
	for (int32 i = InFlight.Num() - 1; i >= 0; --i)
	{
		const FPending P = InFlight[i];
		if (Now - P.SentAt < QueryTimeout)
		{
			continue;
		}
		if (P.Tries < 2)
		{
			if (Tokens >= 1.0)
			{
				Tokens -= 1.0;
				SendQuery(P.Server, 0);
			}
			continue;
		}
		InFlight.RemoveAtSwap(i);
		if (Servers.IsValidIndex(P.Server) && Servers[P.Server].State != EServerState::Responded)
		{
			Servers[P.Server].State = EServerState::NoResponse;
			++Revision;
		}
	}
	while (Queue.Num() > 0 && InFlight.Num() < MaxInFlight && Tokens >= 1.0)
	{
		const int32 Index = Queue[0];
		Queue.RemoveAt(0);
		Tokens -= 1.0;
		SendQuery(Index, 0);
	}

	// details of the selected server: rules and players now and every DetailsInterval
	if (Servers.IsValidIndex(DetailsIndex) && !Servers[DetailsIndex].Ip.IsEmpty())
	{
		if (DetailsSentAt < 0.0 || Now - DetailsSentAt > DetailsInterval)
		{
			if (DetailsSentAt < 0.0)
			{
				// another server selected: also refresh its info (and ping) right away
				if (!InFlight.ContainsByPredicate([this](const FPending& X) { return X.Server == DetailsIndex; }))
				{
					SendQuery(DetailsIndex, 0);
				}
			}
			SendQuery(DetailsIndex, 1);
			SendQuery(DetailsIndex, 2);
			DetailsSentAt = Now;
			bRulesFresh = true;
			bPlayersFresh = true;
		}
		else if (bPlayersFresh && Now - DetailsSentAt > QueryTimeout && Servers[DetailsIndex].State == EServerState::Responded)
		{
			// no players reply: the server is empty
			if (Servers[DetailsIndex].Players.Num() > 0)
			{
				Servers[DetailsIndex].Players.Reset();
				++Revision;
			}
			bPlayersFresh = false;
		}
	}
}

FString FBrowser::GetStatus() const
{
	if (Job)
	{
		return JobStatus;
	}
	int32 Responded = 0;
	for (const FServer& S : Servers)
	{
		Responded += S.State == EServerState::Responded ? 1 : 0;
	}
	const int32 Pending = Queue.Num() + InFlight.Num();
	if (Pending > 0)
	{
		return FString::Printf(TEXT("Pinging servers: %d of %d"), Servers.Num() - Pending, Servers.Num());
	}
	if (Mode == EMode::LAN)
	{
		return Servers.Num() > 0 ? FString::Printf(TEXT("%d LAN server%s"), Servers.Num(), Servers.Num() == 1 ? TEXT("") : TEXT("s"))
			: TEXT("Searching the local network...");
	}
	if (Mode == EMode::Internet && !JobStatus.IsEmpty() && Servers.Num() == 0)
	{
		return JobStatus;
	}
	if (Mode == EMode::None)
	{
		return FString();
	}
	return FString::Printf(TEXT("%d servers, %d answered"), Servers.Num(), Responded);
}

bool FBrowser::IsBusy() const
{
	return Job.IsValid() || Queue.Num() > 0 || InFlight.Num() > 0;
}

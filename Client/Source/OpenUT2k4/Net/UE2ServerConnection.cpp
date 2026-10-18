#include "UE2ServerConnection.h"

#include "UE2Server.h"
#include "UE2NetLog.h"
#include "UE2ServerValidator.h"
#include "../UScript/UScriptCore.h"
#include "IPAddress.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "Sockets.h"

// work in progress: compiled once FUE2Server (UE2Server.cpp) is in
#if OPENUT2K4_UE2SERVER_BUILD

namespace UE2ServerConnectionLocal
{
	/** Engine version we announce: the player's install is 3369 (OldUnreal 3374 clients accept it). */
	constexpr int32 ServerVersion = 3369;
	constexpr int32 MinClientVersion = 3369;
	constexpr double ConnectionTimeout = 20.0;
	/** Clients that never finish the login (HELLO / LOGIN / JOIN) are dropped after this time. */
	constexpr double LoginTimeout = 60.0;
	constexpr double AckTimeout = 1.0;
	constexpr double KeepAliveTime = 0.2;
	constexpr uint8 ChControl = 1;
	constexpr uint8 ChActor = 2;
	constexpr uint8 ChFile = 3;
	constexpr uint32 UE2FuncNetReliable = 0x00000080;
	/** Reliable bunches in flight on one channel before property updates wait (UE2 RELIABLE_BUFFER is 128). */
	constexpr int32 MaxOutstandingReliable = 48;
	/** Salt of UNetConnection::SecureCDKeyHash (the player id the stock server bans by). */
	const TCHAR* CDKeySalt = TEXT("1230asadkgjk358dmvmbjt6838320yjkdhnchjg4958");

	int32 MakeRelative(int32 Value, int32 Reference, int32 Max)
	{
		const int32 Diff = ((Value - Reference + Max / 2) & (Max - 1)) - Max / 2;
		return Reference + Diff;
	}

	int32 ChallengeResponse(int32 Challenge)
	{
		return int32(uint32(Challenge) * 237u) ^ int32(0x93FE92CEu) ^ (Challenge >> 16) ^ int32(uint32(Challenge) << 16);
	}

	bool IsProperty(const FUSField* F)
	{
		return F && F->Class && F->Class->Name.ToString().EndsWith(TEXT("Property"));
	}

	bool IsFunction(const FUSField* F)
	{
		return F && F->Class && F->Class->Name == TEXT("Function");
	}

	FString CleanText(const FString& Text)
	{
		FString Out = Text.Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT(""));
		return Out.Len() > 200 ? Out.Left(200) + TEXT("...") : Out;
	}

	/** Login URL options with the password masked (logs). */
	FString MaskPassword(const FString& Url)
	{
		const int32 At = Url.Find(TEXT("?Password="), ESearchCase::IgnoreCase);
		if (At == INDEX_NONE)
		{
			return Url;
		}
		const int32 End = Url.Find(TEXT("?"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1);
		return Url.Left(At) + TEXT("?Password=***") + (End != INDEX_NONE ? Url.Mid(End) : FString());
	}
}

using namespace UE2ServerConnectionLocal;

FUE2ServerConnection::FUE2ServerConnection(FUE2Server& InServer, const TSharedRef<FInternetAddr>& InAddr, int32 InId)
	: Server(InServer)
	, Addr(InAddr)
	, Id(InId)
{
	CreateTime = Server.GetTime();
	LastReceiveTime = CreateTime;
	LastSendTime = CreateTime;
}

FUE2ServerConnection::~FUE2ServerConnection() = default;

FString FUE2ServerConnection::GetAddrString() const
{
	return Addr->ToString(true);
}

// =================================================================================================
// Packets
// =================================================================================================

void FUE2ServerConnection::ReceivedRawPacket(const uint8* Data, int32 Count, double Now)
{
	if (State == EState::Closed || Count <= 0)
	{
		return;
	}
	LastReceiveTime = Now;
	++PacketsIn;
	BytesIn += Count;
	uint8 Last = Data[Count - 1];
	if (Last == 0)
	{
		++BunchErrors;
		return;
	}
	int64 NumBits = int64(Count) * 8 - 1;
	while (!(Last & 0x80))
	{
		Last <<= 1;
		--NumBits;
	}
	FUE2BitReader R(Data, NumBits);
	const int32 RawId = int32(R.ReadInt(MaxPacketId));
	const int32 PacketId = MakeRelative(RawId, InPacketId, MaxPacketId);
	if (R.IsError())
	{
		++BunchErrors;
		return;
	}
	if (PacketId > InPacketId)
	{
		InPacketId = PacketId;
	}
	PendingAcks.Add(RawId);
	while (!R.AtEnd() && State != EState::Closed)
	{
		const bool bAck = R.ReadBit();
		if (R.IsError())
		{
			break;
		}
		if (bAck)
		{
			const int32 AckId = MakeRelative(int32(R.ReadInt(MaxPacketId)), OutAckPacketId, MaxPacketId);
			if (R.IsError())
			{
				break;
			}
			if (AckId > OutAckPacketId)
			{
				for (int32 Nak = OutAckPacketId + 1; Nak < AckId; ++Nak)
				{
					ReceivedNak(Nak);
				}
				OutAckPacketId = AckId;
			}
			ReceivedAck(AckId);
			continue;
		}
		FInBunch Bunch;
		const bool bControl = R.ReadBit();
		Bunch.bOpen = bControl ? R.ReadBit() : false;
		Bunch.bClose = bControl ? R.ReadBit() : false;
		Bunch.bReliable = R.ReadBit();
		Bunch.Channel = int32(R.ReadInt(MaxChannels));
		Bunch.Sequence = Bunch.bReliable ? MakeRelative(int32(R.ReadInt(MaxChSequence)), InReliable[Bunch.Channel], MaxChSequence) : 0;
		Bunch.ChType = (Bunch.bReliable || Bunch.bOpen) ? uint8(R.ReadInt(MaxChType)) : 0;
		const int32 DataBits = int32(R.ReadInt(MaxPacketBytes * 8));
		Bunch.Data = R.ReadBits(DataBits);
		if (R.IsError())
		{
			++BunchErrors;
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: malformed packet from %s"), *GetAddrString());
			break;
		}
		ReceivedBunch(Bunch);
	}
	if (State == EState::Hello && !Channels.Contains(0))
	{
		// not a client opening its control channel (e.g. a late ack from a client we just refused): drop it silently
		State = EState::Closed;
		PendingAcks.Reset();
	}
}

void FUE2ServerConnection::ReceivedAck(int32 AckId)
{
	// round trip: the server's view of the client's ping (UE2 clients report theirs only after position corrections)
	const double Sent = PacketSendTimes[AckId & 255];
	if (Sent > 0.0)
	{
		const double Rtt = FMath::Clamp(Server.GetTime() - Sent, 0.0, 2.0);
		SmoothedRtt = SmoothedRtt < 0.0 ? Rtt : SmoothedRtt * 0.9 + Rtt * 0.1;
		PingMs = FMath::RoundToInt(SmoothedRtt * 1000.0);
		PacketSendTimes[AckId & 255] = 0.0;
	}
	for (int32 i = OutReliableBunches.Num() - 1; i >= 0; --i)
	{
		FOutBunch& B = OutReliableBunches[i];
		if (B.PacketId != AckId)
		{
			continue;
		}
		if (const int32* ActorId = ActorByChannel.Find(B.Channel))
		{
			if (FActorChannel* AC = ChannelsByActor.Find(*ActorId))
			{
				AC->OutstandingReliable = FMath::Max(0, AC->OutstandingReliable - 1);
				if (B.bOpen)
				{
					AC->bOpenAcked = true;
				}
			}
		}
		OutReliableBunches.RemoveAt(i);
	}
	UnreliableByPacket.Remove(AckId);
}

void FUE2ServerConnection::ReceivedNak(int32 NakId)
{
	for (int32 i = OutReliableBunches.Num() - 1; i >= 0; --i)
	{
		if (OutReliableBunches[i].PacketId == NakId)
		{
			SendQueue.Insert(MoveTemp(OutReliableBunches[i]), 0);
			OutReliableBunches.RemoveAt(i);
		}
	}
	// lost unreliable property values: forget that the client has them, so they go out again while they still differ
	TArray<TPair<int32, TArray<FString>>> Lost;
	if (UnreliableByPacket.RemoveAndCopyValue(NakId, Lost))
	{
		for (const TPair<int32, TArray<FString>>& L : Lost)
		{
			if (FActorChannel* AC = ChannelsByActor.Find(L.Key))
			{
				for (const FString& Key : L.Value)
				{
					AC->Shadow.Remove(Key);
				}
			}
		}
	}
}

void FUE2ServerConnection::QueueBunch(FOutBunch&& Bunch)
{
	QueuedBytes += 3 + (Bunch.Data.GetNumBits() + 7) / 8;
	if (Bunch.bReliable)
	{
		Bunch.Sequence = ++OutReliable[Bunch.Channel];
	}
	SendQueue.Add(MoveTemp(Bunch));
}

void FUE2ServerConnection::WriteBunchHeader(FUE2BitWriter& W, const FOutBunch& B) const
{
	W.WriteBit(false);
	const bool bControl = B.bOpen || B.bClose;
	W.WriteBit(bControl);
	if (bControl)
	{
		W.WriteBit(B.bOpen);
		W.WriteBit(B.bClose);
	}
	W.WriteBit(B.bReliable);
	W.WriteInt(uint32(B.Channel), MaxChannels);
	if (B.bReliable)
	{
		W.WriteInt(uint32(B.Sequence) & (MaxChSequence - 1), MaxChSequence);
	}
	if (B.bReliable || B.bOpen)
	{
		W.WriteInt(B.ChType, MaxChType);
	}
	W.WriteInt(uint32(B.Data.GetNumBits()), MaxPacketBytes * 8);
}

void FUE2ServerConnection::Tick(double Now)
{
	if (State == EState::Closed)
	{
		return;
	}
	if (Now - LastReceiveTime > ConnectionTimeout)
	{
		SetClosed(TEXT("connection timed out"));
		return;
	}
	if (State != EState::Joined && Now - CreateTime > LoginTimeout)
	{
		Close(TEXT("login timed out"));
		return;
	}
	for (int32 i = OutReliableBunches.Num() - 1; i >= 0; --i)
	{
		if (Now - OutReliableBunches[i].SentTime > AckTimeout)
		{
			SendQueue.Insert(MoveTemp(OutReliableBunches[i]), 0);
			OutReliableBunches.RemoveAt(i);
		}
	}
	for (auto It = RecentlyClosed.CreateIterator(); It; ++It)
	{
		if (Now - It.Value() > 3.0)
		{
			It.RemoveCurrent();
		}
	}
	if (SendQueue.Num() > 0 || PendingAcks.Num() > 0 || Now - LastSendTime > KeepAliveTime)
	{
		Flush(Now);
	}
}

void FUE2ServerConnection::Flush(double Now)
{
	if (State == EState::Closed || !Server.GetSocket())
	{
		SendQueue.Reset();
		PendingAcks.Reset();
		return;
	}
	constexpr int64 MaxBits = MaxPacketBytes * 8 - 1;
	TArray<FOutBunch> Queue = MoveTemp(SendQueue);
	SendQueue.Reset();
	int32 Next = 0;
	bool bFirst = true;
	while (bFirst || Next < Queue.Num() || PendingAcks.Num() > 0)
	{
		FUE2BitWriter Packet;
		const int32 ThisPacketId = OutPacketId;
		Packet.WriteInt(uint32(ThisPacketId) & (MaxPacketId - 1), MaxPacketId);
		int32 AcksWritten = 0;
		for (; AcksWritten < PendingAcks.Num(); ++AcksWritten)
		{
			if (Packet.GetNumBits() > MaxBits - 64)
			{
				break;
			}
			Packet.WriteBit(true);
			Packet.WriteInt(uint32(PendingAcks[AcksWritten]), MaxPacketId);
		}
		PendingAcks.RemoveAt(0, AcksWritten);
		bFirst = false;
		int32 InPacket = 0;
		TArray<TPair<int32, TArray<FString>>> Unreliable;
		while (Next < Queue.Num())
		{
			FOutBunch& B = Queue[Next];
			FUE2BitWriter Header;
			WriteBunchHeader(Header, B);
			if (Packet.GetNumBits() + Header.GetNumBits() + B.Data.GetNumBits() > MaxBits)
			{
				if (InPacket > 0 || AcksWritten > 0)
				{
					break;
				}
				// a bunch that cannot fit in any packet (should not happen: callers split them)
				UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: dropping an oversized bunch (%lld bits) on channel %d"), B.Data.GetNumBits(), B.Channel);
				++Next;
				continue;
			}
			Packet.WriteBits(Header);
			Packet.WriteBits(B.Data);
			++InPacket;
			++Next;
			if (B.bReliable)
			{
				B.PacketId = ThisPacketId;
				B.SentTime = Now;
				OutReliableBunches.Add(MoveTemp(B));
			}
			else if (B.UnreliableKeys.Num() > 0)
			{
				Unreliable.Emplace(B.ActorId, MoveTemp(B.UnreliableKeys));
			}
		}
		if (Unreliable.Num() > 0)
		{
			UnreliableByPacket.Add(ThisPacketId, MoveTemp(Unreliable));
		}
		SendPacket(Packet);
		LastSendTime = Now;
	}
}

void FUE2ServerConnection::SendPacket(const FUE2BitWriter& Packet)
{
	const TArray<uint8> Bytes = Packet.GetPacketBytes();
	int32 Sent = 0;
	Server.GetSocket()->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *Addr);
	PacketSendTimes[OutPacketId & 255] = Server.GetTime();
	++OutPacketId;
	++PacketsOut;
	BytesOut += Bytes.Num();
}

// =================================================================================================
// Bunches
// =================================================================================================

void FUE2ServerConnection::ReceivedBunch(FInBunch& Bunch)
{
	const int32 Ch = Bunch.Channel;
	if (Bunch.bReliable && Bunch.Sequence <= InReliable[Ch])
	{
		return;
	}
	FChannelInfo* Channel = Channels.Find(Ch);
	if (!Channel)
	{
		if (!Bunch.bOpen && !Bunch.bReliable)
		{
			return;
		}
		if (Bunch.bOpen && Bunch.ChType != ChControl && Bunch.ChType != ChFile)
		{
			// clients only open the control channel and file channels
			if (Bunch.ChType != ChActor || !ActorByChannel.Contains(Ch))
			{
				InReliable[Ch] = FMath::Max(InReliable[Ch], Bunch.Sequence);
				return;
			}
		}
		if (!Bunch.bOpen && !ActorByChannel.Contains(Ch))
		{
			// a reliable bunch for a channel we do not have (closed meanwhile): consume its sequence
			if (Bunch.Sequence == InReliable[Ch] + 1)
			{
				InReliable[Ch] = Bunch.Sequence;
			}
			return;
		}
		Channel = &Channels.Add(Ch);
		Channel->ChType = Bunch.bOpen ? Bunch.ChType : ChActor;
	}
	if (!Bunch.bReliable)
	{
		ProcessBunch(Bunch);
		return;
	}
	if (Bunch.Sequence != InReliable[Ch] + 1)
	{
		int32 At = 0;
		while (At < Channel->Queued.Num() && Channel->Queued[At].Sequence < Bunch.Sequence)
		{
			++At;
		}
		if (At >= Channel->Queued.Num() || Channel->Queued[At].Sequence != Bunch.Sequence)
		{
			Channel->Queued.Insert(MoveTemp(Bunch), At);
		}
		return;
	}
	TArray<FInBunch> Ready;
	Ready.Add(MoveTemp(Bunch));
	InReliable[Ch] = Ready[0].Sequence;
	while (Channel->Queued.Num() > 0 && Channel->Queued[0].Sequence == InReliable[Ch] + 1)
	{
		InReliable[Ch] = Channel->Queued[0].Sequence;
		Ready.Add(MoveTemp(Channel->Queued[0]));
		Channel->Queued.RemoveAt(0);
	}
	for (FInBunch& B : Ready)
	{
		if (State == EState::Closed)
		{
			break;
		}
		if (!Channels.Contains(Ch))
		{
			if (!B.bOpen)
			{
				continue;
			}
			Channels.Add(Ch).ChType = B.ChType;
		}
		ProcessBunch(B);
	}
}

void FUE2ServerConnection::ProcessBunch(FInBunch& Bunch)
{
	FChannelInfo* Channel = Channels.Find(Bunch.Channel);
	if (!Channel)
	{
		return;
	}
	const uint8 Type = Channel->ChType;
	if (Type == ChControl)
	{
		while (!Bunch.Data.AtEnd() && State != EState::Closed)
		{
			const FString Text = Bunch.Data.ReadString();
			if (Bunch.Data.IsError())
			{
				++BunchErrors;
				break;
			}
			ReceivedText(Text);
		}
		if (Bunch.bClose && State != EState::Closed)
		{
			SetClosed(TEXT("the client disconnected"));
		}
		return;
	}
	if (Type == ChFile)
	{
		// package downloads are not offered (every package is the player's own install): refuse with an empty close
		if (Bunch.bOpen)
		{
			FOutBunch B;
			B.Channel = Bunch.Channel;
			B.ChType = ChFile;
			B.bClose = true;
			B.bReliable = true;
			QueueBunch(MoveTemp(B));
			UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s asked for a package download: refused (only stock packages are used)"), *PlayerName);
		}
		Channels.Remove(Bunch.Channel);
		return;
	}
	if (Type == ChActor)
	{
		ReceivedActorBunch(Bunch);
		if (Bunch.bClose)
		{
			// the client closed an actor channel (UE2 clients close channels of actors they destroyed): stop using it
			if (const int32* ActorId = ActorByChannel.Find(Bunch.Channel))
			{
				const int32 A = *ActorId;
				ChannelsByActor.Remove(A);
				ActorByChannel.Remove(Bunch.Channel);
				RecentlyClosed.Add(Bunch.Channel, Server.GetTime());
			}
			Channels.Remove(Bunch.Channel);
		}
	}
}

// =================================================================================================
// Control channel: the login handshake (server side of UNetPendingLevel / ULevel::NotifyReceivedText)
// =================================================================================================

void FUE2ServerConnection::SendControl(const FString& Text)
{
	FOutBunch B;
	B.Channel = 0;
	B.ChType = ChControl;
	B.bReliable = true;
	B.Data.WriteString(Text);
	if (Server.bTrace)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] control >> %s"), Id, *CleanText(Text));
	}
	QueueBunch(MoveTemp(B));
}

void FUE2ServerConnection::ReceivedText(const FString& Text)
{
	const TCHAR* Str = *Text;
	if (Server.bTrace)
	{
		FString Shown = CleanText(MaskPassword(Text));
		if (Shown.StartsWith(TEXT("AUTH ")))
		{
			Shown = TEXT("AUTH HASH=*** GM=*** USERNAME=*** PASSWORD=***");
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] control << %s"), Id, *Shown);
	}
	if (FParse::Command(&Str, TEXT("HELLO")))
	{
		int32 MinVer = 0, Ver = 0;
		FParse::Value(Str, TEXT("MINVER="), MinVer);
		FParse::Value(Str, TEXT("VER="), Ver);
		ClientVersion = Ver;
		if (Ver < MinClientVersion || MinVer > 3374)
		{
			SendControl(FString::Printf(TEXT("UPGRADE MINVER=%d VER=%d"), MinClientVersion, ServerVersion));
			Flush(Server.GetTime());
			SetClosed(FString::Printf(TEXT("client version %d is not supported"), Ver));
			return;
		}
		if (State == EState::Hello)
		{
			Challenge = Server.NextChallenge();
			State = EState::Challenged;
		}
		// the 3369 / 3374 server's format; STATS=0 (no stats tracking), SEC=1, GZ: a number the clients do not answer
		SendControl(FString::Printf(TEXT("CHALLENGE VER=%d CHALLENGE=%d STATS=0 SEC=1 GZ=%d"), FMath::Min(Ver, ServerVersion), Challenge, Server.Random.RandRange(10, 99)));
	}
	else if (FParse::Command(&Str, TEXT("NETSPEED")))
	{
		const int32 Rate = FCString::Atoi(Str);
		NetSpeed = FMath::Clamp(Rate, 1800, Server.GetSettings().bLAN ? 1000000 : 25000);
	}
	else if (FParse::Command(&Str, TEXT("AUTH")))
	{
		// the client's identity: HASH = MD5 of its CD key (or the key OldUnreal clients make themselves). The stock server
		// salts it (UNetConnection::SecureCDKeyHash) into the player id that kick / ban lists use; kept, never logged.
		FString Hash;
		FParse::Value(Str, TEXT("HASH="), Hash);
		if (!Hash.IsEmpty())
		{
			PlayerHashId = FMD5::HashAnsiString(*(FString(CDKeySalt) + Hash));
			AuthHash = Hash.Left(64);
		}
		Hash.Empty();
	}
	else if (FParse::Command(&Str, TEXT("LOGIN")))
	{
		if (State != EState::Challenged)
		{
			return;
		}
		int32 Response = 0;
		FParse::Value(Str, TEXT("RESPONSE="), Response);
		FString Url;
		const TCHAR* UrlAt = FCString::Strifind(Str, TEXT("URL="));
		if (UrlAt)
		{
			Url = FString(UrlAt + 4).TrimStartAndEnd();
		}
		if (Response != ChallengeResponse(Challenge))
		{
			SendControl(TEXT("FAILCODE CHALLENGE"));
			SendControl(TEXT("FAILURE CHALLENGE"));
			Flush(Server.GetTime());
			SetClosed(TEXT("failed the login challenge"));
			return;
		}
		const int32 Q = Url.Find(TEXT("?"));
		LoginOptions = Q != INDEX_NONE ? Url.Mid(Q) : FString();
		UE_LOG(LogUE2Net, Log, TEXT("UE2Server: login request from %s: %s"), *GetAddrString(), *MaskPassword(LoginOptions));
		FString FailCode, FailText;
		Server.ConnectionLogin(*this, Url, FailCode, FailText);
		if (!FailCode.IsEmpty())
		{
			Close(FString::Printf(TEXT("login refused (%s)"), *FailCode), FailCode, FailText);
			return;
		}
		// the package list, then where to load from, then the level (UNetPendingLevel waits for WELCOME)
		for (const FString& Line : Server.GetUsesLines())
		{
			SendControl(Line);
			PackageMap.AddUses(Line.Mid(5));
		}
		SendControl(TEXT("DLMGR CLASS=Engine.ChannelDownload PARAMS=Enabled COMPRESSION=0"));
		SendControl(FString::Printf(TEXT("WELCOME LEVEL=%s GAME=%s LONE=0"), *Server.GetMapName(), *Server.GetGameClass()));
		State = EState::Welcomed;
		WelcomeTime = Server.GetTime();
	}
	else if (FParse::Command(&Str, TEXT("HAVE")))
	{
		FString GuidText;
		int32 Gen = 0;
		FGuid Guid;
		FParse::Value(Str, TEXT("GUID="), GuidText);
		FParse::Value(Str, TEXT("GEN="), Gen);
		if (UE2GuidFromString(GuidText, Guid))
		{
			PackageMap.SetRemoteGeneration(Guid, Gen);
		}
	}
	else if (FParse::Command(&Str, TEXT("SKIP")))
	{
		FString GuidText;
		FGuid Guid;
		FParse::Value(Str, TEXT("GUID="), GuidText);
		const int32 I = UE2GuidFromString(GuidText, Guid) ? PackageMap.FindPackageByGuid(Guid) : INDEX_NONE;
		if (I != INDEX_NONE)
		{
			SkippedPackages.AddUnique(PackageMap.GetList()[I].Name);
		}
	}
	else if (FParse::Command(&Str, TEXT("JOIN")))
	{
		if (State != EState::Welcomed)
		{
			return;
		}
		// this connection's numbering: our packages cut to the generations both sides have, without the skipped ones
		PackageMap.MatchLocalPackages();
		PackageMap.RemovePackages(SkippedPackages);
		PackageMap.Compute();
		MasterToLocal.Reset();
		for (const FUE2PackageInfo& Info : Server.GetMasterMap().GetList())
		{
			MasterToLocal.Add(PackageMap.FindPackage(Info.Name));
		}
		if (SkippedPackages.Num() > 0)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: %s skipped %d packages: %s"), *PlayerName, SkippedPackages.Num(), *FString::Join(SkippedPackages, TEXT(", ")));
		}
		State = EState::Joined;
		JoinTime = Server.GetTime();
		UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s joined (%s, %d packages, %d objects, %d names)"), *PlayerName, *GetAddrString(), PackageMap.GetList().Num(),
			PackageMap.GetMaxObjectIndex(), PackageMap.GetMaxNameIndex());
		Server.ConnectionJoined(*this);
	}
	else if (FParse::Command(&Str, TEXT("OUTVALIDATE")))
	{
		// an OpenUT2k4 client's answer to the validator's challenge (Net/UE2ServerValidator.h)
		UE2ServerValidator::ReceivedControl(*this, Str);
	}
	else
	{
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2Server: unhandled control message from %s: %s"), *GetAddrString(), *CleanText(Text));
	}
}

void FUE2ServerConnection::Close(const FString& Reason, const FString& FailCode, const FString& FailText)
{
	if (State == EState::Closed)
	{
		return;
	}
	if (!FailCode.IsEmpty())
	{
		SendControl(FString::Printf(TEXT("FAILCODE %s"), *FailCode));
	}
	if (!FailText.IsEmpty())
	{
		SendControl(FString::Printf(TEXT("FAILURE %s"), *FailText));
	}
	FOutBunch B;
	B.Channel = 0;
	B.ChType = ChControl;
	B.bClose = true;
	B.bReliable = true;
	QueueBunch(MoveTemp(B));
	const double Now = Server.GetTime();
	Flush(Now);
	// once more in case the first packet is lost
	for (const FOutBunch& Out : OutReliableBunches)
	{
		if (Out.Channel == 0)
		{
			FOutBunch Again;
			Again.Channel = Out.Channel;
			Again.ChType = Out.ChType;
			Again.bOpen = Out.bOpen;
			Again.bClose = Out.bClose;
			Again.bReliable = true;
			Again.Sequence = Out.Sequence;
			Again.Data = Out.Data;
			SendQueue.Add(MoveTemp(Again));
		}
	}
	OutReliableBunches.Reset();
	Flush(Now);
	SetClosed(Reason);
}

void FUE2ServerConnection::SetClosed(const FString& Reason)
{
	if (State == EState::Closed)
	{
		return;
	}
	const bool bWasJoined = State == EState::Joined;
	State = EState::Closed;
	CloseReason = Reason;
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: connection %d (%s, %s) closed%s%s (packets in %d out %d, bytes in %lld out %lld, actors %d, properties %d, calls sent %d received %d, errors %d)"),
		Id, *PlayerName, *GetAddrString(), Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Reason, PacketsIn, PacketsOut, BytesIn, BytesOut, ActorsOpened,
		PropertiesSent, CallsSent, CallsReceived, BunchErrors);
	if (bWasJoined)
	{
		Server.ConnectionClosed(*this, Reason);
	}
}

// =================================================================================================
// Actor channels
// =================================================================================================

FUE2WireContext FUE2ServerConnection::MakeContext()
{
	FUE2WireContext Ctx;
	Ctx.Map = &PackageMap;
	Ctx.ActorToChannel = [this](int32 ActorId) { return ChannelOfActor(ActorId); };
	Ctx.PackageToMap = [this](int32 MasterIndex) { return MasterToLocal.IsValidIndex(MasterIndex) ? MasterToLocal[MasterIndex] : INDEX_NONE; };
	Ctx.ChannelToActor = [this](int32 Channel) { return ActorOfChannel(Channel); };
	return Ctx;
}

FUE2ServerConnection::FActorChannel* FUE2ServerConnection::FindChannelOfActor(int32 ActorId)
{
	return ChannelsByActor.Find(ActorId);
}

int32 FUE2ServerConnection::ActorOfChannel(int32 Channel) const
{
	const int32* A = ActorByChannel.Find(Channel);
	return A ? *A : 0;
}

int32 FUE2ServerConnection::ChannelOfActor(int32 ActorId) const
{
	const FActorChannel* AC = ChannelsByActor.Find(ActorId);
	return (AC && !AC->bClosing && !AC->bTemporary) ? AC->Channel : 0;
}

int32 FUE2ServerConnection::AllocateChannel()
{
	for (int32 Tries = 0; Tries < int32(MaxChannels) - 1; ++Tries)
	{
		const int32 Candidate = 1 + (NextChannel - 1 + Tries) % (int32(MaxChannels) - 1);
		if (!Channels.Contains(Candidate) && !ActorByChannel.Contains(Candidate) && !RecentlyClosed.Contains(Candidate))
		{
			NextChannel = Candidate + 1;
			return Candidate;
		}
	}
	return 0;
}

bool FUE2ServerConnection::EncodeProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue* Value)
{
	return UE2Codec::WriteProperty(W, Prop, Value, MakeContext());
}

bool FUE2ServerConnection::ReplicateActor(FUE2SvActor& Actor, double Now, bool bOwner)
{
	const FUE2ClassNetCache* NetCache = State == EState::Joined ? PackageMap.GetClassNetCache(Actor.Class) : nullptr;
	if (!NetCache)
	{
		return false;
	}
	if (FinishedActors.Contains(Actor.Id))
	{
		return true;
	}
	FActorChannel* AC = ChannelsByActor.Find(Actor.Id);
	const bool bOpening = AC == nullptr;
	if (AC && (AC->bClosing || AC->bTornOff || AC->bTemporary))
	{
		return true;
	}
	if (AC)
	{
		AC->LastRelevant = Now;
		if (Now - AC->LastUpdate < Actor.UpdateInterval || AC->OutstandingReliable >= MaxOutstandingReliable)
		{
			return true;
		}
	}
	else
	{
		const int32 Ch = AllocateChannel();
		if (!Ch)
		{
			return false;
		}
		AC = &ChannelsByActor.Add(Actor.Id);
		AC->Channel = Ch;
		AC->ActorId = Actor.Id;
		AC->LastRelevant = Now;
		ActorByChannel.Add(Ch, Actor.Id);
		Channels.Add(Ch).ChType = ChActor;
		++ActorsOpened;
		// the shadow starts from the class defaults, as the client spawns the actor with them (Role / RemoteRole: the
		// client's view, i.e. the server's values exchanged)
		for (const TPair<FString, FUE2SvProp>& P : Actor.Props)
		{
			int32 Element = 0, RepIndex = INDEX_NONE;
			FUSProperty* Prop = Server.ResolveKey(Actor.Class, P.Key, Element, RepIndex);
			FUE2NetValue Default;
			if (!Prop)
			{
				continue;
			}
			FUSProperty* DefaultProp = Prop;
			if (P.Key == TEXT("Role") || P.Key == TEXT("RemoteRole"))
			{
				int32 E2 = 0, R2 = 0;
				DefaultProp = Server.ResolveKey(Actor.Class, P.Key == TEXT("Role") ? TEXT("RemoteRole") : TEXT("Role"), E2, R2);
			}
			if (DefaultProp && Server.GetDefaultValue(Actor.Class, DefaultProp, Element, Default))
			{
				FShadow S;
				FUE2BitWriter W;
				if (EncodeProperty(W, Prop, &Default))
				{
					S.Bits = W.GetData();
					S.NumBits = int32(W.GetNumBits());
					AC->Shadow.Add(P.Key, MoveTemp(S));
				}
			}
		}
		if (Server.bTrace)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] ch %d open %s (actor %d)"), Id, Ch, *Actor.ClassPath, Actor.Id);
		}
	}
	AC->LastUpdate = Now;

	// changed properties, in replication index order (the order the client reads them in does not matter, but UE2
	// sends them this way)
	struct FItem { const FString* Key; FUSProperty* Prop; int32 Element; int32 RepIndex; const FUE2NetValue* Value; bool bUnreliable; };
	TArray<FItem> Items;
	for (const TPair<FString, FUE2SvProp>& P : Actor.Props)
	{
		const FUE2SvProp& V = P.Value;
		if ((V.Cond == FUE2SvProp::OwnerOnly && !bOwner) || (V.Cond == FUE2SvProp::NotOwner && bOwner) || (V.Cond == FUE2SvProp::InitialOnly && !bOpening))
		{
			continue;
		}
		if (V.bUnreliable && !bOpening && !AC->bOpenAcked)
		{
			continue;
		}
		int32 Element = 0, RepIndex = INDEX_NONE;
		FUSProperty* Prop = Server.ResolveKey(Actor.Class, P.Key, Element, RepIndex);
		RepIndex = Prop ? NetCache->GetIndexOf(Prop) : INDEX_NONE;
		if (!Prop || RepIndex == INDEX_NONE)
		{
			continue;
		}
		Items.Add({ &P.Key, Prop, Element, RepIndex, (bOwner && V.bOwnerValue) ? &V.OwnerValue : &V.Value, V.bUnreliable });
	}
	Items.Sort([](const FItem& A, const FItem& B) { return A.RepIndex != B.RepIndex ? A.RepIndex < B.RepIndex : A.Element < B.Element; });

	const uint32 MaxIndex = uint32(NetCache->GetMaxIndex());
	constexpr int64 MaxBunchBits = 3600;
	FOutBunch Current;
	auto StartBunch = [&](bool bOpen)
	{
		Current = FOutBunch();
		Current.Channel = AC->Channel;
		Current.ChType = ChActor;
		Current.bOpen = bOpen;
		Current.bReliable = true;
		Current.ActorId = Actor.Id;
		if (bOpen)
		{
			// the archetype: the class and where to spawn it, or the map actor
			UE2Codec::WriteObject(Current.Data, Actor.Archetype, MakeContext());
			if (!Actor.bStatic)
			{
				UE2Codec::WriteCompressedVector(Current.Data, Actor.Location);
				if (Actor.bNetInitialRotation)
				{
					UE2Codec::WriteCompressedRotator(Current.Data, Actor.Rotation);
				}
			}
		}
	};
	bool bAnyReliable = bOpening;
	int32 NumInBunch = 0;
	StartBunch(bOpening);
	auto FinishBunch = [&]()
	{
		if (!Current.bOpen && NumInBunch == 0)
		{
			return;
		}
		if (!Current.bOpen && !bAnyReliable)
		{
			Current.bReliable = false;
		}
		if (Current.bReliable)
		{
			++AC->OutstandingReliable;
		}
		else
		{
			Current.UnreliableKeys.Reserve(NumInBunch);
		}
		QueueBunch(MoveTemp(Current));
	};
	TArray<FString> UnreliableKeys;
	for (const FItem& It : Items)
	{
		FUE2BitWriter W;
		if (!EncodeProperty(W, It.Prop, It.Value))
		{
			continue;
		}
		FShadow* S = AC->Shadow.Find(*It.Key);
		if (S && S->NumBits == int32(W.GetNumBits()) && S->Bits == W.GetData())
		{
			continue;
		}
		FUE2BitWriter Field;
		Field.WriteInt(uint32(It.RepIndex), MaxIndex);
		if (It.Prop->ArrayDim != 1)
		{
			Field.WriteByte(uint8(It.Element));
		}
		Field.WriteBits(W);
		if (Current.Data.GetNumBits() + Field.GetNumBits() > MaxBunchBits && (NumInBunch > 0 || Current.bOpen))
		{
			if (!Current.bReliable || !bAnyReliable)
			{
				Current.UnreliableKeys = UnreliableKeys;
			}
			FinishBunch();
			UnreliableKeys.Reset();
			NumInBunch = 0;
			bAnyReliable = false;
			StartBunch(false);
		}
		Current.Data.WriteBits(Field);
		++NumInBunch;
		++PropertiesSent;
		if (It.bUnreliable)
		{
			UnreliableKeys.Add(*It.Key);
		}
		else
		{
			bAnyReliable = true;
		}
		FShadow& NewShadow = AC->Shadow.FindOrAdd(*It.Key);
		NewShadow.Bits = W.GetData();
		NewShadow.NumBits = int32(W.GetNumBits());
		if (Server.bTrace && !It.bUnreliable)
		{
			UE_LOG(LogUE2Net, VeryVerbose, TEXT("UE2ServerTrace: [%d] ch %d %s"), Id, AC->Channel, **It.Key);
		}
	}
	if (Current.bOpen || NumInBunch > 0)
	{
		const bool bReliableBunch = Current.bOpen || bAnyReliable;
		if (!bReliableBunch)
		{
			Current.UnreliableKeys = UnreliableKeys;
		}
		FinishBunch();
	}
	if (Actor.bTearOff)
	{
		AC->bTornOff = true;
	}
	if (bOpening && Actor.bNetTemporary)
	{
		// sent once: the client keeps the actor and simulates it alone (UE2 bNetTemporary); the channel closes now
		AC->bTemporary = true;
		CloseActorChannel(Actor.Id);
		FinishedActors.Add(Actor.Id);
	}
	return true;
}

void FUE2ServerConnection::CloseActorChannel(int32 ActorId)
{
	FActorChannel* AC = ChannelsByActor.Find(ActorId);
	if (!AC)
	{
		return;
	}
	if (State == EState::Joined)
	{
		FOutBunch B;
		B.Channel = AC->Channel;
		B.ChType = ChActor;
		B.bClose = true;
		B.bReliable = true;
		QueueBunch(MoveTemp(B));
	}
	if (Server.bTrace)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] ch %d close (actor %d)"), Id, AC->Channel, ActorId);
	}
	RecentlyClosed.Add(AC->Channel, Server.GetTime());
	ActorByChannel.Remove(AC->Channel);
	Channels.Remove(AC->Channel);
	ChannelsByActor.Remove(ActorId);
}

bool FUE2ServerConnection::CallFunction(FUE2SvActor& Actor, FName FunctionName, const TArray<FUE2NetValue>& Params, bool bOwner, bool bForceUnreliable)
{
	const FUE2ClassNetCache* NetCache = State == EState::Joined ? PackageMap.GetClassNetCache(Actor.Class) : nullptr;
	if (!NetCache)
	{
		return false;
	}
	FUSFunction* Function = NetCache->FindFunction(FunctionName);
	if (!Function)
	{
		static TSet<FString> Warned;
		const FString Key = Actor.ClassPath + TEXT(".") + FunctionName.ToString();
		if (!Warned.Contains(Key))
		{
			Warned.Add(Key);
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: %s has no replicated function %s"), *Actor.ClassPath, *FunctionName.ToString());
		}
		return false;
	}
	FActorChannel* AC = ChannelsByActor.Find(Actor.Id);
	if (!AC)
	{
		ReplicateActor(Actor, Server.GetTime(), bOwner);
		AC = ChannelsByActor.Find(Actor.Id);
	}
	if (!AC || AC->bClosing || AC->bTemporary)
	{
		return false;
	}
	const bool bReliable = !bForceUnreliable && (Function->FunctionFlags & UE2FuncNetReliable) != 0;
	if (!bReliable && !AC->bOpenAcked)
	{
		// the client would drop it (its channel is not open there yet)
		return false;
	}
	FOutBunch B;
	B.Channel = AC->Channel;
	B.ChType = ChActor;
	B.bReliable = bReliable;
	B.ActorId = Actor.Id;
	B.Data.WriteInt(uint32(NetCache->GetIndexOf(Function)), uint32(NetCache->GetMaxIndex()));
	if (!UE2Codec::WriteFunctionParams(B.Data, Function, Params, MakeContext()))
	{
		UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: cannot encode the parameters of %s.%s"), *Actor.ClassPath, *FunctionName.ToString());
		return false;
	}
	if (bReliable)
	{
		++AC->OutstandingReliable;
	}
	++CallsSent;
	if (Server.bTrace && FunctionName != TEXT("ClientAdjustPosition") && FunctionName != TEXT("ShortClientAdjustPosition") && FunctionName != TEXT("VeryShortClientAdjustPosition"))
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2ServerTrace: [%d] ch %d call %s.%s"), Id, AC->Channel, *Actor.ClassPath, *FunctionName.ToString());
	}
	QueueBunch(MoveTemp(B));
	return true;
}

void FUE2ServerConnection::ReceivedActorBunch(FInBunch& Bunch)
{
	const int32 ActorId = ActorOfChannel(Bunch.Channel);
	FUE2SvActor* Actor = ActorId ? Server.FindActor(ActorId) : nullptr;
	const FUE2ClassNetCache* NetCache = Actor ? PackageMap.GetClassNetCache(Actor->Class) : nullptr;
	if (!NetCache)
	{
		return;
	}
	FUE2BitReader& R = Bunch.Data;
	const FUE2WireContext Ctx = MakeContext();
	while (!R.AtEnd() && State == EState::Joined)
	{
		const int32 RepIndex = int32(R.ReadInt(uint32(NetCache->GetMaxIndex())));
		FUSField* Field = R.IsError() ? nullptr : NetCache->GetFromIndex(RepIndex);
		if (!Field || !IsFunction(Field))
		{
			// clients only send function calls
			++BunchErrors;
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: %s sent a bad field %d on %s"), *PlayerName, RepIndex, *Actor->ClassPath);
			break;
		}
		FUSFunction* Function = static_cast<FUSFunction*>(Field);
		TArray<TPair<FName, FUE2NetValue>> Params;
		if (!UE2Codec::ReadFunctionParams(R, Function, Params, Ctx))
		{
			++BunchErrors;
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: cannot read the parameters of %s from %s"), *Function->Name.ToString(), *PlayerName);
			break;
		}
		++CallsReceived;
		Server.ClientFunction(*this, ActorId, Function, Params);
		// the call may have changed the actor list
		Actor = Server.FindActor(ActorId);
		if (!Actor)
		{
			break;
		}
	}
}

#endif // OPENUT2K4_UE2SERVER_BUILD

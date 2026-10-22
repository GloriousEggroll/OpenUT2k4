#include "UE2NetConnection.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "../UScript/UScriptRuntime.h"
#include "UE2NetCDKey.h"
#include "UE2NetLog.h"
#include "UE2NetValidator.h"
#include "UE2ServerCodec.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogUE2Net);

namespace UE2NetConnectionLocal
{
	/** Engine version we announce. 3369 is the last retail patch; OldUnreal's 3374 servers accept it too. */
	constexpr int32 ClientVersion = 3369;
	/** The oldest server version we join, as stock clients announce it (ENGINE_MIN_NET_VERSION): a server rejects
	 *  clients whose MINVER is above its own version, so 3369 here shut out 3339 servers. */
	constexpr int32 ClientMinVersion = 3180;
	/** Seconds without any packet from the server before we give up (UT2004 ConnectionTimeout is 20). */
	constexpr double ReceiveTimeout = 20.0;
	/** After LOGIN, give up if WELCOME or a rejection has not arrived in this time. */
	constexpr double LoginTimeout = 12.0;
	/** A reliable bunch that is not acknowledged in this time is sent again (UT2004 AckTimeout is 1). */
	constexpr double ResendTime = 1.0;
	/** We send at least one packet in this interval (acks / keep-alive; UT2004 KeepAliveTime is 0.2). */
	constexpr double KeepAliveTime = 0.2;
	/** Channel types (UE2 EChannelType). */
	constexpr uint8 ChControl = 1;
	constexpr uint8 ChActor = 2;
	constexpr uint8 ChFile = 3;
	/** UE2 function flag: the call must arrive (sent reliably). */
	constexpr uint32 UE2FUNC_NetReliable = 0x00000080;
	/** UE2 property flags of function parameters. */
	constexpr uint32 UE2CPF_Parm = 0x00000080;
	constexpr uint32 UE2CPF_ReturnParm = 0x00000400;

	/** Value nearest to Reference that has these low bits (UE2 MakeRelative for wrapping sequence numbers). */
	int32 MakeRelative(int32 Value, int32 Reference, int32 Max)
	{
		const int32 Diff = ((Value - Reference + Max / 2) & (Max - 1)) - Max / 2;
		return Reference + Diff;
	}

	/** The engine-level login challenge answer (UGameEngine::ChallengeResponse): a fixed scramble of the number. */
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

	/** Reads Count raw bits (least significant first), as UE2 FArchive::SerializeBits does for enums. */
	uint32 ReadRawBits(FUE2BitReader& R, int32 Count)
	{
		uint32 Value = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			if (R.ReadBit())
			{
				Value |= 1u << i;
			}
		}
		return Value;
	}

	void WriteRawBits(FUE2BitWriter& W, uint32 Value, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			W.WriteBit((Value >> i) & 1);
		}
	}

	/** UE2 FVector::SerializeCompressed / vector property: integer components with a shared bit count. */
	FVector3f ReadCompressedVector(FUE2BitReader& R)
	{
		const uint32 Bits = R.ReadInt(20);
		const int32 Bias = 1 << (Bits + 1);
		const uint32 Max = 1u << (Bits + 2);
		const int32 X = int32(R.ReadInt(Max)) - Bias;
		const int32 Y = int32(R.ReadInt(Max)) - Bias;
		const int32 Z = int32(R.ReadInt(Max)) - Bias;
		return FVector3f(float(X), float(Y), float(Z));
	}

	void WriteCompressedVector(FUE2BitWriter& W, const FVector3f& V)
	{
		const int32 X = FMath::RoundToInt(V.X), Y = FMath::RoundToInt(V.Y), Z = FMath::RoundToInt(V.Z);
		const int32 MaxAbs = FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z));
		const uint32 Bits = uint32(FMath::Clamp(UE2CeilLogTwo(uint32(1 + MaxAbs)), 1, 20) - 1);
		W.WriteInt(Bits, 20);
		const int32 Bias = 1 << (Bits + 1);
		const uint32 Max = 1u << (Bits + 2);
		W.WriteInt(uint32(X + Bias), Max);
		W.WriteInt(uint32(Y + Bias), Max);
		W.WriteInt(uint32(Z + Bias), Max);
	}

	/** UE2 FRotator::SerializeCompressed / rotator property: each component as "present" bit + its high byte. */
	FIntVector ReadCompressedRotator(FUE2BitReader& R)
	{
		int32 C[3];
		for (int32& V : C)
		{
			V = R.ReadBit() ? int32(R.ReadByte()) << 8 : 0;
		}
		return FIntVector(C[0], C[1], C[2]);
	}

	void WriteCompressedRotator(FUE2BitWriter& W, const FIntVector& Rot)
	{
		for (int32 V : { Rot.X, Rot.Y, Rot.Z })
		{
			const uint8 B = uint8((V >> 8) & 0xFF);
			W.WriteBit(B != 0);
			if (B != 0)
			{
				W.WriteByte(B);
			}
		}
	}

	FString CleanText(const FString& Text)
	{
		FString Out = Text.Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT(""));
		return Out.Len() > 200 ? Out.Left(200) + TEXT("...") : Out;
	}

	/** True for a value equal to its type's zero (UE2 UProperty::Matches against no default): not sent as a parameter. */
	bool IsZeroValue(const FUE2NetValue& V)
	{
		switch (V.Kind)
		{
		case FUE2NetValue::EKind::None:
			return true;
		case FUE2NetValue::EKind::Byte:
		case FUE2NetValue::EKind::Int:
		case FUE2NetValue::EKind::Bool:
			return V.I == 0;
		case FUE2NetValue::EKind::Float:
			return V.F == 0.f;
		case FUE2NetValue::EKind::Object:
			return V.Obj.IsNone();
		case FUE2NetValue::EKind::Name:
			return V.N.IsNone();
		case FUE2NetValue::EKind::Str:
			return V.S.IsEmpty();
		case FUE2NetValue::EKind::Vector:
			return V.V.X == 0.f && V.V.Y == 0.f && V.V.Z == 0.f;
		case FUE2NetValue::EKind::Rotator:
			return V.R == FIntVector::ZeroValue;
		case FUE2NetValue::EKind::Plane:
			return V.P.X == 0.f && V.P.Y == 0.f && V.P.Z == 0.f && V.P.W == 0.f;
		case FUE2NetValue::EKind::Struct:
			for (const TPair<FName, FUE2NetValue>& M : V.Members)
			{
				if (!IsZeroValue(M.Value))
				{
					return false;
				}
			}
			return true;
		}
		return false;
	}
}

using namespace UE2NetConnectionLocal;

FUE2NetConnection::FUE2NetConnection() = default;

FString FUE2NetConnection::GetRemoteIP() const
{
	return RemoteAddr.IsValid() ? RemoteAddr->ToString(false) : FString();
}

FUE2NetConnection::~FUE2NetConnection()
{
	UE2NetValidator::ConnectionClosed(*this);
	if (IsOpen())
	{
		Close(TEXT(""));
	}
	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

// =================================================================================================
// Connect / close
// =================================================================================================

bool FUE2NetConnection::Connect(const FSettings& InSettings, FString& OutError)
{
	Settings = InSettings;
#if !UE_BUILD_SHIPPING
	{
		// test hook: -UE2NetLag=<ms> adds that much round trip time (half to each direction)
		float LagMs = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("UE2NetLag="), LagMs) && LagMs > 0.f)
		{
			SimLagSeconds = LagMs / 1000.0;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetLag: adding %.0f ms round trip time"), LagMs);
		}
	}
#endif
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS)
	{
		OutError = TEXT("no socket subsystem");
		return false;
	}
	RemoteAddr = SS->GetAddressFromString(Settings.Host);
	if (!RemoteAddr.IsValid() || !RemoteAddr->IsValid())
	{
		// a host name: one blocking lookup (IPv4 first, UT2004 servers are IPv4 only)
		FAddressInfoResult Res = SS->GetAddressInfo(*Settings.Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, ESocketType::SOCKTYPE_Datagram);
		RemoteAddr.Reset();
		if (Res.ReturnCode == SE_NO_ERROR && Res.Results.Num() > 0)
		{
			RemoteAddr = Res.Results[0].Address->Clone();
		}
	}
	if (!RemoteAddr.IsValid())
	{
		OutError = FString::Printf(TEXT("cannot resolve %s"), *Settings.Host);
		return false;
	}
	RemoteAddr->SetPort(Settings.Port);

	Socket = SS->CreateSocket(NAME_DGram, TEXT("UE2NetClient"), RemoteAddr->GetProtocolType());
	if (!Socket)
	{
		OutError = TEXT("cannot create a UDP socket");
		return false;
	}
	Socket->SetNonBlocking(true);
	int32 Actual = 0;
	Socket->SetReceiveBufferSize(1 << 20, Actual);
	TSharedRef<FInternetAddr> Local = SS->CreateInternetAddr(RemoteAddr->GetProtocolType());
	Local->SetAnyAddress();
	Local->SetPort(0);
	if (!Socket->Bind(*Local))
	{
		OutError = TEXT("cannot bind a UDP socket");
		SS->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	ConnectTime = Now;
	LastReceiveTime = Now;
	State = EState::Connecting;
	UE_LOG(LogUE2Net, Log, TEXT("Connecting to %s (UE2 protocol, version %d)"), *GetAddressString(), ClientVersion);
	// the client opens the control channel with its first reliable bunch
	SendControl(FString::Printf(TEXT("HELLO REVISION=0 MINVER=%d VER=%d"), ClientMinVersion, ClientVersion), true);
	Flush(Now);
	return true;
}

void FUE2NetConnection::Close(const FString& Reason)
{
	if (!IsOpen())
	{
		return;
	}
	// a reliable close bunch on the control channel: the server closes the connection and logs the player out
	FOutBunch CloseBunch;
	CloseBunch.Channel = 0;
	CloseBunch.ChType = ChControl;
	CloseBunch.bClose = true;
	CloseBunch.bReliable = true;
	QueueBunch(MoveTemp(CloseBunch));
	const double Now = FPlatformTime::Seconds();
	Flush(Now);
	// once more in case the first packet is lost (the server drops the duplicate by its sequence number)
	for (FOutBunch& B : OutReliableBunches)
	{
		if (B.Channel == 0 && B.bClose)
		{
			FOutBunch Again = B;
			SendQueue.Add(MoveTemp(Again));
		}
	}
	OutReliableBunches.Reset();
	Flush(Now);
	SetClosed(Reason);
}

void FUE2NetConnection::SetClosed(const FString& Reason)
{
	if (State == EState::Closed)
	{
		return;
	}
	State = EState::Closed;
	CloseReason = Reason;
	UE_LOG(LogUE2Net, Log, TEXT("Connection to %s closed%s%s (packets in %d out %d, bytes in %lld out %lld, actors opened %d closed %d, properties %d, calls %d, bunch errors %d)"),
		*GetAddressString(), Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Reason, PacketsIn, PacketsOut, BytesIn, BytesOut,
		ActorsOpened, ActorsClosed, PropertiesReceived, FunctionsReceived, BunchErrors);
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	if (OnClosed)
	{
		OnClosed(Reason);
	}
}

FString FUE2NetConnection::DescribeStatus() const
{
	switch (State)
	{
	case EState::Connecting: return FString::Printf(TEXT("Connecting to %s..."), *GetAddressString());
	case EState::LoggingIn: return FString::Printf(TEXT("Logging in to %s..."), *GetAddressString());
	case EState::Welcomed: return FString::Printf(TEXT("Loading %s..."), *MapName);
	case EState::Joined: return FString::Printf(TEXT("Connected to %s"), *GetAddressString());
	case EState::Closed: return CloseReason.IsEmpty() ? FString(TEXT("Disconnected")) : FString::Printf(TEXT("Disconnected: %s"), *CloseReason);
	default: return FString();
	}
}

// =================================================================================================
// Tick / packets
// =================================================================================================

void FUE2NetConnection::Tick(double Now)
{
	if (!IsOpen() || !Socket)
	{
		return;
	}
	while (DelayedOut.Num() > 0 && DelayedOut[0].Key <= Now)
	{
		int32 Sent = 0;
		Socket->SendTo(DelayedOut[0].Value.GetData(), DelayedOut[0].Value.Num(), Sent, *RemoteAddr);
		DelayedOut.RemoveAt(0);
	}
	ReceivePackets(Now);
	if (!IsOpen())
	{
		return;
	}
	UE2NetValidator::Tick(*this);
	if (Now - LastReceiveTime > ReceiveTimeout)
	{
		SetClosed(TEXT("the server stopped responding"));
		return;
	}
	// login timeout: after LOGIN we must get WELCOME (or a rejection) in good time. Without this a silent reject would
	// leave us flushing keep-alives for the full 20 s receive timeout.
	if ((State == EState::LoggingIn || State == EState::Connecting) && LoginTime > 0.0 && Now - LoginTime > LoginTimeout)
	{
		SetClosed(TEXT("the server did not finish the login"));
		return;
	}
	// reliable bunches without an ack in time go out again (same sequence number)
	for (int32 i = OutReliableBunches.Num() - 1; i >= 0; --i)
	{
		if (Now - OutReliableBunches[i].SentTime > ResendTime)
		{
			SendQueue.Insert(MoveTemp(OutReliableBunches[i]), 0);
			OutReliableBunches.RemoveAt(i);
		}
	}
	if (SendQueue.Num() > 0 || PendingAcks.Num() > 0 || Now - LastSendTime > KeepAliveTime)
	{
		Flush(Now);
	}
}

void FUE2NetConnection::ReceivePackets(double Now)
{
	uint8 Buffer[4096];
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> From = SS->CreateInternetAddr(RemoteAddr->GetProtocolType());
	for (int32 Guard = 0; Guard < 1000 && Socket && IsOpen(); ++Guard)
	{
		int32 Read = 0;
		if (!Socket->RecvFrom(Buffer, sizeof(Buffer), Read, *From) || Read <= 0)
		{
			break;
		}
		// only the server we talk to
		if (!From->CompareEndpoints(*RemoteAddr))
		{
			continue;
		}
		if (SimLagSeconds > 0.0)
		{
			// test hook -UE2NetLag: hold the packet back (Tick releases it)
			DelayedIn.Add({ Now + SimLagSeconds * 0.5, TArray<uint8>(Buffer, Read) });
			continue;
		}
		LastReceiveTime = Now;
		++PacketsIn;
		BytesIn += Read;
		ReceivedRawPacket(Buffer, Read, Now);
	}
	while (DelayedIn.Num() > 0 && DelayedIn[0].Key <= Now && IsOpen())
	{
		const TArray<uint8> Data = MoveTemp(DelayedIn[0].Value);
		DelayedIn.RemoveAt(0);
		LastReceiveTime = Now;
		++PacketsIn;
		BytesIn += Data.Num();
		ReceivedRawPacket(Data.GetData(), Data.Num(), Now);
	}
}

void FUE2NetConnection::ReceivedRawPacket(const uint8* Data, int32 Count, double Now)
{
	// the packet's last byte holds a terminating 1 bit after the last data bit
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
	// every packet is acknowledged (the server resends reliable bunches of packets it gets no ack for)
	PendingAcks.Add(RawId);

	while (!R.AtEnd() && IsOpen())
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
				// acks come in packet order: ids skipped since the last one were lost
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
			UE_LOG(LogUE2Net, Warning, TEXT("Malformed packet %d (bunch on channel %d)"), PacketId, Bunch.Channel);
			break;
		}
		ReceivedBunch(Bunch);
	}
}

void FUE2NetConnection::ReceivedAck(int32 AckId)
{
	OutReliableBunches.RemoveAll([AckId](const FOutBunch& B) { return B.PacketId == AckId; });
}

void FUE2NetConnection::ReceivedNak(int32 NakId)
{
	for (int32 i = OutReliableBunches.Num() - 1; i >= 0; --i)
	{
		if (OutReliableBunches[i].PacketId == NakId)
		{
			SendQueue.Insert(MoveTemp(OutReliableBunches[i]), 0);
			OutReliableBunches.RemoveAt(i);
		}
	}
}

void FUE2NetConnection::QueueBunch(FOutBunch&& Bunch)
{
	if (Bunch.bReliable)
	{
		Bunch.Sequence = ++OutReliable[Bunch.Channel];
	}
	SendQueue.Add(MoveTemp(Bunch));
}

void FUE2NetConnection::SendControl(const FString& Text, bool bOpen)
{
	FOutBunch B;
	B.Channel = 0;
	B.ChType = ChControl;
	B.bOpen = bOpen;
	B.bReliable = true;
	B.Data.WriteString(Text);
	if (Settings.bTrace)
	{
		// never log a server password, the AUTH line (CD key hash + stats password), or a URL password
		FString Shown = CleanText(Text);
		if (Shown.StartsWith(TEXT("AUTH ")))
		{
			Shown = TEXT("AUTH HASH=*** GM=*** USERNAME=*** PASSWORD=***");
		}
		const int32 At = Shown.Find(TEXT("?Password="), ESearchCase::IgnoreCase);
		if (At != INDEX_NONE)
		{
			int32 End = Shown.Find(TEXT("?"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1);
			Shown = Shown.Left(At) + TEXT("?Password=***") + (End != INDEX_NONE ? Shown.Mid(End) : FString());
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetTrace: control >> %s"), *Shown);
	}
	QueueBunch(MoveTemp(B));
}

void FUE2NetConnection::WriteBunchHeader(FUE2BitWriter& W, const FOutBunch& B) const
{
	W.WriteBit(false); // not an ack
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

void FUE2NetConnection::Flush(double Now)
{
	// once closed, never send again (a rejected login must not keep flushing keep-alive packets at the server)
	if (!Socket || State == EState::Closed)
	{
		SendQueue.Reset();
		PendingAcks.Reset();
		return;
	}
	// payload limit: MaxPacket bytes including the terminating bit
	constexpr int64 MaxBits = MaxPacketBytes * 8 - 1;
	TArray<FOutBunch> Queue = MoveTemp(SendQueue);
	SendQueue.Reset();
	int32 Next = 0;
	bool bFirst = true;
	while (bFirst || Next < Queue.Num())
	{
		FUE2BitWriter Packet;
		const int32 ThisPacketId = OutPacketId;
		Packet.WriteInt(uint32(ThisPacketId) & (MaxPacketId - 1), MaxPacketId);
		if (bFirst)
		{
			for (int32 Ack : PendingAcks)
			{
				Packet.WriteBit(true);
				Packet.WriteInt(uint32(Ack), MaxPacketId);
				if (Packet.GetNumBits() > MaxBits - 64)
				{
					break;
				}
			}
			PendingAcks.Reset();
		}
		bFirst = false;
		int32 InPacket = 0;
		while (Next < Queue.Num())
		{
			FOutBunch& B = Queue[Next];
			FUE2BitWriter Header;
			WriteBunchHeader(Header, B);
			if (Packet.GetNumBits() + Header.GetNumBits() + B.Data.GetNumBits() > MaxBits && InPacket > 0)
			{
				break;
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
		}
		SendPacket(Packet);
		LastSendTime = Now;
	}
}

void FUE2NetConnection::SendPacket(const FUE2BitWriter& Packet)
{
	const TArray<uint8> Bytes = Packet.GetPacketBytes();
	int32 Sent = 0;
	if (SimLagSeconds > 0.0)
	{
		DelayedOut.Add({ FPlatformTime::Seconds() + SimLagSeconds * 0.5, Bytes });
	}
	else
	{
		Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *RemoteAddr);
	}
	++OutPacketId;
	++PacketsOut;
	BytesOut += Bytes.Num();
}

// =================================================================================================
// Bunches / channels
// =================================================================================================

void FUE2NetConnection::ReceivedBunch(FInBunch& Bunch)
{
	const int32 Ch = Bunch.Channel;
	if (Bunch.bReliable && Bunch.Sequence <= InReliable[Ch])
	{
		return; // already processed
	}
	FChannel* Channel = Channels.Find(Ch);
	if (!Channel)
	{
		if (!Bunch.bReliable && !Bunch.bOpen)
		{
			return; // unreliable data for a channel that is gone or not open yet
		}
		if (Bunch.ChType != ChControl && Bunch.ChType != ChActor && Bunch.ChType != ChFile)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Bunch opens channel %d with unknown type %d"), Ch, Bunch.ChType);
			++BunchErrors;
			return;
		}
		Channel = &Channels.Add(Ch);
		Channel->ChType = Bunch.ChType;
		Channel->bOpenedByServer = true;
	}
	if (!Bunch.bReliable)
	{
		ProcessBunch(Bunch);
		return;
	}
	if (Bunch.Sequence != InReliable[Ch] + 1)
	{
		// early: keep it until the ones before it arrive
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
		if (!IsOpen())
		{
			break;
		}
		if (!Channels.Contains(Ch))
		{
			// closed by an earlier bunch: a new open bunch re-creates it
			if (!B.bOpen)
			{
				continue;
			}
			Channels.Add(Ch).ChType = B.ChType;
		}
		ProcessBunch(B);
	}
}

void FUE2NetConnection::ProcessBunch(FInBunch& Bunch)
{
	FChannel* Channel = Channels.Find(Bunch.Channel);
	if (!Channel)
	{
		return;
	}
	const uint8 Type = Channel->ChType;
	if (Type == ChControl)
	{
		while (!Bunch.Data.AtEnd() && IsOpen())
		{
			const FString Text = Bunch.Data.ReadString();
			if (Bunch.Data.IsError())
			{
				++BunchErrors;
				break;
			}
			ReceivedText(Text);
		}
	}
	else if (Type == ChActor)
	{
		ReceivedActorBunch(Bunch);
	}
	else if (Type == ChFile)
	{
		// file download data (UFileChannel): the raw bytes of the file, in order (reliable bunches)
		TArray<uint8> Bytes;
		const int32 NumBytes = int32(Bunch.Data.GetNumBits() / 8);
		Bytes.SetNumUninitialized(NumBytes);
		if (NumBytes > 0)
		{
			Bunch.Data.ReadBytes(Bytes.GetData(), NumBytes);
		}
		if (OnFileData && !Channel->bOpenedByServer)
		{
			OnFileData(Bunch.Channel, Bytes, Bunch.bClose);
		}
		else if (Bunch.bOpen)
		{
			UE_LOG(LogUE2Net, Log, TEXT("Server opened a file channel (%d): ignored"), Bunch.Channel);
		}
		if (!IsOpen() || !Channels.Contains(Bunch.Channel))
		{
			return;
		}
	}
	if (Bunch.bClose && IsOpen())
	{
		if (Type == ChControl)
		{
			SetClosed(TEXT("the server closed the connection"));
			return;
		}
		if (Type == ChActor)
		{
			CloseActor(Bunch.Channel);
		}
		Channels.Remove(Bunch.Channel);
	}
}

// =================================================================================================
// Control channel: the login handshake
// =================================================================================================

void FUE2NetConnection::ReceivedText(const FString& Text)
{
	if (Settings.bTrace)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetTrace: control << %s"), *CleanText(Text));
	}
	const TCHAR* Str = *Text;
	if (FParse::Command(&Str, TEXT("CHALLENGE")))
	{
		FParse::Value(Str, TEXT("VER="), NegotiatedVersion);
		FParse::Value(Str, TEXT("CHALLENGE="), Challenge);
		FParse::Value(Str, TEXT("STATS="), ServerStats);
		UE_LOG(LogUE2Net, Log, TEXT("Server version %d, logging in as \"%s\"%s"), NegotiatedVersion, *Settings.PlayerName, Settings.bSpectator ? TEXT(" (spectator)") : TEXT(""));
		// the login URL: options only (the server ignores the map part)
		FString Name = Settings.PlayerName.Replace(TEXT("?"), TEXT("")).Replace(TEXT(" "), TEXT("_")).Replace(TEXT("#"), TEXT(""));
		if (Name.IsEmpty())
		{
			Name = TEXT("Player");
		}
		FString URL = FString::Printf(TEXT("Entry?Name=%s"), *Name.Left(20));
		if (Settings.bSpectator)
		{
			URL += TEXT("?SpectatorOnly=1");
		}
		URL += Settings.ExtraOptions;
		// honest identification: OpenUT2k4 and its version (stock servers ignore unknown options; Net/UE2NetValidator.h)
		URL += UE2NetValidator::GetLoginOption();
		// the OldUnreal 3374 client's order: AUTH, NETSPEED, LOGIN
		SendAuth();
		SendControl(FString::Printf(TEXT("NETSPEED %d"), Settings.NetSpeed));
		SendControl(FString::Printf(TEXT("LOGIN RESPONSE=%d URL=%s"), ChallengeResponse(Challenge), *URL));
		State = EState::LoggingIn;
		LoginTime = FPlatformTime::Seconds();
	}
	else if (FParse::Command(&Str, TEXT("USES")))
	{
		PackageMap.AddUses(Str);
	}
	else if (FParse::Command(&Str, TEXT("WELCOME")))
	{
		// map names may hold ',' and ')' (see UE2NetPackageMap.cpp ParseValue)
		FParse::Value(Str, TEXT("LEVEL="), MapName, false);
		FParse::Value(Str, TEXT("GAME="), GameClass, false);
		PackageMap.MatchLocalPackages();
		const TArray<FString> Missing = PackageMap.GetMissing();
		const TArray<FString> GenDiff = PackageMap.GetGenerationDifferences();
		UE_LOG(LogUE2Net, Log, TEXT("Welcome: map %s, game %s, %d packages (%d missing or different, %d with other generations)"),
			*MapName, *GameClass, PackageMap.GetList().Num(), Missing.Num(), GenDiff.Num());
		for (const FString& M : Missing)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("  package not available: %s"), *M);
		}
		for (const FString& G : GenDiff)
		{
			UE_LOG(LogUE2Net, Verbose, TEXT("  generations: %s"), *G);
		}
		State = EState::Welcomed;
		if (OnWelcome)
		{
			OnWelcome();
		}
	}
	else if (FParse::Command(&Str, TEXT("FAILCODE")))
	{
		// "FAILCODE <code> [error text]": a machine-readable rejection, usually followed (or preceded) by FAILURE
		FString Rest = FString(Str).TrimStartAndEnd();
		FString Code = Rest, Error;
		Rest.Split(TEXT(" "), &Code, &Error);
		CloseFailCode = Code.ToUpper();
		const FString Message = FailCodeMessage(CloseFailCode, Error.TrimStartAndEnd());
		// a FAILURE may have closed us first with a generic message; the FAILCODE message is clearer, so prefer it
		CloseReason = Message;
		SetClosed(Message);
	}
	else if (FParse::Command(&Str, TEXT("FAILURE")))
	{
		// text form; if a FAILCODE already closed us, keep its message
		if (State != EState::Closed)
		{
			SetClosed(FString(Str).TrimStartAndEnd().IsEmpty() ? FString(TEXT("the server refused the connection")) : FString(Str).TrimStartAndEnd());
		}
	}
	else if (FParse::Command(&Str, TEXT("UPGRADE")))
	{
		int32 MinVer = 0, Ver = 0;
		FParse::Value(Str, TEXT("MINVER="), MinVer);
		FParse::Value(Str, TEXT("VER="), Ver);
		CloseFailCode = TEXT("UPGRADE");
		SetClosed(FString::Printf(TEXT("the server needs engine version %d or newer (server %d)"), MinVer, Ver));
	}
	else if (FParse::Command(&Str, TEXT("OUTVALIDATE")))
	{
		// OpenUT2k4 Validator challenge of our own server (Net/UE2NetValidator.h)
		UE2NetValidator::ReceivedControl(*this, Str);
	}
	else if (FParse::Command(&Str, TEXT("DLMGR")))
	{
		// download methods the server offers, in order (Engine.ChannelDownload, IpDrv.HTTPDownload with the redirect
		// URL in PARAMS=): kept for the package download path (Docs/UE2Net.md "Downloads")
		DownloadManagers.Add(FString(Str).TrimStartAndEnd());
	}
	else
	{
		UE_LOG(LogUE2Net, Verbose, TEXT("Unhandled control message: %s"), *CleanText(Text));
	}
}

FString FUE2NetConnection::FailCodeMessage(const FString& Code, const FString& Error)
{
	// plain-language messages for the known UT2004 FAILCODEs (Engine ULevel::NotifyReceivedText)
	if (Code == TEXT("SERVERFULL")) return TEXT("The server is full.");
	if (Code == TEXT("NEEDPW")) return TEXT("This server needs a password.");
	if (Code == TEXT("WRONGPW")) return TEXT("Wrong password.");
	if (Code == TEXT("NEEDSTATS")) return TEXT("This server requires UT2004 stats tracking. Turn on Track Stats and set a Stats Username and Password.");
	if (Code == TEXT("LOCALBAN")) return TEXT("This server has banned your account.");
	if (Code == TEXT("SESSIONBAN")) return TEXT("You are banned from this server for this session.");
	if (Code == TEXT("CHALLENGE")) return TEXT("The server rejected the CD key challenge.");
	if (Code == TEXT("LOGIN")) return Error.IsEmpty() ? FString(TEXT("The server rejected the login.")) : Error;
	if (!Error.IsEmpty()) return Error;
	return FString::Printf(TEXT("The server refused the connection (%s)."), *Code);
}

void FUE2NetConnection::SendAuth()
{
	// As the OldUnreal 3374 client sends it (UNetPendingLevel::NotifyReceivedText, checked against a local 3374
	// server): always sent, with or without a CD key. The stats account goes only to a STATS=1 server, and only when
	// Track Stats is on and both the username and the password are set; otherwise both are blank.
	FString Hash = UE2NetCDKey::ComputeAuthHash();
	const FString GM = UE2NetCDKey::MakeGM();
	FString Message;
	if (ServerStats == 1 && Settings.bTrackStats && !Settings.StatsUsername.IsEmpty() && !Settings.StatsPassword.IsEmpty())
	{
		FString Password = UE2NetCDKey::EncryptStatsPassword(Settings.StatsPassword);
		Message = FString::Printf(TEXT("AUTH HASH=%s GM=%s USERNAME=%s PASSWORD=%s"), *Hash, *GM, *Settings.StatsUsername, *Password);
		Password.Empty();
	}
	else
	{
		Message = FString::Printf(TEXT("AUTH HASH=%s GM=%s USERNAME= PASSWORD="), *Hash, *GM);
	}
	SendControl(Message);
	// the hash must never reach the log
	Hash.Empty();
	Message.Empty();
}

void FUE2NetConnection::SendJoin()
{
	if (State != EState::Welcomed)
	{
		return;
	}
	// packages we cannot match leave the server's map for this connection (SKIP), so that the numbering of
	// everything after them stays the same on both sides; objects in them then arrive as None
	TArray<FString> Skipped;
	for (const FUE2PackageInfo& Info : PackageMap.GetList())
	{
		if (!Info.bGuidMatch)
		{
			SendControl(FString::Printf(TEXT("SKIP GUID=%s"), *UE2GuidToString(Info.Guid)));
			Skipped.Add(Info.Name);
		}
		else if (Info.LocalGeneration != Info.RemoteGeneration)
		{
			// our file has another number of generations: the server uses the older one we both have
			SendControl(FString::Printf(TEXT("HAVE GUID=%s GEN=%d"), *UE2GuidToString(Info.Guid), Info.LocalGeneration));
		}
	}
	PackageMap.RemovePackages(Skipped);
	PackageMap.Compute();
	UE_LOG(LogUE2Net, Log, TEXT("Joining: %d packages, %d objects, %d names%s"), PackageMap.GetList().Num(), PackageMap.GetMaxObjectIndex(),
		PackageMap.GetMaxNameIndex(), Skipped.Num() ? *FString::Printf(TEXT(", skipped %s"), *FString::Join(Skipped, TEXT(", "))) : TEXT(""));
	SendControl(TEXT("JOIN"));
	State = EState::Joined;
	Flush(FPlatformTime::Seconds());
}

// =================================================================================================
// File channels (package downloads)
// =================================================================================================

int32 FUE2NetConnection::OpenFileChannel(const FGuid& Guid)
{
	if (!IsOpen())
	{
		return 0;
	}
	// a new index each time: the server keeps a channel it closed until our ack of the close arrives, and drops an open
	// bunch for that index meanwhile (seen after a refused download when the next request reused the index)
	int32 Ch = 0;
	for (int32 Tries = 0; Tries < int32(MaxChannels) - 1; ++Tries)
	{
		const int32 Candidate = 1 + (NextFileChannel - 1 + Tries) % (int32(MaxChannels) - 1);
		if (!Channels.Contains(Candidate))
		{
			Ch = Candidate;
			break;
		}
	}
	if (Ch == 0)
	{
		return 0;
	}
	NextFileChannel = Ch + 1;
	FChannel& Channel = Channels.Add(Ch);
	Channel.ChType = ChFile;
	Channel.bOpenedByServer = false;
	// UChannelDownload::ReceiveFile: an open, reliable bunch on a new file channel with the package GUID (four DWORDs)
	FOutBunch B;
	B.Channel = Ch;
	B.ChType = ChFile;
	B.bOpen = true;
	B.bReliable = true;
	const uint32 Words[4] = { Guid.A, Guid.B, Guid.C, Guid.D };
	for (uint32 W : Words)
	{
		const uint8 Le[4] = { uint8(W), uint8(W >> 8), uint8(W >> 16), uint8(W >> 24) };
		B.Data.WriteBytes(Le, 4);
	}
	if (Settings.bTrace)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetTrace: file channel %d >> request %s"), Ch, *UE2GuidToString(Guid));
	}
	QueueBunch(MoveTemp(B));
	return Ch;
}

void FUE2NetConnection::CloseFileChannel(int32 Channel)
{
	const FChannel* C = Channels.Find(Channel);
	if (!IsOpen() || !C || C->ChType != ChFile)
	{
		return;
	}
	FOutBunch B;
	B.Channel = Channel;
	B.ChType = ChFile;
	B.bClose = true;
	B.bReliable = true;
	QueueBunch(MoveTemp(B));
	Channels.Remove(Channel);
}

void FUE2NetConnection::SetNetSpeed(int32 BytesPerSecond)
{
	if (IsOpen())
	{
		SendControl(FString::Printf(TEXT("NETSPEED %d"), BytesPerSecond));
	}
}

// =================================================================================================
// Actor channels
// =================================================================================================

bool FUE2NetConnection::ReadObject(FUE2BitReader& R, FUE2ObjectRef& Out)
{
	Out = FUE2ObjectRef();
	if (R.ReadBit())
	{
		// an actor channel (0 = None)
		Out.Channel = int32(R.ReadInt(MaxChannels));
		return !R.IsError();
	}
	const int32 Index = int32(R.ReadInt(uint32(FMath::Max(1, PackageMap.GetMaxObjectIndex()))));
	if (R.IsError())
	{
		return false;
	}
	PackageMap.IndexToObject(Index, Out);
	return true;
}

bool FUE2NetConnection::ReadStruct(FUE2BitReader& R, FUSStruct* Struct, FUE2NetValue& Out, int32 Depth)
{
	const FName StructName = Struct->Name;
	if (StructName == TEXT("Vector"))
	{
		Out.Kind = FUE2NetValue::EKind::Vector;
		Out.V = ReadCompressedVector(R);
		return !R.IsError();
	}
	if (StructName == TEXT("Rotator"))
	{
		Out.Kind = FUE2NetValue::EKind::Rotator;
		Out.R = ReadCompressedRotator(R);
		return !R.IsError();
	}
	if (StructName == TEXT("Plane"))
	{
		Out.Kind = FUE2NetValue::EKind::Plane;
		const float X = R.ReadInt16(), Y = R.ReadInt16(), Z = R.ReadInt16(), W = R.ReadInt16();
		Out.P = FVector4f(X, Y, Z, W);
		return !R.IsError();
	}
	if (StructName == TEXT("CompressedPosition"))
	{
		// Pawn.PawnPosition (UT2004): location and velocity as compressed vectors, the rotation as pitch and yaw
		// bytes only (no roll, no "present" bits). Worked out from captured traffic: see Docs/UE2Net.md.
		Out.Kind = FUE2NetValue::EKind::Struct;
		FUE2NetValue Loc, Rot, Vel;
		Loc.Kind = FUE2NetValue::EKind::Vector;
		Loc.V = ReadCompressedVector(R);
		Rot.Kind = FUE2NetValue::EKind::Rotator;
		const int32 Pitch = int32(R.ReadByte()) << 8;
		const int32 Yaw = int32(R.ReadByte()) << 8;
		Rot.R = FIntVector(Pitch, Yaw, 0);
		Vel.Kind = FUE2NetValue::EKind::Vector;
		Vel.V = ReadCompressedVector(R);
		Out.Members.Emplace(TEXT("Location"), MoveTemp(Loc));
		Out.Members.Emplace(TEXT("Rotation"), MoveTemp(Rot));
		Out.Members.Emplace(TEXT("Velocity"), MoveTemp(Vel));
		return !R.IsError();
	}
	if (StructName == TEXT("Quat"))
	{
		// Object.Quat (the ONS vehicles' KRigidBody state structs: HoverCraftState, SCarState, CopterState,
		// TreadCraftState): only X, Y and Z go over the wire as floats, W is implied (a unit quaternion, W >= 0).
		// Worked out from captured ONSHoverBike bunches: see Docs/UE2Net.md.
		Out.Kind = FUE2NetValue::EKind::Struct;
		FUE2NetValue X, Y, Z, W;
		X.Kind = Y.Kind = Z.Kind = W.Kind = FUE2NetValue::EKind::Float;
		X.F = R.ReadFloat();
		Y.F = R.ReadFloat();
		Z.F = R.ReadFloat();
		W.F = FMath::Sqrt(FMath::Max(0.f, 1.f - X.F * X.F - Y.F * Y.F - Z.F * Z.F));
		Out.Members.Emplace(TEXT("X"), MoveTemp(X));
		Out.Members.Emplace(TEXT("Y"), MoveTemp(Y));
		Out.Members.Emplace(TEXT("Z"), MoveTemp(Z));
		Out.Members.Emplace(TEXT("W"), MoveTemp(W));
		return !R.IsError();
	}
	// any other struct: its members in field order (the struct's own, then its super struct's)
	Out.Kind = FUE2NetValue::EKind::Struct;
	if (Settings.bTraceBits)
	{
		static TSet<FName> Logged;
		if (!Logged.Contains(StructName))
		{
			Logged.Add(StructName);
			FString Layout;
			for (FUSStruct* S = Struct; S; S = S->GetSuperStruct())
			{
				for (FUSField* F = S->Children; F; F = F->Next)
				{
					if (IsProperty(F))
					{
						const FUSProperty* P = static_cast<const FUSProperty*>(F);
						Layout += FString::Printf(TEXT(" %s:%d%s%s"), *P->Name.ToString(), int32(P->Type), P->Struct ? *(TEXT("/") + P->Struct->Name.ToString()) : TEXT(""),
							PackageMap.ObjectToIndex(F) == INDEX_NONE ? TEXT("(unmapped)") : TEXT(""));
					}
				}
			}
			Trace(FString::Printf(TEXT("struct %s:%s"), *StructName.ToString(), *Layout));
		}
	}
	for (FUSStruct* S = Struct; S; S = S->GetSuperStruct())
	{
		int32 Guard = 0;
		for (FUSField* F = S->Children; F && Guard < 10000; F = F->Next, ++Guard)
		{
			if (!IsProperty(F) || PackageMap.ObjectToIndex(F) == INDEX_NONE)
			{
				continue;
			}
			FUSProperty* P = static_cast<FUSProperty*>(F);
			for (int32 i = 0; i < P->ArrayDim; ++i)
			{
				FUE2NetValue Member;
				if (!ReadProperty(R, P, Member, Depth + 1))
				{
					return false;
				}
				const FName MemberName = P->ArrayDim > 1 ? FName(*FString::Printf(TEXT("%s[%d]"), *P->Name.ToString(), i)) : P->Name;
				Out.Members.Emplace(MemberName, MoveTemp(Member));
			}
		}
	}
	return !R.IsError();
}

bool FUE2NetConnection::ReadProperty(FUE2BitReader& R, FUSProperty* Prop, FUE2NetValue& Out, int32 Depth)
{
	if (Depth > 8)
	{
		return false;
	}
	switch (Prop->Type)
	{
	case EUSType::Byte:
	{
		// enums take just enough bits for their values
		const int32 Bits = Prop->Enum ? UE2CeilLogTwo(uint32(Prop->Enum->Names.Num())) : 8;
		Out.Kind = FUE2NetValue::EKind::Byte;
		Out.I = int32(ReadRawBits(R, Bits));
		break;
	}
	case EUSType::Int:
		Out.Kind = FUE2NetValue::EKind::Int;
		Out.I = R.ReadInt32();
		break;
	case EUSType::Bool:
		Out.Kind = FUE2NetValue::EKind::Bool;
		Out.I = R.ReadBit() ? 1 : 0;
		break;
	case EUSType::Float:
		Out.Kind = FUE2NetValue::EKind::Float;
		Out.F = R.ReadFloat();
		break;
	case EUSType::Object:
	case EUSType::Class:
		Out.Kind = FUE2NetValue::EKind::Object;
		if (!ReadObject(R, Out.Obj))
		{
			return false;
		}
		break;
	case EUSType::Name:
	{
		Out.Kind = FUE2NetValue::EKind::Name;
		const int32 Index = int32(R.ReadInt(uint32(PackageMap.GetMaxNameIndex() + 1)));
		PackageMap.IndexToName(Index, Out.N);
		break;
	}
	case EUSType::Str:
		Out.Kind = FUE2NetValue::EKind::Str;
		Out.S = R.ReadString();
		break;
	case EUSType::Struct:
		if (!Prop->Struct || !ReadStruct(R, Prop->Struct, Out, Depth))
		{
			return false;
		}
		break;
	default:
		// dynamic arrays, delegates and pointers do not replicate in UE2
		return false;
	}
	return !R.IsError();
}

void FUE2NetConnection::ReceivedActorBunch(FInBunch& Bunch)
{
	FUE2BitReader& R = Bunch.Data;
	FUE2NetActor* Actor = Actors.Find(Bunch.Channel);
	bool bJustOpened = false;
	if (!Actor)
	{
		if (!Bunch.bOpen)
		{
			return;
		}
		FUE2NetActor NewActor;
		NewActor.Channel = Bunch.Channel;
		if (!ReadObject(R, NewActor.Archetype) || !NewActor.Archetype.IsStatic())
		{
			// objects of packages we told the server to SKIP cannot be named: the server sends them as a channel
			// reference that is not open (seen: 511); such an actor (e.g. a placed actor of a map we skipped) is ignored
			NewActor.bBroken = true;
			if (!NewActor.Archetype.IsDynamic() || Actors.Contains(NewActor.Archetype.Channel))
			{
				++BunchErrors;
				UE_LOG(LogUE2Net, Warning, TEXT("Actor channel %d: initial bunch without a class or map actor"), Bunch.Channel);
			}
			else
			{
				UE_LOG(LogUE2Net, Verbose, TEXT("Actor channel %d: class or map actor from a skipped package"), Bunch.Channel);
			}
		}
		else
		{
			FString Err;
			const bool bIsClass = PackageMap.GetObjectClassName(NewActor.Archetype) == TEXT("Class");
			if (bIsClass)
			{
				// a spawned actor: class, then where to spawn it
				NewActor.Class = PackageMap.GetClass(NewActor.Archetype, &Err);
				NewActor.SpawnLocation = ReadCompressedVector(R);
				bool bInitialRotation = false;
				if (NewActor.Class)
				{
					FUSPropRef Ref;
					if (Ref.Resolve(NewActor.Class, TEXT("bNetInitialRotation")))
					{
						bInitialRotation = Ref.Get<uint32>(NewActor.Class->GetDefaultObject()) != 0;
					}
				}
				if (bInitialRotation)
				{
					NewActor.SpawnRotation = ReadCompressedRotator(R);
				}
			}
			else
			{
				// an actor placed in the map
				NewActor.bStatic = true;
				NewActor.Class = PackageMap.GetInstanceClass(NewActor.Archetype, &Err);
			}
			if (NewActor.Class)
			{
				NewActor.ClassPath = NewActor.Class->Linker ? NewActor.Class->Linker->PackageName.ToString() + TEXT(".") + NewActor.Class->GetName() : NewActor.Class->GetName();
				NewActor.NetCache = PackageMap.GetClassNetCache(NewActor.Class);
			}
			if (!NewActor.NetCache)
			{
				NewActor.bBroken = true;
				UE_LOG(LogUE2Net, Warning, TEXT("Actor channel %d (%s): class not available%s%s"), Bunch.Channel, *PackageMap.GetObjectPath(NewActor.Archetype),
					Err.IsEmpty() ? TEXT("") : TEXT(": "), *Err);
			}
		}
		Actor = &Actors.Add(Bunch.Channel, MoveTemp(NewActor));
		bJustOpened = true;
		++ActorsOpened;
		if (Settings.bTrace && !Actor->Archetype.IsDynamic())
		{
			if (Actor->bStatic)
			{
				Trace(FString::Printf(TEXT("[ch %d] open map actor %s (%s)"), Actor->Channel, *PackageMap.GetObjectPath(Actor->Archetype), *Actor->ClassPath));
			}
			else
			{
				Trace(FString::Printf(TEXT("[ch %d] open %s at (%.0f, %.0f, %.0f) rotation (%d, %d, %d)"), Actor->Channel, *PackageMap.GetObjectPath(Actor->Archetype),
					Actor->SpawnLocation.X, Actor->SpawnLocation.Y, Actor->SpawnLocation.Z, Actor->SpawnRotation.X, Actor->SpawnRotation.Y, Actor->SpawnRotation.Z));
			}
		}
	}
	++Actor->NumBunches;
	if (Actor->bBroken)
	{
		return;
	}

	TArray<FString> Changed;
	const FUE2ClassNetCache* Cache = Actor->NetCache;
	if (Settings.bTraceBits)
	{
		Trace(FString::Printf(TEXT("[ch %d] bits %s"), Actor->Channel, *R.PeekBitString(4096)));
	}
	const FString ShortClass = Actor->Class ? Actor->Class->GetName() : FString();
	while (!R.AtEnd() && IsOpen())
	{
		const int32 RepIndex = int32(R.ReadInt(uint32(Cache->GetMaxIndex())));
		FUSField* Field = R.IsError() ? nullptr : Cache->GetFromIndex(RepIndex);
		if (!Field)
		{
			++BunchErrors;
			UE_LOG(LogUE2Net, Warning, TEXT("[ch %d] %s: bad replication index %d (max %d)"), Actor->Channel, *ShortClass, RepIndex, Cache->GetMaxIndex());
			break;
		}
		if (IsProperty(Field))
		{
			FUSProperty* Prop = static_cast<FUSProperty*>(Field);
			int32 Element = 0;
			if (Prop->ArrayDim != 1)
			{
				// static arrays: the element index as one byte
				Element = R.ReadByte();
			}
			FUE2NetValue Value;
			if (!ReadProperty(R, Prop, Value) || Element >= Prop->ArrayDim)
			{
				++BunchErrors;
				UE_LOG(LogUE2Net, Warning, TEXT("[ch %d] %s: cannot read %s"), Actor->Channel, *ShortClass, *Prop->Name.ToString());
				break;
			}
			const FString Key = Prop->ArrayDim != 1 ? FString::Printf(TEXT("%s[%d]"), *Prop->Name.ToString(), Element) : Prop->Name.ToString();
			++PropertiesReceived;
			if (Settings.bTrace)
			{
				Trace(FString::Printf(TEXT("[ch %d] %s.%s = %s"), Actor->Channel, *ShortClass, *Key, *ValueToString(Value)));
			}
			Actor->Properties.Add(Key, MoveTemp(Value));
			Changed.Add(Key);
		}
		else if (IsFunction(Field))
		{
			FUSFunction* Function = static_cast<FUSFunction*>(Field);
			if (Function->Name.ToString().StartsWith(TEXT("Server")))
			{
				// servers never call client -> server functions on us: the bunch is misread from here on (a field
				// whose wire format we do not know yet); drop the rest of it rather than act on garbage
				++BunchErrors;
				UE_LOG(LogUE2Net, Warning, TEXT("[ch %d] %s: misread bunch (got the server function %s), rest skipped"), Actor->Channel, *ShortClass, *Function->Name.ToString());
				break;
			}
			TArray<TPair<FName, FUE2NetValue>> Params;
			bool bOk = true;
			int32 Guard = 0;
			for (FUSField* F = Function->Children; F && Guard < 1000; F = F->Next, ++Guard)
			{
				if (!IsProperty(F))
				{
					break;
				}
				FUSProperty* P = static_cast<FUSProperty*>(F);
				if ((P->PropertyFlags & (UE2CPF_Parm | UE2CPF_ReturnParm)) != UE2CPF_Parm)
				{
					break;
				}
				if (PackageMap.ObjectToIndex(P) == INDEX_NONE)
				{
					continue;
				}
				FUE2NetValue Value;
				// bools are always one bit; other parameters have a "present" bit (left out when zero / None)
				if (P->Type == EUSType::Bool || R.ReadBit())
				{
					if (!ReadProperty(R, P, Value))
					{
						bOk = false;
						break;
					}
				}
				Params.Emplace(P->Name, MoveTemp(Value));
			}
			if (!bOk || R.IsError())
			{
				++BunchErrors;
				UE_LOG(LogUE2Net, Warning, TEXT("[ch %d] %s: cannot read the parameters of %s"), Actor->Channel, *ShortClass, *Function->Name.ToString());
				break;
			}
			++FunctionsReceived;
			HandleFunction(*Actor, Function, Params);
		}
		else
		{
			++BunchErrors;
			break;
		}
	}
	if (!IsOpen())
	{
		return;
	}
	if (bJustOpened || Changed.Num() > 0)
	{
		// OpenUT2k4Validator's challenge actor (Net/UE2NetValidator.h)
		UE2NetValidator::ActorChanged(*this, *Actor);
	}
	if (bJustOpened)
	{
		if (OnActorOpened)
		{
			OnActorOpened(*Actor);
		}
	}
	else if (Changed.Num() > 0 && OnActorUpdated)
	{
		OnActorUpdated(*Actor, Changed);
	}
}

void FUE2NetConnection::HandleFunction(FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	const FString Name = Function->Name.ToString();
	if (Settings.bTrace)
	{
		TArray<FString> Parts;
		for (const TPair<FName, FUE2NetValue>& P : Params)
		{
			if (P.Value.Kind != FUE2NetValue::EKind::None)
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), *P.Key.ToString(), *ValueToString(P.Value)));
			}
		}
		Trace(FString::Printf(TEXT("[ch %d] call %s.%s(%s)"), Actor.Channel, Actor.Class ? *Actor.Class->GetName() : TEXT("?"), *Name, *FString::Join(Parts, TEXT(", "))));
	}
	if (Name == TEXT("ClientValidate") && Params.Num() > 0)
	{
		// CD key check (servers that validate keys through the master server): answered like the OldUnreal client,
		// from the CD key or, without one, from the per-install identity
		FUE2NetValue R;
		R.Kind = FUE2NetValue::EKind::Str;
		R.S = UE2NetCDKey::ComputeResponse(Params[0].Value.S);
		SendFunction(Actor.Channel, TEXT("ServerValidationResponse"), { R });
		UE_LOG(LogUE2Net, Log, TEXT("Answered the server's CD key challenge"));
	}
	else if (Name == TEXT("ClientTravel") && Params.Num() > 0)
	{
		UE_LOG(LogUE2Net, Log, TEXT("Server sends us to %s (map change)"), *Params[0].Value.S);
	}
	if (OnFunctionCall)
	{
		OnFunctionCall(Actor, Function, Params);
	}
}

void FUE2NetConnection::CloseActor(int32 Channel)
{
	FUE2NetActor Actor;
	if (Actors.RemoveAndCopyValue(Channel, Actor))
	{
		++ActorsClosed;
		if (Settings.bTrace)
		{
			Trace(FString::Printf(TEXT("[ch %d] close %s"), Channel, Actor.Class ? *Actor.Class->GetName() : *PackageMap.GetObjectPath(Actor.Archetype)));
		}
		if (OnActorClosed)
		{
			OnActorClosed(Actor);
		}
	}
}

// =================================================================================================
// Client -> server function calls
// =================================================================================================

bool FUE2NetConnection::WriteProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue& Value)
{
	switch (Prop->Type)
	{
	case EUSType::Byte:
		WriteRawBits(W, uint32(Value.I), Prop->Enum ? UE2CeilLogTwo(uint32(Prop->Enum->Names.Num())) : 8);
		return true;
	case EUSType::Int:
		W.WriteInt32(Value.I);
		return true;
	case EUSType::Bool:
		W.WriteBit(Value.I != 0);
		return true;
	case EUSType::Float:
		W.WriteFloat(Value.F);
		return true;
	case EUSType::Str:
		W.WriteString(Value.S);
		return true;
	case EUSType::Object:
	case EUSType::Class:
		if (Value.Obj.IsStatic())
		{
			const int32 Index = PackageMap.ObjectToIndex(Value.Obj.PackageIndex, Value.Obj.ExportIndex);
			if (Index != INDEX_NONE)
			{
				W.WriteBit(false);
				W.WriteInt(uint32(Index), uint32(FMath::Max(1, PackageMap.GetMaxObjectIndex())));
				return true;
			}
		}
		W.WriteBit(true);
		W.WriteInt(uint32(Value.Obj.Channel), MaxChannels);
		return true;
	case EUSType::Name:
		// only None is supported for now (no reverse name lookup yet)
		W.WriteInt(uint32(PackageMap.GetMaxNameIndex()), uint32(PackageMap.GetMaxNameIndex() + 1));
		return true;
	case EUSType::Struct:
		if (Prop->Struct && Prop->Struct->Name == TEXT("Vector"))
		{
			WriteCompressedVector(W, Value.V);
			return true;
		}
		if (Prop->Struct && Prop->Struct->Name == TEXT("Rotator"))
		{
			WriteCompressedRotator(W, Value.R);
			return true;
		}
		{
			// other structs (Plane, CompressedPosition, mods' own: NewNet's ReplicatedRotator / ReplicatedVector...)
			// member by member, as the server codec writes them
			FUE2WireContext Ctx;
			Ctx.Map = &PackageMap;
			return UE2Codec::WriteProperty(W, Prop, &Value, Ctx);
		}
	default:
		return false;
	}
}

FUSFunction* FUE2NetConnection::FindReplicatedFunction(int32 Channel, FName FunctionName) const
{
	const FUE2NetActor* Actor = Actors.Find(Channel);
	if (!Actor || !Actor->NetCache)
	{
		return nullptr;
	}
	// the declaration highest up the class tree (the one a UE2 client sends)
	FUSFunction* Function = nullptr;
	for (const FUE2ClassNetCache* C = Actor->NetCache; C; C = C->Super)
	{
		for (FUSField* F : C->Fields)
		{
			if (F->Name == FunctionName && IsFunction(F))
			{
				Function = static_cast<FUSFunction*>(F);
			}
		}
	}
	return Function;
}

bool FUE2NetConnection::SendFunction(int32 Channel, FName FunctionName, const TArray<FUE2NetValue>& Params)
{
	FUE2NetActor* Actor = Actors.Find(Channel);
	if (!Actor || !Actor->NetCache || State != EState::Joined)
	{
		return false;
	}
	FUSFunction* Function = FindReplicatedFunction(Channel, FunctionName);
	if (!Function)
	{
		UE_LOG(LogUE2Net, Warning, TEXT("%s has no replicated function %s"), *Actor->ClassPath, *FunctionName.ToString());
		return false;
	}
	FOutBunch B;
	B.Channel = Channel;
	B.ChType = ChActor;
	B.bReliable = (Function->FunctionFlags & UE2FUNC_NetReliable) != 0;
	B.Data.WriteInt(uint32(Actor->NetCache->GetIndexOf(Function)), uint32(Actor->NetCache->GetMaxIndex()));
	int32 ParmIndex = 0;
	for (FUSField* F = Function->Children; F; F = F->Next)
	{
		if (!IsProperty(F))
		{
			break;
		}
		FUSProperty* P = static_cast<FUSProperty*>(F);
		if ((P->PropertyFlags & (UE2CPF_Parm | UE2CPF_ReturnParm)) != UE2CPF_Parm)
		{
			break;
		}
		const FUE2NetValue* V = Params.IsValidIndex(ParmIndex) ? &Params[ParmIndex] : nullptr;
		++ParmIndex;
		if (PackageMap.ObjectToIndex(P) == INDEX_NONE)
		{
			continue;
		}
		if (P->Type == EUSType::Bool)
		{
			B.Data.WriteBit(V && V->I != 0);
			continue;
		}
		// UE2 UNetDriver::ProcessRemoteFunction: a parameter equal to zero is not sent (one 0 bit), the receiver zeroes it
		const bool bPresent = V && !IsZeroValue(*V);
		B.Data.WriteBit(bPresent);
		if (bPresent && !WriteProperty(B.Data, P, *V))
		{
			UE_LOG(LogUE2Net, Warning, TEXT("%s: cannot send parameter %s"), *FunctionName.ToString(), *P->Name.ToString());
			return false;
		}
	}
	if (Settings.bTrace)
	{
		Trace(FString::Printf(TEXT("[ch %d] send %s.%s (index %d of %d, %lld bits)"), Channel, *Actor->ClassPath, *FunctionName.ToString(),
			Actor->NetCache->GetIndexOf(Function), Actor->NetCache->GetMaxIndex(), B.Data.GetNumBits()));
	}
	QueueBunch(MoveTemp(B));
	return true;
}

int32 FUE2NetConnection::FindPlayerControllerChannel() const
{
	// only the owning client gets a PlayerController replicated, so ours is the one in the list
	for (const TPair<int32, FUE2NetActor>& KV : Actors)
	{
		for (const FUSClass* C = KV.Value.Class; C; C = C->GetSuperClass())
		{
			if (C->Name == TEXT("PlayerController"))
			{
				return KV.Key;
			}
		}
	}
	return 0;
}

// =================================================================================================
// Logging
// =================================================================================================

void FUE2NetConnection::Trace(const FString& Line) const
{
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetTrace: %s"), *Line);
}

FString FUE2NetConnection::ValueToString(const FUE2NetValue& Value)
{
	switch (Value.Kind)
	{
	case FUE2NetValue::EKind::Byte:
	case FUE2NetValue::EKind::Int: return FString::FromInt(Value.I);
	case FUE2NetValue::EKind::Bool: return Value.I ? TEXT("True") : TEXT("False");
	case FUE2NetValue::EKind::Float: return FString::SanitizeFloat(Value.F);
	case FUE2NetValue::EKind::Name: return Value.N.ToString();
	case FUE2NetValue::EKind::Str: return FString::Printf(TEXT("\"%s\""), *CleanText(Value.S));
	case FUE2NetValue::EKind::Vector: return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Value.V.X, Value.V.Y, Value.V.Z);
	case FUE2NetValue::EKind::Rotator: return FString::Printf(TEXT("(%d, %d, %d)"), Value.R.X, Value.R.Y, Value.R.Z);
	case FUE2NetValue::EKind::Plane: return FString::Printf(TEXT("(%g, %g, %g, %g)"), Value.P.X, Value.P.Y, Value.P.Z, Value.P.W);
	case FUE2NetValue::EKind::Object:
		if (Value.Obj.IsDynamic())
		{
			const FUE2NetActor* A = Actors.Find(Value.Obj.Channel);
			return FString::Printf(TEXT("<ch %d%s%s>"), Value.Obj.Channel, A && A->Class ? TEXT(" ") : TEXT(""), A && A->Class ? *A->Class->GetName() : TEXT(""));
		}
		return PackageMap.GetObjectPath(Value.Obj);
	case FUE2NetValue::EKind::Struct:
	{
		TArray<FString> Parts;
		for (const TPair<FName, FUE2NetValue>& M : Value.Members)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *M.Key.ToString(), *ValueToString(M.Value)));
		}
		return FString::Printf(TEXT("{%s}"), *FString::Join(Parts, TEXT(", ")));
	}
	default: return TEXT("None");
	}
}

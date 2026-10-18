#pragma once

#include "CoreMinimal.h"
#include "UE2NetBits.h"
#include "UE2NetConnection.h"
#include "UE2NetPackageMap.h"
#include "UE2ServerCodec.h"

class FInternetAddr;
class FSocket;
class FUE2Server;
struct FUE2SvActor;
struct FUSClass;
struct FUSFunction;
struct FUSProperty;

/**
 * One client of our UE2 (UT2004 3369 / 3374 protocol) server: the server side of FUE2NetConnection. See
 * Docs/UE2Net.md "Hosting: the UE2 server".
 *
 *   packets -> acks + bunches; reliable bunches resent on a nak or after AckTimeout; at most 512 bytes a packet
 *   control channel: HELLO -> CHALLENGE, AUTH / NETSPEED / LOGIN -> USES..., DLMGR, WELCOME (or FAILCODE / FAILURE),
 *                    HAVE / SKIP -> JOIN (the server then creates the player)
 *   actor channels: opened by us, one per replicated actor (FUE2SvActor), properties sent when they differ from what
 *                   this client has (a per-channel shadow of the encoded bits), function calls both ways
 *
 * The socket belongs to FUE2Server (one UDP port for every client); packets are routed here by sender address.
 */
class FUE2ServerConnection
{
public:
	enum class EState : uint8
	{
		/** Waiting for HELLO. */
		Hello,
		/** CHALLENGE sent, waiting for LOGIN. */
		Challenged,
		/** WELCOME sent, waiting for JOIN (the client loads the map). */
		Welcomed,
		/** Playing / spectating: actors replicate. */
		Joined,
		Closed,
	};

	FUE2ServerConnection(FUE2Server& InServer, const TSharedRef<FInternetAddr>& InAddr, int32 InId);
	~FUE2ServerConnection();

	int32 GetId() const { return Id; }
	EState GetState() const { return State; }
	bool IsJoined() const { return State == EState::Joined; }
	bool IsClosed() const { return State == EState::Closed; }
	const TSharedRef<FInternetAddr>& GetAddr() const { return Addr; }
	FString GetAddrString() const;

	void ReceivedRawPacket(const uint8* Data, int32 Count, double Now);
	/** Resends, keep-alives, timeouts. */
	void Tick(double Now);
	/** Sends everything queued (acks, bunches). */
	void Flush(double Now);
	/** Closes the control channel (kick / server shutdown / map change), with an optional FAILURE text first. */
	void Close(const FString& Reason, const FString& FailCode = FString(), const FString& FailText = FString());
	void SendControl(const FString& Text);

	// ---- login data ----
	FString PlayerName;
	/** LOGIN URL options ("?Name=..?Team=1..."). */
	FString LoginOptions;
	bool bSpectatorOnly = false;
	/** AUTH: the player id the stock server derives from HASH (MD5 of the salt + HASH), for kick / ban lists. Never logged. */
	FString PlayerHashId;
	/** AUTH HASH as the client sent it (the master server uplink's client checks send it back to the master). Never logged. */
	FString AuthHash;
	int32 NetSpeed = 10000;
	/** Round trip in ms (the client's ServerUpdatePing). */
	int32 PingMs = 0;
	int32 ClientVersion = 0;

	// ---- replication (driven by FUE2Server) ----
	struct FShadow
	{
		TArray<uint8> Bits;
		int32 NumBits = 0;
	};
	struct FActorChannel
	{
		int32 Channel = 0;
		int32 ActorId = 0;
		/** The open bunch was acknowledged: unreliable updates may go out (the client drops them before the open). */
		bool bOpenAcked = false;
		bool bClosing = false;
		/** bTearOff was sent: no more updates, the channel closes once that is acknowledged. */
		bool bTornOff = false;
		/** bNetTemporary: opened, closed right after the initial bunch. */
		bool bTemporary = false;
		double LastUpdate = -1.0;
		double LastRelevant = 0.0;
		/** Encoded value this client has, per property key ("Health", "TeamSymbols[1]"). */
		TMap<FString, FShadow> Shadow;
		int32 OutstandingReliable = 0;
	};
	FActorChannel* FindChannelOfActor(int32 ActorId);
	const TMap<int32, FActorChannel>& GetActorChannels() const { return ChannelsByActor; }
	/** Actor id of an open actor channel (incoming object references), or 0. */
	int32 ActorOfChannel(int32 Channel) const;
	/** Channel of an actor for this connection's object references: 0 until its channel is open. */
	int32 ChannelOfActor(int32 ActorId) const;

	/** Bytes queued for this client so far (bunch data and headers): what the bandwidth budget is charged with. */
	int64 QueuedBytes = 0;
	/** UE2 NETSPEED budget: bytes this client may still get now (refilled at NetSpeed per second, up to half a second). */
	double Budget = 0.0;
	double BudgetTime = 0.0;
	int64 BudgetCharged = 0;
	/** Line of sight results per actor (UE2 relevancy). */
	struct FVisibility { double LastCheck = -100.0; double LastVisible = -100.0; };
	TMap<int32, FVisibility> Visibility;
	int32 SaturatedTicks = 0;
	/** Actors this client has for good (bNetTemporary ones sent once, torn off ones): never opened again. */
	TSet<int32> FinishedActors;
	/** Replicates one actor: opens its channel if needed and sends the changed properties. False when out of channels. */
	bool ReplicateActor(FUE2SvActor& Actor, double Now, bool bOwner);
	/** Closes the actor's channel (no longer relevant, or destroyed). */
	void CloseActorChannel(int32 ActorId);
	/** Calls a replicated function on an actor's channel (opening it first when needed). */
	bool CallFunction(FUE2SvActor& Actor, FName FunctionName, const TArray<FUE2NetValue>& Params, bool bOwner, bool bForceUnreliable = false);

	FUE2PackageMap& GetPackageMap() { return PackageMap; }
	FUE2WireContext MakeContext();

	// ---- statistics ----
	int32 PacketsIn = 0;
	int32 PacketsOut = 0;
	int64 BytesIn = 0;
	int64 BytesOut = 0;
	int32 BunchErrors = 0;
	int32 ActorsOpened = 0;
	int32 PropertiesSent = 0;
	int32 CallsSent = 0;
	int32 CallsReceived = 0;
	double JoinTime = 0.0;

private:
	static constexpr uint32 MaxPacketId = 16384;
	static constexpr uint32 MaxChannels = 1023;
	static constexpr uint32 MaxChSequence = 1024;
	static constexpr uint32 MaxChType = 8;
	static constexpr int32 MaxPacketBytes = 512;

	struct FOutBunch
	{
		int32 Channel = 0;
		uint8 ChType = 0;
		bool bOpen = false;
		bool bClose = false;
		bool bReliable = false;
		int32 Sequence = 0;
		FUE2BitWriter Data;
		int32 PacketId = -1;
		double SentTime = 0.0;
		/** Unreliable property keys of this bunch (resent on a nak): actor id + keys. */
		int32 ActorId = 0;
		TArray<FString> UnreliableKeys;
	};
	struct FInBunch
	{
		int32 Channel = 0;
		uint8 ChType = 0;
		bool bOpen = false;
		bool bClose = false;
		bool bReliable = false;
		int32 Sequence = 0;
		FUE2BitReader Data;
	};
	struct FChannelInfo
	{
		uint8 ChType = 0;
		TArray<FInBunch> Queued;
	};

	void ReceivedAck(int32 AckId);
	void ReceivedNak(int32 NakId);
	void ReceivedBunch(FInBunch& Bunch);
	void ProcessBunch(FInBunch& Bunch);
	void ReceivedText(const FString& Text);
	void ReceivedActorBunch(FInBunch& Bunch);
	void QueueBunch(FOutBunch&& Bunch);
	void WriteBunchHeader(FUE2BitWriter& W, const FOutBunch& B) const;
	void SendPacket(const FUE2BitWriter& Packet);
	void SetClosed(const FString& Reason);
	int32 AllocateChannel();
	/** Encodes one property element; false when it cannot be written. */
	bool EncodeProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue* Value);

	FUE2Server& Server;
	TSharedRef<FInternetAddr> Addr;
	int32 Id = 0;
	EState State = EState::Hello;
	int32 Challenge = 0;
	FUE2PackageMap PackageMap;
	TArray<FString> SkippedPackages;
	/** Master package index (FUE2Server's USES list) -> index in PackageMap. */
	TArray<int32> MasterToLocal;

	int32 InPacketId = -1;
	int32 OutPacketId = 0;
	int32 OutAckPacketId = -1;
	/** Send time of recent packets (by id modulo 256): the round trip from their acks (PingMs). */
	double PacketSendTimes[256] = {};
	double SmoothedRtt = -1.0;
	TArray<int32> PendingAcks;
	TArray<FOutBunch> SendQueue;
	TArray<FOutBunch> OutReliableBunches;
	/** Unreliable bunches with property keys, by packet id (nak: forget those shadow values). */
	TMap<int32, TArray<TPair<int32, TArray<FString>>>> UnreliableByPacket;
	int32 InReliable[MaxChannels] = {};
	int32 OutReliable[MaxChannels] = {};
	TMap<int32, FChannelInfo> Channels;
	TMap<int32, FActorChannel> ChannelsByActor;
	TMap<int32, int32> ActorByChannel;
	/** Channels closed recently (index -> when): not reused until the close is surely acknowledged. */
	TMap<int32, double> RecentlyClosed;
	int32 NextChannel = 1;

	double LastReceiveTime = 0.0;
	double LastSendTime = 0.0;
	double CreateTime = 0.0;
	double WelcomeTime = 0.0;
	FString CloseReason;

	friend class FUE2Server;
};

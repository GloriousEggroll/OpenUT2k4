#pragma once

#include "CoreMinimal.h"
#include "UE2NetBits.h"
#include "UE2NetPackageMap.h"

class FSocket;
class FInternetAddr;
struct FUSClass;
struct FUSFunction;
struct FUSProperty;
struct FUSStruct;

/**
 * A client connection to a UT2004 (UE2, engine version 3369 / 3374) server: the UE2 net driver re-implemented as a
 * standalone UDP connection (UE5's net driver is not involved). See Docs/UE2Net.md for the protocol.
 *
 *   packets  -> acks + bunches (UNetConnection)
 *   bunches  -> channels: 0 = control (text messages: the login handshake), actor channels (one replicated actor
 *               each: the initial bunch names the class / map actor, then replicated properties and function calls),
 *               file channels (package downloads, opened by us: OpenFileChannel, Net/UE2NetDownload.h)
 *
 * Game thread only. Tick() it every frame; it never blocks except for the host name lookup in Connect().
 */

/** A replicated value decoded from a bunch (one element of a property, or a function parameter). */
struct FUE2NetValue
{
	enum class EKind : uint8 { None, Byte, Int, Bool, Float, Object, Name, Str, Vector, Rotator, Plane, Struct };
	EKind Kind = EKind::None;
	/** Byte / Int / Bool. */
	int32 I = 0;
	float F = 0.f;
	/** Vector (UE2 units, integer precision on the wire). */
	FVector3f V = FVector3f::ZeroVector;
	/** Rotator (UE2 65536 units per turn; 256 unit precision on the wire). */
	FIntVector R = FIntVector::ZeroValue;
	/** Plane. */
	FVector4f P = FVector4f(0, 0, 0, 0);
	FString S;
	FName N;
	FUE2ObjectRef Obj;
	/** Struct members in serialization order. */
	TArray<TPair<FName, FUE2NetValue>> Members;
};

/** One actor the server replicates to us (one actor channel). */
struct FUE2NetActor
{
	int32 Channel = 0;
	/** The class (spawned actors) or the map actor (actors placed in the map) the initial bunch named. */
	FUE2ObjectRef Archetype;
	/** True for actors placed in the map (Archetype is the map's actor export). */
	bool bStatic = false;
	FUSClass* Class = nullptr;
	/** "Package.Class". */
	FString ClassPath;
	/** Spawn location / rotation of the initial bunch (spawned actors only). */
	FVector3f SpawnLocation = FVector3f::ZeroVector;
	FIntVector SpawnRotation = FIntVector::ZeroValue;
	/** Latest replicated values by "Property" or "Property[Index]". Role here is the server's role (our RemoteRole). */
	TMap<FString, FUE2NetValue> Properties;
	const FUE2ClassNetCache* NetCache = nullptr;
	/** The class could not be resolved: bunches of this channel are skipped. */
	bool bBroken = false;
	int32 NumBunches = 0;
};

class FUE2NetConnection
{
public:
	struct FSettings
	{
		FString Host;
		int32 Port = 7777;
		FString PlayerName = TEXT("Player");
		/** Join as a spectator (?SpectatorOnly=1). */
		bool bSpectator = true;
		/** Bytes per second we ask the server to send at most (NETSPEED; 20000 is the 3374 ConfiguredInternetSpeed). */
		int32 NetSpeed = 20000;
		/** Extra login URL options ("?Character=...?Team=..."). */
		FString ExtraOptions;
		/** Send a stats USERNAME / PASSWORD in AUTH (UT2004 "Track Stats"). Off by default, like UT2004. */
		bool bTrackStats = false;
		/** Stats account (sent when bTrackStats, both are set and the server has STATS=1). */
		FString StatsUsername;
		FString StatsPassword;
		/** -UE2NetTrace: log every decoded actor, property and function call. */
		bool bTrace = false;
		/** -UE2NetTraceBits: also log the raw bits of every actor bunch (protocol debugging). */
		bool bTraceBits = false;
	};

	enum class EState : uint8
	{
		Idle,
		/** HELLO sent, waiting for CHALLENGE. */
		Connecting,
		/** LOGIN sent, receiving USES, waiting for WELCOME. */
		LoggingIn,
		/** WELCOME received: the map is known; the game loads it and then calls SendJoin(). */
		Welcomed,
		/** JOIN sent: actors arrive. */
		Joined,
		Closed,
	};

	FUE2NetConnection();
	~FUE2NetConnection();

	bool Connect(const FSettings& InSettings, FString& OutError);
	/** Receives, processes and sends. Now = FPlatformTime::Seconds(). */
	void Tick(double Now);
	/** Packages we lack are skipped (SKIP), the rest announced (HAVE), then JOIN. Call once the map is loaded. */
	void SendJoin();
	/** Closes the control channel (the server logs the player out) and the socket. */
	void Close(const FString& Reason);

	EState GetState() const { return State; }
	bool IsOpen() const { return State != EState::Idle && State != EState::Closed; }
	const FSettings& GetSettings() const { return Settings; }
	FString GetAddressString() const { return FString::Printf(TEXT("%s:%d"), *Settings.Host, Settings.Port); }
	/** WELCOME LEVEL= / GAME=. */
	const FString& GetMapName() const { return MapName; }
	const FString& GetGameClass() const { return GameClass; }
	/** Why the connection closed (empty for a normal disconnect). */
	const FString& GetCloseReason() const { return CloseReason; }
	/** The server's FAILCODE on a rejected login (e.g. "NEEDSTATS", "LOCALBAN", "NEEDPW"), empty otherwise. */
	const FString& GetCloseFailCode() const { return CloseFailCode; }
	FUE2PackageMap& GetPackageMap() { return PackageMap; }
	/** DLMGR lines of the server ("CLASS=IpDrv.HTTPDownload PARAMS=http://... COMPRESSION=1"), in its order of preference. */
	const TArray<FString>& GetDownloadManagers() const { return DownloadManagers; }
	const TMap<int32, FUE2NetActor>& GetActors() const { return Actors; }
	/** Short status line for the HUD. */
	FString DescribeStatus() const;
	/** Value as text for logs ("(x,y,z)", "\"text\"", "Package.Object" ...). */
	FString ValueToString(const FUE2NetValue& Value);

	/**
	 * Calls a replicated function on the server (client -> server RPC) on an actor channel we own (our
	 * PlayerController). Params are in declaration order; missing ones are sent as "not present".
	 */
	bool SendFunction(int32 Channel, FName FunctionName, const TArray<FUE2NetValue>& Params);

	/** Channel of our own PlayerController (the actor the server lets us call functions on), or 0. */
	int32 FindPlayerControllerChannel() const;

	// ---- file channels (Engine.ChannelDownload, Net/UE2NetDownload.h) ----
	/**
	 * Asks the server for a package file: opens a file channel (type 3) with a reliable bunch that holds the package
	 * GUID. The server answers on that channel with the file's bytes (OnFileData), the last bunch closing the channel;
	 * a refusal is a close without data. Returns the channel index, or 0.
	 */
	int32 OpenFileChannel(const FGuid& Guid);
	/** Closes a file channel we opened (cancels that download). */
	void CloseFileChannel(int32 Channel);
	/** Sends NETSPEED (bytes per second the server may send us, capped by its MaxClientRate). */
	void SetNetSpeed(int32 BytesPerSecond);

	// ---- notifications for the game (step 3 builds the world from these) ----
	TFunction<void()> OnWelcome;
	TFunction<void(const FString& Reason)> OnClosed;
	TFunction<void(const FUE2NetActor&)> OnActorOpened;
	TFunction<void(const FUE2NetActor&, const TArray<FString>& ChangedProperties)> OnActorUpdated;
	TFunction<void(const FUE2NetActor&)> OnActorClosed;
	TFunction<void(const FUE2NetActor&, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)> OnFunctionCall;
	/** Bytes that arrived on a file channel; bClose: the server closed the channel (the file is complete, or refused). */
	TFunction<void(int32 Channel, const TArray<uint8>& Data, bool bClose)> OnFileData;

	// ---- statistics ----
	int32 PacketsIn = 0;
	int32 PacketsOut = 0;
	int64 BytesIn = 0;
	int64 BytesOut = 0;
	int32 BunchErrors = 0;
	int32 ActorsOpened = 0;
	int32 ActorsClosed = 0;
	int32 PropertiesReceived = 0;
	int32 FunctionsReceived = 0;

private:
	// UE2 protocol limits (UnConn.h): ranges of the packet / channel / sequence fields
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

	struct FChannel
	{
		uint8 ChType = 0;
		bool bOpenedByServer = false;
		/** Reliable bunches that arrived before their predecessors. */
		TArray<FInBunch> Queued;
	};

	// packets
	void ReceivePackets(double Now);
	void ReceivedRawPacket(const uint8* Data, int32 Count, double Now);
	void ReceivedAck(int32 AckId);
	void ReceivedNak(int32 NakId);
	void ReceivedBunch(FInBunch& Bunch);
	void ProcessBunch(FInBunch& Bunch);
	void QueueBunch(FOutBunch&& Bunch);
	void SendControl(const FString& Text, bool bOpen = false);
	/** Sends the AUTH message (identity hash, GM and, for a STATS=1 server with Track Stats on, the stats account). */
	void SendAuth();
	/** Plain-language message for a FAILCODE. */
	static FString FailCodeMessage(const FString& Code, const FString& Error);
	void Flush(double Now);
	void SendPacket(const FUE2BitWriter& Packet);
	void WriteBunchHeader(FUE2BitWriter& W, const FOutBunch& B) const;

	// control channel
	void ReceivedText(const FString& Text);

	// actor channels
	void ReceivedActorBunch(FInBunch& Bunch);
	void CloseActor(int32 Channel);
	bool ReadObject(FUE2BitReader& R, FUE2ObjectRef& Out);
	bool ReadProperty(FUE2BitReader& R, FUSProperty* Prop, FUE2NetValue& Out, int32 Depth = 0);
	bool ReadStruct(FUE2BitReader& R, FUSStruct* Struct, FUE2NetValue& Out, int32 Depth);
	bool WriteProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue& Value);
	void HandleFunction(FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	void Trace(const FString& Line) const;

	void SetClosed(const FString& Reason);

	FSettings Settings;
	EState State = EState::Idle;
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> RemoteAddr;

	FUE2PackageMap PackageMap;
	FString MapName;
	FString GameClass;
	FString CloseReason;
	FString CloseFailCode;
	/** When LOGIN was sent (for the login timeout); 0 before login. */
	double LoginTime = 0.0;
	TArray<FString> DownloadManagers;
	int32 Challenge = 0;
	int32 NegotiatedVersion = 0;
	/** CHALLENGE STATS=: the server tracks stats (the stats account is only sent then). */
	int32 ServerStats = 0;

	// packet state
	int32 InPacketId = -1;
	int32 OutPacketId = 0;
	int32 OutAckPacketId = -1;
	TArray<int32> PendingAcks;
	TArray<FOutBunch> SendQueue;
	/** Reliable bunches sent and not yet acknowledged. */
	TArray<FOutBunch> OutReliableBunches;
	int32 InReliable[MaxChannels] = {};
	int32 OutReliable[MaxChannels] = {};
	TMap<int32, FChannel> Channels;
	TMap<int32, FUE2NetActor> Actors;

	double LastReceiveTime = 0.0;
	double LastSendTime = 0.0;
	double ConnectTime = 0.0;
	double LastHelloTime = 0.0;
	int32 HelloAttempts = 0;
	bool bLoggedMissing = false;
	/** Test hook -UE2NetLag=<ms>: simulated round trip added (half on each way); packets wait in these queues. */
	double SimLagSeconds = 0.0;
	TArray<TPair<double, TArray<uint8>>> DelayedIn;
	TArray<TPair<double, TArray<uint8>>> DelayedOut;
	/** Where the next file channel index is searched from (indices are not reused at once: see OpenFileChannel). */
	int32 NextFileChannel = 1;
};

#pragma once

#include "CoreMinimal.h"

class FSocket;
class FInternetAddr;

/**
 * UT2004 server browser client: the master server list (TCP) and the UDP server queries, for finding stock UT2004
 * (UE2, build 3369 / 3374) servers. The menu page is ArenaServerBrowserPage.h; joining goes through
 * ArenaNet::JoinServer (Net/UE2NetJoin.h, the UE2 client).
 *
 * Master server (IpDrv.MasterServerLink, TCP port 28902): every message is a little-endian uint32 length and the
 * body; strings are UE2 FStrings (compact index length including the terminator, negative = UTF-16).
 *   master -> challenge string
 *   client -> CD key hash, challenge response, "UT2K4CLIENT", version 3369, platform byte, language,
 *             GPU device / vendor id, CPU speed, CPU type
 *   master -> "APPROVED" (+ uint32)          client -> verification string     master -> "VERIFIED"
 *   client -> query: byte 0, byte filter count, (key, value, byte comparator) per filter (gametype = xDeathMatch ...)
 *   master -> uint32 server count, byte; then one message per server: IPv4 (4 bytes), uint16 game port,
 *             uint16 query port, name, map, game type, byte players, byte max players, uint32 flags, skill
 * The community masters (333networks, OldUnreal's list, OpenSpy) accept listing queries with an empty CD key hash
 * and response, so the browser never reads, hashes or sends a CD key.
 *
 * Server query (UDP, the game port + 1): request 0x80 0 0 0 and a type byte; the reply starts with the same five
 * bytes. 0 = server info (uint32 server id, IP string, int32 game port, int32 query port, name, map, game type,
 * int32 players, int32 max players, int32 ping, int32 flags, skill), 1 = rules (key / value string pairs, possibly
 * several packets), 2 = players (int32 id, name, int32 ping, int32 score, int32 stats id with the team in bits 29 / 30;
 * several packets, none when the server is empty). LAN: the server info query sent to UDP 10777 (broadcast) is
 * answered from each LAN server's query port.
 *
 * Threads: master server and host name lookups run on worker threads; UDP queries run on the game thread from
 * Tick() with a non-blocking socket: at most MaxInFlight queries outstanding, MaxPerSecond sent per second,
 * QueryTimeout per try and one retry.
 */
namespace ArenaServerBrowser
{
	struct FPlayer
	{
		int32 Id = 0;
		FString Name;
		int32 Ping = 0;
		int32 Score = 0;
		/** -1 none, 0 red, 1 blue */
		int32 Team = -1;
	};

	enum class EServerState : uint8 { Waiting, Querying, Responded, NoResponse };

	struct FServer
	{
		/** IPv4 dotted address (resolved), and the host name the player typed (favourites, Open IP) */
		FString Ip;
		FString Host;
		int32 Port = 7777;
		int32 QueryPort = 7778;
		FString Name;
		FString Map;
		/** UE2 game class name without the package ("xDeathMatch", "ONSOnslaughtGame") */
		FString GameType;
		FString Skill;
		int32 NumPlayers = 0;
		int32 MaxPlayers = 0;
		uint32 Flags = 0;
		/** Round trip of the info query in ms, -1 = not measured */
		int32 Ping = -1;
		EServerState State = EServerState::Waiting;
		/** From the master list or an info reply (otherwise only the address is known). */
		bool bHasInfo = false;

		TArray<TPair<FString, FString>> Rules;
		TArray<FPlayer> Players;
		/** Real time of the last rules / players reply. */
		double DetailsAt = -1.0;

		/** "ip:port" (or "host:port" before the host is resolved) */
		FString Address() const;
		/** ServerResponseLine flags bit 0 */
		bool IsPassworded() const { return (Flags & 1u) != 0; }
	};

	struct FMasterServer
	{
		FString Host;
		int32 Port = 28902;
		/** Masters of a group share their lists: one of each group is asked. */
		int32 Group = 1;
	};
	/** UArenaUserSettings::MasterServers, or the OldUnreal 3374 Default.ini list when that is empty. */
	OPENUT2K4_API TArray<FMasterServer> GetMasterServers();

	/** "host", "host:port" (game port, default 7777); also accepts "unreal://" / "ut2004://" prefixes. */
	OPENUT2K4_API bool ParseAddress(const FString& In, FString& OutHost, int32& OutPort);

	/** UE2 colour codes (0x1B + 3 bytes) and control characters removed (bKeepColours: only the control characters). */
	OPENUT2K4_API FString StripColours(const FString& In, bool bKeepColours = false);

	/**
	 * An 8-bit UE2 string's bytes (up to a terminator) as text: colour codes removed, UTF-8 when the bytes are valid
	 * UTF-8 with a multi-byte sequence, else Windows-1252. Shared by the server queries and the UE2 client's strings.
	 * bKeepColours keeps the colour codes as ESC R G B characters (FArenaDraw::TextUT draws them as colour runs).
	 */
	OPENUT2K4_API FString DecodeBytes(const uint8* Bytes, int32 Num, bool bKeepColours = false);

	struct FJob;

	class OPENUT2K4_API FBrowser
	{
	public:
		FBrowser();
		~FBrowser();
		FBrowser(const FBrowser&) = delete;
		FBrowser& operator=(const FBrowser&) = delete;

		/** Asks the master servers for the servers of a game type ("" = all), then pings each. Replaces the list. */
		void RefreshInternet(const FString& GameType);
		/** Broadcasts the LAN query (UDP 10777, also 127.0.0.1) and lists the servers that answer. Replaces the list. */
		void RefreshLAN();
		/** Lists and pings these addresses ("host:port"); names are resolved on a worker thread. Replaces the list. */
		void RefreshAddresses(const TArray<FString>& Addresses);
		/** Stops refreshing (pending queries are dropped). */
		void Stop();

		/** Queries rules and players of a server now and then every DetailsInterval while called each frame. */
		void WantDetails(int32 Index);

		/** Game thread, every frame: receive replies, send queued queries, merge worker results. */
		void Tick();

		const TArray<FServer>& GetServers() const { return Servers; }
		/** Status line: "Contacting the master server...", "Pinging 12 / 300", "300 servers" */
		FString GetStatus() const;
		bool IsBusy() const;
		/** Increments whenever the list changes (sorting / filtering caches). */
		uint32 GetRevision() const { return Revision; }

		static constexpr int32 MaxInFlight = 24;
		static constexpr int32 MaxPerSecond = 60;
		static constexpr double QueryTimeout = 2.0;
		static constexpr double DetailsInterval = 6.0;

	private:
		enum class EMode : uint8 { None, Internet, LAN, Addresses };
		struct FPending
		{
			int32 Server = INDEX_NONE;
			uint8 Type = 0;
			double SentAt = 0.0;
			int32 Tries = 0;
		};
		void Reset(EMode NewMode);
		bool EnsureSocket();
		void CloseSocket();
		void SendQuery(int32 Index, uint8 Type);
		bool SendTo(const FString& Ip, int32 Port, const uint8* Data, int32 Num);
		void Receive();
		void HandlePacket(const uint8* Data, int32 Num, uint32 FromIp, int32 FromPort);
		int32 FindByQueryAddress(const FString& Ip, int32 QueryPort) const;
		void MergeJob();

		EMode Mode = EMode::None;
		TArray<FServer> Servers;
		/** "ip:queryport" -> index */
		TMap<FString, int32> ByQueryAddress;
		/** Info queries still to send (server indices). */
		TArray<int32> Queue;
		TArray<FPending> InFlight;
		double TokenTime = 0.0;
		double Tokens = 0.0;
		FSocket* Socket = nullptr;
		TSharedPtr<FInternetAddr> Scratch;
		TSharedPtr<FJob, ESPMode::ThreadSafe> Job;
		FString JobStatus;
		double LANSentAt = -1.0;
		int32 DetailsIndex = INDEX_NONE;
		double DetailsSentAt = -1.0;
		/** The next rules / players reply of the selected server replaces the old lists. */
		bool bRulesFresh = false;
		bool bPlayersFresh = false;
		uint32 Revision = 1;
	};
}

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameSession.h"
#include "ArenaGameSession.generated.h"

class FSocket;
class UWorld;

/**
 * Hosting OpenUT2k4 games for OpenUT2k4 players (Host Game, UT2K4GamePageMP): the match runs as a UE5 listen server
 * (or a separate -server process for DEDICATED), clients join with UE5's own net driver (IpNetDriver).
 *
 * The server's travel options (ArenaHost::BuildServerOptions):
 *   ?listen ?MaxPlayers=N ?MaxSpectators=N (AGameSession) ?ServerName=<name> ?GamePassword=<pw> ?LANServer=1
 *   ?Advertise=0|1, the game port through ArenaHost::SetListenPort (FURL's default port until the server listens).
 * A client's ?Password=<pw> must match ?GamePassword (ApproveLogin).
 *
 * Advertising: while the world is a server, the session answers the UT2004 server queries (the protocol the server
 * browser speaks, Net/ArenaServerBrowser.h) on UDP game port + 1, and the LAN broadcast on UDP 10777: info (name,
 * map, UE2 game type name, players, max players, flags), rules and players. Flags carry ArenaHost::OpenServerFlag, so
 * the browser joins these with UE5 (ArenaHost::JoinServer) instead of the UE2 client.
 */
UCLASS()
class OPENUT2K4_API AArenaGameSession : public AGameSession
{
	GENERATED_BODY()

public:
	AArenaGameSession();

	virtual void InitOptions(const FString& Options) override;
	virtual FString ApproveLogin(const FString& Options) override;
	virtual void Tick(float DeltaSeconds) override;

	const FString& GetServerName() const { return ServerName; }
	bool HasPassword() const { return !GamePassword.IsEmpty(); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void OpenQuerySockets();
	void CloseQuerySockets();
	void Receive(FSocket* Socket);
	void Answer(uint8 Type, const TSharedRef<class FInternetAddr>& To);

	FString ServerName;
	FString GamePassword;
	bool bLANServer = false;
	bool bAdvertise = true;
	/** ?UE2Host=1: host with our UE2 server (UT2004 and OpenUT2k4 clients; Net/UE2Server.h) on ?UE2Port, not UE5's net driver. */
	bool bUE2Host = false;
	int32 UE2Port = 7777;
	/** ?UE2Dedicated=1: the hosting process has no player of its own. */
	bool bUE2Dedicated = false;
	FString OptionsString;
	int32 GamePort = 0;
	FSocket* QuerySocket = nullptr;
	FSocket* LANSocket = nullptr;
};

namespace ArenaHost
{
	/** The hosting session's query socket (port + 1), for the master server uplink's query heartbeats; null when none. */
	OPENUT2K4_API FSocket* GetHostQuerySocket();

	/** Server info flags bit of an OpenUT2k4 (UE5) server; the low bits are UT2004's (1 password, 8 listen, 16 instagib). */
	constexpr uint32 OpenServerFlag = 1u << 30;
	inline bool IsOpenServer(uint32 Flags) { return (Flags & OpenServerFlag) != 0; }

	/** UE2 game class name of a mode id (DM -> xDeathMatch ...), what UT2004 browsers filter on. */
	OPENUT2K4_API FString UE2GameType(const FString& ModeId);

	/** The game port the next listen server binds (FURL's default port; restored once the server listens). */
	OPENUT2K4_API void SetListenPort(int32 Port);

	/** Server options from the Host Game settings (UArenaUserSettings Host*), appended to the match options. */
	OPENUT2K4_API FString BuildServerOptions(bool bListen);

	/**
	 * DEDICATED: starts this executable as a separate server process with the match URL (editor binary: -server;
	 * a packaged game has no dedicated server mode: a windowless -nullrhi instance hosts it as a listen server with
	 * a spectator-only local player). Returns false (with OutError) when it could not be started.
	 */
	OPENUT2K4_API bool LaunchDedicated(const FString& LevelAndOptions, FString& OutError);

	/**
	 * Asks host:port's query port (port + 1) for its info and waits up to TimeoutSeconds (game thread, blocking:
	 * Open IP only). True when an OpenUT2k4 server answered.
	 */
	OPENUT2K4_API bool ProbeOpenServer(const FString& Host, int32 Port, float TimeoutSeconds = 0.35f);

	/** Joins an OpenUT2k4 server with UE5's net driver: "host:port[?Password=..][?LAN]". False without a local player. */
	OPENUT2K4_API bool JoinServer(const FString& Url);
}

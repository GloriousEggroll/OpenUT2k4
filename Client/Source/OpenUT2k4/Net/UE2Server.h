#pragma once

/** Work in progress switch for the UE2 server (keeps the shared build green while it is written). */
#ifndef OPENUT2K4_UE2SERVER_BUILD
#define OPENUT2K4_UE2SERVER_BUILD 1
#endif

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "UE2NetConnection.h"
#include "UE2NetPackageMap.h"

class FSocket;
class FInternetAddr;
class FUE2ServerConnection;
class FUE2ServerWorld;
class FUE2MasterUplink;
class UWorld;
struct FUSClass;
struct FUSFunction;
struct FUSProperty;
struct FUE2ClassNetCache;

/**
 * Hosting UT2004 games: OpenUT2k4's own UE2 network server (Docs/UE2Net.md "Hosting: the UE2 server"). Real UT2004
 * clients (3369 and OldUnreal 3374) and OpenUT2k4 clients (through our UE2 client, Net/UE2Net*) join the same way;
 * UE5's net driver is not involved.
 *
 *   FUE2Server              the UDP game port, the connections, the package list (USES) built from the player's
 *                           install, replication of the server actors to every connection
 *   FUE2ServerConnection    one client (Net/UE2ServerConnection.h)
 *   FUE2ServerWorld         our native game as stock UT2004 actors (GRI, PRIs, xPawns, weapons, pickups ...), the
 *                           remote players (Net/UE2ServerWorld.h)
 */

/** One replicated property value of a server actor. */
struct FUE2SvProp
{
	enum ECond : uint8
	{
		/** Every connection that has the actor. */
		All,
		/** Only the owner's connection (bNetOwner). */
		OwnerOnly,
		/** Every connection but the owner's. */
		NotOwner,
		/** Only in the initial bunch (bNetInitial). */
		InitialOnly,
	};
	FUE2NetValue Value;
	/** The owner's connection gets this value instead (e.g. Role: AutonomousProxy for the owner). */
	bool bOwnerValue = false;
	FUE2NetValue OwnerValue;
	uint8 Cond = All;
	/** Movement values (PawnPosition, Location ...): unreliable, resent on loss while they still differ. */
	bool bUnreliable = false;
};

/** An actor as the UE2 clients see it (one actor channel per connection that it is relevant to). */
struct FUE2SvActor
{
	int32 Id = 0;
	FUSClass* Class = nullptr;
	const FUE2ClassNetCache* NetCache = nullptr;
	/** "Package.Class". */
	FString ClassPath;
	/** The open bunch's reference in the master package list: the class (spawned actors) or the map actor (bStatic). */
	FUE2ObjectRef Archetype;
	bool bStatic = false;
	/** UE2 units (spawn location and relevancy). */
	FVector3f Location = FVector3f::ZeroVector;
	FIntVector Rotation = FIntVector::ZeroValue;
	bool bNetInitialRotation = false;
	bool bAlwaysRelevant = false;
	bool bOnlyRelevantToOwner = false;
	bool bNetTemporary = false;
	/** bTearOff sent: the channel closes after it. */
	bool bTearOff = false;
	/** Connection id of the owning player (PlayerController, its pawn and inventory), or 0. */
	int32 OwnerConn = 0;
	/** Seconds between property updates. */
	float UpdateInterval = 0.1f;
	/** Relevancy range in UU (0: no limit). */
	float CullDistance = 0.f;
	/** UE2 relevancy: relevant while the viewer can see it (a line of sight test, kept 5 s like UE2's RELEVANT_TIMEOUT). */
	bool bUseLineOfSight = false;
	/** Relevant exactly when this actor (e.g. an attachment's pawn) is open on the connection. */
	int32 RelevancyParent = 0;
	/** UE2 NetPriority: the bandwidth goes to the actors with the highest priority * time since their last update. */
	float NetPriority = 1.f;
	/** Native actor / object it stands for (may be null for synthetic actors such as TeamInfo). */
	TWeakObjectPtr<UObject> Native;
	TMap<FString, FUE2SvProp> Props;
	/** Frame it was last refreshed (actors not refreshed are destroyed). */
	uint32 SeenFrame = 0;
	/** Free form tag of the mapping code ("Pawn", "PRI", ...). */
	FName Kind;

	void Set(const FString& Key, const FUE2NetValue& Value, uint8 Cond = FUE2SvProp::All, bool bUnreliable = false)
	{
		FUE2SvProp& P = Props.FindOrAdd(Key);
		P.Value = Value;
		P.Cond = Cond;
		P.bUnreliable = bUnreliable;
		P.bOwnerValue = false;
	}
	void SetOwnerValue(const FString& Key, const FUE2NetValue& Value)
	{
		FUE2SvProp& P = Props.FindOrAdd(Key);
		P.bOwnerValue = true;
		P.OwnerValue = Value;
	}
};

struct FUE2ServerSettings
{
	/** UDP game port; the query port is Port + 1. */
	int32 Port = 7777;
	FString ServerName;
	FString Password;
	int32 MaxPlayers = 16;
	int32 MaxSpectators = 2;
	bool bLAN = false;
	/** "Show in Server Browsers": the OldUnreal master server uplink (off unless the player ticks it). */
	bool bAdvertise = false;
	/** Listen server (a local player in the world) or dedicated. */
	bool bListen = true;
};

class OPENUT2K4_API FUE2Server
{
public:
	/** Starts the server for the current match world (map and game from the game mode). False with OutError on failure. */
	static bool Start(UWorld* World, const FUE2ServerSettings& Settings, FString& OutError);
	static void Stop(const FString& Reason = FString());
	static FUE2Server* Get();
	static bool IsRunning() { return Get() != nullptr; }

	~FUE2Server();

	UWorld* GetWorld() const { return World.Get(); }
	const FUE2ServerSettings& GetSettings() const { return Settings; }
	const FString& GetMapName() const { return MapName; }
	const FString& GetGameClass() const { return GameClass; }
	int32 GetNumPlayers(bool bCountSpectators = false) const;
	const TArray<TUniquePtr<FUE2ServerConnection>>& GetConnections() const { return Connections; }
	FUE2ServerConnection* FindConnection(int32 ConnId) const;
	FUE2ServerWorld* GetServerWorld() const { return ServerWorld.Get(); }
	/** The master server uplink (null unless "Show in Server Browsers"). */
	FUE2MasterUplink* GetUplink() const { return Uplink.Get(); }

	// ---- package list (USES) ----
	/** The USES lines of this map, in order (map first). */
	const TArray<FString>& GetUsesLines() const { return UsesLines; }
	/** A package the clients can have: in the player's install (or the custom content folder). */
	bool HasPackageFile(const FString& Name) const;
	/** The master package map (every USES package, the server's own generations). */
	FUE2PackageMap& GetMasterMap() { return MasterMap; }
	/** A static object of the master map by path ("XGame.xPawn", "DM-Rankin.Mover0"); None when missing. */
	FUE2ObjectRef FindStatic(const FString& Path);
	/** A script class by "Package.Class" (null when the install does not have it). */
	FUSClass* FindClass(const FString& Path);
	/** Replicated fields of a class (the master map's numbering). */
	const FUE2ClassNetCache* GetNetCache(FUSClass* Class);

	// ---- replication ----
	/** Creates an actor (id assigned); the mapping code fills it every tick. Null when the class is unknown. */
	FUE2SvActor* CreateActor(const FString& ClassPath, FName Kind);
	/** A map actor ("DM-Rankin.Mover0"); null when it is not in the map package. */
	FUE2SvActor* CreateMapActor(const FString& ExportPath, const FString& ClassPath, FName Kind);
	FUE2SvActor* FindActor(int32 Id) { TUniquePtr<FUE2SvActor>* A = Actors.Find(Id); return A ? A->Get() : nullptr; }
	void DestroyActor(int32 Id);
	/**
	 * A replicated function call to a client (ClientMessage, ReceiveLocalizedMessage, ClientAdjustPosition ...) on an
	 * actor of that connection (its PlayerController, or an owned weapon / pawn).
	 */
	bool CallClient(int32 ConnId, int32 ActorId, FName Function, const TArray<FUE2NetValue>& Params, bool bForceUnreliable = false);
	/** The same call on every joined connection's actor returned by GetActor (e.g. each player's controller). */
	void CallAllClients(TFunctionRef<int32(int32 ConnId)> GetActor, FName Function, const TArray<FUE2NetValue>& Params);

	/** The default value of a property element (class defaults), for the shadow of a new channel. False when unknown. */
	bool GetDefaultValue(FUSClass* Class, FUSProperty* Prop, int32 Element, FUE2NetValue& Out);
	/** Property of a "Name" / "Name[i]" key of a class, with its element; null when it does not replicate. */
	FUSProperty* ResolveKey(FUSClass* Class, const FString& Key, int32& OutElement, int32& OutRepIndex);

	// ---- called by the connections ----
	void ConnectionLogin(FUE2ServerConnection& Conn, const FString& Url, FString& OutFailCode, FString& OutFailText);
	void ConnectionJoined(FUE2ServerConnection& Conn);
	void ConnectionClosed(FUE2ServerConnection& Conn, const FString& Reason);
	void ClientFunction(FUE2ServerConnection& Conn, int32 ActorId, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	int32 NextChallenge();
	FSocket* GetSocket() const { return Socket; }
	double GetTime() const;

	/** -UE2ServerTrace: log handshake lines, opened channels and function calls. */
	bool bTrace = false;

private:
	FUE2Server();
	bool Init(UWorld* InWorld, const FUE2ServerSettings& InSettings, FString& OutError);
	void BuildPackageList();
	void AddPackage(const FString& Name, int32 Depth);
	void PreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void ReceivePackets(double Now);
	void Replicate(double Now);
	bool IsRelevant(const FUE2SvActor& Actor, FUE2ServerConnection& Conn, const FVector3f& ViewLoc, double Now);
	/** Static geometry between two points (UE2 units)? */
	bool LineOfSight(const FVector3f& From, const FVector3f& To) const;
	void LogSummary();

	TWeakObjectPtr<UWorld> World;
	FUE2ServerSettings Settings;
	FString MapName;
	FString GameClass;
	FSocket* Socket = nullptr;
	FDelegateHandle PreTickHandle;
	FDelegateHandle PostTickHandle;
	TArray<TUniquePtr<FUE2ServerConnection>> Connections;
	int32 NextConnId = 1;
	TUniquePtr<FUE2ServerWorld> ServerWorld;
	TUniquePtr<FUE2MasterUplink> Uplink;

	// packages
	TArray<FString> UsesLines;
	TSet<FString> AddedPackages;
	FUE2PackageMap MasterMap;
	class FConvPackageManager* Files = nullptr;
	TMap<FString, FUE2ObjectRef> StaticCache;

	// actors
	TMap<int32, TUniquePtr<FUE2SvActor>> Actors;
	int32 NextActorId = 1;
	uint32 Frame = 0;
	struct FResolvedKey { FUSProperty* Prop = nullptr; int32 Element = 0; int32 RepIndex = INDEX_NONE; };
	TMap<FUSClass*, TMap<FString, FResolvedKey>> KeyCache;

	double StartTime = 0.0;
	double NextSummary = 0.0;
	double LastReplicate = 0.0;
	FRandomStream Random;

	friend class FUE2ServerConnection;
	friend class FUE2ServerWorld;
};

namespace ArenaHost
{
	/** True while this process hosts a UE2 server (Host Game). */
	OPENUT2K4_API bool IsUE2Hosting();

	/** A player connected to the hosted UE2 server (the host's admin tab). */
	struct FUE2HostPlayer
	{
		int32 ConnId = 0;
		FString Name;
		int32 PingMs = 0;
		/** Start of the player id (enough to tell players apart, not the whole id). */
		FString IdShort;
		bool bSpectator = false;
	};
	OPENUT2K4_API TArray<FUE2HostPlayer> GetUE2Players();
	/** Removes a player (UT2004 admin "kick"); bBan also adds the player id to the ban list. */
	OPENUT2K4_API bool KickUE2Player(int32 ConnId, bool bBan);
	/** The ban list ("name (id...)") and lifting a ban. */
	OPENUT2K4_API TArray<FString> GetUE2BanList();
	OPENUT2K4_API void UnbanUE2(int32 Index);
	/**
	 * Where bans live (entries "<player id>|<name>"): by default UArenaUserSettings::HostBannedPlayers; a dedicated server
	 * can keep them in its own ini. Used by the login check, KickUE2Player, GetUE2BanList and UnbanUE2.
	 */
	OPENUT2K4_API void SetUE2BanStore(TFunction<TArray<FString>&()> GetList, TFunction<void()> SaveList);
	/** A server / admin chat line to every UE2 client (BroadcastHandler.Broadcast(None, Text, 'Say')). */
	OPENUT2K4_API void UE2ServerSay(const FString& Text);
}

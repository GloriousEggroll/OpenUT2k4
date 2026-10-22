#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class APlayerController;
class AArenaPlayerState;
class FUE2NetConnection;
class UWorld;
struct FUE2NetActor;
struct FUE2NetValue;
struct FUSClass;
struct FUSFunction;

/**
 * Step 3 of Docs/UE2Net.md: the client world layer. It turns the actors a UT2004 server replicates (the connection's
 * OnActorOpened / OnActorUpdated / OnActorClosed / OnFunctionCall) into our native actors and game state while the
 * local player spectates:
 *
 *   PlayerReplicationInfo -> AArenaPlayerState (names, scores, deaths, teams: our HUD and scoreboard)
 *   GameReplicationInfo / TeamInfo -> AArenaGameState (goal, time limit, remaining time, team scores, match phase)
 *   xPawn -> AUE2NetPawn (our character body, PawnPosition interpolation, animation, weapon, ragdoll)
 *   weapon attachments -> third-person weapons, muzzle flashes, bullet impacts; beam effects -> our beams
 *   projectiles -> AUE2NetProjectile (our projectile art); pickups / movers / CTF flags / DOM points -> the map's
 *   native actors (matched by map export name or location); vehicles and other game objects -> placeholders
 *   our PlayerController's calls -> messages (kill lines, local messages, chat), sounds, the announcer, view target
 *
 * Values start from the class defaults (the UScript loader reads them) with the received properties on top; the
 * server's Role / RemoteRole are swapped (its RemoteRole is our Role). Classes that are not mapped (mods ...) are
 * ignored and counted (periodic "UE2NetWorld summary" log line).
 */
class FUE2NetWorld
{
public:
	FUE2NetWorld(FUE2NetConnection& InConnection, UWorld* InWorld);
	~FUE2NetWorld();

	void ActorOpened(const FUE2NetActor& Actor);
	void ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed);
	void ActorClosed(const FUE2NetActor& Actor);
	void FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	void Tick(float DeltaSeconds);

	/** Fire: the next player (ServerViewNextPlayer); alt fire: free camera (ServerViewSelf) and back. */
	bool SpectatorFire(APlayerController* PC, bool bAltFire);
	UWorld* GetWorld() const { return World.Get(); }
	/** Name of the followed player (empty: free camera). */
	FString GetViewedPlayerName() const;
	/** GameReplicationInfo.ServerName. */
	const FString& GetServerName() const { return ServerName; }
	/** Actor channel of our own PlayerReplicationInfo (0 until the server named it). */
	int32 GetOwnPRIChannel() const { return OwnPRIChannel; }
	/** Our player state for a PRI channel (our own PRI: the local PlayerController's). */
	AArenaPlayerState* PSForChannel(int32 PRIChannel);
	/** Our AUE2NetPawn for a pawn channel (also torn-off ones until the channel closes), or null. */
	class AUE2NetPawn* GetNetPawn(int32 PawnChannel) { return PawnForChannel(PawnChannel); }
	/** Our actor for any channel (a vehicle's AUE2NetVehicle, a map actor we drive, a pawn ...), or null. */
	/** Removes what we show for an actor without its end effects (our own guided warhead: Net/UE2NetPlay flies it). */
	void DropNativeActor(int32 Channel);
	AActor* GetNativeActor(int32 Channel) const { const FEntity* E = Channel ? Entities.Find(Channel) : nullptr; return E ? E->Native.Get() : nullptr; }
	/** The channel whose actor (or a component's owner chain up to it) Actor is, or 0. */
	int32 FindChannelOf(const AActor* Actor) const;
	/** Our vehicle actor for a vehicle / turret channel (a gunner seat's: its base vehicle's). */
	class AUE2NetVehicle* GetVehicle(int32 Channel) { return VehicleForChannel(Channel); }

	/** The world layer of the current connection, if any (game thread). */
	static FUE2NetWorld* Get();

	/** Replicated actor kinds the layer knows. */
	enum class EKind : uint8
	{
		Unknown, Ignored, Controller, PRI, GRI, Team, Pawn, Vehicle, Attachment, Projectile, Pickup, Mover, Flag, DomPoint,
		Objective, Beam, GameObject, WeaponPawn, VehicleWeapon, HeldBeam, Num
	};

private:
	struct FEntity
	{
		int32 Channel = 0;
		EKind Kind = EKind::Unknown;
		/** "Package.Class" and the short class names of the class chain, lower case, most derived first. */
		FString ClassPath;
		FString ClassName;
		TArray<FString> Chain;
		FUSClass* Class = nullptr;
		/** Our actor for it (spawned by us when bOwned, else a map actor we drive). */
		TWeakObjectPtr<AActor> Native;
		bool bOwned = false;
		TWeakObjectPtr<AArenaPlayerState> PS;
		int32 LastFlashCount = INDEX_NONE;
		int32 LastHitCount = INDEX_NONE;
		int32 LastHitFxTicker = INDEX_NONE;
		int32 PawnChannel = 0;
		/** Vehicle weapons / weapon pawns: the vehicle (or turret) channel they belong to. */
		int32 BaseChannel = 0;
		int32 LastExplosionCount = INDEX_NONE;
		int32 LastHealth = INDEX_NONE;
		bool bTornOff = false;
		/** ENetRole as seen by us (the server's Role and RemoteRole swapped). */
		uint8 Role = 0;
		uint8 RemoteRole = 0;
	};

	bool Has(const FEntity& E, const TCHAR* ShortClass) const { return E.Chain.Contains(FString(ShortClass)); }
	/** Has, or a mod's copy of the stock class under its own name that extends the stock class's parent instead
	 *  (ProSniperRifle's ProSniperWallHitEffect extends Effects): a class of the chain ends with ShortClassEnd. */
	bool HasLike(const FEntity& E, const TCHAR* ShortClassEnd) const
	{
		return E.Chain.ContainsByPredicate([ShortClassEnd](const FString& C) { return C.EndsWith(ShortClassEnd); });
	}
	EKind Classify(const FEntity& E) const;
	void Apply(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyPRI(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyGRI(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyTeam(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyPawn(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyAttachment(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyProjectile(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyPickup(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	/** An actor of a class we do not know that draws a static mesh (a mutator's decoration: a mask on a player's head). */
	void ApplyDecoration(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyMover(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyFlag(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyDomPoint(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyPlaceholder(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	// vehicles (Vehicle, stationary ONSWeaponPawn turrets), gunner seats (ONSWeaponPawn) and their guns (ONSWeapon)
	void ApplyVehicle(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyWeaponPawn(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyVehicleWeapon(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	class AUE2NetVehicle* SpawnVehicle(FEntity& E, const FUE2NetActor& A);
	/** Our vehicle actor for a vehicle / turret channel (a gunner seat's: its base vehicle's). */
	class AUE2NetVehicle* VehicleForChannel(int32 Channel, int32 Depth = 0);
	/** Seats the pawn of a vehicle / weapon pawn entity (E.PawnChannel) in its vehicle. */
	void LinkSeat(FEntity& E, const FUE2NetActor& A);
	// game objectives: ONS power nodes / cores, Assault objectives; the bombing run ball
	void ApplyObjective(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyPowerNode(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void ApplyAssaultObjective(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	void RefreshAssaultObjectives();
	/** The server's state of an Assault objective (by channel). */
	struct FNetObjective
	{
		TWeakObjectPtr<class AArenaAS_Objective> Obj;
		bool bActive = true;
		bool bDisabled = false;
		bool bDestroyable = false;
		bool bWorked = false;
		bool bLogged = false;
		float Health = 0.f;
		float MaxHealth = 0.f;
		int32 Defender = -1;
		double HeldSince = 0.0;
	};
	TMap<int32, FNetObjective> NetObjectives;
	void ApplyGameObject(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);
	/** Export name of a map actor's channel ("ONSPowerNodeNeutral12"; empty for spawned actors). */
	FString MapActorName(const FUE2NetActor& A) const;
	/** A class default as a vector / rotator (UE2 units; "(X=..,Y=..,Z=..)" / "(Pitch=..,Yaw=..,Roll=..)"). */
	FVector DefaultVector(FUSClass* Class, const TCHAR* Key) const;
	FRotator DefaultRotator(FUSClass* Class, const TCHAR* Key) const;
	/**
	 * A local message class we have no table entry for (ONS, Assault, mod classes): its static GetString run by the
	 * UScript VM with the received switch, PRIs and optional object, shown with the class's own style (console line
	 * or local message). False when there is no text.
	 */
	bool ShowScriptedMessage(const FString& ClassPath, FUSClass* Class, int32 Switch, int32 PRI1, int32 PRI2, const struct FUE2NetValue* Optional);
	void SpawnBeam(FEntity& E, const FUE2NetActor& A);
	/** LinkBeamEffect / PainterBeamEffect of other players: a beam kept while they hold fire. */
	void ApplyHeldBeam(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial);

	// controller calls
	void ReceiveLocalizedMessage(const TArray<TPair<FName, FUE2NetValue>>& Params);
	void HearSound(const TArray<TPair<FName, FUE2NetValue>>& Params, bool b2D);
	void SetViewTargetFromServer(const FUE2NetValue& Target);
	void PlayAnnouncer(const FString& Name);

	// values: the received property, else the class default
	const FUE2NetValue* Find(const FUE2NetActor& A, const TCHAR* Key) const;
	FString DefaultText(FUSClass* Class, const TCHAR* Key) const;
	int32 GetInt(const FUE2NetActor& A, const TCHAR* Key, int32 Default = 0) const;
	float GetFloat(const FUE2NetActor& A, const TCHAR* Key, float Default = 0.f) const;
	bool GetBool(const FUE2NetActor& A, const TCHAR* Key, bool bDefault = false) const;
	FString GetStr(const FUE2NetActor& A, const TCHAR* Key) const;
	FVector GetVec(const FUE2NetActor& A, const TCHAR* Key, const FVector& Default = FVector::ZeroVector) const;
	/** Actor channel of an object property (0 for None or static objects). */
	int32 GetRef(const FUE2NetActor& A, const TCHAR* Key) const;
	/** "Package.Name" of a static object property (classes, sounds, map actors); empty for None / actors. */
	FString GetObjPath(const FUE2NetActor& A, const TCHAR* Key) const;
	FString ObjPath(const FUE2NetValue& V) const;
	/** A class default of a class reference (e.g. a damage type's "bAlwaysGibs"). */
	FString ClassDefault(const FUE2NetValue& ClassRef, const TCHAR* Key) const;
	/** Role / RemoteRole as seen by us (the server's swapped). */
	static void SwapRoles(int32& Role, int32& RemoteRole) { Swap(Role, RemoteRole); }

	FEntity* FindEntity(int32 Channel) { return Channel ? Entities.Find(Channel) : nullptr; }
	class AUE2NetPawn* PawnForChannel(int32 Channel);
	class AUE2NetPawn* PawnOfPRI(int32 PRIChannel, bool bAliveOnly);
	/** The living pawn of a PRI (our own PRI while playing: our local pawn, Net/UE2NetPlay.h). */
	class AArenaCharacter* CharacterOfPRI(int32 PRIChannel);
	int32 TeamIndexOf(int32 TeamInfoChannel) const;
	class AArenaPlayerController* GetPC() const;
	void LogSummary();

	FUE2NetConnection* Connection = nullptr;
	TWeakObjectPtr<UWorld> World;
	TMap<int32, FEntity> Entities;
	/** Torn-off pawns kept a few seconds after their channel closed (ragdolls). */
	TArray<TPair<TWeakObjectPtr<AActor>, double>> Corpses;
	/** Shock cores whose channel closed: their explosion waits a moment, a shock combo there takes its place. */
	TArray<TPair<TWeakObjectPtr<class AUE2NetProjectile>, double>> PendingShockCores;
	/** Recent shock combos (location, real time): a core closing there just goes. */
	TArray<TPair<FVector, double>> RecentCombos;

	int32 OwnPRIChannel = 0;
	int32 GRIChannel = 0;
	FString ServerName;
	// GameReplicationInfo time keeping (UE2 GRI.Timer on clients)
	int32 RemainingTime = 0;
	bool bStopCountDown = true;
	bool bMatchHasBegun = false;
	bool bGameEnded = false;
	double NextSecond = 0.0;
	// Assault (ASGameReplicationInfo): the round time pod counts RoundTimeLimit down from RoundStartTime against the
	// GRI's ElapsedTime (counted locally between updates like UE2's GRI.Timer)
	bool bAssault = false;
	int32 ElapsedTime = 0;
	int32 RoundTimeLimit = 0;
	int32 RoundStartTime = 0;

	// spectating
	int32 FollowPRI = 0;
	int32 FollowPawn = 0;
	/** ClientSetViewTarget named a pawn whose channel is not open yet. */
	int32 PendingViewChannel = 0;
	bool bFreeCamera = true;
	bool bWantFreeCamera = false;
	double LastViewRequest = 0.0;
	double LastVerifyTime = 0.0;

	// sounds: UE2 sends some ClientHearSound twice (Id)
	TMap<int32, double> RecentSoundIds;

	// statistics for the summary log
	int32 KindOpened[int32(EKind::Num)] = {};
	int32 KindLive[int32(EKind::Num)] = {};
	TMap<FString, int32> UnknownClasses;
	/** Decorations with a PrePivot (UE2 draws them at Location + PrePivot along the world axes): their offset from their
	 *  Base's frame (cm) and PrePivot (cm), placed each tick. */
	struct FPrePivoted
	{
		TWeakObjectPtr<USceneComponent> Comp;
		FVector Relative = FVector::ZeroVector;
		FVector PrePivot = FVector::ZeroVector;
	};
	TMap<int32, FPrePivoted> PrePivoted;
	TMap<FString, int32> UnhandledMessages;
	int32 PickupsMatched = 0;
	int32 NodesMatched = 0;
	int32 NodeUpdates = 0;
	int32 ObjectivesMatched = 0;
	int32 ObjectiveUpdates = 0;
	int32 BallUpdates = 0;
	int32 ScriptedMessages = 0;
	TMap<FString, int32> ScriptedMessageClasses;
	int32 ProjectilesClosed = 0;
	double ProjectileDistance = 0.0;
	int32 PickupsUnmatched = 0;
	int32 MessagesShown = 0;
	int32 SoundsPlayed = 0;
	int32 KillLines = 0;
	double NextSummary = 0.0;
	double StartTime = 0.0;
	// -UE2NetSpectateScript="<sec>:next|self;..." (test hook)
	TArray<TPair<double, FString>> Script;
};

namespace ArenaNet
{
	/**
	 * Fire / alt fire of the local player while spectating a UT2004 server: fire asks the server for the next player
	 * (ServerViewNextPlayer), alt fire switches to the free camera (ServerViewSelf) and back. True when handled.
	 */
	OPENUT2K4_API bool HandleSpectatorFire(APlayerController* PC, bool bAltFire);
}

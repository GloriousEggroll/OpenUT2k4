#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "UE2NetPackageMap.h"

class AActor;
class AController;
class AArenaCharacter;
class AArenaFlag;
class AArenaGameMode;
class AArenaGameState;
class AArenaWeapon;
class APlayerState;
class AUE2RemoteController;
class FUE2Server;
class FUE2ServerConnection;
class UObject;
class UWorld;
struct FUE2NetValue;
struct FUE2SvActor;
struct FUSFunction;

/**
 * Our native game as stock UT2004 actors for the clients of our UE2 server (the reverse of the client's
 * FUE2NetWorld), and the remote players. See Docs/UE2Net.md "Hosting: the UE2 server".
 *
 *   AArenaGameState      -> Engine.GameReplicationInfo (+ XGame.xTeamRoster per team), the map's LevelInfo0
 *   AArenaPlayerState    -> XGame.xPlayerReplicationInfo
 *   remote players       -> XGame.xPlayer (owner only) + AUE2RemoteController in our game
 *   AArenaCharacter      -> XGame.xPawn (PawnPosition, Health, AnimAction, death / tear off), its weapon attachment
 *   AArenaWeapon         -> the stock weapon (owner only inventory), AArenaProjectile -> stock projectiles
 *   AArenaPickup         -> the stock pickup class (bHidden while taken), AArenaMover -> the map's Mover (SimInterpolate)
 *   AArenaFlag           -> XGame.xRedFlag / xBlueFlag, the map's flag bases
 *
 * Client calls: ServerMove / DualServerMove / ShortServerMove run one movement step each and are answered with
 * ClientAdjustPosition like UE2; fire, weapon switch, use, respawn, suicide, team change, chat, combos, spectating.
 */
class FUE2ServerWorld
{
public:
	FUE2ServerWorld(FUE2Server& InServer, UWorld* InWorld);
	~FUE2ServerWorld();

	/** Refreshes every server actor from the native game (before the server replicates). */
	void Refresh(float DeltaSeconds);
	/** End of the server tick: the pending movement corrections (one per player and tick, as UE2). */
	void SendAdjustments();

	void PlayerJoined(FUE2ServerConnection& Conn);
	void PlayerLeft(FUE2ServerConnection& Conn);
	void ClientFunction(FUE2ServerConnection& Conn, int32 ActorId, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);

	/** Where this connection's player looks from (relevancy), UE2 units. */
	bool GetViewLocation(int32 ConnId, FVector3f& OutLocation) const;
	/** The PlayerController actor of a connection (0 before it joined). */
	int32 GetControllerActor(int32 ConnId) const;
	/** PRI actor of a player state (0 when none). */
	int32 PRIActorOf(const APlayerState* PS) const;

	// game events (UE2ServerHooks)
	void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot);
	void OnFlagEvent(AArenaFlag* Flag, AController* By, int32 Switch);
	void OnChat(APlayerState* From, const FString& Text, bool bTeam);
	void OnLocalMessage(const TCHAR* MessageClass, int32 Switch, APlayerState* PRI1, APlayerState* PRI2, AController* OnlyTo);
	/** Next map of the rotation (?MapList=, else every map of this game type), clients sent there. */
	bool TravelToNextMap(AArenaGameMode* GM);
	bool TravelToMap(AArenaGameMode* GM, const FString& MapName, int32 RotationSize);

	FString DescribeSummary() const;

private:
	struct FRemotePlayer
	{
		int32 ConnId = 0;
		TWeakObjectPtr<AUE2RemoteController> Controller;
		int32 PCActor = 0;
		bool bSpectator = false;
		/** UE2 controller state the client is told about (PlayerWalking, Dead, Spectating ...). */
		FName State;
		FName LastSentState;
		// movement (PlayerController.ServerMove)
		float CurrentTimeStamp = 0.f;
		double LastUpdateTime = 0.0;
		bool bJumpStatus = false;
		int32 AcknowledgedPawn = 0;
		bool bPendingAdjustment = false;
		float AdjustTimeStamp = 0.f;
		double LastGivePawn = 0.0;
		FVector SpectatorLocation = FVector::ZeroVector;
		FRotator View = FRotator::ZeroRotator;
		// the pawn last given (ClientRestart) and the weapons the client was told about
		TWeakObjectPtr<AActor> RestartedPawn;
		TSet<int32> WeaponsSet;
		int32 ViewTargetActor = 0;
		// driving (PlayerController.ServerDrive, Vehicle.VehicleFire / VehicleCeaseFire)
		float DriveForward = 0.f;
		float DriveStrafe = 0.f;
		float DriveUp = 0.f;
		bool bDriveJump = false;
		int32 DriveCalls = 0;
		bool bVehicleFire[2] = { false, false };
		bool bVehicleUse = false;
		TWeakObjectPtr<APawn> DrivenPawn;
		// statistics
		int32 Moves = 0;
		int32 Steps = 0;
		int32 Adjustments = 0;
		int32 Corrections = 0;
		int32 Trusted = 0;
		double ErrorSum = 0.0;
		float MaxError = 0.f;
		int32 FireCommands = 0;
		int32 Restarts = 0;
	};

	// mapping
	FUE2SvActor* ActorFor(UObject* Native, const FString& ClassPath, FName Kind, bool* bOutNew = nullptr);
	void RefreshGame(AArenaGameMode* GM, AArenaGameState* GS);
	void RefreshPlayerStates(AArenaGameState* GS);
	void RefreshControllers();
	void RefreshPawns(float DeltaSeconds);
	void RefreshProjectiles();
	void RefreshPickups();
	void RefreshMovers();
	void RefreshFlags();
	/** Double domination points, the bombing run ball. */
	void RefreshObjectives();
	/** ONS vehicles (their KRigidBody state, driver, guns) and gunner seats. */
	void RefreshVehicles();
	/** A vehicle class's default DriverWeapons / PassengerWeapons ({class, bone} pairs). */
	TArray<TPair<FString, FName>> DefaultVehicleWeapons(const FString& VehicleClass, const TCHAR* Array, const TCHAR* ClassMember);
	int32 PawnActorOf(const AActor* Pawn) const;
	int32 TeamActor(int32 Team) const { return (Team == 0 || Team == 1) ? TeamActors[Team] : 0; }
	/** The UE2 class of a weapon / its attachment / pickup / ammo pickup ("XWeapons.RocketLauncher" ...). */
	static FString UE2WeaponClass(const UClass* WeaponClass, int32 Which);
	static FString UE2DamageType(const FString& WeaponName, bool bHeadshot);

	// players
	FRemotePlayer* FindPlayer(int32 ConnId) { return Players.Find(ConnId); }
	FRemotePlayer* FindPlayerByController(const AController* C);
	void SpawnRemotePlayer(FRemotePlayer& P, FUE2ServerConnection& Conn);
	void UpdatePossession(FRemotePlayer& P);
	void ServerMove(FRemotePlayer& P, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, bool bRun, bool bDuck, bool bJumpStatus,
		bool bDoubleJump, uint8 DoubleClickMove, int32 View, uint8 OldTimeDelta, int32 OldAccel);
	void MoveAutonomous(FRemotePlayer& P, float DeltaTime, bool bRun, bool bDuck, bool bPressedJump, bool bDoubleJump, uint8 DoubleClickMove, const FVector& AccelUU);
	void SendAdjustment(FRemotePlayer& P);
	void CallPC(int32 ConnId, FName Function, const TArray<FUE2NetValue>& Params, bool bForceUnreliable = false);
	void BroadcastLocalized(const FString& MessageClass, int32 Switch, const APlayerState* PRI1, const APlayerState* PRI2, const FUE2NetValue* Optional);
	void HandleViewChange(FRemotePlayer& P, bool bNext);
	/** The stock fire sound of a weapon's fire mode (its WeaponFire class defaults), cached. */
	bool GetFireSound(const FString& WeaponClass, int32 Mode, FUE2ObjectRef& OutSound, float& OutVolume, float& OutRadius);
	void PlayFireSound(const FUE2SvActor& Pawn, const FString& WeaponClass, int32 Mode);
	/** PlayerController.ClientHearSound to every player in hearing range (ExcludeConn: PlayOwnedSound's owner). */
	void HearSound(const FUE2ObjectRef& Sound, const FVector3f& Location, float Volume, float Radius, int32 ExcludeConn, int32 Id);
	/** Class defaults of the stock classes: an object property as a static reference, a dynamic array of them, a name. */
	FUE2ObjectRef DefaultObject(const FString& ClassPath, const TCHAR* Property, int32 Element = 0);
	TArray<FUE2ObjectRef> DefaultObjectArray(const FString& ClassPath, const TCHAR* Property);
	FName DefaultName(const FString& ClassPath, const TCHAR* Property, int32 Element = 0);
	/** The pawn sound group (XGame.xPawnSoundGroup): Sounds[type], DeathSounds, PainSounds. */
	void PlayPawnSound(const FUE2SvActor& Pawn, const AArenaCharacter* Ch, const TCHAR* Array, int32 Index, float Volume, float Radius, int32 ExcludeConn);
	/** The character's xPawnSoundGroup class (species and sex of its .upl record). */
	FString SoundGroupOf(const AArenaCharacter* Ch);
	/** Character name -> sound group class. */
	TMap<FString, FString> SoundGroups;
	/** The winner's pawn at the end of the match (always relevant: every view follows it). */
	int32 EndGameFocusActor = 0;
	/** Pawn events seen between refreshes: pain, landing, jumping. */
	void PawnEventSounds(FUE2SvActor& A, AArenaCharacter* Ch);
	/** The link gun's beam (XWeapons.LinkBeamEffect) and the shield gun's shield (ShieldEffect3rd*) of a pawn. */
	void RefreshWeaponEffects(FUE2SvActor& Pawn, int32 AttachmentId, AArenaWeapon* Weapon, int32 Team);
	/** Countdown and match end messages / announcements. */
	void RefreshAnnouncements(AArenaGameState* GS);
	/** Instant hit shots: bullet impacts (attachment mHitLocation / SpawnHitCount) or a beam effect actor. */
	void SpawnShotEffect(const FUE2SvActor& Pawn, FUE2SvActor& Attachment, const AArenaWeapon* Weapon);

	FUE2Server& Server;
	TWeakObjectPtr<UWorld> World;
	FString MapName;
	FString ModeId;
	TMap<TWeakObjectPtr<UObject>, int32> NativeActors;
	TMap<int32, FRemotePlayer> Players;
	int32 GRIActor = 0;
	int32 LevelInfoActor = 0;
	int32 TeamActors[2] = { 0, 0 };
	int32 FlagBaseActors[2] = { 0, 0 };
	/** "A" / "B" -> the map's xDomPointA / xDomPointB actor name. */
	TMap<FString, FString> DomPointNames;
	/** Weapon attachment actor per pawn actor. */
	TMap<int32, int32> Attachments;
	/** Pawn actor -> when it died (torn off; its actor goes a little later). */
	TMap<int32, double> DeadPawns;
	/** Last weapon shot counts (FlashCount). */
	TMap<int32, int32> FlashCounts;
	TMap<int32, int32> HitscanCounts;
	/** Short-lived effect actors (beams) and when they were made. */
	TMap<int32, double> TempActors;
	double StartTime = 0.0;
	double LastRemainingMinute = 0.0;
	int32 KillMessages = 0;
	int32 ChatLines = 0;
	int32 SoundsSent = 0;
	struct FPawnState { float Health = -1.f; uint8 Physics = 0; double LastPain = 0.0; float FallSpeed = 0.f; };
	TMap<int32, FPawnState> PawnStates;
	/** Pickup actor -> available at the last refresh. */
	TMap<int32, bool> PickupStates;
	/** Pawn actor -> its link beam / shield effect actor. */
	TMap<int32, int32> BeamActors;
	TMap<int32, int32> ShieldActors;
	/** Vehicle actor -> its gun actors; driver character -> its vehicle actor (this refresh). */
	TMap<int32, TArray<int32>> VehicleGuns;
	TMap<const AActor*, int32> DriverVehicles;
	int32 LastCountdown = -1;
	uint8 LastPhase = 255;
	struct FFireSound { FUE2ObjectRef Sound; float Volume = 1.f; float Radius = 300.f; };
	TMap<FString, FFireSound> FireSounds;
};

/** Calls from the game into the UE2 server (no-ops while no UE2 server runs). */
namespace UE2ServerHooks
{
	OPENUT2K4_API void Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot);
	/** CTF flag events: UE2 CTFMessage switches (0 captured, 1 returned, 2 dropped, 3 returned by a player, 4 taken, 6 picked up). */
	OPENUT2K4_API void FlagEvent(AArenaFlag* Flag, AController* By, int32 Switch);
	/** End of the match on a UE2 server: the next map of the rotation, clients told to follow (ClientTravel). False when not hosting. */
	OPENUT2K4_API bool TravelToNextMap(class AArenaGameMode* GM);
	/** The same to a given map ("DM-Rankin"; the current one restarts it). False when not hosting. */
	OPENUT2K4_API bool TravelToMap(class AArenaGameMode* GM, const FString& MapName);
	/** A stock local message (class path) to every UE2 client, or only to OnlyTo when that is a remote player. */
	OPENUT2K4_API void LocalMessage(const TCHAR* MessageClass, int32 Switch, APlayerState* PRI1, APlayerState* PRI2, AController* OnlyTo);
	/** Chat said by a local (listen host) player: relayed to the UE2 clients. */
	OPENUT2K4_API void Chat(APlayerState* From, const FString& Text, bool bTeam);
}

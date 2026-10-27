#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Components/CapsuleComponent.h"
#include "../ArenaCharacter.h"
#include "UE2NetPlay.generated.h"

class APlayerController;
class AArenaPlayerController;
class FUE2NetConnection;
class UWorld;
struct FUE2NetActor;
struct FUE2NetValue;
struct FUSFunction;
struct FUSClass;
enum class EArenaNavLinkType : uint8;

/**
 * The collision of our own pawn on a UT2004 server: UE2 pawns collide with the level (BSP, static meshes, movers) as
 * an axis-aligned box of CollisionRadius x CollisionRadius x CollisionHeight; the cylinder is only for actor against
 * actor. The body is that box (the pawn's body is never rotated). -UE2NetCapsuleCollision keeps the capsule (A/B test).
 */
UCLASS()
class OPENUT2K4_API UUE2NetBoxCollision : public UCapsuleComponent
{
	GENERATED_BODY()

public:
	virtual void UpdateBodySetup() override;
	virtual FCollisionShape GetCollisionShape(float Inflation = 0.0f) const override;
	static bool UseBox();
};

/**
 * Our own pawn on a UT2004 server (step 4 of Docs/UE2Net.md): a locally controlled AArenaCharacter with the UE2
 * movement rules, predicted on the client like UE2's PlayerController (saved moves, ServerMove, corrections replayed).
 * It never deals or takes damage, picks up items or dies by itself: the server decides all of that, and the world
 * layer (FUE2NetPlay) applies what the server replicates to its owner (health, shield, adrenaline, weapons, ammo).
 */
UCLASS(NotPlaceable, Transient)
class OPENUT2K4_API AUE2NetLocalPawn : public AArenaCharacter
{
	GENERATED_BODY()

public:
	AUE2NetLocalPawn(const FObjectInitializer& ObjectInitializer);
	/** Dodges are relative to the view (the body stays unrotated: its box is axis-aligned). */
	virtual float GetMovementYaw() const override;

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override { return 0.f; }
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override {}
	/** Combo input: asks the server (xPlayer.ServerDoCombo) instead of starting the combo locally. */
	virtual void ActivateCombo(EArenaCombo Combo) override;
	/** Shows the server's active combo (xPawn.CurrentCombo) without asking the server again. */
	void SetNetCombo(EArenaCombo Combo);
	/** Driving (UE2 PlayerDriving): the vehicle the camera stays behind; our body is hidden and does not move. */
	TWeakObjectPtr<AActor> DrivenVehicle;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;
	/** Adds a weapon of our inventory (view model and HUD) without the local default inventory or auto switching: our
	 *  WeaponClass standing in for the server's UE2Class (a mod weapon gets the mod's own assets, Net/UE2NetModWeapons.h). */
	class AArenaWeapon* EnsureNetWeapon(UClass* WeaponClass, const FString& UE2Class);

	/**
	 * The server's walking xPawn rests NetWalkExtraHeight UU higher above a floor than a pawn of its own collision
	 * height touching it (measured: 5.85 UU against our 1.07), but falls until it touches. Our pawn walks with that
	 * much taller a capsule and falls with the plain one, the location kept in the air and lifted on landing.
	 */
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode = 0) override;
	void ApplyNetCollision();
	/** Set while a server correction sets the movement mode: no lift on "landing". */
	bool bNoLandingLift = false;

protected:
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	bool bSettingNetCombo = false;
};

/**
 * Step 4 of Docs/UE2Net.md: playing on a UT2004 server. Created with the world layer (FUE2NetWorld) when our copy of
 * the server's map has loaded; gets the same connection events first.
 *
 *  - our pawn: the PlayerController's Pawn / ClientRestart -> AUE2NetLocalPawn possessed by the local player, and
 *    ServerAcknowledgePossession; the xPawn's owner-only values (Health, ShieldStrength, GroundSpeed ...) -> it
 *  - movement: every physics step is a saved move, sent as ServerMove / DualServerMove / ShortServerMove (UE2
 *    PlayerController.ReplicateMove / CallServerMove formats); ClientAdjustPosition / ShortClientAdjustPosition /
 *    VeryShortClientAdjustPosition / LongClientAdjustPosition reset the pawn and replay the newer saved moves
 *  - input: fire / alt fire (Weapon.ServerStartFire / ServerStopFire), weapon switching (Pawn.ServerChangedWeapon,
 *    Weapon.ClientWeaponSet), respawn (ServerReStartPlayer), use, suicide, team change, ready, chat (console
 *    commands typed by the player only), combos (xPlayer.ServerDoCombo)
 *  - spectating (no pawn): the free camera's position steers the server's spectator through ServerMove, so the
 *    server's relevancy follows our view
 */
class OPENUT2K4_API FUE2NetPlay
{
public:
	FUE2NetPlay(FUE2NetConnection& InConnection, UWorld* InWorld);
	~FUE2NetPlay();

	static FUE2NetPlay* Get();
	/** The server we play on (host:port, as joined). */
	FString GetServerAddress() const;

	void ActorOpened(const FUE2NetActor& Actor);
	void ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed);
	void ActorClosed(const FUE2NetActor& Actor);
	/** True when the call was handled here (movement corrections, possession, weapons ...). */
	bool FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	/** Net ticker (before the world ticks). */
	void Tick(float DeltaSeconds);

	/** Our own xPawn's channel: the world layer leaves it to us. */
	bool IsOwnPawnChannel(int32 Channel) const { return Channel != 0 && Channel == OwnPawnChannel && !DrivingChannel; }
	/** True while we play (a pawn, dead or waiting to respawn) rather than spectate. */
	bool IsPlaying() const;
	/** A pawn was given to us at least once on this map. */
	bool HasSpawned() const { return Spawns > 0; }
	/** Fire asked the server to start us (ServerReStartPlayer) since our last pawn: before the match begins that is our
	 *  ready signal (PlayerController.ServerReStartPlayer: bReadyToPlay while bWaitingToStartMatch). */
	bool HasRequestedRestart() const { return RestartsRequested > Spawns; }
	/**
	 * Without a pawn, the server keeps us spectating until something else spawns us: our PRI is out of lives (round
	 * games such as 3SPN / Freon put players who join mid-round there until the next round starts; LMS) or the
	 * server's controller is in its Spectating state. Fire then views the next player instead of asking to restart.
	 */
	bool IsWaitingForRound() const;
	/** Playing, but the server holds us as a spectator (our PlayerReplicationInfo's bOnlySpectator, e.g. a player
	 *  agreement not answered yet): Fire cannot spawn us. */
	bool IsHeldAsSpectator() const;

	enum class EInput : uint8 { Fire, AltFire, FireRelease, AltFireRelease, NextWeapon, PrevWeapon, Slot, Use, Suicide };
	/** Input of the local player while on the server; true when handled (the local game must not act on it). */
	bool HandleInput(APlayerController* PC, EInput Input, int32 Value = 0);

	// console / script commands
	void Say(const FString& Text, bool bTeam);
	void ChangeTeam(int32 Team);
	void Ready();
	/** UTComp's NotReady (warmup); nothing on stock servers (3369 has no "not ready" call). */
	void NotReady();
	/** A mod's warmup line for the HUD ("Warmup: 2 of 4 ready ..."), else empty. */
	FString GetModStatusText() const;
	/** Actor channel of the weapon we hold (0: none), for the script HUD's Pawn.Weapon (UE2NetScriptHUD). */
	int32 GetCurrentWeaponChannel() const { return CurrentWeaponChannel; }
	void Suicide();
	void Use();
	void RequestRestart();
	/** xPlayer.ServerDoCombo(ComboClass). */
	void DoCombo(FUSClass* ComboClass);
	/** Our predicted pawn (null while dead, waiting or spectating). */
	AUE2NetLocalPawn* GetLocalPawn() const { return LocalPawn.Get(); }
	/** The first-person view model of our weapon of that channel when the mod's script runs it (its client-side effects
	 *  attach there), else null. */
	class USceneComponent* GetScriptWeaponComponent(int32 WeaponChannel) const;

	/** "Health 100, ...": state line for logs and the spectator bar. */
	FString DescribeState() const;
	FString DescribeScore() const;


private:
	struct FMove
	{
		float TimeStamp = 0.f;
		float Delta = 0.f;
		/** Acceleration of the step (cm/s^2) and its input vector (for the replay). */
		FVector Accel = FVector::ZeroVector;
		FVector InputVector = FVector::ZeroVector;
		AArenaCharacter::FUE2StepInput In;
		FRotator View = FRotator::ZeroRotator;
		/** Location / velocity after the step (cm). */
		FVector EndLoc = FVector::ZeroVector;
		FVector EndVel = FVector::ZeroVector;
		/** UE2 bJumpStatus after this move (toggles with each jump press). */
		bool bJumpStatus = false;
		bool bSent = false;
		/** Walking at the end of the step. */
		bool bEndWalking = false;
		/** Floor slope class while walking (-1: flat or not walking) and the learned height difference sent for it (UU). */
		int32 SlopeBucket = -1;
		uint32 FloorKey = 0;
		float SlopeZ = 0.f;
		/** The floor at the end of the step (diagnostics). */
		TWeakObjectPtr<const UPrimitiveComponent> FloorComp;
		/** The mover we stand on at the end of the step and its location then (cm): the server's location on a lift is relative to it. */
		TWeakObjectPtr<const AActor> EndBase;
		FVector EndBaseLoc = FVector::ZeroVector;
		bool IsImportant() const { return In.bPressedJump || In.bDoubleJump || (In.DoubleClickMove != 0 && In.DoubleClickMove < 5); }
	};

	struct FWeaponInfo
	{
		int32 Channel = 0;
		FString ClassPath;
		UClass* Native = nullptr;
		int32 Group = 0;
		float Priority = 0.f;
		int32 Ammo = 0;
		int32 AmmoChannel = 0;
	};

	void PreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	void RecordMove(float DeltaSeconds);
	void SendMoves(bool bForce);
	void SendSpectatorMove(float DeltaSeconds);
	// driving (UE2 PlayerController.PlayerDriving: ServerDrive, Vehicle.VehicleFire / VehicleCeaseFire)
	bool IsVehicleChannel(int32 Channel) const;
	void EnterVehicle(int32 Channel);
	void ExitVehicle(bool bToFoot);
	void SendDrive();
	void TickFireEffects();
public:
	/** PlayerController.Mutate's server call (ServerMutate). */
	void Mutate(const FString& Text);
private:
	/** World time of the last stand-in fire-mode tick (Net/UE2NetScriptHUD: the mod's fire timing). */
	float LastStandInTick = 0.f;
	// guided Redeemer (UE2 PlayerController.PlayerRocketing: the RedeemerWarhead the server made our pawn)
	void StartWarhead(const FUE2NetActor& A);
	void EndWarhead();
	void SendRocketMove();
	/** NewNet weapons: the deferred start fire (sent when the weapon is ready, as UE2's ClientStartFire retries). */
	void TickNewNetFire();
	/** Channel of the server pawn / vehicle we show first along a ray (UU-scaled cm), before the world; 0 for none. */
	int32 TraceShownPawn(const FVector& Start, const FVector& Dir, float Range) const;
	void CallServerMove(const FMove* Pending, const FMove& Move, const FMove* Old);
	void ClientAdjust(const TArray<TPair<FName, FUE2NetValue>>& Params, bool bHasVelocity, bool bVeryShort);
	void ReplayMovesAfter(float TimeStamp);
	void UpdatePing(float TimeStamp);

	void SetOwnPawnChannel(int32 Channel, const TCHAR* Why);
	void ApplyOwnName(const FUE2NetActor& PRI);
	void TrySpawnLocalPawn();
	void DestroyLocalPawn(bool bDied);
	void ApplyOwnPawn(const FUE2NetActor& A, const TArray<FString>* Changed);
	void ApplyWeapons();
	void SwitchToWeapon(int32 WeaponChannel);
	void CycleWeapon(int32 Dir);
	void SelectGroup(int32 Group);

	int32 PCChannel() const;
	AArenaPlayerController* GetPC() const;
	bool Send(int32 Channel, const TCHAR* Function, const TArray<FUE2NetValue>& Params);
	int32 ViewInt(const FRotator& View) const;
	void TickScript(double Now);
	void LogSummary();

	FUE2NetConnection* Connection = nullptr;
	TWeakObjectPtr<UWorld> World;
	FDelegateHandle PostTickHandle;
	FDelegateHandle PreTickHandle;
	TWeakObjectPtr<AUE2NetLocalPawn> LocalPawn;

	int32 OwnPawnChannel = 0;
	int32 OwnPRIChannel = 0;
	/** The vehicle (or gunner seat) we drive, 0 on foot. */
	int32 DrivingChannel = 0;
	/** The guided Redeemer warhead we fly (our pawn while it flies), 0 when none. */
	int32 WarheadChannel = 0;
	TWeakObjectPtr<class AArenaGuidedWarhead> NetWarhead;
	/** RocketServerMove moves the server has not answered yet (UU): replayed after its corrections. */
	struct FRocketMove
	{
		float TimeStamp = 0.f;
		float Delta = 0.f;
		FVector AccelUU = FVector::ZeroVector;
	};
	TArray<FRocketMove> RocketMoves;
	int32 RocketMovesSent = 0;
	int32 WarheadAdjustments = 0;
	float LastDriveSend = -1.f;
	FVector4f LastDriveInput = FVector4f(0.f, 0.f, 0.f, 0.f);
	int32 LastDriveView = 0;
	int32 DriveUpdates = 0;
	int32 VehicleEntries = 0;
	int32 LadderEntries = 0;
	float DrivingTime = 0.f;
	int32 AckedPawnChannel = 0;
	/** The pawn channel whose bTearOff we saw (dead until the next pawn). */
	int32 DeadPawnChannel = 0;
	FName ServerState;
	bool bReadySent = false;
	/** PlayerController.ServerShortTimeout sent (3369's PlayerTick: once the client has precached), and when our controller arrived. */
	bool bShortTimeoutSent = false;
	double ControllerArrivedTime = 0.0;

	// moves
	TArray<FMove> SavedMoves;
	bool bJumpStatus = false;
	float LastSendTime = -1.f;
	/** The server's ClientCapBandwidth (bytes per second, 0: none). */
	int32 ClientCap = 0;
	float LastAdjustTimeStamp = -1.f;
	/** PlayerController.ExactPing (s) from the adjustments' TimeStamps, sent every 4 s (ServerUpdatePing). */
	float ExactPing = 0.f;
	double LastPingUpdate = -100.0;
	float LastTimeStamp = 0.f;
	/** Spectating: where the server's spectator is (UE2 units), as far as we know (its corrections tell us). */
	FVector ServerSpecLoc = FVector::ZeroVector;
	bool bServerSpecKnown = false;
	/**
	 * How much higher (UU) the server's walking pawn rests above a floor than ours. UE2 keeps walking pawns
	 * MINFLOORDIST..MAXFLOORDIST (1.9..2.4) UU above the floor; on our converted level the measured gap is larger.
	 * Learned from the server's corrections while walking; ClientLoc and corrections are shifted by it.
	 */
	float ZOffset = 0.f;
	/**
	 * On slopes and stairs our capsule rests at another height than the server's cylinder: a learned difference per
	 * floor steepness class (the floor normal's Z in steps of 0.02), from corrections that differ only in height.
	 */
	float SlopeOffsets[16] = {};
	int32 SlopeSamples[16] = {};
	TMap<uint32, float> FloorOffsets;
	TMap<uint32, int32> FloorSamples;
	void ClassifyFloor(FMove& M) const;
	int32 ZOffsetSamples = 0;

	// weapons (owner-only actors)
	TMap<int32, FWeaponInfo> Weapons;
	int32 CurrentWeaponChannel = 0;
	double LastSpawnRealTime = 0.0;
	/** Since when our pawn looks unpossessed (Tick: let go of it after a moment). */
	double UnpossessedSince = 0.0;
	double NextIgnoreRefresh = 0.0;
	double LastRestartRequest = 0.0;
	bool bFireHeld[2] = { false, false };
	float NextFireEffect[2] = { 0.f, 0.f };
	/** The weapon whose hold effects TickFireEffects drives (AArenaWeapon::SetNetFireHeld). */
	TWeakObjectPtr<class AArenaWeapon> NetFireWeapon;
	/** A charge released by itself (the shield gun hit someone): no new charge until the button comes up. */
	bool bNetWaitRelease[2] = { false, false };
	float NextMeleeTest = 0.f;
	/** NewNet (Net/UE2NetNewNet.h): a held fire whose NewNet start fire is not sent yet (the weapon is not ready). */
	bool bNewNetFirePending[2] = { false, false };
	TUniquePtr<class FUE2NetNewNet> NewNet;
	/** Client-side parts of server mods (Net/UE2NetModClient.h): hit sounds, ready-up, warmup. */
	TUniquePtr<class FUE2NetModClient> ModClient;
	bool bInActorOpen = false;

	// statistics
	int32 MovesSent = 0;
	int32 PacketsWithMoves = 0;
	int32 DualMoves = 0;
	int32 ShortMoves = 0;
	int32 Adjustments = 0;
	int32 Corrections = 0;
	int32 IgnoredAdjustments = 0;
	double SumCorrection = 0.0;
	float MaxCorrection = 0.f;
	int32 ReplayedMoves = 0;
	int32 SpectatorAdjustments = 0;
	int32 Deaths = 0;
	int32 Spawns = 0;
	int32 RestartsRequested = 0;
	int32 FireCommands = 0;
	int32 WeaponSwitches = 0;
	double StartTime = 0.0;
	double NextSummary = 0.0;
	/** Correction samples (time, error in UU, move flags) for the log. */
	int32 CorrectionLogBudget = 60;

	// -UE2NetPlayScript="<s>:<cmd>;..." (test hook)
	struct FScriptStep { double Time = 0.0; FString Cmd; };
	TArray<FScriptStep> Script;
	/** Scripted movement input: forward / right axes (-1..1) and a turn rate (degrees per second). */
	FVector2D ScriptMove = FVector2D::ZeroVector;
	float ScriptTurn = 0.f;
	bool bScriptFire[2] = { false, false };
	/** Scripted route: way points (cm) walked in order, looping. */
	TArray<FVector> ScriptRoute;
	int32 ScriptRouteIndex = 0;
	FVector ScriptLastProgressLoc = FVector::ZeroVector;
	double ScriptLastProgressTime = 0.0;
	bool bScriptRoute = false;
	FString ScriptRouteKind;
	bool bRouteRequeued = false;
	int32 ScriptStuckCount = 0;
	bool ScriptRouteGoalStop = false;
	int32 ScriptVehicleChannel = 0;
	FVector ScriptGotoTarget = FVector::ZeroVector;
	double ScriptLastUseTime = 0.0;
	FVector RouteDirOverride = FVector::ZeroVector;
	double ScriptBackOffUntil = 0.0;
	FVector ScriptBackOffDir = FVector::ZeroVector;
	/** route=flag: 1 on the way to the enemy flag, 2 on the way home. */
	int32 ScriptFlagStage = 0;
	TArray<EArenaNavLinkType> ScriptRouteLinks;
	bool PlanFlagLeg(AUE2NetLocalPawn* Pawn, int32 Stage);
	bool bScriptCanChat = false;
	bool bScriptDoubleJump = false;
	bool bScriptHunt = false;
	/** hunt=tap: one press per shot (aimed at the moment of each shot) instead of holding fire. */
	bool bScriptHuntTap = false;
	double ScriptTapRelease = 0.0;
	/** -UE2NetNoNewNet (test hook): NewNet weapons get the stock ServerStartFire (no lag compensation), for A/B. */
	bool bNoNewNet = false;
	/** -UE2NetPlayProbe (local servers): ClientLoc is sent off by 500 UU so the server reports its state for every move. */
	bool bProbe = false;
	// special moves (jumps, double jumps, dodges) and the corrections in the second after one
	int32 NumJumps = 0;
	int32 NumDoubleJumps = 0;
	int32 NumDodges = 0;
	int32 CorrectionsAfterSpecial = 0;
	float LastSpecialTime = -100.f;
};

namespace ArenaNet
{
	/** Input of the local player (FUE2NetPlay::EInput as int) while playing on a UT2004 server. True when handled. */
	OPENUT2K4_API bool HandleNetPlayInput(APlayerController* PC, int32 Input, int32 Value = 0);
}

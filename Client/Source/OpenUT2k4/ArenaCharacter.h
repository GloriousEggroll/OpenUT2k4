#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "ArenaTypes.h"
#include "ArenaDamage.h"
#include "ArenaCharacterAnim.h"
#include "ArenaCharacter.generated.h"

class UCameraComponent;
class USceneComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;
class UMaterialInstanceDynamic;
class AArenaWeapon;
class AArenaFlag;
class AArenaCarryable;

/**
 * Player / bot pawn. Body: an imported / replacement player model (skeletal mesh on ACharacter's Mesh, animated
 * natively by UArenaCharacterAnimInstance) when the art is available, otherwise a procedural body made from
 * shape meshes. First-person camera,
 * UT-style movement (double jump, dodge, wall dodge), health/shield/adrenaline, weapon inventory.
 */
UCLASS()
class OPENUT2K4_API AArenaCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AArenaCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// ---- components ----
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
	/** View models of weapons attach here (child of Camera). */
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> WeaponRoot;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> BodyRoot;

	// ---- vitals ----
	UPROPERTY(VisibleInstanceOnly) float Health = ArenaTuning::StartHealth;
	UPROPERTY(VisibleInstanceOnly) float Shield = 0.f;
	UPROPERTY(VisibleInstanceOnly) float Adrenaline = 0.f;

	bool IsDead() const { return bDead; }
	bool IsAlive() const { return !bDead; }
	int32 GetTeam() const;

	/** Returns true if anything was given. MaxAmount caps the result (e.g. 100 for packs, 199 for vials). */
	bool GiveHealth(float Amount, float MaxAmount);
	bool GiveShield(float Amount, float MaxAmount = ArenaTuning::MaxShield);
	/** UE2 xPawn.AddShieldStrength (shield packs 50 / 100, booster combo 5). */
	bool AddShieldStrengthUE2(int32 ShieldAmount);
	/** UE2 xPawn.ShieldAbsorb: the damage left after the armour. */
	float ShieldAbsorbUE2(float Damage);
	/** UE2 xPawn.SmallShieldStrength: the part of the shield from 50 packs (absorbs 50% instead of 75%). */
	float SmallShield = 0.f;
	/** UE2 Pawn.HeadScale (xPawn.SetHeadScale: the 'head' bone is scaled, e.g. by the Big Head mutator); the head hit
	 * sphere scales with it (IsHeadShot: HeadHeight x HeadScale, HeadRadius x HeadScale). */
	float HeadScale = 1.f;
	/** UE2 IsHeadShot: the shot ray passes within HeadRadius 9 UU of the head (bone + HeadHeight 6 UU). */
	bool IsHeadshotUE2(const FVector& HitLocation, const FVector& RayDir) const;
	/** UE2 xPawn.CheckReflect: the held Shield Gun reflects a shot hitting at HitLocation (OutRefNormal = view
	 * direction) and drains AmmoDrain energy. */
	bool CheckShieldReflect(const FVector& HitLocation, FVector& OutRefNormal, int32 AmmoDrain);
	bool GiveAdrenaline(float Amount);
	/** UE2 Controller.AwardAdrenaline (may be negative; also during a combo). */
	void AwardAdrenaline(float Amount);
	/** UE2 combo Duration (Speed 16 s, the others 30 s): AdrenalineCost 100 drains over it. */
	static float GetComboDuration(EArenaCombo Combo);
	/** Gives the weapon if not owned (and switches if better), otherwise adds its pickup ammo. Returns true if anything changed. */
	bool GiveWeapon(TSubclassOf<AArenaWeapon> WeaponClass);
	/** Adds ammo for a weapon class (stored even if the weapon isn't owned yet). Returns true if anything changed. */
	bool GiveAmmo(TSubclassOf<AArenaWeapon> WeaponClass, int32 Amount);
	void GiveDoubleDamage(float Duration = ArenaTuning::DoubleDamageDuration);
	bool HasDoubleDamage() const;
	float GetDoubleDamageRemaining() const;

	// ---- weapons ----
	AArenaWeapon* GetCurrentWeapon() const { return CurrentWeapon; }
	const TArray<TObjectPtr<AArenaWeapon>>& GetWeapons() const { return Weapons; }
	AArenaWeapon* FindWeapon(TSubclassOf<AArenaWeapon> WeaponClass) const;
	void SwitchToWeapon(AArenaWeapon* Weapon);
	void SwitchToSlot(int32 Slot);
	void NextWeapon();
	void PrevWeapon();
	/** Switch to the best weapon that has ammo (called when current runs dry). */
	void SwitchToBestWeapon();
	/**
	 * Removes Weapon from the inventory without destroying it. If it was the current weapon it is unequipped and
	 * the best remaining weapon is selected. Returns false if the weapon was not in the inventory.
	 */
	bool RemoveWeapon(AArenaWeapon* Weapon);
	void StartFire(uint8 Mode);
	void StopFire(uint8 Mode);
	/** Damage multiplier from double damage / berserk */
	float GetDamageScale() const;
	/** Refire time multiplier (berserk < 1) */
	float GetRefireScale() const;

	// ---- movement (UT2004 rules: XGame.xPawn, UnrealGame.UnrealPlayer, Engine.PlayerController / PlayerInput) ----
	/**
	 * Direction key went down (player controller): combo input, and the UE2 double-click detection
	 * (PlayerInput.CheckForDoubleClickMove, run before the next physics step) that dodges.
	 */
	void OnDirectionTap(EArenaDir Dir);
	/** Dodge key: dodge in the direction of the held movement keys (same rules as a double tap). */
	void OnDodgeKey();
	/** The yaw dodges and the dodge key are relative to (UE2 Pawn.Rotation.Yaw; the net play pawn keeps its body unrotated). */
	virtual float GetMovementYaw() const { return GetActorRotation().Yaw; }
	/** UE2 xPawn.Dodge in a view-relative direction (wall dodge when falling). Returns true if a dodge happened. */
	bool Dodge(EArenaDir Dir);
	/** World-space dodge for bots (UE2 AI dodge: no sideways velocity kept; wall needed when falling). */
	bool DodgeWorld(const FVector& WorldDir);
	/** Jump button (players and bots): handled at the next physics step like UE2 bPressedJump (jump or double jump). */
	virtual void Jump() override;
	virtual void StopJumping() override;
	/** UE2 xPawn.DoJump: double jump when allowed, else Pawn.DoJump from the ground. bUpdating (a replayed net move):
	 * no double jump here (UE2 xPawn.DoJump skips it while bUpdating; the move's bDoubleJump replays it). */
	bool DoJumpUE2(bool bUpdating = false);
	/** UE2 xPawn.CanDoubleJump / DoDoubleJump. */
	bool CanDoubleJumpUE2() const;
	bool DoDoubleJumpUE2();
	/** Crouch / walk buttons (players): applied while not falling, like UE2 PlayerWalking.ProcessMove. */
	void SetDuckInput(bool bDown) { bDuckHeld = bDown; }
	bool IsJumpHeld() const { return bJumpHeld; }
	bool IsDuckHeld() const { return bDuckHeld; }
	void SetWalkInput(bool bDown) { bWalkHeld = bDown; }
	/** Called by UArenaCharacterMovement before each physics step (UE2 PlayerController.PlayerMove / ProcessMove). */
	void PreMovementUpdate(float DeltaTime);
	/** UE2 Controller.DoubleClickDir: 0 none, 1 left, 2 right, 3 forward, 4 back (first tap seen), 5 active (dodging), 6 done (landed). */
	uint8 GetDoubleClickDir() const { return DoubleClickDir; }
	int32 GetMultiJumpRemaining() const { return MultiJumpRemaining; }
	/** Last movement events (for tests / debugging): take-off of a jump, double jump or dodge, landing, momentum push. */
	struct FMoveRecord
	{
		FVector TakeoffLocation = FVector::ZeroVector;
		FVector TakeoffVelocity = FVector::ZeroVector;
		/** Feet location and horizontal velocity right after landing (after the dodge landing slowdown). */
		FVector LandLocation = FVector::ZeroVector;
		FVector LandVelocity = FVector::ZeroVector;
		FVector LandImpactVelocity = FVector::ZeroVector;
		FVector PushVelocityBefore = FVector::ZeroVector;
		FVector PushVelocityAfter = FVector::ZeroVector;
		int32 Takeoffs = 0;
		int32 Pushes = 0;
		int32 Jumps = 0;
		int32 DoubleJumps = 0;
		int32 Dodges = 0;
		int32 WallDodges = 0;
	};
	const FMoveRecord& GetMoveRecord() const { return MoveRecord; }
	FVector GetLastLandedVelocity() const { return MoveRecord.LandVelocity; }
	/** UE2 Pawn.AddVelocity (momentum from damage): knocks a walking pawn into falling, halves extra lift above 380 UU/s. */
	void AddVelocityUE2(const FVector& NewVelocity);
	/** UE2 Pawn.TakeDamage momentum rules (extra lift when walking, 0.6 when self inflicted), then AddVelocityUE2. */
	void TakeMomentumUE2(const FVector& Momentum, bool bSelf);

	// UE2 movement properties (centimetres): xPawn defaults
	float MultiJumpBoost = ArenaTuning::MultiJumpBoost;
	float DodgeSpeedFactor = ArenaTuning::DodgeSpeedFactor;
	float DodgeSpeedZ = ArenaTuning::DodgeSpeedZ;
	float MaxFallSpeed = ArenaTuning::MaxFallSpeed;
	bool bCanWallDodge = true;
	bool bCanDodgeDoubleJump = true;
	bool bCanBoostDodge = false;

	// ---- UE2 net play (Net/UE2NetPlay.h: our own pawn on a UT2004 server) ----
	/** The inputs of one physics step, as UE2 PlayerController.ServerMove / SavedMove carry them. */
	struct FUE2StepInput
	{
		/** UE2 eDoubleClickDir passed to ProcessMove (0 none, 1-4 dodge, 5 active). */
		uint8 DoubleClickMove = 0;
		bool bPressedJump = false;
		/** The step did a double jump (UE2 PlayerController.bDoubleJump). */
		bool bDoubleJump = false;
		/** UE2 bRun: the walk key. */
		bool bRun = false;
		bool bDuck = false;
	};
	/** Filled by PreMovementUpdate for every physics step (net play records it as a saved move). */
	FUE2StepInput LastStepInput;
	/** Net play replay (UE2 ClientUpdatePosition): when set, PreMovementUpdate uses these inputs instead of the player's. */
	const FUE2StepInput* StepInputOverride = nullptr;
	/** Net play: a predicted proxy of a UT2004 server's pawn. No local damage, deaths or pickups: the server decides. */
	bool bNetProxy = false;

	// ---- combos ----
	EArenaCombo GetActiveCombo() const { return ActiveCombo; }
	float GetComboRemaining() const;
	virtual void ActivateCombo(EArenaCombo Combo);
	bool IsInvisible() const { return ActiveCombo == EArenaCombo::Invisible; }

	// ---- carried objectives ----
	/** CTF flag being carried (managed by AArenaFlag). */
	UPROPERTY() TObjectPtr<AArenaFlag> CarriedFlag;
	/** Generic carried object (ball ...; managed by AArenaCarryable). Dropped on death / removal. */
	UPROPERTY() TObjectPtr<AArenaCarryable> CarriedObject;
	bool HasFlag() const { return CarriedFlag != nullptr; }
	AArenaCarryable* GetCarriedObject() const { return CarriedObject; }
	/** True when carrying a flag or any carryable. */
	bool IsCarryingObjective() const { return CarriedFlag != nullptr || CarriedObject != nullptr; }

	/** "Use" action (E key for players, AArenaBotController::RequestUse for bots): forwarded to the game rules. */
	bool Use();

	// ---- damage ----
	/** Main damage entry point used by ArenaDamage. Returns damage actually applied to health. */
	float ReceiveArenaDamage(float Damage, AController* InstigatorController, AActor* DamageCauser, const FArenaHitInfo& Info);
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	/** Kill instantly (falling out of world, suicide command). */
	void Suicide();
	/** UE2 Pawn.gibbedBy: killed and gibbed (telefrags, translocating to a disrupted beacon); no armour or protection. */
	void DieGibbed(AController* Killer, const FString& WeaponName);
	/** World time of the last spawn (UE2 Pawn.SpawnTime). */
	float GetSpawnTime() const { return SpawnTime; }
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;

	/** Location of the head (for headshot checks and bot aim). */
	FVector GetHeadLocation() const;
	bool IsHeadshot(const FVector& HitLocation) const;

	/** Recolour the body for the current team. */
	void UpdateTeamColors();

	/** True for SpawnProtectionTime after spawning (invulnerable until it fires). */
	bool HasSpawnProtection() const;

	// ---- skeletal body / animation ----
	/** True when the imported / replacement player model is shown instead of the procedural body. */
	bool IsUsingSkeletalBody() const { return bSkeletalBody; }
	/** Counters and directions the character animation reacts to. */
	const FArenaAnimEvents& GetAnimEvents() const { return AnimEvents; }
	/** Called by the current weapon for every shot (drives the fire animation). */
	void NotifyWeaponFired(uint8 Mode);
	/** Third-person weapon in the model's right hand (nullptr if none). */
	USkeletalMeshComponent* GetThirdPersonWeapon() const { return ThirdPersonWeapon; }
	/** Dual assault rifles: the second rifle in the left hand (UE2 AssaultRifle.OffhandActor), nullptr if none. */
	USkeletalMeshComponent* GetOffhandWeapon() const { return OffhandWeapon; }
	/** UE2 RedeemerGuidedFire: the player flying a guided warhead while this body stays behind (kill credit / respawn). */
	void SetRemoteController(AController* C) { RemoteController = C; }
	AController* GetRemoteController() const { return RemoteController.Get(); }
	/** Re-attach the third-person weapon(s) (e.g. after a second assault rifle was picked up). */
	void RefreshThirdPersonWeapon() { UpdateThirdPersonWeapon(); }
	/** World location of the third-person weapon's muzzle socket. False if the weapon has none / is hidden. */
	bool GetThirdPersonMuzzle(FVector& OutLocation) const;
	/** True when some local player looks through this pawn's first-person camera. */
	bool IsViewedInFirstPerson() const;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;

	/** Last time we took damage / were hit (for HUD / bots) */
	float LastHitTime = -100.f;
	TWeakObjectPtr<AController> LastDamager;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	/** UE2 Pawn.UpdateEyeHeight + CheckBob for the first-person camera (players). */
	void TickViewUE2(float DeltaSeconds);
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void Die(AController* Killer, AActor* DamageCauser, const FString& WeaponName);
	/**
	 * Death visuals of a subclass with its own body (Invasion monsters). Return true when handled; otherwise the
	 * player model ragdolls (or plays its death animation), or gibs when bGib (health below the gib threshold).
	 * Velocity includes the killing blow's momentum.
	 */
	virtual bool HandleDeathBody(bool bGib, const FVector& Velocity) { return false; }
	void GiveDefaultInventory();
	void BuildBody();
	void UpdateMovementForCombo();
	void ClearInventory();
	/** Switch the body to the player model picked for this player (no-op if the art is missing). */
	void SetupSkeletalBody();
	/** Team skin (or team tint) on the player model. */
	void ApplyBodySkin();
	/** Show the current weapon's third-person mesh in the right hand. */
	void UpdateThirdPersonWeapon();
	/** Debug helpers (arena.BehindView, arena.DebugCycleWeapons). */
	void TickArtDebug(float DeltaSeconds);

	UPROPERTY() TArray<TObjectPtr<AArenaWeapon>> Weapons;
	UPROPERTY() TObjectPtr<AArenaWeapon> CurrentWeapon;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> BodyParts;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> TeamMaterials;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> ThirdPersonWeapon;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> OffhandWeapon;
	TWeakObjectPtr<AController> RemoteController;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BodySkinMaterial;
	FArenaAnimEvents AnimEvents;
	/** Body skin base name of the player model (team textures are <name>_0 / _1). */
	FString BodySkinName;
	bool bAllowPlayerModel = true;
	/** Set by subclasses with their own body (Invasion monsters with UT2004 art): no player model, no procedural humanoid. */
	bool bCustomBody = false;
	bool bSkeletalBody = false;
	int32 AppliedSkinTeam = -100;
	int32 AppliedBehindView = -1;
	float DebugNextWeaponTime = 0.f;
	/** Ammo collected for weapons not owned yet */
	TMap<UClass*, int32> PendingAmmo;

	bool bDead = false;
	float SpawnTime = 0.f;
	float DoubleDamageEndTime = 0.f;
	EArenaCombo ActiveCombo = EArenaCombo::None;
	float ComboEndTime = 0.f;
	float LastDodgeTime = -100.f;
	/** UE2 PlayerInput double-click state (players). */
	uint8 DoubleClickDir = 0;
	float DoubleClickTimer = 0.f;
	/** Direction keys pressed since the last physics step (bit per UE2 DCLICK value 1..4). */
	uint8 PendingTapEdges = 0;
	bool bDodgeKeyPending = false;
	bool bPendingJump = false;
	bool bJumpHeld = false;
	bool bDuckHeld = false;
	bool bWalkHeld = false;
	int32 MultiJumpRemaining = 1;
	FMoveRecord MoveRecord;
	// UE2 first-person view state (cm): eye height above the capsule centre, landing dip, walk bob
	float ViewEyeHeight = ArenaTuning::EyeHeight;
	float ViewBaseEyeHeight = ArenaTuning::EyeHeight;
	float ViewOldZ = 0.f;
	float ViewLandBob = 0.f;
	float ViewBobTime = 0.f;
	float ViewAppliedBob = 0.f;
	FVector ViewWalkBob = FVector::ZeroVector;
	bool bViewJustLanded = false;
	bool bViewLandRecovery = false;
	bool bViewInit = false;
public:
	/** First-person eye height above the capsule centre and walk bob (camera-local), for tests. */
	float GetViewEyeHeight() const { return ViewEyeHeight; }
	FVector GetViewWalkBob() const { return ViewWalkBob; }
protected:
	void RecordTakeoff();
	/** UE2 PlayerInput.CheckForDoubleClickMove; returns the DCLICK move (0 none, 1-4 dodge, 5 active). */
	uint8 CheckForDoubleClickMove(float DeltaTime);
	/** UE2 xPawn.Dodge / PerformDodge. Dir and Cross are horizontal unit vectors; Cross keeps that velocity part. */
	bool PerformDodgeUE2(const FVector& Dir, const FVector& Cross, bool bWallDodge);
	/** UE2 xPawn.Dodge wall trace: a wall within 32 UU of the feet on the side opposite to Dir. */
	bool HasWallDodgeSupport(const FVector& Dir) const;
	/** UE2 Pawn.TakeFallingDamage with the current velocity. */
	void TakeFallingDamageUE2();
	int32 GetMaxMultiJump() const { return FMath::Max(0, JumpMaxCount - 1); }
	class UArenaCharacterMovement* GetArenaMovement() const;
	/** recent taps for combo detection */
	TArray<EArenaDir> TapHistory;
	float LastTapHistoryTime = 0.f;
	float BoosterAccum = 0.f;
	bool bLastHitHeadshot = false;
	/** Momentum (launch velocity) of the last hit: the ragdoll / gib push when it kills. */
	FVector LastHitMomentum = FVector::ZeroVector;
	/** Gib set of the player model (ArenaRagdoll::EGibKind: 0 organic, 1 green blood, 2 robot). */
	uint8 GibKindValue = 0;
	bool bFiredSinceSpawn = false;
	float LastHurtSoundTime = -100.f;
	/** Diagnostics for "fell through the floor after respawn" reports */
	FVector SpawnLocation = FVector::ZeroVector;
	bool bReportedSpawnFall = false;
	void ReportSpawnFall(const TCHAR* Reason);
};

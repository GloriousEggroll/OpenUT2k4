#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "ArenaWeapon.generated.h"

class AArenaCharacter;
class USceneComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;
class UAnimSequence;
class UMaterialInterface;
namespace ArenaArt { struct FWeaponArt; }
namespace ArenaWeaponHand { struct FViewParams; }

/**
 * Net play: the server's weapon class this weapon stands in for when it is not a stock one (a mod weapon,
 * Net/UE2NetModWeapons.h), with what of the mod's own to show instead of ours. Set before the weapon is given out.
 */
struct FArenaNetStandIn
{
	/** The server's weapon class ("ProSniperRifle_20_BETA3.ProSniperRifle"); empty: none. */
	FString UE2Class;
	/** Art (view model, fire sounds) and view placement to use instead of the class's; null: ours. */
	const ArenaArt::FWeaponArt* Art = nullptr;
	const ArenaWeaponHand::FViewParams* View = nullptr;
	/** First-person skins (null entries: the mesh's own). */
	TArray<UMaterialInterface*> Skins;
	/** Fire modes' refire times (s); < 0: ours. */
	float FireRate[2] = { -1.f, -1.f };
	/** Weapon bar icon (X1, Y1, X2, Y2) on IconTexture (an art path; empty: the stock HUD atlas). */
	bool bIcon = false;
	FVector4f IconCoords = FVector4f(0.f);
	FString IconTexture;
	/** The mod's fire modes' muzzle flash emitter classes (WeaponFire.FlashEmitterClass, "Pkg.Class"; empty: ours). */
	FString FlashEmitterClass[2];
	/** Weapon.SelectSound (a loaded sound; null: none, as ours). */
	TObjectPtr<class USoundBase> SelectSound = nullptr;
};

/**
 * Base weapon. Owned by an AArenaCharacter, attached to its first-person weapon root.
 * The view model is the imported / replacement first-person mesh (with arms) from ArenaArt when available,
 * animated with its own select / idle / fire / put-down sequences, otherwise the procedural model.
 * Subclasses (ArenaWeapons.h) configure FireModes / ammo / visuals in their constructors
 * and may override Fire behaviour.
 */
UCLASS(Abstract)
class OPENUT2K4_API AArenaWeapon : public AActor
{
	GENERATED_BODY()

public:
	AArenaWeapon();

	// ---- configuration (set in subclass constructors) ----
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") FString WeaponName = TEXT("Weapon");
	/** 1..9 number key slot; one weapon per slot */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 Slot = 1;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 MaxAmmo = 100;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 InitialAmmo = 50;
	/** Ammo given by one ammo pickup for this weapon */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 AmmoPickupAmount = 20;
	/** Weapon never runs out (melee) */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") bool bInfiniteAmmo = false;
	/** Net play: our instant hit shots draw their own tracer (UE2 bullet attachments trace the owner's tracer along its
	 *  own view; the hit effects come from the server's mHitLocation, beam weapons replicate a beam actor instead) */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") bool bNetOwnerTracers = false;
	/** Both modes are one UE2 fire mode object (Weapon.InitWeaponFires: FireModeClass[0] == FireModeClass[1]) with one
	 *  refire time; net play times our shot effects by it */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") bool bSharedFireMode = false;
	/** Base desirability for auto-switch and bots, 0..1 */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") float Priority = 0.5f;
	/** Primary colour used for the view model and HUD icon */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") FLinearColor WeaponColor = FLinearColor(0.4f, 0.4f, 0.45f);
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") FArenaFireMode FireModes[2];
	/** UE2 Weapon.BringUpTime / PutDownTime: a new weapon fires after the old one's put-down plus its bring-up. */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") float BringUpTime = 0.33f;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") float PutDownTime = 0.33f;
	/** Second ammo type for modes with bUseAltAmmo (assault rifle grenades); 0 = none. */
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 AltMaxAmmo = 0;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 AltInitialAmmo = 0;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon") int32 AltAmmoPickupAmount = 0;

	// ---- state ----
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon") int32 Ammo = 0;
	/** Net play: what this weapon stands in for (FArenaNetStandIn); call before GiveTo. */
	void SetNetStandIn(const FArenaNetStandIn& In);
	/** The mod fire mode's FireRate as its client code has it now (a spin-up, a rate set by ModeDoFire): Net/UE2NetScriptHUD. */
	void SetNetFireRate(uint8 Mode, float Rate);
	/** The first-person view model (null: the procedural one). */
	USkeletalMeshComponent* GetViewMesh() const { return ViewMesh; }
	const FArenaNetStandIn& GetNetStandIn() const { return NetStandIn; }
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon") int32 AltAmmo = 0;
	/** Set by the owner right before Equip: the previous weapon's put-down time. */
	float EquipDelay = 0.f;
	/** Shots this weapon reflected (Shield Gun) or that were reflected by a Shield Gun (beams); for tests. */
	int32 ReflectCount = 0;
	/** Shots fired and the mode of the last one (hosting for UT2004 clients: WeaponAttachment.FlashCount / FiringMode). */
	int32 NetShotCount = 0;
	uint8 NetShotMode = 0;
	/** The last instant hit shot: where it started and hit (first segment), the hit normal (zero: no hit), and a count. */
	FVector NetShotStart = FVector::ZeroVector;
	FVector NetShotEnd = FVector::ZeroVector;
	FVector NetShotNormal = FVector::ZeroVector;
	int32 NetHitscanCount = 0;
	/** World time of the last beam tick (link gun alt fire): NetShotStart / NetShotEnd hold its line. */
	double NetBeamTime = -100.0;

	// ---- API used by AArenaCharacter / HUD / bots ----

	/** Called once after spawn when given to a character. */
	virtual void GiveTo(AArenaCharacter* NewOwner);
	/** Make this the active weapon (shows view model). */
	virtual void Equip();
	/** Hide and stop firing. */
	virtual void Unequip();
	/** Fire button pressed / released. Mode 0 = primary, 1 = alt. */
	virtual void StartFire(uint8 Mode);
	virtual void StopFire(uint8 Mode);
	virtual void StopAllFire();

	bool IsFiring(uint8 Mode) const { return Mode < 2 && bFireHeld[Mode]; }
	bool IsEquipped() const { return bEquipped; }
	virtual bool HasAmmo() const;
	virtual bool HasAmmoFor(uint8 Mode) const;
	void AddAmmo(int32 Amount);
	void AddAltAmmo(int32 Amount);
	/** Ammo of the type a mode uses. */
	int32 GetAmmoFor(uint8 Mode) const { return Mode < 2 && FireModes[Mode].bUseAltAmmo ? AltAmmo : Ammo; }
	/** Take Amount of the mode's ammo (shock combo cost); false when there wasn't enough. */
	bool TakeAmmo(uint8 Mode, int32 Amount);
	/** Ammo pickup for this weapon (UE2 HandlePickupQuery): false when nothing could be added. */
	virtual bool GiveAmmoPickup();
	/** Picked up again while owned (UE2: weapon stay forbids it; the assault rifle becomes dual). True if taken. */
	virtual bool OnRepeatPickup(bool bWeaponStay);
	/** UE2 Weapon.AdjustPlayerDamage (shield gun alt): returns the damage left, may scale Momentum. */
	virtual float AdjustPlayerDamage(float Damage, AController* InstigatedBy, const FVector& HitLocation, FVector& Momentum, bool bFell) { return Damage; }
	float GetAmmoFraction() const { return MaxAmmo > 0 ? float(Ammo) / float(MaxAmmo) : 1.f; }

	/** Current zoom FOV override (0 = none). Read by the player camera. */
	virtual float GetZoomFOV() const { return 0.f; }
	/** Damage multiplier for shield-type alt fire, 1 = no protection. Called by AArenaCharacter::TakeDamage. */
	virtual float GetShieldDamageScale(const FVector& DamageDirection) const { return 1.f; }

	/** Bot helpers */
	virtual float GetBotRating(float DistanceToEnemy) const;
	/** Which mode a bot should use at this range. */
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const;
	/** Projectile speed for aim leading (0 = instant hit). */
	float GetProjectileSpeed(uint8 Mode) const;
	/** Net play (Net/UE2NetPlay.h): a shot's sound, muzzle flash, view animation and bullet tracer; the UT2004 server
	 *  fires it. */
	void PlayNetFireEffects(uint8 Mode);
	/** Net play: the fire button of a mode went down / up (UE2 WeaponFire.ModeHoldFire / bFireOnRelease): the hold
	 *  animation, the shield, the link beam and laser sights while held, a charged shot's effects on release when bReady.
	 *  True when a released shot played its effects. */
	bool SetNetFireHeld(uint8 Mode, bool bHeld, bool bReady);
	/** Net play: the mode plays a shot at its fire rate while held (instant hit and projectile modes that don't charge). */
	virtual bool IsNetAutoFire(uint8 Mode) const;
	bool IsNetFireHeld(uint8 Mode) const { return Mode < 2 && bNetFireHeld[Mode]; }
	/** Net play: the UT2004 server's state of this weapon (its weapon actor, replicated to us as its owner), set by
	 *  Net/UE2NetPlay; bValid only in net play. */
	struct FNetWeaponState
	{
		bool bValid = false;
		/** ONSAVRiL.bLockedOn / HomingTarget (the target as we show it) */
		bool bLockedOn = false;
		TWeakObjectPtr<AActor> HomingTarget;
		/** TransLauncher.TransBeacon is out */
		bool bBeaconOut = false;
		/** Our PainterBeamEffect's TargetState (0 aiming, 1 marked, 2 acquired, 3 cancelled), and that the server still
		 *  has it (it fades for 0.4 s after the button comes up) */
		uint8 PainterTargetState = 0;
		bool bPainterBeamOpen = false;
		/** It went while the button was still held (after a strike the server paints no more until the next press) */
		bool bPainterBeamGone = false;
	};
	FNetWeaponState NetState;
	/** UE2 Weapon.GetEffectStart: where our shot effects leave the weapon (the first-person muzzle). */
	FVector GetEffectStart() const { return GetMuzzleLocation(); }
	/** Fire on release for charge modes. */
	bool IsChargeMode(uint8 Mode) const;

	AArenaCharacter* GetArenaOwner() const { return ArenaOwner; }

	/** Root for the first-person model (attached to the owner's camera). */
	UPROPERTY(VisibleAnywhere, Category = "Weapon") TObjectPtr<USceneComponent> ViewRoot;

	/** Imported / replacement art mapping for this weapon (nullptr if none). */
	const ArenaArt::FWeaponArt* GetArt() const { return Art; }
	/** True when the imported first-person mesh replaces the procedural view model. */
	bool HasImportedViewModel() const { return ViewMesh != nullptr; }
	/** Time until which the put-down animation of this weapon is still on screen (< now when not putting down). */
	float GetPutDownEndTime() const { return PutDownEndTime; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Build the procedural first-person model under ViewRoot (called from GiveTo). */
	virtual void BuildViewModel();
	/** Fire one shot of the given mode. ChargeAlpha 0..1 for charged modes. */
	virtual void FireShot(uint8 Mode, float ChargeAlpha);
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha);
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha);
	virtual void FireBeamTick(uint8 Mode, float DeltaSeconds);
	virtual void FireMelee(uint8 Mode, float ChargeAlpha);

	/** Eye location + aim direction of the owner (uses controller rotation). */
	void GetAim(FVector& OutStart, FVector& OutDir) const;
	/** Owner's damage multiplier (double damage / berserk). */
	float GetDamageScale() const;
	/** Owner's refire time multiplier (berserk). */
	float GetRefireScale() const;
	void ConsumeAmmo(uint8 Mode);
	/** UE2 fire timing after a shot of Mode at Now. */
	void AdvanceFireTime(uint8 Mode, float Now);
	/** Kick the view model back a little. */
	void AddRecoil(float Amount);

	/** Imported first-person mesh: creation and animation. */
	void SetupImportedViewModel();
	UAnimSequence* FindViewAnim(const TCHAR* Name);
	/** Plays a view model sequence. Duration > 0 fits the sequence into that time, MinRateTime fits it only if longer. */
	bool PlayViewAnim(const TCHAR* Name, bool bLoop, float Duration = 0.f, float MaxTime = 0.f);
	void TickViewModel(float DeltaSeconds);
	/** UT2004 weapon hand / small weapons on the view model (ArenaWeaponHand.h): offset, pivot, mirroring, hidden. */
	void ApplyHandToViewModel();
	float AppliedHand = 99.f;
	int8 AppliedSmall = -1;
	/** Imported fire sound for the mode if present, else our synthesized one. */
	void PlayFireSound(uint8 Mode, const FVector& Location, float Volume, float Pitch);
	/** Muzzle flash at the view model's muzzle (owner only), plus the imported-art flash on the third-person weapon. */
	void SpawnViewMuzzleFlash(const FLinearColor& Color, float Scale, uint8 Mode = 0);
	/** Imported-art effect style names for a fire mode (NAME_None when there is no art). */
	FName GetArtImpactStyle(uint8 Mode) const;
	FName GetArtBeamStyle(uint8 Mode) const;
	/** Camera first-person FOV for the imported view model. */
	void ApplyViewFOV();

	/** Helper to add a shape mesh to the view model */
	UStaticMeshComponent* AddViewPart(class UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& Scale, const FLinearColor& Color, bool bGlow = false);

	UPROPERTY() TObjectPtr<AArenaCharacter> ArenaOwner;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> ViewParts;
	/** AddViewPart mirrors parts to the left side (second gun of dual assault rifles). */
	bool bMirrorViewParts = false;

	bool bEquipped = false;
	bool bFireHeld[2] = { false, false };
	/** Net play: the fire button is held while the server fires (SetNetFireHeld); effects only, never a local shot. */
	bool bNetFireHeld[2] = { false, false };
	/** Net play: a mode's button went down / up (SetNetFireHeld), for weapon specific hold effects. */
	virtual void NetFireHeldChanged(uint8 Mode, bool bHeld) {}
	/** The fire mode being fired (FireShot); SpawnProjectiles uses its ProjSpawnOffset. */
	uint8 SpawnMode = 0;
	float ChargeStart[2] = { 0.f, 0.f };
	/** Time when the next shot may be fired (shared by both modes, like UT) */
	float NextFireTime = 0.f;
	float RecoilOffset = 0.f;
	/** Muzzle offset in view-model space */
	FVector MuzzleOffset = FVector(60.f, 14.f, -12.f);

	// ---- internals (ArenaWeapon.cpp) ----
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** World-space muzzle location of the view model. */
	FVector GetMuzzleLocation() const;
	/** UE2 Weapon.Hand of the owner: 1 right, -1 left, 0 center, 2 hidden (ArenaWeaponHand::GetHandedness). */
	float GetHand() const;
	/** ECC_Weapon line trace ignoring the owner, triggers and non-solid actors. */
	bool WeaponTrace(const FVector& Start, const FVector& End, FHitResult& OutHit, const AActor* ExtraIgnore = nullptr, bool bIgnoreOwner = true) const;
	/** UE2 Controller.AdjustAim side effects: a bot owner aims the shot (Bot.AdjustAim, returns the fire direction);
	 * the shot target (bot: its enemy; player: PickTarget along the view) is remembered for the warnings below. */
	FVector AdjustAimForShot(uint8 Mode, const FVector& Start, const FVector& Dir);
	/** UE2 InstantWarnTarget: an instant-hit shot warns a bot target (WarnTargetPct). */
	void WarnInstantTarget(uint8 Mode, const FVector& Dir);
	/** UE2 Projectile.PostBeginPlay: ShotTarget's bot gets ReceiveProjectileWarning. */
	void WarnProjectileTarget(class AArenaProjectile* Proj);
	TWeakObjectPtr<class AArenaCharacter> ShotTarget;
	/** Damage + FX for one hitscan hit. */
	void ProcessHitscanHit(uint8 Mode, const FHitResult& Hit, const FVector& ShotDir, float BaseDamage);
	/** UE2 ProjectileFire.DoFireEffect: where a projectile of Mode starts (ProjSpawnOffset, pulled back from walls) and
	 * the direction it leaves in (the view for players, Bot.AdjustAim for bots; also sets ShotTarget). */
	FVector GetProjectileStart(uint8 Mode, FVector& OutDir);
	/** Spawn Count projectiles with the given params (WeaponName is filled in) in a cone of SpreadDeg around the aim. */
	TArray<class AArenaProjectile*> SpawnProjectiles(const FArenaProjectileParams& InParams, int32 Count, float SpreadDeg, uint8 SpreadStyle = 0,
		float UE2Spread = 0.f, const FArenaProjectileParams* ChunkParams = nullptr);
	/** Instant hit spread (UE2 vector spread) and damage scale of a mode; weapons with growing spread / berserk override. */
	virtual float GetHitscanSpread(uint8 Mode) const { return Mode < 2 ? FireModes[Mode].UE2Spread : 0.f; }
	virtual float GetHitscanDamageScale(uint8 Mode) const { return 1.f; }
	/** Fire rate multiplier of a mode (dual assault rifles). */
	virtual float GetModeRefireScale(uint8 Mode) const { return 1.f; }
	/** Continuous beam visual (Beam fire type), visible to everyone. */
	void UpdateLiveBeam(uint8 Mode);
	void HideLiveBeam();
	/** Sound, muzzle flash and recoil for a shot. */
	void PlayFireEffects(uint8 Mode, float ChargeAlpha);
	/** If completely out of ammo, ask the owner to switch. Returns true if a switch was requested. */
	bool CheckOutOfAmmo();
	AController* GetOwnerController() const;
	float GetWorldTime() const;

	UPROPERTY() TObjectPtr<USkeletalMeshComponent> ViewMesh;
	UPROPERTY() TMap<FName, TObjectPtr<UAnimSequence>> ViewAnims;
	const ArenaArt::FWeaponArt* Art = nullptr;
	FArenaNetStandIn NetStandIn;
	/** Keeps NetStandIn.Skins loaded. */
	UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> NetStandInSkins;
	float PutDownEndTime = -1.f;
	float ShowViewTime = 0.f;
	bool bViewShown = false;
	/** End of the current one-shot view animation (then back to idle); large while looping / holding. */
	float ViewAnimEndTime = 0.f;
	/** Fire mode whose hold animation is playing (255 = none). */
	uint8 HoldAnimMode = 255;
	float LastImportedSoundTime[2] = { -100.f, -100.f };

	UPROPERTY() TObjectPtr<UStaticMeshComponent> LiveBeam;
	/** Imported-art version of the continuous beam (textured quads), used instead of LiveBeam when available. */
	UPROPERTY() TArray<TObjectPtr<class UArenaSpriteComponent>> LiveBeamSprites;
	bool bLiveBeamArtTried = false;
	float NextLiveBeamFlash = 0.f;
	UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> LiveBeamMaterial;
	float ShieldAccum = 0.f;
	float LastBeamSoundTime = -100.f;
	float EquipTime = -100.f;
	bool bBeamActive = false;
};

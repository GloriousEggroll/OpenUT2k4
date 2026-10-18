#pragma once

#include "CoreMinimal.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.generated.h"

/*
 * Concrete weapons. Slots follow number keys.
 *  1 Gauntlet        - charged melee blast (self-boost when aimed at floor) / alt: energy shield
 *  2 Carbine         - fast hitscan / alt: lobbed grenade (bounces, explodes)
 *  3 Goo Launcher    - sticky goo globs / alt: charged big glob
 *  4 Pulse Rifle     - hitscan beam / alt: slow energy core; beam hitting core = combo blast
 *  5 Arc Linker      - fast plasma bolts / alt: short continuous beam
 *  6 Rotary Gun      - very fast inaccurate hitscan / alt: slower accurate hitscan
 *  7 Shard Cannon    - spread of bouncing shards / alt: lobbed explosive shell
 *  8 Rocket Launcher - rocket / alt: bouncing grenade
 *  9 Rail Rifle      - long range hitscan with headshots / alt: zoom
 *  0 Insta Rifle     - instagib mode only: one-shot-kill beam, infinite ammo
 */

UCLASS() class OPENUT2K4_API AWeap_Gauntlet : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_Gauntlet();
	virtual float AdjustPlayerDamage(float Damage, AController* InstigatedBy, const FVector& HitLocation, FVector& Momentum, bool bFell) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void StartFire(uint8 Mode) override;
	/** UE2 ShieldGun.CheckReflect: the shield is up and HitLocation is in front of the view; OutRefNormal is the
	 * view direction and AmmoDrain energy is used up on success. */
	bool CheckReflect(const FVector& HitLocation, FVector& OutRefNormal, int32 AmmoDrain);
protected:
	virtual void BuildViewModel() override;
	virtual void FireMelee(uint8 Mode, float ChargeAlpha) override;
	float ShieldDrainAccum = 0.f;
	float NextAutoFireTest = 0.f;
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> ShieldVisual;
	/** Owner-only shield plate in the view model */
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> ShieldPlate;
	/** Emitter glow that swells while charging */
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> ChargeGlow;
	/** Imported shield mesh in front of the imported first-person view model (owner only). */
	UPROPERTY() TObjectPtr<class UStaticMeshComponent> ShieldArtView;
};

UCLASS() class OPENUT2K4_API AWeap_Carbine : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_Carbine();
	virtual bool GiveAmmoPickup() override;
	virtual bool OnRepeatPickup(bool bWeaponStay) override;
	bool IsDualMode() const { return bDualMode; }
	virtual void Tick(float DeltaSeconds) override;
	/** First-person second rifle (dual mode), nullptr until shown. */
	USkeletalMeshComponent* GetOffhandViewMesh() const { return OffhandViewMesh; }
	bool HasMirroredViewParts() const { return bMirroredParts; }
protected:
	virtual void BuildViewModel() override;
	/** AssaultRifle.RenderDualOverlay: the same view model drawn mirrored on the left (flips in over 0.5 s). */
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> OffhandViewMesh;
	float DualPickupTime = -100.f;
	bool bMirroredParts = false;
	virtual float GetHitscanSpread(uint8 Mode) const override;
	virtual float GetHitscanDamageScale(uint8 Mode) const override;
	virtual float GetModeRefireScale(uint8 Mode) const override;
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha) override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	/** AssaultFire: spread grows 0.02 per shot up to 0.12 and resets after 0.5 s without firing. */
	float CurrentSpread = 0.02f;
	float LastPrimaryFireTime = -100.f;
	/** Second assault rifle (AssaultRifle.HandlePickupQuery): fire rate x0.55, spread x1.5, max ammo x2. */
	bool bDualMode = false;
};

UCLASS() class OPENUT2K4_API AWeap_GooLauncher : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_GooLauncher();
	virtual void Tick(float DeltaSeconds) override;
	virtual void StartFire(uint8 Mode) override;
	/** BioChargedFire goop load (1 per GoopUpRate 0.25 s while held, ammo taken as it loads). */
	int32 GetGoopLoad() const { return GoopLoad; }
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	int32 GoopLoad = 0;
	float NextGoopTime = 0.f;
};

UCLASS() class OPENUT2K4_API AWeap_PulseRifle : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_PulseRifle();
protected:
	virtual void BuildViewModel() override;
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha) override;
};

UCLASS() class OPENUT2K4_API AWeap_ArcLinker : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_ArcLinker();
	/** LinkFire: the team mate this beam is linked to (powering their link gun). */
	class AArenaCharacter* GetLinkedPawn() const { return LinkedPawn.Get(); }
	/** Links powering this gun (team mates' beams on us, plus theirs). */
	int32 GetLinks() const;
	bool IsBeamFiring() const { return bEquipped && bFireHeld[1]; }
protected:
	virtual void BuildViewModel() override;
	virtual void FireBeamTick(uint8 Mode, float DeltaSeconds) override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	TWeakObjectPtr<class AArenaCharacter> LinkedPawn;
	float LinkBreakTime = 0.f;
};

UCLASS() class OPENUT2K4_API AWeap_RotaryGun : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_RotaryGun();
	virtual void Tick(float DeltaSeconds) override;
	virtual void StartFire(uint8 Mode) override;
protected:
	virtual void BuildViewModel() override;
	UPROPERTY() TObjectPtr<USceneComponent> Barrels;
	float SpinSpeed = 0.f;
};

UCLASS() class OPENUT2K4_API AWeap_ShardCannon : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_ShardCannon();
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
};

UCLASS() class OPENUT2K4_API AWeap_RocketLauncher : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_RocketLauncher();
	virtual void Tick(float DeltaSeconds) override;
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	bool IsLockedOn() const { return bLockedOn && SeekTarget.IsValid(); }
	int32 GetLoad() const { return Load; }
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	void FireLoad();
	/** RocketMultiFire: rockets loaded while alt is held (1, 2 at 0.95 s, 3 at 1.9 s; fires itself at 2.3 s). */
	int32 Load = 0;
	float LoadStart = 0.f;
	bool bTightSpread = false;
	/** RocketLauncher lock-on (SeekRange 8000 UU, LockAim 0.996, LockRequiredTime 1.25 s, UnLockRequiredTime 0.5 s). */
	TWeakObjectPtr<AActor> SeekTarget;
	bool bLockedOn = false;
	float LockTime = 0.f;
	float UnLockTime = 0.f;
	float NextSeekCheck = 0.f;
};

UCLASS() class OPENUT2K4_API AWeap_RailRifle : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_RailRifle();
	virtual float GetZoomFOV() const override;
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	virtual void Unequip() override;
	virtual void Tick(float DeltaSeconds) override;
protected:
	virtual void BuildViewModel() override;
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha) override;
	float ZoomLevel = 0.f;
	bool bZoomingIn = false;
};

UCLASS() class OPENUT2K4_API AWeap_InstaRifle : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_InstaRifle();
protected:
	virtual void BuildViewModel() override;
};

namespace ArenaWeaponList
{
	/** All normal (non-instagib) weapon classes, slot order. */
	OPENUT2K4_API TArray<TSubclassOf<AArenaWeapon>> AllWeapons();
}

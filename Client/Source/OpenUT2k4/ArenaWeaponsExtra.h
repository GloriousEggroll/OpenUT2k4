#pragma once

#include "CoreMinimal.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.generated.h"

class AArenaStickyGrenade;
class AArenaCrawlerMine;
class UStaticMeshComponent;

/** Beams of held fire modes (painter, mine layer laser), also the other players' beams in net play (Net/UE2NetActors). */
namespace ArenaWeaponBeam
{
	/** A beam on Holder placed every tick in world space: the stock effect class's texture and width as a camera-facing
	 *  quad, else a glowing cylinder. Starts hidden. */
	OPENUT2K4_API UStaticMeshComponent* Make(AActor* Holder, const FLinearColor& Color, const TCHAR* UE2EffectClass = nullptr);
	/** Shows a beam made by Make from Start to End (MeshWidth: the cylinder's width). */
	OPENUT2K4_API void Place(UStaticMeshComponent* Beam, const FVector& Start, const FVector& End, float MeshWidth);
	/** Hides a beam made by Make (a sprite beam without a lifetime would show itself again on its next tick). */
	OPENUT2K4_API void Hide(UStaticMeshComponent* Beam);
	/** PainterBeamEffect's look on a client: Brightness (mColorRange) and width (mSizeRange[0], UU). */
	struct FPainterLook
	{
		float Brightness = 40.f;
		float Width = -1.f;       // < 0: the effect's default, on the first tick
		float AquiredTime = 0.f;
	};
	/** PainterBeamEffect.Tick / Aquired / Cancelled after DeltaTime in the replicated TargetState (0 aiming, 1 marked,
	 *  2 acquired, 3 cancelled), applied to a beam made by Make: 40 aiming, up to 250 once marked; acquired is 250 and
	 *  widens to 16 UU until its 0.4 s timer goes to Cancelled, which fades to 40 and narrows to 1 UU (TargetState stays
	 *  acquired on the wire). */
	OPENUT2K4_API void TickPainter(UStaticMeshComponent* Beam, FPainterLook& Look, uint8 TargetState, float DeltaTime);
}

/**
 * The UT2004 super weapons and the Onslaught / classic weapons, under their own names, with UT2004's numbers and
 * behaviour (XWeapons Redeemer / Painter, UTClassic ClassicSniperRifle, Onslaught ONSAVRiL / ONSGrenadeLauncher /
 * ONSMineLayer). Their projectiles are in ArenaWeaponActors.h.
 */

/** Redeemer: primary launches the warhead, alt flies it yourself (guided, own camera and scope HUD). */
UCLASS() class OPENUT2K4_API AWeap_WarheadLauncher : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_WarheadLauncher();
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override { return 0; }
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
};

/** Ion Painter: hold primary on a spot under open sky for 1.2 s and the ion cannon strikes it; alt zooms. */
UCLASS() class OPENUT2K4_API AWeap_OrbitalDesignator : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_OrbitalDesignator();
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	virtual void Unequip() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual float GetZoomFOV() const override;
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override { return 0; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** Net play: painting is a hold, not shots at the fire rate. */
	virtual bool IsNetAutoFire(uint8 Mode) const override { return Mode != 0 && AArenaWeapon::IsNetAutoFire(Mode); }
	bool IsPainting() const { return bPainting; }
	bool HasValidMark() const { return bValidMark; }
	FVector GetMarkLocation() const { return MarkLocation; }
protected:
	virtual void BuildViewModel() override;
	/** PainterFire.Paint.ModeTick */
	void PaintTick(float DeltaSeconds);
	virtual void NetFireHeldChanged(uint8 Mode, bool bHeld) override;
	void StopPainting();
	UPROPERTY() TObjectPtr<UStaticMeshComponent> PaintBeam;
	/** Net play: PainterBeamEffect's brightness and width of our beam. */
	ArenaWeaponBeam::FPainterLook NetPaintLook;
	bool bPainting = false;
	bool bValidMark = false;
	bool bMarkStarted = false;
	bool bDoHit = false;
	float NextHitTime = 0.f;
	float MarkTime = 0.f;
	FVector MarkLocation = FVector::ZeroVector;
	FVector EndEffect = FVector::ZeroVector;
	float ZoomLevel = 0.f;
	bool bZoomingIn = false;
};

/** Classic Sniper Rifle (UTClassic): 60 damage, headshots x2, 1.33 s, zoom; replaces our Rail Rifle where maps ask for it. */
UCLASS() class OPENUT2K4_API AWeap_MarksmanRifle : public AWeap_RailRifle
{
	GENERATED_BODY()
public:
	AWeap_MarksmanRifle();
protected:
	virtual void BuildViewModel() override;
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha) override;
};

/** AVRiL: anti-vehicle rocket that homes on the vehicle you keep locked; alt zooms while locked. */
UCLASS() class OPENUT2K4_API AWeap_SeekerLauncher : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_SeekerLauncher();
	virtual void Tick(float DeltaSeconds) override;
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	virtual void Unequip() override;
	virtual float GetZoomFOV() const override;
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override { return 0; }
	bool IsLockedOn() const { return bLockedOn && HomingTarget.IsValid(); }
	AActor* GetHomingTarget() const { return HomingTarget.Get(); }
	/** ONSAVRiL.CanLockOnTo: an enemy (or, outside team games, any) vehicle. */
	bool CanLockOnTo(const AActor* Other) const;
	/** Bot locks taken (the bot's enemy's vehicle, ONSAVRiL.WeaponTick AIFocus) and seekers fired by bots (tests / logs). */
	int32 NumBotLocks = 0;
	int32 NumBotShots = 0;
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	/** ONSAVRiL.WeaponTick (every LockCheckFreq 0.2 s). */
	void UpdateLock();
	TWeakObjectPtr<AActor> HomingTarget;
	bool bLockedOn = false;
	float LockCheckTime = 0.f;
	float ZoomLevel = 0.f;
	bool bLosingLock = false;
	float LostLockTime = 0.f;
};

/** ONS Grenade Launcher: up to 8 sticky grenades out, alt detonates them all. */
UCLASS() class OPENUT2K4_API AWeap_StickyLauncher : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_StickyLauncher();
	virtual bool HasAmmo() const override;
	virtual bool HasAmmoFor(uint8 Mode) const override;
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void NotifyGrenadeGone(AArenaStickyGrenade* G);
	int32 GetNumGrenades() const;
	static constexpr int32 MaxGrenades = 8;
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	/** ONSGrenadeAltFire: detonate every grenade (mode 1, fired on press like an instant-hit mode). */
	virtual void FireHitscan(uint8 Mode, float ChargeAlpha) override;
	void DetonateAll();
	TArray<TWeakObjectPtr<AArenaStickyGrenade>> Grenades;
};

/** ONS Mine Layer: throws spider mines (8 at most), alt's laser sends them walking to where it points. */
UCLASS() class OPENUT2K4_API AWeap_CrawlerLayer : public AArenaWeapon
{
	GENERATED_BODY()
public:
	AWeap_CrawlerLayer();
	virtual void StartFire(uint8 Mode) override;
	virtual void StopFire(uint8 Mode) override;
	virtual void Unequip() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual float GetBotRating(float DistanceToEnemy) const override;
	virtual uint8 GetBotFireMode(float DistanceToEnemy) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void NotifyMineGone(AArenaCrawlerMine* M);
	int32 GetNumMines() const;
	static constexpr int32 MaxMines = 8;
protected:
	virtual void BuildViewModel() override;
	virtual void FireProjectile(uint8 Mode, float ChargeAlpha) override;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> TargetBeam;
	TArray<TWeakObjectPtr<AArenaCrawlerMine>> Mines;
	bool bTargeting = false;
	float NextTargetHit = 0.f;
};

namespace ArenaWeaponList
{
	/** The six weapons above (map pickups, mutators); AllWeapons() stays the standard set. */
	OPENUT2K4_API TArray<TSubclassOf<AArenaWeapon>> ExtraWeapons();
}

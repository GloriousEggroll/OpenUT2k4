#pragma once

#include "CoreMinimal.h"
#include "Camera/PlayerCameraManager.h"
#include "ArenaTypes.h"
#include "ArenaViewShake.generated.h"

class AController;
class UWorld;

/**
 * UE2 view shaking (Engine.PlayerController: ShakeView, ViewShake, CheckShake, UpdateShakeRotComponent,
 * ClientDamageShake, WeaponShakeView). The shake state lives in the player's camera manager, which runs ViewShake
 * every unpaused frame and adds ShakeRot to the view rotation and ShakeOffset (along the view axes) to the view
 * location while the player looks through its own pawn or vehicle (PlayerController.CalcFirstPersonView,
 * Vehicle.SpecialCalcFirstPersonView / SpecialCalcBehindView). Bots and spectators of other players get none.
 *
 * Callers (all UT2004's): every weapon shot (WeaponFire.ShakeView / ONSWeapon.ShakeView through WeaponShakeView, gated
 * by the "Weapons Shake View" setting and scaled by 1 - ZoomLevel), damage taken (UnrealPlayer.NotifyTakeHit ->
 * DamageShake), hard landings (Pawn.Landed -> DamageShake), the Redeemer and ion cannon blasts and vehicle explosions
 * (ShakeView of every player whose view target is near, scaled by distance).
 */
namespace ArenaViewShake
{
	/** UE2 PlayerController.ShakeView on Controller (no-op for bots and non-local players). */
	OPENUT2K4_API void ShakeView(AController* Controller, const FArenaViewShake& Shake);
	/** PlayerController.WeaponShakeView: only with the "Weapons Shake View" setting, magnitudes x (1 - ZoomLevel). */
	OPENUT2K4_API void WeaponShakeView(AController* Controller, const FArenaViewShake& Shake);
	/** PlayerController.ClientDamageShake(Damage). */
	OPENUT2K4_API void DamageShake(AController* Controller, int32 Damage);
	/**
	 * RedeemerProjectile / IonCannon / ONSVehicle ShakeView: every player whose view target is within Radius x Falloff
	 * (cm) of Location shakes fully within Radius and linearly less beyond ((Radius x Falloff - Dist) / Radius).
	 * bRequireBase: only view targets standing on something (IonCannon: PC.ViewTarget.Base != None).
	 * Returns the number of players shaken.
	 */
	OPENUT2K4_API int32 ShakeViewersNear(UWorld* World, const FVector& Location, float Radius, float Falloff, const FArenaViewShake& Shake,
		bool bRequireBase = false);

	/** XWeapons.RedeemerProjectile / RedeemerWarhead / IonCannon and ONSVehicle (explosion) defaults. */
	OPENUT2K4_API FArenaViewShake Blast();
	/** UT2004 fire mode defaults (XWeapons / Onslaught / UTClassic WeaponFire subclasses). */
	OPENUT2K4_API FArenaViewShake KickBack();        // ProjectileFire kick: Shield, AssaultGrenade, Rocket, Flak, Redeemer, AVRiL
	OPENUT2K4_API FArenaViewShake AssaultFire();     // AssaultFire, MinigunFire (and MinigunAltFire)
	OPENUT2K4_API FArenaViewShake BioFire();         // BioFire (and ONSGrenadeFire / ONSMineThrowFire)
	OPENUT2K4_API FArenaViewShake BioChargedFire();
	OPENUT2K4_API FArenaViewShake ShockBeamFire();   // ShockBeamFire, SuperShockBeamFire
	OPENUT2K4_API FArenaViewShake ShockProjFire();
	OPENUT2K4_API FArenaViewShake LinkAltFire();     // the link gun's plasma (primary)
	OPENUT2K4_API FArenaViewShake LinkFire();        // the link beam (alt)
	OPENUT2K4_API FArenaViewShake SniperFire();      // SniperFire, ClassicSniperFire

	/**
	 * ONSVehicle proximity view shake (bEnableProximityViewShake, ViewShakeRadius / ViewShakeOffsetMag /
	 * ViewShakeOffsetFreq): every local player whose view target is within Radius (cm) of Location, other than the
	 * vehicle itself, gets this frame an offset of OffsetMag (UU) x sin(time x Freq x 2 pi) x (1 - dist / Radius), like
	 * PlayerController's ambient shake. Approximation: UE2 does this in native code.
	 */
	OPENUT2K4_API void ProximityShake(UWorld* World, const AActor* Source, const FVector& Location, float Radius, const FVector& OffsetMag, float Freq);
	/** Current shake of a player (tests): rotation in degrees (pitch, yaw, roll) and offset in cm. */
	OPENUT2K4_API bool GetCurrentShake(const AController* Controller, FRotator& OutRot, FVector& OutOffset);
}

/** The player's camera manager: runs and applies the UE2 view shake. */
UCLASS()
class OPENUT2K4_API AArenaPlayerCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

public:
	/** UE2 PlayerController.ShakeView: a stronger shake (by magnitude) replaces the current one per part. */
	void ShakeView(const FArenaViewShake& Shake);
	void StopViewShaking();
	/** UE2 PlayerController.ViewShake(DeltaTime). */
	void ViewShake(float DeltaTime);
	/** An ambient offset (UU, view axes) for this frame only; the strongest one of a frame is used. */
	void AddAmbientOffset(const FVector& OffsetUU);

	/** UE2 ShakeRot in rotator units (pitch, yaw, roll) and ShakeOffset in UU. */
	FVector ShakeRotUU = FVector::ZeroVector;
	FVector ShakeOffsetUU = FVector::ZeroVector;

protected:
	virtual void UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime) override;

	FVector ShakeRotMax = FVector::ZeroVector;
	FVector ShakeRotRate = FVector::ZeroVector;
	FVector ShakeRotTime = FVector::ZeroVector;
	FVector ShakeOffsetMax = FVector::ZeroVector;
	FVector ShakeOffsetRate = FVector::ZeroVector;
	FVector ShakeOffsetTime = FVector::ZeroVector;
	double LastShakeWorldTime = -1.0;
	FVector AmbientOffsetUU = FVector::ZeroVector;
};

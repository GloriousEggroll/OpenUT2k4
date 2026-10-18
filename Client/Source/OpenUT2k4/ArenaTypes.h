#pragma once

#include "CoreMinimal.h"
#include "ArenaTypes.generated.h"

class AArenaWeapon;
class AActor;

/**
 * Legacy game type enum, kept for compatibility. The real selector is the string mode id (?Mode=<ModeId>)
 * resolved through ArenaGameRulesRegistry; UArenaGameRules::GetLegacyGameType maps a mode to the closest value.
 */
UENUM(BlueprintType)
enum class EArenaGameType : uint8
{
	Deathmatch,
	TeamDeathmatch,
	CaptureTheFlag
};

/** Match flow tracked by AArenaGameState. */
UENUM(BlueprintType)
enum class EArenaMatchPhase : uint8
{
	Countdown,
	Playing,
	Ended,
	/** Between rounds (AArenaGameMode::EndRound): nobody can be damaged, then Countdown for the next round. */
	RoundOver
};

/** Direction taps used for dodging (double tap) and adrenaline combos. */
UENUM(BlueprintType)
enum class EArenaDir : uint8
{
	Forward,
	Back,
	Left,
	Right
};

/** Active adrenaline combo. */
UENUM(BlueprintType)
enum class EArenaCombo : uint8
{
	None,
	Speed,      // Fwd Fwd Fwd Fwd
	Booster,    // Back Back Back Back
	Invisible,  // Right Right Left Left
	Berserk     // Fwd Fwd Back Back
};

/** How a weapon fire mode delivers damage. */
UENUM(BlueprintType)
enum class EArenaFireType : uint8
{
	Hitscan,     // instant trace, NumShots traces with Spread
	Projectile,  // spawns NumShots AArenaProjectile with Spread
	Beam,        // continuous short-range beam while held (damage per RefireTime tick)
	Melee,       // charge while held, release to hit a short range target / self-boost
	Shield,      // held: frontal damage reduction, consumes ammo
	Zoom         // held: zoom the view (no damage)
};

/** UT2004 projectile classes with their own behaviour (AArenaProjectile). */
UENUM(BlueprintType)
enum class EArenaProjKind : uint8
{
	Generic,
	/** XWeapons.Grenade: bounces (0.5 / 0.8 dampening), explodes 2 s after the first bounce or on a pawn. */
	Grenade,
	/** XWeapons.BioGlob: sticks, rests 2.25 s, merges, splashes globlings, drips from ceilings. */
	BioGlob,
	/** XWeapons.ShockProjectile: shootable core, combo when hit by the shock beam. */
	ShockBall,
	/** XWeapons.LinkProjectile. */
	LinkBolt,
	/** XWeapons.FlakChunk: 0-2 bounces, falls after the first wall, damage fades after 1 s. */
	FlakChunk,
	/** XWeapons.FlakShell: explodes on anything and throws 6 chunks. */
	FlakShell,
	/** XWeapons.RocketProj / SeekingRocketProj (flocking and homing). */
	Rocket
};

/** Parameters for a spawned projectile. Filled by the weapon from FArenaFireMode. */
USTRUCT(BlueprintType)
struct FArenaProjectileParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) float Speed = 3000.f;
	UPROPERTY(EditAnywhere) float MaxSpeed = 3000.f;
	UPROPERTY(EditAnywhere) float GravityScale = 0.f;
	UPROPERTY(EditAnywhere) float Damage = 20.f;
	/** 0 = direct hit only, otherwise radial damage with linear falloff */
	UPROPERTY(EditAnywhere) float DamageRadius = 0.f;
	UPROPERTY(EditAnywhere) float Momentum = 0.f;
	/** Number of bounces off world geometry before exploding (-1 = bounce until lifespan ends) */
	UPROPERTY(EditAnywhere) int32 Bounces = 0;
	/** Velocity kept on each bounce */
	UPROPERTY(EditAnywhere) float Bounciness = 0.6f;
	UPROPERTY(EditAnywhere) float LifeSpan = 5.f;
	/** Visual radius in cm */
	UPROPERTY(EditAnywhere) float Size = 10.f;
	UPROPERTY(EditAnywhere) FLinearColor Color = FLinearColor(0.2f, 0.6f, 1.f);
	/** Sticks to walls/floors and waits (goo); explodes on pawn touch or lifespan end */
	UPROPERTY(EditAnywhere) bool bSticky = false;
	/** Can be detonated by a hitscan beam from the same weapon type (pulse rifle combo) */
	UPROPERTY(EditAnywhere) bool bComboCore = false;
	/** Damage dealt by a combo detonation */
	UPROPERTY(EditAnywhere) float ComboDamage = 200.f;
	UPROPERTY(EditAnywhere) float ComboRadius = 550.f;
	/** Shards: damage scales down after first bounce */
	UPROPERTY(EditAnywhere) bool bShard = false;
	/** Homing/spiral not supported yet */
	UPROPERTY(EditAnywhere) FName ExplosionSound = NAME_None;
	UPROPERTY(EditAnywhere) FString WeaponName;
	/** Imported-art look (ArenaArtFX projectile table, e.g. "Rocket"); none or missing art = procedural glow. */
	UPROPERTY(EditAnywhere) FName Style = NAME_None;

	// ---- UT2004 projectile behaviour (all off by default) ----
	/** Special behaviour of a UT2004 projectile class. */
	UPROPERTY(EditAnywhere) EArenaProjKind Kind = EArenaProjKind::Generic;
	/** Collision radius in cm (UE2 CollisionRadius x 2); < 0 = derived from Size. */
	UPROPERTY(EditAnywhere) float CollisionRadius = -1.f;
	/** cm/s^2 along the flight direction up to MaxSpeed (LinkProjectile: 3000 UU/s^2). */
	UPROPERTY(EditAnywhere) float Acceleration = 0.f;
	/** cm/s added to the launch velocity's Z (FlakShell TossZ 225). */
	UPROPERTY(EditAnywhere) float TossZ = 0.f;
	/** Never hits its instigator (UE2 bCanHitOwner false / ProcessTouch Other != Instigator). */
	UPROPERTY(EditAnywhere) bool bIgnoreInstigator = true;
	/** Can be shot by instant hit weapons (UE2 bProjTarget, the shock core). */
	UPROPERTY(EditAnywhere) bool bProjTarget = false;
	/** UT2004 ammo cost of a shock combo, taken from the combo instigator's weapon. */
	UPROPERTY(EditAnywhere) int32 ComboAmmoCost = 0;
	UPROPERTY(EditAnywhere) float ComboMomentum = 0.f;
	/** Bio: goop level (damage BaseDamage + Damage x level, radius and momentum x sqrt(level)). */
	UPROPERTY(EditAnywhere) int32 GoopLevel = 1;
	UPROPERTY(EditAnywhere) float BaseDamage = 0.f;
	/** Homing target (seeking rockets). */
	TWeakObjectPtr<AActor> SeekTarget;
};

/**
 * UE2 view shake (PlayerController.ShakeView / WeaponFire.ShakeRotMag ...), in UE2 units: rotation magnitude and rate in
 * rotator units (X pitch, Y yaw, Z roll; 65536 = 360 degrees), offset magnitude and rate in UU along the view (X forward,
 * Y right, Z up). The times are UE2's shake counts (PlayerController.CheckShake). See ArenaViewShake.h.
 */
USTRUCT(BlueprintType)
struct FArenaViewShake
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FVector RotMag = FVector::ZeroVector;
	UPROPERTY(EditAnywhere) FVector RotRate = FVector::ZeroVector;
	UPROPERTY(EditAnywhere) float RotTime = 0.f;
	UPROPERTY(EditAnywhere) FVector OffsetMag = FVector::ZeroVector;
	UPROPERTY(EditAnywhere) FVector OffsetRate = FVector::ZeroVector;
	UPROPERTY(EditAnywhere) float OffsetTime = 0.f;

	FArenaViewShake() = default;
	FArenaViewShake(const FVector& InRotMag, const FVector& InRotRate, float InRotTime, const FVector& InOffsetMag, const FVector& InOffsetRate, float InOffsetTime)
		: RotMag(InRotMag), RotRate(InRotRate), RotTime(InRotTime), OffsetMag(InOffsetMag), OffsetRate(InOffsetRate), OffsetTime(InOffsetTime)
	{
	}
	bool IsZero() const { return RotMag.IsZero() && OffsetMag.IsZero(); }
	/** Magnitudes scaled (UE2 callers scale ShakeRotMag / ShakeOffsetMag by distance or zoom). */
	FArenaViewShake Scaled(float Scale) const
	{
		FArenaViewShake S = *this;
		S.RotMag *= Scale;
		S.OffsetMag *= Scale;
		return S;
	}
};

/** One fire mode of a weapon (index 0 = primary, 1 = alt). */
USTRUCT(BlueprintType)
struct FArenaFireMode
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) EArenaFireType FireType = EArenaFireType::Hitscan;
	UPROPERTY(EditAnywhere) float Damage = 10.f;
	/** Seconds between shots (or between damage ticks for Beam) */
	UPROPERTY(EditAnywhere) float RefireTime = 0.5f;
	UPROPERTY(EditAnywhere) int32 AmmoPerShot = 1;
	UPROPERTY(EditAnywhere) float Range = 20000.f;
	/** Cone half angle in degrees */
	UPROPERTY(EditAnywhere) float Spread = 0.f;
	UPROPERTY(EditAnywhere) int32 NumShots = 1;
	/** Impulse applied to hit pawns, in cm/s worth of velocity at 100 damage-equivalent */
	UPROPERTY(EditAnywhere) float Momentum = 0.f;
	/** Head shot multiplier for hitscan (0 = no headshots) */
	UPROPERTY(EditAnywhere) float HeadshotMultiplier = 0.f;
	/** Max charge time for Melee / charged projectiles (0 = no charge) */
	UPROPERTY(EditAnywhere) float MaxChargeTime = 0.f;
	UPROPERTY(EditAnywhere) FArenaProjectileParams Projectile;
	UPROPERTY(EditAnywhere) FLinearColor Color = FLinearColor(0.3f, 0.6f, 1.f);
	UPROPERTY(EditAnywhere) FName FireSound = NAME_None;
	/** Beam visual width for hitscan / beam modes */
	UPROPERTY(EditAnywhere) float BeamWidth = 4.f;

	// ---- UT2004 fire mode values (UE2 WeaponFire / InstantFire / ProjectileFire) ----
	/** Instant hit: damage is Damage (DamageMin), half the time + Rand(DamageMax - DamageMin + 1). 0 = Damage. */
	UPROPERTY(EditAnywhere) float DamageMax = 0.f;
	/** UE2 spread: 0 none; instant hit: aim + VRand() x FRand() x UE2Spread; projectiles: 1 random rotator +-UE2Spread/2
	 * units per axis, 2 a line UE2Spread units apart. Used instead of Spread when > 0. */
	UPROPERTY(EditAnywhere) uint8 SpreadStyle = 0;
	UPROPERTY(EditAnywhere) float UE2Spread = 0.f;
	/** Seconds the fire button must be held before the first shot (minigun wind-up). */
	UPROPERTY(EditAnywhere) float PreFireTime = 0.f;
	/** This mode uses the weapon's second ammo type (AltAmmo; assault rifle grenades). */
	UPROPERTY(EditAnywhere) bool bUseAltAmmo = false;
	/** UE2 InstantFire.bReflective (shock beams): a raised Shield Gun sends the beam on along the holder's view, up to 4 times. */
	UPROPERTY(EditAnywhere) bool bReflective = false;
	/** UE2 ProjectileFire.ProjSpawnOffset in UE2 units (forward, right, up from the eye; x2 for cm). Projectiles start
	 * there (or where a trace from the eye to it hits the world) and fly parallel to the view. */
	UPROPERTY(EditAnywhere) FVector ProjSpawnOffset = FVector(0.f, 0.f, -10.f);
	// ---- UE2 bot aiming / warning (WeaponFire.AimError, WarnTargetPct, bTossed, bRecommendSplashDamage) ----
	/** Bot aim error in UE2 rotator units (65536 = 360 deg) before Bot.AdjustAimError. */
	UPROPERTY(EditAnywhere) float BotAimError = 600.f;
	/** Chance an instant-hit shot warns the bot it is aimed at (it may dodge). */
	UPROPERTY(EditAnywhere) float WarnTargetPct = 0.f;
	/** Lobbed projectile (bots aim with a toss). */
	UPROPERTY(EditAnywhere) bool bTossed = false;
	/** Bots aim at the feet for splash damage. */
	UPROPERTY(EditAnywhere) bool bRecommendSplash = false;
	/** UE2 WeaponFire.ShakeRotMag / ShakeRotRate / ShakeRotTime / ShakeOffset*: the shooter's view shake on every shot
	 * (WeaponFire.ShakeView -> PlayerController.WeaponShakeView; off with the "Weapons Shake View" setting). */
	UPROPERTY(EditAnywhere) FArenaViewShake Shake;
};

namespace ArenaTeam
{
	static constexpr int32 None = -1;
	static constexpr int32 Red = 0;
	static constexpr int32 Blue = 1;

	inline FLinearColor Color(int32 Team)
	{
		switch (Team)
		{
		case Red:  return FLinearColor(1.f, 0.12f, 0.08f);
		case Blue: return FLinearColor(0.1f, 0.35f, 1.f);
		default:   return FLinearColor(0.85f, 0.85f, 0.3f);
		}
	}

	inline FString Name(int32 Team)
	{
		return Team == Red ? TEXT("Red") : Team == Blue ? TEXT("Blue") : TEXT("");
	}
}

/**
 * Gameplay constants, scaled to centimetres (1 UE2 unit = 2 cm, ArenaUE2World::Scale). Movement values are UT2004's own
 * (Engine.Pawn / UnrealGame.UnrealPawn / XGame.xPawn defaults, PlayerInput.DoubleClickTime) times 2.
 */
namespace ArenaTuning
{
	/** UE2 gravity -950 UU/s^2 = -1900 cm/s^2 against our world gravity of -980. */
	static constexpr float GravityScale = 1900.f / 980.f;
	/** xPawn GroundSpeed / AirSpeed 440. */
	static constexpr float GroundSpeed = 880.f;
	/** xPawn WaterSpeed 220. */
	static constexpr float WaterSpeed = 440.f;
	/** Pawn AccelRate 2048. */
	static constexpr float AccelRate = 4096.f;
	/** UnrealPawn AirControl 0.35. */
	static constexpr float AirControl = 0.35f;
	/** xPawn JumpZ 340. */
	static constexpr float JumpZ = 680.f;
	/** xPawn MultiJumpBoost 25 (double jump: JumpZ + MultiJumpBoost), MaxMultiJump 1. */
	static constexpr float MultiJumpBoost = 50.f;
	/** xPawn.DoJump: a double jump needs |Velocity.Z| < 100 UU/s. */
	static constexpr float DoubleJumpMaxVelZ = 200.f;
	/** xPawn DodgeSpeedFactor 1.5 (times GroundSpeed), DodgeSpeedZ 210. */
	static constexpr float DodgeSpeedFactor = 1.5f;
	static constexpr float DodgeSpeed = GroundSpeed * DodgeSpeedFactor;
	static constexpr float DodgeSpeedZ = 420.f;
	/** xPawn.Dodge wall trace: from the feet, one collision radius out, 32 UU further. */
	static constexpr float WallDodgeTraceDist = 64.f;
	/** PlayerInput.DoubleClickTime 0.25 (timers run at 1.1x real time, as in PlayerController.PlayerMove). */
	static constexpr float DoubleTapTime = 0.25f;
	/** PlayerInput: after landing from a dodge, no new dodge until the double-click timer passes -0.35. */
	static constexpr float DodgeCooldown = 0.35f;
	/** xPawn WalkingPct / CrouchedPct 0.4. */
	static constexpr float WalkingPct = 0.4f;
	static constexpr float CrouchedPct = 0.4f;
	/** Pawn MaxFallSpeed 1200: landing faster than this hurts (Pawn.TakeFallingDamage). */
	static constexpr float MaxFallSpeed = 2400.f;
	/** UE2 MAXSTEPHEIGHT 35 and MINFLOORZ 0.7 (acos 0.7 = 45.57 degrees). */
	static constexpr float MaxStepHeight = 70.f;
	static constexpr float WalkableFloorZ = 0.7f;
	static constexpr float StartHealth = 100.f;
	static constexpr float MaxHealth = 100.f;
	static constexpr float SuperHealthMax = 199.f;
	static constexpr float MaxShield = 150.f;
	static constexpr float ShieldAbsorb = 0.75f;
	static constexpr float MaxAdrenaline = 100.f;
	static constexpr float AdrenalinePerKill = 5.f;
	static constexpr float ComboDuration = 16.f;
	static constexpr float DoubleDamageDuration = 30.f;
	static constexpr float SpawnProtectionTime = 2.f;
	/** xPawn CollisionRadius 25, CollisionHeight 44, CrouchHeight 29. */
	static constexpr float CapsuleRadius = 50.f;
	static constexpr float CapsuleHalfHeight = 88.f;
	static constexpr float CrouchedHalfHeight = 58.f;
	/** xPawn BaseEyeHeight 38; crouched: Pawn.StartCrouch min(0.8 x CrouchHeight, CrouchHeight - 10) = 19. */
	static constexpr float EyeHeight = 76.f;
	static constexpr float CrouchedEyeHeight = 38.f;
	/** Pawn.Bob (User.ini default 0.006): view bob per unit of speed. */
	static constexpr float ViewBob = 0.006f;
	static constexpr float MaxJumpHeight = 230.f;   // double jump reach used by bot nav
}

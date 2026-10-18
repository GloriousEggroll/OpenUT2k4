#include "ArenaWeapons.h"
#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaCharacter.h"
#include "ArenaFX.h"
#include "ArenaProjectile.h"
#include "ArenaDamage.h"
#include "ArenaGameMode.h"
#include "ArenaBotController.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	/** Cylinder (axis Z) lying along the view X axis. */
	const FRotator AlongX(90.f, 0.f, 0.f);
	/** Cone with its tip pointing forward (+X). */
	const FRotator ConeForward(-90.f, 0.f, 0.f);

	/** Cylinder scale for a given diameter / length in cm. */
	FVector Tube(float Diameter, float Length)
	{
		return FVector(Diameter / 100.f, Diameter / 100.f, Length / 100.f);
	}

	/** Cube scale for a box size in cm. */
	FVector Box(float X, float Y, float Z)
	{
		return FVector(X / 100.f, Y / 100.f, Z / 100.f);
	}

	void SetPartOpacity(UStaticMeshComponent* Part, float Opacity)
	{
		if (Part)
		{
			if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Part->GetMaterial(0)))
			{
				MID->SetScalarParameterValue(TEXT("Opacity"), Opacity);
			}
		}
	}
}

// =================================================================================================
// 1 Gauntlet
// =================================================================================================

AWeap_Gauntlet::AWeap_Gauntlet()
{
	WeaponName = TEXT("Gauntlet");
	Slot = 1;
	MaxAmmo = 100;
	InitialAmmo = 100;
	AmmoPickupAmount = 0;
	bInfiniteAmmo = false; // primary costs nothing; ammo is the shield's energy and recharges
	Priority = 0.1f;
	WeaponColor = FLinearColor(0.55f, 0.38f, 0.22f);
	MuzzleOffset = FVector(50.f, 12.f, -16.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::KickBack(); // ShieldFire ShakeRot / ShakeOffset defaults
	// XWeapons.ShieldFire: 40..150 damage and 65000..100000 force over a 0.4..2.5 s charge, ShieldRange 112 UU
	// from the pawn's centre; the shield jump (charged into the floor) is AArenaWeapon::FireMelee.
	P.FireType = EArenaFireType::Melee;
	P.Damage = 150.f;
	P.RefireTime = 0.6f;
	P.AmmoPerShot = 0;
	P.Range = 224.f;
	P.Momentum = 2000.f;
	P.MaxChargeTime = 2.5f;
	P.Color = FLinearColor(1.f, 0.55f, 0.15f);
	P.FireSound = TEXT("S_Fire_Hammer");
	P.BeamWidth = 0.f;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Shield;
	A.Damage = 0.f;
	A.RefireTime = 0.1f; // drain 1 energy every 0.1 s while held
	A.AmmoPerShot = 1;
	A.Range = 0.f;
	A.Color = FLinearColor(0.3f, 0.85f, 1.f);
	A.FireSound = TEXT("S_Fire_Hammer");

	// ---- UT2004 XWeapons.ShieldGun (ShieldFire / ShieldAltFire, ShieldAmmo 100) ----
	Priority = 0.35f;
	MaxAmmo = 100;
	InitialAmmo = 100;
	P.Damage = 150.f;           // MaxDamage (MinDamage 40 at MinHoldTime 0.4 s)
	P.RefireTime = 0.6f;
	P.Range = 224.f;            // ShieldRange 112
	P.Momentum = 2000.f;        // MaxForce 100000 (MinForce 65000)
	P.MaxChargeTime = 2.5f;     // FullyChargedTime
	A.RefireTime = 0.2f;        // AmmoRegenTime: 1 shield energy per 0.2 s while up / regained while down
	A.AmmoPerShot = 1;
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].WarnTargetPct = 0.1f;
}

void AWeap_Gauntlet::BuildViewModel()
{
	const FLinearColor Steel(0.32f, 0.33f, 0.36f);
	AddViewPart(ArenaAssets::Cylinder(), FVector(22.f, 12.f, -18.f), AlongX, Tube(9.f, 22.f), Steel);
	AddViewPart(ArenaAssets::Cube(), FVector(39.f, 12.f, -16.f), FRotator::ZeroRotator, Box(15.f, 12.f, 12.f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(39.f, 12.f, -9.f), FRotator::ZeroRotator, Box(12.f, 10.f, 2.f), Steel);
	AddViewPart(ArenaAssets::Cube(), FVector(39.f, 12.f, -23.f), FRotator::ZeroRotator, Box(12.f, 10.f, 2.f), Steel);
	ChargeGlow = AddViewPart(ArenaAssets::Cylinder(), FVector(47.5f, 12.f, -16.f), AlongX, Tube(10.f, 1.5f), FireModes[0].Color, true);

	ShieldPlate = AddViewPart(ArenaAssets::Sphere(), FVector(55.f, 4.f, -8.f), FRotator::ZeroRotator, FVector(0.02f, 0.5f, 0.45f), FireModes[1].Color, true);
	SetPartOpacity(ShieldPlate, 0.25f);
	if (ShieldPlate)
	{
		ShieldPlate->SetVisibility(false);
	}

	// World-space shield seen by everyone else.
	if (ArenaOwner && ArenaOwner->GetRootComponent())
	{
		ShieldVisual = NewObject<UStaticMeshComponent>(this, TEXT("ShieldVisual"));
		ShieldVisual->SetStaticMesh(ArenaAssets::Sphere());
		ShieldVisual->SetupAttachment(ArenaOwner->GetRootComponent());
		ShieldVisual->SetRelativeLocation(FVector(60.f, 0.f, 10.f));
		ShieldVisual->SetRelativeScale3D(FVector(0.12f, 1.3f, 1.7f));
		ShieldVisual->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		ShieldVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ShieldVisual->SetGenerateOverlapEvents(false);
		ShieldVisual->SetCastShadow(false);
		ShieldVisual->SetCanEverAffectNavigation(false);
		ShieldVisual->SetOwnerNoSee(true);
		if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeGlow(this, FireModes[1].Color, 4.f, 0.4f))
		{
			ShieldVisual->SetMaterial(0, MID);
		}
		// Imported shield effect mesh (keeps its own material) instead of the glow disc.
		if (UStaticMesh* ShieldMesh = ArenaArt::Load<UStaticMesh>(TEXT("StaticMeshes/WeaponStaticMesh/Shield")))
		{
			ShieldVisual->SetStaticMesh(ShieldMesh);
			ShieldVisual->SetMaterial(0, nullptr);
			ArenaArtFX::FitMesh(ShieldVisual, 165.f, FRotator::ZeroRotator, FVector(60.f, 0.f, 10.f), true);
		}
		ShieldVisual->SetVisibility(false);
		ShieldVisual->RegisterComponent();
	}

	// First-person shield for the imported view model (the procedural plate is hidden with the procedural parts).
	if (ViewRoot)
	{
		if (UStaticMesh* ShieldMesh = ArenaArt::Load<UStaticMesh>(TEXT("StaticMeshes/WeaponStaticMesh/Shield")))
		{
			ShieldArtView = NewObject<UStaticMeshComponent>(this, TEXT("ShieldArtView"));
			ShieldArtView->SetStaticMesh(ShieldMesh);
			ShieldArtView->SetupAttachment(ViewRoot);
			ShieldArtView->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ShieldArtView->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			ShieldArtView->SetGenerateOverlapEvents(false);
			ShieldArtView->SetCastShadow(false);
			ShieldArtView->SetCanEverAffectNavigation(false);
			ShieldArtView->SetOnlyOwnerSee(true);
			ShieldArtView->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
			ArenaArtFX::FitMesh(ShieldArtView, 70.f, FRotator::ZeroRotator, FVector(60.f, 6.f, -14.f), true);
			ShieldArtView->SetVisibility(false);
			ShieldArtView->RegisterComponent();
		}
	}
}

void AWeap_Gauntlet::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const bool bAlive = ArenaOwner && !ArenaOwner->IsDead();
	const bool bShieldOn = bEquipped && bAlive && bFireHeld[1] && Ammo > 0;
	if (ShieldVisual && ShieldVisual->IsVisible() != bShieldOn)
	{
		ShieldVisual->SetVisibility(bShieldOn);
	}
	if (ShieldPlate && ShieldPlate->IsVisible() != bShieldOn)
	{
		ShieldPlate->SetVisibility(bShieldOn && bEquipped);
	}
	if (ShieldArtView)
	{
		const bool bArtOn = bShieldOn && HasImportedViewModel();
		if (ShieldArtView->IsVisible() != bArtOn)
		{
			ShieldArtView->SetVisibility(bArtOn);
		}
	}

	// ShieldAltFire.Timer: shield energy comes back 1 per AmmoRegenTime (0.2 s) while the shield is down.
	if (!bShieldOn)
	{
		ShieldDrainAccum += DeltaSeconds;
		while (ShieldDrainAccum >= FireModes[1].RefireTime)
		{
			ShieldDrainAccum -= FireModes[1].RefireTime;
			if (Ammo < MaxAmmo)
			{
				++Ammo;
			}
		}
	}
	else
	{
		ShieldDrainAccum = 0.f;
	}

	// ShieldFire.Timer (AutoFireTestFreq 0.15 s): while charging, a pawn within ShieldRange of the centre releases the shot.
	if (bEquipped && bAlive && bFireHeld[0] && GetWorldTime() >= NextAutoFireTest)
	{
		NextAutoFireTest = GetWorldTime() + 0.15f;
		FVector Eye, Dir;
		GetAim(Eye, Dir);
		const FVector Start = ArenaOwner->GetActorLocation();
		FHitResult Hit;
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaShieldAutoFire), false, this);
		QP.AddIgnoredActor(ArenaOwner);
		if (GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Dir * FireModes[0].Range, ECC_Weapon, QP) && Cast<APawn>(Hit.GetActor()))
		{
			StopFire(0);
		}
	}

	// Emitter swells and jitters while charging.
	if (ChargeGlow && bEquipped)
	{
		float Charge = 0.f;
		if (bFireHeld[0] && FireModes[0].MaxChargeTime > 0.f)
		{
			Charge = FMath::Clamp((GetWorldTime() - ChargeStart[0]) / FireModes[0].MaxChargeTime, 0.f, 1.f);
		}
		const float S = 1.f + Charge * 0.6f;
		ChargeGlow->SetRelativeScale3D(Tube(10.f * S, 1.5f));
		const float Jitter = Charge > 0.f ? FMath::FRandRange(-0.4f, 0.4f) * Charge : 0.f;
		ChargeGlow->SetRelativeLocation(FVector(47.5f, 12.f + Jitter, -16.f + Jitter));
	}
}

void AWeap_Gauntlet::StartFire(uint8 Mode)
{
	// ShieldAltFire AmmoPerFire 15: the shield needs 15 energy to come up
	if (Mode == 1 && !bFireHeld[1] && Ammo < 15)
	{
		return;
	}
	Super::StartFire(Mode);
}

bool AWeap_Gauntlet::CheckReflect(const FVector& HitLocation, FVector& OutRefNormal, int32 AmmoDrain)
{
	// ShieldGun.CheckReflect: shield up with energy left, hit from within ~112 degrees of the view direction
	if (!bEquipped || !bFireHeld[1] || Ammo <= 0 || !ArenaOwner)
	{
		return false;
	}
	const FVector FaceDir = ArenaOwner->GetViewRotation().Vector();
	const FVector HitDir = (ArenaOwner->GetActorLocation() - HitLocation + FVector(0.f, 0.f, 16.f)).GetSafeNormal();
	OutRefNormal = FaceDir;
	if (FVector::DotProduct(FaceDir, HitDir) >= -0.37f)
	{
		return false;
	}
	if (AmmoDrain > 0)
	{
		Ammo = FMath::Max(0, Ammo - AmmoDrain);
		ReflectCount++;
	}
	return true;
}

float AWeap_Gauntlet::AdjustPlayerDamage(float Damage, AController* InstigatedBy, const FVector& HitLocation, FVector& Momentum, bool bFell)
{
	// ShieldGun.AdjustPlayerDamage: the shield drains min(2 x energy, damage, 100 (20 for falls)) of the damage, costs
	// half of it in energy (all of it while carrying the flag) and adds 25% momentum.
	const float DamageMax = bFell ? 20.f : 100.f;
	FVector RefNormal;
	if (HitLocation.IsZero() || !CheckReflect(HitLocation, RefNormal, 0))
	{
		return Damage;
	}
	float Drain = FMath::Min(float(Ammo) * 2.f, Damage);
	Drain = FMath::FloorToFloat(FMath::Min(Drain, DamageMax));
	Damage -= Drain;
	Momentum *= 1.25f;
	if (ArenaOwner && ArenaOwner->IsCarryingObjective())
	{
		Ammo = FMath::Max(0, Ammo - int32(FMath::Min(float(Ammo), Drain)));
	}
	else
	{
		Ammo = FMath::Max(0, Ammo - int32(Drain) / 2);
	}
	return Damage;
}

void AWeap_Gauntlet::FireMelee(uint8 Mode, float ChargeAlpha)
{
	Super::FireMelee(Mode, ChargeAlpha);
	SpawnViewMuzzleFlash(FireModes[0].Color, 1.f + 2.f * ChargeAlpha);
}

// =================================================================================================
// 2 Carbine
// =================================================================================================

AWeap_Carbine::AWeap_Carbine()
{
	WeaponName = TEXT("Carbine");
	Slot = 2;
	MaxAmmo = 150;
	InitialAmmo = 100;
	AmmoPickupAmount = 50;
	Priority = 0.3f;
	WeaponColor = FLinearColor(0.38f, 0.4f, 0.26f);
	MuzzleOffset = FVector(64.f, 14.f, -13.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::AssaultFire(); // AssaultFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 8.f;
	P.RefireTime = 0.16f;
	P.AmmoPerShot = 1;
	P.Range = 12000.f;
	P.Spread = 1.2f;
	P.Momentum = 60.f;
	P.Color = FLinearColor(1.f, 0.85f, 0.35f);
	P.FireSound = TEXT("S_Fire_Rifle");
	P.BeamWidth = 1.2f;

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::KickBack(); // AssaultGrenade ShakeRot / ShakeOffset defaults
	A.FireType = EArenaFireType::Projectile;
	A.RefireTime = 0.9f;
	A.AmmoPerShot = 5;
	A.Color = FLinearColor(0.7f, 1.f, 0.3f);
	A.FireSound = TEXT("S_Fire_Goo");
	A.Projectile.Speed = 1600.f;
	A.Projectile.MaxSpeed = 1600.f;
	A.Projectile.GravityScale = 1.f;
	// XWeapons.Grenade: Damage 70, DamageRadius 220, MomentumTransfer 75000
	A.Projectile.Damage = 70.f;
	A.Projectile.DamageRadius = 440.f;
	A.Projectile.Momentum = 1500.f;
	A.Projectile.Bounces = 2;
	A.Projectile.Bounciness = 0.5f;
	A.Projectile.LifeSpan = 2.5f;
	A.Projectile.Size = 7.f;
	A.Projectile.Color = A.Color;
	A.Projectile.ExplosionSound = TEXT("S_Explosion");
	A.Projectile.Style = TEXT("Grenade");

	// ---- UT2004 XWeapons.AssaultRifle (AssaultFire, AssaultGrenade; AssaultAmmo 100/200, GrenadeAmmo 4/8) ----
	Priority = 0.4f;
	MaxAmmo = 200;
	InitialAmmo = 100;
	AmmoPickupAmount = 50;      // AssaultAmmoPickup: +50 bullets and +4 grenades
	AltMaxAmmo = 8;
	AltInitialAmmo = 4;
	AltAmmoPickupAmount = 4;
	P.Damage = 7.f;
	P.DamageMax = 7.f;
	P.RefireTime = 0.16f;
	P.Range = 20000.f;          // TraceRange 10000
	P.Spread = 0.f;
	P.UE2Spread = 0.02f;        // grows +0.02 per shot to 0.12, back to 0.02 after 0.5 s
	P.Momentum = 0.f;
	A.bUseAltAmmo = true;
	A.AmmoPerShot = 1;
	A.RefireTime = 1.f;
	A.MaxChargeTime = 1.f;      // hold: 850 UU/s + 750 UU/s per second, up to 1600
	A.Projectile.Kind = EArenaProjKind::Grenade;
	A.Projectile.Speed = 1700.f;
	A.Projectile.MaxSpeed = 3200.f;
	A.Projectile.GravityScale = ArenaTuning::GravityScale;
	A.Projectile.Damage = 70.f;
	A.Projectile.DamageRadius = 440.f;
	A.Projectile.Momentum = 1500.f;
	A.Projectile.LifeSpan = 14.f;
	A.Projectile.CollisionRadius = 0.1f;   // UE2 0
	A.Projectile.Bounces = -1;
	// UE2 ProjSpawnOffset (UU)
	FireModes[1].ProjSpawnOffset = FVector(25.0f, 10.0f, -7.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].BotAimError = 800.f;
	FireModes[1].WarnTargetPct = 0.5f;
	FireModes[1].bTossed = true;
	FireModes[1].bRecommendSplash = true;
}

void AWeap_Carbine::BuildViewModel()
{
	const FLinearColor Dark(0.12f, 0.12f, 0.12f);
	AddViewPart(ArenaAssets::Cube(), FVector(30.f, 14.f, -16.f), FRotator::ZeroRotator, Box(38.f, 6.f, 8.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(54.f, 14.f, -13.f), AlongX, Tube(3.f, 20.f), Dark);
	AddViewPart(ArenaAssets::Cylinder(), FVector(44.f, 14.f, -19.5f), AlongX, Tube(5.f, 16.f), Dark);
	AddViewPart(ArenaAssets::Cube(), FVector(30.f, 14.f, -23.f), FRotator(15.f, 0.f, 0.f), Box(6.f, 4.f, 12.f), Dark);
	AddViewPart(ArenaAssets::Cube(), FVector(22.f, 14.f, -11.5f), FRotator::ZeroRotator, Box(8.f, 2.f, 3.f), Dark);
	AddViewPart(ArenaAssets::Cube(), FVector(32.f, 17.1f, -15.f), FRotator::ZeroRotator, Box(26.f, 0.5f, 1.2f), FireModes[0].Color, true);
}

float AWeap_Carbine::GetModeRefireScale(uint8 Mode) const
{
	return Mode == 0 && bDualMode ? 0.55f : 1.f;
}

float AWeap_Carbine::GetHitscanSpread(uint8 Mode) const
{
	return Mode == 0 ? CurrentSpread * (bDualMode ? 1.5f : 1.f) : Super::GetHitscanSpread(Mode);
}

float AWeap_Carbine::GetHitscanDamageScale(uint8 Mode) const
{
	// AssaultFire.StartBerserk: DamageMin / DamageMax x 1.33
	if (Mode == 0 && ArenaOwner && ArenaOwner->GetActiveCombo() == EArenaCombo::Berserk && FireModes[0].Damage > 0.f)
	{
		return FMath::FloorToFloat(FireModes[0].Damage * 1.33f) / FireModes[0].Damage;
	}
	return 1.f;
}

void AWeap_Carbine::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	if (Mode == 0)
	{
		// AssaultFire.ModeDoFire
		const float Now = GetWorldTime();
		CurrentSpread = Now - LastPrimaryFireTime > 0.5f ? FireModes[0].UE2Spread : FMath::Min(CurrentSpread + 0.02f, 0.12f);
		LastPrimaryFireTime = Now;
	}
	Super::FireHitscan(Mode, ChargeAlpha);
}

void AWeap_Carbine::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	if (Mode != 1 || !ArenaOwner)
	{
		Super::FireProjectile(Mode, ChargeAlpha);
		return;
	}
	// AssaultGrenade.SpawnProjectile: Speed = clamp(850 + HoldTime x 750, 850, 1600) UU/s (bots: max) + the shooter's
	// forward speed; berserk gains 1.5x faster.
	const FArenaFireMode& FM = FireModes[1];
	const float Hold = FMath::Clamp(ChargeAlpha, 0.f, 1.f) * FM.MaxChargeTime;
	const float Gain = 750.f * (ArenaOwner->GetActiveCombo() == EArenaCombo::Berserk ? 1.5f : 1.f);
	float SpeedUU = ArenaOwner->IsPlayerControlled() ? FMath::Clamp(850.f + Hold * Gain, 850.f, 1600.f) : 1600.f;
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	FArenaProjectileParams P = FM.Projectile;
	P.Speed = SpeedUU * 2.f + FVector::DotProduct(Dir, ArenaOwner->GetVelocity());
	P.MaxSpeed = FMath::Max(P.MaxSpeed, P.Speed);
	SpawnProjectiles(P, 1, 0.f);
}

bool AWeap_Carbine::GiveAmmoPickup()
{
	// AssaultRifle.HandlePickupQuery (grenade ammo pickup): +50 bullets and +AmmoAmount grenades unless both are full
	if (Ammo >= MaxAmmo && AltAmmo >= AltMaxAmmo)
	{
		return false;
	}
	AddAmmo(AmmoPickupAmount);
	AddAltAmmo(AltAmmoPickupAmount);
	return true;
}

void AWeap_Carbine::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bDualMode)
	{
		return;
	}
	const float Now = GetWorldTime();
	if (!ViewMesh)
	{
		// procedural view model: add the mirrored copy once
		if (!bMirroredParts && ViewRoot)
		{
			bMirroredParts = true;
			bMirrorViewParts = true;
			BuildViewModel();
			bMirrorViewParts = false;
		}
		return;
	}
	if (!OffhandViewMesh && Art)
	{
		OffhandViewMesh = NewObject<USkeletalMeshComponent>(this, TEXT("OffhandViewMesh"));
		OffhandViewMesh->SetupAttachment(ViewRoot);
		OffhandViewMesh->SetSkeletalMeshAsset(ViewMesh->GetSkeletalMeshAsset());
		OffhandViewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		OffhandViewMesh->SetGenerateOverlapEvents(false);
		OffhandViewMesh->SetCanEverAffectNavigation(false);
		OffhandViewMesh->SetCastShadow(false);
		OffhandViewMesh->SetReceivesDecals(false);
		OffhandViewMesh->SetOnlyOwnerSee(true);
		OffhandViewMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
		OffhandViewMesh->bComponentUseFixedSkelBounds = true;
		// UE2 draws the same actor twice: the left gun shows the right gun's animation
		OffhandViewMesh->SetLeaderPoseComponent(ViewMesh);
		OffhandViewMesh->RegisterComponent();
		AddInstanceComponent(OffhandViewMesh);
		DualPickupTime = Now;
	}
	if (OffhandViewMesh && Art)
	{
		// Hand = -Hand: DrawScale3D.Y, PlayerViewOffset.Y and the pivot's yaw / roll mirrored; for 0.5 s after the
		// pickup the gun swings up from 90 degrees below the view (Pitch - 16384 - 32768 x (DualPickupTime - Now))
		const float Flip = FMath::Clamp(Now - DualPickupTime, 0.f, 0.5f);
		// the other hand of the main gun (which follows the player's weapon hand; a centred hand is drawn left handed
		// with the second gun right handed, like AssaultRifle.RenderOverlays in dual mode)
		const FVector L = ViewMesh->GetRelativeLocation();
		const FRotator R = ViewMesh->GetRelativeRotation();
		const FVector S = ViewMesh->GetRelativeScale3D();
		OffhandViewMesh->SetRelativeLocationAndRotation(FVector(L.X, -L.Y, L.Z), FRotator(R.Pitch - 90.f + 180.f * Flip, -R.Yaw, -R.Roll));
		OffhandViewMesh->SetRelativeScale3D(FVector(S.X, -S.Y, S.Z));
		OffhandViewMesh->SetVisibility(ViewMesh->IsVisible());
		OffhandViewMesh->SetHiddenInGame(ViewMesh->bHiddenInGame);
	}
}

bool AWeap_Carbine::OnRepeatPickup(bool bWeaponStay)
{
	if (bDualMode)
	{
		return Super::OnRepeatPickup(bWeaponStay);
	}
	// A second assault rifle: dual mode (fire rate x0.55, spread x1.5, double ammo capacity) plus its ammo.
	bDualMode = true;
	MaxAmmo *= 2;
	AltMaxAmmo *= 2;
	AddAmmo(InitialAmmo);
	AddAltAmmo(AltInitialAmmo);
	// AssaultRifle.HandlePickupQuery: AttachToPawn again (the second rifle appears in the left hand)
	if (ArenaOwner)
	{
		ArenaOwner->RefreshThirdPersonWeapon();
	}
	return true;
}

// =================================================================================================
// 3 Goo Launcher
// =================================================================================================

AWeap_GooLauncher::AWeap_GooLauncher()
{
	WeaponName = TEXT("Goo Launcher");
	Slot = 3;
	MaxAmmo = 60;
	InitialAmmo = 25;
	AmmoPickupAmount = 15;
	Priority = 0.45f;
	WeaponColor = FLinearColor(0.12f, 0.3f, 0.12f);
	MuzzleOffset = FVector(60.f, 14.f, -16.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::BioFire(); // BioFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 0.28f;
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(0.3f, 1.f, 0.2f);
	P.FireSound = TEXT("S_Fire_Goo");
	P.Projectile.Speed = 2000.f;
	P.Projectile.MaxSpeed = 2000.f;
	P.Projectile.GravityScale = 1.f;
	P.Projectile.Damage = 30.f;
	P.Projectile.DamageRadius = 120.f;
	P.Projectile.Momentum = 300.f;
	P.Projectile.Bounces = 0;
	P.Projectile.LifeSpan = 6.f;
	P.Projectile.Size = 10.f;
	P.Projectile.Color = P.Color;
	P.Projectile.bSticky = true;
	P.Projectile.ExplosionSound = TEXT("S_Impact");
	P.Projectile.Style = TEXT("Goo");

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::BioChargedFire(); // BioChargedFire ShakeRot / ShakeOffset defaults
	A = P;
	A.RefireTime = 0.6f;
	A.MaxChargeTime = 1.6f; // charge up to 5 globs into one
	A.Color = FLinearColor(0.55f, 1.f, 0.25f);
	A.Projectile.Color = A.Color;
	A.Projectile.Speed = 1700.f;
	A.Projectile.MaxSpeed = 1700.f;
	A.Projectile.LifeSpan = 8.f;
	A.Projectile.ExplosionSound = TEXT("S_Explosion");

	// ---- UT2004 XWeapons.BioRifle (BioFire, BioChargedFire; BioAmmo 20/50, BioGlob) ----
	Priority = 0.55f;
	MaxAmmo = 50;
	InitialAmmo = 20;
	AmmoPickupAmount = 20;
	P.RefireTime = 0.33f;
	P.AmmoPerShot = 1;
	P.Projectile.Kind = EArenaProjKind::BioGlob;
	P.Projectile.Speed = 4000.f;
	P.Projectile.MaxSpeed = 4000.f;
	P.Projectile.GravityScale = ArenaTuning::GravityScale;
	P.Projectile.BaseDamage = 20.f;   // BlowUp: BaseDamage + Damage x GoopLevel
	P.Projectile.Damage = 19.f;
	P.Projectile.DamageRadius = 240.f;
	P.Projectile.Momentum = 800.f;
	P.Projectile.LifeSpan = 20.f;
	P.Projectile.CollisionRadius = 4.f;
	P.Projectile.bSticky = false;
	P.Projectile.GoopLevel = 1;
	A = P;
	A.Color = FLinearColor(0.55f, 1.f, 0.25f);
	A.Projectile.Color = A.Color;
	A.RefireTime = 0.33f;
	A.AmmoPerShot = 0;          // taken while loading (GoopUpRate 0.25 s, MaxGoopLoad 10)
	A.MaxChargeTime = 2.5f;
	A.Projectile.ExplosionSound = TEXT("S_Explosion");
	// UE2 ProjSpawnOffset (UU)
	FireModes[0].ProjSpawnOffset = FVector(20.0f, 9.0f, -6.0f);
	FireModes[1].ProjSpawnOffset = FVector(20.0f, 9.0f, -6.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].WarnTargetPct = 0.5f;
	FireModes[0].bTossed = true;
	FireModes[0].bRecommendSplash = true;
	FireModes[1].WarnTargetPct = 0.8f;
	FireModes[1].bTossed = true;
	FireModes[1].bRecommendSplash = true;
}

void AWeap_GooLauncher::BuildViewModel()
{
	const FLinearColor Grey(0.35f, 0.36f, 0.34f);
	AddViewPart(ArenaAssets::Cylinder(), FVector(32.f, 14.f, -16.f), AlongX, Tube(14.f, 30.f), WeaponColor);
	AddViewPart(ArenaAssets::Sphere(), FVector(32.f, 14.f, -9.f), FRotator::ZeroRotator, FVector(0.16f, 0.07f, 0.06f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cone(), FVector(53.f, 14.f, -16.f), ConeForward, Tube(10.f, 14.f), Grey);
	AddViewPart(ArenaAssets::Cylinder(), FVector(18.f, 14.f, -16.f), AlongX, Tube(16.f, 3.f), Grey);
	AddViewPart(ArenaAssets::Cylinder(), FVector(44.f, 14.f, -16.f), AlongX, Tube(15.f, 2.f), Grey);
}

void AWeap_GooLauncher::StartFire(uint8 Mode)
{
	const bool bWasHeld = Mode < 2 && bFireHeld[Mode];
	Super::StartFire(Mode);
	if (Mode == 1 && !bWasHeld && bFireHeld[1])
	{
		// BioChargedFire.HOLD.BeginState: the first goop loads at once
		GoopLoad = 0;
		NextGoopTime = GetWorldTime();
	}
}

void AWeap_GooLauncher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bEquipped || !bFireHeld[1] || !ArenaOwner || ArenaOwner->IsDead())
	{
		return;
	}
	// BioChargedFire.HOLD.Timer: one goop per GoopUpRate (0.25 s, x0.75 berserk), each taking 1 ammo, up to 10
	const float Now = GetWorldTime();
	while (Now >= NextGoopTime && GoopLoad < 10 && Ammo > 0)
	{
		++GoopLoad;
		--Ammo;
		NextGoopTime += 0.25f * GetRefireScale();
	}
}

void AWeap_GooLauncher::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	if (Mode != 1)
	{
		Super::FireProjectile(Mode, ChargeAlpha);
		return;
	}
	if (GoopLoad <= 0)
	{
		return;
	}
	// BioChargedFire.SpawnProjectile: one glob of GoopLevel = load; BioGlob.AdjustSpeed: Speed x (0.4 + L) / (1.4 x L)
	FArenaProjectileParams P = FireModes[1].Projectile;
	P.GoopLevel = GoopLoad;
	P.Speed = P.Speed * (0.4f + float(GoopLoad)) / (1.4f * float(GoopLoad));
	GoopLoad = 0;
	SpawnProjectiles(P, 1, 0.f);
}

// =================================================================================================
// 4 Pulse Rifle
// =================================================================================================

AWeap_PulseRifle::AWeap_PulseRifle()
{
	WeaponName = TEXT("Pulse Rifle");
	Slot = 4;
	MaxAmmo = 50;
	InitialAmmo = 20;
	AmmoPickupAmount = 10;
	Priority = 0.6f;
	WeaponColor = FLinearColor(0.85f, 0.85f, 0.92f);
	MuzzleOffset = FVector(64.f, 13.f, -13.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::ShockBeamFire(); // ShockBeamFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 45.f;
	P.RefireTime = 0.75f;
	P.AmmoPerShot = 1;
	P.Range = 20000.f;
	P.Momentum = 600.f;
	P.Color = FLinearColor(0.65f, 0.3f, 1.f);
	P.FireSound = TEXT("S_Fire_Pulse");
	P.BeamWidth = 6.f;
	P.bReflective = true;       // ShockBeamFire.bReflective

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::ShockProjFire(); // ShockProjFire ShakeRot / ShakeOffset defaults
	A.FireType = EArenaFireType::Projectile;
	A.RefireTime = 0.6f;
	A.AmmoPerShot = 1;
	A.Color = FLinearColor(0.9f, 0.4f, 1.f);
	A.FireSound = TEXT("S_Fire_Pulse");
	A.Projectile.Speed = 1150.f;
	A.Projectile.MaxSpeed = 1150.f;
	A.Projectile.GravityScale = 0.f;
	A.Projectile.Damage = 45.f;
	A.Projectile.DamageRadius = 70.f;
	A.Projectile.Momentum = 500.f;
	A.Projectile.LifeSpan = 8.f;
	A.Projectile.Size = 16.f;
	A.Projectile.Color = A.Color;
	A.Projectile.bComboCore = true;
	A.Projectile.Style = TEXT("PulseCore");
	A.Projectile.ComboDamage = 200.f;
	A.Projectile.ComboRadius = 550.f;
	A.Projectile.ExplosionSound = TEXT("S_Impact");

	// ---- UT2004 XWeapons.ShockRifle (ShockBeamFire, ShockProjFire; ShockAmmo 20/50) ----
	Priority = 0.63f;
	MaxAmmo = 50;
	InitialAmmo = 20;
	AmmoPickupAmount = 10;
	P.Damage = 45.f;
	P.RefireTime = 0.7f;
	P.Range = 34000.f;          // TraceRange 17000
	P.Momentum = 1200.f;        // Momentum 60000
	A.RefireTime = 0.6f;
	A.Projectile.Speed = 2300.f;
	A.Projectile.MaxSpeed = 2300.f;
	A.Projectile.Damage = 45.f;
	A.Projectile.DamageRadius = 300.f;
	A.Projectile.Momentum = 1400.f;
	A.Projectile.LifeSpan = 10.f;
	A.Projectile.Kind = EArenaProjKind::ShockBall;
	A.Projectile.CollisionRadius = 20.f;   // 10 UU, 20 UU after 0.4 s
	A.Projectile.bProjTarget = true;
	A.Projectile.ComboDamage = 200.f;
	A.Projectile.ComboRadius = 550.f;
	A.Projectile.ComboMomentum = 3000.f;   // ComboMomentumTransfer 150000
	A.Projectile.ComboAmmoCost = 3;
	// UE2 ProjSpawnOffset (UU)
	FireModes[1].ProjSpawnOffset = FVector(24.0f, 8.0f, 0.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].BotAimError = 700.f;
	FireModes[1].WarnTargetPct = 0.5f;
}

void AWeap_PulseRifle::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	// The beam detonates energy cores it touches.
	UWorld* World = GetWorld();
	if (World && Mode == 0)
	{
		FVector Start, Dir;
		GetAim(Start, Dir);
		if (ArenaOwner && ArenaOwner->IsPlayerControlled())
		{
			const float Hand = GetHand(); // ShockBeamFire: ShockProjFire's ProjSpawnOffset, Y x Hand
			Start += Dir.Rotation().RotateVector(FVector(48.f, Hand > 1.f ? 0.f : 16.f * Hand, 0.f));
		}
		Dir = AdjustAimForShot(0, Start, Dir); // a bot's combo shot goes at its core
		FHitResult Hit;
		if (WeaponTrace(Start, Start + Dir * FireModes[0].Range, Hit))
		{
			if (AArenaProjectile* Core = Cast<AArenaProjectile>(Hit.GetActor()))
			{
				const int32 ComboCost = Core->GetParams().ComboAmmoCost;
				if (Core->TriggerCombo(GetOwnerController()))
				{
					// ShockProjectile.TakeDamage: the combo costs ComboAmmoCost (3) more shock ammo
					TakeAmmo(0, ComboCost);
					ArenaFX::SpawnBeam(World, GetMuzzleLocation(), Hit.ImpactPoint, FireModes[0].Color, FireModes[0].BeamWidth * 1.3f, 0.45f,
						GetArtBeamStyle(0).IsNone() ? NAME_None : FName(TEXT("Beam.ShockCombo")));
					return;
				}
			}
		}
	}
	Super::FireHitscan(Mode, ChargeAlpha);
}

void AWeap_PulseRifle::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 13.f, -15.f), FRotator::ZeroRotator, Box(34.f, 5.f, 7.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(50.f, 13.f, -13.f), AlongX, Tube(3.5f, 26.f), FLinearColor(0.3f, 0.15f, 0.45f));
	for (int32 i = 0; i < 3; ++i)
	{
		AddViewPart(ArenaAssets::Cylinder(), FVector(42.f + i * 8.f, 13.f, -13.f), AlongX, Tube(6.f, 1.2f), FireModes[0].Color, true);
	}
	AddViewPart(ArenaAssets::Sphere(), FVector(24.f, 13.f, -10.f), FRotator::ZeroRotator, FVector(0.05f), FireModes[1].Color, true);
}

// =================================================================================================
// 5 Arc Linker
// =================================================================================================

AWeap_ArcLinker::AWeap_ArcLinker()
{
	WeaponName = TEXT("Arc Linker");
	Slot = 5;
	MaxAmmo = 120;
	InitialAmmo = 60;
	AmmoPickupAmount = 40;
	Priority = 0.55f;
	WeaponColor = FLinearColor(0.1f, 0.3f, 0.3f);
	MuzzleOffset = FVector(60.f, 14.f, -14.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::LinkAltFire(); // LinkAltFire (the plasma) ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 0.18f;
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(0.5f, 1.f, 0.2f);
	P.FireSound = TEXT("S_Fire_Link");
	P.Projectile.Speed = 2600.f;
	P.Projectile.MaxSpeed = 2600.f;
	P.Projectile.GravityScale = 0.f;
	P.Projectile.Damage = 22.f;
	P.Projectile.DamageRadius = 0.f;
	// XWeapons.LinkProjectile has no MomentumTransfer: plasma does not push
	P.Projectile.Momentum = 0.f;
	P.Projectile.LifeSpan = 3.f;
	P.Projectile.Size = 7.f;
	P.Projectile.Color = P.Color;
	P.Projectile.ExplosionSound = TEXT("S_Impact");
	P.Projectile.Style = TEXT("LinkBolt");

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::LinkFire(); // LinkFire (the beam) ShakeRot / ShakeOffset defaults
	A.FireType = EArenaFireType::Beam;
	A.Damage = 9.f; // per 0.1 s tick
	A.RefireTime = 0.1f;
	A.AmmoPerShot = 1;
	A.Range = 1700.f;
	A.Momentum = 40.f;
	A.Color = FLinearColor(0.35f, 1.f, 0.55f);
	A.FireSound = TEXT("S_Fire_Beam");
	A.BeamWidth = 5.f;

	// ---- UT2004 XWeapons.LinkGun (LinkAltFire plasma, LinkFire beam; LinkAmmo 70/220) ----
	Priority = 0.68f;
	MaxAmmo = 220;
	InitialAmmo = 70;
	AmmoPickupAmount = 50;
	P.RefireTime = 0.2f;
	P.AmmoPerShot = 2;
	P.Projectile.Kind = EArenaProjKind::LinkBolt;
	P.Projectile.Speed = 2000.f;
	P.Projectile.MaxSpeed = 8000.f;
	P.Projectile.Acceleration = 6000.f;
	P.Projectile.Damage = 30.f;
	P.Projectile.DamageRadius = 0.f;
	P.Projectile.Momentum = 0.f;
	P.Projectile.LifeSpan = 3.f;
	P.Projectile.CollisionRadius = 0.1f;   // UE2 0
	A.Damage = 9.f;
	A.RefireTime = 0.12f;
	A.AmmoPerShot = 1;
	A.Range = 2200.f;           // TraceRange 1100 (+250 per link)
	A.Momentum = 40.f;          // MomentumTransfer 2000
	// UE2 ProjSpawnOffset (UU)
	FireModes[0].ProjSpawnOffset = FVector(25.0f, 8.0f, -3.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].WarnTargetPct = 0.1f;
	FireModes[1].WarnTargetPct = 0.2f;
}

void AWeap_ArcLinker::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 14.f, -16.f), FRotator::ZeroRotator, Box(30.f, 8.f, 8.f), WeaponColor);
	AddViewPart(ArenaAssets::Sphere(), FVector(44.f, 14.f, -14.f), FRotator::ZeroRotator, FVector(0.07f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cube(), FVector(54.f, 10.f, -14.f), FRotator(0.f, 6.f, 0.f), Box(14.f, 1.5f, 2.f), FLinearColor(0.6f, 0.6f, 0.62f));
	AddViewPart(ArenaAssets::Cube(), FVector(54.f, 18.f, -14.f), FRotator(0.f, -6.f, 0.f), Box(14.f, 1.5f, 2.f), FLinearColor(0.6f, 0.6f, 0.62f));
	AddViewPart(ArenaAssets::Cylinder(), FVector(36.f, 14.f, -14.f), AlongX, Tube(9.f, 1.5f), FireModes[1].Color, true);
}

int32 AWeap_ArcLinker::GetLinks() const
{
	// LinkFire.AddLink: every team mate beam locked on us adds 1 + its own links
	int32 Links = 0;
	if (!ArenaOwner || !GetWorld())
	{
		return 0;
	}
	for (TActorIterator<AWeap_ArcLinker> It(GetWorld()); It; ++It)
	{
		if (*It != this && It->IsBeamFiring() && It->GetLinkedPawn() == ArenaOwner)
		{
			Links += 1 + FMath::Min(It->GetLinks(), 8);
		}
	}
	return Links;
}

void AWeap_ArcLinker::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	if (Mode != 0)
	{
		Super::FireProjectile(Mode, ChargeAlpha);
		return;
	}
	// LinkProjectile: Damage x (1 + Links), MaxSpeed + 350 UU per link
	FArenaProjectileParams P = FireModes[0].Projectile;
	const int32 Links = GetLinks();
	P.Damage *= float(1 + Links);
	P.MaxSpeed += 700.f * float(Links);
	SpawnProjectiles(P, 1, 0.f);
}

void AWeap_ArcLinker::FireBeamTick(uint8 Mode, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || Mode > 1 || !ArenaOwner)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	const int32 Links = GetLinks();
	// LinkFire.ModeTick: range + 250 UU per link (x1.5 while linked), damage x (1 + 1.5 x Links), berserk x1.33
	float Range = FM.Range + 500.f * float(Links);
	if (LinkedPawn.IsValid())
	{
		Range *= 1.5f;
	}
	float Damage = FM.Damage;
	if (ArenaOwner->GetActiveCombo() == EArenaCombo::Berserk)
	{
		Damage = FMath::FloorToFloat(Damage * 1.33f);
	}
	Damage = FMath::FloorToFloat(Damage * (1.5f * float(Links) + 1.f)) * GetDamageScale();

	FVector Start, Dir;
	GetAim(Start, Dir);
	FHitResult Hit;
	AArenaCharacter* Mate = nullptr;
	if (WeaponTrace(Start, Start + Dir * Range, Hit))
	{
		// a team mate with a link gun takes the link instead of damage
		if (AArenaCharacter* C = Cast<AArenaCharacter>(Hit.GetActor()))
		{
			const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
			if (GM && GM->IsTeamGame() && GM->AreOnSameTeam(ArenaOwner->GetController(), C->GetController()) && Cast<AWeap_ArcLinker>(C->GetCurrentWeapon()))
			{
				Mate = C;
			}
		}
		if (Mate)
		{
			LinkedPawn = Mate;
			LinkBreakTime = 0.5f;
		}
		else
		{
			ProcessHitscanHit(Mode, Hit, Dir, Damage);
			ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 6.f + FM.BeamWidth * 2.f, GetArtImpactStyle(Mode));
		}
	}
	if (!Mate && LinkedPawn.IsValid())
	{
		// LinkBreakDelay 0.5 s
		LinkBreakTime -= FM.RefireTime;
		if (LinkBreakTime <= 0.f)
		{
			LinkedPawn.Reset();
		}
	}
	AddRecoil(0.4f);
}

// =================================================================================================
// 6 Rotary Gun
// =================================================================================================

AWeap_RotaryGun::AWeap_RotaryGun()
{
	WeaponName = TEXT("Rotary Gun");
	Slot = 6;
	MaxAmmo = 300;
	InitialAmmo = 100;
	AmmoPickupAmount = 50;
	Priority = 0.5f;
	WeaponColor = FLinearColor(0.25f, 0.26f, 0.28f);
	MuzzleOffset = FVector(60.f, 14.f, -17.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::AssaultFire(); // MinigunFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 7.f;
	P.RefireTime = 0.075f;
	P.AmmoPerShot = 1;
	P.Range = 12000.f;
	P.Spread = 3.5f;
	P.Momentum = 40.f;
	P.Color = FLinearColor(1.f, 0.9f, 0.7f);
	P.FireSound = TEXT("S_Fire_Chaingun");
	P.BeamWidth = 1.f;

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::AssaultFire(); // MinigunAltFire extends MinigunFire ShakeRot / ShakeOffset defaults
	A = P;
	A.Damage = 14.f;
	A.RefireTime = 0.18f;
	A.Spread = 0.6f;
	A.Momentum = 90.f;
	A.BeamWidth = 1.4f;
	A.Color = FLinearColor(1.f, 0.75f, 0.4f);

	// ---- UT2004 XWeapons.Minigun (MinigunFire 15 rounds/s, MinigunAltFire 5/s; MinigunAmmo 150/300) ----
	Priority = 0.71f;
	MaxAmmo = 300;
	InitialAmmo = 150;
	AmmoPickupAmount = 50;
	P.Damage = 7.f;
	P.DamageMax = 8.f;
	P.RefireTime = 1.f / 15.f;  // 1 / (RoundsPerRotation 5 x BarrelRotationsPerSec 3)
	P.Range = 20000.f;
	P.Spread = 0.f;
	P.UE2Spread = 0.08f;
	P.Momentum = 0.f;
	P.PreFireTime = 0.27f;      // WindUpTime
	A.Damage = 14.f;
	A.DamageMax = 16.f;
	A.RefireTime = 0.2f;
	A.Range = 20000.f;
	A.Spread = 0.f;
	A.UE2Spread = 0.03f;
	A.Momentum = 0.f;
	A.PreFireTime = 0.15f;
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].BotAimError = 900.f;
	FireModes[1].BotAimError = 900.f;
}

void AWeap_RotaryGun::BuildViewModel()
{
	const FLinearColor Brass(0.75f, 0.55f, 0.25f);
	AddViewPart(ArenaAssets::Cube(), FVector(22.f, 14.f, -18.f), FRotator::ZeroRotator, Box(24.f, 10.f, 10.f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(20.f, 14.f, -11.f), FRotator::ZeroRotator, Box(12.f, 3.f, 4.f), Brass);

	Barrels = NewObject<USceneComponent>(this, TEXT("Barrels"));
	Barrels->SetupAttachment(ViewRoot);
	Barrels->SetRelativeLocation(FVector(36.f, 14.f, -17.f));
	Barrels->RegisterComponent();
	AddInstanceComponent(Barrels);

	const float R = 3.5f;
	for (int32 i = 0; i < 6; ++i)
	{
		const float Ang = i * (2.f * UE_PI / 6.f);
		if (UStaticMeshComponent* B = AddViewPart(ArenaAssets::Cylinder(), FVector(12.f, R * FMath::Cos(Ang), R * FMath::Sin(Ang)), AlongX, Tube(2.2f, 28.f), FLinearColor(0.15f, 0.15f, 0.16f)))
		{
			B->AttachToComponent(Barrels, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}
	for (int32 i = 0; i < 2; ++i)
	{
		if (UStaticMeshComponent* Ring = AddViewPart(ArenaAssets::Cylinder(), FVector(4.f + i * 18.f, 0.f, 0.f), AlongX, Tube(10.f, 1.5f), Brass))
		{
			Ring->AttachToComponent(Barrels, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}
}

void AWeap_RotaryGun::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const bool bSpinning = bEquipped && (bFireHeld[0] || bFireHeld[1]) && HasAmmo();
	const float Target = bSpinning ? (bFireHeld[0] ? 1500.f : 800.f) : 0.f;
	SpinSpeed = FMath::FInterpTo(SpinSpeed, Target, DeltaSeconds, bSpinning ? 6.f : 2.f);
	if (Barrels && SpinSpeed > 1.f)
	{
		Barrels->AddLocalRotation(FRotator(0.f, 0.f, SpinSpeed * DeltaSeconds));
	}
}

void AWeap_RotaryGun::StartFire(uint8 Mode)
{
	// MinigunFire wind-up: the barrels must spin up (WindUpTime) unless they are still turning from the other mode
	if (Mode < 2 && SpinSpeed > 600.f)
	{
		NextFireTime = FMath::Min(NextFireTime, GetWorldTime());
	}
	Super::StartFire(Mode);
}

// =================================================================================================
// 7 Shard Cannon
// =================================================================================================

AWeap_ShardCannon::AWeap_ShardCannon()
{
	WeaponName = TEXT("Shard Cannon");
	Slot = 7;
	MaxAmmo = 40;
	InitialAmmo = 15;
	AmmoPickupAmount = 10;
	Priority = 0.65f;
	WeaponColor = FLinearColor(0.55f, 0.3f, 0.18f);
	MuzzleOffset = FVector(56.f, 14.f, -17.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::KickBack(); // FlakFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 0.9f;
	P.AmmoPerShot = 1;
	P.NumShots = 9;
	P.Spread = 7.f;
	P.Color = FLinearColor(1.f, 0.6f, 0.2f);
	P.FireSound = TEXT("S_Fire_Shard");
	P.Projectile.Speed = 3800.f;
	P.Projectile.MaxSpeed = 3800.f;
	P.Projectile.GravityScale = 0.15f;
	P.Projectile.Damage = 13.f;
	P.Projectile.DamageRadius = 0.f;
	P.Projectile.Momentum = 90.f;
	P.Projectile.Bounces = 2;
	P.Projectile.Bounciness = 0.7f;
	P.Projectile.LifeSpan = 1.8f;
	P.Projectile.Size = 4.f;
	P.Projectile.Color = P.Color;
	P.Projectile.bShard = true;
	P.Projectile.Style = TEXT("FlakChunk");
	P.Projectile.ExplosionSound = TEXT("S_Impact");

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::KickBack(); // FlakAltFire ShakeRot / ShakeOffset defaults
	A.FireType = EArenaFireType::Projectile;
	A.RefireTime = 1.0f;
	A.AmmoPerShot = 1;
	A.NumShots = 1;
	A.Color = FLinearColor(1.f, 0.4f, 0.1f);
	A.FireSound = TEXT("S_Fire_Shard");
	A.Projectile.Speed = 1800.f;
	A.Projectile.MaxSpeed = 1800.f;
	A.Projectile.GravityScale = 1.f;
	// XWeapons.FlakShell: Damage 90, radius 220, MomentumTransfer 75000
	A.Projectile.Damage = 90.f;
	A.Projectile.DamageRadius = 440.f;
	A.Projectile.Momentum = 1500.f;
	A.Projectile.Bounces = 0;
	A.Projectile.LifeSpan = 3.f;
	A.Projectile.Size = 11.f;
	A.Projectile.Color = A.Color;
	A.Projectile.ExplosionSound = TEXT("S_Explosion");
	A.Projectile.Style = TEXT("FlakShell");

	// ---- UT2004 XWeapons.FlakCannon (FlakFire 9 FlakChunks, FlakAltFire FlakShell; FlakAmmo 15/35) ----
	Priority = 0.75f;
	MaxAmmo = 35;
	InitialAmmo = 15;
	AmmoPickupAmount = 10;
	P.RefireTime = 0.8947f;
	P.NumShots = 9;
	P.Spread = 0.f;
	P.SpreadStyle = 1;
	P.UE2Spread = 1400.f;
	P.Projectile.Kind = EArenaProjKind::FlakChunk;
	P.Projectile.Speed = 5000.f;
	P.Projectile.MaxSpeed = 5400.f;
	P.Projectile.GravityScale = 0.f;
	P.Projectile.Damage = 13.f;
	P.Projectile.DamageRadius = 0.f;
	P.Projectile.Momentum = 200.f;
	P.Projectile.LifeSpan = 2.7f;
	P.Projectile.CollisionRadius = 0.1f;   // UE2 0
	P.Projectile.bShard = false;
	A.RefireTime = 1.11f;
	A.Projectile.Kind = EArenaProjKind::FlakShell;
	A.Projectile.Speed = 2400.f;
	A.Projectile.MaxSpeed = 4000.f;
	A.Projectile.TossZ = 450.f;
	A.Projectile.GravityScale = ArenaTuning::GravityScale;
	A.Projectile.Damage = 90.f;
	A.Projectile.DamageRadius = 440.f;
	A.Projectile.Momentum = 1500.f;
	A.Projectile.LifeSpan = 6.f;
	A.Projectile.CollisionRadius = 0.1f;   // UE2 0
	// UE2 ProjSpawnOffset (UU)
	FireModes[0].ProjSpawnOffset = FVector(25.0f, 5.0f, -6.0f);
	FireModes[1].ProjSpawnOffset = FVector(25.0f, 9.0f, -12.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].WarnTargetPct = 0.5f;
	FireModes[1].WarnTargetPct = 0.9f;
	FireModes[1].bTossed = true;
	FireModes[1].bRecommendSplash = true;
}

void AWeap_ShardCannon::BuildViewModel()
{
	const FLinearColor Dark(0.14f, 0.12f, 0.11f);
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 14.f, -17.f), FRotator::ZeroRotator, Box(30.f, 12.f, 10.f), WeaponColor);
	for (int32 i = 0; i < 4; ++i)
	{
		const float Y = (i % 2 == 0) ? -2.6f : 2.6f;
		const float Z = (i < 2) ? -2.6f : 2.6f;
		AddViewPart(ArenaAssets::Cylinder(), FVector(48.f, 14.f + Y, -17.f + Z), AlongX, Tube(3.6f, 12.f), Dark);
	}
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 20.2f, -17.f), FRotator::ZeroRotator, Box(18.f, 0.6f, 4.f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 7.8f, -17.f), FRotator::ZeroRotator, Box(18.f, 0.6f, 4.f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cube(), FVector(24.f, 14.f, -10.5f), FRotator::ZeroRotator, Box(14.f, 6.f, 3.f), Dark);
}

void AWeap_ShardCannon::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	const FArenaFireMode& FM = FireModes[Mode];
	FArenaProjectileParams Chunk = FireModes[0].Projectile;
	Chunk.WeaponName = WeaponName;
	// FlakFire: 9 chunks, SS_Random 1400; FlakAltFire: one shell that throws 6 chunks when it explodes
	SpawnProjectiles(FM.Projectile, FM.NumShots, 0.f, FM.SpreadStyle, FM.UE2Spread, Mode == 1 ? &Chunk : nullptr);
}

// =================================================================================================
// 8 Rocket Launcher
// =================================================================================================

AWeap_RocketLauncher::AWeap_RocketLauncher()
{
	WeaponName = TEXT("Rocket Launcher");
	Slot = 8;
	MaxAmmo = 30;
	InitialAmmo = 12;
	AmmoPickupAmount = 9;
	Priority = 0.8f;
	WeaponColor = FLinearColor(0.4f, 0.08f, 0.06f);
	MuzzleOffset = FVector(60.f, 14.f, -15.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::KickBack(); // RocketFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 0.9f;
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(1.f, 0.3f, 0.1f);
	P.FireSound = TEXT("S_Fire_Rocket");
	P.Projectile.Speed = 2200.f;
	P.Projectile.MaxSpeed = 2200.f;
	P.Projectile.GravityScale = 0.f;
	// XWeapons.RocketProj: Damage 90, DamageRadius 220, MomentumTransfer 50000
	P.Projectile.Damage = 90.f;
	P.Projectile.DamageRadius = 440.f;
	P.Projectile.Momentum = 1000.f;
	P.Projectile.Bounces = 0;
	P.Projectile.LifeSpan = 8.f;
	P.Projectile.Size = 9.f;
	P.Projectile.Color = P.Color;
	P.Projectile.ExplosionSound = TEXT("S_Explosion");
	P.Projectile.Style = TEXT("Rocket");

	FArenaFireMode& A = FireModes[1];
	A.Shake = ArenaViewShake::KickBack(); // RocketMultiFire ShakeRot / ShakeOffset defaults
	A.FireType = EArenaFireType::Projectile;
	A.RefireTime = 0.65f;
	A.AmmoPerShot = 1;
	A.Color = FLinearColor(1.f, 0.5f, 0.1f);
	A.FireSound = TEXT("S_Fire_Rocket");
	A.Projectile.Speed = 1500.f;
	A.Projectile.MaxSpeed = 1500.f;
	A.Projectile.GravityScale = 1.2f;
	A.Projectile.Damage = 85.f;
	A.Projectile.DamageRadius = 300.f;
	A.Projectile.Momentum = 900.f;
	A.Projectile.Bounces = -1;
	A.Projectile.Style = TEXT("Grenade");
	A.Projectile.Bounciness = 0.55f;
	A.Projectile.LifeSpan = 2.f;
	A.Projectile.Size = 9.f;
	A.Projectile.Color = A.Color;
	A.Projectile.ExplosionSound = TEXT("S_Explosion");

	// ---- UT2004 XWeapons.RocketLauncher (RocketFire, RocketMultiFire; RocketAmmo 12/30; RocketProj) ----
	Priority = 0.78f;
	MaxAmmo = 30;
	InitialAmmo = 12;
	AmmoPickupAmount = 9;
	P.RefireTime = 0.9f;
	P.Projectile.Kind = EArenaProjKind::Rocket;
	P.Projectile.Speed = 2700.f;
	P.Projectile.MaxSpeed = 2700.f;
	P.Projectile.GravityScale = 0.f;
	P.Projectile.Damage = 90.f;
	P.Projectile.DamageRadius = 440.f;
	P.Projectile.Momentum = 1000.f;
	P.Projectile.LifeSpan = 8.f;
	P.Projectile.CollisionRadius = 0.1f;   // UE2 0
	A = P;
	A.Color = FLinearColor(1.f, 0.5f, 0.1f);
	A.RefireTime = 0.95f;
	A.AmmoPerShot = 0;          // the loaded rockets are paid when they are fired
	A.MaxChargeTime = 2.3f;     // MaxHoldTime
	A.SpreadStyle = 2;
	A.UE2Spread = 1000.f;       // LooseSpread (TightSpread 300: spiralling flock)
	// UE2 ProjSpawnOffset (UU)
	FireModes[0].ProjSpawnOffset = FVector(25.0f, 6.0f, -6.0f);
	FireModes[1].ProjSpawnOffset = FVector(25.0f, 6.0f, -6.0f);
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].WarnTargetPct = 0.9f;
	FireModes[0].bRecommendSplash = true;
	FireModes[1].WarnTargetPct = 0.9f;
	FireModes[1].bRecommendSplash = true;
}

void AWeap_RocketLauncher::BuildViewModel()
{
	const FLinearColor Grey(0.3f, 0.3f, 0.32f);
	AddViewPart(ArenaAssets::Cylinder(), FVector(32.f, 14.f, -15.f), AlongX, Tube(12.f, 50.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(57.f, 14.f, -15.f), AlongX, Tube(13.5f, 2.f), Grey);
	AddViewPart(ArenaAssets::Cylinder(), FVector(8.f, 14.f, -15.f), AlongX, Tube(13.5f, 2.f), Grey);
	AddViewPart(ArenaAssets::Sphere(), FVector(55.f, 14.f, -15.f), FRotator::ZeroRotator, FVector(0.09f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cube(), FVector(30.f, 14.f, -7.f), FRotator::ZeroRotator, Box(6.f, 2.f, 4.f), Grey);
	AddViewPart(ArenaAssets::Cube(), FVector(26.f, 14.f, -23.f), FRotator(-10.f, 0.f, 0.f), Box(5.f, 4.f, 10.f), Grey);
}

void AWeap_RocketLauncher::StartFire(uint8 Mode)
{
	if (Mode == 0 && bFireHeld[1] && Load > 0)
	{
		// RocketLauncher.ClientStartFire: primary while loading = tight spread (spiralling flock)
		bTightSpread = true;
		return;
	}
	// RocketLauncher.StartFire: not while the other mode is firing
	if (Mode < 2 && bFireHeld[1 - Mode])
	{
		return;
	}
	const bool bWasHeld = Mode < 2 && bFireHeld[Mode];
	Super::StartFire(Mode);
	if (Mode == 1 && !bWasHeld && bFireHeld[1])
	{
		// RocketMultiFire: load starts at 1 rocket
		Load = Ammo > 0 ? 1 : 0;
		LoadStart = FMath::Max(GetWorldTime(), NextFireTime);
		bTightSpread = false;
	}
}

void AWeap_RocketLauncher::StopFire(uint8 Mode)
{
	Super::StopFire(Mode);
	if (Mode == 1)
	{
		Load = 0;
	}
}

void AWeap_RocketLauncher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bEquipped || !ArenaOwner || ArenaOwner->IsDead())
	{
		bLockedOn = false;
		SeekTarget.Reset();
		return;
	}
	const float Now = GetWorldTime();
	if (bFireHeld[1] && Load > 0)
	{
		// RocketMultiFire.ModeTick: a 2nd rocket after FireRate (0.95 s), a 3rd after 2 x FireRate; fires itself at
		// MaxHoldTime (2.3 s) or when no more ammo can be loaded
		const float Hold = Now - LoadStart;
		const float Rate = FireModes[1].RefireTime * GetRefireScale();
		if (Load == 1 && Hold >= Rate && Ammo >= 2)
		{
			Load = 2;
		}
		else if (Load == 2 && Hold >= 2.f * Rate && Ammo >= 3)
		{
			Load = 3;
		}
		if (Hold >= FireModes[1].MaxChargeTime || Load >= Ammo)
		{
			StopFire(1);
		}
	}

	// RocketLauncher.Tick lock-on (human players): every SeekCheckFreq 0.5 s pick the enemy within LockAim
	// (cos 0.996) and SeekRange 8000 UU; locked after LockRequiredTime 1.25 s, lost after UnLockRequiredTime 0.5 s
	if (!ArenaOwner->IsPlayerControlled() || Now < NextSeekCheck)
	{
		return;
	}
	NextSeekCheck = Now + 0.5f;
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	AArenaCharacter* Best = nullptr;
	float BestAim = 0.996f;
	const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (C == ArenaOwner || C->IsDead() || (GM && GM->IsTeamGame() && GM->AreOnSameTeam(ArenaOwner->GetController(), C->GetController())))
		{
			continue;
		}
		const FVector To = C->GetActorLocation() - Eye;
		const float Dist = To.Size();
		const float Aim = FVector::DotProduct(To / FMath::Max(Dist, 1.f), Dir);
		if (Dist > 16000.f || Aim < BestAim)
		{
			continue;
		}
		FHitResult Hit;
		if (WeaponTrace(Eye, C->GetActorLocation(), Hit) && Hit.GetActor() != C)
		{
			continue;
		}
		Best = C;
		BestAim = Aim;
	}
	if (Best)
	{
		if (Best == SeekTarget.Get())
		{
			LockTime += 0.5f;
			if (!bLockedOn && LockTime >= 1.25f)
			{
				bLockedOn = true;
			}
		}
		else
		{
			SeekTarget = Best;
			LockTime = 0.f;
		}
		UnLockTime = 0.f;
	}
	else if (SeekTarget.IsValid())
	{
		UnLockTime += 0.5f;
		if (UnLockTime >= 0.5f)
		{
			SeekTarget.Reset();
			bLockedOn = false;
		}
	}
	else
	{
		bLockedOn = false;
	}
}

void AWeap_RocketLauncher::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	FArenaProjectileParams P = FireModes[0].Projectile;
	AArenaBotController* Bot = ArenaOwner ? Cast<AArenaBotController>(ArenaOwner->GetController()) : nullptr;
	if (Bot && Bot->GetEnemy())
	{
		// RocketLauncher.SpawnProjectile: a bot locks on when skill > 2 + 5 x FRand, 60% of the time, its enemy is over
		// 2000-4000 UU away, seen within 0.4 s and acquired over 1.5 s ago
		const float Now = GetWorldTime();
		const float Dist = FVector::Dist(Bot->GetEnemy()->GetActorLocation(), ArenaOwner->GetActorLocation());
		if (Bot->GetSkill() > 2.f + 5.f * FMath::FRand() && FMath::FRand() < 0.6f && Dist > 4000.f + 4000.f * FMath::FRand()
			&& Now - Bot->GetLastSeenEnemyTime() < 0.4f && Now - Bot->GetAcquireTime() > 1.5f)
		{
			bLockedOn = true;
			SeekTarget = Bot->GetEnemy();
			Bot->NumLockedRockets++;
		}
	}
	if (bLockedOn && SeekTarget.IsValid())
	{
		// SeekingRocketProj (LifeSpan 10); RocketLauncher.SpawnProjectile breaks the lock
		P.SeekTarget = SeekTarget;
		P.LifeSpan = 10.f;
		bLockedOn = false;
		SeekTarget.Reset();
		LockTime = 0.f;
	}
	if (Mode == 0)
	{
		SpawnProjectiles(P, 1, 0.f);
		return;
	}
	if (Load <= 0 || Ammo <= 0)
	{
		return;
	}
	const int32 N = FMath::Clamp(Load, 1, Ammo);
	Ammo = FMath::Max(0, Ammo - N);
	if (bTightSpread && N >= 2)
	{
		// RocketMultiFire.DoFireEffect (tight): rockets from a small circle around the muzzle, flocking with alternating curl
		TArray<AArenaProjectile*> Rockets;
		for (int32 i = 0; i < N; ++i)
		{
			TArray<AArenaProjectile*> One = SpawnProjectiles(P, 1, 0.f);
			if (One.Num() > 0)
			{
				const FRotator Aim = One[0]->GetActorRotation();
				const float A = float(i * 2) * UE_PI / 3.f;
				const FVector Y = FRotationMatrix(Aim).GetUnitAxis(EAxis::Y);
				const FVector Z = FRotationMatrix(Aim).GetUnitAxis(EAxis::Z);
				const FVector X = Aim.Vector();
				const FVector Off = -2.f * 2.f * ((FMath::Sin(A) * 8.f - 7.f) * Y - (FMath::Cos(A) * 8.f - 7.f) * Z) - X * 16.f * FMath::FRand();
				One[0]->SetActorLocation(One[0]->GetActorLocation() + Off);
				Rockets.Add(One[0]);
			}
		}
		bool bCurlOn = false;
		for (AArenaProjectile* R : Rockets)
		{
			R->SetFlock(Rockets, bCurlOn);
			bCurlOn = !bCurlOn;
		}
	}
	else
	{
		// SS_Line: LooseSpread 1000 units between rockets
		SpawnProjectiles(P, N, 0.f, N > 1 ? 2 : 0, FireModes[1].UE2Spread);
	}
	Load = 0;
	bTightSpread = false;
}

// =================================================================================================
// 9 Rail Rifle
// =================================================================================================

AWeap_RailRifle::AWeap_RailRifle()
{
	WeaponName = TEXT("Rail Rifle");
	Slot = 9;
	MaxAmmo = 40;
	InitialAmmo = 15;
	AmmoPickupAmount = 10;
	Priority = 0.7f;
	WeaponColor = FLinearColor(0.12f, 0.18f, 0.3f);
	MuzzleOffset = FVector(68.f, 13.f, -13.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::SniperFire(); // SniperFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 70.f;
	P.RefireTime = 1.3f;
	P.AmmoPerShot = 1;
	P.Range = 30000.f;
	P.Momentum = 500.f;
	P.HeadshotMultiplier = 2.f;
	P.Color = FLinearColor(0.2f, 0.75f, 1.f);
	P.FireSound = TEXT("S_Fire_Rail");
	P.BeamWidth = 4.f;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Zoom;
	A.Damage = 0.f;
	A.AmmoPerShot = 0;
	A.RefireTime = 0.f;
	A.Color = P.Color;

	// ---- UT2004 XWeapons.SniperRifle (lightning gun: SniperFire, SniperZoom; SniperAmmo 15/40) ----
	Priority = 0.69f;
	MaxAmmo = 40;
	InitialAmmo = 15;
	AmmoPickupAmount = 10;
	BringUpTime = 0.36f;
	P.Damage = 70.f;
	P.RefireTime = 1.6f;
	P.Range = 34000.f;
	P.Momentum = 0.f;           // Momentum 1
	P.HeadshotMultiplier = 2.f;
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].BotAimError = 850.f;
	FireModes[0].WarnTargetPct = 0.5f;
}

float AWeap_RailRifle::GetZoomFOV() const
{
	// PlayerController zoom (SniperRifle: ToggleZoom on alt press, StopZoom on release): ZoomLevel grows 1 per
	// second up to 0.9 while held, FOV = 90 - 88 x ZoomLevel; the next alt press zooms back out.
	if (!bEquipped || ZoomLevel <= 0.f)
	{
		return 0.f;
	}
	return FMath::Clamp(90.f - 88.f * ZoomLevel, 1.f, 170.f);
}

void AWeap_RailRifle::StartFire(uint8 Mode)
{
	if (Mode == 1)
	{
		if (ZoomLevel > 0.f)
		{
			ZoomLevel = 0.f;       // EndZoom
			bZoomingIn = false;
		}
		else
		{
			bZoomingIn = true;     // StartZoomWithMax(0.9)
		}
	}
	Super::StartFire(Mode);
}

void AWeap_RailRifle::StopFire(uint8 Mode)
{
	if (Mode == 1)
	{
		bZoomingIn = false;        // StopZoom
	}
	Super::StopFire(Mode);
}

void AWeap_RailRifle::Unequip()
{
	ZoomLevel = 0.f;
	bZoomingIn = false;
	Super::Unequip();
}

void AWeap_RailRifle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bZoomingIn && bEquipped)
	{
		ZoomLevel = FMath::Min(ZoomLevel + DeltaSeconds, 0.9f);
	}
}

void AWeap_RailRifle::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(22.f, 13.f, -16.f), FRotator::ZeroRotator, Box(24.f, 5.f, 8.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(42.f, 13.f, -13.f), AlongX, Tube(3.f, 52.f), FLinearColor(0.55f, 0.6f, 0.65f));
	AddViewPart(ArenaAssets::Cylinder(), FVector(24.f, 13.f, -8.f), AlongX, Tube(4.f, 16.f), FLinearColor(0.08f, 0.08f, 0.1f));
	AddViewPart(ArenaAssets::Cube(), FVector(42.f, 11.f, -13.f), FRotator::ZeroRotator, Box(40.f, 0.6f, 0.6f), FireModes[0].Color, true);
	AddViewPart(ArenaAssets::Cube(), FVector(42.f, 15.f, -13.f), FRotator::ZeroRotator, Box(40.f, 0.6f, 0.6f), FireModes[0].Color, true);
}

void AWeap_RailRifle::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || Mode != 0 || !ArenaOwner)
	{
		Super::FireHitscan(Mode, ChargeAlpha);
		return;
	}
	// SniperFire.DoTrace: the main bolt (headshots x2), then NumArcs 3 arcs of SecTraceDist 300 UU in random
	// directions from the hit point; pawns they hit (not the main target) take SecDamageMult 0.5.
	const FArenaFireMode& FM = FireModes[0];
	FVector Start, Dir;
	GetAim(Start, Dir);
	const FVector End = Start + Dir * FM.Range;
	FHitResult Hit;
	const bool bHit = WeaponTrace(Start, End, Hit);
	const FVector HitPoint = bHit ? FVector(Hit.ImpactPoint) : End;
	const float Damage = FM.Damage * GetDamageScale();
	AActor* MainTarget = nullptr;
	if (bHit)
	{
		ProcessHitscanHit(0, Hit, Dir, Damage);
		ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 8.f + FM.BeamWidth * 3.f, GetArtImpactStyle(0));
		if (!Hit.GetActor() || !Hit.GetActor()->IsRootComponentStatic())
		{
			MainTarget = Hit.GetActor();
		}
	}
	ArenaFX::SpawnBeam(World, GetMuzzleLocation(), HitPoint, FM.Color, FM.BeamWidth, 0.3f, GetArtBeamStyle(0));
	const FVector ArcStart = bHit ? FVector(Hit.ImpactPoint + Hit.ImpactNormal * 4.f) : End;
	for (int32 i = 0; i < 3; ++i)
	{
		const FVector ArcEnd = ArcStart + FMath::VRand() * 600.f;
		FHitResult ArcHit;
		const bool bArcHit = WeaponTrace(ArcStart, ArcEnd, ArcHit);
		if (bArcHit && ArcHit.GetActor() && ArcHit.GetActor() != MainTarget && ArcHit.GetActor() != ArenaOwner && Cast<APawn>(ArcHit.GetActor()))
		{
			FArenaHitInfo Info;
			Info.WeaponName = WeaponName;
			Info.HitLocation = ArcHit.ImpactPoint;
			ArenaDamage::ApplyDamage(ArcHit.GetActor(), FMath::FloorToFloat(Damage * 0.5f), GetOwnerController(), this, Info);
		}
		ArenaFX::SpawnBeam(World, ArcStart, bArcHit ? FVector(ArcHit.ImpactPoint) : ArcEnd, FM.Color, FM.BeamWidth * 0.4f, 0.2f, GetArtBeamStyle(0));
	}
}

// =================================================================================================
// 0 Insta Rifle (instagib)
// =================================================================================================

AWeap_InstaRifle::AWeap_InstaRifle()
{
	WeaponName = TEXT("Insta Rifle");
	Slot = 0;
	MaxAmmo = 1;
	InitialAmmo = 1;
	AmmoPickupAmount = 0;
	bInfiniteAmmo = true;
	Priority = 1.f;
	WeaponColor = FLinearColor(0.9f, 0.9f, 0.95f);
	MuzzleOffset = FVector(66.f, 13.f, -13.f);

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::ShockBeamFire(); // SuperShockBeamFire extends ShockBeamFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 1000.f;
	P.RefireTime = 1.1f;
	P.AmmoPerShot = 0;
	P.Range = 30000.f;
	P.Momentum = 1000.f;
	P.HeadshotMultiplier = 0.f;
	P.Color = FLinearColor(1.f, 0.2f, 0.6f);
	P.FireSound = TEXT("S_Fire_Rail");
	P.BeamWidth = 7.f;

	FireModes[1] = P;

	// ---- UT2004 XWeapons.SuperShockRifle (instagib) ----
	P.Damage = 1000.f;
	P.RefireTime = 1.1f;
	P.Range = 34000.f;
	P.Momentum = 2000.f;        // Momentum 100000
	P.bReflective = true;       // SuperShockBeamFire inherits ShockBeamFire.bReflective
	FireModes[0].bReflective = true;
	FireModes[1] = P;
	// UE2 bot aim error / warning (WeaponFire AimError, WarnTargetPct, bTossed, bRecommendSplashDamage)
	FireModes[0].BotAimError = 700.f;
	FireModes[1].BotAimError = 700.f;
}

void AWeap_InstaRifle::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(24.f, 13.f, -16.f), FRotator::ZeroRotator, Box(26.f, 6.f, 8.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(44.f, 13.f, -13.f), AlongX, Tube(3.5f, 44.f), FireModes[0].Color, true);
	for (int32 i = 0; i < 4; ++i)
	{
		AddViewPart(ArenaAssets::Cylinder(), FVector(32.f + i * 9.f, 13.f, -13.f), AlongX, Tube(6.5f, 1.2f), FLinearColor(0.85f, 0.85f, 0.9f));
	}
	AddViewPart(ArenaAssets::Sphere(), FVector(20.f, 13.f, -10.f), FRotator::ZeroRotator, FVector(0.05f), FireModes[0].Color, true);
}

// =================================================================================================

TArray<TSubclassOf<AArenaWeapon>> ArenaWeaponList::AllWeapons()
{
	TArray<TSubclassOf<AArenaWeapon>> List;
	List.Add(AWeap_Gauntlet::StaticClass());
	List.Add(AWeap_Carbine::StaticClass());
	List.Add(AWeap_GooLauncher::StaticClass());
	List.Add(AWeap_PulseRifle::StaticClass());
	List.Add(AWeap_ArcLinker::StaticClass());
	List.Add(AWeap_RotaryGun::StaticClass());
	List.Add(AWeap_ShardCannon::StaticClass());
	List.Add(AWeap_RocketLauncher::StaticClass());
	List.Add(AWeap_RailRifle::StaticClass());
	return List;
}

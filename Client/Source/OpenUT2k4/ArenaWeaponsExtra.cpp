#include "ArenaWeaponsExtra.h"
#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaBotController.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaFX.h"
#include "ArenaGameMode.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaWeaponActors.h"
#include "ArenaArtFX.h"
#include "ArenaUE2Effects.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Materials/MaterialInstanceDynamic.h"

using ArenaWeaponActors::UU;

namespace ArenaWeaponsExtraLocal
{
	const FRotator AlongX(90.f, 0.f, 0.f);
	FVector Box(float X, float Y, float Z) { return FVector(X, Y, Z) / 100.f; }
	FVector Tube(float Diameter, float Length) { return FVector(Diameter / 100.f, Diameter / 100.f, Length / 100.f); }

	/** A glowing beam mesh (painter / laser) on the weapon's holder (the weapon actor follows the first-person view and
	 * may be hidden), placed every tick in world space. */
	UStaticMeshComponent* MakeBeam(AArenaWeapon* W, const FLinearColor& Color, const TCHAR* UE2EffectClass = nullptr)
	{
		return ArenaWeaponBeam::Make(W->GetArenaOwner() ? static_cast<AActor*>(W->GetArenaOwner()) : static_cast<AActor*>(W), Color, UE2EffectClass);
	}

	void PlaceBeam(UStaticMeshComponent* Beam, const FVector& Start, const FVector& End, float MeshWidth)
	{
		ArenaWeaponBeam::Place(Beam, Start, End, MeshWidth);
	}

	void HideBeam(UStaticMeshComponent* Beam)
	{
		ArenaWeaponBeam::Hide(Beam);
	}

	AArenaBotController* BotOf(const AArenaCharacter* C)
	{
		return C ? Cast<AArenaBotController>(C->GetController()) : nullptr;
	}
}
using namespace ArenaWeaponsExtraLocal;

UStaticMeshComponent* ArenaWeaponBeam::Make(AActor* Holder, const FLinearColor& Color, const TCHAR* UE2EffectClass)
{
	// the stock beam effect's texture and width (XWeapons.PainterBeamEffect xEmitter skin and mSizeRange,
	// Onslaught.ONSMineLayerTargetBeamEffect's BeamEmitter Texture / StartSizeRange) as a camera-facing beam quad
	FArenaUE2ActorDesc D;
	if (UE2EffectClass && ArenaUE2Effects::GetClassDesc(UE2EffectClass, D))
	{
		FString Tex = D.Skins.Num() > 0 ? D.Skins[0] : FString();
		float Width = float(D.Props.Num(TEXT("mSizeRange"), 0.0));
		FColor Tint = D.Props.Color(TEXT("mColorRange"), FColor::White);
		for (const TSharedPtr<FJsonValue>& V : D.Props.Array(TEXT("Emitters")))
		{
			FString Cls;
			TArray<FString> Chain;
			FArenaUE2Props P;
			if (D.GetSubobject(FArenaUE2Props::ToString(V), Cls, Chain, P))
			{
				Tex = P.Str(TEXT("Texture"));
				FVector Mn, Mx;
				P.RangeVector(TEXT("StartSizeRange"), Mn, Mx, FVector(4.0));
				Width = float(Mx.X + Mn.X) * 0.5f;
				Tint = FColor::White;
				break;
			}
		}
		const FString Path = ArenaUE2Effects::SpriteTexturePath(Tex);
		if (!Path.IsEmpty() && Width > 0.f)
		{
			ArenaArtFX::FSpriteDesc SD;
			SD.Texture = *Path;
			SD.Blend = ArenaArtFX::EBlend::Additive;
			SD.Facing = ArenaArtFX::EFacing::Beam;
			SD.Size = FVector2D(100.f, Width * 2.f * UU);
			SD.Tint = FLinearColor(Tint.R / 255.f, Tint.G / 255.f, Tint.B / 255.f) * 2.f;
			SD.FadePower = 0.f;
			SD.bRandomRoll = false;
			UArenaSpriteComponent* SC = NewObject<UArenaSpriteComponent>(Holder);
			SC->SetUsingAbsoluteLocation(true);
			SC->SetUsingAbsoluteRotation(true);
			SC->SetUsingAbsoluteScale(true);
			SC->SetupAttachment(Holder->GetRootComponent());
			if (SC->SetupSprite(SD))
			{
				SC->RegisterComponent();
				SC->SetVisibility(false);
				return SC;
			}
			SC->DestroyComponent();
		}
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Holder);
	C->SetStaticMesh(ArenaAssets::Cylinder());
	C->SetUsingAbsoluteLocation(true);
	C->SetUsingAbsoluteRotation(true);
	C->SetUsingAbsoluteScale(true);
	C->SetupAttachment(Holder->GetRootComponent());
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(false);
	if (UMaterialInstanceDynamic* M = ArenaAssets::MakeGlow(Holder, Color, 10.f, 1.f))
	{
		C->SetMaterial(0, M);
	}
	C->RegisterComponent();
	C->SetVisibility(false);
	return C;
}

void ArenaWeaponBeam::Place(UStaticMeshComponent* Beam, const FVector& Start, const FVector& End, float MeshWidth)
{
	if (UArenaSpriteComponent* S = Cast<UArenaSpriteComponent>(Beam))
	{
		S->SetBeam(Start, End);
		S->SetComponentTickEnabled(true);
	}
	else
	{
		ArenaFX::OrientBeamMesh(Beam, Start, End, MeshWidth);
	}
}

void ArenaWeaponBeam::Hide(UStaticMeshComponent* Beam)
{
	Beam->SetVisibility(false);
	Beam->SetComponentTickEnabled(false);
}

// =================================================================================================
// Warhead Launcher (Redeemer)
// =================================================================================================

AWeap_WarheadLauncher::AWeap_WarheadLauncher()
{
	WeaponName = TEXT("Warhead Launcher");
	Slot = 0;                     // InventoryGroup 0
	MaxAmmo = 1;                  // RedeemerAmmo
	InitialAmmo = 1;
	AmmoPickupAmount = 0;
	Priority = 0.99f;
	WeaponColor = FLinearColor(0.45f, 0.45f, 0.5f);
	BringUpTime = 0.675f;
	PutDownTime = 0.45f;

	for (int32 M = 0; M < 2; ++M)
	{
		FArenaFireMode& F = FireModes[M];
		F.FireType = EArenaFireType::Projectile;
		F.RefireTime = 1.f;           // FireRate 1.0
		F.AmmoPerShot = 1;
		F.Color = FLinearColor(1.f, 0.6f, 0.3f);
		F.FireSound = TEXT("S_Fire_Rocket");
		F.Projectile.Speed = 1000.f * UU;
		F.Projectile.Damage = 250.f;
		F.Projectile.DamageRadius = 2000.f * UU;
		F.ProjSpawnOffset = FVector(100.f, 0.f, 0.f); // RedeemerFire
		F.Shake = ArenaViewShake::KickBack(); // RedeemerFire / RedeemerGuidedFire
		F.WarnTargetPct = 0.5f;
		F.bRecommendSplash = true;
	}
}

float AWeap_WarheadLauncher::GetBotRating(float DistanceToEnemy) const
{
	// Redeemer.GetAIRating: 1.5 against an enemy over 2400 UU away (not flying), else 0.4
	if (!HasAmmo())
	{
		return 0.f;
	}
	return DistanceToEnemy > 2400.f * UU ? 1.5f : 0.4f;
}

void AWeap_WarheadLauncher::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || !ArenaOwner)
	{
		return;
	}
	FVector Dir;
	FVector Start = GetProjectileStart(Mode, Dir);
	AController* C = GetOwnerController();
	// RedeemerFire.SpawnProjectile: at the start, else at the shooter, else a small explosion (500, 400 UU) right there
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaWarheadSpawn), false, ArenaOwner);
	const FCollisionShape Shape = FCollisionShape::MakeSphere(24.f * UU);
	if (World->OverlapBlockingTestByChannel(Start, FQuat::Identity, ECC_WorldStatic, Shape, Q))
	{
		Start = ArenaOwner->GetActorLocation();
		if (World->OverlapBlockingTestByChannel(Start, FQuat::Identity, ECC_WorldStatic, Shape, Q))
		{
			ArenaFX::SpawnExplosion(World, Start, FLinearColor(1.f, 0.5f, 0.2f), 600.f, TEXT("Explosion.Rocket"));
			ArenaDamage::ApplyRadialDamage(World, Start, 500.f, 400.f * UU, 100000.f / 100.f * UU, C, this, WeaponName);
			return;
		}
	}
	FActorSpawnParameters SP;
	SP.Owner = ArenaOwner;
	SP.Instigator = ArenaOwner;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* PC = Cast<APlayerController>(C);
	if (Mode == 1 && PC)
	{
		// RedeemerGuidedFire: fly it yourself
		if (AArenaGuidedWarhead* W = World->SpawnActor<AArenaGuidedWarhead>(AArenaGuidedWarhead::StaticClass(), Start, Dir.Rotation(), SP))
		{
			ArenaOwner->StopFire(1);
			W->Launch(ArenaOwner, PC, WeaponName);
		}
		return;
	}
	if (AArenaWarheadShell* S = World->SpawnActor<AArenaWarheadShell>(AArenaWarheadShell::StaticClass(), Start, Dir.Rotation(), SP))
	{
		S->Launch(C, WeaponName, Dir);
	}
}

void AWeap_WarheadLauncher::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cylinder(), FVector(30.f, 14.f, -20.f), AlongX, Tube(16.f, 60.f), WeaponColor);
	AddViewPart(ArenaAssets::Sphere(), FVector(62.f, 14.f, -20.f), FRotator::ZeroRotator, FVector(0.14f), FireModes[0].Color, true);
}

// =================================================================================================
// Orbital Designator (Ion Painter)
// =================================================================================================

AWeap_OrbitalDesignator::AWeap_OrbitalDesignator()
{
	WeaponName = TEXT("Orbital Designator");
	Slot = 0;                     // InventoryGroup 0
	MaxAmmo = 1;                  // BallAmmo: one strike
	InitialAmmo = 1;
	AmmoPickupAmount = 0;
	Priority = 0.98f;
	WeaponColor = FLinearColor(0.35f, 0.4f, 0.5f);
	BringUpTime = 0.33f;
	PutDownTime = 0.33f;

	FArenaFireMode& P = FireModes[0];
	P.FireType = EArenaFireType::Hitscan; // painted by Tick while held (PainterFire.Paint), never fired as a shot
	P.RefireTime = 0.6f;
	P.AmmoPerShot = 1;
	P.Range = 10000.f * UU;               // TraceRange
	P.Damage = 0.f;
	P.Color = FLinearColor(1.f, 0.25f, 0.2f);
	P.WarnTargetPct = 0.1f;
	P.bRecommendSplash = true;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Zoom;    // PainterZoom
	A.RefireTime = 0.1f;
}

float AWeap_OrbitalDesignator::GetBotRating(float DistanceToEnemy) const
{
	// Painter.GetAIRating: 2 when the enemy stands under open sky below us, else 0
	if (!HasAmmo())
	{
		return 0.f;
	}
	const AArenaBotController* B = BotOf(ArenaOwner);
	const AArenaCharacter* E = B ? B->GetEnemy() : nullptr;
	if (!E || !B->IsEnemyVisible() || ArenaOwner->GetActorLocation().Z < E->GetActorLocation().Z)
	{
		return 0.f;
	}
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaPainterBot), false);
	const FVector Feet = E->GetActorLocation() - FVector(0.f, 0.f, E->GetSimpleCollisionHalfHeight());
	return GetWorld()->LineTraceSingleByChannel(Hit, Feet + FVector(0.f, 0.f, 4.f), Feet + FVector(0.f, 0.f, 4000.f * UU), ECC_Visibility, Q) ? 0.f : 2.f;
}

void AWeap_OrbitalDesignator::StartFire(uint8 Mode)
{
	if (Mode == 1)
	{
		// PainterZoom: like the sniper zoom (press to zoom in, press again to zoom out)
		if (ZoomLevel > 0.f)
		{
			ZoomLevel = 0.f;
			bZoomingIn = false;
		}
		else
		{
			bZoomingIn = true;
		}
		Super::StartFire(Mode);
		return;
	}
	if (Mode != 0 || !bEquipped || !ArenaOwner || ArenaOwner->IsDead() || bPainting || Ammo < 1 || GetWorldTime() < NextFireTime)
	{
		return;
	}
	// PainterFire.Paint.BeginState
	bPainting = true;
	bValidMark = false;
	bMarkStarted = false;
	MarkTime = GetWorldTime();
	NextHitTime = MarkTime;
	if (!PaintBeam)
	{
		PaintBeam = MakeBeam(this, FireModes[0].Color, TEXT("XWeapons.PainterBeamEffect"));
	}
	PlayFireSound(0, GetMuzzleLocation(), 0.5f, 1.f);
}

void AWeap_OrbitalDesignator::StopFire(uint8 Mode)
{
	if (Mode == 1)
	{
		bZoomingIn = false;
		Super::StopFire(Mode);
		return;
	}
	if (Mode == 0)
	{
		StopPainting();
	}
}

void AWeap_OrbitalDesignator::StopPainting()
{
	bPainting = false;
	bMarkStarted = false;
	bValidMark = false;
	if (PaintBeam)
	{
		HideBeam(PaintBeam);
	}
}

void AWeap_OrbitalDesignator::Unequip()
{
	StopPainting();
	ZoomLevel = 0.f;
	bZoomingIn = false;
	Super::Unequip();
}

float AWeap_OrbitalDesignator::GetZoomFOV() const
{
	if (!bEquipped || ZoomLevel <= 0.f)
	{
		return 0.f;
	}
	return FMath::Clamp(90.f - 88.f * ZoomLevel, 1.f, 170.f);
}

void AWeap_OrbitalDesignator::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bZoomingIn && bEquipped)
	{
		ZoomLevel = FMath::Min(ZoomLevel + DeltaSeconds, 0.9f);
	}
	if (bPainting)
	{
		if (!bEquipped || !ArenaOwner || ArenaOwner->IsDead())
		{
			StopPainting();
			return;
		}
		PaintTick(DeltaSeconds);
	}
	else if (bNetFireHeld[0] && PaintBeam && bEquipped && ArenaOwner)
	{
		// net play: the beam from our weapon to where our view hits (as PaintTick, EndEffect); the server paints
		FVector Eye, Dir;
		GetAim(Eye, Dir);
		const FVector Start = Eye + Dir * ArenaOwner->GetSimpleCollisionRadius();
		const FVector End = Start + Dir * FireModes[0].Range;
		FHitResult Hit;
		PaintBeam->SetVisibility(true);
		PlaceBeam(PaintBeam, GetMuzzleLocation(), WeaponTrace(Start, End, Hit) ? FVector(Hit.ImpactPoint) : End, 1.5f);
	}
}

void AWeap_OrbitalDesignator::NetFireHeldChanged(uint8 Mode, bool bHeld)
{
	// net play: PainterBeamEffect of a first-person shooter while primary is held, and PainterFire's start sound
	if (Mode != 0)
	{
		return;
	}
	if (bHeld)
	{
		if (!PaintBeam)
		{
			PaintBeam = MakeBeam(this, FireModes[0].Color, TEXT("XWeapons.PainterBeamEffect"));
		}
		PlayFireSound(0, GetMuzzleLocation(), 0.5f, 1.f);
	}
	else if (PaintBeam)
	{
		HideBeam(PaintBeam);
	}
}

void AWeap_OrbitalDesignator::PaintTick(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	const float Now = GetWorldTime();
	// StartTrace = eye + X x CollisionRadius; bots aim at the enemy's feet (PainterFire.Paint.AdjustAim)
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const FVector Start = Eye + Dir * ArenaOwner->GetSimpleCollisionRadius();
	if (AArenaBotController* B = BotOf(ArenaOwner))
	{
		if (const AArenaCharacter* E = B->GetEnemy())
		{
			const FVector Feet = E->GetActorLocation() - FVector(0.f, 0.f, E->GetSimpleCollisionHalfHeight());
			Dir = (Feet - Start).GetSafeNormal();
		}
	}
	const FVector End = Start + Dir * FireModes[0].Range;
	FHitResult Hit;
	const bool bHit = WeaponTrace(Start, End, Hit);
	// a timer sets bDoHit every 0.25 s
	if (Now >= NextHitTime)
	{
		NextHitTime = Now + 0.25f;
		bDoHit = true;
	}
	if (bHit && Hit.GetActor() != ArenaOwner)
	{
		if (bDoHit)
		{
			bValidMark = false;
			const bool bWorld = !Cast<APawn>(Hit.GetActor()) && !Hit.GetActor()->ActorHasTag(TEXT("ArenaShootable"));
			const FVector HitLoc = Hit.ImpactPoint;
			if (bWorld)
			{
				if (FVector::Dist(HitLoc, MarkLocation) < 50.f * UU)
				{
					if (Now - MarkTime > 0.3f)
					{
						const bool bEngageCannon = Now - MarkTime > 1.2f; // PaintDuration
						AArenaIonCannon* Cannon = AArenaIonCannon::FindOrSpawn(World, ArenaOwner->GetActorLocation());
						if (Cannon && Cannon->CheckMark(GetOwnerController(), HitLoc, bEngageCannon, WeaponName))
						{
							if (Cannon->IsFiring())
							{
								// the strike is on: the painter's one charge is used up
								Ammo = FMath::Max(0, Ammo - 1);
								NextFireTime = Now + FireModes[0].RefireTime;
								StopPainting();
								ArenaOwner->NotifyWeaponFired(0);
								CheckOutOfAmmo();
								return;
							}
							bValidMark = true;
							bMarkStarted = true;
						}
						else
						{
							MarkTime = Now;
							bValidMark = false;
							bMarkStarted = false;
						}
					}
				}
				else
				{
					MarkTime = Now;
					MarkLocation = HitLoc;
					bValidMark = false;
					bMarkStarted = false;
				}
			}
			else
			{
				MarkTime = Now;
				bValidMark = false;
				bMarkStarted = false;
			}
			bDoHit = false;
		}
		EndEffect = Hit.ImpactPoint;
	}
	else
	{
		EndEffect = End;
	}
	if (PaintBeam)
	{
		PaintBeam->SetVisibility(true);
		PlaceBeam(PaintBeam, GetMuzzleLocation(), EndEffect, bValidMark ? 3.f : 1.5f);
	}
}

void AWeap_OrbitalDesignator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PaintBeam)
	{
		PaintBeam->DestroyComponent();
		PaintBeam = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AWeap_OrbitalDesignator::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(26.f, 14.f, -16.f), FRotator::ZeroRotator, Box(36.f, 6.f, 7.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(50.f, 14.f, -14.f), AlongX, Tube(2.5f, 30.f), FireModes[0].Color, true);
}

// =================================================================================================
// Marksman Rifle (Classic Sniper Rifle)
// =================================================================================================

AWeap_MarksmanRifle::AWeap_MarksmanRifle()
{
	WeaponName = TEXT("Marksman Rifle");
	Slot = 9;                     // InventoryGroup 9
	MaxAmmo = 35;                 // ClassicSniperAmmo
	InitialAmmo = 15;
	AmmoPickupAmount = 10;        // ClassicSniperAmmoPickup
	Priority = 0.69f;
	BringUpTime = 0.6f;
	PutDownTime = 0.58f;

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::SniperFire(); // ClassicSniperFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Hitscan;
	P.Damage = 60.f;              // DamageMin = DamageMax 60
	P.DamageMax = 60.f;
	P.HeadshotMultiplier = 2.f;   // HeadShotDamageMult
	P.RefireTime = 1.33f;
	P.AmmoPerShot = 1;
	P.Range = 17000.f * UU;       // TraceRange
	P.Momentum = 0.f;             // Momentum 1
	P.Color = FLinearColor(1.f, 0.85f, 0.5f);
	P.BeamWidth = 1.f;
	P.BotAimError = 850.f;
	P.WarnTargetPct = 0.5f;
	P.bReflective = false;
}

void AWeap_MarksmanRifle::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	// ClassicSniperFire.DoTrace: one bullet (a tracer, no lightning arcs)
	AArenaWeapon::FireHitscan(Mode, ChargeAlpha);
}

void AWeap_MarksmanRifle::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cube(), FVector(26.f, 13.f, -15.f), FRotator::ZeroRotator, Box(40.f, 5.f, 6.f), FLinearColor(0.3f, 0.25f, 0.18f));
	AddViewPart(ArenaAssets::Cylinder(), FVector(56.f, 13.f, -13.f), AlongX, Tube(2.f, 40.f), FLinearColor(0.2f, 0.2f, 0.22f));
	AddViewPart(ArenaAssets::Cylinder(), FVector(28.f, 13.f, -8.f), AlongX, Tube(3.f, 18.f), FLinearColor(0.15f, 0.15f, 0.17f));
}

// =================================================================================================
// Seeker Launcher (AVRiL)
// =================================================================================================

AWeap_SeekerLauncher::AWeap_SeekerLauncher()
{
	WeaponName = TEXT("Seeker Launcher");
	Slot = 8;                     // InventoryGroup 8
	MaxAmmo = 25;                 // ONSAVRiLAmmo
	InitialAmmo = 5;
	AmmoPickupAmount = 5;
	Priority = 0.55f;
	WeaponColor = FLinearColor(0.3f, 0.35f, 0.3f);
	BringUpTime = 0.45f;
	PutDownTime = 0.33f;

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::KickBack(); // ONSAVRiLFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 4.f;           // FireRate 4 (reload)
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(1.f, 0.7f, 0.4f);
	P.FireSound = TEXT("S_Fire_Rocket");
	P.Projectile.Speed = 550.f * UU;
	P.Projectile.Damage = 125.f;
	P.Projectile.DamageRadius = 150.f * UU;
	P.ProjSpawnOffset = FVector(25.f, 6.f, -6.f);
	P.WarnTargetPct = 1.f;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Zoom; // ONSAVRiLAltFire: zoom / follow while locked
	A.RefireTime = 0.1f;
}

bool AWeap_SeekerLauncher::CanLockOnTo(const AActor* Other) const
{
	const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Other);
	if (!V || !ArenaOwner)
	{
		return false;
	}
	const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	if (!GM || !GM->IsTeamGame())
	{
		return true;
	}
	return V->GetTeam() != ArenaWeaponActors::TeamOf(GetOwnerController());
}

void AWeap_SeekerLauncher::UpdateLock()
{
	UWorld* World = GetWorld();
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const float LockAim = 0.996f;
	const float MaxLockRange = 15000.f * UU;
	auto Clear = [World, &Eye](const AActor* A)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaSeekerLock), false, A);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return !World->LineTraceSingleByChannel(Hit, Eye, A->GetActorLocation(), ECC_Visibility, Q, RP);
	};
	// ONSAVRiL.WeaponTick for bots: the vehicle the bot focuses on (its enemy's) is locked at once when the line to it is
	// clear (and it is driven or not just the bot's MoveTarget); otherwise no lock
	if (const AArenaBotController* B = BotOf(ArenaOwner))
	{
		AArenaONS_Vehicle* Focus = B->GetEnemy() ? ArenaONS::GetRiddenVehicle(B->GetEnemy()) : nullptr;
		const FVector BotEye = ArenaOwner->GetActorLocation() + FVector(0.f, 0.f, ArenaOwner->BaseEyeHeight);
		FHitResult Hit;
		FCollisionQueryParams BQ(SCENE_QUERY_STAT(ArenaSeekerBotLock), false, ArenaOwner);
		if (Focus)
		{
			BQ.AddIgnoredActor(Focus);
		}
		const bool bClear = Focus && !World->LineTraceSingleByChannel(Hit, BotEye, Focus->GetActorLocation(), ECC_Visibility, BQ);
		const bool bWasLocked = bLockedOn;
		HomingTarget = bClear && CanLockOnTo(Focus) ? Focus : nullptr;
		bLockedOn = HomingTarget.IsValid();
		if (bLockedOn && !bWasLocked)
		{
			++NumBotLocks;
		}
		return;
	}
	AActor* Current = HomingTarget.Get();
	const bool bKeep = Current && FVector::DotProduct((Current->GetActorLocation() - ArenaOwner->GetActorLocation()).GetSafeNormal(), Dir) >= LockAim
		&& FVector::Dist(Current->GetActorLocation(), ArenaOwner->GetActorLocation()) <= MaxLockRange && Clear(Current);
	if (!bKeep)
	{
		// PickTarget(LockAim, MaxLockRange) among the vehicles
		AActor* Best = nullptr;
		float BestAim = LockAim;
		for (TActorIterator<AArenaONS_Vehicle> It(World); It; ++It)
		{
			AArenaONS_Vehicle* V = *It;
			if (!CanLockOnTo(V))
			{
				continue;
			}
			const FVector To = V->GetActorLocation() - Eye;
			const float Dist = To.Size();
			const float Aim = FVector::DotProduct(To / FMath::Max(Dist, 1.f), Dir);
			if (Dist <= MaxLockRange && Aim > BestAim && Clear(V))
			{
				BestAim = Aim;
				Best = V;
			}
		}
		HomingTarget = Best;
	}
	const bool bWasLocked = bLockedOn;
	bLockedOn = CanLockOnTo(HomingTarget.Get());
	if (bLockedOn && !bWasLocked && ArenaOwner->IsPlayerControlled())
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_LockOn"), ArenaOwner->GetActorLocation(), 0.6f);
	}
}

void AWeap_SeekerLauncher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bEquipped || !ArenaOwner || ArenaOwner->IsDead())
	{
		bLockedOn = false;
		return;
	}
	const float Now = GetWorldTime();
	if (NetState.bValid)
	{
		// net play: the server's lock (ONSAVRiL.bLockedOn / HomingTarget, replicated to the owner)
		const bool bWasLocked = bLockedOn;
		HomingTarget = NetState.HomingTarget;
		bLockedOn = NetState.bLockedOn && HomingTarget.IsValid();
		if (bLockedOn && !bWasLocked)
		{
			ArenaAssets::PlaySoundAt(this, TEXT("S_LockOn"), ArenaOwner->GetActorLocation(), 0.6f);
		}
	}
	else if (Now >= LockCheckTime)
	{
		LockCheckTime = Now + 0.2f; // LockCheckFreq
		UpdateLock();
	}
	// ONSAVRiLAltFire.ModeTick: while held and locked, zoom in (0.6 at most) and keep the view on the target; losing the
	// lock for 1.5 s ends it
	if (bFireHeld[1])
	{
		if (!IsLockedOn())
		{
			if (!bLosingLock)
			{
				bLosingLock = true;
				LostLockTime = Now;
			}
			else if (Now > LostLockTime + 1.5f)
			{
				StopFire(1);
				return;
			}
		}
		else
		{
			bLosingLock = false;
			ZoomLevel = FMath::Min(ZoomLevel + DeltaSeconds, 0.6f);
			if (AController* C = GetOwnerController())
			{
				C->SetControlRotation((HomingTarget->GetActorLocation() - ArenaOwner->GetActorLocation()).Rotation());
			}
		}
	}
}

void AWeap_SeekerLauncher::StartFire(uint8 Mode)
{
	// AllowFire (alt): only players, only while locked
	if (Mode == 1 && (!IsLockedOn() || !ArenaOwner || !ArenaOwner->IsPlayerControlled()))
	{
		return;
	}
	Super::StartFire(Mode);
}

void AWeap_SeekerLauncher::StopFire(uint8 Mode)
{
	if (Mode == 1)
	{
		ZoomLevel = 0.f;
		bLosingLock = false;
	}
	Super::StopFire(Mode);
}

void AWeap_SeekerLauncher::Unequip()
{
	ZoomLevel = 0.f;
	bLockedOn = false;
	HomingTarget.Reset();
	Super::Unequip();
}

float AWeap_SeekerLauncher::GetZoomFOV() const
{
	return bEquipped && ZoomLevel > 0.f ? FMath::Clamp(90.f - ZoomLevel * 88.f, 1.f, 170.f) : 0.f;
}

float AWeap_SeekerLauncher::GetBotRating(float DistanceToEnemy) const
{
	// ONSAVRiL.GetAIRating: AIRating 0.55 without an enemy, 0 against anything but a vehicle; against a vehicle +0.1 when
	// it is over 200 UU higher, and beyond 2000 UU + (dist - 2000) x 0.0002 (UU) up to 2
	if (!HasAmmo())
	{
		return 0.f;
	}
	const AArenaBotController* B = BotOf(ArenaOwner);
	const AArenaCharacter* E = B ? B->GetEnemy() : nullptr;
	if (!B || !E)
	{
		return 0.55f;
	}
	const AArenaONS_Vehicle* V = ArenaONS::GetRiddenVehicle(E);
	if (!V || !CanLockOnTo(V))
	{
		return 0.f;
	}
	float Result = 0.55f;
	const float ZDiff = (ArenaOwner->GetActorLocation().Z - V->GetActorLocation().Z) / UU;
	if (ZDiff < -200.f)
	{
		Result += 0.1f;
	}
	const float Dist = FVector::Dist(V->GetActorLocation(), ArenaOwner->GetActorLocation()) / UU;
	if (Dist > 2000.f)
	{
		return FMath::Min(2.f, Result + (Dist - 2000.f) * 0.0002f);
	}
	return Result;
}

void AWeap_SeekerLauncher::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || !ArenaOwner || Mode != 0)
	{
		return;
	}
	FVector Dir;
	const FVector Start = GetProjectileStart(0, Dir);
	FActorSpawnParameters SP;
	SP.Owner = this;
	SP.Instigator = ArenaOwner;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AArenaSeekerRocket* R = World->SpawnActor<AArenaSeekerRocket>(AArenaSeekerRocket::StaticClass(), Start, Dir.Rotation(), SP))
	{
		R->Launch(this, GetOwnerController(), WeaponName, Dir);
	}
	if (const AArenaBotController* B = BotOf(ArenaOwner))
	{
		++NumBotShots;
		UE_LOG(LogOpenUT2k4, Log, TEXT("BotAVRiL: %s fired at %s (locked %d, %.0f m)"), B->PlayerState ? *B->PlayerState->GetPlayerName() : TEXT("?"), HomingTarget.IsValid() ? *Cast<AArenaONS_Vehicle>(HomingTarget.Get())->GetVehicleName() : TEXT("nothing"), IsLockedOn() ? 1 : 0,
			HomingTarget.IsValid() ? FVector::Dist(HomingTarget->GetActorLocation(), Start) / 100.f : 0.f);
	}
	// ONSAVRiLFire.ShakeView: KickMomentum (-350, 0, 175) UU/s rotated by the shooter (AddVelocity)
	const FVector Kick = ArenaOwner->GetActorRotation().RotateVector(FVector(-350.f, 0.f, 175.f) * UU);
	ArenaOwner->LaunchCharacter(Kick, false, false);
}

void AWeap_SeekerLauncher::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cylinder(), FVector(32.f, 15.f, -18.f), AlongX, Tube(13.f, 70.f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(20.f, 15.f, -26.f), FRotator::ZeroRotator, Box(14.f, 5.f, 12.f), WeaponColor * 0.6f);
}

// =================================================================================================
// Sticky Launcher (ONS Grenade Launcher)
// =================================================================================================

AWeap_StickyLauncher::AWeap_StickyLauncher()
{
	WeaponName = TEXT("Sticky Launcher");
	Slot = 7;                     // InventoryGroup 7
	MaxAmmo = 50;                 // ONSGrenadeAmmo
	InitialAmmo = 10;
	AmmoPickupAmount = 5;         // ONSGrenadeAmmoPickup
	Priority = 0.55f;
	WeaponColor = FLinearColor(0.32f, 0.36f, 0.28f);
	BringUpTime = 0.33f;
	PutDownTime = 0.33f;

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::BioFire(); // ONSGrenadeFire extends BioFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 0.65f;         // ONSGrenadeFire FireRate
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(0.6f, 0.8f, 0.4f);
	P.FireSound = TEXT("S_Fire_Goo");
	P.Projectile.Speed = 1200.f * UU;
	P.Projectile.Damage = 100.f;
	P.Projectile.DamageRadius = 175.f * UU;
	P.Projectile.GravityScale = 1.f;
	P.ProjSpawnOffset = FVector(20.f, 9.f, -6.f); // BioFire
	P.WarnTargetPct = 0.5f;
	P.bTossed = true;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Hitscan; // ONSGrenadeAltFire: detonate them all (FireHitscan)
	A.RefireTime = 1.f;
	A.AmmoPerShot = 0;
	A.Range = 0.f;
	A.Damage = 0.f;
	A.FireSound = TEXT("");
}

int32 AWeap_StickyLauncher::GetNumGrenades() const
{
	int32 N = 0;
	for (const TWeakObjectPtr<AArenaStickyGrenade>& G : Grenades)
	{
		N += G.IsValid() ? 1 : 0;
	}
	return N;
}

bool AWeap_StickyLauncher::HasAmmo() const
{
	// ONSGrenadeLauncher.HasAmmo: grenades still out count
	return GetNumGrenades() > 0 || Super::HasAmmo();
}

bool AWeap_StickyLauncher::HasAmmoFor(uint8 Mode) const
{
	if (Mode == 0)
	{
		// ONSGrenadeFire.AllowFire: not with 8 out
		return GetNumGrenades() < MaxGrenades && Super::HasAmmoFor(0);
	}
	return GetNumGrenades() > 0; // ONSGrenadeAltFire.AllowFire
}

void AWeap_StickyLauncher::NotifyGrenadeGone(AArenaStickyGrenade* G)
{
	Grenades.RemoveAll([G](const TWeakObjectPtr<AArenaStickyGrenade>& X) { return !X.IsValid() || X.Get() == G; });
}

void AWeap_StickyLauncher::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || !ArenaOwner || Mode != 0)
	{
		return;
	}
	FVector Dir;
	const FVector Start = GetProjectileStart(0, Dir);
	FActorSpawnParameters SP;
	SP.Owner = this;
	SP.Instigator = ArenaOwner;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AArenaStickyGrenade* G = World->SpawnActor<AArenaStickyGrenade>(AArenaStickyGrenade::StaticClass(), Start, Dir.Rotation(), SP))
	{
		G->Launch(this, GetOwnerController(), WeaponName, Dir, 1200.f * UU);
		Grenades.Add(G);
	}
}

void AWeap_StickyLauncher::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	if (Mode == 1)
	{
		DetonateAll();
	}
}

void AWeap_StickyLauncher::DetonateAll()
{
	// ONSGrenadeAltFire.DoFireEffect: every grenade explodes
	TArray<TWeakObjectPtr<AArenaStickyGrenade>> All = Grenades;
	Grenades.Reset();
	for (const TWeakObjectPtr<AArenaStickyGrenade>& G : All)
	{
		if (AArenaStickyGrenade* X = G.Get())
		{
			X->Explode();
		}
	}
}

void AWeap_StickyLauncher::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// ONSGrenadeLauncher.Destroyed: the grenades go off with it (the owner died)
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		DetonateAll();
	}
	Super::EndPlay(EndPlayReason);
}

float AWeap_StickyLauncher::GetBotRating(float DistanceToEnemy) const
{
	// ONSGrenadeLauncher.GetAIRating: 0.1 beyond 1500 UU, 0.35 beyond 1000 UU, else 0.55
	if (!HasAmmo())
	{
		return 0.f;
	}
	if (DistanceToEnemy > 1500.f * UU)
	{
		return 0.1f;
	}
	return DistanceToEnemy > 1000.f * UU ? 0.35f : 0.55f;
}

uint8 AWeap_StickyLauncher::GetBotFireMode(float DistanceToEnemy) const
{
	// ONSGrenadeLauncher.BestMode: detonate when all 8 are out, when out of ammo, or when one sticks to a pawn
	if (GetNumGrenades() >= MaxGrenades || (Ammo <= 0 && GetNumGrenades() > 0))
	{
		return 1;
	}
	for (const TWeakObjectPtr<AArenaStickyGrenade>& G : Grenades)
	{
		if (G.IsValid() && Cast<APawn>(G->GetStuckTo()))
		{
			return 1;
		}
	}
	return 0;
}

void AWeap_StickyLauncher::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cylinder(), FVector(30.f, 14.f, -18.f), AlongX, Tube(10.f, 50.f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(22.f, 14.f, -18.f), FRotator::ZeroRotator, Tube(14.f, 12.f), WeaponColor * 0.6f);
}

// =================================================================================================
// Crawler Layer (ONS Mine Layer)
// =================================================================================================

AWeap_CrawlerLayer::AWeap_CrawlerLayer()
{
	WeaponName = TEXT("Crawler Layer");
	Slot = 3;                     // InventoryGroup 3
	MaxAmmo = 25;                 // ONSMineAmmo
	InitialAmmo = 4;
	AmmoPickupAmount = 8;         // ONSMineAmmoPickup
	Priority = 0.5f;
	WeaponColor = FLinearColor(0.33f, 0.33f, 0.3f);
	BringUpTime = 0.35f;
	PutDownTime = 0.4f;

	FArenaFireMode& P = FireModes[0];
	P.Shake = ArenaViewShake::BioFire(); // ONSMineThrowFire extends BioFire ShakeRot / ShakeOffset defaults
	P.FireType = EArenaFireType::Projectile;
	P.RefireTime = 1.1f;          // ONSMineThrowFire FireRate
	P.AmmoPerShot = 1;
	P.Color = FLinearColor(0.9f, 0.5f, 0.3f);
	P.FireSound = TEXT("S_Fire_Goo");
	P.Projectile.Speed = 800.f * UU;
	P.Projectile.Damage = 95.f;
	P.Projectile.DamageRadius = 250.f * UU;
	P.Projectile.GravityScale = 1.f;
	P.ProjSpawnOffset = FVector(20.f, 9.f, -6.f); // BioFire
	P.WarnTargetPct = 0.5f;
	P.bTossed = true;
	P.bRecommendSplash = true;

	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Zoom; // the target laser (Tick while held), no ammo
	A.RefireTime = 0.6f;
	A.Range = 10000.f * UU;
	A.Color = FLinearColor(1.f, 0.2f, 0.15f);
}

int32 AWeap_CrawlerLayer::GetNumMines() const
{
	int32 N = 0;
	for (const TWeakObjectPtr<AArenaCrawlerMine>& M : Mines)
	{
		N += M.IsValid() ? 1 : 0;
	}
	return N;
}

void AWeap_CrawlerLayer::NotifyMineGone(AArenaCrawlerMine* M)
{
	Mines.RemoveAll([M](const TWeakObjectPtr<AArenaCrawlerMine>& X) { return !X.IsValid() || X.Get() == M; });
}

void AWeap_CrawlerLayer::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || !ArenaOwner || Mode != 0)
	{
		return;
	}
	FVector Dir;
	const FVector Start = GetProjectileStart(0, Dir);
	FActorSpawnParameters SP;
	SP.Owner = this;
	SP.Instigator = ArenaOwner;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AArenaCrawlerMine* M = World->SpawnActor<AArenaCrawlerMine>(AArenaCrawlerMine::StaticClass(), Start, FRotator(0.f, Dir.Rotation().Yaw, 0.f), SP))
	{
		// ONSMineThrowFire.SpawnProjectile: the oldest mine goes when MaxMines are out; Velocity = Instigator.Velocity +
		// Speed x Dir (TossZ 0)
		Mines.RemoveAll([](const TWeakObjectPtr<AArenaCrawlerMine>& X) { return !X.IsValid(); });
		if (Mines.Num() >= MaxMines && Mines[0].IsValid())
		{
			Mines[0]->Destroy();
			Mines.RemoveAt(0);
		}
		M->Launch(this, GetOwnerController(), WeaponName, ArenaOwner->GetVelocity() + Dir * 800.f * UU);
		Mines.Add(M);
	}
}

void AWeap_CrawlerLayer::StartFire(uint8 Mode)
{
	if (Mode == 1)
	{
		if (!bEquipped || !ArenaOwner || ArenaOwner->IsDead())
		{
			return;
		}
		bTargeting = true;
		NextTargetHit = GetWorldTime(); // ModeHoldFire: SetTimer(0.5, true); bDoHit = true
		if (!TargetBeam)
		{
			TargetBeam = MakeBeam(this, FireModes[1].Color, TEXT("Onslaught.ONSMineLayerTargetBeamEffect"));
		}
		Super::StartFire(Mode);
		return;
	}
	Super::StartFire(Mode);
}

void AWeap_CrawlerLayer::StopFire(uint8 Mode)
{
	if (Mode == 1)
	{
		bTargeting = false;
		if (TargetBeam)
		{
			HideBeam(TargetBeam);
		}
	}
	Super::StopFire(Mode);
}

void AWeap_CrawlerLayer::Unequip()
{
	bTargeting = false;
	if (TargetBeam)
	{
		HideBeam(TargetBeam);
	}
	Super::Unequip();
}

void AWeap_CrawlerLayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bTargeting || !bEquipped || !ArenaOwner || ArenaOwner->IsDead())
	{
		return;
	}
	// ONSMineLayerAltFire.ModeTick: the laser from the eye (+ CollisionRadius); every 0.5 s all mines get its spot
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const FVector Start = Eye + Dir * ArenaOwner->GetSimpleCollisionRadius();
	const FVector End = Start + Dir * FireModes[1].Range;
	FHitResult Hit;
	const FVector HitLoc = WeaponTrace(Start, End, Hit) && Hit.GetActor() != ArenaOwner ? FVector(Hit.ImpactPoint) : End;
	if (TargetBeam)
	{
		TargetBeam->SetVisibility(true);
		PlaceBeam(TargetBeam, GetMuzzleLocation(), HitLoc, 1.2f);
	}
	const float Now = GetWorldTime();
	if (Now >= NextTargetHit)
	{
		NextTargetHit = Now + 0.5f;
		for (const TWeakObjectPtr<AArenaCrawlerMine>& M : Mines)
		{
			if (M.IsValid())
			{
				M->SetScurryTarget(HitLoc);
			}
		}
	}
}

void AWeap_CrawlerLayer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// the mines stay (UE2 ONSMineLayer.Destroyed only drops its list; mines without an instigator blow up on their timer)
	Mines.Reset();
	if (TargetBeam)
	{
		TargetBeam->DestroyComponent();
		TargetBeam = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

float AWeap_CrawlerLayer::GetBotRating(float DistanceToEnemy) const
{
	// ONSMineLayer.GetAIRating: 0.1 beyond 1500 UU, 0.35 beyond 1000 UU, else 0.55
	if (Ammo <= 0)
	{
		return 0.f;
	}
	if (DistanceToEnemy > 1500.f * UU)
	{
		return 0.1f;
	}
	return DistanceToEnemy > 1000.f * UU ? 0.35f : 0.55f;
}

uint8 AWeap_CrawlerLayer::GetBotFireMode(float DistanceToEnemy) const
{
	// ONSMineLayer.BestMode: lay mines (fewer than 2 out, enemy within 2500 UU, or half the time), else the laser
	if (GetNumMines() < 2 || DistanceToEnemy < 2500.f * UU || FMath::FRand() < 0.5f)
	{
		return 0;
	}
	return 1;
}

void AWeap_CrawlerLayer::BuildViewModel()
{
	AddViewPart(ArenaAssets::Cylinder(), FVector(30.f, 14.f, -18.f), AlongX, Tube(12.f, 46.f), WeaponColor);
	AddViewPart(ArenaAssets::Sphere(), FVector(52.f, 14.f, -18.f), FRotator::ZeroRotator, FVector(0.08f), FireModes[1].Color, true);
}

// =================================================================================================

TArray<TSubclassOf<AArenaWeapon>> ArenaWeaponList::ExtraWeapons()
{
	TArray<TSubclassOf<AArenaWeapon>> List;
	List.Add(AWeap_WarheadLauncher::StaticClass());
	List.Add(AWeap_OrbitalDesignator::StaticClass());
	List.Add(AWeap_MarksmanRifle::StaticClass());
	List.Add(AWeap_SeekerLauncher::StaticClass());
	List.Add(AWeap_StickyLauncher::StaticClass());
	List.Add(AWeap_CrawlerLayer::StaticClass());
	return List;
}

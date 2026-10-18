#include "ArenaWeapon.h"
#include "ArenaWeaponHand.h"
#include "ArenaWeapons.h"
#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaFX.h"
#include "ArenaProjectile.h"
#include "ArenaMover.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaBotController.h"
#include "ArenaGameMode.h"
#include "EngineUtils.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "CollisionQueryParams.h"

AArenaWeapon::AArenaWeapon()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after the owner moved / rotated this frame
	bReplicates = false;
	SetCanBeDamaged(false);

	ViewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewRoot"));
	RootComponent = ViewRoot;

	SetActorEnableCollision(false);
}

void AArenaWeapon::BeginPlay()
{
	Super::BeginPlay();
	if (bInfiniteAmmo)
	{
		Ammo = MaxAmmo;
	}
}

void AArenaWeapon::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	HideLiveBeam();
	Super::EndPlay(EndPlayReason);
}

float AArenaWeapon::GetWorldTime() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.f;
}

AController* AArenaWeapon::GetOwnerController() const
{
	return ArenaOwner ? ArenaOwner->GetController() : nullptr;
}

// ------------------------------------------------------------------------------------------------
// Inventory
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::GiveTo(AArenaCharacter* NewOwner)
{
	ArenaOwner = NewOwner;
	SetOwner(NewOwner);
	SetInstigator(NewOwner);

	if (bInfiniteAmmo)
	{
		Ammo = MaxAmmo;
	}
	else
	{
		Ammo = FMath::Clamp(FMath::Max(Ammo, InitialAmmo), 0, MaxAmmo);
		AltAmmo = FMath::Clamp(FMath::Max(AltAmmo, AltInitialAmmo), 0, AltMaxAmmo);
	}

	if (NewOwner && NewOwner->WeaponRoot)
	{
		AttachToComponent(NewOwner->WeaponRoot, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	}

	if (ViewParts.Num() == 0)
	{
		BuildViewModel();
	}
	if (!ViewMesh)
	{
		SetupImportedViewModel();
	}
	if (ViewRoot)
	{
		ViewRoot->SetVisibility(bEquipped, true);
	}
}

void AArenaWeapon::Equip()
{
	if (!ArenaOwner)
	{
		return;
	}
	bEquipped = true;
	EquipTime = GetWorldTime();
	// UE2: the previous weapon's PutDownTime, then this one's BringUpTime
	NextFireTime = FMath::Max(NextFireTime, EquipTime + EquipDelay + BringUpTime);
	EquipDelay = 0.f;
	RecoilOffset = 0.f;
	if (ViewRoot)
	{
		ViewRoot->SetVisibility(true, true);
	}
	if (ViewMesh)
	{
		// Wait for the previous weapon's put-down animation, then bring this one up.
		float ShowAt = EquipTime;
		for (const AArenaWeapon* Other : ArenaOwner->GetWeapons())
		{
			if (Other && Other != this)
			{
				ShowAt = FMath::Max(ShowAt, Other->GetPutDownEndTime());
			}
		}
		ShowViewTime = ShowAt;
		bViewShown = false;
		PutDownEndTime = -1.f;
		HoldAnimMode = 255;
		ViewMesh->SetHiddenInGame(true);
		ApplyViewFOV();
		TickViewModel(0.f);
	}
}

void AArenaWeapon::Unequip()
{
	StopAllFire();
	const bool bWasShown = bEquipped && bViewShown;
	bEquipped = false;
	if (ViewRoot)
	{
		ViewRoot->SetVisibility(false, true);
	}
	HideLiveBeam();
	if (ViewMesh)
	{
		HoldAnimMode = 255;
		bViewShown = false;
		PutDownEndTime = -1.f;
		if (bWasShown && ArenaOwner && !ArenaOwner->IsDead() && PlayViewAnim(Art->PutDownAnim, false, 0.2f))
		{
			// Keep the mesh up while it is put away; the next weapon appears afterwards.
			ViewMesh->SetVisibility(true);
			PutDownEndTime = GetWorldTime() + 0.2f;
		}
	}
}

void AArenaWeapon::AddAmmo(int32 Amount)
{
	Ammo = FMath::Clamp(Ammo + Amount, 0, MaxAmmo);
}

void AArenaWeapon::AddAltAmmo(int32 Amount)
{
	AltAmmo = FMath::Clamp(AltAmmo + Amount, 0, AltMaxAmmo);
}

bool AArenaWeapon::TakeAmmo(uint8 Mode, int32 Amount)
{
	if (bInfiniteAmmo || Mode > 1)
	{
		return true;
	}
	int32& Pool = FireModes[Mode].bUseAltAmmo ? AltAmmo : Ammo;
	const bool bEnough = Pool >= Amount;
	Pool = FMath::Max(0, Pool - Amount);
	return bEnough;
}

bool AArenaWeapon::GiveAmmoPickup()
{
	// UE2 Weapon.HandlePickupQuery: nothing when full, else AmmoAmount up to MaxAmmo
	if (bInfiniteAmmo || Ammo >= MaxAmmo)
	{
		return false;
	}
	AddAmmo(AmmoPickupAmount);
	return true;
}

bool AArenaWeapon::OnRepeatPickup(bool bWeaponStay)
{
	// UE2 Weapon.GiveAmmo (bNoAmmoInstances): picking the weapon up again adds its InitialAmount; with weapon stay
	// (WeaponPickup.AllowRepeatPickup false) an owned weapon can't be picked up at all.
	if (bWeaponStay || bInfiniteAmmo || (Ammo >= MaxAmmo && (AltMaxAmmo <= 0 || AltAmmo >= AltMaxAmmo)))
	{
		return false;
	}
	AddAmmo(InitialAmmo);
	AddAltAmmo(AltInitialAmmo);
	return true;
}

bool AArenaWeapon::HasAmmoFor(uint8 Mode) const
{
	if (Mode > 1)
	{
		return false;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	if (FM.FireType == EArenaFireType::Zoom)
	{
		return true;
	}
	if (bInfiniteAmmo)
	{
		return true;
	}
	if (FM.FireType == EArenaFireType::Shield)
	{
		return Ammo > 0;
	}
	if (FM.AmmoPerShot <= 0)
	{
		return true;
	}
	return GetAmmoFor(Mode) >= FM.AmmoPerShot;
}

bool AArenaWeapon::HasAmmo() const
{
	if (bInfiniteAmmo)
	{
		return true;
	}
	for (uint8 M = 0; M < 2; ++M)
	{
		const EArenaFireType T = FireModes[M].FireType;
		if (T != EArenaFireType::Zoom && T != EArenaFireType::Shield && HasAmmoFor(M))
		{
			return true;
		}
	}
	return false;
}

void AArenaWeapon::ConsumeAmmo(uint8 Mode)
{
	if (bInfiniteAmmo || Mode > 1)
	{
		return;
	}
	int32& Pool = FireModes[Mode].bUseAltAmmo ? AltAmmo : Ammo;
	Pool = FMath::Max(0, Pool - FMath::Max(0, FireModes[Mode].AmmoPerShot));
}

bool AArenaWeapon::CheckOutOfAmmo()
{
	if (bInfiniteAmmo || HasAmmo() || !ArenaOwner || ArenaOwner->IsDead())
	{
		return false;
	}
	if (bEquipped)
	{
		ArenaOwner->SwitchToBestWeapon();
		return true;
	}
	return false;
}

float AArenaWeapon::GetDamageScale() const
{
	return ArenaOwner ? ArenaOwner->GetDamageScale() : 1.f;
}

float AArenaWeapon::GetRefireScale() const
{
	return ArenaOwner ? FMath::Max(0.1f, ArenaOwner->GetRefireScale()) : 1.f;
}

float AArenaWeapon::GetProjectileSpeed(uint8 Mode) const
{
	if (Mode > 1 || FireModes[Mode].FireType != EArenaFireType::Projectile)
	{
		return 0.f;
	}
	return FireModes[Mode].Projectile.Speed;
}

bool AArenaWeapon::IsChargeMode(uint8 Mode) const
{
	if (Mode > 1)
	{
		return false;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	return FM.FireType == EArenaFireType::Melee
		|| ((FM.FireType == EArenaFireType::Hitscan || FM.FireType == EArenaFireType::Projectile) && FM.MaxChargeTime > 0.f);
}

// ------------------------------------------------------------------------------------------------
// Fire input
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::StartFire(uint8 Mode)
{
	if (Mode > 1 || !bEquipped || !ArenaOwner || ArenaOwner->IsDead() || bFireHeld[Mode])
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	const float Now = GetWorldTime();
	bFireHeld[Mode] = true;

	switch (FM.FireType)
	{
	case EArenaFireType::Zoom:
	case EArenaFireType::Shield:
	case EArenaFireType::Beam:
		ChargeStart[Mode] = Now;
		break;
	default:
		ChargeStart[Mode] = FMath::Max(Now, NextFireTime);
		break;
	}

	if (FM.FireType == EArenaFireType::Zoom)
	{
		return;
	}

	if (!HasAmmoFor(Mode))
	{
		CheckOutOfAmmo();
		return;
	}

	if (ViewMesh && Art && Art->HoldAnim[Mode] && bViewShown)
	{
		if (PlayViewAnim(Art->HoldAnim[Mode], Art->bLoopHold[Mode]))
		{
			HoldAnimMode = Mode;
			ViewAnimEndTime = TNumericLimits<float>::Max();
		}
	}

	if (FM.FireType == EArenaFireType::Shield)
	{
		ShieldAccum = 0.f;
		PlayFireSound(Mode, GetMuzzleLocation(), 0.6f, 1.3f);
		return;
	}

	// UE2: an idle weapon fires at once (after PreFireTime: minigun wind-up); NextFireTime then advances by FireRate.
	if (FM.FireType == EArenaFireType::Hitscan || FM.FireType == EArenaFireType::Projectile || FM.FireType == EArenaFireType::Beam)
	{
		NextFireTime = FMath::Max(NextFireTime, Now + FM.PreFireTime);
	}
	// Instant response for automatic modes (Tick keeps the loop going while held).
	if ((FM.FireType == EArenaFireType::Hitscan || FM.FireType == EArenaFireType::Projectile) && !IsChargeMode(Mode) && Now >= NextFireTime)
	{
		FireShot(Mode, 0.f);
		AdvanceFireTime(Mode, Now);
		CheckOutOfAmmo();
	}
}

void AArenaWeapon::AdvanceFireTime(uint8 Mode, float Now)
{
	// UE2 WeaponFire.ModeDoFire: NextFireTime += FireRate, never behind by more than one shot
	const float Rate = FireModes[Mode].RefireTime * GetRefireScale() * GetModeRefireScale(Mode);
	NextFireTime = FMath::Max(NextFireTime + Rate, Now - Rate * 0.5f);
	NextFireTime = FMath::Max(NextFireTime, Now + 0.0001f);
}

void AArenaWeapon::StopFire(uint8 Mode)
{
	if (Mode > 1 || !bFireHeld[Mode])
	{
		return;
	}
	bFireHeld[Mode] = false;
	const FArenaFireMode& FM = FireModes[Mode];

	if (HoldAnimMode == Mode)
	{
		// Back to idle unless the release fires a shot (which plays its own animation)
		HoldAnimMode = 255;
		ViewAnimEndTime = GetWorldTime();
	}

	if (FM.FireType == EArenaFireType::Beam)
	{
		const uint8 Other = 1 - Mode;
		if (!(bFireHeld[Other] && FireModes[Other].FireType == EArenaFireType::Beam))
		{
			HideLiveBeam();
		}
	}

	if (IsChargeMode(Mode) && bEquipped && ArenaOwner && !ArenaOwner->IsDead())
	{
		const float Now = GetWorldTime();
		if (Now >= NextFireTime && HasAmmoFor(Mode))
		{
			const float Alpha = FM.MaxChargeTime > 0.f ? FMath::Clamp((Now - ChargeStart[Mode]) / FM.MaxChargeTime, 0.f, 1.f) : 1.f;
			FireShot(Mode, Alpha);
			NextFireTime = Now + FM.RefireTime * GetRefireScale();
			CheckOutOfAmmo();
		}
	}
}

void AArenaWeapon::StopAllFire()
{
	bFireHeld[0] = false;
	bFireHeld[1] = false;
	HideLiveBeam();
}

// ------------------------------------------------------------------------------------------------
// Tick / firing loop
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const float Now = GetWorldTime();

	// View model: recoil kick and bring-up animation.
	RecoilOffset = FMath::FInterpTo(RecoilOffset, 0.f, DeltaSeconds, 9.f);
	TickViewModel(DeltaSeconds);
	if (ViewRoot && bEquipped && ViewMesh)
	{
		// The imported model animates itself (select, fire kick); only its placement is applied.
		ViewRoot->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
	}
	else if (ViewRoot && bEquipped)
	{
		const float Raise = FMath::Clamp((Now - EquipTime) / 0.2f, 0.f, 1.f);
		const float Lower = FMath::Square(1.f - Raise) * 30.f;
		ViewRoot->SetRelativeLocationAndRotation(
			FVector(-RecoilOffset, 0.f, -RecoilOffset * 0.12f - Lower),
			FRotator(RecoilOffset * 0.8f - Lower * 0.6f, 0.f, 0.f));
	}

	if (!bEquipped || !ArenaOwner || ArenaOwner->IsDead())
	{
		if (bBeamActive)
		{
			HideLiveBeam();
		}
		if (ArenaOwner && ArenaOwner->IsDead())
		{
			bFireHeld[0] = bFireHeld[1] = false;
		}
		return;
	}

	bool bAnyBeam = false;
	for (uint8 M = 0; M < 2; ++M)
	{
		if (!bFireHeld[M])
		{
			continue;
		}
		const FArenaFireMode& FM = FireModes[M];

		if (IsChargeMode(M))
		{
			// Bots can't hold a charge forever: auto-release shortly after full charge.
			if (!ArenaOwner->IsPlayerControlled() && Now - ChargeStart[M] > FM.MaxChargeTime + 0.6f)
			{
				StopFire(M);
				if (!bEquipped)
				{
					return;
				}
			}
			continue;
		}

		switch (FM.FireType)
		{
		case EArenaFireType::Hitscan:
		case EArenaFireType::Projectile:
			if (Now >= NextFireTime)
			{
				if (HasAmmoFor(M))
				{
					FireShot(M, 0.f);
					AdvanceFireTime(M, Now);
				}
				if (CheckOutOfAmmo() || !bEquipped)
				{
					return;
				}
			}
			break;

		case EArenaFireType::Beam:
			if (HasAmmoFor(M))
			{
				bAnyBeam = true;
				UpdateLiveBeam(M);
				if (Now - LastBeamSoundTime > 0.45f)
				{
					LastBeamSoundTime = Now;
					PlayFireSound(M, GetMuzzleLocation(), 0.7f, FMath::FRandRange(0.95f, 1.05f));
				}
				if (Now >= NextFireTime)
				{
					FireShot(M, 0.f);
					AdvanceFireTime(M, Now);
					if (CheckOutOfAmmo() || !bEquipped)
					{
						return;
					}
				}
			}
			else if (CheckOutOfAmmo())
			{
				return;
			}
			break;

		case EArenaFireType::Shield:
			if (!bInfiniteAmmo && Ammo > 0)
			{
				ShieldAccum += DeltaSeconds;
				const float Interval = FMath::Max(0.02f, FM.RefireTime);
				while (ShieldAccum >= Interval && Ammo > 0)
				{
					ShieldAccum -= Interval;
					Ammo = FMath::Max(0, Ammo - FMath::Max(1, FM.AmmoPerShot));
				}
			}
			break;

		case EArenaFireType::Zoom:
		default:
			break;
		}
	}

	if (!bAnyBeam && bBeamActive)
	{
		HideLiveBeam();
	}
}

void AArenaWeapon::FireShot(uint8 Mode, float ChargeAlpha)
{
	if (Mode > 1 || !ArenaOwner)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	SpawnMode = Mode;
	++NetShotCount;
	NetShotMode = Mode;
	switch (FM.FireType)
	{
	case EArenaFireType::Hitscan:    FireHitscan(Mode, ChargeAlpha); break;
	case EArenaFireType::Projectile: FireProjectile(Mode, ChargeAlpha); break;
	case EArenaFireType::Beam:       FireBeamTick(Mode, FM.RefireTime); break;
	case EArenaFireType::Melee:      FireMelee(Mode, ChargeAlpha); break;
	default: return;
	}

	ConsumeAmmo(Mode);

	if (ArenaOwner)
	{
		// UE2 WeaponFire.ModeDoFire -> ShakeView (locally controlled shooter): PlayerController.WeaponShakeView
		ArenaViewShake::WeaponShakeView(ArenaOwner->GetController(), FM.Shake);
		ArenaOwner->NotifyWeaponFired(Mode);
		// UE2 WeaponFire: the shooter makes a noise bots hear (Pawn.MakeNoise(1.0))
		AArenaBotController::ReportNoise(ArenaOwner, ArenaOwner->GetActorLocation(), 1.f);
	}
	if (FM.FireType != EArenaFireType::Beam)
	{
		PlayFireEffects(Mode, ChargeAlpha);
	}
}

void AArenaWeapon::PlayFireEffects(uint8 Mode, float ChargeAlpha)
{
	const FArenaFireMode& FM = FireModes[Mode];
	const FVector Muzzle = GetMuzzleLocation();
	PlayFireSound(Mode, Muzzle, 1.f, FMath::FRandRange(0.96f, 1.04f));

	if (FM.FireType != EArenaFireType::Melee)
	{
		const float FlashScale = FMath::Clamp(0.6f + FM.BeamWidth * 0.12f + FM.NumShots * 0.05f, 0.6f, 2.f);
		SpawnViewMuzzleFlash(FM.Color, FlashScale, Mode);
	}

	if (ViewMesh && Art && bViewShown && Art->FireAnim[Mode] && HoldAnimMode != Mode)
	{
		// Fit the shot animation into the refire time so rapid fire never lags behind
		const float Refire = FMath::Max(0.05f, FM.RefireTime * GetRefireScale());
		if (PlayViewAnim(Art->FireAnim[Mode], false, 0.f, Refire))
		{
			HoldAnimMode = 255;
		}
	}

	const float ShotDamage = (FM.FireType == EArenaFireType::Projectile ? FM.Projectile.Damage : FM.Damage) * FMath::Max(1, FM.NumShots);
	AddRecoil(FMath::Clamp(ShotDamage / 18.f, 0.6f, 7.f) * (0.7f + 0.3f * ChargeAlpha));
}

void AArenaWeapon::AddRecoil(float Amount)
{
	RecoilOffset = FMath::Clamp(RecoilOffset + Amount, 0.f, 14.f);
}

// ------------------------------------------------------------------------------------------------
// Aim & traces
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::GetAim(FVector& OutStart, FVector& OutDir) const
{
	if (!ArenaOwner)
	{
		OutStart = GetActorLocation();
		OutDir = GetActorForwardVector();
		return;
	}
	OutStart = ArenaOwner->Camera ? ArenaOwner->Camera->GetComponentLocation() : ArenaOwner->GetPawnViewLocation();
	OutDir = ArenaOwner->GetViewRotation().Vector().GetSafeNormal();
	if (OutDir.IsNearlyZero())
	{
		OutDir = ArenaOwner->GetActorForwardVector();
	}
}

FVector AArenaWeapon::GetMuzzleLocation() const
{
	if (ArenaOwner)
	{
		if (ArenaOwner->IsViewedInFirstPerson() && GetHand() > 1.f)
		{
			// Weapon.GetEffectStart: WeaponCentered (Hidden hand) -> CenteredEffectStart, the pawn's location
			return ArenaOwner->GetActorLocation();
		}
		if (ArenaOwner->IsViewedInFirstPerson())
		{
			// First person: the imported view model's muzzle, mapped to where it appears on screen
			// (the model is drawn with its own field of view and scale).
			const FName Socket = Art && Art->MuzzleSocket ? FName(Art->MuzzleSocket) : NAME_None;
			if (ViewMesh && bViewShown && !Socket.IsNone() && ViewMesh->DoesSocketExist(Socket) && ArenaOwner->Camera)
			{
				FMinimalViewInfo View;
				ArenaOwner->Camera->GetCameraView(0.f, View);
				const FVector Socketed = ViewMesh->GetSocketLocation(Socket);
				return View.bUseFirstPersonParameters ? View.TransformWorldToFirstPerson(Socketed, true) : Socketed;
			}
		}
		else
		{
			// Seen from outside: the weapon in the model's hand
			FVector Hand;
			if (ArenaOwner->GetThirdPersonMuzzle(Hand))
			{
				return Hand;
			}
		}
	}
	if (ViewRoot)
	{
		return ViewRoot->GetComponentTransform().TransformPosition(MuzzleOffset);
	}
	return GetActorLocation();
}

bool AArenaWeapon::WeaponTrace(const FVector& Start, const FVector& End, FHitResult& OutHit, const AActor* ExtraIgnore, bool bIgnoreOwner) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaWeaponTrace), false, this);
	if (ArenaOwner && bIgnoreOwner)
	{
		QP.AddIgnoredActor(ArenaOwner);
	}
	if (ExtraIgnore)
	{
		QP.AddIgnoredActor(ExtraIgnore);
	}

	// Skip triggers / pickups / other non-solid things that happen to block the weapon channel.
	for (int32 Attempt = 0; Attempt < 6; ++Attempt)
	{
		if (!World->LineTraceSingleByChannel(OutHit, Start, End, ECC_Weapon, QP))
		{
			return false;
		}
		AActor* HitActor = OutHit.GetActor();
		UPrimitiveComponent* HitComp = OutHit.GetComponent();
		bool bSolid = true;
		static const FName ShootableTag(TEXT("ArenaShootable"));
		if (HitComp && !Cast<APawn>(HitActor) && !Cast<AArenaProjectile>(HitActor) && !(HitActor && HitActor->ActorHasTag(ShootableTag)))
		{
			bSolid = HitComp->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
		}
		if (bSolid)
		{
			return true;
		}
		if (HitActor)
		{
			QP.AddIgnoredActor(HitActor);
		}
		else if (HitComp)
		{
			QP.AddIgnoredComponent(HitComp);
		}
		else
		{
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// Fire types
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::ProcessHitscanHit(uint8 Mode, const FHitResult& Hit, const FVector& ShotDir, float BaseDamage)
{
	AActor* Victim = Hit.GetActor();
	if (!IsValid(Victim) || Mode > 1)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];

	FArenaHitInfo Info;
	Info.WeaponName = WeaponName;
	Info.Momentum = ShotDir.GetSafeNormal() * FM.Momentum;
	Info.bRadial = false;
	Info.HitLocation = Hit.ImpactPoint;

	// Goop on the ground / shock cores take the hit (UE2 TakeDamage of bProjTarget projectiles)
	if (AArenaProjectile* Proj = Cast<AArenaProjectile>(Victim))
	{
		Proj->TakeProjectileDamage(GetOwnerController(), false);
		return;
	}

	float Damage = BaseDamage;
	if (FM.HeadshotMultiplier > 0.f)
	{
		if (const AArenaCharacter* Char = Cast<AArenaCharacter>(Victim))
		{
			if (Char->IsHeadshotUE2(Hit.ImpactPoint, ShotDir))
			{
				Damage *= FM.HeadshotMultiplier;
				Info.bHeadshot = true;
			}
		}
	}
	ArenaDamage::ApplyDamage(Victim, Damage, GetOwnerController(), this, Info);
}

void AArenaWeapon::FireHitscan(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || Mode > 1)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];

	FVector Start, Dir;
	GetAim(Start, Dir);
	const FVector Muzzle = GetMuzzleLocation();
	if (FM.bReflective && ArenaOwner && ArenaOwner->IsPlayerControlled())
	{
		// ShockBeamFire.DoFireEffect: a player's beam starts at ShockProjFire's ProjSpawnOffset (24, 8, 0) UU
		// (Hand x Y; nothing sideways when the weapon is centred or hidden)
		const float Hand = GetHand();
		Start += Dir.Rotation().RotateVector(FVector(48.f, Hand > 1.f ? 0.f : 16.f * Hand, 0.f));
	}
	Dir = AdjustAimForShot(Mode, Start, Dir);
	WarnInstantTarget(Mode, Dir);

	const int32 NumShots = FMath::Max(1, FM.NumShots);
	const float SpreadRad = FMath::DegreesToRadians(FMath::Max(0.f, FM.Spread));
	const float BeamLife = FMath::Clamp(FM.BeamWidth * 0.06f, 0.05f, 0.6f);

	for (int32 i = 0; i < NumShots; ++i)
	{
		// UE2 InstantFire: DamageMin, half the time + Rand(DamageMax - DamageMin + 1)
		float Damage = FM.Damage;
		if (FM.DamageMax > FM.Damage && FMath::FRand() > 0.5f)
		{
			Damage += float(FMath::RandRange(0, int32(FM.DamageMax - FM.Damage)));
		}
		Damage *= GetDamageScale() * GetHitscanDamageScale(Mode);
		if (FM.MaxChargeTime > 0.f)
		{
			Damage *= FMath::Lerp(0.4f, 1.f, ChargeAlpha);
		}
		// UE2 InstantFire.DoFireEffect: Aim + VRand() x FRand() x Spread
		const float UE2Spread = GetHitscanSpread(Mode);
		const FVector ShotDir = UE2Spread > 0.f ? (Dir + FMath::VRand() * FMath::FRand() * UE2Spread).GetSafeNormal()
			: (SpreadRad > 0.f ? FMath::VRandCone(Dir, SpreadRad) : Dir);
		// UE2 InstantFire.DoTrace: a reflective beam that hits a raised Shield Gun goes on from the hit point along the
		// holder's view (ReflectNum < 4); after the first reflection it can hit the shooter too.
		FVector SegStart = Start;
		FVector SegDir = ShotDir;
		FVector BeamFrom = Muzzle;
		const AActor* Reflector = nullptr;
		for (int32 ReflectNum = 0; ReflectNum < 5; ++ReflectNum)
		{
			const FVector End = SegStart + SegDir * FM.Range;
			FHitResult Hit;
			const bool bHit = WeaponTrace(SegStart, End, Hit, Reflector, ReflectNum == 0);
			const FVector HitPoint = bHit ? FVector(Hit.ImpactPoint) : End;
			FVector RefNormal;
			AArenaCharacter* HitChar = bHit ? Cast<AArenaCharacter>(Hit.GetActor()) : nullptr;
			const bool bReflect = FM.bReflective && HitChar && ReflectNum < 4
				&& HitChar->CheckShieldReflect(HitPoint, RefNormal, int32(FM.Damage * 0.25f));
			if (bHit && !bReflect)
			{
				ProcessHitscanHit(Mode, Hit, SegDir, Damage);
				ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 8.f + FM.BeamWidth * 3.f, GetArtImpactStyle(Mode));
			}
			ArenaFX::SpawnBeam(World, BeamFrom, HitPoint, FM.Color, FM.BeamWidth, BeamLife, GetArtBeamStyle(Mode));
			if (ReflectNum == 0 && i == 0)
			{
				// hosting for UT2004 clients: the shot's line (beam effects, WeaponAttachment.mHitLocation)
				NetShotStart = BeamFrom;
				NetShotEnd = HitPoint;
				NetShotNormal = bHit ? FVector(Hit.ImpactNormal) : FVector::ZeroVector;
				++NetHitscanCount;
			}
			if (!bReflect)
			{
				break;
			}
			ReflectCount++;
			SegStart = HitPoint;
			SegDir = RefNormal;
			BeamFrom = HitPoint;
			Reflector = HitChar;
		}
	}
}

FVector AArenaWeapon::GetProjectileStart(uint8 Mode, FVector& OutDir)
{
	UWorld* World = GetWorld();
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const FRotator AimRot = Dir.Rotation();
	OutDir = Dir;
	if (!World || !ArenaOwner)
	{
		return Eye;
	}
	// UE2 ProjectileFire.DoFireEffect: StartProj = eye + X x ProjSpawnOffset.X + Hand x Y x ProjSpawnOffset.Y +
	// Z x ProjSpawnOffset.Z (right handed, UU x 2 = cm); the muzzle flash stays on the view model's muzzle.
	// (Hand x Y: left handed flips it, centred drops it; WeaponCentered (Hidden): only the X part)
	FVector LocalOffset = FireModes[Mode > 1 ? 0 : Mode].ProjSpawnOffset * 2.f;
	const float Hand = GetHand();
	if (Hand > 1.f)
	{
		LocalOffset.Y = LocalOffset.Z = 0.f;
	}
	else
	{
		LocalOffset.Y *= Hand;
	}
	FVector SpawnLoc = Eye + AimRot.RotateVector(LocalOffset);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaProjectileSpawn), false, this);
	QP.AddIgnoredActor(ArenaOwner);
	FCollisionResponseParams RP;
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	FHitResult Block;
	if (World->LineTraceSingleByChannel(Block, Eye, SpawnLoc, ECC_Visibility, QP, RP))
	{
		// UE2: the trace from the eye hit the world first: start at the hit (pulled back 1 cm so it isn't inside).
		SpawnLoc = Block.Location - (SpawnLoc - Eye).GetSafeNormal();
	}

	// UE2 PlayerController.AdjustAim returns the view rotation: a player's projectiles fly parallel to the view.
	// Bots (Bot.AdjustAim) aim from the projectile start at their target.
	OutDir = AdjustAimForShot(Mode, SpawnLoc, Dir);

	return SpawnLoc;
}

TArray<AArenaProjectile*> AArenaWeapon::SpawnProjectiles(const FArenaProjectileParams& InParams, int32 Count, float SpreadDeg, uint8 SpreadStyle,
	float UE2Spread, const FArenaProjectileParams* ChunkParams)
{
	TArray<AArenaProjectile*> Spawned;
	UWorld* World = GetWorld();
	if (!World || !ArenaOwner)
	{
		return Spawned;
	}

	FArenaProjectileParams P = InParams;
	P.WeaponName = WeaponName;

	FVector BaseDir;
	const FVector SpawnLoc = GetProjectileStart(SpawnMode > 1 ? 0 : SpawnMode, BaseDir);
	const int32 N = FMath::Max(1, Count);
	const float SpreadRad = FMath::DegreesToRadians(FMath::Max(0.f, SpreadDeg));
	AController* Ctrl = GetOwnerController();
	const float DamageScale = GetDamageScale();

	for (int32 i = 0; i < N; ++i)
	{
		FVector ShotDir = SpreadRad > 0.f ? FMath::VRandCone(BaseDir, SpreadRad) : BaseDir;
		if (SpreadStyle == 1 && UE2Spread > 0.f)
		{
			// UE2 ProjectileFire SS_Random: X >> R with R = Spread x (FRand() - 0.5) per rotator axis (65536 = 360 deg)
			auto Axis = [UE2Spread]() { return UE2Spread * (FMath::FRand() - 0.5f) * 360.f / 65536.f; };
			ShotDir = FRotator(Axis(), Axis(), Axis()).RotateVector(BaseDir);
		}
		else if (SpreadStyle == 2 && UE2Spread > 0.f && N > 1)
		{
			// UE2 SS_Line: theta = Spread x pi / 32768 x (P - (N - 1) / 2) around the aim's up axis
			const float Theta = UE2Spread * UE_PI / 32768.f * (float(i) - float(N - 1) * 0.5f);
			ShotDir = BaseDir.Rotation().RotateVector(FVector(FMath::Cos(Theta), FMath::Sin(Theta), 0.f));
		}
		const FTransform T(ShotDir.Rotation(), SpawnLoc);
		AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, ArenaOwner, ArenaOwner,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Proj)
		{
			Proj->Init(P, Ctrl, DamageScale);
			if (ChunkParams)
			{
				Proj->ChunkParams = *ChunkParams;
			}
			Proj->FinishSpawning(T);
			if (Spawned.Num() == 0)
			{
				WarnProjectileTarget(Proj);
				// ShockProjFire.SpawnProjectile -> ShockRifle.SetComboTarget (bots)
				if (P.bComboCore)
				{
					if (AArenaBotController* Bot = Cast<AArenaBotController>(ArenaOwner->GetController()))
					{
						Bot->SetComboCore(Proj);
					}
				}
			}
			Spawned.Add(Proj);
		}
	}
	return Spawned;
}

void AArenaWeapon::FireProjectile(uint8 Mode, float ChargeAlpha)
{
	if (Mode > 1)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	FArenaProjectileParams P = FM.Projectile;
	if (FM.MaxChargeTime > 0.f)
	{
		P.Damage *= FMath::Lerp(1.f, 2.f, ChargeAlpha);
		P.Size *= FMath::Lerp(1.f, 1.6f, ChargeAlpha);
		P.DamageRadius *= FMath::Lerp(1.f, 1.4f, ChargeAlpha);
		P.Speed *= FMath::Lerp(1.f, 1.2f, ChargeAlpha);
		P.MaxSpeed = FMath::Max(P.MaxSpeed, P.Speed);
	}
	SpawnProjectiles(P, FM.NumShots, FM.Spread, FM.SpreadStyle, FM.UE2Spread);
}

void AArenaWeapon::FireBeamTick(uint8 Mode, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || Mode > 1)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];
	FVector Start, Dir;
	GetAim(Start, Dir);
	FHitResult Hit;
	const bool bBeamHit = WeaponTrace(Start, Start + Dir * FM.Range, Hit);
	// hosting for UT2004 clients: the beam's line (LinkBeamEffect StartEffect / EndEffect)
	NetShotStart = Start;
	NetShotEnd = bBeamHit ? FVector(Hit.ImpactPoint) : Start + Dir * FM.Range;
	NetShotNormal = bBeamHit ? FVector(Hit.ImpactNormal) : FVector::ZeroVector;
	NetBeamTime = World->GetTimeSeconds();
	if (bBeamHit)
	{
		ProcessHitscanHit(Mode, Hit, Dir, FM.Damage * GetDamageScale());
		ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 6.f + FM.BeamWidth * 2.f, GetArtImpactStyle(Mode));
	}
	AddRecoil(0.4f);
}

void AArenaWeapon::UpdateLiveBeam(uint8 Mode)
{
	UWorld* World = GetWorld();
	if (!World || Mode > 1)
	{
		return;
	}
	const FArenaFireMode& FM = FireModes[Mode];

	// Imported-art beam: textured quads turned to the viewer.
	if (!bLiveBeamArtTried)
	{
		bLiveBeamArtTried = true;
		if (const ArenaArtFX::FEffectDesc* E = ArenaArtFX::FindEffect(GetArtBeamStyle(Mode)))
		{
			for (const ArenaArtFX::FSpriteDesc& D : E->Sprites)
			{
				UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
				C->SetUsingAbsoluteLocation(true);
				C->SetUsingAbsoluteRotation(true);
				C->SetUsingAbsoluteScale(true);
				C->SetupAttachment(GetRootComponent());
				if (C->SetupSprite(D))
				{
					C->RegisterComponent();
					C->SetVisibility(false);
					LiveBeamSprites.Add(C);
				}
				else
				{
					C->DestroyComponent();
				}
			}
		}
	}
	if (LiveBeamSprites.Num() > 0)
	{
		FVector Start, Dir;
		GetAim(Start, Dir);
		FHitResult Hit;
		const FVector End = Start + Dir * FM.Range;
		const FVector HitPoint = WeaponTrace(Start, End, Hit) ? FVector(Hit.ImpactPoint) : End;
		const FVector Muzzle = GetMuzzleLocation();
		const float Now = GetWorldTime();
		for (UArenaSpriteComponent* C : LiveBeamSprites)
		{
			if (C)
			{
				C->SetIntensityScale(0.85f + 0.15f * FMath::Sin(Now * 40.f));
				C->SetBeam(Muzzle, HitPoint);
				C->SetVisibility(true);
			}
		}
		if (Now >= NextLiveBeamFlash)
		{
			NextLiveBeamFlash = Now + 0.12f;
			SpawnViewMuzzleFlash(FM.Color, 1.f, Mode);
		}
		bBeamActive = true;
		return;
	}

	if (!LiveBeam)
	{
		LiveBeam = NewObject<UStaticMeshComponent>(this, TEXT("LiveBeam"));
		LiveBeam->SetStaticMesh(ArenaAssets::Cylinder());
		LiveBeam->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		LiveBeam->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		LiveBeam->SetGenerateOverlapEvents(false);
		LiveBeam->SetCastShadow(false);
		LiveBeam->SetCanEverAffectNavigation(false);
		LiveBeam->SetUsingAbsoluteLocation(true);
		LiveBeam->SetUsingAbsoluteRotation(true);
		LiveBeam->SetUsingAbsoluteScale(true);
		LiveBeam->RegisterComponent();
		LiveBeamMaterial = ArenaAssets::MakeGlow(this, FM.Color, 10.f, 0.9f);
		if (LiveBeamMaterial)
		{
			LiveBeam->SetMaterial(0, LiveBeamMaterial);
		}
	}
	if (LiveBeamMaterial)
	{
		LiveBeamMaterial->SetVectorParameterValue(TEXT("Color"), FM.Color);
	}

	FVector Start, Dir;
	GetAim(Start, Dir);
	FHitResult Hit;
	const FVector End = Start + Dir * FM.Range;
	const FVector HitPoint = WeaponTrace(Start, End, Hit) ? FVector(Hit.ImpactPoint) : End;

	const float Now = GetWorldTime();
	const float Width = FM.BeamWidth * (1.f + 0.2f * FMath::Sin(Now * 45.f));
	ArenaFX::OrientBeamMesh(LiveBeam, GetMuzzleLocation(), HitPoint, Width);
	if (!LiveBeam->IsVisible())
	{
		LiveBeam->SetVisibility(true);
	}
	bBeamActive = true;
}

void AArenaWeapon::HideLiveBeam()
{
	if (LiveBeam)
	{
		LiveBeam->SetVisibility(false);
	}
	for (UArenaSpriteComponent* C : LiveBeamSprites)
	{
		if (C)
		{
			C->SetVisibility(false);
		}
	}
	bBeamActive = false;
}

void AArenaWeapon::FireMelee(uint8 Mode, float ChargeAlpha)
{
	UWorld* World = GetWorld();
	if (!World || Mode > 1 || !ArenaOwner)
	{
		return;
	}
	// XWeapons.ShieldFire.DoFireEffect. FM.Damage / FM.Momentum are the fully charged values (MaxDamage 150,
	// MaxForce 100000 = 2000 cm/s); MinDamage 40 and MinForce 65000 scale with them. The charge counts from
	// MinHoldTime 0.4 s to FullyChargedTime (FM.MaxChargeTime, 2.5 s).
	const FArenaFireMode& FM = FireModes[Mode];
	constexpr float MinHoldTime = 0.4f;
	const float FullTime = FMath::Max(FM.MaxChargeTime, MinHoldTime + 0.1f);
	const float HoldTime = FMath::Clamp(ChargeAlpha, 0.f, 1.f) * FullTime;
	const float Scale = (FMath::Clamp(HoldTime, MinHoldTime, FullTime) - MinHoldTime) / (FullTime - MinHoldTime);
	const float Damage = FMath::Lerp(FM.Damage * (40.f / 150.f), FM.Damage, Scale);
	float Force = FMath::Lerp(FM.Momentum * 0.65f, FM.Momentum, Scale);
	const float Charge = 0.35f + 0.65f * Scale;

	FVector EyeStart, Dir;
	GetAim(EyeStart, Dir);
	// UE2 traces ShieldRange from the pawn's centre (Instigator.Location), not from the eyes.
	const FVector Start = ArenaOwner->GetActorLocation();
	const float Range = FMath::Max(60.f, FM.Range);

	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaMelee), false, this);
	QP.AddIgnoredActor(ArenaOwner);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Start, Start + Dir * Range, ECC_Weapon, QP))
	{
		return;
	}
	AActor* Other = Hit.GetActor();
	if (IsValid(Other) && Cast<APawn>(Other))
	{
		// Hit someone: TakeDamage(Damage, Force * (X + (0,0,0.5)))
		FArenaHitInfo Info;
		Info.WeaponName = WeaponName;
		Info.Momentum = Force * (Dir + FVector(0.f, 0.f, 0.5f));
		Info.HitLocation = Hit.ImpactPoint;
		ArenaDamage::ApplyDamage(Other, Damage * GetDamageScale(), GetOwnerController(), this, Info);
		ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 20.f + 30.f * Charge, GetArtImpactStyle(Mode));
		ArenaAssets::PlaySoundAt(this, TEXT("S_Impact"), Hit.ImpactPoint, 0.9f, 0.8f);
		return;
	}

	// Hit the world: the shield jump. Self damage MinSelfDamage 8 + SelfDamageScale 0.3 x Damage, momentum
	// -SelfForceScale x Force along the aim (the pawn applies the UE2 self 0.6 and AddVelocity rules). Berserk doubles it.
	if (ArenaOwner->GetActiveCombo() == EArenaCombo::Berserk)
	{
		Force *= 2.f;
	}
	FArenaHitInfo Self;
	Self.WeaponName = WeaponName;
	Self.Momentum = -Force * Dir;
	ArenaDamage::ApplyDamage(ArenaOwner, (8.f + 0.3f * Damage) * GetDamageScale(), GetOwnerController(), this, Self);
	// Damageable game objects (UE2 DestroyableObjective) are hit as well.
	if (IsValid(Other) && Other->CanBeDamaged() && !Other->IsRootComponentStatic() && !Cast<AArenaProjectile>(Other) && !Cast<AArenaMover>(Other))
	{
		FArenaHitInfo Info;
		Info.WeaponName = WeaponName;
		Info.Momentum = Force * (Dir + FVector(0.f, 0.f, 0.5f));
		ArenaDamage::ApplyDamage(Other, Damage * GetDamageScale(), GetOwnerController(), this, Info);
	}
	ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, FM.Color, 30.f + 40.f * Charge, GetArtImpactStyle(Mode));
	ArenaAssets::PlaySoundAt(this, TEXT("S_Impact"), Hit.ImpactPoint, 0.8f, 0.7f);
}

// ------------------------------------------------------------------------------------------------
// Bots
// ------------------------------------------------------------------------------------------------

FVector AArenaWeapon::AdjustAimForShot(uint8 Mode, const FVector& Start, const FVector& Dir)
{
	ShotTarget.Reset();
	if (!ArenaOwner || Mode > 1)
	{
		return Dir;
	}
	if (AArenaBotController* Bot = Cast<AArenaBotController>(ArenaOwner->GetController()))
	{
		// Bot.AdjustAim: ShotTarget = the enemy
		ShotTarget = Bot->GetEnemy();
		return Bot->AdjustAimUE2(this, Mode, Start, Dir);
	}
	// PlayerController.AdjustAim (no aiming help): a pawn under the crosshair (10000 UU instant, 4000 UU projectiles),
	// else PickTarget with aim 0.9 for projectiles; the shot itself keeps the view direction.
	const bool bInstant = FireModes[Mode].FireType != EArenaFireType::Projectile;
	const float Range = bInstant ? 20000.f : 8000.f;
	FHitResult Hit;
	if (WeaponTrace(Start, Start + Dir * Range, Hit))
	{
		if (AArenaCharacter* C = Cast<AArenaCharacter>(Hit.GetActor()))
		{
			ShotTarget = C;
			return Dir;
		}
	}
	if (!bInstant)
	{
		const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
		float BestAim = 0.9f;
		for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
		{
			AArenaCharacter* C = *It;
			if (C == ArenaOwner || C->IsDead() || (GM && GM->IsTeamGame() && GM->AreOnSameTeam(ArenaOwner->GetController(), C->GetController())))
			{
				continue;
			}
			const FVector To = C->GetActorLocation() - Start;
			const float Dist = To.Size();
			const float Aim = FVector::DotProduct(To / FMath::Max(Dist, 1.f), Dir);
			if (Dist > Range || Aim <= BestAim)
			{
				continue;
			}
			FHitResult Block;
			if (WeaponTrace(Start, C->GetActorLocation(), Block) && Block.GetActor() != C)
			{
				continue;
			}
			BestAim = Aim;
			ShotTarget = C;
		}
	}
	return Dir;
}

void AArenaWeapon::WarnInstantTarget(uint8 Mode, const FVector& Dir)
{
	AArenaCharacter* T = ShotTarget.Get();
	AArenaBotController* TargetBot = T ? Cast<AArenaBotController>(T->GetController()) : nullptr;
	if (TargetBot && Mode <= 1 && FMath::FRand() < FireModes[Mode].WarnTargetPct)
	{
		TargetBot->ReceiveWarning(ArenaOwner, -1.f, Dir);
	}
}

void AArenaWeapon::WarnProjectileTarget(AArenaProjectile* Proj)
{
	AArenaCharacter* T = ShotTarget.Get();
	if (AArenaBotController* TargetBot = T ? Cast<AArenaBotController>(T->GetController()) : nullptr)
	{
		TargetBot->ReceiveProjectileWarning(Proj);
	}
}

uint8 AArenaWeapon::GetBotFireMode(float DistanceToEnemy) const
{
	float BestScore = -1.f;
	uint8 BestMode = 0;
	for (uint8 M = 0; M < 2; ++M)
	{
		const FArenaFireMode& FM = FireModes[M];
		if (FM.FireType == EArenaFireType::Zoom || FM.FireType == EArenaFireType::Shield || !HasAmmoFor(M))
		{
			continue;
		}

		const bool bProj = FM.FireType == EArenaFireType::Projectile;
		const float Dmg = (bProj ? FM.Projectile.Damage : FM.Damage) * FMath::Max(1, FM.NumShots);
		float Score = Dmg / FMath::Max(0.05f, FM.RefireTime);
		float MaxRange = FM.Range;

		if (bProj)
		{
			const float Speed = FMath::Max(100.f, FM.Projectile.Speed);
			MaxRange = Speed * FMath::Max(0.2f, FM.Projectile.LifeSpan);
			const float Travel = DistanceToEnemy / Speed;
			Score *= FMath::Clamp(1.4f - Travel, 0.25f, 1.2f);
			if (FM.Projectile.DamageRadius > 150.f && DistanceToEnemy < FM.Projectile.DamageRadius)
			{
				Score *= 0.35f; // don't splash ourselves
			}
			if (FM.Projectile.bComboCore)
			{
				Score *= 0.5f;
			}
		}
		if (FM.Spread > 2.f && !bProj)
		{
			Score *= FMath::Clamp(1.f - DistanceToEnemy / 4000.f, 0.2f, 1.f);
		}
		if (FM.FireType == EArenaFireType::Melee)
		{
			Score = DistanceToEnemy < MaxRange + 40.f ? FM.Damage * 2.f : 0.f;
		}
		if (DistanceToEnemy > MaxRange * 0.95f)
		{
			Score *= 0.02f;
		}
		if (Score > BestScore)
		{
			BestScore = Score;
			BestMode = M;
		}
	}
	return BestMode;
}

float AArenaWeapon::GetBotRating(float DistanceToEnemy) const
{
	if (!HasAmmo())
	{
		return 0.f;
	}
	const uint8 Mode = GetBotFireMode(DistanceToEnemy);
	const FArenaFireMode& FM = FireModes[Mode];
	float Rating = Priority;

	switch (FM.FireType)
	{
	case EArenaFireType::Melee:
		Rating = DistanceToEnemy < FM.Range + 60.f ? Priority + 0.3f : 0.02f;
		break;
	case EArenaFireType::Projectile:
	{
		const float Speed = FMath::Max(100.f, FM.Projectile.Speed);
		const float Travel = DistanceToEnemy / Speed;
		if (Travel > 0.6f)
		{
			Rating *= FMath::Clamp(1.2f - (Travel - 0.6f) * 0.5f, 0.35f, 1.f);
		}
		if (FM.Projectile.DamageRadius > 150.f && DistanceToEnemy < FM.Projectile.DamageRadius * 0.8f)
		{
			Rating *= 0.55f;
		}
		break;
	}
	case EArenaFireType::Hitscan:
		if (FM.Spread > 2.f && DistanceToEnemy > 2500.f)
		{
			Rating *= 0.7f;
		}
		if (FM.HeadshotMultiplier > 0.f && DistanceToEnemy > 2500.f)
		{
			Rating *= 1.2f;
		}
		break;
	case EArenaFireType::Beam:
		Rating *= DistanceToEnemy < FM.Range ? 1.15f : 0.4f;
		break;
	default:
		break;
	}

	if (!bInfiniteAmmo && MaxAmmo > 0)
	{
		// Slight preference for weapons that won't run dry immediately.
		Rating *= FMath::Lerp(0.8f, 1.f, FMath::Clamp(GetAmmoFraction() * 4.f, 0.f, 1.f));
	}
	return Rating;
}

// ------------------------------------------------------------------------------------------------
// View model
// ------------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaWeapon::AddViewPart(UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& Scale, const FLinearColor& Color, bool bGlow)
{
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	if (!Part)
	{
		return nullptr;
	}
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(ViewRoot);
	if (bMirrorViewParts)
	{
		Part->SetRelativeLocationAndRotation(FVector(Location.X, -Location.Y, Location.Z), FRotator(Rotation.Pitch, -Rotation.Yaw, -Rotation.Roll));
	}
	else
	{
		Part->SetRelativeLocationAndRotation(Location, Rotation);
	}
	Part->SetRelativeScale3D(Scale);
	Part->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCastShadow(false);
	Part->SetCanEverAffectNavigation(false);
	Part->SetOnlyOwnerSee(true);
	Part->SetReceivesDecals(false);
	UMaterialInstanceDynamic* MID = bGlow
		? ArenaAssets::MakeGlow(this, Color, 6.f, 1.f)
		: ArenaAssets::MakeSurface(this, Color, 0.35f, 0.6f, Color * 0.04f, 0.f);
	if (MID)
	{
		Part->SetMaterial(0, MID);
	}
	Part->SetVisibility(bEquipped);
	Part->RegisterComponent();
	AddInstanceComponent(Part);
	ViewParts.Add(Part);
	return Part;
}

void AArenaWeapon::BuildViewModel()
{
	// Generic fallback model: body, barrel and a glow strip.
	const FLinearColor Dark = WeaponColor * 0.5f;
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 14.f, -16.f), FRotator::ZeroRotator, FVector(0.36f, 0.07f, 0.09f), WeaponColor);
	AddViewPart(ArenaAssets::Cylinder(), FVector(50.f, 14.f, -12.f), FRotator(90.f, 0.f, 0.f), FVector(0.04f, 0.04f, 0.2f), Dark);
	AddViewPart(ArenaAssets::Cube(), FVector(28.f, 14.f, -11.f), FRotator::ZeroRotator, FVector(0.28f, 0.02f, 0.01f), FireModes[0].Color, true);
}

// ------------------------------------------------------------------------------------------------
// Imported view model
// ------------------------------------------------------------------------------------------------

void AArenaWeapon::SetupImportedViewModel()
{
	Art = ArenaArt::FindWeaponArt(GetClass());
	USkeletalMesh* Mesh = Art && Art->FirstPersonMesh ? ArenaArt::Load<USkeletalMesh>(Art->FirstPersonMesh) : nullptr;
	if (!Mesh || !ViewRoot)
	{
		return; // keep the procedural view model
	}
	ViewMesh = NewObject<USkeletalMeshComponent>(this, TEXT("ViewMesh"));
	ViewMesh->SetupAttachment(ViewRoot);
	ViewMesh->SetSkeletalMeshAsset(Mesh);
	ViewMesh->SetRelativeLocationAndRotation(Art->ViewOffset, Art->ViewRotation);
	ViewMesh->SetRelativeScale3D(FVector(Art->ViewScale));
	ViewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ViewMesh->SetGenerateOverlapEvents(false);
	ViewMesh->SetCanEverAffectNavigation(false);
	ViewMesh->SetCastShadow(false);
	ViewMesh->SetReceivesDecals(false);
	ViewMesh->SetOnlyOwnerSee(true);
	// Drawn with its own field of view and scaled towards the camera so it never pokes into walls.
	ViewMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
	ViewMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	ViewMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	ViewMesh->bComponentUseFixedSkelBounds = true;
	ViewMesh->SetVisibility(bEquipped);
	ViewMesh->SetHiddenInGame(!bEquipped);
	ViewMesh->RegisterComponent();
	AddInstanceComponent(ViewMesh);

	// The procedural parts stay as a fallback but are not drawn.
	for (UStaticMeshComponent* Part : ViewParts)
	{
		if (Part)
		{
			Part->SetHiddenInGame(true);
		}
	}
	PlayViewAnim(Art->IdleAnim, true);
	if (bEquipped)
	{
		bViewShown = true;
		ApplyViewFOV();
	}
}

UAnimSequence* AArenaWeapon::FindViewAnim(const TCHAR* Name)
{
	if (!Name || !Art || !Art->AnimPrefix || !ViewMesh)
	{
		return nullptr;
	}
	const FName Key(Name);
	if (TObjectPtr<UAnimSequence>* Found = ViewAnims.Find(Key))
	{
		return *Found;
	}
	UAnimSequence* Seq = ArenaArt::Load<UAnimSequence>(FString(Art->AnimPrefix) + Name);
	const USkeletalMesh* Mesh = ViewMesh->GetSkeletalMeshAsset();
	if (Seq && Mesh && Seq->GetSkeleton() != Mesh->GetSkeleton())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("%s: view animation %s does not match the mesh skeleton"), *GetName(), *Seq->GetPathName());
		Seq = nullptr;
	}
	ViewAnims.Add(Key, Seq);
	return Seq;
}

bool AArenaWeapon::PlayViewAnim(const TCHAR* Name, bool bLoop, float Duration, float MaxTime)
{
	UAnimSequence* Seq = FindViewAnim(Name);
	if (!Seq || !ViewMesh)
	{
		return false;
	}
	const float Length = FMath::Max(Seq->GetPlayLength(), 0.01f);
	float Rate = 1.f;
	if (Duration > 0.f)
	{
		Rate = Length / Duration;
	}
	else if (MaxTime > 0.f && Length > MaxTime)
	{
		Rate = Length / MaxTime;
	}
	ViewMesh->PlayAnimation(Seq, bLoop);
	ViewMesh->SetPlayRate(Rate);
	ViewAnimEndTime = bLoop ? TNumericLimits<float>::Max() : GetWorldTime() + Length / Rate;
	return true;
}

void AArenaWeapon::TickViewModel(float DeltaSeconds)
{
	if (!ViewMesh)
	{
		return;
	}
	const float Now = GetWorldTime();
	if (!bEquipped)
	{
		if (PutDownEndTime > 0.f && Now >= PutDownEndTime)
		{
			PutDownEndTime = -1.f;
			ViewMesh->SetVisibility(false);
		}
		return;
	}
	if (!bViewShown)
	{
		if (Now >= ShowViewTime)
		{
			bViewShown = true;
			ViewMesh->SetHiddenInGame(false);
			ViewMesh->SetVisibility(true);
			if (!PlayViewAnim(Art->SelectAnim, false, 0.3f))
			{
				PlayViewAnim(Art->IdleAnim, true);
			}
		}
		return;
	}
	if (HoldAnimMode == 255 && Now >= ViewAnimEndTime)
	{
		PlayViewAnim(Art->IdleAnim, true);
	}
	ApplyHandToViewModel();
	// Only drawn (and animated) while someone looks through the owner's eyes; Weapon.RenderOverlays draws nothing for
	// the Hidden hand (Hand > 1)
	ViewMesh->SetHiddenInGame(!ArenaOwner || !ArenaOwner->IsViewedInFirstPerson() || GetHand() > 1.f);
}

float AArenaWeapon::GetHand() const
{
	return ArenaWeaponHand::GetHandedness(ArenaOwner ? ArenaOwner->GetController() : nullptr);
}

void AArenaWeapon::ApplyHandToViewModel()
{
	if (!ViewMesh || !Art)
	{
		return;
	}
	const float Hand = GetHand();
	const bool bSmall = ArenaWeaponHand::UseSmallWeapons(ArenaOwner ? ArenaOwner->GetController() : nullptr);
	if (Hand == AppliedHand && int8(bSmall) == AppliedSmall)
	{
		return;
	}
	AppliedHand = Hand;
	AppliedSmall = int8(bSmall);
	// Weapon.RenderOverlays: PlayerViewOffset (SmallViewOffset with small weapons); Y x Hand, or CenteredOffsetY when
	// centred; DrawScale3D.Y x Hand and the pivot's yaw / roll x Hand; centred: view yaw + CenteredYaw, roll CenteredRoll
	const ArenaWeaponHand::FViewParams& P = ArenaWeaponHand::GetViewParams(GetClass());
	const float ToView = (90.f / FMath::Max(1.f, Art->ViewFOV)) * 2.f; // FWeaponArt::SetViewFromSource
	FVector Source = P.bValid ? (bSmall ? P.SmallOffset : P.Offset) : Art->ViewOffset / ToView;
	FRotator Pivot = Art->ViewRotation;
	FVector Scale(Art->ViewScale);
	float H = Hand > 1.f ? 1.f : Hand;
	if (H == 0.f)
	{
		// AssaultRifle.RenderOverlays: dual rifles are never centred, the first one goes left
		const AWeap_Carbine* Dual = Cast<AWeap_Carbine>(this);
		if (Dual && Dual->IsDualMode())
		{
			H = -1.f;
		}
	}
	if (H == 0.f)
	{
		Source.Y = P.bValid ? P.CenteredOffsetY : 0.f;
		const float S = 360.f / 65536.f;
		const FRotator Centered(0.f, P.CenteredYaw * S, P.CenteredRoll * S);
		Pivot = (FQuat(Centered) * FQuat(Pivot)).Rotator();
	}
	else
	{
		Source.Y *= H;
		Scale.Y *= H;
		Pivot.Yaw *= H;
		Pivot.Roll *= H;
	}
	ViewMesh->SetRelativeLocationAndRotation(Source * ToView, Pivot);
	ViewMesh->SetRelativeScale3D(Scale);
}

void AArenaWeapon::ApplyViewFOV()
{
	if (!ViewMesh || !Art || !ArenaOwner || !ArenaOwner->Camera)
	{
		return;
	}
	UCameraComponent* Cam = ArenaOwner->Camera;
	// ViewFOV is horizontal for a 4:3 screen. The first person FOV is relative to the camera's own aspect (the engine
	// scales the view model by tan(FOV/2) / tan(FirstPersonFOV/2), then keeps the vertical extent on any screen), so
	// express the same vertical extent as a horizontal angle at the camera's AspectRatio. The gun then keeps its
	// UT2004 size on 16:9, 21:9 and 32:9 alike (Hor+: wider screens just show more around it), independent of the
	// player's FOV and of the 32:9 FOV cap.
	const float CamAspect = Cam->AspectRatio > 0.f ? Cam->AspectRatio : 16.f / 9.f;
	const float HalfRad = FMath::DegreesToRadians(Art->ViewFOV * 0.5f);
	const float FOV = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(HalfRad) * 0.75f * CamAspect));
	Cam->SetEnableFirstPersonFieldOfView(true);
	Cam->SetFirstPersonFieldOfView(FMath::Clamp(FOV, 30.f, 150.f));
	Cam->SetEnableFirstPersonScale(true);
	Cam->SetFirstPersonScale(0.4f);
}

void AArenaWeapon::PlayFireSound(uint8 Mode, const FVector& Location, float Volume, float Pitch)
{
	if (Mode > 1)
	{
		return;
	}
	if (Art && Art->FireSound[Mode])
	{
		if (USoundBase* Sound = ArenaArt::LoadOneShotSound(Art->FireSound[Mode]))
		{
			const float Now = GetWorldTime();
			if (Now - LastImportedSoundTime[Mode] >= Art->SoundInterval[Mode])
			{
				LastImportedSoundTime[Mode] = Now;
				UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, Volume, Pitch);
			}
			return;
		}
	}
	ArenaAssets::PlaySoundAt(this, FireModes[Mode].FireSound, Location, Volume, Pitch);
}

FName AArenaWeapon::GetArtImpactStyle(uint8 Mode) const
{
	const ArenaArtFX::FWeaponFX* F = ArenaArtFX::FindWeaponFX(GetClass(), Mode);
	return F && F->Impact ? FName(F->Impact) : NAME_None;
}

FName AArenaWeapon::GetArtBeamStyle(uint8 Mode) const
{
	const ArenaArtFX::FWeaponFX* F = ArenaArtFX::FindWeaponFX(GetClass(), Mode);
	return F && F->Beam ? FName(F->Beam) : NAME_None;
}

void AArenaWeapon::SpawnViewMuzzleFlash(const FLinearColor& Color, float Scale, uint8 Mode)
{
	const ArenaArtFX::FWeaponFX* WFX = ArenaArtFX::FindWeaponFX(GetClass(), Mode);
	const FName Style = WFX && WFX->Muzzle ? FName(WFX->Muzzle) : NAME_None;
	const bool bMelee = Mode < 2 && FireModes[Mode].FireType == EArenaFireType::Melee;
	const float ArtScale = (WFX ? WFX->MuzzleScale : 1.f) * (bMelee ? FMath::Clamp(Scale * 0.6f, 0.6f, 2.f) : 1.f);

	// Seen from outside: the imported flash on the third-person weapon's tip.
	if (!Style.IsNone() && ArenaOwner)
	{
		if (USkeletalMeshComponent* TP = ArenaOwner->GetThirdPersonWeapon())
		{
			static const FName Tip(TEXT("tip"));
			if (TP->IsVisible() && TP->DoesSocketExist(Tip))
			{
				const FVector Local = TP->GetSocketTransform(Tip, RTS_Component).GetLocation();
				const bool bOwnerInFirstPerson = ArenaOwner->IsViewedInFirstPerson();
				ArenaArtFX::SpawnAttachedEffect(TP, Style, Local, ArtScale * 1.5f, false, bOwnerInFirstPerson, false);
			}
		}
	}

	const FName Socket = Art && Art->MuzzleSocket ? FName(Art->MuzzleSocket) : NAME_None;
	if (ViewMesh && bViewShown && !Socket.IsNone() && ViewMesh->DoesSocketExist(Socket))
	{
		const FVector Local = ViewMesh->GetSocketTransform(Socket, RTS_Component).GetLocation();
		if (!Style.IsNone() && ArenaArtFX::SpawnAttachedEffect(ViewMesh, Style, Local, ArtScale, true, false, true))
		{
			return;
		}
		if (AArenaFXActor* FX = ArenaFX::SpawnMuzzleFlash(ViewMesh, Color, Local, Scale, true))
		{
			// Drawn in the view model's space so it sits on the barrel.
			TInlineComponentArray<UPrimitiveComponent*> Prims(FX);
			for (UPrimitiveComponent* P : Prims)
			{
				P->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
			}
		}
		return;
	}
	if (!ViewMesh)
	{
		ArenaFX::SpawnMuzzleFlash(ViewRoot, Color, MuzzleOffset, Scale, true, Style);
	}
}

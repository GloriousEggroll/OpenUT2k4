#include "ArenaWeaponActors.h"
#include "ArenaUE2Effects.h"
#include "ArenaViewShake.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaAssets.h"
#include "ArenaBotController.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaEmitters.h"
#include "ArenaFX.h"
#include "ArenaGameMode.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaPlayerState.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaWorldZones.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"

using namespace ArenaWeaponActors;

int32 ArenaWeaponActors::TeamOf(const AController* C)
{
	const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
	return PS && PS->Team >= 0 ? PS->Team : 255;
}

bool ArenaWeaponActors::SameTeam(const UWorld* World, const AController* A, const AController* B)
{
	const AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	return A && B && GM && GM->IsTeamGame() && GM->AreOnSameTeam(A, B);
}

namespace ArenaWeaponActorsLocal
{
	/** The tag that lets weapon traces hit these actors (they don't block pawns). */
	const FName ShootableTag(TEXT("ArenaShootable"));

	/** Shootable, non-blocking projectile collision: blocks the world, weapon traces and projectiles, overlaps pawns. */
	void SetupCollision(USphereComponent* C, float Radius)
	{
		C->InitSphereRadius(Radius);
		C->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		C->SetCollisionObjectType(ECC_WorldDynamic);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		C->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		C->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		C->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
		C->SetCollisionResponseToChannel(ECC_Projectile, ECR_Block);
		C->SetGenerateOverlapEvents(true);
		C->SetCanEverAffectNavigation(false);
		C->SetShouldUpdatePhysicsVolume(false);
	}

	UStaticMeshComponent* AddArtMesh(AActor* Owner, USceneComponent* Parent, const TCHAR* Path, float SizeCm, const FLinearColor& FallbackColor)
	{
		ArenaArtFX::FMeshDesc D;
		D.Mesh = Path;
		D.Size = SizeCm;
		if (UStaticMeshComponent* C = ArenaArtFX::AddMesh(Owner, Parent, D))
		{
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			return C;
		}
		// procedural stand-in
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
		C->SetStaticMesh(ArenaAssets::Sphere());
		C->SetupAttachment(Parent);
		C->SetRelativeScale3D(FVector(SizeCm / 100.f));
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (UMaterialInstanceDynamic* M = ArenaAssets::MakeGlow(Owner, FallbackColor, 4.f, 1.f))
		{
			C->SetMaterial(0, M);
		}
		C->RegisterComponent();
		return C;
	}

	void PlayArtSound(const AActor* At, const TCHAR* Path, float Volume = 1.f)
	{
		if (USoundBase* S = ArenaArt::LoadOneShotSound(Path))
		{
			UGameplayStatics::PlaySoundAtLocation(At, S, At->GetActorLocation(), Volume);
		}
	}

	/** Damage from DamageCauser's own class (chain reactions), for the grenade's "MyDamageType" checks. */
	bool SameKind(const AActor* Self, const AActor* Causer)
	{
		return Causer && Self && Causer->GetClass() == Self->GetClass();
	}

	/** UE2 HurtRadius stages of the Redeemer / ion cannon (2000 UU, the rings at 0.125 .. 1). */
	TArray<AArenaStagedBlast::FStage> NukeStages(float T0)
	{
		const float Fr[] = { 0.125f, 0.3f, 0.475f, 0.65f, 0.825f, 1.f };
		const float Dt[] = { 0.f, 0.5f, 0.2f, 0.2f, 0.2f, 0.2f };
		TArray<AArenaStagedBlast::FStage> S;
		float T = T0;
		for (int32 i = 0; i < 6; ++i)
		{
			T += Dt[i];
			AArenaStagedBlast::FStage St;
			St.Time = T;
			St.RadiusFrac = Fr[i];
			S.Add(St);
		}
		return S;
	}
}
using namespace ArenaWeaponActorsLocal;

bool AArenaWarheadShell::SpawnNukeEffects(UWorld* World, const FVector& At, const FVector& Velocity)
{
	if (!World)
	{
		return false;
	}
	// RedeemerProjectile / RedeemerWarhead Dying.ShakeView: DamageRadius 2000 UU, full within it, fading out to 2x
	ArenaViewShake::ShakeViewersNear(World, At, 2000.f * UU, 2.f, ArenaViewShake::Blast());
	const FVector Back = Velocity.IsNearlyZero() ? FVector::ZeroVector : Velocity.GetSafeNormal() * 100.f * UU;
	const bool bExplosion = ArenaUE2Effects::SpawnEmitter(World, TEXT("XEffects.RedeemerExplosion"), At - Back, ArenaUE2Effects::UE2Rot(0, 16384, 0)) != nullptr;
	if (bExplosion)
	{
		ArenaUE2Effects::SpawnSpriteXEmitter(World, TEXT("XEffects.IonCore"), At);
	}
	return bExplosion;
}

// =================================================================================================
// Staged blast
// =================================================================================================

AArenaStagedBlast::AArenaStagedBlast()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetCanBeDamaged(false);
}

void AArenaStagedBlast::Start(const FVector& Location, float InDamage, float InRadius, float InMomentum, AController* InInstigator, const FString& InWeaponName,
	const TArray<FStage>& InStages, bool bInNuke)
{
	SetActorLocation(Location);
	Damage = InDamage;
	Radius = InRadius;
	Momentum = InMomentum;
	InstigatorController = InInstigator;
	WeaponName = InWeaponName;
	Stages = InStages;
	bNuke = bInNuke;
	NextStage = 0;
	StartTime = GetWorld()->GetTimeSeconds();
}

void AArenaStagedBlast::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	const float T = World->GetTimeSeconds() - StartTime;
	while (Stages.IsValidIndex(NextStage) && T >= Stages[NextStage].Time)
	{
		const float R = Radius * Stages[NextStage].RadiusFrac;
		ArenaDamage::ApplyRadialDamage(World, GetActorLocation(), Damage, R, Momentum, InstigatorController.Get(), this, WeaponName);
		// the rings are UT2004's RedeemerExplosion / NewIonEffect emitters (ArenaUE2Effects); without the stock classes a
		// procedural ring of fire per stage
		if (!bUE2Effect)
		{
			ArenaFX::SpawnExplosion(World, GetActorLocation(), bNuke ? FLinearColor(1.f, 0.55f, 0.2f) : FLinearColor(0.5f, 0.7f, 1.f), R * 0.6f);
		}
		++NextStage;
	}
	if (!Stages.IsValidIndex(NextStage))
	{
		Destroy();
	}
}

// =================================================================================================
// Warhead shell (RedeemerProjectile)
// =================================================================================================

AArenaWarheadShell::AArenaWarheadShell()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetupCollision(Collision, 24.f * UU);
	RootComponent = Collision;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Collision);
	Light->SetIntensity(4000.f);
	Light->SetAttenuationRadius(600.f);
	Light->SetLightColor(FLinearColor(1.f, 0.6f, 0.3f));
	Light->SetCastShadows(false);
	FlightSound = CreateDefaultSubobject<UAudioComponent>(TEXT("FlightSound"));
	FlightSound->SetupAttachment(Collision);
	FlightSound->bAutoActivate = false;
	SetCanBeDamaged(true);
	Tags.Add(ShootableTag);
	InitialLifeSpan = 20.f; // RedeemerProjectile LifeSpan
}

void AArenaWarheadShell::Launch(AController* InInstigator, const FString& InWeaponName, const FVector& Dir)
{
	InstigatorController = InInstigator;
	InstigatorPawn = InInstigator ? InInstigator->GetPawn() : nullptr;
	WeaponName = InWeaponName;
	Velocity = Dir.GetSafeNormal() * 1000.f * UU; // Speed 1000
	SetActorRotation(Dir.Rotation());
	if (APawn* P = InstigatorPawn.Get())
	{
		Collision->IgnoreActorWhenMoving(P, true);
	}
	Collision->OnComponentBeginOverlap.AddDynamic(this, &AArenaWarheadShell::OnOverlap);
	Mesh = AddArtMesh(this, Collision, TEXT("StaticMeshes/WeaponStaticMesh/RedeemerMissile"), 150.f, FLinearColor(1.f, 0.6f, 0.3f));
	// PostBeginPlay: SmokeTrail = Spawn(class'NewRedeemerTrail', self,, Location - 40 * Dir, Rotation); SetBase(self)
	if (AArenaEmitter* T = ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("XEffects.NewRedeemerTrail"), GetActorLocation() - Dir.GetSafeNormal() * 40.f * UU, Dir.Rotation(), 30.f))
	{
		T->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		SmokeTrail = T;
	}
	NextBotWarnTime = GetWorld()->GetTimeSeconds() + 0.5f; // BeginPlay: SetTimer(0.5, true)
	if (USoundBase* S = ArenaArt::Load<USoundBase>(TEXT("Sounds/WeaponSounds/Misc_redeemer_flight")))
	{
		FlightSound->SetSound(S);
		FlightSound->Play();
	}
}

void AArenaWarheadShell::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bBlown)
	{
		return;
	}
	if (FlightSound && FlightSound->Sound && !FlightSound->IsPlaying())
	{
		FlightSound->Play(); // AmbientSound loops
	}
	if (Mesh)
	{
		Mesh->AddLocalRotation(FRotator(0.f, 0.f, 50000.f * 360.f / 65536.f * DeltaSeconds)); // RotationRate roll 50000
	}
	FHitResult Hit;
	SetActorLocation(GetActorLocation() + Velocity * DeltaSeconds, true, &Hit);
	if (Hit.bBlockingHit)
	{
		BlowUp(Hit.Location);
		return;
	}
	if (GetWorld()->GetTimeSeconds() >= NextBotWarnTime)
	{
		NextBotWarnTime += 0.5f;
		WarnBots();
	}
}

void AArenaWarheadShell::WarnBots()
{
	// RedeemerProjectile.Timer: AIController(C) != None && C.Pawn != None && C.GetTeamNum() != Team && Skill >= 2 &&
	// !C.Pawn.IsFiring() && (C.Enemy == None || !C.LineOfSightTo(C.Enemy)) && C.Pawn.CanAttack(self):
	// C.Focus = self; C.FireWeaponAt(self). (Team is the instigator's; 255 without teams, as is every bot's.)
	UWorld* World = GetWorld();
	const int32 Team = TeamOf(InstigatorController.Get());
	for (FConstControllerIterator It = World->GetControllerIterator(); It; ++It)
	{
		AArenaBotController* B = Cast<AArenaBotController>(It->Get());
		AArenaCharacter* P = B ? B->GetArenaPawn() : nullptr;
		if (!P || !P->IsAlive() || TeamOf(B) == Team || B->GetSkill() < 2.f || B->IsBotFiring())
		{
			continue;
		}
		if (B->GetEnemy() && B->IsEnemyVisible())
		{
			continue;
		}
		// Pawn.CanAttack: a weapon that reaches it, with a clear line
		const AArenaWeapon* W = P->GetCurrentWeapon();
		const FVector Eye = P->GetPawnViewLocation();
		const float Dist = FVector::Dist(Eye, GetActorLocation());
		if (!W || Dist > FMath::Max(W->FireModes[0].Range, W->FireModes[1].Range))
		{
			continue;
		}
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaWarheadSight), false, P);
		Q.AddIgnoredActor(this);
		if (World->LineTraceTestByChannel(Eye, GetActorLocation(), ECC_Visibility, Q))
		{
			continue;
		}
		if (B->GetShootTarget() != this)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("Warhead: %s shoots at the incoming warhead (%.0f m away)"), B->PlayerState ? *B->PlayerState->GetPlayerName() : TEXT("?"), Dist / 100.f);
		}
		B->SetShootTarget(this, 0.6f);
		++NumBotsWarned;
	}
}

void AArenaWarheadShell::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AActor* T = SmokeTrail.Get())
	{
		T->Destroy();
	}
	Super::EndPlay(EndPlayReason);
}

void AArenaWarheadShell::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
	const FHitResult& SweepResult)
{
	// RedeemerProjectile.ProcessTouch: anything but the instigator
	if (!bBlown && OtherActor && OtherActor != InstigatorPawn.Get() && Cast<APawn>(OtherActor))
	{
		BlowUp(GetActorLocation());
	}
}

float AArenaWarheadShell::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// RedeemerProjectile.TakeDamage: an enemy's (or nobody's) hit: no instigator / vehicle / crushed -> the nuke, else a
	// small explosion (HurtRadius(Damage, DamageRadius x 0.125)) and the warhead is gone
	if (bBlown || DamageAmount <= 0.f)
	{
		return 0.f;
	}
	if (EventInstigator && InstigatorController.IsValid() && SameTeam(GetWorld(), EventInstigator, InstigatorController.Get()))
	{
		return 0.f;
	}
	if (!EventInstigator || Cast<AArenaONS_Vehicle>(DamageCauser))
	{
		BlowUp(GetActorLocation());
		return DamageAmount;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Warhead: shot down by %s"), EventInstigator->PlayerState ? *EventInstigator->PlayerState->GetPlayerName() : TEXT("?"));
	bBlown = true;
	// RedeemerProjectile.TakeDamage: Spawn(class'SmallRedeemerExplosion')
	if (!ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("XEffects.SmallRedeemerExplosion"), GetActorLocation(), FRotator::ZeroRotator))
	{
		ArenaFX::SpawnExplosion(GetWorld(), GetActorLocation(), FLinearColor(1.f, 0.5f, 0.2f), 500.f, TEXT("Explosion.Rocket"));
	}
	SetActorEnableCollision(false);
	ArenaDamage::ApplyRadialDamage(GetWorld(), GetActorLocation(), DamageAmount, 2000.f * UU * 0.125f, 200000.f / 100.f * UU, InstigatorController.Get(), this, WeaponName);
	Destroy();
	return DamageAmount;
}

void AArenaWarheadShell::BlowUp(const FVector& At)
{
	if (bBlown)
	{
		return;
	}
	bBlown = true;
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	if (FlightSound)
	{
		FlightSound->Stop();
	}
	if (AActor* T = SmokeTrail.Get())
	{
		T->Destroy();
	}
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/Misc_redeemer_explosionsound"), 1.f);
	// BlowUp: ExplosionEffectClass (XEffects.RedeemerExplosion) 100 UU back along the flight, Rot(0, 16384, 0); Dying:
	// IonCore and ShakeView of every player within DamageRadius x 2
	const bool bUE2 = SpawnNukeEffects(GetWorld(), At, Velocity);
	// Dying state code: HurtRadius(250, 2000 UU x 0.125 .. 1) at 0, 0.5, 0.7, 0.9, 1.1, 1.3 s; MomentumTransfer 200000
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AArenaStagedBlast* B = GetWorld()->SpawnActor<AArenaStagedBlast>(AArenaStagedBlast::StaticClass(), At, FRotator::ZeroRotator, SP))
	{
		B->SetUE2Effect(bUE2);
		B->Start(At, 250.f, 2000.f * UU, 200000.f / 100.f * UU, InstigatorController.Get(), WeaponName, NukeStages(0.f), true);
	}
	SetLifeSpan(0.2f);
}

// =================================================================================================
// Guided warhead (RedeemerWarhead)
// =================================================================================================

AArenaGuidedWarhead::AArenaGuidedWarhead()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetupCollision(Collision, 24.f * UU);
	RootComponent = Collision;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Collision);
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(90.f);
	FlightSound = CreateDefaultSubobject<UAudioComponent>(TEXT("FlightSound"));
	FlightSound->SetupAttachment(Collision);
	FlightSound->bAutoActivate = false;
	SetCanBeDamaged(true);
	Tags.Add(ShootableTag);
	AutoPossessAI = EAutoPossessAI::Disabled;
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
}

void AArenaGuidedWarhead::Launch(AArenaCharacter* Old, AController* InFlyer, const FString& InWeaponName)
{
	OldPawn = Old;
	Flyer = InFlyer;
	WeaponName = InWeaponName;
	const FVector Dir = GetActorForwardVector();
	VelUU = Dir * 1000.f; // AirSpeed
	AccelUU = VelUU;
	if (Old)
	{
		Collision->IgnoreActorWhenMoving(Old, true);
	}
	Mesh = AddArtMesh(this, Collision, TEXT("StaticMeshes/WeaponStaticMesh/RedeemerMissile"), 150.f, FLinearColor(1.f, 0.6f, 0.3f));
	// PostBeginPlay: SmokeTrail = Spawn(class'RedeemerTrail', self,, Location - 40 * Dir); SetBase(self)
	if (AArenaEmitter* T = ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("XEffects.RedeemerTrail"), GetActorLocation() - Dir * 40.f * UU, Dir.Rotation(), 30.f))
	{
		T->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		SmokeTrail = T;
	}
	if (Mesh)
	{
		Mesh->SetOwnerNoSee(true); // bOwnerNoSee
	}
	if (USoundBase* S = ArenaArt::Load<USoundBase>(TEXT("Sounds/WeaponSounds/Misc_redeemer_flight")))
	{
		FlightSound->SetSound(S);
		FlightSound->Play();
	}
	if (APlayerController* PC = Cast<APlayerController>(InFlyer))
	{
		// UE2: Possessor.UnPossess(); Instigator.SetOwner(Possessor); Instigator.PlayerReplicationInfo = Possessor.PRI; Possess(Warhead)
		APlayerState* PS = PC->PlayerState;
		if (Old && Old->GetActiveCombo() != EArenaCombo::None)
		{
			Old->ActivateCombo(EArenaCombo::None);
			Old->Adrenaline = 0.f; // Possessor.Adrenaline = 0 when the warhead is launched during a combo
		}
		if (Old)
		{
			Old->StopFire(0);
			Old->StopFire(1);
			Old->SetRemoteController(PC);
		}
		PC->UnPossess();
		if (Old)
		{
			Old->SetOwner(PC);
			Old->SetPlayerState(PS);
		}
		PC->Possess(this);
		SetPlayerState(PS);
		LastSetRotation = GetActorRotation();
		PC->SetControlRotation(LastSetRotation);
		bFireWasDown = true; // the alt button that launched it must be let go first
	}
}

float AArenaGuidedWarhead::GetBankingDeg() const
{
	return Banking * 360.f / 65536.f;
}

void AArenaGuidedWarhead::UpdateRocketAcceleration(float DeltaTime, float YawChange, float PitchChange)
{
	YawAccel = (1.f - 2.f * DeltaTime) * YawAccel + DeltaTime * YawChange;
	PitchAccel = (1.f - 2.f * DeltaTime) * PitchAccel + DeltaTime * PitchChange;
	const FRotator Rot = VelUU.Rotation();
	const FRotationMatrix M(Rot);
	const FVector Y = M.GetUnitAxis(EAxis::Y);
	const FVector Z = M.GetUnitAxis(EAxis::Z);
	// near vertical: no pitching further over the top (PitchThreshold 3000)
	const float PitchThreshold = 3000.f;
	const int32 Pitch = int32(FRotator::NormalizeAxis(Rot.Pitch) * 65536.f / 360.f) & 65535;
	if (float(Pitch) > 16384.f - PitchThreshold && float(Pitch) < 49152.f + PitchThreshold)
	{
		if (float(Pitch) > 49152.f - PitchThreshold)
		{
			PitchAccel = FMath::Max(PitchAccel, 0.f);
		}
		else if (float(Pitch) < 16384.f + PitchThreshold)
		{
			PitchAccel = FMath::Min(PitchAccel, 0.f);
		}
	}
	AccelUU = VelUU + 5.f * (YawAccel * Y + PitchAccel * Z);
	if (AccelUU.IsNearlyZero())
	{
		AccelUU = VelUU;
	}
	AccelUU = AccelUU.GetSafeNormal() * 2000.f; // AccelRate
	// banking (visual roll): YawToBankingRatio 60, BankingResetRate 15, BankingDamping 10, MaxBanking 20000
	BankingVelocity += DeltaTime * (60.f * YawChange - 15.f * Banking - 10.f * BankingVelocity);
	Banking += DeltaTime * BankingVelocity;
	Banking = FMath::Clamp(Banking, -20000.f, 20000.f);
}

void AArenaGuidedWarhead::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	if (bBlown)
	{
		if (!bRelinquished && RelinquishTime > 0.f && Now >= RelinquishTime)
		{
			RelinquishController();
		}
		return;
	}
	if (FlightSound && FlightSound->Sound && !FlightSound->IsPlaying())
	{
		FlightSound->Play();
	}
	// Flying.Tick: the old pawn died, or the player left -> blow up
	AArenaCharacter* Old = OldPawn.Get();
	if (!Old || Old->IsDead() || !Controller || Controller != Flyer.Get())
	{
		BlowUp(GetActorLocation());
		return;
	}
	// PlayerRocketing.PlayerMove: aTurn / aLookUp (UE2 ViewRotation += 32 x dt x aTurn) from this frame's mouse input
	float YawChange = 0.f;
	float PitchChange = 0.f;
	if (DeltaSeconds > 0.f)
	{
		const FRotator CR = Controller->GetControlRotation();
		const float DYaw = FRotator::NormalizeAxis(CR.Yaw - LastSetRotation.Yaw);
		const float DPitch = FRotator::NormalizeAxis(CR.Pitch - LastSetRotation.Pitch);
		YawChange = DYaw * 65536.f / 360.f / (32.f * DeltaSeconds);
		PitchChange = DPitch * 65536.f / 360.f / (32.f * DeltaSeconds);
	}
	UpdateRocketAcceleration(DeltaSeconds, YawChange, PitchChange);
	// PHYS_Flying (UE2 CalcVelocity with the default volume's FluidFriction 0.3, AirSpeed 1000)
	const float VelSize = VelUU.Size();
	const FVector AccelDir = AccelUU.GetSafeNormal();
	VelUU = VelUU - (VelUU - AccelDir * VelSize) * FMath::Min(DeltaSeconds * 0.3f, 1.f);
	VelUU += AccelUU * DeltaSeconds;
	if (VelUU.SizeSquared() > FMath::Square(1000.f))
	{
		VelUU = VelUU.GetSafeNormal() * 1000.f;
	}
	FHitResult Hit;
	SetActorLocation(GetActorLocation() + VelUU * UU * DeltaSeconds, true, &Hit);
	FRotator R = VelUU.Rotation();
	R.Roll = GetBankingDeg();
	SetActorRotation(R);
	if (Hit.bBlockingHit)
	{
		BlowUp(Hit.Location);
		return;
	}
	// the controller's view follows the warhead (SetRotation(Pawn.Rotation))
	const FRotator View(R.Pitch, R.Yaw, 0.f);
	Controller->SetControlRotation(View);
	LastSetRotation = View;
	// Fire / AltFire: blow it up
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		const bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton) || PC->IsInputKeyDown(EKeys::RightMouseButton)
			|| PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger) || PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger);
		if (bDown && !bFireWasDown)
		{
			BlowUp(GetActorLocation());
			return;
		}
		bFireWasDown = bDown;
	}
}

float AArenaGuidedWarhead::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (bBlown || DamageAmount <= 0.f)
	{
		return 0.f;
	}
	if (EventInstigator && Flyer.IsValid() && SameTeam(GetWorld(), EventInstigator, Flyer.Get()))
	{
		return 0.f;
	}
	if (!EventInstigator || Cast<AArenaONS_Vehicle>(DamageCauser))
	{
		BlowUp(GetActorLocation());
		return DamageAmount;
	}
	// "Denied": a small explosion and the controller goes back
	bBlown = true;
	// RedeemerWarhead.TakeDamage: Spawn(class'SmallRedeemerExplosion')
	if (!ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("XEffects.SmallRedeemerExplosion"), GetActorLocation(), FRotator::ZeroRotator))
	{
		ArenaFX::SpawnExplosion(GetWorld(), GetActorLocation(), FLinearColor(1.f, 0.5f, 0.2f), 500.f, TEXT("Explosion.Rocket"));
	}
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	ArenaDamage::ApplyRadialDamage(GetWorld(), GetActorLocation(), DamageAmount, 2000.f * UU * 0.125f, 200000.f / 100.f * UU, Flyer.Get(), this, WeaponName);
	RelinquishController();
	Destroy();
	return DamageAmount;
}

void AArenaGuidedWarhead::BlowUp(const FVector& At)
{
	if (bBlown)
	{
		return;
	}
	bBlown = true;
	bStaticScreen = true;
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	if (FlightSound)
	{
		FlightSound->Stop();
	}
	if (AActor* T = SmokeTrail.Get())
	{
		T->Destroy();
	}
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/Misc_redeemer_explosionsound"), 1.f);
	// RedeemerWarhead.BlowUp: XEffects.RedeemerExplosion; Dying: IonCore, ShakeView
	const bool bUE2 = AArenaWarheadShell::SpawnNukeEffects(GetWorld(), At, VelUU);
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AArenaStagedBlast* B = GetWorld()->SpawnActor<AArenaStagedBlast>(AArenaStagedBlast::StaticClass(), At, FRotator::ZeroRotator, SP))
	{
		B->SetUE2Effect(bUE2);
		B->Start(At, 250.f, 2000.f * UU, 200000.f / 100.f * UU, Flyer.Get(), WeaponName, NukeStages(0.f), true);
	}
	// Dying: RelinquishController after the third ring (0.9 s), then the warhead is gone
	RelinquishTime = GetWorld()->GetTimeSeconds() + 0.9f;
}

void AArenaGuidedWarhead::RelinquishController()
{
	if (bRelinquished)
	{
		return;
	}
	bRelinquished = true;
	AController* C = Controller;
	AArenaCharacter* Old = OldPawn.Get();
	if (Old)
	{
		Old->SetRemoteController(nullptr);
	}
	if (C)
	{
		C->UnPossess();
		if (Old && !Old->IsDead())
		{
			C->Possess(Old);
		}
		// a dead old pawn: the controller is left without a pawn and respawns as usual
	}
	SetLifeSpan(0.1f);
}

void AArenaGuidedWarhead::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (EndPlayReason == EEndPlayReason::Destroyed && !bRelinquished)
	{
		RelinquishController();
	}
	if (AActor* T = SmokeTrail.Get())
	{
		T->Destroy();
	}
	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// Ion cannon
// =================================================================================================

AArenaIonCannon::AArenaIonCannon()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetCanBeDamaged(false);
}

AArenaIonCannon* AArenaIonCannon::FindOrSpawn(UWorld* World, const FVector& Near)
{
	TActorIterator<AArenaIonCannon> It(World);
	if (It)
	{
		return *It;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AArenaIonCannon>(AArenaIonCannon::StaticClass(), Near + FVector(0.f, 0.f, 2000.f * UU), FRotator::ZeroRotator, SP);
}

bool AArenaIonCannon::CheckMark(AController* InAimer, const FVector& Mark, bool bFire, const FString& InWeaponName)
{
	if (bFiring)
	{
		return false;
	}
	// open sky from the mark straight up to the cannon's height
	FVector Top = Mark;
	Top.Z = GetActorLocation().Z;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaIonSky), false);
	FCollisionResponseParams RP;
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Mark + FVector(0.f, 0.f, 2.f), Top, ECC_Visibility, Q, RP))
	{
		return false;
	}
	if (bFire)
	{
		Aimer = InAimer;
		WeaponName = InWeaponName;
		MarkLocation = Mark;
		bFiring = true;
		FireStart = GetWorld()->GetTimeSeconds();
		Stage = 0;
		++Strikes;
	}
	return true;
}

void AArenaIonCannon::SpawnEffect()
{
	// IonCannon.FireSequence.SpawnEffect: an IonEffect from the sky above the mark (the cannon's height, or the ceiling a
	// trace up from the mark hits if higher), plus two from either side across the aimer's line when their path is clear
	UWorld* World = GetWorld();
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaIonBeam), false);
	const bool bHit = World->LineTraceSingleByChannel(Hit, MarkLocation, MarkLocation + FVector(0.f, 0.f, 10000.f * UU), ECC_Visibility, Q);
	// (UE2: a trace without a hit leaves HitLocation at zero)
	const FVector Top(MarkLocation.X, MarkLocation.Y, FMath::Max(bHit ? Hit.Location.Z : 0.f, GetActorLocation().Z));
	bUE2Effect = false;
	auto Spawn = [this, World](const FVector& From)
	{
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AArenaIonEffect* E = World->SpawnActor<AArenaIonEffect>(AArenaIonEffect::StaticClass(), From, FRotator::ZeroRotator, SP);
		if (E && E->AimAt(MarkLocation, FVector::UpVector))
		{
			bUE2Effect = true;
			return;
		}
		if (E)
		{
			E->Destroy();
		}
		ArenaFX::SpawnBeam(World, From, MarkLocation, FLinearColor(0.55f, 0.75f, 1.f), 120.f, 2.f);
	};
	Spawn(Top);
	const APawn* Inst = Aimer.IsValid() ? Aimer->GetPawn() : nullptr;
	FVector CP = Inst ? FVector::CrossProduct(FVector::UpVector, GetActorLocation() - Inst->GetActorLocation()).GetSafeNormal() : FVector(1.f, 0.f, 0.f);
	CP *= 0.5f * (GetActorLocation().Z - MarkLocation.Z);
	for (const float Sign : { 1.f, -1.f })
	{
		if (!World->LineTraceTestByChannel(MarkLocation, Top + CP * Sign, ECC_Visibility, Q))
		{
			Spawn(Top + CP * Sign);
		}
	}
}

void AArenaIonCannon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bFiring)
	{
		return;
	}
	UWorld* World = GetWorld();
	const float T = World->GetTimeSeconds() - FireStart;
	// FireSequence: Sleep(0.5) effect + sound, Sleep(0.5) HurtRadius(150, 2000 UU x 0.125), Sleep(0.5) x 0.3, then every
	// 0.2 s x 0.475 .. 1, at the mark + 200 UU (MomentumTransfer 150000)
	const FVector DamageLoc = MarkLocation + FVector(0.f, 0.f, 200.f * UU);
	if (Stage == 0 && T >= 0.5f)
	{
		++Stage;
		SpawnEffect();
		if (USoundBase* S = ArenaArt::LoadOneShotSound(TEXT("Sounds/WeaponSounds/TAGRifle_IonCannonBlast")))
		{
			UGameplayStatics::PlaySound2D(this, S); // PlayGlobalSound
		}
	}
	const float StageTimes[] = { 1.f, 1.5f, 1.7f, 1.9f, 2.1f, 2.3f };
	const float Fracs[] = { 0.125f, 0.3f, 0.475f, 0.65f, 0.825f, 1.f };
	while (Stage >= 1 && Stage <= 6 && T >= StageTimes[Stage - 1])
	{
		const float R = 2000.f * UU * Fracs[Stage - 1];
		if (Stage == 1)
		{
			// ShakeView (players standing on something within DamageRadius x 2 of DamageLocation), then the first HurtRadius
			ArenaViewShake::ShakeViewersNear(World, DamageLoc, 2000.f * UU, 2.f, ArenaViewShake::Blast(), true);
		}
		ArenaDamage::ApplyRadialDamage(World, DamageLoc, 150.f, R, 150000.f / 100.f * UU, Aimer.Get(), this, WeaponName);
		if (!bUE2Effect)
		{
			ArenaFX::SpawnExplosion(World, DamageLoc, FLinearColor(0.55f, 0.75f, 1.f), R * 0.6f);
		}
		if (Stage == 2)
		{
			if (USoundBase* S = ArenaArt::LoadOneShotSound(TEXT("Sounds/WeaponSounds/Misc_redeemer_explosionsound")))
			{
				UGameplayStatics::PlaySound2D(this, S);
			}
		}
		++Stage;
	}
	if (Stage > 6)
	{
		bFiring = false; // GotoState('Ready')
	}
}

// =================================================================================================
// Seeker rocket (ONSAVRiLRocket)
// =================================================================================================

AArenaSeekerRocket::AArenaSeekerRocket()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetupCollision(Collision, 6.f);
	RootComponent = Collision;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Collision);
	Light->SetIntensity(2500.f);
	Light->SetAttenuationRadius(400.f);
	Light->SetLightColor(FLinearColor(1.f, 0.7f, 0.4f));
	Light->SetCastShadows(false);
	SetCanBeDamaged(true);
	Tags.Add(ShootableTag);
	InitialLifeSpan = 7.f;
}

void AArenaSeekerRocket::Launch(AWeap_SeekerLauncher* InLauncher, AController* InInstigator, const FString& InWeaponName, const FVector& Dir)
{
	Launcher = InLauncher;
	InstigatorController = InInstigator;
	InstigatorPawn = InInstigator ? InInstigator->GetPawn() : nullptr;
	WeaponName = InWeaponName;
	VelUU = Dir.GetSafeNormal() * 550.f; // Speed
	AccelUU = Dir.GetSafeNormal();         // Acceleration = Dir
	SetActorRotation(Dir.Rotation());
	if (APawn* P = InstigatorPawn.Get())
	{
		Collision->IgnoreActorWhenMoving(P, true);
	}
	Mesh = AddArtMesh(this, Collision, TEXT("StaticMeshes/VMWeaponsSM/AVRiLGroup_AVRiLprojectileSM"), 60.f, FLinearColor(1.f, 0.7f, 0.4f));
	const float Now = GetWorld()->GetTimeSeconds();
	NextHome = Now + 0.1f;
	LeadTargetStartTime = Now + 1.f; // LeadTargetDelay
}

void AArenaSeekerRocket::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bExploded)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	while (Now >= NextHome)
	{
		NextHome += 0.1f;
		Home();
	}
	// ONSAVRiLRocket.Tick: Acceleration += Normal(Velocity) x 750 x dt until MaxSpeed 2800, then no more acceleration;
	// integrated in steps of at most 1/60 s so the speed curve (550 + 375 t^2 UU/s) does not depend on the frame rate
	for (float Left = DeltaSeconds; Left > 0.f;)
	{
		const float Dt = FMath::Min(Left, 1.f / 60.f);
		Left -= Dt;
		if (bAccelerating)
		{
			if (VelUU.Size() >= 2800.f)
			{
				AccelUU = FVector::ZeroVector;
				bAccelerating = false;
			}
			else
			{
				AccelUU += VelUU.GetSafeNormal() * 750.f * Dt;
			}
		}
		VelUU += AccelUU * Dt;
		FlightTime += Dt;
	}
	FHitResult Hit;
	SetActorLocation(GetActorLocation() + VelUU * UU * DeltaSeconds, true, &Hit);
	SetActorRotation(VelUU.Rotation());
	if (Hit.bBlockingHit)
	{
		Explode(Hit.Location);
		return;
	}
	// pawns: touch (not the instigator)
	TArray<FOverlapResult> Ov;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaSeekerTouch), false, this);
	if (APawn* P = InstigatorPawn.Get())
	{
		Q.AddIgnoredActor(P);
	}
	if (GetWorld()->OverlapMultiByChannel(Ov, GetActorLocation(), FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius()), Q))
	{
		for (const FOverlapResult& O : Ov)
		{
			if (Cast<APawn>(O.GetActor()))
			{
				Explode(GetActorLocation());
				return;
			}
		}
	}
}

void AArenaSeekerRocket::Home()
{
	AWeap_SeekerLauncher* L = Launcher.Get();
	AActor* Target = L && L->IsLockedOn() ? L->GetHomingTarget() : nullptr;
	if (!Target)
	{
		return;
	}
	const FVector Loc = GetActorLocation() / UU;
	const FVector TLoc = Target->GetActorLocation() / UU;
	const FVector Dir = TLoc - Loc;
	const float VelMag = VelUU.Size();
	FVector ForceDir;
	if (GetWorld()->GetTimeSeconds() >= LeadTargetStartTime && VelMag > 0.f)
	{
		ForceDir = Dir + Target->GetVelocity() / UU * Dir.Size() / (VelMag * 2.f);
		const APawn* IP = InstigatorPawn.Get();
		const float LowestDesiredZ = IP ? FMath::Min(IP->GetActorLocation().Z / UU, TLoc.Z) : TLoc.Z;
		if (ForceDir.Z + Loc.Z < LowestDesiredZ)
		{
			ForceDir.Z += LowestDesiredZ - (ForceDir.Z + Loc.Z);
		}
		ForceDir = ForceDir.GetSafeNormal();
	}
	else
	{
		ForceDir = Dir;
	}
	ForceDir = (ForceDir * 0.8f * VelMag + VelUU).GetSafeNormal();
	VelUU = VelMag * ForceDir;
	AccelUU += 5.f * ForceDir;
}

float AArenaSeekerRocket::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (DamageAmount > 0.f && !bExploded)
	{
		Explode(GetActorLocation());
	}
	return DamageAmount;
}

void AArenaSeekerRocket::Explode(const FVector& At)
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	SetActorEnableCollision(false);
	// Damage 125, DamageRadius 150 UU, MomentumTransfer 50000
	ArenaDamage::ApplyRadialDamage(GetWorld(), At, 125.f, 150.f * UU, 50000.f / 100.f * UU, InstigatorController.Get(), this, WeaponName);
	// ONSAVRiLRocket.Explode: Spawn(class'ONSAVRiLRocketExplosion', ..., Location, rotator(vect(0,0,1)))
	if (!ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("Onslaught.ONSAVRiLRocketExplosion"), At, FRotator(90.f, 0.f, 0.f)))
	{
		ArenaFX::SpawnExplosion(GetWorld(), At, FLinearColor(1.f, 0.6f, 0.3f), 300.f, TEXT("Explosion.Rocket"));
	}
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/BaseImpactAndExplosions_BExplosion3"), 1.f);
	Destroy();
}

// =================================================================================================
// Sticky grenade (ONSGrenadeProjectile)
// =================================================================================================

AArenaStickyGrenade::AArenaStickyGrenade()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetupCollision(Collision, 4.f);
	RootComponent = Collision;
	Beacon = CreateDefaultSubobject<UPointLightComponent>(TEXT("Beacon"));
	Beacon->SetupAttachment(Collision);
	Beacon->SetIntensity(600.f);
	Beacon->SetAttenuationRadius(120.f);
	Beacon->SetCastShadows(false);
	SetCanBeDamaged(true);
	Tags.Add(ShootableTag);
}

void AArenaStickyGrenade::Launch(AWeap_StickyLauncher* InLauncher, AController* InInstigator, const FString& InWeaponName, const FVector& Dir, float SpeedCm)
{
	Launcher = InLauncher;
	InstigatorController = InInstigator;
	InstigatorPawn = InInstigator ? InInstigator->GetPawn() : nullptr;
	WeaponName = InWeaponName;
	Velocity = Dir.GetSafeNormal() * SpeedCm;
	if (APawn* P = InstigatorPawn.Get())
	{
		Collision->IgnoreActorWhenMoving(P, true);
	}
	Mesh = AddArtMesh(this, Collision, TEXT("StaticMeshes/VMWeaponsSM/PlayerWeaponsGroup_VMGrenade"), 12.f, FLinearColor(0.4f, 0.45f, 0.3f));
	// ONSGrenadeBeaconRed / Blue
	const int32 Team = TeamOf(InInstigator);
	Beacon->SetLightColor(Team == 1 ? FLinearColor(0.3f, 0.45f, 1.f) : FLinearColor(1.f, 0.3f, 0.25f));
}

void AArenaStickyGrenade::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bExploded)
	{
		return;
	}
	if (StuckTo.IsValid())
	{
		// PawnBaseDied: the pawn it sticks to is dead -> fall off
		if (const AArenaCharacter* C = Cast<AArenaCharacter>(StuckTo.Get()))
		{
			if (C->IsDead())
			{
				UnStick();
			}
		}
		return;
	}
	if (IsValid(StuckTo.Get()) == false && GetAttachParentActor())
	{
		UnStick();
	}
	if (bResting)
	{
		return;
	}
	// ONSGrenadeProjectile PHYS_Falling: the volume's gravity, FluidFriction and TerminalVelocity (UE2 physFalling)
	Velocity = ArenaWorldZones::NewFallVelocity(GetWorld(), GetActorLocation(), Velocity, ArenaTuning::GravityScale, DeltaSeconds);
	FHitResult Hit;
	SetActorLocation(GetActorLocation() + Velocity * DeltaSeconds, true, &Hit);
	if (Hit.bBlockingHit)
	{
		AActor* Other = Hit.GetActor();
		if (Other && (Cast<APawn>(Other) || Cast<AArenaONS_Vehicle>(Other)) && Other != InstigatorPawn.Get() && Other != IgnoreActor.Get())
		{
			Stick(Other);
			return;
		}
		// ONSGrenadeProjectile.HitWall: DampenFactor 0.5 along the normal, DampenFactorParallel 0.8
		const FVector N = Hit.ImpactNormal;
		const FVector VNorm = FVector::DotProduct(Velocity, N) * N;
		Velocity = -VNorm * 0.5f + (Velocity - VNorm) * 0.8f;
		if (Velocity.Size() < 40.f * UU)
		{
			bResting = true;
			Velocity = FVector::ZeroVector;
		}
		else if (Velocity.Size() > 250.f * UU)
		{
			PlayArtSound(this, TEXT("Sounds/WeaponSounds/PGrenFloor1_P1GrenFloor1"), 0.6f);
		}
		return;
	}
	// touching a pawn (overlap response): stick (ProcessTouch)
	TArray<FOverlapResult> Ov;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaGrenadeTouch), false, this);
	if (APawn* P = InstigatorPawn.Get())
	{
		Q.AddIgnoredActor(P);
	}
	if (AActor* Ig = IgnoreActor.Get())
	{
		Q.AddIgnoredActor(Ig);
	}
	if (GetWorld()->OverlapMultiByChannel(Ov, GetActorLocation(), FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius()), Q))
	{
		for (const FOverlapResult& O : Ov)
		{
			AActor* A = O.GetActor();
			if (A && (Cast<APawn>(A) || Cast<AArenaONS_Vehicle>(A)) && !Cast<AArenaGuidedWarhead>(A))
			{
				Stick(A);
				return;
			}
		}
	}
}

void AArenaStickyGrenade::Stick(AActor* Other)
{
	StuckTo = Other;
	bResting = true;
	Velocity = FVector::ZeroVector;
	if (USceneComponent* R = Other->GetRootComponent())
	{
		AttachToComponent(R, FAttachmentTransformRules::KeepWorldTransform);
	}
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/BaseGunTech_BGrenfloor1"), 0.8f);
}

void AArenaStickyGrenade::UnStick()
{
	IgnoreActor = StuckTo.Get();
	StuckTo.Reset();
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Velocity = FVector::ZeroVector;
	bResting = false;
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
}

float AArenaStickyGrenade::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// ONSGrenadeProjectile.TakeDamage: stuck and hit by something else than a grenade -> someone else's hit knocks it
	// off; otherwise it explodes
	if (DamageAmount <= 0.f || bExploded)
	{
		return 0.f;
	}
	if (StuckTo.IsValid() && !SameKind(this, DamageCauser))
	{
		if (!EventInstigator || EventInstigator != InstigatorController.Get())
		{
			UnStick();
			return DamageAmount;
		}
	}
	Explode();
	return DamageAmount;
}

void AArenaStickyGrenade::Explode()
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	const FVector At = GetActorLocation();
	SetActorEnableCollision(false);
	// Damage 100, DamageRadius 175 UU, MomentumTransfer 50000
	ArenaDamage::ApplyRadialDamage(GetWorld(), At, 100.f, 175.f * UU, 50000.f / 100.f * UU, InstigatorController.Get(), this, WeaponName);
	// ONSGrenadeProjectile.Destroyed: Spawn(class'ONSGrenadeExplosionEffect', ..., Location, rotator(vect(0,0,1)))
	if (!ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("Onslaught.ONSGrenadeExplosionEffect"), At, FRotator(90.f, 0.f, 0.f)))
	{
		ArenaFX::SpawnExplosion(GetWorld(), At, FLinearColor(1.f, 0.6f, 0.3f), 350.f, TEXT("Explosion.Grenade"));
	}
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/BaseImpactAndExplosions_BExplosion3"), 1.f);
	Destroy();
}

void AArenaStickyGrenade::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AWeap_StickyLauncher* L = Launcher.Get())
	{
		L->NotifyGrenadeGone(this);
	}
	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// Crawler mine (ONSMineProjectile)
// =================================================================================================

AArenaCrawlerMine::AArenaCrawlerMine()
{
	PrimaryActorTick.bCanEverTick = true;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetupCollision(Collision, 10.f * UU);
	RootComponent = Collision;
	SetCanBeDamaged(true);
	Tags.Add(ShootableTag);
}

void AArenaCrawlerMine::Launch(AWeap_CrawlerLayer* InLayer, AController* InInstigator, const FString& InWeaponName, const FVector& VelocityCm)
{
	Layer = InLayer;
	InstigatorController = InInstigator;
	InstigatorPawn = InInstigator ? InInstigator->GetPawn() : nullptr;
	WeaponName = InWeaponName;
	TeamNum = TeamOf(InInstigator);
	Velocity = VelocityCm;
	if (APawn* P = InstigatorPawn.Get())
	{
		Collision->IgnoreActorWhenMoving(P, true);
	}
	Collision->OnComponentBeginOverlap.AddDynamic(this, &AArenaCrawlerMine::OnOverlap);
	if (USkeletalMesh* M = ArenaArt::Load<USkeletalMesh>(TEXT("SkeletalMeshes/ONSWeapons-A/ParasiteMine")))
	{
		Body = NewObject<USkeletalMeshComponent>(this, TEXT("Body"));
		Body->SetSkeletalMeshAsset(M);
		Body->SetupAttachment(Collision);
		Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Body->SetRelativeScale3D(FVector(0.2f)); // DrawScale (the mesh is imported at 2 cm per unit)
		Body->SetRelativeLocation(FVector(0.f, 0.f, -2.9f * UU * 0.2f - 10.f * UU)); // PrePivot, standing on the collision bottom
		Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Body->RegisterComponent();
	}
	else
	{
		Fallback = AddArtMesh(this, Collision, TEXT("StaticMeshes/ONSWeapons-SM/MineLayerAmmo"), 30.f, TeamNum == 1 ? FLinearColor(0.3f, 0.4f, 1.f) : FLinearColor(1.f, 0.3f, 0.2f));
	}
	PlayAnim(TEXT("StartUp"), false, 0.5f);
	GotoState(EState::Flying);
}

void AArenaCrawlerMine::PlayAnim(const TCHAR* Name, bool bLoop, float Rate)
{
	if (!Body)
	{
		return;
	}
	if (UAnimSequence* A = ArenaArt::Load<UAnimSequence>(FString(TEXT("Animations/ONSWeapons-A/ParasiteMineAnims_")) + Name))
	{
		Body->PlayAnimation(A, bLoop);
		Body->SetPlayRate(Rate);
	}
}

void AArenaCrawlerMine::GotoState(EState NewState)
{
	State = NewState;
	StateTime = 0.f;
	const float Now = GetWorld()->GetTimeSeconds();
	switch (NewState)
	{
	case EState::OnGround:
		Velocity = FVector::ZeroVector;
		NextTimer = Now; // SetTimer(DetectionTimer 0.5, true); Timer()
		break;
	case EState::Scurrying:
		NextTimer = Now + 0.25f; // Sleep(0.25), SetTimer(DetectionTimer / 2)
		break;
	case EState::ScurryToTargetLoc:
		NextTimer = Now;
		break;
	default:
		break;
	}
}

void AArenaCrawlerMine::SetScurryTarget(const FVector& Location)
{
	// SetScurryTarget: only mines without a target; TargetLoc = NewTargetLoc + VRand() x Rand(TargetLocFuzz 250)
	if (TargetPawn.IsValid() && State == EState::Scurrying)
	{
		return;
	}
	TargetLoc = Location + FMath::VRand() * float(FMath::RandRange(0, 249)) * UU;
	if (State != EState::Flying)
	{
		GotoState(EState::ScurryToTargetLoc);
	}
}

void AArenaCrawlerMine::AcquireTarget()
{
	TargetPawn.Reset();
	float BestDist = 0.f;
	const FVector Loc = GetActorLocation();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* A = *It;
		if (!A || A == InstigatorPawn.Get() || Cast<AArenaGuidedWarhead>(A))
		{
			continue;
		}
		const float Dist = FVector::Dist(A->GetActorLocation(), Loc);
		if (Dist > 750.f * UU)
		{
			continue;
		}
		if (const AArenaCharacter* C = Cast<AArenaCharacter>(A))
		{
			if (C->IsDead())
			{
				continue;
			}
		}
		if (const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(A))
		{
			if (!V->HasDriver())
			{
				continue;
			}
		}
		else if (!Cast<AArenaCharacter>(A))
		{
			continue;
		}
		AController* AC = A->GetController();
		const int32 T = TeamOf(AC);
		if (T != 255 && T == TeamNum)
		{
			continue;
		}
		// VisibleCollidingActors: a clear line
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaMineSight), false, this);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		if (GetWorld()->LineTraceSingleByChannel(Hit, Loc, A->GetActorLocation(), ECC_Visibility, Q, RP))
		{
			continue;
		}
		if (!TargetPawn.IsValid() || Dist < BestDist)
		{
			TargetPawn = A;
			BestDist = Dist;
		}
	}
}

bool AArenaCrawlerMine::WalkStep(float DeltaSeconds)
{
	const FVector Start = GetActorLocation();
	FHitResult Hit;
	SetActorLocation(Start + FVector(Velocity.X, Velocity.Y, 0.f) * DeltaSeconds, true, &Hit);
	if (Hit.bBlockingHit && Hit.ImpactNormal.Z < 0.7f)
	{
		// step up small ledges (UE2 MaxStepHeight 35 UU)
		FHitResult Up;
		SetActorLocation(GetActorLocation() + FVector(0.f, 0.f, 35.f * UU), true, &Up);
		SetActorLocation(GetActorLocation() + FVector(Velocity.X, Velocity.Y, 0.f) * DeltaSeconds, true, &Up);
	}
	// follow the floor
	FHitResult Floor;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaMineFloor), false, this);
	const FVector L = GetActorLocation();
	const float R = Collision->GetScaledSphereRadius();
	if (GetWorld()->SweepSingleByChannel(Floor, L, L - FVector(0.f, 0.f, 40.f * UU), FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(R), Q)
		&& Floor.ImpactNormal.Z >= 0.7f)
	{
		SetActorLocation(Floor.Location + FVector(0.f, 0.f, 0.5f));
		return true;
	}
	return false;
}

void AArenaCrawlerMine::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bExploded)
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	StateTime += DeltaSeconds;
	switch (State)
	{
	case EState::Flying:
	{
		// ONSMineProjectile PHYS_Falling: the volume's gravity, FluidFriction and TerminalVelocity (UE2 physFalling)
		Velocity = ArenaWorldZones::NewFallVelocity(GetWorld(), GetActorLocation(), Velocity, ArenaTuning::GravityScale, DeltaSeconds);
		FHitResult Hit;
		SetActorLocation(GetActorLocation() + Velocity * DeltaSeconds, true, &Hit);
		if (Hit.bBlockingHit)
		{
			if (Hit.ImpactNormal.Z >= 0.7f)
			{
				// Flying.Landed: stand up on the surface
				FRotator R = Hit.ImpactNormal.Rotation();
				R.Pitch -= 90.f;
				SetActorRotation(FRotator(0.f, GetActorRotation().Yaw, 0.f));
				PlayArtSound(this, TEXT("Sounds/WeaponSounds/BaseGunTech_BGrenfloor1"), 0.6f);
				GotoState(EState::OnGround);
				PlayAnim(TEXT("CloseDown"), false, 1.f); // OnGround: Sleep(0.4) CloseDown
			}
			else if (bBounce)
			{
				// Flying.HitWall: the first wall bounces (an enemy pawn: blow up), the next one blows up
				if (APawn* P = Cast<APawn>(Hit.GetActor()))
				{
					if (TeamOf(P->GetController()) != TeamNum || TeamNum == 255)
					{
						BlowUp();
						return;
					}
				}
				bBounce = false;
				const FVector N = Hit.ImpactNormal;
				Velocity = Velocity - 2.f * FVector::DotProduct(Velocity, N) * N;
			}
			else
			{
				BlowUp();
			}
		}
		break;
	}
	case EState::OnGround:
		if (Now >= NextTimer)
		{
			NextTimer = Now + 0.5f; // DetectionTimer
			if (!InstigatorController.IsValid())
			{
				BlowUp();
				return;
			}
			AcquireTarget();
			if (TargetPawn.IsValid())
			{
				PlayAnim(TEXT("StartUp"), false, 1.f);
				GotoState(EState::Scurrying);
			}
		}
		break;
	case EState::Scurrying:
	{
		if (Now >= NextTimer)
		{
			NextTimer = Now + 0.25f; // DetectionTimer / 2
			AActor* T = TargetPawn.Get();
			if (!T)
			{
				GotoState(EState::Flying);
				break;
			}
			FVector NewLoc = T->GetActorLocation() - GetActorLocation();
			const float TargetDist = NewLoc.Size();
			if (TargetDist < 1000.f * UU)
			{
				NewLoc.Z = 0.f;
				Velocity = NewLoc.GetSafeNormal() * 525.f * UU; // ScurrySpeed
				if (TargetDist < 225.f * UU)
				{
					// leap: Velocity x 1.2, Z 350 UU/s
					Velocity *= 1.2f;
					Velocity.Z = 350.f * UU;
					bBounce = true;
					GotoState(EState::Flying);
					break;
				}
				SetActorRotation(FRotator(0.f, NewLoc.Rotation().Yaw, 0.f));
				if (StateTime < 0.3f || !Body || !Body->IsPlaying())
				{
					PlayAnim(TEXT("Scurry"), true, 1.f);
				}
			}
			else
			{
				GotoState(EState::Flying);
				break;
			}
		}
		if (StateTime >= 0.25f && !WalkStep(DeltaSeconds))
		{
			GotoState(EState::Flying);
		}
		break;
	}
	case EState::ScurryToTargetLoc:
	{
		if (Now >= NextTimer)
		{
			NextTimer = Now + 0.25f;
			FVector NewLoc = TargetLoc - GetActorLocation();
			NewLoc.Z = 0.f;
			if (NewLoc.Size() < 250.f * UU)
			{
				AcquireTarget();
				GotoState(TargetPawn.IsValid() ? EState::Scurrying : EState::Flying);
				break;
			}
			Velocity = NewLoc.GetSafeNormal() * 525.f * UU;
			SetActorRotation(FRotator(0.f, NewLoc.Rotation().Yaw, 0.f));
			PlayAnim(TEXT("Scurry"), true, 1.f);
		}
		if (!WalkStep(DeltaSeconds))
		{
			GotoState(EState::Flying);
		}
		break;
	}
	}
}

void AArenaCrawlerMine::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
	const FHitResult& SweepResult)
{
	// ONSMineProjectile.ProcessTouch: an enemy pawn (not the instigator) -> blow up
	if (bExploded || !OtherActor || OtherActor == InstigatorPawn.Get())
	{
		return;
	}
	if (APawn* P = Cast<APawn>(OtherActor))
	{
		if (Cast<AArenaGuidedWarhead>(P))
		{
			return;
		}
		if (const AArenaCharacter* C = Cast<AArenaCharacter>(P))
		{
			if (C->IsDead())
			{
				return;
			}
		}
		const int32 T = TeamOf(P->GetController());
		if (T == 255 || T != TeamNum)
		{
			BlowUp();
		}
	}
}

float AArenaCrawlerMine::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// ONSMineProjectile.TakeDamage: any hit but another of our own mines going off; team mates' hits don't count
	if (DamageAmount <= 0.f || !EventInstigator || bExploded)
	{
		return 0.f;
	}
	if (EventInstigator == InstigatorController.Get() && SameKind(this, DamageCauser))
	{
		return 0.f;
	}
	if (EventInstigator != InstigatorController.Get() && SameTeam(GetWorld(), EventInstigator, InstigatorController.Get()))
	{
		return 0.f;
	}
	BlowUp();
	return DamageAmount;
}

FVector AArenaCrawlerMine::LastBlowUpLocation = FVector::ZeroVector;

void AArenaCrawlerMine::BlowUp()
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	const FVector At = GetActorLocation();
	LastBlowUpLocation = At;
	SetActorEnableCollision(false);
	// Damage 95, DamageRadius 250 UU, MomentumTransfer 50000
	ArenaDamage::ApplyRadialDamage(GetWorld(), At, 95.f, 250.f * UU, 50000.f / 100.f * UU, InstigatorController.Get(), this, WeaponName);
	// ONSMineProjectile.Destroyed: Spawn(class'ONSGrenadeExplosionEffect', ..., Location, rotator(vect(0,0,1)))
	if (!ArenaUE2Effects::SpawnEmitter(GetWorld(), TEXT("Onslaught.ONSGrenadeExplosionEffect"), At, FRotator(90.f, 0.f, 0.f)))
	{
		ArenaFX::SpawnExplosion(GetWorld(), At, FLinearColor(1.f, 0.55f, 0.25f), 400.f, TEXT("Explosion.Grenade"));
	}
	PlayArtSound(this, TEXT("Sounds/WeaponSounds/RocketLauncher_RocketLauncherFire"), 0.8f);
	Destroy();
}

void AArenaCrawlerMine::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AWeap_CrawlerLayer* L = Layer.Get())
	{
		L->NotifyMineGone(this);
	}
	Super::EndPlay(EndPlayReason);
}

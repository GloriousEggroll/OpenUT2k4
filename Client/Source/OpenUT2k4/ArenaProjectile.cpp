#include "ArenaProjectile.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaFX.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Engine/Scene.h"
#include "TimerManager.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	constexpr float InstigatorGraceTime = 0.15f;
}

AArenaProjectile::AArenaProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	// always ticks: FlightTime (game time of flight); the behaviour below is a no-op for plain projectiles
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = false;
	SetCanBeDamaged(false);

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(6.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_Projectile);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Projectile, ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Weapon, ECR_Ignore);
	Collision->SetGenerateOverlapEvents(true);
	Collision->SetCanEverAffectNavigation(false);
	Collision->bReturnMaterialOnMove = false;
	Collision->SetShouldUpdatePhysicsVolume(false);
	RootComponent = Collision;

	Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visual"));
	Visual->SetupAttachment(Collision);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Visual->SetCollisionResponseToAllChannels(ECR_Ignore);
	Visual->SetGenerateOverlapEvents(false);
	Visual->SetCastShadow(false);
	Visual->SetCanEverAffectNavigation(false);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Collision);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->SetCastShadows(false);
	Light->bAffectTranslucentLighting = false;
	Light->Intensity = 2500.f;
	Light->AttenuationRadius = 200.f;

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->InitialSpeed = 3000.f;
	Movement->MaxSpeed = 3000.f;
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
	Movement->ProjectileGravityScale = 0.f;
	Movement->BounceVelocityStopSimulatingThreshold = 25.f;
}

void AArenaProjectile::Init(const FArenaProjectileParams& InParams, AController* InInstigatorController, float InDamageScale)
{
	Params = InParams;
	InstigatorController = InInstigatorController;
	DamageScale = InDamageScale;

	const float Size = FMath::Max(1.f, Params.Size);

	// Collision: the UE2 collision radius when given, else combo cores are big, easy targets and everything else is
	// a bit smaller than its glow.
	const float CollisionRadius = Params.CollisionRadius >= 0.f ? FMath::Max(0.1f, Params.CollisionRadius)
		: (Params.bComboCore ? Size : FMath::Clamp(Size * 0.6f, 3.f, 30.f));
	Collision->SetSphereRadius(CollisionRadius);
	Collision->SetCollisionResponseToChannel(ECC_Weapon, (Params.bComboCore || Params.bProjTarget) ? ECR_Block : ECR_Ignore);
	// Projectiles touch each other (UE2 ProcessTouch: shock cores, goop merging); handled in OnCollisionOverlap.
	Collision->SetCollisionResponseToChannel(ECC_Projectile, ECR_Overlap);

	Movement->InitialSpeed = Params.Speed;
	// UE2 PHYS_Falling projectiles (bio globs, grenades, flak shells) have no speed cap but the physics volume's
	// TerminalVelocity (UArenaWorldZoneSubsystem); MaxSpeed limits PHYS_Projectile flight only
	Movement->MaxSpeed = Params.GravityScale != 0.f ? 0.f : FMath::Max(Params.Speed, Params.MaxSpeed);
	Movement->ProjectileGravityScale = Params.GravityScale;
	Movement->bShouldBounce = Params.Bounces != 0;
	Movement->Bounciness = Params.Bounciness;
	Movement->Friction = Params.bShard ? 0.05f : 0.3f;
	switch (Params.Kind)
	{
	case EArenaProjKind::Grenade:
		// bounces are computed by hand (UE2 Grenade.HitWall dampening)
		Movement->bShouldBounce = true;
		Movement->Bounciness = 1.f;
		Movement->Friction = 0.f;
		Movement->BounceVelocityStopSimulatingThreshold = 40.f;
		break;
	case EArenaProjKind::FlakChunk:
	{
		// FlakChunk.PostBeginPlay: 25% no bounce, 50% one, 25% two; flies straight until the first wall
		const float R = FMath::FRand();
		BouncesLeft = R > 0.75f ? 2 : (R > 0.25f ? 1 : 0);
		Movement->bShouldBounce = true;
		Movement->Bounciness = 1.f;
		Movement->Friction = 0.f;
		Movement->ProjectileGravityScale = 0.f;
		break;
	}
	case EArenaProjKind::FlakShell:
	case EArenaProjKind::ShockBall:
	case EArenaProjKind::LinkBolt:
	case EArenaProjKind::Rocket:
		Movement->bShouldBounce = false;
		break;
	case EArenaProjKind::BioGlob:
		Movement->bShouldBounce = false;
		BioRand3 = FMath::RandRange(0, 2);
		break;
	default:
		break;
	}

	// Visual: glowing sphere of diameter 2*Size
	Visual->SetStaticMesh(ArenaAssets::Sphere());
	Visual->SetRelativeScale3D(FVector(Size / 50.f));
	if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeGlow(this, Params.Color, Params.bComboCore ? 12.f : 8.f, 1.f))
	{
		Visual->SetMaterial(0, MID);
	}
	if (Params.bShard)
	{
		// Shards: stretched along flight direction
		Visual->SetRelativeScale3D(FVector(Size / 25.f, Size / 60.f, Size / 60.f));
	}

	Light->SetLightColor(Params.Color);
	Light->SetAttenuationRadius(FMath::Clamp(Size * 18.f, 120.f, 900.f));
	Light->SetIntensity(Params.bComboCore ? 9000.f : (Params.bShard ? 900.f : 3500.f));

	InitialLifeSpan = FMath::Max(0.1f, Params.LifeSpan);
	ArtDesc = ArenaArtFX::FindProjectileArt(Params.Style);
}

void AArenaProjectile::BuildArt()
{
	if (!ArtDesc)
	{
		return;
	}
	const ArenaArtFX::FProjectileArt& Art = *ArtDesc;
	const float S = FMath::Max(1.f, Params.Size) / FMath::Max(1.f, Art.RefSize);
	int32 Built = 0;

	// Vertex-animated blob
	if (Art.SkelMesh)
	{
		if (USkeletalMesh* Mesh = ArenaArt::Load<USkeletalMesh>(Art.SkelMesh))
		{
			ArtSkel = NewObject<USkeletalMeshComponent>(this, TEXT("ArtSkel"));
			ArtSkel->SetSkeletalMeshAsset(Mesh);
			ArtSkel->SetupAttachment(Collision);
			ArtSkel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ArtSkel->SetGenerateOverlapEvents(false);
			ArtSkel->SetCanEverAffectNavigation(false);
			ArtSkel->SetCastShadow(false);
			ArtSkel->bComponentUseFixedSkelBounds = true;
			if (UMaterialInterface* Mat = Art.SkelMaterial ? ArenaArt::Load<UMaterialInterface>(Art.SkelMaterial) : nullptr)
			{
				for (int32 i = 0; i < FMath::Max(1, Mesh->GetMaterials().Num()); ++i)
				{
					ArtSkel->SetMaterial(i, Mat);
				}
			}
			const FBoxSphereBounds B = Mesh->GetImportedBounds();
			const float Fit = Art.SkelSize * S / FMath::Max(1.f, float(B.BoxExtent.GetMax()) * 2.f);
			ArtSkel->SetRelativeScale3D(FVector(Fit));
			ArtBaseScale = Fit;
			ArtSkel->SetRelativeLocation(-FVector(B.Origin) * Fit);
			ArtSkel->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			ArtSkel->RegisterComponent();
			if (UAnimSequence* Anim = Art.FlyAnim ? ArenaArt::Load<UAnimSequence>(Art.FlyAnim) : nullptr)
			{
				ArtSkel->PlayAnimation(Anim, true);
			}
			++Built;
		}
	}
	// Static meshes (fallback for the blob)
	if (!ArtSkel)
	{
		for (const ArenaArtFX::FMeshDesc& M : Art.Meshes)
		{
			if (UStaticMeshComponent* C = ArenaArtFX::AddMesh(this, Collision, M, S))
			{
				ArtMeshes.Add(C);
				ArtMeshSpin.Add(M.Spin);
				++Built;
			}
		}
	}
	for (const ArenaArtFX::FSpriteDesc& D : Art.Sprites)
	{
		UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
		C->SetupAttachment(Collision);
		if (C->SetupSprite(D, S))
		{
			C->RegisterComponent();
			++Built;
		}
		else
		{
			C->DestroyComponent();
		}
	}
	if (Built == 0)
	{
		ArtDesc = nullptr; // keep the procedural look (and its effects)
		return;
	}
	Visual->SetVisibility(false);
	if (Art.LightScale <= 0.f)
	{
		Light->SetVisibility(false);
	}
	else
	{
		Light->SetIntensity(Light->Intensity * Art.LightScale);
	}
	for (const float Spin : ArtMeshSpin)
	{
		if (Spin != 0.f)
		{
			SetActorTickEnabled(true);
			break;
		}
	}
}

void AArenaProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FlightTime += DeltaSeconds;
	TickBehaviour(DeltaSeconds);
	if (bStuck)
	{
		return;
	}
	for (int32 i = 0; i < ArtMeshes.Num(); ++i)
	{
		if (ArtMeshes[i] && ArtMeshSpin.IsValidIndex(i) && ArtMeshSpin[i] != 0.f)
		{
			ArtMeshes[i]->AddLocalRotation(FRotator(0.f, 0.f, ArtMeshSpin[i] * DeltaSeconds));
		}
	}
}

FName AArenaProjectile::GetArtEffect(bool bExplosion) const
{
	if (!ArtDesc)
	{
		return NAME_None;
	}
	const TCHAR* Name = bExplosion ? ArtDesc->Explosion : ArtDesc->Impact;
	return Name ? FName(Name) : NAME_None;
}

void AArenaProjectile::BeginPlay()
{
	Super::BeginPlay();
	BuildArt();

	Collision->OnComponentHit.AddDynamic(this, &AArenaProjectile::OnCollisionHit);
	Collision->OnComponentBeginOverlap.AddDynamic(this, &AArenaProjectile::OnCollisionOverlap);
	Movement->OnProjectileBounce.AddDynamic(this, &AArenaProjectile::OnProjectileBounced);
	Movement->OnProjectileStop.AddDynamic(this, &AArenaProjectile::OnProjectileStopped);

	if (!InstigatorController.IsValid() && GetInstigator())
	{
		InstigatorController = GetInstigator()->GetController();
	}

	// Ignore the shooter briefly so the projectile can leave the capsule.
	if (APawn* P = GetInstigator())
	{
		GracePawn = P;
		GraceEndTime = GetWorld()->GetTimeSeconds() + InstigatorGraceTime;
		Collision->IgnoreActorWhenMoving(P, true);
		if (UPrimitiveComponent* PawnRoot = Cast<UPrimitiveComponent>(P->GetRootComponent()))
		{
			PawnRoot->IgnoreActorWhenMoving(this, true);
		}
		TWeakObjectPtr<AArenaProjectile> WeakThis(this);
		GetWorldTimerManager().SetTimer(GraceTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->EndInstigatorGrace();
			}
		}), InstigatorGraceTime, false);
	}

	if (Params.LifeSpan > 0.f)
	{
		SetLifeSpan(Params.LifeSpan);
	}

	SpawnTime = GetWorld()->GetTimeSeconds();
	InitialDir = GetActorForwardVector();
	if (Params.TossZ != 0.f && Movement)
	{
		Movement->Velocity.Z += Params.TossZ;
	}
	if (Params.bIgnoreInstigator)
	{
		// UE2 ProcessTouch ignores the instigator for the projectile's whole life (radius damage still hits it).
		GetWorldTimerManager().ClearTimer(GraceTimer);
		GraceEndTime = TNumericLimits<float>::Max();
	}
	if (Params.Kind == EArenaProjKind::ShockBall)
	{
		// ShockProjectile.Timer: collision grows to 20 UU after 0.4 s
		TWeakObjectPtr<AArenaProjectile> WeakThis(this);
		GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
		{
			if (WeakThis.IsValid() && WeakThis->Collision)
			{
				WeakThis->Collision->SetSphereRadius(40.f);
			}
		}), 0.4f, false);
	}
	if (Params.Acceleration > 0.f || Params.Kind == EArenaProjKind::Rocket)
	{
		SetActorTickEnabled(true);
	}
	if (Params.Kind == EArenaProjKind::BioGlob && Params.GoopLevel > 1)
	{
		BioSetGoopLevel(Params.GoopLevel);
	}
}

void AArenaProjectile::SetFlock(const TArray<AArenaProjectile*>& Mates, bool bInCurl)
{
	Flock.Reset();
	for (AArenaProjectile* M : Mates)
	{
		if (M && M != this && Flock.Num() < 2)
		{
			Flock.Add(M);
		}
	}
	bCurl = bInCurl;
	SetActorTickEnabled(true);
}

void AArenaProjectile::TickBehaviour(float DeltaSeconds)
{
	if (!Movement || bExploded || bStuck)
	{
		return;
	}
	// LinkProjectile: Acceleration = Normal(Velocity) * 3000 (up to MaxSpeed)
	if (Params.Acceleration > 0.f)
	{
		Movement->Velocity += Movement->Velocity.GetSafeNormal() * Params.Acceleration * DeltaSeconds;
		Movement->Velocity = Movement->Velocity.GetClampedToMaxSize(Movement->MaxSpeed);
	}
	if (Params.Kind != EArenaProjKind::Rocket)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	FVector& V = Movement->Velocity;
	if (Now >= NextSteerTime)
	{
		NextSteerTime = Now + 0.1f;
		FVector Accel = FVector::ZeroVector;
		if (Flock.Num() > 0)
		{
			// RocketProj.Timer (flock of a tight volley): keep heading along Dir while pulling towards / curling around mates
			V = Params.Speed * (InitialDir * 0.5f * Params.Speed + V).GetSafeNormal();
			for (const TWeakObjectPtr<AArenaProjectile>& M : Flock)
			{
				if (!M.IsValid())
				{
					continue;
				}
				const FVector ForceDir = M->GetActorLocation() - GetActorLocation();
				// FlockStiffness -40, FlockRadius 12 UU, FlockMaxForce 600 UU, FlockCurlForce 450 UU (cm: x2)
				const float ForceMag = -40.f * (2.f * 24.f - ForceDir.Size());
				Accel = ForceDir.GetSafeNormal() * FMath::Min(ForceMag, 1200.f);
				const FVector CurlDir = FVector::CrossProduct(M->Movement ? M->Movement->Velocity : FVector::ZeroVector, ForceDir).GetSafeNormal();
				Accel += (bCurl == M->bCurl ? 1.f : -1.f) * CurlDir * 900.f;
			}
		}
		if (AActor* Target = Params.SeekTarget.Get())
		{
			// SeekingRocketProj.Timer: turn towards the target while it is ahead of the launch direction
			const FVector ForceDir = (Target->GetActorLocation() - GetActorLocation()).GetSafeNormal();
			if (FVector::DotProduct(ForceDir, InitialDir) > 0.f)
			{
				const float VelMag = V.Size();
				const FVector NewDir = (ForceDir * 0.5f * VelMag + V).GetSafeNormal();
				V = VelMag * NewDir;
				Accel += 10.f * NewDir;
			}
		}
		SteerAccel = Accel;
	}
	V += SteerAccel * DeltaSeconds;
}

void AArenaProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(GraceTimer);
	TouchIgnore[0].Reset();
	TouchIgnore[1].Reset();
	EndInstigatorGrace();
	Super::EndPlay(EndPlayReason);
}

void AArenaProjectile::EndInstigatorGrace()
{
	APawn* P = GracePawn.Get();
	if (P && (TouchIgnore[0].Get() == P || TouchIgnore[1].Get() == P))
	{
		return; // a reflected copy never touches its reflector (undone in EndPlay)
	}
	if (P)
	{
		if (Collision)
		{
			Collision->IgnoreActorWhenMoving(P, false);
		}
		if (UPrimitiveComponent* PawnRoot = Cast<UPrimitiveComponent>(P->GetRootComponent()))
		{
			PawnRoot->IgnoreActorWhenMoving(this, false);
		}
	}
	GracePawn.Reset();
}

bool AArenaProjectile::IsInGrace(const AActor* Other) const
{
	if (Other && (TouchIgnore[0].Get() == Other || TouchIgnore[1].Get() == Other))
	{
		return true;
	}
	return Other && GracePawn.Get() == Other && GetWorld() && GetWorld()->GetTimeSeconds() < GraceEndTime;
}

bool AArenaProjectile::TryShieldReflect(AActor* Other, const FVector& HitLocation)
{
	AArenaCharacter* Char = Cast<AArenaCharacter>(Other);
	UWorld* World = GetWorld();
	if (!Char || !World || !Movement)
	{
		return false;
	}
	FVector RefNormal;
	if (!Char->CheckShieldReflect(HitLocation, RefNormal, int32(Params.Damage * 0.25f)))
	{
		return false;
	}
	// LinkProjectile mirrors its flight about the view direction; ShockProjectile flies straight along it.
	const FVector X = Movement->Velocity.GetSafeNormal();
	const FVector RefDir = Params.Kind == EArenaProjKind::ShockBall ? RefNormal : (X - 2.f * RefNormal * FVector::DotProduct(X, RefNormal)).GetSafeNormal();
	const FTransform T(RefDir.Rotation(), HitLocation + RefDir * 40.f);
	bExploded = true; // the copy must not touch (and detonate on) this dying original
	if (AArenaProjectile* P = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, Char, Char,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
	{
		FArenaProjectileParams NewParams = Params;
		P->Init(NewParams, InstigatorController.Get(), DamageScale);
		// UE2 keeps the original Instigator through every reflection; Owner is the last reflector.
		P->TouchIgnore[0] = TouchIgnore[0].IsValid() ? TouchIgnore[0].Get() : static_cast<AActor*>(GetInstigator());
		P->TouchIgnore[1] = Char;
		P->ReflectGeneration = ReflectGeneration + 1;
		for (const TWeakObjectPtr<AActor>& Ign : P->TouchIgnore)
		{
			if (Ign.IsValid() && P->Collision)
			{
				P->Collision->IgnoreActorWhenMoving(Ign.Get(), true);
			}
		}
		P->FinishSpawning(T);
	}
	Destroy();
	return true;
}

void AArenaProjectile::OnCollisionHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
	if (bExploded || !IsValid(this) || IsActorBeingDestroyed())
	{
		return;
	}
	if (OtherActor == this || IsInGrace(OtherActor) || Cast<AArenaProjectile>(OtherActor))
	{
		return;
	}

	const FVector HitLoc = Hit.bBlockingHit ? FVector(Hit.Location) : GetActorLocation();
	const FVector HitNormal = Hit.bBlockingHit ? FVector(Hit.ImpactNormal) : FVector::UpVector;

	if (Cast<APawn>(OtherActor))
	{
		if ((Params.Kind == EArenaProjKind::ShockBall || Params.Kind == EArenaProjKind::LinkBolt) && TryShieldReflect(OtherActor, HitLoc))
		{
			return;
		}
		switch (Params.Kind)
		{
		case EArenaProjKind::FlakChunk:
		{
			// FlakChunk.ProcessTouch: only while faster than 200 UU/s; damage fades by DamageAtten per second after 1 s
			const float Speed = Movement ? Movement->Velocity.Size() : 0.f;
			if (Speed > 400.f)
			{
				const float Age = GetWorld()->GetTimeSeconds() - SpawnTime;
				const float Dmg = FMath::Max(5.f, FMath::FloorToFloat(Params.Damage - 5.f * FMath::Max(0.f, Age - 1.f)));
				FArenaHitInfo Info;
				Info.WeaponName = Params.WeaponName;
				Info.Momentum = Movement->Velocity.GetSafeNormal() * Params.Momentum;
				ArenaDamage::ApplyDamage(OtherActor, Dmg * DamageScale, InstigatorController.Get(), this, Info);
				bExploded = true;
				Destroy();
			}
			return;
		}
		case EArenaProjKind::Grenade:
			Explode(HitLoc, HitNormal, OtherActor);
			return;
		case EArenaProjKind::FlakShell:
			FlakShellExplode(HitLoc, HitNormal);
			return;
		case EArenaProjKind::BioGlob:
			BioBlowUp();
			return;
		default:
			// Direct pawn hit: always detonate (goo too).
			Explode(HitLoc, HitNormal, OtherActor);
			return;
		}
	}

	if (bStuck)
	{
		return;
	}

	switch (Params.Kind)
	{
	case EArenaProjKind::Grenade:
	case EArenaProjKind::FlakChunk:
		return; // handled in OnProjectileBounced
	case EArenaProjKind::FlakShell:
		FlakShellExplode(HitLoc, HitNormal);
		return;
	case EArenaProjKind::BioGlob:
		BioLanded(Hit);
		return;
	default:
		break;
	}

	if (Params.bSticky)
	{
		// Stick handled in OnProjectileStopped (movement stops on this impact because sticky goo doesn't bounce),
		// but if bouncing was configured anyway, stick now.
		if (Params.Bounces != 0)
		{
			StickTo(Hit);
		}
		return;
	}

	if (Params.Bounces == 0)
	{
		Explode(HitLoc, HitNormal, nullptr);
	}
	// Bouncing projectiles are counted in OnProjectileBounced.
}

void AArenaProjectile::OnCollisionOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bExploded || OtherActor == this || IsInGrace(OtherActor))
	{
		return;
	}
	if (AArenaProjectile* OtherProj = Cast<AArenaProjectile>(OtherActor))
	{
		if (OtherProj->bExploded)
		{
			return;
		}
		// BioGlob.Flying.ProcessTouch: a flying glob merges into a glob on the ground
		if (Params.Kind == EArenaProjKind::BioGlob && !bStuck && OtherProj->IsBioOnGround())
		{
			OtherProj->BioMergeWithGlob(Params.GoopLevel);
			bExploded = true;
			Destroy();
			return;
		}
		// Shock cores (bProjTarget) are hit by other projectiles: ShockProjectile / RocketProj.ProcessTouch
		if (OtherProj->GetParams().bProjTarget && !bStuck && Params.Kind != EArenaProjKind::BioGlob && Params.Kind != EArenaProjKind::FlakChunk)
		{
			const FVector Loc = GetActorLocation();
			if (Params.Kind == EArenaProjKind::ShockBall)
			{
				OtherProj->Explode(OtherProj->GetActorLocation(), (OtherProj->GetActorLocation() - Loc).GetSafeNormal(), nullptr);
			}
			Explode(Loc, (Loc - OtherProj->GetActorLocation()).GetSafeNormal(), nullptr);
		}
		return;
	}
	if (!bStuck)
	{
		return;
	}
	if (Cast<APawn>(OtherActor))
	{
		if (Params.Kind == EArenaProjKind::BioGlob)
		{
			// BioGlob.OnGround.ProcessTouch: blow up TouchDetonationDelay (0.15 s) later
			TWeakObjectPtr<AArenaProjectile> WeakThis(this);
			GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->BioBlowUp();
				}
			}), 0.15f, false);
			return;
		}
		Explode(GetActorLocation(), StuckNormal, OtherActor);
	}
}

void AArenaProjectile::OnProjectileBounced(const FHitResult& ImpactResult, const FVector& ImpactVelocity)
{
	if (bExploded)
	{
		return;
	}
	if (Cast<APawn>(ImpactResult.GetActor()))
	{
		return; // handled by OnCollisionHit
	}
	const FVector N = ImpactResult.ImpactNormal.GetSafeNormal();

	if (Params.Kind == EArenaProjKind::Grenade)
	{
		// Grenade.HitWall: fuse (ExplodeTimer 2 s) on the first bounce, VNorm x -DampenFactor + (V - VNorm) x DampenFactorParallel
		if (!bFuseSet)
		{
			bFuseSet = true;
			TWeakObjectPtr<AArenaProjectile> WeakThis(this);
			GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->Explode(WeakThis->GetActorLocation(), FVector::UpVector, nullptr);
				}
			}), 2.f, false);
		}
		const FVector VNorm = FVector::DotProduct(ImpactVelocity, N) * N;
		Movement->Velocity = -VNorm * 0.5f + (ImpactVelocity - VNorm) * 0.8f;
		if (Movement->Velocity.Size() < 40.f)
		{
			Movement->StopMovementImmediately();
		}
		return;
	}
	if (Params.Kind == EArenaProjKind::FlakChunk)
	{
		// FlakChunk.HitWall: falls from now on; bounces left: reflect x 0.65; none: stays where it lands for 1 s
		Movement->ProjectileGravityScale = Params.GravityScale > 0.f ? Params.GravityScale : ArenaTuning::GravityScale;
		if (BouncesLeft > 0)
		{
			--BouncesLeft;
			Movement->Velocity = 0.65f * (ImpactVelocity - 2.f * N * FVector::DotProduct(ImpactVelocity, N));
		}
		else
		{
			Movement->StopMovementImmediately();
			Movement->SetComponentTickEnabled(false);
			bStuck = true;
			SetLifeSpan(1.f);
		}
		return;
	}

	++BounceCount;
	if (Params.bShard && BounceCount == 1)
	{
		DamageScale *= 0.5f;
	}

	if (Params.bSticky)
	{
		StickTo(ImpactResult);
		return;
	}

	if (Params.Bounces > 0 && BounceCount > Params.Bounces)
	{
		const FVector Loc = ImpactResult.bBlockingHit ? FVector(ImpactResult.Location) : GetActorLocation();
		if (Params.DamageRadius > 0.f)
		{
			Explode(Loc, ImpactResult.ImpactNormal, nullptr);
		}
		else
		{
			Fizzle(Loc, ImpactResult.ImpactNormal);
		}
	}
}

void AArenaProjectile::OnProjectileStopped(const FHitResult& ImpactResult)
{
	if (bExploded)
	{
		return;
	}
	if (Params.bSticky && !bStuck && !Cast<APawn>(ImpactResult.GetActor()))
	{
		StickTo(ImpactResult);
	}
	// Bouncing grenades that come to rest just sit until a pawn touches them or their lifespan ends.
}

void AArenaProjectile::StickTo(const FHitResult& Hit)
{
	if (bStuck || bExploded)
	{
		return;
	}
	bStuck = true;
	StuckNormal = Hit.bBlockingHit ? FVector(Hit.ImpactNormal) : FVector::UpVector;

	if (Movement && Movement->UpdatedComponent)
	{
		Movement->StopMovementImmediately();
		Movement->ProjectileGravityScale = 0.f;
		Movement->SetComponentTickEnabled(false);
	}

	if (ArtSkel)
	{
		// Settle the blob on the surface and let it drip.
		ArtSkel->SetWorldRotation(FRotationMatrix::MakeFromZ(StuckNormal).Rotator());
		if (UAnimSequence* Anim = ArtDesc && ArtDesc->StuckAnim ? ArenaArt::Load<UAnimSequence>(ArtDesc->StuckAnim) : nullptr)
		{
			ArtSkel->PlayAnimation(Anim, true);
		}
	}

	// Flatten slightly against the surface.
	if (Visual)
	{
		const float S = FMath::Max(1.f, Params.Size) / 50.f;
		Visual->SetWorldRotation(FRotationMatrix::MakeFromZ(StuckNormal).Rotator());
		Visual->SetWorldScale3D(FVector(S * 1.25f, S * 1.25f, S * 0.6f));
	}

	// Follow moving geometry.
	if (UPrimitiveComponent* HitComp = Hit.GetComponent())
	{
		if (HitComp->GetMobility() == EComponentMobility::Movable)
		{
			AttachToComponent(HitComp, FAttachmentTransformRules::KeepWorldTransform);
		}
	}

	// Pawns now overlap us (trigger the blast) instead of bumping into us.
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetSphereRadius(FMath::Max(Collision->GetUnscaledSphereRadius(), Params.Size * 1.1f));
	Collision->UpdateOverlaps();

	ArenaAssets::PlaySoundAt(this, TEXT("S_Impact"), GetActorLocation(), 0.35f, 1.4f);
}

void AArenaProjectile::Fizzle(const FVector& Location, const FVector& Normal)
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	ArenaFX::SpawnImpact(GetWorld(), Location, Normal, Params.Color, FMath::Max(6.f, Params.Size * 1.5f), GetArtEffect(false));
	Destroy();
}

FVector AArenaProjectile::LastExplodeLocation = FVector::ZeroVector;
FVector AArenaProjectile::LastExplodeInstigatorLocation = FVector::ZeroVector;

void AArenaProjectile::Explode(const FVector& HitLocation, const FVector& HitNormal, AActor* DirectHitActor)
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;

	UWorld* World = GetWorld();
	if (!World)
	{
		Destroy();
		return;
	}

	const FVector N = HitNormal.IsNearlyZero() ? FVector::UpVector : HitNormal.GetSafeNormal();
	// on a pawn the blast is where the projectile touched it (UE2 ProcessTouch HitLocation); off walls slightly out
	const FVector Loc = Cast<APawn>(DirectHitActor) ? HitLocation : HitLocation + N * 4.f;
	LastExplodeLocation = Loc;
	LastExplodeInstigatorLocation = GetInstigator() ? GetInstigator()->GetActorLocation() : FVector::ZeroVector;
	AController* Ctrl = InstigatorController.Get();
	const float Dmg = Params.Damage * DamageScale;

	if (Params.DamageRadius > 0.f)
	{
		HurtRadiusAt(Loc, N, Dmg, Params.DamageRadius, Params.Momentum, Params.WeaponName);
	}
	else
	{
		if (IsValid(DirectHitActor))
		{
			FArenaHitInfo Info;
			Info.WeaponName = Params.WeaponName;
			FVector Dir = Movement ? Movement->Velocity.GetSafeNormal() : FVector::ZeroVector;
			if (Dir.IsNearlyZero())
			{
				Dir = (DirectHitActor->GetActorLocation() - GetActorLocation()).GetSafeNormal();
			}
			Info.Momentum = Dir * Params.Momentum;
			Info.bRadial = false;
			Info.HitLocation = HitLocation;
			ArenaDamage::ApplyDamage(DirectHitActor, Dmg, Ctrl, this, Info);
		}
		ArenaFX::SpawnImpact(World, Loc, N, Params.Color, FMath::Max(10.f, Params.Size * 2.5f), GetArtEffect(false));
		const FName Snd = Params.ExplosionSound.IsNone() ? FName(TEXT("S_Impact")) : Params.ExplosionSound;
		ArenaAssets::PlaySoundAt(this, Snd, Loc, 0.6f, FMath::FRandRange(0.9f, 1.15f));
	}

	Destroy();
}

void AArenaProjectile::HurtRadiusAt(const FVector& Loc, const FVector& N, float Damage, float Radius, float Momentum, const FString& Name)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	ArenaDamage::ApplyRadialDamage(World, Loc, Damage, Radius, Momentum, InstigatorController.Get(), this, Name);
	ArenaFX::SpawnExplosion(World, Loc, Params.Color, Radius, GetArtEffect(true), N);
	const FName Snd = Params.ExplosionSound.IsNone() ? FName(TEXT("S_Explosion")) : Params.ExplosionSound;
	ArenaAssets::PlaySoundAt(this, Snd, Loc, 1.f, FMath::FRandRange(0.92f, 1.08f));
}

void AArenaProjectile::FlakShellExplode(const FVector& HitLocation, const FVector& HitNormal)
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	UWorld* World = GetWorld();
	const FVector N = HitNormal.IsNearlyZero() ? FVector::UpVector : HitNormal.GetSafeNormal();
	// FlakShell.Explode: HurtRadius(Damage, 220 UU), then 6 FlakChunks within +-16000 units of each rotation axis
	HurtRadiusAt(HitLocation + N * 4.f, N, Params.Damage * DamageScale, Params.DamageRadius, Params.Momentum, Params.WeaponName);
	const FVector Start = GetActorLocation() + 20.f * N;
	const FRotator Base = Movement && !Movement->Velocity.IsNearlyZero() ? Movement->Velocity.Rotation() : GetActorRotation();
	FArenaProjectileParams Chunk = ChunkParams;
	Chunk.WeaponName = Params.WeaponName;
	for (int32 i = 0; World && Chunk.Speed > 0.f && i < 6; ++i)
	{
		auto Off = []() { return (FMath::FRand() * 32000.f - 16000.f) * 360.f / 65536.f; };
		const FRotator R(Base.Pitch + Off(), Base.Yaw + Off(), Base.Roll + Off());
		const FTransform T(R, Start);
		if (AArenaProjectile* P = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, GetOwner(), GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			P->Init(Chunk, InstigatorController.Get(), DamageScale);
			P->FinishSpawning(T);
		}
	}
	Destroy();
}

void AArenaProjectile::BioSetGoopLevel(int32 NewLevel)
{
	Params.GoopLevel = FMath::Max(1, NewLevel);
	const float GoopVolume = FMath::Sqrt(float(Params.GoopLevel));
	if (Visual)
	{
		Visual->SetRelativeScale3D(FVector(FMath::Max(1.f, Params.Size) / 50.f * GoopVolume));
	}
	if (ArtSkel)
	{
		ArtSkel->SetRelativeScale3D(FVector(ArtBaseScale * GoopVolume));
	}
}

void AArenaProjectile::BioLanded(const FHitResult& Hit)
{
	if (bStuck || bExploded)
	{
		return;
	}
	SurfaceNormal = Hit.bBlockingHit ? FVector(Hit.ImpactNormal) : FVector::UpVector;
	// BioGlob.Flying.Landed: goop above the core level (MaxGoopLevel 5 + Rand3 - 3) splashes off as globlings
	const int32 CoreGoopLevel = BioRand3 + 5 - 3;
	if (Params.GoopLevel > CoreGoopLevel)
	{
		BioSplashGlobs(Params.GoopLevel - CoreGoopLevel);
		BioSetGoopLevel(CoreGoopLevel);
	}
	StickTo(Hit);
	// goop on the ground: collision 10 UU x sqrt(level), a projectile target, blows up after RestTime 2.25 s
	Collision->SetSphereRadius(20.f * FMath::Sqrt(float(Params.GoopLevel)));
	Collision->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
	TWeakObjectPtr<AArenaProjectile> WeakThis(this);
	const bool bCeiling = FVector::DotProduct(SurfaceNormal, FVector(0.f, 0.f, -1.f)) > 0.7f;
	GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis, bCeiling]()
	{
		if (WeakThis.IsValid())
		{
			// OnGround.AnimEnd: on a ceiling the glob drips after DripTime 1.8 s, else it blows up after RestTime
			if (bCeiling)
			{
				WeakThis->BioDrip();
			}
			else
			{
				WeakThis->BioBlowUp();
			}
		}
	}), bCeiling ? 1.8f : 2.25f, false);
}

void AArenaProjectile::BioDrip()
{
	if (!bStuck || bExploded || !Movement)
	{
		return;
	}
	// OnGround.Timer (bDrip): fall again with Velocity = Gravity x 0.2
	bStuck = false;
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Weapon, ECR_Ignore);
	Collision->SetSphereRadius(FMath::Max(1.f, Params.CollisionRadius));
	Movement->SetComponentTickEnabled(true);
	Movement->SetUpdatedComponent(Collision);
	Movement->ProjectileGravityScale = Params.GravityScale;
	Movement->Velocity = FVector(0.f, 0.f, Movement->GetGravityZ() * 0.2f);
}

void AArenaProjectile::BioMergeWithGlob(int32 AdditionalGoopLevel)
{
	if (!bStuck || bExploded)
	{
		return;
	}
	// BioGlob.OnGround.MergeWithGlob: above MaxGoopLevel 5 the extra (plus 0-2) splashes off
	int32 NewGoopLevel = AdditionalGoopLevel + Params.GoopLevel;
	if (NewGoopLevel > 5)
	{
		BioRand3 = (BioRand3 + 1) % 3;
		const int32 ExtraSplash = BioRand3;
		BioSplashGlobs(NewGoopLevel - 5 + ExtraSplash);
		NewGoopLevel = 5 - ExtraSplash;
	}
	BioSetGoopLevel(NewGoopLevel);
	Collision->SetSphereRadius(20.f * FMath::Sqrt(float(Params.GoopLevel)));
	TWeakObjectPtr<AArenaProjectile> WeakThis(this);
	GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
	{
		if (WeakThis.IsValid())
		{
			WeakThis->BioBlowUp();
		}
	}), 2.25f, false);
}

void AArenaProjectile::BioSplashGlobs(int32 NumGloblings)
{
	UWorld* World = GetWorld();
	if (!World || NumGloblings <= 0)
	{
		return;
	}
	const float GoopVolume = FMath::Sqrt(float(Params.GoopLevel));
	FArenaProjectileParams G = Params;
	G.GoopLevel = 1;
	for (int32 i = 0; i < NumGloblings; ++i)
	{
		// SplashGlobs: GloblingSpeed 200 UU + FRand x 150 along (SurfaceNormal + VRand x 0.8)
		const FVector Loc = GetActorLocation() + GoopVolume * (2.f * 2.f + 8.f) * SurfaceNormal;
		const FVector Vel = (400.f + FMath::FRand() * 300.f) * (SurfaceNormal + FMath::VRand() * 0.8f);
		const FTransform T(Vel.Rotation(), Loc);
		G.Speed = Vel.Size();
		G.MaxSpeed = FMath::Max(G.MaxSpeed, G.Speed);
		if (AArenaProjectile* P = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, GetOwner(), GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			P->Init(G, InstigatorController.Get(), DamageScale);
			P->FinishSpawning(T);
		}
	}
}

void AArenaProjectile::BioBlowUp()
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	// BioGlob.BlowUp: Damage = BaseDamage + Damage x GoopLevel, radius and momentum x sqrt(GoopLevel)
	const float GoopVolume = FMath::Sqrt(float(Params.GoopLevel));
	const float Dmg = (Params.BaseDamage + Params.Damage * float(Params.GoopLevel)) * DamageScale;
	const FVector N = bStuck ? StuckNormal : FVector::UpVector;
	HurtRadiusAt(GetActorLocation() + N * 2.f, N, Dmg, Params.DamageRadius * GoopVolume, Params.Momentum * GoopVolume, Params.WeaponName);
	Destroy();
}

void AArenaProjectile::TakeProjectileDamage(AController* DamageInstigator, bool bShockBeam)
{
	if (bExploded)
	{
		return;
	}
	if (Params.Kind == EArenaProjKind::BioGlob && bStuck)
	{
		// OnGround.TakeDamage with a goop detonating damage type: blow up 0.1 s later
		TWeakObjectPtr<AArenaProjectile> WeakThis(this);
		GetWorldTimerManager().SetTimer(KindTimer, FTimerDelegate::CreateWeakLambda(this, [WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->BioBlowUp();
			}
		}), 0.1f, false);
		return;
	}
	if (bShockBeam && Params.bComboCore)
	{
		TriggerCombo(DamageInstigator);
	}
}

bool AArenaProjectile::TriggerCombo(AController* ComboInstigator)
{
	if (!Params.bComboCore || bExploded)
	{
		return false;
	}
	bExploded = true;

	UWorld* World = GetWorld();
	const FVector Loc = GetActorLocation();
	AController* Ctrl = ComboInstigator ? ComboInstigator : InstigatorController.Get();
	const FString ComboName = Params.WeaponName.IsEmpty() ? FString(TEXT("Combo")) : Params.WeaponName + TEXT(" Combo");

	if (World)
	{
		// ShockProjectile.SuperExplosion: HurtRadius(ComboDamage, ComboRadius, ComboMomentumTransfer)
		ArenaDamage::ApplyRadialDamage(World, Loc, Params.ComboDamage * DamageScale, Params.ComboRadius,
			Params.ComboMomentum > 0.f ? Params.ComboMomentum : FMath::Max(Params.Momentum * 2.f, 900.f), Ctrl, this, ComboName);
		if (!ArtDesc || !ArenaArtFX::SpawnEffect(World, TEXT("Explosion.ShockCombo"), Loc, FRotator::ZeroRotator, Params.ComboRadius / 550.f))
		{
			ArenaFX::SpawnExplosion(World, Loc, Params.Color, Params.ComboRadius);
			ArenaFX::SpawnExplosion(World, Loc, FMath::Lerp(Params.Color, FLinearColor::White, 0.6f), Params.ComboRadius * 0.55f);
		}
		ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), Loc, 1.3f, 1.f);
	}
	Destroy();
	return true;
}

void AArenaProjectile::LifeSpanExpired()
{
	if (bExploded)
	{
		Super::LifeSpanExpired();
		return;
	}
	if (Params.Kind == EArenaProjKind::BioGlob)
	{
		BioBlowUp();
	}
	else if (Params.Kind == EArenaProjKind::FlakChunk || Params.Kind == EArenaProjKind::LinkBolt)
	{
		bExploded = true;
		Destroy();
	}
	else if (Params.DamageRadius > 0.f || Params.bSticky)
	{
		Explode(GetActorLocation(), bStuck ? StuckNormal : FVector::UpVector, nullptr);
	}
	else
	{
		bExploded = true;
		Destroy();
	}
}

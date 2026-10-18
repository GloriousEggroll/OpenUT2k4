#include "ArenaBR_BallLauncher.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaBR_Ball.h"
#include "ArenaTranslocator.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerState.h"

namespace ArenaBR
{
	constexpr float LockRange = 5000.f;
	constexpr float LockMinDot = 0.8f;
}

AArenaBR_BallLauncher::AArenaBR_BallLauncher()
{
	WeaponName = TEXT("Ball Launcher");
	Slot = 0;
	MaxAmmo = 1;
	InitialAmmo = 1;
	AmmoPickupAmount = 0;
	bInfiniteAmmo = true;
	Priority = 5.f;
	WeaponColor = FLinearColor(0.85f, 0.65f, 0.2f);
	MuzzleOffset = FVector(62.f, 14.f, -12.f);

	// Fire modes only describe the weapon for HUD / bots; StartFire is overridden.
	FArenaFireMode& P = FireModes[0];
	P.FireType = EArenaFireType::Projectile;
	P.Damage = 0.f;
	P.RefireTime = 0.5f;
	P.AmmoPerShot = 0;
	P.Color = FLinearColor(1.f, 0.8f, 0.3f);
	P.Projectile.Speed = PassSpeed;
	P.Projectile.MaxSpeed = PassSpeed;
	FArenaFireMode& A = FireModes[1];
	A.FireType = EArenaFireType::Zoom;
	A.AmmoPerShot = 0;
	A.Color = FLinearColor(0.3f, 1.f, 0.5f);
}

AArenaBR_Ball* AArenaBR_BallLauncher::GetCarriedBall() const
{
	return ArenaOwner ? Cast<AArenaBR_Ball>(ArenaOwner->GetCarriedObject()) : nullptr;
}

bool AArenaBR_BallLauncher::HasAmmo() const
{
	return GetCarriedBall() != nullptr;
}

bool AArenaBR_BallLauncher::HasAmmoFor(uint8 Mode) const
{
	return Mode < 2 && HasAmmo();
}

void AArenaBR_BallLauncher::StartFire(uint8 Mode)
{
	if (Mode > 1 || !bEquipped || !ArenaOwner || ArenaOwner->IsDead() || !HasAuthority())
	{
		return;
	}
	// Bots never throw by holding fire at enemies: UArenaRules_BR decides their passes and shots.
	const AController* C = GetOwnerController();
	if (!C || !C->IsPlayerController())
	{
		return;
	}
	if (Mode == 0)
	{
		ThrowPass();
	}
	else
	{
		TryLock();
	}
}

FVector AArenaBR_BallLauncher::GetThrowStart() const
{
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const FVector Wanted = Eye + Dir * 60.f - FVector(0.f, 0.f, 15.f);
	// Don't start the flight inside a wall right in front of the face
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBRThrowStart), false, ArenaOwner);
	if (GetWorld()->SweepSingleByObjectType(Hit, ArenaOwner->GetActorLocation(), Wanted, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic), FCollisionShape::MakeSphere(24.f), Query))
	{
		return Hit.Location;
	}
	return Wanted;
}

bool AArenaBR_BallLauncher::Launch(const FVector& Velocity, AArenaCharacter* HomingTarget)
{
	AArenaBR_Ball* Ball = GetCarriedBall();
	const float Now = GetWorldTime();
	if (!Ball || Now < NextActionTime)
	{
		return false;
	}
	NextActionTime = Now + 0.4f;
	AddRecoil(6.f);
	// BallShoot.ModeDoFire with xBombingRun.bBallDrainsTransloc (on by default): the thrower's translocator is drained
	if (ArenaOwner)
	{
		for (AArenaWeapon* W : ArenaOwner->GetWeapons())
		{
			if (AWeap_Translocator* T = Cast<AWeap_Translocator>(W))
			{
				T->DrainCharges();
			}
		}
	}
	Ball->LaunchFrom(ArenaOwner, GetThrowStart(), Velocity, HomingTarget);
	LockedTarget.Reset();
	// The launcher removes itself on the next tick (the ball is gone)
	return true;
}

bool AArenaBR_BallLauncher::ThrowPass()
{
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	AArenaCharacter* Target = LockedTarget.Get();
	if (Target && (!Target->IsAlive() || FVector::Dist(Target->GetActorLocation(), ArenaOwner->GetActorLocation()) > ArenaBR::LockRange * 1.3f))
	{
		Target = nullptr;
	}
	FVector Velocity;
	if (Target)
	{
		const FVector ToTarget = (Target->GetActorLocation() - Eye).GetSafeNormal();
		Velocity = (ToTarget + FVector(0.f, 0.f, 0.12f)).GetSafeNormal() * PassSpeed;
	}
	else
	{
		Velocity = Dir * PassSpeed + FVector(0.f, 0.f, 180.f) + ArenaOwner->GetVelocity() * 0.3f;
	}
	return Launch(Velocity, Target);
}

AArenaCharacter* AArenaBR_BallLauncher::TryLock()
{
	if (!ArenaOwner)
	{
		return nullptr;
	}
	FVector Eye, Dir;
	GetAim(Eye, Dir);
	const int32 MyTeam = ArenaOwner->GetTeam();
	AArenaCharacter* Best = nullptr;
	float BestDot = ArenaBR::LockMinDot;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (C == ArenaOwner || !C->IsAlive() || C->GetTeam() != MyTeam)
		{
			continue;
		}
		const FVector To = C->GetActorLocation() - Eye;
		const float Dist = To.Size();
		if (Dist > ArenaBR::LockRange || Dist < 1.f)
		{
			continue;
		}
		const float Dot = FVector::DotProduct(To / Dist, Dir);
		if (Dot <= BestDot)
		{
			continue;
		}
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBRLock), false, ArenaOwner);
		Query.AddIgnoredActor(C);
		if (GetWorld()->LineTraceSingleByObjectType(Hit, Eye, C->GetActorLocation(), FCollisionObjectQueryParams(ECC_WorldStatic), Query))
		{
			continue;
		}
		Best = C;
		BestDot = Dot;
	}
	LockedTarget = Best;

	AController* MyController = GetOwnerController();
	AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	if (GM && MyController && MyController->IsPlayerController())
	{
		if (Best)
		{
			const APlayerState* PS = Best->GetPlayerState();
			GM->SendCenterMessage(MyController, FString::Printf(TEXT("Pass locked on %s"), PS ? *PS->GetPlayerName() : TEXT("teammate")), FLinearColor(0.4f, 1.f, 0.5f), 1.5f);
		}
		else
		{
			GM->SendCenterMessage(MyController, TEXT("No teammate in sight"), FLinearColor(1.f, 0.6f, 0.3f), 1.2f);
		}
	}
	if (Best)
	{
		ArenaAssets::PlaySound2D(this, TEXT("S_MenuSelect"), 0.6f, 1.3f);
	}
	return Best;
}

bool AArenaBR_BallLauncher::PassTo(AArenaCharacter* Teammate)
{
	if (!Teammate || !ArenaOwner)
	{
		return false;
	}
	LockedTarget = Teammate;
	const FVector Start = GetThrowStart();
	const FVector ToTarget = (Teammate->GetActorLocation() - Start).GetSafeNormal();
	return Launch((ToTarget + FVector(0.f, 0.f, 0.15f)).GetSafeNormal() * PassSpeed, Teammate);
}

bool AArenaBR_BallLauncher::ThrowAt(const FVector& TargetPoint)
{
	AArenaBR_Ball* Ball = GetCarriedBall();
	if (!Ball || !ArenaOwner)
	{
		return false;
	}
	const FVector Start = GetThrowStart();
	const FVector Delta = TargetPoint - Start;
	const FVector Flat(Delta.X, Delta.Y, 0.f);
	const float D = Flat.Size();
	const float H = Delta.Z;
	const float G = FMath::Abs(GetWorld()->GetGravityZ() * Ball->FlightGravityScale);
	if (D < 1.f || G < 1.f)
	{
		return Launch(Delta.GetSafeNormal() * PassSpeed, nullptr);
	}
	const FVector FlatDir = Flat / D;
	for (float V = 1400.f; V <= 3400.f; V += 200.f)
	{
		const float V2 = V * V;
		const float Disc = V2 * V2 - G * (G * D * D + 2.f * H * V2);
		if (Disc < 0.f)
		{
			continue;
		}
		const float Angle = FMath::Atan((V2 - FMath::Sqrt(Disc)) / (G * D)); // low arc
		const FVector Velocity = FlatDir * (V * FMath::Cos(Angle)) + FVector(0.f, 0.f, V * FMath::Sin(Angle));
		return Launch(Velocity, nullptr);
	}
	// Out of range: throw as far as possible towards it
	return Launch((FlatDir + FVector(0.f, 0.f, 1.f)).GetSafeNormal() * 3400.f, nullptr);
}

AArenaBR_BallLauncher* AArenaBR_BallLauncher::GiveTo_BR(AArenaCharacter* Character)
{
	if (!Character || Character->IsDead())
	{
		return nullptr;
	}
	AArenaBR_BallLauncher* Launcher = Cast<AArenaBR_BallLauncher>(Character->FindWeapon(AArenaBR_BallLauncher::StaticClass()));
	if (!Launcher || Launcher->bRemoving)
	{
		if (Launcher)
		{
			Character->RemoveWeapon(Launcher);
		}
		Character->GiveWeapon(AArenaBR_BallLauncher::StaticClass());
		Launcher = Cast<AArenaBR_BallLauncher>(Character->FindWeapon(AArenaBR_BallLauncher::StaticClass()));
	}
	if (Launcher)
	{
		Launcher->bRemoving = false;
		Launcher->LockedTarget.Reset();
		Character->SwitchToWeapon(Launcher);
	}
	return Launcher;
}

void AArenaBR_BallLauncher::RemoveFrom(AArenaCharacter* Character)
{
	if (!Character)
	{
		return;
	}
	TArray<AArenaBR_BallLauncher*> Launchers;
	for (AArenaWeapon* W : Character->GetWeapons())
	{
		if (AArenaBR_BallLauncher* Launcher = Cast<AArenaBR_BallLauncher>(W))
		{
			Launchers.Add(Launcher);
		}
	}
	for (AArenaBR_BallLauncher* Launcher : Launchers)
	{
		Launcher->bRemoving = true;
		Character->RemoveWeapon(Launcher);
		Launcher->Destroy();
	}
}

void AArenaBR_BallLauncher::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!ArenaOwner || bRemoving || !HasAuthority())
	{
		return;
	}
	if (!GetCarriedBall())
	{
		RemoveFrom(ArenaOwner);
		return;
	}
	// While the ball is held nothing else may be selected
	if (ArenaOwner->IsAlive() && ArenaOwner->GetCurrentWeapon() != this)
	{
		ArenaOwner->SwitchToWeapon(this);
	}
	if (AArenaCharacter* Target = LockedTarget.Get())
	{
		if (!Target->IsAlive() || FVector::Dist(Target->GetActorLocation(), ArenaOwner->GetActorLocation()) > ArenaBR::LockRange * 1.3f)
		{
			LockedTarget.Reset();
		}
	}
	if (LockGlow)
	{
		LockGlow->SetVisibility(bEquipped && LockedTarget.IsValid());
	}
	if (BallGlow && bEquipped)
	{
		const float S = 0.15f + 0.01f * FMath::Sin(GetWorldTime() * 6.f);
		BallGlow->SetRelativeScale3D(FVector(S));
	}
}

void AArenaBR_BallLauncher::BuildViewModel()
{
	const FLinearColor Dark(0.1f, 0.1f, 0.12f);
	// Grip body and a short cradle with two prongs holding the ball
	AddViewPart(ArenaAssets::Cube(), FVector(26.f, 14.f, -19.f), FRotator::ZeroRotator, FVector(0.30f, 0.08f, 0.08f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(22.f, 14.f, -26.f), FRotator(15.f, 0.f, 0.f), FVector(0.06f, 0.04f, 0.12f), Dark);
	AddViewPart(ArenaAssets::Cylinder(), FVector(44.f, 14.f, -17.f), FRotator(90.f, 0.f, 0.f), FVector(0.12f, 0.12f, 0.08f), Dark);
	AddViewPart(ArenaAssets::Cube(), FVector(54.f, 6.f, -13.f), FRotator(0.f, 12.f, 0.f), FVector(0.18f, 0.02f, 0.03f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(54.f, 22.f, -13.f), FRotator(0.f, -12.f, 0.f), FVector(0.18f, 0.02f, 0.03f), WeaponColor);
	AddViewPart(ArenaAssets::Cube(), FVector(30.f, 18.5f, -16.f), FRotator::ZeroRotator, FVector(0.2f, 0.005f, 0.012f), FireModes[0].Color, true);
	BallGlow = AddViewPart(ArenaAssets::Sphere(), FVector(62.f, 14.f, -12.f), FRotator::ZeroRotator, FVector(0.15f), FLinearColor(1.f, 0.78f, 0.25f), true);
	LockGlow = AddViewPart(ArenaAssets::Sphere(), FVector(34.f, 14.f, -12.5f), FRotator::ZeroRotator, FVector(0.03f), FireModes[1].Color, true);
	if (LockGlow)
	{
		LockGlow->SetVisibility(false);
	}
}

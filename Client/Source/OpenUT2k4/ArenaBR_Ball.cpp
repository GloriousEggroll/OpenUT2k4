#include "ArenaBR_Ball.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArtItems.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaRules_BR.h"
#include "ArenaBR_BallLauncher.h"
#include "ArenaTypes.h"
#include "ArenaNavGraph.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"

namespace ArenaBR
{
	static UArenaRules_BR* GetRulesBR(const UObject* WorldContext)
	{
		const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
		const AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
		return GM ? Cast<UArenaRules_BR>(GM->GetRules()) : nullptr;
	}

	static const FLinearColor BallColor(1.f, 0.78f, 0.25f);
}

AArenaBR_Ball::AArenaBR_Ball()
{
	DisplayName = TEXT("Ball");
	AutoReturnTime = 25.f;
	Bounciness = 0.68f;
	FlightGravityScale = 0.8f;
	FlightRadius = 22.f;
	RepickDelay = 0.5f;
	RestHeight = 55.f;
	CarryOffset = FVector(30.f, 28.f, 100.f);
	CarryScale = 0.75f;
	Tags.Add(ArenaNavIgnoreTag());
	if (Trigger)
	{
		Trigger->SetSphereRadius(90.f);
	}
}

void AArenaBR_Ball::BuildModel()
{
	for (UStaticMeshComponent* Part : ModelParts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	ModelParts.Reset();

	using namespace ArenaBR;
	// Imported ball (and its spawn pad at home), else a core sphere with two crossing bands; plus a soft glow shell
	const ArenaArtItems::FBombingRunArt& Art = ArenaArtItems::GetBombingRunArt();
	if (SpawnPad)
	{
		SpawnPad->DestroyComponent();
		SpawnPad = nullptr;
	}
	if (UStaticMeshComponent* Ball = ArenaArtItems::AddItemMesh(this, Model, Art.Ball, 1.f, true))
	{
		ModelParts.Add(Ball);
		// The pad stays at the home spot (the actor root, RestHeight above the floor).
		ArenaArtItems::FItemMesh Pad = Art.SpawnPad;
		Pad.Offset = FVector(0.f, 0.f, -RestHeight);
		SpawnPad = ArenaArtItems::AddItemMesh(this, GetRootComponent(), Pad, 1.f, false);
	}
	else
	{
		AddModelPart(ArenaAssets::Sphere(), FVector::ZeroVector, FVector(40.f), ArenaAssets::MakeSurface(this, BallColor, 0.25f, 0.7f, BallColor * 0.35f), true);
		UMaterialInterface* BandMat = ArenaAssets::MakeSurface(this, FLinearColor(0.08f, 0.08f, 0.1f), 0.4f, 0.8f);
		AddModelPart(ArenaAssets::Cylinder(), FVector::ZeroVector, FVector(43.f, 43.f, 5.f), BandMat, false);
		if (UStaticMeshComponent* Band2 = AddModelPart(ArenaAssets::Cylinder(), FVector::ZeroVector, FVector(43.f, 43.f, 5.f), BandMat, false))
		{
			Band2->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
		}
	}
	CoreGlowMaterial = ArenaAssets::MakeGlow(this, BallColor, 3.f, 0.35f);
	AddModelPart(ArenaAssets::Sphere(), FVector::ZeroVector, FVector(54.f), CoreGlowMaterial, false);

	// Beacon: a tall thin light column so everybody can find the ball (tinted by the carrier's team)
	BeaconMaterial = ArenaAssets::MakeGlow(this, BallColor, 4.f, 0.45f);
	AddModelPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 420.f), FVector(10.f, 10.f, 760.f), BeaconMaterial, false);
	AddModelPart(ArenaAssets::Cone(), FVector(0.f, 0.f, 830.f), FVector(40.f, 40.f, 40.f), BeaconMaterial, false);

	// The carrier sees the ball in its launcher instead of the world model
	for (UStaticMeshComponent* Part : ModelParts)
	{
		if (Part)
		{
			Part->SetOwnerNoSee(true);
		}
	}
	Light->SetLightColor(BallColor);
	BeaconTeam = -2;
	UpdateBeaconColor();
}

void AArenaBR_Ball::UpdateBeaconColor()
{
	const int32 Team = (IsCarried() && Carrier) ? Carrier->GetTeam() : -1;
	if (Team == BeaconTeam)
	{
		return;
	}
	BeaconTeam = Team;
	const FLinearColor Col = (Team == 0 || Team == 1) ? ArenaTeam::Color(Team) : ArenaBR::BallColor;
	if (BeaconMaterial)
	{
		BeaconMaterial->SetVectorParameterValue(TEXT("Color"), Col);
	}
	if (CoreGlowMaterial)
	{
		CoreGlowMaterial->SetVectorParameterValue(TEXT("Color"), Col);
	}
	if (Light)
	{
		Light->SetLightColor(Col);
	}
}

FString AArenaBR_Ball::GetCarrierHudText() const
{
	return TEXT("BALL IN HAND  -  FIRE: PASS   ALT-FIRE: LOCK TEAMMATE");
}

float AArenaBR_Ball::GetReturnCountdown() const
{
	if (GetCarryState() != EArenaCarryState::Dropped)
	{
		return 0.f;
	}
	return FMath::Max(0.f, GetAutoReturnTime() - GetTimeOnGround());
}

void AArenaBR_Ball::SetNetMesh(UStaticMesh* Mesh, float DrawScale)
{
	UStaticMeshComponent* Part = ModelParts.Num() > 0 ? ModelParts[0].Get() : nullptr;
	if (!Part)
	{
		return;
	}
	if (!OwnBallMesh)
	{
		OwnBallMesh = Part->GetStaticMesh();
		OwnBallTransform = Part->GetRelativeTransform();
	}
	if (Mesh)
	{
		if (Part->GetStaticMesh() != Mesh)
		{
			Part->SetStaticMesh(Mesh);
		}
		Part->SetRelativeTransform(FTransform(OwnBallTransform.GetRotation(), FVector::ZeroVector, FVector(FMath::Max(0.1f, DrawScale))));
	}
	else if (Part->GetStaticMesh() != OwnBallMesh)
	{
		Part->SetStaticMesh(OwnBallMesh);
		Part->SetRelativeTransform(OwnBallTransform);
	}
}

void AArenaBR_Ball::ClearThrowInfo()
{
	LastThrower.Reset();
	LastThrowerTeam = -1;
	HomingTarget.Reset();
}

bool AArenaBR_Ball::CanBePickedUpBy(AArenaCharacter* Character) const
{
	if (!Super::CanBePickedUpBy(Character))
	{
		return false;
	}
	const int32 T = Character->GetTeam();
	return T == 0 || T == 1;
}

void AArenaBR_Ball::LaunchFrom(AArenaCharacter* Thrower, const FVector& Start, const FVector& Velocity, AArenaCharacter* InHomingTarget)
{
	LastThrower = Thrower ? Thrower->GetController() : nullptr;
	LastThrowerTeam = Thrower ? Thrower->GetTeam() : -1;
	HomingTarget = InHomingTarget;
	HomingUntil = GetWorld()->GetTimeSeconds() + 3.f;
	Throw(Start, Velocity, Thrower ? Thrower->GetController() : nullptr);
	SetOwner(nullptr);
	ArenaAssets::PlaySoundAt(this, TEXT("S_JumpPad"), Start, 0.7f, 1.4f);
}

void AArenaBR_Ball::OnPickedUp(AArenaCharacter* Character)
{
	SetOwner(Character);
	HomingTarget.Reset();
	AArenaBR_BallLauncher::GiveTo_BR(Character);
	UpdateBeaconColor();
	if (UArenaRules_BR* Rules = ArenaBR::GetRulesBR(this))
	{
		Rules->OnBallPickedUp(this, Character);
	}
}

void AArenaBR_Ball::OnDropped(AController* By)
{
	SetOwner(nullptr);
	const bool bThrown = GetCarryState() == EArenaCarryState::InFlight;
	if (!bThrown)
	{
		HomingTarget.Reset();
	}
	UpdateBeaconColor();
	if (UArenaRules_BR* Rules = ArenaBR::GetRulesBR(this))
	{
		Rules->OnBallDropped(this, By, bThrown);
	}
}

void AArenaBR_Ball::OnReturned(AController* By)
{
	SetOwner(nullptr);
	ClearThrowInfo();
	UpdateBeaconColor();
	if (UArenaRules_BR* Rules = ArenaBR::GetRulesBR(this))
	{
		Rules->OnBallReturned(this);
	}
}

void AArenaBR_Ball::OnLanded()
{
	HomingTarget.Reset();
	ArenaAssets::PlaySoundAt(this, TEXT("S_Hit"), GetObjectLocation(), 0.4f, 0.6f);
}

void AArenaBR_Ball::Tick(float DeltaSeconds)
{
	// (a UT2004 server's ball, IsNetDriven: no homing or scoring here, the server decides)
	const bool bWasFlying = HasAuthority() && !IsNetDriven() && GetCarryState() == EArenaCarryState::InFlight;
	const FVector From = Model ? Model->GetComponentLocation() : GetActorLocation();

	if (bWasFlying)
	{
		// Homing pass: steer towards the locked teammate's chest and cancel gravity while homing
		AArenaCharacter* Target = HomingTarget.Get();
		if (Target && Target->IsAlive() && GetWorld()->GetTimeSeconds() < HomingUntil)
		{
			const FVector Aim = Target->GetActorLocation() + FVector(0.f, 0.f, 20.f);
			const FVector ToTarget = Aim - From;
			const float Speed = FMath::Max(FlightVelocity.Size(), AArenaBR_BallLauncher::PassSpeed * 0.9f);
			const FVector Desired = ToTarget.GetSafeNormal() * Speed;
			FlightVelocity = FMath::Lerp(FlightVelocity, Desired, FMath::Clamp(DeltaSeconds * 5.f, 0.f, 1.f));
			FlightVelocity.Z -= GetWorld()->GetGravityZ() * FlightGravityScale * DeltaSeconds;
		}
		else
		{
			HomingTarget.Reset();
		}
	}

	Super::Tick(DeltaSeconds);

	if (bWasFlying && Model)
	{
		const FVector To = Model->GetComponentLocation();
		if (UArenaRules_BR* Rules = ArenaBR::GetRulesBR(this))
		{
			Rules->OnBallFlightSegment(this, From, To);
		}
	}
	UpdateBeaconColor();

	// Gentle spin while resting / flying
	if (Model && !IsCarried())
	{
		Model->AddLocalRotation(FRotator(0.f, 90.f * DeltaSeconds, 0.f));
	}
}

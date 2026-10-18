#include "ArenaONS_PowerNode.h"
#include "OpenUT2k4.h"
#include "ArenaRules_ONS.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaAssets.h"
#include "ArenaFX.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaArtItems.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaWeapons.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

namespace ArenaONSNode
{
	constexpr float NodeHealth = 1000.f;
	constexpr float CoreHealth = 2500.f;
	constexpr float BuildTime = 10.f;
	const FLinearColor NeutralColor(0.75f, 0.8f, 0.9f);
	const FLinearColor MetalColor(0.11f, 0.12f, 0.14f);
	const FLinearColor TrimColor(0.32f, 0.34f, 0.38f);

	FLinearColor TeamOrNeutral(int32 Team)
	{
		return (Team == 0 || Team == 1) ? ArenaTeam::Color(Team) : NeutralColor;
	}
}
using namespace ArenaONSNode;

// =============================================================================================
// AArenaONS_PowerNode
// =============================================================================================

AArenaONS_PowerNode::AArenaONS_PowerNode()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetCanBeDamaged(true);
	AutoPossessAI = EAutoPossessAI::Disabled;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	AIControllerClass = nullptr;
	bUseControllerRotationYaw = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Solid = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Solid"));
	Solid->SetupAttachment(Root);
	Solid->InitCapsuleSize(85.f, 180.f);
	Solid->SetRelativeLocation(FVector(0.f, 0.f, 230.f));
	Solid->SetCollisionObjectType(ECC_WorldDynamic);
	Solid->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Solid->SetCollisionResponseToAllChannels(ECR_Block);
	Solid->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Solid->SetCanEverAffectNavigation(false);
	Solid->SetGenerateOverlapEvents(false);

	CrystalRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CrystalRoot"));
	CrystalRoot->SetupAttachment(Root);
	CrystalRoot->SetRelativeLocation(FVector(0.f, 0.f, 270.f));

	HealthBarRoot = CreateDefaultSubobject<USceneComponent>(TEXT("HealthBarRoot"));
	HealthBarRoot->SetupAttachment(Root);
	HealthBarRoot->SetRelativeLocation(FVector(0.f, 0.f, 480.f));
	HealthBarRoot->SetUsingAbsoluteRotation(true);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(CrystalRoot);
	Light->SetMobility(EComponentMobility::Movable);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 9000.f;
	Light->AttenuationRadius = 900.f;
	Light->CastShadows = false;
}

void AArenaONS_PowerNode::InitNode(UArenaRules_ONS* InRules, int32 InIndex, bool bInCore, int32 InCoreTeam, const FString& InName)
{
	Rules = InRules;
	NodeIndex = InIndex;
	bIsCore = bInCore;
	NodeName = InName;
	MaxHealth = bIsCore ? CoreHealth : NodeHealth;
	if (bIsCore)
	{
		Solid->SetCapsuleSize(190.f, 320.f);
		Solid->SetRelativeLocation(FVector(0.f, 0.f, 330.f));
		CrystalRoot->SetRelativeLocation(FVector(0.f, 0.f, 380.f));
		HealthBarRoot->SetRelativeLocation(FVector(0.f, 0.f, 800.f));
		Light->AttenuationRadius = 1600.f;
		Light->Intensity = 20000.f;
	}
	ResetNode(InCoreTeam);
}

void AArenaONS_PowerNode::BeginPlay()
{
	Super::BeginPlay();
	BuildModel();
	UpdateCollision();
	UpdateLook();
}

FVector AArenaONS_PowerNode::GetAimPoint() const
{
	return GetActorLocation() + FVector(0.f, 0.f, bIsCore ? 330.f : 250.f);
}

void AArenaONS_PowerNode::AddLink(AArenaONS_PowerNode* Other)
{
	if (Other && Other != this && !Links.Contains(Other))
	{
		Links.Add(Other);
	}
}

void AArenaONS_PowerNode::SetNetworkFlags(bool bAttackableByRed, bool bAttackableByBlue, bool bBuildableByRed, bool bBuildableByBlue)
{
	const uint8 NewAttack = uint8((bAttackableByRed ? 1 : 0) | (bAttackableByBlue ? 2 : 0));
	const uint8 NewBuild = uint8((bBuildableByRed ? 1 : 0) | (bBuildableByBlue ? 2 : 0));
	const bool bChanged = NewAttack != AttackableMask;
	AttackableMask = NewAttack;
	BuildableMask = NewBuild;
	if (bChanged)
	{
		UpdateLook();
	}
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

void AArenaONS_PowerNode::ResetNode(int32 InCoreTeam)
{
	Builder.Reset();
	LastAttacker.Reset();
	BuildRateBonus = 0.f;
	if (bIsCore)
	{
		Health = MaxHealth;
		BuildProgress = 1.f;
		SetState(EArenaONS_NodeState::Active, InCoreTeam);
	}
	else
	{
		Health = 0.f;
		BuildProgress = 0.f;
		SetState(EArenaONS_NodeState::Neutral, ArenaTeam::None);
	}
}

void AArenaONS_PowerNode::SetState(EArenaONS_NodeState NewState, int32 NewTeam)
{
	State = NewState;
	Team = NewState == EArenaONS_NodeState::Neutral ? ArenaTeam::None : NewTeam;
	UpdateCollision();
	UpdateLook();
}

void AArenaONS_PowerNode::SetNetState(EArenaONS_NodeState NewState, int32 NewTeam, float NewHealth, float NewMaxHealth, bool bDamaged)
{
	bNetDriven = true;
	UWorld* World = GetWorld();
	const EArenaONS_NodeState OldState = State;
	const int32 OldTeam = Team;
	if (NewMaxHealth > 0.f)
	{
		MaxHealth = NewMaxHealth;
	}
	Health = FMath::Clamp(NewHealth, 0.f, MaxHealth);
	BuildProgress = NewState == EArenaONS_NodeState::Active ? 1.f
		: (NewState == EArenaONS_NodeState::Constructing ? FMath::Clamp(Health / FMath::Max(1.f, MaxHealth), 0.f, 1.f) : 0.f);
	if (bDamaged && World)
	{
		LastDamageTime = World->GetTimeSeconds();
	}
	const int32 NewTeamValue = NewState == EArenaONS_NodeState::Neutral ? ArenaTeam::None : NewTeam;
	if (OldState == NewState && OldTeam == NewTeamValue)
	{
		return;
	}
	SetState(NewState, NewTeam);
	if (!World || !HasActorBegunPlay())
	{
		return;
	}
	// the effects the local rules play for the same events (Tick: built, TakeDamage: destroyed)
	if (NewState == EArenaONS_NodeState::Active && OldState == EArenaONS_NodeState::Constructing)
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_Flag_Captured"), GetActorLocation(), 1.f, 1.1f);
		ArenaFX::SpawnExplosion(World, GetAimPoint(), TeamOrNeutral(Team), 350.f, TEXT("Explosion.Rocket"));
	}
	else if (OldState == EArenaONS_NodeState::Active && NewState == EArenaONS_NodeState::Neutral)
	{
		ArenaFX::SpawnExplosion(World, GetAimPoint(), TeamOrNeutral(OldTeam), bIsCore ? 900.f : 500.f, TEXT("Explosion.Rocket"));
		ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), GetAimPoint(), 1.5f, bIsCore ? 0.7f : 0.9f);
	}
}

void AArenaONS_PowerNode::UpdateCollision()
{
	if (!Solid)
	{
		return;
	}
	const bool bSolid = bIsCore || State != EArenaONS_NodeState::Neutral;
	Solid->SetCollisionEnabled(bSolid ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}

bool AArenaONS_PowerNode::StartConstruction(int32 InTeam, AController* By)
{
	if (!HasAuthority() || bIsCore || State != EArenaONS_NodeState::Neutral || !IsBuildableBy(InTeam))
	{
		return false;
	}
	Builder = By;
	BuildProgress = 0.f;
	Health = MaxHealth * 0.1f;
	SetState(EArenaONS_NodeState::Constructing, InTeam);
	ArenaAssets::PlaySoundAt(this, TEXT("S_Pickup_Power"), GetActorLocation(), 1.f, 0.8f);
	if (UArenaRules_ONS* R = Rules.Get())
	{
		R->NotifyConstructionStarted(this, By);
	}
	return true;
}

void AArenaONS_PowerNode::ForceOwner(int32 InTeam)
{
	if (!HasAuthority() || bIsCore || (InTeam != 0 && InTeam != 1))
	{
		return;
	}
	BuildProgress = 1.f;
	Health = MaxHealth;
	SetState(EArenaONS_NodeState::Active, InTeam);
}

void AArenaONS_PowerNode::ReceiveLink(float Amount, AController* By)
{
	if (!HasAuthority() || Amount <= 0.f)
	{
		return;
	}
	const UWorld* World = GetWorld();
	if (State == EArenaONS_NodeState::Constructing)
	{
		// Each beam tick keeps a construction speed bonus alive for a moment.
		BuildRateBonus = FMath::Clamp(Amount * 0.2f, 0.5f, 2.f);
		BuildRateBonusUntil = World ? World->GetTimeSeconds() + 0.25f : 0.f;
	}
	else if (State == EArenaONS_NodeState::Active && Health < MaxHealth)
	{
		Health = FMath::Min(MaxHealth, Health + Amount * 1.2f);
	}
}

float AArenaONS_PowerNode::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (!HasAuthority() || bNetDriven || DamageAmount <= 0.f || State == EArenaONS_NodeState::Neutral)
	{
		return 0.f;
	}
	UWorld* World = GetWorld();
	const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
	if (GS && !GS->IsPlaying())
	{
		return 0.f;
	}
	const int32 AttackerTeam = ArenaONS::TeamOfController(EventInstigator);

	// Friendly Arc Linker beam: build / repair instead of damage.
	if (AttackerTeam == Team && Cast<AWeap_ArcLinker>(DamageCauser))
	{
		ReceiveLink(DamageAmount, EventInstigator);
		return 0.f;
	}
	if (AttackerTeam == Team || !IsAttackableBy(AttackerTeam))
	{
		// Shielded: a small spark so the shooter sees it does nothing.
		if (World && FMath::FRand() < 0.15f)
		{
			ArenaFX::SpawnImpact(World, GetAimPoint(), FVector::UpVector, TeamOrNeutral(Team), 30.f, TEXT("Impact.ShockBall"));
		}
		return 0.f;
	}

	Health -= DamageAmount;
	LastDamageTime = World ? World->GetTimeSeconds() : 0.f;
	LastAttacker = EventInstigator;

	if (bIsCore)
	{
		if (UArenaRules_ONS* R = Rules.Get())
		{
			R->NotifyCoreDamaged(this, DamageAmount, EventInstigator);
		}
	}

	if (Health <= 0.f)
	{
		Health = 0.f;
		ArenaFX::SpawnExplosion(World, GetAimPoint(), TeamOrNeutral(Team), bIsCore ? 900.f : 500.f, TEXT("Explosion.Rocket"));
		ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), GetAimPoint(), 1.5f, bIsCore ? 0.7f : 0.9f);
		if (bIsCore)
		{
			if (UArenaRules_ONS* R = Rules.Get())
			{
				R->NotifyCoreDestroyed(this, EventInstigator);
			}
		}
		else
		{
			const int32 OldTeam = Team;
			BuildProgress = 0.f;
			SetState(EArenaONS_NodeState::Neutral, ArenaTeam::None);
			if (UArenaRules_ONS* R = Rules.Get())
			{
				R->NotifyNodeDestroyed(this, OldTeam, EventInstigator);
			}
		}
	}
	return DamageAmount;
}

void AArenaONS_PowerNode::TickTouch()
{
	UWorld* World = GetWorld();
	if (!World || bIsCore || State != EArenaONS_NodeState::Neutral)
	{
		return;
	}
	const AArenaGameState* GS = World->GetGameState<AArenaGameState>();
	if (GS && !GS->IsPlaying())
	{
		return;
	}
	const FVector Loc = GetActorLocation();
	const float R = GetPadRadius() + 40.f;
	for (TActorIterator<AArenaCharacter> It(World); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (!C || !C->IsAlive() || !C->GetActorEnableCollision() || !C->GetController())
		{
			continue;
		}
		const FVector P = C->GetActorLocation();
		if (FVector::DistSquared2D(P, Loc) > R * R || P.Z < Loc.Z - 60.f || P.Z > Loc.Z + 320.f)
		{
			continue;
		}
		const int32 T = C->GetTeam();
		if (IsBuildableBy(T) && StartConstruction(T, C->GetController()))
		{
			return;
		}
	}
}

void AArenaONS_PowerNode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	VisualTime += DeltaSeconds;

	if (HasAuthority() && !bNetDriven)
	{
		if (Now >= NextTouchCheck)
		{
			NextTouchCheck = Now + 0.2f;
			TickTouch();
		}
		const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
		if (State == EArenaONS_NodeState::Constructing && (!GS || GS->IsPlaying()))
		{
			const float Rate = 1.f + (Now < BuildRateBonusUntil ? BuildRateBonus : 0.f);
			const float Delta = FMath::Min(DeltaSeconds * Rate / BuildTime, 1.f - BuildProgress);
			BuildProgress += Delta;
			Health = FMath::Min(MaxHealth, Health + MaxHealth * 0.9f * Delta);
			if (BuildProgress >= 1.f - KINDA_SMALL_NUMBER)
			{
				BuildProgress = 1.f;
				SetState(EArenaONS_NodeState::Active, Team);
				ArenaAssets::PlaySoundAt(this, TEXT("S_Flag_Captured"), GetActorLocation(), 1.f, 1.1f);
				ArenaFX::SpawnExplosion(World, GetAimPoint(), TeamOrNeutral(Team), 350.f, TEXT("Explosion.Rocket"));
				if (UArenaRules_ONS* R = Rules.Get())
				{
					R->NotifyNodeBuilt(this, Builder.Get());
				}
			}
		}
	}

	// ---- visuals (everywhere) ----
	if (!bModelBuilt)
	{
		return;
	}
	const bool bActive = State == EArenaONS_NodeState::Active;
	const float Progress = State == EArenaONS_NodeState::Neutral ? 0.f : (bActive ? 1.f : BuildProgress);
	const float Spin = bActive ? 40.f : (State == EArenaONS_NodeState::Constructing ? 120.f : 12.f);
	if (CrystalPart)
	{
		const float Bob = FMath::Sin(VisualTime * 1.7f) * (bIsCore ? 12.f : 8.f);
		CrystalRoot->SetRelativeLocation(FVector(0.f, 0.f, (bIsCore ? 380.f : 270.f) + Bob));
		CrystalRoot->SetRelativeRotation(FRotator(0.f, FMath::Fmod(VisualTime * Spin, 360.f), 0.f));
		const float S = bIsCore ? 1.f : FMath::Lerp(0.35f, 1.f, Progress);
		CrystalPart->SetRelativeScale3D((bIsCore ? FVector(1.4f) : FVector(1.1f)) * S);
	}
	if (ArtFrame)
	{
		// The energy frame rises out of the base while the node is built.
		const float Grow = FMath::Lerp(0.12f, 1.f, Progress);
		ArtFrame->SetRelativeScale3D(FVector(ArtFrameScale.X, ArtFrameScale.Y, ArtFrameScale.Z * Grow));
		ArtFrame->SetRelativeLocation(ArtFrameLocation - FVector(0.f, 0.f, ArtFrameBottom * Grow));
	}
	for (UArenaSpriteComponent* Flare : ArtFlares)
	{
		if (Flare)
		{
			const float Pulse = State == EArenaONS_NodeState::Constructing ? 0.6f + 0.4f * FMath::Abs(FMath::Sin(VisualTime * 6.f)) : 1.f;
			Flare->SetIntensityScale((State == EArenaONS_NodeState::Neutral ? 0.35f : 1.f) * Pulse * (bIsCore ? 1.f : FMath::Lerp(0.3f, 1.f, Progress)));
		}
	}
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		if (Rings[i])
		{
			const float Dir = (i % 2 == 0) ? 1.f : -1.f;
			Rings[i]->SetRelativeRotation(FRotator(FMath::Fmod(VisualTime * 35.f * Dir, 360.f), FMath::Fmod(VisualTime * 50.f, 360.f), 25.f * Dir));
			Rings[i]->SetVisibility(State != EArenaONS_NodeState::Neutral, true);
		}
	}
	if (!bIsCore)
	{
		for (USceneComponent* Pylon : Pylons)
		{
			if (Pylon)
			{
				Pylon->SetRelativeScale3D(FVector(1.f, 1.f, FMath::Max(0.02f, 2.6f * Progress)));
			}
		}
	}

	// Health bar: shown when not neutral, faces the local camera.
	const bool bShowBar = State != EArenaONS_NodeState::Neutral;
	if (HealthBack && HealthFill)
	{
		HealthBack->SetVisibility(bShowBar);
		HealthFill->SetVisibility(bShowBar);
		if (bShowBar)
		{
			const float BarW = bIsCore ? 320.f : 220.f;
			const float Frac = GetHealthFraction();
			HealthFill->SetRelativeScale3D(FVector(0.12f, FMath::Max(0.01f, BarW * Frac / 100.f), 0.16f));
			HealthFill->SetRelativeLocation(FVector(4.f, -BarW * 0.5f + BarW * Frac * 0.5f, 0.f));
			if (HealthFillMat)
			{
				const bool bFlash = Now - LastDamageTime < 0.4f && FMath::Fmod(Now * 10.f, 2.f) < 1.f;
				HealthFillMat->SetVectorParameterValue(TEXT("Color"), bFlash ? FLinearColor::White : TeamOrNeutral(Team));
			}
			if (World)
			{
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					if (PC->PlayerCameraManager)
					{
						const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
						const FVector To = CamLoc - HealthBarRoot->GetComponentLocation();
						HealthBarRoot->SetWorldRotation(FRotator(0.f, To.Rotation().Yaw, 0.f));
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaONS_PowerNode::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Loc, const FRotator& Rot, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh || !Parent)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
	Part->SetRelativeTransform(FTransform(Rot, Loc, SizeCm / 100.f));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCollisionResponseToAllChannels(ECR_Ignore);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->SetCastShadow(bShadow);
	if (Material)
	{
		Part->SetMaterial(0, Material);
	}
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void AArenaONS_PowerNode::BuildModel()
{
	if (bModelBuilt)
	{
		return;
	}
	bModelBuilt = true;
	UStaticMesh* Cyl = ArenaAssets::Cylinder();
	UStaticMesh* Cube = ArenaAssets::Cube();
	UStaticMesh* Sphere = ArenaAssets::Sphere();
	UStaticMesh* Cone = ArenaAssets::Cone();
	const FLinearColor Col = TeamOrNeutral(Team);

	auto Glow = [this, &Col](float Intensity, float Opacity = 1.f) { return ArenaAssets::MakeGlow(this, Col, Intensity, Opacity); };
	auto Metal = [this]() { return ArenaAssets::MakeSurface(this, MetalColor, 0.35f, 0.85f); };
	auto Trim = [this]() { return ArenaAssets::MakeSurface(this, TrimColor, 0.45f, 0.6f); };

	bArtModel = BuildArtModel();
	if (bArtModel)
	{
		// imported model built; only the health bar below is shared
	}
	else if (!bIsCore)
	{
		// Hexagonal-looking pad: two stacked discs with a glowing seam.
		AddPart(Root, Cyl, FVector(0.f, 0.f, 6.f), FRotator::ZeroRotator, FVector(460.f, 460.f, 12.f), Metal());
		AddPart(Root, Cyl, FVector(0.f, 0.f, 14.f), FRotator::ZeroRotator, FVector(340.f, 340.f, 6.f), Trim());
		TeamGlow.Add(AddPart(Root, Cyl, FVector(0.f, 0.f, 4.f), FRotator::ZeroRotator, FVector(490.f, 490.f, 6.f), Glow(5.f), false));
		TeamGlow.Add(AddPart(Root, Cyl, FVector(0.f, 0.f, 18.f), FRotator::ZeroRotator, FVector(120.f, 120.f, 3.f), Glow(8.f), false));
		// Three emitter studs on the rim
		for (int32 i = 0; i < 3; ++i)
		{
			const float A = FMath::DegreesToRadians(30.f + 120.f * i);
			const FVector Dir(FMath::Cos(A), FMath::Sin(A), 0.f);
			AddPart(Root, Cube, Dir * 200.f + FVector(0.f, 0.f, 20.f), FRotator(0.f, 30.f + 120.f * i, 0.f), FVector(50.f, 36.f, 26.f), Trim());
			TeamGlow.Add(AddPart(Root, Sphere, Dir * 200.f + FVector(0.f, 0.f, 38.f), FRotator::ZeroRotator, FVector(18.f), Glow(10.f), false));
			// Pylons lean in towards the crystal; their height grows with the build progress.
			const FVector Base = Dir * 165.f + FVector(0.f, 0.f, 20.f);
			USceneComponent* Holder = NewObject<USceneComponent>(this);
			Holder->SetupAttachment(Root);
			Holder->SetRelativeLocation(Base);
			Holder->SetRelativeRotation(FRotator(-18.f, FMath::RadiansToDegrees(A) + 180.f, 0.f));
			Holder->SetRelativeScale3D(FVector(1.f, 1.f, 0.02f));
			Holder->RegisterComponent();
			AddPart(Holder, Cube, FVector(0.f, 0.f, 50.f), FRotator::ZeroRotator, FVector(26.f, 26.f, 100.f), Metal());
			TeamGlow.Add(AddPart(Holder, Cube, FVector(14.f, 0.f, 55.f), FRotator::ZeroRotator, FVector(4.f, 10.f, 80.f), Glow(7.f), false));
			Pylons.Add(Holder);
		}
		CrystalPart = AddPart(CrystalRoot, Cube, FVector::ZeroVector, FRotator(45.f, 0.f, 35.26f), FVector(100.f), Glow(9.f), false);
		TeamGlow.Add(CrystalPart);
		AddPart(CrystalRoot, Sphere, FVector::ZeroVector, FRotator::ZeroRotator, FVector(60.f), ArenaAssets::MakeGlow(this, FLinearColor(1.f, 1.f, 1.f), 6.f, 0.8f), false);
		for (int32 i = 0; i < 2; ++i)
		{
			USceneComponent* Ring = NewObject<USceneComponent>(this);
			Ring->SetupAttachment(CrystalRoot);
			Ring->RegisterComponent();
			Rings.Add(Ring);
			TeamGlow.Add(AddPart(Ring, Cyl, FVector::ZeroVector, FRotator::ZeroRotator, FVector(i == 0 ? 210.f : 170.f, i == 0 ? 210.f : 170.f, 4.f), Glow(6.f, 0.85f), false));
		}
		ShieldPart = AddPart(Root, Sphere, FVector(0.f, 0.f, 220.f), FRotator::ZeroRotator, FVector(520.f), Glow(1.2f, 0.12f), false);
	}
	else
	{
		// Core: a broad stepped base, four tall conduit pillars and a large floating heart.
		AddPart(Root, Cyl, FVector(0.f, 0.f, 12.f), FRotator::ZeroRotator, FVector(760.f, 760.f, 24.f), Metal());
		AddPart(Root, Cyl, FVector(0.f, 0.f, 34.f), FRotator::ZeroRotator, FVector(560.f, 560.f, 20.f), Trim());
		TeamGlow.Add(AddPart(Root, Cyl, FVector(0.f, 0.f, 6.f), FRotator::ZeroRotator, FVector(800.f, 800.f, 8.f), Glow(5.f), false));
		TeamGlow.Add(AddPart(Root, Cyl, FVector(0.f, 0.f, 46.f), FRotator::ZeroRotator, FVector(400.f, 400.f, 4.f), Glow(6.f), false));
		for (int32 i = 0; i < 4; ++i)
		{
			const float A = FMath::DegreesToRadians(45.f + 90.f * i);
			const FVector Dir(FMath::Cos(A), FMath::Sin(A), 0.f);
			AddPart(Root, Cyl, Dir * 300.f + FVector(0.f, 0.f, 290.f), FRotator::ZeroRotator, FVector(60.f, 60.f, 520.f), Metal());
			AddPart(Root, Cube, Dir * 300.f + FVector(0.f, 0.f, 40.f), FRotator(0.f, 45.f + 90.f * i, 0.f), FVector(110.f, 110.f, 60.f), Trim());
			TeamGlow.Add(AddPart(Root, Cone, Dir * 300.f + FVector(0.f, 0.f, 590.f), FRotator::ZeroRotator, FVector(70.f, 70.f, 80.f), Glow(9.f), false));
			// Light strips up each pillar
			TeamGlow.Add(AddPart(Root, Cube, Dir * 268.f + FVector(0.f, 0.f, 290.f), FRotator(0.f, 45.f + 90.f * i, 0.f), FVector(6.f, 14.f, 460.f), Glow(7.f), false));
		}
		CrystalPart = AddPart(CrystalRoot, Sphere, FVector::ZeroVector, FRotator::ZeroRotator, FVector(220.f), Glow(7.f), false);
		TeamGlow.Add(CrystalPart);
		AddPart(CrystalRoot, Cube, FVector::ZeroVector, FRotator(45.f, 0.f, 35.26f), FVector(150.f), ArenaAssets::MakeGlow(this, FLinearColor(1.f, 1.f, 1.f), 5.f, 0.7f), false);
		for (int32 i = 0; i < 3; ++i)
		{
			USceneComponent* Ring = NewObject<USceneComponent>(this);
			Ring->SetupAttachment(CrystalRoot);
			Ring->RegisterComponent();
			Rings.Add(Ring);
			const float D = 420.f - i * 60.f;
			TeamGlow.Add(AddPart(Ring, Cyl, FVector::ZeroVector, FRotator::ZeroRotator, FVector(D, D, 5.f), Glow(5.f, 0.8f), false));
		}
		ShieldPart = AddPart(Root, Sphere, FVector(0.f, 0.f, 330.f), FRotator::ZeroRotator, FVector(820.f), Glow(1.f, 0.1f), false);
	}

	// UT2004 drew no bar in the world: with the art the HUD shows node health (UArenaRules_ONS::DrawNodeHealth, HUD
	// meter art over the node). Only the procedural stand-in node keeps this world-space bar (faces the local camera).
	if (bArtModel)
	{
		return;
	}
	const float BarW = bIsCore ? 320.f : 220.f;
	HealthBack = AddPart(HealthBarRoot, Cube, FVector::ZeroVector, FRotator::ZeroRotator, FVector(10.f, BarW + 12.f, 26.f), ArenaAssets::MakeSurface(this, FLinearColor(0.02f, 0.02f, 0.03f), 0.9f), false);
	HealthFillMat = ArenaAssets::MakeGlow(this, Col, 6.f);
	HealthFill = AddPart(HealthBarRoot, Cube, FVector(4.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(12.f, BarW, 16.f), HealthFillMat, false);
}

bool AArenaONS_PowerNode::BuildArtModel()
{
	const ArenaArtItems::FOnslaughtArt& Art = ArenaArtItems::GetOnslaughtArt();
	auto AddFlare = [this](USceneComponent* Parent, const TCHAR* Texture, float Size, float Spin)
	{
		ArenaArtFX::FSpriteDesc D;
		D.Texture = Texture;
		D.Size = FVector2D(Size, Size);
		D.Spin = Spin;
		D.Intensity = 2.f;
		UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
		C->SetupAttachment(Parent);
		if (C->SetupSprite(D))
		{
			C->RegisterComponent();
			ArtFlares.Add(C);
		}
		else
		{
			C->DestroyComponent();
		}
	};

	if (!bIsCore)
	{
		UStaticMeshComponent* Base = ArenaArtItems::AddItemMesh(this, Root, Art.NodeBase, 1.f, true);
		if (!Base)
		{
			return false;
		}
		Parts.Add(Base);
		const float BaseTop = Base->Bounds.GetBox().Max.Z - GetActorLocation().Z;
		if (USkeletalMesh* Frame = ArenaArt::Load<USkeletalMesh>(Art.NodeFrame))
		{
			ArtFrame = NewObject<USkeletalMeshComponent>(this, TEXT("ArtFrame"));
			ArtFrame->SetSkeletalMeshAsset(Frame);
			ArtFrame->SetupAttachment(Root);
			ArtFrame->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ArtFrame->SetGenerateOverlapEvents(false);
			ArtFrame->SetCanEverAffectNavigation(false);
			ArtFrame->bComponentUseFixedSkelBounds = true;
			const FBoxSphereBounds B = Frame->GetImportedBounds();
			const float S = Art.NodeFrameHeight / FMath::Max(1.f, float(B.BoxExtent.Z) * 2.f);
			ArtFrameScale = FVector(S);
			ArtFrame->SetRelativeScale3D(ArtFrameScale);
			// Scaling in Z grows the frame from its bottom: put the bottom on the base.
			ArtFrameBottom = (B.Origin.Z - B.BoxExtent.Z) * S;
			ArtFrameLocation = FVector(-B.Origin.X * S, -B.Origin.Y * S, FMath::Max(0.f, BaseTop));
			ArtFrame->SetRelativeLocation(ArtFrameLocation - FVector(0.f, 0.f, ArtFrameBottom));
			ArtFrame->RegisterComponent();
		}
		AddFlare(CrystalRoot, Art.Flare, 300.f, 25.f);
		if (UStaticMeshComponent* Shield = ArenaArtItems::AddItemMesh(this, Root, Art.NodeShield))
		{
			ShieldPart = Shield;
		}
		else
		{
			ShieldPart = AddPart(Root, ArenaAssets::Sphere(), FVector(0.f, 0.f, 220.f), FRotator::ZeroRotator, FVector(520.f),
				ArenaAssets::MakeGlow(this, TeamOrNeutral(Team), 1.2f, 0.12f), false);
		}
		return true;
	}

	UStaticMeshComponent* Core = ArenaArtItems::AddItemMesh(this, Root, Art.Core, 1.f, true);
	if (!Core)
	{
		return false;
	}
	Parts.Add(Core);
	ArtEnergy = ArenaArtItems::AddItemMesh(this, Root, Art.CoreEnergy);
	AddFlare(CrystalRoot, Art.Flare, 650.f, 15.f);
	if (UStaticMeshComponent* Shield = ArenaArtItems::AddItemMesh(this, Root, Art.CoreShield))
	{
		ShieldPart = Shield;
	}
	else
	{
		ShieldPart = AddPart(Root, ArenaAssets::Sphere(), FVector(0.f, 0.f, 330.f), FRotator::ZeroRotator, FVector(820.f),
			ArenaAssets::MakeGlow(this, TeamOrNeutral(Team), 1.f, 0.1f), false);
	}
	return true;
}

void AArenaONS_PowerNode::UpdateArtLook()
{
	const ArenaArtItems::FOnslaughtArt& Art = ArenaArtItems::GetOnslaughtArt();
	const bool bNeutral = State == EArenaONS_NodeState::Neutral;
	const int32 T = (Team == 0 || Team == 1) ? Team : -1;
	if (ArtFrame)
	{
		// Sections 1..3 are the energy (flare, beams, coils): team materials, hidden while neutral.
		const int32 NumLODs = ArtFrame->GetNumLODs();
		for (int32 Slot = 1; Slot <= 3; ++Slot)
		{
			for (int32 LOD = 0; LOD < NumLODs; ++LOD)
			{
				ArtFrame->ShowMaterialSection(Slot, Slot, !bNeutral && T >= 0, LOD);
			}
			if (T >= 0)
			{
				if (UMaterialInterface* Mat = ArenaArtItems::LoadMaterial(Art.NodeFrameMaterials[T][Slot - 1]))
				{
					ArtFrame->SetMaterial(Slot, Mat);
				}
			}
		}
	}
	if (T >= 0)
	{
		UMaterialInterface* Energy = ArenaArtItems::LoadMaterial(Art.CoreShieldMaterial[T]);
		// Only the art shield mesh takes the team energy material; the procedural sphere keeps its own glow. (Not
		// "is the material dynamic": runtime content materials are dynamic instances too.)
		if (Energy && ShieldPart && ShieldPart->GetStaticMesh() != ArenaAssets::Sphere())
		{
			ShieldPart->SetMaterial(0, Energy);
		}
		if (Energy && ArtEnergy)
		{
			ArtEnergy->SetMaterial(0, Energy);
		}
	}
	if (ArtEnergy)
	{
		ArtEnergy->SetVisibility(bIsCore && T >= 0 && State == EArenaONS_NodeState::Active);
	}
	for (UArenaSpriteComponent* Flare : ArtFlares)
	{
		if (Flare)
		{
			Flare->SetTint(TeamOrNeutral(Team));
		}
	}
}

void AArenaONS_PowerNode::OnRep_Look()
{
	UpdateCollision();
	UpdateLook();
}

void AArenaONS_PowerNode::UpdateLook()
{
	if (!bModelBuilt)
	{
		return;
	}
	const bool bNeutral = State == EArenaONS_NodeState::Neutral;
	const FLinearColor Col = TeamOrNeutral(Team);
	const float Dim = bNeutral ? 0.35f : (State == EArenaONS_NodeState::Constructing ? 0.7f : 1.f);
	for (UStaticMeshComponent* Part : TeamGlow)
	{
		if (UMaterialInstanceDynamic* MID = Part ? Cast<UMaterialInstanceDynamic>(Part->GetMaterial(0)) : nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Col * Dim);
		}
	}
	if (Light)
	{
		Light->SetLightColor(Col);
		Light->SetIntensity((bIsCore ? 20000.f : 9000.f) * (bNeutral ? 0.25f : 1.f));
	}
	if (bArtModel)
	{
		UpdateArtLook();
	}
	if (ShieldPart)
	{
		// Shielded when the opposing team cannot damage it right now.
		const int32 Enemy = (Team == 0 || Team == 1) ? 1 - Team : -1;
		const bool bShielded = !bNeutral && Enemy >= 0 && !IsAttackableBy(Enemy);
		ShieldPart->SetVisibility(bShielded);
		UMaterialInstanceDynamic* MID = ShieldPart->GetStaticMesh() == ArenaAssets::Sphere() ? Cast<UMaterialInstanceDynamic>(ShieldPart->GetMaterial(0)) : nullptr;
		if (MID) // the procedural shield's own glow (art shield materials may be shared runtime instances)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Col);
		}
	}
}

void AArenaONS_PowerNode::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaONS_PowerNode, Team);
	DOREPLIFETIME(AArenaONS_PowerNode, State);
	DOREPLIFETIME(AArenaONS_PowerNode, Health);
	DOREPLIFETIME(AArenaONS_PowerNode, MaxHealth);
	DOREPLIFETIME(AArenaONS_PowerNode, BuildProgress);
	DOREPLIFETIME(AArenaONS_PowerNode, AttackableMask);
	DOREPLIFETIME(AArenaONS_PowerNode, BuildableMask);
	DOREPLIFETIME(AArenaONS_PowerNode, bIsCore);
	DOREPLIFETIME(AArenaONS_PowerNode, NodeIndex);
	DOREPLIFETIME(AArenaONS_PowerNode, NodeName);
	DOREPLIFETIME(AArenaONS_PowerNode, Links);
	DOREPLIFETIME(AArenaONS_PowerNode, LastDamageTime);
}

// =============================================================================================
// AArenaONS_PowerLink
// =============================================================================================

AArenaONS_PowerLink::AArenaONS_PowerLink()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
	SetActorEnableCollision(false);
}

void AArenaONS_PowerLink::InitLink(AArenaONS_PowerNode* InA, AArenaONS_PowerNode* InB)
{
	A = InA;
	B = InB;
}

void AArenaONS_PowerLink::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	if (!A || !B)
	{
		return;
	}
	auto MakeBeam = [this](float Width, float Intensity, float Opacity, TObjectPtr<UMaterialInstanceDynamic>& OutMat) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(ArenaAssets::Cylinder());
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetGenerateOverlapEvents(false);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(false);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->SetupAttachment(RootComponent);
		OutMat = ArenaAssets::MakeGlow(this, NeutralColor, Intensity, Opacity);
		if (OutMat)
		{
			C->SetMaterial(0, OutMat);
		}
		C->RegisterComponent();
		return C;
	};
	const FVector LinkA = A->GetActorLocation() + FVector(0.f, 0.f, A->IsCore() ? 380.f : 270.f);
	const FVector LinkB = B->GetActorLocation() + FVector(0.f, 0.f, B->IsCore() ? 380.f : 270.f);
	if (!bArtTried)
	{
		bArtTried = true;
		const ArenaArtItems::FOnslaughtArt& Art = ArenaArtItems::GetOnslaughtArt();
		for (int32 i = 0; i < 2; ++i)
		{
			ArenaArtFX::FSpriteDesc D;
			D.Texture = i == 0 ? Art.LinkBeam : Art.LinkBolt;
			D.Facing = ArenaArtFX::EFacing::Beam;
			D.bRandomRoll = false;
			D.Size = FVector2D(0.f, i == 0 ? 60.f : 34.f);
			D.TileLength = i == 0 ? 0.f : 700.f;
			D.Pan = i == 0 ? 0.f : -0.6f;
			D.Intensity = i == 0 ? 1.5f : 2.f;
			UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
			C->SetUsingAbsoluteLocation(true);
			C->SetUsingAbsoluteRotation(true);
			C->SetUsingAbsoluteScale(true);
			C->SetupAttachment(RootComponent);
			if (C->SetupSprite(D))
			{
				C->RegisterComponent();
				C->SetBeam(LinkA, LinkB);
				ArtBeams.Add(C);
			}
			else
			{
				C->DestroyComponent();
			}
		}
	}
	if (ArtBeams.Num() > 0)
	{
		const int32 TA = A->IsActive() ? A->GetTeam() : -1;
		const int32 TB = B->IsActive() ? B->GetTeam() : -1;
		FLinearColor Col = NeutralColor * 0.35f;
		float Strength = 0.5f;
		if (TA >= 0 && TA == TB)
		{
			Col = ArenaTeam::Color(TA);
			Strength = 1.f;
		}
		else if (TA >= 0 || TB >= 0)
		{
			Col = FMath::Lerp(NeutralColor * 0.35f, ArenaTeam::Color(TA >= 0 ? TA : TB), 0.5f);
			Strength = 0.75f;
		}
		const float Pulse = 0.8f + 0.2f * FMath::Sin(Time * 4.f);
		for (UArenaSpriteComponent* C : ArtBeams)
		{
			if (C)
			{
				C->SetTint(Col);
				C->SetIntensityScale(Strength * Pulse);
			}
		}
		return;
	}
	if (!Beam)
	{
		Beam = MakeBeam(18.f, 2.5f, 0.35f, BeamMat);
		Core = MakeBeam(6.f, 7.f, 0.9f, CoreMat);
		const FVector SA = A->GetActorLocation() + FVector(0.f, 0.f, A->IsCore() ? 380.f : 270.f);
		const FVector SB = B->GetActorLocation() + FVector(0.f, 0.f, B->IsCore() ? 380.f : 270.f);
		ArenaFX::OrientBeamMesh(Beam, SA, SB, 18.f);
		ArenaFX::OrientBeamMesh(Core, SA, SB, 5.f);
	}

	// Colour: team colour when both ends are active for one team, half-tinted when one end is, else neutral.
	const int32 TA = A->IsActive() ? A->GetTeam() : -1;
	const int32 TB = B->IsActive() ? B->GetTeam() : -1;
	int32 Key = -1;
	FLinearColor Col = NeutralColor * 0.4f;
	if (TA >= 0 && TA == TB)
	{
		Key = TA;
		Col = ArenaTeam::Color(TA);
	}
	else if (TA >= 0 || TB >= 0)
	{
		const int32 T = TA >= 0 ? TA : TB;
		Key = 10 + T;
		Col = FMath::Lerp(NeutralColor * 0.4f, ArenaTeam::Color(T), 0.45f);
	}
	const float Pulse = 0.75f + 0.25f * FMath::Sin(Time * 4.f);
	if (BeamMat)
	{
		BeamMat->SetVectorParameterValue(TEXT("Color"), Col);
		BeamMat->SetScalarParameterValue(TEXT("Intensity"), (Key >= 0 && Key < 10 ? 3.f : 1.2f) * Pulse);
	}
	if (CoreMat && Key != LastColorKey)
	{
		CoreMat->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(Col, FLinearColor::White, 0.3f));
		CoreMat->SetScalarParameterValue(TEXT("Intensity"), Key >= 0 && Key < 10 ? 8.f : 2.f);
	}
	LastColorKey = Key;
}

void AArenaONS_PowerLink::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaONS_PowerLink, A);
	DOREPLIFETIME(AArenaONS_PowerLink, B);
}

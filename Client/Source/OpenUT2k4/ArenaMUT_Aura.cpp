#include "ArenaMUT_Aura.h"
#include "ArenaAssets.h"
#include "ArenaTypes.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace ArenaMUT
{
	/** The aura root sits at the capsule centre; the feet are this far below it. */
	constexpr float AuraFeetZ = -88.f;
}

AArenaMUT_Aura::AArenaMUT_Aura()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(true);
	bAlwaysRelevant = true;
	SetCanBeDamaged(false);

	AuraRoot = CreateDefaultSubobject<USceneComponent>(TEXT("AuraRoot"));
	AuraRoot->SetMobility(EComponentMobility::Movable);
	RootComponent = AuraRoot;

	OrbitRoot = CreateDefaultSubobject<USceneComponent>(TEXT("OrbitRoot"));
	OrbitRoot->SetupAttachment(AuraRoot);
	OrbitRoot->SetMobility(EComponentMobility::Movable);

	AuraLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("AuraLight"));
	AuraLight->SetupAttachment(AuraRoot);
	AuraLight->SetMobility(EComponentMobility::Movable);
	AuraLight->SetRelativeLocation(FVector(0.f, 0.f, 20.f));
	AuraLight->IntensityUnits = ELightUnits::Unitless;
	AuraLight->Intensity = 9000.f;
	AuraLight->AttenuationRadius = 520.f;
	AuraLight->CastShadows = false;
	AuraLight->SetLightColor(AuraColor());
}

FLinearColor AArenaMUT_Aura::AuraColor()
{
	return FLinearColor(0.85f, 0.15f, 1.f);
}

UStaticMeshComponent* AArenaMUT_Aura::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm,
	const FRotator& Rotation, UMaterialInterface* Material)
{
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
	Part->SetRelativeTransform(FTransform(Rotation, Location, SizeCm / 100.f));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCollisionResponseToAllChannels(ECR_Ignore);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->SetCastShadow(false);
	Part->SetOwnerNoSee(true);
	Part->SetMaterial(0, Material);
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void AArenaMUT_Aura::BeginPlay()
{
	Super::BeginPlay();

	const FLinearColor C = AuraColor();
	UMaterialInterface* ShellMat = ArenaAssets::MakeGlow(this, C, 2.5f, 0.16f);
	UMaterialInterface* RingMat = ArenaAssets::MakeGlow(this, C, 6.f, 0.6f);
	UMaterialInterface* MoteMat = ArenaAssets::MakeGlow(this, FLinearColor(1.f, 0.6f, 1.f), 12.f, 1.f);
	UMaterialInterface* CrownMat = ArenaAssets::MakeGlow(this, C, 8.f, 0.85f);

	// Translucent shell around the body
	Shell = AddPart(AuraRoot, ArenaAssets::Sphere(), FVector(0.f, 0.f, 4.f), FVector(150.f, 150.f, 225.f), FRotator::ZeroRotator, ShellMat);
	// Floor ring (flat cylinder) and a smaller inner disc
	Ring = AddPart(AuraRoot, ArenaAssets::Cylinder(), FVector(0.f, 0.f, ArenaMUT::AuraFeetZ + 3.f), FVector(230.f, 230.f, 3.f), FRotator::ZeroRotator, RingMat);
	// Inverted crown spike above the head
	AddPart(AuraRoot, ArenaAssets::Cone(), FVector(0.f, 0.f, 150.f), FVector(34.f, 34.f, 40.f), FRotator(180.f, 0.f, 0.f), CrownMat);
	// Orbiting motes
	for (int32 i = 0; i < 4; ++i)
	{
		const float Angle = i * PI * 0.5f;
		const FVector Loc(FMath::Cos(Angle) * 85.f, FMath::Sin(Angle) * 85.f, (i % 2 == 0) ? 40.f : -20.f);
		AddPart(OrbitRoot, ArenaAssets::Sphere(), Loc, FVector(16.f), FRotator::ZeroRotator, MoteMat);
	}
}

void AArenaMUT_Aura::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;

	if (OrbitRoot)
	{
		OrbitRoot->SetRelativeRotation(FRotator(0.f, FMath::Fmod(Age * 140.f, 360.f), 0.f));
	}
	const float Pulse = 0.5f + 0.5f * FMath::Sin(Age * 4.f);
	if (Shell)
	{
		const float S = 1.f + 0.06f * Pulse;
		Shell->SetRelativeScale3D(FVector(1.5f * S, 1.5f * S, 2.25f * S));
	}
	if (Ring)
	{
		const float S = 2.1f + 0.4f * Pulse;
		Ring->SetRelativeScale3D(FVector(S, S, 0.03f));
	}
	if (AuraLight)
	{
		AuraLight->SetIntensity(6000.f + 6000.f * Pulse);
	}
}

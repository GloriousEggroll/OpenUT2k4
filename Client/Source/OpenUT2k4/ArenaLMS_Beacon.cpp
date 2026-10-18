#include "ArenaLMS_Beacon.h"
#include "ArenaAssets.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace ArenaLMS
{
	constexpr float BeaconColumnHeight = 1800.f;
}

AArenaLMS_Beacon::AArenaLMS_Beacon()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(true);
	bAlwaysRelevant = true;
	SetCanBeDamaged(false);

	BeaconRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BeaconRoot"));
	BeaconRoot->SetMobility(EComponentMobility::Movable);
	RootComponent = BeaconRoot;

	Spinner = CreateDefaultSubobject<USceneComponent>(TEXT("Spinner"));
	Spinner->SetupAttachment(BeaconRoot);
	Spinner->SetMobility(EComponentMobility::Movable);
	Spinner->SetRelativeLocation(FVector(0.f, 0.f, 160.f));

	BeaconLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("BeaconLight"));
	BeaconLight->SetupAttachment(BeaconRoot);
	BeaconLight->SetMobility(EComponentMobility::Movable);
	BeaconLight->SetRelativeLocation(FVector(0.f, 0.f, 180.f));
	BeaconLight->IntensityUnits = ELightUnits::Unitless;
	BeaconLight->Intensity = 12000.f;
	BeaconLight->AttenuationRadius = 700.f;
	BeaconLight->CastShadows = false;
	BeaconLight->SetLightColor(BeaconColor());
}

FLinearColor AArenaLMS_Beacon::BeaconColor()
{
	return FLinearColor(1.f, 0.55f, 0.08f);
}

UStaticMeshComponent* AArenaLMS_Beacon::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm,
	const FRotator& Rotation, UMaterialInterface* Material, bool bOwnerNoSee)
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
	Part->SetOwnerNoSee(bOwnerNoSee);
	Part->SetMaterial(0, Material);
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void AArenaLMS_Beacon::BeginPlay()
{
	Super::BeginPlay();

	const FLinearColor C = BeaconColor();
	UMaterialInterface* ColumnMat = ArenaAssets::MakeGlow(this, C, 6.f, 0.45f);
	UMaterialInterface* PointerMat = ArenaAssets::MakeGlow(this, C, 10.f, 0.95f);

	// Light column rising from above the head
	Column = AddPart(BeaconRoot, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 200.f + ArenaLMS::BeaconColumnHeight * 0.5f),
		FVector(14.f, 14.f, ArenaLMS::BeaconColumnHeight), FRotator::ZeroRotator, ColumnMat, false);
	// Spinning downward pointer (two cones and a flat ring) right above the head; hidden for the camper's own view
	AddPart(Spinner, ArenaAssets::Cone(), FVector(0.f, 0.f, 0.f), FVector(40.f, 40.f, 46.f), FRotator(180.f, 0.f, 0.f), PointerMat, true);
	AddPart(Spinner, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 30.f), FVector(70.f, 70.f, 4.f), FRotator::ZeroRotator, PointerMat, true);
	for (int32 i = 0; i < 3; ++i)
	{
		const float Angle = i * 2.f * PI / 3.f;
		AddPart(Spinner, ArenaAssets::Cube(), FVector(FMath::Cos(Angle) * 48.f, FMath::Sin(Angle) * 48.f, 30.f), FVector(18.f, 8.f, 8.f),
			FRotator(0.f, FMath::RadiansToDegrees(Angle), 0.f), PointerMat, true);
	}
}

void AArenaLMS_Beacon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	if (Spinner)
	{
		Spinner->SetRelativeRotation(FRotator(0.f, FMath::Fmod(Age * 180.f, 360.f), 0.f));
		Spinner->SetRelativeLocation(FVector(0.f, 0.f, 160.f + 10.f * FMath::Sin(Age * 5.f)));
	}
	const float Pulse = 0.5f + 0.5f * FMath::Sin(Age * 6.f);
	if (Column)
	{
		const float W = 0.12f + 0.06f * Pulse;
		Column->SetRelativeScale3D(FVector(W, W, ArenaLMS::BeaconColumnHeight / 100.f));
	}
	if (BeaconLight)
	{
		BeaconLight->SetIntensity(7000.f + 9000.f * Pulse);
	}
}

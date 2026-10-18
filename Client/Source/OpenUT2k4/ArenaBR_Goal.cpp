#include "ArenaBR_Goal.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArtItems.h"
#include "ArenaTypes.h"
#include "ArenaNavGraph.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

namespace ArenaBR
{
	constexpr int32 RingSegments = 20;
}

AArenaBR_Goal::AArenaBR_Goal()
{
	PrimaryActorTick.bCanEverTick = true;
	SetActorEnableCollision(false);
	Tags.Add(ArenaNavIgnoreTag());

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Hoop = CreateDefaultSubobject<USceneComponent>(TEXT("Hoop"));
	Hoop->SetupAttachment(Root);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Hoop);
	Light->SetMobility(EComponentMobility::Movable);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 9000.f;
	Light->AttenuationRadius = 900.f;
	Light->CastShadows = false;
}

void AArenaBR_Goal::Setup(int32 InTeam, const FVector& InFloorLocation, const FVector& InHoopCenter, float FacingYaw)
{
	Team = (InTeam == 1) ? 1 : 0;
	FloorLocation = InFloorLocation;
	HoopCenter = InHoopCenter;
	Facing = FacingYaw;
	SetActorLocationAndRotation(FloorLocation, FRotator(0.f, Facing, 0.f));
	if (bBuilt)
	{
		BuildVisuals();
	}
}

void AArenaBR_Goal::BeginPlay()
{
	Super::BeginPlay();
	BuildVisuals();
	bBuilt = true;
}

UStaticMeshComponent* AArenaBR_Goal::AddPart(UStaticMesh* Mesh, const FVector& RelLocation, const FRotator& RelRotation, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Hoop);
	Part->SetRelativeTransform(FTransform(RelRotation, RelLocation, SizeCm / 100.f));
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

void AArenaBR_Goal::BuildVisuals()
{
	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	Parts.Reset();

	// Hoop frame: centred on HoopCenter, the ring plane faces along the actor yaw (towards the centre)
	Hoop->SetWorldLocationAndRotation(HoopCenter, FRotator(0.f, Facing, 0.f));

	const FLinearColor Col = ArenaTeam::Color(Team);
	UMaterialInterface* Frame = ArenaAssets::MakeSurface(this, FLinearColor(0.12f, 0.12f, 0.14f), 0.4f, 0.8f, Col * 0.05f);
	RingMaterial = ArenaAssets::MakeGlow(this, Col, 5.f, 0.9f);
	UMaterialInterface* Fill = ArenaAssets::MakeGlow(this, Col, 1.2f, 0.12f);

	// Imported delivery hoop (team pulse texture); it floats like the original, without posts.
	{
		const ArenaArtItems::FBombingRunArt& Art = ArenaArtItems::GetBombingRunArt();
		ArenaArtItems::FItemMesh GoalMesh = Art.Goal;
		GoalMesh.Size = HoopRadius * Art.GoalWidthPerRadius;
		if (UStaticMeshComponent* Goal = ArenaArtItems::AddItemMesh(this, Hoop, GoalMesh, 1.f, true))
		{
			if (UMaterialInterface* Pulse = ArenaArtItems::LoadMaterial(Art.GoalPulseMaterial[Team == ArenaTeam::Blue ? 1 : 0]))
			{
				Goal->SetMaterial(1, Pulse);
			}
			Parts.Add(Goal);
			Light->SetLightColor(Col);
			return;
		}
	}

	// Ring of segments in the local YZ plane
	const float R = HoopRadius;
	const float SegLen = 2.f * PI * R / ArenaBR::RingSegments * 1.08f;
	for (int32 i = 0; i < ArenaBR::RingSegments; ++i)
	{
		const float A = 2.f * PI * i / ArenaBR::RingSegments;
		const FVector P(0.f, FMath::Cos(A) * R, FMath::Sin(A) * R);
		const FVector Tangent(0.f, -FMath::Sin(A), FMath::Cos(A));
		// Cube stretched along its local Z, which is turned to the circle's tangent
		const FRotator Rot = FRotationMatrix::MakeFromXZ(FVector(1.f, 0.f, 0.f), Tangent).Rotator();
		AddPart(ArenaAssets::Cube(), P, Rot, FVector(14.f, 12.f, SegLen), RingMaterial, false);
	}
	// Faint membrane inside the ring (cylinder axis turned to the facing direction)
	AddPart(ArenaAssets::Cylinder(), FVector::ZeroVector, FRotator(90.f, 0.f, 0.f), FVector(2.f * R, 2.f * R, 3.f), Fill, false);

	// Support posts down to the floor and a base plate
	const float Above = HoopCenter.Z - FloorLocation.Z;
	if (Above > 20.f)
	{
		const float Sides[2] = { -1.f, 1.f };
		for (const float Side : Sides)
		{
			AddPart(ArenaAssets::Cylinder(), FVector(0.f, Side * (R + 16.f), -Above * 0.5f), FRotator::ZeroRotator, FVector(16.f, 16.f, Above), Frame, true);
			AddPart(ArenaAssets::Sphere(), FVector(0.f, Side * (R + 16.f), 0.f), FRotator::ZeroRotator, FVector(26.f), RingMaterial, false);
		}
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, -Above + 4.f), FRotator::ZeroRotator, FVector(2.f * R + 120.f, 2.f * R + 120.f, 8.f), Frame, true);
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, -Above + 9.f), FRotator::ZeroRotator, FVector(2.f * R + 80.f, 2.f * R + 80.f, 2.f), RingMaterial, false);
	}
	Light->SetLightColor(Col);
}

bool AArenaBR_Goal::IsCharacterInside(const FVector& CharacterLocation) const
{
	// A sphere around the ring, plus (for hoops that can be walked into) the column above the base plate
	if (FVector::Dist(CharacterLocation, HoopCenter) < HoopRadius + 80.f)
	{
		return true;
	}
	if (!IsElevated())
	{
		const float Z = CharacterLocation.Z - FloorLocation.Z;
		return FVector::Dist2D(CharacterLocation, FloorLocation) < HoopRadius + 60.f && Z > -60.f && Z < FMath::Max(500.f, HoopCenter.Z - FloorLocation.Z + HoopRadius);
	}
	return false;
}

bool AArenaBR_Goal::SegmentPassesThrough(const FVector& From, const FVector& To) const
{
	const FVector Closest = FMath::ClosestPointOnSegment(HoopCenter, From, To);
	return FVector::Dist(Closest, HoopCenter) < HoopRadius;
}

void AArenaBR_Goal::PlayScoreEffect()
{
	FlashUntil = GetWorld()->GetTimeSeconds() + 2.f;
	ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), HoopCenter, 1.f);
}

void AArenaBR_Goal::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetWorld()->GetTimeSeconds();
	const bool bFlash = Now < FlashUntil;
	const float Pulse = bFlash ? 6.f + 6.f * FMath::Abs(FMath::Sin(Now * 14.f)) : 4.f + 1.5f * FMath::Sin(Now * 2.f);
	if (RingMaterial)
	{
		RingMaterial->SetScalarParameterValue(TEXT("Intensity"), Pulse);
	}
	Light->SetIntensity(bFlash ? 30000.f : 9000.f);
}

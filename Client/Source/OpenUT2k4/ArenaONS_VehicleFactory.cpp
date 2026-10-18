#include "ArenaONS_VehicleFactory.h"
#include "OpenUT2k4.h"
#include "ArenaONS_PowerNode.h"
#include "ArenaAssets.h"
#include "ArenaFX.h"
#include "ArenaGameState.h"
#include "ArenaArtVehicles.h"
#include "ArenaArt.h"
#include "Engine/StaticMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

AArenaONS_VehicleFactory::AArenaONS_VehicleFactory()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	bReplicates = true;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
	SetActorEnableCollision(false);
}

void AArenaONS_VehicleFactory::InitFactory(EArenaONS_VehicleType InType, AArenaONS_PowerNode* InOwnerNode, float InRespawnTime)
{
	VehicleType = InType;
	OwnerNode = InOwnerNode;
	RespawnTime = InRespawnTime > 0.f ? InRespawnTime : AArenaONS_Vehicle::GetSpec(InType).RespawnTime;
}

void AArenaONS_VehicleFactory::BeginPlay()
{
	Super::BeginPlay();
	// Floor marking: a flat bay outline in the owner's colour.
	const FArenaONS_VehicleSpec& S = AArenaONS_Vehicle::GetSpec(VehicleType);
	const FVector Size(S.HalfExtent.X * 2.f + 120.f, S.HalfExtent.Y * 2.f + 120.f, 2.f);
	auto Make = [this](const FVector& Loc, const FVector& SizeCm, UMaterialInterface* Mat) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(ArenaAssets::Cube());
		C->SetupAttachment(RootComponent);
		C->SetRelativeTransform(FTransform(FRotator::ZeroRotator, Loc, SizeCm / 100.f));
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(false);
		if (Mat)
		{
			C->SetMaterial(0, Mat);
		}
		C->RegisterComponent();
		return C;
	};
	// UT2004's ONSVehicleFactory is invisible: the vehicle simply stands where the map put it, and the maps model
	// their own bays. With the vehicle art there is no bay marking; without it (procedural vehicles) a team outline
	// and a dark plate mark the bay.
	if (!ArenaArtVehicles::IsAvailable(VehicleType))
	{
		RingMat = ArenaAssets::MakeGlow(this, FLinearColor(0.7f, 0.7f, 0.7f), 3.f, 0.8f);
		Ring = Make(FVector(0.f, 0.f, 2.f), Size, RingMat);
		Pad = Make(FVector(0.f, 0.f, 3.f), Size - FVector(24.f, 24.f, 0.f), ArenaAssets::MakeSurface(this, FLinearColor(0.08f, 0.08f, 0.09f), 0.8f));
	}

	// With an imported / replacement model: a translucent hologram of the vehicle marks the bay while it rebuilds.
	if (USkeletalMesh* Model = ArenaArtVehicles::IsAvailable(VehicleType) ? ArenaArtVehicles::LoadChassis(VehicleType) : nullptr)
	{
		const ArenaArtVehicles::FVehicleArt* Art = ArenaArtVehicles::Find(VehicleType);
		// Height of the vehicle's centre above the floor at rest.
		float Height = S.HalfExtent.Z;
		for (const FVector& P : S.SuspensionPoints)
		{
			Height = FMath::Max(Height, S.RestLength * (1.f - S.RestCompression) + S.WheelRadius - P.Z);
		}
		Hologram = NewObject<USkeletalMeshComponent>(this);
		Hologram->SetSkeletalMeshAsset(Model);
		Hologram->SetupAttachment(RootComponent);
		Hologram->SetRelativeLocation(FVector(0.f, 0.f, Height) + (Art ? Art->MeshOffset : FVector::ZeroVector));
		Hologram->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Hologram->SetCanEverAffectNavigation(false);
		Hologram->SetCastShadow(false);
		HologramMat = ArenaAssets::MakeGlow(this, FLinearColor(0.5f, 0.5f, 0.5f), 1.f, 0.22f);
		for (int32 i = 0; i < Hologram->GetNumMaterials(); ++i)
		{
			Hologram->SetMaterial(i, HologramMat);
		}
		Hologram->SetVisibility(false);
		Hologram->RegisterComponent();
	}
	NextSpawnTime = 0.f;
}

int32 AArenaONS_VehicleFactory::GetActiveTeam() const
{
	const AArenaONS_PowerNode* Node = OwnerNode.Get();
	if (!Node || !Node->IsActive())
	{
		return ArenaTeam::None;
	}
	return Node->GetTeam();
}

void AArenaONS_VehicleFactory::UpdateLook()
{
	const int32 T = GetActiveTeam();
	if (T == LookTeam)
	{
		return;
	}
	LookTeam = T;
	if (RingMat)
	{
		RingMat->SetVectorParameterValue(TEXT("Color"), (T == 0 || T == 1) ? ArenaTeam::Color(T) : FLinearColor(0.25f, 0.25f, 0.25f));
	}
	if (HologramMat && (T == 0 || T == 1))
	{
		HologramMat->SetVectorParameterValue(TEXT("Color"), ArenaTeam::Color(T));
	}
}

void AArenaONS_VehicleFactory::UpdateHologram()
{
	if (!Hologram)
	{
		return;
	}
	// Shown while the bay is active and its vehicle is being rebuilt.
	const bool bShow = GetActiveTeam() >= 0 && !Vehicle.IsValid();
	if (Hologram->IsVisible() != bShow)
	{
		Hologram->SetVisibility(bShow);
	}
}

bool AArenaONS_VehicleFactory::FindSpawnTransform(FTransform& Out) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const FArenaONS_VehicleSpec& S = AArenaONS_Vehicle::GetSpec(VehicleType);
	const FVector Base = GetActorLocation();
	const FRotator Rot(0.f, GetActorRotation().Yaw, 0.f);
	const float Lift = S.HalfExtent.Z + S.RestLength * (1.f - S.RestCompression) + S.WheelRadius + 15.f;
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSFactory), false, this);
	const FCollisionShape Box = FCollisionShape::MakeBox(S.HalfExtent + FVector(10.f, 10.f, 5.f));

	// Spiral of candidates around the bay
	TArray<FVector> Offsets = { FVector::ZeroVector };
	for (int32 RingIdx = 1; RingIdx <= 3; ++RingIdx)
	{
		const float R = RingIdx * 220.f;
		for (int32 i = 0; i < 8; ++i)
		{
			const float A = FMath::DegreesToRadians(45.f * i);
			Offsets.Add(FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 0.f));
		}
	}
	for (const FVector& Off : Offsets)
	{
		const FVector P = Base + Rot.RotateVector(Off);
		FHitResult Floor;
		if (!World->LineTraceSingleByChannel(Floor, P + FVector(0.f, 0.f, 300.f), P - FVector(0.f, 0.f, 800.f), ECC_Visibility, QP) || Floor.ImpactNormal.Z < 0.7f)
		{
			continue;
		}
		const FVector Center = FVector(P.X, P.Y, Floor.ImpactPoint.Z + Lift);
		if (World->OverlapBlockingTestByChannel(Center, Rot.Quaternion(), ECC_Pawn, Box, QP))
		{
			continue;
		}
		Out = FTransform(Rot, Center);
		return true;
	}
	return false;
}

AArenaONS_Vehicle* AArenaONS_VehicleFactory::SpawnVehicle()
{
	UWorld* World = GetWorld();
	const int32 T = GetActiveTeam();
	if (!World || !HasAuthority() || T < 0 || Vehicle.IsValid())
	{
		return nullptr;
	}
	FTransform Xf;
	if (!FindSpawnTransform(Xf))
	{
		if (++SpawnFailures == 3)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("ONS: factory for %s at %s has no clear spot"), *AArenaONS_Vehicle::GetTypeName(VehicleType), *GetActorLocation().ToCompactString());
		}
		NextSpawnTime = World->GetTimeSeconds() + 3.f;
		return nullptr;
	}
	SpawnFailures = 0;
	AArenaONS_Vehicle* V = World->SpawnActorDeferred<AArenaONS_Vehicle>(AArenaONS_Vehicle::StaticClass(), Xf, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!V)
	{
		return nullptr;
	}
	V->InitVehicle(VehicleType, T, this);
	V->FinishSpawning(Xf);
	Vehicle = V;
	ArenaFX::SpawnImpact(World, Xf.GetLocation(), FVector::UpVector, ArenaTeam::Color(T), 120.f, TEXT("Impact.ShockBall"));
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: spawned %s for team %d at %s"), *V->GetVehicleName(), T, *Xf.GetLocation().ToCompactString());
	return V;
}

void AArenaONS_VehicleFactory::ResetFactory()
{
	if (AArenaONS_Vehicle* V = Vehicle.Get())
	{
		V->EjectAll();
		V->Destroy();
	}
	Vehicle.Reset();
	NextSpawnTime = 0.f;
	LookTeam = -2;
}

void AArenaONS_VehicleFactory::NotifyVehicleDestroyed(AArenaONS_Vehicle* InVehicle)
{
	if (Vehicle.Get() == InVehicle)
	{
		Vehicle.Reset();
		NextSpawnTime = GetWorld() ? GetWorld()->GetTimeSeconds() + RespawnTime : 0.f;
	}
}

void AArenaONS_VehicleFactory::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateLook();
	UpdateHologram();
	if (!HasAuthority())
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	const AArenaGameState* GS = World->GetGameState<AArenaGameState>();
	if (GS && (GS->Phase == EArenaMatchPhase::Ended || GS->Phase == EArenaMatchPhase::RoundOver))
	{
		return;
	}
	AArenaONS_Vehicle* V = Vehicle.Get();
	const int32 T = GetActiveTeam();
	if (V)
	{
		// An unused vehicle parked at a bay that changed hands is recycled for the new owner.
		if (V->IsEmpty() && T >= 0 && V->GetTeam() != T && FVector::Dist(V->GetActorLocation(), GetActorLocation()) < 800.f)
		{
			V->Destroy();
			Vehicle.Reset();
			NextSpawnTime = Now + 2.f;
		}
		// Abandoned far away for a long time: count it as lost.
		return;
	}
	if (T >= 0 && Now >= NextSpawnTime)
	{
		SpawnVehicle();
	}
}

#include "ArenaCarryable.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaGameState.h"
#include "ArenaTypes.h"
#include "ArenaWorldZones.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/WorldSettings.h"

AArenaCarryable::AArenaCarryable()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Model = CreateDefaultSubobject<USceneComponent>(TEXT("Model"));
	Model->SetupAttachment(Root);

	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	Trigger->SetupAttachment(Model);
	Trigger->InitSphereRadius(80.f);
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetCanEverAffectNavigation(false);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Model);
	Light->SetMobility(EComponentMobility::Movable);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 6000.f;
	Light->AttenuationRadius = 450.f;
	Light->CastShadows = false;
}

void AArenaCarryable::BeginPlay()
{
	Super::BeginPlay();
	HomeLocation = GetActorLocation();
	Model->SetRelativeLocation(FVector(0.f, 0.f, RestHeight));
	BuildModel();
	bModelBuilt = true;
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AArenaCarryable::OnOverlap);
	bCheckOverlaps = true;
}

void AArenaCarryable::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseFromCarrier();
	Super::EndPlay(EndPlayReason);
}

void AArenaCarryable::SetTeam(int32 InTeam)
{
	Team = (InTeam == 0 || InTeam == 1) ? InTeam : -1;
	if (bModelBuilt)
	{
		BuildModel();
	}
}

void AArenaCarryable::SetHomeLocation(const FVector& FloorLocation)
{
	HomeLocation = FloorLocation;
	SetActorLocation(FloorLocation);
	if (CarryState == EArenaCarryState::Home)
	{
		PlaceModel(HomeLocation + FVector(0.f, 0.f, RestHeight), GetActorRotation().Yaw);
	}
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaCarryable::AddModelPart(UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Model);
	Part->SetRelativeTransform(FTransform(FRotator::ZeroRotator, Location, SizeCm / 100.f));
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
	ModelParts.Add(Part);
	return Part;
}

void AArenaCarryable::BuildModel()
{
	for (UStaticMeshComponent* Part : ModelParts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	ModelParts.Reset();

	const FLinearColor Col = ArenaTeam::Color(Team);
	AddModelPart(ArenaAssets::Sphere(), FVector::ZeroVector, FVector(36.f), ArenaAssets::MakeSurface(this, Col, 0.3f, 0.6f, Col * 0.4f), true);
	AddModelPart(ArenaAssets::Sphere(), FVector::ZeroVector, FVector(46.f), ArenaAssets::MakeGlow(this, Col, 3.f, 0.35f), false);
	Light->SetLightColor(Col);
}

FString AArenaCarryable::GetCarrierHudText() const
{
	return FString::Printf(TEXT("YOU HAVE THE %s"), *DisplayName.ToUpper());
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

bool AArenaCarryable::IsMatchPlaying() const
{
	const UWorld* World = GetWorld();
	const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
	return GS && GS->IsPlaying();
}

FVector AArenaCarryable::GetObjectLocation() const
{
	if (CarryState == EArenaCarryState::Carried && IsValid(Carrier))
	{
		return Carrier->GetActorLocation();
	}
	return Model ? Model->GetComponentLocation() : GetActorLocation();
}

float AArenaCarryable::GetTimeOnGround() const
{
	return CarryState == EArenaCarryState::Dropped && GetWorld() ? GetWorld()->GetTimeSeconds() - DroppedTime : 0.f;
}

void AArenaCarryable::PlaceModel(const FVector& WorldLocation, float Yaw)
{
	Model->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	Model->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepWorldTransform);
	Model->SetWorldLocationAndRotation(WorldLocation, FRotator(0.f, Yaw, 0.f));
	Model->SetWorldScale3D(FVector::OneVector);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	bCheckOverlaps = true;
}

void AArenaCarryable::ReleaseFromCarrier()
{
	if (Carrier)
	{
		if (Carrier->CarriedObject == this)
		{
			Carrier->CarriedObject = nullptr;
		}
		LastCarrier = Carrier;
		Carrier = nullptr;
	}
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

bool AArenaCarryable::PickUp(AArenaCharacter* Character)
{
	if (!Character || !HasAuthority() || Character->CarriedObject || CarryState == EArenaCarryState::Carried)
	{
		return false;
	}
	Carrier = Character;
	Character->CarriedObject = this;
	CarryState = EArenaCarryState::Carried;
	FlightVelocity = FVector::ZeroVector;
	bCheckOverlaps = false;
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	USceneComponent* AttachParent = Character->BodyRoot ? Character->BodyRoot.Get() : Character->GetRootComponent();
	Model->AttachToComponent(AttachParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	Model->SetRelativeLocationAndRotation(CarryOffset, CarryRotation);
	Model->SetRelativeScale3D(FVector(CarryScale));

	OnPickedUp(Character);
	return true;
}

void AArenaCarryable::Drop(AController* By)
{
	if (CarryState != EArenaCarryState::Carried)
	{
		return;
	}
	if (bNetDriven)
	{
		// a UT2004 server's object whose carrier went away here first (its pawn closed): rest where it is until the
		// server's next word, no rules
		const FVector At = Model->GetComponentLocation();
		ReleaseFromCarrier();
		CarryState = EArenaCarryState::Dropped;
		DroppedTime = GetWorld()->GetTimeSeconds();
		PlaceModel(At, Model->GetComponentRotation().Yaw);
		Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		bCheckOverlaps = false;
		return;
	}
	AArenaCharacter* OldCarrier = Carrier;
	AController* CarrierController = IsValid(OldCarrier) ? OldCarrier->GetController() : nullptr;
	const FVector From = IsValid(OldCarrier) ? OldCarrier->GetActorLocation() : Model->GetComponentLocation();
	const float Yaw = IsValid(OldCarrier) ? OldCarrier->GetActorRotation().Yaw : 0.f;
	ReleaseFromCarrier();

	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaCarryableDrop), false, this);
	if (OldCarrier)
	{
		Query.AddIgnoredActor(OldCarrier);
	}
	const bool bFloor = GetWorld()->LineTraceSingleByObjectType(Hit, From, From - FVector(0.f, 0.f, 20000.f), FCollisionObjectQueryParams(ECC_WorldStatic), Query);

	CarryState = EArenaCarryState::Dropped;
	DroppedTime = GetWorld()->GetTimeSeconds();
	NoRepickUntil = 0.f;
	OnDropped(By ? By : CarrierController);

	if (!bFloor)
	{
		// Fell into a bottomless area: counts as an automatic return
		ReturnHome(nullptr);
		return;
	}
	PlaceModel(Hit.ImpactPoint + FVector(0.f, 0.f, RestHeight), Yaw);
}

void AArenaCarryable::Throw(const FVector& Start, const FVector& Velocity, AController* By)
{
	if (!HasAuthority())
	{
		return;
	}
	const bool bWasCarried = CarryState == EArenaCarryState::Carried;
	ReleaseFromCarrier();
	PlaceModel(Start, Velocity.Rotation().Yaw);
	CarryState = EArenaCarryState::InFlight;
	FlightVelocity = Velocity;
	FlightStartTime = GetWorld()->GetTimeSeconds();
	NoRepickUntil = FlightStartTime + RepickDelay;
	if (bWasCarried)
	{
		OnDropped(By);
	}
}

void AArenaCarryable::ReturnHome(AController* By)
{
	if (CarryState == EArenaCarryState::Home)
	{
		return;
	}
	SendHome();
	OnReturned(By);
}

void AArenaCarryable::SendHome()
{
	ReleaseFromCarrier();
	CarryState = EArenaCarryState::Home;
	FlightVelocity = FVector::ZeroVector;
	NoRepickUntil = 0.f;
	PlaceModel(HomeLocation + FVector(0.f, 0.f, RestHeight), GetActorRotation().Yaw);
}

void AArenaCarryable::SetNetState(EArenaCarryState NewState, AArenaCharacter* NewCarrier, const FVector& Location, const FVector& Velocity)
{
	bNetDriven = true;
	bCheckOverlaps = false;
	if (NewState == EArenaCarryState::Carried && IsValid(NewCarrier))
	{
		if (Carrier == NewCarrier && CarryState == EArenaCarryState::Carried)
		{
			return;
		}
		ReleaseFromCarrier();
		Carrier = NewCarrier;
		if (!NewCarrier->CarriedObject)
		{
			NewCarrier->CarriedObject = this;
		}
		CarryState = EArenaCarryState::Carried;
		FlightVelocity = FVector::ZeroVector;
		USceneComponent* AttachParent = NewCarrier->BodyRoot ? NewCarrier->BodyRoot.Get() : NewCarrier->GetRootComponent();
		Model->AttachToComponent(AttachParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		Model->SetRelativeLocationAndRotation(CarryOffset, CarryRotation);
		Model->SetRelativeScale3D(FVector(CarryScale));
	}
	else
	{
		const bool bWasCarried = CarryState == EArenaCarryState::Carried;
		ReleaseFromCarrier();
		switch (NewState)
		{
		case EArenaCarryState::InFlight:
			if (bWasCarried || CarryState != EArenaCarryState::InFlight || FVector::DistSquared(Model->GetComponentLocation(), Location) > FMath::Square(150.f))
			{
				PlaceModel(Location, Velocity.Rotation().Yaw);
			}
			CarryState = EArenaCarryState::InFlight;
			FlightVelocity = Velocity;
			FlightStartTime = GetWorld()->GetTimeSeconds();
			break;
		case EArenaCarryState::Dropped:
			CarryState = EArenaCarryState::Dropped;
			FlightVelocity = FVector::ZeroVector;
			DroppedTime = GetWorld()->GetTimeSeconds();
			PlaceModel(Location + FVector(0.f, 0.f, RestHeight), Model->GetComponentRotation().Yaw);
			break;
		default:
			CarryState = EArenaCarryState::Home;
			FlightVelocity = FVector::ZeroVector;
			PlaceModel(HomeLocation + FVector(0.f, 0.f, RestHeight), GetActorRotation().Yaw);
			break;
		}
	}
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bCheckOverlaps = false;
	OnNetStateChanged();
}

// ---------------------------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------------------------

bool AArenaCarryable::CanBePickedUpBy(AArenaCharacter* Character) const
{
	if (!Character || !Character->IsAlive() || Character->CarriedObject || !IsMatchPlaying())
	{
		return false;
	}
	if (Character == LastCarrier.Get() && GetWorld()->GetTimeSeconds() < NoRepickUntil)
	{
		return false;
	}
	return true;
}

void AArenaCarryable::OnTouched(AArenaCharacter* Character)
{
	if (CanBePickedUpBy(Character))
	{
		PickUp(Character);
	}
}

void AArenaCarryable::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	TryTouch(Cast<AArenaCharacter>(OtherActor));
}

void AArenaCarryable::TryTouch(AArenaCharacter* Character)
{
	if (!Character || !Character->IsAlive() || !HasAuthority() || bNetDriven || CarryState == EArenaCarryState::Carried)
	{
		return;
	}
	OnTouched(Character);
}

// ---------------------------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------------------------

void AArenaCarryable::TickFlight(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	const FVector From = Model->GetComponentLocation();
	// UE2 physFalling in the physics volume at the item: its full Gravity vector (sideways tubes carry a thrown
	// ball / flag), TerminalVelocity, and ZoneVelocity when the volume moves projectiles and effects (bMoveProjectiles)
	FVector ZoneMove = FVector::ZeroVector;
	const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this);
	if (const AArenaPhysicsVolume* Vol = Zones && Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(From) : nullptr)
	{
		FlightVelocity = Vol->ClampToTerminalVelocity(FlightVelocity + Vol->GetGravityVector() * FlightGravityScale * DeltaSeconds);
		if (Vol->bMoveProjectiles)
		{
			ZoneMove = Vol->ZoneVelocity;
		}
	}
	else
	{
		// outside volumes: the level's gravity and its DefaultPhysicsVolume's TerminalVelocity
		FlightVelocity = ArenaWorldZones::NewFallVelocity(World, From, FlightVelocity, FlightGravityScale, DeltaSeconds);
	}

	const FVector To = From + (FlightVelocity + ZoneMove) * DeltaSeconds;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaCarryableFlight), false, this);
	FHitResult Hit;
	const bool bHit = World->SweepSingleByObjectType(Hit, From, To, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
		FCollisionShape::MakeSphere(FlightRadius), Query);
	if (bHit && !Hit.bStartPenetrating)
	{
		Model->SetWorldLocation(Hit.Location);
		FlightVelocity = FMath::GetReflectionVector(FlightVelocity, Hit.ImpactNormal) * Bounciness;
		if (Hit.ImpactNormal.Z > 0.7f && FlightVelocity.Size() < 200.f)
		{
			// Came to rest on a floor
			CarryState = EArenaCarryState::Dropped;
			DroppedTime = World->GetTimeSeconds();
			FlightVelocity = FVector::ZeroVector;
			PlaceModel(Hit.ImpactPoint + FVector(0.f, 0.f, RestHeight), Model->GetComponentRotation().Yaw);
			OnLanded();
			return;
		}
	}
	else
	{
		Model->SetWorldLocation(To);
	}

	const AWorldSettings* WS = World->GetWorldSettings();
	const float KillZ = WS ? WS->KillZ : -100000.f;
	if (bNetDriven)
	{
		return; // the server says where it is
	}
	if (Model->GetComponentLocation().Z < KillZ || World->GetTimeSeconds() - FlightStartTime > 10.f)
	{
		ReturnHome(nullptr);
	}
}

void AArenaCarryable::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority())
	{
		return;
	}
	if (bNetDriven)
	{
		// a UT2004 server's object: only its flight is simulated between the server's updates
		if (CarryState == EArenaCarryState::InFlight)
		{
			TickFlight(DeltaSeconds);
		}
		Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	switch (CarryState)
	{
	case EArenaCarryState::Carried:
		if (!IsValid(Carrier) || Carrier->IsDead())
		{
			Drop(nullptr);
		}
		break;
	case EArenaCarryState::Dropped:
		if (GetAutoReturnTime() > 0.f && GetWorld()->GetTimeSeconds() - DroppedTime >= GetAutoReturnTime())
		{
			ReturnHome(nullptr);
		}
		break;
	case EArenaCarryState::InFlight:
		TickFlight(DeltaSeconds);
		break;
	default:
		break;
	}

	// Re-test characters already inside the trigger (rules may allow a pickup later, e.g. after the throw delay)
	if (CarryState != EArenaCarryState::Carried)
	{
		bCheckOverlaps = false;
		TArray<AActor*> Overlapping;
		Trigger->GetOverlappingActors(Overlapping, AArenaCharacter::StaticClass());
		for (AActor* Actor : Overlapping)
		{
			TryTouch(Cast<AArenaCharacter>(Actor));
			if (CarryState == EArenaCarryState::Carried)
			{
				break;
			}
		}
	}
}

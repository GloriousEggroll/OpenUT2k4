#include "ArenaPickup.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArtItems.h"
#include "ArenaCharacter.h"
#include "ArenaWeapon.h"
#include "ArenaPlayerController.h"
#include "ArenaBotController.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	const FLinearColor HealthWhite(0.92f, 0.92f, 0.9f);
	const FLinearColor HealthGreen(0.2f, 1.f, 0.3f);
	const FLinearColor VialBlue(0.3f, 0.6f, 1.f);
	const FLinearColor ShieldGold(1.f, 0.78f, 0.18f);
	const FLinearColor DamagePurple(0.62f, 0.15f, 1.f);
	const FLinearColor AdrenalineColor(0.62f, 1.f, 0.15f);
	/** Height of the actor above the floor (ArenaLevelBuilder places pickups this high). */
	constexpr float PickupFloorHeight = 60.f;

	const AArenaWeapon* PickupWeaponCDO(TSubclassOf<AArenaWeapon> Class)
	{
		return Class ? Class->GetDefaultObject<AArenaWeapon>() : nullptr;
	}
}

AArenaPickup::AArenaPickup()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	Trigger->SetupAttachment(Root);
	Trigger->InitSphereRadius(70.f);
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetCanEverAffectNavigation(false);

	ModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ModelRoot"));
	ModelRoot->SetupAttachment(Root);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Root);
	Light->SetMobility(EComponentMobility::Movable);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 4000.f;
	Light->AttenuationRadius = 350.f;
	Light->CastShadows = false;
	Light->SetRelativeLocation(FVector(0.f, 0.f, 20.f));
}

void AArenaPickup::Setup(EArenaPickupType InType, TSubclassOf<AArenaWeapon> InWeaponClass)
{
	PickupType = InType;
	WeaponClass = InWeaponClass;
	if (bModelBuilt)
	{
		BuildModel();
	}
}

void AArenaPickup::SetOnBase(bool bOnBase)
{
	BaseHint = bOnBase ? 1 : 0;
	if (bModelBuilt)
	{
		BuildModel();
	}
}

void AArenaPickup::BeginPlay()
{
	Super::BeginPlay();

	BobPhase = FMath::FRandRange(0.f, 2.f * PI);
	ModelRoot->SetRelativeRotation(FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f));
	BuildModel();
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AArenaPickup::OnOverlap);
	SetAvailable(true);
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaPickup::AddModelPart(UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& SizeCm, UMaterialInterface* Material)
{
	if (!Mesh || bArtModel)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(ModelRoot);
	Part->SetRelativeTransform(FTransform(Rotation, Location, SizeCm / 100.f));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCollisionResponseToAllChannels(ECR_Ignore);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCastShadow(false);
	Part->SetCanEverAffectNavigation(false);
	if (Material)
	{
		Part->SetMaterial(0, Material);
	}
	Part->RegisterComponent();
	ModelParts.Add(Part);
	return Part;
}

void AArenaPickup::SetNetItemMesh(UStaticMesh* Mesh)
{
	if (!bModelBuilt)
	{
		BuildModel();
	}
	UStaticMeshComponent* Item = bArtModel && ModelParts.Num() > 0 ? ModelParts[0].Get() : nullptr;
	if (!Mesh || !Item || !Item->GetStaticMesh() || Item->GetStaticMesh() == Mesh)
	{
		return;
	}
	// the new mesh takes the old one's place and largest dimension
	const float OldSize = float(Item->GetStaticMesh()->GetBounds().BoxExtent.GetMax());
	const float NewSize = float(Mesh->GetBounds().BoxExtent.GetMax());
	Item->SetStaticMesh(Mesh);
	Item->EmptyOverrideMaterials();
	if (OldSize > 0.f && NewSize > 0.f)
	{
		Item->SetRelativeScale3D(Item->GetRelativeScale3D() * (OldSize / NewSize));
	}
}

void AArenaPickup::SetNetItemOffset(const FVector& Offset)
{
	if (!bModelBuilt)
	{
		BuildModel();
	}
	UStaticMeshComponent* Item = bArtModel && ModelParts.Num() > 0 ? ModelParts[0].Get() : nullptr;
	if (!Item)
	{
		return;
	}
	// UE2 draws a mesh at -PrePivot in its own (scaled, turned) frame
	const FVector Shift = -Item->GetRelativeRotation().RotateVector(Offset * Item->GetRelativeScale3D());
	Item->SetRelativeLocation(Item->GetRelativeLocation() - NetItemOffset + Shift);
	NetItemOffset = Shift;
}

void AArenaPickup::BuildModel()
{
	for (UStaticMeshComponent* Part : ModelParts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	ModelParts.Reset();
	if (BaseMesh)
	{
		BaseMesh->DestroyComponent();
		BaseMesh = nullptr;
	}
	bModelBuilt = true;

	// Imported item mesh (and base) when available; the procedural shapes below are skipped then, only the
	// light colour is taken from them.
	bArtModel = false;
	if (const ArenaArtItems::FPickupArt* Art = ArenaArtItems::FindPickupArt(PickupType, WeaponClass))
	{
		if (UStaticMeshComponent* ItemMesh = ArenaArtItems::AddItemMesh(this, ModelRoot, Art->Item))
		{
			ModelParts.Add(ItemMesh);
			bArtModel = true;
			const bool bWantBase = BaseHint < 0 ? Art->bBaseByDefault : BaseHint > 0;
			if (bWantBase && Art->Base.Mesh)
			{
				// The actor floats PickupFloorHeight above the floor; the base stands on it and does not bob.
				ArenaArtItems::FItemMesh B = Art->Base;
				B.Offset = FVector(0.f, 0.f, -PickupFloorHeight);
				BaseMesh = ArenaArtItems::AddItemMesh(this, RootComponent, B, 1.f, false);
			}
		}
	}

	UStaticMesh* Cube = ArenaAssets::Cube();
	UStaticMesh* Sphere = ArenaAssets::Sphere();
	UStaticMesh* Cylinder = ArenaAssets::Cylinder();
	const FRotator Upright = FRotator::ZeroRotator;
	const FRotator AlongX(90.f, 0.f, 0.f); // cylinder axis Z -> X

	FLinearColor LightColor = FLinearColor::White;
	float LightIntensity = 4000.f;

	switch (PickupType)
	{
	case EArenaPickupType::HealthPack:
	case EArenaPickupType::SuperHealth:
	{
		const float S = PickupType == EArenaPickupType::SuperHealth ? 1.5f : 1.f;
		UMaterialInterface* White = ArenaAssets::MakeSurface(this, HealthWhite, 0.4f, 0.f, HealthWhite * 0.3f);
		UMaterialInterface* Green = ArenaAssets::MakeGlow(this, HealthGreen, 8.f);
		AddModelPart(Cube, FVector::ZeroVector, Upright, FVector(18.f, 56.f, 18.f) * S, White);
		AddModelPart(Cube, FVector::ZeroVector, Upright, FVector(18.f, 18.f, 56.f) * S, White);
		AddModelPart(Cube, FVector::ZeroVector, Upright, FVector(21.f, 44.f, 9.f) * S, Green);
		AddModelPart(Cube, FVector::ZeroVector, Upright, FVector(21.f, 9.f, 44.f) * S, Green);
		if (PickupType == EArenaPickupType::SuperHealth)
		{
			AddModelPart(Sphere, FVector::ZeroVector, Upright, FVector(110.f), ArenaAssets::MakeGlow(this, FLinearColor(0.3f, 0.7f, 1.f), 1.5f, 0.25f));
			LightIntensity = 9000.f;
		}
		LightColor = HealthGreen;
		break;
	}
	case EArenaPickupType::HealthVial:
	{
		AddModelPart(Sphere, FVector(0.f, 0.f, -4.f), Upright, FVector(22.f), ArenaAssets::MakeGlow(this, VialBlue, 8.f));
		AddModelPart(Cylinder, FVector(0.f, 0.f, 12.f), Upright, FVector(8.f, 8.f, 12.f), ArenaAssets::MakeSurface(this, HealthWhite, 0.3f, 0.5f));
		LightColor = VialBlue;
		LightIntensity = 1500.f;
		break;
	}
	case EArenaPickupType::ShieldSmall:
	case EArenaPickupType::ShieldLarge:
	{
		const bool bLarge = PickupType == EArenaPickupType::ShieldLarge;
		const float D = bLarge ? 64.f : 44.f;
		UMaterialInterface* Gold = ArenaAssets::MakeSurface(this, ShieldGold, 0.25f, 0.9f, ShieldGold * 0.4f);
		// upright disc (cylinder axis along X)
		AddModelPart(Cylinder, FVector::ZeroVector, AlongX, FVector(D, D, 10.f), Gold);
		AddModelPart(Cylinder, FVector::ZeroVector, AlongX, FVector(D * 0.6f, D * 0.6f, 14.f), ArenaAssets::MakeGlow(this, ShieldGold, 8.f));
		if (bLarge)
		{
			AddModelPart(Cylinder, FVector::ZeroVector, AlongX, FVector(D + 18.f, D + 18.f, 4.f), ArenaAssets::MakeGlow(this, FLinearColor(1.f, 0.55f, 0.1f), 6.f, 0.8f));
			LightIntensity = 9000.f;
		}
		LightColor = ShieldGold;
		break;
	}
	case EArenaPickupType::DoubleDamage:
	{
		AddModelPart(Cube, FVector::ZeroVector, FRotator(45.f, 0.f, 45.f), FVector(40.f), ArenaAssets::MakeSurface(this, DamagePurple, 0.2f, 0.6f, DamagePurple * 0.6f));
		AddModelPart(Sphere, FVector::ZeroVector, Upright, FVector(62.f), ArenaAssets::MakeGlow(this, DamagePurple, 5.f, 0.5f));
		LightColor = DamagePurple;
		LightIntensity = 12000.f;
		break;
	}
	case EArenaPickupType::Adrenaline:
	{
		UMaterialInterface* Pill = ArenaAssets::MakeGlow(this, AdrenalineColor, 7.f);
		const FRotator Tilt(90.f, 0.f, 30.f);
		AddModelPart(Cylinder, FVector::ZeroVector, Tilt, FVector(12.f, 12.f, 18.f), Pill);
		const FVector Axis = Tilt.RotateVector(FVector::UpVector);
		AddModelPart(Sphere, Axis * 9.f, Upright, FVector(12.f), Pill);
		AddModelPart(Sphere, -Axis * 9.f, Upright, FVector(12.f), ArenaAssets::MakeSurface(this, HealthWhite, 0.3f));
		LightColor = AdrenalineColor;
		LightIntensity = 1500.f;
		break;
	}
	case EArenaPickupType::Weapon:
	{
		const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass);
		const FLinearColor Col = CDO ? CDO->WeaponColor : FLinearColor(0.5f, 0.5f, 0.55f);
		const float Len = 50.f + (CDO ? FMath::Clamp(CDO->Slot, 1, 9) * 5.f : 20.f);
		UMaterialInterface* Body = ArenaAssets::MakeSurface(this, Col, 0.35f, 0.6f, Col * 0.15f);
		AddModelPart(Cube, FVector(-5.f, 0.f, 0.f), Upright, FVector(Len, 14.f, 16.f), Body);
		AddModelPart(Cylinder, FVector(Len * 0.5f + 10.f, 0.f, 3.f), AlongX, FVector(8.f, 8.f, 36.f), ArenaAssets::MakeSurface(this, FLinearColor(0.2f, 0.2f, 0.22f), 0.3f, 0.8f));
		AddModelPart(Cube, FVector(-Len * 0.3f, 0.f, -14.f), FRotator(-15.f, 0.f, 0.f), FVector(12.f, 10.f, 22.f), Body);
		AddModelPart(Cube, FVector(0.f, 0.f, 9.f), Upright, FVector(Len * 0.7f, 6.f, 3.f), ArenaAssets::MakeGlow(this, Col, 8.f));
		LightColor = Col;
		LightIntensity = 5000.f;
		break;
	}
	case EArenaPickupType::Ammo:
	default:
	{
		const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass);
		const FLinearColor Col = CDO ? CDO->WeaponColor : FLinearColor(0.5f, 0.5f, 0.55f);
		AddModelPart(Cube, FVector(0.f, 0.f, -20.f), Upright, FVector(30.f, 22.f, 22.f), ArenaAssets::MakeSurface(this, Col * 0.7f, 0.5f, 0.4f));
		AddModelPart(Cube, FVector(0.f, 0.f, -20.f), Upright, FVector(32.f, 24.f, 5.f), ArenaAssets::MakeGlow(this, Col, 6.f));
		LightColor = Col;
		LightIntensity = 1500.f;
		break;
	}
	}

	if (Light)
	{
		Light->SetLightColor(LightColor);
		Light->SetIntensity(LightIntensity);
	}
}

// ---------------------------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------------------------

void AArenaPickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bAvailable)
	{
		if (GetWorld()->GetTimeSeconds() >= RespawnAt)
		{
			SetAvailable(true);
			CheckOverlappingCharacters();
		}
		return;
	}

	BobPhase += DeltaSeconds * 2.2f;
	if (BobPhase > 2.f * PI)
	{
		BobPhase -= 2.f * PI;
	}
	const float SpinSpeed = PickupType == EArenaPickupType::DoubleDamage ? 120.f : 80.f;
	FRotator Rot = ModelRoot->GetRelativeRotation();
	Rot.Yaw = FRotator::NormalizeAxis(Rot.Yaw + SpinSpeed * DeltaSeconds);
	ModelRoot->SetRelativeLocationAndRotation(FVector(0.f, 0.f, 8.f * FMath::Sin(BobPhase)), Rot);
}

void AArenaPickup::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!bAvailable || !HasAuthority())
	{
		return;
	}
	if (AArenaCharacter* Character = Cast<AArenaCharacter>(OtherActor))
	{
		GiveTo(Character);
	}
}

void AArenaPickup::CheckOverlappingCharacters()
{
	if (!bAvailable || !HasAuthority())
	{
		return;
	}
	TArray<AActor*> Overlapping;
	Trigger->GetOverlappingActors(Overlapping, AArenaCharacter::StaticClass());
	for (AActor* Actor : Overlapping)
	{
		if (AArenaCharacter* Character = Cast<AArenaCharacter>(Actor))
		{
			if (GiveTo(Character) && !bAvailable)
			{
				break;
			}
		}
	}
}

bool AArenaPickup::GiveTo(AArenaCharacter* Character)
{
	if (!Character || !Character->IsAlive() || !bAvailable)
	{
		return false;
	}
	if (const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>())
	{
		if (const UArenaGameRules* Rules = GM->GetRules(); Rules && !Rules->CanPickUp(Character, this))
		{
			return false;
		}
	}

	bool bGiven = false;
	FName Sound = NAME_None;
	const AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	const UArenaGameRules* Rules = GM ? GM->GetRules() : nullptr;
	// UE2: in non-team games super health (vial, keg) and shields are taken even when they can't be used (denial)
	const bool bDeny = !(GM && GM->IsTeamGame());
	const bool bWeaponStay = !Rules || Rules->IsWeaponStay();
	bool bStays = false;
	switch (PickupType)
	{
	case EArenaPickupType::HealthVial:
		// MiniHealthPack: +5 up to SuperHealthMax 199
		bGiven = Character->GiveHealth(5.f, ArenaTuning::SuperHealthMax) || bDeny;
		Sound = TEXT("S_Pickup_Health");
		break;
	case EArenaPickupType::HealthPack:
		bGiven = Character->GiveHealth(25.f, ArenaTuning::MaxHealth);
		Sound = TEXT("S_Pickup_Health");
		break;
	case EArenaPickupType::SuperHealth:
		bGiven = Character->GiveHealth(100.f, ArenaTuning::SuperHealthMax) || bDeny;
		Sound = TEXT("S_Pickup_Power");
		break;
	case EArenaPickupType::ShieldSmall:
		bGiven = Character->AddShieldStrengthUE2(50) || bDeny;
		Sound = TEXT("S_Pickup_Shield");
		break;
	case EArenaPickupType::ShieldLarge:
		bGiven = Character->AddShieldStrengthUE2(100) || bDeny;
		Sound = TEXT("S_Pickup_Shield");
		break;
	case EArenaPickupType::DoubleDamage:
		Character->GiveDoubleDamage(ArenaTuning::DoubleDamageDuration);
		bGiven = true;
		Sound = TEXT("S_Pickup_Power");
		break;
	case EArenaPickupType::Adrenaline:
		// AdrenalinePickup: always taken (AwardAdrenaline clamps)
		Character->AwardAdrenaline(2.f);
		bGiven = true;
		Sound = TEXT("S_Pickup_Power");
		break;
	case EArenaPickupType::Weapon:
		if (AArenaWeapon* Owned = WeaponClass ? Character->FindWeapon(WeaponClass) : nullptr)
		{
			// UE2 HandlePickupQuery: with weapon stay an owned weapon can't be picked up again (the assault rifle
			// becomes dual); without it, it adds the weapon's initial ammo
			bGiven = Owned->OnRepeatPickup(bWeaponStay);
		}
		else
		{
			bGiven = WeaponClass && Character->GiveWeapon(WeaponClass);
		}
		bStays = bWeaponStay;
		Sound = TEXT("S_Pickup_Weapon");
		break;
	case EArenaPickupType::Ammo:
		if (const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass))
		{
			if (AArenaWeapon* Owned = Character->FindWeapon(WeaponClass))
			{
				bGiven = Owned->GiveAmmoPickup();
			}
			else
			{
				bGiven = Character->GiveAmmo(WeaponClass, FMath::Max(1, CDO->AmmoPickupAmount));
			}
		}
		Sound = TEXT("S_Pickup_Ammo");
		break;
	}

	if (!bGiven)
	{
		return false;
	}

	ArenaAssets::PlaySoundAt(this, Sound, GetActorLocation());
	// UE2 Pawn.HandlePickup: MakeNoise(0.2)
	AArenaBotController::ReportNoise(Character, Character->GetActorLocation(), 0.2f);
	if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(Character->GetController()))
	{
		PC->NotifyPickup(GetDisplayName());
	}

	// Weapons stay (UE2 bWeaponStay); everything else respawns after its RespawnTime.
	if (!bStays)
	{
		SetAvailable(false);
	}
	return true;
}

void AArenaPickup::SetAvailable(bool bNewAvailable)
{
	bAvailable = bNewAvailable;
	if (!bAvailable)
	{
		RespawnAt = GetWorld()->GetTimeSeconds() + GetRespawnTime();
	}
	ModelRoot->SetVisibility(bAvailable, true);
	Light->SetVisibility(bAvailable);
}

void AArenaPickup::SetNetAvailable(bool bNewAvailable)
{
	bAvailable = bNewAvailable;
	RespawnAt = bNewAvailable ? 0.f : 1.0e12f;
	ModelRoot->SetVisibility(bAvailable, true);
	Light->SetVisibility(bAvailable);
}

float AArenaPickup::GetRespawnTime() const
{
	switch (PickupType)
	{
	// UE2 RespawnTime: TournamentHealth 30, SuperHealthPack 60, ShieldPack 30, SuperShieldPack 60, UDamagePack 90,
	// AdrenalinePickup 30, Ammo 30, WeaponPickup 30
	case EArenaPickupType::HealthVial:   return 30.f;
	case EArenaPickupType::HealthPack:   return 30.f;
	case EArenaPickupType::SuperHealth:  return 60.f;
	case EArenaPickupType::ShieldSmall:  return 30.f;
	case EArenaPickupType::ShieldLarge:  return 60.f;
	case EArenaPickupType::DoubleDamage: return 90.f;
	case EArenaPickupType::Adrenaline:   return 30.f;
	case EArenaPickupType::Ammo:         return 30.f;
	case EArenaPickupType::Weapon:       return 30.f;
	}
	return 20.f;
}

float AArenaPickup::GetRespawnRemaining() const
{
	if (bAvailable)
	{
		return 0.f;
	}
	const UWorld* World = GetWorld();
	return World ? FMath::Max(0.f, RespawnAt - World->GetTimeSeconds()) : 0.f;
}

float AArenaPickup::GetBotDesire(const AArenaCharacter* Bot) const
{
	if (!Bot || !bAvailable || Bot->IsDead())
	{
		return 0.f;
	}

	const float Health = Bot->Health;
	switch (PickupType)
	{
	case EArenaPickupType::HealthVial:
		return Health < ArenaTuning::SuperHealthMax ? 0.12f + 0.3f * (1.f - Health / ArenaTuning::SuperHealthMax) : 0.f;
	case EArenaPickupType::HealthPack:
		return Health < ArenaTuning::MaxHealth ? 0.15f + 1.1f * (1.f - Health / ArenaTuning::MaxHealth) : 0.f;
	case EArenaPickupType::SuperHealth:
		return Health < ArenaTuning::SuperHealthMax ? 0.8f + 0.7f * (1.f - Health / ArenaTuning::SuperHealthMax) : 0.f;
	case EArenaPickupType::ShieldSmall:
		return Bot->Shield < 100.f ? 0.25f + 0.4f * (1.f - Bot->Shield / 100.f) : 0.f;
	case EArenaPickupType::ShieldLarge:
		return Bot->Shield < ArenaTuning::MaxShield ? 0.7f + 0.5f * (1.f - Bot->Shield / ArenaTuning::MaxShield) : 0.f;
	case EArenaPickupType::DoubleDamage:
		return Bot->HasDoubleDamage() ? 0.4f : 1.2f;
	case EArenaPickupType::Adrenaline:
		return Bot->Adrenaline < ArenaTuning::MaxAdrenaline ? 0.1f : 0.f;
	case EArenaPickupType::Weapon:
	{
		const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass);
		if (!CDO)
		{
			return 0.f;
		}
		if (const AArenaWeapon* Owned = Bot->FindWeapon(WeaponClass))
		{
			const float Frac = Owned->GetAmmoFraction();
			return Frac < 0.5f ? 0.25f * (1.f - Frac) : 0.f;
		}
		return 0.45f + 0.6f * FMath::Clamp(CDO->Priority, 0.f, 1.f);
	}
	case EArenaPickupType::Ammo:
	{
		if (const AArenaWeapon* Owned = Bot->FindWeapon(WeaponClass))
		{
			const float Frac = Owned->GetAmmoFraction();
			return Frac < 1.f ? 0.4f * (1.f - Frac) : 0.f;
		}
		return 0.05f;
	}
	}
	return 0.f;
}

FString AArenaPickup::GetDisplayName() const
{
	switch (PickupType)
	{
	case EArenaPickupType::HealthVial:   return TEXT("Health Vial");
	case EArenaPickupType::HealthPack:   return TEXT("Health Pack");
	case EArenaPickupType::SuperHealth:  return TEXT("Super Health");
	case EArenaPickupType::ShieldSmall:  return TEXT("Shield Pack");
	case EArenaPickupType::ShieldLarge:  return TEXT("Heavy Shield");
	case EArenaPickupType::DoubleDamage: return TEXT("Double Damage");
	case EArenaPickupType::Adrenaline:   return TEXT("Adrenaline");
	case EArenaPickupType::Weapon:
		if (const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass))
		{
			return CDO->WeaponName;
		}
		return TEXT("Weapon");
	case EArenaPickupType::Ammo:
		if (const AArenaWeapon* CDO = PickupWeaponCDO(WeaponClass))
		{
			return CDO->WeaponName + TEXT(" Ammo");
		}
		return TEXT("Ammo");
	}
	return TEXT("Item");
}

#include "ArenaAS_Objective.h"
#include "ArenaFX.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArt.h"
#include "ArenaCharacter.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaNavGraph.h"
#include "ArenaTypes.h"
#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

namespace ArenaAS
{
	/** Height window around the objective floor that counts as "in the zone" (character centres). */
	constexpr float ZoneBelow = 150.f;
	constexpr float ZoneAbove = 380.f;
	/** Bots keep "holding" Use as long as they call RequestUse at least this often. */
	constexpr float BotUseHoldWindow = 0.35f;
	/** A single hit never takes more than this fraction of a destroy objective's health (instagib). */
	constexpr float MaxHitFraction = 0.2f;

	static int32 TeamOfController(const AController* C)
	{
		const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		return PS ? PS->Team : ArenaTeam::None;
	}

	static FLinearColor CompletedColor() { return FLinearColor(0.35f, 0.9f, 0.45f); }
	static FLinearColor InactiveColor() { return FLinearColor(0.45f, 0.45f, 0.5f); }
}

AArenaAS_Objective::AArenaAS_Objective()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	ModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ModelRoot"));
	ModelRoot->SetupAttachment(Root);

	// Only destroy objectives collide (weapons, projectiles, explosions, pawns); everything else is visual.
	HitBox = CreateDefaultSubobject<UBoxComponent>(TEXT("HitBox"));
	HitBox->SetupAttachment(Root);
	HitBox->InitBoxExtent(FVector(110.f, 110.f, 150.f));
	HitBox->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
	HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HitBox->SetCollisionObjectType(ECC_WorldDynamic);
	HitBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	HitBox->SetCanEverAffectNavigation(false);
	HitBox->SetGenerateOverlapEvents(false);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Root);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetRelativeLocation(FVector(0.f, 0.f, 180.f));
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 6000.f;
	Light->AttenuationRadius = 600.f;
	Light->CastShadows = false;

	// The nav graph must not treat the objective as a wall (bots walk up to it / stand in it).
	Tags.Add(ArenaNavIgnoreTag());
}

void AArenaAS_Objective::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaAS_Objective, Type);
	DOREPLIFETIME(AArenaAS_Objective, Index);
	DOREPLIFETIME(AArenaAS_Objective, Stage);
	DOREPLIFETIME(AArenaAS_Objective, bOptional);
	DOREPLIFETIME(AArenaAS_Objective, Description);
	DOREPLIFETIME(AArenaAS_Objective, Radius);
	DOREPLIFETIME(AArenaAS_Objective, Duration);
	DOREPLIFETIME(AArenaAS_Objective, MaxHealth);
	DOREPLIFETIME(AArenaAS_Objective, Health);
	DOREPLIFETIME(AArenaAS_Objective, Progress);
	DOREPLIFETIME(AArenaAS_Objective, bCompleted);
	DOREPLIFETIME(AArenaAS_Objective, bActive);
	DOREPLIFETIME(AArenaAS_Objective, bContested);
	DOREPLIFETIME(AArenaAS_Objective, bWorked);
	DOREPLIFETIME(AArenaAS_Objective, AttackingTeam);
	DOREPLIFETIME(AArenaAS_Objective, DefendingTeam);
	DOREPLIFETIME(AArenaAS_Objective, bFromMap);
	DOREPLIFETIME(AArenaAS_Objective, bMapStandIn);
	DOREPLIFETIME(AArenaAS_Objective, MapVehicleMesh);
	DOREPLIFETIME(AArenaAS_Objective, MapVehicleTransform);
	DOREPLIFETIME(AArenaAS_Objective, MapMesh);
	DOREPLIFETIME(AArenaAS_Objective, MapDestroyedMesh);
	DOREPLIFETIME(AArenaAS_Objective, MapMeshTransform);
}

void AArenaAS_Objective::BeginPlay()
{
	Super::BeginPlay();
	if (!bModelBuilt)
	{
		BuildModel();
	}
}

void AArenaAS_Objective::OnRep_Type()
{
	if (HasActorBegunPlay())
	{
		BuildModel();
	}
}

void AArenaAS_Objective::SetMapSource(const FString& MeshObjectPath, const FTransform& MeshTransform, const FString& DestroyedMeshObjectPath, bool bInStandIn)
{
	bFromMap = true;
	bMapStandIn = bInStandIn;
	MapMesh = MeshObjectPath;
	MapMeshTransform = MeshTransform;
	MapDestroyedMesh = DestroyedMeshObjectPath;
}

FString AArenaAS_Objective::MeshObjectPathToRelative(const FString& ObjectPath)
{
	TArray<FString> Parts;
	ObjectPath.ParseIntoArray(Parts, TEXT("."));
	if (Parts.Num() < 2)
	{
		return FString();
	}
	FString Name = Parts[1];
	for (int32 i = 2; i < Parts.Num(); ++i)
	{
		Name += TEXT("_") + Parts[i];
	}
	for (TCHAR& C : Name)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
		{
			C = TEXT('_');
		}
	}
	return FString::Printf(TEXT("StaticMeshes/%s/%s"), *Parts[0], *Name);
}

void AArenaAS_Objective::Setup(EArenaASObjectiveType InType, int32 InIndex, int32 InStage, bool bInOptional, const FString& InDescription,
	float InRadius, float InDuration, float InHealth)
{
	Type = InType;
	Index = InIndex;
	Stage = InStage;
	bOptional = bInOptional;
	Description = InDescription;
	Radius = FMath::Max(60.f, InRadius);
	Duration = FMath::Max(0.5f, InDuration);
	MaxHealth = Type == EArenaASObjectiveType::Destroy ? FMath::Max(50.f, InHealth) : 0.f;
	Health = MaxHealth;

	if (Type == EArenaASObjectiveType::Destroy)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		HitBox->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
		HitBox->SetCollisionResponseToChannel(ECC_Projectile, ECR_Block);
		HitBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		HitBox->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		HitBox->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		// ECC_WorldStatic stays ignored: the bot nav graph and floor traces see through it.
		HitBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
	BuildModel();
	ResetObjective();
}

void AArenaAS_Objective::ResetObjective()
{
	Progress = 0.f;
	Health = MaxHealth;
	bCompleted = false;
	bContested = false;
	bWorked = false;
	bPendingCompletion = false;
	LastHitTime = -100.f;
	Users.Reset();
	Contributors.Reset();
	if (Type == EArenaASObjectiveType::Destroy && HitBox && !bVehicleLinked)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}
}

void AArenaAS_Objective::SetRoles(int32 InAttackingTeam, int32 InDefendingTeam)
{
	AttackingTeam = InAttackingTeam;
	DefendingTeam = InDefendingTeam;
}

void AArenaAS_Objective::SetActive(bool bInActive)
{
	bActive = bInActive;
	if (!bActive)
	{
		bWorked = false;
		bContested = false;
	}
}

void AArenaAS_Objective::SetNetState(bool bInActive, bool bInCompleted, float InHealth, float InMaxHealth, float InProgress, bool bInWorked, int32 InDefendingTeam)
{
	bActive = bInActive;
	bCompleted = bInCompleted;
	if (InMaxHealth > 0.f)
	{
		MaxHealth = InMaxHealth;
		Health = FMath::Clamp(InHealth, 0.f, MaxHealth);
	}
	Progress = bCompleted ? 1.f : FMath::Clamp(InProgress, 0.f, 1.f);
	bWorked = bInWorked && bActive && !bCompleted;
	if (InDefendingTeam == 0 || InDefendingTeam == 1)
	{
		DefendingTeam = InDefendingTeam;
		AttackingTeam = 1 - InDefendingTeam;
	}
	if (HitBox && Type == EArenaASObjectiveType::Destroy && !bVehicleLinked)
	{
		HitBox->SetCollisionEnabled(bCompleted ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
	}
}

void AArenaAS_Objective::SetLinkedVehicle(AActor* InVehicle)
{
	LinkedVehicle = InVehicle;
	bVehicleLinked = true;
	if (MapVehicleComp)
	{
		MapVehicleComp->DestroyComponent();
		MapVehicleComp = nullptr;
	}
	if (HitBox)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

float AArenaAS_Objective::GetWorkFraction() const
{
	if (bCompleted)
	{
		return 1.f;
	}
	return Type == EArenaASObjectiveType::Destroy ? 1.f - GetHealthFraction() : FMath::Clamp(Progress, 0.f, 1.f);
}

FVector AArenaAS_Objective::GetMarkerLocation() const
{
	const float Z = Type == EArenaASObjectiveType::Destroy ? 160.f : Type == EArenaASObjectiveType::Use ? 120.f : 100.f;
	return GetActorLocation() + FVector(0.f, 0.f, Z);
}

FString AArenaAS_Objective::TypeLabel(EArenaASObjectiveType InType)
{
	switch (InType)
	{
	case EArenaASObjectiveType::Touch:   return TEXT("REACH");
	case EArenaASObjectiveType::Hold:    return TEXT("HOLD");
	case EArenaASObjectiveType::Destroy: return TEXT("DESTROY");
	case EArenaASObjectiveType::Use:     return TEXT("USE");
	}
	return FString();
}

bool AArenaAS_Objective::IsInZone(const FVector& Location, float ExtraRadius) const
{
	const FVector Me = GetActorLocation();
	const float DZ = Location.Z - Me.Z;
	return FVector::Dist2D(Location, Me) <= Radius + ExtraRadius && DZ >= -ArenaAS::ZoneBelow && DZ <= ArenaAS::ZoneAbove;
}

bool AArenaAS_Objective::IsInUseRange(const AActor* Pawn) const
{
	return Pawn && IsInZone(Pawn->GetActorLocation(), 40.f);
}

void AArenaAS_Objective::AddContributor(AController* C)
{
	if (C)
	{
		Contributors.AddUnique(C);
	}
}

// ---------------------------------------------------------------------------------------------
// Server logic
// ---------------------------------------------------------------------------------------------

bool AArenaAS_Objective::NotifyUse(AController* User, float Now)
{
	if (!HasAuthority() || !IsActive() || Type != EArenaASObjectiveType::Use || !User)
	{
		return false;
	}
	if (ArenaAS::TeamOfController(User) != AttackingTeam || !IsInUseRange(User->GetPawn()))
	{
		return false;
	}
	Users.Add(User, Now);
	return true;
}

void AArenaAS_Objective::ApplyAttackDamage(float Damage, AController* By)
{
	if (!HasAuthority() || !IsActive() || Type != EArenaASObjectiveType::Destroy || Damage <= 0.f)
	{
		return;
	}
	const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	if (GS && !GS->IsPlaying())
	{
		return;
	}
	if (ArenaAS::TeamOfController(By) != AttackingTeam)
	{
		return;
	}
	Damage = FMath::Min(Damage, MaxHealth * ArenaAS::MaxHitFraction);
	Health = FMath::Max(0.f, Health - Damage);
	LastHitTime = GetWorld()->GetTimeSeconds();
	AddContributor(By);
	if (Health <= 0.f && !bCompleted)
	{
		bCompleted = true;
		bPendingCompletion = true;
		if (HitBox)
		{
			HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
}

float AArenaAS_Objective::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	const float Before = Health;
	ApplyAttackDamage(DamageAmount, EventInstigator);
	return Before - Health;
}

bool AArenaAS_Objective::ServerTick(float DeltaSeconds, float Now)
{
	if (!HasAuthority())
	{
		return false;
	}
	if (bPendingCompletion)
	{
		bPendingCompletion = false;
		bWorked = false;
		return true;
	}
	if (!IsActive())
	{
		return false;
	}

	// Who is around?
	int32 AttackersIn = 0, DefendersIn = 0;
	TArray<AController*, TInlineAllocator<8>> AttackersInZone;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (!C->IsAlive())
		{
			continue;
		}
		const int32 Team = C->GetTeam();
		if (Team != AttackingTeam && Team != DefendingTeam)
		{
			continue;
		}
		if (!IsInZone(C->GetActorLocation()))
		{
			continue;
		}
		if (Team == AttackingTeam)
		{
			++AttackersIn;
			AttackersInZone.Add(C->GetController());
		}
		else
		{
			++DefendersIn;
		}
	}

	switch (Type)
	{
	case EArenaASObjectiveType::Touch:
		bWorked = AttackersIn > 0;
		if (AttackersIn > 0)
		{
			for (AController* C : AttackersInZone)
			{
				AddContributor(C);
			}
			Progress = 1.f;
			bCompleted = true;
			bWorked = false;
			return true;
		}
		break;

	case EArenaASObjectiveType::Hold:
		bContested = AttackersIn > 0 && DefendersIn > 0;
		bWorked = AttackersIn > 0 && DefendersIn == 0;
		if (bWorked)
		{
			// a few attackers together are a little faster
			const float Rate = 1.f + 0.25f * FMath::Min(AttackersIn - 1, 2);
			Progress = FMath::Min(1.f, Progress + DeltaSeconds * Rate / Duration);
			for (AController* C : AttackersInZone)
			{
				AddContributor(C);
			}
		}
		else if (AttackersIn == 0)
		{
			Progress = FMath::Max(0.f, Progress - DeltaSeconds * 0.5f / Duration);
		}
		if (Progress >= 1.f)
		{
			bCompleted = true;
			bWorked = false;
			bContested = false;
			return true;
		}
		break;

	case EArenaASObjectiveType::Use:
	{
		int32 NumUsers = 0;
		for (auto It = Users.CreateIterator(); It; ++It)
		{
			AController* C = It.Key().Get();
			APawn* P = C ? C->GetPawn() : nullptr;
			const AArenaCharacter* AC = Cast<AArenaCharacter>(P);
			bool bHolding = false;
			if (AC && AC->IsAlive() && ArenaAS::TeamOfController(C) == AttackingTeam && IsInUseRange(P))
			{
				if (const APlayerController* PC = Cast<APlayerController>(C))
				{
					// Humans: the key must stay down. Remote clients (no key state on the server) keep using
					// until they leave the console.
					bHolding = !PC->IsLocalController()
						|| PC->IsInputKeyDown(EKeys::E) || PC->IsInputKeyDown(EKeys::Gamepad_FaceButton_Left);
				}
				else
				{
					bHolding = Now - It.Value() <= ArenaAS::BotUseHoldWindow;
				}
			}
			if (!bHolding)
			{
				It.RemoveCurrent();
				continue;
			}
			++NumUsers;
			AddContributor(C);
		}
		bWorked = NumUsers > 0;
		if (bWorked)
		{
			Progress = FMath::Min(1.f, Progress + DeltaSeconds / Duration);
		}
		else
		{
			// letting go loses the progress quickly
			Progress = FMath::Max(0.f, Progress - DeltaSeconds * 2.f / Duration);
		}
		if (Progress >= 1.f)
		{
			bCompleted = true;
			bWorked = false;
			Users.Reset();
			return true;
		}
		break;
	}

	case EArenaASObjectiveType::Destroy:
		bWorked = Now - LastHitTime < 1.f;
		break;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaAS_Objective::AddPart(UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, UMaterialInterface* Material, const FRotator& Rotation)
{
	if (!Mesh || !ModelRoot)
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
	Part->SetCanEverAffectNavigation(false);
	Part->SetCastShadow(false);
	if (Material)
	{
		Part->SetMaterial(0, Material);
	}
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void AArenaAS_Objective::BuildModel()
{
	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	Parts.Reset();
	StateGlows.Reset();
	Beacon = nullptr;
	ProgressPart = nullptr;
	ProgressGlow = nullptr;
	CorePart = nullptr;
	bModelBuilt = true;
	LastVisualState = -1;

	if (MapMeshComp)
	{
		MapMeshComp->DestroyComponent();
		MapMeshComp = nullptr;
	}
	MapMeshShown = -1;
	if (bFromMap)
	{
		// UT2004 objectives have no model of their own beyond the map's: no light, no beacon.
		Light->SetVisibility(false);
		MapMeshAsset = MapMesh.IsEmpty() ? nullptr : ArenaArt::Load<UStaticMesh>(MeshObjectPathToRelative(MapMesh));
		MapDestroyedAsset = MapDestroyedMesh.IsEmpty() ? nullptr : ArenaArt::Load<UStaticMesh>(MeshObjectPathToRelative(MapDestroyedMesh));
		if (MapMeshAsset)
		{
			MapMeshComp = NewObject<UStaticMeshComponent>(this, TEXT("MapMesh"));
			MapMeshComp->SetMobility(EComponentMobility::Movable);
			MapMeshComp->SetStaticMesh(MapMeshAsset);
			MapMeshComp->SetupAttachment(RootComponent);
			MapMeshComp->SetUsingAbsoluteLocation(true);
			MapMeshComp->SetUsingAbsoluteRotation(true);
			MapMeshComp->SetUsingAbsoluteScale(true);
			MapMeshComp->SetWorldTransform(MapMeshTransform);
			MapMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			MapMeshComp->SetCanEverAffectNavigation(false);
			MapMeshComp->RegisterComponent();
			if (Type == EArenaASObjectiveType::Destroy)
			{
				// Damage box on the mesh's bounds (oriented with it)
				const FBoxSphereBounds B = MapMeshAsset->GetBounds();
				const FVector Scale = MapMeshTransform.GetScale3D().GetAbs();
				HitBox->SetUsingAbsoluteLocation(true);
				HitBox->SetUsingAbsoluteRotation(true);
				HitBox->SetWorldLocationAndRotation(MapMeshTransform.TransformPosition(B.Origin), MapMeshTransform.GetRotation());
				HitBox->SetBoxExtent((B.BoxExtent * Scale).ComponentMax(FVector(20.f)));
			}
		}
		else if (!MapMesh.IsEmpty())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("AS: objective mesh %s not found"), *MapMesh);
		}
		if (MapVehicleComp)
		{
			MapVehicleComp->DestroyComponent();
			MapVehicleComp = nullptr;
		}
		if (USkeletalMesh* VehicleMesh = MapVehicleMesh.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(MapVehicleMesh))
		{
			// a DestroyVehicleObjective's vehicle: the vehicle class's own model where its factory is
			MapVehicleComp = NewObject<USkeletalMeshComponent>(this, TEXT("MapVehicle"));
			MapVehicleComp->SetSkeletalMeshAsset(VehicleMesh);
			MapVehicleComp->SetupAttachment(RootComponent);
			MapVehicleComp->SetUsingAbsoluteLocation(true);
			MapVehicleComp->SetUsingAbsoluteRotation(true);
			MapVehicleComp->SetUsingAbsoluteScale(true);
			MapVehicleComp->SetWorldTransform(MapVehicleTransform);
			MapVehicleComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			MapVehicleComp->SetCanEverAffectNavigation(false);
			MapVehicleComp->RegisterComponent();
			bMapVehicleGone = false;
			if (Type == EArenaASObjectiveType::Destroy)
			{
				const FBoxSphereBounds B = VehicleMesh->GetBounds();
				HitBox->SetUsingAbsoluteLocation(true);
				HitBox->SetUsingAbsoluteRotation(true);
				HitBox->SetWorldLocationAndRotation(MapVehicleTransform.TransformPosition(B.Origin), MapVehicleTransform.GetRotation());
				HitBox->SetBoxExtent((B.BoxExtent * MapVehicleTransform.GetScale3D().GetAbs()).ComponentMax(FVector(30.f)));
			}
			UpdateVisuals(0.f);
			return;
		}
		else if (!MapVehicleMesh.IsEmpty())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("AS: objective vehicle mesh %s not found"), *MapVehicleMesh);
		}
		if (MapMeshComp || !bMapStandIn)
		{
			UpdateVisuals(0.f);
			return;
		}
		// the map's thing is not recreated here: the procedural model below stands in for it
		Light->SetVisibility(true);
	}

	const FLinearColor Col = ArenaAS::InactiveColor();
	auto Glow = [this, &Col](float Intensity, float Opacity)
	{
		UMaterialInstanceDynamic* M = ArenaAssets::MakeGlow(this, Col, Intensity, Opacity);
		if (M)
		{
			StateGlows.Add(M);
		}
		return M;
	};
	UMaterialInterface* Metal = ArenaAssets::MakeSurface(this, FLinearColor(0.16f, 0.17f, 0.19f), 0.35f, 0.8f);
	UMaterialInterface* Plate = ArenaAssets::MakeSurface(this, FLinearColor(0.32f, 0.33f, 0.35f), 0.5f, 0.4f);
	const float D = Radius * 2.f;

	switch (Type)
	{
	case EArenaASObjectiveType::Touch:
	{
		// a marker pad with a light shaft and a hovering arrow
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 3.f), FVector(D, D, 6.f), Plate);
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 2.f), FVector(D + 30.f, D + 30.f, 3.f), Glow(5.f, 0.8f));
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 7.f), FVector(D * 0.45f, D * 0.45f, 2.f), Glow(3.f, 0.5f));
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 170.f), FVector(22.f, 22.f, 320.f), Glow(2.f, 0.25f));
		Beacon = AddPart(ArenaAssets::Cone(), FVector(0.f, 0.f, 230.f), FVector(70.f, 70.f, 90.f), Glow(7.f, 0.9f), FRotator(180.f, 0.f, 0.f));
		Light->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
		Light->AttenuationRadius = 500.f;
		break;
	}
	case EArenaASObjectiveType::Hold:
	{
		// a translucent zone with posts around the rim and a pylon in the middle
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 2.f), FVector(D, D, 2.f), Glow(1.5f, 0.18f));
		const int32 NumPosts = 10;
		for (int32 i = 0; i < NumPosts; ++i)
		{
			const float A = 2.f * PI * i / NumPosts;
			const FVector P(FMath::Cos(A) * Radius, FMath::Sin(A) * Radius, 30.f);
			AddPart(ArenaAssets::Cube(), P, FVector(16.f, 16.f, 60.f), Glow(6.f, 0.9f));
		}
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 70.f), FVector(50.f, 50.f, 140.f), Metal);
		AddPart(ArenaAssets::Sphere(), FVector(0.f, 0.f, 160.f), FVector(60.f, 60.f, 60.f), Glow(8.f, 0.9f));
		Beacon = AddPart(ArenaAssets::Cube(), FVector(0.f, 0.f, 240.f), FVector(40.f, 40.f, 40.f), Glow(7.f, 0.8f), FRotator(45.f, 0.f, 45.f));
		ProgressBaseScale = FVector(D / 100.f, D / 100.f, 3.f / 100.f);
		ProgressGlow = ArenaAssets::MakeGlow(this, ArenaTeam::Color(AttackingTeam), 4.f, 0.45f);
		ProgressPart = AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 5.f), FVector(D, D, 3.f), ProgressGlow);
		Light->SetRelativeLocation(FVector(0.f, 0.f, 200.f));
		Light->AttenuationRadius = FMath::Max(600.f, Radius * 1.6f);
		break;
	}
	case EArenaASObjectiveType::Use:
	{
		// a console on a pad: pedestal, slanted screen, use ring on the floor
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 2.f), FVector(D, D, 2.f), Glow(1.2f, 0.15f));
		AddPart(ArenaAssets::Cube(), FVector(0.f, 0.f, 55.f), FVector(70.f, 90.f, 110.f), Metal);
		AddPart(ArenaAssets::Cube(), FVector(0.f, 0.f, 118.f), FVector(80.f, 100.f, 16.f), Plate, FRotator(0.f, 0.f, 0.f));
		AddPart(ArenaAssets::Cube(), FVector(0.f, 0.f, 140.f), FVector(60.f, 84.f, 6.f), Glow(8.f, 0.95f), FRotator(-20.f, 0.f, 0.f));
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 4.f), FVector(150.f, 150.f, 3.f), Glow(4.f, 0.7f));
		Beacon = AddPart(ArenaAssets::Cone(), FVector(0.f, 0.f, 240.f), FVector(50.f, 50.f, 60.f), Glow(7.f, 0.9f), FRotator(180.f, 0.f, 0.f));
		ProgressBaseScale = FVector(1.4f, 1.4f, 0.03f);
		ProgressGlow = ArenaAssets::MakeGlow(this, ArenaTeam::Color(AttackingTeam), 5.f, 0.6f);
		ProgressPart = AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 6.f), FVector(140.f, 140.f, 3.f), ProgressGlow);
		Light->SetRelativeLocation(FVector(0.f, 0.f, 170.f));
		Light->AttenuationRadius = 450.f;
		break;
	}
	case EArenaASObjectiveType::Destroy:
	{
		// a caged power core: the glowing core shrinks with the objective's health
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 15.f), FVector(230.f, 230.f, 30.f), Metal);
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 3.f), FVector(270.f, 270.f, 4.f), Glow(5.f, 0.7f));
		for (int32 i = 0; i < 4; ++i)
		{
			const float A = PI * 0.25f + PI * 0.5f * i;
			AddPart(ArenaAssets::Cube(), FVector(FMath::Cos(A) * 95.f, FMath::Sin(A) * 95.f, 150.f), FVector(22.f, 22.f, 260.f), Plate);
		}
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 285.f), FVector(200.f, 200.f, 24.f), Metal);
		AddPart(ArenaAssets::Cylinder(), FVector(0.f, 0.f, 300.f), FVector(120.f, 120.f, 6.f), Glow(6.f, 0.9f));
		CoreBaseLocation = FVector(0.f, 0.f, 30.f);
		CoreBaseScale = FVector(0.9f, 0.9f, 2.4f);
		CorePart = AddPart(ArenaAssets::Cylinder(), CoreBaseLocation + FVector(0.f, 0.f, 120.f), FVector(90.f, 90.f, 240.f), Glow(9.f, 0.95f));
		Beacon = AddPart(ArenaAssets::Sphere(), FVector(0.f, 0.f, 360.f), FVector(40.f, 40.f, 40.f), Glow(7.f, 0.9f));
		Light->SetRelativeLocation(FVector(0.f, 0.f, 200.f));
		Light->AttenuationRadius = 700.f;
		break;
	}
	}
	UpdateVisuals(0.f);
}

FLinearColor AArenaAS_Objective::GetStateColor() const
{
	if (bCompleted)
	{
		return ArenaAS::CompletedColor();
	}
	if (!bActive)
	{
		return ArenaAS::InactiveColor();
	}
	return ArenaTeam::Color(DefendingTeam);
}

void AArenaAS_Objective::UpdateVisuals(float DeltaSeconds)
{
	VisualTime += DeltaSeconds;
	// The map's objective mesh: replaced by its destroyed mesh (or gone) once destroyed, back on a new round.
	if (MapMeshComp && Type == EArenaASObjectiveType::Destroy)
	{
		const int32 Shown = bCompleted ? 1 : 0;
		if (Shown != MapMeshShown)
		{
			MapMeshShown = Shown;
			MapMeshComp->SetStaticMesh(bCompleted && MapDestroyedAsset ? MapDestroyedAsset.Get() : MapMeshAsset.Get());
			MapMeshComp->SetVisibility(!bCompleted || MapDestroyedAsset != nullptr);
		}
	}
	// a DestroyVehicleObjective's vehicle: blown up when destroyed, back on a new round
	if (MapVehicleComp && bMapVehicleGone != bCompleted)
	{
		bMapVehicleGone = bCompleted;
		MapVehicleComp->SetVisibility(!bCompleted);
		if (bCompleted && DeltaSeconds > 0.f)
		{
			ArenaFX::SpawnExplosion(GetWorld(), MapVehicleComp->Bounds.Origin, FLinearColor(1.f, 0.5f, 0.15f), 450.f, TEXT("Explosion.Rocket"));
		}
	}
	const int32 State = bCompleted ? 2 : bActive ? 1 : 0;
	const int32 Key = State * 16 + (DefendingTeam + 1) * 4 + (AttackingTeam + 1);
	if (Key != LastVisualState)
	{
		LastVisualState = Key;
		const FLinearColor C = GetStateColor();
		for (UMaterialInstanceDynamic* M : StateGlows)
		{
			if (M)
			{
				M->SetVectorParameterValue(TEXT("Color"), C);
			}
		}
		if (ProgressGlow)
		{
			ProgressGlow->SetVectorParameterValue(TEXT("Color"), ArenaTeam::Color(AttackingTeam));
		}
		Light->SetLightColor(C);
		Light->SetIntensity(State == 1 ? 7000.f : State == 2 ? 2500.f : 1200.f);
		if (Beacon)
		{
			Beacon->SetVisibility(State == 1);
		}
	}

	if (Beacon && State == 1)
	{
		const FVector L = Beacon->GetRelativeLocation();
		const float BaseZ = Type == EArenaASObjectiveType::Destroy ? 360.f : Type == EArenaASObjectiveType::Use ? 240.f : Type == EArenaASObjectiveType::Hold ? 240.f : 230.f;
		Beacon->SetRelativeLocation(FVector(L.X, L.Y, BaseZ + 18.f * FMath::Sin(VisualTime * 2.2f)));
		FRotator R = Beacon->GetRelativeRotation();
		R.Yaw = FMath::Fmod(VisualTime * 90.f, 360.f);
		Beacon->SetRelativeRotation(R);
	}
	if (ProgressPart)
	{
		const float P = bCompleted ? 1.f : FMath::Clamp(Progress, 0.f, 1.f);
		ProgressPart->SetVisibility(P > 0.01f);
		ProgressPart->SetRelativeScale3D(FVector(ProgressBaseScale.X * P, ProgressBaseScale.Y * P, ProgressBaseScale.Z));
	}
	if (CorePart)
	{
		const float H = bCompleted ? 0.f : GetHealthFraction();
		CorePart->SetVisibility(H > 0.01f);
		CorePart->SetRelativeScale3D(FVector(CoreBaseScale.X, CoreBaseScale.Y, FMath::Max(0.01f, CoreBaseScale.Z * H)));
		CorePart->SetRelativeLocation(CoreBaseLocation + FVector(0.f, 0.f, CoreBaseScale.Z * 100.f * H * 0.5f));
	}
	if (State == 1 && bWorked)
	{
		Light->SetIntensity(7000.f + 4000.f * FMath::Abs(FMath::Sin(VisualTime * 8.f)));
	}
}

void AArenaAS_Objective::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateVisuals(DeltaSeconds);
}

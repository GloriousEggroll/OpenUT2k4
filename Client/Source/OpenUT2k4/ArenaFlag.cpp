#include "ArenaFlag.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArt.h"
#include "ArenaArtItems.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaTypes.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	constexpr float FlagAutoReturnTime = 20.f;
	/** Flag model placement relative to the holder's BodyRoot (which sits at the feet). */
	const FVector FlagCarryOffset(-32.f, 0.f, 70.f);
	const FRotator FlagCarryRotation(0.f, 90.f, 8.f);
	constexpr float FlagCarryScale = 0.85f;
}

AArenaFlag::AArenaFlag()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	BaseModel = CreateDefaultSubobject<USceneComponent>(TEXT("BaseModel"));
	BaseModel->SetupAttachment(Root);

	FlagModel = CreateDefaultSubobject<USceneComponent>(TEXT("FlagModel"));
	FlagModel->SetupAttachment(Root);

	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	Trigger->SetupAttachment(FlagModel);
	Trigger->InitSphereRadius(90.f);
	Trigger->SetRelativeLocation(FVector(0.f, 0.f, 90.f));
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetCanEverAffectNavigation(false);

	FlagLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("FlagLight"));
	FlagLight->SetupAttachment(FlagModel);
	FlagLight->SetMobility(EComponentMobility::Movable);
	FlagLight->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
	FlagLight->IntensityUnits = ELightUnits::Unitless;
	FlagLight->Intensity = 8000.f;
	FlagLight->AttenuationRadius = 500.f;
	FlagLight->CastShadows = false;

	BaseLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("BaseLight"));
	BaseLight->SetupAttachment(BaseModel);
	BaseLight->SetMobility(EComponentMobility::Movable);
	BaseLight->SetRelativeLocation(FVector(0.f, 0.f, 40.f));
	BaseLight->IntensityUnits = ELightUnits::Unitless;
	BaseLight->Intensity = 6000.f;
	BaseLight->AttenuationRadius = 400.f;
	BaseLight->CastShadows = false;
}

void AArenaFlag::SetTeam(int32 InTeam)
{
	Team = (InTeam == ArenaTeam::Blue) ? ArenaTeam::Blue : ArenaTeam::Red;
	if (bModelBuilt)
	{
		BuildModel();
	}
}

void AArenaFlag::BeginPlay()
{
	Super::BeginPlay();
	HomeLocation = GetActorLocation();
	BuildModel();
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AArenaFlag::OnOverlap);
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

UStaticMeshComponent* AArenaFlag::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Location, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh || !Parent)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
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
	Parts.Add(Part);
	return Part;
}

void AArenaFlag::BuildModel()
{
	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	Parts.Reset();
	bModelBuilt = true;

	const FLinearColor TeamCol = ArenaTeam::Color(Team);
	const int32 TeamIndex = Team == ArenaTeam::Blue ? 1 : 0;
	const ArenaArtItems::FFlagArt& Art = ArenaArtItems::GetFlagArt();
	if (FlagSymbol)
	{
		FlagSymbol->DestroyComponent();
		FlagSymbol = nullptr;
	}
	if (FlagSkel)
	{
		FlagSkel->DestroyComponent();
		FlagSkel = nullptr;
	}

	// Imported flag base (team texture), else the procedural plate.
	bool bArtBase = false;
	if (UStaticMeshComponent* Base = ArenaArtItems::AddItemMesh(this, BaseModel, Art.Base, 1.f, true))
	{
		if (UMaterialInterface* Mat = ArenaArtItems::LoadMaterial(Art.BaseMaterial[TeamIndex]))
		{
			Base->SetMaterial(0, Mat);
		}
		Parts.Add(Base);
		bArtBase = true;
	}
	if (!bArtBase)
	{
		AddPart(BaseModel, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 4.f), FVector(220.f, 220.f, 8.f),
			ArenaAssets::MakeSurface(this, FLinearColor(0.25f, 0.24f, 0.23f), 0.4f, 0.6f), true);
		AddPart(BaseModel, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 3.f), FVector(260.f, 260.f, 4.f),
			ArenaAssets::MakeGlow(this, TeamCol, 7.f), false);
		AddPart(BaseModel, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 9.f), FVector(150.f, 150.f, 2.f),
			ArenaAssets::MakeGlow(this, TeamCol, 4.f, 0.6f), false);
	}

	// Imported waving flag, else the procedural pole and cloth.
	if (USkeletalMesh* Mesh = ArenaArt::Load<USkeletalMesh>(Art.Mesh))
	{
		FlagSkel = NewObject<USkeletalMeshComponent>(this, TEXT("FlagSkel"));
		FlagSkel->SetSkeletalMeshAsset(Mesh);
		FlagSkel->SetupAttachment(FlagModel);
		FlagSkel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FlagSkel->SetGenerateOverlapEvents(false);
		FlagSkel->SetCanEverAffectNavigation(false);
		FlagSkel->bComponentUseFixedSkelBounds = true;
		UMaterialInterface* Mat = ArenaArtItems::LoadMaterial(Art.TeamMaterial[TeamIndex]);
		if (!Mat)
		{
			Mat = ArenaArtItems::LoadMaterial(Art.TeamMaterialFallback[TeamIndex]);
		}
		if (Mat)
		{
			for (int32 i = 0; i < FMath::Max(1, Mesh->GetMaterials().Num()); ++i)
			{
				FlagSkel->SetMaterial(i, Mat);
			}
		}
		// Pole at the mesh origin, cloth along +X: turn the cloth to +Y like the procedural flag (carry pose),
		// stand the pole bottom on the floor.
		const FBoxSphereBounds B = Mesh->GetImportedBounds();
		const float S = Art.Height / FMath::Max(1.f, float(B.BoxExtent.Z) * 2.f);
		FlagSkel->SetRelativeScale3D(FVector(S));
		FlagSkel->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
		FlagSkel->SetRelativeLocation(FVector(0.f, 0.f, -(B.Origin.Z - B.BoxExtent.Z) * S));
		FlagSkel->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		FlagSkel->RegisterComponent();
		if (UAnimSequence* Anim = ArenaArt::Load<UAnimSequence>(Art.Anim))
		{
			FlagSkel->PlayAnimation(Anim, true);
		}
		AddFlagSymbol(Mesh, TeamIndex);
	}
	else
	{
		AddPart(FlagModel, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 90.f), FVector(6.f, 6.f, 180.f),
			ArenaAssets::MakeSurface(this, FLinearColor(0.75f, 0.75f, 0.78f), 0.3f, 0.9f), true);
		AddPart(FlagModel, ArenaAssets::Sphere(), FVector(0.f, 0.f, 183.f), FVector(12.f),
			ArenaAssets::MakeGlow(this, TeamCol, 10.f), false);
		AddPart(FlagModel, ArenaAssets::Cube(), FVector(0.f, 38.f, 152.f), FVector(3.f, 72.f, 46.f),
			ArenaAssets::MakeSurface(this, TeamCol, 0.8f, 0.f, TeamCol * 0.35f), true);
		AddPart(FlagModel, ArenaAssets::Cube(), FVector(0.f, 38.f, 152.f), FVector(4.f, 30.f, 12.f),
			ArenaAssets::MakeGlow(this, FLinearColor::White, 4.f), false);
	}

	FlagLight->SetLightColor(TeamCol);
	BaseLight->SetLightColor(TeamCol);
}

void AArenaFlag::AddFlagSymbol(USkeletalMesh* Mesh, int32 TeamIndex)
{
	// UT2004 adds the team symbol to the cloth at runtime (CTFFlag.UpdateForTeam sets the TexScaler inside the flag
	// shader's diffuse Combiner), so neither the imported nor the runtime flag material carries it. Draw it as a second
	// layer: the same mesh following FlagSkel's pose, with the symbol through the shipped lit translucent master
	// (white symbol over the team cloth, like the Combiner's add).
	const ArenaArtItems::FFlagArt& Art = ArenaArtItems::GetFlagArt();
	// The match's symbol (picked by the game mode, replicated), else the table's default.
	const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	const FString MatchSymbol = GS ? GS->GetTeamSymbol(TeamIndex) : FString();
	UTexture2D* Symbol = MatchSymbol.IsEmpty() ? nullptr : ArenaArt::Load<UTexture2D>(MatchSymbol);
	if (!Symbol && Art.Symbol[TeamIndex])
	{
		Symbol = ArenaArt::Load<UTexture2D>(Art.Symbol[TeamIndex]);
	}
	AppliedSymbol = MatchSymbol;
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/UT2004/M_ImportedTranslucent_2S.M_ImportedTranslucent_2S"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Symbol || !Master || !FlagSkel)
	{
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("Flag %d: no team symbol layer (symbol %s, master %s)"), TeamIndex, Symbol ? TEXT("ok") : TEXT("missing"), Master ? TEXT("ok") : TEXT("missing"));
		return;
	}
	// Team symbols are clamped textures in UT2004 (outside the symbol the TexScaler shows nothing, not a repeat).
	if (Symbol->AddressX != TA_Clamp || Symbol->AddressY != TA_Clamp)
	{
		Symbol->AddressX = TA_Clamp;
		Symbol->AddressY = TA_Clamp;
		Symbol->UpdateResource();
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Master, this);
	MID->SetTextureParameterValue(TEXT("Base"), Symbol);
	MID->SetVectorParameterValue(TEXT("UVTransform"), FLinearColor(Art.SymbolScale, 0.f, 0.f, Art.SymbolScale));
	MID->SetVectorParameterValue(TEXT("UVOffsetPan"), FLinearColor(Art.SymbolOffset.X, Art.SymbolOffset.Y, 0.f, 0.f));

	FlagSymbol = NewObject<USkeletalMeshComponent>(this, TEXT("FlagSymbol"));
	FlagSymbol->SetSkeletalMeshAsset(Mesh);
	FlagSymbol->SetupAttachment(FlagSkel);
	FlagSymbol->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FlagSymbol->SetGenerateOverlapEvents(false);
	FlagSymbol->SetCanEverAffectNavigation(false);
	FlagSymbol->SetCastShadow(false);
	FlagSymbol->bComponentUseFixedSkelBounds = true;
	for (int32 i = 0; i < FMath::Max(1, Mesh->GetMaterials().Num()); ++i)
	{
		FlagSymbol->SetMaterial(i, MID);
	}
	FlagSymbol->RegisterComponent();
	FlagSymbol->SetLeaderPoseComponent(FlagSkel);
}

void AArenaFlag::RefreshTeamSymbol()
{
	const AArenaGameState* GS = GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	const int32 TeamIndex = Team == ArenaTeam::Blue ? 1 : 0;
	if (!FlagSkel || !GS || GS->GetTeamSymbol(TeamIndex) == AppliedSymbol)
	{
		return;
	}
	if (FlagSymbol)
	{
		FlagSymbol->DestroyComponent();
		FlagSymbol = nullptr;
	}
	if (USkeletalMesh* Mesh = FlagSkel->GetSkeletalMeshAsset())
	{
		AddFlagSymbol(Mesh, TeamIndex);
	}
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

AArenaGameMode* AArenaFlag::GetArenaGameMode() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
}

bool AArenaFlag::IsMatchPlaying() const
{
	const UWorld* World = GetWorld();
	const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
	return GS && GS->IsPlaying();
}

FVector AArenaFlag::GetFlagLocation() const
{
	if (State == EArenaFlagState::Carried && IsValid(Holder))
	{
		return Holder->GetActorLocation();
	}
	return FlagModel ? FlagModel->GetComponentLocation() : GetActorLocation();
}

void AArenaFlag::PlaceFlagModel(const FVector& WorldLocation, float Yaw)
{
	FlagModel->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	FlagModel->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepWorldTransform);
	FlagModel->SetWorldLocationAndRotation(WorldLocation, FRotator(0.f, Yaw, 0.f));
	FlagModel->SetWorldScale3D(FVector::OneVector);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	bCheckOverlaps = true;
}

void AArenaFlag::SendHome()
{
	if (Holder)
	{
		if (Holder->CarriedFlag == this)
		{
			Holder->CarriedFlag = nullptr;
		}
		Holder = nullptr;
	}
	State = EArenaFlagState::Home;
	PlaceFlagModel(HomeLocation, GetActorRotation().Yaw);
}

// ---------------------------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------------------------

void AArenaFlag::Drop(AController* By)
{
	if (State != EArenaFlagState::Carried)
	{
		return;
	}

	AController* HolderController = IsValid(Holder) ? Holder->GetController() : nullptr;
	const FVector From = IsValid(Holder) ? Holder->GetActorLocation() : FlagModel->GetComponentLocation();
	const float Yaw = IsValid(Holder) ? Holder->GetActorRotation().Yaw : 0.f;

	if (IsValid(Holder) && Holder->CarriedFlag == this)
	{
		Holder->CarriedFlag = nullptr;
	}
	AActor* OldHolder = Holder;
	Holder = nullptr;

	// Find the floor below the drop point (level geometry only).
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaFlagDrop), false, this);
	if (OldHolder)
	{
		Query.AddIgnoredActor(OldHolder);
	}
	const FCollisionObjectQueryParams Objects(ECC_WorldStatic);
	const bool bFloor = GetWorld()->LineTraceSingleByObjectType(Hit, From, From - FVector(0.f, 0.f, 20000.f), Objects, Query);

	AArenaGameMode* GM = GetArenaGameMode();
	State = EArenaFlagState::Dropped;
	DroppedTime = GetWorld()->GetTimeSeconds();
	if (GM)
	{
		GM->FlagDropped(this, By ? By : HolderController);
	}

	if (!bFloor)
	{
		// Fell into a bottomless area: return immediately (counts as an automatic return).
		SendHome();
		if (GM)
		{
			GM->FlagReturned(this, nullptr);
		}
		return;
	}
	PlaceFlagModel(Hit.ImpactPoint, Yaw);
}

void AArenaFlag::ReturnHome(AController* By)
{
	if (State == EArenaFlagState::Home)
	{
		return;
	}
	SendHome();
	if (AArenaGameMode* GM = GetArenaGameMode())
	{
		GM->FlagReturned(this, By);
	}
}

void AArenaFlag::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	TryTouch(Cast<AArenaCharacter>(OtherActor));
}

void AArenaFlag::TryTouch(AArenaCharacter* Character)
{
	if (!Character || !Character->IsAlive() || !HasAuthority() || !IsMatchPlaying())
	{
		return;
	}
	if (State == EArenaFlagState::Carried)
	{
		return;
	}

	const int32 CharTeam = Character->GetTeam();
	if (CharTeam != ArenaTeam::Red && CharTeam != ArenaTeam::Blue)
	{
		return;
	}
	AController* Controller = Character->GetController();
	AArenaGameMode* GM = GetArenaGameMode();

	if (CharTeam == Team)
	{
		if (State == EArenaFlagState::Dropped)
		{
			ReturnHome(Controller);
		}
		else if (State == EArenaFlagState::Home)
		{
			AArenaFlag* EnemyFlag = Character->CarriedFlag;
			if (EnemyFlag && EnemyFlag != this && EnemyFlag->GetTeam() != Team)
			{
				if (GM)
				{
					GM->FlagCaptured(EnemyFlag, Controller);
				}
				EnemyFlag->SendHome();
				Character->CarriedFlag = nullptr;
			}
		}
		return;
	}

	// enemy: take the flag (from home or where it was dropped)
	if (Character->CarriedFlag)
	{
		return;
	}
	Holder = Character;
	Character->CarriedFlag = this;
	State = EArenaFlagState::Carried;
	bCheckOverlaps = false;
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	USceneComponent* AttachParent = Character->BodyRoot ? Character->BodyRoot.Get() : Character->GetRootComponent();
	FlagModel->AttachToComponent(AttachParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	FlagModel->SetRelativeLocationAndRotation(FlagCarryOffset, FlagCarryRotation);
	FlagModel->SetRelativeScale3D(FVector(FlagCarryScale));

	if (GM)
	{
		GM->FlagTaken(this, Controller);
	}
}

void AArenaFlag::SetNetState(EArenaFlagState NewState, AArenaCharacter* NewHolder, const FVector& Location)
{
	bNetDriven = true;
	bCheckOverlaps = false;
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (Holder && Holder != NewHolder && Holder->CarriedFlag == this)
	{
		Holder->CarriedFlag = nullptr;
	}
	if (NewState == EArenaFlagState::Carried && IsValid(NewHolder))
	{
		if (Holder == NewHolder && State == EArenaFlagState::Carried)
		{
			return;
		}
		Holder = NewHolder;
		NewHolder->CarriedFlag = this;
		State = EArenaFlagState::Carried;
		USceneComponent* AttachParent = NewHolder->BodyRoot ? NewHolder->BodyRoot.Get() : NewHolder->GetRootComponent();
		FlagModel->AttachToComponent(AttachParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		FlagModel->SetRelativeLocationAndRotation(FlagCarryOffset, FlagCarryRotation);
		FlagModel->SetRelativeScale3D(FVector(FlagCarryScale));
		return;
	}
	Holder = nullptr;
	if (NewState == EArenaFlagState::Dropped)
	{
		State = EArenaFlagState::Dropped;
		DroppedTime = GetWorld()->GetTimeSeconds();
		PlaceFlagModel(Location, FlagModel->GetComponentRotation().Yaw);
	}
	else
	{
		State = EArenaFlagState::Home;
		PlaceFlagModel(HomeLocation, GetActorRotation().Yaw);
	}
	Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bCheckOverlaps = false;
}

void AArenaFlag::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bNetDriven)
	{
		return;
	}

	if (State == EArenaFlagState::Carried)
	{
		if (!IsValid(Holder) || Holder->IsDead())
		{
			Drop(nullptr);
		}
	}
	else if (State == EArenaFlagState::Dropped)
	{
		if (GetWorld()->GetTimeSeconds() - DroppedTime >= FlagAutoReturnTime)
		{
			ReturnHome(nullptr);
		}
	}

	if (bCheckOverlaps && State != EArenaFlagState::Carried)
	{
		bCheckOverlaps = false;
		TArray<AActor*> Overlapping;
		Trigger->GetOverlappingActors(Overlapping, AArenaCharacter::StaticClass());
		for (AActor* Actor : Overlapping)
		{
			TryTouch(Cast<AArenaCharacter>(Actor));
			if (State == EArenaFlagState::Carried)
			{
				break;
			}
		}
	}
}

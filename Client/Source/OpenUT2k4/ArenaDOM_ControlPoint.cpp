#include "ArenaDOM_ControlPoint.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArtItems.h"
#include "ArenaCharacter.h"
#include "ArenaTypes.h"
#include "ArenaNavGraph.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

namespace ArenaDOM
{
	static const FLinearColor NeutralColor(0.85f, 0.85f, 0.8f);
	static const FLinearColor LockedColor(0.22f, 0.22f, 0.24f);
	constexpr float EmblemHeight = 270.f;
}

AArenaDOM_ControlPoint::AArenaDOM_ControlPoint()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetActorEnableCollision(false);
	Tags.Add(ArenaNavIgnoreTag());

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Emblem = CreateDefaultSubobject<USceneComponent>(TEXT("Emblem"));
	Emblem->SetupAttachment(Root);
	Emblem->SetRelativeLocation(FVector(0.f, 0.f, ArenaDOM::EmblemHeight));

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Emblem);
	Light->SetMobility(EComponentMobility::Movable);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 9000.f;
	Light->AttenuationRadius = 900.f;
	Light->CastShadows = false;

	LabelText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	LabelText->SetupAttachment(Root);
	LabelText->SetRelativeLocation(FVector(0.f, 0.f, 430.f));
	LabelText->SetHorizontalAlignment(EHTA_Center);
	LabelText->SetVerticalAlignment(EVRTA_TextCenter);
	LabelText->SetWorldSize(110.f);
	LabelText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LabelText->SetCastShadow(false);
}

void AArenaDOM_ControlPoint::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaDOM_ControlPoint, Label);
	DOREPLIFETIME(AArenaDOM_ControlPoint, OwnerTeam);
	DOREPLIFETIME(AArenaDOM_ControlPoint, bEnabled);
	DOREPLIFETIME(AArenaDOM_ControlPoint, EnableTime);
	DOREPLIFETIME(AArenaDOM_ControlPoint, ScoreTime);
}

void AArenaDOM_ControlPoint::BeginPlay()
{
	Super::BeginPlay();
	BuildVisuals();
	bBuilt = true;
	UpdateVisuals();
}

UStaticMeshComponent* AArenaDOM_ControlPoint::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& RelLocation, const FRotator& RelRotation, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
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

void AArenaDOM_ControlPoint::BuildVisuals()
{
	using namespace ArenaDOM;
	USceneComponent* Root = GetRootComponent();
	UMaterialInterface* Frame = ArenaAssets::MakeSurface(this, FLinearColor(0.1f, 0.1f, 0.12f), 0.45f, 0.7f);
	GlowMat = ArenaAssets::MakeGlow(this, NeutralColor, 4.f, 0.9f);
	SurfaceMat = ArenaAssets::MakeSurface(this, NeutralColor, 0.3f, 0.5f, NeutralColor * 0.3f);
	BeamMat = ArenaAssets::MakeGlow(this, NeutralColor, 1.5f, 0.18f);

	// Imported pad, letter and ring; the sky beam stays as a locator.
	{
		const ArenaArtItems::FDominationArt& Art = ArenaArtItems::GetDominationArt();
		if (UStaticMeshComponent* Pad = ArenaArtItems::AddItemMesh(this, Root, Art.Pad, 1.f, true))
		{
			Parts.Add(Pad);
			ArtLetter = ArenaArtItems::AddItemMesh(this, Emblem, Art.Letter[Label == TEXT("B") ? 1 : 0], 1.f, true);
			ArtRing = ArenaArtItems::AddItemMesh(this, Emblem, Art.Ring);
			AddPart(Root, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 900.f), FRotator::ZeroRotator, FVector(26.f, 26.f, 1400.f), BeamMat, false);
			LabelText->SetText(FText::FromString(Label));
			return;
		}
	}

	// Floor pad: dark plate, glowing rim, four low pylons
	AddPart(Root, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 6.f), FRotator::ZeroRotator, FVector(TouchRadius * 2.f, TouchRadius * 2.f, 12.f), Frame, true);
	AddPart(Root, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 12.5f), FRotator::ZeroRotator, FVector(TouchRadius * 2.f + 14.f, TouchRadius * 2.f + 14.f, 2.f), GlowMat, false);
	AddPart(Root, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 13.f), FRotator::ZeroRotator, FVector(TouchRadius * 1.4f, TouchRadius * 1.4f, 2.f), Frame, false);
	for (int32 i = 0; i < 4; ++i)
	{
		const float A = HALF_PI * i + PI * 0.25f;
		const FVector P(FMath::Cos(A) * (TouchRadius + 10.f), FMath::Sin(A) * (TouchRadius + 10.f), 40.f);
		AddPart(Root, ArenaAssets::Cube(), P, FRotator(0.f, FMath::RadiansToDegrees(A), 0.f), FVector(18.f, 18.f, 70.f), Frame, true);
		AddPart(Root, ArenaAssets::Cube(), P + FVector(0.f, 0.f, 37.f), FRotator(0.f, FMath::RadiansToDegrees(A), 0.f), FVector(20.f, 20.f, 6.f), GlowMat, false);
	}
	// Floating emblem: a tilted cube core in a glow shell, plus a faint sky beam
	AddPart(Emblem, ArenaAssets::Cube(), FVector::ZeroVector, FRotator(45.f, 0.f, 35.26f), FVector(60.f), SurfaceMat, true);
	AddPart(Emblem, ArenaAssets::Sphere(), FVector::ZeroVector, FRotator::ZeroRotator, FVector(130.f), GlowMat, false);
	AddPart(Root, ArenaAssets::Cylinder(), FVector(0.f, 0.f, 900.f), FRotator::ZeroRotator, FVector(26.f, 26.f, 1400.f), BeamMat, false);

	LabelText->SetText(FText::FromString(Label));
}

void AArenaDOM_ControlPoint::UpdateVisuals()
{
	using namespace ArenaDOM;
	if (!bBuilt)
	{
		return;
	}
	const FLinearColor Col = !bEnabled ? LockedColor : ((OwnerTeam == 0 || OwnerTeam == 1) ? ArenaTeam::Color(OwnerTeam) : NeutralColor);
	if (GlowMat)
	{
		GlowMat->SetVectorParameterValue(TEXT("Color"), Col);
	}
	if (SurfaceMat)
	{
		SurfaceMat->SetVectorParameterValue(TEXT("Color"), Col);
		SurfaceMat->SetVectorParameterValue(TEXT("Emissive"), Col * 0.4f);
	}
	if (BeamMat)
	{
		BeamMat->SetVectorParameterValue(TEXT("Color"), Col);
	}
	{
		const ArenaArtItems::FDominationArt& Art = ArenaArtItems::GetDominationArt();
		const int32 Idx = !bEnabled ? 2 : (OwnerTeam == 0 || OwnerTeam == 1 ? OwnerTeam : 2);
		if (ArtLetter)
		{
			if (UMaterialInterface* M = ArenaArtItems::LoadMaterial(Art.LetterMaterial[Idx]))
			{
				ArtLetter->SetMaterial(0, M);
			}
		}
		if (ArtRing)
		{
			if (UMaterialInterface* M = ArenaArtItems::LoadMaterial(Art.RingMaterial[Idx]))
			{
				ArtRing->SetMaterial(0, M);
			}
		}
	}
	Light->SetLightColor(Col);
	Light->SetIntensity(bEnabled ? 9000.f : 1500.f);
	LabelText->SetText(FText::FromString(Label));
	LabelText->SetTextRenderColor((bEnabled ? (Col * 0.8f + FLinearColor(0.25f, 0.25f, 0.25f)).GetClamped() : FLinearColor(0.5f, 0.5f, 0.5f)).ToFColor(true));
}

void AArenaDOM_ControlPoint::OnRep_State()
{
	UpdateVisuals();
}

void AArenaDOM_ControlPoint::SetOwnerTeam(int32 Team, AController* By)
{
	OwnerTeam = (Team == 0 || Team == 1) ? Team : -1;
	LastToucher = By;
	UpdateVisuals();
}

void AArenaDOM_ControlPoint::SetDisabled(float UntilServerTime)
{
	bEnabled = false;
	OwnerTeam = -1;
	EnableTime = UntilServerTime;
	ScoreTime = 0.f;
	UpdateVisuals();
}

void AArenaDOM_ControlPoint::SetEnabled()
{
	bEnabled = true;
	EnableTime = 0.f;
	UpdateVisuals();
}

void AArenaDOM_ControlPoint::GetTouching(AArenaCharacter* OutFirst[2]) const
{
	OutFirst[0] = nullptr;
	OutFirst[1] = nullptr;
	const FVector Floor = GetActorLocation();
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		const int32 Team = C->GetTeam();
		if (!C->IsAlive() || (Team != 0 && Team != 1) || OutFirst[Team])
		{
			continue;
		}
		const FVector L = C->GetActorLocation();
		const float Z = L.Z - Floor.Z;
		if (FVector::Dist2D(L, Floor) < TouchRadius + ArenaTuning::CapsuleRadius && Z > -40.f && Z < 320.f)
		{
			OutFirst[Team] = C;
		}
	}
}

void AArenaDOM_ControlPoint::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetWorld()->GetTimeSeconds();
	if (Emblem)
	{
		const float Bob = FMath::Sin(Now * 1.6f) * 12.f;
		Emblem->SetRelativeLocationAndRotation(FVector(0.f, 0.f, ArenaDOM::EmblemHeight + Bob), FRotator(0.f, Now * (bEnabled ? 70.f : 15.f), 0.f));
	}
	if (GlowMat)
	{
		const float Pulse = bEnabled ? (OwnerTeam >= 0 ? 4.f + FMath::Sin(Now * 3.f) : 3.f) : 1.f + 0.8f * FMath::Abs(FMath::Sin(Now * 4.f));
		GlowMat->SetScalarParameterValue(TEXT("Intensity"), Pulse);
	}
	// Turn the label to the local viewer
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		FVector ViewLoc;
		FRotator ViewRot;
		PC->GetPlayerViewPoint(ViewLoc, ViewRot);
		const FVector To = ViewLoc - LabelText->GetComponentLocation();
		LabelText->SetWorldRotation(FRotator(0.f, To.Rotation().Yaw, 0.f));
	}
}

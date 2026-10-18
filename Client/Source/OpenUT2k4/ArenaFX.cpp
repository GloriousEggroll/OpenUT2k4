#include "ArenaFX.h"
#include "ArenaAssets.h"
#include "ArenaArtFX.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Scene.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInstanceDynamic.h"

AArenaFXActor::AArenaFXActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	SetCanBeDamaged(false);
	bReplicates = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetReceivesDecals(false);
	Mesh->SetCanEverAffectNavigation(false);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Root);
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->SetCastShadows(false);
	Light->bAffectTranslucentLighting = false;
	Light->SetVisibility(false);
	Light->Intensity = 0.f;

	SetActorEnableCollision(false);
}

void AArenaFXActor::InitFX(UStaticMesh* InMesh, const FLinearColor& InColor, float InLifeTime, const FVector& InStartScale, const FVector& InEndScale,
	float InGlowIntensity, float InLightIntensity, float InLightRadius, bool bOnlyOwnerSee)
{
	LifeTime = FMath::Max(0.01f, InLifeTime);
	Age = 0.f;
	StartScale = InStartScale;
	EndScale = InEndScale;
	GlowIntensity = InGlowIntensity;
	LightIntensity = InLightIntensity;

	if (InMesh)
	{
		Mesh->SetStaticMesh(InMesh);
		MID = ArenaAssets::MakeGlow(this, InColor, GlowIntensity, 1.f);
		if (MID)
		{
			Mesh->SetMaterial(0, MID);
		}
		Mesh->SetOnlyOwnerSee(bOnlyOwnerSee);
		Mesh->SetVisibility(true);
	}
	else
	{
		Mesh->SetVisibility(false);
	}

	if (LightIntensity > 0.f)
	{
		Light->SetLightColor(InColor);
		Light->SetAttenuationRadius(FMath::Max(10.f, InLightRadius));
		Light->SetIntensity(LightIntensity);
		Light->SetVisibility(true);
	}
	else
	{
		Light->SetVisibility(false);
	}

	SetLifeSpan(LifeTime + 0.5f); // safety net in case ticking is paused
	ApplyAlpha(0.f);
}

void AArenaFXActor::ApplyAlpha(float Alpha)
{
	const float A = FMath::Clamp(Alpha, 0.f, 1.f);
	const float Fade = FMath::Pow(1.f - A, FMath::Max(0.1f, FadePower));

	if (Mesh && Mesh->GetStaticMesh())
	{
		// Ease-out growth looks punchier for explosions
		const float Grow = 1.f - FMath::Square(1.f - A);
		Mesh->SetRelativeScale3D(FMath::Lerp(StartScale, EndScale, Grow).ComponentMax(FVector(0.0001f)));
		if (MID)
		{
			MID->SetScalarParameterValue(TEXT("Opacity"), Fade);
			MID->SetScalarParameterValue(TEXT("Intensity"), GlowIntensity * Fade);
		}
	}
	if (Light && LightIntensity > 0.f)
	{
		Light->SetIntensity(LightIntensity * Fade * Fade);
	}
}

void AArenaFXActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	if (Age >= LifeTime)
	{
		Destroy();
		return;
	}
	ApplyAlpha(Age / LifeTime);
}

namespace
{
	AArenaFXActor* SpawnFXActor(UWorld* World, const FVector& Location, const FRotator& Rotation, AActor* Owner = nullptr)
	{
		if (!World || World->bIsTearingDown)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = Owner;
		Params.ObjectFlags |= RF_Transient;
		return World->SpawnActor<AArenaFXActor>(AArenaFXActor::StaticClass(), Location, Rotation, Params);
	}

	FLinearColor Brighten(const FLinearColor& C, float Towards)
	{
		return FMath::Lerp(C, FLinearColor::White, Towards);
	}
}

void ArenaFX::OrientBeamMesh(UStaticMeshComponent* BeamMesh, const FVector& Start, const FVector& End, float Width)
{
	if (!BeamMesh)
	{
		return;
	}
	const FVector Delta = End - Start;
	const float Len = Delta.Size();
	if (Len < 1.f)
	{
		BeamMesh->SetVisibility(false);
		return;
	}
	const FVector Dir = Delta / Len;
	const FRotator Rot = FRotationMatrix::MakeFromZ(Dir).Rotator();
	const float W = FMath::Max(0.1f, Width) / 100.f;
	BeamMesh->SetWorldLocationAndRotation((Start + End) * 0.5f, Rot);
	BeamMesh->SetWorldScale3D(FVector(W, W, Len / 100.f));
}

namespace
{
	float ArtScale(FName Style, float Radius)
	{
		const ArenaArtFX::FEffectDesc* E = ArenaArtFX::FindEffect(Style);
		return (E && E->RefRadius > 0.f) ? FMath::Clamp(Radius / E->RefRadius, 0.2f, 4.f) : 1.f;
	}
}

AArenaFXActor* ArenaFX::SpawnBeam(UWorld* World, const FVector& Start, const FVector& End, const FLinearColor& Color, float Width, float LifeTime, FName Style)
{
	if (!Style.IsNone())
	{
		if (AArenaFXActor* Art = ArenaArtFX::SpawnBeamEffect(World, Style, Start, End, 1.f))
		{
			return Art;
		}
	}
	const FVector Delta = End - Start;
	const float Len = Delta.Size();
	if (Len < 1.f)
	{
		return nullptr;
	}
	const FRotator Rot = FRotationMatrix::MakeFromZ(Delta / Len).Rotator();
	AArenaFXActor* FX = SpawnFXActor(World, (Start + End) * 0.5f, Rot);
	if (!FX)
	{
		return nullptr;
	}
	const float W = FMath::Max(0.2f, Width) / 100.f;
	const float L = Len / 100.f;
	FX->FadePower = 1.5f;
	FX->InitFX(ArenaAssets::Cylinder(), Color, LifeTime, FVector(W, W, L), FVector(W * 0.25f, W * 0.25f, L), 10.f, 0.f, 0.f);

	// Thin bright inner core for wider beams
	if (Width >= 3.f)
	{
		if (AArenaFXActor* Core = SpawnFXActor(World, (Start + End) * 0.5f, Rot))
		{
			const float CW = W * 0.35f;
			Core->FadePower = 0.7f;
			Core->InitFX(ArenaAssets::Cylinder(), Brighten(Color, 0.7f), LifeTime * 0.6f, FVector(CW, CW, L), FVector(CW * 0.2f, CW * 0.2f, L), 16.f, 0.f, 0.f);
		}
	}
	return FX;
}

AArenaFXActor* ArenaFX::SpawnImpact(UWorld* World, const FVector& Location, const FVector& Normal, const FLinearColor& Color, float Radius, FName Style)
{
	if (!Style.IsNone())
	{
		const FVector N = Normal.IsNearlyZero() ? FVector::UpVector : Normal.GetSafeNormal();
		if (AArenaFXActor* Art = ArenaArtFX::SpawnEffect(World, Style, Location + N * 3.f, N.Rotation(), ArtScale(Style, Radius)))
		{
			return Art;
		}
	}
	const FVector N = Normal.GetSafeNormal();
	const FVector Loc = Location + N * FMath::Min(Radius * 0.2f, 6.f);
	AArenaFXActor* FX = SpawnFXActor(World, Loc, N.IsNearlyZero() ? FRotator::ZeroRotator : N.Rotation());
	if (!FX)
	{
		return nullptr;
	}
	const float S = FMath::Max(1.f, Radius) / 50.f; // sphere mesh is 100 cm diameter
	FX->FadePower = 1.2f;
	FX->InitFX(ArenaAssets::Sphere(), Brighten(Color, 0.25f), 0.22f, FVector(S * 0.25f), FVector(S), 10.f, 2500.f + Radius * 60.f, Radius * 8.f);
	return FX;
}

AArenaFXActor* ArenaFX::SpawnExplosion(UWorld* World, const FVector& Location, const FLinearColor& Color, float Radius, FName Style, const FVector& Normal)
{
	if (!Style.IsNone())
	{
		const FVector N = Normal.IsNearlyZero() ? FVector::UpVector : Normal.GetSafeNormal();
		if (AArenaFXActor* Art = ArenaArtFX::SpawnEffect(World, Style, Location + N * 10.f, N.Rotation(), ArtScale(Style, Radius)))
		{
			return Art;
		}
	}
	const float R = FMath::Max(20.f, Radius);
	const float S = R / 50.f;
	AArenaFXActor* FX = SpawnFXActor(World, Location, FRotator::ZeroRotator);
	if (!FX)
	{
		return nullptr;
	}
	FX->FadePower = 1.3f;
	FX->InitFX(ArenaAssets::Sphere(), Color, 0.5f, FVector(S * 0.1f), FVector(S * 0.75f), 8.f, 20000.f + R * 120.f, R * 3.f);

	// White-hot core
	if (AArenaFXActor* Core = SpawnFXActor(World, Location, FRotator::ZeroRotator))
	{
		Core->FadePower = 0.8f;
		Core->InitFX(ArenaAssets::Sphere(), Brighten(Color, 0.8f), 0.25f, FVector(S * 0.05f), FVector(S * 0.4f), 18.f, 0.f, 0.f);
	}
	// Flattened shock ring
	if (AArenaFXActor* Ring = SpawnFXActor(World, Location, FRotator::ZeroRotator))
	{
		Ring->FadePower = 1.f;
		Ring->InitFX(ArenaAssets::Sphere(), Brighten(Color, 0.4f), 0.35f, FVector(S * 0.2f, S * 0.2f, S * 0.02f), FVector(S * 1.1f, S * 1.1f, S * 0.04f), 6.f, 0.f, 0.f);
	}
	return FX;
}

AArenaFXActor* ArenaFX::SpawnMuzzleFlash(USceneComponent* AttachTo, const FLinearColor& Color, const FVector& RelativeOffset, float Scale, bool bOnlyOwnerSee, FName Style)
{
	if (!AttachTo)
	{
		return nullptr;
	}
	if (!Style.IsNone())
	{
		if (AArenaFXActor* Art = ArenaArtFX::SpawnAttachedEffect(AttachTo, Style, RelativeOffset, Scale, bOnlyOwnerSee, false, false))
		{
			return Art;
		}
	}
	UWorld* World = AttachTo->GetWorld();
	const FTransform T = AttachTo->GetComponentTransform();
	AArenaFXActor* FX = SpawnFXActor(World, T.TransformPosition(RelativeOffset), T.Rotator(), AttachTo->GetOwner());
	if (!FX)
	{
		return nullptr;
	}
	FX->AttachToComponent(AttachTo, FAttachmentTransformRules::KeepWorldTransform);
	FX->SetActorRelativeLocation(RelativeOffset);
	FX->SetActorRelativeRotation(FRotator::ZeroRotator);
	const float S = 0.12f * FMath::Max(0.1f, Scale);
	FX->FadePower = 1.f;
	// Elongated along the barrel (X)
	FX->InitFX(ArenaAssets::Sphere(), Brighten(Color, 0.4f), 0.07f, FVector(S * 1.6f, S, S), FVector(S * 2.4f, S * 1.4f, S * 1.4f), 14.f, 3000.f * Scale, 350.f * Scale, bOnlyOwnerSee);
	return FX;
}

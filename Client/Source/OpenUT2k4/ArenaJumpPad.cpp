#include "ArenaJumpPad.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaArtFX.h"
#include "ArenaBotController.h"
#include "ArenaCharacter.h"
#include "ArenaTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	TAutoConsoleVariable<int32> CVarUE2JumpVelocity(TEXT("arena.JumpPadUE2Velocity"), 1,
		TEXT("1: imported jump pads launch with the map's UE2 JumpVelocity; 0: a computed arc to the target."));
	const FLinearColor JumpPadGlowColor(0.15f, 1.f, 0.65f);
	constexpr float JumpPadLaunchCooldown = 0.5f;

	void SetupJumpPadVisualPart(UStaticMeshComponent* Comp)
	{
		Comp->SetMobility(EComponentMobility::Movable);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCollisionResponseToAllChannels(ECR_Ignore);
		Comp->SetGenerateOverlapEvents(false);
		Comp->SetCanEverAffectNavigation(false);
	}
}

AArenaJumpPad::AArenaJumpPad()
{
	PrimaryActorTick.bCanEverTick = true;
	// every frame after movement: the pawns' paths since the last frame are tested against the cylinder
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	PadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PadMesh"));
	PadMesh->SetupAttachment(Root);
	SetupJumpPadVisualPart(PadMesh);
	PadMesh->SetRelativeLocation(FVector(0.f, 0.f, 6.f));
	PadMesh->SetRelativeScale3D(FVector(2.2f, 2.2f, 0.12f));

	GlowMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("GlowMesh"));
	GlowMesh->SetupAttachment(Root);
	SetupJumpPadVisualPart(GlowMesh);
	GlowMesh->SetCastShadow(false);
	GlowMesh->SetRelativeLocation(FVector(0.f, 0.f, 7.f));
	GlowMesh->SetRelativeScale3D(FVector(1.7f, 1.7f, 0.13f));

	ArrowMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LiftBeam"));
	ArrowMesh->SetupAttachment(Root);
	SetupJumpPadVisualPart(ArrowMesh);
	ArrowMesh->SetCastShadow(false);
	ArrowMesh->SetRelativeLocation(FVector(0.f, 0.f, 75.f));
	ArrowMesh->SetRelativeScale3D(FVector(1.3f, 1.3f, 1.3f));

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Root);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
	Light->IntensityUnits = ELightUnits::Unitless;
	Light->Intensity = 6000.f;
	Light->AttenuationRadius = 450.f;
	Light->CastShadows = false;
	Light->LightColor = JumpPadGlowColor.ToFColor(true);
}

void AArenaJumpPad::SetUE2Collision(const FVector& Center, float Radius, float HalfHeight)
{
	bHasUE2Center = true;
	UE2Center = Center;
	TriggerRadius = FMath::Max(1.f, Radius);
	TriggerHalfHeight = FMath::Max(1.f, HalfHeight);
}

FVector AArenaJumpPad::GetTriggerCenter() const
{
	return bHasUE2Center ? UE2Center : GetActorLocation() + FVector(0.f, 0.f, TriggerHalfHeight);
}

bool AArenaJumpPad::Touches(const FVector& Location, float Radius, float HalfHeight) const
{
	const FVector D = Location - GetTriggerCenter();
	return FVector2D(D.X, D.Y).SizeSquared() <= FMath::Square(TriggerRadius + Radius) && FMath::Abs(D.Z) <= TriggerHalfHeight + HalfHeight;
}

void AArenaJumpPad::SetTarget(const FVector& InTarget, float InApexHeight)
{
	Target = InTarget;
	ApexHeight = FMath::Max(InApexHeight, 50.f);
}

void AArenaJumpPad::BeginPlay()
{
	Super::BeginPlay();

	PadMesh->SetStaticMesh(ArenaAssets::Cylinder());
	PadMesh->SetMaterial(0, ArenaAssets::MakeSurface(this, FLinearColor(0.12f, 0.14f, 0.16f), 0.35f, 0.8f));
	GlowMesh->SetStaticMesh(ArenaAssets::Cylinder());
	GlowMesh->SetMaterial(0, ArenaAssets::MakeGlow(this, JumpPadGlowColor, 8.f));
	ArrowMesh->SetStaticMesh(ArenaAssets::Cylinder());
	ArrowMesh->SetMaterial(0, ArenaAssets::MakeGlow(this, JumpPadGlowColor, 1.5f, 0.2f));

	// A pad hanging in the air (BR-Colossus tube mouths: the UE2 pad is an invisible cylinder in the tube) has no
	// plate to draw: the floor below is somewhere else.
	if (bHasUE2Center && UE2Center.Z - GetActorLocation().Z > TriggerHalfHeight + 60.f)
	{
		GlowMesh->SetVisibility(false);
		PadMesh->SetVisibility(false);
		ArrowMesh->SetVisibility(false);
		Light->SetVisibility(false);
		return;
	}

	// Imported jump pad textures: a turning target disc and rings closing in on it, instead of the glow disc.
	{
		ArenaArtFX::FSpriteDesc Disc;
		Disc.Texture = TEXT("Textures/EpicParticles/JumpPad_JumpCircle01aw");
		Disc.Facing = ArenaArtFX::EFacing::Fixed;
		Disc.Size = FVector2D(200.f, 200.f);
		Disc.Spin = 40.f;
		Disc.Tint = JumpPadGlowColor;
		Disc.Intensity = 2.f;
		Disc.Offset = FVector(0.f, 0.f, 14.f);
		UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
		C->SetupAttachment(GetRootComponent());
		if (C->SetupSprite(Disc))
		{
			C->RegisterComponent();
			// The UT2004 maps model their jump pads (the pad actor itself is invisible): with the art, the procedural
			// plate and lift column are not drawn either.
			GlowMesh->SetVisibility(false);
			PadMesh->SetVisibility(false);
			ArrowMesh->SetVisibility(false);
			for (int32 i = 0; i < 2; ++i)
			{
				ArenaArtFX::FSpriteDesc Ring;
				Ring.Texture = TEXT("Textures/XEffectMat/Link_grey_ring");
				Ring.Facing = ArenaArtFX::EFacing::Fixed;
				Ring.Size = FVector2D(230.f, 230.f);
				Ring.EndScale = 0.25f;
				Ring.LifeTime = 1.2f;
				Ring.Delay = 0.6f * i;
				Ring.FadeIn = 0.3f;
				Ring.bLoop = true;
				Ring.Tint = JumpPadGlowColor;
				Ring.Intensity = 1.5f;
				Ring.Offset = FVector(0.f, 0.f, 16.f + 2.f * i);
				UArenaSpriteComponent* R = NewObject<UArenaSpriteComponent>(this);
				R->SetupAttachment(GetRootComponent());
				if (R->SetupSprite(Ring))
				{
					R->RegisterComponent();
				}
				else
				{
					R->DestroyComponent();
				}
			}
		}
		else
		{
			C->DestroyComponent();
		}
	}
}

FVector AArenaJumpPad::ComputeLaunchVelocity(const FVector& From) const
{
	const UWorld* World = GetWorld();
	const float GravityZ = (World ? World->GetGravityZ() : -980.f) * ArenaTuning::GravityScale;
	return ComputeLaunchVelocityWithGravity(From, GravityZ);
}

FVector AArenaJumpPad::ComputeLaunchVelocityWithGravity(const FVector& From, float GravityZ) const
{
	const float G = FMath::Max(1.f, -GravityZ);
	const float ApexZ = FMath::Max(From.Z, Target.Z) + ApexHeight;
	const float Up = FMath::Max(1.f, ApexZ - From.Z);
	const float Down = FMath::Max(0.f, ApexZ - Target.Z);

	const float Vz = FMath::Sqrt(2.f * G * Up);
	const float FlightTime = Vz / G + FMath::Sqrt(2.f * Down / G);

	FVector Horizontal = Target - From;
	Horizontal.Z = 0.f;
	const FVector Vxy = FlightTime > KINDA_SMALL_NUMBER ? Horizontal / FlightTime : FVector::ZeroVector;
	return FVector(Vxy.X, Vxy.Y, Vz);
}

bool AArenaJumpPad::TryLaunch(AArenaCharacter* Character)
{
	if (!Character || !Character->IsAlive() || !HasAuthority())
	{
		return false;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const TWeakObjectPtr<AActor> Key(Character);
	if (const float* Last = LastLaunch.Find(Key))
	{
		if (Now - *Last < JumpPadLaunchCooldown)
		{
			return false;
		}
	}

	const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	const float GravityZ = Move ? Move->GetGravityZ() : GetWorld()->GetGravityZ() * ArenaTuning::GravityScale;
	const FVector From = Character->GetActorLocation();
	const bool bUE2 = !UE2JumpVelocity.IsNearlyZero() && CVarUE2JumpVelocity.GetValueOnGameThread() != 0;
	const FVector Velocity = bUE2 ? UE2JumpVelocity : ComputeLaunchVelocityWithGravity(From, GravityZ);
	const FVector Before = Character->GetVelocity();
	Character->LaunchCharacter(Velocity, true, true);
	LastLaunch.Add(Key, Now);
	if (!Cast<AArenaBotController>(Character->GetController()) || !UE2Name.IsEmpty())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("JumpPad: %s touched %s at %s (speed %.0f) -> launched %s (%s)"), *Character->GetName(),
			UE2Name.IsEmpty() ? *GetName() : *UE2Name, *From.ToCompactString(), Before.Size(), *Velocity.ToCompactString(),
			bUE2 ? TEXT("UE2 JumpVelocity") : TEXT("arc to target"));
	}
	if (AArenaBotController* Bot = Cast<AArenaBotController>(Character->GetController()))
	{
		// one line per bot launch: the planned ones (a JumpPad link of the path) show that bots path through pads
		const bool bPlanned = Bot->IsFollowingPadLink();
		Bot->NumPadLaunches += bPlanned ? 1 : 0;
		Bot->NotifyJumpPadLaunch(Target);
		UE_LOG(LogOpenUT2k4, Log, TEXT("JumpPad: bot %s launched at %s towards %s (%s)"), *Bot->GetName(),
			*GetActorLocation().ToCompactString(), *Target.ToCompactString(), bPlanned ? TEXT("path link") : TEXT("not on a pad link"));
	}
	ArenaAssets::PlaySoundAt(this, TEXT("S_JumpPad"), GetActorLocation());
	return true;
}

void AArenaJumpPad::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}

	// UE2 Touch: a pawn's cylinder starts overlapping the pad's cylinder. The pawn's path since the last frame is
	// sampled so a fast pawn (tube rides: 100+ m/s) can not pass through between two frames.
	for (TActorIterator<AArenaCharacter> It(World); It; ++It)
	{
		AArenaCharacter* Character = *It;
		// a UT2004 server's pawns (bNetProxy) are launched by the server; only our own one is predicted here
		if (!Character->IsAlive() || (Character->bNetProxy && !Character->IsLocallyControlled()))
		{
			Touchers.Remove(Character);
			continue;
		}
		const float R = Character->GetSimpleCollisionRadius();
		const float H = Character->GetSimpleCollisionHalfHeight();
		const FVector Loc = Character->GetActorLocation();
		FToucher* T = Touchers.Find(Character);
		const bool bNew = !T;
		if (bNew)
		{
			T = &Touchers.Add(Character);
			T->LastLocation = Loc;
		}
		const bool bInside = Touches(Loc, R, H);
		bool bTouched = bInside;
		const FVector Path = Loc - T->LastLocation;
		// a teleport (respawn, teleporter) is no path: UE2 touches nothing on the way
		const bool bTeleported = Path.Size() > (Character->GetVelocity().Size() + 3000.f) * DeltaSeconds * 2.f + 50.f;
		if (!bTouched && !bNew && !bTeleported)
		{
			const float Step = 0.5f * FMath::Max(10.f, FMath::Min3(TriggerRadius, TriggerHalfHeight, R));
			const int32 Steps = FMath::Clamp(FMath::CeilToInt(float(Path.Size()) / Step), 1, 256);
			for (int32 i = 1; i < Steps && !bTouched; ++i)
			{
				bTouched = Touches(T->LastLocation + Path * (float(i) / float(Steps)), R, H);
			}
		}
		const bool bEntered = bTouched && !T->bTouching;
		T->LastLocation = Loc;
		T->bTouching = bInside;
		const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
		// entering, or standing on the pad again after the cooldown (landed back on it)
		if (bEntered || (bInside && Move && !Move->IsFalling()))
		{
			if (TryLaunch(Character))
			{
				T->bTouching = true;
			}
		}
	}

	// forget stale entries
	for (auto It = Touchers.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	if (LastLaunch.Num() > 0)
	{
		const float Now = World->GetTimeSeconds();
		for (auto It = LastLaunch.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid() || Now - It.Value() > 5.f)
			{
				It.RemoveCurrent();
			}
		}
	}
}

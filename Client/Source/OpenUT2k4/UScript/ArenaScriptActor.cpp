#include "ArenaScriptActor.h"
#include "ArenaUScript.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

void AArenaScriptActor::SetupScript(FName InTag, FName InEvent, float InRadius, float InHeight, bool bInCollide, bool bShootable)
{
	Tag = InTag;
	Event = InEvent;
	CollisionRadius = InRadius;
	CollisionHeight = InHeight;
	bCollideActors = bInCollide;
	bWantsTouch = bInCollide && InRadius > 0.f;
	if (bShootable && InRadius > 0.f && !ShootCollision)
	{
		ShootCollision = NewObject<UCapsuleComponent>(this, TEXT("ShootCollision"));
		ShootCollision->InitCapsuleSize(InRadius, FMath::Max(InHeight, InRadius));
		ShootCollision->SetupAttachment(RootComponent);
		ShootCollision->SetCollisionProfileName(TEXT("NoCollision"));
		ShootCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		ShootCollision->SetCollisionObjectType(ECC_WorldDynamic);
		ShootCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
		ShootCollision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Block); // weapon traces
		ShootCollision->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block); // projectiles
		ShootCollision->RegisterComponent();
	}
}

void AArenaScriptActor::Trigger(AActor* Other, APawn* EventInstigator)
{
	ArenaUScriptHooks::ScriptActorEvent(this, Other, EventInstigator, 0);
}

void AArenaScriptActor::UnTrigger(AActor* Other, APawn* EventInstigator)
{
	ArenaUScriptHooks::ScriptActorEvent(this, Other, EventInstigator, 1);
}

void AArenaScriptActor::Touch(APawn* Other)
{
	ArenaUScriptHooks::ScriptActorEvent(this, Other, Other, 2);
}

void AArenaScriptActor::UnTouch(APawn* Other)
{
	ArenaUScriptHooks::ScriptActorEvent(this, Other, Other, 3);
}

float AArenaScriptActor::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	ArenaUScriptHooks::ScriptActorDamage(this, DamageAmount, EventInstigator, DamageCauser);
	return DamageAmount;
}

void AArenaScriptActor::SetMesh(UStaticMesh* Mesh, const FVector& Scale3D, const FRotator& Rotation)
{
	if (!Mesh)
	{
		return;
	}
	if (!MeshComponent)
	{
		MeshComponent = NewObject<UStaticMeshComponent>(this, TEXT("ScriptMesh"));
		MeshComponent->SetupAttachment(RootComponent);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->RegisterComponent();
	}
	MeshComponent->SetStaticMesh(Mesh);
	MeshComponent->SetRelativeScale3D(Scale3D);
	MeshComponent->SetWorldRotation(Rotation);
	SetHidden(false);
}

void AArenaScriptActor::SetScriptHidden(bool bHide)
{
	if (MeshComponent)
	{
		MeshComponent->SetVisibility(!bHide);
	}
}

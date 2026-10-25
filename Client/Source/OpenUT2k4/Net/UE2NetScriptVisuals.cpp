#include "UE2NetScriptVisuals.h"

#include "../ArenaArt.h"
#include "../ArenaEmitters.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaUE2Effects.h"
#include "../ArenaUE2World.h"
#include "../UScript/UScriptBridge.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptPresentation.h"
#include "../UScript/UScriptRuntime.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UE2NetLog.h"
#include "UE2NetModContent.h"
#include "UE2NetModWeapons.h"

namespace UE2NetScriptVisualsLocal
{
	// EDrawType
	constexpr uint8 DT_Mesh = 2;
	constexpr uint8 DT_StaticMesh = 8;

	FUSProperty* Prop(FUSObject* O, const TCHAR* Name)
	{
		return O && O->Data ? O->FindProperty(FName(Name)) : nullptr;
	}

	template <typename T>
	T Get(FUSObject* O, const TCHAR* Name, const T& Default = T())
	{
		FUSProperty* P = Prop(O, Name);
		return P && P->ElementSize >= int32(sizeof(T)) ? *reinterpret_cast<const T*>(O->Data + P->Offset) : Default;
	}

	FString ObjPath(FUSObject* O, const TCHAR* Name)
	{
		FUSProperty* P = Prop(O, Name);
		if (!P || (P->Type != EUSType::Object && P->Type != EUSType::Class))
		{
			return FString();
		}
		const FUSObject* V = *reinterpret_cast<FUSObject* const*>(O->Data + P->Offset);
		return V ? V->GetPathName() : FString();
	}

	TArray<FString> ObjPaths(FUSObject* O, const TCHAR* Name)
	{
		TArray<FString> Out;
		FUSProperty* P = Prop(O, Name);
		if (!P || P->Type != EUSType::Array || !P->Inner || P->Inner->Type != EUSType::Object)
		{
			return Out;
		}
		const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(O->Data + P->Offset);
		for (int32 i = 0; i < A.Num && A.Data; ++i)
		{
			const FUSObject* V = *reinterpret_cast<FUSObject* const*>(A.Data + i * P->Inner->ElementSize);
			Out.Add(V ? V->GetPathName() : FString());
		}
		return Out;
	}

	bool IsEmitterClass(const FUSObject* O)
	{
		return O && (O->IsA(FName(TEXT("Emitter"))) || O->IsA(FName(TEXT("xEmitter"))));
	}

	/** An xEmitter class of mesh particles (ArenaUE2Effects::SpawnMeshXEmitter draws its bursts). */
	bool IsMeshXEmitter(const FString& ClassPath)
	{
		FArenaUE2ActorDesc D;
		if (!ArenaUE2Effects::GetClassDesc(ClassPath, D))
		{
			return false;
		}
		const FString Type = D.Props.Str(TEXT("mParticleType"));
		return Type.Contains(TEXT("Mesh")) || int32(D.Props.Num(TEXT("mParticleType"), 0.0)) == 4;
	}

	bool IsStockPackage(const FString& Pkg)
	{
		return UE2NetModWeapons::IsStockClass(Pkg + TEXT(".X"));
	}
}

using namespace UE2NetScriptVisualsLocal;

struct FUE2NetScriptVisuals::FVisual
{
	TWeakObjectPtr<AUE2NetScriptVisual> Actor;
	FString Mesh;
	bool bStatic = false;
	TArray<FString> Skins;
	FString AnimPrefix;
	uint32 AnimSerial = 0;
	bool bEmitter = false;
	bool bEmitterTried = false;
	/** A mesh-particle xEmitter: one burst each time script raises its mStartParticles (its Trigger). */
	bool bMeshBursts = false;
	int32 LastStartParticles = 0;
};

AUE2NetScriptVisual::AUE2NetScriptVisual()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	SetReplicates(false);
}

void AUE2NetScriptVisual::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Emitter)
	{
		// (UE2 Emitter.Kill / Destroy: the particles in flight finish)
		Emitter->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		Emitter->SetLifeSpan(Reason == EEndPlayReason::Destroyed ? 1.5f : 0.01f);
		Emitter = nullptr;
	}
	Super::EndPlay(Reason);
}

FUE2NetScriptVisuals::FUE2NetScriptVisuals(UWorld* InWorld) : World(InWorld)
{
}

FUE2NetScriptVisuals::~FUE2NetScriptVisuals()
{
	for (const TPair<FUSObject*, TSharedPtr<FVisual>>& KV : Visuals)
	{
		if (AUE2NetScriptVisual* A = KV.Value->Actor.Get())
		{
			A->Destroy();
		}
	}
}

AActor* FUE2NetScriptVisuals::Find(FUSObject* Obj) const
{
	const TSharedPtr<FVisual>* V = Visuals.Find(Obj);
	return V ? (*V)->Actor.Get() : nullptr;
}

FString FUE2NetScriptVisuals::Describe() const
{
	TMap<FString, int32> ByClass;
	for (const TPair<FUSObject*, TSharedPtr<FVisual>>& KV : Visuals)
	{
		++ByClass.FindOrAdd(KV.Key && KV.Key->Class ? KV.Key->Class->GetPathName() : FString());
	}
	ByClass.ValueSort([](int32 A, int32 B) { return A > B; });
	FString Top;
	int32 n = 0;
	for (const TPair<FString, int32>& KV : ByClass)
	{
		if (n++ == 5)
		{
			break;
		}
		Top += FString::Printf(TEXT("%s%s x%d"), Top.IsEmpty() ? TEXT("") : TEXT(", "), *KV.Key, KV.Value);
	}
	return FString::Printf(TEXT("%d shown (%s; made: %d meshes, %d static meshes, %d emitters; %d animations played)"), Visuals.Num(), *Top, MadeMeshes, MadeStaticMeshes,
		MadeEmitters, AnimsPlayed);
}

void FUE2NetScriptVisuals::Sync(const TArray<FUSObject*>& Actors, TFunctionRef<USceneComponent*(FUSObject*)> Mirrored)
{
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	FUSEngineProps& P = USProps();
	const float S = ArenaUE2World::Scale;
	TSet<FUSObject*> Seen;
	for (FUSObject* O : Actors)
	{
		if (!O || O->IsPendingKill() || !O->Data || !(O->Flags & USF_Actor))
		{
			continue;
		}
		const uint8 DrawType = Get<uint8>(O, TEXT("DrawType"));
		const bool bEmitter = IsEmitterClass(O);
		const FString Mesh = DrawType == DT_Mesh ? ObjPath(O, TEXT("Mesh")) : (DrawType == DT_StaticMesh ? ObjPath(O, TEXT("StaticMesh")) : FString());
		if (!bEmitter && Mesh.IsEmpty())
		{
			continue;
		}
		Seen.Add(O);
		TSharedPtr<FVisual>& VP = Visuals.FindOrAdd(O);
		if (!VP.IsValid())
		{
			VP = MakeShared<FVisual>();
		}
		FVisual& V = *VP;
		AUE2NetScriptVisual* A = V.Actor.Get();
		if (!A)
		{
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SP.ObjectFlags |= RF_Transient;
			A = W->SpawnActor<AUE2NetScriptVisual>(AUE2NetScriptVisual::StaticClass(), FTransform::Identity, SP);
			if (!A)
			{
				continue;
			}
			V.Actor = A;
		}
		// ---- what it draws ----
		const bool bStatic = DrawType == DT_StaticMesh;
		if (!Mesh.IsEmpty() && (Mesh != V.Mesh || bStatic != V.bStatic))
		{
			V.Mesh = Mesh;
			V.bStatic = bStatic;
			V.Skins.Reset();
			V.AnimSerial = 0;
			if (bStatic)
			{
				UE2NetModContent::EnsureConverted(Mesh, TEXT("StaticMesh"));
				if (UStaticMesh* SM = ArenaRuntimeContent::LoadStaticMeshByObjectPath(Mesh))
				{
					if (!A->Static)
					{
						A->Static = NewObject<UStaticMeshComponent>(A, TEXT("Static"));
						A->Static->SetupAttachment(A->Root);
						A->Static->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						A->Static->SetGenerateOverlapEvents(false);
						A->Static->RegisterComponent();
					}
					A->Static->SetStaticMesh(SM);
					++MadeStaticMeshes;
				}
			}
			else
			{
				UE2NetModContent::EnsureConverted(Mesh, TEXT("SkeletalMesh"));
				if (USkeletalMesh* SK = ArenaArt::Load<USkeletalMesh>(UE2NetModWeapons::ArtPathOf(TEXT("SkeletalMeshes"), Mesh)))
				{
					if (!A->Skeletal)
					{
						A->Skeletal = NewObject<USkeletalMeshComponent>(A, TEXT("Skeletal"));
						A->Skeletal->SetupAttachment(A->Root);
						A->Skeletal->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						A->Skeletal->SetGenerateOverlapEvents(false);
						A->Skeletal->RegisterComponent();
					}
					A->Skeletal->SetSkeletalMeshAsset(SK);
					V.AnimPrefix = UE2NetModWeapons::AnimPrefixForMesh(Mesh);
					++MadeMeshes;
				}
			}
			if (A->Static)
			{
				A->Static->SetVisibility(bStatic);
			}
			if (A->Skeletal)
			{
				A->Skeletal->SetVisibility(!bStatic);
			}
		}
		if (bEmitter)
		{
			const FString ClassPath = O->Class->GetPathName();
			if (!V.bEmitterTried)
			{
				V.bEmitterTried = true;
				FString Pkg;
				ClassPath.Split(TEXT("."), &Pkg, nullptr);
				if (!IsStockPackage(Pkg))
				{
					ArenaUE2Effects::AddPackageFile(UE2NetModContent::PackageFile(Pkg));
				}
				// an xEmitter of mesh particles bursts when triggered (its Trigger adds to mStartParticles), others run on
				V.bMeshBursts = O->IsA(FName(TEXT("xEmitter"))) && IsMeshXEmitter(ClassPath);
				AActor* E = nullptr;
				if (!V.bMeshBursts)
				{
					E = O->IsA(FName(TEXT("xEmitter"))) ? ArenaUE2Effects::SpawnSpriteXEmitter(W, ClassPath, A->GetActorLocation()) : nullptr;
					// (UE2 Emitter.AutoDestroy: gone once its particles are; otherwise it lives as long as its script actor)
					E = E ? E : ArenaUE2Effects::SpawnEmitter(W, ClassPath, A->GetActorLocation(), A->GetActorRotation(),
						Get<uint32>(O, TEXT("AutoDestroy")) ? 20.f : 600.f);
				}
				if (E)
				{
					E->AttachToComponent(A->Root, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
					A->Emitter = E;
					V.bEmitter = true;
					++MadeEmitters;
				}
			}
			if (V.bMeshBursts)
			{
				const int32 Start = Get<int32>(O, TEXT("mStartParticles"));
				if (Start > V.LastStartParticles)
				{
					if (AActor* E = ArenaUE2Effects::SpawnMeshXEmitter(W, ClassPath, A->GetActorLocation(), A->GetActorRotation()))
					{
						E->AttachToComponent(A->Root, FAttachmentTransformRules::KeepWorldTransform);
						++MadeEmitters;
					}
				}
				V.LastStartParticles = Start;
			}
		}
		// ---- skins ----
		UMeshComponent* MeshComp = V.bStatic ? static_cast<UMeshComponent*>(A->Static.Get()) : static_cast<UMeshComponent*>(A->Skeletal.Get());
		if (MeshComp)
		{
			const TArray<FString> Skins = ObjPaths(O, TEXT("Skins"));
			if (Skins != V.Skins)
			{
				V.Skins = Skins;
				MeshComp->EmptyOverrideMaterials();
				const TArray<UMaterialInterface*> Materials = UE2NetModWeapons::LoadSkins(Skins);
				for (int32 i = 0; i < Materials.Num() && i < MeshComp->GetNumMaterials(); ++i)
				{
					if (Materials[i])
					{
						MeshComp->SetMaterial(i, Materials[i]);
					}
				}
			}
		}
		// ---- where ----
		FUSObject* Base = P.Base.Get<FUSObject*>(O);
		USceneComponent* Parent = Base && !Base->IsPendingKill() ? Mirrored(Base) : nullptr;
		if (!Parent && Base)
		{
			if (const TSharedPtr<FVisual>* BV = Visuals.Find(Base))
			{
				if (AUE2NetScriptVisual* BA = (*BV)->Actor.Get())
				{
					Parent = BA->Skeletal ? static_cast<USceneComponent*>(BA->Skeletal) : (BA->Static ? static_cast<USceneComponent*>(BA->Static) : BA->Root.Get());
				}
			}
		}
		A->Root->SetUsingAbsoluteScale(true);
		const FUSVector S3 = Get<FUSVector>(O, TEXT("DrawScale3D"), FUSVector(1.f, 1.f, 1.f));
		A->Root->SetRelativeScale3D(FVector(S3.X, S3.Y, S3.Z) * P.DrawScale.Get<float>(O, 1.f));
		if (Parent)
		{
			const FName Bone = Get<FName>(O, TEXT("AttachmentBone"));
			const FName Socket = !Bone.IsNone() && Parent->DoesSocketExist(Bone) ? Bone : NAME_None;
			if (A->Root->GetAttachParent() != Parent || A->Root->GetAttachSocketName() != Socket)
			{
				A->Root->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Socket);
			}
			const FUSVector RL = Get<FUSVector>(O, TEXT("RelativeLocation"));
			A->Root->SetRelativeLocationAndRotation(FVector(RL.X, RL.Y, RL.Z) * S, USUnits::FromUE2(Get<FUSRotator>(O, TEXT("RelativeRotation"))));
			// UE2 moves attached actors: script reads their Location
			P.Location.Set<FUSVector>(O, USUnits::ToUE2(A->Root->GetComponentLocation()));
			P.Rotation.Set<FUSRotator>(O, USUnits::ToUE2(A->Root->GetComponentRotation()));
		}
		else
		{
			if (A->Root->GetAttachParent())
			{
				A->Root->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
			}
			A->SetActorLocationAndRotation(USUnits::FromUE2(P.Location.Get<FUSVector>(O)), USUnits::FromUE2(P.Rotation.Get<FUSRotator>(O)));
		}
		A->SetActorHiddenInGame(P.bHidden.Get<uint32>(O) != 0);
		// its particles are done: UE2 Emitter.AutoDestroy, an xEmitter without mRegen (native AutoDestroy too)
		if (V.bEmitter && !IsValid(A->Emitter) && ((O->IsA(FName(TEXT("Emitter"))) && Get<uint32>(O, TEXT("AutoDestroy")) != 0) || (O->IsA(FName(TEXT("xEmitter"))) && Get<uint32>(O, TEXT("mRegen")) == 0)))
		{
			Finished.Add(O);
		}
		// ---- animation ----
		const USPresentation::FAnim* Anim = USPresentation::FindAnim(O);
		if (Anim && Anim->Serial != V.AnimSerial && A->Skeletal && !V.bStatic)
		{
			V.AnimSerial = Anim->Serial;
			if (Anim->bStopped)
			{
				A->Skeletal->bPauseAnims = true;
			}
			else if (UAnimSequence* Seq = V.AnimPrefix.IsEmpty() ? nullptr : ArenaArt::Load<UAnimSequence>(V.AnimPrefix + Anim->Sequence.ToString()))
			{
				A->Skeletal->bPauseAnims = false;
				A->Skeletal->PlayAnimation(Seq, Anim->bLoop);
				A->Skeletal->SetPlayRate(FMath::Max(0.01f, Anim->Rate));
				++AnimsPlayed;
			}
		}
	}
	// script actors that are gone (destroyed, or nothing to draw any more)
	for (auto It = Visuals.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			if (AUE2NetScriptVisual* A = It.Value()->Actor.Get())
			{
				A->Destroy();
			}
			It.RemoveCurrent();
		}
	}
}

#include "ArenaSkybox.h"
#include "OpenUT2k4.h"
#include "ArenaRuntimeContent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace ArenaSkyboxLocal
{
	TAutoConsoleVariable<int32> CVarResolution(TEXT("arena.SkyboxResolution"), 512, TEXT("Size of each UE2 sky box face (applies on map load). 1024 is sharper but costs frame time while the sky refreshes."));
	TAutoConsoleVariable<float> CVarRefresh(TEXT("arena.SkyboxRefresh"), 1.f,
		TEXT("Seconds for one full re-capture of the UE2 sky box (one face at a time; animated skies). 0 = captured once at load."));

	/** Capture orientation of face I: +X, -X, +Y, -Y, +Z, -Z. */
	FRotator FaceRotation(int32 I)
	{
		switch (I)
		{
		case 0: return FRotator(0.f, 0.f, 0.f);
		case 1: return FRotator(0.f, 180.f, 0.f);
		case 2: return FRotator(0.f, 90.f, 0.f);
		case 3: return FRotator(0.f, -90.f, 0.f);
		case 4: return FRotator(90.f, 0.f, 0.f);
		default: return FRotator(-90.f, 0.f, 0.f);
		}
	}
}

AArenaSkybox::AArenaSkybox()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	CubeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CubeRoot"));
	CubeRoot->SetupAttachment(RootComponent);
	CubeRoot->SetUsingAbsoluteLocation(true);
	CubeRoot->SetUsingAbsoluteRotation(true);
}

int32 AArenaSkybox::GetResolution()
{
	return FMath::Clamp(ArenaSkyboxLocal::CVarResolution.GetValueOnGameThread(), 64, 4096);
}

AArenaSkybox* AArenaSkybox::Spawn(UWorld* World, const FVector& SkyViewLocation)
{
	if (!World)
	{
		return nullptr;
	}
	TActorIterator<AArenaSkybox> Existing(World);
	if (Existing)
	{
		return *Existing;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaSkybox* Sky = World->SpawnActor<AArenaSkybox>(AArenaSkybox::StaticClass(), FTransform(SkyViewLocation), Params);
	if (Sky)
	{
		Sky->SkyLocation = SkyViewLocation;
		Sky->BuildCube();
	}
	return Sky;
}

void AArenaSkybox::AddSkyPrimitive(UPrimitiveComponent* Comp)
{
	if (!Comp || Faces.Contains(Comp))
	{
		return;
	}
	SkyPrimitives.AddUnique(Comp);
	Comp->SetVisibleInSceneCaptureOnly(true);
	Comp->SetCastShadow(false);
	for (USceneCaptureComponent2D* C : Captures)
	{
		C->ShowOnlyComponents.AddUnique(Comp);
	}
	CapturesDone = 0;
}

void AArenaSkybox::BuildCube()
{
	using namespace ArenaSkyboxLocal;
	if (bBuilt)
	{
		return;
	}
	bBuilt = true;
	const int32 Res = GetResolution();
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_ArenaSkyFace.M_ArenaSkyFace"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Parent)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Skybox: /Game/Materials/M_ArenaSkyFace missing (run Tools/GenerateContent.py), no 3D sky box"));
		return;
	}
	// one unit cube (half size 1), one section per face; UVs match the face's capture
	TArray<ArenaRuntimeContent::FMeshSection> Sections;
	for (int32 i = 0; i < 6; ++i)
	{
		const FRotator R = FaceRotation(i);
		const FVector F = R.Vector();
		const FVector Rt = FRotationMatrix(R).GetUnitAxis(EAxis::Y);
		const FVector Up = FRotationMatrix(R).GetUnitAxis(EAxis::Z);
		ArenaRuntimeContent::FMeshSection& S = Sections.AddDefaulted_GetRef();
		S.Material = FString::Printf(TEXT("ArenaSkyFace%d"), i);
		S.bCollides = false;
		const float Us[4] = { -1, 1, 1, -1 };
		const float Vs[4] = { 1, 1, -1, -1 };
		for (int32 k = 0; k < 4; ++k)
		{
			// slightly larger than the cube so the edges overlap
			const FVector P = F + Rt * Us[k] * 1.002 + Up * Vs[k] * 1.002;
			S.Positions.Add(FVector3f(P));
			S.Normals.Add(FVector3f(-F));
			S.UV0.Add(FVector2f((Us[k] + 1.f) * 0.5f, (1.f - Vs[k]) * 0.5f));
		}
		S.Indices = { 0, 1, 2, 0, 2, 3 };
	}
	UStaticMesh* Cube = ArenaRuntimeContent::BuildStaticMesh(TEXT("ArenaSkyCube"), Sections);
	if (!Cube)
	{
		return;
	}
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, TEXT("SkyCube"));
	Mesh->SetStaticMesh(Cube);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->bAffectDistanceFieldLighting = false;
	Mesh->bVisibleInRayTracing = false;
	Mesh->bVisibleInReflectionCaptures = true;
	Mesh->SetHiddenInSceneCapture(true);
	Mesh->bNeverDistanceCull = true;
	Mesh->SetupAttachment(CubeRoot);
	Mesh->SetRelativeScale3D(FVector(CubeHalfSize));
	Mesh->RegisterComponent();
	Faces.Add(Mesh);

	for (int32 i = 0; i < 6; ++i)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
		RT->RenderTargetFormat = RTF_RGBA16f;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->InitAutoFormat(Res, Res);
		RT->UpdateResourceImmediate(true);
		Targets.Add(RT);

		USceneCaptureComponent2D* C = NewObject<USceneCaptureComponent2D>(this);
		C->SetupAttachment(RootComponent);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetWorldLocationAndRotation(SkyLocation, FaceRotation(i));
		C->FOVAngle = 90.f;
		C->TextureTarget = RT;
		C->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
		C->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		C->bCaptureEveryFrame = false;
		C->bCaptureOnMovement = false;
		C->bAlwaysPersistRenderingState = true;
		C->ShowFlags.SetAtmosphere(false);
		C->ShowFlags.SetFog(false);
		C->ShowFlags.SetVolumetricFog(false);
		C->ShowFlags.SetCloud(false);
		C->ShowFlags.SetMotionBlur(false);
		C->ShowFlags.SetBloom(false);
		C->ShowFlags.SetDynamicShadows(false);
		C->ShowFlags.SetAmbientOcclusion(false);
		C->ShowFlags.SetScreenSpaceReflections(false);
		C->ShowFlags.SetContactShadows(false);
		C->ShowFlags.SetDistanceFieldAO(false);
		C->ShowFlags.SetLumenGlobalIllumination(false);
		C->ShowFlags.SetLumenReflections(false);
		C->ShowFlags.SetTemporalAA(false);
		C->ShowFlags.SetAntiAliasing(false);
		C->bUseRayTracingIfEnabled = false;
		C->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = 1;
		C->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		C->PostProcessSettings.bOverride_ReflectionMethod = 1;
		C->PostProcessSettings.ReflectionMethod = EReflectionMethod::None;
		// Full Ray Tracing (ArenaRayTracing) turns MegaLights on project-wide; the sky faces keep plain deferred lights.
		C->PostProcessSettings.bOverride_bMegaLights = 1;
		C->PostProcessSettings.bMegaLights = false;
		C->ShowFlags.SetMegaLights(false);
		for (const TWeakObjectPtr<UPrimitiveComponent>& P : SkyPrimitives)
		{
			if (P.IsValid())
			{
				C->ShowOnlyComponents.Add(P.Get());
			}
		}
		C->RegisterComponent();
		Captures.Add(C);

		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
		MID->SetTextureParameterValue(TEXT("Base"), RT);
		FaceMaterials.Add(MID);
		Mesh->SetMaterial(i, MID);
	}

	// the height fog must not cover the far cube (UE2 sky boxes carry their own look)
	for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
	{
		if (UExponentialHeightFogComponent* F = It->GetComponent())
		{
			if (F->FogCutoffDistance <= 0.f || F->FogCutoffDistance > CubeHalfSize * 0.5f)
			{
				F->FogCutoffDistance = CubeHalfSize * 0.5f;
				F->MarkRenderStateDirty();
			}
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Skybox: 3D sky box at %s, %d x %d faces"), *SkyLocation.ToCompactString(), Res, Res);
}

void AArenaSkybox::BeginPlay()
{
	Super::BeginPlay();
	BuildCube();
}

void AArenaSkybox::CaptureAll()
{
	for (USceneCaptureComponent2D* C : Captures)
	{
		C->CaptureSceneDeferred();
	}
}

void AArenaSkybox::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Captures.Num() != 6)
	{
		return;
	}
	// the cube is centred on the viewer: no parallax, like UE2
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			CubeRoot->SetWorldLocation(PC->PlayerCameraManager->GetCameraCacheView().Location);
		}
	}
	if (CapturesDone < 6)
	{
		CaptureAll();
		CapturesDone = 6;
		return;
	}
	const float Refresh = ArenaSkyboxLocal::CVarRefresh.GetValueOnGameThread();
	if (Refresh <= 0.f)
	{
		return;
	}
	// one face every Refresh / 6 seconds
	CaptureTimer += DeltaSeconds;
	if (CaptureTimer >= Refresh / 6.f)
	{
		CaptureTimer = 0.f;
		Captures[NextFace % 6]->CaptureSceneDeferred();
		NextFace = (NextFace + 1) % 6;
	}
}

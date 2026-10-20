#include "ArenaEmitters.h"
#include "OpenUT2k4.h"
#include "ArenaArtFX.h"
#include "ArenaRuntimeContent.h"
#include "ArenaSkybox.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMeshBuilder.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "UObject/StrongObjectPtr.h"

namespace ArenaEmittersLocal
{
	TAutoConsoleVariable<float> CVarEmitterDistance(TEXT("arena.EmitterDistance"), 25000.f,
		TEXT("UE2 emitters farther than this from the viewer (cm) pause and are not drawn (0 = never)."));
	TAutoConsoleVariable<int32> CVarEmitters(TEXT("arena.Emitters"), 1, TEXT("Draw UE2 map emitters (0 = off)."));

	constexpr float S = ArenaUE2World::Scale;

	FVector RandIn(FRandomStream& R, const FVector& Mn, const FVector& Mx)
	{
		return FVector(R.FRandRange(Mn.X, Mx.X), R.FRandRange(Mn.Y, Mx.Y), R.FRandRange(Mn.Z, Mx.Z));
	}

	float RandRange(FRandomStream& R, const FVector2D& Rg)
	{
		return R.FRandRange(float(Rg.X), float(Rg.Y));
	}

	UMaterialInterface* ParticleParent(FArenaParticleParams::EStyle Style)
	{
		// strong: once a map's emitters are gone nothing else references the parents, and a garbage-collected parent
		// left every later map's particles on the texture's opaque surface material (black squares)
		static TStrongObjectPtr<UMaterialInterface> Cache[4];
		static bool bTried[4] = { false, false, false, false };
		const int32 I = int32(Style);
		if (!bTried[I])
		{
			bTried[I] = true;
			const TCHAR* Names[4] = { TEXT("/Game/Materials/M_ArenaParticleAdd.M_ArenaParticleAdd"), TEXT("/Game/Materials/M_ArenaParticleAlpha.M_ArenaParticleAlpha"),
				TEXT("/Game/Materials/M_ArenaParticleMod.M_ArenaParticleMod"), TEXT("/Game/Materials/M_ArenaParticleDarken.M_ArenaParticleDarken") };
			Cache[I].Reset(LoadObject<UMaterialInterface>(nullptr, Names[I], nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
		return Cache[I].Get();
	}

	/** The particle texture: a texture, or the Base texture of a UE2 material (Shader, FinalBlend ...). */
	UTexture* LoadParticleTexture(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UTexture2D* T = ArenaRuntimeContent::LoadTextureByObjectPath(Path))
		{
			return T;
		}
		if (UMaterialInterface* M = ArenaRuntimeContent::LoadMaterialByObjectPath(Path))
		{
			UTexture* T = nullptr;
			if (M->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Base")), T) && T)
			{
				return T;
			}
		}
		return nullptr;
	}

	FLinearColor ToLinear(const FColor& C)
	{
		return FLinearColor(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f);
	}
}

// ---------------------------------------------------------------------------------------------
// FArenaParticleParams
// ---------------------------------------------------------------------------------------------

void FArenaParticleParams::Load(const FString& UE2Class, const TArray<FString>& Chain, const FArenaUE2Props& P, float InDrawScale)
{
	using namespace ArenaEmittersLocal;
	auto In = [&Chain, &UE2Class](const TCHAR* C)
	{
		if (UE2Class.Equals(C, ESearchCase::IgnoreCase)) return true;
		for (const FString& X : Chain) if (X.Equals(C, ESearchCase::IgnoreCase)) return true;
		return false;
	};
	Kind = In(TEXT("SparkEmitter")) ? EKind::Spark : In(TEXT("BeamEmitter")) ? EKind::Beam : In(TEXT("MeshEmitter")) ? EKind::Mesh : EKind::Sprite;
	DrawScale = InDrawScale;
	TexturePath = P.Str(TEXT("Texture"));
	if (TexturePath.StartsWith(TEXT("Engine.S_")))
	{
		TexturePath.Reset(); // editor icon default
	}
	MeshPath = P.Str(TEXT("StaticMesh"));
	// UE2 EParticleDrawStyle: Regular, AlphaBlend, Modulated, Translucent, AlphaModulate, Darken, Brighten
	switch (int32(P.Num(TEXT("DrawStyle"), 3)))
	{
	case 2: Style = EStyle::Modulate; break;
	case 5: Style = EStyle::Darken; break; // dest * (1 - src): black texels leave the scene unchanged
	case 3: case 6: Style = EStyle::Additive; break;
	default: Style = EStyle::Alpha; break;
	}
	MaxParticles = FMath::Clamp(int32(P.Num(TEXT("MaxParticles"), 10)), 0, 400);
	bRespawnDeadParticles = P.Bool(TEXT("RespawnDeadParticles"), true);
	bAutomaticInitialSpawning = P.Bool(TEXT("AutomaticInitialSpawning"), true);
	InitialParticlesPerSecond = float(P.Num(TEXT("InitialParticlesPerSecond"), 0.0));
	ParticlesPerSecond = float(P.Num(TEXT("ParticlesPerSecond"), 0.0));
	bAutoDestroy = P.Bool(TEXT("AutoDestroy"));
	bAutoReset = P.Bool(TEXT("AutoReset"));
	AutoResetTimeRange = P.Range(TEXT("AutoResetTimeRange"));
	bTriggerDisabled = P.Bool(TEXT("TriggerDisabled"), true);
	bResetOnTrigger = P.Bool(TEXT("ResetOnTrigger"));
	bDisabled = P.Bool(TEXT("Disabled"));
	LifetimeRange = P.Range(TEXT("LifetimeRange"), FVector2D(4.f, 4.f));
	InitialDelayRange = P.Range(TEXT("InitialDelayRange"));
	bLocalSpace = int32(P.Num(TEXT("CoordinateSystem"), 0)) == 1;
	StartLocationShape = uint8(P.Num(TEXT("StartLocationShape"), 0));
	StartLocationOffset = P.Vec(TEXT("StartLocationOffset")) * S;
	P.RangeVector(TEXT("StartLocationRange"), StartLocationMin, StartLocationMax);
	StartLocationMin *= S;
	StartLocationMax *= S;
	SphereRadiusRange = P.Range(TEXT("SphereRadiusRange")) * S;
	P.RangeVector(TEXT("StartLocationPolarRange"), StartLocationPolarMin, StartLocationPolarMax);
	P.RangeVector(TEXT("StartVelocityRange"), StartVelocityMin, StartVelocityMax);
	StartVelocityMin *= S;
	StartVelocityMax *= S;
	StartVelocityRadialRange = P.Range(TEXT("StartVelocityRadialRange")) * S;
	bGetVelocityDirectionFrom = int32(P.Num(TEXT("GetVelocityDirectionFrom"), 0)) != 0;
	Acceleration = P.Vec(TEXT("Acceleration")) * S;
	P.RangeVector(TEXT("VelocityLossRange"), VelocityLossMin, VelocityLossMax);
	MaxAbsVelocity = P.Vec(TEXT("MaxAbsVelocity")) * S;
	P.RangeVector(TEXT("StartSizeRange"), StartSizeMin, StartSizeMax, FVector(100.f));
	bUniformSize = P.Bool(TEXT("UniformSize"));
	bUseColorScale = P.Bool(TEXT("UseColorScale"));
	bUseSizeScale = P.Bool(TEXT("UseSizeScale"));
	bUseRegularSizeScale = P.Bool(TEXT("UseRegularSizeScale"), true);
	ColorScaleRepeats = float(P.Num(TEXT("ColorScaleRepeats"), 0.0));
	SizeScaleRepeats = float(P.Num(TEXT("SizeScaleRepeats"), 0.0));
	bool bAnyAlpha = false;
	for (const TSharedPtr<FJsonValue>& V : P.Array(TEXT("ColorScale")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		double T = 0.0;
		O->TryGetNumberField(TEXT("RelativeTime"), T);
		FArenaUE2Props Tmp;
		Tmp.Obj = O;
		const FColor C = Tmp.Color(TEXT("Color"), FColor::White);
		bAnyAlpha |= C.A > 0;
		ColorScale.Add(TPair<float, FLinearColor>(float(T), ToLinear(C)));
	}
	if (!bAnyAlpha)
	{
		// UE2 ColorScale alpha left at 0 by the editor: only the colour fades
		for (TPair<float, FLinearColor>& C : ColorScale) C.Value.A = 1.f;
	}
	for (const TSharedPtr<FJsonValue>& V : P.Array(TEXT("SizeScale")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		double T = 0.0, Sz = 1.0;
		O->TryGetNumberField(TEXT("RelativeTime"), T);
		O->TryGetNumberField(TEXT("RelativeSize"), Sz);
		SizeScale.Add(TPair<float, FVector>(float(T), FVector(Sz)));
	}
	P.RangeVector(TEXT("ColorMultiplierRange"), ColorMultiplierMin, ColorMultiplierMax, FVector::OneVector);
	Opacity = float(P.Num(TEXT("Opacity"), 1.0));
	bFadeOut = P.Bool(TEXT("FadeOut"));
	bFadeIn = P.Bool(TEXT("FadeIn"));
	FadeOutStartTime = float(P.Num(TEXT("FadeOutStartTime"), 0.0));
	FadeInEndTime = float(P.Num(TEXT("FadeInEndTime"), 0.0));
	if (const TSharedPtr<FJsonObject> F = P.Object(TEXT("FadeOutFactor")))
	{
		FadeOutFactor = FVector4f(float(F->GetNumberField(TEXT("X"))), float(F->GetNumberField(TEXT("Y"))), float(F->GetNumberField(TEXT("Z"))), float(F->GetNumberField(TEXT("W"))));
	}
	if (const TSharedPtr<FJsonObject> F = P.Object(TEXT("FadeInFactor")))
	{
		FadeInFactor = FVector4f(float(F->GetNumberField(TEXT("X"))), float(F->GetNumberField(TEXT("Y"))), float(F->GetNumberField(TEXT("Z"))), float(F->GetNumberField(TEXT("W"))));
	}
	bSpinParticles = P.Bool(TEXT("SpinParticles"));
	FVector Mn, Mx;
	P.RangeVector(TEXT("StartSpinRange"), Mn, Mx);
	StartSpinRange = FVector2D(Mn.X, Mx.X);
	P.RangeVector(TEXT("SpinsPerSecondRange"), Mn, Mx);
	SpinsPerSecondRange = FVector2D(Mn.X, Mx.X);
	SpinCCWorCW = float(P.Vec(TEXT("SpinCCWorCW"), FVector(0.5f)).X);
	// UE2 UseDirectionAs: None, Up, Right, Forward, Normal, UpAndNormal, RightAndNormal, Scale
	switch (int32(P.Num(TEXT("UseDirectionAs"), 0)))
	{
	case 1: case 2: case 3: Facing = 2; break;
	case 4: case 5: case 6: Facing = 1; break;
	default: Facing = 0; break;
	}
	TextureUSubdivisions = FMath::Max(1, int32(P.Num(TEXT("TextureUSubdivisions"), 1)));
	TextureVSubdivisions = FMath::Max(1, int32(P.Num(TEXT("TextureVSubdivisions"), 1)));
	bBlendBetweenSubdivisions = P.Bool(TEXT("BlendBetweenSubdivisions"));
	bUseRandomSubdivision = P.Bool(TEXT("UseRandomSubdivision"));
	if (P.Bool(TEXT("UseSubdivisionScale")))
	{
		for (const TSharedPtr<FJsonValue>& V : P.Array(TEXT("SubdivisionScale")))
		{
			SubdivisionScale.Add(float(V->AsNumber()));
		}
	}
	SubdivisionStart = int32(P.Num(TEXT("SubdivisionStart"), 0));
	SubdivisionEnd = int32(P.Num(TEXT("SubdivisionEnd"), 0));
	TimeBetweenSegmentsRange = P.Range(TEXT("TimeBetweenSegmentsRange"), FVector2D(0.1f, 0.1f));
	BeamTextureUScale = float(P.Num(TEXT("BeamTextureUScale"), 1.0));
	BeamSegments = FMath::Clamp(int32(P.Num(TEXT("HighFrequencyPoints"), 1)), 1, 24);
	P.RangeVector(TEXT("HighFrequencyNoiseRange"), BeamNoiseMin, BeamNoiseMax);
	BeamNoiseMin *= S;
	BeamNoiseMax *= S;
	if (Kind == EKind::Beam)
	{
		// DetermineEndPointBy: Velocity, Distance, Offset ...
		const int32 By = int32(P.Num(TEXT("DetermineEndPointBy"), 0));
		const TArray<TSharedPtr<FJsonValue>>& Ends = P.Array(TEXT("BeamEndPoints"));
		if ((By == 2 || By == 5) && Ends.Num() > 0 && Ends[0]->AsObject().IsValid())
		{
			FArenaUE2Props E;
			E.Obj = Ends[0]->AsObject();
			E.RangeVector(TEXT("Offset"), BeamEndMin, BeamEndMax);
			BeamEndMin *= S;
			BeamEndMax *= S;
		}
		else
		{
			const FVector2D D = P.Range(TEXT("BeamDistanceRange"), FVector2D(500.f, 500.f)) * S;
			// along the velocity direction (resolved per particle)
			BeamEndMin = FVector(D.X, 0, 0);
			BeamEndMax = FVector(D.Y, 0, 0);
			BeamSegments = -FMath::Abs(BeamSegments); // negative: by distance along velocity
		}
	}
	if (Kind == EKind::Mesh)
	{
		bUseMeshBlendMode = P.Bool(TEXT("UseMeshBlendMode"), true);
	}
	SecondsBeforeInactive = float(P.Num(TEXT("SecondsBeforeInactive"), 1.0));
}

void FArenaParticleParams::LoadXEmitter(const FArenaUE2Props& P, const FString& Texture, float InDrawScale)
{
	using namespace ArenaEmittersLocal;
	auto R2 = [&P](const TCHAR* K, float Def) { return FVector2D(P.Num(K, Def), P.Num(FString(K) + TEXT("[1]"), P.Num(K, Def))); };
	Kind = EKind::Sprite;
	DrawScale = InDrawScale;
	TexturePath = Texture;
	// UE2 ERenderStyle: None, Normal, Masked, Translucent, Modulated, Alpha, Additive, Subtractive, Particle, AlphaZ
	switch (int32(P.Num(TEXT("Style"), 3)))
	{
	case 4: Style = EStyle::Modulate; break;
	case 3: case 6: case 8: Style = EStyle::Additive; break;
	default: Style = EStyle::Alpha; break;
	}
	MaxParticles = FMath::Clamp(int32(P.Num(TEXT("mMaxParticles"), 10)), 0, 400);
	LifetimeRange = R2(TEXT("mLifeRange"), 1.f);
	const FVector2D Regen = R2(TEXT("mRegenRange"), 0.f);
	ParticlesPerSecond = float(Regen.X + Regen.Y) * 0.5f;
	bRespawnDeadParticles = ParticlesPerSecond > 0.f;
	bAutomaticInitialSpawning = true;
	if (!P.Bool(TEXT("mRegen"), true))
	{
		bRespawnDeadParticles = false;
	}
	const FVector2D Speed = R2(TEXT("mSpeedRange"), 0.f) * S;
	const FVector DirDev = P.Vec(TEXT("mDirDev"));
	StartVelocityMin = FVector(Speed.X, -DirDev.Y * Speed.Y, -DirDev.Z * Speed.Y);
	StartVelocityMax = FVector(Speed.Y, DirDev.Y * Speed.Y, DirDev.Z * Speed.Y);
	const FVector PosDev = P.Vec(TEXT("mPosDev")) * S;
	StartLocationMin = -PosDev;
	StartLocationMax = PosDev;
	const FVector2D Mass = R2(TEXT("mMassRange"), 0.f);
	Acceleration = FVector(0, 0, -float(Mass.X + Mass.Y) * 0.5f * 40.f * S);
	const float Air = float(P.Num(TEXT("mAirResistance"), 0.0));
	VelocityLossMin = VelocityLossMax = FVector(Air);
	const FVector2D Size = R2(TEXT("mSizeRange"), 10.f);
	StartSizeMin = FVector(Size.X);
	StartSizeMax = FVector(Size.Y);
	bUniformSize = true;
	const float Growth = float(P.Num(TEXT("mGrowthRate"), 0.0));
	const float Life = float(LifetimeRange.X + LifetimeRange.Y) * 0.5f;
	if (FMath::Abs(Growth) > 0.f && Size.Y > 0.f)
	{
		bUseSizeScale = true;
		bUseRegularSizeScale = false;
		SizeScale.Add(TPair<float, FVector>(0.f, FVector(1.f)));
		SizeScale.Add(TPair<float, FVector>(1.f, FVector(FMath::Max(0.05f, 1.f + Growth * Life / float((Size.X + Size.Y) * 0.5f)))));
	}
	FArenaUE2Props C0;
	const FColor A = P.Color(TEXT("mColorRange"), FColor::White);
	const FColor B = P.Color(TEXT("mColorRange[1]"), A);
	ColorMultiplierMin = FVector(FMath::Min(A.R, B.R) / 255.f, FMath::Min(A.G, B.G) / 255.f, FMath::Min(A.B, B.B) / 255.f);
	ColorMultiplierMax = FVector(FMath::Max(A.R, B.R) / 255.f, FMath::Max(A.G, B.G) / 255.f, FMath::Max(A.B, B.B) / 255.f);
	bFadeOut = P.Bool(TEXT("mAttenuate"), true);
	FadeOutStartTime = 0.f;
	bSpinParticles = P.Bool(TEXT("mRandOrient")) || P.Has(TEXT("mSpinRange[1]"));
	StartSpinRange = FVector2D(0.f, 1.f);
	const FVector2D Spin = R2(TEXT("mSpinRange"), 0.f);
	SpinsPerSecondRange = Spin / 360.f;
	TextureUSubdivisions = FMath::Max(1, int32(P.Num(TEXT("mNumTileColumns"), 1)));
	TextureVSubdivisions = FMath::Max(1, int32(P.Num(TEXT("mNumTileRows"), 1)));
	bUseRandomSubdivision = P.Bool(TEXT("mRandTextures"));
}

// ---------------------------------------------------------------------------------------------
// UArenaParticleComponent and its scene proxy
// ---------------------------------------------------------------------------------------------

namespace ArenaEmittersLocal
{
	class FParticleSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FParticleSceneProxy(UArenaParticleComponent* InComponent)
			: FPrimitiveSceneProxy(InComponent)
			, Material(InComponent->Material)
		{
			if (Material)
			{
				MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
				MaterialProxy = Material->GetRenderProxy();
			}
			bVerifyUsedMaterials = false;
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		void SetData_RenderThread(TArray<UArenaParticleComponent::FDrawParticle>&& InParticles, uint8 InFacing)
		{
			Particles = MoveTemp(InParticles);
			Facing = InFacing;
		}

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			if (!MaterialProxy || Particles.Num() == 0)
			{
				return;
			}
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}
				const FSceneView* View = Views[ViewIndex];
				const FMatrix ViewToWorld = View->ViewMatrices.GetViewToWorld();
				const FVector3f CamRight = FVector3f(FVector(ViewToWorld.TransformVector(FVector(1, 0, 0))).GetSafeNormal());
				const FVector3f CamUp = FVector3f(FVector(ViewToWorld.TransformVector(FVector(0, 1, 0))).GetSafeNormal());
				const FVector3f CamFwd = FVector3f(FVector(ViewToWorld.TransformVector(FVector(0, 0, 1))).GetSafeNormal());
				const FVector3f ViewOrigin = FVector3f(View->ViewMatrices.GetViewOrigin());
				FDynamicMeshBuilder Builder(View->GetFeatureLevel());
				for (const UArenaParticleComponent::FDrawParticle& P : Particles)
				{
					FVector3f Right, Up;
					FVector3f Normal = -CamFwd;
					const bool bAxis = !P.Axis.IsNearlyZero();
					if (bAxis && (Facing == 2 || Facing == 3))
					{
						// stretched along the axis (streaks, sparks, beams), turned to the viewer around it
						const FVector3f Center = P.Location + P.Axis * 0.5f;
						const FVector3f AxisN = P.Axis.GetSafeNormal();
						FVector3f Side = FVector3f::CrossProduct(AxisN, (ViewOrigin - Center).GetSafeNormal());
						if (Side.IsNearlyZero())
						{
							Side = CamRight;
						}
						Side.Normalize();
						Right = P.Axis * 0.5f;
						Up = Side * P.Size.Y;
						Normal = FVector3f::CrossProduct(Right.GetSafeNormal(), Side);
						const int32 V0 = Builder.AddVertex(FDynamicMeshVertex(Center - Right - Up, Right.GetSafeNormal(), Normal, FVector2f(P.UV.X, P.UV.Y), P.Color));
						Builder.AddVertex(FDynamicMeshVertex(Center + Right - Up, Right.GetSafeNormal(), Normal, FVector2f(P.UV.Z, P.UV.Y), P.Color));
						Builder.AddVertex(FDynamicMeshVertex(Center + Right + Up, Right.GetSafeNormal(), Normal, FVector2f(P.UV.Z, P.UV.W), P.Color));
						Builder.AddVertex(FDynamicMeshVertex(Center - Right + Up, Right.GetSafeNormal(), Normal, FVector2f(P.UV.X, P.UV.W), P.Color));
						Builder.AddTriangle(V0, V0 + 1, V0 + 2);
						Builder.AddTriangle(V0, V0 + 2, V0 + 3);
						continue;
					}
					if (Facing == 1)
					{
						// flat, facing the projection normal (Z)
						Right = FVector3f(1, 0, 0);
						Up = FVector3f(0, 1, 0);
						Normal = FVector3f(0, 0, 1);
					}
					else
					{
						Right = CamRight;
						Up = CamUp;
					}
					const float Sn = FMath::Sin(P.Rotation), Cs = FMath::Cos(P.Rotation);
					const FVector3f R = (Right * Cs + Up * Sn) * P.Size.X;
					const FVector3f U = (Up * Cs - Right * Sn) * P.Size.Y;
					const FVector3f Tx = R.GetSafeNormal();
					const int32 V0 = Builder.AddVertex(FDynamicMeshVertex(P.Location - R + U, Tx, Normal, FVector2f(P.UV.X, P.UV.Y), P.Color));
					Builder.AddVertex(FDynamicMeshVertex(P.Location + R + U, Tx, Normal, FVector2f(P.UV.Z, P.UV.Y), P.Color));
					Builder.AddVertex(FDynamicMeshVertex(P.Location + R - U, Tx, Normal, FVector2f(P.UV.Z, P.UV.W), P.Color));
					Builder.AddVertex(FDynamicMeshVertex(P.Location - R - U, Tx, Normal, FVector2f(P.UV.X, P.UV.W), P.Color));
					Builder.AddTriangle(V0, V0 + 1, V0 + 2);
					Builder.AddTriangle(V0, V0 + 2, V0 + 3);
				}
				Builder.GetMesh(FMatrix::Identity, MaterialProxy, SDPG_World, true, false, ViewIndex, Collector);
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View);
			Result.bDynamicRelevance = true;
			Result.bShadowRelevance = false;
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
			Result.bVelocityRelevance = false;
			return Result;
		}

		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize() + Particles.GetAllocatedSize(); }

	private:
		UMaterialInterface* Material = nullptr;
		const FMaterialRenderProxy* MaterialProxy = nullptr;
		FMaterialRelevance MaterialRelevance;
		TArray<UArenaParticleComponent::FDrawParticle> Particles;
		uint8 Facing = 0;
	};
}

UArenaParticleComponent::UArenaParticleComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bCastDynamicShadow = false;
	bReceivesDecals = false;
	SetCanEverAffectNavigation(false);
	bUseAsOccluder = false;
	bAffectDistanceFieldLighting = false;
	bVisibleInRayTracing = false;
	// particles are in world space
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
}

void UArenaParticleComponent::SetParticleMaterial(UMaterialInterface* InMaterial)
{
	Material = InMaterial;
	MarkRenderStateDirty();
}

void UArenaParticleComponent::SetParticles(TArray<FDrawParticle>&& InParticles, uint8 InFacing, const FBox& InBounds)
{
	if (InBounds.IsValid && (!WorldBounds.IsValid || !WorldBounds.IsInside(InBounds) || WorldBounds.GetVolume() > InBounds.ExpandBy(400.f).GetVolume() * 8.0))
	{
		WorldBounds = InBounds.ExpandBy(200.f);
		UpdateBounds();
		MarkRenderTransformDirty();
	}
	if (ArenaEmittersLocal::FParticleSceneProxy* Proxy = static_cast<ArenaEmittersLocal::FParticleSceneProxy*>(SceneProxy))
	{
		ENQUEUE_RENDER_COMMAND(ArenaParticleData)([Proxy, Data = MoveTemp(InParticles), InFacing](FRHICommandListImmediate&) mutable
		{
			Proxy->SetData_RenderThread(MoveTemp(Data), InFacing);
		});
	}
}

FPrimitiveSceneProxy* UArenaParticleComponent::CreateSceneProxy()
{
	return Material ? new ArenaEmittersLocal::FParticleSceneProxy(this) : nullptr;
}

FBoxSphereBounds UArenaParticleComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (WorldBounds.IsValid)
	{
		return FBoxSphereBounds(WorldBounds);
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(100.f), 100.f);
}

void UArenaParticleComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (Material)
	{
		OutMaterials.Add(Material);
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaEmitter
// ---------------------------------------------------------------------------------------------

AArenaEmitter::AArenaEmitter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Rng.GenerateNewSeed();
}

float AArenaEmitter::GetMaxDistance()
{
	return ArenaEmittersLocal::CVarEmitterDistance.GetValueOnGameThread();
}

void AArenaEmitter::AddEmitter(const FArenaParticleParams& Params)
{
	FEmitterState& E = Emitters.AddDefaulted_GetRef();
	E.P = Params;
}

int32 AArenaEmitter::NumLiveParticles() const
{
	int32 N = 0;
	for (const FEmitterState& E : Emitters)
	{
		for (const FArenaParticle& P : E.Particles)
		{
			N += P.bAlive ? 1 : 0;
		}
	}
	return N;
}

void AArenaEmitter::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	bAutoReset = Desc.Props.Bool(TEXT("AutoReset"));
	TimeTillResetRange = Desc.Props.Range(TEXT("TimeTillResetRange"));
	if (Desc.IsA(TEXT("xEmitter")))
	{
		FArenaParticleParams P;
		P.LoadXEmitter(Desc.Props, Desc.Skins.Num() > 0 ? Desc.Skins[0] : Desc.Props.Str(TEXT("Texture")), Desc.DrawScale);
		AddEmitter(P);
	}
	else
	{
		for (const TSharedPtr<FJsonValue>& V : Desc.Props.Array(TEXT("Emitters")))
		{
			FString Class;
			TArray<FString> Chain;
			FArenaUE2Props Props;
			if (Desc.GetSubobject(FArenaUE2Props::ToString(V), Class, Chain, Props))
			{
				FArenaParticleParams P;
				P.Load(Class, Chain, Props, Desc.DrawScale);
				AddEmitter(P);
			}
		}
	}
	if (Desc.bHidden)
	{
		for (FEmitterState& E : Emitters)
		{
			E.P.bDisabled = true;
		}
	}
}

void AArenaEmitter::BeginPlay()
{
	using namespace ArenaEmittersLocal;
	Super::BeginPlay();
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
	{
		Events->RegisterActor(this, Tag);
	}
	AArenaSkybox* Sky = nullptr;
	if (Tags.Contains(FName(TEXT("ArenaSkyZone"))))
	{
		TActorIterator<AArenaSkybox> It(GetWorld());
		Sky = It ? *It : nullptr;
	}
	for (FEmitterState& E : Emitters)
	{
		E.Delay = RandRange(Rng, E.P.InitialDelayRange);
		E.bActive = !E.P.bDisabled;
		if (E.P.Kind == FArenaParticleParams::EKind::Mesh)
		{
			UStaticMesh* SM = E.P.MeshPath.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadStaticMeshByObjectPath(E.P.MeshPath);
			if (!SM)
			{
				E.bActive = false;
				continue;
			}
			const int32 N = FMath::Min(E.P.MaxParticles, 48);
			for (int32 i = 0; i < N; ++i)
			{
				UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
				C->SetStaticMesh(SM);
				C->SetMobility(EComponentMobility::Movable);
				C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				C->SetCastShadow(false);
				C->SetUsingAbsoluteLocation(true);
				C->SetUsingAbsoluteRotation(true);
				C->SetUsingAbsoluteScale(true);
				C->SetupAttachment(RootComponent);
				C->SetVisibility(false);
				C->RegisterComponent();
				E.MeshComps.Add(C);
				MeshParticleComps.Add(C);
				if (Sky)
				{
					Sky->AddSkyPrimitive(C);
				}
			}
			continue;
		}
		UTexture* Tex = LoadParticleTexture(E.P.TexturePath);
		UMaterialInterface* Parent = ParticleParent(E.P.Style);
		UMaterialInterface* Mat = nullptr;
		if (Parent && Tex)
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
			MID->SetTextureParameterValue(TEXT("Base"), Tex);
			Mat = MID;
		}
		else if (Parent && !E.P.TexturePath.IsEmpty())
		{
			// no particle material without a parent: a surface material would draw the particles opaque
			Mat = ArenaRuntimeContent::LoadMaterialByObjectPath(E.P.TexturePath);
		}
		if (!Mat)
		{
			E.bActive = false;
			continue;
		}
		// reach: start area + flight over the longest life (for visibility before any particle exists)
		{
			const FArenaParticleParams& P = E.P;
			const float Life = float(FMath::Max(P.LifetimeRange.X, P.LifetimeRange.Y));
			const float Speed = float(FMath::Max(P.StartVelocityMin.GetAbsMax(), P.StartVelocityMax.GetAbsMax())) + float(P.StartVelocityRadialRange.GetAbsMax());
			const float Reach = FMath::Min(6000.f, float(P.StartLocationMax.GetAbsMax() + P.StartLocationMin.GetAbsMax()) + float(P.SphereRadiusRange.GetAbsMax())
				+ Speed * Life + 0.5f * float(P.Acceleration.Size()) * Life * Life + float(P.StartSizeMax.GetMax()) * ArenaUE2World::Scale);
			E.Reach = FBox::BuildAABB(GetActorTransform().TransformPosition(P.StartLocationOffset), FVector(FMath::Max(Reach, 100.f)));
		}
		SecondsBeforeInactive = FMath::Max(SecondsBeforeInactive, E.P.SecondsBeforeInactive > 0.f ? E.P.SecondsBeforeInactive : 1.f);
		UArenaParticleComponent* C = NewObject<UArenaParticleComponent>(this);
		C->SetParticleMaterial(Mat);
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		E.Comp = C;
		ParticleComps.Add(C);
		if (Sky)
		{
			Sky->AddSkyPrimitive(C);
		}
	}
}

void AArenaEmitter::ResetEmitter(FEmitterState& E)
{
	for (FArenaParticle& P : E.Particles)
	{
		P.bAlive = false;
	}
	E.Spawned = 0;
	E.SpawnAccum = 0.f;
	E.bFinished = false;
	E.bInitialBurstDone = false;
	E.Delay = ArenaEmittersLocal::RandRange(Rng, E.P.InitialDelayRange);
}

void AArenaEmitter::SpawnParticle(FEmitterState& E, FArenaParticle& Pt)
{
	using namespace ArenaEmittersLocal;
	const FArenaParticleParams& P = E.P;
	const FTransform Xf = GetActorTransform();
	FVector Local = P.StartLocationOffset;
	switch (P.StartLocationShape)
	{
	case 1:
	{
		const FVector Dir = Rng.GetUnitVector();
		Local += Dir * RandRange(Rng, P.SphereRadiusRange) + RandIn(Rng, P.StartLocationMin, P.StartLocationMax);
		break;
	}
	case 2:
	{
		// polar: X = pitch-ish angle, Y = yaw-ish angle (turns of 65536), Z = radius
		const float A = float(Rng.FRandRange(P.StartLocationPolarMin.X, P.StartLocationPolarMax.X) * 2.0 * PI / 65536.0);
		const float B = float(Rng.FRandRange(P.StartLocationPolarMin.Y, P.StartLocationPolarMax.Y) * 2.0 * PI / 65536.0);
		const float Rd = float(Rng.FRandRange(P.StartLocationPolarMin.Z, P.StartLocationPolarMax.Z) * S);
		Local += FVector(FMath::Sin(A) * FMath::Cos(B), FMath::Sin(A) * FMath::Sin(B), FMath::Cos(A)) * Rd;
		break;
	}
	default:
		Local += RandIn(Rng, P.StartLocationMin, P.StartLocationMax);
		break;
	}
	Pt.Location = Xf.TransformPosition(Local);
	FVector Vel = RandIn(Rng, P.StartVelocityMin, P.StartVelocityMax);
	if (P.bGetVelocityDirectionFrom && !Local.IsNearlyZero())
	{
		Vel += Local.GetSafeNormal() * RandRange(Rng, P.StartVelocityRadialRange);
	}
	Pt.Velocity = Xf.TransformVectorNoScale(Vel);
	Pt.PrevLocation = Pt.Location;
	Pt.VelocityLoss = RandIn(Rng, P.VelocityLossMin, P.VelocityLossMax);
	Pt.StartSize = RandIn(Rng, P.StartSizeMin, P.StartSizeMax);
	if (P.bUniformSize)
	{
		Pt.StartSize = FVector(Pt.StartSize.X);
	}
	Pt.ColorMul = FLinearColor(Rng.FRandRange(P.ColorMultiplierMin.X, P.ColorMultiplierMax.X), Rng.FRandRange(P.ColorMultiplierMin.Y, P.ColorMultiplierMax.Y),
		Rng.FRandRange(P.ColorMultiplierMin.Z, P.ColorMultiplierMax.Z), 1.f);
	Pt.Age = 0.f;
	Pt.Life = FMath::Max(0.05f, RandRange(Rng, P.LifetimeRange));
	if (P.bSpinParticles)
	{
		const float Dir = Rng.FRand() < P.SpinCCWorCW ? 1.f : -1.f;
		Pt.Spin = RandRange(Rng, P.StartSpinRange) * 2.f * PI;
		Pt.SpinRate = Dir * RandRange(Rng, P.SpinsPerSecondRange) * 2.f * PI;
	}
	else
	{
		Pt.Spin = Pt.SpinRate = 0.f;
	}
	const int32 Frames = P.TextureUSubdivisions * P.TextureVSubdivisions;
	Pt.Subdivision = P.bUseRandomSubdivision && Frames > 1 ? Rng.RandRange(0, Frames - 1) : 0;
	if (P.Kind == FArenaParticleParams::EKind::Beam)
	{
		if (P.BeamSegments < 0)
		{
			const FVector Dir = Pt.Velocity.IsNearlyZero() ? Xf.GetRotation().GetForwardVector() : Pt.Velocity.GetSafeNormal();
			Pt.BeamEnd = Pt.Location + Dir * Rng.FRandRange(P.BeamEndMin.X, P.BeamEndMax.X);
		}
		else
		{
			Pt.BeamEnd = Xf.TransformPosition(RandIn(Rng, P.BeamEndMin, P.BeamEndMax));
		}
		Pt.Velocity = FVector::ZeroVector;
	}
	Pt.bAlive = true;
	++E.Spawned;
}

void AArenaEmitter::Simulate(FEmitterState& E, float Dt)
{
	const FArenaParticleParams& P = E.P;
	if (E.Particles.Num() != P.MaxParticles)
	{
		E.Particles.SetNum(P.MaxParticles);
	}
	// age and move
	for (FArenaParticle& Pt : E.Particles)
	{
		if (!Pt.bAlive)
		{
			continue;
		}
		Pt.Age += Dt;
		if (Pt.Age >= Pt.Life)
		{
			Pt.bAlive = false;
			continue;
		}
		Pt.PrevLocation = Pt.Location;
		Pt.Velocity += P.Acceleration * Dt;
		if (!Pt.VelocityLoss.IsNearlyZero())
		{
			Pt.Velocity *= FVector(FMath::Max(0.0, 1.0 - Pt.VelocityLoss.X * Dt), FMath::Max(0.0, 1.0 - Pt.VelocityLoss.Y * Dt), FMath::Max(0.0, 1.0 - Pt.VelocityLoss.Z * Dt));
		}
		if (!P.MaxAbsVelocity.IsNearlyZero())
		{
			for (int32 a = 0; a < 3; ++a)
			{
				if (P.MaxAbsVelocity[a] > 0.0)
				{
					Pt.Velocity[a] = FMath::Clamp(Pt.Velocity[a], -P.MaxAbsVelocity[a], P.MaxAbsVelocity[a]);
				}
			}
		}
		Pt.Location += Pt.Velocity * Dt;
		Pt.Spin += Pt.SpinRate * Dt;
	}
	if (!E.bActive || E.bFinished)
	{
		return;
	}
	if (E.Delay > 0.f)
	{
		E.Delay -= Dt;
		return;
	}
	// spawning
	const float AvgLife = FMath::Max(0.05f, float(P.LifetimeRange.X + P.LifetimeRange.Y) * 0.5f);
	float Rate = 0.f;
	if (!E.bInitialBurstDone && P.InitialParticlesPerSecond > 0.f)
	{
		Rate = P.InitialParticlesPerSecond;
		if (E.Spawned >= P.MaxParticles)
		{
			E.bInitialBurstDone = true;
		}
	}
	else if (P.ParticlesPerSecond > 0.f)
	{
		Rate = P.ParticlesPerSecond;
	}
	else if (P.bRespawnDeadParticles || E.Spawned < P.MaxParticles)
	{
		Rate = P.bAutomaticInitialSpawning || E.Spawned >= P.MaxParticles ? P.MaxParticles / AvgLife : 1.0e6f;
	}
	if (!P.bRespawnDeadParticles && E.Spawned >= P.MaxParticles)
	{
		Rate = 0.f;
		bool bAnyAlive = false;
		for (const FArenaParticle& Pt : E.Particles)
		{
			bAnyAlive |= Pt.bAlive;
		}
		if (!bAnyAlive)
		{
			E.bFinished = true;
			if (P.bAutoReset)
			{
				E.Delay = ArenaEmittersLocal::RandRange(Rng, P.AutoResetTimeRange);
				ResetEmitter(E);
				E.bFinished = false;
			}
		}
		return;
	}
	E.SpawnAccum += Rate * Dt;
	int32 Budget = FMath::Min(int32(E.SpawnAccum), P.MaxParticles);
	E.SpawnAccum -= float(int32(E.SpawnAccum));
	for (FArenaParticle& Pt : E.Particles)
	{
		if (Budget <= 0)
		{
			break;
		}
		if (!Pt.bAlive)
		{
			SpawnParticle(E, Pt);
			--Budget;
		}
	}
}

void AArenaEmitter::Draw(FEmitterState& E)
{
	const FArenaParticleParams& P = E.P;
	const int32 Frames = P.TextureUSubdivisions * P.TextureVSubdivisions;
	auto ColorAt = [&P](float T) -> FLinearColor
	{
		if (!P.bUseColorScale || P.ColorScale.Num() == 0)
		{
			return FLinearColor::White;
		}
		if (P.ColorScaleRepeats > 0.f)
		{
			T = FMath::Frac(T * (P.ColorScaleRepeats + 1.f));
		}
		if (T <= P.ColorScale[0].Key)
		{
			return P.ColorScale[0].Value;
		}
		for (int32 i = 1; i < P.ColorScale.Num(); ++i)
		{
			if (T <= P.ColorScale[i].Key)
			{
				const float A = (T - P.ColorScale[i - 1].Key) / FMath::Max(P.ColorScale[i].Key - P.ColorScale[i - 1].Key, 1e-4f);
				return FMath::Lerp(P.ColorScale[i - 1].Value, P.ColorScale[i].Value, A);
			}
		}
		return P.ColorScale.Last().Value;
	};
	auto SizeAt = [&P](float T) -> float
	{
		if (!P.bUseSizeScale || P.SizeScale.Num() == 0)
		{
			return 1.f;
		}
		if (P.SizeScaleRepeats > 0.f)
		{
			T = FMath::Frac(T * (P.SizeScaleRepeats + 1.f));
		}
		if (T <= P.SizeScale[0].Key)
		{
			return float(P.SizeScale[0].Value.X);
		}
		for (int32 i = 1; i < P.SizeScale.Num(); ++i)
		{
			if (T <= P.SizeScale[i].Key)
			{
				const float A = (T - P.SizeScale[i - 1].Key) / FMath::Max(P.SizeScale[i].Key - P.SizeScale[i - 1].Key, 1e-4f);
				return float(FMath::Lerp(P.SizeScale[i - 1].Value.X, P.SizeScale[i].Value.X, double(A)));
			}
		}
		return float(P.SizeScale.Last().Value.X);
	};

	if (P.Kind == FArenaParticleParams::EKind::Mesh)
	{
		for (int32 i = 0; i < E.MeshComps.Num(); ++i)
		{
			UStaticMeshComponent* C = E.MeshComps[i];
			const FArenaParticle* Pt = E.Particles.IsValidIndex(i) ? &E.Particles[i] : nullptr;
			if (!C)
			{
				continue;
			}
			if (!Pt || !Pt->bAlive)
			{
				C->SetVisibility(false);
				continue;
			}
			const float T = Pt->Age / Pt->Life;
			FVector Scale = Pt->StartSize * SizeAt(T);
			// UE2 mesh particles: StartSize is a scale (sizes above 10 are treated as percent)
			if (Scale.GetMax() > 10.0)
			{
				Scale /= 100.0;
			}
			C->SetWorldLocationAndRotation(Pt->Location, FRotator(0.f, FMath::RadiansToDegrees(Pt->Spin), 0.f));
			C->SetWorldScale3D(Scale * P.DrawScale);
			C->SetVisibility(true);
		}
		return;
	}
	if (!E.Comp)
	{
		return;
	}
	TArray<UArenaParticleComponent::FDrawParticle> Out;
	Out.Reserve(E.Particles.Num());
	FBox Bounds(ForceInit);
	for (const FArenaParticle& Pt : E.Particles)
	{
		if (!Pt.bAlive)
		{
			continue;
		}
		const float T = Pt.Age / Pt.Life;
		FLinearColor C = ColorAt(T) * Pt.ColorMul;
		C.A = ColorAt(T).A;
		float Alpha = P.Opacity;
		if (P.bFadeOut && Pt.Age > P.FadeOutStartTime)
		{
			const float F = 1.f - (Pt.Age - P.FadeOutStartTime) / FMath::Max(Pt.Life - P.FadeOutStartTime, 0.01f);
			Alpha *= FMath::Clamp(F, 0.f, 1.f) * P.FadeOutFactor.W + (1.f - P.FadeOutFactor.W);
			C.R *= FMath::Lerp(1.f, FMath::Clamp(F, 0.f, 1.f), P.FadeOutFactor.X);
			C.G *= FMath::Lerp(1.f, FMath::Clamp(F, 0.f, 1.f), P.FadeOutFactor.Y);
			C.B *= FMath::Lerp(1.f, FMath::Clamp(F, 0.f, 1.f), P.FadeOutFactor.Z);
		}
		if (P.bFadeIn && Pt.Age < P.FadeInEndTime)
		{
			const float F = Pt.Age / FMath::Max(P.FadeInEndTime, 0.01f);
			Alpha *= F * P.FadeInFactor.W + (1.f - P.FadeInFactor.W);
		}
		C.A *= Alpha;
		if (C.A <= 0.002f && P.Style != FArenaParticleParams::EStyle::Modulate)
		{
			continue;
		}
		UArenaParticleComponent::FDrawParticle& D = Out.AddDefaulted_GetRef();
		const float Sz = SizeAt(T) * P.DrawScale * (P.bUseRegularSizeScale || P.bUseSizeScale ? 1.f : 1.f);
		D.Location = FVector3f(Pt.Location);
		D.Size = FVector2f(float(Pt.StartSize.X) * Sz * ArenaUE2World::Scale, float(P.bUniformSize ? Pt.StartSize.X : Pt.StartSize.Y) * Sz * ArenaUE2World::Scale);
		D.Rotation = Pt.Spin;
		D.Color = FColor(uint8(FMath::Clamp(C.R, 0.f, 1.f) * 255.f), uint8(FMath::Clamp(C.G, 0.f, 1.f) * 255.f), uint8(FMath::Clamp(C.B, 0.f, 1.f) * 255.f),
			uint8(FMath::Clamp(C.A, 0.f, 1.f) * 255.f));
		// texture sheet frame
		int32 Frame = Pt.Subdivision;
		if (!P.bUseRandomSubdivision && Frames > 1)
		{
			const int32 First = FMath::Clamp(P.SubdivisionStart, 0, Frames - 1);
			const int32 Last = P.SubdivisionEnd > 0 ? FMath::Clamp(P.SubdivisionEnd - 1, First, Frames - 1) : Frames - 1;
			if (P.SubdivisionScale.Num() > 0)
			{
				Frame = 0;
				for (int32 i = 0; i < P.SubdivisionScale.Num(); ++i)
				{
					if (T >= P.SubdivisionScale[i])
					{
						Frame = i;
					}
				}
				Frame = FMath::Clamp(First + Frame, 0, Frames - 1);
			}
			else
			{
				Frame = First + FMath::Min(int32(T * (Last - First + 1)), Last - First);
			}
		}
		const float Du = 1.f / P.TextureUSubdivisions, Dv = 1.f / P.TextureVSubdivisions;
		const int32 Col = Frame % P.TextureUSubdivisions, Row = Frame / P.TextureUSubdivisions;
		D.UV = FVector4f(Col * Du, Row * Dv, (Col + 1) * Du, (Row + 1) * Dv);
		D.Axis = FVector3f::ZeroVector;
		if (P.Kind == FArenaParticleParams::EKind::Beam)
		{
			D.Axis = FVector3f(Pt.BeamEnd - Pt.Location);
			D.UV.Z = D.UV.X + Du * FMath::Max(1.f, P.BeamTextureUScale);
			Bounds += Pt.BeamEnd;
		}
		else if (P.Kind == FArenaParticleParams::EKind::Spark)
		{
			const float Seg = float(P.TimeBetweenSegmentsRange.X + P.TimeBetweenSegmentsRange.Y) * 0.5f;
			D.Axis = FVector3f(-Pt.Velocity * FMath::Clamp(Seg * 2.f, 0.02f, 0.2f));
			D.Location = FVector3f(Pt.Location) + D.Axis * -0.f;
			D.Size.Y = FMath::Max(1.5f, D.Size.Y * 0.1f);
		}
		else if (P.Facing == 2 && !Pt.Velocity.IsNearlyZero())
		{
			D.Axis = FVector3f(Pt.Velocity.GetSafeNormal()) * D.Size.Y * 2.f;
			D.Location -= D.Axis * 0.5f;
		}
		Bounds += Pt.Location;
	}
	const uint8 Facing = P.Kind == FArenaParticleParams::EKind::Beam || P.Kind == FArenaParticleParams::EKind::Spark ? 2 : P.Facing;
	if (E.Reach.IsValid)
	{
		Bounds += E.Reach;
	}
	if (Bounds.IsValid)
	{
		float MaxSize = 0.f;
		for (const UArenaParticleComponent::FDrawParticle& D : Out)
		{
			MaxSize = FMath::Max3(MaxSize, D.Size.X, D.Size.Y);
		}
		Bounds = Bounds.ExpandBy(MaxSize * 1.5f);
	}
	E.Comp->SetParticles(MoveTemp(Out), Facing, Bounds);
}

void AArenaEmitter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ArenaEmittersLocal::CVarEmitters.GetValueOnGameThread() == 0)
	{
		for (FEmitterState& E : Emitters)
		{
			if (E.Comp) E.Comp->SetVisibility(false);
		}
		return;
	}
	// pause far away (sky box emitters always run: they are seen through the sky box)
	const float MaxDist = GetMaxDistance();
	FVector View;
	const bool bSky = Tags.Contains(FName(TEXT("ArenaSkyZone")));
	if (!bSky && MaxDist > 0.f && ArenaArtFX::GetViewLocation(GetWorld(), View) && FVector::DistSquared(View, GetActorLocation()) > FMath::Square(MaxDist))
	{
		return;
	}
	// UE2 SecondsBeforeInactive: not seen for a while -> pause (the reach bounds keep it testable for visibility)
	if (!bSky && !IsRecentlyVisible())
	{
		PausedTime += DeltaSeconds;
		return;
	}
	// catch up a little after a pause so continuous effects (steam, fire) are not empty when seen again
	const float Dt = FMath::Min(DeltaSeconds + FMath::Min(PausedTime, 0.5f), 0.25f);
	PausedTime = 0.f;
	bool bAllFinished = Emitters.Num() > 0;
	for (FEmitterState& E : Emitters)
	{
		Simulate(E, Dt);
		Draw(E);
		bAllFinished &= E.bFinished || !E.bActive;
	}
	if (bAutoReset && bAllFinished)
	{
		if (ResetTimer < 0.f)
		{
			ResetTimer = ArenaEmittersLocal::RandRange(Rng, TimeTillResetRange);
		}
		ResetTimer -= Dt;
		if (ResetTimer <= 0.f)
		{
			ResetTimer = -1.f;
			for (FEmitterState& E : Emitters)
			{
				if (!E.P.bDisabled)
				{
					ResetEmitter(E);
				}
			}
		}
	}
}

bool AArenaEmitter::IsRecentlyVisible() const
{
	if (GetWorld() && GetWorld()->GetTimeSeconds() - CreationTime < 2.f)
	{
		return true;
	}
	for (const UArenaParticleComponent* C : ParticleComps)
	{
		if (C && C->WasRecentlyRendered(SecondsBeforeInactive))
		{
			return true;
		}
	}
	for (const UStaticMeshComponent* C : MeshParticleComps)
	{
		if (C && C->WasRecentlyRendered(SecondsBeforeInactive))
		{
			return true;
		}
	}
	return ParticleComps.Num() == 0 && MeshParticleComps.Num() == 0;
}

void AArenaEmitter::Trigger(AActor* Other, APawn* EventInstigator)
{
	for (FEmitterState& E : Emitters)
	{
		if (E.P.bResetOnTrigger)
		{
			ResetEmitter(E);
			E.bActive = true;
		}
		else if (!E.P.bTriggerDisabled)
		{
			E.bActive = !E.bActive;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// AArenaAmbientSound
// ---------------------------------------------------------------------------------------------

AArenaAmbientSound::AArenaAmbientSound()
{
	PrimaryActorTick.bCanEverTick = false;
	Audio = CreateDefaultSubobject<UAudioComponent>(TEXT("Audio"));
	RootComponent = Audio;
	Audio->bAutoActivate = false;
	Audio->bAllowSpatialization = true;
	Audio->bOverrideAttenuation = true;
	SetHidden(true);
}

void AArenaAmbientSound::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	Tag = Desc.Tag;
	const FString Path = Desc.Props.Str(TEXT("AmbientSound"));
	USoundBase* Snd = Path.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadSoundByObjectPath(Path);
	Setup(Snd, float(Desc.Props.Num(TEXT("SoundRadius"), 64.0)), uint8(FMath::Clamp(Desc.Props.Num(TEXT("SoundVolume"), 128.0), 0.0, 255.0)),
		uint8(FMath::Clamp(Desc.Props.Num(TEXT("SoundPitch"), 64.0), 1.0, 255.0)), Desc.IsA(TEXT("AmbientSound")) ? 0.25f : 1.f);
}

void AArenaAmbientSound::Setup(USoundBase* Sound, float InRadius, uint8 InVolume, uint8 InPitch, float ClassVolume)
{
	AmbientSound = Sound;
	SoundRadius = InRadius;
	SoundVolume = InVolume;
	SoundPitch = InPitch;
	if (!Sound)
	{
		return;
	}
	// UE2 ambient sounds: audible within 25 * (SoundRadius + 1) units, linear falloff. Volume: SoundVolume / 255 times
	// the class volume (AmbientSound.AmbientVolume, Default.ini 0.25) and ALAudio's AmbientVolume (0.5), which takes the
	// place of the effects volume (0.3, the output stage's mix level) for ambient loops
	const float Radius = 25.f * (SoundRadius + 1.f) * ArenaUE2World::Scale;
	FSoundAttenuationSettings& Att = Audio->AttenuationOverrides;
	Att.bAttenuate = true;
	Att.bSpatialize = true;
	Att.AttenuationShape = EAttenuationShape::Sphere;
	Att.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	Att.AttenuationShapeExtents = FVector(Radius * 0.1f, 0.f, 0.f);
	Att.FalloffDistance = Radius * 0.9f;
	Audio->SetSound(Sound);
	Audio->SetVolumeMultiplier(SoundVolume / 255.f * ClassVolume * (0.5f / 0.3f));
	Audio->SetPitchMultiplier(FMath::Clamp(SoundPitch / 64.f, 0.25f, 4.f));
	Audio->bIsUISound = false;
	Audio->Play(FMath::FRand() * 2.f);
}

#include "ArenaImportedLighting.h"
#include "OpenUT2k4.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Engine/TextureCube.h"
#include "TextureResource.h"
#include "ArenaContent.h"
#include "ArenaRayTracing.h"
#include "GameFramework/PlayerStart.h"
#include "CollisionQueryParams.h"

namespace
{
	// ---- defaults (see the header for what each does) ----
	/** Skylight leaking fraction every imported map gets (the floor for zones with AmbientBrightness 0). */
	constexpr float DefaultLeakBase = 0.20f;
	/** Additional leaking per 16 points of average zone AmbientBrightness. */
	constexpr float DefaultLeakPerAmbient16 = 0.06f;
	constexpr float MaxLeak = 0.45f;
	/** How much of the zone ambient hue goes into the leak tint (0 = white, 1 = full zone color). */
	constexpr float DefaultTintStrength = 0.5f;
	/** Multiplier on the converted UT2004 light intensities. */
	constexpr float DefaultLightScale = 1.0f;
	/** Ambient fill (SkyLight with a flat cubemap) intensity for zone AmbientBrightness 0, and per 16 points. */
	constexpr float DefaultFillBase = 0.6f;
	constexpr float DefaultFillPerAmbient16 = 0.3f;
	/** Multiplier on the level SkyLight intensity. */
	constexpr float DefaultSkyScale = 1.0f;
	/** Histogram auto exposure clamp, EV100 (manual exposure as imported = EV100 0, lower = brighter). */
	constexpr float DefaultExposureMinEV = -1.0f;
	constexpr float DefaultExposureMaxEV = 0.25f;
	constexpr float DefaultExposureBias = 0.0f;
	/**
	 * Full Ray Tracing shadows the map lights, which UT2004 levels were not lit for (their lightmaps were much
	 * softer), so interiors get darker: let the auto exposure open this much further (EV) in that mode.
	 */
	constexpr float FullRayTracingExtraExposureEV = -0.5f;
	/**
	 * Runtime levels with software Lumen (no ray tracing): the runtime-built meshes have no distance fields, so
	 * every Lumen ray that leaves the screen reaches the sky and the ambient fill lights occluded areas at full
	 * strength (brighter, flatter and tinted compared to the cooked levels). The fill is scaled by how open the
	 * level is (MeasureOpenness): enclosed levels keep SoftwareIndoorFillScale, open-air levels the full fill. Measured
	 * against the cooked levels (DM-Rankin, CTF-Face3, ONS-Torlan, DM-Deck17).
	 */
	constexpr float SoftwareIndoorFillScale = 0.15f;
	/** Openness (fraction of sky-facing probe rays that escape) at and above which a level counts as open air
	 * (measured: DM-Rankin and DM-Deck17 0.00, CTF-Face3 0.16, ONS-Torlan 0.72; spawn points are mostly indoors). */
	constexpr float SoftwareOpenAirOpenness = 0.15f;
	/** Probe rays per sample point and their length (cm). */
	constexpr int32 OpennessRays = 48;
	constexpr float OpennessRayLength = 40000.f;

	/** What Apply set up for the current level, so Refresh can re-tune it when the ray tracing setting changes. */
	struct FAppliedLevel
	{
		TWeakObjectPtr<UWorld> World;
		float SkyIntensity = 0.f;
		/** MeasureOpenness result (runtime levels only), -1 = not measured. */
		float Openness = -1.f;
		TArray<TWeakObjectPtr<USkyLightComponent>> Skies;
		/** Unbound post process volumes and their auto exposure lower limit (normal, Full Ray Tracing). */
		TArray<TWeakObjectPtr<APostProcessVolume>> Volumes;
		float MinBrightness = 0.f;
		float FullMinBrightness = 0.f;
		bool bAutoExposure = false;
		/** The level's own local lights and whether they cast shadows as built (Full Ray Tracing turns shadows on). */
		TArray<TPair<TWeakObjectPtr<ULocalLightComponent>, bool>> Lights;
	};
	FAppliedLevel AppliedLevel;

	float Opt(const TCHAR* Name, float Default)
	{
		float V = Default;
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), Name, V);
#endif
		return V;
	}

	/**
	 * Small HDR cubemap for the ambient fill: bright from above, dimmer from the sides, darkest from below, so
	 * the fill still has some shape (UT2004 ambient was completely flat).
	 */
	UTextureCube* MakeAmbientCubemap()
	{
		constexpr int32 Size = 8;
		UTextureCube* Tex = UTextureCube::CreateTransient(Size, Size, PF_FloatRGBA, TEXT("ArenaAmbientCube"));
		if (!Tex)
		{
			return nullptr;
		}
		Tex->SRGB = false;
		Tex->CompressionSettings = TC_HDR;
		FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
		FFloat16Color* Data = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		// Face order +X -X +Y -Y +Z -Z
		const float FaceValue[6] = { 0.8f, 0.8f, 0.8f, 0.8f, 1.0f, 0.45f };
		for (int32 Face = 0; Face < 6; ++Face)
		{
			for (int32 I = 0; I < Size * Size; ++I)
			{
				Data[Face * Size * Size + I] = FFloat16Color(FLinearColor(FaceValue[Face], FaceValue[Face], FaceValue[Face], 1.f));
			}
		}
		Mip.BulkData.Unlock();
		Tex->UpdateResource();
		return Tex;
	}

	/**
	 * How open to the sky a level is: the fraction of cosine-distributed upward rays from the player starts and
	 * pickups that hit no collision. Runtime levels with software Lumen use it to scale the ambient fill, which no
	 * distance field occludes there.
	 */
	float MeasureOpenness(UWorld* World)
	{
		TArray<FVector> Points;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->IsA<APlayerStart>() || It->GetClass()->GetName().Contains(TEXT("Pickup")))
			{
				Points.Add(It->GetActorLocation() + FVector(0.f, 0.f, 60.f));
			}
		}
		if (Points.Num() == 0)
		{
			return -1.f;
		}
		constexpr int32 MaxPoints = 64;
		const int32 Step = FMath::Max(1, Points.Num() / MaxPoints);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ArenaOpenness), true);
		// the level's sky surfaces (UE2 FakeBackdrop) are solid but a ray reaching them sees the sky
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(FName(TEXT("ArenaFakeBackdrop"))))
			{
				Params.AddIgnoredActor(*It);
			}
		}
		int32 Escaped = 0, Total = 0;
		for (int32 P = 0; P < Points.Num(); P += Step)
		{
			for (int32 R = 0; R < OpennessRays; ++R)
			{
				// Fibonacci spiral over the disc, lifted onto the hemisphere: cosine-weighted directions.
				const float U = (R + 0.5f) / OpennessRays;
				const float Phi = R * 2.39996323f + P;
				const float Radius = FMath::Sqrt(U);
				const FVector Dir(Radius * FMath::Cos(Phi), Radius * FMath::Sin(Phi), FMath::Sqrt(1.f - U));
				Escaped += World->LineTraceTestByChannel(Points[P], Points[P] + Dir * OpennessRayLength, ECC_Visibility, Params) ? 0 : 1;
				++Total;
			}
		}
		return Total > 0 ? float(Escaped) / Total : -1.f;
	}

	/** UE2 HSV (hue 0..255, saturation 0..255 where 255 = white) to a linear color with max channel 1. */
	FLinearColor UE2Color(double Hue, double Saturation)
	{
		const FLinearColor HSV(float(FMath::Clamp(Hue, 0.0, 255.0) / 255.0 * 360.0), float(1.0 - FMath::Clamp(Saturation, 0.0, 255.0) / 255.0), 1.f);
		const FLinearColor Srgb = HSV.HSVToLinearRGB(); // HSV math happens on the gamma encoded value in UE2
		return FLinearColor(FMath::Pow(Srgb.R, 2.2f), FMath::Pow(Srgb.G, 2.2f), FMath::Pow(Srgb.B, 2.2f));
	}
}

void ArenaImportedLighting::Apply(UWorld* World, const TSharedPtr<FJsonObject>& Root)
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("ImpOff")))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ImportedLighting: disabled (-ImpOff)"));
		return;
	}
#endif
	if (!World || !Root.IsValid())
	{
		return;
	}

	// ---- zone ambient (non sky zones), averaged ----
	double AmbSum = 0.0, Weight = 0.0;
	FLinearColor TintSum(0.f, 0.f, 0.f, 0.f);
	const TSharedPtr<FJsonObject>* Raw = nullptr;
	if (Root->TryGetObjectField(TEXT("raw_markers"), Raw) && Raw && Raw->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Zones = nullptr;
		if ((*Raw)->TryGetArrayField(TEXT("zones"), Zones))
		{
			for (const TSharedPtr<FJsonValue>& V : *Zones)
			{
				const TSharedPtr<FJsonObject> Z = V.IsValid() ? V->AsObject() : nullptr;
				bool bSky = false;
				if (!Z.IsValid() || (Z->TryGetBoolField(TEXT("sky_zone"), bSky) && bSky))
				{
					continue;
				}
				double Amb = 0.0, Hue = 0.0, Sat = 255.0;
				Z->TryGetNumberField(TEXT("ambient_brightness"), Amb);
				Z->TryGetNumberField(TEXT("ambient_hue"), Hue);
				Z->TryGetNumberField(TEXT("ambient_saturation"), Sat);
				// A few maps use 255 for special effect zones; cap so they do not dominate the map average.
				Amb = FMath::Clamp(Amb, 0.0, 48.0);
				AmbSum += Amb;
				Weight += 1.0;
				if (Amb > 0.0)
				{
					TintSum += UE2Color(Hue, Sat) * float(Amb);
				}
			}
		}
	}
	const float Ambient = Weight > 0.0 ? float(AmbSum / Weight) : 0.f;
	FLinearColor Tint = FLinearColor::White;
	if (TintSum.GetMax() > 0.f)
	{
		// Half strength: the leak is already colored by the sky, and full UE2 hues get garish in a linear pipeline.
		const FLinearColor ZoneTint = TintSum / TintSum.GetMax();
		Tint = FLinearColor::LerpUsingHSV(FLinearColor::White, ZoneTint, FMath::Clamp(Opt(TEXT("ImpTint="), DefaultTintStrength), 0.f, 1.f));
		Tint /= FMath::Max(Tint.GetMax(), 0.01f);
	}

	const float Leak = FMath::Clamp(Opt(TEXT("ImpLeak="), DefaultLeakBase) + Opt(TEXT("ImpLeakAmb="), DefaultLeakPerAmbient16) * Ambient / 16.f, 0.f, MaxLeak);
	const float LightScale = Opt(TEXT("ImpLightScale="), DefaultLightScale);
	const float SkyScale = Opt(TEXT("ImpSkyScale="), DefaultSkyScale);
	const bool bAuto = Opt(TEXT("ImpAuto="), 1.f) > 0.5f;
	const float ExpMin = Opt(TEXT("ImpExpMin="), DefaultExposureMinEV);
	const float ExpMax = Opt(TEXT("ImpExpMax="), DefaultExposureMaxEV);
	const float Bias = Opt(TEXT("ImpBias="), DefaultExposureBias);

	// Min / max brightness are EV100 with the extended luminance range (project default), else plain luminance.
	static const IConsoleVariable* ExtendedRangeVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"));
	const bool bExtendedRange = !ExtendedRangeVar || ExtendedRangeVar->GetInt() == 1;
	const float MinBrightness = bExtendedRange ? ExpMin : FMath::Exp2(ExpMin);
	const float MaxBrightness = bExtendedRange ? FMath::Max(ExpMax, ExpMin + 0.01f) : FMath::Exp2(FMath::Max(ExpMax, ExpMin + 0.01f));

	// ---- post process: the level's unbound volume(s) ----
	int32 NumPP = 0;
	TArray<TWeakObjectPtr<APostProcessVolume>> Volumes;
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		APostProcessVolume* PP = *It;
		if (!PP->bUnbound)
		{
			continue;
		}
		FPostProcessSettings& S = PP->Settings;
		S.bOverride_LumenSkylightLeaking = 1;
		S.LumenSkylightLeaking = Leak;
		S.bOverride_LumenSkylightLeakingTint = 1;
		S.LumenSkylightLeakingTint = Tint;
		S.bOverride_AutoExposureBias = 1;
		S.AutoExposureBias = Bias;
		if (bAuto)
		{
			S.bOverride_AutoExposureMethod = 1;
			S.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
			S.bOverride_AutoExposureApplyPhysicalCameraExposure = 1;
			S.AutoExposureApplyPhysicalCameraExposure = 0;
			S.bOverride_AutoExposureMinBrightness = 1;
			S.AutoExposureMinBrightness = MinBrightness;
			S.bOverride_AutoExposureMaxBrightness = 1;
			S.AutoExposureMaxBrightness = MaxBrightness;
			S.bOverride_AutoExposureSpeedUp = 1;
			S.AutoExposureSpeedUp = 2.f;
			S.bOverride_AutoExposureSpeedDown = 1;
			S.AutoExposureSpeedDown = 1.5f;
			// Ignore the darkest / brightest pixels (black voids, sky, light sprites) when metering.
			S.bOverride_AutoExposureLowPercent = 1;
			S.AutoExposureLowPercent = 50.f;
			S.bOverride_AutoExposureHighPercent = 1;
			S.AutoExposureHighPercent = 90.f;
		}
		Volumes.Add(PP);
		++NumPP;
	}

	// ---- sky light: replace the (nearly black) captured atmosphere with a flat ambient cubemap ----
	// The fill is the UT2004 zone ambient: full strength where Lumen sees the sky, the leak fraction elsewhere.
	int32 NumSky = 0;
	TArray<TWeakObjectPtr<USkyLightComponent>> Skies;
	float SkyIntensity = 0.f;
	const bool bFill = Opt(TEXT("ImpFill="), 1.f) > 0.5f;
	const float FillIntensity = SkyScale * (Opt(TEXT("ImpFillBase="), DefaultFillBase) + Opt(TEXT("ImpFillAmb="), DefaultFillPerAmbient16) * Ambient / 16.f);
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		USkyLightComponent* C = It->GetLightComponent();
		if (!C)
		{
			continue;
		}
		C->UnregisterComponent();
		if (bFill)
		{
			C->SourceType = SLS_SpecifiedCubemap;
			C->Cubemap = MakeAmbientCubemap();
			C->bRealTimeCapture = false;
			C->bLowerHemisphereIsBlack = false;
			C->Intensity = FillIntensity;
			C->LightColor = Tint.ToFColor(true);
		}
		else
		{
			C->Intensity *= SkyScale;
		}
		C->RegisterComponent();
		C->SetCaptureIsDirty();
		Skies.Add(C);
		SkyIntensity = C->Intensity;
		++NumSky;
	}

	// ---- converted UT2004 lights ----
	int32 NumLights = 0;
	TArray<TPair<TWeakObjectPtr<ULocalLightComponent>, bool>> Lights;
	for (TObjectIterator<ULocalLightComponent> It; It; ++It)
	{
		ULocalLightComponent* L = *It;
		if (L->GetWorld() == World && L->Mobility == EComponentMobility::Movable && !L->IsTemplate())
		{
			Lights.Emplace(L, L->CastShadows != 0);
			if (!FMath::IsNearlyEqual(LightScale, 1.f))
			{
				L->SetIntensity(L->Intensity * LightScale);
				++NumLights;
			}
		}
	}

	UE_LOG(LogOpenUT2k4, Log, TEXT("ImportedLighting: zone ambient %.1f tint %s -> skylight leak %.3f; exposure %s [%.2f, %.2f]%s bias %.2f; light scale %.2f (%d lights); fill %s %.3f, sky scale %.2f (%d); %d post process volumes"),
		Ambient, *Tint.ToString(), Leak, bAuto ? TEXT("histogram") : TEXT("manual"), ExpMin, ExpMax, bExtendedRange ? TEXT(" EV100") : TEXT(" (legacy luminance range)"), Bias, LightScale, NumLights, bFill ? TEXT("on") : TEXT("off"), FillIntensity, SkyScale, NumSky, NumPP);

	AppliedLevel.World = World;
	AppliedLevel.SkyIntensity = SkyIntensity;
	AppliedLevel.Skies = MoveTemp(Skies);
	AppliedLevel.Lights = MoveTemp(Lights);
	AppliedLevel.Volumes = MoveTemp(Volumes);
	AppliedLevel.bAutoExposure = bAuto;
	AppliedLevel.MinBrightness = MinBrightness;
	const float FullExpMin = ExpMin + Opt(TEXT("ImpFullExp="), FullRayTracingExtraExposureEV);
	AppliedLevel.FullMinBrightness = bExtendedRange ? FullExpMin : FMath::Exp2(FullExpMin);
	AppliedLevel.Openness = ArenaContent::UseRuntimeContent() ? MeasureOpenness(World) : -1.f;
	Refresh();
}

void ArenaImportedLighting::Refresh()
{
	UWorld* World = AppliedLevel.World.Get();
	if (!World)
	{
		return;
	}
	// Cooked levels have distance fields (software Lumen sees them) and ray tracing sees every level: no change.
	const bool bCompensate = ArenaContent::UseRuntimeContent() && !ArenaRayTracing::IsActive();
	const float Open = FMath::SmoothStep(0.f, SoftwareOpenAirOpenness, FMath::Max(AppliedLevel.Openness, 0.f));
	const float FillScale = bCompensate ? Opt(TEXT("ImpSwFill="), FMath::Lerp(SoftwareIndoorFillScale, 1.f, Open)) : 1.f;
	for (const TWeakObjectPtr<USkyLightComponent>& Sky : AppliedLevel.Skies)
	{
		if (Sky.IsValid())
		{
			Sky->SetIntensity(AppliedLevel.SkyIntensity * FillScale);
		}
	}
	// Full Ray Tracing: every map light casts (MegaLights, ray traced) shadows; otherwise as built (UT2004 lights
	// are unshadowed: their shadows came from lightmaps).
	const bool bFull = ArenaRayTracing::IsFullActive();
	int32 NumShadowed = 0;
	for (const TPair<TWeakObjectPtr<ULocalLightComponent>, bool>& Light : AppliedLevel.Lights)
	{
		if (ULocalLightComponent* L = Light.Key.Get())
		{
			L->SetCastShadows(bFull || Light.Value);
			NumShadowed += L->CastShadows ? 1 : 0;
		}
	}
	if (AppliedLevel.bAutoExposure)
	{
		for (const TWeakObjectPtr<APostProcessVolume>& PP : AppliedLevel.Volumes)
		{
			if (PP.IsValid())
			{
				PP->Settings.AutoExposureMinBrightness = bFull ? AppliedLevel.FullMinBrightness : AppliedLevel.MinBrightness;
			}
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("ImportedLighting: %s Lumen%s on a %s level (openness %.2f) -> fill x%.2f, %d of %d map lights cast shadows"),
		ArenaRayTracing::IsActive() ? TEXT("ray traced") : TEXT("software"), bFull ? TEXT(" + full ray tracing") : TEXT(""),
		ArenaContent::UseRuntimeContent() ? TEXT("runtime") : TEXT("cooked"), AppliedLevel.Openness, FillScale, NumShadowed, AppliedLevel.Lights.Num());
}

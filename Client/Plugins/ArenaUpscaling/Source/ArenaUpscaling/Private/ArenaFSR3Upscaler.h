#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "Templates/RefCounting.h"
#include "TemporalUpscaler.h"

/**
 * AMD FidelityFX Super Resolution 3.1 (FSR 3.1.4) as an Unreal third-party temporal upscaler.
 *
 * The FidelityFX SDK's host side (ffx_fsr3upscaler.cpp: constants, resource ping-pong, dispatch order) is reimplemented
 * here on top of RDG, and its GPU passes (Shaders/Private/FidelityFX, MIT) are compiled by Unreal's shader compiler, so
 * the same code runs on Vulkan (Linux, Windows), D3D12 and Metal. No AMD binaries are involved.
 *
 * Installed per view family by FArenaFSRViewExtension when FSR is selected; the renderer then calls AddPasses instead of
 * TSR. Output is written at the view family's secondary (display) resolution, before the HUD and menus are drawn.
 */
class FArenaFSR3Upscaler final : public UE::Renderer::Private::ITemporalUpscaler
{
public:
	struct FSettings
	{
		/** RCAS sharpening 0..1; <= 0 skips the sharpening pass. */
		float Sharpness = 0.f;
		/** Frame time in seconds (FSR uses it to scale accumulation on long frames). */
		float DeltaTimeSeconds = 1.f / 60.f;
	};

	/** Persistent per-view state: the "previous frame" textures and the constants FSR carries between frames. */
	class FHistory final : public IHistory, public TRefCountingMixin<FHistory>
	{
	public:
		virtual const TCHAR* GetDebugName() const override { return FArenaFSR3Upscaler::DebugName(); }
		virtual uint64 GetGPUSizeBytes() const override;
		virtual void AddRef() const override { TRefCountingMixin<FHistory>::AddRef(); }
		virtual FReturnedRefCountValue Release() const override { return TRefCountingMixin<FHistory>::Release(); }
		virtual FReturnedRefCountValue GetRefCount() const override { return TRefCountingMixin<FHistory>::GetRefCount(); }

		TRefCountPtr<IPooledRenderTarget> Accumulation;
		TRefCountPtr<IPooledRenderTarget> Luma;
		TRefCountPtr<IPooledRenderTarget> LumaHistory;
		TRefCountPtr<IPooledRenderTarget> InternalUpscaled;
		TRefCountPtr<IPooledRenderTarget> FrameInfo;

		FIntPoint RenderExtent = FIntPoint::ZeroValue;
		FIntPoint UpscaleExtent = FIntPoint::ZeroValue;
		FIntPoint RenderSize = FIntPoint::ZeroValue;
		FIntPoint UpscaleSize = FIntPoint::ZeroValue;
		FVector2f Jitter = FVector2f::ZeroVector;
		float PreExposure = 1.f;
		float FrameIndex = 0.f;
		float JitterPhaseCount = 0.f;
	};

	explicit FArenaFSR3Upscaler(const FSettings& InSettings) : Settings(InSettings) {}

	static const TCHAR* DebugName() { return TEXT("FSR3"); }

	// ITemporalUpscaler
	virtual const TCHAR* GetDebugName() const override { return DebugName(); }
	virtual FOutputs AddPasses(FRDGBuilder& GraphBuilder, const FSceneView& View, const FInputs& Inputs) const override;
	virtual float GetMinUpsampleResolutionFraction() const override { return 0.25f; }
	virtual float GetMaxUpsampleResolutionFraction() const override { return 1.f; }
	virtual ITemporalUpscaler* Fork_GameThread(const class FSceneViewFamily& ViewFamily) const override { return new FArenaFSR3Upscaler(Settings); }

private:
	FSettings Settings;
};

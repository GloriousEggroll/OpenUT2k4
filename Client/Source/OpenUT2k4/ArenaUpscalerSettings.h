#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "ArenaUpscalerSettings.generated.h"

/**
 * Upscaling and latency settings (UArenaUserSettings::Upscaler, UpscalerQuality, FSRSharpness, LowLatency), applied
 * through the ArenaUpscaling plugin (Plugins/ArenaUpscaling, see Docs/Upscaling.md):
 *
 *  - Upscaler: TSR (Unreal's Temporal Super Resolution), FSR (AMD FSR 3.1, compiled from the FidelityFX SDK source for
 *    every RHI), or Off (native resolution, FXAA, no temporal reconstruction).
 *  - Quality: the render resolution of TSR and FSR. Auto keeps Unreal's display-based default (lower resolution on large
 *    displays); Native AA renders at 100 % and only uses the upscaler for anti-aliasing; Quality 67 %, Balanced 59 %,
 *    Performance 50 %, Ultra Performance 33 % (AMD's per-axis ratios 1.5x, 1.7x, 2x, 3x).
 *  - FSR Sharpness: RCAS sharpening after FSR (0 = off).
 *  - Low Latency: AMD Anti-Lag (VK_AMD_anti_lag) where the Vulkan driver provides it natively (AMD's driver, RADV in
 *    Mesa 26.3+), otherwise engine frame pacing (the game thread waits for the render thread every frame).
 *
 * The scene is upscaled before the HUD and menus are drawn, so they always render at native resolution.
 * Non-shipping test overrides: -ArenaUpscaler=TSR|FSR|Off, -ArenaUpscalerQuality=0..5, -ArenaFSRSharpness=0..1,
 * -ArenaLowLatency=0|1.
 */
namespace ArenaUpscalerSettings
{
	constexpr int32 UpscalerTSR = 0;
	constexpr int32 UpscalerFSR = 1;
	constexpr int32 UpscalerOff = 2;
	constexpr int32 NumUpscalers = 3;
	/** Auto, Native AA, Quality, Balanced, Performance, Ultra Performance */
	constexpr int32 NumQualities = 6;

	/** Push the saved settings to the renderer and the plugin. Called at startup and whenever one of them changes. */
	void Apply();

	FString GetUpscalerText(int32 Choice);
	FString GetQualityText(int32 Choice);
	FString GetSharpnessText(float Sharpness);
	FString GetLowLatencyText(int32 Choice);

	/** Screen percentage for a quality choice (0 = Unreal's automatic default). */
	float GetScreenPercentage(int32 Quality);
}

/** Applies the upscaler and low latency settings when the engine starts (before the first frame). */
UCLASS()
class OPENUT2K4_API UArenaUpscalerSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
};

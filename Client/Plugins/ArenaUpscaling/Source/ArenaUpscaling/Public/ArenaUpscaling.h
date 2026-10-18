#pragma once

#include "CoreMinimal.h"

/**
 * OpenUT2k4 upscaling and latency features (plugin ArenaUpscaling). All of it is optional and degrades gracefully:
 *
 *  - FSR 3.1 temporal upscaler: AMD FidelityFX Super Resolution 3.1.4 compiled from the MIT FidelityFX SDK source by
 *    Unreal's shader compiler, so it runs on any SM5+ GPU and RHI (Vulkan on Linux/Windows, Metal on Mac). Replaces TSR
 *    for game views while enabled; the resolution is set by r.ScreenPercentage as for TSR.
 *  - Low latency: VK_AMD_anti_lag on the Vulkan RHI where the driver provides it natively (AMD's driver, RADV from Mesa
 *    26.3). The driver delays the start of each frame so the CPU does not run ahead of the GPU, cutting input-to-photon
 *    latency when GPU bound. Elsewhere it falls back to engine frame pacing (r.OneFrameThreadLag 0).
 *
 * Console variables (also usable for testing): r.Arena.FSR (0/1), r.Arena.FSR.Sharpness (0..1), r.Arena.LowLatency (0/1).
 * Startup: -ArenaLowLatency=0/1 overrides the saved setting; -ArenaMesaAntiLagLayer=1 loads Mesa's anti-lag layer.
 */
namespace ArenaUpscaling
{
	/** True when FSR can run (the RHI supports SM5 compute; false on mobile feature levels). */
	ARENAUPSCALING_API bool IsFSRSupported();
	/** Select FSR for game views (applied from the next frame). Sharpness 0..1 (0 = no RCAS sharpening pass). */
	ARENAUPSCALING_API void SetFSR(bool bEnabled, float Sharpness);
	/** True while FSR is selected and supported. */
	ARENAUPSCALING_API bool IsFSREnabled();

	enum class ELowLatencyState : uint8
	{
		Off,
		/** VK_AMD_anti_lag drives the frame start (AMD driver, RADV in Mesa 26.3+). */
		AntiLag,
		/** No usable anti-lag extension: engine frame pacing (r.OneFrameThreadLag 0) on any RHI. */
		FramePacing,
	};

	/** Turn low latency on or off (applied live from the next frame). */
	ARENAUPSCALING_API void SetLowLatency(bool bEnabled);
	ARENAUPSCALING_API ELowLatencyState GetLowLatencyState();
	/** VK_AMD_anti_lag is enabled on the device and usable. */
	ARENAUPSCALING_API bool IsAntiLagAvailable();
	/** Human readable backend, e.g. "AMD Anti-Lag (VK_AMD_anti_lag)", or the fallback and why. */
	ARENAUPSCALING_API FString GetLowLatencyDescription();
	/**
	 * Average time from the start of a frame's input sampling to its present call over the last second (ms), measured
	 * by the plugin on every RHI. 0 before the first measurement. Lower with anti-lag on when the GPU is the bottleneck.
	 */
	ARENAUPSCALING_API float GetInputToPresentMs();
}

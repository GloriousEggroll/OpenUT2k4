#pragma once

#include "CoreMinimal.h"
#include "ArenaUpscaling.h"

/**
 * Low latency through VK_AMD_anti_lag on the Vulkan RHI.
 *
 * Before the RHI starts (PreRHIInit, called from the module's PostConfigInit startup) the device extension is requested
 * from the Vulkan RHI. It is used when the driver provides it natively (AMD's driver; RADV from Mesa 26.3). Mesa <= 26.2
 * only offers it through VK_LAYER_MESA_anti_lag, which corrupts the timeline semaphore values of vkQueueSubmit and makes
 * Unreal lose the device, so that layer is not used (opt in with -ArenaMesaAntiLagLayer=1 to test a fixed one).
 * Without the extension, low latency falls back to engine frame pacing on every RHI: r.OneFrameThreadLag 0 (the game
 * thread waits for the render thread every frame, so input is sampled closer to rendering).
 * At runtime two engine modular features drive anti-lag:
 *  - IMaxTickRateHandlerModule::HandleMaxTickRate, called at the start of every frame before input is sampled:
 *    vkAntiLagUpdateAMD(stage INPUT, maxFPS = frame rate cap). The driver sleeps here so the frame starts "just in time".
 *  - ILatencyMarkerModule::SetPresentLatencyMarkerStart, called by the Vulkan RHI right before vkQueuePresentKHR:
 *    vkAntiLagUpdateAMD(stage PRESENT) with the same frame index.
 * Both features also time input sampling to present for every RHI (GetInputToPresentMs).
 */
namespace ArenaAntiLag
{
	/** Request the Vulkan extension / layer. Must run before the RHI is created. */
	void PreRHIInit();
	/** Register the modular features (after the engine is up). */
	void Startup();
	void Shutdown();

	void SetEnabled(bool bEnabled);
	ArenaUpscaling::ELowLatencyState GetState();
	bool IsAntiLagAvailable();
	FString GetDescription();
	float GetInputToPresentMs();
}

#include "ArenaAntiLag.h"
#include "ArenaUpscalingLog.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Features/IModularFeatures.h"
#include "Performance/LatencyMarkerModule.h"
#include "Performance/MaxTickRateHandlerModule.h"
#include "RHI.h"
#include "DynamicRHI.h"

#if ARENA_UPSCALING_WITH_VULKAN
#include "IVulkanDynamicRHI.h"
#endif

namespace ArenaAntiLag
{
	static TAutoConsoleVariable<int32> CVarLowLatency(
		TEXT("r.Arena.LowLatency"), 0,
		TEXT("Low latency through VK_AMD_anti_lag (Vulkan RHI): 0 = off, 1 = on. Set by the game's Low Latency setting."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarLowLatencyLog(
		TEXT("r.Arena.LowLatency.LogInterval"), 10,
		TEXT("Seconds between low latency status lines in the log (input-to-present time, anti-lag calls); 0 = never."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarAllowMesaLayer(
		TEXT("r.Arena.LowLatency.AllowMesaLayer"), 0,
		TEXT("Allow VK_AMD_anti_lag from Mesa's VK_LAYER_MESA_anti_lag (Mesa <= 26.2). Off by default: that layer rewrites ")
		TEXT("timeline semaphore values of vkQueueSubmit wrongly and Unreal's Vulkan RHI loses the device. Read at startup; ")
		TEXT("-ArenaMesaAntiLagLayer=1 sets it."),
		ECVF_Default);

	/** Low latency choice read from the saved settings before the RHI starts. */
	[[maybe_unused]] static bool bWantedAtStartup = false;
	/** We asked the loader for VK_LAYER_MESA_anti_lag (only with -ArenaMesaAntiLagLayer=1). */
	[[maybe_unused]] static bool bLayerRequested = false;
	/** The player's environment enables the Mesa layer (ENABLE_LAYER_MESA_ANTI_LAG=1). */
	[[maybe_unused]] static bool bLayerFromEnvironment = false;
	/** Set by the Vulkan RHI's feature callback while creating the device: VK_AMD_anti_lag gets enabled. */
	[[maybe_unused]] static bool bDeviceExtensionAccepted = false;

	struct FRuntime
	{
		bool bResolved = false;
		bool bVulkan = false;
		/** VK_AMD_anti_lag is enabled on the device and safe to use. */
		bool bExtensionLoaded = false;
		/** Enabled, but provided by the Mesa 26.2 layer that breaks Unreal's submissions: not used. */
		bool bBlockedMesaLayer = false;
		/** Anti-lag currently driving the frame start. */
		bool bActive = false;
		/** Engine frame pacing fallback currently applied (r.OneFrameThreadLag 0). */
		bool bEnginePacing = false;
		FString SavedOneFrameThreadLag;
		uint64 InputCalls = 0;
		uint64 PresentCalls = 0;
#if ARENA_UPSCALING_WITH_VULKAN
		VkDevice Device = VK_NULL_HANDLE;
		PFN_vkAntiLagUpdateAMD AntiLagUpdate = nullptr;
#endif
	};
	static FRuntime Runtime;

	// ---- input-to-present timing (every RHI) ----
	static constexpr int32 TimingSlots = 64;
	static double InputTimes[TimingSlots] = {};
	static uint64 InputFrames[TimingSlots] = {};
	static double LatencySumMs = 0.0;
	static int32 LatencyCount = 0;
	static float LastAverageMs = 0.f;
	static double LastLogTime = 0.0;
	static FCriticalSection TimingLock;

#if ARENA_UPSCALING_WITH_VULKAN
	class FAntiLagDeviceExtension final : public FVulkanRHIExternalDeviceExtensionBase
	{
	public:
		FAntiLagDeviceExtension() : FVulkanRHIExternalDeviceExtensionBase(VK_AMD_ANTI_LAG_EXTENSION_NAME)
		{
			FMemory::Memzero(Features);
			Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
		}
		virtual VkBaseOutStructure* GetFeaturesStruct() override { return reinterpret_cast<VkBaseOutStructure*>(&Features); }
		virtual bool FeaturesCallback() override
		{
			UE_LOG(LogArenaUpscaling, Log, TEXT("VK_AMD_anti_lag: device reports antiLag feature = %d"), int32(Features.antiLag));
			bDeviceExtensionAccepted = Features.antiLag == VK_TRUE;
			return bDeviceExtensionAccepted;
		}
		virtual void DuplicateCallback() override { bDeviceExtensionAccepted = true; }

		VkPhysicalDeviceAntiLagFeaturesAMD Features;
	};

	static bool IsVulkanRHI()
	{
		return GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan;
	}
#endif

	static void Resolve()
	{
		if (Runtime.bResolved || !GDynamicRHI)
		{
			return;
		}
		Runtime.bResolved = true;
#if ARENA_UPSCALING_WITH_VULKAN
		if (IsVulkanRHI() && bDeviceExtensionAccepted)
		{
			Runtime.bVulkan = true;
			IVulkanDynamicRHI* Vulkan = GetIVulkanDynamicRHI();
			// The RHI's list of loaded extension names does not keep externally requested names alive after device
			// creation, so rely on our own extension object: the RHI enabled it if the feature check passed (or the
			// engine already enables it itself).
			Runtime.Device = Vulkan->RHIGetVkDevice();
			Runtime.AntiLagUpdate = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(Vulkan->RHIGetVkDeviceProcAddr("vkAntiLagUpdateAMD"));
			Runtime.bExtensionLoaded = Runtime.AntiLagUpdate != nullptr;
			// Mesa <= 26.2 only has the extension through its layer, which corrupts the timeline semaphore values of
			// vkQueueSubmit (it appends its own semaphore but shifts the value array): Unreal's Vulkan RHI then loses
			// the device on the first anti-lag frame. Native RADV support (Mesa 26.3+) and AMD's driver are fine.
			if (Runtime.bExtensionLoaded && (bLayerRequested || bLayerFromEnvironment) && CVarAllowMesaLayer.GetValueOnAnyThread() == 0)
			{
				Runtime.bExtensionLoaded = false;
				Runtime.bBlockedMesaLayer = true;
			}
		}
		else if (IsVulkanRHI())
		{
			Runtime.bVulkan = true;
		}
#endif
		UE_LOG(LogArenaUpscaling, Log, TEXT("Low latency: RHI=%s, VK_AMD_anti_lag usable=%s%s, Mesa layer requested=%s / from environment=%s, setting=%s"),
			GDynamicRHI->GetName(), Runtime.bExtensionLoaded ? TEXT("yes") : TEXT("no"),
			Runtime.bBlockedMesaLayer ? TEXT(" (enabled through Mesa's anti-lag layer, which loses the device with Unreal: not used)") : TEXT(""),
			bLayerRequested ? TEXT("yes") : TEXT("no"), bLayerFromEnvironment ? TEXT("yes") : TEXT("no"),
			CVarLowLatency.GetValueOnAnyThread() != 0 ? TEXT("on") : TEXT("off"));
	}

	/** Fallback on every RHI: the game thread waits for the render thread each frame, so input is sampled later. */
	static void SetEnginePacing(bool bOn)
	{
		if (bOn == Runtime.bEnginePacing)
		{
			return;
		}
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.OneFrameThreadLag"));
		if (!Var)
		{
			return;
		}
		Runtime.bEnginePacing = bOn;
		if (bOn)
		{
			Runtime.SavedOneFrameThreadLag = Var->GetString();
			Var->Set(0, ECVF_SetByCode);
		}
		else
		{
			Var->Set(*Runtime.SavedOneFrameThreadLag, ECVF_SetByCode);
		}
		UE_LOG(LogArenaUpscaling, Log, TEXT("Low latency frame pacing (r.OneFrameThreadLag %s) %s"), bOn ? TEXT("0") : *Runtime.SavedOneFrameThreadLag, bOn ? TEXT("enabled") : TEXT("disabled"));
	}

	static void LogStatus(double Now)
	{
		const int32 Interval = CVarLowLatencyLog.GetValueOnAnyThread();
		if (Interval <= 0 || Now - LastLogTime < Interval)
		{
			return;
		}
		LastLogTime = Now;
		UE_LOG(LogArenaUpscaling, Log, TEXT("Low latency %s: input-to-present %.2f ms (1 s average), vkAntiLagUpdateAMD calls: %llu input, %llu present"),
			Runtime.bActive ? TEXT("ON (anti-lag)") : Runtime.bEnginePacing ? TEXT("ON (frame pacing)") : TEXT("off"), LastAverageMs, Runtime.InputCalls, Runtime.PresentCalls);
	}

	/** Engine hook at the start of each frame, before input is sampled (FEngineLoop::Tick -> UpdateTimeAndHandleMaxTickRate). */
	class FMaxTickRateHandler final : public IMaxTickRateHandlerModule
	{
	public:
		virtual void Initialize() override {}
		virtual void SetEnabled(bool bInEnabled) override { ArenaAntiLag::SetEnabled(bInEnabled); }
		virtual bool GetEnabled() override { return Runtime.bActive; }
		virtual bool GetAvailable() override { return Runtime.bExtensionLoaded; }
		virtual void SetFlags(uint32) override {}
		virtual uint32 GetFlags() override { return 0; }

		virtual bool HandleMaxTickRate(float DesiredMaxTickRate) override
		{
			Resolve();
			const bool bWant = CVarLowLatency.GetValueOnGameThread() != 0;
			SetEnginePacing(bWant && !Runtime.bExtensionLoaded);
#if ARENA_UPSCALING_WITH_VULKAN
			if (Runtime.bExtensionLoaded)
			{
				if (bWant != Runtime.bActive)
				{
					Runtime.bActive = bWant;
					UE_LOG(LogArenaUpscaling, Log, TEXT("Low latency (VK_AMD_anti_lag) %s"), bWant ? TEXT("enabled") : TEXT("disabled"));
					if (!bWant)
					{
						VkAntiLagDataAMD Data;
						FMemory::Memzero(Data);
						Data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
						Data.mode = VK_ANTI_LAG_MODE_OFF_AMD;
						Runtime.AntiLagUpdate(Runtime.Device, &Data);
					}
				}
				if (Runtime.bActive)
				{
					VkAntiLagPresentationInfoAMD Presentation;
					FMemory::Memzero(Presentation);
					Presentation.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
					Presentation.stage = VK_ANTI_LAG_STAGE_INPUT_AMD;
					Presentation.frameIndex = GFrameCounter;
					VkAntiLagDataAMD Data;
					FMemory::Memzero(Data);
					Data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
					Data.mode = VK_ANTI_LAG_MODE_ON_AMD;
					Data.maxFPS = DesiredMaxTickRate > 0.f ? uint32(FMath::RoundToInt(DesiredMaxTickRate)) : 0u;
					Data.pPresentationInfo = &Presentation;
					// Sleeps in the driver until the frame should start (and enforces maxFPS).
					Runtime.AntiLagUpdate(Runtime.Device, &Data);
					++Runtime.InputCalls;
					return true;
				}
			}
#endif
			return false;
		}
	};

	/** Engine latency markers: input sampling (game thread) and present (RHI thread, right before vkQueuePresentKHR). */
	class FLatencyMarkers final : public ILatencyMarkerModule
	{
	public:
		virtual void Initialize() override {}
		virtual void SetEnabled(bool bInEnabled) override { ArenaAntiLag::SetEnabled(bInEnabled); }
		virtual bool GetEnabled() override { return Runtime.bActive; }
		virtual bool GetAvailable() override { return Runtime.bExtensionLoaded; }
		virtual void SetFlashIndicatorEnabled(bool) override {}
		virtual bool GetFlashIndicatorEnabled() override { return false; }

		virtual void SetInputSampleLatencyMarker(uint64 FrameNumber) override
		{
			FScopeLock Lock(&TimingLock);
			const int32 Slot = int32(FrameNumber % TimingSlots);
			InputTimes[Slot] = FPlatformTime::Seconds();
			InputFrames[Slot] = FrameNumber;
		}
		virtual void SetSimulationLatencyMarkerStart(uint64) override {}
		virtual void SetSimulationLatencyMarkerEnd(uint64) override {}
		virtual void SetPresentLatencyMarkerStart(uint64 FrameNumber) override
		{
#if ARENA_UPSCALING_WITH_VULKAN
			if (Runtime.bActive && Runtime.AntiLagUpdate)
			{
				VkAntiLagPresentationInfoAMD Presentation;
				FMemory::Memzero(Presentation);
				Presentation.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
				Presentation.stage = VK_ANTI_LAG_STAGE_PRESENT_AMD;
				Presentation.frameIndex = FrameNumber;
				VkAntiLagDataAMD Data;
				FMemory::Memzero(Data);
				Data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
				Data.mode = VK_ANTI_LAG_MODE_ON_AMD;
				Data.pPresentationInfo = &Presentation;
				Runtime.AntiLagUpdate(Runtime.Device, &Data);
				++Runtime.PresentCalls;
			}
#endif
			const double Now = FPlatformTime::Seconds();
			FScopeLock Lock(&TimingLock);
			const int32 Slot = int32(FrameNumber % TimingSlots);
			if (InputFrames[Slot] == FrameNumber && InputTimes[Slot] > 0.0)
			{
				LatencySumMs += (Now - InputTimes[Slot]) * 1000.0;
				++LatencyCount;
			}
			static double WindowStart = Now;
			if (Now - WindowStart >= 1.0 && LatencyCount > 0)
			{
				LastAverageMs = float(LatencySumMs / LatencyCount);
				LatencySumMs = 0.0;
				LatencyCount = 0;
				WindowStart = Now;
				LogStatus(Now);
			}
		}
		virtual void SetPresentLatencyMarkerEnd(uint64) override {}
		virtual void SetRenderSubmitLatencyMarkerStart(uint64) override {}
		virtual void SetRenderSubmitLatencyMarkerEnd(uint64) override {}
		virtual void SetFlashIndicatorLatencyMarker(uint64) override {}
		virtual void SetCustomLatencyMarker(uint32, uint64) override {}

		virtual float GetTotalLatencyInMs() override { return LastAverageMs; }
		virtual float GetGameLatencyInMs() override { return LastAverageMs; }
		virtual float GetRenderLatencyInMs() override { return 0.f; }
		virtual float GetSimulationLatencyInMs() override { return 0.f; }
		virtual float GetRenderSubmitLatencyInMs() override { return 0.f; }
		virtual float GetPresentLatencyInMs() override { return 0.f; }
		virtual float GetDriverLatencyInMs() override { return 0.f; }
		virtual float GetOSRenderQueueLatencyInMs() override { return 0.f; }
		virtual float GetGPURenderLatencyInMs() override { return 0.f; }
		virtual float GetRenderSubmitOffsetFromFrameStartInMs() override { return 0.f; }
		virtual float GetPresentOffsetFromFrameStartInMs() override { return 0.f; }
		virtual float GetDriverOffsetFromFrameStartInMs() override { return 0.f; }
		virtual float GetOSRenderQueueOffsetFromFrameStartInMs() override { return 0.f; }
		virtual float GetGPURenderOffsetFromFrameStartInMs() override { return 0.f; }
	};

	static TUniquePtr<FMaxTickRateHandler> TickRateHandler;
	static TUniquePtr<FLatencyMarkers> LatencyMarkers;

	void PreRHIInit()
	{
		int32 Saved = 0;
		if (GConfig)
		{
			GConfig->GetInt(TEXT("/Script/OpenUT2k4.ArenaUserSettings"), TEXT("LowLatency"), Saved, GGameIni);
		}
		int32 Override = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("-ArenaLowLatency="), Override))
		{
			Saved = Override;
		}
		bWantedAtStartup = Saved != 0;
		CVarLowLatency->Set(bWantedAtStartup ? 1 : 0, ECVF_SetByCode);

#if ARENA_UPSCALING_WITH_VULKAN
		// The device extension is harmless when unused, so always ask for it: drivers with native support (AMD on
		// Windows, RADV from Mesa 26.3) can then be switched on live.
		TArray<TSharedRef<FVulkanRHIExternalDeviceExtensionBase>> Extensions;
		Extensions.Add(MakeShared<FAntiLagDeviceExtension>());
		IVulkanDynamicRHI::AddEnabledDeviceExtensionsAndLayers(Extensions, TArrayView<const ANSICHAR* const>());
#if PLATFORM_LINUX
		bLayerFromEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("ENABLE_LAYER_MESA_ANTI_LAG")) == TEXT("1");
		// Mesa's implicit anti-lag layer (Mesa <= 26.2) is off unless ENABLE_LAYER_MESA_ANTI_LAG=1, and it breaks Unreal's
		// queue submissions (see Resolve). Only load it on explicit request, for testing a fixed layer.
		int32 AllowLayer = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("-ArenaMesaAntiLagLayer="), AllowLayer) && AllowLayer != 0)
		{
			CVarAllowMesaLayer->Set(1, ECVF_SetByCommandline);
			static const ANSICHAR* const Layers[] = { "VK_LAYER_MESA_anti_lag" };
			IVulkanDynamicRHI::AddEnabledInstanceExtensionsAndLayers(TArrayView<const ANSICHAR* const>(), TArrayView<const ANSICHAR* const>(Layers, 1));
			bLayerRequested = true;
		}
#endif
#endif
		UE_LOG(LogArenaUpscaling, Log, TEXT("Low latency at startup: %s (Vulkan anti-lag extension requested: %s, Mesa layer requested: %s)"),
			bWantedAtStartup ? TEXT("on") : TEXT("off"), ARENA_UPSCALING_WITH_VULKAN ? TEXT("yes") : TEXT("no"), bLayerRequested ? TEXT("yes") : TEXT("no"));
	}

	void Startup()
	{
		TickRateHandler = MakeUnique<FMaxTickRateHandler>();
		LatencyMarkers = MakeUnique<FLatencyMarkers>();
		IModularFeatures::Get().RegisterModularFeature(IMaxTickRateHandlerModule::GetModularFeatureName(), TickRateHandler.Get());
		IModularFeatures::Get().RegisterModularFeature(ILatencyMarkerModule::GetModularFeatureName(), LatencyMarkers.Get());
	}

	void Shutdown()
	{
		if (TickRateHandler)
		{
			IModularFeatures::Get().UnregisterModularFeature(IMaxTickRateHandlerModule::GetModularFeatureName(), TickRateHandler.Get());
			TickRateHandler.Reset();
		}
		if (LatencyMarkers)
		{
			IModularFeatures::Get().UnregisterModularFeature(ILatencyMarkerModule::GetModularFeatureName(), LatencyMarkers.Get());
			LatencyMarkers.Reset();
		}
	}

	void SetEnabled(bool bEnabled)
	{
		CVarLowLatency->Set(bEnabled ? 1 : 0, ECVF_SetByCode);
	}

	ArenaUpscaling::ELowLatencyState GetState()
	{
		Resolve();
		if (CVarLowLatency.GetValueOnAnyThread() == 0)
		{
			return ArenaUpscaling::ELowLatencyState::Off;
		}
		return Runtime.bExtensionLoaded ? ArenaUpscaling::ELowLatencyState::AntiLag : ArenaUpscaling::ELowLatencyState::FramePacing;
	}

	bool IsAntiLagAvailable()
	{
		Resolve();
		return Runtime.bExtensionLoaded;
	}

	FString GetDescription()
	{
		Resolve();
		if (Runtime.bExtensionLoaded)
		{
			return TEXT("AMD Anti-Lag (VK_AMD_anti_lag)");
		}
		if (Runtime.bBlockedMesaLayer)
		{
			return TEXT("frame pacing (Mesa's anti-lag layer is incompatible; native RADV anti-lag needs Mesa 26.3+)");
		}
		if (Runtime.bVulkan)
		{
			return TEXT("frame pacing (driver has no VK_AMD_anti_lag)");
		}
		return TEXT("frame pacing (VK_AMD_anti_lag needs the Vulkan renderer)");
	}

	float GetInputToPresentMs()
	{
		return LastAverageMs;
	}
}

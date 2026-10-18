#include "ArenaUpscaling.h"
#include "ArenaAntiLag.h"
#include "ArenaFSR3Upscaler.h"
#include "ArenaUpscalingLog.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "SceneView.h"
#include "SceneUtils.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogArenaUpscaling);

static TAutoConsoleVariable<int32> CVarArenaFSR(
	TEXT("r.Arena.FSR"), 0,
	TEXT("Use AMD FSR 3.1 instead of TSR as the temporal upscaler for game views (1), or not (0). The render resolution is r.ScreenPercentage."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarArenaFSRSharpness(
	TEXT("r.Arena.FSR.Sharpness"), 0.f,
	TEXT("FSR RCAS sharpening, 0..1 (0 = off)."),
	ECVF_Default);

/**
 * Installs the FSR upscaler on game view families while it is selected. Views that do not use a temporal anti-aliasing
 * method are left alone (the renderer only calls third-party upscalers in place of TAA/TSR).
 */
class FArenaFSRViewExtension final : public FSceneViewExtensionBase
{
public:
	FArenaFSRViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}

	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override
	{
		if (InViewFamily.GetTemporalUpscalerInterface() != nullptr || InViewFamily.Views.Num() == 0)
		{
			return;
		}
		for (const FSceneView* View : InViewFamily.Views)
		{
			if (!View || !IsTemporalAccumulationBasedMethod(View->AntiAliasingMethod))
			{
				return;
			}
		}
		FArenaFSR3Upscaler::FSettings Settings;
		Settings.Sharpness = FMath::Clamp(CVarArenaFSRSharpness.GetValueOnGameThread(), 0.f, 1.f);
		Settings.DeltaTimeSeconds = InViewFamily.Time.GetDeltaRealTimeSeconds();
		InViewFamily.SetTemporalUpscalerInterface(new FArenaFSR3Upscaler(Settings));

		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogArenaUpscaling, Log, TEXT("FSR 3.1 upscaler active (sharpness %.2f)"), Settings.Sharpness);
		}
	}

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
	{
		const UWorld* World = Context.GetWorld();
		return World && World->IsGameWorld() && ArenaUpscaling::IsFSREnabled();
	}
};

class FArenaUpscalingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ArenaUpscaling"));
		if (Plugin.IsValid())
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/ArenaUpscaling"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
		ArenaAntiLag::PreRHIInit();
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]()
		{
			ViewExtension = FSceneViewExtensions::NewExtension<FArenaFSRViewExtension>();
			ArenaAntiLag::Startup();
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		ArenaAntiLag::Shutdown();
		ViewExtension.Reset();
	}

private:
	TSharedPtr<FArenaFSRViewExtension, ESPMode::ThreadSafe> ViewExtension;
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FArenaUpscalingModule, ArenaUpscaling)

namespace ArenaUpscaling
{
	bool IsFSRSupported()
	{
		return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
	}

	void SetFSR(bool bEnabled, float Sharpness)
	{
		CVarArenaFSR->Set(bEnabled ? 1 : 0, ECVF_SetByCode);
		CVarArenaFSRSharpness->Set(FMath::Clamp(Sharpness, 0.f, 1.f), ECVF_SetByCode);
	}

	bool IsFSREnabled()
	{
		return CVarArenaFSR.GetValueOnAnyThread() != 0 && IsFSRSupported();
	}

	void SetLowLatency(bool bEnabled) { ArenaAntiLag::SetEnabled(bEnabled); }
	ELowLatencyState GetLowLatencyState() { return ArenaAntiLag::GetState(); }
	bool IsAntiLagAvailable() { return ArenaAntiLag::IsAntiLagAvailable(); }
	FString GetLowLatencyDescription() { return ArenaAntiLag::GetDescription(); }
	float GetInputToPresentMs() { return ArenaAntiLag::GetInputToPresentMs(); }
}

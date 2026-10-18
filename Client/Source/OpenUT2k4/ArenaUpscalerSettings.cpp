#include "ArenaUpscalerSettings.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#if ARENA_WITH_UPSCALING
#include "ArenaUpscaling.h"
#else
// The dedicated server binary is built without the upscaler plugin (headless, OpenUT2k4.Build.cs): the same calls as no-ops.
namespace ArenaUpscaling
{
	inline bool IsFSRSupported() { return false; }
	inline void SetFSR(bool, float) {}
	inline void SetLowLatency(bool) {}
	inline bool IsAntiLagAvailable() { return false; }
}
#endif
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	int32 GetUpscalerChoice()
	{
		int32 Choice = UArenaUserSettings::Get()->Upscaler;
#if !UE_BUILD_SHIPPING
		FString Arg;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaUpscaler="), Arg))
		{
			Choice = Arg.Equals(TEXT("FSR"), ESearchCase::IgnoreCase) ? ArenaUpscalerSettings::UpscalerFSR
				: Arg.Equals(TEXT("Off"), ESearchCase::IgnoreCase) || Arg.Equals(TEXT("Native"), ESearchCase::IgnoreCase) ? ArenaUpscalerSettings::UpscalerOff
				: ArenaUpscalerSettings::UpscalerTSR;
		}
#endif
		if (Choice == ArenaUpscalerSettings::UpscalerFSR && !ArenaUpscaling::IsFSRSupported())
		{
			Choice = ArenaUpscalerSettings::UpscalerTSR;
		}
		return FMath::Clamp(Choice, 0, ArenaUpscalerSettings::NumUpscalers - 1);
	}

	int32 GetQualityChoice()
	{
		int32 Choice = UArenaUserSettings::Get()->UpscalerQuality;
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), TEXT("ArenaUpscalerQuality="), Choice);
#endif
		return FMath::Clamp(Choice, 0, ArenaUpscalerSettings::NumQualities - 1);
	}

	float GetSharpness()
	{
		float Sharpness = UArenaUserSettings::Get()->FSRSharpness;
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), TEXT("ArenaFSRSharpness="), Sharpness);
#endif
		return FMath::Clamp(Sharpness, 0.f, 1.f);
	}

	bool GetLowLatency()
	{
		int32 Choice = UArenaUserSettings::Get()->LowLatency;
		// Same switch the plugin reads before the RHI starts, so the layer request and the live state agree.
		FParse::Value(FCommandLine::Get(), TEXT("ArenaLowLatency="), Choice);
		return Choice != 0;
	}

	void SetCVar(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			if (Var->GetFloat() != Value)
			{
				Var->Set(Value, ECVF_SetByGameOverride);
			}
		}
	}
}

float ArenaUpscalerSettings::GetScreenPercentage(int32 Quality)
{
	switch (Quality)
	{
	case 1: return 100.f;
	case 2: return 100.f / 1.5f;
	case 3: return 100.f / 1.7f;
	case 4: return 50.f;
	case 5: return 100.f / 3.f;
	default: return 0.f;
	}
}

void ArenaUpscalerSettings::Apply()
{
	if (GIsEditor)
	{
		return;
	}
	const int32 Upscaler = GetUpscalerChoice();
	const int32 Quality = GetQualityChoice();
	if (Upscaler == UpscalerOff)
	{
		// Native resolution without temporal reconstruction: FXAA.
		SetCVar(TEXT("r.AntiAliasingMethod"), 1.f);
		SetCVar(TEXT("r.ScreenPercentage"), 100.f);
	}
	else
	{
		// TSR, also the anti-aliasing method FSR replaces (third-party upscalers run in place of TAA/TSR).
		SetCVar(TEXT("r.AntiAliasingMethod"), 4.f);
		SetCVar(TEXT("r.ScreenPercentage"), GetScreenPercentage(Quality));
	}
	ArenaUpscaling::SetFSR(Upscaler == UpscalerFSR, GetSharpness());
	ArenaUpscaling::SetLowLatency(GetLowLatency());
	UE_LOG(LogOpenUT2k4, Log, TEXT("Upscaler: %s, quality %s (r.ScreenPercentage %.1f), FSR sharpness %.2f, low latency %s"),
		*GetUpscalerText(Upscaler), *GetQualityText(Quality), Upscaler == UpscalerOff ? 100.f : GetScreenPercentage(Quality), GetSharpness(),
		*GetLowLatencyText(GetLowLatency() ? 1 : 0));
}

FString ArenaUpscalerSettings::GetUpscalerText(int32 Choice)
{
	switch (Choice)
	{
	case UpscalerFSR: return ArenaUpscaling::IsFSRSupported() ? TEXT("AMD FSR 3.1") : TEXT("AMD FSR 3.1 (unsupported)");
	case UpscalerOff: return TEXT("Off (native)");
	default: return TEXT("TSR");
	}
}

FString ArenaUpscalerSettings::GetQualityText(int32 Choice)
{
	static const TCHAR* const Names[NumQualities] = { TEXT("Auto"), TEXT("Native AA"), TEXT("Quality"), TEXT("Balanced"), TEXT("Performance"), TEXT("Ultra Performance") };
	const int32 Index = FMath::Clamp(Choice, 0, NumQualities - 1);
	if (Index == 0)
	{
		return Names[0];
	}
	return FString::Printf(TEXT("%s (%d%%)"), Names[Index], FMath::RoundToInt(GetScreenPercentage(Index)));
}

FString ArenaUpscalerSettings::GetSharpnessText(float Sharpness)
{
	const int32 Percent = FMath::RoundToInt(FMath::Clamp(Sharpness, 0.f, 1.f) * 100.f);
	return Percent == 0 ? FString(TEXT("Off")) : FString::Printf(TEXT("%d%%"), Percent);
}

FString ArenaUpscalerSettings::GetLowLatencyText(int32 Choice)
{
	if (Choice == 0)
	{
		return ArenaUpscaling::IsAntiLagAvailable() ? TEXT("Off") : TEXT("Off (no Anti-Lag)");
	}
	// AMD Anti-Lag where the driver offers VK_AMD_anti_lag, engine frame pacing everywhere else.
	return ArenaUpscaling::IsAntiLagAvailable() ? TEXT("On (AMD Anti-Lag)") : TEXT("On (frame pacing)");
}

void UArenaUpscalerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ArenaUpscalerSettings::Apply();
}

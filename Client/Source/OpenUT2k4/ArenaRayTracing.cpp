#include "ArenaRayTracing.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#include "ArenaImportedLighting.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderUtils.h"
#include "RHIGlobals.h"
#include "RHI.h"

namespace
{
	int32 GetChoice()
	{
		int32 Choice = UArenaUserSettings::Get()->RayTracing;
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), TEXT("ArenaRayTracing="), Choice);
#endif
		return FMath::Clamp(Choice, 0, ArenaRayTracing::NumChoices - 1);
	}

	/** Auto: on for discrete GPUs with ray tracing; integrated GPUs keep software Lumen for frame rate. */
	bool AutoWantsRayTracing()
	{
		return !GRHIDeviceIsIntegrated;
	}

	int32 GetFullChoice()
	{
		int32 Choice = UArenaUserSettings::Get()->FullRayTracing;
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), TEXT("ArenaFullRT="), Choice);
#endif
		return Choice != 0 ? 1 : 0;
	}

	/**
	 * Render settings of Full Ray Tracing. Each value is applied at game-override priority (above scalability,
	 * project and device profile settings; the console and the command line still win) and the previous value is
	 * remembered, so switching Full Ray Tracing off restores exactly what was there before.
	 */
	struct FFullCVar
	{
		const TCHAR* Name;
		const TCHAR* Value;
	};
	const FFullCVar FullCVars[] =
	{
		// Direct lighting of every local light through MegaLights with ray traced shadows: the map lights get
		// shadows at a fixed cost however many there are (ArenaImportedLighting turns their shadows on). The sun
		// stays on virtual shadow maps: through MegaLights (r.MegaLights.DirectionalLights) its patches of light
		// came out dimmer and unstable among hundreds of local lights, and its VSM shadows are already crisp.
		{ TEXT("r.MegaLights.EnableForProject"), TEXT("1") },
		{ TEXT("r.MegaLights.DefaultShadowMethod"), TEXT("0") },
		// Alpha masked materials (foliage, grates, fences) cast shadows with their mask.
		{ TEXT("r.MegaLights.HardwareRayTracing.EvaluateMaterialMode"), TEXT("1") },
		// Lumen hit lighting for reflections (2): reflection rays evaluate the material and lights where they hit.
		// Hit lighting for GI too (1) looked the same on our test maps at up to 1.6x the GPU time (DM-Deck17
		// 11.5 ms against 7.3 ms at 1080p on an RX 9070 XT), so GI keeps the surface cache.
		{ TEXT("r.Lumen.HardwareRayTracing.LightingMode"), TEXT("2") },
		// Reflections: trace rougher surfaces, two bounces between mirrors.
		{ TEXT("r.Lumen.Reflections.MaxRoughnessToTrace"), TEXT("0.6") },
		{ TEXT("r.Lumen.Reflections.MaxBounces"), TEXT("2") },
		// Ray traced reflections on the front layer of translucent surfaces (glass, water, shields).
		{ TEXT("r.Lumen.TranslucencyReflections.FrontLayer.Enable"), TEXT("1") },
	};
	/** Values the FullCVars had before Full Ray Tracing changed them (empty while it is off). */
	TMap<FString, FString> SavedFullCVars;

	void ApplyFullCVars(bool bFull)
	{
		for (const FFullCVar& Entry : FullCVars)
		{
			IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Entry.Name);
			if (!Var)
			{
				continue;
			}
			if (bFull)
			{
				if (!SavedFullCVars.Contains(Entry.Name))
				{
					SavedFullCVars.Add(Entry.Name, Var->GetString());
				}
				Var->Set(Entry.Value, ECVF_SetByGameOverride);
			}
			else if (const FString* Saved = SavedFullCVars.Find(Entry.Name))
			{
				// With cvar history (non-shipping) Unset hands the value back to whatever set it before (scalability,
				// project settings...); otherwise put the saved value back at our priority.
				Var->Unset(ECVF_SetByGameOverride);
				if (Var->GetString() != *Saved)
				{
					Var->Set(**Saved, ECVF_SetByGameOverride);
				}
			}
		}
		if (!bFull)
		{
			SavedFullCVars.Reset();
		}
	}

	void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			if (Var->GetInt() != Value)
			{
				Var->Set(Value, ECVF_SetByGameOverride);
			}
		}
	}
}

bool ArenaRayTracing::IsSupported()
{
	return IsRayTracingAllowed();
}

bool ArenaRayTracing::IsActive()
{
	if (!IsSupported())
	{
		return false;
	}
	const int32 Choice = GetChoice();
	return Choice == 1 || (Choice == 0 && AutoWantsRayTracing());
}

void ArenaRayTracing::Apply()
{
	if (GIsEditor)
	{
		return; // editor viewports keep the project settings
	}
	const bool bOn = IsActive();
	if (IsSupported())
	{
		// r.RayTracing.Enable also drops the ray tracing scene (BLAS / TLAS updates) when off.
		SetCVarInt(TEXT("r.RayTracing.Enable"), bOn ? 1 : 0);
	}
	SetCVarInt(TEXT("r.Lumen.HardwareRayTracing"), bOn ? 1 : 0);
	ApplyFullCVars(IsFullActive());
	UE_LOG(LogOpenUT2k4, Log, TEXT("RayTracing: setting %s, supported %s (RHI %s, inline %s, shaders %s, integrated GPU %s) -> %s Lumen; full ray tracing %s"),
		*GetChoiceText(GetChoice()), IsSupported() ? TEXT("yes") : TEXT("no"), GRHISupportsRayTracing ? TEXT("yes") : TEXT("no"),
		GRHISupportsInlineRayTracing ? TEXT("yes") : TEXT("no"), GRHISupportsRayTracingShaders ? TEXT("yes") : TEXT("no"),
		GRHIDeviceIsIntegrated ? TEXT("yes") : TEXT("no"), bOn ? TEXT("hardware ray traced") : TEXT("software"), *GetFullChoiceText(GetFullChoice()));
	ArenaImportedLighting::Refresh();
}

FString ArenaRayTracing::GetChoiceText(int32 Choice)
{
	if (!IsSupported())
	{
		return TEXT("Off (unsupported)");
	}
	switch (FMath::Clamp(Choice, 0, NumChoices - 1))
	{
	case 0: return AutoWantsRayTracing() ? TEXT("Auto (On)") : TEXT("Auto (Off)");
	case 1: return TEXT("On");
	default: return TEXT("Off");
	}
}

bool ArenaRayTracing::IsFullActive()
{
	return IsActive() && GetFullChoice() != 0;
}

bool ArenaRayTracing::CanUseFull()
{
	return IsActive();
}

FString ArenaRayTracing::GetFullChoiceText(int32 Choice)
{
	if (!CanUseFull())
	{
		return TEXT("Off (needs ray tracing)");
	}
	return Choice != 0 ? TEXT("On") : TEXT("Off");
}

void ArenaRayTracing::StartTestHooks()
{
#if !UE_BUILD_SHIPPING
	// -ArenaToggleFullRT=<sec>,<sec>,...: flip the Full Ray Tracing row at those world seconds, as the Settings menu
	// does (applies live and saves), for live toggle checks.
	FString Toggles;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaToggleFullRT="), Toggles, false))
	{
		TArray<FString> Parts;
		Toggles.ParseIntoArray(Parts, TEXT(","));
		TSharedRef<TArray<float>> Times = MakeShared<TArray<float>>();
		for (const FString& Part : Parts)
		{
			Times->Add(FCString::Atof(*Part));
		}
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Times](float)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				const UWorld* World = Context.World();
				if (World && World->IsGameWorld() && Times->Num() > 0 && World->GetTimeSeconds() >= (*Times)[0])
				{
					Times->RemoveAt(0);
					UArenaUserSettings::Get()->ChangeSetting(FName(TEXT("FullRayTracing")), 1);
					UE_LOG(LogOpenUT2k4, Log, TEXT("RayTracing: test toggle at %.1f s -> Full Ray Tracing %s"), World->GetTimeSeconds(),
						*GetFullChoiceText(UArenaUserSettings::Get()->FullRayTracing));
				}
			}
			return Times->Num() > 0;
		}));
	}

	// -ArenaExecAt=<sec>:<command>;<sec>:<command>...: run console commands at those world seconds (e.g. profilegpu).
	FString ExecAt;
	if (FParse::Value(FCommandLine::Get(), TEXT("ArenaExecAt="), ExecAt, false))
	{
		TArray<FString> Parts;
		ExecAt.ParseIntoArray(Parts, TEXT(";"));
		TSharedRef<TArray<TPair<float, FString>>> Cmds = MakeShared<TArray<TPair<float, FString>>>();
		for (const FString& Part : Parts)
		{
			FString When, Cmd;
			if (Part.Split(TEXT(":"), &When, &Cmd))
			{
				Cmds->Emplace(FCString::Atof(*When), Cmd);
			}
		}
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Cmds](float)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (World && World->IsGameWorld() && Cmds->Num() > 0 && World->GetTimeSeconds() >= (*Cmds)[0].Key)
				{
					const FString Cmd = (*Cmds)[0].Value;
					Cmds->RemoveAt(0);
					UE_LOG(LogOpenUT2k4, Log, TEXT("RayTracing: test exec at %.1f s: %s"), World->GetTimeSeconds(), *Cmd);
					GEngine->Exec(World, *Cmd);
				}
			}
			return Cmds->Num() > 0;
		}));
	}

	FString Range;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ArenaFrameStats="), Range))
	{
		return;
	}
	FString FromText, ToText;
	if (!Range.Split(TEXT("-"), &FromText, &ToText))
	{
		return;
	}
	const float From = FCString::Atof(*FromText);
	const float To = FCString::Atof(*ToText);
	struct FSamples
	{
		TArray<float> FrameMs;
		TArray<float> GpuMs;
	};
	TSharedRef<FSamples> Samples = MakeShared<FSamples>();
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Samples, From, To](float)
	{
		const UWorld* World = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() && Context.World()->IsGameWorld())
			{
				World = Context.World();
				break;
			}
		}
		const float Now = World ? World->GetTimeSeconds() : 0.f;
		if (Now >= From && Now < To)
		{
			Samples->FrameMs.Add(float(FApp::GetDeltaTime() * 1000.0));
			Samples->GpuMs.Add(float(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0))));
			return true;
		}
		if (Now < To || Samples->FrameMs.Num() == 0)
		{
			return true;
		}
		auto Summary = [](TArray<float>& V, float& Avg, float& Median, float& P99)
		{
			V.Sort();
			double Sum = 0.0;
			for (float X : V)
			{
				Sum += X;
			}
			Avg = float(Sum / V.Num());
			Median = V[V.Num() / 2];
			P99 = V[FMath::Min(V.Num() - 1, int32(V.Num() * 0.99f))];
		};
		float FA, FM, FP, GA, GM, GP;
		Summary(Samples->FrameMs, FA, FM, FP);
		Summary(Samples->GpuMs, GA, GM, GP);
		UE_LOG(LogOpenUT2k4, Log, TEXT("FrameStats: %d frames [%.0f s, %.0f s] ray tracing %s%s: frame avg %.2f ms (%.1f fps) median %.2f p99 %.2f; GPU avg %.2f median %.2f p99 %.2f ms"),
			Samples->FrameMs.Num(), From, To, ArenaRayTracing::IsActive() ? TEXT("on") : TEXT("off"), ArenaRayTracing::IsFullActive() ? TEXT(" (full)") : TEXT(""), FA, 1000.f / FMath::Max(FA, 0.001f), FM, FP, GA, GM, GP);
		return false;
	}));
#endif
}

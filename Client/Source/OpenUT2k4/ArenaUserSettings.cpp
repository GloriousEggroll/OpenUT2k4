#include "ArenaUserSettings.h"
#include "Engine/Engine.h"
#include "AudioDevice.h"
#include "ArenaArtHUD.h"
#include "ArenaDisplay.h"
#include "ArenaHDR.h"
#include "ArenaRayTracing.h"
#include "ArenaUpscalerSettings.h"

UArenaUserSettings::UArenaUserSettings()
{
}

UArenaUserSettings* UArenaUserSettings::Get()
{
	return GetMutableDefault<UArenaUserSettings>();
}

void UArenaUserSettings::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject) && FOVVersion < 1)
	{
		// Older saves stored the horizontal FOV at 16:9 (the camera's aspect). Keep the same view: same vertical
		// extent, expressed as horizontal degrees at 4:3 (tan(h43/2) = tan(h169/2) * 0.75), on the 5 degree grid.
		FOVVersion = 1;
		if (!FMath::IsNearlyEqual(FOV, 90.f))
		{
			const float Old = FMath::Clamp(FOV, 60.f, 150.f);
			const float New = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(Old * 0.5f)) * 0.75f));
			FOV = FMath::Clamp(FMath::RoundToFloat(New / 5.f) * 5.f, MinFOV, MaxFOV);
		}
	}
}

void UArenaUserSettings::Save()
{
	MouseSensitivity = FMath::Clamp(MouseSensitivity, MinSensitivity, MaxSensitivity);
	FOV = FMath::Clamp(FOV, MinFOV, MaxFOV);
	Brightness = FMath::Clamp(Brightness, MinBrightness, MaxBrightness);
	Volume = FMath::Clamp(Volume, 0.f, 1.f);
	MusicVolume = FMath::Clamp(MusicVolume, 0.f, 1.f);
	Crosshair = FMath::Clamp(Crosshair, 0, ArenaArtHUD::NumCrosshairs() - 1);
	HudScale = FMath::Clamp(HudScale, MinHudScale, MaxHudScale);
	HudWidth = FMath::Clamp(HudWidth, 0, 3);
	HudWidthPercent = FMath::Clamp(HudWidthPercent, 50.f, 100.f);
	Monitor = FMath::Max(0, Monitor);
	WindowMode = FMath::Clamp(WindowMode, 0, ArenaDisplay::NumWindowModes - 1);
	ResolutionX = FMath::Max(0, ResolutionX);
	ResolutionY = FMath::Max(0, ResolutionY);
	HDR = HDR != 0 ? 1 : 0;
	HDRPeakNits = FMath::Clamp(HDRPeakNits, 0, 10000);
	HDRPaperWhiteNits = FMath::Clamp(HDRPaperWhiteNits, ArenaHDR::MinPaperWhite, ArenaHDR::MaxPaperWhite);
	RayTracing = FMath::Clamp(RayTracing, 0, ArenaRayTracing::NumChoices - 1);
	FullRayTracing = FullRayTracing != 0 ? 1 : 0;
	Upscaler = FMath::Clamp(Upscaler, 0, ArenaUpscalerSettings::NumUpscalers - 1);
	UpscalerQuality = FMath::Clamp(UpscalerQuality, 0, ArenaUpscalerSettings::NumQualities - 1);
	FSRSharpness = FMath::Clamp(FSRSharpness, 0.f, 1.f);
	LowLatency = LowLatency != 0 ? 1 : 0;
	// Game.ini only writes sections listed under [SectionsToSave] (Config/DefaultGame.ini); write right away
	// so a crash or a forced quit keeps the change.
	SaveConfig();
	GConfig->Flush(false, GetClass()->GetConfigName());
}

void UArenaUserSettings::ApplyAudio() const
{
	if (GEngine)
	{
		if (FAudioDevice* Device = GEngine->GetMainAudioDeviceRaw())
		{
			Device->SetTransientPrimaryVolume(FMath::Clamp(Volume, 0.f, 1.f));
		}
	}
}

const TArray<FName>& UArenaUserSettings::GetAdjustableSettingIds()
{
	static const TArray<FName> Ids = { FName(TEXT("Sensitivity")), FName(TEXT("InvertY")), FName(TEXT("FOV")), FName(TEXT("Crosshair")), FName(TEXT("HudScale")), FName(TEXT("HudWidth")), FName(TEXT("HudWidthPercent")), FName(TEXT("Brightness")), FName(TEXT("RayTracing")), FName(TEXT("FullRayTracing")), FName(TEXT("Upscaler")), FName(TEXT("UpscalerQuality")), FName(TEXT("FSRSharpness")), FName(TEXT("LowLatency")), FName(TEXT("Volume")), FName(TEXT("MusicVolume")), FName(TEXT("Monitor")), FName(TEXT("WindowMode")), FName(TEXT("Resolution")), FName(TEXT("HDR")), FName(TEXT("HDRPeak")), FName(TEXT("HDRPaperWhite")) };
	return Ids;
}

FString UArenaUserSettings::GetSettingLabel(FName Id)
{
	if (Id == TEXT("Sensitivity")) { return TEXT("Mouse Sensitivity"); }
	if (Id == TEXT("InvertY"))     { return TEXT("Invert Mouse Y"); }
	if (Id == TEXT("FOV"))         { return TEXT("Field of View"); }
	if (Id == TEXT("Crosshair"))   { return TEXT("Crosshair"); }
	if (Id == TEXT("HudScale"))    { return TEXT("HUD Scale"); }
	if (Id == TEXT("HudWidth"))    { return TEXT("HUD Width"); }
	if (Id == TEXT("HudWidthPercent")) { return TEXT("HUD Width (Custom)"); }
	if (Id == TEXT("Brightness"))  { return TEXT("Brightness"); }
	if (Id == TEXT("RayTracing"))  { return TEXT("Ray Tracing"); }
	if (Id == TEXT("FullRayTracing")) { return TEXT("Full Ray Tracing"); }
	if (Id == TEXT("Upscaler"))    { return TEXT("Upscaler"); }
	if (Id == TEXT("UpscalerQuality")) { return TEXT("Upscaler Quality"); }
	if (Id == TEXT("FSRSharpness")) { return TEXT("FSR Sharpness"); }
	if (Id == TEXT("LowLatency"))  { return TEXT("Low Latency"); }
	if (Id == TEXT("Volume"))      { return TEXT("Volume"); }
	if (Id == TEXT("MusicVolume")) { return TEXT("Music Volume"); }
	if (Id == TEXT("Monitor"))     { return TEXT("Monitor"); }
	if (Id == TEXT("WindowMode"))  { return TEXT("Window Mode"); }
	if (Id == TEXT("Resolution"))  { return TEXT("Resolution"); }
	if (Id == TEXT("HDR"))         { return TEXT("HDR"); }
	if (Id == TEXT("HDRPeak"))     { return TEXT("HDR Peak Brightness"); }
	if (Id == TEXT("HDRPaperWhite")) { return TEXT("HDR Paper White"); }
	return Id.ToString();
}

FString UArenaUserSettings::GetSettingValueText(FName Id) const
{
	if (Id == TEXT("Sensitivity")) { return FString::Printf(TEXT("%.2f"), MouseSensitivity); }
	if (Id == TEXT("InvertY"))     { return bInvertY ? TEXT("On") : TEXT("Off"); }
	if (Id == TEXT("FOV"))
	{
		// The setting is 4:3 horizontal (UT2004); show what that is on this screen
		const float Horizontal = ArenaDisplay::GetHorizontalFOV(FOV, ArenaDisplay::GetViewAspect());
		return FString::Printf(TEXT("%d (%d wide)"), FMath::RoundToInt(FOV), FMath::RoundToInt(Horizontal));
	}
	if (Id == TEXT("Crosshair"))   { return ArenaArtHUD::GetCrosshairName(Crosshair); }
	if (Id == TEXT("HudScale"))    { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(HudScale * 100.f)); }
	if (Id == TEXT("HudWidth"))
	{
		static const TCHAR* Names[4] = { TEXT("Full Width"), TEXT("21:9"), TEXT("16:9"), TEXT("Custom") };
		return Names[FMath::Clamp(HudWidth, 0, 3)];
	}
	if (Id == TEXT("HudWidthPercent")) { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(HudWidthPercent)); }
	if (Id == TEXT("Brightness"))  { return FMath::Abs(Brightness) < 0.01f ? FString(TEXT("0")) : FString::Printf(TEXT("%+.2f EV"), Brightness); }
	if (Id == TEXT("RayTracing"))  { return ArenaRayTracing::GetChoiceText(RayTracing); }
	if (Id == TEXT("FullRayTracing")) { return ArenaRayTracing::GetFullChoiceText(FullRayTracing); }
	if (Id == TEXT("Upscaler"))    { return ArenaUpscalerSettings::GetUpscalerText(Upscaler); }
	if (Id == TEXT("UpscalerQuality")) { return Upscaler == ArenaUpscalerSettings::UpscalerOff ? FString(TEXT("Native")) : ArenaUpscalerSettings::GetQualityText(UpscalerQuality); }
	if (Id == TEXT("FSRSharpness")) { return ArenaUpscalerSettings::GetSharpnessText(FSRSharpness); }
	if (Id == TEXT("LowLatency"))  { return ArenaUpscalerSettings::GetLowLatencyText(LowLatency); }
	if (Id == TEXT("Volume"))      { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Volume * 100.f)); }
	if (Id == TEXT("MusicVolume")) { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(MusicVolume * 100.f)); }
	if (Id == TEXT("Monitor"))     { return ArenaDisplay::GetChoiceName(Monitor); }
	if (Id == TEXT("WindowMode"))  { return ArenaDisplay::GetWindowModeText(WindowMode); }
	if (Id == TEXT("Resolution"))  { return ArenaDisplay::GetResolutionText(); }
	if (Id == TEXT("HDR"))         { return ArenaHDR::GetChoiceText(HDR); }
	if (Id == TEXT("HDRPeak"))     { return ArenaHDR::GetPeakText(HDRPeakNits); }
	if (Id == TEXT("HDRPaperWhite")) { return ArenaHDR::GetPaperWhiteText(HDRPaperWhiteNits); }
	return FString();
}

float UArenaUserSettings::StepSensitivity(float Current, int32 Dir)
{
	constexpr float Pivot = 2.f;
	constexpr float Eps = 0.001f;
	Current = FMath::Clamp(Current, MinSensitivity, MaxSensitivity);
	if (Dir == 0)
	{
		return Current;
	}
	const float Step = (Dir > 0) ? (Current < Pivot - Eps ? 0.1f : 0.25f) : (Current > Pivot + Eps ? 0.25f : 0.1f);
	float Result = FMath::RoundToFloat((Current + Dir * Step) / Step) * Step;
	// Off-grid starting values (hand-edited ini) must still move in the requested direction
	if (Dir > 0 && Result <= Current + Eps)
	{
		Result += Step;
	}
	else if (Dir < 0 && Result >= Current - Eps)
	{
		Result -= Step;
	}
	// Never skip over the pivot when stepping 0.25 downwards from an off-grid value
	if (Dir < 0 && Current > Pivot + Eps && Result < Pivot)
	{
		Result = Pivot;
	}
	return FMath::Clamp(Result, MinSensitivity, MaxSensitivity);
}

FString UArenaUserSettings::CleanStatsText(const FString& In)
{
	FString Out;
	for (const TCHAR C : In.TrimStartAndEnd())
	{
		if (FChar::IsWhitespace(C))
		{
			Out.AppendChar(TEXT('_'));
		}
		else if (C >= 32 && C != 127 && C != TEXT('"'))
		{
			Out.AppendChar(C);
		}
	}
	return Out.Left(MaxStatsTextLen);
}

bool UArenaUserSettings::HasValidStatsLogin() const
{
	return StatsUsername.Len() >= MinStatsUsernameLen && StatsPassword.Len() >= MinStatsPasswordLen
		&& CleanStatsText(StatsUsername) == StatsUsername && CleanStatsText(StatsPassword) == StatsPassword;
}

bool UArenaUserSettings::ChangeSetting(FName Id, int32 Dir)
{
	const int32 D = Dir >= 0 ? 1 : -1;
	if (Id == TEXT("Sensitivity"))
	{
		MouseSensitivity = StepSensitivity(MouseSensitivity, D);
	}
	else if (Id == TEXT("InvertY"))
	{
		bInvertY = !bInvertY;
	}
	else if (Id == TEXT("FOV"))
	{
		FOV = FMath::Clamp(FMath::RoundToFloat(FOV / 5.f) * 5.f + D * 5.f, MinFOV, MaxFOV);
	}
	else if (Id == TEXT("Crosshair"))
	{
		// wraps around; choices whose texture is missing fall back to the built-in crosshair when drawn
		const int32 Num = FMath::Max(1, ArenaArtHUD::NumCrosshairs());
		Crosshair = ((FMath::Clamp(Crosshair, 0, Num - 1) + D) % Num + Num) % Num;
	}
	else if (Id == TEXT("HudWidth"))
	{
		HudWidth = ((FMath::Clamp(HudWidth, 0, 3) + D) % 4 + 4) % 4;
	}
	else if (Id == TEXT("HudWidthPercent"))
	{
		// adjusting the percentage selects the custom width
		HudWidthPercent = FMath::Clamp(FMath::RoundToFloat(HudWidthPercent / 5.f) * 5.f + D * 5.f, 50.f, 100.f);
		HudWidth = 3;
	}
	else if (Id == TEXT("HudScale"))
	{
		HudScale = FMath::Clamp(FMath::RoundToFloat(HudScale / HudScaleStep) * HudScaleStep + D * HudScaleStep, MinHudScale, MaxHudScale);
	}
	else if (Id == TEXT("Brightness"))
	{
		// read every frame by FArenaBrightnessViewExtension
		Brightness = FMath::Clamp(FMath::RoundToFloat(Brightness / BrightnessStep) * BrightnessStep + D * BrightnessStep, MinBrightness, MaxBrightness);
	}
	else if (Id == TEXT("RayTracing"))
	{
		// Auto / On / Off, wraps around; switches the renderer right away (r.RayTracing.EnableOnDemand)
		RayTracing = ((FMath::Clamp(RayTracing, 0, ArenaRayTracing::NumChoices - 1) + D) % ArenaRayTracing::NumChoices + ArenaRayTracing::NumChoices) % ArenaRayTracing::NumChoices;
		Save();
		ArenaRayTracing::Apply();
		return true;
	}
	else if (Id == TEXT("FullRayTracing"))
	{
		// Off / On toggle; stays as it is while ray traced Lumen is unavailable or switched off
		if (!ArenaRayTracing::CanUseFull())
		{
			return true;
		}
		FullRayTracing = FullRayTracing != 0 ? 0 : 1;
		Save();
		ArenaRayTracing::Apply();
		return true;
	}
	else if (Id == TEXT("Upscaler") || Id == TEXT("UpscalerQuality") || Id == TEXT("FSRSharpness") || Id == TEXT("LowLatency"))
	{
		// Upscaler: TSR / FSR / Off, quality presets and sharpness wrap around; low latency toggles. All apply right away.
		if (Id == TEXT("Upscaler"))
		{
			Upscaler = ((FMath::Clamp(Upscaler, 0, ArenaUpscalerSettings::NumUpscalers - 1) + D) % ArenaUpscalerSettings::NumUpscalers + ArenaUpscalerSettings::NumUpscalers) % ArenaUpscalerSettings::NumUpscalers;
		}
		else if (Id == TEXT("UpscalerQuality"))
		{
			UpscalerQuality = ((FMath::Clamp(UpscalerQuality, 0, ArenaUpscalerSettings::NumQualities - 1) + D) % ArenaUpscalerSettings::NumQualities + ArenaUpscalerSettings::NumQualities) % ArenaUpscalerSettings::NumQualities;
		}
		else if (Id == TEXT("FSRSharpness"))
		{
			FSRSharpness = FMath::Clamp(FMath::RoundToFloat(FSRSharpness * 10.f + D) / 10.f, 0.f, 1.f);
		}
		else
		{
			LowLatency = LowLatency != 0 ? 0 : 1;
		}
		Save();
		ArenaUpscalerSettings::Apply();
		return true;
	}
	else if (Id == TEXT("Volume"))
	{
		Volume = FMath::Clamp(FMath::RoundToFloat((Volume + D * 0.05f) * 20.f) / 20.f, 0.f, 1.f);
		ApplyAudio();
	}
	else if (Id == TEXT("MusicVolume"))
	{
		// applied by UArenaMusicSubsystem on its next tick
		MusicVolume = FMath::Clamp(FMath::RoundToFloat((MusicVolume + D * 0.05f) * 20.f) / 20.f, 0.f, 1.f);
	}
	else if (Id == TEXT("Monitor"))
	{
		// wraps around; 0 = automatic. Moves a fullscreen window right away.
		const int32 Num = ArenaDisplay::NumChoices();
		Monitor = ((FMath::Clamp(Monitor, 0, Num - 1) + D) % Num + Num) % Num;
		Save();
		ArenaDisplay::Apply();
		return true;
	}
	else if (Id == TEXT("WindowMode") || Id == TEXT("Resolution"))
	{
		// Fullscreen / Borderless / Windowed wrap around; resolutions step through the mode's list. Applied right away.
		if (Id == TEXT("WindowMode"))
		{
			WindowMode = ((FMath::Clamp(WindowMode, 0, ArenaDisplay::NumWindowModes - 1) + D) % ArenaDisplay::NumWindowModes + ArenaDisplay::NumWindowModes) % ArenaDisplay::NumWindowModes;
		}
		else
		{
			if (WindowMode == ArenaDisplay::WindowBorderless)
			{
				return true; // always the desktop size
			}
			const FIntPoint Res = ArenaDisplay::StepResolution(D);
			ResolutionX = Res.X;
			ResolutionY = Res.Y;
		}
		Save();
		ArenaDisplay::ApplyWindowSettings();
		return true;
	}
	else if (Id == TEXT("HDR") || Id == TEXT("HDRPeak") || Id == TEXT("HDRPaperWhite"))
	{
		// HDR toggles (stays off where unsupported); peak brightness and paper white step. Applied right away.
		if (Id == TEXT("HDR"))
		{
			if (!ArenaHDR::IsSupported())
			{
				return true; // keeps the saved choice for a display that can show HDR
			}
			HDR = HDR != 0 ? 0 : 1;
		}
		else if (Id == TEXT("HDRPeak"))
		{
			HDRPeakNits = ArenaHDR::StepPeak(HDRPeakNits, D);
		}
		else
		{
			HDRPaperWhiteNits = FMath::Clamp((HDRPaperWhiteNits / ArenaHDR::PaperWhiteStep + D) * ArenaHDR::PaperWhiteStep, ArenaHDR::MinPaperWhite, ArenaHDR::MaxPaperWhite);
		}
		Save();
		ArenaHDR::Apply();
		return true;
	}
	else
	{
		return false;
	}
	Save();
	return true;
}

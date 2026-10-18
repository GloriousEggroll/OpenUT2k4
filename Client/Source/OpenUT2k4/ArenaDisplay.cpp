#include "ArenaDisplay.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"
#include "GenericPlatform/GenericApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "DynamicRHI.h"
#include "ArenaHDR.h"

#if PLATFORM_LINUX
THIRD_PARTY_INCLUDES_START
#include "SDL3/SDL.h"
THIRD_PARTY_INCLUDES_END
#endif

#if PLATFORM_LINUX
namespace ArenaDisplayLocal
{
	struct FDisplay
	{
		SDL_DisplayID Id = 0;
		SDL_Rect Bounds = {};
		FString Name;
	};

	/** Displays ordered top to bottom, then left to right (the order shown in settings). */
	TArray<FDisplay> GetDisplays()
	{
		TArray<FDisplay> Result;
		if (!SDL_WasInit(SDL_INIT_VIDEO))
		{
			return Result;
		}
		int Count = 0;
		if (SDL_DisplayID* Ids = SDL_GetDisplays(&Count))
		{
			for (int i = 0; i < Count; ++i)
			{
				FDisplay D;
				D.Id = Ids[i];
				SDL_GetDisplayBounds(D.Id, &D.Bounds);
				const char* Name = SDL_GetDisplayName(D.Id);
				D.Name = Name ? FString(UTF8_TO_TCHAR(Name)) : FString::Printf(TEXT("Display %d"), i + 1);
				Result.Add(D);
			}
			SDL_free(Ids);
		}
		Result.Sort([](const FDisplay& A, const FDisplay& B)
		{
			return A.Bounds.y != B.Bounds.y ? A.Bounds.y < B.Bounds.y : A.Bounds.x < B.Bounds.x;
		});
		return Result;
	}

	/** Automatic choice: the display at the desktop origin, else SDL's primary display. */
	int32 AutoIndex(const TArray<FDisplay>& Displays)
	{
		for (int32 i = 0; i < Displays.Num(); ++i)
		{
			if (Displays[i].Bounds.x == 0 && Displays[i].Bounds.y == 0)
			{
				return i;
			}
		}
		const SDL_DisplayID Primary = SDL_GetPrimaryDisplay();
		for (int32 i = 0; i < Displays.Num(); ++i)
		{
			if (Displays[i].Id == Primary)
			{
				return i;
			}
		}
		return Displays.Num() > 0 ? 0 : INDEX_NONE;
	}

	int32 TargetIndex(const TArray<FDisplay>& Displays)
	{
		const int32 Choice = UArenaUserSettings::Get()->Monitor;
		return (Choice >= 1 && Choice <= Displays.Num()) ? Choice - 1 : AutoIndex(Displays);
	}

	/** Shorten EDID names like "Dell Inc. AW3225QF 2Q49YZ3 (DP-2)" a little for the menu. */
	FString ShortName(const FString& Name)
	{
		// Drop the maker's legal suffix and the serial number EDID names carry ("Dell Inc. AW3225QF 2Q49YZ3 (DP-2)"
		// -> "Dell AW3225QF (DP-2)"), then cap the length so the value fits the settings box.
		FString Out = Name;
		Out.ReplaceInline(TEXT(" Inc."), TEXT(""));
		TArray<FString> Words;
		Out.ParseIntoArrayWS(Words);
		TArray<FString> Kept;
		for (const FString& Word : Words)
		{
			int32 Digits = 0;
			for (const TCHAR C : Word)
			{
				Digits += FChar::IsDigit(C) ? 1 : 0;
			}
			const bool bSerial = Kept.Num() >= 2 && !Word.StartsWith(TEXT("(")) && Word.Len() >= 6 && Digits >= 2 && Digits < Word.Len();
			if (!bSerial)
			{
				Kept.Add(Word);
			}
		}
		Out = FString::Join(Kept, TEXT(" "));
		return Out.Len() > 24 ? Out.Left(23) + TEXT("~") : Out;
	}

	SDL_Window* GetGameWindow()
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			return nullptr;
		}
		const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
		if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
		{
			return nullptr;
		}
		return static_cast<SDL_Window*>(Window->GetNativeWindow()->GetOSWindowHandle());
	}

	/** Returns true when the window is windowed or already fullscreen on the chosen display. */
	bool ApplyToWindow(SDL_Window* Win)
	{
		if (!Win || !(SDL_GetWindowFlags(Win) & SDL_WINDOW_FULLSCREEN))
		{
			return true;
		}
		const TArray<FDisplay> Displays = GetDisplays();
		const int32 Index = TargetIndex(Displays);
		if (Index == INDEX_NONE || SDL_GetDisplayForWindow(Win) == Displays[Index].Id)
		{
			return true;
		}
		const FDisplay& Target = Displays[Index];
		UE_LOG(LogOpenUT2k4, Log, TEXT("Display: moving fullscreen window to '%s' (%d,%d %dx%d)"),
			*Target.Name, Target.Bounds.x, Target.Bounds.y, Target.Bounds.w, Target.Bounds.h);
		// Leave fullscreen, point the window at the target display, and fullscreen there with that display's
		// own desktop mode (Wayland: xdg_toplevel.set_fullscreen on that output; X11: moves the window).
		SDL_SetWindowFullscreen(Win, false);
		SDL_SyncWindow(Win);
		SDL_SetWindowPosition(Win, SDL_WINDOWPOS_CENTERED_DISPLAY(Target.Id), SDL_WINDOWPOS_CENTERED_DISPLAY(Target.Id));
		SDL_SetWindowFullscreenMode(Win, SDL_GetDesktopDisplayMode(Target.Id));
		SDL_SetWindowFullscreen(Win, true);
		SDL_SyncWindow(Win);
		return SDL_GetDisplayForWindow(Win) == Target.Id;
	}
}
#endif

namespace ArenaDisplay
{
	int32 NumChoices()
	{
#if PLATFORM_LINUX
		const int32 Num = ArenaDisplayLocal::GetDisplays().Num();
		return Num > 1 ? Num + 1 : 1;
#else
		return 1;
#endif
	}

	FString GetChoiceName(int32 Choice)
	{
#if PLATFORM_LINUX
		using namespace ArenaDisplayLocal;
		const TArray<FDisplay> Displays = GetDisplays();
		if (Choice >= 1 && Choice <= Displays.Num())
		{
			return FString::Printf(TEXT("%d: %s"), Choice, *ShortName(Displays[Choice - 1].Name));
		}
		const int32 Auto = AutoIndex(Displays);
		if (Displays.Num() > 1 && Auto != INDEX_NONE)
		{
			return FString::Printf(TEXT("Auto (%s)"), *ShortName(Displays[Auto].Name));
		}
#endif
		return TEXT("Automatic");
	}

	void Apply()
	{
#if PLATFORM_LINUX
		ArenaDisplayLocal::ApplyToWindow(ArenaDisplayLocal::GetGameWindow());
#endif
	}

	// ---- field of view ----

	float GetViewAspect()
	{
		if (GEngine && GEngine->GameViewport)
		{
			FVector2D Size;
			GEngine->GameViewport->GetViewportSize(Size);
			if (Size.X > 0.f && Size.Y > 0.f)
			{
				return Size.X / Size.Y;
			}
		}
		return 16.f / 9.f;
	}

	float GetHorizontalFOV(float FOV43, float Aspect)
	{
		// Same vertical extent as a 4:3 screen: tan(H/2) = tan(FOV43/2) * (3/4) * Aspect
		const float HalfTan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FOV43, 1.f, 170.f) * 0.5f)) * 0.75f * FMath::Max(Aspect, 0.1f);
		return FMath::Min(FMath::RadiansToDegrees(2.f * FMath::Atan(HalfTan)), MaxHorizontalFOV);
	}

	float GetCameraFOV(float FOV43, float CameraAspect)
	{
		// The engine derives the vertical FOV as atan(tan(CameraFOV/2) / CameraAspect) and widens it to the viewport.
		// Pick the camera FOV whose vertical extent gives the wanted (capped) horizontal angle on this viewport.
		const float ViewAspect = GetViewAspect();
		const float HalfTanH = FMath::Tan(FMath::DegreesToRadians(GetHorizontalFOV(FOV43, ViewAspect) * 0.5f));
		const float HalfTanV = HalfTanH / ViewAspect;
		return FMath::RadiansToDegrees(2.f * FMath::Atan(HalfTanV * FMath::Max(CameraAspect, 0.1f)));
	}

	// ---- window mode and resolution ----

	FString GetWindowModeText(int32 Mode)
	{
		switch (Mode)
		{
		case WindowFullscreen: return TEXT("Fullscreen");
		case WindowWindowed:   return TEXT("Windowed");
		default:               return TEXT("Borderless");
		}
	}

	FIntPoint GetDesktopResolution()
	{
#if PLATFORM_LINUX
		using namespace ArenaDisplayLocal;
		const TArray<FDisplay> Displays = GetDisplays();
		const int32 Index = TargetIndex(Displays);
		if (Index != INDEX_NONE)
		{
			if (const SDL_DisplayMode* Mode = SDL_GetDesktopDisplayMode(Displays[Index].Id))
			{
				return FIntPoint(Mode->w, Mode->h);
			}
			return FIntPoint(Displays[Index].Bounds.w, Displays[Index].Bounds.h);
		}
#endif
		FDisplayMetrics Metrics;
		FDisplayMetrics::RebuildDisplayMetrics(Metrics);
		if (Metrics.PrimaryDisplayWidth > 0 && Metrics.PrimaryDisplayHeight > 0)
		{
			return FIntPoint(Metrics.PrimaryDisplayWidth, Metrics.PrimaryDisplayHeight);
		}
		return FIntPoint(1920, 1080);
	}

	TArray<FIntPoint> GetResolutions(int32 Mode)
	{
		const FIntPoint Desktop = GetDesktopResolution();
		TArray<FIntPoint> Result;
		if (Mode == WindowBorderless)
		{
			Result.Add(Desktop);
			return Result;
		}
		auto AddUnique = [&Result](const FIntPoint& P)
		{
			if (P.X >= 640 && P.Y >= 480)
			{
				Result.AddUnique(P);
			}
		};
#if PLATFORM_LINUX
		{
			using namespace ArenaDisplayLocal;
			const TArray<FDisplay> Displays = GetDisplays();
			const int32 Index = TargetIndex(Displays);
			if (Index != INDEX_NONE)
			{
				int Count = 0;
				if (SDL_DisplayMode** Modes = SDL_GetFullscreenDisplayModes(Displays[Index].Id, &Count))
				{
					for (int i = 0; i < Count; ++i)
					{
						AddUnique(FIntPoint(Modes[i]->w, Modes[i]->h));
					}
					SDL_free(Modes);
				}
			}
		}
#else
		{
			FScreenResolutionArray Modes;
			if (RHIGetAvailableResolutions(Modes, true))
			{
				for (const FScreenResolutionRHI& R : Modes)
				{
					AddUnique(FIntPoint(R.Width, R.Height));
				}
			}
		}
#endif
		AddUnique(Desktop);
		if (Mode == WindowWindowed)
		{
			// Common 16:9, 16:10, 21:9 and 32:9 window sizes that fit on this display
			static const FIntPoint Common[] = {
				{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},
				{1280, 800}, {1680, 1050}, {1920, 1200}, {2560, 1600},
				{2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160},
				{3840, 1080}, {5120, 1440}, {7680, 2160} };
			for (const FIntPoint& P : Common)
			{
				if (P.X <= Desktop.X && P.Y <= Desktop.Y)
				{
					AddUnique(P);
				}
			}
		}
		Result.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
		return Result;
	}

	FString GetAspectText(FIntPoint Size)
	{
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return FString();
		}
		const float R = float(Size.X) / float(Size.Y);
		struct FNamed { float Ratio; float Tolerance; const TCHAR* Name; };
		static const FNamed Named[] = {
			{4.f / 3.f, 0.02f, TEXT("4:3")}, {5.f / 4.f, 0.02f, TEXT("5:4")}, {16.f / 10.f, 0.02f, TEXT("16:10")},
			{16.f / 9.f, 0.02f, TEXT("16:9")}, {2.36f, 0.07f, TEXT("21:9")}, {32.f / 9.f, 0.05f, TEXT("32:9")},
			{48.f / 9.f, 0.08f, TEXT("48:9")} };
		for (const FNamed& N : Named)
		{
			if (FMath::Abs(R - N.Ratio) <= N.Tolerance)
			{
				return N.Name;
			}
		}
		return FString::Printf(TEXT("%.2f:1"), R);
	}

	/** The resolution the settings ask for in the current window mode. */
	static FIntPoint GetWantedResolution(const TArray<FIntPoint>& Choices)
	{
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		if (S->WindowMode == WindowBorderless || Choices.Num() == 0)
		{
			return GetDesktopResolution();
		}
		const FIntPoint Wanted(S->ResolutionX, S->ResolutionY);
		if (Wanted.X > 0 && Wanted.Y > 0)
		{
			return Wanted;
		}
		// Not chosen yet: the desktop size in fullscreen, a window a size smaller in windowed mode
		const FIntPoint Desktop = GetDesktopResolution();
		if (S->WindowMode == WindowWindowed)
		{
			for (int32 i = Choices.Num() - 1; i >= 0; --i)
			{
				if (Choices[i].X < Desktop.X && Choices[i].Y < Desktop.Y)
				{
					return Choices[i];
				}
			}
		}
		return Desktop;
	}

	FString GetResolutionText()
	{
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		const FIntPoint Res = GetWantedResolution(GetResolutions(S->WindowMode));
		const FString Text = FString::Printf(TEXT("%dx%d (%s)"), Res.X, Res.Y, *GetAspectText(Res));
		return S->WindowMode == WindowBorderless ? FString(TEXT("Desktop ")) + Text : Text;
	}

	FIntPoint StepResolution(int32 Dir)
	{
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		const TArray<FIntPoint> Choices = GetResolutions(S->WindowMode);
		const FIntPoint Current = GetWantedResolution(Choices);
		if (Choices.Num() == 0)
		{
			return Current;
		}
		int32 Index = Choices.IndexOfByKey(Current);
		if (Index == INDEX_NONE)
		{
			// Off the list (hand-edited ini or another display): continue from the nearest size by pixel count
			Index = 0;
			for (int32 i = 0; i < Choices.Num(); ++i)
			{
				if (int64(Choices[i].X) * Choices[i].Y <= int64(Current.X) * Current.Y)
				{
					Index = i;
				}
			}
			if (Dir < 0)
			{
				return Choices[Index];
			}
		}
		const int32 Num = Choices.Num();
		return Choices[((Index + (Dir >= 0 ? 1 : -1)) % Num + Num) % Num];
	}

	static EWindowMode::Type ToEngineWindowMode(int32 Mode)
	{
		switch (Mode)
		{
		case WindowFullscreen: return EWindowMode::Fullscreen;
		case WindowWindowed:   return EWindowMode::Windowed;
		default:               return EWindowMode::WindowedFullscreen;
		}
	}

	void ApplyWindowSettings()
	{
		UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
		if (!GUS)
		{
			return;
		}
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		const FIntPoint Res = GetWantedResolution(GetResolutions(S->WindowMode));
		const EWindowMode::Type Mode = ToEngineWindowMode(S->WindowMode);
		UE_LOG(LogOpenUT2k4, Log, TEXT("Display: window mode %s, %dx%d (%s)"), *GetWindowModeText(S->WindowMode), Res.X, Res.Y, *GetAspectText(Res));
		// GameUserSettings drives the engine's window (r.SetRes) and is what the engine reads at the next start.
		GUS->SetFullscreenMode(Mode);
		GUS->SetScreenResolution(Res);
		GUS->ApplyResolutionSettings(false);
		GUS->SaveSettings();
		Apply();
	}

	/** True when the command line chose the window (-windowed, -fullscreen, -ResX=, -ResY=): the settings then leave it alone. */
	static bool HasCommandLineWindow()
	{
		const TCHAR* Cmd = FCommandLine::Get();
		int32 Dummy = 0;
		return FParse::Param(Cmd, TEXT("windowed")) || FParse::Param(Cmd, TEXT("fullscreen"))
			|| FParse::Value(Cmd, TEXT("ResX="), Dummy) || FParse::Value(Cmd, TEXT("ResY="), Dummy);
	}

	/** At startup: bring the window in line with the settings if GameUserSettings.ini says otherwise. */
	static void ApplyStartupWindow()
	{
		UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
		if (!GUS || HasCommandLineWindow() || !GEngine->GameViewport)
		{
			return;
		}
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		const FIntPoint Res = GetWantedResolution(GetResolutions(S->WindowMode));
		if (GUS->GetFullscreenMode() != ToEngineWindowMode(S->WindowMode) || GUS->GetScreenResolution() != Res)
		{
			ApplyWindowSettings();
		}
	}
}

void UArenaDisplaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UArenaDisplaySubsystem::Tick), 0.25f);
}

void UArenaDisplaySubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

bool UArenaDisplaySubsystem::Tick(float DeltaTime)
{
#if PLATFORM_LINUX
	// The engine may (re)enter fullscreen on its own default display at startup or after a resolution or window
	// mode change; keep the window on the chosen display. Cheap: a few SDL queries every quarter second.
	SDL_Window* Win = ArenaDisplayLocal::GetGameWindow();
	if (Win)
	{
		ArenaDisplayLocal::ApplyToWindow(Win);
	}
#endif
	if (!bStartupApplied && GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWindow().IsValid())
	{
		bStartupApplied = true;
		ArenaDisplay::ApplyStartupWindow();
		// HDR needs the game window (the swapchain surface is probed for HDR colour spaces)
		ArenaHDR::Apply();
		StartTime = FPlatformTime::Seconds();
#if !UE_BUILD_SHIPPING
		// Test switches: -ArenaHDRToggle=<sec>[,<sec>...] flips HDR live at those times after startup;
		// -ArenaWindowTest=<sec> logs the window / viewport / FOV state then (and -ArenaFOV=<deg> sets the FOV, not saved).
		FString Toggles;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaHDRToggle="), Toggles, false))
		{
			TArray<FString> Parts;
			Toggles.ParseIntoArray(Parts, TEXT(","));
			for (const FString& P : Parts)
			{
				HDRToggleTimes.Add(FCString::Atod(*P));
			}
		}
		float TestFOV = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaFOV="), TestFOV) && TestFOV > 0.f)
		{
			UArenaUserSettings::Get()->FOV = FMath::Clamp(TestFOV, UArenaUserSettings::MinFOV, UArenaUserSettings::MaxFOV);
		}
		FParse::Value(FCommandLine::Get(), TEXT("ArenaWindowTest="), WindowTestTime);
#endif
	}
#if !UE_BUILD_SHIPPING
	if (bStartupApplied)
	{
		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		while (HDRToggleTimes.Num() > 0 && Elapsed >= HDRToggleTimes[0])
		{
			HDRToggleTimes.RemoveAt(0);
			ArenaHDR::TestToggle();
		}
		if (WindowTestTime > 0.0 && Elapsed >= WindowTestTime)
		{
			WindowTestTime = 0.0;
			const float Aspect = ArenaDisplay::GetViewAspect();
			const float FOV43 = UArenaUserSettings::Get()->FOV;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Display test: viewport aspect %.3f (%s), FOV %.0f at 4:3 -> %.1f horizontal, camera FOV %.1f (16:9 camera), settings: %s / %s / FOV '%s'"),
				Aspect, *ArenaDisplay::GetAspectText(FIntPoint(FMath::RoundToInt(Aspect * 1000.f), 1000)), FOV43, ArenaDisplay::GetHorizontalFOV(FOV43, Aspect),
				ArenaDisplay::GetCameraFOV(FOV43, 16.f / 9.f), *UArenaUserSettings::Get()->GetSettingValueText(TEXT("WindowMode")),
				*UArenaUserSettings::Get()->GetSettingValueText(TEXT("Resolution")), *UArenaUserSettings::Get()->GetSettingValueText(TEXT("FOV")));
			FString Res;
			for (const FIntPoint& P : ArenaDisplay::GetResolutions(ArenaDisplay::WindowWindowed))
			{
				Res += FString::Printf(TEXT(" %dx%d"), P.X, P.Y);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("Display test: windowed resolutions:%s"), *Res);
			Res.Reset();
			for (const FIntPoint& P : ArenaDisplay::GetResolutions(ArenaDisplay::WindowFullscreen))
			{
				Res += FString::Printf(TEXT(" %dx%d"), P.X, P.Y);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("Display test: fullscreen resolutions:%s; HDR: %s"), *Res, *UArenaUserSettings::Get()->GetSettingValueText(TEXT("HDR")));
		}
	}
#endif
	return true;
}

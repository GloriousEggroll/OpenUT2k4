#include "ArenaHDR.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "HDRHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if ARENA_WITH_VULKAN
#if PLATFORM_WINDOWS && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include "IVulkanDynamicRHI.h"
#endif

#if PLATFORM_LINUX
THIRD_PARTY_INCLUDES_START
#include "SDL3/SDL.h"
#if ARENA_WITH_VULKAN
#include "SDL3/SDL_vulkan.h"
#endif
THIRD_PARTY_INCLUDES_END
#endif

namespace ArenaHDRLocal
{
	struct FProbe
	{
		bool bDone = false;
		/** The RHI decides HDR support itself (Metal, D3D): GRHISupportsHDROutput as the RHI set it. */
		bool bEngineManaged = false;
		bool bEngineSupport = false;
		/** Vulkan: the window surface offers an HDR10 (ST 2084) colour space with a format we can render to. */
		bool bSwapchainHDR10 = false;
		EPixelFormat HDRFormat = PF_Unknown;
		/** The display (or the compositor for this window) reports HDR. */
		bool bDisplayKnown = false;
		bool bDisplayHDR = false;
		/** Display peak luminance in nits (0 = unknown). */
		float DisplayPeakNits = 0.f;
		FString Reason;
	};
	static FProbe Probe;
	static bool bApplied = false;
	static bool bActive = false;
	static int32 AppliedPeak = -1;
	static int32 AppliedPaperWhite = -1;
	/** -ArenaHDR=0|1 (all builds): overrides the saved choice for this run without saving it. -1 = none. */
	static int32 CommandLineHDR = -1;
	/** Test switch (non-shipping) -ArenaHDRToggle=<sec>: flip HDR live after that many seconds. */
	static double ToggleOverride = -1.0;

	static TSharedPtr<SWindow> GetGameSWindow()
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			return nullptr;
		}
		return GEngine->GameViewport->GetWindow();
	}

	static void* GetOSWindow()
	{
		const TSharedPtr<SWindow> Window = GetGameSWindow();
		if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
		{
			return nullptr;
		}
		return Window->GetNativeWindow()->GetOSWindowHandle();
	}

#if ARENA_WITH_VULKAN
	static const TCHAR* ColorSpaceName(VkColorSpaceKHR Space)
	{
		switch (Space)
		{
		case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:        return TEXT("SRGB_NONLINEAR");
		case VK_COLOR_SPACE_HDR10_ST2084_EXT:          return TEXT("HDR10_ST2084");
		case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:  return TEXT("EXTENDED_SRGB_LINEAR (scRGB)");
		case VK_COLOR_SPACE_BT2020_LINEAR_EXT:         return TEXT("BT2020_LINEAR");
		case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:  return TEXT("DISPLAY_P3_NONLINEAR");
		case VK_COLOR_SPACE_HDR10_HLG_EXT:             return TEXT("HDR10_HLG");
		default:                                       return nullptr;
		}
	}

	/** Lists the colour spaces the game window's Vulkan surface offers (a second surface on the same window). */
	static void ProbeVulkan(void* OSWindow)
	{
		IVulkanDynamicRHI* RHI = GetIVulkanDynamicRHI();
		const VkInstance Instance = RHI->RHIGetVkInstance();
		const VkPhysicalDevice PhysicalDevice = RHI->RHIGetVkPhysicalDevice();
		const PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)RHI->RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceSurfaceFormatsKHR");
		const PFN_vkDestroySurfaceKHR DestroySurface = (PFN_vkDestroySurfaceKHR)RHI->RHIGetVkInstanceProcAddr("vkDestroySurfaceKHR");
		if (!GetFormats || !DestroySurface || !OSWindow)
		{
			Probe.Reason = TEXT("Vulkan surface queries unavailable");
			return;
		}
		VkSurfaceKHR Surface = VK_NULL_HANDLE;
#if PLATFORM_LINUX
		if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(OSWindow), Instance, nullptr, &Surface))
		{
			Surface = VK_NULL_HANDLE;
		}
#elif PLATFORM_WINDOWS
		if (const PFN_vkCreateWin32SurfaceKHR CreateWin32 = (PFN_vkCreateWin32SurfaceKHR)RHI->RHIGetVkInstanceProcAddr("vkCreateWin32SurfaceKHR"))
		{
			VkWin32SurfaceCreateInfoKHR Info = {};
			Info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
			Info.hinstance = GetModuleHandle(nullptr);
			Info.hwnd = static_cast<HWND>(OSWindow);
			if (CreateWin32(Instance, &Info, nullptr, &Surface) != VK_SUCCESS)
			{
				Surface = VK_NULL_HANDLE;
			}
		}
#endif
		if (Surface == VK_NULL_HANDLE)
		{
			Probe.Reason = TEXT("could not query the window's Vulkan surface");
			return;
		}
		uint32 Num = 0;
		GetFormats(PhysicalDevice, Surface, &Num, nullptr);
		TArray<VkSurfaceFormatKHR> Formats;
		Formats.SetNumZeroed(Num);
		if (Num > 0)
		{
			GetFormats(PhysicalDevice, Surface, &Num, Formats.GetData());
		}
		DestroySurface(Instance, Surface, nullptr);

		TArray<FString> Spaces;
		bool bHDR10_10bit = false;
		bool bHDR10_Float = false;
		for (const VkSurfaceFormatKHR& F : Formats)
		{
			if (const TCHAR* Name = ColorSpaceName(F.colorSpace))
			{
				Spaces.AddUnique(Name);
			}
			if (F.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
			{
				bHDR10_10bit |= F.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
				bHDR10_Float |= F.format == VK_FORMAT_R16G16B16A16_SFLOAT;
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: Vulkan surface offers %d formats; colour spaces: %s; HDR10 with A2B10G10R10: %s, with RGBA16F: %s"),
			Formats.Num(), *FString::Join(Spaces, TEXT(", ")), bHDR10_10bit ? TEXT("yes") : TEXT("no"), bHDR10_Float ? TEXT("yes") : TEXT("no"));
		// HDR10 is 10 bit PQ; the engine's tonemapper writes PQ code values, so a 10 bit swapchain is enough.
		Probe.bSwapchainHDR10 = bHDR10_10bit || bHDR10_Float;
		Probe.HDRFormat = bHDR10_10bit ? PF_A2B10G10R10 : (bHDR10_Float ? PF_FloatRGBA : PF_Unknown);
		if (!Probe.bSwapchainHDR10)
		{
			Probe.Reason = TEXT("the Vulkan driver / compositor offers no HDR10 colour space for this window");
		}
	}
#endif

	static void ProbeDisplay(void* OSWindow)
	{
#if PLATFORM_LINUX
		SDL_Window* Win = static_cast<SDL_Window*>(OSWindow);
		if (!Win)
		{
			return;
		}
		const SDL_PropertiesID WinProps = SDL_GetWindowProperties(Win);
		const bool bWindowHDR = SDL_GetBooleanProperty(WinProps, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
		const float SdrWhite = SDL_GetFloatProperty(WinProps, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.f);
		const float Headroom = SDL_GetFloatProperty(WinProps, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.f);
		const SDL_DisplayID Display = SDL_GetDisplayForWindow(Win);
		const bool bDisplayHDR = Display != 0 && SDL_GetBooleanProperty(SDL_GetDisplayProperties(Display), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
		const char* DisplayName = Display != 0 ? SDL_GetDisplayName(Display) : nullptr;
		UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: SDL video driver '%s', display '%s': display HDR %s, window HDR %s, SDR white %.2f, headroom %.2f"),
			UTF8_TO_TCHAR(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?"), UTF8_TO_TCHAR(DisplayName ? DisplayName : "?"),
			bDisplayHDR ? TEXT("on") : TEXT("off"), bWindowHDR ? TEXT("on") : TEXT("off"), SdrWhite, Headroom);
		// Only SDL's Wayland backend knows the output's HDR state (colour management protocol). Under X11 (e.g.
		// gamescope's Xwayland, whose WSI layer offers HDR10 itself) the surface's colour spaces decide alone.
		const char* Driver = SDL_GetCurrentVideoDriver();
		Probe.bDisplayKnown = Driver && FCStringAnsi::Strcmp(Driver, "wayland") == 0;
		Probe.bDisplayHDR = bDisplayHDR || bWindowHDR;
		if (bWindowHDR && Headroom > 1.f)
		{
			// SDL white level is relative to 80 nits (scRGB 1.0). Compositors often describe the output by its PQ
			// container (10000 nits max, e.g. labwc: headroom 49 over a 203 nit reference) rather than the panel's
			// target luminance; such values say nothing about the monitor, so only plausible panel peaks are kept.
			const float Peak = 80.f * FMath::Max(SdrWhite, 1.f) * Headroom;
			Probe.DisplayPeakNits = (Peak >= 300.f && Peak <= 2000.f) ? Peak : 0.f;
			UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: SDL peak estimate %.0f nits%s"), Peak, Probe.DisplayPeakNits > 0.f ? TEXT("") : TEXT(" (implausible for a panel, Auto uses 1000)"));
		}
#endif
	}

	static void RunProbe()
	{
		if (Probe.bDone || !GDynamicRHI)
		{
			return;
		}
		void* OSWindow = GetOSWindow();
		if (!OSWindow)
		{
			return;
		}
		Probe.bDone = true;
		if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoHDRProbe")))
		{
			Probe.Reason = TEXT("probe disabled (-ArenaNoHDRProbe)");
			UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: %s"), *Probe.Reason);
			return;
		}
#if ARENA_WITH_VULKAN
		if (GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::Vulkan)
		{
			ProbeVulkan(OSWindow);
			ProbeDisplay(OSWindow);
			if (Probe.bSwapchainHDR10 && Probe.bDisplayKnown && !Probe.bDisplayHDR)
			{
				Probe.Reason = TEXT("the display is in SDR mode (turn HDR on in the desktop's display settings)");
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: %s"), Probe.Reason.IsEmpty() ? TEXT("supported") : *(TEXT("not available: ") + Probe.Reason));
			return;
		}
#endif
		// Metal / D3D decide HDR support themselves
		Probe.bEngineManaged = true;
		Probe.bEngineSupport = GRHISupportsHDROutput;
		if (!Probe.bEngineSupport)
		{
			Probe.Reason = TEXT("the RHI reports no HDR output for this display");
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: engine-managed (%s): %s"), GDynamicRHI->GetName(), Probe.bEngineSupport ? TEXT("supported") : TEXT("not supported"));
	}

	static bool IsSupportedNow()
	{
		if (!Probe.bDone)
		{
			return false;
		}
		if (Probe.bEngineManaged)
		{
			return Probe.bEngineSupport;
		}
		return Probe.bSwapchainHDR10 && (!Probe.bDisplayKnown || Probe.bDisplayHDR);
	}

	static void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByCode);
		}
	}

	static void SetCVarFloat(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByCode);
		}
	}

	/**
	 * The Vulkan RHI only recreates a swapchain when the size, fullscreen state or pixel format changes. HDR10 and
	 * SDR both use a 10 bit back buffer here, so switching only the colour space would keep the old swapchain: bounce
	 * the window's viewport through an 8 bit format; Slate then resizes it back to its HDR / SDR format on the next
	 * frame (FSlateViewportInfo::ResizeViewport sees the changed HDR state) and the swapchain is created in the new
	 * colour space.
	 */
	static void ForceSwapchainRecreate()
	{
		const TSharedPtr<SWindow> Window = GetGameSWindow();
		if (!Window.IsValid() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
		{
			return;
		}
		FRHIViewport* Viewport = static_cast<FRHIViewport*>(FSlateApplication::Get().GetRenderer()->GetViewportResource(*Window));
		if (!Viewport)
		{
			return;
		}
		// Any valid size: Slate resizes the viewport back to its own extent and HDR format on the next frame
		const FVector2D WindowSize = Window->GetViewportSize().X > 0.f ? FVector2D(Window->GetViewportSize()) : FVector2D(Window->GetSizeInScreen());
		const FIntPoint Size(FMath::RoundToInt(WindowSize.X), FMath::RoundToInt(WindowSize.Y));
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return;
		}
		const bool bFullscreen = Window->GetWindowMode() == EWindowMode::Fullscreen;
		RHIResizeViewport(Viewport, Size.X, Size.Y, bFullscreen, PF_B8G8R8A8);
	}
}

namespace ArenaHDR
{
	using namespace ArenaHDRLocal;

	const TArray<int32>& GetPeakChoices()
	{
		static const TArray<int32> Choices = { 0, 400, 600, 800, 1000, 1500, 2000, 3000, 4000 };
		return Choices;
	}

	bool IsSupported()
	{
		return IsSupportedNow();
	}

	bool IsActive()
	{
		return bActive;
	}

	FString GetUnsupportedReason()
	{
		if (!Probe.bDone)
		{
			return TEXT("no game window yet");
		}
		return IsSupportedNow() ? FString() : Probe.Reason;
	}

	FString GetChoiceText(int32 Choice)
	{
		if (Probe.bDone && !IsSupportedNow())
		{
			return TEXT("Off (not supported by display)");
		}
		return Choice != 0 ? TEXT("On") : TEXT("Off");
	}

	int32 GetEffectivePeakNits()
	{
		const int32 Setting = UArenaUserSettings::Get()->HDRPeakNits;
		if (Setting > 0)
		{
			return Setting;
		}
		return Probe.DisplayPeakNits >= 100.f ? FMath::RoundToInt(Probe.DisplayPeakNits) : 1000;
	}

	FString GetPeakText(int32 Nits)
	{
		if (Nits > 0)
		{
			return FString::Printf(TEXT("%d nits"), Nits);
		}
		return Probe.DisplayPeakNits >= 100.f ? FString::Printf(TEXT("Auto (%d nits)"), FMath::RoundToInt(Probe.DisplayPeakNits)) : FString(TEXT("Auto (1000 nits)"));
	}

	FString GetPaperWhiteText(int32 Nits)
	{
		return FString::Printf(TEXT("%d nits"), Nits);
	}

	int32 StepPeak(int32 Current, int32 Dir)
	{
		const TArray<int32>& Choices = GetPeakChoices();
		int32 Index = Choices.IndexOfByKey(Current);
		if (Index == INDEX_NONE)
		{
			Index = 0;
			for (int32 i = 1; i < Choices.Num(); ++i)
			{
				if (Choices[i] <= Current)
				{
					Index = i;
				}
			}
		}
		const int32 Num = Choices.Num();
		return Choices[((Index + (Dir >= 0 ? 1 : -1)) % Num + Num) % Num];
	}

	void TestToggle()
	{
		ToggleOverride = bActive ? 0.0 : 1.0;
		UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: test toggle -> %s"), ToggleOverride > 0.0 ? TEXT("on") : TEXT("off"));
		Apply();
	}

	void Apply()
	{
		RunProbe();
		if (!Probe.bDone)
		{
			return; // no game window yet; UArenaDisplaySubsystem calls again once it exists
		}
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		static bool bParsed = false;
		if (!bParsed)
		{
			bParsed = true;
			FParse::Value(FCommandLine::Get(), TEXT("ArenaHDR="), CommandLineHDR);
		}
		const int32 Choice = ToggleOverride >= 0.0 ? int32(ToggleOverride) : (CommandLineHDR >= 0 ? CommandLineHDR : S->HDR);
		const bool bWant = Choice != 0 && IsSupportedNow();
		const int32 Peak = GetEffectivePeakNits();
		const int32 PaperWhite = FMath::Clamp(S->HDRPaperWhiteNits, MinPaperWhite, MaxPaperWhite);
		if (bApplied && bWant == bActive && (!bWant || (Peak == AppliedPeak && PaperWhite == AppliedPaperWhite)))
		{
			return;
		}
		const bool bModeChange = !bApplied ? bWant : bWant != bActive;
		bApplied = true;
		AppliedPeak = Peak;
		AppliedPaperWhite = PaperWhite;
		bActive = bWant;
		// The bounce swapchain must be created while the output device is still sRGB (some WSIs, e.g. gamescope's,
		// offer HDR10 only with 10 bit formats): bounce before switching on, after switching off.
		if (!Probe.bEngineManaged && bModeChange && bWant)
		{
			ForceSwapchainRecreate();
		}

		UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
		if (!Probe.bEngineManaged)
		{
			// Vulkan: the RHI never reports HDR support; we probed the surface ourselves. Report it only while HDR is
			// on, so SDR keeps the engine's plain path (no UI composition pass).
			FlushRenderingCommands();
			GRHISupportsHDROutput = bWant;
			if (bWant)
			{
				GRHIHDRDisplayOutputFormat = Probe.HDRFormat;
			}
		}
		if (GUS)
		{
			// Scene and UI share one paper white (UI composition scales the SDR HUD and menus to it).
			GUS->SetHDRUILuminanceSeparate(false);
			GUS->SetHDRPaperWhiteNits(float(PaperWhite));
			GUS->SetHDRUILuminanceNits(float(PaperWhite));
			GUS->EnableHDRDisplayOutput(bWant, Peak);
		}
		else
		{
			SetCVarInt(TEXT("r.HDR.EnableHDROutput"), bWant ? 1 : 0);
		}
		SetCVarFloat(TEXT("r.HDR.PaperWhite"), float(PaperWhite));
		SetCVarInt(TEXT("r.HDR.PaperWhite.Mode"), 1);
		SetCVarFloat(TEXT("r.HDR.UI.Luminance"), float(PaperWhite));
		SetCVarInt(TEXT("r.HDR.UI.Luminance.Mode"), 0);
		SetCVarInt(TEXT("r.HDR.UI.CompositeMode"), 1);
		if (!Probe.bEngineManaged)
		{
			// The engine picks the output device per platform (ChooseHDRDeviceAndColorGamut), which is a no-op on Linux
			// and scRGB in Windows editor builds; Vulkan presents HDR10: ACES ST 2084 with BT.2020 primaries.
			SetCVarInt(TEXT("r.HDR.Display.OutputDevice"), bWant ? (Peak > 1000 ? int32(EDisplayOutputFormat::HDR_ACES_2000nit_ST2084) : int32(EDisplayOutputFormat::HDR_ACES_1000nit_ST2084)) : int32(EDisplayOutputFormat::SDR_sRGB));
			SetCVarInt(TEXT("r.HDR.Display.ColorGamut"), bWant ? int32(EDisplayColorGamut::Rec2020_D65) : int32(EDisplayColorGamut::sRGB_D65));
			SetCVarInt(TEXT("r.HDR.Display.MaxLuminance"), bWant ? Peak : 0);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("HDR: output %s (peak %d nits, paper white %d nits, back buffer %s)"),
			bWant ? TEXT("on") : TEXT("off"), Peak, PaperWhite, bWant ? GPixelFormats[GRHIHDRDisplayOutputFormat].Name : TEXT("SDR"));
		if (!Probe.bEngineManaged && bModeChange && !bWant)
		{
			ForceSwapchainRecreate();
		}
	}
}

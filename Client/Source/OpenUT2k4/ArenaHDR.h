#pragma once

#include "CoreMinimal.h"

/**
 * HDR display output (UArenaUserSettings::HDR, HDRPeakNits, HDRPaperWhiteNits). Docs/HDR.md has the background.
 *
 * Vulkan (Linux, Windows): Unreal's Vulkan RHI can present to an HDR10 (ST 2084 PQ, BT.2020) swapchain when
 * r.HDR.Display.OutputDevice asks for it, but it never reports HDR support itself (GRHISupportsHDROutput stays
 * false; only the D3D and Metal RHIs set it). This probes the game window's Vulkan surface for an HDR10 colour space
 * and, when the player turns HDR on, enables the engine's HDR path: HDR output support, r.HDR.EnableHDROutput, the
 * ACES ST 2084 output device with BT.2020 primaries, the peak luminance for the tonemapper and the paper white that
 * scene and UI (HUD canvas, menus, bitmap fonts; composited by r.HDR.UI.CompositeMode) are scaled to. The window's
 * swapchain is then recreated in the HDR10 colour space, live.
 *
 * Metal (Mac): the Metal RHI supports EDR output itself; the setting uses the engine's GameUserSettings path.
 */
namespace ArenaHDR
{
	/** Peak brightness choices in nits (0 = automatic, from the display when it reports one). */
	const TArray<int32>& GetPeakChoices();
	/** Paper white (scene and UI brightness for SDR white) range in nits, stepped by PaperWhiteStep. */
	constexpr int32 MinPaperWhite = 80;
	constexpr int32 MaxPaperWhite = 400;
	constexpr int32 PaperWhiteStep = 20;
	constexpr int32 DefaultPaperWhite = 200;

	/** The game window and display can show HDR (probed once the window exists; false before that). */
	bool IsSupported();
	/** HDR output is on right now. */
	bool IsActive();
	/** Why HDR is unavailable, e.g. "no HDR10 colour space on this swapchain" (empty when supported). */
	FString GetUnsupportedReason();

	FString GetChoiceText(int32 Choice);
	FString GetPeakText(int32 Nits);
	FString GetPaperWhiteText(int32 Nits);
	/** Next peak brightness choice (wraps around). */
	int32 StepPeak(int32 Current, int32 Dir);

	/** Peak luminance used for the tonemapper: the setting, or the display's when automatic (1000 when unknown). */
	int32 GetEffectivePeakNits();

	/**
	 * Probe (first call) and apply the HDR settings to the game window now. Safe to call any time. The command line
	 * -ArenaHDR=0|1 overrides the saved choice for the run (not saved).
	 */
	void Apply();
	/** Flip HDR on / off live without saving (test switch -ArenaHDRToggle=<sec>, UArenaDisplaySubsystem). */
	void TestToggle();
}

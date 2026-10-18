#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "ArenaDisplay.generated.h"

/**
 * Which monitor a fullscreen game window uses (UArenaUserSettings::Monitor).
 *
 * The engine fullscreens on the window's current display, which on Wayland is whatever SDL lists first:
 * compositors don't let clients place windows, so the engine's "primary display" is just the first output
 * announced. This re-applies fullscreen on the chosen display through SDL (Wayland honours a fullscreen
 * request for a specific output). Choice 0 = automatic: the display at the desktop origin (0,0), which is
 * the primary monitor in the usual layouts; 1.. = displays ordered left to right, top to bottom.
 * Linux only; elsewhere the setting only shows "Automatic".
 */
namespace ArenaDisplay
{
	/** Number of selectable choices including "Automatic" (>= 1). */
	int32 NumChoices();
	/** Display text for a choice, e.g. "Automatic (Dell AW3225QF)" or "2: UGREEN-UHD". */
	FString GetChoiceName(int32 Choice);
	/** Move the fullscreen game window to the chosen display now (no-op when windowed or already there). */
	void Apply();

	// ---- field of view (wide / ultrawide screens) ----
	/**
	 * The player's FOV setting is defined the UT2004 way: horizontal degrees on a 4:3 screen (DefaultFOV 90). Wider
	 * screens keep that vertical extent and see more to the sides (Hor+): 90 is about 106 at 16:9, 121 at 21:9 and
	 * 139 at 32:9. The horizontal angle is capped at MaxHorizontalFOV (very wide windows then lose a little height
	 * instead of stretching further).
	 */
	constexpr float MaxHorizontalFOV = 150.f;
	/** Width / height of the game viewport (16:9 before a viewport exists). */
	float GetViewAspect();
	/** Horizontal FOV in degrees shown on a screen of the given aspect for a 4:3 FOV, capped at MaxHorizontalFOV. */
	float GetHorizontalFOV(float FOV43, float Aspect);
	/**
	 * FOV to give a camera whose AspectRatio is CameraAspect (UCameraComponent: 16:9, APlayerCameraManager: 4:3) so a
	 * 4:3 FOV shows correctly on the current viewport. The engine keeps the vertical FOV
	 * (AspectRatio_MaintainYFOV, Config/DefaultEngine.ini) and treats the camera FOV as horizontal at CameraAspect.
	 */
	float GetCameraFOV(float FOV43, float CameraAspect);

	// ---- window mode and resolution (UArenaUserSettings::WindowMode / ResolutionX / ResolutionY) ----
	enum EWindowChoice : int32 { WindowFullscreen = 0, WindowBorderless = 1, WindowWindowed = 2, NumWindowModes = 3 };
	FString GetWindowModeText(int32 Mode);
	/**
	 * Selectable resolutions for a window mode, smallest first. Fullscreen: the chosen display's own modes (SDL on
	 * Linux, the RHI's mode list elsewhere). Windowed: those plus common 16:10, 21:9 and 32:9 sizes that fit on the
	 * display. Borderless always uses the display's desktop size (one entry).
	 */
	TArray<FIntPoint> GetResolutions(int32 Mode);
	/** Desktop size of the chosen display. */
	FIntPoint GetDesktopResolution();
	/** Display text of the current resolution setting, e.g. "3440x1440 (21:9)". */
	FString GetResolutionText();
	/** Next / previous resolution of the current window mode's list (wraps around). */
	FIntPoint StepResolution(int32 Dir);
	/** Aspect label of a size: "16:9", "16:10", "21:9", "32:9", "4:3", ... or "2.40:1". */
	FString GetAspectText(FIntPoint Size);
	/** Apply the window mode and resolution settings now (and store them in GameUserSettings.ini for the next start). */
	void ApplyWindowSettings();
}

/** Applies the monitor choice once the game window exists (and again after the window mode changes), the window mode / resolution settings and HDR. */
UCLASS()
class OPENUT2K4_API UArenaDisplaySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	bool Tick(float DeltaTime);

	FTSTicker::FDelegateHandle TickHandle;
	/** Window settings and HDR applied once the game window exists. */
	bool bStartupApplied = false;
	double StartTime = 0.0;
	/** Test switches (non-shipping): seconds after startup to flip HDR, and to log the display state. */
	TArray<double> HDRToggleTimes;
	double WindowTestTime = 0.0;
};

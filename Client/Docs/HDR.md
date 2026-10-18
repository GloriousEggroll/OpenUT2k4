# HDR output

What Unreal Engine 5.8 can do for HDR output on our platforms (Vulkan on Linux and Windows, Metal on Mac), what the
game does about it (`ArenaHDR`, Settings: HDR / HDR Peak Brightness / HDR Paper White), and what was verified.

## How UE 5.8 does HDR output

* `r.HDR.EnableHDROutput` (1 = on) asks for an HDR swapchain. `IsHDREnabled()` is
  `GRHISupportsHDROutput && r.HDR.EnableHDROutput` (`Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp`).
* `r.HDR.Display.OutputDevice` picks the encoding the tonemapper writes: 0 sRGB, 1 Rec709, 2 gamma 2.2,
  3 / 4 ACES 1000 / 2000 nit ST 2084 (PQ, "HDR10"), 5 / 6 ACES 1000 / 2000 nit scRGB, 7 linear EXR, 8 linear.
  `r.HDR.Display.ColorGamut` picks the primaries: 0 Rec709 / sRGB, 1 DCI-P3, 2 Rec2020, 3 ACES, 4 ACEScg.
* `r.HDR.Display.MaxLuminance` (nits) shapes the ACES HDR tonemap curve (`HDRConfigureCVars`,
  `ConfigureACESTonemapParams`). `r.HDR.PaperWhite` / `r.HDR.PaperWhite.Mode` set the luminance of SDR white.
* UI: with `r.HDR.UI.CompositeMode 1` (default) Slate and canvas elements are drawn into an SDR UI buffer and
  composited over the HDR scene at `r.HDR.UI.Level` x `r.HDR.UI.Luminance` (or the paper white,
  `r.HDR.UI.Luminance.Mode 0`). Canvas batched elements switch their blending for that buffer
  (`BatchedElements.cpp`, `bCompositeUI`). So the HUD, the menus and the bitmap fonts stay SDR content shown at
  paper white instead of being read as PQ code values.
* `UGameUserSettings::EnableHDRDisplayOutput(bEnable, Nits)` sets these through the cvar sink
  `HDRSettingChangedSinkCallback` (`UnrealEngine.cpp`), which calls `FPlatformMisc::ChooseHDRDeviceAndColorGamut`
  to pick the output device for the platform.
* Slate re-checks the window's HDR state every frame (`FSlateViewportInfo::ResizeViewport` compares the HDR
  metadata) and resizes the RHI viewport with `GRHIHDRDisplayOutputFormat` when it changes, so HDR can switch
  live.

### Vulkan RHI (Linux and Windows)

* Instance extension `VK_EXT_swapchain_colorspace` is enabled by the RHI (`VulkanExtensions.cpp`).
* The swapchain picks its colour space from `r.HDR.Display.OutputDevice`: sRGB nonlinear, BT709 nonlinear, or
  `VK_COLOR_SPACE_HDR10_ST2084_EXT` for devices 3 / 4 (`VulkanSwapChain.cpp`). scRGB (devices 5 / 6) is not
  mapped and falls back to sRGB. It logs `Creating new VK swapchain with <present mode>, <format>, <colour space>`
  (in editor builds only for the first swapchain).
* The Vulkan RHI never sets `GRHISupportsHDROutput` (only the D3D11 / D3D12 and Metal RHIs do), so out of the box
  `r.HDR.EnableHDROutput` does nothing on Vulkan, and `RHIGetDisplaysInformation` reports no displays, so the engine
  knows nothing about the monitor's luminance.
* `VK_EXT_hdr_metadata` is not used by the RHI (the log shows it as available but not enabled), so no HDR10
  mastering metadata (max CLL / FALL) is sent; the compositor or display uses its defaults.
* `FVulkanDynamicRHI::RHIResizeViewport` only recreates the swapchain when the size, fullscreen flag or pixel format
  changes. SDR already uses a 10 bit back buffer (`r.DefaultBackBufferPixelFormat` A2B10G10R10), so a colour space
  change alone would keep the old swapchain.
* `FGenericPlatformMisc::ChooseHDRDeviceAndColorGamut` (Linux) is a no-op, so the engine would leave the output
  device at sRGB even with HDR on; on Windows it picks scRGB in editor builds (not presentable on Vulkan) and
  ST 2084 / Rec2020 in game builds.

### What ArenaHDR adds (Source/OpenUT2k4/ArenaHDR.cpp)

1. Once the game window exists, it creates a second Vulkan surface on the window (SDL_Vulkan_CreateSurface on
   Linux, vkCreateWin32SurfaceKHR on Windows), lists its formats with `vkGetPhysicalDeviceSurfaceFormatsKHR` and
   logs the colour spaces (`HDR: Vulkan surface offers ...`). HDR is available when HDR10 ST 2084 is offered with
   A2B10G10R10 (preferred) or RGBA16F.
2. On Linux it also reads SDL's display / window HDR properties (`SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN`,
   `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, SDR white level and headroom) and refuses HDR when the output is in SDR
   mode. The headroom gives the "Auto" peak brightness.
3. When the player turns HDR on: `GRHISupportsHDROutput = true`, `GRHIHDRDisplayOutputFormat` = the probed format,
   `UGameUserSettings::EnableHDRDisplayOutput(true, Peak)` plus paper white / UI luminance, then (set by code, which
   outranks the engine's device-profile values) `r.HDR.Display.OutputDevice` 3 (4 above 1000 nits),
   `r.HDR.Display.ColorGamut` 2 (Rec2020), `r.HDR.Display.MaxLuminance` = peak, `r.HDR.PaperWhite` = paper white
   (`r.HDR.PaperWhite.Mode 1`), `r.HDR.UI.Luminance.Mode 0` (UI follows paper white), `r.HDR.UI.CompositeMode 1`.
4. It bounces the window's RHI viewport through an 8 bit format so the next Slate frame recreates the swapchain in
   the new colour space (works live, both directions).
5. Turning HDR off restores sRGB output and reports no HDR support again, so SDR keeps the engine's normal path.

Our Brightness setting is an exposure bias in the post process (before the tonemapper), so it works the same in
HDR. Auto exposure is off project-wide (`r.DefaultFeature.AutoExposure=False`), and where a map's post process
volume turns it on, it adapts the scene before tonemapping as in SDR. The HDR tonemapper maps scene white to
paper white and highlights up to the peak luminance.

## Platform and compositor support

| Platform | What is needed | Status in this game |
|---|---|---|
| Linux, KDE Plasma 6 (KWin) | HDR on in Display Settings; KWin implements `wp_color_manager_v1` (Plasma 6.3+), Mesa 25.1+ (RADV / ANV / NVK) or NVIDIA 595+ expose HDR10 colour spaces | Supported (same path as labwc; not tested here) |
| Linux, GNOME 48+ (Mutter) | HDR toggle in Settings > Displays (Mutter 48+), colour management protocol | Supported by the same path (not tested here) |
| Linux, labwc 0.20 / wlroots 0.20.1+ | Vulkan renderer (`WLR_RENDERER=vulkan`), HDR output enabled for the monitor; labwc 0.20 is the first release with HDR10 output and colour-management-v1 | Tested on this machine: see Verification |
| Linux, labwc 0.9 / wlroots 0.19 | wlroots 0.19 only spoke the protocol; renderer / backend HDR support came in 0.20 | No HDR (setting shows not supported) |
| Linux, Sway 1.12+ | wlroots 0.20 | Should behave like labwc (not tested) |
| Linux, gamescope | `gamescope --hdr-enabled`; gamescope exposes HDR10 to Vulkan clients through its WSI layer when its own output (or nested host compositor) is HDR | See Verification |
| Linux, X11 / XWayland | X11 has no HDR colour spaces for Vulkan surfaces | Not possible (setting shows not supported) |
| Windows 10 / 11, Vulkan | "Use HDR" on in Windows display settings; AMD and NVIDIA Vulkan drivers then offer HDR10 ST 2084 with A2B10G10R10 (scRGB with RGBA16F) | Implemented through the same probe (Win32 surface); not built or tested in this pass |
| macOS, Metal | EDR: the Metal RHI always reports HDR support and uses `PF_PLATFORM_HDR_0` (RGBA16F, extended linear Display P3, scRGB 1000 / 2000 nit device from `FMacPlatformMisc::ChooseHDRDeviceAndColorGamut`) | Uses the engine's own path (`EnableHDRDisplayOutput`); the toggle is always offered because the RHI does not ask the display; not tested |

Notes:

* Mesa's Vulkan WSI got Wayland colour management in Mesa 25.1: it exposes `VK_EXT_swapchain_colorspace` colour spaces
  and `VK_EXT_hdr_metadata` when the compositor supports the protocol, so the old `VK_hdr_layer` is not needed.
* NVIDIA's proprietary driver lagged behind (forum reports through 2025); the Arch wiki lists nvidia-open 595.58.03+.
* An SDR output on an HDR-capable compositor still lists HDR10 colour spaces (the compositor would tone map), so the
  game also asks SDL whether the output is in HDR mode.
* No calibration screen yet: peak brightness and paper white are plain settings (Auto takes the peak from the
  compositor's reported headroom).

## Verification on this machine

Machine: AMD Radeon RX 9070 XT (RADV, Mesa 26.2.4), labwc 0.20.2 on wlroots 0.20.2, two 3840x2160 HDR monitors
(Dell AW3225QF on DP-2, UGREEN UHD on HDMI-A-1), SDL 3.4.4 (UE's bundled build) on Wayland.

* `wayland-info` shows `wp_color_manager_v1` version 2 with parametric image descriptions, the `st2084_pq`
  transfer function and `bt2020` primaries; both outputs currently use a PQ / BT.2020 image description with target
  luminance 0..380 nits (AW3225QF) and 0..427 nits (UGREEN).
* `vulkaninfo` on a Wayland surface (RADV GFX1201) lists `VK_COLOR_SPACE_HDR10_ST2084_EXT` for 15 formats including
  A2B10G10R10 and RGBA16F; under X11 / XWayland only sRGB nonlinear is listed. `VK_EXT_hdr_metadata` revision 3 is
  available.

Game runs (UnrealEditor -game, Development, windowed on the AW3225QF):

* Probe: `HDR: Vulkan surface offers 120 formats; colour spaces: SRGB_NONLINEAR, BT2020_LINEAR, HDR10_ST2084; HDR10
  with A2B10G10R10: yes, with RGBA16F: yes` and `HDR: SDL video driver 'wayland', display 'Dell Inc. AW3225QF ...':
  display HDR on, window HDR on, SDR white 1.00, headroom 49.26`. labwc describes its outputs by the PQ container
  (max 10000 nits, reference 203), so SDL's headroom (49 x 80 nits = 3941) is not the panel's peak; Auto ignores
  estimates outside 300..2000 nits and uses 1000. (The panel targets, 380 / 427 nits, are only in the protocol's
  `target_luminance`, which SDL does not expose.)
* `-ArenaHDR=1`: `HDR: output on (peak 1000 nits, paper white 200 nits, back buffer A2B10G10R10)`. A Wayland
  protocol trace (`WAYLAND_DEBUG=client`) then shows Mesa's WSI, for the new swapchain, calling
  `wp_color_manager_v1.get_surface`, `create_parametric_creator`, `set_primaries_named(6 = bt2020)`,
  `set_tf_named(11 = st2084_pq)`, `create`, the compositor answering `wp_image_description_v1.ready`, and
  `wp_color_management_surface_v1.set_image_description(...)` on the game's `wl_surface`: labwc accepted the HDR10
  surface. No protocol error (SDL only uses the separate surface feedback object, so it does not clash with Mesa).
* Live switch (`-ArenaHDRToggle=14`): `HDR: output off`, and the trace shows Mesa destroying the colour management
  surface for the new sRGB swapchain (back to the compositor's default sRGB).
* Editor builds log the RHI's `Creating new VK swapchain with ...` line only for the first swapchain (UE prints it
  once `WITH_EDITOR`), so the HDR swapchain itself is not in the log there; packaged builds log every swapchain.
* gamescope 3.16 nested in labwc (`gamescope --hdr-enabled -- <game> -sdlvideodriver=x11`): gamescope's Wayland
  backend reports `bExposeHDRSupport: false` (it requires the named `srgb` transfer function from the host
  compositor, which labwc does not list; same cause as gamescope issue 2221 on Plasma 6.7), so its WSI layer offers
  only sRGB and the game shows HDR as not supported. With `--hdr-debug-force-support` the WSI layer offers
  HDR10_ST2084 (A2B10G10R10) and scRGB, so the game can enable HDR inside gamescope; gamescope then outputs SDR to
  labwc because the host path is not exposed. Running gamescope as the session (embedded / DRM backend, e.g. a
  SteamOS-style session) is the configuration where gamescope HDR is meant to work; not tested here.

Screenshots: Unreal's screenshot path reads back the viewport's colour buffer. With HDR on, that buffer holds PQ
encoded BT.2020 values, which the screenshot saves as if they were sRGB, so HDR screenshots look washed out and
desaturated; they show that the HDR encoding is active, not what the monitor shows.

## Sources

* Unreal Engine 5.8 source: `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapChain.cpp`, `VulkanViewport.cpp`,
  `VulkanExtensions.cpp`, `RenderCore/Private/RenderCore.cpp`, `Engine/Private/UnrealEngine.cpp`
  (`HDRSettingChangedSinkCallback`), `Engine/Private/GameUserSettings.cpp`,
  `SlateRHIRenderer/Private/SlateRHIRenderer.cpp`, `Engine/Private/BatchedElements.cpp`,
  `Apple/MetalRHI/Private/MetalRHI.cpp`, `Core/Public/*/…PlatformMisc.h` (`ChooseHDRDeviceAndColorGamut`).
* Epic: [High Dynamic Range Display Output](https://dev.epicgames.com/documentation/zh-cn/unreal-engine/high-dynamic-range-display-output-in-unreal-engine)
* Khronos: [VkColorSpaceKHR](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkColorSpaceKHR.html),
  [VK_EXT_swapchain_colorspace](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_swapchain_colorspace.html)
* Phoronix: [Mesa's Vulkan WSI Now Supports Wayland Color Management](https://www.phoronix.com/news/Mesa-Vulkan-WSI-HDR-CM)
* Phoronix: [wlroots Merges Wayland Color Management / HDR Support](https://www.phoronix.com/news/wlroots-color-management),
  [wlroots 0.20 Released, Sway 1.12-rc1](https://phoronix.com/news/wlroots-0.20-sway-1.12-rc1)
* Linuxiac: [Labwc 0.20 Released with wlroots 0.20 Support](https://linuxiac.com/labwc-0-20-wayland-compositor-released-with-wlroots-0-20-support/)
* Arch Wiki: [HDR](https://wiki.archlinux.org/title/HDR) (KDE, GNOME 48 / Mutter, gamescope, Mesa 25.1, NVIDIA)
* Zamundaaa: [VK_hdr_layer](https://github.com/Zamundaaa/VK_hdr_layer) (obsolete with Mesa 25.1+)
* Gamescope: [issue 2221 (Plasma 6.7 nested HDR regression)](https://github.com/ValveSoftware/gamescope/issues/2221)
* SDL: [SDL_GetWindowProperties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties) (HDR_enabled, SDR white level, headroom)
* Apple: [HDR content (EDR)](https://developer.apple.com/documentation/metal/hdr-content)
* NVIDIA forums: [Wayland color management protocol not yet supported by Nvidia driver](https://forums.developer.nvidia.com/t/wayland-color-management-protocol-not-yet-supported-by-nvidia-driver/338444)

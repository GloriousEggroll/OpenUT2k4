# Upscaling, frame generation and low latency

Status as of October 2026, for OpenUT2k4 on Unreal Engine 5.8 with the Vulkan RHI (Linux, Windows) and Metal (Mac).
Implementation: `Plugins/ArenaUpscaling` (renderer side) and `Source/OpenUT2k4/ArenaUpscalerSettings.*` (settings).

## Summary

| Feature | Status in OpenUT2k4 | Why |
|---------|---------------------|-----|
| AMD FSR 4 (ML upscaler) | **Not possible** | AMD ships FSR 4 only as prebuilt, signed **DirectX 12** DLLs for Windows, under a binary-only licence that forbids reverse engineering. The FidelityFX SDK 2.x states that Vulkan is not supported. There is no Vulkan, Linux or Metal build, no source, and nothing an MIT project may vendor. |
| AMD FSR 3.1 upscaler | **Implemented** (setting *Upscaler: AMD FSR 3.1*) | FSR 3.1.4 shader source from FidelityFX SDK v1.1.4 is MIT. We compile it with Unreal's shader compiler, so it runs on Vulkan, D3D12 and Metal on any SM5 GPU, RDNA 4 included. |
| TSR | Kept, still the default | It is built in. |
| FSR 3.1 frame generation | **Not implemented** | It needs the FidelityFX frame-interpolation *swapchain*, which replaces the application's swapchain and present. Its Vulkan version is Win32-only code, and the UE 5.8 Vulkan RHI has no hook to replace its swapchain. See [Frame generation](#frame-generation). |
| Low latency: VK_AMD_anti_lag | **Implemented** where the driver provides it natively (AMD's driver; RADV from Mesa 26.3) | Mesa 26.2 (installed here) only provides it through `VK_LAYER_MESA_anti_lag`, and that layer makes Unreal lose the device (a layer bug, see below). In that case we fall back to engine frame pacing. |
| Low latency: VK_NV_low_latency2 (Reflex on Vulkan) | **Not implemented** | It needs `VkSwapchainLatencyCreateInfoNV` at swapchain creation and present IDs on every `vkQueuePresentKHR`. The UE 5.8 Vulkan RHI does neither, and a plugin cannot add them. |

## Research

### FSR 4 and the FidelityFX SDK 2.x

- The FidelityFX / "AMD FSR" SDK 2.0 (August 2025) brought FSR 4 to developers. The current release is 2.3.0
  "Redstone" (June 2026), with FSR Upscaling 4.1.1, ML frame generation 4.0.1, Ray Regeneration and Radiance Caching.
  The repository README lists in its known issues: "Vulkan is currently not supported in SDK". [1][2][3]
- Every 2.x release ships prebuilt, signed DLLs (`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`,
  `amd_fidelityfx_framegeneration_dx12.dll` in `Kits/FidelityFX/signedbin`). Only the DX12 backend has source. [1][2]
- AMD's FSR 4 page: "Supports: DirectX 12". "FSR 4 is available only as a prebuilt, signed DLLs as part of FidelityFX
  SDK official releases". It lists Windows 10/11 as the supported operating systems. [4]
- The SDK 2.x licence (`Kits/FidelityFX/docs/license.md`) allows redistribution "in binary form only" and states
  "No reverse engineering, decompilation, or disassembly of this Software is permitted". That is not MIT. The 1.x SDK
  (FSR 3.1) was MIT and shipped full source with a Vulkan backend. [5][6]
- On Linux, FSR 4 runs only in *Windows* DX12 games, through Proton/vkd3d-proton. RADV implements the FP8 and
  matrix paths it needs (`VK_EXT_shader_float8` and `VK_KHR_cooperative_matrix` on RDNA 4; this machine's Mesa 26.2.4
  reports `shaderFloat8 = true` on the RX 9070 XT), with FP8 emulation on RDNA 3. A native Vulkan game cannot use it,
  because there is no Vulkan FSR 4 library. [7][8]
- AMD's Unreal plugin: the current "AMD FSR" plugin (v4.1.1, UE 5.3 to 5.8, FSR Upscaling 4.1.1, FSR 3.1.5, frame
  generation 4.0.1 and 3.1.6) is built on the SDK 2.3 DLLs. Its guide says "The RHI backend has been removed". Frame
  generation pacing uses a D3D12 proxy swapchain, ML frame generation needs Windows 11, and Vulkan and Linux are not
  mentioned. The older FSR 3.1.4 plugin (up to UE 5.6/5.7) had an RHI fallback backend with basic frame generation,
  but it does not support UE 5.8. AMD's Anti-Lag 2 Unreal plugin targets DX11/DX12 and is incompatible with the RHI
  frame-generation backend. [9][10][11][12]

**Conclusion:** FSR 4 cannot be added to a Vulkan/Metal game, and its binaries cannot be shipped in an MIT project.
OpenUT2k4 uses FSR 3.1 instead (all GPUs, all RHIs). If AMD releases FSR 4 for Vulkan with redistributable terms,
it would replace the shader passes in `ArenaFSR3Upscaler.cpp` behind the same setting.

### FSR 3.1 (what we use)

- FidelityFX SDK v1.1.4 (May 2025), FSR 3.1.4, MIT (`LICENSE.txt`). It has DX12 and Vulkan backends. Source for all
  shaders is in `sdk/include/FidelityFX/gpu`. [6][13]
- We do not use the SDK's C++ runtime or its Vulkan backend. That backend needs precompiled shader blobs, which come
  from a Windows-only shader tool. Instead the plugin compiles the SDK's HLSL passes with Unreal's shader compiler
  (DXC to SPIR-V for Vulkan, to Metal on Mac) and reimplements the small host-side dispatch logic on RDG. The
  previous AMD Unreal plugin's "RHI backend" worked the same way.

### Low latency

- **VK_AMD_anti_lag** has two calls. `vkAntiLagUpdateAMD(stage INPUT)` runs before input sampling, and the driver
  sleeps there so the CPU does not run ahead of the GPU. `stage PRESENT` runs before present.
- Mesa 25.3 added it as an implicit Vulkan layer, `VK_LAYER_MESA_anti_lag`. The layer is disabled unless
  `ENABLE_LAYER_MESA_ANTI_LAG=1`. Mesa 26.3 (stable November 2026) replaces it with a native RADV implementation on a
  shared `vk_frame_pacer`. [14][15][16]
- AMD's Windows driver exposes the extension natively (Anti-Lag 2 on Vulkan needs Adrenalin 24.9.1 or later). [12]
- **Mesa 26.2 layer bug (found here):** on `vkQueueSubmit` with a `VkTimelineSemaphoreSubmitInfo`, the layer appends
  its own signal semaphore *at the end* of `pSignalSemaphores`. It then copies the application's signal values to
  `pSignalSemaphoreValues[1..]`, which shifts them by one. Unreal's timeline semaphore is then signalled with value 0,
  and the first anti-lag frame ends in `VK_ERROR_DEVICE_LOST`. We reproduced this on this machine with both TSR and
  FSR. The `vkQueueSubmit2` path of the layer is correct, but UE 5.8 uses `vkQueueSubmit`. Source:
  `src/vulkan/anti-lag-layer/anti_lag_layer.c`, `anti_lag_QueueSubmit` (Mesa 26.2 branch). [17]
- **VK_NV_low_latency2** (Reflex) needs the swapchain to be created with `VkSwapchainLatencyCreateInfoNV`,
  `vkSetLatencySleepModeNV` / `vkLatencySleepNV` on that swapchain, and `VkPresentIdKHR` on each present. UE 5.8 does
  none of these, and the swapchain handle is private to the RHI. Mesa does not implement it. The third-party
  `low_latency_layer` provides both extensions on any GPU; with it, our VK_AMD_anti_lag path applies. [16][18]

## What is implemented

### Upscaler setting (`Upscaler`, `Upscaler Quality`, `FSR Sharpness`)

- **TSR** (default): Unreal's Temporal Super Resolution.
- **AMD FSR 3.1**: `FArenaFSR3Upscaler`, a third-party `ITemporalUpscaler` installed per view family by a scene view
  extension. It falls back to TSR where SM5 compute is missing.
- **Off (native)**: 100 % resolution with FXAA, no temporal reconstruction.
- Quality (TSR and FSR): Auto (Unreal's display-based default), Native AA 100 %, Quality 67 %, Balanced 59 %,
  Performance 50 %, Ultra Performance 33 %, through `r.ScreenPercentage`.
- FSR Sharpness: RCAS 0 to 100 % (default 20 %, 0 skips the pass).
- Everything applies live and is saved in `UArenaUserSettings`.

How the FSR integration works (`ArenaFSR3Upscaler.cpp`, `ArenaFSR3Inputs.usf`, `ArenaFSR3Upscaler.usf`):

1. A convert pass turns Unreal's encoded velocity into FSR motion vectors (UV offsets to the previous frame). Where
   no velocity was written (static geometry), it reconstructs camera motion from depth and `ClipToPrevClip`. It also
   copies depth to R32F and writes the exposure the tonemapper will apply (eye adaptation divided by pre-exposure).
2. The eight FSR 3.1.4 passes run as in the SDK: prepare inputs, luma pyramid (SPD), shading change pyramid,
   shading change, prepare reactivity, luma instability, accumulate, and RCAS. The inputs are linear HDR colour,
   reversed infinite depth, render-resolution motion vectors and Unreal's jitter (`TemporalJitterPixels`).
3. The history (accumulation, luma, luma history, upscaled colour and frame info) lives in the view state as the
   upscaler's `IHistory`. It is reset on camera cuts and size changes.
4. FSR runs where TSR runs: after depth of field and before motion blur, bloom and tonemapping. Our Brightness view
   extension, ray tracing and Lumen therefore work unchanged, and HDR output (tonemapper / swapchain colour space)
   is unaffected. The HUD, menus and console are drawn afterwards by Canvas/Slate at native resolution, so they are
   never upscaled, interpolated or blurred.

Not done: reactive / transparency masks from the renderer (FSR uses its default all-zero masks, as AMD's sample does
without them). Fast particles and translucent effects can therefore show slightly more ghosting than with TSR.

### Low Latency setting

- **On, with a usable `VK_AMD_anti_lag`:** AMD Anti-Lag. The extension (with its `antiLag` feature) is requested
  from the Vulkan RHI at PostConfigInit through `IVulkanDynamicRHI::AddEnabledDeviceExtensionsAndLayers`. Each frame,
  the engine's `IMaxTickRateHandlerModule::HandleMaxTickRate`, which runs before input is pumped, calls
  `vkAntiLagUpdateAMD(ON, stage INPUT, maxFPS = frame rate cap)`. `ILatencyMarkerModule::SetPresentLatencyMarkerStart`,
  which the Vulkan RHI calls right before `vkQueuePresentKHR`, calls `stage PRESENT` with the same frame index.
- **On, otherwise:** engine frame pacing on every RHI. `r.OneFrameThreadLag 0` makes the game thread wait for the
  render thread every frame, so input is sampled closer to rendering.
- The setting shows which backend is in use: "On (AMD Anti-Lag)" or "On (frame pacing)".
- The Mesa layer is never loaded by us. If `ENABLE_LAYER_MESA_ANTI_LAG=1` is set, the extension it provides is
  ignored. To test a fixed layer, use `-ArenaMesaAntiLagLayer=1`.
- Verification: the log (`LogArenaUpscaling`) prints which backend is active. Every 10 s it also prints the average
  time from input sampling to the present call, plus the `vkAntiLagUpdateAMD` call counts
  (`r.Arena.LowLatency.LogInterval`). Players can compare MangoHud's frame time graph and the in-game feel.
  `VK_LOADER_DEBUG=layer` shows which layers load.

## Frame generation

FSR 3.1 frame interpolation (MIT, SDK v1.1.4) has two parts:

- GPU passes: optical flow and frame interpolation, about 15 compute shaders. These could be compiled the same way
  as the upscaler.
- A frame-interpolation **swapchain**. It replaces `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR` and
  `vkQueuePresentKHR`, keeps its own presenter thread for pacing, presents the interpolated frame between real ones,
  and composites the UI on both.

This is not feasible on UE 5.8's Vulkan RHI without engine modification, for three reasons:

1. The SDK's Vulkan swapchain (`sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp`,
   about 3000 lines) is written against Win32: `CreateEvent`, `CRITICAL_SECTION`, `QueryPerformanceCounter`,
   `SetThreadPriority`. It would need a port and its own testing before it could run on Linux.
2. The UE Vulkan RHI owns the swapchain (`FVulkanSwapChain`, `FVulkanViewport::Present`) and has no supported way to
   substitute it. The entry points are exported function-pointer globals in a *private* header
   (`VulkanLoader.h`, `VulkanDynamicAPI`). Overwriting `vkQueuePresentKHR` and its siblings from a plugin and forcing
   a swapchain rebuild would be an engine patch in disguise. It would also break with every engine update and
   conflict with HDR swapchain work.
3. Without a paced presenter (a separate present thread with timing), presenting the interpolated frame from the
   render thread produces uneven frame times. AMD's own note on its RHI fallback backend says it gives only basic
   frame generation with worse pacing. With vsync/FIFO, frame generation also adds about one frame of latency.

Options for later:

- Patch the engine's Vulkan viewport, with a source build, to route present through a portable port of the FFX
  swapchain.
- Wait for AMD to ship Vulkan frame generation with redistributable terms.
- Use driver-side frame generation outside the game: AMD Fluid Motion Frames on Windows, or Vulkan layers such as
  `lsfg-vk` on Linux. Both interpolate the HUD as well.

The settings therefore have no Frame Generation row. A row that does nothing would mislead players.

## Measurements

See the README section "Upscaling and low latency" for the numbers (MangoHud and UE frame stats, RX 9070 XT, Mesa 26.2.4, 1080p and 4K, Native / TSR / FSR Quality / FSR Performance, Low Latency off and on).

## Sources

1. FidelityFX SDK repository (README, SDK 2.3.0, known issues): https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
2. FidelityFX SDK releases (v2.0.0 to v2.3.0 signed DLL packages; v1.1.x Vulkan fixes): https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases
3. AMD FidelityFX SDK 2.0 announcement: https://gpuopen.com/learn/amd-fidelityfx-sdk-2-0/ ; Phoronix: https://www.phoronix.com/news/AMD-FidelityFX-SDK-2.0 ; OC3D (DX12 only): https://overclock3d.net/news/gpu-displays/amd-brings-fsr-4-to-developers-with-its-fidelityfx-sdk-2-0/
4. AMD FSR 4 page: https://gpuopen.com/fidelityfx-super-resolution-4/
5. SDK 2.x licence: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/license.md
6. SDK v1.1.4 (MIT): https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4
7. FSR 4 on Linux / RDNA 4 (CachyOS guide): https://discuss.cachyos.org/t/how-to-use-fsr4-on-rdna4-gpus-outdated/9004
8. FSR 4 on RDNA 3 under Linux (FP8 emulation, PCGH): https://www.pcgameshardware.de/Grafikkarten-Grafikkarte-97980/News/FSR-4-unter-Linux-auch-mit-RDNA-3-1478172/
9. AMD FSR Unreal Engine plugin guide (v4.1.1, UE 5.3 to 5.8): https://gpuopen.com/learn/ue-fsr/
10. AMD FSR plugin for UE 5.8: https://gpuopen.com/learn/amd-fsr-plugin-updated-for-unreal-engine-58/
11. FSR 3.1.4 Unreal plugin guide (RHI backend, UE up to 5.6/5.7): https://gpuopen.com/learn/ue-fsr3/
12. AMD Radeon Anti-Lag 2: https://gpuopen.com/anti-lag-2/
13. AMD FSR 3 overview (Vulkan backend in 3.1): https://gpuopen.com/fidelityfx-superresolution-3/
14. Mesa 25.3 anti-lag layer: https://www.phoronix.com/news/Mesa-Vulkan-AMD-Anti-Lag
15. Native RADV VK_AMD_anti_lag in Mesa 26.3: https://phoronix.com/news/Mesa-26.3-RADV-VK_AMD_anti_lag ; proposal: https://www.phoronix.com/news/Valve-New-AMD-Anti-Lag
16. low_latency_layer (VK_NV_low_latency2 and VK_AMD_anti_lag on any GPU): https://www.gamingonlinux.com/2026/05/new-low-latency-layer-brings-reflex-and-anti-lag-2-to-amd-and-intel-gpus-on-linux/
17. Mesa anti-lag layer source (26.2): https://gitlab.freedesktop.org/mesa/mesa/-/blob/26.2/src/vulkan/anti-lag-layer/anti_lag_layer.c
18. VK_NV_low_latency2 specification: https://registry.khronos.org/vulkan/specs/latest/man/html/VK_NV_low_latency2.html

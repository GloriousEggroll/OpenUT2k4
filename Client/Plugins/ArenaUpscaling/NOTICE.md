# ArenaUpscaling third-party code

`Shaders/Private/FidelityFX/` contains GPU shader headers of the AMD FidelityFX SDK v1.1.4 (FSR 3.1.4 upscaler,
Single Pass Downsampler, FSR 1 RCAS), Copyright (C) 2024 Advanced Micro Devices, Inc., MIT licence
(`Shaders/Private/FidelityFX/LICENSE.txt`). Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4
(`sdk/include/FidelityFX/gpu/`).

Modifications made for OpenUT2k4 (each modified file says so in its header):

- every file renamed from `.h` to `.ush` (Unreal only accepts `.ush`/`.usf` shader includes) and its `#include`
  paths rewritten to absolute Unreal virtual paths (`/Plugin/ArenaUpscaling/Private/FidelityFX/...`);
- `fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.ush`: resources and constant buffer members declared as plain
  globals without register bindings and with plain HLSL types, so Unreal binds them by name.

The algorithm code is otherwise unchanged. Everything else in this plugin (`Source/`, `ArenaFSR3*.usf`) is OpenUT2k4
code under the project's MIT licence; `ArenaFSR3Upscaler.cpp` reimplements the SDK's host-side dispatch logic
(`sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`, MIT) on Unreal's render graph.

No AMD binaries (DLLs, signed or otherwise) are included or loaded. FSR 4 is not included: AMD ships it only as
signed DirectX 12 DLLs for Windows under a binary-only licence (see `Docs/Upscaling.md`).

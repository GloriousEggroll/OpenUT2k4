#!/usr/bin/env bash
# Generate the OpenUT2k4 content (materials, sounds, maps) with the editor's Python plugin.
#
# Usage: Scripts/generate-content.sh [UE_ROOT]
#   UE_ROOT defaults to $UE_ROOT, then ${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x
#
# The OpenUT2k4 editor module must be built first (build-linux.sh does this), otherwise the
# editor cannot load the project. Set OPENUT2K4_BUILD_EDITOR=1 to build it here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT="${PROJECT_DIR}/OpenUT2k4.uproject"
UE_ROOT="${1:-${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}}"
EDITOR="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd"

if [[ ! -x "${EDITOR}" ]]; then
    echo "error: UnrealEditor-Cmd not found at ${EDITOR} (set UE_ROOT or pass the engine path)" >&2
    exit 1
fi

if [[ "${OPENUT2K4_BUILD_EDITOR:-0}" == "1" ]]; then
    "${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh" OpenUT2k4Editor Linux Development \
        "-Project=${PROJECT}" -WaitMutex
fi

echo "==> Generating content into ${PROJECT_DIR}/Content"
"${EDITOR}" "${PROJECT}" -run=pythonscript "-script=${PROJECT_DIR}/Tools/GenerateContent.py" \
    -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput

if [[ ! -f "${PROJECT_DIR}/Content/Materials/M_ArenaSurface.uasset" ]]; then
    echo "error: content generation did not produce Content/Materials/M_ArenaSurface.uasset" >&2
    exit 1
fi
echo "==> Content generated"

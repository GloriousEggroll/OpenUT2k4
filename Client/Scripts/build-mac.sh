#!/usr/bin/env bash
# Build, cook and package OpenUT2k4 for macOS (Shipping).
#
# Note: Unreal Engine has no Vulkan RHI on macOS, so the Mac build renders with Metal
# (SF_METAL_SM5/SM6, see Config/DefaultEngine.ini). Linux and Windows use Vulkan.
#
# Usage: Scripts/build-mac.sh [--with-imports] [UE_ROOT]
#   UE_ROOT defaults to $UE_ROOT, then "/Users/Shared/Epic Games/UE_5.8"
#   Default: a release package with no UT2004 data (checked by Scripts/check-release.sh). Output: Dist/Mac/
#   --with-imports: developer package that also cooks the local editor imports (never distribute it).
#            Output: Dist/Mac-WithImports/
set -euo pipefail

WITH_IMPORTS=0
POSITIONAL=()
for arg in "$@"; do
    case "${arg}" in
        --with-imports) WITH_IMPORTS=1 ;;
        *) POSITIONAL+=("${arg}") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT="${PROJECT_DIR}/OpenUT2k4.uproject"
UE_ROOT="${1:-${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}}"
RUNUAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
BUILD_SH="${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh"
ARCHIVE_DIR="${PROJECT_DIR}/Dist/Mac"
[[ ${WITH_IMPORTS} -eq 1 ]] && ARCHIVE_DIR="${PROJECT_DIR}/Dist/Mac-WithImports"

if [[ -x "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd" ]]; then
    EDITOR="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"
else
    EDITOR="${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
fi

for f in "${RUNUAT}" "${BUILD_SH}" "${EDITOR}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: ${f} not found (set UE_ROOT or pass the engine path)" >&2
        exit 1
    fi
done

if [[ ! -d "${PROJECT_DIR}/Content/Materials" ]]; then
    echo "==> Content/Materials missing: building OpenUT2k4Editor and generating content"
    "${BUILD_SH}" OpenUT2k4Editor Mac Development "-Project=${PROJECT}" -WaitMutex
    "${EDITOR}" "${PROJECT}" -run=pythonscript "-script=${PROJECT_DIR}/Tools/GenerateContent.py" \
        -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput
    if [[ ! -f "${PROJECT_DIR}/Content/Materials/M_ArenaSurface.uasset" ]]; then
        echo "error: content generation failed" >&2
        exit 1
    fi
fi

MAPS="/Game/Maps/MainMenu+/Game/Maps/Arena"
EXTRA=()
if [[ ${WITH_IMPORTS} -eq 1 ]]; then
    # Local UT2004 editor imports (git-ignored, differ per machine): add their maps and the WithImports config.
    EXTRA+=(-CustomConfig=WithImports)
    if [[ -d "${PROJECT_DIR}/Content/Imported/Maps" ]]; then
        while IFS= read -r -d '' UMAP; do
            MAPS+="+/Game/Imported/Maps/$(basename "${UMAP}" .umap)"
        done < <(find "${PROJECT_DIR}/Content/Imported/Maps" -maxdepth 1 -name '*.umap' -print0 | sort -z)
    fi
fi

# Start from an empty output folder so files of an older package (or a --with-imports one) never linger.
rm -rf "${ARCHIVE_DIR}"

echo "==> BuildCookRun (Mac, Shipping, Metal$([[ ${WITH_IMPORTS} -eq 1 ]] && echo ', with local imports - do not distribute'))"
"${RUNUAT}" BuildCookRun \
    "-project=${PROJECT}" \
    -platform=Mac \
    -clientconfig=Shipping \
    "-map=${MAPS}" \
    -build -cook -stage -pak -iostore \
    -archive "-archivedirectory=${ARCHIVE_DIR}" \
    "${EXTRA[@]+"${EXTRA[@]}"}" \
    -nop4 -utf8output -unattended

# Licence and notices next to the executable
cp "${PROJECT_DIR}/../LICENSE" "${PROJECT_DIR}/../NOTICE.md" "${ARCHIVE_DIR}/"

if [[ ${WITH_IMPORTS} -eq 0 ]]; then
    "${SCRIPT_DIR}/check-release.sh" "${ARCHIVE_DIR}" "${UE_ROOT}"
    # release archive + detached signature, only with OPENUT2K4_SIGNING_KEY (Server/Docs/Validator.md "Release signing")
    VERSION="$(sed -n 's/^ProjectVersion=//p' "${PROJECT_DIR}/Config/DefaultGame.ini" | tr -d '\r')"
    "${SCRIPT_DIR}/sign-release.sh" "${ARCHIVE_DIR}" "OpenUT2k4-${VERSION}-Mac"
fi

echo "==> Done: ${ARCHIVE_DIR}"

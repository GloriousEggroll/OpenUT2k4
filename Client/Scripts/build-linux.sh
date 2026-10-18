#!/usr/bin/env bash
# Build, cook and package OpenUT2k4 for Linux (Shipping, Vulkan).
#
# Usage: Scripts/build-linux.sh [--with-imports] [--development] [UE_ROOT]
#   UE_ROOT defaults to $UE_ROOT, then ${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x
#   Default: a release package with no UT2004 data (players convert their own install on first start),
#            checked by Scripts/check-release.sh afterwards. Output: Dist/Linux/
#   --with-imports: developer package that also cooks the local editor imports (Content/Imported).
#            It contains UT2004 data and must never be distributed. Output: Dist/Linux-WithImports/
#   --development: Development configuration instead of Shipping (log output, -map overrides work).
set -euo pipefail

WITH_IMPORTS=0
CONFIG=Shipping
POSITIONAL=()
for arg in "$@"; do
    case "${arg}" in
        --with-imports) WITH_IMPORTS=1 ;;
        --development) CONFIG=Development ;;
        *) POSITIONAL+=("${arg}") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT="${PROJECT_DIR}/OpenUT2k4.uproject"
UE_ROOT="${1:-${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}}"
RUNUAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
BUILD_SH="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"
ARCHIVE_DIR="${PROJECT_DIR}/Dist/Linux"
[[ ${WITH_IMPORTS} -eq 1 ]] && ARCHIVE_DIR="${PROJECT_DIR}/Dist/Linux-WithImports"
[[ ${CONFIG} == Development ]] && ARCHIVE_DIR+="-Dev"

for f in "${RUNUAT}" "${BUILD_SH}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: ${f} not found (set UE_ROOT or pass the engine path)" >&2
        exit 1
    fi
done

# Generated content (materials/sounds/maps) is produced by the editor, which needs the
# project's editor module, so build OpenUT2k4Editor first.
if [[ ! -d "${PROJECT_DIR}/Content/Materials" ]]; then
    echo "==> Content/Materials missing: building OpenUT2k4Editor and generating content"
    "${BUILD_SH}" OpenUT2k4Editor Linux Development "-Project=${PROJECT}" -WaitMutex
    "${SCRIPT_DIR}/generate-content.sh" "${UE_ROOT}"
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

echo "==> BuildCookRun (Linux, ${CONFIG}$([[ ${WITH_IMPORTS} -eq 1 ]] && echo ', with local imports - do not distribute'))"
"${RUNUAT}" BuildCookRun \
    "-project=${PROJECT}" \
    -platform=Linux \
    "-clientconfig=${CONFIG}" \
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
    "${SCRIPT_DIR}/sign-release.sh" "${ARCHIVE_DIR}" "OpenUT2k4-${VERSION}-Linux$([[ ${CONFIG} == Development ]] && echo -Dev)"
fi

echo "==> Done: ${ARCHIVE_DIR}"

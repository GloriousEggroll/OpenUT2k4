#!/usr/bin/env bash
# Build, cook and package the OpenUT2k4 dedicated server for macOS (Server/Docs/DedicatedServer.md "Building").
# UNVERIFIED: written on Linux without a Mac to run it on.
#
# Usage: Server/build-mac.sh [--shipping] [--server-target] [UE_ROOT]
#   UE_ROOT defaults to $UE_ROOT, then /Users/Shared/Epic Games/UE_5.8
#   Output: Client/Dist/Server-Mac/ (no UT2004 data; checked by Scripts/check-release.sh)
#   Default: the OpenUT2k4Server target as a Game target (an installed engine), Development configuration.
#   --shipping       Shipping configuration (no log file on an installed engine)
#   --server-target  a source-built engine: a real Server target (UE_SERVER), cooked for MacServer
set -euo pipefail

CONFIG=Development
SERVER_TARGET=0
POSITIONAL=()
for arg in "$@"; do
    case "${arg}" in
        --shipping) CONFIG=Shipping ;;
        --server-target) SERVER_TARGET=1 ;;
        *) POSITIONAL+=("${arg}") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../Client" && pwd)"
PROJECT="${PROJECT_DIR}/OpenUT2k4.uproject"
UE_ROOT="${1:-${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}}"
RUNUAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
BUILD_SH="${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh"
ARCHIVE_DIR="${PROJECT_DIR}/Dist/Server-Mac"
[[ ${CONFIG} == Shipping ]] && ARCHIVE_DIR+="-Shipping"

for f in "${RUNUAT}" "${BUILD_SH}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: ${f} not found (set UE_ROOT or pass the engine path)" >&2
        exit 1
    fi
done
if [[ ! -d "${PROJECT_DIR}/Content/Materials" ]]; then
    echo "error: Content/Materials missing: run Client/Scripts/build-mac.sh once (it generates the content)" >&2
    exit 1
fi

rm -rf -- "${ARCHIVE_DIR:?}"

if [[ ${SERVER_TARGET} -eq 1 ]]; then
    export OPENUT2K4_SERVER_TARGET_TYPE=Server
    TARGET_ARGS=(-server -noclient -serverplatform=Mac "-serverconfig=${CONFIG}" -target=OpenUT2k4Server)
else
    export OPENUT2K4_SERVER_TARGET_TYPE=Game
    TARGET_ARGS=(-platform=Mac "-clientconfig=${CONFIG}" -target=OpenUT2k4Server)
fi

echo "==> BuildCookRun OpenUT2k4Server (Mac, ${CONFIG})"
"${RUNUAT}" BuildCookRun \
    "-project=${PROJECT}" \
    "${TARGET_ARGS[@]}" \
    "-map=/Game/Maps/MainMenu+/Game/Maps/Arena" \
    -build -cook -stage -pak -iostore -nodebuginfo \
    -archive "-archivedirectory=${ARCHIVE_DIR}" \
    "-stagingdirectory=${PROJECT_DIR}/Saved/StagedBuilds/Server" \
    -nop4 -utf8output -unattended

for SUB in MacServer Mac; do
    if [[ -d "${ARCHIVE_DIR}/${SUB}" ]]; then
        shopt -s dotglob
        mv "${ARCHIVE_DIR}/${SUB}"/* "${ARCHIVE_DIR}/"
        rmdir "${ARCHIVE_DIR}/${SUB}"
        shopt -u dotglob
    fi
done
find "${ARCHIVE_DIR}" -name '*.dSYM' -prune -exec rm -rf {} +

mkdir -p "${ARCHIVE_DIR}/Server/mac"
cp "${SCRIPT_DIR}/Package/openut2k4-server" "${SCRIPT_DIR}/Package/ucc" "${ARCHIVE_DIR}/"
cp "${SCRIPT_DIR}/Package/OpenUT2k4Server.ini" "${ARCHIVE_DIR}/Server/"
cp "${SCRIPT_DIR}/Package/mac/"* "${ARCHIVE_DIR}/Server/mac/"
chmod +x "${ARCHIVE_DIR}/openut2k4-server" "${ARCHIVE_DIR}/ucc" "${ARCHIVE_DIR}/Server/mac/"*.sh
cp "${SCRIPT_DIR}/../LICENSE" "${SCRIPT_DIR}/../NOTICE.md" "${ARCHIVE_DIR}/"
"${SCRIPT_DIR}/package-validator.sh" "${ARCHIVE_DIR}"

"${PROJECT_DIR}/Scripts/check-release.sh" "${ARCHIVE_DIR}" "${UE_ROOT}"
# release archive + detached signature, only with OPENUT2K4_SIGNING_KEY (Server/Docs/Validator.md "Release signing")
VERSION="$(sed -n 's/^ProjectVersion=//p' "${PROJECT_DIR}/Config/DefaultGame.ini" | tr -d '\r')"
"${PROJECT_DIR}/Scripts/sign-release.sh" "${ARCHIVE_DIR}" "OpenUT2k4Server-${VERSION}-Mac"
echo "==> Done: ${ARCHIVE_DIR}"

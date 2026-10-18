#!/usr/bin/env bash
# Build, cook and package the OpenUT2k4 dedicated server for Linux (Server/Docs/DedicatedServer.md "Building").
#
# Usage: Server/build-linux.sh [--shipping] [--server-target] [UE_ROOT]
#   UE_ROOT defaults to $UE_ROOT, then ${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x
#   Output: Client/Dist/Server-Linux/ (no UT2004 data; checked by Scripts/check-release.sh)
#   Default: the OpenUT2k4Server target as a Game target (what an installed engine can build), Development
#            configuration (a Game target has no log in Shipping on an installed engine; a server needs its log).
#   --shipping       Shipping configuration (no log file on an installed engine).
#   --server-target  a source-built engine: OpenUT2k4Server as a real Server target (UE_SERVER, cooked for
#                    LinuxServer: no shaders or render data). Unverified here (no source engine on the build machine).
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
UE_ROOT="${1:-${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}}"
RUNUAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
BUILD_SH="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"
ARCHIVE_DIR="${PROJECT_DIR}/Dist/Server-Linux"
[[ ${CONFIG} == Shipping ]] && ARCHIVE_DIR+="-Shipping"

for f in "${RUNUAT}" "${BUILD_SH}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: ${f} not found (set UE_ROOT or pass the engine path)" >&2
        exit 1
    fi
done

if [[ ! -d "${PROJECT_DIR}/Content/Materials" ]]; then
    echo "==> Content/Materials missing: building OpenUT2k4Editor and generating content"
    "${BUILD_SH}" OpenUT2k4Editor Linux Development "-Project=${PROJECT}" -WaitMutex
    "${SCRIPT_DIR}/generate-content.sh" "${UE_ROOT}"
fi

# Start from an empty output folder so files of an older package never linger.
rm -rf -- "${ARCHIVE_DIR:?}"

if [[ ${SERVER_TARGET} -eq 1 ]]; then
    export OPENUT2K4_SERVER_TARGET_TYPE=Server
    TARGET_ARGS=(-server -noclient -serverplatform=Linux "-serverconfig=${CONFIG}" -target=OpenUT2k4Server)
else
    export OPENUT2K4_SERVER_TARGET_TYPE=Game
    TARGET_ARGS=(-platform=Linux "-clientconfig=${CONFIG}" -target=OpenUT2k4Server)
fi

echo "==> BuildCookRun OpenUT2k4Server (Linux, ${CONFIG}, $([[ ${SERVER_TARGET} -eq 1 ]] && echo 'Server target' || echo 'Game target'))"
"${RUNUAT}" BuildCookRun \
    "-project=${PROJECT}" \
    "${TARGET_ARGS[@]}" \
    "-map=/Game/Maps/MainMenu+/Game/Maps/Arena" \
    -build -cook -stage -pak -iostore -nodebuginfo \
    -archive "-archivedirectory=${ARCHIVE_DIR}" \
    "-stagingdirectory=${PROJECT_DIR}/Saved/StagedBuilds/Server" \
    -nop4 -utf8output -unattended

# UAT archives into <dir>/<Platform>Server or <dir>/Linux depending on the target type: flatten to ARCHIVE_DIR
for SUB in LinuxServer Linux; do
    if [[ -d "${ARCHIVE_DIR}/${SUB}" && ! -d "${ARCHIVE_DIR}/OpenUT2k4" ]]; then
        shopt -s dotglob
        mv "${ARCHIVE_DIR}/${SUB}"/* "${ARCHIVE_DIR}/"
        rmdir "${ARCHIVE_DIR}/${SUB}"
        shopt -u dotglob
    fi
done

# no debug symbols in the server package (-nodebuginfo normally keeps them out)
find "${ARCHIVE_DIR}" \( -name '*.debug' -o -name '*.sym' \) -delete
# headless: the Vulkan loader and its validation layers (about 260 MB) are never loaded by a -nullrhi server
rm -rf -- "${ARCHIVE_DIR:?}/Engine/Binaries/ThirdParty/Vulkan"

# the server's own files: launchers, default ini, service files
mkdir -p "${ARCHIVE_DIR}/Server/linux"
cp "${SCRIPT_DIR}/Package/openut2k4-server" "${SCRIPT_DIR}/Package/ucc" "${ARCHIVE_DIR}/"
cp "${SCRIPT_DIR}/Package/OpenUT2k4Server.ini" "${ARCHIVE_DIR}/Server/"
cp "${SCRIPT_DIR}/Package/linux/"* "${ARCHIVE_DIR}/Server/linux/"
chmod +x "${ARCHIVE_DIR}/openut2k4-server" "${ARCHIVE_DIR}/ucc" "${ARCHIVE_DIR}/Server/linux/"*.sh
cp "${SCRIPT_DIR}/../LICENSE" "${SCRIPT_DIR}/../NOTICE.md" "${ARCHIVE_DIR}/"
cat > "${ARCHIVE_DIR}/Server/README.txt" <<'TXT'
OpenUT2k4 dedicated server (Linux)

  ./openut2k4-server -ConvertOnly -UT2004Dir=<UT2004 install>       convert your UT2004 content once
  ./openut2k4-server [DM-Rankin?game=XGame.xDeathMatch] [-ServerConfig=<ini>] [-ServerProfile=<dir>]
  ./ucc server DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8 ini=<ini>      (UT2004 style)
  Server/linux/install-service.sh [--user|--system] [--convert --ut2004 <install>]   a systemd service
  Server/linux/uninstall-service.sh [--user|--system] [--purge]

Configuration: copy Server/OpenUT2k4Server.ini and pass it with -ServerConfig=. Admin commands on stdin (help), or
the TCP admin console ([OpenUT2k4.AdminConsole]). The package contains no UT2004 data: the server needs your own
UT2004 install (converted once into the content cache). Documentation: Server/Docs/DedicatedServer.md in the source tree.
TXT

"${PROJECT_DIR}/Scripts/check-release.sh" "${ARCHIVE_DIR}" "${UE_ROOT}"
# release archive + detached signature, only with OPENUT2K4_SIGNING_KEY (Server/Docs/Validator.md "Release signing")
VERSION="$(sed -n 's/^ProjectVersion=//p' "${PROJECT_DIR}/Config/DefaultGame.ini" | tr -d '\r')"
"${PROJECT_DIR}/Scripts/sign-release.sh" "${ARCHIVE_DIR}" "OpenUT2k4Server-${VERSION}-Linux"
echo "==> Done: ${ARCHIVE_DIR} ($(du -sh "${ARCHIVE_DIR}" | cut -f1))"

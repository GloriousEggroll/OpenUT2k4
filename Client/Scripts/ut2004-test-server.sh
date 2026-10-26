#!/usr/bin/env bash
# Runs a local, LAN-only UT2004 dedicated server for testing OpenUT2k4's server browser and network code.
#
# Usage: Scripts/ut2004-test-server.sh [options] <scratch dir>
#   -m, --map <Map>        map (default DM-Rankin)
#   -g, --game <Class>     game type class (default XGame.xDeathMatch; e.g. XGame.xCTFGame, XGame.xBombingRun,
#                          XGame.xTeamGame, XGame.xDoubleDom, Onslaught.ONSOnslaughtGame, UT2k4Assault.ASGameInfo)
#   -p, --port <n>         game port (default 7777; the query port is port + 1)
#   -b, --bots <n>         bots on the server (default 0): ?MinPlayers=n plus our TestBots mutator
#                          (Scripts/ut2004-test-server/TestBots.uc), which adds the bots even while no human is
#                          connected (stock servers only add MinPlayers bots for connected players). The mutator is
#                          compiled once with the Windows UCC.exe of --client-root under Wine (ucc make does not run
#                          on Linux) and cached in <scratch dir>/testbots/.
#   -c, --client-root <d>  a full UT2004 install with System/UCC.exe for compiling the mutator (read only; default
#                          $UT2004_CLIENT_ROOT, then ${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004)
#   -n, --name <text>      server name (default "OpenUT2k4 Test Server")
#   -o, --options <text>   extra URL options appended as is (e.g. "?GoalScore=5?TimeLimit=10")
#   -r, --root <dir>       the server install (read only; default $UT2004_SERVER_ROOT, then ~/ut2004-dedicated-server, an extracted copy of
#                          the 3369.3 dedicated server), a native Linux server with System/ucc-bin-linux-amd64,
#                          or a Windows install (System/UCC.exe, run under Wine) as a fallback
#   -w, --wine             force Wine (System/UCC.exe) even if a native binary exists
#   -f, --foreground       run in the foreground (default: background, prints the PID)
#
# Everything goes into the scratch dir: a copy of System/ (with our ini changes), symlinks to the server's other
# folders, the log (server.log) and the PID (server.pid). The server root is never written to.
# With --bots the match starts at once (bWaitForNetPlayers=False), so the bots show in the players query.
# The log (System/server.log) is written when the server exits; the native 3369 binary buffers it.
# The server is LAN only: bound to 127.0.0.1 (MULTIHOME), master server uplink, GameSpy uplink and stats disabled,
# no web admin. Stop it with: kill "$(cat <scratch dir>/server.pid)"  (only that PID).
set -euo pipefail

DEFAULT_ROOT="${HOME}/ut2004-dedicated-server"
ROOT="${UT2004_SERVER_ROOT:-$DEFAULT_ROOT}"
CLIENT_ROOT="${UT2004_CLIENT_ROOT:-${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAP="DM-Rankin"
GAME="XGame.xDeathMatch"
PORT=7777
BOTS=0
NAME="OpenUT2k4 Test Server"
EXTRA=""
WINE=0
FOREGROUND=0
RUN_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--map) MAP="$2"; shift 2 ;;
        -g|--game) GAME="$2"; shift 2 ;;
        -p|--port) PORT="$2"; shift 2 ;;
        -b|--bots) BOTS="$2"; shift 2 ;;
        -n|--name) NAME="$2"; shift 2 ;;
        -o|--options) EXTRA="$2"; shift 2 ;;
        -r|--root) ROOT="$2"; shift 2 ;;
        -c|--client-root) CLIENT_ROOT="$2"; shift 2 ;;
        -w|--wine) WINE=1; shift ;;
        -f|--foreground) FOREGROUND=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        -*) echo "error: unknown option $1" >&2; exit 2 ;;
        *) RUN_DIR="$1"; shift ;;
    esac
done

if [[ -z "${RUN_DIR}" ]]; then
    echo "error: give a scratch dir for this server instance (see --help)" >&2
    exit 2
fi
if [[ ! -d "${ROOT}/System" ]]; then
    echo "error: no UT2004 server at ${ROOT} (expected ${ROOT}/System)" >&2
    exit 1
fi
ROOT="$(cd "${ROOT}" && pwd)"
mkdir -p "${RUN_DIR}"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"
case "${RUN_DIR}/" in
    "${ROOT}/"*) echo "error: the scratch dir must be outside the server root" >&2; exit 1 ;;
esac

if [[ -f "${RUN_DIR}/server.pid" ]] && kill -0 "$(cat "${RUN_DIR}/server.pid")" 2>/dev/null; then
    echo "error: a server from ${RUN_DIR} is still running (PID $(cat "${RUN_DIR}/server.pid"))" >&2
    exit 1
fi

# per-run copy of System/ (inis and logs are written there); everything else is a symlink to the read-only root
rm -rf "${RUN_DIR}/System"
cp -r "${ROOT}/System" "${RUN_DIR}/System"
chmod -R u+w "${RUN_DIR}/System"
for Entry in "${ROOT}"/*; do
    Base="$(basename "${Entry}")"
    [[ "${Base}" == "System" ]] && continue
    ln -sfn "${Entry}" "${RUN_DIR}/${Base}"
done

# TestBots mutator (--bots): compiled once per scratch dir, then copied into System/
if [[ "${BOTS}" -gt 0 ]]; then
    BOTS_DIR="${RUN_DIR}/testbots"
    BOTS_U="${BOTS_DIR}/OUTestBots.u"
    BOTS_SRC="${SCRIPT_DIR}/ut2004-test-server/TestBots.uc"
    if [[ ! -f "${BOTS_U}" || "${BOTS_SRC}" -nt "${BOTS_U}" ]]; then
        if [[ ! -f "${CLIENT_ROOT}/System/UCC.exe" ]] || ! command -v wine > /dev/null; then
            echo "error: --bots needs Wine and a UT2004 install with System/UCC.exe (--client-root)" >&2
            exit 1
        fi
        echo "==> compiling the TestBots mutator (Wine, ${CLIENT_ROOT}/System/UCC.exe)"
        BUILD="${BOTS_DIR}/build"
        rm -rf "${BUILD}"
        mkdir -p "${BUILD}/OUTestBots/Classes"
        cp -r "${CLIENT_ROOT}/System" "${BUILD}/System"
        chmod -R u+w "${BUILD}/System"
        rm -f "${BUILD}/System/OUTestBots.u"
        cp "${BOTS_SRC}" "${BUILD}/OUTestBots/Classes/TestBots.uc"
        for Entry in "${CLIENT_ROOT}"/*; do
            Base="$(basename "${Entry}")"
            [[ "${Base}" == "System" ]] && continue
            ln -sfn "${Entry}" "${BUILD}/${Base}"
        done
        # the last EditPackages line of UT2004.ini gets OUTestBots after it
        python3 -I - "${BUILD}/System/UT2004.ini" <<'PY'
import sys
path = sys.argv[1]
text = open(path, 'rb').read().decode('latin-1')
nl = '\r\n' if '\r\n' in text else '\n'
lines = text.split(nl)
last = max(i for i, l in enumerate(lines) if l.startswith('EditPackages='))
lines.insert(last + 1, 'EditPackages=OUTestBots')
open(path, 'wb').write(nl.join(lines).encode('latin-1'))
PY
        (cd "${BUILD}/System" && env WINEDEBUG=-all "WINEPREFIX=${WINEPREFIX:-${BOTS_DIR}/wineprefix}" timeout 300 \
            wine UCC.exe make -ini=UT2004.ini > "${BOTS_DIR}/make.log" 2>&1) || true
        if [[ ! -f "${BUILD}/System/OUTestBots.u" ]]; then
            echo "error: compiling the TestBots mutator failed, see ${BOTS_DIR}/make.log" >&2
            exit 1
        fi
        cp "${BUILD}/System/OUTestBots.u" "${BOTS_U}"
        rm -rf "${BUILD}"
    fi
    cp "${BOTS_U}" "${RUN_DIR}/System/OUTestBots.u"
fi

INI="${RUN_DIR}/System/UT2004.ini"
if [[ ! -f "${INI}" ]]; then
    cp "${RUN_DIR}/System/Default.ini" "${INI}"
fi
# ini changes: no uplinks / stats / web admin, LAN only
# with bots, the match starts without waiting for a human (bWaitForNetPlayers) so the bots join and show in queries
WAIT_SETTINGS=()
if [[ "${BOTS}" -gt 0 ]]; then
    WAIT_SETTINGS=("UnrealGame.DeathMatch|bWaitForNetPlayers|False" "${GAME}|bWaitForNetPlayers|False")
fi
python3 -I - "${INI}" "${NAME}" "${WAIT_SETTINGS[@]}" <<'PY'
import sys, re
path, name = sys.argv[1], sys.argv[2]
raw = open(path, 'rb').read()
text = raw.decode('latin-1')
nl = '\r\n' if '\r\n' in text else '\n'
lines = text.split(nl)
out, section = [], ''
want = {
    'ipdrv.masterserveruplink': {'DoUplink': 'False', 'UplinkToGamespy': 'False', 'SendStats': 'False', 'ServerBehindNAT': 'False'},
    'engine.gamereplicationinfo': {'ServerName': name, 'ShortName': name[:20]},
    'uweb.webserver': {'bEnabled': 'False'},
}
names = {'ipdrv.masterserveruplink': 'IpDrv.MasterServerUplink', 'engine.gamereplicationinfo': 'Engine.GameReplicationInfo',
         'uweb.webserver': 'UWeb.WebServer'}
for extra in sys.argv[3:]:
    sec, key, value = extra.split('|', 2)
    want.setdefault(sec.lower(), {})[key] = value
    names[sec.lower()] = sec
seen = set()
def flush(sec):
    for k, v in want.get(sec, {}).items():
        if (sec, k) not in seen:
            out.append(f'{k}={v}')
            seen.add((sec, k))
for line in lines:
    m = re.match(r'^\[(.+)\]\s*$', line)
    if m:
        # append keys missing from the previous section before its trailing blank lines
        trail = []
        while out and out[-1] == '':
            trail.append(out.pop())
        flush(section)
        out.extend(trail)
        section = m.group(1).lower()
        out.append(line)
        continue
    key = line.split('=', 1)[0].strip()
    if section in want and key in want[section]:
        out.append(f'{key}={want[section][key]}')
        seen.add((section, key))
        continue
    if section == 'engine.gameengine' and line.strip().lower() == 'serveractors=uweb.webserver':
        continue
    out.append(line)
flush(section)
for sec, kv in want.items():
    if any((sec, k) not in seen for k in kv):
        out.append('')
        out.append(f'[{names[sec]}]')
        for k, v in kv.items():
            if (sec, k) not in seen:
                out.append(f'{k}={v}')
open(path, 'wb').write(nl.join(out).encode('latin-1'))
PY

URL="${MAP}?game=${GAME}?Port=${PORT}"
if [[ "${BOTS}" -gt 0 ]]; then
    URL="${URL}?MinPlayers=${BOTS}?Mutator=OUTestBots.TestBots"
fi
URL="${URL}${EXTRA}"
# one ?Mutator= list (UE2 reads the first one only): the options' mutators join TestBots'
URL="$(python3 -I -c '
import sys
parts = sys.argv[1].split("?")
muts = [p.split("=", 1)[1] for p in parts[1:] if p.lower().startswith("mutator=")]
rest = [p for p in parts[1:] if not p.lower().startswith("mutator=")]
print("?".join([parts[0]] + rest + (["Mutator=" + ",".join(muts)] if muts else [])))
' "${URL}")"

cd "${RUN_DIR}/System"
NATIVE="${RUN_DIR}/System/ucc-bin-linux-amd64"
if [[ "${WINE}" == "0" && -f "${NATIVE}" && "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]]; then
    chmod +x "${NATIVE}"
    # HOME points into the run dir so the server never reads or writes ~/.ut2004
    CMD=(env "HOME=${RUN_DIR}" "${NATIVE}" server "${URL}" ini=UT2004.ini log=server.log -nohomedir -forcelogflush -multihome=127.0.0.1)
elif [[ -f "${RUN_DIR}/System/UCC.exe" ]]; then
    CMD=(env "WINEDEBUG=-all" "WINEPREFIX=${WINEPREFIX:-${RUN_DIR}/wineprefix}" wine UCC.exe server "${URL}" -ini=UT2004.ini -log=server.log -forcelogflush -multihome=127.0.0.1)
else
    echo "error: no ucc-bin-linux-amd64 or UCC.exe in ${ROOT}/System" >&2
    exit 1
fi

echo "==> ${URL}"
echo "    run dir: ${RUN_DIR}"
echo "    game port ${PORT} (UDP), query port $((PORT + 1)) (UDP), 127.0.0.1 only"
if [[ "${FOREGROUND}" == "1" ]]; then
    exec "${CMD[@]}"
fi
"${CMD[@]}" > "${RUN_DIR}/server.stdout" 2>&1 < /dev/null &
PID=$!
echo "${PID}" > "${RUN_DIR}/server.pid"
echo "    PID ${PID} (stop: kill ${PID}); log: ${RUN_DIR}/System/server.log"

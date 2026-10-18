#!/usr/bin/env bash
# Installs the OpenUT2k4 dedicated server as a systemd service (Server/Docs/DedicatedServer.md "Linux (systemd)").
#
# Usage (from the server package):
#   Server/linux/install-service.sh [--user | --system] [--name <instance>] [--config <ini>] [--profile <dir>]
#       [--state-dir <dir>] [--content-cache <dir>] [--ut2004 <install>] [--run-as <user>] [--convert]
#       [--no-start] [--no-enable] [--root <package dir>] [-- <extra server switches>]
#
#   --user     a user service (systemctl --user), the default when not run as root: runs as you, no root needed.
#              To keep it running without a login session: loginctl enable-linger $USER
#   --system   a system service (root): runs as an unprivileged account (--run-as, default "openut2k4", created
#              with useradd --system when missing)
#   --name     instance name (several servers: one name each, with their own config, state and port). Default: default
#   --config   the server ini. Default: a copy of Server/OpenUT2k4Server.ini, made once in the config folder
#              (user: ~/.config/openut2k4-server/<name>.ini, system: /etc/openut2k4-server/<name>.ini)
#   --profile  a server profile folder (-ServerProfile=)
#   --state-dir  bans, the engine's Saved folder and the logs. Default: ~/.local/share/openut2k4-server/<name> (user)
#              or /var/lib/openut2k4-server/<name> (system)
#   --content-cache / --ut2004   the converted content and the UT2004 install (written into a new config copy's
#              [OpenUT2k4.Paths] and passed on the command line)
#   --convert  convert the UT2004 install into the content cache first when the cache is not ready
set -euo pipefail

MODE=""
NAME=default
CONFIG=""
PROFILE=""
STATE=""
CACHE=""
UT2004=""
RUNAS=""
CONVERT=0
START=1
ENABLE=1
ROOT=""
EXTRA=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user) MODE=user ;;
        --system) MODE=system ;;
        --name) NAME="$2"; shift ;;
        --config) CONFIG="$2"; shift ;;
        --profile) PROFILE="$2"; shift ;;
        --state-dir) STATE="$2"; shift ;;
        --content-cache) CACHE="$2"; shift ;;
        --ut2004) UT2004="$2"; shift ;;
        --run-as) RUNAS="$2"; shift ;;
        --convert) CONVERT=1 ;;
        --no-start) START=0 ;;
        --no-enable) ENABLE=0 ;;
        --root) ROOT="$2"; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

if [[ ! "${NAME}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "error: --name may only contain letters, digits, '.', '_' and '-'" >&2
    exit 2
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${ROOT:-${SCRIPT_DIR}/../..}" && pwd)"
if [[ ! -x "${ROOT}/openut2k4-server" ]]; then
    echo "error: ${ROOT} is not an OpenUT2k4 server package (no openut2k4-server); use --root" >&2
    exit 2
fi
if [[ -z "${MODE}" ]]; then
    MODE=$([[ $(id -u) -eq 0 ]] && echo system || echo user)
fi
UNIT="openut2k4-server-${NAME}.service"

if [[ ${MODE} == user ]]; then
    if [[ $(id -u) -eq 0 ]]; then
        echo "error: --user as root: use --system" >&2
        exit 2
    fi
    CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
    UNIT_DIR="${CONFIG_HOME}/systemd/user"
    CONFIG_DIR="${CONFIG_HOME}/openut2k4-server"
    STATE="${STATE:-${XDG_DATA_HOME:-$HOME/.local/share}/openut2k4-server/${NAME}}"
    SYSTEMCTL=(systemctl --user)
    RUNAS="$(id -un)"
    WANTEDBY=default.target
    USERLINE=""
    HARDENING=""
else
    if [[ $(id -u) -ne 0 ]]; then
        echo "error: --system needs root (sudo)" >&2
        exit 2
    fi
    UNIT_DIR=/etc/systemd/system
    CONFIG_DIR=/etc/openut2k4-server
    STATE="${STATE:-/var/lib/openut2k4-server/${NAME}}"
    SYSTEMCTL=(systemctl)
    RUNAS="${RUNAS:-openut2k4}"
    if ! id -u "${RUNAS}" >/dev/null 2>&1; then
        echo "==> creating the system account ${RUNAS}"
        useradd --system --create-home --home-dir "/var/lib/openut2k4-server" --shell /usr/sbin/nologin "${RUNAS}"
    fi
    WANTEDBY=multi-user.target
    USERLINE="User=${RUNAS}"$'\n'"Group=$(id -gn "${RUNAS}")"
    HARDENING="ProtectHome=read-only"$'\n'"ReadWritePaths=${STATE}"
fi

mkdir -p "${STATE}/Logs"
if [[ -z "${CONFIG}" ]]; then
    mkdir -p "${CONFIG_DIR}"
    CONFIG="${CONFIG_DIR}/${NAME}.ini"
    if [[ ! -f "${CONFIG}" ]]; then
        cp "${ROOT}/Server/OpenUT2k4Server.ini" "${CONFIG}"
        if [[ -n "${CACHE}" ]]; then sed -i "s|^ContentCache=.*|ContentCache=${CACHE}|" "${CONFIG}"; fi
        if [[ -n "${UT2004}" ]]; then sed -i "s|^UT2004Dir=.*|UT2004Dir=${UT2004}|" "${CONFIG}"; fi
        echo "==> new server config ${CONFIG} (edit it, then: ${SYSTEMCTL[*]} restart ${UNIT})"
    fi
fi
CONFIG="$(readlink -f "${CONFIG}")"
if [[ ! -f "${CONFIG}" ]]; then
    echo "error: ${CONFIG} does not exist" >&2
    exit 2
fi
if [[ ${MODE} == system ]]; then
    chown -R "${RUNAS}:" "${STATE}"
    chmod 0750 "${STATE}"
    # the config may hold the admin password: readable by root and the service account only
    chown "root:$(id -gn "${RUNAS}")" "${CONFIG}"
    chmod 0640 "${CONFIG}"
fi

EXTRA_ARGS=""
if [[ -n "${PROFILE}" ]]; then EXTRA_ARGS+=" \"-ServerProfile=$(readlink -f "${PROFILE}")\""; fi
if [[ -n "${CACHE}" ]]; then EXTRA_ARGS+=" \"-ContentCache=${CACHE}\""; fi
if [[ -n "${UT2004}" ]]; then EXTRA_ARGS+=" \"-UT2004Dir=${UT2004}\""; fi
for A in "${EXTRA[@]+"${EXTRA[@]}"}"; do
    EXTRA_ARGS+=" \"${A}\""
done

# content: the cache must be converted once before the server can run
CACHE_DIR="${CACHE}"
if [[ -z "${CACHE_DIR}" ]]; then
    CACHE_DIR=$(sed -n 's/^ContentCache=\(..*\)$/\1/p' "${CONFIG}" | tail -1)
fi
if [[ -z "${CACHE_DIR}" ]]; then
    RUNAS_HOME=$(getent passwd "${RUNAS}" | cut -d: -f6)
    if [[ ${MODE} == user ]]; then
        CACHE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/OpenUT2k4/ContentCache"
    else
        CACHE_DIR="${RUNAS_HOME}/.local/share/OpenUT2k4/ContentCache"
    fi
fi
if [[ ! -f "${CACHE_DIR}/cache.json" ]]; then
    if [[ ${CONVERT} -eq 1 ]]; then
        echo "==> converting the UT2004 install into ${CACHE_DIR} (once; a few minutes)"
        CONV=("${ROOT}/openut2k4-server" -ConvertOnly "-ContentCache=${CACHE_DIR}" "-userdir=${STATE}")
        if [[ -n "${UT2004}" ]]; then CONV+=("-UT2004Dir=${UT2004}"); fi
        if [[ ${MODE} == system ]]; then
            mkdir -p "${CACHE_DIR}"
            chown "${RUNAS}:" "${CACHE_DIR}"
            runuser -u "${RUNAS}" -- "${CONV[@]}" </dev/null
        else
            "${CONV[@]}" </dev/null
        fi
    else
        echo "warning: no content cache at ${CACHE_DIR}: run again with --convert [--ut2004 <install>], or start the game once" >&2
    fi
fi

# the unit from the template (bash substitution: values may hold any character but newlines in paths)
TEMPLATE="$(cat "${ROOT}/Server/linux/openut2k4-server.service.in")"
TEMPLATE="${TEMPLATE//@NAME@/${NAME}}"
TEMPLATE="${TEMPLATE//@ROOT@/${ROOT}}"
TEMPLATE="${TEMPLATE//@CONFIG@/${CONFIG}}"
TEMPLATE="${TEMPLATE//@STATE@/${STATE}}"
TEMPLATE="${TEMPLATE//@LOGDIR@/${STATE}/Logs}"
TEMPLATE="${TEMPLATE//@WANTEDBY@/${WANTEDBY}}"
TEMPLATE="${TEMPLATE//@USERLINE@/${USERLINE}}"
TEMPLATE="${TEMPLATE//@HARDENING@/${HARDENING}}"
TEMPLATE="${TEMPLATE//@EXTRA@/${EXTRA_ARGS}}"
mkdir -p "${UNIT_DIR}"
printf '%s\n' "${TEMPLATE}" > "${UNIT_DIR}/${UNIT}"
echo "==> installed ${UNIT_DIR}/${UNIT}"

"${SYSTEMCTL[@]}" daemon-reload
if [[ ${ENABLE} -eq 1 ]]; then "${SYSTEMCTL[@]}" enable "${UNIT}"; fi
if [[ ${START} -eq 1 ]]; then "${SYSTEMCTL[@]}" restart "${UNIT}"; fi
JOURNAL="journalctl$([[ ${MODE} == user ]] && echo ' --user') -u ${UNIT} -f"
echo "==> ${UNIT}: ${SYSTEMCTL[*]} status ${UNIT} | ${JOURNAL} | log: ${STATE}/Logs/OpenUT2k4Server.log"

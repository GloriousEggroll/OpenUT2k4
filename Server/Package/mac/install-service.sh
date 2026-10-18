#!/usr/bin/env bash
# Installs / removes the OpenUT2k4 dedicated server as a launchd job on macOS (Server/Docs/DedicatedServer.md "macOS (launchd)").
# UNVERIFIED: written without a Mac to test on.
#
# Usage (from the server package):
#   Server/mac/install-service.sh [--agent | --daemon] [--name <instance>] [--config <ini>] [--profile <dir>]
#       [--state-dir <dir>] [--content-cache <dir>] [--ut2004 <install>] [--run-as <user>] [--no-start]
#   Server/mac/install-service.sh --uninstall [--agent | --daemon] [--name <instance>] [--purge]
#
#   --agent   a LaunchAgent of the current user (default when not root): ~/Library/LaunchAgents, runs while logged in
#   --daemon  a LaunchDaemon (root): /Library/LaunchDaemons, runs at boot as --run-as (default: _openut2k4 must exist,
#             or name an existing unprivileged account)
set -euo pipefail
MODE=""
NAME=default
CONFIG=""
PROFILE=""
STATE=""
CACHE=""
UT2004=""
RUNAS=""
START=1
UNINSTALL=0
PURGE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --agent) MODE=agent ;;
        --daemon) MODE=daemon ;;
        --name) NAME="$2"; shift ;;
        --config) CONFIG="$2"; shift ;;
        --profile) PROFILE="$2"; shift ;;
        --state-dir) STATE="$2"; shift ;;
        --content-cache) CACHE="$2"; shift ;;
        --ut2004) UT2004="$2"; shift ;;
        --run-as) RUNAS="$2"; shift ;;
        --no-start) START=0 ;;
        --uninstall) UNINSTALL=1 ;;
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
if [[ ! "${NAME}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "error: bad --name" >&2
    exit 2
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
if [[ -z "${MODE}" ]]; then
    MODE=$([[ $(id -u) -eq 0 ]] && echo daemon || echo agent)
fi
LABEL="org.openut2k4.server.${NAME}"
if [[ ${MODE} == agent ]]; then
    PLIST_DIR="$HOME/Library/LaunchAgents"
    DOMAIN="gui/$(id -u)"
    CONFIG_DIR="$HOME/Library/Application Support/OpenUT2k4Server"
    STATE="${STATE:-$HOME/Library/Application Support/OpenUT2k4Server/${NAME}}"
    USERNAME=""
else
    if [[ $(id -u) -ne 0 ]]; then
        echo "error: --daemon needs root (sudo)" >&2
        exit 2
    fi
    PLIST_DIR=/Library/LaunchDaemons
    DOMAIN=system
    CONFIG_DIR=/Library/Application\ Support/OpenUT2k4Server
    STATE="${STATE:-/Library/Application Support/OpenUT2k4Server/${NAME}}"
    RUNAS="${RUNAS:-_openut2k4}"
    if ! id -u "${RUNAS}" >/dev/null 2>&1; then
        echo "error: the account ${RUNAS} does not exist; create an unprivileged account (System Settings or dscl) or use --run-as" >&2
        exit 2
    fi
    USERNAME=$'\t<key>UserName</key>\n\t<string>'"${RUNAS}"$'</string>\n'
fi
PLIST="${PLIST_DIR}/${LABEL}.plist"

if [[ ${UNINSTALL} -eq 1 ]]; then
    launchctl bootout "${DOMAIN}/${LABEL}" 2>/dev/null || true
    rm -f -- "${PLIST:?}"
    echo "==> removed ${LABEL}"
    if [[ ${PURGE} -eq 1 ]]; then
        rm -rf -- "${STATE:?}"
        rm -f -- "${CONFIG_DIR:?}/${NAME:?}.ini"
        echo "==> deleted ${STATE}"
    fi
    exit 0
fi

mkdir -p "${CONFIG_DIR}" "${STATE}/Logs"
if [[ -z "${CONFIG}" ]]; then
    CONFIG="${CONFIG_DIR}/${NAME}.ini"
    if [[ ! -f "${CONFIG}" ]]; then
        cp "${ROOT}/Server/OpenUT2k4Server.ini" "${CONFIG}"
        echo "==> new server config ${CONFIG}"
    fi
fi
if [[ ${MODE} == daemon ]]; then
    chown -R "${RUNAS}" "${STATE}"
    chmod 0640 "${CONFIG}"
    chown "root:$(id -gn "${RUNAS}")" "${CONFIG}"
fi

xml_escape() { local s="$1"; s="${s//&/&amp;}"; s="${s//</&lt;}"; s="${s//>/&gt;}"; printf '%s' "$s"; }
EXTRA=""
add_arg() { EXTRA+=$'\t\t<string>'"$(xml_escape "$1")"$'</string>\n'; }
if [[ -n "${PROFILE}" ]]; then add_arg "-ServerProfile=${PROFILE}"; fi
if [[ -n "${CACHE}" ]]; then add_arg "-ContentCache=${CACHE}"; fi
if [[ -n "${UT2004}" ]]; then add_arg "-UT2004Dir=${UT2004}"; fi

T="$(cat "${ROOT}/Server/mac/org.openut2k4.server.plist.in")"
T="${T//@LABEL@/${LABEL}}"
T="${T//@ROOT@/$(xml_escape "${ROOT}")}"
T="${T//@CONFIG@/$(xml_escape "${CONFIG}")}"
T="${T//@STATE@/$(xml_escape "${STATE}")}"
T="${T//@EXTRA@/${EXTRA}}"
T="${T//@USERNAME@/${USERNAME}}"
mkdir -p "${PLIST_DIR}"
printf '%s\n' "${T}" > "${PLIST}"
plutil -lint "${PLIST}" >/dev/null
echo "==> installed ${PLIST}"
launchctl bootout "${DOMAIN}/${LABEL}" 2>/dev/null || true
if [[ ${START} -eq 1 ]]; then
    launchctl bootstrap "${DOMAIN}" "${PLIST}"
    echo "==> started: launchctl print ${DOMAIN}/${LABEL}; stop: launchctl bootout ${DOMAIN}/${LABEL}; log: ${STATE}/Logs/OpenUT2k4Server.log"
fi

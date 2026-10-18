#!/usr/bin/env bash
# Removes an OpenUT2k4 dedicated server service installed by install-service.sh.
#
# Usage: Server/linux/uninstall-service.sh [--user | --system] [--name <instance>] [--purge]
#   --purge  also deletes the instance's default config copy and state folder (bans, logs). The content cache is kept.
#            (A --config / --state-dir given at install time is never deleted: remove those yourself.)
set -euo pipefail
MODE=""
NAME=default
PURGE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user) MODE=user ;;
        --system) MODE=system ;;
        --name) NAME="$2"; shift ;;
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
if [[ ! "${NAME}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "error: bad --name" >&2
    exit 2
fi
if [[ -z "${MODE}" ]]; then
    MODE=$([[ $(id -u) -eq 0 ]] && echo system || echo user)
fi
UNIT="openut2k4-server-${NAME}.service"
if [[ ${MODE} == user ]]; then
    SYSTEMCTL=(systemctl --user)
    UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}/openut2k4-server/${NAME}.ini"
    STATE="${XDG_DATA_HOME:-$HOME/.local/share}/openut2k4-server/${NAME}"
else
    if [[ $(id -u) -ne 0 ]]; then
        echo "error: --system needs root (sudo)" >&2
        exit 2
    fi
    SYSTEMCTL=(systemctl)
    UNIT_DIR=/etc/systemd/system
    CONFIG="/etc/openut2k4-server/${NAME}.ini"
    STATE="/var/lib/openut2k4-server/${NAME}"
fi
"${SYSTEMCTL[@]}" disable --now "${UNIT}" 2>/dev/null || true
rm -f -- "${UNIT_DIR:?}/${UNIT:?}"
"${SYSTEMCTL[@]}" daemon-reload
"${SYSTEMCTL[@]}" reset-failed "${UNIT}" 2>/dev/null || true
echo "==> removed ${UNIT}"
if [[ ${PURGE} -eq 1 ]]; then
    rm -rf -- "${STATE:?}"
    rm -f -- "${CONFIG:?}"
    echo "==> deleted ${STATE} and ${CONFIG}"
else
    for KEPT in "${CONFIG}" "${STATE}"; do
        if [[ -e "${KEPT}" ]]; then echo "    kept ${KEPT} (--purge deletes it)"; fi
    done
fi

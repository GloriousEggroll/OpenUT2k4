#!/usr/bin/env bash
# Enable Nanite on the imported UT2004 static meshes (Tools/EnableNanite.py), in chunks.
# Resumable: already converted meshes are skipped.
#
# Usage: Scripts/enable-nanite.sh [CHUNKS]     (default 8)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UE_ROOT="${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}"
EDITOR="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd"
LOG_DIR="${PROJECT_DIR}/Saved/EnableNanite"
CHUNKS="${1:-8}"
mkdir -p "${LOG_DIR}"

for ((i = 0; i < CHUNKS; i++)); do
    echo "==> [$(date +%H:%M:%S)] chunk ${i}/${CHUNKS}"
    OPENUT2K4_NANITE_CHUNK="${i}/${CHUNKS}" "${EDITOR}" "${PROJECT_DIR}/OpenUT2k4.uproject" -run=pythonscript \
        "-script=${PROJECT_DIR}/Tools/EnableNanite.py" \
        -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput > "${LOG_DIR}/chunk_${i}.log" 2>&1
    grep -E "\[EnableNanite\] finished" "${LOG_DIR}/chunk_${i}.log" || echo "   chunk ${i} did not finish (see ${LOG_DIR}/chunk_${i}.log)"
done

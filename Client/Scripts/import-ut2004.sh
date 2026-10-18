#!/usr/bin/env bash
# Import UT2004 content from your own local install as placeholder content.
#
# Usage: Scripts/import-ut2004.sh [--force] <Map> [<Map> ...]
#        Scripts/import-ut2004.sh [--force] --all-exported
#        Scripts/import-ut2004.sh [--force] --everything [--chunks N] [--map-batch K] [--steps LIST]
#        Scripts/import-ut2004.sh --fix-materials
#
#   1. Runs Tools/ut2k4_export.py for every named map that is not in ImportCache/ yet
#      (game folder: $UT2004_DIR, default ${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004).
#   2. Runs Tools/ImportUT2004.py in the editor: textures, materials, static meshes, BSP and
#      /Game/Imported/Maps/<Map>, plus Content/ImportedData/<Map>.json for the game.
#   --everything exports and imports the whole install: every map in Maps/ and every texture, static
#   mesh, sound, skeletal mesh, animation and music package (not only what maps reference). It runs
#   in resumable steps (export, skeletal+music, N asset chunks, map batches of K), skips up-to-date
#   work, logs to Saved/ImportUT2004/ and keeps going when a step fails; run it again to resume.
#   --steps limits it to a comma list of: export,materials,skeletal,assets,maps,report.
#   --fix-materials re-resolves the cached materials (Tools/ut2k4_materials.py: texture stats, baked
#   team skins, audit) and rewrites the existing material instances in place (Base, parent, parameters);
#   meshes and levels pick the changes up without being imported again.
#
# Environment: UE_ROOT (engine root), UT2004_DIR (game folder), OPENUT2K4_BUILD_EDITOR=1 (build OpenUT2k4Editor first).
# All output (ImportCache/, Content/Imported/, Content/ImportedData/) is local only and git-ignored.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT="${PROJECT_DIR}/OpenUT2k4.uproject"
UE_ROOT="${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}"
UT2004_DIR="${UT2004_DIR:-${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004}"
EDITOR="${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd"
LOG_DIR="${PROJECT_DIR}/Saved/ImportUT2004"

if [[ $# -eq 0 ]]; then
    sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
fi

ARGS=()
MAPS=()
EVERYTHING=0
FORCE=0
CHUNKS=16
MAP_BATCH=6
STEPS="export,materials,skeletal,assets,maps,report"
FIX_MATERIALS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --everything) EVERYTHING=1 ;;
        --force) FORCE=1; ARGS+=("$1") ;;
        --chunks) CHUNKS="$2"; shift ;;
        --map-batch) MAP_BATCH="$2"; shift ;;
        --steps) STEPS="$2"; shift ;;
        --fix-materials) FIX_MATERIALS=1; ARGS+=("$1") ;;
        --*) ARGS+=("$1") ;;
        *) MAPS+=("$1"); ARGS+=("$1") ;;
    esac
    shift
done

if [[ ! -x "${EDITOR}" ]]; then
    echo "error: UnrealEditor-Cmd not found at ${EDITOR} (set UE_ROOT)" >&2
    exit 1
fi
mkdir -p "${LOG_DIR}"

if [[ "${OPENUT2K4_BUILD_EDITOR:-0}" == "1" ]]; then
    "${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh" OpenUT2k4Editor Linux Development \
        "-Project=${PROJECT}" -WaitMutex
fi

# Runs Tools/ImportUT2004.py in the editor with the given arguments; log name first.
# Retries (after a minute) when the editor never got to the script, e.g. while the game
# module is being rebuilt by someone else.
run_editor() {
    local name="$1"; shift
    local log="${LOG_DIR}/${name}.log"
    local attempt rc
    for attempt in 1 2 3 4 5; do
        echo "==> [$(date +%H:%M:%S)] editor: ${name} (attempt ${attempt})"
        # arguments go through the environment: -script= splits on spaces
        OPENUT2K4_IMPORT_ARGS="$*" "${EDITOR}" "${PROJECT}" -run=pythonscript \
            "-script=${PROJECT_DIR}/Tools/ImportUT2004.py" \
            -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput > "${log}" 2>&1
        rc=$?
        if grep -q "Running Python script" "${log}"; then
            grep -E "\[ImportUT2004\] (finished|all-assets|skeletal done|chunk)" "${log}" | sed 's/.*\[ImportUT2004\]/   /' | tail -4
            [[ ${rc} -ne 0 ]] && echo "   step ${name} exited with ${rc} (see ${log}); continuing"
            return ${rc}
        fi
        echo "   editor did not start the script (rc ${rc}); retrying in 60s (see ${log})"
        sleep 60
    done
    echo "   giving up on ${name}"
    return 1
}

has_step() { [[ ",${STEPS}," == *",$1,"* ]]; }

# texture statistics, baked Combiner textures and the material audit / fix list (Tools/ut2k4/matresolve.py)
material_pass() {
    echo "==> [$(date +%H:%M:%S)] material pass (texture stats, bakes, audit)"
    python3 -I "${PROJECT_DIR}/Tools/ut2k4_materials.py" > "${LOG_DIR}/materials.log" 2>&1 \
        || echo "   material pass failed (see ${LOG_DIR}/materials.log)"
    tail -1 "${LOG_DIR}/materials.log"
}

if [[ ${FIX_MATERIALS} -eq 1 ]]; then
    material_pass
    run_editor fix_materials --fix-materials
    exit $?
fi

if [[ ${EVERYTHING} -eq 1 ]]; then
    T0=$(date +%s)
    FORCE_ARG=()
    [[ ${FORCE} -eq 1 ]] && FORCE_ARG=(--force)
    if has_step export; then
        echo "==> [$(date +%H:%M:%S)] exporting the whole install from ${UT2004_DIR}"
        python3 "${PROJECT_DIR}/Tools/ut2k4_export.py" --game "${UT2004_DIR}" \
            --out "${PROJECT_DIR}/ImportCache" --everything "${FORCE_ARG[@]}" \
            > "${LOG_DIR}/export.log" 2>&1 || echo "   export finished with errors (see ${LOG_DIR}/export.log)"
        tail -2 "${LOG_DIR}/export.log"
    fi
    if has_step materials; then
        material_pass
    fi
    if has_step skeletal; then
        run_editor skeletal --all-assets --kinds music,skeletal "${FORCE_ARG[@]}"
    fi
    if has_step assets; then
        for ((i = 0; i < CHUNKS; i++)); do
            run_editor "assets_${i}_of_${CHUNKS}" --all-assets --kinds textures,materials,staticmeshes,sounds \
                --chunk "${i}/${CHUNKS}" "${FORCE_ARG[@]}"
        done
    fi
    if has_step maps; then
        mapfile -t ALL_MAPS < <(ls "${PROJECT_DIR}/ImportCache/maps" 2>/dev/null)
        for ((i = 0; i < ${#ALL_MAPS[@]}; i += MAP_BATCH)); do
            batch=("${ALL_MAPS[@]:i:MAP_BATCH}")
            run_editor "maps_$(printf %03d $i)" --skip-built "${FORCE_ARG[@]}" "${batch[@]}"
        done
    fi
    if has_step report; then
        python3 "${PROJECT_DIR}/Tools/ImportUT2004.py" --report | tee "${LOG_DIR}/report.txt"
    fi
    echo "==> --everything finished in $(( ($(date +%s) - T0) / 60 )) min; logs in ${LOG_DIR}"
    exit 0
fi

# 1. export maps that are missing from the cache
MISSING=()
for m in "${MAPS[@]+"${MAPS[@]}"}"; do
    [[ -f "${PROJECT_DIR}/ImportCache/maps/${m}/map.json" ]] || MISSING+=("${m}")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "==> Exporting ${MISSING[*]} from ${UT2004_DIR}"
    python3 "${PROJECT_DIR}/Tools/ut2k4_export.py" --game "${UT2004_DIR}" \
        --out "${PROJECT_DIR}/ImportCache" --map "${MISSING[@]}" || exit 1
    material_pass
fi

# 2. editor import (arguments go through the environment: -script= splits on spaces)
echo "==> Importing ${ARGS[*]} into ${PROJECT_DIR}/Content/Imported"
export OPENUT2K4_IMPORT_ARGS="${ARGS[*]}"
"${EDITOR}" "${PROJECT}" -run=pythonscript "-script=${PROJECT_DIR}/Tools/ImportUT2004.py" \
    -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput || exit 1
echo "==> Import finished. Maps: /Game/Imported/Maps/<Map> (listed in the main menu layouts)"

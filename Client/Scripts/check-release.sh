#!/usr/bin/env bash
# Checks that a packaged build contains no UT2004 data, so it is safe to distribute.
#
# Usage: Scripts/check-release.sh <package dir> [UE_ROOT]
# Fails when the package contains Content/ImportedData, loose UT2004-derived files, or any cooked
# package under /Game/Imported (paks and IoStore containers are listed with UnrealPak).
set -uo pipefail

DIR="${1:?usage: check-release.sh <package dir> [UE_ROOT]}"
UE_ROOT="${2:-${UE_ROOT:-${HOME}/UnrealEngine/Linux_Unreal_Engine_5.x}}"
UNREALPAK="${UE_ROOT}/Engine/Binaries/$([[ $(uname) == Darwin ]] && echo Mac || echo Linux)/UnrealPak"
FAIL=0

echo "==> Checking ${DIR} for UT2004 data"
if [[ ! -d "${DIR}" ]]; then
    echo "error: ${DIR} does not exist" >&2
    exit 2
fi

# Loose files: the game-side json and anything that looks like converted UT2004 content.
LOOSE=$(find "${DIR}" \( -path '*/ImportedData*' -o -path '*/ImportCache*' -o -path '*/ContentCache*' \
    -o -iname '*.ut2' -o -iname '*.utx' -o -iname '*.usx' -o -iname '*.uax' -o -iname '*.ukx' -o -iname '*.umx' \
    -o -iname '*.ogg' -o -iname '*.gltf' \) -print)
if [[ -n "${LOOSE}" ]]; then
    echo "FAIL: loose UT2004 content:"
    echo "${LOOSE}" | head -20 | sed 's/^/    /'
    FAIL=1
fi

# Containers: list every file name and look for the import folders.
while IFS= read -r -d '' C; do
    if [[ -x "${UNREALPAK}" ]]; then
        LIST=$("${UNREALPAK}" "${C}" -List 2>/dev/null)
    else
        LIST=$(strings -n 8 "${C}")
    fi
    HITS=$(grep -aE '(Game|Content)/(Imported|ImportedData)/' <<<"${LIST}" | head -10)
    COUNT=$(grep -acE '\.(uasset|umap|uexp|ubulk|json)' <<<"${LIST}")
    echo "    $(basename "${C}"): ${COUNT} files listed"
    if [[ -n "${HITS}" ]]; then
        echo "FAIL: ${C} contains imported content:"
        sed 's/^/    /' <<<"${HITS}"
        FAIL=1
    fi
done < <(find "${DIR}" \( -name '*.pak' -o -name '*.utoc' \) -print0)

# Raw byte scan as a second net (container directory indexes keep package names readable).
if grep -rlaq --include='*.utoc' --include='*.pak' -e '/Game/Imported/' -e 'Content/Imported/' "${DIR}" 2>/dev/null; then
    echo "FAIL: a container mentions /Game/Imported:"
    grep -rla --include='*.utoc' --include='*.pak' -e '/Game/Imported/' -e 'Content/Imported/' "${DIR}" | sed 's/^/    /'
    FAIL=1
fi

if [[ ${FAIL} -ne 0 ]]; then
    echo "==> NOT safe to distribute"
    exit 1
fi
echo "==> OK: no UT2004 data found"

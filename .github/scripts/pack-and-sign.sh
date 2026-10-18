#!/usr/bin/env bash
# CI: packs a Dist folder into <out dir>/<name>.tar.gz (or .zip with --zip) and, when the release key is available
# (RELEASE_KEY_FILE), writes <archive>.minisig and <archive>.sha256 with Client/Tools/openut2k4_signing.py.
# Without the key it still writes the .sha256.
#
# Usage: .github/scripts/pack-and-sign.sh [--zip] <dist dir> <out dir> <name>
set -euo pipefail

ZIP=0
if [[ "${1:-}" == "--zip" ]]; then ZIP=1; shift; fi
DIR="$(cd "${1:?dist dir}" && pwd)"
OUT="${2:?out dir}"
NAME="${3:?archive name}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

if [[ ${ZIP} -eq 1 ]]; then
    ARCHIVE="${OUT}/${NAME}.zip"
    rm -f "${ARCHIVE}"
    # the archive holds one top-level folder named like the archive
    STAGE="$(mktemp -d)"
    cp -R "${DIR}" "${STAGE}/${NAME}"
    (cd "${STAGE}" && 7z a -tzip -mx=7 "${ARCHIVE}" "${NAME}" > /dev/null)
    rm -rf "${STAGE}"
else
    ARCHIVE="${OUT}/${NAME}.tar.gz"
    BASE="$(basename "${DIR}")"
    if tar --version 2>/dev/null | grep -q GNU; then
        tar -C "$(dirname "${DIR}")" --transform "s|^${BASE}|${NAME}|" -czf "${ARCHIVE}" "${BASE}"
    else
        tar -C "$(dirname "${DIR}")" -s "|^${BASE}|${NAME}|" -czf "${ARCHIVE}" "${BASE}"    # bsdtar (macOS)
    fi
fi

if [[ -n "${RELEASE_KEY_FILE:-}" && -f "${RELEASE_KEY_FILE}" ]]; then
    # writes .minisig and .sha256; the key never reaches the log
    python3 -I "${ROOT}/Client/Tools/openut2k4_signing.py" sign --key "${RELEASE_KEY_FILE}" "${ARCHIVE}" > /dev/null
    echo "==> Packed and signed: ${ARCHIVE}"
else
    (cd "${OUT}" && { sha256sum "$(basename "${ARCHIVE}")" 2>/dev/null || shasum -a 256 "$(basename "${ARCHIVE}")"; } > "$(basename "${ARCHIVE}").sha256")
    echo "==> Packed (unsigned): ${ARCHIVE}"
fi

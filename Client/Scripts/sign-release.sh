#!/usr/bin/env bash
# Packs a Dist folder into a release archive and signs it, when OPENUT2K4_SIGNING_KEY is set (the path of the project
# owner's release key, made with: python3 Client/Tools/openut2k4_signing.py genkey; it lives outside the repository).
# Without the variable nothing happens. Output next to the folder: <name>.tar.gz, .tar.gz.minisig, .tar.gz.sha256.
# Players check a download with: minisign -Vm <archive> -P <published public key> (Server/Docs/Validator.md).
#
# Usage: Scripts/sign-release.sh <dist dir> <archive base name>
set -euo pipefail
DIR="${1:?usage: sign-release.sh <dist dir> <archive base name>}"
NAME="${2:?usage: sign-release.sh <dist dir> <archive base name>}"
if [[ -z "${OPENUT2K4_SIGNING_KEY:-}" ]]; then
    echo "==> OPENUT2K4_SIGNING_KEY not set: no release archive / signature"
    exit 0
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f "${OPENUT2K4_SIGNING_KEY}" ]]; then
    echo "error: OPENUT2K4_SIGNING_KEY: no file there" >&2
    exit 1
fi
PARENT="$(cd "$(dirname "${DIR}")" && pwd)"
ARCHIVE="${PARENT}/${NAME}.tar.gz"
echo "==> Packing ${ARCHIVE}"
tar -C "${PARENT}" -czf "${ARCHIVE}" "$(basename "${DIR}")"
# the key's path and contents never reach the log
python3 -I "${SCRIPT_DIR}/../Tools/openut2k4_signing.py" sign --key "${OPENUT2K4_SIGNING_KEY}" "${ARCHIVE}" > /dev/null
echo "==> Signed: ${ARCHIVE}.minisig, ${ARCHIVE}.sha256"

#!/usr/bin/env bash
# Copies the OpenUT2k4 Validator into a server package (Server/Docs/Validator.md): the compiled mutator for stock
# UT2004 servers (OpenUT2k4Validator.u / .int from Server/Validator/build-validator.sh), its default ini, the
# UnrealScript source, the signing / ban list tool and the documentation. Called by Server/build-linux.sh and
# Server/build-mac.sh; Server/build-windows.bat does the same copies.
#
# Usage: Server/package-validator.sh <package dir>
#   Without a compiled mutator (Server/Validator/Build/OpenUT2k4Validator.u) the package gets everything else and a
#   warning; OPENUT2K4_REQUIRE_VALIDATOR=1 (release builds) makes that an error.
set -euo pipefail

PKG="${1:?usage: package-validator.sh <package dir>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/Validator"
DST="${PKG}/Validator"

rm -rf -- "${DST:?}"
mkdir -p "${DST}/System" "${DST}/Classes" "${DST}/Tools"
if [[ -f "${SRC}/Build/OpenUT2k4Validator.u" ]]; then
    cp "${SRC}/Build/OpenUT2k4Validator.u" "${DST}/System/"
    cp "${SRC}/OpenUT2k4Validator.int" "${DST}/System/"
elif [[ "${OPENUT2K4_REQUIRE_VALIDATOR:-0}" == "1" ]]; then
    echo "error: no compiled validator (run Server/Validator/build-validator.sh first)" >&2
    exit 1
else
    echo "warning: no compiled validator in ${SRC}/Build: the package gets its source and tools only" >&2
fi
cp "${SRC}/OpenUT2k4Validator.ini" "${DST}/System/"
cp "${SRC}/Classes/"*.uc "${DST}/Classes/"
cp "${SCRIPT_DIR}/../Client/Tools/openut2k4_signing.py" "${DST}/Tools/"
cp "${SCRIPT_DIR}/Docs/Validator.md" "${DST}/"
cat > "${DST}/README.txt" <<'TXT'
OpenUT2k4 Validator (optional, for server admins)

Stock UT2004 servers (3369 / OldUnreal 3374):
  copy System/OpenUT2k4Validator.u and .int into the server's System folder, merge System/OpenUT2k4Validator.ini
  into the server ini, and add under [Engine.GameEngine]:  ServerActors=OpenUT2k4Validator.OUTValidator
OpenUT2k4 dedicated server (this package): the validator is built in; enable it with the same ServerActors line in
  your server ini (it is off by default).
Shared ban lists: Tools/openut2k4_signing.py (genkey --name banlist, banlist-export / -sign / -verify / -import).
Classes/ holds the mutator's UnrealScript source. Full documentation: Validator.md.
TXT
echo "==> Validator: ${DST}"

#!/usr/bin/env bash
# Compiles the OpenUT2k4 Validator mutator (OpenUT2k4Validator.u) with UT2004's own compiler (UCC make).
#
# Usage: Server/Validator/build-validator.sh [--ut2004 <dir>] [--out <dir>]
#   --ut2004 <dir>  a UT2004 install or dedicated server with System/UCC.exe (read only; default $UT2004_DIR, then
#                   ${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004). The Windows UCC.exe runs under Wine on Linux
#                   and macOS (ucc make of the 3369 Linux binaries does not work); on Windows run it in Git Bash / MSYS.
#   --out <dir>     where OpenUT2k4Validator.u and .int go (default Server/Validator/Build, git-ignored)
# The compiled .u is a build artifact: never commit it. Compiled against 3369 packages it runs on stock 3369 servers
# and on OldUnreal 3374 servers. Install: Server/Docs/Validator.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UT="${UT2004_DIR:-${HOME}/UnrealEngine/UT2004/Unreal Tournament 2004}"
OUT="${SCRIPT_DIR}/Build"
PKG=OpenUT2k4Validator
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ut2004) UT="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "error: unknown option $1" >&2; exit 2 ;;
    esac
done
if [[ ! -f "${UT}/System/UCC.exe" ]]; then
    echo "error: no ${UT}/System/UCC.exe (give a UT2004 install with --ut2004)" >&2
    exit 1
fi
RUNNER=()
if [[ "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* && "$(uname -s)" != CYGWIN* ]]; then
    command -v wine > /dev/null || { echo "error: Wine is needed to run UCC.exe" >&2; exit 1; }
    RUNNER=(env WINEDEBUG=-all "WINEPREFIX=${WINEPREFIX:-${OUT}/wineprefix}" wine)
fi
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
BUILD="${OUT}/build"
rm -rf "${BUILD}"
mkdir -p "${BUILD}/${PKG}/Classes"
# a private copy of System/ (UCC writes its ini and log there); everything else is linked read only
cp -r "${UT}/System" "${BUILD}/System"
chmod -R u+w "${BUILD}/System"
rm -f "${BUILD}/System/${PKG}.u"
for Entry in "${UT}"/*; do
    Base="$(basename "${Entry}")"
    [[ "${Base}" == "System" ]] && continue
    ln -sfn "${Entry}" "${BUILD}/${Base}"
done
cp "${SCRIPT_DIR}/Classes/"*.uc "${BUILD}/${PKG}/Classes/"
INI="${BUILD}/System/UT2004.ini"
[[ -f "${INI}" ]] || cp "${BUILD}/System/Default.ini" "${INI}"
python3 -I - "${INI}" "${PKG}" <<'PY'
import sys
path, pkg = sys.argv[1], sys.argv[2]
text = open(path, 'rb').read().decode('latin-1')
nl = '\r\n' if '\r\n' in text else '\n'
lines = [l for l in text.split(nl) if l.strip() != 'EditPackages=' + pkg]
last = max(i for i, l in enumerate(lines) if l.startswith('EditPackages='))
lines.insert(last + 1, 'EditPackages=' + pkg)
open(path, 'wb').write(nl.join(lines).encode('latin-1'))
PY
echo "==> UCC make (${UT}/System/UCC.exe)"
set +e
(cd "${BUILD}/System" && timeout 600 "${RUNNER[@]}" ./UCC.exe make -ini=UT2004.ini > "${OUT}/make.log" 2>&1)
set -e
if [[ ! -f "${BUILD}/System/${PKG}.u" ]] || grep -q -E 'Error|Failure' "${OUT}/make.log"; then
    grep -E -i 'error|warning' "${OUT}/make.log" | head -40 >&2 || true
    echo "error: compiling ${PKG} failed, see ${OUT}/make.log" >&2
    exit 1
fi
grep -i 'warning' "${OUT}/make.log" | head -20 || true
cp "${BUILD}/System/${PKG}.u" "${OUT}/${PKG}.u"
cp "${SCRIPT_DIR}/${PKG}.int" "${OUT}/${PKG}.int"
cp "${SCRIPT_DIR}/${PKG}.ini" "${OUT}/${PKG}.ini.example"
rm -rf "${BUILD}"
echo "==> ${OUT}/${PKG}.u ($(wc -c < "${OUT}/${PKG}.u") bytes), ${PKG}.int, ${PKG}.ini.example"

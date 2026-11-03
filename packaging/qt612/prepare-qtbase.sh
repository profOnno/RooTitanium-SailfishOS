#!/bin/bash
# Apply the chum qt6-qtbase patches to the Qt 6.12.0 source tree.
#
# The spec applies six patches plus the sb2 file-copy workaround; all of them
# still apply on 6.12.0. Without `dont-use-statxmntid.patch` the build fails on
# Sailfish's older kernel headers (struct statx has no stx_mnt_id).
#
# Idempotent: a marker in the source tree prevents double application.
set -euo pipefail

R=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${SRC:-$R/scratch/qt612-src/qtbase-everywhere-src-6.12.0}
P=$R/packaging/qt612/patches-qtbase
MARKER=$SRC/.rt-patched

die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$SRC" ] || die "qtbase source not found: $SRC"
[ -d "$P" ] || die "patch directory not found: $P"

if [ -f "$MARKER" ]; then
    echo "= qtbase already patched ($MARKER)"
    exit 0
fi

# Order from the chum spec: workaround first, then Patch1, Patch2, Patch10,
# Patch50, Patch54, Patch103.
for file in \
    "$P/0001-workaround-sb2-filecopy-bug.patch" \
    "$P/qtbase-CMake-Install-objects-files-into-ARCHDATADIR.patch" \
    "$P/qtbase-use-only-major-minor-for-private-api-tag.patch" \
    "$P/0010-disable-arm32-pixman-simd.patch" \
    "$P/qtbase-version-check.patch" \
    "$P/qtbase-cxxflag.patch" \
    "$P/dont-use-statxmntid.patch"
do
    echo "→ $(basename "$file")"
    patch -d "$SRC" -p1 --forward --batch --no-backup-if-mismatch < "$file" \
        || die "patch failed: $(basename "$file")"
done

touch "$MARKER"
echo "qtbase patches applied"

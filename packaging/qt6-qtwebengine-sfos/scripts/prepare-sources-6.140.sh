#!/bin/bash
# Prepare the QtWebEngine 6.140 source tree for the RooTitanium build:
#
#   1. SFOS patches from sailfishos-chum/qt6-qtwebengine (branch qt6-lts-6.11.1,
#      pinned commit; see patches-sfos/README.md);
#   2. RooTitanium's own patches-6.140/;
#   3. the droidmedia decoder insertion (apply-droidmedia.sh);
#   4. the droidmedia camera insertion (apply-droidcamera.sh).
#
# Usage:
#   scripts/prepare-sources-6.140.sh            # apply
#   scripts/prepare-sources-6.140.sh --check    # dry-run only
#   scripts/prepare-sources-6.140.sh --reset    # git checkout the tree first
#
# SRC overrides the source directory (default scratch/qtwebengine-6.140).
# Idempotent: patches already applied are skipped.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
SRC=${SRC:-$REPO/scratch/qtwebengine-6.140}
SFOS=$REPO/packaging/qt6-qtwebengine-sfos/patches-sfos
OURS=$REPO/packaging/qt6-qtwebengine-sfos/patches-6.140
CHROMIUM=$SRC/src/3rdparty/chromium
MODE=${1:-}

die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$SRC" ] || die "source tree not found: $SRC"
[ -d "$CHROMIUM" ] || die "Chromium submodule not initialised: $CHROMIUM"

if [ "$MODE" = "--reset" ]; then
    echo "→ resetting the source tree"
    git -C "$SRC" checkout -- .
    git -C "$CHROMIUM" checkout -- .
fi

# ---------------------------------------------------------------------------
apply() {  # apply <dir> <patch> [extra patch args...]
    local dir=$1 file=$2
    shift 2
    local name
    name=$(basename "$file")
    if [ "$MODE" = "--check" ]; then
        if patch -d "$dir" -p1 --dry-run --batch "$@" < "$file" >/dev/null 2>&1; then
            echo "✓ applies: $name"
        elif patch -d "$dir" -p1 --dry-run --reverse --batch "$@" < "$file" >/dev/null 2>&1; then
            echo "= already applied: $name"
        else
            echo "✗ DOES NOT APPLY: $name"
            return 1
        fi
        return 0
    fi
    if patch -d "$dir" -p1 --dry-run --reverse --batch "$@" < "$file" >/dev/null 2>&1; then
        echo "= already applied: $name"
        return 0
    fi
    echo "→ $name"
    patch -d "$dir" -p1 --forward --batch --no-backup-if-mismatch "$@" < "$file"
}

# SFOS / Rinigus patches, in the order used by the chum spec. The GL-includes
# patch is deliberately not applied (not needed on 6.140); the openh264 and
# system-ffmpeg patches are not used because the build bundles ffmpeg.
apply "$SRC"      "$SFOS/qtwebengine-SIOCGSTAMP.patch"
apply "$SRC"      "$SFOS/qtwebengine-link-pipewire.patch"
apply "$SRC"      "$SFOS/qtwebengine-aarch64-new-stat.patch"
apply "$SRC"      "$SFOS/qtwebengine-fix-build.patch"
# This one operates on the Chromium subtree.
apply "$CHROMIUM" "$SFOS/qtwebengine-aarch64-hwcap2-bti.patch"
apply "$SRC"      "$SFOS/qtwebengine-sfos-egl-validating-command-decoder.patch"
apply "$SRC"      "$SFOS/qtwebengine-sfos-shared-gl-texture-compositor.patch"

# RooTitanium patches (0300..0311; the missing numbers are patches retired on
# 6.140, see patches-6.140/README.md).
for p in "$OURS"/03[0-9][0-9]-*.patch; do
    [ -e "$p" ] || continue
    apply "$SRC" "$p"
done

if [ "$MODE" = "--check" ]; then
    echo
    echo "dry-run only; re-run without --check to apply"
    exit 0
fi

# droidmedia decoder and camera insertions (scripts are idempotent).
VER=6.140.0 BT="$SRC" "$REPO/packaging/qt6-qtwebengine-sfos/scripts/apply-droidmedia.sh"
VER=6.140.0 BT="$SRC" "$REPO/packaging/qt6-qtwebengine-sfos/scripts/apply-droidcamera.sh"

echo
echo "Sources ready. Changed files:"
git -C "$SRC" status --short | head -40
echo
echo "Next: ARCH=aarch64 scripts/configure-6.140.sh"

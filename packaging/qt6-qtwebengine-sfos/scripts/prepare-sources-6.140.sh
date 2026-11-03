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
# A marker ($SRC/.rt-prepared) makes re-runs cheap; if a run is interrupted,
# use --reset before starting over.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
SRC=${SRC:-$REPO/scratch/qtwebengine-6.140}
SFOS=$REPO/packaging/qt6-qtwebengine-sfos/patches-sfos
OURS=$REPO/packaging/qt6-qtwebengine-sfos/patches-6.140
CHROMIUM=$SRC/src/3rdparty/chromium
MARKER=$SRC/.rt-prepared
MODE=${1:-}

die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$SRC" ] || die "source tree not found: $SRC"
[ -d "$CHROMIUM" ] || die "Chromium submodule not initialised: $CHROMIUM"

# Ordered list: "directory|patch". The hwcap2 patch lives inside the Chromium
# submodule; the others apply at the QtWebEngine root.
PATCHES=(
    "$SRC|$SFOS/qtwebengine-SIOCGSTAMP.patch"
    "$SRC|$SFOS/qtwebengine-link-pipewire.patch"
    "$SRC|$SFOS/qtwebengine-aarch64-new-stat.patch"
    "$SRC|$SFOS/qtwebengine-fix-build.patch"
    "$CHROMIUM|$SFOS/qtwebengine-aarch64-hwcap2-bti.patch"
    "$SRC|$SFOS/qtwebengine-sfos-egl-validating-command-decoder.patch"
    "$SRC|$SFOS/qtwebengine-sfos-shared-gl-texture-compositor.patch"
)
# RooTitanium patches. 0307 is retired on 6.140.0: upstream already creates the
# texture from the EGLImage (see patches-6.140/README.md).
for p in "$OURS"/03[0-9][0-9]-*.patch; do
    [ -e "$p" ] || continue
    case "$(basename "$p")" in
        0307-*) continue ;;
    esac
    PATCHES+=("$SRC|$p")
done

if [ "$MODE" = "--reset" ]; then
    echo "→ resetting the source tree"
    git -C "$SRC" checkout -- .
    git -C "$CHROMIUM" checkout -- .
    rm -f "$MARKER"
fi

if [ "$MODE" = "--check" ]; then
    rc=0
    for entry in "${PATCHES[@]}"; do
        dir=${entry%%|*}
        file=${entry#*|}
        name=$(basename "$file")
        if patch -d "$dir" -p1 --dry-run --batch < "$file" >/dev/null 2>&1; then
            echo "✓ applies: $name"
        else
            echo "✗ does not apply (yet): $name"
            rc=1
        fi
    done
    echo
    echo "note: 0310/0311 depend on 0309, so a per-patch dry-run on an unpatched"
    echo "      tree reports them as failing; they apply fine in order."
    exit $rc
fi

if [ -f "$MARKER" ]; then
    echo "= patches already applied (marker: $MARKER)"
    echo "  use --reset to start from a clean tree"
else
    for entry in "${PATCHES[@]}"; do
        dir=${entry%%|*}
        file=${entry#*|}
        echo "→ $(basename "$file")"
        patch -d "$dir" -p1 --forward --batch --no-backup-if-mismatch < "$file" \
            || die "patch failed: $(basename "$file") (run --reset to retry)"
    done
    printf '%s\n' "${PATCHES[@]}" > "$MARKER"
fi

# droidmedia decoder and camera insertions (scripts are idempotent).
VER=6.140.0 BT="$SRC" "$REPO/packaging/qt6-qtwebengine-sfos/scripts/apply-droidmedia.sh"
VER=6.140.0 BT="$SRC" "$REPO/packaging/qt6-qtwebengine-sfos/scripts/apply-droidcamera.sh"

echo
echo "Sources ready. Changed files:"
git -C "$SRC" status --short | head -40
echo
echo "Next: ARCH=aarch64 scripts/configure-6.140.sh"

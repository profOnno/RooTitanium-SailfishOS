#!/bin/bash
# Compila il plugin input-context di Maliit per lo stack Qt 6.12 del target
# clonato (task 2.5 #6). Il binario di chum e' compilato per Qt 6.8: usa API
# private di Qt (QPlatformInputContext) e su 6.12 non si carica, quindi va
# ricompilato.
#
#   ARCH=aarch64 packaging/qt612/build-maliit-qt612.sh
#
# Sorgente: repo chum qt-sfos-maliit-platforminputcontext commit 9bb86e6, il cui
# sottomodulo upstream = sailfishos-open/maliit-framework f5aa5d6 (ramo qt6).
# La patch maliit-qt6.10-guiprivate.patch serve da Qt >= 6.10 per Qt6::GuiPrivate.
# Output: scratch/qt612/build-$ARCH/maliit/libmaliitplatforminputcontextplugin.so
# (il path che finisci-motore.sh passa ad assembla-bundle-qt612.py).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
ARCH=${ARCH:-aarch64}

# Fuori dalla SDK non c'è sb2: rientra nella chroot e rilancia questo script.
if ! command -v sb2 >/dev/null 2>&1; then
    source "$R/packaging/sdk-local.sh"
    sdk_run "ARCH='$ARCH' bash '$HERE/build-maliit-qt612.sh'"
    exit $?
fi

case "$ARCH" in
    aarch64) T=SailfishOS-5.1.0.11-aarch64-qt612 ;;
    armv7hl) T=SailfishOS-5.1.0.11-armv7hl-qt612 ;;
    *) echo "ARCH sconosciuta: $ARCH" >&2; exit 1 ;;
esac

SRC=$R/scratch/maliit-src/chum
UP=$SRC/upstream
BLD=$R/scratch/qt612/build-$ARCH/maliit
PATCH=$R/packaging/harbour-rootitanium/maliit-qt6.10-guiprivate.patch

# Sorgenti: clone shallow la prima volta (solo se mancano).
if [ ! -d "$UP" ]; then
    mkdir -p "$R/scratch/maliit-src"
    git clone --quiet https://github.com/sailfishos-chum/qt-sfos-maliit-platforminputcontext.git "$SRC"
    git -C "$SRC" checkout --quiet 9bb86e6
    git -C "$SRC" submodule update --init --recursive
fi
# La patch è idempotente: la applico solo se GuiPrivate non c'è ancora.
if ! grep -q 'Qt6::GuiPrivate' "$UP/CMakeLists.txt"; then
    patch -d "$UP" -p1 --forward < "$PATCH"
fi

mkdir -p "$BLD"
# La build gira come utente SDK: se lanciato da root i path sarebbero suoi.
[ "$(id -u)" -eq 0 ] && chown -R "${SDK_USER:-sailfish}" "$R/scratch/maliit-src" "$BLD" || true

sb2 -t "$T" -R cmake -S "$UP" -B "$BLD" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -Denable-docs=OFF -Denable-tests=OFF -Denable-glib=OFF -Denable-xcb=OFF \
    -Denable-wayland=OFF -Denable-qt6-inputcontext=ON -Denable-hwkeyboard=OFF
sb2 -t "$T" -R cmake --build "$BLD" -j"${J:-12}"
sb2 -t "$T" -R strip "$BLD/libmaliitplatforminputcontextplugin.so"
ls -l "$BLD/libmaliitplatforminputcontextplugin.so"

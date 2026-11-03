#!/bin/bash
# Compila il plugin di posizione (task 2.5 #4) via sb2.
# Si può lanciare dalla workstation:
#   ARCH=aarch64 packaging/rt-position-plugin/build.sh
# Lo script rientra da solo nella SDK chroot locale (vedi packaging/sdk-local.sh).
# ARCH=armv7hl per il 32 bit, QT612=1 contro lo stack Qt 6.12 del target clonato.
# Output: libqtposition_rtgeoclue1.so qui accanto, da installare nel bundle in
# plugins/position/.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-aarch64}

# Fuori dalla SDK non c'è sb2: rientra nella chroot e rilancia questo script.
if ! command -v sb2 >/dev/null 2>&1; then
  REPO=$(cd "$HERE/../.." && pwd)
  source "$REPO/packaging/sdk-local.sh"
  sdk_run "HERE='$HERE' ARCH='$ARCH' QT612='${QT612:-}' bash '$HERE/build.sh'"
  exit $?
fi

cd "$HERE"
case "$ARCH" in
  aarch64) T=SailfishOS-5.1.0.11-aarch64.default; L=lib64 ;;
  armv7hl) T=SailfishOS-5.1.0.11-armv7hl;         L=lib ;;
esac
# QT612=1: contro lo stack Qt 6.12 del target clonato (task 2.5 #6)
[ -n "${QT612:-}" ] && T=SailfishOS-5.1.0.11-$ARCH-qt612
OUT=${QT612:+build-$ARCH-qt612}; OUT=${OUT:-build-$ARCH}
INC="-I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtDBus -I/usr/include/qt6/QtPositioning"
mkdir -p $OUT
sb2 -t "$T" /usr/$L/qt6/libexec/moc $INC rtgeoclue1.cpp -o $OUT/rtgeoclue1.moc
sb2 -t "$T" c++ -std=c++17 -fPIC -O2 -shared $INC -I$OUT rtgeoclue1.cpp \
  -o $OUT/libqtposition_rtgeoclue1.so -L/usr/$L -lQt6Positioning -lQt6DBus -lQt6Core
sb2 -t "$T" strip $OUT/libqtposition_rtgeoclue1.so
ls -l $OUT/libqtposition_rtgeoclue1.so

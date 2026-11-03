#!/bin/bash
# RooTitanium — wire the droidmedia video capture (getUserMedia) path into the
# Chromium build tree, replacing the V4L2 factory, which on SailfishOS cannot
# see the camera (the Android HAL does not expose /dev/videoN).
#
# Why a script and not a patch, same as the decoder: the canonical sources live
# in ../droidcamera/ and are ~700 lines. Two copies (source + hand-written
# patch) drift apart on the first edit; here there is a single copy and the
# script carries it into the tree together with the wiring.
#
# IDEMPOTENT: re-running it is harmless, and it must be re-run after every
# regeneration of the build tree (like apply-droidmedia.sh and
# apply-build-fixes.sh).
#
# Usage, from the workstation:
#   ./apply-droidcamera.sh          # wire it in
#   ./apply-droidcamera.sh --check  # only report whether it is wired
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
VER=${VER:-6.8.4}
ARCH=${ARCH:-aarch64}
case "$ARCH" in
  aarch64) TOPDIR=build ;;
  armv7hl) TOPDIR=build-armv7hl ;;
  *) echo "ERROR: unknown ARCH: $ARCH" >&2; exit 1 ;;
esac
# BT overridable: for Chromium 140 the tree lives elsewhere (task 2.5 #6)
BT=${BT:-$REPO/packaging/qt6-qtwebengine-sfos/$TOPDIR/BUILD/qt6-qtwebengine-$VER/upstream}
SRC=$REPO/packaging/qt6-qtwebengine-sfos/droidcamera
CR=$BT/src/3rdparty/chromium
DEST=$CR/media/capture/video/droidmedia

die() { echo "ERROR: $*" >&2; exit 1; }
[ -d "$CR" ] || die "build tree not found: $CR (is VER=$VER right?)"
[ -d "$SRC" ] || die "camera sources not found: $SRC"

CAPTURE_GN=$CR/media/capture/BUILD.gn
FACTORY=$CR/media/capture/video/create_video_capture_device_factory.cc

if [ "${1:-}" = "--check" ]; then
    ok=0
    [ -f "$DEST/droid_video_capture_device.cc" ] && echo "✓ sources in the tree" || { echo "✗ sources missing"; ok=1; }
    grep -q droid_video_capture_device "$CAPTURE_GN" && echo "✓ GN wired" || { echo "✗ GN not wired"; ok=1; }
    grep -q DroidVideoCaptureDeviceFactory "$FACTORY" && echo "✓ factory wired" || { echo "✗ factory not wired"; ok=1; }
    exit $ok
fi

# ── 1) sources into the tree ─────────────────────────────────────────────────
mkdir -p "$DEST"
cp "$SRC/droid_video_capture_device.cc" \
   "$SRC/droid_video_capture_device.h" \
   "$SRC/droid_video_capture_device_factory.cc" \
   "$SRC/droid_video_capture_device_factory.h" "$DEST/"
echo "→ sources copied to media/capture/video/droidmedia/"

# ── 2) media/capture/BUILD.gn: pkg_config + sources ──────────────────────────
if ! grep -q droid_video_capture_device "$CAPTURE_GN"; then
    python3 - "$CAPTURE_GN" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

# pkg_config() is not a native GN function: the import is required.
imp = 'import("//build/config/linux/pkg_config.gni")'
if imp not in s:
    anchor_imp = 'import("//build/config/jumbo.gni")'
    assert anchor_imp in s, "reference import not found"
    s = s.replace(anchor_imp, anchor_imp + '\n' + imp, 1)

anchor = 'jumbo_component("capture_lib") {'
block = '''# RooTitanium: video capture via droidmedia (SailfishOS). The .pc comes from
# droidmedia-devel installed in the sb2 target; -ldroidmedia is the STATIC shim,
# so there is no .so dependency in the RPM (loading happens at runtime via
# hybris).
pkg_config("droidmedia_camera_config") {
  packages = [ "droidmedia" ]
}

'''
assert anchor in s, "capture_lib target not found"
s = s.replace(anchor, block + anchor, 1)

# Sources next to the Linux V4L2 ones. libyuv is already a capture_lib
# dependency (YCbCr -> I420 conversion), so nothing is added.
linux = '''  if (is_linux) {
    sources += [
      "video/linux/v4l2_capture_delegate_gpu_helper.cc",
'''
ours = '''  # RooTitanium: on SailfishOS the camera goes through droidmedia, not V4L2.
  # No new GN flag: this tree is only built for SFOS.
  if (is_linux) {
    sources += [
      "video/droidmedia/droid_video_capture_device.cc",
      "video/droidmedia/droid_video_capture_device.h",
      "video/droidmedia/droid_video_capture_device_factory.cc",
      "video/droidmedia/droid_video_capture_device_factory.h",
    ]
    configs += [ ":droidmedia_camera_config" ]
  }

'''
assert linux in s, "capture_lib linux block not found"
s = s.replace(linux, ours + linux, 1)
open(p, 'w', encoding='utf-8').write(s)
PY
    echo "→ media/capture/BUILD.gn wired"
else
    echo "= media/capture/BUILD.gn already wired"
fi

# ── 3) create_video_capture_device_factory.cc: our factory, if it answers ────
if ! grep -q DroidVideoCaptureDeviceFactory "$FACTORY"; then
    python3 - "$FACTORY" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

inc = '#include "media/capture/video/linux/video_capture_device_factory_linux.h"'
assert s.count(inc) >= 1, "Linux factory include not found"
s = s.replace(
    inc,
    inc + '\n#include "media/capture/video/droidmedia/droid_video_capture_device_factory.h"',
    1)

linux = '''#if BUILDFLAG(IS_LINUX)
  return std::make_unique<VideoCaptureDeviceFactoryLinux>(ui_task_runner);
#elif BUILDFLAG(IS_CHROMEOS)'''
ours = '''#if BUILDFLAG(IS_LINUX)
  // RooTitanium: on SailfishOS the camera is not a /dev/videoN. If droidmedia
  // answers (symbols + at least one camera) use it; otherwise stay on V4L2, so
  // the same binary does not regress on a native port or with an old
  // droidmedia.
  if (DroidVideoCaptureDeviceFactory::IsSupported()) {
    return std::make_unique<DroidVideoCaptureDeviceFactory>();
  }
  return std::make_unique<VideoCaptureDeviceFactoryLinux>(ui_task_runner);
#elif BUILDFLAG(IS_CHROMEOS)'''
assert linux in s, "IS_LINUX branch of the factory not found"
s = s.replace(linux, ours, 1)
open(p, 'w', encoding='utf-8').write(s)
PY
    echo "→ create_video_capture_device_factory.cc wired"
else
    echo "= create_video_capture_device_factory.cc already wired"
fi

echo
echo "Wiring complete. Verify with: $0 --check"
echo
# Editing BUILD.gn triggers the gn-regen on the first ninja, and that regen
# rewrites toolchain.ninja with .rsp names beyond NAME_MAX (255): "File name too
# long" halfway through the build. Same trap as the decoder, 17/09.
echo "⚠️  Now run:  ARCH=$ARCH ./apply-build-fixes.sh ninja"

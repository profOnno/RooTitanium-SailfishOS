# SFOS patches for QtWebEngine 6.140

Vendored copies of the Sailfish OS patches from
`https://github.com/sailfishos-chum/qt6-qtwebengine`, branch
`qt6-lts-6.11.1`, commit **a60d10a5aeda402897f7b18fbc15f5ccbccc9dc7**
(fetched 2026-11-03).

They are applied by `../scripts/prepare-sources-6.140.sh` before the
RooTitanium patches. Not included, on purpose:

| Patch | Why not |
|---|---|
| `qtwebengine-fix-build-on-SFOS-Comment-out-GL-includes.patch` | not needed on 6.140: `qt_overrides.cc` no longer includes the GL headers |
| `*system-openh264*`, `fix-building-with-system-ffmpeg` | the build bundles ffmpeg/openh264 (`-DFEATURE_webengine_system_ffmpeg=OFF`) |

`qtwebengine-aarch64-hwcap2-bti.patch` applies inside
`src/3rdparty/chromium`, not at the QtWebEngine root; the script takes care of
that.

To refresh them:

```sh
for p in qtwebengine-SIOCGSTAMP.patch qtwebengine-link-pipewire.patch \
         qtwebengine-aarch64-new-stat.patch qtwebengine-fix-build.patch \
         qtwebengine-aarch64-hwcap2-bti.patch \
         qtwebengine-sfos-egl-validating-command-decoder.patch \
         qtwebengine-sfos-shared-gl-texture-compositor.patch; do
  curl -fsSL -o "$p" \
    "https://raw.githubusercontent.com/sailfishos-chum/qt6-qtwebengine/a60d10a5aeda402897f7b18fbc15f5ccbccc9dc7/rpm/$p"
done
```

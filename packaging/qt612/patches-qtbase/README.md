# qtbase patches for Qt 6.12.0

Vendored from `https://github.com/sailfishos-chum/qt6-qtbase`, commit
**44e72841971dcb5cc9bf4594a21dfa5e42d76d0e** (fetched 2026-11-03).

They are the patches from the chum `qt6-qtbase` spec (6.8.4); all of them
still apply to 6.12.0. `prepare-qtbase.sh` applies them before the build.

| Patch | Why |
|---|---|
| `0001-workaround-sb2-filecopy-bug.patch` | sb2 leaves 000-mode files after some copy operations |
| `qtbase-CMake-Install-objects-files-into-ARCHDATADIR.patch` | install object files under ARCHDATADIR |
| `qtbase-use-only-major-minor-for-private-api-tag.patch` | private API tag = major.minor |
| `0010-disable-arm32-pixman-simd.patch` | armv7hl pixman SIMD workaround |
| `qtbase-version-check.patch` | namespace `QT_VERSION_CHECK` workaround |
| `qtbase-cxxflag.patch` | drop -O3, default to -O2 |
| `dont-use-statxmntid.patch` | **required**: Sailfish kernel headers lack `stx_mnt_id` |

Refresh:

```sh
for p in 0001-workaround-sb2-filecopy-bug.patch 0010-disable-arm32-pixman-simd.patch \
         dont-use-statxmntid.patch \
         qtbase-CMake-Install-objects-files-into-ARCHDATADIR.patch \
         qtbase-cxxflag.patch qtbase-use-only-major-minor-for-private-api-tag.patch \
         qtbase-version-check.patch; do
  curl -fsSL -o "$p" \
    "https://raw.githubusercontent.com/sailfishos-chum/qt6-qtbase/44e72841971dcb5cc9bf4594a21dfa5e42d76d0e/rpm/$p"
done
```

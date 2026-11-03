# Local SDK environment (this machine)

Notes for building on the current container. The 2026 build machine used a
Docker engine (`sailfish-sdk-build-engine_RootGPT`, user `mersdk`); this one has
the plain Platform SDK chroot instead. The repository scripts now use
`packaging/sdk-local.sh` and no longer need Docker.

## SDK

- Chroot: `/srv/sailfishos/sdks/sfossdk/sdk-chroot`
- SDK user: `sailfish` (home `/home/sailfish`); no `mersdk` user exists
- Version: Sailfish OS 5.1.0.11 (Pispala)
- Targets: `SailfishOS-latest-{aarch64,armv7hl,i486}` (+ `.default` snapshots)
- `mb2 1.4.94`, `sb2`, `sdk-assistant` are inside the chroot

## Disk layout

`/` is a nearly full btrfs LXD pool (a few GB free), while
`/mnt/ai_workspace` has ~680 GB. The SDK **targets** were therefore moved to the
workspace disk and bind-mounted back:

```sh
# now in /etc/fstab, mounted by systemd:
/mnt/ai_workspace/sailfish-sdk/targets /srv/sailfishos/targets none \
  bind,nofail,x-systemd.requires-mounts-for=/mnt/ai_workspace 0 0
```

Check it with `findmnt /srv/sailfishos/targets`; if the container ever starts
without the mount, re-apply it with:

```sh
sudo mount --bind /mnt/ai_workspace/sailfish-sdk/targets /srv/sailfishos/targets
```

Target clones and package installs then land on the workspace disk. The
workspace is also bind-mounted **inside** the chroot at the same path
(`/mnt/ai_workspace`, hook `mount_sdk` in `/home/sailfish/.mersdkrc`), so build
scripts can pass host paths unchanged instead of translating them to
`/parentroot/...`.

The hook also bind-mounts **the repository only** into every target
(`<target>/mnt/ai_workspace/RooTitanium`), because `sb2` chroots into the
target. Binding all of `/mnt/ai_workspace` there would recurse: the relocated
SDK targets live under it.

## Writes from sb2: use `-R`

scratchbox2 marks host paths as `readonly-if-not-root`: a normal
`sb2 -t <target> cmake ...` gets **EROFS** as soon as it writes outside the
target root or the home directory. Build commands must therefore use
`sb2 -t <target> -R cmake ...` (simulated root). Files stay owned by the SDK
user (`sailfish`), so this does not litter root-owned build artifacts.

This is why `build-module.sh`, `configure-6.140.sh` and `build-con-guardia.sh`
pass `-R` to `sb2`.

## Running commands

```sh
source packaging/sdk-local.sh
sdk_run "sb2 -t SailfishOS-latest-aarch64 rpm -q cmake"
sdk_exec sb2 -t SailfishOS-latest-aarch64 rpm -q cmake
```

`sdk_run` takes a single shell command line; `sdk_exec` takes a command plus
arguments. Both enter the chroot as `sailfish`, with `HOME=/home/sailfish`.
Override `SDK_CHROOT` / `SDK_USER` if the SDK moves.

## Still missing before a 6.140 build

See `Documentation/TASK-camera.md` and `Documentation/TASK-chromium-140.md`:

- Qt 6.12 stack in the cloned `…-qt612` targets;
- build deps in the target (`gcc`, `ninja`, `gperf`, `bison`, …) and
  `droidmedia-devel` from `adaptation-common`;
- `nodejs-bin` and `python3-html5lib` RPMs (not in the SFOS repos);
- `scratch/qtwebengine-6.140` with the Chromium submodule;
- the Qt 6.12.0 source tarballs in `scratch/qt612-src/`.

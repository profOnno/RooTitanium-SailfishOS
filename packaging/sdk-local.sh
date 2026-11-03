#!/bin/bash
# Local Sailfish Platform SDK helper.
#
# The build scripts used to run inside the Docker engine
# `sailfish-sdk-build-engine_RootGPT` as user `mersdk`. That container no longer
# exists on this machine; the SDK is the plain chroot at
# /srv/sailfishos/sdks/sfossdk, whose SDK user is `sailfish`.
#
# Source this file and use `sdk_run` / `sdk_exec` instead of
# `docker exec -u mersdk <engine> ...`:
#
#   source "$REPO/packaging/sdk-local.sh"
#   sdk_run "cd /mnt/ai_workspace/RooTitanium/scratch/... && sb2 -t TARGET ..."
#
# The chroot sees the workspace disk at the same path (/mnt/ai_workspace),
# thanks to the mount_sdk hook in /home/sailfish/.mersdkrc, so host paths can be
# passed through unchanged.
#
# Overridable: SDK_CHROOT, SDK_USER, SDK_HOME.

SDK_CHROOT=${SDK_CHROOT:-/srv/sailfishos/sdks/sfossdk/sdk-chroot}
SDK_USER=${SDK_USER:-sailfish}
SDK_HOME=${SDK_HOME:-/home/$SDK_USER}

sdk_run() {  # sdk_run "<shell command line>"
  local cmd=$1
  [ -x "$SDK_CHROOT" ] || {
    echo "ERROR: SDK chroot not found: $SDK_CHROOT" >&2
    return 1
  }
  if [ "$(id -u)" -eq 0 ]; then
    # sudo -u is used (not runuser) because sdk-chroot re-execs itself through
    # sudo when it is not root; running as the SDK user keeps $SUDO_USER and the
    # chroot's HOME consistent with an interactive session.
    sudo -u "$SDK_USER" -H "$SDK_CHROOT" bash -c "$cmd"
  elif [ "$(id -un)" = "$SDK_USER" ]; then
    "$SDK_CHROOT" bash -c "$cmd"
  else
    sudo -n -u "$SDK_USER" -H "$SDK_CHROOT" bash -c "$cmd"
  fi
}

sdk_exec() {  # sdk_exec <command> [args...]
  if [ "$(id -u)" -eq 0 ]; then
    sudo -u "$SDK_USER" -H "$SDK_CHROOT" "$@"
  elif [ "$(id -un)" = "$SDK_USER" ]; then
    "$SDK_CHROOT" "$@"
  else
    sudo -n -u "$SDK_USER" -H "$SDK_CHROOT" "$@"
  fi
}

# True when the Docker-based engine is available (old machines). New code
# should not depend on it; this only keeps the thermal guards working where
# they still make sense.
sdk_docker_engine_available() {
  command -v docker >/dev/null 2>&1 &&
    docker inspect "${ENGINE:-sailfish-sdk-build-engine_RootGPT}" >/dev/null 2>&1
}

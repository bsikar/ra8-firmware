#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/ci/install_unicorn.sh -- build + install the pinned Unicorn from
# source, reproducibly, so ra8_emulator decodes Armv8.1-M identically on every
# machine (devcontainer, dev box, self-hosted runner). See scripts/ci/unicorn_pin.sh
# for the pin and the reason it exists (#354).
#
# WHY FROM SOURCE, not apt:
#   - No distro ships the pinned upstream release at one apt version string
#     across Debian (dev box) and Ubuntu (runner / devcontainer), so an
#     `apt-get install libunicorn-dev=<ver>` pin cannot be identical everywhere.
#   - A source build of one tagged release, verified by sha256, IS byte-for-byte
#     reproducible everywhere -- exactly the arm-gcc URL+sha256 pattern (#178).
#
# This is provisioning, NOT a gate. It is invoked by .devcontainer/Dockerfile
# and run by hand when provisioning the dev box or a runner (docs/TOOLCHAIN.md).
# It is deliberately NOT called from any GitHub-workflow step: the ci-parity
# gate forbids a workflow "infra" step from invoking anything under scripts/,
# and enforcing the pin is the ra8_emulator gate's job (check_unicorn_version.sh),
# not the provisioning step's.
#
# Idempotent: if the pinned version is already installed at the prefix it prints
# and exits 0 without rebuilding.
#
# Usage:
#   /bin/bash -p scripts/ci/install_unicorn.sh              # Linux: /usr/local; macOS: per-user
#   RA8_UNICORN_PREFIX=$HOME/opt/unicorn /bin/bash -p scripts/ci/install_unicorn.sh
#   /bin/bash -p scripts/ci/install_unicorn.sh --force      # rebuild even if already pinned

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # shellcheck source=scripts/ci/unicorn_pin.sh
  source "$here/unicorn_pin.sh"

  force=0
  [[ "${1:-}" == "--force" ]] && force=1

  prefix="$RA8_UNICORN_PREFIX"

  # sudo only when the prefix is not writable by the current user (a $HOME prefix
  # needs none; /usr/local does). For a not-yet-created prefix, test the nearest
  # existing ancestor -- that is what `cmake --install` needs to write into. A box
  # without sudo and a root-owned prefix is a provisioning error, so surface it.
  maybe_sudo() {
    local p="$prefix"
    while [[ -n "$p" && "$p" != "/" && ! -e "$p" ]]; do p="$(dirname "$p")"; done
    if [[ -w "$p" ]]; then
      "$@"
    elif command -v sudo >/dev/null 2>&1; then
      sudo "$@"
    else
      echo "ERROR: cannot write to $prefix and sudo is unavailable." >&2
      return 1
    fi
  }

  # Read the version out of an installed header, if present. UC_VERSION_* are
  # defined to the UC_API_* triple, so read those directly (2 . 1 . 4).
  installed_header_version() {
    local hdr="$prefix/include/unicorn/unicorn.h"
    [[ -f "$hdr" ]] || return 1
    local maj min pat
    maj="$(sed -n 's/^#define[[:space:]]\{1,\}UC_API_MAJOR[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$hdr" | head -1)"
    min="$(sed -n 's/^#define[[:space:]]\{1,\}UC_API_MINOR[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$hdr" | head -1)"
    pat="$(sed -n 's/^#define[[:space:]]\{1,\}UC_API_PATCH[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$hdr" | head -1)"
    [[ -n "$maj" && -n "$min" && -n "$pat" ]] || return 1
    echo "${maj}.${min}.${pat}"
  }

  if [[ "$force" -eq 0 ]]; then
    if have="$(installed_header_version 2>/dev/null)" && [[ "$have" == "$RA8_UNICORN_VERSION" ]]; then
      echo "[install_unicorn] Unicorn $RA8_UNICORN_VERSION already installed at $prefix -- nothing to do."
      exit 0
    fi
  fi

  for tool in curl cmake make tar cc; do
    command -v "$tool" >/dev/null 2>&1 || {
      echo "ERROR: '$tool' is required to build Unicorn from source." >&2
      exit 1
    }
  done

  if command -v sha256sum >/dev/null 2>&1; then
    checksum_cmd=(sha256sum)
  elif command -v shasum >/dev/null 2>&1; then
    checksum_cmd=(shasum -a 256)
  else
    echo "ERROR: 'sha256sum' or 'shasum' is required to verify Unicorn source." >&2
    exit 1
  fi

  work="$(mktemp -d)"
  trap 'rm -rf "$work"' EXIT

  # Unicorn's bundled QEMU configure requires that pkg-config exists even when
  # this arm-only build has no pkg-config dependencies.  Keep optional probes
  # disabled when a minimal macOS toolchain lacks it.
  if ! command -v pkg-config >/dev/null 2>&1; then
    cat >"$work/pkg-config-unavailable" <<'EOF'
#!/bin/sh
exit 1
EOF
    chmod +x "$work/pkg-config-unavailable"
    export PKG_CONFIG="$work/pkg-config-unavailable"
  fi

  echo "[install_unicorn] downloading Unicorn $RA8_UNICORN_VERSION ..."
  curl -fsSL "$RA8_UNICORN_TARBALL_URL" -o "$work/unicorn.tar.gz"
  echo "${RA8_UNICORN_TARBALL_SHA256}  $work/unicorn.tar.gz" | "${checksum_cmd[@]}" -c -

  tar -xzf "$work/unicorn.tar.gz" -C "$work"
  src="$work/unicorn-${RA8_UNICORN_VERSION}"
  [[ -d "$src" ]] || {
    echo "ERROR: extracted tree $src not found." >&2
    exit 1
  }

  # Unicorn 2.1.4 reads CTR_EL0 when its macOS cache-size sysctl is unavailable.
  # That system register is privileged on Apple Silicon and terminates the host
  # process with SIGILL.  macOS has a safe fallback cache-line size, so retain the
  # register probe only on non-Apple AArch64 hosts.
  if [[ "$(uname -s)" == "Darwin" ]]; then
    cacheinfo="$src/qemu/util/cacheinfo.c"
    sed 's/^#if defined(__aarch64__)$/#if defined(__aarch64__) \&\& !defined(__APPLE__)/' \
      "$cacheinfo" >"$cacheinfo.tmp"
    mv "$cacheinfo.tmp" "$cacheinfo"
  fi

  echo "[install_unicorn] building (arm only, Release) ..."
  # UNICORN_ARCH=arm: ra8_emulator only emulates the Cortex-M (Armv8.1-M) core, so
  # building just the arm target keeps the build small + fast without changing the
  # arm decode. BUILD_SHARED_LIBS=ON produces libunicorn.so for dynamic linking +
  # ldconfig resolution, matching the runner's existing /usr/local layout.
  cmake -S "$src" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DUNICORN_ARCH=arm \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
  cmake --build "$work/build" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

  echo "[install_unicorn] installing to $prefix ..."
  maybe_sudo cmake --install "$work/build" >/dev/null

  # Refresh the loader cache so the freshly installed libunicorn.so is resolved
  # ahead of any stale distro package (only meaningful for a system prefix).
  # ldconfig lives in /sbin, which is NOT on a normal user's PATH, so a bare
  # `command -v ldconfig` misses it and the cache silently keeps resolving the old
  # copy -- probe the sbin paths explicitly. Best-effort: disable errexit AROUND
  # the call (not with `|| true`, which masks a multi-command callee's status).
  for ldc in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
    if command -v "$ldc" >/dev/null 2>&1; then
      set +e
      maybe_sudo "$ldc"
      set -e
      break
    fi
  done

  got="$(installed_header_version 2>/dev/null || echo unknown)"
  if [[ "$got" != "$RA8_UNICORN_VERSION" ]]; then
    echo "ERROR: post-install header reports '$got', expected '$RA8_UNICORN_VERSION'." >&2
    exit 1
  fi
  echo "[install_unicorn] Unicorn $RA8_UNICORN_VERSION installed at $prefix."
else
  [[ "$-" == *p* ]]
fi

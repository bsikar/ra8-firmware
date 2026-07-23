#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/install_unicorn.sh -- build + install the pinned Unicorn from
# source, reproducibly, so board_sim decodes Armv8.1-M identically on every
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
# and enforcing the pin is the board_sim gate's job (check_unicorn_version.sh),
# not the provisioning step's.
#
# Idempotent: if the pinned version is already installed at the prefix it prints
# and exits 0 without rebuilding.
#
# Usage:
#   scripts/ci/install_unicorn.sh              # install to $RA8_UNICORN_PREFIX (/usr/local)
#   RA8_UNICORN_PREFIX=$HOME/opt/unicorn ...    # per-user prefix (no sudo)
#   scripts/ci/install_unicorn.sh --force      # rebuild even if already pinned

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

for tool in curl cmake make sha256sum tar cc; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERROR: '$tool' is required to build Unicorn from source." >&2
    exit 1
  }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "[install_unicorn] downloading Unicorn $RA8_UNICORN_VERSION ..."
curl -fsSL "$RA8_UNICORN_TARBALL_URL" -o "$work/unicorn.tar.gz"
echo "${RA8_UNICORN_TARBALL_SHA256}  $work/unicorn.tar.gz" | sha256sum -c -

tar -xzf "$work/unicorn.tar.gz" -C "$work"
src="$work/unicorn-${RA8_UNICORN_VERSION}"
[[ -d "$src" ]] || {
  echo "ERROR: extracted tree $src not found." >&2
  exit 1
}

echo "[install_unicorn] building (arm only, Release) ..."
# UNICORN_ARCH=arm: board_sim only emulates the Cortex-M (Armv8.1-M) core, so
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

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Provision a fresh macOS checkout to build and run ra8_emulator. Everything
# installed by this script stays under the invoking user's home directory.

set -euo pipefail

case "${1:-}" in
  "") ;;
  -h | --help)
    cat <<'EOF'
Usage: make emu-setup

Installs the macOS dependencies needed to build firmware and run ra8_emulator:
Xcode Command Line Tools, Homebrew packages, Arm GNU Toolchain 13.3.rel1, and
the pinned Unicorn library.  Installs remain in the invoking user's home
directory except for the standard Xcode/Homebrew installers.
EOF
    exit 0
    ;;
  *)
    echo "ERROR: unknown argument '$1' (try --help)." >&2
    exit 2
    ;;
esac

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "ERROR: scripts/emu/setup_macos.sh only supports macOS." >&2
  exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
prefix="${RA8_UNICORN_PREFIX:-${HOME}/.local/ra8-firmware/unicorn}"
arm_prefix="${HOME}/opt/arm-gnu-toolchain-13.3"
arm_release="13.3.rel1"

if ! xcode-select -p >/dev/null 2>&1; then
  echo "[emu-setup] requesting Xcode Command Line Tools (the full Xcode app is not required) ..." >&2
  xcode-select --install >/dev/null 2>&1 || true
  echo "ERROR: finish the Command Line Tools installer, then rerun: make emu-setup" >&2
  exit 1
fi
brew="$(command -v brew 2>/dev/null || true)"
if [[ -z "$brew" ]]; then
  for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
    if [[ -x "$candidate" ]]; then
      brew="$candidate"
      break
    fi
  done
fi
if [[ -z "$brew" ]]; then
  echo "[emu-setup] Homebrew is required; launching its official installer ..."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  brew="$(command -v brew 2>/dev/null || true)"
  if [[ -z "$brew" ]]; then
    for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
      if [[ -x "$candidate" ]]; then
        brew="$candidate"
        break
      fi
    done
  fi
fi
if [[ -z "$brew" ]]; then
  echo "ERROR: Homebrew installation did not provide brew; restart the shell, then rerun make emu-setup." >&2
  exit 1
fi
eval "$("$brew" shellenv)"

brew_missing=()
command -v cmake >/dev/null 2>&1 || brew_missing+=(cmake)
capstone_prefix="$("$brew" --prefix capstone 2>/dev/null || true)"
[[ -f "$capstone_prefix/include/capstone/capstone.h" ]] || brew_missing+=(capstone)
if [[ "${#brew_missing[@]}" -gt 0 ]]; then
  echo "[emu-setup] installing Homebrew dependencies: ${brew_missing[*]} ..."
  "$brew" install "${brew_missing[@]}"
else
  echo "[emu-setup] Homebrew dependencies already installed."
fi

arm_gcc="$arm_prefix/bin/arm-none-eabi-gcc"
if [[ ! -x "$arm_gcc" ]] || [[ "$("$arm_gcc" -dumpfullversion 2>/dev/null || true)" != 13.3.* ]]; then
  case "$(uname -m)" in
    arm64)
      arm_arch="aarch64"
      arm_sha256="c8824bffd057afce2259f7618254e840715f33523a3d4e4294f471208f976764"
      ;;
    x86_64)
      arm_arch="x86_64"
      arm_sha256="95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4"
      ;;
    *)
      echo "ERROR: unsupported macOS architecture $(uname -m) for Arm GNU Toolchain." >&2
      exit 1
      ;;
  esac
  arm_url="https://developer.arm.com/-/media/Files/downloads/gnu/${arm_release}/binrel/arm-gnu-toolchain-${arm_release}-${arm_arch}-arm-none-eabi.tar.xz"
  arm_work="$(mktemp -d)"
  trap 'rm -rf "$arm_work"' EXIT
  echo "[emu-setup] installing Arm GNU Toolchain $arm_release to $arm_prefix ..."
  curl -fsSL "$arm_url" -o "$arm_work/arm-gcc.tar.xz"
  echo "$arm_sha256  $arm_work/arm-gcc.tar.xz" | shasum -a 256 -c -
  rm -rf "$arm_prefix"
  mkdir -p "$arm_prefix"
  tar -xf "$arm_work/arm-gcc.tar.xz" -C "$arm_prefix" --strip-components=1
fi
echo "[emu-setup] using $($arm_gcc -dumpfullversion) Arm GNU Toolchain"

echo "[emu-setup] installing pinned Unicorn to $prefix ..."
RA8_UNICORN_PREFIX="$prefix" bash "$root/scripts/ci/install_unicorn.sh"

echo "[emu-setup] configuring ra8_emulator ..."
emu_build="$root/tools/ra8_emulator/build"
# A pre-setup configure may have cached Homebrew's incompatible Unicorn bottle.
# Reset only this host-tool build so CMake resolves the freshly pinned library.
rm -rf "$emu_build"
cmake -S "$root/tools/ra8_emulator" -B "$emu_build"
cmake --build "$emu_build" --parallel

echo "[emu-setup] ready: make emu-blink"

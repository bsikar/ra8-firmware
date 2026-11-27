#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/builders/find_tools.sh -- locate tools the scripts/emu gates need
# when they are not on the system PATH.
#
# The CI runner and the devcontainer provision every tool these gates call by
# bare name on the system PATH: zig, cmake/just/ninja, and the pinned Arm GNU
# 13.3 toolchain (docs/TOOLCHAIN.md). A developer box does not have to: the Arm
# toolchain installs user-space (the tarball extracts under ~/opt as
# arm-gnu-toolchain-<rel>-x86_64-arm-none-eabi), zig distributions unpack
# wherever they were downloaded, and the cmake/just/ninja binaries land in
# ~/.local/bin (the standard user-install prefix, which non-login shells do not
# source from .profile). This fragment discovers those installs and exports
# them into PATH, so a local run of smoke.sh / eil_all.sh / matrix.sh needs no
# hand-built PATH -- and no sudo. Explicit overrides (RA8_ZIG,
# RA8_ARM_TOOLCHAIN_BIN) keep their documented precedence.
#
# SOURCED, NEVER EXECUTED. Each entry point sources this file and calls
# ra8_find_tools_activate ONCE, right before the first tool use -- after every
# early-exit mode (--selftest, --list), which must stay tool-free so the gate
# wiring itself can be audited on a box that has nothing installed.

# Return success when $1 is already a PATH entry.
ra8_path_has() { # dir -> 0 when dir is a PATH entry
  case ":$PATH:" in *":$1:"*) return 0 ;; *) return 1 ;; esac
}

# Return success when dotted-numeric version $1 is greater than $2. awk, not
# `sort -V`: macOS ships BSD sort, which has no -V, and these gates must pick
# the right zig on both platforms.
ra8_version_gt() { # v1 v2 -> 0 when v1 > v2
  awk -v a="$1" -v b="$2" 'BEGIN {
    n = split(a, A, "."); m = split(b, B, ".")
    for (i = 1; i <= (n > m ? n : m); i++) {
      x = (i <= n ? A[i] + 0 : 0); y = (i <= m ? B[i] + 0 : 0)
      if (x > y) exit 0
      if (x < y) exit 1
    }
    exit 1
  }'
}

# Echo the absolute path of the zig binary to use. Precedence: $RA8_ZIG, then
# PATH, then the layouts a zig distribution unpacks to. When several
# candidates exist the NEWEST wins: ra8_emulator's build.zig.zon sets
# minimum_zig_version and `zig build` aborts mid-gate on a stale one, so a
# 0.13 top-level binary unpacked beside a 0.17 distribution must not shadow it
# merely by sitting at the shorter path.
ra8_find_zig() { # -> zig path on stdout, non-zero with a message when none
  if [ -n "${RA8_ZIG:-}" ]; then
    if [ ! -x "$RA8_ZIG" ]; then
      echo "error: RA8_ZIG='$RA8_ZIG' is not an executable" >&2
      return 1
    fi
    printf '%s\n' "$RA8_ZIG"
    return 0
  fi
  local _zig
  _zig="$(command -v zig 2>/dev/null || true)"
  if [ -n "$_zig" ]; then
    printf '%s\n' "$_zig"
    return 0
  fi
  local _cand _v _best="" _best_v=""
  for _cand in "$HOME"/zig/zig "$HOME"/zig/zig-*/zig "$HOME"/zig-*/zig \
               "$HOME"/opt/zig/zig "$HOME"/opt/zig/zig-*/zig "$HOME"/opt/zig-*/zig \
               /opt/zig/zig /opt/zig/zig-*/zig /opt/zig-*/zig; do
    [ -x "$_cand" ] || continue
    _v="$("$_cand" version 2>/dev/null)" || continue
    [ -n "$_v" ] || continue
    if [ -z "$_best_v" ] || ra8_version_gt "$_v" "$_best_v"; then
      _best="$_cand"
      _best_v="$_v"
    fi
  done
  if [ -z "$_best" ]; then
    echo "error: no zig on PATH or in the user-space install layouts" \
         "(~/zig, ~/opt/zig, /opt/zig); set RA8_ZIG=<path-to-zig>" >&2
    return 1
  fi
  printf '%s\n' "$_best"
}

# Export the discovered zig into PATH so every bare `zig build` in the gates
# (the emulator, mkfontimg, exfat_mkimage) resolves to it.
ra8_find_zig_activate() { # -> 0
  local _zig _dir
  _zig="$(ra8_find_zig)" || return 1
  _dir="${_zig%/*}"
  ra8_path_has "$_dir" && return 0
  export PATH="$_dir:$PATH"
  return 0
}

# Export the pinned Arm GNU toolchain into the environment when it is not on
# PATH: prepend its bin dir so the bare arm-none-eabi-* calls in the gates
# resolve, and export RA8_ARM_TOOLCHAIN_BIN so cmake/toolchain-ra8d2.cmake's
# HINTS agree with what was found instead of falling back to whatever stray
# arm-gcc a later PATH search would surface. The version assertion in the
# toolchain file stays the authority on WHICH release this is; discovery only
# decides WHERE it is.
ra8_find_arm_toolchain_activate() { # -> 0
  command -v arm-none-eabi-gcc >/dev/null 2>&1 && return 0
  local _dir
  for _dir in ${RA8_ARM_TOOLCHAIN_BIN:+"$RA8_ARM_TOOLCHAIN_BIN"} \
              /opt/arm-gnu-toolchain-15.2/bin "$HOME"/opt/arm-gnu-toolchain-15.2/bin \
              /opt/arm-gnu-toolchain-13.3/bin "$HOME"/opt/arm-gnu-toolchain-13.3/bin \
              /opt/arm-gnu-toolchain-*/bin "$HOME"/opt/arm-gnu-toolchain-*/bin; do
    [ -n "$_dir" ] || continue
    [ -x "$_dir/arm-none-eabi-gcc" ] || continue
    ra8_path_has "$_dir" && return 0
    export PATH="$_dir:$PATH"
    export RA8_ARM_TOOLCHAIN_BIN="$_dir"
    return 0
  done
  echo "error: arm-none-eabi-gcc not found on PATH or in the documented" \
       "user-space layouts (/opt, ~/opt); set RA8_ARM_TOOLCHAIN_BIN=<toolchain>/bin" \
       "(see docs/TOOLCHAIN.md)" >&2
  return 1
}

# Locate every tool the emu gates call by bare name and export it into PATH.
# Also cover the user-install prefix and the no-make/ninja generator case:
#
# - ~/.local/bin: the cmake.org tarballs, just's installer and pipx all land
#   there, and run_just.sh already honours it as just's last resort; a
#   non-login shell just does not have it on PATH yet.
# - CMAKE_GENERATOR=Ninja when GNU make is absent but ninja is present:
#   cmake's default generator is Unix Makefiles, so the cross app builds would
#   fail to configure on such a box for no reason worth a system package.
#   Never set when make exists or the caller pinned a generator.
ra8_find_tools_activate() { # -> 0, or non-zero naming the missing tool
  local _user_local="${HOME:-}/.local/bin"
  if [ -d "$_user_local" ] && ! ra8_path_has "$_user_local"; then
    export PATH="$_user_local:$PATH"
  fi
  if ! command -v make >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1 &&
     [ -z "${CMAKE_GENERATOR:-}" ]; then
    export CMAKE_GENERATOR=Ninja
  fi
  ra8_find_zig_activate || return 1
  ra8_find_arm_toolchain_activate || return 1
  return 0
}

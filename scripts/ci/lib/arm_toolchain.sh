#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/arm_toolchain.sh -- resolve and verify the pinned Arm toolchain.
#
# SOURCED, NEVER EXECUTED. One home for "how a caller reaches the pinned
# cross-compiler", so scripts/ci.sh (every cross-build gate) and
# scripts/checks/clang_tidy.sh (its firmware pass, and the pre-commit hook that
# runs it) resolve the SAME arm-none-eabi-gcc. Sourced the same way as
# parallelism.sh / tool_env.sh.
#
# Why this is no longer left to each caller (#570): clang_tidy.sh's firmware
# pass needs a compiler that understands -mcpu=cortex-m85 (12.3+), and it fails
# LOUDLY with RC_INFRA when it does not (require_arm_system_includes). gate_tidy
# set that compiler up by calling use_pinned_arm_toolchain before clang_tidy.sh;
# the pre-commit hook (#569) did not, so on any box whose default
# arm-none-eabi-gcc is the distro 12.2 the hook's clang_tidy.sh aborted with
# exit 2 and -- correctly -- refused to ratchet, blocking every C commit.
# clang_tidy.sh now calls this itself, so whoever runs it gets the pinned
# toolchain rather than depending on the caller to remember.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_ARM_TOOLCHAIN_SH:-}" ]; then
  _RA8_ARM_TOOLCHAIN_SH=1

  # Keep this aligned with RA8_PINNED_ARM_GCC_VERSION in the CMake toolchain.
  # Patch releases within the 13.3 release line are accepted, exactly as CMake
  # accepts them; another major/minor is different code generation and fails.
  RA8_PINNED_ARM_GCC_MAJOR_MINOR="13.3"

  # Prepend the pinned Arm GNU Toolchain when the runner provisions it under
  # /opt (or $HOME/opt). The apt gcc-arm-none-eabi package ships no C++ standard
  # library, so apps with remaining C++ translation units fail with
  # "fatal error: cstddef"; the official ARM toolchain under /opt bundles
  # libstdc++. RA8_ARM_TOOLCHAIN_BIN is an operator override for a box that
  # keeps it elsewhere. Only an existing bin/ holding arm-none-eabi-gcc is
  # prepended; a no-op when none is found, in which case the caller's
  # require_arm_* guard fails loudly with remediation.
  use_pinned_arm_toolchain() {
    local candidate
    for candidate in \
      "${RA8_ARM_TOOLCHAIN_BIN:-}" \
      /opt/arm-gnu-toolchain-13.3/bin \
      "$HOME/opt/arm-gnu-toolchain-13.3/bin"; do
      if [[ -n "$candidate" && -x "$candidate/arm-none-eabi-gcc" ]]; then
        case "$PATH" in
          "$candidate" | "$candidate":*) ;;
          *) PATH="$candidate:$PATH" ;;
        esac
        export PATH
        return 0
      fi
    done
    # Resolution is intentionally non-fatal. Callers that need the compiler
    # immediately follow this with require_pinned_arm_toolchain(), which emits
    # the complete diagnostic rather than an unexplained `set -e` exit here.
    return 0
  }

  # Verify that GCC and the HIL-relevant binutils all come from one 13.3
  # release directory. Checking GCC alone is insufficient: an older nm or
  # objcopy later on PATH can inspect or rewrite a 13.3-built image and make a
  # HIL result depend on the shell that launched it.
  require_pinned_arm_toolchain() {
    local gcc_path gcc_dir tool tool_path tool_dir version
    local -a tools=(
      arm-none-eabi-gcc
      arm-none-eabi-g++
      arm-none-eabi-nm
      arm-none-eabi-objcopy
      arm-none-eabi-strings
    )

    use_pinned_arm_toolchain
    for tool in "${tools[@]}"; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool is missing from the pinned Arm GNU Toolchain." >&2
        echo "       Install 13.3.rel1 under /opt/arm-gnu-toolchain-13.3" >&2
        echo "       or \$HOME/opt/arm-gnu-toolchain-13.3, or set" >&2
        echo "       RA8_ARM_TOOLCHAIN_BIN to that release's bin directory." >&2
        return 1
      fi
    done

    gcc_path="$(command -v arm-none-eabi-gcc)"
    gcc_dir="$(cd "$(dirname "$gcc_path")" && pwd -P)"
    version="$(arm-none-eabi-gcc -dumpfullversion 2>/dev/null)" || {
      echo "ERROR: $gcc_path -dumpfullversion failed." >&2
      return 1
    }
    case "$version" in
      "$RA8_PINNED_ARM_GCC_MAJOR_MINOR" | "$RA8_PINNED_ARM_GCC_MAJOR_MINOR".*) ;;
      *)
        echo "ERROR: arm-none-eabi-gcc is $version ($gcc_path); expected the" >&2
        echo "       pinned 13.3.rel1 release (GCC 13.3.x)." >&2
        return 1
        ;;
    esac

    if ! arm-none-eabi-gcc -mcpu=cortex-m85 -E - </dev/null >/dev/null 2>&1; then
      echo "ERROR: $gcc_path does not accept -mcpu=cortex-m85." >&2
      return 1
    fi

    for tool in "${tools[@]:1}"; do
      tool_path="$(command -v "$tool")"
      tool_dir="$(cd "$(dirname "$tool_path")" && pwd -P)"
      if [[ "$tool_dir" != "$gcc_dir" ]]; then
        echo "ERROR: $tool resolves to $tool_path, outside the pinned GCC" >&2
        echo "       directory $gcc_dir." >&2
        return 1
      fi
      if ! "$tool" --version >/dev/null 2>&1; then
        echo "ERROR: $tool_path --version failed." >&2
        return 1
      fi
    done

    echo "PASS: Arm GNU Toolchain $version resolved from $gcc_dir"
  }

  _ra8_arm_toolchain_selftest_fixture() {
    local bin_dir="$1"
    local version="$2"
    local omit="${3:-}"
    local tool
    mkdir -p "$bin_dir"
    for tool in gcc g++ nm objcopy strings; do
      [[ "$tool" == "$omit" ]] && continue
      if [[ "$tool" == "gcc" ]]; then
        # shellcheck disable=SC2016  # the generated fixture, not this writer, expands $1
        printf '%s\n' \
          '#!/bin/sh' \
          'if [ "${1:-}" = "-dumpfullversion" ]; then' \
          "  printf '%s\\n' '$version'" \
          'fi' \
          'exit 0' >"$bin_dir/arm-none-eabi-$tool"
      else
        # shellcheck disable=SC2016  # the generated fixture, not this writer, expands $1
        printf '%s\n' \
          '#!/bin/sh' \
          'if [ "${1:-}" = "--version" ]; then' \
          "  printf '%s\\n' 'Arm GNU Toolchain $version'" \
          'fi' \
          'exit 0' >"$bin_dir/arm-none-eabi-$tool"
      fi
      chmod +x "$bin_dir/arm-none-eabi-$tool"
    done
  }

  # Both directions are load-bearing: a complete 13.3 bundle must pass, while
  # a version mismatch and a split/missing binutils bundle must both fail.
  # Each case drives the same resolver and probe the HIL gate invokes.
  arm_toolchain_selftest() (
    set -euo pipefail
    local tmp good bad incomplete first_path
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-arm-toolchain.XXXXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    good="$tmp/good/bin"
    bad="$tmp/bad/bin"
    incomplete="$tmp/incomplete/bin"
    _ra8_arm_toolchain_selftest_fixture "$good" "13.3.7"
    _ra8_arm_toolchain_selftest_fixture "$bad" "14.2.1"
    _ra8_arm_toolchain_selftest_fixture "$incomplete" "13.3.1" "nm"

    # The selected pin must move ahead of a wrong compiler and an existing
    # later copy of itself, without duplicating on the second resolution.
    RA8_ARM_TOOLCHAIN_BIN="$good" PATH="$bad:$good:/usr/bin:/bin"
    export RA8_ARM_TOOLCHAIN_BIN PATH
    require_pinned_arm_toolchain >/dev/null
    [[ "$(command -v arm-none-eabi-gcc)" == "$good/arm-none-eabi-gcc" ]]
    first_path="$PATH"
    use_pinned_arm_toolchain
    [[ "$PATH" == "$first_path" ]]

    RA8_ARM_TOOLCHAIN_BIN="$bad" PATH="/usr/bin:/bin"
    export RA8_ARM_TOOLCHAIN_BIN PATH
    if require_pinned_arm_toolchain >/dev/null 2>&1; then
      echo "arm_toolchain selftest: wrong GCC major/minor was accepted" >&2
      return 1
    fi

    RA8_ARM_TOOLCHAIN_BIN="$incomplete" PATH="/usr/bin:/bin"
    export RA8_ARM_TOOLCHAIN_BIN PATH
    if require_pinned_arm_toolchain >/dev/null 2>&1; then
      echo "arm_toolchain selftest: incomplete/split binutils were accepted" >&2
      return 1
    fi

    echo "arm_toolchain selftest: PASS"
  )
fi

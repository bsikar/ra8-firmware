# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/lib/arm_toolchain.sh -- put the pinned Arm GNU Toolchain on PATH.
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

  # Prepend the pinned Arm GNU Toolchain when the runner provisions it under
  # /opt (or $HOME/opt). The apt gcc-arm-none-eabi package ships no C++ standard
  # library, so C++ apps (ereader_shelf -> ra8_epub + tinyxml2) fail with
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
        PATH="$candidate:$PATH"
        export PATH
        return 0
      fi
    done
  }
fi

# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
# shellcheck disable=SC2034  # the TIDY_* arrays are assigned here and READ by clang_tidy.sh and invoke.sh, sourced alongside; shellcheck cannot see across the three
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/tidy/passes.sh -- The six passes, and the state they share.
#
# SOURCED, NEVER EXECUTED. See collect.sh for the loading contract.
#
# One function per pass, invoked by run_clang_tidy in the order they appear
# here. The order is part of the contract: the tidy gate pipes this script's
# whole output into tidy_ratchet.py, and keeping the passes in a fixed sequence
# keeps that log stable across runs and reviewable in a diff.
#
# Functions here: assert_scope_covered, detect_sdk_args,
# route_files_into_lists, run_pass_host, run_pass_firmware, run_pass_cxx,
# run_pass_objc, run_pass_ra8p1, run_pass_tools, run_all_passes

# ---------------------------------------------------------------------------
# State shared between run_clang_tidy and the per-pass functions below.
#
# Globals rather than parameters because these are bash ARRAYS, which cannot
# survive being passed through "$@" without the quoting of every element
# becoming load-bearing. Assembled once in run_clang_tidy, read-only here.
# ---------------------------------------------------------------------------
TIDY_LIST_DIR=""
TIDY_FIX_FLAG=()
TIDY_INCLUDE_ARG=()
TIDY_SDK_ARG=()
TIDY_DB_ARG=()

# ---------------------------------------------------------------------------
# Scope self-guard (#296). tools/ and tests/ went unlinted for the whole life
# of this script because collect_source_files quietly excluded them, and
# nothing failed when it did -- an empty bucket looked exactly like a clean
# one. Assert the buckets are populated so any future edit that narrows the
# scope fails loudly instead of silently shrinking what CI checks.
#
# $@  the collected file list
# ---------------------------------------------------------------------------
assert_scope_covered() {
  local files=("$@")
  local root matched f
  for root in tests tools libs src examples port; do
    matched=0
    for f in "${files[@]}"; do
      case "$f" in "$FIRMWARE_DIR/$root"/*)
        matched=1
        break
        ;;
      esac
    done
    if [[ "$matched" -eq 0 ]]; then
      print_error "Lint scope regression: no files collected under $root/."
      print_error "clang-tidy must cover every first-party root (see #296, #369)."
      exit "$RC_INFRA"
    fi
  done
}

# ---------------------------------------------------------------------------
# Resolve the macOS SDK into TIDY_SDK_ARG (a no-op on every other platform).
#
# On macOS the Homebrew-installed clang/clang-tidy ships its own resource
# directory but does NOT bundle the C standard library headers (string.h,
# stddef.h, etc.). Those live in the Command Line Tools SDK. Without an
# explicit -isystem the lint reports spurious "string.h file not found"
# errors. Resolve the SDK once here so every file gets the same flag.
# ---------------------------------------------------------------------------
detect_sdk_args() {
  TIDY_SDK_ARG=()
  [[ "$(uname -s)" == "Darwin" ]] || return 0
  local macos_sdk
  if macos_sdk="$(xcrun --show-sdk-path 2>/dev/null)" && [[ -d "$macos_sdk/usr/include" ]]; then
    # -isystem resolves the C stdlib headers; -isysroot additionally lets
    # clang-tidy find the macOS frameworks (CoreGraphics, AppKit) that the
    # host display backend includes -- without it the Command Line Tools
    # build reports a spurious "CoreGraphics/CoreGraphics.h file not found".
    TIDY_SDK_ARG=(--extra-arg="-isysroot" --extra-arg="$macos_sdk"
      --extra-arg="-isystem" --extra-arg="$macos_sdk/usr/include")
  fi
}

# ---------------------------------------------------------------------------
# Deal the collected files into one list file per pass, under TIDY_LIST_DIR.
#
# PER-RUN directory, never fixed paths under build/. These files used to be
# build/tidy-<pass>.files, which two concurrent runs -- a manual one and the
# pre-push hook, say -- silently share: the second run truncates the first's
# lists, whole passes come back with zero files, and `invoke_clang_tidy`
# returns early without printing anything. The run then reports success over
# a fraction of the tree, and against the ratchet it reads as findings having
# been burned down. Observed exactly once, costing 142 phantom fixes.
#
# $@  the collected file list
# ---------------------------------------------------------------------------
route_files_into_lists() {
  local pass
  for pass in host firmware cxx objc ra8p1 tools; do
    : >"$TIDY_LIST_DIR/$pass.files"
  done
  local f bucket
  for f in "$@"; do
    bucket="$(route_bucket "$f")"
    # A bucket with no pass behind it would land in a list file nothing ever
    # reads, and those files would go unlinted in silence -- the failure mode
    # this whole script is written to prevent. Refuse instead.
    case "$bucket" in
      host | firmware | cxx | objc | ra8p1 | tools) ;;
      *)
        print_error "route_bucket returned unknown bucket '$bucket' for $f."
        print_error "Every bucket needs a run_pass_* function, or its files go unlinted."
        exit "$RC_INFRA"
        ;;
    esac
    printf '%s\n' "$f" >>"$TIDY_LIST_DIR/$bucket.files"
  done
}

# ---------------------------------------------------------------------------
# Pass 1 -- host C, against the unit-test build's compile database.
# ---------------------------------------------------------------------------
run_pass_host() {
  invoke_clang_tidy "$1" "host C" "$TIDY_LIST_DIR/host.files" \
    "${TIDY_FIX_FLAG[@]}" "${TIDY_DB_ARG[@]}" "${TIDY_INCLUDE_ARG[@]}" "${TIDY_SDK_ARG[@]}"
}

# ---------------------------------------------------------------------------
# Pass 2 -- the cross-compiled firmware, against the CROSS database (#369).
#
# The database is built first and its own coverage check must pass, so this
# can never analyse a subset while reporting success.
# ---------------------------------------------------------------------------
run_pass_firmware() {
  [[ -s "$TIDY_LIST_DIR/firmware.files" ]] || return 0
  build_cross_db
  # Fail loudly HERE, in the current shell, if the cross-compiler cannot report
  # its own system include paths (#387). firmware_pass_args appends them, but it
  # is consumed through `mapfile < <(...)`, which discards the exit code -- so an
  # empty include list would otherwise sail through and turn ~100 firmware TUs
  # into clang-diagnostic-error.
  require_arm_system_includes
  local firmware_arg=()
  mapfile -t firmware_arg < <(firmware_pass_args)
  invoke_clang_tidy "$1" "firmware (cross)" "$TIDY_LIST_DIR/firmware.files" \
    "${TIDY_FIX_FLAG[@]}" "${firmware_arg[@]}" "${TIDY_SDK_ARG[@]}"
}

# ---------------------------------------------------------------------------
# Pass 3 -- first-party C++, same host database, C++ language flags (#370).
# ---------------------------------------------------------------------------
run_pass_cxx() {
  local cxx_arg=()
  mapfile -t cxx_arg < <(cxx_pass_args)
  invoke_clang_tidy "$1" "C++" "$TIDY_LIST_DIR/cxx.files" \
    "${TIDY_FIX_FLAG[@]}" "${cxx_arg[@]}" "${TIDY_INCLUDE_ARG[@]}" "${TIDY_SDK_ARG[@]}"
}

# ---------------------------------------------------------------------------
# Pass 4 -- Objective-C, which only reaches this list on macOS
# (collect_source_files drops it elsewhere, because AppKit does not exist to
# parse against). Say so out loud on other platforms: a bucket that is empty
# because the platform cannot do the work must not read the same as a bucket
# that came back clean.
# ---------------------------------------------------------------------------
run_pass_objc() {
  if [[ ! -s "$TIDY_LIST_DIR/objc.files" ]]; then
    print_warning "Objective-C not linted on $(uname -s): needs the macOS SDK (#370)."
    return 0
  fi
  local objc_arg=()
  mapfile -t objc_arg < <(objc_pass_args)
  invoke_clang_tidy "$1" "Objective-C" "$TIDY_LIST_DIR/objc.files" \
    "${TIDY_FIX_FLAG[@]}" "${objc_arg[@]}" "${TIDY_INCLUDE_ARG[@]}" "${TIDY_SDK_ARG[@]}"
}

# ---------------------------------------------------------------------------
# Pass 5 -- the RA8P1-only TUs, re-parsed with the RA8P1 device define.
#
# The RA8P1 build-foundation apps (examples/ra8p1_foundation/*), the
# RA8P1-only Ethos-U55 NPU driver TUs (libs/ra8_hal/{src/ra8_npu.c,
# inc/ra8_npu.h,inc/ra8_npu_regs.h}, plus the #227/#228 additions
# ra8_npu_loader.*, ra8_npu_quant.*, ra8_ethosu_kernel.{h,cc}), and the
# RA8P1 board layer (libs/ra8_board_ra8p1/*) carry -- directly or through
# ra8_ethosu_shim.h / ra8_device.h -- an `#error` guard that fires unless
# RA8_DEVICE_RA8P1 is defined; they are meant to be built ONLY with
# cmake/toolchain-ra8p1.cmake. clang-tidy compiles every example / lints every
# header in the DEFAULT (RA8D2) device context, so those files must be linted
# in a second pass that adds the RA8P1 device define; otherwise they abort at
# the guard with a clang-diagnostic-error instead of being analysed (the
# standalone `ra8_npu` headers hit the guard; ra8_npu.c is an empty TU without
# the define). The define is build-config only -- it does not change what the
# readability / bugprone checks report on their bodies (the concise-preprocessor
# nit still fires).
#
# These TUs are not part of the host test build, so the include dirs their
# CMake targets add are absent from compile_commands.json. Put them on the
# quote-include search path so those headers resolve (otherwise clang-tidy
# reports a clang-diagnostic-error and the degraded parse cascades into
# spurious readability findings):
#   - libs/ra8_board_ra8p1/inc  -- "ra8_board_ra8p1.h" for the board TUs.
#   - tools/vela/generated      -- the committed, generated .npub model header
#                                  that examples/ra8p1_foundation/npu_vela
#                                  includes (its CMake adds the same dir).
# ---------------------------------------------------------------------------
run_pass_ra8p1() {
  invoke_clang_tidy "$1" "ra8p1" "$TIDY_LIST_DIR/ra8p1.files" \
    "${TIDY_FIX_FLAG[@]}" "${TIDY_DB_ARG[@]}" --extra-arg="-DRA8_DEVICE_RA8P1" \
    --extra-arg="-I$FIRMWARE_DIR/libs/ra8_board_ra8p1/inc" \
    --extra-arg="-I$FIRMWARE_DIR/tools/vela/generated" \
    "${TIDY_INCLUDE_ARG[@]}" "${TIDY_SDK_ARG[@]}"
}

# ---------------------------------------------------------------------------
# Pass 6 -- the host dev tools, on a hand-assembled compile command because
# none of their own build files feed a compile database (see tools_pass_args).
# ---------------------------------------------------------------------------
run_pass_tools() {
  local tools_arg=()
  mapfile -t tools_arg < <(tools_pass_args)
  invoke_clang_tidy "$1" "tools" "$TIDY_LIST_DIR/tools.files" \
    "${TIDY_FIX_FLAG[@]}" "${TIDY_INCLUDE_ARG[@]}" "${TIDY_SDK_ARG[@]}" "${tools_arg[@]}"
}

# ---------------------------------------------------------------------------
# The pass sequence -- the one list of what a run does.
#
# Order is fixed and load-bearing: the tidy gate pipes the whole output into
# tidy_ratchet.py and a human reads it in a diff, so a run must always report
# host -> firmware -> C++ -> Objective-C -> ra8p1 -> tools.
#
# Every pass runs even when an earlier one found violations. Stopping at the
# first failure would report a partial picture as the whole one, and the
# ratchet would then read the passes that never ran as having been cleaned up.
#
# $1  clang-tidy binary
# ---------------------------------------------------------------------------
run_all_passes() {
  local clang_tidy="$1"
  local exit_code=0
  run_pass_host "$clang_tidy" || exit_code=1
  run_pass_firmware "$clang_tidy" || exit_code=1
  run_pass_cxx "$clang_tidy" || exit_code=1
  run_pass_objc "$clang_tidy" || exit_code=1
  run_pass_ra8p1 "$clang_tidy" || exit_code=1
  run_pass_tools "$clang_tidy" || exit_code=1
  return "$exit_code"
}

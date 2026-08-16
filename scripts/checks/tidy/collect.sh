#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
#
# scripts/checks/tidy/collect.sh -- What gets linted, and which pass owns it.
#
# SOURCED, NEVER EXECUTED. scripts/checks/clang_tidy.sh sources every file in
# this directory and is the only entry point. These fragments hold helper
# BODIES only: the option parsing, the pass orchestration and the selftest
# entry stay in the driver, so there is still exactly one way to run this.
#
# Functions here: collect_source_files, route_bucket

# ---------------------------------------------------------------------------
# Collect first-party source files (exclude vendor paths)
#
# Scope: EVERY first-party C-family file in the repository -- C, C++ and
# Objective-C, translation units and headers alike, under libs/, src/,
# tests/, tools/, examples/ and port/. CLAUDE.md ("Scope: these
# standards apply to EVERY first-party file in the repository") makes the
# host tools, the host test suite and the firmware subject to exactly the
# same rules; a file being "just an emulator", "just a test" or "just an
# example" is not a reason to relax them.
#
# The ONLY exemptions are vendored SOUP (libs/third_party/)
# and generated tables (libs/ra8_fonts/, tools/vela/generated/), matching the
# CLAUDE.md exemption list. Build trees and CMake-fetched deps are excluded
# because they are not source.
#
# Scope is derived from `git ls-files`, NOT from a directory glob. An earlier
# revision globbed `examples/*/*/main.c` to find app directories, which
# matched only the apps sitting exactly three levels deep while the tree's
# actual layout is examples/<tier>/.../<app>/, up to five deep. That is the
# #296 / #332 / #358 / #359 / #360 defect: a scan list that silently stopped
# matching the tree. Deriving from git means a new directory at any depth is
# picked up the day it lands.
#
# HOW A FILE GETS A COMPILE COMMAND (#369, #370)
# ----------------------------------------------
# clang-tidy cannot analyse a translation unit it cannot parse, and it cannot
# parse one without the flags it is really compiled with. There are three
# sources of those flags here, and `route_bucket` below picks between them:
#
#   host       the unit-test build's compile_commands.json -- describes the
#              host-buildable C and C++ in libs/, src/, tests/ and tools/.
#   firmware   a CROSS-COMPILE compile_commands.json built by
#              scripts/builders/build_cross_compile_db.py, covering every
#              cross-compiled TU in examples/ and port/ plus the
#              handful of libs/ and src/ TUs that include ThreadX / NetX /
#              USBX vendor headers.
#   fixed      a hand-assembled command, for the host dev tools whose own
#              per-tool build files never feed any compile database.
#
# The firmware bucket is what #369 was about. Pointing clang-tidy at the
# firmware against the HOST database was measured rather than assumed: it
# surfaced 135 findings across 96 files, and EVERY ONE was a
# clang-diagnostic-error rather than an actionable style finding. The answer
# was never a wider glob -- it was a database that describes how those TUs
# actually compile.
# ---------------------------------------------------------------------------
collect_source_files() {
  cd "$FIRMWARE_DIR" || return 1
  git ls-files --cached --others --exclude-standard |
    grep -E '\.(c|h|cpp|cc|cxx|hpp|hh|hxx|m)$' |
    grep -E '^(libs|src|tests|tools|examples|port)/' |
    # Vendored SOUP and generated tables -- the CLAUDE.md exemption list.
    grep -Ev '^(libs/third_party/|libs/ra8_fonts/|tools/vela/generated/)' |
    # Build trees and CMake-fetched deps are not source.
    grep -Ev '(^|/)(build|build-[^/]*|_deps)/' |
    while IFS= read -r f; do
      # Objective-C needs the macOS SDK's AppKit / CoreGraphics headers to
      # parse at all, so it can only be linted on a macOS host. Claim it only
      # where it can really be checked: `--list-files` feeds
      # check_lint_coverage.py, and claiming coverage this platform cannot
      # deliver would make the coverage matrix report a lint that never ran.
      case "$f" in
        *.m)
          [[ "$(uname -s)" == "Darwin" ]] || continue
          ;;
      esac
      printf '%s/%s\n' "$FIRMWARE_DIR" "$f"
    done || true
}

# ---------------------------------------------------------------------------
# Which pass owns a file -- the single place that decision is made.
#
# Prints one of: firmware | ra8p1 | tools | cxx | objc | host
#
# Kept as one function (rather than inlined into the loop that fills the pass
# lists) so `--selftest` can interrogate the routing directly. A router that
# quietly sent every file to one bucket would still "lint everything" and
# report success while analysing most files with the wrong flags.
# ---------------------------------------------------------------------------
route_bucket() {
  local f="$1"
  case "$f" in
    *.m) echo objc && return 0 ;;
    # C++, but cross-compiled: the first-party Ethos-U55 kernel is pulled into
    # the TFLite-micro object library by cmake/tflite_micro.cmake and exists
    # only in an RA8P1 build, so the CROSS database is the one that knows how
    # it compiles. Checked before the generic C++ rule below.
    */ra8_ethosu_kernel.cc) echo firmware && return 0 ;;
    *.cpp | *.cc | *.cxx | *.hpp | *.hh | *.hxx) echo cxx && return 0 ;;
  esac
  case "$f" in
    # ...except the HOSTED ports. port/posix/ binds fw_if_fs and
    # ra8_io_stream to the host kernel ABI, declares itself
    # `[Ring 4 / Host Port] {World: Host}`, and is compiled only by
    # tests/cmake/unit_tests.cmake -- so the HOST database is the one that
    # knows how it compiles, and no app cross-compiles it at all. Checked
    # before the generic port/ rule below, and kept in step with
    # HOST_PORT_ROOTS in scripts/builders/build_cross_compile_db.py.
    */port/posix/*) echo host && return 0 ;;
    # Cross-compiled firmware: examples/ and port/ in full.
    */examples/* | */port/*) echo firmware && return 0 ;;
    # A board layer's boot/ directory is the reset path compiled into every
    # image of that board -- firmware by any definition, but it lives under
    # libs/ and so used to fall through to the host bucket, whose database
    # does not contain it at all. clang-tidy then analysed it with default
    # HOSTED flags, which is precisely the wrong-flags failure this router
    # exists to prevent. It surfaced when `main` became `void main(void)`
    # behind `__STDC_HOSTED__ == 0`: a hosted-flags parse cannot see the
    # declaration and reports `use of undeclared identifier 'main'` (#707).
    */libs/ra8_board_*/boot/*) echo firmware && return 0 ;;
    # src/app is the ra8d2-ereader image -- a cross-compiled application built
    # by ra8_add_app(), registered in RA8_APPS, and present in the cross
    # database. It is firmware that happens not to live under examples/, and
    # routing it to the host bucket analysed its `void main(void)` entry point
    # as if it were a hosted program (#707).
    */src/app/*) echo firmware && return 0 ;;
  esac
  # A libs/ or src/ TU that includes a ThreadX / NetX / USBX vendor header is
  # firmware too: the host database carries no path to those headers, so it
  # parsed as a clang-diagnostic-error and used to be skipped outright. The
  # cross database compiles it for real and therefore knows where they live.
  if grep -qlE '#\s*include\s*[<"](tx_api|nx_api|ux_api)\.h[">]' "$f" 2>/dev/null; then
    echo firmware && return 0
  fi
  case "$f" in
    */tools/*) echo tools && return 0 ;;
    */ra8_npu.c | */ra8_npu.h | */ra8_npu_regs.h | \
      */ra8_npu_loader.c | */ra8_npu_loader.h | \
      */ra8_npu_quant.c | */ra8_npu_quant.h | \
      */ra8_ethosu_kernel.h | \
      */libs/ra8_board_ra8p1/*)
      echo ra8p1 && return 0
      ;;
  esac
  echo host
}

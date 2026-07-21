# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/tidy/pass_args.sh -- One argument builder per clang-tidy pass.
#
# SOURCED, NEVER EXECUTED. See collect.sh for the loading contract.
#
# Each function here prints, one per line, the arguments its pass needs on top
# of the common ones. Keeping them separate from the invoker is what lets a
# pass gain a flag without any risk to the other five: route_bucket decides
# WHICH pass a file lands in, these decide HOW that pass parses it.
#
# Functions here: db_pass_args, cxx_pass_args, objc_pass_args,
# arm_system_includes, firmware_pass_args, tools_pass_args

# ---------------------------------------------------------------------------
# Arguments shared by every pass that resolves its compile command from the
# host test build's compile_commands.json.
#
# Note: no --config-file. We let clang-tidy auto-discover .clang-tidy by
# walking up from each source file. That picks up the project-root config AND
# per-directory overrides (e.g. libs/ra8_nsc/src/.clang-tidy), which
# --config-file would suppress.
#
# The compile_commands.json captures GCC-only warning flags
# (-Wduplicated-branches, -Wduplicated-cond, -Wlogical-op,
# -Wformat-{overflow,truncation}=2). cmake/ra8_warnings.cmake gates these via
# $<COMPILE_LANG_AND_ID:C,GNU> generator expressions so they're emitted only
# when CC=gcc, but the generator expression resolves to literal flags in the
# compile_commands.json, which clang-tidy then sees as "unknown warning option"
# errors when it parses the file with clang. -Wno-unknown-warning-option
# silences those without affecting the actual GCC firmware build.
# ---------------------------------------------------------------------------
db_pass_args() {
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-std=c2x" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_SIMULATOR_MODE" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Arguments for the C++ pass.
#
# The .cpp / .cc shims (tinyxml2 EPUB + RaBook, the C++ allocator shim, the
# litehtml reflow v2 backend, the Ethos-U55 kernel wrapper) and their tests are
# host-buildable and sit in the SAME host compile database as the C -- so this
# reuses it, and only fixes up the language. `-std=c2x` from db_pass_args is a
# C standard and makes clang reject every C++ TU outright, which is why the
# C++ files needed their own pass rather than simply being added to the glob
# (#370). The C++ standard is taken from the database entry itself when the TU
# is in it; -std=c++17 is the floor the vendored tinyxml2 and litehtml need.
# ---------------------------------------------------------------------------
cxx_pass_args() {
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-xc++" \
    "--extra-arg-before=-std=c++17" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_SIMULATOR_MODE" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Arguments for the Objective-C pass (macOS only -- see collect_source_files).
#
# The two .m files are the Cocoa window backends for board_sim and ra8_viewer.
# They are compiled by their tools' own build files, so like the tools/ pass
# they get a fixed compile command rather than a database entry: the tool
# include directories, and -xobjective-c so clang parses the language it is.
# The macOS SDK root is supplied separately by run_clang_tidy.
# ---------------------------------------------------------------------------
objc_pass_args() {
  local dir
  for dir in "$FIRMWARE_DIR"/tools/*/ "$FIRMWARE_DIR"/tools/*/inc "$FIRMWARE_DIR"/tools/*/src; do
    [[ -d "$dir" ]] && printf -- '--extra-arg=-I%s\n' "${dir%/}"
  done
  printf '%s\n' \
    "--extra-arg-before=-xobjective-c" \
    "--extra-arg-before=-std=gnu2x" \
    "--extra-arg=-fobjc-arc" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Every -isystem the cross-compiler itself reports, asked of the compiler
# rather than restated here. `-E -v` prints the search list between two
# marker lines; everything indented in between is a directory.
#
# The database built by build_cross_db names the GCC driver, but clang-tidy
# parses with clang, which does not know where that toolchain keeps
# <string.h>. Without these, 135 of the firmware TUs failed with "'string.h'
# file not found" and were never analysed at all. They are ASKED FOR, not
# hardcoded: the compiler prints its own search list, so a toolchain bump
# moves them automatically.
# ---------------------------------------------------------------------------
arm_system_includes() {
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  command -v "$cc" &>/dev/null || return 0
  "$cc" -mcpu=cortex-m85 -mthumb -E -v -x c /dev/null 2>&1 |
    sed -n '/#include <\.\.\.> search starts here/,/End of search list/p' |
    grep '^ ' |
    while IFS= read -r dir; do
      dir="${dir# }"
      [[ -d "$dir" ]] && printf -- '--extra-arg=-isystem%s\n' "$dir"
    done || true
}

# ---------------------------------------------------------------------------
# Arguments for the firmware (cross-compile) pass, on top of CROSS_DB_DIR.
#
# Beyond the toolchain's own system includes above, this adds
# -Wno-unknown-warning-option for the same GCC-only warning flags the host
# pass has to neutralise.
# ---------------------------------------------------------------------------
firmware_pass_args() {
  # readability-magic-numbers is OFF for this pass, and this is a
  # de-duplication rather than an exemption: check_magic_numbers.py owns the
  # no-magic-numbers rule over exactly these files, and it is the only one of
  # the two that models the rule correctly.
  #
  # Measured on the firmware surface: 887 magic-number findings, of which 822
  # were rows of USB descriptor byte tables -- `0x09U, 0x02U, 0x20U, ...`.
  # This project has an explicit, reasoned position on those (see the
  # `_DATA_ROW_RE` comment in check_magic_numbers.py): a row of pure numeric
  # literals is data, not logic, and the rule is meaningless for it. clang-tidy
  # has no data-row concept and cannot be taught one. The remaining 65 were
  # real, and check_magic_numbers.py now reports every one of them -- its
  # scope used to stop at `main.c` inside examples/, so a whole block of
  # hand-addressed SAU registers had been sitting unflagged. All are fixed.
  #
  # So the rule is still enforced here, by the checker that gets it right; what
  # is switched off is a second, coarser implementation of the same rule that
  # would only ever report the 822 it gets wrong.
  printf '%s\n' \
    "-p=$CROSS_DB_DIR" \
    "--checks=-readability-magic-numbers" \
    "--extra-arg=-Wno-unknown-warning-option"
  arm_system_includes
}

# ---------------------------------------------------------------------------
# Arguments for the tools/ pass.
#
# The host dev tools are built by their own per-tool build files -- two CMake
# projects (board_sim, mkbookimg/mkfontimg) and three Makefiles (cache_bench,
# glyph_bench, reader_vmem) -- and none of them feed the host test compile
# database. Rather than configure five separate build trees just to obtain a
# compile command, hand clang-tidy a fixed one: the caller's include union
# (every -I the firmware build uses, so the ra8_*_regs.h headers board_sim
# consumes resolve), plus each tool's own source and inc directory, plus the
# Unicorn/Capstone headers board_sim needs, discovered through pkg-config.
#
# Nothing here is a per-file allowlist: the directories are globbed, so a new
# tool or a new tools/<tool>/inc is picked up with no edit. A tool that grows
# an include this does not cover fails LOUDLY as a clang-diagnostic-error
# rather than being silently skipped.
# ---------------------------------------------------------------------------
tools_pass_args() {
  local dir
  for dir in "$FIRMWARE_DIR"/tools/*/ "$FIRMWARE_DIR"/tools/*/inc "$FIRMWARE_DIR"/tools/*/src; do
    [[ -d "$dir" ]] && printf -- '--extra-arg=-I%s\n' "${dir%/}"
  done
  local cflag
  local pkg
  if command -v pkg-config &>/dev/null; then
    # unicorn + capstone: tools/board_sim. libcurl: tools/media_dl's network
    # backend. Without the include dir clang-tidy cannot find the header and
    # reports clang-diagnostic-error instead of linting the file at all -- a
    # silent hole exactly like the one #296 closed, so keep this in step with
    # the runner + devcontainer package lists.
    #
    # Query one package per call on purpose: `pkg-config --cflags a b c` prints
    # NOTHING when any single package is absent, which would silently drop the
    # other two packages' include dirs and turn their TUs into parse errors.
    for pkg in unicorn capstone libcurl; do
      for cflag in $(pkg-config --cflags "$pkg" 2>/dev/null); do
        printf -- '--extra-arg=%s\n' "$cflag"
      done
    done
  fi
  # Homebrew installs unicorn/capstone outside the default search path and
  # ships no .pc on some formulae versions; add its include root when present.
  [[ -d /opt/homebrew/include ]] && printf -- '--extra-arg=-I/opt/homebrew/include\n'
  # _GNU_SOURCE mirrors tools/media_dl/CMakeLists.txt: mdl_export.c uses
  # posix_spawn_file_actions_addchdir_np() and `environ`, which glibc gates
  # behind it. Without it here the TU degrades to clang-diagnostic-error and
  # is never actually linted.
  #
  # CMAKE_C_EXTENSIONS is ON for every tool (board_sim) or -std=gnu23 is set
  # explicitly (the Makefile tools), so lint them as GNU C23 too.
  printf '%s\n' '--' 'cc' '-std=gnu2x' '-D_GNU_SOURCE'
}

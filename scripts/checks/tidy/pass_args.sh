#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
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
# arm_system_includes, gcc_integer_constant_macros, firmware_pass_args,
# tools_pass_args

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
# ---------------------------------------------------------------------------
# The one place the no-magic-numbers de-duplication is written down.
#
# scripts/checks/check_magic_numbers.py OWNS this rule across every first-party
# .c and .h, it runs blocking in the pre-commit-checks gate, and it is the only
# one of the two implementations that models the rule correctly: it knows about
# data rows (a line of pure numeric literals is data, not logic -- 822 of the
# firmware surface's 887 findings were USB descriptor tables), it honours
# MAGIC-OK and clang-tidy's own NOLINT waivers, and it carries the project's
# explicit, written position that unit tests are exempt because a test is built
# from literal stimulus and expected-value vectors.
#
# clang-tidy's readability-magic-numbers is a second, coarser implementation of
# the same rule. The firmware pass switched it off for exactly this reason; the
# host, C++, tools and Objective-C passes never did, so they kept reporting a
# rule the tree enforces elsewhere and deliberately exempts here -- 398 findings
# on this branch, essentially all of them in test code, against a ratchet
# baseline that carries not one magic-number row.
#
# This is de-duplication, not exemption. The rule is still enforced, by the
# checker that gets it right, over a scope that now includes headers precisely
# so that this claim stays true.
# ---------------------------------------------------------------------------
magic_number_dedup_arg() {
  printf '%s\n' "--checks=-readability-magic-numbers"
}

db_pass_args() {
  magic_number_dedup_arg
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-std=c2x" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_OFF_TARGET" \
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
  magic_number_dedup_arg
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-xc++" \
    "--extra-arg-before=-std=c++17" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_OFF_TARGET" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Arguments for the Objective-C pass (macOS only -- see collect_source_files).
#
# The two .m files are the Cocoa window backends for ra8_emulator and ra8_viewer.
# They are compiled by their tools' own build files, so like the tools/ pass
# they get a fixed compile command rather than a database entry: the tool
# include directories, and -xobjective-c so clang parses the language it is.
# The macOS SDK root is supplied separately by run_clang_tidy.
# ---------------------------------------------------------------------------
objc_pass_args() {
  magic_number_dedup_arg
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
#
# FAILS rather than emitting nothing on an unusable compiler (#387). The distro
# default arm-none-eabi-gcc 12.2 cannot parse -mcpu=cortex-m85: it answers the
# query with an EMPTY search list and exit 1, and the old `|| return 0` / `||
# true` turned that into an empty-but-successful include set. ~100 firmware TUs
# then parsed with no system headers, every one a clang-diagnostic-error, and a
# ratchet baseline generated in that state would freeze those parse errors
# forever (the exact symptom #369 existed to fix). An empty list is
# indistinguishable from "a compiler with no system includes", which is not a
# real configuration, so this returns non-zero and prints nothing to stdout
# when it cannot obtain a usable, marker-bearing list. require_arm_system_includes
# is the loud, synchronous guard the firmware pass calls before trusting it --
# process substitution swallows this exit code, so a plain caller cannot.
# ---------------------------------------------------------------------------
arm_system_includes() {
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  command -v "$cc" &>/dev/null || return 1
  local raw
  raw="$("$cc" -mcpu=cortex-m85 -mthumb -E -v -x c /dev/null 2>&1)" || true
  local -a dirs=()
  local dir
  while IFS= read -r dir; do
    dir="${dir# }"
    [[ -d "$dir" ]] && dirs+=("$dir")
  done < <(printf '%s\n' "$raw" |
    sed -n '/#include <\.\.\.> search starts here/,/End of search list/p' |
    grep '^ ')
  # Floor: a non-empty list that carries the arm-none-eabi sysroot marker.
  # Below that the query did not really answer, so fail rather than hand back
  # an empty set that reads as success.
  [[ ${#dirs[@]} -gt 0 ]] || return 1
  local have_marker=0
  for dir in "${dirs[@]}"; do
    case "$dir" in *arm-none-eabi*)
      have_marker=1
      break
      ;;
    esac
  done
  [[ "$have_marker" -eq 1 ]] || return 1
  printf -- '--extra-arg=-isystem%s\n' "${dirs[@]}"
}

# ---------------------------------------------------------------------------
# The loud guard for arm_system_includes, called synchronously by the firmware
# pass (run_pass_firmware) BEFORE the include list is captured.
#
# arm_system_includes is otherwise consumed through `mapfile < <(...)`, and
# process substitution discards the inner exit code -- so a non-zero return
# there would be silently ignored and the empty list used anyway. This runs it
# in the current shell, and on failure names the offending compiler and its
# version and aborts the whole gate with RC_INFRA, so a broken toolchain can
# never reach clang-tidy (or, worse, the ratchet baseline) as an empty include
# set (#387).
# ---------------------------------------------------------------------------
require_arm_system_includes() {
  arm_system_includes >/dev/null && return 0
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  local version
  version="$("$cc" --version 2>&1 | head -1)" || version="<not found on PATH>"
  print_error "arm_system_includes: could not obtain ARM system include paths."
  print_error "  compiler: $cc"
  print_error "  version:  ${version:-<unknown>}"
  print_error "  A compiler that cannot answer -mcpu=cortex-m85 (the distro-default"
  print_error "  arm-none-eabi-gcc 12.2 is the usual culprit) yields an EMPTY include"
  print_error "  list; ~100 firmware TUs would then parse as clang-diagnostic-error and"
  print_error "  a ratchet baseline built now would freeze those parse errors forever."
  print_error "  Put the pinned 13.3 toolchain on PATH (use_pinned_arm_toolchain, or"
  print_error "  point RA8_ARM_TOOLCHAIN_BIN / ARM_CC at it)."
  exit "$RC_INFRA"
}

# ---------------------------------------------------------------------------
# The ten `__INTn_C` / `__UINTn_C` constant builders GCC predefines and clang
# does not.
#
# The firmware lane really is compiled `-ffreestanding`, so `__STDC_HOSTED__`
# is 0 and the toolchain's <stdint.h> hands the job to GCC's own
# `stdint-gcc.h`. That header spells every constant macro as
# `#define UINT64_C(c) __UINT64_C(c)` and relies on a GCC predefine; clang
# predefines only `__UINT64_C_SUFFIX__`. So the first `UINT64_C()` in a
# firmware TU is `use of undeclared identifier '__UINT64_C'` -- a
# clang-diagnostic-error that ABORTS THE PARSE, leaving that whole translation
# unit unanalysed while the run still reports a finding count. That is the #369
# shape, and `libs/ra8_c6link/inc/ra8_c6link_mdl_transfer.h`, reached from
# three apps and two libraries, sat in exactly that state.
#
# ASKED FOR, not restated: the bodies come from the cross-compiler's own `-dM`
# dump, so a toolchain bump moves them and no suffix is guessed here. Below the
# floor this returns non-zero rather than emitting a partial set -- a
# half-answered query would silently restore the parse abort for whichever
# width it dropped, which is the failure this exists to remove.
# ---------------------------------------------------------------------------
gcc_integer_constant_macros() {
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  command -v "$cc" &>/dev/null || return 1
  local -a defs=()
  local def
  while IFS= read -r def; do
    defs+=("--extra-arg=-D$def")
  done < <("$cc" -mcpu=cortex-m85 -mthumb -dM -E -x c /dev/null 2>/dev/null |
    sed -nE 's/^#define (__U?INT(8|16|32|64|MAX)_C\(c\)) /\1=/p')
  # Floor: signed and unsigned across 8, 16, 32, 64 and MAX -- ten rows.
  [[ "${#defs[@]}" -eq 10 ]] || return 1
  printf '%s\n' "${defs[@]}"
}

# ---------------------------------------------------------------------------
# The loud guard for gcc_integer_constant_macros, called synchronously by the
# firmware pass for the same reason require_arm_system_includes is: the list is
# otherwise consumed through `mapfile < <(...)`, which discards the exit code,
# so a compiler that answered nothing would hand back an empty set that reads
# as success and every UINT64_C-bearing TU would go back to being unanalysed.
# ---------------------------------------------------------------------------
require_gcc_integer_constant_macros() {
  gcc_integer_constant_macros >/dev/null && return 0
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  print_error "gcc_integer_constant_macros: the cross-compiler did not report its"
  print_error "  __INTn_C / __UINTn_C constant builders."
  print_error "  compiler: $cc"
  print_error "  clang does not predefine them, and -ffreestanding selects GCC's"
  print_error "  stdint-gcc.h, which needs them -- so without this every firmware TU"
  print_error "  using UINT64_C() and friends fails to PARSE and is left unanalysed."
  print_error "  Put the pinned 13.3 toolchain on PATH (use_pinned_arm_toolchain, or"
  print_error "  point RA8_ARM_TOOLCHAIN_BIN / ARM_CC at it)."
  exit "$RC_INFRA"
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
  # -ffreestanding: the firmware lane really is compiled that way
  # (cmake/ra8_add_app.cmake), so analysing it any other way analyses a
  # program the build never produces. For a TU the cross database describes,
  # this merely restates the flag already in its command line; it is
  # load-bearing for the TUs the database does NOT describe -- e.g.
  # libs/ra8_board_ra8p1/boot/vector_table.c appears in no compile database at
  # all, so clang-tidy falls back to defaults, and a hosted default cannot see
  # the freestanding `void main(void)` declaration that
  # libs/ra8_core/inc/ra8_boot_entry.h guards behind `__STDC_HOSTED__ == 0`
  # (#707).
  magic_number_dedup_arg
  printf '%s\n' \
    "-p=$CROSS_DB_DIR" \
    "--extra-arg=-Wno-unknown-warning-option" \
    "--extra-arg=-ffreestanding"
  arm_system_includes
  gcc_integer_constant_macros
}

# ---------------------------------------------------------------------------
# Arguments for the tools/ pass.
#
# The host dev tools are built by their own per-tool build files -- two CMake
# projects (ra8_emulator, mkbookimg/mkfontimg) and three Makefiles (cache_bench,
# glyph_bench, reader_vmem) -- and none of them feed the host test compile
# database. Rather than configure five separate build trees just to obtain a
# compile command, hand clang-tidy a fixed one: the caller's include union
# (every -I the firmware build uses, so the ra8_*_regs.h headers ra8_emulator
# consumes resolve), plus each tool's own source and inc directory, plus the
# Unicorn/Capstone headers ra8_emulator needs, discovered through pkg-config.
#
# Nothing here is a per-file allowlist: the directories are globbed, so a new
# tool or a new tools/<tool>/inc is picked up with no edit. A tool that grows
# an include this does not cover fails LOUDLY as a clang-diagnostic-error
# rather than being silently skipped.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Four fixture-path definitions each tool CMakeLists supplies through
# target_compile_definitions. Without them the TU stops at a deliberate
# `#error "... must name the ..."` guard and is never linted -- a parse failure
# reported as a finding, which is the #369 shape again. Mirroring them here is
# safe precisely BECAUSE of those guards: if a name changes, the #error fires
# and says so rather than the value drifting silently.
# ---------------------------------------------------------------------------
tools_fixture_definitions() {
  printf -- '--extra-arg=-DRA8_FMT_TEST_PNG="%s"\n' \
    "$FIRMWARE_DIR/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png"
  printf -- '--extra-arg=-DMDL_SITE_CONFIG_DIR="%s"\n' "$FIRMWARE_DIR/tools/media_dl/sites"
  printf -- '--extra-arg=-DMDL_SITE_FIXTURE_DIR="%s"\n' \
    "$FIRMWARE_DIR/tools/media_dl/tests/fixtures/sites"
  # ...and the shipped-descriptor count, counted the same way the listfile
  # counts it (a CONFIGURE_DEPENDS glob over sites/*.conf) so the two cannot
  # disagree about how many descriptors ship.
  local site_count
  site_count="$(git -C "$FIRMWARE_DIR" ls-files "tools/media_dl/sites/*.conf" | wc -l)"
  printf -- '--extra-arg=-DMDL_SHIPPED_SITE_COUNT=%s\n' "$site_count"
}

tools_pass_args() {
  magic_number_dedup_arg
  local dir
  for dir in "$FIRMWARE_DIR"/tools/*/ "$FIRMWARE_DIR"/tools/*/inc "$FIRMWARE_DIR"/tools/*/src; do
    [[ -d "$dir" ]] && printf -- '--extra-arg=-I%s\n' "${dir%/}"
  done
  # ...and every OTHER directory under tools/ that really holds a header.
  # The three globs above stop at tools/<tool>/src, but ra8_emulator keeps its
  # private headers a level deeper (src/host/emu_host_io_internal.h,
  # src/engine/emu_elf_source_internal.h), so 78 of its TUs came back as
  # clang-diagnostic-error "file not found" -- not linted at all, just
  # unparsed. Derived from git rather than from a fixed depth, for the same
  # reason the collection is: a new nested directory is picked up the day it
  # lands, and a gitignored build tree is never on the list.
  while IFS= read -r dir; do
    printf -- '--extra-arg=-I%s/%s\n' "$FIRMWARE_DIR" "$dir"
  done < <(git -C "$FIRMWARE_DIR" ls-files "tools/*.h" "tools/*.hpp" |
    sed -E "s#/[^/]+\$##" | sort -u)
  local cflag
  local pkg
  if command -v pkg-config &>/dev/null; then
    # unicorn + capstone: tools/ra8_emulator. libcurl: tools/media_dl's network
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
  tools_fixture_definitions
  # CMAKE_C_EXTENSIONS is ON for every tool (ra8_emulator) or -std=gnu23 is set
  # explicitly (the Makefile tools), so lint them as GNU C23 too.
  printf '%s\n' '--' 'cc' '-std=gnu2x' '-D_GNU_SOURCE'
}

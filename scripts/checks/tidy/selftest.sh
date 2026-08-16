#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
#
# scripts/checks/tidy/selftest.sh -- Proving the scope and routing still work.
#
# SOURCED, NEVER EXECUTED. See collect.sh for the loading contract.
#
# Every failure mode this script has ever had was silent: a glob that stopped
# matching, a bucket that emptied, a root that was quietly dropped. None of
# them made anything go red. So assert the positive (each bucket claims the
# files it should) AND the negative (a file that must NOT land in a bucket does
# not), and fail loudly on either.
#
# Functions here: selftest_routing, selftest_scope, run_selftest

# ---------------------------------------------------------------------------
# Routing: each path shape must reach its own pass. The firmware roots are
# the #369 regression; the C++/Objective-C rows are the #370 one.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The include-fragment carve-out, in both directions, over REAL files -- the
# rule reads file contents, so fake paths cannot exercise it.
#
# Must fire: a header declaring a non-inline file-scope `static` function.
# Must stay quiet: a header of `static inline` accessors (the HAL shape), and
# a header with no `static` at all. A carve-out that widened to every header
# would drop the whole first-party header surface out of the analysis and read
# as a smaller, cleaner run.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The exact-path fragment registry must name only files that still exist and
# must route every one of them to `included`. A stale row is how a registry
# rots into an allowlist nobody re-reads, and it would go on silently
# exempting a path that had moved.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_fragment_registry() {
  local failures=0 registered
  if [[ "${#TIDY_HEADER_FRAGMENTS[@]}" -eq 0 ]]; then
    print_error "selftest: the fragment registry is empty"
    failures=$((failures + 1))
  fi
  for registered in "${TIDY_HEADER_FRAGMENTS[@]}"; do
    if [[ ! -f "$FIRMWARE_DIR/$registered" ]]; then
      print_error "selftest: registered fragment $registered no longer exists"
      failures=$((failures + 1))
    elif [[ "$(route_bucket "$FIRMWARE_DIR/$registered")" != "included" ]]; then
      print_error "selftest: registered fragment $registered did not route to included"
      failures=$((failures + 1))
    fi
  done
  printf "%s\n" "$failures"
}

selftest_include_fragments() {
  local failures=0 tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tidy-frag.XXXXXXXX")"
  printf "%s\n" "RA8_INTERNAL static int internal_helper(void);" >"$tmp/fragment.h"
  printf "%s\n" "static inline int accessor(void) { return 0; }" >"$tmp/inline.h"
  printf "%s\n" "int public_api(void);" >"$tmp/plain.h"
  local got
  got="$(route_bucket "$tmp/fragment.h")"
  if [[ "$got" != "included" ]]; then
    print_error "selftest: a static-declaring header routed to '$got', expected 'included'"
    failures=$((failures + 1))
  fi
  for got in inline plain; do
    if [[ "$(route_bucket "$tmp/$got.h")" == "included" ]]; then
      print_error "selftest: $got.h was treated as an include fragment"
      failures=$((failures + 1))
    fi
  done
  rm -rf -- "$tmp"

  failures=$((failures + $(selftest_fragment_registry)))

  # ...and the live floor: the real tree carries this class, so a rule that
  # stopped matching would report zero and go unnoticed.
  local live=0 f
  while IFS= read -r f; do
    [[ "$(route_bucket "$f")" == "included" ]] && live=$((live + 1))
  done < <(git -C "$FIRMWARE_DIR" ls-files "tests/support/*.h")
  if [[ "$live" -eq 0 ]]; then
    print_error "selftest: no tests/support header is recognised as an include fragment"
    failures=$((failures + 1))
  fi
  printf "%s\n" "$failures"
}

selftest_routing() {
  local failures=0
  local -a cases=(
    "$FIRMWARE_DIR/examples/x/y/main.c:firmware"
    "$FIRMWARE_DIR/port/usbx/src/a.c:firmware"
    "$FIRMWARE_DIR/port/posix/src/fw_if_fs_posix.c:host"
    "$FIRMWARE_DIR/libs/ra8_epub/src/shim.cpp:cxx"
    "$FIRMWARE_DIR/libs/ra8_hal/src/k.cc:cxx"
    "$FIRMWARE_DIR/tools/ra8_emulator/src/display/board_view.m:objc"
    "$FIRMWARE_DIR/tools/ra8_emulator/src/x.c:tools"
    "$FIRMWARE_DIR/libs/ra8_core/src/ra8_err.c:host"
    "$FIRMWARE_DIR/libs/ra8_board_ra8p1/src/b.c:ra8p1"
  )
  local entry path want got
  for entry in "${cases[@]}"; do
    path="${entry%:*}"
    want="${entry##*:}"
    got="$(route_bucket "$path")"
    if [[ "$got" != "$want" ]]; then
      print_error "selftest: $path routed to '$got', expected '$want'"
      failures=$((failures + 1))
    fi
  done
  failures=$((failures + $(selftest_include_fragments)))
  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# Scope, in both directions, over the real collection.
#
# NOTE ON `grep -q` AND `set -o pipefail`, which cost a debugging round here:
# `grep -q` exits the moment it matches, so the writer upstream of it dies of
# SIGPIPE (141) and pipefail then reports the whole pipeline as FAILED even
# though the match succeeded. Every match test below therefore feeds grep
# from a here-string, never through a pipe.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The generated-source registry, both directions. It must name at least one
# path (an empty registry silently lints generated bytes again), every path it
# names must be OUT of the collection, and a hand-authored neighbour in the
# same directory must still be IN it -- so a carve-out that widened from the
# exact path to its whole directory fails here rather than reporting a
# smaller, cleaner run.
#
# $1  the collected listing
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_generated_registry() {
  local listing="$1"
  local failures=0 generated_list rel
  generated_list="$(generated_source_paths)"
  if [[ -z "$generated_list" ]]; then
    print_error "selftest: the generated-source registry is empty"
    failures=$((failures + 1))
  fi
  while IFS= read -r rel; do
    [[ -z "$rel" ]] && continue
    if grep -qxF "$FIRMWARE_DIR/$rel" <<<"$listing"; then
      print_error "selftest: generated source $rel is claimed but must be exempt"
      failures=$((failures + 1))
    fi
  done <<<"$generated_list"
  if ! grep -qxF "$FIRMWARE_DIR/libs/ra8_c6link/src/ra8_c6link_mdl.c" <<<"$listing"; then
    print_error "selftest: a hand-authored neighbour of a generated source is no longer claimed"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_scope() {
  local failures=0
  local listing
  listing="$(collect_source_files)"

  # Every first-party root must actually be claimed. This is the assertion
  # that would have caught #296 and #369 on the day they landed.
  local root count
  for root in libs src tests tools examples port; do
    count="$(grep -c "^$FIRMWARE_DIR/$root/" <<<"$listing" || true)"
    if [[ "$count" -eq 0 ]]; then
      print_error "selftest: no files collected under $root/ -- scope regression"
      failures=$((failures + 1))
    fi
  done

  # The negative direction. Vendored SOUP and generated tables must NOT be
  # claimed: a scope that swallowed them would report coverage this project
  # explicitly does not want, and would bury real findings under SOUP noise.
  local forbidden
  for forbidden in libs/third_party libs/ra8_fonts tools/vela/generated; do
    if grep -q "^$FIRMWARE_DIR/$forbidden/" <<<"$listing"; then
      print_error "selftest: $forbidden/ is claimed but must be exempt"
      failures=$((failures + 1))
    fi
  done

  failures=$((failures + $(selftest_generated_registry "$listing")))

  # C++ and Objective-C must be present in the collection at all. Before #370
  # the collection matched `*.c` and `*.h` only, so both languages were
  # invisible -- and an invisible language reports no findings forever.
  local note
  for note in cpp cc; do
    if ! grep -q "\.$note\$" <<<"$listing"; then
      print_error "selftest: no .$note files collected -- C++ is out of scope again"
      failures=$((failures + 1))
    fi
  done
  if [[ "$(uname -s)" == "Darwin" ]] && ! grep -q '\.m$' <<<"$listing"; then
    print_error "selftest: no .m files collected on macOS -- Objective-C is out of scope"
    failures=$((failures + 1))
  fi

  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# arm_system_includes must FAIL, never emit an empty success, when the compiler
# cannot answer the -mcpu=cortex-m85 query (#387). Assert both directions:
#
#   * negative -- a compiler that exists but prints no search list (fake
#     with `true`, exactly what gcc 12.2 does for cortex-m85), and a compiler
#     that is not on PATH, must both make arm_system_includes return non-zero;
#   * positive -- whatever arm-none-eabi-gcc on PATH CAN target cortex-m85 must
#     yield a non-empty list carrying the arm-none-eabi sysroot marker.
#
# The positive half is skipped (not failed) when no cortex-m85-capable compiler
# is on PATH, so a developer running --selftest without the pinned toolchain
# still exercises the load-bearing negative half. clang_tidy.sh main() runs
# use_pinned_arm_toolchain before every mode, so the positive half fires
# whenever the pinned toolchain is installed, --selftest included.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_arm_includes() {
  local failures=0

  if ARM_CC=true arm_system_includes >/dev/null 2>&1; then
    print_error "selftest: arm_system_includes succeeded for a compiler that emits no includes"
    failures=$((failures + 1))
  fi
  if ARM_CC="ra8-no-such-cc-$$" arm_system_includes >/dev/null 2>&1; then
    print_error "selftest: arm_system_includes succeeded for a missing compiler"
    failures=$((failures + 1))
  fi

  if arm-none-eabi-gcc -mcpu=cortex-m85 -E - </dev/null >/dev/null 2>&1; then
    local out=""
    if ! out="$(arm_system_includes)"; then
      print_error "selftest: arm_system_includes failed for a working cortex-m85 compiler"
      failures=$((failures + 1))
    elif ! grep -q 'arm-none-eabi' <<<"$out"; then
      print_error "selftest: arm_system_includes output lacks the arm-none-eabi sysroot marker"
      failures=$((failures + 1))
    fi
  fi

  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# --selftest: prove the scope and the routing still work, in BOTH directions.
# ---------------------------------------------------------------------------
run_selftest() {
  local failures=0
  failures=$((failures + $(selftest_routing)))
  failures=$((failures + $(selftest_scope)))
  failures=$((failures + $(selftest_arm_includes)))

  if [[ "$failures" -ne 0 ]]; then
    print_error "clang_tidy.sh selftest FAILED with $failures problem(s)."
    return 1
  fi
  print_success "clang_tidy.sh selftest: scope and pass routing OK"
  return 0
}

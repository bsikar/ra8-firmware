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
# Functions here: selftest_routing, selftest_scope, selftest_arm_includes,
# selftest_gcc_constant_macros, selftest_firmware_c23_mode,
# selftest_included_header_diagnostics, selftest_tidy_tool_resolution,
# run_selftest

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
# The exact-path fragment registry is the ownership source for the shipped
# HeaderFilterRegex. Prove existence/routing, its content-derived half, and the
# regex mirror in both directions: stale, missing, new, ordinary, generated,
# or vendored paths must all make the selftest fail.
# ---------------------------------------------------------------------------
fragment_registry_paths() {
  local record
  for record in ${TIDY_HEADER_FRAGMENT_RECORDS[@]+"${TIDY_HEADER_FRAGMENT_RECORDS[@]}"}; do
    printf '%s\n' "${record#*|}"
  done
}

selftest_fragment_records() {
  local failures=0 record mode rel
  if [[ "${#TIDY_HEADER_FRAGMENT_RECORDS[@]}" -ne 28 ]]; then
    print_error "selftest: expected 28 owned include fragments, got ${#TIDY_HEADER_FRAGMENT_RECORDS[@]}"
    failures=$((failures + 1))
  fi
  if [[ -n "$(fragment_registry_paths | sort | uniq -d)" ]]; then
    print_error "selftest: duplicate include-fragment registry path"
    failures=$((failures + 1))
  fi
  for record in ${TIDY_HEADER_FRAGMENT_RECORDS[@]+"${TIDY_HEADER_FRAGMENT_RECORDS[@]}"}; do
    mode="${record%%|*}"
    rel="${record#*|}"
    if [[ "$mode" != "static-decl" && "$mode" != "includer-context" ]]; then
      print_error "selftest: invalid fragment ownership mode $mode for $rel"
      failures=$((failures + 1))
    elif [[ ! -f "$FIRMWARE_DIR/$rel" ]]; then
      print_error "selftest: registered fragment $rel no longer exists"
      failures=$((failures + 1))
    elif [[ "$(route_bucket "$FIRMWARE_DIR/$rel")" != "included" ]]; then
      print_error "selftest: registered fragment $rel did not route to included"
      failures=$((failures + 1))
    fi
  done
  printf '%s\n' "$failures"
}

selftest_fragment_content_census() {
  local failures=0 record mode rel abs state
  for record in ${TIDY_HEADER_FRAGMENT_RECORDS[@]+"${TIDY_HEADER_FRAGMENT_RECORDS[@]}"}; do
    mode="${record%%|*}"
    rel="${record#*|}"
    if ! state="$(header_static_decl_state "$FIRMWARE_DIR/$rel")"; then
      failures=$((failures + 1))
      continue
    fi
    if [[ "$mode" == "static-decl" && "$state" != yes ]]; then
      print_error "selftest: static-decl fragment $rel no longer has that shape"
      failures=$((failures + 1))
    elif [[ "$mode" == "includer-context" && "$state" != no ]]; then
      print_error "selftest: $rel now belongs in the derived static-decl class"
      failures=$((failures + 1))
    fi
  done
  while IFS= read -r abs; do
    case "$abs" in *.h | *.hh | *.hpp | *.hxx) ;; *) continue ;; esac
    rel="${abs#"$FIRMWARE_DIR"/}"
    if ! state="$(header_static_decl_state "$abs")"; then
      failures=$((failures + 1))
      continue
    fi
    if [[ "$state" == yes ]] && ! header_fragment_mode "$rel" >/dev/null; then
      print_error "selftest: new unregistered include fragment $rel"
      failures=$((failures + 1))
    fi
  done < <(collect_source_files)
  printf '%s\n' "$failures"
}

configured_header_filter_regex() {
  sed -n "s/^HeaderFilterRegex: '\\(.*\\)'$/\\1/p" "$FIRMWARE_DIR/.clang-tidy"
}

selftest_fragment_header_filter() {
  local failures=0 regex rel record matched=0 direct=0 abs
  regex="$(configured_header_filter_regex)"
  if [[ -z "$regex" ]]; then
    print_error "selftest: .clang-tidy has no readable HeaderFilterRegex"
    printf '%s\n' 1
    return 0
  fi
  while IFS= read -r rel; do
    case "$rel" in *.h | *.hh | *.hpp | *.hxx) ;; *) continue ;; esac
    [[ -f "$FIRMWARE_DIR/$rel" ]] || continue
    if grep -Eq "$regex" <<<"$FIRMWARE_DIR/$rel"; then
      matched=$((matched + 1))
      if ! header_fragment_mode "$rel" >/dev/null; then
        print_error "selftest: HeaderFilterRegex admits non-fragment $rel"
        failures=$((failures + 1))
      fi
    fi
  done < <(git -C "$FIRMWARE_DIR" ls-files)
  for record in ${TIDY_HEADER_FRAGMENT_RECORDS[@]+"${TIDY_HEADER_FRAGMENT_RECORDS[@]}"}; do
    rel="${record#*|}"
    if ! grep -Eq "$regex" <<<"$FIRMWARE_DIR/$rel"; then
      print_error "selftest: HeaderFilterRegex omits owned fragment $rel"
      failures=$((failures + 1))
    fi
  done
  [[ "$matched" -eq 28 ]] || {
    print_error "selftest: HeaderFilterRegex matched $matched headers, expected 28"
    failures=$((failures + 1))
  }
  while IFS= read -r abs; do
    case "$abs" in *.h | *.hh | *.hpp | *.hxx) ;; *) continue ;; esac
    rel="${abs#"$FIRMWARE_DIR"/}"
    header_fragment_mode "$rel" >/dev/null || direct=$((direct + 1))
  done < <(collect_source_files)
  [[ "$direct" -ge 800 ]] || {
    print_error "selftest: only $direct ordinary headers remain direct lint inputs"
    failures=$((failures + 1))
  }
  print_status "selftest: header ownership direct=$direct included=$matched"
  printf '%s\n' "$failures"
}

selftest_reviewer_header_ownership() {
  local failures=0 threadx xml
  threadx="port/esp-hosted/tests/inc/ra8_esp_hosted_tx_shim_internal.h"
  xml="apps/shared_libs/xml/tests/inc/ra8_rabook_xml_shim_test_fixture_internal.h"
  if [[ "$(route_bucket "$FIRMWARE_DIR/$threadx")" == "included" ]]; then
    print_error "selftest: ThreadX shim header lost direct-lint ownership"
    failures=$((failures + 1))
  fi
  if [[ "$(route_bucket "$FIRMWARE_DIR/$xml")" == "included" ]]; then
    print_error "selftest: XML fixture header lost direct-lint ownership"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_malformed_fragment() {
  local path="$1" scan_rc=0
  route_bucket "$path" >/dev/null 2>&1 || scan_rc=$?
  if [[ "$scan_rc" -ne "$RC_INFRA" ]]; then
    print_error "selftest: malformed header classifier did not fail closed"
    return 1
  fi
  return 0
}

selftest_fragment_registry() {
  local failures=0
  failures=$((failures + $(selftest_fragment_records)))
  failures=$((failures + $(selftest_fragment_content_census)))
  failures=$((failures + $(selftest_fragment_header_filter)))
  failures=$((failures + $(selftest_reviewer_header_ownership)))
  printf '%s\n' "$failures"
}

create_fragment_routing_fixtures() {
  local tmp="$1"
  printf "%s\n" "RA8_INTERNAL static int internal_helper(void);" >"$tmp/fragment.h"
  printf "%s\n" "RA8_INTERNAL static int" "internal_helper(void);" >"$tmp/multiline_return.h"
  printf "%s\n" "RA8_INTERNAL" "static" "int internal_helper(void);" >"$tmp/multiline_specifiers.h"
  printf "%s\n" "static inline int accessor(void) { return 0; }" >"$tmp/inline.h"
  printf "%s\n" "static inline int accessor(void) { return 0; }" \
    "RA8_INTERNAL static int internal_helper(void);" >"$tmp/mixed.h"
  printf "%s\n" "int public_api(void);" >"$tmp/plain.h"
  printf "%s\n" "static int value = internal_helper();" >"$tmp/static_data.h"
  printf "%s\n" "static int (*handler)(void);" >"$tmp/static_function_pointer.h"
  printf "%s\n" "void live_source(void);" >"$tmp/live.c"
  printf "%s\n" '#include "tx_api.h"' >"$tmp/vendor_api.c"
  printf "%s\n" "static int broken(" >"$tmp/malformed.h"
}

selftest_include_fragments() {
  local failures=0 tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tidy-frag.XXXXXXXX")"
  create_fragment_routing_fixtures "$tmp"
  local got
  for got in fragment mixed multiline_return multiline_specifiers; do
    if [[ "$(route_bucket "$tmp/$got.h")" != "included" ]]; then
      print_error "selftest: $got.h did not route to included"
      failures=$((failures + 1))
    fi
  done
  for got in inline plain; do
    if [[ "$(route_bucket "$tmp/$got.h")" == "included" ]]; then
      print_error "selftest: $got.h was treated as an include fragment"
      failures=$((failures + 1))
    fi
  done
  for got in static_data static_function_pointer; do
    if [[ "$(route_bucket "$tmp/$got.h")" == "included" ]]; then
      print_error "selftest: $got.h data object was treated as an include fragment"
      failures=$((failures + 1))
    fi
  done
  if ! python3 "$FIRMWARE_DIR/scripts/checks/tidy/static_decl_scan.py" --selftest >&2; then
    failures=$((failures + 1))
  fi
  if ! source_path_is_live "$tmp/live.c"; then
    print_error "selftest: a live source path was rejected"
    failures=$((failures + 1))
  fi
  if source_path_is_live "$tmp/missing.c"; then
    print_error "selftest: an absent cached source path was accepted"
    failures=$((failures + 1))
  fi
  if [[ "$(route_bucket "$tmp/vendor_api.c")" != "firmware" ]]; then
    print_error "selftest: a vendor-API source did not route to firmware"
    failures=$((failures + 1))
  fi
  if ! selftest_malformed_fragment "$tmp/malformed.h"; then
    failures=$((failures + 1))
  fi
  rm -rf -- "$tmp"

  failures=$((failures + $(selftest_fragment_registry)))

  # ...and the live floor: the real tree carries this class, so a rule that
  # stopped matching would report zero and go unnoticed.
  local live=0 f
  while IFS= read -r f; do
    [[ "$(route_bucket "$f")" == "included" ]] && live=$((live + 1))
  done < <(git -C "$FIRMWARE_DIR" ls-files --cached --others --exclude-standard \
    "tests/support/inc/*.h")
  if [[ "$live" -eq 0 ]]; then
    print_error "selftest: no tests/support header is recognised as an include fragment"
    failures=$((failures + 1))
  fi
  printf "%s\n" "$failures"
}

selftest_routing() {
  local failures=0
  local -a cases=(
    "$FIRMWARE_DIR/examples/x/y/src/main.c:firmware"
    "$FIRMWARE_DIR/port/usbx/src/a.c:firmware"
    "$FIRMWARE_DIR/port/posix/src/fw_if_fs_posix.c:host"
    "$FIRMWARE_DIR/examples/x/y/tests/src/test_y.c:host"
    "$FIRMWARE_DIR/apps/board/stand_alone/ereader/tests/src/test_y.c:host"
    "$FIRMWARE_DIR/apps/host/mdl/tests/src/test_y.c:tools"
    "$FIRMWARE_DIR/apps/shared_libs/epub/src/shim.cpp:cxx"
    "$FIRMWARE_DIR/libs/ra8_hal/src/k.cc:cxx"
    "$FIRMWARE_DIR/tools/ra8_emulator/src/display/board_view.m:objc"
    "$FIRMWARE_DIR/tools/ra8_emulator/src/x.c:tools"
    "$FIRMWARE_DIR/libs/ra8_core/src/ra8_err.c:host"
    "$FIRMWARE_DIR/libs/ra8_board_ra8p1/src/b.c:ra8p1"
  )
  local entry path want got
  for entry in ${cases[@]+"${cases[@]}"}; do
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
  for root in libs tests tools apps examples port; do
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
# gcc_integer_constant_macros must FAIL, never emit a partial success, when the
# compiler cannot report its __INTn_C / __UINTn_C builders. Both directions:
#
#   * negative -- a compiler that exists but defines nothing (fake with
#     `true`), and a compiler that is not on PATH, must both make it return
#     non-zero rather than an empty list that reads as success;
#   * positive -- a real cortex-m85-capable compiler must yield the full set,
#     and it must include `__UINT64_C`, the width whose absence aborted the
#     parse of every TU reaching ra8_c6link_mdl_transfer.h.
#
# The positive half is skipped (not failed) without the pinned toolchain, in
# step with selftest_arm_includes above.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_gcc_constant_macros() {
  local failures=0

  if ARM_CC=true gcc_integer_constant_macros >/dev/null 2>&1; then
    print_error "selftest: gcc_integer_constant_macros succeeded for a compiler defining none"
    failures=$((failures + 1))
  fi
  if ARM_CC="ra8-no-such-cc-$$" gcc_integer_constant_macros >/dev/null 2>&1; then
    print_error "selftest: gcc_integer_constant_macros succeeded for a missing compiler"
    failures=$((failures + 1))
  fi

  if arm-none-eabi-gcc -mcpu=cortex-m85 -E - </dev/null >/dev/null 2>&1; then
    local out=""
    if ! out="$(gcc_integer_constant_macros)"; then
      print_error "selftest: gcc_integer_constant_macros failed for a working cortex-m85 compiler"
      failures=$((failures + 1))
    elif ! grep -qF -- '-D__UINT64_C(c)=' <<<"$out"; then
      print_error "selftest: gcc_integer_constant_macros dropped __UINT64_C"
      failures=$((failures + 1))
    fi
  fi

  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# Bare firmware headers borrow a nearby compile-database command. Prove that
# the firmware pass always supplies the C23 language floor and that the exact
# argument makes a header using the standard `bool` / `true` keywords parse
# with clang-tidy. The C17 negative direction keeps the fixture sensitive: if
# the C23 argument is dropped, the original ra8_err.h parse regression returns.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_firmware_c23_mode() {
  local failures=0 language_arg mode clang_tidy tmp
  language_arg="$(firmware_language_args)"
  if [[ "$language_arg" != '--extra-arg-before=-std=c2x' ]]; then
    print_error "selftest: firmware language argument is '$language_arg', expected C23"
    failures=$((failures + 1))
    printf '%s\n' "$failures"
    return 0
  fi

  mode="${language_arg#--extra-arg-before=}"
  if ! clang_tidy="$(find_clang_tidy)"; then
    print_error "selftest: clang-tidy is required for the firmware C23 parse probe"
    failures=$((failures + 1))
    printf '%s\n' "$failures"
    return 0
  fi

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tidy-c23.XXXXXXXX")"
  printf '%s\n' 'static inline bool firmware_header_ok(void) { return true; }' >"$tmp/bare_bool.h"
  if ! "$clang_tidy" '--checks=-*,clang-analyzer-core.DivideZero' "$tmp/bare_bool.h" -- \
    -x c "$mode" >/dev/null 2>&1; then
    print_error "selftest: bare bool firmware header did not parse with $mode"
    failures=$((failures + 1))
  fi
  if "$clang_tidy" '--checks=-*,clang-analyzer-core.DivideZero' "$tmp/bare_bool.h" -- \
    -x c -std=c17 >/dev/null 2>&1; then
    print_error "selftest: bare bool fixture unexpectedly parsed as C17"
    failures=$((failures + 1))
  fi
  rm -rf -- "$tmp"
  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# Prove clang-tidy's included-header reporting itself in both directions with
# actual static-declaration fragments: the bad authored declaration must fire,
# its conforming neighbour must not, and a bad vendor declaration outside the
# probe filter must stay hidden. The registry census above separately proves
# that the shipped HeaderFilterRegex owns exactly the 24 live fragments.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
selftest_included_header_diagnostics() {
  local failures=0 clang_tidy tmp vendor_tmp log probe_filter
  if ! clang_tidy="$(find_clang_tidy)"; then
    print_error "selftest: clang-tidy is required for the included-header probe"
    printf '%s\n' 1
    return 0
  fi

  mkdir -p "$FIRMWARE_DIR/tests/fixtures/inc"
  tmp="$(mktemp -d "$FIRMWARE_DIR/tests/fixtures/inc/tidy-header.XXXXXXXX")"
  vendor_tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tidy-vendor.XXXXXXXX")"
  log="$tmp/tidy.log"
  probe_filter='(^|.*/)tidy-header\.[^/]*/(bad|good)\.h$'
  printf '%s\n' 'static void badlyNamedHeaderFunction(void) {}' >"$tmp/bad.h"
  printf '%s\n' 'static void well_named_header_function(void) {}' >"$tmp/good.h"
  printf '%s\n' 'static void badlyNamedVendorFunction(void) {}' >"$vendor_tmp/vendor_bad.h"
  printf '%s\n' '#include "bad.h"' '#include "good.h"' '#include "vendor_bad.h"' \
    'int main(void) { badlyNamedHeaderFunction(); well_named_header_function(); badlyNamedVendorFunction(); return 0; }' >"$tmp/main.c"

  "$clang_tidy" '--checks=-*,readability-identifier-naming' \
    --config-file="$FIRMWARE_DIR/.clang-tidy" --header-filter="$probe_filter" \
    "$tmp/main.c" -- -std=c2x -I"$vendor_tmp" >"$log" 2>&1 || true
  if ! grep -qF "$tmp/bad.h:1:" "$log"; then
    print_error "selftest: clang-tidy hid a bad included-fragment diagnostic"
    failures=$((failures + 1))
  fi
  if grep -qF "$tmp/good.h:" "$log"; then
    print_error "selftest: clang-tidy reported the conforming included fragment"
    failures=$((failures + 1))
  fi
  if grep -qF "$vendor_tmp/vendor_bad.h:" "$log"; then
    print_error "selftest: included-header probe admitted a vendor diagnostic"
    failures=$((failures + 1))
  fi
  rm -rf -- "$tmp" "$vendor_tmp"
  printf '%s\n' "$failures"
}

# ---------------------------------------------------------------------------
# Prove the pinned tidy major outranks a hostile bare link, explicit selection
# remains available, and every C/C++ driver reports the same major. The
# configure_fuzz_db probes bind those checks to the production caller so a
# selftest of a now-unused helper cannot stay green.
#
# Prints the number of failures.
# ---------------------------------------------------------------------------
write_fake_clang_driver() {
  local path="$1" family="$2" major="$3"
  printf '#!/bin/sh\nprintf "%s version %s.1.0\\n"\n' "$family" "$major" >"$path"
  chmod +x "$path"
}

write_fixed_major_probe() {
  local path="$1"
  printf '%s\n' \
    '#!/bin/sh' \
    'test "$#" -eq 1 || exit 2' \
    'printf "18\n"' >"$path"
  chmod +x "$path"
}

selftest_tidy_selection() {
  local tmp="$1" failures=0 found drivers
  if ! found="$(PATH="$tmp:/usr/bin:/bin" CLANG_TIDY="" find_clang_tidy)"; then
    print_error "selftest: pinned clang-tidy was not found"
    failures=$((failures + 1))
  elif [[ "$found" != "$RA8_PINNED_CLANG_TIDY" ]]; then
    print_error "selftest: default clang-tidy did not use the repository pin"
    failures=$((failures + 1))
  fi
  if ! found="$(PATH="$tmp:/usr/bin:/bin" CLANG_TIDY=clang-tidy-custom find_clang_tidy)"; then
    print_error "selftest: explicit major-18 clang-tidy override was rejected"
    failures=$((failures + 1))
  elif [[ "$found" != clang-tidy-custom ]]; then
    print_error "selftest: explicit clang-tidy override was not preserved"
    failures=$((failures + 1))
  elif ! drivers="$(PATH="$tmp:/usr/bin:/bin" matching_clang_drivers "$found")"; then
    print_error "selftest: explicit clang-tidy override rejected its exact compiler pair"
    failures=$((failures + 1))
  elif [[ "$drivers" != $'clang-18\nclang++-18' ]]; then
    print_error "selftest: explicit clang-tidy override selected the wrong compiler pair"
    failures=$((failures + 1))
  fi
  if PATH="$tmp:/usr/bin:/bin" CLANG_TIDY=clang-tidy \
    find_clang_tidy >/dev/null 2>&1; then
    print_error "selftest: explicit clang-tidy major 22 bypassed the repository pin"
    failures=$((failures + 1))
  fi
  if PATH="$tmp/hostile:$tmp:/usr/bin:/bin" CLANG_TIDY="" \
    find_clang_tidy >/dev/null 2>&1; then
    print_error "selftest: same-name hostile clang-tidy major 22 bypassed the repository pin"
    failures=$((failures + 1))
  fi
  if PATH="$tmp:/usr/bin:/bin" CLANG_TIDY=missing-tidy \
    find_clang_tidy >/dev/null; then
    print_error "selftest: an explicit missing clang-tidy was accepted"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_tidy_driver_pairs() {
  local tmp="$1" failures=0 drivers

  # MC/DC for `(cc_major != major || cxx_major != major)`:
  #   F,F -> accept; T,F -> reject C mismatch; F,T -> reject C++ mismatch.
  # The three vectors independently prove both conditions affect the decision.
  if ! drivers="$(PATH="$tmp:/usr/bin:/bin" matching_clang_drivers clang-tidy-18)"; then
    print_error "selftest: matching clang-18 compiler pair was rejected"
    failures=$((failures + 1))
  elif [[ "$drivers" != $'clang-18\nclang++-18' ]]; then
    print_error "selftest: clang-tidy-18 selected a mismatched compiler pair"
    failures=$((failures + 1))
  fi
  if PATH="$tmp/pair-cc-wrong:/usr/bin:/bin" \
    matching_clang_drivers clang-tidy-18 >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a C driver reporting the wrong major"
    failures=$((failures + 1))
  fi
  if PATH="$tmp/pair-cxx-wrong:/usr/bin:/bin" \
    matching_clang_drivers clang-tidy-18 >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a C++ driver reporting the wrong major"
    failures=$((failures + 1))
  fi
  if PATH="$tmp/pair-missing-cxx" \
    matching_clang_drivers clang-tidy-18 >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a major with no matching C++ driver"
    failures=$((failures + 1))
  fi
  if PATH="$tmp:/usr/bin:/bin" matching_clang_drivers clang-tidy-66 >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a compiler whose reported major mismatched"
    failures=$((failures + 1))
  fi
  if PATH="$tmp:/usr/bin:/bin" matching_clang_drivers clang-tidy >/dev/null 2>&1; then
    print_error "selftest: tidy major 22 accepted a matching but unpinned compiler pair"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_tidy_driver_availability() {
  local tmp="$1" failures=0 major_probe
  major_probe="$tmp/fixed-major-probe"

  # MC/DC for `(! command -v cc || ! command -v cxx)`:
  #   F,F continues; T,F rejects missing C; F,T rejects missing C++.
  # Inject the exercised fixed-major command into the production resolver so
  # each vector is load-bearing on this availability decision rather than
  # merely failing at a redundant later version probe.
  if PATH="$tmp/pair-missing-cc" \
    matching_clang_drivers clang-tidy-18 "$major_probe" >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a missing C driver"
    failures=$((failures + 1))
  fi
  if PATH="$tmp/pair-missing-cxx" \
    matching_clang_drivers clang-tidy-18 "$major_probe" >/dev/null 2>&1; then
    print_error "selftest: tidy accepted a missing C++ driver"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_tidy_production_binding() {
  local tmp="$1" failures=0 selected
  if ! selected="$({
    configure_fuzz_tree() {
      printf '%s\n%s\n' "${4:?}" "${5:?}"
    }
    fuzz_db_is_usable() {
      : "${1:?}"
      return 0
    }
    merge_compile_db() {
      : "${1:?}" "${2:?}"
    }
    configure_fuzz_tree unused unused unused clang-18 clang++-18
    fuzz_db_is_usable unused
    merge_compile_db unused unused
    FIRMWARE_DIR="$tmp/firmware" BUILD_DIR="$tmp/build" \
      PATH="$tmp:/usr/bin:/bin" configure_fuzz_db "$tmp/tests" /dev/null clang-tidy-18
  })"; then
    print_error "selftest: production fuzz configure rejected the exact compiler pair"
    failures=$((failures + 1))
  elif [[ "$selected" != $'clang-18\nclang++-18\nclang-18\nclang++-18' ]]; then
    print_error "selftest: production fuzz configure did not forward the resolved pair"
    failures=$((failures + 1))
  fi
  if (
    configure_fuzz_tree() {
      return 0
    }
    fuzz_db_is_usable() {
      return 0
    }
    merge_compile_db() {
      return 0
    }
    configure_fuzz_tree unused unused unused clang-18 clang++-18
    fuzz_db_is_usable unused
    merge_compile_db unused unused
    FIRMWARE_DIR="$tmp/firmware" BUILD_DIR="$tmp/build" \
      PATH="$tmp/pair-cxx-wrong:/usr/bin:/bin" \
      configure_fuzz_db "$tmp/tests" /dev/null clang-tidy-18
  ) >/dev/null 2>&1; then
    print_error "selftest: production fuzz configure bypassed the compiler-pair check"
    failures=$((failures + 1))
  fi
  printf '%s\n' "$failures"
}

selftest_tidy_tool_resolution() {
  local failures=0 tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tidy-tools.XXXXXXXX")"
  write_fake_clang_driver "$tmp/clang-tidy-18" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/clang-tidy" "Debian LLVM" 22
  write_fake_clang_driver "$tmp/clang-tidy-custom" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/clang-18" "Debian clang" 18
  cp "$tmp/clang-18" "$tmp/clang++-18"
  write_fake_clang_driver "$tmp/clang-22" "Debian clang" 22
  cp "$tmp/clang-22" "$tmp/clang++-22"
  write_fake_clang_driver "$tmp/clang-tidy-77" "Debian LLVM" 77
  write_fake_clang_driver "$tmp/clang-77" "Debian clang" 77
  write_fake_clang_driver "$tmp/clang-tidy-66" "Debian LLVM" 66
  write_fake_clang_driver "$tmp/clang-66" "Debian clang" 65
  write_fake_clang_driver "$tmp/clang++-66" "Debian clang" 66
  write_fixed_major_probe "$tmp/fixed-major-probe"
  mkdir "$tmp/hostile"
  write_fake_clang_driver "$tmp/hostile/clang-tidy-18" "Debian LLVM" 22
  mkdir \
    "$tmp/pair-cc-wrong" \
    "$tmp/pair-cxx-wrong" \
    "$tmp/pair-missing-cc" \
    "$tmp/pair-missing-cxx"
  write_fake_clang_driver "$tmp/pair-cc-wrong/clang-tidy-18" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/pair-cc-wrong/clang-18" "Debian clang" 17
  write_fake_clang_driver "$tmp/pair-cc-wrong/clang++-18" "Debian clang" 18
  write_fake_clang_driver "$tmp/pair-cxx-wrong/clang-tidy-18" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/pair-cxx-wrong/clang-18" "Debian clang" 18
  write_fake_clang_driver "$tmp/pair-cxx-wrong/clang++-18" "Debian clang" 17
  write_fake_clang_driver "$tmp/pair-missing-cc/clang-tidy-18" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/pair-missing-cc/clang++-18" "Debian clang" 18
  write_fake_clang_driver "$tmp/pair-missing-cxx/clang-tidy-18" "Debian LLVM" 18
  write_fake_clang_driver "$tmp/pair-missing-cxx/clang-18" "Debian clang" 18

  failures=$((failures + $(selftest_tidy_selection "$tmp")))
  failures=$((failures + $(selftest_tidy_driver_pairs "$tmp")))
  failures=$((failures + $(selftest_tidy_driver_availability "$tmp")))
  failures=$((failures + $(selftest_tidy_production_binding "$tmp")))
  rm -rf -- "$tmp"
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
  failures=$((failures + $(selftest_gcc_constant_macros)))
  failures=$((failures + $(selftest_firmware_c23_mode)))
  failures=$((failures + $(selftest_included_header_diagnostics)))
  failures=$((failures + $(selftest_tidy_tool_resolution)))

  if [[ "$failures" -ne 0 ]]; then
    print_error "clang_tidy.sh selftest FAILED with $failures problem(s)."
    return 1
  fi
  print_success "clang_tidy.sh selftest: scope and pass routing OK"
  return 0
}

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/misra/selftest.sh -- Behavioral proof for the MISRA audit driver.
#
# SOURCED, NEVER EXECUTED. scripts/checks/misra_check_inner.sh is the only
# entry point. These fixtures exercise the production driver functions in both
# directions without duplicating its analyzer or scope authorities.

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "misra/selftest.sh: sourced module; run scripts/checks/misra_check_inner.sh --selftest" >&2
  exit 2
fi

# Declare the values supplied by the sourcing driver as this module's explicit
# interface. A declaration without an initializer preserves the driver's value.
declare SCRIPT_DIR
declare RA8_MISRA_9_PATCH
declare -a RA8_MISRA_DUMP_ARGS
declare -a RA8_MISRA_ROOTS
declare -a RA8_MISRA_BUILD_DIRS
declare -a SUPPRESS_ARGS
declare -a DUMPS
declare RA8_MISRA_POSIX_MODEL_SOURCE
declare RA8_MISRA_POSIX_LIBRARY_NAME

# --- selftest --------------------------------------------------------------
# Both directions, against a throwaway tree, driving the SAME two functions the
# scan drives. One direction alone proves nothing here: an exclusion that
# matched everything would also produce a perfectly quiet, perfectly wrong
# audit, and that failure reads as a burn-down.
_ra8_misra_expect() {
  if [[ "$1" == "yes" ]]; then
    echo "  [ok] $2"
  else
    echo "  [FAIL] $2"
    RA8_MISRA_SELFTEST_FAILS=$((RA8_MISRA_SELFTEST_FAILS + 1))
  fi
}

_ra8_misra_expect_listed() {
  local listing="$1" wanted="$2"
  if grep -qxF "$wanted" "$listing"; then
    _ra8_misra_expect yes "excluded (build output): $wanted"
  else
    _ra8_misra_expect no "excluded (build output): $wanted"
  fi
}

_ra8_misra_expect_absent() {
  local listing="$1" wanted="$2"
  if grep -qxF "$wanted" "$listing"; then
    _ra8_misra_expect no "still scanned (source root): $wanted"
  else
    _ra8_misra_expect yes "still scanned (source root): $wanted"
  fi
}

# A dirty checkout: build output under every root that can produce one, plus
# source under the two paths a blanket */build/* glob would wrongly swallow.
_ra8_misra_selftest_fixture() {
  local tree="$1"
  mkdir -p \
    "$tree/tools/foo/build/CMakeFiles/3.28.3/CompilerIdC" \
    "$tree/apps/board/stand_alone/prod/build/CMakeFiles" \
    "$tree/examples/ek_ra8d2/hw_validated/smoke/blink/build" \
    "$tree/tests/build-cov" \
    "$tree/libs/ra8_x/build" \
    "$tree/libs/ra8_x/builders" \
    "$tree/libs/ra8_x/src" \
    "$tree/tools/foo/src"
  : >"$tree/tools/foo/build/CMakeFiles/3.28.3/CompilerIdC/CMakeCCompilerId.c"
  : >"$tree/libs/ra8_x/build/real.c"
  : >"$tree/libs/ra8_x/builders/real.c"
  : >"$tree/libs/ra8_x/src/real.c"
  : >"$tree/tools/foo/src/real.c"
}

_ra8_misra_selftest_roots() {
  (cd "$1" && ra8_misra_build_output_dirs libs port tools apps examples tests)
}

# Both directions over the dirty fixture, plus the non-vacuity floor: an
# enumerator that returned nothing would pass every must-stay-quiet assertion
# here on its own.
_ra8_misra_selftest_dirty() {
  local listing="$1"

  # MUST FIRE -- every root where build output can legitimately live.
  _ra8_misra_expect_listed "$listing" "tools/foo/build"
  _ra8_misra_expect_listed "$listing" "apps/board/stand_alone/prod/build"
  _ra8_misra_expect_listed "$listing" "examples/ek_ra8d2/hw_validated/smoke/blink/build"
  _ra8_misra_expect_listed "$listing" "tests/build-cov"

  # MUST STAY QUIET -- libs/ is not a build-tree root, so `build` there is an
  # ordinary directory that may hold real source; `builders/` is never one.
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/build"
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/builders"
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/src"
  _ra8_misra_expect_absent "$listing" "tools/foo/src"

  if [[ "$(wc -l <"$listing")" -ge 4 ]]; then
    _ra8_misra_expect yes "the dirty fixture yields a non-empty exclusion set"
  else
    _ra8_misra_expect no "the dirty fixture yields a non-empty exclusion set"
  fi
}

# The other end of the same property: a CLEAN tree must exclude NOTHING, so an
# exclusion that had quietly become blanket cannot pass as a quiet audit.
_ra8_misra_selftest_clean() {
  local tree="$1" listing="$2"
  mkdir -p "$tree/libs/ra8_x/src" "$tree/tools/foo/src"
  : >"$tree/libs/ra8_x/src/real.c"
  _ra8_misra_selftest_roots "$tree" >"$listing"
  if [[ ! -s "$listing" ]]; then
    _ra8_misra_expect yes "a clean tree excludes nothing"
  else
    _ra8_misra_expect no "a clean tree excludes nothing"
  fi
}

# The membership predicate the header path and the dump discovery are filtered
# with must agree with the enumeration, in both directions.
_ra8_misra_selftest_predicate() {
  local listing="$1" probe
  probe="tools/foo/build/CMakeFiles/3.28.3/CompilerIdC/CMakeCCompilerId.c"
  ra8_misra_load_build_dirs "$listing"
  if ra8_misra_is_build_output "$probe"; then
    _ra8_misra_expect yes "the membership predicate rejects a file inside a build tree"
  else
    _ra8_misra_expect no "the membership predicate rejects a file inside a build tree"
  fi
  if ra8_misra_is_build_output "libs/ra8_x/build/real.c"; then
    _ra8_misra_expect no "the membership predicate keeps source under a source root"
  else
    _ra8_misra_expect yes "the membership predicate keeps source under a source root"
  fi
}

_ra8_misra_selftest_initializer_fixture() {
  local fixture="$1"
  cat >"$fixture" <<'CEOF'
typedef struct {
  unsigned int first;
  unsigned int second;
} inner_t;

typedef struct {
  unsigned int tag;
  inner_t inner;
} outer_t;

static outer_t s_top = {};
static outer_t s_nested = {.inner = {}};
static outer_t s_designated = {.tag = 1U, .inner = {}};
static unsigned int s_array[2] = {};
static outer_t s_excess_empty = {{{}}};
static outer_t s_under_braced = {0U, 0U};
static outer_t s_duplicate = {.tag = 0U, .tag = 1U};

int main(void)
{
  return (int)(s_top.tag + s_nested.tag + s_designated.tag + s_array[0] +
               s_excess_empty.tag + s_under_braced.tag + s_duplicate.tag);
}
CEOF
}

_ra8_misra_selftest_initializer_findings() {
  local fixture="$1" raw="$2" empty_case empty_label empty_line
  local top_line nested_line designated_line array_line excess_line under_line duplicate_line
  top_line="$(grep -nF 's_top = {}' "$fixture" | cut -d: -f1)"
  nested_line="$(grep -nF 's_nested' "$fixture" | head -1 | cut -d: -f1)"
  designated_line="$(grep -nF 's_designated' "$fixture" | head -1 | cut -d: -f1)"
  array_line="$(grep -nF 's_array' "$fixture" | head -1 | cut -d: -f1)"
  excess_line="$(grep -nF 's_excess_empty' "$fixture" | head -1 | cut -d: -f1)"
  under_line="$(grep -nF 's_under_braced' "$fixture" | head -1 | cut -d: -f1)"
  duplicate_line="$(grep -nF 's_duplicate' "$fixture" | head -1 | cut -d: -f1)"
  for empty_case in \
    "$top_line:top-level" "$nested_line:nested" \
    "$designated_line:designated" "$array_line:array"; do
    empty_line="${empty_case%%:*}"
    empty_label="${empty_case#*:}"
    if grep -F "[$fixture:$empty_line]" "$raw" | grep -qF '[misra-c2012-9.2]'; then
      _ra8_misra_expect no "$empty_label C23 = {} stays quiet for Rule 9.2"
    else
      _ra8_misra_expect yes "$empty_label C23 = {} stays quiet for Rule 9.2"
    fi
    if grep -F "[$fixture:$empty_line]" "$raw" | grep -qF '[misra-c2012-9.4]'; then
      _ra8_misra_expect no "$empty_label C23 = {} stays quiet for Rule 9.4"
    else
      _ra8_misra_expect yes "$empty_label C23 = {} stays quiet for Rule 9.4"
    fi
  done
  if grep -F "[$fixture:$excess_line]" "$raw" | grep -qF '[misra-c2012-9.2]'; then
    _ra8_misra_expect yes "an excess empty brace level still produces Rule 9.2"
  else
    _ra8_misra_expect no "an excess empty brace level still produces Rule 9.2"
  fi
  if grep -F "[$fixture:$under_line]" "$raw" | grep -qF '[misra-c2012-9.2]'; then
    _ra8_misra_expect yes "an under-braced aggregate still produces Rule 9.2"
  else
    _ra8_misra_expect no "an under-braced aggregate still produces Rule 9.2"
  fi
  if grep -F "[$fixture:$duplicate_line]" "$raw" | grep -qF '[misra-c2012-9.4]'; then
    _ra8_misra_expect yes "a duplicate designated member still produces Rule 9.4"
  else
    _ra8_misra_expect no "a duplicate designated member still produces Rule 9.4"
  fi
}

_ra8_misra_selftest_addon_drift() {
  local tmp="$1" addon_dir="$2" posix_cfg="$3"
  local cfg_drift dependency_drift drift patch_drift
  drift="$tmp/addons-drift"
  mkdir -p "$drift"
  cp "$addon_dir/misra.py" "$addon_dir/misra_9.py" \
    "$addon_dir/cppcheckdata.py" "$drift/"
  printf '\n# selftest digest drift\n' >>"$drift/misra_9.py"
  if ra8_misra_stage_addons "$drift" "$tmp/drift-output" >/dev/null 2>&1; then
    _ra8_misra_expect no "addon digest drift fails closed"
  else
    _ra8_misra_expect yes "addon digest drift fails closed"
  fi
  dependency_drift="$tmp/addons-dependency-drift"
  mkdir -p "$dependency_drift"
  cp "$addon_dir/misra.py" "$addon_dir/misra_9.py" \
    "$addon_dir/cppcheckdata.py" "$dependency_drift/"
  printf '\n# selftest dependency drift\n' >>"$dependency_drift/cppcheckdata.py"
  if ra8_misra_stage_addons \
    "$dependency_drift" "$tmp/dependency-drift-output" >/dev/null 2>&1; then
    _ra8_misra_expect no "cppcheckdata.py digest drift fails closed"
  else
    _ra8_misra_expect yes "cppcheckdata.py digest drift fails closed"
  fi
  patch_drift="$tmp/patch-drift.patch"
  cp "$RA8_MISRA_9_PATCH" "$patch_drift"
  printf '\n# selftest patch drift\n' >>"$patch_drift"
  if ra8_misra_stage_addons \
    "$addon_dir" "$tmp/patch-drift-output" "$patch_drift" >/dev/null 2>&1; then
    _ra8_misra_expect no "compatibility-patch digest drift fails closed"
  else
    _ra8_misra_expect yes "compatibility-patch digest drift fails closed"
  fi
  cfg_drift="$tmp/posix-drift.cfg"
  cp "$posix_cfg" "$cfg_drift"
  printf '\n<!-- selftest model drift -->\n' >>"$cfg_drift"
  if ra8_misra_stage_addons \
    "$addon_dir" "$tmp/cfg-drift-output" "$RA8_MISRA_9_PATCH" \
    "$cfg_drift" >/dev/null 2>&1; then
    _ra8_misra_expect no "POSIX-library digest drift fails closed"
  else
    _ra8_misra_expect yes "POSIX-library digest drift fails closed"
  fi
}

_ra8_misra_selftest_addon() {
  local tmp="$1" addon_dir="$2" staged="$3" posix_cfg="$4" fixture raw
  fixture="$tmp/c23-empty-initializer.c"
  _ra8_misra_selftest_initializer_fixture "$fixture"
  if ra8_misra_refresh_dump \
    "$fixture" "$tmp/cppcheck.txt" "C23 initializer cppcheck probe" \
    cppcheck "${RA8_MISRA_DUMP_ARGS[@]}"; then
    _ra8_misra_expect yes "the C23 fixture produces a fresh common-argument dump"
  else
    _ra8_misra_expect no "the C23 fixture produces a fresh common-argument dump"
    return
  fi
  raw="$tmp/addon.txt"
  if ra8_misra_capture_addon \
    "$raw" "C23 initializer MISRA add-on probe" \
    env PYTHONPATH="$staged" python3 "$staged/misra.py" --quiet "$fixture.dump"; then
    _ra8_misra_expect yes "the staged C23 add-on completes with an expected status"
  else
    _ra8_misra_expect no "the staged C23 add-on completes with an expected status"
    return
  fi
  _ra8_misra_selftest_initializer_findings "$fixture" "$raw"
  _ra8_misra_selftest_addon_drift "$tmp" "$addon_dir" "$posix_cfg"
}

_ra8_misra_selftest_dump_args() {
  local arg has_fixed_exclusion has_parser_model has_suppressions
  has_fixed_exclusion=0
  has_parser_model=0
  has_suppressions=0
  for arg in "${RA8_MISRA_DUMP_ARGS[@]}"; do
    [[ "$arg" == "-ilibs/third_party" ]] && has_fixed_exclusion=1
    [[ "$arg" == "--std=c11" ]] && has_parser_model=1
    [[ "$arg" == --suppress=* ]] && has_suppressions=1
  done
  if [[ "$has_fixed_exclusion" -eq 1 ]]; then
    _ra8_misra_expect yes "the common dump authority carries third-party exclusions"
  else
    _ra8_misra_expect no "the common dump authority carries third-party exclusions"
  fi
  if [[ "$has_parser_model" -eq 1 ]]; then
    _ra8_misra_expect yes "the common dump authority carries the parser model"
  else
    _ra8_misra_expect no "the common dump authority carries the parser model"
  fi
  if [[ "$has_suppressions" -eq 1 && ${#SUPPRESS_ARGS[@]} -gt 0 ]]; then
    _ra8_misra_expect yes "the common dump authority carries repository suppressions"
  else
    _ra8_misra_expect no "the common dump authority carries repository suppressions"
  fi
}

_ra8_misra_selftest_posix_other_diagnostics() {
  local tmp="$1" source="$2"
  awk '/\[misra-c2012-[0-9]+[.][0-9]+\] *$/ &&
    $0 !~ /\[misra-c2012-17[.]3\] *$/ { print }' \
    "$tmp/bare-posix-misra.txt" | sort -u >"$tmp/bare-posix-non173.txt"
  awk '/\[misra-c2012-[0-9]+[.][0-9]+\] *$/ &&
    $0 !~ /\[misra-c2012-17[.]3\] *$/ { print }' \
    "$tmp/modeled-posix-misra.txt" | sort -u >"$tmp/modeled-posix-non173.txt"
  if cmp -s "$tmp/bare-posix-non173.txt" "$tmp/modeled-posix-non173.txt"; then
    _ra8_misra_expect yes "the POSIX model changes no focused diagnostic except Rule 17.3"
  else
    _ra8_misra_expect no "the POSIX model changes no focused diagnostic except Rule 17.3"
  fi
  if ra8_misra_remove_dump_artifacts "$source"; then
    _ra8_misra_expect yes "the POSIX probe artifacts are removable"
  else
    _ra8_misra_expect no "the POSIX probe artifacts are removable"
  fi
}

_ra8_misra_selftest_posix_model() {
  local tmp="$1" staged="$2" source mode output
  local bare_count modeled_count modeled_reserved_count
  local -a library_args
  source="$RA8_MISRA_POSIX_MODEL_SOURCE"
  bare_count=0
  modeled_count=0
  modeled_reserved_count=0
  for mode in bare modeled; do
    library_args=()
    if [[ "$mode" == modeled ]]; then
      library_args=("--library=$staged/$RA8_MISRA_POSIX_LIBRARY_NAME")
    fi
    if ra8_misra_refresh_dump \
      "$source" "$tmp/$mode-posix-cppcheck.txt" "$mode POSIX cppcheck probe" \
      cppcheck "${RA8_MISRA_DUMP_ARGS[@]}" "${library_args[@]}"; then
      _ra8_misra_expect yes "$mode POSIX probe produces a fresh common-argument dump"
    else
      _ra8_misra_expect no "$mode POSIX probe produces a fresh common-argument dump"
      continue
    fi
    if ra8_misra_capture_addon \
      "$tmp/$mode-posix-misra.txt" "$mode POSIX MISRA add-on probe" \
      env PYTHONPATH="$staged" python3 "$staged/misra.py" --quiet "$source.dump"; then
      _ra8_misra_expect yes "$mode POSIX staged add-on status is expected"
    else
      _ra8_misra_expect no "$mode POSIX staged add-on status is expected"
      continue
    fi
    output="$(awk '/\[misra-c2012-17[.]3\] *$/ { count++ }
      END { print count + 0 }' "$tmp/$mode-posix-misra.txt")"
    if [[ "$mode" == bare ]]; then
      bare_count="$output"
    else
      modeled_count="$output"
      modeled_reserved_count="$(awk '/\[misra-c2012-21[.]1\] *$/ { count++ }
        END { print count + 0 }' "$tmp/$mode-posix-misra.txt")"
    fi
  done
  if [[ "$bare_count" -eq 4 ]]; then
    _ra8_misra_expect yes "the bare POSIX probe exposes exactly four implicit declarations"
  else
    _ra8_misra_expect no "the bare POSIX probe exposes exactly four implicit declarations"
  fi
  if [[ "$modeled_count" -eq 0 ]]; then
    _ra8_misra_expect yes "the staged POSIX model resolves all four declarations"
  else
    _ra8_misra_expect no "the staged POSIX model resolves all four declarations"
  fi
  if [[ "$modeled_reserved_count" -eq 1 ]]; then
    _ra8_misra_expect yes "the modeled probe retains the stable Rule 21.1 finding"
  else
    _ra8_misra_expect no "the modeled probe retains the stable Rule 21.1 finding"
  fi
  _ra8_misra_selftest_posix_other_diagnostics "$tmp" "$source"
}

_ra8_misra_selftest_dump_fail_closed() {
  local source="$1" log="$2" marker="$3" symlink_command="$4" target="$5"
  mkdir "$source.dump"
  if ra8_misra_refresh_dump "$source" "$log" "unremovable probe" \
    touch "$marker"; then
    _ra8_misra_expect no "an unremovable stale dump is rejected"
  else
    _ra8_misra_expect yes "an unremovable stale dump is rejected"
  fi
  if [[ ! -e "$marker" ]]; then
    _ra8_misra_expect yes "the analyzer is not run after stale-dump removal fails"
  else
    _ra8_misra_expect no "the analyzer is not run after stale-dump removal fails"
  fi
  rmdir "$source.dump"
  : >"$source.dump"
  if ra8_misra_refresh_dump "$source" "$log" "injected failure probe" \
    sh -c 'exit 23'; then
    _ra8_misra_expect no "an analyzer command failure is rejected"
  else
    _ra8_misra_expect yes "an analyzer command failure is rejected"
  fi
  if [[ ! -e "$source.dump" ]]; then
    _ra8_misra_expect yes "a failed analyzer cannot reuse a stale dump"
  else
    _ra8_misra_expect no "a failed analyzer cannot reuse a stale dump"
  fi
  if ra8_misra_refresh_dump "$source" "$log" "zero-without-dump probe" true; then
    _ra8_misra_expect no "status zero without a dump is rejected"
  else
    _ra8_misra_expect yes "status zero without a dump is rejected"
  fi
  if ra8_misra_refresh_dump "$source" "$log" "symlinked-dump probe" \
    "$symlink_command" "$target"; then
    _ra8_misra_expect no "a symlinked dump is rejected"
  else
    _ra8_misra_expect yes "a symlinked dump is rejected"
  fi
  if ra8_misra_remove_dump_artifacts "$source"; then
    _ra8_misra_expect yes "the rejected symlink artifact is removable"
  else
    _ra8_misra_expect no "the rejected symlink artifact is removable"
  fi
}

_ra8_misra_selftest_addon_fail_closed() {
  local log="$1" traceback_command="$2"
  if ra8_misra_capture_addon "$log" "injected add-on probe" sh -c 'exit 23'; then
    _ra8_misra_expect no "an unexpected MISRA add-on status is rejected"
  else
    _ra8_misra_expect yes "an unexpected MISRA add-on status is rejected"
  fi
  if ra8_misra_capture_addon "$log" "traceback add-on probe" \
    "$traceback_command"; then
    _ra8_misra_expect no "a traceback with status one is rejected"
  else
    _ra8_misra_expect yes "a traceback with status one is rejected"
  fi
}

_ra8_misra_selftest_fail_closed() {
  local tmp="$1" log marker source symlink_command target traceback_command
  source="$tmp/fail-closed.c"
  log="$tmp/fail-closed-errors.txt"
  marker="$tmp/command-ran"
  target="$tmp/symlink-target.dump"
  symlink_command="$tmp/make-symlink-dump"
  traceback_command="$tmp/emit-traceback"
  : >"$source"
  : >"$target"
  cat >"$symlink_command" <<'SH'
#!/bin/sh
ln -s "$1" "$2.dump"
SH
  cat >"$traceback_command" <<'SH'
#!/bin/sh
printf 'Traceback (most recent call last):\n  selftest\n'
exit 1
SH
  chmod 700 "$symlink_command" "$traceback_command"
  _ra8_misra_selftest_dump_fail_closed \
    "$source" "$log" "$marker" "$symlink_command" "$target"
  _ra8_misra_selftest_addon_fail_closed "$log" "$traceback_command"
}

_ra8_misra_selftest_staged_copy_integrity() {
  local tmp="$1" addon_dir="$2" posix_cfg="$3" wrapper target
  wrapper="$tmp/mutating-copy-bin"
  mkdir -p "$wrapper"
  cat >"$wrapper/cp" <<'SH'
#!/bin/sh
/bin/cp "$@" || exit $?
last=""
for arg do
  last="$arg"
done
case "${RA8_MISRA_MUTATE_STAGED:-}" in
  misra.py | cppcheckdata.py)
    if [ -d "$last" ] && [ -f "$last/$RA8_MISRA_MUTATE_STAGED" ]; then
      printf '\n# injected staged-copy mutation\n' >>"$last/$RA8_MISRA_MUTATE_STAGED"
    fi
    ;;
  patch)
    case "$last" in
      *misra_9-c23-empty-initializer.patch)
        printf '\n# injected staged-patch mutation\n' >>"$last"
        ;;
    esac
    ;;
esac
SH
  chmod 700 "$wrapper/cp"

  for target in misra.py cppcheckdata.py patch; do
    export RA8_MISRA_MUTATE_STAGED="$target"
    if PATH="$wrapper:$PATH" ra8_misra_stage_addons \
      "$addon_dir" "$tmp/stage-mutation-$target" \
      "$RA8_MISRA_9_PATCH" "$posix_cfg" >/dev/null 2>&1; then
      _ra8_misra_expect no "$target staged-copy substitution fails closed"
    else
      _ra8_misra_expect yes "$target staged-copy substitution fails closed"
    fi
  done
  unset RA8_MISRA_MUTATE_STAGED
}

_ra8_misra_selftest_dump_inventory() {
  local tmp="$1" root
  local -a saved_build_dirs saved_roots
  saved_roots=("${RA8_MISRA_ROOTS[@]}")
  saved_build_dirs=("${RA8_MISRA_BUILD_DIRS[@]}")
  root="$tmp/inventory-root"
  mkdir -p "$root"
  : >"$root/a.c"
  : >"$root/b.cpp"
  RA8_MISRA_ROOTS=("$root")
  RA8_MISRA_BUILD_DIRS=()

  : >"$root/a.c.dump"
  : >"$root/b.cpp.dump"
  if ra8_misra_collect_dump_inventory && [[ ${#DUMPS[@]} -eq 2 ]]; then
    _ra8_misra_expect yes "an exact regular dump inventory is accepted"
  else
    _ra8_misra_expect no "an exact regular dump inventory is accepted"
  fi

  rm -f -- "$root/b.cpp.dump"
  if ra8_misra_collect_dump_inventory >/dev/null 2>&1; then
    _ra8_misra_expect no "a missing translation-unit dump is rejected"
  else
    _ra8_misra_expect yes "a missing translation-unit dump is rejected"
  fi

  : >"$root/b.cpp.dump"
  : >"$root/orphan.c.dump"
  if ra8_misra_collect_dump_inventory >/dev/null 2>&1; then
    _ra8_misra_expect no "an orphan analyzer dump is rejected"
  else
    _ra8_misra_expect yes "an orphan analyzer dump is rejected"
  fi

  rm -f -- "$root/orphan.c.dump" "$root/b.cpp.dump"
  ln -s "$root/a.c.dump" "$root/b.cpp.dump"
  if ra8_misra_collect_dump_inventory >/dev/null 2>&1; then
    _ra8_misra_expect no "a substituted symlink dump is rejected"
  else
    _ra8_misra_expect yes "a substituted symlink dump is rejected"
  fi
  if ra8_misra_remove_all_dump_artifacts &&
    [[ ! -e "$root/a.c.dump" && ! -L "$root/b.cpp.dump" ]]; then
    _ra8_misra_expect yes "pre-scan cleanup removes regular and symlink residue"
  else
    _ra8_misra_expect no "pre-scan cleanup removes regular and symlink residue"
  fi

  mkdir "$root/a.c.dump"
  if ra8_misra_remove_all_dump_artifacts >/dev/null 2>&1; then
    _ra8_misra_expect no "unremovable whole-tree residue is rejected"
  else
    _ra8_misra_expect yes "unremovable whole-tree residue is rejected"
  fi
  rmdir "$root/a.c.dump"
  RA8_MISRA_ROOTS=("${saved_roots[@]}")
  RA8_MISRA_BUILD_DIRS=("${saved_build_dirs[@]}")
}

_ra8_misra_selftest_parser_diagnostics() {
  local tmp="$1" log
  log="$tmp/parser-diagnostics.txt"
  : >"$log"
  if ra8_misra_reject_parse_failures "$log"; then
    _ra8_misra_expect yes "a parser-clean cppcheck log is accepted"
  else
    _ra8_misra_expect no "a parser-clean cppcheck log is accepted"
  fi
  printf '%s\n' 'fixture.c:1:1: error: broken AST [internalAstError]' >"$log"
  if ra8_misra_reject_parse_failures "$log" >/dev/null 2>&1; then
    _ra8_misra_expect no "a zero-status parser failure is rejected"
  else
    _ra8_misra_expect yes "a zero-status parser failure is rejected"
  fi
  if ra8_misra_reject_parse_failures "$tmp/missing-log" >/dev/null 2>&1; then
    _ra8_misra_expect no "an unreadable analyzer log is rejected"
  else
    _ra8_misra_expect yes "an unreadable analyzer log is rejected"
  fi
}

_ra8_misra_selftest_source_only() {
  local log="$1/direct-execution.txt"
  if bash "$SCRIPT_DIR/misra/selftest.sh" --selftest >"$log" 2>&1; then
    _ra8_misra_expect no "the sourced selftest module rejects direct execution"
  elif grep -qF \
    "misra/selftest.sh: sourced module; run scripts/checks/misra_check_inner.sh --selftest" \
    "$log"; then
    _ra8_misra_expect yes "the sourced selftest module rejects direct execution"
  else
    _ra8_misra_expect no "the sourced selftest module rejects direct execution"
  fi
}

ra8_misra_selftest() {
  local addon_dir listing posix_cfg staged tmp
  RA8_MISRA_SELFTEST_FAILS=0
  echo "misra_check_inner.sh --selftest"
  tmp="$(mktemp -d)"
  listing="$tmp/excluded.txt"

  _ra8_misra_selftest_fixture "$tmp/tree"
  _ra8_misra_selftest_roots "$tmp/tree" >"$listing"
  _ra8_misra_selftest_dirty "$listing"
  _ra8_misra_selftest_clean "$tmp/clean" "$tmp/clean.txt"
  _ra8_misra_selftest_predicate "$listing"
  if ra8_misra_prepare_scan_arguments; then
    _ra8_misra_expect yes "the real common dump arguments are prepared"
    _ra8_misra_selftest_dump_args
  else
    _ra8_misra_expect no "the real common dump arguments are prepared"
  fi
  if addon_dir="$(ra8_misra_find_addon_dir)"; then
    _ra8_misra_expect yes "the pinned cppcheck addons are discoverable"
  else
    _ra8_misra_expect no "the pinned cppcheck addons are discoverable"
    addon_dir=""
  fi
  if posix_cfg="$(ra8_misra_find_posix_cfg)"; then
    _ra8_misra_expect yes "the pinned cppcheck POSIX model is discoverable"
  else
    _ra8_misra_expect no "the pinned cppcheck POSIX model is discoverable"
    posix_cfg=""
  fi
  staged="$tmp/addons-staged"
  if [[ -n "$addon_dir" && -n "$posix_cfg" ]] &&
    ra8_misra_stage_addons "$addon_dir" "$staged" "$RA8_MISRA_9_PATCH" "$posix_cfg"; then
    _ra8_misra_expect yes "the exact cppcheck addons and POSIX model are staged"
    _ra8_misra_selftest_addon "$tmp" "$addon_dir" "$staged" "$posix_cfg"
    _ra8_misra_selftest_posix_model "$tmp" "$staged"
    _ra8_misra_selftest_staged_copy_integrity "$tmp" "$addon_dir" "$posix_cfg"
  else
    _ra8_misra_expect no "the exact cppcheck addons and POSIX model are staged"
  fi
  _ra8_misra_selftest_fail_closed "$tmp"
  _ra8_misra_selftest_dump_inventory "$tmp"
  _ra8_misra_selftest_parser_diagnostics "$tmp"
  _ra8_misra_selftest_source_only "$tmp"

  rm -rf "$tmp"
  if [[ "$RA8_MISRA_SELFTEST_FAILS" -ne 0 ]]; then
    echo "SELFTEST FAILED: $RA8_MISRA_SELFTEST_FAILS assertion(s)" >&2
    return 1
  fi
  echo "selftest: all assertions held (both directions)."
  return 0
}

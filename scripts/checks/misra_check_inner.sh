#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware -- MISRA-C 2012 audit (advisory)
#
# Runs cppcheck + the bundled misra.py addon over every first-party C source
# root (libs/ + src/ + port/ + tools/ + apps/, excluding vendored/generated
# code) and emits:
#
#   build/misra/results.txt  -- one violation per line, parsed
#   build/misra/raw.txt      -- raw cppcheck stderr
#   build/misra/misra-raw.txt-- raw misra.py stdout
#   stdout                   -- per-rule tally
#
# This is the audit hook for IEC 61508 SIL 3 / DO-178C MISRA-C 2012
# compliance. cppcheck-MISRA covers roughly two thirds of the
# mandatory + required rules; see docs/MISRA.md for the gap plan.
#
# Out of scope: tests/, examples/, mocks/, and any BUILD TREE beneath a root
# that produces one (see the build-output section below -- the exclusion is
# derived from lint_targets.is_build_output, never from a */build/* glob).
# Host tools are first-party production code and deliberately remain in scope.
#
# Usage: misra_check_inner.sh [--selftest]
#
# Strategy: cppcheck's `--addon=misra` dispatcher silently drops
# misra.py findings when no MISRA rule-texts file is supplied (the
# rule texts are copyrighted by MISRA Ltd and cannot be redistributed).
# To keep this audit working out of the box we instead generate
# `--dump` files with cppcheck and run misra.py directly on them --
# the rule IDs (e.g. [misra-c2012-15.5]) are emitted regardless.
#

set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328); cppcheck's
# -j below derives from it instead of raw nproc.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

cd "$ROOT_DIR"

OUT_DIR="$ROOT_DIR/build/misra"
mkdir -p "$OUT_DIR"
RAW="$OUT_DIR/raw.txt"
MISRA_RAW="$OUT_DIR/misra-raw.txt"
RESULTS="$OUT_DIR/results.txt"

# The first-party source roots this audit covers. Everything downstream --
# the header search path, the cppcheck scan list, the dump discovery and the
# cleanup trap -- is derived from this ONE array, so a root cannot be added
# to some of those four and silently omitted from the others.
#
# It already was. When apps/ joined the scan list the header path beneath it
# was still spelled as a hand-written glob naming only tools/*/inc, so every
# header under apps/ became unreachable: cppcheck charged each caller MISRA
# 17.3 for the resulting implicit declarations and each definition 8.4 for
# the declaration it could no longer see, while SILENTLY LOSING the 9.2,
# 11.x, 18.4, 20.1 and 21.15 findings it can only raise once the types in
# those headers are known. A net drop in findings is the dangerous direction
# of that failure -- it reads as a burn-down.
RA8_MISRA_ROOTS=(libs port tools apps)

# cppcheck 2.13's bundled Rule 9 helper predates C23 empty initializers. Keep
# the installed tool immutable: verify its exact bytes, copy its addons into a
# disposable directory, apply the reviewed one-predicate compatibility patch,
# and verify the resulting bytes before the audit can use them.
RA8_CPPCHECK_VERSION="Cppcheck 2.13.0"
RA8_MISRA_PY_SHA256="0a44b511fe5a27b43d41dba1178044b03619cbfc96d51cc4c4fe4b23d3c0ac99"
RA8_MISRA_9_SHA256="acda74759305d78dffc5fe0340c1bab5b5633072590f1a7d5547defa2fbf299b"
RA8_CPPCHECKDATA_PY_SHA256="3c6929ad7476de139b18cf2b06812e052a272a6571e9050b116c82125adc6e3d"
RA8_POSIX_CFG_SHA256="12f1f5c65b6a2220419f2c9930ea8f1d955fc3733df1ab35ad4f0a7d57b1ba17"
RA8_MISRA_9_PATCHED_SHA256="9befd72ac9ed19386f8006c5afdbf2cf84d8e304e61692118568176bdde561fe"
RA8_MISRA_9_PATCH_SHA256="ce2e53b7aef6ae906c6e507ad261258546c3645a02ccd3106c21f80ce22f9060"
RA8_MISRA_9_PATCH="$SCRIPT_DIR/patches/cppcheck-2.13/misra_9-c23-empty-initializer.patch"
RA8_MISRA_POSIX_MODEL_SOURCE="port/posix/src/fw_if_fs_posix_common.c"
RA8_MISRA_POSIX_LIBRARY_NAME="posix.cfg"

ra8_misra_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "[ERROR] sha256sum or shasum is required" >&2
    return 1
  fi
}

ra8_misra_find_addon_dir() {
  local candidate
  for candidate in /opt/homebrew/share/Cppcheck/addons /usr/share/cppcheck/addons \
    /usr/local/share/Cppcheck/addons /usr/lib/*-linux-gnu/cppcheck/addons; do
    if [[ -d "$candidate" && -f "$candidate/misra.py" && -f "$candidate/misra_9.py" &&
      -f "$candidate/cppcheckdata.py" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  echo "[ERROR] cppcheck MISRA addons not found" >&2
  return 1
}

ra8_misra_find_posix_cfg() {
  local candidate
  for candidate in /opt/homebrew/share/Cppcheck/cfg/posix.cfg \
    /usr/share/cppcheck/cfg/posix.cfg \
    /usr/local/share/Cppcheck/cfg/posix.cfg \
    /usr/lib/*-linux-gnu/cppcheck/cfg/posix.cfg; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  echo "[ERROR] cppcheck POSIX library model not found" >&2
  return 1
}

ra8_misra_expect_sha256() {
  local path="$1" expected="$2" label="$3" actual
  actual="$(ra8_misra_sha256 "$path")" || return 1
  if [[ "$actual" != "$expected" ]]; then
    echo "[ERROR] $label digest drift: expected $expected, got $actual" >&2
    return 1
  fi
}

ra8_misra_verify_stage_inputs() {
  local source_dir="$1" patch_path="$2" posix_cfg="$3" version
  [[ -f "$posix_cfg" ]] || {
    echo "[ERROR] missing cppcheck POSIX library model: $posix_cfg" >&2
    return 1
  }
  version="$(cppcheck --version)"
  if [[ "$version" != "$RA8_CPPCHECK_VERSION" ]]; then
    echo "[ERROR] cppcheck version drift: expected $RA8_CPPCHECK_VERSION, got $version" >&2
    return 1
  fi
  [[ -f "$patch_path" ]] || {
    echo "[ERROR] missing cppcheck compatibility patch: $patch_path" >&2
    return 1
  }
  ra8_misra_expect_sha256 \
    "$source_dir/misra.py" "$RA8_MISRA_PY_SHA256" "upstream misra.py" || return 1
  ra8_misra_expect_sha256 \
    "$source_dir/misra_9.py" "$RA8_MISRA_9_SHA256" "upstream misra_9.py" || return 1
  ra8_misra_expect_sha256 \
    "$source_dir/cppcheckdata.py" "$RA8_CPPCHECKDATA_PY_SHA256" \
    "upstream cppcheckdata.py" || return 1
  ra8_misra_expect_sha256 \
    "$patch_path" "$RA8_MISRA_9_PATCH_SHA256" "compatibility patch" || return 1
  ra8_misra_expect_sha256 \
    "$posix_cfg" "$RA8_POSIX_CFG_SHA256" "upstream posix.cfg" || return 1
  command -v patch >/dev/null 2>&1 || {
    echo "[ERROR] patch is required for the cppcheck compatibility correction" >&2
    return 1
  }
}

ra8_misra_verify_staged_inputs() {
  local destination="$1" staged_patch="$2"
  ra8_misra_expect_sha256 \
    "$destination/misra.py" "$RA8_MISRA_PY_SHA256" \
    "staged misra.py" || return 1
  ra8_misra_expect_sha256 \
    "$destination/misra_9.py" "$RA8_MISRA_9_SHA256" \
    "staged upstream misra_9.py" || return 1
  ra8_misra_expect_sha256 \
    "$destination/cppcheckdata.py" "$RA8_CPPCHECKDATA_PY_SHA256" \
    "staged cppcheckdata.py" || return 1
  ra8_misra_expect_sha256 \
    "$destination/$RA8_MISRA_POSIX_LIBRARY_NAME" "$RA8_POSIX_CFG_SHA256" \
    "staged posix.cfg" || return 1
  ra8_misra_expect_sha256 \
    "$staged_patch" "$RA8_MISRA_9_PATCH_SHA256" \
    "staged compatibility patch" || return 1
}

ra8_misra_apply_staged_patch() {
  local destination="$1" staged_patch="$2"
  if ! patch -d "$destination" -p1 --batch --forward <"$staged_patch" >/dev/null; then
    echo "[ERROR] cppcheck C23 compatibility patch did not apply" >&2
    return 1
  fi
  rm -f -- "$staged_patch" || return 1
  if [[ -e "$staged_patch" || -L "$staged_patch" ]]; then
    echo "[ERROR] staged compatibility patch survived removal" >&2
    return 1
  fi
}

ra8_misra_verify_patched_addons() {
  local destination="$1"
  ra8_misra_expect_sha256 \
    "$destination/misra.py" "$RA8_MISRA_PY_SHA256" \
    "staged misra.py after patching" || return 1
  ra8_misra_expect_sha256 \
    "$destination/misra_9.py" "$RA8_MISRA_9_PATCHED_SHA256" \
    "patched misra_9.py" || return 1
  ra8_misra_expect_sha256 \
    "$destination/cppcheckdata.py" "$RA8_CPPCHECKDATA_PY_SHA256" \
    "staged cppcheckdata.py after patching" || return 1
  ra8_misra_expect_sha256 \
    "$destination/$RA8_MISRA_POSIX_LIBRARY_NAME" "$RA8_POSIX_CFG_SHA256" \
    "staged posix.cfg after patching" || return 1
}

ra8_misra_stage_addons() {
  local source_dir="$1" destination="$2" patch_path="${3:-$RA8_MISRA_9_PATCH}"
  local posix_cfg="${4:-}" staged_patch
  if [[ -z "$posix_cfg" ]]; then
    posix_cfg="$(ra8_misra_find_posix_cfg)" || return 1
  fi
  ra8_misra_verify_stage_inputs "$source_dir" "$patch_path" "$posix_cfg" || return 1

  mkdir -p "$destination" || return 1
  staged_patch="$destination/misra_9-c23-empty-initializer.patch"
  cp "$source_dir/misra.py" "$source_dir/misra_9.py" \
    "$source_dir/cppcheckdata.py" "$destination/" || return 1
  cp "$posix_cfg" "$destination/$RA8_MISRA_POSIX_LIBRARY_NAME" || return 1
  cp "$patch_path" "$staged_patch" || return 1

  ra8_misra_verify_staged_inputs "$destination" "$staged_patch" || return 1
  ra8_misra_apply_staged_patch "$destination" "$staged_patch" || return 1
  ra8_misra_verify_patched_addons "$destination" || return 1
}

# ---------------------------------------------------------------------------
# BUILD OUTPUT IS NOT SOURCE -- and `build/` does not mean the same thing
# everywhere in this tree.
#
# The scan used to hand cppcheck the bare root directories with no build
# exclusion at all, so whatever an untracked build tree happened to leave under
# a scanned root was audited as first-party source. Regenerating the baseline on
# a dirty checkout therefore FROZE that junk into the ratchet: 12 rows of CMake
# compiler-probe source (tools/*/build/CMakeFiles/.../CMakeCCompilerId.c) had to
# be purged from .github/misra-baseline.txt by hand. Junk in the baseline is
# worse than junk in a report -- the ratchet then measures the tree against a
# population that never existed, and a real finding can hide behind a probe row.
#
# A blanket `*/build/*` exclusion is the WRONG fix, and lint_targets.py already
# says why. `build` names build OUTPUT only beneath the roots that produce it
# (tools/<t>/build, apps/<cat>/<p>/build, examples/**/<app>/build, tests/build,
# docs/build, port/**). Under libs/ it is an ordinary directory
# name a module is entitled to use for real source, and `builders/` is not a
# build tree at all despite the prefix. A glob cannot tell those apart, and a
# glob that swallows source is the dangerous direction: the audit goes quieter
# and the burn-down looks like progress.
#
# So the verdict is not re-derived here. This asks lint_targets.is_build_output
# -- the ONE definition of "this is build output" in the tree, shared with
# .gitignore and thirteen other checkers -- and turns its answer into cppcheck
# -i flags plus a filter for the header path and the dump discovery. A second
# description of the same fact is exactly how `build/` came to mean two things.
#
# The alternative considered and rejected: scanning only `git ls-files`. It
# would have fixed this case, since build trees are gitignored, but by a rule
# that is blanket in a different disguise -- it would drop every ignored path,
# not just build output -- and it costs real recall. A brand-new .c that has not
# been `git add`ed yet would silently leave the audit, and a baseline
# regenerated in that state would be missing a whole file. It would also mean
# re-implementing cppcheck's own source-file selection in bash, so the scanned
# POPULATION could drift from what cppcheck picks up walking a directory.
# Excluding the OUTPUT keeps the population exactly as it was and changes only
# what was never source.
# ---------------------------------------------------------------------------

# Print every build-output directory beneath the roots named as arguments.
# Build trees are pruned rather than descended, so a nested one is reported once
# by its outermost directory.
ra8_misra_build_output_dirs() {
  RA8_MISRA_CHECKS_DIR="$SCRIPT_DIR" python3 - "$@" <<'PYEOF'
import os
import sys

sys.path.insert(0, os.environ["RA8_MISRA_CHECKS_DIR"])

from lint_targets import is_build_output

for root in sys.argv[1:]:
    for dirpath, dirnames, _filenames in os.walk(root):
        descend = []
        for name in sorted(dirnames):
            candidate = os.path.join(dirpath, name)
            # is_build_output classifies a FILE path -- its last component is
            # the file name and is never treated as a directory. Probe with a
            # dummy leaf so the directory itself is what gets judged.
            if is_build_output(candidate + "/probe"):
                print(candidate)
            else:
                descend.append(name)
        dirnames[:] = descend
PYEOF
}

# True when a path lies inside one of the enumerated build trees. Used to filter
# the header path and the dump discovery with the same verdict the scan uses.
ra8_misra_is_build_output() {
  local candidate="$1" excluded
  for excluded in ${RA8_MISRA_BUILD_DIRS[@]+${RA8_MISRA_BUILD_DIRS[@]+"${RA8_MISRA_BUILD_DIRS[@]}"}}; do
    if [[ "$candidate" == "$excluded" || "$candidate" == "$excluded"/* ]]; then
      return 0
    fi
  done
  return 1
}

ra8_misra_load_build_dirs() {
  local listing="$1"
  mapfile -t RA8_MISRA_BUILD_DIRS <"$listing"
}

ra8_misra_header_dirs() {
  find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} -type d -name inc \
    -not -path '*/third_party/*' 2>/dev/null
  find apps -type d -name src -not -path '*/third_party/*' 2>/dev/null
}

# Assemble every semantic cppcheck dump option in one array. The whole-tree
# scan, the targeted POSIX refresh, and both behavioral selftests consume this
# exact authority. Callers may add only scheduling (-j), one staged library
# model, and their input path(s).
ra8_misra_build_dump_args() {
  local build_dir
  RA8_MISRA_EXCLUDE_ARGS=()
  for build_dir in ${RA8_MISRA_BUILD_DIRS[@]+${RA8_MISRA_BUILD_DIRS[@]+"${RA8_MISRA_BUILD_DIRS[@]}"}}; do
    RA8_MISRA_EXCLUDE_ARGS+=("-i$build_dir")
  done
  RA8_MISRA_DUMP_ARGS=(
    --dump
    --enable=warning
    --inline-suppr
    "${SUPPRESS_ARGS[@]}"
    --suppress=missingIncludeSystem
    --suppress=unmatchedSuppression
    --suppress=syntaxError
    --suppress=internalError
    '--suppress=*:libs/third_party/*'
    -ilibs/third_party
    '--suppress=*:apps/shared_libs/third_party/*'
    -iapps/shared_libs/third_party
    -itools/vela/generated
    "${RA8_MISRA_EXCLUDE_ARGS[@]}"
    -U__clang__
    --std=c11
    --platform=unix32
    --language=c
    --quiet
    --error-exitcode=0
    "${INCLUDE_DIRS[@]}"
  )
}

ra8_misra_prepare_scan_arguments() {
  local inc_dir line

  RA8_MISRA_BUILD_DIRS=()
  while IFS= read -r line || [[ -n "$line" ]]; do
    RA8_MISRA_BUILD_DIRS+=("$line")
  done < <(ra8_misra_build_output_dirs ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"})

  INCLUDE_DIRS=()
  while IFS= read -r inc_dir; do
    [[ -n "$inc_dir" ]] || continue
    ra8_misra_is_build_output "$inc_dir" && continue
    INCLUDE_DIRS+=("-I$inc_dir")
  done < <(ra8_misra_header_dirs | sort -u)
  if [[ ${#INCLUDE_DIRS[@]} -eq 0 ]]; then
    echo "[ERROR] no header roots found -- run from the repo root" >&2
    return 1
  fi

  SUPPRESS_ARGS=()
  while IFS= read -r line; do
    line="${line%$'\r'}"
    line="${line## }"
    line="${line%% }"
    [[ -z "$line" ]] && continue
    case "$line" in
      \#*) continue ;;
    esac
    SUPPRESS_ARGS+=("--suppress=$line")
  done <"$ROOT_DIR/.cppcheck-suppressions"

  ra8_misra_build_dump_args
}

# Run cppcheck with checked status. --error-exitcode=0 keeps diagnostics from
# changing its status; any remaining nonzero value is therefore an execution,
# configuration, or timeout failure and must stop the audit.
ra8_misra_run_checked() {
  local error_log="$1" label="$2" rc
  shift 2

  if "$@" 2>>"$error_log"; then
    rc=0
  else
    rc=$?
  fi
  if [[ "$rc" -eq 124 ]]; then
    echo "[ERROR] $label exceeded the 10-minute budget" >&2
    return 1
  fi
  if [[ "$rc" -ne 0 ]]; then
    echo "[ERROR] $label failed with status $rc" >&2
    return 1
  fi
}

# A targeted dump must never reuse the ordinary pass's bytes. Refuse to run if
# either artifact cannot be removed, and reject symlinked output even when its
# target is a regular file.
ra8_misra_remove_dump_artifacts() {
  local artifact source="$1"
  for artifact in "$source.dump" "$source.ctu-info"; do
    if [[ -e "$artifact" || -L "$artifact" ]]; then
      if ! rm -f -- "$artifact" 2>/dev/null; then
        echo "[ERROR] cannot remove stale analyzer artifact: $artifact" >&2
        return 1
      fi
    fi
    if [[ -e "$artifact" || -L "$artifact" ]]; then
      echo "[ERROR] stale analyzer artifact survived removal: $artifact" >&2
      return 1
    fi
  done
}

ra8_misra_refresh_dump() {
  local error_log="$2" label="$3" source="$1"
  shift 3

  if ! ra8_misra_remove_dump_artifacts "$source"; then
    return 1
  fi
  if ! ra8_misra_run_checked "$error_log" "$label" "$@" "$source"; then
    return 1
  fi
  if [[ ! -f "$source.dump" || -L "$source.dump" ]]; then
    echo "[ERROR] $label produced no trustworthy dump" >&2
    return 1
  fi
}

# Discover the exact translation-unit population cppcheck is expected to dump.
# Keeping this derivation beside the dump inventory prevents a successful
# command that silently omits one source from looking like debt burn-down.
ra8_misra_source_files() {
  find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} -type f \
    \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
    -not -path '*/third_party/*' -not -path '*/vela/generated/*' 2>/dev/null |
    sort
}

# A killed audit can leave valid-looking dumps behind. Remove every artifact
# before the producer starts and fail if any path cannot be removed; cleanup at
# EXIT alone cannot protect the next run from SIGKILL residue.
ra8_misra_remove_all_dump_artifacts() {
  local artifact
  while IFS= read -r artifact; do
    [[ -n "$artifact" ]] || continue
    if ! rm -f -- "$artifact" 2>/dev/null; then
      echo "[ERROR] cannot remove stale analyzer artifact: $artifact" >&2
      return 1
    fi
    if [[ -e "$artifact" || -L "$artifact" ]]; then
      echo "[ERROR] stale analyzer artifact survived removal: $artifact" >&2
      return 1
    fi
  done < <(
    find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} \
      \( -name '*.dump' -o -name '*.ctu-info' \) \
      -not -path '*/third_party/*' -not -path '*/vela/generated/*' 2>/dev/null |
      sort
  )
}

# `--error-exitcode=0` keeps ordinary diagnostics from failing cppcheck. Parse
# failures are different: they can leave a syntactically valid but incomplete
# dump and therefore must remain fatal even when cppcheck returns zero.
ra8_misra_reject_parse_failures() {
  local error_log="$1" rc
  if grep -Eq '\[(internalAstError|internalError|syntaxError)\] *$' "$error_log"; then
    echo "[ERROR] cppcheck emitted a fatal parser diagnostic" >&2
    return 1
  else
    rc=$?
  fi
  if [[ "$rc" -ne 1 ]]; then
    echo "[ERROR] cannot inspect cppcheck diagnostics: $error_log" >&2
    return 1
  fi
}

# Bind the consumer to exactly one regular, non-symlink dump for every source.
# Orphan, substituted, missing, or extra dumps are rejected before misra.py can
# consume them. The global DUMPS array is the sole output used by the add-on.
ra8_misra_collect_dump_inventory() {
  local actual_dump expected_dump index source
  local -a actual_dumps=() expected_dumps=()

  while IFS= read -r source; do
    [[ -n "$source" ]] || continue
    ra8_misra_is_build_output "$source" && continue
    expected_dumps+=("$source.dump")
  done < <(ra8_misra_source_files)
  if [[ ${#expected_dumps[@]} -eq 0 ]]; then
    echo "[ERROR] expected zero translation-unit dumps -- the audit did not run" >&2
    return 1
  fi

  while IFS= read -r actual_dump; do
    [[ -n "$actual_dump" ]] || continue
    ra8_misra_is_build_output "$actual_dump" && continue
    if [[ ! -f "$actual_dump" || -L "$actual_dump" ]]; then
      echo "[ERROR] untrustworthy analyzer dump: $actual_dump" >&2
      return 1
    fi
    actual_dumps+=("$actual_dump")
  done < <(
    find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} -name '*.dump' \
      -not -path '*/third_party/*' -not -path '*/vela/generated/*' | sort
  )

  if [[ ${#actual_dumps[@]} -ne ${#expected_dumps[@]} ]]; then
    echo "[ERROR] dump inventory mismatch: expected ${#expected_dumps[@]}, got ${#actual_dumps[@]}" >&2
    return 1
  fi
  for ((index = 0; index < ${#expected_dumps[@]}; ++index)); do
    expected_dump="${expected_dumps[$index]}"
    actual_dump="${actual_dumps[$index]}"
    if [[ "$actual_dump" != "$expected_dump" ]]; then
      echo "[ERROR] dump inventory mismatch: expected $expected_dump, got $actual_dump" >&2
      return 1
    fi
  done
  DUMPS=("${actual_dumps[@]}")
}

# misra.py returns 0 when clean and 1 when it emits findings. Any other status
# is an execution failure. A traceback is a crash regardless of status (the
# add-on has returned 1 after writing a partial traceback in practice).
ra8_misra_capture_addon() {
  local label="$2" output="$1" rc
  shift 2

  if "$@" >"$output" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  case "$rc" in
    0 | 1) ;;
    *)
      echo "[ERROR] $label failed with status $rc" >&2
      return 1
      ;;
  esac
  if grep -qF 'Traceback (most recent call last):' "$output"; then
    echo "[ERROR] $label emitted a Python traceback with status $rc" >&2
    return 1
  fi
}

# The selftest is a separate sourced module so production orchestration and its
# extensive adversarial fixtures remain independently reviewable.
# shellcheck source=scripts/checks/misra/selftest.sh
. "$SCRIPT_DIR/misra/selftest.sh"

case "${1:-}" in
  --selftest)
    ra8_misra_selftest
    exit $?
    ;;
  "") ;;
  *)
    echo "misra_check_inner.sh: unknown option: $1" >&2
    echo "usage: misra_check_inner.sh [--selftest]" >&2
    exit 2
    ;;
esac

# The build trees to keep out of this run. Empty on a clean checkout, which is
# why the selftest asserts that case too.
if ! ra8_misra_prepare_scan_arguments; then
  exit 2
fi
if [[ ${#RA8_MISRA_BUILD_DIRS[@]} -gt 0 ]]; then
  echo "[INFO] excluding ${#RA8_MISRA_BUILD_DIRS[@]} build tree(s) from the scan:" >&2
  printf '         %s\n' ${RA8_MISRA_BUILD_DIRS[@]+"${RA8_MISRA_BUILD_DIRS[@]}"} >&2
fi

# Delete dump artefacts on EVERY exit path, not just success. A stale dump
# left behind by an aborted run (disk-full, timeout, ^C) would be picked up
# by the next run's find and could resurrect findings for source that has
# since changed -- silent corruption of the ratchet comparison.
# shellcheck disable=SC2329  # invoked by `trap cleanup_dumps EXIT` below.
cleanup_dumps() {
  find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} -name '*.dump' -not -path '*/third_party/*' -delete 2>/dev/null || true
  find ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"} -name '*.ctu-info' -not -path '*/third_party/*' -delete 2>/dev/null || true
  if [[ -n "${RA8_MISRA_STAGED_ADDONS:-}" && -d "$RA8_MISRA_STAGED_ADDONS" ]]; then
    rm -rf -- "$RA8_MISRA_STAGED_ADDONS"
  fi
}
trap cleanup_dumps EXIT

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "[ERROR] cppcheck not in PATH (brew install cppcheck)" >&2
  exit 2
fi

ADDON_DIR="$(ra8_misra_find_addon_dir)" || exit 2
POSIX_CFG="$(ra8_misra_find_posix_cfg)" || exit 2
RA8_MISRA_STAGED_ADDONS="$(mktemp -d "$OUT_DIR/cppcheck-addons.XXXXXX")"
ra8_misra_stage_addons \
  "$ADDON_DIR" "$RA8_MISRA_STAGED_ADDONS" "$RA8_MISRA_9_PATCH" "$POSIX_CFG" || exit 2
MISRA_PY="$RA8_MISRA_STAGED_ADDONS/misra.py"
POSIX_LIBRARY="$RA8_MISRA_STAGED_ADDONS/$RA8_MISRA_POSIX_LIBRARY_NAME"

JOBS="${JOBS:-$(ra8_max_jobs)}"

# Header roots, enumerated FROM the scan roots above rather than hand-picked,
# and at ANY depth beneath them.
#
# The list used to name five directories while the audit scanned libs/
# AND port/, so every header under port/*/inc was invisible: cppcheck saw
# the calls into those interfaces as implicitly-declared functions and
# charged the caller MISRA 17.3 for them. That is a defect in the audit's
# view of the tree, not in the tree -- the same failure the annotation
# gate had when its include path was a hand-picked list (see
# _include_args() in check_annotations.py).
#
# It recurred twice while apps/ was being added, which is why neither a
# hand-written list nor a fixed-depth glob is used here any more. First the
# glob still named only tools/*/inc, so the product headers under apps/ went
# missing. Then the products moved down a level, under a category directory,
# and a one-star glob missed them again. Finding every directory NAMED inc under
# the scanned roots has no depth to get wrong: the audit's header path is a
# consequence of the layout instead of a second description of it.
#
# ...and the same for `src` under apps/, where the PRIVATE header roots have to
# be on the path too. Elsewhere they do not: a libs/ or tools/ module is one
# directory pair, so a TU reaches its own `*_internal.h` by the same-directory
# rule and needs no -I. A product under apps/ spans two directories -- its
# portable core at apps/shared_libs/<product> and each build form at
# apps/<form>/<product> -- so a form's TU includes core `*_internal.h` headers
# that same-directory resolution cannot see. Without this, cppcheck charged the
# caller 17.3 for the resulting implicit declarations AND silently stopped
# raising 9.2 on aggregates whose types it could no longer resolve. A net DROP
# in findings is the dangerous direction of that failure: it reads as a
# burn-down. Derived by the same find, so a product that grows a third
# directory is covered the day it lands.

echo "[INFO] cppcheck MISRA-C 2012 audit -- $(cppcheck --version)" >&2
echo "[INFO] jobs=$JOBS  output=$RESULTS" >&2

if command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT_CMD=(gtimeout 600)
elif command -v timeout >/dev/null 2>&1; then
  TIMEOUT_CMD=(timeout 600)
else
  TIMEOUT_CMD=()
fi

# cppcheck 2.13 (Ubuntu 24.04 / Debian 12 -- the CI + dev-box version) is
# finicky about the --suppressions-list parser ("Failed to add suppression.
# No id."); convert each non-comment, non-blank line into an explicit
# --suppress= flag instead, exactly like the cppcheck CI job does. This
# syntax is accepted by every cppcheck version.

# The pinned cppcheck 2.13 does not support --std=c23. The codebase uses C23
# typed enums (`enum : uint8_t`) and `[[nodiscard]]`-style attributes;
# both raise syntaxError on the affected line. cppcheck recovers and
# continues parsing the rest of the translation unit, so MISRA
# coverage on the remaining ~95% of code is still useful.
#
# -U__clang__ is load-bearing. ra8_attributes.h guards the annotation
# macros with `#if defined(__clang__) && !defined(__CPPCHECK__)` so the
# `[[clang::annotate("...")]]` form -- which cppcheck's C parser cannot
# represent, and which damages the parse of whatever declaration follows
# -- stays hidden from this audit. That guard is not sufficient on its
# own: cppcheck does not merely evaluate the condition, it ENUMERATES
# configurations for every macro named in it, and emits a
# `__GNUC__;__clang__` configuration in which __clang__ is defined and
# __CPPCHECK__ is not. In that configuration the attribute survives, and
# misra.py reports phantom findings against the annotated file -- 14.2,
# 16.2 and 17.3 where the source has no loop, no switch and no implicit
# declaration, plus inflated 15.5 exit-point counts and an extra 8.4 for
# every definition whose declaration the damaged parse swallowed.
# Mentioning __CPPCHECK__ in the guard is what makes cppcheck enumerate
# it, so the header alone can never close this.
#
# -U removes __clang__ from configuration enumeration entirely, so no
# such configuration is generated. This is also the configuration that
# ships: the firmware is built by arm-none-eabi-gcc, where these macros
# are already comments. Suppressing the four rules or absorbing the
# findings into the baseline would blind the ratchet to real defects in
# every annotated file.
echo "[INFO] generating cppcheck dumps under ${RA8_MISRA_ROOTS[*]} ..." >&2
: >"$RAW"
if ! ra8_misra_remove_all_dump_artifacts; then
  exit 1
fi
if ! ra8_misra_run_checked \
  "$RAW" "whole-tree cppcheck" \
  ${TIMEOUT_CMD[@]+"${TIMEOUT_CMD[@]}"} cppcheck -j "$JOBS" \
  "${RA8_MISRA_DUMP_ARGS[@]}" \
  ${RA8_MISRA_ROOTS[@]+"${RA8_MISRA_ROOTS[@]}"}; then
  exit 1
fi
if ! ra8_misra_reject_parse_failures "$RAW"; then
  exit 1
fi

# Replace only the hosted POSIX filesystem TU's ordinary dump with one using
# the exact staged, digest-pinned POSIX model. Applying it globally changes
# type inference in unrelated code; filtering results would hide findings.
echo "[INFO] regenerating $RA8_MISRA_POSIX_MODEL_SOURCE with $POSIX_LIBRARY ..." >&2
if ! ra8_misra_refresh_dump \
  "$RA8_MISRA_POSIX_MODEL_SOURCE" "$RAW" "targeted POSIX cppcheck model" \
  ${TIMEOUT_CMD[@]+"${TIMEOUT_CMD[@]}"} cppcheck \
  "${RA8_MISRA_DUMP_ARGS[@]}" "--library=$POSIX_LIBRARY"; then
  exit 1
fi
if ! ra8_misra_reject_parse_failures "$RAW"; then
  exit 1
fi

# Run misra.py on every dump file produced under the first-party roots.
if ! ra8_misra_collect_dump_inventory; then
  exit 1
fi
echo "[INFO] running misra.py on ${#DUMPS[@]} dump file(s) ..." >&2

if ! ra8_misra_capture_addon \
  "$MISRA_RAW" "whole-tree MISRA add-on" \
  env PYTHONPATH="$RA8_MISRA_STAGED_ADDONS" \
  python3 "$MISRA_PY" --quiet ${DUMPS[@]+"${DUMPS[@]}"}; then
  exit 1
fi

# misra.py emits one diagnostic per line in the format:
#   [path/file.c:LINE] (severity) <message> [misra-c2012-X.Y]
# Parse into TSV: rule \t severity \t file \t line \t message
grep -E '\[misra-c2012-[0-9]+\.[0-9]+\] *$' "$MISRA_RAW" | awk '
{
    n = match($0, /\[misra-[A-Za-z0-9.\-]+\] *$/);
    if (n == 0) next;
    rule = substr($0, RSTART+1, RLENGTH-2);
    sub(/ *$/, "", rule);
    head = substr($0, 1, RSTART-1);
    sub(/ *$/, "", head);
    if (match(head, /^\[[^]]+\]/) == 0) next;
    locator = substr(head, 2, RLENGTH-2);
    rest = substr(head, RLENGTH+1);
    sub(/^ */, "", rest);
    sev = "style";
    if (match(rest, /^\([a-z]+\)/)) {
        sev = substr(rest, 2, RLENGTH-2);
        rest = substr(rest, RLENGTH+1);
        sub(/^ */, "", rest);
    }
    n2 = split(locator, lp, ":");
    file = lp[1];
    line = (n2 >= 2) ? lp[2] : "";
    msg = rest;
    gsub(/\t/, " ", msg);
    printf("%s\t%s\t%s\t%s\t%s\n", rule, sev, file, line, msg);
}' | sort -u >"$RESULTS"

# Dump artefacts are removed by the EXIT trap (cleanup_dumps above).

TOTAL=$(wc -l <"$RESULTS" | tr -d ' ')

echo
echo "MISRA-C 2012 audit complete"
echo "  results:    $RESULTS"
echo "  cppcheck:   $RAW"
echo "  misra.py:   $MISRA_RAW"
echo "  total unique violations: $TOTAL"
echo
echo "Top 10 most-violated rules:"
awk -F'\t' '{print $1}' "$RESULTS" | sort | uniq -c | sort -rn | head -10

exit 0

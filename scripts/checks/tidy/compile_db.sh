#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
#
# scripts/checks/tidy/compile_db.sh -- Where each pass's compile commands come from.
#
# SOURCED, NEVER EXECUTED. See collect.sh for the loading contract.
#
# clang-tidy cannot analyse a translation unit it cannot parse, and it cannot
# parse one without the flags the TU is really compiled with. This fragment
# assembles the two databases that supply them: the host unit-test build
# (BUILD_DIR) and the cross-compile database (CROSS_DB_DIR).
#
# Functions here: configure_build, configure_reflow_v2_db, merge_compile_db,
# collect_include_args, build_cross_db

# ---------------------------------------------------------------------------
# Configure test build to generate compile_commands.json
# ---------------------------------------------------------------------------
configure_build() {
  local tests_dir="$FIRMWARE_DIR/tests"
  BUILD_DIR="$FIRMWARE_DIR/build/tidy"

  if [[ ! -d "$tests_dir" ]]; then
    print_warning "$tests_dir does not exist yet. Falling back to a host-native"
    print_warning "configure of the top-level CMakeLists.txt."
    tests_dir="$FIRMWARE_DIR"
  fi

  print_status "Configuring test build in $BUILD_DIR ..."
  local cmake_stdout="/dev/null"
  if [[ "${VERBOSE:-}" == "true" ]]; then
    cmake_stdout="/dev/stdout"
  fi

  # Disable coverage instrumentation so the tidy build does not
  # leave .gcno files that the parallel coverage build (build/coverage)
  # would mistakenly merge under gcovr.
  cmake -B "$BUILD_DIR" -S "$tests_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-DUNIT_TEST -DRA8_OFF_TARGET" \
    -DRA8_COVERAGE=OFF \
    -Wno-dev \
    >"$cmake_stdout"

  if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    print_error "compile_commands.json was not generated."
    exit "$RC_INFRA"
  fi

  configure_reflow_v2_db "$tests_dir" "$cmake_stdout"

  print_status "compile_commands.json ready."
}

# ---------------------------------------------------------------------------
# Merge the alternate reflow engine's commands into the primary database.
#
# ra8_reflow ships TWO mutually exclusive engines: the hand-rolled v1 (the
# default) and the LiteHTML-backed v2 behind RA8_REFLOW_USE_LITEHTML. Only
# one set of sources is compiled, so the default configure in configure_build
# has no command for libs/ra8_reflow/v2/ra8_reflow_v2.cpp or its test -- and
# clang-tidy then infers one, drops the litehtml include path and reports
# "'litehtml.h' file not found" instead of analysing either file.
#
# Configure the other engine into its own tree and merge its entries in for
# files the default tree does not cover. Merging (rather than switching) is
# what keeps BOTH engines linted from one database: flipping the option would
# simply move the blind spot onto v1.
#
# $1  the source directory to configure
# $2  where cmake's stdout goes (/dev/null unless --verbose)
# ---------------------------------------------------------------------------
configure_reflow_v2_db() {
  local tests_dir="$1"
  local cmake_stdout="$2"
  local alt_dir="$FIRMWARE_DIR/build/tidy-reflow-v2"

  print_status "Configuring the alternate reflow engine in $alt_dir ..."
  if cmake -B "$alt_dir" -S "$tests_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-DUNIT_TEST -DRA8_OFF_TARGET" \
    -DRA8_COVERAGE=OFF \
    -DRA8_REFLOW_USE_LITEHTML=ON \
    -Wno-dev \
    >"$cmake_stdout" 2>&1; then
    merge_compile_db "$alt_dir/compile_commands.json" "$BUILD_DIR/compile_commands.json"
  else
    print_error "The RA8_REFLOW_USE_LITEHTML configure failed."
    print_error "Without it the v2 engine and its test cannot be parsed, so"
    print_error "this gate would report them clean having never analysed them."
    exit "$RC_INFRA"
  fi
}

# ---------------------------------------------------------------------------
# Merge compile-database $1 into $2, adding only entries for files $2 lacks.
#
# "Only what is missing" is deliberate: the primary database describes how a
# file is really built, and an alternate configure must never silently
# redefine that. It fills gaps, it does not overrule.
# ---------------------------------------------------------------------------
merge_compile_db() {
  python3 - "$1" "$2" <<'PY'
import json
import os
import sys

src_path, dst_path = sys.argv[1], sys.argv[2]
with open(dst_path, encoding="utf-8") as handle:
    dst = json.load(handle)
try:
    with open(src_path, encoding="utf-8") as handle:
        src = json.load(handle)
except OSError:
    sys.exit(0)


def key(entry):
    return os.path.realpath(os.path.join(entry["directory"], entry["file"]))


have = {key(e) for e in dst}
added = [e for e in src if key(e) not in have]
if added:
    with open(dst_path, "w", encoding="utf-8") as handle:
        json.dump(dst + added, handle, indent=1)
print(f"merged {len(added)} entry(ies) from {os.path.basename(os.path.dirname(src_path))}")
PY
}

# ---------------------------------------------------------------------------
# Union of every -I directory in the generated compile_commands.json.
#
# clang-tidy has no compile command for a bare header, nor for a TU that the
# default test configure leaves out of the build (tests/fuzz/, tests/bench/).
# For those it *infers* one from the nearest file it does know, which routinely
# drops an include directory and turns the lint into a clang-diagnostic-error
# ("'miniz.h' file not found") instead of a real analysis. Appending the union
# of the build's own -I flags makes every first-party include resolvable no
# matter which command was inferred. It is derived from the compile database,
# so it needs no maintenance as targets gain include directories.
# ---------------------------------------------------------------------------
collect_include_args() {
  python3 - "$BUILD_DIR/compile_commands.json" <<'PY'
import json
import shlex
import sys

seen = set()
with open(sys.argv[1], encoding="utf-8") as handle:
    entries = json.load(handle)
for entry in entries:
    args = entry.get("arguments") or shlex.split(entry.get("command", ""))
    for arg in args:
        if arg.startswith("-I") and len(arg) > 2 and arg not in seen:
            seen.add(arg)
            print(arg)
PY
}

# ---------------------------------------------------------------------------
# The cross-compile compile database the firmware pass parses against.
#
# CROSS_DB_DIR is produced by scripts/builders/build_cross_compile_db.py, which
# performs the real CMake cross-configures and FAILS if any first-party
# firmware TU ends up without a compile command. That check is the reason this
# pass cannot silently shrink: a database covering less is an error there, not
# a faster run here.
#
# The two fix-ups needed on top of it live in pass_args.sh
# (arm_system_includes, firmware_pass_args).
# ---------------------------------------------------------------------------
CROSS_DB_DIR=""

build_cross_db() {
  CROSS_DB_DIR="$FIRMWARE_DIR/build/xtidy"
  print_status "Building the cross-compile compile database ..."
  local out
  if ! out=$(python3 "$FIRMWARE_DIR/scripts/builders/build_cross_compile_db.py" \
    --out "$CROSS_DB_DIR" --check 2>&1); then
    printf '%s\n' "$out" >&2
    print_error "The cross-compile compile database could not be built."
    print_error "Without it the firmware TUs cannot be parsed, so this gate"
    print_error "would report a clean run over code it never analysed."
    exit "$RC_INFRA"
  fi
  printf '%s\n' "$out" | tail -2 >&2
}

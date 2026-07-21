#!/usr/bin/env bash
#
# scripts/build/all_examples.sh -- build every examples/<app> target.
#
# Iterates each top-level examples/<app>/ directory containing a main.c,
# invokes `make <app>` from the repo root, captures pass/fail per app,
# prints a summary table at the end, and exits non-zero on any failure.
#
# Usage:
#   bash scripts/build/all_examples.sh
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$REPO_ROOT" || exit 1

if [ ! -d examples ]; then
  echo "error: examples/ directory not found at $REPO_ROOT" >&2
  exit 1
fi

# ereader_shelf includes a generated baked-book-library header (library.h): the
# full version (real .rabook blobs + cover thumbnails) is produced from the
# Git-LFS content/library/*.epub sources by tools/bake_library.py via
# `make books`, and is gitignored as a build artifact. A fresh checkout has
# none, so the app fails to compile with "library.h: No such file or directory".
# For the cross-build we only need ereader_shelf to COMPILE, not to embed real
# books, and regenerating from LFS sources needs git-lfs + Pillow that CI lacks.
# So emit a 0-book stub (with the same struct/symbols) when the real header is
# absent; `make books` still produces the full library for HIL / deployment.
shelf_lib="examples/ek_ra8d2/hw_validated/hil/ereader_shelf/library.h"
if [ ! -f "$shelf_lib" ]; then
  echo "build_all: emitting a 0-book stub $shelf_lib (run 'make books' for the full library)"
  cat >"$shelf_lib" <<'STUB'
/**
 * @file library.h
 * @generated build_all_examples.sh stub -- do not edit by hand.
 * @brief Stub baked book library (0 books) emitted by build_all_examples.sh.
 * @details ereader_shelf includes this generated header. The real version
 *          (full .rabook blobs + pre-decoded cover thumbnails) is produced by
 *          tools/bake_library.py via `make books`. This 0-book stub lets the
 *          app compile for the cross-build when the Git-LFS .epub sources and
 *          the book-compile tooling are unavailable; it loads books from the SD
 *          card at runtime instead.
 * @since Version 1.0.0
 */
#pragma once

#include <stdint.h>

/** @brief One openable baked book: compressed blob + cover thumbnail + metadata. */
typedef struct {
  const uint8_t* blob;    /**< RBKZ container start.            */
  uint32_t       len;     /**< Container length in bytes.       */
  const uint8_t* thumb;   /**< gray8 cover thumbnail, or NULL.  */
  uint16_t       thumb_w; /**< Thumbnail width in pixels.       */
  uint16_t       thumb_h; /**< Thumbnail height in pixels.      */
  const char*    title;   /**< Display title.                   */
  const char*    author;  /**< Display author.                  */
} library_book_t;

/** @brief Number of baked books: one empty placeholder in this compile stub. */
typedef enum : uint16_t {
  k_library_count = 1U,
} library_count_t;

/** @brief Baked book table; a single empty placeholder (cross-build is
 *         compile-only, so the null blob is never opened at runtime). */
static const library_book_t k_library[k_library_count] = {
    {nullptr, 0U, nullptr, 0U, 0U, "", ""},
};
STUB
fi

# Auto-discover apps: every examples/<tier>/<name>/ dir with a main.c.
# Apps live at arbitrary depth under examples/. Discover them via find.
# The build-target name is the bare directory name -- `make blink` works
# regardless of how deep the app lives.
apps=()
while IFS= read -r main_c; do
  d="$(dirname "$main_c")"
  # examples/host/* are macOS-only dev tools, not cross-compiled firmware.
  case "$d" in
    examples/host/* | ./examples/host/*) continue ;;
  esac
  name="$(basename "$d")"
  if [ -f "$d/Makefile" ]; then
    apps+=("$name")
  fi
done < <(find examples -name "main.c" | sort)

if [ "${#apps[@]}" -eq 0 ]; then
  echo "error: no buildable apps discovered under examples/" >&2
  exit 1
fi

# Sort the discovered apps alphabetically for stable output.
mapfile -t apps < <(printf '%s\n' "${apps[@]}" | sort)

LOG_DIR="$REPO_ROOT/build/build_all_examples"
mkdir -p "$LOG_DIR"

# Per-app exit codes are collected in a status directory: each worker writes
# $STATUS_DIR/<app> containing the app's `make` return code. Completion order
# under the parallel pool is nondeterministic, so the summary is reconstructed
# from these files and re-sorted alphabetically (same order as the serial run).
STATUS_DIR="$LOG_DIR/.status"
rm -rf "$STATUS_DIR"
mkdir -p "$STATUS_DIR"

# Worker pool size: one `make <app>` per worker, each build SINGLE-THREADED.
# The across-app pool is the only parallelism -- we deliberately do NOT also
# pass a per-app --parallel, which would oversubscribe (nproc x nproc).
detect_ncpu() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  else
    echo 1
  fi
}
MAX_JOBS="${MAX_JOBS:-$(detect_ncpu)}"
# Guard against a non-numeric / zero override.
case "$MAX_JOBS" in
  '' | *[!0-9]*) MAX_JOBS=1 ;;
esac
if [ "$MAX_JOBS" -lt 1 ]; then
  MAX_JOBS=1
fi

echo "==> Building ${#apps[@]} apps from examples/ (pool: $MAX_JOBS parallel jobs)"
echo

# Build one app: run `make <app>` single-threaded, tee the per-app log exactly
# as the serial version did, and record the exit code in the status directory.
# Runs in a subshell spawned by xargs, so it must re-derive its environment.
build_one_app() {
  app="$1"
  log_file="$LOG_DIR/${app}.log"
  if make "$app" >"$log_file" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  printf '%s\n' "$rc" >"$STATUS_DIR/${app}"
  if [ "$rc" -eq 0 ]; then
    printf "  [build] %-40s ... PASS\n" "$app"
  else
    printf "  [build] %-40s ... FAIL (rc=%s, log: %s)\n" "$app" "$rc" "$log_file"
  fi
}
export -f build_one_app
export LOG_DIR STATUS_DIR REPO_ROOT

# Warm the one shared prerequisite BEFORE the pool starts. Every top-level
# `make <app>` lists build/compile_commands.json as a normal prerequisite,
# regenerated by a single cmake configure into the shared build/ tree. On a
# cold checkout that file is absent, so a pool launched straight away has its
# whole first wave of workers racing that one configure at once -- their cmake
# TryCompile scratch dirs collide and a handful of apps die with "file failed
# to open for writing" / "No rule to make target .../TryCompile-*". Generating
# it once here, serially, leaves it up to date for every worker (no per-app
# build touches a CMake input), so the shared configure never runs concurrently.
echo "==> Warming shared build/compile_commands.json (serial, pre-pool)"
if ! make compile_commands >"$LOG_DIR/compile_commands.log" 2>&1; then
  echo "error: failed to generate build/compile_commands.json (log: $LOG_DIR/compile_commands.log)" >&2
  exit 1
fi

# Prebuild the universal first-party library archive ONCE, then hand every
# eligible per-app build a pointer to it so it links the archive instead of
# recompiling ~180 identical library sources (ra8_core / ra8_hal / ra8_net_pal
# / ra8_usb_pal / board / secure_app) into its own ELF. That redundant
# recompile -- ~180 sources x ~200 apps -- was the dominant cost of this gate.
# cmake/ra8_add_app.cmake reads RA8_SHARED_LIB_ARCHIVE at configure time and
# links it with --whole-archive (+ the toolchain --gc-sections), producing a
# byte-identical flashable image to the from-source build. Ineligible apps
# (TrustZone -mcmse, non-ek board, insecure-crypto opt-in) ignore the archive
# and compile from source, so their objects and gates (e.g. the tz_nsc_cgc_usb
# SG-veneer offsets) stay correct. Set RA8_NO_SHARED_LIBS=1 to force every app
# back onto the from-source path (the pre-fast-path behaviour).
SHARED_LIB_DIR="$REPO_ROOT/build/shared_libs"
SHARED_LIB_ARCHIVE="$SHARED_LIB_DIR/libra8_shared_ek_ra8d2.a"
if [ "${RA8_NO_SHARED_LIBS:-0}" != "1" ]; then
  echo "==> Prebuilding the universal library archive (serial, pre-pool)"
  if cmake -S "$REPO_ROOT/cmake/shared_libs" -B "$SHARED_LIB_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchain-ra8d2.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    >"$LOG_DIR/shared_libs.log" 2>&1 &&
    cmake --build "$SHARED_LIB_DIR" --parallel "$MAX_JOBS" \
      >>"$LOG_DIR/shared_libs.log" 2>&1 &&
    [ -f "$SHARED_LIB_ARCHIVE" ]; then
    export RA8_SHARED_LIB_ARCHIVE="$SHARED_LIB_ARCHIVE"
    echo "    archive ready: $SHARED_LIB_ARCHIVE"
  else
    echo "error: failed to prebuild $SHARED_LIB_ARCHIVE (log: $LOG_DIR/shared_libs.log)" >&2
    echo "       the shared libraries do not compile under the canonical flags;" >&2
    echo "       fix that, or set RA8_NO_SHARED_LIBS=1 to force the from-source path." >&2
    exit 1
  fi
else
  echo "==> RA8_NO_SHARED_LIBS=1: every app compiles the libraries from source"
fi

# Bounded worker pool across apps. Prefer xargs -P for portability; app names
# are bare directory names (no spaces / newlines), so newline-delimited xargs
# input is safe. Fall back to a strict serial loop if xargs -P is unavailable.
if printf '' | xargs -P 2 -I '{}' true >/dev/null 2>&1; then
  printf '%s\n' "${apps[@]}" |
    xargs -P "$MAX_JOBS" -I '{}' bash -c 'build_one_app "$@"' _ '{}'
else
  echo "build_all: xargs -P unavailable, falling back to serial build" >&2
  for app in "${apps[@]}"; do
    build_one_app "$app"
  done
fi

# Reconstruct results in the original alphabetical app order from status files.
results=()
fail_count=0
for app in "${apps[@]}"; do
  status_file="$STATUS_DIR/${app}"
  if [ -f "$status_file" ] && [ "$(cat "$status_file")" = "0" ]; then
    results+=("PASS")
  else
    results+=("FAIL")
    fail_count=$((fail_count + 1))
  fi
done

echo
echo "============================================================"
echo " Build summary"
echo "============================================================"
printf " %-40s  %-8s\n" "App" "Status"
printf " %-40s  %-8s\n" "----------------------------------------" "--------"
for i in "${!apps[@]}"; do
  printf " %-40s  %-8s\n" "${apps[$i]}" "${results[$i]}"
done
echo "============================================================"
total="${#apps[@]}"
pass_count=$((total - fail_count))
echo " Total: $total   Passed: $pass_count   Failed: $fail_count"
echo "============================================================"

if [ "$fail_count" -gt 0 ]; then
  exit 1
fi

exit 0

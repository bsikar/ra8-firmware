#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/all_examples.sh -- build every firmware configuration.
#
# Reads the canonical application/configuration matrix from ra8_apps.py,
# invokes `just apps::build <configuration>` from the repo root, captures
# pass/fail per configuration, prints a summary, and fails on any error.
#
# Usage:
#   bash scripts/builders/all_examples.sh
#   printf '%s\0' app1 app2 | bash scripts/builders/all_examples.sh --selected0
#   bash scripts/builders/all_examples.sh --selftest
#
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328). The
# across-app worker pool below defaults to it so this canonical cross-build
# does not grab every core when it shares the box with other gate jobs.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$REPO_ROOT/scripts/ci/lib/parallelism.sh"
# shellcheck source=scripts/builders/lib/app_batch.sh
. "$REPO_ROOT/scripts/builders/lib/app_batch.sh"

cd "$REPO_ROOT" || exit 1

selected_mode=0
case "${1:-}" in
  --selftest)
    [[ $# -eq 1 ]] || {
      echo "usage: all_examples.sh [--selected0|--selftest]" >&2
      exit 2
    }
    ra8_app_batch_selftest "$REPO_ROOT/scripts/dev/ra8_apps.py"
    exit $?
    ;;
  --selected0)
    [[ $# -eq 1 ]] || {
      echo "usage: all_examples.sh [--selected0|--selftest]" >&2
      exit 2
    }
    selected_mode=1
    ra8_app_batch_read_nul || exit $?
    ra8_app_batch_resolve "$REPO_ROOT/scripts/dev/ra8_apps.py" || exit $?
    ra8_app_batch_require_census \
      "${#RA8_APP_BATCH_ITEMS[@]}" "${RA8_SELECTED_APP_FLOOR:-1}" || exit $?
    ;;
  '') ;;
  *)
    echo "usage: all_examples.sh [--selected0|--selftest]" >&2
    exit 2
    ;;
esac

# ereader_shelf includes a generated baked-book-library header (library.h): the
# full version (real .rabook blobs + cover thumbnails) is produced from the
# Git-LFS e-reader content library .epub sources by
# tools/epub_compile/src/bake_library.py via
# `just tools::books`, and is gitignored as a build artifact. A fresh checkout has
# none, so the app fails to compile with "library.h: No such file or directory".
# For the cross-build we only need ereader_shelf to COMPILE, not to embed real
# books, and regenerating from LFS sources needs git-lfs + Pillow that CI lacks.
# So emit a 0-book stub (with the same struct/symbols) when the real header is
# absent; `just tools::books` still produces the full library for HIL / deployment.
need_shelf=1
if ((selected_mode)); then
  need_shelf=0
  for selected_app in "${RA8_APP_BATCH_ITEMS[@]}"; do
    [[ "$selected_app" == *"::ereader_shelf" ]] && need_shelf=1
  done
fi
shelf_lib="examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/inc/library.h"
if ((need_shelf)) && [ ! -f "$shelf_lib" ]; then
  echo "build_all: emitting a 0-book stub $shelf_lib (run 'just tools::books' for the full library)"
  cat >"$shelf_lib" <<'STUB'
/**
 * @file library.h
 * @generated build_all_examples.sh stub -- do not edit by hand.
 * @brief Stub baked book library (0 books) emitted by build_all_examples.sh.
 * @details ereader_shelf includes this generated header. The real version
 *          (full .rabook blobs + pre-decoded cover thumbnails) is produced by
 *          tools/epub_compile/src/bake_library.py via `just tools::books`. This
 *          0-book stub lets the
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
  k_library_count = 1U, /**< Number of entries in k_library. */
} library_count_t;

/** @brief Baked book table; a single empty placeholder (cross-build is
 *         compile-only, so the null blob is never opened at runtime). */
static const library_book_t k_library[k_library_count] = {
    {nullptr, 0U, nullptr, 0U, 0U, "", ""},
};
STUB
fi

apps=()
if ((selected_mode)); then
  apps=("${RA8_APP_BATCH_ITEMS[@]}")
else
  # One authority defines what the execution matrix builds. The independent
  # structural union checker intentionally does not import this helper.
  while IFS= read -r -d '' config; do
    apps+=("$config")
  done < <(python3 "$REPO_ROOT/scripts/dev/ra8_apps.py" matrix --nul)
fi

if [ "${#apps[@]}" -eq 0 ]; then
  echo "error: the canonical firmware build matrix is empty" >&2
  exit 1
fi

# Sort the configurations alphabetically for stable output.
#
# LC_ALL=C is load-bearing, not cosmetic. Sharding (below) slices this list by
# index, so every shard must agree byte-for-byte on the ORDER or an app is
# built twice while another is never built at all -- and the union check would
# be the only thing standing between that and a green run. Locale-dependent
# collation across heterogeneous runners is exactly how that drift happens.
sorted_apps=()
while IFS= read -r line; do
  sorted_apps+=("$line")
done < <(printf '%s\n' "${apps[@]}" | LC_ALL=C sort)
apps=("${sorted_apps[@]}")

if ((selected_mode)); then
  LOG_DIR="$REPO_ROOT/build/build_selected_apps"
else
  LOG_DIR="$REPO_ROOT/build/build_all_examples"
fi
mkdir -p "$LOG_DIR"

# --- sharding -------------------------------------------------------------
# RA8_BUILD_SHARDS=N + RA8_BUILD_SHARD=K (1-based) build only this shard's
# slice. Both unset is the default and builds everything, so the local suite
# (`just quality::native`, `just build_all`) is bit-for-bit unchanged.
#
# The slice is a STRIDE (i % N == K-1), never a contiguous block. Per-app cost
# is wildly uneven -- the ~8 shared-archive-ineligible apps (TrustZone, RA8P1)
# compile all ~180 library sources from source while an eligible app compiles
# ~16 TUs -- and alphabetical order clusters related heavy apps together
# (threadx_*, usb_*, npu_*). A contiguous split would hand one shard most of
# the expensive apps; a stride interleaves them.
#
# The manifests written here are what scripts/checks/check_build_shard_union.py
# reads to PROVE the shards covered every app exactly once. A shard that
# silently built nothing leaves an empty manifest and fails that check rather
# than passing quietly.
shard_n="${RA8_BUILD_SHARDS:-1}"
shard_k="${RA8_BUILD_SHARD:-1}"
case "$shard_n" in
  '' | *[!0-9]*)
    echo "error: RA8_BUILD_SHARDS must be a positive integer, got '$shard_n'" >&2
    exit 1
    ;;
esac
case "$shard_k" in
  '' | *[!0-9]*)
    echo "error: RA8_BUILD_SHARD must be a positive integer, got '$shard_k'" >&2
    exit 1
    ;;
esac
if [ "$shard_n" -lt 1 ] || [ "$shard_k" -lt 1 ] || [ "$shard_k" -gt "$shard_n" ]; then
  echo "error: need 1 <= RA8_BUILD_SHARD ($shard_k) <= RA8_BUILD_SHARDS ($shard_n)" >&2
  exit 1
fi
if ((selected_mode)) && { [[ "$shard_n" -ne 1 ]] || [[ "$shard_k" -ne 1 ]]; }; then
  echo "error: selected-app builds cannot also be sharded" >&2
  exit 2
fi
# The full discovered set, written identically by every shard. The union
# checker re-derives the structure independently and cross-checks, so a shard
# cannot vouch for its own idea of what the tree contains.
if ((!selected_mode)); then
  SHARD_DIR="$LOG_DIR/.shard"
  mkdir -p "$SHARD_DIR"
  printf '%s\n' "${apps[@]}" >"$SHARD_DIR/all-configs.txt"
fi
if ((!selected_mode)) && [ "$shard_n" -gt 1 ]; then
  sharded=()
  for i in "${!apps[@]}"; do
    if [ "$((i % shard_n))" -eq "$((shard_k - 1))" ]; then
      sharded+=("${apps[$i]}")
    fi
  done
  if [ "${#sharded[@]}" -eq 0 ]; then
    echo "error: shard $shard_k/$shard_n selected 0 of ${#apps[@]} apps -- more" >&2
    echo "       shards than apps is a scheduling bug, not an empty build." >&2
    exit 1
  fi
  apps=("${sharded[@]}")
  echo "==> shard $shard_k/$shard_n: ${#apps[@]} of $(wc -l <"$SHARD_DIR/all-configs.txt" | tr -d ' ') configurations"
fi
if ((!selected_mode)); then
  printf '%s\n' "${apps[@]}" >"$SHARD_DIR/shard-${shard_k}-of-${shard_n}.txt"
fi

# Per-app exit codes are collected in a status directory: each worker writes
# $STATUS_DIR/<app> containing the app build's return code. Completion order
# under the parallel pool is nondeterministic, so the summary is reconstructed
# from these files and re-sorted alphabetically (same order as the serial run).
STATUS_DIR="$LOG_DIR/.status"
rm -rf "$STATUS_DIR"
mkdir -p "$STATUS_DIR"

# Worker pool size: one `just apps::build <app>` per worker, each build
# SINGLE-THREADED.
# The across-app pool is the only parallelism -- we deliberately do NOT also
# pass a per-app --parallel, which would oversubscribe (jobs x jobs). The pool
# width is the canonical bounded value (RA8_MAX_JOBS / CMAKE_BUILD_PARALLEL_LEVEL
# / nproc); MAX_JOBS is still honoured as this script's own explicit override.
MAX_JOBS="${MAX_JOBS:-$(ra8_max_jobs)}"
# Guard against a non-numeric / zero override.
case "$MAX_JOBS" in
  '' | *[!0-9]*) MAX_JOBS=1 ;;
esac
if [ "$MAX_JOBS" -lt 1 ]; then
  MAX_JOBS=1
fi

echo "==> Building ${#apps[@]} firmware configurations (pool: $MAX_JOBS parallel jobs)"
echo

# Build one app through the repository's authoritative Just recipe, tee the per-app log exactly
# as the serial version did, and record the exit code in the status directory.
# Runs in a subshell spawned by xargs, so it must re-derive its environment.
# shellcheck disable=SC2329  # exported and invoked by the bounded worker pool
build_one_app() {
  local record="$1" separator=$'\034' index app app_name log_file status_file rc
  index="${record%%"$separator"*}"
  app="${record#*"$separator"}"
  app_name="$(python3 "$REPO_ROOT/scripts/dev/ra8_apps.py" name "$app")" || return 2
  printf -v index '%04d' "$index"
  log_file="$LOG_DIR/${index}-${app_name}.log"
  status_file="$STATUS_DIR/$index"
  if /bin/bash -p "$REPO_ROOT/scripts/dev/run_just.sh" apps::build "$app" "$BUILD_TYPE" >"$log_file" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  printf '%s\n' "$rc" >"$status_file"
  if [ "$rc" -eq 0 ]; then
    printf "  [build] %-40s ... PASS\n" "$app"
  else
    printf "  [build] %-40s ... FAIL (rc=%s, log: %s)\n" "$app" "$rc" "$log_file"
  fi
}
export -f build_one_app
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
export LOG_DIR STATUS_DIR REPO_ROOT BUILD_TYPE

# Warm the one shared prerequisite BEFORE the pool starts.
echo "==> Warming shared build/compile_commands.json (serial, pre-pool)"
if ! cmake -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchain-ra8d2.cmake" -B "$REPO_ROOT/build" "$REPO_ROOT" >"$LOG_DIR/compile_commands.log" 2>&1; then
  echo "error: failed to generate build/compile_commands.json (log: $LOG_DIR/compile_commands.log)" >&2
  exit 1
fi

# Prebuild the universal first-party library archive ONCE, then hand every
# eligible per-app build a pointer to it so it links the archive instead of
# recompiling ~180 identical library sources (ra8_core / ra8_hal / ra8_net_pal
# / ra8_usb_pal / board / ra8_secure_app) into its own ELF. That redundant
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

# Bounded worker pool across apps. Records are NUL-delimited so selected IDs are
# each one argv even if a future valid identifier contains whitespace. The
# shared dispatcher falls back to a strict serial loop if xargs -P is absent.
records=()
for i in "${!apps[@]}"; do
  records+=("$i"$'\034'"${apps[$i]}")
done
printf '%s\0' "${records[@]}" |
  ra8_app_batch_dispatch_nul "$MAX_JOBS" bash -c 'build_one_app "$@"' _

# Reconstruct results in the original alphabetical app order from status files.
results=()
fail_count=0
for i in "${!apps[@]}"; do
  app="${apps[$i]}"
  printf -v status_slot '%04d' "$i"
  status_file="$STATUS_DIR/$status_slot"
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
printf " %-56s  %-8s\n" "Configuration" "Status"
printf " %-56s  %-8s\n" "--------------------------------------------------------" "--------"
for i in "${!apps[@]}"; do
  printf " %-56s  %-8s\n" "${apps[$i]}" "${results[$i]}"
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

#!/usr/bin/env bash
#
# scripts/build_all_examples.sh -- build every examples/<app> target.
#
# Iterates each top-level examples/<app>/ directory containing a main.c,
# invokes `make <app>` from the repo root, captures pass/fail per app,
# prints a summary table at the end, and exits non-zero on any failure.
#
# Usage:
#   bash scripts/build_all_examples.sh
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

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
    cat > "$shelf_lib" <<'STUB'
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
IFS=$'\n' apps=($(printf '%s\n' "${apps[@]}" | sort)) ; unset IFS

LOG_DIR="$REPO_ROOT/build/build_all_examples"
mkdir -p "$LOG_DIR"

echo "==> Building ${#apps[@]} apps from examples/"
echo

results=()
status_codes=()
fail_count=0

for app in "${apps[@]}"; do
    log_file="$LOG_DIR/${app}.log"
    printf "  [build] %-40s ... " "$app"
    if make "$app" >"$log_file" 2>&1; then
        results+=("PASS")
        status_codes+=(0)
        echo "PASS"
    else
        rc=$?
        results+=("FAIL")
        status_codes+=("$rc")
        fail_count=$((fail_count + 1))
        echo "FAIL (rc=$rc, log: $log_file)"
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

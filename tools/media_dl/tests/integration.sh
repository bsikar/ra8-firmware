#!/usr/bin/env bash
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# media_dl end-to-end integration harness. For EVERY export format it runs the
# full pipeline the reader depends on -- package a folder of pages into that
# format, then open the result in the native ra8_viewer headless and assert it
# renders a non-blank frame. This is the test that catches the class of bug the
# unit tests miss: an archive that writes fine but the viewer cannot open (the
# 0x107 open/render failures). Pages are synthetic and non-copyright (gen_pages.py),
# so the harness needs no network and no checked-in media.
#
# Exit status is the number of FAILED formats (0 = all good). Formats whose
# optional external tool is absent (rar for cbr) are SKIPPED, not failed.
#
# Usage: tools/media_dl/tests/integration.sh [--no-build] [FORMAT ...]
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MDL_DIR="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$MDL_DIR/../.." && pwd)"
VIEWER_DIR="$ROOT/tools/ra8_viewer"

MEDIA_DL="$MDL_DIR/build/media_dl"
VIEWER="$VIEWER_DIR/build/ra8_viewer"

WORK="$MDL_DIR/build/integration"
PAGES="$WORK/pages"
PPMDIR="$WORK/ppm"

BUILD=1
FORMATS=""
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    *) FORMATS="$FORMATS $arg" ;;
  esac
done
[ -n "$FORMATS" ] || FORMATS="cbz cbt cbt.gz cbt.xz cbr epub jof rabook"

say() { printf '%s\n' "$*"; }
rule() { printf '%s\n' "------------------------------------------------------------"; }

# --- build the two tools under test -----------------------------------------
if [ "$BUILD" -eq 1 ]; then
  say "== building media_dl + ra8_viewer =="
  cmake -B "$MDL_DIR/build" -S "$MDL_DIR" >/dev/null || {
    say "media_dl configure FAILED"
    exit 99
  }
  cmake --build "$MDL_DIR/build" -j >/dev/null || {
    say "media_dl build FAILED"
    exit 99
  }
  cmake -B "$VIEWER_DIR/build" -S "$VIEWER_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null ||
    {
      say "viewer configure FAILED"
      exit 99
    }
  cmake --build "$VIEWER_DIR/build" -j >/dev/null || {
    say "viewer build FAILED"
    exit 99
  }
fi
[ -x "$MEDIA_DL" ] || {
  say "missing $MEDIA_DL (build first)"
  exit 99
}
[ -x "$VIEWER" ] || {
  say "missing $VIEWER (build first)"
  exit 99
}

# --- synthetic, non-copyright pages -----------------------------------------
rm -rf "$WORK"
mkdir -p "$PAGES" "$PPMDIR"
python3 "$HERE/gen_pages.py" "$PAGES" 6 800 1200 || {
  say "gen_pages FAILED"
  exit 99
}

# `--pack` writes the archive as <abs(dir)>.<ext>; jof instead writes one
# page_NNNN.jof sibling per page INTO the dir. Report the artifact the viewer
# should open for a given format+dir.
artifact_for() {
  case "$1" in
    jof) printf '%s/page_0001.jof' "$2" ;;
    *) printf '%s.%s' "$2" "$1" ;;
  esac
}

pass=0
fail=0
skip=0
failed_list=""
for fmt in $FORMATS; do
  rule
  # Skip formats whose optional external tool is absent (documented as optional).
  case "$fmt" in
    cbr) command -v rar >/dev/null 2>&1 || {
      say "SKIP  $fmt  (needs 'rar' on PATH)"
      skip=$((skip + 1))
      continue
    } ;;
    cbt.xz) command -v xz >/dev/null 2>&1 || {
      say "SKIP  $fmt  (needs 'xz' on PATH)"
      skip=$((skip + 1))
      continue
    } ;;
    rabook) python3 -c 'import PIL' >/dev/null 2>&1 || {
      say "SKIP  $fmt  (needs python3 + Pillow)"
      skip=$((skip + 1))
      continue
    } ;;
  esac

  # Each format gets a private copy so jof's .jof siblings never leak into
  # another format's archive (list_pages() globs every non-hidden file).
  fdir="$WORK/in_${fmt//./_}"
  rm -rf "$fdir"
  cp -R "$PAGES" "$fdir"

  # 1) package
  if ! "$MEDIA_DL" --pack "$fdir" --format "$fmt" >"$WORK/pack_${fmt//./_}.log" 2>&1; then
    say "FAIL  $fmt  (pack)"
    sed 's/^/        /' "$WORK/pack_${fmt//./_}.log"
    fail=$((fail + 1))
    failed_list="$failed_list $fmt(pack)"
    continue
  fi
  art="$(artifact_for "$fmt" "$fdir")"
  if [ ! -s "$art" ]; then
    say "FAIL  $fmt  (no artifact at $art)"
    fail=$((fail + 1))
    failed_list="$failed_list $fmt(artifact)"
    continue
  fi

  # 2) open in the viewer, headless, and dump the first page
  ppm="$PPMDIR/${fmt//./_}.ppm"
  if ! "$VIEWER" "$art" --headless --dump-ppm "$ppm" >"$WORK/view_${fmt//./_}.log" 2>&1; then
    say "FAIL  $fmt  (viewer open/render)"
    sed 's/^/        /' "$WORK/view_${fmt//./_}.log"
    fail=$((fail + 1))
    failed_list="$failed_list $fmt(view)"
    continue
  fi

  # 3) assert the render is non-blank
  if ! python3 "$HERE/check_ppm.py" "$ppm" >"$WORK/ppm_${fmt//./_}.log" 2>&1; then
    say "FAIL  $fmt  (blank render)"
    sed 's/^/        /' "$WORK/ppm_${fmt//./_}.log"
    fail=$((fail + 1))
    failed_list="$failed_list $fmt(blank)"
    continue
  fi
  say "PASS  $fmt  ->  $(basename "$art")  ($(sed -n 's/.*(\(.*\))/\1/p' "$WORK/ppm_${fmt//./_}.log"))"
  pass=$((pass + 1))
done

rule
say "media_dl integration: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ] || say "FAILED:$failed_list"
exit "$fail"

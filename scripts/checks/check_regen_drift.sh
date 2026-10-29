#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/check_regen_drift.sh -- regeneration drift gate for the kept
# generators (RA8FW-1001, child of RA8FW-343).
#
# Every kept generator must regenerate its committed output byte-identically.
# This gate re-runs each registered generator against a throwaway worktree of
# the current HEAD and fails when the regeneration leaves any tracked path
# modified: the committed copy no longer matches what the generator produces,
# so either the output is stale (regenerate and commit) or the generator is
# not reproducible across hosts (that is a finding, not a skip).
#
# The gate runs every generator under the repository's locked Python
# environment (RA8_TOOL_VENV when set, else the repo-local .venv). It refuses
# to run on a bare interpreter: Pillow wheels bundle their own FreeType, so
# glyph rasterization is only byte-stable inside the locked toolchain
# (uv.lock pins the versions). A missing environment is FATAL rather than a
# skipped gate -- exiting 0 here would report byte-identity for nothing,
# which is exactly the vacuous green the gate-honesty epic exists to prevent.
#
#     bash scripts/checks/check_regen_drift.sh
#     bash scripts/checks/check_regen_drift.sh --only pinouts widget_icons
#
# Not registered here, with reasons (it is a kept generator but cannot
# regenerate on a bare checkout):
#   scripts/gen/ereader_golden.py -- needs a cross-built ereader_ui.elf and
#     the ra8_emulator binary; its byte-identity gate is the golden check in
#     scripts/emu/smoke_run.sh, which runs it against the built pair.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"

die() {
  echo "regen-drift: $*" >&2
  exit 2
}

# ---- locked Python environment resolution (fail-closed) --------------------
python_bin=""
if [[ -n "${RA8_TOOL_VENV:-}" ]]; then
  python_bin="$(/usr/bin/python3 -I "$SCRIPT_DIR/../dev/managed_python_env.py" verify \
    --env "${RA8_TOOL_VENV%/}" --pyproject "$ROOT/pyproject.toml" \
    --lock "$ROOT/uv.lock" --group ci --print-bin 2>/dev/null)" ||
    die "RA8_TOOL_VENV=$RA8_TOOL_VENV does not verify against pyproject.toml/uv.lock"
elif [[ -x "$ROOT/.venv/bin/python3" ]]; then
  python_bin="$ROOT/.venv/bin/python3"
else
  die "no locked Python environment found. Run scripts/dev/setup_python.sh (or set RA8_TOOL_VENV). Refusing to run the generators on a bare interpreter: Pillow bundles its own FreeType, so regeneration is only byte-stable inside the locked toolchain."
fi

# ---- the kept generators, name -> canonical command ------------------------
# Each entry runs with the repo root as its working directory inside the
# throwaway worktree. Paths the generator writes are committed paths; the
# gate requires git status to stay empty afterwards.
declare -a GEN_NAMES=(
  pinouts
  widget_icons
  literata_atlas
  bluenoise_mask
  chapter_map
  comic_fixture
  comic_large_fixture
  manga_fixture
  unarch_xz_fixture
  rabook_gray8_fixture
  driver_status
  roadmap_stats
  sbom_check
  rabook_parity
  display_atlases
)
case "${1:-}" in
  --only) shift; GEN_NAMES=("$@") ;;
  ""|--) ;;
  *) die "usage: check_regen_drift.sh [--only NAME ...]" ;;
esac

gen_cmd() {
  case "$1" in
    pinouts)             "$python_bin" scripts/gen/gen_pinouts.py ;;
    widget_icons)        "$python_bin" scripts/gen/widget_icons.py ;;
    literata_atlas)      "$python_bin" scripts/gen/literata_atlas.py ;;
    bluenoise_mask)      "$python_bin" scripts/gen/gen_bluenoise_mask.py ;;
    chapter_map)         bash scripts/gen/build_chapter_map.sh ;;
    comic_fixture)       "$python_bin" scripts/gen/gen_comic_fixture.py ;;
    comic_large_fixture) "$python_bin" scripts/gen/gen_comic_large_fixture.py ;;
    manga_fixture)       "$python_bin" scripts/gen/gen_manga_page_fixture.py ;;
    unarch_xz_fixture)   "$python_bin" scripts/gen/gen_unarch_xz_fixture.py ;;
    rabook_gray8_fixture) "$python_bin" scripts/gen/rabook_gray8_fixture.py ;;
    driver_status)       "$python_bin" scripts/gen/gen_driver_status.py ;;
    # Its own --check mode is the byte-identity check (RA8FW-335): refuse a
    # stale summary block in docs/ROADMAP.md.
    roadmap_stats)       bash scripts/builders/roadmap_stats.sh --check ;;
    # Same pattern (RA8FW-340): the SBOM's --check mode fails when the
    # committed SBOM is stale or the tree drifted.
    sbom_check)          "$python_bin" scripts/gen/gen_sbom.py --check ;;
    # The four just/tools.just rabook_parity modes, generator then
    # clang-format, exactly as the rabook_golden recipe runs them. The
    # formatter is load-bearing for byte-identity (it re-wraps the hex
    # tables); no formatter is a FAIL, not a skip.
    rabook_parity)
      formatter=""
      if command -v clang-format-22 >/dev/null 2>&1; then
        formatter="$(command -v clang-format-22)"
      elif command -v clang-format >/dev/null 2>&1; then
        formatter="$(command -v clang-format)"
      else
        die "rabook_parity needs clang-format-22 or clang-format for byte-identity (the just recipe formats every generated fixture)"
      fi
      fixture_dir="apps/shared_libs/rabook_compile/tests/inc"
      "$python_bin" scripts/gen/rabook_parity_gen.py \
        tests/fixtures/rabook_parity \
        "$fixture_dir/rabook_parity_fixture.h" \
        examples/ek_ra8d2/hw_pending/compile_on_m33/inc/parity_fixture.h
      "$python_bin" scripts/gen/rabook_parity_gen.py --realbook \
        tests/fixtures/rabook_realbook \
        "$fixture_dir/rabook_realbook_fixture.h"
      "$python_bin" scripts/gen/rabook_parity_gen.py --downscale \
        "$fixture_dir/rabook_downscale_parity_fixture.h"
      "$python_bin" scripts/gen/rabook_parity_gen.py --color \
        "$fixture_dir/rabook_color_parity_fixture.h"
      "$formatter" -i \
        "$fixture_dir/rabook_parity_fixture.h" \
        examples/ek_ra8d2/hw_pending/compile_on_m33/inc/parity_fixture.h \
        "$fixture_dir/rabook_realbook_fixture.h" \
        "$fixture_dir/rabook_downscale_parity_fixture.h" \
        "$fixture_dir/rabook_color_parity_fixture.h" ;;
    # Registered last and expected to be named in the findings: its
    # rasterization is host-dependent (FreeType inside the Pillow wheel), so
    # a byte-identity failure here is a finding for the rasterizer decision,
    # not stale output.
    display_atlases)     "$python_bin" scripts/gen/display_atlases.py ;;
    *) die "unknown generator $1" ;;
  esac
}

work="$(mktemp -d "${TMPDIR:-/tmp}/regen-drift.XXXXXX")"
cleanup() { git worktree remove --force "$work/wt" >/dev/null 2>&1 || true; rm -rf "$work"; }
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$work/wt" HEAD >/dev/null || die "could not create the throwaway worktree"

drift_total=0
fail_total=0
for name in "${GEN_NAMES[@]}"; do
  git -C "$work/wt" reset -q --hard HEAD
  git -C "$work/wt" clean -qfd
  if ! (cd "$work/wt" && gen_cmd "$name") >"$work/$name.log" 2>&1; then
    echo "FAIL   $name (generator exited nonzero; log at $work/$name.log)"
    fail_total=$((fail_total + 1))
    continue
  fi
  status="$(git -C "$work/wt" status --porcelain)"
  if [[ -z "$status" ]]; then
    echo "PASS   $name"
  else
    echo "DRIFT  $name -- regeneration differs from the committed output:"
    printf '%s\n' "$status" | sed 's/^/         /'
    drift_total=$((drift_total + 1))
  fi
done

echo "regen-drift: $drift_total drifting, $fail_total failed, $(( ${#GEN_NAMES[@]} - drift_total - fail_total )) byte-identical of ${#GEN_NAMES[@]} generators"
[[ $drift_total -eq 0 && $fail_total -eq 0 ]]

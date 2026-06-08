#!/usr/bin/env bash
#
# scripts/ui_render_check.sh -- render the shared bedroom UI at every panel
# size and assert it still FILLS the panel (no regression to clipping).
#
# The bedroom UI (examples/shared/bedroom_ui) is resolution-adaptive: the same
# source that fills the 1024x600 EK-RA8D2 panel must also lay out cleanly on a
# 480x272 or 800x480 display -- tab bar, heading, and the 2x2 card grid all on
# screen. This gate proves that. It builds tools/simulator headless, then for
# every tools/simulator/panels/*.toml and tabs 0/1/2 it runs
# `sim --panel <toml> --png <out> --tab <n>` and checks the PPM:
#
#   * a non-trivial number of distinct colors overall (a blank / failed render
#     has one or two colors), AND
#   * content present in ALL FOUR quadrants (top-left / top-right / bottom-left
#     / bottom-right). The old, pre-adaptive layout crammed everything into the
#     top-left and left the other three quadrants as bare background -- this
#     quadrant check is what catches that clipping regression.
#
# It asserts pixel CONTENT, complementing scripts/board_sim_smoke.sh (which is
# the boot/run smoke layer on the CPU emulator). Exits non-zero on any failure.
#
#   scripts/ui_render_check.sh                 # all panels under panels/
#   scripts/ui_render_check.sh ek_ra8d2 sample_800x480   # explicit basenames
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
sim_dir="$ROOT/tools/simulator"

# Render-fill thresholds. The adaptive UI produces hundreds of distinct colors
# overall and roughly eighty per quadrant; these floors sit far below that yet
# far above a blank frame or a top-left-clipped one (empty other quadrants).
min_total_colors=16
min_quadrant_colors=8

# Tabs the bedroom UI exposes (Primary / Secondary 1 / Secondary 2).
tabs=(0 1 2)

# Panel basenames: explicit args, else every descriptor under panels/.
panels=("$@")
if [ "${#panels[@]}" -eq 0 ]; then
    for toml in "$sim_dir"/panels/*.toml; do
        panels+=("$(basename "$toml" .toml)")
    done
fi

echo "ui_render_check: building the simulator (headless) ..."
# Bound parallelism to the CPU count. The simulator pulls in the whole GUIX
# core (hundreds of TUs); an unbounded -j can exhaust the per-user process
# limit on some hosts. Fall back to a single job if the CPU count is unknown.
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
[ -n "$jobs" ] || jobs=1
cmake -B "$sim_dir/build" -S "$sim_dir" >/dev/null
cmake --build "$sim_dir/build" -j "$jobs" >/dev/null
sim="$sim_dir/build/sim"

# Analyse one PPM: parse the P6 header, count distinct colors overall and in
# each of the four quadrants, and fail if any floor is not met. Self-contained
# (Python 3 only) so the gate runs identically on macOS and the Linux runner.
check_render() {
    python3 - "$1" "$min_total_colors" "$min_quadrant_colors" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
min_total = int(sys.argv[2])
min_quad = int(sys.argv[3])

data = path.read_bytes()
if data[:2] != b"P6":
    print("not a P6 PPM", file=sys.stderr)
    sys.exit(1)

# Read width, height, maxval -- three integer tokens after the magic, honouring
# '#' comments and any whitespace run between fields.
idx = 2
toks = []
while len(toks) < 3:
    while idx < len(data) and data[idx:idx + 1].isspace():
        idx += 1
    if data[idx:idx + 1] == b"#":
        while idx < len(data) and data[idx:idx + 1] != b"\n":
            idx += 1
        continue
    start = idx
    while idx < len(data) and not data[idx:idx + 1].isspace():
        idx += 1
    toks.append(int(data[start:idx]))
idx += 1  # exactly one whitespace byte separates maxval from the pixel data
width, height, _maxval = toks

pixels = data[idx:idx + width * height * 3]
if len(pixels) < width * height * 3:
    print("truncated pixel data", file=sys.stderr)
    sys.exit(1)


def distinct(x0, y0, x1, y1):
    seen = set()
    for y in range(y0, y1):
        base = (y * width + x0) * 3
        for x in range(x1 - x0):
            off = base + x * 3
            seen.add(pixels[off:off + 3])
    return len(seen)


mx, my = width // 2, height // 2
total = distinct(0, 0, width, height)
quads = {
    "top-left": distinct(0, 0, mx, my),
    "top-right": distinct(mx, 0, width, my),
    "bottom-left": distinct(0, my, mx, height),
    "bottom-right": distinct(mx, my, width, height),
}

problems = []
if total < min_total:
    problems.append("only %d distinct colors (need >= %d)" % (total, min_total))
for name, count in quads.items():
    if count < min_quad:
        problems.append("%s nearly empty: %d colors (need >= %d)" % (name, count, min_quad))

if problems:
    print("; ".join(problems), file=sys.stderr)
    sys.exit(1)

print("colors=%d quads=tl:%d,tr:%d,bl:%d,br:%d"
      % (total, quads["top-left"], quads["top-right"],
         quads["bottom-left"], quads["bottom-right"]))
sys.exit(0)
PY
}

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

fail=0
for panel in "${panels[@]}"; do
    toml="$sim_dir/panels/$panel.toml"
    if [ ! -f "$toml" ]; then
        printf '  %-22s NO PANEL (%s)\n' "$panel" "$toml"
        fail=1
        continue
    fi
    for tab in "${tabs[@]}"; do
        label="$panel tab$tab"
        ppm="$out_dir/${panel}_tab${tab}.ppm"
        if ! "$sim" --panel "$toml" --png "$ppm" --tab "$tab" >/dev/null 2>&1; then
            printf '  %-22s RENDER FAIL\n' "$label"
            fail=1
            continue
        fi
        if report="$(check_render "$ppm" 2>&1)"; then
            printf '  %-22s OK (%s)\n' "$label" "$report"
        else
            printf '  %-22s FILL FAIL (%s)\n' "$label" "$report"
            fail=1
        fi
    done
done

if [ "$fail" -eq 0 ]; then
    echo "ui_render_check: ALL PASS"
    exit 0
fi
echo "ui_render_check: FAILURES"
exit 1

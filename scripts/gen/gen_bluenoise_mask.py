#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the void-and-cluster blue-noise dither threshold mask (#477).

The 16-level e-ink panel (Waveshare 6inch HD, IT8951, 4 bpp) shows 16 gray
levels; source figures carry 256. ``libs/ra8_gfx/src/ra8_gfx_dither.c`` breaks
up the 256->16 banding by thresholding each pixel against a precomputed
blue-noise mask indexed purely by ``(x, y)`` position plus the pixel's own
value -- no neighbour state, so the dither is independent per pixel/tile
(seamless across tile boundaries), fully deterministic (ra8_emulator byte ==
silicon, the EIL==HIL rule), and SIMD-friendly. This script bakes that mask as
a committed C table, following the ``rabook_parity_gen.py`` /
``rabook_gray8_fixture.py`` generate-and-commit pattern.

Method: Ulichney's void-and-cluster algorithm ("The void-and-cluster method for
dither array generation", Robert Ulichney, Proc. SPIE 1913, 1993). A small
homogeneous "prototype" binary pattern is grown/shrunk one pixel at a time,
always filling the largest void or breaking the tightest cluster (measured by a
toroidal Gaussian filter), which assigns every cell a rank whose level sets are
blue-noise distributed. The ranks become an 8-bit threshold texture.

Determinism: the initial minority pixels come from ``random.Random(_SEED)``
(Mersenne Twister, stable across CPython releases) and the Gaussian filter uses
an INTEGER fixed-point kernel, so every argmin/argmax is an exact integer
comparison -- the emitted bytes are identical on any host, no floating-point
tie-breaking. Re-running the script reproduces the committed header byte for
byte.

Usage:
    python3 scripts/gen/gen_bluenoise_mask.py            # rewrite the header
    python3 scripts/gen/gen_bluenoise_mask.py --check    # fail if it drifted
    make bluenoise-mask-update                           # the same, via make
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from pathlib import Path

# --- mask geometry ---------------------------------------------------------
# 64x64 (power-of-two edge so the runtime index is a bitmask, not a modulo).
# 4096 bytes of .rodata: bounded, small, and a tiling period the eye does not
# resolve on a 1448x1072 panel.
_DIM = 64
_N = _DIM * _DIM

# --- void-and-cluster tuning ----------------------------------------------
_SEED = 0x5747_4E42  # "WGNB" -- fixed so the initial pattern is reproducible.
_SIGMA = 1.9  # Gaussian filter std-dev (Ulichney: ~1.5-2.0).
_RADIUS = 6  # Kernel truncation radius (~3*sigma); 13x13 support.
_KERNEL_SCALE = 4096  # Fixed-point weight scale -> integer energy field.
_INIT_FRACTION = 10  # Prototype minority density: 1/_INIT_FRACTION of cells.

# --- emitted-table formatting ---------------------------------------------
_BYTE_LEVELS = 256  # Threshold texture depth (8-bit).
_ROW_BYTES = 16  # Byte literals per emitted C source row (clang-format fit).

_OUT = (
    Path(__file__).resolve().parents[2]
    / "libs"
    / "ra8_gfx"
    / "src"
    / "ra8_gfx_dither_mask_internal.h"
)


def _wrap(value: int) -> int:
    """Reduce a coordinate onto the toroidal ``[0, _DIM)`` mask edge."""
    return value % _DIM


def _build_kernel() -> list[tuple[int, int, int]]:
    """Return the integer Gaussian filter as ``(dx, dy, weight)`` offsets.

    Weights are ``round(_KERNEL_SCALE * exp(-(dx^2+dy^2)/(2*sigma^2)))`` over
    the truncated ``[-_RADIUS, _RADIUS]`` square, dropping zero-weight cells.
    Keeping them integer makes the energy field exact, so no float rounding can
    flip an argmin tie between platforms.
    """
    kernel: list[tuple[int, int, int]] = []
    denom = 2.0 * _SIGMA * _SIGMA
    for dy in range(-_RADIUS, _RADIUS + 1):
        for dx in range(-_RADIUS, _RADIUS + 1):
            weight = round(_KERNEL_SCALE * math.exp(-((dx * dx) + (dy * dy)) / denom))
            if weight > 0:
                kernel.append((dx, dy, weight))
    return kernel


def _toggle(energy: list[int], kernel: list[tuple[int, int, int]], pos: int, sign: int) -> None:
    """Add (``sign=+1``) or remove (``sign=-1``) one minority pixel at ``pos``.

    Splats the Gaussian kernel, wrapped toroidally, onto the energy field so
    ``energy[q]`` always equals the filtered concentration of minority pixels
    at ``q``.
    """
    px = pos % _DIM
    py = pos // _DIM
    for dx, dy, weight in kernel:
        qx = _wrap(px + dx)
        qy = _wrap(py + dy)
        energy[(qy * _DIM) + qx] += sign * weight


def _tightest_cluster(energy: list[int], pattern: list[bool]) -> int:
    """Index of the set (``True``) cell with the highest surrounding energy."""
    best = -1
    best_energy = 0
    for pos in range(_N):
        if pattern[pos] and ((best < 0) or (energy[pos] > best_energy)):
            best = pos
            best_energy = energy[pos]
    return best


def _largest_void(energy: list[int], pattern: list[bool]) -> int:
    """Index of the clear (``False``) cell with the lowest surrounding energy."""
    best = -1
    best_energy = 0
    for pos in range(_N):
        if (not pattern[pos]) and ((best < 0) or (energy[pos] < best_energy)):
            best = pos
            best_energy = energy[pos]
    return best


def _initial_pattern(kernel: list[tuple[int, int, int]]) -> tuple[list[bool], list[int]]:
    """Build a homogeneous prototype binary pattern and its energy field.

    Seeds ``_N // _INIT_FRACTION`` minority pixels at random, then repeatedly
    moves the tightest cluster's pixel into the largest void until the two
    coincide -- the fixed point at which the pattern is maximally homogeneous.
    """
    rng = random.Random(_SEED)  # noqa: S311 -- non-crypto: deterministic mask seed
    ones = _N // _INIT_FRACTION
    chosen = rng.sample(range(_N), ones)
    pattern = [False] * _N
    energy = [0] * _N
    for pos in chosen:
        pattern[pos] = True
        _toggle(energy, kernel, pos, 1)

    # NASA-style bounded loop: homogenisation cannot need more swaps than cells.
    for _ in range(_N):
        cluster = _tightest_cluster(energy, pattern)
        pattern[cluster] = False
        _toggle(energy, kernel, cluster, -1)
        void = _largest_void(energy, pattern)
        pattern[void] = True
        _toggle(energy, kernel, void, 1)
        if void == cluster:
            break
    return pattern, energy


def _rank_matrix() -> list[int]:
    """Run the three void-and-cluster phases and return per-cell ranks 0.._N-1."""
    kernel = _build_kernel()
    prototype, proto_energy = _initial_pattern(kernel)
    ones = sum(1 for cell in prototype if cell)
    ranks = [0] * _N

    # Phase 1: rank the prototype's minority pixels, tightest cluster last.
    pattern = list(prototype)
    energy = list(proto_energy)
    for rank in range(ones - 1, -1, -1):
        cluster = _tightest_cluster(energy, pattern)
        ranks[cluster] = rank
        pattern[cluster] = False
        _toggle(energy, kernel, cluster, -1)

    # Phases 2+3: fill from the prototype, always into the largest void. Because
    # filtering the clear cells is the complement of filtering the set cells,
    # "largest void" and "tightest cluster of the majority" are the same pick,
    # so one loop covers both halves symmetrically.
    pattern = list(prototype)
    energy = list(proto_energy)
    for rank in range(ones, _N):
        void = _largest_void(energy, pattern)
        ranks[void] = rank
        pattern[void] = True
        _toggle(energy, kernel, void, 1)
    return ranks


def _threshold_texture() -> list[int]:
    """Map ranks 0.._N-1 to centred 8-bit thresholds uniform over [0, 255]."""
    ranks = _rank_matrix()
    texture: list[int] = []
    for rank in ranks:
        level = (((2 * rank) + 1) * _BYTE_LEVELS) // (2 * _N)
        texture.append(min(level, _BYTE_LEVELS - 1))
    return texture


def _format_rows(texture: list[int]) -> str:
    """Render the texture as clang-format-clean 16-per-row hex byte literals."""
    lines: list[str] = []
    for start in range(0, _N, _ROW_BYTES):
        cells = texture[start : start + _ROW_BYTES]
        lines.append("  " + " ".join(f"0x{value:02X}," for value in cells))
    return "\n".join(lines)


def _render_header() -> str:
    """Produce the full committed header text for the blue-noise mask table."""
    rows = _format_rows(_threshold_texture())
    return f"""/**
 * @file ra8_gfx_dither_mask_internal.h
 * @brief Void-and-cluster blue-noise threshold mask for the e-ink dither (#477).
 * @ingroup grp_ereader
 *
 * @generated by scripts/gen/gen_bluenoise_mask.py (make bluenoise-mask-update);
 *            do not hand-edit. Bakes the {_DIM}x{_DIM} blue-noise threshold
 *            texture ra8_gfx_dither.c thresholds gray8 samples against to
 *            quantise them to the panel's 16 gray levels without banding.
 *
 * @details
 * Each byte is a threshold in [0, 255], blue-noise distributed so that the
 * level set {{mask < t}} is a high-frequency (void-and-cluster) dot pattern for
 * every t -- the property that gives smooth, grain-only dithering with no Bayer
 * cross-hatch. The table is indexed toroidally by ``(y & {_DIM - 1}) * {_DIM} +
 * (x & {_DIM - 1})`` at absolute panel coordinates, so abutting tiles share one
 * continuous mask phase and never seam. Included by exactly one translation
 * unit (ra8_gfx_dither.c); ``static`` keeps it a single private definition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

/**
 * @var s_ra8_gfx_dither_mask
 * @brief {_DIM}x{_DIM} row-major blue-noise threshold texture ({_N} bytes).
 * @details Generated; read-only. Entry ``[(y & {_DIM - 1}) * {_DIM} + (x &
 *          {_DIM - 1})]`` is the [0, 255] dither threshold for panel pixel
 *          ``(x, y)``. Do not modify -- regenerate via the script above.
 * @note Generated, build-only data; not hand-authored.
 * @since 0.1.0
 */
static const uint8_t s_ra8_gfx_dither_mask[] = {{
{rows}
}};
"""


def _write() -> int:
    """Rewrite the committed mask header in place; return a process exit code."""
    _OUT.write_text(_render_header(), encoding="ascii")
    print(f"gen_bluenoise_mask.py: wrote {_OUT}")
    return 0


def _check() -> int:
    """Fail (exit 1) when the committed header differs from a fresh regenerate."""
    fresh = _render_header()
    try:
        current = _OUT.read_text(encoding="ascii")
    except OSError:
        sys.stderr.write(f"gen_bluenoise_mask.py: {_OUT} is missing.\n")
        return 1
    if current != fresh:
        sys.stderr.write(
            "gen_bluenoise_mask.py: committed mask is stale. "
            "Run `make bluenoise-mask-update` and commit the result.\n"
        )
        return 1
    print("gen_bluenoise_mask.py: committed mask matches a fresh regenerate.")
    return 0


def main() -> int:
    """Parse arguments and either rewrite or verify the committed mask header."""
    parser = argparse.ArgumentParser(description="Generate the blue-noise dither mask table.")
    parser.add_argument(
        "--check", action="store_true", help="verify the committed header is up to date"
    )
    args = parser.parse_args()
    return _check() if args.check else _write()


if __name__ == "__main__":
    raise SystemExit(main())

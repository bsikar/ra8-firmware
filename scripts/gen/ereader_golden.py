#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""ereader_golden.py -- golden-image regression gate for the e-reader chrome.

The ``ereader_ui`` example paints its Library and Reading screens into the GLCDC
framebuffer. ``tools/ra8_emulator`` renders that firmware framebuffer
deterministically, so we can pin the chrome with checked-in golden images and
fail CI (via ``scripts/emu/smoke.sh``) when an unrelated change shifts a pixel.

ra8_emulator's ``--frames-out DIR`` captures numbered P6 PPM images of the
1024x600 panel framebuffer only (no overlay or debug sidebar), so no cropping is
needed. Each screen is run for a fixed virtual-time budget (``--ms 2000``), the
final scanned frame ``frame_NNNNN.ppm`` is read, and its golden bytes are stored
gzipped (the flat 16-level-grayscale chrome compresses ~30x).

Usage
-----
    ereader_golden.py check  --elf E --emulator B --golden-dir D [--out-dir O]
    ereader_golden.py update --elf E --emulator B --golden-dir D

``check`` renders each screen and compares against
``<golden-dir>/<name>.ppm.gz``; mismatches are reported (with the actual image
written to ``--out-dir`` for inspection) and exit status is non-zero.
``update`` regenerates the goldens in place.

The script is stdlib-only (gzip / subprocess / argparse) so it runs anywhere the
ra8_emulator binary and the cross-built ``.elf`` exist.
"""

from __future__ import annotations

import argparse
import gzip
import subprocess
import sys
import tempfile
from pathlib import Path

# Virtual-time settle budget per screen in milliseconds of SysTick time.
RENDER_MS = 2000

# Number of integer tokens in a PPM (P6) header: width, height, maxval.
PPM_HEADER_TOKEN_COUNT = 3

# The screens to capture: (golden name, ra8_emulator extra args). Reading is
# reached by tapping a Library book card; keyboard by tapping the toolbar
# Search field -- both via the genuine touch path (--touch X,Y). battery_low
# fits the Click module's MAX17048 fuel gauge (--click) and seeds it below the
# critical threshold (--battery 8) so the low-battery nag banner overlay is
# captured over the Library chrome.
SCREENS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("library", ()),
    ("reading", ("--touch", "250,250")),
    ("keyboard", ("--touch", "200,100")),
    ("battery_low", ("--click", "--battery", "8")),
)


def validate_ppm(data: bytes, path: Path) -> bytes:
    """Verify P6 PPM header + raster length and return ``data`` unchanged."""
    if data[:2] != b"P6":
        msg = f"{path}: not a P6 PPM"
        raise ValueError(msg)
    idx = 2
    tokens: list[int] = []
    while len(tokens) < PPM_HEADER_TOKEN_COUNT:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        start = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        tokens.append(int(data[start:idx]))
    idx += 1  # single whitespace byte separating the header from the raster
    width, height, _maxval = tokens
    expected_bytes = width * height * 3
    if len(data) - idx != expected_bytes:
        msg = f"{path}: expected {expected_bytes} raster bytes, got {len(data) - idx}"
        raise ValueError(msg)
    return data


def render_panel(emulator: Path, elf: Path, extra: tuple[str, ...]) -> bytes:
    """Run ra8_emulator for one screen and return its final panel P6 PPM bytes."""
    with tempfile.TemporaryDirectory() as tmpdir:
        cmd = [
            str(emulator),
            str(elf),
            "--ms",
            str(RENDER_MS),
            *extra,
            "--frames-out",
            tmpdir,
        ]
        # Capture as bytes, not text: ra8_emulator's diagnostic stream can carry
        # raw bytes (e.g. 0xFF SPI idle bytes from an SD bring-up with no card),
        # which would crash a UTF-8 text decode.
        proc = subprocess.run(cmd, capture_output=True, check=False)  # noqa: S603  # trusted: fixed ra8_emulator argv built from caller-supplied paths
        if proc.returncode != 0:
            err = proc.stderr.decode("utf-8", "replace")
            msg = f"ra8_emulator failed: {' '.join(cmd)}\n{err}"
            raise RuntimeError(msg)
        frames = sorted(Path(tmpdir).glob("frame_*.ppm"))
        if not frames:
            msg = f"ra8_emulator produced no panel frames: {' '.join(cmd)}"
            raise RuntimeError(msg)
        last = frames[-1]
        return validate_ppm(last.read_bytes(), last)


def golden_path(golden_dir: Path, name: str) -> Path:
    """Path of one screen's golden image, gzip-compressed.

    One naming rule shared by the update and check paths, so the two can never
    disagree about which file a screen owns.
    """
    return golden_dir / f"{name}.ppm.gz"


def do_update(args: argparse.Namespace) -> int:
    """Re-render every screen and overwrite its golden image.

    Accepts whatever the emulator currently produces as correct, so it must
    only be run when the change in output is understood and intended -- this
    is the operation that can silently bless a rendering regression.

    Returns 0.
    """
    args.golden_dir.mkdir(parents=True, exist_ok=True)
    for name, extra in SCREENS:
        panel = render_panel(args.emulator, args.elf, extra)
        golden_path(args.golden_dir, name).write_bytes(gzip.compress(panel, 9))
        print(f"  updated {name}.ppm.gz ({len(panel)} bytes -> gz)")
    return 0


def do_check(args: argparse.Namespace) -> int:
    """Re-render every screen and compare it against its golden image.

    A MISSING golden counts as a failure rather than being created on the fly;
    silently generating it would make the first run of a new screen pass
    against no reference at all.

    Returns the number of failing screens (0 when every screen matches).
    """
    failures = 0
    for name, extra in SCREENS:
        gpath = golden_path(args.golden_dir, name)
        if not gpath.exists():
            print(f"  [FAIL] {name}: golden missing ({gpath}); run 'update'")
            failures += 1
            continue
        actual = render_panel(args.emulator, args.elf, extra)
        expected = gzip.decompress(gpath.read_bytes())
        if actual == expected:
            print(f"  [PASS] {name}: chrome matches golden")
            continue
        failures += 1
        msg = "size differs" if len(actual) != len(expected) else "pixels differ"
        print(f"  [FAIL] {name}: {msg} (actual {len(actual)} vs golden {len(expected)})")
        if args.out_dir is not None:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            out = args.out_dir / f"{name}.actual.ppm"
            out.write_bytes(actual)
            print(f"         wrote {out} for inspection")
    if failures != 0:
        print(f"[FAIL] ereader chrome golden: {failures} screen(s) drifted.")
        return 1
    print("[PASS] ereader chrome golden: all screens match.")
    return 0


def main() -> int:
    """Dispatch to the golden-image check or update mode.

    The mode is a required positional with no default, deliberately: defaulting
    to ``update`` would let a careless invocation overwrite the references it
    was meant to test against.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("check", "update"))
    parser.add_argument("--elf", type=Path, required=True, help="cross-built ereader_ui.elf")
    parser.add_argument("--emulator", type=Path, required=True, help="ra8_emulator binary")
    parser.add_argument("--golden-dir", type=Path, required=True, help="golden image directory")
    parser.add_argument("--out-dir", type=Path, default=None, help="where to dump mismatches")
    args = parser.parse_args()

    if not args.elf.exists():
        print(f"error: elf not found: {args.elf}", file=sys.stderr)
        return 2
    if not args.emulator.exists():
        print(f"error: ra8_emulator not found: {args.emulator}", file=sys.stderr)
        return 2

    return do_update(args) if args.mode == "update" else do_check(args)


if __name__ == "__main__":
    sys.exit(main())

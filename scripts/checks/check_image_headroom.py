#!/usr/bin/env python3
"""Fail-closed headroom canary for linked firmware images.

The linker already refuses an image that overflows a region, but that is a wall
rather than a warning: the build is fine at 99.9% and broken at 100.1%, with no
signal in between. This checker fires earlier, at a recorded ceiling below the
region size, so the last few KB get spent deliberately instead of being
discovered by a link failure on somebody else's pull request.

Occupancy is the region's HIGH-WATER MARK, the highest end address of any
section placed in it, minus the region base. That is what the linker's own
"Memory region / Used Size" report counts, and it is not the same as summing
section sizes: alignment padding between sections belongs to the region even
though it belongs to no section. On ereader_shelf's SRAM the two differ by 4 B.

Two kinds of region, because they answer different questions:

  load  a flash/MRAM region, holding sections that occupy image bytes.
        Positioned by LMA, and only LOAD+CONTENTS sections count.
  ram   a RAM region. Positioned by VMA, and every ALLOC section counts,
        including NOBITS ones. .bss occupies RAM at runtime while costing
        nothing in the image, so a load-style measurement reports a RAM region
        as essentially empty: ereader_shelf's SRAM is 83.58% full and would
        measure 0 B.

It never skips. A missing ELF, a missing ceiling row, a missing objdump or a
region with no section in it is an error, not a pass: a scan that was not
performed is never reported as clean.

Usage:
    check_image_headroom.py --all
    check_image_headroom.py --elf <path> --app <ceiling-row-name>
    check_image_headroom.py --selftest
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CEILINGS = REPO_ROOT / ".github" / "image-headroom-ceilings.tsv"
DEFAULT_OBJDUMP = "arm-none-eabi-objdump"

CEILING_FIELDS = 6
KINDS = ("load", "ram")

# objdump -h row: idx name size vma lma fileoff algn
SECTION_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+[0-9a-f]{8}"
)


class HeadroomError(Exception):
    """A condition that must fail the check rather than quietly pass."""


class Section:
    """One row of `objdump -h`, with the flags that decide where it lives."""

    def __init__(self, name: str, size: int, vma: int, lma: int, flags: str) -> None:
        """Record one section and decode the flags that place it."""
        self.name = name
        self.size = size
        self.vma = vma
        self.lma = lma
        self.alloc = "ALLOC" in flags
        self.loadable = "LOAD" in flags and "CONTENTS" in flags

    def address_for(self, kind: str) -> int:
        """Where this section sits in a region of the given kind."""
        return self.lma if kind == "load" else self.vma

    def counts_for(self, kind: str) -> bool:
        """Whether this section occupies a region of the given kind."""
        if self.size == 0:
            return False
        return self.loadable if kind == "load" else self.alloc


def parse_objdump_sections(text: str) -> list[Section]:
    """Return every section objdump printed, with its flags."""
    out: list[Section] = []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        matched = SECTION_RE.match(line)
        if not matched:
            continue
        flags = lines[i + 1].upper() if i + 1 < len(lines) else ""
        out.append(
            Section(
                matched.group(1),
                int(matched.group(2), 16),
                int(matched.group(3), 16),
                int(matched.group(4), 16),
                flags,
            )
        )
    return out


def region_usage(sections: list[Section], base: int, size: int, kind: str) -> tuple[int, list[str]]:
    """Return the region's high-water usage and the section names placed in it.

    High-water, not a sum: inter-section alignment padding is part of what the
    region holds, which is why this reproduces the linker's own figure.
    """
    top = base
    placed = []
    for sec in sections:
        if not sec.counts_for(kind):
            continue
        start = sec.address_for(kind)
        if base <= start < base + size:
            top = max(top, start + sec.size)
            placed.append(sec.name)
    return (top - base if placed else 0), placed


def _row_from_fields(fields: list[str], lineno: int) -> dict[str, object]:
    """Build one validated ceiling row from its six fields."""
    _app, region, kind, base, region_bytes, ceiling = fields
    if kind not in KINDS:
        msg = f"{CEILINGS.name}:{lineno}: kind must be one of {KINDS}, got {kind!r}"
        raise HeadroomError(msg)
    try:
        entry: dict[str, object] = {
            "region": region,
            "kind": kind,
            "base": int(base, 0),
            "region_bytes": int(region_bytes, 0),
            "ceiling_bytes": int(ceiling, 0),
        }
    except ValueError as exc:
        msg = f"{CEILINGS.name}:{lineno}: {exc}"
        raise HeadroomError(msg) from exc
    if int(entry["ceiling_bytes"]) > int(entry["region_bytes"]):
        msg = (
            f"{CEILINGS.name}:{lineno}: ceiling {entry['ceiling_bytes']} exceeds region "
            f"size {entry['region_bytes']}; the canary could never fire"
        )
        raise HeadroomError(msg)
    return entry


def parse_ceilings(text: str) -> dict[str, list[dict[str, object]]]:
    """Map each app name to its pinned region rows."""
    rows: dict[str, list[dict[str, object]]] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split("\t") if f.strip()]
        if len(fields) != CEILING_FIELDS:
            msg = (
                f"{CEILINGS.name}:{lineno}: expected {CEILING_FIELDS} tab-separated fields "
                f"(app, region, kind, base, region_bytes, ceiling_bytes), got {len(fields)}"
            )
            raise HeadroomError(msg)
        rows.setdefault(fields[0], []).append(_row_from_fields(fields, lineno))
    if not rows:
        msg = f"{CEILINGS.name}: no ceiling rows; refusing to report a pass"
        raise HeadroomError(msg)
    return rows


def evaluate(app: str, row: dict[str, object], objdump_text: str) -> tuple[int, str | None]:
    """Return (used_bytes, error_or_None) for one app against one region row."""
    sections = parse_objdump_sections(objdump_text)
    if not sections:
        msg = f"{app}: objdump printed no section table; cannot measure"
        raise HeadroomError(msg)
    base = int(row["base"])
    region_bytes = int(row["region_bytes"])
    ceiling = int(row["ceiling_bytes"])
    kind = str(row["kind"])
    used, placed = region_usage(sections, base, region_bytes, kind)
    if not placed:
        msg = (
            f"{app}: no {kind} section lands in {row['region']} at {base:#010x}; "
            f"the ceiling row is wrong or the image is not linked there"
        )
        raise HeadroomError(msg)
    if used > ceiling:
        return used, (
            f"{app}: {row['region']} holds {used} B, over the {ceiling} B ceiling by "
            f"{used - ceiling} B (region is {region_bytes} B). Shrink the image, or "
            f"re-pin {CEILINGS.name} deliberately in the same change."
        )
    return used, None


def run_objdump(objdump: str, elf: Path) -> str:
    """Return `objdump -h` output for one ELF, erroring if the tool cannot run."""
    try:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, operator-supplied tool path
            [objdump, "-h", str(elf)], capture_output=True, text=True, check=False
        )
    except FileNotFoundError as exc:
        msg = f"{objdump} not found; cannot measure {elf}"
        raise HeadroomError(msg) from exc
    if proc.returncode != 0:
        msg = f"{objdump} -h {elf} failed: {proc.stderr.strip()}"
        raise HeadroomError(msg)
    return proc.stdout


def app_dir(app: str) -> Path:
    """Resolve an app's source directory through the repo's own discovery helper."""
    helper = REPO_ROOT / "scripts" / "dev" / "ra8_apps.py"
    try:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, in-repo helper
            [sys.executable, str(helper), "dir", app],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        msg = f"{app}: cannot run {helper.name} to resolve its directory"
        raise HeadroomError(msg) from exc
    out = proc.stdout.strip()
    if proc.returncode != 0 or not out:
        msg = (
            f"{app}: {helper.name} could not resolve a directory "
            f"({proc.stderr.strip() or 'no output'}). A pinned row must name a real app."
        )
        raise HeadroomError(msg)
    return REPO_ROOT / out


def _report(app: str, row: dict[str, object], used: int) -> None:
    """Print one passing region line."""
    ceiling = int(row["ceiling_bytes"])
    region_bytes = int(row["region_bytes"])
    print(
        f"image-headroom: {app} {row['region']} ({row['kind']}) {used} B, "
        f"{ceiling - used} B under the {ceiling} B ceiling "
        f"({region_bytes - used} B under the hard region limit)"
    )


def check_all(objdump: str) -> int:
    """Measure every pinned row against the image the cross-build produced.

    The ceiling file is the only place an app or region needs adding: this walks
    it, so pinning another one never means editing a gate.
    """
    if not CEILINGS.exists():
        msg = f"{CEILINGS} is missing; cannot report a pass"
        raise HeadroomError(msg)
    pinned = parse_ceilings(CEILINGS.read_text())
    worst = 0
    for app, regions in sorted(pinned.items()):
        elf = app_dir(app) / "build" / f"{app}.elf"
        if not elf.exists():
            msg = (
                f"{app}: {elf} does not exist. This runs after the cross-build; "
                f"an absent image is a failed build, not a pass."
            )
            raise HeadroomError(msg)
        dumped = run_objdump(objdump, elf)
        for row in regions:
            used, err = evaluate(app, row, dumped)
            if err:
                print(f"image-headroom: {err}", file=sys.stderr)
                worst = max(worst, 1)
            else:
                _report(app, row, used)
    return worst


def _resolve(args: argparse.Namespace) -> tuple[str, dict[str, object], Path]:
    """Validate inputs and return (app, ceiling row, elf path) for one region."""
    if args.elf is None:
        msg = "--elf is required (or --all, or --selftest)"
        raise HeadroomError(msg)
    if not CEILINGS.exists():
        msg = f"{CEILINGS} is missing; cannot report a pass"
        raise HeadroomError(msg)
    pinned = parse_ceilings(CEILINGS.read_text())
    app = args.app or args.elf.stem
    if app not in pinned:
        msg = (
            f"{app}: no row in {CEILINGS.name}. Add one (measure with "
            f"`{DEFAULT_OBJDUMP} -h` after a link) rather than skipping it."
        )
        raise HeadroomError(msg)
    regions = pinned[app]
    if args.region:
        regions = [r for r in regions if r["region"] == args.region]
        if not regions:
            msg = f"{app}: no row for region {args.region} in {CEILINGS.name}"
            raise HeadroomError(msg)
    if len(regions) != 1:
        names = ", ".join(str(r["region"]) for r in regions)
        msg = f"{app} pins several regions ({names}); pass --region to pick one"
        raise HeadroomError(msg)
    if not args.elf.exists():
        msg = (
            f"{args.elf} does not exist. This check runs after a link; an absent "
            f"image is a failed build, not a pass."
        )
        raise HeadroomError(msg)
    return app, regions[0], args.elf


FAKE_OBJDUMP = """
build/x.elf:     file format elf32-littlearm

Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .vectors      00000200  02000000  02000000  00001000  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
  1 .text         00001000  02000200  02000200  00001200  2**3
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  2 .data         000000ac  22000000  02001200  00002200  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  3 .bss          000d5da8  22000100  020012ac  000022ac  2**3
                  ALLOC
  4 .debug_info   00001234  00000000  00000000  00003000  2**0
                  CONTENTS, READONLY, DEBUGGING
"""

MRAM_BASE = 0x02000000
MRAM_SIZE = 1048576
SRAM_BASE = 0x22000000
SRAM_SIZE = 1048320
# .vectors + .text + .data are contiguous from the MRAM base, so high-water == sum here.
EXPECTED_LOAD = 0x200 + 0x1000 + 0xAC
# .bss starts at 0x22000100, past the 0xac of .data, so 0x54 of padding belongs to SRAM.
EXPECTED_RAM = 0x100 + 0xD5DA8
SAMPLE_PINNED_REGIONS = 2


def _expect_error(call: Callable[[], object], label: str, failures: list[str]) -> None:
    """Record whether `call` refused, which is the behaviour being asserted."""
    try:
        call()
    except HeadroomError:
        print(f"  [ok] {label}")
        return
    print(f"  [FAIL] {label}")
    failures.append(label)


def selftest() -> int:
    """Exercise both directions, so the canary is known to fire as well as to pass."""
    failures: list[str] = []

    def ok(label: str, *, cond: bool) -> None:
        print(f"  [{'ok' if cond else 'FAIL'}] {label}")
        if not cond:
            failures.append(label)

    sections = parse_objdump_sections(FAKE_OBJDUMP)
    by_name = {s.name: s for s in sections}
    ok("debug section is not ALLOC", cond=not by_name[".debug_info"].alloc)
    ok("nobits .bss is ALLOC but not loadable", cond=by_name[".bss"].alloc)
    ok("nobits .bss is not loadable", cond=not by_name[".bss"].loadable)

    load_used, load_placed = region_usage(sections, MRAM_BASE, MRAM_SIZE, "load")
    ok("load region counts LOAD sections by LMA", cond=load_used == EXPECTED_LOAD)
    ok(".bss excluded from the load region", cond=".bss" not in load_placed)
    ok(".debug_info excluded from the load region", cond=".debug_info" not in load_placed)

    ram_used, ram_placed = region_usage(sections, SRAM_BASE, SRAM_SIZE, "ram")
    ok("ram region counts ALLOC sections by VMA", cond=ram_used == EXPECTED_RAM)
    ok(".bss included in the ram region", cond=".bss" in ram_placed)
    ok("ram high-water includes inter-section padding", cond=ram_used > 0xAC + 0xD5DA8)
    ok(
        "a ram region measured load-style reads as empty",
        cond=region_usage(sections, SRAM_BASE, SRAM_SIZE, "load")[1] == [],
    )

    load_row = {"region": "MRAM", "kind": "load", "base": MRAM_BASE, "region_bytes": MRAM_SIZE}
    ram_row = {"region": "SRAM", "kind": "ram", "base": SRAM_BASE, "region_bytes": SRAM_SIZE}
    ok(
        "under ceiling passes",
        cond=evaluate("x", dict(load_row, ceiling_bytes=load_used + 1), FAKE_OBJDUMP)[1] is None,
    )
    ok(
        "exactly at ceiling passes",
        cond=evaluate("x", dict(load_row, ceiling_bytes=load_used), FAKE_OBJDUMP)[1] is None,
    )
    fired = evaluate("x", dict(load_row, ceiling_bytes=load_used - 1), FAKE_OBJDUMP)[1]
    ok("one byte over the ceiling fails", cond=fired is not None and "over the" in fired)
    ram_fired = evaluate("x", dict(ram_row, ceiling_bytes=ram_used - 1), FAKE_OBJDUMP)[1]
    ok("a ram region over its ceiling fails too", cond=ram_fired is not None)

    _expect_error(
        lambda: evaluate("x", dict(load_row, base=0x60000000, ceiling_bytes=10), FAKE_OBJDUMP),
        "a wrong region base errors instead of passing at zero",
        failures,
    )
    _expect_error(
        lambda: evaluate("x", dict(load_row, ceiling_bytes=1), "no section table here"),
        "unparseable objdump output errors",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("app\tMRAM\tload\t0x02000000\t1048576\t2000000"),
        "a ceiling above the region size is rejected",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("app\tMRAM\tflash\t0x02000000\t1048576\t1000"),
        "an unknown region kind is rejected",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("# only a comment\n"),
        "an empty ceiling file errors",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("app\tMRAM\tload\t0x02000000\t1048576"),
        "a short row errors",
        failures,
    )

    good = parse_ceilings(
        "a\tMRAM\tload\t0x02000000\t1048576\t1000000\na\tSRAM\tram\t0x22000000\t1048320\t900000\n"
    )
    ok("one app can pin several regions", cond=len(good["a"]) == SAMPLE_PINNED_REGIONS)

    print(f"selftest: {len(failures)} failure(s)")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    """Measure linked images against their recorded ceilings."""
    parser = argparse.ArgumentParser(description="Image headroom canary.")
    parser.add_argument("--elf", type=Path, help="linked ELF to measure")
    parser.add_argument("--app", help="ceiling row to use; defaults to the ELF stem")
    parser.add_argument("--region", help="which pinned region to measure")
    parser.add_argument("--objdump", default=DEFAULT_OBJDUMP)
    parser.add_argument(
        "--all",
        action="store_true",
        dest="check_every_pinned_row",
        help="measure every app pinned in the ceiling file",
    )
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.check_every_pinned_row:
        try:
            return check_all(args.objdump)
        except HeadroomError as exc:
            print(f"image-headroom: {exc}", file=sys.stderr)
            return 2

    try:
        app, row, elf = _resolve(args)
        used, err = evaluate(app, row, run_objdump(args.objdump, elf))
    except HeadroomError as exc:
        print(f"image-headroom: {exc}", file=sys.stderr)
        return 2

    if err:
        print(f"image-headroom: {err}", file=sys.stderr)
        return 1
    _report(app, row, used)
    return 0


if __name__ == "__main__":
    sys.exit(main())

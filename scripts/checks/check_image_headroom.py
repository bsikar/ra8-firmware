#!/usr/bin/env python3
"""Fail-closed headroom canary for linked firmware images.

The linker already refuses an image that overflows its MRAM region, but that is
a wall rather than a warning: the build is fine at 99.9% and broken at 100.1%,
with no signal in between. This checker fires earlier, at a recorded ceiling
below the region size, so the last few KB get spent deliberately instead of
being discovered by a link failure on somebody else's pull request.

It never skips. A missing ELF, a missing ceiling row, a missing objdump or a
region that measures zero loadable bytes is an error, not a pass, for the same
reason check_runner_image_deps.py refuses to report a scan it did not perform:
a canary that goes quiet when it cannot measure is worse than no canary.

Usage:
    check_image_headroom.py --elf <path-to-elf>
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

CEILING_FIELDS = 5

# objdump -h row: idx name size vma lma fileoff algn
SECTION_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+[0-9a-f]{8}"
)


class HeadroomError(Exception):
    """A condition that must fail the check rather than quietly pass."""


def parse_objdump_sections(text: str) -> list[tuple[str, int, int, bool]]:
    """Return (name, size, lma, loadable) for every section objdump printed.

    A section only occupies bytes in a load region when it actually carries
    contents. .bss has an LMA inside MRAM but is NOBITS, so it comes back with
    loadable=False and must not be summed.
    """
    out: list[tuple[str, int, int, bool]] = []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        matched = SECTION_RE.match(line)
        if not matched:
            continue
        name = matched.group(1)
        size = int(matched.group(2), 16)
        lma = int(matched.group(4), 16)
        flags = lines[i + 1].upper() if i + 1 < len(lines) else ""
        loadable = "LOAD" in flags and "CONTENTS" in flags
        out.append((name, size, lma, loadable))
    return out


def region_usage(
    sections: list[tuple[str, int, int, bool]], base: int, size: int
) -> tuple[int, list[str]]:
    """Return bytes loaded into [base, base+size) and the section names counted."""
    total = 0
    counted = []
    for name, sec_size, lma, loadable in sections:
        if not loadable or sec_size == 0:
            continue
        if base <= lma < base + size:
            total += sec_size
            counted.append(name)
    return total, counted


def _row_from_fields(fields: list[str], lineno: int) -> dict[str, object]:
    """Build one validated ceiling row from its five fields."""
    _app, region, base, region_bytes, ceiling = fields
    try:
        entry: dict[str, object] = {
            "region": region,
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


def parse_ceilings(text: str) -> dict[str, dict[str, object]]:
    """Map each app name to its validated ceiling row."""
    rows: dict[str, dict[str, object]] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split("\t") if f.strip()]
        if len(fields) != CEILING_FIELDS:
            msg = (
                f"{CEILINGS.name}:{lineno}: expected {CEILING_FIELDS} tab-separated fields "
                f"(app, region, base, region_bytes, ceiling_bytes), got {len(fields)}"
            )
            raise HeadroomError(msg)
        rows[fields[0]] = _row_from_fields(fields, lineno)
    if not rows:
        msg = f"{CEILINGS.name}: no ceiling rows; refusing to report a pass"
        raise HeadroomError(msg)
    return rows


def evaluate(app: str, row: dict[str, object], objdump_text: str) -> tuple[int, str | None]:
    """Return (used_bytes, error_or_None) for one app against one ceiling row."""
    sections = parse_objdump_sections(objdump_text)
    if not sections:
        msg = f"{app}: objdump printed no section table; cannot measure"
        raise HeadroomError(msg)
    base = int(row["base"])
    region_bytes = int(row["region_bytes"])
    ceiling = int(row["ceiling_bytes"])
    used, counted = region_usage(sections, base, region_bytes)
    if not counted:
        msg = (
            f"{app}: no loadable section lands in {row['region']} at {base:#010x}; "
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


def check_all(objdump: str) -> int:
    """Measure every pinned row against the image the cross-build produced.

    The ceiling file is the only place an app needs adding: this walks it, so
    pinning a new app never means editing a gate.
    """
    if not CEILINGS.exists():
        msg = f"{CEILINGS} is missing; cannot report a pass"
        raise HeadroomError(msg)
    rows = parse_ceilings(CEILINGS.read_text())
    worst = 0
    for app, row in sorted(rows.items()):
        elf = app_dir(app) / "build" / f"{app}.elf"
        if not elf.exists():
            msg = (
                f"{app}: {elf} does not exist. This runs after the cross-build; "
                f"an absent image is a failed build, not a pass."
            )
            raise HeadroomError(msg)
        used, err = evaluate(app, row, run_objdump(objdump, elf))
        if err:
            print(f"image-headroom: {err}", file=sys.stderr)
            worst = max(worst, 1)
        else:
            ceiling = int(row["ceiling_bytes"])
            region_bytes = int(row["region_bytes"])
            print(
                f"image-headroom: {app} {row['region']} {used} B, "
                f"{ceiling - used} B under the {ceiling} B ceiling "
                f"({region_bytes - used} B under the hard region limit)"
            )
    return worst


def _resolve(args: argparse.Namespace) -> tuple[str, dict[str, object], Path]:
    """Validate inputs and return (app, ceiling row, elf path)."""
    if args.elf is None:
        msg = "--elf is required (or --selftest)"
        raise HeadroomError(msg)
    if not CEILINGS.exists():
        msg = f"{CEILINGS} is missing; cannot report a pass"
        raise HeadroomError(msg)
    rows = parse_ceilings(CEILINGS.read_text())
    app = args.app or args.elf.stem
    if app not in rows:
        msg = (
            f"{app}: no row in {CEILINGS.name}. Add one (measure with "
            f"`{DEFAULT_OBJDUMP} -h` after a link) rather than skipping it."
        )
        raise HeadroomError(msg)
    if not args.elf.exists():
        msg = (
            f"{args.elf} does not exist. This check runs after a link; an absent "
            f"image is a failed build, not a pass."
        )
        raise HeadroomError(msg)
    return app, rows[app], args.elf


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
  3 .bss          000d5da8  220000b0  020012ac  000022ac  2**3
                  ALLOC
"""

MRAM_BASE = 0x02000000
MRAM_SIZE = 1048576
EXPECTED_USED = 0x200 + 0x1000 + 0xAC
SAMPLE_CEILING = 1000000


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
    loadable = {name for name, _, _, load in sections if load}
    ok("nobits .bss is not loadable", cond=".bss" not in loadable)
    ok("loadable sections parsed", cond=loadable == {".vectors", ".text", ".data"})

    used, counted = region_usage(sections, MRAM_BASE, MRAM_SIZE)
    ok("usage sums LMA-resident loadable sections only", cond=used == EXPECTED_USED)
    ok(".bss excluded despite an LMA inside the region", cond=".bss" not in counted)

    base_row = {"region": "MRAM", "base": MRAM_BASE, "region_bytes": MRAM_SIZE}
    under = dict(base_row, ceiling_bytes=used + 1)
    exact = dict(base_row, ceiling_bytes=used)
    over = dict(base_row, ceiling_bytes=used - 1)
    ok("under ceiling passes", cond=evaluate("x", under, FAKE_OBJDUMP)[1] is None)
    ok("exactly at ceiling passes", cond=evaluate("x", exact, FAKE_OBJDUMP)[1] is None)
    fired = evaluate("x", over, FAKE_OBJDUMP)[1]
    ok("one byte over the ceiling fails", cond=fired is not None and "over the" in fired)

    _expect_error(
        lambda: evaluate("x", dict(base_row, base=0x60000000, ceiling_bytes=10), FAKE_OBJDUMP),
        "a wrong region base errors instead of passing at zero",
        failures,
    )
    _expect_error(
        lambda: evaluate("x", exact, "no section table here"),
        "unparseable objdump output errors",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("app\tMRAM\t0x02000000\t1048576\t2000000"),
        "a ceiling above the region size is rejected",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("# only a comment\n"),
        "an empty ceiling file errors",
        failures,
    )
    _expect_error(
        lambda: parse_ceilings("app\tMRAM\t0x02000000\t1048576"),
        "a short row errors",
        failures,
    )

    good = parse_ceilings(f"a\tMRAM\t0x02000000\t1048576\t{SAMPLE_CEILING}\n")
    ok("a well-formed row parses", cond=good["a"]["ceiling_bytes"] == SAMPLE_CEILING)

    print(f"selftest: {len(failures)} failure(s)")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    """Measure one linked image against its recorded ceiling."""
    parser = argparse.ArgumentParser(description="Image headroom canary.")
    parser.add_argument("--elf", type=Path, help="linked ELF to measure")
    parser.add_argument("--app", help="ceiling row to use; defaults to the ELF stem")
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
    ceiling = int(row["ceiling_bytes"])
    region_bytes = int(row["region_bytes"])
    print(
        f"image-headroom: {app} {row['region']} {used} B, {ceiling - used} B under the "
        f"{ceiling} B ceiling ({region_bytes - used} B under the hard region limit)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the committed per-package pinout references under ``docs/pinouts/``.

Every orderable RA8D2 and RA8P1 part number is a point in a four-axis
product matrix -- feature set, code-MRAM size, junction-temperature grade
and package -- but only two of those axes move a ball: the **package**
(LFBGA 224 / 289 / 303) and whether the **feature set** bonds out the MIPI
DSI/CSI pins (``B`` and ``K`` do, ``A`` and ``J`` do not). The 32 part
numbers per group therefore collapse onto six distinct ball maps, and this
script emits one file per map plus an index that resolves any part number
to its map.

The ball maps are not transcribed; they are parsed out of the primary
source, section 1.7 "Pin Lists" of each group datasheet:

* RA8D2 -- ``docs/reference/ra8d2-datasheet.pdf`` (R01DS0493EJ), Table 1.16
  (Standard product) and Table 1.17 (SiP product).
* RA8P1 -- ``docs/reference/ra8p1-datasheet.pdf`` (R01DS0439EJ), Table 1.17
  (Standard product) and Table 1.18 (SiP product).

Each of those tables is one row per *signal position* with a leading column
per package variant giving that variant's ball coordinate, or an em dash
where the variant does not bond the position out. Selecting a variant is
therefore selecting one leading column and dropping its dashed rows.

The parse is not trusted on its own. ``validate_variant()`` holds every
variant to four independent facts the datasheet states elsewhere: the ball
count its package name implies, the I/O-port count the "Function Comparison"
table prints, no ball claimed twice, and -- the load-bearing one -- the exact
port-pin set drawn by that variant's section 1.6 ball-grid FIGURE, which is a
wholly separate rendering of the same information. A mis-sliced column fails
the run instead of quietly emitting a thinner table.

The work is split across sibling modules: ``pinout_model`` (the product
matrix and table shape), ``pinout_parse`` (PDF -> rows), ``pinout_render``
(rows -> files) and ``pinout_selftest``. This file owns validation, the
group-vs-group diff, and the CLI.

Usage::

    python3 scripts/gen/gen_pinouts.py            # write docs/pinouts/
    python3 scripts/gen/gen_pinouts.py --check    # fail if they are stale
    python3 scripts/gen/gen_pinouts.py --selftest # assert both directions

``--check`` is what the ``pinout-freshness`` gate runs, so a datasheet
revision that moves a ball cannot land without the reference moving with it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pinout_selftest
from pinout_model import (
    BALL_COLUMNS,
    EXPECTED_IO_PORTS,
    FIELDS,
    GROUPS,
    OUT_DIR,
    PACKAGES,
    REPO_ROOT,
    Group,
    ParseError,
    sram_for,
    variant_slug,
)
from pinout_parse import (
    extract_parts,
    figure_port_sets,
    parse_pin_list,
    pdf_text,
)
from pinout_render import render_index, render_variant

PARTS_PER_GROUP = 32


def validate_variant(
    group: Group, column: tuple, rows: list[dict], figures: dict
) -> tuple[list[dict], int]:
    """Hold one variant to every count the datasheet states elsewhere.

    Returns its balls and I/O-port count. Raises rather than returning a
    verdict: a generator that emitted a table it could not corroborate would
    be worse than one that emitted nothing, because the output looks the
    same either way.
    """
    package, mipi = column
    where = f"{group.name} {package} mipi={mipi}"
    mine = [r for r in rows if r["balls"][column] is not None]

    if len({r["balls"][column] for r in mine}) != len(mine):
        msg = f"{where}: the same ball appears twice"
        raise ParseError(msg)

    if len(mine) != PACKAGES[package].balls:
        msg = f"{where}: parsed {len(mine)} balls, the package has {PACKAGES[package].balls}"
        raise ParseError(msg)

    ports = {r["port"] for r in mine if r["port"]}
    if len(ports) != EXPECTED_IO_PORTS[column]:
        msg = (
            f"{where}: parsed {len(ports)} I/O port pins, the datasheet's "
            f"Function Comparison table says {EXPECTED_IO_PORTS[column]}"
        )
        raise ParseError(msg)

    drawn = figures[column]
    if ports != drawn:
        msg = (
            f"{where}: the section 1.7 pin list and the section 1.6 "
            f"ball-grid figure disagree -- only in the list "
            f"{sorted(ports - drawn)}, only in the figure "
            f"{sorted(drawn - ports)}"
        )
        raise ParseError(msg)
    return mine, len(ports)


def build_group(group: Group) -> tuple[dict, list, list, dict]:
    """Parse, validate and render one MCU group."""
    text = pdf_text(group.pdf)
    parts = extract_parts(text, group)
    if len(parts) != PARTS_PER_GROUP:
        msg = f"{group.name}: expected {PARTS_PER_GROUP} part numbers, found {len(parts)}"
        raise ParseError(msg)
    figures = figure_port_sets(text)
    by_kind = {kind: parse_pin_list(text, group, kind) for kind in BALL_COLUMNS}

    files: dict[str, str] = {}
    variants = []
    for kind, rows in by_kind.items():
        for column in BALL_COLUMNS[kind]:
            package, mipi = column
            mine, io_pins = validate_variant(group, column, rows, figures)
            filename = f"{group.slug}_{variant_slug(package, mipi)}.txt"
            files[filename] = render_variant(group, package, mipi, rows, parts)
            variants.append((group.name, package, mipi, filename, len(mine), io_pins, kind))

    index = [
        (part, sram_for(part), f"{group.slug}_{variant_slug(part.package, part.mipi)}.txt")
        for part in parts
    ]
    return files, variants, index, by_kind


def build() -> dict:
    """Parse both datasheets and render every output file in memory."""
    files: dict[str, str] = {}
    variants: list = []
    parts_index: list = []
    per_group_rows: dict = {}

    for group in GROUPS:
        group_files, group_variants, group_parts, rows = build_group(group)
        files.update(group_files)
        variants += group_variants
        parts_index += group_parts
        per_group_rows[group.name] = rows

    files["README.md"] = render_index(
        {
            "variants": variants,
            "parts": parts_index,
            "compat": compare_groups(per_group_rows),
        }
    )
    return {name: scrub(body) for name, body in files.items()}


def _signature(rows: list[dict]) -> dict:
    """Map every (variant, ball) to the function set printed against it."""
    out = {}
    for row in rows:
        for column, ball in row["balls"].items():
            if ball is not None:
                out[(column, ball)] = tuple(row[f] for f in FIELDS)
    return out


def compare_groups(per_group_rows: dict) -> list[str]:
    """Diff the two groups' pin lists and describe the result.

    "The RA8P1 is pin-compatible with the RA8D2" was an assertion carried in
    prose. Diffing the two parsed pin lists turns it into a measurement that
    is re-taken on every run, so a future revision that breaks it says so.
    """
    a, b = GROUPS[0], GROUPS[1]
    notes = []
    for kind in BALL_COLUMNS:
        sig_a = _signature(per_group_rows[a.name][kind])
        sig_b = _signature(per_group_rows[b.name][kind])
        only_a = sorted(set(sig_a) - set(sig_b))
        only_b = sorted(set(sig_b) - set(sig_a))
        differing = sorted(k for k in set(sig_a) & set(sig_b) if sig_a[k] != sig_b[k])
        label = "Standard" if kind == "standard" else "SiP"

        if not (only_a or only_b or differing):
            notes.append(
                f"- **{label} products: identical.** Every ball of every "
                f"{label} package carries the same function set on the "
                f"{a.name} and the {b.name} -- {len(sig_a)} (variant, ball) "
                f"pairs compared, established by diffing the two parsed pin "
                f"lists rather than by assertion."
            )
            continue

        notes.append(
            f"- **{label} products: {len(differing)} ball(s) differ**, "
            f"{len(only_a)} only on the {a.name}, {len(only_b)} only on "
            f"the {b.name}."
        )
        for column, ball in (differing + only_a + only_b)[:20]:
            notes.append(
                f"  - `{ball}` (LFBGA {PACKAGES[column[0]].balls}"
                f"{'' if column[1] else ' without MIPI'})"
            )

    return [
        *notes,
        "",
        "The one function the two groups do not share (the RA8P1's Ethos-U55",
        "NPU) is not pinned out, so pin compatibility is what the diff above",
        "shows. See `docs/reference/ra8p1_vs_ra8d2.md` for the register-level delta.",
    ]


def scrub(content: str) -> str:
    """Strip trailing whitespace; the format gates reject it repo-wide."""
    return "\n".join(line.rstrip() for line in content.split("\n"))


def write(files: dict[str, str]) -> None:
    """Write every rendered file, removing any output that is no longer ours."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, content in sorted(files.items()):
        (OUT_DIR / name).write_text(content, encoding="ascii")
    for stale in sorted(OUT_DIR.iterdir()):
        if stale.name not in files:
            stale.unlink()


def check(files: dict[str, str]) -> int:
    """Fail if the committed files are not what a fresh parse produces."""
    problems = []
    for name, content in sorted(files.items()):
        path = OUT_DIR / name
        if not path.is_file():
            problems.append(f"missing: {path.relative_to(REPO_ROOT)}")
        elif path.read_text(encoding="utf-8") != content:
            problems.append(f"stale: {path.relative_to(REPO_ROOT)}")
    if OUT_DIR.is_dir():
        problems.extend(
            f"unexpected: {extra.relative_to(REPO_ROOT)}"
            for extra in sorted(OUT_DIR.iterdir())
            if extra.name not in files
        )

    for problem in problems:
        print(f"gen_pinouts: {problem}", file=sys.stderr)
    if problems:
        print(
            "gen_pinouts: run 'python3 scripts/gen/gen_pinouts.py' and commit the result",
            file=sys.stderr,
        )
        return 1
    print(f"gen_pinouts: {len(files)} file(s) up to date")
    return 0


def main() -> int:
    """Run the self-test, freshness check, or generator requested by the CLI."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="fail if the committed files are stale"
    )
    parser.add_argument(
        "--selftest", action="store_true", help="assert the parser fires and stays quiet"
    )
    args = parser.parse_args()

    if args.selftest:
        return pinout_selftest.run()

    try:
        files = build()
    except ParseError as exc:
        print(f"gen_pinouts: {exc}", file=sys.stderr)
        return 1

    if args.check:
        return check(files)
    write(files)
    print(f"gen_pinouts: wrote {len(files)} file(s) to {OUT_DIR.relative_to(REPO_ROOT)}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

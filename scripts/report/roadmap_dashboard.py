#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""roadmap_dashboard.py -- generate a human-friendly roadmap dashboard.

This consumer reads docs/ROADMAP.md using the same conventions as
scripts/report/roadmap_stats.py:

    - "## <phase>"   -- phase heading (groups drivers)
    - "### <name>"   -- driver heading
    - "`[<m>]` Status:" line where <m> is one of " x~!"
    - a fenced ``` ... ``` block per driver containing 14 checkboxes

The script emits two artifacts:

    1. docs/ROADMAP_DASHBOARD.md
       A markdown report with a per-phase progress table and a
       per-driver progress bar.

    2. docs/badges/<name>.svg
       Static, shields.io-compatible SVG badges:
         - drivers.svg   "drivers <DONE>/<total>"
         - coverage.svg  "coverage <pct>%"
         - done.svg      "done <DONE>"
         - wip.svg       "wip <WIP>"
         - blocked.svg   "blocked <BLOCKED>"
         - todo.svg      "todo <TODO>"

This script intentionally does NOT modify ROADMAP.md and does NOT
share state with roadmap_stats.py beyond the on-disk markdown.

Usage:

    python3 scripts/report/roadmap_dashboard.py
    python3 scripts/report/roadmap_dashboard.py --check

In --check mode the script exits non-zero if the dashboard or any
badge would change on disk.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ROADMAP_PATH = REPO_ROOT / "docs" / "ROADMAP.md"
DASHBOARD_PATH = REPO_ROOT / "docs" / "ROADMAP_DASHBOARD.md"
BADGES_DIR = REPO_ROOT / "docs" / "badges"

PHASE_HEADING_RE = re.compile(r"^##\s+(?!#)(.+?)\s*$")
DRIVER_HEADING_RE = re.compile(r"^###\s+(.+?)\s*$")
STATUS_LINE_RE = re.compile(r"`\[(?P<mark>[ x~!])\]`\s*Status:")
CHECKBOX_RE = re.compile(r"^\s*\[(?P<mark>[ x~!])\]")

BAR_WIDTH = 20

STATUS_LABEL = {
    "x": "DONE",
    "~": "WIP",
    "!": "BLOCKED",
    " ": "TODO",
}

# Shields-style colors keyed by status / metric.
COLOR_DONE = "#4c1"
COLOR_WIP = "#dfb317"
COLOR_BLOCKED = "#e05d44"
COLOR_TODO = "#9f9f9f"
COLOR_INFO = "#007ec6"

# Coverage thresholds used to pick a badge color for checklist completion.
COV_EXCELLENT = 95.0  # >= this -> green (COLOR_DONE)
COV_GOOD = 75.0  # >= this -> light-green
COV_OK = 50.0  # >= this -> yellow (COLOR_WIP)
COV_LOW = 25.0  # >= this -> orange


class Driver:
    """One "### <name>" driver section."""

    def __init__(self, name: str, status: str, phase: str) -> None:
        """Create a driver section with zero counted boxes.

        Box counts start at zero and are accumulated by the parser as it walks
        the section, so a Driver is only complete once its whole section has
        been read.
        """
        self.name = name
        self.status = status  # one of " x~!"
        self.phase = phase
        self.total_boxes = 0
        self.ticked_boxes = 0

    @property
    def status_label(self) -> str:
        """Human-readable status, mapped from the single-character marker.

        Raises KeyError on an unrecognised marker rather than defaulting: an
        unknown status in ROADMAP.md is a typo, and silently rendering it as
        TODO would understate progress.
        """
        return STATUS_LABEL[self.status]

    @property
    def pct(self) -> float:
        """Checklist completion as a percentage; 0.0 when the section has no boxes.

        A box-less section reports 0 rather than 100 -- "nothing to do" and
        "everything done" are different claims, and only the second is
        evidence.
        """
        if self.total_boxes == 0:
            return 0.0
        return 100.0 * self.ticked_boxes / self.total_boxes


def parse_roadmap(text: str) -> list[Driver]:
    """Walk ROADMAP.md and produce one Driver per "### " section."""
    drivers: list[Driver] = []
    lines = text.splitlines()
    i = 0
    n = len(lines)
    current_phase = "(unphased)"

    while i < n:
        line = lines[i]

        ph = PHASE_HEADING_RE.match(line)
        if ph:
            current_phase = ph.group(1)
            i += 1
            continue

        m = DRIVER_HEADING_RE.match(line)
        if not m:
            i += 1
            continue

        name = m.group(1)

        # Find the Status: line within the next 8 lines.
        status_mark: str | None = None
        for j in range(i + 1, min(i + 8, n)):
            sm = STATUS_LINE_RE.search(lines[j])
            if sm:
                status_mark = sm.group("mark")
                break
        if status_mark is None:
            i += 1
            continue

        drv = Driver(name=name, status=status_mark, phase=current_phase)

        # Walk to the first fenced block before the next "### "/"## ".
        k = i + 1
        while k < n:
            if lines[k].startswith("### ") or lines[k].startswith("## "):
                break
            if lines[k].strip().startswith("```"):
                k += 1
                while k < n and not lines[k].strip().startswith("```"):
                    cb = CHECKBOX_RE.match(lines[k])
                    if cb:
                        drv.total_boxes += 1
                        if cb.group("mark") == "x":
                            drv.ticked_boxes += 1
                    k += 1
                break
            k += 1

        drivers.append(drv)
        i = max(k, i + 1)

    return drivers


def progress_bar(ticked: int, total: int, width: int = BAR_WIDTH) -> str:
    """Return a "[====    ] N/M" style ASCII progress bar."""
    if total <= 0:
        return "[" + " " * width + "] 0/0"
    filled = (ticked * width) // total
    filled = min(filled, width)
    bar = "=" * filled + " " * (width - filled)
    return f"[{bar}] {ticked}/{total}"


def status_glyph(status: str) -> str:
    """Badge text for a status marker, defaulting to TODO for anything unknown.

    Unlike ``status_label`` this is lenient, because it feeds a rendered badge
    where an exception would break the whole dashboard over one bad marker.
    """
    return {
        "x": "DONE",
        "~": "WIP",
        "!": "BLOCKED",
        " ": "TODO",
    }[status]


def _render_header() -> list[str]:
    """The do-not-edit banner and page title."""
    return [
        "<!--",
        "  ROADMAP_DASHBOARD.md -- generated by",
        "  scripts/report/roadmap_dashboard.py.  Do not edit by hand;",
        "  re-run `make dashboard` to refresh.",
        "-->",
        "",
        "# Roadmap Dashboard",
        "",
        "Auto-generated view of `docs/ROADMAP.md`.  Each driver has 14 "
        "checkboxes; bars show `ticked / total`.",
        "",
    ]


def _render_totals(drivers: list[Driver]) -> list[str]:
    """The whole-roadmap totals table and overall progress bar."""
    counts = {"DONE": 0, "WIP": 0, "BLOCKED": 0, "TODO": 0}
    total_boxes = 0
    ticked_boxes = 0
    for d in drivers:
        counts[d.status_label] += 1
        total_boxes += d.total_boxes
        ticked_boxes += d.ticked_boxes
    pct = (100.0 * ticked_boxes / total_boxes) if total_boxes else 0.0
    return [
        "## Totals",
        "",
        "| Metric | Value |",
        "| --- | --- |",
        f"| Drivers tracked | {len(drivers)} |",
        f"| DONE | {counts['DONE']} |",
        f"| WIP | {counts['WIP']} |",
        f"| BLOCKED | {counts['BLOCKED']} |",
        f"| TODO | {counts['TODO']} |",
        f"| Checklist coverage | {ticked_boxes}/{total_boxes} ({pct:.1f}%) |",
        "",
        "Overall: `" + progress_bar(ticked_boxes, total_boxes, BAR_WIDTH * 2) + "`",
        "",
    ]


def _group_by_phase(drivers: list[Driver]) -> tuple[list[str], dict[str, list[Driver]]]:
    """Group drivers by phase, preserving first-seen order.

    First-seen rather than sorted: the roadmap's phase order is the plan's
    order, and re-sorting it alphabetically would present the schedule wrong.
    """
    phase_order: list[str] = []
    by_phase: dict[str, list[Driver]] = {}
    for d in drivers:
        if d.phase not in by_phase:
            by_phase[d.phase] = []
            phase_order.append(d.phase)
        by_phase[d.phase].append(d)
    return phase_order, by_phase


def _render_per_phase(phase_order: list[str], by_phase: dict[str, list[Driver]]) -> list[str]:
    """One summary row per phase: driver counts by status, boxes, and a bar."""
    out = [
        "## Per-phase progress",
        "",
        "| Phase | Drivers | DONE | WIP | BLOCKED | TODO | Boxes | Bar |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for phase in phase_order:
        items = by_phase[phase]
        ph_counts = {"DONE": 0, "WIP": 0, "BLOCKED": 0, "TODO": 0}
        ph_total = 0
        ph_ticked = 0
        for d in items:
            ph_counts[d.status_label] += 1
            ph_total += d.total_boxes
            ph_ticked += d.ticked_boxes
        bar = progress_bar(ph_ticked, ph_total, BAR_WIDTH)
        out.append(
            "| "
            + " | ".join(
                [
                    phase.replace("|", "/"),
                    str(len(items)),
                    str(ph_counts["DONE"]),
                    str(ph_counts["WIP"]),
                    str(ph_counts["BLOCKED"]),
                    str(ph_counts["TODO"]),
                    f"{ph_ticked}/{ph_total}",
                    "`" + bar + "`",
                ]
            )
            + " |"
        )
    out.append("")
    return out


def _render_per_driver(phase_order: list[str], by_phase: dict[str, list[Driver]]) -> list[str]:
    """One table per phase listing each driver's status and checklist bar."""
    out = ["## Per-driver progress", ""]
    for phase in phase_order:
        out.extend(
            [f"### {phase}", "", "| Driver | Status | Boxes | Bar |", "| --- | --- | ---: | --- |"]
        )
        for d in by_phase[phase]:
            bar = progress_bar(d.ticked_boxes, d.total_boxes, BAR_WIDTH)
            out.append(
                "| "
                + " | ".join(
                    [
                        d.name.replace("|", "/"),
                        status_glyph(d.status),
                        f"{d.ticked_boxes}/{d.total_boxes}",
                        "`" + bar + "`",
                    ]
                )
                + " |"
            )
        out.append("")
    return out


def render_dashboard(drivers: list[Driver]) -> str:
    """Build the markdown dashboard text."""
    phase_order, by_phase = _group_by_phase(drivers)
    out = [
        *_render_header(),
        *_render_totals(drivers),
        *_render_per_phase(phase_order, by_phase),
        *_render_per_driver(phase_order, by_phase),
    ]
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------- #
# SVG badge renderer (shields.io-compatible static SVG)
# --------------------------------------------------------------------- #

# Approximate "Verdana 11px" character width.  We use a fixed 7-px-per-char
# value which is close enough for rendering and keeps this stdlib-only.
_CHAR_PX = 7


def _text_px(text: str) -> int:
    return max(_CHAR_PX * len(text), _CHAR_PX)


def render_badge(label: str, value: str, color: str) -> str:
    """Return a shields-style 2-segment SVG badge as a string."""
    label_w = _text_px(label) + 10
    value_w = _text_px(value) + 10
    total_w = label_w + value_w
    height = 20

    # Centers, in 10x scale to mimic shields.
    label_cx = label_w * 5
    value_cx = label_w * 10 + value_w * 5
    label_text_w = (label_w - 10) * 10
    value_text_w = (value_w - 10) * 10

    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{total_w}" height="{height}" '
        f'role="img" aria-label="{label}: {value}">\n'
        f"  <title>{label}: {value}</title>\n"
        f'  <linearGradient id="s" x2="0" y2="100%">\n'
        f'    <stop offset="0" stop-color="#bbb" stop-opacity=".1"/>\n'
        f'    <stop offset="1" stop-opacity=".1"/>\n'
        f"  </linearGradient>\n"
        f'  <clipPath id="r"><rect width="{total_w}" height="{height}" '
        f'rx="3" fill="#fff"/></clipPath>\n'
        f'  <g clip-path="url(#r)">\n'
        f'    <rect width="{label_w}" height="{height}" fill="#555"/>\n'
        f'    <rect x="{label_w}" width="{value_w}" height="{height}" '
        f'fill="{color}"/>\n'
        f'    <rect width="{total_w}" height="{height}" fill="url(#s)"/>\n'
        f"  </g>\n"
        f'  <g fill="#fff" text-anchor="middle" '
        f'font-family="Verdana,Geneva,DejaVu Sans,sans-serif" '
        f'text-rendering="geometricPrecision" font-size="110">\n'
        f'    <text aria-hidden="true" x="{label_cx}" y="150" '
        f'fill="#010101" fill-opacity=".3" transform="scale(.1)" '
        f'textLength="{label_text_w}">{label}</text>\n'
        f'    <text x="{label_cx}" y="140" transform="scale(.1)" '
        f'fill="#fff" textLength="{label_text_w}">{label}</text>\n'
        f'    <text aria-hidden="true" x="{value_cx}" y="150" '
        f'fill="#010101" fill-opacity=".3" transform="scale(.1)" '
        f'textLength="{value_text_w}">{value}</text>\n'
        f'    <text x="{value_cx}" y="140" transform="scale(.1)" '
        f'fill="#fff" textLength="{value_text_w}">{value}</text>\n'
        f"  </g>\n"
        f"</svg>\n"
    )


def coverage_color(pct: float) -> str:
    """Badge colour for a coverage percentage, banded worst-to-best.

    Thresholds are compared descending so each band claims everything above
    it; the order of the tests is the band definition.
    """
    if pct >= COV_EXCELLENT:
        return COLOR_DONE
    if pct >= COV_GOOD:
        return "#97ca00"
    if pct >= COV_OK:
        return COLOR_WIP
    if pct >= COV_LOW:
        return "#fe7d37"
    return COLOR_BLOCKED


def build_badges(drivers: list[Driver]) -> dict[str, str]:
    """Return a {filename: svg_text} mapping for every badge we emit."""
    counts = {"DONE": 0, "WIP": 0, "BLOCKED": 0, "TODO": 0}
    total_boxes = 0
    ticked_boxes = 0
    for d in drivers:
        counts[d.status_label] += 1
        total_boxes += d.total_boxes
        ticked_boxes += d.ticked_boxes

    total = len(drivers)
    pct = (100.0 * ticked_boxes / total_boxes) if total_boxes else 0.0

    badges: dict[str, str] = {}
    badges["drivers.svg"] = render_badge(
        "drivers",
        f"{counts['DONE']}/{total}",
        COLOR_DONE if counts["DONE"] == total else COLOR_INFO,
    )
    badges["coverage.svg"] = render_badge("coverage", f"{pct:.1f}%", coverage_color(pct))
    badges["done.svg"] = render_badge("done", str(counts["DONE"]), COLOR_DONE)
    badges["wip.svg"] = render_badge(
        "wip",
        str(counts["WIP"]),
        COLOR_WIP if counts["WIP"] else COLOR_TODO,
    )
    badges["blocked.svg"] = render_badge(
        "blocked",
        str(counts["BLOCKED"]),
        COLOR_BLOCKED if counts["BLOCKED"] else COLOR_DONE,
    )
    badges["todo.svg"] = render_badge(
        "todo",
        str(counts["TODO"]),
        COLOR_TODO if counts["TODO"] else COLOR_DONE,
    )
    return badges


# --------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------- #


def _write_if_changed(path: pathlib.Path, content: str) -> bool:
    """Write `content` to `path` if different.  Return True if changed."""
    if path.exists():
        old = path.read_text(encoding="utf-8")
        if old == content:
            return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def _build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser for this generator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if any output file would change",
    )
    parser.add_argument(
        "--roadmap",
        default=str(ROADMAP_PATH),
        help=f"path to ROADMAP.md (default: {ROADMAP_PATH})",
    )
    parser.add_argument(
        "--dashboard",
        default=str(DASHBOARD_PATH),
        help=f"output dashboard path (default: {DASHBOARD_PATH})",
    )
    parser.add_argument(
        "--badges-dir",
        default=str(BADGES_DIR),
        help=f"output badges directory (default: {BADGES_DIR})",
    )
    return parser


def _check_up_to_date(
    dashboard_path: pathlib.Path,
    dashboard_md: str,
    badges_dir: pathlib.Path,
    badges: dict[str, str],
    driver_count: int,
) -> int:
    """Compare every output against what would be generated, WITHOUT writing.

    Writing here would defeat the purpose: CI runs this mode, and a generator
    that refreshes its own output always agrees with itself and can never
    report a stale file.

    Returns 1 (listing each stale path) when anything would change, else 0.
    """
    changed: list[str] = []
    if not dashboard_path.exists() or (dashboard_path.read_text(encoding="utf-8") != dashboard_md):
        changed.append(str(dashboard_path))
    for name, svg in badges.items():
        target = badges_dir / name
        if not target.exists() or (target.read_text(encoding="utf-8") != svg):
            changed.append(str(target))
    if changed:
        print("roadmap_dashboard.py: the following outputs are stale:", file=sys.stderr)
        for c in changed:
            print(f"  {c}", file=sys.stderr)
        print("  (run `make dashboard` to refresh)", file=sys.stderr)
        return 1
    print(
        f"roadmap_dashboard.py: dashboard up to date (drivers={driver_count})",
        file=sys.stderr,
    )
    return 0


def main(argv: list[str]) -> int:
    """Regenerate the roadmap dashboard, or with ``--check`` verify it is current.

    ``--check`` writes nothing and exits 1 when the file would change, which
    is what CI runs -- otherwise the dashboard would be regenerated by the
    runner and always agree with itself.

    Returns 0 when the dashboard is up to date or was rewritten, 1 under
    ``--check`` when it is stale.
    """
    args = _build_parser().parse_args(argv)

    roadmap_path = pathlib.Path(args.roadmap)
    if not roadmap_path.exists():
        print(
            f"roadmap_dashboard.py: not found: {roadmap_path}",
            file=sys.stderr,
        )
        return 2

    text = roadmap_path.read_text(encoding="utf-8")
    drivers = parse_roadmap(text)

    dashboard_md = render_dashboard(drivers)
    badges = build_badges(drivers)

    dashboard_path = pathlib.Path(args.dashboard)
    badges_dir = pathlib.Path(args.badges_dir)

    changed: list[str] = []

    if args.check:
        return _check_up_to_date(dashboard_path, dashboard_md, badges_dir, badges, len(drivers))

    if _write_if_changed(dashboard_path, dashboard_md):
        changed.append(str(dashboard_path))
    for name, svg in badges.items():
        if _write_if_changed(badges_dir / name, svg):
            changed.append(str(badges_dir / name))

    if changed:
        print(
            f"roadmap_dashboard.py: wrote {len(changed)} file(s) (drivers={len(drivers)})",
            file=sys.stderr,
        )
        for c in changed:
            print(f"  {c}", file=sys.stderr)
    else:
        print(
            f"roadmap_dashboard.py: no changes (drivers={len(drivers)})",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Refresh the summary of the closed historical HAL completion record.

This script keeps archived evidence internally consistent; it does not track
current work. The Summary block sits between two HTML-comment markers that this
script owns:

    <!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->
    ...
    <!-- END SUMMARY -->

This script:

    1. Parses every archived "###" driver section in ROADMAP.md.
    2. Detects each driver's historical status from its `Status: ...` line.
    3. Counts the checkboxes inside the section's fenced code
       block (`[ ]` / `[~]` / `[x]` / `[!]`).
    4. Rewrites the summary block with deterministic counts.

Modes:

    --check  -- exit non-zero if the file would change. CI / pre-commit.
    (default) -- rewrite the file in place.

The check mode is the gate the pre-commit hook uses: it ensures
the summary you committed matches the checkbox state of the rest
of the file.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ROADMAP_PATH = REPO_ROOT / "docs" / "ROADMAP.md"

BEGIN_MARK = "<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->"
END_MARK = "<!-- END SUMMARY -->"

DRIVER_HEADING_RE = re.compile(r"^###\s+(.+?)\s*$")
STATUS_LINE_RE = re.compile(r"`\[(?P<mark>[ x~!])\]`\s*Status:")
CHECKBOX_RE = re.compile(r"^\s*\[(?P<mark>[ x~!])\]")


def parse_roadmap(text: str) -> tuple[dict[str, int], int, int]:  # noqa: PLR0912  # parser/gate dispatch, splitting hurts readability
    """Return ({status_counts}, total_boxes, ticked_boxes).

    status_counts has keys 'DONE', 'WIP', 'BLOCKED', 'TODO'.
    """
    counts = {"DONE": 0, "WIP": 0, "BLOCKED": 0, "TODO": 0}
    total_boxes = 0
    ticked_boxes = 0

    lines = text.splitlines()
    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]
        m = DRIVER_HEADING_RE.match(line)
        if not m:
            i += 1
            continue

        # Look for the Status line within the next ~6 lines.
        status_mark: str | None = None
        for j in range(i + 1, min(i + 8, n)):
            sm = STATUS_LINE_RE.search(lines[j])
            if sm:
                status_mark = sm.group("mark")
                break
        if status_mark is None:
            i += 1
            continue

        if status_mark == "x":
            counts["DONE"] += 1
        elif status_mark == "~":
            counts["WIP"] += 1
        elif status_mark == "!":
            counts["BLOCKED"] += 1
        else:
            counts["TODO"] += 1

        # Walk forward to the section's checkbox block, which sits
        # inside the next fenced ``` ... ``` block. Stop at the
        # next "###" or "##".
        k = i + 1
        while k < n:
            if lines[k].startswith("### ") or lines[k].startswith("## "):
                break
            if lines[k].strip().startswith("```"):
                k += 1
                while k < n and not lines[k].strip().startswith("```"):
                    cb = CHECKBOX_RE.match(lines[k])
                    if cb:
                        total_boxes += 1
                        if cb.group("mark") == "x":
                            ticked_boxes += 1
                    k += 1
                break
            k += 1

        i = max(k, i + 1)

    return counts, total_boxes, ticked_boxes


def render_summary(
    counts: dict[str, int],
    total_boxes: int,
    ticked_boxes: int,
) -> str:
    """Render the summary block that sits between the ROADMAP.md markers.

    Emits the BEGIN/END markers as part of the block, so the result can be
    substituted wholesale and the markers can never be lost by a rewrite.
    """
    total_drivers = sum(counts.values())
    pct = ticked_boxes / total_boxes * 100.0 if total_boxes else 0.0
    return "\n".join(
        [
            BEGIN_MARK,
            f"- Total drivers tracked: {total_drivers}",
            f"- DONE:    {counts['DONE']}",
            f"- WIP:     {counts['WIP']}",
            f"- BLOCKED: {counts['BLOCKED']}",
            f"- TODO:    {counts['TODO']}",
            f"- Checklist coverage: {ticked_boxes}/{total_boxes} boxes ticked ({pct:.1f}%)",
            END_MARK,
        ]
    )


def rewrite(text: str, summary: str) -> str:
    """Replace the marked summary region of ROADMAP.md with ``summary``.

    Raises when either marker is missing rather than appending: without them
    there is no way to know which part of the file is generated, and guessing
    would eventually overwrite hand-written prose.

    Everything outside the markers is preserved byte-for-byte.
    """
    if BEGIN_MARK not in text or END_MARK not in text:
        msg = "ROADMAP.md is missing the BEGIN/END SUMMARY markers"
        raise ValueError(msg)
    pre, _, rest = text.partition(BEGIN_MARK)
    _, _, post = rest.partition(END_MARK)
    return pre + summary + post


def main(argv: list[str]) -> int:
    """Recompute the roadmap summary, or with ``--check`` verify it is current.

    ``--check`` exits 1 when the file would change and writes nothing, so CI
    detects a stale summary instead of quietly regenerating it.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="check mode: exit 1 if the file would change",
    )
    parser.add_argument(
        "--roadmap",
        default=str(ROADMAP_PATH),
        help=f"path to ROADMAP.md (default: {ROADMAP_PATH})",
    )
    args = parser.parse_args(argv)

    path = pathlib.Path(args.roadmap)
    if not path.exists():
        print(f"roadmap_stats.py: not found: {path}", file=sys.stderr)
        return 2

    text = path.read_text(encoding="utf-8")
    counts, total_boxes, ticked_boxes = parse_roadmap(text)
    summary = render_summary(counts, total_boxes, ticked_boxes)

    try:
        new_text = rewrite(text, summary)
    except ValueError as exc:
        print(f"roadmap_stats.py: {exc}", file=sys.stderr)
        return 2

    if new_text == text:
        print(
            f"roadmap_stats.py: unchanged "
            f"(drivers={sum(counts.values())} "
            f"DONE={counts['DONE']} WIP={counts['WIP']} "
            f"BLOCKED={counts['BLOCKED']} TODO={counts['TODO']} "
            f"boxes={ticked_boxes}/{total_boxes})",
            file=sys.stderr,
        )
        return 0

    if args.check:
        print(
            "roadmap_stats.py: ROADMAP.md summary is stale "
            "(run `just docs::record_stats` to refresh)",
            file=sys.stderr,
        )
        return 1

    path.write_text(new_text, encoding="utf-8")
    print(
        f"roadmap_stats.py: rewrote summary "
        f"(drivers={sum(counts.values())} "
        f"DONE={counts['DONE']} WIP={counts['WIP']} "
        f"BLOCKED={counts['BLOCKED']} TODO={counts['TODO']} "
        f"boxes={ticked_boxes}/{total_boxes})",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

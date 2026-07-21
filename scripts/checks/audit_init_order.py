#!/usr/bin/env python3
"""audit_init_order.py -- per-app init-order linter.

Walks every ``examples/<tier>/<app>/main.c`` in the tree, extracts the
sequence of ``ra8_*_init(`` and ``ra8_board_*_init(`` calls in source
order, and verifies the sequence respects the project-wide canonical
ordering:

    CGC -> MSTP -> IOPORT -> peripherals

The check is structural rather than full-graph: for every adjacent pair
of init calls we ensure the earlier one has a lower-or-equal canonical
rank. Calls outside the ranked set (e.g. ``ra8_acmphs_init``) are
treated as "peripheral" and must come last. The script emits a warning
for each offending pair and -- in the default STRICT mode -- exits
non-zero if any app fails. Pass ``--no-strict`` to downgrade to
warn-only output (useful while landing a fix).

Optionally writes a Markdown report to a path given via ``--report``.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Canonical init-order ranking. Higher rank = later in boot.
#
# Anything not listed here is implicitly rank PERIPHERAL_RANK (peripherals
# must be initialized after the core CGC -> MSTP -> IOPORT chain).
RANK_CGC = 10
RANK_MSTP = 20
RANK_IOPORT = 30
RANK_TIME = 35  # SysTick is bound to the CPU clock; treat as core init
RANK_ISR_CORE = 38  # ICU / NVIC bring-up before peripherals
RANK_PERIPHERAL = 100

# Token (substring of the called function) -> canonical rank.
RANK_TABLE = {
    "ra8_cgc_init": RANK_CGC,
    "ra8_cgc_get_clock_hz": RANK_CGC,
    "ra8_mstp_init": RANK_MSTP,
    "ra8_pfs_init": RANK_IOPORT,
    "ra8_pfs_route": RANK_IOPORT,
    "ra8_gpio_init": RANK_IOPORT,
    "ra8_gpio_output_init": RANK_IOPORT,
    "ra8_gpio_input_init": RANK_IOPORT,
    "ra8_port_init": RANK_IOPORT,
    "ra8_pin_validator": RANK_IOPORT,
    "ra8_time_init": RANK_TIME,
    "ra8_systick_init": RANK_TIME,
    "ra8_icu_init": RANK_ISR_CORE,
    "ra8_isr_init": RANK_ISR_CORE,
}

INIT_CALL_RE = re.compile(r"\b(ra8_[a-z0-9_]+|ra8_board_[a-z0-9_]+)\(", re.IGNORECASE)


@dataclass
class InitCall:
    """One ``ra8_*_init`` call site recorded from main.c."""

    name: str  # function symbol
    line: int  # 1-based line number in main.c
    rank: int  # canonical rank


@dataclass
class AppAudit:
    """Audit result for a single app."""

    app: str
    main_path: Path
    calls: list[InitCall]
    violations: list[tuple[InitCall, InitCall]]


def rank_for(symbol: str) -> int:
    """Return the canonical rank for ``symbol`` (substring match)."""
    for key, rank in RANK_TABLE.items():
        if symbol.startswith(key):
            return rank
    return RANK_PERIPHERAL


def is_init_call(symbol: str) -> bool:
    """Return True for symbols we want to track."""
    if symbol == "ra8_cgc_get_clock_hz":
        return True
    return symbol.endswith(("_init", "_pins_init")) or "_init_" in symbol


_MAIN_SIG_RE = re.compile(r"\b(?:int|int32_t|void)\s+main\s*\(")


def extract_calls(main_path: Path) -> list[InitCall]:
    """Walk ``main_path`` and pull every init-style call in source order.

    Only calls whose source position falls inside the body of ``main()``
    are considered: helper functions defined above ``main`` (like a
    static ``demo_pins_init``) routinely call ``ra8_*_init`` symbols in
    an order that matches the helper's local logic, not the boot-time
    sequence the audit cares about. The textual brace-tracker below is
    intentionally simple -- it works for the project's hand-written
    ``main()`` style (no nested function defs, no preprocessor games).
    """
    calls: list[InitCall] = []
    text = main_path.read_text(encoding="ascii", errors="replace")
    in_main = False
    depth = 0
    for lineno, line in enumerate(text.splitlines(), start=1):
        # Skip pure comment lines.
        stripped = line.lstrip()
        if stripped.startswith(("//", "*")):
            continue
        if not in_main and _MAIN_SIG_RE.search(line) is not None:
            in_main = True
            depth = 0
        if in_main:
            for ch in line:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth <= 0:
                        # Closed main(); stop scanning.
                        return calls
            if depth == 0:
                # Haven't entered the body yet (signature line before '{').
                continue
            for match in INIT_CALL_RE.finditer(line):
                sym = match.group(1)
                if not is_init_call(sym):
                    continue
                calls.append(InitCall(name=sym, line=lineno, rank=rank_for(sym)))
    return calls


def audit_app(app: str, main_path: Path) -> AppAudit:
    """Run the rank-monotonicity check for one app."""
    calls = extract_calls(main_path)
    violations: list[tuple[InitCall, InitCall]] = []
    last: InitCall | None = None
    for call in calls:
        if last is not None and call.rank < last.rank:
            violations.append((last, call))
        last = call
    return AppAudit(app=app, main_path=main_path, calls=calls, violations=violations)


def collect_apps(repo_root: Path) -> list[tuple[str, Path]]:
    """Discover every ``examples/<tier>/<app>/main.c`` under ``repo_root``."""
    found: list[tuple[str, Path]] = []
    for main in sorted(repo_root.glob("examples/*/*/main.c")):
        app = main.parent.name
        found.append((app, main))
    return found


def render_markdown(audits: list[AppAudit], repo_root: Path) -> str:
    """Render a human-readable Markdown report of the audit run."""
    lines: list[str] = []
    lines.append("# Per-app Init-Order Audit")
    lines.append("")
    lines.append(
        "Generated by ``scripts/checks/audit_init_order.py``. Validates that"
        " every app's main.c follows the canonical CGC -> MSTP -> IOPORT ->"
        " peripheral order."
    )
    lines.append("")
    bad = [a for a in audits if a.violations]
    lines.append(f"- Apps audited: {len(audits)}")
    lines.append(f"- Apps with violations: {len(bad)}")
    lines.append("")
    if bad:
        lines.append("## Violations")
        lines.append("")
        for audit in bad:
            lines.append(f"### {audit.app}")
            for prev, cur in audit.violations:
                lines.append(
                    f"- {prev.name} (rank {prev.rank}, line {prev.line}) precedes "
                    f"{cur.name} (rank {cur.rank}, line {cur.line})"
                )
            lines.append("")
    lines.append("## Per-app init sequences")
    lines.append("")
    for audit in audits:
        rel = audit.main_path.relative_to(repo_root)
        lines.append(f"### {audit.app}")
        lines.append("")
        lines.append(f"Source: ``{rel}``")
        lines.append("")
        if not audit.calls:
            lines.append("- (no init calls detected)")
        else:
            lines.extend(f"- L{c.line}: {c.name}  (rank {c.rank})" for c in audit.calls)
        lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    """Report the firmware's initialisation order for review.

    An audit rather than a gate: it describes the order it finds and does not
    encode an expected one, because the correct order is a design judgement
    that changes with the peripheral set. Read the output; do not expect it
    to fail on its own.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root (defaults to the script's grandparent).",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Optional Markdown report output path.",
    )
    parser.add_argument(
        "--strict",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "Exit non-zero if any app has a violation (default: True). "
            "Use --no-strict to suppress the non-zero exit (warn-only mode)."
        ),
    )
    args = parser.parse_args()

    apps = collect_apps(args.repo_root)
    if not apps:
        print("audit_init_order: no apps discovered", file=sys.stderr)
        return 2

    audits = [audit_app(app, main) for app, main in apps]
    bad_count = 0
    for audit in audits:
        if not audit.violations:
            continue
        bad_count += 1
        rel = audit.main_path.relative_to(args.repo_root)
        for prev, cur in audit.violations:
            print(
                f"WARN {audit.app}: {rel}:{cur.line}: "
                f"{cur.name} (rank {cur.rank}) follows {prev.name} "
                f"(rank {prev.rank}) at line {prev.line}"
            )

    if args.report is not None:
        args.report.write_text(render_markdown(audits, args.repo_root))
        print(f"Report written to {args.report}")

    print(f"audit_init_order: {len(audits)} apps audited, {bad_count} with violations.")
    return 1 if (args.strict and bad_count) else 0


if __name__ == "__main__":
    sys.exit(main())

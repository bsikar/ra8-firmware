#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""audit_init_order.py -- per-app init-order linter.

Walks every ``main.c`` under ``examples/`` at ANY depth, extracts the
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
import tempfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report  # needs the sys.path line above

# Minimum number of apps a healthy discovery finds. Measured at 217; the floor
# sits well below that so adding or removing apps does not trip it, but far
# above the 11 the old depth-capped glob returned -- which is the number this
# floor exists to reject.
APP_FLOOR = 150

# Number of init calls in the selftest's well-ordered fixture. Named so the
# assertion reads as an expectation rather than a bare literal.
SELFTEST_ORDERED_CALLS = 3

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
    """Discover every app ``src/main.c`` under ``examples/``, at any depth.

    The glob used to be ``examples/*/*/main.c`` -- exactly three levels -- while
    the tree's real layout is ``examples/<tier>/.../<app>/``, up to five deep.
    It therefore audited 11 of 217 apps and reported "0 with violations" on the
    other 206, for as long as it had been wired into the pre-commit hook and the
    ``pre-commit-checks`` gate. The same depth-capped glob had already been
    found and fixed in the clang-tidy file collector; this is the second
    instance, so the fix here is the recursive form plus the floor in ``main()``
    that makes a collapsed discovery fail instead of pass.
    """
    return [
        (main.parent.parent.name, main) for main in sorted(repo_root.glob("examples/**/src/main.c"))
    ]


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


def selftest() -> int:
    """Prove the ordering detector and the discovery floor, in both directions.

    The two properties are asserted separately on purpose. A detector that has
    stopped matching and a tree that is genuinely ordered produce identical
    output, and so do a collapsed glob and an empty ``examples/``; only an
    explicit assertion tells them apart.

    Returns:
        0 when every assertion held, 1 otherwise.
    """
    failures: list[str] = []
    root = Path(__file__).resolve().parents[2]

    with tempfile.TemporaryDirectory() as tmp:
        app = Path(tmp) / "examples" / "tier" / "group" / "deep" / "app"
        src = app / "src"
        src.mkdir(parents=True)

        # Written in the project's multi-line main() style: extract_calls walks
        # braces to stay inside main()'s body, so a one-line body would open and
        # close before any call is seen and BOTH directions would pass vacuously.
        def app_main(*calls: str) -> str:
            body = "\n".join(f"  {call}();" for call in calls)
            return f"int main(void)\n{{\n{body}\n  return 0;\n}}\n"

        (src / "main.c").write_text(app_main("ra8_cgc_init", "ra8_mstp_init", "ra8_sci_init"))
        found = collect_apps(Path(tmp))
        expect(len(found) == 1, "recursive discovery reaches a nested app src/main.c", failures)
        ordered = audit_app("app", src / "main.c")
        expect(
            len(ordered.calls) == SELFTEST_ORDERED_CALLS,
            "all three init calls are extracted from main()",
            failures,
        )
        expect(not ordered.violations, "a correctly ordered app stays quiet", failures)

        (src / "main.c").write_text(app_main("ra8_sci_init", "ra8_cgc_init"))
        inverted = audit_app("app", src / "main.c")
        expect(bool(inverted.violations), "a peripheral before CGC fires", failures)

    live = collect_apps(root)
    expect(
        len(live) >= APP_FLOOR,
        f"live discovery sees {len(live)} app(s) (floor {APP_FLOOR})",
        failures,
    )
    return report(failures)


def build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser for this gate."""
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
        "--selftest",
        action="store_true",
        help="prove the ordering detector fires on a bad sequence and spares a good one",
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
    return parser


def main() -> int:
    """Gate the canonical CGC -> MSTP -> IOPORT -> peripheral init order.

    Strict by default: an app whose init calls run out of canonical order
    fails the run. ``--no-strict`` downgrades to warn-only output, which is
    useful while landing a fix but is not what any caller in this tree passes.

    Discovery is floored. A glob that stops matching does not report
    violations it cannot see -- it reports a clean tree, which reads as an
    improvement. So a discovery below ``APP_FLOOR`` is a hard error rather
    than a quiet success.

    Returns:
        0 when every discovered app is ordered correctly, 1 when any is not
        (strict mode), and 2 when discovery collapsed.
    """
    args = build_parser().parse_args()

    if args.selftest:
        return selftest()

    apps = collect_apps(args.repo_root)
    if len(apps) < APP_FLOOR:
        print(
            f"audit_init_order: FATAL -- discovered only {len(apps)} app(s) under "
            f"{args.repo_root / 'examples'}, below the floor of {APP_FLOOR}.\n"
            "  A collapsed discovery reports success because it saw nothing. The\n"
            "  glob was depth-capped at three levels for the life of this gate and\n"
            "  audited 11 of 217 apps; the floor exists so that cannot recur.",
            file=sys.stderr,
        )
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

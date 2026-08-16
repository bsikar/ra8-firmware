# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Deterministic fixtures for the MISRA deviation-register checker self-test."""

from __future__ import annotations

Case = tuple[str, str, str, str, int]


def _fixture_baseline(rules: list[str]) -> tuple[str, list[tuple[str, str, int]]]:
    """Build the synthetic baseline and return its parsed source rows."""
    base_rows = [("a.c", rules[0], 3), ("b.c", rules[0], 1)]
    base_rows += [(f"f{i}.c", rules[i - 1], i) for i in range(2, len(rules) + 1)]
    total = sum(count for _file, _rule, count in base_rows)
    baseline = "\n".join(
        [
            "# fixture baseline",
            "# cppcheck: Cppcheck 9.9.9",
            f"# total findings: {total}",
            "# columns: file<TAB>rule<TAB>count",
            *[f"{file}\t{rule}\t{count}" for file, rule, count in sorted(base_rows)],
            "",
        ]
    )
    return baseline, base_rows


def _fixture_suppressions(rules: list[str]) -> str:
    """Build the synthetic cppcheck suppression list."""
    return "\n".join(
        [
            "# fixture suppressions",
            "unusedStructMember:inc/*.h",
            f"{rules[0]}:a.c:10",
            f"{rules[0]}:a.c:20",
            f"{rules[1]}:glob/*",
            *[f"{rule}:filler{i}.c" for i, rule in enumerate(rules[2:12])],
            "",
        ]
    )


def _fixture_index_lines(
    rules: list[str], base_rows: list[tuple[str, str, int]]
) -> tuple[list[str], list[tuple[str, str]]]:
    """Build the register index and machine-derived population lines."""
    deviations = [(f"D-{i:03d}", rules[i - 1]) for i in range(1, 7)]
    register_rows = [
        f"| {deviation} | {rule} | Advisory | Fixture | Active | 2027-01-01 "
        f"| {4 if rule == rules[0] else int(rule.split('-')[2].split('.')[0])} "
        f"| {2 if rule == rules[0] else 1} |"
        for deviation, rule in deviations
    ]
    residual_rules = rules[6:]
    residual_findings = sum(int(rule.split("-")[2].split(".")[0]) for rule in residual_rules)
    total = sum(count for _file, _rule, count in base_rows)
    lines = [
        "# Fixture register",
        "",
        "Fixture header (D-001..D-006 active).",
        "",
        "## Deviation index",
        "",
        "| ID    | Rule | Category | Class | Status | MAR | Findings | Files |",
        "|-------|------|----------|-------|--------|-----|---------:|------:|",
        *register_rows,
        "",
        "## Derived population",
        "",
        f"Baseline: {total} findings across {len(base_rows)} file/rule rows (Cppcheck 9.9.9).",
        "",
        f"Residual (no deviation record): {len(residual_rules)} rules, "
        f"{residual_findings} findings, {len(residual_rules)} rows.",
        "",
    ]
    return lines, deviations


def _fixture_detail_lines(rules: list[str], deviations: list[tuple[str, str]]) -> list[str]:
    """Build ownership, excerpt, and deviation-section fixture lines."""
    return [
        f"- `{rules[0]}` (2 rows, 1 path): fixture family one.",
        f"- `{rules[1]}` (1 row, 1 path): fixture family two.",
        *[f"- `{rule}` (1 row, 1 path): fixture filler family." for rule in rules[2:12]],
        "",
        f"Highest-count files for {rules[0]} (top 2, derived):",
        "",
        "| File | Findings |",
        "|------|---------:|",
        "| `a.c` | 3 |",
        "| `b.c` | 1 |",
        "",
        *[
            f"## {deviation}: Rule {rule.removeprefix('misra-c2012-')} -- fixture\n"
            for deviation, rule in deviations
        ],
        "Population note under the last section: 6 findings across 1 files.",
        "",
    ]


def fixture_files() -> dict[str, str]:
    """Build a mutually consistent register, baseline, and suppression fixture."""
    rules = [f"misra-c2012-{i}.1" for i in range(1, 33)]
    baseline, base_rows = _fixture_baseline(rules)
    index_lines, deviations = _fixture_index_lines(rules, base_rows)
    document = "\n".join(index_lines + _fixture_detail_lines(rules, deviations))
    return {
        "doc": document,
        "baseline": baseline,
        "suppressions": _fixture_suppressions(rules),
    }


def _register_cases(exit_drift: int, exit_malformed: int) -> list[Case]:
    """Return index-row and heading mutation cases."""
    return [
        (
            "stale register count fires",
            "doc",
            "2027-01-01 | 4 | 2 |",
            "2027-01-01 | 5 | 2 |",
            exit_drift,
        ),
        (
            "missing register row fires",
            "doc",
            "| D-003 | misra-c2012-3.1 | Advisory | Fixture | Active | 2027-01-01 | 3 | 1 |\n",
            "",
            exit_drift,
        ),
        (
            "extra register row fires",
            "doc",
            "## Derived population",
            "| D-099 | misra-c2012-31.1 | Advisory | Fixture | Active | 2027-01-01 | 31 | 1 |\n"
            "\n## Derived population",
            exit_drift,
        ),
        (
            "misshapen register row is malformed",
            "doc",
            "| D-002 | misra-c2012-2.1 | Advisory | Fixture | Active | 2027-01-01 | 2 | 1 |",
            "| D-002 | misra-c2012-2.1 | 2 | 1 |",
            exit_malformed,
        ),
        (
            "heading rule mismatch fires",
            "doc",
            "## D-002: Rule 2.1 -- fixture",
            "## D-002: Rule 2.2 -- fixture",
            exit_drift,
        ),
    ]


def _claim_cases(exit_drift: int, exit_malformed: int) -> list[Case]:
    """Return derived-claim and suppression-ownership mutation cases."""
    return [
        ("missing provenance line is malformed", "doc", "Baseline: ", "Basel1ne: ", exit_malformed),
        (
            "stale residual fires",
            "doc",
            "Residual (no deviation record): 26 rules,",
            "Residual (no deviation record): 27 rules,",
            exit_drift,
        ),
        (
            "suppression row drift fires",
            "suppressions",
            "misra-c2012-1.1:a.c:20",
            "misra-c2012-1.1:a.c:20\nmisra-c2012-1.1:c.c:9",
            exit_drift,
        ),
        (
            "unowned suppression family fires",
            "suppressions",
            "misra-c2012-2.1:glob/*",
            "misra-c2012-2.1:glob/*\nmisra-c2012-3.1:zzz.c",
            exit_drift,
        ),
        (
            "ghost ownership bullet fires",
            "suppressions",
            "misra-c2012-2.1:glob/*\n",
            "",
            exit_drift,
        ),
    ]


def _evidence_cases(exit_drift: int, exit_malformed: int) -> list[Case]:
    """Return baseline, excerpt, and population mutation cases."""
    return [
        (
            "misordered excerpt fires",
            "doc",
            "| `a.c` | 3 |\n| `b.c` | 1 |",
            "| `b.c` | 1 |\n| `a.c` | 3 |",
            exit_drift,
        ),
        (
            "tampered baseline header is malformed",
            "baseline",
            "# total findings: ",
            "# total findings: 1",
            exit_malformed,
        ),
        (
            "vacuous baseline is malformed",
            "baseline",
            "a.c\tmisra-c2012-1.1\t3",
            "a.c\tmisra-c2012-1.1\tX",
            exit_malformed,
        ),
        (
            "two-column baseline row is malformed",
            "baseline",
            "b.c\tmisra-c2012-1.1\t1",
            "b.c\tmisra-c2012-1.1",
            exit_malformed,
        ),
        (
            "stale in-section population fires",
            "doc",
            "6 findings across 1 files.",
            "7 findings across 1 files.",
            exit_drift,
        ),
    ]


def _shape_cases(exit_drift: int, exit_malformed: int) -> list[Case]:
    """Return malformed-shape and deleted-evidence mutation cases."""
    return [
        (
            "wrapped excerpt intro is malformed",
            "doc",
            "Highest-count files for misra-c2012-1.1 (top 2, derived):",
            "Highest-count files for misra-c2012-1.1\n(top 2, derived):",
            exit_malformed,
        ),
        (
            "short deviation-id register row is malformed",
            "doc",
            "## Derived population",
            "| D-11 | misra-c2012-31.1 | Advisory | Fixture | Active | 2027-01-01 | 31 | 1 |\n"
            "\n## Derived population",
            exit_malformed,
        ),
        (
            "short deviation-id heading is malformed",
            "doc",
            "## D-002: Rule 2.1 -- fixture",
            "## D-11: Rule 2.1 -- fixture",
            exit_malformed,
        ),
        (
            "deleting the whole excerpt block is malformed",
            "doc",
            "Highest-count files for misra-c2012-1.1 (top 2, derived):\n\n"
            "| File | Findings |\n|------|---------:|\n| `a.c` | 3 |\n| `b.c` | 1 |\n",
            "",
            exit_malformed,
        ),
        (
            "deleting one ownership bullet fires as an unowned waiver",
            "doc",
            "- `misra-c2012-1.1` (2 rows, 1 path): fixture family one.",
            "",
            exit_drift,
        ),
    ]


def _retirement_cases(exit_drift: int, exit_malformed: int) -> list[Case]:
    """Return population-placement and deviation-retirement mutation cases."""
    return [
        (
            "population claim outside a deviation section is malformed",
            "doc",
            "## Derived population",
            "Rule 1.1 alone accounts for 4 findings across 2 files.\n\n## Derived population",
            exit_malformed,
        ),
        (
            "population claim reworded with 'in' still fires",
            "doc",
            "6 findings across 1 files.",
            "7 findings in 1 file.",
            exit_drift,
        ),
        (
            "retiring a deviation without amending the header range fires",
            "doc",
            "| D-006 | misra-c2012-6.1 | Advisory | Fixture | Active | 2027-01-01 | 6 | 1 |\n",
            "",
            exit_drift,
        ),
        (
            "a register row naming a rule absent from the baseline fires",
            "doc",
            "| D-006 | misra-c2012-6.1 | Advisory | Fixture | Active | 2027-01-01 | 6 | 1 |",
            "| D-006 | misra-c2012-99.9 | Advisory | Fixture | Active | 2027-01-01 | 0 | 0 |",
            exit_drift,
        ),
        (
            "a missing header range is malformed",
            "doc",
            "Fixture header (D-001..D-006 active).",
            "Fixture header.",
            exit_drift,
        ),
    ]


def selftest_cases(exit_ok: int, exit_drift: int, exit_malformed: int) -> list[Case]:
    """Enumerate all mutation cases and the quiet control."""
    return [
        ("clean fixture passes", "doc", "", "", exit_ok),
        *_register_cases(exit_drift, exit_malformed),
        *_claim_cases(exit_drift, exit_malformed),
        *_evidence_cases(exit_drift, exit_malformed),
        *_shape_cases(exit_drift, exit_malformed),
        *_retirement_cases(exit_drift, exit_malformed),
    ]

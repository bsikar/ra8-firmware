#!/usr/bin/env python3
"""A count written into a document is a measurement, so measure it.

Several pages in this tree argue a decision from counts of the tree itself:
`docs/PORTS.md` argues the build-first port order from how many example files
reach past the board for each peripheral, and `arch/README.md` argues the
#694 migration order from how many first-party files include each misfiled
Armv8-M header. The counts are load-bearing prose, and both pages were written
the same way: the command run by hand once, the number pasted, the tree left
free to move underneath it. `docs/PORTS.md` had four figures rot before a gate
owned them; `arch/README.md` had all four rot inside a week.

So the mechanism is the page's, not this script's. A page opts in by carrying a
fenced MEASURED BLOCK: one entry per figure, each naming the table row it
backs, the count it claims, and the command that produces it. This checker
finds every page carrying that marker, re-runs every entry against the tree,
and fails when the manifest count, or the table cell it names, has drifted.
Both directions matter equally: a count that grew silently is a migration
going backwards, and a count that shrank silently is progress the page is not
crediting.

Everything checked is DERIVED from the pages. The rows, the patterns, the
qualifiers and the roots all come out of the manifests, so a new page is
checked as soon as it carries a block, and there is no second place in this
script to keep in step.

What it reports:

  - miscounted-measurement: the manifest claims N, the tree says M.
  - undocumented-row: a table row carries a count with no manifest entry
    behind it, which is a number nobody can reproduce.
  - unbound-measurement: a manifest entry names a table row that no table has.
  - contradicted-row: the manifest and the table disagree about the same
    figure.
  - unparsable-measurement: an entry in the manifest block that is not a
    measurement, so the block cannot be read as a whole.

Usage:
  scripts/checks/check_measured_counts.py [--check] [--update] [--selftest]

`--update` rewrites the manifest counts and the table cells to what the tree
says, for the case where the drift is real progress being banked.

Exit codes: 0 clean, 1 findings, 2 usage/internal error.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Pages opt in by carrying the marker; these are the trees searched for them.
PAGE_ROOTS = ("docs", "arch", "libs", "apps", "scripts")
SKIP_DIRS = frozenset({"third_party", "node_modules"})

# A read that finds fewer than this did not find tidier pages; it collapsed.
MEASUREMENT_FLOOR = 8
PAGE_FLOOR = 2
SCANNED_FILE_FLOOR = 100

BLOCK_MARKER = "MEASURED BLOCK"
FENCE_RE = re.compile(r"^```")
# "# clock / CGC -- 227 file(s)" and "# timer / counter / capture [GPT] -- 13 file(s)"
CLAIM_RE = re.compile(
    r"^#\s*(?P<row>.+?)\s*(?:\[(?P<qual>[A-Za-z0-9/_-]+)\])?\s*--\s*(?P<count>\d+)\s*file\(s\)\s*$"
)
# The command is the specification, so it is parsed rather than trusted.
COMMAND_RE = re.compile(
    r"^grep -rlE '(?P<pattern>[^']+)' (?P<roots>[A-Za-z0-9_./-]+(?: [A-Za-z0-9_./-]+)*)"
    r"(?P<includes>(?: --include=\*\.[A-Za-z0-9]+)+)"
    r"(?P<filter>(?: \| grep -v /third_party/)?) \| wc -l\s*$"
)
# The population rows are a file count, not a match count, so the page states
# them as the find that produces them.
FIND_RE = re.compile(
    r"^find (?P<root>[A-Za-z0-9_./-]+) \\\( (?P<names>-name '\*\.[A-Za-z0-9]+'"
    r"(?: -o -name '\*\.[A-Za-z0-9]+')*) \\\) -type f \| wc -l\s*$"
)
INCLUDE_RE = re.compile(r"--include=\*(\.[A-Za-z0-9]+)")
NAME_RE = re.compile(r"-name '\*(\.[A-Za-z0-9]+)'")
TABLE_ROW_RE = re.compile(r"^\|(?P<body>.*)\|\s*$")
TABLE_RULE_RE = re.compile(r"^\|[\s:|-]+\|\s*$")
CELL_COUNT_RE = re.compile(r"\d+")


class CheckError(RuntimeError):
    """A read that cannot be trusted, as distinct from a finding."""


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        raise CheckError("not inside a git work tree")
    return Path(out.stdout.strip())


def strip_markup(text: str) -> str:
    """A row label is compared as prose, not as markdown."""
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\*\*([^*]*)\*\*", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return " ".join(text.split())


class Measurement:
    """One figure the page claims, with the command that produces it."""

    def __init__(
        self,
        row: str,
        qualifier: str | None,
        claimed: int,
        pattern: str | None,
        roots: tuple[str, ...],
        suffixes: tuple[str, ...],
        claim_line: int,
        skip_third_party: bool = False,
    ) -> None:
        self.row = row
        self.qualifier = qualifier
        self.claimed = claimed
        self.pattern = pattern
        self.roots = roots
        self.suffixes = suffixes
        self.claim_line = claim_line
        self.skip_third_party = skip_third_party

    @property
    def key(self) -> str:
        return f"{self.row} [{self.qualifier}]" if self.qualifier else self.row

    def cell_text(self, count: int) -> str:
        return f"{count} {self.qualifier}" if self.qualifier else str(count)

    def measure(self, tree: dict[str, str]) -> int:
        """Re-run the documented command over `tree`, a path -> text map."""
        matcher = None
        if self.pattern is not None:
            try:
                matcher = re.compile(self.pattern)
            except re.error as exc:  # pragma: no cover - guarded by a finding
                raise CheckError(
                    f"{self.key}: unreadable pattern {self.pattern!r}: {exc}"
                ) from exc
        prefixes = tuple(root.rstrip("/") + "/" for root in self.roots)
        hits = 0
        for path, text in tree.items():
            if not path.startswith(prefixes):
                continue
            if self.skip_third_party and "/third_party/" in path:
                continue
            if not any(path.endswith(suffix) for suffix in self.suffixes):
                continue
            if matcher is None or matcher.search(text):
                hits += 1
        return hits


def read_tree(root: Path, subdirs: set[str], suffixes: set[str]) -> dict[str, str]:
    """Every file a manifest entry could name, read once."""
    tree: dict[str, str] = {}
    for subdir in sorted(subdirs):
        base = root / subdir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in suffixes:
                continue
            try:
                tree[path.relative_to(root).as_posix()] = path.read_text(
                    encoding="utf-8", errors="replace"
                )
            except OSError as exc:
                raise CheckError(f"cannot read {path}: {exc}") from exc
    return tree


def parse_manifest(
    lines: list[str], rel: str = "page"
) -> tuple[list[Measurement], list[tuple[int, str]]]:
    """Read the fenced block the page marks as its manifest."""
    start = None
    for index, line in enumerate(lines):
        if BLOCK_MARKER in line:
            start = index
            break
    if start is None:
        raise CheckError(f"{rel} carries no {BLOCK_MARKER} marker")

    open_fence = None
    for index in range(start, -1, -1):
        if FENCE_RE.match(lines[index]):
            open_fence = index
            break
    if open_fence is None:
        raise CheckError(f"the {BLOCK_MARKER} marker is not inside a fenced block")

    close_fence = None
    for index in range(open_fence + 1, len(lines)):
        if FENCE_RE.match(lines[index]):
            close_fence = index
            break
    if close_fence is None:
        raise CheckError(f"the {BLOCK_MARKER} block is never closed")

    measurements: list[Measurement] = []
    unparsable: list[tuple[int, str]] = []
    pending: tuple[re.Match[str], int] | None = None

    for index in range(open_fence + 1, close_fence):
        raw = lines[index]
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            claim = CLAIM_RE.match(line)
            if claim:
                if pending is not None:
                    unparsable.append((pending[1] + 1, "claim with no command under it"))
                pending = (claim, index)
            continue
        command = COMMAND_RE.match(line)
        population = None if command else FIND_RE.match(line)
        if command is None and population is None:
            unparsable.append((index + 1, line))
            pending = None
            continue
        if pending is None:
            unparsable.append((index + 1, "command with no claim above it"))
            continue
        claim, claim_index = pending
        pending = None
        if command is not None:
            pattern = command.group("pattern")
            roots = tuple(command.group("roots").split())
            suffixes = tuple(INCLUDE_RE.findall(command.group("includes")))
            skip_third_party = bool(command.group("filter").strip())
        else:
            pattern = None
            roots = (population.group("root"),)
            suffixes = tuple(NAME_RE.findall(population.group("names")))
            skip_third_party = False
        measurements.append(
            Measurement(
                row=strip_markup(claim.group("row")),
                qualifier=claim.group("qual"),
                claimed=int(claim.group("count")),
                pattern=pattern,
                roots=roots,
                suffixes=suffixes,
                claim_line=claim_index,
                skip_third_party=skip_third_party,
            )
        )
    if pending is not None:
        unparsable.append((pending[1] + 1, "claim with no command under it"))

    return measurements, unparsable


def parse_tables(lines: list[str]) -> list[tuple[int, str, list[str]]]:
    """Every markdown table row, as (line index, first cell, all cells)."""
    rows: list[tuple[int, str, list[str]]] = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        match = TABLE_ROW_RE.match(stripped)
        if match is None or TABLE_RULE_RE.match(stripped):
            continue
        cells = [cell.strip() for cell in match.group("body").split("|")]
        if not cells:
            continue
        rows.append((index, strip_markup(cells[0]), cells))
    return rows


def analyse(
    text: str, tree: dict[str, str], rel: str = "page"
) -> tuple[list[str], list[Measurement], dict[str, int]]:
    """Report every drift between the page, its manifest and the tree."""
    lines = text.splitlines()
    measurements, unparsable = parse_manifest(lines, rel)
    rows = parse_tables(lines)

    findings: list[str] = []
    for line_no, detail in unparsable:
        findings.append(f"{rel}:{line_no}: unparsable-measurement: {detail}")

    by_row: dict[str, list[tuple[int, list[str]]]] = {}
    for index, label, cells in rows:
        by_row.setdefault(label, []).append((index, cells))

    measured: dict[str, int] = {}
    for measurement in measurements:
        actual = measurement.measure(tree)
        measured[measurement.key] = actual
        if actual != measurement.claimed:
            findings.append(
                f"{rel}:{measurement.claim_line + 1}: miscounted-measurement: "
                f"{measurement.key} claims {measurement.claimed}, the tree says {actual}"
            )

        candidates = by_row.get(measurement.row)
        if not candidates:
            findings.append(
                f"{rel}:{measurement.claim_line + 1}: unbound-measurement: "
                f"no table row named {measurement.row!r}"
            )
            continue
        wanted = measurement.cell_text(measurement.claimed)
        if not any(
            any(wanted == cell or wanted in cell for cell in cells) for _, cells in candidates
        ):
            index = candidates[0][0]
            findings.append(
                f"{rel}:{index + 1}: contradicted-row: {measurement.row} "
                f"does not carry {wanted!r} the manifest claims for it"
            )

    documented = {measurement.row for measurement in measurements}
    for index, label, cells in rows:
        if label in documented or not label:
            continue
        for cell in cells[1:]:
            bare = strip_markup(cell)
            if CELL_COUNT_RE.fullmatch(bare) or re.fullmatch(r"\d+ [A-Za-z0-9/_-]+", bare):
                findings.append(
                    f"{rel}:{index + 1}: undocumented-row: {label} carries "
                    f"the count {bare!r} with no manifest entry behind it"
                )
                break

    counts = {
        "measurements": len(measurements),
        "rows": len(rows),
        "scanned": len(tree),
    }
    return findings, measurements, counts


def rewrite(text: str, tree: dict[str, str]) -> str:
    """Bank the tree's numbers into the manifest and the rows it names."""
    lines = text.splitlines()
    measurements, _ = parse_manifest(lines)
    for measurement in measurements:
        actual = measurement.measure(tree)
        if actual == measurement.claimed:
            continue
        old_cell = measurement.cell_text(measurement.claimed)
        new_cell = measurement.cell_text(actual)
        claim = lines[measurement.claim_line]
        lines[measurement.claim_line] = claim.replace(
            f"-- {measurement.claimed} file(s)", f"-- {actual} file(s)"
        )
        for index, label, cells in parse_tables(lines):
            if label != measurement.row:
                continue
            updated = [
                cell.replace(old_cell, new_cell) if old_cell in cell else cell for cell in cells
            ]
            if updated != cells:
                lines[index] = "| " + " | ".join(updated) + " |"
                break
        measurement.claimed = actual
    return "\n".join(lines) + "\n"


def needed_scope(measurements: list[Measurement]) -> tuple[set[str], set[str]]:
    subdirs = {
        root.strip("/") for measurement in measurements for root in measurement.roots
    }
    suffixes = {suffix for measurement in measurements for suffix in measurement.suffixes}
    return subdirs, suffixes


def discover_pages(root: Path) -> list[str]:
    """Every markdown page that opted in by carrying the marker."""
    pages: list[str] = []
    for top in PAGE_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.md")):
            if SKIP_DIRS & set(path.relative_to(root).parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise CheckError(f"cannot read {path}: {exc}") from exc
            if BLOCK_MARKER in text:
                pages.append(path.relative_to(root).as_posix())
    return pages


def run(root: Path, update: bool) -> int:
    pages = discover_pages(root)
    if len(pages) < PAGE_FLOOR:
        raise CheckError(
            f"found {len(pages)} page(s) carrying a {BLOCK_MARKER}, floor is "
            f"{PAGE_FLOOR}; the discovery, not the tree, is what shrank"
        )

    provisional: list[Measurement] = []
    texts: dict[str, str] = {}
    for rel in pages:
        text = (root / rel).read_text(encoding="utf-8")
        texts[rel] = text
        entries, _ = parse_manifest(text.splitlines(), rel)
        if not entries:
            raise CheckError(f"{rel} carries a {BLOCK_MARKER} with no measurement in it")
        provisional.extend(entries)
    if len(provisional) < MEASUREMENT_FLOOR:
        raise CheckError(
            f"the manifests hold {len(provisional)} measurement(s), floor is "
            f"{MEASUREMENT_FLOOR}; a block or its grammar collapsed"
        )

    subdirs, suffixes = needed_scope(provisional)
    tree = read_tree(root, subdirs, suffixes)
    if len(tree) < SCANNED_FILE_FLOOR:
        raise CheckError(
            f"scanned {len(tree)} file(s) under {sorted(subdirs)}, floor is "
            f"{SCANNED_FILE_FLOOR}; the scan, not the tree, is what shrank"
        )

    findings: list[str] = []
    measured = 0
    rows = 0
    for rel in pages:
        text = texts[rel]
        if update:
            (root / rel).write_text(rewrite(text, tree), encoding="utf-8")
            text = (root / rel).read_text(encoding="utf-8")
        page_findings, _, counts = analyse(text, tree, rel)
        findings.extend(page_findings)
        measured += counts["measurements"]
        rows += counts["rows"]

    verb = "re-pinned" if update else "checked"
    print(
        f"measured-counts: {verb} {measured} measurement(s) across {len(pages)} "
        f"page(s) over {len(tree)} scanned file(s) in {rows} table row(s)."
    )
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        if not update:
            print(
                f"measured-counts: {len(findings)} finding(s). Re-run the command "
                "each entry names; if the drift is real, "
                "`scripts/checks/check_measured_counts.py --update` banks it.",
                file=sys.stderr,
            )
        return 1
    print("measured-counts: every documented count matches the tree.")
    return 0


########################################################################
# Selftest. Both directions, on constructed pages, so a matcher that
# stopped matching cannot report a clean catalog.
########################################################################

_TREE = {
    "examples/a/main.c": "ra8_cgc_start(); ra8_gpt_open();",
    "examples/b/main.c": "ra8_cgc_stop();",
    "examples/c/main.h": "void none(void);",
    "examples/d/main.c": "ra8_gpt_close();",
    "docs/elsewhere.c": "ra8_cgc_start();",
    "libs/one/src/a.c": '#include "ra8_scb.h"',
    "libs/third_party/vendor/b.c": '#include "ra8_scb.h"',
    "apps/two/main.c": '#include "ra8_scb.h"',
}


def _page(
    clock_claim: int = 2,
    gpt_claim: int = 2,
    scb_claim: int = 2,
    clock_cell: str | None = None,
    gpt_cell: str | None = None,
    scb_cell: str | None = None,
    extra_rows: str = "",
    extra_entries: str = "",
) -> str:
    clock_cell = str(clock_claim) if clock_cell is None else clock_cell
    gpt_cell = f"{gpt_claim} GPT" if gpt_cell is None else gpt_cell
    scb_cell = str(scb_claim) if scb_cell is None else scb_cell
    return f"""# Port catalog

| Port | Coupled example files |
| --- | ---: |
| clock / CGC | {clock_cell} |
| timer / counter / capture | {gpt_cell} |
| `ra8_scb.h` | {scb_cell} |
{extra_rows}
### How the numbers were measured

```sh
# {BLOCK_MARKER}
# clock / CGC -- {clock_claim} file(s)
grep -rlE 'ra8_cgc' examples --include=*.c --include=*.h | wc -l
# timer / counter / capture [GPT] -- {gpt_claim} file(s)
grep -rlE 'ra8_gpt' examples --include=*.c --include=*.h | wc -l
# ra8_scb.h -- {scb_claim} file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_scb\.h"' libs apps --include=*.c --include=*.h | grep -v /third_party/ | wc -l
{extra_entries}```
"""


def _kinds(findings: list[str]) -> list[str]:
    kinds = []
    for finding in findings:
        parts = finding.split(": ")
        kinds.append(parts[1] if len(parts) > 1 else finding)
    return sorted(set(kinds))


def _selftest_cases() -> list[tuple[str, list[str]]]:
    cases: list[tuple[str, list[str]]] = []

    findings, _, counts = analyse(_page(), _TREE, "page.md")
    cases.append(("a page that matches the tree is silent", _kinds(findings)))
    if counts["measurements"] != 3:
        cases.append(("manifest read", ["wrong-measurement-count"]))

    findings, _, _ = analyse(_page(clock_claim=9), _TREE, "page.md")
    cases.append(("a stale count is reported", _kinds(findings)))

    findings, _, _ = analyse(_page(clock_claim=1), _TREE, "page.md")
    cases.append(("a count below the tree is reported too", _kinds(findings)))

    findings, _, _ = analyse(_page(clock_cell="7"), _TREE, "page.md")
    cases.append(("a table cell that drifted from the manifest", _kinds(findings)))

    findings, _, _ = analyse(_page(gpt_cell="2"), _TREE, "page.md")
    cases.append(("a qualifier dropped from the cell", _kinds(findings)))

    findings, _, _ = analyse(
        _page(extra_rows="| serial | 4 |\n"), _TREE, "page.md"
    )
    cases.append(("a row nobody can reproduce", _kinds(findings)))

    findings, _, _ = analyse(
        _page(extra_entries="# gpio -- 0 file(s)\ngrep -rlE 'ra8_gpio' examples --include=*.c | wc -l\n"),
        _TREE,
        "page.md",
    )
    cases.append(("an entry naming no table row", _kinds(findings)))

    findings, _, _ = analyse(
        _page(extra_entries="python3 -c 'print(3)'\n"), _TREE, "page.md"
    )
    cases.append(("a command that is not a measurement", _kinds(findings)))

    findings, _, _ = analyse(
        _page(extra_entries="# display -- 1 file(s)\n"), _TREE, "page.md"
    )
    cases.append(("a claim with no command under it", _kinds(findings)))

    # Scope: the same pattern outside the named root must not count.
    findings, _, _ = analyse(_page(), dict(_TREE, **{"docs/more.c": "ra8_cgc_x();"}), "page.md")
    cases.append(("a hit outside examples/ is not counted", _kinds(findings)))

    # Scope: a suffix the command does not name must not count.
    findings, _, _ = analyse(_page(), dict(_TREE, **{"examples/e/main.cpp": "ra8_cgc_x();"}), "page.md")
    cases.append(("a suffix the command excludes is not counted", _kinds(findings)))

    # A real hit in scope must move the number, or nothing is being measured.
    findings, _, _ = analyse(_page(), dict(_TREE, **{"examples/e/main.c": "ra8_cgc_x();"}), "page.md")
    cases.append(("a new reach-in fails the page", _kinds(findings)))

    # Multi-root: the second root counts, so a hit there must move the number.
    findings, _, _ = analyse(
        _page(), dict(_TREE, **{"apps/three/main.c": '#include "ra8_scb.h"'}), "page.md"
    )
    cases.append(("a hit in the second root is counted", _kinds(findings)))

    # The documented `| grep -v /third_party/` stage has to actually exclude.
    findings, _, _ = analyse(
        _page(),
        dict(_TREE, **{"libs/third_party/other/c.c": '#include "ra8_scb.h"'}),
        "page.md",
    )
    cases.append(("a vendored hit is excluded as the command says", _kinds(findings)))

    return cases


_EXPECTED = [
    [],
    ["miscounted-measurement"],
    ["miscounted-measurement"],
    ["contradicted-row"],
    ["contradicted-row"],
    ["undocumented-row"],
    ["unbound-measurement"],
    ["unparsable-measurement"],
    ["unparsable-measurement"],
    [],
    [],
    ["miscounted-measurement"],
    ["miscounted-measurement"],
    [],
]


def selftest() -> int:
    failures: list[str] = []
    cases = _selftest_cases()
    if len(cases) != len(_EXPECTED):
        print(
            f"selftest: {Path(__file__).name} FAIL: {len(cases)} case(s), "
            f"{len(_EXPECTED)} expectation(s)",
            file=sys.stderr,
        )
        return 1
    for (label, actual), expected in zip(cases, _EXPECTED):
        if actual != expected:
            failures.append(f"{label}: expected {expected}, got {actual}")

    # --update has to end the argument, not restate it.
    banked = rewrite(_page(clock_claim=9, clock_cell="9"), _TREE)
    findings, _, _ = analyse(banked, _TREE, "page.md")
    if findings:
        failures.append(f"--update left {_kinds(findings)} behind")
    if "-- 2 file(s)" not in banked or "| clock / CGC | 2 |" not in banked:
        failures.append("--update did not rewrite both the manifest and the row")

    # The floor is the guard against a grammar that stopped matching.
    empty = (
        _page()
        .replace("# clock / CGC -- 2 file(s)", "")
        .replace("# timer / counter / capture [GPT] -- 2 file(s)", "")
        .replace("# ra8_scb.h -- 2 file(s)", "")
    )
    measurements, unparsable = parse_manifest(empty.splitlines(), "page.md")
    if measurements or not unparsable:
        failures.append("a manifest with no claims must read as empty, not as clean")

    if failures:
        for failure in failures:
            print(f"selftest: {Path(__file__).name} FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"selftest: {Path(__file__).name} OK ({len(cases) + 3} both-direction cases)")
    return 0


def main(argv: list[str]) -> int:
    args = set(argv[1:])
    unknown = args - {"--check", "--update", "--selftest"}
    if unknown:
        print(f"usage: {Path(__file__).name} [--check] [--update] [--selftest]", file=sys.stderr)
        return 2
    if "--selftest" in args:
        return selftest()
    try:
        return run(repo_root(), update="--update" in args)
    except CheckError as exc:
        print(f"measured-counts: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

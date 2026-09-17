#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a ``void`` function carries no ``@return`` tag, tree-wide.

``docs/STYLE_GUIDE.md`` says a function that returns nothing documents what it
leaves behind in ``@post``, not in a ``@return Nothing.`` line.  Until now that
rule was enforced nowhere.  ``doxy_audit.py`` exempts ``void`` from the
*required* tag set and ``check_doc_attachment.py``'s DOC003 deliberately
tolerates a bare ``@return Nothing.``, so both are silent on a tag that should
not be there at all.  The only thing that ever complained was Doxygen itself --
"found documented return type for X that does not return anything" -- and that
warning is only visible for the 364 headers in the C ABI reference's input set
(``libs/*/inc/**/*.h``).  Every other first-party declaration was a blind spot.

This gate closes it by reading source text rather than a generator's log, so it
covers the whole first-party C/C++ tree including sources, tests, examples,
tools and the emulator, none of which any documentation build looks at.

Existing debt is frozen in ``.github/void-return-tag-baseline.txt`` as one row
per ``(file, symbol)`` with the number of offending tags on that symbol's block:

* a ``(file, symbol)`` pair that is not on the ledger, or whose count grew, is a
  REGRESSION;
* a row that no longer fires, or fires less, is STALE and must be re-emitted
  with ``--update``, so paying debt down is recorded rather than absorbed.

Line numbers are deliberately not part of the key: an edit above a block shifts
them, and a ledger that churns on unrelated commits trains everyone to re-emit
it without reading the diff.

MATCHING IS DELIBERATELY CONSERVATIVE.  A false positive here freezes a tag
that is actually correct, which is a lie in the ledger that only surfaces when
someone edits that line, so a declaration counts only when its return type is
unambiguously ``void``:

* the doc block is a Doxygen block (``/**`` or ``/*!``); an ordinary ``/*``
  comment is not documentation and is skipped;
* after stripping a closed set of leading storage/attribute tokens (``static``,
  ``inline``, ``extern``, ``_Noreturn``, ``RA8_*`` macros, ``__attribute__``),
  what remains must begin ``void <identifier>(``;
* ``void *`` is a returning function and never matches;
* ``typedef``, ``#define`` and anything else that is not a function
  declaration or definition never matches.

The cost of that conservatism is that an exotic declaration shape can go
unseen.  That is the right direction to be wrong in for a ratchet: an unseen
offender is debt this gate has not started collecting, while a wrongly-frozen
row is debt it invented.

Run::

    check_void_return_tags.py --check      # CI gate (exit 1 on any finding)
    check_void_return_tags.py --update     # re-emit the ledger from the tree
    check_void_return_tags.py --selftest   # synthetic both-direction fixtures
    check_void_return_tags.py --emit-rows  # what the tree says today
    check_void_return_tags.py FILE...      # scan explicit files (pre-commit)
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE = REPO_ROOT / ".github" / "void-return-tag-baseline.txt"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

#: A return-documenting tag, in either Doxygen tag style.
RETURN_TAG = re.compile(r"[@\\](?:return|returns|retval)\b")

#: Start of a Doxygen block. ``/*`` alone is an ordinary comment.
DOC_OPEN = re.compile(r"^\s*/\*[*!]")

#: Leading tokens that are not the return type. Closed set on purpose: an
#: open-ended "strip anything SHOUTY" rule would eat a macro that IS the
#: return type (``UINT`` in the vendored RTOS style) and read it as ``void``.
LEADING_TOKEN = re.compile(
    r"^(?:static|inline|extern|_Noreturn|RA8_[A-Za-z0-9_]+"
    r"|__attribute__\s*\(\(.*?\)\))\s+"
)

#: What a ``void`` function declaration or definition looks like once the
#: leading tokens are gone. ``void *foo(`` fails this on purpose.
VOID_DECL = re.compile(r"^void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")

#: How many lines after the block a declaration may span before giving up.
DECL_LOOKAHEAD = 4


def strip_leading_tokens(text: str) -> str:
    """Remove storage class and attribute tokens preceding the return type."""
    prev = None
    while prev != text:
        prev = text
        text = LEADING_TOKEN.sub("", text, count=1)
    return text


def scan_text(text: str) -> dict[str, int]:
    """Offending symbols in one file's text.

    Returns:
        ``{symbol: tag_count}`` for every declaration whose return type is
        unambiguously ``void`` and whose attached Doxygen block carries at
        least one return-documenting tag. Two blocks on the same symbol (a
        prototype and its definition) aggregate into one entry, so the ledger
        key stays stable when one of them moves.
    """
    lines = text.splitlines()
    total = len(lines)
    found: dict[str, int] = {}
    i = 0
    while i < total:
        if not DOC_OPEN.match(lines[i]):
            i += 1
            continue
        # Collect the block. A single-line /** ... */ closes on its own line.
        end = i
        while end < total and not lines[end].rstrip().endswith("*/"):
            end += 1
        if end >= total:
            break
        block = "\n".join(lines[i : end + 1])
        tags = len(RETURN_TAG.findall(block))
        if tags:
            symbol = attached_void_symbol(lines, end + 1)
            if symbol is not None:
                found[symbol] = found.get(symbol, 0) + tags
        i = end + 1
    return found


def attached_void_symbol(lines: list[str], start: int) -> str | None:
    """The ``void`` function a doc block attaches to, or None.

    Blank lines and ordinary ``//`` comments between the block and the
    declaration do not break the attachment -- Doxygen still binds them -- but
    a preprocessor directive does: a documented ``#define`` is a macro, not a
    function, and has no return type to contradict.
    """
    idx = start
    while idx < len(lines):
        stripped = lines[idx].strip()
        if not stripped or stripped.startswith("//"):
            idx += 1
            continue
        break
    if idx >= len(lines):
        return None
    if lines[idx].lstrip().startswith("#"):
        return None
    joined = " ".join(part.strip() for part in lines[idx : idx + DECL_LOOKAHEAD])
    joined = re.sub(r"\s+", " ", joined).strip()
    match = VOID_DECL.match(strip_leading_tokens(joined))
    return match.group(1) if match else None


def scan_paths(rels: list[str]) -> dict[tuple[str, str], int]:
    """Offending ``(path, symbol)`` pairs across the given repo-relative paths."""
    rows: dict[tuple[str, str], int] = {}
    for rel in rels:
        path = REPO_ROOT / rel
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for symbol, count in scan_text(text).items():
            rows[(rel, symbol)] = count
    return rows


def tree_rows() -> dict[tuple[str, str], int]:
    """Offenders across every tracked first-party C/C++ file."""
    return scan_paths(first_party_paths(SOURCE_SUFFIXES))


LEDGER_HEADER = """\
# Frozen ``@return``-on-``void`` debt (docs/STYLE_GUIDE.md, #900).
#
# Emitted by `python3 scripts/checks/check_void_return_tags.py --update` from
# the tracked first-party C/C++ tree.  Never hand-edit: every field is
# re-derived from source text, so an edit is either a no-op or a lie the gate
# finds on the next run.
#
# <file>\t<symbol>\t<tag-count>
#
# A void function documents what it leaves behind in @post, not in a @return
# line, and Doxygen warns about the tag for the headers it reads.  The rows
# below predate the rule being enforced.  A pair that is missing or whose count
# grew fails as a regression; one that fires less often fails as stale, so
# paying debt down is recorded here rather than silently absorbed.  Line
# numbers are not part of the key, so edits above a block do not churn it.
"""


def format_ledger(rows: dict[tuple[str, str], int]) -> str:
    """Render rows as the ledger file's text, sorted for a readable diff."""
    out = [LEDGER_HEADER]
    for (rel, symbol), count in sorted(rows.items()):
        out.append(f"{rel}\t{symbol}\t{count}\n")
    return "".join(out)


def parse_ledger(text: str) -> tuple[dict[tuple[str, str], int], list[str]]:
    """Parse ledger text into rows plus a list of malformed-row complaints."""
    rows: dict[tuple[str, str], int] = {}
    problems: list[str] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3:
            problems.append(f"line {lineno}: expected 3 tab-separated fields, got {len(fields)}")
            continue
        rel, symbol, count_text = fields
        if not rel or not symbol:
            problems.append(f"line {lineno}: empty file or symbol field")
            continue
        try:
            count = int(count_text)
        except ValueError:
            problems.append(f"line {lineno}: count {count_text!r} is not an integer")
            continue
        if count < 1:
            problems.append(f"line {lineno}: count {count} is not positive")
            continue
        if (rel, symbol) in rows:
            problems.append(f"line {lineno}: duplicate row for {rel} {symbol}")
            continue
        rows[(rel, symbol)] = count
    return rows, problems


def compare(
    actual: dict[tuple[str, str], int], ledger: dict[tuple[str, str], int]
) -> tuple[list[str], list[str]]:
    """Split the difference between tree and ledger into regressions and stale rows."""
    regressions: list[str] = []
    stale: list[str] = []
    for key in sorted(set(actual) | set(ledger)):
        rel, symbol = key
        now = actual.get(key, 0)
        was = ledger.get(key, 0)
        if now > was:
            where = "not on the ledger" if was == 0 else f"ledgered at {was}"
            regressions.append(f"{rel}: {symbol} has {now} return tag(s) on a void function ({where})")
        elif now < was:
            gone = "no longer fires" if now == 0 else f"now fires {now} time(s), ledgered at {was}"
            stale.append(f"{rel}: {symbol} {gone}")
    return regressions, stale


def run_check(explicit: list[str]) -> int:
    """Compare the tree (or an explicit file list) against the ledger."""
    if not BASELINE.exists():
        print(f"FATAL -- ledger {BASELINE.relative_to(REPO_ROOT)} is missing", file=sys.stderr)
        return 1
    ledger, problems = parse_ledger(BASELINE.read_text(encoding="utf-8"))
    if problems:
        print(f"MALFORMED LEDGER -- {BASELINE.relative_to(REPO_ROOT)}", file=sys.stderr)
        for item in problems:
            print(f"  {item}", file=sys.stderr)
        return 1

    if explicit:
        rels = []
        for item in explicit:
            rel = str(pathlib.Path(item).resolve().relative_to(REPO_ROOT))
            if rel.endswith(SOURCE_SUFFIXES):
                rels.append(rel)
        actual = scan_paths(rels)
        scope = set(rels)
        ledger = {key: value for key, value in ledger.items() if key[0] in scope}
    else:
        actual = tree_rows()

    regressions, stale = compare(actual, ledger)
    if regressions:
        print("REGRESSION -- a void function must document @post, not @return:", file=sys.stderr)
        for item in regressions:
            print(f"  {item}", file=sys.stderr)
    if stale:
        print("STALE -- the ledger only shrinks; re-emit it with --update:", file=sys.stderr)
        for item in stale:
            print(f"  {item}", file=sys.stderr)
    if regressions or stale:
        print(
            "\nFix the tag (drop @return, move what it said into @post), then run\n"
            "  python3 scripts/checks/check_void_return_tags.py --update",
            file=sys.stderr,
        )
        return 1
    total = sum(actual.values())
    print(f"OK -- {len(actual)} ledgered symbol(s), {total} frozen @return tag(s) on void functions")
    return 0


def run_update() -> int:
    """Re-emit the ledger from the tree."""
    rows = tree_rows()
    BASELINE.write_text(format_ledger(rows), encoding="utf-8")
    print(
        f"wrote {BASELINE.relative_to(REPO_ROOT)}: "
        f"{len(rows)} symbol(s), {sum(rows.values())} tag(s)"
    )
    return 0


def run_emit_rows() -> int:
    """Print what the tree says today, ledger ignored."""
    rows = tree_rows()
    for (rel, symbol), count in sorted(rows.items()):
        print(f"{rel}\t{symbol}\t{count}")
    return 0


def _selftest() -> int:
    """Synthetic fixtures, both directions."""
    failures: list[str] = []

    def scan(text: str) -> dict[str, int]:
        return scan_text(text)

    expect(
        scan("/**\n * @brief Do it.\n * @return Nothing.\n */\nvoid ra8_do(void);\n") == {"ra8_do": 1},
        "a @return on a void prototype fires",
        failures,
    )
    expect(
        scan("/**\n * @brief Do it.\n * @post Done.\n */\nvoid ra8_do(void);\n") == {},
        "a void prototype with no return tag is clean",
        failures,
    )
    expect(
        scan("/**\n * @return 0 on success.\n */\nint ra8_do(void);\n") == {},
        "a @return on an int function is clean",
        failures,
    )
    expect(
        scan("/**\n * @return The buffer.\n */\nvoid *ra8_buf(void);\n") == {},
        "void * returns something, so it is clean",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\ntypedef void (*ra8_cb_t)(void);\n") == {},
        "a typedef is not a function declaration",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\n#define RA8_DO() do { } while (0)\n") == {},
        "a documented macro has no return type to contradict",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\nRA8_INTERNAL static void internal_do(int a)\n{\n") == {"internal_do": 1},
        "leading RA8_ macro and storage class are stripped",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\nvoid\nra8_do(int a)\n{\n") == {"ra8_do": 1},
        "a return type on its own line still attaches",
        failures,
    )
    expect(
        scan("/**\n * @retval none Nothing.\n */\nvoid ra8_do(void);\n") == {"ra8_do": 1},
        "@retval on a void function fires too",
        failures,
    )
    expect(
        scan("/**\n * \\return Nothing.\n */\nvoid ra8_do(void);\n") == {"ra8_do": 1},
        "the backslash tag style fires",
        failures,
    )
    expect(
        scan("/*\n * @return Nothing.\n */\nvoid ra8_do(void);\n") == {},
        "an ordinary /* comment is not a Doxygen block",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n * @retval none Nothing.\n */\nvoid ra8_do(void);\n") == {"ra8_do": 2},
        "two tags on one block count twice",
        failures,
    )
    expect(
        scan(
            "/**\n * @return Nothing.\n */\nvoid ra8_do(void);\n\n"
            "/**\n * @return Nothing.\n */\nvoid ra8_do(void)\n{\n}\n"
        )
        == {"ra8_do": 2},
        "a prototype and its definition aggregate under one key",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\n\n// a note\nvoid ra8_do(void);\n") == {"ra8_do": 1},
        "a blank line and a // note do not break attachment",
        failures,
    )
    expect(
        scan("/** @return Nothing. */\nvoid ra8_do(void);\n") == {"ra8_do": 1},
        "a single-line doc block fires",
        failures,
    )
    expect(
        scan("/**\n * @brief Text mentioning @return in prose.\n */\nstruct ra8_s { int x; };\n") == {},
        "a tag above a struct attaches to no function",
        failures,
    )
    expect(
        scan("/**\n * @return Nothing.\n */\nstatic inline void ra8_do(void) { }\n") == {"ra8_do": 1},
        "static inline void fires",
        failures,
    )

    # Ledger parsing and comparison, both directions.
    rows, problems = parse_ledger("# comment\n\nlibs/a.c\tsym\t2\n")
    expect(rows == {("libs/a.c", "sym"): 2} and not problems, "a well-formed ledger parses", failures)
    _, problems = parse_ledger("libs/a.c\tsym\n")
    expect(bool(problems), "a two-field row is malformed", failures)
    _, problems = parse_ledger("libs/a.c\tsym\tmany\n")
    expect(bool(problems), "a non-integer count is malformed", failures)
    _, problems = parse_ledger("libs/a.c\tsym\t0\n")
    expect(bool(problems), "a zero count is malformed", failures)
    _, problems = parse_ledger("libs/a.c\tsym\t1\nlibs/a.c\tsym\t2\n")
    expect(bool(problems), "a duplicate row is malformed", failures)

    reg, stale = compare({("libs/a.c", "sym"): 1}, {})
    expect(len(reg) == 1 and not stale, "an unledgered offender is a regression", failures)
    reg, stale = compare({("libs/a.c", "sym"): 2}, {("libs/a.c", "sym"): 1})
    expect(len(reg) == 1 and not stale, "a grown count is a regression", failures)
    reg, stale = compare({}, {("libs/a.c", "sym"): 1})
    expect(not reg and len(stale) == 1, "a paid-down row is stale", failures)
    reg, stale = compare({("libs/a.c", "sym"): 1}, {("libs/a.c", "sym"): 1})
    expect(not reg and not stale, "an unchanged tree is clean", failures)

    expect(
        format_ledger({("libs/b.c", "s"): 1, ("libs/a.c", "s"): 1}).splitlines()[-2:]
        == ["libs/a.c\ts\t1", "libs/b.c\ts\t1"],
        "the ledger is emitted in sorted order",
        failures,
    )
    return report(failures)


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch one mode."""
    parser = argparse.ArgumentParser(
        description="Fail when a void function documents a @return tag.",
    )
    parser.add_argument("--check", action="store_true", help="compare the tree against the ledger")
    parser.add_argument("--update", action="store_true", help="re-emit the ledger from the tree")
    parser.add_argument("--selftest", action="store_true", help="run synthetic fixtures")
    parser.add_argument("--emit-rows", action="store_true", help="print today's offenders")
    parser.add_argument("files", nargs="*", help="explicit files to scan (pre-commit hook)")
    args = parser.parse_args(argv)

    modes = [args.check, args.update, args.selftest, args.emit_rows]
    if sum(1 for mode in modes if mode) > 1:
        parser.error("choose one of --check / --update / --selftest / --emit-rows")
    if args.selftest:
        return _selftest()
    if args.update:
        return run_update()
    if args.emit_rows:
        return run_emit_rows()
    return run_check(args.files)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

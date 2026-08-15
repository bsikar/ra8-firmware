# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The file-header and ``@param``-direction rules from ``docs/STYLE_GUIDE.md``.

Both rules were stated as facts by the style guide and implemented by nothing
(#532).  The guide said the file-header block's tag order mattered "because the
cite_check / world_tag scripts grep on it" -- neither does: ``cite_check.py``
greps ``HUM Ch`` and ``check_world_tags.py`` greps the ``[Ring N / X]`` /
``{World: X}`` pair, and neither has ever read ``@file``, ``@brief`` or
``@details``.  The guide also said a plain ``@param`` without a direction "is
rejected", while ``doxy_functions``'s regex made the bracket optional with an
in-source comment reading "any direction".

These two live here rather than in ``doxy_functions`` / ``doxy_members``
because they are properties of the COMMENT TEXT rather than of a symbol.  A
file header attaches to no declaration at all, and NOT ONE of the 55 bare
``@param`` tags the rule found in the tree sat on a function declaration: 53
documented function-like MACROS and 2 documented a callback typedef, none of
which the function gate looks at.  Scope is therefore the derived first-party C
set from ``lint_targets.first_party_paths`` -- ``git ls-files`` minus the named
SOUP / generated exemptions -- not ``doxy_scope.SCAN_DIRS``, which stops at
``libs``/``src``/``port``.

WHAT IS HARD AND WHAT IS RATCHETED
----------------------------------
``@file`` (present, and naming this file), ``@brief`` and the ``@param``
direction are HARD: the tree already held all three at zero offenders once the
one stale ``@file`` in ``port/mbedtls`` and the 55 bare ``@param`` tags were
fixed, so there is no debt to grandfather and a new violation fails on sight.

``@details`` is RATCHETED against ``.github/doxy-details-baseline.txt``, the
same shape ``tidy_ratchet`` and ``misra_ratchet`` use.  130 first-party files
had no ``@details`` paragraph when the rule was first enforced, and closing
that means writing 130 real paragraphs -- a documentation campaign, not a
mechanical edit.  Turning the whole tree red for it would produce exactly the
tag-shaped filler ``doxy_audit``'s own docstring warns against, and a gate that
cries wolf gets switched off.  So the debt is frozen file-by-file and may only
shrink: a file outside the baseline fails immediately, and ``--update-baseline``
refuses to ADD an entry.

BOTH ACCEPTANCE PROPERTIES FROM #190
------------------------------------
*No constant is compared to itself.*  Every verdict here is re-derived from the
file text on each run; the baseline is an allow-list of paths, never a
transcribed copy of the measurement.

*Every scan has a vacuity floor.*  A checker that silently scans nothing
reports a clean tree, and this repository has now found that exact failure
twice.  Both the file count and the number of ``@param`` tags actually reached
are floored below, so a collapsed enumeration or a broken comment lexer fails
loudly instead of passing.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from doxy_lex import blank_noncode
from doxy_scope import GENERATED_PROTOCOL_FILES
from lint_targets import first_party_paths

#: The real checkout root; every reported path is relative to it.
REPO_ROOT = Path(__file__).resolve().parents[2]

#: Frozen ``@details`` debt: one repo-relative path per line, ``#`` comments.
DETAILS_BASELINE = REPO_ROOT / ".github" / "doxy-details-baseline.txt"

#: Suffixes this gate audits. Every first-party C/C++ translation unit and
#: header; `lint_targets` decides which of those are ours.
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

#: Vacuity floor on the file enumeration. Measured 2115 first-party C files at
#: the time of writing; this is not a policy on tree size, it is a refusal to
#: report a clean tree after enumerating almost none of it.
FILE_SCAN_FLOOR = 1500

#: Vacuity floor on the ``@param`` tags the comment lexer actually reached.
#: Measured 16291. Floors the OTHER way this gate can go quiet: the file list
#: can be perfect while `blank_noncode` stops yielding doc comments, in which
#: case every ``@param`` rule silently checks nothing.
PARAM_SCAN_FLOOR = 5000

#: The three directions docs/STYLE_GUIDE.md names, and the only three the tree
#: uses (measured: in 12392, out 2549, in,out 1292). Whitespace inside the
#: bracket is stripped before the comparison, so ``[in, out]`` is accepted.
VALID_DIRECTIONS = frozenset({"in", "out", "in,out"})

#: Doxygen accepts ``@cmd`` and ``\cmd`` interchangeably, and this tree uses
#: both -- ``port/mbedtls`` carries upstream's backslash form. A rule keyed on
#: ``@`` alone would silently exempt every backslash-form header.
_TAG_RE = {name: re.compile(r"[@\\]" + name + r"\b") for name in ("file", "brief", "details")}

#: ``\file`` takes its optional argument on its OWN line; when the name is
#: pushed to the next line doxygen reads the command as argument-less, meaning
#: "the file this block is in". 17 headers in this tree are written that way,
#: so the argument is matched same-line-only and an empty one is correct.
_FILE_ARG_RE = re.compile(r"[@\\]file[ \t]*(?P<rest>[^\r\n]*)")

#: One ``@param``, with its direction bracket if it has one.
_PARAM_RE = re.compile(r"[@\\]param(?P<bracket>[ \t]*\[[^\]]*\])?")

#: Offender lines printed before the report truncates, so a hook stays readable.
OFFENDER_CAP = 50

#: Rule code for the ratcheted rule. Named because two functions branch on it.
DETAILS_MISSING = "DETAILS_MISSING"

#: One finding: repo-relative path, 1-based line, rule code, human detail.
Row = tuple[str, int, str, str]


def _doc_comments(raw: str) -> list[tuple[int, str]]:
    """Return ``(offset, text)`` for every Doxygen comment in ``raw``.

    Uses the shared blanking lexer rather than a regex so that a ``/**`` inside
    a string literal is not mistaken for a doc block. Plain ``/* ... */``
    comments are excluded: a ``@param`` in one is prose, not documentation.
    """
    _code, comments = blank_noncode(raw)
    return [(start, raw[start:end]) for start, end, style in comments if style is not None]


def _line_of(raw: str, offset: int) -> int:
    """1-based line number of ``offset`` in ``raw``."""
    return raw.count("\n", 0, offset) + 1


def _file_block(docs: list[tuple[int, str]]) -> tuple[int, str] | None:
    r"""The first Doxygen comment carrying a ``@file`` / ``\file`` command."""
    for offset, text in docs:
        if _TAG_RE["file"].search(text):
            return offset, text
    return None


def _file_arg(block: str) -> str:
    """The argument given to ``@file``, or ``""`` when it has none."""
    match = _FILE_ARG_RE.search(block)
    if match is None:
        return ""
    tokens = match.group("rest").split()
    return tokens[0] if tokens else ""


def _file_arg_resolves(rel: str, arg: str) -> bool:
    """Whether ``@file <arg>`` names the file it sits in.

    Accepts the bare basename (what the style guide asks for), the full
    repo-relative path (which hundreds of files in this tree actually use),
    any trailing path segment of it, and an absent argument.

    This is deliberately a RESOLUTION rule, not the style guide's original
    "filename only, not the path". Both spellings resolve in doxygen, both are
    in wide use here, and the defect worth catching is the third case: a
    ``@file`` left naming the old location after a ``git mv``, which
    ``docs/DOCS.md`` already lists as a common doxygen warning.
    """
    value = arg.replace("\\", "/").strip()
    if not value:
        return True
    return rel == value or rel.endswith("/" + value) or Path(rel).name == value


def _audit_file_block(rel: str, raw: str, docs: list[tuple[int, str]]) -> list[Row]:
    """Check the file-header block: present, self-naming, ``@brief``, ``@details``."""
    found = _file_block(docs)
    if found is None:
        return [(rel, 1, "FILE_BLOCK_MISSING", "no file-header Doxygen block carrying @file")]
    offset, block = found
    line = _line_of(raw, offset)
    rows: list[Row] = []
    arg = _file_arg(block)
    if not _file_arg_resolves(rel, arg):
        detail = f"@file names '{arg}', which is not this file"
        rows.append((rel, line, "FILE_TAG_MISMATCH", detail))
    if not _TAG_RE["brief"].search(block):
        rows.append((rel, line, "BRIEF_MISSING", "the file-header block has no @brief"))
    if not _TAG_RE["details"].search(block):
        rows.append((rel, line, DETAILS_MISSING, "the file-header block has no @details"))
    return rows


def _audit_params(rel: str, raw: str, docs: list[tuple[int, str]]) -> tuple[list[Row], int]:
    """Check every ``@param`` carries one of the three legal directions.

    Returns the findings and the number of ``@param`` tags reached, which the
    caller floors: a lexer that stopped yielding doc comments would otherwise
    report a clean tree over zero tags.
    """
    rows: list[Row] = []
    seen = 0
    for offset, block in docs:
        for match in _PARAM_RE.finditer(block):
            seen += 1
            line = _line_of(raw, offset + match.start())
            bracket = match.group("bracket")
            if bracket is None:
                rows.append(
                    (rel, line, "PARAM_NO_DIRECTION", "plain @param: no [in]/[out]/[in,out]")
                )
                continue
            direction = re.sub(r"\s+", "", bracket).strip("[]")
            if direction not in VALID_DIRECTIONS:
                rows.append(
                    (rel, line, "PARAM_BAD_DIRECTION", f"@param[{direction}] is not a direction")
                )
    return rows, seen


def audit_text(rel: str, raw: str) -> tuple[list[Row], int]:
    """Audit one file's text. Returns its findings and its ``@param`` count."""
    docs = _doc_comments(raw)
    rows = _audit_file_block(rel, raw, docs)
    param_rows, seen = _audit_params(rel, raw, docs)
    return [*rows, *param_rows], seen


def read_baseline() -> set[str]:
    """The frozen ``@details`` debt, or an empty set when the file is absent."""
    if not DETAILS_BASELINE.is_file():
        return set()
    lines = DETAILS_BASELINE.read_text(encoding="ascii").splitlines()
    return {ln.strip() for ln in lines if ln.strip() and not ln.lstrip().startswith("#")}


BASELINE_HEADER = """\
# doxy_audit --style: the frozen @details debt (#532).
#
# One repo-relative path per line: a first-party C file whose file-header
# Doxygen block carries no @details paragraph. docs/STYLE_GUIDE.md requires
# one; these predate the rule being enforced and are excused until written.
#
# This list may only SHRINK. A file that is not in it and has no @details
# fails the gate on sight, and `--update-baseline` refuses to add an entry.
# Closing the debt means this file reaching zero paths and being deleted --
# by WRITING the paragraphs, never by generating tag-shaped filler.
"""


def scan() -> tuple[list[Row], list[str], int]:
    """Audit the whole first-party C set.

    Returns the findings, the paths actually READ, and the ``@param`` count.
    The second element is deliberately not the enumeration: ``git ls-files``
    still lists a path deleted from the working tree without ``git rm``, and a
    file that is not there is not a documentation violation. Skipping it and
    flooring on what was read means a mass disappearance still fails, while one
    half-finished deletion does not crash the pre-commit hook.
    """
    read: list[str] = []
    rows: list[Row] = []
    params = 0
    for rel in first_party_paths(SOURCE_SUFFIXES):
        if rel in GENERATED_PROTOCOL_FILES:
            continue
        path = REPO_ROOT / rel
        if not path.is_file():
            continue
        read.append(rel)
        file_rows, seen = audit_text(rel, path.read_text(encoding="utf-8", errors="replace"))
        rows.extend(file_rows)
        params += seen
    return rows, read, params


def _floor_failure(paths: list[str], params: int) -> str | None:
    """The vacuity complaint for this scan, or None when both floors clear."""
    if len(paths) < FILE_SCAN_FLOOR:
        return (
            f"READ only {len(paths)} first-party C file(s), floor is "
            f"{FILE_SCAN_FLOOR}. A collapsed scope reports a clean tree because "
            "it looked at almost nothing."
        )
    if params < PARAM_SCAN_FLOOR:
        return (
            f"reached only {params} @param tag(s), floor is {PARAM_SCAN_FLOOR}. "
            "The doc-comment lexer has stopped yielding blocks, so the direction "
            "rule is checking nothing."
        )
    return None


def partition(rows: list[Row], baseline: set[str]) -> tuple[list[Row], list[str]]:
    """Split findings into real violations and stale baseline entries.

    A ``DETAILS_MISSING`` row for a baselined path is excused debt. A baselined
    path with no such row is stale -- the paragraph was written, or the file is
    gone -- and is a FAILURE rather than a notice, because a ratchet that
    tolerates a stale entry lets the debt quietly grow back into it.
    """
    still_missing = {row[0] for row in rows if row[2] == DETAILS_MISSING}
    violations = [row for row in rows if not (row[2] == DETAILS_MISSING and row[0] in baseline)]
    return violations, sorted(baseline - still_missing)


def _print_rows(rows: list[Row]) -> None:
    """Print offender rows, truncated to ``OFFENDER_CAP``."""
    for rel, line, code, detail in rows[:OFFENDER_CAP]:
        print(f"  {rel}:{line}  {code}  --  {detail}")
    if len(rows) > OFFENDER_CAP:
        print(f"  ... and {len(rows) - OFFENDER_CAP} more")


def run_check() -> int:
    """The gate. Returns 0 when clean, 1 on findings, 2 when it could not run."""
    rows, paths, params = scan()
    complaint = _floor_failure(paths, params)
    if complaint is not None:
        sys.stderr.write(f"doxy_audit --style: FATAL -- {complaint}\n")
        return 2
    rows.sort(key=lambda row: (row[0], row[1]))
    violations, stale = partition(rows, read_baseline())
    if not violations and not stale:
        print(
            f"doxy_audit --style: violations=0 (PASS) over {len(paths)} files, "
            f"{params} @param tags, {len(read_baseline())} baselined @details"
        )
        return 0
    print(f"doxy_audit --style: violations={len(violations)} stale-baseline={len(stale)} (FAIL)")
    if violations:
        print("Offenders (file:line  rule  -- detail):")
        _print_rows(violations)
        print()
        print("docs/STYLE_GUIDE.md 'File-header Doxygen block' and 'Function")
        print("documentation' state these rules; this gate is what enforces them.")
    if stale:
        print("Stale @details baseline entries (the paragraph is written, or the file is gone):")
        for rel in stale[:OFFENDER_CAP]:
            print(f"  {rel}")
        print()
        print("Lock the progress in with:")
        print("  python3 scripts/checks/doxy_audit.py --style --update-baseline")
    return 1


def run_update_baseline() -> int:
    """Shrink the ``@details`` baseline to what is still missing. Never grows it."""
    rows, paths, params = scan()
    complaint = _floor_failure(paths, params)
    if complaint is not None:
        sys.stderr.write(f"doxy_audit --style: FATAL -- {complaint}\n")
        return 2
    missing = {row[0] for row in rows if row[2] == DETAILS_MISSING}
    baseline = read_baseline()
    additions = sorted(missing - baseline)
    if additions:
        sys.stderr.write(
            "doxy_audit --style --update-baseline: refusing to GROW the baseline. "
            f"{len(additions)} file(s) have no @details and are not frozen debt:\n"
        )
        for rel in additions[:OFFENDER_CAP]:
            sys.stderr.write(f"  {rel}\n")
        sys.stderr.write("Write the @details paragraph instead.\n")
        return 1
    kept = sorted(missing & baseline)
    DETAILS_BASELINE.write_text(BASELINE_HEADER + "\n".join(kept) + "\n", encoding="ascii")
    print(f"doxy_audit --style: baseline rewritten, {len(baseline)} -> {len(kept)} entries")
    return 0

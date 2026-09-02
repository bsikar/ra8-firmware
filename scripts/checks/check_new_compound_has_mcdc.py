#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject a NEW compound decision that lands without an MC/DC test.

A new compound boolean decision (``&&`` / ``||``) in production code must
arrive with an accompanying MC/DC test vector set. Per CLAUDE.md
"IEC 61508 SIL 3 / DO-178C Level B Qualification" and docs/MCDC.md, every
compound boolean decision in production code under ``libs/``,
``apps/shared_libs/``, ``port/``, and firmware applications must have a
matching MC/DC test vector set in an indexed test translation unit. The test
declares its vector pattern in a Doxygen ``@par MC/DC:`` block that cites the
decision as ``path@function`` -- the source path and the *enclosing function*
of the decision. Citing by function (not line number) means unrelated edits
that shift lines never invalidate a citation.

This is a *static* check: it never builds or runs the test suite. It compares
structural fingerprints of logical ``&&`` / ``||`` expressions in each
changed production component. Formatting, file splits, and stable-symbol
function moves therefore do not turn existing decisions into "new" ones;
renamed decision owners need a citation at their new anchor. Adding an operator
or changing predicate structure also creates a new fingerprint. For each new
fingerprint, it searches supported indexed test sources under ``tests/`` and
``apps/`` for a ``@par MC/DC:`` block citing ``path@that_function``.

Identifiers are alpha-normalized so a systematic local rename is cosmetic.
Consequently this is not a predicate-equivalence proof: a replacement with
the same operator/comparison topology can compare equal. The whole-tree debt
ratchet and executed MC/DC gate remain responsible for detecting coverage loss
after such substitutions. That boundary is explicit and self-tested.

Two selection modes, and NO third silent one:

  * ``--range BASE..HEAD [--repo DIR]`` -- audit the files changed in that
    commit range, run against DIR (default ``.``). This is the mode CI uses;
    ``scripts/ci.sh``'s ``ci_commit_range`` / ``ci_history_repo`` resolve the
    range and the history repository the same way every other range-aware
    gate does. A range that does not resolve in the repository is FATAL, not
    a clean scan of nothing.

  * ``--staged`` -- audit the git index against HEAD. This is the mode the
    local ``scripts/git/pre-commit`` hook uses: it gates exactly what is
    about to be committed.

Invoked with NEITHER mode, the check FAILS LOUDLY (exit 2) rather than
reporting a clean scan of zero files. That is the #355 defect this rewrite
closes: the check used ``git diff --cached`` unconditionally, so in any CI
checkout -- where nothing is staged -- it saw 0 files and exited 0, having
audited nothing in any CI run, ever. A scan that examined zero files can
never exit 0 silently: the audited file count is always reported, and a
scope that could not be established is a non-PASS.

Besides the two CLI modes there is a git-free WHOLE-TREE scan, ``audit_tree()``,
which walks the checked-out production sources and reports every uncovered
compound decision with its enclosing function. It is the measurement
``scripts/checks/mcdc_compound_ratchet.py`` ratchets against the committed
``.github/mcdc-compound-baseline.txt``, and it is what makes CI enforcement
possible while a large backlog is still outstanding: the delta modes above fail
the moment an existing uncovered decision line is merely *reformatted*, which
with a backlog this size is a cliff rather than a ratchet. ``audit_tree()``
counts, so the debt is frozen and can only shrink. The detection primitives are
shared, so there is exactly one definition of "this decision lacks MC/DC
vectors".

The check intentionally does NOT cover:
  * Either canonical ``third_party`` root -- SOUP exempted per docs/MCDC.md.
  * ``tests/`` -- only production code.
  * ``examples/`` and host tools -- outside this structural citation ratchet;
    the executed per-file MC/DC floor covers represented files from both.
  * Single-condition ``if (x)`` -- MC/DC only applies to compound decisions.

Exit codes:
  0  the audited (non-empty or legitimately empty) scope adds no uncovered
     compound decision.
  1  one or more NEW decisions lack a matching MC/DC test.
  2  no usable scan scope (no mode given, or an unresolvable range) -- the
     scope could not be established, so no verdict is trustworthy.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import firmware_app_dirs, is_build_output_path
from mcdc_compound_delta import (
    COMPOUND_OP_RE,
    NO_ENCLOSING_FUNCTION,
    enclosing_function,
    lexical_code_view,
    new_decision_occurrences,
)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Production directories that are subject to the MC/DC gate.
#
# "Production" here means code that runs on the target: the platform libraries
# (the Ring 5 secure substrate among them, as libs/ra8_secure_app), the RTOS
# ports, reusable application-domain production modules, and the FIRMWARE
# products. Shared modules are explicit because they compile into product
# images without owning a linker script. The remaining apps/ product tier
# mixes host programs (the mdl CLI) and firmware, so that half appears through
# a derivation. Deriving the firmware products from
# ``lint_targets.firmware_app_dirs()`` keeps the scope's MEANING fixed while
# the tree moves under it: when the e-reader composition moved into the
# products tier, a literal tuple would have dropped six firmware translation
# units out of this gate and reported the resulting smaller count as a
# burn-down.
PROD_PREFIXES: tuple[str, ...] = (
    "libs/",
    "port/",
    "apps/shared_libs/",
    *(f"{d}/" for d in firmware_app_dirs()),
)

# Test translation units allowed to carry executable MC/DC vector sets.
TEST_SOURCE_SUFFIXES: tuple[str, ...] = (".c", ".cpp")

# Display limits.
MAX_DISPLAYED_FINDINGS = 50  # Max findings to print before summarizing the rest.
SNIPPET_MAX_LEN = 80  # Max characters of a decision snippet before truncation.
SNIPPET_TRUNCATE_LEN = 77  # Length of truncated snippet body (leaves room for "...").

# Number of tab-separated fields in a `git diff --name-status -M` rename row.
RENAME_ROW_FIELD_COUNT = 3  # <status>\t<old>\t<new>
CHANGE_ROW_FIELD_COUNT = 2  # <status>\t<path>

# The canonical empty-tree object. Used as the "base" when a range names a
# root/new-branch head with no parent, so every decision in every changed file
# is treated as new. `git` always resolves it, in every repository.
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

# Excluded subtrees (SOUP, tests, generated, etc.).
EXCLUDED_SUBSTRINGS: tuple[str, ...] = ("/third_party/", "/tests/", "/test/")

# Regex matching one citation token inside a `@par MC/DC:` block. The only
# accepted form is `path@function_name`: it pins the decision to its enclosing
# function, so unrelated edits that shift lines never invalidate it, and --
# having no `:line` -- it is not flagged by check_line_citations.py.
# The path alternation is built from PROD_PREFIXES rather than spelled out: a
# citation naming a file this gate scans must parse, and the two drifting apart
# is silent -- the citation simply stops matching and its decision reads as
# uncovered.
SYMBOL_CITATION_RE = re.compile(
    r"(?P<path>(?:"
    + "|".join(re.escape(prefix.rstrip("/")) for prefix in PROD_PREFIXES)
    + r")/[A-Za-z0-9_./-]+\.c)@(?P<sym>[A-Za-z_]\w*)"
)

# Regex isolating each `@par MC/DC:` block in a test file. The block starts at
# `@par MC/DC:` and runs until the next `@par`, the next `*/`, or the next
# blank Doxygen line (` *` followed by EOL).
MCDC_BLOCK_RE = re.compile(
    r"@par\s+MC/DC\s*:.*?(?=(?:\*/|@par\s+\w|\n\s*\*\s*\n))",
    re.IGNORECASE | re.DOTALL,
)


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------


def _git(*args: str) -> str:
    """Run ``git <args...>`` and return stdout, raising on a non-zero exit."""
    return subprocess.run(  # noqa: S603  # trusted: fixed git argv
        ["git", *args],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def _git_ok(*args: str) -> bool:
    """Whether ``git <args...>`` exits 0 (used for object-existence probes)."""
    return (
        subprocess.run(  # noqa: S603  # trusted: fixed git argv
            ["git", *args],  # noqa: S607  # trusted: fixed git argv
            check=False,
            capture_output=True,
            text=True,
        ).returncode
        == 0
    )


def _blob_at(repo: str, rev: str, path: str) -> str:
    """Content of ``path`` at ``rev`` in ``repo``, or "" when it is absent.

    The empty string is the meaningful case for the base revision: it makes
    every decision in a file that did not exist there count as new.
    """
    try:
        return _git("-C", repo, "show", f"{rev}:{path}")
    except subprocess.CalledProcessError:
        return ""


def _path_included(path: str, *, prefixes: tuple[str, ...]) -> bool:
    """Whether ``path`` is a production ``.c`` file the gate should audit."""
    if not path.endswith(".c"):
        return False
    if not any(path.startswith(pre) for pre in prefixes):
        return False
    return not (is_build_output_path(path) or any(sub in path for sub in EXCLUDED_SUBSTRINGS))


def _is_test_source_name(name: str) -> bool:
    """Whether ``name`` is a supported MC/DC test translation unit.

    Both orderings of the convention count. ``test_<module>.c`` is the common
    one, but the tree also carries ``<module>_test.c`` and companion units
    under ``tests/support/``, and a citation written in one of those used to be
    invisible: the glob was ``tests/test_*.{c,cpp}`` only, so every decision
    those suites cover read as UNCOVERED. That is the same scope collapse that
    once hid 81 decisions in the two ``.cpp`` EPUB suites -- a checker whose
    scope quietly stops matching reports FEWER findings, which reads as an
    improvement.
    """
    return (name.startswith("test_") or name.endswith(_TEST_NAME_SUFFIXES)) and name.endswith(
        TEST_SOURCE_SUFFIXES
    )


#: Trailing forms of the same convention, checked before the extension.
_TEST_NAME_SUFFIXES = tuple(f"_test{suffix}" for suffix in TEST_SOURCE_SUFFIXES)


def _working_test_sources(root_or_dir: Path) -> list[Path]:
    """Return every supported test translation unit under ``root_or_dir``."""
    sources: list[Path] = []
    if root_or_dir.name in ("tests", "test"):
        dirs_to_check = [root_or_dir]
    else:
        dirs_to_check = [
            d for dir_name in ("tests", "apps") if (d := root_or_dir / dir_name).is_dir()
        ]
        if not dirs_to_check and root_or_dir.is_dir():
            dirs_to_check = [root_or_dir]
    for d in dirs_to_check:
        for path in d.rglob("*"):
            if path.is_file() and _is_test_source_name(path.name):
                try:
                    rel = path.relative_to(root_or_dir).as_posix()
                    if not is_build_output_path(rel):
                        sources.append(path)
                except ValueError:
                    sources.append(path)
    return sorted(sources)


# ---------------------------------------------------------------------------
# Staged-mode selection (the local pre-commit hook)
# ---------------------------------------------------------------------------


def staged_files() -> list[str]:
    """Production ``.c`` paths staged for commit (added/copied/modified/renamed).

    Deletions are excluded: a removed file has no decision left to cover.
    """
    out = _git("diff", "--cached", "--name-only", "--diff-filter=ACMR")
    return [p for p in out.splitlines() if _path_included(p, prefixes=PROD_PREFIXES)]


def _parse_change_rows(out: str) -> list[tuple[str | None, str | None]]:
    """Parse name-status rows into ``(old_path, new_path)`` pairs."""
    pairs: list[tuple[str | None, str | None]] = []
    for row in out.splitlines():
        parts = row.split("\t")
        status = parts[0][:1] if parts else ""
        if status == "R" and len(parts) == RENAME_ROW_FIELD_COUNT:
            pairs.append((parts[1], parts[2]))
        elif status == "C" and len(parts) == RENAME_ROW_FIELD_COUNT:
            pairs.append((None, parts[2]))
        elif len(parts) == CHANGE_ROW_FIELD_COUNT and status == "A":
            pairs.append((None, parts[1]))
        elif len(parts) == CHANGE_ROW_FIELD_COUNT and status == "D":
            pairs.append((parts[1], None))
        elif len(parts) == CHANGE_ROW_FIELD_COUNT and status == "M":
            pairs.append((parts[1], parts[1]))
    return pairs


def _production_change_pairs(out: str) -> list[tuple[str | None, str | None]]:
    """Changed path pairs with at least one production endpoint."""
    pairs: list[tuple[str | None, str | None]] = []
    for old_path, new_path in _parse_change_rows(out):
        old_prod = old_path is not None and _path_included(old_path, prefixes=PROD_PREFIXES)
        new_prod = new_path is not None and _path_included(new_path, prefixes=PROD_PREFIXES)
        if old_prod or new_prod:
            pairs.append((old_path if old_prod else None, new_path if new_prod else None))
    return pairs


def staged_change_pairs() -> list[tuple[str | None, str | None]]:
    """All staged production changes, including deletions used as move ancestry."""
    out = _git("diff", "--cached", "--name-status", "-M40%", "--diff-filter=ACMRD")
    return _production_change_pairs(out)


def staged_blob(path: str) -> str:
    """Staged (index) content of ``path``, or "" when it is not staged.

    Reads the INDEX rather than the working tree, so unstaged edits sitting
    alongside a staged change cannot make the gate judge content not about to
    be committed.
    """
    try:
        return _git("show", f":0:{path}")
    except subprocess.CalledProcessError:
        return ""


def head_blob(path: str) -> str:
    """HEAD content of ``path``, or "" when the file is newly added."""
    try:
        return _git("show", f"HEAD:{path}")
    except subprocess.CalledProcessError:
        return ""


def staged_rename_map() -> dict[str, str]:
    """Map each staged rename's new path to its pre-rename old path.

    A ``git mv`` plus interior edits would otherwise make every decision in the
    moved file look brand new. A 40% similarity bar still pairs a rename that
    also renamed many interior symbols; mispairing only ever suppresses a "new"
    finding, so the generous threshold is safe.
    """
    out = _git("diff", "--cached", "--name-status", "-M40%", "--diff-filter=R")
    return _parse_rename_rows(out)


def collect_staged_citations() -> list[tuple[str, str]]:
    """Every citation in test sources present in the git index.

    Staged mode judges exactly the prospective commit. Untracked tests and
    unstaged citation edits must not change its verdict, while a staged test
    added with the decision must count immediately.
    """
    cites: list[tuple[str, str]] = []
    listing = _git("ls-files", "--cached", "--", "tests", "apps")
    for path in listing.splitlines():
        name = path.rsplit("/", 1)[-1]
        if _is_test_source_name(name) and not is_build_output_path(path):
            cites.extend(_extract_citations(staged_blob(path)))
    return cites


# ---------------------------------------------------------------------------
# Range-mode selection (CI)
# ---------------------------------------------------------------------------


def _parse_rename_rows(out: str) -> dict[str, str]:
    """Parse ``git diff --name-status`` rename rows into new -> old paths."""
    mapping: dict[str, str] = {}
    for row in out.splitlines():
        parts = row.split("\t")
        if len(parts) == RENAME_ROW_FIELD_COUNT and parts[0].startswith("R"):
            _status, old, new = parts
            mapping[new] = old
    return mapping


def resolve_range(repo: str, spec: str) -> tuple[str, str] | None:
    """Resolve a range spec to a ``(base, head)`` pair, or None when unusable.

    Accepts the shapes ``ci_commit_range`` emits: ``BASE..HEAD``, ``A...B``
    (symmetric, resolved via merge-base), and a bare ``HEAD`` (base becomes its
    first parent, or the empty tree at a root commit). Returns None -- the
    caller's cue to fail loudly -- when the spec is empty or names an endpoint
    the repository does not contain, which is the failure mode of pointing the
    gate at a snapshot whose object store lacks those commits.
    """
    spec = spec.strip()
    if not spec:
        return None
    if "..." in spec:
        left, _, right = spec.partition("...")
        head = right or "HEAD"
        left = left or "HEAD"
        try:
            base = _git("-C", repo, "merge-base", left, head).strip()
        except subprocess.CalledProcessError:
            return None
    elif ".." in spec:
        left, _, right = spec.partition("..")
        base = left
        head = right or "HEAD"
    else:
        head = spec
        base = (
            _git("-C", repo, "rev-parse", "--verify", "--quiet", f"{head}~1").strip()
            if _git_ok("-C", repo, "rev-parse", "--verify", "--quiet", f"{head}~1")
            else EMPTY_TREE
        )
    if not _git_ok("-C", repo, "rev-parse", "--verify", "--quiet", f"{head}^{{commit}}"):
        return None
    if not base:
        base = EMPTY_TREE
    if base != EMPTY_TREE and not _git_ok("-C", repo, "cat-file", "-e", f"{base}^{{commit}}"):
        return None
    return (base, head)


def changed_prod_files(repo: str, base: str, head: str) -> list[str]:
    """Production ``.c`` files changed between ``base`` and ``head`` in ``repo``."""
    out = _git("-C", repo, "diff", "--name-only", "--diff-filter=ACMR", base, head)
    return [p for p in out.splitlines() if _path_included(p, prefixes=PROD_PREFIXES)]


def range_change_pairs(repo: str, base: str, head: str) -> list[tuple[str | None, str | None]]:
    """All production changes in a range, including move-source deletions."""
    out = _git("-C", repo, "diff", "--name-status", "-M40%", "--diff-filter=ACMRD", base, head)
    return _production_change_pairs(out)


def range_rename_map(repo: str, base: str, head: str) -> dict[str, str]:
    """Map each rename between ``base`` and ``head`` to its pre-rename path."""
    out = _git("-C", repo, "diff", "--name-status", "-M40%", "--diff-filter=R", base, head)
    return _parse_rename_rows(out)


def collect_range_citations(repo: str, head: str) -> list[tuple[str, str]]:
    """Every citation in a supported indexed test source at ``head``.

    Reads the tests as committed at the audited revision (not the working
    tree), so the citation set matches the code under audit even when ``repo``
    is not the current checkout -- exactly the case under the CI snapshot,
    where the gate runs from a clean snapshot but resolves the range against
    the real history repository.
    """
    cites: list[tuple[str, str]] = []
    try:
        listing = _git("-C", repo, "ls-tree", "-r", "--name-only", head, "--", "tests", "apps")
    except subprocess.CalledProcessError:
        return cites
    for path in listing.splitlines():
        name = path.rsplit("/", 1)[-1]
        if _is_test_source_name(name):
            cites.extend(_extract_citations(_blob_at(repo, head, path)))
    return cites


# ---------------------------------------------------------------------------
# Decision detection
# ---------------------------------------------------------------------------


def compound_decision_lines(text: str) -> set[tuple[int, str]]:
    """Every line holding a compound operator outside comments and strings.

    Returns a set of ``(line_no, normalized_line)`` with 1-based line numbers.
    The normalized text -- whitespace-collapsed with ``NULL`` folded to
    ``nullptr`` -- is carried so the same decision compares equal across a
    cosmetic reformat or the C23 ``nullptr`` migration.

    Comment, literal, and preprocessor text is removed by
    ``lexical_code_view()`` -- the same whole-source view the delta modes
    read, so the ratchet measurement and the delta gate cannot disagree about
    what a decision is. The line-local scrub this replaced could not see that
    an operator sat on an interior line of a multi-line Doxygen block, nor
    that a `#define` continued onto the next line, so it counted prose and
    conditional-compilation logic as MC/DC debt (issue #790).
    """
    found: set[tuple[int, str]] = set()
    for idx, raw in enumerate(lexical_code_view(text).splitlines(), start=1):
        if COMPOUND_OP_RE.search(raw):
            normalized = re.sub(r"\s+", " ", raw.strip())
            normalized = re.sub(r"\bNULL\b", "nullptr", normalized)
            found.add((idx, normalized))
    return found


def new_decisions(new_text: str, base_text: str) -> list[tuple[int, str]]:
    """Compound decisions present in ``new_text`` but not in ``base_text``.

    A decision counts as "not new" when the SAME normalized scrubbed line
    appears anywhere in ``base_text`` (regardless of line number), so pure
    insertions above an existing decision do not trip the gate.
    """
    base_norms = {norm for _, norm in compound_decision_lines(base_text)}
    new = compound_decision_lines(new_text)
    return sorted(
        [(ln, norm) for (ln, norm) in new if norm not in base_norms],
        key=lambda t: t[0],
    )


# ---------------------------------------------------------------------------
# Test-side citation index
# ---------------------------------------------------------------------------


def _extract_citations(text: str) -> list[tuple[str, str]]:
    """Every ``(path, function)`` citation inside a ``@par MC/DC:`` block."""
    cites: list[tuple[str, str]] = []
    for block in MCDC_BLOCK_RE.findall(text):
        cites.extend((m.group("path"), m.group("sym")) for m in SYMBOL_CITATION_RE.finditer(block))
    return cites


def has_matching_citation(
    src_path: str,
    src_line: int,
    src_text: str,
    symbol_cites: list[tuple[str, str]],
) -> bool:
    """Whether some test cites the enclosing function of this decision.

    Matches at FUNCTION granularity: a citation names ``path@function``, so
    adding a second decision to an already-cited function satisfies the gate.
    That is deliberate -- line-exact citations would churn on every edit above
    the decision -- but it proves a vector set exists for the function, not
    that the new decision itself is individually covered.
    """
    fn = enclosing_function(src_text, src_line)
    if fn is None:
        return False
    return any(path == src_path and sym == fn for path, sym in symbol_cites)


# ---------------------------------------------------------------------------
# Core audit
# ---------------------------------------------------------------------------


def audit_files(
    files: list[str],
    new_occurrences: list[tuple[str, int, str, str]],
    symbol_cites: list[tuple[str, str]],
) -> list[tuple[str, int, str]]:
    """One finding per function that owns a new uncited structural decision."""
    findings: list[tuple[str, int, str]] = []
    file_set = set(files)
    cite_set = set(symbol_cites)
    reported: set[tuple[str, str]] = set()
    for path, line_no, snippet, symbol in new_occurrences:
        owner = (path, symbol)
        if path not in file_set or owner in reported:
            continue
        reported.add(owner)
        if owner not in cite_set:
            findings.append((path, line_no, snippet))
    return findings


def audit_range(repo: str, base: str, head: str) -> tuple[list[str], list[tuple[str, int, str]]]:
    """Audit files changed between ``base`` and ``head`` in ``repo``.

    Returns ``(changed_files, findings)`` so callers can both report the file
    count (a scan of zero files must never be silent) and act on the findings.
    """
    files = changed_prod_files(repo, base, head)
    symbol_cites = collect_range_citations(repo, head)
    new_occurrences = new_decision_occurrences(
        range_change_pairs(repo, base, head),
        lambda p: _blob_at(repo, head, p),
        lambda p: _blob_at(repo, base, p),
    )
    findings = audit_files(files, new_occurrences, symbol_cites)
    return files, findings


def audit_staged() -> tuple[list[str], list[tuple[str, int, str]]]:
    """Audit the staged index against HEAD (the local pre-commit-hook mode)."""
    files = staged_files()
    symbol_cites = collect_staged_citations()
    new_occurrences = new_decision_occurrences(staged_change_pairs(), staged_blob, head_blob)
    findings = audit_files(files, new_occurrences, symbol_cites)
    return files, findings


# ---------------------------------------------------------------------------
# Whole-tree scan (the ratchet's measurement)
# ---------------------------------------------------------------------------


def production_files(root: Path) -> list[str]:
    """Every production ``.c`` file under ``root``, as sorted repo-relative paths.

    Walks the checked-out tree rather than git, so the scan works identically in
    a developer checkout, a CI ``git archive`` snapshot, and a throwaway
    fixture. Selection is delegated to the same predicate the git-based modes
    use, so all three modes agree on what "production code" means.
    """
    found: list[str] = []
    for prefix in PROD_PREFIXES:
        base = root / prefix.rstrip("/")
        if not base.is_dir():
            continue
        for path in base.rglob("*.c"):
            rel = path.relative_to(root).as_posix()
            if _path_included(rel, prefixes=PROD_PREFIXES):
                found.append(rel)
    return sorted(found)


def _read_text(path: Path) -> str:
    """Contents of ``path``, or "" when it cannot be read.

    An unreadable file yields no decisions and no citations rather than
    aborting the scan; the scope guards in the ratchet are what notice when
    that has happened at a scale that matters.
    """
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def collect_tree_citations(root: Path) -> list[tuple[str, str]]:
    """Every citation in ``root``'s test sources."""
    cites: list[tuple[str, str]] = []
    for tf in _working_test_sources(root):
        cites.extend(_extract_citations(_read_text(tf)))
    return cites


def collect_tree_citation_occurrences(root: Path) -> list[tuple[str, int, str, str]]:
    """Return ``(test path, line, source path, function)`` for every citation."""
    occurrences: list[tuple[str, int, str, str]] = []
    for test_file in _working_test_sources(root):
        text = _read_text(test_file)
        try:
            test_rel = test_file.relative_to(root).as_posix()
        except ValueError:
            test_rel = test_file.as_posix()
        for block_match in MCDC_BLOCK_RE.finditer(text):
            block = block_match.group(0)
            for cite_match in SYMBOL_CITATION_RE.finditer(block):
                offset = block_match.start() + cite_match.start()
                line = text.count("\n", 0, offset) + 1
                occurrences.append(
                    (
                        test_rel,
                        line,
                        cite_match.group("path"),
                        cite_match.group("sym"),
                    )
                )
    return occurrences


def _defined_functions(text: str) -> set[str]:
    """Return function definitions in one clang-formatted C translation unit."""
    functions: set[str] = set()
    for line, source_line in enumerate(text.splitlines(), start=1):
        if source_line != "{":
            continue
        function = enclosing_function(text, line)
        if function is not None:
            functions.add(function)
    return functions


def stale_tree_citations(root: Path) -> list[tuple[str, int, str, str]]:
    """Return citations whose ``path@function`` resolves to no live definition."""
    symbol_index: dict[str, set[str]] = {}
    for source_path in production_files(root):
        symbol_index[source_path] = _defined_functions(_read_text(root / source_path))
    return [
        occurrence
        for occurrence in collect_tree_citation_occurrences(root)
        if occurrence[3] not in symbol_index.get(occurrence[2], set())
    ]


def audit_tree(root: Path) -> tuple[list[str], list[tuple[str, str, int, str]]]:
    """Every uncovered compound decision in the tree at ``root``.

    Returns ``(production_files, findings)`` where each finding is
    ``(path, enclosing_function, line, snippet)``. Unlike the delta modes this
    treats every decision in the tree as in scope, which is what a ratchet needs
    to measure: a count that is invariant under reformatting and that rises the
    moment the tree gains an uncovered decision.

    The file list is returned alongside the findings so a caller can refuse to
    trust a scan that examined implausibly little -- an empty or partial scan
    reports FEWER findings, which reads as an improvement.
    """
    files = production_files(root)
    cites = set(collect_tree_citations(root))
    findings: list[tuple[str, str, int, str]] = []
    for rel in files:
        text = _read_text(root / rel)
        for line_no, normalized in sorted(compound_decision_lines(text)):
            fn = enclosing_function(text, line_no)
            if fn is not None and (rel, fn) in cites:
                continue
            bucket = fn if fn is not None else NO_ENCLOSING_FUNCTION
            findings.append((rel, bucket, line_no, normalized))
    return files, findings


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def _report(files: list[str], findings: list[tuple[str, int, str]], scope: str) -> int:
    """Print the audited file count then the verdict; return the exit code.

    The count is printed unconditionally: a scan that examined zero files can
    never pass silently, so even a legitimately empty diff says so out loud.
    """
    print(f"check_new_compound_has_mcdc.py: audited {len(files)} production file(s) in {scope}.")
    if not files:
        print("check_new_compound_has_mcdc.py: no production file changed -- nothing to audit.")
        return 0
    if not findings:
        print("check_new_compound_has_mcdc.py: 0 findings.")
        return 0

    print()
    print("[FAIL] check_new_compound_has_mcdc.py: new compound boolean")
    print("       decisions landed without an accompanying MC/DC test")
    print("       vector set in an indexed test translation unit.")
    print()
    print("       Per docs/MCDC.md, every `&&` / `||` decision under")
    print("       libs/, apps/shared_libs/, port/, and the discovered")
    print("       firmware product directories must")
    print("       have a co-located or repository test function whose")
    print("       `@par MC/DC:` block cites the decision as")
    print("       `path@function` (the enclosing function of the")
    print("       decision -- a drift-proof anchor, no line numbers).")
    print()
    print("       Offending decisions (path:line is informational):")
    for path, line_no, normalized in findings[:MAX_DISPLAYED_FINDINGS]:
        snippet = (
            normalized
            if len(normalized) <= SNIPPET_MAX_LEN
            else normalized[:SNIPPET_TRUNCATE_LEN] + "..."
        )
        print(f"         {path}:{line_no}: {snippet}")
    if len(findings) > MAX_DISPLAYED_FINDINGS:
        print(f"         ... and {len(findings) - MAX_DISPLAYED_FINDINGS} more")
    print()
    print("       Fix: add a `test_mcdc_<decision>` function in the")
    print("       matching indexed test translation unit with N+1 vectors and")
    print("       a `@par MC/DC:` block citing `path@function`, then")
    print("       re-run. See docs/MCDC.md for the worked example.")
    return 1


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _run_range(spec: str, repo: str) -> int:
    """Resolve and audit a commit range, failing loudly on an unusable scope."""
    rng = resolve_range(repo, spec)
    if rng is None:
        print(
            f"check_new_compound_has_mcdc.py: FATAL -- range '{spec}' does not\n"
            f"       resolve in repository '{repo}'. Refusing to report a clean\n"
            "       scan of zero files: an unresolvable range means the gate is\n"
            "       looking at nothing (the #355 defect), not that the tree is\n"
            "       clean. Under the CI suite the range is resolved against the\n"
            "       history repository (RA8_CI_HISTORY_REPO); pass --repo to it.",
            file=sys.stderr,
        )
        return 2
    base, head = rng
    files, findings = audit_range(repo, base, head)
    return _report(files, findings, f"range {base[:12]}..{head[:12]}")


def main(argv: list[str]) -> int:
    """Dispatch to the selected mode, or fail loudly when none was given.

    Exactly one of ``--selftest`` / ``--range`` / ``--staged`` selects the
    scope. With none of them the check exits 2 rather than silently auditing
    the empty staged set -- the #355 defect that left it toothless in every CI
    run.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--range",
        dest="commit_range",
        metavar="BASE..HEAD",
        help="audit files changed in this commit range (the CI mode)",
    )
    ap.add_argument(
        "--repo",
        default=".",
        metavar="DIR",
        help="repository the range is resolved and read against (default '.')",
    )
    ap.add_argument(
        "--staged",
        action="store_true",
        help="audit the git index against HEAD (the pre-commit-hook mode)",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the detector fires on a new uncovered decision and not otherwise",
    )
    args = ap.parse_args(argv[1:])

    if args.selftest:
        # Deferred import: check_new_compound_has_mcdc_selftest imports FROM
        # this module, so importing it at module load time would cycle.
        from check_new_compound_has_mcdc_selftest import (  # noqa: PLC0415 -- avoids import cycle
            run_selftest,
        )

        return run_selftest()
    if args.commit_range is not None:
        return _run_range(args.commit_range, args.repo)
    if args.staged:
        files, findings = audit_staged()
        return _report(files, findings, "the staged index")

    print(
        "check_new_compound_has_mcdc.py: FATAL -- no scan scope selected.\n"
        "       Pass --range <base..head> [--repo DIR] (CI) or --staged (the\n"
        "       pre-commit hook). This check used to default to `git diff\n"
        "       --cached`, so in any CI checkout -- where nothing is staged --\n"
        "       it saw 0 files and exited 0, auditing nothing in any CI run\n"
        "       (issue #355). A scope that cannot be established is now a\n"
        "       non-PASS, never a clean scan of zero files.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

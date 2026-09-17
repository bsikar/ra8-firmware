#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
r"""check_mcdc_block.py -- Require @par MC/DC: blocks on unit tests.

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B Qualification" and
docs/MCDC.md, a test that exercises a compound boolean decision must declare
its MC/DC vector pattern in a Doxygen ``@par MC/DC:`` block. Without that
block a test can drive a compound decision and still prove nothing about
MC/DC -- it looks like coverage, and the distinction is exactly what DO-178C
Level B turns on. The repo convention is that EVERY unit test carries the
block: a real vector pattern when the code under test has a compound
decision, or an explicit "(no compound decisions in this test ...)"
statement when it does not, so the absence of a block is never left
ambiguous.

This checker enforces the convention: every ``TEST(...)`` / ``TEST_F(...)``
and every ``test_*(void)`` function must have a preceding Doxygen block
containing ``@par MC/DC:``.

Three selection modes, and NO fourth silent one:

  * ``--all`` -- audit every ``tests/**/*.c`` in the tree. This is the mode
    CI uses. It reads the working tree (``git ls-files`` when inside a repo,
    a filesystem walk otherwise), so it is INDEPENDENT of the git index:
    it reports the same finding count in a fresh ``actions/checkout`` (where
    nothing is staged), under ``scripts/ci.sh`` (which stages ``git add
    -A``), and on a developer's checkout. That index-independence is the
    #325 fix -- see below.

  * ``--staged`` -- audit the staged ``tests/**/*.c`` files. This is the mode
    the local ``scripts/git/pre-commit`` hook uses: it gates exactly the test
    files about to be committed.

  * ``--range BASE..HEAD [--repo DIR]`` -- audit the test files changed in a
    commit range. Available for a PR-delta gate; a range that does not
    resolve is FATAL, not a clean scan of nothing.

Invoked with NONE of these modes, the check FAILS LOUDLY (exit 2) rather
than reporting a clean scan of zero files.

That is the #325 defect this rewrite closes: the check used ``git diff
--cached --name-only`` unconditionally, so in any CI checkout -- where
nothing is staged -- it saw 0 files and exited 0, having audited nothing in
any CI run, ever. Meanwhile ``scripts/ci.sh`` stages the whole snapshot
(``git add -A``), so the SAME checker examined every test file locally and
reported a real backlog. The two environments disagreed, silently, and the
one that read as green was the one that checked nothing. A scan that
examined zero files can never exit 0 silently now: the audited file count is
always reported, and a scope that could not be established is a non-PASS.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

MAX_DISPLAYED_FINDINGS = 50  # Max number of findings to print before summarizing the rest.

# A test function: a Google-Test-style ``TEST(suite, name)`` / ``TEST_F`` or a
# bare ``(static) void|int|UINT test_name(void)`` definition. Matches the
# forms the repo's hand-rolled and vendored (ThreadX ``UINT``) test harnesses
# actually use.
TEST_FUNC_PATTERN = re.compile(
    r"""
    (?:
        ^\s*TEST(?:_F)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)
        |
        ^\s*(?:static\s+)?(?:void|int|UINT)\s+(test_[A-Za-z_]\w*)\s*\(\s*void\s*\)
    )
    """,
    re.VERBOSE | re.MULTILINE,
)

DOXYGEN_BLOCK_END_RE = re.compile(r"\*/\s*$")
MCDC_TAG_RE = re.compile(r"@par\s+MC/DC\s*:", re.IGNORECASE)


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------


def _git(*args: str, repo: str = ".") -> str:
    """Run ``git -C repo <args...>`` and return stdout (text)."""
    return subprocess.run(  # noqa: S603  # trusted: fixed git argv
        ["git", "-C", repo, *args],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def _git_ok(*args: str, repo: str = ".") -> tuple[bool, str]:
    """Run ``git -C repo <args...>``; return (success, stdout)."""
    proc = subprocess.run(  # noqa: S603  # trusted: fixed git argv
        ["git", "-C", repo, *args],  # noqa: S607  # trusted: fixed git argv
        check=False,
        capture_output=True,
        text=True,
    )
    return proc.returncode == 0, proc.stdout


def _is_test_c(path: str) -> bool:
    """Whether ``path`` is a first-party unit-test C source file."""
    return path.startswith("tests/") and path.endswith(".c")


def all_test_files(repo: str = ".") -> list[Path]:
    """Every tracked ``tests/**/*.c`` file, index-independent.

    Prefers ``git ls-files`` (the tracked set) when ``repo`` is a git
    checkout, and falls back to a filesystem walk otherwise. Neither path
    consults the index, so the finding count is the same whether or not
    anything is staged -- the whole point of the #325 fix.
    """
    ok, out = _git_ok("ls-files", "--", "tests", repo=repo)
    root = Path(repo)
    if ok and out.strip():
        return [root / p for p in out.splitlines() if _is_test_c(p) and (root / p).is_file()]
    tests_dir = root / "tests"
    if not tests_dir.is_dir():
        return []
    return sorted(tests_dir.rglob("*.c"))


def staged_test_files(repo: str = ".") -> list[Path]:
    """Staged (index) ``tests/**/*.c`` files, excluding deletions.

    Scoped to the index because this backs a pre-commit hook: it polices what
    is about to be committed, not the whole tree.
    """
    out = _git("diff", "--cached", "--name-only", "--diff-filter=ACMR", repo=repo)
    root = Path(repo)
    return [root / p for p in out.splitlines() if _is_test_c(p) and (root / p).is_file()]


def resolve_range(repo: str, spec: str) -> tuple[str, str] | None:
    """Resolve ``BASE..HEAD`` / ``A...B`` / a single rev into a ``(base, head)`` pair.

    Returns ``None`` when either endpoint does not resolve in ``repo`` -- the
    caller turns that into a FATAL exit, never a clean scan of nothing.
    """
    spec = spec.strip()
    if "..." in spec:
        base_spec, head_spec = spec.split("...", 1)
    elif ".." in spec:
        base_spec, head_spec = spec.split("..", 1)
    else:
        base_spec, head_spec = f"{spec}~1", spec
    base_spec = base_spec.strip() or "HEAD~1"
    head_spec = head_spec.strip() or "HEAD"
    ok_b, base = _git_ok("rev-parse", "--verify", "--quiet", f"{base_spec}^{{commit}}", repo=repo)
    ok_h, head = _git_ok("rev-parse", "--verify", "--quiet", f"{head_spec}^{{commit}}", repo=repo)
    if not (ok_b and ok_h and base.strip() and head.strip()):
        return None
    return base.strip(), head.strip()


def range_test_files(repo: str, base: str, head: str) -> list[Path]:
    """The ``tests/**/*.c`` files added/modified between ``base`` and ``head``."""
    out = _git("diff", "--name-only", "--diff-filter=ACMR", base, head, repo=repo)
    root = Path(repo)
    return [root / p for p in out.splitlines() if _is_test_c(p) and (root / p).is_file()]


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------


def preceding_doxygen_block(lines: list[str], func_lineno: int) -> str:
    """Text of the Doxygen block immediately above a 1-based function line.

    Blank lines between the block and the function are tolerated, so ordinary
    spacing does not detach a block from what it documents. Returns "" when no
    block precedes the function.
    """
    end = func_lineno - 2  # zero-based index of the line just above the function
    while end >= 0 and not lines[end].strip():
        end -= 1
    if end < 0:
        return ""
    if not DOXYGEN_BLOCK_END_RE.search(lines[end]):
        return ""
    start = end
    while start >= 0 and "/**" not in lines[start]:
        start -= 1
    if start < 0:
        return ""
    return "\n".join(lines[start : end + 1])


def scan_text(path: str, text: str) -> list[tuple[str, int, str]]:
    """Every test function in ``text`` lacking a preceding ``@par MC/DC:`` block.

    Returns ``(path, 1-based-line, func_name)`` tuples.
    """
    lines = text.splitlines()
    findings: list[tuple[str, int, str]] = []
    for match in TEST_FUNC_PATTERN.finditer(text):
        func_name = next((g for g in match.groups() if g), "<unknown>")
        func_line = text[: match.start()].count("\n") + 1
        block = preceding_doxygen_block(lines, func_line)
        if not block or not MCDC_TAG_RE.search(block):
            findings.append((path, func_line, func_name))
    return findings


def scan_paths(paths: list[Path], *, rel_to: str = ".") -> list[tuple[str, int, str]]:
    """Scan every path, reporting finding paths relative to ``rel_to``."""
    root = Path(rel_to)
    findings: list[tuple[str, int, str]] = []
    for path in paths:
        try:
            display = str(path.relative_to(root))
        except ValueError:
            display = str(path)
        text = path.read_text(encoding="utf-8", errors="ignore")
        findings.extend(scan_text(display, text))
    return findings


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def report(files: list[Path], findings: list[tuple[str, int, str]], scope: str) -> int:
    """Print the verdict for ``findings`` over ``files`` and return the exit code.

    The audited file count is ALWAYS printed, so a scan of zero files can
    never read as a silent clean PASS (the #325 defect).
    """
    if findings:
        print("[FAIL] check_mcdc_block.py: unit tests missing the required")
        print("       @par MC/DC: block.")
        print()
        print("       Per CLAUDE.md and docs/MCDC.md, every unit test must")
        print("       declare its MC/DC vector pattern in a Doxygen")
        print("       `@par MC/DC:` block -- the real N+1 vectors when the")
        print("       code under test has a compound `&&` / `||` decision,")
        print('       or an explicit "(no compound decisions in this test)"')
        print("       statement when it does not.")
        print()
        print(f"       Missing block ({len(findings)} findings; {len(files)} files scanned):")
        for name, lineno, func in findings[:MAX_DISPLAYED_FINDINGS]:
            print(f"         {name}:{lineno}: {func}")
        if len(findings) > MAX_DISPLAYED_FINDINGS:
            print(f"         ... and {len(findings) - MAX_DISPLAYED_FINDINGS} more")
        return 1

    print(f"check_mcdc_block.py: 0 findings ({scope}; {len(files)} files scanned).")
    return 0


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------

_ST_WITH_BLOCK = """\
/**
 * @test present
 * @par MC/DC:
 * Decision: `if (a == 0 || b == 0)` (2 conditions, OR; N+1 = 3 vectors).
 * - V1: a=1,b=1 -> false (control)
 * - V2: a=0,b=1 -> true  (varies a)
 * - V3: a=1,b=0 -> true  (varies b)
 */
static void test_has_block(void)
{
  TEST_ASSERT(guard(0, 1) || guard(1, 0));
}
"""

_ST_NO_BLOCK = """\
/** @test absent -- this test documents no vector pattern. */
static void test_missing_block(void)
{
  TEST_ASSERT(guard(0, 1) || guard(1, 0));
}
"""


def selftest() -> int:
    """Assert the detector fires on a test with no block and stays quiet with one.

    Exercises the REAL ``scan_paths`` code path against throwaway files, so a
    detector that quietly stopped matching cannot pass as clean. Both
    directions are asserted: it must flag ``test_missing_block`` and stay
    silent on ``test_has_block``.
    """
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "tests").mkdir()
        good = root / "tests" / "test_good.c"
        bad = root / "tests" / "test_bad.c"
        good.write_text(_ST_WITH_BLOCK, encoding="utf-8")
        bad.write_text(_ST_NO_BLOCK, encoding="utf-8")

        good_findings = scan_paths([good], rel_to=str(root))
        if good_findings:
            failures.append(f"  fired on a test that HAS a @par MC/DC: block: {good_findings}")

        bad_findings = scan_paths([bad], rel_to=str(root))
        bad_names = {f[2] for f in bad_findings}
        if bad_names != {"test_missing_block"}:
            failures.append(f"  expected exactly the block-less test, got {sorted(bad_names)}")

        both = scan_paths([good, bad], rel_to=str(root))
        if {f[2] for f in both} != {"test_missing_block"}:
            got = sorted(f[2] for f in both)
            failures.append(f"  mixed scan should flag only test_missing_block, got {got}")

    if failures:
        print("check_mcdc_block.py: --selftest FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        "check_mcdc_block.py: --selftest OK "
        "(fires on a test with no @par MC/DC: block; silent on one that has it)."
    )
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _run_range(spec: str, repo: str) -> int:
    """Resolve and audit a commit range, failing loudly on an unusable scope."""
    rng = resolve_range(repo, spec)
    if rng is None:
        print(
            f"check_mcdc_block.py: FATAL -- range '{spec}' does not resolve in\n"
            f"       repository '{repo}'. Refusing to report a clean scan of zero\n"
            "       files: an unresolvable range means the gate is looking at\n"
            "       nothing (the #325 defect), not that the tree is clean.",
            file=sys.stderr,
        )
        return 2
    base, head = rng
    files = range_test_files(repo, base, head)
    findings = scan_paths(files, rel_to=repo)
    return report(files, findings, f"range {base[:12]}..{head[:12]}")


def main(argv: list[str]) -> int:
    """Dispatch to the selected mode, or fail loudly when none was given.

    Exactly one of ``--selftest`` / ``--all`` / ``--staged`` / ``--range``
    selects the scope. With none of them the check exits 2 rather than
    silently auditing the empty staged set -- the #325 defect that left it
    toothless in every CI run.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--all",
        action="store_true",
        help="audit every tests/**/*.c in the tree (the CI mode; index-independent)",
    )
    ap.add_argument(
        "--staged",
        action="store_true",
        help="audit the staged tests/**/*.c files (the pre-commit-hook mode)",
    )
    ap.add_argument(
        "--range",
        dest="commit_range",
        metavar="BASE..HEAD",
        help="audit test files changed in this commit range (a PR-delta mode)",
    )
    ap.add_argument(
        "--repo",
        default=".",
        metavar="DIR",
        help="repository the scope is resolved and read against (default '.')",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the detector fires on a test with no block and not otherwise",
    )
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()
    if args.all:
        files = all_test_files(args.repo)
        findings = scan_paths(files, rel_to=args.repo)
        return report(files, findings, "whole tree")
    if args.commit_range is not None:
        return _run_range(args.commit_range, args.repo)
    if args.staged:
        files = staged_test_files(args.repo)
        findings = scan_paths(files, rel_to=args.repo)
        return report(files, findings, "the staged index")

    print(
        "check_mcdc_block.py: FATAL -- no scan scope selected.\n"
        "       Pass --all (CI, whole tree), --staged (the pre-commit hook),\n"
        "       or --range <base..head> [--repo DIR]. This check used to\n"
        "       default to `git diff --cached`, so in any CI checkout -- where\n"
        "       nothing is staged -- it saw 0 files and exited 0, auditing\n"
        "       nothing in any CI run (issue #325). A scope that cannot be\n"
        "       established is now a non-PASS, never a clean scan of zero files.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

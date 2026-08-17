# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction regression test for check_new_compound_has_mcdc.py.

Split out of the detector itself to keep both files under the repository's
per-file line cap (matching the ``doxy_audit.py`` / ``doxy_selftest.py``
split). Exercises the REAL range-audit code path against a throwaway git
repository, so a detector that quietly stopped matching cannot pass as clean:
every fixture below asserts a real gap FIRES and a correctly covered or
exempt form of the same shape stays SILENT.

Imports the mechanism under test from ``check_new_compound_has_mcdc`` rather
than re-implementing it, so there is exactly one definition of "this decision
lacks MC/DC vectors" for both the detector and its own regression test.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from check_new_compound_has_mcdc import (
    _git,
    _is_test_source_name,
    _working_test_sources,
    audit_range,
    resolve_range,
)

_ST_PRE_C = """\
ra8_err_t ra8_pre_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

# The head revision of the pre-existing file: modified (a comment added) but
# the decision line is byte-identical, so it must NOT be flagged as new.
_ST_PRE_C_HEAD = """\
ra8_err_t ra8_pre_fn(int a, int b)
{
  /* unrelated edit that touches the file but not the decision */
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_NEWDEC_C = """\
ra8_err_t ra8_new_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_COVERED_C = """\
ra8_err_t ra8_covered_fn(int c, int d)
{
  if (c && d) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_TEST_COVERED_C = """\
/**
 * @test ra8_covered_fn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (c && d)` cites libs/covered.c@ra8_covered_fn
 * - Vector 1: c=1, d=1 -> true
 * - Vector 2: c=0, d=1 -> false (varies c)
 * - Vector 3: c=1, d=0 -> false (varies d)
 */
void test_mcdc_ra8_covered_fn(void) {}
"""

_ST_SOUP_C = """\
int soup_fn(int e, int f)
{
  if (e && f) {
    return 1;
  }
  return 0;
}
"""

# A NOLINTNEXTLINE marker mid-parameter-list, the exact shape that made
# enclosing_function() return "NOLINTNEXTLINE" instead of the real function
# name (its own parenthesised rule name satisfied the walk's "(" stop
# condition before the walk reached the true signature). Uncovered and not
# cited anywhere: must fire, attributed to the real function.
_ST_NOLINT_SIG_C = """\
ra8_err_t ra8_nolint_sig_fn(int g,
                            // NOLINTNEXTLINE(readability-non-const-parameter)
                            int h)
{
  if (g && h) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

# Same shape, but cited under the REAL function name. If enclosing_function()
# regressed to reading "NOLINTNEXTLINE" as the name, this citation (which
# names the true function) would not match and the decision would wrongly
# fire despite being covered: must stay quiet.
_ST_NOLINT_SIG_COVERED_C = """\
ra8_err_t ra8_nolint_sig_covered_fn(int i,
                                    // NOLINTNEXTLINE(readability-non-const-parameter)
                                    int j)
{
  if (i && j) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_TEST_NOLINT_SIG_COVERED_C = """\
/**
 * @test ra8_nolint_sig_covered_fn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (i && j)` cites
 * libs/nolint_sig_covered.c@ra8_nolint_sig_covered_fn
 * - Vector 1: i=1, j=1 -> true
 * - Vector 2: i=0, j=1 -> false (varies i)
 * - Vector 3: i=1, j=0 -> false (varies j)
 */
void test_mcdc_ra8_nolint_sig_covered_fn(void) {}
"""


def _st_git(repo: Path, *args: str) -> None:
    """Run a git command inside the self-test fixture repository."""
    subprocess.run(  # noqa: S603  # trusted: fixed git argv, temp repo
        [  # noqa: S607
            "git",
            "-C",
            str(repo),
            "-c",
            "user.email=selftest@localhost",
            "-c",
            "user.name=selftest",
            *args,
        ],
        check=True,
        capture_output=True,
        text=True,
    )


def _st_write(repo: Path, rel: str, body: str) -> None:
    """Write ``body`` to ``rel`` under ``repo``, creating parent dirs."""
    dst = repo / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(body, encoding="utf-8")


def _st_build_fixture(repo: Path) -> tuple[str, str]:
    """Build a two-commit fixture and return the ``(base, head)`` SHAs.

    The base commit holds a pre-existing decision; the head commit adds a
    genuinely new uncovered decision (must fire), a new decision that carries
    vectors (must stay quiet), a SOUP decision (excluded), and a cosmetic edit
    to the pre-existing file (unchanged decision must stay quiet).
    """
    repo.mkdir(parents=True, exist_ok=True)
    _st_git(repo, "init", "--quiet")
    _st_write(repo, "libs/pre.c", _ST_PRE_C)
    _st_write(repo, "tests/.keep", "")
    _st_git(repo, "add", "-A")
    _st_git(repo, "commit", "--quiet", "--no-verify", "-m", "base")
    base = _git("-C", str(repo), "rev-parse", "HEAD").strip()

    _st_write(repo, "libs/pre.c", _ST_PRE_C_HEAD)
    _st_write(repo, "libs/newdec.c", _ST_NEWDEC_C)
    _st_write(repo, "libs/covered.c", _ST_COVERED_C)
    _st_write(repo, "tests/test_covered.cpp", _ST_TEST_COVERED_C)
    _st_write(repo, "libs/third_party/soup.c", _ST_SOUP_C)
    _st_write(repo, "libs/nolint_sig.c", _ST_NOLINT_SIG_C)
    _st_write(repo, "libs/nolint_sig_covered.c", _ST_NOLINT_SIG_COVERED_C)
    _st_write(repo, "tests/test_nolint_sig_covered.cpp", _ST_TEST_NOLINT_SIG_COVERED_C)
    _st_git(repo, "add", "-A")
    _st_git(repo, "commit", "--quiet", "--no-verify", "-m", "head")
    head = _git("-C", str(repo), "rev-parse", "HEAD").strip()
    return base, head


def _st_check(files: list[str], findings: list[tuple[str, int, str]]) -> list[str]:
    """Assert the range audit fired on the right decision and nowhere else."""
    found_paths = {p for p, _, _ in findings}
    failures: list[str] = []
    if "libs/newdec.c" not in found_paths:
        failures.append("  did NOT fire on libs/newdec.c (a new uncovered decision)")
    if "libs/covered.c" in found_paths:
        failures.append("  fired on libs/covered.c (it already carries MC/DC vectors)")
    if "libs/pre.c" in found_paths:
        failures.append("  fired on libs/pre.c (a pre-existing, unchanged decision)")
    if any("third_party" in p for p in found_paths):
        failures.append("  fired inside libs/third_party/ (SOUP is exempt)")
    if "libs/nolint_sig.c" not in found_paths:
        failures.append(
            "  did NOT fire on libs/nolint_sig.c (uncovered decision behind a "
            "NOLINTNEXTLINE-commented multi-line signature)"
        )
    if "libs/nolint_sig_covered.c" in found_paths:
        failures.append(
            "  fired on libs/nolint_sig_covered.c (cited under its real function "
            "name; enclosing_function() must skip the NOLINTNEXTLINE comment "
            "line, not resolve it as the function name)"
        )
    if "libs/third_party/soup.c" in files:
        failures.append("  selected a libs/third_party/ file (SOUP must be excluded)")
    if len(findings) != 2:  # noqa: PLR2004 -- fixture plants exactly 2 uncovered decisions
        failures.append(
            f"  expected exactly 2 finding(s), got {len(findings)}: {sorted(found_paths)}"
        )
    return failures


def _st_check_test_scope() -> list[str]:
    """Assert the test-source name rule, in both directions.

    Must accept both orderings of the convention and both extensions, and must
    still REFUSE an ordinary source file -- a rule that widened to every .c
    would sweep production files into the citation index and make uncovered
    decisions look covered.

    Returns:
        One message per misclassification; empty when the rule is correct.
    """
    failures: list[str] = [
        f"  {name} is not recognised as a test translation unit"
        for name in ("test_x.c", "test_x.cpp", "x_test.c", "x_test.cpp")
        if not _is_test_source_name(name)
    ]
    failures += [
        f"  {name} was wrongly recognised as a test translation unit"
        for name in ("latest.c", "protest.cpp", "x_tester.c", "test_x.h", "mdl_net.c")
        if _is_test_source_name(name)
    ]
    with tempfile.TemporaryDirectory() as td:
        tests_dir = Path(td) / "tests"
        (tests_dir / "support").mkdir(parents=True)
        (tests_dir / "test_top.c").write_text("", encoding="utf-8")
        (tests_dir / "support" / "thing_test.c").write_text("", encoding="utf-8")
        (tests_dir / "support" / "helper.c").write_text("", encoding="utf-8")
        found = {path.name for path in _working_test_sources(tests_dir)}
    if found != {"test_top.c", "thing_test.c"}:
        failures.append(f"  the recursive enumeration returned {sorted(found)}")
    return failures


def run_selftest() -> int:
    """Assert the detector fires on a new uncovered decision and stays quiet otherwise.

    Exercises the REAL range-audit code path against a throwaway repository, so
    a detector that quietly stopped matching cannot pass as clean. Both
    directions are asserted: it must fire on ``libs/newdec.c`` and stay silent
    on the pre-existing decision, the vector-covered decision, and the SOUP
    decision.
    """
    with tempfile.TemporaryDirectory() as td:
        repo = Path(td) / "fixture"
        base, head = _st_build_fixture(repo)
        rng = resolve_range(str(repo), f"{base}..{head}")
        if rng is None:
            print("check_new_compound_has_mcdc.py: --selftest FAILED", file=sys.stderr)
            print("  a valid base..head range did not resolve", file=sys.stderr)
            return 1
        files, findings = audit_range(str(repo), *rng)
        failures = _st_check(files, findings)
    failures += _st_check_test_scope()

    if failures:
        print("check_new_compound_has_mcdc.py: --selftest FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        "check_new_compound_has_mcdc.py: --selftest OK "
        "(fires on a new uncovered decision; silent on pre-existing, "
        "vector-covered, and SOUP decisions; test-source scope holds both ways)."
    )
    return 0

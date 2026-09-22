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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from check_new_compound_has_mcdc import (
    _git,
    _is_test_source_name,
    _working_test_sources,
    audit_range,
    compound_decision_lines,
    resolve_range,
)
from git_environment import isolated_git_environment, trusted_git_executable

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

_ST_APP_NEWDEC_C = """\
ra8_err_t ra8_app_new_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_APP_COVERED_C = """\
ra8_err_t ra8_app_covered_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_APP_COLOCATED_TEST_C = """\
/**
 * @test ra8_app_covered_fn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (a && b)` cites
 * apps/shared_libs/covered/src/covered.c@ra8_app_covered_fn
 * - Vector 1: a=1, b=1 -> true
 * - Vector 2: a=0, b=1 -> false (varies a)
 * - Vector 3: a=1, b=0 -> false (varies b)
 */
void test_mcdc_ra8_app_covered_fn(void) {}
"""

_ST_MOVE_BASE_C = """\
ra8_err_t ra8_moved_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_MOVE_HEAD_C = """\
ra8_err_t ra8_moved_fn(int left, int right)
{
  if (left &&
      right) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_MOVE_EDIT_BASE_C = """\
ra8_err_t ra8_moved_edit_fn(int a, int b, int c)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_MOVE_EDIT_HEAD_C = """\
ra8_err_t ra8_moved_edit_fn(int a, int b, int c)
{
  if ((a && b) || c) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_SUBSTITUTE_BASE_C = """\
ra8_err_t ra8_substitute_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_SUBSTITUTE_HEAD_C = """\
ra8_err_t ra8_substitute_fn(int c, int d)
{
  if (c || d) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_SPLIT_BASE_C = """\
ra8_err_t ra8_split_first(int a, int b)
{
  return (a && b) ? k_ra8_ok : k_ra8_err;
}

ra8_err_t ra8_split_second(int c, int d)
{
  return (c || d) ? k_ra8_ok : k_ra8_err;
}
"""

_ST_SPLIT_HEAD_C = """\
ra8_err_t ra8_split_first(int a, int b)
{
  return (a && b) ? k_ra8_ok : k_ra8_err;
}
"""

_ST_SPLIT_EXTRACTED_C = """\
ra8_err_t ra8_split_second(int left, int right)
{
  return (left || right) ? k_ra8_ok : k_ra8_err;
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

_ST_INDEX_BASE_C = """\
ra8_err_t ra8_staged_fn(int a, int b)
{
  if (a) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_INDEX_HEAD_C = """\
ra8_err_t ra8_staged_fn(int a, int b)
{
  if (a && b) {
    return k_ra8_ok;
  }
  return k_ra8_err;
}
"""

_ST_INDEX_TEST_C = """\
/**
 * @test ra8_staged_fn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (a && b)` cites libs/staged.c@ra8_staged_fn
 * - Vector 1: a=1, b=1 -> true
 * - Vector 2: a=0, b=1 -> false (varies a)
 * - Vector 3: a=1, b=0 -> false (varies b)
 */
void test_mcdc_ra8_staged_fn(void) {}
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

#: Sentinel a firing fixture line carries, so the expectation is read off the
#: fixture itself. Asserting the exact LINE NUMBERS that fire -- rather than a
#: count -- also proves the lexical view preserves source line numbering: a
#: masking bug that swallowed a newline could not pass as clean.
_ST_LEXICAL_MARKER = "ST-DECISION"

#: Every construct that must NEVER read as a compound decision: multi-line
#: Doxygen prose (including an `@code` span), literals holding comment
#: delimiters, escaped quotes, a backslash-spliced line comment, a spliced
#: `#define`, and a `#if` guard.
_ST_LEXICAL_QUIET_C = """\
/**
 * @brief Multi-line Doxygen prose is not code.
 *
 * @details The interior lines of this block are exactly what a line-local
 * scrub cannot see: a decision quoted as `(a == 1) && (b == 2)`, SEC1
 * notation of the form ``0x04 || X(32) || Y(32)``, and a signature spelled
 * ``raw r||s``.
 *
 * @par MC/DC:
 * Decision: `if (a)` (1 condition, no compound `&&`/`||`).
 *
 * @code
 * if (example_a && example_b) {
 *   return 0;
 * }
 * @endcode
 */
static int st_quiet_fn(int a, int b)
{
  /* single-line block comment holding a && b */
  const char *plain = "a && b";
  const char *fake_open = "/* not a comment */ || still a string";
  const char *escaped = "he said \\" && \\" and meant it";
  const char apostrophe = '\\'';
  // spliced line comment whose continuation still holds \\
  the operators && and || on the next source line
  (void)plain;
  (void)fake_open;
  (void)escaped;
  (void)apostrophe;
  return a + b;
}

#define ST_QUIET_MACRO(a, b) \\
  ((a) && (b))

#if defined(ST_QUIET_ONE) || defined(ST_QUIET_TWO)
static int st_quiet_directive(void) { return 1; }
#endif
"""

#: Every construct that MUST read as a compound decision. Each such line
#: carries `_ST_LEXICAL_MARKER`; no other line may. The unterminated-looking
#: `"/*"` literal is the over-blanking probe: a lexer that let it open a
#: comment would silence both decisions below it.
_ST_LEXICAL_FIRES_C = """\
static int st_fire_fn(int a, int b)
{
  const char *opener = "/*";
  (void)opener;
  if (a && b) { /* ST-DECISION -- a literal must not open a comment */
    return 1;
  }
  /* a block comment ending mid-line: || */ if (a || b) { /* ST-DECISION */
    return 2;
  }
  return 0;
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
        [
            trusted_git_executable(),
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
    _st_write(repo, "apps/shared_libs/moved/src/original.c", _ST_MOVE_BASE_C)
    _st_write(repo, "apps/shared_libs/moved_edit/src/original.c", _ST_MOVE_EDIT_BASE_C)
    _st_write(repo, "apps/shared_libs/substitute/src/predicate.c", _ST_SUBSTITUTE_BASE_C)
    _st_write(repo, "apps/shared_libs/split/src/combined.c", _ST_SPLIT_BASE_C)
    _st_write(repo, "tests/.keep", "")
    _st_git(repo, "add", "-A")
    _st_git(repo, "commit", "--quiet", "--no-verify", "-m", "base")
    base = _git("-C", str(repo), "rev-parse", "HEAD").strip()

    _st_write(repo, "libs/pre.c", _ST_PRE_C_HEAD)
    _st_write(repo, "libs/newdec.c", _ST_NEWDEC_C)
    _st_write(repo, "apps/shared_libs/demo/src/newdec.c", _ST_APP_NEWDEC_C)
    _st_write(repo, "apps/shared_libs/covered/src/covered.c", _ST_APP_COVERED_C)
    _st_write(
        repo,
        "apps/shared_libs/covered/tests/src/test_covered.c",
        _ST_APP_COLOCATED_TEST_C,
    )
    _st_write(repo, "libs/covered.c", _ST_COVERED_C)
    _st_write(repo, "tests/test_covered.cpp", _ST_TEST_COVERED_C)
    _st_write(repo, "libs/third_party/soup.c", _ST_SOUP_C)
    _st_write(repo, "apps/shared_libs/third_party/soup/source.c", _ST_SOUP_C)
    _st_write(repo, "libs/nolint_sig.c", _ST_NOLINT_SIG_C)
    _st_write(repo, "libs/nolint_sig_covered.c", _ST_NOLINT_SIG_COVERED_C)
    _st_write(repo, "tests/test_nolint_sig_covered.cpp", _ST_TEST_NOLINT_SIG_COVERED_C)
    _st_git(
        repo,
        "mv",
        "apps/shared_libs/moved/src/original.c",
        "apps/shared_libs/moved/src/relocated.c",
    )
    _st_write(repo, "apps/shared_libs/moved/src/relocated.c", _ST_MOVE_HEAD_C)
    _st_git(
        repo,
        "mv",
        "apps/shared_libs/moved_edit/src/original.c",
        "apps/shared_libs/moved_edit/src/relocated.c",
    )
    _st_write(repo, "apps/shared_libs/moved_edit/src/relocated.c", _ST_MOVE_EDIT_HEAD_C)
    _st_write(repo, "apps/shared_libs/substitute/src/predicate.c", _ST_SUBSTITUTE_HEAD_C)
    _st_write(repo, "apps/shared_libs/split/src/combined.c", _ST_SPLIT_HEAD_C)
    _st_write(repo, "apps/shared_libs/split/src/extracted.c", _ST_SPLIT_EXTRACTED_C)
    _st_git(repo, "add", "-A")
    _st_git(repo, "commit", "--quiet", "--no-verify", "-m", "head")
    head = _git("-C", str(repo), "rev-parse", "HEAD").strip()
    return base, head


def _st_check_move_contract(found_paths: set[str]) -> list[str]:
    """Check move/split/substitution and app-colocated citation behavior."""
    failures: list[str] = []
    if "apps/shared_libs/moved_edit/src/relocated.c" not in found_paths:
        failures.append("  did NOT fire when a moved function gained a compound operator")
    if "apps/shared_libs/moved/src/relocated.c" in found_paths:
        failures.append("  fired on a moved/reformatted decision with no operator growth")
    if "apps/shared_libs/split/src/extracted.c" in found_paths:
        failures.append("  fired on a same-component function extraction")
    if "apps/shared_libs/substitute/src/predicate.c" not in found_paths:
        failures.append("  did NOT fire on a same-count structural predicate replacement")
    if "apps/shared_libs/covered/src/covered.c" in found_paths:
        failures.append("  ignored an app-colocated MC/DC citation")
    return failures


def _st_check(files: list[str], findings: list[tuple[str, int, str]]) -> list[str]:
    """Assert the range audit fired on the right decision and nowhere else."""
    found_paths = {p for p, _, _ in findings}
    failures = _st_check_move_contract(found_paths)
    if "libs/newdec.c" not in found_paths:
        failures.append("  did NOT fire on libs/newdec.c (a new uncovered decision)")
    if "apps/shared_libs/demo/src/newdec.c" not in found_paths:
        failures.append("  did NOT fire on app-owned production code")
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
    if any("third_party" in path for path in files):
        failures.append("  selected a third_party file (SOUP must be excluded)")
    if len(findings) != 5:  # noqa: PLR2004 -- fixture plants exactly 5 uncovered functions
        failures.append(
            f"  expected exactly 5 finding(s), got {len(findings)}: {sorted(found_paths)}"
        )
    return failures


def _st_check_lexical_view() -> list[str]:
    """Assert the lexical view hides prose and literals but never real code.

    Both directions, because either alone proves nothing: a view that blanked
    the whole file would satisfy the quiet fixture while silently reporting no
    debt anywhere, and a view that blanked nothing would satisfy a firing
    fixture while counting every comment as a decision. The firing expectation
    is the exact set of marked line numbers, which doubles as the non-vacuity
    floor -- an empty result can never satisfy it.
    """
    failures: list[str] = []
    quiet = compound_decision_lines(_ST_LEXICAL_QUIET_C)
    if quiet:
        detail = "; ".join(f"line {line}: {text}" for line, text in sorted(quiet))
        failures.append(f"  prose, a literal, or a directive read as a decision: {detail}")
    expected = {
        index
        for index, line in enumerate(_ST_LEXICAL_FIRES_C.splitlines(), start=1)
        if _ST_LEXICAL_MARKER in line
    }
    fired = {line for line, _text in compound_decision_lines(_ST_LEXICAL_FIRES_C)}
    if fired != expected:
        failures.append(
            f"  real decisions fired on lines {sorted(fired)}; expected {sorted(expected)}"
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


def _st_staged_result(repo: Path) -> int:
    """Run the real staged-mode CLI against ``repo`` and return its status."""
    checker = Path(__file__).with_name("check_new_compound_has_mcdc.py")
    return subprocess.run(  # noqa: S603 -- trusted interpreter/checker paths
        [sys.executable, str(checker), "--staged"],
        cwd=repo,
        check=False,
        capture_output=True,
        text=True,
    ).returncode


def _st_check_index_citations() -> list[str]:
    """Prove staged mode reads citations from the index in both directions."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        repo = Path(td) / "fixture"
        repo.mkdir()
        _st_git(repo, "init", "--quiet")
        _st_write(repo, "libs/staged.c", _ST_INDEX_BASE_C)
        _st_git(repo, "add", "-A")
        _st_git(repo, "commit", "--quiet", "--no-verify", "-m", "base")
        _st_write(repo, "libs/staged.c", _ST_INDEX_HEAD_C)
        _st_git(repo, "add", "libs/staged.c")
        _st_write(repo, "tests/test_staged.c", _ST_INDEX_TEST_C)
        if _st_staged_result(repo) != 1:
            failures.append("  an untracked citation satisfied the staged decision gate")
        _st_git(repo, "add", "tests/test_staged.c")
        if _st_staged_result(repo) != 0:
            failures.append("  a staged citation did not satisfy the staged decision gate")
        _st_write(repo, "tests/test_staged.c", "/* unstaged removal */\n")
        if _st_staged_result(repo) != 0:
            failures.append("  an unstaged citation removal changed the index verdict")
    return failures


def _run_selftest_body() -> int:
    """Assert the detector fires on a new uncovered decision and stays quiet otherwise.

    Exercises the REAL range-audit code path against a throwaway repository, so
    a detector that quietly stopped matching cannot pass as clean. Both
    directions are asserted: it must fire on platform and app-owned production
    code and stay silent on the pre-existing decision, the vector-covered
    decision, and both vendor-root SOUP decisions. ``_st_check_lexical_view()``
    asserts the same pair for the lexical view the measurement reads through,
    so comment prose can never be frozen into the ratchet baseline as debt.
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
    failures += _st_check_lexical_view()
    failures += _st_check_test_scope()
    failures += _st_check_index_citations()

    if failures:
        print("check_new_compound_has_mcdc.py: --selftest FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        "check_new_compound_has_mcdc.py: --selftest OK "
        "(fires on new platform/app and moved-with-growth decisions; silent on "
        "moves, splits, alpha-renames, colocated vectors, and both "
        "SOUP roots; test-source scope holds both ways; the lexical view hides "
        "multi-line prose, literals, and spliced directives while every marked "
        "real decision still fires)."
    )
    return 0


def run_selftest() -> int:
    """Run range and index fixtures without inheriting the caller's repo."""
    with isolated_git_environment():
        return _run_selftest_body()

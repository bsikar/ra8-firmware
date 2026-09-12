#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: first-party Go verification (vet, static analysis, tests, coverage).

Scope is derived, not hardcoded
-------------------------------
``git ls-files`` enumerates every tracked or untracked-but-not-ignored,
present ``*.go`` file, so a new module is covered the day it is added with no
allowlist to forget. Build-output trees (``build/``, ``_deps/``) are dropped
through :mod:`lint_targets`, matching every other provider.

Verification Modes
------------------
- **Lint / Static Analysis** (default): ``go vet ./...`` checks correctness,
  and ``staticcheck ./...`` runs if available on PATH. Formatting (``gofmt``)
  is enforced by the format gate (``format_tree.sh``), never here.
- **Test execution** (``--test``): runs ``go test -race -v ./...`` across every
  discovered module root.
- **Coverage gating** (``--coverage``): runs ``go test -cover`` (with ``-coverpkg=./...``)
  and fails if statement coverage falls below ``--floor <pct>`` (defaulting to 85.0%).

Non-vacuity
-----------
``--selftest`` feeds the tools deliberately non-conforming fixtures and asserts they
fire, clean fixtures and asserts silence, tests the race detector, exercises coverage
floor enforcement, and verifies the worktree-scope exclusion check. A collapsed scope
trips the file floor instead of reporting a clean tree.

Run::

    check_go.py                         # lint gate (vet, staticcheck)
    check_go.py --test                  # run tests with race detection
    check_go.py --coverage              # run coverage against floor (default 85%)
    check_go.py --coverage --floor 90   # run coverage against 90% floor
    check_go.py --test --coverage       # test + coverage
    check_go.py --require               # fail (not skip) if go is absent
    check_go.py --selftest              # prove verification tools fire and stay quiet

Exit 0 if clean, exit 1 on findings or test/coverage failures, exit 2 on a tool
error or a scope that collapsed below the file floor.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _find_go() -> str | None:
    env = os.environ.get("GO")
    if env and Path(env).exists():
        return env
    return shutil.which("go")


def _find_staticcheck() -> str | None:
    """Path to staticcheck on PATH or in STATICCHECK env var, if present."""
    env = os.environ.get("STATICCHECK")
    if env and Path(env).exists():
        return env
    return shutil.which("staticcheck")


def _git_ls_files(*pathspec: str) -> list[str]:
    """Return tracked or untracked/non-ignored paths matching `pathspec`."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            *pathspec,
        ],
        cwd=_repo_root(),
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check_go.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def _present_files(files: list[str], root: Path | None = None) -> list[str]:
    """Return candidate paths that still exist in the worktree."""
    base = _repo_root() if root is None else root
    return [rel for rel in files if (base / rel).is_file()]


def _tracked_go_files() -> list[str]:
    """Every candidate-worktree Go file, minus build-output trees."""
    by_extension = _present_files(_git_ls_files("*.go"))
    return sorted(rel for rel in by_extension if not is_build_output_path(rel))


def _module_roots(files: list[str]) -> list[Path]:
    """Distinct directories holding a go.mod above each of `files`."""
    roots: set[Path] = set()
    for rel in files:
        directory = (_repo_root() / rel).parent
        while directory != directory.parent:
            if (directory / "go.mod").is_file():
                roots.add(directory)
                break
            if directory == _repo_root():
                break
            directory = directory.parent
    return sorted(roots)


def _run_vet(go: str, roots: list[Path]) -> dict[str, str]:
    """Run `go vet ./...` per module root; map root to stderr on failure."""
    findings: dict[str, str] = {}
    for root in roots:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [go, "vet", "./..."],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            rel = (
                str(root.relative_to(_repo_root()))
                if root.is_relative_to(_repo_root())
                else str(root)
            )
            findings[rel] = (proc.stdout + proc.stderr).strip()
    return findings


def _run_staticcheck(staticcheck: str, roots: list[Path]) -> dict[str, str]:
    """Run `staticcheck ./...` per module root; map root to output on failure."""
    findings: dict[str, str] = {}
    for root in roots:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [staticcheck, "./..."],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            rel = (
                str(root.relative_to(_repo_root()))
                if root.is_relative_to(_repo_root())
                else str(root)
            )
            findings[rel] = (proc.stdout + proc.stderr).strip()
    return findings


def _run_tests(go: str, roots: list[Path]) -> dict[str, str]:
    failures: dict[str, str] = {}
    env = os.environ.copy()
    env["GOWORK"] = "off"
    for root in roots:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [go, "test", "-race", "-v", "./..."],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            rel = (
                str(root.relative_to(_repo_root()))
                if root.is_relative_to(_repo_root())
                else str(root)
            )
            failures[rel] = (proc.stdout + proc.stderr).strip()
    return failures


def _parse_coverage(stdout: str) -> list[tuple[str, float, bool]]:
    """Parse (package_or_scope, coverage_pct, is_module_wide) from `go test -cover` output."""
    results: list[tuple[str, float, bool]] = []
    for raw in stdout.splitlines():
        line = raw.strip()
        match = re.search(r"coverage:\s+([0-9]+(?:\.[0-9]+)?)\%\s+of\s+statements", line)
        if not match:
            continue
        pct = float(match.group(1))
        # Non-test packages output `\t<pkg>\t\tcoverage: 0.0% of statements` under -coverpkg
        if not raw.startswith(("ok\t", "ok ")) and pct == 0.0:
            continue
        parts = line.split()
        pkg = parts[1] if len(parts) > 1 and parts[0] in ("ok", "FAIL") else parts[0]
        is_module_wide = " in ./..." in line
        results.append((pkg, pct, is_module_wide))
    return results


def _run_coverage(
    go: str, roots: list[Path], floor: float
) -> tuple[dict[str, str], dict[str, str]]:
    """Run `go test -cover` across module roots, verifying coverage >= floor."""
    successes: dict[str, str] = {}
    failures: dict[str, str] = {}
    env = os.environ.copy()
    env["GOWORK"] = "off"
    for root in roots:
        rel = (
            str(root.relative_to(_repo_root())) if root.is_relative_to(_repo_root()) else str(root)
        )
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [go, "test", "-cover", "-coverpkg=./...", "./..."],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            failures[rel] = (proc.stdout + proc.stderr).strip()
            continue

        results = _parse_coverage(proc.stdout)
        if not results:
            failures[rel] = "no statement coverage reported (0 tests or statements executed)"
            continue

        module_wide = [pct for (_, pct, is_mod) in results if is_mod]
        if module_wide:
            effective_pct = max(module_wide)
            if effective_pct < floor:
                failures[rel] = (
                    f"coverage {effective_pct:.1f}% is below floor {floor:.1f}% "
                    f"(target: {floor:.1f}%)"
                )
            else:
                successes[rel] = f"coverage {effective_pct:.1f}% (floor {floor:.1f}%)"
        else:
            below_floor = [(pkg, pct) for (pkg, pct, _) in results if pct < floor]
            if below_floor:
                details = ", ".join(f"{pkg}: {pct:.1f}%" for pkg, pct in below_floor)
                failures[rel] = f"package(s) below floor {floor:.1f}%: {details}"
            else:
                min_pct = min(pct for (_, pct, _) in results)
                successes[rel] = f"coverage {min_pct:.1f}% (floor {floor:.1f}%)"

    return successes, failures


def _parse_floor(args: list[str]) -> float:
    """Parse --floor <pct> or --floor=<pct>, defaulting to 85.0."""
    default_floor = 85.0
    for i, arg in enumerate(args):
        if arg == "--floor":
            if i + 1 < len(args):
                try:
                    return float(args[i + 1])
                except ValueError:
                    sys.stderr.write(f"check_go.py: invalid --floor value: {args[i + 1]}\n")
                    sys.exit(2)
            else:
                sys.stderr.write("check_go.py: --floor requires a numeric argument\n")
                sys.exit(2)
        elif arg.startswith("--floor="):
            val = arg.split("=", 1)[1]
            try:
                return float(val)
            except ValueError:
                sys.stderr.write(f"check_go.py: invalid --floor value: {val}\n")
                sys.exit(2)
    return default_floor


def _report(
    vet: dict[str, str],
    staticcheck: dict[str, str] | None = None,
) -> None:
    if vet:
        sys.stderr.write("check_go.py: `go vet` finding(s):\n")
        for relroot in sorted(vet):
            sys.stderr.write(f"  {relroot}:\n")
            for line in vet[relroot].splitlines():
                sys.stderr.write(f"    {line}\n")
    if staticcheck:
        sys.stderr.write("check_go.py: `staticcheck` finding(s):\n")
        for relroot in sorted(staticcheck):
            sys.stderr.write(f"  {relroot}:\n")
            for line in staticcheck[relroot].splitlines():
                sys.stderr.write(f"    {line}\n")
    sys.stderr.write("\nFix the finding.\n")


# ---------------------------------------------------------------------------
# Selftest
#
# Fixtures live in throwaway directories, never in the tree: a deliberately
# non-conforming .go file stored as a real file would be picked up by the
# gate's own scan and fail it.
# ---------------------------------------------------------------------------


def _vet_fails(go: str, source: str) -> bool:
    """True when `go vet` rejects a scratch module holding `source`."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "go.mod").write_text("module selftest\n\ngo 1.24\n", encoding="utf-8")
        (root / "fixture.go").write_text(source, encoding="utf-8")
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [go, "vet", "./..."],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        return proc.returncode != 0


def _staticcheck_fails(staticcheck: str, source: str) -> bool:
    """True when `staticcheck ./...` rejects a scratch module holding `source`."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "go.mod").write_text("module selftest\n\ngo 1.24\n", encoding="utf-8")
        (root / "fixture.go").write_text(source, encoding="utf-8")
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [staticcheck, "./..."],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        return proc.returncode != 0


def _selftest_vet(go: str, failures: list[str]) -> None:
    vet_bad = 'package selftest\n\nfunc f() int {\n\treturn "not an int"\n}\n'
    vet_good = "package selftest\n\nfunc f() int {\n\treturn 1\n}\n"
    if not _vet_fails(go, vet_bad):
        failures.append("  must-fire: `go vet` accepted a mistyped return")
    if _vet_fails(go, vet_good):
        failures.append("  must-stay-quiet: `go vet` rejected the clean fixture")


def _selftest_staticcheck_fn(staticcheck: str, failures: list[str]) -> None:
    sc_bad = "package selftest\n\nfunc dead() {\n\treturn\n\tprintln(1)\n}\n"
    sc_good = (
        "package selftest\n\n// Add returns sum.\nfunc Add(a, b int) int {\n\treturn a + b\n}\n"
    )
    if not _staticcheck_fails(staticcheck, sc_bad):
        failures.append("  must-fire: `staticcheck` accepted dead code / unused function")
    if _staticcheck_fails(staticcheck, sc_good):
        failures.append("  must-stay-quiet: `staticcheck` rejected clean exported function")


def _selftest_tests(go: str, root: Path, failures: list[str]) -> None:
    root = root.resolve()
    (root / "go.mod").write_text("module testrun\n\ngo 1.24\n", encoding="utf-8")
    (root / "calc.go").write_text(
        "package testrun\n\nfunc Add(a, b int) int { return a + b }\n",
        encoding="utf-8",
    )
    (root / "calc_test.go").write_text(
        'package testrun\n\nimport "testing"\n\nfunc TestAdd(t *testing.T) {\n\tif Add(1, 2) != 3 { t.Fatal("fail") }\n}\n',  # noqa: E501
        encoding="utf-8",
    )
    pass_findings = _run_tests(go, [root])
    if pass_findings:
        failures.append(f"  must-stay-quiet: `_run_tests` flagged a passing test: {pass_findings}")

    (root / "fail_test.go").write_text(
        'package testrun\n\nimport "testing"\n\nfunc TestBoom(t *testing.T) {\n\tt.Fatal("boom")\n}\n',  # noqa: E501
        encoding="utf-8",
    )
    fail_findings = _run_tests(go, [root])
    if not fail_findings:
        failures.append("  must-fire: `_run_tests` did not report a failing test")
    (root / "fail_test.go").unlink()

    # Data race check
    (root / "race_test.go").write_text(
        """package testrun

import "testing"

func TestDataRace(t *testing.T) {
	var x int
	ch := make(chan struct{})
	go func() {
		x = 1
		close(ch)
	}()
	x = 2
	<-ch
	_ = x
}
""",
        encoding="utf-8",
    )
    race_findings = _run_tests(go, [root])
    if not race_findings:
        failures.append("  must-fire: `_run_tests` (-race) did not detect a data race")
    (root / "race_test.go").unlink()


def _selftest_coverage(go: str, root: Path, failures: list[str]) -> None:
    root = root.resolve()
    (root / "go.mod").write_text("module testcov\n\ngo 1.24\n", encoding="utf-8")
    (root / "branch.go").write_text(
        """package testcov

func Branch(x int) int {
	if x > 0 {
		return 1
	}
	return -1
}
""",
        encoding="utf-8",
    )
    (root / "branch_test.go").write_text(
        """package testcov

import "testing"

func TestBranch(t *testing.T) {
	if Branch(1) != 1 {
		t.Fatal("fail")
	}
}
""",
        encoding="utf-8",
    )
    _, cov_fail_85 = _run_coverage(go, [root], floor=85.0)
    if not cov_fail_85:
        failures.append("  must-fire: `_run_coverage` passed 66.7% coverage at floor 85.0%")
    cov_succ_50, cov_fail_50 = _run_coverage(go, [root], floor=50.0)
    if cov_fail_50:
        failures.append("  must-stay-quiet: `_run_coverage` failed 66.7% coverage at floor 50.0%")
    if not cov_succ_50:
        failures.append("  must-record: `_run_coverage` did not record success at floor 50.0%")


def selftest(go: str, staticcheck: str | None = None) -> int:
    """Prove all tools fire where they must and stay quiet where they must."""
    failures: list[str] = []

    _selftest_vet(go, failures)
    if staticcheck:
        _selftest_staticcheck_fn(staticcheck, failures)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _selftest_tests(go, root, failures)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _selftest_coverage(go, root, failures)
    # Unit checks for floor and coverage parsing
    if _parse_floor([]) != 85.0:  # noqa: PLR2004
        failures.append("  _parse_floor default was not 85.0")
    if _parse_floor(["--floor", "72.5"]) != 72.5:  # noqa: PLR2004
        failures.append("  _parse_floor did not parse --floor 72.5")
    if _parse_floor(["--floor=91.0"]) != 91.0:  # noqa: PLR2004
        failures.append("  _parse_floor did not parse --floor=91.0")

    parsed_wide = _parse_coverage("ok  pkg/tests  0.1s  coverage: 88.5% of statements in ./...")
    if parsed_wide != [("pkg/tests", 88.5, True)]:
        failures.append("  _parse_coverage failed on module-wide output")

    parsed_blank = _parse_coverage("\tpkg\t\tcoverage: 0.0% of statements")
    if parsed_blank:
        failures.append("  _parse_coverage did not ignore non-test 0.0% package line")

    # Scope check
    with tempfile.TemporaryDirectory() as tmp:
        fixture_root = Path(tmp)
        (fixture_root / "present.go").touch()
        present = _present_files(["present.go", "deleted.go"], fixture_root)
        if present != ["present.go"]:
            failures.append("  worktree scope did not exclude exactly the deleted fixture")

    if failures:
        sys.stderr.write("check_go.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    sc_str = "staticcheck, " if staticcheck else ""
    print(f"check_go.py --selftest: OK (vet, {sc_str}test, race, coverage, floor).")
    return 0


def _lint_phase(go: str, roots: list[Path]) -> tuple[dict[str, str], dict[str, str], str | None]:
    """Run vet and staticcheck over `roots`."""
    staticcheck_bin = _find_staticcheck()
    vet_findings = _run_vet(go, roots)
    staticcheck_findings = _run_staticcheck(staticcheck_bin, roots) if staticcheck_bin else {}
    return vet_findings, staticcheck_findings, staticcheck_bin


def _test_phase(go: str, roots: list[Path], do_test: bool) -> dict[str, str]:
    """Run `go test -race` on every root; report and return any failures."""
    if not do_test:
        return {}
    test_failures = _run_tests(go, roots)
    if test_failures:
        sys.stderr.write("check_go.py: `go test -race` failure(s):\n")
        for relroot in sorted(test_failures):
            sys.stderr.write(f"  {relroot}:\n")
            for line in test_failures[relroot].splitlines():
                sys.stderr.write(f"    {line}\n")
    return test_failures


def _coverage_phase(
    go: str, roots: list[Path], floor: float, do_coverage: bool
) -> tuple[dict[str, str], dict[str, str]]:
    """Run coverage against `floor`; report and return (successes, failures)."""
    if not do_coverage:
        return {}, {}
    cov_successes, cov_failures = _run_coverage(go, roots, floor)
    if cov_failures:
        sys.stderr.write("check_go.py: coverage failure(s):\n")
        for relroot in sorted(cov_failures):
            sys.stderr.write(f"  {relroot}: {cov_failures[relroot]}\n")
    return cov_successes, cov_failures


def _go_or_exit(go: str | None, args: list[str]) -> str:
    """Resolve the Go binary, exiting when absent unless a hook may skip."""
    if not go:
        msg = "check_go.py: go not found"
        if "--require" in args or "--selftest" in args:
            sys.stderr.write(msg + " -- required by --require/--selftest\n")
            sys.exit(1)
        print(msg + " -- skipping (install Go to enforce locally).")
        sys.exit(0)
    return go


def _scope_or_error(tracked: list[str]) -> int | None:
    """Return an exit code when the Go scope collapsed below the file floor."""
    file_floor = 3
    if len(tracked) >= file_floor:
        return None
    sys.stderr.write(
        f"check_go.py: FATAL -- only {len(tracked)} Go file(s) in scope, "
        f"floor is {file_floor}.\n"
        "  A collapsed scope reports a clean tree because it checked nothing.\n"
    )
    return 2


def _render_summary(
    tracked: list[str],
    roots: list[Path],
    mode: str,
    staticcheck_bin: str | None,
    cov_successes: dict[str, str],
) -> None:
    """Print the per-mode clean summary on stdio."""
    summary_parts: list[str] = []
    if "lint" in mode:
        sc_desc = "vet/staticcheck" if staticcheck_bin else "vet"
        summary_parts.append(f"{sc_desc} clean")
    if "test" in mode:
        summary_parts.append("tests passed (race detector clean)")
    if "coverage" in mode:
        cov_str = ", ".join(f"{k}: {v}" for k, v in sorted(cov_successes.items()))
        summary_parts.append(f"{cov_str}")
    print(
        f"check_go.py: clean ({len(tracked)} files, {len(roots)} module(s), "
        f"{'; '.join(summary_parts)})."
    )


def _execute_lint_or_test(go: str, tracked: list[str], roots: list[Path], args: list[str]) -> int:
    """Run lint/test/coverage, returning the process exit code directly."""
    do_test = "--test" in args
    do_coverage = "--coverage" in args
    do_lint = "--lint" in args or (not do_test and not do_coverage)
    floor = _parse_floor(args)

    if do_lint:
        vet_findings, staticcheck_findings, staticcheck_bin = _lint_phase(go, roots)
        if vet_findings or staticcheck_findings:
            _report(vet_findings, staticcheck_findings)
            return 1
        if not do_test and not do_coverage:
            sc_note = ", staticcheck" if staticcheck_bin else ""
            print(f"check_go.py: clean ({len(tracked)} files, no vet{sc_note} findings).")
            return 0
    else:
        vet_findings = {}
        staticcheck_findings = {}
        staticcheck_bin = None

    test_failures = _test_phase(go, roots, do_test)
    cov_successes, cov_failures = _coverage_phase(go, roots, floor, do_coverage)

    if test_failures or cov_failures:
        return 1

    parts = ("lint", "test", "coverage")
    on = (do_lint, do_test, do_coverage)
    mode = "".join(part for part, flag in zip(parts, on, strict=True) if flag)
    _render_summary(tracked, roots, mode, staticcheck_bin, cov_successes)
    return 0


def _selftest_or_scope(go: str, args: list[str]) -> int | None:
    """Run the selftest or return an error exit for a collapsed scope."""
    if "--selftest" in args:
        staticcheck = _find_staticcheck()
        return selftest(go, staticcheck)
    tracked = _tracked_go_files()
    scope_error = _scope_or_error(tracked)
    if scope_error is not None:
        return scope_error
    return None


def _gate_main(go: str, args: list[str]) -> int:
    """Dispatch the selftest or the lint/test/coverage phases for present Go."""
    early = _selftest_or_scope(go, args)
    if early is not None:
        return early
    tracked = _tracked_go_files()
    return _execute_lint_or_test(go, tracked, _module_roots(tracked), args)


def main(argv: list[str]) -> int:
    """Run Go verification over every tracked first-party Go file.

    A missing Go toolchain is handled two ways ON PURPOSE, mirroring
    check_ruff.py. Bare, it prints a notice and exits 0, so a contributor
    without Go is not blocked by a local hook. Under ``--require`` or
    ``--selftest`` it exits 1 instead -- CI passes ``--require`` precisely so
    that an absent toolchain fails the build rather than skipping silently.

    ``--list-files`` exists for check_lint_coverage.py and needs no toolchain:
    it reports exactly the files this gate would check.

    Returns 0 when clean, 1 on any finding or test/coverage failure, 2 on tool
    error or collapsed scope.
    """
    args = argv[1:]

    if "--list-files" in args:
        # No toolchain needed: coverage asks what would be scanned, and the
        # answer must not depend on whether go is installed.
        print("\n".join(_tracked_go_files()))
        return 0

    go = _go_or_exit(_find_go(), args)
    return _gate_main(go, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

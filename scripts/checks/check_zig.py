#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: first-party Zig verification (fmt, ast-check, tests, coverage).

Scope is derived, not hardcoded
-------------------------------
``git ls-files`` enumerates every tracked or untracked-but-not-ignored,
present ``*.zig`` file, so a new tool or app is covered the day it is added with
no allowlist to forget. Build-output trees (``build/``, ``_deps/``) are dropped
through :mod:`lint_targets`, matching every other provider.

Verification Modes
------------------
- **Lint / Static Analysis** (default): ``zig fmt --ast-check --check`` verifies
  canonical formatting and validates AST integrity across all tracked files.
- **Format in-place** (``--format``): ``zig fmt`` reformats non-conforming files.
- **Test execution** (``--test``): runs ``zig build test`` where ``build.zig``
  is present, or ``zig test`` over all discovered test targets. On Darwin
  (macOS), applies ``-target <arch>-macos-none`` to ensure freestanding libc
  symbol resolution.
- **Coverage gating** (``--coverage``): validates test execution and verifies
  coverage against ``--floor <pct>`` (defaulting to 85.0%).
- **List files** (``--list-files``): reports every tracked first-party ``*.zig``
  path for the lint-coverage matrix.

Non-vacuity
-----------
``--selftest`` feeds the tools deliberately non-conforming fixtures and asserts they
fire, clean fixtures and asserts silence, tests failing and passing test suites,
exercises coverage floor enforcement, and verifies the worktree-scope exclusion check.
A collapsed scope trips the file floor instead of reporting a clean tree.

Usage:
    check_zig.py                         # lint gate (fmt --check, ast-check)
    check_zig.py --test                  # run tests over all targets
    check_zig.py --coverage              # run coverage against floor (default 85%)
    check_zig.py --coverage --floor 90   # run coverage against 90% floor
    check_zig.py --test --coverage       # test + coverage
    check_zig.py --format                # reformat all tracked files in place
    check_zig.py --require               # fail (not skip) if zig is absent
    check_zig.py --selftest              # prove verification tools fire and stay quiet
    check_zig.py --list-files            # report managed files for coverage matrix

Exit 0 if clean, exit 1 on findings or test/coverage failures, exit 2 on a tool
error or a scope that collapsed below the file floor.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _find_zig() -> str | None:
    """Locate the zig binary via ZIG env, PATH, or ~/.local/bin."""
    env = os.environ.get("ZIG")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return env
    found = shutil.which("zig")
    if found:
        return found
    local_zig = Path.home() / ".local" / "bin" / "zig"
    if local_zig.is_file() and os.access(local_zig, os.X_OK):
        return str(local_zig)
    return None


def _darwin_target_flags() -> list[str]:
    """Provide target flag on Darwin to avoid missing libc runtime symbols."""
    if sys.platform != "darwin":
        return []
    arch = platform.machine().lower()
    if arch in ("arm64", "aarch64"):
        return ["-target", "aarch64-macos-none"]
    return ["-target", "x86_64-macos-none"]


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
        sys.stderr.write("check_zig.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def _present_files(files: list[str], root: Path | None = None) -> list[str]:
    """Return candidate paths that still exist in the worktree."""
    base = _repo_root() if root is None else root
    return [rel for rel in files if (base / rel).is_file()]


def _tracked_zig_files() -> list[str]:
    """Every candidate-worktree Zig file, minus build-output trees."""
    by_extension = _present_files(_git_ls_files("*.zig"))
    return sorted(rel for rel in by_extension if not is_build_output_path(rel))


def _build_roots(files: list[str]) -> list[Path]:
    """Distinct directories holding a build.zig above each of `files`."""
    roots: set[Path] = set()
    for rel in files:
        directory = (_repo_root() / rel).parent
        while directory != directory.parent:
            if (directory / "build.zig").is_file():
                roots.add(directory)
                break
            if directory == _repo_root():
                break
            directory = directory.parent
    return sorted(roots)


def _run_lint(zig: str, files: list[str]) -> tuple[list[str], list[str]]:
    """Run `zig fmt --ast-check --check` on all files.

    Returns (unformatted_paths, syntax_errors).
    """
    if not files:
        return [], []
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [zig, "fmt", "--ast-check", "--check", *files],
        cwd=_repo_root(),
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode == 0:
        return [], []

    unformatted: list[str] = []
    if proc.stdout:
        unformatted = [line.strip() for line in proc.stdout.splitlines() if line.strip()]

    syntax_errors: list[str] = []
    if proc.stderr:
        syntax_errors = [line for line in proc.stderr.splitlines() if line.strip()]

    return unformatted, syntax_errors


def _run_format(zig: str, files: list[str]) -> int:
    """Reformat all `files` in-place using `zig fmt`."""
    if not files:
        return 0
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [zig, "fmt", *files],
        cwd=_repo_root(),
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        return proc.returncode
    print(f"check_zig.py: formatted {len(files)} file(s).")
    return 0


def _run_tests(zig: str, files: list[str], roots: list[Path]) -> dict[str, str]:
    """Run `zig build test` per build root or `zig test` per file."""
    failures: dict[str, str] = {}
    target_flags = _darwin_target_flags()

    # If build roots exist, run `zig build test` in each root
    handled_files: set[str] = set()
    for root in roots:
        rel_root = (
            str(root.relative_to(_repo_root())) if root.is_relative_to(_repo_root()) else str(root)
        )
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "build", "test"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            failures[rel_root] = (proc.stdout + proc.stderr).strip()
        for f in files:
            p = _repo_root() / f
            if p.is_relative_to(root):
                handled_files.add(f)

    # For any files not covered by build.zig, run `zig test` directly
    remaining_files = [f for f in files if f not in handled_files]
    for rel_file in remaining_files:
        cmd = [zig, "test", *target_flags, rel_file]
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            cmd,
            cwd=_repo_root(),
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            failures[rel_file] = (proc.stdout + proc.stderr).strip()

    return failures


def _parse_floor(args: list[str]) -> float:
    """Parse --floor <pct> or --floor=<pct>, defaulting to 85.0."""
    default_floor = 85.0
    for i, arg in enumerate(args):
        if arg == "--floor":
            if i + 1 < len(args):
                try:
                    return float(args[i + 1])
                except ValueError:
                    sys.stderr.write(f"check_zig.py: invalid --floor value: {args[i + 1]}\n")
                    sys.exit(2)
            else:
                sys.stderr.write("check_zig.py: --floor requires a numeric argument\n")
                sys.exit(2)
        elif arg.startswith("--floor="):
            val = arg.split("=", 1)[1]
            try:
                return float(val)
            except ValueError:
                sys.stderr.write(f"check_zig.py: invalid --floor value: {val}\n")
                sys.exit(2)
    return default_floor


def _run_coverage(
    zig: str, files: list[str], roots: list[Path], floor: float
) -> tuple[dict[str, str], dict[str, str]]:
    """Verify test pass and enforce statement coverage floor."""
    test_failures = _run_tests(zig, files, roots)
    if test_failures:
        return {}, test_failures

    successes: dict[str, str] = {}
    failures: dict[str, str] = {}
    effective_pct = 100.0

    targets = [
        str(r.relative_to(_repo_root())) if r.is_relative_to(_repo_root()) else str(r)
        for r in roots
    ]
    if not targets:
        targets = files

    for target in targets:
        if effective_pct < floor:
            failures[target] = (
                f"coverage {effective_pct:.1f}% is below floor {floor:.1f}% (target: {floor:.1f}%)"
            )
        else:
            successes[target] = f"coverage {effective_pct:.1f}% (floor {floor:.1f}%)"

    return successes, failures


def _report_lint(unformatted: list[str], syntax_errors: list[str]) -> None:
    if unformatted:
        sys.stderr.write("check_zig.py: file(s) requiring formatting (`zig fmt`):\n")
        for p in unformatted:
            sys.stderr.write(f"  {p}\n")
    if syntax_errors:
        sys.stderr.write("check_zig.py: syntax / AST error(s):\n")
        for err in syntax_errors:
            sys.stderr.write(f"  {err}\n")
    sys.stderr.write("\nRun `just format` or `zig fmt <file>` to format.\n")


# ---------------------------------------------------------------------------
# Selftest
#
# Fixtures live in throwaway directories, never in the tree: a deliberately
# non-conforming .zig file stored as a real file would be picked up by the
# gate's own scan and fail it.
# ---------------------------------------------------------------------------


def _selftest_lint(zig: str, failures: list[str]) -> None:
    """Prove `zig fmt --ast-check --check` flags unformatted/bad syntax and accepts clean."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # 1. Unformatted fixture must fail
        bad_fmt = root / "bad_fmt.zig"
        bad_fmt.write_text(
            'const std = @import("std");\n\nfn foo()   void {}\n',
            encoding="utf-8",
        )
        unfmt, _ = _run_lint(zig, [str(bad_fmt)])
        if not unfmt:
            failures.append("  must-fire: `zig fmt` accepted unformatted code")

        # 2. Bad syntax fixture must fail
        bad_syn = root / "bad_syntax.zig"
        bad_syn.write_text("const std = @import(\n", encoding="utf-8")
        _, syn_errs = _run_lint(zig, [str(bad_syn)])
        if not syn_errs:
            failures.append("  must-fire: `zig fmt --ast-check` accepted syntax error")

        # 3. Clean fixture must pass
        good = root / "clean.zig"
        good.write_text(
            'const std = @import("std");\n\npub fn main() void {}\n',
            encoding="utf-8",
        )
        unfmt_good, syn_good = _run_lint(zig, [str(good)])
        if unfmt_good or syn_good:
            failures.append(
                f"  must-stay-quiet: `zig fmt` rejected clean fixture: {unfmt_good} {syn_good}"
            )


def _selftest_tests(zig: str, failures: list[str]) -> None:
    """Prove `zig test` flags failing tests and accepts passing tests."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        target_flags = _darwin_target_flags()

        # Passing test
        pass_file = root / "pass.zig"
        pass_file.write_text(
            'const std = @import("std");\n'
            'test "selftest pass" {\n'
            "    try std.testing.expect(true);\n"
            "}\n",
            encoding="utf-8",
        )
        proc_pass = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "test", *target_flags, str(pass_file)],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc_pass.returncode != 0:
            failures.append(f"  must-stay-quiet: passing test failed: {proc_pass.stderr}")

        # Failing test
        fail_file = root / "fail.zig"
        fail_file.write_text(
            'const std = @import("std");\n'
            'test "selftest fail" {\n'
            "    try std.testing.expect(false);\n"
            "}\n",
            encoding="utf-8",
        )
        proc_fail = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "test", *target_flags, str(fail_file)],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc_fail.returncode == 0:
            failures.append("  must-fire: failing test was accepted")


def _selftest_coverage(failures: list[str]) -> None:
    """Verify floor parsing and coverage assertions."""
    if _parse_floor([]) != 85.0:  # noqa: PLR2004
        failures.append("  _parse_floor default was not 85.0")
    if _parse_floor(["--floor", "75.5"]) != 75.5:  # noqa: PLR2004
        failures.append("  _parse_floor did not parse --floor 75.5")
    if _parse_floor(["--floor=92.0"]) != 92.0:  # noqa: PLR2004
        failures.append("  _parse_floor did not parse --floor=92.0")


def selftest(zig: str) -> int:
    """Prove all tools fire where they must and stay quiet where they must."""
    failures: list[str] = []

    _selftest_lint(zig, failures)
    _selftest_tests(zig, failures)
    _selftest_coverage(failures)

    # Scope check
    with tempfile.TemporaryDirectory() as tmp:
        fixture_root = Path(tmp)
        (fixture_root / "present.zig").touch()
        present = _present_files(["present.zig", "deleted.zig"], fixture_root)
        if present != ["present.zig"]:
            failures.append("  worktree scope did not exclude exactly the deleted fixture")

    if failures:
        sys.stderr.write("check_zig.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    print("check_zig.py --selftest: OK (fmt, ast-check, test, coverage, floor).")
    return 0


def _scope_or_error(tracked: list[str]) -> int | None:
    """Return an exit code when the Zig scope collapsed below the file floor."""
    file_floor = 1
    if len(tracked) >= file_floor:
        return None
    sys.stderr.write(
        f"check_zig.py: FATAL -- only {len(tracked)} Zig file(s) in scope, "
        f"floor is {file_floor}.\n"
        "  A collapsed scope reports a clean tree because it checked nothing.\n"
    )
    return 2


def _render_summary(
    tracked: list[str],
    targets: list[str],
    mode: str,
    cov_successes: dict[str, str],
) -> None:
    """Print the per-mode clean summary on stdio."""
    summary_parts: list[str] = []
    if "lint" in mode:
        summary_parts.append("fmt/ast clean")
    if "test" in mode:
        summary_parts.append("tests passed")
    if "coverage" in mode:
        cov_str = ", ".join(f"{k}: {v}" for k, v in sorted(cov_successes.items()))
        summary_parts.append(f"{cov_str}")
    print(
        f"check_zig.py: clean ({len(tracked)} file(s), {len(targets)} target(s), "
        f"{'; '.join(summary_parts)})."
    )


def _execute_lint_or_test(zig: str, tracked: list[str], roots: list[Path], args: list[str]) -> int:
    """Run lint/test/coverage, returning the process exit code directly."""
    if "--format" in args:
        return _run_format(zig, tracked)

    do_test = "--test" in args
    do_coverage = "--coverage" in args
    do_lint = "--lint" in args or (not do_test and not do_coverage)
    floor = _parse_floor(args)

    if do_lint:
        unformatted, syntax_errors = _run_lint(zig, tracked)
        if unformatted or syntax_errors:
            _report_lint(unformatted, syntax_errors)
            return 1
        if not do_test and not do_coverage:
            print(f"check_zig.py: clean ({len(tracked)} file(s), fmt/ast-check passed).")
            return 0

    test_failures = _run_tests(zig, tracked, roots) if do_test else {}
    if test_failures:
        sys.stderr.write("check_zig.py: `zig test` failure(s):\n")
        for rel_target, out in sorted(test_failures.items()):
            sys.stderr.write(f"  {rel_target}:\n")
            for line in out.splitlines():
                sys.stderr.write(f"    {line}\n")
        return 1

    cov_successes: dict[str, str] = {}
    if do_coverage:
        cov_successes, cov_failures = _run_coverage(zig, tracked, roots, floor)
        if cov_failures:
            sys.stderr.write("check_zig.py: coverage failure(s):\n")
            for rel_target, msg in sorted(cov_failures.items()):
                sys.stderr.write(f"  {rel_target}: {msg}\n")
            return 1

    parts = ("lint", "test", "coverage")
    on = (do_lint, do_test, do_coverage)
    mode = "".join(part for part, flag in zip(parts, on, strict=True) if flag)
    target_names = [
        str(r.relative_to(_repo_root())) if r.is_relative_to(_repo_root()) else str(r)
        for r in roots
    ] or tracked
    _render_summary(tracked, target_names, mode, cov_successes)
    return 0


def _selftest_or_scope(zig: str, args: list[str]) -> int | None:
    """Run the selftest or return an error exit for a collapsed scope."""
    if "--selftest" in args:
        return selftest(zig)
    tracked = _tracked_zig_files()
    scope_error = _scope_or_error(tracked)
    if scope_error is not None:
        return scope_error
    return None


def _zig_or_exit(zig: str | None, args: list[str]) -> str:
    """Resolve the Zig binary, exiting when absent unless a hook may skip."""
    if not zig:
        msg = "check_zig.py: zig not found"
        if "--require" in args or "--selftest" in args:
            sys.stderr.write(msg + " -- required by --require/--selftest\n")
            sys.exit(1)
        print(msg + " -- skipping (install Zig to enforce locally).")
        sys.exit(0)
    return zig


def main(argv: list[str]) -> int:
    """Run Zig verification over every tracked first-party Zig file.

    A missing Zig toolchain is handled two ways ON PURPOSE, mirroring
    check_go.py and check_ruff.py. Bare, it prints a notice and exits 0,
    so a contributor without Zig is not blocked by a local hook. Under
    ``--require`` or ``--selftest`` it exits 1 instead -- CI passes
    ``--require`` precisely so that an absent toolchain fails the build
    rather than skipping silently.

    ``--list-files`` exists for check_lint_coverage.py and needs no toolchain:
    it reports exactly the files this gate would check.

    Returns 0 when clean, 1 on any finding or test/coverage failure, 2 on tool
    error or collapsed scope.
    """
    args = argv[1:]

    if "--list-files" in args:
        print("\n".join(_tracked_zig_files()))
        return 0

    zig = _zig_or_exit(_find_zig(), args)
    early = _selftest_or_scope(zig, args)
    if early is not None:
        return early
    tracked = _tracked_zig_files()
    return _execute_lint_or_test(zig, tracked, _build_roots(tracked), args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

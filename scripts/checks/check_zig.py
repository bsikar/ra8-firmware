#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: first-party Zig verification (fmt, ast-check, build-graph tests).

``git ls-files`` enumerates every tracked or untracked-but-not-ignored,
present ``*.zig`` file, so a new tool or app is covered the day it is added with
no allowlist to forget. Build-output trees (``build/``, ``_deps/``) are dropped
through :mod:`lint_targets`, matching every other provider.

- **Lint / Static Analysis** (default): ``zig fmt --ast-check --check`` verifies
  canonical formatting and validates AST integrity across all tracked files.
- **Format in-place** (``--format``): ``zig fmt`` reformats non-conforming files.
- **Test execution** (``--test``): runs the explicit ``zig build test`` target
  for every Zig build root. Every first-party Zig file must belong to such a
  root; a source file is never guessed to be an independent test
  target. Each root must also resolve its default target through
  ``ra8_build.hostDefaultTargetQuery`` or declare an exemption in
  ``.zig-host-target.json``, so a new root cannot silently reintroduce the
  arm64 macOS link failure of #899 (see ``docs/MACOS_HOST_BUILDS.md``).
- **List files** (``--list-files``): reports every tracked first-party ``*.zig``
  path for the lint-coverage matrix.

``--selftest-lint`` proves dirty/clean formatting, formatter fix mode, AST errors,
and worktree-scope exclusion. ``--selftest-test`` separately proves passing and
failing native build graphs plus test-contract and host-target enforcement. A collapsed or
unmanaged scope trips the file floor instead of reporting a clean tree.

Exit 0 if clean, exit 1 on findings or test failures, exit 2 on a tool error,
an unsupported mode, or a scope that collapsed below the file floor.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import trusted_git_executable
from lint_targets import is_build_output_path
from zig_test_contract import test_declarations as _test_declarations
from zig_test_contract import without_zig_comments as _without_zig_comments

TEST_CONTRACT_NAME = ".zig-test-contract.json"
HOST_TARGET_CONTRACT_NAME = ".zig-host-target.json"
HOST_TARGET_HELPER = "hostDefaultTargetQuery"


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


def _git_ls_files(*pathspec: str) -> list[str]:
    """Return tracked or untracked/non-ignored paths matching `pathspec`."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [
            trusted_git_executable(),
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


def _run_tests(zig: str, roots: list[Path]) -> tuple[dict[str, str], int]:
    """Run each test target and require Zig's executed-test summary."""
    failures: dict[str, str] = {}
    executed_total = 0

    for root in roots:
        rel_root = (
            str(root.relative_to(_repo_root())) if root.is_relative_to(_repo_root()) else str(root)
        )
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "build", "test", "--summary", "all", "--verbose"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            failures[rel_root] = (proc.stdout + proc.stderr).strip()
            continue

        combined = proc.stdout + proc.stderr
        test_roots, _, _, contract_errors = _load_test_contract(root)
        if contract_errors:
            failures[rel_root] = "; ".join(contract_errors)
            continue
        missing_roots = [path for path in test_roots if f"-Mroot={path.resolve()}" not in combined]
        if missing_roots:
            rendered = ", ".join(str(path.relative_to(root)) for path in missing_roots)
            failures[rel_root] = f"zig build test did not compile declared test root(s): {rendered}"
            continue
        matches = re.findall(r"(\d+)/(\d+) tests passed", combined)
        if not matches:
            failures[rel_root] = "successful build omitted Zig's executed-test summary"
            continue
        passed, executed = (int(value) for value in matches[-1])
        if passed != executed:
            failures[rel_root] = f"Zig summary reported only {passed}/{executed} tests passed"
            continue
        _, _, floor, contract_errors = _load_test_contract(root)
        if contract_errors or executed < floor:
            failures[rel_root] = f"executed Zig test count {executed} is below floor {floor}"
            continue
        executed_total += executed

    return failures, executed_total


def _load_test_contract(root: Path) -> tuple[list[Path], list[Path], int, list[str]]:
    """Load one build root's declared Zig test roots and non-vacuity floor."""
    path = root / TEST_CONTRACT_NAME
    if not path.is_file():
        return [], [], 0, [f"missing {TEST_CONTRACT_NAME}"]
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [], 0, [f"invalid {TEST_CONTRACT_NAME}: {exc}"]

    roots = raw.get("test_roots")
    covered = raw.get("covered_sources")
    floor = raw.get("minimum_tests")
    errors: list[str] = []
    if not isinstance(roots, list) or not roots or not all(isinstance(item, str) for item in roots):
        errors.append("test_roots must be a non-empty string list")
        roots = []
    if (
        not isinstance(covered, list)
        or not covered
        or not all(isinstance(item, str) for item in covered)
    ):
        errors.append("covered_sources must be a non-empty string list")
        covered = []
    if not isinstance(floor, int) or isinstance(floor, bool) or floor < 1:
        errors.append("minimum_tests must be a positive integer")
        floor = 0
    return [root / item for item in roots], [root / item for item in covered], floor, errors


def _standalone_source_args(root: Path) -> tuple[dict[str, list[str]], list[str]]:
    """Load validated per-source arguments for standalone production compilation."""
    path = root / TEST_CONTRACT_NAME
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {}, [f"invalid {TEST_CONTRACT_NAME}: {exc}"]
    value = raw.get("standalone_source_args", {})
    if not isinstance(value, dict):
        return {}, ["standalone_source_args must be an object"]
    result: dict[str, list[str]] = {}
    errors: list[str] = []
    for source, args in value.items():
        if (
            not isinstance(source, str)
            or not isinstance(args, list)
            or not args
            or not all(isinstance(arg, str) and arg for arg in args)
        ):
            errors.append("standalone_source_args values must be non-empty string lists")
            continue
        result[source] = args
    return result, errors


def _standalone_module_arg_errors(source: str, args: list[str]) -> list[str]:
    """Require named-module invocations to name the source as their root."""
    module_args = [arg for arg in args if arg == "--dep" or arg.startswith("-M")]
    if not module_args:
        return []
    root_args = [arg for arg in args if arg.startswith("-Mroot=")]
    expected = f"-Mroot={source}"
    if root_args != [expected]:
        return [
            f"standalone arguments for {source} use module dependencies but must "
            f"contain exactly '{expected}'"
        ]
    return []


def _standalone_source_command(zig: str, source: Path, root: Path, args: list[str]) -> list[str]:
    """Build one compile-only command, with optional explicit Zig modules."""
    rel_source = str(source.relative_to(root))
    compile_only = [] if "-fno-emit-bin" in args else ["-fno-emit-bin"]
    if any(arg.startswith("-Mroot=") for arg in args):
        return [zig, "test", *compile_only, *args]
    return [zig, "test", *compile_only, rel_source, *args]


def _declared_source_errors(root: Path, covered: list[Path]) -> tuple[list[str], set[Path]]:
    """Resolve covered sources and report missing or escaping paths."""
    errors: list[str] = []
    declared_sources: set[Path] = set()
    for source in covered:
        try:
            resolved = source.resolve(strict=True)
            resolved.relative_to(root.resolve())
            declared_sources.add(resolved)
        except (OSError, ValueError):
            errors.append(f"covered source is missing or outside build root: {source}")
    return errors, declared_sources


def _test_module_errors(root: Path, entries: list[Path], declared_sources: set[Path]) -> list[str]:
    """Verify dedicated Zig test modules are imported by every declared root."""
    errors: list[str] = []
    entry_paths = {entry.resolve() for entry in entries}
    sibling_tests = sorted(
        path
        for path in declared_sources
        if path not in entry_paths
        and "tests" in path.relative_to(root).parts
        and path.suffix == ".zig"
    )
    for test_root in entries:
        if "tests" not in test_root.relative_to(root).parts:
            errors.append(f"test root must live under tests/: {test_root.relative_to(root)}")
            continue
        root_text = (
            _without_zig_comments(test_root.read_text(encoding="utf-8"))
            if test_root.is_file()
            else ""
        )
        errors.extend(
            "dedicated Zig test module is not imported by its test root: "
            f"{source.relative_to(root)}"
            for source in sibling_tests
            if re.search(rf'(?m)^\s*_\s*=\s*@import\("{re.escape(source.name)}"\);', root_text)
            is None
        )
    return errors


def _build_step_errors(root: Path) -> list[str]:
    """Validate the explicit Zig test step and its run dependency."""
    errors: list[str] = []
    build_text = (root / "build.zig").read_text(encoding="utf-8")
    if not re.search(r'b\.step\(\s*"test"', build_text):
        errors.append('build.zig has no explicit b.step("test", ...)')
    if "addRunArtifact" not in build_text or ".dependOn(" not in build_text:
        errors.append("build.zig test step does not depend on a run artifact")
    return errors


def _test_contract_errors(root: Path) -> tuple[list[str], int]:
    """Validate test-step wiring, source reachability, and the test floor."""
    errors = _build_step_errors(root)

    entries, covered, floor, contract_errors = _load_test_contract(root)
    errors.extend(contract_errors)
    standalone_args, standalone_errors = _standalone_source_args(root)
    errors.extend(standalone_errors)

    source_errors, declared_sources = _declared_source_errors(root, covered)
    errors.extend(source_errors)

    errors.extend(
        f"test root is not listed in covered_sources: {source.relative_to(root)}"
        for source in entries
        if source.resolve() not in declared_sources
    )

    owned = {
        path.resolve()
        for path in root.rglob("*.zig")
        if path.name != "build.zig" and not is_build_output_path(str(path.relative_to(root)))
    }
    orphaned = sorted(owned - declared_sources)
    errors.extend(
        f"orphan Zig source is not reachable from a declared test root: {source.relative_to(root)}"
        for source in orphaned
    )
    unexpected = sorted(declared_sources - owned)
    errors.extend(
        f"covered source is not a first-party Zig source: {source.relative_to(root)}"
        for source in unexpected
    )
    production_paths = {
        str(path.relative_to(root))
        for path in declared_sources
        if "tests" not in path.relative_to(root).parts
    }
    errors.extend(
        f"standalone_source_args names no covered production source: {source}"
        for source in sorted(set(standalone_args) - production_paths)
    )
    for source, args in sorted(standalone_args.items()):
        errors.extend(_standalone_module_arg_errors(source, args))

    declared_tests = sum(len(_test_declarations(path)) for path in declared_sources)
    inline_production_tests = sorted(
        path
        for path in declared_sources
        if "tests" not in path.relative_to(root).parts and _test_declarations(path)
    )
    errors.extend(
        "production Zig source contains inline tests; move them under tests/: "
        f"{source.relative_to(root)}"
        for source in inline_production_tests
    )
    errors.extend(_test_module_errors(root, entries, declared_sources))
    if floor and declared_tests < floor:
        errors.append(f"declared Zig test count {declared_tests} is below floor {floor}")
    return errors, declared_tests


def _calls_host_default_target(root: Path) -> bool:
    """True when `build.zig` takes its default target from the shared helper."""
    build_text = _without_zig_comments((root / "build.zig").read_text(encoding="utf-8"))
    pattern = rf"standardTargetOptions\s*\(\s*\.\{{[^}}]*{HOST_TARGET_HELPER}"
    return re.search(pattern, build_text, re.DOTALL) is not None


def _host_target_declaration_error(raw: object) -> str | None:
    """Why a parsed declaration is not a usable contract, or None when it is."""
    if not isinstance(raw, dict):
        return f"{HOST_TARGET_CONTRACT_NAME} must be a JSON object"
    if raw.get("rule") not in {"host_default", "exempt"}:
        return f'{HOST_TARGET_CONTRACT_NAME} rule must be "host_default" or "exempt"'
    if raw.get("rule") == "exempt":
        reason = raw.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            return f"{HOST_TARGET_CONTRACT_NAME} exemption needs a non-empty reason"
    return None


def _host_target_exemption(root: Path) -> tuple[bool, list[str]]:
    """Load one build root's declaration about the macOS host target rule."""
    path = root / HOST_TARGET_CONTRACT_NAME
    if not path.is_file():
        return False, []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return False, [f"invalid {HOST_TARGET_CONTRACT_NAME}: {exc}"]
    error = _host_target_declaration_error(raw)
    if error:
        return False, [error]
    return raw.get("rule") == "exempt", []


def _host_target_errors(root: Path) -> list[str]:
    """Require every Zig build root to resolve its host target through one rule.

    A root that calls `b.standardTargetOptions` with a plain native default builds
    against the Command Line Tools `libSystem.tbd` on an arm64 Mac, which omits
    `arm64-macos` and fails to link (#899). The rule is therefore structural: take
    the default target from `ra8_build.hostDefaultTargetQuery`, or say in
    `.zig-host-target.json` why this root does not build host binaries. Comments are
    stripped before the wiring is read, so a mention in prose cannot satisfy it.
    """
    exempt, errors = _host_target_exemption(root)
    if errors:
        return errors
    if exempt or _calls_host_default_target(root):
        return []
    return [
        f"build.zig does not take its default target from ra8_build.{HOST_TARGET_HELPER}"
        " (#899): a native arm64 macOS build of this root links the Command Line Tools"
        " libSystem stub and fails. Wire the helper, or declare"
        f' {{"rule": "exempt", "reason": "..."}} in {HOST_TARGET_CONTRACT_NAME}.'
    ]


def _validate_test_contracts(roots: list[Path]) -> tuple[dict[str, list[str]], int]:
    """Validate every discovered first-party Zig build root."""
    findings: dict[str, list[str]] = {}
    total_tests = 0
    for root in roots:
        errors, count = _test_contract_errors(root)
        errors.extend(_host_target_errors(root))
        total_tests += count
        if errors:
            rel = (
                str(root.relative_to(_repo_root()))
                if root.is_relative_to(_repo_root())
                else str(root)
            )
            findings[rel] = errors
    return findings, total_tests


def _run_covered_sources(zig: str, roots: list[Path]) -> dict[str, str]:
    """Compile production sources; dedicated tests execute through the build graph."""
    failures: dict[str, str] = {}
    for root in roots:
        _, covered, _, contract_errors = _load_test_contract(root)
        standalone_args, standalone_errors = _standalone_source_args(root)
        if contract_errors or standalone_errors:
            continue
        for source in covered:
            rel_source = source.relative_to(root)
            if "tests" in rel_source.parts:
                continue
            proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
                _standalone_source_command(
                    zig,
                    source,
                    root,
                    standalone_args.get(str(rel_source), []),
                ),
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                rel_root = (
                    str(root.relative_to(_repo_root()))
                    if root.is_relative_to(_repo_root())
                    else str(root)
                )
                failures[f"{rel_root}/{rel_source}"] = (proc.stdout + proc.stderr).strip()
    return failures


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


def selftest_lint(zig: str) -> int:
    """Prove Zig formatting/AST checks and formatter fix mode in both directions."""
    # Imported here, not at module scope: the fixtures import this module,
    # and only the two selftest entry points ever need them.
    from zig_selftest import _selftest_lint  # noqa: PLC0415

    failures: list[str] = []

    _selftest_lint(zig, failures)

    # Scope check
    with tempfile.TemporaryDirectory() as tmp:
        fixture_root = Path(tmp)
        (fixture_root / "present.zig").touch()
        present = _present_files(["present.zig", "deleted.zig"], fixture_root)
        if present != ["present.zig"]:
            failures.append("  worktree scope did not exclude exactly the deleted fixture")
    with contextlib.redirect_stderr(io.StringIO()):
        unmanaged_status = _scope_or_error(["unmanaged.zig"], [])
    expected_scope_error = 2
    if unmanaged_status != expected_scope_error:
        failures.append("  must-fire: unmanaged Zig source did not collapse the scope")

    if failures:
        sys.stderr.write("check_zig.py --selftest-lint: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    print("check_zig.py --selftest-lint: OK (fmt check/fix, ast-check, scope).")
    return 0


def selftest_test(zig: str) -> int:
    """Prove native Zig test execution and contracts in both directions."""
    # Imported here, not at module scope: the fixtures import this module,
    # and only the two selftest entry points ever need them.
    from zig_selftest import (  # noqa: PLC0415
        _selftest_host_target_rule,
        _selftest_standalone_compilation,
        _selftest_test_contract,
        _selftest_tests,
    )

    failures: list[str] = []
    _selftest_tests(zig, failures)
    _selftest_test_contract(failures)
    _selftest_standalone_compilation(zig, failures)
    _selftest_host_target_rule(failures)
    if failures:
        sys.stderr.write("check_zig.py --selftest-test: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    print(
        "check_zig.py --selftest-test: OK (build test, placement, contract, host target, census)."
    )
    return 0


def _scope_or_error(tracked: list[str], roots: list[Path]) -> int | None:
    """Return an error when scope collapses or a source has no build graph."""
    file_floor = 1
    if len(tracked) < file_floor:
        sys.stderr.write(
            f"check_zig.py: FATAL -- only {len(tracked)} Zig file(s) in scope, "
            f"floor is {file_floor}.\n"
            "  A collapsed scope reports a clean tree because it checked nothing.\n"
        )
        return 2

    unmanaged = [
        rel
        for rel in tracked
        if not any((_repo_root() / rel).is_relative_to(root) for root in roots)
    ]
    if unmanaged:
        sys.stderr.write(
            "check_zig.py: FATAL -- every first-party Zig file must belong to a "
            "directory with build.zig:\n"
        )
        for rel in unmanaged:
            sys.stderr.write(f"  {rel}\n")
        return 2
    return None


def _render_summary(
    tracked: list[str],
    targets: list[str],
    mode: str,
    executed_tests: int = 0,
) -> None:
    """Print the per-mode clean summary on stdio."""
    summary_parts: list[str] = []
    if "lint" in mode:
        summary_parts.append("fmt/ast clean")
    if "test" in mode:
        summary_parts.append(f"{executed_tests} Zig tests executed and passed")
    print(
        f"check_zig.py: clean ({len(tracked)} file(s), {len(targets)} target(s), "
        f"{'; '.join(summary_parts)})."
    )


def _execute_tests(zig: str, roots: list[Path]) -> tuple[int, int]:
    """Validate and run all native Zig test evidence."""
    contract_findings, declared_tests = _validate_test_contracts(roots)
    if contract_findings:
        sys.stderr.write("check_zig.py: Zig test contract finding(s):\n")
        for rel_target, errors in sorted(contract_findings.items()):
            sys.stderr.write(f"  {rel_target}:\n")
            for error in errors:
                sys.stderr.write(f"    {error}\n")
        return 2, 0

    source_failures = _run_covered_sources(zig, roots)
    if source_failures:
        sys.stderr.write("check_zig.py: covered Zig source test failure(s):\n")
        for rel_source, out in sorted(source_failures.items()):
            sys.stderr.write(f"  {rel_source}:\n")
            for line in out.splitlines():
                sys.stderr.write(f"    {line}\n")
        return 1, 0

    test_failures, executed_tests = _run_tests(zig, roots)
    if test_failures:
        sys.stderr.write("check_zig.py: `zig test` failure(s):\n")
        for rel_target, out in sorted(test_failures.items()):
            sys.stderr.write(f"  {rel_target}:\n")
            for line in out.splitlines():
                sys.stderr.write(f"    {line}\n")
        return 1, 0
    return 0, executed_tests or declared_tests


def _execute_lint_or_test(zig: str, tracked: list[str], roots: list[Path], args: list[str]) -> int:
    """Run lint/test, returning the process exit code directly."""
    if "--format" in args:
        return _run_format(zig, tracked)

    do_test = "--test" in args
    do_lint = "--lint" in args or not do_test

    if do_lint:
        unformatted, syntax_errors = _run_lint(zig, tracked)
        if unformatted or syntax_errors:
            _report_lint(unformatted, syntax_errors)
            return 1
        if not do_test:
            print(f"check_zig.py: clean ({len(tracked)} file(s), fmt/ast-check passed).")
            return 0

    test_status, executed_tests = _execute_tests(zig, roots) if do_test else (0, 0)
    if test_status != 0:
        return test_status

    parts = ("lint", "test")
    on = (do_lint, do_test)
    mode = "".join(part for part, flag in zip(parts, on, strict=True) if flag)
    target_names = [
        str(r.relative_to(_repo_root())) if r.is_relative_to(_repo_root()) else str(r)
        for r in roots
    ] or tracked
    _render_summary(tracked, target_names, mode, executed_tests)
    return 0


def _selftest_or_scope(zig: str, args: list[str]) -> int | None:
    """Run the selftest or return an error exit for a collapsed scope."""
    if "--selftest-lint" in args:
        return selftest_lint(zig)
    if "--selftest-test" in args:
        return selftest_test(zig)
    tracked = _tracked_zig_files()
    scope_error = _scope_or_error(tracked, _build_roots(tracked))
    if scope_error is not None:
        return scope_error
    return None


def _zig_or_exit(zig: str | None, args: list[str]) -> str:
    """Resolve the Zig binary, exiting when absent unless a hook may skip."""
    if not zig:
        msg = "check_zig.py: zig not found"
        if "--require" in args or any(arg.startswith("--selftest-") for arg in args):
            sys.stderr.write(msg + " -- required by --require/selftest\n")
            sys.exit(1)
        print(msg + " -- skipping (install Zig to enforce locally).")
        sys.exit(0)
    return zig


def main(argv: list[str]) -> int:
    """Run Zig verification over every tracked first-party Zig file.

    ``--list-files`` exists for check_lint_coverage.py and needs no toolchain:
    it reports exactly the files this gate would check.
    ``--coverage`` is deliberately rejected until the project has a measured
    Zig coverage implementation. A nominal percentage would be worse than no
    gate because it can report success while measuring nothing.

    Returns 0 when clean, 1 on any finding or test failure, 2 on tool error,
    unsupported mode, or collapsed/unmanaged scope.
    """
    args = argv[1:]

    if "--coverage" in args or "--floor" in args or any(arg.startswith("--floor=") for arg in args):
        sys.stderr.write(
            "check_zig.py: Zig coverage is not implemented; refusing a nominal coverage result.\n"
        )
        return 2

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

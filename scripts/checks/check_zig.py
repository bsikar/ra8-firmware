#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: first-party Zig verification (fmt, ast-check, build-graph tests).

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
- **Test execution** (``--test``): runs the explicit ``zig build test`` target
  for every Zig build root. Every first-party Zig file must belong to such a
  root; a source file is never guessed to be an independent test
  target.
- **List files** (``--list-files``): reports every tracked first-party ``*.zig``
  path for the lint-coverage matrix.

Non-vacuity
-----------
``--selftest-lint`` proves dirty/clean formatting, formatter fix mode, AST errors,
and worktree-scope exclusion. ``--selftest-test`` separately proves passing and
failing native build graphs plus test-contract enforcement. A collapsed or
unmanaged scope trips the file floor instead of reporting a clean tree.

Usage:
    check_zig.py                         # lint gate (fmt --check, ast-check)
    check_zig.py --test                  # run every explicit `zig build test` target
    check_zig.py --format                # reformat all tracked files in place
    check_zig.py --require               # fail (not skip) if zig is absent
    check_zig.py --selftest-lint         # prove fmt/AST checks and fix mode
    check_zig.py --selftest-test         # prove native test execution/contracts
    check_zig.py --list-files            # report managed files for coverage matrix

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
from typing import cast

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import trusted_git_executable
from lint_targets import is_build_output_path

TEST_CONTRACT_NAME = ".zig-test-contract.json"
TEST_DECL_RE = re.compile(r'(?m)^\s*test(?:\s+(?:"[^"\n]+"|[A-Za-z_][A-Za-z0-9_]*))?\s*\{')


def _without_zig_comments(text: str) -> str:
    """Remove line and nested block comments while preserving line structure."""
    output: list[str] = []
    index = 0
    block_depth = 0
    while index < len(text):
        pair = text[index : index + 2]
        if block_depth:
            if pair == "/*":
                block_depth += 1
                index += 2
            elif pair == "*/":
                block_depth -= 1
                index += 2
            else:
                output.append("\n" if text[index] == "\n" else " ")
                index += 1
        elif pair == "//":
            newline = text.find("\n", index)
            if newline < 0:
                break
            output.append("\n")
            index = newline + 1
        elif pair == "/*":
            block_depth = 1
            index += 2
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def _test_declarations(path: Path) -> list[str]:
    """Return real Zig test declarations, excluding comment text."""
    return TEST_DECL_RE.findall(_without_zig_comments(path.read_text(encoding="utf-8")))


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


def _test_contract_errors(root: Path) -> tuple[list[str], int]:
    """Validate test-step wiring, source reachability, and the test floor."""
    errors: list[str] = []
    build_file = root / "build.zig"
    build_text = build_file.read_text(encoding="utf-8")
    if not re.search(r'b\.step\(\s*"test"', build_text):
        errors.append('build.zig has no explicit b.step("test", ...)')
    if "addRunArtifact" not in build_text or ".dependOn(" not in build_text:
        errors.append("build.zig test step does not depend on a run artifact")

    entries, covered, floor, contract_errors = _load_test_contract(root)
    errors.extend(contract_errors)
    standalone_args, standalone_errors = _standalone_source_args(root)
    errors.extend(standalone_errors)

    declared_sources: set[Path] = set()
    for source in covered:
        try:
            resolved = source.resolve(strict=True)
            resolved.relative_to(root.resolve())
            declared_sources.add(resolved)
        except (OSError, ValueError):
            errors.append(f"covered source is missing or outside build root: {source}")

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
    for test_root in entries:
        if "tests" not in test_root.relative_to(root).parts:
            errors.append(f"test root must live under tests/: {test_root.relative_to(root)}")
            continue
        root_text = (
            _without_zig_comments(test_root.read_text(encoding="utf-8"))
            if test_root.is_file()
            else ""
        )
        entry_paths = {entry.resolve() for entry in entries}
        sibling_tests = sorted(
            path
            for path in declared_sources
            if path not in entry_paths
            and "tests" in path.relative_to(root).parts
            and path.suffix == ".zig"
        )
        errors.extend(
            "dedicated Zig test module is not imported by its test root: "
            f"{source.relative_to(root)}"
            for source in sibling_tests
            if re.search(rf'(?m)^\s*_\s*=\s*@import\("{re.escape(source.name)}"\);', root_text)
            is None
        )
    if floor and declared_tests < floor:
        errors.append(f"declared Zig test count {declared_tests} is below floor {floor}")
    return errors, declared_tests


def _validate_test_contracts(roots: list[Path]) -> tuple[dict[str, list[str]], int]:
    """Validate every discovered first-party Zig build root."""
    findings: dict[str, list[str]] = {}
    total_tests = 0
    for root in roots:
        errors, count = _test_contract_errors(root)
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
                [zig, "test", str(rel_source), *standalone_args.get(str(rel_source), [])],
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
        if _run_format(zig, [str(bad_fmt)]) != 0:
            failures.append("  must-stay-quiet: `zig fmt` could not fix unformatted code")
        fixed_unfmt, fixed_syn = _run_lint(zig, [str(bad_fmt)])
        if fixed_unfmt or fixed_syn:
            failures.append(
                f"  must-stay-quiet: formatted fixture remained dirty: {fixed_unfmt} {fixed_syn}"
            )

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


def _write_build_fixture(root: Path, test_source: str) -> None:
    """Write one minimal Zig 0.14 graph with a dedicated test root."""
    (root / "tests").mkdir(exist_ok=True)
    (root / "build.zig").write_text(
        'const std = @import("std");\n'
        "\n"
        "pub fn build(b: *std.Build) void {\n"
        "    const target = b.standardTargetOptions(.{});\n"
        "    const optimize = b.standardOptimizeOption(.{});\n"
        "    const tests = b.addTest(.{\n"
        "        .root_module = b.createModule(.{\n"
        '            .root_source_file = b.path("tests/main.zig"),\n'
        "            .target = target,\n"
        "            .optimize = optimize,\n"
        "        }),\n"
        "    });\n"
        "    const run_tests = b.addRunArtifact(tests);\n"
        '    const test_step = b.step("test", "Run unit tests");\n'
        "    test_step.dependOn(&run_tests.step);\n"
        "}\n",
        encoding="utf-8",
    )
    (root / "tests/main.zig").write_text(test_source, encoding="utf-8")
    (root / TEST_CONTRACT_NAME).write_text(
        json.dumps(
            {
                "test_roots": ["tests/main.zig"],
                "covered_sources": ["tests/main.zig"],
                "minimum_tests": 1,
            }
        )
        + "\n",
        encoding="utf-8",
    )


def _selftest_tests(zig: str, failures: list[str]) -> None:
    """Prove the build-graph test target fails and passes as expected."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        # Passing build graph.
        _write_build_fixture(
            root,
            'const std = @import("std");\n'
            'test "selftest pass" {\n'
            "    try std.testing.expect(true);\n"
            "}\n",
        )
        proc_pass = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "build", "test"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc_pass.returncode != 0:
            failures.append(f"  must-stay-quiet: passing build graph failed: {proc_pass.stderr}")

        # Failing build graph.
        _write_build_fixture(
            root,
            'const std = @import("std");\n'
            'test "selftest fail" {\n'
            "    try std.testing.expect(false);\n"
            "}\n",
        )
        proc_fail = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
            [zig, "build", "test"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if proc_fail.returncode == 0:
            failures.append("  must-fire: failing build graph was accepted")

        # A raw-text import mention must not hide an independently failing source.
        _write_build_fixture(
            root,
            '/*\n_ = @import("shadow.zig");\n*/\n'
            'test "real root" {\n'
            '    try @import("std").testing.expect(true);\n'
            "}\n",
        )
        (root / "tests/shadow.zig").write_text(
            'test "unwired failure" {\n    try @import("std").testing.expect(false);\n}\n',
            encoding="utf-8",
        )
        (root / TEST_CONTRACT_NAME).write_text(
            json.dumps(
                {
                    "test_roots": ["tests/main.zig"],
                    "covered_sources": ["tests/main.zig", "tests/shadow.zig"],
                    "minimum_tests": 1,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        wiring_errors, _ = _test_contract_errors(root)
        if not any("test module is not imported" in error for error in wiring_errors):
            failures.append("  must-fire: test module hidden by a commented import was accepted")

        # A test step that runs a different root must not satisfy the contract.
        (root / "tests/shadow.zig").write_text(
            'test "dummy pass" {\n    try @import("std").testing.expect(true);\n}\n',
            encoding="utf-8",
        )
        build_text = (root / "build.zig").read_text(encoding="utf-8")
        (root / "build.zig").write_text(
            build_text.replace('b.path("tests/main.zig")', 'b.path("tests/shadow.zig")'),
            encoding="utf-8",
        )
        causal_failures, _ = _run_tests(zig, [root])
        if not any(
            "did not compile declared test root" in item for item in causal_failures.values()
        ):
            failures.append("  must-fire: test step wired to a dummy root was accepted")


def _selftest_test_declaration_grammar(
    root: Path,
    passing_source: str,
    contract: Path,
    raw_contract: dict[str, object],
    failures: list[str],
) -> None:
    """Prove identifier tests count and comment text never counts."""
    test_root = root / "tests/main.zig"
    test_root.write_text(
        passing_source + 'test identifierContract { try @import("std").testing.expect(true); }\n',
        encoding="utf-8",
    )
    raw_contract["minimum_tests"] = 2
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, count = _test_contract_errors(root)
    identifier_test_count = 2
    if errors or count != identifier_test_count:
        failures.append(f"  must-stay-quiet: identifier-named Zig test was not counted: {errors}")
    test_root.write_text(passing_source, encoding="utf-8")
    raw_contract["minimum_tests"] = 1

    identifier_source = root / "src/identifier_test.zig"
    identifier_source.write_text(
        'test productionContract { try @import("std").testing.expect(true); }\n',
        encoding="utf-8",
    )
    covered_sources = cast("list[str]", raw_contract["covered_sources"])
    covered_sources.append("src/identifier_test.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("production Zig source contains inline tests" in error for error in errors):
        failures.append("  must-fire: identifier-named production Zig test was accepted")
    identifier_source.unlink()
    covered_sources.remove("src/identifier_test.zig")

    comment_source = root / "src/comment_only.zig"
    comment_source.write_text(
        '/* test "not real" { try @import("std").testing.expect(false); } */\n'
        "pub const value: u32 = 1;\n",
        encoding="utf-8",
    )
    covered_sources.append("src/comment_only.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, count = _test_contract_errors(root)
    if any("production Zig source contains inline tests" in error for error in errors):
        failures.append("  must-stay-quiet: block-comment test text was treated as production test")
    if count != 1:
        failures.append("  must-stay-quiet: block-comment test text inflated the test floor")
    comment_source.unlink()
    covered_sources.remove("src/comment_only.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")


def _selftest_test_contract(failures: list[str]) -> None:
    """Prove test wiring, floor, and orphan checks fire and stay quiet."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        passing_source = (
            'const helper = @import("../helper.zig");\n'
            'test "contract pass" {\n'
            "    _ = helper.value;\n"
            "}\n"
        )
        _write_build_fixture(root, passing_source)
        (root / "helper.zig").write_text("pub const value: u32 = 1;\n", encoding="utf-8")
        (root / TEST_CONTRACT_NAME).write_text(
            json.dumps(
                {
                    "test_roots": ["tests/main.zig"],
                    "covered_sources": ["tests/main.zig", "helper.zig"],
                    "minimum_tests": 1,
                }
            )
            + "\n",
            encoding="utf-8",
        )

        errors, count = _test_contract_errors(root)
        if errors or count != 1:
            failures.append(f"  must-stay-quiet: compliant Zig test contract failed: {errors}")

        contract = root / TEST_CONTRACT_NAME
        raw_contract = json.loads(contract.read_text(encoding="utf-8"))

        raw_contract["standalone_source_args"] = {"helper.zig": ["-fno-emit-bin"]}
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
        errors, _ = _test_contract_errors(root)
        if errors:
            failures.append(
                f"  must-stay-quiet: valid standalone source arguments failed: {errors}"
            )

        raw_contract["standalone_source_args"] = {"missing.zig": ["-fno-emit-bin"]}
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
        errors, _ = _test_contract_errors(root)
        if not any("names no covered production source" in error for error in errors):
            failures.append("  must-fire: standalone arguments for an unknown source were accepted")

        raw_contract["standalone_source_args"] = {"helper.zig": []}
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
        errors, _ = _test_contract_errors(root)
        if not any("non-empty string lists" in error for error in errors):
            failures.append("  must-fire: empty standalone source arguments were accepted")

        raw_contract["standalone_source_args"] = {"helper.zig": ["-fno-emit-bin"]}
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")

        (root / "src").mkdir()
        _selftest_test_declaration_grammar(root, passing_source, contract, raw_contract, failures)
        (root / "src/inline.zig").write_text(
            'test "production test" { try @import("std").testing.expect(true); }\n',
            encoding="utf-8",
        )
        raw_contract["covered_sources"].append("src/inline.zig")
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
        errors, _ = _test_contract_errors(root)
        if not any("production Zig source contains inline tests" in error for error in errors):
            failures.append("  must-fire: inline test in production Zig source was accepted")
        (root / "src/inline.zig").unlink()
        raw_contract["covered_sources"].remove("src/inline.zig")
        contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")

        (root / "orphan.zig").write_text("pub const orphan = true;\n", encoding="utf-8")
        errors, _ = _test_contract_errors(root)
        if not any("orphan Zig source" in error for error in errors):
            failures.append("  must-fire: orphan Zig source was accepted")
        (root / "orphan.zig").unlink()

        (root / TEST_CONTRACT_NAME).write_text(
            json.dumps(
                {
                    "test_roots": ["tests/main.zig"],
                    "covered_sources": ["tests/main.zig", "helper.zig"],
                    "minimum_tests": 2,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        errors, _ = _test_contract_errors(root)
        if not any("below floor" in error for error in errors):
            failures.append("  must-fire: Zig test count below its floor was accepted")

        build_text = (root / "build.zig").read_text(encoding="utf-8")
        (root / "build.zig").write_text(
            build_text.replace(
                'b.step("test", "Run unit tests")', 'b.step("verify", "Run unit tests")'
            ),
            encoding="utf-8",
        )
        errors, _ = _test_contract_errors(root)
        if not any("no explicit" in error for error in errors):
            failures.append("  must-fire: missing Zig build test step was accepted")


def selftest_lint(zig: str) -> int:
    """Prove Zig formatting/AST checks and formatter fix mode in both directions."""
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
    failures: list[str] = []
    _selftest_tests(zig, failures)
    _selftest_test_contract(failures)
    if failures:
        sys.stderr.write("check_zig.py --selftest-test: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1
    print("check_zig.py --selftest-test: OK (build test, placement, contract, census).")
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

    A missing Zig toolchain is handled two ways ON PURPOSE, mirroring
    check_go.py and check_ruff.py. Bare, it prints a notice and exits 0,
    so a contributor without Zig is not blocked by a local hook. Under
    ``--require`` or a ``--selftest-*`` mode exits 1 instead -- CI passes
    ``--require`` precisely so that an absent toolchain fails the build
    rather than skipping silently.

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

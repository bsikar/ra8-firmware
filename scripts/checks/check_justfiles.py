#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Linter and format checker for repository justfiles."""

from __future__ import annotations

import argparse
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIRMWARE_BUILD_RE = re.compile(
    r'^build app="" build_type="([A-Za-z0-9]+)":$',
    re.MULTILINE,
)
BARE_NESTED_JUST_RE = re.compile(r"(?:^\s*@?|\b(?:then|do|else)\s+|(?:&&|\|\||;)\s*)just(?=\s|$)")
CI_SH_CALL_RE = re.compile(r"^\s*@?/bin/bash\s+-p\s+scripts/ci\.sh(?P<args>(?:\s+.*)?)$")
CI_SH_SWITCHES = frozenset({"--container", "--fast", "--list-gates", "--native", "--rebuild"})
CI_SH_VALUE_OPTIONS = frozenset({"--gate", "--selftest-abort"})
NATIVE_FAST_RECIPE_RE = re.compile(
    r"^native_fast:[ \t]*\n(?P<body>(?:[ \t]+[^\n]*(?:\n|$))*)",
    re.MULTILINE,
)
NATIVE_FAST_COMMAND = "/bin/bash -p scripts/ci.sh --native --fast"


def check_firmware_build_default(text: str) -> list[str]:
    """Keep the migrated per-app build default byte-for-behaviour compatible."""
    match = FIRMWARE_BUILD_RE.search(text)
    if match is None:
        return ["just/apps.just: firmware build recipe/default is missing"]
    if match.group(1) != "RelWithDebInfo":
        return [
            "just/apps.just: apps::build must default to RelWithDebInfo "
            "(the historical per-app contract)"
        ]
    return []


def check_nested_just_invocations(text: str, rel: str) -> list[str]:
    """Require same-environment recursion to preserve the invoking Just path.

    A recipe may be entered through an absolute executable while that
    executable's directory is absent from PATH, as on a noninteractive SSH
    session. ``just_executable()`` preserves the known-good executable. A bare
    ``just`` passed through ``devcontainer_run.sh`` is intentionally excluded:
    it runs in the container's namespace, where the host executable path is
    invalid and the image owns PATH.
    """
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if "scripts/ci/devcontainer_run.sh" in line:
            continue
        if BARE_NESTED_JUST_RE.search(line) is not None:
            findings.append(
                f"{rel}:{number}: nested Just call must use "
                '"{{ just_executable() }}" instead of PATH lookup'
            )
    return findings


def check_ci_driver_invocations(text: str, rel: str) -> list[str]:
    """Reject stale or malformed ``scripts/ci.sh`` options in Just recipes."""
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        match = CI_SH_CALL_RE.fullmatch(line)
        if match is None:
            continue
        try:
            args = shlex.split(match.group("args"))
        except ValueError as exc:
            findings.append(f"{rel}:{number}: cannot parse scripts/ci.sh arguments: {exc}")
            continue
        index = 0
        while index < len(args):
            option = args[index]
            if option in CI_SH_SWITCHES:
                index += 1
                continue
            if option in CI_SH_VALUE_OPTIONS:
                if index + 1 >= len(args) or args[index + 1].startswith("--"):
                    findings.append(f"{rel}:{number}: {option} requires one value")
                    break
                index += 2
                continue
            findings.append(
                f"{rel}:{number}: unsupported scripts/ci.sh option or argument {option!r}"
            )
            break
    return findings


def check_ci_native_fast_contract(text: str, rel: str) -> list[str]:
    """Pin the public native-fast recipe to the CI driver's real argv."""
    matches = list(NATIVE_FAST_RECIPE_RE.finditer(text))
    if len(matches) != 1:
        return [f"{rel}: expected exactly one native_fast recipe, found {len(matches)}"]
    body = [line.strip() for line in matches[0].group("body").splitlines() if line.strip()]
    if body != [NATIVE_FAST_COMMAND]:
        return [f"{rel}: native_fast must contain only `{NATIVE_FAST_COMMAND}`; found {body!r}"]
    return []


def find_justfiles() -> list[Path]:
    """Return all tracked justfiles in the repository."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable and fixed argv
        [
            git_bin,
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "justfile",
            "*.just",
            "just/*.just",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    paths: list[Path] = []
    for raw_line in proc.stdout.splitlines():
        line = raw_line.strip()
        if line and (REPO_ROOT / line).is_file():
            paths.append(REPO_ROOT / line)
    return sorted(paths)


def check_file(path: Path) -> list[str]:
    """Check a justfile for syntax and formatting."""
    findings: list[str] = []
    if not path.is_file():
        return [f"{path}: file not found"]

    rel = path.relative_to(REPO_ROOT).as_posix()
    text = path.read_text(encoding="utf-8")
    findings.extend(check_nested_just_invocations(text, rel))
    findings.extend(check_ci_driver_invocations(text, rel))

    if path.resolve() == (REPO_ROOT / "just/apps.just").resolve():
        findings.extend(check_firmware_build_default(text))
    if rel == "just/ci.just":
        findings.extend(check_ci_native_fast_contract(text, rel))

    just_bin = shutil.which("just")
    if not just_bin:
        return findings

    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [just_bin, "--unstable", "--fmt", "--check", "--justfile", str(path)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        findings.append(
            f"{rel}: formatting check failed (run `just --fmt --justfile {rel}` to fix)"
        )
    return findings


def _selftest_build_default() -> tuple[int, str | None]:
    """Exercise the firmware-build default in both directions."""
    build_cases = (
        ('build app="" build_type="RelWithDebInfo":\n', False, "historical default passes"),
        ('build app="" build_type="Debug":\n', True, "Debug default fires"),
        ('build app="":\n', True, "missing selectable default fires"),
    )
    for text, expected, label in build_cases:
        if bool(check_firmware_build_default(text)) != expected:
            return len(build_cases), label
    return len(build_cases), None


def _selftest_nested_just() -> tuple[int, str | None]:
    """Exercise nested Just invocation policy in both directions."""
    nested_cases = (
        ("    just quality::run\n", True, "direct nested lookup fires"),
        ("    @just hooks\n", True, "quiet nested lookup fires"),
        ("    if ok; then just tests::build; fi\n", True, "shell-chain lookup fires"),
        (
            '    "{{ just_executable() }}" quality::run\n',
            False,
            "invoking executable stays quiet",
        ),
        (
            "    bash scripts/ci/devcontainer_run.sh -- just quality::local::check\n",
            False,
            "container-owned lookup stays quiet",
        ),
        ('    @echo "Run just quality::run"\n', False, "help prose stays quiet"),
    )
    for text, expected, label in nested_cases:
        if bool(check_nested_just_invocations(text, "fixture.just")) != expected:
            return len(nested_cases), label
    return len(nested_cases), None


def _selftest_ci_driver() -> tuple[int, str | None]:
    """Exercise generic CI-driver option validation in both directions."""
    ci_driver_cases = (
        (
            "    /bin/bash -p scripts/ci.sh --native --fast\n",
            False,
            "separate native and fast switches stay valid",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --gate work-harness\n",
            False,
            "gate value stays valid",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --native-fast\n",
            True,
            "invented combined switch fires",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --gate\n",
            True,
            "missing gate value fires",
        ),
    )
    for text, expected, label in ci_driver_cases:
        if bool(check_ci_driver_invocations(text, "fixture.just")) != expected:
            return len(ci_driver_cases), label
    return len(ci_driver_cases), None


def _selftest_native_fast() -> tuple[int, str | None]:
    """Exercise the exact native-fast recipe contract in both directions."""
    native_fast_cases = (
        (
            f"native_fast:\n    {NATIVE_FAST_COMMAND}\n\nalias native-fast := native_fast\n",
            False,
            "exact native-fast recipe stays valid",
        ),
        (
            "native_fast:\n    bash scripts/ci.sh --native-fast\n",
            True,
            "historical stale recipe fires",
        ),
        (
            "native_fast_renamed:\n    /bin/bash -p scripts/ci.sh --native --fast\n",
            True,
            "missing native-fast recipe fires",
        ),
    )
    for text, expected, label in native_fast_cases:
        if bool(check_ci_native_fast_contract(text, "just/ci.just")) != expected:
            return len(native_fast_cases), label
    return len(native_fast_cases), None


def selftest() -> int:
    """Run internal selftest."""
    total = 0
    for run_cases in (
        _selftest_build_default,
        _selftest_nested_just,
        _selftest_ci_driver,
        _selftest_native_fast,
    ):
        count, failure = run_cases()
        total += count
        if failure is not None:
            print(f"selftest: check_justfiles.py FAIL: {failure}", file=sys.stderr)
            return 1
    print(f"selftest: check_justfiles.py OK ({total} both-direction cases)")
    return 0


def main() -> int:
    """Check justfiles in the repository."""
    parser = argparse.ArgumentParser(description="Check justfiles in the repository")
    parser.add_argument("--list-files", action="store_true", help="List all scanned justfiles")
    parser.add_argument("--check", action="store_true", help="Run formatting check on justfiles")
    parser.add_argument("--selftest", action="store_true", help="Run internal selftest")
    parser.add_argument("paths", nargs="*", help="Optional specific paths to check")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    files = [Path(p).resolve() for p in args.paths] if args.paths else find_justfiles()

    if args.list_files:
        for f in files:
            print(f.relative_to(REPO_ROOT))
        return 0

    findings: list[str] = []
    for f in files:
        findings.extend(check_file(f))

    if findings:
        for finding in findings:
            sys.stderr.write(f"{finding}\n")
        return 1

    print(f"Justfiles clean ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

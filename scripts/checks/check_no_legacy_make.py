#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject legacy repository task invocations in authored surfaces.

The repository task runner is Just. GNU Make can still be a real dependency
of CMake or an upstream source build, so this checker deliberately matches
only command-shaped task invocations: an executable shell/YAML/Docker line, a
shell command array, or a command presented in quotes/backticks or after a
user-guidance verb. Natural English, CMake/Makefile names, tool lists, and
dependency probes such as ``command -v make`` stay outside that shape. A real
upstream build belongs outside the CI, Just, developer-script, and MCP
task-entry-point scope; adding one there requires a narrow, path-specific
exception and a negative self-test.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SELF = "scripts/checks/check_no_legacy_make.py"
EXACT_FILES = frozenset(
    {".clangd", ".cppcheck-suppressions", ".env.example", "CMakePresets.json", "justfile"}
)
PREFIXES = (
    ".devcontainer/",
    ".github/workflows/",
    ".vscode/",
    "just/",
    "scripts/",
    "tools/mcp/",
)
DOC_SUFFIXES = frozenset({".md", ".mdx", ".rst"})
EXCLUDED_PREFIXES = (
    "docs/sbom/upstream/",
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "port/netxduo/",
    "port/nimble/",
    "port/threadx/",
    "port/usbx/",
    "tests/fixtures/",
)
BASELINE_RE = re.compile(r"^\.github/[^/]*baseline[^/]*\.txt$")
MIN_SCOPED_FILES = 650

# Quoting the executable does not change what runs. Keep these alternatives
# explicit so the expression cannot accept mismatched quotes.
MAKE_EXECUTABLE = r'(?:g?make|"g?make"|\'g?make\')'

# Command at the beginning of a shell line, a one-line YAML ``run:``, or a
# Dockerfile RUN. Matching the first argument even when it starts with ``-`` is
# important: the legacy runner's ``-C apps/...`` form was the dominant
# pre-migration build entry point.
ACTIVE_COMMAND_RE = re.compile(
    rf"^\s*(?:(?:RUN|run:)\s+)?({MAKE_EXECUTABLE})(?=\s|$)"
    r"(?:\s+([^\s#;&|]+))?"
)

# Shell arrays are frequently executed later as ``"${cmd[@]}"``. Looking only
# for a command in column zero lets such an invocation hide indefinitely.
ARRAY_COMMAND_RE = re.compile(
    rf"^\s*[A-Za-z_][A-Za-z0-9_]*\s*=\(\s*({MAKE_EXECUTABLE})(?=\s|\))"
    r"(?:\s+([^\s)]+))?"
)

# A bare command in a comment must end after the target. This accepts old
# one-line usage hints while rejecting a natural-language sentence.
COMMENT_COMMAND_RE = re.compile(rf"^\s*#\s*({MAKE_EXECUTABLE})\s+([^\s]+)\s*[.`'\"]?\s*$")

# A command shown to a reader verbatim in backticks or quotes. The checker
# source builds its mutation strings dynamically so its own self-test does not
# need a live exception.
QUOTED_COMMAND_RE = re.compile(
    r"(?:`|'|\")(g?make)(?:\s+([^\s`'\"]+)|(?:`|'|\")+\s+(?:target|recipe|task)\b)"
)

# Unquoted user guidance. Requiring an action verb avoids natural sentences
# such as "these limits make an empty scan fail".
GUIDANCE_COMMAND_RE = re.compile(
    rf"\b(?:run|use|invoke|try|rerun|execute)\s+({MAKE_EXECUTABLE})\s+"
    r"([^\s`'\"]+)",
    re.IGNORECASE,
)


def scoped_files() -> list[str]:
    """Return authored documentation and automation covered by the migration contract."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable and fixed arguments
        [
            git_bin,
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    rels = proc.stdout.decode("utf-8", errors="strict").split("\0")
    selected = set()
    for rel in rels:
        if not rel or rel.startswith(EXCLUDED_PREFIXES):
            continue
        path = Path(rel)
        if not (REPO_ROOT / path).is_file():
            continue
        if (
            rel in EXACT_FILES
            or rel.startswith(PREFIXES)
            or BASELINE_RE.match(rel) is not None
            or path.suffix.lower() in DOC_SUFFIXES
            or path.name == "Dockerfile"
        ):
            selected.add(rel)
    # The checker may be validated before its newly-created file is staged.
    if (REPO_ROOT / SELF).is_file():
        selected.add(SELF)
    return sorted(selected)


def legacy_invocation(line: str, *, active_commands: bool = True) -> str | None:
    """Return the command-shaped legacy invocation on ``line``, if any."""
    patterns = [COMMENT_COMMAND_RE, QUOTED_COMMAND_RE, GUIDANCE_COMMAND_RE]
    if active_commands:
        patterns[0:0] = [ACTIVE_COMMAND_RE, ARRAY_COMMAND_RE]
    for pattern in patterns:
        match = pattern.search(line)
        if match is not None:
            executable = match.group(1).strip("\"'")
            first_arg = match.group(2)
            return f"{executable} {first_arg}" if first_arg else executable
    return None


def scan(rels: list[str]) -> list[str]:
    """Return path/line findings for every command-shaped legacy reference."""
    findings: list[str] = []
    for rel in rels:
        path = REPO_ROOT / rel
        active_commands = rel.endswith((".sh", ".yml", ".yaml")) or path.name == "Dockerfile"
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            invocation = legacy_invocation(line, active_commands=active_commands)
            if invocation is not None:
                findings.append(f"{rel}:{number}: legacy repository task: {invocation}")
    return findings


def selftest() -> int:
    """Prove command forms fire and legitimate Make mentions stay quiet."""
    command = "ma" + "ke"
    gnu_command = "g" + command
    cases = (
        (f"{command} ci", True, "a direct shell task fires"),
        (f"{command} -C apps/board/stand_alone/blink build", True, "a -C task fires"),
        (f"{gnu_command} ci", True, "a gmake task fires"),
        (f"cmd=({command} -C apps/blink)", True, "a command array fires"),
        (f'cmd=("{command}" "-C" apps/blink)', True, "a quoted array command fires"),
        (f'"{command}" -C apps/blink', True, "a quoted executable fires"),
        (f"run: {command} -C apps/blink", True, "a one-line YAML command fires"),
        (f"RUN {command} coverage", True, "a Dockerfile task fires"),
        (f"# {command} ci-native", True, "a bare comment hint fires"),
        (f"# `{command} sbom` regenerates it", True, "a backticked hint fires"),
        (
            f"CI (or a local ``{command}`` target) catches drift",
            True,
            "a quoted legacy task-runner reference fires",
        ),
        (f"Please run {command} misra", True, "an unquoted user hint fires"),
        ("command -v make || missing=build-essential", False, "a dependency probe stays quiet"),
        ("command -v gmake || missing=build-essential", False, "a gmake probe stays quiet"),
        ("for tool in curl cmake make tar cc; do", False, "an upstream tool list stays quiet"),
        ("these controls make an empty scan fail", False, "natural English stays quiet"),
        ("# make the detector fail", False, "natural comment prose stays quiet"),
        ("CMakeLists.txt and GNUmakefile", False, "build-system filenames stay quiet"),
        (
            "# Make is required by an upstream source build",
            False,
            "an explanatory mention stays quiet",
        ),
    )
    failures = [
        label for line, expected, label in cases if bool(legacy_invocation(line)) != expected
    ]
    if failures:
        for failure in failures:
            print(f"check_no_legacy_make.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"check_no_legacy_make.py --selftest: PASS ({len(cases)} both-direction cases)")
    return 0


def main() -> int:
    """Run detector self-tests or scan the live tracked scope."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: check_no_legacy_make.py [--selftest]", file=sys.stderr)
        return 2
    try:
        rels = scoped_files()
    except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
        print(f"check_no_legacy_make.py: cannot enumerate tracked files: {exc}", file=sys.stderr)
        return 2
    if len(rels) < MIN_SCOPED_FILES or SELF not in rels:
        print(
            f"check_no_legacy_make.py: scope collapsed to {len(rels)} file(s); "
            f"expected at least {MIN_SCOPED_FILES} including {SELF}",
            file=sys.stderr,
        )
        return 2
    findings = scan(rels)
    if findings:
        print("check_no_legacy_make.py: legacy repository task references:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print("Use the authoritative namespaced Just recipe instead.", file=sys.stderr)
        return 1
    print(f"check_no_legacy_make.py: clean ({len(rels)} authored files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

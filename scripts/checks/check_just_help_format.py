#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ruff: noqa: D103,E501,FLY002,I001,PLR2004,SLF001
"""Check spacing and description columns in rendered Just help screens.

Every command row must use two-space indentation, contain a description, and
start that description at the same column as the other command rows in its
screen. The checker renders the same root and module screens used by
``check_just_navigation.py`` so a command can be both reachable and readable.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from scripts.checks import check_just_navigation as navigation
from scripts.checks import check_just_references as references


COMMAND_ROW_RE = re.compile(r"^(?P<indent>\s*)just\s+(?P<command>\S+)(?P<tail>.*)$")
ECHO_ROW_RE = re.compile(
    r'^(?P<indent>\s*)@echo (?P<quote>["\'])(?P<text>.*)(?P=quote)(?P<newline>\n?)$'
)


def _screen_rows(screen: str, output: str) -> tuple[list[str], list[tuple[int, int]]]:
    """Return formatting findings and ``(line, description-column)`` rows."""
    findings: list[str] = []
    rows: list[tuple[int, int]] = []
    path = Path("rendered-screen.just")
    for number, line in enumerate(output.splitlines(), start=1):
        if not references.references_in_line(path, line):
            continue
        if line.rstrip() != line:
            findings.append(f"{screen}:{number}: command row has trailing whitespace")
        match = COMMAND_ROW_RE.match(line)
        if match is None:
            findings.append(
                f"{screen}:{number}: command row must start with two spaces followed by `just`"
            )
            continue
        if len(match.group("indent")) != 2:
            findings.append(
                f"{screen}:{number}: command row uses {len(match.group('indent'))} leading spaces; expected 2"
            )
        separators = list(re.finditer(r" {2,}(?=\S)", match.group("tail")))
        if not separators:
            findings.append(f"{screen}:{number}: command row has no description column")
            continue
        separator = separators[-1]
        description_column = match.start("tail") + separator.end()
        rows.append((number, description_column))
    return findings, rows


def _render_screen(repo_root: Path, module: str | None) -> tuple[str, str | None]:
    """Render one Just screen and return its output or an error."""
    just_bin = shutil.which("just")
    if just_bin is None:
        return "", "just is required to render help screens"
    argv = [] if module is None else [module]
    try:
        proc = subprocess.run(  # noqa: S603 -- resolved executable and fixed argv
            [just_bin, *argv],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except subprocess.TimeoutExpired:
        return "", "screen did not finish within 10 seconds"
    output = "\n".join(part for part in (proc.stdout, proc.stderr) if part)
    if proc.returncode != 0:
        detail = output.strip().splitlines()
        return "", detail[-1] if detail else f"just exited {proc.returncode}"
    return output, None


def _audit_screen(screen: str, output: str) -> list[str]:
    """Check one rendered screen's command indentation and columns."""
    findings, rows = _screen_rows(screen, output)
    columns = {column for _line, column in rows}
    if len(columns) > 1:
        expected = max(columns)
        for line, column in rows:
            if column != expected:
                findings.append(
                    f"{screen}:{line}: description starts at column {column}; "
                    f"expected column {expected}"
                )
    return findings


def _source_row(text: str) -> tuple[str, str] | None:
    """Split one authored ``@echo`` command row into prefix and description."""
    match = ECHO_ROW_RE.match(text)
    if match is None or not references.references_in_line(
        Path("rendered-screen.just"), match.group("text")
    ):
        return None
    row = COMMAND_ROW_RE.match(match.group("text"))
    if row is None:
        return None
    separators = list(re.finditer(r" {2,}(?=\S)", row.group("tail")))
    if not separators:
        return None
    separator = separators[-1]
    description_start = row.start("tail") + separator.end()
    prefix = match.group("text")[: description_start - len(separator.group())].rstrip()
    description = match.group("text")[description_start:]
    return prefix, description


def _visible_width(text: str) -> int:
    """Measure the width after Just decodes common string escapes."""
    return len(text.replace(r"\"", '"'))


def _fix_authored_help(repo_root: Path) -> list[Path]:
    """Align authored ``@echo`` command rows in every tracked Justfile."""
    changed: list[Path] = []
    paths = [repo_root / "Justfile", *sorted((repo_root / "just").glob("*.just"))]
    for path in paths:
        original = path.read_text(encoding="utf-8")
        lines = original.splitlines(keepends=True)
        parsed: dict[int, tuple[str, str]] = {}
        for index, line in enumerate(lines):
            row = _source_row(line)
            if row is not None:
                parsed[index] = row
        if not parsed:
            continue
        target = max(_visible_width(prefix) + 2 for prefix, _description in parsed.values())
        rewritten = list(lines)
        for index, (prefix, description) in parsed.items():
            match = ECHO_ROW_RE.match(lines[index])
            if match is None:
                continue
            text = prefix + (" " * (target - _visible_width(prefix))) + description
            rewritten[index] = (
                f"{match.group('indent')}@echo {match.group('quote')}{text}"
                f"{match.group('quote')}{match.group('newline')}"
            )
        updated = "".join(rewritten)
        if updated != original:
            path.write_text(updated, encoding="utf-8")
            changed.append(path)
    return changed


def check(repo_root: Path) -> list[str]:
    """Render every navigable Just screen and return formatting findings."""
    try:
        dump = navigation._just_dump(repo_root)
        modules = navigation._modules(dump)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        return [str(exc)]
    findings: list[str] = []
    screens: list[tuple[str, str | None]] = [("<root>", None)]
    for module, node in modules:
        default = navigation._screen_recipe(node)
        if default is None or default.get("parameters"):
            continue
        screens.append((module, module))
    for screen, module in screens:
        output, error = _render_screen(repo_root, module)
        if error is not None:
            findings.append(f"{screen}: screen unavailable: {error}")
            continue
        findings.extend(_audit_screen(screen, output))
    return findings


def _selftest() -> int:
    """Exercise aligned, misaligned, missing-description, and whitespace cases."""
    aligned = "\n".join(
        (
            f"{'  just checks::list':<48}List registered checks",
            f"{'  just checks::run <name>':<48}Run one registered check",
        )
    )
    misaligned = "\n".join(
        (
            f"{'  just checks::list':<44}List registered checks",
            f"{'  just checks::run <name>':<40}Run one registered check",
        )
    )
    failures: list[str] = []
    if _audit_screen("checks", aligned):
        failures.append("aligned command descriptions were rejected")
    if not any(
        "description starts at column" in finding
        for finding in _audit_screen("checks", misaligned)
    ):
        failures.append("misaligned command descriptions were accepted")
    if not any(
        "no description column" in finding
        for finding in _audit_screen("checks", "  just checks::list")
    ):
        failures.append("missing command description was accepted")
    if not any(
        "trailing whitespace" in finding
        for finding in _audit_screen("checks", "  just checks::list  List \t")
    ):
        failures.append("trailing whitespace was accepted")
    if failures:
        for failure in failures:
            print(f"selftest: check_just_help_format.py FAIL: {failure}", file=sys.stderr)
        return 1
    print("selftest: check_just_help_format.py OK (spacing and column cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run checker selftests")
    parser.add_argument("--fix", action="store_true", help="align authored @echo command rows")
    args = parser.parse_args()
    if args.selftest:
        return _selftest()
    repo_root = Path(__file__).resolve().parent.parent.parent
    if args.fix:
        changed = _fix_authored_help(repo_root)
        for path in changed:
            print(f"formatted {path.relative_to(repo_root)}")
    findings = check(Path(__file__).resolve().parent.parent.parent)
    if findings:
        print("check-just-help-format: inconsistent rendered help rows:", file=sys.stderr)
        for finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        print(f"check-just-help-format: FAIL ({len(findings)} finding(s))", file=sys.stderr)
        return 1
    print("Just help format clean: command rows have aligned descriptions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

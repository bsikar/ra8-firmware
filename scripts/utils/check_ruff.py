#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ruff lint + format for first-party Python (scripts/, tools/, tests/).

Ruff (configured in pyproject.toml) is the linter and formatter for the
project's Python tooling.  This wrapper runs ``ruff check`` and
``ruff format --check`` and fails on any finding -- there is no
grandfathering; the tree must be clean.  Ruff must be on PATH or named via the
``RUFF`` environment variable; without it the gate skips locally (so
contributors are not blocked) unless ``--require`` is passed, which CI uses to
fail on a missing tool.

Run::

    check_ruff.py             # gate (fail on any finding)
    check_ruff.py --require   # fail (not skip) if ruff is absent

Exit 0 if clean, exit 1 on findings, exit 2 on ruff error.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGETS = ("scripts", "tools", "tests")


def _find_ruff() -> str | None:
    env = os.environ.get("RUFF")
    if env and Path(env).exists():
        return env
    return shutil.which("ruff")


def _existing_targets() -> list[str]:
    return [t for t in TARGETS if (REPO_ROOT / t).is_dir()]


def _rel(filename: str) -> str:
    path = Path(filename)
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return filename


def _run_check(ruff: str, targets: list[str]) -> dict[str, dict[str, int]]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "check", "--output-format=json", *targets],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"ruff check failed (exit {proc.returncode})\n")
        sys.exit(2)
    findings: dict[str, dict[str, int]] = {}
    for item in json.loads(proc.stdout or "[]"):
        rel = _rel(item["filename"])
        code = item.get("code") or "SYNTAX"
        findings.setdefault(rel, {})
        findings[rel][code] = findings[rel].get(code, 0) + 1
    return findings


def _run_format(ruff: str, targets: list[str]) -> list[str]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "format", "--check", *targets],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    lines = (proc.stdout + proc.stderr).splitlines()
    return sorted(
        _rel(line.split(":", 1)[1].strip()) for line in lines if line.startswith("Would reformat:")
    )


def _report(lint: dict[str, dict[str, int]], fmt: list[str]) -> None:
    if lint:
        sys.stderr.write("check_ruff.py: ruff lint finding(s):\n")
        for relfile in sorted(lint):
            for code, count in sorted(lint[relfile].items()):
                sys.stderr.write(f"  {relfile}: {code} x{count}\n")
    if fmt:
        sys.stderr.write("check_ruff.py: file(s) need `ruff format`:\n")
        for relfile in fmt:
            sys.stderr.write(f"  {relfile}\n")
    sys.stderr.write("\nFix the finding, or run `ruff format`.\n")


def main(argv: list[str]) -> int:
    ruff = _find_ruff()
    if not ruff:
        msg = "check_ruff.py: ruff not found"
        if "--require" in argv[1:]:
            sys.stderr.write(msg + " (--require set)\n")
            return 1
        print(msg + " -- skipping (install ruff to enforce locally).")
        return 0

    targets = _existing_targets()
    lint = _run_check(ruff, targets)
    fmt = _run_format(ruff, targets)
    if not lint and not fmt:
        print("check_ruff.py: clean (no lint or format findings).")
        return 0
    _report(lint, fmt)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

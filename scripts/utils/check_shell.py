#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: shellcheck + shfmt for first-party shell scripts.

ShellCheck (correctness, at ``--severity=warning``) and shfmt (formatting,
2-space case-indented) are the shell equivalents of ruff.  This wrapper fails
on any finding -- no grandfathering.  Both tools must be on PATH (or named via
``SHELLCHECK`` / ``SHFMT``); without them the gate skips locally unless
``--require`` is passed, which CI uses to fail on a missing tool.

Run::

    check_shell.py             # gate (fail on any finding)
    check_shell.py --require   # fail (not skip) if a tool is absent

Exit 0 if clean, exit 1 on findings, exit 2 on tool error.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# shfmt style: 2-space indent, indent switch-case branches (matches the C side).
SHFMT_ARGS = ("-i", "2", "-ci")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "/build/",
    "/build-cov/",
    "/_deps/",
    "node_modules/",
)


def _find(env_var: str, name: str) -> str | None:
    env = os.environ.get(env_var)
    if env and Path(env).exists():
        return env
    return shutil.which(name)


def _scripts() -> list[str]:
    out = []
    for path in REPO_ROOT.rglob("*.sh"):
        rel = str(path.relative_to(REPO_ROOT))
        if not any(frag in f"/{rel}" for frag in EXCLUDE_FRAGMENTS):
            out.append(rel)
    return sorted(out)


def _run_shellcheck(tool: str, files: list[str]) -> dict[str, dict[str, int]]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [tool, "--severity=warning", "-f", "json", *files],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"shellcheck failed (exit {proc.returncode})\n")
        sys.exit(2)
    findings: dict[str, dict[str, int]] = {}
    for item in json.loads(proc.stdout or "[]"):
        rel = item["file"]
        code = f"SC{item['code']}"
        findings.setdefault(rel, {})
        findings[rel][code] = findings[rel].get(code, 0) + 1
    return findings


def _run_shfmt(tool: str, files: list[str]) -> list[str]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [tool, "-l", *SHFMT_ARGS, *files],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return sorted(line.strip() for line in proc.stdout.splitlines() if line.strip())


def _report(checks: dict[str, dict[str, int]], fmt: list[str]) -> None:
    if checks:
        sys.stderr.write("check_shell.py: shellcheck finding(s):\n")
        for relfile in sorted(checks):
            for code, count in sorted(checks[relfile].items()):
                sys.stderr.write(f"  {relfile}: {code} x{count}\n")
    if fmt:
        joined = " ".join(SHFMT_ARGS)
        sys.stderr.write(f"check_shell.py: file(s) need `shfmt -w {joined}`:\n")
        for relfile in fmt:
            sys.stderr.write(f"  {relfile}\n")
    sys.stderr.write("\nFix the finding or reformat.\n")


def main(argv: list[str]) -> int:
    sc_tool = _find("SHELLCHECK", "shellcheck")
    fmt_tool = _find("SHFMT", "shfmt")
    if not sc_tool or not fmt_tool:
        missing = " and ".join(
            n for n, t in (("shellcheck", sc_tool), ("shfmt", fmt_tool)) if not t
        )
        msg = f"check_shell.py: {missing} not found"
        if "--require" in argv[1:]:
            sys.stderr.write(msg + " (--require set)\n")
            return 1
        print(msg + " -- skipping (install to enforce locally).")
        return 0

    files = _scripts()
    if not files:
        print("check_shell.py: no shell scripts to scan")
        return 0
    checks = _run_shellcheck(sc_tool, files)
    fmt = _run_shfmt(fmt_tool, files)
    if not checks and not fmt:
        print("check_shell.py: clean (no shellcheck or shfmt findings).")
        return 0
    _report(checks, fmt)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

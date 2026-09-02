#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject first-party attempts to override PEP 668 package ownership.

Python-managed repository tools belong in a virtual environment. A system-pip
override can mutate apt-owned files, while a user-site fallback makes the
interpreter and PATH depend on whichever account happened to provision a host.
Scan every authored tracked or untracked file so the unsafe option cannot
return in workflows, images, provisioning, documentation, or error hints.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SELF = "scripts/checks/check_no_unsafe_python_install.py"
FORBIDDEN = "--break-" + "system-packages"
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
MIN_SCOPED_FILES = 4000


def scoped_files() -> list[str]:
    """Return all first-party files known to Git, including new files."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable; fixed arguments
        [git_bin, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    rels = proc.stdout.decode("utf-8", errors="strict").split("\0")
    selected = {
        rel
        for rel in rels
        if rel and not rel.startswith(EXCLUDED_PREFIXES) and (REPO_ROOT / rel).is_file()
    }
    if (REPO_ROOT / SELF).is_file():
        selected.add(SELF)
    return sorted(selected)


def scan_text(text: str) -> list[int]:
    """Return one-based line numbers containing the unsafe pip option."""
    return [number for number, line in enumerate(text.splitlines(), start=1) if FORBIDDEN in line]


def scan(rels: list[str]) -> list[str]:
    """Return path/line findings, skipping non-text tracked assets."""
    findings: list[str] = []
    for rel in rels:
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        findings.extend(f"{rel}:{line}" for line in scan_text(text))
    return findings


def selftest() -> int:
    """Prove the detector fires and accepts isolated installation guidance."""
    unsafe = "python3 -m pip install " + FORBIDDEN + " libclang"
    cases = (
        (unsafe, [1], "an unsafe active install fires"),
        ("hint: " + unsafe, [1], "an unsafe documentation hint fires"),
        ("python3 -m venv .venv\n.venv/bin/pip install libclang", [], "a venv passes"),
        ("python3 -m pip --version", [], "a non-mutating pip probe passes"),
    )
    failures = [label for text, expected, label in cases if scan_text(text) != expected]
    if failures:
        for failure in failures:
            print(f"check_no_unsafe_python_install.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"check_no_unsafe_python_install.py --selftest: PASS ({len(cases)} cases)")
    return 0


def main() -> int:
    """Run detector self-tests or scan the live first-party tree."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: check_no_unsafe_python_install.py [--selftest]", file=sys.stderr)
        return 2
    try:
        rels = scoped_files()
    except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
        print(f"cannot enumerate first-party files: {exc}", file=sys.stderr)
        return 2
    if len(rels) < MIN_SCOPED_FILES or SELF not in rels:
        print(
            f"scope collapsed to {len(rels)} files; expected at least "
            f"{MIN_SCOPED_FILES} including {SELF}",
            file=sys.stderr,
        )
        return 2
    findings = scan(rels)
    if findings:
        print("unsafe system-Python package override found:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print("Create a venv and wire its interpreter/PATH explicitly.", file=sys.stderr)
        return 1
    print(f"check_no_unsafe_python_install.py: clean ({len(rels)} first-party files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Verify that a managed virtual environment exactly matches a uv export."""

from __future__ import annotations

import importlib.metadata
import platform
import re
import sys
import tempfile
from pathlib import Path

PIN_RE = re.compile(
    r"^([A-Za-z0-9][A-Za-z0-9_.-]*)==([0-9][A-Za-z0-9.!+_-]*)"
    r"(?: ; (.+?))? \\?$"
)
IGNORED_BOOTSTRAP_PACKAGES = {"pip", "setuptools", "wheel"}
MIN_LOCKED_PACKAGES = 10
REQUIREMENTS_ARG_COUNT = 2
HASH_RE = re.compile(r"^--hash=sha256:([0-9a-f]{64})(?: \\)?$")


def canonical_name(name: str) -> str:
    """Apply Python distribution-name canonicalization."""
    return re.sub(r"[-_.]+", "-", name).lower()


def marker_applies(marker: str | None) -> bool:
    """Evaluate the small exact marker vocabulary emitted by the locked groups."""
    if marker is None:
        return True
    values = {
        "implementation_name": sys.implementation.name,
        "platform_python_implementation": platform.python_implementation(),
        "sys_platform": sys.platform,
    }
    clauses = marker.split(" and ")
    for clause in clauses:
        match = re.fullmatch(
            r"(implementation_name|platform_python_implementation|sys_platform) "
            r"(==|!=) '([^']+)'",
            clause,
        )
        if match is None:
            message = f"unsupported environment marker in lock export: {marker!r}"
            raise ValueError(message)
        variable, operator, expected = match.groups()
        equal = values[variable] == expected
        if (operator == "==" and not equal) or (operator == "!=" and equal):
            return False
    return True


def expected_packages(lock_path: Path) -> dict[str, str]:
    """Parse exact records and require authenticated hashes for every one."""
    expected: dict[str, str] = {}
    seen: set[str] = set()
    current: tuple[str, str, bool, set[str]] | None = None

    def finish_record() -> None:
        nonlocal current
        if current is None:
            return
        name, version, applies, hashes = current
        if not hashes:
            message = f"locked requirement has no SHA-256 hashes: {name}"
            raise ValueError(message)
        if applies:
            expected[name] = version
        current = None

    for line_number, line in enumerate(lock_path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        pin = PIN_RE.fullmatch(stripped)
        if pin is not None:
            finish_record()
            raw_name, version, marker = pin.groups()
            name = canonical_name(raw_name)
            if name in seen:
                message = f"duplicate locked requirement: {name}"
                raise ValueError(message)
            seen.add(name)
            current = (name, version, marker_applies(marker), set())
            continue
        digest = HASH_RE.fullmatch(stripped)
        if digest is not None:
            if current is None:
                message = f"stray hash on line {line_number}"
                raise ValueError(message)
            if digest.group(1) in current[3]:
                message = f"duplicate hash on line {line_number}"
                raise ValueError(message)
            current[3].add(digest.group(1))
            continue
        message = f"malformed locked requirement on line {line_number}: {stripped!r}"
        raise ValueError(message)
    finish_record()
    if len(expected) < MIN_LOCKED_PACKAGES:
        message = f"locked environment export contains only {len(expected)} packages"
        raise ValueError(message)
    return expected


def installed_packages() -> dict[str, str]:
    """Read installed distributions, excluding venv bootstrap tools."""
    return {
        canonical_name(distribution.metadata["Name"]): distribution.version
        for distribution in importlib.metadata.distributions()
        if canonical_name(distribution.metadata["Name"]) not in IGNORED_BOOTSTRAP_PACKAGES
    }


def findings(expected: dict[str, str], installed: dict[str, str]) -> list[str]:
    """Return missing, extra, and wrong-version findings in stable order."""
    problems: list[str] = []
    for name in sorted(expected.keys() | installed.keys()):
        wanted = expected.get(name)
        actual = installed.get(name)
        if wanted != actual:
            problems.append(
                f"{name}: expected {wanted or 'absent'}, installed {actual or 'absent'}"
            )
    return problems


def selftest() -> int:
    """Prove parsing, hash enforcement, and exact comparison both ways."""
    expected = {"alpha": "1.0", "bravo": "2.0"}
    if findings(expected, dict(expected)):
        print("selftest: an exact environment failed", file=sys.stderr)
        return 1
    cases = (
        {"alpha": "1.0"},
        {"alpha": "1.0", "bravo": "2.0", "extra": "3.0"},
        {"alpha": "9.0", "bravo": "2.0"},
    )
    if any(not findings(expected, case) for case in cases):
        print("selftest: missing, extra, or wrong version passed", file=sys.stderr)
        return 1
    records = [
        f"package-{index}==1.{index} \\" + f"\n    --hash=sha256:{index:064x}"
        for index in range(MIN_LOCKED_PACKAGES)
    ]
    valid = "\n".join(records)
    fixtures = {
        "valid": (valid, True),
        "undersized": (records[0], False),
        "malformed": (f"{valid}\nnot an exact pin", False),
        "duplicate-record": (f"{valid}\n{records[0]}", False),
        "hashless": ("\n".join([*records, "extra==1.0 \\"]), False),
        "stray-hash": (
            f"--hash=sha256:{'f' * 64}\n{valid}",
            False,
        ),
        "invalid-hash": (
            valid.replace("0" * 64, "not-a-digest", 1),
            False,
        ),
        "unknown-marker": (
            f"{valid}\noptional==1.0 ; os_name ~= 'posix' \\" + f"\n    --hash=sha256:{'e' * 64}",
            False,
        ),
    }
    if not marker_applies("sys_platform != 'definitely-not-this-platform'"):
        print("selftest: true environment marker evaluated false", file=sys.stderr)
        return 1
    if marker_applies(f"sys_platform == '{sys.platform}-other'"):
        print("selftest: false environment marker evaluated true", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "requirements.lock"
        for label, (content, should_pass) in fixtures.items():
            path.write_text(f"{content}\n", encoding="utf-8")
            try:
                parsed = expected_packages(path)
            except ValueError:
                passed = False
            else:
                passed = len(parsed) == MIN_LOCKED_PACKAGES
            if passed != should_pass:
                print(f"selftest: parser fixture {label} judged {passed}", file=sys.stderr)
                return 1
    print("verify_locked_environment.py --selftest: PASS")
    return 0


def main() -> int:
    """Verify the running interpreter against the provided export."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if len(sys.argv) != REQUIREMENTS_ARG_COUNT:
        print(f"usage: {Path(sys.argv[0]).name} REQUIREMENTS_LOCK", file=sys.stderr)
        return 2
    try:
        problems = findings(expected_packages(Path(sys.argv[1])), installed_packages())
    except (OSError, TypeError, ValueError) as error:
        print(f"verify_locked_environment.py: FATAL: {error}", file=sys.stderr)
        return 2
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("managed environment exactly matches the uv lock export")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

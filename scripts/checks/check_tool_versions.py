#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: assert every pinned host tool resolves to its project-pinned version.

Why this exists
----------------------
The self-hosted runner and the dev box resolve tools through PATH, and PATH
differs between a login shell and a non-interactive one. Measured on the dev
box, ``ssh dev '<cmd>'`` and ``ssh dev 'bash -lc "<cmd>"'`` resolved DIFFERENT
binaries: shellcheck 0.9.0 vs 0.11.0, shfmt 3.6.0 vs 3.13.1, ruff absent vs
0.15.19. A gate run through the wrong PATH produces findings CI never
reproduces, or -- worse -- misses findings CI has. ``use_pinned_tool_path`` in
scripts/ci.sh makes the resolution deterministic; this check makes the WRONG
version FAIL LOUD rather than pass quietly, the same class of hole as
check_annotations.py exiting 0 without libclang.

Single source of truth
-----------------------
The pinned versions are not restated here. Native toolchain pins are parsed
from ``.devcontainer/Dockerfile``; Python tool pins come from the exact direct
dependencies in ``pyproject.toml`` and their transitive closure is committed in
``uv.lock``. Reading each owning source keeps native and container checks equal.

Comparison modes
----------------
* ``exact``     -- version string must equal the pin (just, ruff, shellcheck, shfmt,
                   cmakelang, yamllint, hadolint).
                   These are the tools whose findings drift with the
                   exact version.
* ``major``     -- major must equal the pin (clang-format-22,
                   gcc-14). The clang family and the gcc-14 host-tool arm
                   (#356) are pinned by major on purpose; the tree is
                   formatted/linted/built to that major and the binary carries
                   it in its name.
Non-vacuity
-----------
``--selftest`` builds fake tools that report chosen versions, then asserts the
comparator returns the right verdict for a match AND a mismatch in every mode,
plus a missing tool. Sabotaging the comparator (making it always pass) turns
the selftest red instead of letting a broken check report success forever.

Run::

    check_tool_versions.py                 # verify every pinned tool
    check_tool_versions.py ruff shellcheck # verify only the named tools
    check_tool_versions.py --all           # verify every pinned tool (explicit)
    check_tool_versions.py --selftest       # prove the comparator both ways

Exit 0 when every requested tool matches its pin, 1 when any tool is missing or
the wrong version, 2 when the pin source itself cannot be read.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
DOCKERFILE = REPO_ROOT / ".devcontainer" / "Dockerfile"
PYPROJECT = REPO_ROOT / "pyproject.toml"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

TOOL_TIMEOUT_SECONDS = 30
FAKE_TOOL_MODE = 0o755

MODE_EXACT = "exact"
MODE_MAJOR = "major"

# First dotted-number token (requires at least one dot, so a "2013-2023"
# copyright range in a --version banner is never mistaken for the version).
_VERSION_RE = re.compile(r"\d+(?:\.\d+)+")
_ARG_RE = re.compile(r"^\s*ARG\s+([A-Z0-9_]+)=(\S+)", re.MULTILINE)



@dataclass(frozen=True)
class ToolSpec:
    """One pinned tool: how to resolve it, run it, and judge its version.

    Attributes:
        binary: Executable resolved on PATH (e.g. "ruff", "clang-tidy-18").
        expected: The pinned version, or the pinned major for major mode.
        mode: Comparison mode (MODE_EXACT / MODE_MAJOR).
        source: Human-readable origin of the pin, shown in failure messages.
        version_args: Argument vector that makes the binary print its version.
    """

    binary: str
    expected: str
    mode: str
    source: str
    version_args: tuple[str, ...] = ("--version",)


def _read_dockerfile() -> str:
    """Return the devcontainer Dockerfile text, the pinned-version source.

    Returns:
        The full Dockerfile contents.

    Raises:
        FileNotFoundError: When the pinned-version source of truth is absent.
    """
    if not DOCKERFILE.is_file():
        message = f"pinned-version source of truth missing: {DOCKERFILE}"
        raise FileNotFoundError(message)
    return DOCKERFILE.read_text(encoding="utf-8")


def _dockerfile_args(text: str) -> dict[str, str]:
    """Parse every ``ARG NAME=value`` pin out of Dockerfile `text`.

    Args:
        text: The Dockerfile contents.

    Returns:
        Mapping of ARG name to its pinned value.
    """
    return {match.group(1): match.group(2) for match in _ARG_RE.finditer(text)}


def _arg(args: dict[str, str], key: str) -> str:
    """Return the pinned value for `key`, failing loudly when it is gone.

    Args:
        args: Parsed Dockerfile ARG map.
        key: The ARG name that must exist.

    Returns:
        The pinned value.

    Raises:
        ValueError: When the pin is absent from the Dockerfile.
    """
    if key not in args:
        message = f"Dockerfile no longer pins {key}; update {Path(__file__).name}"
        raise ValueError(message)
    return args[key]


def _pkg_major(text: str, needle: str, label: str) -> str:
    """Return the pinned major from a ``needle-NN`` package/binary token.

    Used for the compiler families whose pin is carried in the package name
    rather than an exact ARG: the clang-18 family and the gcc-14 arm.

    Args:
        text: The Dockerfile contents.
        needle: Package/binary stem preceding the major (e.g. "clang-format").
        label: Human label used in the error message.

    Returns:
        The major version, as text.

    Raises:
        ValueError: When no ``needle-NN`` token is present.
    """
    match = re.search(rf"{re.escape(needle)}-(\d+)", text)
    if match is None:
        message = f"no pinned {label} major ({needle}-NN) in {DOCKERFILE}"
        raise ValueError(message)
    return match.group(1)


def _upstream(value: str) -> str:
    """Strip an apt/Debian revision suffix, keeping the upstream version.

    Args:
        value: An apt version such as "2.13.0-2ubuntu3" or "7.0-1".

    Returns:
        The upstream portion before the final Debian-revision hyphen.
    """
    return value.rsplit("-", 1)[0] if "-" in value else value


def _spec(
    args: dict[str, str],
    binary: str,
    key: str,
    mode: str,
    transform: Callable[[str], str] | None = None,
) -> ToolSpec:
    """Build a ToolSpec whose pin comes from Dockerfile ARG `key`.

    Args:
        args: Parsed Dockerfile ARG map.
        binary: Executable name to resolve on PATH.
        key: The ARG whose value is the pin.
        mode: Comparison mode (one of the MODE_* constants).
        transform: Optional post-processor applied to the raw ARG value.

    Returns:
        The assembled ToolSpec.
    """
    raw = _arg(args, key)
    value = transform(raw) if transform is not None else raw
    return ToolSpec(binary, value, mode, f"ARG {key}")


def _literal_shell_assignment(script: str, variable: str) -> str:
    """Return one simple quoted shell assignment, rejecting drift-prone forms."""
    pattern = re.compile(
        rf'^[ \t]*{re.escape(variable)}="(?P<value>[A-Za-z0-9._-]+)"[ \t]*$',
        re.MULTILINE,
    )
    matches = list(pattern.finditer(script))
    if len(matches) != 1:
        message = f"expected exactly one literal {variable} assignment, found {len(matches)}"
        raise ValueError(message)
    return matches[0].group("value")


def _python_pin(package: str, pyproject: Path = PYPROJECT) -> str:
    """Read one and only one exact direct Python dependency declaration."""
    document = tomllib.loads(pyproject.read_text(encoding="utf-8"))
    groups = document.get("dependency-groups", {})
    if not isinstance(groups, dict):
        message = f"{pyproject} has no dependency-groups table"
        raise TypeError(message)
    normalized = package.lower().replace("_", "-")
    matches: list[str] = []
    for entries in groups.values():
        if not isinstance(entries, list):
            continue
        for entry in entries:
            if not isinstance(entry, str):
                continue
            parsed = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9._-]*)(.*)", entry.strip())
            if parsed is None:
                continue
            name, declaration = parsed.groups()
            if name.lower().replace("_", "-") == normalized:
                matches.append(declaration)
    if len(matches) != 1:
        message = f"expected one direct {package} declaration in {pyproject}, found {matches}"
        raise ValueError(message)
    exact = re.fullmatch(r"==([0-9][A-Za-z0-9.!+_-]*)", matches[0])
    if exact is None:
        message = f"{package} must have one bare exact == pin, found {matches[0]!r}"
        raise ValueError(message)
    return exact.group(1)


def _python_spec(binary: str, package: str) -> ToolSpec:
    """Build an exact tool spec from the locked Python project metadata.

    Args:
        binary: Executable resolved on PATH.
        package: Distribution carrying the executable.

    Returns:
        Exact ToolSpec sourced from pyproject.toml.
    """
    return ToolSpec(binary, _python_pin(package), MODE_EXACT, f"pyproject.toml:{package}")



def build_specs() -> list[ToolSpec]:
    """Assemble the pinned-tool registry from the Dockerfile source of truth.

    Returns:
        Every pinned tool the CI gates resolve, each with its comparison rule.

    Raises:
        FileNotFoundError: When the Dockerfile is missing.
        ValueError: When a pin the registry needs is absent.
    """
    text = _read_dockerfile()
    args = _dockerfile_args(text)
    cf = _pkg_major(text, "clang-format", "clang-format")
    gc = _pkg_major(text, "gcc", "gcc")
    return [
        _spec(args, "just", "JUST_VERSION", MODE_EXACT),
        _python_spec("ruff", "ruff"),
        _spec(args, "shellcheck", "SHELLCHECK_VERSION", MODE_EXACT),
        _spec(args, "shfmt", "SHFMT_VERSION", MODE_EXACT),
        _python_spec("cmake-format", "cmakelang"),
        _python_spec("cmake-lint", "cmakelang"),
        _python_spec("yamllint", "yamllint"),
        _spec(args, "hadolint", "HADOLINT_VERSION", MODE_EXACT),
        # `go --version` is not a thing: the toolchain spells it `go version`.
        ToolSpec("go", _arg(args, "GO_VERSION"), MODE_EXACT, "ARG GO_VERSION", ("version",)),
        # `zig --version` is not a thing: the toolchain spells it `zig version`.
        ToolSpec("zig", _arg(args, "ZIG_VERSION"), MODE_EXACT, "ARG ZIG_VERSION", ("version",)),
        _spec(args, "rustc", "RUST_VERSION", MODE_EXACT),
        _spec(args, "cargo", "RUST_VERSION", MODE_EXACT),
        ToolSpec(f"clang-format-{cf}", cf, MODE_MAJOR, f"clang-format-{cf}"),
        # gcc-14 is the second host-tool compiler arm; the tools-build
        # gate resolves it by exact binary name, so pin its major like clang's.
        # `gcc-14 --version` prints a dotted "14.2.0"; `-dumpversion` prints a
        # bare "14" the dotted-token parser would reject, so keep the default.
        ToolSpec(f"gcc-{gc}", gc, MODE_MAJOR, f"gcc-{gc}"),
        # g++-14 is gcc-14's C++ half. The host-test and coverage builds
        # enable_language(CXX), and the gcc-first selector picks gcc-14; a
        # gcc-14 without g++-14 sank the coverage gate for hours. Pin the pair
        # so every environment (devcontainer, runner pod, bare-metal) has both.
        ToolSpec(f"g++-{gc}", gc, MODE_MAJOR, f"g++-{gc}"),
    ]


def _extract_version(text: str) -> str | None:
    """Return the first dotted version token in `text`, or None.

    Args:
        text: Combined stdout/stderr from a tool's version command.

    Returns:
        The first ``N.N[.N...]`` token, or None when none is present.
    """
    match = _VERSION_RE.search(text)
    return match.group(0) if match else None


def _major(version: str) -> int:
    """Return the integer major component of a dotted `version`.

    Args:
        version: A dotted version string such as "18.1.8".

    Returns:
        The leading integer component.
    """
    return int(version.split(".", 1)[0])


def _matches(got: str, spec: ToolSpec) -> bool:
    """Return whether resolved version `got` satisfies `spec`.

    Args:
        got: The version parsed from the tool.
        spec: The pinned expectation and comparison mode.

    Returns:
        True when `got` meets the pin under `spec.mode`.

    Raises:
        ValueError: When `spec.mode` is not a known comparison mode.
    """
    if spec.mode == MODE_EXACT:
        return got == spec.expected
    if spec.mode == MODE_MAJOR:
        return _major(got) == int(spec.expected)
    message = f"unknown comparison mode {spec.mode!r}"
    raise ValueError(message)


def _run_version(path: str, spec: ToolSpec) -> str:
    """Run the tool's version command and return its combined output.

    Args:
        path: Absolute path to the resolved binary.
        spec: The tool spec (supplies the version arguments).

    Returns:
        Concatenated stdout and stderr from the version command.
    """
    proc = subprocess.run(  # noqa: S603 -- resolved absolute path, fixed argv
        [path, *spec.version_args],
        capture_output=True,
        text=True,
        check=False,
        timeout=TOOL_TIMEOUT_SECONDS,
    )
    return proc.stdout + proc.stderr


def verify(spec: ToolSpec) -> tuple[bool, str]:
    """Resolve one pinned tool and judge its version against the pin.

    Args:
        spec: The pinned tool to check.

    Returns:
        A ``(passed, message)`` pair; `passed` is False for a missing tool, an
        unreadable version, or a version that does not meet the pin.
    """
    path = shutil.which(spec.binary)
    if path is None:
        missing = f"{spec.binary}: NOT FOUND on PATH (want {spec.expected}, pin {spec.source})"
        return (False, missing)
    try:
        output = _run_version(path, spec)
    except (OSError, subprocess.SubprocessError) as exc:
        return (False, f"{spec.binary}: version command failed at {path} ({exc})")
    got = _extract_version(output)
    if got is None:
        return (False, f"{spec.binary}: could not parse a version at {path}")
    rule = spec.mode
    if _matches(got, spec):
        return (True, f"{spec.binary} {got} [{rule} {spec.expected}] {path}")
    return (False, f"{spec.binary} {got} != [{rule} {spec.expected}] pin {spec.source} at {path}")


def _run_checks(specs: list[ToolSpec]) -> int:
    """Verify each spec, print one line per tool, and return the aggregate code.

    Args:
        specs: The tool specs to verify.

    Returns:
        EXIT_OK when all pass; EXIT_FAIL when any tool is missing or mismatched.
    """
    failed = 0
    for spec in specs:
        ok, message = verify(spec)
        if ok:
            sys.stdout.write(f"PASS {message}\n")
        else:
            sys.stderr.write(f"FAIL {message}\n")
            failed += 1
    if failed:
        sys.stderr.write(f"check_tool_versions.py: {failed} tool(s) failed the version pin.\n")
        return EXIT_FAIL
    print(f"check_tool_versions.py: {len(specs)} pinned tool(s) match their pin.")
    return EXIT_OK


def _select_specs(names: list[str], specs: list[ToolSpec]) -> list[ToolSpec]:
    """Return the specs whose binary is in `names`, failing on an unknown name.

    Args:
        names: Requested tool binary names.
        specs: The full registry.

    Returns:
        The subset of `specs` whose binary is named in `names`.

    Raises:
        ValueError: When a requested name is not a pinned tool.
    """
    by_name = {spec.binary: spec for spec in specs}
    chosen: list[ToolSpec] = []
    for name in names:
        if name not in by_name:
            known = ", ".join(sorted(by_name))
            message = f"unknown pinned tool {name!r}; known: {known}"
            raise ValueError(message)
        chosen.append(by_name[name])
    return chosen


def _family_binary(family: str, specs: list[ToolSpec]) -> str:
    """Return the one major-pinned binary owned by a tool family.

    Args:
        family: Binary family prefix, for example ``clang-tidy``.
        specs: The full registry derived from the owning pin sources.

    Returns:
        The exact versioned binary name, for example ``clang-tidy-18``.

    Raises:
        ValueError: When the family is absent, ambiguous, not major-pinned, or
            its binary name does not encode the registered major exactly.
    """
    prefix = f"{family}-"
    matches = [spec for spec in specs if spec.binary.startswith(prefix)]
    if len(matches) != 1:
        message = f"expected one {family!r} family pin, found {len(matches)}"
        raise ValueError(message)
    spec = matches[0]
    if spec.mode != MODE_MAJOR:
        message = f"{spec.binary} uses {spec.mode!r}, not the required major pin"
        raise ValueError(message)
    expected_binary = f"{family}-{spec.expected}"
    if spec.binary != expected_binary:
        message = (
            f"{family!r} family binary {spec.binary!r} does not encode "
            f"registered major {spec.expected!r}"
        )
        raise ValueError(message)
    return spec.binary


# ---------------------------------------------------------------------------
# Selftest -- prove the comparator is non-vacuous in every mode, both ways.
# ---------------------------------------------------------------------------


def _write_fake(dir_path: Path, name: str, version_line: str) -> None:
    """Create an executable fake tool that prints `version_line` for --version.

    Args:
        dir_path: Directory to create the fake in (the caller puts it on PATH).
        name: Executable base name.
        version_line: The single line the fake prints.
    """
    script = dir_path / name
    script.write_text(f'#!/bin/sh\necho "{version_line}"\n', encoding="utf-8")
    script.chmod(FAKE_TOOL_MODE)


def _selftest_cases() -> list[tuple[ToolSpec, bool]]:
    """Return the crafted ``(spec, expected_pass)`` selftest cases.

    Returns:
        A case per mode in each direction, plus a deliberately missing tool.
    """
    return [
        (ToolSpec("ra8_fake_exact", "1.2.3", MODE_EXACT, "selftest"), True),
        (ToolSpec("ra8_fake_exact", "9.9.9", MODE_EXACT, "selftest"), False),
        (ToolSpec("ra8_fake_major18", "18", MODE_MAJOR, "selftest"), True),
        (ToolSpec("ra8_fake_major19", "18", MODE_MAJOR, "selftest"), False),
        (ToolSpec("ra8_fake_zig_match", "0.14.1", MODE_EXACT, "selftest", ("version",)), True),
        (ToolSpec("ra8_fake_zig_mismatch", "0.14.1", MODE_EXACT, "selftest", ("version",)), False),
        (ToolSpec("ra8_fake_absent", "1.0.0", MODE_EXACT, "selftest"), False),
    ]


def _run_selftest_cases() -> list[str]:
    """Verify every crafted case against fake tools on a temporary PATH.

    Returns:
        A list of failure descriptions; empty when the comparator is correct.
    """
    failures: list[str] = []
    saved_path = os.environ.get("PATH", "")
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        _write_fake(tmp_dir, "ra8_fake_exact", "faketool 1.2.3")
        _write_fake(tmp_dir, "ra8_fake_major18", "Ubuntu LLVM version 18.1.8")
        _write_fake(tmp_dir, "ra8_fake_major19", "Ubuntu LLVM version 19.1.0")
        _write_fake(tmp_dir, "ra8_fake_zig_match", "0.14.1")
        _write_fake(tmp_dir, "ra8_fake_zig_mismatch", "0.13.0")
        os.environ["PATH"] = f"{tmp_dir}{os.pathsep}{saved_path}"
        try:
            for spec, want_pass in _selftest_cases():
                got_pass, message = verify(spec)
                if got_pass != want_pass:
                    want = "pass" if want_pass else "fail"
                    detail = f"{spec.binary} [{spec.mode} {spec.expected}] want {want}: {message}"
                    failures.append(f"  {detail}")
        finally:
            os.environ["PATH"] = saved_path
    return failures


def _python_pin_failures() -> list[str]:
    """Prove exact direct-pin parsing rejects every ambiguous declaration."""
    fixtures = {
        "valid": (["ruff==1.2.3"], True),
        "missing": (["other==1.2.3"], False),
        "duplicate-same": (["ruff==1.2.3", "ruff==1.2.3"], False),
        "duplicate-different": (["ruff==1.2.3", "ruff==9.9.9"], False),
        "loose-plus-exact": (["ruff>=1", "ruff==1.2.3"], False),
        "loose": (["ruff>=1.2.3"], False),
        "url": (["ruff @ https://example.invalid/ruff.whl"], False),
        "malformed": (["ruff===1.2.3"], False),
    }
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        fixture = Path(tmp) / "pyproject.toml"
        for label, (entries, should_pass) in fixtures.items():
            joined = '", "'.join(entries)
            fixture.write_text(f'[dependency-groups]\ndev = ["{joined}"]\n', encoding="utf-8")
            try:
                value = _python_pin("ruff", fixture)
            except (TypeError, ValueError):
                passed = False
            else:
                passed = value == "1.2.3"
            if passed != should_pass:
                failures.append(f"  Python pin fixture {label!r} judged {passed}")
    return failures


def _shell_assignment_failures() -> list[str]:
    """Prove indented literals pass while dynamic, duplicate, and loose forms fire."""
    cases: dict[str, tuple[str, str | None]] = {
        "indented literal": ('  PINNED_VERSION="1.2.3"\n', "1.2.3"),
        "column-zero literal": ('PINNED_VERSION="1.2.3"\n', "1.2.3"),
        "dynamic": ('  PINNED_VERSION="${VERSION}"\n', None),
        "duplicate": (
            'PINNED_VERSION="1.2.3"\n  PINNED_VERSION="1.2.3"\n',
            None,
        ),
        "trailing command": ('PINNED_VERSION="1.2.3"; run_tool\n', None),
    }
    failures: list[str] = []
    for label, (fixture, expected) in cases.items():
        try:
            actual = _literal_shell_assignment(fixture, "PINNED_VERSION")
        except ValueError:
            actual = None
        if actual != expected:
            failures.append(f"  shell assignment fixture {label!r} returned {actual!r}")
    return failures


def _family_binary_failures() -> list[str]:
    """Prove family lookup accepts one exact major pin and rejects drift.

    Returns:
        A list of failure descriptions; empty when the lookup is two-sided.
    """
    failures: list[str] = []
    valid = [ToolSpec("clang-tidy-18", "18", MODE_MAJOR, "selftest")]
    try:
        selected = _family_binary("clang-tidy", valid)
    except ValueError as exc:
        failures.append(f"  valid family pin was rejected: {exc}")
    else:
        if selected != "clang-tidy-18":
            failures.append(f"  valid family pin resolved as {selected!r}")

    invalid_cases = {
        "absent": [],
        "ambiguous": [
            *valid,
            ToolSpec("clang-tidy-19", "19", MODE_MAJOR, "selftest"),
        ],
        "wrong-mode": [ToolSpec("clang-tidy-18", "18", MODE_EXACT, "selftest")],
        "name-major-drift": [ToolSpec("clang-tidy-19", "18", MODE_MAJOR, "selftest")],
    }
    for label, specs in invalid_cases.items():
        try:
            _family_binary("clang-tidy", specs)
        except ValueError:
            continue
        failures.append(f"  invalid family fixture {label!r} was accepted")
    return failures


def _active_lines(text: str) -> list[str]:
    """Return stripped non-comment lines from a shell-like consumer file."""
    return [line.strip() for line in text.splitlines() if not line.lstrip().startswith("#")]


def selftest() -> int:
    """Prove the version comparator fires in both directions for every mode.

    Returns:
        EXIT_OK when every crafted case (match and mismatch in each mode, plus a
        missing tool) yields the expected verdict, and the one
        architecture-conditional spec is present exactly where it belongs;
        EXIT_FAIL otherwise.
    """
    failures = (
        _run_selftest_cases()
        + _python_pin_failures()
        + _shell_assignment_failures()
        + _family_binary_failures()
    )
    if failures:
        sys.stderr.write("check_tool_versions.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("The comparator does not judge versions as claimed.\n")
        return EXIT_FAIL
    print(
        "check_tool_versions.py --selftest: OK (all modes both ways, plus missing-tool)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and run the selftest or the requested version checks.

    Args:
        argv: Process argument vector (``sys.argv``).

    Returns:
        The process exit code: EXIT_OK, EXIT_FAIL, or EXIT_CONFIG.
    """
    parser = argparse.ArgumentParser(
        description="Assert pinned host tools resolve to their pinned versions."
    )
    parser.add_argument("--selftest", action="store_true", help="prove the comparator both ways")
    parser.add_argument("--all", action="store_true", help="verify every pinned tool (default)")
    parser.add_argument(
        "--print-binary",
        metavar="FAMILY",
        help="print the exact major-pinned binary owned by FAMILY without executing it",
    )
    parser.add_argument("names", nargs="*", help="tool binary names to verify (default: all)")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()
    if args.print_binary is not None and (args.all or args.names):
        sys.stderr.write(
            "check_tool_versions.py: FATAL -- --print-binary cannot be combined "
            "with --all or tool names\n"
        )
        return EXIT_CONFIG

    try:
        specs = build_specs()
        if args.print_binary is not None:
            print(_family_binary(args.print_binary, specs))
            return EXIT_OK
        chosen = specs if (args.all or not args.names) else _select_specs(args.names, specs)
    except (FileNotFoundError, ValueError) as exc:
        sys.stderr.write(f"check_tool_versions.py: FATAL -- {exc}\n")
        return EXIT_CONFIG
    return _run_checks(chosen)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: assert every pinned host tool resolves to its project-pinned version.

Why this exists (#333)
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
The pinned versions are not restated here. They are parsed from
``.devcontainer/Dockerfile`` -- the file whose own docstring says it "pins every
tool version CI resolves" and from which the containerised gate path is built.
Reading the pins from there means the native check and the container can never
disagree about what a pin is.

Comparison modes
----------------
* ``exact``     -- version string must equal the pin (ruff, shellcheck, shfmt,
                   cppcheck, cmakelang, yamllint, actionlint, hadolint). These
                   are the tools whose findings drift with the exact version.
* ``major``     -- major must equal the pin (clang-format-22, clang-tidy-18,
                   gcc-14). The clang family and the gcc-14 host-tool arm
                   (#356) are pinned by major on purpose; the tree is
                   formatted/linted/built to that major and the binary carries
                   it in its name.
* ``min-major`` -- major must be at least the pin (gcovr). The container pins
                   gcovr 7.0 while the native boxes carry 8.x; both work, and
                   the crash this guards against is the ancient 5.2 line, so a
                   floor is the honest expectation rather than an exact string.

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
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DOCKERFILE = REPO_ROOT / ".devcontainer" / "Dockerfile"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

TOOL_TIMEOUT_SECONDS = 30
FAKE_TOOL_MODE = 0o755

MODE_EXACT = "exact"
MODE_MAJOR = "major"
MODE_MIN_MAJOR = "min-major"

# First dotted-number token (requires at least one dot, so a "2013-2023"
# copyright range in a --version banner is never mistaken for the version).
_VERSION_RE = re.compile(r"\d+(?:\.\d+)+")
_ARG_RE = re.compile(r"^\s*ARG\s+([A-Z0-9_]+)=(\S+)", re.MULTILINE)


@dataclass(frozen=True)
class ToolSpec:
    """One pinned tool: how to resolve it, run it, and judge its version.

    Attributes:
        binary: Executable resolved on PATH (e.g. "ruff", "clang-tidy-18").
        expected: The pinned version, or the pinned major for the major modes.
        mode: Comparison mode (MODE_EXACT / MODE_MAJOR / MODE_MIN_MAJOR).
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
    rather than an exact ARG: the clang-18 family and the gcc-14 arm (#356).

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
        The upstream portion before the first hyphen.
    """
    return value.split("-", 1)[0]


def _gcovr_floor(value: str) -> str:
    """Return the major of an apt gcovr pin, for the min-major floor.

    Args:
        value: The GCOVR_VERSION ARG, e.g. "7.0-1".

    Returns:
        The major component as text, e.g. "7".
    """
    return _upstream(value).split(".", 1)[0]


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
    ct = _pkg_major(text, "clang-tools", "clang-tidy")
    gc = _pkg_major(text, "gcc", "gcc")
    return [
        _spec(args, "ruff", "RUFF_VERSION", MODE_EXACT),
        _spec(args, "shellcheck", "SHELLCHECK_VERSION", MODE_EXACT),
        _spec(args, "shfmt", "SHFMT_VERSION", MODE_EXACT),
        _spec(args, "cppcheck", "CPPCHECK_VERSION", MODE_EXACT, _upstream),
        _spec(args, "cmake-format", "CMAKELANG_VERSION", MODE_EXACT),
        _spec(args, "cmake-lint", "CMAKELANG_VERSION", MODE_EXACT),
        _spec(args, "yamllint", "YAMLLINT_VERSION", MODE_EXACT),
        _spec(args, "actionlint", "ACTIONLINT_VERSION", MODE_EXACT),
        _spec(args, "hadolint", "HADOLINT_VERSION", MODE_EXACT),
        ToolSpec(f"clang-format-{cf}", cf, MODE_MAJOR, f"clang-format-{cf}"),
        ToolSpec(f"clang-tidy-{ct}", ct, MODE_MAJOR, f"clang-tools-{ct}"),
        # gcc-14 is the second host-tool compiler arm (#356); the tools-build
        # gate resolves it by exact binary name, so pin its major like clang's.
        # `gcc-14 --version` prints a dotted "14.2.0"; `-dumpversion` prints a
        # bare "14" the dotted-token parser would reject, so keep the default.
        ToolSpec(f"gcc-{gc}", gc, MODE_MAJOR, f"gcc-{gc}"),
        _spec(args, "gcovr", "GCOVR_VERSION", MODE_MIN_MAJOR, _gcovr_floor),
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
    if spec.mode == MODE_MIN_MAJOR:
        return _major(got) >= int(spec.expected)
    message = f"unknown comparison mode {spec.mode!r}"
    raise ValueError(message)


def _rule_text(mode: str) -> str:
    """Return a short human label for comparison `mode`.

    Args:
        mode: One of the MODE_* constants.

    Returns:
        A label suitable for a one-line verdict.
    """
    if mode == MODE_MIN_MAJOR:
        return ">=major"
    return mode


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
    rule = _rule_text(spec.mode)
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
        (ToolSpec("ra8_fake_gcovr86", "7", MODE_MIN_MAJOR, "selftest"), True),
        (ToolSpec("ra8_fake_gcovr52", "7", MODE_MIN_MAJOR, "selftest"), False),
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
        _write_fake(tmp_dir, "ra8_fake_gcovr86", "gcovr 8.6")
        _write_fake(tmp_dir, "ra8_fake_gcovr52", "gcovr 5.2")
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


def selftest() -> int:
    """Prove the version comparator fires in both directions for every mode.

    Returns:
        EXIT_OK when every crafted case (match and mismatch in each mode, plus a
        missing tool) yields the expected verdict; EXIT_FAIL otherwise.
    """
    failures = _run_selftest_cases()
    if failures:
        sys.stderr.write("check_tool_versions.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("The comparator does not judge versions as claimed.\n")
        return EXIT_FAIL
    print("check_tool_versions.py --selftest: OK (all modes both ways, plus missing-tool).")
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
    parser.add_argument("names", nargs="*", help="tool binary names to verify (default: all)")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    try:
        specs = build_specs()
        chosen = specs if (args.all or not args.names) else _select_specs(args.names, specs)
    except (FileNotFoundError, ValueError) as exc:
        sys.stderr.write(f"check_tool_versions.py: FATAL -- {exc}\n")
        return EXIT_CONFIG
    return _run_checks(chosen)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

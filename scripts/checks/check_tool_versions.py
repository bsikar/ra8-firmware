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
The pinned versions are not restated here. Native toolchain pins are parsed
from ``.devcontainer/Dockerfile``; Python tool pins come from the exact direct
dependencies in ``pyproject.toml`` and their transitive closure is committed in
``uv.lock``. Reading each owning source keeps native and container checks equal.

Comparison modes
----------------
* ``exact``     -- version string must equal the pin (just, ruff, shellcheck, shfmt,
                   cppcheck, cmakelang, yamllint, actionlint, hadolint, gcovr,
                   doxygen). These are the tools whose findings drift with the
                   exact version. gcovr is exact because 8.4 changed its data
                   model to retain multiple coverage records per source line,
                   which changes this tree's per-file line and branch counts.
* ``major``     -- major must equal the pin (clang-format-22, clang-tidy-18,
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

It also asserts the one spec that is not unconditional. doxygen is pinned only
where the Dockerfile installs the pinned release, so a mistake in that condition
could silently drop the tool from the registry -- and a pin nobody compares is
exactly how the deployed image sat on doxygen 1.9.8 against a 1.16.1 pin
(#522). The selftest therefore checks the spec is present on the pinned
architecture and absent on the other, in both directions.

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

# The second place the doxygen release is written down: the provisioner that
# resolves it for the `docs` gate on a host with no devcontainer image. The
# Dockerfile's own comment says to bump the two together; _assert_doxygen_pin_
# stated_once is what makes that true rather than hoped for.
DOXYGEN_PROVISIONER = REPO_ROOT / "scripts" / "builders" / "provision_doxygen.sh"

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

# The machine the Dockerfile installs the pinned doxygen release on, and the
# shell test it uses to decide. doxygen publishes no official linux-arm64
# binary, so the Dockerfile keeps apt's unpinned doxygen on every other
# architecture -- including the arm64 container a `just ci` on Apple Silicon
# builds from this same file. Asserting the pin unconditionally would therefore
# turn the container path red on every Mac.
#
# The guard string is checked against the Dockerfile rather than assumed: if
# the install stops being architecture-conditional, this spec must stop being
# conditional too, and a silent disagreement between the two is the shape of
# bug that left the doxygen pin unchecked in the first place (#522).
K_DOXYGEN_PINNED_MACHINE = "x86_64"
_DOXYGEN_ARCH_GUARD = f'"$(uname -m)" = "{K_DOXYGEN_PINNED_MACHINE}"'


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


def _assert_doxygen_pin_stated_once(args: dict[str, str]) -> None:
    """Assert the Dockerfile and provision_doxygen.sh name the same release.

    The doxygen pin is written down twice on purpose -- the Dockerfile bakes the
    release into the image, and provision_doxygen.sh resolves it for the ``docs``
    gate on hosts that have no such image -- and the Dockerfile's own comment
    says to bump them together. Nothing enforced that, so "one release, cited
    twice" was one release and a hope. A silent split would give the docs gate a
    different doxygen from the one every other tool sees, which is the same
    class of divergence this whole file exists to prevent.

    Args:
        args: Parsed Dockerfile ARG map.

    Raises:
        ValueError: When either the version or the x86_64 sha256 disagrees, or
            when the provisioner no longer states them in a readable form.
    """
    if not DOXYGEN_PROVISIONER.is_file():
        message = f"{DOXYGEN_PROVISIONER} is missing; the doxygen pin cannot be cross-checked"
        raise ValueError(message)
    script = DOXYGEN_PROVISIONER.read_text(encoding="utf-8")
    pairs = (
        ("PINNED_VERSION", "DOXYGEN_VERSION"),
        ("SHA256_LINUX_X64", "DOXYGEN_SHA256_LINUX_X64"),
    )
    for var, arg in pairs:
        try:
            value = _literal_shell_assignment(script, var)
        except ValueError as exc:
            message = f"{DOXYGEN_PROVISIONER} no longer states {var}; update {Path(__file__).name}"
            raise ValueError(message) from exc
        if value != _arg(args, arg):
            message = (
                f"doxygen pin split: {DOCKERFILE.name} ARG {arg}={_arg(args, arg)} but "
                f"{DOXYGEN_PROVISIONER.name} {var}={value}. They are one release "
                f"cited twice and must be bumped together."
            )
            raise ValueError(message)


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


def _doxygen_spec(text: str, args: dict[str, str]) -> ToolSpec | None:
    """Return the pinned-doxygen spec, or None where the Dockerfile pins none.

    The ``docs`` gate itself was never exposed by this gap -- provision_doxygen.sh
    resolves the pinned release into RA8_TOOLS_CACHE and prepends it to PATH, so
    the gate gets the pin wherever it runs. The hole was in what
    ``toolchain-parity`` asserted about the ENVIRONMENT: the deployed runner
    image sat on apt's doxygen 1.9.8 against a 1.16.1 pin for as long as it did
    because the one gate whose job is "pinned host tools match the Dockerfile"
    was not looking at that tool (#522).

    Args:
        text: The Dockerfile contents.
        args: Parsed Dockerfile ARG map.

    Returns:
        The doxygen ToolSpec on an architecture the Dockerfile pins it for,
        None otherwise.

    Raises:
        ValueError: When the Dockerfile no longer guards the install on the
            architecture this function knows about, or no longer pins the
            version at all.
    """
    # Read the pin first, so a renamed ARG fails here rather than being skipped
    # on an unpinned architecture and never noticed. Same for the cross-check:
    # a split pin is wrong on every architecture, not only the pinned one.
    spec = _spec(args, "doxygen", "DOXYGEN_VERSION", MODE_EXACT)
    _assert_doxygen_pin_stated_once(args)
    if _DOXYGEN_ARCH_GUARD not in text:
        message = (
            f"{DOCKERFILE} no longer installs the pinned doxygen under "
            f"[ {_DOXYGEN_ARCH_GUARD} ]; update {Path(__file__).name} to match "
            f"whichever architectures it now pins"
        )
        raise ValueError(message)
    if platform.machine() != K_DOXYGEN_PINNED_MACHINE:
        return None
    return spec


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
    doxygen = _doxygen_spec(text, args)
    return [
        _spec(args, "just", "JUST_VERSION", MODE_EXACT),
        _python_spec("ruff", "ruff"),
        _spec(args, "shellcheck", "SHELLCHECK_VERSION", MODE_EXACT),
        _spec(args, "shfmt", "SHFMT_VERSION", MODE_EXACT),
        _spec(args, "cppcheck", "CPPCHECK_VERSION", MODE_EXACT, _upstream),
        _python_spec("cmake-format", "cmakelang"),
        _python_spec("cmake-lint", "cmakelang"),
        _python_spec("yamllint", "yamllint"),
        _spec(args, "actionlint", "ACTIONLINT_VERSION", MODE_EXACT),
        _spec(args, "hadolint", "HADOLINT_VERSION", MODE_EXACT),
        ToolSpec(f"clang-format-{cf}", cf, MODE_MAJOR, f"clang-format-{cf}"),
        ToolSpec(f"clang-tidy-{ct}", ct, MODE_MAJOR, f"clang-tools-{ct}"),
        # gcc-14 is the second host-tool compiler arm (#356); the tools-build
        # gate resolves it by exact binary name, so pin its major like clang's.
        # `gcc-14 --version` prints a dotted "14.2.0"; `-dumpversion` prints a
        # bare "14" the dotted-token parser would reject, so keep the default.
        ToolSpec(f"gcc-{gc}", gc, MODE_MAJOR, f"gcc-{gc}"),
        # g++-14 is gcc-14's C++ half. The host-test and coverage builds
        # enable_language(CXX), and the gcc-first selector picks gcc-14; a
        # gcc-14 without g++-14 sank the coverage gate for hours. Pin the pair
        # so every environment (devcontainer, runner pod, bare-metal) has both.
        ToolSpec(f"g++-{gc}", gc, MODE_MAJOR, f"g++-{gc}"),
        _python_spec("gcovr", "gcovr"),
        # Pinned only where the Dockerfile pins it; see _doxygen_spec.
        *([doxygen] if doxygen is not None else []),
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
        A case per mode in each direction, the gcovr exact-pin regression in
        both directions, plus a deliberately missing tool.
    """
    return [
        (ToolSpec("ra8_fake_exact", "1.2.3", MODE_EXACT, "selftest"), True),
        (ToolSpec("ra8_fake_exact", "9.9.9", MODE_EXACT, "selftest"), False),
        (ToolSpec("ra8_fake_major18", "18", MODE_MAJOR, "selftest"), True),
        (ToolSpec("ra8_fake_major19", "18", MODE_MAJOR, "selftest"), False),
        (ToolSpec("ra8_fake_gcovr70", "7.0", MODE_EXACT, "selftest"), True),
        (ToolSpec("ra8_fake_gcovr86", "7.0", MODE_EXACT, "selftest"), False),
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
        _write_fake(tmp_dir, "ra8_fake_gcovr70", "gcovr 7.0")
        _write_fake(tmp_dir, "ra8_fake_gcovr86", "gcovr 8.6")
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


def _gcovr_registry_failures() -> list[str]:
    """Verify the live gcovr spec is the exact uv-project package pin.

    Returns:
        A list of failure descriptions; empty when the registry enforces the
        pyproject.toml direct version exactly.
    """
    raw_pin = _python_pin("gcovr")
    specs = [spec for spec in build_specs() if spec.binary == "gcovr"]
    if len(specs) != 1:
        return [f"  expected one gcovr spec, found {len(specs)}"]
    spec = specs[0]
    failures: list[str] = []
    if spec.mode != MODE_EXACT:
        failures.append(f"  gcovr uses {spec.mode!r}, not exact comparison")
    if spec.expected != raw_pin:
        failures.append(f"  gcovr expects {spec.expected!r}, not uv project pin {raw_pin!r}")
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


def _arch_conditional_failures() -> list[str]:
    """Verify the doxygen spec appears exactly where the Dockerfile pins it.

    The registry is otherwise unconditional, so this one spec is the only place
    a mistake could silently drop a pin from the gate -- which is the state that
    let a 1.9.8-against-1.16.1 drift survive in the deployed image (#522). Assert
    both directions rather than trusting the condition.

    Returns:
        A list of failure descriptions; empty when the spec is conditional as
        documented.
    """
    failures: list[str] = []
    text = _read_dockerfile()
    args = _dockerfile_args(text)
    with patch.object(platform, "machine", return_value=K_DOXYGEN_PINNED_MACHINE):
        spec = _doxygen_spec(text, args)
        if spec is None or spec.binary != "doxygen":
            failures.append(
                f"  no doxygen spec on {K_DOXYGEN_PINNED_MACHINE}, "
                f"where the Dockerfile installs the pinned release"
            )
    with patch.object(platform, "machine", return_value="aarch64"):
        if _doxygen_spec(text, args) is not None:
            failures.append(
                "  a doxygen spec on aarch64, where the Dockerfile deliberately "
                "leaves apt's unpinned doxygen in place (no official arm64 build)"
            )
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


def _tidy_consumer_findings(just_text: str, gate_text: str, direct_text: str) -> list[str]:
    """Validate all three clang-tidy consumers use the registry query.

    Args:
        just_text: Contents of ``just/ci.just``.
        gate_text: Contents of the CI analysis gate body.
        direct_text: Contents of the direct clang-tidy driver.

    Returns:
        Stable finding identifiers; empty only for the required consumer shape.
    """
    just_lines = _active_lines(just_text)
    gate_lines = _active_lines(gate_text)
    direct_active = "\n".join(_active_lines(direct_text))
    findings: list[str] = []
    just_query = (
        "export CLANG_TIDY := env('CLANG_TIDY', `python3 "
        "scripts/checks/check_tool_versions.py --print-binary clang-tidy`)"
    )
    if just_lines.count(just_query) != 1:
        findings.append("just-query")
    gate_query = (
        'pinned_tidy="$(python3 scripts/checks/check_tool_versions.py --print-binary clang-tidy)"'
    )
    gate_require = 'require_tool_versions "$pinned_tidy"'
    gate_selftest = 'CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh --selftest'
    gate_check = (
        'CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh '
        '--check --verbose >"$log" 2>&1 || rc=$?'
    )
    gate_required = (gate_query, gate_require, gate_selftest, gate_check)
    if any(gate_lines.count(line) != 1 for line in gate_required):
        findings.append("gate-query-or-consumer")
    direct_query = re.compile(
        r'if ! RA8_PINNED_CLANG_TIDY="\$\(\n\s*python3 '
        r'"\$SCRIPT_DIR/check_tool_versions\.py" --print-binary clang-tidy\n\s*\)"; then'
    )
    if len(direct_query.findall(direct_active)) != 1:
        findings.append("direct-query")
    for label, active in (("just", just_lines), ("gate", gate_lines), ("direct", direct_active)):
        joined = "\n".join(active) if isinstance(active, list) else active
        if re.search(r"\bclang-tidy-[0-9]+\b", joined):
            findings.append(f"{label}-hardcoded-major")
    return findings


def _tidy_consumer_failures() -> list[str]:
    """Prove live and fixture consumers bind to the version registry."""
    valid_just = (
        "export CLANG_TIDY := env('CLANG_TIDY', `python3 "
        "scripts/checks/check_tool_versions.py --print-binary clang-tidy`)\n"
    )
    gate_query = (
        'pinned_tidy="$(python3 scripts/checks/check_tool_versions.py --print-binary clang-tidy)"'
    )
    gate_require = 'require_tool_versions "$pinned_tidy"'
    gate_selftest = 'CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh --selftest'
    gate_check = (
        'CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh '
        '--check --verbose >"$log" 2>&1 || rc=$?'
    )
    valid_gate = f"{gate_query}\n{gate_require}\n{gate_selftest}\n{gate_check}"
    valid_direct = (
        'if ! RA8_PINNED_CLANG_TIDY="$(\n'
        '  python3 "$SCRIPT_DIR/check_tool_versions.py" --print-binary clang-tidy\n'
        ')"; then\n'
    )
    failures: list[str] = []
    if _tidy_consumer_findings(valid_just, valid_gate, valid_direct):
        failures.append("  valid clang-tidy consumer fixture was rejected")
    query_command = "python3 scripts/checks/check_tool_versions.py --print-binary clang-tidy"
    mutations = {
        "just hardcode": (
            valid_just.replace(query_command, "echo clang-tidy-18"),
            valid_gate,
            valid_direct,
        ),
        "gate hardcode": (
            valid_just,
            valid_gate.replace(f"$({query_command})", "clang-tidy-18"),
            valid_direct,
        ),
        "gate bypass": (
            valid_just,
            valid_gate.replace(gate_require, "require_tool_versions clang-tidy-18"),
            valid_direct,
        ),
        "direct hardcode": (
            valid_just,
            valid_gate,
            'RA8_PINNED_CLANG_TIDY="clang-tidy-18"\n',
        ),
    }
    for label, fixture in mutations.items():
        if not _tidy_consumer_findings(*fixture):
            failures.append(f"  clang-tidy consumer mutation {label!r} was accepted")
    live = (
        (REPO_ROOT / "just/ci.just").read_text(encoding="utf-8"),
        (REPO_ROOT / "scripts/ci/gates/analysis.sh").read_text(encoding="utf-8"),
        (REPO_ROOT / "scripts/checks/clang_tidy.sh").read_text(encoding="utf-8"),
    )
    failures.extend(
        f"  live clang-tidy consumer: {item}" for item in _tidy_consumer_findings(*live)
    )
    return failures


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
        + _gcovr_registry_failures()
        + _python_pin_failures()
        + _shell_assignment_failures()
        + _arch_conditional_failures()
        + _family_binary_failures()
        + _tidy_consumer_failures()
    )
    if failures:
        sys.stderr.write("check_tool_versions.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("The comparator does not judge versions as claimed.\n")
        return EXIT_FAIL
    print(
        "check_tool_versions.py --selftest: OK (all modes and the gcovr exact "
        "pin both ways, plus missing-tool and the arch-conditional doxygen pin)."
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

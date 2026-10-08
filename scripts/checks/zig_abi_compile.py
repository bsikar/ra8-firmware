# SPDX-License-Identifier: Apache-2.0
"""Compile and archive rules for the Zig ABI policy checker.

Everything that reaches a toolchain lives here: building the C argument
list for a target, compiling a translation unit per build mode, reading
symbols out of an archive with nm, and the mode-coverage rules that say
which of those have to be exercised.

Separated from :mod:`check_zig_abi_policy` because this is the only part
of the checker that shells out. Keeping it behind one import makes the
boundary between reading the repository and running the toolchain
visible, and it imports strictly downward, so the checker calls into it
and never the other way.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]

MIN_NM_SYMBOL_FIELDS = 3

# nm's archive form, "archive.a:member.o:address", carries two colons.
MIN_NM_MEMBER_COLONS = 2

MODE_C_FLAGS = {"Debug": "-O0", "ReleaseSafe": "-O2", "ReleaseSmall": "-Oz"}

RA8_C_ARGUMENTS = ("-mcpu=cortex_m85", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")


def _c_target_arguments(target: str, arguments: list[str]) -> list[str]:
    """Translate registered Zig target selections to Zig C compiler flags."""
    if target == "ra8":
        return ["-target", "thumb-freestanding-eabihf", *RA8_C_ARGUMENTS]
    translated: list[str] = []
    for argument in arguments:
        if argument.startswith("-Dtarget="):
            translated.extend(("-target", argument.removeprefix("-Dtarget=")))
        elif argument.startswith("-Dcpu="):
            cpu = argument.removeprefix("-Dcpu=")
            translated.append(f"-mcpu={cpu}")
    return translated


def _c_compile_findings(
    library: dict[str, Any],
    zig: str,
    job: tuple[str, list[str], str],
    output: Path,
    repository_root: Path = ROOT,
) -> list[str]:
    """Compile the public C header under the same target and optimization mode."""
    target, _arguments, mode = job
    includes = library.get("c_include_dirs")
    if not isinstance(includes, list) or not includes:
        return [f"{library['name']}: missing C include directories"]
    probe = output / "abi_header_probe.c"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text(f'#include "{Path(library["public_header"]).name}"\n', encoding="utf-8")
    command = [
        zig,
        "cc",
        "-std=c2x",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-c",
        MODE_C_FLAGS[mode],
        *_c_target_arguments(target, library["targets"][target]),
        *(item for include in includes for item in ("-I", str(repository_root / include))),
        str(probe),
        "-o",
        str(output / "abi_header_probe.o"),
    ]
    proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy arguments
        command, cwd=repository_root, capture_output=True, text=True, check=False
    )
    if proc.returncode == 0:
        return []
    detail = (proc.stdout + proc.stderr).strip()
    return [f"{library['name']}: {target}/{mode} C header compile failed: {detail}"]


def _matrix_jobs(
    library: dict[str, Any], required_modes: set[str]
) -> list[tuple[str, list[str], str]]:
    """Return the complete deterministic target-by-mode build matrix."""
    return [
        (target, arguments, mode)
        for target, arguments in library["targets"].items()
        for mode in sorted(required_modes)
    ]


def _archive_symbols(
    nm: str, archive: Path, name: str, *, bundle_compiler_rt: bool = False
) -> tuple[list[str], set[str]]:
    """Read one archive's global defined symbols with the resolved nm tool."""
    if not archive.is_file():
        return [f"{name} archive missing: {archive}"], set()
    proc = subprocess.run(  # noqa: S603 -- resolved tool; fresh archive
        [nm, "-A", "-g", "--defined-only", str(archive)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return [f"{name} symbol scan failed: {proc.stderr.strip()}"], set()
    return [], _archive_symbol_names(proc.stdout, bundle_compiler_rt=bundle_compiler_rt)


def _archive_symbol_names(output: str, *, bundle_compiler_rt: bool = False) -> set[str]:
    """Parse GNU/LLVM nm -A output, excluding only bundled compiler runtime members."""
    actual: set[str] = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < MIN_NM_SYMBOL_FIELDS:
            continue
        # GNU nm -A emits archive(member): before the symbol fields. Ignore only
        # the compiler runtime member supplied explicitly by Zig's build, never
        # similarly named symbols from the library's own object files.
        member_match = re.search(r"[\[(]([^\)\]]+)[\)\]]", parts[0])
        member = member_match.group(1) if member_match else ""
        if not member and parts[0].count(":") >= MIN_NM_MEMBER_COLONS:
            archive_and_member, _address = parts[0].rsplit(":", 1)
            _archive, member = archive_and_member.split(":", 1)
        if bundle_compiler_rt and Path(member).name == "compiler_rt.o":
            continue
        actual.add(parts[-1].removeprefix("_"))
    return actual


def _host_mode_test_findings(
    library: dict[str, Any], command: list[str], mode: str, repository_root: Path
) -> tuple[list[str], int]:
    """Run the canonical host ABI tests in one optimization mode when required."""
    if library.get("run_host_tests_in_all_modes") is not True:
        return [], 0
    proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy
        [*command, "test"], cwd=repository_root, capture_output=True, text=True, check=False
    )
    if proc.returncode == 0:
        return [], 1
    detail = (proc.stdout + proc.stderr).strip()
    return [f"{library['name']}: host/{mode} ABI tests failed: {detail}"], 0


def _compiled_findings(
    library: dict[str, Any],
    zig: str,
    nm: str,
    required_modes: set[str],
    repository_root: Path = ROOT,
) -> tuple[list[str], dict[str, int]]:
    """Build every registered target/mode and compare all global exports exactly."""
    name = library["name"]
    findings: list[str] = []
    counts = {"c_matrix": 0, "zig_matrix": 0, "mode_tests": 0}
    with tempfile.TemporaryDirectory(prefix="ra8-zig-abi-") as tmp:
        for target, arguments, mode in _matrix_jobs(library, required_modes):
            output = Path(tmp) / target / mode
            c_findings = _c_compile_findings(
                library, zig, (target, arguments, mode), output, repository_root
            )
            findings.extend(c_findings)
            counts["c_matrix"] += int(not c_findings)
            command = [
                zig,
                "build",
                "--build-file",
                str(repository_root / library["build_root"] / "build.zig"),
                "--prefix",
                str(output / "install"),
                "--cache-dir",
                str(Path(tmp) / "cache"),
                *arguments,
                f"-Doptimize={mode}",
            ]
            # Zig 0.17's build runner takes the global cache only from the environment.
            environment = {**os.environ, "ZIG_GLOBAL_CACHE_DIR": str(Path(tmp) / "global-cache")}
            proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy arguments
                command,
                cwd=repository_root,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                detail = (proc.stdout + proc.stderr).strip()
                findings.append(f"{name}: {target}/{mode} archive build failed: {detail}")
                continue
            archive = output / "install" / "lib" / f"lib{library['library_name']}.a"
            build_source = (repository_root / library["build_root"] / "build.zig").read_text(
                encoding="utf-8"
            )
            bundle_compiler_rt = bool(
                re.search(r"\blibrary\.bundle_compiler_rt\s*=\s*true\s*;", build_source)
            )
            symbol_findings, actual = _archive_symbols(
                nm,
                archive,
                f"{name}: {target}/{mode}",
                bundle_compiler_rt=bundle_compiler_rt,
            )
            findings.extend(symbol_findings)
            if symbol_findings:
                continue
            expected = {row["name"] for row in library["exports"]} | {
                row["name"]
                for row in library.get("data_exports", [])
                if isinstance(row, dict) and isinstance(row.get("name"), str)
            }
            findings.extend(_compiled_symbol_findings(f"{name}: {target}/{mode}", expected, actual))
            counts["zig_matrix"] += 1
            if target == "host":
                test_findings, passed = _host_mode_test_findings(
                    library, command, mode, repository_root
                )
                findings.extend(test_findings)
                counts["mode_tests"] += passed
    return findings, counts


def _compiled_symbol_findings(name: str, expected: set[str], actual: set[str]) -> list[str]:
    """Report both directions of drift in a compiled archive's public symbols."""
    findings: list[str] = []
    if expected - actual:
        missing = ", ".join(sorted(expected - actual))
        findings.append(f"{name}: compiled archive missing export(s): {missing}")
    if actual - expected:
        unexpected = ", ".join(sorted(actual - expected))
        findings.append(f"{name}: compiled archive unexpected export(s): {unexpected}")
    return findings


def _mode_test_policy_findings(
    policy: dict[str, Any], libraries: list[dict[str, Any]]
) -> list[str]:
    """Require one named canonical fixture to run tests in every host mode."""
    selected = policy.get("mode_test_library")
    matches = [library for library in libraries if library.get("name") == selected]
    if not isinstance(selected, str) or len(matches) != 1:
        return ["policy must name exactly one mode_test_library"]
    library = matches[0]
    if library.get("run_host_tests_in_all_modes") is not True:
        return [f"{selected}: run_host_tests_in_all_modes must be true"]
    if "host" not in library.get("targets", {}):
        return [f"{selected}: mode-test library must support host"]
    return []

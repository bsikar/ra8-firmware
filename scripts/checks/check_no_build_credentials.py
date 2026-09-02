#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject build-time ingestion or embedding of bench network credentials."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import isolated_git_environment, trusted_git_executable

CMAKE_TOKENS = (
    "RA8_C6_WIFI_SSID",
    "RA8_C6_WIFI_PSK",
    "RA8_MEDIA_DOWNLOAD_URL",
    "openbao_client.py",
    "wifi.env",
)
SOURCE_TOKENS = (
    "RA8_C6_WIFI_SSID",
    "RA8_C6_WIFI_PSK",
    "RA8_MEDIA_DOWNLOAD_URL",
)
SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".h", ".hh", ".hpp", ".hxx"))
LEGACY_SCRIPT_PATTERNS = (
    re.compile(r"load_c6_wifi_env\.py"),
    re.compile(r"(?:source|\.)[^\n]*wifi\.env"),
    re.compile(
        r"openbao_client\.py(?:(?:\\\r?\n)|[^\n])*\bget\b"
        r"(?:(?:\\\r?\n)|[^\n])*\bbench-network\b"
    ),
)
JUST_CREDENTIAL_PATTERNS = tuple(
    re.compile(pattern)
    for token in SOURCE_TOKENS
    for pattern in (
        rf"(?:^|[\s;]){re.escape(token)}\s*=",
        rf"\$(?:\{{{re.escape(token)}(?:\}}|[:?+\-])|{re.escape(token)}\b)",
    )
)
MIN_CMAKE_FILES = 200
MIN_SOURCE_FILES = 400
MIN_SCRIPT_FILES = 40
MIN_JUST_FILES = 20
MIN_SCOPED_PARTS = 2


def _is_source(path: PurePosixPath) -> bool:
    """Return whether a path is first-party app/example C-family source."""
    return path.suffix.lower() in SOURCE_SUFFIXES and path.parts[:1] in (
        ("apps",),
        ("examples",),
    )


def _is_scoped_script(path: PurePosixPath) -> bool:
    """Return whether a path is build/HIL automation covered by this gate."""
    return (
        len(path.parts) >= MIN_SCOPED_PARTS
        and path.parts[0] == "scripts"
        and path.parts[1] in {"builders", "hil"}
    )


def _is_just_entry(path: PurePosixPath) -> bool:
    """Return whether a path is a repository Just command entry point."""
    return path.name == "justfile" or (path.parts[:1] == ("just",) and path.suffix == ".just")


def findings_for(path: PurePosixPath, text: str) -> list[str]:
    """Return forbidden build-credential mechanisms found in one file."""
    findings: list[str] = []
    is_cmake = path.name == "CMakeLists.txt" or path.suffix == ".cmake"
    if is_cmake:
        findings.extend(
            f"build configuration consumes {token}" for token in CMAKE_TOKENS if token in text
        )
    if _is_source(path):
        findings.extend(
            f"firmware source embeds {token}" for token in SOURCE_TOKENS if token in text
        )
    if _is_scoped_script(path) or _is_just_entry(path):
        findings.extend(
            "automation uses a legacy build-time credential path"
            for pattern in LEGACY_SCRIPT_PATTERNS
            if pattern.search(text)
        )
    if _is_just_entry(path):
        findings.extend(
            "Just build entry point expands or assigns a credential variable"
            for pattern in JUST_CREDENTIAL_PATTERNS
            if pattern.search(text)
        )
    return findings


def tracked_files(root: Path) -> list[PurePosixPath]:
    """Return present tracked and untracked files, excluding ignored artifacts."""
    git = trusted_git_executable()
    result = subprocess.run(  # noqa: S603 - executable resolved from PATH above
        [git, "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return [PurePosixPath(line) for line in result.stdout.splitlines() if (root / line).is_file()]


def inspect_tree(
    root: Path,
    minimums: tuple[int, int, int, int] = (
        MIN_CMAKE_FILES,
        MIN_SOURCE_FILES,
        MIN_SCRIPT_FILES,
        MIN_JUST_FILES,
    ),
) -> tuple[list[str], tuple[int, int, int, int]]:
    """Return violations and scope counts after scanning one Git worktree."""
    violations: list[str] = []
    cmake_count = 0
    source_count = 0
    script_count = 0
    just_count = 0
    for path in tracked_files(root):
        is_cmake = path.name == "CMakeLists.txt" or path.suffix == ".cmake"
        is_source = _is_source(path)
        is_script = _is_scoped_script(path)
        is_just = _is_just_entry(path)
        if not (is_cmake or is_source or is_script or is_just):
            continue
        cmake_count += int(is_cmake)
        source_count += int(is_source)
        script_count += int(is_script)
        just_count += int(is_just)
        try:
            text = (root / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            violations.append(f"{path}: cannot inspect file: {exc}")
            continue
        violations.extend(f"{path}: {finding}" for finding in findings_for(path, text))
    min_cmake, min_source, min_script, min_just = minimums
    if cmake_count < min_cmake:
        violations.append(
            f"scan reached only {cmake_count} CMake files; expected at least {min_cmake}"
        )
    if source_count < min_source:
        violations.append(
            "scan reached only "
            f"{source_count} app/example source files; expected at least {min_source}"
        )
    if script_count < min_script:
        violations.append(
            f"scan reached only {script_count} build/HIL scripts; expected at least {min_script}"
        )
    if just_count < min_just:
        violations.append(
            f"scan reached only {just_count} Just entry points; expected at least {min_just}"
        )
    return violations, (cmake_count, source_count, script_count, just_count)


def check_tree(root: Path) -> int:
    """Scan the live tree and enforce non-vacuity floors for every scope."""
    violations, counts = inspect_tree(root)
    if violations:
        for violation in violations:
            print(f"check_no_build_credentials.py: {violation}", file=sys.stderr)
        return 1
    cmake_count, source_count, script_count, just_count = counts
    print(
        "check_no_build_credentials.py: "
        f"{cmake_count} CMake + {source_count} app/example source + "
        f"{script_count} build/HIL script + {just_count} Just files are credential-free"
    )
    return 0


def _write_selftest_file(root: Path, relative: str, text: str) -> None:
    """Create one selftest fixture inside a temporary Git worktree."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _selftest_cases() -> tuple[tuple[PurePosixPath, str, bool], ...]:
    """Return direct must-fire and must-stay-quiet detection cases."""
    return (
        (
            PurePosixPath("examples/board/app/CMakeLists.txt"),
            'target_compile_definitions(app PRIVATE RA8_C6_WIFI_PSK="secret")',
            True,
        ),
        (
            PurePosixPath("examples/board/app/src/main.c"),
            "static const char psk[] = RA8_C6_WIFI_PSK;",
            True,
        ),
        (
            PurePosixPath("scripts/hil/run.sh"),
            "python3 scripts/secrets/openbao_client.py \\\n"
            "  get secret/ra8d2/bench-network bench_psk",
            True,
        ),
        (
            PurePosixPath("examples/board/app/src/main.mm"),
            "static const char psk[] = RA8_C6_WIFI_PSK;",
            True,
        ),
        (
            PurePosixPath("just/apps.just"),
            'build:\n    RA8_C6_WIFI_PSK="$RA8_C6_WIFI_PSK" cmake --build build',
            True,
        ),
        (
            PurePosixPath("examples/board/app/CMakeLists.txt"),
            "target_sources(app PRIVATE src/main.c)",
            False,
        ),
        (
            PurePosixPath("examples/board/app/src/main.c"),
            'puts("ra8_net_provision: READY v1");',
            False,
        ),
        (
            PurePosixPath("scripts/hil/run.sh"),
            "python3 scripts/secrets/wifi_provision.py emit | ssh bench cat",
            False,
        ),
    )


def _case_selftest_failures() -> list[str]:
    """Return failures from the direct detection cases."""
    failures = []
    for path, text, should_fire in _selftest_cases():
        fired = bool(findings_for(path, text))
        if fired != should_fire:
            failures.append(f"{path}: expected fired={should_fire}, got {fired}")
    return failures


def _seed_selftest_worktree(root: Path, git: str) -> None:
    """Create and track the clean end-to-end selftest fixture set."""
    subprocess.run(  # noqa: S603 -- resolved Git executable and fixed fixture argv
        [git, "init", "-q"], cwd=root, check=True
    )
    _write_selftest_file(root, "CMakeLists.txt", "project(selftest C)\n")
    _write_selftest_file(
        root,
        "examples/board/app/src/main.cpp",
        "int main() { return 0; }\n",
    )
    _write_selftest_file(root, "scripts/hil/run.sh", "#!/usr/bin/env bash\ntrue\n")
    _write_selftest_file(root, "just/apps.just", "build:\n    true\n")
    subprocess.run(  # noqa: S603 -- resolved Git executable and fixed fixture argv
        [git, "add", "."], cwd=root, check=True
    )


def _worktree_selftest_failures(git: str) -> list[str]:
    """Return failures from tracked-file, untracked-file, and floor checks."""
    failures = []
    with tempfile.TemporaryDirectory() as name:
        root = Path(name)
        _seed_selftest_worktree(root, git)
        clean_findings, clean_counts = inspect_tree(root, (1, 1, 1, 1))
        if clean_findings or clean_counts != (1, 1, 1, 1):
            failures.append("end-to-end clean worktree or scope counts failed")

        _write_selftest_file(
            root,
            "examples/board/app/src/leak.hxx",
            "#define RA8_C6_WIFI_PSK secret\n",
        )
        leak_findings, _ = inspect_tree(root, (1, 1, 1, 1))
        if not any("leak.hxx" in finding for finding in leak_findings):
            failures.append("end-to-end untracked C++ header leak was missed")

        floor_findings, _ = inspect_tree(root, (2, 3, 2, 2))
        floor_scopes = (
            "CMake files",
            "source files",
            "build/HIL scripts",
            "Just entry points",
        )
        if not all(any(scope in finding for finding in floor_findings) for scope in floor_scopes):
            failures.append("end-to-end non-vacuity floors did not all fire")
    return failures


def _run_selftest_body() -> int:
    """Prove legacy embedding fires while runtime provisioning stays quiet."""
    failures = _case_selftest_failures()

    git = trusted_git_executable()
    failures.extend(_worktree_selftest_failures(git))
    if failures:
        for failure in failures:
            print(f"check_no_build_credentials.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        "check_no_build_credentials.py --selftest: PASS "
        "(5 must-fire, 3 must-stay-quiet, tracked-file and floor cases)"
    )
    return 0


def run_selftest() -> int:
    """Run temporary-repository cases without inheriting the caller's repo."""
    with isolated_git_environment():
        return _run_selftest_body()


def main() -> int:
    """Parse arguments and run either the selftest or live scan."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()
    return check_tree(Path(__file__).resolve().parents[2])


if __name__ == "__main__":
    sys.exit(main())

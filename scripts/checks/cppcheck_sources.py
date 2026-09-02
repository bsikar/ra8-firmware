#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Produce and validate cppcheck inputs from an isolated Git census.

Cppcheck must receive explicit candidate-tree translation units. Directory
operands recursively ingest ignored in-tree build output, while an inherited
Git index/configuration or Python startup hook can silently replace a naive
manifest. This module is the single cppcheck source-scope authority consumed
by both the registered gate and the advisory wrapper.
"""

from __future__ import annotations

import argparse
import io
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import BinaryIO, TextIO

# Isolated Python deliberately omits the script directory. Add only the two
# repository-owned module roots this checker imports; inherited PYTHONPATH and
# sitecustomize remain unavailable under `-I -S`.
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

import lint_targets
from git_environment import (
    sanitized_git_environment,
    trusted_git_executable,
)

SOURCE_ROOTS = ("libs/", "examples/", "tools/")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")
MIN_SOURCE_UNITS = 879


class CensusError(RuntimeError):
    """Raised when the source census or transported manifest is untrustworthy."""

    @classmethod
    def git_failed(cls, detail: str) -> CensusError:
        """Build the error for a failed Git census command."""
        return cls(f"git ls-files failed: {detail or 'no diagnostic'}")

    @classmethod
    def invalid_encoding(cls) -> CensusError:
        """Build the error for bytes outside the repository path encoding."""
        return cls("source manifest contains a non-UTF-8 path")

    @classmethod
    def collapsed(cls, count: int, minimum: int) -> CensusError:
        """Build the error for a census below its authenticated floor."""
        return cls(f"cppcheck source census has {count} unit(s); floor is {minimum}")

    @classmethod
    def unsafe_path(cls, rel: str, reason: str) -> CensusError:
        """Build the error for a path outside the regular-file boundary."""
        return cls(f"unsafe cppcheck source path {rel!r}: {reason}")

    @classmethod
    def invalid_manifest(cls, reason: str) -> CensusError:
        """Build the error for malformed transported bytes."""
        return cls(f"invalid cppcheck source manifest: {reason}")

    @classmethod
    def duplicate_manifest(cls) -> CensusError:
        """Build the error for duplicate transported paths."""
        return cls.invalid_manifest("duplicate paths")

    @classmethod
    def unsorted_manifest(cls) -> CensusError:
        """Build the error for a nondeterministic transported order."""
        return cls.invalid_manifest("paths are not deterministically sorted")

    @classmethod
    def missing_terminator(cls) -> CensusError:
        """Build the error for a truncated NUL manifest."""
        return cls.invalid_manifest("missing final NUL terminator")

    @classmethod
    def empty_manifest_field(cls) -> CensusError:
        """Build the error for an empty path within a manifest."""
        return cls.invalid_manifest("empty path field")

    @classmethod
    def unsafe_transport(cls) -> CensusError:
        """Build the error for a non-regular manifest transport."""
        return cls.invalid_manifest("transport is not a regular file")


def trusted_python_executable() -> str:
    """Return the fixed Python authority shared by the shell adapter."""
    expected = "/usr/bin/python3"
    configured = os.environ.get("RA8_TRUSTED_PYTHON", expected)
    if configured != expected:
        raise CensusError.unsafe_path(configured, "not the trusted Python executable")
    path = Path(expected)
    try:
        path.lstat()
    except OSError as error:
        raise CensusError.unsafe_path(expected, "unavailable") from error
    resolved = path.resolve(strict=True)
    resolved_info = resolved.lstat()
    if not stat.S_ISREG(resolved_info.st_mode) or not os.access(resolved, os.X_OK):
        raise CensusError.unsafe_path(expected, "does not resolve to a regular executable")
    return expected


def git_paths(root: Path) -> list[str]:
    """Return tracked and non-ignored candidate paths through sanitized Git."""
    process = subprocess.run(  # noqa: S603 -- fixed executable from shared Git authority.
        [
            trusted_git_executable(),
            "-C",
            str(root),
            "--work-tree",
            str(root),
            "-c",
            "core.excludesFile=/dev/null",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=root,
        env=sanitized_git_environment(),
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        detail = os.fsdecode(process.stderr).strip()
        raise CensusError.git_failed(detail)
    try:
        decoded = process.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CensusError.invalid_encoding() from error
    return [rel for rel in decoded.split("\0") if rel]


def lexical_parts(rel: str) -> tuple[str, ...]:
    """Return safe POSIX path components or reject lexical escape syntax."""
    raw_parts = rel.split("/")
    pure = PurePosixPath(rel)
    if pure.is_absolute() or not raw_parts or any(part in {"", ".", ".."} for part in raw_parts):
        raise CensusError.unsafe_path(rel, "path is not a confined repository-relative name")
    return tuple(raw_parts)


def is_cppcheck_unit(rel: str, root: Path) -> bool:
    """Return whether one safe repository path is an in-scope translation unit."""
    return (
        rel.startswith(SOURCE_ROOTS)
        and rel.endswith(SOURCE_SUFFIXES)
        and lint_targets.language_of(rel, root) == "c"
    )


def regular_source_exists(root: Path, rel: str) -> bool:
    """Require a non-symlink regular file under non-symlink repo directories."""
    parts = lexical_parts(rel)
    root_resolved = root.resolve(strict=True)
    current = root_resolved
    for index, part in enumerate(parts):
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            return False
        if stat.S_ISLNK(info.st_mode):
            raise CensusError.unsafe_path(rel, "symlink component is forbidden")
        if index < len(parts) - 1 and not stat.S_ISDIR(info.st_mode):
            raise CensusError.unsafe_path(rel, "parent component is not a directory")
        if index == len(parts) - 1 and not stat.S_ISREG(info.st_mode):
            raise CensusError.unsafe_path(rel, "source is not a regular file")
    try:
        current.resolve(strict=True).relative_to(root_resolved)
    except (OSError, ValueError) as error:
        raise CensusError.unsafe_path(rel, "resolved path escapes the repository") from error
    return True


def validate_sources(root: Path, sources: list[str], minimum: int) -> list[str]:
    """Validate order, uniqueness, scope, confinement, and anti-vacuity."""
    if len(sources) != len(set(sources)):
        raise CensusError.duplicate_manifest()
    if sources != sorted(sources):
        raise CensusError.unsorted_manifest()
    if len(sources) < minimum:
        raise CensusError.collapsed(len(sources), minimum)
    for rel in sources:
        lexical_parts(rel)
        if not is_cppcheck_unit(rel, root):
            raise CensusError.unsafe_path(rel, "outside authenticated roots or suffixes")
        if not regular_source_exists(root, rel):
            raise CensusError.unsafe_path(rel, "source disappeared before validation")
    return sources


def collect_sources(root: Path, minimum: int) -> list[str]:
    """Collect sorted regular sources and reject unsafe candidate entries."""
    sources: list[str] = []
    for rel in git_paths(root):
        lexical_parts(rel)
        if not is_cppcheck_unit(rel, root):
            continue
        if regular_source_exists(root, rel):
            sources.append(rel)
    return validate_sources(root, sorted(sources), minimum)


def decode_manifest(data: bytes) -> list[str]:
    """Decode one canonical NUL-terminated path manifest."""
    if not data or not data.endswith(b"\0"):
        raise CensusError.missing_terminator()
    fields = data[:-1].split(b"\0")
    if any(not field for field in fields):
        raise CensusError.empty_manifest_field()
    try:
        return [field.decode("utf-8") for field in fields]
    except UnicodeDecodeError as error:
        raise CensusError.invalid_encoding() from error


def emit_sources(sources: list[str], output: BinaryIO, nul_terminated: bool) -> None:
    """Write a path manifest without shell re-tokenization."""
    separator = b"\0" if nul_terminated else b"\n"
    output.write(separator.join(path.encode("utf-8") for path in sources) + separator)


def run_census(
    root: Path,
    minimum: int,
    output: BinaryIO,
    error_output: TextIO,
    nul_terminated: bool,
) -> int:
    """Run the production census and return a process-style status."""
    try:
        sources = collect_sources(root, minimum)
    except CensusError as error:
        print(f"cppcheck_sources.py: FATAL -- {error}", file=error_output)
        return 2
    emit_sources(sources, output, nul_terminated)
    return 0


def run_manifest_validation(
    root: Path,
    manifest: Path,
    output: BinaryIO,
    error_output: TextIO,
    nul_terminated: bool,
) -> int:
    """Validate transported bytes independently and emit a normalized manifest."""
    try:
        info = manifest.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
            raise CensusError.unsafe_transport()
        sources = validate_sources(root, decode_manifest(manifest.read_bytes()), MIN_SOURCE_UNITS)
    except (CensusError, OSError) as error:
        print(f"cppcheck_sources.py: FATAL -- {error}", file=error_output)
        return 2
    emit_sources(sources, output, nul_terminated)
    return 0


def fixture_git(root: Path, *args: str, extra_env: dict[str, str] | None = None) -> bytes:
    """Run trusted Git for a synthetic selftest repository."""
    environment = sanitized_git_environment()
    if extra_env:
        environment.update(extra_env)
    process = subprocess.run(  # noqa: S603 -- fixed executable from shared Git authority.
        [trusted_git_executable(), "-C", str(root), *args],
        cwd=root,
        env=environment,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        raise CensusError.git_failed(os.fsdecode(process.stderr).strip())
    return process.stdout


def write_fixture(root: Path) -> None:
    """Create the ordinary, ignored, generated, and SOUP fixture population."""
    files = {
        ".gitignore": "/tools/**/build/\n",
        "libs/ra8_core/src/tracked.c": "int tracked_source(void) { return 0; }\n",
        "tools/demo/src/candidate.cpp": "int candidate_source() { return 0; }\n",
        "tools/demo/build/generated.c": "int ignored_output(void) { return 0; }\n",
        "libs/ra8_fonts/generated.c": "int generated_font(void) { return 0; }\n",
        "libs/third_party/vendor.c": "int vendor_source(void) { return 0; }\n",
    }
    for rel, content in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="ascii")


def initialize_fixture(root: Path) -> None:
    """Initialize Git and stage the ordinary, generated, and SOUP fixtures."""
    fixture_git(root, "init", "-q")
    fixture_git(
        root,
        "add",
        ".gitignore",
        "libs/ra8_core/src/tracked.c",
        "libs/ra8_fonts/generated.c",
        "libs/third_party/vendor.c",
    )


def expect(passed: bool, label: str, failures: list[str]) -> None:
    """Record and print one selftest expectation."""
    print(f"  [{'ok' if passed else 'FAIL'}] {label}")
    if not passed:
        failures.append(label)


def capture_census(root: Path, minimum: int = 1) -> tuple[int, list[str], str]:
    """Drive the production census entry point and decode successful output."""
    output = io.BytesIO()
    errors = io.StringIO()
    status_code = run_census(root, minimum, output, errors, nul_terminated=True)
    paths = decode_manifest(output.getvalue()) if status_code == 0 else []
    return status_code, paths, errors.getvalue()


def basic_fixture_selftest(root: Path, failures: list[str]) -> None:
    """Prove ordinary inclusion and ignored/generated/SOUP exclusion."""
    status_code, paths, errors = capture_census(root)
    expected = {
        "libs/ra8_core/src/tracked.c",
        "tools/demo/src/candidate.cpp",
    }
    expect(status_code == 0 and not errors, "valid Git census succeeds", failures)
    expect(set(paths) == expected, "ordinary tracked and candidate sources are exact", failures)
    expect(
        "tools/demo/build/generated.c" not in paths,
        "ignored build output is excluded",
        failures,
    )
    expect(
        not ({"libs/ra8_fonts/generated.c", "libs/third_party/vendor.c"} & set(paths)),
        "generated and SOUP sources are excluded",
        failures,
    )


def symlink_fixture_selftest(root: Path, raw_root: Path, failures: list[str]) -> None:
    """Prove external and internal candidate symlinks fail closed."""
    failure_status = 2
    outside = raw_root / "outside.c"
    outside.write_text("int outside(void) { return 0; }\n", encoding="ascii")
    external_link = root / "libs/ra8_core/src/untracked_external.c"
    external_link.symlink_to(outside)
    status_code, _, errors = capture_census(root)
    expect(
        status_code == failure_status and "symlink" in errors,
        "candidate external symlink fails",
        failures,
    )
    external_link.unlink()

    internal_link = root / "tools/demo/src/untracked_internal.c"
    internal_link.symlink_to(root / "libs/ra8_core/src/tracked.c")
    status_code, _, errors = capture_census(root)
    expect(
        status_code == failure_status and "symlink" in errors,
        "candidate internal symlink fails",
        failures,
    )
    internal_link.unlink()

    special = root / "tools/demo/src/untracked_special.c"
    os.mkfifo(special)
    try:
        validate_sources(root, ["tools/demo/src/untracked_special.c"], 1)
    except CensusError as error:
        expect("regular file" in str(error), "special source entry fails", failures)
    else:
        expect(passed=False, label="special source entry fails", failures=failures)
    special.unlink()


def manifest_fixture_selftest(root: Path, failures: list[str]) -> None:
    """Prove transported duplicates, scope drift, and low counts fail closed."""
    valid = "libs/ra8_core/src/tracked.c"
    duplicate = (valid + "\0" + valid + "\0").encode()
    try:
        validate_sources(root, decode_manifest(duplicate), 1)
    except CensusError as error:
        expect("duplicate" in str(error), "duplicate manifest path fails", failures)
    else:
        expect(passed=False, label="duplicate manifest path fails", failures=failures)

    unexpected = root / "tests/src/unexpected.c"
    unexpected.parent.mkdir(parents=True, exist_ok=True)
    unexpected.write_text("int unexpected(void) { return 0; }\n", encoding="ascii")
    try:
        validate_sources(root, ["tests/src/unexpected.c"], 1)
    except CensusError as error:
        expect("authenticated roots" in str(error), "unexpected scope fails", failures)
    else:
        expect(passed=False, label="unexpected scope fails", failures=failures)

    try:
        validate_sources(root, [valid], 2)
    except CensusError as error:
        expect("floor is 2" in str(error), "below-floor manifest fails", failures)
    else:
        expect(passed=False, label="below-floor manifest fails", failures=failures)


def empty_and_missing_selftest(raw_root: Path, failures: list[str]) -> None:
    """Prove empty and missing Git censuses fail without emitting paths."""
    failure_status = 2
    empty = raw_root / "empty"
    empty.mkdir()
    fixture_git(empty, "init", "-q")
    status_code, paths, errors = capture_census(empty)
    expect(
        status_code == failure_status and not paths and "0 unit(s)" in errors,
        "zero-input census fails closed",
        failures,
    )
    missing = raw_root / "not-a-repo"
    missing.mkdir()
    status_code, paths, errors = capture_census(missing)
    expect(
        status_code == failure_status and not paths and "git ls-files failed" in errors,
        "missing Git census fails closed",
        failures,
    )


def hostile_boundary_selftest(repo_root: Path, raw_root: Path, failures: list[str]) -> None:
    """Drive the exact isolated interpreter boundary under hostile startup state."""
    expected = collect_sources(repo_root, MIN_SOURCE_UNITS)
    empty_index = raw_root / "hostile-index"
    fixture_git(repo_root, "read-tree", "--empty", extra_env={"GIT_INDEX_FILE": str(empty_index)})
    excludes = raw_root / "exclude-all"
    excludes.write_text("*\n", encoding="ascii")
    config = raw_root / "hostile-git-config"
    config.write_text(f"[core]\n\texcludesFile = {excludes}\n", encoding="ascii")
    python_path = raw_root / "hostile-python"
    python_path.mkdir()
    marker = raw_root / "sitecustomize-fired"
    (python_path / "sitecustomize.py").write_text(
        f"from pathlib import Path\nPath({str(marker)!r}).write_text('fired', encoding='ascii')\n",
        encoding="ascii",
    )
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_CONFIG": str(config),
            "GIT_INDEX_FILE": str(empty_index),
            "GIT_WORK_TREE": str(raw_root),
            "PYTHONHOME": str(raw_root / "fake-home"),
            "PYTHONPATH": str(python_path),
        }
    )
    process = subprocess.run(  # noqa: S603 -- fixed trusted Python boundary.
        [trusted_python_executable(), "-I", "-S", str(Path(__file__).resolve()), "--null"],
        cwd=repo_root,
        env=environment,
        capture_output=True,
        check=False,
    )
    actual = decode_manifest(process.stdout) if process.returncode == 0 else []
    expect(
        process.returncode == 0 and actual == expected and not marker.exists(),
        "hostile Git and Python startup state cannot replace the manifest",
        failures,
    )


def run_selftest() -> int:
    """Prove scope, transport, filesystem, and process-boundary behavior."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="cppcheck-sources-selftest-") as raw:
        raw_root = Path(raw)
        fixture = raw_root / "repo"
        fixture.mkdir()
        write_fixture(fixture)
        initialize_fixture(fixture)
        basic_fixture_selftest(fixture, failures)
        symlink_fixture_selftest(fixture, raw_root, failures)
        manifest_fixture_selftest(fixture, failures)
        empty_and_missing_selftest(raw_root, failures)
        repo_root = Path(__file__).resolve().parents[2]
        hostile_boundary_selftest(repo_root, raw_root, failures)
    if failures:
        print(f"cppcheck_sources.py --selftest: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("cppcheck_sources.py --selftest: all cases pass (both directions).")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse command-line options."""
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--selftest", action="store_true", help="run boundary selftests")
    modes.add_argument("--validate-manifest", type=Path, help="validate a NUL manifest")
    parser.add_argument("--null", action="store_true", help="NUL-terminate output paths")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    """Run selftests, validate transport, or emit the repository manifest."""
    args = parse_args(argv)
    if args.selftest:
        return run_selftest()
    repo_root = Path(__file__).resolve().parents[2]
    if args.validate_manifest is not None:
        return run_manifest_validation(
            repo_root,
            args.validate_manifest,
            sys.stdout.buffer,
            sys.stderr,
            args.null,
        )
    return run_census(
        repo_root,
        MIN_SOURCE_UNITS,
        sys.stdout.buffer,
        sys.stderr,
        args.null,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

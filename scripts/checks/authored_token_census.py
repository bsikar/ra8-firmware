# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Git-authored file census and encoding-independent rare-token search."""

from __future__ import annotations

import os
import stat
import subprocess
from collections.abc import Callable, Collection
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from tempfile import TemporaryDirectory

from git_environment import LOCAL_GIT_ENVIRONMENT, isolated_git_environment, trusted_git_executable

BATCH_HEADER_FIELDS = 3
REGULAR_INDEX_MODES = frozenset({"100644", "100755"})
TOKEN_ENCODINGS = ("utf-8", "utf-16-le", "utf-16-be")
SELFTEST_TOKEN = "repair-" + "entry"


class CensusError(RuntimeError):
    """Raised when Git or the worktree cannot prove an authored-file census."""


@dataclass(frozen=True)
class IndexEntry:
    """One stage-zero regular-file candidate from the inherited live index."""

    relative: str
    mode: str
    object_id: str


@dataclass(frozen=True)
class AuthoredSource:
    """One independently validated index or worktree byte source."""

    relative: str
    view: str
    data: bytes


GitRunner = Callable[[list[str], Path, bytes | None], subprocess.CompletedProcess[bytes]]


def _default_git_runner(
    argv: list[str], cwd: Path, input_data: bytes | None
) -> subprocess.CompletedProcess[bytes]:
    """Run one non-mutating Git census command."""
    return subprocess.run(  # noqa: S603 -- argv is the fixed Git census below
        argv, cwd=cwd, input=input_data, capture_output=True, check=False
    )


def _git_output(
    repo_root: Path,
    args: tuple[str, ...],
    runner: GitRunner = _default_git_runner,
    input_data: bytes | None = None,
) -> bytes:
    """Return warning-free Git output or fail closed."""
    argv = [trusted_git_executable(), *args]
    try:
        result = runner(argv, repo_root, input_data)
    except OSError as exc:
        message = f"Git authored-file census could not start: {exc}"
        raise CensusError(message) from exc
    stderr = result.stderr.decode("utf-8", errors="replace").strip()
    if result.returncode != 0:
        detail = stderr or f"exit {result.returncode}"
        message = f"Git authored-file census failed: {detail}"
        raise CensusError(message)
    if stderr:
        message = f"Git authored-file census warned: {stderr}"
        raise CensusError(message)
    return result.stdout


def _nul_records(output: bytes) -> tuple[bytes, ...]:
    """Parse complete NUL-delimited Git records fail closed."""
    if not output:
        return ()
    if not output.endswith(b"\0"):
        message = "Git authored-file census returned a truncated record"
        raise CensusError(message)
    records = tuple(output[:-1].split(b"\0"))
    if any(not record for record in records):
        message = "Git authored-file census returned an empty path"
        raise CensusError(message)
    return records


def _decode_git_path(raw: bytes) -> str:
    """Decode and constrain one repository-relative Git path."""
    try:
        relative = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        message = "Git authored-file census found a non-UTF-8 path"
        raise CensusError(message) from exc
    pure = PurePosixPath(relative)
    if not relative or pure.is_absolute() or ".." in pure.parts or pure.as_posix() != relative:
        message = f"Git authored-file census returned unsafe path {relative!r}"
        raise CensusError(message)
    return relative


def _git_paths(repo_root: Path, args: tuple[str, ...]) -> tuple[str, ...]:
    """Return unique decoded paths from one Git query."""
    records = _nul_records(_git_output(repo_root, args))
    paths = tuple(_decode_git_path(record) for record in records)
    if len(paths) != len(set(paths)):
        message = "Git authored-file census returned duplicate paths"
        raise CensusError(message)
    return paths


def _git_index_entries(repo_root: Path) -> tuple[IndexEntry, ...]:
    """Parse the inherited live index, rejecting conflicts and duplicates."""
    entries = []
    seen = set()
    output = _git_output(repo_root, ("ls-files", "-z", "--stage"))
    for record in _nul_records(output):
        try:
            metadata, raw_path = record.split(b"\t", 1)
            raw_mode, raw_object_id, raw_stage = metadata.split(b" ", 2)
            mode = raw_mode.decode("ascii")
            object_id = raw_object_id.decode("ascii")
        except ValueError as exc:
            message = "Git authored-file census returned malformed stage data"
            raise CensusError(message) from exc
        except UnicodeDecodeError as exc:
            message = "Git authored-file census returned non-ASCII stage metadata"
            raise CensusError(message) from exc
        relative = _decode_git_path(raw_path)
        if raw_stage != b"0":
            message = f"Git authored-file census found an unresolved index stage: {relative}"
            raise CensusError(message)
        if relative in seen:
            message = f"Git authored-file census returned a duplicate index path: {relative}"
            raise CensusError(message)
        if len(object_id) not in {40, 64} or any(
            character not in "0123456789abcdef" for character in object_id
        ):
            message = f"Git authored-file census returned an invalid object ID: {relative}"
            raise CensusError(message)
        seen.add(relative)
        entries.append(IndexEntry(relative, mode, object_id))
    return tuple(entries)


def _scoped_index_entries(
    entries: Collection[IndexEntry], excluded_parts: Collection[str]
) -> tuple[IndexEntry, ...]:
    """Apply explicit tree exclusions, then require regular index modes."""
    scoped = []
    for entry in entries:
        if any(part in excluded_parts for part in PurePosixPath(entry.relative).parts):
            continue
        if entry.mode == "120000":
            message = f"first-party authored path is an index symlink: {entry.relative}"
            raise CensusError(message)
        if entry.mode not in REGULAR_INDEX_MODES:
            message = f"first-party authored path has unsupported index mode: {entry.relative}"
            raise CensusError(message)
        scoped.append(entry)
    return tuple(scoped)


def _parse_blob_batch(output: bytes, entries: Collection[IndexEntry]) -> tuple[bytes, ...]:
    """Parse exact `git cat-file --batch` blob records fail closed."""
    cursor = 0
    blobs = []
    for entry in entries:
        line_end = output.find(b"\n", cursor)
        if line_end < 0:
            message = "Git authored-file blob batch returned a truncated header"
            raise CensusError(message)
        header = output[cursor:line_end].split(b" ")
        if (
            len(header) != BATCH_HEADER_FIELDS
            or header[0] != entry.object_id.encode()
            or header[1] != b"blob"
        ):
            message = f"Git authored-file blob batch returned wrong metadata: {entry.relative}"
            raise CensusError(message)
        try:
            size = int(header[2])
        except ValueError as exc:
            message = f"Git authored-file blob batch returned an invalid size: {entry.relative}"
            raise CensusError(message) from exc
        start = line_end + 1
        end = start + size
        if size < 0 or end >= len(output) or output[end : end + 1] != b"\n":
            message = f"Git authored-file blob batch truncated content: {entry.relative}"
            raise CensusError(message)
        blobs.append(output[start:end])
        cursor = end + 1
    if cursor != len(output):
        message = "Git authored-file blob batch returned trailing data"
        raise CensusError(message)
    return tuple(blobs)


def _index_blobs(repo_root: Path, entries: Collection[IndexEntry]) -> tuple[bytes, ...]:
    """Read all inherited-index blobs in one warning-free Git process."""
    if not entries:
        return ()
    request = b"".join(f"{entry.object_id}\n".encode() for entry in entries)
    output = _git_output(repo_root, ("cat-file", "--batch"), input_data=request)
    return _parse_blob_batch(output, entries)


def path_lstat(path: Path) -> os.stat_result:
    """Read one worktree path without following a symbolic link."""
    return path.lstat()


def path_read_bytes(path: Path) -> bytes:
    """Read one proven-regular authored file as uninterpreted bytes."""
    return path.read_bytes()


def read_file(path: Path, reader: Callable[[Path], bytes] = path_read_bytes) -> bytes:
    """Read one authored file and convert every I/O failure into census failure."""
    try:
        return reader(path)
    except OSError as exc:
        message = f"authored file cannot be read: {path}: {exc}"
        raise CensusError(message) from exc


def _authored_inventory(
    repo_root: Path,
    excluded_parts: Collection[str],
    lstat_file: Callable[[Path], os.stat_result] = path_lstat,
) -> tuple[tuple[IndexEntry, ...], tuple[str, ...]]:
    """Return validated index entries and present nonignored worktree paths."""
    entries = _scoped_index_entries(_git_index_entries(repo_root), excluded_parts)
    untracked = _git_paths(repo_root, ("ls-files", "-z", "--others", "--exclude-standard"))
    untracked = tuple(
        relative
        for relative in untracked
        if not any(part in excluded_parts for part in PurePosixPath(relative).parts)
    )
    index_paths = {entry.relative for entry in entries}
    worktree = []
    for relative in sorted(index_paths | set(untracked)):
        path = repo_root / relative
        try:
            path_stat = lstat_file(path)
        except FileNotFoundError as exc:
            if relative in index_paths:
                continue
            message = f"authored path disappeared during census: {relative}"
            raise CensusError(message) from exc
        except OSError as exc:
            message = f"cannot inspect authored path {relative}: {exc}"
            raise CensusError(message) from exc
        if stat.S_ISLNK(path_stat.st_mode):
            message = f"first-party authored path is a worktree symlink: {relative}"
            raise CensusError(message)
        if not stat.S_ISREG(path_stat.st_mode):
            message = f"first-party authored path is not a regular file: {relative}"
            raise CensusError(message)
        worktree.append(relative)
    return entries, tuple(worktree)


def authored_files(
    repo_root: Path,
    excluded_parts: Collection[str],
    lstat_file: Callable[[Path], os.stat_result] = path_lstat,
) -> list[Path]:
    """Return the union of live-index and nonignored worktree paths."""
    entries, worktree = _authored_inventory(repo_root, excluded_parts, lstat_file)
    relative_paths = {entry.relative for entry in entries} | set(worktree)
    return [repo_root / relative for relative in sorted(relative_paths)]


def authored_sources(
    repo_root: Path,
    excluded_parts: Collection[str],
    lstat_file: Callable[[Path], os.stat_result] = path_lstat,
    reader: Callable[[Path], bytes] = path_read_bytes,
) -> tuple[AuthoredSource, ...]:
    """Read inherited-index blobs and present worktree bytes independently."""
    entries, worktree = _authored_inventory(repo_root, excluded_parts, lstat_file)
    sources = [
        AuthoredSource(entry.relative, "index", data)
        for entry, data in zip(entries, _index_blobs(repo_root, entries), strict=True)
    ]
    for relative in worktree:
        data = read_file(repo_root / relative, reader)
        sources.append(AuthoredSource(relative, "worktree", data))
    return tuple(sources)


def token_hits(
    data: bytes,
    tokens: Collection[str],
    encodings: Collection[str] = TOKEN_ENCODINGS,
) -> tuple[str, ...]:
    """Find rare tokens in each explicitly supported text encoding."""
    return tuple(
        token for token in tokens if any(token.encode(encoding) in data for encoding in encodings)
    )


def init_test_repo(repo_root: Path) -> None:
    """Initialize one throwaway Git repository for semantic selftests."""
    _run_test_git(repo_root, "init", "-q")


def _run_test_git(repo_root: Path, *args: str) -> None:
    """Run one mutating Git command inside a throwaway selftest repository."""
    subprocess.run(  # noqa: S603 -- fixed Git selftest command
        [trusted_git_executable(), *args],
        cwd=repo_root,
        capture_output=True,
        check=True,
    )


def _write(repo_root: Path, relative: str, data: bytes = b"fixture\n") -> Path:
    """Write one throwaway census fixture."""
    path = repo_root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _raises_census(operation: Callable[[], object]) -> bool:
    """Return whether one semantic selftest operation fails closed."""
    try:
        operation()
    except CensusError:
        return True
    return False


def _source_hits(repo_root: Path, excluded_parts: Collection[str]) -> set[tuple[str, str]]:
    """Return path/view pairs containing the synthetic rare token."""
    return {
        (source.relative, source.view)
        for source in authored_sources(repo_root, excluded_parts)
        if token_hits(source.data, (SELFTEST_TOKEN,))
    }


def _selftest_index_worktree_views(excluded_parts: Collection[str]) -> list[str]:
    """Prove index and worktree bytes are scanned as independent views."""
    token = SELFTEST_TOKEN.encode()
    cases = (
        ("staged unsafe/worktree safe", token, b"safe\n", {("entry", "index")}),
        ("index unsafe/worktree deleted", token, None, {("entry", "index")}),
        ("index safe/worktree unsafe", b"safe\n", token, {("entry", "worktree")}),
    )
    failures = []
    for name, index_data, worktree_data, expected in cases:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            init_test_repo(root)
            entry = _write(root, "entry", index_data)
            _run_test_git(root, "add", "entry")
            if worktree_data is None:
                entry.unlink()
            else:
                entry.write_bytes(worktree_data)
            if _source_hits(root, excluded_parts) != expected:
                failures.append(f"  {name} did not preserve both byte views")
    return failures


def _selftest_scope(excluded_parts: Collection[str]) -> list[str]:
    """Prove Git inventory, exclusions, ignores and deletion semantics."""
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, ".gitignore", b"/build-cov/\n")
        _write(root, "tracked.txt")
        _write(root, "deleted.txt")
        excluded = tuple(f"nested/{part}/ignored" for part in sorted(excluded_parts))
        for relative in excluded:
            _write(root, relative, SELFTEST_TOKEN.encode())
        tracked_excluded = tuple(path for path in excluded if "/.git/" not in path)
        _run_test_git(root, "add", "-f", ".gitignore", "tracked.txt", "deleted.txt")
        if tracked_excluded:
            _run_test_git(root, "add", "-f", "--", *tracked_excluded)
        _run_test_git(
            root,
            "-c",
            "user.name=fixture",
            "-c",
            "user.email=fixture@example.invalid",
            "commit",
            "-qm",
            "fixture",
        )
        _run_test_git(root, "rm", "-q", "deleted.txt")
        _write(root, "untracked.txt")
        _write(root, "build-cov/ignored", SELFTEST_TOKEN.encode())
        inputs = {
            path.relative_to(root).as_posix() for path in authored_files(root, excluded_parts)
        }
        required = {".gitignore", "tracked.txt", "untracked.txt"}
        forbidden = {*excluded, "deleted.txt", "build-cov/ignored"}
        if not required <= inputs or forbidden & inputs:
            return ["  Git census did not preserve tracked/untracked/ignored/deleted scope"]
    return []


def _selftest_symlinks(excluded_parts: Collection[str]) -> list[str]:
    """Prove worktree/index aliases fail while excluded vendor aliases stay out."""
    failures = []
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "vendor/target")
        (root / "alias").symlink_to("vendor/target")
        if not _raises_census(lambda: authored_files(root, excluded_parts)):
            failures.append("  an untracked first-party worktree symlink was accepted")
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "vendor/target")
        alias = root / "alias"
        alias.symlink_to("vendor/target")
        _run_test_git(root, "add", "alias")
        alias.unlink()
        alias.write_text("regular replacement\n", encoding="utf-8")
        if not _raises_census(lambda: authored_files(root, excluded_parts)):
            failures.append("  a first-party index symlink was accepted after worktree replacement")
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "vendor/target")
        alias = _write(root, "alias")
        _run_test_git(root, "add", "alias")
        alias.unlink()
        alias.symlink_to("vendor/target")
        if not _raises_census(lambda: authored_sources(root, excluded_parts)):
            failures.append("  a worktree symlink was accepted over a regular index entry")
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "vendor/target")
        (root / "vendor/alias").symlink_to("target")
        _run_test_git(root, "add", "-f", "vendor/target", "vendor/alias")
        if _raises_census(lambda: authored_files(root, excluded_parts)):
            failures.append("  an explicitly excluded vendor symlink entered first-party scope")
    return failures


def _selftest_encodings(excluded_parts: Collection[str]) -> list[str]:
    """Prove UTF-8, UTF-16LE, UTF-16BE and BOM token discovery end to end."""
    cases = {
        "utf8": SELFTEST_TOKEN.encode("utf-8"),
        "utf16le": SELFTEST_TOKEN.encode("utf-16-le"),
        "utf16be": SELFTEST_TOKEN.encode("utf-16-be"),
        "utf16bom": b"\xff\xfe" + SELFTEST_TOKEN.encode("utf-16-le"),
    }
    failures = []
    for name, data in cases.items():
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            init_test_repo(root)
            relative = f"{name}.blob"
            _write(root, relative, data)
            _run_test_git(root, "add", relative)
            expected = {(relative, "index"), (relative, "worktree")}
            if _source_hits(root, excluded_parts) != expected:
                failures.append(f"  authored {name} rare token was not detected")
    return failures


def _commit_fixture(repo_root: Path) -> None:
    """Commit the current throwaway index with local synthetic identity."""
    _run_test_git(
        repo_root,
        "-c",
        "user.name=fixture",
        "-c",
        "user.email=fixture@example.invalid",
        "commit",
        "-qm",
        "fixture",
    )


def _selftest_staged_deletions(excluded_parts: Collection[str]) -> list[str]:
    """Prove staged deletion drops only index data while retained bytes stay scanned."""
    failures = []
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "entry", SELFTEST_TOKEN.encode())
        _run_test_git(root, "add", "entry")
        _commit_fixture(root)
        _run_test_git(root, "rm", "-q", "entry")
        if any(source.relative == "entry" for source in authored_sources(root, excluded_parts)):
            failures.append("  staged deletion remained in an authored byte view")
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        _write(root, "entry", SELFTEST_TOKEN.encode())
        _run_test_git(root, "add", "entry")
        _commit_fixture(root)
        _run_test_git(root, "rm", "--cached", "-q", "entry")
        if _source_hits(root, excluded_parts) != {("entry", "worktree")}:
            failures.append("  staged index deletion hid retained nonignored worktree bytes")
    return failures


def _failed_git_runner(
    argv: list[str], _cwd: Path, _input: bytes | None
) -> subprocess.CompletedProcess[bytes]:
    """Return one synthetic failing Git result."""
    return subprocess.CompletedProcess(argv, 128, b"", b"fatal")


def _warned_git_runner(
    argv: list[str], _cwd: Path, _input: bytes | None
) -> subprocess.CompletedProcess[bytes]:
    """Return a success status carrying an unreadable-directory warning."""
    warning = b"warning: could not open directory: Permission denied"
    return subprocess.CompletedProcess(argv, 0, b"", warning)


def _denied_git_runner(
    _argv: list[str], _cwd: Path, _input: bytes | None
) -> subprocess.CompletedProcess[bytes]:
    """Raise the execution failure used by the Git fail-closed test."""
    message = "fixture denied"
    raise PermissionError(message)


def _denied_path(_path: Path) -> bytes:
    """Raise the permission failure used by stat/read fail-closed tests."""
    message = "fixture denied"
    raise PermissionError(message)


def _selftest_fail_closed(excluded_parts: Collection[str]) -> list[str]:
    """Prove Git warnings/failures and stat/read errors cannot shrink the scan."""
    failures = []
    if not _raises_census(lambda: _git_output(Path.cwd(), ("ls-files",), _failed_git_runner)):
        failures.append("  a failing Git authored census was accepted")
    if not _raises_census(lambda: _git_output(Path.cwd(), ("ls-files",), _warned_git_runner)):
        failures.append("  an unreadable-directory Git warning was accepted")
    if not _raises_census(lambda: _git_output(Path.cwd(), ("ls-files",), _denied_git_runner)):
        failures.append("  a Git execution error was accepted")
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        init_test_repo(root)
        file_path = _write(root, "blocked")
        if not _raises_census(lambda: authored_files(root, excluded_parts, _denied_path)):
            failures.append("  an authored lstat error was accepted")
        if not _raises_census(lambda: authored_sources(root, excluded_parts, reader=_denied_path)):
            failures.append(f"  an authored read error was accepted for {file_path.name}")
    return failures


def _fixture_selftests(excluded_parts: Collection[str]) -> list[str]:
    """Run every nested-repository semantic fixture in the current environment."""
    return (
        _selftest_index_worktree_views(excluded_parts)
        + _selftest_scope(excluded_parts)
        + _selftest_symlinks(excluded_parts)
        + _selftest_encodings(excluded_parts)
        + _selftest_staged_deletions(excluded_parts)
        + _selftest_fail_closed(excluded_parts)
    )


def _repo_snapshot(repo_root: Path) -> tuple[bytes, bytes, bytes]:
    """Capture the outer state that a nested fixture must not mutate."""
    with isolated_git_environment():
        return (
            _git_output(repo_root, ("rev-parse", "HEAD")),
            (repo_root / ".git" / "index").read_bytes(),
            _git_output(repo_root, ("status", "--porcelain=v1", "-z")),
        )


def _selftest_hostile_environment(excluded_parts: Collection[str]) -> list[str]:
    """Prove hook-local Git routing cannot capture nested census fixtures."""
    failures = []
    with TemporaryDirectory() as tmp:
        outer = Path(tmp)
        with isolated_git_environment():
            init_test_repo(outer)
            _write(outer, "outer-sentinel")
            _run_test_git(outer, "add", "outer-sentinel")
            _run_test_git(
                outer,
                "-c",
                "user.name=fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "commit",
                "-qm",
                "outer",
            )
        before = _repo_snapshot(outer)
        original = {name: os.environ.get(name) for name in LOCAL_GIT_ENVIRONMENT}
        hostile = dict.fromkeys(LOCAL_GIT_ENVIRONMENT, "hostile")
        hostile.update(
            {
                "GIT_DIR": str(outer / ".git"),
                "GIT_WORK_TREE": str(outer),
                "GIT_INDEX_FILE": str(outer / ".git" / "index"),
                "GIT_PREFIX": "",
            }
        )
        try:
            os.environ.update(hostile)
            with isolated_git_environment():
                if any(os.environ.get(name) == "hostile" for name in LOCAL_GIT_ENVIRONMENT):
                    failures.append("  nested census retained a hostile Git routing/config value")
                failures.extend(_fixture_selftests(excluded_parts))
        finally:
            for name, value in original.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
        if _repo_snapshot(outer) != before:
            failures.append("  nested census changed the hostile outer Git repository")
    return failures


def selftest(excluded_parts: Collection[str]) -> list[str]:
    """Prove the Git-authored census and raw-token detector in both directions."""
    with isolated_git_environment():
        failures = _fixture_selftests(excluded_parts)
    return failures + _selftest_hostile_environment(excluded_parts)

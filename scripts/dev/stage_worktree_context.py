#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Build and verify an exact archive of selected candidate-worktree paths.

The dev-box Ansible role must provision from the checkout that invoked it,
including unstaged edits and non-ignored new files. Recursively copying source
directories also copies ignored workstation residue, while ``git archive``
cannot see dirty or untracked candidate bytes. This helper uses Git only as the
census: present cached files plus non-ignored untracked files are archived from
their current worktree bytes.

Archive metadata is normalized so identical candidate trees produce identical
bytes on different controllers. Regular files preserve only the current
worktree's portable executable bit, including unstaged chmod changes; the index
distinguishes tracked symlinks from regular files. The verifier compares every
archive member, byte, link target, and normalized mode against an extracted
tree, including unexpected files.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from git_environment import LOCAL_GIT_ENVIRONMENT, sanitized_git_environment, trusted_git_executable

EXIT_MISMATCH = 1
EXIT_USAGE = 2
REGULAR_MODE = 0o644
EXECUTABLE_MODE = 0o755
DIRECTORY_MODE = 0o755
SYMLINK_MODE = 0o777
GIT_REGULAR_MODE = "100644"
GIT_EXECUTABLE_MODE = "100755"
GIT_SYMLINK_MODE = "120000"
GIT_STAGE_NORMAL = "0"
GIT_STAGE_FIELD_COUNT = 3


class ContextError(RuntimeError):
    """A candidate census, archive, or verification contract failed."""


@dataclass(frozen=True)
class Entry:
    """One normalized archive entry."""

    name: str
    kind: str
    mode: int
    data: bytes = b""
    link: str = ""


def _git(root: Path, args: list[str]) -> bytes:
    """Run one Git query with ``root`` as the only repository selector."""
    proc = subprocess.run(  # noqa: S603 -- fixed git executable and caller-built argv
        [trusted_git_executable(), *args],
        cwd=root,
        env=sanitized_git_environment(),
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = os.fsdecode(proc.stderr).strip()
        message = f"git {' '.join(args)} failed: {detail}"
        raise ContextError(message)
    return proc.stdout


def _candidate_paths(root: Path, scopes: tuple[str, ...]) -> list[str]:
    """Return sorted live cached or non-ignored untracked paths in scope."""
    raw = _git(
        root,
        ["ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *scopes],
    )
    paths: list[str] = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        rel = os.fsdecode(record)
        source = root / rel
        try:
            source.lstat()
        except FileNotFoundError:
            continue
        paths.append(rel)
    return sorted(set(paths), key=os.fsencode)


def _tracked_modes(root: Path, scopes: tuple[str, ...]) -> dict[str, str]:
    """Return index modes for normal-stage tracked paths in scope."""
    raw = _git(root, ["ls-files", "-z", "--stage", "--cached", "--", *scopes])
    modes: dict[str, str] = {}
    for record in raw.split(b"\0"):
        if not record:
            continue
        header, separator, raw_path = record.partition(b"\t")
        fields = header.split()
        if not separator or len(fields) != GIT_STAGE_FIELD_COUNT:
            message = "git ls-files --stage returned a malformed record"
            raise ContextError(message)
        mode, _object_id, stage = (os.fsdecode(field) for field in fields)
        rel = os.fsdecode(raw_path)
        if stage != GIT_STAGE_NORMAL:
            message = f"cannot archive conflicted index entry: {rel}"
            raise ContextError(message)
        modes[rel] = mode
    return modes


def _regular_mode(source: Path, git_mode: str | None) -> int:
    """Normalize the candidate worktree's current executable mode."""
    if git_mode is not None and git_mode not in (GIT_EXECUTABLE_MODE, GIT_REGULAR_MODE):
        message = f"unsupported tracked file mode {git_mode}: {source}"
        raise ContextError(message)
    return EXECUTABLE_MODE if source.stat().st_mode & stat.S_IXUSR else REGULAR_MODE


def _file_entry(root: Path, rel: str, git_mode: str | None) -> Entry:
    """Read one live worktree path into its normalized representation."""
    source = root / rel
    metadata = source.lstat()
    if stat.S_ISLNK(metadata.st_mode):
        if git_mode not in (None, GIT_SYMLINK_MODE):
            message = f"worktree type disagrees with index for {rel}"
            raise ContextError(message)
        return Entry(rel, "symlink", SYMLINK_MODE, link=str(source.readlink()))
    if stat.S_ISREG(metadata.st_mode):
        if git_mode == GIT_SYMLINK_MODE:
            message = f"worktree type disagrees with index for {rel}"
            raise ContextError(message)
        return Entry(rel, "file", _regular_mode(source, git_mode), data=source.read_bytes())
    message = f"unsupported worktree entry type: {rel}"
    raise ContextError(message)


def collect_entries(root: Path, scopes: tuple[str, ...]) -> list[Entry]:
    """Collect normalized directories and candidate file entries."""
    modes = _tracked_modes(root, scopes)
    files = [_file_entry(root, rel, modes.get(rel)) for rel in _candidate_paths(root, scopes)]
    directories: set[str] = set()
    for entry in files:
        parent = PurePosixPath(entry.name).parent
        while parent != PurePosixPath("."):
            directories.add(parent.as_posix())
            parent = parent.parent
    dirs = [Entry(name, "directory", DIRECTORY_MODE) for name in directories]
    return sorted([*dirs, *files], key=lambda entry: os.fsencode(entry.name))


def _tar_info(entry: Entry) -> tarfile.TarInfo:
    """Create normalized tar metadata for one entry."""
    info = tarfile.TarInfo(entry.name)
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    info.mode = entry.mode
    if entry.kind == "directory":
        info.type = tarfile.DIRTYPE
    elif entry.kind == "symlink":
        info.type = tarfile.SYMTYPE
        info.linkname = entry.link
    else:
        info.type = tarfile.REGTYPE
        info.size = len(entry.data)
    return info


def create_archive(root: Path, output: Path, scopes: tuple[str, ...]) -> str:
    """Write an atomic deterministic tar and return its SHA-256 digest."""
    entries = collect_entries(root, scopes)
    if not entries:
        message = "candidate census is empty"
        raise ContextError(message)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp-{os.getpid()}")
    try:
        with tarfile.open(temporary, "w", format=tarfile.GNU_FORMAT) as archive:
            for entry in entries:
                info = _tar_info(entry)
                payload = io.BytesIO(entry.data) if entry.kind == "file" else None
                archive.addfile(info, payload)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return hashlib.sha256(output.read_bytes()).hexdigest()


def _safe_name(raw_name: str) -> str:
    """Return a canonical relative member name or reject the archive."""
    name = raw_name.rstrip("/")
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or path.as_posix() != name:
        message = f"unsafe or non-canonical archive member: {raw_name!r}"
        raise ContextError(message)
    return name


def _archive_entries(archive_path: Path) -> dict[str, Entry]:
    """Read normalized entries from a trusted candidate archive."""
    entries: dict[str, Entry] = {}
    with tarfile.open(archive_path, "r:") as archive:
        for member in archive.getmembers():
            name = _safe_name(member.name)
            if name in entries:
                message = f"duplicate archive member: {name}"
                raise ContextError(message)
            if member.isdir():
                entry = Entry(name, "directory", member.mode & 0o777)
            elif member.issym():
                entry = Entry(name, "symlink", SYMLINK_MODE, link=member.linkname)
            elif member.isfile():
                source = archive.extractfile(member)
                if source is None:
                    message = f"cannot read archive member: {name}"
                    raise ContextError(message)
                entry = Entry(name, "file", member.mode & 0o777, data=source.read())
            else:
                message = f"unsupported archive member type: {name}"
                raise ContextError(message)
            entries[name] = entry
    if not entries:
        message = "archive contains no entries"
        raise ContextError(message)
    return entries


def _tree_entries(directory: Path) -> dict[str, Entry]:
    """Read every file, directory, and symlink below an extracted root."""
    entries: dict[str, Entry] = {}
    if not directory.is_dir():
        return entries
    for base, dirs, files in os.walk(directory, followlinks=False):
        base_path = Path(base)
        names = sorted([*dirs, *files], key=os.fsencode)
        for name in names:
            path = base_path / name
            rel = path.relative_to(directory).as_posix()
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                entries[rel] = Entry(rel, "symlink", SYMLINK_MODE, link=str(path.readlink()))
                if name in dirs:
                    dirs.remove(name)
            elif stat.S_ISDIR(metadata.st_mode):
                entries[rel] = Entry(rel, "directory", metadata.st_mode & 0o777)
            elif stat.S_ISREG(metadata.st_mode):
                entries[rel] = Entry(rel, "file", metadata.st_mode & 0o777, data=path.read_bytes())
            else:
                message = f"unsupported staged entry type: {rel}"
                raise ContextError(message)
    return entries


def verify_archive(archive_path: Path, directory: Path) -> list[str]:
    """Return exact archive-versus-directory mismatch descriptions."""
    expected = _archive_entries(archive_path)
    actual = _tree_entries(directory)
    findings = [
        f"missing: {name}" for name in sorted(expected.keys() - actual.keys(), key=os.fsencode)
    ]
    findings.extend(
        f"unexpected: {name}" for name in sorted(actual.keys() - expected.keys(), key=os.fsencode)
    )
    for name in sorted(expected.keys() & actual.keys(), key=os.fsencode):
        wanted = expected[name]
        got = actual[name]
        if wanted.kind != got.kind:
            findings.append(f"type mismatch: {name}")
        elif wanted.mode != got.mode:
            findings.append(f"mode mismatch: {name}")
        elif wanted.kind == "file" and wanted.data != got.data:
            findings.append(f"content mismatch: {name}")
        elif wanted.kind == "symlink" and wanted.link != got.link:
            findings.append(f"link mismatch: {name}")
    return findings


def _git_init(root: Path) -> None:
    """Initialize the selftest repository with deterministic identity."""
    root.mkdir()
    _git(root, ["init", "-q"])
    _git(root, ["config", "user.email", "context@example.invalid"])
    _git(root, ["config", "user.name", "Context Selftest"])


def _write_fixture(root: Path) -> None:
    """Create tracked, dirty, deleted, ignored, untracked, and link inputs."""
    (root / "scripts/tool dir").mkdir(parents=True)  # PATHREF-OK: fixture
    (root / ".devcontainer").mkdir()
    (root / ".gitignore").write_text("__pycache__/\n*.pyc\n", encoding="ascii")
    (root / ".devcontainer/Dockerfile").write_text("FROM scratch\n", encoding="ascii")
    tracked = root / "scripts/tracked.sh"  # PATHREF-OK: fixture
    tracked.write_text("#!/bin/sh\necho index\n", encoding="ascii")
    tracked.chmod(REGULAR_MODE)
    deleted = root / "scripts/deleted.py"  # PATHREF-OK: fixture
    deleted.write_text("old = True\n", encoding="ascii")
    (root / "scripts/tracked-link").symlink_to("tracked.sh")  # PATHREF-OK: fixture
    _git(root, ["add", ".gitignore", ".devcontainer", "scripts"])
    _git(root, ["commit", "-qm", "fixture"])
    tracked.write_text("#!/bin/sh\necho worktree\n", encoding="ascii")
    tracked.chmod(EXECUTABLE_MODE)
    deleted.unlink()
    (root / "scripts/tool dir/new helper.py").write_text(  # PATHREF-OK: fixture
        "value = 1\n", encoding="ascii"
    )
    (root / "scripts/tool dir/__pycache__").mkdir()  # PATHREF-OK: fixture
    (root / "scripts/tool dir/__pycache__/helper.pyc").write_bytes(  # PATHREF-OK: fixture
        b"ignored"
    )
    (root / "outside.txt").write_text("outside\n", encoding="ascii")


def _extract_for_selftest(archive_path: Path, target: Path) -> None:
    """Extract a helper-produced archive after validating every member name."""
    with tarfile.open(archive_path, "r:") as archive:
        for member in archive.getmembers():
            destination = target / _safe_name(member.name)
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
                destination.chmod(member.mode)
            elif member.issym():
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.symlink_to(member.linkname)
            elif member.isfile():
                source = archive.extractfile(member)
                if source is None:
                    message = f"cannot read archive member: {member.name}"
                    raise ContextError(message)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read())
                destination.chmod(member.mode)
            else:
                message = f"unsupported archive member type: {member.name}"
                raise ContextError(message)


def _selftest_fixture(root: Path) -> list[str]:
    """Run the exact create/verify path and return assertion failures."""
    _git_init(root)
    _write_fixture(root)
    archive_one = root.parent / "context one.tar"
    archive_two = root.parent / "context two.tar"
    digest_one = create_archive(root, archive_one, (".devcontainer", "scripts"))
    digest_two = create_archive(root, archive_two, (".devcontainer", "scripts"))
    names = set(_archive_entries(archive_one))
    failures: list[str] = []
    required = {
        ".devcontainer/Dockerfile",
        "scripts/tracked.sh",  # PATHREF-OK: fixture
        "scripts/tracked-link",  # PATHREF-OK: fixture
        "scripts/tool dir/new helper.py",  # PATHREF-OK: fixture
    }
    excluded = {
        "scripts/deleted.py",  # PATHREF-OK: fixture
        "scripts/tool dir/__pycache__/helper.pyc",  # PATHREF-OK: fixture
        "outside.txt",
    }
    if not required.issubset(names):
        failures.append("tracked, symlink, or spaced untracked input was omitted")
    if names & excluded:
        failures.append("deleted, ignored, or out-of-scope input entered the archive")
    if digest_one != digest_two or archive_one.read_bytes() != archive_two.read_bytes():
        failures.append("identical candidate inputs did not produce identical archive bytes")
    extracted = root.parent / "extracted"
    extracted.mkdir()
    _extract_for_selftest(archive_one, extracted)
    if verify_archive(archive_one, extracted):
        failures.append("an exact extraction did not verify")
    if (extracted / "scripts/tracked.sh").read_text(encoding="ascii") != (  # PATHREF-OK: fixture
        "#!/bin/sh\necho worktree\n"
    ):
        failures.append("tracked worktree bytes were not archived")
    if (extracted / "scripts/tracked.sh").stat().st_mode & 0o777 != (  # PATHREF-OK: fixture
        EXECUTABLE_MODE
    ):
        failures.append("tracked executable mode was not preserved")
    if not (extracted / "scripts/tracked-link").is_symlink():  # PATHREF-OK: fixture
        failures.append("tracked symlink mode was not preserved")
    return failures


def _outer_repo_snapshot(root: Path) -> tuple[bytes, bytes, bytes, bytes, bytes]:
    """Capture the outer repository state a nested fixture must not change."""
    return (
        _git(root, ["rev-parse", "HEAD"]),
        (root / ".git/index").read_bytes(),
        (root / ".git/config").read_bytes(),
        _git(root, ["status", "--porcelain=v1", "-z"]),
        (root / "outer.txt").read_bytes(),
    )


def _hook_environment_selftest(base: Path) -> list[str]:
    """Prove hook-local Git routing cannot escape into an outer repository."""
    base.mkdir()
    outer = base / "outer"
    _git_init(outer)
    (outer / "outer.txt").write_bytes(b"outer-state\n")
    _git(outer, ["add", "outer.txt"])
    _git(outer, ["commit", "-qm", "outer seed"])
    before = _outer_repo_snapshot(outer)
    hook_env = {
        "GIT_DIR": str(outer / ".git"),
        "GIT_INDEX_FILE": str(outer / ".git/index"),
        "GIT_PREFIX": "",
        "GIT_WORK_TREE": str(outer),
    }
    previous = {name: os.environ.get(name) for name in hook_env}
    failures: list[str] = []
    try:
        os.environ.update(hook_env)
        failures.extend(_selftest_fixture(base / "inner"))
    except (ContextError, OSError, tarfile.TarError) as exc:
        failures.append(f"inner fixture failed under outer hook environment: {exc}")
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
    if _outer_repo_snapshot(outer) != before:
        failures.append(
            "inner fixture changed the outer repository HEAD/index/config/status/worktree"
        )
    reported = set(os.fsdecode(_git(outer, ["rev-parse", "--local-env-vars"])).splitlines())
    if not reported.issubset(LOCAL_GIT_ENVIRONMENT):
        failures.append("LOCAL_GIT_ENVIRONMENT omits a name from `git rev-parse --local-env-vars`")
    return failures


def run_selftest() -> int:
    """Prove inclusion, exclusion, reproducibility, and drift detection."""
    with tempfile.TemporaryDirectory(prefix="ra8-context-selftest-") as temp:
        base = Path(temp)
        failures = _selftest_fixture(base / "repo")
        failures.extend(_hook_environment_selftest(base / "hook-env"))
        archive = base / "context one.tar"
        extracted = base / "extracted"
        extra = extracted / "scripts/ignored.pyc"  # PATHREF-OK: fixture
        extra.write_bytes(b"residue")
        findings = verify_archive(archive, extracted)
        if "unexpected: scripts/ignored.pyc" not in findings:  # PATHREF-OK: fixture
            failures.append("an unexpected ignored-style residue did not fail verification")
        extra.unlink()
        target = extracted / "scripts/tracked.sh"  # PATHREF-OK: fixture
        target.chmod(REGULAR_MODE)
        findings = verify_archive(archive, extracted)
        if "mode mismatch: scripts/tracked.sh" not in findings:  # PATHREF-OK: fixture
            failures.append("executable mode drift did not fail verification")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return EXIT_MISMATCH
    print("stage_worktree_context.py selftest: PASS")
    return 0


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    subparsers = parser.add_subparsers(dest="command")
    create = subparsers.add_parser("create", help="create a deterministic candidate archive")
    create.add_argument("--root", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("paths", nargs="+")
    verify = subparsers.add_parser("verify", help="compare an archive to an extracted directory")
    verify.add_argument("--archive", type=Path, required=True)
    verify.add_argument("--directory", type=Path, required=True)
    return parser


def main() -> int:
    """Dispatch archive creation, exact verification, or the selftest."""
    args = _parser().parse_args()
    if args.selftest:
        if args.command is not None:
            print("--selftest cannot be combined with a command", file=sys.stderr)
            return EXIT_USAGE
        return run_selftest()
    try:
        if args.command == "create":
            digest = create_archive(args.root.resolve(), args.output.resolve(), tuple(args.paths))
            print(digest)
            return 0
        if args.command == "verify":
            findings = verify_archive(args.archive, args.directory)
            for finding in findings:
                print(finding, file=sys.stderr)
            return EXIT_MISMATCH if findings else 0
    except (ContextError, OSError, tarfile.TarError) as exc:
        print(f"stage_worktree_context.py: {exc}", file=sys.stderr)
        return EXIT_USAGE
    print("choose create, verify, or --selftest", file=sys.stderr)
    return EXIT_USAGE


if __name__ == "__main__":
    raise SystemExit(main())

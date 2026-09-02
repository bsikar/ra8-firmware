#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Assemble a complete reviewed worktree candidate in a strict private repo."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Never

sys.path.insert(0, str(Path(__file__).resolve().parent))

from git_environment import sanitized_git_environment, trusted_git_executable

TREE_ROW_FIELDS = 3


class CandidateError(RuntimeError):
    """The source tree could not be frozen into an exact safe candidate."""


@dataclass(frozen=True)
class SourceState:
    """NUL-safe identity of every byte and Git input consumed by assembly."""

    index_sha256: str
    inputs_sha256: str
    tracked: tuple[str, ...]
    untracked: tuple[str, ...]


@dataclass(frozen=True)
class SourceRouting:
    """Validated immutable routing captured before private assembly."""

    root: Path
    git_dir: Path
    index: Path
    objects: tuple[Path, ...]
    head: str


def _fail(message: str) -> Never:
    raise CandidateError(message)


def _git(
    root: Path,
    args: Sequence[str],
    *,
    extra_env: dict[str, str] | None = None,
    check: bool = True,
) -> bytes:
    environment = sanitized_git_environment()
    if extra_env:
        environment.update(extra_env)
    proc = subprocess.run(  # noqa: S603 -- fixed absolute Git authority and audited argv
        [trusted_git_executable(), "-C", str(root), *args],
        env=environment,
        capture_output=True,
        check=False,
    )
    if check and proc.returncode != 0:
        detail = os.fsdecode(proc.stderr).strip()
        _fail(f"Git {' '.join(args)} failed: {detail}")
    return proc.stdout


def _absolute_git_path(
    root: Path,
    *args: str,
    extra_env: dict[str, str] | None = None,
) -> Path:
    raw = os.fsdecode(_git(root, args, extra_env=extra_env)).strip()
    path = Path(raw)
    if not path.is_absolute() or "\n" in raw or "\r" in raw:
        _fail(f"Git returned unsafe routing path: {raw!r}")
    return Path(os.path.realpath(path))


def _inherited_alternate_dirs() -> tuple[Path, ...]:
    """Validate inherited alternates without reinterpreting Git quoting rules."""
    raw = os.environ.get("GIT_ALTERNATE_OBJECT_DIRECTORIES", "")
    if not raw:
        return ()
    if any(char in raw for char in ("\n", "\r", "\0", "'", '"')):
        _fail("inherited Git alternates contain quoting or control characters")
    entries = raw.split(os.pathsep)
    if any(not entry for entry in entries):
        _fail("inherited Git alternates contain an empty separator entry")
    normalized: list[Path] = []
    for entry in entries:
        path = Path(entry)
        if not path.is_absolute():
            _fail(f"inherited Git alternate is not absolute: {entry!r}")
        path = Path(os.path.realpath(path))
        try:
            info = path.lstat()
        except OSError as exc:
            message = f"inherited Git alternate is unavailable: {path}"
            raise CandidateError(message) from exc
        if not stat.S_ISDIR(info.st_mode) or path.is_symlink():
            _fail(f"inherited Git alternate is not a regular directory: {path}")
        if path not in normalized:
            normalized.append(path)
    return tuple(normalized)


def _alternate_environment(objects: Sequence[Path]) -> dict[str, str]:
    """Bind one normalized alternate list for every source-object query."""
    if not objects:
        return {}
    return {"GIT_ALTERNATE_OBJECT_DIRECTORIES": os.pathsep.join(map(str, objects))}


def _normalized_object_dirs(root: Path, inherited: Sequence[Path]) -> tuple[Path, ...]:
    environment = _alternate_environment(inherited)
    primary = _absolute_git_path(
        root,
        "rev-parse",
        "--path-format=absolute",
        "--git-path",
        "objects",
        extra_env=environment,
    )
    report = os.fsdecode(_git(root, ("count-objects", "-v"), extra_env=environment))
    paths = [primary]
    paths.extend(
        Path(line.removeprefix("alternate: "))
        for line in report.splitlines()
        if line.startswith("alternate: ")
    )
    normalized: list[Path] = []
    for raw in paths:
        if not raw.is_absolute() or any(char in str(raw) for char in ("\n", "\r", "\0")):
            _fail(f"Git object path is not safe and absolute: {raw}")
        path = Path(os.path.realpath(raw))
        try:
            info = path.lstat()
        except OSError as exc:
            message = f"Git object path is unavailable: {path}"
            raise CandidateError(message) from exc
        if not stat.S_ISDIR(info.st_mode) or path.is_symlink():
            _fail(f"Git object path is not a regular directory: {path}")
        if path not in normalized:
            normalized.append(path)
    return tuple(normalized)


def _capture_routing(source: Path) -> SourceRouting:
    inherited = _inherited_alternate_dirs()
    environment = _alternate_environment(inherited)
    root = _absolute_git_path(source, "rev-parse", "--show-toplevel", extra_env=environment)
    if root != Path(os.path.realpath(source)):
        _fail("source must be the repository toplevel")
    git_dir = _absolute_git_path(
        root,
        "rev-parse",
        "--path-format=absolute",
        "--absolute-git-dir",
        extra_env=environment,
    )
    index = _absolute_git_path(
        root,
        "rev-parse",
        "--path-format=absolute",
        "--git-path",
        "index",
        extra_env=environment,
    )
    try:
        info = index.lstat()
    except OSError as exc:
        message = "active source index is unavailable"
        raise CandidateError(message) from exc
    if not stat.S_ISREG(info.st_mode) or index.is_symlink():
        _fail("active source index is not a regular non-symlink file")
    head = os.fsdecode(_git(root, ("rev-parse", "--verify", "HEAD"), extra_env=environment)).strip()
    if len(head) not in {40, 64} or any(char not in "0123456789abcdef" for char in head):
        _fail("source HEAD is not a full object identifier")
    return SourceRouting(root, git_dir, index, _normalized_object_dirs(root, inherited), head)


def _source_environment(routing: SourceRouting) -> dict[str, str]:
    """Return the immutable normalized object-routing environment."""
    return _alternate_environment(routing.objects[1:])


def _index_entries(routing: SourceRouting) -> tuple[tuple[str, str, str], ...]:
    records: list[tuple[str, str, str]] = []
    for row in _git(
        routing.root,
        ("ls-files", "--stage", "-z"),
        extra_env=_source_environment(routing),
    ).split(b"\0"):
        if not row:
            continue
        metadata, separator, raw_path = row.partition(b"\t")
        fields = metadata.split()
        if not separator or len(fields) != TREE_ROW_FIELDS:
            _fail("malformed staged-index row")
        mode, _blob, stage = (os.fsdecode(field) for field in fields)
        path = os.fsdecode(raw_path)
        if stage != "0":
            _fail(f"unmerged index entry is not a candidate input: {path}")
        if mode not in {"100644", "100755", "120000"}:
            _fail(f"unsupported candidate index mode {mode}: {path}")
        records.append((path, mode, stage))
    return tuple(records)


def _untracked(routing: SourceRouting) -> tuple[str, ...]:
    rows = _git(
        routing.root,
        ("ls-files", "--others", "--exclude-standard", "-z"),
        extra_env=_source_environment(routing),
    ).split(b"\0")
    paths = tuple(sorted(os.fsdecode(row) for row in rows if row))
    for path in paths:
        if "\n" in path or "\r" in path:
            _fail(f"untracked path cannot be represented by the approval manifest: {path!r}")
    return paths


def _path_identity(root: Path, relative: str) -> bytes:
    path = root / relative
    try:
        info = path.lstat()
    except FileNotFoundError:
        return b"absent"
    prefix = f"{relative}\0{stat.S_IMODE(info.st_mode):o}\0".encode()
    if stat.S_ISREG(info.st_mode):
        return prefix + b"file\0" + hashlib.sha256(path.read_bytes()).digest()
    if stat.S_ISLNK(info.st_mode):
        return prefix + b"link\0" + hashlib.sha256(os.fsencode(path.readlink())).digest()
    _fail(f"special file is not an allowed candidate input: {relative}")


def _source_state(routing: SourceRouting) -> SourceState:
    entries = _index_entries(routing)
    tracked = tuple(path for path, _mode, _stage in entries)
    untracked = _untracked(routing)
    digest = hashlib.sha256()
    for path in (*tracked, *untracked):
        digest.update(_path_identity(routing.root, path))
        digest.update(b"\0")
    return SourceState(
        hashlib.sha256(routing.index.read_bytes()).hexdigest(),
        digest.hexdigest(),
        tracked,
        untracked,
    )


def _head_policy_population(routing: SourceRouting) -> tuple[tuple[str, str, str], ...]:
    rows = _git(
        routing.root,
        ("ls-tree", "-rz", "--full-tree", routing.head),
        extra_env=_source_environment(routing),
    ).split(b"\0")
    population: list[tuple[str, str, str]] = []
    for row in rows:
        if not row:
            continue
        metadata, separator, raw_path = row.partition(b"\t")
        fields = metadata.split()
        if not separator or len(fields) != TREE_ROW_FIELDS:
            _fail("malformed HEAD tree row")
        mode, object_type, blob = (os.fsdecode(field) for field in fields)
        path = os.fsdecode(raw_path)
        if Path(path).name in {".gitattributes", ".gitignore"}:
            if object_type != "blob":
                _fail(f"HEAD policy path is not a blob: {path}")
            population.append((path, mode, blob))
    return tuple(sorted(population))


def _candidate_policy_population(
    routing: SourceRouting, state: SourceState
) -> tuple[tuple[str, str, str], ...]:
    population: list[tuple[str, str, str]] = []
    for relative in (*state.tracked, *state.untracked):
        if Path(relative).name not in {".gitattributes", ".gitignore"}:
            continue
        path = routing.root / relative
        try:
            info = path.lstat()
        except FileNotFoundError:
            _fail(f"bootstrap policy file was deleted: {relative}")
        if not stat.S_ISREG(info.st_mode) or path.is_symlink():
            _fail(f"bootstrap policy file is not regular: {relative}")
        mode = "100755" if info.st_mode & stat.S_IXUSR else "100644"
        blob = os.fsdecode(
            _git(
                routing.root,
                ("hash-object", "--no-filters", "--", relative),
                extra_env=_source_environment(routing),
            )
        ).strip()
        population.append((relative, mode, blob))
    return tuple(sorted(population))


def _verify_bootstrap_policy(routing: SourceRouting, state: SourceState) -> None:
    head = _head_policy_population(routing)
    candidate = _candidate_policy_population(routing, state)
    if candidate != head:
        _fail(
            "bootstrap requires the complete .gitattributes/.gitignore "
            "population byte-equal to HEAD"
        )
    attributes = sum(path.endswith(".gitattributes") for path, _mode, _blob in head)
    ignores = sum(path.endswith(".gitignore") for path, _mode, _blob in head)
    if (attributes, ignores) != (6, 27):
        _fail(f"bootstrap policy population changed: {attributes} attributes, {ignores} ignores")


def _approved_untracked(manifest: Path) -> tuple[str, ...]:
    try:
        rows = manifest.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        message = f"cannot read approved untracked manifest: {manifest}"
        raise CandidateError(message) from exc
    if any(not row or "\0" in row or "\r" in row for row in rows):
        _fail("approved untracked manifest contains an invalid row")
    if len(rows) != len(set(rows)) or rows != sorted(rows):
        _fail("approved untracked manifest must be sorted and unique")
    return tuple(rows)


def _refuse_reappeared_staged_deletion(routing: SourceRouting) -> None:
    rows = _git(
        routing.root,
        ("diff", "--cached", "--no-renames", "--name-status", "-z", routing.head),
        extra_env=_source_environment(routing),
    ).split(b"\0")
    for offset in range(0, len(rows) - 1, 2):
        status_text = os.fsdecode(rows[offset])
        path = os.fsdecode(rows[offset + 1])
        if status_text == "D" and os.path.lexists(routing.root / path):
            _fail(f"staged deletion reappeared in the worktree; disposition required: {path}")


def _write_alternates(repo: Path, objects: Sequence[Path]) -> None:
    target = repo / ".git/objects/info/alternates"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("".join(f"{path}\n" for path in objects), encoding="utf-8")


def _init_private(repo: Path) -> None:
    if repo.exists() or repo.is_symlink():
        _fail(f"private candidate target already exists: {repo}")
    template = repo.with_name(f"{repo.name}.empty-template")
    if template.exists() or template.is_symlink():
        _fail(f"private template target already exists: {template}")
    repo.mkdir(parents=True)
    template.mkdir()
    _git(repo, ("-c", f"init.templateDir={template}", "init", "--quiet"))
    template.rmdir()


def _assemble_union(routing: SourceRouting, output: Path) -> str:
    _init_private(output)
    candidate_index = output / ".git/candidate.index"
    candidate_objects = output / ".git/candidate-objects"
    candidate_objects.mkdir()
    shutil.copyfile(routing.index, candidate_index)
    _write_alternates(output, routing.objects)
    alternate_text = os.pathsep.join(str(path) for path in routing.objects)
    environment = {
        "GIT_DIR": str(output / ".git"),
        "GIT_WORK_TREE": str(routing.root),
        "GIT_INDEX_FILE": str(candidate_index),
        "GIT_OBJECT_DIRECTORY": str(candidate_objects),
        "GIT_ALTERNATE_OBJECT_DIRECTORIES": alternate_text,
        "GIT_LFS_SKIP_SMUDGE": "1",
    }
    _git(routing.root, ("add", "-A", "--", ":/"), extra_env=environment)
    tree = os.fsdecode(_git(routing.root, ("write-tree",), extra_env=environment)).strip()
    _write_alternates(output, (*routing.objects, candidate_objects))
    return tree


def _round_trip(output: Path, tree: str, objects: Sequence[Path]) -> None:
    materialized = output / "materialized"
    _init_private(materialized)
    _write_alternates(materialized, objects)
    _git(materialized, ("read-tree", tree))
    environment = {"GIT_LFS_SKIP_SMUDGE": "1"}
    _git(materialized, ("checkout-index", "--all"), extra_env=environment)
    if _git(materialized, ("ls-files", "--others", "--exclude-standard", "-z")):
        _fail("candidate round trip produced untracked paths")
    _git(materialized, ("add", "-A", "-f"))
    actual = os.fsdecode(_git(materialized, ("write-tree",))).strip()
    if actual != tree:
        _fail("candidate round trip changed tree bytes or modes")


def assemble_candidate(
    source: Path,
    output: Path,
    approved_manifest: Path,
    *,
    after_capture: Callable[[], None] | None = None,
) -> str:
    """Assemble current index, tracked worktree, and approved untracked bytes."""
    routing = _capture_routing(Path(os.path.realpath(source)))
    if Path(os.path.realpath(output)).is_relative_to(routing.root):
        _fail("private candidate target must be outside the source repository")
    before = _source_state(routing)
    if before.untracked != _approved_untracked(approved_manifest):
        _fail("nonignored untracked population differs from the reviewed manifest")
    _verify_bootstrap_policy(routing, before)
    _refuse_reappeared_staged_deletion(routing)
    if after_capture is not None:
        after_capture()
    tree = _assemble_union(routing, output)
    after = _source_state(routing)
    if after != before:
        _fail("source candidate inputs changed during assembly")
    candidate_objects = output / ".git/candidate-objects"
    _round_trip(output, tree, (*routing.objects, candidate_objects))
    return tree


def _fixture_git(root: Path, *args: str) -> bytes:
    return _git(root, args)


def _prepare_source_fixture(base: Path) -> tuple[Path, bytes]:
    """Create the complete staged/unstaged/untracked assembly fixture."""
    source = base / "source"
    source.mkdir()
    _fixture_git(source, "init", "--quiet")
    _fixture_git(source, "config", "user.email", "selftest@invalid")
    _fixture_git(source, "config", "user.name", "selftest")
    (source / ".gitattributes").write_text(
        "*.txt text eol=crlf\n*.bin filter=lfs diff=lfs -text\n", encoding="ascii"
    )
    (source / ".gitignore").write_text("ignored.tmp\n", encoding="ascii")
    for index in range(1, 6):
        nested = source / f"p{index}"
        nested.mkdir()
        (nested / ".gitattributes").write_text("*.txt text\n", encoding="ascii")
    for index in range(1, 27):
        nested = source / f"i{index}"
        nested.mkdir()
        (nested / ".gitignore").write_text("scratch\n", encoding="ascii")
    (source / "tracked.txt").write_text("base\n", encoding="ascii")
    (source / "delete.txt").write_text("delete\n", encoding="ascii")
    executable = source / "run.sh"
    executable.write_text("#!/bin/sh\n", encoding="ascii")
    executable.chmod(0o755)
    (source / "target").write_text("target\n", encoding="ascii")
    (source / "link").symlink_to("target")
    pointer = b"version https://git-lfs.github.com/spec/v1\noid sha256:" + b"0" * 64 + b"\nsize 0\n"
    (source / "asset.bin").write_bytes(pointer)
    _fixture_git(source, "add", "-A")
    _fixture_git(source, "commit", "--quiet", "-m", "base")
    return source, pointer


def _apply_candidate_changes(base: Path, source: Path) -> Path:
    """Apply every candidate input class and a source-local filter attack."""
    marker = base / "filter.ran"
    helper = base / "filter.sh"
    helper.write_text(f"#!/bin/sh\nprintf x >>{marker}\ncat\n", encoding="ascii")
    helper.chmod(0o755)
    _fixture_git(source, "config", "filter.lfs.clean", str(helper))
    _fixture_git(source, "config", "filter.lfs.smudge", str(helper))
    _fixture_git(source, "config", "filter.lfs.required", "true")
    (source / "tracked.txt").write_bytes(b"changed\r\n")
    (source / "delete.txt").unlink()
    (source / "new.txt").write_text("new\n", encoding="ascii")
    (source / "ignored.tmp").write_text("ignored\n", encoding="ascii")
    return marker


def _assert_candidate(base: Path, source: Path, pointer: bytes, marker: Path) -> None:
    """Verify byte, mode, ignore, deletion, and filter semantics."""
    manifest = base / "approved.txt"
    manifest.write_text("new.txt\n", encoding="ascii")
    tree = assemble_candidate(source, base / "candidate", manifest)
    materialized = base / "candidate/materialized"
    if marker.exists():
        _fail("source-local clean/smudge filter executed during candidate assembly")
    if (materialized / "ignored.tmp").exists() or (materialized / "delete.txt").exists():
        _fail("ignored junk or a deleted tracked path entered the candidate")
    if (materialized / "asset.bin").read_bytes() != pointer:
        _fail("LFS pointer changed during strict materialization")
    if (materialized / "tracked.txt").read_bytes() != b"changed\r\n":
        _fail("built-in CRLF checkout semantics were not preserved")
    if not (materialized / "link").is_symlink() or not os.access(materialized / "run.sh", os.X_OK):
        _fail("symlink or executable mode was not preserved")
    if not tree:
        _fail("candidate assembly returned an empty tree identity")


def _assert_toctou_refused(base: Path, source: Path) -> None:
    """Prove a worktree byte race invalidates the candidate."""
    target = base / "toctou"
    shutil.copytree(source, target, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    _fixture_git(target, "init", "--quiet")
    _fixture_git(target, "config", "user.email", "selftest@invalid")
    _fixture_git(target, "config", "user.name", "selftest")
    _fixture_git(target, "add", "-A")
    _fixture_git(target, "commit", "--quiet", "-m", "base")
    manifest = base / "toctou-approved.txt"
    manifest.write_text("", encoding="ascii")

    def mutate() -> None:
        (target / "tracked.txt").write_text("raced\n", encoding="ascii")

    try:
        assemble_candidate(target, base / "toctou-candidate", manifest, after_capture=mutate)
    except CandidateError as exc:
        if "changed during assembly" not in str(exc):
            raise
    else:
        _fail("candidate assembly accepted a source mutation after capture")


def _assert_staged_deletion_disposition(base: Path) -> None:
    """Accept an absent staged deletion and reject every reappeared entry."""
    source = base / "staged-deletion"
    source.mkdir()
    _fixture_git(source, "init", "--quiet")
    _fixture_git(source, "config", "user.email", "selftest@invalid")
    _fixture_git(source, "config", "user.name", "selftest")
    victim = source / "victim"
    victim.write_text("tracked\n", encoding="ascii")
    _fixture_git(source, "add", "victim")
    _fixture_git(source, "commit", "--quiet", "-m", "tracked victim")
    victim.unlink()
    _fixture_git(source, "add", "-u", "--", "victim")
    routing = _capture_routing(source)
    _refuse_reappeared_staged_deletion(routing)

    victim.write_text("resurrected\n", encoding="ascii")
    try:
        _refuse_reappeared_staged_deletion(routing)
    except CandidateError:
        pass
    else:
        _fail("regular-file resurrection of a staged deletion was accepted")

    victim.unlink()
    victim.symlink_to("missing-target")
    if victim.exists() or not os.path.lexists(victim):
        _fail("dangling-symlink control did not prove exists false / lexists true")
    try:
        _refuse_reappeared_staged_deletion(routing)
    except CandidateError:
        pass
    else:
        _fail("dangling-symlink resurrection of a staged deletion was accepted")


def _assert_alternate_routing(base: Path) -> None:
    """Prove absolute alternates work and ambiguous inherited forms fail closed."""
    source = base / "alternate-source"
    source.mkdir()
    _fixture_git(source, "init", "--quiet")
    _fixture_git(source, "config", "user.email", "selftest@invalid")
    _fixture_git(source, "config", "user.name", "selftest")
    (source / "tracked.txt").write_text("alternate object\n", encoding="ascii")
    _fixture_git(source, "add", "tracked.txt")
    _fixture_git(source, "commit", "--quiet", "-m", "alternate")
    objects = source / ".git/objects"
    alternate = base / "alternate-objects"
    objects.rename(alternate)
    objects.mkdir()
    (objects / "info").mkdir()
    (objects / "pack").mkdir()
    second = base / "second-alternate"
    second.mkdir()

    original = os.environ.get("GIT_ALTERNATE_OBJECT_DIRECTORIES")
    try:
        os.environ["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = os.pathsep.join(
            (str(alternate), str(second))
        )
        routing = _capture_routing(source)
        if alternate not in routing.objects or second not in routing.objects:
            _fail("multiple absolute inherited alternates were not normalized")
        if not _source_state(routing).tracked:
            _fail("source objects were not readable through an inherited alternate")

        invalid = (
            "relative-objects",
            f"{alternate}{os.pathsep}",
            f'"{alternate}"',
            f"{alternate}\n{second}",
            str(base / "missing-alternate"),
        )
        for value in invalid:
            os.environ["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = value
            try:
                _capture_routing(source)
            except CandidateError:
                continue
            _fail(f"unsafe inherited alternate was accepted: {value!r}")
    finally:
        if original is None:
            os.environ.pop("GIT_ALTERNATE_OBJECT_DIRECTORIES", None)
        else:
            os.environ["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = original


def selftest() -> None:
    """Prove union semantics, hostile-filter isolation, and TOCTOU refusal."""
    with tempfile.TemporaryDirectory(prefix="ra8-candidate-selftest-") as temp:
        base = Path(temp)
        source, pointer = _prepare_source_fixture(base)
        marker = _apply_candidate_changes(base, source)
        _assert_candidate(base, source, pointer, marker)
        _assert_toctou_refused(base, source)
        _assert_staged_deletion_disposition(base)
        _assert_alternate_routing(base)
    print("assemble_candidate.py --selftest: PASS")


def main() -> int:
    """Parse the strict candidate-assembly command line."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--approved-untracked", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        if any(value is not None for value in (args.source, args.output, args.approved_untracked)):
            parser.error("--selftest takes no assembly paths")
        selftest()
        return 0
    if args.source is None or args.output is None or args.approved_untracked is None:
        parser.error("--source, --output, and --approved-untracked are required")
    tree = assemble_candidate(args.source, args.output, args.approved_untracked)
    print(tree)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

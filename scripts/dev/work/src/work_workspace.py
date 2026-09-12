# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Read-only client for canonical ``agent_workspace.sh`` ownership metadata."""

from __future__ import annotations

import os
import re
import shlex
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

from work_git import WorkError, discover_repo, resolve_commit, run_git_readonly

OWNER = "work"
SCHEMA = "2"
KEY_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")
ISSUE_RE = re.compile(r"^[1-9][0-9]{0,9}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40,64}$")
EXPECTED_FIELDS = {
    "schema",
    "name",
    "created",
    "by",
    "ref",
    "base_commit",
    "path",
    "branch",
    "owner",
}

READY = "READY"
STALE = "STALE"
FOREIGN = "FOREIGN"
FORGED = "FORGED"


class ClaimError(WorkError):
    """Canonical workspace metadata is unreadable or contradictory."""


def fail(message: str) -> NoReturn:
    """Raise one metadata error."""
    raise ClaimError(message)


def is_identifier(value: str) -> bool:
    """Return whether ``value`` is an issue number or safe plan key."""
    return ISSUE_RE.fullmatch(value) is not None or KEY_RE.fullmatch(value) is not None


def workspace_name(identifier: str) -> str:
    """Return the canonical workspace name for one work identifier."""
    if not is_identifier(identifier):
        fail("invalid work identifier")
    return f"work-{identifier}"


def branch_name(identifier: str) -> str:
    """Return the canonical branch for one work identifier."""
    if not is_identifier(identifier):
        fail("invalid work identifier")
    return f"work/{identifier}"


def metadata_dir(ws_root: Path) -> Path:
    """Return the canonical workspace metadata directory."""
    return ws_root / ".meta"


def metadata_path(ws_root: Path, identifier: str) -> Path:
    """Return one canonical metadata path."""
    return metadata_dir(ws_root) / workspace_name(identifier)


def lexical_absolute(path: Path) -> Path:
    """Return an absolute normalized path without following any symlink.

    Args:
        path: Path supplied by configuration or metadata.

    Returns:
        An absolute lexical path. ``..`` components are normalized, while a
        symlinked child remains visible to the later ``lstat`` check.
    """
    return Path(
        os.path.abspath(  # noqa: PTH100 -- preserve symlink evidence for lstat
            os.fspath(path.expanduser())
        )
    )


@dataclass(frozen=True)
class Claim:
    """One canonical workspace record owned by the work client."""

    identifier: str
    name: str
    worktree: Path
    branch: str
    base_ref: str
    base_commit: str
    created: str
    creator: str


def _single_line(value: str, field: str) -> str:
    """Require safe single-line metadata text."""
    if not value or not value.isascii() or not value.isprintable():
        fail(f"metadata field {field} must be non-empty printable single-line ASCII")
    return value


def _read_fields(path: Path) -> dict[str, str]:
    """Read one private, regular canonical metadata record."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        fail(f"canonical metadata is absent: {path}")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        fail(f"canonical metadata is not a regular file: {path}")
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        fail(f"canonical metadata could not be read: {path}")
    fields: dict[str, str] = {}
    for line in lines:
        key, separator, value = line.partition("=")
        if not separator or key in fields:
            fail(f"canonical metadata has an invalid or duplicate field: {path}")
        fields[key] = _single_line(value, key)
    if set(fields) != EXPECTED_FIELDS or fields.get("schema") != SCHEMA:
        fail(f"canonical metadata schema is not supported: {path}")
    return fields


def load_claim(path: Path, ws_root: Path) -> Claim:
    """Load and internally validate one work-owned canonical record."""
    fields = _read_fields(path)
    name = fields["name"]
    if fields["owner"] != OWNER or path.name != name or not name.startswith("work-"):
        fail(f"metadata is not a canonical work-owned claim: {path}")
    identifier = name.removeprefix("work-")
    root = lexical_absolute(ws_root)
    expected_path = root / name
    recorded_path = lexical_absolute(Path(fields["path"]))
    checks = (
        is_identifier(identifier),
        fields["branch"] == branch_name(identifier),
        recorded_path.is_absolute(),
        recorded_path == expected_path,
        COMMIT_RE.fullmatch(fields["base_commit"]) is not None,
    )
    if not all(checks):
        fail(f"canonical work claim contradicts its derived identity: {path}")
    try:
        worktree_info = expected_path.lstat()
    except FileNotFoundError:
        worktree_info = None
    if worktree_info is not None and (
        stat.S_ISLNK(worktree_info.st_mode) or not stat.S_ISDIR(worktree_info.st_mode)
    ):
        fail(f"canonical worktree path is not a real directory: {expected_path}")
    return Claim(
        identifier=identifier,
        name=name,
        worktree=recorded_path,
        branch=fields["branch"],
        base_ref=fields["ref"],
        base_commit=fields["base_commit"],
        created=fields["created"],
        creator=fields["by"],
    )


def list_claims(ws_root: Path) -> list[tuple[Path, Claim | ClaimError]]:
    """List canonical work claims without interpreting agent-owned metadata."""
    directory = metadata_dir(ws_root)
    if not directory.exists():
        return []
    if not directory.is_dir() or directory.is_symlink():
        fail(f"canonical metadata directory is unsafe: {directory}")
    found: list[tuple[Path, Claim | ClaimError]] = []
    for path in sorted(directory.iterdir(), key=lambda item: item.name):
        try:
            if path.name.startswith("work-"):
                found.append((path, load_claim(path, ws_root)))
            else:
                fields = _read_fields(path)
                if fields.get("owner") == OWNER:
                    found.append((path, ClaimError(f"work owner used a nonreserved name: {path}")))
        except ClaimError as exc:
            # A malformed record using the reserved work- prefix is visible;
            # unrelated legacy/agent records remain the canonical tool's concern.
            if path.name.startswith("work-"):
                found.append((path, exc))
    return found


def _worktree_bindings(cwd: Path) -> dict[Path, tuple[str | None, str | None]]:
    """Return registered path -> (branch, HEAD) bindings from Git porcelain."""
    done = run_git_readonly(["worktree", "list", "--porcelain"], cwd=cwd)
    if not done.ok:
        fail("git worktree inventory failed")
    result: dict[Path, tuple[str | None, str | None]] = {}
    path: Path | None = None
    head: str | None = None
    branch: str | None = None
    for line in [*done.stdout.splitlines(), ""]:
        if not line:
            if path is not None:
                result[lexical_absolute(path)] = (branch, head)
            path, head, branch = None, None, None
        elif line.startswith("worktree "):
            path = Path(line.removeprefix("worktree "))
        elif line.startswith("HEAD "):
            head = line.removeprefix("HEAD ")
        elif line.startswith("branch refs/heads/"):
            branch = line.removeprefix("branch refs/heads/")
    return result


def classify(claim: Claim, cwd: Path) -> str:
    """Verify that the registered worktree is attached to the claimed branch."""
    try:
        info = claim.worktree.lstat()
    except FileNotFoundError:
        return STALE
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        return FORGED
    binding = _worktree_bindings(cwd).get(lexical_absolute(claim.worktree))
    if binding is None:
        return FOREIGN
    branch, registered_head = binding
    if branch != claim.branch or registered_head is None:
        return FOREIGN
    return _classify_repository(claim, cwd, registered_head)


def _classify_repository(claim: Claim, cwd: Path, registered_head: str) -> str:
    """Verify repository identity and immutable object bindings for one claim."""
    try:
        caller_repo = discover_repo(cwd)
        claim_repo = discover_repo(claim.worktree)
    except WorkError:
        return FORGED
    if claim_repo.common_dir != caller_repo.common_dir:
        return FORGED
    head = resolve_commit("HEAD", cwd=claim.worktree)
    base = resolve_commit(claim.base_commit, cwd=claim.worktree)
    if head != registered_head or base != claim.base_commit:
        return FORGED
    return READY


def recovery_command(claim: Claim, repo_root: Path, *, stale: bool) -> str:
    """Return one shell-quoted canonical recovery command for human review."""
    action = "forget" if stale else "release"
    return shlex.join(
        ["/bin/bash", "-p", str(repo_root / "scripts/dev/agent_workspace.sh"), action, claim.name]
    )

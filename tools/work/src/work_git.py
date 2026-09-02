# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Process choke point, token redaction, and the git adapter behind ``work``.

Every subprocess this tool starts goes through :func:`run_process`, and every
byte handed back from one has already passed :func:`redact`. That is
deliberate. A GitHub token reaches a repository tool through a remote URL, an
askpass helper, or a ``gh`` diagnostic far more often than through anything the
tool itself holds, and a single choke point is the only arrangement in which
"is this output redacted" has one answer rather than one answer per call site.

The git surface is split in two on purpose. :func:`run_git_readonly` refuses
any argv whose subcommand is outside :data:`READ_ONLY_SUBCOMMANDS`, so a path
documented as read-only -- ``work landed`` is the main reason the guard exists
-- cannot quietly acquire a ``push`` or a ``branch -D`` in a later edit.
The unguarded process adapter is private to that wrapper; workspace creation
belongs exclusively to ``scripts/dev/agent_workspace.sh``.

Nothing in this module fetches, pushes, or otherwise reaches the network, and
nothing here deletes a file, a branch, or a worktree.

Failure is signalled by raising :class:`WorkError` or one of its subclasses;
the exit-code mapping belongs to ``work.py``.
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/dev"))

from git_environment import (
    GitEnvironmentError,
    reject_untrusted_executable_attributes,
    sanitized_git_environment,
    trusted_git_executable,
)

#: Wall-clock ceiling for any single captured command, in seconds.
DEFAULT_TIMEOUT_S = 60

#: Replacement text substituted for anything token-shaped.
REDACTED = "[REDACTED-TOKEN]"

#: Bounds of the printable ASCII range :func:`printable` keeps, and what it
#: substitutes for everything else, including line terminators.
PRINTABLE_LOW = 0x20
PRINTABLE_HIGH = 0x7E
UNPRINTABLE = "?"

#: Shapes GitHub currently issues: the ``gh?_`` family and fine-grained PATs.
TOKEN_PATTERNS = (
    re.compile(r"gh[pousr]_[A-Za-z0-9]{16,}"),
    re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
)

#: Per-subcommand allowlist of the EXACT option spellings this tool uses. The
#: guard is a whitelist rather than a blacklist because "read-only subcommand"
#: is not a property of the subcommand at all: ``git diff --output=FILE`` and
#: ``git log --output=FILE`` both truncate and create files, and both sailed
#: through a guard that only looked at the first word. Any token starting with
#: ``-`` that is not listed here for its own subcommand is refused.
READ_ONLY_OPTIONS: dict[str, frozenset[str]] = {
    "rev-parse": frozenset(
        {"--show-toplevel", "--git-common-dir", "--git-dir", "--verify", "--quiet"}
    ),
    "status": frozenset({"--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none"}),
    "diff": frozenset({"--stat", "--no-ext-diff", "--no-textconv"}),
    "log": frozenset(),
    "show-ref": frozenset({"--verify", "--quiet"}),
    "worktree": frozenset({"--porcelain"}),
    "branch": frozenset({"--list"}),
}

#: git subcommands that cannot change repository state. Derived from the
#: allowlist above so the two can never disagree about what is permitted.
READ_ONLY_SUBCOMMANDS = frozenset(READ_ONLY_OPTIONS)

#: Subcommands whose first positional argument selects a mode, and the only
#: mode each may select. ``git worktree list`` reads; ``git worktree add`` and
#: ``git worktree remove`` very much do not.
READ_ONLY_MODES: dict[str, str] = {"worktree": "list"}

#: Options a subcommand must carry to be the reading form of itself.
REQUIRED_OPTIONS: dict[str, str] = {"branch": "--list"}

#: Fixed Git prefix applied by this module after the caller argv has passed the
#: read-only guard. Repository configuration is deliberately overridden after
#: it is loaded: a local ``core.fsmonitor`` helper is executable code, while a
#: pager, external diff, or untracked-cache override can hide the truth the
#: workflow client is asking Git to report.
GIT_READ_PREFIX = (
    "--no-pager",
    "--no-optional-locks",
    "-c",
    "core.fsmonitor=false",
    "-c",
    "core.untrackedCache=false",
    "-c",
    "core.pager=cat",
    "-c",
    "pager.status=false",
    "-c",
    "pager.diff=false",
    "-c",
    "diff.external=",
)


class WorkError(Exception):
    """Any condition that stops ``work`` from producing a trustworthy answer."""


class ToolMissingError(WorkError):
    """A required external executable is absent from ``PATH``."""


class GitCommandError(WorkError):
    """A git command ran and reported a non-zero status."""


class GitWriteAttemptError(WorkError):
    """An argv that could change repository state reached a read-only runner."""


def redact(text: str) -> str:
    """Replace every token-shaped substring in ``text`` with a fixed placeholder.

    Args:
        text: Arbitrary captured output or exception text.

    Returns:
        The same text with each GitHub-token-shaped run replaced by
        :data:`REDACTED`.
    """
    out = text
    for pattern in TOKEN_PATTERNS:
        out = pattern.sub(REDACTED, out)
    return out


def printable(text: str) -> str:
    """Reduce ``text`` to printable ASCII, so nothing it contains can rewrite a report.

    A manifest is a file on disk that anyone able to write the state directory
    can author, and several of its fields are pure display data that no
    validation constrains -- ``base_ref`` and ``creator`` most obviously. An
    escape sequence in one of those repositions the cursor and repaints the
    line, so a planted record could make ``work landed`` report a base, a
    branch or an author that is not what it actually found.

    Report callers pass one logical line at a time. Newlines and every other
    character outside space..tilde therefore become :data:`UNPRINTABLE`; a
    planted metadata field cannot inject a second, trusted-looking row.

    Args:
        text: Any string on its way to stdout or stderr.

    Returns:
        The same text with every non-printable character replaced.
    """
    return "".join(
        char if PRINTABLE_LOW <= ord(char) <= PRINTABLE_HIGH else UNPRINTABLE for char in text
    )


@dataclass(frozen=True)
class Completed:
    """One captured subprocess result, already redacted."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        """Whether the process exited zero."""
        return self.returncode == 0


def run_process(
    argv: Sequence[str],
    *,
    cwd: Path | None = None,
    timeout: int = DEFAULT_TIMEOUT_S,
    env: Mapping[str, str] | None = None,
) -> Completed:
    """Run ``argv`` with output captured, and return the redacted result.

    Args:
        argv: Full argument vector, executable first. Never a shell string.
        cwd: Directory to run from, or None to inherit the caller's.
        timeout: Seconds before the child is killed and the call fails.
        env: Explicit child environment, or None to inherit the caller's.

    Returns:
        A :class:`Completed` whose streams have passed :func:`redact`.

    Raises:
        ToolMissingError: The executable named by ``argv[0]`` does not exist.
        WorkError: The command exceeded ``timeout``.
    """
    listed = list(argv)
    try:
        proc = subprocess.run(  # noqa: S603 -- fixed argv list, no shell, resolved executable
            listed,
            cwd=None if cwd is None else str(cwd),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=None if env is None else dict(env),
            check=False,
        )
    except FileNotFoundError as exc:
        msg = f"executable not found: {redact(str(exc))}"
        raise ToolMissingError(msg) from exc
    except subprocess.TimeoutExpired as exc:
        msg = f"command timed out after {timeout}s: {redact(' '.join(listed))}"
        raise WorkError(msg) from exc
    return Completed(
        argv=tuple(listed),
        returncode=proc.returncode,
        stdout=redact(proc.stdout or ""),
        stderr=redact(proc.stderr or ""),
    )


def git_executable() -> str:
    """Return the repository's one absolute control-plane Git authority."""
    try:
        return trusted_git_executable()
    except GitEnvironmentError as exc:
        raise ToolMissingError(str(exc)) from exc


def git_child_environment() -> dict[str, str]:
    """Return the shared hardened environment used by every child Git.

    Returns:
        The environment produced by the repository-wide nested-Git authority.
    """
    return sanitized_git_environment()


def _subcommand_index(argv: Sequence[str]) -> int:
    """Return the index of the git subcommand in ``argv``, or -1 if there is none.

    The subcommand must be argv[0]. No global option, including ``-C``, is
    skipped: a leading option remains the token :func:`assert_read_only` judges
    and is refused rather than silently reinterpreted.

    Args:
        argv: A git argument vector with the executable already removed.

    Returns:
        Index of the subcommand token, or -1 when the vector has none.
    """
    return 0 if argv else -1


def git_subcommand(argv: Sequence[str]) -> str | None:
    """Return argv[0] as the Git subcommand; never skip global options.

    Args:
        argv: A git argument vector with the executable already removed.

    Returns:
        The subcommand token, or None when the vector names none.
    """
    index = _subcommand_index(argv)
    return None if index < 0 else argv[index]


def _assert_subcommand(argv: Sequence[str]) -> int:
    """Check everything before the subcommand, and return where it starts.

    Args:
        argv: A git argument vector with the executable already removed.

    Returns:
        The index of the subcommand token.

    Raises:
        GitWriteAttemptError: The vector carries a global option other than the
            ``-C <path>`` pair, names no subcommand, or names one outside
            :data:`READ_ONLY_SUBCOMMANDS`.
    """
    index = _subcommand_index(argv)
    if index < 0:
        msg = "read-only git runner received an argv with no subcommand"
        raise GitWriteAttemptError(msg)
    sub = argv[index]
    if sub.startswith("-"):
        msg = f"read-only git runner refused the global option: {sub}"
        raise GitWriteAttemptError(msg)
    if sub not in READ_ONLY_SUBCOMMANDS:
        msg = f"read-only git runner refused subcommand: {sub}"
        raise GitWriteAttemptError(msg)
    return index


def _assert_tail(sub: str, tail: Sequence[str]) -> None:
    """Check every argument after the subcommand against that subcommand's allowlist.

    Args:
        sub: The subcommand, already known to be permitted.
        tail: Everything after it.

    Raises:
        GitWriteAttemptError: An option is not allowlisted for ``sub``, a
            required option is absent, or a mode-selecting positional is either
            wrong or duplicated.
    """
    allowed = READ_ONLY_OPTIONS[sub]
    for token in tail:
        if token.startswith("-") and token not in allowed:
            msg = f"read-only git runner refused option {token!r} for subcommand {sub!r}"
            raise GitWriteAttemptError(msg)
    required = REQUIRED_OPTIONS.get(sub)
    if required is not None and required not in tail:
        msg = f"read-only git runner requires {required} for subcommand {sub!r}"
        raise GitWriteAttemptError(msg)
    mode = READ_ONLY_MODES.get(sub)
    if mode is None:
        return
    positionals = [token for token in tail if not token.startswith("-")]
    if positionals != [mode]:
        msg = f"read-only git runner allows only: git {sub} {mode}"
        raise GitWriteAttemptError(msg)


def assert_read_only(argv: Sequence[str]) -> None:
    """Raise unless ``argv`` is a git invocation that cannot change any state.

    The check is a whitelist at three levels -- the global prefix, the
    subcommand, and every single option after it -- because none of the three
    is safe on its own. ``git diff --output=FILE`` truncates a file while being
    a "read-only subcommand", and ``git worktree list add -b x /tmp/z HEAD``
    is a write wearing a listing subcommand as a hat.

    Args:
        argv: A git argument vector with the executable already removed.

    Raises:
        GitWriteAttemptError: The vector is not one of the exact forms this
            tool issues.
    """
    index = _assert_subcommand(argv)
    _assert_tail(argv[index], list(argv[index + 1 :]))


def _run_git(argv: Sequence[str], *, cwd: Path, timeout: int = DEFAULT_TIMEOUT_S) -> Completed:
    """Run git with ``argv`` from ``cwd`` with no read-only guard applied.

    Args:
        argv: Git arguments with the executable omitted.
        cwd: Directory to run from.
        timeout: Seconds before the child is killed.

    Returns:
        The captured, redacted result.
    """
    return run_process(
        [git_executable(), *GIT_READ_PREFIX, *argv],
        cwd=cwd,
        timeout=timeout,
        env=git_child_environment(),
    )


def run_git_readonly(
    argv: Sequence[str], *, cwd: Path, timeout: int = DEFAULT_TIMEOUT_S
) -> Completed:
    """Run a git command that has been proved incapable of changing state.

    Args:
        argv: Git arguments with the executable omitted.
        cwd: Directory to run from.
        timeout: Seconds before the child is killed.

    Returns:
        The captured, redacted result.

    Raises:
        GitWriteAttemptError: ``argv`` did not pass :func:`assert_read_only`.
    """
    assert_read_only(argv)
    return _run_git(argv, cwd=cwd, timeout=timeout)


def git_text(argv: Sequence[str], *, cwd: Path) -> str:
    """Run a read-only git command and return its stdout, failing loudly.

    Args:
        argv: Git arguments with the executable omitted.
        cwd: Directory to run from.

    Returns:
        Captured stdout.

    Raises:
        GitCommandError: The command exited non-zero.
    """
    done = run_git_readonly(argv, cwd=cwd)
    if not done.ok:
        joined = " ".join(argv)
        msg = f"git {joined} failed (exit {done.returncode}): {done.stderr.strip()}"
        raise GitCommandError(msg)
    return done.stdout


@dataclass(frozen=True)
class RepoPaths:
    """Where the repository under the caller's feet actually lives."""

    toplevel: Path
    common_dir: Path
    git_dir: Path

    @property
    def is_linked_worktree(self) -> bool:
        """Whether this checkout is a linked worktree rather than the main one."""
        return self.git_dir != self.common_dir


def discover_repo(cwd: Path) -> RepoPaths:
    """Locate the repository containing ``cwd``.

    ``--git-common-dir`` is asked for rather than assuming ``.git`` is a
    directory: in a linked worktree ``.git`` is a file, and the shared state
    this tool writes must land beside the main repository rather than once per
    worktree.

    Args:
        cwd: Any directory inside the repository.

    Returns:
        The resolved toplevel, common git directory, and per-worktree git
        directory.

    Raises:
        GitCommandError: ``cwd`` is not inside a git repository.
    """
    toplevel = Path(git_text(["rev-parse", "--show-toplevel"], cwd=cwd).strip()).resolve()
    common = _resolve_git_path(git_text(["rev-parse", "--git-common-dir"], cwd=cwd).strip(), cwd)
    git_dir = _resolve_git_path(git_text(["rev-parse", "--git-dir"], cwd=cwd).strip(), cwd)
    return RepoPaths(toplevel=toplevel, common_dir=common, git_dir=git_dir)


def reject_executable_attributes(cwd: Path, commit: str | None = None) -> None:
    """Apply the repository-wide trusted attribute policy as a workflow error."""
    try:
        reject_untrusted_executable_attributes(cwd, commit)
    except GitEnvironmentError as exc:
        raise WorkError(str(exc)) from exc


def _resolve_git_path(raw: str, cwd: Path) -> Path:
    """Turn a possibly relative ``git rev-parse`` path answer into an absolute one.

    Args:
        raw: The path git printed.
        cwd: The directory the command ran from, which relative answers are
            relative to.

    Returns:
        An absolute, symlink-resolved path.
    """
    path = Path(raw)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve()


def worktree_paths(cwd: Path) -> list[Path]:
    """Return the resolved path of every worktree registered in this repository.

    Args:
        cwd: Any directory inside the repository.

    Returns:
        Resolved worktree paths, in the order git reported them.
    """
    out = git_text(["worktree", "list", "--porcelain"], cwd=cwd)
    marker = "worktree "
    return [
        Path(line[len(marker) :]).resolve() for line in out.splitlines() if line.startswith(marker)
    ]


def branch_exists(name: str, *, cwd: Path) -> bool:
    """Whether a local branch of exactly ``name`` exists.

    Args:
        name: Branch name without the ``refs/heads/`` prefix.
        cwd: Any directory inside the repository.

    Returns:
        True when the ref resolves.
    """
    done = run_git_readonly(["show-ref", "--verify", "--quiet", f"refs/heads/{name}"], cwd=cwd)
    return done.ok


def resolve_commit(ref: str, *, cwd: Path) -> str | None:
    """Resolve ``ref`` to a commit id without touching the network.

    Args:
        ref: Any revision expression.
        cwd: Any directory inside the repository.

    Returns:
        The full commit id, or None when the reference does not resolve.
    """
    done = run_git_readonly(["rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"], cwd=cwd)
    text = done.stdout.strip()
    return text if done.ok and text else None


def resolve_tree(ref: str, *, cwd: Path) -> str | None:
    """Resolve the tree object belonging to ``ref`` without touching the network.

    Args:
        ref: Commit-ish whose content tree is required.
        cwd: Any directory inside the repository.

    Returns:
        The full tree object id, or None when the reference does not resolve.
    """
    done = run_git_readonly(["rev-parse", "--verify", "--quiet", f"{ref}^{{tree}}"], cwd=cwd)
    text = done.stdout.strip()
    return text if done.ok and text else None


def porcelain_status(cwd: Path) -> list[str]:
    """Return the ``git status --porcelain`` lines for the tree at ``cwd``.

    Args:
        cwd: A working tree.

    Returns:
        One entry per reported path, untracked files included.
    """
    reject_executable_attributes(cwd)
    out = git_text(
        ["status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none"],
        cwd=cwd,
    )
    return [line for line in out.splitlines() if line.strip()]


def diff_stat(cwd: Path, base: str) -> str:
    """Return ``git diff --stat <base>...HEAD`` for the tree at ``cwd``.

    Args:
        cwd: A working tree.
        base: The base revision to compare against.

    Returns:
        The diffstat text, or an explanatory line when the base does not
        resolve in that tree.
    """
    reject_executable_attributes(cwd)
    done = run_git_readonly(
        ["diff", "--no-ext-diff", "--no-textconv", "--stat", f"{base}...HEAD"],
        cwd=cwd,
    )
    if not done.ok:
        return f"(no diffstat: base {base} did not resolve here)"
    return done.stdout.rstrip()

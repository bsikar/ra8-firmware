#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Keep nested Git fixtures independent from the invoking hook repository.

Git exports repository-local environment variables while running hooks.  A
``git -C <temporary-directory>`` command does not override those variables, so
an otherwise isolated selftest can read, stage, or commit the caller's index.
Every child Git writer and fixture must enter :func:`isolated_git_environment`
or pass :func:`sanitized_git_environment` before its first Git command.
Read-only real-tree queries that deliberately judge the caller's index may
inherit that routing; writers never may.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import stat
import subprocess
import sys
import tempfile
from collections.abc import Iterator, Mapping, Sequence
from contextlib import contextmanager
from pathlib import Path

TRUSTED_GIT_PATH = Path("/usr/bin/git")
PUSH_CAPTURE_FIELDS = 4

LOCAL_GIT_ENVIRONMENT = (
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_COMMON_DIR",
    "GIT_CONFIG",
    "GIT_CONFIG_COUNT",
    "GIT_CONFIG_PARAMETERS",
    "GIT_DIR",
    "GIT_GRAFT_FILE",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_INTERNAL_SUPER_PREFIX",
    "GIT_NO_REPLACE_OBJECTS",
    "GIT_OBJECT_DIRECTORY",
    "GIT_PREFIX",
    "GIT_REPLACE_REF_BASE",
    "GIT_SHALLOW_FILE",
    "GIT_WORK_TREE",
)


class GitEnvironmentError(RuntimeError):
    """A nested Git fixture escaped its repository boundary."""


def _fail(message: str) -> None:
    """Raise a fixture-boundary error with caller-provided detail."""
    raise GitEnvironmentError(message)


def trusted_git_executable() -> str:
    """Return the one absolute Git executable allowed for control-plane work."""
    configured = os.environ.get("RA8_TRUSTED_GIT", str(TRUSTED_GIT_PATH))
    if configured != str(TRUSTED_GIT_PATH):
        _fail(f"refusing non-authority Git executable: {configured}")
    try:
        info = TRUSTED_GIT_PATH.lstat()
    except OSError as exc:
        message = "trusted /usr/bin/git is unavailable"
        raise GitEnvironmentError(message) from exc
    if not stat.S_ISREG(info.st_mode) or TRUSTED_GIT_PATH.is_symlink():
        _fail("trusted /usr/bin/git is not a regular non-symlink executable")
    if not os.access(TRUSTED_GIT_PATH, os.X_OK):
        _fail("trusted /usr/bin/git is not executable")
    return str(TRUSTED_GIT_PATH)


def sanitized_git_environment(
    source: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Return a noninteractive environment isolated from caller Git policy.

    Repository-local routing is only one way a nested Git command can escape
    its fixture. Global or system configuration can select an attributes file,
    and attributes can execute clean/smudge filters. Every inherited ``GIT_*``
    selector and executable-helper variable is therefore removed here, then
    the supported noninteractive controls are rebound to safe values.
    """
    environment = os.environ if source is None else source
    helper_environment = frozenset(
        {
            "DIFF",
            "EDITOR",
            "LESS",
            "LV",
            "MERGE_TOOL",
            "PAGER",
            "SSH_ASKPASS",
            "SUDO_ASKPASS",
            "VISUAL",
            "BASH_ENV",
            "ENV",
            "PYTHONHOME",
            "PYTHONPATH",
        }
    )
    clean = {
        name: value
        for name, value in environment.items()
        if not name.startswith(("GIT_", "BASH_FUNC_")) and name not in helper_environment
    }
    clean.update(
        {
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_EDITOR": "false",
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_PAGER": "cat",
            "RA8_TRUSTED_GIT": trusted_git_executable(),
            "GIT_SEQUENCE_EDITOR": "false",
            "GIT_SSH_COMMAND": "false",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_CONFIG_COUNT": "3",
            "GIT_CONFIG_KEY_0": "core.hooksPath",
            "GIT_CONFIG_VALUE_0": os.devnull,
            "GIT_CONFIG_KEY_1": "core.fsmonitor",
            "GIT_CONFIG_VALUE_1": "false",
            "GIT_CONFIG_KEY_2": "core.attributesFile",
            "GIT_CONFIG_VALUE_2": os.devnull,
            "PAGER": "cat",
            "TERM": "dumb",
        }
    )
    return clean


def _network_git_environment(source: Mapping[str, str]) -> dict[str, str]:
    """Keep operator transport policy while removing every other Git selector."""
    transport_names = {
        "GIT_ASKPASS",
        "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_NOSYSTEM",
        "GIT_CONFIG_SYSTEM",
        "GIT_SSH",
        "GIT_SSH_COMMAND",
        "GIT_SSH_VARIANT",
        "GIT_TERMINAL_PROMPT",
    }
    clean = {name: value for name, value in source.items() if not name.startswith("GIT_")}
    clean.update({name: source[name] for name in transport_names if name in source})
    clean.update({"GIT_OPTIONAL_LOCKS": "0", "GIT_PAGER": "cat", "PAGER": "cat", "TERM": "dumb"})
    return clean


@contextmanager
def isolated_git_environment() -> Iterator[None]:
    """Temporarily install the hardened child environment, then restore all bytes."""
    original = dict(os.environ)
    os.environ.clear()
    os.environ.update(sanitized_git_environment(original))
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(original)


def _git(root: Path, *args: str, clean: bool = True) -> bytes:
    """Run Git for the helper's synthetic fixture."""
    environment = sanitized_git_environment() if clean else os.environ.copy()
    proc = subprocess.run(  # noqa: S603 -- fixed executable and selftest argv
        [trusted_git_executable(), "-C", str(root), *args],
        env=environment,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = os.fsdecode(proc.stderr).strip()
        message = f"git {' '.join(args)} failed: {detail}"
        _fail(message)
    return proc.stdout


def _local_config_values(root: Path, key: str) -> tuple[str, ...]:
    """Return every local repository config value, preserving cardinality."""
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and validated config key
        [
            trusted_git_executable(),
            "-C",
            str(root),
            "config",
            "--local",
            "--null",
            "--get-all",
            key,
        ],
        env=sanitized_git_environment(),
        capture_output=True,
        check=False,
    )
    if proc.returncode == 1:
        return ()
    if proc.returncode != 0:
        detail = os.fsdecode(proc.stderr).strip()
        _fail(f"cannot read local Git config {key}: {detail}")
    return tuple(os.fsdecode(value) for value in proc.stdout.split(b"\0") if value)


def _attribute_text_tokens(text: str) -> list[str]:
    """Return the governed tokens from decoded Git attribute text."""
    tokens: list[str] = []
    for raw_line in text.splitlines():
        line = raw_line.lstrip()
        if line and not line.startswith("#"):
            tokens.extend(line.split()[1:])
    return tokens


def _attribute_tokens(path: Path) -> list[str]:
    """Return attribute tokens from one real, bounded UTF-8 attribute file."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return []
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        _fail(f"refusing non-regular Git attribute file: {path}")
    try:
        return _attribute_text_tokens(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as exc:
        message = f"refusing unreadable Git attribute file: {path}"
        raise GitEnvironmentError(message) from exc


def _trusted_attribute(token: str) -> bool:
    """Return whether an executable-shaped token matches the fixed tree policy."""
    name, separator, value = token.partition("=")
    if not separator:
        return False
    return (name == "diff" and value in {"c", "cpp", "lfs"}) or (
        name == "filter" and value == "lfs"
    )


def _validate_local_driver_config(root: Path) -> None:
    """Reject unexpected filter definitions and executable diff drivers."""
    expected = {
        "filter.lfs.clean": "git-lfs clean -- %f",
        "filter.lfs.process": "git-lfs filter-process",
        "filter.lfs.required": "true",
        "filter.lfs.smudge": "git-lfs smudge -- %f",
    }
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and config query
        [
            trusted_git_executable(),
            "-C",
            str(root),
            "config",
            "--local",
            "--null",
            "--name-only",
            "--get-regexp",
            "^(filter|diff)\\.",
        ],
        env=sanitized_git_environment(),
        capture_output=True,
        check=False,
    )
    if proc.returncode not in {0, 1}:
        _fail(f"cannot inventory local Git drivers: {os.fsdecode(proc.stderr).strip()}")
    names = {os.fsdecode(item) for item in proc.stdout.split(b"\0") if item}
    unexpected_filters = sorted(
        name for name in names if name.startswith("filter.") and name not in expected
    )
    executable_diffs = sorted(
        name
        for name in names
        if name == "diff.external" or name.endswith((".command", ".textconv"))
    )
    if unexpected_filters or executable_diffs:
        bad = ", ".join([*unexpected_filters, *executable_diffs])
        _fail(f"refusing untrusted local Git driver config: {bad}")
    actual = {key: _local_config_values(root, key) for key in expected}
    exact = {key: (value,) for key, value in expected.items()}
    if not (all(not values for values in actual.values()) or actual == exact):
        _fail("refusing drifted partial filter.lfs local configuration")


def _shell_contract(
    source: Mapping[str, str], *, network: bool = False
) -> tuple[tuple[str, str, str], ...]:
    """Describe the strict child environment as shell-safe structured rows."""
    clean = _network_git_environment(source) if network else sanitized_git_environment(source)
    rows = [("unset", name, "") for name in sorted(source) if name not in clean]
    governed = {name for name in clean if name.startswith("GIT_") or name in {"PAGER", "TERM"}}
    rows.extend(("set", name, clean[name]) for name in sorted(governed))
    return tuple(rows)


def _commit_attribute_sources(root: Path, commit: str) -> list[tuple[str, list[str]]]:
    """Return every .gitattributes token set from one exact commit tree."""
    resolved = os.fsdecode(_git(root, "rev-parse", "--verify", f"{commit}^{{commit}}")).strip()
    raw_paths = _git(root, "ls-tree", "-r", "--name-only", "-z", resolved)
    sources: list[tuple[str, list[str]]] = []
    for raw_path in raw_paths.split(b"\0"):
        if not raw_path:
            continue
        relative = os.fsdecode(raw_path)
        if Path(relative).name != ".gitattributes":
            continue
        source = f"{resolved}:{relative}"
        try:
            text = _git(root, "show", source).decode("utf-8")
        except UnicodeError as exc:
            message = f"refusing non-UTF-8 Git attribute blob: {source}"
            raise GitEnvironmentError(message) from exc
        sources.append((source, _attribute_text_tokens(text)))
    return sources


def _worktree_attribute_sources(root: Path) -> list[tuple[str, list[str]]]:
    """Return every live worktree .gitattributes token set."""
    sources: list[tuple[str, list[str]]] = []
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [name for name in names if name != ".git"]
        if ".gitattributes" in files:
            path = Path(directory) / ".gitattributes"
            sources.append((str(path), _attribute_tokens(path)))
    return sources


def reject_untrusted_executable_attributes(root: Path, commit: str | None = None) -> None:
    """Refuse novel filter/diff attributes before a nested checkout runs.

    The repository's exact built-in C/C++ diff drivers and locally configured
    Git-LFS boundary are intentional. Any other driver name, or drift in those
    local definitions, is executable policy and fails closed.
    """
    root = root.resolve()
    common = Path(os.fsdecode(_git(root, "rev-parse", "--git-common-dir")).strip())
    git_dir = Path(os.fsdecode(_git(root, "rev-parse", "--git-dir")).strip())
    common = common if common.is_absolute() else (root / common).resolve()
    git_dir = git_dir if git_dir.is_absolute() else (root / git_dir).resolve()
    sources = (
        _commit_attribute_sources(root, commit)
        if commit is not None
        else _worktree_attribute_sources(root)
    )
    info_paths = dict.fromkeys([common / "info/attributes", git_dir / "info/attributes"])
    sources.extend((str(path), _attribute_tokens(path)) for path in info_paths)
    for source, tokens in sources:
        for token in tokens:
            name = token.partition("=")[0]
            if name in {"diff", "filter"} and not _trusted_attribute(token):
                _fail(f"refusing untrusted Git attribute {token!r} from {source}")
    _validate_local_driver_config(root)


def _tree_digest(root: Path) -> str:
    """Hash worktree paths, modes, link targets, and regular-file bytes."""
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: os.fsencode(str(item))):
        if ".git" in path.relative_to(root).parts:
            continue
        rel = os.fsencode(path.relative_to(root).as_posix())
        mode = stat.S_IMODE(path.lstat().st_mode)
        digest.update(rel + b"\0" + str(mode).encode("ascii") + b"\0")
        if path.is_symlink():
            digest.update(b"L" + os.fsencode(path.readlink()))
        elif path.is_file():
            digest.update(b"F" + path.read_bytes())
        else:
            digest.update(b"D")
    return digest.hexdigest()


def _outer_snapshot(root: Path) -> tuple[bytes, bytes, bytes, bytes, str, str]:
    """Capture the repository state a nested fixture must not mutate."""
    return (
        _git(root, "rev-parse", "HEAD"),
        (root / ".git" / "index").read_bytes(),
        (root / ".git" / "config").read_bytes(),
        _git(root, "status", "--porcelain=v1", "-z"),
        _tree_digest(root),
        _tree_digest(root / ".git" / "objects"),
    )


def _init_outer(root: Path) -> tuple[bytes, bytes, bytes, bytes, str, str]:
    """Create and snapshot a synthetic repository representing a hook caller."""
    root.mkdir()
    _git(root, "init", "--quiet")
    _git(root, "config", "user.email", "selftest@invalid")
    _git(root, "config", "user.name", "selftest")
    (root / "sentinel.txt").write_text("outer sentinel\n", encoding="ascii")
    _git(root, "add", "sentinel.txt")
    _git(root, "commit", "--quiet", "-m", "outer sentinel")
    return _outer_snapshot(root)


def _exercise_nested_repo(outer: Path, inner: Path) -> None:
    """Prove the hostile environment routes unsanitized Git to ``outer``."""
    os.environ["GIT_DIR"] = str(outer / ".git")
    os.environ["GIT_WORK_TREE"] = str(outer)
    os.environ["GIT_INDEX_FILE"] = str(outer / ".git" / "index")
    resolved = os.fsdecode(_git(inner, "rev-parse", "--show-toplevel", clean=False)).strip()
    if Path(resolved).resolve() != outer.resolve():
        _fail("hostile Git environment did not reproduce outer routing")
    with isolated_git_environment():
        _git(inner, "init", "--quiet", clean=False)
        _git(inner, "config", "user.email", "selftest@invalid", clean=False)
        _git(inner, "config", "user.name", "selftest", clean=False)
        (inner / "fixture.txt").write_text("inner fixture\n", encoding="ascii")
        _git(inner, "add", "fixture.txt", clean=False)
        _git(inner, "commit", "--quiet", "-m", "inner fixture", clean=False)


def _hostile_config_environment(root: Path) -> tuple[dict[str, str], Path]:
    """Create global/system config whose attributes execute a byte-preserving filter."""
    root.mkdir()
    marker = root / "filter-executed"
    helper = root / "filter-helper.sh"
    helper.write_text(
        f"#!/bin/sh\nprintf x >> {shlex.quote(str(marker))}\ncat\n",
        encoding="ascii",
    )
    helper.chmod(0o755)
    attributes = root / "global-attributes"
    attributes.write_text("* filter=ra8-hostile\n", encoding="ascii")
    system_config = root / "system.config"
    system_config.write_text(
        f"[core]\n\tattributesFile = {attributes}\n",
        encoding="ascii",
    )
    global_config = root / "global.config"
    global_config.write_text(
        f'[filter "ra8-hostile"]\n\tclean = {helper}\n\tsmudge = {helper}\n\trequired = true\n',
        encoding="ascii",
    )
    return (
        {
            "GIT_ATTR_NOSYSTEM": "0",
            "GIT_CONFIG_GLOBAL": str(global_config),
            "GIT_CONFIG_NOSYSTEM": "0",
            "GIT_CONFIG_SYSTEM": str(system_config),
        },
        marker,
    )


def _hostile_process_environment(root: Path) -> tuple[dict[str, str], tuple[Path, ...]]:
    """Build PATH, shell-startup, exported-function, and Python-startup attacks."""
    root.mkdir()
    git_marker = root / "git-authority-executed"
    bash_marker = root / "bash-startup-executed"
    python_marker = root / "python-startup-executed"
    fake_bin = root / "source/.venv/bin"
    fake_bin.mkdir(parents=True)
    fake_git = fake_bin / "git"
    fake_git.write_text(
        f'#!/bin/sh\nprintf x >> {shlex.quote(str(git_marker))}\nexec /usr/bin/git "$@"\n',
        encoding="ascii",
    )
    fake_git.chmod(0o755)
    bash_env = root / "bash-env"
    bash_env.write_text(f"printf x >> {shlex.quote(str(bash_marker))}\n", encoding="ascii")
    python_path = root / "python-path"
    python_path.mkdir()
    home = root / "home"
    home.mkdir()
    (python_path / "sitecustomize.py").write_text(
        "from pathlib import Path\n"
        f"Path({str(python_marker)!r}).write_text('x', encoding='ascii')\n",
        encoding="ascii",
    )
    function = f'() {{ printf x >> {shlex.quote(str(git_marker))}; /usr/bin/git "$@"; }}'
    environment = {
        "BASH_ENV": str(bash_env),
        "BASH_FUNC_git%%": function,
        "ENV": str(bash_env),
        "HOME": str(home),
        "PATH": f"{fake_bin}:{os.environ.get('PATH', '')}",
        "PYTHONPATH": str(python_path),
        "RA8_NON_GIT_SENTINEL": "preserved",
    }
    return environment, (git_marker, bash_marker, python_marker)


def _prove_hostile_process_attacks_execute(
    hostile: Mapping[str, str], markers: Sequence[Path]
) -> None:
    """Prove every interpreter/executable attack is live before strict boundaries."""
    environment = os.environ.copy()
    for name in ("SSH_CLIENT", "SSH_CONNECTION", "SSH_TTY"):
        environment.pop(name, None)
    environment.update(hostile)
    subprocess.run(
        ["/bin/bash", "-c", "git --version >/dev/null"],
        env=environment,
        check=True,
    )
    subprocess.run(
        ["/usr/bin/python3", "-c", "pass"],
        env=environment,
        check=True,
    )
    missing = [str(path) for path in markers if not path.exists()]
    if missing:
        _fail(f"hostile executable/interpreter probes did not fire: {missing}")
    for path in markers:
        path.unlink()


def _prove_hostile_config_executes(root: Path, hostile: Mapping[str, str], marker: Path) -> None:
    """Prove the hostile configuration is live before repaired code suppresses it."""
    probe = root / "config-probe"
    probe.mkdir()
    _git(probe, "init", "--quiet")
    (probe / "probe.txt").write_text("probe\n", encoding="ascii")
    environment = sanitized_git_environment()
    for name in tuple(environment):
        if name == "GIT_CONFIG_COUNT" or name.startswith(("GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_")):
            environment.pop(name)
    environment.update(hostile)
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and selftest argv
        [trusted_git_executable(), "-C", str(probe), "add", "probe.txt"],
        env=environment,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0 or not marker.is_file():
        detail = os.fsdecode(proc.stderr).strip()
        _fail(f"hostile config/attribute probe did not execute its filter: {detail}")
    marker.unlink()


def _prove_inherited_global_config_remains_available(root: Path) -> None:
    """Prove direct real-tree callers may still inherit harmless global policy."""
    config = root / "harmless-global.config"
    config.write_text("[ra8]\n\tharmless = visible\n", encoding="ascii")
    direct = os.environ.copy()
    direct.update(
        {
            "GIT_CONFIG_GLOBAL": str(config),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": os.devnull,
        }
    )
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and fixed config query
        [trusted_git_executable(), "config", "--global", "--get", "ra8.harmless"],
        env=direct,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0 or proc.stdout != b"visible\n":
        _fail("direct inherited Git environment lost harmless global config")
    direct["RA8_NON_GIT_SENTINEL"] = "preserved"
    clean = sanitized_git_environment(direct)
    if clean.get("RA8_NON_GIT_SENTINEL") != "preserved":
        _fail("sanitizer removed an unrelated non-Git environment variable")
    if clean.get("GIT_CONFIG_GLOBAL") != os.devnull or clean.get("GIT_CONFIG_SYSTEM") != os.devnull:
        _fail("sanitizer did not bind global/system Git configuration to safe files")


def _python_selftest(
    repo_root: Path, label: str, relative: str, *args: str
) -> tuple[str, tuple[str, ...]]:
    """Build one isolated, absolute-interpreter selftest command."""
    return label, ("/usr/bin/python3", "-I", str(repo_root / relative), *args)


def _registered_fixture_commands(repo_root: Path) -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Return the exact selftest suites protected by the nested-Git boundary."""
    return (
        _python_selftest(
            repo_root,
            "init-order",
            "scripts/checks/check_init_order_freshness.py",
            "--selftest",
        ),
        _python_selftest(
            repo_root,
            "roadmap-dashboard",
            "scripts/checks/check_roadmap_dashboard_freshness.py",
            "--selftest",
        ),
        _python_selftest(
            repo_root,
            "markdown-references",
            "scripts/checks/check_markdown_references.py",
            "--selftest",
        ),
        _python_selftest(
            repo_root,
            "python-lock-policy",
            "scripts/checks/check_python_lock_policy.py",
            "--selftest",
        ),
        _python_selftest(repo_root, "work-harness", "tools/work/src/work.py", "--selftest"),
        _python_selftest(
            repo_root,
            "workspace-lifecycle",
            "tools/work/tests/test_workspace_lifecycle.py",
        ),
        _python_selftest(
            repo_root,
            "pre-commit-bootstrap",
            "scripts/checks/check_hook_parity.py",
            "--selftest",
        ),
        _python_selftest(
            repo_root, "candidate-assembly", "scripts/dev/assemble_candidate.py", "--selftest"
        ),
    )


def _run_registered_fixture(
    outer: Path,
    hostile: Mapping[str, str],
    markers: Sequence[Path],
    label: str,
    argv: tuple[str, ...],
) -> None:
    """Run one suite under hostile routing/config and prove no outer mutation."""
    repo_root = Path(__file__).resolve().parents[2]
    before = _outer_snapshot(outer)
    environment = os.environ.copy()
    for name in ("SSH_CLIENT", "SSH_CONNECTION", "SSH_TTY"):
        environment.pop(name, None)
    environment.update(
        {
            "GIT_INDEX_FILE": str(outer / ".git" / "index"),
            "GIT_OBJECT_DIRECTORY": str(outer / ".git" / "objects"),
            "PYTHONDONTWRITEBYTECODE": "1",
            **hostile,
        }
    )
    proc = subprocess.run(  # noqa: S603 -- fixed interpreter and audited selftest paths
        argv,
        cwd=repo_root,
        env=environment,
        capture_output=True,
        check=False,
        timeout=180,
    )
    if proc.returncode != 0:
        detail = os.fsdecode(proc.stderr or proc.stdout).strip()
        _fail(f"{label} failed under hostile Git routing: {detail}")
    if before != _outer_snapshot(outer):
        _fail(f"{label} mutated hostile outer Git state")
    fired = [str(path) for path in markers if path.exists()]
    if fired:
        _fail(f"{label} executed hostile inherited policy: {fired}")


def _exercise_registered_fixture_selftests(outer: Path) -> None:
    """Run every repaired fixture suite under routing and config/filter attacks."""
    repo_root = Path(__file__).resolve().parents[2]
    git_hostile, git_marker = _hostile_config_environment(outer.parent / "config-attack")
    process_hostile, process_markers = _hostile_process_environment(outer.parent / "process-attack")
    hostile = {**git_hostile, **process_hostile}
    markers = (git_marker, *process_markers)
    _prove_hostile_config_executes(outer.parent, git_hostile, git_marker)
    _prove_hostile_process_attacks_execute(process_hostile, process_markers)
    for label, argv in _registered_fixture_commands(repo_root):
        _run_registered_fixture(outer, hostile, markers, label, argv)


def _prepare_snapshot_source(base: Path) -> tuple[Path, tuple[Path, Path, Path]]:
    """Create a committed EOL/filter tree with executable local Git policy."""
    source = base / "snapshot-source"
    source.mkdir()
    _git(source, "init", "--quiet")
    local_marker = base / "source-local-filter"
    fsmonitor_marker = base / "source-local-fsmonitor"
    template_marker = base / "source-local-template-executed"
    local_helper = base / "source-local-helper.sh"
    local_helper.write_text(
        f"#!/bin/sh\nprintf x >> {shlex.quote(str(local_marker))}\ncat\n",
        encoding="ascii",
    )
    local_helper.chmod(0o755)
    fsmonitor = base / "source-local-fsmonitor.sh"
    fsmonitor.write_text(
        f"#!/bin/sh\nprintf x >> {shlex.quote(str(fsmonitor_marker))}\nprintf '\\n'\n",
        encoding="ascii",
    )
    fsmonitor.chmod(0o755)
    template = base / "source-local-template-dir"
    (template / "hooks").mkdir(parents=True)
    template_hook = template / "hooks/reference-transaction"
    template_hook.write_text(
        f"#!/bin/sh\nprintf x >> {shlex.quote(str(template_marker))}\n",
        encoding="ascii",
    )
    template_hook.chmod(0o755)
    (source / ".gitattributes").write_text(
        "sentinel.txt text eol=crlf\nevil.bin filter=evil\n", encoding="ascii"
    )
    (source / "sentinel.txt").write_text("snapshot sentinel\n", encoding="ascii")
    (source / "evil.bin").write_bytes(b"raw fixture\n")
    _git(source, "add", ".gitattributes", "sentinel.txt", "evil.bin")
    _git(
        source,
        "-c",
        "user.email=selftest@invalid",
        "-c",
        "user.name=selftest",
        "commit",
        "--quiet",
        "-m",
        "snapshot",
    )
    _git(source, "config", "--local", "filter.evil.clean", str(local_helper))
    _git(source, "config", "--local", "filter.evil.smudge", str(local_helper))
    _git(source, "config", "--local", "filter.evil.required", "true")
    _git(source, "config", "--local", "core.fsmonitor", str(fsmonitor))
    _git(source, "config", "--local", "init.templateDir", str(template))
    return source, (local_marker, fsmonitor_marker, template_marker)


def _exercise_shell_snapshot_boundary(outer: Path) -> None:
    """Prove fresh checkout keeps EOL semantics without source Git helpers."""
    repo_root = Path(__file__).resolve().parents[2]
    hostile, marker = _hostile_config_environment(outer.parent / "shell-config-attack")
    _prove_hostile_config_executes(outer.parent / "shell-config-attack", hostile, marker)
    source, local_markers = _prepare_snapshot_source(outer.parent)
    output = outer.parent / "snapshot-output"
    output.mkdir()
    before = _outer_snapshot(outer)
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_INDEX_FILE": str(outer / ".git" / "index"),
            "GIT_OBJECT_DIRECTORY": str(outer / ".git" / "objects"),
            **hostile,
        }
    )
    script = """
set -euo pipefail
source "$1/scripts/dev/git_environment.sh"
source "$1/scripts/ci/lib/snapshot.sh"
REPO_ROOT="$2"
materialise_head_snapshot "$3"
git -C "$3" ls-files --error-unmatch sentinel.txt >/dev/null
"""
    proc = subprocess.run(  # noqa: S603 -- fixed Bash executable and audited fixture argv
        [
            "/bin/bash",
            "-p",
            "-c",
            script,
            "shell-snapshot",
            str(repo_root),
            str(source),
            str(output),
        ],
        env=environment,
        capture_output=True,
        check=False,
        timeout=60,
    )
    if proc.returncode != 0:
        detail = os.fsdecode(proc.stderr or proc.stdout).strip()
        _fail(f"shell snapshot boundary failed under hostile Git policy: {detail}")
    if before != _outer_snapshot(outer):
        _fail("shell snapshot boundary mutated hostile outer Git state")
    if any(path.exists() for path in (marker, *local_markers)):
        _fail("shell snapshot boundary executed inherited or source-local Git policy")
    if (output / "sentinel.txt").read_bytes() != b"snapshot sentinel\r\n":
        _fail("strict snapshot lost committed CRLF checkout semantics")
    if (output / "evil.bin").read_bytes() != b"raw fixture\n":
        _fail("strict snapshot changed an unconfigured filtered blob")


def _exercise_shell_push_transport(root: Path) -> None:
    """Prove bounded push keeps SSH policy but cannot inherit repo routing."""
    adapter = Path(__file__).with_suffix(".sh")
    capture = root / "push-environment"
    capture_helper = root / "capture-push-environment"
    capture_helper.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n%s\\n%s\\n%s\\n' \"${GIT_SSH_COMMAND-unset}\" "
        '"${GIT_DIR-unset}" "${GIT_CONFIG_COUNT-unset}" "$*" >"$RA8_PUSH_CAPTURE"\n',
        encoding="ascii",
    )
    capture_helper.chmod(0o755)
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_DIR": str(root / "hostile.git"),
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "core.hooksPath",
            "GIT_CONFIG_VALUE_0": str(root / "hostile-hooks"),
            "GIT_SSH_COMMAND": "ssh -F operator-config",
            "RA8_PUSH_CAPTURE": str(capture),
            "RA8_PUSH_HELPER": str(capture_helper),
        }
    )
    script = (
        'source "$1"\n'
        "run_git_network_with_inherited_transport -c "
        "'alias.ra8-capture=! \"$RA8_PUSH_HELPER\"' "
        "ra8-capture push origin gh-pages\n"
    )
    proc = subprocess.run(  # noqa: S603 -- fixed Bash and controlled fixture argv
        ["/bin/bash", "-p", "-c", script, "shell-push", str(adapter)],
        env=environment,
        capture_output=True,
        check=False,
        timeout=30,
    )
    if proc.returncode != 0:
        _fail(f"shell push transport boundary failed: {os.fsdecode(proc.stderr).strip()}")
    lines = capture.read_text(encoding="ascii").splitlines()
    expected_tail = ["unset", "push origin gh-pages"]
    hostile_dir = str(root / "hostile.git")
    if (
        len(lines) != PUSH_CAPTURE_FIELDS
        or lines[0] != "ssh -F operator-config"
        or lines[1] == hostile_dir
        or lines[2:] != expected_tail
    ):
        _fail(f"shell push transport selected the wrong environment/argv: {lines!r}")


def _exercise_shell_command_authority(root: Path) -> None:
    """Prove aliases, functions, hashes, PATH, and source tools cannot select Git."""
    adapter = Path(__file__).with_suffix(".sh")
    script = r"""
set -euo pipefail
mode="$1"
helper="$2"
marker="$3"
adapter="$4"
case "$mode" in
  alias)
    shopt -s expand_aliases
    alias git="'$helper'"
    eval 'git control'
    ;;
  function)
    git() { "$helper" "$@"; }
    git control
    ;;
  hash)
    hash -p "$helper" git
    git control
    ;;
  path | source-venv)
    PATH="$(dirname "$helper"):$PATH"
    git control
    ;;
  *) exit 64 ;;
esac
[[ -s "$marker" ]]
rm -f -- "$marker"
source "$adapter"
run_sanitized_git --version >/dev/null
[[ ! -e "$marker" ]]
"""
    for mode in ("alias", "function", "hash", "path", "source-venv"):
        directory = root / ("source/.venv/bin" if mode == "source-venv" else f"{mode}-bin")
        directory.mkdir(parents=True)
        marker = root / f"{mode}.fired"
        helper = directory / "git"
        helper.write_text(f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\n", encoding="ascii")
        helper.chmod(0o755)
        proc = subprocess.run(  # noqa: S603 -- fixed Bash and private authority fixture
            [
                "/bin/bash",
                "-p",
                "-c",
                script,
                "authority",
                mode,
                str(helper),
                str(marker),
                str(adapter),
            ],
            capture_output=True,
            check=False,
            timeout=30,
        )
        if proc.returncode != 0 or marker.exists():
            detail = os.fsdecode(proc.stderr or proc.stdout).strip()
            _fail(f"shell Git authority failed for {mode}: {detail}")


def _prove_hostile_index_and_object_routing(outer: Path, inner: Path) -> None:
    """Prove the external index/object environment would mutate without isolation."""
    _init_outer(outer)
    inner.mkdir()
    _git(inner, "init", "--quiet")
    (inner / "probe.txt").write_text("hostile routing probe\n", encoding="ascii")
    index_before = (outer / ".git" / "index").read_bytes()
    objects_before = _tree_digest(outer / ".git" / "objects")
    with isolated_git_environment():
        os.environ["GIT_INDEX_FILE"] = str(outer / ".git" / "index")
        os.environ["GIT_OBJECT_DIRECTORY"] = str(outer / ".git" / "objects")
        _git(inner, "add", "probe.txt", clean=False)
    if (outer / ".git" / "index").read_bytes() == index_before:
        _fail("hostile GIT_INDEX_FILE probe did not redirect the fixture index")
    if _tree_digest(outer / ".git" / "objects") == objects_before:
        _fail("hostile GIT_OBJECT_DIRECTORY probe did not redirect fixture objects")


def run_selftest() -> int:
    """Verify nested fixture activity cannot alter a hostile outer repository."""
    original = dict(os.environ)
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-git-environment-") as temp:
            base = Path(temp)
            outer = base / "outer"
            inner = base / "inner"
            before = _init_outer(outer)
            inner.mkdir()
            _exercise_nested_repo(outer, inner)
            after = _outer_snapshot(outer)
            if before != after:
                _fail("nested fixture mutated outer Git or worktree state")
            if os.fsdecode(_git(inner, "status", "--porcelain=v1")).strip():
                _fail("nested fixture repository is not clean")
            _exercise_registered_fixture_selftests(outer)
            _exercise_shell_snapshot_boundary(outer)
            _exercise_shell_push_transport(base)
            _exercise_shell_command_authority(base)
            _prove_hostile_index_and_object_routing(base / "probe-outer", base / "probe-inner")
            _prove_inherited_global_config_remains_available(base)
    except (GitEnvironmentError, OSError) as exc:
        print(f"SELFTEST FAIL: {exc}")
        return 1
    finally:
        os.environ.clear()
        os.environ.update(original)
    print(
        "selftest: hostile routing/config/filter environment, shell snapshot/push, and "
        "8 registered fixture suites stay isolated: OK"
    )
    return 0


def main() -> int:
    """Print the shared variable list or run its mutation regression."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--names", action="store_true")
    group.add_argument("--shell-contract", action="store_true")
    group.add_argument("--network-shell-contract", action="store_true")
    group.add_argument("--selftest", action="store_true")
    group.add_argument("--check-attributes", metavar="ROOT")
    parser.add_argument("--commit")
    args = parser.parse_args()
    if args.commit is not None and args.check_attributes is None:
        parser.error("--commit requires --check-attributes")
    if args.names:
        print("\n".join(LOCAL_GIT_ENVIRONMENT))
        return 0
    if args.shell_contract or args.network_shell_contract:
        for action, name, value in _shell_contract(os.environ, network=args.network_shell_contract):
            print(f"{action}\t{name}\t{value}")
        return 0
    if args.selftest:
        return run_selftest()
    try:
        reject_untrusted_executable_attributes(Path(args.check_attributes), args.commit)
    except (GitEnvironmentError, OSError) as exc:
        print(f"Git attribute policy: FAIL: {exc}", file=sys.stderr)
        return 1
    print("Git attribute policy: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

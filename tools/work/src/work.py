#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Repository workflow client for plans and canonical agent workspaces.

The only creating path delegates to ``scripts/dev/agent_workspace.sh``. This
module owns no worktree lock, metadata store, branch creator, cleanup path, or
gate implementation. It reads the shared monitor's cached verdict without
polling. GitHub mutations are emitted for human review and never executed here.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import shutil
import stat
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/dev"))

from work_emit import render_commands
from work_gh import STATE_DEGRADED, STATE_UNAVAILABLE, Probe, probe_auth, probe_version
from work_git import (
    GitCommandError,
    RepoPaths,
    ToolMissingError,
    WorkError,
    branch_exists,
    diff_stat,
    discover_repo,
    git_child_environment,
    git_executable,
    porcelain_status,
    printable,
    redact,
    reject_executable_attributes,
    resolve_commit,
    resolve_tree,
    run_process,
)
from work_plan import PlanError, load_plan, plan_to_json, render_summary
from work_workspace import (
    FOREIGN,
    FORGED,
    OWNER,
    READY,
    STALE,
    Claim,
    ClaimError,
    branch_name,
    classify,
    is_identifier,
    list_claims,
    load_claim,
    metadata_dir,
    metadata_path,
    recovery_command,
    workspace_name,
)

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2
MIN_PYTHON = (3, 11)
CHECK_OK = "OK"
CHECK_FAIL = "FAIL"
WS_ROOT_ENV = "RA8_WS_ROOT"
SELFTEST_MINIMUM = 101


@dataclass(frozen=True)
class Check:
    """One doctor result."""

    name: str
    state: str
    detail: str


@dataclass(frozen=True)
class StartPlan:
    """One dry-run description passed to the canonical workspace creator."""

    identifier: str
    name: str
    branch: str
    target: Path
    ws_root: Path
    ref: str
    base_commit: str | None
    refusals: tuple[str, ...]


def _print(text: str) -> None:
    """Print one sanitized logical line."""
    print(printable(text))


def _fail(text: str) -> None:
    """Print one sanitized logical error line."""
    print(printable(text), file=sys.stderr)


def _notice(text: str) -> None:
    """Print one sanitized non-error diagnostic without polluting stdout."""
    print(printable(text), file=sys.stderr)


def _print_block(text: str, *, error: bool = False) -> None:
    """Print captured multiline output one sanitized line at a time."""
    writer = _fail if error else _print
    for line in text.splitlines():
        writer(line)


def _ws_root(value: str | None = None) -> Path:
    """Return the absolute lexical canonical workspace root."""
    selected = value or os.environ.get(WS_ROOT_ENV) or str(Path.home() / "ra8-ws")
    if not selected.isascii() or not selected.isprintable():
        message = "workspace root must contain only printable single-line ASCII"
        raise WorkError(message)
    # ``resolve`` would follow a hostile symlink before the claim validator can reject it.
    return Path(
        os.path.abspath(  # noqa: PTH100 -- preserve symlink evidence for claim validation
            os.fspath(Path(selected).expanduser())
        )
    )


def _workspace_script(paths: RepoPaths) -> Path:
    """Return the canonical workspace lifecycle implementation."""
    return paths.toplevel / "scripts/dev/agent_workspace.sh"


def _writability(path: Path) -> tuple[bool, Path]:
    """Return whether the nearest existing ancestor is writable."""
    probe = path
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    return os.access(probe, os.W_OK), probe


def _probe_to_check(probe: Probe) -> Check:
    """Adapt one GitHub probe."""
    return Check(probe.name, probe.state, probe.detail)


def _doctor_checks(paths: RepoPaths, cwd: Path) -> list[Check]:
    """Build read-only readiness checks, including emitted-script dependencies."""
    version = ".".join(str(part) for part in sys.version_info[:3])
    python_state = CHECK_OK if sys.version_info[:2] >= MIN_PYTHON else CHECK_FAIL
    git_version = run_process([git_executable(), "--version"], cwd=cwd).stdout.strip()
    root = _ws_root()
    writable, ancestor = _writability(root)
    script = _workspace_script(paths)
    jq = shutil.which("jq")
    jq_state = CHECK_OK if jq is not None else CHECK_FAIL
    jq_detail = run_process([jq, "--version"], cwd=cwd).stdout.strip() if jq else "missing"
    kind = "linked worktree" if paths.is_linked_worktree else "main worktree"
    return [
        Check("python", python_state, f"{version} (minimum {MIN_PYTHON[0]}.{MIN_PYTHON[1]})"),
        Check("git", CHECK_OK, git_version),
        Check("jq", jq_state, jq_detail),
        Check("repository", CHECK_OK, f"{paths.toplevel} ({kind})"),
        Check(
            "canonical workspace tool",
            CHECK_OK if script.is_file() else CHECK_FAIL,
            str(script),
        ),
        Check(
            "workspace root",
            CHECK_OK if writable else CHECK_FAIL,
            f"{root} (nearest ancestor {ancestor} is {'writable' if writable else 'NOT writable'})",
        ),
        _probe_to_check(probe_version()),
        _probe_to_check(probe_auth()),
    ]


def cmd_doctor(_options: argparse.Namespace) -> int:
    """Report local readiness; ``gh auth status`` may perform a read-only API probe."""
    cwd = Path.cwd()
    paths = discover_repo(cwd)
    checks = _doctor_checks(paths, cwd)
    width = max(len(check.name) for check in checks)
    for check in checks:
        _print(f"  {check.state:<12} {check.name:<{width}}  {check.detail}")
    for check in checks:
        if check.state in (STATE_DEGRADED, STATE_UNAVAILABLE):
            _print(f"notice: {check.name} is {check.state.lower()} -- {check.detail}")
    failed = [check for check in checks if check.state == CHECK_FAIL]
    if failed:
        _fail(f"work doctor: {len(failed)} check(s) FAILED")
        return EXIT_FAIL
    return EXIT_OK


def _reject_nonregular_destination(path: Path) -> None:
    """Reject an existing destination unless ``lstat`` proves it is regular."""
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return
    if not stat.S_ISREG(mode):
        message = f"refusing to replace destination that is not a regular file: {path}"
        raise WorkError(message)


def _write_text_atomic(path: Path, text: str) -> None:
    """Write derived output atomically without opening a nonregular destination."""
    _reject_nonregular_destination(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="ascii") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        _reject_nonregular_destination(path)
        Path(temporary).replace(path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            Path(temporary).unlink()


def _plan_output_problem(options: argparse.Namespace) -> str:
    """Return why the requested stdout artifacts are ambiguous, if they are."""
    stdout_modes = sum(
        (
            options.json == "-",
            options.emit_commands,
            options.summary,
        )
    )
    if stdout_modes > 1:
        return "--json -, --emit-commands, and --summary are mutually exclusive stdout modes"
    return ""


def cmd_plan(options: argparse.Namespace) -> int:
    """Validate notes and emit deterministic artifacts."""
    if problem := _plan_output_problem(options):
        _fail(f"work plan: {problem}")
        return EXIT_CONFIG
    notes = Path(options.notes).expanduser()
    try:
        plan = load_plan(notes)
    except PlanError as exc:
        for problem in exc.problems:
            _fail(f"{notes}: {problem}")
        _fail(f"{notes}: {len(exc.problems)} problem(s); nothing was emitted")
        return EXIT_FAIL
    emitted = False
    if options.json is not None:
        text = plan_to_json(plan)
        if options.json == "-":
            sys.stdout.write(text)
        else:
            _write_text_atomic(Path(options.json), text)
            _notice(f"wrote {options.json}")
        emitted = True
    if options.emit_commands:
        sys.stdout.write(render_commands(plan))
        emitted = True
    if options.summary or not emitted:
        sys.stdout.write(render_summary(plan))
    return EXIT_OK


def _build_start_plan(paths: RepoPaths, cwd: Path, options: argparse.Namespace) -> StartPlan:
    """Build a read-only preview; canonical creation repeats checks under its lock."""
    root = _ws_root(options.ws_root)
    identifier = options.identifier
    name = workspace_name(identifier)
    branch = branch_name(identifier)
    target = root / name
    base = resolve_commit(options.ref, cwd=cwd)
    if base is not None:
        reject_executable_attributes(cwd, base)
    refusals: list[str] = []
    if base is None:
        refusals.append(f"start ref does not resolve locally: {options.ref}")
    if target.exists() or target.is_symlink():
        refusals.append(f"workspace path already exists: {target}")
    claim_path = metadata_path(root, identifier)
    if claim_path.exists() or claim_path.is_symlink():
        refusals.append(f"canonical workspace metadata already exists for {name}")
    if branch_exists(branch, cwd=cwd):
        refusals.append(f"branch already exists: {branch}")
    if target.parent != root:
        refusals.append("derived workspace is not a direct child of the canonical root")
    if not _workspace_script(paths).is_file():
        refusals.append("canonical workspace script is absent")
    return StartPlan(identifier, name, branch, target, root, options.ref, base, tuple(refusals))


def _describe_start(paths: RepoPaths, start: StartPlan) -> None:
    """Print one mutation-free canonical-workspace preview."""
    _print("work start: DRY RUN -- nothing has been created.")
    _print(f"  identifier   {start.identifier}")
    _print(f"  canonical    {start.name}")
    _print(f"  branch       {start.branch}")
    _print(f"  worktree     {start.target}")
    _print(f"  start ref    {start.ref} -> {start.base_commit or 'unresolved'}")
    _print(f"  metadata     {metadata_path(start.ws_root, start.identifier)}")
    _print(f"  creator      {_workspace_script(paths)}")
    _print("Re-run with --execute to ask the canonical workspace lifecycle to create it.")


def _execute_start(paths: RepoPaths, start: StartPlan) -> int:
    """Delegate creation to the sole workspace lock/state authority."""
    if start.base_commit is None:
        return EXIT_FAIL
    argv = [
        "/bin/bash",
        "-p",
        str(_workspace_script(paths)),
        "create",
        start.name,
        start.base_commit,
        "--branch",
        start.branch,
        "--owner",
        OWNER,
    ]
    environment = git_child_environment()
    environment[WS_ROOT_ENV] = str(start.ws_root)
    environment["RA8_WS_UPSTREAM"] = str(paths.toplevel)
    done = run_process(argv, cwd=paths.toplevel, timeout=300, env=environment)
    _print_block(done.stdout)
    if not done.ok:
        _print_block(done.stderr, error=True)
        return EXIT_FAIL
    return EXIT_OK


def cmd_start(options: argparse.Namespace) -> int:
    """Preview or canonically create one identifier-owned workspace."""
    if not is_identifier(options.identifier):
        _fail(f"work start: refusing invalid identifier {options.identifier!r}")
        return EXIT_CONFIG
    cwd = Path.cwd()
    paths = discover_repo(cwd)
    start = _build_start_plan(paths, cwd, options)
    _describe_start(paths, start)
    if start.refusals:
        for refusal in start.refusals:
            _fail(f"REFUSE: {refusal}")
        return EXIT_FAIL
    return _execute_start(paths, start) if options.execute else EXIT_OK


def _claim_for(identifier: str, paths: RepoPaths, root: Path) -> Claim:
    """Load one canonical claim and require exact branch-to-worktree binding."""
    claim = load_claim(metadata_path(root, identifier), root)
    verdict = classify(claim, paths.toplevel)
    if verdict != READY:
        message = f"canonical claim is {verdict}"
        raise ClaimError(message)
    return claim


def cmd_status(options: argparse.Namespace) -> int:
    """Report only work-owned records from canonical workspace metadata."""
    paths = discover_repo(Path.cwd())
    root = _ws_root(options.ws_root)
    records = list_claims(root)
    if not records:
        _print(f"work status: no work-owned canonical metadata under {metadata_dir(root)}")
        return EXIT_OK
    forged = False
    for path, value in records:
        if isinstance(value, ClaimError):
            _print(f"  {FORGED:<9} {path.name:<24} {value}")
            forged = True
            continue
        verdict = classify(value, paths.toplevel)
        _print(f"  {verdict:<9} {value.identifier:<24} {value.branch:<28} {value.worktree}")
        forged = forged or verdict == FORGED
        if verdict in (STALE, FOREIGN):
            command = recovery_command(value, paths.toplevel, stale=verdict == STALE)
            _print(f"            after human review: {command}")
    return EXIT_FAIL if forged else EXIT_OK


def _remote_ci(paths: RepoPaths, workspace: Path, head: str) -> tuple[str, str]:
    """Read the shared monitor's cached verdict without polling GitHub."""
    monitor = paths.toplevel / "scripts/ci/monitor.sh"
    if not monitor.is_file():
        return "UNKNOWN", "shared CI monitor script is absent"
    result = run_process(
        ["/bin/bash", "-p", str(monitor), "status", "--sha", head],
        cwd=workspace,
        timeout=30,
    )
    state = {0: "PASS", 1: "FAIL", 3: "UNKNOWN"}.get(result.returncode, "UNKNOWN")
    detail = next(
        (line for line in result.stdout.splitlines() if line.strip()), "no monitor detail"
    )
    return state, detail


def _workspace_report(claim: Claim, phase: str) -> tuple[str | None, list[str]]:
    """Print shared workspace facts and return its HEAD plus dirty paths."""
    dirty = porcelain_status(claim.worktree)
    head = resolve_commit("HEAD", cwd=claim.worktree)
    if head is None:
        _fail(f"work {phase}: workspace HEAD does not resolve")
        return None, dirty
    _print(f"work {phase}: {claim.identifier}")
    _print(f"  branch      {claim.branch}")
    _print(f"  worktree    {claim.worktree}")
    _print(f"  base        {claim.base_ref} ({claim.base_commit})")
    _print(f"  claimed     {claim.created} by {claim.creator}")
    _print(f"  HEAD        {head}")
    _print(f"  working set {len(dirty)} changed path(s)")
    _print("  diffstat vs base:")
    for line in diff_stat(claim.worktree, claim.base_commit).splitlines() or ["(empty)"]:
        _print(f"    {line}")
    return head, dirty


def _clean_claim(options: argparse.Namespace, phase: str) -> tuple[RepoPaths, Claim] | None:
    """Load one exact claim and require a clean committed working tree."""
    if not is_identifier(options.identifier):
        _fail(f"work {phase}: refusing invalid identifier {options.identifier!r}")
        return None
    paths = discover_repo(Path.cwd())
    root = _ws_root(options.ws_root)
    try:
        claim = _claim_for(options.identifier, paths, root)
    except ClaimError as exc:
        _fail(f"work {phase}: {exc}")
        return None
    _head, dirty = _workspace_report(claim, phase)
    if dirty:
        _fail(f"work {phase}: REFUSE -- commit or intentionally discard the working set first")
        _fail("just ci validates committed HEAD, not these uncommitted bytes")
        return None
    return paths, claim


def cmd_ready(options: argparse.Namespace) -> int:
    """Run exact local CI for a clean committed work claim before the sole push."""
    loaded = _clean_claim(options, "ready")
    if loaded is None:
        return EXIT_FAIL
    _paths, claim = loaded
    if not options.run_ci:
        _fail("work ready: REFUSE -- pass --run-ci to record exact local gate evidence")
        return EXIT_FAIL
    just = shutil.which("just")
    if just is None:
        _fail("work ready: just is not installed")
        return EXIT_FAIL
    _print("  local CI    RUNNING: just ci")
    result = run_process([just, "ci"], cwd=claim.worktree, timeout=7200)
    _print_block(result.stdout)
    if not result.ok:
        _print_block(result.stderr, error=True)
        _fail(f"work ready: local CI did not PASS (exit {result.returncode})")
        return EXIT_FAIL
    head = resolve_commit("HEAD", cwd=claim.worktree)
    _print(f"  local CI    PASS: just ci at {head}")
    _print("work ready: review/squash this tree, then perform the one authorized push")
    return EXIT_OK


def cmd_landed(options: argparse.Namespace) -> int:
    """Require content-equivalent pushed dev and cached PASS after the push."""
    loaded = _clean_claim(options, "landed")
    if loaded is None:
        return EXIT_FAIL
    paths, claim = loaded
    claim_head = resolve_commit("HEAD", cwd=claim.worktree)
    dev_head = resolve_commit("origin/dev", cwd=claim.worktree)
    claim_tree = resolve_tree(claim_head, cwd=claim.worktree) if claim_head else None
    dev_tree = resolve_tree(dev_head, cwd=claim.worktree) if dev_head else None
    if dev_head is None or claim_tree is None or claim_tree != dev_tree:
        _fail("work landed: origin/dev is absent or not content-equivalent to this work claim")
        return EXIT_FAIL
    ci_state, ci_detail = _remote_ci(paths, claim.worktree, dev_head)
    _print(f"  remote CI   {ci_state}: {ci_detail}")
    if ci_state != "PASS":
        _fail("work landed: remote CI is not PASS; Landed would be false")
        return EXIT_FAIL
    _print(f"work landed: origin/dev {dev_head} is content-equivalent and remotely green")
    _print(
        "  now set the board card to Landed, close citing the squash SHA, "
        "then review branch cleanup"
    )
    return EXIT_OK


HANDLERS = {
    "doctor": cmd_doctor,
    "plan": cmd_plan,
    "start": cmd_start,
    "status": cmd_status,
    "ready": cmd_ready,
    "landed": cmd_landed,
}


def build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser."""
    parser = argparse.ArgumentParser(prog="work", description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="run the offline tests")
    sub = parser.add_subparsers(dest="command", metavar="<command>")
    sub.add_parser("doctor", help="report local and emitted-script readiness")
    plan = sub.add_parser("plan", help="validate notes and emit an ordered plan")
    plan.add_argument("notes")
    plan.add_argument("--json", metavar="PATH")
    stdout = plan.add_mutually_exclusive_group()
    stdout.add_argument("--emit-commands", action="store_true")
    stdout.add_argument("--summary", action="store_true")
    start = sub.add_parser("start", help="preview or canonically create one workspace")
    start.add_argument("identifier")
    start.add_argument("--ref", default="HEAD")
    start.add_argument("--execute", action="store_true")
    start.add_argument("--ws-root", metavar="DIR")
    status = sub.add_parser("status", help="list canonical work-owned workspaces")
    status.add_argument("--ws-root", metavar="DIR")
    ready = sub.add_parser("ready", help="run exact local CI before the sole push")
    ready.add_argument("identifier")
    ready.add_argument("--ws-root", metavar="DIR")
    ready.add_argument("--run-ci", action="store_true")
    landed = sub.add_parser("landed", help="require content-equivalent green origin/dev")
    landed.add_argument("identifier")
    landed.add_argument("--ws-root", metavar="DIR")
    return parser


def run_selftest() -> int:
    """Run tests and fail if discovery silently collapses below its floor."""
    tests = Path(__file__).resolve().parents[1] / "tests"
    suite = unittest.TestLoader().discover(str(tests), top_level_dir=str(tests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.testsRun < SELFTEST_MINIMUM:
        _fail(f"work selftest: discovered {result.testsRun} tests, below floor {SELFTEST_MINIMUM}")
        return EXIT_FAIL
    return EXIT_OK if result.wasSuccessful() else EXIT_FAIL


def main(argv: list[str]) -> int:
    """Dispatch one command with redacted failures."""
    parser = build_parser()
    options = parser.parse_args(argv)
    if options.selftest:
        return run_selftest()
    if options.command is None:
        parser.print_help()
        return EXIT_CONFIG
    try:
        return HANDLERS[options.command](options)
    except (ToolMissingError, GitCommandError, ClaimError, WorkError, OSError) as exc:
        _fail(f"work: {redact(str(exc))}")
        return EXIT_CONFIG


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

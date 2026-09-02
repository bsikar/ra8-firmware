#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Guard hook-to-Just parity and the immutable pre-commit owner transport."""

from __future__ import annotations

import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.checks import hook_parity_mutations as mutations
from scripts.checks import hook_transport_support as transport
from scripts.checks.hook_git_policy_selftest import run_hostile_owner_cases as _hostile
from scripts.checks.hook_runtime_selftest import (
    default_signal_test_command,
    run_runtime_selftests,
)
from scripts.dev.git_environment import sanitized_git_environment, trusted_git_executable

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_ROOT = Path(os.environ.get("RA8_HOOK_POLICY_ROOT", REPO_ROOT)).resolve()
HOOKS_JUST = POLICY_ROOT / "just" / "hooks.just"
PRE_COMMIT = POLICY_ROOT / "scripts" / "git" / "pre-commit"
PRE_PUSH = POLICY_ROOT / "scripts" / "git" / "pre-push"
HOOK_LAUNCHER_FILE = POLICY_ROOT / "scripts" / "git" / "hook-launcher"
HOOK_INSTALLER = POLICY_ROOT / "scripts" / "git" / "install-hooks.sh"
PROOF_WRITER = POLICY_ROOT / "scripts" / "git" / "write-proof.py"
CI_SCRIPT = POLICY_ROOT / "scripts" / "ci.sh"
CI_GATES_DIR = POLICY_ROOT / "scripts" / "ci" / "gates"
ROOT_JUSTFILE = POLICY_ROOT / "justfile"
ROOT_CMAKE = POLICY_ROOT / "CMakeLists.txt"
RUN_JUST = POLICY_ROOT / "scripts" / "dev" / "run_just.sh"
CANDIDATE_CHECKER = POLICY_ROOT / "scripts" / "checks" / "check_hook_parity.py"
MUTATION_HELPER = POLICY_ROOT / "scripts" / "checks" / "hook_parity_mutations.py"
TRUSTED_CHECKER = Path(__file__).resolve()
TRUSTED_RUNTIME = REPO_ROOT / "scripts" / "checks" / "hook_runtime_selftest.py"
TRUSTED_MUTATIONS = REPO_ROOT / "scripts" / "checks" / "hook_parity_mutations.py"
# The validator boundary is every module the validator EXECUTES. When this
# logic lived in two files both were pinned; the split to five left two
# unpinned, and a candidate that gutted
# hook_git_policy_selftest.run_hostile_owner_cases -- deleting every hostile
# HOME and hostile PATH proof -- was approved by the immutable HEAD validator.
# From the following commit onward those proofs would never run again.
#
# scripts/dev/git_environment.py is deliberately NOT here: run_bootstrap_
# validator_case rewrites it inside its fixture to emulate an older HEAD, so
# byte equality is the wrong instrument for that file.
CANDIDATE_BOUNDARY_MODULES = (
    "scripts/checks/hook_transport_support.py",
    "scripts/checks/hook_git_policy_selftest.py",
)
ABORTED = 3
EXPECTED_POLICY_FAILURE = 42

PRE_COMMIT_GATES = (
    "ascii",
    "copyright",
    "since",
    "format",
    "pre-commit-checks",
    "shebangs",
    "entry-points",
    "annotations",
    "doc-attachment",
    "toolchain-parity",
    "lint-py-shell",
    "lint-just",
    "cite-check",
    "hil-eil-parity",
    "roadmap-stats",
    "sbom",
    "soup-upstream",
)

STAGED_CHECKS = (
    "check_mcdc_block.py --staged",
    "check_new_compound_has_mcdc.py --staged",
    "check_obsolete_standards.py --staged",
)

JUST_EXECUTABLE = '"{{ just_executable() }}"'
HOOK_LAUNCHER = "scripts/dev/run_just.sh"
PRE_COMMIT_SHA256 = "d5cba09dfbdb9b03f3d94cd3fea59e4ca98c626c6edd85d499171c8b812222d5"
INSTALLED_LAUNCHER_SHA256 = "1ad13a9da6b76e6f8449ace4df6a535d2972d1062654899b353ccd1d4a863b08"
HOOK_INSTALLER_SHA256 = "18850cb6b3c06c2c1794b6f60103cd9acb584bf7f8745f1c877ff4f838119e86"
PROOF_WRITER_SHA256 = "09ec423b2f922c03f83504f92786fe018255ccefc31c0ef7c30bb53bb5ff5406"
BOOTSTRAP_ORDER = (
    "capture_source",
    "source_metadata",
    "resolve_owner_tools",
    "prepare_private_repository",
    "write_candidate_tree",
    "checkout_candidate_tree",
    "verify_candidate_tree",
    "prepare_head_control_plane",
    "make_policy_tools",
    "run_head_validator",
    "run_snapshot_policy",
    "make_owner_proof",
    "verify_completion_proof",
    "verify_source_unchanged",
)


class ParityError(RuntimeError):
    """A hook-parity self-test found a safety regression."""


def _fail(message: str) -> None:
    """Raise one self-test failure without embedding messages in exceptions."""
    raise ParityError(message)


def _recipe(text: str, name: str) -> str:
    """Return one top-level Just recipe, including its indented body."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith(f"{name}")), -1)
    if start < 0 or not lines[start].endswith(":"):
        return ""
    end = start + 1
    while end < len(lines):
        line = lines[end]
        if line and not line.startswith((" ", "\t")) and not line.startswith("#"):
            break
        end += 1
    return "\n".join(lines[start:end])


def _active_lines(recipe: str) -> tuple[str, ...]:
    """Return non-comment shell lines from a recipe."""
    return tuple(
        line.strip()
        for line in recipe.splitlines()[1:]
        if line.strip() and not line.lstrip().startswith("#")
    )


def _digest(path: Path) -> str:
    """Return the SHA-256 digest of one exact policy surface."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _check_candidate_control_plane(
    checker: bytes, runtime: bytes, mutation_helper: bytes, run_just: str
) -> list[str]:
    """Reject candidate attempts to replace the immutable validator boundary."""
    failures: list[str] = []
    if checker != TRUSTED_CHECKER.read_bytes():
        failures.append("candidate hook validator differs from immutable HEAD")
    if runtime != TRUSTED_RUNTIME.read_bytes():
        failures.append("candidate hook runtime validator differs from immutable HEAD")
    if mutation_helper != TRUSTED_MUTATIONS.read_bytes():
        failures.append("candidate hook mutation helper differs from immutable HEAD")
    for relative in CANDIDATE_BOUNDARY_MODULES:
        candidate = POLICY_ROOT / relative
        if not candidate.is_file():
            failures.append(f"candidate {relative} is absent from the validator boundary")
        elif candidate.read_bytes() != (REPO_ROOT / relative).read_bytes():
            failures.append(f"candidate {relative} differs from immutable HEAD")
    proof_names = ("RA8_STAGED_HOOK_PROOF", "RA8_STAGED_GATE_PROOF")
    if any(name in run_just for name in proof_names):
        failures.append("candidate run_just.sh branches on an owner proof capability")
    return failures


def _check_installation_surfaces() -> list[str]:
    """Pin the stable common-dir installer, launcher, and proof helper."""
    failures: list[str] = []
    failures.extend(
        _check_candidate_control_plane(
            CANDIDATE_CHECKER.read_bytes(),
            (POLICY_ROOT / "scripts/checks/hook_runtime_selftest.py").read_bytes(),
            MUTATION_HELPER.read_bytes(),
            RUN_JUST.read_text(encoding="utf-8"),
        )
    )
    exact = (
        (HOOK_LAUNCHER_FILE, INSTALLED_LAUNCHER_SHA256, "installed launcher"),
        (HOOK_INSTALLER, HOOK_INSTALLER_SHA256, "hook installer"),
        (PROOF_WRITER, PROOF_WRITER_SHA256, "atomic proof writer"),
    )
    failures.extend(
        f"{label} differs from its exact audited digest"
        for path, expected, label in exact
        if _digest(path) != expected
    )
    launcher = HOOK_LAUNCHER_FILE.read_text(encoding="utf-8")
    installer = HOOK_INSTALLER.read_text(encoding="utf-8")
    root_just = ROOT_JUSTFILE.read_text(encoding="utf-8")
    cmake = ROOT_CMAKE.read_text(encoding="utf-8")
    required_launcher = (
        "#!/bin/bash -p",
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "system_git=/usr/bin/git",
        'hook_args=("$@")',
        '"$system_git" -C "$root" cat-file blob "$blob"',
        '"$system_git" -C "$root" hash-object --no-filters "$owner"',
        # The dispatch must still be an exec, not a fork-and-wait: pinning only
        # the argv line would accept a launcher that keeps running as the parent.
        "exec env -u BASH_ENV -u ENV -u PYTHONHOME -u PYTHONPATH",
        '"$bash_bin" -p "$owner" "${hook_args[@]}"',
    )
    if any(token not in launcher for token in required_launcher):
        failures.append("installed launcher lost HEAD, argv, or signal ownership")
    required_installer = (
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "TRUSTED_GIT=/usr/bin/git",
        "--git-common-dir",
        'cat-file blob "$blob"',
        "refusing unmanaged",
    )
    if any(token not in installer for token in required_installer):
        failures.append("hook installer lost common-dir or unmanaged-path safety")
    if root_just.count("/bin/bash -p scripts/git/install-hooks.sh") != 1:
        failures.append("root Justfile must expose exactly one explicit hook installer")
    if "core.hooksPath" in cmake or "install-hooks.sh" in cmake:
        failures.append("CMake must not mutate Git hook configuration")
    return failures


def _check_wrappers(pre_commit: str, pre_push: str) -> list[str]:
    """Check that executable hook files remain transport-only wrappers."""
    failures: list[str] = []
    push_command = pre_push.replace("\\\n", " ")
    digest = hashlib.sha256(pre_commit.encode("utf-8")).hexdigest()
    if digest != PRE_COMMIT_SHA256:
        failures.append("pre-commit owner hook differs from its exact audited digest")
    if '"$root/scripts/ci.sh" --staged-hook' in pre_commit:
        failures.append("pre-commit still enters live ci.sh before snapshotting")
    main_start = pre_commit.rfind("main() {")
    main_body = pre_commit[main_start:] if main_start >= 0 else ""
    positions = tuple(main_body.find(name) for name in BOOTSTRAP_ORDER)
    if -1 in positions or positions != tuple(sorted(positions)):
        failures.append("pre-commit owner hook lost snapshot-first execution order")
    active = _active_lines("owner:\n" + pre_commit)
    if any(line.startswith(("source ", ". ")) for line in active) or "scripts/ci/lib" in pre_commit:
        failures.append("pre-commit owner hook imports live repository control code")
    if HOOK_LAUNCHER not in push_command or 'git_hooks::pre-push "$@"' not in push_command:
        failures.append("pre-push wrapper does not forward argv to git_hooks::pre-push")
    if pre_push.count(HOOK_LAUNCHER) != 1:
        failures.append("pre-push wrapper contains policy beyond one Just dispatch")
    for label, command in (("pre-push", push_command),):
        if (
            '--justfile "$root/justfile"' not in command
            or '--working-directory "$root"' not in command
        ):
            failures.append(f"{label} wrapper does not anchor the launcher at the repository root")
    return failures


def _check_pre_commit_flow(recipe: str, active: tuple[str, ...]) -> list[str]:
    """Check candidate dispatch after immutable HEAD validation."""
    failures: list[str] = []
    forbidden = ("RA8_STAGED_HOOK_PROOF", "RA8_STAGED_GATE_PROOF", "write-proof.py")
    if any(token in recipe for token in forbidden):
        failures.append("candidate pre-commit policy can access owner proof capability")
    if 'local gate="$1"' not in recipe:
        failures.append("pre-commit gate declaration is not isolated")
    if f'{JUST_EXECUTABLE} quality::local::gate "$gate"' not in recipe:
        failures.append("pre-commit lost its direct registered-gate dispatch")
    if any(line == "exit 0" for line in active):
        failures.append("pre-commit contains an early-success exit")
    return failures


def _check_pre_commit(hooks: str) -> list[str]:
    """Check staged semantics and the base hook's still-valid gate coverage."""
    failures: list[str] = []
    recipe = _recipe(hooks, "pre-commit")
    active = _active_lines(recipe)
    if not recipe:
        return ["hooks.just has no pre-commit recipe"]
    if recipe.count("#!/bin/bash -p") != 1:
        failures.append("pre-commit recipe lost its exact privileged Bash owner")
    snapshot_requirements = (
        '[[ "${RA8_STAGED_HOOK_SNAPSHOT:-0}" == "1" ]]',
        "git diff --quiet --no-ext-diff",
        "git ls-files --others --exclude-standard",
    )
    failures.extend(
        f"pre-commit lost staged-snapshot assertion: {requirement}"
        for requirement in snapshot_requirements
        if requirement not in recipe
    )
    gate_positions = tuple(recipe.find(f"\n        {gate}\n") for gate in PRE_COMMIT_GATES)
    if -1 in gate_positions:
        failures.append("pre-commit lost a registered gate")
    elif gate_positions != tuple(sorted(gate_positions)):
        failures.append("pre-commit registered gates were reordered")
    loop = '    for gate in "${gates[@]}"; do\n        run_gate "$gate"\n    done'
    if loop not in recipe:
        failures.append("pre-commit gate loop is dead, wrapped, or reordered")
    if f'{JUST_EXECUTABLE} quality::local::gate "$gate"' not in recipe:
        failures.append("pre-commit lost registered gate dispatch")
    failures.extend(
        f"pre-commit lost index-sensitive check: {check}"
        for check in STAGED_CHECKS
        if check not in recipe
    )
    if "git diff --cached --name-only --diff-filter=ACMR -z" not in recipe:
        failures.append("pre-commit C trigger is not derived from the staged index")
    failures.extend(
        f"pre-commit lost staged-C {gate} trigger"
        for gate in ("tidy", "cppcheck")
        if f"run_gate {gate}" not in recipe
    )
    failures.extend(_check_pre_commit_flow(recipe, active))
    prohibited = (
        "just ci",
        f"{JUST_EXECUTABLE} ci",
        "quality::run",
        "checks::devcontainer",
    )
    if any(any(token in line for token in prohibited) for line in active):
        failures.append("pre-commit invokes full/push-only CI")
    if any(line.startswith("just ") for line in active):
        failures.append("pre-commit uses PATH lookup instead of just_executable()")
    return failures


# Every token the snapshot bootstrap must still contain, hoisted out of
# _check_snapshot_dispatch so that function stays inside the NASA Power of 10
# Rule 4 length cap as the boundary grows.
BOOTSTRAP_REQUIREMENTS = (
    'SOURCE_INDEX="$(active_index_path "$SOURCE_ROOT")"',
    "install_strict_git_environment",
    "validate_inherited_alternates",
    "init_private_repository",
    "GIT_CONFIG_GLOBAL=/dev/null",
    "GIT_CONFIG_SYSTEM=/dev/null",
    "GIT_CONFIG_KEY_0=core.hooksPath",
    "GIT_CONFIG_KEY_1=core.fsmonitor",
    "GIT_CONFIG_KEY_2=core.attributesFile",
    'GIT_INDEX_FILE="$COPIED_INDEX"',
    'GIT_OBJECT_DIRECTORY="$CAPTURE_OBJECTS"',
    "start_new_session=True",
    "preexec_fn=reset_child_signals",
    "wait_policy_ready",
    "resolve_owner_tools",
    # The supervisor interpreters are fixed absolute paths and may never be
    # resolved out of the mutable source tree. This replaces a .venv PATH
    # marker that can no longer fire, because the candidate policy is
    # deliberately permitted to use that same ignored .venv.
    "OWNER_PYTHON=/usr/bin/python3",
    "OWNER_BASH=/bin/bash",
    '"$SOURCE_ROOT/"*) die "owner Just resolves through the mutable source tree" ;;',
    '[[ "$OWNER_PYTHON" == /* && "$OWNER_BASH" == /* && "$OWNER_JUST" == /* ]]',
    "RA8_OWNER_PATH=/usr/bin:/bin:/usr/sbin:/sbin",
    'PATH="$RA8_OWNER_PATH"',
    '"$account_home/.local/bin/just"',
    "make_policy_tools",
    "prepare_head_control_plane",
    "run_head_validator",
    "verify_bootstrap_policy_population",
    "head_supports_attribute_validation",
    'RA8_HOOK_POLICY_ROOT="$SNAPSHOT_DIR"',
    '"$OWNER_JUST"',
    '--shell "$OWNER_BASH" --clear-shell-args --shell-arg -puc',
    "activate_owner_signal_forwarding",
    "drain_group(proc",
    "signal.SIGKILL",
    "wait_group_empty",
    'OLDPWD="$SNAPSHOT_DIR"',
    "verify_source_unchanged",
    'wait "$pid"',
    "make_owner_proof",
    "verify_completion_proof",
)


def _check_snapshot_dispatch(ci_script: str, pre_commit: str) -> list[str]:
    """Pin immutable validation before trusted direct candidate dispatch."""
    failures: list[str] = []
    bootstrap_requirements = BOOTSTRAP_REQUIREMENTS
    failures.extend(
        f"pre-commit bootstrap lost exact behavior: {token}"
        for token in bootstrap_requirements
        if token not in pre_commit
    )
    if "--staged-hook" in ci_script or "selftest-staged-runner" in ci_script:
        failures.append("ci.sh retains an alternate live staged-hook front door")
    if "set -euo pipefail\nexit 0\n" in ci_script:
        failures.append("ci.sh contains an early-success exit before gate dispatch")
    if ci_script.count('run_gate_capture "$gate"') != 1:
        failures.append("ci.sh lost its single registered-gate dispatch")
    if any(token in ci_script for token in ("RA8_STAGED_GATE_PROOF", "write_staged_gate_proof")):
        failures.append("candidate ci.sh retains a forgeable proof capability")
    return failures


def _check_pre_push(hooks: str) -> list[str]:
    """Check LFS, pushed-commit policy, and the one full-CI invocation."""
    failures: list[str] = []
    recipe = _recipe(hooks, "pre-push remote url")
    if not recipe:
        return ["hooks.just has no pre-push recipe"]
    required = (
        "git lfs pre-push",
        "/bin/bash -p scripts/git/commit-msg --selftest",
        "COMMIT_IDENTITY=",
        '/bin/bash -p scripts/git/commit-msg "$message"',
        "git rev-list",
        "SKIP_CI_PUSH",
        "--working-directory",
        "ci_rc=$?",
        '[[ "$ci_rc" -eq 3 ]]',
    )
    failures.extend(
        f"pre-push lost required behavior: {token}" for token in required if token not in recipe
    )
    if recipe.count("#!/bin/bash -p") != 1:
        failures.append("pre-push recipe lost its exact privileged Bash owner")
    if "checks::devcontainer" in recipe or "quality::fast" in recipe:
        failures.append("pre-push runs a partial suite instead of root `just ci`")
    if "mapfile" in recipe or "readarray" in recipe:
        failures.append("pre-push uses an array builtin absent from macOS Bash 3.2")
    ci_command = [
        line for line in _active_lines(recipe) if line.startswith(f"{JUST_EXECUTABLE} --justfile")
    ]
    if len(ci_command) != 1 or " ci" not in recipe:
        failures.append("pre-push must invoke root `just ci` exactly once")
    if any(line.startswith("just ") for line in _active_lines(recipe)):
        failures.append("pre-push uses PATH lookup instead of just_executable()")
    return failures


def _source_reaches_runtime_proof(text: str) -> bool:
    """Let Bash parse/source a gate fragment and require post-source proof."""
    payload = f"{text}\nprintf 'RA8-SOURCE-PROOF\\n'\n"
    command = "set -euo pipefail; source /dev/stdin"
    result = subprocess.run(  # noqa: S603 -- fixed shell parses private policy text
        ["/bin/bash", "--noprofile", "--norc", "-p", "-c", command],
        input=payload,
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.endswith("RA8-SOURCE-PROOF\n")


def _check_gate_sources(gate_sources: tuple[str, ...]) -> list[str]:
    """Runtime-prove every sourced gate fragment returns to ci.sh dispatch."""
    failures: list[str] = []
    for number, text in enumerate(gate_sources, start=1):
        if not _source_reaches_runtime_proof(text):
            failures.append(f"gate source {number} bypasses its post-source runtime proof")
    return failures


def validate(
    pre_commit: str,
    pre_push: str,
    hooks: str,
    ci_script: str,
    gate_sources: tuple[str, ...],
) -> list[str]:
    """Return every hook parity failure found in the supplied texts."""
    failures = _check_installation_surfaces()
    failures.extend(_check_wrappers(pre_commit, pre_push))
    failures.extend(_check_snapshot_dispatch(ci_script, pre_commit))
    failures.extend(_check_gate_sources(gate_sources))
    if 'set working-directory := ".."' not in hooks:
        failures.append("hooks.just does not anchor recipes at the repository root")
    failures.extend(_check_pre_commit(hooks))
    failures.extend(_check_pre_push(hooks))
    return failures


def _live_texts() -> tuple[str, str, str, str, tuple[str, ...]]:
    """Read every executable hook-policy surface."""
    gate_sources = tuple(
        path.read_text(encoding="utf-8") for path in sorted(CI_GATES_DIR.glob("*.sh"))
    )
    return (
        PRE_COMMIT.read_text(encoding="utf-8"),
        PRE_PUSH.read_text(encoding="utf-8"),
        HOOKS_JUST.read_text(encoding="utf-8"),
        CI_SCRIPT.read_text(encoding="utf-8"),
        gate_sources,
    )


def _structural_selftest(texts: tuple[str, str, str, str, tuple[str, ...]]) -> None:
    """Prove all named control-flow and nonce regressions are rejected."""
    pre_commit, pre_push, hooks, ci_script, gate_sources = texts
    if validate(*texts):
        _fail("live hook policy was rejected by its own baseline")
    for number, case in enumerate(
        mutations.mutation_cases(pre_commit, hooks, ci_script, gate_sources), start=1
    ):
        mutated_pre_commit, mutated_hooks, mutated_ci, mutated_gates = case
        if not validate(mutated_pre_commit, pre_push, mutated_hooks, mutated_ci, mutated_gates):
            _fail(f"control-flow mutation {number} escaped")
    rejected_sources = (
        "   exit 0\n",
        "   return 0\n",
        "{ exit 0; }\n",
        "{ return 0; }\n",
        "if true; then exit 0; fi\n",
        "if true; then return 0; fi\n",
    )
    accepted_sources = (
        "# exit 0\ngate_fixture() { :; }\n",
        "( exit 0 )\ngate_fixture() { :; }\n",
        "( return 0 )\ngate_fixture() { :; }\n",
    )
    if any(not _check_gate_sources((source,)) for source in rejected_sources):
        _fail("active sourced-gate exit mutation escaped Bash runtime proof")
    if any(_check_gate_sources((source,)) for source in accepted_sources):
        _fail("comment or non-bypassing subshell was misclassified as active exit")
    checker = TRUSTED_CHECKER.read_bytes()
    runtime = TRUSTED_RUNTIME.read_bytes()
    mutation_helper = TRUSTED_MUTATIONS.read_bytes()
    run_just = RUN_JUST.read_text(encoding="utf-8")
    control_mutations = (
        (checker + b"\n# candidate mutation\n", runtime, mutation_helper, run_just),
        (checker, runtime + b"\n# candidate mutation\n", mutation_helper, run_just),
        (checker, runtime, mutation_helper + b"\n# candidate mutation\n", run_just),
        (
            checker,
            runtime,
            mutation_helper,
            "if [[ -n ${RA8_STAGED_HOOK_PROOF-} ]]; then exit 0; fi\n",
        ),
    )
    if any(not _check_candidate_control_plane(*mutation) for mutation in control_mutations):
        _fail("candidate validator or run_just proof mutation escaped")


def _git(
    root: Path,
    *args: str,
    env: dict[str, str] | None = None,
    input_data: bytes | None = None,
) -> bytes:
    """Run one checked Git command in an isolated synthetic repository."""
    proc = subprocess.run(  # noqa: S603 -- fixed fixture command
        [trusted_git_executable(), "-C", str(root), *args],
        env=sanitized_git_environment() if env is None else env,
        input=input_data,
        capture_output=True,
        check=False,
    )
    if proc.returncode:
        _fail(proc.stderr.decode(errors="replace").strip())
    return proc.stdout


def _write_fixture_file(path: Path, text: str, *, executable: bool = False) -> None:
    """Write one synthetic fixture file and optionally make it executable."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


POLICY_FIXTURE_FILES = (  # noqa: SIM905 -- compact fixed census stays below size cap
    "CMakeLists.txt justfile just/hooks.just scripts/checks/check_hook_parity.py "
    "scripts/checks/hook_git_policy_selftest.py scripts/checks/hook_runtime_selftest.py "
    "scripts/checks/hook_parity_mutations.py "
    "scripts/checks/hook_transport_support.py "
    "scripts/ci.sh scripts/dev/git_environment.py "
    "scripts/dev/run_just.sh scripts/git/hook-launcher scripts/git/install-hooks.sh "
    "scripts/git/pre-commit scripts/git/pre-push scripts/git/write-proof.py"
).split()


def _make_transport_fixture(root: Path, staged: str, worktree: str) -> None:
    """Create a candidate index whose policy mode differs from its worktree."""
    _git(root, "init", "--quiet")
    _git(root, "config", "user.email", "selftest@invalid")
    _git(root, "config", "user.name", "selftest")
    for relative in POLICY_FIXTURE_FILES:
        source = REPO_ROOT / relative
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for source in sorted((REPO_ROOT / "scripts/ci/gates").glob("*.sh")):
        destination = root / "scripts/ci/gates" / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    transport.write_transport_justfiles(root)
    _write_fixture_file(root / "policy-mode", "success\n")
    _write_fixture_file(root / ".gitignore", ".venv/\nignored-dir/*\n")
    for index in range(6):
        _write_fixture_file(root / f"policy-attributes/{index}/.gitattributes", "* text\n")
    for index in range(26):
        _write_fixture_file(root / f"policy-ignores/{index}/.gitignore", "scratch\n")
    _write_fixture_file(root / "delete-me", "delete\n")
    _write_fixture_file(root / "resurrect-me", "original\n")
    _write_fixture_file(root / "mode.sh", "#!/usr/bin/env bash\n")
    _write_fixture_file(root / "link-target", "target\n")
    _write_fixture_file(root / "conflict.txt", "base\n")
    _write_fixture_file(root / "ignored-dir/tracked.txt", "tracked despite ignore\n")
    _git(root, "add", ".")
    _git(root, "add", "-f", "ignored-dir/tracked.txt")
    _git(root, "commit", "--quiet", "-m", "fixture")
    _write_fixture_file(root / "policy-mode", f"{staged}\n")
    _git(root, "add", "policy-mode")
    _write_fixture_file(root / "policy-mode", f"{worktree}\n")


def _source_state(root: Path, index: Path | None = None) -> tuple[str, tuple[tuple[str, str], ...]]:
    """Hash the source index and every loose/packed object-store file."""
    index_path = index or (root / ".git/index")
    index_digest = hashlib.sha256(index_path.read_bytes()).hexdigest()
    objects = root / ".git/objects"
    object_state = tuple(
        (path.relative_to(objects).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest())
        for path in sorted(objects.rglob("*"))
        if path.is_file()
    )
    return index_digest, object_state


def _run_owner(
    root: Path, temp_root: Path, extra_env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """Run the audited owner hook against one synthetic active index."""
    environment = os.environ.copy() if extra_env is None else extra_env.copy()
    environment["TMPDIR"] = str(temp_root)
    return subprocess.run(  # noqa: S603 -- audited hook path
        ["/bin/bash", "-p", str(PRE_COMMIT)],
        cwd=root,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )


def _transport_case(base: Path, name: str, staged: str, worktree: str, expected: int) -> None:
    """Prove the hook rules only on candidate-index policy bytes."""
    root = base / name
    temp_root = base / f"{name}-tmp"
    root.mkdir()
    temp_root.mkdir()
    _make_transport_fixture(root, staged, worktree)
    marker = base / f"{name}.venv"
    transport.install_venv_wrappers(root, marker)
    before = _source_state(root)
    environment = transport.transport_environment(base, root)
    inherited = environment["PATH"]
    environment.update(
        PATH=f"{root / '.venv/bin'}:{inherited}",
        RA8_SELFTEST_VENV=str(marker),
    )
    result = _run_owner(
        root,
        temp_root,
        environment,
    )
    if result.returncode != expected:
        _fail(f"{name}: expected {expected}, got {result.returncode}: {result.stderr}")
    if tuple(temp_root.iterdir()):
        _fail(f"{name}: snapshot residue remained")
    if _source_state(root) != before:
        _fail(f"{name}: source index or object store changed")
    if marker.exists():
        _fail(f"{name}: an ignored source .venv wrapper became a trusted owner tool")


def _wait_for_path(path: Path, process: subprocess.Popen[str]) -> None:
    """Wait briefly for the staged fixture child to report readiness."""
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            _fail(f"signal fixture exited before ready: {process.returncode}")
        time.sleep(0.02)
    _fail("signal fixture did not become ready")


def _kill_ready_group(ready: Path) -> None:
    """Kill one synthetic policy group recorded by its supervisor."""
    try:
        pgid = int(ready.read_text(encoding="ascii").strip())
        os.killpg(pgid, signal.SIGKILL)
    except (OSError, ValueError):
        return


def _force_fixture_cleanup(process: subprocess.Popen[str], temp_root: Path) -> None:
    """Kill both synthetic owner and policy groups after a test timeout."""
    for ready in tuple(temp_root.rglob("policy-ready")):
        _kill_ready_group(ready)
    if process.poll() is None:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
    with suppress(subprocess.TimeoutExpired):
        process.wait(timeout=5)


def _signal_case(base: Path, sig: signal.Signals) -> None:
    """Signal only the owner PID and prove its policy group is reaped."""
    root = base / f"signal-{sig.name.lower()}"
    temp_root = base / f"signal-{sig.name.lower()}-tmp"
    ready = base / f"{sig.name}.ready"
    continued = base / f"{sig.name}.continued"
    root.mkdir()
    temp_root.mkdir()
    _make_transport_fixture(root, "hang", "success")
    environment = transport.transport_environment(base, root)
    environment.update(
        RA8_SELFTEST_VENV=str(base / "signal.venv"),
        TMPDIR=str(temp_root),
        RA8_SELFTEST_READY=str(ready),
        RA8_SELFTEST_CONTINUED=str(continued),
    )
    process = subprocess.Popen(  # noqa: S603 -- audited hook path
        default_signal_test_command("/bin/bash", "-p", str(PRE_COMMIT)),
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        _wait_for_path(ready, process)
        os.kill(process.pid, sig)
        _stdout, stderr = process.communicate(timeout=20)
    finally:
        _force_fixture_cleanup(process, temp_root)
    if process.returncode != ABORTED or "ABORTED" not in stderr:
        _fail(f"{sig.name}: owner did not report UNKNOWN: {process.returncode}")
    if continued.exists() or tuple(temp_root.iterdir()):
        _fail(f"{sig.name}: child continued or snapshot residue remained")


def _shape_case(base: Path) -> None:
    """Prove candidate add/delete/mode/link/ignore semantics and spaces."""
    root, temp_root = base / "shape", base / "shape-tmp"
    root.mkdir()
    temp_root.mkdir()
    _make_transport_fixture(root, "inspect", "failure")
    _write_fixture_file(root / "path with spaces/added.txt", "added\n")
    _git(root, "add", "path with spaces/added.txt")
    _git(root, "rm", "delete-me", "resurrect-me")
    _write_fixture_file(root / "resurrect-me", "worktree resurrection\n")
    (root / "mode.sh").chmod(0o755)
    _git(root, "add", "mode.sh")
    (root / "alias").symlink_to("link-target")
    _git(root, "add", "alias")
    _write_fixture_file(root / "untracked.txt", "exclude me\n")
    before = _source_state(root)
    environment = transport.transport_environment(base, root)
    environment["RA8_SELFTEST_VENV"] = str(base / "shape.venv")
    result = _run_owner(root, temp_root, environment)
    if result.returncode or _source_state(root) != before or tuple(temp_root.iterdir()):
        _fail(f"candidate-shape fidelity failed: {result.returncode}: {result.stderr}")


def _custom_index_case(base: Path) -> None:
    """Prove hostile hook routing selects an inherited index with spaces."""
    root, temp_root = base / "custom-index", base / "custom-index-tmp"
    root.mkdir()
    temp_root.mkdir()
    _make_transport_fixture(root, "failure", "success")
    custom = root / ".git/custom index"
    shutil.copy2(root / ".git/index", custom)
    _git(root, "reset", "--mixed", "HEAD")
    before_custom = _source_state(root, custom)
    before_default = _source_state(root)
    environment = {
        "GIT_DIR": str(root / ".git"),
        "GIT_WORK_TREE": str(root),
        "GIT_INDEX_FILE": ".git/custom index",
        "GIT_PREFIX": "hostile/",
    }
    environment.update(transport.transport_environment(base, root))
    environment["GIT_INDEX_FILE"] = ".git/custom index"
    environment["RA8_SELFTEST_VENV"] = str(base / "custom.venv")
    result = _run_owner(root, temp_root, environment)
    if result.returncode != EXPECTED_POLICY_FAILURE:
        _fail(f"custom index was not authoritative: {result.returncode}: {result.stderr}")
    if _source_state(root, custom) != before_custom or _source_state(root) != before_default:
        _fail("custom/default index or source objects changed")


def _conflicted_index_case(base: Path) -> None:
    """Prove an unmerged active index fails closed without residue."""
    root, temp_root = base / "conflict", base / "conflict-tmp"
    root.mkdir()
    temp_root.mkdir()
    _make_transport_fixture(root, "success", "success")
    base_blob = os.fsdecode(_git(root, "rev-parse", "HEAD:conflict.txt")).strip()
    _write_fixture_file(root / "ours", "ours\n")
    _write_fixture_file(root / "theirs", "theirs\n")
    ours = os.fsdecode(_git(root, "hash-object", "-w", "ours")).strip()
    theirs = os.fsdecode(_git(root, "hash-object", "-w", "theirs")).strip()
    index_info = (
        f"100644 {base_blob} 1\tconflict.txt\n"
        f"100644 {ours} 2\tconflict.txt\n"
        f"100644 {theirs} 3\tconflict.txt\n"
    )
    _git(root, "update-index", "--index-info", input_data=index_info.encode("ascii"))
    before = _source_state(root)
    environment = transport.transport_environment(base, root)
    environment["RA8_SELFTEST_VENV"] = str(base / "conflict.venv")
    result = _run_owner(root, temp_root, environment)
    if result.returncode == 0 or _source_state(root) != before or tuple(temp_root.iterdir()):
        _fail("conflicted active index did not fail closed")


def _transport_selftest() -> None:
    """Exercise staged-vs-worktree selection, exact shape, and signals."""
    with tempfile.TemporaryDirectory(prefix="ra8-hook-parity-") as temporary:
        base = Path(temporary)
        _transport_case(base, "staged-wins", "success", "failure", 0)
        _transport_case(base, "failure-wins", "failure", "success", 42)
        _shape_case(base)
        _custom_index_case(base)
        _conflicted_index_case(base)
        transport.run_bootstrap_validator_case(
            base,
            (
                _make_transport_fixture,
                _git,
                _source_state,
                _run_owner,
                _fail,
            ),
        )
        _hostile(
            base,
            (
                _make_transport_fixture,
                _git,
                _source_state,
                _run_owner,
                transport.transport_environment,
            ),
        )
        _signal_case(base, signal.SIGTERM)
        _signal_case(base, signal.SIGINT)


def candidate_selftest() -> int:
    """Run immutable structural mutation proofs against one candidate root."""
    try:
        _structural_selftest(_live_texts())
    except (OSError, ParityError, subprocess.TimeoutExpired) as exc:
        print(f"check_hook_parity.py: candidate selftest failed: {exc}", file=sys.stderr)
        return 1
    print("check_hook_parity.py: candidate selftest passed")
    return 0


def selftest() -> int:
    """Prove structural guards and the owner transport against regressions."""
    try:
        _structural_selftest(_live_texts())
        _transport_selftest()
        run_runtime_selftests()
    except (OSError, ParityError, subprocess.TimeoutExpired) as exc:
        print(f"check_hook_parity.py: selftest failed: {exc}", file=sys.stderr)
        return 1
    print("check_hook_parity.py: selftest passed")
    return 0


def main() -> int:
    """Run the self-test or validate the live hook files."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:] == ["--candidate-selftest"]:
        return candidate_selftest()
    if sys.argv[1:]:
        print("usage: check_hook_parity.py [--selftest|--candidate-selftest]", file=sys.stderr)
        return 2
    failures = validate(*_live_texts())
    for failure in failures:
        print(f"check_hook_parity.py: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("check_hook_parity.py: hook wrappers and Just policy are in parity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

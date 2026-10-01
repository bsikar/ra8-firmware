# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Governed inputs for HIL convergence safety."""

from __future__ import annotations

import json
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
DEV_ENTRY = "infra/ansible/roles/dev_box/tasks/hil_runner.yml"
DEV_ROLE = "infra/ansible/roles/dev_box/tasks/hil_runner_transaction.yml"
DEV_MAIN_ENTRY = "infra/ansible/roles/dev_box/tasks/main.yml"
DEV_MAIN = "infra/ansible/roles/dev_box/tasks/transaction.yml"
DEV_IMAGE_LOCK = "infra/ansible/roles/dev_box/tasks/image_lock.yml"
DEV_GUARD = "infra/ansible/roles/dev_box/tasks/hil_mutation_guard.yml"
DEV_HANDLER = "infra/ansible/roles/dev_box/handlers/main.yml"
IDLE_HELPER = "infra/ansible/roles/dev_box/files/ra8-hil-runner-idle-stop.py"
BENCH_ENTRY = "infra/ansible/roles/hil_bench/tasks/main.yml"
BENCH_ROLE = "infra/ansible/roles/hil_bench/tasks/transaction.yml"
BENCH_GUARD = "infra/ansible/roles/hil_bench/tasks/transaction_guard.yml"
C6_ENTRY = "infra/ansible/roles/c6_toolchain/tasks/main.yml"
C6_ROLE = "infra/ansible/roles/c6_toolchain/tasks/transaction.yml"
AD2_ENTRY = "infra/ansible/roles/ad2_tools/tasks/main.yml"
AD2_ROLE = "infra/ansible/roles/ad2_tools/tasks/transaction.yml"
BENCH_DEFAULTS = "infra/ansible/roles/hil_bench/defaults/main.yml"
FLEET = "scripts/dev/fleet.py"
FLEET_BENCH = "scripts/dev/fleet_bench.py"
FLEET_CAPACITY_CLIENT = "scripts/dev/fleet_capacity_client.py"
FLEET_RECONCILE = "scripts/dev/fleet_reconcile.py"
FLEET_RECONCILE_ARC_SELFTEST = "scripts/dev/fleet_reconcile_arc_selftest.py"
FLEET_RECONCILE_PROCESS = "scripts/dev/fleet_reconcile_process.py"
FLEET_RUNNER = "scripts/dev/fleet_runner_maintenance.py"
FLEET_WSL = "scripts/dev/fleet_wsl.py"
FLEET_WSL_STAGE = "scripts/dev/fleet_wsl_stage.py"
FLEET_MODEL = "scripts/dev/fleet_model.py"
FLEET_RUNNER_MODEL = "scripts/dev/fleet_runner_model.py"
FLEET_REACH = "scripts/dev/fleet_reach.py"
FLEET_PATH_AUTHORITY = "scripts/dev/fleet_path_authority.py"
GATE = "scripts/ci/gates/checks.sh"
HIL_JUST = "just/hil.just"
DECLARATION = "infra/fleet.yml"
PLAYBOOKS = (
    "infra/ansible/playbooks/dev-box.yml",
    "infra/ansible/playbooks/hil-bench.yml",
)
class WorkflowPolicyError(ValueError):
    """A playbook cannot provide a safe workflow dependency closure."""


def load_bench_transaction(root: Path) -> object:
    """Follow the public bench role into its one owned dynamic transaction."""
    entry_path = root / BENCH_ENTRY
    entry = yaml.safe_load(entry_path.read_text(encoding="utf-8"))
    if not isinstance(entry, list) or len(entry) != 1 or not isinstance(entry[0], dict):
        message = "HIL bench role entry is not one dynamic transaction"
        raise WorkflowPolicyError(message)
    include = entry[0].get("ansible.builtin.include_tasks")
    if not isinstance(include, dict) or set(include) != {"file"}:
        message = "HIL bench role entry does not own one task file"
        raise WorkflowPolicyError(message)
    relative = include["file"]
    if relative != "transaction.yml":
        message = "HIL bench role transaction authority drifted"
        raise WorkflowPolicyError(message)
    transaction = entry_path.with_name(relative)
    if transaction.is_symlink() or not transaction.is_file():
        message = "HIL bench role transaction is unavailable or linked"
        raise WorkflowPolicyError(message)
    return yaml.safe_load(transaction.read_text(encoding="utf-8"))


def _image_lock_selftest_process_tokens() -> tuple[str, ...]:
    """Return bounded process, group, readiness, and worker-proof tokens."""
    return (
        '[[ "${BASH_SOURCE[0]}" != "$0" ]]',
        '      exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
        "write_fake_image_runtime() {",
        'RA8_CONTAINER_RUNTIME="$fake_runtime" RA8_IMAGE_LOCK_DIR="$managed" \\\n'
        "        cmd_ensure --rebuild",
        'SELFTEST_DEADLINE_STEPS="${SELFTEST_DEADLINE_STEPS:?}"',
        "bounded_child_reap() {",
        '  bounded_process_terminal "$pid" || return 1\n  if wait "$pid"; then',
        "bounded_process_terminal() {\n"
        '  local pid="$1" steps="${2:-$SELFTEST_DEADLINE_STEPS}" attempt\n'
        "  for ((attempt = 0; attempt < steps; ++attempt)); do",
        "bounded_group_empty() {\n"
        '  local pgid="$1" steps="${2:-$SELFTEST_DEADLINE_STEPS}" attempt live\n'
        "  for ((attempt = 0; attempt < steps; ++attempt)); do",
        "bounded_group_gone() {\n"
        '  local pgid="$1" attempt members\n'
        "  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do",
        "bounded_process_absent() {\n"
        '  local pid="$1" attempt\n'
        "  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do",
        "wait_for_status_file() {\n"
        '  local path="$1" pid="$2" attempt\n'
        "  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do",
        '    if process_is_terminal "$pid"; then\n'
        '      [[ -s "$path" ]] && return 0\n'
        "      return 1",
        "cleanup_image_lock_case() {",
        "force_signal_controller_cleanup() {",
        "fresh_lock_probe() {\n"
        '  local lock="$SELFTEST_MANAGED_DIR/devcontainer-image.lock"\n'
        '  if ! exec 7<"$lock"; then\n'
        "    return 1\n"
        "  fi\n"
        "  if ! flock -n 7; then",
        "worker_group_is_safe() {\n"
        "  local own_pgid\n"
        '  own_pgid="$(ps -o pgid= -p "$$" | tr -d \' \')"',
    )


def _image_lock_selftest_cleanup_tokens() -> tuple[str, ...]:
    """Return group termination, lock release, and trap-lifecycle tokens."""
    return (
        '[[ "$SELFTEST_WORKER_PGID" == "$SELFTEST_WORKER_PID" ]]',
        '[[ "$SELFTEST_WORKER_PGID" != "$own_pgid" ]]',
        "worker_group_signal_is_authorized() {\n"
        '  worker_group_is_safe && [[ "$SELFTEST_WORKER_PGID" == "$SELFTEST_WORKER_PID" ]] &&\n'
        '    shell_owns_live_child "$SELFTEST_WORKER_PID"',
        "if worker_group_is_safe 2>/dev/null && ! bounded_group_empty "
        '"$SELFTEST_WORKER_PGID"; then\n'
        "    if worker_group_signal_is_authorized 2>/dev/null; then\n"
        "      group_signal_authorized=1\n"
        '      kill -TERM -- "-$SELFTEST_WORKER_PGID"',
        'if [[ "$SELFTEST_WORKER_SHARED_GROUP" != "1" ]] &&\n'
        "    worker_group_is_safe 2>/dev/null && ! bounded_group_empty "
        '"$SELFTEST_WORKER_PGID" 50; then\n'
        '    if [[ "$group_signal_authorized" == "1" ]] &&\n'
        "      worker_group_signal_is_authorized 2>/dev/null; then\n"
        '      kill -KILL -- "-$SELFTEST_WORKER_PGID"',
        '    signal_owned_controller_group TERM "$controller" 2>/dev/null || return 1',
        '  if ! bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS"; then',
        '    kill -KILL -- "-$controller" 2>/dev/null || return 1',
        "release_parent_lock() {\n"
        "  local release_failed=0\n"
        '  if [[ "$SELFTEST_PARENT_LOCK_OPEN" == "1" ]]; then\n'
        "    flock -u 8 || release_failed=1",
        "  trap image_lock_case_exit EXIT",
        "clear_image_lock_case_traps() {\n  restore_selftest_root_traps",
        "  trap 'image_lock_case_signal 129' HUP",
        "  trap 'image_lock_case_signal 130' INT",
        "  trap 'image_lock_case_signal 143' TERM",
    )


def _image_lock_selftest_scenario_tokens() -> tuple[str, ...]:
    """Return attack dispatch, completion, and fallback-proof tokens."""
    return (
        'image_lock_selftest_worker() {\n  local mode="$1" managed="$2" case_dir="$3" fake_runtime',
        'record_worker_group "$case_dir" "$mode"\n'
        '  wait_for_worker_ack "$case_dir" "$$" || exit 124',
        "      exec 8>&-\n      trap '' HUP INT TERM\n"
        '      exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
        "  begin_selftest_spawn_critical abort_bound_worker_spawn || return 1",
        '  selftest_inject_bound_exit worker "$SELFTEST_WORKER_PID"',
        "  if ! read_worker_group; then\n    abort_bound_worker_spawn 1\n  fi",
        "early-exit)",
        "pre-ready-hang)",
        "normal | post-ready-build-hang)",
        "start_image_lock_worker post-ready-build-hang ||",
        "  signal-controller)",
        'wait_for_status_file "$SELFTEST_CASE_DIR/build-entered.status" "$SELFTEST_WORKER_PID"',
        "assert_no_surviving_descendants() {",
        "selftest_forced_build_contention() {",
        "  if fresh_lock_probe; then\n"
        '    die "selftest: fresh lock probe accepted a held lock"\n'
        "  fi",
        '  fresh_lock_probe || die "selftest: fresh lock probe failed after worker completion"',
        'reap_worker || die "selftest: normal child did not finish"',
        'wait_for_status_file "$SELFTEST_CASE_DIR/done.status" '
        '"$SELFTEST_WORKER_PID" ||\n'
        '    die "selftest: forced rebuild did not complete after release"',
        "assert_no_surviving_descendants || cleanup_failed=1",
        "fresh_lock_probe || cleanup_failed=1",
        'bounded_group_gone "$SELFTEST_WORKER_PGID" || cleanup_failed=1',
        'bounded_process_absent "$pid" || return 1',
    )


def image_lock_cases_required_tokens() -> tuple[str, ...]:
    """Return unique allocation and suite tokens owned by the cases helper."""
    return (
        '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_CASES_PARENT"',
        "  begin_selftest_spawn_critical allocation_bound_spawn_signal ||",
        '  selftest_controller_launcher_refusals "$tmp"',
        '  selftest_controller_persisted_ps_failure "$tmp" ||',
        '  run_image_lock_scenario early-exit selftest_early_exit "$tmp"',
        '  run_image_lock_scenario pre-ready-hang selftest_pre_ready_hang "$tmp"',
        '  run_image_lock_scenario forced-build-contention selftest_forced_build_contention "$tmp"',
        '  run_image_lock_scenario post-ready-hang selftest_post_ready_hang "$tmp"',
        '  run_image_lock_scenario signal-ready-timeout selftest_signal_ready_timeout "$tmp"',
        '  run_image_lock_scenario signal-cleanup selftest_signal_cleanup "$tmp"',
    )


def image_lock_receipt_required_tokens() -> tuple[str, ...]:
    """Return source authority and interfaces owned by the receipt helper."""
    return (
        '[[ "${BASH_SOURCE[0]}" != "$0" ]] || {',
        '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_LOCK_RECEIPT_PARENT"',
        '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
        '"$SELFTEST_LOCK_RECEIPT_PARENT_DIR/devcontainer_image.sh"',
        "expected_image_lock_suite_receipts() {",
        "scenario_receipt_value() {",
        "validate_scenario_receipt_directory() {",
        "write_scenario_receipt() {",
        "require_scenario_receipt() {",
        "verify_scenario_receipt_files() {",
        "expected_cleanup_receipt() {",
        "expected_force_cleanup_receipt() {",
        "parent_lock_fd_is_closed() {",
        "require_cleanup_receipt() {",
        "require_force_cleanup_receipt() {",
        "write_worker_cleanup_proof_file() {",
        "require_worker_cleanup_proof_file() {",
        "write_controller_cleanup_receipt_file() {",
        "require_controller_cleanup_receipt_file() {",
    )


def image_lock_signal_required_tokens() -> tuple[str, ...]:
    """Return unique signal-controller tokens owned by the signal helper."""
    return (
        '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_SIGNAL_PARENT"',
        "selftest_signal_ready_timeout() {",
        "selftest_signal_cleanup() {",
        '  [[ ! -e "$ready" && ! -L "$ready" && ! -e "$ack" && ! -L "$ack" ]] || return 1\n'
        "  begin_selftest_spawn_critical controller_bound_spawn_signal || return 1",
        '  [[ ! -e "$ready" && ! -L "$ready" ]] || return 1\n'
        "  begin_selftest_spawn_critical controller_bound_spawn_signal || return 1",
        '  selftest_inject_bound_exit controller "$controller_pid"',
        '    kill -KILL -- "-$controller" 2>/dev/null || '
        'bounded_group_gone "$controller" || return 1',
        'selftest_signal_cleanup() {\n  local tmp="$1" signal expected '
        "controller case_dir managed launcher_mode\n  for signal in HUP INT TERM; do",
        'bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS"',
        '  force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||\n'
        '    die "selftest: handler-hang group fallback failed"',
        'if ! wait_for_status_file "$case_dir/controller-ready.status" "$controller"; then\n'
        '      SELFTEST_FORCE_CLEANUP_RECEIPT=""\n'
        '      force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||\n'
        '        die "selftest: $signal unready-controller cleanup failed"',
    )


def _image_lock_selftest_semantic_tokens() -> tuple[str, ...]:
    """Return fail-closed managed-object attack tokens."""
    return (
        'if (RA8_IMAGE_LOCK_DIR="$tmp/missing-marker" resolve_image_lock ',
        'if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then\n'
        '    die "selftest: symlinked managed image lock group marker passed"',
        'if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then\n'
        '    die "selftest: multiply-linked managed image lock group marker passed"',
        'die "selftest: wrong managed image lock directory group passed"',
        'die "selftest: wrong managed image lock file group passed"',
        'die "selftest: non-root managed image lock group marker passed"',
        'die "selftest: writable managed image lock group marker passed"',
        'die "selftest: root group in managed image lock group marker passed"',
        'die "selftest: binary managed image lock group marker passed"',
    )


def image_lock_selftest_required_tokens() -> tuple[str, ...]:
    """Return every unique token required from the image-lock selftest harness."""
    return (
        '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_LOCK_HELPER_PARENT"',
        *_image_lock_selftest_process_tokens(),
        *_image_lock_selftest_cleanup_tokens(),
        *_image_lock_selftest_scenario_tokens(),
        *_image_lock_selftest_semantic_tokens(),
    )


def _image_selftest_source_paths() -> dict[str, str]:
    """Return the complete image-selftest source authority mapping."""
    return {
        "devcontainer_image": "scripts/ci/devcontainer_image.sh",
        "image_lock_digest": "scripts/checks/hil_convergence_safety_image_lock_digest.py",
        "image_harness_policy": ("scripts/checks/hil_convergence_safety_image_harness_policy.py"),
        "image_process_analysis": (
            "scripts/checks/hil_convergence_safety_image_process_analysis.py"
        ),
        "image_process_policy": ("scripts/checks/hil_convergence_safety_image_process_policy.py"),
        "image_subreaper_policy": (
            "scripts/checks/hil_convergence_safety_image_subreaper_policy.py"
        ),
        "process_source_fixtures": (
            "scripts/checks/hil_convergence_safety_process_source_fixtures.py"
        ),
        "raw_digest_controls": "scripts/checks/hil_convergence_safety_raw_digest_controls.py",
        "raw_digest_runtime": "scripts/checks/hil_convergence_safety_raw_digest_runtime.py",
        "runtime_cleanup": "scripts/checks/hil_convergence_safety_runtime_cleanup.py",
        "runtime_escape": "scripts/checks/hil_convergence_safety_runtime_escape.py",
        "runtime_fixtures": "scripts/checks/hil_convergence_safety_runtime_fixtures.py",
        "runtime_launcher": "scripts/checks/hil_convergence_safety_runtime_launcher.py",
        "runtime_loader": "scripts/checks/hil_convergence_safety_runtime_loader.py",
        "runtime_loader_harness": "scripts/checks/hil_convergence_safety_runtime_loader_harness.py",
        "runtime_root_swap": "scripts/checks/hil_convergence_safety_runtime_root_swap.py",
        "runtime_mutations": "scripts/checks/hil_convergence_safety_runtime_mutations.py",
        "runtime_sources": "scripts/checks/hil_convergence_safety_runtime_sources.py",
        "process_mutations": "scripts/checks/hil_convergence_safety_process_mutations.py",
        "semantic_mutations": "scripts/checks/hil_convergence_safety_semantic_mutations.py",
        "hil_convergence_entry": "scripts/checks/check_hil_convergence_safety.py",
        "source_fixtures": "scripts/checks/hil_convergence_safety_source_fixtures.py",
        "devcontainer_image_lock_receipts": "scripts/ci/devcontainer_image_lock_receipts.bash",
        "devcontainer_image_lock_selftest": "scripts/ci/devcontainer_image_lock_selftest.bash",
        "devcontainer_image_selftest": "scripts/ci/devcontainer_image_selftest.bash",
        "devcontainer_image_bound_exit_selftest": (
            "scripts/ci/devcontainer_image_bound_exit_selftest.bash"
        ),
        "devcontainer_image_selftest_cases": "scripts/ci/devcontainer_image_selftest_cases.bash",
        "devcontainer_image_signal_selftest": "scripts/ci/devcontainer_image_signal_selftest.bash",
        "devcontainer_image_selftest_supervisor": (
            "scripts/ci/devcontainer_image_selftest_supervisor.py"
        ),
        "devcontainer_image_selftest_supervisor_cases": (
            "scripts/ci/devcontainer_image_selftest_supervisor_cases.py"
        ),
        "devcontainer_image_selftest_process": (
            "scripts/ci/devcontainer_image_selftest_process.py"
        ),
    }


def _governed_source_paths() -> dict[str, str]:
    """Return the complete governed source-name to repository-path mapping."""
    return {
        "dev_role": DEV_ROLE,
        "dev_entry": DEV_ENTRY,
        "dev_main": DEV_MAIN,
        "dev_image_lock": DEV_IMAGE_LOCK,
        "dev_defaults": "infra/ansible/roles/dev_box/defaults/main.yml",
        "dev_main_entry": DEV_MAIN_ENTRY,
        "dev_guard": DEV_GUARD,
        "idle_helper": IDLE_HELPER,
        "fleet": FLEET,
        "fleet_bench": FLEET_BENCH,
        "fleet_capacity_client": FLEET_CAPACITY_CLIENT,
        "fleet_reconcile": FLEET_RECONCILE,
        "fleet_reconcile_arc_selftest": FLEET_RECONCILE_ARC_SELFTEST,
        "fleet_reconcile_process": FLEET_RECONCILE_PROCESS,
        "fleet_runner": FLEET_RUNNER,
        "fleet_wsl": FLEET_WSL,
        "fleet_wsl_stage": FLEET_WSL_STAGE,
        "fleet_model": FLEET_MODEL,
        "fleet_runner_model": FLEET_RUNNER_MODEL,
        "fleet_reach": FLEET_REACH,
        "fleet_path_authority": FLEET_PATH_AUTHORITY,
        "gate": GATE,
        "bench_role": BENCH_ROLE,
        "bench_entry": BENCH_ENTRY,
        "bench_guard": BENCH_GUARD,
        "c6_role": C6_ROLE,
        "c6_entry": C6_ENTRY,
        "ad2_role": AD2_ROLE,
        "ad2_entry": AD2_ENTRY,
        "bench_defaults": BENCH_DEFAULTS,
        "declaration": DECLARATION,
        "dev_playbook": PLAYBOOKS[0],
        "bench_playbook": PLAYBOOKS[1],
        "root_justfile": "justfile",
        "infra_just": "just/infra.just",
        "hil_just": HIL_JUST,
        "infra_sh": "scripts/dev/infra.sh",
        "infra_bootstrap": "infra/bootstrap.sh",
        "dockerfile": ".devcontainer/Dockerfile",
        **_image_selftest_source_paths(),
        "dockerignore": ".dockerignore",
        "ci_runner": "infra/ansible/roles/ci_runner/tasks/main.yml",
        "wsl_role": "infra/ansible/roles/wsl_ci_host/tasks/main.yml",
        "setup_ansible": "scripts/dev/setup_ansible.sh",
        "provision_toolchain": "scripts/dev/provision_dev_box_toolchain.sh",
        "bench_client": "scripts/hil/lib/bench_client.sh",
        "bench_host": "scripts/hil/lib/bench_host.sh",
        "bench_lock_verify": "scripts/hil/lib/bench_lock_verify.py",
        "bench_lock_capability": "scripts/dev/bench_lock_capability.py",
        "fleet_transaction_auth": "scripts/dev/fleet_transaction_auth.py",
    }


def load_inputs(root: Path) -> dict[str, str]:
    """Read all governed sources, treating the removed restart handler as empty."""
    paths = _governed_source_paths()
    result = {key: (root / value).read_text(encoding="utf-8") for key, value in paths.items()}
    result["dev_transaction"] = result["dev_main"]
    _header, separator, image_lock_tasks = result["dev_image_lock"].partition("---\n")
    include = (
        "- name: Converge the managed devcontainer image lock authority\n"
        "  ansible.builtin.include_tasks: image_lock.yml\n"
    )
    if separator:
        result["dev_main"] = result["dev_main"].replace(include, image_lock_tasks)
    hil_shells = {
        path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
        for path in sorted((root / "scripts/hil").rglob("*.sh"))
        if path.is_file()
    }
    monitor = root / "scripts/ci/monitor.sh"
    hil_shells[monitor.relative_to(root).as_posix()] = monitor.read_text(encoding="utf-8")
    result["hil_shells"] = json.dumps(hil_shells, sort_keys=True)
    handler = root / DEV_HANDLER
    result["dev_handler"] = handler.read_text(encoding="utf-8") if handler.exists() else ""
    return result


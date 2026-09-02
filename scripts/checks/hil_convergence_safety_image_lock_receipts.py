# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Executable-order policy for the devcontainer image-lock selftest receipts."""

from __future__ import annotations

import re

PROCESS_SOURCE_SPECS = (
    (
        "suite-root nested allocation selection removed",
        "devcontainer_image_selftest",
        "begin_selftest_tmp",
        '  if [[ -n "$SELFTEST_SUITE_ROOT" ]]; then\n    selftest_suite_root_is_safe || return 1',
    ),
    (
        "shared worker process-group binding removed",
        "devcontainer_image_lock_selftest",
        "record_worker_group",
        '    [[ "$pgid" == "$PPID" && "$pgid" != "$pid" ]] ||',
    ),
    (
        "isolated worker process-group binding removed",
        "devcontainer_image_lock_selftest",
        "record_worker_group",
        '    [[ "$pgid" == "$pid" ]] || die "selftest isolated worker is not its group leader"',
    ),
    (
        "suite anchor canonical path binding removed",
        "devcontainer_image_selftest",
        "selftest_suite_anchor_is_safe",
        '    "$anchor" == "$canonical/ra8-devcontainer-image-selftest.$suffix" &&',
    ),
    (
        "suite anchor identity binding removed",
        "devcontainer_image_selftest",
        "selftest_suite_anchor_is_safe",
        '    "$(file_identity "$anchor")" == "$SELFTEST_SUITE_ANCHOR_IDENTITY" &&',
    ),
    (
        "suite anchor owner binding removed",
        "devcontainer_image_selftest",
        "selftest_suite_anchor_is_safe",
        '    "$(file_owner_id "$anchor")" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &&',
    ),
    (
        "suite anchor mode binding removed",
        "devcontainer_image_selftest",
        "selftest_suite_anchor_is_safe",
        '    "$(file_mode "$anchor")" == "700" ]]',
    ),
    (
        "suite anchor child depth binding removed",
        "devcontainer_image_selftest",
        "selftest_suite_path_is_safe",
        '  [[ "$suite" == "$SELFTEST_SUITE_ANCHOR/ra8-devcontainer-image-selftest.$suffix" &&',
    ),
    (
        "suite anchor producer path removed",
        "devcontainer_image_selftest",
        "establish_selftest_suite_root",
        '  SELFTEST_SUITE_ANCHOR="$SELFTEST_TMP_DIR"',
    ),
    (
        "suite anchor producer identity removed",
        "devcontainer_image_selftest",
        "establish_selftest_suite_root",
        '  SELFTEST_SUITE_ANCHOR_IDENTITY="$SELFTEST_TMP_IDENTITY"',
    ),
    (
        "suite anchor producer owner removed",
        "devcontainer_image_selftest",
        "establish_selftest_suite_root",
        '  SELFTEST_SUITE_ANCHOR_OWNER_UID="$SELFTEST_TMP_OWNER_UID"',
    ),
    (
        "suite anchor receiver path removed",
        "devcontainer_image_selftest",
        "configure_selftest_suite_authority",
        '  SELFTEST_SUITE_ANCHOR="$3"',
    ),
    (
        "suite anchor receiver identity removed",
        "devcontainer_image_selftest",
        "configure_selftest_suite_authority",
        '  SELFTEST_SUITE_ANCHOR_IDENTITY="$4"',
    ),
    (
        "suite anchor receiver owner removed",
        "devcontainer_image_selftest",
        "configure_selftest_suite_authority",
        '  SELFTEST_SUITE_ANCHOR_OWNER_UID="$5"',
    ),
    (
        "suite anchor dispatcher propagation removed",
        "devcontainer_image_selftest_cases",
        "selftest_one_allocation_signal",
        '    "$tmp" "$SELFTEST_TMP_IDENTITY" "$SELFTEST_SUITE_ANCHOR" \\\n'
        '    "$SELFTEST_SUITE_ANCHOR_IDENTITY" "$SELFTEST_SUITE_ANCHOR_OWNER_UID"; then',
    ),
)
LEGACY_PROCESS_FINDINGS = {
    "image lock jobs-table lookup bypassed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        "done < <(jobs -r -l)",
    ),
    "image lock verified PID signal reverted to stale jobspec": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'builtin kill -"$signal" "$child"',
    ),
    "allocation KILL direct-child guard removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'signal_owned_live_child KILL "$child" ||',
    ),
    "suite-root parent grammar reverted to ten characters": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        '"$suffix" =~ ^[0-9a-f]{32}$ && ! -L "$canonical"',
    ),
    "portable Bash 3 shell identity replaced by BASHPID": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'local destination="$1" value="$$:${BASH_SUBSHELL:-0}"',
        "devcontainer image selftest: macOS Bash 3.2 BASHPID dependency returned",
    ),
    "fresh allocation signal process bypassed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        '--selftest-allocation-checkpoint-child "$phase" "$receipt"',
    ),
    "selftest atomic directory allocation replaced by mktemp create": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'if (umask 077 && mkdir -m 0700 -- "$candidate"); then',
        "devcontainer image lifecycle selftest: allocation-before-binding returned",
    ),
    "selftest suite-root binding removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'establish_selftest_suite_root || die "selftest: could not bind its suite root"',
    ),
    "selftest suite-root completion proof removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'clear_selftest_suite_root || die "selftest: suite-root cleanup did not complete"',
    ),
    "worker group direct-child authority removed": (
        "dev box image lock: bounded selftest harness is not load-bearing",
    ),
    "worker leader TERM resistance removed": (
        "devcontainer image-lock selftest: required process-authority token is not unique: "
        "pre-ready-hang | signal-controller | post-ready-build-hang) trap '' HUP INT TERM ;;",
    ),
    "process-enumeration failure scenario removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'selftest_ps_failure_cleanup "$tmp"',
    ),
    "rebound process-group refusal removed": (
        "devcontainer image-lock selftest: required process-authority token is not unique: "
        "rebound numeric process group gained signal authority",
    ),
    "process-enumeration descendant proof removed": (
        "devcontainer image-lock selftest: required process-authority token is not unique: "
        "repeated ps failure left a signal-ignoring descendant",
    ),
}


def _process_source_finding(key: str, owner: str, token: str) -> str:
    """Return one exact Bash source-policy diagnostic."""
    return f"devcontainer image source policy: {key}:{owner} token is not unique: {token}"


def semantic_process_findings(label: str) -> tuple[str, ...] | None:
    """Return one exact legacy Bash process-authority finding set."""
    if (findings := LEGACY_PROCESS_FINDINGS.get(label)) is not None:
        return findings
    for candidate, key, owner, token in PROCESS_SOURCE_SPECS:
        if label == candidate:
            return (_process_source_finding(key, owner, token),)
    return None


def process_source_errors(inputs: dict[str, str]) -> list[str]:
    """Bind nested-root selection and shared/isolated group identity by owner."""
    errors = []
    for _label, key, owner, token in PROCESS_SOURCE_SPECS:
        body = _body(inputs[key], owner, 0)
        if body is None or body.count(token) != 1:
            errors.append(_process_source_finding(key, owner, token))
    return errors


def _body(source: str, name: str, indent: int) -> str | None:
    """Return one Bash function body whose closing brace shares its indentation."""
    prefix = " " * indent
    opening = f"{prefix}{name}() {{\n"
    if source.count(opening) != 1:
        return None
    start = source.find(opening)
    if start < 0:
        return None
    body_start = start + len(opening)
    match = re.search(rf"(?m)^{re.escape(prefix)}}}\s*$", source[body_start:])
    if match is None:
        return None
    return source[body_start : body_start + match.start()]


def _ordered(body: str, anchors: tuple[str, ...]) -> bool:
    """Require active anchors exactly once and in strictly increasing order."""
    active = "\n".join(line for line in body.splitlines() if not line.lstrip().startswith("#"))
    position = -1
    for anchor in anchors:
        if active.count(anchor) != 1:
            return False
        position = active.find(anchor, position + 1)
        if position < 0:
            return False
    return True


def _early_success(body: str, indent: int) -> bool:
    """Detect a top-level success return injected before completion accounting."""
    prefix = " " * (indent + 2)
    return any(
        line in (f"{prefix}return", f"{prefix}return 0", f"{prefix}exit 0")
        for line in body.splitlines()
    )


def _function_errors(source: str, names: tuple[str, ...], indent: int) -> list[str]:
    """Require functions to exist and refuse top-level early-success exits."""
    errors = []
    for name in names:
        body = _body(source, name, indent)
        if body is None:
            errors.append(f"image lock receipt: missing {name}")
        elif _early_success(body, indent):
            errors.append(f"image lock receipt: {name} can return success before its proof")
    return errors


def _order_error(source: str, name: str, indent: int, anchors: tuple[str, ...]) -> str | None:
    """Return one finding when a function's executable proof order drifts."""
    body = _body(source, name, indent)
    if body is None or not _ordered(body, anchors):
        return f"image lock receipt: {name} proof order drifted"
    return None


def _main_order_errors(image_source: str, cases_source: str) -> list[str]:
    """Bind the lock, command, and main completion paths in executable order."""
    build_anchors = (
        'exec 9<"$IMAGE_LOCK_FILE"',
        "validate_opened_image_lock 9\n      if ! flock -n 9; then",
        "if ! flock -n 9; then",
        "fi\n      validate_opened_image_lock 9",
        'build_image "$want"\n    )',
    )
    command_anchors = (
        'SELFTEST_RECEIPT_DIR="$tmp/image-lock-selftest-receipts"',
        'mkdir -m 0700 "$SELFTEST_RECEIPT_DIR"',
        'dispatch_image_lock_selftest suite "$tmp"',
        '[[ "$SELFTEST_DISPATCH_COMPLETE" == "1" ]]',
        "SELFTEST_FILE_RECEIPTS_VERIFIED=0",
        "verify_scenario_receipt_files early-exit pre-ready-hang forced-build-contention",
        '[[ "$SELFTEST_FILE_RECEIPTS_VERIFIED" == "1" ]]',
        'selftest_runtime_labels "$base"',
        "SELFTEST_COMMAND_COMPLETE=1",
    )
    errors = []
    for source, name, indent, anchors in (
        (image_source, "build_locked", 2, build_anchors),
        (cases_source, "cmd_selftest", 0, command_anchors),
    ):
        error = _order_error(source, name, indent, anchors)
        if error:
            errors.append(error)
    main_order = (
        "SELFTEST_COMMAND_COMPLETE=0",
        "cmd_selftest",
        '[[ "$SELFTEST_COMMAND_COMPLETE" == "1" ]]',
        "SELFTEST_MAIN_COMPLETE=1",
    )
    main_body = _body(image_source, "main", 2)
    if main_body is None or not _ordered(main_body, main_order):
        errors.append("image lock receipt: main selftest completion check drifted")
    top_order = (
        "SELFTEST_MAIN_COMPLETE=0",
        'main "$@"',
        '[[ "$SELFTEST_MAIN_COMPLETE" == "1" ]] || die "selftest main returned before completion"',
    )
    if not _ordered(image_source, top_order):
        errors.append("image lock receipt: top-level main completion check drifted")
    return errors


def _worker_cleanup_checks() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Return lock release and direct worker cleanup proof order."""
    return (
        (
            "release_parent_lock",
            (
                "flock -u 8 || release_failed=1",
                "if exec 8>&-; then",
                "SELFTEST_PARENT_LOCK_OPEN=0",
                'return "$release_failed"',
            ),
        ),
        (
            "cleanup_image_lock_case",
            (
                'SELFTEST_CLEANUP_RECEIPT=""',
                'kill -TERM -- "-$SELFTEST_WORKER_PGID"',
                "release_parent_lock",
                "kill -KILL",
                "reap_worker",
                'bounded_process_absent "$SELFTEST_WORKER_PID"',
                'bounded_group_gone "$SELFTEST_WORKER_PGID"',
                "assert_no_surviving_descendants",
                "parent_lock_fd_is_closed",
                "fresh_lock_probe",
                'SELFTEST_CLEANUP_RECEIPT="$(expected_cleanup_receipt)"',
                'return "$cleanup_failed"',
            ),
        ),
    )


def _controller_cleanup_checks() -> tuple[tuple[str, tuple[str, ...]], ...]:
    """Return forced and signal-controller cleanup proof order."""
    return (
        (
            "force_signal_controller_cleanup",
            (
                'SELFTEST_FORCE_CLEANUP_RECEIPT=""',
                'signal_owned_controller_group TERM "$controller"',
                'bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS"',
                "verify_signal_controller_cleanup",
                'SELFTEST_FORCE_CLEANUP_RECEIPT="$(expected_force_cleanup_receipt "$case_dir")"',
            ),
        ),
        (
            "verify_signal_controller_cleanup",
            (
                "reap_controller",
                'bounded_process_absent "$controller"',
                "read_worker_group_from_case",
                'bounded_process_absent "$SELFTEST_WORKER_PID"',
                'bounded_group_gone "$SELFTEST_WORKER_PGID"',
                "assert_no_surviving_descendants",
                "parent_lock_fd_is_closed",
                "fresh_lock_probe",
                "write_controller_cleanup_receipt_file",
                "require_controller_cleanup_receipt_file",
            ),
        ),
        (
            "image_lock_case_signal",
            (
                "cleanup_image_lock_case",
                "require_cleanup_receipt",
                "write_worker_cleanup_proof_file",
                'exit "$status"',
            ),
        ),
    )


def _cleanup_order_errors(source: str) -> list[str]:
    """Bind cleanup receipts after every termination and lock-release fact."""
    errors = []
    checks = (*_worker_cleanup_checks(), *_controller_cleanup_checks())
    for name, anchors in checks:
        error = _order_error(source, name, 0, anchors)
        if error:
            errors.append(error)
    return errors


def _simple_worker_scenario_checks() -> tuple[tuple[str, tuple[str, ...], str], ...]:
    """Return complete semantic tails for exit and pre-ready scenarios."""
    pre_ready_negative = (
        'if wait_for_status_file "$SELFTEST_CASE_DIR/ready.status" '
        '"$SELFTEST_WORKER_PID"; then\n'
        '    die "selftest: pre-ready hang unexpectedly became ready"\n'
        "  fi"
    )
    return (
        (
            "selftest_early_exit",
            (
                "start_image_lock_worker early-exit",
                "reap_worker",
                '[[ "$SELFTEST_REAP_STATUS" == "23" ]]',
                "cleanup_image_lock_case",
                "require_cleanup_receipt",
                "clear_image_lock_case_traps",
            ),
            "early-exit",
        ),
        (
            "selftest_pre_ready_hang",
            (
                "start_image_lock_worker pre-ready-hang",
                pre_ready_negative,
                "cleanup_image_lock_case",
                "require_cleanup_receipt",
                "clear_image_lock_case_traps",
            ),
            "pre-ready-hang",
        ),
    )


def _contention_scenario_checks() -> tuple[tuple[str, tuple[str, ...], str], ...]:
    """Return complete semantic tails for lock-contention scenarios."""
    post_ready_negative = (
        'if wait_for_status_file "$SELFTEST_CASE_DIR/done.status" '
        '"$SELFTEST_WORKER_PID"; then\n'
        '    die "selftest: build-hang worker unexpectedly completed"\n'
        "  fi"
    )
    contention_loop = (
        "for ((attempt = 0; attempt < 20; ++attempt)); do\n"
        '    [[ ! -e "$SELFTEST_CASE_DIR/build-entered.status" ]] ||\n'
        '      die "selftest: forced rebuild bypassed the held lock"\n'
        '    process_is_terminal "$SELFTEST_WORKER_PID" &&\n'
        '      die "selftest: forced rebuild exited while the lock was held"\n'
        "    sleep 0.01\n"
        "  done"
    )
    return (
        (
            "selftest_forced_build_contention",
            (
                "hold_parent_lock",
                "if fresh_lock_probe; then",
                "start_image_lock_worker normal",
                'wait_for_status_file "$SELFTEST_CASE_DIR/ready.status"',
                contention_loop,
                "release_parent_lock",
                'wait_for_status_file "$SELFTEST_CASE_DIR/done.status"',
                "reap_worker",
                '[[ "$SELFTEST_REAP_STATUS" == "0" ]]',
                "fresh_lock_probe || die "
                '"selftest: fresh lock probe failed after worker completion"',
                "cleanup_image_lock_case",
                "require_cleanup_receipt",
                "clear_image_lock_case_traps",
            ),
            "forced-build-contention",
        ),
        (
            "selftest_post_ready_hang",
            (
                "start_image_lock_worker post-ready-build-hang",
                'wait_for_status_file "$SELFTEST_CASE_DIR/ready.status"',
                "release_parent_lock",
                'wait_for_status_file "$SELFTEST_CASE_DIR/build-entered.status"',
                post_ready_negative,
                "cleanup_image_lock_case",
                "require_cleanup_receipt",
                "clear_image_lock_case_traps",
            ),
            "post-ready-hang",
        ),
    )


def _signal_scenario_checks() -> tuple[tuple[str, tuple[str, ...], str], ...]:
    """Return complete semantic tails for the two signal scenarios."""
    ready_negative = (
        'if wait_for_status_file "$case_dir/controller-ready.status" "$controller"; then\n'
        '    die "selftest: delayed signal controller unexpectedly became ready"\n'
        "  fi"
    )
    return (
        (
            "selftest_signal_ready_timeout",
            (
                'start_signal_controller controller "$managed" "$case_dir" delay-controller-ready',
                ready_negative,
                "force_signal_controller_cleanup",
                "require_force_cleanup_receipt",
            ),
            "signal-ready-timeout",
        ),
        (
            "selftest_signal_cleanup",
            (
                "for signal in HUP INT TERM; do",
                'case "$signal" in HUP) expected=129 ;; INT) expected=130 ;; '
                "TERM) expected=143 ;; esac",
                'wait_for_status_file "$case_dir/controller-ready.status" "$controller"',
                'signal_owned_controller_group "$signal" "$controller"',
                'bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS"',
                "verify_signal_controller_cleanup",
                '[[ "$SELFTEST_REAP_STATUS" == "$expected" ]]',
                "require_worker_cleanup_proof_file",
                "require_controller_cleanup_receipt_file",
                "  done",
            ),
            "signal-cleanup",
        ),
    )


def _scenario_order_errors(lock_source: str, signal_source: str) -> list[str]:
    """Require each complete scenario proof before its final unique receipt."""
    errors = []
    checks = (
        *_simple_worker_scenario_checks(),
        *_contention_scenario_checks(),
        *_signal_scenario_checks(),
    )
    for function, semantics, receipt in checks:
        source = signal_source if function.startswith("selftest_signal_") else lock_source
        body = _body(source, function, 0)
        final = f"write_scenario_receipt {receipt}"
        if body is None or not _ordered(body, (*semantics, final)):
            errors.append(f"image lock receipt: {function} completion order drifted")
            continue
        executable = [
            line.strip()
            for line in body.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        if not executable or executable[-1] != final:
            errors.append(f"image lock receipt: {function} receipt is not its final action")
    return errors


def _dispatch_order_errors(lock_source: str, receipt_source: str, cases_source: str) -> list[str]:
    """Bind exact scenario identities, count, file proof, and dispatcher latch."""
    calls = (
        'run_image_lock_scenario early-exit selftest_early_exit "$tmp"',
        'run_image_lock_scenario pre-ready-hang selftest_pre_ready_hang "$tmp"',
        'run_image_lock_scenario forced-build-contention selftest_forced_build_contention "$tmp"',
        'run_image_lock_scenario post-ready-hang selftest_post_ready_hang "$tmp"',
        'run_image_lock_scenario signal-ready-timeout selftest_signal_ready_timeout "$tmp"',
        'run_image_lock_scenario signal-cleanup selftest_signal_cleanup "$tmp"',
    )
    dispatcher = _body(cases_source, "run_managed_image_lock_suite", 0)
    runner = _body(lock_source, "run_image_lock_scenario", 0)
    verifier = _body(receipt_source, "verify_scenario_receipt_files", 0)
    errors = []
    if dispatcher is None or not _ordered(
        dispatcher, (*calls, "verify_image_lock_suite_receipts", "SELFTEST_DISPATCH_COMPLETE=1")
    ):
        errors.append("image lock receipt: dispatcher scenario set or latch drifted")
    runner_order = (
        '"$scenario" "$tmp"',
        "require_scenario_receipt",
        "SELFTEST_SUITE_RECEIPTS+=",
        "SELFTEST_SUITE_COUNT",
    )
    if runner is None or not _ordered(runner, runner_order):
        errors.append("image lock receipt: scenario runner accounting drifted")
    verifier_order = (
        "SELFTEST_FILE_RECEIPTS_VERIFIED=0",
        '[[ "$#" == "6" ]]',
        "require_scenario_receipt",
        "find ",
        '[[ "$count" == "6" ]]',
        "SELFTEST_FILE_RECEIPTS_VERIFIED=1",
    )
    if verifier is None or not _ordered(verifier, verifier_order):
        errors.append("image lock receipt: independent receipt verifier drifted")
    return errors


def errors(
    image_source: str,
    receipt_source: str,
    selftest_source: str,
    cases_source: str,
    signal_source: str,
) -> list[str]:
    """Return completion-accounting findings for the image-lock implementation."""
    main_names = ("build_locked", "main")
    receipt_helper_names = (
        "expected_image_lock_suite_receipts",
        "scenario_receipt_value",
        "validate_scenario_receipt_directory",
        "write_scenario_receipt",
        "require_scenario_receipt",
        "verify_scenario_receipt_files",
        "expected_cleanup_receipt",
        "expected_force_cleanup_receipt",
        "parent_lock_fd_is_closed",
        "require_cleanup_receipt",
        "require_force_cleanup_receipt",
        "write_worker_cleanup_proof_file",
        "require_worker_cleanup_proof_file",
        "write_controller_cleanup_receipt_file",
        "require_controller_cleanup_receipt_file",
    )
    lock_helper_names = (
        "reap_worker",
        "reap_controller",
        "release_parent_lock",
        "fresh_lock_probe",
        "assert_no_surviving_descendants",
        "cleanup_image_lock_case",
        "force_signal_controller_cleanup",
        "verify_signal_controller_cleanup",
        "verify_image_lock_suite_receipts",
        "run_image_lock_scenario",
        "dispatch_image_lock_selftest",
        "selftest_early_exit",
        "selftest_pre_ready_hang",
        "selftest_forced_build_contention",
        "selftest_post_ready_hang",
    )
    signal_helper_names = (
        "selftest_signal_ready_timeout",
        "selftest_signal_cleanup",
    )
    return (
        _function_errors(image_source, main_names, 2)
        + _function_errors(cases_source, ("cmd_selftest",), 0)
        + _function_errors(receipt_source, receipt_helper_names, 0)
        + _function_errors(selftest_source, lock_helper_names, 0)
        + _function_errors(cases_source, ("run_managed_image_lock_suite",), 0)
        + _function_errors(signal_source, signal_helper_names, 0)
        + _main_order_errors(image_source, cases_source)
        + _cleanup_order_errors(selftest_source)
        + _scenario_order_errors(selftest_source, signal_source)
        + _dispatch_order_errors(selftest_source, receipt_source, cases_source)
    )

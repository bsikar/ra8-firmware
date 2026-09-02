# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Cross-language source policy for the devcontainer image selftest harness."""

from __future__ import annotations

import hil_convergence_safety_image_process_analysis as image_process_analysis

_IMAGE_PROCESS_ANALYSIS_MODULE = "hil_convergence_safety_image_process_analysis"
_IMAGE_PROCESS_ANALYSIS_IMPORT = (
    "import " + _IMAGE_PROCESS_ANALYSIS_MODULE + " as image_process_analysis"
)
ModuleSpec = tuple[str, str, tuple[str, ...]]


def _exact_shell_token_errors(source: str, label: str, tokens: tuple[str, ...]) -> list[str]:
    """Require each security-sensitive shell token exactly once."""
    return [
        f"{label}: required process-authority token is not unique: {token}"
        for token in tokens
        if source.count(token) != 1
    ]


def _image_lifecycle_process_errors(lifecycle: str, bound_exit: str) -> list[str]:
    """Require exact lifecycle allocation and child-process authorities."""
    return _exact_shell_token_errors(
        f"{lifecycle}\n{bound_exit}",
        "devcontainer image lifecycle selftest",
        (
            "live_child_jobspec() {",
            "done < <(jobs -r -l)",
            'builtin kill -"$signal" "$child"',
            "selftest_suite_root_is_safe() {",
            "selftest_suite_anchor_is_safe() {",
            "selftest_suite_path_is_safe() {",
            "configure_selftest_suite_authority() {",
            "configure_bound_exit_nested_suite() {",
            "publish_bound_exit_nested_root() {",
            "establish_selftest_suite_root() {",
            "set_selftest_shell_identity() {",
            'local destination="$1" value="$$:${BASH_SUBSHELL:-0}"',
            "SELFTEST_DEADLINE_STEPS=200",
            ': "$SELFTEST_DEADLINE_STEPS"',
            "od -An -N16 -tx1 /dev/urandom",
            'if (umask 077 && mkdir -m 0700 -- "$candidate"); then',
            '"$suffix" =~ ^[0-9a-f]{32}$ && ! -L "$canonical"',
            '"$(file_identity "$anchor")" == "$SELFTEST_SUITE_ANCHOR_IDENTITY"',
            '"$SELFTEST_SUITE_ROOT_OWNER_UID" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID"',
            "run_bound_exit_supervisor() {",
            'RA8_SELFTEST_BOUND_ENTRY="$1"',
            '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_HELPER_PARENT"',
            '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
            '"$SELFTEST_HELPER_PARENT_DIR/devcontainer_image.sh"',
            "selftest_descriptor_bound_entry() {",
            'RA8_SELFTEST_BOUND_ENTRY="$wrong_entry"',
            'RA8_SELFTEST_BOUND_ENTRY="$SCRIPT_DIR/devcontainer_image.sh"',
            'select_selftest_group_id "$tmp" "$rejected" >/dev/null 2>&1 && return 1\n'
            "  done\n  return 0",
            "write_supervisor_stall_fixture() {",
            '    "exec -a \\"\\$0\\" /bin/sleep 30" >"$destination") || return 1',
            "selftest_bound_exit_supervisor_failures() {",
            'run_bound_exit_supervisor --selftest-parent-death "$stall_entry" "$tmp"',
            'run_bound_exit_supervisor --selftest-watchdog-expiry "$stall_entry" "$tmp"',
            'run_bound_exit_supervisor --selftest-closed-death-fd "$stall_entry" "$tmp"',
            'run_bound_exit_supervisor --selftest-closed-entry-fd "$stall_entry" "$tmp"',
            'run_bound_exit_supervisor --selftest-hardlink-bound "$stall_entry" "$tmp"',
            'RA8_SELFTEST_NESTED_PARENT_IDENTITY="$SELFTEST_TMP_IDENTITY"',
            'RA8_SELFTEST_NESTED_ROOT_RECEIPT="$nested_receipt"',
            'run_bound_exit_supervisor "$stall_entry" "$bound" "$outer"',
            'run_bound_exit_supervisor --selftest-missing-entry "$tmp"',
            'run_bound_exit_supervisor --selftest-entry-binding "$tmp"',
        ),
    )


def _image_cases_process_errors(cases: str) -> list[str]:
    """Require exact case-controller authorities."""
    errors = _exact_shell_token_errors(
        cases,
        "devcontainer image cases selftest",
        (
            "configure_bound_exit_nested_suite || die",
            "publish_bound_exit_nested_root || die",
            'selftest_pre_isolation_exit_refusal "$tmp" ||',
            'signal_owned_live_child KILL "$child" ||',
            "stale allocation child PID retained signal authority",
            'establish_selftest_suite_root || die "selftest: could not bind its suite root"',
            'clear_selftest_suite_root || die "selftest: suite-root cleanup did not complete"',
            '--selftest-allocation-checkpoint-child "$phase" "$receipt"',
            '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_CASES_PARENT"',
            '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
            '"$SELFTEST_CASES_PARENT_DIR/devcontainer_image.sh"',
            "selftest_temp_root_is_safe() {",
            '"$SELFTEST_TMP_ROOT" == "$(cd -P /tmp && pwd)" &&',
            '"$(file_special_mode "$SELFTEST_TMP_ROOT")" == "1777" ]]',
            '"$SELFTEST_TMP_ROOT" == "$SELFTEST_SUITE_ROOT" &&',
            (
                '"$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] &&\n'
                "    selftest_suite_root_is_safe"
            ),
            'selftest_temp_root_is_safe || die "selftest: allocation parent authority is unsafe"',
            'selftest_descriptor_bound_entry "$tmp" ||',
            'selftest_controller_persisted_ps_failure "$tmp"',
        ),
    )
    errors += _exact_shell_token_errors(
        cases, "devcontainer image cases selftest", ('selftest_ps_failure_cleanup "$tmp"',)
    )
    return errors


def _image_signal_process_errors(signal_source: str) -> list[str]:
    """Require exact isolated signal-controller authorities."""
    return _exact_shell_token_errors(
        signal_source,
        "devcontainer image signal selftest",
        (
            'local launcher_mode="${5:-}" controller_pid pending '
            'ready="$3/controller-launcher.ready"',
            '--selftest-case-signal-child "$signal" "$tmp" "$SELFTEST_TMP_IDENTITY"',
            'signal_owned_live_child TERM "$controller_pid" || controller_bound_spawn_signal 1',
            "selftest_pre_isolation_exit_refusal() {",
            '! signal_owned_live_child TERM "$child" || controller_bound_spawn_signal 1',
            "signal_owned_controller_group() {",
            'controller_group_signal_is_authorized "$controller" || return 1',
            'builtin kill -"$signal" -- "-$controller"',
            'signal_owned_controller_group "$signal" "$controller" ||',
            'signal_owned_controller_group TERM "$controller" ||',
            '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_SIGNAL_PARENT"',
            '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
            '"$SELFTEST_SIGNAL_PARENT_DIR/devcontainer_image.sh"',
            'kill -KILL -- "-$controller" 2>/dev/null || bounded_group_gone "$controller"',
        ),
    )


def _image_lock_process_errors(
    lifecycle: str, cases: str, signal_source: str, lock: str
) -> list[str]:
    """Reject unsafe lock-helper PID and portability fallbacks."""
    errors = _exact_shell_token_errors(
        lock,
        "devcontainer image-lock selftest",
        (
            "worker_group_signal_is_authorized() {",
            "image_lock_ps_group_snapshot() {",
            'signal_owned_controller_group TERM "$controller"',
            "SELFTEST_CONTROLLER_CLEANUP_STEPS=1600",
            'wait_for_worker_ack "$case_dir" "$$" || exit 124',
            'if wait "$SELFTEST_BOUND_DIRECT_CHILD"; then child_status=0; else child_status=$?; fi',
            "rebound numeric process group gained signal authority",
            "pre-ready-hang | signal-controller | post-ready-build-hang) trap '' HUP INT TERM ;;",
            "repeated ps failure left a signal-ignoring descendant",
            '"${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_LOCK_HELPER_PARENT"',
            '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
            '"$SELFTEST_LOCK_HELPER_PARENT_DIR/devcontainer_image.sh"',
        ),
    )
    forbidden = (
        'kill -TERM "$controller"',
        'kill -KILL "$controller"',
        'signal_owned_live_child KILL "$controller"',
        'signal_owned_live_child TERM "$controller"',
        'kill -TERM "$SELFTEST_WORKER_PID"',
    )
    if any(token in lock for token in forbidden):
        errors.append("devcontainer image-lock selftest: bare PID signal fallback returned")
    if 'mktemp -d "$SELFTEST_TMP_ROOT/' in lifecycle:
        errors.append("devcontainer image lifecycle selftest: allocation-before-binding returned")
    if any("BASHPID" in source for source in (lifecycle, cases, signal_source, lock)):
        errors.append("devcontainer image selftest: macOS Bash 3.2 BASHPID dependency returned")
    return errors


def _image_supervisor_token_errors(
    supervisor: str, cases_source: str, process_source: str
) -> list[str]:
    """Delegate Python process-authority checking to its focused module."""
    return image_process_analysis.supervisor_errors(supervisor, cases_source, process_source)


def _semantic_aggregator_errors(source: str) -> list[str]:
    """Require every split runtime selftest to remain imported and dispatched."""
    return _exact_shell_token_errors(
        source,
        "hil convergence semantic selftest",
        (
            "import hil_convergence_safety_process_mutations as process_mutations",
            _IMAGE_PROCESS_ANALYSIS_IMPORT,
            "import hil_convergence_safety_raw_digest_runtime as raw_digest_runtime",
            "import hil_convergence_safety_runtime_cleanup as runtime_cleanup",
            "import hil_convergence_safety_runtime_escape as runtime_escape",
            "import hil_convergence_safety_runtime_mutations as runtime_mutations",
            "process_mutations.process_authority_mutations()",
            "image_process_analysis.semantic_process_findings(label)",
            "raw_digest_runtime.cases(inputs)",
            "runtime_cleanup.cases(inputs)",
            "runtime_escape.cases(inputs)",
            "runtime_mutations.runtime_cases(inputs)",
        ),
    )


def _runtime_module_specs(inputs: dict[str, str]) -> tuple[ModuleSpec, ...]:
    """Return runtime cleanup, loader, mutation, and escape bindings."""
    return (
        (
            "runtime cleanup",
            inputs["runtime_cleanup"],
            (
                "import hil_convergence_safety_runtime_loader as runtime_loader",
                "runtime_loader.cases(inputs)",
            ),
        ),
        (
            "runtime loader",
            inputs["runtime_loader"],
            (
                "import hil_convergence_safety_runtime_loader_harness as loader_harness",
                "loader_harness.run(",
            ),
        ),
        (
            "runtime mutations",
            inputs["runtime_mutations"],
            (
                "import hil_convergence_safety_runtime_launcher as runtime_launcher",
                "import hil_convergence_safety_runtime_sources as runtime_sources",
                "import hil_convergence_safety_runtime_root_swap as runtime_root_swap",
                "_write_sources = runtime_sources.publish",
                "runtime_launcher.launch(",
            ),
        ),
        (
            "runtime escape",
            inputs["runtime_escape"],
            ("from hil_convergence_safety_runtime_mutations import (",),
        ),
    )


def _image_module_specs(inputs: dict[str, str]) -> tuple[ModuleSpec, ...]:
    """Return image analysis, harness, and mutation-catalog bindings."""
    return (
        (
            "image process analysis",
            inputs["image_process_analysis"],
            (
                "import hil_convergence_safety_image_process_policy as catalog",
                "import hil_convergence_safety_image_subreaper_policy as subreaper_policy",
                "CROSS_LANGUAGE_SCOPED_TOKENS = catalog.CROSS_LANGUAGE_SCOPED_TOKENS",
                "subreaper_policy.errors(supervisor, process_source)",
            ),
        ),
        (
            "image harness policy",
            inputs["image_harness_policy"],
            (
                _IMAGE_PROCESS_ANALYSIS_IMPORT,
                "image_process_analysis."
                "supervisor_errors("
                "supervisor, cases_source, process_source)",
                "image_process_analysis." + "source_errors(inputs)",
            ),
        ),
        (
            "process mutation catalog",
            inputs["process_mutations"],
            (
                "import hil_convergence_safety_process_source_fixtures as process_source_fixtures",
                "import hil_convergence_safety_runtime_fixtures as runtime_fixtures",
                "import hil_convergence_safety_source_fixtures as source_fixtures",
                "process_source_fixtures.process_authority_mutations()",
                "runtime_fixtures.process_authority_mutations()",
                "*source_fixtures.process_authority_mutations(),",
            ),
        ),
    )


def _image_dispatch_specs(inputs: dict[str, str]) -> tuple[ModuleSpec, ...]:
    """Return the public checker and authenticated shell-loader bindings."""
    return (
        (
            "image harness consumer",
            inputs["hil_convergence_entry"],
            (
                "import hil_convergence_safety_image_harness_policy as image_harness_policy",
                "image_harness_policy.errors(inputs)",
            ),
        ),
        (
            "bound-exit helper loader",
            inputs["devcontainer_image"],
            (
                "SELFTEST_BOUND_EXIT_RAW_SHA256=",
                'source_approved_selftest_helper "$SCRIPT_DIR/'
                'devcontainer_image_bound_exit_selftest.bash"',
                '"$SELFTEST_BOUND_EXIT_RAW_SHA256"',
            ),
        ),
    )


def _split_runtime_module_errors(inputs: dict[str, str]) -> list[str]:
    """Bind every focused split helper to one exact production consumer."""
    specifications = (
        *_runtime_module_specs(inputs),
        *_image_module_specs(inputs),
        *_image_dispatch_specs(inputs),
    )
    errors = []
    for label, source, tokens in specifications:
        errors.extend(_exact_shell_token_errors(source, label, tokens))
    return errors


def errors(inputs: dict[str, str]) -> list[str]:
    """Bind signal cleanup to live jobs and bounded process-enumeration failure."""
    lifecycle = inputs["devcontainer_image_selftest"]
    bound_exit = inputs["devcontainer_image_bound_exit_selftest"]
    cases = inputs["devcontainer_image_selftest_cases"]
    signal_source = inputs["devcontainer_image_signal_selftest"]
    lock = inputs["devcontainer_image_lock_selftest"]
    supervisor = inputs["devcontainer_image_selftest_supervisor"]
    supervisor_cases = inputs["devcontainer_image_selftest_supervisor_cases"]
    supervisor_process = inputs["devcontainer_image_selftest_process"]
    return (
        _image_lifecycle_process_errors(lifecycle, bound_exit)
        + _image_cases_process_errors(cases)
        + _image_signal_process_errors(signal_source)
        + _image_lock_process_errors(lifecycle, cases, signal_source, lock)
        + _image_supervisor_token_errors(supervisor, supervisor_cases, supervisor_process)
        + image_process_analysis.source_errors(inputs)
        + _semantic_aggregator_errors(inputs["semantic_mutations"])
        + _split_runtime_module_errors(inputs)
    )

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Source-only process-authority mutations for image-lock runtime checks."""

from __future__ import annotations

Mutation = tuple[str, str, str, str]

SUPERVISOR_BOUND_FINDING = "devcontainer image supervisor: bound cleanup is not load-bearing"
SUPERVISOR_ORDER_FINDING = "devcontainer image supervisor: cleanup proof order drifted"
SUPERVISOR_ORDER_ONLY_LABELS = frozenset(
    {
        "bound receipt hardlink refusal removed",
        "bound receipt owner binding removed",
        "bound receipt mode binding removed",
        "bound receipt truncation removed",
    }
)
SUPERVISOR_DOUBLE_FINDING_LABELS = frozenset(
    {
        "bound-exit parent-death polling removed",
        "bound-exit controller group cleanup reduced to controller PID",
        "controller close failure bypasses group KILL",
    }
)
SUPERVISOR_TOKEN_FINDINGS = {
    "controller root identity binding removed": (
        "devcontainer image supervisor: required process-authority token is not unique: "
        "identity == expected_identity"
    ),
    "controller root path-shape binding removed": (
        "devcontainer image supervisor: required process-authority token is not unique: "
        "_suite_root_path_is_safe(resolved, metadata)"
    ),
}
LIFECYCLE_SEMANTIC_FINDINGS = {
    "case signal dispatch image lock load removed": (
        "dev box image lock: fail-closed helper or selftest is not load-bearing"
    ),
    "main descriptor basename predicate removed": (
        "dev box image lock: fail-closed helper or selftest is not load-bearing"
    ),
    "main descriptor file and link predicate removed": (
        "dev box image lock: fail-closed helper or selftest is not load-bearing"
    ),
    "main descriptor canonical path predicate removed": (
        "dev box image lock: fail-closed helper or selftest is not load-bearing"
    ),
    "lifecycle helper canonical parent proof removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
        '"$SELFTEST_HELPER_PARENT_DIR/devcontainer_image.sh"'
    ),
    "lock helper canonical parent proof removed": (
        "devcontainer image-lock selftest: required process-authority token is not unique: "
        '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
        '"$SELFTEST_LOCK_HELPER_PARENT_DIR/devcontainer_image.sh"'
    ),
    "descriptor-bound wrong entry refusal removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'RA8_SELFTEST_BOUND_ENTRY="$wrong_entry"'
    ),
    "descriptor-bound canonical entry proof removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'RA8_SELFTEST_BOUND_ENTRY="$SCRIPT_DIR/devcontainer_image.sh"'
    ),
    "group selection explicit success removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'select_selftest_group_id "$tmp" "$rejected" >/dev/null 2>&1 && return 1\n'
        "  done\n  return 0"
    ),
    "parent-death watchdog selftest removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor --selftest-parent-death "$stall_entry" "$tmp"'
    ),
    "live-supervisor watchdog deadline selftest removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor --selftest-watchdog-expiry "$stall_entry" "$tmp"'
    ),
    "bound-exit supervisor failure cases removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        "selftest_bound_exit_supervisor_failures() {"
    ),
    "closed death descriptor selftest removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor --selftest-closed-death-fd "$stall_entry" "$tmp"'
    ),
    "hardlink publication group proof removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor --selftest-hardlink-bound "$stall_entry" "$tmp"'
    ),
    "missing payload entry proof removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor --selftest-missing-entry "$tmp"'
    ),
    "stall fixture descendant marker removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        '    "exec -a \\"\\$0\\" /bin/sleep 30" >"$destination") || return 1'
    ),
    "nested phase parent identity binding removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'RA8_SELFTEST_NESTED_PARENT_IDENTITY="$SELFTEST_TMP_IDENTITY"'
    ),
    "nested phase root receipt binding removed": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'RA8_SELFTEST_NESTED_ROOT_RECEIPT="$nested_receipt"'
    ),
    "supervisor failure payload regressed to full entry": (
        "devcontainer image lifecycle selftest: required process-authority token is not unique: "
        'run_bound_exit_supervisor "$stall_entry" "$bound" "$outer"'
    ),
}
SIGNAL_SEMANTIC_FINDINGS = {
    "signal helper canonical parent proof removed": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
        '"$SELFTEST_SIGNAL_PARENT_DIR/devcontainer_image.sh"',
    ),
    "signal controller PID local renamed to controller": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        'local launcher_mode="${5:-}" controller_pid pending '
        'ready="$3/controller-launcher.ready"',
    ),
    "image lock controller group authorization removed": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        'controller_group_signal_is_authorized "$controller" || return 1',
    ),
    "image lock controller group target reduced to PID": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        'builtin kill -"$signal" -- "-$controller"',
    ),
    "image lock controller group signal reduced to direct PID": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        'signal_owned_controller_group "$signal" "$controller" ||',
        "image lock receipt: selftest_signal_cleanup completion order drifted",
    ),
    "pre-isolation numeric PID regained signal authority": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        'signal_owned_live_child TERM "$controller_pid" || controller_bound_spawn_signal 1',
    ),
    "exited pre-isolation child refusal removed": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        '! signal_owned_live_child TERM "$child" || controller_bound_spawn_signal 1',
    ),
    "image lock selftest_signal_ready_timeout early success refused": (
        "image lock receipt: selftest_signal_ready_timeout can return success before its proof",
    ),
    "image lock selftest_signal_cleanup early success refused": (
        "dev box image lock: bounded selftest harness is not load-bearing",
        "image lock receipt: selftest_signal_cleanup can return success before its proof",
    ),
    "image lock signal-ready negative deadline proof removed": (
        "image lock receipt: selftest_signal_ready_timeout completion order drifted",
    ),
    "image lock signal expected status proof removed": (
        "image lock receipt: selftest_signal_cleanup completion order drifted",
    ),
    "image lock signal fallback removed": (
        "dev box image lock: bounded selftest harness is not load-bearing",
    ),
    "image lock unready-controller fallback removed": (
        "dev box image lock: bounded selftest harness is not load-bearing",
    ),
    "fresh case signal process bypassed": (
        "devcontainer image signal selftest: required process-authority token is not unique: "
        '--selftest-case-signal-child "$signal" "$tmp" "$SELFTEST_TMP_IDENTITY"',
    ),
}
CASES_SEMANTIC_FINDINGS = {
    "allocation parent proof call removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'selftest_temp_root_is_safe || die "selftest: allocation parent authority is unsafe"',
    ),
    "canonical tmp special-mode proof removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        '"$(file_special_mode "$SELFTEST_TMP_ROOT")" == "1777" ]]',
    ),
    "nested suite-root safety proof removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        '"$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] &&\n'
        "    selftest_suite_root_is_safe",
    ),
    "cases helper canonical parent proof removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        '"${DEVCONTAINER_SELFTEST_PARENT:-}" == '
        '"$SELFTEST_CASES_PARENT_DIR/devcontainer_image.sh"',
    ),
    "exited pre-isolation child negative not dispatched": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        'selftest_pre_isolation_exit_refusal "$tmp" ||',
    ),
    "nested phase parent configuration removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        "configure_bound_exit_nested_suite || die",
    ),
    "nested phase root publication removed": (
        "devcontainer image cases selftest: required process-authority token is not unique: "
        "publish_bound_exit_nested_root || die",
    ),
}
SUPERVISOR_CASES_SEMANTIC_FINDINGS = {
    "cases root identity binding removed": (
        "devcontainer image supervisor cases: required process-authority token is not unique: "
        'f"{opened_identity[0]}:{opened_identity[1]}" != expected_identity',
    ),
    "supervisor cases source-only sentinel removed": (
        "devcontainer image supervisor cases: required process-authority token is not unique: "
        'globals().get("_RA8_SUPERVISOR_CASES_VERSION")',
    ),
    "observation failure injection removed": (
        "devcontainer image supervisor cases: required process-authority token is not unique: "
        "observation_injected = True\n            _inject_observation_failure()",
    ),
}


def semantic_findings(label: str, key: str) -> tuple[str, ...] | None:
    """Return exact diagnostics for source-only runtime mutation fixtures."""
    expected = None
    if key == "devcontainer_image_selftest_supervisor":
        token = SUPERVISOR_TOKEN_FINDINGS.get(label)
        if token is not None:
            expected = (token,)
        elif label in SUPERVISOR_ORDER_ONLY_LABELS:
            expected = (SUPERVISOR_ORDER_FINDING,)
        elif label in SUPERVISOR_DOUBLE_FINDING_LABELS:
            expected = (SUPERVISOR_BOUND_FINDING, SUPERVISOR_ORDER_FINDING)
        else:
            expected = (SUPERVISOR_BOUND_FINDING,)
    elif key == "devcontainer_image_signal_selftest":
        expected = SIGNAL_SEMANTIC_FINDINGS.get(label)
    elif key == "devcontainer_image_selftest_cases":
        expected = CASES_SEMANTIC_FINDINGS.get(label)
        if expected is None and label.startswith("managed image lock receipt "):
            expected = ("dev box image lock: fail-closed helper or selftest is not load-bearing",)
    elif key == "devcontainer_image_selftest_supervisor_cases":
        expected = SUPERVISOR_CASES_SEMANTIC_FINDINGS.get(label)
    else:
        finding = LIFECYCLE_SEMANTIC_FINDINGS.get(label)
        expected = None if finding is None else (finding,)
    return expected


def _legacy_process_authority_mutations() -> tuple[Mutation, ...]:
    """Return nested-root and fresh-process semantic mutations."""
    lifecycle = "devcontainer_image_selftest"
    cases = "devcontainer_image_selftest_cases"
    signal_helper = "devcontainer_image_signal_selftest"
    parent_identity = '      RA8_SELFTEST_NESTED_PARENT_IDENTITY="$SELFTEST_TMP_IDENTITY" \\\n'
    root_receipt = '      RA8_SELFTEST_NESTED_ROOT_RECEIPT="$nested_receipt" \\\n'
    supervisor_call = '    if run_bound_exit_supervisor "$stall_entry" "$bound" "$outer" \\\n'
    full_entry_call = supervisor_call.replace('"$stall_entry"', '"$SELFTEST_IMAGE_ENTRY"')
    configure = (
        '  configure_bound_exit_nested_suite || die "selftest: injected parent suite is unsafe"\n'
    )
    publish = (
        '    publish_bound_exit_nested_root || die "selftest: injected nested-root '
        'receipt failed"\n'
    )
    signal_owned = (
        '    signal_owned_live_child TERM "$controller_pid" || controller_bound_spawn_signal 1\n'
    )
    signal_numeric = '    builtin kill -TERM "$controller_pid" || controller_bound_spawn_signal 1\n'
    exited_refusal = (
        '  ! signal_owned_live_child TERM "$child" || controller_bound_spawn_signal 1\n'
    )
    exited_dispatch = '  selftest_pre_isolation_exit_refusal "$tmp" ||\n'
    return (
        ("nested phase parent identity binding removed", lifecycle, parent_identity, ""),
        ("nested phase root receipt binding removed", lifecycle, root_receipt, ""),
        (
            "supervisor failure payload regressed to full entry",
            lifecycle,
            supervisor_call,
            full_entry_call,
        ),
        ("nested phase parent configuration removed", cases, configure, ""),
        ("nested phase root publication removed", cases, publish, "    true\n"),
        (
            "pre-isolation numeric PID regained signal authority",
            signal_helper,
            signal_owned,
            signal_numeric,
        ),
        (
            "exited pre-isolation child refusal removed",
            signal_helper,
            exited_refusal,
            "  true\n",
        ),
        (
            "exited pre-isolation child negative not dispatched",
            cases,
            exited_dispatch,
            "  false ||\n",
        ),
    )


def _root_binding_mutations() -> tuple[Mutation, ...]:
    """Return exact controller, cases-root, and source-only mutations."""
    supervisor = "devcontainer_image_selftest_supervisor"
    cases = "devcontainer_image_selftest_supervisor_cases"
    identity_source = "identity == expected_identity"
    path_source = "_suite_root_path_is_safe(resolved, metadata)"
    cases_source = 'f"{opened_identity[0]}:{opened_identity[1]}" != expected_identity'
    sentinel_source = 'globals().get("_RA8_SUPERVISOR_CASES_VERSION")'
    observation_source = "observation_injected = True\n            _inject_observation_failure()"
    observation_replacement = "observation_injected = True\n            None"
    return (
        ("controller root identity binding removed", supervisor, identity_source, "True"),
        ("controller root path-shape binding removed", supervisor, path_source, "True"),
        ("cases root identity binding removed", cases, cases_source, "False"),
        (
            "supervisor cases source-only sentinel removed",
            cases,
            sentinel_source,
            "CASES_LOAD_VERSION",
        ),
        (
            "observation failure injection removed",
            cases,
            observation_source,
            observation_replacement,
        ),
    )


def _receipt_write_mutations() -> tuple[Mutation, ...]:
    """Return removals of exact receipt/proof writes that must fire policy."""
    supervisor = "devcontainer_image_selftest_supervisor"
    cases = "devcontainer_image_selftest_supervisor_cases"
    identity_call = (
        "        _write_exact(\n"
        "            identity_descriptor,\n"
        '            f"{supervisor.pid}\\n".encode("ascii"),\n'
        "            RECEIPT_MAX_BYTES,\n"
        "        )"
    )
    proof_call = (
        "        _write_exact(\n"
        "            proof_descriptor,\n"
        '            b"K\\n" if killed and contained else b"F\\n",\n'
        "            RECEIPT_MAX_BYTES,\n"
        "        )"
    )
    return (
        (
            "exclusive receipt exact-write use removed",
            supervisor,
            "        _write_exact(descriptor, payload, ENTRY_MAX_BYTES)",
            "        os.write(descriptor, payload)",
        ),
        (
            "watchdog identity receipt exact-write removed",
            cases,
            identity_call,
            '        os.write(identity_descriptor, f"{supervisor.pid}\\n".encode("ascii"))',
        ),
        (
            "watchdog proof receipt exact-write removed",
            cases,
            proof_call,
            '        os.write(proof_descriptor, b"K\\n" if killed and contained else b"F\\n")',
        ),
    )


def process_authority_mutations() -> tuple[Mutation, ...]:
    """Return extracted and runtime-root semantic mutation fixtures."""
    return (
        *_legacy_process_authority_mutations(),
        *_root_binding_mutations(),
        *_receipt_write_mutations(),
    )

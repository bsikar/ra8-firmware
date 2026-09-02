# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Static image-lock mutations split from the aggregate convergence fixtures."""

from __future__ import annotations

from hil_convergence_safety_semantic_mutations import process_authority_mutations

Mutation = tuple[str, str, str, str]


def _authority_mutations() -> tuple[Mutation, ...]:
    """Return mutations of the duplicated canonical lock authority."""
    return (
        (
            "managed image lock default moved back into the sticky cache",
            "dev_defaults",
            "dev_box_image_lock_dir: /var/cache/ra8-devcontainer-image-lock",
            "dev_box_image_lock_dir: /var/cache/ra8-tools",
        ),
        (
            "script canonical image lock authority drifted",
            "devcontainer_image",
            'CANONICAL_IMAGE_LOCK_DIR="/var/cache/ra8-devcontainer-image-lock"',
            'CANONICAL_IMAGE_LOCK_DIR="/var/cache/ra8-tools"',
        ),
        (
            "shell profile reintroduces a second image lock authority",
            "dev_main",
            '      export RA8_CONTAINER_RUNTIME="{{ dev_box_container_runtime }}"\n',
            '      export RA8_CONTAINER_RUNTIME="{{ dev_box_container_runtime }}"\n'
            '      export RA8_IMAGE_LOCK_DIR="{{ dev_box_image_lock_dir }}"\n',
        ),
        (
            "managed image lock transaction include drifted",
            "dev_transaction",
            "- name: Converge the managed devcontainer image lock authority\n"
            "  ansible.builtin.include_tasks: image_lock.yml\n",
            "- name: Converge the managed devcontainer image lock authority\n"
            "  ansible.builtin.include_tasks: hil_runner.yml\n",
        ),
    )


def _ansible_proof_mutations() -> tuple[Mutation, ...]:
    """Return Ansible post-stat and identity-proof mutations."""
    return (
        *_ansible_stat_mutations(),
        *_ansible_identity_mutations(),
    )


def _ansible_stat_mutations() -> tuple[Mutation, ...]:
    """Return Ansible no-follow stat mutations."""
    return (
        (
            "managed image lock marker pre-stat follows links",
            "dev_main",
            "- name: Inspect the managed image lock group marker without following links\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.gid"\n'
            "    follow: false",
            "- name: Inspect the managed image lock group marker without following links\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.gid"\n'
            "    follow: true",
        ),
        (
            "managed image lock directory post-stat follows links",
            "dev_main",
            "- name: Reinspect the converged managed image lock directory\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}"\n'
            "    follow: false",
            "- name: Reinspect the converged managed image lock directory\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}"\n'
            "    follow: true",
        ),
        (
            "managed image lock file post-stat follows links",
            "dev_main",
            "- name: Reinspect the converged managed image lock\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.lock"\n'
            "    follow: false",
            "- name: Reinspect the converged managed image lock\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.lock"\n'
            "    follow: true",
        ),
    )


def _ansible_identity_mutations() -> tuple[Mutation, ...]:
    """Return Ansible numeric identity-proof mutations."""
    return (
        (
            "managed image lock directory numeric owner proof weakened",
            "dev_main",
            "      - dev_box_image_lock_dir_after.stat.uid == 0",
            "      - dev_box_image_lock_dir_after.stat.uid >= 0",
        ),
        (
            "managed image lock numeric owner proof weakened",
            "dev_main",
            "      - dev_box_image_lock_after.stat.uid == 0",
            "      - dev_box_image_lock_after.stat.uid >= 0",
        ),
        (
            "managed image lock marker post-stat follows links",
            "dev_main",
            "- name: Reinspect the converged managed image lock group marker\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.gid"\n'
            "    follow: false",
            "- name: Reinspect the converged managed image lock group marker\n"
            "  become: true\n"
            "  ansible.builtin.stat:\n"
            '    path: "{{ dev_box_image_lock_dir }}/devcontainer-image.gid"\n'
            "    follow: true",
        ),
        (
            "managed image lock marker numeric owner proof weakened",
            "dev_main",
            "      - dev_box_image_lock_gid_marker_after.stat.uid == 0",
            "      - dev_box_image_lock_gid_marker_after.stat.uid >= 0",
        ),
        (
            "managed image lock marker numeric group content proof removed",
            "dev_main",
            "      - dev_box_image_lock_dir_after.stat.gid == "
            "(dev_box_image_lock_gid.stdout | int)\n"
            "      - dev_box_image_lock_after.stat.gid == "
            "(dev_box_image_lock_gid.stdout | int)",
            "      - dev_box_image_lock_dir_after.stat.gid == "
            "(dev_box_image_lock_gid.stdout | int)\n"
            "      - dev_box_image_lock_after.stat.gid >= 0",
        ),
    )


def _serialization_mutations() -> tuple[Mutation, ...]:
    """Return no-create, inode, force, and selftest mutations."""
    return (
        (
            "managed image lock open can recreate a missing file",
            "devcontainer_image",
            '      exec 9<"$IMAGE_LOCK_FILE" || die "cannot open existing image lock: '
            '$IMAGE_LOCK_FILE"',
            '      exec 9>>"$IMAGE_LOCK_FILE" || die "cannot open existing image lock: '
            '$IMAGE_LOCK_FILE"',
        ),
        (
            "managed image lock pre-flock opened-inode validation removed",
            "devcontainer_image",
            "      validate_opened_image_lock 9\n      if ! flock -n 9; then",
            "      # validate_opened_image_lock 9\n      if ! flock -n 9; then",
        ),
        (
            "managed image lock post-flock opened-inode validation removed",
            "devcontainer_image",
            "        flock 9\n      fi\n      validate_opened_image_lock 9",
            "        flock 9\n      fi\n      # validate_opened_image_lock 9",
        ),
        (
            "forced image rebuild bypasses serialization",
            "devcontainer_image",
            '      build_locked "$want" forced "" 1',
            '      build_image "$want"',
        ),
        (
            "managed image lock strict flock refusal bypassed",
            "devcontainer_image",
            '      [[ "$IMAGE_LOCK_MANAGED" == "0" ]] ||',
            '      true || [[ "$IMAGE_LOCK_MANAGED" == "0" ]] ||',
        ),
        (
            "managed image lock selftest dispatcher stranded",
            "devcontainer_image_selftest_cases",
            '    dispatch_image_lock_selftest suite "$tmp"',
            '    # dispatch_image_lock_selftest suite "$tmp"',
        ),
        (
            "managed image lock top-level completion check removed",
            "devcontainer_image",
            '  main "$@"\n'
            '  if [[ "${1:-}" == "--selftest" ]]; then\n'
            '    [[ "$SELFTEST_MAIN_COMPLETE" == "1" ]] || '
            'die "selftest main returned before completion"\n'
            "  fi",
            '  main "$@"',
        ),
        (
            "managed image lock ensure preflight stranded",
            "devcontainer_image",
            "    managed_image_lock_preflight\n    want=",
            "    # managed_image_lock_preflight\n    want=",
        ),
    )


def _selftest_fallback_mutations() -> tuple[Mutation, ...]:
    """Return controller fallback and escalation mutations."""
    return (
        (
            "image lock signal fallback removed",
            "devcontainer_image_signal_selftest",
            '  force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||\n'
            '    die "selftest: handler-hang group fallback failed"',
            "      true",
        ),
        (
            "image lock fallback controller TERM removed",
            "devcontainer_image_lock_selftest",
            '    signal_owned_controller_group TERM "$controller" 2>/dev/null || return 1',
            "    true",
        ),
        (
            "image lock controller cleanup bound shortened below worker cleanup",
            "devcontainer_image_lock_selftest",
            "SELFTEST_CONTROLLER_CLEANUP_STEPS=1600",
            "SELFTEST_CONTROLLER_CLEANUP_STEPS=50",
        ),
        (
            "image lock unready-controller fallback removed",
            "devcontainer_image_signal_selftest",
            '      force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||\n'
            '        die "selftest: $signal unready-controller cleanup failed"',
            "      true",
        ),
    )


def _early_return_mutations(key: str, names: tuple[str, ...], indent: str) -> tuple[Mutation, ...]:
    """Build early-success mutations for one exact shell authority."""
    return tuple(
        (
            f"image lock {name} early success refused",
            key,
            f"{indent}{name}() {{\n",
            f"{indent}{name}() {{\n{indent}  return 0\n",
        )
        for name in names
    )


def _receipt_early_return_mutations() -> tuple[Mutation, ...]:
    """Return must-fire early-success mutations for every receipt boundary."""
    receipt_names = (
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
    helper_names = (
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
    return (
        *_early_return_mutations("devcontainer_image", ("build_locked", "main"), "  "),
        *_early_return_mutations("devcontainer_image_lock_receipts", receipt_names, ""),
        *_early_return_mutations("devcontainer_image_lock_selftest", helper_names, ""),
        *_early_return_mutations("devcontainer_image_selftest_cases", ("cmd_selftest",), ""),
        *_early_return_mutations(
            "devcontainer_image_signal_selftest",
            ("selftest_signal_ready_timeout", "selftest_signal_cleanup"),
            "",
        ),
    )


def _receipt_semantic_mutations() -> tuple[Mutation, ...]:
    """Return removals of the scenario facts that authorize final receipts."""
    post_ready_negative = (
        '  if wait_for_status_file "$SELFTEST_CASE_DIR/done.status" '
        '"$SELFTEST_WORKER_PID"; then\n'
        '    die "selftest: build-hang worker unexpectedly completed"\n'
        "  fi\n"
    )
    ready_timeout_negative = (
        '  if wait_for_status_file "$case_dir/controller-ready.status" "$controller"; then\n'
        '    die "selftest: delayed signal controller unexpectedly became ready"\n'
        "  fi\n"
    )
    contention_loop = (
        "  for ((attempt = 0; attempt < 20; ++attempt)); do\n"
        '    [[ ! -e "$SELFTEST_CASE_DIR/build-entered.status" ]] ||\n'
        '      die "selftest: forced rebuild bypassed the held lock"\n'
        '    process_is_terminal "$SELFTEST_WORKER_PID" &&\n'
        '      die "selftest: forced rebuild exited while the lock was held"\n'
        "    sleep 0.01\n"
        "  done\n"
    )
    signal_status = (
        '    [[ "$SELFTEST_REAP_STATUS" == "$expected" ]] ||\n'
        '      die "selftest: $signal controller returned $SELFTEST_REAP_STATUS, '
        'expected $expected"\n'
    )
    return (
        (
            "image lock post-ready negative completion proof removed",
            "devcontainer_image_lock_selftest",
            post_ready_negative,
            "",
        ),
        (
            "image lock post-ready proof stranded in comments",
            "devcontainer_image_lock_selftest",
            post_ready_negative,
            "  # post-ready negative completion proof removed\n",
        ),
        (
            "image lock signal-ready negative deadline proof removed",
            "devcontainer_image_signal_selftest",
            ready_timeout_negative,
            "",
        ),
        (
            "image lock signal expected status proof removed",
            "devcontainer_image_signal_selftest",
            signal_status,
            "",
        ),
        (
            "image lock forced contention observation loop removed",
            "devcontainer_image_lock_selftest",
            contention_loop,
            "",
        ),
    )


def _raw_return_mutations() -> tuple[Mutation, ...]:
    """Return equivalent Bash success spellings rejected by the raw digest."""
    opening = "parent_lock_fd_is_closed() {\n"
    variants = (
        ("return plus-zero", "  return +0\n"),
        ("return double-zero", "  return 00\n"),
        ("arithmetic return", '  return "$((0))"\n'),
        ("command return", "  command return 0\n"),
        ("builtin return", "  builtin return 0\n"),
        ("exit success", "  exit 0\n"),
        ("inline success", "  true; return 0\n"),
    )
    return tuple(
        (
            f"image lock raw digest rejects {label}",
            "devcontainer_image_lock_receipts",
            opening,
            opening + payload,
        )
        for label, payload in variants
    )


def _raw_surface_mutations() -> tuple[Mutation, ...]:
    """Return raw comment and alternate-definition mutations."""
    return (
        (
            "image lock entry raw-byte comment change",
            "devcontainer_image",
            "# Copyright (c) 2026 Brighton Sikarskie\n",
            "# Copyright (c) 2026 Brighton Sikarskie \n",
        ),
        (
            "image lock helper raw-byte comment change",
            "devcontainer_image_lock_selftest",
            "# Copyright (c) 2026 Brighton Sikarskie\n",
            "# Copyright (c) 2026 Brighton Sikarskie \n",
        ),
        (
            "image lock receipt helper raw-byte comment change",
            "devcontainer_image_lock_receipts",
            "# Copyright (c) 2026 Brighton Sikarskie\n",
            "# Copyright (c) 2026 Brighton Sikarskie \n",
        ),
        (
            "image lock appended alternate function definition",
            "devcontainer_image_lock_selftest",
            "  *) die \"unknown image-lock selftest dispatch '$command'\" ;;\n  esac\n}\n",
            "  *) die \"unknown image-lock selftest dispatch '$command'\" ;;\n"
            "  esac\n"
            "}\n\n"
            "parent_lock_fd_is_closed() {\n"
            "  return +0\n"
            "}\n",
        ),
    )


def _raw_digest_mutations() -> tuple[Mutation, ...]:
    """Return raw byte, alternate-definition, and stranded-proof attacks."""
    return (
        *_raw_return_mutations(),
        *_raw_surface_mutations(),
        (
            "image lock active cleanup proof replaced by no-op",
            "devcontainer_image_lock_selftest",
            "  parent_lock_fd_is_closed || cleanup_failed=1\n",
            "  true\n",
        ),
        (
            "image lock cleanup proof stranded in a string",
            "devcontainer_image_lock_selftest",
            "  parent_lock_fd_is_closed || cleanup_failed=1\n",
            "  printf '%s\\n' 'parent_lock_fd_is_closed' >/dev/null\n",
        ),
        (
            "image lock cleanup proof stranded in a comment",
            "devcontainer_image_lock_selftest",
            "  parent_lock_fd_is_closed || cleanup_failed=1\n",
            "  # parent_lock_fd_is_closed || cleanup_failed=1\n",
        ),
        (
            "image lock signal-controller sleep inherits parent lock FD",
            "devcontainer_image_lock_selftest",
            '  if [[ "$readiness_mode" == "delay-controller-ready" ]]; then\n'
            "    while :; do sleep 1 8>&-; done\n"
            "  fi\n",
            '  if [[ "$readiness_mode" == "delay-controller-ready" ]]; then\n'
            "    while :; do sleep 1; done\n"
            "  fi\n",
        ),
    )


def _selftest_semantic_attack_mutations() -> tuple[Mutation, ...]:
    """Return managed-object semantic attack mutations."""
    return (
        (
            "missing managed group marker attack removed",
            "devcontainer_image_lock_selftest",
            'if (RA8_IMAGE_LOCK_DIR="$tmp/missing-marker" resolve_image_lock ',
            "if (false && RA8_IMAGE_LOCK_DIR=unused resolve_image_lock ",
        ),
        (
            "symlinked managed group marker attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: symlinked managed image lock group marker passed"',
            "      true",
        ),
        (
            "hardlinked managed group marker attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: multiply-linked managed image lock group marker passed"',
            "      true",
        ),
        (
            "wrong managed directory group attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: wrong managed image lock directory group passed"',
            "      true",
        ),
        (
            "wrong managed lock group attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: wrong managed image lock file group passed"',
            "      true",
        ),
        (
            "wrong managed group marker owner attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: non-root managed image lock group marker passed"',
            "      true",
        ),
        (
            "wrong managed group marker mode attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: writable managed image lock group marker passed"',
            "      true",
        ),
        (
            "wrong managed group marker content attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: root group in managed image lock group marker passed"',
            "      true",
        ),
        (
            "binary managed group marker attack removed",
            "devcontainer_image_lock_selftest",
            '    die "selftest: binary managed image lock group marker passed"',
            "    true",
        ),
    )


def _extraction_authority_mutations() -> tuple[Mutation, ...]:
    """Return load-bearing mutations for extracted supervisor responsibilities."""
    return (
        (
            "supervisor interruption-handler definition removed",
            "devcontainer_image_selftest_supervisor",
            "def _install_interruption_handlers(supervisor: BoundGroup) -> None:",
            "def _install_interruption_handlers_disabled(supervisor: BoundGroup) -> None:",
        ),
        (
            "supervisor interruption-handler call removed",
            "devcontainer_image_selftest_supervisor",
            "_install_interruption_handlers(supervisor)",
            "pass  # mutation: handler install removed",
        ),
        (
            "closed-controller command definition removed",
            "devcontainer_image_selftest_supervisor_cases",
            "def _closed_controller_command(",
            "def _closed_controller_command_disabled(",
        ),
        (
            "closed-controller command call removed",
            "devcontainer_image_selftest_supervisor_cases",
            "command = _closed_controller_command(\n"
            "            entry_authority, status, death_descriptor, root_descriptor, "
            "root_identity\n"
            "        )\n"
            "        child = subprocess.Popen(",
            "child = subprocess.Popen(",
        ),
    )


def runtime_selftest_tail_mutations() -> tuple[Mutation, ...]:
    """Return process-controller fallback and semantic attack mutations."""
    return (
        *_selftest_fallback_mutations(),
        *process_authority_mutations(),
        *_extraction_authority_mutations(),
        *_selftest_semantic_attack_mutations(),
    )


def runtime_readiness_mutations() -> tuple[Mutation, ...]:
    """Return readiness, liveness, and cleanup-trap mutations."""
    return (
        (
            "forced-build readiness deadline removed",
            "devcontainer_image_lock_selftest",
            "wait_for_status_file() {\n"
            '  local path="$1" pid="$2" attempt\n'
            "  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do",
            'wait_for_status_file() {\n  local path="$1" pid="$2" attempt\n  while true; do',
        ),
        (
            "forced-build readiness child-liveness check bypassed",
            "devcontainer_image_lock_selftest",
            '    if process_is_terminal "$pid"; then\n'
            '      [[ -s "$path" ]] && return 0\n'
            "      return 1",
            "    if false; then\n      return 0\n      return 1",
        ),
        (
            "forced-build failure cleanup trap removed",
            "devcontainer_image_lock_selftest",
            "  trap image_lock_case_exit EXIT",
            "  trap ':' EXIT",
        ),
    )


def runtime_worker_mutations() -> tuple[Mutation, ...]:
    """Return isolated worker-group handshake mutations."""
    return (
        (
            "image lock worker process-group isolation removed",
            "devcontainer_image_lock_selftest",
            '    exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
            '    exec /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
        ),
        (
            "image lock worker inherited parent lock close removed",
            "devcontainer_image_lock_selftest",
            "      exec 8>&-\n      trap '' HUP INT TERM\n"
            '      exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
            "      :\n      trap '' HUP INT TERM\n"
            '      exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY"',
        ),
        (
            "image lock worker verified group handshake removed",
            "devcontainer_image_lock_selftest",
            "  if ! read_worker_group; then\n    abort_bound_worker_spawn 1\n  fi",
            "  if false; then\n    abort_bound_worker_spawn 1\n  fi",
        ),
        (
            "image lock worker group record moved after dispatch",
            "devcontainer_image_lock_selftest",
            '  record_worker_group "$case_dir" "$mode"\n'
            '  wait_for_worker_ack "$case_dir" "$$" || exit 124',
            '  wait_for_worker_ack "$case_dir" "$$" || exit 124\n'
            '  record_worker_group "$case_dir" "$mode"',
        ),
        (
            "image lock worker pgid equality guard removed",
            "devcontainer_image_lock_selftest",
            '    [[ "$pgid" == "$pid" ]] || die "selftest isolated worker is not its group leader"',
            "  true",
        ),
        (
            "image lock worker own-group guard removed",
            "devcontainer_image_lock_selftest",
            '  [[ "$SELFTEST_WORKER_PGID" != "$own_pgid" ]]',
            "  true",
        ),
    )


def runtime_loader_mutations() -> tuple[Mutation, ...]:
    """Return authenticated receipt and lock helper loader mutations."""
    return (
        (
            "managed image lock receipt helper source removed",
            "devcontainer_image_selftest_cases",
            '  source_approved_selftest_helper "$receipts" "$IMAGE_LOCK_RECEIPTS_RAW_SHA256"',
            "  true",
        ),
        (
            "managed image lock receipt source-only diagnostic weakened",
            "devcontainer_image_selftest_cases",
            '    "$output" == "error: devcontainer image lock receipt helper is source-only"',
            '    "$output" != ""',
        ),
        (
            "managed image lock selftest helper source removed",
            "devcontainer_image_selftest_cases",
            '  source_approved_selftest_helper "$helper" "$IMAGE_LOCK_SELFTEST_RAW_SHA256"',
            "  true",
        ),
        (
            "managed image lock receipt helper loaded after its consumer",
            "devcontainer_image_selftest_cases",
            '  source_approved_selftest_helper "$receipts" "$IMAGE_LOCK_RECEIPTS_RAW_SHA256"\n'
            '  source_approved_selftest_helper "$helper" "$IMAGE_LOCK_SELFTEST_RAW_SHA256"',
            '  source_approved_selftest_helper "$helper" "$IMAGE_LOCK_SELFTEST_RAW_SHA256"\n'
            '  source_approved_selftest_helper "$receipts" "$IMAGE_LOCK_RECEIPTS_RAW_SHA256"',
        ),
    )


def mutations() -> tuple[Mutation, ...]:
    """Return the split image-lock authority/proof/serialization mutations."""
    return (
        *_authority_mutations(),
        *_ansible_proof_mutations(),
        *_serialization_mutations(),
        *_receipt_early_return_mutations(),
        *_receipt_semantic_mutations(),
        *_raw_digest_mutations(),
    )

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Independent must-fire mutations for the HIL convergence safety gate."""

from __future__ import annotations

from hil_convergence_safety_image_lock_fixtures import (
    mutations as split_image_lock_mutations,
)
from hil_convergence_safety_image_lock_fixtures import (
    runtime_loader_mutations,
    runtime_readiness_mutations,
    runtime_selftest_tail_mutations,
    runtime_worker_mutations,
)

Mutation = tuple[str, str, str, str]
Reorder = tuple[str, str, str]


def _helper_mutations() -> tuple[Mutation, ...]:
    """Return executable idle-stop helper mutations."""
    return (
        (
            "freeze token stranded in a comment",
            "idle_helper",
            'control.command("freeze", service)',
            'control.command("thaw", service)\n        # control.command("freeze", service)',
        ),
        (
            "transport-loss thaw removal",
            "idle_helper",
            "signal.signal(signal.SIGHUP, _interrupt)",
            "signal.signal(signal.SIGTERM, _interrupt)",
        ),
        (
            "semantic helper selftest stranded in a comment",
            "gate",
            "  python3 infra/ansible/roles/dev_box/files/"
            "ra8-hil-runner-idle-stop.py --selftest ignored.service",
            "  # python3 infra/ansible/roles/dev_box/files/"
            "ra8-hil-runner-idle-stop.py --selftest ignored.service",
        ),
        (
            "absent-unit distinction removal",
            "idle_helper",
            'if load_state == "not-found":',
            'if load_state == "inactive":',
        ),
    )


def _runner_dispatch_mutations() -> tuple[Mutation, ...]:
    """Return native dispatcher mutations."""
    return (
        (
            "whole-apply wrapper token stranded in a comment",
            "fleet",
            "guard = _bench_guard_argv(host, plays, args)",
            "# guard = _bench_guard_argv(host, plays, args)\n    guard = []",
        ),
        (
            "native-runner preflight removal",
            "fleet",
            "maintenance = _prepare_native_runner(",
            "maintenance = frm.MaintenanceDecision(",
        ),
        (
            "no-op preflight bypass",
            "fleet_runner",
            "if not has_changes:",
            "if False and not has_changes:",
        ),
        (
            "no-op token stranded in a comment",
            "fleet_runner",
            "    if not has_changes:",
            "    # if not has_changes:\n    if False:",
        ),
        (
            "native Ansible sanitizer stranded in a comment",
            "fleet",
            "env=frm.ansible_environment(os.environ, fm.ANSIBLE_DIR),",
            "env=None,  # env=frm.ansible_environment(os.environ, fm.ANSIBLE_DIR),",
        ),
    )


def _bench_boundary_mutations() -> tuple[Mutation, ...]:
    """Return bench lock, authentication, and refusal mutations."""
    return (
        (
            "bench privileged Bash boundary removal",
            "fleet_bench",
            '"/bin/bash",\n        "-p",\n        str(request.repo_root / "scripts/hil/bench.sh"),',
            'str(request.repo_root / "scripts/hil/bench.sh"),',
        ),
        (
            "bench live-lock privileged Bash boundary removal",
            "fleet_bench",
            '"--norc",\n                "-p",\n                "-c",\n                script,',
            '"--norc",\n                "-c",\n                script,',
        ),
        (
            "bench privileged Bash execution probe order invalid",
            "fleet_bench",
            '"/bin/bash",\n                "--noprofile",\n                "--norc",\n'
            '                "-p",\n                "-c",\n                probe,',
            '"/bin/bash",\n                "-p",\n                "--noprofile",\n'
            '                "--norc",\n                "-c",\n                probe,',
        ),
        (
            "bench privileged Bash execution probe stranded",
            "fleet_bench",
            "failures = _privileged_bash_selftest()",
            "failures = []  # _privileged_bash_selftest()",
        ),
        (
            "bench tag refusal bypass",
            "fleet_bench",
            "if request.tags and not request.trusted_tags:",
            "if False and request.tags and not request.trusted_tags:",
        ),
        (
            "bench extra-var refusal bypass",
            "fleet_bench",
            "if request.extra_vars:",
            "if False and request.extra_vars:",
        ),
        (
            "well-formed inherited lock accepted without authentication",
            "fleet_bench",
            "if not authenticate(request.repo_root, capability):",
            "if False and not authenticate(request.repo_root, capability):",
        ),
    )


def _runner_boundary_mutations() -> tuple[Mutation, ...]:
    """Return native environment and transport mutations."""
    return (
        (
            "native child PATH inheritance restored",
            "fleet_runner",
            '"LC_ALL": "C.UTF-8",\n        "PATH": "/usr/bin:/bin",',
            '"LC_ALL": "C.UTF-8",\n        "PATH": environment.get("PATH", "/usr/bin:/bin"),',
        ),
        (
            "collection link census stranded",
            "fleet_runner",
            "link_errors = fpa.confined_link_errors(collections)",
            "link_errors = []  # fpa.confined_link_errors(collections)",
        ),
        (
            "absolute SSH authority removed",
            "fleet_reach",
            '["/usr/bin/ssh", *SSH_OPTIONS]',
            '["ssh", *SSH_OPTIONS]',
        ),
    )


def _wsl_mode_mutations() -> tuple[Mutation, ...]:
    """Return managed WSL environment and operation-mode mutations."""
    return (
        (
            "WSL sanitizer stranded as inert text",
            "fleet_wsl",
            '\'  case "$name" in ANSIBLE_*) unset "$name" ;; esac\',',
            '\'# case "$name" in ANSIBLE_*) unset "$name" ;; esac\',',
        ),
        (
            "WSL uv sanitizer stranded as inert text",
            "fleet_wsl",
            '\'  case "$name" in UV_*) unset "$name" ;; esac\',',
            '\'# case "$name" in UV_*) unset "$name" ;; esac\',',
        ),
        (
            "WSL apply-only sync decision bypassed",
            "fleet_wsl",
            '"authority_digest=${authority_digest%% *}",\n'
            "        'if [ \"$mode\" = apply ]; then',",
            '"authority_digest=${authority_digest%% *}",\n'
            "        'if [ \"$mode\" != apply ]; then',",
        ),
        (
            "WSL operation mode forced to apply",
            "fleet",
            "            request.args.mode,",
            '            "apply",  # request.args.mode',
        ),
        (
            "WSL check-mode verification made mutating",
            "fleet_wsl",
            'f"{sync_flags} --check",',
            'f"{sync_flags}",',
        ),
    )


def _wsl_transport_mutations() -> tuple[Mutation, ...]:
    """Return WSL stage, cache, shell-boundary, and archive mutations."""
    return (
        (
            "WSL owned-stage selftest stranded",
            "fleet_wsl",
            "stage_failures = fws.run_selftest()",
            "stage_failures = []  # fws.run_selftest()",
        ),
        (
            "WSL no-follow cache receiver weakened",
            "fleet_wsl_stage",
            "os.O_EXCL|os.O_NOFOLLOW",
            "os.O_EXCL|0  # os.O_NOFOLLOW",
        ),
        (
            "WSL cache ownership proof stranded",
            "fleet_wsl",
            "stdin=fws.cache_prepare_script(),",
            'stdin="true\\n",  # fws.cache_prepare_script()',
        ),
        (
            "WSL Bash env-empty boundary removed",
            "fleet_runner_model",
            "HOME=/root PATH=/usr/bin:/bin /bin/bash -s",
            "HOME=/root PATH=/hostile /bin/bash -s",
        ),
        (
            "WSL archive PATH lookup restored",
            "fleet_wsl_stage",
            'tar_tool = Path("/usr/bin/tar")',
            'tar_tool = Path("tar")',
        ),
    )


def _wsl_durability_mutations() -> tuple[Mutation, ...]:
    """Return crash-durable publication mutations."""
    return (
        (
            "stage publication parent fsync removed",
            "fleet_wsl_stage",
            '\'  mv -- "$stage" "$previous"\',\n'
            '            \'  sync_dir "$(dirname -- "$stage")"\',',
            '\'  mv -- "$stage" "$previous"\',\n'
            '            \'  true # sync_dir "$(dirname -- "$stage")"\',',
        ),
        (
            "cache publication parent fsync removed",
            "fleet_wsl_stage",
            "'sync_file \"$part\"',",
            "'true # sync_file \"$part\"',",
        ),
        (
            "WSL managed lock marker parent fsync removed",
            "fleet_wsl",
            "'  sync_dir \"$managed_root\"',",
            "'  true # sync_dir \"$managed_root\"',",
        ),
    )


def _role_decision_mutations() -> tuple[Mutation, ...]:
    """Return Ansible service and holder-decision mutations."""
    return (
        (
            "restart without idle proof",
            "dev_role",
            "state: started",
            "state: restarted",
        ),
        (
            "J-Link token stranded behind true",
            "bench_role",
            'JLinkExe -device "$device" -if SWD',
            'true # JLinkExe -device "$device" -if SWD',
        ),
        (
            "J-Link contract device authority replaced",
            "bench_role",
            "--default JLINK_DEVICE",
            "--default PI_REPO",
        ),
        (
            "J-Link duplicate role default introduced",
            "bench_defaults",
            "hil_bench_jlink_speed: 1000",
            'hil_bench_jlink_device: "literal"\nhil_bench_jlink_speed: 1000',
        ),
        (
            "bench kernel verifier source replaced",
            "bench_guard",
            "bench_lock_verify.py",
            "bench_lock_verify_disabled.py",
        ),
        (
            "delegated kernel verifier source replaced",
            "dev_guard",
            "bench_lock_verify.py",
            "bench_lock_verify_disabled.py",
        ),
        (
            "listener decision or-true bypass",
            "dev_guard",
            "dev_box_hil_runner_initial_activity.stdout | trim in ['inactive', 'failed'])))",
            "dev_box_hil_runner_initial_activity.stdout | trim in "
            "['inactive', 'failed']))) or true",
        ),
    )


def _context_authority_mutations() -> tuple[Mutation, ...]:
    """Return lock-verifier and staged-context authority mutations."""
    return (
        (
            "lock verifier regular-fd filter removed",
            "bench_lock_verify",
            "if not stat.S_ISREG(before.st_mode):\n                    continue",
            "if stat.S_ISREG(before.st_mode):\n                    pass",
        ),
        (
            "lock verifier live descriptor selftest stranded",
            "bench_lock_verify",
            "+ _live_descriptor_selftest()",
            "+ []  # live descriptor selftest stranded",
        ),
        (
            "dev context root lock scope removed",
            "dev_defaults",
            "  - uv.lock",
            "  # uv.lock scope removed",
        ),
        (
            "dev context consumed pin renamed away from its authority",
            "dev_main",
            "    - HADOLINT_SHA256_ARM64",
            "    - HADOLINT_SHA256_AARCH64",
        ),
        (
            "dev context consumed root input omitted from assertion",
            "dev_main",
            "    - .dockerignore",
            "    # .dockerignore assertion removed",
        ),
        *_image_lock_mutations(),
    )


def _image_lock_mutations() -> tuple[Mutation, ...]:
    """Return managed image-lock ownership and fail-closed mutations."""
    return (
        *split_image_lock_mutations(),
        *_image_lock_ansible_object_mutations(),
        *_image_lock_runtime_authority_mutations(),
        *_image_lock_runtime_selftest_mutations(),
    )


def _image_lock_ansible_object_mutations() -> tuple[Mutation, ...]:
    """Return Ansible refusal and object-mode mutations."""
    return (
        *_image_lock_ansible_refusal_mutations(),
        *_image_lock_ansible_permission_mutations(),
        *_image_lock_ansible_marker_mutations(),
    )


def _image_lock_ansible_refusal_mutations() -> tuple[Mutation, ...]:
    """Return numeric-group and unsafe-object refusal mutations."""
    return (
        (
            "managed image lock numeric group resolution changed users",
            "dev_main",
            '      - --\n      - "{{ dev_box_user }}"\n  register: dev_box_image_lock_gid',
            "      - --\n      - root\n  register: dev_box_image_lock_gid",
        ),
        (
            "unsafe managed image lock directory refusal removed",
            "dev_main",
            "        not dev_box_image_lock_dir_before.stat.exists or\n",
            "        true or\n",
        ),
        (
            "unsafe managed image lock file refusal removed",
            "dev_main",
            "        not dev_box_image_lock_before.stat.exists or\n",
            "        true or\n",
        ),
    )


def _image_lock_ansible_permission_mutations() -> tuple[Mutation, ...]:
    """Return managed directory and lock permission mutations."""
    return (
        (
            "managed image lock directory made group-replaceable",
            "dev_main",
            '    mode: "0750"\n\n- name: Reinspect the converged managed image lock directory',
            '    mode: "0770"\n\n- name: Reinspect the converged managed image lock directory',
        ),
        (
            "managed image lock file made world-writable",
            "dev_main",
            '    mode: "0660"\n    access_time: preserve',
            '    mode: "0666"\n    access_time: preserve',
        ),
        (
            "managed image lock directory numeric group drifted",
            "dev_main",
            '    group: "{{ dev_box_image_lock_gid.stdout }}"\n    mode: "0750"',
            '    group: root\n    mode: "0750"',
        ),
        (
            "managed image lock file numeric group drifted",
            "dev_main",
            '    group: "{{ dev_box_image_lock_gid.stdout }}"\n    mode: "0660"',
            '    group: root\n    mode: "0660"',
        ),
    )


def _image_lock_ansible_marker_mutations() -> tuple[Mutation, ...]:
    """Return managed numeric-group marker mutations."""
    return (
        (
            "managed image lock group marker refusal removed",
            "dev_main",
            "        not dev_box_image_lock_gid_marker_before.stat.exists or\n",
            "        true or\n",
        ),
        (
            "managed image lock group marker made writable",
            "dev_main",
            '    mode: "0444"\n    unsafe_writes: false',
            '    mode: "0644"\n    unsafe_writes: false',
        ),
        (
            "managed image lock group marker owner drifted",
            "dev_main",
            '    owner: root\n    group: root\n    mode: "0444"',
            '    owner: nobody\n    group: root\n    mode: "0444"',
        ),
        (
            "managed image lock group marker content drifted",
            "dev_main",
            '    content: "{{ dev_box_image_lock_gid.stdout }}\\n"',
            '    content: "0\\n"',
        ),
        (
            "managed image lock group marker loses atomic writes",
            "dev_main",
            "    unsafe_writes: false",
            "    unsafe_writes: true",
        ),
    )


def _image_lock_runtime_authority_mutations() -> tuple[Mutation, ...]:
    """Return runtime metadata and discovery mutations."""
    return (
        *_image_lock_runtime_discovery_mutations(),
        *_image_lock_runtime_marker_mutations(),
        *runtime_loader_mutations(),
    )


def _image_lock_runtime_discovery_mutations() -> tuple[Mutation, ...]:
    """Return managed metadata and canonical discovery mutations."""
    return (
        (
            "runtime managed directory metadata validation weakened",
            "devcontainer_image",
            '    [[ "$owner" == "0" && "$mode" == "750" ]] ||',
            '    [[ "$owner" == "0" ]] ||',
        ),
        (
            "runtime managed lock mode validation weakened",
            "devcontainer_image",
            '      [[ "$group" == "$IMAGE_LOCK_GROUP_GID" ]] ||',
            '      [[ "$group" == "0" ]] ||',
        ),
        (
            "canonical non-login image lock discovery removed",
            "devcontainer_image",
            '    elif [[ -e "$canonical_dir" || -L "$canonical_dir" ||\n'
            '      -e "$canonical_dir/devcontainer-image.lock" ||\n'
            '      -L "$canonical_dir/devcontainer-image.lock" ||\n'
            '      -e "$canonical_dir/devcontainer-image.gid" ||\n'
            '      -L "$canonical_dir/devcontainer-image.gid" ]]; then',
            "    elif false; then",
        ),
    )


def _image_lock_runtime_marker_mutations() -> tuple[Mutation, ...]:
    """Return managed marker identity and content mutations."""
    return (
        (
            "managed image lock group marker hardlink check removed",
            "devcontainer_image",
            '    [[ "$links" == "1" && "$owner" == "0" && "$group" == "0" '
            '&& "$mode" == "444" ]] ||',
            '    [[ "$owner" == "0" && "$group" == "0" && "$mode" == "444" ]] ||',
        ),
        (
            "managed image lock group marker owner check removed",
            "devcontainer_image",
            '"$links" == "1" && "$owner" == "0" && "$group" == "0"',
            '"$links" == "1" && "$owner" != "" && "$group" == "0"',
        ),
        (
            "managed image lock group marker mode check weakened",
            "devcontainer_image",
            '&& "$mode" == "444" ]] ||',
            '&& "$mode" == "644" ]] ||',
        ),
        (
            "managed image lock group marker allows root group content",
            "devcontainer_image",
            '[[ "$marker_gid" =~ ^[0-9]+$ && "$marker_gid" != "0" ]] ||',
            '[[ "$marker_gid" =~ ^[0-9]+$ ]] ||',
        ),
        (
            "managed image lock group marker trailing content check removed",
            "devcontainer_image",
            "    if IFS= read -r -n 1 extra <&7; then",
            "    if false; then",
        ),
        (
            "managed image lock group marker exact size proof removed",
            "devcontainer_image",
            "    ((size == ${#marker_gid} + 1)) ||",
            "    true ||",
        ),
        (
            "managed image lock group marker opened-inode check removed",
            "devcontainer_image",
            '    [[ "$opened" == "$identity" && "$current" == "$opened" ]] ||',
            "    true ||",
        ),
    )


def _image_lock_runtime_selftest_mutations() -> tuple[Mutation, ...]:
    """Return bounded contention-selftest mutations."""
    return (
        *runtime_readiness_mutations(),
        *runtime_worker_mutations(),
        *_image_lock_selftest_cleanup_mutations(),
        *_image_lock_selftest_lock_control_mutations(),
        *_image_lock_selftest_dispatch_mutations(),
        *_image_lock_selftest_completion_mutations(),
        *_image_lock_selftest_trap_mutations(),
        *runtime_selftest_tail_mutations(),
    )


def _image_lock_selftest_cleanup_mutations() -> tuple[Mutation, ...]:
    """Return group termination, lock release, and reap mutations."""
    return (
        (
            "image lock cleanup group TERM removed",
            "devcontainer_image_lock_selftest",
            "    if worker_group_signal_is_authorized 2>/dev/null; then\n"
            "      group_signal_authorized=1\n"
            '      kill -TERM -- "-$SELFTEST_WORKER_PGID" 2>/dev/null || cleanup_failed=1',
            "    if worker_group_signal_is_authorized 2>/dev/null; then\n      true",
        ),
        (
            "image lock cleanup group KILL escalation removed",
            "devcontainer_image_lock_selftest",
            '    if [[ "$group_signal_authorized" == "1" ]] &&\n'
            "      worker_group_signal_is_authorized 2>/dev/null; then\n"
            '      kill -KILL -- "-$SELFTEST_WORKER_PGID" 2>/dev/null || cleanup_failed=1',
            '    if [[ "$group_signal_authorized" == "1" ]] &&\n      true; then\n      true',
        ),
        (
            "image lock cleanup unlock removed",
            "devcontainer_image_lock_selftest",
            "    flock -u 8 || release_failed=1\n    if exec 8>&-; then",
            "    true || release_failed=1\n    if exec 8>&-; then",
        ),
        (
            "image lock cleanup descriptor close removed",
            "devcontainer_image_lock_selftest",
            "    if exec 8>&-; then\n      SELFTEST_PARENT_LOCK_OPEN=0",
            "    if true; then\n      SELFTEST_PARENT_LOCK_OPEN=0",
        ),
        (
            "image lock bounded reap terminal proof removed",
            "devcontainer_image_lock_selftest",
            '  bounded_process_terminal "$pid" || return 1\n  if wait "$pid"; then',
            '  true\n  if wait "$pid"; then',
        ),
    )


def _image_lock_selftest_lock_control_mutations() -> tuple[Mutation, ...]:
    """Return lock-probe mutations and their two-sided controls."""
    return (
        (
            "image lock fresh lock probe bypassed",
            "devcontainer_image_lock_selftest",
            "  if ! flock -n 7; then\n    if ! exec 7>&-; then",
            "  if ! true; then\n    if ! exec 7>&-; then",
        ),
        (
            "image lock held-lock negative control removed",
            "devcontainer_image_lock_selftest",
            "  if fresh_lock_probe; then\n"
            '    die "selftest: fresh lock probe accepted a held lock"',
            '  if false; then\n    die "selftest: fresh lock probe accepted a held lock"',
        ),
        (
            "image lock released-lock positive control removed",
            "devcontainer_image_lock_selftest",
            '  fresh_lock_probe || die "selftest: fresh lock probe failed after worker completion"',
            "  true",
        ),
    )


def _image_lock_selftest_dispatch_mutations() -> tuple[Mutation, ...]:
    """Return mutations that strand individual attack dispatchers."""
    return (
        (
            "image lock early-exit attack removed",
            "devcontainer_image_selftest_cases",
            '  run_image_lock_scenario early-exit selftest_early_exit "$tmp"',
            "      true",
        ),
        (
            "image lock pre-ready-hang attack removed",
            "devcontainer_image_selftest_cases",
            '  run_image_lock_scenario pre-ready-hang selftest_pre_ready_hang "$tmp"',
            "      true",
        ),
        (
            "image lock post-ready-hang attack removed",
            "devcontainer_image_selftest_cases",
            '  run_image_lock_scenario post-ready-hang selftest_post_ready_hang "$tmp"',
            "      true",
        ),
        (
            "image lock signal cleanup attack removed",
            "devcontainer_image_selftest_cases",
            '  run_image_lock_scenario signal-cleanup selftest_signal_cleanup "$tmp"',
            "      true",
        ),
        (
            "image lock unready signal cleanup attack removed",
            "devcontainer_image_selftest_cases",
            '  run_image_lock_scenario signal-ready-timeout selftest_signal_ready_timeout "$tmp"',
            "      true",
        ),
    )


def _image_lock_selftest_completion_mutations() -> tuple[Mutation, ...]:
    """Return completion, descendant, and final-group proof mutations."""
    return (
        (
            "image lock successful child reap removed",
            "devcontainer_image_lock_selftest",
            '  reap_worker || die "selftest: normal child did not finish"',
            "  true",
        ),
        (
            "image lock successful done-status wait removed",
            "devcontainer_image_lock_selftest",
            '  wait_for_status_file "$SELFTEST_CASE_DIR/done.status" '
            '"$SELFTEST_WORKER_PID" ||\n'
            '    die "selftest: forced rebuild did not complete after release"',
            "  true",
        ),
        (
            "image lock cleanup descendant assertion removed",
            "devcontainer_image_lock_selftest",
            "  assert_no_surviving_descendants || cleanup_failed=1",
            "  true",
        ),
        (
            "image lock cleanup fresh-lock assertion removed",
            "devcontainer_image_lock_selftest",
            "  fresh_lock_probe || cleanup_failed=1",
            "  true",
        ),
        (
            "image lock cleanup final group-gone assertion removed",
            "devcontainer_image_lock_selftest",
            '    bounded_group_gone "$SELFTEST_WORKER_PGID" || cleanup_failed=1',
            "    true",
        ),
        (
            "image lock cleanup descendant-absence assertion removed",
            "devcontainer_image_lock_selftest",
            '    bounded_process_absent "$pid" || return 1',
            "    true",
        ),
    )


def _image_lock_selftest_trap_mutations() -> tuple[Mutation, ...]:
    """Return signal and trap-lifecycle mutations."""
    return (
        (
            "image lock HUP cleanup trap removed",
            "devcontainer_image_lock_selftest",
            "  trap 'image_lock_case_signal 129' HUP",
            "  trap ':' HUP",
        ),
        (
            "image lock INT cleanup trap removed",
            "devcontainer_image_lock_selftest",
            "  trap 'image_lock_case_signal 130' INT",
            "  trap ':' INT",
        ),
        (
            "image lock TERM cleanup trap removed",
            "devcontainer_image_lock_selftest",
            "  trap 'image_lock_case_signal 143' TERM",
            "  trap ':' TERM",
        ),
        (
            "image lock cleanup trap clear removed",
            "devcontainer_image_lock_selftest",
            "clear_image_lock_case_traps() {\n  restore_selftest_root_traps",
            "clear_image_lock_case_traps() {\n  true",
        ),
    )


def _role_entry_mutations() -> tuple[Mutation, ...]:
    """Return independently selectable role and workflow mutations."""
    return (
        (
            "dev role prefix guard removed",
            "dev_main",
            "Authenticate the HIL and bench mutation boundary before this role",
            "Skip the HIL and bench mutation boundary before this role",
        ),
        (
            "direct HIL task guard removed",
            "dev_role",
            "Re-authenticate the mutation boundary for direct HIL task inclusion",
            "Skip the mutation boundary for direct HIL task inclusion",
        ),
        (
            "direct bench role guard removed",
            "bench_role",
            "Authenticate the whole-bench transaction before this role",
            "Skip the whole-bench transaction before this role",
        ),
        (
            "direct C6 role guard removed",
            "c6_role",
            "Authenticate the whole-bench transaction before the C6 role",
            "Skip the whole-bench transaction before the C6 role",
        ),
        (
            "direct AD2 role guard removed",
            "ad2_role",
            "Authenticate the whole-bench transaction before the AD2 role",
            "Skip the whole-bench transaction before the AD2 role",
        ),
        (
            "dev play start-at-task front door bypass",
            "dev_playbook",
            "when: dev_box_hil_mutation_authenticated | default(false) | bool",
            "when: true",
        ),
        (
            "bench play start-at-task front door bypass",
            "bench_playbook",
            "- name: Converge the bench host beneath its authenticated front door\n"
            "      when: hil_bench_transaction_authenticated | default(false) | bool",
            "- name: Converge the bench host beneath its authenticated front door\n"
            "      when: true",
        ),
        (
            "default nproc escape",
            "workflow",
            "RA8_MAX_JOBS: 4",
            "RA8_MAX_JOBS: ${{ nproc }}",
        ),
        (
            "parallelism drift",
            "workflow",
            "CMAKE_BUILD_PARALLEL_LEVEL: 4",
            "CMAKE_BUILD_PARALLEL_LEVEL: 8",
        ),
    )


def _transaction_entry_mutations() -> tuple[Mutation, ...]:
    """Return dynamic transaction-entry mutations for direct selector attacks."""
    return (
        (
            "dev role dynamic transaction entry replaced by static import",
            "dev_main_entry",
            "ansible.builtin.include_tasks:",
            "ansible.builtin.import_tasks:",
        ),
        (
            "dev dynamic transaction entry replaced by static import",
            "dev_entry",
            "ansible.builtin.include_tasks:",
            "ansible.builtin.import_tasks:",
        ),
        (
            "bench dynamic transaction entry replaced by static import",
            "bench_entry",
            "ansible.builtin.include_tasks:",
            "ansible.builtin.import_tasks:",
        ),
        (
            "C6 dynamic transaction entry replaced by static import",
            "c6_entry",
            "ansible.builtin.include_tasks:",
            "ansible.builtin.import_tasks:",
        ),
        (
            "AD2 dynamic transaction entry replaced by static import",
            "ad2_entry",
            "ansible.builtin.include_tasks:",
            "ansible.builtin.import_tasks:",
        ),
    )


def mutations() -> tuple[Mutation, ...]:
    """Return every independent must-fire mutation."""
    return (
        *_helper_mutations(),
        *_runner_dispatch_mutations(),
        *_bench_boundary_mutations(),
        *_runner_boundary_mutations(),
        *_wsl_mode_mutations(),
        *_wsl_transport_mutations(),
        *_wsl_durability_mutations(),
        *_role_decision_mutations(),
        *_context_authority_mutations(),
        *_role_entry_mutations(),
        *_transaction_entry_mutations(),
    )


def reorders() -> tuple[Reorder, ...]:
    """Return task reorderings that must violate the HIL convergence boundary."""
    return (
        (
            "managed image lock directory creation moved before refusal",
            "Create the managed image lock directory",
            "Refuse an unsafe managed image lock directory",
        ),
        (
            "managed image lock file creation moved before refusal",
            "Create the stable managed image lock file",
            "Refuse an unsafe managed image lock file",
        ),
        (
            "managed image lock marker creation moved before refusal",
            "Create the managed image lock numeric group marker atomically",
            "Refuse an unsafe managed image lock group marker",
        ),
        (
            "managed image lock file creation moved before directory refusal",
            "Create the stable managed image lock file",
            "Refuse an unsafe managed image lock directory",
        ),
        (
            "managed image lock marker creation moved before directory refusal",
            "Create the managed image lock numeric group marker atomically",
            "Refuse an unsafe managed image lock directory",
        ),
        (
            "managed image lock directory creation moved before file refusal",
            "Create the managed image lock directory",
            "Refuse an unsafe managed image lock file",
        ),
        (
            "managed image lock marker creation moved before file refusal",
            "Create the managed image lock numeric group marker atomically",
            "Refuse an unsafe managed image lock file",
        ),
        (
            "managed image lock directory creation moved before marker refusal",
            "Create the managed image lock directory",
            "Refuse an unsafe managed image lock group marker",
        ),
        (
            "managed image lock file creation moved before marker refusal",
            "Create the stable managed image lock file",
            "Refuse an unsafe managed image lock group marker",
        ),
        (
            "managed image lock group refusal moved after directory creation",
            "Create the managed image lock directory",
            "Refuse an unsafe managed image lock numeric primary group",
        ),
        (
            "real image build moved before its staleness selftest",
            "Build the gate image unless the cached one matches this context",
            "Prove the staleness check itself works, before trusting its verdict",
        ),
    )

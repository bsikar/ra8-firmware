# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Process-authority mutation catalog for HIL convergence selftests."""

from __future__ import annotations

import hil_convergence_safety_process_source_fixtures as process_source_fixtures
import hil_convergence_safety_runtime_fixtures as runtime_fixtures
import hil_convergence_safety_source_fixtures as source_fixtures

Mutation = tuple[str, str, str, str]

_BOUND_EXIT_MUTATION_LABELS = frozenset(
    {
        "descriptor-bound wrong entry refusal removed",
        "descriptor-bound canonical entry proof removed",
        "parent-death watchdog selftest removed",
        "live-supervisor watchdog deadline selftest removed",
        "closed death descriptor selftest removed",
        "bound-exit supervisor failure cases removed",
        "stall fixture descendant marker removed",
        "hardlink publication group proof removed",
        "missing payload entry proof removed",
        "nested phase parent identity binding removed",
        "nested phase root receipt binding removed",
        "supervisor failure payload regressed to full entry",
        "supervisor launcher interpreter changed",
        "supervisor launcher no-bytecode flag removed",
        "supervisor launcher isolation flag removed",
        "supervisor launcher site-import refusal removed",
        "supervisor launcher bound program removed",
        "supervisor launcher bound entry environment removed",
        "supervisor launcher process descriptor option removed",
        "supervisor launcher process descriptor changed",
        "supervisor launcher cases descriptor option removed",
        "supervisor launcher cases descriptor changed",
    }
)


def _process_authority_mutations_1() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "case signal dispatch image lock load removed",
            "devcontainer_image",
            "\n        load_image_lock_selftest\n        selftest_case_signal_child ",
            "\n        selftest_case_signal_child ",
        ),
        (
            "image lock jobs-table lookup bypassed",
            "devcontainer_image_selftest",
            "  done < <(jobs -r -l)",
            "  done < <(printf '[1] 1 Running\\n')",
        ),
        (
            "descriptor-bound wrong entry refusal removed",
            "devcontainer_image_selftest",
            '  if output="$(RA8_SELFTEST_BOUND_ENTRY="$wrong_entry" \\\n',
            '  if output="$(RA8_SELFTEST_BOUND_ENTRY="" \\\n',
        ),
        (
            "descriptor-bound canonical entry proof removed",
            "devcontainer_image_selftest",
            '  output="$(RA8_SELFTEST_BOUND_ENTRY="$SCRIPT_DIR/devcontainer_image.sh" \\\n',
            '  output="$(RA8_SELFTEST_BOUND_ENTRY="" \\\n',
        ),
        (
            "group selection explicit success removed",
            "devcontainer_image_selftest",
            (
                '  select_selftest_group_id "$tmp" "$rejected" >/dev/null 2>&1 && return 1\n'
                "  done\n  return 0"
            ),
            ('  select_selftest_group_id "$tmp" "$rejected" >/dev/null 2>&1 && return 1\n  done'),
        ),
    )


def _process_authority_mutations_1b() -> tuple[Mutation, ...]:
    """Return the second bounded group of process-authority mutations."""
    return (
        (
            "image lock verified PID signal reverted to stale jobspec",
            "devcontainer_image_selftest",
            '  builtin kill -"$signal" "$child"',
            '  builtin kill -"$signal" "$job_spec"',
        ),
        (
            "signal controller PID local renamed to controller",
            "devcontainer_image_signal_selftest",
            (
                '  local launcher_mode="${5:-}" controller_pid pending '
                'ready="$3/controller-launcher.ready"'
            ),
            (
                '  local launcher_mode="${5:-}" controller pending '
                'ready="$3/controller-launcher.ready"'
            ),
        ),
        (
            "image lock controller group signal reduced to direct PID",
            "devcontainer_image_signal_selftest",
            '    signal_owned_controller_group "$signal" "$controller" ||',
            '    signal_owned_live_child "$signal" "$controller" ||',
        ),
        (
            "image lock controller group authorization removed",
            "devcontainer_image_signal_selftest",
            '  controller_group_signal_is_authorized "$controller" || return 1',
            "  true",
        ),
        (
            "image lock controller group target reduced to PID",
            "devcontainer_image_signal_selftest",
            '  builtin kill -"$signal" -- "-$controller"',
            '  builtin kill -"$signal" -- "$controller"',
        ),
        (
            "allocation KILL direct-child guard removed",
            "devcontainer_image_selftest_cases",
            '  signal_owned_live_child KILL "$child" ||',
            "  true ||",
        ),
        (
            "suite-root nested allocation selection removed",
            "devcontainer_image_selftest",
            (
                '  if [[ -n "$SELFTEST_SUITE_ROOT" ]]; then\n    selftest_suite_'
                "root_is_safe || return 1"
            ),
            "  if false; then\n    true",
        ),
    )


def _process_authority_mutations_2() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "suite-root parent grammar reverted to ten characters",
            "devcontainer_image_selftest",
            '    "$suffix" =~ ^[0-9a-f]{32}$ && ! -L "$canonical" && -d "$canonical" &&',
            '    "$suffix" =~ ^[[:alnum:]]{10}$ && ! -L "$canonical" && -d "$canonical" &&',
        ),
        (
            "portable Bash 3 shell identity replaced by BASHPID",
            "devcontainer_image_selftest",
            '  local destination="$1" value="$$:${BASH_SUBSHELL:-0}"',
            '  local destination="$1" value="$BASHPID"',
        ),
        (
            "fresh allocation signal process bypassed",
            "devcontainer_image_selftest_cases",
            (
                '  if /bin/bash -p -- "$SCRIPT_DIR/devcontainer_image.sh" \\\n   '
                ' --selftest-allocation-checkpoint-child "$phase" "$receipt" \\\n'
            ),
            "  if (\n",
        ),
        (
            "fresh case signal process bypassed",
            "devcontainer_image_signal_selftest",
            (
                '  if /bin/bash -p -- "$SCRIPT_DIR/devcontainer_image.sh" \\\n   '
                ' --selftest-case-signal-child "$signal" "$tmp" "$SELFTEST_TMP_'
                'IDENTITY" \\\n'
            ),
            "  if (\n",
        ),
    )


def _process_authority_mutations_3() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "selftest atomic directory allocation replaced by mktemp create",
            "devcontainer_image_selftest",
            '    if (umask 077 && mkdir -m 0700 -- "$candidate"); then',
            ('    if SELFTEST_TMP_DIR="$(mktemp -d "$SELFTEST_TMP_ROOT/unsafe.XXXXXXXXXX")"; then'),
        ),
        (
            "selftest suite-root binding removed",
            "devcontainer_image_selftest_cases",
            '  establish_selftest_suite_root || die "selftest: could not bind its suite root"',
            "  true",
        ),
        (
            "selftest suite-root completion proof removed",
            "devcontainer_image_selftest_cases",
            '  clear_selftest_suite_root || die "selftest: suite-root cleanup did not complete"',
            "  true",
        ),
        (
            "worker group direct-child authority removed",
            "devcontainer_image_lock_selftest",
            (
                '  worker_group_is_safe && [[ "$SELFTEST_WORKER_PGID" == "$SELF'
                'TEST_WORKER_PID" ]] &&\n    shell_owns_live_child "$SELFTEST_WO'
                'RKER_PID"'
            ),
            "  worker_group_is_safe",
        ),
    )


def _process_authority_mutations_4() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "shared worker process-group binding removed",
            "devcontainer_image_lock_selftest",
            '    [[ "$pgid" == "$PPID" && "$pgid" != "$pid" ]] ||',
            '    [[ "$pgid" == "$pid" ]] ||',
        ),
        (
            "isolated worker process-group binding removed",
            "devcontainer_image_lock_selftest",
            (
                '    [[ "$pgid" == "$pid" ]] || die "selftest isolated worker i'
                's not its group leader"'
            ),
            "    true",
        ),
        (
            "worker leader TERM resistance removed",
            "devcontainer_image_lock_selftest",
            (
                "  pre-ready-hang | signal-controller | post-ready-build-hang"
                ") trap '' HUP INT TERM ;;"
            ),
            "  pre-ready-hang | signal-controller | post-ready-build-hang) true ;;",
        ),
        (
            "process-enumeration failure scenario removed",
            "devcontainer_image_selftest_cases",
            '  selftest_ps_failure_cleanup "$tmp"',
            "      true",
        ),
    )


def _process_authority_mutations_5() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "rebound process-group refusal removed",
            "devcontainer_image_lock_selftest",
            (
                '  ! worker_group_signal_is_authorized ||\n    die "selftest: re'
                'bound numeric process group gained signal authority"'
            ),
            "  true",
        ),
        (
            "process-enumeration descendant proof removed",
            "devcontainer_image_lock_selftest",
            (
                '  assert_no_surviving_descendants ||\n    die "selftest: repeat'
                'ed ps failure left a signal-ignoring descendant"'
            ),
            "  true",
        ),
        (
            "bound-exit supervisor pre-spawn signal block removed",
            "devcontainer_image_selftest_supervisor",
            "old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)",
            "old_mask = set()",
        ),
        (
            "bound-exit payload signal reset removed",
            "devcontainer_image_selftest_supervisor",
            (
                "        _reset_managed_signals()\n        try:\n            os.e"
                "xecl(  # noqa: S606 -- fixed Bash and descriptor-bound entry"
            ),
            (
                "        try:\n            os.execl(  # noqa: S606 -- fixed prot"
                "ected Bash and descriptor-bound entry"
            ),
        ),
    )


def _process_authority_mutations_6() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound-exit parent-death pipe removed",
            "devcontainer_image_selftest_process",
            "self.death_read, self.death_write = os.pipe2(os.O_CLOEXEC)",
            "self.death_read, self.death_write = (None, None)",
        ),
        (
            "bound-exit parent-death descriptor propagation removed",
            "devcontainer_image_selftest_process",
            "inherited = [source_descriptor, self.death_read, root_descriptor]",
            "inherited = [source_descriptor, root_descriptor]",
        ),
        (
            "bound-exit controller interpreter changed",
            "devcontainer_image_selftest_process",
            "        argv = (\n            sys.executable,",
            '        argv = (\n            os.environ["PYTHON"],',
        ),
        (
            "bound-exit controller no-bytecode isolation removed",
            "devcontainer_image_selftest_process",
            '        argv = (\n            sys.executable,\n            "-B",',
            '        argv = (\n            sys.executable,\n            "--version",',
        ),
    )


def _process_authority_mutations_7() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound-exit controller isolated-mode flag removed",
            "devcontainer_image_selftest_process",
            '        argv = (\n            sys.executable,\n            "-B",\n            "-I",',
            "        argv = (\n            sys.executable,\n"
            '            "-B",\n            "--version",',
        ),
        (
            "bound-exit controller site-import refusal removed",
            "devcontainer_image_selftest_process",
            '        argv = (\n            sys.executable,\n            "-B",\n'
            '            "-I",\n            "-S",',
            '        argv = (\n            sys.executable,\n            "-B",\n'
            '            "-I",\n            "--version",',
        ),
        (
            "bound-exit controller immutable helper path removed",
            "devcontainer_image_selftest_process",
            '        argv = (\n            sys.executable,\n            "-B",\n'
            '            "-I",\n            "-S",\n            SUPERVISOR_PROGRAM,\n'
            '            "--controller",',
            '        argv = (\n            sys.executable,\n            "-B",\n'
            '            "-I",\n            "-S",\n'
            '            "scripts/ci/devcontainer_image_selftest_supervisor.py",\n'
            '            "--controller",',
        ),
        (
            "bound-exit payload fixed Bash path removed",
            "devcontainer_image_selftest_supervisor",
            '                "/bin/bash",',
            '                "/usr/bin/env",',
        ),
    )


def _process_authority_mutations_8() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound-exit payload protected Bash mode removed",
            "devcontainer_image_selftest_supervisor",
            '                "-p",',
            '                "-c",',
        ),
        (
            "bound-exit parent-death polling removed",
            "devcontainer_image_selftest_supervisor",
            "ready, _, _ = select.select((death_descriptor,), (), (), POLL_SECONDS)",
            "ready = ()",
        ),
        (
            "bound-exit controller group cleanup reduced to controller PID",
            "devcontainer_image_selftest_supervisor",
            "os.killpg(os.getpgrp(), signal.SIGKILL)",
            "os.kill(os.getpid(), signal.SIGKILL)",
        ),
        (
            "bound-exit terminal payload is polled after reap",
            "devcontainer_image_selftest_supervisor",
            "            if not published:\n                child_status = _poll_payload(child)",
            "            if True:\n                child_status = _poll_payload(child)",
        ),
    )


def _process_authority_mutations_9() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound-exit controller liveness proof removed",
            "devcontainer_image_selftest_supervisor",
            '        supervisor.require_running("after publishing status")',
            "        pass",
        ),
        (
            "bound receipt hardlink refusal removed",
            "devcontainer_image_selftest_supervisor",
            "            or metadata.st_nlink != 1",
            "            or False",
        ),
        (
            "bound receipt owner binding removed",
            "devcontainer_image_selftest_supervisor",
            "            or metadata.st_uid != os.getuid()",
            "            or False",
        ),
        (
            "bound receipt mode binding removed",
            "devcontainer_image_selftest_supervisor",
            "            or stat.S_IMODE(metadata.st_mode) != RECEIPT_MODE",
            "            or False",
        ),
    )


def _process_authority_mutations_10() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound receipt truncation removed",
            "devcontainer_image_selftest_supervisor",
            "        os.ftruncate(descriptor, 0)",
            "        pass",
        ),
        (
            "status receipt no-follow descriptor removed",
            "devcontainer_image_selftest_supervisor",
            "            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)",
            "            descriptor = os.open(path, os.O_RDONLY)",
        ),
        (
            "process stat bytes parser replaced with text decoding",
            "devcontainer_image_selftest_process",
            '(entry / "stat").read_bytes()',
            '(entry / "stat").read_text(encoding="ascii").encode("ascii")',
        ),
        (
            "emergency cleanup process identity check removed",
            "devcontainer_image_selftest_supervisor_cases",
            "    if not _identity_is_current(authority):\n        return False",
            "    if False:\n        return False",
        ),
    )


def _process_authority_mutations_11() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "parent-death watchdog selftest removed",
            "devcontainer_image_selftest",
            (
                '  run_bound_exit_supervisor --selftest-parent-death "$stall_entr'
                'y" "$tmp" \\\n    "$SELFTEST_TMP_IDENTITY" || {'
            ),
            "  true || {",
        ),
        (
            "live-supervisor watchdog deadline selftest removed",
            "devcontainer_image_selftest",
            (
                '  run_bound_exit_supervisor --selftest-watchdog-expiry "$stall_'
                'entry" "$tmp" \\\n    "$SELFTEST_TMP_IDENTITY" || {'
            ),
            "  true || {",
        ),
        (
            "closed death descriptor selftest removed",
            "devcontainer_image_selftest",
            (
                '  run_bound_exit_supervisor --selftest-closed-death-fd "$stall_'
                'entry" "$tmp" \\\n    "$SELFTEST_TMP_IDENTITY" || {'
            ),
            "  true || {",
        ),
        (
            "controller close failure bypasses group KILL",
            "devcontainer_image_selftest_supervisor",
            (
                "        for private_descriptor in (death_descriptor, root_descriptor):\n"
                "            with suppress(OSError):\n"
                "                os.close(private_descriptor)\n"
                "        os.killpg(os.getpgrp(), signal.SIGKILL)"
            ),
            (
                "        for private_descriptor in (death_descriptor, root_descriptor):\n"
                "            os.close(private_descriptor)\n"
                "        os.killpg(os.getpgrp(), signal.SIGKILL)"
            ),
        ),
    )


def _process_authority_mutations_12() -> tuple[Mutation, ...]:
    """Return one bounded group of process-authority mutations."""
    return (
        (
            "bound-exit group cleanup reduced to leader PID",
            "devcontainer_image_selftest_process",
            "os.killpg(leader, signal.SIGKILL)",
            "os.kill(leader, signal.SIGKILL)",
        ),
        (
            "bound-exit cleanup signal block moved after authority checks",
            "devcontainer_image_selftest_process",
            (
                "        signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNA"
                "LS)\n        if self.authority_lost:"
            ),
            (
                "        if self.authority_lost:\n            signal"
                ".pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)"
            ),
        ),
        (
            "bound-exit status atomic publication removed",
            "devcontainer_image_selftest_supervisor",
            "os.link(temporary, path, follow_symlinks=False)",
            'path.write_text(value, encoding="ascii")',
        ),
        (
            "bound-exit supervisor failure cases removed",
            "devcontainer_image_selftest",
            "selftest_bound_exit_supervisor_failures() {",
            "selftest_bound_exit_supervisor_failures_disabled() {",
        ),
    )


def _process_authority_mutations_13() -> tuple[Mutation, ...]:
    """Return the hidden supervisor runtime-proof mutations."""
    return (
        (
            "closed death descriptor source fd propagation removed",
            "devcontainer_image_selftest_supervisor_cases",
            "    inherited = [source_descriptor, root_descriptor]",
            "    inherited = [root_descriptor]",
        ),
        (
            "closed death descriptor expected KILL status weakened",
            "devcontainer_image_selftest_supervisor_cases",
            "return 0 if observed and cleaned and child.returncode == -signal.SIGKILL else 1",
            "return 0 if observed and cleaned and child.returncode is not None else 1",
        ),
        (
            "watchdog expiry pre-release proof removed",
            "devcontainer_image_selftest_supervisor_cases",
            (
                "            pre_release_proven = killed_receipt and members is not None\n"
                "            pre_release_proven = pre_release_proven and members <= {authority.pid}"
            ),
            "            pre_release_proven = True",
        ),
        (
            "watchdog expiry test deadline extended",
            "devcontainer_image_selftest_supervisor_cases",
            "launch = ControllerLaunch(entry, status, SELFTEST_WATCHDOG_TIMEOUT_SECONDS)",
            "launch = ControllerLaunch(entry, status, WATCHDOG_TIMEOUT_SECONDS)",
        ),
        (
            "hardlink publication preservation proof removed",
            "devcontainer_image_selftest_supervisor_cases",
            (
                'preserved = victim.read_bytes() == b"preserve\\n" and '
                "victim.stat().st_nlink == HARDLINK_COUNT"
            ),
            "preserved = True",
        ),
        (
            "missing payload exec status weakened",
            "devcontainer_image_selftest_supervisor",
            "        except OSError:\n            os._exit(127)",
            "        except OSError:\n            os._exit(1)",
        ),
    )


def _process_authority_mutations_15() -> tuple[Mutation, ...]:
    """Return public Bash dispatch mutations after the supervisor split."""
    return (
        (
            "hardlink publication group proof removed",
            "devcontainer_image_selftest",
            (
                '  run_bound_exit_supervisor --selftest-hardlink-bound "$stall_'
                'entry" "$tmp" \\\n    "$SELFTEST_TMP_IDENTITY" || {'
            ),
            "  true || {",
        ),
        (
            "missing payload entry proof removed",
            "devcontainer_image_selftest",
            (
                '  run_bound_exit_supervisor --selftest-missing-entry "$tmp" '
                '"$SELFTEST_TMP_IDENTITY" || {'
            ),
            "  true || {",
        ),
    )


def _process_authority_mutations_14() -> tuple[Mutation, ...]:
    """Return the watchdog observation and runner-lifetime mutations."""
    return (
        (
            "watchdog exact KILL receipt weakened",
            "devcontainer_image_selftest_supervisor_cases",
            "killed = result.si_code == os.CLD_KILLED and result.si_status == signal.SIGKILL",
            "killed = result is not None",
        ),
        (
            "watchdog runner liveness proof removed",
            "devcontainer_image_selftest_supervisor_cases",
            "if pre_release_proven and runner_is_live:",
            "if pre_release_proven:",
        ),
        (
            "watchdog post-release group proof removed",
            "devcontainer_image_selftest_supervisor_cases",
            "watchdog_succeeded = expected and _wait_group_gone(authority.group)",
            "watchdog_succeeded = expected",
        ),
        (
            "watchdog post-reap PID guard conflated with expected status",
            "devcontainer_image_selftest_supervisor_cases",
            (
                "                runner_status = _wait_direct_child_status(runner)\n"
                "                runner_reaped = runner_status is not None\n"
                "                expected = runner_status == STALL_STATUS"
            ),
            (
                "                runner_status = _wait_direct_child_status(runner)\n"
                "                runner_reaped = runner_status == STALL_STATUS\n"
                "                expected = runner_status == STALL_STATUS"
            ),
        ),
        (
            "hardlink runner post-reap signal guard removed",
            "devcontainer_image_selftest_supervisor_cases",
            (
                "        if not hardlink_runner_reaped:\n"
                "            with suppress(ProcessLookupError):"
            ),
            "        if True:\n            with suppress(ProcessLookupError):",
        ),
        (
            "stall fixture descendant marker removed",
            "devcontainer_image_selftest",
            '    "exec -a \\"\\$0\\" /bin/sleep 30" >"$destination") || return 1',
            '    "/bin/sleep 30" >"$destination") || return 1',
        ),
    )


def _main_descriptor_mutations() -> tuple[Mutation, ...]:
    """Return mutations for descriptor-bound main-script path authority."""
    return (
        (
            "main descriptor basename predicate removed",
            "devcontainer_image",
            '      "$ra8_bound_entry" == /*/devcontainer_image.sh &&\n',
            "      true &&\n",
        ),
        (
            "main descriptor file and link predicate removed",
            "devcontainer_image",
            '      -f "$ra8_bound_entry" && ! -L "$ra8_bound_entry" &&\n',
            "      true &&\n",
        ),
        (
            "main descriptor canonical path predicate removed",
            "devcontainer_image",
            '    [[ "$ra8_bound_entry" == "$SCRIPT_DIR/devcontainer_image.sh" ]] || {\n',
            "    false || {\n",
        ),
    )


def _helper_parent_mutations() -> tuple[Mutation, ...]:
    """Return mutations for canonical helper-parent path bindings."""
    specifications = (
        ("lifecycle", "devcontainer_image_selftest", "SELFTEST_HELPER_PARENT_DIR"),
        ("cases", "devcontainer_image_selftest_cases", "SELFTEST_CASES_PARENT_DIR"),
        ("signal", "devcontainer_image_signal_selftest", "SELFTEST_SIGNAL_PARENT_DIR"),
        ("lock", "devcontainer_image_lock_selftest", "SELFTEST_LOCK_HELPER_PARENT_DIR"),
    )
    return tuple(
        (
            f"{label} helper canonical parent proof removed",
            key,
            (
                '  "${DEVCONTAINER_SELFTEST_PARENT:-}" == '
                f'"${variable}/devcontainer_image.sh" &&\n'
            ),
            (f'  "${{DEVCONTAINER_SELFTEST_PARENT:-}}" == "${variable}/not-main.sh" &&\n'),
        )
        for label, key, variable in specifications
    )


def _entry_descriptor_mutations() -> tuple[Mutation, ...]:
    """Return mutations for the reserved descriptor execution namespace."""
    key = "devcontainer_image_selftest_process"
    return (
        (
            "entry descriptor floor lowered into helper range",
            key,
            "ENTRY_EXEC_DESCRIPTOR_MINIMUM = 64",
            "ENTRY_EXEC_DESCRIPTOR_MINIMUM = 8",
        ),
        (
            "entry descriptor reservation call removed",
            key,
            "            descriptor = _reserve_entry_descriptor(descriptor)",
            "            descriptor = descriptor",
        ),
        (
            "entry descriptor propagation removed",
            key,
            "            inherited.append(self.entry_descriptor)",
            "            pass",
        ),
        (
            "entry descriptor high-FD duplication removed",
            key,
            (
                "        reserved = fcntl.fcntl(\n"
                "            descriptor,\n"
                "            fcntl.F_DUPFD_CLOEXEC,\n"
                "            ENTRY_EXEC_DESCRIPTOR_MINIMUM,\n"
                "        )"
            ),
            "        reserved = os.dup(descriptor)",
        ),
        (
            "entry descriptor original close removed",
            key,
            "        os.close(descriptor)",
            "        pass",
        ),
        (
            "entry descriptor CLOEXEC readback removed",
            key,
            "        descriptor_flags = fcntl.fcntl(reserved, fcntl.F_GETFD)",
            "        descriptor_flags = fcntl.FD_CLOEXEC",
        ),
    )


def _entry_descriptor_predicate_mutations() -> tuple[Mutation, ...]:
    """Return the two independent reserved-descriptor predicate mutations."""
    key = "devcontainer_image_selftest_process"
    return (
        (
            "entry descriptor reservation bound removed",
            key,
            "    if reserved < ENTRY_EXEC_DESCRIPTOR_MINIMUM or not (",
            "    if not (",
        ),
        (
            "entry descriptor CLOEXEC predicate removed",
            key,
            (
                "    if reserved < ENTRY_EXEC_DESCRIPTOR_MINIMUM or not "
                "(descriptor_flags & fcntl.FD_CLOEXEC):"
            ),
            ("    if reserved < ENTRY_EXEC_DESCRIPTOR_MINIMUM or not (descriptor_flags >= 0):"),
        ),
    )


def _tmp_root_mutations() -> tuple[Mutation, ...]:
    """Return mutations for canonical and nested allocation-parent proofs."""
    key = "devcontainer_image_selftest_cases"
    return (
        (
            "allocation parent proof call removed",
            key,
            '  selftest_temp_root_is_safe || die "selftest: allocation parent authority is unsafe"',
            "  true",
        ),
        (
            "canonical tmp special-mode proof removed",
            key,
            '    "$(file_special_mode "$SELFTEST_TMP_ROOT")" == "1777" ]]',
            '    -d "$SELFTEST_TMP_ROOT" ]]',
        ),
        (
            "nested suite-root safety proof removed",
            key,
            (
                '    "$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] &&\n'
                "    selftest_suite_root_is_safe"
            ),
            (
                '    "$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] &&\n'
                "    true"
            ),
        ),
    )


def _suite_anchor_validation_mutations() -> tuple[Mutation, ...]:
    """Bind suite-anchor path, identity, owner, mode, and depth validation."""
    lifecycle = "devcontainer_image_selftest"
    return (
        (
            "suite anchor canonical path binding removed",
            lifecycle,
            '    "$anchor" == "$canonical/ra8-devcontainer-image-selftest.$suffix" &&',
            "    true &&",
        ),
        (
            "suite anchor identity binding removed",
            lifecycle,
            '    "$(file_identity "$anchor")" == "$SELFTEST_SUITE_ANCHOR_IDENTITY" &&',
            "    true &&",
        ),
        (
            "suite anchor owner binding removed",
            lifecycle,
            '    "$(file_owner_id "$anchor")" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &&',
            "    true &&",
        ),
        (
            "suite anchor mode binding removed",
            lifecycle,
            '    "$(file_mode "$anchor")" == "700" ]]',
            "    true ]]",
        ),
        (
            "suite anchor child depth binding removed",
            lifecycle,
            '  [[ "$suite" == "$SELFTEST_SUITE_ANCHOR/ra8-devcontainer-image-selftest.$suffix" &&',
            "  [[ true &&",
        ),
    )


def _suite_anchor_flow_mutations() -> tuple[Mutation, ...]:
    """Bind suite-anchor production, dispatch, and receipt authority."""
    lifecycle = "devcontainer_image_selftest"
    cases = "devcontainer_image_selftest_cases"
    return (
        (
            "suite anchor producer path removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR="$SELFTEST_TMP_DIR"',
            '  SELFTEST_SUITE_ANCHOR=""',
        ),
        (
            "suite anchor producer identity removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR_IDENTITY="$SELFTEST_TMP_IDENTITY"',
            '  SELFTEST_SUITE_ANCHOR_IDENTITY=""',
        ),
        (
            "suite anchor producer owner removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR_OWNER_UID="$SELFTEST_TMP_OWNER_UID"',
            '  SELFTEST_SUITE_ANCHOR_OWNER_UID=""',
        ),
        (
            "suite anchor receiver path removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR="$3"',
            '  SELFTEST_SUITE_ANCHOR=""',
        ),
        (
            "suite anchor receiver identity removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR_IDENTITY="$4"',
            '  SELFTEST_SUITE_ANCHOR_IDENTITY=""',
        ),
        (
            "suite anchor receiver owner removed",
            lifecycle,
            '  SELFTEST_SUITE_ANCHOR_OWNER_UID="$5"',
            '  SELFTEST_SUITE_ANCHOR_OWNER_UID=""',
        ),
        (
            "suite anchor dispatcher propagation removed",
            cases,
            '    "$tmp" "$SELFTEST_TMP_IDENTITY" "$SELFTEST_SUITE_ANCHOR" \\\n'
            '    "$SELFTEST_SUITE_ANCHOR_IDENTITY" "$SELFTEST_SUITE_ANCHOR_OWNER_UID"; then',
            '    "$tmp" "$SELFTEST_TMP_IDENTITY" "" "" ""; then',
        ),
    )


def _process_authority_mutations() -> tuple[Mutation, ...]:
    """Return the unassigned image-lock process-authority mutation catalog."""
    return (
        *_process_authority_mutations_1(),
        *_process_authority_mutations_1b(),
        *_process_authority_mutations_2(),
        *_process_authority_mutations_3(),
        *_process_authority_mutations_4(),
        *_process_authority_mutations_5(),
        *_process_authority_mutations_6(),
        *_process_authority_mutations_7(),
        *_process_authority_mutations_8(),
        *_process_authority_mutations_9(),
        *_process_authority_mutations_10(),
        *_process_authority_mutations_11(),
        *_process_authority_mutations_12(),
        *_process_authority_mutations_13(),
        *_process_authority_mutations_14(),
        *_process_authority_mutations_15(),
        *_main_descriptor_mutations(),
        *_helper_parent_mutations(),
        *_entry_descriptor_mutations(),
        *_entry_descriptor_predicate_mutations(),
        *_tmp_root_mutations(),
        *_suite_anchor_validation_mutations(),
        *_suite_anchor_flow_mutations(),
        *runtime_fixtures.process_authority_mutations(),
        *source_fixtures.process_authority_mutations(),
        *process_source_fixtures.process_authority_mutations(),
    )


def process_authority_mutations() -> tuple[Mutation, ...]:
    """Assign every mutation to the source module that owns its target bytes."""
    return tuple(
        (
            label,
            "devcontainer_image_bound_exit_selftest"
            if label in _BOUND_EXIT_MUTATION_LABELS
            else key,
            old,
            new,
        )
        for label, key, old, new in _process_authority_mutations()
    )

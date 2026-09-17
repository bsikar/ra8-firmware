# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Scoped process-authority policy for devcontainer image selftests."""

from __future__ import annotations

PROCESS_MODULE_SEMANTIC_TOKENS = {
    "entry descriptor floor lowered into helper range": "ENTRY_EXEC_DESCRIPTOR_MINIMUM = 64",
}

SUPERVISOR_SEMANTIC_PROCESS_TOKENS = {
    "bound-exit supervisor pre-spawn signal block removed": (
        "old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)"
    ),
    "bound-exit parent-death pipe removed": (
        "self.death_read, self.death_write = os.pipe2(os.O_CLOEXEC)"
    ),
    "bound-exit parent-death descriptor propagation removed": (
        "inherited = [source_descriptor, self.death_read, root_descriptor]"
    ),
    "bound-exit controller interpreter changed": "            sys.executable,",
    "bound-exit controller no-bytecode isolation removed": '            "-B",',
    "bound-exit controller isolated-mode flag removed": '            "-I",',
    "bound-exit controller site-import refusal removed": '            "-S",',
    "bound-exit controller immutable helper path removed": (
        '            __file__,\n            "--controller",'
    ),
    "bound-exit payload fixed Bash path removed": '                "/bin/bash",',
    "bound-exit payload protected Bash mode removed": '                "-p",',
    "bound-exit parent-death polling removed": (
        "ready, _, _ = select.select((death_descriptor,), (), (), POLL_SECONDS)"
    ),
    "bound-exit payload signal reset removed": (
        "        _reset_managed_signals()\n"
        "        try:\n"
        "            os.execl(  # noqa: S606 -- fixed Bash and descriptor-bound entry"
    ),
    "controller close failure bypasses group KILL": (
        "for private_descriptor in (death_descriptor, root_descriptor):\n"
        "            with suppress(OSError):\n"
        "                os.close(private_descriptor)\n"
        "        os.killpg(os.getpgrp(), signal.SIGKILL)"
    ),
    "bound-exit controller group cleanup reduced to controller PID": (
        "for private_descriptor in (death_descriptor, root_descriptor):\n"
        "            with suppress(OSError):\n"
        "                os.close(private_descriptor)\n"
        "        os.killpg(os.getpgrp(), signal.SIGKILL)"
    ),
    "bound-exit terminal payload is polled after reap": (
        "            if not published:\n                child_status = _poll_payload(child)"
    ),
    "bound-exit controller liveness proof removed": (
        '        supervisor.require_running("after publishing status")'
    ),
    "bound receipt hardlink refusal removed": "            or metadata.st_nlink != 1",
    "bound receipt owner binding removed": "            or metadata.st_uid != os.getuid()",
    "bound receipt mode binding removed": (
        "            or stat.S_IMODE(metadata.st_mode) != RECEIPT_MODE"
    ),
    "bound receipt truncation removed": "        os.ftruncate(descriptor, 0)",
    "receipt exact-write helper definition removed": "def _write_exact(",
    "exclusive receipt exact-write use removed": (
        "        _write_exact(descriptor, payload, ENTRY_MAX_BYTES)"
    ),
    "bound receipt exact-write use removed": (
        "        _write_exact(descriptor, payload, RECEIPT_MAX_BYTES)"
    ),
    "status receipt no-follow descriptor removed": (
        "            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)"
    ),
    "process stat bytes parser replaced with text decoding": '(entry / "stat").read_bytes()',
    "bound-exit group cleanup reduced to leader PID": "os.killpg(leader, signal.SIGKILL)",
    "bound-exit cleanup signal block moved after authority checks": (
        "        signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)\n"
        "        if self.pid is None or self.reaped:"
    ),
    "bound-exit status atomic publication removed": (
        "os.link(temporary, path, follow_symlinks=False)"
    ),
    "missing payload exec status weakened": "        except OSError:\n            os._exit(127)",
    "supervisor interruption-handler definition removed": (
        "def _install_interruption_handlers(supervisor: BoundGroup) -> None:"
    ),
    "supervisor interruption-handler call removed": "_install_interruption_handlers(supervisor)",
}
SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS = {
    "emergency cleanup process identity check removed": (
        "    if not _identity_is_current(authority):\n        return False"
    ),
    "supervisor cases source-only sentinel removed": (
        'globals().get("_RA8_SUPERVISOR_CASES_VERSION")'
    ),
    "closed death descriptor source fd propagation removed": (
        "inherited = [source_descriptor, root_descriptor]"
    ),
    "closed death descriptor expected KILL status weakened": (
        "return 0 if observed and cleaned and child.returncode == -signal.SIGKILL else 1"
    ),
    "watchdog expiry pre-release proof removed": (
        "pre_release_proven = killed_receipt and members is not None\n"
        "            pre_release_proven = pre_release_proven and members <= {authority.pid}"
    ),
    "watchdog expiry test deadline extended": (
        "launch = ControllerLaunch(entry, status, SELFTEST_WATCHDOG_TIMEOUT_SECONDS)"
    ),
    "hardlink publication preservation proof removed": (
        'preserved = victim.read_bytes() == b"preserve\\n" '
        "and victim.stat().st_nlink == HARDLINK_COUNT"
    ),
    "watchdog exact KILL receipt weakened": (
        "killed = result.si_code == os.CLD_KILLED and result.si_status == signal.SIGKILL"
    ),
    "watchdog runner liveness proof removed": "if pre_release_proven and runner_is_live:",
    "watchdog post-release group proof removed": (
        "watchdog_succeeded = expected and _wait_group_gone(authority.group)"
    ),
    "watchdog post-reap PID guard conflated with expected status": (
        "runner_status = _wait_direct_child_status(runner)\n"
        "                runner_reaped = runner_status is not None\n"
        "                expected = runner_status == STALL_STATUS"
    ),
    "hardlink runner post-reap signal guard removed": (
        "if not hardlink_runner_reaped:\n            with suppress(ProcessLookupError):"
    ),
    "closed-controller command definition removed": "def _closed_controller_command(",
    "closed-controller command call removed": (
        "command = _closed_controller_command(\n"
        "            entry_authority, status, death_descriptor, root_descriptor, "
        "root_identity\n"
        "        )\n"
        "        child = subprocess.Popen("
    ),
}


def _scoped_specs(
    function: str, kind: str, entries: tuple[tuple[str, str], ...]
) -> dict[str, tuple[str, str, str]]:
    """Bind compact label/token data to one owning function."""
    return {label: (function, kind, token) for label, token in entries}


SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS = {
    **_scoped_specs(
        "_closed_controller_command",
        "command construction",
        (
            ("closed-controller interpreter changed", "sys.executable,"),
            ("closed-controller no-bytecode flag removed", '"-B",'),
            ("closed-controller isolation flag removed", '"-I",'),
            ("closed-controller site-import refusal removed", '"-S",'),
            ("closed-controller supervisor authority removed", "SUPERVISOR_PROGRAM,"),
            ("closed-controller mode removed", '"--controller",'),
        ),
    ),
    **_scoped_specs(
        "_closed_controller_descriptor_selftest",
        "Popen authority",
        (
            (
                "closed-controller subreaper enable removed",
                "if not supervisor.enable_subreaper():",
            ),
            (
                "closed-controller Popen descriptor propagation removed",
                "pass_fds=tuple(inherited),",
            ),
            (
                "closed-controller Popen session isolation removed",
                "start_new_session=True,",
            ),
            (
                "closed-controller bound adoption removed",
                "supervisor.bind_spawned_child(child)",
            ),
        ),
    ),
    **_scoped_specs(
        "_watchdog_expiry_runner",
        "containment proof",
        (
            (
                "watchdog identity receipt exact-write removed",
                (
                    "        _write_exact(\n"
                    "            identity_descriptor,\n"
                    '            f"{supervisor.pid}\\n".encode("ascii"),\n'
                    "            RECEIPT_MAX_BYTES,\n"
                    "        )"
                ),
            ),
            (
                "watchdog pre-proof containment removed",
                "contained = supervisor.contain()",
            ),
            (
                "watchdog proof receipt exact-write removed",
                (
                    "        _write_exact(\n"
                    "            proof_descriptor,\n"
                    '            b"K\\n" if killed and contained else b"F\\n",\n'
                    "            RECEIPT_MAX_BYTES,\n"
                    "        )"
                ),
            ),
        ),
    ),
    **_scoped_specs(
        "_refused_controller_launch",
        "command construction",
        (
            ("refused-controller interpreter changed", "sys.executable,"),
            ("refused-controller no-bytecode flag removed", '"-B",'),
            ("refused-controller isolation flag removed", '"-I",'),
            ("refused-controller site-import refusal removed", '"-S",'),
            ("refused-controller supervisor authority removed", "SUPERVISOR_PROGRAM,"),
            ("refused-controller mode removed", '"--controller",'),
        ),
    ),
    **_scoped_specs(
        "_refused_controller_launch",
        "Popen authority",
        (
            (
                "refused-controller descriptor propagation removed",
                "pass_fds=(source_descriptor, root_descriptor),",
            ),
            ("refused-controller session isolation removed", "start_new_session=True,"),
        ),
    ),
}

SUPERVISOR_SCOPED_LOADER_TOKENS = {
    **_scoped_specs(
        "_load_cases_dispatch",
        "loader binding",
        (
            ("cases loader bound source read removed", "source = _read_cases_source(descriptor)"),
            ("cases loader pre-exec digest binding removed", "if digest != CASES_RAW_SHA256:"),
            ("cases loader namespace name binding removed", '"__name__": "_ra8_supervisor_cases",'),
            (
                "cases loader namespace filename binding removed",
                '"__file__": f"/proc/self/fd/{descriptor}",',
            ),
            ("cases loader grant version binding removed", '"_RA8_SUPERVISOR_CASES_VERSION": 1,'),
            (
                "cases loader authenticated exec removed",
                "exec(  # noqa: S102 -- exact digest-bound source-only FD",
            ),
            ("cases loader compile source binding removed", "compile(source,"),
            (
                "cases loader compile filename binding removed",
                'namespace["__file__"],',
            ),
            ("cases loader compile exec-mode binding removed", ', "exec")'),
            (
                "cases loader grant exact consumption removed",
                'grant = namespace.pop("_RA8_SUPERVISOR_CASES_VERSION", None)',
            ),
            (
                "cases loader grant absence postcondition removed",
                'if grant != 1 or "_RA8_SUPERVISOR_CASES_VERSION" in namespace:',
            ),
            (
                "cases loader post-exec same-FD digest removed",
                "if hashlib.sha256(_read_cases_source(descriptor)).hexdigest() != digest:",
            ),
            ("cases loader callable dispatch binding removed", "if not callable(dispatch):"),
            (
                "cases loader dispatch namespace identity removed",
                "if dispatch.__globals__ is not namespace:",
            ),
            (
                "cases loader descriptor final-close removed",
                "finally:\n        os.close(descriptor)",
            ),
        ),
    ),
    **_scoped_specs(
        "_read_cases_source",
        "source binding",
        (
            ("cases source metadata regular binding removed", "stat.S_ISREG(metadata.st_mode)"),
            ("cases source metadata link binding removed", "metadata.st_nlink == 1"),
            ("cases source metadata owner binding removed", "metadata.st_uid == os.getuid()"),
            ("cases source metadata group binding removed", "metadata.st_gid == os.getgid()"),
            (
                "cases source metadata mode binding removed",
                "stat.S_IMODE(metadata.st_mode) == CASES_MODE",
            ),
            ("cases source metadata byte bound removed", "0 < metadata.st_size <= CASES_MAX_BYTES"),
            ("cases source read step bound removed", "for _step in range(CASES_READ_STEPS):"),
            (
                "cases source descriptor pread binding removed",
                "chunk = os.pread(descriptor, 4096, offset)",
            ),
            (
                "cases source complete-size postcondition removed",
                "if len(source) != metadata.st_size or len(source) > CASES_MAX_BYTES:",
            ),
        ),
    ),
}

CROSS_LANGUAGE_SCOPED_TOKENS = (
    (
        "entry descriptor reservation call removed",
        "devcontainer_image_selftest_process",
        "BoundGroup.spawn",
        "descriptor = _reserve_entry_descriptor(descriptor)",
    ),
    (
        "entry descriptor propagation removed",
        "devcontainer_image_selftest_process",
        "BoundGroup.spawn",
        "inherited.append(self.entry_descriptor)",
    ),
    (
        "parent-death Popen descriptor propagation removed",
        "devcontainer_image_selftest_process",
        "BoundGroup.spawn",
        "pass_fds=tuple(inherited),",
    ),
    (
        "parent-death Popen session isolation removed",
        "devcontainer_image_selftest_process",
        "BoundGroup.spawn",
        "start_new_session=True,",
    ),
    (
        "entry descriptor high-FD duplication removed",
        "devcontainer_image_selftest_process",
        "_reserve_entry_descriptor",
        "fcntl.fcntl(\n"
        "            descriptor,\n"
        "            fcntl.F_DUPFD_CLOEXEC,\n"
        "            ENTRY_EXEC_DESCRIPTOR_MINIMUM,\n"
        "        )",
    ),
    (
        "entry descriptor original close removed",
        "devcontainer_image_selftest_process",
        "_reserve_entry_descriptor",
        "os.close(descriptor)",
    ),
    (
        "entry descriptor CLOEXEC readback removed",
        "devcontainer_image_selftest_process",
        "_reserve_entry_descriptor",
        "descriptor_flags = fcntl.fcntl(reserved, fcntl.F_GETFD)",
    ),
    (
        "entry descriptor reservation bound removed",
        "devcontainer_image_selftest_process",
        "_reserve_entry_descriptor",
        "if reserved < ENTRY_EXEC_DESCRIPTOR_MINIMUM or not (",
    ),
    (
        "entry descriptor CLOEXEC predicate removed",
        "devcontainer_image_selftest_process",
        "_reserve_entry_descriptor",
        "descriptor_flags & fcntl.FD_CLOEXEC",
    ),
    (
        "supervisor launcher bound entry environment removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        'RA8_SELFTEST_BOUND_ENTRY="$1"',
    ),
    (
        "supervisor launcher interpreter changed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        "/usr/bin/python3",
    ),
    (
        "supervisor launcher no-bytecode flag removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " -B ",
    ),
    (
        "supervisor launcher isolation flag removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " -I ",
    ),
    (
        "supervisor launcher site-import refusal removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " -S ",
    ),
    (
        "supervisor launcher bound program removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        '"$program"',
    ),
    (
        "supervisor launcher process descriptor option removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " --process-fd ",
    ),
    (
        "supervisor launcher process descriptor changed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " 9 ",
    ),
    (
        "supervisor launcher cases descriptor option removed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " --cases-fd ",
    ),
    (
        "supervisor launcher cases descriptor changed",
        "devcontainer_image_bound_exit_selftest",
        "run_bound_exit_supervisor",
        " 8 ",
    ),
    (
        "payload entry descriptor metadata binding removed",
        "devcontainer_image_selftest_supervisor",
        "_spawn_payload",
        "if not _entry_metadata_is_safe(metadata):",
    ),
    (
        "payload procfd path binding removed",
        "devcontainer_image_selftest_supervisor",
        "_spawn_payload",
        'entry = f"/proc/self/fd/{descriptor}"',
    ),
    (
        "payload post-exec pathname identity removed",
        "devcontainer_image_selftest_process",
        "BoundGroup._close_entry_authority",
        "and (path_metadata.st_dev, path_metadata.st_ino) == self.entry_identity",
    ),
    (
        "payload post-exec descriptor identity removed",
        "devcontainer_image_selftest_process",
        "BoundGroup._close_entry_authority",
        "and current_identity == self.entry_identity",
    ),
    (
        "payload post-exec digest proof removed",
        "devcontainer_image_selftest_process",
        "BoundGroup._close_entry_authority",
        "and current_digest == self.entry_digest",
    ),
    (
        "main suite-root canonical tmp changed",
        "devcontainer_image_selftest_supervisor",
        "<module>",
        '"/tmp"  # noqa: S108 -- fixed physical parent; random mode-0700 inode-bound direct child',
    ),
    (
        "main suite-root canonical parent removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_path_is_safe",
        "and root.parent == canonical",
    ),
    (
        "main suite-root direct resolution removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_path_is_safe",
        "and resolved == root",
    ),
    (
        "main suite-root suffix length removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_path_is_safe",
        "and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH",
    ),
    (
        "main suite-root hex suffix removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_path_is_safe",
        'and all(character in "0123456789abcdef" for character in suffix)',
    ),
    (
        "main suite-root directory type removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_metadata_is_safe",
        "stat.S_ISDIR(metadata.st_mode)",
    ),
    (
        "main suite-root owner removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_metadata_is_safe",
        "and metadata.st_uid == os.getuid()",
    ),
    (
        "main suite-root group removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_metadata_is_safe",
        "and metadata.st_gid == os.getgid()",
    ),
    (
        "main suite-root private mode removed",
        "devcontainer_image_selftest_supervisor",
        "_suite_root_metadata_is_safe",
        "and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE",
    ),
    (
        "main suite-root inode identity removed",
        "devcontainer_image_selftest_supervisor",
        "_open_suite_root_authority",
        "if not _suite_root_metadata_is_safe(after) or identity != (\n"
        "            before.st_dev,\n            before.st_ino,\n        ):",
    ),
    (
        "cases suite-root canonical tmp changed",
        "devcontainer_image_selftest_supervisor_cases",
        "<module>",
        '"/tmp"  # noqa: S108 -- fixed physical parent; random mode-0700 inode-bound direct child',
    ),
    (
        "cases suite-root canonical parent removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and root.parent == canonical",
    ),
    (
        "cases suite-root direct resolution removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and resolved == root",
    ),
    (
        "cases suite-root suffix length removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH",
    ),
    (
        "cases suite-root hex suffix removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        'and all(character in "0123456789abcdef" for character in suffix)',
    ),
    (
        "cases suite-root directory type removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and stat.S_ISDIR(metadata.st_mode)",
    ),
    (
        "cases suite-root owner removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and metadata.st_uid == os.getuid()",
    ),
    (
        "cases suite-root group removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and metadata.st_gid == os.getgid()",
    ),
    (
        "cases suite-root private mode removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE",
    ),
    (
        "cases suite-root inode identity removed",
        "devcontainer_image_selftest_supervisor_cases",
        "_suite_root_is_safe",
        "and identity == expected_identity",
    ),
    (
        "spawn handler name grammar removed",
        "devcontainer_image_selftest",
        "begin_selftest_spawn_critical",
        '[[ "$handler" =~ ^[a-z_][a-z0-9_]*$ ]]',
    ),
    (
        "spawn handler function binding removed",
        "devcontainer_image_selftest",
        "begin_selftest_spawn_critical",
        'declare -F "$handler" >/dev/null',
    ),
    (
        "spawn EXIT handler binding removed",
        "devcontainer_image_selftest",
        "finish_selftest_spawn_critical",
        'trap "$handler \\$?" EXIT',
    ),
    (
        "spawn HUP handler binding removed",
        "devcontainer_image_selftest",
        "finish_selftest_spawn_critical",
        'trap "$handler 129" HUP',
    ),
    (
        "spawn INT handler binding removed",
        "devcontainer_image_selftest",
        "finish_selftest_spawn_critical",
        'trap "$handler 130" INT',
    ),
    (
        "spawn TERM handler binding removed",
        "devcontainer_image_selftest",
        "finish_selftest_spawn_critical",
        'trap "$handler 143" TERM',
    ),
    (
        "allocation launcher execv removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        "os.execv(",
    ),
    (
        "allocation launcher Bash executable changed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        '"/bin/bash", [',
    ),
    (
        "allocation launcher Bash argv0 changed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        '["/bin/bash",',
    ),
    (
        "allocation launcher protected Bash flag removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        '"-p",',
    ),
    (
        "allocation launcher option terminator removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        '"--",',
    ),
    (
        "allocation launcher Python interpreter changed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        "/usr/bin/python3",
    ),
    (
        "allocation launcher no-bytecode flag removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        " -B ",
    ),
    (
        "allocation launcher isolation flag removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        " -I ",
    ),
    (
        "allocation launcher site-import refusal removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        " -S ",
    ),
    (
        "allocation launcher command-string mode removed",
        "devcontainer_image_selftest_cases",
        "selftest_allocation_signal_path",
        " -c ",
    ),
    (
        "signal launcher execv removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        "os.execv(",
    ),
    (
        "signal launcher Bash executable changed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        '"/bin/bash", [',
    ),
    (
        "signal launcher Bash argv0 changed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        '["/bin/bash",',
    ),
    (
        "signal launcher protected Bash flag removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        '"-p",',
    ),
    (
        "signal launcher option terminator removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        '"--",',
    ),
    (
        "signal launcher session isolation removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        "/usr/bin/setsid ",
    ),
    (
        "signal launcher Python interpreter changed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        "/usr/bin/python3",
    ),
    (
        "signal launcher no-bytecode flag removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        " -B ",
    ),
    (
        "signal launcher isolation flag removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        " -I ",
    ),
    (
        "signal launcher site-import refusal removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        " -S ",
    ),
    (
        "signal launcher command-string mode removed",
        "devcontainer_image_signal_selftest",
        "start_signal_controller",
        " -c ",
    ),
)

TRIPWIRE_LABELS = ("replaced-lock build tripwire removed", "missing-lock build tripwire removed")
TRIPWIRE_PATTERN = 'build_image() { : >"$marker"; }'

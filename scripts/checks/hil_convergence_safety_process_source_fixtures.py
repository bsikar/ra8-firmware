# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact source mutations for the authenticated supervisor process boundary."""

from __future__ import annotations

from hil_convergence_safety_image_subreaper_policy import PROCESS_LOADER_TOKENS

Mutation = tuple[str, str, str, str]
PROCESS = "devcontainer_image_selftest_process"
SUPERVISOR = "devcontainer_image_selftest_supervisor"
SUPERVISOR_CASES = "devcontainer_image_selftest_supervisor_cases"
_selftest_key = "devcontainer_image_selftest"
_selftest_cases_key = "devcontainer_image_selftest_cases"
_signal_selftest_key = "devcontainer_image_signal_selftest"


def _process_mutations_1() -> tuple[Mutation, ...]:
    """Return exact subreaper capability and child-binding mutations."""
    return (
        (
            "child subreaper capability constant changed",
            PROCESS,
            "PR_SET_CHILD_SUBREAPER = 36",
            "PR_SET_CHILD_SUBREAPER = 0",
        ),
        (
            "child subreaper verification constant changed",
            PROCESS,
            "PR_GET_CHILD_SUBREAPER = 37",
            "PR_GET_CHILD_SUBREAPER = 0",
        ),
        (
            "child subreaper prctl activation removed",
            PROCESS,
            "if prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:",
            "if False:",
        ),
        (
            "child subreaper prctl verification removed",
            PROCESS,
            "result = prctl(PR_GET_CHILD_SUBREAPER, ctypes.addressof(state), 0, 0, 0)",
            "result = 0",
        ),
        (
            "child subreaper verified-state proof removed",
            PROCESS,
            "return result == 0 and state.value == 1",
            "return result == 0",
        ),
        (
            "child subreaper activation assignment removed",
            PROCESS,
            "        self.subreaper = _enable_child_subreaper()",
            "        self.subreaper = True",
        ),
        (
            "child subreaper initial-empty proof removed",
            PROCESS,
            "return self.subreaper and _child_table_is_empty()",
            "return self.subreaper",
        ),
        (
            "child subreaper direct-parent binding removed",
            PROCESS,
            "        if parent != os.getpid() or identity is None:\n            return None",
            "        if identity is None:\n            return None",
        ),
    )


def _manual_child_mutations() -> tuple[Mutation, ...]:
    """Return exact manual-child binding mutations."""
    return (
        (
            "manual child subreaper gate removed",
            PROCESS,
            "        if not self.subreaper or self.child is not None or self.pid is not None:",
            "        if False:",
        ),
        (
            "manual child containment binding removed",
            PROCESS,
            "        self.child = child\n"
            "        self.pid = child.pid\n"
            "        self.children_contained = False",
            "        self.child = child\n"
            "        self.pid = child.pid\n"
            "        self.children_contained = True",
        ),
        (
            "manual child isolated identity proof removed",
            PROCESS,
            "            and identity.pid == identity.group == identity.session",
            "            and identity.pid > 0",
        ),
        (
            "manual child direct-parent identity proof removed",
            PROCESS,
            "            and children.get(child.pid) == identity",
            "            and children is not None",
        ),
    )


def _manual_terminal_mutations() -> tuple[Mutation, ...]:
    """Return exact unreaped terminal-child authority mutations."""
    return (
        (
            "manual child terminal authority initialization removed",
            PROCESS,
            "        self.leader_terminal = False",
            "        self.leader_terminal = True",
        ),
        (
            "manual child WNOWAIT observation removed",
            PROCESS,
            "            terminal = os.waitid(\n"
            "                os.P_PID,\n"
            "                child.pid,\n"
            "                os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
            "            )",
            "            terminal = None",
        ),
        (
            "manual child early-reap PID release removed",
            PROCESS,
            "        except ChildProcessError:\n"
            "            self.reaped = True\n"
            "            self.leader_terminal = True",
            "        except ChildProcessError:\n            self.leader_terminal = True",
        ),
        (
            "manual child terminal state binding removed",
            PROCESS,
            "        self.leader_terminal = terminal is not None",
            "        self.leader_terminal = bool(terminal)",
        ),
        (
            "manual terminal child signal bypass removed",
            PROCESS,
            "        if self.leader_terminal:\n            return self._finish_terminal_leader()",
            "        if False:\n            return self._finish_terminal_leader()",
        ),
    )


def _adopted_drain_mutations() -> tuple[Mutation, ...]:
    """Return exact retained-leader and pre-reap descendant-drain mutations."""
    return (
        (
            "child subreaper leader exclusion removed",
            PROCESS,
            "            leader = children.pop(excluded_pid, None)\n"
            "            if leader != self.leader_identity:",
            "            leader = self.leader_identity\n"
            "            if leader != self.leader_identity:",
        ),
        (
            "child subreaper adopted exclusion dispatch removed",
            PROCESS,
            "            children = self._bound_direct_children(excluded_pid)",
            "            children = _direct_children()",
        ),
        (
            "child subreaper leader WNOWAIT retention removed",
            PROCESS,
            "                result = os.waitid(\n"
            "                    os.P_PID,\n"
            "                    self.pid,\n"
            "                    os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
            "                )",
            "                result = os.waitid(\n"
            "                    os.P_PID, self.pid, os.WEXITED | os.WNOHANG\n"
            "                )",
        ),
        (
            "child subreaper lost authority fail-stop removed",
            PROCESS,
            "        if self.authority_lost:\n            return False",
            "        if False:\n            return False",
        ),
        (
            "child subreaper pre-reap adopted drain removed",
            PROCESS,
            "        descendants_drained = leader_terminal and "
            "self._cleanup_adopted_children(leader)",
            "        descendants_drained = leader_terminal",
        ),
    )


def _process_mutations_2() -> tuple[Mutation, ...]:
    """Return exact subreaper signal, reap, and containment mutations."""
    return (
        (
            "child subreaper exact kill removed",
            PROCESS,
            "os.kill(authority.pid, signal.SIGKILL)",
            "os.kill(authority.pid, signal.SIGTERM)",
        ),
        (
            "child subreaper pre-signal identity proof removed",
            PROCESS,
            "        if _bind_process(authority.pid) != authority:\n"
            "            return False\n"
            "        with suppress(ProcessLookupError):",
            "        if False:\n"
            "            return False\n"
            "        with suppress(ProcessLookupError):",
        ),
        (
            "child subreaper WNOWAIT proof removed",
            PROCESS,
            "        result = os.waitid(\n"
            "            os.P_PID,\n"
            "            authority.pid,\n"
            "            os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
            "        )",
            "        result = os.waitid(\n"
            "            os.P_PID, authority.pid, os.WEXITED | os.WNOHANG\n        )",
        ),
        (
            "child subreaper pre-reap identity proof removed",
            PROCESS,
            "        if _bind_process(authority.pid) != authority:\n"
            "            return False\n"
            "        waited, _status = os.waitpid(authority.pid, 0)",
            "        if False:\n            return False\n"
            "        waited, _status = os.waitpid(authority.pid, 0)",
        ),
        (
            "child subreaper exact reap removed",
            PROCESS,
            "return waited == authority.pid",
            "return True",
        ),
        (
            "child subreaper empty completion removed",
            PROCESS,
            "                if _child_table_is_empty():",
            "                if True:",
        ),
    )


def _process_mutations_3() -> tuple[Mutation, ...]:
    """Return terminal subreaper completion and fail-stop mutations."""
    return (
        (
            "child subreaper ECHILD completion removed",
            PROCESS,
            "    except ChildProcessError:\n        return True\n    return False",
            "    except ChildProcessError:\n        return False\n    return False",
        ),
        (
            "child subreaper blocking containment removed",
            PROCESS,
            "        while not cleaned and not self.children_contained:",
            "        if not cleaned and not self.children_contained:",
        ),
    )


def _cross_source_mutations() -> tuple[Mutation, ...]:
    """Return exact main-supervisor subreaper call mutations."""
    return (
        (
            "child subreaper enable call removed",
            SUPERVISOR,
            "    owned_test_descriptor = active.test_descriptor\n"
            "    if not supervisor.enable_subreaper():\n"
            "        return INTEGRITY_REFUSAL_STATUS",
            "    owned_test_descriptor = active.test_descriptor\n"
            "    if False:\n"
            "        return INTEGRITY_REFUSAL_STATUS",
        ),
        (
            "child subreaper final containment call removed",
            SUPERVISOR,
            "        supervisor.contain()",
            "        supervisor.cleanup()",
        ),
        (
            "closed-controller subreaper enable removed",
            SUPERVISOR_CASES,
            "        if not supervisor.enable_subreaper():\n            return 1\n"
            '        if mode in ("death", "observation"):',
            '        if mode in ("death", "observation"):',
        ),
        (
            "closed-controller bound adoption removed",
            SUPERVISOR_CASES,
            "        supervisor.bind_spawned_child(child)",
            "        supervisor.child, supervisor.pid = child, child.pid",
        ),
        (
            "watchdog pre-proof containment removed",
            SUPERVISOR_CASES,
            "        contained = supervisor.contain()\n"
            "        _write_exact(\n"
            "            proof_descriptor,\n"
            '            b"K\\n" if killed and contained else b"F\\n",\n'
            "            RECEIPT_MAX_BYTES,\n"
            "        )",
            "        _write_exact(\n"
            "            proof_descriptor,\n"
            '            b"K\\n" if killed and contained else b"F\\n",\n'
            "            RECEIPT_MAX_BYTES,\n"
            "        )",
        ),
    )


def _loader_replacements() -> dict[str, str]:
    """Return one parseable weakening for each authenticated-loader property."""
    return {
        "process source metadata regular binding removed": "True",
        "process source metadata link binding removed": "True",
        "process source metadata owner binding removed": "True",
        "process source metadata group binding removed": "True",
        "process source metadata mode binding removed": "True",
        "process source metadata byte bound removed": "metadata.st_size >= 0",
        "process source read step bound removed": "for _step in (0,):",
        "process source descriptor pread binding removed": ("chunk = os.read(descriptor, 4096)"),
        "process source complete-size postcondition removed": "if False:",
        "process loader bound source read removed": 'source = b""',
        "process loader pre-exec digest binding removed": "if False:",
        "process loader namespace name binding removed": ('module_name = "_ra8_unbound_process"'),
        "process loader private module construction removed": "module = object()",
        "process loader private module namespace removed": "namespace = {}",
        "process loader namespace filename binding removed": (
            'namespace["__file__"] = "process.py"'
        ),
        "process loader grant version binding removed": (
            'namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 2'
        ),
        "process loader authenticated exec removed": (
            "eval(  # mutation: authenticated exec removed"
        ),
        "process loader compile source binding removed": 'compile(b"",',
        "process loader compile filename binding removed": '"process.py",',
        "process loader compile exec-mode binding removed": ', "eval")',
        "process loader grant exact consumption removed": (
            'grant = namespace.get("_RA8_SUPERVISOR_PROCESS_VERSION")'
        ),
        "process loader grant absence postcondition removed": "if False:",
        "process loader post-exec same-FD digest removed": "if False:",
        "process loader validation delegation removed": "return ()",
        "process loader class API binding removed": "if False or not all(",
        "process loader function API binding removed": "True for value in functions",
        "process loader API namespace identity removed": "if False:",
        "process loader class namespace binding removed": "escaped = False",
        "process loader function namespace binding removed": "escaped = escaped",
        "process loader method namespace binding removed": "escaped = escaped",
        "process loader preexisting module refusal removed": "if False:",
        "process loader module registration removed": "namespace[module_name] = module",
        "process loader module identity postcondition removed": "escaped = escaped",
        "process loader module residue cleanup removed": (
            "if False:\n            del sys.modules[module_name]"
        ),
        "process loader descriptor final-close removed": "os.fstat(descriptor)",
        "process loader install residue refusal removed": "if False:",
    }


def _loader_anchor_overrides() -> dict[str, str]:
    """Return unique contextual anchors for otherwise repeated loader tokens."""
    metadata_block = (
        "    safe = (\n"
        "        stat.S_ISREG(metadata.st_mode)\n"
        "        and metadata.st_nlink == 1\n"
        "        and metadata.st_uid == os.getuid()\n"
        "        and metadata.st_gid == os.getgid()\n"
        "        and stat.S_IMODE(metadata.st_mode) == PROCESS_MODE\n"
        "        and 0 < metadata.st_size <= PROCESS_MAX_BYTES\n"
        "    )"
    )
    pread_block = (
        "    for _step in range(PROCESS_READ_STEPS):\n"
        "        chunk = os.pread(descriptor, 4096, offset)"
    )
    exec_block = (
        "        exec(  # noqa: S102 -- exact digest-bound source-only FD\n"
        '            compile(source, namespace["__file__"], "exec"), namespace\n'
        "        )\n"
        '        grant = namespace.pop("_RA8_SUPERVISOR_PROCESS_VERSION", None)'
    )
    close_block = (
        "        if module is not None and sys.modules.get(module_name) is module:\n"
        "            del sys.modules[module_name]\n"
        "        os.close(descriptor)"
    )
    overrides = dict.fromkeys(
        (
            "process source metadata regular binding removed",
            "process source metadata link binding removed",
            "process source metadata owner binding removed",
            "process source metadata group binding removed",
            "process source metadata mode binding removed",
            "process source metadata byte bound removed",
        ),
        metadata_block,
    )
    overrides["process source descriptor pread binding removed"] = pread_block
    for label in (
        "process loader authenticated exec removed",
        "process loader compile source binding removed",
        "process loader compile filename binding removed",
        "process loader compile exec-mode binding removed",
    ):
        overrides[label] = exec_block
    overrides["process loader module residue cleanup removed"] = close_block
    overrides["process loader descriptor final-close removed"] = close_block
    return overrides


def _loader_mutations() -> tuple[Mutation, ...]:
    """Return exact mutations for every scoped process-loader authority."""
    replacements = _loader_replacements()
    if replacements.keys() != PROCESS_LOADER_TOKENS.keys():
        message = "process-loader mutation census drifted"
        raise RuntimeError(message)
    overrides = _loader_anchor_overrides()
    mutations = []
    for label, (_function, token) in PROCESS_LOADER_TOKENS.items():
        old = overrides.get(label, token)
        replacement = old.replace(token, replacements[label])
        mutations.append((label, SUPERVISOR, old, replacement))
    order_old = (
        '        namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 1\n'
        "        sys.modules[module_name] = module"
    )
    order_new = (
        "        sys.modules[module_name] = module\n"
        '        namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 1'
    )
    return (
        *tuple(mutations),
        ("process loader authentication order changed", SUPERVISOR, order_old, order_new),
    )


def _owner_extraction_mutations() -> tuple[Mutation, ...]:
    """Require every registered Python and Bash semantic owner to exist exactly once."""
    owners = (
        (PROCESS, "BoundGroup._close_entry_authority", "    def _close_entry_authority("),
        (PROCESS, "BoundGroup.spawn", "    def spawn("),
        (SUPERVISOR, "_load_cases_dispatch", "def _load_cases_dispatch("),
        (SUPERVISOR, "_open_suite_root_authority", "def _open_suite_root_authority("),
        (SUPERVISOR, "_read_cases_source", "def _read_cases_source("),
        (SUPERVISOR, "_spawn_payload", "def _spawn_payload("),
        (SUPERVISOR, "_suite_root_metadata_is_safe", "def _suite_root_metadata_is_safe("),
        (SUPERVISOR, "_suite_root_path_is_safe", "def _suite_root_path_is_safe("),
        (SUPERVISOR_CASES, "_closed_controller_command", "def _closed_controller_command("),
        (
            SUPERVISOR_CASES,
            "_closed_controller_descriptor_selftest",
            "def _closed_controller_descriptor_selftest(",
        ),
        (SUPERVISOR_CASES, "_refused_controller_launch", "def _refused_controller_launch("),
        (SUPERVISOR_CASES, "_suite_root_is_safe", "def _suite_root_is_safe("),
        (SUPERVISOR_CASES, "_watchdog_expiry_runner", "def _watchdog_expiry_runner("),
        (_selftest_key, "begin_selftest_spawn_critical", "begin_selftest_spawn_critical() {\n"),
        (_selftest_key, "finish_selftest_spawn_critical", "finish_selftest_spawn_critical() {\n"),
        (
            "devcontainer_image_bound_exit_selftest",
            "run_bound_exit_supervisor",
            "run_bound_exit_supervisor() {\n",
        ),
        (
            _selftest_cases_key,
            "selftest_allocation_signal_path",
            "selftest_allocation_signal_path() {\n",
        ),
        (_signal_selftest_key, "start_signal_controller", "start_signal_controller() {\n"),
    )
    mutations = []
    for key, owner, opening in owners:
        replacement = "x_" + opening if "() {" in opening else opening.replace("def ", "def x_", 1)
        mutations.append(
            (
                f"semantic owner renamed: {key}:{owner}",
                key,
                opening,
                replacement,
            )
        )
    return tuple(mutations)


def _owner_ambiguity_mutations() -> tuple[Mutation, ...]:
    """Require representative top-level and class owners to remain unambiguous."""
    return (
        (
            f"semantic owner duplicated: {SUPERVISOR_CASES}:_suite_root_is_safe",
            SUPERVISOR_CASES,
            "def _entry_belongs_to_root(",
            "def _suite_root_is_safe(",
        ),
        (
            f"semantic owner duplicated: {PROCESS}:BoundGroup.spawn",
            PROCESS,
            "    def enable_subreaper(",
            "    def spawn(",
        ),
    )


def process_authority_mutations() -> tuple[Mutation, ...]:
    """Return the complete focused process-boundary mutation set."""
    return (
        *_process_mutations_1(),
        *_manual_child_mutations(),
        *_manual_terminal_mutations(),
        *_adopted_drain_mutations(),
        *_process_mutations_2(),
        *_process_mutations_3(),
        *_cross_source_mutations(),
        *_loader_mutations(),
        *_owner_extraction_mutations(),
        *_owner_ambiguity_mutations(),
    )

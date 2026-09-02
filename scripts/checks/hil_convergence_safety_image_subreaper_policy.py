# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact Linux child-subreaper policy for the image selftest supervisor."""

from __future__ import annotations

import ast

SUBREAPER_TOKENS = {
    "child subreaper capability constant changed": "PR_SET_CHILD_SUBREAPER = 36",
    "child subreaper verification constant changed": "PR_GET_CHILD_SUBREAPER = 37",
    "child subreaper prctl activation removed": (
        "if prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:"
    ),
    "child subreaper prctl verification removed": (
        "result = prctl(PR_GET_CHILD_SUBREAPER, ctypes.addressof(state), 0, 0, 0)"
    ),
    "child subreaper verified-state proof removed": ("return result == 0 and state.value == 1"),
    "child subreaper activation assignment removed": (
        "        self.subreaper = _enable_child_subreaper()"
    ),
    "child subreaper initial-empty proof removed": (
        "return self.subreaper and _child_table_is_empty()"
    ),
    "manual child subreaper gate removed": (
        "        if not self.subreaper or self.child is not None or self.pid is not None:"
    ),
    "manual child containment binding removed": (
        "        self.child = child\n"
        "        self.pid = child.pid\n"
        "        self.children_contained = False"
    ),
    "manual child isolated identity proof removed": (
        "            and identity.pid == identity.group == identity.session"
    ),
    "manual child direct-parent identity proof removed": (
        "            and children.get(child.pid) == identity"
    ),
    "manual child terminal authority initialization removed": (
        "        self.leader_terminal = False"
    ),
    "manual child WNOWAIT observation removed": (
        "            terminal = os.waitid(\n"
        "                os.P_PID,\n"
        "                child.pid,\n"
        "                os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
        "            )"
    ),
    "manual child early-reap PID release removed": (
        "        except ChildProcessError:\n"
        "            self.reaped = True\n"
        "            self.leader_terminal = True"
    ),
    "manual child terminal state binding removed": (
        "        self.leader_terminal = terminal is not None"
    ),
    "manual terminal child signal bypass removed": (
        "        if self.leader_terminal:\n            return self._finish_terminal_leader()"
    ),
    "child subreaper leader exclusion removed": (
        "            leader = children.pop(excluded_pid, None)\n"
        "            if leader != self.leader_identity:"
    ),
    "child subreaper adopted exclusion dispatch removed": (
        "            children = self._bound_direct_children(excluded_pid)"
    ),
    "child subreaper leader WNOWAIT retention removed": (
        "                result = os.waitid(\n"
        "                    os.P_PID,\n"
        "                    self.pid,\n"
        "                    os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
        "                )"
    ),
    "child subreaper lost authority fail-stop removed": (
        "        if self.authority_lost:\n            return False"
    ),
    "child subreaper pre-reap adopted drain removed": (
        "        descendants_drained = leader_terminal and self._cleanup_adopted_children(leader)"
    ),
    "child subreaper direct-parent binding removed": (
        "        if parent != os.getpid() or identity is None:\n            return None"
    ),
    "child subreaper pre-signal identity proof removed": (
        "        if _bind_process(authority.pid) != authority:\n"
        "            return False\n"
        "        with suppress(ProcessLookupError):"
    ),
    "child subreaper exact kill removed": "os.kill(authority.pid, signal.SIGKILL)",
    "child subreaper WNOWAIT proof removed": (
        "        result = os.waitid(\n"
        "            os.P_PID,\n"
        "            authority.pid,\n"
        "            os.WEXITED | os.WNOHANG | os.WNOWAIT,\n"
        "        )"
    ),
    "child subreaper pre-reap identity proof removed": (
        "        if _bind_process(authority.pid) != authority:\n"
        "            return False\n"
        "        waited, _status = os.waitpid(authority.pid, 0)"
    ),
    "child subreaper exact reap removed": "return waited == authority.pid",
    "child subreaper ECHILD completion removed": (
        "    except ChildProcessError:\n        return True\n    return False"
    ),
    "child subreaper empty completion removed": ("                if _child_table_is_empty():"),
    "child subreaper enable call removed": "    if not supervisor.enable_subreaper():",
    "child subreaper blocking containment removed": (
        "        while not cleaned and not self.children_contained:"
    ),
    "child subreaper final containment call removed": "        supervisor.contain()",
}

MOVED_PROCESS_TOKENS = {
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
        '            SUPERVISOR_PROGRAM,\n            "--controller",'
    ),
    "process stat bytes parser replaced with text decoding": '(entry / "stat").read_bytes()',
    "bound-exit group cleanup reduced to leader PID": "os.killpg(leader, signal.SIGKILL)",
    "bound-exit cleanup signal block moved after authority checks": (
        "        signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)\n"
        "        if self.authority_lost:"
    ),
}

PROCESS_STATIC_TOKENS = (
    "root_descriptor = _anchored_root_descriptor(launch.status)",
    "members is not None and members <= {leader}",
    "        if not self._reap():\n            self.cleaning = False\n            return False",
)

PROCESS_LOADER_TOKENS = {
    "process source metadata regular binding removed": (
        "_read_process_source",
        "stat.S_ISREG(metadata.st_mode)",
    ),
    "process source metadata link binding removed": (
        "_read_process_source",
        "metadata.st_nlink == 1",
    ),
    "process source metadata owner binding removed": (
        "_read_process_source",
        "metadata.st_uid == os.getuid()",
    ),
    "process source metadata group binding removed": (
        "_read_process_source",
        "metadata.st_gid == os.getgid()",
    ),
    "process source metadata mode binding removed": (
        "_read_process_source",
        "stat.S_IMODE(metadata.st_mode) == PROCESS_MODE",
    ),
    "process source metadata byte bound removed": (
        "_read_process_source",
        "0 < metadata.st_size <= PROCESS_MAX_BYTES",
    ),
    "process source read step bound removed": (
        "_read_process_source",
        "for _step in range(PROCESS_READ_STEPS):",
    ),
    "process source descriptor pread binding removed": (
        "_read_process_source",
        "chunk = os.pread(descriptor, 4096, offset)",
    ),
    "process source complete-size postcondition removed": (
        "_read_process_source",
        "if len(source) != metadata.st_size or len(source) > PROCESS_MAX_BYTES:",
    ),
    "process loader bound source read removed": (
        "_load_process_api",
        "source = _read_process_source(descriptor)",
    ),
    "process loader pre-exec digest binding removed": (
        "_load_process_api",
        "if digest != PROCESS_RAW_SHA256:",
    ),
    "process loader namespace name binding removed": (
        "_load_process_api",
        'module_name = "_ra8_supervisor_process"',
    ),
    "process loader private module construction removed": (
        "_load_process_api",
        "module = types.ModuleType(module_name)",
    ),
    "process loader private module namespace removed": (
        "_load_process_api",
        "namespace = module.__dict__",
    ),
    "process loader namespace filename binding removed": (
        "_load_process_api",
        'namespace["__file__"] = f"/proc/self/fd/{descriptor}"',
    ),
    "process loader grant version binding removed": (
        "_load_process_api",
        'namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 1',
    ),
    "process loader authenticated exec removed": (
        "_load_process_api",
        "exec(  # noqa: S102 -- exact digest-bound source-only FD",
    ),
    "process loader compile source binding removed": ("_load_process_api", "compile(source,"),
    "process loader compile filename binding removed": (
        "_load_process_api",
        'namespace["__file__"],',
    ),
    "process loader compile exec-mode binding removed": ("_load_process_api", ', "exec")'),
    "process loader grant exact consumption removed": (
        "_load_process_api",
        'grant = namespace.pop("_RA8_SUPERVISOR_PROCESS_VERSION", None)',
    ),
    "process loader grant absence postcondition removed": (
        "_load_process_api",
        'if grant != 1 or "_RA8_SUPERVISOR_PROCESS_VERSION" in namespace:',
    ),
    "process loader post-exec same-FD digest removed": (
        "_load_process_api",
        "if hashlib.sha256(_read_process_source(descriptor)).hexdigest() != digest:",
    ),
    "process loader validation delegation removed": (
        "_load_process_api",
        "return _validate_process_api(module_name, namespace, module)",
    ),
    "process loader class API binding removed": (
        "_validate_process_api",
        "if not all(isinstance(value, type) for value in classes) or not all(",
    ),
    "process loader function API binding removed": (
        "_validate_process_api",
        "isinstance(value, types.FunctionType) for value in functions",
    ),
    "process loader API namespace identity removed": (
        "_validate_process_api",
        "if escaped:",
    ),
    "process loader class namespace binding removed": (
        "_validate_process_api",
        'escaped = any(value.__module__ != namespace["__name__"] for value in classes)',
    ),
    "process loader function namespace binding removed": (
        "_validate_process_api",
        "escaped = escaped or any(value.__globals__ is not namespace for value in functions)",
    ),
    "process loader method namespace binding removed": (
        "_validate_process_api",
        "escaped = escaped or any(value.__globals__ is not namespace for value in methods)",
    ),
    "process loader preexisting module refusal removed": (
        "_load_process_api",
        "if module_name in sys.modules:",
    ),
    "process loader module registration removed": (
        "_load_process_api",
        "sys.modules[module_name] = module",
    ),
    "process loader module identity postcondition removed": (
        "_validate_process_api",
        "escaped = escaped or sys.modules.get(module_name) is not module",
    ),
    "process loader module residue cleanup removed": (
        "_load_process_api",
        "if module is not None and sys.modules.get(module_name) is module:\n"
        "            del sys.modules[module_name]",
    ),
    "process loader descriptor final-close removed": (
        "_load_process_api",
        "os.close(descriptor)",
    ),
    "process loader install residue refusal removed": (
        "_install_process_api",
        'if "_ra8_supervisor_process" in sys.modules:',
    ),
}

PROCESS_LOADER_ORDER = (
    'module_name = "_ra8_supervisor_process"',
    "if module_name in sys.modules:",
    "module = types.ModuleType(module_name)",
    "namespace = module.__dict__",
    'namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 1',
    "sys.modules[module_name] = module",
    "exec(  # noqa: S102 -- exact digest-bound source-only FD",
    'grant = namespace.pop("_RA8_SUPERVISOR_PROCESS_VERSION", None)',
    "if hashlib.sha256(_read_process_source(descriptor)).hexdigest() != digest:",
    "return _validate_process_api(module_name, namespace, module)",
    "if module is not None and sys.modules.get(module_name) is module:",
    "del sys.modules[module_name]",
    "os.close(descriptor)",
)
PROCESS_VALIDATION_ORDER = (
    "api = tuple(namespace.get(name) for name in names)",
    "classes, functions = api[:2], api[2:]",
    "if not all(isinstance(value, type) for value in classes) or not all(",
    "methods = tuple(",
    'escaped = any(value.__module__ != namespace["__name__"] for value in classes)',
    "escaped = escaped or sys.modules.get(module_name) is not module",
    "return api",
)
PROCESS_LOADER_ORDER_DIAGNOSTIC = (
    "devcontainer image supervisor: authenticated process-loader order drifted"
)

SUPERVISOR_SUBREAPER_LABELS = frozenset(
    {"child subreaper enable call removed", "child subreaper final containment call removed"}
)
SUPERVISOR_SUBREAPER_OWNERS = {
    "child subreaper enable call removed": "_supervise",
    "child subreaper final containment call removed": "_supervise",
}
SUPERVISOR_SUBREAPER_ORDER = (
    "if not supervisor.enable_subreaper():",
    "old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)",
    "supervisor.spawn(source_descriptor, launch)",
    "        supervisor.contain()",
)
PROCESS_SUBREAPER_ORDER = (
    "os.killpg(leader, signal.SIGKILL)",
    "leader_terminal = self._wait_leader_terminal()",
    "self._cleanup_adopted_children(leader)",
    "members = _group_members(leader) if descendants_drained else None",
    "members is not None and members <= {leader} and self._reap()",
    "descendants_clean = self._cleanup_adopted_children()",
    "return descendants_clean and self._close_entry_authority()",
)


def semantic_findings(label: str) -> tuple[str, ...] | None:
    """Return the exact focused finding for one child-subreaper mutation."""
    loader = PROCESS_LOADER_TOKENS.get(label)
    if loader is not None:
        function, token = loader
        return (
            "devcontainer image supervisor: "
            f"{function} process-loader token is not unique: {token}",
        )
    if label == "process loader authentication order changed":
        return (PROCESS_LOADER_ORDER_DIAGNOSTIC,)
    token = SUBREAPER_TOKENS.get(label, MOVED_PROCESS_TOKENS.get(label))
    if token is None:
        return None
    authority = (
        "devcontainer image supervisor"
        if label in SUPERVISOR_SUBREAPER_LABELS
        else "devcontainer image supervisor process"
    )
    finding = f"{authority}: required process-authority token is not unique: {token}"
    findings = (finding,)
    if label in {
        "child subreaper enable call removed",
        "child subreaper final containment call removed",
    }:
        findings += ("devcontainer image supervisor: subreaper cleanup order drifted",)
    if label == "bound-exit group cleanup reduced to leader PID":
        findings += ("devcontainer image supervisor: subreaper cleanup order drifted",)
    if label == "bound-exit cleanup signal block moved after authority checks":
        authority_token = SUBREAPER_TOKENS["child subreaper lost authority fail-stop removed"]
        findings += (
            "devcontainer image supervisor process: required process-authority token "
            f"is not unique: {authority_token}",
        )
    if label == "child subreaper lost authority fail-stop removed":
        moved = MOVED_PROCESS_TOKENS["bound-exit cleanup signal block moved after authority checks"]
        findings += (
            "devcontainer image supervisor process: required process-authority token "
            f"is not unique: {moved}",
        )
    if label == "child subreaper pre-reap adopted drain removed":
        findings += ("devcontainer image supervisor: subreaper cleanup order drifted",)
    return findings


def _function_source(source: str, name: str) -> str | None:
    """Return one complete top-level function from parsed source."""
    try:
        module = ast.parse(source)
    except SyntaxError:
        return None
    for node in module.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            return ast.get_source_segment(source, node)
    return None


def _loader_errors(supervisor: str) -> list[str]:
    """Bind every process-loader property inside its exact owning function."""
    findings = []
    for function, token in PROCESS_LOADER_TOKENS.values():
        body = _function_source(supervisor, function)
        if body is None or body.count(token) != 1:
            findings.append(
                "devcontainer image supervisor: "
                f"{function} process-loader token is not unique: {token}"
            )
    order_specs = (
        ("_load_process_api", PROCESS_LOADER_ORDER),
        ("_validate_process_api", PROCESS_VALIDATION_ORDER),
    )
    for function, order in order_specs:
        body = _function_source(supervisor, function)
        if body is not None and all(body.count(token) == 1 for token in order):
            positions = tuple(body.index(token) for token in order)
            if positions != tuple(sorted(positions)):
                findings.append(PROCESS_LOADER_ORDER_DIAGNOSTIC)
    return findings


def errors(supervisor: str, process_source: str) -> list[str]:
    """Require every subreaper token once and its cleanup chain in order."""
    findings = []
    for label, token in SUBREAPER_TOKENS.items():
        if label in SUPERVISOR_SUBREAPER_LABELS:
            owner = SUPERVISOR_SUBREAPER_OWNERS[label]
            source = _function_source(supervisor, owner) or ""
        else:
            source = process_source
        authority = (
            "devcontainer image supervisor"
            if label in SUPERVISOR_SUBREAPER_LABELS
            else "devcontainer image supervisor process"
        )
        if source.count(token) != 1:
            findings.append(f"{authority}: required process-authority token is not unique: {token}")
    findings.extend(
        "devcontainer image supervisor process: required process-authority token is not unique: "
        f"{token}"
        for token in MOVED_PROCESS_TOKENS.values()
        if process_source.count(token) != 1
    )
    findings.extend(
        "devcontainer image supervisor process: required process-authority token is not unique: "
        f"{token}"
        for token in PROCESS_STATIC_TOKENS
        if process_source.count(token) != 1
    )
    orders = (
        (supervisor, SUPERVISOR_SUBREAPER_ORDER),
        (process_source, PROCESS_SUBREAPER_ORDER),
    )
    for source, order in orders:
        position = -1
        for token in order:
            position = source.find(token, position + 1)
            if position < 0:
                findings.append("devcontainer image supervisor: subreaper cleanup order drifted")
                break
    return findings + _loader_errors(supervisor)

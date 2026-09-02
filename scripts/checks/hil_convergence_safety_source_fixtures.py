# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural mutations for image-lock suppression rationale authorities."""

from __future__ import annotations

Mutation = tuple[str, str, str, str]

CLOSED_COMMAND_BLOCK = """    return (
        sys.executable,
        "-B",
        "-I",
        "-S",
        SUPERVISOR_PROGRAM,
        "--controller",
        entry_authority,
        str(status),
        str(death_descriptor),
        str(root_descriptor),
        root_identity,
        str(SELFTEST_WATCHDOG_TIMEOUT_SECONDS),
    )"""
CLOSED_POPEN_BLOCK = (
    "        child = subprocess.Popen(  # noqa: S603 -- pinned "
    "interpreter and supervisor FD\n"
    "            command,\n"
    "            pass_fds=tuple(inherited),\n"
    "            start_new_session=True,\n"
    "        )"
)
REFUSED_POPEN_BLOCK = (
    "    child = subprocess.Popen(  # noqa: S603 -- pinned "
    "interpreter and supervisor FD\n"
    "        (\n"
    "            sys.executable,\n"
    '            "-B",\n'
    '            "-I",\n'
    '            "-S",\n'
    "            SUPERVISOR_PROGRAM,\n"
    '            "--controller",\n'
    '            "99",\n'
    "            str(status),\n"
    "            str(CLOSED_DESCRIPTOR),\n"
    "            str(root_descriptor),\n"
    "            identity,\n"
    "            str(SELFTEST_WATCHDOG_TIMEOUT_SECONDS),\n"
    "        ),\n"
    "        pass_fds=(source_descriptor, root_descriptor),\n"
    "        start_new_session=True,\n"
    "    )"
)
MAIN_POPEN_BLOCK = (
    "        child = subprocess.Popen(  # noqa: S603 -- current pinned "
    "interpreter and helper\n"
    "            argv,\n"
    "            pass_fds=tuple(inherited),\n"
    "            start_new_session=True,\n"
    "        )\n"
    "        self.bind_spawned_child(child)"
)
SUPERVISOR_LAUNCH_BLOCK = (
    '  RA8_SELFTEST_BOUND_ENTRY="$1" \\\n'
    '    /usr/bin/python3 -B -I -S "$program" --process-fd 9 --cases-fd 8 "$@" || status=$?'
)
PAYLOAD_ENTRY_BLOCK = '''        descriptor = int(entry_authority)
        metadata = os.fstat(descriptor)
        if not _entry_metadata_is_safe(metadata):
            message = "controller entry descriptor is unsafe"
            _refuse_entry(message)
        entry = f"/proc/self/fd/{descriptor}"'''
ENTRY_INTEGRITY_BLOCK = """            self.entry_integrity = (
                _entry_metadata_is_safe(metadata)
                and path_metadata is not None
                and _entry_metadata_is_safe(path_metadata)
                and (path_metadata.st_dev, path_metadata.st_ino) == self.entry_identity
                and current_identity == self.entry_identity
                and current_digest == self.entry_digest
            )"""
MAIN_ROOT_PATH_BLOCK = """        canonical = CANONICAL_TMP.resolve(strict=True)
        resolved = root.resolve(strict=True)
    except OSError:
        return False
    suffix = root.name.removeprefix(SUITE_ROOT_PREFIX)
    return (
        root.is_absolute()
        and root.parent == canonical
        and resolved == root
        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and _suite_root_metadata_is_safe(metadata)
    )"""
MAIN_ROOT_METADATA_BLOCK = """    return (
        stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
    )"""
CASES_ROOT_BLOCK = """        canonical = CANONICAL_TMP.resolve(strict=True)
        metadata = root.lstat()
        suffix = root.name.removeprefix("ra8-devcontainer-image-selftest.")
        identity = f"{metadata.st_dev}:{metadata.st_ino}"
        resolved = root.resolve(strict=True)
    except OSError:
        return False
    return (
        root.is_absolute()
        and root.parent == canonical
        and resolved == root
        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
        and identity == expected_identity
    )"""
ROOT_CANONICAL_VALUE = (
    '    "/tmp"  # noqa: S108 -- fixed physical parent; random mode-0700 inode-bound direct child'
)
MAIN_ROOT_IDENTITY_BLOCK = """        if not _suite_root_metadata_is_safe(after) or identity != (
            before.st_dev,
            before.st_ino,
        ):
            message = "suite root identity changed while opening"
            _refuse_entry(message)"""
HANDLER_VALIDATION_BLOCK = (
    '  [[ "$handler" =~ ^[a-z_][a-z0-9_]*$ ]] && declare -F "$handler" >/dev/null ||\n    return 1'
)
CASES_EMBEDDED_EXEC_BLOCK = """os.execv("/bin/bash", ["/bin/bash", "-p", "--", sys.argv[1],
    "--selftest-allocation-signal-child", *sys.argv[2:]])"""
CASES_PYTHON_LAUNCH_BLOCK = (
    '  /usr/bin/python3 -B -I -S -c "$supervisor" '
    '"$SCRIPT_DIR/devcontainer_image.sh" \\\n'
    '    "$ready" "$tmp" "$SELFTEST_TMP_IDENTITY" "$path" "$identity" \\\n'
    '    "$SELFTEST_SUITE_ANCHOR" "$SELFTEST_SUITE_ANCHOR_IDENTITY" \\\n'
    '    "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &'
)
SIGNAL_EMBEDDED_EXEC_BLOCK = (
    '    if os.path.isfile(ack): os.execv("/bin/bash", ["/bin/bash", "-p", "--", entry, *args])'
)
SIGNAL_PYTHON_LAUNCH_BLOCK = '''    exec /usr/bin/setsid /usr/bin/python3 -B -I -S -c "$launcher" \\
      "$SELFTEST_IMAGE_ENTRY" "$ready" "$ack" "$launcher_mode" \\
      --selftest-image-lock-signal-controller "$managed" "$case_dir" "$readiness_mode"'''
REPLACED_LOCK_TRIPWIRE_BLOCK = (
    '  marker="$tmp/unexpected-swap-build"\n'
    '  mv "$IMAGE_LOCK_FILE" "$managed/original-image.lock"\n'
    '  : >"$IMAGE_LOCK_FILE"\n'
    '  chgrp "$SELFTEST_GROUP_GID" "$IMAGE_LOCK_FILE"\n'
    '  chmod 0660 "$IMAGE_LOCK_FILE"\n'
    "  if (\n"
    "    # shellcheck disable=SC2329  # must-not-fire tripwire records a build after "
    "replaced-lock refusal.\n"
    '    build_image() { : >"$marker"; }\n'
    '    build_locked dead forced "" 1 >/dev/null 2>&1\n'
    "  ); then"
)
MISSING_LOCK_TRIPWIRE_BLOCK = (
    '  rm -f "$IMAGE_LOCK_FILE"\n'
    "  if (\n"
    "    # shellcheck disable=SC2329  # must-not-fire tripwire records a build after "
    "missing-lock refusal.\n"
    '    build_image() { : >"$marker"; }\n'
    '    build_locked dead forced "" 1 >/dev/null 2>&1\n'
    "  ); then"
)
CASES_SOURCE_READ_BLOCK = '''def _read_cases_source(descriptor: int) -> bytes:
    """Read one bounded authenticated cases module without using its pathname."""
    metadata = os.fstat(descriptor)
    safe = (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == CASES_MODE
        and 0 < metadata.st_size <= CASES_MAX_BYTES
    )
    if not safe:
        message = "supervisor cases metadata is unsafe"
        _refuse_entry(message)
    parts = []
    offset = 0
    for _step in range(CASES_READ_STEPS):
        chunk = os.pread(descriptor, 4096, offset)
        if not chunk:
            break
        parts.append(chunk)
        offset += len(chunk)
    source = b"".join(parts)
    if len(source) != metadata.st_size or len(source) > CASES_MAX_BYTES:
        message = "supervisor cases read is incomplete"
        _refuse_entry(message)
    return source'''
CASES_LOADER_BLOCK = (
    "def _load_cases_dispatch(descriptor: int) -> Callable[[list[str]], int | None]:\n"
    '    """Load the authenticated source-only cases dispatcher from its bound FD."""\n'
    """    try:
        source = _read_cases_source(descriptor)
        digest = hashlib.sha256(source).hexdigest()
        if digest != CASES_RAW_SHA256:
            message = "supervisor cases digest drifted"
            _refuse_entry(message)
        namespace = {
            "__name__": "_ra8_supervisor_cases",
            "__file__": f"/proc/self/fd/{descriptor}",
            "_RA8_SUPERVISOR_CASES_VERSION": 1,
        }
        exec(  # noqa: S102 -- exact digest-bound source-only FD
            compile(source, namespace["__file__"], "exec"), namespace
        )
        grant = namespace.pop("_RA8_SUPERVISOR_CASES_VERSION", None)
        if grant != 1 or "_RA8_SUPERVISOR_CASES_VERSION" in namespace:
            message = "supervisor cases load grant was not consumed"
            _refuse_entry(message)
        if hashlib.sha256(_read_cases_source(descriptor)).hexdigest() != digest:
            message = "supervisor cases changed while loading"
            _refuse_entry(message)
        dispatch = namespace.get("dispatch_supervisor_cases")
        if not callable(dispatch):
            message = "supervisor cases dispatcher is absent"
            _refuse_entry(message)
        if dispatch.__globals__ is not namespace:
            message = "supervisor cases dispatcher escaped its private namespace"
            _refuse_entry(message)
        return dispatch
    finally:
        os.close(descriptor)"""
)


def process_authority_mutations() -> tuple[Mutation, ...]:
    """Return every scoped suppression-rationale structural mutation."""
    return (
        *_closed_controller_command_mutations(),
        *_refused_controller_popen_mutations(),
        *_closed_controller_popen_mutations(),
        *_main_supervisor_launch_mutations(),
        *_payload_entry_mutations(),
        *_main_suite_root_path_mutations(),
        *_main_suite_root_identity_mutations(),
        *_cases_suite_root_safety_mutations(),
        *_spawn_handler_mutations(),
        *_embedded_launcher_mutations(),
        *_build_tripwire_mutations(),
        *_cases_source_read_mutations(),
        *_cases_loader_binding_mutations(),
        *_cases_loader_postcondition_mutations(),
    )


def _replace_in_block(block: str, old: str, new: str) -> str:
    """Return one scoped source mutation with a unique inner authority."""
    if block.count(old) != 1:
        message = f"runtime fixture inner authority is not unique: {old}"
        raise ValueError(message)
    return block.replace(old, new)


def _closed_controller_command_mutations() -> tuple[Mutation, ...]:
    """Bind every executable component of the closed-controller command."""
    key = "devcontainer_image_selftest_supervisor_cases"
    changes = (
        (
            "closed-controller interpreter changed",
            "        sys.executable,",
            '        "/bin/false",',
        ),
        ("closed-controller no-bytecode flag removed", '        "-B",', '        "--version",'),
        ("closed-controller isolation flag removed", '        "-I",', '        "--version",'),
        ("closed-controller site-import refusal removed", '        "-S",', '        "--version",'),
        (
            "closed-controller supervisor authority removed",
            "        SUPERVISOR_PROGRAM,",
            '        "/tmp/supervisor.py",',
        ),
        (
            "closed-controller mode removed",
            '        "--controller",',
            '        "--version",',
        ),
    )
    return tuple(
        (label, key, CLOSED_COMMAND_BLOCK, _replace_in_block(CLOSED_COMMAND_BLOCK, old, new))
        for label, old, new in changes
    )


def _refused_controller_popen_mutations() -> tuple[Mutation, ...]:
    """Bind the refused-controller Popen argv, descriptors, and session."""
    key = "devcontainer_image_selftest_supervisor_cases"
    refused_changes = (
        (
            "refused-controller interpreter changed",
            "            sys.executable,",
            '            "/bin/false",',
        ),
        (
            "refused-controller no-bytecode flag removed",
            '            "-B",',
            '            "--version",',
        ),
        (
            "refused-controller isolation flag removed",
            '            "-I",',
            '            "--version",',
        ),
        (
            "refused-controller site-import refusal removed",
            '            "-S",',
            '            "--version",',
        ),
        (
            "refused-controller supervisor authority removed",
            "            SUPERVISOR_PROGRAM,",
            '            "/tmp/supervisor.py",',
        ),
        (
            "refused-controller mode removed",
            '            "--controller",',
            '            "--version",',
        ),
        (
            "refused-controller descriptor propagation removed",
            "        pass_fds=(source_descriptor, root_descriptor),",
            "        pass_fds=(),",
        ),
        (
            "refused-controller session isolation removed",
            "        start_new_session=True,",
            "        start_new_session=False,",
        ),
    )
    return tuple(
        (label, key, REFUSED_POPEN_BLOCK, _replace_in_block(REFUSED_POPEN_BLOCK, old, new))
        for label, old, new in refused_changes
    )


def _closed_controller_popen_mutations() -> tuple[Mutation, ...]:
    """Bind the closed-controller Popen descriptors and isolated session."""
    key = "devcontainer_image_selftest_supervisor_cases"
    return (
        (
            "closed-controller Popen descriptor propagation removed",
            key,
            CLOSED_POPEN_BLOCK,
            _replace_in_block(
                CLOSED_POPEN_BLOCK,
                "            pass_fds=tuple(inherited),",
                "            pass_fds=(),",
            ),
        ),
        (
            "closed-controller Popen session isolation removed",
            key,
            CLOSED_POPEN_BLOCK,
            _replace_in_block(
                CLOSED_POPEN_BLOCK,
                "            start_new_session=True,",
                "            start_new_session=False,",
            ),
        ),
    )


def _main_supervisor_launch_mutations() -> tuple[Mutation, ...]:
    """Bind the main supervisor's Popen and protected Bash launcher argv."""
    process = "devcontainer_image_selftest_process"
    lifecycle = "devcontainer_image_selftest"
    popen = (
        (
            "parent-death Popen descriptor propagation removed",
            process,
            MAIN_POPEN_BLOCK,
            _replace_in_block(
                MAIN_POPEN_BLOCK,
                "            pass_fds=tuple(inherited),",
                "            pass_fds=(),",
            ),
        ),
        (
            "parent-death Popen session isolation removed",
            process,
            MAIN_POPEN_BLOCK,
            _replace_in_block(
                MAIN_POPEN_BLOCK,
                "            start_new_session=True,",
                "            start_new_session=False,",
            ),
        ),
    )
    changes = (
        ("supervisor launcher interpreter changed", "/usr/bin/python3", "/bin/false"),
        ("supervisor launcher no-bytecode flag removed", " -B ", " --version "),
        ("supervisor launcher isolation flag removed", " -I ", " --version "),
        ("supervisor launcher site-import refusal removed", " -S ", " --version "),
        ("supervisor launcher bound program removed", '"$program"', '"$SELFTEST_SUPERVISOR"'),
        (
            "supervisor launcher bound entry environment removed",
            'RA8_SELFTEST_BOUND_ENTRY="$1"',
            'RA8_SELFTEST_BOUND_ENTRY=""',
        ),
        ("supervisor launcher process descriptor option removed", " --process-fd ", " --version "),
        ("supervisor launcher process descriptor changed", " --process-fd 9 ", " --process-fd 7 "),
        ("supervisor launcher cases descriptor option removed", " --cases-fd ", " --version "),
        ("supervisor launcher cases descriptor changed", " --cases-fd 8 ", " --cases-fd 7 "),
    )
    launch = tuple(
        (
            label,
            lifecycle,
            SUPERVISOR_LAUNCH_BLOCK,
            _replace_in_block(SUPERVISOR_LAUNCH_BLOCK, old, new),
        )
        for label, old, new in changes
    )
    return (*popen, *launch)


def _payload_entry_mutations() -> tuple[Mutation, ...]:
    """Bind the S606 payload to its opened FD and post-exec digest."""
    key = "devcontainer_image_selftest_supervisor"
    process = "devcontainer_image_selftest_process"
    spawn_changes = (
        (
            "payload entry descriptor metadata binding removed",
            "        if not _entry_metadata_is_safe(metadata):",
            "        if False:",
        ),
        (
            "payload procfd path binding removed",
            '        entry = f"/proc/self/fd/{descriptor}"',
            "        entry = entry_authority",
        ),
    )
    spawn = tuple(
        (label, key, PAYLOAD_ENTRY_BLOCK, _replace_in_block(PAYLOAD_ENTRY_BLOCK, old, new))
        for label, old, new in spawn_changes
    )
    integrity_changes = (
        (
            "payload post-exec pathname identity removed",
            "                and (path_metadata.st_dev, path_metadata.st_ino) "
            "== self.entry_identity",
            "                and True",
        ),
        (
            "payload post-exec descriptor identity removed",
            "                and current_identity == self.entry_identity",
            "                and True",
        ),
        (
            "payload post-exec digest proof removed",
            "                and current_digest == self.entry_digest",
            "                and True",
        ),
    )
    integrity = tuple(
        (
            label,
            process,
            ENTRY_INTEGRITY_BLOCK,
            _replace_in_block(ENTRY_INTEGRITY_BLOCK, old, new),
        )
        for label, old, new in integrity_changes
    )
    return (*spawn, *integrity)


def _main_suite_root_path_mutations() -> tuple[Mutation, ...]:
    """Bind the main supervisor to one canonical direct suite-root path."""
    main = "devcontainer_image_selftest_supervisor"
    main_path_changes = (
        ("main suite-root canonical tmp changed", ROOT_CANONICAL_VALUE, '    "/var/tmp"'),
        (
            "main suite-root canonical parent removed",
            "        and root.parent == canonical",
            "        and True",
        ),
        (
            "main suite-root direct resolution removed",
            "        and resolved == root",
            "        and True",
        ),
        (
            "main suite-root suffix length removed",
            "        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH",
            "        and True",
        ),
        (
            "main suite-root hex suffix removed",
            '        and all(character in "0123456789abcdef" for character in suffix)',
            "        and True",
        ),
    )
    return tuple(
        (
            label,
            main,
            ROOT_CANONICAL_VALUE if "canonical tmp" in label else MAIN_ROOT_PATH_BLOCK,
            (
                _replace_in_block(ROOT_CANONICAL_VALUE, old, new)
                if "canonical tmp" in label
                else _replace_in_block(MAIN_ROOT_PATH_BLOCK, old, new)
            ),
        )
        for label, old, new in main_path_changes
    )


def _main_suite_root_identity_mutations() -> tuple[Mutation, ...]:
    """Bind main suite-root directory metadata and retained inode identity."""
    main = "devcontainer_image_selftest_supervisor"
    metadata_changes = (
        (
            "main suite-root directory type removed",
            "        stat.S_ISDIR(metadata.st_mode)",
            "        True",
        ),
        (
            "main suite-root owner removed",
            "        and metadata.st_uid == os.getuid()",
            "        and True",
        ),
        (
            "main suite-root group removed",
            "        and metadata.st_gid == os.getgid()",
            "        and True",
        ),
        (
            "main suite-root private mode removed",
            "        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE",
            "        and True",
        ),
    )
    main_metadata = tuple(
        (
            label,
            main,
            MAIN_ROOT_METADATA_BLOCK,
            _replace_in_block(MAIN_ROOT_METADATA_BLOCK, old, new),
        )
        for label, old, new in metadata_changes
    )
    identity = (
        (
            "main suite-root inode identity removed",
            main,
            MAIN_ROOT_IDENTITY_BLOCK,
            _replace_in_block(
                MAIN_ROOT_IDENTITY_BLOCK,
                "        if not _suite_root_metadata_is_safe(after) or identity != (\n"
                "            before.st_dev,\n"
                "            before.st_ino,\n"
                "        ):",
                "        if not _suite_root_metadata_is_safe(after):",
            ),
        ),
    )
    return (*main_metadata, *identity)


def _cases_suite_root_changes() -> tuple[tuple[str, str, str], ...]:
    """Return each independent supervisor-cases root weakening."""
    return (
        ("cases suite-root canonical tmp changed", ROOT_CANONICAL_VALUE, '    "/var/tmp"'),
        (
            "cases suite-root canonical parent removed",
            "        and root.parent == canonical",
            "        and True",
        ),
        (
            "cases suite-root direct resolution removed",
            "        and resolved == root",
            "        and True",
        ),
        (
            "cases suite-root suffix length removed",
            "        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH",
            "        and True",
        ),
        (
            "cases suite-root hex suffix removed",
            '        and all(character in "0123456789abcdef" for character in suffix)',
            "        and True",
        ),
        (
            "cases suite-root directory type removed",
            "        and stat.S_ISDIR(metadata.st_mode)",
            "        and True",
        ),
        (
            "cases suite-root owner removed",
            "        and metadata.st_uid == os.getuid()",
            "        and True",
        ),
        (
            "cases suite-root group removed",
            "        and metadata.st_gid == os.getgid()",
            "        and True",
        ),
        (
            "cases suite-root private mode removed",
            "        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE",
            "        and True",
        ),
        (
            "cases suite-root inode identity removed",
            "        and identity == expected_identity",
            "        and True",
        ),
    )


def _cases_suite_root_safety_mutations() -> tuple[Mutation, ...]:
    """Bind supervisor cases to the same canonical private suite root."""
    cases = "devcontainer_image_selftest_supervisor_cases"
    return tuple(
        (
            label,
            cases,
            ROOT_CANONICAL_VALUE if "canonical tmp" in label else CASES_ROOT_BLOCK,
            (
                _replace_in_block(ROOT_CANONICAL_VALUE, old, new)
                if "canonical tmp" in label
                else _replace_in_block(CASES_ROOT_BLOCK, old, new)
            ),
        )
        for label, old, new in _cases_suite_root_changes()
    )


def _spawn_handler_mutations() -> tuple[Mutation, ...]:
    """Bind the approved handler grammar and all four SC2064 trap expansions."""
    key = "devcontainer_image_selftest"
    validation = (
        (
            "spawn handler name grammar removed",
            key,
            HANDLER_VALIDATION_BLOCK,
            _replace_in_block(
                HANDLER_VALIDATION_BLOCK,
                '[[ "$handler" =~ ^[a-z_][a-z0-9_]*$ ]]',
                "true",
            ),
        ),
        (
            "spawn handler function binding removed",
            key,
            HANDLER_VALIDATION_BLOCK,
            _replace_in_block(HANDLER_VALIDATION_BLOCK, 'declare -F "$handler" >/dev/null', "true"),
        ),
    )
    trap_lines = (
        ("spawn EXIT handler binding removed", '  trap "$handler \\$?" EXIT'),
        ("spawn HUP handler binding removed", '  trap "$handler 129" HUP'),
        ("spawn INT handler binding removed", '  trap "$handler 130" INT'),
        ("spawn TERM handler binding removed", '  trap "$handler 143" TERM'),
    )
    traps = tuple((label, key, line, "  true") for label, line in trap_lines)
    return (*validation, *traps)


def _embedded_exec_mutations(key: str, prefix: str, block: str) -> tuple[Mutation, ...]:
    """Bind one embedded Python execv to protected Bash and exact argv."""
    changes = (
        (f"{prefix} execv removed", "os.execv(", "os.spawnv(os.P_WAIT, "),
        (f"{prefix} Bash executable changed", 'os.execv("/bin/bash"', 'os.execv("/bin/false"'),
        (f"{prefix} Bash argv0 changed", '["/bin/bash",', '["bash",'),
        (f"{prefix} protected Bash flag removed", '"-p",', '"-c",'),
        (f"{prefix} option terminator removed", '"--",', '"-c",'),
    )
    return tuple(
        (label, key, block, _replace_in_block(block, old, new)) for label, old, new in changes
    )


def _python_launcher_mutations(
    key: str, prefix: str, block: str, *, setsid: bool
) -> tuple[Mutation, ...]:
    """Bind one embedded-code launcher to isolated system Python."""
    changes = [
        (f"{prefix} Python interpreter changed", "/usr/bin/python3", "/bin/false"),
        (f"{prefix} no-bytecode flag removed", " -B ", " --version "),
        (f"{prefix} isolation flag removed", " -I ", " --version "),
        (f"{prefix} site-import refusal removed", " -S ", " --version "),
        (f"{prefix} command-string mode removed", " -c ", " --version "),
    ]
    if setsid:
        changes.append((f"{prefix} session isolation removed", "/usr/bin/setsid ", ""))
    return tuple(
        (label, key, block, _replace_in_block(block, old, new)) for label, old, new in changes
    )


def _embedded_launcher_mutations() -> tuple[Mutation, ...]:
    """Return scoped mutations for both Python-to-protected-Bash launchers."""
    cases = "devcontainer_image_selftest_cases"
    signal = "devcontainer_image_signal_selftest"
    return (
        *_embedded_exec_mutations(cases, "allocation launcher", CASES_EMBEDDED_EXEC_BLOCK),
        *_python_launcher_mutations(
            cases, "allocation launcher", CASES_PYTHON_LAUNCH_BLOCK, setsid=False
        ),
        *_embedded_exec_mutations(signal, "signal launcher", SIGNAL_EMBEDDED_EXEC_BLOCK),
        *_python_launcher_mutations(
            signal, "signal launcher", SIGNAL_PYTHON_LAUNCH_BLOCK, setsid=True
        ),
    )


def _build_tripwire_mutations() -> tuple[Mutation, ...]:
    """Prove both intentionally uncalled build callbacks remain must-not-fire tripwires."""
    key = "devcontainer_image_selftest_cases"
    callback = '    build_image() { : >"$marker"; }'
    replacement = "    true"
    return (
        (
            "replaced-lock build tripwire removed",
            key,
            REPLACED_LOCK_TRIPWIRE_BLOCK,
            _replace_in_block(REPLACED_LOCK_TRIPWIRE_BLOCK, callback, replacement),
        ),
        (
            "missing-lock build tripwire removed",
            key,
            MISSING_LOCK_TRIPWIRE_BLOCK,
            _replace_in_block(MISSING_LOCK_TRIPWIRE_BLOCK, callback, replacement),
        ),
    )


def _cases_source_read_mutations() -> tuple[Mutation, ...]:
    """Bind the cases source to regular owned bounded descriptor bytes."""
    key = "devcontainer_image_selftest_supervisor"
    changes = (
        (
            "cases source metadata regular binding removed",
            "        stat.S_ISREG(metadata.st_mode)",
            "        True",
        ),
        (
            "cases source metadata link binding removed",
            "        and metadata.st_nlink == 1",
            "        and True",
        ),
        (
            "cases source metadata owner binding removed",
            "        and metadata.st_uid == os.getuid()",
            "        and True",
        ),
        (
            "cases source metadata group binding removed",
            "        and metadata.st_gid == os.getgid()",
            "        and True",
        ),
        (
            "cases source metadata mode binding removed",
            "        and stat.S_IMODE(metadata.st_mode) == CASES_MODE",
            "        and True",
        ),
        (
            "cases source metadata byte bound removed",
            "        and 0 < metadata.st_size <= CASES_MAX_BYTES",
            "        and True",
        ),
        (
            "cases source read step bound removed",
            "    for _step in range(CASES_READ_STEPS):",
            "    for _step in iter(int, 1):",
        ),
        (
            "cases source descriptor pread binding removed",
            "        chunk = os.pread(descriptor, 4096, offset)",
            "        chunk = os.read(descriptor, 4096)",
        ),
        (
            "cases source complete-size postcondition removed",
            "    if len(source) != metadata.st_size or len(source) > CASES_MAX_BYTES:",
            "    if False:",
        ),
    )
    return tuple(
        (
            label,
            key,
            CASES_SOURCE_READ_BLOCK,
            _replace_in_block(CASES_SOURCE_READ_BLOCK, old, new),
        )
        for label, old, new in changes
    )


def _cases_loader_binding_mutations() -> tuple[Mutation, ...]:
    """Bind cases compilation to authenticated source and namespace bytes."""
    key = "devcontainer_image_selftest_supervisor"
    changes = (
        (
            "cases loader bound source read removed",
            "        source = _read_cases_source(descriptor)",
            "        source = b''",
        ),
        (
            "cases loader pre-exec digest binding removed",
            "        if digest != CASES_RAW_SHA256:",
            "        if False:",
        ),
        (
            "cases loader namespace name binding removed",
            '            "__name__": "_ra8_supervisor_cases",',
            '            "__name__": "__main__",',
        ),
        (
            "cases loader namespace filename binding removed",
            '            "__file__": f"/proc/self/fd/{descriptor}",',
            '            "__file__": "supervisor_cases.py",',
        ),
        (
            "cases loader grant version binding removed",
            '            "_RA8_SUPERVISOR_CASES_VERSION": 1,',
            '            "_RA8_SUPERVISOR_CASES_VERSION": 0,',
        ),
        (
            "cases loader authenticated exec removed",
            "        exec(  # noqa: S102 -- exact digest-bound source-only FD",
            "        eval(  # exact mutation",
        ),
        (
            "cases loader compile source binding removed",
            "            compile(source,",
            "            compile(b'',",
        ),
        (
            "cases loader compile filename binding removed",
            'compile(source, namespace["__file__"],',
            'compile(source, "supervisor_cases.py",',
        ),
        (
            "cases loader compile exec-mode binding removed",
            'namespace["__file__"], "exec")',
            'namespace["__file__"], "eval")',
        ),
    )
    return tuple(
        (label, key, CASES_LOADER_BLOCK, _replace_in_block(CASES_LOADER_BLOCK, old, new))
        for label, old, new in changes
    )


def _cases_loader_postcondition_mutations() -> tuple[Mutation, ...]:
    """Bind grant consumption, stable source, dispatch, and descriptor close."""
    key = "devcontainer_image_selftest_supervisor"
    changes = (
        (
            "cases loader grant exact consumption removed",
            '        grant = namespace.pop("_RA8_SUPERVISOR_CASES_VERSION", None)',
            "        grant = 1",
        ),
        (
            "cases loader grant absence postcondition removed",
            '        if grant != 1 or "_RA8_SUPERVISOR_CASES_VERSION" in namespace:',
            "        if grant != 1:",
        ),
        (
            "cases loader post-exec same-FD digest removed",
            "        if hashlib.sha256(_read_cases_source(descriptor)).hexdigest() != digest:",
            "        if False:",
        ),
        (
            "cases loader callable dispatch binding removed",
            "        if not callable(dispatch):",
            "        if False:",
        ),
        (
            "cases loader dispatch namespace identity removed",
            "        if dispatch.__globals__ is not namespace:",
            "        if False:",
        ),
        (
            "cases loader descriptor final-close removed",
            "    finally:\n        os.close(descriptor)",
            "    finally:\n        pass",
        ),
    )
    return tuple(
        (label, key, CASES_LOADER_BLOCK, _replace_in_block(CASES_LOADER_BLOCK, old, new))
        for label, old, new in changes
    )

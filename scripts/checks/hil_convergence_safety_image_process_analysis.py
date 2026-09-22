# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Source analysis for the devcontainer image process-authority policy."""

from __future__ import annotations

import ast
import re

import hil_convergence_safety_image_lock_receipts as image_lock_receipts
import hil_convergence_safety_image_process_policy as catalog
import hil_convergence_safety_image_subreaper_policy as subreaper_policy

CROSS_LANGUAGE_SCOPED_TOKENS = catalog.CROSS_LANGUAGE_SCOPED_TOKENS
PROCESS_MODULE_SEMANTIC_TOKENS = catalog.PROCESS_MODULE_SEMANTIC_TOKENS
SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS = catalog.SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS
SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS = catalog.SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS
SUPERVISOR_SCOPED_LOADER_TOKENS = catalog.SUPERVISOR_SCOPED_LOADER_TOKENS
SUPERVISOR_SEMANTIC_PROCESS_TOKENS = catalog.SUPERVISOR_SEMANTIC_PROCESS_TOKENS
TRIPWIRE_LABELS = catalog.TRIPWIRE_LABELS
TRIPWIRE_PATTERN = catalog.TRIPWIRE_PATTERN


def _cross_language_finding(key: str, owner: str, token: str) -> str:
    """Return one exact cross-language source diagnostic."""
    return f"devcontainer image source policy: {key}:{owner} token is not unique: {token}"


def _scoped_finding(authority: str, function: str, kind: str, token: str) -> str:
    """Return one exact function-scoped semantic diagnostic."""
    return f"{authority}: {function} {kind} token is not unique: {token}"


def _precomputed_semantic_findings(label: str) -> tuple[str, ...] | None:
    """Return findings owned by split or non-Python process policies."""
    if (subreaper := subreaper_policy.semantic_findings(label)) is not None:
        return subreaper
    if label in TRIPWIRE_LABELS:
        return ("devcontainer image source policy: build tripwire count drifted",)
    return image_lock_receipts.semantic_process_findings(label)


def semantic_process_findings(label: str) -> tuple[str, ...] | None:
    """Return the exact focused diagnostic for one process mutation label."""
    if (precomputed := _precomputed_semantic_findings(label)) is not None:
        return precomputed
    if (token := PROCESS_MODULE_SEMANTIC_TOKENS.get(label)) is not None:
        return (
            "devcontainer image process helper: required process-authority token "
            f"is not unique: {token}",
        )
    for candidate, key, owner, token in CROSS_LANGUAGE_SCOPED_TOKENS:
        if label == candidate:
            return (_cross_language_finding(key, owner, token),)
    scoped = (
        (SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS, "devcontainer image supervisor cases"),
        (SUPERVISOR_SCOPED_LOADER_TOKENS, "devcontainer image supervisor"),
    )
    for specifications, authority in scoped:
        if (spec := specifications.get(label)) is not None:
            return (_scoped_finding(authority, *spec),)
    token = SUPERVISOR_SEMANTIC_PROCESS_TOKENS.get(label)
    authority = "devcontainer image supervisor"
    if token is None:
        token = SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS.get(label)
        authority = "devcontainer image supervisor cases"
    if token is None:
        return None
    findings = (f"{authority}: required process-authority token is not unique: {token}",)
    if label == "bound-exit supervisor pre-spawn signal block removed":
        findings += ("devcontainer image supervisor: subreaper cleanup order drifted",)
    return findings


def _exact_tokens(source: str, label: str, tokens: tuple[str, ...]) -> list[str]:
    """Require every Python process-authority token exactly once."""
    return [
        f"{label}: required process-authority token is not unique: {token}"
        for token in tokens
        if source.count(token) != 1
    ]


def _source_ordered(source: str, anchors: tuple[str, ...]) -> bool:
    """Find repeated anchors only after the preceding authority."""
    position = -1
    for anchor in anchors:
        position = source.find(anchor, position + 1)
        if position < 0:
            return False
    return True


def _function_source(source: str, name: str) -> str | None:
    """Return one complete top-level Python function from parsed source."""
    try:
        module = ast.parse(source)
    except SyntaxError:
        return None
    scope = module.body
    target = name
    if "." in name:
        class_name, target = name.split(".", 1)
        classes = [
            node
            for node in module.body
            if isinstance(node, ast.ClassDef) and node.name == class_name
        ]
        if len(classes) != 1:
            return None
        scope = classes[0].body
    matches = [
        node
        for node in scope
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == target
    ]
    if len(matches) != 1:
        return None
    function = matches[0]
    if function.end_lineno is None:
        return None
    lines = source.splitlines(keepends=True)
    return "".join(lines[function.lineno - 1 : function.end_lineno])


def _bash_function_source(source: str, name: str) -> str | None:
    """Return one complete top-level Bash function body."""
    opening = re.compile(rf"(?m)^{re.escape(name)}\(\) \{{\n")
    matches = list(opening.finditer(source))
    if len(matches) != 1:
        return None
    start = matches[0].start()
    match = re.search(r"(?m)^}\s*$", source[matches[0].end() :])
    if match is None:
        return None
    end = matches[0].end() + match.end()
    return source[start:end]


def _owner_source(source: str, key: str, owner: str) -> str | None:
    """Resolve one Python or Bash semantic owner without widening its scope."""
    if owner == "<module>":
        return source
    if key in (
        "devcontainer_image_selftest_process",
        "devcontainer_image_selftest_supervisor",
        "devcontainer_image_selftest_supervisor_cases",
    ):
        return _function_source(source, owner)
    body = _bash_function_source(source, owner)
    if body is not None and owner == "run_bound_exit_supervisor":
        lines = body.splitlines()
        return "\n".join(
            line
            for index, line in enumerate(lines)
            if line.rstrip().endswith("|| status=$?")
            or (
                line.rstrip().endswith("\\")
                and index + 1 < len(lines)
                and lines[index + 1].rstrip().endswith("|| status=$?")
            )
        )
    return body


def source_errors(inputs: dict[str, str]) -> list[str]:
    """Bind fixed launchers, roots, entry FDs, traps, and tripwires by owner."""
    errors = _exact_tokens(
        inputs["devcontainer_image_selftest_process"],
        "devcontainer image process helper",
        tuple(PROCESS_MODULE_SEMANTIC_TOKENS.values()),
    )
    grouped: dict[tuple[str, str], list[str]] = {}
    for _label, key, owner, token in CROSS_LANGUAGE_SCOPED_TOKENS:
        grouped.setdefault((key, owner), []).append(token)
    for (key, owner), tokens in grouped.items():
        body = _owner_source(inputs[key], key, owner)
        if body is None:
            finding = f"devcontainer image source policy: {key}:{owner} owner is missing, "
            finding += "ambiguous, or unparseable"
            errors.append(finding)
            continue
        missing = [
            _cross_language_finding(key, owner, token) for token in tokens if body.count(token) != 1
        ]
        errors.extend(missing)
        positions = [body.find(token) for token in tokens]
        if not missing and positions != sorted(positions):
            errors.append(f"devcontainer image source policy: {key}:{owner} order drifted")
    tripwire_body = _bash_function_source(
        inputs["devcontainer_image_selftest_cases"], "selftest_managed_discovery_and_open"
    )
    if tripwire_body is None or tripwire_body.count(TRIPWIRE_PATTERN) != len(TRIPWIRE_LABELS):
        errors.append("devcontainer image source policy: build tripwire count drifted")
    return errors + image_lock_receipts.process_source_errors(inputs)


def _scoped_token_errors(
    source: str,
    authority: str,
    specifications: dict[str, tuple[str, str, str]],
) -> list[str]:
    """Bind each semantic token only inside its owning function."""
    errors = []
    functions: dict[str, list[tuple[str, str]]] = {}
    for function, kind, token in specifications.values():
        functions.setdefault(function, []).append((kind, token))
    for function, scoped in functions.items():
        body = _function_source(source, function)
        if body is None:
            finding = f"{authority}: {function} scoped owner is missing, ambiguous, or unparseable"
            errors.append(finding)
            continue
        missing = [
            _scoped_finding(authority, function, kind, token)
            for kind, token in scoped
            if body.count(token) != 1
        ]
        errors.extend(missing)
        positions = [body.find(token) for _, token in scoped]
        if not missing and positions != sorted(positions):
            errors.append(f"{authority}: {function} scoped proof order drifted")
    return errors


def _supervisor_token_errors(supervisor: str) -> list[str]:
    """Bind the supervisor to retained roots, entries, and child groups."""
    return _exact_tokens(
        supervisor,
        "devcontainer image supervisor",
        (
            'CASES_ARG = "--cases-fd"',
            "def _open_suite_root_authority(",
            "os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW",
            "def _close_suite_root_authority(",
            "def _anchored_root_descriptor(path: Path) -> int:",
            "def _open_entry_authority(",
            "def _census_cleanup_retry_selftest(",
            *dict.fromkeys(
                token
                for label, token in SUPERVISOR_SEMANTIC_PROCESS_TOKENS.items()
                if label not in subreaper_policy.MOVED_PROCESS_TOKENS
            ),
            "process != os.getpgrp()",
            "process != os.getsid(0)",
            "_suite_root_path_is_safe(resolved, metadata)",
            "identity == expected_identity",
            "_spawn_payload(entry_authority, (death_descriptor, root_descriptor))",
            "for private_descriptor in (death_descriptor, root_descriptor):",
            "def _load_cases_dispatch(descriptor: int)",
            "cases_dispatch = _load_cases_dispatch(cases_descriptor)",
            "hidden_status = _dispatch_controller(request_argv)",
            'f"{identity[0]}:{identity[1]}" != request_argv[6]',
            "anchored_request = SupervisorRequest(",
            "root_integrity = _close_suite_root_authority(descriptor, root, identity)",
        ),
    )


def _supervisor_cases_token_errors(cases_source: str) -> list[str]:
    """Bind authenticated cases to their adversarial cleanup directions."""
    return _exact_tokens(
        cases_source,
        "devcontainer image supervisor cases",
        (
            *SUPERVISOR_CASES_SEMANTIC_PROCESS_TOKENS.values(),
            "!= CASES_LOAD_VERSION:",
            'message = "supervisor cases module is source-only"',
            'identity = f"{metadata.st_dev}:{metadata.st_ino}"\n        resolved = root.resolve',
            "def _controller_isolation_selftest(",
            'root_descriptor, "0:0", root / "controller-wrong-identity.status"',
            'sibling = root / "controller-sibling"',
            "def _reap_cleanup_retry_selftest(",
            'original_reap = vars(BoundGroup)["_reap"]',
            'type.__setattr__(BoundGroup, "_reap", fail_reap)',
            "and not supervisor.cleaning\n"
            "                and supervisor.entry_descriptor is not None",
            "observation_injected = True\n            _inject_observation_failure()",
            "succeeded = observation_injected and supervisor.pid is not None",
            "def _open_validated_root(",
            'f"{opened_identity[0]}:{opened_identity[1]}" != expected_identity',
            "anchored_root = _anchored_root_path(descriptor)",
            "root_integrity = _close_suite_root_authority(descriptor, root, opened_identity)",
            "status = INTEGRITY_REFUSAL_STATUS",
        ),
    )


def _supervisor_order_errors(supervisor: str) -> list[str]:
    """Require root, group, and controller proofs in fail-closed order."""
    orders = (
        (
            "def _install_interruption_handlers(supervisor: BoundGroup) -> None:",
            "def _supervise(",
            "old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)",
            "_install_interruption_handlers(supervisor)",
            "supervisor.spawn(source_descriptor, launch)",
        ),
        (
            "process = os.getpid()",
            "process != os.getpgrp()",
            "for managed in MANAGED_SIGNALS:",
            "child = _spawn_payload(",
            "select.select((death_descriptor,), (), (), POLL_SECONDS)",
            "for private_descriptor in (death_descriptor, root_descriptor):",
            "os.killpg(os.getpgrp(), signal.SIGKILL)",
        ),
        (
            "descriptor, identity = _open_suite_root_authority(root)",
            'f"{identity[0]}:{identity[1]}" != request_argv[6]',
            "anchored_request = SupervisorRequest(",
            "result = _supervise(anchored_request)",
            "root_integrity = _close_suite_root_authority(descriptor, root, identity)",
        ),
    )
    if any(not _source_ordered(supervisor, order) for order in orders):
        return ["devcontainer image supervisor: cleanup proof order drifted"]
    return []


def supervisor_errors(supervisor: str, cases_source: str, process_source: str) -> list[str]:
    """Return retained-root and child-process findings for Python helpers."""
    return (
        _supervisor_token_errors(supervisor)
        + _supervisor_cases_token_errors(cases_source)
        + _scoped_token_errors(
            supervisor, "devcontainer image supervisor", SUPERVISOR_SCOPED_LOADER_TOKENS
        )
        + _scoped_token_errors(
            cases_source,
            "devcontainer image supervisor cases",
            SUPERVISOR_CASES_SCOPED_PROCESS_TOKENS,
        )
        + _supervisor_order_errors(supervisor)
        + subreaper_policy.errors(supervisor, process_source)
    )

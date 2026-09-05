# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Semantic mutation and raw-path fixtures for image-lock convergence."""

from __future__ import annotations

import ast
import hashlib
from collections.abc import Callable

import hil_convergence_safety_image_lock_digest as image_lock_digest
import hil_convergence_safety_image_process_analysis as image_process_analysis
import hil_convergence_safety_process_mutations as process_mutations
import hil_convergence_safety_raw_digest_runtime as raw_digest_runtime
import hil_convergence_safety_runtime_cleanup as runtime_cleanup
import hil_convergence_safety_runtime_escape as runtime_escape
import hil_convergence_safety_runtime_fixtures as runtime_fixtures
import hil_convergence_safety_runtime_mutations as runtime_mutations

Mutation = tuple[str, str, str, str]
Scan = Callable[[dict[str, str]], list[str]]


class SemanticMutationError(ValueError):
    """A semantic mutation fixture no longer has one exact authority."""


def _scoped_owner_authority(key: str, owner: str) -> str | None:
    """Map one scoped Python owner to its production diagnostic authority."""
    return {
        (
            "devcontainer_image_selftest_supervisor",
            "_load_cases_dispatch",
        ): "devcontainer image supervisor",
        (
            "devcontainer_image_selftest_supervisor",
            "_read_cases_source",
        ): "devcontainer image supervisor",
        (
            "devcontainer_image_selftest_supervisor_cases",
            "_closed_controller_command",
        ): "devcontainer image supervisor cases",
        (
            "devcontainer_image_selftest_supervisor_cases",
            "_closed_controller_descriptor_selftest",
        ): "devcontainer image supervisor cases",
        (
            "devcontainer_image_selftest_supervisor_cases",
            "_refused_controller_launch",
        ): "devcontainer image supervisor cases",
        (
            "devcontainer_image_selftest_supervisor_cases",
            "_watchdog_expiry_runner",
        ): "devcontainer image supervisor cases",
    }.get((key, owner))


def _owner_mutation_findings(label: str) -> tuple[str, ...] | None:
    """Return exact diagnostics for owner absence and ambiguity fixtures."""
    prefixes = ("semantic owner renamed: ", "semantic owner duplicated: ")
    prefix = next((value for value in prefixes if label.startswith(value)), None)
    if prefix is None:
        return None
    key, separator, owner = label.removeprefix(prefix).partition(":")
    if not separator or not key or not owner:
        return None
    if (authority := _scoped_owner_authority(key, owner)) is not None:
        finding = f"{authority}: {owner} scoped owner is missing, ambiguous, or unparseable"
    else:
        finding = (
            f"devcontainer image source policy: {key}:{owner} "
            "owner is missing, ambiguous, or unparseable"
        )
    extras = {
        ("semantic owner renamed: devcontainer_image_selftest_supervisor:_load_cases_dispatch"): (
            "devcontainer image supervisor: required process-authority token "
            "is not unique: def _load_cases_dispatch(descriptor: int)",
        ),
        (
            "semantic owner renamed: "
            "devcontainer_image_selftest_supervisor:_open_suite_root_authority"
        ): (
            "devcontainer image supervisor: required process-authority token "
            "is not unique: def _open_suite_root_authority(",
        ),
        (
            "semantic owner renamed: "
            "devcontainer_image_selftest_supervisor_cases:_closed_controller_command"
        ): (
            "devcontainer image supervisor cases: required process-authority token "
            "is not unique: def _closed_controller_command(",
        ),
    }
    return (*extras.get(label, ()), finding)


def semantic_image_findings(label: str, key: str) -> tuple[str, ...] | None:
    """Return the exact non-digest diagnostics required for one helper mutation."""
    if (owner_findings := _owner_mutation_findings(label)) is not None:
        return owner_findings
    expected = image_process_analysis.semantic_process_findings(label)
    if label == "closed-controller command definition removed" and expected is not None:
        return (
            *expected,
            "devcontainer image supervisor cases: _closed_controller_command "
            "scoped owner is missing, ambiguous, or unparseable",
        )
    if label == "suite anchor identity binding removed" and expected is not None:
        expected = (
            "devcontainer image lifecycle selftest: required process-authority token "
            'is not unique: "$(file_identity "$anchor")" == '
            '"$SELFTEST_SUITE_ANCHOR_IDENTITY"',
            *expected,
        )
    order_labels = {
        "bound-exit supervisor pre-spawn signal block removed",
        "bound-exit parent-death polling removed",
        "bound-exit controller group cleanup reduced to controller PID",
        "supervisor interruption-handler definition removed",
        "supervisor interruption-handler call removed",
    }
    if label in order_labels and expected is not None:
        return (*expected, "devcontainer image supervisor: cleanup proof order drifted")
    if label == "supervisor launcher bound entry environment removed" and expected is not None:
        return (
            *expected,
            "devcontainer image lifecycle selftest: required process-authority token "
            "is not unique: "
            'RA8_SELFTEST_BOUND_ENTRY="$1"',
        )
    if expected is not None:
        return expected
    if key == "devcontainer_image_lock_receipts" and label.endswith(" early success refused"):
        function = label.removeprefix("image lock ").removesuffix(" early success refused")
        expected = (f"image lock receipt: {function} can return success before its proof",)
    else:
        expected = runtime_fixtures.semantic_findings(label, key)
    return expected


def _rebind_main_mutation(inputs: dict[str, str]) -> dict[str, str]:
    """Rebind a mutated main image script into the raw digest authority."""
    changed = dict(inputs)
    main_digest = hashlib.sha256(changed["devcontainer_image"].encode("utf-8")).hexdigest()
    current_main = image_lock_digest.DEVCONTAINER_IMAGE_RAW_SHA256
    changed["image_lock_digest"] = _replace_exact_pin(
        changed["image_lock_digest"], current_main, main_digest, "the raw main pin"
    )
    return changed


def rebind_helper_mutation(inputs: dict[str, str], key: str) -> dict[str, str]:
    """Rebind one mutated image authority so only its semantic detector can fire."""
    if key == "devcontainer_image":
        return _rebind_main_mutation(inputs)
    current_pins = {
        "devcontainer_image_selftest": image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256,
        "devcontainer_image_bound_exit_selftest": (
            image_lock_digest.DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256
        ),
        "devcontainer_image_lock_receipts": (
            image_lock_digest.DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256
        ),
        "devcontainer_image_lock_selftest": (
            image_lock_digest.DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256
        ),
        "devcontainer_image_selftest_cases": (
            image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256
        ),
        "devcontainer_image_signal_selftest": (
            image_lock_digest.DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256
        ),
        "devcontainer_image_selftest_process": (
            image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256
        ),
        "devcontainer_image_selftest_supervisor": (
            image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256
        ),
        "devcontainer_image_selftest_supervisor_cases": (
            image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256
        ),
    }
    current_helper = current_pins.get(key)
    if current_helper is None:
        message = f"unsupported semantic helper mutation authority: {key}"
        raise SemanticMutationError(message)
    helper_digest = hashlib.sha256(inputs[key].encode("utf-8")).hexdigest()
    changed = dict(inputs)
    if key == "devcontainer_image_selftest_supervisor_cases":
        return _rebind_supervisor_cases_mutation(changed, helper_digest)
    if key == "devcontainer_image_selftest_process":
        return _rebind_process_mutation(changed, helper_digest)
    if changed["devcontainer_image"].count(current_helper) != 1:
        message = "semantic helper mutation cannot rebind the main helper pin"
        raise SemanticMutationError(message)
    changed["devcontainer_image"] = changed["devcontainer_image"].replace(
        current_helper,
        helper_digest,
    )
    main_digest = hashlib.sha256(changed["devcontainer_image"].encode("utf-8")).hexdigest()
    authority = changed["image_lock_digest"]
    current_main = image_lock_digest.DEVCONTAINER_IMAGE_RAW_SHA256
    if authority.count(current_helper) != 1 or authority.count(current_main) != 1:
        message = "semantic helper mutation cannot rebind the raw digest authority"
        raise SemanticMutationError(message)
    changed["image_lock_digest"] = authority.replace(
        current_helper,
        helper_digest,
    ).replace(current_main, main_digest)
    return changed


def _replace_exact_pin(source: str, old: str, new: str, context: str) -> str:
    """Replace one and only one raw digest in a named authority."""
    if source.count(old) != 1:
        message = f"semantic cases mutation cannot rebind {context}"
        raise SemanticMutationError(message)
    return source.replace(old, new)


def _rebind_supervisor_cases_mutation(changed: dict[str, str], cases_digest: str) -> dict[str, str]:
    """Cascade one cases mutation through supervisor, main, and raw authority."""
    old_cases = image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256
    old_supervisor = image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256
    old_main = image_lock_digest.DEVCONTAINER_IMAGE_RAW_SHA256
    supervisor = _replace_exact_pin(
        changed["devcontainer_image_selftest_supervisor"],
        old_cases,
        cases_digest,
        "the supervisor cases pin",
    )
    changed["devcontainer_image_selftest_supervisor"] = supervisor
    supervisor_digest = hashlib.sha256(supervisor.encode("utf-8")).hexdigest()
    main = _replace_exact_pin(
        changed["devcontainer_image"], old_cases, cases_digest, "the main cases pin"
    )
    main = _replace_exact_pin(main, old_supervisor, supervisor_digest, "the main supervisor pin")
    changed["devcontainer_image"] = main
    main_digest = hashlib.sha256(main.encode("utf-8")).hexdigest()
    authority = _replace_exact_pin(
        changed["image_lock_digest"], old_cases, cases_digest, "the raw cases pin"
    )
    authority = _replace_exact_pin(
        authority, old_supervisor, supervisor_digest, "the raw supervisor pin"
    )
    changed["image_lock_digest"] = _replace_exact_pin(
        authority, old_main, main_digest, "the raw main pin"
    )
    return changed


def _rebind_process_mutation(changed: dict[str, str], process_digest: str) -> dict[str, str]:
    """Cascade one process mutation through supervisor, main, and raw authority."""
    old_process = image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256
    old_supervisor = image_lock_digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256
    old_main = image_lock_digest.DEVCONTAINER_IMAGE_RAW_SHA256
    supervisor = _replace_exact_pin(
        changed["devcontainer_image_selftest_supervisor"],
        old_process,
        process_digest,
        "the supervisor process pin",
    )
    changed["devcontainer_image_selftest_supervisor"] = supervisor
    supervisor_digest = hashlib.sha256(supervisor.encode("utf-8")).hexdigest()
    main = _replace_exact_pin(
        changed["devcontainer_image"], old_process, process_digest, "the main process pin"
    )
    main = _replace_exact_pin(main, old_supervisor, supervisor_digest, "the main supervisor pin")
    changed["devcontainer_image"] = main
    main_digest = hashlib.sha256(main.encode("utf-8")).hexdigest()
    authority = _replace_exact_pin(
        changed["image_lock_digest"], old_process, process_digest, "the raw process pin"
    )
    authority = _replace_exact_pin(
        authority, old_supervisor, supervisor_digest, "the raw supervisor pin"
    )
    changed["image_lock_digest"] = _replace_exact_pin(
        authority, old_main, main_digest, "the raw main pin"
    )
    return changed


def process_authority_mutations() -> tuple[Mutation, ...]:
    """Return every process mutation from its focused catalog."""
    return process_mutations.process_authority_mutations()


def _replace_image_lock_authority(source: str, name: str, value: str | None) -> str:
    """Replace or remove one direct raw-byte authority assignment in source text."""
    tree = ast.parse(source)
    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id == name
    ]
    if len(matches) != 1:
        message = f"raw-byte authority assignment is not unique: {name}"
        raise SemanticMutationError(message)
    node = matches[0]
    lines = source.splitlines(keepends=True)
    replacement = [] if value is None else [f'{name} = "{value}"\n']
    lines[node.lineno - 1 : node.end_lineno] = replacement
    return "".join(lines)


def image_lock_digest_pin_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Drive wrong and missing raw-byte pins through the production scan."""
    cases = []
    source = inputs["image_lock_digest"]
    for name in image_lock_digest.pin_names():
        wrong = dict(inputs)
        wrong["image_lock_digest"] = _replace_image_lock_authority(source, name, "0" * 64)
        cases.append((f"wrong image-lock raw pin fires: {name}", bool(scan(wrong))))
        missing = dict(inputs)
        missing["image_lock_digest"] = _replace_image_lock_authority(source, name, None)
        cases.append((f"missing image-lock raw pin fires: {name}", bool(scan(missing))))
    return cases


def _semantic_aggregator_tokens() -> tuple[tuple[str, str, str], ...]:
    """Bind the semantic coordinator to every focused mutation provider."""
    semantic = "semantic_mutations", "hil convergence semantic selftest"
    return (
        (*semantic, "import " + "hil_convergence_safety_raw_digest_runtime as raw_digest_runtime"),
        (*semantic, "import " + "hil_convergence_safety_runtime_cleanup as runtime_cleanup"),
        (*semantic, "import " + "hil_convergence_safety_runtime_escape as runtime_escape"),
        (*semantic, "import " + "hil_convergence_safety_runtime_mutations as runtime_mutations"),
        (*semantic, "import " + "hil_convergence_safety_process_mutations as process_mutations"),
        (
            *semantic,
            "import " + "hil_convergence_safety_image_process_analysis as image_process_analysis",
        ),
        (*semantic, "raw_digest_runtime." + "cases(inputs)"),
        (*semantic, "runtime_cleanup." + "cases(inputs)"),
        (*semantic, "runtime_escape." + "cases(inputs)"),
        (*semantic, "runtime_mutations." + "runtime_cases(inputs)"),
        (*semantic, "process_mutations." + "process_authority_mutations()"),
        (*semantic, "image_process_analysis." + "semantic_process_findings(label)"),
    )


def _runtime_aggregator_tokens() -> tuple[tuple[str, str, str], ...]:
    """Bind runtime cleanup, mutation, and escape providers."""
    return (
        (
            "runtime_cleanup",
            "runtime cleanup",
            "import hil_convergence_safety_runtime_loader as runtime_loader",
        ),
        ("runtime_cleanup", "runtime cleanup", "runtime_loader.cases(inputs)"),
        (
            "runtime_mutations",
            "runtime mutations",
            "import hil_convergence_safety_runtime_launcher as runtime_launcher",
        ),
        (
            "runtime_mutations",
            "runtime mutations",
            "import hil_convergence_safety_runtime_sources as runtime_sources",
        ),
        ("runtime_mutations", "runtime mutations", "_write_sources = runtime_sources.publish"),
        ("runtime_mutations", "runtime mutations", "runtime_launcher.launch("),
        (
            "runtime_escape",
            "runtime escape",
            "from hil_convergence_safety_runtime_mutations import (",
        ),
    )


def _process_aggregator_tokens() -> tuple[tuple[str, str, str], ...]:
    """Bind process analysis and mutation catalogs to their dependencies."""
    return (
        (
            "image_process_analysis",
            "image process analysis",
            "import hil_convergence_safety_image_process_policy as catalog",
        ),
        (
            "image_process_analysis",
            "image process analysis",
            "import hil_convergence_safety_image_subreaper_policy as subreaper_policy",
        ),
        (
            "image_process_analysis",
            "image process analysis",
            "CROSS_LANGUAGE_SCOPED_TOKENS = catalog.CROSS_LANGUAGE_SCOPED_TOKENS",
        ),
        (
            "image_process_analysis",
            "image process analysis",
            "subreaper_policy.errors(supervisor, process_source)",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "import hil_convergence_safety_process_source_fixtures as process_source_fixtures",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "import hil_convergence_safety_runtime_fixtures as runtime_fixtures",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "import hil_convergence_safety_source_fixtures as source_fixtures",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "process_source_fixtures.process_authority_mutations()",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "runtime_fixtures.process_authority_mutations()",
        ),
        (
            "process_mutations",
            "process mutation catalog",
            "*source_fixtures.process_authority_mutations(),",
        ),
    )


def _image_dispatch_aggregator_tokens() -> tuple[tuple[str, str, str], ...]:
    """Bind the image harness, its public consumer, and the shell loader."""
    return (
        (
            "image_harness_policy",
            "image harness policy",
            "import " + "hil_convergence_safety_image_process_analysis as image_process_analysis",
        ),
        (
            "image_harness_policy",
            "image harness policy",
            "image_process_analysis.supervisor_errors(supervisor, cases_source, process_source)",
        ),
        (
            "image_harness_policy",
            "image harness policy",
            "image_process_analysis.source_errors(inputs)",
        ),
        (
            "hil_convergence_entry",
            "image harness consumer",
            "import hil_convergence_safety_image_harness_policy as image_harness_policy",
        ),
        (
            "hil_convergence_entry",
            "image harness consumer",
            "image_harness_policy.errors(inputs)",
        ),
        (
            "devcontainer_image",
            "bound-exit helper loader",
            "SELFTEST_BOUND_EXIT_RAW_SHA256=",
        ),
        (
            "devcontainer_image",
            "bound-exit helper loader",
            'source_approved_selftest_helper "$SCRIPT_DIR/'
            'devcontainer_image_bound_exit_selftest.bash"',
        ),
        (
            "devcontainer_image",
            "bound-exit helper loader",
            '"$SELFTEST_BOUND_EXIT_RAW_SHA256"',
        ),
    )


def _aggregator_tokens() -> tuple[tuple[str, str, str], ...]:
    """Return exact split-module consumer tokens and their checker labels."""
    return (
        *_semantic_aggregator_tokens(),
        *_runtime_aggregator_tokens(),
        *_process_aggregator_tokens(),
        *_image_dispatch_aggregator_tokens(),
    )


def aggregator_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Prove every split runtime test import and dispatch remains load-bearing."""
    cases = []
    for key, authority, token in _aggregator_tokens():
        source = inputs[key]
        if source.count(token) != 1:
            message = f"semantic aggregator fixture is not unique: {token}"
            raise SemanticMutationError(message)
        changed = dict(inputs)
        changed[key] = source.replace(token, "", 1)
        if key == "devcontainer_image":
            changed = _rebind_main_mutation(changed)
        expected = f"{authority}: required process-authority token is not unique: {token}"
        cases.append((f"semantic aggregator removal fires: {token}", scan(changed) == [expected]))
    return cases


def wsl_clock_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Prove WSL clock and keep-alive regressions cannot pass."""
    key = "wsl_role"
    source = inputs[key]
    safe = "waitsync 0 0 0 1"
    start = "- name: Start the Windows autostart task now"
    if source.count(safe) != 1 or source.count(start) != 1:
        message = "WSL clock or keep-alive fixture is not unique"
        raise SemanticMutationError(message)
    clock_changed = dict(inputs)
    clock_changed[key] = source.replace(safe, "waitsync 0 0.1 0.0 1", 1)
    start_changed = dict(inputs)
    start_changed[key] = source.replace(start, f"{start} later", 1)
    return [
        ("WSL slew-safe readiness threshold restoration fires", bool(scan(clock_changed))),
        ("WSL immediate keep-alive start removal fires", bool(scan(start_changed))),
    ]


def _fleet_import_cases() -> tuple[tuple[str, str, str, str, str], ...]:
    """Return split fleet import-removal mutation specifications."""
    return (
        ("fleet", "import fleet_capacity_client as fcc", "", "capacity import", "runtime imports"),
        (
            "fleet_reconcile",
            "import fleet_reconcile_process as frp",
            "",
            "process import",
            "runtime imports",
        ),
        (
            "fleet_reconcile",
            "import fleet_reconcile_arc_selftest as fras",
            "",
            "ARC selftest import",
            "runtime imports",
        ),
    )


def _fleet_selftest_source_cases() -> tuple[tuple[str, str, str, str, str], ...]:
    """Return split fleet selftest definition and dispatch mutations."""
    return (
        (
            "fleet_capacity_client",
            "def run_selftest(data: dict[str, Any]) -> list[str]:",
            "def _removed_selftest(data: dict[str, Any]) -> list[str]:",
            "capacity selftest definition",
            "executable selftests",
        ),
        (
            "fleet_reconcile_process",
            "def run_selftest() -> list[str]:",
            "def _removed_selftest() -> list[str]:",
            "process selftest definition",
            "executable selftests",
        ),
        (
            "fleet_reconcile_arc_selftest",
            "def run(apply_host: ApplyHost) -> list[str]:",
            "def _removed_run(apply_host: ApplyHost) -> list[str]:",
            "ARC selftest definition",
            "executable selftests",
        ),
        (
            "fleet_reconcile",
            "    failures.extend(frp.run_selftest())\n",
            "",
            "process selftest call",
            "executable selftests",
        ),
        (
            "fleet_reconcile",
            "    failures.extend(fras.run(apply_host))\n",
            "",
            "ARC selftest call",
            "executable selftests",
        ),
    )


def fleet_split_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Prove split fleet runtime imports, sources, and selftests are load-bearing."""
    results = []
    for key, token, replacement, label, diagnostic in (
        *_fleet_import_cases(),
        *_fleet_selftest_source_cases(),
    ):
        if inputs[key].count(token) != 1:
            message = f"split fleet mutation fixture is not unique: {key}:{token}"
            raise SemanticMutationError(message)
        changed = dict(inputs)
        changed[key] = inputs[key].replace(token, replacement, 1)
        expected = f"fleet split modules: {diagnostic} are not exact"
        results.append((f"fleet split {label} removal fires", expected in scan(changed)))
    return results


def fleet_guard_dispatch_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Prove guarded re-entry derives and passes its guardian capability."""
    mutations = (
        (
            "        guardian = _bench_guard_subprocess_kwargs("
            "args.command, fml.guardian_subprocess_kwargs)\n",
            "        guardian = {}\n",
            "guardian derivation",
        ),
        (
            "        return _run(guard, cwd=fm.REPO_ROOT, subprocess_kwargs=guardian)\n",
            "        return _run(guard, cwd=fm.REPO_ROOT)\n",
            "guardian pass-through",
        ),
    )
    expected = "fleet.py: selector/extra-var refusal is not before lock and inventory"
    results = []
    for old, new, label in mutations:
        if inputs["fleet"].count(old) != 1:
            message = f"guard dispatch mutation fixture is not unique: {label}"
            raise SemanticMutationError(message)
        changed = dict(inputs)
        changed["fleet"] = inputs["fleet"].replace(old, new, 1)
        results.append((f"fleet {label} mutation fires", expected in scan(changed)))
    return results


def fleet_activation_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Prove ARC activation check and final restore order stay load-bearing."""
    mutations = (
        (
            "    clean, changed = inspect_activation_host(data, host, run)\n",
            "    clean, changed = inspect_host(data, host, run)\n",
            "activation check",
        ),
        (
            '    restore = run(fleet_command(host, "restore"))\n',
            '    restore = run(fleet_command(host, "check"))\n',
            "final restore",
        ),
    )
    expected = "fleet reconciliation: ARC activation/check/restore order is not exact"
    source = inputs["fleet_reconcile"]
    start = source.index("def _activate_arc(")
    end = source.index("\ndef apply_host(", start)
    activation = source[start:end]
    results = []
    for old, new, label in mutations:
        if activation.count(old) != 1:
            message = f"ARC order mutation fixture is not unique: {label}"
            raise SemanticMutationError(message)
        changed = dict(inputs)
        changed["fleet_reconcile"] = source[:start] + activation.replace(old, new) + source[end:]
        results.append((f"fleet ARC {label} mutation fires", expected in scan(changed)))
    return results


def digest_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return raw digest pin and path-identity mutation cases."""
    return (
        image_lock_digest_pin_cases(inputs, scan)
        + raw_digest_runtime.cases(inputs)
        + runtime_mutations.runtime_cases(inputs)
        + runtime_cleanup.cases(inputs)
        + runtime_escape.cases(inputs)
    )

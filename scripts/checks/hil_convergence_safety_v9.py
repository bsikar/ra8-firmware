# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Semantic checks for public startup and live bench capabilities."""

from __future__ import annotations

import ast
import json
import os
import pwd
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import hil_convergence_safety_image_lock_receipts as image_lock_receipts
import hil_convergence_safety_policy as policy
import hil_convergence_safety_roles as roles
import yaml
from check_shebangs import (
    PINNED_INTERPRETER_BOUNDARIES,
    PRIVILEGED_BODY_CLOSE,
    PRIVILEGED_BODY_PREFIX,
)

CONTEXT_INPUT_FIELD_COUNT = 2
DEV_DIR = Path(__file__).resolve().parents[1] / "dev"
HIL_LIB = Path(__file__).resolve().parents[1] / "hil/lib"
for module_dir in (DEV_DIR, HIL_LIB):
    sys.path.insert(0, str(module_dir))

import bench_lock_broker as broker  # noqa: E402 -- DEV_DIR is inserted above
import bench_lock_capability as capability  # noqa: E402 -- DEV_DIR is inserted above
import bench_lock_verify as lock_verify  # noqa: E402 -- HIL_LIB is inserted above
import fleet_transaction_auth as transaction  # noqa: E402 -- DEV_DIR is inserted above


def _lock_verifier_errors(source: str) -> list[str]:
    """Require nonblocking descriptor classification and its live selftest."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return ["bench lock verifier: invalid Python"]
    digest = next(
        (
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "_has_script_digest"
        ),
        None,
    )
    expected = ast.parse("if not stat.S_ISREG(before.st_mode):\n    continue").body[0]
    matches = (
        []
        if digest is None
        else [node for node in ast.walk(digest) if ast.dump(node) == ast.dump(expected)]
    )
    errors = [] if len(matches) == 1 else ["bench lock verifier: non-regular fds may block"]
    run = next(
        (
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "run_selftest"
        ),
        None,
    )
    calls = [
        node
        for node in (() if run is None else ast.walk(run))
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "_live_descriptor_selftest"
    ]
    if len(calls) != 1:
        errors.append("bench lock verifier: live descriptor selftest is not load-bearing")
    return errors


def _canonical_context_inputs(source: str) -> set[str]:
    """Return exact paths from the image builder's canonical root-input table."""
    pattern = re.compile(
        r"(?ms)^  canonical_root_context_inputs\(\) \{\n"
        r"    cat <<'EOF'\n(.*?)^EOF\n  \}"
    )
    matches = pattern.findall(source)
    if len(matches) != 1:
        return set()
    rows = [line.split(maxsplit=1) for line in matches[0].splitlines() if line]
    if any(len(row) != CONTEXT_INPUT_FIELD_COUNT or not row[0].isdigit() for row in rows):
        return set()
    return {row[1] for row in rows}


def _context_scope_errors(
    defaults_source: str, transaction_source: str, image_source: str
) -> list[str]:
    """Require every asserted provisioning input to enter the exact archive."""
    try:
        defaults = yaml.safe_load(defaults_source)
        tasks = yaml.safe_load(transaction_source)
    except yaml.YAMLError:
        return ["dev box context: malformed defaults or transaction"]
    scopes = defaults.get("dev_box_context_paths") if isinstance(defaults, dict) else None
    if (
        not isinstance(scopes, list)
        or not scopes
        or any(not isinstance(scope, str) or not scope for scope in scopes)
        or not isinstance(tasks, list)
    ):
        return ["dev box context: archive scopes are missing or malformed"]
    matches = [
        task
        for task in tasks
        if isinstance(task, dict) and task.get("name") == "Assert the staged context arrived"
    ]
    required = matches[0].get("loop") if len(matches) == 1 else None
    if not isinstance(required, list) or any(not isinstance(path, str) for path in required):
        return ["dev box context: authoritative input census is missing or malformed"]
    consumed = _canonical_context_inputs(image_source)
    if not consumed:
        return ["dev box context: image root-input authority is missing or malformed"]

    def covered(path: str) -> bool:
        return any(path == scope or path.startswith(scope.rstrip("/") + "/") for scope in scopes)

    errors: list[str] = []
    missing = sorted(path for path in {*required, *consumed} if not covered(path))
    if missing:
        errors.append(
            "dev box context: required paths escape archive scopes: " + ", ".join(missing)
        )
    unasserted = sorted(consumed - set(required))
    if unasserted:
        errors.append(
            "dev box context: consumed root inputs escape the assertion: " + ", ".join(unasserted)
        )
    return errors


def _image_lock_tasks(
    tasks: list[object], names: tuple[str, ...]
) -> tuple[dict[str, dict[str, object]], list[str]]:
    """Return each required image-lock task exactly once."""
    found: dict[str, dict[str, object]] = {}
    errors: list[str] = []
    for name in names:
        matches = [task for task in tasks if isinstance(task, dict) and task.get("name") == name]
        if len(matches) == 1:
            found[name] = matches[0]
        else:
            errors.append(f"dev box image lock: task is not unique: {name}")
    return found, errors


def _normalized_conditions(task: dict[str, object]) -> set[str] | None:
    """Return an assert task's whitespace-normalized conditions."""
    assertion = task.get("ansible.builtin.assert")
    conditions = assertion.get("that") if isinstance(assertion, dict) else None
    if not isinstance(conditions, list) or not all(isinstance(item, str) for item in conditions):
        return None
    return {" ".join(item.split()) for item in conditions}


def _stat_task_is_exact(task: dict[str, object], path: str, register: str) -> bool:
    """Return whether a stat task pins its path, no-follow behavior, and result."""
    stat = task.get("ansible.builtin.stat")
    return (
        task.get("become") is True
        and isinstance(stat, dict)
        and stat.get("path") == path
        and stat.get("follow") is False
        and task.get("register") == register
    )


def _image_lock_directory_errors(named: dict[str, dict[str, object]]) -> list[str]:
    """Require refusal, convergence, and post-proof for the managed directory."""
    errors: list[str] = []
    path = "{{ dev_box_image_lock_dir }}"
    inspect_name = "Inspect the managed image lock directory without following links"
    if not _stat_task_is_exact(named[inspect_name], path, "dev_box_image_lock_dir_before"):
        errors.append("dev box image lock: directory inspection may follow links")
    refuse_name = "Refuse an unsafe managed image lock directory"
    required_refusal = {
        "not dev_box_image_lock_dir_before.stat.exists or "
        "(dev_box_image_lock_dir_before.stat.isdir and "
        "not dev_box_image_lock_dir_before.stat.islnk and "
        "dev_box_image_lock_dir_before.stat.uid == 0 and "
        "dev_box_image_lock_dir_before.stat.gid "
        "== (dev_box_image_lock_gid.stdout | int) and "
        "dev_box_image_lock_dir_before.stat.mode == '0750')"
    }
    if _normalized_conditions(named[refuse_name]) != required_refusal:
        errors.append("dev box image lock: unsafe directory refusal drifted")
    expected_directory = {
        "path": path,
        "state": "directory",
        "owner": "root",
        "group": "{{ dev_box_image_lock_gid.stdout }}",
        "mode": "0750",
    }
    create_name = "Create the managed image lock directory"
    if named[create_name].get("ansible.builtin.file") != expected_directory or not named[
        create_name
    ].get("become"):
        errors.append("dev box image lock: directory ownership or mode drifted")
    post_name = "Reinspect the converged managed image lock directory"
    if (
        not _stat_task_is_exact(named[post_name], path, "dev_box_image_lock_dir_after")
        or named[post_name].get("when") != "not ansible_check_mode"
    ):
        errors.append("dev box image lock: directory post-stat may follow links")
    proof_name = "Prove the managed image lock directory identity and permissions"
    required_proof = {
        "dev_box_image_lock_dir_after.stat.isdir",
        "not dev_box_image_lock_dir_after.stat.islnk",
        "dev_box_image_lock_dir_after.stat.uid == 0",
        "dev_box_image_lock_dir_after.stat.gid == (dev_box_image_lock_gid.stdout | int)",
        "dev_box_image_lock_dir_after.stat.mode == '0750'",
    }
    if (
        _normalized_conditions(named[proof_name]) != required_proof
        or named[proof_name].get("when") != "not ansible_check_mode"
    ):
        errors.append("dev box image lock: directory post-proof drifted")
    return errors


def _image_lock_file_errors(named: dict[str, dict[str, object]]) -> list[str]:
    """Require the stable single-link file authority and post-proof."""
    errors: list[str] = []
    lock_path = "{{ dev_box_image_lock_dir }}/devcontainer-image.lock"
    inspect_name = "Inspect the managed image lock file without following links"
    if not _stat_task_is_exact(named[inspect_name], lock_path, "dev_box_image_lock_before"):
        errors.append("dev box image lock: file inspection may follow links")
    refuse_name = "Refuse an unsafe managed image lock file"
    required_refusal = {
        "not dev_box_image_lock_before.stat.exists or "
        "(dev_box_image_lock_before.stat.isreg and "
        "not dev_box_image_lock_before.stat.islnk and "
        "dev_box_image_lock_before.stat.nlink == 1 and "
        "dev_box_image_lock_before.stat.uid == 0 and "
        "dev_box_image_lock_before.stat.gid "
        "== (dev_box_image_lock_gid.stdout | int) and "
        "dev_box_image_lock_before.stat.mode == '0660')"
    }
    if _normalized_conditions(named[refuse_name]) != required_refusal:
        errors.append("dev box image lock: unsafe file refusal drifted")
    expected_lock = {
        "path": lock_path,
        "state": "touch",
        "follow": False,
        "owner": "root",
        "group": "{{ dev_box_image_lock_gid.stdout }}",
        "mode": "0660",
        "access_time": "preserve",
        "modification_time": "preserve",
    }
    create_name = "Create the stable managed image lock file"
    if named[create_name].get("ansible.builtin.file") != expected_lock or not named[
        create_name
    ].get("become"):
        errors.append("dev box image lock: file ownership, mode, or inode stability drifted")
    post_name = "Reinspect the converged managed image lock"
    if (
        not _stat_task_is_exact(named[post_name], lock_path, "dev_box_image_lock_after")
        or named[post_name].get("when") != "not ansible_check_mode"
    ):
        errors.append("dev box image lock: file post-stat may follow links")
    proof_name = "Prove the managed image lock identity and permissions"
    required_conditions = {
        "dev_box_image_lock_after.stat.isreg",
        "not dev_box_image_lock_after.stat.islnk",
        "dev_box_image_lock_after.stat.nlink == 1",
        "dev_box_image_lock_after.stat.uid == 0",
        "dev_box_image_lock_after.stat.gid == (dev_box_image_lock_gid.stdout | int)",
        "dev_box_image_lock_after.stat.gid == dev_box_image_lock_dir_after.stat.gid",
        "dev_box_image_lock_after.stat.mode == '0660'",
    }
    if (
        _normalized_conditions(named[proof_name]) != required_conditions
        or named[proof_name].get("when") != "not ansible_check_mode"
    ):
        errors.append("dev box image lock: post-convergence identity proof drifted")
    return errors


def _image_lock_gid_errors(named: dict[str, dict[str, object]]) -> list[str]:
    """Require the non-root numeric primary-group authority."""
    resolve = named["Resolve the managed image lock numeric primary group"]
    command = resolve.get("ansible.builtin.command")
    expected = {"argv": ["/usr/bin/id", "-g", "--", "{{ dev_box_user }}"]}
    errors: list[str] = []
    if (
        resolve.get("become") is not True
        or command != expected
        or resolve.get("register") != "dev_box_image_lock_gid"
        or resolve.get("changed_when") is not False
        or resolve.get("check_mode") is not False
    ):
        errors.append("dev box image lock: numeric group resolution drifted")
    required = {
        "dev_box_image_lock_gid.stdout is regex('^[0-9]+$')",
        "(dev_box_image_lock_gid.stdout | int) > 0",
    }
    refusal = named["Refuse an unsafe managed image lock numeric primary group"]
    if _normalized_conditions(refusal) != required:
        errors.append("dev box image lock: unsafe numeric group refusal drifted")
    return errors


def _image_lock_marker_errors(named: dict[str, dict[str, object]]) -> list[str]:
    """Require the root-owned immutable numeric-group marker authority."""
    marker = "{{ dev_box_image_lock_dir }}/devcontainer-image.gid"
    return [
        *_image_lock_marker_inspection_errors(named, marker),
        *_image_lock_marker_convergence_errors(named, marker),
        *_image_lock_marker_proof_errors(named),
    ]


def _image_lock_marker_inspection_errors(
    named: dict[str, dict[str, object]], marker: str
) -> list[str]:
    """Require no-follow inspection and exact refusal of an unsafe marker."""
    errors: list[str] = []
    inspect = named["Inspect the managed image lock group marker without following links"]
    inspect_stat = inspect.get("ansible.builtin.stat")
    if (
        not _stat_task_is_exact(inspect, marker, "dev_box_image_lock_gid_marker_before")
        or not isinstance(inspect_stat, dict)
        or inspect_stat.get("get_checksum") is not True
        or inspect_stat.get("checksum_algorithm") != "sha256"
    ):
        errors.append("dev box image lock: group marker inspection may follow links")
    required_refusal = {
        "not dev_box_image_lock_gid_marker_before.stat.exists or "
        "(dev_box_image_lock_gid_marker_before.stat.isreg and "
        "not dev_box_image_lock_gid_marker_before.stat.islnk and "
        "dev_box_image_lock_gid_marker_before.stat.nlink == 1 and "
        "dev_box_image_lock_gid_marker_before.stat.uid == 0 and "
        "dev_box_image_lock_gid_marker_before.stat.gid == 0 and "
        "dev_box_image_lock_gid_marker_before.stat.mode == '0444' and "
        "dev_box_image_lock_gid_marker_before.stat.checksum "
        "== ((dev_box_image_lock_gid.stdout ~ '\\n') | hash('sha256')))"
    }
    refuse = named["Refuse an unsafe managed image lock group marker"]
    if _normalized_conditions(refuse) != required_refusal:
        errors.append("dev box image lock: unsafe group marker refusal drifted")
    return errors


def _image_lock_marker_convergence_errors(
    named: dict[str, dict[str, object]], marker: str
) -> list[str]:
    """Require atomic marker convergence and an exact post-stat."""
    errors: list[str] = []
    expected_copy = {
        "dest": marker,
        "content": "{{ dev_box_image_lock_gid.stdout }}\n",
        "owner": "root",
        "group": "root",
        "mode": "0444",
        "unsafe_writes": False,
    }
    create = named["Create the managed image lock numeric group marker atomically"]
    if (
        create.get("become") is not True
        or create.get("ansible.builtin.copy") != expected_copy
        or create.get("when")
        != "not ansible_check_mode or dev_box_image_lock_dir_before.stat.exists"
    ):
        errors.append("dev box image lock: group marker atomic convergence drifted")
    post = named["Reinspect the converged managed image lock group marker"]
    post_stat = post.get("ansible.builtin.stat")
    if (
        not _stat_task_is_exact(post, marker, "dev_box_image_lock_gid_marker_after")
        or not isinstance(post_stat, dict)
        or post_stat.get("get_checksum") is not True
        or post_stat.get("checksum_algorithm") != "sha256"
        or post.get("when") != "not ansible_check_mode"
    ):
        errors.append("dev box image lock: group marker post-stat drifted")
    return errors


def _image_lock_marker_proof_errors(
    named: dict[str, dict[str, object]],
) -> list[str]:
    """Require the marker, directory, and lock to share the approved identity."""
    required_proof = {
        "dev_box_image_lock_gid_marker_after.stat.isreg",
        "not dev_box_image_lock_gid_marker_after.stat.islnk",
        "dev_box_image_lock_gid_marker_after.stat.nlink == 1",
        "dev_box_image_lock_gid_marker_after.stat.uid == 0",
        "dev_box_image_lock_gid_marker_after.stat.gid == 0",
        "dev_box_image_lock_gid_marker_after.stat.mode == '0444'",
        "dev_box_image_lock_gid_marker_after.stat.checksum "
        "== ((dev_box_image_lock_gid.stdout ~ '\\n') | hash('sha256'))",
        "dev_box_image_lock_dir_after.stat.gid == (dev_box_image_lock_gid.stdout | int)",
        "dev_box_image_lock_after.stat.gid == (dev_box_image_lock_gid.stdout | int)",
    }
    proof = named["Prove the managed image lock group marker identity and content"]
    if (
        _normalized_conditions(proof) != required_proof
        or proof.get("when") != "not ansible_check_mode"
    ):
        return ["dev box image lock: group marker identity proof drifted"]
    return []


def _image_lock_environment_errors(named: dict[str, dict[str, object]]) -> list[str]:
    """Require both root image commands to consume the managed path explicitly."""
    expected_environment = {
        "RA8_CI_IMAGE": "{{ dev_box_ci_image }}",
        "RA8_IMAGE_LOCK_DIR": "{{ dev_box_image_lock_dir }}",
    }
    command_names = (
        "Prove the staleness check itself works, before trusting its verdict",
        "Build the gate image unless the cached one matches this context",
    )
    return [
        f"dev box image lock: explicit environment drifted: {name}"
        for name in command_names
        if named[name].get("environment") != expected_environment
    ]


def _image_lock_order_errors(tasks: list[object]) -> list[str]:
    """Require refusal-before-repair and selftest-before-use task ordering."""
    positions = {
        task["name"]: index
        for index, task in enumerate(tasks)
        if isinstance(task, dict) and isinstance(task.get("name"), str)
    }
    refusals = (
        "Refuse an unsafe managed image lock directory",
        "Refuse an unsafe managed image lock file",
        "Refuse an unsafe managed image lock group marker",
    )
    creations = (
        "Create the managed image lock directory",
        "Create the stable managed image lock file",
        "Create the managed image lock numeric group marker atomically",
    )
    required_pairs = (
        (
            "Resolve the managed image lock numeric primary group",
            "Refuse an unsafe managed image lock numeric primary group",
        ),
        (
            "Refuse an unsafe managed image lock numeric primary group",
            creations[0],
        ),
        *((refusal, creation) for refusal in refusals for creation in creations),
        (
            "Prove the managed image lock group marker identity and content",
            "Prove the staleness check itself works, before trusting its verdict",
        ),
        (
            "Prove the staleness check itself works, before trusting its verdict",
            "Build the gate image unless the cached one matches this context",
        ),
    )
    return [
        f"dev box image lock: required task order drifted: {before} before {after}"
        for before, after in required_pairs
        if positions.get(before, len(tasks)) >= positions.get(after, -1)
    ]


def _image_lock_profile_errors(tasks: list[object]) -> list[str]:
    """Refuse a profile lock authority now that canonical discovery is exact."""
    name = "Set the container runtime and the shared compiler cache for every shell"
    matches = [task for task in tasks if isinstance(task, dict) and task.get("name") == name]
    copy = matches[0].get("ansible.builtin.copy") if len(matches) == 1 else None
    content = copy.get("content") if isinstance(copy, dict) else None
    if not isinstance(content, str):
        return ["dev box image lock: shell profile task is missing"]
    if "RA8_IMAGE_LOCK_DIR" in content:
        return ["dev box image lock: shell profile is a second lock authority"]
    return []


def _image_lock_task_errors(tasks: list[object]) -> list[str]:
    """Require exact Ansible ownership, identity, and caller environments."""
    names = (
        "Resolve the managed image lock numeric primary group",
        "Refuse an unsafe managed image lock numeric primary group",
        "Inspect the managed image lock directory without following links",
        "Refuse an unsafe managed image lock directory",
        "Create the managed image lock directory",
        "Reinspect the converged managed image lock directory",
        "Prove the managed image lock directory identity and permissions",
        "Inspect the managed image lock file without following links",
        "Refuse an unsafe managed image lock file",
        "Create the stable managed image lock file",
        "Reinspect the converged managed image lock",
        "Prove the managed image lock identity and permissions",
        "Inspect the managed image lock group marker without following links",
        "Refuse an unsafe managed image lock group marker",
        "Create the managed image lock numeric group marker atomically",
        "Reinspect the converged managed image lock group marker",
        "Prove the managed image lock group marker identity and content",
        "Prove the staleness check itself works, before trusting its verdict",
        "Build the gate image unless the cached one matches this context",
    )
    named, errors = _image_lock_tasks(tasks, names)
    if errors:
        return errors
    return [
        *_image_lock_gid_errors(named),
        *_image_lock_directory_errors(named),
        *_image_lock_file_errors(named),
        *_image_lock_marker_errors(named),
        *_image_lock_environment_errors(named),
        *_image_lock_order_errors(tasks),
        *_image_lock_profile_errors(tasks),
    ]


def _image_lock_required_source_tokens() -> tuple[str, ...]:
    """Return the exact production image-lock authority tokens."""
    return (
        'ra8_bound_entry="${RA8_SELFTEST_BOUND_ENTRY-}"',
        "unset -v RA8_SELFTEST_BOUND_ENTRY",
        '[[ "${BASH_SOURCE[0]}" =~ ^/proc/self/fd/[0-9]+$ &&',
        '"$ra8_bound_entry" == /*/devcontainer_image.sh &&',
        '-f "$ra8_bound_entry" && ! -L "$ra8_bound_entry" &&',
        '"${BASH_SOURCE[0]}" -ef "$ra8_bound_entry"',
        '[[ "$ra8_bound_entry" == "$SCRIPT_DIR/devcontainer_image.sh" ]] || {',
        '"${BASH_SOURCE[0]}" -ef "$main_path"',
        'CANONICAL_IMAGE_LOCK_DIR="/var/cache/ra8-devcontainer-image-lock"',
        'IMAGE_LOCK_GROUP_GID=""',
        "validate_managed_image_lock_group_marker() {",
        "fd_size() {",
        "validate_managed_image_lock_dir() {",
        "validate_image_lock_file() {",
        "validate_opened_image_lock() {",
        "\n  resolve_image_lock() {",
        '    elif [[ -e "$canonical_dir" || -L "$canonical_dir" ||\n'
        '      -e "$canonical_dir/devcontainer-image.lock" ||\n'
        '      -L "$canonical_dir/devcontainer-image.lock" ||\n'
        '      -e "$canonical_dir/devcontainer-image.gid" ||\n'
        '      -L "$canonical_dir/devcontainer-image.gid" ]]; then',
        '\n      lock_dir="$canonical_dir"\n      IMAGE_LOCK_MANAGED=1\n',
        '[[ "$links" == "1" && "$owner" == "0" && "$group" == "0" && "$mode" == "444" ]]',
        '[[ "$marker_gid" =~ ^[0-9]+$ && "$marker_gid" != "0" ]]',
        "if IFS= read -r -n 1 extra <&7; then",
        'size="$(fd_size 7)"',
        "((size == ${#marker_gid} + 1)) ||\n"
        '      die "managed image lock group marker must contain exactly one numeric gid line"',
        '[[ "$opened" == "$identity" && "$current" == "$opened" ]] ||\n'
        '      die "managed image lock group marker changed while opening it"',
        '[[ "$owner" == "0" && "$mode" == "750" ]]',
        '[[ "$group" == "$IMAGE_LOCK_GROUP_GID" ]] ||\n'
        '      die "managed image lock directory gid does not match its marker: $group"',
        '[[ "$group" == "$IMAGE_LOCK_GROUP_GID" ]] ||\n'
        '        die "managed image lock gid does not match its marker: $group"',
        'exec 9<"$IMAGE_LOCK_FILE"',
        'build_locked "$want" forced "" 1',
        "\n    managed_image_lock_preflight\n",
        'dispatch_image_lock_selftest "$@"',
        "\n        load_image_lock_selftest\n        selftest_case_signal_child ",
        'export IMAGE_LOCK_RECEIPTS_RAW_SHA256="',
        '"$helper" == "$SCRIPT_DIR/devcontainer_image_lock_receipts.bash" ||',
        '\n      [[ "$IMAGE_LOCK_MANAGED" == "0" ]] ||\n'
        '        die "flock is required for the managed image lock"',
    )


def _image_lock_required_case_tokens() -> tuple[str, ...]:
    """Return the exact receipt-first image-lock loader tokens."""
    return (
        "load_image_lock_selftest() {",
        'local receipts="$SCRIPT_DIR/devcontainer_image_lock_receipts.bash"',
        'output="$(/bin/bash -p -- "$receipts" 2>&1)"',
        '"$output" == "error: devcontainer image lock receipt helper is source-only"',
        'source_approved_selftest_helper "$receipts" "$IMAGE_LOCK_RECEIPTS_RAW_SHA256"\n'
        '  source_approved_selftest_helper "$helper" "$IMAGE_LOCK_SELFTEST_RAW_SHA256"',
        "declare -F expected_image_lock_suite_receipts scenario_receipt_value",
        "require_controller_cleanup_receipt_file dispatch_image_lock_selftest >/dev/null",
        '\n    dispatch_image_lock_selftest suite "$tmp"\n',
    )


def _image_lock_script_errors(image_source: str, cases_source: str) -> list[str]:
    """Require discovery, no-create locking, and force serialization."""
    required_source = _image_lock_required_source_tokens()
    required_cases = _image_lock_required_case_tokens()
    errors = []
    if any(image_source.count(token) != 1 for token in required_source) or any(
        cases_source.count(token) != 1 for token in required_cases
    ):
        errors.append("dev box image lock: fail-closed helper or selftest is not load-bearing")
    expected_open_validations = 2
    open_validation = "\n      validate_opened_image_lock 9\n"
    if image_source.count(open_validation) != expected_open_validations:
        errors.append("dev box image lock: opened inode is not checked before and after flock")
    if "/var/cache/ra8-tools" in image_source or "RA8_TOOLS_CACHE_DIR" in image_source:
        errors.append("dev box image lock: unmanaged path still uses the sticky shared cache")
    if 'exec 9>>"$IMAGE_LOCK_FILE"' in image_source or 'exec 9>"$IMAGE_LOCK_FILE"' in image_source:
        errors.append("dev box image lock: managed open may recreate the lock")
    if 'while [[ ! -e "$ready" ]]' in image_source:
        errors.append("dev box image lock: force-contention readiness wait is unbounded")
    return errors


def _image_lock_selftest_errors(
    source: str,
    receipt_source: str,
    cases_source: str,
    signal_source: str,
) -> list[str]:
    """Require bounded process-group cleanup and every semantic attack."""
    required_once = policy.image_lock_selftest_required_tokens()
    receipt_required_once = policy.image_lock_receipt_required_tokens()
    cases_required_once = policy.image_lock_cases_required_tokens()
    signal_required_once = policy.image_lock_signal_required_tokens()
    required_present = (
        '"$SELFTEST_CASE_DIR/ready.status"',
        '"$SELFTEST_CASE_DIR/done.status"',
        "exec 8>&-",
    )
    errors = []
    if (
        any(source.count(token) != 1 for token in required_once)
        or any(token not in source for token in required_present)
        or any(receipt_source.count(token) != 1 for token in receipt_required_once)
        or any(cases_source.count(token) != 1 for token in cases_required_once)
        or any(signal_source.count(token) != 1 for token in signal_required_once)
    ):
        errors.append("dev box image lock: bounded selftest harness is not load-bearing")
    forbidden = (
        'kill -KILL "$SELFTEST_WORKER_PID"',
        'kill -KILL "$controller"',
        'wait "$SELFTEST_WORKER_PID" || true',
    )
    if any(
        token in source
        or token in receipt_source
        or token in cases_source
        or token in signal_source
        for token in forbidden
    ):
        errors.append("dev box image lock: selftest contains an unbounded wait")
    return errors


def _image_lock_authority_errors(inputs: dict[str, str]) -> list[str]:
    """Require one managed cross-user image lock and a private local default."""
    defaults_source = inputs["dev_defaults"]
    transaction_entry_source = inputs["dev_transaction"]
    transaction_source = inputs["dev_main"]
    image_source = inputs["devcontainer_image"]
    image_receipt_source = inputs["devcontainer_image_lock_receipts"]
    image_selftest_source = inputs["devcontainer_image_lock_selftest"]
    image_cases_source = inputs["devcontainer_image_selftest_cases"]
    image_signal_source = inputs["devcontainer_image_signal_selftest"]
    include = (
        "- name: Converge the managed devcontainer image lock authority\n"
        "  ansible.builtin.include_tasks: image_lock.yml\n"
    )
    try:
        defaults = yaml.safe_load(defaults_source)
        tasks = yaml.safe_load(transaction_source)
    except yaml.YAMLError:
        return ["dev box image lock: malformed defaults or transaction"]
    if not isinstance(defaults, dict) or not isinstance(tasks, list):
        return ["dev box image lock: defaults or transaction has the wrong shape"]
    errors = _image_lock_task_errors(tasks)
    if transaction_entry_source.count(include) != 1:
        errors.append("dev box image lock: transaction include boundary drifted")
    authority = "/var/cache/ra8-devcontainer-image-lock"
    if defaults.get("dev_box_image_lock_dir") != authority:
        errors.append("dev box image lock: managed directory authority drifted")
    if image_source.count(f'CANONICAL_IMAGE_LOCK_DIR="{authority}"') != 1:
        errors.append("dev box image lock: Ansible and script authorities diverged")
    return [
        *errors,
        *_image_lock_script_errors(image_source, image_cases_source),
        *_image_lock_selftest_errors(
            image_selftest_source,
            image_receipt_source,
            image_cases_source,
            image_signal_source,
        ),
        *image_lock_receipts.errors(
            image_source,
            image_receipt_source,
            image_selftest_source,
            image_cases_source,
            image_signal_source,
        ),
    ]


def semantic_errors() -> list[str]:
    """Execute offline selftests for each new authentication boundary."""
    failures = [
        *capability.run_selftest(),
        *broker.run_selftest(),
        *lock_verify.run_selftest(),
        *transaction.run_selftest(),
    ]
    return [f"HIL convergence v9 semantic selftest: {failure}" for failure in failures]


def _semantic_authority_lines(lines: tuple[str, ...]) -> list[str]:
    """Normalize wrapper presentation indentation for semantic comparison."""
    return [line.strip() for line in lines]


def startup_authority_selftest() -> list[str]:
    """Prove an indented canonical wrapper retains the same semantics."""
    return roles.startup_authority_selftest(PRIVILEGED_BODY_PREFIX)


def _public_script_errors(source: str, rel: str, safe_path: str) -> list[str]:
    """Require sanitation using check_shebangs' authority, never a drifting copy."""
    expected_preamble = list(PINNED_INTERPRETER_BOUNDARIES[rel])
    lines = source.splitlines()
    if lines[: len(expected_preamble)] != expected_preamble:
        return [f"{rel}: interpreter boundary is not exact"]
    executable = [
        line.strip()
        for line in lines[len(expected_preamble) :]
        if line.strip() and not line.lstrip().startswith("#")
    ]
    prefix = _semantic_authority_lines(PRIVILEGED_BODY_PREFIX)
    suffix = _semantic_authority_lines(PRIVILEGED_BODY_CLOSE)
    sanitizer = "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV"
    if rel == "scripts/dev/provision_dev_box_toolchain.sh":
        sanitizer += " TMPDIR"
    expected = [
        "set -euo pipefail",
        "export BASH_ENV=/dev/null ENV=/dev/null PYTHONNOUSERSITE=1",
        sanitizer,
        f"PATH={safe_path}",
        "export PATH",
    ]
    wrapper_ok = executable[: len(prefix)] == prefix and executable[-len(suffix) :] == suffix
    body = executable[len(prefix) : -len(suffix)] if wrapper_ok else []
    errors = [] if body[:5] == expected else [f"{rel}: startup sanitizer moved"]
    if any(re.search(r"(?<![/\.\w-])bash\b", line) for line in executable):
        errors.append(f"{rel}: child Bash resolves through caller PATH")
    return errors


def _installer_errors(inputs: dict[str, str]) -> list[str]:
    """Check direct public setup and tool-provision entrypoint boundaries."""
    return (
        _public_script_errors(
            inputs["setup_ansible"], "scripts/dev/setup_ansible.sh", "/usr/bin:/bin"
        )
        + _public_script_errors(
            inputs["provision_toolchain"],
            "scripts/dev/provision_dev_box_toolchain.sh",
            "/usr/local/bin:/usr/bin:/bin",
        )
        + _public_script_errors(inputs["infra_bootstrap"], "infra/bootstrap.sh", "/usr/bin:/bin")
    )


def _infra_boundary_errors(infra_just: str, infra_sh: str) -> list[str]:
    """Bind the dependency-free public boundary probe to sanitized infra."""
    recipe = "_boundary_selftest:\n    {{ infra }} --selftest-boundary"
    endpoint = """if [[ "${1:-}" == --selftest-boundary ]]; then
  if (($# != 1)); then
    echo "error: infrastructure boundary selftest takes no arguments" >&2
    exit 1
  fi
  exit 0
fi"""
    sanitizer = (
        '[[ -z "${BASH_ENV:-}" && -z "${ENV:-}" && -z "${PYTHONPATH:-}"'
        ' && -z "${PYTHONHOME:-}" ]] || {\n'
        '  echo "error: infrastructure startup environment was not sanitized" >&2\n'
        "  exit 1\n}"
    )
    errors = []
    if infra_just.count(recipe) != 1:
        errors.append("just/infra.just: public boundary selftest recipe is not exact")
    if infra_sh.count(endpoint) != 1:
        errors.append("scripts/dev/infra.sh: boundary selftest endpoint is not exact")
    if infra_sh.count(sanitizer) != 1:
        errors.append("scripts/dev/infra.sh: startup sanitizer is not exact")
    elif infra_sh.find(endpoint) < infra_sh.find(sanitizer) + len(sanitizer):
        errors.append("scripts/dev/infra.sh: boundary selftest precedes startup sanitation")
    return errors


def _hil_shell_errors(raw_sources: str) -> list[str]:
    """Require fixed privileged Bash at every HIL-owned shell boundary."""
    try:
        sources = json.loads(raw_sources)
    except json.JSONDecodeError:
        return ["HIL shell boundary inventory is malformed"]
    if not isinstance(sources, dict) or not sources:
        return ["HIL shell boundary inventory is empty"]
    errors: list[str] = []
    for path, source in sources.items():
        if not isinstance(path, str) or not isinstance(source, str):
            errors.append("HIL shell boundary inventory entry is malformed")
            continue
        shebangs = [line.strip() for line in source.splitlines() if line.lstrip().startswith("#!")]
        if not shebangs or any(line != "#!/bin/bash -p" for line in shebangs):
            errors.append(f"{path}: shell entry is not fixed privileged Bash")
        executable = [line for line in source.splitlines() if not line.lstrip().startswith("#")]
        if any(re.search(r"(?<!/)\bbash\b", line) for line in executable):
            errors.append(f"{path}: active Bash invocation resolves through caller PATH")
        if (
            path == "scripts/ci/monitor.sh"
            and source.count("ExecStart=/bin/bash -p $self daemon") != 1
        ):
            errors.append("scripts/ci/monitor.sh: generated service Bash argv is not exact")
    return errors


def _write_executable(path: Path, marker: Path) -> None:
    """Write one hostile startup executable used only in the temp fixture."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!/bin/sh\nprintf x >>'{marker}'\nexit 91\n", encoding="utf-8")
    path.chmod(0o755)


def _hostile_environment(fixture: Path, marker: Path) -> dict[str, str]:
    """Create one hostile caller environment without executing its payloads."""
    startup = fixture / "startup.sh"
    startup.write_text(f"printf x >>'{marker}'\n", encoding="utf-8")
    site = fixture / "site"
    site.mkdir()
    (site / "sitecustomize.py").write_text(
        f"from pathlib import Path\nPath({str(marker)!r}).write_text('x')\n",
        encoding="utf-8",
    )
    fake_bin = fixture / "bin"
    for name in ("bash", "dirname", "python3"):
        _write_executable(fake_bin / name, marker)
    fake_venv = fixture / "venv"
    for name in ("python3", "ansible-playbook"):
        _write_executable(fake_venv / f"bin/{name}", marker)
    account = pwd.getpwuid(os.getuid())
    return {
        "BASH_ENV": str(startup),
        "ENV": str(startup),
        "HOME": account.pw_dir,
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "LOGNAME": account.pw_name,
        "PATH": f"{fake_bin}:/usr/bin:/bin",
        "PYTHONHOME": str(fixture / "python-home"),
        "PYTHONPATH": str(site),
        "RA8_TOOL_VENV": str(fake_venv),
        "USER": account.pw_name,
    }


def _run_boundary(argv: list[str], root: Path, environment: dict[str, str]) -> int:
    """Run one offline public boundary with fixed process controls."""
    result = subprocess.run(  # noqa: S603 -- reviewed fixed test argv
        argv,
        cwd=root,
        env=environment,
        capture_output=True,
        timeout=60,
        check=False,
    )
    return result.returncode


def _just_boundary_errors(
    just: str, root: Path, fixture: Path, marker: Path, environment: dict[str, str]
) -> list[str]:
    """Exercise the infra and HIL Just public entrypoints offline."""
    prefix = [str(Path(just).resolve()), "--justfile", str(root / "justfile")]
    hostile_tool_env = dict(environment)
    clean_tool_env = dict(environment)
    del clean_tool_env["RA8_TOOL_VENV"]
    hostile_rc = _run_boundary([*prefix, "infra::list"], root, hostile_tool_env)
    errors = []
    if marker.exists() or hostile_rc == 0:
        errors.append("caller-selected tool environment did not fail closed")
    marker.unlink(missing_ok=True)
    infra_rc = _run_boundary([*prefix, "infra::_boundary_selftest"], root, clean_tool_env)
    if marker.exists() or infra_rc != 0:
        errors.append("hostile startup executed at the public infra entry")
    marker.unlink(missing_ok=True)
    hil_rc = _run_boundary(
        [*prefix, "hil::preflash_check", str(fixture / "absent.elf")],
        root,
        clean_tool_env,
    )
    if marker.exists():
        errors.append("hostile startup executed at the public HIL entry")
    if hil_rc == 0:
        errors.append("offline invalid-image HIL entry unexpectedly succeeded")
    return errors


def _installer_boundary_selftest(
    root: Path, marker: Path, environment: dict[str, str]
) -> list[str]:
    """Exercise the three direct setup/provision boundaries offline."""
    errors = []
    for relative in (
        "scripts/dev/setup_ansible.sh",
        "scripts/dev/provision_dev_box_toolchain.sh",
        "infra/bootstrap.sh",
    ):
        marker.unlink(missing_ok=True)
        argv = ["/bin/bash", "-p", str(root / relative), "--selftest-boundary"]
        if _run_boundary(argv, root, environment) != 0 or marker.exists():
            errors.append(f"{relative}: hostile startup boundary failed")
    return errors


def public_boundary_selftest(root: Path) -> list[str]:
    """Run real public entries with every supported startup injection hostile."""
    just = shutil.which("just")
    if just is None:
        return ["public infra boundary selftest cannot find Just"]
    with tempfile.TemporaryDirectory(prefix="ra8-infra-boundary-") as raw:
        fixture = Path(raw)
        marker = fixture / "executed"
        environment = _hostile_environment(fixture, marker)
        return _just_boundary_errors(
            just, root, fixture, marker, environment
        ) + _installer_boundary_selftest(root, marker, environment)


def errors(inputs: dict[str, str]) -> list[str]:
    """Return structural v9 findings for the supplied source bytes."""
    return (
        _lock_verifier_errors(inputs["bench_lock_verify"])
        + _context_scope_errors(
            inputs["dev_defaults"], inputs["dev_main"], inputs["devcontainer_image"]
        )
        + _image_lock_authority_errors(inputs)
        + roles.pin_and_shell_authority_errors(
            inputs["dev_main"], inputs["dockerfile"], inputs["root_justfile"]
        )
        + roles.dev_box_shell_boundary_errors(inputs["dev_main"], inputs["hil_just"])
        + _installer_errors(inputs)
        + _infra_boundary_errors(inputs["infra_just"], inputs["infra_sh"])
        + _hil_shell_errors(inputs["hil_shells"])
    )

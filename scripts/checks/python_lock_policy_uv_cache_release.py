# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Release/runtime policy checks for the managed uv-cache provisioner."""

from __future__ import annotations

import hashlib
import re

from python_lock_policy_uv_cache_release_contracts import (
    provisioner_dispatch,
    provisioner_expected_bodies,
    provisioner_mutations,
    provisioner_release_tmp_contract,
)

EXPECTED_PROVISIONER_UV_RUNS = 3
EXPECTED_SELFTEST_DISPATCH_CALLS = 3
RELEASE_SELFTEST_PATH = "scripts/dev/provision_dev_box_toolchain_selftest.bash"


def _shell_function(source: str, name: str, indent: str = "  ") -> list[str] | None:
    """Extract one two-space-indented top-level Bash function semantically."""
    source_lines = source.splitlines()
    starts = [index for index, line in enumerate(source_lines) if line == f"{indent}{name}() {{"]
    if len(starts) != 1:
        return None
    end = next(
        (
            index
            for index in range(starts[0] + 1, len(source_lines))
            if source_lines[index] == f"{indent}}}"
        ),
        None,
    )
    if end is None:
        return None
    return [
        " ".join(line.strip().split())
        for line in source_lines[starts[0] + 1 : end]
        if line.strip() and not line.lstrip().startswith("#")
    ]


def _shell_subshell_function(source: str, name: str, indent: str = "  ") -> list[str] | None:
    """Extract one two-space-indented top-level Bash subshell function."""
    source_lines = source.splitlines()
    starts = [index for index, line in enumerate(source_lines) if line == f"{indent}{name}() ("]
    if len(starts) != 1:
        return None
    end = next(
        (
            index
            for index in range(starts[0] + 1, len(source_lines))
            if source_lines[index] == f"{indent})"
        ),
        None,
    )
    if end is None:
        return None
    return [
        " ".join(line.strip().split())
        for line in source_lines[starts[0] + 1 : end]
        if line.strip() and not line.lstrip().startswith("#")
    ]


def _expected_shell_body(source: str, name: str) -> list[str]:
    """Return one canonical shell function body."""
    body = _shell_function(source, name)
    if body is None:
        message = f"canonical shell fixture is missing {name}"
        raise ValueError(message)
    return body


def _provisioner_body(source: str, selftest_source: str, name: str) -> list[str] | None:
    """Return one function from exactly one of the split provisioner surfaces."""
    bodies = (
        _shell_function(source, name),
        _shell_function(selftest_source, name, ""),
    )
    present = [body for body in bodies if body is not None]
    return present[0] if len(present) == 1 else None


def _contains_once(lines: list[str], sequence: list[str]) -> bool:
    """Return whether one exact contiguous semantic shell sequence occurs."""
    hits = sum(
        lines[index : index + len(sequence)] == sequence
        for index in range(len(lines) - len(sequence) + 1)
    )
    return hits == 1


def _provisioner_release_tmp_findings(source: str, selftest_source: str) -> list[str]:
    """Bind privileged release installers to one owned temporary root."""
    findings: list[str] = []
    contract = provisioner_release_tmp_contract()
    helper_names = (
        "release_tmp_reset",
        "release_tmp_identity",
        "release_tmp_root_is_safe",
        "release_tmp_is_safe",
        "release_tmp_pending_cleanup",
        "release_tmp_cleanup_owned",
        "release_tmp_exit",
        "release_tmp_signal",
        "install_release_tmp_traps",
        "release_tmp_allocation_checkpoint",
        "release_tmp_begin",
        "release_tmp_path_is_absent",
        "release_tmp_signal_case",
        "release_tmp_signal_child",
        "release_tmp_replacement_refusal",
        "release_tmp_wrong_owner_refusal",
        "release_tmp_contract_selftest",
    )
    findings.extend(
        f"dev-box provisioner {name} semantic contract drifted"
        for name in helper_names
        if _provisioner_body(source, selftest_source, name) != _expected_shell_body(contract, name)
    )
    if source.count("  unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV TMPDIR\n") != 1:
        findings.append("dev-box privileged boundary does not sanitize TMPDIR exactly once")
    if any(token in source or token in selftest_source for token in ("mktemp -d", "trap 'rm -rf")):
        findings.append("dev-box provisioner retains an unowned temporary-directory authority")
    for name in (
        "install_shellcheck",
        "install_shfmt",
        "install_actionlint",
        "install_hadolint",
        "install_just",
        "install_doxygen",
    ):
        body = _shell_subshell_function(source, name)
        if (
            body is None
            or body.count("release_tmp_begin") != 1
            or body.count('tmp="$RELEASE_TMP_DIR"') != 1
        ):
            findings.append(f"dev-box {name} does not use the shared release temporary root")
    return findings


def _provisioner_install_findings(source: str) -> list[str]:
    """Bind the provisioner's mutating install path."""
    install = _shell_function(source, "install_python_tools")
    install_prefix = [
        'local venv="$1" cache_modes_current=1',
        "if ! uv_cache_modes_current; then",
        "cache_modes_current=0",
        "fi",
        "uv_bootstrap_apply --ensure >/dev/null",
        'uv_cache_apply_report "${cache_modes_current}"',
    ]
    if install is None or install[: len(install_prefix)] != install_prefix:
        return ["dev-box apply path is not bound to mode-check, ensure, and exact report"]
    run_calls = sum(line.startswith("uv_bootstrap_apply_run ") for line in install)
    if run_calls != EXPECTED_PROVISIONER_UV_RUNS or any("uv_bin" in line for line in install):
        return ["dev-box apply path does not execute all uv work through bootstrap snapshots"]
    return []


def _provisioner_dispatch_findings(source: str) -> list[str]:
    """Bind the provisioner's read-only branch and selftest dispatch."""
    findings: list[str] = []
    main = _shell_function(source, "main")
    check_sequence = [
        "else",
        "if uv_cache_check; then",
        ":",
        "else",
        "uv_check_status=$?",
        'return "${uv_check_status}"',
        "fi",
        "fi",
    ]
    if main is None or not _contains_once(main, check_sequence):
        findings.append("dev-box --check-only path is not directly bound to uv_cache_check")
    dispatch = list(provisioner_dispatch())
    semantic = [
        " ".join(line.strip().split())
        for line in source.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not _contains_once(semantic, dispatch):
        findings.append("dev-box provisioner contract selftest is not the production dispatch")
    return findings


def _provisioner_selftest_findings(selftest_source: str) -> list[str]:
    """Bind the contract test to every status and apply-marker direction."""
    body = _shell_function(selftest_source, "uv_cache_contract_selftest", "")
    joined = " ".join(body or [])
    required = (
        'uv_cache_check_scenario_selftest 0 "" current-path',
        '2 " ... uv-cache restricted -> shared (planned)" drift-path',
        '1 "error: authenticated uv cache check failed" invalid-path',
        '"$(uv_cache_apply_report 0)" != " ... uv-cache restricted -> shared"',
        "release_selftest_loader_refusals || return 1",
        "release_tmp_contract_selftest",
        "python_no_bytecode_residue_selftest",
        "current) return 0 ;;",
        "drift) return 2 ;;",
        "invalid) return 1 ;;",
    )
    if body is None or any(token not in joined for token in required):
        return ["dev-box provisioner contract selftest call chain drifted"]
    return []


def _release_loader_runtime_findings(selftest_source: str) -> list[str]:
    """Bind hostile loader tests to every reviewed refusal direction."""
    case_body = " ".join(
        _shell_subshell_function(selftest_source, "release_selftest_loader_case", "") or []
    )
    matrix_body = " ".join(
        _shell_function(selftest_source, "release_selftest_loader_refusals", "") or []
    ).replace("\\ ", "")
    case_tokens = (
        'if [[ "$kind" == "parent-symlink" ]]; then',
        'ln -s "${fixture_dir##*/}" "$directory" || return 1',
        "printf 'return 42\\n' >\"$helper\"",
        '[[ "$status" == "42" && -z "${RA8_RELEASE_LOADER_MARKER:-}" ]]',
        '[[ "$status" != "0" && -z "${RA8_RELEASE_LOADER_MARKER:-}" ]]',
        'source_release_selftest_helper_from "$main" "$helper" "$directory" "$digest"',
    )
    matrix_tokens = (
        "safe parent-symlink main-symlink main-hardlink main-mode",
        "helper-symlink helper-hardlink helper-mode digest inplace path-replace return-42",
        "main-owner main-group helper-owner helper-group",
        'release_selftest_loader_case "$root" "$kind" || return 1',
    )
    if any(token not in case_body for token in case_tokens) or any(
        token not in matrix_body for token in matrix_tokens
    ):
        return ["dev-box release selftest hostile runtime matrix drifted"]
    return []


def _release_selftest_loader_findings(source: str, selftest_source: str) -> list[str]:
    """Bind the split release selftest to one privileged two-FD loader."""
    findings: list[str] = []
    digest = hashlib.sha256(selftest_source.encode("utf-8")).hexdigest()
    pin = re.findall(r'^  RELEASE_SELFTEST_RAW_SHA256="([0-9a-f]{64})"$', source, re.MULTILINE)
    if pin != [digest]:
        findings.append("dev-box release selftest raw digest is stale or ambiguous")
    required = (
        "local identity digest source_path source_status=0",
        'local helper="$ROOT/scripts/dev/provision_dev_box_toolchain_selftest.bash"',
        'local main="$ROOT/scripts/dev/provision_dev_box_toolchain.sh"',
        'local expected_dir="$ROOT/scripts/dev" expected_digest="$RELEASE_SELFTEST_RAW_SHA256"',
        '[[ "$resolved" == "$expected_dir" && -f "$main" && ! -L "$main" &&',
        '"$(stat -c \'%h\' "$main")" == "1" && "$(stat -c \'%a\' "$main")" == "755"',
        '("$main_owner" == "0" || "$main_owner" == "$caller_uid")',
        '("$caller_uid" != "0" || ("$main_owner" == "0" && "$main_group" == "0"))',
        '-f "$helper" && ! -L "$helper" && "$(stat -c \'%h\' "$helper")" == "1"',
        '"$(stat -c \'%a\' "$helper")" == "644"',
        '"$(stat -c \'%u\' "$helper")" == "$main_owner"',
        '"$(stat -c \'%g\' "$helper")" == "$main_group"',
        'exec 7<"$helper"',
        'exec 8<"$helper"',
        '"$(stat -Lc \'%d:%i\' /proc/self/fd/7)" == "$identity"',
        '"$(stat -Lc \'%d:%i\' /proc/self/fd/8)" == "$identity"',
        'digest="$(sha256sum <&7)"',
        '[[ "$digest" == "$expected_digest" ]]',
        "release_selftest_open_checkpoint || {",
        'source_path="/proc/self/fd/8"',
        'source "$source_path" || source_status=$?',
        "exec 8<&- || return 1",
        '[[ "$source_status" == "0" ]] || return "$source_status"',
        "declare -F release_tmp_contract_selftest release_tmp_signal_child >/dev/null",
        'source_release_selftest_helper_from "$main" "$helper" '
        '"$expected_dir" "$expected_digest" || return 1',
    )
    if any(source.count(token) != 1 for token in required):
        findings.append("dev-box release selftest loader authority drifted")
    if source.count("load_release_selftest_helper || exit 1") != EXPECTED_SELFTEST_DISPATCH_CALLS:
        findings.append("dev-box release selftest dispatcher is not load-bearing")
    helper_required = (
        '[[ "${BASH_SOURCE[0]}" != "$0" ]]',
        '[[ "$-" == *p* ]]',
        '"${BASH_SOURCE[1]##*/}" == "provision_dev_box_toolchain.sh"',
    )
    if any(selftest_source.count(token) != 1 for token in helper_required):
        findings.append("dev-box release selftest source boundary drifted")
    return findings


def provisioner_findings(source: str, selftest_source: str) -> list[str]:
    """Bind read-only check and mutating apply to disjoint shell helpers."""
    findings: list[str] = []
    for name, expected in provisioner_expected_bodies().items():
        body = _provisioner_body(source, selftest_source, name)
        if body != _expected_shell_body(expected, name):
            findings.append(f"dev-box provisioner {name} semantic contract drifted")
    findings.extend(_provisioner_release_tmp_findings(source, selftest_source))
    findings.extend(_provisioner_install_findings(source))
    findings.extend(_provisioner_dispatch_findings(source))
    findings.extend(_provisioner_selftest_findings(selftest_source))
    findings.extend(_release_selftest_loader_findings(source, selftest_source))
    findings.extend(_release_loader_runtime_findings(selftest_source))
    return findings


def _mutate_once(source: str, old: str, new: str) -> str:
    """Apply one exact mutation and fail if the fixture authority drifted."""
    if source.count(old) != 1:
        message = f"selftest mutation anchor count changed: {old!r}"
        raise ValueError(message)
    return source.replace(old, new, 1)


def _hollow_shell_function(source: str, name: str) -> str:
    """Replace one two-space-indented shell function body with a no-op."""
    lines = source.splitlines()
    starts = [
        index for index, line in enumerate(lines) if line in (f"  {name}() {{", f"{name}() {{")
    ]
    if len(starts) != 1:
        message = f"selftest shell function is missing: {name}"
        raise ValueError(message)
    indent = "  " if lines[starts[0]].startswith("  ") else ""
    end = next(index for index in range(starts[0] + 1, len(lines)) if lines[index] == f"{indent}}}")
    replacement = [lines[starts[0]], "    :", lines[end]]
    return "\n".join([*lines[: starts[0]], *replacement, *lines[end + 1 :]]) + "\n"


def _mutate_provisioner_pair(
    provisioner: str, selftest_source: str, old: str, new: str
) -> tuple[str, str]:
    """Mutate the unique owner of one provisioner contract token."""
    helper_old = "\n".join(line.removeprefix("  ") for line in old.split("\n"))
    helper_new = "\n".join(line.removeprefix("  ") for line in new.split("\n"))
    hits = provisioner.count(old) + selftest_source.count(helper_old)
    if hits != 1:
        message = f"selftest mutation anchor count changed across provisioner inputs: {old!r}"
        raise ValueError(message)
    if old in provisioner:
        return _mutate_once(provisioner, old, new), selftest_source
    return provisioner, _mutate_once(selftest_source, helper_old, helper_new)


def _release_loader_path_mutations() -> tuple[tuple[str, str, list[str]], ...]:
    """Return loader path, metadata, and wrapper-call mutations."""
    loader_diagnostic = ["dev-box release selftest loader authority drifted"]
    return (
        (
            "local identity digest source_path source_status=0",
            "local identity digest source_path source_status",
            loader_diagnostic,
        ),
        (
            'local helper="$ROOT/scripts/dev/provision_dev_box_toolchain_selftest.bash"',
            'local helper="$ROOT/scripts/dev/provision_dev_box_toolchain.sh"',
            loader_diagnostic,
        ),
        (
            'local main="$ROOT/scripts/dev/provision_dev_box_toolchain.sh"',
            'local main="$ROOT/scripts/dev/provision_dev_box_toolchain_selftest.bash"',
            loader_diagnostic,
        ),
        (
            'local expected_dir="$ROOT/scripts/dev" expected_digest="$RELEASE_SELFTEST_RAW_SHA256"',
            'local expected_dir="$(pwd)" expected_digest="$RELEASE_SELFTEST_RAW_SHA256"',
            loader_diagnostic,
        ),
        (
            'local expected_dir="$ROOT/scripts/dev" expected_digest="$RELEASE_SELFTEST_RAW_SHA256"',
            'local expected_dir="$ROOT/scripts/dev" expected_digest="$(sha256sum "$helper")"',
            loader_diagnostic,
        ),
        (
            'source_release_selftest_helper_from "$main" "$helper" '
            '"$expected_dir" "$expected_digest" || return 1',
            'source_release_selftest_helper_from "$helper" "$main" '
            '"$expected_dir" "$expected_digest" || return 1',
            loader_diagnostic,
        ),
        (
            '[[ "$resolved" == "$expected_dir" && -f "$main" && ! -L "$main" &&',
            "[[ true &&",
            loader_diagnostic,
        ),
        ('"$(stat -c \'%h\' "$main")" == "1"', '"1" == "1"', loader_diagnostic),
        ('"$(stat -c \'%a\' "$main")" == "755"', '"755" == "755"', loader_diagnostic),
        ('("$main_owner" == "0" || "$main_owner" == "$caller_uid")', "true", loader_diagnostic),
        (
            '("$caller_uid" != "0" || ("$main_owner" == "0" && "$main_group" == "0"))',
            "true",
            loader_diagnostic,
        ),
        ('-f "$helper" && ! -L "$helper"', '-e "$helper"', loader_diagnostic),
        ('"$(stat -c \'%h\' "$helper")" == "1"', '"1" == "1"', loader_diagnostic),
        ('"$(stat -c \'%a\' "$helper")" == "644"', '"644" == "644"', loader_diagnostic),
        ('"$(stat -c \'%u\' "$helper")" == "$main_owner"', "true", loader_diagnostic),
        ('"$(stat -c \'%g\' "$helper")" == "$main_group"', "true", loader_diagnostic),
    )


def _release_loader_fd_mutations() -> tuple[tuple[str, str, list[str]], ...]:
    """Return loader descriptor, digest, source, and dispatch mutations."""
    loader_diagnostic = ["dev-box release selftest loader authority drifted"]
    return (
        ('exec 7<"$helper"', 'exec 7<"$main"', loader_diagnostic),
        ('exec 8<"$helper"', 'exec 8<"$main"', loader_diagnostic),
        ('"$(stat -Lc \'%d:%i\' /proc/self/fd/7)" == "$identity"', "true", loader_diagnostic),
        ('"$(stat -Lc \'%d:%i\' /proc/self/fd/8)" == "$identity"', "true", loader_diagnostic),
        ('digest="$(sha256sum <&7)"', 'digest="$expected_digest"', loader_diagnostic),
        ('[[ "$digest" == "$expected_digest" ]]', '[[ -n "$digest" ]]', loader_diagnostic),
        ("release_selftest_open_checkpoint || {", "true || {", loader_diagnostic),
        ('source_path="/proc/self/fd/8"', 'source_path="$helper"', loader_diagnostic),
        ('source "$source_path" || source_status=$?', ":", loader_diagnostic),
        ('[[ "$source_status" == "0" ]] || return "$source_status"', ":", loader_diagnostic),
        (
            '  elif [ "${1:-}" = "--selftest-uv-cache-contract" ]; then\n'
            "    load_release_selftest_helper || exit 1",
            '  elif [ "${1:-}" = "--selftest-uv-cache-contract" ]; then\n    true',
            [
                "dev-box provisioner contract selftest is not the production dispatch",
                "dev-box release selftest dispatcher is not load-bearing",
            ],
        ),
    )


def _rebind_release_helper(provisioner: str, helper_source: str) -> str:
    """Rebind the exact helper raw digest after a semantic helper mutation."""
    helper_digest = hashlib.sha256(helper_source.encode("utf-8")).hexdigest()
    return re.sub(
        r'(?m)^(  RELEASE_SELFTEST_RAW_SHA256=")[0-9a-f]{64}("$)',
        rf"\g<1>{helper_digest}\g<2>",
        provisioner,
        count=1,
    )


def _release_loader_helper_failures(provisioner: str, selftest_source: str) -> list[str]:
    """Prove source-parent and hostile runtime directions after digest rebinding."""
    failures = []
    parent_mutation = _mutate_once(
        selftest_source,
        '"${BASH_SOURCE[1]##*/}" == "provision_dev_box_toolchain.sh"',
        "true",
    )
    rebound = _rebind_release_helper(provisioner, parent_mutation)
    if provisioner_findings(rebound, parent_mutation) != [
        "dev-box release selftest source boundary drifted"
    ]:
        failures.append("uv release selftest parent mutation passed")
    helper_mutations = (
        (
            "release_selftest_loader_refusals || return 1",
            ": # loader refusal matrix removed",
            "dev-box provisioner contract selftest call chain drifted",
        ),
        (
            "safe parent-symlink main-symlink main-hardlink main-mode",
            "safe main-symlink main-hardlink main-mode",
            "dev-box release selftest hostile runtime matrix drifted",
        ),
        (
            "helper-symlink helper-hardlink helper-mode digest inplace path-replace return-42",
            "helper-symlink helper-hardlink helper-mode digest inplace path-replace",
            "dev-box release selftest hostile runtime matrix drifted",
        ),
        (
            '[[ "$status" == "42" && -z "${RA8_RELEASE_LOADER_MARKER:-}" ]]',
            '[[ "$status" != "0" ]]',
            "dev-box release selftest hostile runtime matrix drifted",
        ),
        (
            'source_release_selftest_helper_from "$main" "$helper" "$directory" "$digest"',
            'source_release_selftest_helper_from "$helper" "$main" "$directory" "$digest"',
            "dev-box release selftest hostile runtime matrix drifted",
        ),
    )
    for old, new, diagnostic in helper_mutations:
        changed = _mutate_once(selftest_source, old, new)
        rebound = _rebind_release_helper(provisioner, changed)
        if provisioner_findings(rebound, changed) != [diagnostic]:
            failures.append(f"uv release selftest runtime mutation passed: {old}")
    return failures


def _release_loader_mutation_failures(provisioner: str, selftest_source: str) -> list[str]:
    """Prove every split-helper loader and call-site safeguard must fire."""
    failures = []
    mutations = (*_release_loader_path_mutations(), *_release_loader_fd_mutations())
    for old, new, expected in mutations:
        changed = _mutate_once(provisioner, old, new)
        if provisioner_findings(changed, selftest_source) != expected:
            failures.append(f"uv release selftest loader mutation passed: {old}")
    failures.extend(_release_loader_helper_failures(provisioner, selftest_source))
    return failures


def provisioner_mutation_failures(provisioner: str, selftest_source: str) -> list[str]:
    """Prove check/apply shell commands and both marker directions are bound."""
    failures = []
    if provisioner_findings(provisioner, selftest_source):
        failures.append("live uv provisioner semantic contract failed")
    failures.extend(
        f"uv provisioner mutation passed: {old}"
        for old, new in provisioner_mutations()
        if not provisioner_findings(
            *_mutate_provisioner_pair(provisioner, selftest_source, old, new)
        )
    )
    failures.extend(
        f"hollow uv provisioner selftest passed policy: {name}"
        for name in (
            "uv_bootstrap_apply_run",
            "release_tmp_reset",
            "release_tmp_identity",
            "release_tmp_root_is_safe",
            "release_tmp_is_safe",
            "release_tmp_pending_cleanup",
            "release_tmp_cleanup_owned",
            "release_tmp_exit",
            "release_tmp_signal",
            "install_release_tmp_traps",
            "release_tmp_allocation_checkpoint",
            "release_tmp_begin",
            "release_tmp_path_is_absent",
            "release_tmp_signal_case",
            "release_tmp_signal_child",
            "release_tmp_replacement_refusal",
            "release_tmp_wrong_owner_refusal",
            "release_tmp_contract_selftest",
            "uv_cache_check_scenario_selftest",
            "python_selftest_tmp_identity",
            "python_selftest_suite_is_safe",
            "python_selftest_tmp_is_safe",
            "python_selftest_tmp_cleanup",
            "python_selftest_suite_cleanup",
            "python_selftest_pending_cleanup",
            "python_selftest_suite_candidate",
            "python_selftest_allocation_signal",
            "python_selftest_allocation_signal_child",
            "python_selftest_allocation_signal_case",
            "python_selftest_replacement_refusal",
            "python_bytecode_invocation_selftest",
            "python_no_bytecode_residue_selftest",
            "uv_cache_contract_selftest",
        )
        if not provisioner_findings(
            *(
                (_hollow_shell_function(provisioner, name), selftest_source)
                if f"  {name}() {{" in provisioner
                else (provisioner, _hollow_shell_function(selftest_source, name))
            )
        )
    )
    failures.extend(_release_loader_mutation_failures(provisioner, selftest_source))
    return failures

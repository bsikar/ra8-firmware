#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Bind HIL rig consumers to one typed value and parser authority."""

from __future__ import annotations

import os
import re
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
RIG_ENV = "scripts/hil/lib/rig_env.sh"
CONTRACT = "scripts/hil/lib/rig_contract.sh"
PARSER = "scripts/hil/rig_env_parse.py"
ANSIBLE = "infra/ansible/roles/dev_box/tasks/hil_runner_transaction.yml"
BENCH_ANSIBLE = "infra/ansible/roles/hil_bench/tasks/transaction.yml"
REMOTE_GDB_ARGS = "scripts/dev/remote_gdb_args.py"
GATE = "scripts/ci/gates/checks.sh"
EXAMPLE = ".env.example"
ETH_SCRIPT = "scripts/hil/eth_tcp.sh"
HIL_DOC = "docs/HIL_DEVELOPER_WORKFLOW.md"
HIL_SUITE = "docs/HIL_SUITE.md"
FIELDS = ("PI_HOST", "JLINK_SN", "JLINK_DEVICE", "PI_REPO")
EXPANSION = re.compile(
    r"\$(?:PI_HOST|JLINK_SN|JLINK_DEVICE|PI_REPO)\b"
    r"|\$\{(?:PI_HOST|JLINK_SN|JLINK_DEVICE|PI_REPO)\}"
)
PI_HOST_EXPANSION = re.compile(r"\$PI_HOST\b|\$\{PI_HOST\}")
SOURCE_RIG = re.compile(r"^\s*(?:source|[.])\s+[^#\n]*rig_env[.]sh", re.MULTILINE)


@dataclass(frozen=True)
class CommandResult:
    """Captured status and streams from one fixed-argv test child."""

    returncode: int
    stdout: str
    stderr: str


def _read(root: Path, relative: str) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return f"<UNREADABLE:{error}>"


def _active_lines(text: str) -> list[str]:
    return [
        line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")
    ]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _named_ansible_tasks(value: object, name: str) -> list[dict[str, object]]:
    """Find every exact task through Ansible block/rescue/always nesting."""
    if not isinstance(value, list):
        return []
    matches: list[dict[str, object]] = []
    for item in value:
        if not isinstance(item, dict):
            continue
        if item.get("name") == name:
            matches.append(item)
        for section in ("block", "rescue", "always"):
            matches.extend(_named_ansible_tasks(item.get(section), name))
    return matches


def _audit_ansible_ephemeral_reporting(text: str) -> list[str]:
    """Bind secure controller scratch tasks to truthful no-drift reporting."""
    try:
        document = yaml.safe_load(text)
    except yaml.YAMLError as error:
        return [f"{ANSIBLE}: cannot parse task structure: {error}"]
    findings: list[str] = []
    names = (
        "Allocate a protected allowlist result file on the control node",
        "Remove the protected allowlist result file",
    )
    for name in names:
        tasks = _named_ansible_tasks(document, name)
        if len(tasks) != 1:
            findings.append(f"{ANSIBLE}: expected exactly one task {name!r}; found {len(tasks)}")
            continue
        task = tasks[0]
        if task.get("changed_when") is not False:
            findings.append(f"{ANSIBLE}: {name!r} reports same-run scratch as managed drift")
        if task.get("check_mode") is not False:
            findings.append(f"{ANSIBLE}: {name!r} no longer executes its secure check-mode path")
    return findings


def _run_fixed(arguments: tuple[str, ...], environment: dict[str, str]) -> CommandResult:
    """Run fixed argv without a shell and capture both output streams."""
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        actions = (
            (os.POSIX_SPAWN_DUP2, stdout_file.fileno(), 1),
            (os.POSIX_SPAWN_DUP2, stderr_file.fileno(), 2),
        )
        process = os.posix_spawn(arguments[0], arguments, environment, file_actions=actions)
        _, status = os.waitpid(process, 0)
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
    return CommandResult(os.waitstatus_to_exitcode(status), stdout, stderr)


def _inside_double_quotes(line: str, offset: int) -> bool:
    escaped = False
    quoted = False
    for character in line[:offset]:
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == '"':
            quoted = not quoted
    return quoted


def _quoted_expansion(line: str, match: re.Match[str]) -> bool:
    """Accept a quote segment or an exact double-quoted argv expansion."""
    adjacent_quotes = (
        match.start() > 0
        and match.end() < len(line)
        and line[match.start() - 1] == '"'
        and line[match.end()] == '"'
    )
    return adjacent_quotes or _inside_double_quotes(line, match.start())


def audit_rig_env(text: str) -> list[str]:
    """Report loader drift from the shared typed authority."""
    findings: list[str] = []
    required = (
        'source "$_rig_lib_dir/rig_contract.sh"',
        '"$_rig_python" -I "$_rig_parser"',
        '--output "$_rig_result" --format nul --include-interactive',
        "while IFS= read -r -d '' _rig_name",
        "protected rig parser returned an unknown field",
        "ra8_rig_contract_default JLINK_DEVICE",
        "ra8_rig_contract_default PI_REPO",
        "ra8_rig_validate_loaded true",
        'ra8_rig_require "$@"',
        "protected NUL-delimited pairs",
    )
    findings.extend(
        f"{RIG_ENV}: missing shared-contract binding {token!r}"
        for token in required
        if token not in text
    )
    if re.search(r"PI_HOST.*=~|JLINK_(?:SN|DEVICE).*=~|PI_REPO.*=~", text):
        findings.append(f"{RIG_ENV}: duplicated an inline rig grammar")
    if re.search(r"(?:source|[.])\s+[\"']?\$\{?_rig_env_file", text):
        findings.append(f"{RIG_ENV}: executes the selected rig environment")
    if re.search(r"(^|[;(])\s*eval\b", text, re.MULTILINE):
        findings.append(f"{RIG_ENV}: eval is forbidden for parsed rig data")
    return findings


def audit_contract(text: str) -> list[str]:
    """Report missing value, startup, or source-mode contract defenses."""
    findings: list[str] = []
    required = (
        'if [[ "$-" == *p* ]]; then',
        "unset -v BASH_ENV ENV",
        "BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=",
        "if ((${#ra8_startup_env_unset[@]})); then",
        'if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then',
        "sourced rig contract refuses inherited Bash functions",
        'exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV',
        "--descendant-selftest",
        "PI_HOST ssh_target required",
        "JLINK_SN identifier required",
        "JLINK_DEVICE identifier optional R7KA8D2KF_CPU0",
        "PI_REPO repo_path optional ra8-firmware",
        "_ra8_rig_validate_ipv4",
        "_ra8_rig_validate_dns",
        "_ra8_rig_validate_repo_path",
    )
    findings.extend(
        f"{CONTRACT}: missing authority token {token!r}" for token in required if token not in text
    )
    if re.search(r"(^|[;(])\s*eval\b", text, re.MULTILINE):
        findings.append(f"{CONTRACT}: eval is forbidden in the value authority")
    return findings


def audit_ansible(text: str) -> list[str]:
    """Report Ansible parser or exact-serialization authority drift."""
    findings: list[str] = []
    required = (
        "{{ dev_box_context_src }}/scripts/hil/rig_env_parse.py",
        'dev_box_hil_pi_repo: "{{ dev_box_hil_rig_env.PI_REPO }}"',
        'dev_box_hil_jlink_device: "{{ dev_box_hil_rig_env.JLINK_DEVICE }}"',
        "PI_REPO={{ dev_box_hil_pi_repo }}",
        'service = ["PI_HOST", "JLINK_SN", "JLINK_DEVICE"]',
        'interactive = [*service, "PI_REPO"]',
    )
    findings.extend(
        f"{ANSIBLE}: missing parser/serialization binding {token!r}"
        for token in required
        if token not in text
    )
    forbidden = (
        'allowed = {"PI_HOST"',
        "import shlex",
        "dev_box_hil_interactive_pi_host\n        is match",
        "dev_box_hil_rig_env.JLINK_SN is match",
        "dev_box_hil_runner_jlink_device_default",
    )
    findings.extend(
        f"{ANSIBLE}: retains duplicated parser/value grammar {token!r}"
        for token in forbidden
        if token in text
    )
    findings.extend(_audit_ansible_ephemeral_reporting(text))
    return findings


def audit_parser(text: str) -> list[str]:
    """Report parser delegation, protected-I/O, or shell execution drift."""
    findings: list[str] = []
    required = (
        'CONTRACT = Path(__file__).resolve().parent / "lib" / "rig_contract.sh"',
        '("/bin/bash", "-p", str(CONTRACT), *arguments)',
        'result = _run_contract("--validate", name, value)',
        'result = _run_contract("--describe")',
        "duplicate {key}",
        "unsupported assignment syntax",
        "never executes or expands",
        "ASSIGNMENT_RE.fullmatch",
        "LITERAL_RE.fullmatch",
        "INTERACTIVE_FIELDS",
        'output_format == "nul"',
    )
    findings.extend(
        f"{PARSER}: missing authority/parser binding {token!r}"
        for token in required
        if token not in text
    )
    source_reader = text.partition("def _read_source")[2].partition("def _parse_allowlisted_line")[
        0
    ]
    findings.extend(
        f"{PARSER}: protected source reader lacks {token!r}"
        for token in (
            "flags |= os.O_NOFOLLOW",
            "flags |= os.O_NONBLOCK",
            "info.st_uid != os.getuid()",
            "stat.S_IMODE(info.st_mode) != PROTECTED_MODE",
        )
        if token not in source_reader
    )
    if "shell=True" in text or re.search(r"\beval\s*\(", text):
        findings.append(f"{PARSER}: shell/eval execution is forbidden")
    return findings


def audit_bench_ansible(text: str) -> list[str]:
    """Bind the bench health check directly to the rig contract default."""
    required = (
        "/scripts/hil/lib/rig_contract.sh",
        "--default JLINK_DEVICE",
        'JLinkExe -device "$device"',
    )
    findings = [
        f"{BENCH_ANSIBLE}: missing device-authority binding {token!r}"
        for token in required
        if token not in text
    ]
    if "hil_bench_jlink_device" in text or "R7KA8D2KF_CPU0" in text:
        findings.append(f"{BENCH_ANSIBLE}: retains an independent J-Link device authority")
    return findings


def audit_remote_gdb_args(text: str) -> list[str]:
    """Require start-only device input and a device-free cleanup interface."""
    required = (
        "def remote_command(serial: str, port: str, *, device: str)",
        'remote.add_argument("--device", required=True)',
        "print(remote_command(args.serial, args.port, device=args.device))",
    )
    findings = [
        f"{REMOTE_GDB_ARGS}: missing start/cleanup split {token!r}"
        for token in required
        if token not in text
    ]
    if re.search(r'add_argument\("--device",\s*default=', text):
        findings.append(f"{REMOTE_GDB_ARGS}: device still has an independent default")
    if 'add_parser("cleanup")' in text or '"cleanup"' in text:
        findings.append(f"{REMOTE_GDB_ARGS}: cleanup retains a device-command surface")
    return findings


def audit_hil_consumers(root: Path) -> list[str]:
    """Report unsafe typed-field interpolation across HIL shell consumers."""
    findings: list[str] = []
    exempt = {RIG_ENV, CONTRACT, "scripts/hil/lib/tty_resolve.sh"}
    for path in sorted((root / "scripts/hil").rglob("*.sh")):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        active = "\n".join(_active_lines(text))
        if relative != CONTRACT and "R7KA8D2KF_CPU0" in active:
            findings.append(f"{relative}: hard-codes the contract-owned J-Link device")
        if re.search(r"\b(?:source|[.])\s+[\"']?\.env\b", active):
            findings.append(f"{relative}: executes a remote or local .env bypass")
        if relative not in exempt and EXPANSION.search(active) and not SOURCE_RIG.search(text):
            findings.append(f"{relative}: expands a rig field without sourcing rig_env.sh")
        for line_number, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            findings.extend(
                f"{relative}:{line_number}: PI_HOST is not one quoted argv/string value"
                for match in PI_HOST_EXPANSION.finditer(line)
                if not _quoted_expansion(line, match)
            )
            if relative not in {RIG_ENV, CONTRACT} and re.search(r"\$(?:\{)?PI_REPO\b", line):
                parameter_guard = line.strip() == ': "${PI_REPO:?}"'
                if not parameter_guard and ("printf -v" not in line or "%q" not in line):
                    findings.append(
                        f"{relative}:{line_number}: PI_REPO bypasses explicit %q serialization"
                    )
    required_calls = {
        "scripts/hil/run.sh": "rig_require PI_HOST JLINK_SN JLINK_DEVICE",
        "scripts/hil/run_direct.sh": "rig_require PI_HOST JLINK_SN JLINK_DEVICE",
        "scripts/hil/run_local.sh": "rig_require JLINK_SN JLINK_DEVICE",
        "scripts/hil/rtt_scrape.sh": "rig_require PI_HOST JLINK_DEVICE",
        "scripts/hil/camera_picture.sh": ("rig_require PI_HOST JLINK_SN JLINK_DEVICE PI_REPO"),
    }
    for relative, required in required_calls.items():
        if (root / relative).is_file() and required not in _read(root, relative):
            findings.append(f"{relative}: missing exact required-field call {required!r}")
    return findings


def audit_docs_and_ethernet(root: Path) -> list[str]:
    """Report stale Ethernet semantics or missing contract documentation."""
    findings: list[str] = []
    document = _read(root, HIL_DOC)
    suite = _read(root, HIL_SUITE)
    ethernet = _read(root, ETH_SCRIPT)
    defaults = _read(root, "infra/ansible/roles/hil_bench/defaults/main.yml")
    findings.extend(
        f"{HIL_DOC}: missing stable HIL semantic {token!r}"
        for token in (
            "rig_contract.sh",
            "interactive loader never",
            "non-symlink, mode-0600 regular file",
            "hil_bench_eth_iface",
            "hil_bench_eth_mac",
            "hil_bench_eth_sysfs_device",
            "hil_bench_eth_phc_index",
            "USB-Ethernet",
            "intentionally rejected",
            "IPv6 targets, including bracketed literals, are intentionally rejected",
        )
        if token not in document
    )
    findings.extend(
        f"{HIL_SUITE}: missing stable Ethernet semantic {token!r}"
        for token in (
            "fleet-declared built-in board-facing interface",
            "installed policy verifies its MAC, sysfs device, PHC",
            "USB adapters are rejected",
        )
        if token not in suite
    )
    findings.extend(
        f"HIL Ethernet prose retains stale auto-detection claim {stale!r}"
        for stale in ("detected automatically", "enxXX", "usbX device")
        if stale in document or stale in suite or stale in ethernet
    )
    findings.extend(
        f"{HIL_SUITE}: retains stale USB-interface claim {stale!r}"
        for stale in ("USB-Ethernet adapter", "enxXX", "usbX")
        if stale in suite
    )
    findings.extend(
        f"{ETH_SCRIPT}: missing installed-policy binding {token!r}"
        for token in (
            '"$RA8_HIL_PRIVILEGED_HELPER" --policy-interface',
            "verifies its permanent MAC, canonical sysfs device, PHC",
        )
        if token not in ethernet
    )
    findings.extend(
        f"hil_bench defaults lost fleet-owned Ethernet field {token!r}"
        for token in (
            'hil_bench_eth_iface: ""',
            'hil_bench_eth_mac: ""',
            'hil_bench_eth_sysfs_device: ""',
            "hil_bench_eth_phc_index: -1",
        )
        if token not in defaults
    )
    return findings


def scan(root: Path = REPO_ROOT) -> list[str]:
    """Run every typed rig, consumer, registration, and documentation audit."""
    findings: list[str] = []
    findings.extend(audit_rig_env(_read(root, RIG_ENV)))
    findings.extend(audit_contract(_read(root, CONTRACT)))
    findings.extend(audit_parser(_read(root, PARSER)))
    findings.extend(audit_ansible(_read(root, ANSIBLE)))
    findings.extend(audit_bench_ansible(_read(root, BENCH_ANSIBLE)))
    findings.extend(audit_remote_gdb_args(_read(root, REMOTE_GDB_ARGS)))
    findings.extend(audit_hil_consumers(root))
    findings.extend(audit_docs_and_ethernet(root))
    gate = _read(root, GATE)
    findings.extend(
        f"{GATE}: rig checker is not registered as {command!r}"
        for command in (
            "/bin/bash -p scripts/hil/lib/rig_contract.sh --selftest",
            "python3 scripts/hil/rig_env_parse.py --selftest",
            "python3 scripts/checks/check_hil_rig_contract.py --selftest",
            "python3 scripts/checks/check_hil_rig_contract.py",
        )
        if command not in gate
    )
    if "PI_REPO=" not in _read(root, EXAMPLE):
        findings.append(f"{EXAMPLE}: PI_REPO is absent from the documented contract")
    return findings


FAKE_HARNESS = r"""
set -euo pipefail
ssh() { [[ "$#" -eq 1 && "$1" == "sikar@10.0.40.103" ]]; printf 'SSH-FAKE\n'; }
scp() { [[ "$#" -eq 2 && "$2" == "sikar@10.0.40.103:/tmp/fw.hex" ]]; printf 'SCP-FAKE\n'; }
source "$1"
rig_require PI_HOST JLINK_SN JLINK_DEVICE PI_REPO
[[ -z "${TAPO_PASS+x}" ]]
ssh "$PI_HOST"
scp fw.hex "${PI_HOST}:/tmp/fw.hex"
"""
FAKE_VALID = (
    "PI_HOST=sikar@10.0.40.103\n"
    "JLINK_SN=123456789\n"
    "JLINK_DEVICE=R7KA8D2KF_CPU0\n"
    "PI_REPO=/home/ra8-hil/ra8-firmware\n"
)
FAKE_INVALID = (
    "PI_HOST=-oProxyCommand=bad\nJLINK_SN=1\n",
    "PI_HOST=user@@host\nJLINK_SN=1\n",
    "PI_HOST=$'host\\ncommand'\nJLINK_SN=1\n",
    "PI_HOST=host\nJLINK_SN=-1\n",
    "PI_HOST=host\nJLINK_SN=1\nPI_REPO=../repo\n",
    'PI_HOST=host\nJLINK_SN=1\nPI_REPO="repo\'bad"\n',
)


def _fake_value_cases(env_file: Path, command: tuple[str, ...]) -> None:
    """Prove valid argv transport and reject hostile declared values."""
    env_file.write_text(FAKE_VALID, encoding="utf-8")
    env_file.chmod(0o600)
    environment = {"PATH": "/usr/bin:/bin", "RA8_RIG_ENV": str(env_file)}
    result = _run_fixed(command, environment)
    _require(
        not result.returncode and result.stdout == "SSH-FAKE\nSCP-FAKE\n",
        f"valid fake transport failed: {result!r}",
    )
    for content in FAKE_INVALID:
        env_file.write_text(content, encoding="utf-8")
        result = _run_fixed(command, environment)
        _require(
            result.returncode != 0 and "-FAKE" not in result.stdout,
            f"unsafe rig value reached fake transport: {content!r}",
        )


def _fake_loader_cases(env_file: Path, command: tuple[str, ...]) -> None:
    """Prove commands/secrets stay data and protected-path checks fail closed."""
    marker = env_file.parent / "executed"
    env_file.write_text(
        FAKE_VALID
        + f"TAPO_PASS='$(touch {marker}); literal spaces | data'\n"
        + f"touch {marker}\n",
        encoding="utf-8",
    )
    environment = {"PATH": "/usr/bin:/bin", "RA8_RIG_ENV": str(env_file)}
    result = _run_fixed(command, environment)
    _require(
        not result.returncode and not marker.exists(),
        "declarative rig loader executed an unrelated command row",
    )
    env_file.chmod(0o644)
    result = _run_fixed(command, environment)
    _require(result.returncode != 0, "interactive loader accepted mode-0644 input")
    target = env_file.parent / "target.env"
    target.write_text(FAKE_VALID, encoding="utf-8")
    target.chmod(0o600)
    env_file.unlink()
    env_file.symlink_to(target)
    result = _run_fixed(command, environment)
    _require(result.returncode != 0, "interactive loader accepted a symlink input")


def fake_transport_selftest() -> None:
    """Prove accepted values are one argv and rejected values never reach fakes."""
    with tempfile.TemporaryDirectory(prefix="ra8-rig-transport-") as temporary:
        env_file = Path(temporary) / "rig.env"
        command = (
            "/bin/bash",
            "-p",
            "-c",
            FAKE_HARNESS,
            "rig-harness",
            str(REPO_ROOT / RIG_ENV),
        )
        _fake_value_cases(env_file, command)
        _fake_loader_cases(env_file, command)


def consumer_quoting_selftest() -> None:
    """Prove the interpolation audit rejects bare option-position values."""
    with tempfile.TemporaryDirectory(prefix="ra8-rig-consumer-") as temporary:
        root = Path(temporary)
        script = root / "scripts" / "hil" / "consumer.sh"
        script.parent.mkdir(parents=True)
        script.write_text(
            '#!/bin/bash -p\nsource "$ROOT/scripts/hil/lib/rig_env.sh"\nssh $PI_HOST\n',
            encoding="utf-8",
        )
        findings = audit_hil_consumers(root)
        _require(
            any("PI_HOST is not one quoted" in finding for finding in findings),
            "consumer audit accepted an unquoted PI_HOST",
        )
        script.write_text(
            '#!/bin/bash -p\nsource "$ROOT/scripts/hil/lib/rig_env.sh"\nssh "$PI_HOST"\n',
            encoding="utf-8",
        )
        _require(not audit_hil_consumers(root), "consumer audit rejected quoted PI_HOST")
        script.write_text(
            '#!/bin/bash -p\nsource "$ROOT/scripts/hil/lib/rig_env.sh"\n'
            "rig_require PI_HOST\ndevice R7KA8D2KF_CPU0\nsource .env\n",
            encoding="utf-8",
        )
        findings = audit_hil_consumers(root)
        _require(
            any("hard-codes" in finding for finding in findings)
            and any(".env bypass" in finding for finding in findings),
            "consumer audit accepted literal-device and executable-env bypasses",
        )


Mutation = tuple[Callable[[str], list[str]], str]


def _authority_mutations(sources: dict[str, str]) -> tuple[Mutation, ...]:
    """Build must-fire mutations for the loader, contracts, and consumers."""
    return (
        (
            audit_rig_env,
            sources["rig"].replace("ra8_rig_validate_loaded true", ": # omitted", 1),
        ),
        (
            audit_rig_env,
            sources["rig"].replace('source "$_rig_lib_dir/rig_contract.sh"', ":", 1),
        ),
        (
            audit_contract,
            sources["contract"].replace(
                "BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=",
                "*) :",
                1,
            ),
        ),
        (audit_ansible, sources["ansible"].replace("rig_env_parse.py", "other_parser.py", 1)),
        (
            audit_ansible,
            sources["ansible"].replace("PI_REPO={{ dev_box_hil_pi_repo }}", "", 1),
        ),
        (
            audit_ansible,
            sources["ansible"].replace(
                'dev_box_hil_jlink_device: "{{ dev_box_hil_rig_env.JLINK_DEVICE }}"',
                "dev_box_hil_runner_jlink_device_default",
                1,
            ),
        ),
        (
            audit_bench_ansible,
            sources["bench"].replace('JLinkExe -device "$device"', "JLinkExe -device literal", 1),
        ),
        (
            audit_remote_gdb_args,
            sources["remote"].replace(
                'remote.add_argument("--device", required=True)',
                'remote.add_argument("--device", default="literal")',
                1,
            ),
        ),
    )


def _ansible_reporting_mutations(sources: dict[str, str]) -> tuple[Mutation, ...]:
    """Build must-fire mutations for same-run scratch reporting controls."""
    allocation = "      register: dev_box_hil_rig_env_result\n"
    allocation += "      changed_when: false\n      check_mode: false"
    cleanup = "        - dev_box_hil_rig_env_result.path is defined\n"
    cleanup += "      changed_when: false\n      check_mode: false"
    return (
        (
            audit_ansible,
            sources["ansible"].replace(
                allocation,
                allocation.replace("      changed_when: false\n", "", 1),
                1,
            ),
        ),
        (
            audit_ansible,
            sources["ansible"].replace(
                cleanup,
                cleanup.replace("      changed_when: false\n", "", 1),
                1,
            ),
        ),
        (
            audit_ansible,
            _append_duplicate_task(
                sources["ansible"],
                "Allocate a protected allowlist result file on the control node",
            ),
        ),
        (
            audit_ansible,
            _append_duplicate_task(
                sources["ansible"],
                "Remove the protected allowlist result file",
            ),
        ),
    )


def _append_duplicate_task(text: str, name: str) -> str:
    """Append one valid duplicate-name task for an exact-count mutation."""
    return (
        f"{text.rstrip()}\n\n- name: {name}\n"
        "  ansible.builtin.debug:\n"
        "    msg: duplicate exact-name fixture\n"
        "  changed_when: false\n"
        "  check_mode: false\n"
    )


def _parser_mutations(sources: dict[str, str]) -> tuple[Mutation, ...]:
    """Build must-fire mutations for parser delegation and protected I/O."""
    return (
        (
            audit_parser,
            sources["parser"].replace(
                'result = _run_contract("--validate", name, value)', "return", 1
            ),
        ),
        (
            audit_parser,
            sources["parser"].replace('("/bin/bash", "-p", str(CONTRACT), *arguments)', "()", 1),
        ),
        (audit_parser, sources["parser"].replace("flags |= os.O_NOFOLLOW", "pass", 1)),
        (audit_parser, sources["parser"].replace("ASSIGNMENT_RE.fullmatch", "re.match", 1)),
    )


def _static_mutations(sources: dict[str, str]) -> tuple[Mutation, ...]:
    """Build exact must-fire mutations for every static authority seam."""
    return (
        _authority_mutations(sources)
        + _ansible_reporting_mutations(sources)
        + _parser_mutations(sources)
    )


def selftest() -> None:
    """Exercise every static binding and fake transport in both directions."""
    sources = {
        "rig": _read(REPO_ROOT, RIG_ENV),
        "contract": _read(REPO_ROOT, CONTRACT),
        "ansible": _read(REPO_ROOT, ANSIBLE),
        "bench": _read(REPO_ROOT, BENCH_ANSIBLE),
        "parser": _read(REPO_ROOT, PARSER),
        "remote": _read(REPO_ROOT, REMOTE_GDB_ARGS),
    }
    audits = (
        audit_rig_env(sources["rig"]),
        audit_contract(sources["contract"]),
        audit_ansible(sources["ansible"]),
        audit_bench_ansible(sources["bench"]),
        audit_parser(sources["parser"]),
        audit_remote_gdb_args(sources["remote"]),
    )
    _require(not any(audits), "live approved consumer fixtures are inconsistent")
    mutations = _static_mutations(sources)
    for audit, mutated in mutations:
        _require(
            bool(audit(mutated)),
            f"{audit.__name__} stayed quiet on a must-fire mutation",
        )
    fake_transport_selftest()
    consumer_quoting_selftest()
    print(
        "check_hil_rig_contract.py --selftest: PASS "
        f"({len(mutations) + 7} must-fire, 3 valid paths must-pass)"
    )


def main() -> int:
    """Run a selftest or scan the current repository."""
    if sys.argv[1:] == ["--selftest"]:
        selftest()
        return 0
    if sys.argv[1:]:
        print("usage: check_hil_rig_contract.py [--selftest]", file=sys.stderr)
        return 2
    findings = scan()
    if findings:
        print("check_hil_rig_contract.py: findings:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("check_hil_rig_contract.py: shared contract, consumers, and HIL semantics agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

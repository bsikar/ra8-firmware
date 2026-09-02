# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact execution and raw-byte ownership contract for native-HIL cache repair."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from collections.abc import Callable
from copy import deepcopy
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

import authored_token_census as atc
import hil_cache_isolation_rules as hci
import yaml
from check_shebangs import PRIVILEGED_BODY_PREFIX

from scripts.dev.git_environment import trusted_git_executable

RULES_SOURCE = "scripts/checks/hil_cache_repair_rules.py"
PLAYBOOK = "infra/ansible/playbooks/hil-cache-repair.yml"
DRIVER = "scripts/dev/hil_cache_repair.sh"
JUSTFILE = "infra/hil-cache.just"
DOCUMENTATION = "infra/README.md"
JUST_REFERENCE_CHECKER = "scripts/checks/check_just_references.py"
ENTRYPOINT_POLICY = "scripts/checks/shell_entrypoint_policy.py"
SHEBANG_CHECKER = "scripts/checks/check_shebangs.py"
DEV_BOX_DEFAULTS = "infra/ansible/roles/dev_box/defaults/main.yml"
PRECOMMIT_HOOK = "just/hooks.just"
FULL_GATE = "scripts/ci/gates/checks.sh"
CORE_INPUT_FILES = (
    PLAYBOOK,
    DRIVER,
    JUSTFILE,
    DOCUMENTATION,
    DEV_BOX_DEFAULTS,
    hci.HIL_RUNNER_TASKS,
    hci.HIL_RUNNER_SERVICE,
    hci.SHARED_CACHE_TASKS,
)
SHARED_CACHE_ROOT = hci.SHARED_CACHE_ROOT
HIL_CACHE_ROOT = hci.HIL_CACHE_ROOT
CACHE_ROOT = HIL_CACHE_ROOT
RUNNER_USER = "ra8-hil"
SAFETY_VARS = frozenset({"dev_box_ccache_dir", "dev_box_hil_ccache_dir", "dev_box_hil_runner_user"})
INVENTORY_SCOPE_PARTS = frozenset({"group_vars", "host_vars"})
SELFTEST_OVERRIDE_COUNT = 9
DEV_CONNECT_DIGEST = "cfeb5a1bcdf44c1fb822392bbc6dc985754d13562694797c0a8dd7b3bc01597f"
ENTRYPOINT_TOKENS = (
    "hil_cache_repair.sh",
    "hil-cache-repair.yml",
    "hil-cache.just",
    "hil_cache_check",
    "hil_cache_apply",
)
PINNED_ENTRYPOINT_OWNER_FILES = frozenset({RULES_SOURCE})
PINNED_CHECKER_OCCURRENCES = {
    JUST_REFERENCE_CHECKER: (
        'STANDALONE_SURFACES = {"infra/hil-cache.just": frozenset({"check", "apply"})}',
        'f"`{just_word} --justfile infra/hil-cache.just check`",',
        'f"`{just_word} --justfile infra/hil-cache.just missing`",',
    ),
    SHEBANG_CHECKER: (),
    ENTRYPOINT_POLICY: ('"scripts/dev/hil_cache_repair.sh": ShellPolicy(',),
}
AUTHORED_EXCLUDED_PARTS = frozenset(
    {
        ".cache",
        ".git",
        ".venv",
        "__pycache__",
        "build",
        "generated",
        "node_modules",
        "third_party",
        "vendor",
    }
)

_repair_just_fixture = "just --justfile infra/hil-cache.just"
_repair_driver_fixture = "bash scripts/dev/hil_cache_repair.sh"
ENTRYPOINT_OWNERSHIP_SELFTEST_CASES = (
    ("inline documentation", "docs/inline.md", f"Use `{_repair_just_fixture} apply`.\n"),
    ("bulleted documentation", "docs/bullet.md", f"- {_repair_just_fixture} apply\n"),
    (
        "environment direct driver",
        "scripts/direct.sh",  # PATHREF-OK: throwaway ownership selftest fixture
        f"env -i {_repair_driver_fixture} apply\n",
    ),
    (
        "root recipe",
        "justfile",
        f"hil_cache_apply:\n    {_repair_driver_fixture} apply\n",
    ),
    (
        "module recipe",
        "just/infra.just",
        f"hil_cache_check:\n    {_repair_driver_fixture} check\n",
    ),
    (
        "renamed driver recipe",
        "just/alternate.just",
        f"cache-repair:\n    {_repair_driver_fixture} apply\n",
    ),
    (
        "renamed playbook recipe",
        "just/alternate.just",
        "cache-repair:\n    ansible-playbook infra/ansible/playbooks/hil-cache-repair.yml\n",
    ),
    (
        "renamed standalone recipe",
        "just/alternate.just",
        "cache-repair:\n    just --justfile infra/hil-cache.just apply\n",
    ),
    ("bash login startup", ".bash_login", f"{_repair_driver_fixture} apply\n"),
    ("bash logout startup", ".bash_logout", f"{_repair_driver_fixture} apply\n"),
    ("zsh login startup", ".zlogin", f"{_repair_driver_fixture} apply\n"),
    ("zsh logout startup", ".zlogout", f"{_repair_driver_fixture} apply\n"),
    (
        "GNU makefile",
        "GNUmakefile",
        f"repair:\n\t{_repair_driver_fixture} apply\n",
    ),
    (
        "lowercase makefile",
        "makefile",
        f"repair:\n\t{_repair_driver_fixture} apply\n",
    ),
    (
        "arbitrary extension",
        "notes/repair.not-a-command-type",
        f"{_repair_driver_fixture} apply\n",
    ),
    (
        "arbitrary extensionless name",
        "tools/repair",
        f"{_repair_driver_fixture} apply\n",
    ),
)

SAFE_DOCUMENTED_COMMANDS = (
    "just --justfile infra/hil-cache.just check",
    "just --justfile infra/hil-cache.just apply",
)
APPROVED_JUSTFILE = """# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set dotenv-load := false

# Dry-run only the fixed private HIL compiler-cache repair on dev
check:
    /bin/bash -p "{{ justfile_directory() }}/../scripts/dev/hil_cache_repair.sh" check

# Apply only the fixed private HIL compiler-cache repair on dev
[confirm("Create or repair only ra8-hil's private compiler cache on dev?")]
apply:
    /bin/bash -p "{{ justfile_directory() }}/../scripts/dev/hil_cache_repair.sh" apply
"""
APPROVED_PLAYBOOK_DIGEST = "dafa425d2a2661d3c726e72117199c49a9debf603103347b18c151badf7d8bd0"

APPROVED_DRIVER_LINES = (
    *(line.strip() for line in PRIVILEGED_BODY_PREFIX),
    "set -euo pipefail",
    "export BASH_ENV=/dev/null ENV=/dev/null PYTHONNOUSERSITE=1",
    "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
    "PATH=/usr/bin:/bin",
    "export PATH",
    'ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"',
    'PYTHON="${ROOT}/.venv/bin/python3"',
    'ANSIBLE_PLAYBOOK="${ROOT}/.venv/bin/ansible-playbook"',
    'mode="${1:-}"',
    "reject_ansible_environment() {",
    "local name",
    "while IFS='=' read -r name _; do",
    'if [[ "${name}" == ANSIBLE_* ]]; then',
    'echo "error: inherited ANSIBLE_* environment is not allowed" >&2',
    "return 2",
    "fi",
    "done < <(env)",
    "}",
    "selftest_rejection() {",
    'local key="$1" output rc',
    "set +e",
    'output="$(env "${key}=unsafe" "${BASH_SOURCE[0]}" __probe_environment 2>&1)"',
    "rc=$?",
    "set -e",
    'if [ "${rc}" -ne 2 ] || [ "${output}" != "error: inh'
    'erited ANSIBLE_* environment is not allowed" ]; then',
    'echo "hil_cache_repair.sh --selftest: ${key} was not rejected fail-closed" >&2',
    "return 1",
    "fi",
    "}",
    "selftest() {",
    "selftest_rejection ANSIBLE_HOST_KEY_CHECKING",
    "selftest_rejection ANSIBLE_ACTION_PLUGINS",
    'echo "hil_cache_repair.sh --selftest: PASS"',
    "}",
    "reject_ansible_environment",
    'if [ "$#" -eq 1 ] && [ "${mode}" = __probe_boundary ]; then',
    '[ "${PATH}" = /usr/bin:/bin ] && [ "${BASH_ENV}" = /dev/null ] &&',
    '[ "${ENV}" = /dev/null ] && [ -z "${PYTHONPATH:-}" ] &&',
    '[ -z "${PYTHONHOME:-}" ] && [ -z "${RA8_TOOL_VENV:-}" ]',
    "exit",
    "fi",
    'if [ "$#" -eq 1 ] && [ "${mode}" = __probe_environment ]; then',
    'echo "environment accepted"',
    "exit 0",
    "fi",
    'if [ "$#" -eq 1 ] && [ "${mode}" = --selftest ]; then',
    "selftest",
    "exit 0",
    "fi",
    'if [ "$#" -ne 1 ] || { [ "${mode}" != check ] && [ "${mode}" != apply ]; }; then',
    'echo "usage: $0 check|apply" >&2',
    "exit 2",
    "fi",
    'if [ ! -x "${PYTHON}" ] || [ ! -x "${ANSIBLE_PLAYBOOK}" ]; then',
    "echo \"error: locked Ansible environment is absent; run 'just setup-python'\" >&2",
    "exit 2",
    "fi",
    "umask 077",
    'scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-hil-cache-repair.XXXXXXXX")"',
    'inventory="${scratch}/inventory.ini"',
    'playbook="${scratch}/hil-cache-repair.yml"',
    'config="${scratch}/ansible.cfg"',
    "cleanup() {",
    'rm -f -- "${inventory}" "${playbook}" "${config}"',
    'rmdir -- "${scratch}"',
    "}",
    "trap cleanup EXIT",
    "trap 'exit 129' HUP",
    "trap 'exit 130' INT",
    "trap 'exit 143' TERM",
    '"${PYTHON}" "${ROOT}/scripts/checks/check_fleet_declaration.py" >/dev/null',
    '"${PYTHON}" "${ROOT}/scripts/dev/fleet.py" inventory --stdout >"${inventory}"',
    'cp "${ROOT}/infra/ansible/playbooks/hil-cache-repair.yml" "${playbook}"',
    "printf '%s\\n' '[defaults]' 'host_key_checking = "
    "True' 'retry_files_enabled = False' >\"${config}\"",
    'args=("${ANSIBLE_PLAYBOOK}" -i "${inventory}" "${playbook}" --limit dev)',
    'if [ "${mode}" = check ]; then',
    "args+=(--check --diff)",
    "fi",
    'ANSIBLE_CONFIG="${config}" \\',
    'ANSIBLE_COLLECTIONS_PATH="${ROOT}/.ansible/collections" \\',
    "ANSIBLE_COLLECTIONS_SCAN_SYS_PATH=false \\",
    "PYTHONNOUSERSITE=1 \\",
    '"${args[@]}"',
    "else",
    '[[ "$-" == *p* ]]',
    "fi",
)


def _read_yaml(path: Path) -> tuple[object, str | None]:
    """Read one YAML document, returning a fail-closed error."""
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")), None
    except (OSError, UnicodeError, yaml.YAMLError) as exc:
        return None, str(exc)


def _active_shell_lines(text: str) -> tuple[str, ...]:
    """Strip comments and blanks from the small fixed driver."""
    return tuple(
        line.strip()
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def _check_playbook_data(data: object) -> list[str]:
    """Require the complete play document, not a tag-derived approximation."""
    try:
        encoded = json.dumps(data, sort_keys=True, separators=(",", ":")).encode()
    except (TypeError, ValueError):
        encoded = b""
    if hashlib.sha256(encoded).hexdigest() != APPROVED_PLAYBOOK_DIGEST:
        return [f"{PLAYBOOK}: execution surface differs from the exact cache-only contract"]
    return []


def _check_driver_text(text: str) -> list[str]:
    """Pin every executable line in the fixed no-argument driver."""
    if _active_shell_lines(text) != APPROVED_DRIVER_LINES:
        return [f"{DRIVER}: executable surface differs from the isolated driver contract"]
    return []


def _check_justfile_text(text: str) -> list[str]:
    """Require the complete standalone file byte-for-byte."""
    if text != APPROVED_JUSTFILE:
        return [f"{JUSTFILE}: content differs from the exact standalone entry point"]
    return []


def _check_documentation_text(text: str) -> list[str]:
    """Require the repair documentation to expose only the isolated entry point."""
    repair_occurrences = tuple(
        line for line in text.splitlines() if any(token in line for token in ENTRYPOINT_TOKENS)
    )
    if repair_occurrences != SAFE_DOCUMENTED_COMMANDS:
        return [f"{DOCUMENTATION}: HIL cache repair commands are not the exact safe front door"]
    return []


def _authored_files(
    repo_root: Path,
    lstat_file: Callable[[Path], os.stat_result] = atc.path_lstat,
) -> list[Path]:
    """Return Git-authored regular files outside explicit excluded trees."""
    return atc.authored_files(repo_root, AUTHORED_EXCLUDED_PARTS, lstat_file)


def policy_input_files(repo_root: Path) -> tuple[str, ...]:
    """Return core inputs plus every file in the authored-token census."""
    discovered = (path.relative_to(repo_root).as_posix() for path in _authored_files(repo_root))
    return tuple(dict.fromkeys((*CORE_INPUT_FILES, *discovered)))


def _entrypoint_byte_hits(data: bytes) -> tuple[str, ...]:
    """Find repair tokens encoded as UTF-8, UTF-16LE or UTF-16BE."""
    return atc.token_hits(data, ENTRYPOINT_TOKENS)


def _check_checker_occurrences(text: str, relative: str = JUST_REFERENCE_CHECKER) -> list[str]:
    """Pin every allowed repair reference in one generic checker."""
    actual = tuple(
        line.strip()
        for line in text.splitlines()
        if any(token in line for token in ENTRYPOINT_TOKENS)
    )
    if actual != PINNED_CHECKER_OCCURRENCES[relative]:
        return [f"{relative}: repair references differ from exact checker internals"]
    return []


def _check_exact_owner_source(source: atc.AuthoredSource) -> list[str] | None:
    """Validate one exact front-door owner in either authored byte view."""
    checks = {
        DRIVER: _check_driver_text,
        JUSTFILE: _check_justfile_text,
        DOCUMENTATION: _check_documentation_text,
    }
    check = checks.get(source.relative)
    if check is None:
        return None
    try:
        text = source.data.decode("utf-8")
    except UnicodeDecodeError:
        return [f"{source.relative} [{source.view}]: exact front-door owner is not UTF-8"]
    return [f"{problem} [{source.view} view]" for problem in check(text)]


def _check_entrypoint_ownership(
    repo_root: Path,
    lstat_file: Callable[[Path], os.stat_result] = atc.path_lstat,
    read_file: Callable[[Path], bytes] = atc.path_read_bytes,
) -> list[str]:
    """Reject every repair entrypoint occurrence outside its exact owners."""
    problems = []
    try:
        authored = atc.authored_sources(repo_root, AUTHORED_EXCLUDED_PARTS, lstat_file, read_file)
    except atc.CensusError as exc:
        return [f"authored-file repair ownership cannot be proven: {exc}"]
    for source in authored:
        relative = source.relative
        exact_owner_problems = _check_exact_owner_source(source)
        if exact_owner_problems is not None:
            problems.extend(exact_owner_problems)
            continue
        if relative in PINNED_ENTRYPOINT_OWNER_FILES:
            continue
        if relative in PINNED_CHECKER_OCCURRENCES:
            try:
                checker_text = source.data.decode("utf-8")
            except UnicodeDecodeError:
                problems.append(
                    f"{relative} [{source.view}]: exact checker internals are not UTF-8"
                )
            else:
                problems.extend(_check_checker_occurrences(checker_text, relative))
            continue
        hits = _entrypoint_byte_hits(source.data)
        if hits:
            problems.append(
                f"{relative} [{source.view}]: repair entrypoint token(s) "
                f"{list(hits)!r} have no ownership"
            )
    return problems


def _mapping_keys(value: object) -> set[str]:
    """Collect mapping keys recursively from inventory variable documents."""
    keys: set[str] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            keys.add(str(key))
            keys.update(_mapping_keys(child))
    elif isinstance(value, list):
        for child in value:
            keys.update(_mapping_keys(child))
    return keys


def _check_inventory_overrides(repo_root: Path) -> list[str]:
    """Reject safety-owned path or identity variables in every inventory scope."""
    ansible_root = repo_root / "infra" / "ansible"
    problems = []
    for path in sorted(ansible_root.rglob("*")):
        if not path.is_file() or path.suffix not in {".json", ".yml", ".yaml"}:
            continue
        if not (INVENTORY_SCOPE_PARTS & set(path.parts)):
            continue
        loaded, error = _read_yaml(path)
        relative = path.relative_to(repo_root).as_posix()
        if error:
            problems.append(f"{relative}: cannot audit safety-owned variables: {error}")
            continue
        problems.extend(
            f"{relative}: may not override safety-owned variable {key!r}"
            for key in sorted(_mapping_keys(loaded) & SAFETY_VARS)
        )
    return problems


def _check_defaults(defaults: object) -> list[str]:
    """Keep the full dev_box converge aligned with the standalone repair."""
    if not isinstance(defaults, dict):
        return [f"{DEV_BOX_DEFAULTS}: expected a YAML mapping"]
    expected = {
        "dev_box_ccache_dir": SHARED_CACHE_ROOT,
        "dev_box_hil_ccache_dir": HIL_CACHE_ROOT,
        "dev_box_hil_ccache_max_size": "10G",
        "dev_box_hil_runner_user": RUNNER_USER,
    }
    return [
        f"{DEV_BOX_DEFAULTS}: {key} is {defaults.get(key)!r}, expected {value!r}"
        for key, value in expected.items()
        if defaults.get(key) != value
    ]


def _connect_digest(connect: object) -> str:
    """Hash a normalized connection mapping without reporting endpoint values."""
    try:
        encoded = json.dumps(connect, sort_keys=True, separators=(",", ":")).encode()
    except (TypeError, ValueError):
        return ""
    return hashlib.sha256(encoded).hexdigest()


def _check_dispatch(
    fleet: dict[str, Any], expected_connect_digest: str = DEV_CONNECT_DIGEST
) -> list[str]:
    """Require the fixed dev target and its private SSH identity to remain exact."""
    dev = fleet.get("hosts", {}).get("dev", {})
    if not isinstance(dev, dict):
        return ["infra/fleet.yml: fixed HIL cache target or private SSH identity changed"]
    hil = dev.get("hil_runner", {})
    connect = dev.get("connect", {})
    if (
        dev.get("class") != "dev_box"
        or "dev-box" not in dev.get("provisions", [])
        or not isinstance(hil, dict)
        or not isinstance(connect, dict)
        or set(connect) != {"address", "user"}
        or _connect_digest(connect) != expected_connect_digest
    ):
        return ["infra/fleet.yml: fixed HIL cache target or private SSH identity changed"]
    return []


def _check_text_file(
    repo_root: Path, relative: str, checker: Callable[[str], list[str]]
) -> list[str]:
    """Read and validate one textual execution-surface file."""
    try:
        return checker((repo_root / relative).read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as exc:
        return [f"{relative}: cannot read execution surface: {exc}"]


def _check_gate_wiring_texts(hook_text: str, gate_text: str) -> list[str]:
    """Require unconditional pre-commit and full-gate repair validation."""
    hook_line = "        pre-commit-checks"
    gate_pair = (
        "  python3 scripts/checks/check_fleet_declaration.py --selftest\n"
        "  python3 scripts/checks/check_fleet_declaration.py\n"
    )
    problems = []
    if hook_text.splitlines().count(hook_line) != 1:
        problems.append(f"{PRECOMMIT_HOOK}: pre-commit-checks is not wired exactly once")
    if gate_text.count(gate_pair) != 1:
        problems.append(f"{FULL_GATE}: fleet guard pair is not wired exactly once")
    return problems


def _check_gate_wiring(repo_root: Path) -> list[str]:
    """Read the two unconditional gate dispatch surfaces fail-closed."""
    try:
        return _check_gate_wiring_texts(
            (repo_root / PRECOMMIT_HOOK).read_text(encoding="utf-8"),
            (repo_root / FULL_GATE).read_text(encoding="utf-8"),
        )
    except (OSError, UnicodeError) as exc:
        return [f"HIL cache repair gate wiring cannot be read: {exc}"]


def check(repo_root: Path, fleet: dict[str, Any]) -> list[str]:
    """Validate the standalone playbook, driver, recipes, defaults and inventory."""
    playbook, playbook_error = _read_yaml(repo_root / PLAYBOOK)
    defaults, defaults_error = _read_yaml(repo_root / DEV_BOX_DEFAULTS)
    problems = []
    if playbook_error:
        problems.append(f"{PLAYBOOK}: cannot read YAML: {playbook_error}")
    else:
        problems += _check_playbook_data(playbook)
    if defaults_error:
        problems.append(f"{DEV_BOX_DEFAULTS}: cannot read YAML: {defaults_error}")
    else:
        problems += _check_defaults(defaults)
    problems += _check_text_file(repo_root, DRIVER, _check_driver_text)
    problems += _check_text_file(repo_root, JUSTFILE, _check_justfile_text)
    problems += _check_text_file(repo_root, DOCUMENTATION, _check_documentation_text)
    return (
        problems
        + _check_entrypoint_ownership(repo_root)
        + _check_inventory_overrides(repo_root)
        + hci.check(repo_root)
        + _check_gate_wiring(repo_root)
        + _check_dispatch(fleet)
    )


def _playbook_mutations() -> dict[str, Any]:
    """Return independently widened playbook documents for both-direction tests."""
    return {
        "rescue systemctl": lambda play: play[0]["tasks"][-1].update(
            {"rescue": [{"name": "restart", "ansible.builtin.command": "systemctl restart x"}]}
        ),
        "pre-task always": lambda play: play[0].update(
            {"pre_tasks": [{"name": "escape", "tags": "always", "ansible.builtin.command": "id"}]}
        ),
        "role always": lambda play: play[0].update(
            {"roles": [{"role": "dev_box", "tags": "always"}]}
        ),
        "include apply delegate": lambda play: play[0]["tasks"].append(
            {
                "name": "escape include delegate",
                "ansible.builtin.include_tasks": {
                    "file": "escape.yml",
                    "apply": {"delegate_to": "star"},
                },
            }
        ),
        "include apply check mode": lambda play: play[0]["tasks"].append(
            {
                "name": "escape include check mode",
                "ansible.builtin.include_tasks": {
                    "file": "escape.yml",
                    "apply": {"check_mode": False},
                },
            }
        ),
        "include apply vars": lambda play: play[0]["tasks"].append(
            {
                "name": "escape include vars",
                "ansible.builtin.include_tasks": {
                    "file": "escape.yml",
                    "apply": {"vars": {"dev_box_hil_ccache_dir": "/"}},
                },
            }
        ),
        "filesystem root": lambda play: play[0]["tasks"][4]["ansible.builtin.file"].update(
            {"path": "/"}
        ),
        "root identity": lambda play: play[0]["tasks"][0]["ansible.builtin.getent"].update(
            {"key": "root"}
        ),
        "all hosts": lambda play: play[0].update({"hosts": "all"}),
        "task vars override": lambda play: play[0]["tasks"][4].update(
            {"vars": {"dev_box_hil_ccache_dir": "/"}}
        ),
        "handler notify": lambda play: play[0]["tasks"][4].update({"notify": "restart runner"}),
        "forced check mode": lambda play: play[0]["tasks"][5].update({"check_mode": False}),
        "delegation": lambda play: play[0]["tasks"][5].update({"delegate_to": "star"}),
        "connection override": lambda play: play[0].update({"connection": "local"}),
        "shared cache crossing": lambda play: play[0]["tasks"][4]["ansible.builtin.file"].update(
            {"path": SHARED_CACHE_ROOT}
        ),
    }


def _selftest_playbook(repo_root: Path) -> list[str]:
    """Prove the exact document accepts once and rejects every widening shape."""
    failures = []
    approved, error = _read_yaml(repo_root / PLAYBOOK)
    if error:
        return [f"  cannot load approved standalone playbook: {error}"]
    if _check_playbook_data(deepcopy(approved)):
        failures.append("  the approved standalone playbook was rejected")
    for name, mutate in _playbook_mutations().items():
        broken = deepcopy(approved)
        mutate(broken)
        if not _check_playbook_data(broken):
            failures.append(f"  playbook widening was accepted: {name}")
    return failures


def _selftest_driver(repo_root: Path) -> list[str]:
    """Prove exact driver lines and inherited-environment rejection."""
    failures = []
    good_driver = "\n".join(APPROVED_DRIVER_LINES)
    if _check_driver_text(good_driver):
        failures.append("  the approved driver execution lines were rejected")
    if not _check_driver_text(good_driver + "\nsystemctl restart ra8-hil\n"):
        failures.append("  an extra driver command was accepted")

    clean_env = {key: value for key, value in os.environ.items() if not key.startswith("ANSIBLE_")}
    driver_selftest = subprocess.run(  # noqa: S603 -- exact repository-owned executable
        [repo_root / DRIVER, "--selftest"],
        check=False,
        capture_output=True,
        text=True,
        env=clean_env,
    )
    if (
        driver_selftest.returncode
        or driver_selftest.stdout.strip() != "hil_cache_repair.sh --selftest: PASS"
        or driver_selftest.stderr
    ):
        failures.append("  driver did not reject inherited Ansible control variables")
    with TemporaryDirectory(prefix="ra8-hil-cache-boundary-") as raw:
        poison = Path(raw) / "startup.sh"
        poison.write_text("printf poisoned\n", encoding="utf-8")
        hostile = clean_env.copy()
        for key in ("PYTHONHOME", "PYTHONPATH", "RA8_TOOL_VENV"):
            hostile[key] = "/unsafe"
        hostile.update(BASH_ENV=str(poison), ENV=str(poison), PATH=str(Path(raw) / "absent-bin"))
        boundary = subprocess.run(  # noqa: S603 -- exact repository-owned executable
            [repo_root / DRIVER, "__probe_boundary"],
            check=False,
            capture_output=True,
            text=True,
            env=hostile,
        )
        if boundary.returncode or boundary.stdout or boundary.stderr:
            failures.append("  hostile startup state reached the cache-repair boundary")
    return failures


def _selftest_just_content() -> list[str]:
    """Prove every byte of the standalone Justfile is pinned."""
    failures = []
    if _check_justfile_text(APPROVED_JUSTFILE):
        failures.append("  approved standalone Justfile was rejected")
    just_mutations = {
        "caller-controlled host": APPROVED_JUSTFILE.replace("check:", "check host:", 1),
        "removed confirmation": APPROVED_JUSTFILE.replace("[confirm", "[private", 1),
        "dotenv enabled": APPROVED_JUSTFILE.replace("dotenv-load := false", "dotenv-load := true"),
        "unexpected shell setting": APPROVED_JUSTFILE.replace(
            "set dotenv-load := false",
            'set dotenv-load := false\nset shell := ["sh", "-c"]',
        ),
        "import": APPROVED_JUSTFILE.replace(
            "set dotenv-load := false", 'set dotenv-load := false\nimport "../just/infra.just"'
        ),
        "export backtick": APPROVED_JUSTFILE.replace(
            "set dotenv-load := false", "set dotenv-load := false\nexport PATH := `printf /tmp`"
        ),
        "per-recipe working directory": APPROVED_JUSTFILE.replace(
            "check:", '[working-directory("/tmp")]\ncheck:', 1
        ),
        "attribute before confirmation": APPROVED_JUSTFILE.replace(
            "[confirm", "[private]\n[confirm", 1
        ),
        "changed wrapper": APPROVED_JUSTFILE.replace("hil_cache_repair.sh", "infra.sh", 1),
        "extra body line": APPROVED_JUSTFILE.replace(
            " check\n\n# Apply", " check\n    true\n\n# Apply"
        ),
        "ssh star body": APPROVED_JUSTFILE.replace(
            " check\n\n# Apply", " check\n    ssh star true\n\n# Apply"
        ),
        "reboot body": APPROVED_JUSTFILE.replace(" apply\n", " apply\n    reboot\n"),
    }
    # A mutation whose anchor stopped matching silently rewrites nothing, so the
    # must-fire case passes while proving nothing. A `just --fmt` reflow of the
    # standalone justfile did exactly that to the two body-injection cases.
    failures.extend(
        f"  Just bypass mutation is vacuous: {name}"
        for name, broken in just_mutations.items()
        if broken == APPROVED_JUSTFILE
    )
    failures.extend(
        f"  Just bypass was accepted: {name}"
        for name, broken in just_mutations.items()
        if not _check_justfile_text(broken)
    )
    return failures


def _selftest_documentation() -> list[str]:
    """Prove repair docs cannot route through the root graph or direct driver."""
    approved = "\n".join(SAFE_DOCUMENTED_COMMANDS)
    failures = []
    if _check_documentation_text(approved):
        failures.append("  approved standalone repair documentation was rejected")
    mutations = {
        "root module": approved.replace(
            SAFE_DOCUMENTED_COMMANDS[0], "jus" + "t infra::hil_cache_check"
        ),
        "inline Markdown": approved + "\nUse `just --justfile infra/hil-cache.just apply`.",
        "bulleted Markdown": approved + "\n- just --justfile infra/hil-cache.just apply",
        "environment-prefixed direct driver": approved
        + "\nenv -i bash scripts/dev/hil_cache_repair.sh apply",
        "direct driver": approved.replace(
            SAFE_DOCUMENTED_COMMANDS[0], "bash scripts/dev/hil_cache_repair.sh check"
        ),
        "extra argument": approved.replace(
            SAFE_DOCUMENTED_COMMANDS[0], f"{SAFE_DOCUMENTED_COMMANDS[0]} --limit star"
        ),
    }
    failures.extend(
        f"  unsafe documented repair front door was accepted: {name}"
        for name, broken in mutations.items()
        if not _check_documentation_text(broken)
    )
    return failures


def _selftest_entrypoint_ownership() -> list[str]:
    """Prove every alternate documentation, script and Just owner fires."""
    failures = []
    for name, relative, content in ENTRYPOINT_OWNERSHIP_SELFTEST_CASES:
        with TemporaryDirectory() as tmp, atc.isolated_git_environment():
            root = Path(tmp)
            path = root / relative
            atc.init_test_repo(root)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            if not _check_entrypoint_ownership(root):
                failures.append(f"  unowned repair entrypoint was accepted: {name}")
    return failures


def _selftest_exact_owner_views() -> list[str]:
    """Prove exact owners are checked independently in index and worktree views."""
    approved_driver = "\n".join(APPROVED_DRIVER_LINES) + "\n"
    unsafe_driver = approved_driver + "systemctl restart ra8-hil\n"
    approved_docs = "\n".join(SAFE_DOCUMENTED_COMMANDS) + "\n"
    unsafe_docs = "bash scripts/dev/hil_cache_repair.sh apply\n"
    unsafe_just = APPROVED_JUSTFILE + "\nrogue:\n    reboot\n"
    cases = (
        ("documentation", DOCUMENTATION, approved_docs, unsafe_docs),
        ("driver", DRIVER, approved_driver, unsafe_driver),
        ("Justfile", JUSTFILE, APPROVED_JUSTFILE, unsafe_just),
    )
    failures = []
    for name, relative, approved, unsafe in cases:
        for unsafe_view in ("index", "worktree"):
            with TemporaryDirectory() as tmp, atc.isolated_git_environment():
                root = Path(tmp)
                atc.init_test_repo(root)
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                index_data = unsafe if unsafe_view == "index" else approved
                path.write_text(index_data, encoding="utf-8")
                subprocess.run(  # noqa: S603 -- fixed throwaway fixture command
                    [trusted_git_executable(), "add", "--", relative],
                    cwd=root,
                    capture_output=True,
                    check=True,
                )
                worktree_data = unsafe if unsafe_view == "worktree" else approved
                path.write_text(worktree_data, encoding="utf-8")
                if not _check_entrypoint_ownership(root):
                    failures.append(f"  unsafe {name} {unsafe_view} view was accepted")
    return failures


def _selftest_policy_inputs() -> list[str]:
    """Prove Git-authored scope, encoding, alias and I/O behavior."""
    return atc.selftest(AUTHORED_EXCLUDED_PARTS)


def _selftest_checker_occurrences() -> list[str]:
    """Prove generic-checker ownership accepts only its exact references."""
    failures = []
    for relative, expected in PINNED_CHECKER_OCCURRENCES.items():
        approved = "\n".join(expected)
        if _check_checker_occurrences(approved, relative):
            failures.append(f"  approved {relative} repair references were rejected")
        widened = approved + '\nsubprocess.run(["bash", "hil_cache_repair.sh", "apply"])'
        if not _check_checker_occurrences(widened, relative):
            failures.append(f"  executable repair reference in {relative} was accepted")
    return failures


def _selftest_gate_wiring() -> list[str]:
    """Prove every census change reaches both unconditional repair gates."""
    hook = "pre-commit:\n    gates=(\n        pre-commit-checks\n    )\n"
    gate = (
        "_pcc_repository_structure() (\n"
        "  python3 scripts/checks/check_fleet_declaration.py --selftest\n"
        "  python3 scripts/checks/check_fleet_declaration.py\n"
        ")\n"
    )
    failures = []
    if _check_gate_wiring_texts(hook, gate):
        failures.append("  approved unconditional repair-gate wiring was rejected")
    if not _check_gate_wiring_texts(hook.replace("pre-commit-checks", "lint-just"), gate):
        failures.append("  pre-commit repair-gate removal was accepted")
    if not _check_gate_wiring_texts(hook, gate.replace(" --selftest", "")):
        failures.append("  full-gate repair selftest removal was accepted")
    if not _check_gate_wiring_texts(hook, gate.replace("declaration.py\n", "other.py\n")):
        failures.append("  full-gate live repair guard removal was accepted")
    return failures


def _selftest_just_isolation() -> list[str]:
    """Prove an explicit standalone invocation never parses root or modules."""
    just = shutil.which("just")
    if just is None:
        return ["  cannot prove explicit Justfile isolation without just"]
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        standalone = root / JUSTFILE
        module = root / "just" / "hostile.just"
        standalone.parent.mkdir(parents=True)
        module.parent.mkdir(parents=True)
        standalone.write_text(APPROVED_JUSTFILE, encoding="utf-8")
        (root / "justfile").write_text(
            "export PATH := `touch root-assignment-fired; printf /usr/bin`\n"
            'mod hostile "just/hostile.just"\n'
            "this is invalid Just syntax\n"
            "check:\n"
            "    @true\n",
            encoding="utf-8",
        )
        module.write_text(
            "export PYTHONPATH := `touch module-assignment-fired; printf /tmp`\n"
            "this is invalid Just syntax\n"
            "module-check:\n"
            "    @true\n",
            encoding="utf-8",
        )
        markers = (root / "root-assignment-fired", root / "module-assignment-fired")
        hostile_files = ((root / "justfile", "check"), (module, "module-check"))
        for hostile_file, recipe in hostile_files:
            hostile_result = subprocess.run(  # noqa: S603 -- pinned throwaway fixture
                [just, "--justfile", hostile_file, "--dry-run", recipe],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )
            if not hostile_result.returncode:
                return ["  hostile root/module fixture did not fail when parsed directly"]
        for marker in markers:
            marker.unlink(missing_ok=True)

        result = subprocess.run(  # noqa: S603 -- pinned tool and throwaway Justfile
            [just, "--justfile", standalone, "--dry-run", "check"],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            return ["  explicit standalone Justfile dry-run failed"]
        if any(marker.exists() for marker in markers):
            return ["  explicit standalone invocation parsed a hostile root or module"]
    return []


def _selftest_inventory() -> list[str]:
    """Prove inventory scopes cannot override either safety-owned value."""
    failures = []
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        if _check_inventory_overrides(root):
            failures.append("  an empty inventory-variable scope was rejected")
        for relative in (
            "infra/ansible/inventory/host_vars/dev.yml",
            "infra/ansible/group_vars/all.yaml",
            "infra/ansible/inventory/host_vars/dev.json",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.suffix == ".json":
                content = json.dumps(
                    {
                        "dev_box_ccache_dir": "/",
                        "dev_box_hil_ccache_dir": "/",
                        "dev_box_hil_runner_user": "root",
                    }
                )
            else:
                content = (
                    "dev_box_ccache_dir: /\n"
                    "dev_box_hil_ccache_dir: /\n"
                    "dev_box_hil_runner_user: root\n"
                )
            path.write_text(content, encoding="utf-8")
        if len(_check_inventory_overrides(root)) != SELFTEST_OVERRIDE_COUNT:
            failures.append("  host_vars/group_vars safety overrides were accepted")
    return failures


def _selftest_files(repo_root: Path) -> list[str]:
    """Prove driver, recipe and inventory override checks fire both ways."""
    return (
        _selftest_driver(repo_root)
        + _selftest_just_content()
        + _selftest_documentation()
        + _selftest_entrypoint_ownership()
        + _selftest_exact_owner_views()
        + _selftest_policy_inputs()
        + _selftest_checker_occurrences()
        + _selftest_gate_wiring()
        + _selftest_just_isolation()
        + _selftest_inventory()
        + hci.selftest()
    )


def selftest(repo_root: Path) -> list[str]:
    """Prove the standalone execution contract has both acceptance directions."""
    failures = _selftest_playbook(repo_root) + _selftest_files(repo_root)
    approved_defaults = {
        "dev_box_ccache_dir": SHARED_CACHE_ROOT,
        "dev_box_hil_ccache_dir": HIL_CACHE_ROOT,
        "dev_box_hil_ccache_max_size": "10G",
        "dev_box_hil_runner_user": RUNNER_USER,
    }
    if _check_defaults(approved_defaults):
        failures.append("  aligned full-role defaults were rejected")
    unsafe_defaults = dict(approved_defaults)
    unsafe_defaults.update({"dev_box_hil_ccache_dir": "/", "dev_box_hil_runner_user": "root"})
    if not _check_defaults(unsafe_defaults):
        failures.append("  unsafe full-role defaults were accepted")
    good_connect = {"address": "192.0.2.10", "user": "dev-user"}
    expected_digest = _connect_digest(good_connect)
    good_fleet = {
        "hosts": {
            "dev": {
                "class": "dev_box",
                "provisions": ["dev-box"],
                "hil_runner": {},
                "connect": good_connect,
            },
            "star": {"connect": {"address": "192.0.2.20", "user": "bench-user"}},
        }
    }
    if _check_dispatch(good_fleet, expected_digest):
        failures.append("  fixed synthetic dev transport was rejected")
    repointed = deepcopy(good_fleet)
    repointed["hosts"]["dev"]["connect"] = deepcopy(repointed["hosts"]["star"]["connect"])
    jumped = deepcopy(good_fleet)
    jumped["hosts"]["dev"]["connect"]["jump"] = "star"
    if not _check_dispatch(repointed, expected_digest):
        failures.append("  dev repointed to the bench transport was accepted")
    if not _check_dispatch(jumped, expected_digest):
        failures.append("  dev ProxyJump through the bench was accepted")
    if not _check_dispatch({"hosts": {}}, expected_digest):
        failures.append("  missing fixed dev dispatcher target was accepted")
    return failures

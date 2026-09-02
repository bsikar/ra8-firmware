#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Require shell and GUI entry points to use the repository-owned Just launcher.

Just recipes export the exact invoking executable through ``RA8_JUST``. Shell
scripts then enter Just through ``scripts/dev/run_just.sh``, which preserves
that executable even when a noninteractive PATH cannot find it. Direct calls
inside a devcontainer command are a different namespace: the image owns its
PATH and its pinned Just, so those argv tails remain bare by design.

VS Code tasks, the IDE runbook, and the project MCP configuration are included
because GUI processes commonly inherit a reduced PATH and must not bypass Just
with raw script entry points. Workflow YAML remains outside this check: those
calls are process entry points whose jobs provision Just before use. The sibling
``check_justfiles.py`` owns recipe bodies and the devcontainer boundary.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shell_entrypoint_policy import PRIVILEGED_PATHS, SHELL_POLICIES
from shell_invocation_policy import (
    scan_caller_text,
)
from shell_invocation_policy import (
    selftest_failures as structural_caller_selftest_failures,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
SELF = "scripts/checks/check_shell_just_invocations.py"
VSCODE_TASKS = ".vscode/tasks.json"
IDE_DOCUMENT = "docs/IDE.md"
MCP_CONFIG = ".mcp.json"
ROOT_JUSTFILE = "justfile"
WORKSPACE_JUSTFILE = "just/ws.just"
SENSITIVE_BOUNDARY_LINES = (
    (
        "just/ci_gate.just",
        (('["/bin/bash", "-p", "scripts/ci.sh", "--list-gates"],', 1),),
    ),
    (
        "scripts/ci/check_ci_parity.py",
        (('["/bin/bash", "-p", str(CI_SH), "--list-gates"],', 1),),
    ),
    (
        "just/hw.just",
        (
            ("#!/bin/bash -p", 4),
            (
                "# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.",
                4,
            ),
            ('/bin/bash -p scripts/dev/openocd_flash.sh "$hex"', 1),
            ('/bin/bash -p scripts/dev/flash.sh "$hex"', 1),
            ('/bin/bash -p scripts/dev/openocd_debug.sh "$elf"', 1),
            ('/bin/bash -p scripts/dev/debug.sh "$elf"', 1),
            ('/bin/bash -p scripts/dev/ozone.sh "$elf"', 1),
            ("/bin/bash -p scripts/dev/monitor.sh", 1),
        ),
    ),
    ("just/emu.just", (("/bin/bash -p scripts/emu/setup_macos.sh", 1),)),
    (
        "scripts/emu/setup_macos.sh",
        (
            ('/bin/bash -p "${installer}"', 1),
            (
                'RA8_UNICORN_PREFIX="$prefix" /bin/bash -p "$root/scripts/ci/install_unicorn.sh"',
                1,
            ),
        ),
    ),
    (
        "infra/network/verify_bench_wifi.sh",
        (
            (
                'setsid /bin/bash -p -c "sleep ${RESTORE_AFTER}; ${RESTORE_CMD}" '
                ">/dev/null 2>&1 </dev/null &",
                1,
            ),
        ),
    ),
    (
        "scripts/builders/docs.sh",
        (('DOXYGEN_BIN="$(/bin/bash -p "${SCRIPT_DIR}/provision_doxygen.sh")"', 1),),
    ),
    (
        "scripts/ci/gates/checks.sh",
        (("/bin/bash -p scripts/ci/devcontainer_image.sh --selftest", 1),),
    ),
    (
        "scripts/ci/gates/hygiene.sh",
        (
            ("/bin/bash -p scripts/dev/setup_python.sh --selftest", 1),
            ("/bin/bash -p scripts/ci/monitor.sh selftest", 1),
        ),
    ),
    (
        "scripts/ci/lib/container.sh",
        (('/bin/bash -p "$repo/scripts/ci/devcontainer_image.sh" "${args[@]}"', 1),),
    ),
    (
        "scripts/dev/remote_gdb_server.sh",
        (
            ('/bin/bash -p "$ROOT/scripts/hil/flash.sh" "$APP_ID"', 1),
            ('REMOTE_START_COMMAND="$(', 1),
            ('"$PYTHON" -I "$ARGS_GUARD" remote-command \\', 1),
            ('-- "$PI_HOST" "$REMOTE_START_COMMAND" <"$REMOTE_GUARD" &', 1),
            ('REMOTE_CLEANUP_COMMAND="$(', 0),
            ('"$PYTHON" -I "$ARGS_GUARD" remote-command cleanup \\', 0),
            ('"$PYTHON" -I "$ARGS_GUARD" remote-command start \\', 0),
            ('/usr/bin/ssh -o BatchMode=yes -o ConnectTimeout=5 -- "$PI_HOST" \\', 0),
            ("\"$REMOTE_CLEANUP_COMMAND\" <<'REMOTE_CLEANUP'", 0),
            ('-- "$PI_HOST" "$REMOTE_START_COMMAND" <<\'REMOTE\' &', 0),
        ),
    ),
    (
        "scripts/dev/remote_gdb_args.py",
        (
            (
                'return shlex.join(["/usr/bin/python3", "-I", "-", "--", *fields])',
                1,
            ),
            ('return shlex.join(["/bin/bash", "-p", "-s", "--", *fields])', 0),
        ),
    ),
    (
        "just/docs.just",
        (("/bin/bash -p scripts/builders/publish_docs.sh", 1),),
    ),
)
MIN_LAUNCHER_ARGV = 3
EXCLUDED_PREFIXES = (
    "docs/sbom/upstream/",
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "port/netxduo/",
    "port/nimble/",
    "port/threadx/",
    "port/usbx/",
    "tests/fixtures/",
)
MIN_SHELL_FILES = 140
MIN_PROTECTED_SCRIPTS = 65
MIN_CALLER_FILES = 650
PROTECTED_SHEBANG = "#!/bin/bash -p"
PROTECTED_REASON = "# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection."
FUTURE_SCRIPT_FIXTURE = "scripts/secrets/future_ceremony.sh"  # PATHREF-OK: future-path test fixture

COMMAND_PREFIX = (
    r"(?:^\s*|(?:&&|\|\||;|\|)\s*|\$\(\s*)"
    r"(?:if\s+|elif\s+|while\s+|until\s+)?!?(?:\s*)"
    r"(?:(?:[A-Za-z_][A-Za-z0-9_]*=[^\s;&|]+)\s+)*"
    r"(?:(?:exec|time)\s+)?"
)
BARE_JUST_RE = re.compile(COMMAND_PREFIX + r"just(?=\s|$)")
ARRAY_JUST_RE = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*=\(\s*(?:[\"'])?just(?:[\"'])?(?=\s|\))")
SHELL_COMMAND_RE = re.compile(
    r"\b(?:ba|z|da)?sh\s+(?:-[A-Za-z]*c|--command)\s+([\"'])\s*just(?=\s)"
)
COMMAND_SUB_JUST_RE = re.compile(r"\$\(\s*just(?=\s|$)")
HEREDOC_RE = re.compile(r"<<-?\s*([\"']?)([A-Za-z_][A-Za-z0-9_]*)\1")
SCRIPT_PATH_RE = re.compile(
    r"(?:\$\{workspaceFolder\}/)?(?P<path>scripts/[A-Za-z0-9_./-]+\.(?:sh|py))"
)
UNPRIVILEGED_RUN_JUST_RE = re.compile(
    r"(?<![A-Za-z0-9_/])bash\s+(?:\"[^\"]*\"|'[^']*'|[^\s;|&]*)scripts/dev/run_just\.sh"
)


def _is_shell(path: Path, text: str) -> bool:
    """Return whether ``path`` is a first-party shell entry point."""
    if path.suffix in {".sh", ".bash", ".zsh"}:
        return True
    first = text.partition("\n")[0]
    return first.startswith("#!") and re.search(r"\b(?:ba|z|da)?sh\b", first) is not None


def scoped_files() -> list[str]:
    """Return tracked and untracked first-party shell entry points."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable and fixed arguments
        [git_bin, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    rels: list[str] = []
    for rel in proc.stdout.decode("utf-8", errors="strict").split("\0"):
        if not rel or rel.startswith(EXCLUDED_PREFIXES):
            continue
        path = REPO_ROOT / rel
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if _is_shell(path, text):
            rels.append(rel)
    return sorted(set(rels))


def bare_just_line(line: str) -> bool:
    """Return whether one active shell line launches Just through PATH."""
    stripped = line.lstrip()
    if not stripped or stripped.startswith("#"):
        return False
    if "devcontainer_run.sh" in line and re.search(r"--\s+.*\bjust\b", line) is not None:
        return False
    masked = _mask_quotes(line)
    return any(
        pattern.search(candidate) is not None
        for pattern, candidate in (
            (BARE_JUST_RE, masked),
            (ARRAY_JUST_RE, line),
            (SHELL_COMMAND_RE, line),
            (COMMAND_SUB_JUST_RE, _mask_single_quotes(line)),
        )
    )


def _mask_quotes(line: str) -> str:
    """Mask quoted prose while retaining token width for shell assignments."""
    masked: list[str] = []
    quote = ""
    escaped = False
    for char in line:
        if quote:
            masked.append("_")
            if escaped:
                escaped = False
            elif char == "\\" and quote == '"':
                escaped = True
            elif char == quote:
                quote = ""
        elif char in "'\"":
            quote = char
            masked.append("_")
        else:
            masked.append(char)
    return "".join(masked)


def _mask_single_quotes(line: str) -> str:
    """Mask single-quoted literals, where command substitution is inactive."""
    masked: list[str] = []
    in_single = False
    for char in line:
        if char == "'":
            in_single = not in_single
            masked.append("_")
        elif in_single:
            masked.append("_")
        else:
            masked.append(char)
    return "".join(masked)


def scan_text(text: str) -> list[int]:
    """Return line numbers containing bare Just commands outside heredocs."""
    findings: list[int] = []
    heredoc_end: str | None = None
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        number = index + 1
        line = lines[index]
        index += 1
        if heredoc_end is not None:
            if line.strip() == heredoc_end:
                heredoc_end = None
            continue
        logical = line
        while logical.rstrip().endswith("\\") and index < len(lines):
            logical = logical.rstrip()[:-1] + " " + lines[index].lstrip()
            index += 1
        if bare_just_line(logical) or UNPRIVILEGED_RUN_JUST_RE.search(logical):
            findings.append(number)
        match = HEREDOC_RE.search(logical)
        if match is not None:
            heredoc_end = match.group(2)
    return findings


def scan(rels: list[str]) -> list[str]:
    """Return every shell, editor, and MCP entry-point bypass finding."""
    findings: list[str] = []
    for rel in rels:
        text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        findings.extend(f"{rel}:{line}" for line in scan_text(text))

    findings.extend(_fixed_surface_findings())
    protected = protected_script_paths(rels)
    if len(protected) < MIN_PROTECTED_SCRIPTS:
        findings.append(
            f"protected shell population collapsed to {len(protected)}; "
            f"expected at least {MIN_PROTECTED_SCRIPTS}"
        )
    if protected != PRIVILEGED_PATHS:
        findings.extend(
            f"typed/header protected population drift: {path}"
            for path in sorted(protected ^ PRIVILEGED_PATHS)
        )
    findings.extend(scan_structural_callers(rels))
    findings.extend(scan_sensitive_boundary_files())
    return findings


def _fixed_surface_findings() -> list[str]:
    """Validate required VS Code, IDE, MCP, and fixed Just surfaces."""
    findings: list[str] = []
    tasks_path = REPO_ROOT / VSCODE_TASKS
    if not tasks_path.is_file():
        findings.append(f"{VSCODE_TASKS}: required GUI task configuration is missing")
    else:
        task_findings = scan_vscode_tasks(tasks_path.read_text(encoding="utf-8"))
        findings.extend(f"{VSCODE_TASKS}: {finding}" for finding in task_findings)

    ide_path = REPO_ROOT / IDE_DOCUMENT
    if not ide_path.is_file():
        findings.append(f"{IDE_DOCUMENT}: required IDE runbook is missing")
    else:
        findings.extend(
            f"{IDE_DOCUMENT}:{line}: launches a raw script instead of run_just.sh"
            for line in scan_ide_document(ide_path.read_text(encoding="utf-8"))
        )

    mcp_path = REPO_ROOT / MCP_CONFIG
    if not mcp_path.is_file():
        findings.append(f"{MCP_CONFIG}: required project MCP configuration is missing")
    else:
        mcp_findings = scan_mcp_config(mcp_path.read_text(encoding="utf-8"))
        findings.extend(f"{MCP_CONFIG}: {finding}" for finding in mcp_findings)
    root_just = REPO_ROOT / ROOT_JUSTFILE
    if not root_just.is_file():
        findings.append(f"{ROOT_JUSTFILE}: required root Justfile is missing")
    else:
        root_findings = scan_root_justfile(root_just.read_text(encoding="utf-8"))
        findings.extend(f"{ROOT_JUSTFILE}: {finding}" for finding in root_findings)
    workspace_just = REPO_ROOT / WORKSPACE_JUSTFILE
    if not workspace_just.is_file():
        findings.append(f"{WORKSPACE_JUSTFILE}: required workspace Justfile is missing")
    else:
        workspace_findings = scan_workspace_justfile(workspace_just.read_text(encoding="utf-8"))
        findings.extend(f"{WORKSPACE_JUSTFILE}: {finding}" for finding in workspace_findings)
    return findings


def is_protected_script_text(text: str) -> bool:
    """Return whether a shell entry owns the exact reviewed startup boundary."""
    return text.splitlines()[:4] == [
        PROTECTED_SHEBANG,
        "# SPDX-License-Identifier: MIT",
        "# Copyright (c) 2026 Brighton Sikarskie",
        PROTECTED_REASON,
    ]


def protected_script_paths(rels: list[str]) -> set[str]:
    """Derive the protected script population from exact reviewed file headers."""
    protected: set[str] = set()
    for rel in rels:
        path = REPO_ROOT / rel
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            continue
        if is_protected_script_text(text):
            protected.add(rel)
    return protected


def caller_files(shell_rels: list[str]) -> list[str]:
    """Return all first-party executable and configuration caller surfaces."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable and fixed arguments
        [git_bin, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    callers = set(shell_rels)
    for rel in proc.stdout.decode("utf-8", errors="strict").split("\0"):
        if not rel or rel.startswith(EXCLUDED_PREFIXES):
            continue
        path = Path(rel)
        if (
            rel == ".env.example"
            or path.name == "justfile"
            or path.name.endswith("Dockerfile")
            or path.suffix
            in {
                ".json",
                ".just",
                ".md",
                ".py",
                ".yaml",
                ".yml",
            }
        ):
            callers.add(rel)
    return sorted(callers)


def scan_structural_callers(shell_rels: list[str]) -> list[str]:
    """Validate all supported first-party caller formats structurally."""
    findings: list[str] = []
    callers = caller_files(shell_rels)
    if len(callers) < MIN_CALLER_FILES:
        findings.append(
            f"structural caller scope collapsed to {len(callers)}; "
            f"expected at least {MIN_CALLER_FILES}"
        )
    for rel in callers:
        path = REPO_ROOT / rel
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            continue
        findings.extend(
            f"{rel}:{finding.line}: {finding.message}"
            for finding in scan_caller_text(rel, text, SHELL_POLICIES)
        )
    return findings


def scan_root_justfile(text: str) -> list[str]:
    """Require fixed Bash at setup and writable-container boundaries."""
    required = (
        "/bin/bash -p scripts/ci/devcontainer_image.sh ensure",
        "/bin/bash -p scripts/dev/setup_python.sh setup",
        "/bin/bash -p scripts/dev/setup_ansible.sh",
        "/bin/bash -p scripts/ci/devcontainer_run.sh -- /bin/bash -p",
        "/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "
        "/bin/bash -p -c 'if [[ -x /usr/bin/nproc ]]",
        "$(/usr/bin/git config core.hooksPath)",
    )
    return [
        f"root recipe lost exact privileged entry: {line}" for line in required if line not in text
    ]


def scan_workspace_justfile(text: str) -> list[str]:
    """Require fixed Bash for workspace lifecycle and monitor boundaries."""
    required = (
        "/bin/bash -p scripts/dev/agent_workspace.sh create",
        "/bin/bash -p scripts/dev/agent_workspace.sh release",
        "/bin/bash -p scripts/dev/agent_workspace.sh list",
        "/bin/bash -p scripts/dev/agent_workspace.sh doctor",
        "/bin/bash -p scripts/dev/agent_workspace.sh reap",
        "/bin/bash -p scripts/ci/monitor.sh status",
        "/bin/bash -p scripts/ci/monitor.sh quota",
    )
    findings = [
        f"workspace recipe lost exact privileged entry: {line}"
        for line in required
        if line not in text
    ]
    multiline_recipes = ("new", "reap", "status")
    if text.count("#!/bin/bash -p") != len(multiline_recipes):
        findings.append("workspace multiline recipes must own exactly three privileged shebangs")
    return findings


def scan_sensitive_boundary_text(rel: str, text: str) -> list[str]:
    """Require exact protected argv at hardware, installer, and nested boundaries."""
    contract = next(
        (required for path, required in SENSITIVE_BOUNDARY_LINES if path == rel),
        None,
    )
    if contract is None:
        return []
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return [
        f"{rel}: expected {count} exact occurrence(s) of {line!r}, found {lines.count(line)}"
        for line, count in contract
        if lines.count(line) != count
    ]


def scan_sensitive_boundary_files() -> list[str]:
    """Scan every fixed sensitive caller, failing closed when one disappears."""
    findings: list[str] = []
    for rel, _required in SENSITIVE_BOUNDARY_LINES:
        path = REPO_ROOT / rel
        if not path.is_file():
            findings.append(f"{rel}: required sensitive boundary is missing")
            continue
        findings.extend(scan_sensitive_boundary_text(rel, path.read_text(encoding="utf-8")))
    return findings


def _direct_just_command(command: object) -> bool:
    """Return whether a VS Code task command directly selects Just."""
    if not isinstance(command, str):
        return False
    first = command.strip().split(maxsplit=1)[0] if command.strip() else ""
    return first == "just" or first.endswith("/just")


def _raw_script_paths(values: list[object]) -> list[str]:
    """Return first-party script paths other than the canonical launcher."""
    paths: list[str] = []
    for value in values:
        if not isinstance(value, str):
            continue
        for match in SCRIPT_PATH_RE.finditer(value):
            path = match.group("path")
            if path != "scripts/dev/run_just.sh":
                paths.append(path)
    return paths


def scan_vscode_tasks(text: str) -> list[str]:
    """Return labels of VS Code tasks that bypass ``run_just.sh``."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        return [f"invalid JSON ({exc.msg})"]
    if not isinstance(document, dict) or not isinstance(document.get("tasks"), list):
        return ["top-level tasks array is missing"]
    findings: list[str] = []
    for index, task in enumerate(document["tasks"], start=1):
        if not isinstance(task, dict):
            findings.append(f"task {index} is not an object")
            continue
        label = task.get("label", f"task {index}")
        if _direct_just_command(task.get("command")):
            findings.append(f"task {label!r} launches Just directly")
            continue
        args = task.get("args", [])
        values = [task.get("command"), *(args if isinstance(args, list) else [])]
        paths = _raw_script_paths(values)
        if paths:
            findings.append(f"task {label!r} launches raw script {paths[0]!r}")
        elif any(_is_just_launcher(value) for value in values) and not _valid_vscode_launcher(
            task.get("command"), args
        ):
            findings.append(f"task {label!r} misplaces scripts/dev/run_just.sh")
    return findings


def scan_ide_document(text: str) -> list[int]:
    """Return IDE runbook lines that invoke first-party scripts directly."""
    return [
        number
        for number, line in enumerate(text.splitlines(), start=1)
        if _raw_script_paths([line])
    ]


def _is_just_launcher(value: object) -> bool:
    """Return whether one argv value names the repository Just launcher."""
    return isinstance(value, str) and value.replace("\\", "/").endswith("scripts/dev/run_just.sh")


def _valid_vscode_launcher(command: object, args: object) -> bool:
    """Require the launcher in executable position, followed by a recipe."""
    if not isinstance(args, list) or not args:
        return False
    return (
        command == "/bin/bash"
        and len(args) >= MIN_LAUNCHER_ARGV
        and args[0] == "-p"
        and _is_just_launcher(args[1])
        and isinstance(args[2], str)
        and bool(args[2])
    )


def scan_mcp_config(text: str) -> list[str]:
    """Return project MCP servers that do not enter through the MCP Just recipe."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        return [f"invalid JSON ({exc.msg})"]
    servers = document.get("mcpServers") if isinstance(document, dict) else None
    if not isinstance(servers, dict) or not servers:
        return ["top-level mcpServers object is missing or empty"]
    findings: list[str] = []
    for name, server in servers.items():
        if not isinstance(server, dict):
            findings.append(f"server {name!r} is not an object")
            continue
        args = server.get("args")
        if server.get("command") != "/bin/bash" or args != [
            "-p",
            "scripts/dev/run_just.sh",
            "tools::mcp_server",
        ]:
            findings.append(
                f"server {name!r} must use exact /bin/bash -p, run_just.sh, MCP recipe argv"
            )
    return findings


def _shell_selftest_failures() -> tuple[list[str], int]:
    """Exercise shell parsing in both directions."""
    cases = (
        ("just ci\n", [1], "direct command fires"),
        ("if just apps::build blink; then :; fi\n", [1], "conditional command fires"),
        ("CC=clang just tools::build\n", [1], "environment-prefixed command fires"),
        ("false || just hil::probe\n", [1], "chained command fires"),
        ("cmd=(just quality::run)\n", [1], "deferred command array fires"),
        ("bash -uc 'just tests::build'\n", [1], "nested shell command fires"),
        ('bash "$root/scripts/dev/run_just.sh" ci\n', [1], "unprivileged launcher fires"),
        (
            '/bin/bash -p "$root/scripts/dev/run_just.sh" ci\n',
            [],
            "privileged launcher stays quiet",
        ),
        (
            "bash scripts/ci/devcontainer_run.sh -- just quality::local::test\n",
            [],
            "container-owned command stays quiet",
        ),
        (
            "exec bash scripts/ci/devcontainer_run.sh -- \\\n  just quality::local::test\n",
            [],
            "continued container-owned command stays quiet",
        ),
        ('version="$(just --version)"\n', [1], "unresolved version probe fires"),
        ('echo "run just ci"\n', [], "help prose stays quiet"),
        ("cat <<'HELP'\ncd repo && just ci\nHELP\n", [], "heredoc guidance stays quiet"),
    )
    return [label for text, expected, label in cases if scan_text(text) != expected], len(cases)


def _task_selftest_failures() -> tuple[list[str], int]:
    """Exercise VS Code entry-point parsing in both directions."""
    task_cases = (
        ('{"tasks":[{"label":"bad","command":"just"}]}', 1, "bare GUI command fires"),
        (
            '{"tasks":[{"label":"bad","command":"/usr/local/bin/just"}]}',
            1,
            "absolute GUI command fires",
        ),
        (
            '{"tasks":[{"label":"safe","command":"/bin/bash",'
            '"args":["-p","scripts/dev/run_just.sh","ci"]}]}',
            0,
            "GUI launcher call stays quiet",
        ),
        (
            '{"tasks":[{"label":"bad","command":"/bin/bash",'
            '"args":["scripts/dev/run_just.sh","ci"]}]}',
            1,
            "GUI launcher without privileged mode fires",
        ),
        (
            '{"tasks":[{"label":"bad","command":"scripts/dev/run_just.sh","args":["ci"]}]}',
            1,
            "direct GUI launcher fires",
        ),
        (
            '{"tasks":[{"label":"bad","command":"bash","args":["scripts/hil/flash.sh"]}]}',
            1,
            "raw GUI script fires",
        ),
        (
            f'{{"tasks":[{{"label":"bad","command":"bash","args":["{FUTURE_SCRIPT_FIXTURE}"]}}]}}',
            1,
            "future raw GUI script fires",
        ),
        (
            '{"tasks":[{"label":"bad","command":"/bin/echo",'
            '"args":["scripts/dev/run_just.sh","ci"]}]}',
            1,
            "misplaced GUI launcher fires",
        ),
    )
    failures = [
        label for text, expected, label in task_cases if len(scan_vscode_tasks(text)) != expected
    ]
    return failures, len(task_cases)


def _ide_selftest_failures() -> tuple[list[str], int]:
    """Exercise IDE runbook entry-point parsing in both directions."""
    ide_cases = (
        ("GDB args: scripts/dev/remote_gdb_server.sh run\n", [1], "raw IDE script fires"),
        (
            "GDB args: scripts/dev/run_just.sh hil::remote_gdb run\n",
            [],
            "IDE launcher stays quiet",
        ),
        (
            f"GDB args: {FUTURE_SCRIPT_FIXTURE} run\n",
            [1],
            "future raw IDE script fires",
        ),
    )
    failures = [label for text, expected, label in ide_cases if scan_ide_document(text) != expected]
    return failures, len(ide_cases)


def _mcp_selftest_failures() -> tuple[list[str], int]:
    """Exercise exact MCP executable and argv parsing in both directions."""
    mcp_cases = (
        (
            '{"mcpServers":{"bad":{"command":"python3","args":["server.py"]}}}',
            1,
            "raw MCP command fires",
        ),
        (
            f'{{"mcpServers":{{"bad":{{"command":"bash","args":["{FUTURE_SCRIPT_FIXTURE}"]}}}}',
            1,
            "future raw MCP script fires",
        ),
        (
            '{"mcpServers":{"safe":{"command":"/bin/bash",'
            '"args":["-p","scripts/dev/run_just.sh","tools::mcp_server"]}}}',
            0,
            "MCP launcher stays quiet",
        ),
        (
            '{"mcpServers":{"bad":{"command":"/bin/bash",'
            '"args":["scripts/dev/run_just.sh","tools::mcp_server"]}}}',
            1,
            "MCP launcher without privileged mode fires",
        ),
        (
            '{"mcpServers":{"bad":{"command":"/bin/bash",'
            '"args":["scripts/dev/run_just.sh","tools::mcp"]}}}',
            1,
            "wrong MCP recipe fires",
        ),
        (
            '{"mcpServers":{"bad":{"command":"/bin/echo",'
            '"args":["scripts/dev/run_just.sh","tools::mcp_server"]}}}',
            1,
            "misplaced MCP launcher fires",
        ),
        (
            '{"mcpServers":{"bad":{"command":"/bin/bash",'
            '"args":["tools::mcp_server","scripts/dev/run_just.sh"]}}}',
            1,
            "reordered MCP argv fires",
        ),
    )
    failures = [
        label for text, expected, label in mcp_cases if len(scan_mcp_config(text)) != expected
    ]
    return failures, len(mcp_cases)


def _root_just_selftest_failures() -> tuple[list[str], int]:
    """Exercise fixed root-recipe Bash ownership in both directions."""
    safe = (
        "/bin/bash -p scripts/ci/devcontainer_image.sh ensure\n"
        "/bin/bash -p scripts/dev/setup_python.sh setup\n"
        "/bin/bash -p scripts/dev/setup_ansible.sh\n"
        "/bin/bash -p scripts/ci/devcontainer_run.sh -- /bin/bash -p\n"
        "/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "
        "/bin/bash -p -c 'if [[ -x /usr/bin/nproc ]]\n"
        "$(/usr/bin/git config core.hooksPath)\n"
    )
    cases = (
        (safe, 0, "privileged root recipes stay quiet"),
        (safe.replace("/bin/bash -p", "bash", 1), 1, "PATH Bash in root recipe fires"),
        (safe.replace(" -- /bin/bash -p", " -- bash"), 1, "PATH inner shell fires"),
        (
            safe.replace("/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin", "env"),
            1,
            "caller environment in CPU probe fires",
        ),
        (
            safe.replace("/usr/bin/git config", "git config"),
            1,
            "PATH Git in hook status fires",
        ),
    )
    failures = [
        label for text, expected, label in cases if len(scan_root_justfile(text)) != expected
    ]
    return failures, len(cases)


def _workspace_just_selftest_failures() -> tuple[list[str], int]:
    """Exercise exact workspace lifecycle and monitor entry points."""
    safe = (
        "#!/bin/bash -p\n#!/bin/bash -p\n#!/bin/bash -p\n"
        "/bin/bash -p scripts/dev/agent_workspace.sh create\n"
        "/bin/bash -p scripts/dev/agent_workspace.sh release\n"
        "/bin/bash -p scripts/dev/agent_workspace.sh list\n"
        "/bin/bash -p scripts/dev/agent_workspace.sh doctor\n"
        "/bin/bash -p scripts/dev/agent_workspace.sh reap\n"
        "/bin/bash -p scripts/ci/monitor.sh status\n"
        "/bin/bash -p scripts/ci/monitor.sh quota\n"
    )
    cases = (
        (safe, 0, "privileged workspace recipes stay quiet"),
        (
            safe.replace("/bin/bash -p scripts/ci/monitor.sh", "bash scripts/ci/monitor.sh", 1),
            1,
            "PATH Bash monitor entry fires",
        ),
        (
            safe.replace(
                "/bin/bash -p scripts/dev/agent_workspace.sh",
                "bash scripts/dev/agent_workspace.sh",
                1,
            ),
            1,
            "PATH Bash lifecycle entry fires",
        ),
        (safe.replace("#!/bin/bash -p", "#!/usr/bin/env bash", 1), 1, "PATH shebang fires"),
    )
    failures = [
        label for text, expected, label in cases if len(scan_workspace_justfile(text)) != expected
    ]
    return failures, len(cases)


def _sensitive_boundary_selftest_failures() -> tuple[list[str], int]:
    """Exercise every exact nested boundary in both directions."""
    failures: list[str] = []
    count = 1
    for rel, requirements in SENSITIVE_BOUNDARY_LINES:
        safe_lines = [line for line, copies in requirements for _ in range(copies)]
        safe = "\n".join(safe_lines) + "\n"
        count += 3
        if scan_sensitive_boundary_text(rel, safe):
            failures.append(f"exact sensitive boundary {rel} was rejected")
        if "/bin/bash -p" in safe:
            weakened = safe.replace("/bin/bash -p", "bash", 1)
        elif '"/usr/bin/python3", "-I"' in safe:
            weakened = safe.replace('"/usr/bin/python3", "-I"', '"python3"', 1)
        else:
            weakened = safe.replace('"/bin/bash", "-p"', '"bash"', 1)
        if not scan_sensitive_boundary_text(rel, weakened):
            failures.append(f"weakened sensitive boundary {rel} was accepted")
        duplicated = safe + requirements[0][0] + "\n"
        if not scan_sensitive_boundary_text(rel, duplicated):
            failures.append(f"duplicated sensitive boundary {rel} was accepted")

    if scan_sensitive_boundary_text("scripts/emu/matrix.sh", "bash ordinary.sh\n"):
        failures.append("ordinary emulator wrapper was pulled into the sensitive registry")

    contracts = dict(SENSITIVE_BOUNDARY_LINES)
    server_rel = "scripts/dev/remote_gdb_server.sh"
    args_rel = "scripts/dev/remote_gdb_args.py"
    server_safe = (
        "\n".join(line for line, copies in contracts[server_rel] for _ in range(copies)) + "\n"
    )
    old_cleanup = server_safe + 'REMOTE_CLEANUP_COMMAND="$(\n'
    if not scan_sensitive_boundary_text(server_rel, old_cleanup):
        failures.append("obsolete remote PID-sweep authority was accepted")
    heredoc = server_safe.replace(
        '-- "$PI_HOST" "$REMOTE_START_COMMAND" <"$REMOTE_GUARD" &',
        '-- "$PI_HOST" "$REMOTE_START_COMMAND" <<\'REMOTE\' &',
    )
    if not scan_sensitive_boundary_text(server_rel, heredoc):
        failures.append("inline remote heredoc replaced the fixed supervisor payload")
    args_safe = (
        "\n".join(line for line, copies in contracts[args_rel] for _ in range(copies)) + "\n"
    )
    reordered = args_safe.replace(
        '["/usr/bin/python3", "-I", "-", "--", *fields]',
        '["/usr/bin/python3", "-I", "-", *fields, "--"]',
    )
    if not scan_sensitive_boundary_text(args_rel, reordered):
        failures.append("reordered remote supervisor argv was accepted")
    count += 3
    return failures, count


def _structural_caller_selftest_failures() -> tuple[list[str], int]:
    """Exercise the format-aware caller authority and header derivation."""
    failures, count = structural_caller_selftest_failures()
    headers = (
        (
            f"{PROTECTED_SHEBANG}\n"
            "# SPDX-License-Identifier: MIT\n"
            "# Copyright (c) 2026 Brighton Sikarskie\n"
            f"{PROTECTED_REASON}\n"
        ),
        (
            f"{PROTECTED_SHEBANG}\n{PROTECTED_REASON}\n"
            "# SPDX-License-Identifier: MIT\n"
            "# Copyright (c) 2026 Brighton Sikarskie\n"
        ),
        (f"{PROTECTED_SHEBANG}\n# SPDX-License-Identifier: MIT\n{PROTECTED_REASON}\n"),
        "#!/usr/bin/env bash\n# ordinary entry\n",
    )
    if [is_protected_script_text(text) for text in headers] != [True, False, False, False]:
        failures.append("protected script population derivation is not two-sided")
    return failures, count + len(headers)


def _entrypoint_selftest_failures() -> tuple[list[str], int]:
    """Combine VS Code, IDE, and MCP entry-point tests."""
    groups = (
        _task_selftest_failures(),
        _ide_selftest_failures(),
        _mcp_selftest_failures(),
        _root_just_selftest_failures(),
        _workspace_just_selftest_failures(),
        _sensitive_boundary_selftest_failures(),
        _structural_caller_selftest_failures(),
    )
    return [failure for failures, _count in groups for failure in failures], sum(
        count for _failures, count in groups
    )


def selftest() -> int:
    """Prove shell and GUI entry-point checks in both directions."""
    shell_failures, shell_count = _shell_selftest_failures()
    entry_failures, entry_count = _entrypoint_selftest_failures()
    failures = shell_failures + entry_failures
    for failure in failures:
        print(f"check_shell_just_invocations.py --selftest: FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"check_shell_just_invocations.py --selftest: PASS ({shell_count + entry_count} cases)")
    return 0


def main() -> int:
    """Run detector self-tests or scan the live first-party shell scope."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: check_shell_just_invocations.py [--selftest]", file=sys.stderr)
        return 2
    try:
        rels = scoped_files()
    except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
        print(f"cannot enumerate first-party shell files: {exc}", file=sys.stderr)
        return 2
    if len(rels) < MIN_SHELL_FILES or not (REPO_ROOT / SELF).is_file():
        print(
            f"shell scope collapsed to {len(rels)} files; expected at least "
            f"{MIN_SHELL_FILES} with checker {SELF}",
            file=sys.stderr,
        )
        return 2
    findings = scan(rels)
    if findings:
        print("shell or GUI entry point(s) bypass scripts/dev/run_just.sh:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"check_shell_just_invocations.py: clean ({len(rels)} shell files + GUI entry points)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

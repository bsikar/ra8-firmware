#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the exhaustive typed first-party shell entry-point authority.

NUL cleanup rejects forged input; paths/symlinks fail; same-UID writes are out of scope.
"""

from __future__ import annotations

import dataclasses
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import files_for
from privileged_startup_runtime_selftest import (
    PrivateRun,
    WrapperVariant,
    run_private,
    run_privileged_wrapper_runtime_cases,
)
from shell_entrypoint_policy import (
    PORTABLE_SH_SHEBANG,
    PORTABLE_SHEBANG,
    PRIVILEGED_PATHS,
    PRIVILEGED_REASON,
    PRIVILEGED_SHEBANG,
    SHELL_POLICIES,
    ShellDialect,
    ShellPolicy,
    ShellSecurity,
    ShellUsage,
    merge_policy_tables,
)
from shell_entrypoint_policy_ci import CI_POLICY_ROWS
from shell_entrypoint_policy_hil import HIL_POLICY_ROWS

REPO_ROOT = Path(__file__).resolve().parents[2]
EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2
EARLY_EXIT_STATUS = 43

PRIVILEGED_BODY_OPEN = 'if [[ "$-" == *p* ]]; then'
FAILED_CLEANUP_EXEC = "_ra8_startup_refuse 'could not enter sanitized process'"
PRIVILEGED_BODY_PREFIX = (
    PRIVILEGED_BODY_OPEN,
    "unset -v BASH_ENV ENV",
    "declare -a ra8_startup_env_unset=()",
    "_ra8_startup_refuse() {",
    "  printf 'error: privileged startup %s\\n' \"$1\" >&2",
    "  exit 1",
    "}",
    "ra8_startup_env_done_count=0",
    "while IFS= read -r -d '' ra8_startup_env_row; do",
    '  ra8_startup_env_name="${ra8_startup_env_row%%=*}"',
    '  case "$ra8_startup_env_name" in',
    "    RA8_STARTUP_ENV_DONE)",
    "      ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))",
    "      ;;",
    # Current Bash uses the %% suffix; patched Bash 3.2 can use ().
    "    BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u \"$ra8_startup_env_name\") ;;",
    "  esac",
    "done < <(",
    "  /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&",
    "    /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\\0'",
    ")",
    (
        "((ra8_startup_env_done_count == 1)) && "
        '[[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || '
        "_ra8_startup_refuse 'environment enumeration was incomplete'"
    ),
    "if ((${#ra8_startup_env_unset[@]})); then",
    "  [[ -z \"${RA8_STARTUP_ENV_SCRUBBED-}\" ]] || _ra8_startup_refuse 'scrub did not converge'",
    '  ra8_startup_reentry="$0"',
    "  [[ \"$ra8_startup_reentry\" == */* ]] || _ra8_startup_refuse 'requires a script path'",
    '  if [[ "$ra8_startup_reentry" != /* ]]; then',
    '    ra8_startup_reentry="$PWD/$ra8_startup_reentry"',
    "  fi",
    '  ra8_startup_check="$ra8_startup_reentry"',
    '  while [[ "$ra8_startup_check" != "/" ]]; do',
    "    [[ ! -L \"$ra8_startup_check\" ]] || _ra8_startup_refuse 'refuses a symlinked path'",
    '    ra8_startup_parent="${ra8_startup_check%/*}"',
    '    [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"',
    '    [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||',
    "      _ra8_startup_refuse 'cannot validate its script path'",
    '    ra8_startup_check="$ra8_startup_parent"',
    "  done",
    "  [[ -f \"$ra8_startup_reentry\" ]] || _ra8_startup_refuse 'refuses a non-regular path'",
    '  if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \\',
    "    -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \\",
    '    /bin/bash -p -- "$ra8_startup_reentry" "$@"; then',
    f"    {FAILED_CLEANUP_EXEC}",
    "  fi",
    "fi",
    "unset -v ra8_startup_check ra8_startup_env_done_count",
    "unset -v ra8_startup_env_name ra8_startup_env_row",
    "unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry",
    "unset -v RA8_STARTUP_ENV_DONE",
    "unset -v RA8_STARTUP_ENV_SCRUBBED",
    "unset -f _ra8_startup_refuse",
)
PRIVILEGED_DUAL_BODY_PREFIX = (
    *PRIVILEGED_BODY_PREFIX[
        : PRIVILEGED_BODY_PREFIX.index("if ((${#ra8_startup_env_unset[@]})); then") + 1
    ],
    '  if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then',
    "    printf 'error: sourced privileged entry refuses inherited Bash functions\\n' >&2",
    "    unset -v ra8_startup_env_done_count",
    "    unset -v ra8_startup_env_name ra8_startup_env_row ra8_startup_env_unset",
    "    unset -v RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED",
    "    unset -f _ra8_startup_refuse",
    "    return 2",
    "  fi",
    *PRIVILEGED_BODY_PREFIX[
        PRIVILEGED_BODY_PREFIX.index("if ((${#ra8_startup_env_unset[@]})); then") + 1 :
    ],
)
PRIVILEGED_RIG_BODY_PREFIX = tuple(
    "    printf 'error: sourced rig contract refuses inherited Bash functions\\n' >&2"
    if line
    == "    printf 'error: sourced privileged entry refuses inherited Bash functions\\n' >&2"
    else line
    for line in PRIVILEGED_DUAL_BODY_PREFIX
)
PRIVILEGED_BODY_CLOSE = ("else", '[[ "$-" == *p* ]]', "fi")
PRIVILEGED_RUNTIME_VARIANTS = tuple(
    WrapperVariant(name, prefix, PRIVILEGED_BODY_CLOSE)
    for name, prefix in (
        ("plain", PRIVILEGED_BODY_PREFIX),
        ("dual", PRIVILEGED_DUAL_BODY_PREFIX),
        ("rig", PRIVILEGED_RIG_BODY_PREFIX),
    )
)
FORBIDDEN_REEXEC_TOKENS = (
    "builtin unset BASH_ENV",
    "builtin exec /bin/bash",
    "command builtin unset BASH_ENV",
    "command builtin exec /bin/bash",
)
PINNED_INTERPRETER_BOUNDARIES = {
    **dict.fromkeys(
        PRIVILEGED_PATHS,
        (
            PRIVILEGED_SHEBANG,
            "# SPDX-License-Identifier: MIT",
            "# Copyright (c) 2026 Brighton Sikarskie",
            PRIVILEGED_REASON,
        ),
    ),
}


def _shell_census() -> tuple[str, ...]:
    """Return the canonical tracked-and-untracked first-party shell census."""
    return tuple(files_for(("shell",))["shell"])


def _is_executable(path: Path) -> bool:
    """Return whether any executable mode bit is set."""
    return bool(path.stat().st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))


def _header(path: Path) -> tuple[str, ...]:
    """Return the four-line combined preamble, or an empty tuple when unreadable."""
    try:
        return tuple(path.read_text(encoding="utf-8").splitlines()[:4])
    except (OSError, UnicodeError):
        return ()


def _header_matches(header: tuple[str, ...], expected: tuple[str, ...]) -> bool:
    """Return whether ``header`` starts with the complete expected preamble."""
    return header[: len(expected)] == expected


def _policy_findings(
    census: set[str],
    policies: dict[str, ShellPolicy],
) -> list[str]:
    """Return missing and stale typed-authority entries."""
    missing = sorted(census - policies.keys())
    findings = [f"unclassified shell entry point: {path}" for path in missing]
    findings.extend(
        f"stale shell entry-point authority: {path}" for path in sorted(policies.keys() - census)
    )
    for path, policy in sorted(policies.items()):
        if policy.usage is ShellUsage.SOURCED_ONLY and policy.executable:
            findings.append(f"{path}: sourced-only policy cannot be executable")
        if policy.source_requires_privileged_parent and policy.usage is ShellUsage.ENTRY:
            findings.append(f"{path}: entry-only policy cannot require a privileged source parent")
        if (
            policy.security is ShellSecurity.PRIVILEGED
            and policy.usage is not ShellUsage.ENTRY
            and not policy.source_requires_privileged_parent
        ):
            findings.append(f"{path}: privileged sourced usage requires a privileged parent")
        if policy.security is ShellSecurity.PRIVILEGED and policy.dialect is not ShellDialect.BASH:
            findings.append(f"{path}: privileged policy requires the Bash dialect")
    return findings


def _path_findings(rel: str, policy: ShellPolicy) -> list[str]:
    """Validate one path's exact shebang, reason, and executable mode."""
    path = REPO_ROOT / rel
    header = _header(path)
    findings: list[str] = []
    if policy.security is ShellSecurity.PRIVILEGED:
        expected_header = PINNED_INTERPRETER_BOUNDARIES[rel]
    elif policy.dialect is ShellDialect.POSIX_SH:
        expected_header = (PORTABLE_SH_SHEBANG,)
    else:
        expected_header = (PORTABLE_SHEBANG,)
    if not _header_matches(header, expected_header):
        findings.append(
            f"{rel}: {policy.security.value}/{policy.usage.value} header "
            f"must start with {expected_header!r}"
        )
    try:
        executable = _is_executable(path)
    except OSError as exc:
        findings.append(f"{rel}: cannot inspect executable mode: {exc}")
    else:
        if executable != policy.executable:
            findings.append(
                f"{rel}: executable={executable} disagrees with typed authority "
                f"executable={policy.executable}"
            )
    return findings


def _active_lines(text: str) -> tuple[str, ...]:
    """Return nonblank, noncomment physical lines stripped for guard checks."""
    return tuple(
        line.strip()
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def _single_outer_if(text: str) -> bool:
    """Return whether shfmt parses the whole program as one outer if."""
    shfmt = shutil.which("shfmt", path="/usr/local/bin:/usr/bin:/opt/homebrew/bin:/opt/local/bin")
    if shfmt is None:
        return False
    result = subprocess.run(  # noqa: S603 - fixed absolute executable from a closed path.
        [shfmt, "--to-json"],
        input=text,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return False
    try:
        tree = json.loads(result.stdout)
    except json.JSONDecodeError:
        return False
    statements = tree.get("Stmts", [])
    return len(statements) == 1 and statements[0].get("Cmd", {}).get("Type") == "IfClause"


def privileged_body_findings(
    rel: str,
    text: str,
    policy: ShellPolicy | None = None,
) -> list[str]:
    """Require the complete real body to live in the privileged branch."""
    active = _active_lines(text)
    findings: list[str] = []
    if rel == "scripts/hil/lib/rig_contract.sh":
        prefix = PRIVILEGED_RIG_BODY_PREFIX
    elif policy is not None and policy.usage is ShellUsage.DUAL_USE:
        prefix = PRIVILEGED_DUAL_BODY_PREFIX
    else:
        prefix = PRIVILEGED_BODY_PREFIX
    if active[: len(prefix)] != tuple(line.strip() for line in prefix):
        findings.append(
            f"{rel}: wrapper and complete descendant startup cleanup are not first active code"
        )
    if active[-len(PRIVILEGED_BODY_CLOSE) :] != PRIVILEGED_BODY_CLOSE:
        findings.append(f"{rel}: privileged-body wrapper does not close the entire real body")
    if not _single_outer_if(text):
        findings.append(f"{rel}: active code escapes the outer privileged branch")
    if any(token in text for token in FORBIDDEN_REEXEC_TOKENS):
        findings.append(f"{rel}: unsafe in-script sanitization/re-exec remains")
    return findings


def _requires_privileged_body(rel: str, text: str, policy: ShellPolicy) -> bool:
    """Derive wrappers exhaustively from the typed privilege/usage authority."""
    del rel, text
    return (
        policy.security is ShellSecurity.PRIVILEGED and policy.usage is not ShellUsage.SOURCED_ONLY
    )


def scan() -> tuple[list[str], int, int, int, int]:
    """Return findings and typed population counts."""
    census = set(_shell_census())
    findings = _policy_findings(census, SHELL_POLICIES)
    for rel in sorted(census & SHELL_POLICIES.keys()):
        findings.extend(_path_findings(rel, SHELL_POLICIES[rel]))
    guarded = 0
    for rel in sorted(census & SHELL_POLICIES.keys()):
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            findings.append(f"{rel}: cannot inspect privileged body: {exc}")
            continue
        if _requires_privileged_body(rel, text, SHELL_POLICIES[rel]):
            guarded += 1
            findings.extend(privileged_body_findings(rel, text, SHELL_POLICIES[rel]))
    privileged = sum(
        policy.security is ShellSecurity.PRIVILEGED for policy in SHELL_POLICIES.values()
    )
    sourced = sum(policy.usage is ShellUsage.SOURCED_ONLY for policy in SHELL_POLICIES.values())
    return findings, len(census), privileged, sourced, guarded


def _fixture_text(
    body: str = "printf 'BODY mode=%s\\n' \"$-\"",
    *,
    cleanup: bool = True,
    prefix: tuple[str, ...] | None = None,
) -> str:
    """Return one inert exact privileged-body fixture."""
    if prefix is None:
        prefix = PRIVILEGED_BODY_PREFIX if cleanup else (PRIVILEGED_BODY_OPEN,)
    guarded = "\n".join((prefix[0], *(f"  {line}" for line in prefix[1:]), f"  {body}"))
    return (
        f"{PRIVILEGED_SHEBANG}\n"
        "# SPDX-License-Identifier: MIT\n"
        "# Copyright (c) 2026 Brighton Sikarskie\n"
        f"{PRIVILEGED_REASON}\n"
        f"{guarded}\n"
        f"{PRIVILEGED_BODY_CLOSE[0]}\n  {PRIVILEGED_BODY_CLOSE[1]}\n"
        f"{PRIVILEGED_BODY_CLOSE[2]}\n"
    )


@dataclasses.dataclass(frozen=True)
class StartupCase:
    """One hostile startup fixture driven through the governed wrapper."""

    prefix: tuple[str, ...] | None = None
    body_override: str | None = None
    args: tuple[str, ...] = ()
    extra_env: dict[str, str] | None = None
    producer: str = "live"
    entry: str = "direct"
    raw_function: bool = True
    count_entries: bool = False
    timeout: float = 10.0


SELFTEST_TALLY = {"runtime": 0, "structural_mutations": 0}


def _producer_script(kind: str) -> str:
    """Return a synthetic NUL-framed producer for one completeness attack."""
    marker = "RA8_STARTUP_ENV_DONE=1\\0"
    rows = {
        "zero": "#!/bin/sh\nexit 0\n",
        "duplicate": f"#!/bin/sh\nprintf '{marker}{marker}'\n",
        "not-last": f"#!/bin/sh\nprintf '{marker}AFTER=1\\0'\n",
        "torn": "#!/bin/sh\nprintf 'BASH_FUNC_probe%%%%=() { :; }\\0'\nexit 42\n",
        "failed": "#!/bin/sh\nexit 2\n",
    }
    return rows[kind]


def _startup_fixture_text(root: Path, case: StartupCase, body: str | None, cleanup: bool) -> str:
    """Render one wrapper fixture, replacing its producer when requested."""
    fixture = (
        _fixture_text(cleanup=cleanup, prefix=case.prefix)
        if body is None
        else _fixture_text(body, cleanup=cleanup, prefix=case.prefix)
    )
    if case.producer != "live":
        producer = root / "environment-producer"
        producer.write_text(_producer_script(case.producer), encoding="ascii")
        producer.chmod(0o700)
        live = (
            "    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&\n"
            "      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\\0'"
        )
        if fixture.count(live) != 1:
            message = "startup producer fixture no longer matches the governed wrapper"
            raise RuntimeError(message)
        fixture = fixture.replace(live, f"    {producer}", 1)
    if case.count_entries:
        entry = 'if [[ "$-" == *p* ]]; then\n'
        counted = entry + '  /usr/bin/printf x >>"${RA8_STARTUP_ENTRY_LOG:?}"\n'
        fixture = fixture.replace(entry, counted, 1)
    return fixture


def _startup_bash_env(early_exit: bool, descendant: bool) -> str:
    """Return hostile startup bytes for one runtime fixture."""
    if early_exit:
        return "printf 'EARLY\\n'\nexit 43\n"
    names = ("command", "builtin", "unset", "exec", "exit", "/bin/bash")
    definitions = ["shopt -s expand_aliases"]
    for index, name in enumerate(names):
        definitions.append(f"function {name} {{ printf 'HOSTILE-{index}\\n'; }}")
        definitions.append(f"alias {name}='printf ALIAS-{index}\\n'")
    if descendant:
        definitions.append("printf 'DESCENDANT-STARTUP\\n'")
    return "\n".join(definitions) + "\n"


def _startup_argv(root: Path, script: Path, entry: str, privileged: bool) -> list[str]:
    """Materialize and select one script-name shape."""
    hardlink = root / "hardlink.sh"
    if entry == "hardlink":
        os.link(script, hardlink)
    leaf_link = root / "leaf-link.sh"
    if entry == "leaf-symlink":
        leaf_link.symlink_to(script)
    parent_link = root / "parent-link"
    if entry == "parent-symlink":
        parent_link.symlink_to(root, target_is_directory=True)
    choices = {
        "absolute": ["/bin/bash", "-p", str(script)],
        "relative": ["./fixture.sh"],
        "hardlink": [str(hardlink)],
        "leaf-symlink": [str(leaf_link)],
        "parent-symlink": [str(parent_link / script.name)],
        "bare": ["/bin/bash", "-p", script.name],
        "dev-fd": ["/bin/bash", "-p", "-c", f"exec /bin/bash -p <(cat {shlex_quote(str(script))})"],
        "sourced": [
            "/bin/bash",
            "-p",
            "-c",
            f". {shlex_quote(str(script))}; printf 'SOURCED_RC=%s\\n' \"$?\"",
        ],
    }
    return choices.get(entry, [str(script)] if privileged else ["/bin/bash", str(script)])


def _run_fixture(
    *,
    privileged: bool,
    early_exit: bool,
    descendant: bool = False,
    cleanup: bool = True,
    case: StartupCase | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run the inert wrapper against hostile startup definitions."""
    case = case if case is not None else StartupCase()
    with tempfile.TemporaryDirectory(prefix="ra8-privileged-body-") as raw:
        # macOS exposes its temporary root through /var -> /private/var. Use
        # the physical spelling so ordinary fixtures do not accidentally test
        # the explicit symlink-parent refusal.
        root = Path(raw).resolve()
        script = root / "fixture.sh"
        bash_env = root / "bash-env"
        body = (
            '/bin/bash -c "type probe >/dev/null 2>&1 && '
            "printf 'IMPORTED\\n' || printf 'CHILD\\n'\""
            if descendant
            else None
        )
        body = case.body_override if case.body_override is not None else body
        fixture = _startup_fixture_text(root, case, body, cleanup)
        script.write_text(fixture, encoding="utf-8")
        script.chmod(0o700)
        bash_env.write_text(_startup_bash_env(early_exit, descendant), encoding="utf-8")
        argv = _startup_argv(root, script, case.entry, privileged)
        environment = {
            "BASH_ENV": str(bash_env),
            "ENV": str(bash_env),
            "PATH": "/usr/bin:/bin",
            "LC_ALL": "C",
        }
        if case.count_entries:
            environment["RA8_STARTUP_ENTRY_LOG"] = str(root / "entry-log")
        if case.raw_function:
            environment["BASH_FUNC_probe%%"] = "() { printf 'RAW-FUNCTION\\n'; }"
        environment.update(case.extra_env or {})
        SELFTEST_TALLY["runtime"] += 1
        return run_private(
            PrivateRun(
                (*argv, *case.args),
                root,
                environment,
                timeout=case.timeout,
            )
        )


def shlex_quote(value: str) -> str:
    """Quote one fixture-only path without adding a runtime dependency."""
    return "'" + value.replace("'", "'\\''") + "'"


def _authority_selftest_failures() -> list[str]:
    """Exercise typed census and independent usage/security policies."""
    failures: list[str] = []
    policy = ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    )
    if _policy_findings({"a.sh"}, {"a.sh": policy}):
        failures.append("matching typed census was rejected")
    if not _policy_findings({"a.sh", "new.sh"}, {"a.sh": policy}):
        failures.append("future unclassified shell was accepted")
    if not _policy_findings({"a.sh"}, {"a.sh": policy, "old.sh": policy}):
        failures.append("stale authority entry was accepted")
    sourced = ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=True,
    )
    if _policy_findings({"lib.sh"}, {"lib.sh": sourced}):
        failures.append("valid privileged sourced-only policy was rejected")
    failures.extend(_preamble_selftest_failures())
    if not _policy_findings(
        {"lib.sh"},
        {
            "lib.sh": ShellPolicy(
                ShellSecurity.PRIVILEGED,
                ShellUsage.SOURCED_ONLY,
                ShellDialect.BASH,
                executable=True,
                source_requires_privileged_parent=True,
            )
        },
    ):
        failures.append("executable sourced-only policy was accepted")
    dual = ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.DUAL_USE,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=True,
    )
    if _policy_findings({"dual.sh"}, {"dual.sh": dual}):
        failures.append("valid non-executable dual-use policy was rejected")
    failures.extend(_privileged_parent_selftest_failures((sourced, dual)))
    if len(SHELL_POLICIES) != len(set(SHELL_POLICIES)):
        failures.append("aggregate policy authority contains duplicate paths")
    for name, domain in (("CI", CI_POLICY_ROWS), ("HIL", HIL_POLICY_ROWS)):
        if any(row[0] not in SHELL_POLICIES for row in domain):
            failures.append(f"{name} domain policy rows were not merged")
        try:
            merge_policy_tables({domain[0][0]: policy}, domain)
        except ValueError:
            pass
        else:
            failures.append(f"duplicate {name} domain policy path was accepted")
    return failures


def _privileged_parent_selftest_failures(
    policies: tuple[ShellPolicy, ...],
) -> list[str]:
    """Reject privileged sourced policies without a privileged parent."""
    failures: list[str] = []
    for policy in policies:
        weakened = dataclasses.replace(policy, source_requires_privileged_parent=False)
        if _policy_findings({"fixture.sh"}, {"fixture.sh": weakened}):
            continue
        failures.append(
            f"privileged {policy.usage.value} policy without a privileged parent was accepted"
        )
    return failures


def _preamble_selftest_failures() -> list[str]:
    """Exercise the combined privileged-shell preamble."""
    failures: list[str] = []
    protected = PINNED_INTERPRETER_BOUNDARIES["scripts/hil/all.sh"]
    canonical_protected = (
        "#!/bin/bash -p",
        "# SPDX-License-Identifier: MIT",
        "# Copyright (c) 2026 Brighton Sikarskie",
        "# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.",
    )
    if not _header_matches(canonical_protected, protected):
        failures.append("canonical combined privileged preamble was rejected")
    old_order = (
        PRIVILEGED_SHEBANG,
        PRIVILEGED_REASON,
        "# SPDX-License-Identifier: MIT",
        "# Copyright (c) 2026 Brighton Sikarskie",
    )
    if _header_matches(old_order, protected):
        failures.append("security rationale before attribution was accepted")
    wrong_reason = (*canonical_protected[:3], "# SHEBANG-SECURITY: vague rationale.")
    if _header_matches(wrong_reason, protected):
        failures.append("non-canonical privileged security rationale was accepted")
    return failures


def _guard_structure_mutations(safe: str) -> tuple[str, ...]:
    """Return independent weakenings of the exact wrapper."""
    return (
        safe.replace(PRIVILEGED_BODY_OPEN, 'if [[ "$-" != *p* ]]; then', 1),
        safe.replace("  unset -v BASH_ENV ENV\n", "", 1),
        safe.replace(
            "      BASH_FUNC_*%% | BASH_FUNC_*'()') "
            'ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;\n',
            "",
            1,
        ),
        safe.replace("    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&\n", "", 1),
        safe.replace("      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\\0'\n", "", 1),
        safe.replace(
            "  ((ra8_startup_env_done_count == 1)) && "
            '[[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || '
            "_ra8_startup_refuse 'environment enumeration was incomplete'\n",
            "  true\n",
            1,
        ),
        safe.replace(
            '    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || '
            "_ra8_startup_refuse 'scrub did not converge'\n",
            "  true\n",
            1,
        ),
        safe.replace(
            '    [[ "$ra8_startup_reentry" == */* ]] || '
            "_ra8_startup_refuse 'requires a script path'\n",
            "  true\n",
            1,
        ),
        safe.replace(
            '      [[ ! -L "$ra8_startup_check" ]] || '
            "_ra8_startup_refuse 'refuses a symlinked path'\n",
            "  true\n",
            1,
        ),
        safe.replace(
            '    [[ -f "$ra8_startup_reentry" ]] || '
            "_ra8_startup_refuse 'refuses a non-regular path'\n",
            "  true\n",
            1,
        ),
        safe.replace(
            '    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \\\n',
            "",
            1,
        ),
        safe.replace(f"      {FAILED_CLEANUP_EXEC}\n", "", 1),
        safe.replace("else\n", "fi\nBODY_AFTER\nelse\n", 1),
        safe + "BODY_AFTER\n",
        safe.replace('  [[ "$-" == *p* ]]', "  command exit 1", 1),
        safe.replace("  printf", "  command builtin exec /bin/bash -p\n  printf", 1),
    )


def _guard_structure_selftest_failures() -> list[str]:
    """Exercise exact outer-wrapper structure in both directions."""
    failures: list[str] = []
    safe = _fixture_text()
    if privileged_body_findings("fixture.sh", safe):
        failures.append("exact privileged-body wrapper was rejected")
    mutations = _guard_structure_mutations(safe)
    SELFTEST_TALLY["structural_mutations"] += len(mutations)
    if any(text == safe for text in mutations):
        failures.append("a structural mutation did not change the fixture")
    if any(not privileged_body_findings("fixture.sh", text) for text in mutations):
        failures.append("a weakened privileged-body wrapper was accepted")
    return failures


def _guard_runtime_selftest_failures() -> list[str]:
    """Exercise weak startup and descendant-channel attacks at runtime."""
    failures: list[str] = []
    weak = _run_fixture(privileged=False, early_exit=False)
    if weak.returncode == 0 or weak.stdout or "BODY" in weak.stderr:
        failures.append("weak Bash invocation reached output under hostile functions/aliases")
    direct = _run_fixture(privileged=True, early_exit=True)
    if direct.returncode != 0 or "BODY mode=" not in direct.stdout or "EARLY" in direct.stdout:
        failures.append("privileged shebang did not ignore hostile early-exit BASH_ENV")
    early = _run_fixture(privileged=False, early_exit=True)
    if (
        early.returncode != EARLY_EXIT_STATUS
        or "BODY" in early.stdout
        or "EARLY" not in early.stdout
    ):
        failures.append("weak early-exit BASH_ENV behavior was reported dishonestly")
    descendant = _run_fixture(privileged=True, early_exit=False, descendant=True)
    control = _run_fixture(privileged=True, early_exit=False, descendant=True, cleanup=False)
    if descendant.returncode != 0 or descendant.stdout != "CHILD\n":
        failures.append("privileged body leaked hostile startup channels to a child Bash")
    if "DESCENDANT-STARTUP" not in control.stdout or "IMPORTED" not in control.stdout:
        failures.append("descendant startup/function control did not demonstrate both attacks")
    return failures


def _legacy_phantom_selftest_failures() -> list[str]:
    """Prove the old line framing loops and NUL framing does not."""
    legacy_prefix = (
        PRIVILEGED_BODY_OPEN,
        "unset -v BASH_ENV ENV",
        "declare -a ra8_startup_env_unset=()",
        "while IFS='=' read -r ra8_startup_env_name _; do",
        'case "$ra8_startup_env_name" in',
        'BASH_FUNC_*%%) ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;',
        "esac",
        "done < <(/usr/bin/env)",
        "if ((${#ra8_startup_env_unset[@]})); then",
        'exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \\',
        '/bin/bash -p -- "$0" "$@"',
        "fi",
        "unset -v ra8_startup_env_name ra8_startup_env_unset",
    )
    phantom = {"RA8_STARTUP_PHANTOM": "\nBASH_FUNC_phantom%%=() { :; }\n}"}
    failures: list[str] = []
    try:
        _run_fixture(
            privileged=True,
            early_exit=False,
            case=StartupCase(
                prefix=legacy_prefix,
                extra_env=phantom,
                raw_function=False,
                timeout=0.5,
            ),
        )
    except subprocess.TimeoutExpired:
        pass
    else:
        failures.append("the legacy line-framed negative control did not loop")

    fixed = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(extra_env=phantom, raw_function=False),
    )
    if fixed.returncode != 0 or fixed.stdout.count("BODY mode=") != 1:
        failures.append("an embedded-newline phantom row did not reach the body exactly once")
    return failures


def _producer_completion_selftest_failures() -> list[str]:
    """Require zero, duplicate, misplaced, torn, and failed producers to refuse."""
    failures: list[str] = []
    for producer in ("zero", "duplicate", "not-last", "torn", "failed"):
        result = _run_fixture(
            privileged=True,
            early_exit=False,
            case=StartupCase(producer=producer),
        )
        if result.returncode == 0 or "enumeration was incomplete" not in result.stderr:
            failures.append(f"the {producer} environment producer did not fail closed")
    return failures


def _startup_convergence_selftest_failures() -> list[str]:
    """Prove NUL framing, bounded re-entry, argv, and producer completeness."""
    failures = [
        *_legacy_phantom_selftest_failures(),
        *_producer_completion_selftest_failures(),
    ]
    sentinel = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(extra_env={"RA8_STARTUP_ENV_SCRUBBED": "attacker"}),
    )
    if sentinel.returncode == 0 or "did not converge" not in sentinel.stderr:
        failures.append("an attacker-supplied scrub sentinel skipped the real enumeration")

    counted = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(
            body_override='/usr/bin/wc -c <"${RA8_STARTUP_ENTRY_LOG:?}"',
            count_entries=True,
        ),
    )
    if counted.returncode != 0 or counted.stdout.strip() != "2":
        failures.append("function cleanup did not converge in exactly two process entries")

    argv = ("plain", "", "two words", "tab\tvalue", "line one\nline two")
    roundtrip_status = 9
    roundtrip = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(
            body_override=f"printf 'ARG:%s\\n' \"$@\"\n  exit {roundtrip_status}",
            args=argv,
        ),
    )
    expected = "".join(f"ARG:{value}\n" for value in argv)
    if roundtrip.returncode != roundtrip_status or roundtrip.stdout != expected:
        failures.append("the bounded cleanup re-entry corrupted argv or exit status")
    return failures


def _reentry_path_selftest_failures() -> list[str]:
    """Exercise every supported and refused script-name shape."""
    failures: list[str] = []
    for entry in ("direct", "absolute", "relative", "hardlink"):
        result = _run_fixture(
            privileged=True,
            early_exit=False,
            case=StartupCase(entry=entry),
        )
        if result.returncode != 0 or result.stdout.count("BODY mode=") != 1:
            failures.append(f"the supported {entry} re-entry path was rejected")
    for entry in ("leaf-symlink", "parent-symlink", "bare", "dev-fd"):
        result = _run_fixture(
            privileged=True,
            early_exit=False,
            case=StartupCase(entry=entry),
        )
        if result.returncode == 0 or "privileged startup" not in result.stderr:
            failures.append(f"the unsafe {entry} re-entry path was accepted")
    return failures


def _wrapper_variant_selftest_failures() -> list[str]:
    """Run the plain, dual-use, and rig variants instead of pinning text only."""
    failures: list[str] = []
    dual_sourced = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(prefix=PRIVILEGED_DUAL_BODY_PREFIX, entry="sourced"),
    )
    if "SOURCED_RC=2" not in dual_sourced.stdout or "BODY mode=" in dual_sourced.stdout:
        failures.append("the sourced dual-use wrapper did not refuse inherited functions")
    rig_sourced = _run_fixture(
        privileged=True,
        early_exit=False,
        case=StartupCase(prefix=PRIVILEGED_RIG_BODY_PREFIX, entry="sourced"),
    )
    if "SOURCED_RC=2" not in rig_sourced.stdout or "sourced rig contract" not in rig_sourced.stderr:
        failures.append("the sourced rig wrapper did not use its fail-closed branch")
    for label, prefix in (
        ("dual-use", PRIVILEGED_DUAL_BODY_PREFIX),
        ("rig", PRIVILEGED_RIG_BODY_PREFIX),
    ):
        result = _run_fixture(
            privileged=True,
            early_exit=False,
            case=StartupCase(prefix=prefix),
        )
        if result.returncode != 0 or result.stdout.count("BODY mode=") != 1:
            failures.append(f"the directly executed {label} wrapper did not run exactly once")
    return failures


def _guard_derivation_selftest_failures() -> list[str]:
    """Prove typed policy, not historical repair tokens, derives wrappers."""
    failures: list[str] = []
    weak_policy = ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    )
    weak_text = (
        f"{PRIVILEGED_SHEBANG}\n"
        "# SPDX-License-Identifier: MIT\n"
        "# Copyright (c) 2026 Brighton Sikarskie\n"
        f"{PRIVILEGED_REASON}\n"
        'if [[ "$-" != *p* ]]; then\n'
        '  exec /bin/bash -p "$0" "$@"\n'
        "fi\nprintf 'BODY\\n'\n"
    )
    if not _requires_privileged_body("future.sh", weak_text, weak_policy):
        failures.append("future privileged weak re-exec did not derive a body-wrapper requirement")
    elif not privileged_body_findings("future.sh", weak_text):
        failures.append("future privileged weak re-exec passed without the governed wrapper")
    no_repair_tokens = (
        f"{PRIVILEGED_SHEBANG}\n"
        "# SPDX-License-Identifier: MIT\n"
        "# Copyright (c) 2026 Brighton Sikarskie\n"
        f"{PRIVILEGED_REASON}\n"
        "printf 'FUTURE BODY\\n'\n"
    )
    if not _requires_privileged_body("future.sh", no_repair_tokens, weak_policy):
        failures.append("privileged entry without historical repair tokens escaped the wrapper")
    portable_policy = ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    )
    if _requires_privileged_body("portable.sh", no_repair_tokens, portable_policy):
        failures.append("portable entry incorrectly inherited the privileged-body wrapper")
    source_policy = ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=True,
    )
    if _requires_privileged_body("source.sh", no_repair_tokens, source_policy):
        failures.append("sourced-only helper incorrectly became a launchable guarded entry")
    return failures


def _guard_selftest_failures() -> list[str]:
    """Return all structural, runtime, and policy-derivation failures."""
    failures = (
        _guard_structure_selftest_failures()
        + _guard_runtime_selftest_failures()
        + _startup_convergence_selftest_failures()
        + _reentry_path_selftest_failures()
        + _wrapper_variant_selftest_failures()
        + _guard_derivation_selftest_failures()
    )
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-wrapper-state-") as temporary:
            SELFTEST_TALLY["runtime"] += run_privileged_wrapper_runtime_cases(
                Path(temporary), PRIVILEGED_RUNTIME_VARIANTS
            )
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        failures.append(f"hostile wrapper state matrix failed: {exc}")
    return failures


def _selftest_failures() -> list[str]:
    """Return every authority and full-body wrapper selftest failure."""
    minimum_runtime_cases = 25 + (5 * len(PRIVILEGED_RUNTIME_VARIANTS))
    minimum_structural_mutations = 15
    minimum_privileged_paths = 81
    minimum_guarded_paths = 71
    SELFTEST_TALLY.update(runtime=0, structural_mutations=0)
    failures = _authority_selftest_failures() + _guard_selftest_failures()
    if SELFTEST_TALLY["runtime"] < minimum_runtime_cases:
        failures.append(f"the runtime attack matrix collapsed below {minimum_runtime_cases} cases")
    if SELFTEST_TALLY["structural_mutations"] < minimum_structural_mutations:
        failures.append("the structural mutation corpus collapsed below 15 cases")
    try:
        _, _, privileged, _, guarded = scan()
    except (OSError, subprocess.SubprocessError, UnicodeError) as exc:
        failures.append(f"the live wrapper census could not be measured: {exc}")
    else:
        if privileged < minimum_privileged_paths or guarded < minimum_guarded_paths:
            failures.append(
                f"the live wrapper census collapsed to {privileged} privileged/{guarded} guarded"
            )
    return failures


def selftest() -> int:
    """Run both-direction typed-authority and startup-attack fixtures."""
    failures = _selftest_failures()
    for failure in failures:
        print(f"check_shebangs.py --selftest: FAIL: {failure}", file=sys.stderr)
    if failures:
        return EXIT_FAIL
    print(
        "check_shebangs.py --selftest: PASS "
        f"({SELFTEST_TALLY['runtime']} runtime cases, "
        f"{SELFTEST_TALLY['structural_mutations']} structural mutations)"
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Run the selftest or the exhaustive live authority scan."""
    if argv[1:] == ["--selftest"]:
        return selftest()
    if argv[1:]:
        print("usage: check_shebangs.py [--selftest]", file=sys.stderr)
        return EXIT_CONFIG
    try:
        findings, total, privileged, sourced, guarded = scan()
    except (OSError, subprocess.SubprocessError, UnicodeError) as exc:
        print(f"check_shebangs.py: FATAL: {exc}", file=sys.stderr)
        return EXIT_CONFIG
    if findings:
        print("check_shebangs.py: typed shell-entrypoint finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return EXIT_FAIL
    portable = total - privileged
    print(
        "check_shebangs.py: exhaustive authority clean "
        f"({total} shell files: {privileged} privileged, {portable} portable, "
        f"{sourced} sourced-only, {guarded} structurally guarded)"
    )
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

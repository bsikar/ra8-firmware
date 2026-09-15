#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Linter for repository justfiles.

Formatting (``just --fmt``) is enforced by the format gate (``format_tree.sh``),
never here.
"""

from __future__ import annotations

import argparse
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIRMWARE_BUILD_RE = re.compile(
    r'^build app="" build_type="([A-Za-z0-9]+)":$',
    re.MULTILINE,
)
BARE_NESTED_JUST_RE = re.compile(r"(?:^\s*@?|\b(?:then|do|else)\s+|(?:&&|\|\||;)\s*)just(?=\s|$)")
CI_SH_CALL_RE = re.compile(r"^\s*@?/bin/bash\s+-p\s+scripts/ci\.sh(?P<args>(?:\s+.*)?)$")
CI_SH_SWITCHES = frozenset({"--container", "--fast", "--list-gates", "--native", "--rebuild"})
CI_SH_VALUE_OPTIONS = frozenset({"--gate", "--selftest-abort"})
VALUE_OPTION_TOKEN_COUNT = 2
REMOTE_CI_PREPARE_COUNT = 2
NATIVE_FAST_RECIPE_RE = re.compile(
    r"^native_fast:[ \t]*\n(?P<body>(?:[ \t]+[^\n]*(?:\n|$))*)",
    re.MULTILINE,
)
NATIVE_FAST_COMMAND = "/bin/bash -p scripts/ci.sh --native --fast"
REMOTE_CI_WSL_BLOCK_RE = re.compile(
    r'if \[\[ "\$remote_shell" == wsl\* \]\]; then(?P<body>.*?)\n    else', re.DOTALL
)
REMOTE_CI_LINUX_BLOCK_RE = re.compile(
    r"^    else(?P<body>.*?)^    fi\n[ \t]*\n    if ! printf", re.MULTILINE | re.DOTALL
)
REMOTE_CI_SNAPSHOT_COMMIT = (
    "git -c user.email=ci@localhost -c user.name=ci commit --quiet --no-verify "
    "-m 'remote CI transport snapshot'"
)
REMOTE_CI_HISTORY_COMMIT = "git commit --quiet --allow-empty --no-verify -m $remote_history_message"


def check_firmware_build_default(text: str) -> list[str]:
    """Keep the migrated per-app build default byte-for-behaviour compatible."""
    match = FIRMWARE_BUILD_RE.search(text)
    if match is None:
        return ["just/apps.just: firmware build recipe/default is missing"]
    if match.group(1) != "RelWithDebInfo":
        return [
            "just/apps.just: apps::build must default to RelWithDebInfo "
            "(the historical per-app contract)"
        ]
    return []


def check_nested_just_invocations(text: str, rel: str) -> list[str]:
    """Require same-environment recursion to preserve the invoking Just path.

    A recipe may be entered through an absolute executable while that
    executable's directory is absent from PATH, as on a noninteractive SSH
    session. ``just_executable()`` preserves the known-good executable. A bare
    ``just`` passed through ``devcontainer_run.sh`` is intentionally excluded:
    it runs in the container's namespace, where the host executable path is
    invalid and the image owns PATH.
    """
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if "scripts/ci/devcontainer_run.sh" in line:
            continue
        # An SSH command runs on a different host, whose Just executable is
        # intentionally resolved from that host's PATH. The invoking
        # executable path is valid only for same-machine recursion.
        if re.search(r"\bssh\b.*\bjust(?=\s|$)", line):
            continue
        if BARE_NESTED_JUST_RE.search(line) is not None:
            findings.append(
                f"{rel}:{number}: nested Just call must use "
                '"{{ just_executable() }}" instead of PATH lookup'
            )
    return findings


def check_ci_driver_invocations(text: str, rel: str) -> list[str]:
    """Reject stale or malformed ``scripts/ci.sh`` options in Just recipes."""
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        match = CI_SH_CALL_RE.fullmatch(line)
        if match is None:
            continue
        try:
            args = shlex.split(match.group("args"))
        except ValueError as exc:
            findings.append(f"{rel}:{number}: cannot parse scripts/ci.sh arguments: {exc}")
            continue
        index = 0
        while index < len(args):
            option = args[index]
            if option in CI_SH_SWITCHES:
                index += 1
                continue
            if option in CI_SH_VALUE_OPTIONS:
                if index + 1 >= len(args) or args[index + 1].startswith("--"):
                    findings.append(f"{rel}:{number}: {option} requires one value")
                    break
                index += VALUE_OPTION_TOKEN_COUNT
                continue
            findings.append(
                f"{rel}:{number}: unsupported scripts/ci.sh option or argument {option!r}"
            )
            break
    return findings


def check_ci_native_fast_contract(text: str, rel: str) -> list[str]:
    """Pin the public native-fast recipe to the CI driver's real argv."""
    matches = list(NATIVE_FAST_RECIPE_RE.finditer(text))
    if len(matches) != 1:
        return [f"{rel}: expected exactly one native_fast recipe, found {len(matches)}"]
    body = [line.strip() for line in matches[0].group("body").splitlines() if line.strip()]
    if body != [NATIVE_FAST_COMMAND]:
        return [f"{rel}: native_fast must contain only `{NATIVE_FAST_COMMAND}`; found {body!r}"]
    return []


def _missing_active_snippets(
    text: str, rel: str, scope: str, snippets: tuple[str, ...]
) -> list[str]:
    """Return diagnostics for required active remote-transport fragments."""
    return [
        f"{rel}: {scope} remote CI missing active {snippet!r}"
        for snippet in snippets
        if snippet not in text
    ]


def check_remote_ci_contract(text: str, rel: str) -> list[str]:
    """Require remote CI to preserve host isolation and WSL container parity."""
    findings: list[str] = []
    wsl_match = REMOTE_CI_WSL_BLOCK_RE.search(text)
    linux_match = REMOTE_CI_LINUX_BLOCK_RE.search(text)
    if wsl_match is None:
        findings.append(f"{rel}: remote CI has no WSL isolation branch")
    else:
        wsl = wsl_match.group("body")
        findings.extend(
            _missing_active_snippets(
                wsl,
                rel,
                "WSL",
                (
                    'remote_profile="/etc/profile.d/ra8-dev-slice.sh"',
                    'remote_launcher="/usr/local/bin/ra8-dev"',
                    'gate_command="/bin/bash -p scripts/ci.sh"',
                    'gate_command="/bin/bash -p scripts/ci.sh --gate $remote_gate_arg --container"',
                ),
            )
        )
    if linux_match is None:
        findings.append(f"{rel}: remote CI has no Linux throttling branch")
    else:
        linux = linux_match.group("body")
        findings.extend(
            _missing_active_snippets(
                linux,
                rel,
                "Linux",
                (
                    "RA8_REMOTE_MAX_JOBS",
                    'remote_jobs="${RA8_REMOTE_MAX_JOBS:-2}"',
                    'if [[ ! "$remote_jobs" =~ ^[1-9][0-9]*$ ]]; then',
                    "nice -n 19 ionice -c3",
                    'gate_command="/bin/bash -p scripts/ci.sh --native"',
                ),
            )
        )
    if text.count(REMOTE_CI_SNAPSHOT_COMMIT) != 1:
        findings.append(f"{rel}: remote CI must create exactly one transport snapshot commit")
    if REMOTE_CI_HISTORY_COMMIT not in text:
        findings.append(f"{rel}: remote CI must preserve the candidate commit metadata")
    findings.extend(
        _missing_active_snippets(
            text,
            rel,
            "",
            (
                "remote_name={{ quote(name) }}",
                "remote_host={{ quote(host) }}",
                (
                    "read -r -a remote_ssh "
                    '<<<"$(./.venv/bin/python3 scripts/dev/fleet.py ssh-target "$remote_host")"'
                ),
                "printf -v remote_gate_arg '%q' \"$remote_name\"",
                'remote_tar="${remote_shell%/bin/bash -s}ionice -c3 /usr/bin/tar"',
                "printf 'source %q\\n' \"$remote_profile\"",
                'printf \'exec %s %s\\n\' "$remote_launcher" "$gate_command"',
            ),
        )
    )
    findings.extend(
        f"{rel}: remote CI must shell-quote {raw} before Bash parses it"
        for raw in ("{{ name }}", "{{ host }}")
        if raw in text
    )
    return findings


def find_justfiles() -> list[Path]:
    """Return all tracked justfiles in the repository."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- resolved Git executable and fixed argv
        [
            git_bin,
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "justfile",
            "*.just",
            "just/*.just",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    paths = [
        REPO_ROOT / line
        for raw_line in proc.stdout.splitlines()
        if (line := raw_line.strip()) and (REPO_ROOT / line).is_file()
    ]
    return sorted(paths)


def check_file(path: Path) -> list[str]:
    """Check a justfile for structural defects (nesting, driver calls, contracts)."""
    findings: list[str] = []
    if not path.is_file():
        return [f"{path}: file not found"]

    rel = path.relative_to(REPO_ROOT).as_posix()
    text = path.read_text(encoding="utf-8")
    findings.extend(check_nested_just_invocations(text, rel))
    findings.extend(check_ci_driver_invocations(text, rel))

    if path.resolve() == (REPO_ROOT / "just/apps.just").resolve():
        findings.extend(check_firmware_build_default(text))
    if rel == "just/ci.just":
        findings.extend(check_ci_native_fast_contract(text, rel))
    if rel == "just/ci_remote.just":
        findings.extend(check_remote_ci_contract(text, rel))
    return findings


def _selftest_build_default() -> tuple[int, str | None]:
    """Exercise the firmware-build default in both directions."""
    build_cases = (
        ('build app="" build_type="RelWithDebInfo":\n', False, "historical default passes"),
        ('build app="" build_type="Debug":\n', True, "Debug default fires"),
        ('build app="":\n', True, "missing selectable default fires"),
    )
    for text, expected, label in build_cases:
        if bool(check_firmware_build_default(text)) != expected:
            return len(build_cases), label
    return len(build_cases), None


def _selftest_nested_just() -> tuple[int, str | None]:
    """Exercise nested Just invocation policy in both directions."""
    nested_cases = (
        ("    just quality::run\n", True, "direct nested lookup fires"),
        ("    @just hooks\n", True, "quiet nested lookup fires"),
        ("    if ok; then just tests::build; fi\n", True, "shell-chain lookup fires"),
        (
            '    "{{ just_executable() }}" quality::run\n',
            False,
            "invoking executable stays quiet",
        ),
        (
            "    bash scripts/ci/devcontainer_run.sh -- just quality::local::check\n",
            False,
            "container-owned lookup stays quiet",
        ),
        ('    @echo "Run just quality::run"\n', False, "help prose stays quiet"),
    )
    for text, expected, label in nested_cases:
        if bool(check_nested_just_invocations(text, "fixture.just")) != expected:
            return len(nested_cases), label
    return len(nested_cases), None


def _selftest_ci_driver() -> tuple[int, str | None]:
    """Exercise generic CI-driver option validation in both directions."""
    ci_driver_cases = (
        (
            "    /bin/bash -p scripts/ci.sh --native --fast\n",
            False,
            "separate native and fast switches stay valid",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --gate work-harness\n",
            False,
            "gate value stays valid",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --native-fast\n",
            True,
            "invented combined switch fires",
        ),
        (
            "    /bin/bash -p scripts/ci.sh --gate\n",
            True,
            "missing gate value fires",
        ),
    )
    for text, expected, label in ci_driver_cases:
        if bool(check_ci_driver_invocations(text, "fixture.just")) != expected:
            return len(ci_driver_cases), label
    return len(ci_driver_cases), None


def _selftest_native_fast() -> tuple[int, str | None]:
    """Exercise the exact native-fast recipe contract in both directions."""
    native_fast_cases = (
        (
            f"native_fast:\n    {NATIVE_FAST_COMMAND}\n\nalias native-fast := native_fast\n",
            False,
            "exact native-fast recipe stays valid",
        ),
        (
            "native_fast:\n    bash scripts/ci.sh --native-fast\n",
            True,
            "historical stale recipe fires",
        ),
        (
            "native_fast_renamed:\n    /bin/bash -p scripts/ci.sh --native --fast\n",
            True,
            "missing native-fast recipe fires",
        ),
    )
    for text, expected, label in native_fast_cases:
        if bool(check_ci_native_fast_contract(text, "just/ci.just")) != expected:
            return len(native_fast_cases), label
    return len(native_fast_cases), None


def _selftest_remote_ci() -> tuple[int, str | None]:
    """Exercise the remote isolation contract in both directions."""
    valid = f"""remote_name={{{{ quote(name) }}}}
remote_host={{{{ quote(host) }}}}
remote_shell="x"
read -r -a remote_ssh <<<"$(./.venv/bin/python3 scripts/dev/fleet.py ssh-target "$remote_host")"
remote_tar="${{remote_shell%/bin/bash -s}}ionice -c3 /usr/bin/tar"
printf -v remote_gate_arg '%q' "$remote_name"
remote_prepare="git init -q && git add --all && {REMOTE_CI_SNAPSHOT_COMMIT} && \\
    $remote_history_environment {REMOTE_CI_HISTORY_COMMIT}"
if [[ "$remote_shell" == wsl* ]]; then
    remote_profile="/etc/profile.d/ra8-dev-slice.sh"
    remote_launcher="/usr/local/bin/ra8-dev"
    if [[ "$remote_name" == "__full__" ]]; then
        gate_command="/bin/bash -p scripts/ci.sh"
    else
        gate_command="/bin/bash -p scripts/ci.sh --gate $remote_gate_arg --container"
    fi
else
    remote_jobs="${{RA8_REMOTE_MAX_JOBS:-2}}"
    if [[ ! "$remote_jobs" =~ ^[1-9][0-9]*$ ]]; then
        exit 2
    fi
    remote_launcher="env RA8_MAX_JOBS=$remote_jobs CMAKE_BUILD_PARALLEL_LEVEL=$remote_jobs \\
        nice -n 19 ionice -c3"
    if [[ "$remote_name" == "__full__" ]]; then
        gate_command="/bin/bash -p scripts/ci.sh --native"
    else
        gate_command="/bin/bash -p scripts/ci.sh --gate $remote_gate_arg"
    fi
fi

if ! printf
    printf 'source %q\\n' "$remote_profile"
    printf 'exec %s %s\\n' "$remote_launcher" "$gate_command"
    """
    valid = "\n".join(f"    {line}" for line in valid.splitlines())
    remote_cases = (
        (valid, False, "isolated WSL transport stays valid"),
        (valid.replace("--container", ""), True, "native WSL gate fires"),
        (valid.replace(REMOTE_CI_SNAPSHOT_COMMIT, "", 1), True, "missing transport HEAD fires"),
        (valid.replace(REMOTE_CI_HISTORY_COMMIT, "", 1), True, "missing candidate metadata fires"),
        (valid.replace("nice -n 19 ionice -c3", "", 1), True, "unthrottled Linux gate fires"),
        (
            valid.replace('if [[ ! "$remote_jobs" =~ ^[1-9][0-9]*$ ]]; then', "", 1),
            True,
            "unvalidated job cap fires",
        ),
        (valid + '\n    echo "{{ name }} {{ host }}"', True, "raw recipe argument fires"),
    )
    for text, expected, label in remote_cases:
        if bool(check_remote_ci_contract(text, "just/ci_remote.just")) != expected:
            return len(remote_cases), label
    return len(remote_cases), None


def selftest() -> int:
    """Run internal selftest."""
    total = 0
    for run_cases in (
        _selftest_build_default,
        _selftest_nested_just,
        _selftest_ci_driver,
        _selftest_native_fast,
        _selftest_remote_ci,
    ):
        count, failure = run_cases()
        total += count
        if failure is not None:
            print(f"selftest: check_justfiles.py FAIL: {failure}", file=sys.stderr)
            return 1
    print(f"selftest: check_justfiles.py OK ({total} both-direction cases)")
    return 0


def main() -> int:
    """Check justfiles in the repository."""
    parser = argparse.ArgumentParser(description="Check justfiles in the repository")
    parser.add_argument("--list-files", action="store_true", help="List all scanned justfiles")
    parser.add_argument("--check", action="store_true", help="Check justfiles (structural)")
    parser.add_argument("--selftest", action="store_true", help="Run internal selftest")
    parser.add_argument("paths", nargs="*", help="Optional specific paths to check")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    files = [Path(p).resolve() for p in args.paths] if args.paths else find_justfiles()

    if args.list_files:
        for f in files:
            print(f.relative_to(REPO_ROOT))
        return 0

    findings: list[str] = []
    for f in files:
        findings.extend(check_file(f))

    if findings:
        for finding in findings:
            sys.stderr.write(f"{finding}\n")
        return 1

    print(f"Justfiles clean ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

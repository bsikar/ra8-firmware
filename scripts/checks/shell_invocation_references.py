# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact reviewed non-command occurrences for the shell caller authority."""

from __future__ import annotations

from dataclasses import dataclass

PROCESS_CALLS = frozenset(
    {
        "asyncio.create_subprocess_exec",
        "asyncio.create_subprocess_shell",
        "os.execv",
        "os.execve",
        "os.execvp",
        "os.execvpe",
        "run_process",
        "subprocess.Popen",
        "subprocess.call",
        "subprocess.check_call",
        "subprocess.check_output",
        "subprocess.run",
    }
)
SHELL_FENCE_LANGUAGES = frozenset({"bash", "console", "sh", "shell", "zsh"})
JSON_FENCE_LANGUAGES = frozenset({"json", "jsonc"})
YAML_COMMAND_KEYS = frozenset({"cmd", "command", "run", "shell"})
DOCKER_COMMANDS = frozenset({"CMD", "ENTRYPOINT", "RUN"})


@dataclass(frozen=True)
class ExactReference:
    """One reviewed non-executable or structurally nested governed occurrence."""

    rel: str
    target: str
    logical: str
    reason: str


EXACT_REFERENCES = (
    ExactReference(
        "scripts/ci/gates/tests.sh",
        "scripts/dev/agent_workspace.sh",
        "/bin/bash -p -n scripts/dev/agent_workspace.sh",
        "Bash parses this file without executing it; the executable caller is separately checked.",
    ),
    ExactReference(
        "scripts/emu/smoke.sh",
        "scripts/emu/smoke_run.sh",
        'if ! grep -q "^$fn() {" "$ROOT/scripts/emu/smoke_run.sh"; then',
        "The smoke selftest reads function declarations as data and never launches the helper.",
    ),
    ExactReference(
        "scripts/emu/smoke.sh",
        "scripts/emu/smoke_apps.sh",
        (
            "ov_apps=\"$(awk '/^uart_expect_override\\(\\)/{f=1} "
            'f&&/^}/{f=0} f\' "$ROOT/scripts/emu/smoke_apps.sh" | '
            "grep -oE '^[[:space:]]+[a-z0-9_]+\\)' | tr -d ' )')\""
        ),
        "The smoke selftest parses a case table as data and never launches the helper.",
    ),
    ExactReference(
        "scripts/git/hook-launcher",
        "scripts/git/pre-commit",
        (
            "exec env -u BASH_ENV -u ENV -u PYTHONHOME -u PYTHONPATH "
            '"$bash_bin" -p "$owner" "${hook_args[@]}"'
        ),
        (
            "The launcher verifies Bash and the immutable hook owner by exact "
            "path, blob, and inode first."
        ),
    ),
    ExactReference(
        "scripts/ci/lib/container.sh",
        "scripts/ci.sh",
        (
            'exec "${runtime[@]}" run --rm --init -u 0:0 '
            "--tmpfs /home/ra8-ci:rw,mode=0700 -e RA8_CI_INNER=1 "
            '-e RA8_CI_FAST="$fast" -e RA8_CI_GATE="$gate" '
            "-e HOME=/home/ra8-ci "
            "-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory "
            "-e GIT_CONFIG_VALUE_0=/workspace "
            "-e RA8_MAX_JOBS "
            '-e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-'
            '${RA8_MAX_JOBS:-}}" -v "$repo":/workspace:ro '
            '${extra[@]+"${extra[@]}"} ${worktree[@]+"${worktree[@]}"} '
            '${ccache[@]+"${ccache[@]}"} ${toolcache[@]+"${toolcache[@]}"} '
            '"${nofile[@]}" -w /workspace "$image" /bin/bash -p scripts/ci.sh'
        ),
        (
            "The reviewed container-runtime argv executes the exact privileged "
            "Bash prefix only after the selected pinned image."
        ),
    ),
)

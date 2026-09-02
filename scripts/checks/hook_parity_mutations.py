# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural mutation fixtures for hook control-plane parity self-tests."""

from __future__ import annotations


def _owner_mutations(
    pre_commit: str, hooks: str, ci_script: str, gate_sources: tuple[str, ...]
) -> tuple[tuple[str, str, str, tuple[str, ...]], ...]:
    """Return mutations of the self-contained owner bootstrap."""
    return (
        (
            pre_commit.replace("set -euo pipefail\n", "set -euo pipefail\nexit 0\n", 1),
            hooks,
            ci_script,
            gate_sources,
        ),
        (
            pre_commit.replace("capture_source\n", "exit 0\n  capture_source\n", 1),
            hooks,
            ci_script,
            gate_sources,
        ),
        (
            pre_commit.replace(
                'PATH="$RA8_OWNER_PATH"',
                'PATH="$SOURCE_ROOT/.venv/bin:$PATH"',
                1,
            ),
            hooks,
            ci_script,
            gate_sources,
        ),
        (
            pre_commit.replace("start_new_session=True", "start_new_session=False", 1),
            hooks,
            ci_script,
            gate_sources,
        ),
        (
            pre_commit.replace("os.killpg(process.pid, signum)", "pass", 1),
            hooks,
            ci_script,
            gate_sources,
        ),
        (pre_commit.replace('wait "$pid"', "true", 1), hooks, ci_script, gate_sources),
        (
            pre_commit.replace("verify_source_unchanged", "true", 1),
            hooks,
            ci_script,
            gate_sources,
        ),
        (
            pre_commit.replace('"$OWNER_JUST" \\\n', '"$OWNER_BASH" \\\n    "$OWNER_JUST" \\\n', 1),
            hooks,
            ci_script,
            gate_sources,
        ),
    )


def _hook_mutations(
    pre_commit: str, hooks: str, ci_script: str, gate_sources: tuple[str, ...]
) -> tuple[tuple[str, str, str, tuple[str, ...]], ...]:
    """Return candidate early-success, dead-loop, and ordering mutations."""
    return (
        (
            pre_commit,
            hooks.replace("#!/bin/bash -p", "#!/usr/bin/env bash", 1),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace("#!/bin/bash -p", "#!/usr/bin/env bash"),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace("    gates=(", "    exit 0\n    gates=(", 1),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace(
                "    gates=(",
                "    python3 scripts/git/write-proof.py /tmp/forged\n    gates=(",
                1,
            ),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace('        run_gate "$gate"', "        true", 1),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace(
                '    for gate in "${gates[@]}"; do\n        run_gate "$gate"\n    done',
                '    if false; then\n        for gate in "${gates[@]}"; do\n'
                '            run_gate "$gate"\n        done\n    fi',
                1,
            ),
            ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks.replace(
                "        ascii\n        copyright", "        copyright\n        ascii", 1
            ),
            ci_script,
            gate_sources,
        ),
    )


def _ci_mutations(
    pre_commit: str, hooks: str, ci_script: str, gate_sources: tuple[str, ...]
) -> tuple[tuple[str, str, str, tuple[str, ...]], ...]:
    """Return early dispatch and candidate proof-capability mutations."""
    return (
        (
            pre_commit,
            hooks,
            ci_script.replace("set -euo pipefail\n", "set -euo pipefail\nexit 0\n", 1),
            gate_sources,
        ),
        (
            pre_commit,
            hooks,
            "if [[ $1 == --staged-hook ]]; then exit 0; fi\n" + ci_script,
            gate_sources,
        ),
        (
            pre_commit,
            hooks,
            ci_script.replace('run_gate_capture "$gate"', "RA8_GATE_RC=0", 1),
            gate_sources,
        ),
        (
            pre_commit,
            hooks,
            "RA8_STAGED_GATE_PROOF=/tmp/forged\n" + ci_script,
            gate_sources,
        ),
    )


def mutation_cases(
    pre_commit: str, hooks: str, ci_script: str, gate_sources: tuple[str, ...]
) -> tuple[tuple[str, str, str, tuple[str, ...]], ...]:
    """Return every historical early-success bypass and proof mutation."""
    common = (pre_commit, hooks, ci_script, gate_sources)
    gate_exit = ("exit 0\n" + gate_sources[0], *gate_sources[1:])
    return (
        *_owner_mutations(*common),
        *_hook_mutations(*common),
        *_ci_mutations(*common),
        (pre_commit, hooks, ci_script, gate_exit),
    )

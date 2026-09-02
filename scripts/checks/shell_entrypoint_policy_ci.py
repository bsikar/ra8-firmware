# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Raw typed-policy rows for the CI shell domain."""

from __future__ import annotations

from typing import TypeAlias

ShellPolicyRow: TypeAlias = tuple[str, str, str, str, bool, bool]

CI_POLICY_ROWS: tuple[ShellPolicyRow, ...] = (
    ("scripts/ci.sh", "privileged", "entry", "bash", True, False),
    ("scripts/ci/devcontainer_image.sh", "privileged", "entry", "bash", True, False),
    (
        "scripts/ci/devcontainer_image_lock_receipts.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    (
        "scripts/ci/devcontainer_image_lock_selftest.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    (
        "scripts/ci/devcontainer_image_selftest.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    (
        "scripts/ci/devcontainer_image_bound_exit_selftest.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    (
        "scripts/ci/devcontainer_image_selftest_cases.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    (
        "scripts/ci/devcontainer_image_signal_selftest.bash",
        "privileged",
        "sourced-only",
        "bash",
        False,
        True,
    ),
    ("scripts/ci/devcontainer_run.sh", "portable", "entry", "bash", True, False),
    ("scripts/ci/fleet_capacity.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/analysis.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/build.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/checks.sh", "portable", "entry", "bash", True, False),
    ("scripts/ci/gates/emu.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/hygiene.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/lint.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/manual.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/gates/tests.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/install_unicorn.sh", "privileged", "entry", "bash", True, False),
    ("scripts/ci/lib/abort.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/lib/arm_toolchain.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/lib/container.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/lib/nofile.sh", "portable", "entry", "bash", True, False),
    ("scripts/ci/lib/parallelism.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/lib/snapshot.sh", "portable", "sourced-only", "bash", False, False),
    ("scripts/ci/lib/tool_env.sh", "portable", "dual-use", "bash", False, False),
    ("scripts/ci/monitor.sh", "privileged", "entry", "bash", True, False),
    ("scripts/ci/test-docker.sh", "portable", "entry", "bash", True, False),
    ("scripts/ci/unicorn_pin.sh", "portable", "sourced-only", "bash", False, False),
)

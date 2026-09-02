# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Typed authority for every first-party shell entry point.

This table is intentionally exhaustive. The shebang gate compares it with the
repository's canonical shell census, so a new shell file cannot inherit any
policy by naming convention. Startup security, entry/source usage, dialect,
and executable mode are independent axes. Security-pinned entries use the
canonical combined preamble: shebang, SPDX, copyright, then the exact
``SHEBANG-SECURITY`` rationale. A privileged sourced-only helper,
for example, must keep its protected header but can only be sourced by a
privileged parent; it never becomes a launchable privileged entry merely
because it uses the protected header.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

from shell_entrypoint_policy_ci import CI_POLICY_ROWS
from shell_entrypoint_policy_hil import HIL_POLICY_ROWS, ShellPolicyRow


class ShellSecurity(StrEnum):
    """Startup trust required by a shell file."""

    PORTABLE = "portable"
    PRIVILEGED = "privileged"


class ShellUsage(StrEnum):
    """Whether a file is an entry point, a sourced helper, or both."""

    ENTRY = "entry"
    SOURCED_ONLY = "sourced-only"
    DUAL_USE = "dual-use"


class ShellDialect(StrEnum):
    """Exact interpreter dialect for portable headers."""

    BASH = "bash"
    POSIX_SH = "posix-sh"


class DuplicateShellPolicyError(ValueError):
    """A path appeared in more than one typed policy domain."""


@dataclass(frozen=True)
class ShellPolicy:
    """One shell path's independent startup, usage, dialect, and mode contract."""

    security: ShellSecurity
    usage: ShellUsage
    dialect: ShellDialect
    executable: bool
    source_requires_privileged_parent: bool


PRIVILEGED_SHEBANG = "#!/bin/bash -p"
PRIVILEGED_REASON = (
    "# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection."
)
PORTABLE_SHEBANG = "#!/usr/bin/env bash"
PORTABLE_SH_SHEBANG = "#!/usr/bin/env sh"

_BASE_SHELL_POLICIES: dict[str, ShellPolicy] = {
    "apps/host/mdl/tests/http_integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "apps/host/mdl/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "coprocessor/esp32c6/build.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "coprocessor/esp32c6/flash.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_handoff_gate.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_render_gate.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/scripts/build_payload.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    (
        "examples/ek_ra8d2/hw_validated/hil/glcdc_render/tests/scripts/ra8_emulator_fb_crc.sh"
    ): ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "infra/bootstrap.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "infra/network/ap_openwrt.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "infra/network/verify_bench_wifi.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/all_examples.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/books.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/build_host_tools.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/build_shared_libs.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/docs.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/host_cmake.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/init_fuzz_corpora.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/lib/app_batch.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/provision_doxygen.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/publish_docs.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/builders/select_host_compiler.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/check_nsc_cmse.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/check_stack_usage.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/check_unicorn_version.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/clang_tidy.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/cppcheck.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/format_code.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/iwyu.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/lint_selftest.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/misra_check.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/misra_check_inner.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/misra/selftest.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/osv_scan.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/run_fuzz.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/scan_build.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/collect.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/compile_db.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/invoke.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/pass_args.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/passes.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/checks/tidy/selftest.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/agent_workspace.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/agent_workspace_selftest.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/debug.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/exfat_macos_interop.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/flash.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/git_environment.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=True,
    ),
    "scripts/dev/hil_cache_repair.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/infra.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/monitor.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/openocd_debug.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/openocd_flash.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/ozone.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/provision_dev_box_toolchain.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/provision_dev_box_toolchain_selftest.bash": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=True,
    ),
    "scripts/dev/remote_gdb_server.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/run_just.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/setup_ansible.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/dev/setup_python.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/eil_all.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/emu_fixtures.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/matrix.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/matrix_triage.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/setup_macos.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/smoke.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/smoke_apps.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/smoke_assert.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/emu/smoke_run.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "scripts/gen/build_chapter_map.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/gen/gen_ra8_media_proto.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/commit-msg": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/github_askpass.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/hook-launcher": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/install-hooks.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/post-checkout": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/post-commit": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/post-merge": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/pre-commit": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/git/pre-push": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/report/mcdc_report.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/report/tree_coverage.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/secrets/openbao_configure.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/secrets/openbao_unseal.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "scripts/secrets/rot_provision.sh": ShellPolicy(
        ShellSecurity.PRIVILEGED,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tests/build_tests.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tests/fixtures/epub/run_probe.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tests/run_tests.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/exfat_mkimage/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/glyph_bench/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "tools/mkbookimg/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/mkfontimg/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_imagepack/tests/check_production_surface.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_imagepack/tests/convert_integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_imagepack/tests/rabook_inspect_integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_imagepack/tests/verify_integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.POSIX_SH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_viewer/tests/run_corpus.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
    "tools/rabook_viewer/tests/run_workspace_test.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.SOURCED_ONLY,
        ShellDialect.BASH,
        executable=False,
        source_requires_privileged_parent=False,
    ),
    "tools/reader_vmem/tests/integration.sh": ShellPolicy(
        ShellSecurity.PORTABLE,
        ShellUsage.ENTRY,
        ShellDialect.BASH,
        executable=True,
        source_requires_privileged_parent=False,
    ),
}


def _policy_from_row(row: ShellPolicyRow) -> tuple[str, ShellPolicy]:
    """Convert one compact domain row into the canonical typed value."""
    path, security, usage, dialect, executable, privileged_parent = row
    return path, ShellPolicy(
        ShellSecurity(security),
        ShellUsage(usage),
        ShellDialect(dialect),
        executable=executable,
        source_requires_privileged_parent=privileged_parent,
    )


def merge_policy_tables(
    base: dict[str, ShellPolicy],
    *domains: tuple[ShellPolicyRow, ...],
) -> dict[str, ShellPolicy]:
    """Merge independently reviewable domains, rejecting duplicate authority."""
    merged = dict(base)
    for domain in domains:
        for row in domain:
            path, policy = _policy_from_row(row)
            if path in merged:
                raise DuplicateShellPolicyError(path)
            merged[path] = policy
    return merged


SHELL_POLICIES = merge_policy_tables(_BASE_SHELL_POLICIES, CI_POLICY_ROWS, HIL_POLICY_ROWS)

PRIVILEGED_PATHS = frozenset(
    path for path, policy in SHELL_POLICIES.items() if policy.security is ShellSecurity.PRIVILEGED
)
SOURCED_ONLY_PATHS = frozenset(
    path for path, policy in SHELL_POLICIES.items() if policy.usage is ShellUsage.SOURCED_ONLY
)

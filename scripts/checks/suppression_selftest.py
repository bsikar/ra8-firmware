# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction fixtures for the suppression inventory scanner."""

from __future__ import annotations

import tempfile
from pathlib import Path

from selftest_assert import expect, report
from suppression_build_controls import compiler_records
from suppression_catalog import REQUIRED_FAMILIES
from suppression_compiler_selftest import (
    assert_clang_tidy_config_fail_closed,
    assert_compiler_controls,
    assert_compiler_tool_probes,
)
from suppression_debt_selftest import assert_baseline_ceiling_controls
from suppression_governance_selftest import assert_governance_parsers
from suppression_identity_selftest import assert_identity_semantics
from suppression_ledger_selftest import assert_ledger_gate
from suppression_mcdc_selftest import assert_mcdc_macro_binding
from suppression_model import Inventory, Suppression
from suppression_scan import decode_git_paths, scan_paths
from suppression_selftest_fixtures import FIXTURES
from suppression_selftest_integrity import assert_identity_and_structure
from suppression_stranded_selftest import (
    assert_stranded_branch_markers,
    assert_stranded_line_markers,
)
from suppression_tool_selftest import assert_tool_control_syntax_awareness

EXPECTED_CLANG_ROWS = 14
EXPECTED_BROAD_NOLINT_ROWS = 3
EXPECTED_OPTIONAL_INACTIVE = 2
EXPECTED_CENTRAL_ROWS = 4
EXPECTED_BUILD_ROWS = 37
EXPECTED_PROJECT_ROWS = 3
EXPECTED_PYTHON_REGION_ROWS = 2
EXPECTED_RUFF_CONFIG_ROWS = 3
EXPECTED_SHELLCHECK_ROWS = 11
EXPECTED_SHELLCHECK_GLOBAL_ROWS = 4
EXPECTED_ACTIVE_SHELL_STATUS_ROWS = 4
EXPECTED_EMBEDDED_SHELL_STATUS_ROWS = 2
EXPECTED_JUST_SHELL_STATUS_ROWS = 2
EXPECTED_YAML_SHELL_STATUS_ROWS = 5
EXPECTED_ANSIBLE_CONTROL_ROWS = 4
EXPECTED_CTEST_CONTROL_ROWS = 3
EXPECTED_PYTHON_CONTROL_ROWS = 13
EXPECTED_WORKFLOW_CONTROL_ROWS = 3
EXPECTED_MALFORMED_NATIVE_SKIP_ROWS = 2
EXPECTED_BASELINE_ROWS = 4
EXPECTED_MCDC_ROWS = 7
EXPECTED_NATIVE_SKIP_ROWS = 3
EXPECTED_COVERAGE_MASK_ROWS = 3
EXPECTED_YAML_BUILD_ROWS = 8
EXPECTED_CMAKE_WNO_DEV_ROWS = 8
EXPECTED_COMPILER_WNO_DEV_ROWS = 3
SHELL_ARGUMENT_TOOL_LINE = 6
EXPECTED_DEAD_ANCHOR_FINDINGS = 2


def _write_fixture(root: Path) -> list[str]:
    """Write the deterministic ASCII fixture and return its relative paths."""
    for rel, text in FIXTURES.items():
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="ascii")
    return sorted(FIXTURES)


def _assert_families(inventory: Inventory, failures: list[str]) -> None:
    """Assert every phase-one recognizer fires through the production scanner."""
    seen = {item.family for item in inventory.suppressions}
    repository_bound = {
        "ansible-lint-config",
        "checker-nonfatal-control",
        "checker-scope-control",
        "ci-parity-exemption",
        "documentation-control",
        "generated-artifact",
        "other-language-control",
        "security-analysis-control",
    }
    for family in sorted(REQUIRED_FAMILIES - repository_bound):
        expect(family in seen, f"must fire: {family}", failures)


def _assert_python_syntax_awareness(inventory: Inventory, failures: list[str]) -> None:
    """Assert Python controls fire only in active comments and carry reasons."""
    python_rows = [item for item in inventory.suppressions if item.path == "sample.py"]
    python_rules = {item.rule for item in python_rows}
    expect(
        {"S603", "S607"} <= python_rules,
        "must fire: comma-separated noqa rules become separate rows",
        failures,
    )
    tools = {item.tool for item in python_rows}
    expect(
        {"bandit", "coverage.py", "mypy", "pylint", "pyright", "ruff-format"} <= tools,
        "must fire: every Python analyzer, formatter, and coverage control",
        failures,
    )
    expect(
        not any("blank-reason" in item.concerns for item in python_rows),
        "quiet: every supported Python fixture carries a local reason",
        failures,
    )
    bare_rows = [item for item in inventory.suppressions if item.path == "bare_python_controls.py"]
    expect(
        all("blank-reason" in item.concerns for item in bare_rows),
        "must fire: bare Python controls retain blank-reason concerns",
        failures,
    )
    expect(
        len([item for item in python_rows if item.tool == "coverage.py"]) == 1
        and len([item for item in python_rows if item.tool == "pylint"])
        == EXPECTED_PYTHON_REGION_ROWS
        and len([item for item in python_rows if item.tool == "ruff-format"])
        == EXPECTED_PYTHON_REGION_ROWS,
        "quiet: directive-looking Python strings do not add inventory rows",
        failures,
    )


def _assert_control_syntax_awareness(inventory: Inventory, failures: list[str]) -> None:
    """Assert test and infrastructure controls use active language syntax."""
    python_rows = [item for item in inventory.suppressions if item.path == "controls.py"]
    expect(
        len(python_rows) == EXPECTED_PYTHON_CONTROL_ROWS,
        "must fire: Python skip and strict-xfail calls/decorators",
        failures,
    )
    expect(
        len([item for item in python_rows if "non-strict-xfail" in item.concerns]) == 1,
        "must fire: permissive marker xfail is distinct from strict/runtime xfail",
        failures,
    )
    ctest_rows = [item for item in inventory.suppressions if item.family == "ctest"]
    expect(
        len(ctest_rows) == EXPECTED_CTEST_CONTROL_ROWS,
        "must fire: active CTest fail, disabled, and skip properties",
        failures,
    )
    expect(
        not any("blank-reason" in item.concerns for item in ctest_rows),
        "quiet: getters, strings, and comments are not CTest controls",
        failures,
    )
    workflow_rows = [item for item in inventory.suppressions if item.family == "workflow"]
    expect(
        len(workflow_rows) == EXPECTED_WORKFLOW_CONTROL_ROWS,
        "must fire: workflow soft failure and both missing-artifact modes",
        failures,
    )
    ansible_rows = [item for item in inventory.suppressions if item.family == "ansible"]
    expect(
        len(ansible_rows) == EXPECTED_ANSIBLE_CONTROL_ROWS,
        "must fire: every requested active Ansible result/log control",
        failures,
    )
    expect(
        not any("blank-reason" in item.concerns for item in workflow_rows + ansible_rows),
        "quiet: YAML block payloads and explicit false controls stay inactive",
        failures,
    )


def _assert_global_shellcheck_controls(
    inventory: Inventory, shellcheck_rows: list[Suppression], failures: list[str]
) -> None:
    """Assert local and repository-wide ShellCheck controls are distinct."""
    expect(
        len(shellcheck_rows) == EXPECTED_SHELLCHECK_ROWS,
        "quiet: quoted strings and heredoc payloads are not ShellCheck controls",
        failures,
    )
    controls = {item.directive for item in shellcheck_rows}
    expect(
        {
            "shellcheck enable",
            "shellcheck external-sources",
            "shellcheck shell",
            "shellcheck source",
            "shellcheck source-path",
        }
        <= controls,
        "must fire: every supported ShellCheck analysis control",
        failures,
    )
    global_rows = [
        item
        for item in inventory.suppressions
        if item.directive in {"SHELLCHECK_OPTS exclude", ".shellcheckrc exclude"}
    ]
    expect(
        len(global_rows) == EXPECTED_SHELLCHECK_GLOBAL_ROWS
        and not any(item.concerns for item in global_rows),
        "must fire: reasoned global ShellCheck exclusions, but not quoted lookalikes",
        failures,
    )


def _assert_shell_syntax_awareness(inventory: Inventory, failures: list[str]) -> None:
    """Assert shell controls fire only where their syntax is executable."""
    shell_rows = [item for item in inventory.suppressions if item.path == "sample.sh"]
    shellcheck_rows = [item for item in shell_rows if item.family == "shellcheck"]
    _assert_global_shellcheck_controls(inventory, shellcheck_rows, failures)
    status_rows = [item for item in shell_rows if item.family == "shell-status"]
    active_status = [item for item in status_rows if item.scope == "command-list"]
    embedded_status = [item for item in status_rows if item.scope == "embedded-shell-or-heredoc"]
    expect(
        len(active_status) == EXPECTED_ACTIVE_SHELL_STATUS_ROWS,
        "must fire: direct and multiline masks while strings stay inactive",
        failures,
    )
    expect(
        len(embedded_status) == EXPECTED_EMBEDDED_SHELL_STATUS_ROWS,
        "must fire: quoted and heredoc masks remain visible for embedded-shell review",
        failures,
    )
    expect(
        active_status[0].reason == "fixture cleanup status is intentionally ignored",
        "must fire: active status mask carries its same-line rationale",
        failures,
    )
    just_status = [
        item
        for item in inventory.suppressions
        if item.path == "just/sample.just" and item.family == "shell-status"
    ]
    expect(
        len(just_status) == EXPECTED_JUST_SHELL_STATUS_ROWS
        and all(item.scope == "command-list" for item in just_status),
        "must fire: Just recipe shell masks are inventoried as active syntax",
        failures,
    )


def _assert_debt_control_families(inventory: Inventory, failures: list[str]) -> None:
    """Assert the four newly-supported debt/control grammars in both directions."""
    baseline_rows = [item for item in inventory.suppressions if item.family == "baseline-ratchet"]
    expect(
        len(baseline_rows) == EXPECTED_BASELINE_ROWS,
        "must fire: exact baseline schemas become source-located rows",
        failures,
    )
    codes = {item.code for item in inventory.findings}
    expect(
        {
            "duplicate-baseline-row",
            "stale-baseline-path",
            "malformed-baseline-row",
            "unknown-baseline-file",
            "baseline-growth",
        }
        <= codes,
        "must fire: duplicate, malformed, unknown, growing, and path-missing baseline debt",
        failures,
    )
    mcdc_rows = [item for item in inventory.suppressions if item.family == "mcdc-deactivation"]
    expect(
        len(mcdc_rows) == EXPECTED_MCDC_ROWS
        and any(item.scope == "function" for item in mcdc_rows),
        "must fire: exact comment and literal macro MC/DC grammars",
        failures,
    )
    expect(
        any(item.scope.startswith("decision-line:") for item in mcdc_rows),
        "must fire: a reasoned MC/DC marker pairs to one compound decision",
        failures,
    )
    expect(
        {
            "malformed-mcdc-deactivation",
            "duplicate-mcdc-deactivation",
            "unpaired-mcdc-deactivation",
            "unknown-mcdc-macro",
            "malformed-mcdc-macro",
        }
        <= codes,
        "must fire: malformed, duplicate, unpaired, and unknown MC/DC controls",
        failures,
    )
    assert_mcdc_macro_binding(inventory, failures)
    _assert_native_coverage_controls(inventory, failures)


def _assert_native_coverage_controls(inventory: Inventory, failures: list[str]) -> None:
    """Assert native-test skips and gcovr masks fire only on active syntax."""
    native_rows = [item for item in inventory.suppressions if item.path == "native_test.cpp"]
    expect(
        len(native_rows) == EXPECTED_NATIVE_SKIP_ROWS
        and len([item for item in native_rows if "blank-reason" in item.concerns]) == 1,
        "must fire: GTest and both Unity skip forms while strings stay quiet",
        failures,
    )
    malformed_skips = [
        item for item in inventory.findings if item.code == "malformed-native-test-skip"
    ]
    expect(
        len(malformed_skips) == EXPECTED_MALFORMED_NATIVE_SKIP_ROWS,
        "must fire: malformed GTest and Unity skip calls",
        failures,
    )
    coverage_rows = [item for item in inventory.suppressions if item.family == "coverage-mask"]
    expect(
        len(coverage_rows) == EXPECTED_COVERAGE_MASK_ROWS
        and not any(item.concerns for item in coverage_rows),
        "must fire: active shell and gcovr.cfg masks while inactive values stay quiet",
        failures,
    )
    config_rules = {item.rule for item in coverage_rows if item.path == "config/gcovr.cfg"}
    expect(
        config_rules
        == {"gcov-ignore-parse-errors=negative_hits.warn", "exclude-unreachable-branches"},
        "quiet: false gcovr.cfg booleans do not become active masks",
        failures,
    )
    codes = {item.code for item in inventory.findings}
    expect(
        {"duplicate-coverage-mask", "malformed-coverage-mask"} <= codes,
        "must fire: duplicate and invalid gcovr.cfg controls fail closed",
        failures,
    )
    expect(
        any(
            item.code == "malformed-coverage-mask" and item.path == "invalid/gcovr.cfg"
            for item in inventory.findings
        )
        and any(
            item.code == "duplicate-coverage-mask" and item.path == "config/gcovr.cfg"
            for item in inventory.findings
        ),
        "must fire: gcovr.cfg invalid and duplicate branches are both exercised",
        failures,
    )


def _assert_syntax_awareness(root: Path, inventory: Inventory, failures: list[str]) -> None:
    """Assert directive-looking strings and prose stay outside the inventory."""
    c_rows = [item for item in inventory.suppressions if item.path == "sample.c"]
    clang_rows = [item for item in c_rows if item.family == "clang-tidy"]
    expect(
        len(clang_rows) == EXPECTED_CLANG_ROWS,
        "must fire: clang-tidy raw-source NOLINT semantics are inventoried",
        failures,
    )
    clang_rules = {item.rule for item in clang_rows}
    expect(
        {
            "raw_token",
            "raw_spliced_token",
            "readability-magic-numbers",
            "readability/fn_size",
        }
        <= clang_rules,
        "must fire: strings, identifiers, splices, and live vendor NOLINT forms",
        failures,
    )
    expect(
        len([item for item in clang_rows if item.rule == "*"]) == EXPECTED_BROAD_NOLINT_ROWS,
        "must fire: bare and bracket NOLINT forms follow clang-tidy semantics",
        failures,
    )
    _assert_shell_syntax_awareness(inventory, failures)
    prose_rows = [item for item in inventory.suppressions if item.path == "policy.md"]
    expect(
        len(prose_rows) == EXPECTED_PROJECT_ROWS,
        "quiet: prose, fenced, and inline-code directive examples are ignored",
        failures,
    )
    bare = [item for item in prose_rows if item.rule == "MAGIC-OK"]
    expect(
        bool(bare and "blank-reason" in bare[0].concerns),
        "must fire: bare project marker has a blank reason",
        failures,
    )
    _assert_python_syntax_awareness(inventory, failures)
    cpplint = [item for item in c_rows if item.rule == "whitespace/line_length"]
    expect(
        bool(cpplint and cpplint[0].tool == "cpplint"),
        "quiet: cpplint slash rule is classified without an unknown finding",
        failures,
    )
    splice = [item for item in c_rows if item.rule == "readability-redundant-string-init"]
    expect(bool(splice), "must fire: directive in a spliced C line comment", failures)
    _assert_control_syntax_awareness(inventory, failures)
    assert_tool_control_syntax_awareness(root, inventory, failures)


def _assert_integrity_findings(inventory: Inventory, failures: list[str]) -> None:
    """Assert unknown and duplicate syntax fail closed instead of disappearing."""
    codes = [item.code for item in inventory.findings]
    expect(
        not any(item.code == "unsupported-category" for item in inventory.findings),
        "quiet: every formerly unsupported class has a typed recognizer",
        failures,
    )
    expect("unknown-directive" in codes, "must fire: unknown project marker", failures)
    expect(
        "unterminated-html-comment" in codes,
        "must fire: unterminated multiline HTML comment",
        failures,
    )
    expect("malformed-heredoc" in codes, "must fire: empty heredoc delimiter", failures)
    expect("unterminated-heredoc" in codes, "must fire: unterminated heredoc body", failures)
    expect(
        "unterminated-shell-quote" in codes,
        "must fire: unterminated multiline shell quote",
        failures,
    )
    expect(
        "unterminated-line-comment-splice" in codes,
        "must fire: C line-comment splice reaching EOF",
        failures,
    )
    malformed = [item for item in inventory.findings if item.path == "malformed.py"]
    malformed_messages = {item.message for item in malformed}
    expect(
        "noqa: F401 E402" in malformed_messages,
        "must fire: whitespace-separated multi-rule noqa",
        failures,
    )
    for directive in (
        "pragma: no cover because fixture",
        "pylint: disable",
        "mypy: disable-error-code",
        "pyright: nonsense",
        "bandit: skip=",
        "fmt: sideways",
        "ruff: noqa: E402 F401",
    ):
        expect(
            directive in malformed_messages,
            f"must fire: malformed Python control {directive}",
            failures,
        )
    malformed_nolint = [item for item in inventory.findings if "NOLINT(foo,,bar)" in item.message]
    expect(bool(malformed_nolint), "must fire: empty NOLINT rule ID", failures)
    expect("duplicate-directive" in codes, "must fire: duplicate central waiver", failures)
    malformed_cppcheck = [
        item for item in inventory.findings if item.code == "malformed-cppcheck-list"
    ]
    expect(bool(malformed_cppcheck), "must fire: bogus cppcheck rule ID", failures)
    broad = [item for item in inventory.suppressions if item.rule == "-w"]
    expect(
        bool(broad and "broad-rule" in broad[0].concerns),
        "must fire: blanket compiler warning disable",
        failures,
    )


def _assert_yaml_and_ruff_config(inventory: Inventory, failures: list[str]) -> None:
    """Assert executable YAML blocks and central Ruff controls are distinct."""
    yaml_rows = [item for item in inventory.suppressions if item.path == "sample.yml"]
    yaml_shell_rows = [item for item in yaml_rows if item.family == "shell-status"]
    expect(
        len(yaml_shell_rows) == EXPECTED_YAML_SHELL_STATUS_ROWS
        and all(item.provenance == "yaml-shell-block" for item in yaml_shell_rows),
        "must fire: content, workflow run, and Ansible shell YAML payloads",
        failures,
    )
    syntax_rows = [
        item for item in yaml_rows if item.provenance not in {"yaml-shell-block", "build-config"}
    ]
    expect(
        len(syntax_rows) == 1,
        "quiet: non-executable YAML block payloads are not active syntax",
        failures,
    )
    expect(
        not any(item.path == "windows.yml" for item in inventory.findings),
        "quiet: PowerShell workflow blocks do not emit Bash parser findings",
        failures,
    )
    ruff_config = [
        item
        for item in inventory.suppressions
        if item.provenance == "central-config" and item.tool == "ruff"
    ]
    expect(
        len(ruff_config) == EXPECTED_RUFF_CONFIG_ROWS,
        "must fire: every active Ruff global/per-file/path waiver is inventoried",
        failures,
    )
    expect(
        not any(item.concerns for item in ruff_config),
        "quiet: Ruff central waivers have precise local reasons",
        failures,
    )


def _assert_regions_and_config(inventory: Inventory, failures: list[str]) -> None:
    """Assert region structure and repository-global waivers use the real model."""
    region_findings = [item for item in inventory.findings if "region" in item.code]
    expect(
        not any(item.path == "sample.c" for item in region_findings),
        "quiet: balanced regions are accepted",
        failures,
    )
    expect(
        any(item.path == "unmatched.c" for item in region_findings),
        "must fire: unmatched region end",
        failures,
    )
    unmatched_messages = " ".join(
        item.message for item in region_findings if item.path == "unmatched.c"
    )
    expect(
        "GCOVR_EXCL_BR" in unmatched_messages
        and "LCOV_EXCL" in unmatched_messages
        and "GCOVR_EXCL" in unmatched_messages,
        "must fire: coverage tool and BR/non-BR region identities do not cross-close",
        failures,
    )
    central = [item for item in inventory.suppressions if item.provenance == "central-list"]
    expect(
        len(central) == EXPECTED_CENTRAL_ROWS,
        "must fire: global cppcheck config is inventoried",
        failures,
    )
    reasons = {item.rule: item.reason for item in central}
    expect(
        "paired divider rationale" in reasons["unusedFunction"],
        "quiet: paired cppcheck section rationale is frozen",
        failures,
    )
    expect(
        "local rationale A" in reasons["unreadVariable"],
        "quiet: adjacent local rationale replaces grouped rationale",
        failures,
    )
    expect(
        "local rationale B" in reasons["unknownMacro"],
        "quiet: second adjacent local rationale does not accumulate",
        failures,
    )
    _assert_yaml_and_ruff_config(inventory, failures)
    _assert_build_config(inventory, failures)


def _assert_build_config(inventory: Inventory, failures: list[str]) -> None:
    """Dispatch the unchanged build-control assertion groups."""
    build = [item for item in inventory.suppressions if item.provenance == "build-config"]
    expect(
        len(build) == EXPECTED_BUILD_ROWS,
        "must fire: build-config warning controls are inventoried",
        failures,
    )
    _assert_yaml_build_config(inventory, failures)
    _assert_shell_path_build_config(build, failures)
    _assert_cmake_family_build_config(build, failures)
    _assert_cmake_dev_build_config(build, failures)
    _assert_build_reason_capture(failures)
    _assert_yaml_build_reason_capture(failures)


def _assert_build_reason_capture(failures: list[str]) -> None:
    """Assert warning controls retain only locally attached rationales."""
    source = """# Suppression rationale: pinned compiler emits a false positive.
target_compile_options(
    sample PRIVATE
    -Wno-shadow # ABI callback parameter cannot change
    -Wno-cast-align
)
# Unrelated prose must not approve a control.
target_compile_options(other PRIVATE -Wno-unused-parameter)
# Suppression rationale:
target_compile_options(empty PRIVATE -Wno-padded)
"""
    records = compiler_records("CMakeLists.txt", source)
    reasons = {item.rule: item.reason for item in records}
    expect(
        reasons.get("-Wno-shadow") == "ABI callback parameter cannot change"
        and reasons.get("-Wno-cast-align") == "pinned compiler emits a false positive.",
        "quiet: inline and command-level compiler reasons are retained",
        failures,
    )
    expect(
        not reasons.get("-Wno-unused-parameter") and not reasons.get("-Wno-padded"),
        "must fire: unrelated or empty comments do not become reasons",
        failures,
    )
    missing = {item.rule for item in records if "blank-reason" in item.concerns}
    expect(
        {"-Wno-unused-parameter", "-Wno-padded"} <= missing,
        "must fire: reasonless build controls retain blank-reason concerns",
        failures,
    )


def _assert_yaml_build_reason_capture(failures: list[str]) -> None:
    """Assert YAML command rationales bind locally in both block styles."""
    source = """steps:
  - run: |
      # Suppression rationale: pinned YAML compiler wrapper.
      clang -Wno-shadow sample.c
      clang -Wno-padded sample.c # generated ABI contract
      # unrelated prose
      clang -Wno-conversion sample.c
  - run: >
      # Suppression rationale: folded command uses the pinned wrapper.

      clang -Wno-cast-align sample.c
"""
    records = compiler_records("workflow.yml", source)
    reasons = {item.rule: item.reason for item in records}
    expect(
        reasons.get("-Wno-shadow") == "pinned YAML compiler wrapper."
        and reasons.get("-Wno-padded") == "generated ABI contract"
        and reasons.get("-Wno-cast-align") == "folded command uses the pinned wrapper.",
        "quiet: literal and folded YAML compiler reasons are retained",
        failures,
    )
    conversion = next(
        (item for item in records if item.rule == "-Wno-conversion"),
        None,
    )
    expect(
        conversion is not None and not conversion.reason and "blank-reason" in conversion.concerns,
        "must fire: unrelated YAML comments do not become compiler reasons",
        failures,
    )


def _assert_yaml_build_config(inventory: Inventory, failures: list[str]) -> None:
    """Assert YAML command ownership and folded-line source mapping."""
    yaml_rows = [item for item in inventory.suppressions if item.path == "sample.yml"]
    expect(
        len([item for item in yaml_rows if item.provenance == "build-config"])
        == EXPECTED_YAML_BUILD_ROWS,
        "must fire: active YAML cflags are inventoried",
        failures,
    )
    yaml_folded = {
        (item.line, item.column, item.family, item.rule)
        for item in yaml_rows
        if item.line in {14, 17}
    }
    expect(
        yaml_folded
        == {
            (14, 7, "cmake", "-Wno-dev"),
            (17, 7, "compiler", "-Wno-conversion"),
        },
        "must fire: folded YAML command flags retain exact source ownership",
        failures,
    )
    expect(
        len([item for item in yaml_rows if item.family == "python"]) == 1,
        "quiet: YAML block-scalar payload is not active YAML syntax",
        failures,
    )
    yaml_data_lines = {
        item.line
        for item in yaml_rows
        if item.provenance == "build-config" and item.line in {3, 21, 28}
    }
    expect(
        not yaml_data_lines,
        "quiet: literal and folded YAML data blocks stay outside command syntax",
        failures,
    )


def _assert_shell_path_build_config(build: list[Suppression], failures: list[str]) -> None:
    """Assert absolute, quoted, and escaped executable path ownership."""
    shell_build = [
        item
        for item in build
        if item.path == "sample.sh" and item.line in {16, 17, 18, 19, 20, 21, 22, 23}
    ]
    expect(
        {(item.line, item.family, item.rule) for item in shell_build}
        == {
            (17, "cmake", "-Wno-dev"),
            (18, "compiler", "-Wno-sign-conversion"),
            (20, "cmake", "-Wno-dev"),
            (21, "compiler", "-Wno-float-conversion"),
            (22, "compiler", "-Wno-padded"),
        },
        "quiet: CMake data stays quiet while quoted and nested absolute tools fire",
        failures,
    )
    spaced_path_build = [
        item
        for item in build
        if item.path == "sample.sh" and item.line in {24, 25, 26, 27, 28, 29, 30, 31, 32}
    ]
    expect(
        {(item.line, item.family, item.rule) for item in spaced_path_build}
        == {
            (24, "compiler", "-Wno-error"),
            (25, "cmake", "-Wno-dev"),
            (26, "compiler", "-Wno-shadow"),
            (27, "compiler", "-Wno-padded"),
            (28, "compiler", "-Wno-switch"),
            (29, "compiler", "-Wno-cast-align"),
        },
        "quiet: quoted/escaped path data stays quiet while executable words fire",
        failures,
    )


def _assert_cmake_family_build_config(build: list[Suppression], failures: list[str]) -> None:
    """Assert exact CMake diagnostic options retain CMake ownership."""
    cmake_family_fixture_locations = {
        ("sample.sh", 33),
        ("sample.sh", 34),
        ("sample.sh", 35),
        ("sample.sh", 36),
        ("sample.sh", 37),
        ("sample.sh", 38),
        ("sample.sh", 39),
        ("sample.sh", 40),
        ("CMakeLists.txt", 6),
        ("CMakeLists.txt", 7),
        ("CMakeLists.txt", 8),
    }
    cmake_family_rows = [
        item for item in build if (item.path, item.line) in cmake_family_fixture_locations
    ]
    expect(
        {(item.path, item.line, item.family, item.rule) for item in cmake_family_rows}
        == {
            ("sample.sh", 33, "cmake", "-Wno-deprecated"),
            ("sample.sh", 34, "cmake", "-Wno-error=dev"),
            ("sample.sh", 35, "cmake", "-Wno-error=deprecated"),
            ("sample.sh", 36, "compiler", "-Wno-deprecated"),
            ("sample.sh", 37, "compiler", "-Wno-deprecated"),
            ("CMakeLists.txt", 6, "cmake", "-Wno-deprecated"),
            ("CMakeLists.txt", 7, "compiler", "-Wno-deprecated"),
        },
        "quiet: CMake data stays quiet while exact diagnostic and compiler owners fire",
        failures,
    )
    argument_tool_names = [
        item for item in build if item.path == "sample.sh" and item.line == SHELL_ARGUMENT_TOOL_LINE
    ]
    expect(
        not argument_tool_names,
        "quiet: compiler-looking arguments are not executable command words",
        failures,
    )


def _assert_cmake_dev_build_config(build: list[Suppression], failures: list[str]) -> None:
    """Assert -Wno-dev ownership and shell blanket controls."""
    cmake_controls = [item for item in build if item.rule == "-Wno-dev"]
    expect(
        len([item for item in cmake_controls if item.family == "cmake"])
        == EXPECTED_CMAKE_WNO_DEV_ROWS,
        "must fire: configure-mode CMake -Wno-dev has CMake ownership",
        failures,
    )
    expect(
        len([item for item in cmake_controls if item.family == "compiler"])
        == EXPECTED_COMPILER_WNO_DEV_ROWS,
        "must fire: compiler and target_compile_options -Wno-dev stay compiler-owned",
        failures,
    )
    data_controls = [
        item
        for item in build
        if item.path == "CMakeLists.txt" and item.line in {4, 5} and item.rule == "-Wno-dev"
    ]
    expect(
        not data_controls,
        "quiet: CMake data strings containing -Wno-dev are not controls",
        failures,
    )
    shell_blanket = [item for item in build if item.path == "sample.sh" and item.rule == "-w"]
    expect(
        len(shell_blanket) == 1,
        "quiet: shell test/fold -w stay quiet while the compiler option array fires",
        failures,
    )


def _assert_nonvacuity(root: Path, failures: list[str]) -> None:
    """Assert a collapsed scan reports vacuity rather than a clean result."""
    tiny = scan_paths(root, ["sample.py"], enforce_floors=True)
    codes = {item.code for item in tiny.findings}
    expect("vacuous-files" in codes, "must fire: collapsed file census", failures)
    expect("vacuous-inventory" in codes, "must fire: collapsed directive census", failures)
    expect("missing-family" in codes, "must fire: collapsed family coverage", failures)
    expect(
        {"vacuous-family", "vacuous-baseline-files", "vacuous-baseline-rows"} <= codes,
        "must fire: audited family and baseline census floors",
        failures,
    )
    expect(
        not any(
            item.code == "missing-family" and item.message == "test-control"
            for item in tiny.findings
        ),
        "quiet: the audited exact native-skip population is present",
        failures,
    )
    injected = scan_paths(root, ["native_test.cpp"], enforce_floors=True)
    expect(
        any(item.code == "unexpected-family-count" for item in injected.findings),
        "must fire: an injected native skip violates the audited exact family contract",
        failures,
    )


def _assert_ruff_config_fail_closed(root: Path, failures: list[str]) -> None:
    """Assert valid Ruff syntax outside the source locator cannot disappear."""
    pyproject = root / "pyproject.toml"
    text = pyproject.read_text(encoding="ascii")
    pyproject.write_text(
        text.replace('["SLF001"]', '["SLF001", "S603"]'),
        encoding="ascii",
    )
    inventory = scan_paths(root, ["pyproject.toml"])
    expect(
        any(item.code == "malformed-ruff-config" for item in inventory.findings),
        "must fire: unsupported-but-valid Ruff config shape fails closed",
        failures,
    )


def _assert_tool_config_fail_closed(root: Path, failures: list[str]) -> None:
    """Assert malformed central tool ignore syntax cannot disappear."""
    config = root / ".hadolint.yaml"
    text = config.read_text(encoding="ascii")
    config.write_text(text.replace("- DL3008", "- not-a-rule"), encoding="ascii")
    inventory = scan_paths(root, [".hadolint.yaml"])
    expect(
        any(item.code == "malformed-tool-config" for item in inventory.findings),
        "must fire: malformed Hadolint central ignore fails closed",
        failures,
    )


def _assert_optional_tool_activation(root: Path, failures: list[str]) -> None:
    """Assert unused Prettier/markdownlint comments are not active waivers."""
    inventory = scan_paths(root, ["docs/format_controls.md"])
    optional = {item.tool for item in inventory.suppressions} & {"prettier", "markdownlint"}
    inactive = [item for item in inventory.findings if item.code == "inactive-tool-control"]
    expect(
        not optional and len(inactive) >= EXPECTED_OPTIONAL_INACTIVE,
        "quiet: optional formatter/linter controls require repository configuration",
        failures,
    )


def _assert_path_safety(root: Path, inventory: Inventory, failures: list[str]) -> None:
    """Assert invalid Git names and escaping symlinks fail closed."""
    paths, findings = decode_git_paths(b"bad-\xff-name\0", root)
    expect(not paths and bool(findings), "must fire: non-UTF-8 Git path", failures)
    paths, findings = decode_git_paths(b"missing-or-broken\0", root)
    expect(
        paths == ["missing-or-broken"] and not findings,
        "quiet: Git paths are preserved for fail-closed read validation",
        failures,
    )
    unsafe = [item for item in inventory.findings if item.code == "unsafe-symlink"]
    expect(bool(unsafe), "must fire: symlink escaping repository", failures)
    broken = [
        item
        for item in inventory.findings
        if item.code == "read-error" and item.path == "broken-link"
    ]
    expect(bool(broken), "must fire: broken symlink is not silently omitted", failures)
    encoding = [item for item in inventory.findings if item.code == "invalid-text-encoding"]
    expect(bool(encoding), "must fire: invalid UTF-8 authored text", failures)
    vendor = [
        item
        for item in inventory.suppressions
        if item.family == "encoding-exemption"
        and item.owner == "vendor"
        and item.scope.startswith("blob:sha256:")
    ]
    expect(
        bool(vendor),
        "quiet: vendored legacy encoding is inventoried as an exemption",
        failures,
    )


def run_selftest() -> int:
    """Drive production entry points through must-fire and must-stay-quiet cases."""
    print("check_suppressions.py selftest:")
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-suppressions-") as temp:
        base = Path(temp)
        root = base / "repo"
        root.mkdir()
        paths = _write_fixture(root)
        outside = base / "outside.txt"
        outside.write_text("# noqa: F401 -- must not be followed\n", encoding="ascii")
        (root / "outside-link").symlink_to(outside)
        paths.append("outside-link")
        (root / "broken-link").symlink_to(root / "does-not-exist")
        paths.append("broken-link")
        (root / "invalid.txt").write_bytes(b"authored-\xff-text\n")
        paths.append("invalid.txt")
        vendor = root / "libs/third_party/vendor/legacy.txt"
        vendor.parent.mkdir(parents=True, exist_ok=True)
        vendor.write_bytes(b"vendored-\xff-text\n")
        paths.append("libs/third_party/vendor/legacy.txt")
        inventory = scan_paths(root, paths)
        assert_identity_and_structure(root, inventory, failures, EXPECTED_DEAD_ANCHOR_FINDINGS)
        _assert_families(inventory, failures)
        _assert_syntax_awareness(root, inventory, failures)
        assert_stranded_branch_markers(failures)
        assert_stranded_line_markers(failures)
        _assert_debt_control_families(inventory, failures)
        assert_compiler_controls(inventory, root, failures)
        assert_compiler_tool_probes(root, failures)
        assert_clang_tidy_config_fail_closed(root, failures)
        _assert_integrity_findings(inventory, failures)
        _assert_regions_and_config(inventory, failures)
        _assert_ruff_config_fail_closed(root, failures)
        _assert_tool_config_fail_closed(root, failures)
        _assert_optional_tool_activation(root, failures)
        _assert_nonvacuity(root, failures)
        _assert_path_safety(root, inventory, failures)
        assert_governance_parsers(root, failures)
        assert_baseline_ceiling_controls(base, failures)
        assert_identity_semantics(base, failures)
        assert_ledger_gate(base, failures)
    return report(failures)

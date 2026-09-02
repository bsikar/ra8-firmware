# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Explicit both-direction assertions for external-tool suppression syntax."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

from selftest_assert import expect
from suppression_model import Inventory, Suppression

EXPECTED_REGION_ROWS = 2
EXPECTED_HADOLINT_ROWS = 4
EXPECTED_MARKDOWN_CONFIG_ROWS = 2
REPO_ROOT = Path(__file__).resolve().parents[2]
SHFMT_TIMEOUT_SECONDS = 10

TOOL_CONTROL_FIXTURES = {
    "docs/markdownlint_configure.md": (
        """<!-- Suppression rationale: fixed input. -->
<!-- markdownlint-configure-file
{
  "MD013": false,
  "MD033": false
}
-->
# Configured fixture

<span>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</span>
"""
    ),
    "docs/markdownlint_configure_true.md": (
        """<!-- markdownlint-configure-file { "MD013": true } -->
# Enabled fixture
"""
    ),
    "docs/markdownlint_configure_empty.md": (
        """<!-- markdownlint-configure-file {} -->
<!-- markdownlint-configure-file { "MD013": {} } -->
# Empty configuration fixture
"""
    ),
    "docs/markdownlint_configure_options.md": (
        """<!-- Suppression rationale: widens generated lines. -->
<!-- markdownlint-configure-file
{
  "MD013": {
    "line_length": 100
  }
}
-->
# Option fixture
"""
    ),
    "docs/markdownlint_configure_mixed.md": (
        """<!-- Suppression rationale: generated content needs two relaxations. -->
<!-- markdownlint-configure-file
{
  "MD013": false,
  "MD033": {
    "allowed_elements": [
      "span"
    ]
  }
}
-->
# Mixed fixture

<span>generated</span>
"""
    ),
    "docs/markdownlint_active.md": (
        """# Active fixture

<span>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</span>
"""
    ),
}


def _assert_clang_format_behavior(root: Path, failures: list[str]) -> None:
    """Probe empty-colon regions with the repository-pinned clang-format major."""
    tool = shutil.which("clang-format-22")
    if tool is None:
        expect(
            tool is not None,
            "must fire: clang-format-22 is available for behavioral probes",
            failures,
        )
        return
    version = subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed argv
        [tool, "--version"],
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    pinned = version.returncode == 0 and "clang-format version 22." in version.stdout
    expect(pinned, "must fire: behavioral probes use clang-format-22", failures)
    if not pinned:
        return
    probe = subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed fixture
        [tool, "--style=LLVM", "apps/fixture/src/clang_empty_colon_control.c"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    expect(
        probe.returncode == 0
        and "int preserved[] = {1,  2};" in probe.stdout
        and "int reformatted[] = {1, 2};" in probe.stdout,
        "quiet: clang-format-22 honors empty-colon off/on controls",
        failures,
    )


def _assert_c_controls(
    root: Path, by_path: dict[str, list[Suppression]], failures: list[str]
) -> None:
    """Assert clang-format and every documented IWYU command spelling."""
    c_path = "apps/fixture/src/format_controls.c"
    c_directives = {item.directive for item in by_path[c_path]}
    expect(
        {"clang-format off", "clang-format on"} <= c_directives,
        "must fire: exact lowercase clang-format region delimiters",
        failures,
    )
    colon_rows = by_path["apps/fixture/src/clang_colon_control.c"]
    expect(
        len(colon_rows) == EXPECTED_REGION_ROWS and bool(colon_rows[0].reason),
        "must fire: clang-format's behaviorally supported colon rationale",
        failures,
    )
    empty_rows = by_path["apps/fixture/src/clang_empty_colon_control.c"]
    empty_off = next(item for item in empty_rows if item.scope == "region-start")
    expect(
        len(empty_rows) == EXPECTED_REGION_ROWS and "blank-reason" in empty_off.concerns,
        "must fire: empty-colon clang-format off is a blank-reason control",
        failures,
    )
    _assert_clang_format_behavior(root, failures)
    iwyu_commands = {
        "always_keep",
        "associated",
        "begin_exports",
        "begin_keep",
        "end_exports",
        "end_keep",
        "export",
        "friend",
        "keep",
        "no_forward_declare",
        "no_include",
        "private",
    }
    seen_iwyu = {
        item.directive.removeprefix("IWYU pragma: ")
        for item in by_path[c_path]
        if item.tool == "iwyu"
    }
    expect(iwyu_commands == seen_iwyu, "must fire: every documented IWYU pragma", failures)


def _assert_yamllint(by_path: dict[str, list[Suppression]], failures: list[str]) -> None:
    """Assert yamllint's file, line, region, and repeated-rule forms."""
    yaml_directives = {
        item.directive for item in by_path["format_controls.yml"] if item.tool == "yamllint"
    }
    expect(
        {"yamllint disable", "yamllint disable-line", "yamllint enable"} == yaml_directives,
        "must fire: yamllint line and region spellings",
        failures,
    )
    disable_file = [item for item in by_path["disable_file.yml"] if item.tool == "yamllint"]
    expect(
        len(disable_file) == 1 and disable_file[0].directive == "yamllint disable-file",
        "must fire: first-line yamllint disable-file",
        failures,
    )
    expect(
        {
            (item.directive, item.rule)
            for item in by_path["format_controls.yml"]
            if item.tool == "yamllint"
        }
        == {
            ("yamllint disable", "comments"),
            ("yamllint disable", "line-length"),
            ("yamllint disable-line", "line-length"),
            ("yamllint disable-line", "trailing-spaces"),
            ("yamllint enable", "comments"),
            ("yamllint enable", "line-length"),
        },
        "must fire: space-separated repeated yamllint rule tokens",
        failures,
    )


def _assert_shfmt_behavior(root: Path, failures: list[str]) -> None:
    """Probe EditorConfig semantics with the exact project-pinned shfmt."""
    dockerfile = (REPO_ROOT / ".devcontainer" / "Dockerfile").read_text(encoding="ascii")
    pin_match = re.search(r"^ARG SHFMT_VERSION=(\S+)$", dockerfile, re.MULTILINE)
    tool = shutil.which("shfmt")
    if pin_match is None or tool is None:
        expect(
            pin_match is not None and tool is not None,
            "must fire: pinned shfmt is available for behavioral probes",
            failures,
        )
        return
    version = subprocess.run(  # noqa: S603 -- resolved absolute path and fixed argv
        [tool, "--version"],
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    pinned = version.returncode == 0 and version.stdout.strip().removeprefix(
        "v"
    ) == pin_match.group(1)
    expect(pinned, "must fire: behavioral probes use the project-pinned shfmt", failures)
    if not pinned:
        return
    paths = [
        "generated/ignored.sh",
        "generated/mixed-case.sh",
        "generated/false.sh",
        "generated/unset.sh",
        "bad/generated/uppercase-value.sh",
    ]
    probe = subprocess.run(  # noqa: S603 -- resolved pinned tool and fixed fixture paths
        [tool, "--apply-ignore", "-l", *paths],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    formatted = set(probe.stdout.splitlines())
    expect(
        probe.returncode == 1 and not probe.stderr and not formatted.intersection(paths[:2]),
        "quiet: pinned shfmt honors lowercase and mixed-case ignore=true",
        failures,
    )
    expect(
        formatted == set(paths[2:]),
        "must fire: pinned shfmt rejects uppercase true and keeps false/unset active",
        failures,
    )


def _assert_hadolint_behavior(root: Path, failures: list[str]) -> None:
    """Probe spaced ignore syntax with the exact project-pinned Hadolint."""
    dockerfile = (REPO_ROOT / ".devcontainer" / "Dockerfile").read_text(encoding="ascii")
    pin_match = re.search(r"^ARG HADOLINT_VERSION=(\S+)$", dockerfile, re.MULTILINE)
    tool = shutil.which("hadolint")
    if pin_match is None or tool is None:
        expect(
            pin_match is not None and tool is not None,
            "must fire: pinned Hadolint is available for behavioral probes",
            failures,
        )
        return
    version = subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed argv
        [tool, "--version"],
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    pinned = version.returncode == 0 and version.stdout.strip().endswith(pin_match.group(1))
    expect(pinned, "must fire: behavioral probes use project-pinned Hadolint", failures)
    if not pinned:
        return
    probes = [
        subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed fixture
            [tool, "--format", "json", path],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
            timeout=SHFMT_TIMEOUT_SECONDS,
        )
        for path in ("Dockerfile", "Dockerfile.hadolint-active")
    ]
    try:
        ignored, active = (json.loads(probe.stdout) for probe in probes)
    except json.JSONDecodeError:
        ignored, active = None, None
    active_codes = {item.get("code") for item in active} if isinstance(active, list) else set()
    expect(
        probes[0].returncode == 0
        and ignored == []
        and probes[1].returncode == 1
        and active_codes == {"DL3003", "SC2164"},
        "quiet: Hadolint accepts whitespace around equals and commas",
        failures,
    )


def _assert_cmake_lint_behavior(root: Path, failures: list[str]) -> None:
    """Probe cmakelang's literal first space and later whitespace."""
    project = (REPO_ROOT / "pyproject.toml").read_text(encoding="ascii")
    pin_match = re.search(r'"cmakelang==(\S+)"', project)
    tool = shutil.which("cmake-lint")
    if pin_match is None or tool is None:
        expect(
            pin_match is not None and tool is not None,
            "must fire: pinned cmake-lint is available for probes",
            failures,
        )
        return
    version = subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed argv
        [tool, "--version"],
        capture_output=True,
        text=True,
        check=False,
        timeout=SHFMT_TIMEOUT_SECONDS,
    )
    pinned = version.returncode == 0 and version.stdout.strip() == pin_match.group(1)
    expect(pinned, "must fire: behavioral probes use project-pinned cmake-lint", failures)
    if not pinned:
        return
    space, tab = [
        subprocess.run(  # noqa: S603 -- resolved pinned binary and fixed fixtures
            [tool, path],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
            timeout=SHFMT_TIMEOUT_SECONDS,
        )
        for path in ("cmake_lint_space.cmake", "cmake_lint_tab.cmake")
    ]
    expect(
        space.returncode == 0
        and "C0103" not in space.stdout
        and "R0912" not in space.stdout
        and tab.returncode == 1
        and "[C0103]" in tab.stdout,
        "must fire: cmake-lint accepts later tabs but rejects a direct post-colon tab",
        failures,
    )


def _assert_build_controls(
    root: Path,
    inventory: Inventory,
    by_path: dict[str, list[Suppression]],
    failures: list[str],
) -> None:
    """Assert Hadolint, CMake, and shfmt's EditorConfig controls."""
    hadolint_rows = [
        item
        for path in ("Dockerfile", "Dockerfile.global")
        for item in by_path[path]
        if item.tool == "hadolint"
    ]
    hadolint_rules = {(item.path, item.rule) for item in hadolint_rows}
    expect(
        len(hadolint_rows) == EXPECTED_HADOLINT_ROWS
        and hadolint_rules
        == {
            ("Dockerfile", "DL3003"),
            ("Dockerfile", "SC2164"),
            ("Dockerfile.global", "DL3008"),
            ("Dockerfile.global", "SC1091"),
        },
        "must fire: spaced Hadolint lists normalize to one row per rule",
        failures,
    )
    _assert_hadolint_behavior(root, failures)
    cmake_directives = {item.directive for item in by_path["format_controls.cmake"]}
    expect(
        {
            "cmake-format off",
            "cmake-format on",
            "cmake-lint disable",
            "cmf off",
            "cmf on",
        }
        == cmake_directives,
        "must fire: real CMake formatter regions and lint disable pragma",
        failures,
    )
    cmake_space = [item for item in by_path["cmake_lint_space.cmake"] if item.tool == "cmake-lint"]
    expect(
        {item.rule for item in cmake_space} == {"C0103", "R0912"}
        and not by_path.get("cmake_lint_tab.cmake"),
        "must fire: cmake-lint requires a first space and accepts later tabs",
        failures,
    )
    _assert_cmake_lint_behavior(root, failures)
    shfmt = [item for item in by_path[".editorconfig"] if item.tool == "shfmt"]
    expect(
        {item.rule for item in shfmt} == {"[generated/**]", "[generated/mixed-case.sh]"},
        "must fire: case-insensitive shfmt ignore=true properties are controls",
        failures,
    )
    expect(
        not any(item.path == ".editorconfig" for item in inventory.findings),
        "quiet: valid shfmt false and unset properties add no findings",
        failures,
    )
    _assert_shfmt_behavior(root, failures)


def _assert_markdownlint_behavior(root: Path, failures: list[str]) -> None:
    """Probe official configure-file behavior when a Markdown CLI is installed."""
    tool = shutil.which("markdownlint-cli2") or shutil.which("markdownlint")
    if tool is None:
        return
    configured, active = [
        subprocess.run(  # noqa: S603 -- resolved tool and fixed fixture paths
            [tool, path],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
            timeout=SHFMT_TIMEOUT_SECONDS,
        )
        for path in ("docs/markdownlint_configure.md", "docs/markdownlint_active.md")
    ]
    active_output = active.stdout + active.stderr
    expect(
        configured.returncode == 0
        and active.returncode != 0
        and "MD013" in active_output
        and "MD033" in active_output,
        "quiet: installed markdownlint honors multiline configure-file JSON",
        failures,
    )


def _assert_doxygen_controls(
    inventory: Inventory,
    by_path: dict[str, list[Suppression]],
    failures: list[str],
) -> None:
    """Assert Doxygen controls in plain authored inputs and vendored source."""
    markdown = by_path["docs/format_controls.md"]
    markdown_doxygen = [item for item in markdown if item.tool == "doxygen"]
    expect(
        len(markdown_doxygen) == EXPECTED_REGION_ROWS,
        "quiet: Markdown HTML-comment Doxygen commands are inactive",
        failures,
    )
    documentation_paths = {item.path for item in inventory.suppressions if item.tool == "doxygen"}
    expect(
        {
            "apps/fixture/src/format_controls.c",
            "docs/format_controls.dox",
            "docs/format_controls.md",
            "scripts/checks/suppression_selftest.py",
        }
        <= documentation_paths,
        "must fire: Doxygen controls in every configured Doxyfile language",
        failures,
    )
    vendor_path = "libs/third_party/vendor/include/vendor_docs.h"
    vendor_rows = [item for item in by_path[vendor_path] if item.tool == "doxygen"]
    expect(
        len(vendor_rows) == EXPECTED_REGION_ROWS
        and {item.owner for item in vendor_rows} == {"vendor"}
        and not any(item.concerns for item in vendor_rows),
        "quiet: vendored Doxygen conditionals are inventoried as vendor-owned",
        failures,
    )
    expect(
        not any(item.path == vendor_path for item in inventory.findings),
        "quiet: vendored Doxygen conditionals are exempt, not unknown",
        failures,
    )


def _assert_invalid_markdownlint_configures(inventory: Inventory, failures: list[str]) -> None:
    """Assert malformed JSON and unsupported values both fail closed."""
    malformed = [
        item
        for item in inventory.findings
        if item.path == "docs/wrong_format_controls.md"
        and "markdownlint-configure-file" in item.message
    ]
    expect(
        len(malformed) == EXPECTED_MARKDOWN_CONFIG_ROWS,
        "must fire: malformed JSON and unsupported configure-file values fail closed",
        failures,
    )


def _assert_markdownlint_configures(
    inventory: Inventory,
    by_path: dict[str, list[Suppression]],
    failures: list[str],
) -> None:
    """Assert configure-file values in both valid and invalid directions."""
    disabled = by_path["docs/markdownlint_configure.md"]
    expect(
        len(disabled) == EXPECTED_MARKDOWN_CONFIG_ROWS
        and {item.rule for item in disabled} == {"MD013", "MD033"}
        and {item.reason for item in disabled} == {"fixed input."}
        and not any(item.concerns for item in disabled),
        "must fire: configure-file false values normalize one row per rule",
        failures,
    )
    options = by_path["docs/markdownlint_configure_options.md"]
    expect(
        len(options) == 1
        and options[0].rule == "MD013"
        and options[0].reason == "widens generated lines."
        and not options[0].concerns,
        "must fire: non-empty configure-file option objects are inventoried",
        failures,
    )
    mixed = by_path["docs/markdownlint_configure_mixed.md"]
    expect(
        len(mixed) == EXPECTED_MARKDOWN_CONFIG_ROWS
        and {item.rule for item in mixed} == {"MD013", "MD033"}
        and {item.reason for item in mixed} == {"generated content needs two relaxations."}
        and not any(item.concerns for item in mixed),
        "must fire: mixed false and option values preserve both relaxations",
        failures,
    )
    quiet_paths = {
        "docs/markdownlint_configure_true.md",
        "docs/markdownlint_configure_empty.md",
    }
    expect(
        all(not by_path.get(path) for path in quiet_paths),
        "quiet: true and empty configure-file values add no waiver rows",
        failures,
    )
    valid_paths = quiet_paths | {
        "docs/markdownlint_configure.md",
        "docs/markdownlint_configure_options.md",
        "docs/markdownlint_configure_mixed.md",
    }
    expect(
        not any(item.path in valid_paths for item in inventory.findings),
        "quiet: every valid configure-file object has zero integrity findings",
        failures,
    )
    _assert_invalid_markdownlint_configures(inventory, failures)


def _assert_documentation_controls(
    root: Path, inventory: Inventory, by_path: dict[str, list[Suppression]], failures: list[str]
) -> None:
    """Assert configured Markdown controls and all Doxyfile input languages."""
    markdown = by_path["docs/format_controls.md"]
    markdown_controls = {item.directive for item in markdown}
    expect(
        {
            "markdownlint capture",
            "markdownlint disable",
            "markdownlint disable-file",
            "markdownlint disable-line",
            "markdownlint disable-next-line",
            "markdownlint enable",
            "markdownlint enable-file",
            "markdownlint restore",
            "prettier-ignore",
        }
        <= markdown_controls,
        "must fire: configured Prettier and every markdownlint control spelling",
        failures,
    )
    _assert_markdownlint_configures(inventory, by_path, failures)
    _assert_doxygen_controls(inventory, by_path, failures)
    expect(
        not any(item.path == "docs/format_controls.md" for item in inventory.findings),
        "quiet: valid Markdown controls have zero integrity findings",
        failures,
    )
    _assert_markdownlint_behavior(root, failures)


def _assert_invalid_controls(inventory: Inventory, failures: list[str]) -> None:
    """Assert wrong syntax stays inactive and every invalid region fails closed."""
    invalid_paths = {
        "Dockerfile.bad",
        "apps/fixture/src/wrong_controls.c",
        "bad/.editorconfig",
        "docs/wrong_format_controls.md",
        "cmake_lint_tab.cmake",
        "scripts/checks/suppression_scan.py",
        "wrong_format_controls.cmake",
        "wrong_format_controls.yml",
    }
    expect(
        not any(item.path in invalid_paths for item in inventory.suppressions),
        "quiet: wrong case, suffix, separator, argument, and language never count",
        failures,
    )
    malformed_paths = {
        item.path
        for item in inventory.findings
        if item.code in {"malformed-tool-config", "malformed-tool-control"}
    }
    expect(
        invalid_paths <= malformed_paths,
        "must fire: every invalid tool-control fixture fails closed",
        failures,
    )

    unmatched_paths = {
        "apps/fixture/src/unmatched_controls.c",
        "docs/unmatched_format_controls.dox",
        "unmatched_format_controls.cmake",
        "unmatched_format_controls.yml",
    }
    region_paths = {item.path for item in inventory.findings if "region" in item.code}
    expect(
        unmatched_paths <= region_paths,
        "must fire: unmatched clang-format, IWYU, Doxygen, CMake, and yamllint regions",
        failures,
    )


def _assert_rationales(inventory: Inventory, failures: list[str]) -> None:
    """Assert the exact controls whose active syntax carries no rationale."""
    assigned_tools = {
        "clang-format",
        "cmake-format",
        "cmake-lint",
        "doxygen",
        "hadolint",
        "iwyu",
        "markdownlint",
        "prettier",
        "shfmt",
        "yamllint",
    }
    assigned = {item for item in inventory.suppressions if item.tool in assigned_tools}
    blank = [item for item in assigned if "blank-reason" in item.concerns]
    blank_keys = {(item.path, item.tool, item.scope) for item in blank}
    expect(
        blank_keys
        == {
            ("apps/fixture/src/clang_empty_colon_control.c", "clang-format", "region-start"),
            ("disable_file.yml", "yamllint", "file"),
        },
        "must fire: empty-colon off and first-line disable-file have blank reasons",
        failures,
    )


def assert_tool_control_syntax_awareness(
    root: Path, inventory: Inventory, failures: list[str]
) -> None:
    """Assert every assigned tool spelling and invalid direction explicitly."""
    by_path: dict[str, list[Suppression]] = {}
    for item in inventory.suppressions:
        by_path.setdefault(item.path, []).append(item)
    _assert_c_controls(root, by_path, failures)
    _assert_yamllint(by_path, failures)
    _assert_build_controls(root, inventory, by_path, failures)
    _assert_documentation_controls(root, inventory, by_path, failures)
    _assert_invalid_controls(inventory, failures)
    _assert_rationales(inventory, failures)

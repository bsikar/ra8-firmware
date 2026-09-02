# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Compiler-control assertions shared by the suppression selftest."""

from __future__ import annotations

import os
from pathlib import Path

from check_no_dynamic_alloc import check as check_dynamic_alloc
from selftest_assert import expect
from suppression_model import Inventory
from suppression_scan import scan_paths

EXPECTED_COMPILER_PRAGMA_ROWS = 12
EXPECTED_CLANG_TIDY_CONFIG_ROWS = 1
EXPECTED_ALLOC_PROBLEMS = 2
EXPECTED_CPP_ALLOC_PROBLEMS = 1
EXPECTED_CPP_ALLOC_ROWS = 2
EXPECTED_GNU_UNUSED_ROWS = 2
EXPECTED_GNU_SANITIZER_ROWS = 3


def _run_tool(executable: str, arguments: list[str], output: Path) -> tuple[int, str]:
    """Run one fixed absolute tool path and capture its complete diagnostics."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    descriptor = os.open(output, flags, 0o600)
    actions = [
        (os.POSIX_SPAWN_DUP2, descriptor, 1),
        (os.POSIX_SPAWN_DUP2, descriptor, 2),
        (os.POSIX_SPAWN_CLOSE, descriptor),
    ]
    try:
        process = os.posix_spawn(
            executable,
            [executable, *arguments],
            os.environ,
            file_actions=actions,
        )
        _, status = os.waitpid(process, 0)
    finally:
        os.close(descriptor)
    return os.waitstatus_to_exitcode(status), output.read_text(encoding="utf-8")


def _assert_control_rows(inventory: Inventory, failures: list[str]) -> None:
    """Assert valid C-family controls fire once and string lookalikes stay inert."""
    rows = [item for item in inventory.suppressions if item.path == "compiler_controls.c"]
    pragmas = [item for item in rows if item.provenance == "compiler-pragma"]
    expect(
        len(pragmas) == EXPECTED_COMPILER_PRAGMA_ROWS,
        "must fire: paired GCC and clang diagnostic state controls",
        failures,
    )
    expect(
        len([item for item in rows if item.directive == "[[maybe_unused]]"]) == 1
        and len([item for item in rows if item.directive == "__attribute__((unused))"])
        == EXPECTED_GNU_UNUSED_ROWS
        and len([item for item in rows if item.directive == "[[gnu::unused]]"]) == 1
        and len([item for item in rows if item.directive == "__attribute__((no_sanitize))"])
        == EXPECTED_GNU_SANITIZER_ROWS
        and len([item for item in rows if item.directive == "[[clang::no_sanitize]]"]) == 1,
        "must fire: every clang-18-valid compiler attribute spelling is inventoried",
        failures,
    )
    expect(
        len([item for item in rows if item.directive == "RA8_NASA_RULE_3_OK"]) == 1,
        "must fire: reason-bearing NASA Rule 3 waiver",
        failures,
    )
    expect(
        len([item for item in rows if item.directive == "alloc-allow"]) == 1,
        "must fire: same-line reasoned allocation waiver",
        failures,
    )


def _assert_malformed_controls(inventory: Inventory, failures: list[str]) -> None:
    """Assert malformed, orphaned, mismatched, and unscoped controls fail closed."""
    codes = {item.code for item in inventory.findings if item.path == "malformed_controls.c"}
    expected = {
        "malformed-alloc-allow",
        "malformed-diagnostic-pragma",
        "malformed-gnu-attribute",
        "malformed-maybe-unused",
        "malformed-standard-attribute",
        "malformed-nasa-rule-3-waiver",
        "orphan-alloc-allow",
        "unmatched-diagnostic-pop",
        "unmatched-region-end",
        "unmatched-region-start",
        "unscoped-diagnostic-control",
    }
    expect(
        expected <= codes,
        "must fire: malformed, orphaned, and unscoped compiler controls",
        failures,
    )


def _assert_clang_tidy_and_alloc(inventory: Inventory, root: Path, failures: list[str]) -> None:
    """Assert clang-tidy reasoning and lexical allocation scope."""
    config = [
        item
        for item in inventory.suppressions
        if item.path == ".clang-tidy" and item.directive == "Checks exclude"
    ]
    expect(
        len(config) == EXPECTED_CLANG_TIDY_CONFIG_ROWS and not config[0].concerns,
        "must fire: source-located reasoned clang-tidy global exclusion",
        failures,
    )
    alloc_problems = check_dynamic_alloc(root / "alloc_consumer.c")
    expect(
        len(alloc_problems) == EXPECTED_ALLOC_PROBLEMS
        and any("string_only" in problem for problem in alloc_problems)
        and any("without governed allocation" in problem for problem in alloc_problems),
        "quiet: allocation waiver works only in a lexical comment with a governed call",
        failures,
    )
    cpp_rows = [
        item
        for item in inventory.suppressions
        if item.path == "alloc_consumer.cpp" and item.directive == "alloc-allow"
    ]
    cpp_problems = check_dynamic_alloc(root / "alloc_consumer.cpp")
    cpp_findings = [
        item
        for item in inventory.findings
        if item.path == "alloc_consumer.cpp" and item.code == "orphan-alloc-allow"
    ]
    expect(
        len(cpp_rows) == EXPECTED_CPP_ALLOC_ROWS
        and {item.rule for item in cpp_rows} == {"C++ new", "C++ delete"}
        and len(cpp_problems) == EXPECTED_CPP_ALLOC_PROBLEMS
        and "without governed allocation" in cpp_problems[0]
        and len(cpp_findings) == 1,
        "must fire: C++ new/delete controls are active and orphan controls fail closed",
        failures,
    )


def assert_compiler_controls(inventory: Inventory, root: Path, failures: list[str]) -> None:
    """Assert every assigned C-family control fires without string false positives."""
    _assert_control_rows(inventory, failures)
    _assert_malformed_controls(inventory, failures)
    _assert_clang_tidy_and_alloc(inventory, root, failures)


def _clang_syntax_status(root: Path, name: str, source: str) -> int:
    """Compile one C23 fixture with the pinned warning-strict Clang."""
    fixture = root / f"{name}.c"
    fixture.write_text(source, encoding="ascii")
    status, _ = _run_tool(
        "/usr/bin/clang-18",
        [
            "-std=c2x",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsyntax-only",
            str(fixture),
        ],
        root / f"{name}.log",
    )
    return status


def _assert_compiler_probe(root: Path, failures: list[str]) -> None:
    """Prove pinned Clang accepts every inventoried valid grammar."""
    compiler = Path("/usr/bin/clang-18")
    expect(compiler.is_file(), "must fire: pinned clang-18 is available", failures)
    if not compiler.is_file():
        return
    source = """_Pragma("clang diagnostic push")
_Pragma("clang diagnostic ignored \\"-Wunused-variable\\"")
void probe(void) { int unused; }
_Pragma("clang diagnostic pop")
[[deprecated, maybe_unused]] static int maybe_value;
[[gnu::unused]] static int scoped_gnu_value;
__attribute((unused)) static int short_gnu_value;
__attribute__((unused)) static int gnu_value;
__attribute__((no_sanitize(\"undefined\"))) int identity(int value) { return value; }
__attribute__((__no_sanitize__(\"undefined\"))) int identity_two(int value) { return value; }
__attribute__((__no_sanitize_address__)) int identity_three(int value) { return value; }
[[clang::no_sanitize(\"undefined\")]] int identity_four(int value) { return value; }
"""
    accepted_status = _clang_syntax_status(root, "compiler_probe", source)
    expect(
        accepted_status == 0,
        "quiet: clang-18 accepts _Pragma and every supported attribute grammar",
        failures,
    )


def _assert_compiler_rejections(root: Path, failures: list[str]) -> None:
    """Prove pinned Clang rejects the three fail-closed grammar boundaries."""
    if not Path("/usr/bin/clang-18").is_file():
        return
    invalid = (
        ("compiler_bad_unused", "__attribute__((unused extra)) static int value;\n"),
        (
            "compiler_bad_sanitizer",
            "__attribute__((no_sanitize(foo))) int identity(int value) { return value; }\n",
        ),
        ("compiler_bad_maybe", "[[maybe_unused bogus]] static int value;\n"),
    )
    for name, source in invalid:
        rejected_status = _clang_syntax_status(root, name, source)
        expect(
            rejected_status != 0,
            f"must fire: clang-18 rejects {name.removeprefix('compiler_bad_')}",
            failures,
        )


def _assert_clang_tidy_probe(root: Path, failures: list[str]) -> None:
    """Prove pinned clang-tidy accepts the inventoried negative check glob."""
    tidy = Path("/usr/bin/clang-tidy-18")
    expect(tidy.is_file(), "must fire: pinned clang-tidy-18 is available", failures)
    if tidy.is_file():
        dumped_status, dumped_output = _run_tool(
            "/usr/bin/clang-tidy-18",
            [
                f"--config-file={root / '.clang-tidy'}",
                "--dump-config",
            ],
            root / "clang_tidy_probe.log",
        )
        expect(
            dumped_status == 0 and "-readability-fixture" in dumped_output,
            "quiet: clang-tidy-18 activates the inventoried negative Checks glob",
            failures,
        )


def assert_compiler_tool_probes(root: Path, failures: list[str]) -> None:
    """Prove pinned compiler tools accept the inventoried grammars."""
    _assert_compiler_probe(root, failures)
    _assert_compiler_rejections(root, failures)
    _assert_clang_tidy_probe(root, failures)


def assert_clang_tidy_config_fail_closed(root: Path, failures: list[str]) -> None:
    """Assert valid YAML shapes outside the source locator cannot disappear."""
    config = root / ".clang-tidy"
    original = config.read_text(encoding="ascii")
    config.write_text("Checks: [readability-*]\n", encoding="ascii")
    inventory = scan_paths(root, [".clang-tidy"])
    expect(
        any(item.code == "malformed-clang-tidy-config" for item in inventory.findings),
        "must fire: non-string clang-tidy Checks config fails closed",
        failures,
    )
    config.write_text(original, encoding="ascii")

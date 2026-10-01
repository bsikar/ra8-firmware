# SPDX-License-Identifier: Apache-2.0
"""Selftest fixtures for the first-party Zig gate.

Holds the fixtures check_zig.py runs under --selftest-lint and
--selftest-test: the throwaway trees they build, the must-fire cases
that prove each rule actually discriminates, and the grammar and
contract probes.

Separated from :mod:`check_zig` because the fixtures are much larger
than the rules they exercise and are only ever loaded by the two
selftest entry points. check_zig imports this module lazily inside
those entry points, so the ordinary lint and test paths never pay for
it.
"""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path
from typing import cast

from check_zig import (
    HOST_TARGET_CONTRACT_NAME,
    HOST_TARGET_HELPER,
    TEST_CONTRACT_NAME,
    _host_target_errors,
    _run_covered_sources,
    _run_format,
    _run_lint,
    _run_tests,
    _test_contract_errors,
)


def _selftest_lint(zig: str, failures: list[str]) -> None:
    """Prove `zig fmt --ast-check --check` flags unformatted/bad syntax and accepts clean."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # 1. Unformatted fixture must fail
        bad_fmt = root / "bad_fmt.zig"
        bad_fmt.write_text(
            'const std = @import("std");\n\nfn foo()   void {}\n',
            encoding="utf-8",
        )
        unfmt, _ = _run_lint(zig, [str(bad_fmt)])
        if not unfmt:
            failures.append("  must-fire: `zig fmt` accepted unformatted code")
        if _run_format(zig, [str(bad_fmt)]) != 0:
            failures.append("  must-stay-quiet: `zig fmt` could not fix unformatted code")
        fixed_unfmt, fixed_syn = _run_lint(zig, [str(bad_fmt)])
        if fixed_unfmt or fixed_syn:
            failures.append(
                f"  must-stay-quiet: formatted fixture remained dirty: {fixed_unfmt} {fixed_syn}"
            )

        # 2. Bad syntax fixture must fail
        bad_syn = root / "bad_syntax.zig"
        bad_syn.write_text("const std = @import(\n", encoding="utf-8")
        _, syn_errs = _run_lint(zig, [str(bad_syn)])
        if not syn_errs:
            failures.append("  must-fire: `zig fmt --ast-check` accepted syntax error")

        # 3. Clean fixture must pass
        good = root / "clean.zig"
        good.write_text(
            'const std = @import("std");\n\npub fn main() void {}\n',
            encoding="utf-8",
        )
        unfmt_good, syn_good = _run_lint(zig, [str(good)])
        if unfmt_good or syn_good:
            failures.append(
                f"  must-stay-quiet: `zig fmt` rejected clean fixture: {unfmt_good} {syn_good}"
            )


def _write_build_fixture(root: Path, test_source: str) -> None:
    """Write one minimal Zig 0.14 graph with a dedicated test root."""
    (root / "tests").mkdir(exist_ok=True)
    (root / "build.zig").write_text(
        'const std = @import("std");\n'
        "\n"
        "pub fn build(b: *std.Build) void {\n"
        "    const target = b.standardTargetOptions(.{});\n"
        "    const optimize = b.standardOptimizeOption(.{});\n"
        "    const tests = b.addTest(.{\n"
        "        .root_module = b.createModule(.{\n"
        '            .root_source_file = b.path("tests/main.zig"),\n'
        "            .target = target,\n"
        "            .optimize = optimize,\n"
        "        }),\n"
        "    });\n"
        "    const run_tests = b.addRunArtifact(tests);\n"
        '    const test_step = b.step("test", "Run unit tests");\n'
        "    test_step.dependOn(&run_tests.step);\n"
        "}\n",
        encoding="utf-8",
    )
    (root / "tests/main.zig").write_text(test_source, encoding="utf-8")
    (root / TEST_CONTRACT_NAME).write_text(
        json.dumps(
            {
                "test_roots": ["tests/main.zig"],
                "covered_sources": ["tests/main.zig"],
                "minimum_tests": 1,
            }
        )
        + "\n",
        encoding="utf-8",
    )


def _selftest_build_execution(zig: str, root: Path, failures: list[str]) -> None:
    """Prove passing and failing Zig build-graph test execution."""
    _write_build_fixture(
        root,
        'const std = @import("std");\n'
        'test "selftest pass" {\n'
        "    try std.testing.expect(true);\n"
        "}\n",
    )
    proc_pass = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [zig, "build", "test"], cwd=root, capture_output=True, text=True, check=False
    )
    if proc_pass.returncode != 0:
        failures.append(f"  must-stay-quiet: passing build graph failed: {proc_pass.stderr}")

    _write_build_fixture(
        root,
        'const std = @import("std");\n'
        'test "selftest fail" {\n'
        "    try std.testing.expect(false);\n"
        "}\n",
    )
    proc_fail = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [zig, "build", "test"], cwd=root, capture_output=True, text=True, check=False
    )
    if proc_fail.returncode == 0:
        failures.append("  must-fire: failing build graph was accepted")


def _selftest_build_wiring(zig: str, root: Path, failures: list[str]) -> None:
    """Prove comments and dummy roots cannot satisfy causal test wiring."""
    # A raw-text import mention must not hide an independently failing source.
    _write_build_fixture(
        root,
        '/*\n_ = @import("shadow.zig");\n*/\n'
        'test "real root" {\n'
        '    try @import("std").testing.expect(true);\n'
        "}\n",
    )
    (root / "tests/shadow.zig").write_text(
        'test "unwired failure" {\n    try @import("std").testing.expect(false);\n}\n',
        encoding="utf-8",
    )
    (root / TEST_CONTRACT_NAME).write_text(
        json.dumps(
            {
                "test_roots": ["tests/main.zig"],
                "covered_sources": ["tests/main.zig", "tests/shadow.zig"],
                "minimum_tests": 1,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    wiring_errors, _ = _test_contract_errors(root)
    if not any("test module is not imported" in error for error in wiring_errors):
        failures.append("  must-fire: test module hidden by a commented import was accepted")

    # A test step that runs a different root must not satisfy the contract.
    (root / "tests/shadow.zig").write_text(
        'test "dummy pass" {\n    try @import("std").testing.expect(true);\n}\n',
        encoding="utf-8",
    )
    build_text = (root / "build.zig").read_text(encoding="utf-8")
    (root / "build.zig").write_text(
        build_text.replace('b.path("tests/main.zig")', 'b.path("tests/shadow.zig")'),
        encoding="utf-8",
    )
    causal_failures, _ = _run_tests(zig, [root])
    if not any("did not compile declared test root" in item for item in causal_failures.values()):
        failures.append("  must-fire: test step wired to a dummy root was accepted")


def _selftest_tests(zig: str, failures: list[str]) -> None:
    """Prove the build-graph test target fails and passes as expected."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _selftest_build_execution(zig, root, failures)
        _selftest_build_wiring(zig, root, failures)


def _selftest_test_declaration_grammar(
    root: Path,
    passing_source: str,
    contract: Path,
    raw_contract: dict[str, object],
    failures: list[str],
) -> None:
    """Prove identifier tests count and comment text never counts."""
    test_root = root / "tests/main.zig"
    test_root.write_text(
        passing_source + 'test identifierContract { try @import("std").testing.expect(true); }\n',
        encoding="utf-8",
    )
    raw_contract["minimum_tests"] = 2
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, count = _test_contract_errors(root)
    identifier_test_count = 2
    if errors or count != identifier_test_count:
        failures.append(f"  must-stay-quiet: identifier-named Zig test was not counted: {errors}")
    test_root.write_text(passing_source, encoding="utf-8")
    raw_contract["minimum_tests"] = 1

    identifier_source = root / "src/identifier_test.zig"
    identifier_source.write_text(
        'test productionContract { try @import("std").testing.expect(true); }\n',
        encoding="utf-8",
    )
    covered_sources = cast("list[str]", raw_contract["covered_sources"])
    covered_sources.append("src/identifier_test.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("production Zig source contains inline tests" in error for error in errors):
        failures.append("  must-fire: identifier-named production Zig test was accepted")
    identifier_source.unlink()
    covered_sources.remove("src/identifier_test.zig")

    comment_source = root / "src/comment_only.zig"
    comment_source.write_text(
        '/* test "not real" { try @import("std").testing.expect(false); } */\n'
        "pub const value: u32 = 1;\n",
        encoding="utf-8",
    )
    covered_sources.append("src/comment_only.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, count = _test_contract_errors(root)
    if any("production Zig source contains inline tests" in error for error in errors):
        failures.append("  must-stay-quiet: block-comment test text was treated as production test")
    if count != 1:
        failures.append("  must-stay-quiet: block-comment test text inflated the test floor")
    comment_source.unlink()
    covered_sources.remove("src/comment_only.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")


def _selftest_standalone_args(
    root: Path, contract: Path, raw_contract: dict[str, object], failures: list[str]
) -> None:
    """Prove per-source standalone compiler arguments are strictly validated."""
    raw_contract["standalone_source_args"] = {"helper.zig": ["-fno-emit-bin"]}
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if errors:
        failures.append(f"  must-stay-quiet: valid standalone source arguments failed: {errors}")

    raw_contract["standalone_source_args"] = {"missing.zig": ["-fno-emit-bin"]}
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("names no covered production source" in error for error in errors):
        failures.append("  must-fire: standalone arguments for an unknown source were accepted")

    raw_contract["standalone_source_args"] = {"helper.zig": []}
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("non-empty string lists" in error for error in errors):
        failures.append("  must-fire: empty standalone source arguments were accepted")

    raw_contract["standalone_source_args"] = {
        "helper.zig": [
            "--dep",
            "build_config",
            "-Mroot=wrong.zig",
            "-Mbuild_config=helper.zig",
        ]
    }
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("must contain exactly '-Mroot=helper.zig'" in error for error in errors):
        failures.append("  must-fire: mismatched standalone named root was accepted")

    raw_contract["standalone_source_args"] = {"helper.zig": ["-fno-emit-bin"]}
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")


def _selftest_standalone_compilation(zig: str, failures: list[str]) -> None:
    """Prove compile-only externs and named build-option modules both work."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "src").mkdir()
        (root / "tests").mkdir()
        (root / "src/extern.zig").write_text(
            "extern fn external_log() void;\n"
            "pub export fn callExternalLog() void { external_log(); }\n",
            encoding="utf-8",
        )
        (root / "src/configured.zig").write_text(
            'const build_config = @import("build_config");\n'
            "pub export fn offTarget() u8 { return @intFromBool(build_config.off_target); }\n",
            encoding="utf-8",
        )
        (root / "src/standalone_build_config.zig").write_text(
            "pub const off_target: bool = true;\n",
            encoding="utf-8",
        )
        (root / "tests/main.zig").write_text(
            'test "standalone fixture" { try @import("std").testing.expect(true); }\n',
            encoding="utf-8",
        )
        contract = {
            "test_roots": ["tests/main.zig"],
            "covered_sources": [
                "src/extern.zig",
                "src/configured.zig",
                "src/standalone_build_config.zig",
                "tests/main.zig",
            ],
            "standalone_source_args": {
                "src/configured.zig": [
                    "--dep",
                    "build_config",
                    "-Mroot=src/configured.zig",
                    "-Mbuild_config=src/standalone_build_config.zig",
                ]
            },
            "minimum_tests": 1,
        }
        (root / TEST_CONTRACT_NAME).write_text(json.dumps(contract) + "\n", encoding="utf-8")
        source_failures = _run_covered_sources(zig, [root])
        if source_failures:
            failures.append(
                "  must-stay-quiet: standalone compile-only/module fixture failed: "
                f"{source_failures}"
            )

        del contract["standalone_source_args"]
        (root / TEST_CONTRACT_NAME).write_text(json.dumps(contract) + "\n", encoding="utf-8")
        source_failures = _run_covered_sources(zig, [root])
        if not any(key.endswith("src/configured.zig") for key in source_failures):
            failures.append("  must-fire: missing standalone module dependency was accepted")


def _selftest_production_coverage(
    root: Path, contract: Path, raw_contract: dict[str, object], failures: list[str]
) -> None:
    """Prove production inline tests and orphan sources are rejected."""
    inline = root / "src/inline.zig"
    inline.write_text(
        'test "production test" { try @import("std").testing.expect(true); }\n',
        encoding="utf-8",
    )
    raw_contract["covered_sources"].append("src/inline.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("production Zig source contains inline tests" in error for error in errors):
        failures.append("  must-fire: inline test in production Zig source was accepted")
    inline.unlink()
    raw_contract["covered_sources"].remove("src/inline.zig")
    contract.write_text(json.dumps(raw_contract) + "\n", encoding="utf-8")

    orphan = root / "orphan.zig"
    orphan.write_text("pub const orphan = true;\n", encoding="utf-8")
    errors, _ = _test_contract_errors(root)
    if not any("orphan Zig source" in error for error in errors):
        failures.append("  must-fire: orphan Zig source was accepted")
    orphan.unlink()


def _selftest_host_target_rule(failures: list[str]) -> None:
    """Prove the macOS host target rule fires and stays quiet in both directions."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write_build_fixture(
            root,
            'test "host target" {\n    try @import("std").testing.expect(true);\n}\n',
        )
        build_zig = root / "build.zig"
        plain_text = build_zig.read_text(encoding="utf-8")

        if not any("#899" in error for error in _host_target_errors(root)):
            failures.append("  must-fire: build root with a plain native default was accepted")

        commented = plain_text.replace(
            "    const target = b.standardTargetOptions(.{});",
            f"    // .default_target = ra8_build.{HOST_TARGET_HELPER}(b)\n"
            "    const target = b.standardTargetOptions(.{});",
        )
        build_zig.write_text(commented, encoding="utf-8")
        if not _host_target_errors(root):
            failures.append("  must-fire: commented host-target wiring was accepted")

        wired = plain_text.replace(
            "b.standardTargetOptions(.{})",
            f"b.standardTargetOptions(.{{ .default_target = ra8_build.{HOST_TARGET_HELPER}(b) }})",
        )
        build_zig.write_text(wired, encoding="utf-8")
        if _host_target_errors(root):
            failures.append("  must-stay-quiet: wired host-target build root was rejected")

        build_zig.write_text(plain_text, encoding="utf-8")
        contract = root / HOST_TARGET_CONTRACT_NAME
        contract.write_text(
            json.dumps({"rule": "exempt", "reason": "cross-compiles for ARM only"}) + "\n",
            encoding="utf-8",
        )
        if _host_target_errors(root):
            failures.append("  must-stay-quiet: declared host-target exemption was rejected")

        contract.write_text(json.dumps({"rule": "exempt", "reason": "  "}) + "\n", encoding="utf-8")
        if not any("non-empty reason" in error for error in _host_target_errors(root)):
            failures.append("  must-fire: host-target exemption without a reason was accepted")

        contract.write_text(json.dumps({"rule": "whatever"}) + "\n", encoding="utf-8")
        if not any("must be" in error for error in _host_target_errors(root)):
            failures.append("  must-fire: unknown host-target rule was accepted")

        contract.write_text('{"rule":\n', encoding="utf-8")
        if not any("invalid" in error for error in _host_target_errors(root)):
            failures.append("  must-fire: malformed host-target declaration was accepted")

        contract.write_text(json.dumps({"rule": "host_default"}) + "\n", encoding="utf-8")
        if not any("#899" in error for error in _host_target_errors(root)):
            failures.append('  must-fire: declared "host_default" without the wiring was accepted')


def _selftest_test_contract(failures: list[str]) -> None:
    """Prove test wiring, floor, and orphan checks fire and stay quiet."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        passing_source = (
            'const helper = @import("../helper.zig");\n'
            'test "contract pass" {\n'
            "    _ = helper.value;\n"
            "}\n"
        )
        _write_build_fixture(root, passing_source)
        (root / "helper.zig").write_text("pub const value: u32 = 1;\n", encoding="utf-8")
        (root / TEST_CONTRACT_NAME).write_text(
            json.dumps(
                {
                    "test_roots": ["tests/main.zig"],
                    "covered_sources": ["tests/main.zig", "helper.zig"],
                    "minimum_tests": 1,
                }
            )
            + "\n",
            encoding="utf-8",
        )

        errors, count = _test_contract_errors(root)
        if errors or count != 1:
            failures.append(f"  must-stay-quiet: compliant Zig test contract failed: {errors}")

        contract = root / TEST_CONTRACT_NAME
        raw_contract = json.loads(contract.read_text(encoding="utf-8"))
        _selftest_standalone_args(root, contract, raw_contract, failures)

        (root / "src").mkdir()
        _selftest_test_declaration_grammar(root, passing_source, contract, raw_contract, failures)
        _selftest_production_coverage(root, contract, raw_contract, failures)

        (root / TEST_CONTRACT_NAME).write_text(
            json.dumps(
                {
                    "test_roots": ["tests/main.zig"],
                    "covered_sources": ["tests/main.zig", "helper.zig"],
                    "minimum_tests": 2,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        errors, _ = _test_contract_errors(root)
        if not any("below floor" in error for error in errors):
            failures.append("  must-fire: Zig test count below its floor was accepted")

        build_text = (root / "build.zig").read_text(encoding="utf-8")
        (root / "build.zig").write_text(
            build_text.replace(
                'b.step("test", "Run unit tests")', 'b.step("verify", "Run unit tests")'
            ),
            encoding="utf-8",
        )
        errors, _ = _test_contract_errors(root)
        if not any("no explicit" in error for error in errors):
            failures.append("  must-fire: missing Zig build test step was accepted")

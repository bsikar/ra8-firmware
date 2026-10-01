# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Prove the Zig ABI policy checker fires on every failure class it advertises.

Imported by check_zig_abi_policy.py --selftest. The checker enforces a
hand-maintained inventory, so the only thing standing between it and a
vacuous pass is a fixture that sabotages one rule at a time and asserts the
matching finding appears. That evidence is a subject of its own and lives
here rather than as the tail of the checker.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

from check_zig_abi_policy import (
    RA8_ZIG_ARGUMENTS,
    REQUIRED_MODES,
    _archive_symbol_names,
    _compatibility_findings,
    _compiled_findings,
    _contains_token_sequence,
    _library_findings,
    _matrix_jobs,
    _mode_test_policy_findings,
    _normalized_header_digest,
    _repository_inventory_findings,
    _validate,
)


def _selftest_fixture(root: Path, header: str, adapter: str) -> dict[str, Any]:
    """Write the isolated ABI fixture and return its compliant policy row."""
    (root / "build").mkdir()
    (root / "inc").mkdir()
    (root / "tests").mkdir()
    (root / "inc/demo.h").write_text(header, encoding="utf-8")
    (root / "build/adapter.zig").write_text(adapter, encoding="utf-8")
    (root / "build/build.zig").write_text(
        """const std = @import("std");
pub fn build(b: *std.Build) void {
    const module = b.createModule(.{
        .root_source_file = b.path("adapter.zig"),
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
    });
    b.installArtifact(b.addLibrary(.{ .name = "demo", .linkage = .static, .root_module = module }));
}
""",
        encoding="utf-8",
    )
    tests = {
        "c": "int demo_run(void); int main(void) { return demo_run(); }\n",
        "rust": 'extern "C" { fn demo_run(); } #[test] fn calls() { unsafe { demo_run() } }\n',
        "zig": 'extern fn demo_run() void; test "calls" { demo_run(); }\n',
    }
    for language, contents in tests.items():
        (root / f"tests/test.{language}").write_text(contents, encoding="utf-8")
    registration = {
        "test_roots": ["test.c", "test.rust", "test.zig"],
        "covered_sources": ["test.c", "test.rust", "test.zig"],
    }
    (root / "tests/contract.json").write_text(json.dumps(registration), encoding="utf-8")
    return {
        "name": "demo",
        "build_root": "build",
        "public_header": "inc/demo.h",
        "adapter": "build/adapter.zig",
        "library_name": "demo",
        "symbol_prefix": "demo_",
        "compatibility_sha256": _normalized_header_digest(header),
        "layout_assertions": ["sizeof(demo_config_t) == 4U"],
        "c_include_dirs": ["inc"],
        "target_class": "host-ra8",
        "targets": {"host": [], "ra8": list(RA8_ZIG_ARGUMENTS)},
        "run_host_tests_in_all_modes": True,
        "contract_tests": [
            {
                "language": language,
                "path": f"tests/test.{language}",
                "registration": "tests/contract.json",
            }
            for language in ("c", "rust", "zig")
        ],
        "exports": [
            {
                "name": "demo_run",
                "calling_context": "task-only-non-reentrant",
                "ownership": "borrows input and publishes output",
            }
        ],
    }


def _selftest_target_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise clean inventory plus missing-adapter and missing-target findings."""
    if findings := _library_findings(base, {"host", "ra8"}, root):
        return f"must-stay-quiet fixture failed: {findings}"
    if not any(
        "unregistered Zig export adapter" in item
        for item in _repository_inventory_findings([], [], root)
    ):
        return "must-fire fixture was accepted: omitted adapter"
    host_only = json.loads(json.dumps(base))
    host_only["targets"].pop("ra8")
    findings, _ = _validate(
        {
            "required_targets": ["host", "ra8"],
            "required_modes": sorted(REQUIRED_MODES),
            "mode_test_library": "demo",
            "libraries": [host_only],
        },
        compile_archives=False,
        repository_root=root,
    )
    required = (
        "missing required target classification: ra8",
        "target classification host-ra8 requires",
    )
    return next(
        (
            f"must-fire fixture was accepted: {item}"
            for item in required
            if not any(item in finding for finding in findings)
        ),
        None,
    )


def _selftest_mode_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise optimization matrix completeness and all-mode host-test policy."""
    findings, _ = _validate(
        {
            "required_targets": ["host", "ra8"],
            "required_modes": ["Debug", "ReleaseSafe"],
            "mode_test_library": "demo",
            "libraries": [base],
        },
        compile_archives=False,
        repository_root=root,
    )
    if not any("missing: ReleaseSmall" in item for item in findings):
        return "must-fire fixture was accepted: inventory without ReleaseSmall"
    expected = {(target, mode) for target in ("host", "ra8") for mode in REQUIRED_MODES}
    actual = {(target, mode) for target, _arguments, mode in _matrix_jobs(base, REQUIRED_MODES)}
    if actual != expected:
        return "must-stay-quiet fixture lost a target/mode matrix job"
    no_tests = json.loads(json.dumps(base))
    no_tests["run_host_tests_in_all_modes"] = False
    if not any(
        "run_host_tests_in_all_modes must be true" in item
        for item in _mode_test_policy_findings({"mode_test_library": "demo"}, [no_tests])
    ):
        return "must-fire fixture was accepted: disabled mode tests"
    return None


def _selftest_source_policy(base: dict[str, Any], root: Path, adapter: str) -> str | None:
    """Exercise adapter syntax, executable evidence, and RA8 target identity findings."""
    mutations = {
        "prohibited boundary type": adapter.replace("?*u32", "[]u32"),
        "missing export": adapter.replace("pub export fn", "pub fn"),
        "unexpected export": adapter + "export fn rogue_helper() callconv(.c) void {}\n",
    }
    for label, changed in mutations.items():
        (root / "build/adapter.zig").write_text(changed, encoding="utf-8")
        findings = _library_findings(base, {"host", "ra8"}, root)
        if not any(label.split()[0] in item for item in findings):
            return f"must-fire fixture was accepted: {label}"
    (root / "build/adapter.zig").write_text(adapter, encoding="utf-8")
    c_test = root / "tests/test.c"
    valid = c_test.read_text(encoding="utf-8")
    c_test.write_text("/* demo_run main( */\n", encoding="utf-8")
    findings = _library_findings(base, {"host", "ra8"}, root)
    c_test.write_text(valid, encoding="utf-8")
    if not any("not executable ABI evidence" in item for item in findings):
        return "must-fire fixture was accepted: dead contract test"
    broken = json.loads(json.dumps(base))
    broken["targets"]["ra8"] = ["-Dtarget=thumb-freestanding-eabihf"]
    if not any(
        "does not match RA8D2 CPU/FPU" in item
        for item in _library_findings(broken, {"host", "ra8"}, root)
    ):
        return "must-fire fixture was accepted: mislabeled RA8 target"
    return None


def _selftest_metadata_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise documentation, digest, and mandatory-field findings."""
    for field, expected in (("calling_context", "calling context"), ("ownership", "ownership")):
        broken = json.loads(json.dumps(base))
        broken["exports"][0][field] = ""
        if not any(expected in item for item in _library_findings(broken, {"host", "ra8"}, root)):
            return f"must-fire fixture was accepted: undocumented {field}"
    broken = json.loads(json.dumps(base))
    broken["compatibility_sha256"] = "0" * 64
    if not any(
        "compatibility drift" in item for item in _library_findings(broken, {"host", "ra8"}, root)
    ):
        return "must-fire fixture was accepted: compatibility drift"
    for field, expected in (
        ("public_header", "missing public_header"),
        ("contract_tests", "missing contract tests"),
        ("targets", "missing target evidence"),
    ):
        broken = json.loads(json.dumps(base))
        broken.pop(field)
        if not any(expected in item for item in _library_findings(broken, {"host", "ra8"}, root)):
            return f"must-fire fixture was accepted: {expected}"
    if issue := _selftest_data_exports(base, root):
        return issue
    return _selftest_c_retention(base, root)


def _selftest_data_exports(base: dict[str, Any], root: Path) -> str | None:
    """Exercise the exported-data row in both directions."""
    (root / "build/data.h").write_text("extern DemoState demo_state;\n", encoding="utf-8")
    adapter = root / "build/adapter.zig"
    adapter.write_text(
        adapter.read_text(encoding="utf-8")
        + "const DemoState = extern struct { value: u32 };\n"
        + "pub export var demo_state: DemoState = .{ .value = 0 };\n",
        encoding="utf-8",
    )
    quiet = json.loads(json.dumps(base))
    quiet["data_exports"] = [
        {
            "name": "demo_state",
            "header": "build/data.h",
            "adapter": "build/adapter.zig",
            "calling_context": "task-only-non-reentrant",
            "ownership": "owns one shared mutable demo state",
        }
    ]
    if any(
        "demo_state" in item or "data export" in item
        for item in _library_findings(quiet, {"host", "ra8"}, root)
    ):
        return "must-stay-quiet fixture failed: declared exported data read as drift"
    mutations = (
        ({"adapter": "build/missing.zig"}, "missing data export adapter"),
        ({"header": "build/build.zig"}, "data export header does not declare"),
        ({"calling_context": "whenever"}, "undocumented calling context"),
        ({"ownership": "x"}, "undocumented ownership"),
    )
    for patch, expected in mutations:
        broken = json.loads(json.dumps(quiet))
        broken["data_exports"][0].update(patch)
        if not any(expected in item for item in _library_findings(broken, {"host", "ra8"}, root)):
            return f"must-fire fixture was accepted: {expected}"
    return None


def _selftest_c_retention(base: dict[str, Any], root: Path) -> str | None:
    """Exercise the retained-C exception in both directions."""
    reason = "support translation unit retained by a later change on the integration branch"
    (root / "inc/demo.h").write_text(
        "int demo_run(void);\nint demo_bind(void);\n", encoding="utf-8"
    )
    (root / "demo_bind.c").write_text("int demo_bind(void) { return 0; }\n", encoding="utf-8")
    (root / "demo_other.c").write_text("int demo_other(void) { return 0; }\n", encoding="utf-8")
    quiet = json.loads(json.dumps(base))
    quiet["compatibility_sha256"] = _normalized_header_digest(
        (root / "inc/demo.h").read_text(encoding="utf-8")
    )
    quiet["c_retained_exports"] = [{"name": "demo_bind", "source": "demo_bind.c", "reason": reason}]
    if any("unexpected export" in item for item in _library_findings(quiet, {"host", "ra8"}, root)):
        return "must-stay-quiet fixture failed: declared C retention read as header drift"
    if not any(
        "header unexpected export" in item
        for item in _library_findings({**quiet, "c_retained_exports": []}, {"host", "ra8"}, root)
    ):
        return "must-fire fixture was accepted: undeclared C-implemented header symbol"
    mutations = (
        ([{"name": "demo_absent", "source": "demo_bind.c", "reason": reason}], "stale C retention"),
        (
            [{"name": "demo_bind", "source": "demo_gone.c", "reason": reason}],
            "missing C retention source",
        ),
        (
            [{"name": "demo_bind", "source": "demo_other.c", "reason": reason}],
            "C retention source does not define",
        ),
        (
            [{"name": "demo_bind", "source": "demo_bind.c", "reason": "x"}],
            "undocumented C retention",
        ),
    )
    for rows, expected in mutations:
        broken = json.loads(json.dumps(quiet))
        broken["c_retained_exports"] = rows
        if not any(expected in item for item in _library_findings(broken, {"host", "ra8"}, root)):
            return f"must-fire fixture was accepted: {expected}"
    return None


def _selftest_compiled_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise compiled target counts and both symbol-drift directions when tools exist."""
    zig = shutil.which("zig")
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if zig is None or nm is None:
        return None
    compiled = json.loads(json.dumps(base))
    compiled["exports"][0]["name"] = "demo_missing"
    compiled["run_host_tests_in_all_modes"] = False
    findings, counts = _compiled_findings(compiled, zig, nm, {"Debug"}, root)
    expected_count = len(compiled["targets"])
    if counts["c_matrix"] != expected_count or counts["zig_matrix"] != expected_count:
        return (
            f"must-stay-quiet fixture missed compiled targets: counts={counts}, findings={findings}"
        )
    for expected in ("compiled archive missing export", "compiled archive unexpected export"):
        if not any(expected in item for item in findings):
            return f"must-fire fixture was accepted: {expected}"
    return None


def _selftest_layout_assertions() -> str | None:
    """Prove assertion matching tolerates layout without accepting false evidence."""
    fragment = "sizeof(demo_config_t) == 4U"
    valid = 'static_assert(sizeof(demo_config_t) ==\n  4U, "layout");\n'
    policy = {
        "compatibility_sha256": _normalized_header_digest(valid),
        "layout_assertions": [fragment],
    }
    if _compatibility_findings(policy, "demo", valid, ""):
        return "must-stay-quiet reformatted layout assertion was rejected"
    invalid = (
        (f"/* {fragment} */\n", "", "comment-only layout assertion"),
        (f"#if 0\n{fragment}\n#endif\n", "", "disabled layout assertion"),
        (f"#if (0)\n{fragment}\n#endif\n", "", "parenthesized-disabled layout assertion"),
        (f"#ifdef NEVER_DEFINED\n{fragment}\n#endif\n", "", "conditional layout assertion"),
        (f'const char *evidence = "{fragment}";\n', "", "string-only layout assertion"),
        ("size of(demo_config_t) == 4U;\n", "", "split-token layout assertion"),
        ("sizeof(demo_", "config_t) == 4U;\n", "cross-file layout assertion"),
    )
    for header, adapter, label in invalid:
        findings = _compatibility_findings(policy, "demo", header, adapter)
        if not any("missing representation assertion" in finding for finding in findings):
            return f"must-fire fixture was accepted: {label}"
    zig_fragment = "@sizeOf(AbiError) != 2"
    if _contains_token_sequence(f"const evidence = \\\\{zig_fragment};\n", zig_fragment):
        return "must-fire fixture was accepted: Zig multiline-string layout assertion"
    return None


def _selftest_source_inventory(root: Path, base: dict[str, Any]) -> str | None:
    """Reject omitted sources and symbol drift while accepting exact registration."""
    source = root / "libs/demo/src/adapter.zig"
    source.parent.mkdir(parents=True)
    source.write_text("pub export fn demo_run() callconv(.c) void {}\n", encoding="utf-8")
    row = {"path": "libs/demo/src/adapter.zig", "kind": "library-adapter", "symbols": ["demo_run"]}
    library = {"name": "demo", "build_root": "libs/demo", "adapter": row["path"]}
    base_row = {
        "path": base["adapter"],
        "kind": "library-adapter",
        "symbols": ["demo_run"],
    }
    if findings := _repository_inventory_findings([base, library], [base_row, row], root):
        return f"must-stay-quiet fixture failed: exact export source inventory: {findings}"
    wrong = {**row, "symbols": ["demo_other"]}
    if not any(
        "declared symbol inventory differs" in item
        for item in _repository_inventory_findings([base, library], [base_row, wrong], root)
    ):
        return "must-fire fixture was accepted: unknown/mismatched export symbol"
    if not any(
        "unregistered Zig export adapter" in item
        for item in _repository_inventory_findings([], [], root)
    ):
        return "must-fire fixture was accepted: unregistered exported source"
    return None


def _selftest_archive_symbols() -> str | None:
    """Prove every nm archive-member spelling parses and runtime filtering is exact.

    nm prints an archive member three ways depending on build and platform:
    ``lib.a(member.o):``, ``lib.a[member.o]:`` and a bare colon form carrying
    the member's cache path. All three have to yield the same symbol set, and
    a runtime-named symbol defined outside a runtime member must still be seen.
    """
    parsed = _archive_symbol_names(
        "libdemo.a(adapter.o): 00000000 T demo_run\n"
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    if parsed != {"demo_run"}:
        return f"compiler runtime member filtering was not exact: {sorted(parsed)}"
    retained = _archive_symbol_names(
        "libdemo.a(adapter.o): 00000000 T __zig_probe_stack\n"
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    if retained != {"_zig_probe_stack"}:
        return "must-fire fixture was accepted: runtime-named symbol in non-runtime member"
    bracket_member = _archive_symbol_names(
        "libdemo.a[adapter.o]: 00000000 T demo_run\n"
        "libdemo.a[compiler_rt.o]: 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    if bracket_member != {"demo_run"}:
        return f"bracket-form nm archive members were not parsed: {sorted(bracket_member)}"
    colon_member = _archive_symbol_names(
        "libdemo.a:/opt/zig-cache/compiler_rt.o:00000000 W __zig_probe_stack\n"
        "libdemo.a:/opt/zig-cache/adapter.o:00000000 T demo_run\n",
        bundle_compiler_rt=True,
    )
    if colon_member != {"demo_run"}:
        return f"colon-form nm archive members were not parsed: {sorted(colon_member)}"
    unbundled = _archive_symbol_names(
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=False,
    )
    if unbundled != {"_zig_probe_stack"}:
        return "must-fire fixture was accepted: unconfigured compiler runtime member"
    return None


def _selftest() -> int:
    """Prove every advertised policy finding fires and compliant input stays quiet."""
    header = """typedef struct { unsigned value; } demo_config_t;
int demo_run(const demo_config_t *config, unsigned *output);
static_assert(sizeof(demo_config_t) == 4U, "layout");
"""
    adapter = (
        "const DemoConfig = extern struct { value: u32 };\n"
        "pub export fn demo_run(config: ?*const DemoConfig, output: ?*u32) "
        "callconv(.c) i32 { _ = config; _ = output; return 0; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="ra8-zig-abi-selftest-") as tmp:
        root = Path(tmp).resolve()

        base = _selftest_fixture(root, header, adapter)
        checks = (
            lambda: _selftest_target_policy(base, root),
            lambda: _selftest_mode_policy(base, root),
            lambda: _selftest_source_policy(base, root, adapter),
            lambda: _selftest_metadata_policy(base, root),
            lambda: _selftest_compiled_policy(base, root),
            _selftest_layout_assertions,
            lambda: _selftest_source_inventory(root, base),
            _selftest_archive_symbols,
        )
        for check in checks:
            if error := check():
                print(error, file=sys.stderr)
                return 1
    print("check_zig_abi_policy.py --selftest: OK (quiet + all named failure classes).")
    return 0

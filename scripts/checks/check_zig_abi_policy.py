#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the declared Zig-to-C ABI inventory against source and archives."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "config/zig_abi_policy.json"
CONTEXTS = {
    "boot-only",
    "isr-safe",
    "task-safe-reentrant",
    "task-only-non-reentrant",
    "task-only-reentrancy-guarded",
    "serialized-test-control",
}
MIN_OWNERSHIP_LENGTH = 12
MIN_NM_SYMBOL_FIELDS = 3
REQUIRED_MODES = {"Debug", "ReleaseSafe", "ReleaseSmall"}
MODE_C_FLAGS = {"Debug": "-O0", "ReleaseSafe": "-O2", "ReleaseSmall": "-Oz"}
RA8_ZIG_ARGUMENTS = (
    "-Dtarget=thumb-freestanding-eabihf",
    "-Dcpu=cortex_m85+fp_armv8-d32-fp64",
)
RA8_C_ARGUMENTS = ("-mcpu=cortex_m85", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
GENERATED_PATH_PARTS = {".zig-cache", "zig-out"}
REPOSITORY_EXCLUDED_PATH_PARTS = {*GENERATED_PATH_PARTS, "third_party"}
PROHIBITED_ZIG_TYPES = (
    (re.compile(r"\[\](?:const\s+)?"), "slice"),
    (re.compile(r"(?<![=!])!(?!=)"), "error union"),
    (re.compile(r"\bbool\b"), "bool"),
    (re.compile(r"\banytype\b"), "anytype"),
    (re.compile(r"\bcomptime\b"), "comptime"),
)


class PolicyError(Exception):
    """Raised when the policy file itself is unreadable or malformed."""


def _strip_comments(text: str) -> str:
    """Remove C/Zig comments before inventory extraction."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n\r]*", "", text)


def _normalized_header_digest(text: str) -> str:
    """Hash representation-bearing header text without comments or whitespace."""
    normalized = re.sub(r"\s+", "", _strip_comments(text))
    return hashlib.sha256(normalized.encode()).hexdigest()


def _header_exports(text: str, prefix: str) -> set[str]:
    """Extract namespaced function declarations from a public C header."""
    return set(re.findall(rf"\b({re.escape(prefix)}[A-Za-z0-9_]+)\s*\(", _strip_comments(text)))


def _zig_exports(text: str) -> tuple[set[str], dict[str, str]]:
    """Extract exported Zig names and complete declaration heads."""
    clean = _strip_comments(text)
    matches = list(re.finditer(r"\b(?:pub\s+)?export\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)", clean))
    heads: dict[str, str] = {}
    for match in matches:
        brace = clean.find("{", match.end())
        heads[match.group(1)] = clean[match.start() : brace if brace >= 0 else len(clean)]
    return set(heads), heads


def _load_policy(path: Path = POLICY) -> dict[str, Any]:
    """Load the policy document or raise a named validation error."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        message = f"policy is unreadable: {exc}"
        raise PolicyError(message) from exc
    if not isinstance(data, dict):
        message = "policy root must be an object"
        raise PolicyError(message)
    return data


def _metadata_findings(name: str, metadata: object) -> tuple[list[str], set[str]]:
    """Validate per-export classifications and return declared names."""
    if not isinstance(metadata, list):
        return [f"{name}: exports must be a list"], set()
    findings: list[str] = []
    declared: set[str] = set()
    for row in metadata:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            findings.append(f"{name}: malformed export metadata")
            continue
        symbol = row["name"]
        declared.add(symbol)
        if row.get("calling_context") not in CONTEXTS:
            findings.append(f"{name}: undocumented calling context: {symbol}")
        ownership = row.get("ownership")
        if not isinstance(ownership, str) or len(ownership.strip()) < MIN_OWNERSHIP_LENGTH:
            findings.append(f"{name}: undocumented ownership: {symbol}")
    return findings, declared


def _inventory_findings(
    name: str,
    declared: set[str],
    header_names: set[str],
    zig_names: set[str],
    zig_heads: dict[str, str],
) -> list[str]:
    """Compare metadata, header, and Zig declarations and reject native-only types."""
    findings: list[str] = []
    for label, actual in (("header", header_names), ("Zig adapter", zig_names)):
        missing = sorted(declared - actual)
        unexpected = sorted(actual - declared)
        if missing:
            findings.append(f"{name}: {label} missing export(s): {', '.join(missing)}")
        if unexpected:
            findings.append(f"{name}: {label} unexpected export(s): {', '.join(unexpected)}")
    for symbol in sorted(declared & zig_names):
        head = zig_heads[symbol]
        if "callconv(.c)" not in re.sub(r"\s+", "", head):
            findings.append(f"{name}: export lacks callconv(.c): {symbol}")
        findings.extend(
            f"{name}: prohibited Zig-only type {type_name}: {symbol}"
            for pattern, type_name in PROHIBITED_ZIG_TYPES
            if pattern.search(head)
        )
    return findings


def _adapter_scope_findings(
    name: str, build_root: Path, adapter: Path, repository_root: Path = ROOT
) -> list[str]:
    """Reject exports declared outside the one registered adapter."""
    findings: list[str] = []
    for source in build_root.rglob("*.zig"):
        generated = any(part in GENERATED_PATH_PARTS for part in source.parts)
        if source.resolve() == adapter or generated:
            continue
        other_names, _ = _zig_exports(source.read_text(encoding="utf-8"))
        if other_names:
            relative = source.relative_to(repository_root)
            findings.append(
                f"{name}: export outside adapter {relative}: " + ", ".join(sorted(other_names))
            )
    return findings


def _repository_inventory_findings(
    libraries: list[dict[str, Any]], repository_root: Path = ROOT
) -> list[str]:
    """Require every hand-written Zig export adapter in the repository inventory."""
    registered = {
        (repository_root / value).resolve()
        for library in libraries
        if isinstance(library, dict) and isinstance((value := library.get("adapter")), str)
    }
    discovered: set[Path] = set()
    findings: list[str] = []
    for source in repository_root.rglob("*.zig"):
        if any(part in REPOSITORY_EXCLUDED_PATH_PARTS for part in source.parts):
            continue
        text = source.read_text(encoding="utf-8")
        names, _ = _zig_exports(text)
        if names or re.search(r"\b@export\s*\(", _strip_comments(text)):
            discovered.add(source.resolve())
        if re.search(r"\b@export\s*\(", _strip_comments(text)):
            findings.append(
                "dynamic @export is prohibited at ABI boundaries: "
                f"{source.relative_to(repository_root)}"
            )
    findings.extend(
        f"unregistered Zig export adapter: {source.relative_to(repository_root)}"
        for source in sorted(discovered - registered)
    )
    findings.extend(
        f"registered adapter has no Zig export: {source.relative_to(repository_root)}"
        for source in sorted(registered - discovered)
    )
    return findings


def _compatibility_findings(
    library: dict[str, Any], name: str, header_text: str, adapter_text: str
) -> list[str]:
    """Check the released normalized header and required representation assertions."""
    findings: list[str] = []
    expected = library.get("compatibility_sha256")
    actual = _normalized_header_digest(header_text)
    if expected != actual:
        findings.append(f"{name}: compatibility drift: expected {expected}, got {actual}")
    findings.extend(
        f"{name}: missing representation assertion: {fragment}"
        for fragment in library.get("layout_assertions", [])
        if not isinstance(fragment, str)
        or (fragment not in header_text and fragment not in adapter_text)
    )
    return findings


def _contract_test_findings(  # noqa: PLR0912 -- validates every malformed evidence branch
    library: dict[str, Any], name: str, prefix: str, repository_root: Path = ROOT
) -> list[str]:
    """Bind ABI tests to the language test manifests or CMake test graph."""
    tests = library.get("contract_tests")
    if not isinstance(tests, list) or not tests:
        return [f"{name}: missing contract tests"]
    findings: list[str] = []
    seen: set[str] = set()
    required_tokens = {"c": "main(", "rust": "#[test]", "zig": 'test"'}
    for row in tests:
        if not isinstance(row, dict):
            findings.append(f"{name}: malformed contract test metadata")
            continue
        language = row.get("language")
        path_value = row.get("path")
        registration_value = row.get("registration")
        symbol_value = row.get("symbol_evidence", path_value)
        if language not in required_tokens or not isinstance(path_value, str):
            findings.append(f"{name}: malformed contract test metadata")
            continue
        seen.add(language)
        path = repository_root / path_value
        symbol_path = repository_root / symbol_value if isinstance(symbol_value, str) else path
        registration = (
            repository_root / registration_value if isinstance(registration_value, str) else None
        )
        if not path.is_file():
            findings.append(f"{name}: missing {language} contract test: {path_value}")
            continue
        clean = _strip_comments(path.read_text(encoding="utf-8"))
        symbol_text = (
            _strip_comments(symbol_path.read_text(encoding="utf-8"))
            if symbol_path.is_file()
            else ""
        )
        if prefix not in symbol_text or required_tokens[language] not in re.sub(r"\s+", "", clean):
            findings.append(f"{name}: {language} contract test is not executable ABI evidence")
        if registration is None or not registration.is_file():
            findings.append(f"{name}: missing {language} test registration: {registration_value}")
            continue
        registration_text = _strip_comments(registration.read_text(encoding="utf-8"))
        if registration.suffix == ".json":
            try:
                contract = json.loads(registration_text)
            except json.JSONDecodeError:
                findings.append(f"{name}: malformed {language} test registration")
                continue
            relative = path.relative_to(registration.parent).as_posix()
            registered_sources = set(contract.get("test_roots", [])) | set(
                contract.get("covered_sources", [])
            )
            if relative not in registered_sources:
                findings.append(f"{name}: {language} contract test is not registered: {path_value}")
            if symbol_path != path:
                symbol_relative = symbol_path.relative_to(registration.parent).as_posix()
                if symbol_relative not in registered_sources:
                    findings.append(
                        f"{name}: {language} symbol evidence is not registered: {symbol_value}"
                    )
        elif path.name not in registration_text:
            findings.append(f"{name}: C contract test is not registered: {path_value}")
    if "c" not in seen or "zig" not in seen:
        findings.append(f"{name}: contract tests must include C and Zig")
    return findings


def _target_findings(library: dict[str, Any], name: str, required_targets: set[str]) -> list[str]:
    """Validate explicit Zig build arguments for every supported target class."""
    targets = library.get("targets")
    if not isinstance(targets, dict) or not targets:
        return [f"{name}: missing target evidence"]
    findings: list[str] = []
    target_class = library.get("target_class")
    expected = {"host", "ra8"} if target_class == "host-ra8" else {"host"}
    if target_class not in {"host-only", "host-ra8"}:
        findings.append(f"{name}: missing supported-target classification")
    elif set(targets) != expected:
        findings.append(
            f"{name}: target classification {target_class} requires: {', '.join(sorted(expected))}"
        )
    for target, arguments in targets.items():
        if target not in required_targets:
            findings.append(f"{name}: unexpected target classification: {target}")
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            findings.append(f"{name}: malformed build arguments for target: {target}")
        if target == "ra8" and tuple(arguments) != RA8_ZIG_ARGUMENTS:
            findings.append(f"{name}: RA8 target evidence does not match RA8D2 CPU/FPU")
    return findings


def _library_findings(
    library: dict[str, Any], required_targets: set[str], repository_root: Path = ROOT
) -> list[str]:
    """Return source, metadata, test, and compatibility findings for one library."""
    findings: list[str] = []
    name = library.get("name", "<unnamed>")
    prefix = library.get("symbol_prefix")
    if not isinstance(prefix, str) or not prefix:
        return [f"{name}: missing symbol_prefix"]
    paths: dict[str, Path] = {}
    for field in ("build_root", "public_header", "adapter"):
        value = library.get(field)
        path = repository_root / value if isinstance(value, str) else repository_root / "<missing>"
        paths[field] = path
        if not path.exists():
            findings.append(f"{name}: missing {field}: {value}")
    if findings:
        return findings

    header_text = paths["public_header"].read_text(encoding="utf-8")
    adapter_text = paths["adapter"].read_text(encoding="utf-8")
    header_names = _header_exports(header_text, prefix)
    zig_names, zig_heads = _zig_exports(adapter_text)
    metadata_findings, declared = _metadata_findings(name, library.get("exports"))
    findings.extend(metadata_findings)
    findings.extend(_inventory_findings(name, declared, header_names, zig_names, zig_heads))
    findings.extend(
        _adapter_scope_findings(
            name,
            paths["build_root"].resolve(),
            paths["adapter"].resolve(),
            repository_root,
        )
    )
    findings.extend(_compatibility_findings(library, name, header_text, adapter_text))
    findings.extend(_contract_test_findings(library, name, prefix, repository_root))
    findings.extend(_target_findings(library, name, required_targets))
    return findings


def _c_target_arguments(target: str, arguments: list[str]) -> list[str]:
    """Translate registered Zig target selections to Zig C compiler flags."""
    if target == "ra8":
        return ["-target", "thumb-freestanding-eabihf", *RA8_C_ARGUMENTS]
    translated: list[str] = []
    for argument in arguments:
        if argument.startswith("-Dtarget="):
            translated.extend(("-target", argument.removeprefix("-Dtarget=")))
        elif argument.startswith("-Dcpu="):
            cpu = argument.removeprefix("-Dcpu=")
            translated.append(f"-mcpu={cpu}")
    return translated


def _c_compile_findings(
    library: dict[str, Any],
    zig: str,
    job: tuple[str, list[str], str],
    output: Path,
    repository_root: Path = ROOT,
) -> list[str]:
    """Compile the public C header under the same target and optimization mode."""
    target, _arguments, mode = job
    includes = library.get("c_include_dirs")
    if not isinstance(includes, list) or not includes:
        return [f"{library['name']}: missing C include directories"]
    probe = output / "abi_header_probe.c"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text(f'#include "{Path(library["public_header"]).name}"\n', encoding="utf-8")
    command = [
        zig,
        "cc",
        "-std=c2x",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-c",
        MODE_C_FLAGS[mode],
        *_c_target_arguments(target, library["targets"][target]),
        *(item for include in includes for item in ("-I", str(repository_root / include))),
        str(probe),
        "-o",
        str(output / "abi_header_probe.o"),
    ]
    proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy arguments
        command, cwd=repository_root, capture_output=True, text=True, check=False
    )
    if proc.returncode == 0:
        return []
    detail = (proc.stdout + proc.stderr).strip()
    return [f"{library['name']}: {target}/{mode} C header compile failed: {detail}"]


def _matrix_jobs(
    library: dict[str, Any], required_modes: set[str]
) -> list[tuple[str, list[str], str]]:
    """Return the complete deterministic target-by-mode build matrix."""
    return [
        (target, arguments, mode)
        for target, arguments in library["targets"].items()
        for mode in sorted(required_modes)
    ]


def _compiled_findings(
    library: dict[str, Any],
    zig: str,
    nm: str,
    required_modes: set[str],
    repository_root: Path = ROOT,
) -> tuple[list[str], dict[str, int]]:
    """Build every registered target/mode and compare all global exports exactly."""
    name = library["name"]
    findings: list[str] = []
    counts = {"c_matrix": 0, "zig_matrix": 0, "mode_tests": 0}
    with tempfile.TemporaryDirectory(prefix="ra8-zig-abi-") as tmp:
        for target, arguments, mode in _matrix_jobs(library, required_modes):
            output = Path(tmp) / target / mode
            c_findings = _c_compile_findings(
                library, zig, (target, arguments, mode), output, repository_root
            )
            findings.extend(c_findings)
            counts["c_matrix"] += int(not c_findings)
            command = [
                zig,
                "build",
                "--build-file",
                str(repository_root / library["build_root"] / "build.zig"),
                "--prefix",
                str(output / "install"),
                "--cache-dir",
                str(Path(tmp) / "cache"),
                "--global-cache-dir",
                str(Path(tmp) / "global-cache"),
                *arguments,
                f"-Doptimize={mode}",
            ]
            proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy arguments
                command, cwd=repository_root, capture_output=True, text=True, check=False
            )
            if proc.returncode != 0:
                detail = (proc.stdout + proc.stderr).strip()
                findings.append(f"{name}: {target}/{mode} archive build failed: {detail}")
                continue
            archive = output / "install" / "lib" / f"lib{library['library_name']}.a"
            if not archive.is_file():
                findings.append(f"{name}: {target}/{mode} archive missing: {archive}")
                continue
            proc = subprocess.run(  # noqa: S603 -- resolved tool; fresh archive
                [nm, "-g", "--defined-only", str(archive)],
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                detail = proc.stderr.strip()
                findings.append(f"{name}: {target}/{mode} symbol scan failed: {detail}")
                continue
            actual = {
                parts[-1].removeprefix("_")
                for line in proc.stdout.splitlines()
                if len(parts := line.split()) >= MIN_NM_SYMBOL_FIELDS
            }
            expected = {row["name"] for row in library["exports"]}
            findings.extend(_compiled_symbol_findings(f"{name}: {target}/{mode}", expected, actual))
            counts["zig_matrix"] += 1
            if target == "host" and library.get("run_host_tests_in_all_modes") is True:
                test_proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy
                    [*command, "test"],
                    cwd=repository_root,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if test_proc.returncode != 0:
                    detail = (test_proc.stdout + test_proc.stderr).strip()
                    findings.append(f"{name}: host/{mode} ABI tests failed: {detail}")
                else:
                    counts["mode_tests"] += 1
    return findings, counts


def _compiled_symbol_findings(name: str, expected: set[str], actual: set[str]) -> list[str]:
    """Report both directions of drift in a compiled archive's public symbols."""
    findings: list[str] = []
    if expected - actual:
        missing = ", ".join(sorted(expected - actual))
        findings.append(f"{name}: compiled archive missing export(s): {missing}")
    if actual - expected:
        unexpected = ", ".join(sorted(actual - expected))
        findings.append(f"{name}: compiled archive unexpected export(s): {unexpected}")
    return findings


def _mode_test_policy_findings(
    policy: dict[str, Any], libraries: list[dict[str, Any]]
) -> list[str]:
    """Require one named canonical fixture to run tests in every host mode."""
    selected = policy.get("mode_test_library")
    matches = [library for library in libraries if library.get("name") == selected]
    if not isinstance(selected, str) or len(matches) != 1:
        return ["policy must name exactly one mode_test_library"]
    library = matches[0]
    if library.get("run_host_tests_in_all_modes") is not True:
        return [f"{selected}: run_host_tests_in_all_modes must be true"]
    if "host" not in library.get("targets", {}):
        return [f"{selected}: mode-test library must support host"]
    return []


def _validate(
    policy: dict[str, Any], *, compile_archives: bool, repository_root: Path = ROOT
) -> tuple[list[str], dict[str, int]]:
    """Validate the complete policy and return findings plus non-vacuity counts."""
    findings: list[str] = []
    required = policy.get("required_targets")
    required_targets = set(required) if isinstance(required, list) else set()
    if required_targets != {"host", "ra8"}:
        findings.append("policy required_targets must contain exactly host and ra8")
    modes = policy.get("required_modes")
    required_modes = set(modes) if isinstance(modes, list) else set()
    if required_modes != REQUIRED_MODES:
        missing = ", ".join(sorted(REQUIRED_MODES - required_modes)) or "none"
        unexpected = ", ".join(sorted(required_modes - REQUIRED_MODES)) or "none"
        findings.append(
            "policy optimization modes must be Debug, ReleaseSafe, ReleaseSmall "
            f"(missing: {missing}; unexpected: {unexpected})"
        )
    libraries = policy.get("libraries")
    if not isinstance(libraries, list) or not libraries:
        return [*findings, "policy must inventory at least one ABI library"], {}
    zig = shutil.which("zig") if compile_archives else None
    nm = (shutil.which("llvm-nm") or shutil.which("nm")) if compile_archives else None
    if compile_archives and (zig is None or nm is None):
        findings.append("compiled policy check requires zig and llvm-nm or nm")
    counts = {
        "libraries": len(libraries),
        "headers": 0,
        "adapters": 0,
        "exports": 0,
        "tests": 0,
        "targets": 0,
        "modes": len(required_modes & REQUIRED_MODES),
        "c_matrix": 0,
        "zig_matrix": 0,
        "mode_tests": 0,
    }
    typed_libraries = [library for library in libraries if isinstance(library, dict)]
    findings.extend(_repository_inventory_findings(typed_libraries, repository_root))
    findings.extend(_mode_test_policy_findings(policy, typed_libraries))
    target_classes = {
        target
        for library in typed_libraries
        if isinstance((targets := library.get("targets")), dict)
        for target in targets
    }
    findings.extend(
        f"missing required target classification: {target}"
        for target in sorted(required_targets - target_classes)
    )
    counts["targets"] = len(target_classes & required_targets)
    for library in libraries:
        if not isinstance(library, dict):
            findings.append("policy library rows must be objects")
            continue
        findings.extend(_library_findings(library, required_targets, repository_root))
        counts["headers"] += int(isinstance(library.get("public_header"), str))
        counts["adapters"] += int(isinstance(library.get("adapter"), str))
        counts["exports"] += len(library.get("exports", []))
        counts["tests"] += len(library.get("contract_tests", []))
        if compile_archives and zig is not None and nm is not None:
            compiled_findings, compiled_counts = _compiled_findings(
                library, zig, nm, required_modes & REQUIRED_MODES, repository_root
            )
            findings.extend(compiled_findings)
            for key, value in compiled_counts.items():
                counts[key] += value
    floors = {
        "libraries": 1,
        "headers": 1,
        "adapters": 1,
        "exports": 1,
        "tests": 3,
        "targets": 2,
        "modes": 3,
    }
    for item, floor in floors.items():
        if counts[item] < floor:
            findings.append(f"non-vacuity failure: {item}={counts[item]}, floor={floor}")
    if compile_archives:
        expected_matrix = sum(
            len(library.get("targets", {})) * len(REQUIRED_MODES) for library in typed_libraries
        )
        findings.extend(
            f"non-vacuity failure: {key}={counts[key]}, expected={expected_matrix}"
            for key in ("c_matrix", "zig_matrix")
            if counts[key] != expected_matrix
        )
        if counts["mode_tests"] != len(REQUIRED_MODES):
            findings.append(
                f"non-vacuity failure: mode_tests={counts['mode_tests']}, "
                f"expected={len(REQUIRED_MODES)}"
            )
    return findings, counts


def _selftest() -> int:  # noqa: C901,PLR0911,PLR0912,PLR0915 -- explicit must-fire matrix
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
        test_contents = {
            "c": "int demo_run(void); int main(void) { return demo_run(); }\n",
            "rust": 'extern "C" { fn demo_run(); } #[test] fn calls() { unsafe { demo_run() } }\n',
            "zig": 'extern fn demo_run() void; test "calls" { demo_run(); }\n',
        }
        for language, contents in test_contents.items():
            (root / f"tests/test.{language}").write_text(contents, encoding="utf-8")
        registration = {
            "test_roots": ["test.c", "test.rust", "test.zig"],
            "covered_sources": ["test.c", "test.rust", "test.zig"],
        }
        (root / "tests/contract.json").write_text(json.dumps(registration), encoding="utf-8")

        def fixture_library_findings(
            library: dict[str, Any], required_targets: set[str]
        ) -> list[str]:
            """Validate a selftest library within the isolated fixture root."""
            return _library_findings(library, required_targets, root)

        try:
            base: dict[str, Any] = {
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
                "targets": {
                    "host": [],
                    "ra8": list(RA8_ZIG_ARGUMENTS),
                },
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
            if findings := fixture_library_findings(base, {"host", "ra8"}):
                print(f"must-stay-quiet fixture failed: {findings}", file=sys.stderr)
                return 1
            if not any(
                "unregistered Zig export adapter" in item
                for item in _repository_inventory_findings([], root)
            ):
                print("must-fire fixture was accepted: omitted adapter", file=sys.stderr)
                return 1
            host_only = json.loads(json.dumps(base))
            host_only["targets"].pop("ra8")
            target_findings, _ = _validate(
                {
                    "required_targets": ["host", "ra8"],
                    "required_modes": sorted(REQUIRED_MODES),
                    "mode_test_library": "demo",
                    "libraries": [host_only],
                },
                compile_archives=False,
                repository_root=root,
            )
            if not any(
                "missing required target classification: ra8" in item for item in target_findings
            ):
                print("must-fire fixture was accepted: inventory without RA8", file=sys.stderr)
                return 1
            if not any(
                "target classification host-ra8 requires" in item for item in target_findings
            ):
                print(
                    "must-fire fixture was accepted: host-ra8 library without RA8", file=sys.stderr
                )
                return 1
            mode_findings, _ = _validate(
                {
                    "required_targets": ["host", "ra8"],
                    "required_modes": ["Debug", "ReleaseSafe"],
                    "mode_test_library": "demo",
                    "libraries": [base],
                },
                compile_archives=False,
                repository_root=root,
            )
            if not any("missing: ReleaseSmall" in item for item in mode_findings):
                print(
                    "must-fire fixture was accepted: inventory without ReleaseSmall",
                    file=sys.stderr,
                )
                return 1
            expected_jobs = {
                (target, mode) for target in ("host", "ra8") for mode in REQUIRED_MODES
            }
            actual_jobs = {
                (target, mode) for target, _arguments, mode in _matrix_jobs(base, REQUIRED_MODES)
            }
            if actual_jobs != expected_jobs:
                print("must-stay-quiet fixture lost a target/mode matrix job", file=sys.stderr)
                return 1
            no_mode_tests = json.loads(json.dumps(base))
            no_mode_tests["run_host_tests_in_all_modes"] = False
            if not any(
                "run_host_tests_in_all_modes must be true" in item
                for item in _mode_test_policy_findings(
                    {"mode_test_library": "demo"}, [no_mode_tests]
                )
            ):
                print("must-fire fixture was accepted: disabled mode tests", file=sys.stderr)
                return 1
            mutations = {
                "prohibited boundary type": adapter.replace("?*u32", "[]u32"),
                "missing export": adapter.replace("pub export fn", "pub fn"),
                "unexpected export": adapter + "export fn rogue_helper() callconv(.c) void {}\n",
            }
            for label, changed in mutations.items():
                (root / "build/adapter.zig").write_text(changed, encoding="utf-8")
                if not any(
                    label.split()[0] in item
                    for item in fixture_library_findings(base, {"host", "ra8"})
                ):
                    print(f"must-fire fixture was accepted: {label}", file=sys.stderr)
                    return 1
            (root / "build/adapter.zig").write_text(adapter, encoding="utf-8")
            c_test = root / "tests/test.c"
            valid_c_test = c_test.read_text(encoding="utf-8")
            c_test.write_text("/* demo_run main( */\n", encoding="utf-8")
            if not any(
                "not executable ABI evidence" in item
                for item in fixture_library_findings(base, {"host", "ra8"})
            ):
                print("must-fire fixture was accepted: dead contract test", file=sys.stderr)
                return 1
            c_test.write_text(valid_c_test, encoding="utf-8")
            broken_target = json.loads(json.dumps(base))
            broken_target["targets"]["ra8"] = ["-Dtarget=thumb-freestanding-eabihf"]
            if not any(
                "does not match RA8D2 CPU/FPU" in item
                for item in fixture_library_findings(broken_target, {"host", "ra8"})
            ):
                print("must-fire fixture was accepted: mislabeled RA8 target", file=sys.stderr)
                return 1
            for field, expected in (
                ("calling_context", "calling context"),
                ("ownership", "ownership"),
            ):
                broken = json.loads(json.dumps(base))
                broken["exports"][0][field] = ""
                if not any(
                    expected in item for item in fixture_library_findings(broken, {"host", "ra8"})
                ):
                    print(f"must-fire fixture was accepted: undocumented {field}", file=sys.stderr)
                    return 1
            broken = json.loads(json.dumps(base))
            broken["compatibility_sha256"] = "0" * 64
            if not any(
                "compatibility drift" in item
                for item in fixture_library_findings(broken, {"host", "ra8"})
            ):
                print("must-fire fixture was accepted: compatibility drift", file=sys.stderr)
                return 1
            for field, expected in (
                ("public_header", "missing public_header"),
                ("contract_tests", "missing contract tests"),
                ("targets", "missing target evidence"),
            ):
                broken = json.loads(json.dumps(base))
                broken.pop(field)
                if not any(
                    expected in item for item in fixture_library_findings(broken, {"host", "ra8"})
                ):
                    print(f"must-fire fixture was accepted: {expected}", file=sys.stderr)
                    return 1
            zig = shutil.which("zig")
            nm = shutil.which("llvm-nm") or shutil.which("nm")
            if zig is not None and nm is not None:
                compiled = json.loads(json.dumps(base))
                compiled["exports"][0]["name"] = "demo_missing"
                compiled["run_host_tests_in_all_modes"] = False
                compiled_findings, compiled_counts = _compiled_findings(
                    compiled, zig, nm, {"Debug"}, root
                )
                expected_compiled = len(compiled["targets"])
                if (
                    compiled_counts["c_matrix"] != expected_compiled
                    or compiled_counts["zig_matrix"] != expected_compiled
                ):
                    print(
                        "must-stay-quiet fixture did not execute both compiled targets: "
                        f"counts={compiled_counts}, findings={compiled_findings}",
                        file=sys.stderr,
                    )
                    return 1
                for expected in (
                    "compiled archive missing export",
                    "compiled archive unexpected export",
                ):
                    if not any(expected in item for item in compiled_findings):
                        print(f"must-fire fixture was accepted: {expected}", file=sys.stderr)
                        return 1
        finally:
            pass
    print("check_zig_abi_policy.py --selftest: OK (quiet + all named failure classes).")
    return 0


def main() -> int:
    """Run self-test, print normalized digests, or enforce the committed policy."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--print-digests", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return _selftest()
    try:
        policy = _load_policy()
    except PolicyError as exc:
        print(f"check_zig_abi_policy.py: {exc}", file=sys.stderr)
        return 2
    if args.print_digests:
        for library in policy.get("libraries", []):
            header = (ROOT / library["public_header"]).read_text(encoding="utf-8")
            print(f"{library['name']} {_normalized_header_digest(header)}")
        return 0
    if not args.check:
        parser.error("choose --selftest, --check, or --print-digests")
    findings, counts = _validate(policy, compile_archives=True)
    if findings:
        print("check_zig_abi_policy.py: finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(
        "check_zig_abi_policy.py: clean "
        "(" + ", ".join(f"{key}={value}" for key, value in counts.items()) + ")."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

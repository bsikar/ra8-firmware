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


def _adapter_scope_findings(name: str, build_root: Path, adapter: Path) -> list[str]:
    """Reject exports declared outside the one registered adapter."""
    findings: list[str] = []
    for source in build_root.rglob("*.zig"):
        generated = any(part in {".zig-cache", "zig-out"} for part in source.parts)
        if source.resolve() == adapter or generated:
            continue
        other_names, _ = _zig_exports(source.read_text(encoding="utf-8"))
        if other_names:
            relative = source.relative_to(ROOT)
            findings.append(
                f"{name}: export outside adapter {relative}: " + ", ".join(sorted(other_names))
            )
    return findings


def _repository_inventory_findings(libraries: list[dict[str, Any]]) -> list[str]:
    """Require every hand-written Zig export adapter in the repository inventory."""
    registered = {
        (ROOT / value).resolve()
        for library in libraries
        if isinstance(library, dict) and isinstance((value := library.get("adapter")), str)
    }
    discovered: set[Path] = set()
    findings: list[str] = []
    for source in ROOT.rglob("*.zig"):
        if any(part in {".zig-cache", "zig-out", "third_party"} for part in source.parts):
            continue
        text = source.read_text(encoding="utf-8")
        names, _ = _zig_exports(text)
        if names or re.search(r"\b@export\s*\(", _strip_comments(text)):
            discovered.add(source.resolve())
        if re.search(r"\b@export\s*\(", _strip_comments(text)):
            findings.append(
                f"dynamic @export is prohibited at ABI boundaries: {source.relative_to(ROOT)}"
            )
    findings.extend(
        f"unregistered Zig export adapter: {source.relative_to(ROOT)}"
        for source in sorted(discovered - registered)
    )
    findings.extend(
        f"registered adapter has no Zig export: {source.relative_to(ROOT)}"
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
    library: dict[str, Any], name: str, prefix: str
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
        path = ROOT / path_value
        symbol_path = ROOT / symbol_value if isinstance(symbol_value, str) else path
        registration = ROOT / registration_value if isinstance(registration_value, str) else None
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
    for target, arguments in targets.items():
        if target not in required_targets:
            findings.append(f"{name}: unexpected target classification: {target}")
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            findings.append(f"{name}: malformed build arguments for target: {target}")
        if target == "ra8" and "-Dcpu=cortex_m85" not in arguments:
            findings.append(f"{name}: RA8 target evidence does not select cortex_m85")
    return findings


def _library_findings(library: dict[str, Any], required_targets: set[str]) -> list[str]:
    """Return source, metadata, test, and compatibility findings for one library."""
    findings: list[str] = []
    name = library.get("name", "<unnamed>")
    prefix = library.get("symbol_prefix")
    if not isinstance(prefix, str) or not prefix:
        return [f"{name}: missing symbol_prefix"]
    paths: dict[str, Path] = {}
    for field in ("build_root", "public_header", "adapter"):
        value = library.get(field)
        path = ROOT / value if isinstance(value, str) else ROOT / "<missing>"
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
        _adapter_scope_findings(name, paths["build_root"].resolve(), paths["adapter"].resolve())
    )
    findings.extend(_compatibility_findings(library, name, header_text, adapter_text))
    findings.extend(_contract_test_findings(library, name, prefix))
    findings.extend(_target_findings(library, name, required_targets))
    return findings


def _compiled_findings(library: dict[str, Any], zig: str, nm: str) -> list[str]:
    """Build every registered target and compare all global exports exactly."""
    name = library["name"]
    findings: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-zig-abi-") as tmp:
        for target, arguments in library["targets"].items():
            output = Path(tmp) / target
            command = [
                zig,
                "build",
                "--build-file",
                str(ROOT / library["build_root"] / "build.zig"),
                "--prefix",
                str(output / "install"),
                "--cache-dir",
                str(output / "cache"),
                "--global-cache-dir",
                str(output / "global-cache"),
                *arguments,
            ]
            proc = subprocess.run(  # noqa: S603 -- tool path resolved; policy is reviewed input
                command, cwd=ROOT, capture_output=True, text=True, check=False
            )
            if proc.returncode != 0:
                detail = (proc.stdout + proc.stderr).strip()
                findings.append(f"{name}: {target} archive build failed: {detail}")
                continue
            archive = output / "install" / "lib" / f"lib{library['library_name']}.a"
            if not archive.is_file():
                findings.append(f"{name}: {target} archive missing: {archive}")
                continue
            proc = subprocess.run(  # noqa: S603 -- tool path resolved; archive is freshly built
                [nm, "-g", "--defined-only", str(archive)],
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                findings.append(
                    f"{name}: {target} archive symbol scan failed: {proc.stderr.strip()}"
                )
                continue
            actual = {
                parts[-1].removeprefix("_")
                for line in proc.stdout.splitlines()
                if len(parts := line.split()) >= MIN_NM_SYMBOL_FIELDS
            }
            expected = {row["name"] for row in library["exports"]}
            findings.extend(_compiled_symbol_findings(f"{name}: {target}", expected, actual))
    return findings


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


def _validate(
    policy: dict[str, Any], *, compile_archives: bool
) -> tuple[list[str], dict[str, int]]:
    """Validate the complete policy and return findings plus non-vacuity counts."""
    findings: list[str] = []
    required = policy.get("required_targets")
    required_targets = set(required) if isinstance(required, list) else set()
    if required_targets != {"host", "ra8"}:
        findings.append("policy required_targets must contain exactly host and ra8")
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
    }
    typed_libraries = [library for library in libraries if isinstance(library, dict)]
    findings.extend(_repository_inventory_findings(typed_libraries))
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
        findings.extend(_library_findings(library, required_targets))
        counts["headers"] += int(isinstance(library.get("public_header"), str))
        counts["adapters"] += int(isinstance(library.get("adapter"), str))
        counts["exports"] += len(library.get("exports", []))
        counts["tests"] += len(library.get("contract_tests", []))
        if compile_archives and zig is not None and nm is not None:
            findings.extend(_compiled_findings(library, zig, nm))
    floors = {"libraries": 1, "headers": 1, "adapters": 1, "exports": 1, "tests": 3, "targets": 2}
    for item, floor in floors.items():
        if counts[item] < floor:
            findings.append(f"non-vacuity failure: {item}={counts[item]}, floor={floor}")
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
        original_root = globals()["ROOT"]
        globals()["ROOT"] = root
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
                "targets": {
                    "host": [],
                    "ra8": ["-Dtarget=thumb-freestanding-eabihf", "-Dcpu=cortex_m85"],
                },
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
            if findings := _library_findings(base, {"host", "ra8"}):
                print(f"must-stay-quiet fixture failed: {findings}", file=sys.stderr)
                return 1
            if not any(
                "unregistered Zig export adapter" in item
                for item in _repository_inventory_findings([])
            ):
                print("must-fire fixture was accepted: omitted adapter", file=sys.stderr)
                return 1
            host_only = json.loads(json.dumps(base))
            host_only["targets"].pop("ra8")
            target_findings, _ = _validate(
                {"required_targets": ["host", "ra8"], "libraries": [host_only]},
                compile_archives=False,
            )
            if not any(
                "missing required target classification: ra8" in item for item in target_findings
            ):
                print("must-fire fixture was accepted: inventory without RA8", file=sys.stderr)
                return 1
            mutations = {
                "prohibited boundary type": adapter.replace("?*u32", "[]u32"),
                "missing export": adapter.replace("pub export fn", "pub fn"),
                "unexpected export": adapter + "export fn rogue_helper() callconv(.c) void {}\n",
            }
            for label, changed in mutations.items():
                (root / "build/adapter.zig").write_text(changed, encoding="utf-8")
                if not any(
                    label.split()[0] in item for item in _library_findings(base, {"host", "ra8"})
                ):
                    print(f"must-fire fixture was accepted: {label}", file=sys.stderr)
                    return 1
            (root / "build/adapter.zig").write_text(adapter, encoding="utf-8")
            c_test = root / "tests/test.c"
            valid_c_test = c_test.read_text(encoding="utf-8")
            c_test.write_text("/* demo_run main( */\n", encoding="utf-8")
            if not any(
                "not executable ABI evidence" in item
                for item in _library_findings(base, {"host", "ra8"})
            ):
                print("must-fire fixture was accepted: dead contract test", file=sys.stderr)
                return 1
            c_test.write_text(valid_c_test, encoding="utf-8")
            broken_target = json.loads(json.dumps(base))
            broken_target["targets"]["ra8"] = ["-Dtarget=thumb-freestanding-eabihf"]
            if not any(
                "does not select cortex_m85" in item
                for item in _library_findings(broken_target, {"host", "ra8"})
            ):
                print("must-fire fixture was accepted: mislabeled RA8 target", file=sys.stderr)
                return 1
            for field, expected in (
                ("calling_context", "calling context"),
                ("ownership", "ownership"),
            ):
                broken = json.loads(json.dumps(base))
                broken["exports"][0][field] = ""
                if not any(expected in item for item in _library_findings(broken, {"host", "ra8"})):
                    print(f"must-fire fixture was accepted: undocumented {field}", file=sys.stderr)
                    return 1
            broken = json.loads(json.dumps(base))
            broken["compatibility_sha256"] = "0" * 64
            if not any(
                "compatibility drift" in item for item in _library_findings(broken, {"host", "ra8"})
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
                if not any(expected in item for item in _library_findings(broken, {"host", "ra8"})):
                    print(f"must-fire fixture was accepted: {expected}", file=sys.stderr)
                    return 1
            zig = shutil.which("zig")
            nm = shutil.which("llvm-nm") or shutil.which("nm")
            if zig is not None and nm is not None:
                compiled = json.loads(json.dumps(base))
                compiled["exports"][0]["name"] = "demo_missing"
                compiled_findings = _compiled_findings(compiled, zig, nm)
                for expected in (
                    "compiled archive missing export",
                    "compiled archive unexpected export",
                ):
                    if not any(expected in item for item in compiled_findings):
                        print(f"must-fire fixture was accepted: {expected}", file=sys.stderr)
                        return 1
        finally:
            globals()["ROOT"] = original_root
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

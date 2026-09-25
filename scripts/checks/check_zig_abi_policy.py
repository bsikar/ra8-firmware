#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the declared Zig-to-C ABI inventory against source and archives."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
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
# scripts/ci/lib/lang_toolchains.sh unpacks the pinned Zig release into
# build/tools/, so the upstream standard library lands inside the worktree.
PROVISIONED_TOOLCHAIN_PREFIX = ("build", "tools")
PROHIBITED_ZIG_TYPES = (
    (re.compile(r"\[\](?:const\s+)?"), "slice"),
    (re.compile(r"(?<![=!])!(?!=)"), "error union"),
    (re.compile(r"\bbool\b"), "bool"),
    (re.compile(r"\banytype\b"), "anytype"),
    (re.compile(r"\bcomptime\b"), "comptime"),
)


class PolicyError(Exception):
    """Raised when the policy file itself is unreadable or malformed."""


@dataclass(slots=True)
class LibraryContext:
    """Carry paths and identity shared by a library's policy checks."""

    library: dict[str, Any]
    name: str
    prefix: str
    repository_root: Path
    build_root: Path


@dataclass(slots=True)
class ExportInventory:
    """Keep compared C and Zig symbol sets together for policy checks."""

    declared: set[str]
    header_names: set[str]
    zig_names: set[str]
    zig_declared: set[str]


def _strip_comments(text: str) -> str:
    """Remove C/Zig comments before inventory extraction."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n\r]*", "", text)


def _strip_conditional_blocks(text: str) -> str:
    """Remove conditional-preprocessor regions from assertion evidence."""
    kept: list[str] = []
    conditional_depth = 0
    for line in text.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", line)
        if directive:
            kind = directive.group(1)
            if kind in {"if", "ifdef", "ifndef"}:
                conditional_depth += 1
                continue
            if conditional_depth and kind == "endif":
                conditional_depth -= 1
                continue
        if not conditional_depth:
            kept.append(line)
    return "".join(kept)


def _lexical_tokens(text: str) -> list[str]:
    """Return assertion-relevant C/Zig tokens while ignoring layout."""
    clean = re.sub(r"(?m)\\\\[^\r\n]*", "", text)
    # Preserve only the field-name string argument of a real Zig @offsetOf
    # expression. Removing all strings made correct field-offset assertions
    # impossible to match, while retaining arbitrary strings admits fake proof.
    clean = re.sub(
        r'(@offsetOf\s*\(\s*[^,()]+,\s*)"((?:\\.|[^"\\])*)"(\s*\))',
        lambda match: match.group(1) + "FIELDNAME_" + match.group(2) + match.group(3),
        clean,
    )
    clean = re.sub(r'"(?:\\.|[^"\\\r\n])*"|\'(?:\\.|[^\'\\\r\n])*\'', "", clean)
    clean = _strip_conditional_blocks(_strip_comments(clean))
    return re.findall(
        r"@[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*|\d+[A-Za-z]*|==|!=|\S", clean
    )


def _contains_token_sequence(text: str, fragment: str) -> bool:
    """Match one policy fragment as a contiguous lexical token sequence."""
    tokens = _lexical_tokens(text)
    expected = _lexical_tokens(fragment)
    width = len(expected)
    return bool(expected) and any(
        tokens[index : index + width] == expected for index in range(len(tokens))
    )


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
    context: LibraryContext, inventory: ExportInventory, zig_heads: dict[str, str]
) -> list[str]:
    """Compare full header metadata and Zig-owned adapter declarations."""
    findings: list[str] = []
    for label, expected, actual in (
        ("header", inventory.declared, inventory.header_names),
        ("Zig adapter", inventory.zig_declared, inventory.zig_names),
    ):
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        if missing:
            findings.append(f"{context.name}: {label} missing export(s): {', '.join(missing)}")
        if unexpected:
            findings.append(
                f"{context.name}: {label} unexpected export(s): {', '.join(unexpected)}"
            )
    for symbol in sorted(inventory.zig_declared & inventory.zig_names):
        head = zig_heads[symbol]
        findings.extend(
            f"{context.name}: prohibited Zig-only type {type_name}: {symbol}"
            for pattern, type_name in PROHIBITED_ZIG_TYPES
            if pattern.search(head)
        )
    return findings


def _retained_c_export_findings(
    context: LibraryContext, declared: set[str], header_names: set[str], zig_names: set[str]
) -> tuple[list[str], set[str]]:
    """Validate exact retained-C symbol/source ownership for split ports."""
    rows = context.library.get("retained_c_exports", [])
    if not isinstance(rows, list):
        return [f"{context.name}: retained_c_exports must be a list"], set()
    retained: set[str] = set()
    findings: list[str] = []
    for row in rows:
        if (
            not isinstance(row, dict)
            or set(row) != {"name", "source"}
            or not isinstance(row.get("name"), str)
            or not isinstance(row.get("source"), str)
        ):
            findings.append(f"{context.name}: malformed retained C export metadata")
            continue
        symbol = row["name"]
        source_value = row["source"]
        if symbol in retained:
            findings.append(f"{context.name}: duplicate retained C export: {symbol}")
            continue
        retained.add(symbol)
        if symbol not in declared or symbol not in header_names:
            findings.append(
                f"{context.name}: retained C export is not declared by the public header: {symbol}"
            )
        if symbol in zig_names:
            findings.append(
                f"{context.name}: retained C export is also defined by a Zig adapter: {symbol}"
            )
        source = context.repository_root / source_value
        try:
            source.resolve().relative_to(context.build_root.resolve())
        except ValueError:
            findings.append(
                f"{context.name}: retained C source is outside build root: {source_value}"
            )
            continue
        if not source.is_file():
            findings.append(f"{context.name}: missing retained C source: {source_value}")
            continue
        source_text = _strip_comments(source.read_text(encoding="utf-8"))
        definition = re.search(rf"\b{re.escape(symbol)}\s*\([^;{{}}]*\)\s*\{{", source_text)
        if definition is None:
            findings.append(
                f"{context.name}: retained C source does not define {symbol}: {source_value}"
            )
    return findings, retained


def _adapter_scope_findings(
    name: str,
    build_root: Path,
    adapter: Path,
    repository_root: Path = ROOT,
    additional_adapters: set[Path] | None = None,
) -> list[str]:
    """Reject exports outside the adapter while allowing test-local link stubs."""
    findings: list[str] = []
    for source in build_root.rglob("*.zig"):
        generated = any(part in GENERATED_PATH_PARTS for part in source.parts)
        relative_parts = source.relative_to(build_root).parts
        if (
            source.resolve() == adapter
            or source.resolve() in (additional_adapters or set())
            or generated
            or "tests" in relative_parts
        ):
            continue
        other_names, _ = _zig_exports(source.read_text(encoding="utf-8"))
        if other_names:
            relative = source.relative_to(repository_root)
            findings.append(
                f"{name}: export outside adapter {relative}: " + ", ".join(sorted(other_names))
            )
    return findings


def _registered_adapter_paths(
    libraries: list[dict[str, Any]], repository_root: Path
) -> set[Path]:
    """Collect production adapters, excluding library-local test helpers."""
    registered: set[Path] = set()
    for library in libraries:
        if not isinstance(library, dict):
            continue
        adapter = library.get("adapter")
        if isinstance(adapter, str):
            registered.add((repository_root / adapter).resolve())
        additional = library.get("additional_adapters", [])
        if not isinstance(additional, list):
            continue
        for row in additional:
            if not isinstance(row, dict) or not isinstance(row.get("path"), str):
                continue
            parts = Path(row["path"]).parts
            library_test = (
                row.get("role") == "test-only" and parts and parts[0] == "libs"
                and "tests" in parts[1:]
            )
            if not library_test:
                registered.add((repository_root / row["path"]).resolve())
    return registered


def _peer_adapter_paths(library: dict[str, Any]) -> set[str | None]:
    """Return the public adapter paths owned by one library."""
    paths = {library.get("adapter")}
    additional = library.get("additional_adapters", [])
    if isinstance(additional, list):
        paths.update(
            item.get("path")
            for item in additional
            if isinstance(item, dict) and item.get("role") == "public"
        )
    return paths


def _peer_adapter_findings(
    libraries: list[dict[str, Any]], repository_root: Path
) -> list[str]:
    """Validate peer-public adapter ownership and build-root containment."""
    library_by_name = {
        library["name"]: library
        for library in libraries
        if isinstance(library, dict) and isinstance(library.get("name"), str)
    }
    findings: list[str] = []
    for library in libraries:
        if not isinstance(library, dict):
            continue
        rows = library.get("additional_adapters", [])
        if not isinstance(rows, list):
            continue
        for row in rows:
            if isinstance(row, dict) and row.get("role") == "peer-public":
                findings.extend(
                    _one_peer_adapter_findings(library, row, library_by_name, repository_root)
                )
    return findings


def _one_peer_adapter_findings(
    owner: dict[str, Any],
    row: dict[str, Any],
    libraries: dict[str, dict[str, Any]],
    repository_root: Path,
) -> list[str]:
    """Check one peer adapter against its declared owner."""
    owner_name = owner.get("name")
    peer_name = row.get("owner_library")
    peer = libraries.get(peer_name)
    path_value = row.get("path")
    if not isinstance(path_value, str) or peer is None:
        return [f"{owner_name}: peer adapter has unknown owner: {peer_name}"]
    if peer_name == owner_name:
        return [f"{owner_name}: peer adapter owner must be a different library"]
    if path_value not in _peer_adapter_paths(peer):
        return [
            f"{owner_name}: peer adapter {path_value} is not registered by owner {peer_name}"
        ]
    if row.get("symbol_prefix") != peer.get("symbol_prefix"):
        return [f"{owner_name}: peer adapter prefix does not match owner {peer_name}"]
    try:
        (repository_root / path_value).resolve().relative_to(
            (repository_root / peer["build_root"]).resolve()
        )
    except (KeyError, TypeError, ValueError):
        return [
            f"{owner_name}: peer adapter is outside owner {peer_name} "
            f"build root: {path_value}"
        ]
    return []


def _discovered_adapter_paths(repository_root: Path) -> tuple[set[Path], list[str]]:
    """Find Zig export sources and dynamic @export declarations repo-wide."""
    discovered: set[Path] = set()
    findings: list[str] = []
    for source in repository_root.rglob("*.zig"):
        if any(part in REPOSITORY_EXCLUDED_PATH_PARTS for part in source.parts):
            continue
        relative_parts = source.relative_to(repository_root).parts
        if relative_parts[0] == "libs" and "tests" in relative_parts[1:]:
            continue
        if relative_parts[: len(PROVISIONED_TOOLCHAIN_PREFIX)] == PROVISIONED_TOOLCHAIN_PREFIX:
            continue
        text = source.read_text(encoding="utf-8")
        names, _ = _zig_exports(text)
        clean_text = _strip_comments(text)
        has_dynamic_export = re.search(r"\b@export\s*\(", clean_text) is not None
        if names or has_dynamic_export:
            discovered.add(source.resolve())
        if has_dynamic_export:
            findings.append(
                "dynamic @export is prohibited at ABI boundaries: "
                f"{source.relative_to(repository_root)}"
            )
    return discovered, findings


def _repository_inventory_findings(
    libraries: list[dict[str, Any]], repository_root: Path = ROOT
) -> list[str]:
    """Require every hand-written Zig export adapter in the repository inventory."""
    registered = _registered_adapter_paths(libraries, repository_root)
    discovered, findings = _discovered_adapter_paths(repository_root)
    findings.extend(_peer_adapter_findings(libraries, repository_root))
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
        or not (
            _contains_token_sequence(header_text, fragment)
            or _contains_token_sequence(adapter_text, fragment)
        )
    )
    return findings


def _json_registration_findings(
    name: str, language: str, path: Path, symbol_path: Path, registration: Path
) -> list[str]:
    """Verify JSON test-contract reachability for source and symbol evidence."""
    try:
        contract = json.loads(_strip_comments(registration.read_text(encoding="utf-8")))
    except json.JSONDecodeError:
        return [f"{name}: malformed {language} test registration"]
    registered = set(contract.get("test_roots", [])) | set(contract.get("covered_sources", []))
    findings: list[str] = []
    if path.relative_to(registration.parent).as_posix() not in registered:
        findings.append(f"{name}: {language} contract test is not registered: {path}")
    if (
        symbol_path != path
        and symbol_path.relative_to(registration.parent).as_posix() not in registered
    ):
        findings.append(f"{name}: {language} symbol evidence is not registered: {symbol_path}")
    return findings


def _contract_test_findings(
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
        if registration.suffix == ".json":
            findings.extend(
                _json_registration_findings(name, language, path, symbol_path, registration)
            )
        else:
            registration_text = registration.read_text(encoding="utf-8")
            if path.name not in _strip_comments(
                registration_text
            ) and not _cmake_glob_registers_test(path, registration, registration_text):
                findings.append(f"{name}: C contract test is not registered: {path_value}")
    if "c" not in seen or "zig" not in seen:
        findings.append(f"{name}: contract tests must include C and Zig")
    return findings


def _cmake_glob_registers_test(path: Path, registration: Path, text: str) -> bool:
    """Recognize tests registered through the canonical tests source glob."""
    tests_root = registration.parent.parent
    if registration.parent.name != "cmake" or tests_root.name != "tests":
        return False
    try:
        relative = path.relative_to(tests_root).as_posix()
    except ValueError:
        return False
    # The CMake glob contains /*/, which the C/C++ comment stripper mistakes
    # for a block comment. Strip CMake line comments only for this inspection.
    clean = re.sub(r"(?m)#.*$", "", text)
    compact = re.sub(r"\s+", " ", clean)
    glob = re.search(
        r"file\s*\(\s*GLOB\s+RA8_TEST_SOURCES\s+CONFIGURE_DEPENDS\s+"
        r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/\*/src/test_\*\.c\s*\)",
        clean,
        re.IGNORECASE,
    )
    if glob is None or not fnmatch.fnmatchcase(relative, "*/src/test_*.c"):
        return False
    if "foreach(src ${RA8_TEST_SOURCES})" not in compact:
        return False
    if "ra8_add_test(${name} ${src})" not in compact:
        return False
    removals = re.findall(r"list\s*\(\s*REMOVE_ITEM\s+RA8_TEST_SOURCES(.*?)\)", clean, re.DOTALL)
    candidates = {
        f"${{CMAKE_CURRENT_SOURCE_DIR}}/{relative}",
        f"${{FW_ROOT}}/tests/{relative}",
    }
    return not any(any(candidate in removal for candidate in candidates) for removal in removals)


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


def _additional_header_findings(
    context: LibraryContext, rows: list[Any], header_names: set[str]
) -> list[str]:
    """Validate extra headers and add their public symbols to the inventory."""
    findings: list[str] = []
    for row in rows:
        if not isinstance(row, dict):
            findings.append(f"{context.name}: malformed additional header metadata")
            continue
        path_value = row.get("path")
        header_prefix = row.get("symbol_prefix")
        digest = row.get("compatibility_sha256")
        role = row.get("role")
        if (
            set(row) != {"path", "role", "symbol_prefix", "compatibility_sha256"}
            or not isinstance(path_value, str)
            or role not in {"test-only", "public"}
            or not isinstance(header_prefix, str)
            or not header_prefix
            or not isinstance(digest, str)
        ):
            findings.append(f"{context.name}: malformed additional header metadata")
            continue
        if role == "public" and header_prefix != context.prefix:
            findings.append(
                f"{context.name}: public additional header must use the library symbol prefix"
            )
            continue
        additional_path = context.repository_root / path_value
        try:
            additional_path.resolve().relative_to(context.build_root)
        except ValueError:
            findings.append(
                f"{context.name}: additional header is outside build root: {path_value}"
            )
            continue
        if not additional_path.is_file():
            findings.append(f"{context.name}: missing additional header: {path_value}")
            continue
        additional_text = additional_path.read_text(encoding="utf-8")
        if digest != _normalized_header_digest(additional_text):
            findings.append(
                f"{context.name}: additional header compatibility drift: {path_value}"
            )
        header_names.update(_header_exports(additional_text, header_prefix))
    return findings


def _additional_adapter_result(
    context: LibraryContext, row: object
) -> tuple[list[str], Path | None, set[str], dict[str, str]]:
    """Validate one extra adapter and return its path and public symbols."""
    if (
        not isinstance(row, dict)
        or row.get("role") not in {"test-only", "public", "peer-public"}
        or not isinstance(row.get("path"), str)
        or not isinstance(row.get("symbol_prefix"), str)
        or not row["symbol_prefix"]
    ):
        return [f"{context.name}: malformed additional adapter metadata"], None, set(), {}
    if row["role"] == "public" and row["symbol_prefix"] != context.prefix:
        return [
            f"{context.name}: public additional adapter must use the library symbol prefix"
        ], None, set(), {}
    if row["role"] == "peer-public" and (
        not isinstance(row.get("owner_library"), str)
        or not row["owner_library"]
        or row["owner_library"] == context.name
    ):
        return [
            f"{context.name}: peer-public adapter requires a distinct owner_library"
        ], None, set(), {}
    path = context.repository_root / row["path"]
    path_error = None
    try:
        path.resolve().relative_to(context.build_root)
    except ValueError:
        path_error = f"{context.name}: test-only adapter is outside build root: {row['path']}"
    if path_error is None and not path.is_file():
        path_error = f"{context.name}: missing additional adapter: {row['path']}"
    if path_error is not None:
        return [path_error], None, set(), {}
    names, heads = _zig_exports(path.read_text(encoding="utf-8"))
    if not names or any(not symbol.startswith(row["symbol_prefix"]) for symbol in names):
        return [
            f"{context.name}: additional adapter exports do not match its prefix: {row['path']}"
        ], None, set(), {}
    public_names = names if row["role"] == "public" else set()
    public_heads = heads if row["role"] == "public" else {}
    return [], path.resolve(), public_names, public_heads


def _additional_adapter_findings(
    context: LibraryContext, rows: list[Any], zig_names: set[str], zig_heads: dict[str, str]
) -> tuple[list[str], set[Path]]:
    """Validate all extra adapters and collect allowed paths/public exports."""
    findings: list[str] = []
    additional_paths: set[Path] = set()
    for row in rows:
        row_findings, path, names, heads = _additional_adapter_result(context, row)
        findings.extend(row_findings)
        if path is not None:
            additional_paths.add(path)
        zig_names.update(names)
        zig_heads.update(heads)
    return findings, additional_paths


def _library_findings(
    library: dict[str, Any], required_targets: set[str], repository_root: Path = ROOT
) -> list[str]:
    """Return source, metadata, test, and compatibility findings for one library."""
    name = library.get("name", "<unnamed>")
    prefix = library.get("symbol_prefix")
    if not isinstance(prefix, str) or not prefix:
        return [f"{name}: missing symbol_prefix"]
    paths: dict[str, Path] = {}
    findings: list[str] = []
    for field in ("build_root", "public_header", "adapter"):
        value = library.get(field)
        path = repository_root / value if isinstance(value, str) else repository_root / "<missing>"
        paths[field] = path
        if not path.exists():
            findings.append(f"{name}: missing {field}: {value}")
    if findings:
        return findings

    context = LibraryContext(library, name, prefix, repository_root, paths["build_root"].resolve())
    header_text = paths["public_header"].read_text(encoding="utf-8")
    header_names = _header_exports(header_text, prefix)
    additional_headers = library.get("additional_headers", [])
    if not isinstance(additional_headers, list):
        return [f"{name}: additional_headers must be a list"]
    findings.extend(_additional_header_findings(context, additional_headers, header_names))

    adapter_text = paths["adapter"].read_text(encoding="utf-8")
    zig_names, zig_heads = _zig_exports(adapter_text)
    additional_adapters = library.get("additional_adapters", [])
    if not isinstance(additional_adapters, list):
        findings.append(f"{name}: additional_adapters must be a list")
        additional_adapters = []
    adapter_findings, additional_paths = _additional_adapter_findings(
        context, additional_adapters, zig_names, zig_heads
    )
    findings.extend(adapter_findings)

    metadata_findings, declared = _metadata_findings(name, library.get("exports"))
    findings.extend(metadata_findings)
    retained_findings, retained_c = _retained_c_export_findings(
        context, declared, header_names, zig_names
    )
    findings.extend(retained_findings)
    inventory = ExportInventory(declared, header_names, zig_names, declared - retained_c)
    findings.extend(_inventory_findings(context, inventory, zig_heads))
    findings.extend(
        _adapter_scope_findings(
            name, context.build_root, paths["adapter"].resolve(), repository_root, additional_paths
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
    additional_headers = library.get("additional_headers", [])
    if not isinstance(additional_headers, list) or any(
        not isinstance(row, dict) or not isinstance(row.get("path"), str)
        for row in additional_headers
    ):
        return [f"{library['name']}: malformed additional header metadata"]
    probe = output / "abi_header_probe.c"
    probe.parent.mkdir(parents=True, exist_ok=True)
    headers = [library["public_header"]]
    headers.extend(row["path"] for row in additional_headers)
    probe.write_text(
        "".join(f'#include "{Path(header).name}"\n' for header in headers),
        encoding="utf-8",
    )
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


def _archive_symbol_names(output: str, *, bundle_compiler_rt: bool = False) -> set[str]:
    """Parse nm output, ignoring only explicitly bundled compiler runtime members."""
    actual: set[str] = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < MIN_NM_SYMBOL_FIELDS:
            continue
        member_match = re.search(r"[\[(]([^\)\]]+)[\)\]]", parts[0])
        member = member_match.group(1) if member_match else ""
        if not member and parts[0].count(":") > 1:
            archive_and_member, _address = parts[0].rsplit(":", 1)
            _archive, member = archive_and_member.split(":", 1)
        if bundle_compiler_rt and Path(member).name == "compiler_rt.o":
            continue
        actual.add(parts[-1].removeprefix("_"))
    return actual


def _bundle_compiler_rt_enabled(build_root: Path) -> bool:
    """Return whether a library build explicitly bundles Zig's runtime archive."""
    try:
        build_source = (build_root / "build.zig").read_text(encoding="utf-8")
    except OSError:
        return False
    build_source = _strip_comments(build_source)
    return (
        re.search(
            r"(?m)^[ \t]*library\.bundle_compiler_rt[ \t]*=[ \t]*true[ \t]*;[ \t]*$",
            build_source,
        )
        is not None
    )


def _archive_symbols(
    nm: str, archive: Path, name: str, *, bundle_compiler_rt: bool = False
) -> tuple[list[str], set[str]]:
    """Read public global definitions, excluding explicitly bundled runtime members."""
    if not archive.is_file():
        return [f"{name} archive missing: {archive}"], set()
    proc = subprocess.run(  # noqa: S603 -- resolved tool; fresh archive
        [nm, "-A", "-g", "--defined-only", str(archive)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return [f"{name} symbol scan failed: {proc.stderr.strip()}"], set()
    return [], _archive_symbol_names(proc.stdout, bundle_compiler_rt=bundle_compiler_rt)
def _host_mode_test_findings(
    library: dict[str, Any], command: list[str], mode: str, repository_root: Path
) -> tuple[list[str], int]:
    """Run the canonical host ABI tests in one optimization mode when required."""
    if library.get("run_host_tests_in_all_modes") is not True:
        return [], 0
    proc = subprocess.run(  # noqa: S603 -- pinned Zig; reviewed policy
        [*command, "test"], cwd=repository_root, capture_output=True, text=True, check=False
    )
    if proc.returncode == 0:
        return [], 1
    detail = (proc.stdout + proc.stderr).strip()
    return [f"{library['name']}: host/{mode} ABI tests failed: {detail}"], 0


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
            bundle_compiler_rt = _bundle_compiler_rt_enabled(
                repository_root / library["build_root"]
            )
            symbol_findings, actual = _archive_symbols(
                nm,
                archive,
                f"{name}: {target}/{mode}",
                bundle_compiler_rt=bundle_compiler_rt,
            )
            findings.extend(symbol_findings)
            if symbol_findings:
                continue
            retained_c = {
                row["name"]
                for row in library.get("retained_c_exports", [])
                if isinstance(row, dict) and isinstance(row.get("name"), str)
            }
            expected = {row["name"] for row in library["exports"]} - retained_c
            findings.extend(_compiled_symbol_findings(f"{name}: {target}/{mode}", expected, actual))
            counts["zig_matrix"] += 1
            if target == "host":
                test_findings, passed = _host_mode_test_findings(
                    library, command, mode, repository_root
                )
                findings.extend(test_findings)
                counts["mode_tests"] += passed
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


def _policy_counts(libraries: list[Any], required_modes: set[str]) -> dict[str, int]:
    """Initialize the policy's non-vacuity counters."""
    return {
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


def _floor_findings(counts: dict[str, int]) -> list[str]:
    """Report policy inventory counts below their non-vacuity floors."""
    floors = {
        "libraries": 1,
        "headers": 1,
        "adapters": 1,
        "exports": 1,
        "tests": 3,
        "targets": 2,
        "modes": 3,
    }
    return [
        f"non-vacuity failure: {item}={counts[item]}, floor={floor}"
        for item, floor in floors.items()
        if counts[item] < floor
    ]


def _required_policy_sets(policy: dict[str, Any]) -> tuple[set[str], set[str], list[str]]:
    """Normalize required target/mode sets and report exact-set drift."""
    findings: list[str] = []
    required = policy.get("required_targets")
    targets = set(required) if isinstance(required, list) else set()
    if targets != {"host", "ra8"}:
        findings.append("policy required_targets must contain exactly host and ra8")
    modes_value = policy.get("required_modes")
    modes = set(modes_value) if isinstance(modes_value, list) else set()
    if modes != REQUIRED_MODES:
        missing = ", ".join(sorted(REQUIRED_MODES - modes)) or "none"
        unexpected = ", ".join(sorted(modes - REQUIRED_MODES)) or "none"
        findings.append(
            "policy optimization modes must be Debug, ReleaseSafe, ReleaseSmall "
            f"(missing: {missing}; unexpected: {unexpected})"
        )
    return targets, modes, findings


def _validate(
    policy: dict[str, Any], *, compile_archives: bool, repository_root: Path = ROOT
) -> tuple[list[str], dict[str, int]]:
    """Validate the complete policy and return findings plus non-vacuity counts."""
    required_targets, required_modes, findings = _required_policy_sets(policy)
    libraries = policy.get("libraries")
    if not isinstance(libraries, list) or not libraries:
        return [*findings, "policy must inventory at least one ABI library"], {}
    zig = shutil.which("zig") if compile_archives else None
    nm = (shutil.which("llvm-nm") or shutil.which("nm")) if compile_archives else None
    if compile_archives and (zig is None or nm is None):
        findings.append("compiled policy check requires zig and llvm-nm or nm")
    counts = _policy_counts(libraries, required_modes)
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
    findings.extend(_floor_findings(counts))
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
        for item in _repository_inventory_findings([], root)
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
    default_c_abi = adapter.replace(" callconv(.c)", "")
    (root / "build/adapter.zig").write_text(default_c_abi, encoding="utf-8")
    if findings := _library_findings(base, {"host", "ra8"}, root):
        return f"must-stay-quiet default export C ABI fixture failed: {findings}"
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


def _selftest_test_only_adapter_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise test-only adapter validation and production-inventory exclusion."""
    path = root / "build/test_adapter.zig"
    path.write_text("pub export fn demo_test_helper() callconv(.c) void {}\n", encoding="utf-8")
    row = json.loads(json.dumps(base))
    row["additional_adapters"] = [
        {"path": "build/test_adapter.zig", "role": "test-only", "symbol_prefix": "demo_test_"}
    ]
    if findings := _library_findings(row, {"host", "ra8"}, root):
        return f"must-stay-quiet test-only adapter fixture failed: {findings}"
    policy_findings = _repository_inventory_findings([row], root)
    if policy_findings:
        return f"must-stay-quiet registered test-only adapter was rejected: {policy_findings}"
    for mutation, expected in (
        ("role", "public"),
        ("symbol_prefix", "rogue_"),
    ):
        broken = json.loads(json.dumps(row))
        broken["additional_adapters"][0][mutation] = expected
        if not _library_findings(broken, {"host", "ra8"}, root):
            return f"must-fire fixture was accepted: malformed test-only adapter {mutation}"
    broken = json.loads(json.dumps(row))
    broken["additional_adapters"][0]["path"] = "../outside.zig"
    if not any(
        "outside build root" in finding
        for finding in _library_findings(broken, {"host", "ra8"}, root)
    ):
        return "must-fire fixture was accepted: test-only adapter outside build root"
    malformed = json.loads(json.dumps(row))
    malformed["additional_adapters"] = None
    if not any(
        "additional_adapters must be a list" in finding
        for finding in _library_findings(malformed, {"host", "ra8"}, root)
    ):
        return "must-fire fixture was accepted: malformed additional adapter collection"
    path.unlink()
    return None


def _selftest_library_test_adapter_policy(base: dict[str, Any], root: Path) -> str | None:
    """Allow a library-local test adapter without inventorying it as production."""
    # A test-only helper may live in the library's tests tree. It is checked
    # against its declared prefix and owner build root, but is not a production
    # adapter registered in the repository-wide production inventory.
    inventory_root = root / "test-only-inventory"
    library_root = inventory_root / "libs/demo"
    (library_root / "src").mkdir(parents=True)
    (library_root / "inc").mkdir(parents=True)
    (library_root / "tests").mkdir(parents=True)
    (library_root / "src/adapter.zig").write_text(
        (root / "build/adapter.zig").read_text(encoding="utf-8"), encoding="utf-8"
    )
    (library_root / "inc/demo.h").write_text(
        (root / "inc/demo.h").read_text(encoding="utf-8"), encoding="utf-8"
    )
    inventory_tests = inventory_root / "tests"
    inventory_tests.mkdir(parents=True)
    for language in ("c", "rust", "zig"):
        (inventory_tests / f"test.{language}").write_text(
            (root / f"tests/test.{language}").read_text(encoding="utf-8"), encoding="utf-8"
        )
    (inventory_tests / "contract.json").write_text(
        (root / "tests/contract.json").read_text(encoding="utf-8"), encoding="utf-8"
    )
    library_test_adapter = library_root / "tests/test_adapter.zig"
    library_test_adapter.write_text(
        "pub export fn demo_test_helper() callconv(.c) void {}\n", encoding="utf-8"
    )
    library_row = json.loads(json.dumps(base))
    library_row["build_root"] = "libs/demo"
    library_row["public_header"] = "libs/demo/inc/demo.h"
    library_row["adapter"] = "libs/demo/src/adapter.zig"
    library_row["additional_adapters"] = [
        {
            "path": "libs/demo/tests/test_adapter.zig",
            "role": "test-only",
            "symbol_prefix": "demo_test_",
        }
    ]
    if findings := _library_findings(library_row, {"host", "ra8"}, inventory_root):
        return f"must-stay-quiet library test-only adapter fixture failed: {findings}"
    if findings := _repository_inventory_findings([library_row], inventory_root):
        return f"must-stay-quiet library test-only adapter registered as production: {findings}"
    shutil.rmtree(inventory_root)
    return None


def _selftest_peer_adapter_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise explicit ownership for a cross-library public adapter."""
    path = root / "build/test_adapter.zig"
    path.write_text("pub export fn demo_test_helper() callconv(.c) void {}\n", encoding="utf-8")
    peer_path = root / "build/peer_adapter.zig"
    peer_path.write_text(
        "pub export fn peer_demo_run() callconv(.c) i32 { return 0; }\n",
        encoding="utf-8",
    )
    row = json.loads(json.dumps(base))
    owner = json.loads(json.dumps(base))
    owner["name"] = "peer_demo"
    owner["symbol_prefix"] = "peer_demo_"
    owner["adapter"] = "build/peer_adapter.zig"
    row["additional_adapters"] = [
        {"path": "build/test_adapter.zig", "role": "test-only", "symbol_prefix": "demo_test_"},
        {
            "path": "build/peer_adapter.zig",
            "role": "peer-public",
            "symbol_prefix": "peer_demo_",
            "owner_library": "peer_demo",
        },
    ]
    if findings := _library_findings(row, {"host", "ra8"}, root):
        return f"must-stay-quiet peer adapter fixture failed: {findings}"
    if findings := _repository_inventory_findings([row, owner], root):
        return f"must-stay-quiet explicitly owned peer adapter was rejected: {findings}"
    broken_peer = json.loads(json.dumps(row))
    broken_peer["additional_adapters"][1]["owner_library"] = "unknown"
    if not any(
        "unknown owner" in finding
        for finding in _repository_inventory_findings([broken_peer, owner], root)
    ):
        return "must-fire fixture was accepted: unknown peer adapter owner"
    broken_peer["additional_adapters"][1]["owner_library"] = "peer_demo"
    broken_peer["additional_adapters"][1]["path"] = "build/test_adapter.zig"
    peer_findings = _repository_inventory_findings([broken_peer, owner], root)
    if not any("not registered by owner" in finding for finding in peer_findings):
        return f"must-fire fixture was accepted: unowned peer adapter path: {peer_findings}"
    row["additional_adapters"] = []
    peer_path.unlink()
    path.unlink()
    return None


def _selftest_additional_adapter_policy(base: dict[str, Any], root: Path) -> str | None:
    """Run the independent test-only, library-test, and peer-adapter fixtures."""
    for check in (
        _selftest_test_only_adapter_policy,
        _selftest_library_test_adapter_policy,
        _selftest_peer_adapter_policy,
    ):
        if error := check(base, root):
            return error
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
    return None


def _selftest_retained_c_exports(base: dict[str, Any], root: Path) -> str | None:
    """Keep retained C exports in header metadata but out of Zig archive parity."""
    original_header = (root / "inc/demo.h").read_text(encoding="utf-8")
    header = original_header + "int demo_retained(void);\n"
    (root / "inc/demo.h").write_text(header, encoding="utf-8")
    c_source = root / "build/retained.c"
    c_source.write_text("int demo_retained(void) { return 0; }\n", encoding="utf-8")
    row = json.loads(json.dumps(base))
    row["compatibility_sha256"] = _normalized_header_digest(header)
    row["exports"].append(
        {
            "name": "demo_retained",
            "calling_context": "task-only-non-reentrant",
            "ownership": "Uses no retained caller resource.",
        }
    )
    row["retained_c_exports"] = [{"name": "demo_retained", "source": "build/retained.c"}]
    if findings := _library_findings(row, {"host", "ra8"}, root):
        return f"must-stay-quiet retained C export fixture failed: {findings}"
    zig_owned = {item["name"] for item in row["exports"]} - {
        item["name"] for item in row["retained_c_exports"]
    }
    if zig_owned != {"demo_run"}:
        return f"retained C export leaked into Zig archive expectations: {zig_owned}"
    if _compiled_symbol_findings("demo", zig_owned, {"demo_run"}):
        return "must-stay-quiet Zig archive omitted the expected Zig-owned export"
    if not any(
        "unexpected export(s): demo_retained" in finding
        for finding in _compiled_symbol_findings("demo", zig_owned, {"demo_run", "demo_retained"})
    ):
        return "must-fire fixture accepted retained C export in the Zig archive"
    for mutation, message in (
        ("missing", "missing retained C source"),
        ("outside", "retained C source is outside build root"),
        ("declaration", "retained C source does not define"),
    ):
        broken = json.loads(json.dumps(row))
        if mutation == "missing":
            broken["retained_c_exports"][0]["source"] = "build/missing.c"
        elif mutation == "outside":
            broken["retained_c_exports"][0]["source"] = "../outside.c"
        else:
            c_source.write_text("int demo_retained(void);\n", encoding="utf-8")
        findings = _library_findings(broken, {"host", "ra8"}, root)
        if not any(message in finding for finding in findings):
            return f"must-fire fixture accepted invalid retained C ownership: {mutation}"
        if mutation == "declaration":
            c_source.write_text("int demo_retained(void) { return 0; }\n", encoding="utf-8")
    (root / "inc/demo.h").write_text(original_header, encoding="utf-8")
    return None


def _selftest_additional_header_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise typed public and test-only headers in the combined ABI inventory."""
    path = root / "build/inc/demo_internal.h"
    path.parent.mkdir(parents=True, exist_ok=True)
    header = "int demo_private_run(void);\n"
    path.write_text(header, encoding="utf-8")
    adapter_path = root / "build/adapter.zig"
    original_adapter = adapter_path.read_text(encoding="utf-8")
    adapter_path.write_text(
        original_adapter + "pub export fn demo_private_run() callconv(.c) i32 { return 0; }\n",
        encoding="utf-8",
    )
    with_header = json.loads(json.dumps(base))
    with_header["additional_headers"] = [
        {
            "path": "build/inc/demo_internal.h",
            "role": "test-only",
            "symbol_prefix": "demo_private_",
            "compatibility_sha256": _normalized_header_digest(header),
        }
    ]
    with_header["exports"].append(
        {
            "name": "demo_private_run",
            "calling_context": "task-only-non-reentrant",
            "ownership": "Uses no caller-owned resources.",
        }
    )
    findings = _library_findings(with_header, {"host", "ra8"}, root)
    if findings:
        return f"must-stay-quiet additional header fixture failed: {findings}"
    broken = json.loads(json.dumps(with_header))
    broken["additional_headers"][0]["role"] = "public"
    findings = _library_findings(broken, {"host", "ra8"}, root)
    if not any(
        "public additional header must use the library symbol prefix" in item for item in findings
    ):
        return "must-fire fixture accepted a public header with a mismatched symbol prefix"

    public_header = "int demo_public_run(void);\n"
    path.write_text(public_header, encoding="utf-8")
    adapter_path.write_text(
        original_adapter + "pub export fn demo_public_run() callconv(.c) i32 { return 0; }\n",
        encoding="utf-8",
    )
    public_row = json.loads(json.dumps(base))
    public_row["additional_headers"] = [
        {
            "path": "build/inc/demo_internal.h",
            "role": "public",
            "symbol_prefix": "demo_",
            "compatibility_sha256": _normalized_header_digest(public_header),
        }
    ]
    public_row["exports"].append(
        {
            "name": "demo_public_run",
            "calling_context": "task-only-non-reentrant",
            "ownership": "borrows no caller-owned resources.",
        }
    )
    findings = _library_findings(public_row, {"host", "ra8"}, root)
    if findings:
        return f"must-stay-quiet public additional header fixture failed: {findings}"
    broken = json.loads(json.dumps(public_row))
    broken["additional_headers"][0]["compatibility_sha256"] = "0" * 64
    findings = _library_findings(broken, {"host", "ra8"}, root)
    if not any("additional header compatibility drift" in item for item in findings):
        return "must-fire fixture accepted public additional-header digest drift"
    broken = json.loads(json.dumps(public_row))
    broken["additional_headers"][0]["path"] = "../outside.h"
    findings = _library_findings(broken, {"host", "ra8"}, root)
    if not any("additional header is outside build root" in item for item in findings):
        return "must-fire fixture accepted public additional header outside its build root"
    return None


def _selftest_library_test_exports(base: dict[str, Any], root: Path) -> str | None:
    """Ignore implementation exports placed under a Zig library's test directory."""
    test_source = root / "libs/demo/tests/mock.zig"
    test_source.parent.mkdir(parents=True)
    test_source.write_text(
        "pub export fn demo_test_only() callconv(.c) void {}\n", encoding="utf-8"
    )
    findings = _repository_inventory_findings([base], root)
    if findings:
        return f"must-stay-quiet library test fixture failed: {findings}"
    link_stub = root / "build/tests/link_stub.zig"
    link_stub.parent.mkdir(parents=True)
    link_stub.write_text("export fn demo_test_stub() callconv(.c) void {}\n", encoding="utf-8")
    findings = _adapter_scope_findings("demo", root / "build", root / "build/adapter.zig", root)
    if findings:
        return f"must-stay-quiet adapter test-stub fixture failed: {findings}"
    rogue = root / "build/rogue.zig"
    rogue.write_text("export fn demo_rogue() callconv(.c) void {}\n", encoding="utf-8")
    findings = _adapter_scope_findings("demo", root / "build", root / "build/adapter.zig", root)
    if not any("demo_rogue" in item for item in findings):
        return "must-fire fixture was accepted: production export outside adapter"
    return None


def _selftest_cmake_glob_registration(root: Path) -> str | None:
    """Exercise glob-based C test registration and explicit removal detection."""
    tests_root = root / "tests"
    registration = tests_root / "cmake/unit_tests.cmake"
    registration.parent.mkdir(parents=True, exist_ok=True)
    path = tests_root / "usb/src/test_demo.c"
    path.parent.mkdir(parents=True, exist_ok=True)
    text = (
        "file(GLOB RA8_TEST_SOURCES CONFIGURE_DEPENDS "
        "${CMAKE_CURRENT_SOURCE_DIR}/*/src/test_*.c)\n"
        "foreach(src ${RA8_TEST_SOURCES})\n"
        "  ra8_add_test(${name} ${src})\n"
        "endforeach()\n"
    )
    if not _cmake_glob_registers_test(path, registration, text):
        return "must-stay-quiet CMake test glob fixture failed"
    removed = (
        text
        + "list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/usb/src/test_demo.c)\n"
    )
    if _cmake_glob_registers_test(path, registration, removed):
        return "must-fire fixture was accepted: CMake glob explicitly removes test"
    return None


def _selftest_compiled_policy(base: dict[str, Any], root: Path) -> str | None:
    """Exercise compiled target counts and both symbol-drift directions when tools exist."""
    zig = shutil.which("zig")
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if zig is None or nm is None:
        return None
    archive_output = (
        "libdemo.a:/build/adapter.o:0000000000000000 T demo_run\n"
        "libdemo.a:/zig-cache/compiler_rt.o:0000000000000000 W __zig_probe_stack\n"
        "libdemo.a:/build/rogue.o:0000000000000000 T demo_extra"
    )
    archive_symbols = _archive_symbol_names(archive_output, bundle_compiler_rt=True)
    if archive_symbols != {"demo_run", "demo_extra"}:
        return f"archive member provenance filter failed: {archive_symbols}"
    findings = _compiled_symbol_findings("demo", {"demo_run"}, archive_symbols)
    if not any("unexpected export(s): demo_extra" in item for item in findings):
        return "must-fire fixture was accepted: rogue user archive member export"
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


def _selftest_archive_member_policy(root: Path) -> str | None:
    """Prove bundled runtime symbols are filtered by archive-member provenance."""
    parsed = _archive_symbol_names(
        "libdemo.a(adapter.o): 00000000 T demo_run\n"
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    retained = _archive_symbol_names(
        "libdemo.a(adapter.o): 00000000 T __zig_probe_stack\n"
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    bracket_member = _archive_symbol_names(
        "libdemo.a[adapter.o]: 00000000 T demo_run\n"
        "libdemo.a[compiler_rt.o]: 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=True,
    )
    colon_member = _archive_symbol_names(
        "libdemo.a:/opt/zig-cache/compiler_rt.o:00000000 W __zig_probe_stack\n"
        "libdemo.a:/opt/zig-cache/adapter.o:00000000 T demo_run\n",
        bundle_compiler_rt=True,
    )
    unbundled = _archive_symbol_names(
        "libdemo.a(compiler_rt.o): 00000000 T __zig_probe_stack\n",
        bundle_compiler_rt=False,
    )
    cases = (
        (parsed, {"demo_run"}, "paren-form runtime member was not filtered"),
        (retained, {"_zig_probe_stack"}, "runtime-named library export was hidden"),
        (bracket_member, {"demo_run"}, "bracket-form member was not parsed"),
        (colon_member, {"demo_run"}, "colon-form member was not parsed"),
        (unbundled, {"_zig_probe_stack"}, "unbundled runtime member was hidden"),
    )
    for actual, expected, message in cases:
        if actual != expected:
            return f"{message}: expected={expected}, actual={actual}"

    flag_root = root / "compiler-rt-flag-selftest"
    flag_root.mkdir()
    build_file = flag_root / "build.zig"
    for source, expected in (
        ("library.bundle_compiler_rt = true;\n", True),
        ("library.bundle_compiler_rt = false;\n", False),
        ("// library.bundle_compiler_rt = true;\n", False),
    ):
        build_file.write_text(source, encoding="utf-8")
        if _bundle_compiler_rt_enabled(flag_root) is not expected:
            return f"compiler runtime flag detection mismatch: {source.strip()}"
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
    field_fragment = '@offsetOf(Regs, "TYPE") == 0x00'
    field_assertion = (
        "// The source's ABI field offset is asserted below.\n"
        'std.debug.assert(@offsetOf(Regs, "TYPE") == 0x00);\n'
    )
    if _compatibility_findings(
        {
            "compatibility_sha256": _normalized_header_digest(""),
            "layout_assertions": [field_fragment],
        },
        "demo",
        "",
        field_assertion,
    ):
        return "must-stay-quiet Zig field-offset assertion was rejected"
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
        for check in (_selftest_target_policy, _selftest_mode_policy):
            if error := check(base, root):
                print(error, file=sys.stderr)
                return 1
        for check in (
            lambda: _selftest_source_policy(base, root, adapter),
            lambda: _selftest_additional_adapter_policy(base, root),
            lambda: _selftest_metadata_policy(base, root),
            lambda: _selftest_retained_c_exports(base, root),
            lambda: _selftest_additional_header_policy(base, root),
            lambda: _selftest_library_test_exports(base, root),
            lambda: _selftest_cmake_glob_registration(root),
            lambda: _selftest_compiled_policy(base, root),
            lambda: _selftest_archive_member_policy(root),
            _selftest_layout_assertions,
        ):
            if error := check():
                print(error, file=sys.stderr)
                return 1
    print(
        "check_zig_abi_policy.py --selftest: OK "
        "(quiet + all named failures + runtime provenance)."
    )
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

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the declared Zig-to-C ABI inventory against source and archives."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, NamedTuple

from zig_abi_inventory import (
    GENERATED_PATH_PARTS,
    _boundary_adapter_paths,
    _boundary_adapter_rows,
    _boundary_header_rows,
    _repository_inventory_findings,
    _source_inventory_for_library,
)
from zig_abi_lexer import (
    _contains_token_sequence,
    _header_exports,
    _normalized_header_digest,
    _strip_comments,
    _symbol_root,
    _zig_exports,
)

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
ADDITIONAL_HEADER_ROLES = {"test-only", "internal"}
MIN_OWNERSHIP_LENGTH = 12
MIN_NM_SYMBOL_FIELDS = 3
# tests/<suite>/src/test_<x>.c -- the shape unit_tests.cmake globs.
GLOBBED_TEST_PATH_PARTS = 4
# nm's archive form, "archive.a:member.o:address", carries two colons.
MIN_NM_MEMBER_COLONS = 2
REQUIRED_MODES = {"Debug", "ReleaseSafe", "ReleaseSmall"}
MODE_C_FLAGS = {"Debug": "-O0", "ReleaseSafe": "-O2", "ReleaseSmall": "-Oz"}
RA8_ZIG_ARGUMENTS = (
    "-Dtarget=thumb-freestanding-eabihf",
    "-Dcpu=cortex_m85+fp_armv8-d32-fp64",
)
RA8_C_ARGUMENTS = ("-mcpu=cortex_m85", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
PROHIBITED_ZIG_TYPES = (
    (re.compile(r"\[\](?:const\s+)?"), "slice"),
    (re.compile(r"(?<![=!])!(?!=)"), "error union"),
    (re.compile(r"\bbool\b"), "bool"),
    (re.compile(r"\banytype\b"), "anytype"),
    (re.compile(r"\bcomptime\b"), "comptime"),
)


class PolicyError(Exception):
    """Raised when the policy file itself is unreadable or malformed."""


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


def _data_export_findings(
    library: dict[str, Any], name: str, repository_root: Path
) -> tuple[list[str], set[str]]:
    """Validate exported mutable data at a C/Zig boundary.

    A ported protocol core can own a single shared instance the retained C
    translation unit still reads: ra8_sdmmc_spi keeps src/ra8_sdmmc_spi_io.c,
    which drives ``g_sdmmc_spi_state`` the Zig adapter now defines. That is a
    data symbol, not a function, so the export checks above cannot see it, and
    a silent second definition would link two card states. Each row names the
    header that declares it ``extern`` and the adapter that defines it, and
    both have to still say so.
    """
    rows = library.get("data_exports", [])
    if not isinstance(rows, list):
        return [f"{name}: data_exports is not a list"], set()
    findings: list[str] = []
    declared: set[str] = set()
    build_root = (repository_root / library.get("build_root", "<missing>")).resolve()
    for row in rows:
        if (
            not isinstance(row, dict)
            or set(row) != {"name", "header", "adapter", "calling_context", "ownership"}
            or not all(isinstance(row.get(field), str) for field in row)
        ):
            findings.append(f"{name}: malformed data export metadata")
            continue
        symbol = row["name"]
        if symbol in declared:
            findings.append(f"{name}: duplicate data export: {symbol}")
            continue
        declared.add(symbol)
        if row["calling_context"] not in CONTEXTS:
            findings.append(f"{name}: undocumented calling context: {symbol}")
        if len(row["ownership"].strip()) < MIN_OWNERSHIP_LENGTH:
            findings.append(f"{name}: undocumented ownership: {symbol}")
        for field in ("header", "adapter"):
            path = repository_root / row[field]
            try:
                path.resolve().relative_to(build_root)
            except ValueError:
                findings.append(f"{name}: data export {field} is outside build root: {row[field]}")
                continue
            if not path.is_file():
                findings.append(f"{name}: missing data export {field}: {row[field]}")
                continue
            text = _strip_comments(path.read_text(encoding="utf-8"))
            if field == "header":
                present = re.search(
                    rf"\bextern\b[^;{{}}]*\b{re.escape(symbol)}"
                    r"\s*(?:\[[^\]]*\]\s*)*;",
                    text,
                )
            else:
                present = re.search(
                    rf"\bpub\s+export\s+(?:var|const)\s+{re.escape(symbol)}\s*:", text
                )
            if present is None:
                findings.append(
                    f"{name}: data export {field} does not declare {symbol}: {row[field]}"
                )
    return findings, declared


def _c_retained_findings(
    name: str,
    library: dict[str, Any],
    header_names: set[str],
    repository_root: Path,
) -> tuple[list[str], set[str]]:
    """Validate header symbols a port deliberately leaves implemented in C.

    A ported library may keep a support translation unit whose symbols the
    public header still declares: ra8_lsm6dso keeps src/ra8_lsm6dso_bind.c,
    the house-I2C-seam binder, which CALLS the ported ABI rather than
    implementing it. Those names are not Zig exports, so they must not be
    read as header drift. Each entry has to name the C source that defines
    it, and that source has to still define it, so a retired or renamed
    symbol cannot sit here unnoticed.
    """
    rows = library.get("c_retained_exports", [])
    if not isinstance(rows, list):
        return [f"{name}: c_retained_exports is not a list"], set()
    findings: list[str] = []
    retained: set[str] = set()
    for row in rows:
        row_findings, accepted = _c_retention_row_findings(name, row, header_names, repository_root)
        findings.extend(row_findings)
        if accepted is not None:
            retained.add(accepted)
    return findings, retained


def _c_retention_declaration_finding(
    name: str,
    symbol: str,
    sibling_header: object,
    header_names: set[str],
    repository_root: Path,
) -> str | None:
    """Return why a retained symbol is not declared where the row says it is.

    A row either names its own sibling header or leans on the library's public
    header. Both spellings have to still declare the symbol, so a renamed or
    retired declaration cannot sit here unnoticed.

    ``sibling_header`` is typed ``object`` because it arrives straight from the
    policy file: proving it is a usable path is this function's job, so it
    cannot be annotated as one on the way in.
    """
    if sibling_header is None:
        if symbol not in header_names:
            return f"{name}: stale C retention, absent from the public header: {symbol}"
        return None
    if not isinstance(sibling_header, str) or not sibling_header:
        return f"{name}: c_retained_exports header is not a path: {symbol}"
    header_path = repository_root / sibling_header
    if not header_path.exists():
        return f"{name}: missing C retention header: {sibling_header}"
    declared_here = _header_exports(header_path.read_text(encoding="utf-8"), _symbol_root(symbol))
    if symbol not in declared_here:
        return f"{name}: stale C retention, absent from {sibling_header}: {symbol}"
    return None


def _c_retention_source_finding(
    name: str,
    symbol: str,
    source: object,
    repository_root: Path,
) -> str | None:
    """Return why the C source a retained symbol names does not define it.

    ``source`` is typed ``object`` for the same reason as ``sibling_header``
    above: it arrives unvalidated from the policy file. A file that no longer
    defines the symbol is the case this exists to catch, because that is how a
    retention silently outlives the code it describes.
    """
    if not isinstance(source, str) or not source:
        return f"{name}: c_retained_exports lacks a source: {symbol}"
    path = repository_root / source
    if not path.exists():
        return f"{name}: missing C retention source: {source}"
    retention_text = _strip_comments(path.read_text(encoding="utf-8"))
    if not re.search(rf"\b{re.escape(symbol)}\s*\(", retention_text):
        return f"{name}: C retention source does not define {symbol}: {source}"
    return None


def _c_retention_row_findings(
    name: str,
    row: object,
    header_names: set[str],
    repository_root: Path,
) -> tuple[list[str], str | None]:
    """Validate one c_retained_exports row, returning the symbol it accepts.

    The second element is the symbol to treat as C-owned, or None when the row
    did not survive validation. An undocumented reason is reported but does not
    on its own disqualify the row, which is why it is not an early return.
    ``row`` is typed ``object`` because it is raw policy input.
    """
    if not isinstance(row, dict):
        return [f"{name}: c_retained_exports row is not an object"], None
    symbol = row.get("name")
    reason = row.get("reason")
    if not isinstance(symbol, str) or not symbol:
        return [f"{name}: c_retained_exports row lacks a name"], None
    findings: list[str] = []
    if not isinstance(reason, str) or len(reason.strip()) < MIN_OWNERSHIP_LENGTH:
        findings.append(f"{name}: undocumented C retention: {symbol}")
    declaration = _c_retention_declaration_finding(
        name, symbol, row.get("header"), header_names, repository_root
    )
    if declaration is not None:
        findings.append(declaration)
        return findings, None
    source_finding = _c_retention_source_finding(name, symbol, row.get("source"), repository_root)
    if source_finding is not None:
        findings.append(source_finding)
        return findings, None
    return findings, symbol


def _additional_header_findings(
    name: str,
    library: dict[str, Any],
    repository_root: Path,
) -> tuple[list[str], set[str]]:
    """Return findings plus the names declared by a library's non-public headers.

    A port keeps the C ABI it replaced, and for some libraries part of that ABI
    is an internal header the host suite includes to drive predicates the
    optimizer would otherwise fold away. ra8_usb_pal is the first: the two
    priv_usb_pal_* predicates live in src/ra8_usb_pal_internal.h, not in the
    public header, and the Zig archive exports them under the same names. Each
    row has to name its role and the prefix it contributes, so an internal
    header cannot quietly widen the public surface.
    """
    findings: list[str] = []
    declared: set[str] = set()
    rows = library.get("additional_headers", [])
    if not isinstance(rows, list):
        return [f"{name}: additional_headers is not a list"], declared
    for row in rows:
        if not isinstance(row, dict):
            findings.append(f"{name}: additional_headers row is not an object")
            continue
        path_value = row.get("path")
        role = row.get("role")
        prefix = row.get("symbol_prefix")
        if not isinstance(path_value, str) or not path_value:
            findings.append(f"{name}: additional_headers row lacks a path")
            continue
        if role not in ADDITIONAL_HEADER_ROLES:
            findings.append(f"{name}: invalid additional header role: {path_value}")
            continue
        if not isinstance(prefix, str) or not prefix:
            findings.append(f"{name}: additional header lacks a symbol_prefix: {path_value}")
            continue
        path = repository_root / path_value
        if not path.exists():
            findings.append(f"{name}: missing additional header: {path_value}")
            continue
        names = _header_exports(path.read_text(encoding="utf-8"), prefix)
        if not names:
            findings.append(f"{name}: additional header declares no {prefix} symbol: {path_value}")
            continue
        declared |= names
    return findings, declared


class _ExportSets(NamedTuple):
    """The three views of one library's exports that have to agree.

    ``declared`` is what the policy metadata pins, ``header_names`` what the
    public header declares, and ``zig_names``/``zig_heads`` what the adapter
    actually exports and the source line each export is declared on. They are
    carried together because no caller has a reason to compare a subset.
    """

    declared: set[str]
    header_names: set[str]
    zig_names: set[str]
    zig_heads: dict[str, str]


def _inventory_findings(
    name: str,
    exports: _ExportSets,
    allow_c_bool: set[str] | None = None,
) -> list[str]:
    """Compare metadata, header, and Zig declarations and reject native-only types."""
    declared = exports.declared
    zig_names = exports.zig_names
    findings: list[str] = []
    for label, actual in (("header", exports.header_names), ("Zig adapter", zig_names)):
        missing = sorted(declared - actual)
        unexpected = sorted(actual - declared)
        if missing:
            findings.append(f"{name}: {label} missing export(s): {', '.join(missing)}")
        if unexpected:
            findings.append(f"{name}: {label} unexpected export(s): {', '.join(unexpected)}")
    for symbol in sorted(declared & zig_names):
        head = exports.zig_heads[symbol]
        if "callconv(.c)" not in re.sub(r"\s+", "", head):
            findings.append(f"{name}: export lacks callconv(.c): {symbol}")
        findings.extend(
            f"{name}: prohibited Zig-only type {type_name}: {symbol}"
            for pattern, type_name in PROHIBITED_ZIG_TYPES
            if pattern.search(head)
            and not (type_name == "bool" and symbol in (allow_c_bool or set()))
        )
    return findings


def _adapter_prefix_findings(
    name: str,
    library: dict[str, Any],
    repository_root: Path,
) -> list[str]:
    """Reject an adapter export outside the namespaces that adapter declares."""
    findings: list[str] = []
    for row in _boundary_adapter_rows(library):
        if "symbol_prefixes" not in row:
            continue
        accepted = row["symbol_prefixes"]
        if (
            not isinstance(accepted, list)
            or not accepted
            or not all(isinstance(item, str) and item for item in accepted)
            or len(set(accepted)) != len(accepted)
        ):
            findings.append(f"{name}: malformed adapter symbol_prefixes: {row['path']}")
            continue
        text = (repository_root / row["path"]).read_text(encoding="utf-8")
        names, _heads = _zig_exports(text)
        unexpected = sorted(
            symbol for symbol in names if not any(symbol.startswith(item) for item in accepted)
        )
        if unexpected:
            findings.append(
                f"{name}: adapter export prefix mismatch in {row['path']}: " + ", ".join(unexpected)
            )
    return findings


def _adapter_scope_findings(
    name: str,
    build_root: Path,
    adapters: set[Path],
    repository_root: Path = ROOT,
) -> list[str]:
    """Reject exports outside the explicitly registered source inventory."""
    findings: list[str] = []
    for source in build_root.rglob("*.zig"):
        generated = any(part in GENERATED_PATH_PARTS for part in source.parts)
        if source.resolve() in adapters or generated:
            continue
        other_names, _ = _zig_exports(source.read_text(encoding="utf-8"))
        if other_names:
            relative = source.relative_to(repository_root)
            findings.append(
                f"{name}: export outside adapter {relative}: " + ", ".join(sorted(other_names))
            )
    return findings


def _compatibility_findings(
    row: dict[str, Any], name: str, header_text: str, adapter_text: str
) -> list[str]:
    """Check one released normalized header and its representation assertions.

    ``row`` is a pinned header row from :func:`_boundary_header_rows`, so the
    single-header and multi-header spellings reach identical checks. Findings
    name the header when a library fronts more than one, because "compatibility
    drift" against three headers is otherwise unattributable.
    """
    findings: list[str] = []
    label = f"{name} ({row['path']})" if row.get("multi") else name
    expected = row.get("compatibility_sha256")
    actual = _normalized_header_digest(header_text)
    if expected != actual:
        findings.append(f"{label}: compatibility drift: expected {expected}, got {actual}")
    findings.extend(
        f"{label}: missing representation assertion: {fragment}"
        for fragment in row.get("layout_assertions", [])
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
        registration_source = registration.read_text(encoding="utf-8")
        registration_text = (
            _strip_comments(registration_source)
            if registration.suffix == ".json"
            else re.sub(r"(?m)#.*$", "", registration_source)
        )
        if registration.suffix == ".json":
            findings.extend(
                _json_registration_findings(name, language, path, symbol_path, registration)
            )
        else:
            explicitly_registered = path.name in registration_text
            path_parts = Path(path_value).parts
            normalized_registration = " ".join(registration_text.split())
            cmake_globs_test_sources = (
                "file(GLOB RA8_TEST_SOURCES" in normalized_registration
                and "${CMAKE_CURRENT_SOURCE_DIR}/*/src/test_*.c" in normalized_registration
            )
            removed_test_paths = re.findall(
                r"list\s*\(\s*REMOVE_ITEM\s+RA8_TEST_SOURCES\b([^)]*)\)",
                registration_text,
                flags=re.DOTALL,
            )
            glob_registered = (
                registration.name == "unit_tests.cmake"
                and len(path_parts) == GLOBBED_TEST_PATH_PARTS
                and path_parts[0] == "tests"
                and path_parts[2] == "src"
                and path_parts[3].startswith("test_")
                and path_parts[3].endswith(".c")
                and cmake_globs_test_sources
                and not any(path.name in command for command in removed_test_paths)
            )
            if not explicitly_registered and not glob_registered:
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


def _library_paths_findings(
    library: dict[str, Any],
    name: str,
    header_rows: list[dict[str, Any]],
    adapter_paths: list[str],
    repository_root: Path = ROOT,
) -> tuple[list[str], Path]:
    """Check the build root, pinned headers and adapters all exist on disk.

    Returns the findings and the build root, so the caller does not rebuild a
    path it already had to resolve to report a missing one.
    """
    findings: list[str] = []
    if not header_rows:
        findings.append(f"{name}: missing public_header")
    if not adapter_paths:
        findings.append(f"{name}: missing adapter")
    build_root_value = library.get("build_root")
    build_root = (
        repository_root / build_root_value
        if isinstance(build_root_value, str)
        else repository_root / "<missing>"
    )
    if not build_root.exists():
        findings.append(f"{name}: missing build_root: {build_root_value}")
    for row in header_rows:
        value = row.get("path")
        if not isinstance(value, str) or not (repository_root / value).exists():
            findings.append(f"{name}: missing public_header: {value}")
    findings.extend(
        f"{name}: missing adapter: {value}"
        for value in adapter_paths
        if not (repository_root / value).exists()
    )
    return findings, build_root


def _header_namespace_names(
    name: str, header_rows: list[dict[str, Any]], header_texts: dict[str, str], prefix: str
) -> tuple[set[str], list[str]]:
    """Collect the declared C symbols across every pinned header.

    A pinned header may front a different namespace than the library's own:
    ra8_ota publishes the ra8_ota_ API from inc/ra8_ota.h and the promoted
    priv_ota_ predicates from src/ra8_ota_internal.h, and both are ABI the port
    has to keep. A row without its own symbol_prefix reads as before.
    """
    names: set[str] = set()
    findings: list[str] = []
    for row in header_rows:
        row_prefix = row.get("symbol_prefix", prefix)
        if not isinstance(row_prefix, str) or not row_prefix:
            findings.append(f"{name}: malformed public header symbol_prefix: {row['path']}")
            continue
        names |= _header_exports(header_texts[row["path"]], row_prefix)
    return names, findings


def _layout_assertion_text(
    name: str, library: dict[str, Any], build_root: Path, repository_root: Path
) -> tuple[str, list[str]]:
    """Gather representation-assertion text from the declared layout sources.

    Representation assertions do not have to sit in the adapter: a port that
    keeps its layout comptime asserts with the types they describe names those
    files here, and they must live inside the build root so a row cannot claim
    evidence from another library.
    """
    layout_sources = library.get("layout_sources", [])
    if not isinstance(layout_sources, list) or not all(
        isinstance(value, str) for value in layout_sources
    ):
        return "", [f"{name}: layout_sources must be a list of paths"]
    findings: list[str] = []
    text = ""
    for value in layout_sources:
        path = repository_root / value
        try:
            path.resolve().relative_to(build_root.resolve())
        except ValueError:
            findings.append(f"{name}: layout source is outside build root: {value}")
            continue
        if not path.is_file():
            findings.append(f"{name}: missing layout source: {value}")
            continue
        text += "\n" + path.read_text(encoding="utf-8")
    return text, findings


def _library_findings(
    library: dict[str, Any],
    required_targets: set[str],
    repository_root: Path = ROOT,
    source_inventory: list[dict[str, Any]] | None = None,
) -> list[str]:
    """Return source, metadata, test, and compatibility findings for one library."""
    name = library.get("name", "<unnamed>")
    prefix = library.get("symbol_prefix")
    if not isinstance(prefix, str) or not prefix:
        return [f"{name}: missing symbol_prefix"]
    header_rows = _boundary_header_rows(library)
    adapter_paths = _boundary_adapter_paths(library)
    findings, build_root = _library_paths_findings(
        library, name, header_rows, adapter_paths, repository_root
    )
    if findings:
        return findings

    header_texts = {
        row["path"]: (repository_root / row["path"]).read_text(encoding="utf-8")
        for row in header_rows
    }
    adapter_texts = {
        value: (repository_root / value).read_text(encoding="utf-8") for value in adapter_paths
    }
    # The boundary is the union: a declaration in any pinned header is part of
    # the C ABI this library fronts, and an export in any registered adapter is
    # the Zig side of it. Checking them per-file would reject a facade whose
    # backend header declares what the backend adapter exports.
    header_text = "\n".join(header_texts.values())
    adapter_text = "\n".join(adapter_texts.values())
    header_names, prefix_findings = _header_namespace_names(name, header_rows, header_texts, prefix)
    findings.extend(prefix_findings)
    zig_names, zig_heads = _zig_exports(adapter_text)
    metadata_findings, declared = _metadata_findings(name, library.get("exports"))
    findings.extend(metadata_findings)
    data_findings, _data_exports = _data_export_findings(library, name, repository_root)
    findings.extend(data_findings)
    allow_c_bool = {
        row["name"]
        for row in library.get("exports", [])
        if isinstance(row, dict) and row.get("allow_c_bool") is True
    }
    if allow_c_bool and "bool" not in header_text:
        findings.append(f"{name}: C bool exception lacks a bool declaration in the public header")
    additional_findings, additional_names = _additional_header_findings(
        name, library, repository_root
    )
    findings.extend(additional_findings)
    header_names |= additional_names
    retention_findings, retained = _c_retained_findings(
        name, library, header_names, repository_root
    )
    findings.extend(retention_findings)
    findings.extend(
        _inventory_findings(
            name,
            _ExportSets(declared, header_names - retained, zig_names, zig_heads),
            allow_c_bool,
        )
    )
    declared_sources = _source_inventory_for_library(
        source_inventory or [], library, repository_root
    ) | {(repository_root / value).resolve() for value in adapter_paths}
    findings.extend(
        _adapter_scope_findings(name, build_root.resolve(), declared_sources, repository_root)
    )
    layout_text, layout_findings = _layout_assertion_text(
        name, library, build_root, repository_root
    )
    findings.extend(layout_findings)
    assertion_text = adapter_text + layout_text
    findings.extend(_adapter_prefix_findings(name, library, repository_root))
    multi_header = len(header_rows) > 1
    for row in header_rows:
        findings.extend(
            _compatibility_findings(
                {**row, "multi": multi_header},
                name,
                header_texts[row["path"]],
                assertion_text,
            )
        )
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


def _archive_symbols(
    nm: str, archive: Path, name: str, *, bundle_compiler_rt: bool = False
) -> tuple[list[str], set[str]]:
    """Read one archive's global defined symbols with the resolved nm tool."""
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


def _archive_symbol_names(output: str, *, bundle_compiler_rt: bool = False) -> set[str]:
    """Parse GNU/LLVM nm -A output, excluding only bundled compiler runtime members."""
    actual: set[str] = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < MIN_NM_SYMBOL_FIELDS:
            continue
        # GNU nm -A emits archive(member): before the symbol fields. Ignore only
        # the compiler runtime member supplied explicitly by Zig's build, never
        # similarly named symbols from the library's own object files.
        member_match = re.search(r"[\[(]([^\)\]]+)[\)\]]", parts[0])
        member = member_match.group(1) if member_match else ""
        if not member and parts[0].count(":") >= MIN_NM_MEMBER_COLONS:
            archive_and_member, _address = parts[0].rsplit(":", 1)
            _archive, member = archive_and_member.split(":", 1)
        if bundle_compiler_rt and Path(member).name == "compiler_rt.o":
            continue
        actual.add(parts[-1].removeprefix("_"))
    return actual


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
            build_source = (repository_root / library["build_root"] / "build.zig").read_text(
                encoding="utf-8"
            )
            bundle_compiler_rt = bool(
                re.search(r"\blibrary\.bundle_compiler_rt\s*=\s*true\s*;", build_source)
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
            expected = {row["name"] for row in library["exports"]} | {
                row["name"]
                for row in library.get("data_exports", [])
                if isinstance(row, dict) and isinstance(row.get("name"), str)
            }
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
    source_inventory = policy.get("source_inventory", [])
    if not isinstance(source_inventory, list):
        findings.append("policy source_inventory must be a list")
        source_inventory = []
    findings.extend(
        _repository_inventory_findings(typed_libraries, source_inventory, repository_root)
    )
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
        findings.extend(
            _library_findings(library, required_targets, repository_root, source_inventory)
        )
        counts["headers"] += len(_boundary_header_rows(library))
        counts["adapters"] += len(_boundary_adapter_paths(library))
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


def main() -> int:
    """Run self-test, print normalized digests, or enforce the committed policy."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--print-digests", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        # Imported here, not at module scope: the fixtures import this module
        # back for the rules they exercise, and nothing but --selftest needs
        # them.
        from zig_abi_policy_selftest import _selftest  # noqa: PLC0415

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

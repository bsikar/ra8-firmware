# SPDX-License-Identifier: Apache-2.0
"""Repository source inventory for the Zig ABI policy checker.

Answers one question for :mod:`check_zig_abi_policy`: for a library named in
the policy, which files on disk actually carry its boundary, and do the
sources the policy claims match the sources the repository has?

The boundary row readers live here too. They are what "a library's declared
surface" means, so inventory is their main consumer, and keeping them in
this module is what lets it import strictly downward: the lexer and its own
constants, never the checker that calls it.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, NamedTuple

from zig_abi_lexer import _header_exports, _strip_comments, _zig_exports

ROOT = Path(__file__).resolve().parents[2]

GENERATED_PATH_PARTS = {".zig-cache", "zig-out"}

# libs/<name>/... -- the name is readable only once there are parts past it.
LIBS_SOURCE_MIN_PARTS = 2

# libs/<name>/src/<file> -- a library-support source sits exactly this deep.
LIBRARY_SRC_PATH_PARTS = 4

INVENTORY_SOURCE_KINDS = {"library-adapter", "library-support", "test-helper", "app-adapter"}

# The two kinds that publish symbols across the C boundary; the rest are internal.
ADAPTER_SOURCE_KINDS = {"library-adapter", "app-adapter"}

REPOSITORY_EXCLUDED_PATH_PARTS = {*GENERATED_PATH_PARTS, "third_party"}

# scripts/ci/lib/lang_toolchains.sh unpacks the pinned Zig release into
# build/tools/, so the upstream standard library lands inside the worktree.
PROVISIONED_TOOLCHAIN_PREFIX = ("build", "tools")


def _boundary_header_rows(library: dict[str, Any]) -> list[dict[str, Any]]:
    """Return every pinned public-header row for one library, oldest key first.

    A library can front more than one public header: ra8_audio publishes the
    transport-neutral facade plus one header per backend, and each carries its
    own normalized digest and representation assertions. The single-header
    spelling stays valid and reads as a one-row list, so a row written before
    multi-boundary support keeps its exact meaning.
    """
    single = library.get("public_header")
    rows: list[dict[str, Any]] = []
    if isinstance(single, str):
        rows.append(
            {
                "path": single,
                "compatibility_sha256": library.get("compatibility_sha256"),
                "layout_assertions": library.get("layout_assertions", []),
            }
        )
    extra = library.get("public_headers")
    if isinstance(extra, list):
        rows.extend(row for row in extra if isinstance(row, dict))
    return rows


def _boundary_adapter_rows(library: dict[str, Any]) -> list[dict[str, Any]]:
    """Return every adapter as a row, the single-key spelling first.

    A bare path reads as a row whose accepted export prefix is the library's
    own ``symbol_prefix``, so every row written before prefix-aware adapters
    keeps its exact meaning. A library whose archive fronts two namespaces
    spells the wider ones out: ra8_ota exports the public ``ra8_ota_`` API and
    the promoted ``priv_ota_`` predicates from one adapter, and the parser
    adapter beside it exports only ``priv_ota_``, which a single library-wide
    prefix cannot express.
    """
    rows: list[dict[str, Any]] = []
    single = library.get("adapter")
    if isinstance(single, str):
        rows.append({"path": single})
    extra = library.get("adapters")
    if isinstance(extra, list):
        for item in extra:
            if isinstance(item, str):
                rows.append({"path": item})
            elif isinstance(item, dict) and isinstance(item.get("path"), str):
                rows.append(item)
    return rows


def _boundary_adapter_paths(library: dict[str, Any]) -> list[str]:
    """Return every adapter path for one library, the single-key spelling first."""
    return [row["path"] for row in _boundary_adapter_rows(library)]


class _InventorySource(NamedTuple):
    """One export-inventory row resolved against the tree it claims to describe.

    ``declared`` is the path exactly as the policy spells it, because every
    finding quotes that spelling rather than the resolved one.
    """

    declared: str
    path: Path
    relative: Path
    root: Path
    owners: list[dict[str, Any]]
    inferred_lib: str | None
    policy_bound: bool


def _resolved_inventory_source(
    row: dict[str, Any], libraries: list[dict[str, Any]], root: Path
) -> _InventorySource:
    """Resolve an inventory row to its owning build roots and inferred library."""
    path = (root / row["path"]).resolve()
    owners = [
        library
        for library in libraries
        if isinstance(library, dict)
        and isinstance(library.get("build_root"), str)
        and isinstance(library.get("name"), str)
        and (root / library["build_root"]).resolve() in path.parents
    ]
    relative = path.relative_to(root)
    return _InventorySource(
        declared=row["path"],
        path=path,
        relative=relative,
        root=root,
        owners=owners,
        inferred_lib=(
            relative.parts[1]
            if len(relative.parts) > LIBS_SOURCE_MIN_PARTS and relative.parts[0] == "libs"
            else None
        ),
        policy_bound=any(
            (root / value).resolve() == path
            for library in owners
            for value in _boundary_adapter_paths(library)
        ),
    )


def _adapter_binding_findings(source: _InventorySource) -> list[str]:
    """Check an adapter no policy row binds against its library's public header."""
    if source.inferred_lib is None or source.relative.parts[2] != "src":
        return [
            f"{source.declared}: adapter must be bound to a policy row or a libs/<name>/src header"
        ]
    header_dir = source.root / "libs" / source.inferred_lib / "inc"
    headers = list(header_dir.glob("*.h")) if header_dir.is_dir() else []
    source_names, _ = _zig_exports(source.path.read_text(encoding="utf-8"))
    header_symbols = (
        set().union(
            *(
                _header_exports(header.read_text(encoding="utf-8"), f"{source.inferred_lib}_")
                for header in headers
            )
        )
        if headers
        else set()
    )
    if not headers or not source_names <= header_symbols:
        return [f"{source.declared}: adapter exports are not declared by its public C header"]
    return []


def _contract_registers(contract: Path, path: Path) -> bool:
    """Report whether one Zig test contract names this path among its sources."""
    try:
        data = json.loads(contract.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    registered = set(data.get("test_roots", [])) | set(data.get("covered_sources", []))
    return path.relative_to(contract.parent).as_posix() in registered


def _test_helper_findings(source: _InventorySource) -> list[str]:
    """Require a test helper to sit under tests/ and be named by a test contract."""
    findings: list[str] = []
    if "tests" not in source.relative.parts:
        findings.append(f"{source.declared}: test-helper is outside tests/")
    contract_roots = [
        source.root / owner["build_root"]
        for owner in source.owners
        if isinstance(owner.get("build_root"), str)
    ]
    if source.inferred_lib is not None:
        contract_roots.append(source.root / "libs" / source.inferred_lib)
    # Every contract is read, not just up to the first match, so that a contract
    # outside this source's own tree still raises the way it always has.
    matches = [
        _contract_registers(contract_root / ".zig-test-contract.json", source.path)
        for contract_root in contract_roots
    ]
    if not any(matches):
        findings.append(f"{source.declared}: test-helper is not declared by a Zig test contract")
    return findings


def _inventory_row_findings(
    row: object, libraries: list[dict[str, Any]], root: Path
) -> tuple[list[str], Path | None]:
    """Classify one declared export source, and return the path it registers.

    ``row`` is deliberately untyped: the malformed-entry finding exists precisely
    because the policy file can carry something that is not a row at all.
    """
    if not isinstance(row, dict) or not isinstance(row.get("path"), str):
        return ["malformed Zig export source inventory entry"], None
    path = (root / row["path"]).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        return [f"export source path escapes repository: {row['path']}"], None
    findings: list[str] = []
    kind = row.get("kind")
    symbols = row.get("symbols")
    if kind not in INVENTORY_SOURCE_KINDS:
        findings.append(f"{row['path']}: invalid export source kind: {kind}")
    if not isinstance(symbols, list) or not all(isinstance(item, str) for item in symbols):
        findings.append(f"{row['path']}: symbols must be an explicit string list")
        symbols = []
    if not path.is_file():
        findings.append(f"registered Zig export source is missing: {row['path']}")
        return findings, None
    source = _resolved_inventory_source(row, libraries, root)
    if kind in ADAPTER_SOURCE_KINDS and not source.policy_bound:
        findings.extend(_adapter_binding_findings(source))
    elif (
        kind in {"library-support", "test-helper"}
        and source.inferred_lib is None
        and len(source.owners) != 1
    ):
        findings.append(f"{row['path']}: source must belong to exactly one library build root")
    if kind == "library-support" and (
        len(source.relative.parts) < LIBRARY_SRC_PATH_PARTS or source.relative.parts[-2] != "src"
    ):
        findings.append(f"{row['path']}: library-support source must be under a library src/")
    names, _ = _zig_exports(path.read_text(encoding="utf-8"))
    if set(symbols) != names:
        findings.append(
            f"{row['path']}: declared symbol inventory differs: "
            f"missing={', '.join(sorted(names - set(symbols)))}; "
            f"unexpected={', '.join(sorted(set(symbols) - names))}"
        )
    if kind == "test-helper":
        findings.extend(_test_helper_findings(source))
    return findings, path


def _discovered_export_sources(repository_root: Path) -> tuple[set[Path], list[str]]:
    """Find every Zig source in the tree that exports across the C boundary."""
    discovered: set[Path] = set()
    findings: list[str] = []
    for source in repository_root.rglob("*.zig"):
        if any(part in REPOSITORY_EXCLUDED_PATH_PARTS for part in source.parts):
            continue
        relative_parts = source.relative_to(repository_root).parts
        if relative_parts[: len(PROVISIONED_TOOLCHAIN_PREFIX)] == PROVISIONED_TOOLCHAIN_PREFIX:
            continue
        text = source.read_text(encoding="utf-8")
        names, _ = _zig_exports(text)
        dynamic_export = re.search(r"\b@export\s*\(", _strip_comments(text))
        if names or dynamic_export:
            discovered.add(source.resolve())
        if dynamic_export:
            findings.append(
                "dynamic @export is prohibited at ABI boundaries: "
                f"{source.relative_to(repository_root)}"
            )
    return discovered, findings


def _repository_inventory_findings(
    libraries: list[dict[str, Any]],
    source_inventory: list[dict[str, Any]] | None = None,
    repository_root: Path = ROOT,
) -> list[str]:
    """Require each hand-written Zig export source and symbol to be classified."""
    registered = {
        (repository_root / value).resolve()
        for library in libraries
        if isinstance(library, dict)
        for value in _boundary_adapter_paths(library)
    }
    findings: list[str] = []
    root = repository_root.resolve()
    for row in source_inventory or []:
        row_findings, declared = _inventory_row_findings(row, libraries, root)
        findings.extend(row_findings)
        if declared is not None:
            registered.add(declared)
    discovered, discovery_findings = _discovered_export_sources(repository_root)
    findings.extend(discovery_findings)
    findings.extend(
        f"unregistered Zig export adapter: {source.relative_to(repository_root)}"
        for source in sorted(discovered - registered)
    )
    findings.extend(
        f"registered adapter has no Zig export: {source.relative_to(repository_root)}"
        for source in sorted(registered - discovered)
    )
    return findings


def _source_inventory_for_library(
    source_inventory: list[dict[str, Any]], library: dict[str, Any], repository_root: Path
) -> set[Path]:
    """Return the exact declared export sources within one library build root."""
    build_root = (repository_root / library["build_root"]).resolve()
    return {
        (repository_root / row["path"]).resolve()
        for row in source_inventory
        if isinstance(row, dict)
        and isinstance(row.get("path"), str)
        and (repository_root / row["path"]).resolve().is_relative_to(build_root)
    }

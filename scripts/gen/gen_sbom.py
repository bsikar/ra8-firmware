#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate and validate the ra8-firmware Software Bill of Materials (SBOM).

This is the supply-chain provenance gate for the vendored third-party SOUP
(Software Of Unknown Provenance) under ``libs/third_party/`` plus the one
bundled font data asset under ``libs/fonts/``.  It emits a machine-readable
CycloneDX 1.5 JSON document at ``docs/sbom/ra8-firmware.cdx.json`` that
records, for every component: name, version, SPDX license (with the
Apache-2.0 election for the dual-licensed crypto), package URL (purl) where
one is meaningful, in-tree path, upstream URL, and provenance class.

The curated ``REGISTRY`` in the sibling module ``sbom_registry.py`` is the
single source of truth for the fields that cannot be derived mechanically
(license election, upstream URL, purl, provenance); this module is the logic
that renders and validates it.  Everything that CAN be cross-checked against
the tree is:

  * **Directory drift** -- every direct child of ``libs/third_party/`` must
    have a registry entry, and every registry directory must exist on disk.
    A newly vendored component with no entry fails the gate.
  * **Version drift** -- for components whose in-tree headers carry a version
    macro (the ThreadX family, Mbed TLS, TF-PSA-Crypto, miniz, TinyXML-2,
    stb), the macro is re-read from source and compared to the recorded
    version.  Versions are never invented; a component with no upstream
    release tag (litehtml, NimBLE dev snapshots) is pinned to the exact
    upstream commit its vendored tree is byte-identical to (T5-09).
  * **License-file presence** -- each entry that names a LICENSE file must
    have it on disk.  stb ships no standalone LICENSE (text in header tails)
    and is reported as a known gap rather than a hard failure.

The emitted JSON is deterministic (no wall-clock timestamp, content-derived
serial number, ``ensure_ascii``) so ``--check`` can compare it byte-for-byte
against the committed file and so the SBOM is reproducible.

Run::

    gen_sbom.py            # regenerate the committed SBOM + print a summary
    gen_sbom.py --check    # fail if the committed SBOM is stale or the tree
                           #   drifted from the registry (the CI/hook gate)
    gen_sbom.py --print    # write nothing; print the SBOM JSON to stdout
    gen_sbom.py --commits  # print `<key> <upstream-commit>` per pinned
                           #   component (consumed by the weekly OSV scan)

Exit 0 if clean, 1 on drift / a catalogued-tree mismatch (including a version
macro that no longer parses). argparse exits 2 on a usage error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import uuid
from pathlib import Path
from typing import TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sbom_registry import (
    PROV_NOT_VENDORED,
    REGISTRY,
    Component,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
THIRD_PARTY_DIR = Path("libs/third_party")
SBOM_REL_PATH = Path("docs/sbom/ra8-firmware.cdx.json")

PROJECT_NAME = "ra8-firmware"
BOM_FORMAT = "CycloneDX"
CYCLONEDX_SPEC = "1.5"
BOM_REVISION = 1
GENERATOR_NAME = "gen_sbom.py"

EXIT_OK = 0
EXIT_DRIFT = 1


def _read_source(comp: Component) -> str | None:
    """Return the text of `comp`'s version-probe file, or None if unreadable."""
    if comp.probe_file is None:
        return None
    path = REPO_ROOT / comp.path / comp.probe_file
    if not path.is_file():
        return None
    return path.read_text(encoding="utf-8", errors="replace")


def probe_version(comp: Component) -> str | None:
    """Re-derive `comp`'s version from its in-tree source, or None.

    Supports two shapes: a single-capture regex (``probe_re``) or a
    MAJOR/MINOR/PATCH macro triplet identified by ``probe_prefix``.
    """
    text = _read_source(comp)
    if text is None:
        return None
    if comp.probe_re is not None:
        match = re.search(comp.probe_re, text)
        return match.group(1) if match else None
    if comp.probe_prefix is not None:
        parts = []
        for level in ("MAJOR", "MINOR", "PATCH"):
            pattern = rf"{comp.probe_prefix}_{level}_VERSION(?:\s+|\s*=\s*)(\d+)"
            match = re.search(pattern, text)
            if match is None:
                return None
            parts.append(match.group(1))
        return ".".join(parts)
    return None


def _third_party_dirs() -> set[str]:
    """Return the direct child directory names under libs/third_party/."""
    base = REPO_ROOT / THIRD_PARTY_DIR
    return {p.name for p in base.iterdir() if p.is_dir()} if base.is_dir() else set()


def _catalogued_top_dirs() -> set[str]:
    """Return the direct-child dir names of libs/third_party/ the registry covers.

    A nested key such as ``fsp_blobs/r_sce_AMC`` catalogues the ``fsp_blobs``
    top-level directory, so a blob tree with sub-components does not read as
    uncatalogued.
    """
    prefix = THIRD_PARTY_DIR.parts
    dirs: set[str] = set()
    for comp in REGISTRY:
        parts = Path(comp.path).parts
        if parts[: len(prefix)] == prefix and len(parts) > len(prefix):
            dirs.add(parts[len(prefix)])
    return dirs


def cross_check() -> tuple[list[str], list[str]]:
    """Cross-check the registry against the tree.

    Returns ``(errors, warnings)``.  Errors are hard failures (a directory
    the registry claims is missing, an uncatalogued directory, or a version
    macro that disagrees with the recorded version).  Warnings are advisory
    (a missing LICENSE file for a component that declares one is an error;
    stb's documented no-LICENSE gap is a warning).
    """
    errors: list[str] = []
    warnings: list[str] = []

    catalogued = _catalogued_top_dirs()
    on_disk = _third_party_dirs()
    errors.extend(
        f"libs/third_party/{extra}: on disk but not in REGISTRY (uncatalogued SOUP)"
        for extra in sorted(on_disk - catalogued)
    )
    errors.extend(
        f"libs/third_party/{missing}: in REGISTRY but not on disk"
        for missing in sorted(catalogued - on_disk)
    )

    for comp in REGISTRY:
        comp_path = REPO_ROOT / comp.path
        if comp.provenance == PROV_NOT_VENDORED:
            if comp_path.exists():
                warnings.append(f"{comp.key}: marked not-vendored but present on disk")
            continue
        if not comp_path.exists():
            errors.append(f"{comp.key}: recorded path '{comp.path}' does not exist")
            continue
        _check_version(comp, errors)
        _check_license_file(comp, errors, warnings)

    return errors, warnings


def _check_version(comp: Component, errors: list[str]) -> None:
    """Append an error if the probed version disagrees with the record."""
    if comp.expected_version is None:
        return
    probed = probe_version(comp)
    if probed is None:
        errors.append(f"{comp.key}: version probe found no version in '{comp.probe_file}'")
    elif probed != comp.expected_version:
        errors.append(
            f"{comp.key}: version drift -- source says {probed}, "
            f"registry says {comp.expected_version}"
        )


def _check_license_file(comp: Component, errors: list[str], warnings: list[str]) -> None:
    """Error on a declared-but-missing LICENSE; warn on the stb gap."""
    if comp.license_file is None:
        if comp.spdx is not None:
            warnings.append(f"{comp.key}: no standalone LICENSE file in-tree (license in headers)")
        return
    if not (REPO_ROOT / comp.license_file).is_file():
        errors.append(f"{comp.key}: declared LICENSE '{comp.license_file}' is missing")


def _licenses_block(comp: Component) -> list[dict] | None:
    """Build the CycloneDX ``licenses`` array for a component."""
    if comp.spdx is not None:
        if " OR " in comp.spdx or " AND " in comp.spdx:
            return [{"expression": comp.spdx}]
        return [{"license": {"id": comp.spdx}}]
    if comp.license_name is not None:
        return [{"license": {"name": comp.license_name}}]
    return None


def _properties_block(comp: Component) -> list[dict]:
    """Build the CycloneDX ``properties`` array for a component."""
    props: list[dict] = [
        {"name": "ra8:provenance", "value": comp.provenance},
        {"name": "ra8:path", "value": comp.path},
    ]
    if comp.upstream_commit is not None:
        props.append({"name": "ra8:upstreamCommit", "value": comp.upstream_commit})
    if comp.license_original is not None:
        props.append({"name": "ra8:licenseOriginal", "value": comp.license_original})
    if comp.license_election is not None:
        props.append({"name": "ra8:licenseElection", "value": comp.license_election})
    if comp.license_file is not None:
        props.append({"name": "ra8:licenseFile", "value": comp.license_file})
    if comp.copyright is not None:
        props.append({"name": "ra8:copyright", "value": comp.copyright})
    props.append({"name": "ra8:modified", "value": "true" if comp.modified else "false"})
    for i, note in enumerate(comp.extra_notes):
        props.append({"name": f"ra8:note{i}", "value": note})
    return props


def component_entry(comp: Component) -> dict:
    """Render one registry `Component` as a CycloneDX component object."""
    entry: dict = {"type": comp.ctype, "bom-ref": comp.key, "name": comp.name}
    if comp.group is not None:
        entry["group"] = comp.group
    entry["version"] = comp.version
    entry["description"] = comp.description
    entry["scope"] = comp.scope
    licenses = _licenses_block(comp)
    if licenses is not None:
        entry["licenses"] = licenses
    if comp.license_note is not None:
        entry["copyright"] = comp.license_note if comp.copyright is None else comp.copyright
    if comp.purl is not None:
        entry["purl"] = comp.purl
    if comp.aggregate_sha256 is not None:
        entry["hashes"] = [{"alg": "SHA-256", "content": comp.aggregate_sha256}]
    entry["externalReferences"] = [{"type": "vcs", "url": comp.url}]
    entry["properties"] = _properties_block(comp)
    return entry


def _serial_number() -> str:
    """Return a content-derived (deterministic) CycloneDX serial number."""
    canonical = "|".join(f"{c.key}={c.version}={c.spdx or c.license_name}" for c in REGISTRY)
    return f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, canonical)}"


def build_bom() -> dict:
    """Assemble the full CycloneDX 1.5 BOM document as an ordered dict."""
    return {
        "bomFormat": BOM_FORMAT,
        "specVersion": CYCLONEDX_SPEC,
        "serialNumber": _serial_number(),
        "version": BOM_REVISION,
        "metadata": {
            "tools": [{"vendor": PROJECT_NAME, "name": GENERATOR_NAME}],
            "component": {
                "type": "application",
                "bom-ref": PROJECT_NAME,
                "name": PROJECT_NAME,
                "version": "unversioned",
                "description": "Renesas RA8D2 (Cortex-M85) bare-metal firmware.",
            },
            "properties": [
                {
                    "name": "ra8:sbomNote",
                    "value": (
                        "Generated by scripts/gen/gen_sbom.py from the "
                        "REGISTRY cross-checked against libs/third_party/. "
                        "Human inventory: THIRD_PARTY_LICENSES.md. Per-"
                        "component qualification: docs/SOUP/."
                    ),
                },
            ],
        },
        "components": [component_entry(c) for c in REGISTRY],
    }


def serialize(bom: dict) -> str:
    """Serialize the BOM deterministically (ASCII, 2-space indent, newline)."""
    return json.dumps(bom, indent=2, ensure_ascii=True) + "\n"


def _print_summary(warnings: list[str], stream: TextIO) -> None:
    """Print a one-line-per-class provenance summary to `stream`."""
    by_prov: dict[str, int] = {}
    for comp in REGISTRY:
        by_prov[comp.provenance] = by_prov.get(comp.provenance, 0) + 1
    print(f"{GENERATOR_NAME}: {len(REGISTRY)} components", file=stream)
    for prov in sorted(by_prov):
        print(f"  {prov:24s} {by_prov[prov]}", file=stream)
    for warn in warnings:
        print(f"  WARN {warn}", file=stream)


def run_write(to_stdout: bool) -> int:
    """Regenerate the SBOM; write it (or print it) and report cross-checks."""
    errors, warnings = cross_check()
    text = serialize(build_bom())
    # In --print mode stdout must stay pure JSON, so the summary goes to stderr.
    summary_stream = sys.stderr if to_stdout else sys.stdout
    if to_stdout:
        sys.stdout.write(text)
    else:
        out = REPO_ROOT / SBOM_REL_PATH
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"{GENERATOR_NAME}: wrote {SBOM_REL_PATH}")
    _print_summary(warnings, summary_stream)
    if errors:
        for err in errors:
            print(f"  ERROR {err}", file=sys.stderr)
        return EXIT_DRIFT
    return EXIT_OK


def run_check() -> int:
    """Fail if the committed SBOM is stale or the tree drifted from the registry."""
    errors, warnings = cross_check()
    expected = serialize(build_bom())
    out = REPO_ROOT / SBOM_REL_PATH
    if not out.is_file():
        print(
            f"{GENERATOR_NAME}: {SBOM_REL_PATH} is missing; run gen_sbom.py",
            file=sys.stderr,
        )
        return EXIT_DRIFT
    actual = out.read_text(encoding="utf-8")
    if actual != expected:
        print(
            f"{GENERATOR_NAME}: {SBOM_REL_PATH} is stale; run gen_sbom.py to regenerate",
            file=sys.stderr,
        )
        errors = [*errors, "committed SBOM does not match the registry"]
    for warn in warnings:
        print(f"  WARN {warn}")
    if errors:
        for err in errors:
            print(f"  ERROR {err}", file=sys.stderr)
        return EXIT_DRIFT
    print(f"{GENERATOR_NAME}: SBOM matches the tree ({len(REGISTRY)} components).")
    return EXIT_OK


def run_commits() -> int:
    """Print one ``<key> <upstream-commit>`` line per commit-pinned component.

    This is the machine interface behind the weekly OSV CVE scan
    (``scripts/checks/osv_scan.sh``): OSV.dev indexes C/C++ advisories as GIT
    commit ranges queryable only by commit hash (GitHub purls do not
    resolve), so the scan materializes each pinned commit as a stub git
    checkout and lets ``osv-scanner`` issue the exact commit queries.
    Exits nonzero when the registry carries no pins at all, which would
    mean the scan is wired to nothing.
    """
    pinned = [comp for comp in REGISTRY if comp.upstream_commit is not None]
    for comp in pinned:
        print(f"{comp.key} {comp.upstream_commit}")
    if not pinned:
        print(f"{GENERATOR_NAME}: no commit-pinned component in REGISTRY", file=sys.stderr)
        return EXIT_DRIFT
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch to the write / check / print / commits action."""
    parser = argparse.ArgumentParser(description="Generate/validate the ra8-firmware SBOM.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the committed SBOM is stale or the tree drifted",
    )
    parser.add_argument(
        "--print",
        dest="to_stdout",
        action="store_true",
        help="print the SBOM to stdout instead of writing the file",
    )
    parser.add_argument(
        "--commits",
        action="store_true",
        help="print `<key> <upstream-commit>` per commit-pinned component",
    )
    args = parser.parse_args(argv)
    if args.check:
        return run_check()
    if args.commits:
        return run_commits()
    return run_write(args.to_stdout)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

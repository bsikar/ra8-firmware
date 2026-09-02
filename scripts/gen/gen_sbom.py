#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate and validate the ra8-firmware Software Bill of Materials (SBOM).

This is the supply-chain provenance gate for the vendored third-party SOUP
(Software Of Unknown Provenance) under the platform and application vendor
roots, plus any registry-backed ``tools/<tool>/third_party/<component>`` and the one
bundled font data asset under ``libs/ra8_fonts/``.  It emits a machine-readable
CycloneDX 1.5 JSON document at ``docs/sbom/ra8-firmware.cdx.json`` that
records, for every component: name, version, SPDX license (with the
Apache-2.0 election for the dual-licensed crypto), package URL (purl) where
one is meaningful, in-tree path, upstream URL, and provenance class.

The curated ``REGISTRY`` in the sibling module ``sbom_registry.py`` is the
single source of truth for the fields that cannot be derived mechanically
(license election, upstream URL, purl, provenance); this module is the logic
that renders and validates it.  Everything that CAN be cross-checked against
the tree is:

  * **Directory drift** -- every direct child of a supported vendor root must
    have a registry entry, and every registry directory must exist on disk.
    A newly vendored component with no entry fails the gate.
  * **Version drift** -- for components whose in-tree headers carry a version
    macro (the ThreadX family, Mbed TLS, TF-PSA-Crypto, miniz,
    stb), the macro is re-read from source and compared to the recorded
    version.  Versions are never invented; a component with no upstream
    release tag (litehtml, NimBLE dev snapshots) is pinned to the exact
    upstream commit its vendored tree is byte-identical to (T5-09).
  * **License-file presence** -- each entry that names a LICENSE file must
    have it on disk.  stb ships no standalone LICENSE (text in header tails)
    and is reported as a known gap rather than a hard failure.
  * **Content integrity** -- every vendored component carries a SHA-256
    ``aggregate`` digest over its whole tree, RE-DERIVED from disk on every
    run (see ``tree_digest``).  A single mutated vendored byte changes the
    digest, so ``--check`` fails.

That last check is *self*-referential by nature: it proves the tree has not
changed since the SBOM was regenerated, never that the tree was right when it
was vendored.  The complementary check lives in
``scripts/checks/check_soup_upstream.py`` (#548), which compares every vendored
file against the blob hash its upstream project publishes for the pinned
revision.  The two are deliberately separate: this one needs no network and
covers every byte under a component path, that one needs a fetch (done weekly)
and covers the identity claim the digest cannot reach.

That last one used to be the hole.  ``aggregate_sha256`` was a hand-transcribed
literal in ``sbom_registry.py``, present on four of twenty-three components and
absent from NimBLE -- the one component that had actually drifted.  Nothing
ever computed it, so ``--check``'s byte-comparison of regenerated-against-
committed JSON compared a constant with itself, and appending a line to a
vendored source still printed ``SBOM matches the tree`` with status 0 (#538).
Provenance is now DERIVED, never transcribed: a value re-computed from the tree
on each run is the only kind that can disagree with the tree.

The emitted JSON is deterministic (no wall-clock timestamp, content-derived
serial number, ``ensure_ascii``) so ``--check`` can compare it byte-for-byte
against the committed file and so the SBOM is reproducible.

Run::

    gen_sbom.py             # regenerate the committed SBOM + print a summary
    gen_sbom.py --check     # fail if the committed SBOM is stale, the tree
                            #   drifted from the registry, or a vendored file
                            #   changed (the CI/hook gate)
    gen_sbom.py --print     # write nothing; print the SBOM JSON to stdout
    gen_sbom.py --commits   # print `<key> <upstream-commit>` per pinned
                            #   component (consumed by the weekly OSV scan)
    gen_sbom.py --selftest  # prove the digest detects a mutation AND stays
                            #   stable on an unchanged tree, then exit

Exit 0 if clean, 1 on drift / a catalogued-tree mismatch (including a version
macro that no longer parses), 2 when the enumeration itself collapsed and no
verdict is possible.  argparse exits 2 on a usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import isolated_git_environment, trusted_git_executable
from sbom_registry import (
    PROV_NOT_VENDORED,
    REGISTRY,
    Component,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXED_VENDOR_ROOTS = (
    Path("libs/third_party"),
    Path("apps/shared_libs/third_party"),
)
SBOM_REL_PATH = Path("docs/sbom/ra8-firmware.cdx.json")

PROJECT_NAME = "ra8-firmware"
BOM_FORMAT = "CycloneDX"
CYCLONEDX_SPEC = "1.5"
BOM_REVISION = 1
GENERATOR_NAME = "gen_sbom.py"

DIGEST_ALG = "SHA-256"
GIT_MODE_SYMLINK = "120000"
TOOL_VENDOR_ROOT_PARTS = 3

# Vacuity floors.  A digest over an empty file list is a perfectly stable
# hash of nothing, and would report a component as verified when its
# enumeration had collapsed -- the same shape as every other finding under the
# gate-honesty epic.  Both floors are MEASURED, not round: 9738 file records
# across the 22 vendored components on 2026-07-28 (nimble 827, threadx 4758,
# ... fonts/Literata 1).  The total floor is set well below that so ordinary
# vendored churn does not trip it, but far above any plausible collapse.
COMPONENT_FILE_FLOOR = 1
TOTAL_FILE_FLOOR = 5000

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_VACUOUS = 2


class VacuousScanError(Exception):
    """Raised when an enumeration collapsed and no honest verdict is possible."""


def _git_ls_files(rel_path: str, root: Path = REPO_ROOT) -> list[tuple[str, str]]:
    """Return ``(git mode, repo-relative path)`` for the worktree under `rel_path`.

    Git supplies tracked plus untracked/non-ignored path names rather than a
    filesystem walk, so build output and ignored scratch files cannot perturb a
    provenance hash. Deleted index entries are dropped; current worktree modes
    are recorded so unstaged moves, additions, deletions, and chmod changes are
    all visible to the gate.

    Args:
        rel_path: Repo-relative path of a component (a directory or one file).
        root: Repository worktree to enumerate; defaults to this checkout.

    Returns:
        ``(mode, path)`` pairs, unsorted; ``digest_entries`` imposes the order.

    Raises:
        VacuousScanError: When ``git ls-files`` cannot run at all.
    """
    proc = subprocess.run(  # noqa: S603  # trusted: fixed git argv, no shell
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            rel_path,
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        message = f"`git ls-files -- {rel_path}` failed ({proc.returncode}): {proc.stderr.strip()}"
        raise VacuousScanError(message)
    entries: list[tuple[str, str]] = []
    for record in proc.stdout.split("\0"):
        if not record:
            continue
        path = record
        source = root / path
        if not source.exists() and not source.is_symlink():
            continue
        if source.is_symlink():
            mode = GIT_MODE_SYMLINK
        else:
            mode = "100755" if source.stat().st_mode & stat.S_IXUSR else "100644"
        entries.append((mode, path))
    return entries


def digest_entries(base: Path, entries: list[tuple[str, str]], strip: str) -> str:
    """Hash `entries` into one SHA-256 over path, mode and content.

    Every field is length-framed before it is fed to the hash, so no rename can
    be made to collide with a content edit by moving bytes across the boundary
    between the two.  Paths are made component-relative (``strip`` is removed)
    so relocating a vendored tree wholesale is not reported as a modification,
    while renaming a file *inside* it is.

    Args:
        base: Directory the paths in `entries` are resolved against.
        entries: ``(git mode, path)`` pairs from `_git_ls_files`.
        strip: Path prefix to remove, making each path component-relative.

    Returns:
        The lower-case hex SHA-256 digest.

    Raises:
        VacuousScanError: When `entries` is empty -- a digest of nothing is stable
            and meaningless, and must never render as a verified component.
    """
    if len(entries) < COMPONENT_FILE_FLOOR:
        message = f"'{strip}' enumerated 0 tracked files, floor is {COMPONENT_FILE_FLOOR}"
        raise VacuousScanError(message)
    prefix = strip.rstrip("/") + "/"
    hasher = hashlib.sha256()
    for mode, path in sorted(entries, key=lambda item: item[1]):
        inner = path[len(prefix) :] if path.startswith(prefix) else Path(path).name
        target = base / path
        payload = (
            str(target.readlink()).encode() if mode == GIT_MODE_SYMLINK else target.read_bytes()
        )
        header = f"{mode} {len(inner)} {inner} {len(payload)}\n".encode()
        hasher.update(header)
        hasher.update(payload)
    return hasher.hexdigest()


_DIGEST_CACHE: dict[str, tuple[str, int]] = {}


def tree_digest(comp: Component) -> tuple[str, int]:
    """Re-derive `comp`'s integrity digest and tracked-file count from the tree.

    Args:
        comp: The registry component to hash.

    Returns:
        ``(hex digest, file count)``.

    Raises:
        VacuousScanError: When the component enumerates no tracked file.
    """
    if comp.path not in _DIGEST_CACHE:
        entries = _git_ls_files(comp.path)
        _DIGEST_CACHE[comp.path] = (digest_entries(REPO_ROOT, entries, comp.path), len(entries))
    return _DIGEST_CACHE[comp.path]


def hashed_components() -> tuple[Component, ...]:
    """Return the registry entries that must carry a derived integrity digest.

    Everything vendored qualifies.  ``PROV_NOT_VENDORED`` is the single
    exclusion and it is self-proving: `cross_check` already errors when such a
    path exists on disk, so the class cannot be used to hide a real tree.
    """
    return tuple(comp for comp in REGISTRY if comp.provenance != PROV_NOT_VENDORED)


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


def _vendor_root_for(path: Path) -> Path | None:
    """Return the supported vendor root containing ``path``, if any.

    Tool SOUP is permitted only at ``tools/<tool>/third_party/<component>``.
    This keeps ownership local without creating a repository-wide tools vendor
    bucket, while allowing the registry to grow if a truly tool-exclusive
    dependency is introduced later.
    """
    parts = path.parts
    if parts[:2] == ("libs", "third_party"):
        return Path(*parts[:2])
    if parts[:3] == ("apps", "shared_libs", "third_party"):
        return Path(*parts[:3])
    if len(parts) >= TOOL_VENDOR_ROOT_PARTS and parts[0] == "tools" and parts[2] == "third_party":
        return Path(*parts[:3])
    return None


def _vendor_roots() -> tuple[Path, ...]:
    """Discover fixed, registry-declared, and on-disk tool-private roots."""
    roots = set(FIXED_VENDOR_ROOTS)
    tools = REPO_ROOT / "tools"
    if tools.is_dir():
        roots.update(
            Path("tools") / tool.name / "third_party"
            for tool in tools.iterdir()
            if tool.is_dir() and (tool / "third_party").is_dir()
        )
    for comp in REGISTRY:
        root = _vendor_root_for(Path(comp.path))
        if root is not None:
            roots.add(root)
    return tuple(sorted(roots, key=lambda path: path.as_posix()))


def _third_party_dirs() -> set[str]:
    """Return repo-relative direct-child paths under every supported vendor root."""
    found: set[str] = set()
    for rel_root in _vendor_roots():
        base = REPO_ROOT / rel_root
        if base.is_dir():
            found.update(
                (rel_root / path.name).as_posix() for path in base.iterdir() if path.is_dir()
            )
    return found


def _catalogued_top_dirs() -> set[str]:
    """Return direct-child paths of supported vendor roots covered by the registry.

    A nested path such as ``esp-hosted/common/protobuf-c`` catalogues the
    ``esp-hosted`` top-level directory, so a tree carrying a separately
    pinned sub-component does not read as uncatalogued.
    """
    dirs: set[str] = set()
    for comp in REGISTRY:
        parts = Path(comp.path).parts
        for rel_root in _vendor_roots():
            prefix = rel_root.parts
            if parts[: len(prefix)] == prefix and len(parts) > len(prefix):
                dirs.add((rel_root / parts[len(prefix)]).as_posix())
                break
    return dirs


def _directory_drift(on_disk: set[str], catalogued: set[str]) -> list[str]:
    """Return both directions of vendor-root/registry drift."""
    errors = [
        f"{extra}: on disk but not in REGISTRY (uncatalogued SOUP)"
        for extra in sorted(on_disk - catalogued)
    ]
    errors.extend(
        f"{missing}: in REGISTRY but not on disk" for missing in sorted(catalogued - on_disk)
    )
    return errors


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
    errors.extend(_directory_drift(on_disk, catalogued))

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

    _check_scan_not_vacuous(errors)
    return errors, warnings


def _check_scan_not_vacuous(errors: list[str]) -> None:
    """Append an error if the vendored enumeration collapsed below its floor.

    Every component is individually floored inside `digest_entries`; this is
    the aggregate trip-wire that catches a whole-tree collapse (a bad cwd, a
    git that ran but returned nothing) before it can render as 23 verified
    components.

    Args:
        errors: Error list to append to, in place.
    """
    total = 0
    for comp in hashed_components():
        try:
            total += tree_digest(comp)[1]
        except VacuousScanError as exc:  # one bad component must not stop the sweep
            errors.append(f"{comp.key}: {exc}")
    if total and total < TOTAL_FILE_FLOOR:
        errors.append(
            f"only {total} vendored file(s) enumerated across the registry, floor is "
            f"{TOTAL_FILE_FLOOR}. A collapsed enumeration reports every component "
            "verified because it hashed nothing."
        )


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


def _properties_block(comp: Component, file_count: int) -> list[dict]:
    """Build the CycloneDX ``properties`` array for a component.

    ``ra8:fileCount`` is published alongside the digest deliberately: a digest
    alone is opaque, so a collapsed enumeration would change it without saying
    why.  The count makes the size of the hashed set visible in the committed
    SBOM and therefore in every diff.

    Args:
        comp: The registry component being rendered.
        file_count: Tracked files that went into `comp`'s digest.

    Returns:
        The CycloneDX property objects, in stable order.
    """
    props: list[dict] = [
        {"name": "ra8:provenance", "value": comp.provenance},
        {"name": "ra8:path", "value": comp.path},
    ]
    if comp.provenance != PROV_NOT_VENDORED:
        props.append({"name": "ra8:fileCount", "value": str(file_count)})
    if comp.upstream_commit is not None:
        props.append({"name": "ra8:upstreamCommit", "value": comp.upstream_commit})
    if comp.upstream_ref is not None:
        props.append({"name": "ra8:upstreamRef", "value": comp.upstream_ref})
    if comp.upstream_archive_sha256 is not None:
        props.append({"name": "ra8:upstreamArchiveSha256", "value": comp.upstream_archive_sha256})
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
    if comp.provenance != PROV_NOT_VENDORED:
        digest, count = tree_digest(comp)
        entry["hashes"] = [{"alg": DIGEST_ALG, "content": digest}]
    else:
        count = 0
    entry["externalReferences"] = [{"type": "vcs", "url": comp.url}]
    entry["properties"] = _properties_block(comp, count)
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
                        "REGISTRY cross-checked against every supported third_party root. "
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


def _committed_digests(text: str) -> dict[str, str]:
    """Extract ``bom-ref -> SHA-256 content`` from a committed SBOM document.

    Args:
        text: The committed SBOM JSON.

    Returns:
        One entry per component that publishes a SHA-256 hash; components
        without one are absent from the mapping.
    """
    try:
        doc = json.loads(text)
    except json.JSONDecodeError:
        return {}
    out: dict[str, str] = {}
    for entry in doc.get("components", []):
        for item in entry.get("hashes", []):
            if item.get("alg") == DIGEST_ALG:
                out[entry.get("bom-ref", "")] = item.get("content", "")
    return out


def _integrity_errors(committed: str) -> list[str]:
    """Name every component whose vendored tree no longer hashes to its record.

    This is the message that matters.  A bare "the SBOM is stale, regenerate
    it" would invite exactly the wrong reflex -- regenerating adopts the
    mutation and the provenance claim is quietly relaxed.  Naming the component
    and the two digests says what actually happened: a file under a vendored
    SOUP tree changed, and either the change is illegitimate or the component's
    ``docs/SOUP/`` "Modifications" section owes an entry.

    Args:
        committed: Text of the committed SBOM document.

    Returns:
        Human-readable error strings, one per drifted component.
    """
    recorded = _committed_digests(committed)
    errors: list[str] = []
    for comp in hashed_components():
        was = recorded.get(comp.key)
        if was is None:
            continue
        try:
            now, count = tree_digest(comp)
        except VacuousScanError:
            continue  # already reported by _check_scan_not_vacuous
        if now != was:
            errors.append(
                f"{comp.key}: VENDORED TREE DRIFT -- {count} file(s) under '{comp.path}' "
                f"now hash to {now}, the committed SBOM records {was}. A vendored SOUP "
                "tree changed. Do not just regenerate: confirm the change is intended, "
                f"and record it in docs/SOUP/ as a modification of the upstream pin."
            )
    return errors


def run_check() -> int:
    """Fail if the committed SBOM is stale or the tree drifted from the registry."""
    errors, warnings = cross_check()
    out = REPO_ROOT / SBOM_REL_PATH
    if not out.is_file():
        print(
            f"{GENERATOR_NAME}: {SBOM_REL_PATH} is missing; run gen_sbom.py",
            file=sys.stderr,
        )
        return EXIT_DRIFT
    actual = out.read_text(encoding="utf-8")
    integrity = _integrity_errors(actual)
    errors = [*errors, *integrity]
    if actual != serialize(build_bom()) and not integrity:
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
    hashed = len(hashed_components())
    files = sum(tree_digest(comp)[1] for comp in hashed_components())
    print(
        f"{GENERATOR_NAME}: SBOM matches the tree ({len(REGISTRY)} components; "
        f"{hashed} SHA-256 digests re-derived over {files} vendored files)."
    )
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


def _selftest_tree(root: Path) -> list[tuple[str, str]]:
    """Materialise a small fixture tree under `root` and return its entries.

    Args:
        root: Directory to create the fixture under.

    Returns:
        ``(mode, path)`` pairs in the shape `_git_ls_files` produces.
    """
    (root / "vendor" / "src").mkdir(parents=True)
    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n")
    (root / "vendor" / "src" / "b.c").write_bytes(b"int b;\n")
    (root / "vendor" / "LICENSE").write_bytes(b"MIT\n")
    return [
        ("100644", "vendor/src/a.c"),
        ("100644", "vendor/src/b.c"),
        ("100644", "vendor/LICENSE"),
    ]


def _selftest_worktree_cases(root: Path) -> list[tuple[str, bool]]:
    """Prove the SBOM census observes unstaged vendor-tree state both ways."""
    repo = root / "repo"
    vendor = repo / "vendor"
    vendor.mkdir(parents=True)
    tracked = vendor / "tracked.c"
    removed = vendor / "removed.c"
    tracked.write_bytes(b"int tracked;\n")
    removed.write_bytes(b"int removed;\n")
    (repo / ".gitignore").write_text("vendor/ignored.c\n", encoding="ascii")
    subprocess.run(  # noqa: S603 -- fixed Git authority and fixture-only argv
        [trusted_git_executable(), "init", "-q", "-b", "main", "."],
        cwd=repo,
        check=True,
    )
    subprocess.run(  # noqa: S603 -- fixed Git authority and fixture-only argv
        [trusted_git_executable(), "add", "-A"],
        cwd=repo,
        check=True,
    )
    original_digest = digest_entries(repo, _git_ls_files("vendor", repo), "vendor")

    tracked.write_bytes(b"int changed;\n")
    tracked.chmod(0o755)
    removed.unlink()
    (vendor / "untracked.c").write_bytes(b"int untracked;\n")
    (vendor / "ignored.c").write_bytes(b"int ignored;\n")
    entries = _git_ls_files("vendor", repo)
    paths = {path for _mode, path in entries}
    return [
        (
            "MUST FIRE: unstaged bytes and mode change the SBOM worktree digest",
            digest_entries(repo, entries, "vendor") != original_digest
            and ("100755", "vendor/tracked.c") in entries,
        ),
        (
            "MUST FIRE: an untracked vendor file enters the SBOM census",
            "vendor/untracked.c" in paths,
        ),
        (
            "MUST FIRE: a deleted tracked vendor file leaves the SBOM census",
            "vendor/removed.c" not in paths,
        ),
        (
            "MUST NOT FIRE: an ignored vendor file stays outside the SBOM census",
            "vendor/ignored.c" not in paths,
        ),
    ]


def _selftest_shape_cases(
    root: Path, entries: list[tuple[str, str]], base: str
) -> list[tuple[str, bool]]:
    """Assert that changing the SHAPE of the file set changes the digest.

    Content mutation is covered by the caller; these are the cases a naive
    "hash the concatenated bytes" digest would miss -- an added or removed
    file, and a mode change.

    Args:
        root: The fixture root from `_selftest_tree`.
        entries: That fixture's entry list, content-restored.
        base: The digest of the unmodified fixture.

    Returns:
        One ``(label, passed)`` pair per assertion.
    """
    (root / "vendor" / "src" / "c.c").write_bytes(b"int a;\n")
    vacuous = False
    try:
        digest_entries(root, [], "vendor")
    except VacuousScanError:
        vacuous = True
    return [
        (
            "MUST FIRE: an added file changes the digest",
            digest_entries(root, [*entries, ("100644", "vendor/src/c.c")], "vendor") != base,
        ),
        (
            "MUST FIRE: a removed file changes the digest",
            digest_entries(root, entries[:-1], "vendor") != base,
        ),
        (
            "MUST FIRE: a mode change changes the digest",
            digest_entries(root, [("100755", entries[0][1]), *entries[1:]], "vendor") != base,
        ),
        ("MUST FIRE: an empty enumeration raises rather than hashing nothing", vacuous),
    ]


def _selftest_registry_cases() -> list[tuple[str, bool]]:
    """Return registry/tree ownership assertions for every supported vendor shape."""
    return [
        (
            "MUST FIRE: the live registry publishes a digest for every vendored component",
            len(hashed_components()) == len(REGISTRY) - 1,
        ),
        (
            "MUST NOT FIRE: matching entries across all vendor-root shapes stay quiet",
            not _directory_drift(
                {
                    "libs/third_party/platform",
                    "apps/shared_libs/third_party/app",
                    "tools/viewer/third_party/tool_only",
                },
                {
                    "libs/third_party/platform",
                    "apps/shared_libs/third_party/app",
                    "tools/viewer/third_party/tool_only",
                },
            ),
        ),
        (
            "MUST FIRE: an uncatalogued app vendor is detected",
            bool(
                _directory_drift(
                    {"libs/third_party/platform", "apps/shared_libs/third_party/extra"},
                    {"libs/third_party/platform"},
                )
            ),
        ),
        (
            "MUST FIRE: a missing app vendor is detected",
            bool(
                _directory_drift(
                    {"libs/third_party/platform"},
                    {"libs/third_party/platform", "apps/shared_libs/third_party/app"},
                )
            ),
        ),
        (
            "MUST NOT FIRE: the narrow tool-private vendor shape is supported",
            _vendor_root_for(Path("tools/viewer/third_party/decoder"))
            == Path("tools/viewer/third_party"),
        ),
        (
            "MUST FIRE: a repository-wide tools vendor bucket is unsupported",
            _vendor_root_for(Path("tools/third_party/decoder")) is None,
        ),
    ]


def _selftest_cases(root: Path, entries: list[tuple[str, str]]) -> list[tuple[str, bool]]:
    """Run every digest assertion against the fixture and return ``(label, ok)``.

    Args:
        root: The fixture root from `_selftest_tree`.
        entries: That fixture's entry list.

    Returns:
        One ``(label, passed)`` pair per assertion, both directions covered.
    """
    base = digest_entries(root, entries, "vendor")
    cases: list[tuple[str, bool]] = [
        (
            "MUST NOT FIRE: an unchanged tree hashes identically",
            digest_entries(root, entries, "vendor") == base,
        ),
        (
            "MUST NOT FIRE: enumeration order does not change the digest",
            digest_entries(root, list(reversed(entries)), "vendor") == base,
        ),
    ]

    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n/* injected */\n")
    cases.append(
        (
            "MUST FIRE: one mutated vendored byte changes the digest",
            digest_entries(root, entries, "vendor") != base,
        )
    )
    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n")
    cases.append(
        (
            "MUST NOT FIRE: restoring the byte restores the digest",
            digest_entries(root, entries, "vendor") == base,
        )
    )

    cases.extend(_selftest_shape_cases(root, entries, base))

    cases.extend(_selftest_registry_cases())
    return cases


def _run_selftest_body() -> int:
    """Prove the integrity digest fires on a mutation and stays quiet otherwise.

    Both directions are asserted because only one of them was ever true before:
    the old hardcoded ``aggregate_sha256`` was perfectly stable on an unchanged
    tree and equally stable on a mutated one.  A selftest that checked only the
    quiet direction would have passed against the broken code (#538).

    Returns:
        ``EXIT_OK`` when every case holds, ``EXIT_VACUOUS`` otherwise.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cases = _selftest_cases(root, _selftest_tree(root))
        cases.extend(_selftest_worktree_cases(root))
    failed = [label for label, ok in cases if not ok]
    for label, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    if failed:
        print(f"{GENERATOR_NAME}: selftest FAILED ({len(failed)} case(s))", file=sys.stderr)
        return EXIT_VACUOUS
    print(f"{GENERATOR_NAME}: selftest passed ({len(cases)} cases, both directions).")
    return EXIT_OK


def run_selftest() -> int:
    """Run SBOM worktree fixtures without inheriting the caller's repository."""
    with isolated_git_environment():
        return _run_selftest_body()


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch to the write / check / print / commits action."""
    parser = argparse.ArgumentParser(description="Generate/validate the ra8-firmware SBOM.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the committed SBOM is stale or the tree drifted",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the integrity digest detects a mutation, then exit",
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
    if args.selftest:
        return run_selftest()
    try:
        if args.check:
            return run_check()
        if args.commits:
            return run_commits()
        return run_write(args.to_stdout)
    except VacuousScanError as exc:
        print(f"{GENERATOR_NAME}: FATAL -- {exc}", file=sys.stderr)
        return EXIT_VACUOUS


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

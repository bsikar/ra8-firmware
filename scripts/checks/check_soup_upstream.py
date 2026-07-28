#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every vendored SOUP file is the file its upstream project published.

``docs/SOUP/*.md``, ``THIRD_PARTY_LICENSES.md`` and the SBOM's
``commit-pinned-sha256`` provenance class all assert the same strong claim --
the vendored tree is byte-identical to a named upstream revision -- and until
this gate nothing checked it.  ``gen_sbom.py``'s digest (#538) proves only that
the tree has not changed since the SBOM was last regenerated: a tree that was
already wrong at vendor-in hashes faithfully and reports clean forever.

How upstream identity is established
------------------------------------
Each component pins an upstream revision in ``scripts/gen/sbom_registry.py``.
``--refresh`` fetches that revision **from the upstream project** and writes
what upstream publishes for every file we vendor into
``docs/sbom/upstream/<key>.manifest``.  For a git upstream that is
``git ls-tree -r``: a ``--filter=blob:none`` fetch brings the tree objects
without any file content, and the blob SHA-1s in them are already content
hashes.  For miniz -- whose single-file amalgamation exists only as a release
zip, never in the upstream git tree -- it is the pinned, SHA-256-verified
release artifact instead.

``--check`` then runs offline, comparing the blob ids git records for our
tracked files against those manifests.  Two independently produced hashes, so
no constant is ever compared with itself; and the gate needs no network, so a
push does not depend on twenty upstream hosts being reachable.  ``--refresh``
is re-run by the weekly ``soup-upstream-refresh`` gate to catch what the
offline half structurally cannot: an upstream tag that moved, a rewritten
history, or a project that vanished.

Deliberate deviations are DECLARED, never inferred
--------------------------------------------------
Vendored SOUP is sometimes patched on purpose (libwebp's arena allocator,
TinyXML-2's #151 whitespace fix, stb's bounds hardening) and sometimes carries
files upstream has none of (mbedtls' build-generated config-check headers).
Those files must be listed in the registry's ``patched_files`` /
``local_files`` with a justification; ``--refresh`` REFUSES to write a
``patch``/``local`` record for a file the registry has not declared.  That
refusal is what keeps the manifest honest: without it, a corrupted file would
be silently re-recorded as "modified on purpose" on the next refresh, and the
gate would go green having absorbed the corruption.

Run::

    check_soup_upstream.py                # offline: tree vs committed manifests
    check_soup_upstream.py --refresh      # NETWORK: fetch upstream, rewrite them
    check_soup_upstream.py --verify-upstream   # NETWORK: refetch, compare, write nothing
    check_soup_upstream.py --selftest     # prove it fires and stays quiet

Exit 0 clean, 1 on a provenance failure, 2 when the scan itself collapsed
(a missing manifest set, a floor breach) and no honest verdict is possible.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))

from sbom_registry import (
    PROV_NOT_VENDORED,
    REGISTRY,
    UPSTREAM_ARCHIVE,
    Component,
)
from soup_manifest import (
    KIND_LOCAL,
    KIND_MOVED,
    KIND_OK,
    KIND_PATCH,
    REPO_ROOT,
    Entry,
    ManifestError,
    format_manifest,
    git_ls_files,
    manifest_path,
    parse_manifest,
)

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_VACUOUS = 2

# Vacuity floors.  A manifest set that silently covered zero components would
# pronounce all of them clean, and a manifest made only of `patch`/`local` rows
# would prove nothing about upstream at all -- it would record our opinion of
# our own tree, which is exactly the defect this gate exists to remove.  All
# three are MEASURED against the live tree on 2026-07-28 -- 22 components,
# 9735 vendored files, 9715 of them byte-identical to their pinned upstream
# revision -- and set below the measurement by enough slack that ordinary
# re-vendoring does not trip them, but far above any plausible collapse.
MIN_COMPONENTS = 20
MIN_ENTRIES = 9000
MIN_UPSTREAM_VERIFIED = 8900

GIT_TIMEOUT_S = 900
FETCH_TIMEOUT_S = 300


class VacuousScanError(Exception):
    """Raised when the enumeration collapsed and no honest verdict is possible."""


def vendored_components() -> tuple[Component, ...]:
    """Return every registry entry that is actually vendored in this tree."""
    return tuple(comp for comp in REGISTRY if comp.provenance != PROV_NOT_VENDORED)


def blob_id(data: bytes) -> str:
    """Return the git blob SHA-1 of `data`.

    Computed here rather than shelled out to ``git hash-object`` on purpose:
    that command applies the repository's ``.gitattributes``, which would
    line-ending-normalise an archive member and yield a hash that matches
    nothing.  A vendored blob is raw bytes (``libs/third_party/** -text``), so
    the raw framing is the correct one.

    Args:
        data: The file's exact bytes.

    Returns:
        Lower-case hex SHA-1.
    """
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()  # noqa: S324


# --------------------------------------------------------------------------- #
# Offline verification -- the per-push gate.                                   #
# --------------------------------------------------------------------------- #


def _entry_errors(comp: Component, entry: Entry, ours: tuple[str, str]) -> list[str]:
    """Return every way `entry` disagrees with what our index holds.

    Args:
        comp: The component being checked.
        entry: The manifest record for one file.
        ours: ``(mode, blob)`` git records for that file.

    Returns:
        Human-readable error strings; empty when the file is as declared.
    """
    mode, blob = ours
    where = f"{comp.key}: {comp.path}/{entry.rel_path}"
    errors: list[str] = []
    if mode != entry.mode:
        errors.append(f"{where}: mode {mode}, upstream manifest records {entry.mode}")
    declared_patch = dict(comp.patched_files)
    declared_local = dict(comp.local_files)
    if entry.kind in (KIND_OK, KIND_MOVED):
        if blob != entry.upstream_blob:
            source = entry.upstream_path or entry.rel_path
            errors.append(
                f"{where}: NOT the upstream file. Ours hashes to {blob}; "
                f"{comp.upstream_ref or comp.upstream_commit}:{source} is {entry.upstream_blob}. "
                "Either restore the upstream bytes, or declare the change in "
                "sbom_registry.patched_files and record it in docs/SOUP/."
            )
        if entry.rel_path in declared_patch or entry.rel_path in declared_local:
            errors.append(
                f"{where}: the registry declares a deviation for this file, but it is recorded "
                "as byte-identical to upstream. The declaration is stale -- drop it, here and "
                "in the component's docs/SOUP/ 'Deviations / patches' section."
            )
    elif entry.kind == KIND_PATCH:
        if entry.rel_path not in declared_patch:
            errors.append(f"{where}: manifest says 'patch' but the registry declares no patch")
        elif not comp.modified:
            errors.append(
                f"{where}: declared as patched while the component records modified=False"
            )
        if blob != entry.local_blob:
            errors.append(
                f"{where}: patched file changed. Ours hashes to {blob}; the reviewed "
                f"patch is {entry.local_blob}. A patched file is still pinned -- an "
                "edit on top of it needs a refresh and a docs/SOUP/ update."
            )
    elif entry.rel_path not in declared_local:
        errors.append(f"{where}: manifest says 'local' but the registry declares no such file")
    elif blob != entry.local_blob:
        errors.append(
            f"{where}: local file changed. Ours hashes to {blob}, manifest {entry.local_blob}"
        )
    return errors


def _component_errors(comp: Component, root: Path) -> tuple[list[str], int, int]:
    """Verify one component against its committed manifest.

    Args:
        comp: The component to verify.
        root: Repository root to verify inside.

    Returns:
        ``(errors, entry count, upstream-verified count)``.

    Raises:
        VacuousScanError: When the manifest is missing, unparseable, or empty.
    """
    path = root / manifest_path(comp.key)
    if not path.is_file():
        message = (
            f"{comp.key}: no upstream manifest at {manifest_path(comp.key)}. "
            "Run check_soup_upstream.py --refresh (needs the network)."
        )
        raise VacuousScanError(message)
    manifest = parse_manifest(comp.key, path.read_text(encoding="utf-8"), manifest_path(comp.key))
    if not manifest.entries:
        message = f"{comp.key}: manifest records zero files"
        raise VacuousScanError(message)

    errors: list[str] = []
    recorded = manifest.by_path()
    ours = git_ls_files(comp.path, comp.nested_paths, root)
    errors.extend(
        f"{comp.key}: {comp.path}/{rel_path} is in the upstream manifest but not in the "
        "tree. The vendored subset lost a file."
        for rel_path in sorted(set(recorded) - set(ours))
    )
    errors.extend(
        f"{comp.key}: {comp.path}/{rel_path} is tracked but absent from the upstream "
        "manifest. A file appeared inside a vendored SOUP tree."
        for rel_path in sorted(set(ours) - set(recorded))
    )
    for rel_path in sorted(set(ours) & set(recorded)):
        errors.extend(_entry_errors(comp, recorded[rel_path], ours[rel_path]))

    # The revision the SBOM PUBLISHES and the revision the manifest was
    # VERIFIED against must be the same one, or the SBOM advertises a pin
    # nothing checked -- which is the shape of the whole finding.
    for label, declared, recorded_pin in (
        ("commit", comp.upstream_commit, manifest.header.get("commit")),
        ("archive SHA-256", comp.upstream_archive_sha256, manifest.header.get("archive-sha256")),
    ):
        if declared and recorded_pin != declared:
            errors.append(
                f"{comp.key}: registry pins {label} {declared} but the manifest was generated "
                f"from {recorded_pin}. The published pin and the verified pin must match."
            )
    if not (manifest.header.get("commit") or manifest.header.get("archive-sha256")):
        message = f"{comp.key}: manifest records no upstream revision"
        raise VacuousScanError(message)
    return errors, len(manifest.entries), manifest.verified_count()


def run_check(
    comps: tuple[Component, ...] | None = None,
    root: Path = REPO_ROOT,
    floors: tuple[int, int, int] = (MIN_COMPONENTS, MIN_ENTRIES, MIN_UPSTREAM_VERIFIED),
) -> int:
    """Verify every vendored component against its committed manifest, offline.

    Args:
        comps: Components to verify; defaults to every vendored registry entry.
        root: Repository root to verify inside.
        floors: ``(components, entries, upstream-verified)`` vacuity floors.
            Defaulted to the measured tree-wide values, so the CI path uses
            exactly the constants above; the selftest supplies fixture-sized
            ones and asserts the real constants separately.

    Returns:
        Process exit status.
    """
    comps = vendored_components() if comps is None else comps
    errors: list[str] = []
    entries = verified = 0
    try:
        for comp in comps:
            comp_errors, n_entries, n_verified = _component_errors(comp, root)
            errors.extend(comp_errors)
            entries += n_entries
            verified += n_verified
        _check_floors(len(comps), entries, verified, floors)
    except (VacuousScanError, ManifestError) as exc:
        print(f"check_soup_upstream: FATAL -- {exc}", file=sys.stderr)
        return EXIT_VACUOUS

    known = {manifest_path(c.key) for c in comps}
    stray = sorted(
        p.relative_to(root)
        for p in (root / manifest_path("x")).parent.rglob("*" + manifest_path("x").suffix)
        if p.relative_to(root) not in known
    )
    errors.extend(f"{path}: manifest with no registry component" for path in stray)

    if errors:
        for err in errors:
            print(f"  ERROR {err}", file=sys.stderr)
        print(
            f"check_soup_upstream: {len(errors)} provenance failure(s) across "
            f"{len(comps)} vendored components.",
            file=sys.stderr,
        )
        return EXIT_FAIL
    print(
        f"check_soup_upstream: {len(comps)} vendored components, {entries} files, "
        f"{verified} byte-identical to their pinned upstream revision "
        f"({entries - verified} declared deviations)."
    )
    return EXIT_OK


def _check_floors(
    components: int, entries: int, verified: int, floors: tuple[int, int, int]
) -> None:
    """Raise when the scan covered implausibly little to be believed.

    Args:
        components: Components actually verified.
        entries: Manifest records consumed.
        verified: Records proven against an upstream-published hash.
        floors: ``(min components, min entries, min upstream-verified)``.

    Raises:
        VacuousScanError: When any floor is breached.
    """
    min_components, min_entries, min_verified = floors
    if components < min_components:
        message = f"only {components} components covered, floor is {min_components}"
        raise VacuousScanError(message)
    if entries < min_entries:
        message = f"only {entries} files covered, floor is {min_entries}"
        raise VacuousScanError(message)
    if verified < min_verified:
        message = (
            f"only {verified} files were proven against an upstream hash, floor is "
            f"{min_verified}. A manifest of nothing but declared deviations "
            "records our opinion of our own tree and proves no upstream identity."
        )
        raise VacuousScanError(message)


# --------------------------------------------------------------------------- #
# Upstream fetch -- the network half.                                          #
# --------------------------------------------------------------------------- #


def _run_git(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    """Run a git command, returning the completed process."""
    return subprocess.run(  # noqa: S603  # trusted: fixed git argv, no shell
        ["git", *args],  # noqa: S607 -- trusted: resolved from PATH, fixed argv
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
        timeout=GIT_TIMEOUT_S,
    )


def fetch_git_tree(comp: Component, cache: Path) -> tuple[str, dict[str, tuple[str, str]]]:
    """Fetch the pinned upstream revision and return its full file listing.

    ``--filter=blob:none --depth 1`` brings the commit and its trees but no file
    content: the blob SHA-1s recorded in the trees are already the content
    hashes we need, so the whole verification costs tree metadata only.

    Args:
        comp: Component whose upstream to fetch.
        cache: Directory to hold the bare mirrors.

    Returns:
        ``(resolved commit, {upstream path: (mode, blob)})``.

    Raises:
        VacuousScanError: When the fetch fails or the ref resolves to nothing.
    """
    ref = comp.upstream_ref or comp.upstream_commit
    if not ref:
        message = f"{comp.key}: no upstream_ref and no upstream_commit to fetch"
        raise VacuousScanError(message)
    mirror = cache / comp.key.replace("/", "__")
    if not mirror.exists():
        mirror.mkdir(parents=True)
        _run_git(["init", "-q", "--bare", "."], mirror)
        _run_git(["remote", "add", "origin", comp.upstream_repo or comp.url], mirror)
    proc = _run_git(["fetch", "-q", "--filter=blob:none", "--depth", "1", "origin", ref], mirror)
    if proc.returncode != 0:
        message = (
            f"{comp.key}: fetching {ref} from {comp.upstream_repo or comp.url} failed: "
            f"{proc.stderr.strip()[:300]}"
        )
        raise VacuousScanError(message)
    commit = _run_git(["rev-parse", "FETCH_HEAD^{commit}"], mirror).stdout.strip()
    listing = _run_git(["ls-tree", "-r", commit], mirror).stdout
    tree: dict[str, tuple[str, str]] = {}
    for line in listing.splitlines():
        meta, path = line.split("\t", 1)
        mode, _kind, blob = meta.split()
        tree[path] = (mode, blob)
    if not tree:
        message = f"{comp.key}: upstream {ref} listed zero files"
        raise VacuousScanError(message)
    return commit, tree


def fetch_archive_tree(comp: Component, cache: Path) -> tuple[str, dict[str, tuple[str, str]]]:
    """Download the pinned release artifact and return its member listing.

    The artifact is pinned by SHA-256, so this transport is exactly as strong
    as the git one: the bytes are fixed by a hash recorded in the registry and
    re-verified on every fetch.

    Args:
        comp: Component whose archive to fetch.
        cache: Directory to hold the downloaded artifact.

    Returns:
        ``(archive sha256, {member path: (mode, blob)})``.

    Raises:
        VacuousScanError: On a download failure or a digest mismatch.
    """
    cache.mkdir(parents=True, exist_ok=True)
    local = cache / Path(comp.upstream_archive_url or "").name
    if not local.is_file():
        try:
            with urlopen(comp.upstream_archive_url, timeout=FETCH_TIMEOUT_S) as src:  # noqa: S310
                local.write_bytes(src.read())
        except OSError as exc:
            message = f"{comp.key}: downloading {comp.upstream_archive_url} failed: {exc}"
            raise VacuousScanError(message) from exc
    data = local.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != comp.upstream_archive_sha256:
        message = (
            f"{comp.key}: {comp.upstream_archive_url} hashes to {got}, registry pins "
            f"{comp.upstream_archive_sha256}. The release artifact was replaced."
        )
        raise VacuousScanError(message)
    prefix = comp.upstream_archive_prefix
    tree: dict[str, tuple[str, str]] = {}
    with zipfile.ZipFile(local) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename
            if prefix and not name.startswith(prefix):
                continue
            tree[name[len(prefix) :]] = ("100644", blob_id(archive.read(info)))
    if not tree:
        message = f"{comp.key}: archive contained no member under '{prefix}'"
        raise VacuousScanError(message)
    return got, tree


def _resolve_entry(
    comp: Component, rel_path: str, ours: tuple[str, str], tree: dict[str, tuple[str, str]]
) -> Entry:
    """Classify one vendored file against the upstream listing.

    Declarations are consulted FIRST, and each is checked against upstream
    rather than trusted.  A declaration that has stopped describing the file --
    a patch someone reverted, a "local" file upstream has since published --
    is a claim nothing would otherwise notice, which is the failure mode this
    whole gate exists to remove.  Undeclared files then resolve to the same
    relative path, or to a content-identical file elsewhere upstream (a
    flattened vendor); anything else is refused rather than recorded as an
    intentional patch.

    Args:
        comp: The component being refreshed.
        rel_path: Component-relative path of the vendored file.
        ours: ``(mode, blob)`` from our index.
        tree: Upstream's ``{path: (mode, blob)}``.

    Returns:
        The manifest record for this file.

    Raises:
        VacuousScanError: When the file deviates from upstream and the registry
            has not declared how, or when a declaration is stale.
    """
    mode, blob = ours
    elsewhere = sorted(p for p, (m, b) in tree.items() if b == blob and m == mode)
    if rel_path in dict(comp.patched_files):
        return _resolve_patch(comp, rel_path, ours, tree)
    if rel_path in dict(comp.local_files):
        if rel_path in tree or elsewhere:
            found = tree[rel_path][1] if rel_path in tree else f"as {elsewhere[0]}"
            message = (
                f"{comp.key}: '{rel_path}' is declared as having no upstream counterpart, but "
                f"upstream publishes one at the pinned revision ({found}). Move it to "
                "patched_files, or drop the declaration."
            )
            raise VacuousScanError(message)
        return Entry(KIND_LOCAL, mode, rel_path, local_blob=blob)
    if tree.get(rel_path) == (mode, blob):
        return Entry(KIND_OK, mode, rel_path, upstream_blob=blob)
    if elsewhere:
        return Entry(KIND_MOVED, mode, rel_path, upstream_blob=blob, upstream_path=elsewhere[0])
    detail = (
        f"upstream has it as {tree[rel_path][0]} {tree[rel_path][1]}"
        if rel_path in tree
        else "upstream has no file at that path and no file with those bytes"
    )
    message = (
        f"{comp.key}: '{rel_path}' does not match the pinned upstream revision and the "
        f"registry declares no deviation for it ({detail}). Refusing to record it as an "
        "intentional patch: that is how a corrupted file becomes 'modified on purpose'."
    )
    raise VacuousScanError(message)


def _resolve_patch(
    comp: Component, rel_path: str, ours: tuple[str, str], tree: dict[str, tuple[str, str]]
) -> Entry:
    """Build the `KIND_PATCH` record for a declared patch, or reject the declaration.

    Args:
        comp: The component being refreshed.
        rel_path: Component-relative path of the declared patch.
        ours: ``(mode, blob)`` from our index.
        tree: Upstream's ``{path: (mode, blob)}``.

    Returns:
        The `KIND_PATCH` record.

    Raises:
        VacuousScanError: When upstream has no such file, or when our copy is
            byte-identical to upstream and the declaration is therefore stale.
    """
    mode, blob = ours
    upstream = tree.get(rel_path)
    if upstream is None:
        message = (
            f"{comp.key}: '{rel_path}' is declared as a patch of upstream, but upstream has no "
            "such file at the pinned revision. Declare it in local_files instead."
        )
        raise VacuousScanError(message)
    if upstream[1] == blob:
        message = (
            f"{comp.key}: '{rel_path}' is declared as patched but is byte-identical to "
            "upstream. The declaration is stale -- drop it from patched_files (and from the "
            "component's docs/SOUP/ 'Deviations / patches' section) rather than leaving a "
            "deviation recorded that does not exist."
        )
        raise VacuousScanError(message)
    return Entry(KIND_PATCH, mode, rel_path, upstream_blob=upstream[1], local_blob=blob)


def refresh_component(comp: Component, cache: Path) -> tuple[str, dict[str, str]]:
    """Fetch a component's upstream and build its manifest records.

    Args:
        comp: The component to refresh.
        cache: Directory for upstream mirrors and archives.

    Returns:
        ``(manifest text, header)``; the header carries the per-kind counts.

    Raises:
        VacuousScanError: On any fetch failure or undeclared deviation.
    """
    if comp.upstream_transport == UPSTREAM_ARCHIVE:
        pin, tree = fetch_archive_tree(comp, cache)
        header = {
            "upstream-url": comp.url,
            "transport": UPSTREAM_ARCHIVE,
            "archive-url": comp.upstream_archive_url or "",
            "archive-sha256": pin,
        }
    else:
        pin, tree = fetch_git_tree(comp, cache)
        header = {
            "upstream-url": comp.upstream_repo or comp.url,
            "transport": "git",
            "ref": comp.upstream_ref or comp.upstream_commit or "",
            "commit": pin,
        }
    ours = git_ls_files(comp.path, comp.nested_paths)
    if not ours:
        message = f"{comp.key}: '{comp.path}' enumerated zero tracked files"
        raise VacuousScanError(message)
    entries = [_resolve_entry(comp, rel, ours[rel], tree) for rel in sorted(ours)]
    verified = sum(1 for e in entries if e.kind in (KIND_OK, KIND_MOVED))
    header["upstream-files"] = str(len(tree))
    header["vendored-files"] = str(len(entries))
    header["upstream-verified"] = str(verified)
    header["patched"] = str(sum(1 for e in entries if e.kind == KIND_PATCH))
    header["local"] = str(sum(1 for e in entries if e.kind == KIND_LOCAL))
    return format_manifest(comp.key, header, entries), header


def run_refresh(*, write: bool, only: str | None) -> int:
    """Fetch every component's upstream and rewrite (or verify) its manifest.

    Args:
        write: True to write the manifests; False to compare and report only.
        only: Restrict to one registry key, or None for all.

    Returns:
        Process exit status.
    """
    cache = REPO_ROOT / "build" / "soup-upstream"
    comps = [c for c in vendored_components() if only is None or c.key == only]
    if not comps:
        print(f"check_soup_upstream: no vendored component named '{only}'", file=sys.stderr)
        return EXIT_VACUOUS
    failures: list[str] = []
    for comp in comps:
        try:
            text, header = refresh_component(comp, cache)
        except (VacuousScanError, ManifestError) as exc:
            failures.append(str(exc))
            print(f"  FAIL {comp.key}: {exc}", file=sys.stderr)
            continue
        out = REPO_ROOT / manifest_path(comp.key)
        if write:
            # Only --refresh touches the tree. --verify-upstream must not, not
            # even to create a directory: a scheduled job that quietly adopted
            # upstream's new bytes would launder the event it exists to report.
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text, encoding="utf-8")
        elif not out.is_file() or out.read_text(encoding="utf-8") != text:
            failures.append(f"{comp.key}: committed manifest no longer describes upstream")
            print(
                f"  FAIL {comp.key}: the committed manifest disagrees with upstream "
                f"{header.get('commit') or header.get('archive-sha256')}. The pinned "
                "revision moved, or the vendored tree changed without a refresh.",
                file=sys.stderr,
            )
            continue
        print(
            f"  {'wrote' if write else 'ok   '} {comp.key:24s} "
            f"{header['vendored-files']:>5s} files, "
            f"{header['upstream-verified']:>5s} upstream-verified, "
            f"{header['patched']} patched, {header['local']} local"
        )
    if failures:
        print(f"check_soup_upstream: {len(failures)} component(s) failed", file=sys.stderr)
        return EXIT_FAIL
    print(f"check_soup_upstream: {len(comps)} components resolved against upstream.")
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch to the check / refresh / selftest action."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--refresh", action="store_true", help="NETWORK: rewrite the manifests")
    parser.add_argument(
        "--verify-upstream",
        action="store_true",
        help="NETWORK: refetch upstream and fail if the committed manifests disagree",
    )
    parser.add_argument("--component", help="restrict --refresh/--verify-upstream to one key")
    parser.add_argument("--selftest", action="store_true", help="prove the checker both ways")
    args = parser.parse_args(argv)
    if args.selftest:
        from soup_selftest import run_selftest  # noqa: PLC0415  # selftest-only import

        return run_selftest()
    if args.refresh or args.verify_upstream:
        return run_refresh(write=args.refresh, only=args.component)
    return run_check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
r"""The on-disk format of a vendored-SOUP upstream provenance manifest.

One manifest per vendored component lives under ``docs/sbom/upstream/`` and
records, for every file we vendor, the **git blob SHA-1 that the upstream
project publishes for it**.  A git blob id is a content hash
(``sha1("blob <len>\0" + bytes)``), so recording it pins the bytes exactly --
and because our own index already stores the same hash for every tracked file,
verifying the claim offline is a comparison of two hashes computed by two
different projects, never a value compared against itself (#548).

Why the hashes come from upstream and not from us
-------------------------------------------------
``gen_sbom.py`` re-derives an integrity digest over each vendored tree on every
run, which proves the tree has not changed since the SBOM was regenerated.  It
cannot prove the tree was RIGHT when it was vendored: a bad copy, a partial
subset or a moved tag would be hashed faithfully and reported clean forever.
The manifests close that by carrying evidence that did not originate here --
``check_soup_upstream.py --refresh`` fetches the pinned upstream revision and
writes down what upstream says, and ``--check`` compares our tree to it.

Record kinds
------------
Four, and the last two are the point: SOUP is sometimes patched on purpose, so
"modified" and "corrupted" have to be distinguishable by a machine.

``ok``
    Vendored at the same relative path upstream uses, byte-identical.
``moved``
    Byte-identical, but relocated in our tree (xz-embedded flattens
    ``linux/lib/xz/``; the RSIP blob mirrors upstream's root ``LICENSE.md`` as
    ``UPSTREAM_LICENSE.md``).  The upstream path is recorded so the mapping is
    reviewable rather than inferred at check time.
``patch``
    A deliberate local modification.  Both hashes are recorded: upstream's (so
    a moved pin is still caught) and ours (so an *additional* edit on top of
    the reviewed patch fails).  The registry must declare the file in
    ``patched_files`` or the gate rejects it.
``local``
    A file with no upstream counterpart at all -- a build-generated artifact we
    vendor because the firmware build does not run upstream's generator, or a
    first-party shim.  The registry must declare it in ``local_files``.

Line format, whitespace-separated (no vendored path in this tree contains a
space, and ``parse_manifest`` rejects one that does)::

    ok    <mode> <upstream-blob> <rel-path>
    moved <mode> <upstream-blob> <rel-path> <upstream-path>
    patch <mode> <upstream-blob> <local-blob> <rel-path>
    local <mode> <local-blob> <rel-path>
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_DIR = Path("docs/sbom/upstream")
MANIFEST_SUFFIX = ".manifest"

KIND_OK = "ok"
KIND_MOVED = "moved"
KIND_PATCH = "patch"
KIND_LOCAL = "local"

# Kinds whose bytes were proven equal to a hash published by the upstream
# project.  The vacuity floor counts these and nothing else: a manifest made
# entirely of `patch` and `local` rows records only our own opinion of our own
# tree, which is the defect this whole gate exists to remove.
UPSTREAM_VERIFIED_KINDS = (KIND_OK, KIND_MOVED)

BLOB_RE = re.compile(r"\A[0-9a-f]{40}\Z")
MODE_RE = re.compile(r"\A[0-7]{6}\Z")

HEADER_PREFIX = "# "
MANIFEST_BANNER = "ra8-firmware SOUP upstream provenance manifest -- generated, do not hand-edit"


class ManifestError(Exception):
    """A manifest is unreadable, malformed, or claims something impossible."""


@dataclass(frozen=True)
class Entry:
    """One vendored file's provenance record.

    Attributes:
        kind: One of `KINDS`.
        mode: Six-digit git file mode (``100644`` / ``100755`` / ``120000``).
        rel_path: Path relative to the component root, as vendored.
        upstream_blob: Upstream's blob SHA-1, or None for `KIND_LOCAL`.
        local_blob: Our blob SHA-1, set only for `KIND_PATCH` / `KIND_LOCAL`.
        upstream_path: Upstream's path, set only for `KIND_MOVED`.
    """

    kind: str
    mode: str
    rel_path: str
    upstream_blob: str | None = None
    local_blob: str | None = None
    upstream_path: str | None = None

    def format(self) -> str:
        """Render this entry as one manifest line (no trailing newline)."""
        if self.kind == KIND_OK:
            return f"{KIND_OK} {self.mode} {self.upstream_blob} {self.rel_path}"
        if self.kind == KIND_MOVED:
            return (
                f"{KIND_MOVED} {self.mode} {self.upstream_blob} "
                f"{self.rel_path} {self.upstream_path}"
            )
        if self.kind == KIND_PATCH:
            return (
                f"{KIND_PATCH} {self.mode} {self.upstream_blob} {self.local_blob} {self.rel_path}"
            )
        return f"{KIND_LOCAL} {self.mode} {self.local_blob} {self.rel_path}"


@dataclass(frozen=True)
class Manifest:
    """A parsed manifest: its header fields plus one `Entry` per vendored file."""

    key: str
    header: dict[str, str]
    entries: tuple[Entry, ...]

    def verified_count(self) -> int:
        """Return how many entries were proven against an upstream-published hash."""
        return sum(1 for e in self.entries if e.kind in UPSTREAM_VERIFIED_KINDS)

    def by_path(self) -> dict[str, Entry]:
        """Return the entries keyed by component-relative path."""
        return {e.rel_path: e for e in self.entries}


def manifest_path(key: str) -> Path:
    """Return the repo-relative manifest path for a registry key.

    Nested keys keep their shape (``esp-hosted/protobuf-c`` ->
    ``esp-hosted/protobuf-c.manifest``) so the directory mirrors the registry.

    Args:
        key: The registry component key.

    Returns:
        Repo-relative path of that component's manifest.
    """
    return MANIFEST_DIR / (key + MANIFEST_SUFFIX)


def _parse_entry(line: str, lineno: int, path: Path) -> Entry:
    """Parse one manifest body line into an `Entry`.

    Args:
        line: The raw line, without its newline.
        lineno: 1-based line number, for error messages.
        path: Manifest path, for error messages.

    Returns:
        The parsed entry.

    Raises:
        ManifestError: On any malformed field.
    """
    fields = line.split(" ")
    kind = fields[0]
    # kind -> (field count, indices that must be 40-hex blob ids)
    shape = {
        KIND_OK: (4, (2,)),
        KIND_MOVED: (5, (2,)),
        KIND_PATCH: (5, (2, 3)),
        KIND_LOCAL: (4, (2,)),
    }
    if kind not in shape:
        message = f"{path}:{lineno}: unknown record kind '{kind}'"
        raise ManifestError(message)
    width, blob_fields = shape[kind]
    if len(fields) != width:
        message = f"{path}:{lineno}: '{kind}' record needs {width} fields, got {len(fields)}"
        raise ManifestError(message)
    if not MODE_RE.match(fields[1]):
        message = f"{path}:{lineno}: '{fields[1]}' is not a git file mode"
        raise ManifestError(message)
    for index in blob_fields:
        if not BLOB_RE.match(fields[index]):
            message = f"{path}:{lineno}: '{fields[index]}' is not a 40-hex blob id"
            raise ManifestError(message)
    if kind == KIND_OK:
        return Entry(kind, fields[1], fields[3], upstream_blob=fields[2])
    if kind == KIND_MOVED:
        return Entry(kind, fields[1], fields[3], upstream_blob=fields[2], upstream_path=fields[4])
    if kind == KIND_PATCH:
        return Entry(kind, fields[1], fields[4], upstream_blob=fields[2], local_blob=fields[3])
    return Entry(kind, fields[1], fields[3], local_blob=fields[2])


def parse_manifest(key: str, text: str, path: Path) -> Manifest:
    """Parse a manifest document.

    Args:
        key: Registry key the manifest is expected to describe.
        text: Full manifest text.
        path: Manifest path, for error messages.

    Returns:
        The parsed manifest.

    Raises:
        ManifestError: On a malformed header, a malformed record, a duplicate
            path, or a header ``component:`` that names a different component.
    """
    header: dict[str, str] = {}
    entries: list[Entry] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if not raw:
            continue
        if raw.startswith(HEADER_PREFIX):
            field, _, value = raw[len(HEADER_PREFIX) :].partition(": ")
            if value:
                header[field] = value
            continue
        if raw.startswith("#"):
            continue
        if "\t" in raw or "  " in raw:
            message = f"{path}:{lineno}: fields are single-space separated"
            raise ManifestError(message)
        entries.append(_parse_entry(raw, lineno, path))
    seen: set[str] = set()
    for entry in entries:
        if entry.rel_path in seen:
            message = f"{path}: duplicate record for '{entry.rel_path}'"
            raise ManifestError(message)
        seen.add(entry.rel_path)
    if header.get("component") != key:
        message = f"{path}: header says component '{header.get('component')}', expected '{key}'"
        raise ManifestError(message)
    return Manifest(key=key, header=header, entries=tuple(entries))


def format_manifest(key: str, header: dict[str, str], entries: list[Entry]) -> str:
    """Render a manifest document deterministically.

    Args:
        key: Registry key; written as the ``component`` header field.
        header: Ordered header fields (``component`` is inserted first).
        entries: The records; sorted by path here so the file is reproducible.

    Returns:
        The manifest text, newline-terminated.
    """
    lines = [f"# {MANIFEST_BANNER}.", f"# component: {key}"]
    lines.extend(f"# {field}: {value}" for field, value in header.items())
    lines.extend(entry.format() for entry in sorted(entries, key=lambda e: e.rel_path))
    return "\n".join(lines) + "\n"


def git_ls_files(
    rel_path: str, exclude: tuple[str, ...] = (), root: Path | None = None
) -> dict[str, tuple[str, str]]:
    """Return ``{component-relative path: (mode, blob)}`` for a vendored tree.

    The blob id comes straight out of our index, so nothing is re-hashed here
    and no file content is read: git already computed the same content hash
    upstream did.

    Args:
        rel_path: Repo-relative path of the component (a directory or one file).
        exclude: Repo-relative prefixes to drop -- nested components that carry
            their own registry entry and their own manifest.
        root: Repository to enumerate; defaults to this checkout.  The selftest
            passes a scratch repository so it drives this exact function.

    Returns:
        The tracked files, keyed by path relative to `rel_path`.

    Raises:
        ManifestError: When ``git ls-files`` fails or a path contains a space.
    """
    proc = subprocess.run(  # noqa: S603  # trusted: fixed git argv, no shell
        ["git", "ls-files", "-sz", "--", rel_path],  # noqa: S607 -- trusted: fixed git argv
        cwd=root or REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        message = f"`git ls-files -- {rel_path}` failed: {proc.stderr.strip()}"
        raise ManifestError(message)
    out: dict[str, tuple[str, str]] = {}
    prefix = rel_path.rstrip("/") + "/"
    for record in proc.stdout.split("\0"):
        if not record:
            continue
        meta, path = record.split("\t", 1)
        if " " in path:
            message = f"vendored path '{path}' contains a space; format cannot encode it"
            raise ManifestError(message)
        if any(path == drop or path.startswith(drop.rstrip("/") + "/") for drop in exclude):
            continue
        mode, blob, _stage = meta.split()
        out[path[len(prefix) :] if path.startswith(prefix) else Path(path).name] = (mode, blob)
    return out

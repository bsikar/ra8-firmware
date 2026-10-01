#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate the checked-in media-download codec against the schema it was generated from.

``libs/ra8_c6link/proto/ra8_media_download.proto`` generates the protobuf-c codec
committed beside it (``libs/ra8_c6link/inc/ra8_media_download.pb-c.h`` and
``libs/ra8_c6link/src/ra8_media_download.pb-c.c``).  The regeneration script
``scripts/gen/gen_ra8_media_proto.sh --check`` proves the two match byte for byte, but
it needs the exact pinned generator pair (protobuf-c 1.5.2 / libprotoc 35.1), which is
absent from the dev box and the CI image.  So the repository documented a freshness
check that nothing ran, and 777 generated lines of the wire codec both ends of the media
path depend on could drift with nothing failing.

This checker closes the drift half of that hole WITHOUT the generator.  ``.github
/proto-codec-pairing.txt`` records the SHA-256 of the schema and of both generated
files as one set, written by the regeneration script itself.  Every run re-derives all
three digests from the working tree and compares:

* a hand edit to either generated file moves its digest and FAILS;
* a schema change with a forgotten regenerate moves the schema digest and FAILS;
* a real regenerate rewrites the manifest in the same command, so it PASSES.

What it deliberately does NOT claim: it never proves the committed C is what protoc-c
would emit for this schema today.  Only a regenerate proves that, and only where the
pinned generator exists.  This gate proves the three files moved together, which is the
drift class #715 names.  The manifest is self-attesting: the row set is fixed here, so
deleting a row or adding one is a failure rather than a silent narrowing.

Usage::

    python3 scripts/checks/check_proto_codec_pairing.py             # check (gate mode)
    python3 scripts/checks/check_proto_codec_pairing.py --write     # rewrite manifest
    python3 scripts/checks/check_proto_codec_pairing.py --selftest  # both directions
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

MANIFEST_REL = ".github/proto-codec-pairing.txt"
# A manifest row is "<sha256>  <path>": two fields, the first 64 hex digits.
ROW_FIELDS = 2
SHA256_HEX_LEN = 64

#: The exact row set.  Fixed here, not read from the manifest, so a manifest that
#: dropped a row is a failure rather than a check that quietly stopped covering it.
TRACKED: tuple[str, ...] = (
    "libs/ra8_c6link/proto/ra8_media_download.proto",
    "libs/ra8_c6link/inc/ra8_media_download.pb-c.h",
    "libs/ra8_c6link/src/ra8_media_download.pb-c.c",
)

HEADER_LINES: tuple[str, ...] = (
    "# RA8 media-download schema/codec pairing manifest.",
    "#",
    "# One SHA-256 per file: the schema and the two generated protobuf-c outputs, as one",
    "# set. scripts/checks/check_proto_codec_pairing.py re-derives every digest from the",
    "# tree on each artefact-freshness run, so a hand edit to a generated file, or a",
    "# schema change with a forgotten regenerate, fails the gate. It needs no generator.",
    "#",
    "# This is NOT the byte-exact regeneration check: that is",
    "# `scripts/gen/gen_ra8_media_proto.sh --check` and it needs the pinned generator",
    "# pair (protobuf-c 1.5.2 / libprotoc 35.1). See #715.",
    "#",
    "# Do not hand-edit. Rewritten by `bash scripts/gen/gen_ra8_media_proto.sh --write`.",
    "# Rows: <sha256>  <repo-relative path>",
)


def repo_root() -> Path:
    """Return the repository root, preferring git so the gate is run-from-anywhere."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- git off PATH, fixed argv
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return Path(__file__).resolve().parents[2]
    return Path(out.stdout.strip())


def digest(path: Path) -> str:
    """Return the SHA-256 of ``path`` as lowercase hex."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render(root: Path) -> str:
    """Render the manifest text for the tree at ``root``."""
    rows = [f"{digest(root / rel)}  {rel}" for rel in TRACKED]
    return "\n".join([*HEADER_LINES, *rows]) + "\n"


def parse(text: str) -> tuple[dict[str, str], list[str]]:
    """Parse manifest ``text`` into a path->digest map plus a list of complaints."""
    rows: dict[str, str] = {}
    problems: list[str] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != ROW_FIELDS or len(parts[0]) != SHA256_HEX_LEN:
            problems.append(f"{MANIFEST_REL}:{number}: not a '<sha256>  <path>' row: {raw!r}")
            continue
        sha, rel = parts
        if rel in rows:
            problems.append(f"{MANIFEST_REL}:{number}: duplicate row for {rel}")
            continue
        rows[rel] = sha.lower()
    return rows, problems


def check(root: Path) -> int:
    """Compare the committed manifest against freshly derived digests. 0 on success."""
    manifest = root / MANIFEST_REL
    if not manifest.is_file():
        print(f"FAIL: {MANIFEST_REL} is missing; the codec pairing is unproven.")
        return 1
    rows, problems = parse(manifest.read_text(encoding="utf-8"))
    for rel in TRACKED:
        target = root / rel
        if not target.is_file():
            problems.append(f"{rel}: tracked file is missing from the tree")
            continue
        if rel not in rows:
            problems.append(f"{rel}: no row in {MANIFEST_REL}; run the regenerate script")
            continue
        actual = digest(target)
        if actual != rows[rel]:
            problems.append(
                f"{rel}: digest drifted\n    manifest: {rows[rel]}\n    tree:     {actual}"
            )
    for rel in sorted(set(rows) - set(TRACKED)):
        problems.append(f"{rel}: unexpected row in {MANIFEST_REL}; this gate does not own it")
    if problems:
        print("FAIL: the media-download schema and its generated codec do not pair.")
        for problem in problems:
            print(f"  {problem}")
        print("  Regenerate with: bash scripts/gen/gen_ra8_media_proto.sh --write")
        print("  (that needs protobuf-c 1.5.2 / libprotoc 35.1; see #715)")
        return 1
    print(f"PASS: schema and generated codec pair across {len(TRACKED)} files.")
    return 0


def write(root: Path) -> int:
    """Rewrite the manifest from the tree. 0 on success."""
    for rel in TRACKED:
        if not (root / rel).is_file():
            print(f"FAIL: cannot write {MANIFEST_REL}; {rel} is missing.")
            return 1
    (root / MANIFEST_REL).write_text(render(root), encoding="utf-8")
    print(f"wrote {MANIFEST_REL} ({len(TRACKED)} rows)")
    return 0


def _seed(root: Path) -> None:
    """Seed a throwaway tree that satisfies the checker."""
    for rel in TRACKED:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"content of {rel}\n", encoding="utf-8")
    (root / ".github").mkdir(parents=True, exist_ok=True)
    write(root)


def selftest() -> int:
    """Prove the detector fires and stays quiet, in both directions. 0 on success."""
    mutations: list[tuple[str, bool, object]] = [
        ("clean tree passes", False, None),
        ("hand edit to the generated source fails", True, ("append", TRACKED[2], "/* edit */\n")),
        ("schema change without regenerate fails", True, ("append", TRACKED[0], "// field\n")),
        ("manifest missing a tracked row fails", True, ("drop-row", TRACKED[1], "")),
        ("manifest carrying an unknown row fails", True, ("extra-row", "", "")),
        ("malformed manifest row fails", True, ("garbage", "", "")),
        ("absent manifest fails", True, ("unlink", "", "")),
    ]
    failures = 0
    for name, expect_failure, mutation in mutations:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _seed(root)
            _mutate(root, mutation)
            got_failure = check(root) != 0
            if got_failure != expect_failure:
                verb = "failed" if got_failure else "passed"
                print(f"SELFTEST FAIL: {name}: it {verb} instead.")
                failures += 1
            else:
                print(f"selftest ok: {name}")
    if failures:
        print(f"FAIL: {failures} selftest case(s) did not behave.")
        return 1
    print(f"PASS: selftest proved both directions across {len(mutations)} cases.")
    return 0


def _mutate(root: Path, mutation: object) -> None:
    """Apply one selftest mutation to the seeded tree."""
    if mutation is None:
        return
    kind, rel, payload = mutation  # type: ignore[misc]
    manifest = root / MANIFEST_REL
    if kind == "append":
        target = root / rel
        target.write_text(target.read_text(encoding="utf-8") + payload, encoding="utf-8")
    elif kind == "drop-row":
        lines = manifest.read_text(encoding="utf-8").splitlines()
        kept = [ln for ln in lines if not ln.endswith(rel)]
        manifest.write_text("\n".join(kept) + "\n", encoding="utf-8")
    elif kind == "extra-row":
        ghost = f"{'0' * 64}  libs/ra8_c6link/src/ghost.pb-c.c\n"
        manifest.write_text(manifest.read_text(encoding="utf-8") + ghost, encoding="utf-8")
    elif kind == "garbage":
        garbage = manifest.read_text(encoding="utf-8") + "not-a-digest\n"
        manifest.write_text(garbage, encoding="utf-8")
    elif kind == "unlink":
        manifest.unlink()


def main(argv: list[str]) -> int:
    """Dispatch the three modes."""
    mode = argv[1] if len(argv) > 1 else "--check"
    if mode == "--selftest":
        return selftest()
    if mode == "--write":
        return write(repo_root())
    if mode == "--check":
        return check(repo_root())
    print(f"usage: {argv[0]} [--check|--write|--selftest]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

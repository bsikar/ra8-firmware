#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regenerate the tracked HUM chapter map and require byte identity."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ARTEFACT = Path("docs/reference/CHAPTER_MAP.md")
SOURCE_PDF = Path("docs/reference/ra8d2-hardware-user-manual.pdf")
GENERATOR = Path("scripts/gen/build_chapter_map.sh")
CHAPTER_ROW_RE = re.compile(r"^\|\s+\d+\s+\|")
EXPECTED_CHAPTERS = 69


class GenerationError(RuntimeError):
    """The canonical generator could not produce trustworthy bytes."""


def _run(root: Path, *argv: str) -> subprocess.CompletedProcess[bytes]:
    """Run fixed repository tooling without a shell."""
    return subprocess.run(  # noqa: S603 -- fixed executable and caller-owned argv
        [*argv], cwd=root, capture_output=True, check=False
    )


def _is_tracked(root: Path, path: Path) -> bool:
    """Return whether Git owns ``path`` in the candidate index."""
    git = shutil.which("git")
    if git is None:
        return False
    result = _run(root, git, "ls-files", "--error-unmatch", "--", path.as_posix())
    return result.returncode == 0


def _generate(root: Path) -> bytes:
    """Run the canonical generator into an isolated temporary output."""
    bash = shutil.which("bash")
    if bash is None:
        message = "bash is required"
        raise GenerationError(message)
    with tempfile.TemporaryDirectory() as raw_tmp:
        output = Path(raw_tmp) / ARTEFACT.name
        result = _run(root, bash, str(root / GENERATOR), "--output", str(output))
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            message = detail or "chapter-map generator failed"
            raise GenerationError(message)
        if not output.is_file():
            message = "chapter-map generator wrote no output"
            raise GenerationError(message)
        return output.read_bytes()


def freshness_reason(candidate: bytes | None, fresh: bytes) -> str | None:
    """Return a failure reason for missing/drifted bytes, or ``None``."""
    if candidate is None:
        return "tracked candidate chapter map is missing"
    if candidate != fresh:
        return f"candidate is {len(candidate)} bytes; regenerate is {len(fresh)} bytes"
    return None


def _chapter_count(rendered: bytes) -> int:
    """Count structurally rendered chapter-table rows."""
    text = rendered.decode("ascii")
    return sum(CHAPTER_ROW_RE.match(line) is not None for line in text.splitlines())


def selftest() -> int:
    """Prove both verdict directions and the live generator's invariants."""
    failures: list[str] = []
    sample = b"chapter-map\n"
    if freshness_reason(sample, sample) is not None:
        failures.append("equal bytes were rejected")
    if freshness_reason(sample + b"drift\n", sample) is None:
        failures.append("drifted bytes were accepted")
    if freshness_reason(None, sample) is None:
        failures.append("missing candidate was accepted")
    if not _is_tracked(REPO_ROOT, SOURCE_PDF):
        failures.append(f"source PDF is not tracked: {SOURCE_PDF}")
    first = _generate(REPO_ROOT)
    second = _generate(REPO_ROOT)
    if first != second:
        failures.append("two clean regenerations differ")
    count = _chapter_count(first)
    if count != EXPECTED_CHAPTERS:
        failures.append(f"chapter census drifted: {count} != {EXPECTED_CHAPTERS}")
    if failures:
        for failure in failures:
            print(f"check_chapter_map_freshness.py: selftest FAIL: {failure}", file=sys.stderr)
        return 1
    print("check_chapter_map_freshness.py: selftest OK (both directions; 69 chapters)")
    return 0


def check() -> int:
    """Compare the tracked candidate map with a clean regeneration."""
    if not _is_tracked(REPO_ROOT, ARTEFACT):
        print(f"check_chapter_map_freshness.py: FAIL: {ARTEFACT} is not tracked", file=sys.stderr)
        return 1
    try:
        fresh = _generate(REPO_ROOT)
    except GenerationError as exc:
        print(f"check_chapter_map_freshness.py: ERROR: {exc}", file=sys.stderr)
        return 2
    reason = freshness_reason((REPO_ROOT / ARTEFACT).read_bytes(), fresh)
    if reason is not None:
        print(
            f"check_chapter_map_freshness.py: FAIL: {reason}; run scripts/gen/build_chapter_map.sh",
            file=sys.stderr,
        )
        return 1
    print(f"Chapter map fresh ({EXPECTED_CHAPTERS} chapters from tracked full PDF)")
    return 0


def main() -> int:
    """Dispatch live check or selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    return selftest() if args.selftest else check()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""List first-party, shared, and vendored library descriptions."""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DESCRIPTION_LIMIT = 80
NAME_COLUMN_WIDTH = 30
LIBRARY_PATTERNS = (
    "libs/*",
    "libs/third_party/*",
    "apps/shared_libs/*",
    "apps/shared_libs/third_party/*",
)


def _vendor_brief(lib_dir: Path) -> str:
    """Read one vendored component description from its parent registry."""
    readme_path = lib_dir.parent / "README.md"
    if not readme_path.exists():
        return ""
    readme_text = readme_path.read_text(encoding="utf-8")
    match = re.search(
        rf"\|\s*`?{re.escape(lib_dir.name)}`?\s*\|\s*(.+?)\s*\|",
        readme_text,
    )
    return match.group(1).strip() if match else ""


def _readme_summary(text: str) -> str:
    """Return the first prose line from a library README."""
    excluded_prefixes = ("#", "=", "-", "[", "!", "<", "Copyright", "SPDX")
    for source_line in text.splitlines():
        line = source_line.strip()
        if line and not line.startswith(excluded_prefixes):
            suffix = "..." if len(line) > DESCRIPTION_LIMIT else ""
            return line[:DESCRIPTION_LIMIT] + suffix
    return ""


def _brief(lib_dir: Path) -> str:
    """Extract a library description from its header or README."""
    if "third_party" in lib_dir.parts:
        return _vendor_brief(lib_dir)

    lib_name = lib_dir.name
    header_candidates = [
        lib_dir / "inc" / f"{lib_name}.h",
        lib_dir / "inc" / f"{lib_name.replace('ra8_', '')}.h",
        lib_dir / f"{lib_name}.h",
        lib_dir / "README.md",
        lib_dir / "README.txt",
        lib_dir / "README",
        *(lib_dir / "inc").glob("*.h"),
    ]
    for document in header_candidates:
        if not document.exists():
            continue
        text = document.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r"@brief\s+([^\n]+)", text)
        if match:
            return match.group(1).strip()
        if document.name.startswith("README"):
            summary = _readme_summary(text)
            if summary:
                return summary
    return ""


def _library_dirs() -> list[Path]:
    """Return every direct library component from the canonical roots."""
    candidates = {path for pattern in LIBRARY_PATTERNS for path in REPO_ROOT.glob(pattern)}
    return sorted(path for path in candidates if path.is_dir() and path.name != "third_party")


def _categorized_entries(search_keyword: str | None) -> tuple[list[str], list[str], list[str]]:
    """Build display rows for firmware, shared, and vendored libraries."""
    first_party: list[str] = []
    shared_party: list[str] = []
    third_party: list[str] = []
    for lib_dir in _library_dirs():
        name = lib_dir.name
        brief = _brief(lib_dir)
        if (
            search_keyword
            and search_keyword not in name.lower()
            and search_keyword not in brief.lower()
        ):
            continue
        entry = f"  - {name:<{NAME_COLUMN_WIDTH}} {brief}"
        if "third_party" in lib_dir.parts:
            third_party.append(entry)
        elif "shared_libs" in lib_dir.parts:
            shared_party.append(entry)
        else:
            first_party.append(entry)
    return first_party, shared_party, third_party


def _print_section(title: str, entries: list[str], *, visible: bool = True) -> None:
    """Print one optional titled group."""
    if not entries:
        return
    if visible:
        print(f"\n{title}:")
    for entry in entries:
        print(entry)


def main() -> None:
    """List libraries, optionally filtered by a case-insensitive query."""
    search_keyword = sys.argv[1].lower() if len(sys.argv) > 1 else None
    first_party, shared_party, third_party = _categorized_entries(search_keyword)

    if search_keyword:
        print(f"SEARCH RESULTS IN LIBRARIES FOR '{search_keyword}':")
        if not first_party and not third_party and not shared_party:
            print("  (No matches found)")
    else:
        print("FIRMWARE LIBRARIES:")

    for entry in first_party:
        print(entry)
    _print_section("SHARED LIBRARIES", shared_party, visible=search_keyword is None)
    _print_section("THIRD-PARTY LIBRARIES", third_party, visible=search_keyword is None)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""List test targets belonging to one repository domain."""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MIN_ARGUMENTS = 2
TEST_SUFFIXES = ("c", "cpp")
NAME_COLUMN_WIDTH = 40
CATEGORY_PATTERNS = {
    "shared": ("apps/shared_libs/*/tests",),
    "host": ("apps/host/*/tests",),
    "board": ("apps/board/*/*/tests", "apps/board/*/tests"),
    "tools": ("tools/*/tests",),
}


def _search_patterns(category: str) -> tuple[str, ...]:
    """Return relative glob roots for one test category."""
    return CATEGORY_PATTERNS.get(category, (f"tests/{category}",))


def main() -> None:
    """Print the tests discovered for the requested category."""
    if len(sys.argv) < MIN_ARGUMENTS:
        print("Usage: list_tests.py <category>")
        sys.exit(1)

    category = sys.argv[1].lower()

    search_patterns = _search_patterns(category)

    tests: list[tuple[str, str]] = []
    for search_pattern in search_patterns:
        for suffix in TEST_SUFFIXES:
            for path in REPO_ROOT.glob(f"{search_pattern}/test_*.{suffix}"):
                test_name = path.stem

                # extract brief
                desc = f"{test_name} unit tests"
                with path.open(encoding="utf-8", errors="ignore") as handle:
                    for source_line in handle:
                        match = re.search(r"@brief\s+(.*)", source_line)
                        if match:
                            desc = match.group(1).strip()
                            break

                tests.append((test_name, desc))

    tests.sort(key=lambda item: item[0])

    if not tests:
        print(f"Error: Category '{category}' not found or has no tests.")
        sys.exit(1)

    print(
        f"== {category.upper()} TESTS ({len(tests)}) -- "
        f"local: just tests::local {category} | "
        f"container: just tests::devcontainer {category}\n"
    )
    for test_name, description in tests:
        print(f"  {test_name.ljust(NAME_COLUMN_WIDTH)} {description}")
    print()


if __name__ == "__main__":
    main()

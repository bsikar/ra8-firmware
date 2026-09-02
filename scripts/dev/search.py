#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Search repository applications, examples, and unit-test targets."""

import sys
from pathlib import Path

from ra8_apps import _parse_desc, get_apps

REPO_ROOT = Path(__file__).resolve().parents[2]
MIN_ARGUMENTS = 2


def get_host_apps() -> list[dict[str, str]]:
    """Discover host-side CMake applications."""
    apps: list[dict[str, str]] = []
    host_dir = REPO_ROOT / "apps" / "host"
    if not host_dir.is_dir():
        return apps
    for cmake_file in sorted(host_dir.rglob("CMakeLists.txt")):
        app_dir = cmake_file.parent
        name = app_dir.name
        desc = _parse_desc(str(app_dir)) or f"Host application {name}"
        apps.append(
            {
                "name": name,
                "group": "host",
                "dir": str(app_dir),
                "rel_dir": str(app_dir.relative_to(REPO_ROOT)),
                "desc": desc,
            }
        )
    return apps


def _find_matching_tests(query: str) -> list[str]:
    """Return sorted unit-test basenames containing the query."""
    tests: list[str] = []
    tests_dir = REPO_ROOT / "tests"
    for suffix in ("c", "cpp"):
        for path in tests_dir.rglob(f"test_*.{suffix}"):
            test_name = path.stem
            if query in test_name.lower():
                tests.append(test_name)
    return sorted(tests)


def _print_results(
    query: str,
    apps: list[dict[str, str]],
    examples: list[dict[str, str]],
    tests: list[str],
) -> None:
    """Render grouped search results and their canonical Just commands."""
    total = len(apps) + len(examples) + len(tests)
    if total == 0:
        print(f"No results found for '{query}'.")
        return

    print(f"Search Results for '{query}' ({total} matches):\n")
    if apps:
        print(f"APPS ({len(apps)}):")
        for a in apps:
            name = a["name"]
            print(f"  {a['desc']}")
            if a["group"] == "host":
                print(f"    just apps::host::build {name}")
            else:
                print(f"    just apps::build {name}")
                print(f"    just apps::hardware::flash {name}")
                print(f"    just apps::emulator::run {name}")
        print()

    if examples:
        print(f"\n[ EXAMPLES ] ({len(examples)} matches)")
        for a in examples:
            name = a["name"]
            print(f"  {a['desc']} ({name})")
            print(f"    just apps::build {name}")
            print(f"    just apps::hardware::flash {name}")
            print(f"    just apps::emulator::run {name}")
        print()

    if tests:
        print(f"TESTS ({len(tests)}):")
        for t in tests:
            print(f"  {t}")
            print(f"    just tests::local {t}")
        print()


def main() -> None:
    """Search all supported repository target classes."""
    if len(sys.argv) < MIN_ARGUMENTS:
        print("Usage: just search <keyword>")
        sys.exit(1)

    query = sys.argv[1].lower()
    apps = []
    examples = []

    for a in get_apps() + get_host_apps():
        full_id = f"{a['group'].replace('/', '::')}::{a['name']}".lower()
        desc = a["desc"].lower()
        if query in full_id or query in desc or query in a["name"].lower():
            if "board/stand_alone" in a["group"] or "host" in a["group"]:
                apps.append(a)
            else:
                examples.append(a)

    tests = _find_matching_tests(query)
    _print_results(query, apps, examples, tests)


if __name__ == "__main__":
    main()

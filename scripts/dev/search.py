#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Unified search across Just commands, apps, examples, libraries, tests, gates, and tools."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parent))

from list_libs import _brief as _lib_brief
from list_libs import _library_dirs
from ra8_apps import _parse_desc, app_id, get_apps

REPO_ROOT = Path(__file__).resolve().parents[2]

MIN_ARGUMENTS = 2
GATE_RECORD_FIELD_COUNT = 3
TEST_SUFFIXES = ("c", "cpp", "rs", "zig")


class RecipeRecord(TypedDict):
    """Metadata for one discovered Just recipe."""

    name: str
    namepath: str
    doc: str
    params: str


class AppInfo(TypedDict):
    """Metadata for one discovered host or firmware application."""

    name: str
    group: str
    dir: str
    rel_dir: str
    desc: str
    full_id: str


class LibraryRecord(TypedDict):
    """Metadata for one discovered library component."""

    name: str
    group: str
    path: str
    desc: str


class TestRecord(TypedDict):
    """Metadata for one discovered unit or integration test."""

    name: str
    path: str
    category: str
    desc: str


class GateRecord(TypedDict):
    """Metadata for one discovered CI quality gate."""

    name: str
    speed: str
    desc: str


class ToolRecord(TypedDict):
    """Metadata for one discovered developer host tool."""

    name: str
    path: str
    desc: str


class SearchResults(TypedDict):
    """Container for matched items by target category."""

    commands: list[RecipeRecord]
    apps: list[AppInfo]
    examples: list[AppInfo]
    libraries: list[LibraryRecord]
    tests: list[TestRecord]
    gates: list[GateRecord]
    tools: list[ToolRecord]


def _parse_justfile(path: Path, prefix: str = "") -> list[RecipeRecord]:
    """Recursively discover recipes across Just modules."""
    recipes: list[RecipeRecord] = []
    if not path.is_file():
        return recipes
    text = path.read_text(encoding="utf-8", errors="ignore")
    pending_doc = ""
    mod_re = re.compile(r"^\s*mod\s+([a-zA-Z0-9_-]+)\s+['\"]([^'\"]+)['\"]")
    recipe_re = re.compile(r"^([a-zA-Z0-9_-]+)(?:\s+([^:]+))?:")

    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            comment = stripped.lstrip("#").strip()
            if comment and not comment.startswith(("SPDX-", "Copyright")):
                pending_doc = comment
            continue

        m_mod = mod_re.match(stripped)
        if m_mod:
            sub_name, sub_rel = m_mod.groups()
            sub_path = (path.parent / sub_rel).resolve()
            if sub_path.is_file():
                sub_prefix = f"{prefix}{sub_name}::"
                recipes.extend(_parse_justfile(sub_path, sub_prefix))
            pending_doc = ""
            continue

        m_rec = recipe_re.match(stripped)
        if m_rec:
            r_name = m_rec.group(1)
            namepath = f"{prefix}{r_name}" if prefix else r_name
            recipes.append(
                RecipeRecord(
                    name=r_name,
                    namepath=namepath,
                    doc=pending_doc,
                    params=m_rec.group(2) or "",
                )
            )
            pending_doc = ""
        elif stripped:
            pending_doc = ""
    return recipes


def get_just_recipes() -> list[RecipeRecord]:
    """Discover all recipes exposed by Just across root and submodules."""
    return _parse_justfile(REPO_ROOT / "justfile")


def get_host_apps() -> list[AppInfo]:
    """Discover host-side CMake applications."""
    apps: list[AppInfo] = []
    host_dir = REPO_ROOT / "apps" / "host"
    if not host_dir.is_dir():
        return apps
    for cmake_file in sorted(host_dir.rglob("CMakeLists.txt")):
        app_dir = cmake_file.parent
        name = app_dir.name
        desc = _parse_desc(str(app_dir)) or f"Host application {name}"
        apps.append(
            AppInfo(
                name=name,
                group="host",
                dir=str(app_dir),
                rel_dir=str(app_dir.relative_to(REPO_ROOT)),
                desc=desc,
                full_id=f"host::{name}",
            )
        )
    return apps


def get_all_apps() -> tuple[list[AppInfo], list[AppInfo]]:
    """Discover standalone board/host apps and firmware examples."""
    board_and_host: list[AppInfo] = []
    examples: list[AppInfo] = []

    for a in get_apps():
        full_id = app_id(a)
        desc = a["desc"] or f"Firmware app {a['name']}"
        record = AppInfo(
            name=a["name"],
            group=a["group"],
            dir=a["dir"],
            rel_dir=a["rel_dir"],
            desc=desc,
            full_id=full_id,
        )
        if "board/stand_alone" in a["group"]:
            board_and_host.append(record)
        else:
            examples.append(record)

    board_and_host.extend(get_host_apps())
    return board_and_host, examples


def get_libraries() -> list[LibraryRecord]:
    """Discover first-party, shared, and vendored libraries."""
    libs: list[LibraryRecord] = []
    for lib_dir in _library_dirs():
        name = lib_dir.name
        brief = _lib_brief(lib_dir)
        rel_path = str(lib_dir.relative_to(REPO_ROOT))
        group = "firmware"
        if "third_party" in lib_dir.parts:
            group = "third_party"
        elif "shared_libs" in lib_dir.parts:
            group = "shared"
        libs.append(
            LibraryRecord(
                name=name,
                group=group,
                path=rel_path,
                desc=brief or f"Library {name}",
            )
        )
    return libs


def _extract_brief_from_file(path: Path) -> str:
    """Extract @brief description or top docstring comment from a source file."""
    try:
        with path.open(encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                match = re.search(r"@brief\s+(.*)", line)
                if match:
                    return match.group(1).strip().rstrip("*/").strip()
    except OSError:
        pass
    return ""


def get_tests() -> list[TestRecord]:
    """Discover unit and integration test source files across the repository."""
    tests: list[TestRecord] = []
    search_roots = [
        REPO_ROOT / "tests",
        REPO_ROOT / "apps",
        REPO_ROOT / "tools",
    ]
    for root in search_roots:
        if not root.is_dir():
            continue
        for suffix in TEST_SUFFIXES:
            for path in sorted(root.rglob(f"test_*.{suffix}")):
                if "build" in path.parts:
                    continue
                rel_path = str(path.relative_to(REPO_ROOT))
                category = path.parent.name
                if category in ("src", "inc", "tests"):
                    category = path.parents[1].name
                desc = _extract_brief_from_file(path)
                tests.append(
                    TestRecord(
                        name=path.stem,
                        path=rel_path,
                        category=category,
                        desc=desc,
                    )
                )
    return tests


def get_ci_gates() -> list[GateRecord]:
    """Parse registered CI quality gates from scripts/ci.sh."""
    gates: list[GateRecord] = []
    ci_sh = REPO_ROOT / "scripts" / "ci.sh"
    if not ci_sh.is_file():
        return gates
    content = ci_sh.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r"RA8_GATE_REGISTRY=\s*\(\n(.*?)\n\s*\)", content, re.DOTALL)
    if match:
        for line in match.group(1).splitlines():
            cleaned = line.strip().strip('"').strip("'")
            if not cleaned or cleaned.startswith("#"):
                continue
            parts = cleaned.split("|")
            if len(parts) >= GATE_RECORD_FIELD_COUNT:
                gates.append(
                    GateRecord(
                        name=parts[0].strip(),
                        speed=parts[1].strip(),
                        desc=parts[2].strip(),
                    )
                )
    return gates


def get_tools() -> list[ToolRecord]:
    """Discover developer host tools under tools/."""
    tools: list[ToolRecord] = []
    tools_dir = REPO_ROOT / "tools"
    if not tools_dir.is_dir():
        return tools
    for item in sorted(tools_dir.iterdir()):
        if not item.is_dir() or item.name.startswith((".", "_")) or item.name == "build":
            continue
        desc = ""
        cmake_file = item / "CMakeLists.txt"
        if cmake_file.is_file():
            desc = _parse_desc(item)
        if not desc:
            readme = item / "README.md"
            if readme.is_file():
                for line in readme.read_text(encoding="utf-8", errors="ignore").splitlines():
                    s = line.strip()
                    if s and not s.startswith(("#", "<", "=", "Copyright", "SPDX")):
                        desc = s[:80]
                        break
        tools.append(
            ToolRecord(
                name=item.name,
                path=str(item.relative_to(REPO_ROOT)),
                desc=desc or f"Host developer tool {item.name}",
            )
        )
    return tools


def _matches(query: str, *fields: str) -> bool:
    """Return True if all whitespace-delimited tokens in query appear in fields."""
    text = " ".join(fields).lower()
    query_lower = query.lower()
    if query_lower in text:
        return True
    tokens = query_lower.split()
    return bool(tokens and all(token in text for token in tokens))


def _print_commands(recipes: list[RecipeRecord]) -> None:
    """Render matched Just command recipes."""
    if not recipes:
        return
    print(f"COMMANDS ({len(recipes)}):")
    for r in recipes:
        cmd = f"just {r['namepath']}"
        if r["params"]:
            cmd = f"{cmd} {r['params']}"
        print(f"  {cmd}")
        if r["doc"]:
            print(f"    {r['doc']}")
    print()


def _print_apps(apps: list[AppInfo]) -> None:
    """Render matched standalone board and host applications."""
    if not apps:
        return
    print(f"APPS ({len(apps)}):")
    for a in apps:
        print(f"  {a['desc']} ({a['full_id']})")
        if a["group"] == "host":
            print(f"    just apps::host::build {a['name']}")
            print(f"    just apps::host::run {a['name']}")
        else:
            print(f"    just apps::build {a['name']}")
            print(f"    just apps::hardware::flash {a['name']}")
            print(f"    just apps::emulator::run {a['name']}")
    print()


def _print_examples(examples: list[AppInfo]) -> None:
    """Render matched firmware examples."""
    if not examples:
        return
    print(f"EXAMPLES ({len(examples)}):")
    for a in examples:
        print(f"  {a['desc']} ({a['full_id']})")
        print(f"    just apps::build {a['name']}")
        print(f"    just apps::hardware::flash {a['name']}")
        print(f"    just apps::emulator::run {a['name']}")
    print()


def _print_libraries(libs: list[LibraryRecord]) -> None:
    """Render matched firmware, shared, and vendored libraries."""
    if not libs:
        return
    print(f"LIBRARIES ({len(libs)}):")
    for lib in libs:
        print(f"  {lib['name']} ({lib['path']})")
        if lib["desc"]:
            print(f"    {lib['desc']}")
        print(f"    just libs::search {lib['name']}")
    print()


def _print_tests(tests: list[TestRecord]) -> None:
    """Render matched unit and integration test targets."""
    if not tests:
        return
    print(f"TESTS ({len(tests)}):")
    for t in tests:
        suffix = f" - {t['desc']}" if t["desc"] else ""
        print(f"  {t['name']} ({t['path']}){suffix}")
        print(f"    just tests::local {t['category']}")
    print()


def _print_gates(gates: list[GateRecord]) -> None:
    """Render matched CI quality gates."""
    if not gates:
        return
    print(f"CI GATES ({len(gates)}):")
    for g in gates:
        print(f"  {g['name']} ({g['speed']}) - {g['desc']}")
        print(f"    just quality::gate::run {g['name']}")
        print(f"    just quality::local::gate {g['name']}")
    print()


def _print_tools(tools: list[ToolRecord]) -> None:
    """Render matched host developer tools."""
    if not tools:
        return
    print(f"TOOLS ({len(tools)}):")
    for tool in tools:
        print(f"  {tool['name']} ({tool['path']}) - {tool['desc']}")
        print(f"    just tools::build_one {tool['name']}")
    print()


def _filter_commands(query: str, recipes: list[RecipeRecord]) -> list[RecipeRecord]:
    """Filter recipes matching query while skipping boilerplate default help menus."""
    filtered: list[RecipeRecord] = []
    query_lower = query.lower()
    for r in recipes:
        if (
            r["name"] == "default"
            and "show " in r["doc"].lower()
            and "help" in r["doc"].lower()
            and query_lower != "default"
        ):
            continue
        if r["namepath"] == "search" and query_lower != "search":
            continue
        if _matches(query, r["name"], r["namepath"], r["doc"], r["params"]):
            filtered.append(r)
    return filtered


def search_all(query: str) -> SearchResults:
    """Query across all supported repository target categories."""
    commands = _filter_commands(query, get_just_recipes())
    board_apps, examples = get_all_apps()

    matched_apps = [
        a for a in board_apps if _matches(query, a["name"], a["group"], a["desc"], a["full_id"])
    ]
    matched_examples = [
        a for a in examples if _matches(query, a["name"], a["group"], a["desc"], a["full_id"])
    ]
    matched_libs = [
        lib
        for lib in get_libraries()
        if _matches(query, lib["name"], lib["path"], lib["desc"], lib["group"])
    ]
    matched_tests = [
        t for t in get_tests() if _matches(query, t["name"], t["path"], t["category"], t["desc"])
    ]
    matched_gates = [g for g in get_ci_gates() if _matches(query, g["name"], g["speed"], g["desc"])]
    matched_tools = [
        tool for tool in get_tools() if _matches(query, tool["name"], tool["path"], tool["desc"])
    ]

    return {
        "commands": matched_commands(commands),
        "apps": matched_apps,
        "examples": matched_examples,
        "libraries": matched_libs,
        "tests": matched_tests,
        "gates": matched_gates,
        "tools": matched_tools,
    }


def matched_commands(commands: list[RecipeRecord]) -> list[RecipeRecord]:
    """Deduplicate commands while preserving order."""
    seen: set[str] = set()
    deduped: list[RecipeRecord] = []
    for cmd in commands:
        if cmd["namepath"] not in seen:
            seen.add(cmd["namepath"])
            deduped.append(cmd)
    return deduped


def print_search_results(query: str, results: SearchResults) -> None:
    """Render all category sections with match counts and suggested commands."""
    total = sum(len(items) for items in results.values())
    if total == 0:
        print(f"No results found for '{query}'.")
        return

    print(f"Search Results for '{query}' ({total} matches):\n")
    _print_commands(results["commands"])
    _print_apps(results["apps"])
    _print_examples(results["examples"])
    _print_libraries(results["libraries"])
    _print_tests(results["tests"])
    _print_gates(results["gates"])
    _print_tools(results["tools"])


def selftest() -> int:
    """Verify that unified search discovers known targets in both directions."""
    failures: list[str] = []

    dash_results = search_all("dashboard")
    found_dash = any(
        isinstance(r, dict) and r.get("namepath") == "infra::remote::dashboard"
        for r in dash_results["commands"]
    )
    if not found_dash:
        failures.append("query 'dashboard' did not find recipe 'infra::remote::dashboard'")

    mcdc_results = search_all("mcdc")
    found_mcdc_cmd = any(
        isinstance(r, dict) and "mcdc" in str(r.get("namepath")) for r in mcdc_results["commands"]
    )
    found_mcdc_gate = any(
        isinstance(g, dict) and str(g.get("name")) == "mcdc" for g in mcdc_results["gates"]
    )
    if not found_mcdc_cmd or not found_mcdc_gate:
        failures.append("query 'mcdc' did not find mcdc recipe or gate")

    empty_results = search_all("nonexistent_unique_token_xyz_9999")
    total_empty = sum(len(items) for items in empty_results.values())
    if total_empty != 0:
        failures.append(f"nonexistent query returned {total_empty} unexpected matches")

    if failures:
        for f in failures:
            print(f"  [FAIL] {f}", file=sys.stderr)
        return 1
    print("search.py --selftest: PASS (commands, gates, deduplication, fail-closed non-matches)")
    return 0


def main() -> int:
    """Execute unified search across all repository target classes."""
    if len(sys.argv) < MIN_ARGUMENTS or not sys.argv[1].strip():
        print("Usage: just search <keyword>")
        return 1

    if sys.argv[1] == "--selftest":
        return selftest()

    query = " ".join(sys.argv[1:]).strip()
    results = search_all(query)
    print_search_results(query, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())

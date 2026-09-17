#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""App discovery and resolution helper for ra8-firmware."""

import argparse
import os
import re
import sys
import tempfile
from collections import defaultdict
from collections.abc import Iterator
from pathlib import Path
from typing import TypedDict
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
MIN_LIST_COLUMN_WIDTH = 40


class AppRecord(TypedDict):
    """One discovered firmware application's canonical metadata."""

    name: str
    group: str
    dir: str
    rel_dir: str
    desc: str
    toolchain: str


class BuildConfig(TypedDict):
    """One canonical cross-build configuration for a firmware application."""

    id: str
    app: AppRecord
    variant: str
    cmake_args: tuple[str, ...]
    build_suffix: str


DEFAULT_BUILD_VARIANT = "default"
EREADER_NS_XIP_VARIANT = "ns-xip"


def _parse_desc(dirpath: str | Path) -> str:
    """Extract DESCRIPTION text from CMakeLists.txt or src/main.c @brief."""
    app_dir = Path(dirpath)
    cmake_path = app_dir / "CMakeLists.txt"
    if cmake_path.is_file():
        content = cmake_path.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r'DESCRIPTION\s+"([^"]+)"', content, re.MULTILINE)
        if match:
            return match.group(1).strip()

    main_path = app_dir / "src" / "main.c"
    if main_path.is_file():
        content = main_path.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r"@brief\s+([^\n*]+)", content)
        if match:
            desc = match.group(1).strip()
            if desc and desc != "Main entry point.":
                return desc
    return ""


def _relative_group(app_dir: Path, root: Path) -> str:
    """Return the slash-separated parent group below a discovery root."""
    parent = app_dir.relative_to(root).parent
    return "" if parent == Path() else parent.as_posix()


def _iter_cmake_dirs(root: Path) -> Iterator[Path]:
    """Yield every directory under ``root`` that holds a ``CMakeLists.txt``.

    Build output is pruned from the descent. A ``build`` directory holds only
    CMake's own generated listfiles -- never a discoverable application -- and
    scripts/builders/all_examples.sh runs a pool of concurrent per-app builds
    that create and delete directories underneath exactly those trees while
    discovery is walking them. A plain ``rglob`` therefore raced: ``os.scandir``
    raised ``FileNotFoundError`` on a directory that had just vanished and
    aborted discovery for the whole tree, failing one arbitrary app per
    cross-build run. Pruning removes the race without changing the result set.

    Directory names are sorted so the walk order is stable across runs.
    """
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(name for name in dirnames if name != "build")
        if "CMakeLists.txt" in filenames:
            yield Path(dirpath)


def _get_example_apps() -> list[AppRecord]:
    """Discover firmware examples with real CMake and main source files."""
    apps: list[AppRecord] = []
    examples_dir = REPO_ROOT / "examples"
    if not examples_dir.is_dir():
        return apps
    for app_dir in _iter_cmake_dirs(examples_dir):
        if not (app_dir / "src" / "main.c").is_file():
            continue
        group = _relative_group(app_dir, examples_dir)
        if group.startswith("shared") or not group:
            continue
        toolchain = (
            "cmake/toolchain-ra8p1.cmake" if "ra8p1" in group else "cmake/toolchain-ra8d2.cmake"
        )
        apps.append(
            {
                "name": app_dir.name,
                "group": group,
                "dir": str(app_dir),
                "rel_dir": str(app_dir.relative_to(REPO_ROOT)),
                "desc": _parse_desc(app_dir),
                "toolchain": toolchain,
            }
        )
    return apps


def _get_board_apps() -> list[AppRecord]:
    """Discover standalone board applications."""
    apps: list[AppRecord] = []
    board_dir = REPO_ROOT / "apps" / "board" / "stand_alone"
    if not board_dir.is_dir():
        return apps
    for app_dir in _iter_cmake_dirs(board_dir):
        if not (app_dir / "src" / "main.c").is_file():
            continue
        name = "ra8d2-ereader" if app_dir.name == "ereader" else app_dir.name
        apps.append(
            {
                "name": name,
                "group": "board/stand_alone",
                "dir": str(app_dir),
                "rel_dir": str(app_dir.relative_to(REPO_ROOT)),
                "desc": _parse_desc(app_dir) or "E-reader product firmware",
                "toolchain": "cmake/toolchain-ra8d2.cmake",
            }
        )
    return apps


def get_apps() -> list[AppRecord]:
    """Discover all firmware applications in examples/ and apps/board/."""
    apps = _get_example_apps() + _get_board_apps()
    apps.sort(key=lambda app: (app["group"], app["name"]))
    return apps


def app_id(app: AppRecord) -> str:
    """Return the stable namespaced identifier for one discovered app."""
    return f"{app['group'].replace('/', '::')}::{app['name']}"


def build_configs(app: AppRecord) -> list[BuildConfig]:
    """Return every supported cross-build configuration for ``app``."""
    configs: list[BuildConfig] = [
        {
            "id": app_id(app),
            "app": app,
            "variant": DEFAULT_BUILD_VARIANT,
            "cmake_args": (),
            "build_suffix": "build",
        }
    ]
    if app["rel_dir"] == "apps/board/stand_alone/ereader":
        configs.append(
            {
                "id": f"{app_id(app)}@{EREADER_NS_XIP_VARIANT}",
                "app": app,
                "variant": EREADER_NS_XIP_VARIANT,
                "cmake_args": ("-DRA8_EREADER_NS_XIP=ON",),
                "build_suffix": "build-ns-xip",
            }
        )
    return configs


def get_build_configs() -> list[BuildConfig]:
    """Return the stable canonical cross-build configuration matrix."""
    return sorted(
        (config for app in get_apps() for config in build_configs(app)),
        key=lambda config: config["id"],
    )


def find_build_config(selector: str) -> BuildConfig | None:
    """Resolve an app selector with an optional ``@variant`` suffix."""
    app_selector, separator, variant = selector.rpartition("@")
    if not separator:
        app_selector = selector
        variant = DEFAULT_BUILD_VARIANT
    app = find_app(app_selector)
    if app is None:
        return None
    return next((config for config in build_configs(app) if config["variant"] == variant), None)


def find_app(name: str) -> AppRecord | None:
    """Find an app by unique short name, exact identifier, or board alias.

    A short name that becomes ambiguous deliberately resolves to nothing. The
    caller must then use the namespaced identifier; silently choosing the first
    walk result would build or flash the wrong firmware.
    """
    apps = get_apps()
    query = name.replace("/", "::")
    exact_ids = [app for app in apps if app_id(app) == query]
    if len(exact_ids) == 1:
        return exact_ids[0]
    if query == "ereader":
        return next((app for app in apps if app["name"] == "ra8d2-ereader"), None)
    short_matches = [app for app in apps if app["name"] == query]
    if len(short_matches) == 1:
        return short_matches[0]
    suffix_matches = [app for app in apps if app_id(app).endswith(f"::{query}")]
    return suffix_matches[0] if len(suffix_matches) == 1 else None


def _filter_apps(apps: list[AppRecord], query: str, group: str | None) -> list[AppRecord]:
    """Filter applications by query or group while retaining stable order."""
    if query:
        normalized = query.lower().replace("/", "::")
        return [
            app
            for app in apps
            if normalized in app_id(app).lower()
            or normalized.replace("::", "/") in app["group"].lower()
            or normalized in app["desc"].lower()
            or normalized in app["name"].lower()
        ]
    if group:
        group_slashes = group.lower().replace("::", "/")
        return [
            app
            for app in apps
            if group_slashes in app["group"].lower()
            or group.lower() in app["group"].replace("/", "::").lower()
        ]
    return apps


def _app_count(count: int) -> str:
    """Format an application count without hardcoded singular/plural wording."""
    return f"{count} {'app' if count == 1 else 'apps'}"


def _print_category_summary(apps: list[AppRecord]) -> None:
    """Print the stable category help summary."""
    groups: defaultdict[str, int] = defaultdict(int)
    for app in apps:
        groups[app["group"]] += 1

    print(f"FIRMWARE APPS ({len(apps)} discovered)\n")
    print("USAGE:")
    print("  just search <keyword>                Search apps by keyword or name")
    print(f"  just apps::example::list             List all {len(apps)} firmware apps\n")
    print("CATEGORY FILTERS:")
    categories = (
        ("just apps::filter::hil", "Automated HIL hardware tests", "ek_ra8d2/hw_validated/hil"),
        (
            "just apps::filter::manual",
            "Interactive board demos & LCD screens",
            "ek_ra8d2/hw_validated/manual",
        ),
        (
            "just apps::filter::c6",
            "ESP32-C6 wireless co-processor demos",
            "ek_ra8d2/hw_validated/c6",
        ),
        (
            "just apps::filter::stand_alone",
            "Standalone E-Reader product firmware",
            "board/stand_alone",
        ),
        (
            "just apps::filter::ra8p1",
            "RA8P1 Cortex-M85 + NPU foundation apps",
            "ra8p1_foundation",
        ),
        (
            "just apps::filter::pending",
            "Hardware validation pending queue",
            "ek_ra8d2/hw_pending",
        ),
        (
            "just apps::filter::pending_c6",
            "ESP32-C6 bring-up pending queue",
            "ek_ra8d2/hw_pending/c6",
        ),
        (
            "just apps::filter::pending_manual",
            "Manual validation pending queue",
            "ek_ra8d2/hw_pending/manual",
        ),
        (
            "just apps::filter::revalidation",
            "HIL revalidation queue",
            "ek_ra8d2/hil_needs_revalidation",
        ),
        ("just apps::filter::unsupported", "Unsupported legacy apps", "_unsupported"),
    )
    for command, description, group in categories:
        print(f"  {command:<35} {description} ({_app_count(groups[group])})")


def cmd_list(args: argparse.Namespace) -> int:
    """List all applications or a filtered selection."""
    apps = get_apps()
    query = (args.query or args.filter or "").strip()
    if query and query.lower() == "all":
        args.all = True
        query = ""
    apps = _filter_apps(apps, query, args.group)

    if not args.all and not query and not args.group:
        _print_category_summary(apps)
        return 0

    lines = [
        f"== FIRMWARE APPS ({len(apps)}) -- build: just apps::build <app> | "
        "flash: just apps::hardware::flash <app> | "
        "run: just apps::emulator::run <app>\n"
    ]
    formatted = [(app_id(app), app["desc"]) for app in apps]
    max_len = max((len(identifier) for identifier, _ in formatted), default=0)
    column_width = max(max_len + 2, MIN_LIST_COLUMN_WIDTH)
    lines.extend(
        f"  {identifier.ljust(column_width)} {description}" for identifier, description in formatted
    )
    print("\n".join(lines))
    return 0


def cmd_dir(args: argparse.Namespace) -> int:
    """Print the repository-relative directory for one application."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app '{args.app}' not found", file=sys.stderr)
        return 1
    print(config["app"]["rel_dir"])
    return 0


def cmd_toolchain(args: argparse.Namespace) -> int:
    """Print the CMake toolchain file for one application."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app '{args.app}' not found", file=sys.stderr)
        return 1
    print(config["app"]["toolchain"])
    return 0


def cmd_name(args: argparse.Namespace) -> int:
    """Print the canonical artifact basename for an app identifier."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app '{args.app}' not found or is ambiguous", file=sys.stderr)
        return 1
    print(config["app"]["name"])
    return 0


def cmd_id(args: argparse.Namespace) -> int:
    """Print the stable namespaced identifier for an app selector."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app '{args.app}' not found or is ambiguous", file=sys.stderr)
        return 1
    print(config["id"])
    return 0


def cmd_build_dir(args: argparse.Namespace) -> int:
    """Print the repository-relative build directory for one configuration."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app configuration '{args.app}' not found", file=sys.stderr)
        return 1
    print(Path(config["app"]["rel_dir"]) / config["build_suffix"])
    return 0


def cmd_cmake_args(args: argparse.Namespace) -> int:
    """Write one configuration's additional CMake arguments as NUL records."""
    config = find_build_config(args.app)
    if not config:
        print(f"Error: app configuration '{args.app}' not found", file=sys.stderr)
        return 1
    for argument in config["cmake_args"]:
        sys.stdout.buffer.write(argument.encode("ascii") + b"\0")
    return 0


def cmd_matrix(args: argparse.Namespace) -> int:
    """Print every canonical cross-build configuration."""
    separator = b"\0" if args.nul else b"\n"
    for config in get_build_configs():
        sys.stdout.buffer.write(config["id"].encode("ascii") + separator)
    return 0


def _selftest_write_app(root: Path, relative: str, *, main: bool = True) -> None:
    """Create one minimal discoverable application fixture."""
    app_dir = root / relative
    app_dir.mkdir(parents=True, exist_ok=True)
    (app_dir / "CMakeLists.txt").write_text(
        'set(RA8_APP_NAME fixture)\nset(DESCRIPTION "Fixture app")\n',
        encoding="ascii",
    )
    if main:
        src = app_dir / "src"
        src.mkdir()
        (src / "main.c").write_text("int main(void) { return 0; }\n", encoding="ascii")


def _selftest_discovery(root: Path, failures: list[str]) -> None:
    """Check complete discovery, build variants, and ignored partial trees."""
    _selftest_write_app(root, "examples/tier/alpha")
    _selftest_write_app(root, "examples/tier/partial", main=False)
    _selftest_write_app(root, "examples/tier/alpha/build/ghost")
    _selftest_write_app(root, "apps/board/stand_alone/ereader")
    identifiers = [app_id(app) for app in get_apps()]
    expected = ["board::stand_alone::ra8d2-ereader", "tier::alpha"]
    if identifiers != expected:
        failures.append(f"discovery returned {identifiers!r}, expected {expected!r}")
    configs = [config["id"] for config in get_build_configs()]
    expected_configs = [
        "board::stand_alone::ra8d2-ereader",
        "board::stand_alone::ra8d2-ereader@ns-xip",
        "tier::alpha",
    ]
    if configs != expected_configs:
        failures.append(f"configuration matrix returned {configs!r}")


def _selftest_resolution(root: Path, failures: list[str]) -> None:
    """Check unique selectors resolve and ambiguous or invalid ones fail closed."""
    _selftest_write_app(root, "examples/other/alpha")
    if find_app("tier::alpha") is None or find_build_config("ereader@ns-xip") is None:
        failures.append("namespaced or board-alias selector did not resolve")
    if find_app("alpha") is not None:
        failures.append("ambiguous short selector did not fail closed")
    if find_build_config("tier::alpha@missing") is not None:
        failures.append("unknown build variant did not fail closed")
    if find_build_config("does-not-exist") is not None:
        failures.append("unknown application did not fail closed")


def _selftest_pruned_walk(root: Path, failures: list[str]) -> None:
    """Prove transient build directories are pruned before descent."""
    pruned = False

    def scripted_walk(_root: Path) -> Iterator[tuple[str, list[str], list[str]]]:
        nonlocal pruned
        dirnames = ["build", "stable"]
        yield str(root), dirnames, []
        pruned = "build" not in dirnames
        yield str(root / "stable"), [], ["CMakeLists.txt"]

    with patch.object(os, "walk", scripted_walk):
        found = list(_iter_cmake_dirs(root))
    if not pruned:
        failures.append("build output was not pruned before descent")
    if found != [root / "stable"]:
        failures.append(f"pruned walk returned {found!r}")


def selftest() -> int:
    """Exercise discovery and selector contracts in both directions."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-apps-") as temp:
        root = Path(temp)
        with patch(f"{__name__}.REPO_ROOT", root):
            _selftest_discovery(root, failures)
            _selftest_resolution(root, failures)
            _selftest_pruned_walk(root, failures)
    if failures:
        for failure in failures:
            print(f"  [FAIL] {failure}", file=sys.stderr)
        return 1
    print("ra8_apps.py --selftest: PASS (discovery, variants, pruning, fail-closed selectors)")
    return 0


def main() -> int:
    """Dispatch the app discovery command-line interface."""
    parser = argparse.ArgumentParser(description="ra8 app discovery helper")
    parser.add_argument("--selftest", action="store_true")
    subparsers = parser.add_subparsers(dest="command")

    list_parser = subparsers.add_parser("list")
    list_parser.add_argument("query", nargs="?", default="")
    list_parser.add_argument("--all", "-a", action="store_true")
    list_parser.add_argument("--group", "-g")
    list_parser.add_argument("--filter", "-f")
    list_parser.set_defaults(func=cmd_list)

    for command, function in (
        ("dir", cmd_dir),
        ("toolchain", cmd_toolchain),
        ("name", cmd_name),
        ("id", cmd_id),
        ("build-dir", cmd_build_dir),
        ("cmake-args", cmd_cmake_args),
    ):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("app")
        command_parser.set_defaults(func=function)

    matrix_parser = subparsers.add_parser("matrix")
    matrix_parser.add_argument("--nul", action="store_true")
    matrix_parser.set_defaults(func=cmd_matrix)

    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if hasattr(args, "func"):
        return args.func(args)
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

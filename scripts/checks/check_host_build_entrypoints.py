#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Keep native Just builds on the C23 compiler and complete tool registry."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPILED = frozenset({".c", ".cc", ".cpp", ".cxx", ".m", ".mm"})
RECIPE = re.compile(r"^([A-Za-z_][A-Za-z0-9_-]*)(?:\s+[^:]*)?:\s*$")
RAW_CMAKE_CONFIGURE = re.compile(r"(?:^|\s)cmake\s+(?!--(?:build|install)\b)")
RAW_COMPILER = re.compile(
    r"(?:^|\s)(?:\$\{?cc\}?|\$cc|cc|gcc(?:-[0-9]+)?|clang(?:-[0-9]+)?)\s+"
    r"[^\n]*(?:-std=|(?:^|\s)-c(?:\s|$)|(?:^|\s)-o(?:\s|$))"
)


def _recipe_bodies(text: str) -> list[tuple[str, str]]:
    """Return recipe names and bodies from one Just module."""
    bodies: list[tuple[str, str]] = []
    name = ""
    lines: list[str] = []
    for line in text.splitlines():
        match = RECIPE.match(line)
        if match:
            if name:
                bodies.append((name, "\n".join(lines)))
            name, lines = match.group(1).strip(), []
        elif name and (line.startswith((" ", "\t")) or not line.strip()):
            if not line.lstrip().startswith("#"):
                lines.append(line)
    if name:
        bodies.append((name, "\n".join(lines)))
    return bodies


def _shell_commands(body: str) -> list[str]:
    """Join shell continuation lines into configure-command units."""
    commands: list[str] = []
    current = ""
    for raw in body.splitlines():
        line = raw.strip()
        current = f"{current} {line}".strip()
        if current.endswith("\\"):
            current = current[:-1].rstrip()
        elif current:
            commands.append(current)
            current = ""
    if current:
        commands.append(current)
    return commands


def _recipe_errors(label: str, text: str) -> list[str]:
    """Reject raw native CMake configure and compile-driver bodies."""
    errors: list[str] = []
    for name, body in _recipe_bodies(text):
        errors.extend(
            f"{label}: recipe {name}: raw native CMake bypasses host_cmake.sh"
            for command in _shell_commands(body)
            if RAW_CMAKE_CONFIGURE.search(command) and "CMAKE_TOOLCHAIN_FILE" not in command
        )
        if RAW_COMPILER.search(body):
            errors.append(f"{label}: recipe {name}: raw host compiler invocation bypasses CMake")
    return errors


def _just_files(root: Path) -> list[Path]:
    """Discover the root Just entry point and every module."""
    return [root / "justfile", *sorted((root / "just").glob("*.just"))]


def _compiled_tools(root: Path) -> set[str]:
    """Discover tool roots containing authored compiled implementation."""
    found: set[str] = set()
    tools = root / "tools"
    for path in tools.glob("*/src/**/*"):
        if path.is_file() and path.suffix in COMPILED:
            found.add(path.relative_to(tools).parts[0])
    return found


def _cmake_tools(root: Path) -> set[str]:
    """Discover tool roots managed by the CMake dispatcher."""
    return {path.parent.name for path in (root / "tools").glob("*/CMakeLists.txt")}


def _standalone_cmake(text: str) -> bool:
    """Whether a listfile declares itself as a top-level CMake project."""
    return bool(re.search(r"^\s*project\s*\(", text, flags=re.MULTILINE))


def _shared_dispatch_errors(root: Path) -> list[str]:
    """Ensure shared consumer fragments cannot be configured vacuously."""
    just_text = (root / "just" / "shared.just").read_text(encoding="utf-8")
    dispatcher = root / "scripts" / "builders" / "build_shared_libs.sh"
    errors = []
    if 'build_shared_libs.sh "{{ lib }}"' not in just_text:
        errors.append("just/shared.just does not delegate to the shared-library dispatcher")
    text = dispatcher.read_text(encoding="utf-8") if dispatcher.is_file() else ""
    errors.extend(
        f"shared-library dispatcher lacks contract: {required}"
        for required in ("is_standalone", "host_cmake.sh", "apps::shared::test")
        if required not in text
    )
    return errors


def _inventory_errors(root: Path) -> list[str]:
    """Require every compiled tool to join the discovery-managed registry."""
    missing = sorted(_compiled_tools(root) - _cmake_tools(root))
    return [f"tools/{name}: compiled tool has no CMakeLists.txt" for name in missing]


def _dispatcher_errors(root: Path, listed: set[str]) -> list[str]:
    """Check discovery output and the Just/wrapper contracts."""
    expected = _cmake_tools(root)
    errors = [f"tool dispatcher omits {name}" for name in sorted(expected - listed)]
    errors += [f"tool dispatcher invents {name}" for name in sorted(listed - expected)]
    tools_just = (root / "just" / "tools.just").read_text(encoding="utf-8")
    for required in (
        'build tool="all":',
        'clean tool="all":',
        'build_host_tools.sh build "{{ tool }}"',
        'build_host_tools.sh clean "{{ tool }}"',
    ):
        if required not in tools_just:
            errors.append(f"just/tools.just lacks discovery contract: {required}")
    wrapper = (root / "scripts" / "builders" / "host_cmake.sh").read_text(encoding="utf-8")
    for required in (
        "ra8_select_host_compiler",
        "ra8_select_emulator_compiler",
        "ra8_cmake_reset_if_incompatible",
        "-DCMAKE_C_COMPILER=",
        "-DCMAKE_CXX_COMPILER=",
    ):
        if required not in wrapper:
            errors.append(f"host_cmake.sh lacks compiler/cache contract: {required}")
    dispatcher = (root / "scripts" / "builders" / "build_host_tools.sh").read_text(encoding="utf-8")
    for required in ("clean_one", '"$dir/cache_bench"', '"$dir/miniz_host.o"', '"$dir"/*.trace'):
        if required not in dispatcher:
            errors.append(f"tool dispatcher lacks legacy-clean contract: {required}")
    return errors


def _live_dispatch(root: Path) -> tuple[set[str], list[str]]:
    """Run the read-only dispatcher list mode."""
    script = root / "scripts" / "builders" / "build_host_tools.sh"
    proc = subprocess.run(  # noqa: S603 -- fixed repository script, list-only mode
        [str(script), "list"], text=True, capture_output=True, check=False
    )
    if proc.returncode:
        return set(), [f"tool dispatcher list failed: {proc.stderr.strip()}"]
    return set(proc.stdout.splitlines()), []


def _selftest() -> int:
    """Prove raw builds and registry omissions fire while legal forms stay quiet."""
    failures: list[str] = []
    good = "build:\n    bash scripts/builders/host_cmake.sh tools/x tools/x/build\n"
    cross = """build:
    cmake -S x -B b -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake
    cmake --build b
"""
    bad_cmake = "build:\n    cmake -S tools/x -B tools/x/build\n"
    mixed_cmake = """build:
    cmake -S arm -B arm/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake
    cmake -S tools/x -B tools/x/build
"""
    bad_cc = "build:\n    cc -std=gnu23 src/main.c -o tool\n"
    if _recipe_errors("good", good) or _recipe_errors("cross", cross):
        failures.append("wrapper or ARM-toolchain fixture was rejected")
    if not _recipe_errors("bad-cmake", bad_cmake):
        failures.append("raw native CMake fixture was accepted")
    if not _recipe_errors("mixed-cmake", mixed_cmake):
        failures.append("raw native CMake hidden beside an ARM configure was accepted")
    if not _recipe_errors("bad-cc", bad_cc):
        failures.append("raw compiler fixture was accepted")
    if _standalone_cmake("target_sources(app PRIVATE src/x.c)\n"):
        failures.append("consumer CMake fragment was classified as standalone")
    if not _standalone_cmake("project(shared LANGUAGES C)\n"):
        failures.append("standalone shared CMake project was classified as a fragment")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "just").mkdir()
        (root / "justfile").write_text("default:\n    true\n")
        (root / "just" / "future.just").write_text(bad_cmake)
        discovered = {path.relative_to(root) for path in _just_files(root)}
        if discovered != {Path("justfile"), Path("just/future.just")}:
            failures.append("new Just module was omitted from discovery")
        (root / "tools" / "native" / "src").mkdir(parents=True)
        (root / "tools" / "native" / "src" / "main.c").write_text("int main(void){}\n")
        if not _inventory_errors(root):
            failures.append("compiled tool without CMake was accepted")
        (root / "tools" / "native" / "CMakeLists.txt").write_text("project(native C)\n")
        if _inventory_errors(root):
            failures.append("compiled tool with CMake was rejected")
        if not _dispatcher_errors_fixture({"native"}, set()):
            failures.append("dispatcher omission was accepted")
        if _dispatcher_errors_fixture({"native"}, {"native"}):
            failures.append("complete dispatcher fixture was rejected")
    if failures:
        print("check_host_build_entrypoints.py --selftest FAILED:", file=sys.stderr)
        print("\n".join(f"  {failure}" for failure in failures), file=sys.stderr)
        return 1
    print("check_host_build_entrypoints.py --selftest: PASS (12 both-direction cases)")
    return 0


def _dispatcher_errors_fixture(expected: set[str], listed: set[str]) -> list[str]:
    """Pure set comparison used to prove dispatcher coverage both ways."""
    return [
        *(f"missing {x}" for x in expected - listed),
        *(f"extra {x}" for x in listed - expected),
    ]


def main() -> int:
    """Run selftest or the live host-build surface audit."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return _selftest()
    errors: list[str] = []
    just_files = _just_files(REPO_ROOT)
    for path in just_files:
        errors += _recipe_errors(str(path.relative_to(REPO_ROOT)), path.read_text(encoding="utf-8"))
    errors += _inventory_errors(REPO_ROOT)
    errors += _shared_dispatch_errors(REPO_ROOT)
    listed, list_errors = _live_dispatch(REPO_ROOT)
    errors += list_errors + _dispatcher_errors(REPO_ROOT, listed)
    if errors:
        print("check_host_build_entrypoints.py: host build contract violations:", file=sys.stderr)
        print("\n".join(f"  {error}" for error in errors), file=sys.stderr)
        return 1
    print(
        f"check_host_build_entrypoints.py: clean ({len(just_files)} Just files, "
        f"{len(listed)} compiled tools)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

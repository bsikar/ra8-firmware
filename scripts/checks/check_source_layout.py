#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce repository source/include/test layout for first-party code."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

SCOPED_ROOTS = frozenset({"apps", "examples", "libs", "port", "tests", "tools"})
IMPLEMENTATION_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".m"})
HEADER_SUFFIXES = frozenset({".h", ".hh", ".hpp", ".hxx"})
COMPONENT_HELPER_SUFFIXES = frozenset({".py", ".sh"})
COMPONENT_ROOTS = frozenset({"apps", "examples"})
EXCLUDED_PARTS = frozenset({"_deps", "build"})
TOOL_VENDOR_ROOT_PARTS = 3
TOOL_FILE_MIN_PARTS = 3
EXCLUDED_PREFIXES = (
    PurePosixPath("libs/ra8_fonts"),
    PurePosixPath("tools/vela/generated"),
)
APP_OWNED_VENDORS = frozenset({"libwebp", "litehtml", "miniz", "stb", "xz_embedded"})
APP_COMPRESS_FILES = frozenset(
    {
        PurePosixPath("apps/shared_libs/compress/inc/ra8_compress.h"),
        PurePosixPath("apps/shared_libs/compress/inc/ra8_vfs_compress.h"),
        PurePosixPath("apps/shared_libs/compress/src/ra8_compress.c"),
        PurePosixPath("apps/shared_libs/compress/src/ra8_vfs_compress.c"),
    }
)
LEGACY_COMPRESS_FILES = frozenset(
    {
        PurePosixPath("libs/ra8_io/inc/ra8_io_compress.h"),
        PurePosixPath("libs/ra8_io/inc/ra8_io_vfs_compress.h"),
        PurePosixPath("libs/ra8_io/src/ra8_io_compress.c"),
        PurePosixPath("libs/ra8_io/src/ra8_io_vfs_compress.c"),
    }
)


def is_vendor_path(path: PurePosixPath) -> bool:
    """Return whether ``path`` has a supported vendored-component shape.

    The SBOM gate separately requires every directory under these roots to be
    registered.  A tool-private root is deliberately narrow: only
    ``tools/<tool>/third_party/<component>`` qualifies, so a generic tools
    vendor dumping ground cannot silently appear.
    """
    parts = path.parts
    return (
        parts[:2] == ("libs", "third_party")
        or parts[:3] == ("apps", "shared_libs", "third_party")
        or (
            len(parts) >= TOOL_VENDOR_ROOT_PARTS
            and parts[0] == "tools"
            and parts[2] == "third_party"
        )
    )


def is_excluded(path: PurePosixPath) -> bool:
    """Return whether a path is vendored, generated, or build output."""
    if is_vendor_path(path) or any(path.is_relative_to(prefix) for prefix in EXCLUDED_PREFIXES):
        return True
    return any(part in EXCLUDED_PARTS or part.startswith("build-") for part in path.parts)


def layout_error(path: PurePosixPath) -> str | None:
    """Return the layout error for one repository-relative path, if any."""
    if not path.parts or path.parts[0] not in SCOPED_ROOTS or is_excluded(path):
        return None
    suffix = path.suffix.lower()
    directory_parts = path.parts[1:-1]
    if suffix in IMPLEMENTATION_SUFFIXES and "src" not in directory_parts:
        return "implementation file is not under src/"
    if suffix in HEADER_SUFFIXES and not ({"inc", "src"} & set(directory_parts)):
        return "header is not under inc/ or src/"
    return None


def tracked_paths(root: Path) -> list[PurePosixPath]:
    """Return present tracked and untracked paths without ignored build output."""
    git = shutil.which("git")
    if git is None:
        raise FileNotFoundError
    result = subprocess.run(  # noqa: S603 - resolved absolute git executable
        [git, "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    paths: list[PurePosixPath] = []
    for line in result.stdout.splitlines():
        candidate = PurePosixPath(line)
        if (root / candidate).is_file():
            paths.append(candidate)
    return paths


def vendor_ownership_errors(existing: set[PurePosixPath]) -> list[str]:
    """Reject app-owned vendors in the platform tree and removed vendor debris."""
    errors: list[str] = []
    for name in sorted(APP_OWNED_VENDORS):
        platform = PurePosixPath("libs/third_party") / name
        app = PurePosixPath("apps/shared_libs/third_party") / name
        if platform in existing:
            errors.append(f"{platform}: app-owned vendor must live at {app}")
        if app not in existing:
            errors.append(f"{app}: required app-owned vendor is missing")
    return errors


def compress_ownership_errors(existing: set[PurePosixPath]) -> list[str]:
    """Reject the retired ra8_io compression seam and require its app module."""
    errors = [
        f"{path}: compression is app-owned; remove this legacy ra8_io path"
        for path in sorted(LEGACY_COMPRESS_FILES & existing)
    ]
    errors.extend(
        f"{path}: required app-owned compression module file is missing"
        for path in sorted(APP_COMPRESS_FILES - existing)
    )
    return errors


def is_python_test_module(path: PurePosixPath) -> bool:
    """Return whether a Python filename identifies a test module."""
    return path.stem.startswith("test_") or path.stem.endswith(("_test", "_selftest"))


def python_tool_layout_errors(
    existing: set[PurePosixPath],
) -> list[tuple[PurePosixPath, str]]:
    """Reject flat multi-module tools and regressions from an adopted src layout."""
    root_modules: dict[PurePosixPath, list[PurePosixPath]] = {}
    module_counts: dict[PurePosixPath, int] = {}
    src_roots: set[PurePosixPath] = set()
    errors: list[tuple[PurePosixPath, str]] = []
    for path in sorted(existing):
        if (
            path.suffix != ".py"
            or len(path.parts) < TOOL_FILE_MIN_PARTS
            or path.parts[0] != "tools"
        ):
            continue
        if is_excluded(path):
            continue
        tool_root = PurePosixPath(*path.parts[:2])
        module_counts[tool_root] = module_counts.get(tool_root, 0) + 1
        relative = path.relative_to(tool_root)
        if len(relative.parts) == 1:
            root_modules.setdefault(tool_root, []).append(path)
        elif relative.parts[0] == "src":
            src_roots.add(tool_root)
            if is_python_test_module(path):
                errors.append((path, "Python test module is not under tests/"))

    for tool_root, modules in sorted(root_modules.items()):
        if tool_root in src_roots:
            reason = "Python module is at tool root after src/ layout was adopted"
        elif module_counts[tool_root] > 1:
            reason = "multi-module Python tool must place production modules under src/"
        else:
            continue
        errors.extend((path, reason) for path in modules)
    return errors


def component_helper_layout_errors(
    existing: set[PurePosixPath],
) -> list[tuple[PurePosixPath, str]]:
    """Reject executable helpers placed directly at app/example component roots.

    A sibling CMakeLists.txt identifies a component root without encoding the
    repository's varying app/example nesting depths. Build and generation
    helpers belong under scripts/; verification gates belong under
    tests/scripts/.
    """
    errors: list[tuple[PurePosixPath, str]] = []
    for path in sorted(existing):
        if (
            not path.parts
            or path.parts[0] not in COMPONENT_ROOTS
            or path.suffix.lower() not in COMPONENT_HELPER_SUFFIXES
            or is_excluded(path)
        ):
            continue
        if path.parent / "CMakeLists.txt" in existing:
            errors.append(
                (
                    path,
                    "component helper is at the component root; use scripts/ or tests/scripts/",
                )
            )
    return errors


def _path_layout_selftest_cases() -> dict[str, bool]:
    """Return pass/fail results for direct C-family path classification."""
    expectations = {
        "examples/board/app/main.c": True,
        "tests/hal/test_driver.c": True,
        "tools/widget/widget.h": True,
        "apps/host/widget/src/main.c": False,
        "apps/host/widget/src/widget_internal.h": False,
        "libs/widget/inc/widget.h": False,
        "tests/hal/src/test_driver.c": False,
        "tests/hal/inc/test_fixture.h": False,
        "libs/third_party/vendor.c": False,
        "apps/shared_libs/third_party/vendor.c": False,
        "tools/viewer/third_party/vendor/source.c": False,
        "tools/viewer/src/source.c": False,
        "apps/shared_libs/widget/widget.c": True,
        "tools/vela/generated/model.h": False,
    }
    return {
        name: (layout_error(PurePosixPath(name)) is not None) == should_fail
        for name, should_fail in expectations.items()
    }


def _ownership_selftest_cases() -> dict[str, bool]:
    """Return vendor and compression ownership selftest results."""
    correct = {PurePosixPath("apps/shared_libs/third_party") / name for name in APP_OWNED_VENDORS}
    return {
        "correct ownership stays quiet": not vendor_ownership_errors(correct),
        "wrong platform root fires": bool(
            vendor_ownership_errors(
                correct - {PurePosixPath("apps/shared_libs/third_party/miniz")}
                | {PurePosixPath("libs/third_party/miniz")}
            )
        ),
        "app compression seam stays quiet": not compress_ownership_errors(set(APP_COMPRESS_FILES)),
        "legacy ra8_io compression seam fires": bool(
            compress_ownership_errors(set(APP_COMPRESS_FILES) | set(LEGACY_COMPRESS_FILES))
        ),
    }


def _python_tool_selftest_cases() -> dict[str, bool]:
    """Return flat-versus-structured Python tool layout results."""
    return {
        "single-file Python tool stays quiet": not python_tool_layout_errors(
            {PurePosixPath("tools/solo/runner.py")}
        ),
        "data-only tool stays quiet": not python_tool_layout_errors(
            {PurePosixPath("tools/model/weights.bin")}
        ),
        "flat multi-module Python tool fires": bool(
            python_tool_layout_errors(
                {
                    PurePosixPath("tools/flat/main.py"),
                    PurePosixPath("tools/flat/helper.py"),
                }
            )
        ),
        "root module plus nested helper fires": bool(
            python_tool_layout_errors(
                {
                    PurePosixPath("tools/nested/main.py"),
                    PurePosixPath("tools/nested/lib/helper.py"),
                }
            )
        ),
        "structured Python tool stays quiet": not python_tool_layout_errors(
            {
                PurePosixPath("tools/structured/src/main.py"),
                PurePosixPath("tools/structured/src/helper.py"),
                PurePosixPath("tools/structured/tests/main_selftest.py"),
            }
        ),
        "root module beside src fires": bool(
            python_tool_layout_errors(
                {
                    PurePosixPath("tools/structured/src/main.py"),
                    PurePosixPath("tools/structured/helper.py"),
                }
            )
        ),
        "test module under src fires": bool(
            python_tool_layout_errors({PurePosixPath("tools/structured/src/main_selftest.py")})
        ),
    }


def _component_helper_selftest_cases() -> dict[str, bool]:
    """Return app/example executable-helper layout results."""
    return {
        "app-root Python helper fires": bool(
            component_helper_layout_errors(
                {
                    PurePosixPath("apps/board/demo/CMakeLists.txt"),
                    PurePosixPath("apps/board/demo/generate.py"),
                }
            )
        ),
        "example-root shell helper fires": bool(
            component_helper_layout_errors(
                {
                    PurePosixPath("examples/board/demo/CMakeLists.txt"),
                    PurePosixPath("examples/board/demo/build_payload.sh"),
                }
            )
        ),
        "component scripts helper stays quiet": not component_helper_layout_errors(
            {
                PurePosixPath("examples/board/demo/CMakeLists.txt"),
                PurePosixPath("examples/board/demo/scripts/generate.py"),
            }
        ),
        "test gate under tests/scripts stays quiet": not component_helper_layout_errors(
            {
                PurePosixPath("apps/board/demo/CMakeLists.txt"),
                PurePosixPath("apps/board/demo/tests/scripts/emulator_gate.sh"),
            }
        ),
    }


def run_selftest() -> int:
    """Prove both rejection and acceptance directions of the layout rule."""
    path_cases = _path_layout_selftest_cases()
    path_failed = [name for name, passed in path_cases.items() if not passed]
    if path_failed:
        print(
            f"check_source_layout.py: selftest failed: {', '.join(path_failed)}",
            file=sys.stderr,
        )
        return 1
    policy_cases = {
        **_ownership_selftest_cases(),
        **_python_tool_selftest_cases(),
        **_component_helper_selftest_cases(),
    }
    policy_failed = [name for name, passed in policy_cases.items() if not passed]
    if policy_failed:
        print(
            f"check_source_layout.py: policy selftest failed: {', '.join(policy_failed)}",
            file=sys.stderr,
        )
        return 1
    print(f"check_source_layout.py: selftest passed ({len(path_cases) + len(policy_cases)} cases).")
    return 0


def check_tree(root: Path) -> int:
    """Check every first-party C-family, Python tool, and helper layout."""
    checked = 0
    violations: list[tuple[PurePosixPath, str]] = []
    paths = tracked_paths(root)
    for path in paths:
        if path.suffix.lower() not in IMPLEMENTATION_SUFFIXES | HEADER_SUFFIXES:
            continue
        if path.parts and path.parts[0] in SCOPED_ROOTS and not is_excluded(path):
            checked += 1
        error = layout_error(path)
        if error is not None:
            violations.append((path, error))
    existing_paths = set(paths)
    vendor_dirs = {
        PurePosixPath(base, child.name)
        for base in ("libs/third_party", "apps/shared_libs/third_party")
        for child in (root / base).iterdir()
        if child.is_dir()
    }
    ownership_errors = vendor_ownership_errors(vendor_dirs) + compress_ownership_errors(
        existing_paths
    )
    python_violations = python_tool_layout_errors(existing_paths)
    component_helper_violations = component_helper_layout_errors(existing_paths)
    if violations or ownership_errors or python_violations or component_helper_violations:
        print("Source layout violations:", file=sys.stderr)
        for path, error in violations:
            print(f"  {path}: {error}", file=sys.stderr)
        for error in ownership_errors:
            print(f"  {error}", file=sys.stderr)
        for path, error in python_violations:
            print(f"  {path}: {error}", file=sys.stderr)
        for path, error in component_helper_violations:
            print(f"  {path}: {error}", file=sys.stderr)
        print(
            "Move code to src/, public C headers to inc/, tests to tests/, "
            "and component helpers to scripts/ or tests/scripts/.",
            file=sys.stderr,
        )
        return 1
    print(f"check_source_layout.py: {checked} first-party C-family file(s) follow layout.")
    return 0


def main() -> int:
    """Parse arguments and run the requested validation mode."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()
    return check_tree(Path(__file__).resolve().parents[2])


if __name__ == "__main__":
    raise SystemExit(main())

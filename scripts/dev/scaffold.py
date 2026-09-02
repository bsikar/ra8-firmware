#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate policy-compliant repository project skeletons."""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
NAME_RE = re.compile(r"[a-z][a-z0-9_]*\Z")
GENERATED_FILE_COUNT = 18
SCAFFOLD_TYPES = ("app", "host", "lib", "example", "shared_lib")
SCAFFOLD_RECIPES = (
    "apps::board::new",
    "apps::host::new",
    "libs::new",
    "apps::example::new",
    "apps::shared::new",
)


def project_name(value: str) -> str:
    """Return a safe C/path identifier or raise an argparse error."""
    if NAME_RE.fullmatch(value) is None:
        message = (
            "name must start with a lowercase letter and contain only lowercase "
            "letters, digits, and underscores"
        )
        raise argparse.ArgumentTypeError(message)
    return value


def _format_content(path: Path, content: str) -> str:
    """Format generated C-family source before it reaches the filesystem."""
    if Path(path).suffix not in {".c", ".h"}:
        return content
    formatter = shutil.which("clang-format")
    if formatter is None:
        message = "clang-format is required to scaffold C-family source"
        raise RuntimeError(message)
    formatted = subprocess.run(  # noqa: S603 -- trusted formatter and generated input
        [
            formatter,
            f"--style=file:{REPO_ROOT / '.clang-format'}",
            f"--assume-filename={path}",
        ],
        input=content,
        capture_output=True,
        text=True,
        check=True,
    )
    return formatted.stdout


def create_file(path: Path, content: str) -> None:
    """Create one formatted scaffold file without overwriting existing work."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        formatted = _format_content(path, content.lstrip())
        path.write_text(formatted, encoding="utf-8")
        print(f"Created: {path}")
    else:
        print(f"Skipped: {path} (already exists)")


_HEADER_TMPL = """/**
 * @file inc/{name}.h
 * @brief {desc}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Initialize {name} state.
 * @pre Call once before using this library.
 * @post The library is ready for use.
 * @since 0.1.0
 */
void {name}_init(void);
"""

_LIB_SRC_TMPL = """/**
 * @file src/{name}.c
 * @brief {desc}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "{name}.h"

void {name}_init(void)
{{
  /* The initial scaffold owns no process-wide state. */
}}
"""

_SHARED_LIB_CMAKE_TMPL = """# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)
project({name} C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library({name} STATIC src/{name}.c)
target_include_directories({name} PUBLIC inc PRIVATE src)
target_compile_options({name} PRIVATE -Wall -Wextra -Werror)
"""

_HOST_CMAKE_TMPL = """# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)
project({name} C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(
  {name}
  src/main.c
  src/{name}.c
)

target_include_directories(
  {name} PRIVATE inc
)
target_compile_options({name} PRIVATE -Wall -Wextra -Werror)

# Unit Tests
enable_testing()
add_executable(test_{name} tests/src/test_{name}.c src/{name}.c)
target_include_directories(test_{name} PRIVATE inc)
target_compile_definitions(test_{name} PRIVATE RA8_OFF_TARGET TEST_MODE)
target_compile_options(test_{name} PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_{name} COMMAND test_{name})
"""

_HOST_MAIN_TMPL = """/**
 * @file src/main.c
 * @brief {desc}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "{name}.h"

int main(int argc, char** argv)
{{
  (void)argc;
  (void)argv;
  {name}_init();
  printf("Hello from {name}!\\n");
  return 0;
}}
"""

_HOST_TEST_TMPL = """/**
 * @file tests/src/test_{name}.c
 * @brief Unit tests for {name}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "{name}.h"

int main(void)
{{
  {name}_init();
  printf("Running tests for {name}...\\n");
  printf("All tests passed!\\n");
  return 0;
}}
"""

_FW_CMAKE_TMPL = """# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project({name} LANGUAGES C ASM)
endif()

set(_d "${{CMAKE_CURRENT_SOURCE_DIR}}")
while(NOT EXISTS "${{_d}}/cmake/ra8_add_app.cmake" AND NOT "${{_d}}" STREQUAL "/")
  get_filename_component(_d "${{_d}}" DIRECTORY)
endwhile()
include("${{_d}}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME {name}
  STACK_BYTES 2200
  DESCRIPTION "{desc}"
  # LIBS my_lib
)
"""

_FW_MAIN_TMPL = """/**
 * @file src/main.c
 * @brief {desc}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "{name}.h"

#include "ra8_boot_entry.h"
#include "ra8_log.h"

void main(void)
{{
  {name}_init();
  ra8_log_info("APP", "Hello from {name}!");

  while (1) {{
    /* Application work belongs here. */
  }}
}}
"""


def scaffold_lib(repo_root: Path, name: str) -> None:
    """Create a reusable first-party library skeleton."""
    lib_dir = repo_root / "libs" / name
    desc = f"Library: {name}"
    create_file(lib_dir / "inc" / f"{name}.h", _HEADER_TMPL.format(name=name, desc=desc))
    create_file(lib_dir / "src" / f"{name}.c", _LIB_SRC_TMPL.format(name=name, desc=desc))
    print(
        f"\nLibrary {name} scaffolded! You can now depend on it using "
        f"LIBS {name} in an app's CMakeLists.txt."
    )


def scaffold_shared_lib(repo_root: Path, name: str) -> None:
    """Create an application-scoped shared-library skeleton."""
    app_dir = repo_root / "apps" / "shared_libs" / name
    desc = f"Shared Library: {name}"
    create_file(
        app_dir / "CMakeLists.txt",
        _SHARED_LIB_CMAKE_TMPL.format(name=name, desc=desc),
    )
    create_file(
        app_dir / "src" / f"{name}.c",
        _LIB_SRC_TMPL.format(name=name, desc=desc),
    )
    create_file(
        app_dir / "inc" / f"{name}.h",
        _HEADER_TMPL.format(name=name, desc=desc),
    )
    print(f"\nProject {name} scaffolded at {app_dir}!")


def scaffold_host_app(repo_root: Path, name: str) -> None:
    """Create a host-application skeleton and its unit test."""
    app_dir = repo_root / "apps" / "host" / name
    desc = f"macOS Host App: {name}"
    create_file(app_dir / "CMakeLists.txt", _HOST_CMAKE_TMPL.format(name=name, desc=desc))
    create_file(app_dir / "src" / "main.c", _HOST_MAIN_TMPL.format(name=name, desc=desc))
    create_file(app_dir / "src" / f"{name}.c", _LIB_SRC_TMPL.format(name=name, desc=desc))
    create_file(
        app_dir / "tests" / "src" / f"test_{name}.c",
        _HOST_TEST_TMPL.format(name=name, desc=desc),
    )
    create_file(
        app_dir / "inc" / f"{name}.h",
        _HEADER_TMPL.format(name=name, desc=desc),
    )
    print(f"\nProject {name} scaffolded at {app_dir}!")


def scaffold_firmware_app(repo_root: Path, app_type: str, name: str) -> None:
    """Create a board application or firmware-example skeleton."""
    if app_type == "example":
        app_dir = repo_root / "examples" / "ek_ra8d2" / "hw_pending" / name
        desc = f"Example: {name}"
    else:
        app_dir = repo_root / "apps" / "board" / "stand_alone" / name
        desc = f"Standalone App: {name}"

    create_file(app_dir / "CMakeLists.txt", _FW_CMAKE_TMPL.format(name=name, desc=desc))
    create_file(
        app_dir / "src" / "main.c",
        _FW_MAIN_TMPL.format(name=name, desc=desc),
    )
    create_file(app_dir / "src" / f"{name}.c", _LIB_SRC_TMPL.format(name=name, desc=desc))
    create_file(
        app_dir / "inc" / f"{name}.h",
        _HEADER_TMPL.format(name=name, desc=desc),
    )
    print(f"\nProject {name} scaffolded at {app_dir}!")


def scaffold_project(repo_root: Path, project_type: str, name: str) -> None:
    """Dispatch one validated scaffold kind."""
    if project_type == "lib":
        scaffold_lib(repo_root, name)
    elif project_type == "shared_lib":
        scaffold_shared_lib(repo_root, name)
    elif project_type == "host":
        scaffold_host_app(repo_root, name)
    else:
        scaffold_firmware_app(repo_root, project_type, name)


def _name_template_selftest() -> list[str]:
    """Check identifier containment and render every template."""
    good = ("reader", "ra8_demo", "app2")
    bad = ("", "../escape", "two-words", "Upper", "2fast", "name/path", "space name")
    failures = [f"valid name rejected: {name}" for name in good if NAME_RE.fullmatch(name) is None]
    failures.extend(
        f"unsafe name accepted: {name!r}" for name in bad if NAME_RE.fullmatch(name) is not None
    )
    templates = (
        _HEADER_TMPL,
        _LIB_SRC_TMPL,
        _SHARED_LIB_CMAKE_TMPL,
        _HOST_CMAKE_TMPL,
        _HOST_MAIN_TMPL,
        _HOST_TEST_TMPL,
        _FW_CMAKE_TMPL,
        _FW_MAIN_TMPL,
    )
    for index, template in enumerate(templates):
        rendered = template.format(name="ra8_demo", desc="Self-test scaffold")
        if "{name}" in rendered or "{desc}" in rendered:
            failures.append(f"template {index} left an unsubstituted field")
        if re.search(r"\bTODO\b", rendered):
            failures.append(f"template {index} emitted a bare TODO placeholder")

    return failures


def _tree_selftest() -> list[str]:
    """Generate all five project kinds into an isolated tree."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-scaffold-selftest-") as temp_root:
        with contextlib.redirect_stdout(io.StringIO()):
            for project_type in SCAFFOLD_TYPES:
                scaffold_project(Path(temp_root), project_type, f"probe_{project_type}")
        generated = []
        for directory, _, filenames in os.walk(temp_root):
            generated.extend(Path(directory) / filename for filename in filenames)
        if len(generated) != GENERATED_FILE_COUNT:
            failures.append(
                f"generated-tree census changed: {len(generated)} != {GENERATED_FILE_COUNT} files"
            )
        for path in generated:
            content = Path(path).read_text(encoding="utf-8")
            rel = path.relative_to(temp_root).as_posix()
            if not content.strip():
                failures.append(f"generated an empty file: {rel}")
            if re.search(r"\bTODO\b", content):
                failures.append(f"generated a TODO placeholder: {rel}")
            if (
                rel.endswith("main.c")
                and rel.startswith(("examples/", "apps/board/stand_alone/"))
                and '#include "ra8_boot_entry.h"' not in content
            ):
                failures.append(f"firmware scaffold omits ra8_boot_entry.h: {rel}")
        formatter = shutil.which("clang-format")
        c_files = [path for path in generated if Path(path).suffix in {".c", ".h"}]
        if formatter is None:
            failures.append("clang-format is unavailable for generated-tree validation")
        else:
            formatted = subprocess.run(  # noqa: S603 -- fixed generated files
                [
                    formatter,
                    f"--style=file:{REPO_ROOT / '.clang-format'}",
                    "--dry-run",
                    "--Werror",
                    *c_files,
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            if formatted.returncode != 0:
                failures.append("generated C/header templates are not clang-formatted")
    return failures


def _recipe_selftest() -> list[str]:
    """Check that every Just entry point resolves and rejects a missing name."""
    failures: list[str] = []
    just_bin = shutil.which("just")
    if just_bin is None:
        return ["just executable is unavailable"]
    for recipe in SCAFFOLD_RECIPES:
        resolved = subprocess.run(  # noqa: S603 -- fixed project recipe surface
            [just_bin, "--dry-run", recipe, "ra8_scaffold_probe"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if resolved.returncode != 0:
            failures.append(f"Just scaffold recipe does not resolve: {recipe}")
        missing = subprocess.run(  # noqa: S603 -- argv is [resolved just, fixed recipe], no shell, no caller input
            [just_bin, recipe],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if missing.returncode == 0:
            failures.append(f"Just scaffold recipe accepts a missing name: {recipe}")
    return failures


def selftest() -> int:
    """Prove names, templates, trees, and all five Just entry points."""
    failures = _name_template_selftest() + _tree_selftest() + _recipe_selftest()
    if failures:
        for failure in failures:
            print(f"selftest: scaffold.py FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        "selftest: scaffold.py OK (3 valid names, 7 unsafe names, "
        f"8 templates, {len(SCAFFOLD_RECIPES)} Just recipes)"
    )
    return 0


def main() -> int:
    """Parse the CLI and generate one requested project skeleton."""
    parser = argparse.ArgumentParser(description="Scaffold boilerplate for the RA8 firmware repo")
    parser.add_argument("--selftest", action="store_true", help="run internal detector checks")
    parser.add_argument(
        "type",
        nargs="?",
        choices=SCAFFOLD_TYPES,
        help="Type of project to scaffold",
    )
    parser.add_argument(
        "name", nargs="?", type=project_name, help="Name of the project (e.g. my_cool_app)"
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if args.type is None or args.name is None:
        parser.error("type and name are required unless --selftest is used")

    scaffold_project(REPO_ROOT, args.type, args.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

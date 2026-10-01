# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Static source and linker-script checks for the freestanding runtime ratchet.

These run without an ELF: they scan linker scripts for a heap region or an
`end` symbol, and C/C++ sources for assert.h includes and assert() calls, so
`--check-scripts` and `--check-asserts` need no target image. Imported by
check_freestanding_runtime, which owns the ELF, map and baseline work.
"""

from __future__ import annotations

import pathlib
import re
from typing import Any


def check_linker_script(content: str, path: str) -> list[str]:
    """Check a linker script for prohibited 'end'/'_end' definitions and '.heap' sections."""
    comment_re = re.compile(r"/\*.*?\*/", re.DOTALL)
    stripped = comment_re.sub(" ", content)

    violations = []
    end_sym_re = re.compile(r"\b(PROVIDE\s*\(\s*)?(_?end)\s*=", re.MULTILINE)
    violations.extend(
        f"{path}: defines forbidden heap anchor '{m.group(2)}'"
        for m in end_sym_re.finditer(stripped)
    )

    heap_sec_re = re.compile(r"(?<![a-zA-Z0-9_])\.heap\b")
    if heap_sec_re.search(stripped):
        violations.append(f"{path}: defines forbidden '.heap' output section")

    return violations


def check_all_linker_scripts(repo_root: pathlib.Path, baseline: dict[str, Any]) -> list[str]:
    """Check all target linker scripts in the repository."""
    violations: list[str] = []
    allowed_script_exceptions = set(baseline.get("linker_script_exceptions", []))

    roots = [
        repo_root / "libs",
        repo_root / "examples",
        repo_root / "apps",
    ]

    for root in roots:
        for ld_path in root.glob("**/*.ld"):
            rel_path = str(ld_path.relative_to(repo_root)).replace("\\", "/")
            if "third_party" in rel_path:
                continue

            content = ld_path.read_text(encoding="utf-8", errors="replace")
            script_violations = check_linker_script(content, rel_path)
            for v in script_violations:
                if rel_path in allowed_script_exceptions:
                    continue
                violations.append(v)

    return violations


def check_source_asserts(content: str, path: str) -> list[str]:
    """Check a source file for forbidden <assert.h> and runtime assert()."""
    comment_re = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
    stripped = comment_re.sub(" ", content)
    str_re = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    stripped = str_re.sub(" ", stripped)
    # Ignore preprocessor macro definitions redirecting assert, e.g. #define assert(...)
    macro_def_re = re.compile(r"#\s*define\s+assert\b[^\n]*")
    stripped = macro_def_re.sub(" ", stripped)

    violations = []
    if re.search(r"#\s*include\s+<assert\.h>", content):
        violations.append(f"{path}: includes forbidden libc <assert.h>")

    assert_call_re = re.compile(r"(?<![a-zA-Z0-9_])assert\s*\(")
    for m in assert_call_re.finditer(stripped):
        line_num = content[: m.start()].count("\n") + 1
        violations.append(
            f"{path}:{line_num}: uses forbidden standard assert() "
            "(use RA8_ASSERT for runtime invariants or static_assert for compile-time)"
        )
    return violations


def check_all_source_asserts(repo_root: pathlib.Path) -> list[str]:
    """Check all target-linkable first-party source files for forbidden asserts."""
    violations: list[str] = []
    roots = [
        repo_root / "libs",
        repo_root / "examples",
        repo_root / "apps",
        repo_root / "port",
    ]
    # port/posix is host-only and legitimately uses host facilities.
    # port/esp-hosted is target-linkable and audited (no blanket exemption).
    exempt_fragments = ("third_party", "tests", "apps/host", "port/posix", ".pb-c.")

    for root in roots:
        for p in root.glob("**/*"):
            if not p.is_file() or p.suffix not in (".c", ".h"):
                continue
            rel_path = str(p.relative_to(repo_root)).replace("\\", "/")
            if any(frag in rel_path for frag in exempt_fragments):
                continue
            content = p.read_text(encoding="utf-8", errors="replace")
            violations.extend(check_source_asserts(content, rel_path))

    return violations

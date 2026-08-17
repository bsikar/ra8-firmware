#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce bounded first-party memory ownership in production code.

Applies to RA8D2 firmware, first-party ESP32-C6 port code, and production
host-tool code. Firmware rejects direct and known transitive allocation.
Production tools reject every direct allocator; known third-party SOUP
lifecycle calls remain visible as a non-blocking report because their opaque
implementation is outside first-party ownership.

Flags two classes of violation:

  * **Direct allocator calls** -- C allocators such as malloc, calloc,
    realloc, aligned_alloc, free, strdup, and asprintf, plus C++ ``new`` and
    ``delete`` expressions.

  * **Vendored helpers that allocate transitively** under the hood, so a
    plain grep for malloc/free won't catch them:

      - stb_truetype: stbtt_GetCodepointBitmap[Subpixel],
                      stbtt_GetGlyphBitmap[Subpixel],
                      stbtt_GetCodepointSDF, stbtt_GetGlyphSDF,
                      stbtt_GetCodepointShape, stbtt_GetGlyphShape,
                      stbtt_FreeBitmap, stbtt_FreeSDF, stbtt_FreeShape.
                      The no-alloc paths are the Make*/Box variants.

      - stb_image:    stbi_load, stbi_load_from_memory,
                      stbi_load_from_callbacks, stbi_loadf,
                      stbi_loadf_from_memory, stbi_image_free.

      - miniz:        mz_zip_reader_extract_to_heap,
                      mz_zip_reader_extract_file_to_heap.
                      Use the *_to_mem variants with a caller buffer.

      - tinyxml2:     XMLDocument owns heap-backed node/string pools unless a
                      reviewed caller-owned allocator is explicitly bound.

      - ESP-IDF HTTP: esp_http_client_init, esp_http_client_set_url,
                      esp_http_client_open, esp_http_client_close,
                      esp_http_client_cleanup. These allocate or release
                      transitive HTTP/TLS state and therefore require an exact,
                      reasoned exception until a fixed-memory backend replaces
                      the adapter.

      - Host-tool SOUP boundaries: miniz writer lifecycle, libcurl easy
        lifecycle/request calls, and POSIX spawn setup/execution. These APIs
        may allocate transitively. They are reported for review, but do not
        excuse a first-party ``malloc``/``free`` wrapper around them.

Scope:

  Firmware code under libs/ra8_*/, src/, port/esp32_c6/, and examples/<app>/
  where <app> has main.c + CMakeLists.txt is blocking. First-party production
  C-family code under tools/ is blocking for direct allocation. Build outputs,
  vendored code, and host-side tests/ are exempt.

  Full sweeps enforce measured per-domain and aggregate file-count floors so a
  collapsed firmware or tool walk cannot report a vacuous pass.

Inline suppression:

  Append `alloc-allow: <reason>` on the same line. The reason is
  mandatory; bare `alloc-allow` with no reason is itself rejected.

Example:
      void* p = malloc(64); /* alloc-allow: bringup scratch, removed in v0.2 */

Usage:

    # explicit file list (used by pre-commit):
    python3 scripts/checks/check_no_dynamic_alloc.py FILE [FILE ...]

    # full-repo sweep (CI):
    python3 scripts/checks/check_no_dynamic_alloc.py --all

Exit code:
    0  no violations
    1  violations found
    2  CLI usage error
"""

from __future__ import annotations

import pathlib
import re
import sys

from doxy_lex import blank_noncode
from lint_targets import firmware_app_dirs, is_build_output_path

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {
    ".c",
    ".h",
    ".cc",
    ".cpp",
    ".cxx",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".m",
    ".mm",
}

DIRECT_ALLOCATORS = (
    "malloc",
    "calloc",
    "realloc",
    "reallocarray",
    "aligned_alloc",
    "posix_memalign",
    "valloc",
    "memalign",
    "free",
    "strdup",
    "strndup",
    "asprintf",
    "vasprintf",
)

FIRMWARE_TRANSITIVE_ALLOCATORS = (
    "stbtt_GetCodepointBitmap",
    "stbtt_GetCodepointBitmapSubpixel",
    "stbtt_GetGlyphBitmap",
    "stbtt_GetGlyphBitmapSubpixel",
    "stbtt_GetCodepointSDF",
    "stbtt_GetGlyphSDF",
    "stbtt_GetCodepointShape",
    "stbtt_GetGlyphShape",
    "stbtt_FreeBitmap",
    "stbtt_FreeSDF",
    "stbtt_FreeShape",
    "stbi_load",
    "stbi_load_from_memory",
    "stbi_load_from_callbacks",
    "stbi_loadf",
    "stbi_loadf_from_memory",
    "stbi_image_free",
    "mz_zip_reader_extract_to_heap",
    "mz_zip_reader_extract_file_to_heap",
    "esp_http_client_init",
    "esp_http_client_set_url",
    "esp_http_client_open",
    "esp_http_client_close",
    "esp_http_client_cleanup",
)

HOST_SOUP_ALLOCATORS = (
    "mz_zip_writer_init_file",
    "mz_zip_writer_add_file",
    "mz_zip_writer_add_mem",
    "mz_zip_writer_finalize_archive",
    "mz_zip_writer_end",
    "curl_global_init",
    "curl_easy_init",
    "curl_easy_setopt",
    "curl_easy_perform",
    "curl_easy_cleanup",
    "curl_slist_append",
    "curl_slist_free_all",
    "posix_spawn_file_actions_init",
    "posix_spawnattr_init",
    "posix_spawn",
    "posix_spawnp",
)

TRANSITIVE_ALLOCATORS = FIRMWARE_TRANSITIVE_ALLOCATORS + HOST_SOUP_ALLOCATORS
ALL_NAMES = DIRECT_ALLOCATORS + TRANSITIVE_ALLOCATORS

# Match "<name>(" at a word boundary so e.g. mz_free(...) does not match
# free, and stbtt_GetCodepointBitmapBox does not match
# stbtt_GetCodepointBitmap.
SYM_RE = re.compile(r"\b(" + "|".join(re.escape(n) for n in ALL_NAMES) + r")\b\s*\(")
CPP_DIRECT_RE = re.compile(r"\b(new|delete)\b")
OPAQUE_OWNER_TYPE_RE = re.compile(r"\bXMLDocument\b")

# Inline exemption marker. The reason after the colon is mandatory.
ALLOW_RE = re.compile(r"alloc-allow\s*:\s*\S")
BARE_ALLOW_RE = re.compile(r"alloc-allow\b")

# Minimum number of argv entries needed for a file/flag argument to be present.
MIN_ARGC_WITH_ARG = 2

SCOPE_FILE_FLOORS = {"firmware": 850, "tool": 200}
TOTAL_FILE_FLOOR = 1100


def _firmware_scan_dirs() -> list[pathlib.Path]:
    """The firmware directories this rule governs.

    Deliberately narrower than the whole tree: host tools and tests may
    allocate freely, and Rule 3 is a statement about what runs on the target.

    ``apps/`` is the exception that has to be named. It is the products tier
    and ``_tool_scan_dirs()`` claims all of it, but a firmware PRODUCT is not a
    host program: the e-reader image runs on the chip, where Rule 3 says zero
    dynamic allocation after init, and letting the products root swallow it
    would quietly downgrade it to the report-only tool scope. The firmware
    products are derived, not listed -- see ``lint_targets.firmware_app_dirs``
    -- and ``_scope()`` tests the firmware roots first, so the narrower claim
    wins.
    """
    out: list[pathlib.Path] = []
    libs = REPO_ROOT / "libs"
    if libs.is_dir():
        out.extend(
            entry
            for entry in sorted(libs.iterdir())
            if entry.is_dir() and entry.name.startswith("ra8_")
        )
    src = REPO_ROOT / "src"
    if src.is_dir():
        out.append(src)
    c6_port = REPO_ROOT / "port" / "esp32_c6"
    if c6_port.is_dir():
        out.append(c6_port)
    out.extend(REPO_ROOT / rel for rel in firmware_app_dirs())
    examples = REPO_ROOT / "examples"
    if examples.is_dir():
        for tier in sorted(examples.iterdir()):
            if not tier.is_dir():
                continue
            for entry in sorted(tier.iterdir()):
                if not entry.is_dir():
                    continue
                if (entry / "main.c").is_file() and (entry / "CMakeLists.txt").is_file():
                    out.append(entry)
    return out


def _tool_scan_dirs() -> list[pathlib.Path]:
    """First-party production host trees governed by explicit ownership.

    Both roots, because both hold shipped host code: ``tools/`` the developer
    utilities, ``apps/`` the products. They were one root until media_dl moved
    out of ``tools/``, at which point the file floor below caught the loss --
    199 files against a floor of 200 -- rather than letting a whole product
    stop being checked for direct allocator calls.
    """
    return [
        directory for directory in (REPO_ROOT / "tools", REPO_ROOT / "apps") if directory.is_dir()
    ]


FIRMWARE_SCAN_DIRS = _firmware_scan_dirs()
TOOL_SCAN_DIRS = _tool_scan_dirs()
FIRMWARE_SCAN_RELS = [directory.relative_to(REPO_ROOT) for directory in FIRMWARE_SCAN_DIRS]
TOOL_SCAN_RELS = [directory.relative_to(REPO_ROOT) for directory in TOOL_SCAN_DIRS]


def _scope(path: pathlib.Path) -> str | None:
    """Return ``firmware`` or ``tool`` for an in-scope production source."""
    if path.suffix not in SOURCE_SUFFIXES:
        return None
    try:
        rel = path.resolve().relative_to(REPO_ROOT)
    except ValueError:
        return None
    excluded_parts = {"third_party", "tests", "build", "_deps"}
    rel_str = str(rel)
    if any(part in excluded_parts for part in rel.parts) or is_build_output_path(rel.as_posix()):
        return None
    if any(
        rel_str == str(directory) or rel_str.startswith(str(directory) + "/")
        for directory in FIRMWARE_SCAN_RELS
    ):
        return "firmware"
    if any(
        rel_str == str(directory) or rel_str.startswith(str(directory) + "/")
        for directory in TOOL_SCAN_RELS
    ):
        return "tool"
    return None


def _is_in_scope(path: pathlib.Path) -> bool:
    """Return whether ``path`` belongs to a blocking or report-only scope."""
    return _scope(path) is not None


def check(path: pathlib.Path) -> list[str]:
    """Report every dynamic-allocation call in one production source file.

    An unreadable file yields an empty list rather than raising, so one bad
    file cannot abort the sweep.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []

    original_lines = text.splitlines()
    stripped_lines = blank_noncode(text)[0].splitlines()
    problems: list[str] = []

    for idx, stripped in enumerate(stripped_lines):
        line_no = idx + 1
        original = original_lines[idx] if idx < len(original_lines) else ""
        for m in SYM_RE.finditer(stripped):
            sym = m.group(1)
            if ALLOW_RE.search(original):
                continue
            kind = "direct" if sym in DIRECT_ALLOCATORS else "transitive"
            problems.append(
                f"{path}:{line_no}: {kind} dynamic allocation: {sym}() -- {original.strip()}"
            )
        cpp_code = "" if stripped.lstrip().startswith("#") else stripped
        for match in CPP_DIRECT_RE.finditer(cpp_code):
            if ALLOW_RE.search(original):
                continue
            keyword = match.group(1)
            problems.append(
                f"{path}:{line_no}: direct dynamic allocation: C++ {keyword} -- {original.strip()}"
            )
        for _match in OPAQUE_OWNER_TYPE_RE.finditer(cpp_code):
            if ALLOW_RE.search(original):
                continue
            problems.append(
                f"{path}:{line_no}: transitive dynamic allocation: "
                f"tinyxml2::XMLDocument -- {original.strip()}"
            )

    for idx, original in enumerate(original_lines):
        line_no = idx + 1
        if BARE_ALLOW_RE.search(original) and not ALLOW_RE.search(original):
            problems.append(
                f"{path}:{line_no}: alloc-allow without reason "
                f"-- write `alloc-allow: <reason>` instead"
            )

    return problems


def collect_repo_paths() -> list[pathlib.Path]:
    """Every source file in the blocking and report-only production scopes."""
    directories = FIRMWARE_SCAN_DIRS + TOOL_SCAN_DIRS
    return [
        path for directory in directories for path in directory.rglob("*") if _is_in_scope(path)
    ]


def _scope_floor_errors(counts: dict[str, int]) -> list[str]:
    """Describe every collapsed full-sweep domain or aggregate."""
    errors = [
        f"{scope} enumerated {counts.get(scope, 0)} source file(s); floor is {floor}"
        for scope, floor in SCOPE_FILE_FLOORS.items()
        if counts.get(scope, 0) < floor
    ]
    total = sum(counts.values())
    if total < TOTAL_FILE_FLOOR:
        errors.append(f"total scope enumerated {total} source file(s); floor is {TOTAL_FILE_FLOOR}")
    return errors


def _collect_all_validated() -> list[pathlib.Path] | None:
    """Return the full nonvacuous scope, or report a collapsed enumeration."""
    paths = collect_repo_paths()
    counts = {scope: sum(_scope(path) == scope for path in paths) for scope in SCOPE_FILE_FLOORS}
    floor_errors = _scope_floor_errors(counts)
    if not floor_errors:
        return paths
    print(
        "check_no_dynamic_alloc.py: FATAL -- " + "; ".join(floor_errors),
        file=sys.stderr,
    )
    return None


def _requested_paths() -> tuple[list[pathlib.Path] | None, int]:
    """Resolve CLI scope, returning a nonzero status when it is unusable."""
    if len(sys.argv) >= MIN_ARGC_WITH_ARG and sys.argv[1] == "--all":
        paths = _collect_all_validated()
        return paths, 0 if paths is not None else 2
    if len(sys.argv) >= MIN_ARGC_WITH_ARG:
        return [pathlib.Path(path).resolve() for path in sys.argv[1:]], 0
    print(
        "usage: check_no_dynamic_alloc.py FILE [FILE ...] | --all",
        file=sys.stderr,
    )
    return None, 2


def main() -> int:
    """Fail when firmware or first-party production tools allocate directly.

    Firmware also rejects known transitive allocators. Production tools report
    third-party SOUP lifecycle calls while rejecting direct first-party
    ownership. Tests remain outside this production policy.

    Returns 1 listing each blocking call site, 0 when both production domains
    are clean, and 2 for invalid usage or a collapsed full sweep.
    """
    paths, error = _requested_paths()
    if paths is None:
        return error

    failures: list[str] = []
    reports: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        if not _is_in_scope(path):
            continue
        problems = check(path)
        scope = _scope(path)
        if scope == "firmware":
            failures.extend(problems)
        else:
            failures.extend(
                problem for problem in problems if ": direct dynamic allocation:" in problem
            )
            reports.extend(
                problem for problem in problems if ": transitive dynamic allocation:" in problem
            )

    if reports:
        print(
            "check_no_dynamic_alloc.py: host-tool SOUP allocation report "
            "(non-blocking third-party boundary)."
        )
        for line in reports:
            print(line)
        print(f"\n{len(reports)} host SOUP call site(s) reported; these rows are non-blocking.")

    if failures:
        print(
            "check_no_dynamic_alloc.py: dynamic allocation in firmware or first-party tool code.",
            file=sys.stderr,
        )
        for line in failures:
            print(line, file=sys.stderr)
        print(f"\n{len(failures)} violation(s) found.", file=sys.stderr)
        print(
            "If a call site is genuinely OK (host-only test path, "
            "vendored glue you cannot move), append "
            "`alloc-allow: <reason>` on the same line.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce NASA Power-of-10 Rule 3 -- no dynamic memory after init.

Applies to RA8D2 firmware code and first-party ESP32-C6 port code.

Flags two classes of violation:

  * **Direct allocator calls** -- malloc, calloc, realloc, reallocarray,
    aligned_alloc, posix_memalign, valloc, memalign, free, strdup,
    strndup, asprintf, vasprintf.

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

      - ESP-IDF HTTP: esp_http_client_init, esp_http_client_set_url,
                      esp_http_client_open, esp_http_client_close,
                      esp_http_client_cleanup. These allocate or release
                      transitive HTTP/TLS state and therefore require an exact,
                      reasoned exception until a fixed-memory backend replaces
                      the adapter.

      - Host downloader SOUP boundaries: miniz writer lifecycle, libcurl easy
        lifecycle/request calls, and POSIX spawn setup/execution. These APIs
        may allocate transitively. They are reported for review, but do not
        fail the firmware Rule 3 gate because they never run on a target.

Scope:

  Firmware code under libs/ra8_*/, src/, port/esp32_c6/, and examples/<app>/
  where <app> has main.c + CMakeLists.txt is blocking. tools/media_dl/inc and
  tools/media_dl/src are a separate, non-blocking SOUP-boundary report. Build
  outputs, vendored code, and host-side tests/ are exempt.

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

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}

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

# Inline exemption marker. The reason after the colon is mandatory.
ALLOW_RE = re.compile(r"alloc-allow\s*:\s*\S")
BARE_ALLOW_RE = re.compile(r"alloc-allow\b")

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")

# Minimum number of argv entries needed for a file/flag argument to be present.
MIN_ARGC_WITH_ARG = 2


def _firmware_scan_dirs() -> list[pathlib.Path]:
    """The firmware directories this rule governs.

    Deliberately narrower than the whole tree: host tools and tests may
    allocate freely, and Rule 3 is a statement about what runs on the target.
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


def _host_report_dirs() -> list[pathlib.Path]:
    """Host-only trees whose allocator-bearing SOUP calls remain visible."""
    media_dl = REPO_ROOT / "tools" / "media_dl"
    return [candidate for subtree in ("inc", "src") if (candidate := media_dl / subtree).is_dir()]


FIRMWARE_SCAN_DIRS = _firmware_scan_dirs()
HOST_REPORT_DIRS = _host_report_dirs()
FIRMWARE_SCAN_RELS = [directory.relative_to(REPO_ROOT) for directory in FIRMWARE_SCAN_DIRS]
HOST_REPORT_RELS = [directory.relative_to(REPO_ROOT) for directory in HOST_REPORT_DIRS]


def _scope(path: pathlib.Path) -> str | None:
    """Return ``firmware`` or ``host`` for a source file in either scope."""
    if path.suffix not in SOURCE_SUFFIXES:
        return None
    try:
        rel = path.resolve().relative_to(REPO_ROOT)
    except ValueError:
        return None
    excluded_parts = {"third_party", "build", "_deps"}
    if any(part in excluded_parts for part in rel.parts):
        return None
    rel_str = str(rel)
    if any(
        rel_str == str(directory) or rel_str.startswith(str(directory) + "/")
        for directory in FIRMWARE_SCAN_RELS
    ):
        return "firmware"
    if any(
        rel_str == str(directory) or rel_str.startswith(str(directory) + "/")
        for directory in HOST_REPORT_RELS
    ):
        return "host"
    return None


def _is_in_scope(path: pathlib.Path) -> bool:
    """Return whether ``path`` belongs to a blocking or report-only scope."""
    return _scope(path) is not None


def _strip_comments(text: str) -> str:
    """Strip /* */ and // comments while preserving line numbers."""
    no_block = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", no_block)


def check(path: pathlib.Path) -> list[str]:
    """Report every dynamic-allocation call in one firmware file.

    An unreadable file yields an empty list rather than raising, so one bad
    file cannot abort the sweep.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []

    original_lines = text.splitlines()
    stripped_lines = _strip_comments(text).splitlines()
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

    for idx, original in enumerate(original_lines):
        line_no = idx + 1
        if BARE_ALLOW_RE.search(original) and not ALLOW_RE.search(original):
            problems.append(
                f"{path}:{line_no}: alloc-allow without reason "
                f"-- write `alloc-allow: <reason>` instead"
            )

    return problems


def collect_repo_paths() -> list[pathlib.Path]:
    """Every source file in the blocking and report-only scopes."""
    directories = FIRMWARE_SCAN_DIRS + HOST_REPORT_DIRS
    return [
        path for directory in directories for path in directory.rglob("*") if _is_in_scope(path)
    ]


def main() -> int:
    """Fail when firmware calls malloc, free or a relative.

    The rule targets the target image only -- host tools and tests are out of
    scope -- because the hazard is heap exhaustion and fragmentation on a
    device with no allocation-failure path, not allocation as such.

    Returns 1 listing each call site, 0 when the firmware is clean.
    """
    if len(sys.argv) >= MIN_ARGC_WITH_ARG and sys.argv[1] == "--all":
        paths = collect_repo_paths()
    elif len(sys.argv) >= MIN_ARGC_WITH_ARG:
        paths = [pathlib.Path(p).resolve() for p in sys.argv[1:]]
    else:
        print(
            "usage: check_no_dynamic_alloc.py FILE [FILE ...] | --all",
            file=sys.stderr,
        )
        return 2

    failures: list[str] = []
    reports: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        if not _is_in_scope(path):
            continue
        problems = check(path)
        if _scope(path) == "firmware":
            failures.extend(problems)
        else:
            reports.extend(problems)

    if reports:
        print(
            "check_no_dynamic_alloc.py: host media_dl SOUP allocation report "
            "(non-blocking; not target firmware)."
        )
        for line in reports:
            print(line)
        print(f"\n{len(reports)} host SOUP call site(s) reported; firmware gate unaffected.")

    if failures:
        print(
            "check_no_dynamic_alloc.py: NASA Rule 3 -- no dynamic allocation in firmware code.",
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

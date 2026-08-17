#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject allocator-backed C/POSIX streams in first-party C-family code.

Reusable firmware code uses ``fw_fs_file_t`` and injected ``ra8_io``/logging
facades. Hosted adapters may use raw descriptors and bounded caller-owned
state, but they must not expose or depend on C-runtime ``FILE``/``DIR`` state.
Those opaque stream objects can allocate internally and do not make storage or
stack bounds part of the caller-visible contract.

This gate rejects the stream types, standard streams, file/console stream
functions, and allocator-backed directory walkers in code positions. Matching
bare identifiers, rather than calls alone, also catches typedefs, macro aliases,
function-pointer assignments, and wrappers that merely rename a forbidden API.
Comments and string/character literals are blanked before matching.

Memory-only conversion APIs such as ``snprintf``, ``vsnprintf``, and ``sscanf``
are deliberately outside this rule: they do not create a stream. Separate
bounds, allocation, and format-string gates still govern their safe use.

Scope is version-controlled or newly added C-family source under ``libs/``,
``port/``, ``examples/``, ``src/``, ``coprocessor/``, ``tools/``, and
``tests/``. Vendored SOUP, generated font tables, and exact sources registered
as generated are excluded. Host tools and tests use raw descriptor adapters
and injected streams at their composition edge; being hosted or test-only does
not make opaque allocator-backed streams acceptable.

The full sweep has a zero baseline: one finding fails. Per-root and total file
floors make a collapsed or accidentally narrowed enumeration fatal.

Usage::

    check_no_stdio_streams.py FILE [FILE ...]
    check_no_stdio_streams.py --all
    check_no_stdio_streams.py --selftest

Returns 0 when clean, 1 on policy findings, and 2 on usage/scope failure.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from collections.abc import Iterable

from doxy_lex import blank_noncode
from lint_coverage_rules import PATH_CLASS
from lint_targets import is_build_output_path
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SCOPE_ROOTS = (
    "libs/",
    "port/",
    "examples/",
    "src/",
    "coprocessor/",
    "tools/",
    "apps/",
    "tests/",
)
SOURCE_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx", ".inc", ".m", ".mm")
EXCLUDED_PREFIXES = ("libs/third_party/", "libs/ra8_fonts/")
GENERATED_SOURCE_PATHS = frozenset(
    path for path, classification in PATH_CLASS.items() if classification == "generated-source"
)

# The floors are below current counts, but high enough that dropping a major
# first-party source or test subtree cannot report a vacuous pass.
ROOT_FILE_FLOORS = {
    "libs/": 850,
    "port/": 80,
    "examples/": 400,
    "src/": 10,
    "tools/": 180,
    "apps/": 110,
    "tests/": 650,
}
TOTAL_FILE_FLOOR = 2300

# Every token is rejected wherever it appears in code. This catches direct
# calls and declarations as well as aliases such as ``#define OPEN fopen``.
BANNED_TOKENS = (
    "DIR",
    "FILE",
    "_IO_FILE",
    "__sFILE",
    "alphasort",
    "alphasort64",
    "clearerr",
    "clearerr_unlocked",
    "closedir",
    "dirfd",
    "dprintf",
    "fclose",
    "fcloseall",
    "fdopen",
    "fdopendir",
    "feof",
    "feof_unlocked",
    "ferror",
    "ferror_unlocked",
    "fflush",
    "fflush_unlocked",
    "fgetc",
    "fgetc_unlocked",
    "fgetpos",
    "fgetpos64",
    "fgets",
    "fgetwc",
    "fgetws",
    "fileno",
    "fileno_unlocked",
    "flockfile",
    "fmemopen",
    "fopen",
    "fopen64",
    "fopen_s",
    "fopencookie",
    "fpos_t",
    "fprintf",
    "fputc",
    "fputc_unlocked",
    "fputs",
    "fputwc",
    "fputws",
    "fread",
    "fread_unlocked",
    "freopen",
    "freopen64",
    "freopen_s",
    "fscanf",
    "fseek",
    "fseeko",
    "fseeko64",
    "fsetpos",
    "fsetpos64",
    "ftell",
    "ftello",
    "ftello64",
    "ftrylockfile",
    "ftw",
    "ftw64",
    "funlockfile",
    "funopen",
    "funopen2",
    "fwprintf",
    "fwide",
    "fwrite",
    "fwrite_unlocked",
    "getc",
    "getchar",
    "getchar_unlocked",
    "getdelim",
    "getline",
    "gets",
    "gets_s",
    "getw",
    "getwc",
    "getwchar",
    "nftw",
    "nftw64",
    "open_memstream",
    "open_wmemstream",
    "opendir",
    "pclose",
    "perror",
    "popen",
    "printf",
    "putc",
    "putc_unlocked",
    "putchar",
    "putchar_unlocked",
    "puts",
    "putw",
    "putwc",
    "putwchar",
    "readdir",
    "readdir64",
    "readdir64_r",
    "readdir_r",
    "rewind",
    "rewinddir",
    "scandir",
    "scandir64",
    "scandirat",
    "scandirat64",
    "scanf",
    "seekdir",
    "setbuf",
    "setbuffer",
    "setlinebuf",
    "setvbuf",
    "stderr",
    "stdin",
    "stdout",
    "telldir",
    "tempnam",
    "tmpfile",
    "tmpfile64",
    "tmpfile_s",
    "tmpnam",
    "tmpnam_r",
    "ungetc",
    "ungetwc",
    "vdprintf",
    "vfprintf",
    "vfscanf",
    "vfwprintf",
    "vfwscanf",
    "vprintf",
    "vscanf",
    "vwprintf",
    "vwscanf",
    "versionsort",
    "versionsort64",
    "wprintf",
    "wscanf",
)

TOKEN_RE = re.compile(r"\b(" + "|".join(re.escape(token) for token in BANNED_TOKENS) + r")\b")
FORMAT_ATTRIBUTE_PREFIX_RE = re.compile(
    r"(?:\[\[\s*(?:gnu::)?format|__attribute__\s*\(\(\s*format)\s*\(\s*$"
)

Finding = tuple[int, int, str, str]


def _in_scope(rel: str) -> bool:
    """Return whether ``rel`` is first-party C-family source."""
    normalized = rel.replace("\\", "/").lstrip("./")
    if not normalized.startswith(SCOPE_ROOTS):
        return False
    if not normalized.lower().endswith(SOURCE_SUFFIXES):
        return False
    if normalized.startswith(EXCLUDED_PREFIXES):
        return False
    if normalized in GENERATED_SOURCE_PATHS:
        return False
    return not is_build_output_path(normalized)


def _is_format_attribute(code: str, offset: int, token: str) -> bool:
    """Allow ``printf``/``scanf`` only as a compiler format dialect name."""
    if token not in {"printf", "scanf"}:
        return False
    line_start = code.rfind("\n", 0, offset) + 1
    return FORMAT_ATTRIBUTE_PREFIX_RE.search(code[line_start:offset]) is not None


def scan_text(text: str) -> list[Finding]:
    """Return forbidden code-position tokens in ``text`` with source locations."""
    code, _comments = blank_noncode(text)
    raw_lines = text.splitlines()
    findings: list[Finding] = []
    for match in TOKEN_RE.finditer(code):
        token = match.group(1)
        if _is_format_attribute(code, match.start(), token):
            continue
        line_no = code.count("\n", 0, match.start()) + 1
        line_start = code.rfind("\n", 0, match.start()) + 1
        column = match.start() - line_start + 1
        source = raw_lines[line_no - 1].strip() if line_no <= len(raw_lines) else ""
        findings.append((line_no, column, token, source))
    return findings


def _scope_floor_errors(counts: dict[str, int]) -> list[str]:
    """Describe every per-root or total enumeration floor violation."""
    errors: list[str] = []
    total = sum(counts.values())
    for root, floor in ROOT_FILE_FLOORS.items():
        actual = counts.get(root, 0)
        if actual < floor:
            errors.append(f"{root} enumerated {actual} source file(s); floor is {floor}")
    if total < TOTAL_FILE_FLOOR:
        errors.append(f"total scope enumerated {total} source file(s); floor is {TOTAL_FILE_FLOOR}")
    return errors


def _working_scope() -> tuple[list[pathlib.Path], dict[str, int]]:
    """Enumerate present tracked/new in-scope files and enforce coverage floors."""
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],  # noqa: S607
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(
            "check_no_stdio_streams.py: FATAL -- git working-tree enumeration failed\n"
        )
        raise SystemExit(2)
    rels = sorted(
        rel
        for rel in proc.stdout.split("\0")
        if rel and _in_scope(rel) and (REPO_ROOT / rel).is_file()
    )
    counts = dict.fromkeys(SCOPE_ROOTS, 0)
    for rel in rels:
        root = next(root for root in SCOPE_ROOTS if rel.startswith(root))
        counts[root] += 1
    floor_errors = _scope_floor_errors(counts)
    if floor_errors:
        sys.stderr.write("check_no_stdio_streams.py: FATAL -- " + "; ".join(floor_errors) + "\n")
        raise SystemExit(2)
    return [REPO_ROOT / rel for rel in rels], counts


def _explicit_scope(raw_paths: Iterable[str]) -> list[pathlib.Path]:
    """Filter caller-named files through the same first-party scope policy."""
    paths: list[pathlib.Path] = []
    for raw in raw_paths:
        path = pathlib.Path(raw)
        absolute = path if path.is_absolute() else REPO_ROOT / path
        try:
            rel = absolute.resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            continue
        if _in_scope(rel) and absolute.is_file():
            paths.append(absolute)
    return sorted(set(paths))


def _scan_files(paths: Iterable[pathlib.Path]) -> tuple[int, list[tuple[str, Finding]]]:
    """Read and scan ``paths``, raising when a source file is unreadable."""
    findings: list[tuple[str, Finding]] = []
    scanned = 0
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.resolve().relative_to(REPO_ROOT).as_posix()
        scanned += 1
        findings.extend((rel, finding) for finding in scan_text(text))
    return scanned, findings


def _selftest_tokens(failures: list[str]) -> None:
    """Prove every forbidden identifier fires and legal lookalikes stay quiet."""
    bad = "\n".join(
        f"void *alias_{index} = (void *)&{token};" for index, token in enumerate(BANNED_TOKENS)
    )
    observed = {finding[2] for finding in scan_text(bad)}
    expect(observed == set(BANNED_TOKENS), "every forbidden token fires", failures)
    expect(
        bool(scan_text("#define HOST_OPEN fopen\n")),
        "a macro alias to a forbidden API fires",
        failures,
    )
    allowed = """
      char buffer[32];
      int n = snprintf(buffer, sizeof(buffer), "%u", 7U);
      int parsed = sscanf(buffer, "%d", &n);
      fw_fs_file_t file = {};
      ra8_io_stream_puts(&stream, "FILE stdout opendir printf");
      unsigned stdout_count = 0U;
      [[gnu::format(printf, 3, 4)]] void bounded_log(int, int, const char*, ...);
      /* FILE *ignored = fopen("x", "r"); DIR *dir = opendir("."); */
    """
    expect(not scan_text(allowed), "memory formatting/comments/lookalikes stay quiet", failures)


def _selftest_scope(failures: list[str]) -> None:
    """Prove every requested first-party root and only exact exclusions apply."""
    for root in SCOPE_ROOTS:
        expect(_in_scope(f"{root}future_portable.c"), f"{root} is in scope", failures)
    expect(
        _in_scope("apps/stand_alone/media_dl/src/main.c"),
        "production host tools are in scope",
        failures,
    )
    expect(
        _in_scope("apps/stand_alone/media_dl/tests/test_main.c"),
        "tool test fixtures are in scope",
        failures,
    )
    expect(_in_scope("tests/test_fs.c"), "unit tests are in scope", failures)
    expect(not _in_scope("libs/third_party/miniz/miniz.c"), "vendored SOUP is excluded", failures)
    generated = "libs/ra8_c6link/src/ra8_media_download.pb-c.c"
    expect(not _in_scope(generated), "registered generated protobuf source is excluded", failures)
    expect(
        _in_scope("libs/future/src/future.pb-c.c"),
        "a generated-looking future file is not automatically exempt",
        failures,
    )


def _selftest_floors(failures: list[str]) -> None:
    """Prove both per-root and aggregate non-vacuity floors bite."""
    good = {
        "libs/": 900,
        "port/": 90,
        "examples/": 430,
        "src/": 15,
        "coprocessor/": 0,
        "tools/": 200,
        "apps/": 120,
        "tests/": 700,
    }
    expect(not _scope_floor_errors(good), "current-shaped scope clears every floor", failures)
    root_short = dict(good)
    root_short["port/"] = 79
    expect(bool(_scope_floor_errors(root_short)), "a narrowed production root fails", failures)
    tests_short = dict(good)
    tests_short["tests/"] = 649
    expect(bool(_scope_floor_errors(tests_short)), "a narrowed test root fails", failures)
    # Every root exactly ON its floor, so only the aggregate can object. It has
    # to be recomputed whenever a root is added, or the new root's floor lifts
    # the sum back over TOTAL_FILE_FLOOR and this case stops testing anything.
    total_short = {
        "libs/": 850,
        "port/": 80,
        "examples/": 400,
        "src/": 10,
        "coprocessor/": 0,
        "tools/": 180,
        "apps/": 110,
        "tests/": 650,
    }
    expect(bool(_scope_floor_errors(total_short)), "the aggregate floor fails", failures)


def selftest() -> int:
    """Run both-direction token, scope, generated-source, and floor proofs."""
    print("check_no_stdio_streams.py --selftest")
    failures: list[str] = []
    expect(
        len(BANNED_TOKENS) == len(set(BANNED_TOKENS)),
        "token registry has no duplicates",
        failures,
    )
    _selftest_tokens(failures)
    _selftest_scope(failures)
    _selftest_floors(failures)
    return report(failures)


def _report_findings(scanned: int, findings: list[tuple[str, Finding]]) -> int:
    """Print the zero-baseline result and return its policy exit status."""
    if not findings:
        print(f"check_no_stdio_streams.py: {scanned} first-party source file(s), 0 findings.")
        return 0
    sys.stderr.write(
        "check_no_stdio_streams.py: C/POSIX stream API violation(s); use fw_fs_file_t, "
        "ra8_io/logging, or a raw bounded host adapter:\n"
    )
    for rel, finding in findings:
        line, column, token, source = finding
        sys.stderr.write(f"  {rel}:{line}:{column}: {token}: {source}\n")
    sys.stderr.write(f"\n{len(findings)} finding(s); baseline is zero.\n")
    return 1


def main(argv: list[str]) -> int:
    """Dispatch the selftest, full tracked sweep, or explicit-file scan."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan all tracked first-party source")
    parser.add_argument(
        "--selftest", action="store_true", help="prove the checker in both directions"
    )
    parser.add_argument("files", nargs="*", help="explicit source files")
    args = parser.parse_args(argv[1:])
    if args.selftest:
        if args.all or args.files:
            parser.error("--selftest accepts no other arguments")
        return selftest()
    if args.all and args.files:
        parser.error("--all accepts no explicit files")
    if not args.all and not args.files:
        parser.error("provide --all or at least one source file")
    try:
        paths, _counts = _working_scope() if args.all else (_explicit_scope(args.files), {})
        scanned, findings = _scan_files(paths)
    except OSError as exc:
        sys.stderr.write(f"check_no_stdio_streams.py: FATAL -- {exc}\n")
        return 2
    return _report_findings(scanned, findings)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

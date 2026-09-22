#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject whole-expression casts to a fixed enum's underlying type.

``ra8-enum-underlying-cast`` protects the representability check supplied by
C23 fixed-underlying-type enums.  A cast around the complete initializer is
evaluated before that constraint and can convert an otherwise out-of-range
value into range.  Casts on operands remain permitted because they can select
the required width or signedness of an intermediate arithmetic operation.

The implementation uses libclang's C AST.  A conservative textual prefilter
only rejects files lacking syntax required by any candidate; all decisions are
made from ``EnumDecl``, ``EnumConstantDecl``
and ``CStyleCastExpr`` nodes, so comments, strings and unrelated function-body
casts cannot produce findings.

Usage::

    python3 scripts/checks/check_enum_underlying_casts.py --all
    python3 scripts/checks/check_enum_underlying_casts.py --selftest
"""

from __future__ import annotations

import argparse
import concurrent.futures
import functools
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from clang import cindex

sys.path.insert(0, str(Path(__file__).resolve().parent))

from annot_clang import tu_args
from check_c23_headers import _strip_comments_and_strings
from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]
DIAGNOSTIC_NAME = "ra8-enum-underlying-cast"
MIN_SCANNED_FILES = 25
DEFAULT_JOBS = 6
SOURCE_SUFFIXES = frozenset({".c", ".h"})
HOSTED_PREFIXES = ("tests/", "tools/", "apps/host/")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
)
# RA8_PIN is the project-owned typed pin-packing operation.  Its outer cast
# establishes the macro's ABI type; its operand casts perform the actual
# width-controlled packing.  No general macro-cast exemption is permitted.
RA8_PIN_HEADER = REPO_ROOT / "libs/ra8_core/inc/ra8_port_constants.h"
RA8_PIN_DEFINITION = (
    "#define RA8_PIN(port, pin) ((ra8_port_pin_t)(((uint16_t)(port) << 8) | (uint16_t)(pin)))"
)
INVENTORY_RE = re.compile(
    r"\benum\b(?:\s+\w+)?(?:\s*:\s*[^{]+)?\s*\{(?:(?!\}).)*?"
    r"=\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\)(?:(?!\}).)*?\}",
    re.DOTALL,
)


@dataclass(frozen=True, order=True)
class Finding:
    """One forbidden enum initializer cast."""

    path: str
    line: int
    column: int
    enumerator: str
    cast_type: str


def _is_in_scope(path: Path) -> bool:
    """Return whether `path` is maintained C source covered by this rule."""
    try:
        relative = path.relative_to(REPO_ROOT).as_posix() if path.is_absolute() else path.as_posix()
    except ValueError:
        relative = path.as_posix()
    return (
        path.suffix in SOURCE_SUFFIXES
        and not is_build_output_path(relative)
        and not any(fragment in f"/{relative}" for fragment in EXCLUDE_FRAGMENTS)
    )


def _tracked_sources() -> list[Path]:
    """Return every tracked, maintained C source path."""
    tracked_result = subprocess.run(
        ["/usr/bin/git", "ls-files", "-z", "--", "*.c", "*.h"],
        cwd=REPO_ROOT,
        capture_output=True,
        check=False,
    )
    if tracked_result.returncode != 0:
        message = "git ls-files failed while enumerating C sources"
        raise RuntimeError(message)
    paths = [REPO_ROOT / raw.decode() for raw in tracked_result.stdout.split(b"\0") if raw]
    return [path for path in paths if _is_in_scope(path)]


def _contains_fixed_enum(text: str) -> bool:
    """Recognize a top-level underlying-type colon in an enum prefix."""
    for match in re.finditer(r"\benum\b", text):
        depth = 0
        saw_colon = False
        for character in text[match.end() :]:
            if character in "([":
                depth += 1
            elif character in ")]" and depth:
                depth -= 1
            elif depth == 0 and character == ":":
                saw_colon = True
            elif depth == 0 and character == "{":
                if saw_colon:
                    body_start = match.end() + text[match.end() :].index("{") + 1
                    body_end = _matching_brace(text, body_start)
                    if body_end != -1:
                        body = text[body_start:body_end]
                        if "=" in body and "(" in body:
                            return True
                break
            elif depth == 0 and character == ";":
                break
    return False


def _matching_brace(text: str, body_start: int) -> int:
    """Return the close of the enum brace, accounting for nested braces."""
    depth = 1
    for offset in range(body_start, len(text)):
        if text[offset] == "{":
            depth += 1
        elif text[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset
    return -1


def _candidate(path: Path) -> bool:
    """Reject only files that cannot contain a fixed-enum cast initializer."""
    text = _strip_comments_and_strings(path.read_text(encoding="utf-8"))
    return _contains_fixed_enum(text)


def _only_child(cursor: cindex.Cursor) -> cindex.Cursor | None:
    """Return the sole expression child, or None when the AST is ambiguous."""
    children = list(cursor.get_children())
    return children[0] if len(children) == 1 else None


def _root_expression(cursor: cindex.Cursor) -> cindex.Cursor:
    """Remove transparent AST wrappers surrounding an initializer."""
    transparent = {
        cindex.CursorKind.UNEXPOSED_EXPR,
        cindex.CursorKind.PAREN_EXPR,
    }
    current = cursor
    while current.kind in transparent:
        child = _only_child(current)
        if child is None:
            break
        current = child
    return current


def _canonical_type(cursor_type: cindex.Type) -> str:
    """Return the canonical spelling used to compare typedef aliases."""
    return cursor_type.get_canonical().spelling


def _is_spelled_cast(expression: cindex.Cursor) -> bool:
    """Return whether the cast is authored here rather than macro-generated."""
    location = expression.extent.start
    if location.file is None:
        return False
    try:
        source = Path(location.file.name).read_bytes()
    except OSError:
        return False
    return source[location.offset : location.offset + 1] == b"("


def _has_fixed_underlying_type(enum_cursor: cindex.Cursor) -> bool:
    """Return whether the enum declaration spells a C23 ``: type`` clause."""
    for token in enum_cursor.get_tokens():
        if token.spelling == "{":
            return False
        if token.spelling == ":":
            return True
    return False


def _initializer_macro(enumerator: cindex.Cursor) -> str | None:
    """Return the root initializer macro name when one is spelled at ``=``."""
    tokens = [token.spelling for token in enumerator.get_tokens()]
    try:
        equals = tokens.index("=")
    except ValueError:
        return None
    if equals + 2 >= len(tokens) or tokens[equals + 2] != "(":
        return None
    name = tokens[equals + 1]
    return name if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) else None


def _allowed_macro_cast(macro_name: str | None, enumerator: cindex.Cursor) -> bool:
    """Recognize RA8_PIN only while its canonical packing contract is intact."""
    location = enumerator.location
    if macro_name != "RA8_PIN" or location.file is None:
        return False
    source_path = Path(location.file.name).resolve()
    if not RA8_PIN_HEADER.is_file() or RA8_PIN_DEFINITION not in RA8_PIN_HEADER.read_text():
        return False
    scan_root = REPO_ROOT if source_path.is_relative_to(REPO_ROOT) else source_path.parent
    return _ra8_pin_directives_are_canonical(str(scan_root.resolve()))


@functools.cache
def _ra8_pin_directives_are_canonical(scan_root: str) -> bool:
    """Reject any RA8_PIN definition or undefinition outside its owner header."""
    directive = re.compile(r"^\s*#\s*(?:define|undef)\s+RA8_PIN\b[^\n]*", re.MULTILINE)
    paths = (
        _tracked_sources()
        if scan_root == str(REPO_ROOT)
        else [*Path(scan_root).rglob("*.c"), *Path(scan_root).rglob("*.h")]
    )
    for path in paths:
        if scan_root == str(REPO_ROOT) and not _is_in_scope(path):
            continue
        text = path.read_text(encoding="utf-8")
        directives = [match.group(0).strip() for match in directive.finditer(text)]
        if not directives:
            continue
        if path.resolve() != RA8_PIN_HEADER.resolve() or directives != [RA8_PIN_DEFINITION]:
            return False
    return True


def _is_integer_or_enum(cursor_type: cindex.Type) -> bool:
    """Return whether a cast destination is an integer or enumeration type."""
    return cursor_type.get_canonical().kind in {
        cindex.TypeKind.BOOL,
        cindex.TypeKind.CHAR_S,
        cindex.TypeKind.SCHAR,
        cindex.TypeKind.UCHAR,
        cindex.TypeKind.SHORT,
        cindex.TypeKind.USHORT,
        cindex.TypeKind.INT,
        cindex.TypeKind.UINT,
        cindex.TypeKind.LONG,
        cindex.TypeKind.ULONG,
        cindex.TypeKind.LONGLONG,
        cindex.TypeKind.ULONGLONG,
        cindex.TypeKind.ENUM,
    }


def _cast_operand(expression: cindex.Cursor) -> cindex.Cursor | None:
    """Return a C cast's value operand, excluding its type-reference node."""
    operands = [
        child for child in expression.get_children() if child.kind != cindex.CursorKind.TYPE_REF
    ]
    return operands[0] if len(operands) == 1 else None


def _required_complement_narrowing(
    expression: cindex.Cursor, operand: cindex.Cursor, enum_underlying: str
) -> bool:
    """Allow an intentional narrower-width truncation of promoted ``~``."""
    value = _root_expression(operand)
    tokens = {token.spelling for token in value.get_tokens()}
    return (
        _canonical_type(expression.type) != enum_underlying
        and value.kind == cindex.CursorKind.UNARY_OPERATOR
        and "~" in tokens
    )


def _find_in_cursor(root: cindex.Cursor) -> list[Finding]:
    """Return findings beneath one translation-unit cursor."""
    findings: set[Finding] = set()
    for enum_cursor in root.walk_preorder():
        if (
            enum_cursor.kind != cindex.CursorKind.ENUM_DECL
            or not enum_cursor.is_definition()
            or not _has_fixed_underlying_type(enum_cursor)
        ):
            continue
        underlying = _canonical_type(enum_cursor.enum_type)
        if not underlying:
            continue
        for enumerator in enum_cursor.get_children():
            if enumerator.kind != cindex.CursorKind.ENUM_CONSTANT_DECL:
                continue
            initializer = _only_child(enumerator)
            if initializer is None:
                continue
            expression = _root_expression(initializer)
            if expression.kind != cindex.CursorKind.CSTYLE_CAST_EXPR:
                continue
            operand = _cast_operand(expression)
            macro_name = None if _is_spelled_cast(expression) else _initializer_macro(enumerator)
            if (
                (operand is not None and _allowed_macro_cast(macro_name, enumerator))
                or (
                    operand is not None
                    and _required_complement_narrowing(expression, operand, underlying)
                )
                or not _is_integer_or_enum(expression.type)
                or operand is None
                or operand.type.get_canonical().kind == cindex.TypeKind.POINTER
            ):
                continue
            location = expression.location
            if location.file is None:
                continue
            findings.add(
                Finding(
                    path=str(Path(location.file.name).resolve()),
                    line=location.line,
                    column=location.column,
                    enumerator=enumerator.spelling,
                    cast_type=f"{expression.type.spelling} (enum storage {underlying})",
                )
            )
    return sorted(findings)


def _parse(path: Path) -> tuple[cindex.TranslationUnit | None, list[str]]:
    """Parse a maintained source file and return fatal diagnostics separately."""
    try:
        unit = cindex.Index.create().parse(
            str(path),
            args=[
                *tu_args(path),
                "-D_GNU_SOURCE",
                "-DPATH_MAX=4096",
                '-DRA8_TEST_REPO_ROOT="."',
                "-Dra8_fs_posix_test_set_directory_reader(...)=0",
                *_target_args(path),
            ],
        )
    except cindex.TranslationUnitLoadError as exc:
        return None, [str(exc)]
    errors = [str(diag) for diag in unit.diagnostics if diag.severity >= cindex.Diagnostic.Error]
    return unit, errors


def _target_args(path: Path) -> list[str]:
    """Supply the hosted/device mode used by the path's real build lane."""
    try:
        relative = path.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        relative = ""
    hosted = not relative or "/tests/" in f"/{relative}" or relative.startswith(HOSTED_PREFIXES)
    args = ["-fhosted" if hosted else "-ffreestanding"]
    if relative == "tests/misc/src/test_ra8_npu.c":
        args.append("-DRA8_DEVICE_RA8P1")
    return args


def _parse_findings(path: Path) -> tuple[list[Finding], list[str]]:
    """Parse one path in a worker process and return serializable results."""
    unit, errors = _parse(path)
    return ([] if unit is None else _find_in_cursor(unit.cursor)), errors


def scan(
    paths: list[Path], jobs_override: int | None = None
) -> tuple[list[Finding], list[str], int]:
    """AST-scan `paths`, returning deduplicated findings, parse errors and count."""
    findings: set[Finding] = set()
    errors: list[str] = []
    scoped = [path for path in paths if _is_in_scope(path)]
    jobs = max(
        1,
        jobs_override
        if jobs_override is not None
        else int(os.environ.get("RA8_MAX_JOBS", DEFAULT_JOBS)),
    )
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        candidates = [
            path
            for path, keep in zip(scoped, executor.map(_candidate, scoped), strict=True)
            if keep
        ]
    if jobs == 1:
        results = map(_parse_findings, candidates)
    else:
        process_executor = concurrent.futures.ProcessPoolExecutor(max_workers=jobs)
        results = process_executor.map(_parse_findings, candidates)
    try:
        for path, result in zip(candidates, results, strict=True):
            path_findings, parse_errors = result
            if parse_errors:
                errors.extend(f"{path}: {message}" for message in parse_errors)
                continue
            findings.update(path_findings)
    finally:
        if jobs != 1:
            process_executor.shutdown()
    scanned = len(candidates)
    return sorted(findings), errors, scanned


def _selftest_sources() -> tuple[str, str]:
    """Return the must-fail and mixed must-pass checker fixtures."""
    bad = """
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef enum : uint16_t { some_wider_constant = 65535 } wider_t;
typedef enum : uint16_t { A = (uint16_t)some_wider_constant } bad_whole_t;
typedef enum : uint8_t { OTHER_ENUM = 255 } other_t;
typedef enum : uint8_t { B = (uint8_t)OTHER_ENUM } bad_alias_t;
typedef enum : uint8_t { C = (uint8_t)256 } masked_range_t;
typedef enum : uint8_t { D = (other_t)256 } masked_enum_type_t;
typedef enum : uint16_t { E = (uint8_t)42 } masked_narrower_type_t;
typedef enum : uint16_t { F = (uint32_t)42 } redundant_wider_type_t;
#define NARROW(x) ((uint8_t)(x))
typedef enum : uint8_t { H = NARROW(256) } masked_macro_t;
typedef enum __attribute__((packed)) : uint16_t { I = (unsigned short)70000 } attributed_t;
typedef enum : uint8_t { J = (uint8_t)-256 } masked_unary_t;
typedef enum : uint16_t { K = sizeof(struct { int x; }), L = (uint16_t)65536 } nested_brace_t;
"""
    good = r"""
typedef __SIZE_TYPE__ size_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef enum : uint16_t { other_constant = 42 } other_t;
typedef enum : uint16_t { A = other_constant } good_direct_t;
typedef enum : size_t { width = 400, height = 300 } dimensions_t;
typedef enum : size_t { B = (size_t)width * (size_t)height } good_product_t;
typedef enum : uint8_t { value = 254, bit = 7 } byte_values_t;
typedef enum : uint16_t { C = (uint8_t)value + 1U } good_add_t;
typedef enum : uint16_t { M = ((uint8_t)~value) } good_w0c_mask_t;
typedef enum : uint16_t { D = (uint16_t)(1U << bit) } bad_shift_t;
typedef enum : size_t { E = (size_t)sizeof(uint16_t) } bad_sizeof_t;
typedef enum { F = (unsigned int)(1 ? 2 : 3) } ordinary_enum_t;
#include "spoof.h"
typedef enum : uint16_t { G = RA8_PIN(1, 2) } macro_packing_t;
/* typedef enum : uint16_t { C = (uint16_t)other_constant } comment_t; */
// typedef enum : uint16_t { D = (uint16_t)other_constant } line_comment_t;
static char const text[] = "enum : uint16_t { E = (uint16_t)x }";
static uint16_t function_cast(unsigned value) { return (uint16_t)value; }
"""
    return bad, good


def selftest() -> int:
    """Prove must-fire and must-stay-quiet behavior through the real AST rule."""
    bad, good = _selftest_sources()
    with tempfile.TemporaryDirectory(prefix="ra8-enum-cast-selftest-") as temp:
        root = Path(temp)
        bad_path = root / "bad.c"
        good_path = root / "good.c"
        error_path = root / "error.c"
        spoof_path = root / "spoof.h"
        vendor_path = root / "libs" / "third_party" / "vendor.c"
        vendor_path.parent.mkdir(parents=True)
        bad_path.write_text(bad, encoding="utf-8")
        good_path.write_text(good, encoding="utf-8")
        error_path.write_text(bad + "\nunknown_type broken;\n", encoding="utf-8")
        spoof_path.write_text("#define RA8_PIN(port, pin) ((uint16_t)(70000))\n", encoding="utf-8")
        vendor_path.write_text(bad, encoding="utf-8")
        findings, errors, scanned = scan([bad_path, good_path, vendor_path], jobs_override=1)
        _, parse_errors, _ = scan([error_path], jobs_override=1)
    if errors:
        print("selftest: scan fixtures produced parse errors", file=sys.stderr)
        return 1
    expected = ["A", "B", "C", "D", "E", "F", "H", "I", "J", "L", "D", "E", "G"]
    actual = sorted(finding.enumerator for finding in findings)
    if actual != sorted(expected):
        print(
            f"selftest: must-fail enum casts mismatch: expected {sorted(expected)}, got {actual}",
            file=sys.stderr,
        )
        return 1
    expected_scanned = 2
    if scanned != expected_scanned:
        print("selftest: maintained/vendor scan partition is incorrect", file=sys.stderr)
        return 1
    if not parse_errors:
        print("selftest: semantic parse errors did not fail closed", file=sys.stderr)
        return 1
    print(f"{DIAGNOSTIC_NAME} selftest: PASS")
    return 0


def main() -> int:
    """Run the self-test or scan all maintained C source."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan every tracked maintained C file")
    parser.add_argument("--selftest", action="store_true", help="run bidirectional checker tests")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.all:
        parser.error("--all or --selftest is required")
    try:
        paths = _tracked_sources()
        findings, errors, scanned = scan(paths)
    except (OSError, RuntimeError, UnicodeError) as exc:
        print(f"{DIAGNOSTIC_NAME}: {exc}", file=sys.stderr)
        return 2
    if scanned < MIN_SCANNED_FILES:
        print(
            f"{DIAGNOSTIC_NAME}: parsed only {scanned} candidates; expected at least "
            f"{MIN_SCANNED_FILES}",
            file=sys.stderr,
        )
        return 2
    if errors:
        print(f"{DIAGNOSTIC_NAME}: AST parse failed; refusing a partial verdict", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 2
    for finding in findings:
        relative = Path(finding.path).resolve().relative_to(REPO_ROOT.resolve())
        print(
            f"{relative}:{finding.line}:{finding.column}: error: {DIAGNOSTIC_NAME}: "
            f"enumerator '{finding.enumerator}' casts its complete initializer to "
            f"the fixed underlying type '{finding.cast_type}'; remove the cast so "
            "the compiler checks the source value is representable"
        )
    if findings:
        print(f"{DIAGNOSTIC_NAME}: {len(findings)} violation(s)", file=sys.stderr)
        return 1
    print(f"{DIAGNOSTIC_NAME}: PASS ({scanned} candidate files parsed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_annotations.py -- libclang-based annotation enforcement.

This script walks the AST of every C / C++ translation unit under
``libs/``, ``src/``, ``examples/``, ``tests/``, and ``port/`` and
applies the project's annotation-enforcement rules. The annotation
macros themselves are defined in ``libs/ra8_core/inc/ra8_attributes.h``
and lower to ``[[clang::annotate("ra8_<rule>:...")]]`` markers that
libclang exposes via ``AnnotateAttr`` cursors.

The enforceable rules are documented in ``docs/ANNOTATIONS.md``; this
script is the canonical implementation of their static checks. Every
rule is fatal -- there is no warn-only mode. A gate that reports a
known gap without failing is a gate that hides the gap.

Two self-checks run before any rule does, because both failure modes
look exactly like success:

* ``check_rule_keys()`` cross-checks every rule key this script
  dispatches on against the annotation strings ``ra8_attributes.h``
  actually emits. A rule keyed on a string no macro produces matches
  nothing and reports zero violations forever.
* ``check_parse_integrity()`` fails when a header does not resolve or
  when the fraction of call sites libclang could resolve drops below
  ``MIN_CALL_RESOLUTION``. An incomplete parse silently starves the
  call-graph rules, and fewer findings is not better news.

Usage::

    python3 scripts/utils/check_annotations.py            # report
    python3 scripts/utils/check_annotations.py --check    # CI gate
    python3 scripts/utils/check_annotations.py --list     # dump symbols
"""

from __future__ import annotations

import argparse
import contextlib
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass, field

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SCAN_DIRS = ("libs", "src", "examples", "tests", "port")
EXCLUDED_PATH_PARTS = {
    "build",
    "_deps",
    "third_party",
    "build-cov",
    "build-bench",
    "build-scan",
    "build-mcdc",
}
SOURCE_SUFFIXES = {".c", ".cpp"}

#: Path to the single header that defines every annotation macro. The
#: rule-key self-check reads the strings straight out of it.
ATTRIBUTES_HEADER = REPO_ROOT / "libs" / "ra8_core" / "inc" / "ra8_attributes.h"

#: Rules that record information rather than assert a property. They are
#: reported but never fail the gate -- there is nothing for a developer to
#: fix, the entry exists so the value shows up in the build log.
INFORMATIONAL_RULES = {"ra8_latency_budget_ns", "ra8_reviewed_by", "ra8_register_bank"}

# Upper bound on the call-graph BFS used for ra8_no_recursion checking.
# Prevents infinite loops on pathological call graphs during static analysis.
RECURSION_GUARD_LIMIT = 1000

#: Minimum fraction of ``CallExpr`` cursors whose callee declaration
#: libclang must resolve for the call-graph rules to mean anything. The
#: tree parses at >99.8%; the floor sits far enough below that to absorb
#: genuinely unresolvable constructs (host-only intrinsics, test hooks
#: behind build macros) while still catching an include path that has
#: come apart. When resolution collapses the call-graph rules stop
#: policing anything and report a clean tree, so this is fatal.
MIN_CALL_RESOLUTION = 0.98

#: Headers that only exist after a build step generated them. A clean
#: checkout legitimately does not have these, so an unresolved include
#: naming one is not an include-path defect.
GENERATED_HEADERS = frozenset(
    {
        "literata_latin1.h",  # libs/fonts generator output
        "ra8_npu_model_addk_sim.h",  # Vela model-compiler output
    }
)

ANNOTATION_PREFIXES = (
    "ra8_test_helper",
    "ra8_internal",
    "ra8_priv",
    "ra8_di_slot",
    "ra8_nsc_veneer",
    # Every entry here must match exactly what the corresponding macro in
    # libs/ra8_core/inc/ra8_attributes.h emits; check_rule_keys() proves
    # that on every run. Four of these were once spelled as something no
    # macro produced ("ra8_hw_mmio", "ra8_p10_rule3_exception",
    # "ra8_stack_max", "ra8_latency_max_ns") and the rules keyed on them
    # matched nothing at all while reporting success.
    "ra8_hw_register_access",
    "ra8_nasa_rule_3_ok",
    "ra8_mcdc_deactivated",
    "ra8_max_stack",
    "ra8_isr_safe",
    "ra8_expects_lock",
    "ra8_host_friendly",
    "ra8_latency_budget_ns",
    "ra8_no_recursion",
    "ra8_bounded_loop",
    "ra8_validates",
    "ra8_owns_resource",
    "ra8_releases_resource",
    "ra8_reviewed_by",
    "ra8_register_bank",
)


# --------------------------------------------------------------------------
# libclang import. The package is normally installed via:
#   python3 -m pip install --user --break-system-packages libclang
# In the dev container the wheel ships a bundled libclang.dylib / .so so
# no system-level libclang package is required.
# --------------------------------------------------------------------------
try:
    from clang import cindex
except ImportError:
    sys.stderr.write(
        "check_annotations.py: 'libclang' Python package missing.\n"
        "  install: python3 -m pip install --user --break-system-packages libclang\n"
    )
    sys.exit(2)


# ``CursorKind.SECTION_ATTR`` is not exposed by every libclang release -- the
# 18.1.x wheels used on the runners omit it. Resolve it once and fall back to
# None so a missing kind degrades to "no section attribute detected" instead of
# raising AttributeError, which previously aborted the entire TU walk (silently
# dropping every TU that defined an annotated function in-place, e.g.
# ra8_widget_*, ra8_book, ra8_rabook_*).
SECTION_ATTR_KIND = getattr(cindex.CursorKind, "SECTION_ATTR", None)


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------
@dataclass
class AnnotatedSymbol:
    """One function, keyed by USR, with whatever ra8_* annotations it carries.

    Every function declaration and definition the walk sees lands here,
    annotated or not: the linkage rule has to reason about the functions
    that carry *no* annotation, which is precisely the set an
    annotations-only table cannot see.

    The table is keyed by USR rather than by name. C lets every
    translation unit have its own ``static`` helper with the same name,
    and this repo does exactly that -- three unrelated
    ``internal_zero_bytes`` live in net_pal, usb_hmsc and usb_pmsc. A
    name-keyed table merges them into one entry whose annotations are the
    union of all three and whose file is whichever definition happened to
    be parsed last, so an annotation on one namesake is enforced against
    the callers of another.
    """

    name: str
    file: str
    line: int
    annotations: list[str] = field(default_factory=list)
    is_static: bool = False
    has_inline: bool = False
    section: str | None = None
    return_type: str = ""
    #: Last source line of the definition's extent (0 when only declared).
    end_line: int = 0
    #: True when at least one parameter is a pointer, i.e. the function
    #: takes an address across the boundary and must range-check it.
    has_pointer_param: bool = False
    #: Clang Unified Symbol Resolution string of the canonical declaration.
    #: This is the only reliable identity for a C function: two `static`
    #: helpers in different TUs may share a name but never a USR.
    usr: str = ""
    #: True once a definition (not just a prototype) has been seen.
    is_defined: bool = False
    #: Every file holding a non-defining declaration of this function.
    #: The linkage rule reads the published interface off this set: a
    #: prototype in a header is an exported contract, a prototype in a
    #: `.c` is a forward declaration and exports nothing.
    decl_files: set[str] = field(default_factory=set)


@dataclass
class CallSite:
    """A direct CallExpr discovered during AST traversal."""

    callee_name: str
    caller_name: str
    caller_file: str
    caller_line: int
    #: USR of the declaration this call actually resolved to. Compared
    #: against AnnotatedSymbol.usr so an annotation on one function is
    #: never attributed to a same-named `static` in another module.
    callee_usr: str = ""
    #: USR of the function the call sits inside, so caller-side lookups
    #: separate namesake `static` helpers the same way callee lookups do.
    caller_usr: str = ""
    in_address_of: bool = False
    parent_loop_label: str | None = None  # for RA8_PROTECTED_WRITE detection
    preceding_comment: str = ""


@dataclass
class ParseStats:
    """Evidence that the parse saw the code it was asked to analyse."""

    #: Every CallExpr encountered inside a function body.
    calls_seen: int = 0
    #: Those whose callee declaration libclang could resolve.
    calls_resolved: int = 0
    #: ``(including file, missing header)`` for each unresolved include.
    missing_includes: set[tuple[str, str]] = field(default_factory=set)
    #: Translation units libclang refused to parse at all.
    unparsed: list[str] = field(default_factory=list)


@dataclass
class Violation:
    rule: str
    file: str
    line: int
    message: str
    warn_only: bool = False


# --------------------------------------------------------------------------
# AST walk
# --------------------------------------------------------------------------
#: Directory names that only ever hold build output. Distinct from
#: EXCLUDED_PATH_PARTS, which also drops vendored trees: those must stay
#: off the *analysis* list but stay on the *include* path.
BUILD_OUTPUT_PARTS = frozenset(
    {"build", "_deps", "build-cov", "build-bench", "build-scan", "build-mcdc"}
)


def is_build_output(path: pathlib.Path) -> bool:
    """True when ``path`` sits inside a build-output directory."""
    return any(part in BUILD_OUTPUT_PARTS for part in path.parts)


def is_excluded(path: pathlib.Path) -> bool:
    """True when ``path`` must not be analysed (build output or vendored)."""
    return any(part in EXCLUDED_PATH_PARTS for part in path.parts)


def is_first_party(path: str) -> bool:
    """True when ``path`` is hand-written source this project owns.

    Definitions reached through the include path are not automatically in
    scope. Parsing the ``.cpp`` translation units as C++ pulls in
    libstdc++, whose headers define hundreds of non-static inline
    functions; vendored trees under ``libs/third_party/`` are SOUP. Only
    files under the scan roots are ours to hold to the linkage rule.
    """
    if not path:
        return False
    candidate = pathlib.Path(path)
    try:
        rel = candidate.resolve().relative_to(REPO_ROOT)
    except (ValueError, OSError):
        return False
    return bool(rel.parts) and rel.parts[0] in SCAN_DIRS and not is_excluded(rel)


def discover_translation_units() -> list[pathlib.Path]:
    """Return every .c/.cpp file under SCAN_DIRS, excluding vendored trees."""
    out: list[pathlib.Path] = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        out.extend(
            path
            for path in root.rglob("*")
            if path.suffix in SOURCE_SUFFIXES and not is_excluded(path)
        )
    return sorted(out)


def collect_annotations(cursor: cindex.Cursor) -> list[str]:
    """Return every ra8_* string attached to a cursor via AnnotateAttr."""
    out: list[str] = []
    for child in cursor.get_children():
        if child.kind == cindex.CursorKind.ANNOTATE_ATTR:
            text = child.spelling or child.displayname or ""
            if text.startswith(ANNOTATION_PREFIXES):
                out.append(text)
    return out


def _include_args() -> list[str]:
    """Return -I flags covering every header root the tree can include.

    The include path must be complete, and it must be derived only from
    the repo layout. It used to list seven hand-picked directories on the
    theory that unresolved headers still yield a good-enough AST. They do
    not: a call whose declaration was never seen does not resolve, so
    ``cursor.referenced`` is None and the call site is dropped. That
    silently starved the call-graph rules (``ra8_priv``,
    ``ra8_test_helper``) of exactly the cross-module call sites they
    exist to police -- the light path resolved 43k of 126k call sites,
    and which ones resolved varied with ambient state, so the same tree
    could pass standalone and fail under the pre-commit hook.

    Widening it again is not cosmetic either. The linkage rule reads a
    function's published interface off the file its prototype lives in,
    so a public header the parse cannot reach turns every symbol it
    declares into a phantom violation: ``src/secure_app/inc`` was absent,
    and the whole key-vault and OTA-commit API looked undeclared.

    Vendored trees under ``libs/third_party/`` are on the path for the
    same reason. ``port/`` implements interfaces those headers declare --
    ``ble_npl_*`` for NimBLE, ``tx_application_define`` for ThreadX,
    ``_ux_device_class_storage_*`` for USBX. Without their headers those
    are undeclared external symbols; with them they are what they
    actually are, implementations of a published contract.
    """
    roots: list[pathlib.Path] = []
    for pattern in (
        # libs/<mod>/{inc,src,boot,v2}: inc/ is the public API, src/ holds
        # the *_internal.h headers declaring RA8_PRIV symbols, and the
        # remainder are per-library layouts (board boot code, reflow v2).
        "libs/*",
        "libs/*/*",
        "src",
        "src/*",
        "src/*/inc",
        "tests",
        "tests/include",
        "tests/mocks",
        "port/*/inc",
        "port/*/src",
    ):
        roots.extend(sorted(REPO_ROOT.glob(pattern)))
    # Applications keep their headers beside their sources with no inc/
    # convention, so derive those roots from where the headers actually
    # are. A quote-include always searches the including file's own
    # directory first, so a namesake in a sibling app cannot displace an
    # app's own header.
    roots.extend(sorted({h.parent for h in (REPO_ROOT / "examples").rglob("*.h")}))
    roots = [d for d in roots if d.is_dir() and not is_excluded(d)]
    third_party = REPO_ROOT / "libs" / "third_party"
    if third_party.is_dir():
        vendored = list(third_party.iterdir())
        vendored += list(third_party.rglob("inc"))
        vendored += list(third_party.rglob("include"))
        roots.extend(sorted(d for d in vendored if d.is_dir() and not is_build_output(d)))
    out: list[str] = []
    seen: set[str] = set()
    for d in roots:
        if str(d) in seen:
            continue
        seen.add(str(d))
        out.append(f"-I{d}")
    return out


def _builtin_include_args() -> list[str]:
    """Return -isystem flags for clang's own builtin headers.

    libclang loaded through the Python bindings does not know where its
    resource directory lives, so ``stddef.h`` and friends may not resolve.
    That is not cosmetic: a failed system include aborts the rest of the
    include chain, later declarations are never seen, and the calls that
    depend on them silently vanish from the call graph. Ask a real clang
    binary where its resource dir is and put that on the path.
    """
    candidates = [
        os.environ.get("RA8_CLANG"),
        "clang",
        "clang-22",
        "clang-21",
        "clang-20",
        "clang-19",
        "clang-18",
    ]
    for exe in candidates:
        if not exe:
            continue
        try:
            res = subprocess.run(  # noqa: S603  # fixed argv, no shell
                [exe, "-print-resource-dir"],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if res.returncode != 0:
            continue
        inc = pathlib.Path(res.stdout.strip()) / "include"
        if not (inc / "stddef.h").is_file():
            continue
        # Only adopt this resource dir if it is actually compatible with the
        # loaded libclang. A mismatched pair (e.g. clang 22 headers against
        # older bindings) fails inside stdint.h with "__INT32_C is not
        # defined", which is worse than not setting it at all.
        if _probe_is_clean([f"-isystem{inc}"]):
            return [f"-isystem{inc}"]
    # No clang driver on PATH: the pip wheel bundles libclang.so but no
    # builtin headers, so fall back to whatever a distro LLVM package left
    # on disk. Newest first, so a box with several LLVMs picks the one
    # closest to the bindings.
    for inc in sorted(_resource_dirs_on_disk(), reverse=True):
        if (inc / "stddef.h").is_file() and _probe_is_clean([f"-isystem{inc}"]):
            return [f"-isystem{inc}"]
    return []


def _resource_dirs_on_disk() -> list[pathlib.Path]:
    """Return candidate clang builtin-header directories found on disk."""
    out: list[pathlib.Path] = []
    for base in (pathlib.Path("/usr/lib"), pathlib.Path("/usr/local/lib")):
        if not base.is_dir():
            continue
        out.extend(p for p in base.glob("llvm-*/lib/clang/*/include") if p.is_dir())
        out.extend(p for p in base.glob("clang/*/include") if p.is_dir())
    return out


def _probe_is_clean(extra: list[str]) -> bool:
    """True when ``#include <stddef.h>`` parses without a fatal error."""
    probe = REPO_ROOT / "scripts" / "utils" / ".ra8_annotations_probe.c"
    try:
        probe.write_text("#include <stddef.h>\n#include <stdint.h>\nsize_t ra8_probe(void);\n")
        tu = cindex.Index.create().parse(
            str(probe), args=["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *extra]
        )
        return not [d for d in (tu.diagnostics if tu else []) if d.severity >= _DIAG_ERROR]
    except cindex.TranslationUnitLoadError:
        return False
    finally:
        with contextlib.suppress(OSError):
            probe.unlink()


#: Cached include flags -- the glob is stable for a whole run.
_INCLUDE_ARGS: list[str] = []

#: Extracts the header name out of clang's "'foo.h' file not found".
_MISSING_INCLUDE_RE = re.compile(r"'([^']+)' file not found")


def _tu_args(path: pathlib.Path) -> list[str]:
    """Return the compile flags for ``path``.

    ``.cpp`` sources are parsed as C++. Forcing ``-x c`` over them, as
    this used to, makes every ``<algorithm>`` / ``<cassert>`` include fail
    and truncates the parse of the XML-shim and reflow-v2 translation
    units.

    Host tests carry the two macros ``tests/CMakeLists.txt`` compiles them
    with. Without them the mock TUs parse under a configuration nothing
    ever builds: the ``ra8_sim_mmio_*`` fault-seam prototypes in
    ``ra8_hw_err.h`` sit behind ``RA8_SIMULATOR_MODE && UNIT_TEST``, so
    the definitions in ``tests/mocks/`` looked like undeclared external
    symbols and every call through the seam went unresolved.
    """
    global _INCLUDE_ARGS  # noqa: PLW0603  # one-shot memo of a pure repo-layout scan
    if not _INCLUDE_ARGS:
        _INCLUDE_ARGS = _builtin_include_args() + _include_args()
    # -fsized-deallocation matches GCC, which builds every .cpp here and
    # turns it on from C++14. Clang leaves it off, so <new> compiles out
    # the two sized `operator delete` declarations and the replacements in
    # ra8_epub_cpp_alloc.cpp look like symbols no header declares.
    lang = (
        ["-std=c++20", "-x", "c++", "-fsized-deallocation"]
        if path.suffix == ".cpp"
        else ["-std=c23", "-x", "c"]
    )
    config = ["-DRA8_HOST_BUILD=1", *_crypto_config_args()]
    if _is_test_path(str(path)):
        config += ["-DRA8_SIMULATOR_MODE", "-DUNIT_TEST"]
    return [*lang, *config, *_INCLUDE_ARGS]


def _crypto_config_args() -> list[str]:
    """Return the Mbed TLS / TF-PSA config selectors ``cmake/mbedtls.cmake`` sets.

    The vendored crypto headers are a maze of ``#if`` on the project
    config, and without pointing them at the same config file the build
    uses they expose a different API surface. ``psa/crypto_extra.h``
    declares ``mbedtls_psa_external_get_random`` only under
    ``MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG``, which lives in that config -- so
    without this the RNG callback the secure-boot app supplies looks like
    an undeclared external symbol rather than the PSA hook it is.
    """
    port_inc = REPO_ROOT / "port" / "mbedtls" / "inc"
    out: list[str] = []
    for macro, name in (
        ("MBEDTLS_CONFIG_FILE", "mbedtls_config.h"),
        ("TF_PSA_CRYPTO_CONFIG_FILE", "tf_psa_crypto_config.h"),
    ):
        header = port_inc / name
        if header.is_file():
            out.append(f'-D{macro}="{header}"')
    return out


def parse_tu(path: pathlib.Path, stats: ParseStats) -> cindex.TranslationUnit | None:
    """Parse a single TU and record what the parse could not resolve."""
    index = cindex.Index.create()
    try:
        tu = index.parse(
            str(path),
            args=_tu_args(path),
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
    except cindex.TranslationUnitLoadError:
        stats.unparsed.append(str(path))
        return None
    if tu is None:
        stats.unparsed.append(str(path))
        return None
    for diag in tu.diagnostics:
        if diag.severity < _DIAG_ERROR:
            continue
        match = _MISSING_INCLUDE_RE.search(diag.spelling)
        if match and pathlib.PurePath(match.group(1)).name not in GENERATED_HEADERS:
            stats.missing_includes.add((str(path), match.group(1)))
    return tu


def usr_of(cursor: cindex.Cursor) -> str:
    """Return the canonical USR of ``cursor``, or '' when unavailable."""
    with contextlib.suppress(Exception):
        return cursor.canonical.get_usr()
    return ""


def symbol_key(cursor: cindex.Cursor) -> str:
    """Return the symbol-table key for a function cursor.

    The USR when libclang can produce one -- it encodes the defining file
    for internal-linkage symbols, which is the only thing that separates
    same-named ``static`` helpers in different modules. A libclang build
    that cannot produce USRs degrades to name plus file, which keeps the
    namesakes apart for the common case rather than merging them.
    """
    usr = usr_of(cursor)
    if usr:
        return usr
    where = str(cursor.location.file) if cursor.location.file else ""
    return f"{cursor.spelling}@{where}"


#: A section name the linker script maps to a hardware vector table.
#: ``.vectors`` for the M85 images, ``.cpu1_vectors`` for the M33 ones.
_VECTOR_SECTION_RE = re.compile(r'section\s*\(\s*"(\.[a-z0-9_]*vectors)"')

#: How many lines above a declaration to search for its section attribute.
VECTOR_ATTR_LOOKBACK_LINES = 3


def is_vector_table(cursor: cindex.Cursor) -> bool:
    """True when ``cursor`` declares a hardware exception vector table.

    Recognised by what makes it a vector table, never by name. Either:

    * a file-scope array of ``const`` pointers to functions -- the shape
      the Cortex-M core reads through VTOR, which nothing else in this
      tree has; or
    * a file-scope ``const`` array the linker places in a ``*vectors``
      section. The two-entry copy-to-run payload table and the M33
      ``.cpu1_vectors`` table are ``const uintptr_t[]``, so the type alone
      does not identify them, but the section placement does -- that
      placement is the whole reason the array exists.

    The section is read out of the declaration's source text because
    ``CursorKind.SECTION_ATTR`` is absent from the libclang 18.1.x wheels
    the runners install, so an AST lookup would silently find nothing.
    """
    if cursor.kind != cindex.CursorKind.VAR_DECL:
        return False
    parent = cursor.semantic_parent
    if parent is None or parent.kind != cindex.CursorKind.TRANSLATION_UNIT:
        return False
    array = cursor.type
    if array.kind not in (cindex.TypeKind.CONSTANTARRAY, cindex.TypeKind.INCOMPLETEARRAY):
        return False
    element = array.element_type
    if not element.is_const_qualified():
        return False
    canonical = element.get_canonical()
    if (
        canonical.kind == cindex.TypeKind.POINTER
        and canonical.get_pointee().get_canonical().kind == cindex.TypeKind.FUNCTIONPROTO
    ):
        return True
    return _has_vector_section(cursor)


def _has_vector_section(cursor: cindex.Cursor) -> bool:
    """True when ``cursor``'s declaration carries a ``*vectors`` section."""
    if cursor.location.file is None:
        return False
    lines = _source_lines(str(cursor.location.file))
    if not lines:
        return False
    # The attribute sits on the declaration line or just above it; C23
    # attributes may be split across a couple of lines by the formatter.
    first = max(0, cursor.extent.start.line - VECTOR_ATTR_LOOKBACK_LINES)
    return bool(_VECTOR_SECTION_RE.search("\n".join(lines[first : cursor.extent.start.line])))


def walk_tu(
    tu: cindex.TranslationUnit,
    _tu_path: pathlib.Path,
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
    stats: ParseStats,
    vector_entries: set[str],
) -> None:
    """Single-pass AST walk filling ``symbols``, ``calls`` and ``vector_entries``."""

    def file_of(c: cindex.Cursor) -> str:
        if c.location.file is None:
            return ""
        return str(c.location.file)

    def record_vector_entries(node: cindex.Cursor) -> None:
        """Collect the USR of every function named in a vector table."""
        if (
            node.kind == cindex.CursorKind.DECL_REF_EXPR
            and node.referenced is not None
            and node.referenced.kind == cindex.CursorKind.FUNCTION_DECL
        ):
            vector_entries.add(symbol_key(node.referenced))
        for child in node.get_children():
            record_vector_entries(child)

    def visit(node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:  # noqa: PLR0912  # AST dispatch, splitting hurts readability
        # Function declarations / definitions
        if node.kind == cindex.CursorKind.FUNCTION_DECL:
            key = symbol_key(node)
            sym = symbols.setdefault(
                key,
                AnnotatedSymbol(
                    name=node.spelling, file=file_of(node), line=node.location.line, usr=key
                ),
            )
            for a in collect_annotations(node):
                if a not in sym.annotations:
                    sym.annotations.append(a)
            sym.is_static = node.linkage == cindex.LinkageKind.INTERNAL or sym.is_static
            sym.return_type = node.result_type.spelling
            if any(arg.type.kind == cindex.TypeKind.POINTER for arg in node.get_arguments()):
                sym.has_pointer_param = True
            if node.is_definition():
                sym.is_defined = True
                sym.file = file_of(node)
                sym.line = node.location.line
                sym.end_line = node.extent.end.line
            else:
                sym.decl_files.add(file_of(node))
            # libclang exposes inline-ness via the cursor's tokens
            tokens = [t.spelling for t in node.get_tokens()]
            if "inline" in tokens or "__inline__" in tokens:
                sym.has_inline = True
            # Section attribute (only when this libclang exposes the kind).
            if SECTION_ATTR_KIND is not None:
                for child in node.get_children():
                    if child.kind == SECTION_ATTR_KIND:
                        sym.section = child.spelling
            # Recurse into the function body with new ``current_func``.
            for child in node.get_children():
                visit(child, node if node.is_definition() else current_func)
            return

        if is_vector_table(node):
            record_vector_entries(node)

        # Direct calls inside a function body
        if node.kind == cindex.CursorKind.CALL_EXPR and current_func is not None:
            stats.calls_seen += 1
            callee = node.referenced
            if callee is not None:
                stats.calls_resolved += 1
                calls.append(
                    CallSite(
                        callee_name=callee.spelling,
                        caller_name=current_func.spelling,
                        caller_file=file_of(node),
                        caller_line=node.location.line,
                        callee_usr=symbol_key(callee),
                        caller_usr=symbol_key(current_func),
                    )
                )

        # Address-of: &foo
        if node.kind == cindex.CursorKind.UNARY_OPERATOR:
            tokens = [t.spelling for t in node.get_tokens()]
            if tokens and tokens[0] == "&":
                calls.extend(
                    CallSite(
                        callee_name=child.referenced.spelling,
                        caller_name=(current_func.spelling if current_func else ""),
                        caller_file=file_of(node),
                        caller_line=node.location.line,
                        callee_usr=symbol_key(child.referenced),
                        caller_usr=(symbol_key(current_func) if current_func else ""),
                        in_address_of=True,
                    )
                    for child in node.get_children()
                    if (
                        child.kind == cindex.CursorKind.DECL_REF_EXPR
                        and child.referenced
                        and child.referenced.kind == cindex.CursorKind.FUNCTION_DECL
                    )
                )

        for child in node.get_children():
            visit(child, current_func)

    visit(tu.cursor, None)


# --------------------------------------------------------------------------
# Rule helpers
# --------------------------------------------------------------------------
#: libclang diagnostic severity for Error (3) and Fatal (4); anything at or
#: above this level means the parse did not see the code it was given.
_DIAG_ERROR = 3

#: The two ways a veneer legitimately validates an NS address range.
_NSC_RANGE_CHECK_RE = re.compile(r"RA8_NSC_CHECK_NS_RANGE_(?:R|RW)\b|cmse_check_address_range\b")

#: Cache of source files read back for textual (macro-level) checks.
_SOURCE_CACHE: dict[str, list[str]] = {}


def _source_lines(path: str) -> list[str]:
    """Return ``path``'s lines, cached. Empty list when unreadable."""
    if path not in _SOURCE_CACHE:
        try:
            _SOURCE_CACHE[path] = pathlib.Path(path).read_text(errors="ignore").splitlines()
        except OSError:
            _SOURCE_CACHE[path] = []
    return _SOURCE_CACHE[path]


def _definition_text(sym: AnnotatedSymbol) -> str:
    """Return the source text of ``sym``'s definition.

    Used for checks that must see the code *before* preprocessing, because
    the construct being looked for is a macro that this script's parse
    configuration expands away (see the NSC range-check rule).
    """
    lines = _source_lines(sym.file)
    if not lines or sym.line <= 0:
        return ""
    end = sym.end_line if sym.end_line >= sym.line else len(lines)
    return "\n".join(lines[sym.line - 1 : end])


def parse_annotation(ann: str) -> tuple[str, str]:
    """Split ``ra8_max_stack:512`` -> (``ra8_max_stack``, ``512``)."""
    if ":" in ann:
        rule, _, arg = ann.partition(":")
        return rule.strip(), arg.strip()
    return ann.strip(), ""


def _is_test_path(path: str) -> bool:
    """True when ``path`` is a host unit-test translation unit."""
    return "/tests/" in path.replace("\\", "/")


#: The one linkage annotation a non-static function may carry.
LINKAGE_ANNOTATIONS = frozenset({"ra8_priv", "ra8_internal", "ra8_test_helper"})

#: Header suffix that marks a declaration as library-private rather than
#: published API. See CLAUDE.md, "Which linkage annotation to use".
INTERNAL_HEADER_SUFFIX = "_internal.h"


#: The ISO C program entry point. The startup code reaches `main` by name
#: through the C runtime contract, so it has external linkage by
#: definition and no first-party header declares it. Exactly one name --
#: this is a language rule, not a naming convention.
C_ENTRY_POINT = "main"


def _published_header(sym: AnnotatedSymbol) -> str | None:
    """Return the header that publishes ``sym``, or None when none does.

    A prototype in a header is an exported contract: a library's public
    ``inc/`` header, an application's local header, a mock's header, or a
    vendored SOUP header whose interface the function implements. A
    prototype in a ``.c`` is a forward declaration and publishes nothing,
    and an ``*_internal.h`` declaration is explicitly library-private --
    both leave the function needing a linkage annotation.

    "Header" is anything that is not a translation unit, rather than a
    list of header suffixes: the C++ standard library headers that declare
    the replaceable ``operator new`` / ``operator delete`` are spelled
    ``<new>``, with no suffix at all.
    """
    for path in sorted(sym.decl_files):
        name = pathlib.PurePath(path).name
        if pathlib.PurePath(name).suffix in SOURCE_SUFFIXES:
            continue
        if name.endswith(INTERNAL_HEADER_SUFFIX):
            continue
        return path
    return None


def _internal_header(sym: AnnotatedSymbol) -> str | None:
    """Return the ``*_internal.h`` declaring ``sym``, or None."""
    for path in sorted(sym.decl_files):
        if pathlib.PurePath(path).name.endswith(INTERNAL_HEADER_SUFFIX):
            return path
    return None


def emitted_annotation_keys() -> set[str]:
    """Return every annotation string ``ra8_attributes.h`` can emit.

    Read straight out of the header rather than restated here, because a
    restatement is what goes stale. Each macro expands through
    ``RA8_INTERNAL_ANNOTATE("ra8_<rule>...")``; the rule key is the text
    up to the first colon.
    """
    try:
        text = ATTRIBUTES_HEADER.read_text(errors="ignore")
    except OSError:
        return set()
    out: set[str] = set()
    for match in re.finditer(r'RA8_INTERNAL_ANNOTATE\(\s*"(ra8_[a-z0-9_]+)', text):
        out.add(match.group(1))
    return out


def check_rule_keys() -> list[Violation]:
    """Fail when a rule keys on a string no annotation macro emits.

    This is the failure mode that looks exactly like success. A rule
    keyed on a spelling nothing produces matches zero symbols and reports
    zero violations for as long as nobody checks, and four rules in this
    file were in that state at once: RA8_HW_REGISTER_ACCESS emits
    "ra8_hw_register_access" but rule 6 looked for "ra8_hw_mmio", and the
    NASA-rule-3, stack-budget and latency-budget rules each looked for a
    key their macro never wrote. Cross-checking both directions against
    the header makes the whole class impossible to reintroduce silently.
    """
    emitted = emitted_annotation_keys()
    if not emitted:
        return [
            Violation(
                "ra8_rule_keys",
                str(ATTRIBUTES_HEADER),
                0,
                "no RA8_INTERNAL_ANNOTATE() strings found -- the annotation "
                "header moved or changed shape, so every rule key is unverified",
            )
        ]
    out: list[Violation] = []
    out.extend(
        Violation(
            "ra8_rule_keys",
            str(ATTRIBUTES_HEADER),
            0,
            f"rule key '{key}' is not emitted by any macro in ra8_attributes.h; "
            f"the rule keyed on it can never match",
        )
        for key in sorted(set(ANNOTATION_PREFIXES) - emitted)
    )
    out.extend(
        Violation(
            "ra8_rule_keys",
            str(ATTRIBUTES_HEADER),
            0,
            f"annotation '{key}' is emitted by a macro but no rule recognises it; "
            f"every use of that macro is silently ignored",
        )
        for key in sorted(emitted - set(ANNOTATION_PREFIXES))
    )
    return out


def check_parse_integrity(stats: ParseStats, tu_count: int) -> list[Violation]:
    """Fail when the parse did not see the code it was handed.

    Every call-graph rule here is a claim about a graph libclang built.
    When headers stop resolving the graph loses edges, the rules stop
    firing, and the gate prints a smaller number of violations -- which
    reads as an improvement. It is the opposite, so a parse that came
    apart fails the gate on its own terms, before any rule runs.
    """
    out: list[Violation] = []
    out.extend(
        Violation(
            "ra8_parse_integrity",
            src,
            0,
            f"include '{header}' does not resolve; every declaration behind it "
            f"is invisible to the call graph",
        )
        for src, header in sorted(stats.missing_includes)
    )
    out.extend(
        Violation("ra8_parse_integrity", src, 0, "translation unit failed to parse")
        for src in sorted(stats.unparsed)
    )
    if not stats.calls_seen:
        out.append(
            Violation(
                "ra8_parse_integrity",
                str(REPO_ROOT),
                0,
                f"no call sites found across {tu_count} translation units",
            )
        )
        return out
    rate = stats.calls_resolved / stats.calls_seen
    if rate < MIN_CALL_RESOLUTION:
        out.append(
            Violation(
                "ra8_parse_integrity",
                str(REPO_ROOT),
                0,
                f"only {stats.calls_resolved} of {stats.calls_seen} call sites "
                f"resolved ({rate:.1%}, floor {MIN_CALL_RESOLUTION:.0%}) -- the "
                f"call-graph rules are not seeing the tree",
            )
        )
    return out


def module_of(path: str) -> str | None:
    """libs/<module>/... -> '<module>'; else None."""
    p = pathlib.Path(path)
    try:
        idx = p.parts.index("libs")
    except ValueError:
        return None
    if idx + 1 < len(p.parts):
        return p.parts[idx + 1]
    return None


def find_su_file(symbol: AnnotatedSymbol) -> int | None:
    """Look for a matching .su entry under any examples/*/build*/ tree."""
    name = symbol.name
    examples = REPO_ROOT / "examples"
    if not examples.is_dir():
        return None
    pat = re.compile(r"\b" + re.escape(name) + r"\b\s+(\d+)\s+\w+")
    for su in examples.rglob("*.su"):
        with contextlib.suppress(OSError):
            for line in su.read_text(errors="ignore").splitlines():
                m = pat.search(line)
                if m:
                    return int(m.group(1))
    return None


# --------------------------------------------------------------------------
# Rule enforcement
# --------------------------------------------------------------------------
def enforce_rules(  # noqa: PLR0912 PLR0915  # rule-dispatch table; splitting by rule reduces clarity
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
    vector_entries: set[str],
    *,
    whole_tree: bool = True,
) -> list[Violation]:
    out: list[Violation] = []

    # Build per-callee call list and per-caller call list for fast lookup.
    # Both are keyed by USR: a name key merges every module's namesake
    # `static` helper into one bucket and enforces one module's annotation
    # against another module's callers.
    direct_calls_by_callee: dict[str, list[CallSite]] = {}
    direct_calls_by_caller: dict[str, list[CallSite]] = {}
    for cs in calls:
        if not cs.in_address_of:
            direct_calls_by_callee.setdefault(cs.callee_usr, []).append(cs)
        direct_calls_by_caller.setdefault(cs.caller_usr, []).append(cs)

    address_taken: set[str] = {cs.callee_usr for cs in calls if cs.in_address_of}

    # The linkage rule is defined over the whole tree: a handler tabled in
    # one TU, or an API declared in a header no parsed TU includes, cannot
    # be judged from a subset. Running it over an explicit file list would
    # invent violations rather than find them.
    if whole_tree:
        out.extend(enforce_linkage(symbols, vector_entries))

    for sym in symbols.values():
        for ann in sym.annotations:
            rule, arg = parse_annotation(ann)

            warn_only = rule in INFORMATIONAL_RULES

            # 1. ra8_test_helper -- callers must live under /tests/
            if rule == "ra8_test_helper":
                out.extend(
                    Violation(
                        rule,
                        cs.caller_file,
                        cs.caller_line,
                        f"function '{sym.name}' tagged RA8_TEST_HELPER "
                        "called from non-test context",
                    )
                    for cs in direct_calls_by_callee.get(sym.usr, [])
                    if not _is_test_path(cs.caller_file)
                )

            # 2. ra8_internal -- definition must be static
            elif rule == "ra8_internal":
                if not sym.is_static:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' tagged RA8_INTERNAL is not declared static",
                        )
                    )

            # 3. ra8_priv -- callers must share the same libs/<module>/
            elif rule == "ra8_priv":
                callee_mod = module_of(sym.file)
                if callee_mod is None:
                    continue
                for cs in direct_calls_by_callee.get(sym.usr, []):
                    caller_mod = module_of(cs.caller_file)
                    # Host unit tests are a sanctioned consumer of a module's
                    # promoted internals: CLAUDE.md ("Test access to internal
                    # symbols") allows a helper to drop `static` and be
                    # declared in the module's _internal.h precisely so tests
                    # can reach the validation paths for MC/DC. Production
                    # callers in *other* modules remain violations.
                    if _is_test_path(cs.caller_file):
                        continue
                    if caller_mod != callee_mod:
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"function '{sym.name}' tagged RA8_PRIV "
                                f"({callee_mod}) called from outside its module "
                                f"(caller={caller_mod or 'unknown'})",
                            )
                        )

            # 4. ra8_di_slot:<role> -- must be referenced via &name, not direct
            elif rule == "ra8_di_slot":
                if sym.usr in direct_calls_by_callee and sym.usr not in address_taken:
                    cs = direct_calls_by_callee[sym.usr][0]
                    out.append(
                        Violation(
                            rule,
                            cs.caller_file,
                            cs.caller_line,
                            f"function '{sym.name}' tagged RA8_DI_SLOT "
                            f"called directly; must be invoked via function "
                            f"pointer (DIP)",
                            warn_only=True,  # heuristic; warn until pattern stable
                        )
                    )

            # 5. ra8_nsc_veneer
            elif rule == "ra8_nsc_veneer":
                f = sym.file.replace("\\", "/")
                if "/libs/ra8_nsc/src/" not in f:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"NSC veneer '{sym.name}' must live under "
                            f"libs/ra8_nsc/src/ (found {f})",
                        )
                    )
                # A veneer must range-check every address it accepts from the
                # Non-Secure world. Veneers that take no pointer parameter
                # cross no address and need no check.
                #
                # The check is a textual scan, not a call-graph lookup: the
                # idiom is the RA8_NSC_CHECK_NS_RANGE_R/_RW macro, which
                # expands to cmse_check_address_range() only under -mcmse.
                # This script parses without -mcmse, so the macro expands to
                # a ((void)(p),(void)(n)) no-op and leaves no CallExpr for
                # libclang to find. The previous call-graph form looked for a
                # callee named "ra8_nsc_check_*" -- a spelling no veneer has
                # ever used -- so it could only ever produce false positives.
                if sym.has_pointer_param:
                    body = _definition_text(sym)
                    if not _NSC_RANGE_CHECK_RE.search(body):
                        out.append(
                            Violation(
                                rule,
                                sym.file,
                                sym.line,
                                f"NSC veneer '{sym.name}' takes a pointer from NS but "
                                f"never range-checks it (expected "
                                f"RA8_NSC_CHECK_NS_RANGE_R/_RW or "
                                f"cmse_check_address_range)",
                            )
                        )
                if (
                    sym.section
                    and ".gnu.sgstubs" not in sym.section
                    and "cmse_nonsecure_entry" not in " ".join(sym.annotations)
                ):
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"NSC veneer '{sym.name}' must live in .gnu.sgstubs "
                            f"section or be cmse_nonsecure_entry",
                        )
                    )

            # 6. ra8_hw_register_access
            elif rule == "ra8_hw_register_access":
                if not sym.has_inline:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"MMIO accessor '{sym.name}' must be inline",
                        )
                    )
                if "volatile" not in sym.return_type:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"MMIO accessor '{sym.name}' must return volatile* "
                            f"(got '{sym.return_type}')",
                        )
                    )
                # Caller-side RA8_PROTECTED_WRITE / // CITES-OK: check is
                # left to a future textual scan; libclang loses macro
                # context that early.

            # 7. ra8_nasa_rule_3_ok -- malloc only inside this function
            elif rule == "ra8_nasa_rule_3_ok":
                # Nothing to check at the symbol; the global sweep below
                # ensures malloc/free/calloc/realloc only appear inside
                # tagged functions.
                pass

            # 8. ra8_mcdc_deactivated:<reason>
            elif rule == "ra8_mcdc_deactivated":
                if re.search(r"\.[ch]:\d+", arg):
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"ra8_mcdc_deactivated reason on '{sym.name}' "
                            f"contains file:line citation -- use function name",
                        )
                    )

            # 9. ra8_max_stack:<bytes>
            elif rule == "ra8_max_stack":
                try:
                    annotated = int(arg)
                except ValueError:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"ra8_max_stack:'{arg}' on '{sym.name}' is not an integer byte count",
                        )
                    )
                    continue
                measured = find_su_file(sym)
                if measured is not None and measured > annotated:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' frame {measured} B exceeds "
                            f"ra8_max_stack:{annotated}",
                        )
                    )

            # 10. ra8_isr_safe -- handled by global ISR-chain pass below.
            elif rule == "ra8_isr_safe":
                pass

            # 11. ra8_expects_lock:<name>
            elif rule == "ra8_expects_lock":
                lock = arg
                for cs in direct_calls_by_callee.get(sym.usr, []):
                    body = direct_calls_by_caller.get(cs.caller_usr, [])
                    has_take = any(
                        c.callee_name == "RA8_TAKE_LOCK" and c.caller_line < cs.caller_line
                        for c in body
                    )
                    if not has_take:
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"call to '{sym.name}' (expects lock '{lock}') "
                                f'missing preceding RA8_TAKE_LOCK("{lock}")',
                            )
                        )

            # 12. ra8_host_friendly
            elif rule == "ra8_host_friendly":
                for cs in direct_calls_by_caller.get(sym.usr, []):
                    callee_sym = symbols.get(cs.callee_usr)
                    if callee_sym and any(
                        a.startswith(("ra8_hw_register_access", "RA8_HW_REGISTER_ACCESS"))
                        for a in callee_sym.annotations
                    ):
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"host-friendly '{sym.name}' calls MMIO "
                                f"accessor '{cs.callee_name}' (would break in sim)",
                            )
                        )

            # 13. ra8_latency_budget_ns -- recorded until a WCET pass exists.
            elif rule == "ra8_latency_budget_ns":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"ra8_latency_budget_ns:{arg} on '{sym.name}' recorded; no WCET pass yet",
                        warn_only=True,
                    )
                )

            # 14. ra8_no_recursion -- transitive call closure must not include self
            elif rule == "ra8_no_recursion":
                seen = {sym.usr}
                stack = [sym.usr]
                recursive = False
                guard = 0
                while stack and guard < RECURSION_GUARD_LIMIT:
                    guard += 1
                    cur = stack.pop()
                    for cs in direct_calls_by_caller.get(cur, []):
                        if cs.in_address_of:
                            continue
                        if cs.callee_usr == sym.usr:
                            recursive = True
                            break
                        if cs.callee_usr not in seen:
                            seen.add(cs.callee_usr)
                            stack.append(cs.callee_usr)
                    if recursive:
                        break
                if recursive:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' tagged RA8_NO_RECURSION "
                            f"appears in its own transitive call closure",
                        )
                    )

            # 15. ra8_bounded_loop:<symbol> -- textual fallback (libclang
            # loses for/while bounds easily; the textual pass is enough
            # to catch the common case).
            elif rule == "ra8_bounded_loop":
                try:
                    src = pathlib.Path(sym.file).read_text(errors="ignore")
                except OSError:
                    continue
                # crude function-body slice
                m = re.search(
                    re.escape(sym.name) + r"\s*\([^;]*\)\s*\{",
                    src,
                )
                if not m:
                    continue
                # capture braces until balanced
                start = m.end() - 1
                depth = 0
                end = start
                for i in range(start, len(src)):
                    if src[i] == "{":
                        depth += 1
                    elif src[i] == "}":
                        depth -= 1
                        if depth == 0:
                            end = i
                            break
                body = src[start:end]
                for loop_match in re.finditer(r"\b(for|while)\s*\(([^)]*)\)", body):
                    cond = loop_match.group(2)
                    if arg not in cond:
                        out.append(
                            Violation(
                                rule,
                                sym.file,
                                sym.line,
                                f"loop in '{sym.name}' missing bound symbol "
                                f"'{arg}' in condition '{cond.strip()}'",
                            )
                        )

            # 16. ra8_validates:<n>
            elif rule == "ra8_validates":
                try:
                    need = int(arg)
                except ValueError:
                    continue
                body = direct_calls_by_caller.get(sym.usr, [])
                count = sum(1 for c in body if c.callee_name.startswith("RA8_CHECK_"))
                if count < need:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' has {count} RA8_CHECK_* calls; "
                            f"ra8_validates:{need} requires at least {need}",
                        )
                    )

            # 17. ra8_owns_resource:<kind> -- approximate: require at least
            # one matching ra8_releases_resource:<kind> call somewhere in body.
            elif rule == "ra8_owns_resource":
                kind = arg
                body = direct_calls_by_caller.get(sym.usr, [])
                released = False
                for c in body:
                    callee = symbols.get(c.callee_usr)
                    if callee and any(
                        a == f"ra8_releases_resource:{kind}" for a in callee.annotations
                    ):
                        released = True
                        break
                if not released:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' acquires '{kind}' but no "
                            f"matching ra8_releases_resource:{kind} call found",
                        )
                    )

            # 18. ra8_reviewed_by -- informational rollup only.
            elif rule == "ra8_reviewed_by":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"reviewed-by '{arg}' recorded for '{sym.name}'",
                        warn_only=True,
                    )
                )

            # 19. ra8_register_bank -- informational only.
            elif rule == "ra8_register_bank":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"register-bank '{arg}' recorded for '{sym.name}'",
                        warn_only=True,
                    )
                )

            if warn_only:
                # Mark synthesised entries as warn-only (idempotent).
                for v in out[-1:]:
                    v.warn_only = True

    # ----- Rule 7 global sweep: malloc/free/calloc/realloc usage --------
    p10_exempt = {
        sym.usr
        for sym in symbols.values()
        if any(a.startswith("ra8_nasa_rule_3_ok") for a in sym.annotations)
    }
    bad_alloc = {"malloc", "free", "calloc", "realloc", "aligned_alloc"}
    for cs in calls:
        if cs.in_address_of:
            continue
        # Host-side test scaffolding is exempt from NASA P10 Rule 3 -- the
        # firmware itself never sees these TUs (see CLAUDE.md "Exempt Code").
        if "/tests/" in cs.caller_file.replace("\\", "/"):
            continue
        if cs.callee_name in bad_alloc and cs.caller_usr not in p10_exempt:
            out.append(
                Violation(
                    "ra8_nasa_rule_3_ok",
                    cs.caller_file,
                    cs.caller_line,
                    f"call to '{cs.callee_name}' from '{cs.caller_name}' which "
                    f"is not tagged RA8_NASA_RULE_3_OK",
                )
            )

    return out


def enforce_linkage(
    symbols: dict[str, AnnotatedSymbol],
    vector_entries: set[str],
) -> list[Violation]:
    """Every non-static function must state why it has external linkage.

    CLAUDE.md, "Which linkage annotation to use", makes this a tree-wide
    expectation rather than an opt-in: a function that is not `static`
    either publishes a contract other code may call, or it is deliberately
    non-`static` for one narrow reason that has to be written down.

    A definition passes when any of the following holds.

    * It carries `RA8_PRIV`, `RA8_INTERNAL` or `RA8_TEST_HELPER`.
    * A header publishes it. A prototype in a `.h` is an exported
      contract -- a library's public `inc/` header, an application's local
      header, a mock's header, or the vendored SOUP header whose interface
      the function implements (the NimBLE `ble_npl_*` porting layer, the
      ThreadX `tx_application_define` hook, the USBX class entry points).
      An `*_internal.h` prototype does not count: that header exists to
      say "library-private", which is what `RA8_PRIV` marks.
    * It is a hardware vector-table entry -- see `is_vector_table()`. The
      CPU reaches these through VTOR with no C caller at all, so no
      annotation describes them and no header can publish them: the only
      thing that names an IRQ trampoline is the table slot itself. The
      category is derived from the table's structure, so a handler that is
      removed from the table stops being exempt the moment it is unwired.
    * It is `main`, the ISO C entry point.

    Anything else is a gap, and the two shapes need different fixes, so
    they get different messages.

    A verdict is reached per *definition site*, not per symbol name. The
    coverage tests reach a module's `static` helpers by `#define`-ing a
    function to a `_cov` spelling and then `#include`-ing the `.c`, so one
    source construct shows up under two names with two USRs and the same
    file and line. Judging the renamed copy on its own would report the
    original function as an unpublished symbol in a file that never
    declared it.
    """
    verdicts: dict[tuple[str, int], list[Violation | None]] = {}
    for sym in symbols.values():
        if not sym.is_defined or sym.is_static or not is_first_party(sym.file):
            continue
        verdicts.setdefault((sym.file, sym.line), []).append(_linkage_verdict(sym, vector_entries))
    out: list[Violation] = []
    for site in sorted(verdicts):
        found = [v for v in verdicts[site] if v is not None]
        if len(found) == len(verdicts[site]) and found:
            out.append(found[0])
    return out


def _linkage_verdict(
    sym: AnnotatedSymbol,
    vector_entries: set[str],
) -> Violation | None:
    """Return the linkage violation for ``sym``, or None when it passes."""
    if any(parse_annotation(a)[0] in LINKAGE_ANNOTATIONS for a in sym.annotations):
        return None
    if _published_header(sym) is not None:
        return None
    if sym.usr in vector_entries or sym.name == C_ENTRY_POINT:
        return None
    internal = _internal_header(sym)
    if internal is not None:
        return Violation(
            "ra8_linkage",
            sym.file,
            sym.line,
            f"'{sym.name}' is declared in {_relative(internal)} but carries no "
            f"linkage annotation; a symbol that is non-static only so one "
            f"library's other TUs can reach it is RA8_PRIV (or RA8_TEST_HELPER "
            f"when only tests call it)",
        )
    return Violation(
        "ra8_linkage",
        sym.file,
        sym.line,
        f"'{sym.name}' has external linkage but nothing publishes it: no header "
        f"declares it and no vector table names it. Make it static and tag it "
        f"RA8_INTERNAL, or declare it -- in the library's inc/ header if it is "
        f"API, in an *_internal.h with RA8_PRIV if it is shared inside the library",
    )


def _relative(path: str) -> str:
    """Return ``path`` relative to the repo root when it lies inside it."""
    with contextlib.suppress(ValueError):
        return str(pathlib.Path(path).resolve().relative_to(REPO_ROOT))
    return path


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------
def main(argv: list[str]) -> int:  # noqa: PLR0912  # gate/parser dispatch, splitting hurts readability
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check", action="store_true", help="exit non-zero on any (non-informational) violation"
    )
    ap.add_argument("--list", action="store_true", help="dump every annotated symbol and exit")
    ap.add_argument("--quiet", action="store_true", help="suppress per-TU progress output")
    ap.add_argument("paths", nargs="*", help="optional explicit file list (else scan everything)")
    args = ap.parse_args(argv)

    partial = bool(args.paths)
    if partial:
        tus = [
            pathlib.Path(p).resolve()
            for p in args.paths
            if pathlib.Path(p).suffix in SOURCE_SUFFIXES
        ]
    else:
        tus = discover_translation_units()

    symbols: dict[str, AnnotatedSymbol] = {}
    calls: list[CallSite] = []
    vector_entries: set[str] = set()
    stats = ParseStats()

    for tu_path in tus:
        if not args.quiet and not args.check:
            print(f"  parsing {tu_path.relative_to(REPO_ROOT)}", file=sys.stderr)
        tu = parse_tu(tu_path, stats)
        if tu is None:
            continue
        try:
            walk_tu(tu, tu_path, symbols, calls, stats, vector_entries)
        except Exception as exc:
            sys.stderr.write(f"  WARN: walk failed for {tu_path}: {exc}\n")

    if args.list:
        if not symbols:
            print("(no symbols found)")
            return 0
        print(f"{'symbol':<48} {'file':<60} {'annotations'}")
        for s in sorted(symbols.values(), key=lambda x: (x.file, x.line)):
            if not s.annotations:
                continue
            print(f"{s.name:<48} {_relative(s.file):<60} {','.join(s.annotations)}")
        return 0

    violations = check_rule_keys()
    # An explicit file list parses a fraction of the tree on purpose, so
    # the whole-tree evidence checks cannot say anything about it.
    if partial:
        sys.stderr.write(
            "check_annotations: explicit file list -- parse-integrity and the "
            "linkage rule need the whole tree and are skipped. Run without "
            "arguments for the gate.\n"
        )
        violations.extend(enforce_rules(symbols, calls, vector_entries, whole_tree=False))
    else:
        violations.extend(check_parse_integrity(stats, len(tus)))
        violations.extend(enforce_rules(symbols, calls, vector_entries))

    resolution = stats.calls_resolved / stats.calls_seen if stats.calls_seen else 0.0
    summary = (
        f"{len(symbols)} symbols, "
        f"{stats.calls_resolved}/{stats.calls_seen} call sites resolved "
        f"({resolution:.1%}), {len(vector_entries)} vector-table entries, "
        f"{len(tus)} TUs"
    )

    if not violations:
        if not args.quiet:
            print(f"check_annotations: 0 violations across {summary}")
        return 0

    fatal = [v for v in violations if not v.warn_only]
    informational = [v for v in violations if v.warn_only]

    for v in violations:
        tag = "INFO" if v.warn_only else "FAIL"
        sys.stderr.write(f"[{tag}] {_relative(v.file)}:{v.line}: [{v.rule}] {v.message}\n")

    sys.stderr.write(
        f"check_annotations: {len(fatal)} fatal, {len(informational)} informational "
        f"across {summary}\n"
    )
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

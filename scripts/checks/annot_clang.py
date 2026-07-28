# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Getting libclang to actually parse this tree, and proving that it did.

Two responsibilities that belong together because one is the evidence for the
other: build the compile flags that let a translation unit parse completely,
then measure whether it did.

The measurement is not optional bookkeeping.  Every call-graph rule in this
checker is a claim about a graph libclang built, so an include that stops
resolving does not produce a loud error -- it removes edges, the rules stop
firing, and the gate prints *fewer* violations, which reads as an improvement.
:data:`MIN_CALL_RESOLUTION` and :func:`check_parse_integrity` exist to make
that failure mode fatal, and they live in the same module as the flags whose
degradation they detect.
"""

from __future__ import annotations

import contextlib
import os
import pathlib
import re
import subprocess
import sys

from annot_model import ParseStats, Violation
from annot_scope import is_build_output, is_excluded, is_test_path, repo_root

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

#: libclang diagnostic severity for Error (3) and Fatal (4); anything at or
#: above this level means the parse did not see the code it was given.
DIAG_ERROR = 3

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
        "literata_latin1.h",  # libs/ra8_fonts generator output
        "ra8_npu_model_addk_sim.h",  # Vela model-compiler output
    }
)

#: Headers a clean checkout legitimately lacks on THIS platform -- distinct
#: from GENERATED_HEADERS (never produced without a build step, on any host):
#: these exist only on the OS that ships them. ``sys/personality.h`` is
#: glibc/Linux-only (the ``personality()`` syscall this checker's own probe
#: (tests/mocks/ra8_sim_mmap.c) calls has no Darwin/BSD equivalent), so the
#: include can never resolve on macOS. That is expected, not a parse defect:
#: CLAUDE.md already documents that the host test suite does not run natively
#: on macOS at all (mmap of peripheral RAM below 4 GiB is refused there), so
#: this file's Linux-only code path is not exercised on that host either.
#: Empty on Linux, where the header is real and a missing resolution there
#: would still be the include-path defect this checker exists to catch.
NON_LINUX_MISSING_HEADERS = frozenset({"personality.h"}) if sys.platform != "linux" else frozenset()

#: Cached include flags -- the glob is stable for a whole run.
_INCLUDE_ARGS: list[str] = []

#: Extracts the header name out of clang's "'foo.h' file not found".
_MISSING_INCLUDE_RE = re.compile(r"'([^']+)' file not found")

#: Directory patterns under the repo root that are header roots for some TU.
_INCLUDE_ROOT_PATTERNS = (
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
    # A port may nest a compatibility include root under inc/ when the
    # software it ports supplies headers by name that the host SDK would
    # otherwise provide (port/esp-hosted/inc/idf_compat/ holds the ESP-IDF
    # headers the vendored driver includes). Derive those roots rather than
    # naming them: a port that grows one and is not listed here parses with
    # an unresolved include, and every declaration behind it drops out of
    # the call graph.
    "port/*/inc/*",
    "port/*/src",
    # tools/<tool>/{inc,src}: the same public/private split the libraries
    # use. cache_bench keeps its headers flat beside its sources, and the
    # Vela model compiler writes generated/ -- both are real include roots
    # for their own TUs, so derive them rather than assume the convention.
    "tools/*",
    "tools/*/inc",
    "tools/*/src",
    # a tool whose src/ is grouped into domain subdirs (ra8_emulator:
    # engine/periph/usb/display/io) keeps its *_internal.h beside the .c;
    # a cross-boundary include from tests/ needs the subdir on the path,
    # so derive those roots too (the same courtesy libs/*/* already gets).
    "tools/*/src/*",
    "tools/*/generated",
)


def _vendored_include_roots() -> list[pathlib.Path]:
    """Return the header roots inside ``libs/third_party/``.

    Vendored trees are excluded from *analysis* but must stay on the
    *include* path: ``port/`` implements interfaces those headers declare --
    ``ble_npl_*`` for NimBLE, ``tx_application_define`` for ThreadX,
    ``_ux_device_class_storage_*`` for USBX. Without their headers those are
    undeclared external symbols; with them they are what they actually are,
    implementations of a published contract.
    """
    third_party = repo_root() / "libs" / "third_party"
    if not third_party.is_dir():
        return []
    vendored = list(third_party.iterdir())
    vendored += list(third_party.rglob("inc"))
    vendored += list(third_party.rglob("include"))
    # Not every vendored tree uses an inc/ or include/ convention. esp-hosted
    # puts its public headers directly in host/ and common/**, and its driver
    # includes them by bare name, so the two conventions above miss them
    # entirely -- `esp_hosted_os_abstraction.h` went unresolved and the whole
    # OS-abstraction seam disappeared from the call graph.
    #
    # The tempting fix, deriving roots from wherever a `.h` actually sits, was
    # tried and rejected: it puts ~600 directories on the path, several of them
    # C++ trees, and a namesake header there then shadows the one a C
    # translation unit meant -- forty examples/ TUs started failing to resolve
    # `cstdint`. Include shadowing is exactly what a wider path buys, so the
    # extra roots are named per tree instead. Add a row here when a vendored
    # tree lands whose public headers are not under inc/ or include/.
    for extra in ("esp-hosted/host", "esp-hosted/host/api/priv", "esp-hosted/common"):
        vendored.append(third_party / extra)
    vendored += sorted((third_party / "esp-hosted" / "common").glob("*"))
    vendored += sorted((third_party / "esp-hosted" / "host" / "drivers").rglob("*"))
    seen: set[pathlib.Path] = set()
    ordered: list[pathlib.Path] = []
    for d in vendored:
        if d in seen or not d.is_dir() or is_build_output(d):
            continue
        seen.add(d)
        ordered.append(d)
    return ordered


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
    """
    root = repo_root()
    roots: list[pathlib.Path] = []
    for pattern in _INCLUDE_ROOT_PATTERNS:
        roots.extend(sorted(root.glob(pattern)))
    # Applications keep their headers beside their sources with no inc/
    # convention, so derive those roots from where the headers actually
    # are. A quote-include always searches the including file's own
    # directory first, so a namesake in a sibling app cannot displace an
    # app's own header.
    roots.extend(sorted({h.parent for h in (root / "examples").rglob("*.h")}))
    roots = [d for d in roots if d.is_dir() and not is_excluded(d)]
    roots.extend(_vendored_include_roots())

    out: list[str] = []
    seen: set[str] = set()
    for d in roots:
        if str(d) in seen:
            continue
        seen.add(str(d))
        out.append(f"-I{d}")
    return out


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
    # Alongside this checker, wherever it lives -- a hardcoded directory
    # here becomes a FileNotFoundError the moment the file moves.
    probe = pathlib.Path(__file__).resolve().parent / ".ra8_annotations_probe.c"
    try:
        probe.write_text("#include <stddef.h>\n#include <stdint.h>\nsize_t ra8_probe(void);\n")
        tu = cindex.Index.create().parse(
            str(probe), args=["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *extra]
        )
        return not [d for d in (tu.diagnostics if tu else []) if d.severity >= DIAG_ERROR]
    except cindex.TranslationUnitLoadError:
        return False
    finally:
        with contextlib.suppress(OSError):
            probe.unlink()


def _homebrew_keg_only_clang_candidates() -> list[str]:
    """Return full paths to keg-only Homebrew ``clang-<N>`` binaries, if any.

    Homebrew's ``llvm@<N>`` formulas are keg-only (a system default `clang`
    must stay whatever Apple or the unversioned `llvm` formula provides), so
    their ``bin/`` is never linked onto PATH -- ``command -v clang-18`` fails
    even when ``brew install llvm@18`` succeeded and the binary is sitting
    right there. ``brew --prefix llvm@<N>`` finds it directly, tried at the
    same versions ``_resource_dir_from_driver`` already tries by bare name
    (which is enough on Linux, where the devcontainer's clang-18 package
    installs onto PATH normally). A missing ``brew`` (Linux, or a Mac without
    it) degrades to an empty list.
    """
    out: list[str] = []
    for major in ("22", "21", "20", "19", "18"):
        try:
            res = subprocess.run(  # noqa: S603 -- fixed argv, no shell
                ["brew", "--prefix", f"llvm@{major}"],  # noqa: S607 -- brew from PATH is intended
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if res.returncode != 0 or not res.stdout.strip():
            continue
        candidate = pathlib.Path(res.stdout.strip()) / "bin" / f"clang-{major}"
        if candidate.is_file():
            out.append(str(candidate))
    return out


def _resource_dir_from_driver() -> list[str]:
    """Ask a real clang binary where its builtin headers live."""
    candidates = [
        os.environ.get("RA8_CLANG"),
        "clang",
        "clang-22",
        "clang-21",
        "clang-20",
        "clang-19",
        "clang-18",
        *_homebrew_keg_only_clang_candidates(),
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
    return []


def _darwin_sysroot_args() -> list[str]:
    """Return -isysroot and Homebrew include flags, on macOS only.

    libclang loaded through the Python bindings has no notion of the
    platform SDK a real ``clang`` driver invocation picks up by default, so
    on macOS libc headers behind the SDK sysroot (``<stdio.h>``, most of
    ``<stdint.h>``'s transitive includes, and everything else under
    ``usr/include``) do not resolve -- unlike the compiler's OWN builtin
    headers (``stddef.h``, ``stdarg.h``, ...), which ``_builtin_include_args``
    already supplies from the resource directory and which resolve with or
    without a sysroot. ``xcrun --show-sdk-path`` reports the active SDK
    regardless of whether it is a full Xcode install or just the Command
    Line Tools, so ask it rather than hardcode a path that drifts with
    every OS/Xcode update. Measured effect on this tree: 86.6% call-site
    resolution without a sysroot, 99.9%+ with one (#488).

    ``tools/ra8_emulator`` includes ``<unicorn/unicorn.h>``, which on macOS
    is a Homebrew package living outside any path clang searches by
    default (unlike Linux, where the devcontainer installs it under
    ``/usr/local/include``). Add the active Homebrew prefix's ``include/``
    so that header, and any other Homebrew-installed header, resolves the
    same way it does for a real local ``clang`` invocation.

    A missing ``xcrun`` / ``brew`` (this checker also runs on Linux, where
    neither exists) degrades to an empty list rather than raising -- the
    Linux parse already reaches the floor without either flag.
    """
    if sys.platform != "darwin":
        return []
    out: list[str] = []
    try:
        sdk = subprocess.run(
            ["xcrun", "--show-sdk-path"],  # noqa: S607 -- xcrun from PATH is intended
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        sdk = None
    if sdk is not None and sdk.returncode == 0 and sdk.stdout.strip():
        out.append(f"-isysroot{sdk.stdout.strip()}")
    try:
        brew = subprocess.run(
            ["brew", "--prefix"],  # noqa: S607 -- brew from PATH is intended
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        brew = None
    if brew is not None and brew.returncode == 0 and brew.stdout.strip():
        inc = pathlib.Path(brew.stdout.strip()) / "include"
        if inc.is_dir():
            out.append(f"-I{inc}")
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
    from_driver = _resource_dir_from_driver()
    if from_driver:
        return from_driver
    # No clang driver on PATH: the pip wheel bundles libclang.so but no
    # builtin headers, so fall back to whatever a distro LLVM package left
    # on disk. Newest first, so a box with several LLVMs picks the one
    # closest to the bindings.
    for inc in sorted(_resource_dirs_on_disk(), reverse=True):
        if (inc / "stddef.h").is_file() and _probe_is_clean([f"-isystem{inc}"]):
            return [f"-isystem{inc}"]
    return []


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
    port_inc = repo_root() / "port" / "mbedtls" / "inc"
    out: list[str] = []
    for macro, name in (
        ("MBEDTLS_CONFIG_FILE", "mbedtls_config.h"),
        ("TF_PSA_CRYPTO_CONFIG_FILE", "tf_psa_crypto_config.h"),
    ):
        header = port_inc / name
        if header.is_file():
            out.append(f'-D{macro}="{header}"')
    return out


def tu_args(path: pathlib.Path) -> list[str]:
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
        _INCLUDE_ARGS = _builtin_include_args() + _darwin_sysroot_args() + _include_args()
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
    if is_test_path(str(path)):
        config += ["-DRA8_SIMULATOR_MODE", "-DUNIT_TEST"]
    return [*lang, *config, *_INCLUDE_ARGS]


def parse_tu(path: pathlib.Path, stats: ParseStats) -> cindex.TranslationUnit | None:
    """Parse a single TU and record what the parse could not resolve."""
    index = cindex.Index.create()
    try:
        tu = index.parse(
            str(path),
            args=tu_args(path),
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
    except cindex.TranslationUnitLoadError:
        stats.unparsed.append(str(path))
        return None
    if tu is None:
        stats.unparsed.append(str(path))
        return None
    for diag in tu.diagnostics:
        if diag.severity < DIAG_ERROR:
            continue
        match = _MISSING_INCLUDE_RE.search(diag.spelling)
        if not match:
            continue
        header_name = pathlib.PurePath(match.group(1)).name
        if header_name not in GENERATED_HEADERS and header_name not in NON_LINUX_MISSING_HEADERS:
            stats.missing_includes.add((str(path), match.group(1)))
    return tu


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
                str(repo_root()),
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
                str(repo_root()),
                0,
                f"only {stats.calls_resolved} of {stats.calls_seen} call sites "
                f"resolved ({rate:.1%}, floor {MIN_CALL_RESOLUTION:.0%}) -- the "
                f"call-graph rules are not seeing the tree",
            )
        )
    return out

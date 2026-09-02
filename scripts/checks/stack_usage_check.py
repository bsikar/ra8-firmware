#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Aggregate gcc ``-fstack-usage`` .su files into a project-wide stack report.

Background
----------

NASA Power-of-10 Rule 3 forbids dynamic memory allocation after init.
The silent equivalent is unbounded stack growth: a deep call chain or a
single oversized frame can blow past the linker-reserved stack region
and corrupt adjacent RAM. IEC 61508 SIL 3 and DO-178C Level B both
require the project to *demonstrate* a worst-case stack bound, not
merely assume one.

When compiled with ``-fstack-usage``, gcc emits a ``<file>.su`` file
next to each ``.o``. Each line has the form::

    path/to/file.c:LINE:COL:function_name<TAB>FRAME_BYTES<TAB>QUALIFIER

where QUALIFIER is one of ``static``, ``dynamic``, ``bounded``, or
combinations like ``dynamic,bounded``.

What this script does
---------------------

1. Walks every ``examples/**/build*/**/*.su`` file.
2. For each function: records TU, function name, frame size, qualifier.
3. Flags any function with frame > ``--frame-limit`` (default 2048) or
   with a ``dynamic`` qualifier.
4. Writes ``build/stack_usage.csv`` and a per-app
   ``build/stack_usage_<app>.txt``.
5. Exits non-zero if a critical-path module
   (ra8_isr, ra8_check, ra8_err, ra8_mpu, ra8_cgc, ra8_pfs) has any function
   with frame > 256 bytes or any ``dynamic`` qualifier anywhere.

This script is report-only by default for the rest of the codebase --
the per-target ``-Wstack-usage=N`` warning (in cmake/ra8_warnings.cmake)
is the build-time gate; this script is the project-wide aggregator.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

DEFAULT_FRAME_LIMIT = 2048
CRITICAL_FRAME_LIMIT = 256
CRITICAL_MODULES = (
    "ra8_isr",
    "ra8_check",
    "ra8_err",
    "ra8_mpu",
    "ra8_cgc",
    "ra8_pfs",
)

# --- Enumeration floor (issue #386) ------------------------------------------
#
# A scan that parsed ZERO functions -- no .su files at all, or .su files that
# decode to nothing -- used to exit 0, byte-for-byte identical to a genuine
# clean pass. A build-flag change, a moved build directory, or a compiler swap
# silently produces zero .su files; the gate then reported success over a stack
# budget it never measured. On a bare-metal target a blown frame is memory
# corruption rather than an exception, so this is the highest-consequence
# instance of the "green while measuring nothing" defect this tree keeps
# finding, and it left every RA8_MAX_STACK annotation unverified while it
# appeared enforced.
#
# The floor makes a degraded enumeration read as FAILURE, never as an
# improvement -- the same shape as check_lint_coverage.py's FILE_FLOOR and
# check_annotations.py's MIN_CALL_RESOLUTION. Below it the gate exits non-zero
# and says the sweep collapsed.
#
# Calibrated against a full cross-build: the `build-cross` gate runs
# scripts/builders/all_examples.sh (218 apps at time of writing), then the
# `stack-usage` gate aggregates its .su output. That build parses 90,140
# first-party-plus-SOUP functions from 7552 .su files. The floor sits far below
# that (~5.5%) so any legitimate build clears it with an 18x margin, while the
# collapse modes above -- which drive the count to zero or a handful -- trip it
# unambiguously. --allow-empty (the pre-commit path) skips the floor, because a
# single-app local build legitimately parses far fewer than a full sweep.
DEFAULT_MIN_FUNCTIONS = 5000

# Process exit codes. Distinct so a caller (and a reader of the CI log) can tell
# "the code has an over-budget frame" from "this gate could not measure its
# subject", mirroring the convention in check_lint_coverage.py.
RC_OK = 0
RC_VIOLATION = 1
RC_ENUMERATION_BROKE = 2

# --- Strict-mode partitioning -------------------------------------------------
#
# In `--strict` mode the soft per-app frame limit is promoted to a hard
# gate, but only for *first-party* code (everything outside
# THIRD_PARTY_PATH_FRAGMENTS). Third-party SOUP is exempt because:
#
#   * It is pre-qualified and lives under one of the two canonical third-party
#     roots -- its justification documents are filed under docs/SOUP/ (see
#     docs/SOUP/README.md).
#   * The largest current offenders (miniz mz_zip_reader_*,
#     tinfl_decompress_mem_to_*, ~10 kB frames) are deflate / zip
#     decoder helpers that are only invoked from the ereader app's
#     dedicated worker thread, which carries a generously-sized stack.
#
# The FIRST_PARTY_EXEMPTIONS list is the explicit, documented escape
# hatch for first-party functions that have a justified large frame.
# Add a tuple `(tu_substring, function_name, max_bytes, rationale)` per
# entry; the gate will accept any first-party frame at or below
# `max_bytes` for that function. Empty list today means *no* first-
# party function is exempt.

THIRD_PARTY_PATH_FRAGMENTS = (
    "/libs/third_party/",
    "/apps/shared_libs/third_party/",
    # Vendor-supplied port shims live under port/ but mostly call into
    # third_party/ libraries; they remain first-party and gated.
)

FIRST_PARTY_EXEMPTIONS = (
    # ("tu_substring", "function_name", max_bytes, "rationale"),
    #
    # Example template -- intentionally empty today (every first-party
    # function is currently under the 2048-byte default limit, see
    # `python3 scripts/checks/stack_usage_check.py --strict` against
    # HEAD as of the commit that added this list):
    #
    # (
    #     "apps/shared_libs/epub/src/epub_open.c",
    #     "priv_parse_archive",
    #     4344,
    #     "ZIP central-directory parser: 4 kB scratch struct on stack "
    #     "to avoid heap; only invoked once at chapter open from the "
    #     "ereader worker thread (8 kB stack budget).",
    # ),
)


def is_first_party(tu_path: str) -> bool:
    """Whether a .su translation-unit path is ours rather than vendored SOUP.

    Decided by path fragment against the vendored trees, because the TU path
    inside a .su is whatever the compiler was given and is not reliably
    repo-relative -- a prefix test would miss an absolute one.
    """
    return not any(frag in tu_path for frag in THIRD_PARTY_PATH_FRAGMENTS)


def exemption_for(tu_path: str, func: str) -> int:
    """Return the exempt max-bytes for a first-party (tu, func) pair, or 0."""
    for tu_frag, fn_name, max_bytes, _why in FIRST_PARTY_EXEMPTIONS:
        if tu_frag in tu_path and fn_name == func:
            return max_bytes
    return 0


_SU_LINE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+):(?P<func>[^\s\t]+)"
    r"[\t ]+(?P<bytes>\d+)[\t ]+(?P<qual>[A-Za-z,]+)\s*$"
)


class StackEntry:
    """One function's stack frame, as reported by a single line of a .su file.

    ``tu`` is the path the COMPILER recorded, not where the .su was found. The
    two diverge for the shared-library archive, and the gates key off ``tu``
    precisely so that a critical-path frame is caught wherever it was built.
    """

    __slots__ = ("app", "bytes_", "func", "qualifier", "su_file", "tu")

    def __init__(  # noqa: PLR0913  # data class init, all fields are required
        self,
        app: str,
        tu: str,
        func: str,
        bytes_: int,
        qualifier: str,
        su_file: Path,
    ) -> None:
        """Record one parsed .su line; every field is required and none is derived."""
        self.app = app
        self.tu = tu
        self.func = func
        self.bytes_ = bytes_
        self.qualifier = qualifier
        self.su_file = su_file

    @property
    def is_dynamic(self) -> bool:
        """Whether gcc reported this frame as dynamically sized.

        The qualifier is a comma-separated set, so it is split and matched as a
        whole token rather than by substring -- a compound qualifier must be
        decomposed, not pattern-matched.

        ``dynamic,bounded`` counts as dynamic here even though gcc could bound
        it. That is the conservative reading and it is deliberate: a bounded
        dynamic frame still has no single compile-time size to put in a
        worst-case stack budget, so it is surfaced for a human to judge.
        """
        return "dynamic" in self.qualifier.split(",")

    @property
    def is_critical(self) -> bool:
        """Whether this frame belongs to a module held to the tighter 256-byte limit.

        Matches the module name in the TU path OR in the function name, since a
        critical-path function can be defined in a differently-named TU; either
        hit is enough to pull the frame under the stricter budget.
        """
        return any(m in self.tu for m in CRITICAL_MODULES) or any(
            m in self.func for m in CRITICAL_MODULES
        )


# The CI cross-build fast path (scripts/builders/all_examples.sh) compiles the
# universal first-party library set (ra8_core / ra8_hal / ra8_net_pal /
# ra8_usb_pal / board / secure_app) ONCE into a static archive under
# build/shared_libs/ instead of recompiling it into every app, so those
# sources' .su files land there rather than under examples/<app>/build/. That
# archive holds the critical-path modules (ra8_isr / ra8_check / ra8_err /
# ra8_mpu / ra8_cgc / ra8_pfs), so it MUST be aggregated too -- otherwise the
# gate would silently lose its whole shared-library surface. See
# cmake/ra8_shared_libs.cmake.
SHARED_LIB_BUILD_SUBDIR = "build/shared_libs"
SHARED_LIB_APP_NAME = "ra8_shared"


def find_su_files(repo_root: Path) -> list:
    """Collect every gcc ``.su`` file the cross-build emits.

    Two roots:

    * ``examples/`` -- each app's per-app CMake output lives in ``<app>/
      build*/``, nested 2-4 levels below ``examples/`` (``examples/<tier>/
      .../<app>/``). A ``.su`` is only ever emitted inside such a build tree,
      so we recurse to any depth rather than assume a fixed layout. This also
      covers the ineligible apps (TrustZone, non-ek board) that the fast path
      still compiles from source per-app.

    * ``build/shared_libs/`` -- the prebuilt universal-library archive the
      fast path compiles once (see ``SHARED_LIB_BUILD_SUBDIR``). Absent on a
      per-app-only build, so this is additive and never regresses the old
      examples-only sweep.
    """
    found: list = []
    examples = repo_root / "examples"
    if examples.is_dir():
        found.extend(p for p in examples.rglob("*.su") if p.is_file())
    shared = repo_root / SHARED_LIB_BUILD_SUBDIR
    if shared.is_dir():
        found.extend(p for p in shared.rglob("*.su") if p.is_file())
    return sorted(found)


def app_name_for(su_file: Path, repo_root: Path) -> str:
    """Attribute a .su file to the app whose build tree produced it.

    Used only for GROUPING the per-app reports. The critical-path and
    first-party gates deliberately key off the TU path recorded inside the .su
    instead, so a shared-library frame is still judged correctly despite being
    filed under a synthetic app name.

    Returns "unknown" rather than raising for a .su outside both known roots:
    an unattributable file should still appear in the aggregate report.
    """
    # Archive .su files live under build/shared_libs/, not examples/. Attribute
    # them to a synthetic app so the per-app report groups them; the
    # critical-path and first-party gates key off the real TU path (e.g.
    # libs/ra8_hal/src/ra8_isr.c), which is preserved inside the .su, so those
    # gates still fire on shared-library frames.
    try:
        su_file.relative_to(repo_root / SHARED_LIB_BUILD_SUBDIR)
    except ValueError:
        pass
    else:
        return SHARED_LIB_APP_NAME
    try:
        rel = su_file.relative_to(repo_root / "examples")
    except ValueError:
        return "unknown"
    parts = rel.parts
    # The app directory is the path component immediately preceding the
    # first `build*` segment: examples/<tier>/.../<app>/build*/... .
    for i, part in enumerate(parts):
        if part.startswith("build"):
            return parts[i - 1] if i > 0 else "unknown"
    return parts[0] if parts else "unknown"


def parse_su(su_file: Path, app: str) -> list:
    """Parse one .su file into StackEntry records, skipping anything unrecognised.

    Every failure mode here is a silent skip -- an unreadable file, a line that
    does not match the .su grammar, a byte count that is not an integer. That
    tolerance suits a format gcc owns and may extend, but note the consequence:
    if the .su grammar ever changed wholesale this would return no entries, and
    ``main`` reports a parse of zero functions as success. The gate would go
    quiet rather than fail. Nothing currently detects that.
    """
    entries = []
    try:
        text = su_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return entries
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = _SU_LINE.match(line)
        if not match:
            continue
        try:
            nbytes = int(match.group("bytes"))
        except ValueError:
            continue
        entries.append(
            StackEntry(
                app,
                match.group("path"),
                match.group("func"),
                nbytes,
                match.group("qual"),
                su_file,
            )
        )
    return entries


def write_csv(out_path: Path, entries: list) -> None:
    """Write the flat machine-readable report of every parsed frame.

    Written with ``newline=""`` as the csv module requires, and in ASCII so a
    non-ASCII symbol name fails here rather than producing a file the rest of
    the ASCII-only toolchain cannot read.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="ascii", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["app", "tu", "function", "frame_bytes", "qualifier"])
        for e in entries:
            writer.writerow([e.app, e.tu, e.func, e.bytes_, e.qualifier])


def write_per_app(out_dir: Path, entries: list) -> None:
    """Write one human-readable report per app, biggest frame first.

    Sorted descending because these files are read to answer "what should I
    shrink?", so the answer belongs at the top rather than after a scroll.
    """
    by_app = defaultdict(list)
    for e in entries:
        by_app[e.app].append(e)
    out_dir.mkdir(parents=True, exist_ok=True)
    for app, rows in sorted(by_app.items()):
        rows.sort(key=lambda r: r.bytes_, reverse=True)
        path = out_dir / f"stack_usage_{app}.txt"
        with path.open("w", encoding="ascii") as fh:
            fh.write(f"# stack usage report for {app}\n")
            fh.write(f"# {len(rows)} functions analysed\n")
            fh.write("# columns: bytes  qualifier  function  tu\n\n")
            for r in rows:
                fh.write(f"{r.bytes_:>8}  {r.qualifier:<16}  {r.func}  {r.tu}\n")


def find_violations(entries: list, frame_limit: int) -> tuple[list, list]:
    """Partition entries into critical-path and soft breaches.

    The two buckets are mutually exclusive by construction: a critical-path
    frame is judged ONLY against the 256-byte limit and never also counted as
    a soft breach, so one oversized function cannot be reported twice.

    Returns ``(critical, soft)``.
    """
    critical = []
    soft = []
    for e in entries:
        if e.is_critical:
            if e.bytes_ > CRITICAL_FRAME_LIMIT or e.is_dynamic:
                critical.append(e)
        elif e.bytes_ > frame_limit or e.is_dynamic:
            soft.append(e)
    return critical, soft


def print_top_n(entries: list, n: int) -> None:
    """Print the ``n`` largest frames across every app to stdout.

    Ranks the whole corpus rather than each app separately, which is what makes
    it useful on a CI log: the worst frame in the tree surfaces even when it
    lives in an app nobody was looking at.
    """
    ranked = sorted(entries, key=lambda r: r.bytes_, reverse=True)[:n]
    print(f"\nTop {len(ranked)} stack-frame offenders across all apps:")
    print(f"{'bytes':>8}  {'qualifier':<16}  app/function  (tu)")
    for r in ranked:
        print(f"{r.bytes_:>8}  {r.qualifier:<16}  {r.app}/{r.func}  ({r.tu})")


def _add_enumeration_args(parser: argparse.ArgumentParser) -> None:
    """Add the #386 enumeration-floor and self-check options.

    Kept out of ``_build_parser`` so that function stays within the 60-line
    NASA Rule 4 budget the ``function-size`` gate enforces; these are the
    controls that make an empty or collapsed sweep fail loudly.
    """
    parser.add_argument(
        "--min-functions",
        type=int,
        default=DEFAULT_MIN_FUNCTIONS,
        help=(
            "Enumeration floor (issue #386). A sweep that parses fewer than "
            "this many functions is treated as a collapsed enumeration and "
            "FAILS, so a degraded run reporting fewer results cannot read as a "
            "pass. Ignored under --allow-empty."
        ),
    )
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help=(
            "Pre-commit-friendly: a freshly cloned tree has no .su files yet, "
            "and forcing a full app build inside the hook would cost minutes "
            "per commit. With this flag a sweep that finds NO .su files exits "
            "0 and the function floor is not enforced. It does NOT excuse .su "
            "files that decode to zero functions -- that is corruption, not a "
            "fresh clone, and still fails. Omit this flag in CI so an empty or "
            "collapsed sweep goes red."
        ),
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help=(
            "Assert the gate fires in BOTH directions on synthetic .su "
            "fixtures -- an empty sweep and an over-budget frame go red, a "
            "real clean run stays green -- then exit. Proves an enumeration "
            "collapse cannot pass as clean."
        ),
    )


def _build_parser() -> argparse.ArgumentParser:
    """Every command-line option this gate accepts."""
    parser = argparse.ArgumentParser(
        description="Aggregate gcc .su files into a project-wide report."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    parser.add_argument(
        "--frame-limit",
        type=int,
        default=DEFAULT_FRAME_LIMIT,
    )
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument(
        "--warn-only",
        action="store_true",
        help=(
            "Pre-commit-friendly mode: report soft violations and the "
            "top-N table without failing on per-app frame breaches "
            "or critical-module budget breaches. The only hard "
            "failure left is a `dynamic` qualifier anywhere (NASA "
            "Power-of-10 Rule 3 -- VLAs / alloca are forbidden). "
            "Skips the report entirely if no .su files are present "
            "yet (so a fresh clone never blocks a commit)."
        ),
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help=(
            "Promote the soft frame limit to a hard gate for "
            "*first-party* TUs (everything outside both third-party roots). "
            "Third-party SOUP remains exempt. First-party functions "
            "with a justified large frame must be enrolled in "
            "FIRST_PARTY_EXEMPTIONS at the top of this script."
        ),
    )
    _add_enumeration_args(parser)
    return parser


def _report_strict(soft: list, frame_limit: int) -> int:
    """Promote a first-party soft violation into a hard failure.

    Third-party SOUP stays exempt; a first-party function with a justified
    large frame must be enrolled in FIRST_PARTY_EXEMPTIONS with a rationale.
    """
    offenders = []
    for e in soft:
        if not is_first_party(e.tu):
            continue
        allowed = exemption_for(e.tu, e.func)
        if allowed > 0 and e.bytes_ <= allowed:
            continue
        offenders.append(e)
    if not offenders:
        return 0
    print(
        f"\nSTRICT first-party violations: "
        f"{len(offenders)} function(s) exceed "
        f"{frame_limit} bytes and are not exempt:"
    )
    for e in sorted(offenders, key=lambda r: r.bytes_, reverse=True):
        print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")
    print(
        "\nFix: either reduce the frame size (move scratch "
        "buffers to module-static storage) or enroll the "
        "function in FIRST_PARTY_EXEMPTIONS at the top of "
        "scripts/checks/stack_usage_check.py with a written "
        "rationale."
    )
    return 1


def _report_critical(critical: list) -> None:
    """Print the critical-module frame breaches."""
    print(
        f"\nCRITICAL-PATH violations "
        f"(>{CRITICAL_FRAME_LIMIT} bytes or dynamic in "
        f"{','.join(CRITICAL_MODULES)}):"
    )
    for e in sorted(critical, key=lambda r: r.bytes_, reverse=True):
        print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")


def _report_dynamic(all_entries: list) -> int:
    """NASA P10 Rule 3: a `dynamic` qualifier is a VLA or alloca.

    Forbidden in this firmware regardless of frame size, in any module, and
    the gate applies even in --warn-only mode. No deviation procedure.
    """
    dyn = [e for e in all_entries if e.is_dynamic]
    if not dyn:
        return 0
    print(
        f"\nNASA P10 Rule 3 violation: "
        f"{len(dyn)} function(s) carry a `dynamic` qualifier "
        f"(VLA or alloca). Forbidden in this firmware -- "
        f"replace with a fixed-size buffer or an enum-bounded "
        f"static array."
    )
    for e in sorted(dyn, key=lambda r: r.bytes_, reverse=True):
        print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")
    return 1


def _collect_entries(repo_root: Path, su_files: list) -> list:
    """Parse every .su file into entries tagged with their owning app."""
    all_entries = []
    for su in su_files:
        app = app_name_for(su, repo_root)
        all_entries.extend(parse_su(su, app))
    return all_entries


def _print_census(su_count: int, func_count: int) -> None:
    """Print the file/function census that tells a real pass from an empty one.

    Emitted on EVERY path -- pass and fail alike, and regardless of --quiet --
    because it is the one line that makes a green result meaningful (#386): a
    reader can see whether the sweep actually measured anything.
    """
    print(f"stack_usage_check: parsed {func_count} function(s) from {su_count} .su file(s).")


def _check_enumeration(
    su_count: int, func_count: int, min_functions: int, allow_empty: bool
) -> int:
    """Decide whether the sweep measured enough to trust its verdict (#386).

    A gate that examined nothing must FAIL, not pass. The three collapse modes
    are graded distinctly:

    * NO .su files -- an unbuilt or moved build tree. Under ``--allow-empty``
      (the pre-commit path) this is a fresh clone and is tolerated; otherwise
      it is a build that never ran and fails.
    * .su files that decode to ZERO functions -- corruption or a grammar the
      parser no longer understands. This is never a fresh clone, so it fails
      even under ``--allow-empty``.
    * FEWER functions than the floor -- a partially collapsed enumeration. A
      run reporting far fewer results than a real build must not read as an
      improvement, so it fails; ``--allow-empty`` skips the floor because a
      single-app local build legitimately parses fewer.

    Returns ``RC_OK`` when the sweep is trustworthy, ``RC_ENUMERATION_BROKE``
    otherwise (after printing the reason to stderr).
    """
    if su_count == 0:
        if allow_empty:
            print(
                "stack_usage_check: no .su files found; nothing built with "
                "-fstack-usage yet -- allowed under --allow-empty.",
                file=sys.stderr,
            )
            return RC_OK
        print(
            "stack_usage_check: FAIL -- no .su files found under examples/**/build*/ "
            "or build/shared_libs/.\n"
            "  The build never ran or its output moved: there is no stack budget to "
            "measure, so this is a failure, not a pass.\n"
            "  Run the build-cross gate first (it compiles every app with "
            "-fstack-usage).",
            file=sys.stderr,
        )
        return RC_ENUMERATION_BROKE

    if func_count == 0:
        print(
            f"stack_usage_check: FAIL -- found {su_count} .su file(s) but parsed 0 "
            "functions.\n"
            "  .su files present that decode to nothing is corruption or a changed "
            ".su grammar, not a fresh clone; the stack budget went unmeasured.",
            file=sys.stderr,
        )
        return RC_ENUMERATION_BROKE

    if not allow_empty and func_count < min_functions:
        print(
            f"stack_usage_check: FAIL -- parsed only {func_count} function(s) from "
            f"{su_count} .su file(s), below the floor of {min_functions}.\n"
            "  The enumeration collapsed (a partial build, a moved build dir, or a "
            "compiler swap). A degraded sweep reporting fewer frames must not read as "
            "a pass.",
            file=sys.stderr,
        )
        return RC_ENUMERATION_BROKE

    return RC_OK


def main(argv: list) -> int:
    """Aggregate every .su file, write the reports, and gate on the violations.

    A green run is only evidence that stack budgets were checked when the sweep
    actually parsed functions. Every no-input path is therefore graded by
    ``_check_enumeration`` and prints the file/function census: an empty sweep
    or one collapsed below ``--min-functions`` FAILS with
    ``RC_ENUMERATION_BROKE`` rather than passing vacuously (#386). The only
    tolerated empty case is ``--allow-empty`` with no .su files at all -- the
    pre-commit fresh-clone path, where forcing a full build inside the hook
    would cost minutes per commit.

    Severity is tiered rather than uniform. Critical-path modules breach at 256
    bytes, everything else at ``--frame-limit`` (2048), and a ``dynamic``
    qualifier anywhere is a P10 Rule 3 violation regardless of size --
    that last check runs on ALL entries and is the one thing ``--warn-only``
    cannot downgrade.

    Returns ``RC_ENUMERATION_BROKE`` (2) when the sweep measured too little to
    trust, ``RC_VIOLATION`` (1) on a strict-mode soft breach, an un-waived
    critical breach, or any dynamic frame, and ``RC_OK`` (0) otherwise.
    """
    args = _build_parser().parse_args(argv)

    if args.selftest:
        return selftest()

    repo_root = args.repo_root.resolve()
    su_files = find_su_files(repo_root)
    all_entries = _collect_entries(repo_root, su_files)

    su_count = len(su_files)
    func_count = len(all_entries)
    _print_census(su_count, func_count)

    verdict = _check_enumeration(su_count, func_count, args.min_functions, args.allow_empty)
    if verdict != RC_OK:
        return verdict
    if su_count == 0:
        # --allow-empty with a fresh clone: nothing to measure, nothing to gate.
        return RC_OK

    out_dir = repo_root / "build"
    write_csv(out_dir / "stack_usage.csv", all_entries)
    write_per_app(out_dir, all_entries)

    critical, soft = find_violations(all_entries, args.frame_limit)

    if not args.quiet:
        print(f"  CSV:        {out_dir / 'stack_usage.csv'}")
        print(f"  per-app:    {out_dir}/stack_usage_<app>.txt")
        print_top_n(all_entries, args.top)

    if soft:
        print(
            f"\nSOFT violations: {len(soft)} function(s) over {args.frame_limit} bytes or dynamic:"
        )
        for e in sorted(soft, key=lambda r: r.bytes_, reverse=True):
            print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")

    if args.strict and _report_strict(soft, args.frame_limit):
        return RC_VIOLATION

    if critical:
        _report_critical(critical)
        if not args.warn_only:
            return RC_VIOLATION

    return _report_dynamic(all_entries)


def _write_su_fixture(build_dir: Path, name: str, lines: list) -> None:
    """Write a synthetic .su file under a fake app build tree for the selftest."""
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / name).write_text("".join(f"{line}\n" for line in lines), encoding="ascii")


def selftest() -> int:
    """Assert the gate fires in BOTH directions, so a collapse cannot pass clean.

    Builds throwaway .su fixtures in a temp tree and drives ``main`` against
    them. A vacuous selftest would defeat the whole point of #386, so each
    direction is asserted against a real return code:

    * an empty sweep (no .su files, no --allow-empty) FAILS;
    * .su files that decode to zero functions FAIL even under --allow-empty;
    * a sweep below the floor FAILS, but the same fixture PASSES once the floor
      is lowered -- proving the floor, not something else, is what tripped;
    * an over-budget first-party frame FAILS under --strict;
    * a genuine clean run PASSES.

    Returns ``RC_OK`` when every direction behaves, ``RC_VIOLATION`` otherwise.
    """
    failures: list = []

    def expect_red(got: int, label: str) -> None:
        if got == RC_OK:
            failures.append(f"{label}: expected non-zero, got {got}")

    def expect_green(got: int, label: str) -> None:
        if got != RC_OK:
            failures.append(f"{label}: expected 0, got {got}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        build = root / "examples" / "app" / "build"
        low = ["--strict", "--min-functions", "1"]
        clean_line = "apps/shared_libs/epub/src/ok.c:10:1:small_fn\t128\tstatic"

        # 1. Empty sweep -- no .su anywhere. Must FAIL without --allow-empty ...
        expect_red(main(["--repo-root", str(root)]), "empty sweep")
        # ... and be tolerated WITH --allow-empty (fresh clone).
        expect_green(main(["--repo-root", str(root), "--allow-empty"]), "empty sweep, allow-empty")

        # 2. .su present but decoding to zero functions -- corruption. Must FAIL
        #    even under --allow-empty.
        _write_su_fixture(build, "junk.su", ["not a stack-usage line at all", ""])
        expect_red(
            main(["--repo-root", str(root), "--allow-empty"]),
            "zero functions, allow-empty",
        )

        # 3. One clean function. Below the default floor -> FAIL on the floor ...
        _write_su_fixture(build, "junk.su", [clean_line])
        expect_red(main(["--repo-root", str(root), "--strict"]), "below floor")
        # ... but PASS once the floor is lowered to admit it (isolates the floor).
        expect_green(main(["--repo-root", str(root), *low]), "clean run, floor=1")

        # 4. An over-budget first-party frame must FAIL under --strict even with
        #    the floor satisfied.
        _write_su_fixture(
            build,
            "over.su",
            [clean_line, "apps/shared_libs/epub/src/big.c:20:1:priv_scratch\t9000\tstatic"],
        )
        expect_red(main(["--repo-root", str(root), *low]), "over-budget frame")

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return RC_VIOLATION
    print("selftest: stack_usage_check empty-sweep + floor + over-budget detection OK")
    return RC_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
stack_usage_check.py -- aggregate gcc -fstack-usage `.su` files into a
project-wide stack-bound report.

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

1. Walks every ``examples/*/*/build*/**/*.su`` file.
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

# --- Strict-mode partitioning -------------------------------------------------
#
# In `--strict` mode the soft per-app frame limit is promoted to a hard
# gate, but only for *first-party* code (everything outside
# THIRD_PARTY_PATH_FRAGMENTS). Third-party SOUP is exempt because:
#
#   * It is pre-qualified and lives under libs/third_party/ -- its
#     justification documents are filed under docs/SOUP/ (see
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
    #     "libs/ra8_epub/src/ra8_epub_open.c",
    #     "priv_parse_archive",
    #     4344,
    #     "ZIP central-directory parser: 4 kB scratch struct on stack "
    #     "to avoid heap; only invoked once at chapter open from the "
    #     "ereader worker thread (8 kB stack budget).",
    # ),
)


def is_first_party(tu_path: str) -> bool:
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
    __slots__ = ("app", "bytes_", "func", "qualifier", "su_file", "tu")

    def __init__(self, app, tu, func, bytes_, qualifier, su_file):  # noqa: PLR0913  # data class init, all fields are required
        self.app = app
        self.tu = tu
        self.func = func
        self.bytes_ = bytes_
        self.qualifier = qualifier
        self.su_file = su_file

    @property
    def is_dynamic(self) -> bool:
        return "dynamic" in self.qualifier.split(",")

    @property
    def is_critical(self) -> bool:
        return any(m in self.tu for m in CRITICAL_MODULES) or any(
            m in self.func for m in CRITICAL_MODULES
        )


# The CI cross-build fast path (scripts/build/all_examples.sh) compiles the
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
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="ascii", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["app", "tu", "function", "frame_bytes", "qualifier"])
        for e in entries:
            writer.writerow([e.app, e.tu, e.func, e.bytes_, e.qualifier])


def write_per_app(out_dir: Path, entries: list) -> None:
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


def find_violations(entries: list, frame_limit: int):
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
    ranked = sorted(entries, key=lambda r: r.bytes_, reverse=True)[:n]
    print(f"\nTop {len(ranked)} stack-frame offenders across all apps:")
    print(f"{'bytes':>8}  {'qualifier':<16}  app/function  (tu)")
    for r in ranked:
        print(f"{r.bytes_:>8}  {r.qualifier:<16}  {r.app}/{r.func}  ({r.tu})")


def main(argv: list) -> int:  # noqa: PLR0911 PLR0912  # parser/gate dispatch, splitting hurts readability
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
            "*first-party* TUs (everything outside libs/third_party/). "
            "Third-party SOUP remains exempt. First-party functions "
            "with a justified large frame must be enrolled in "
            "FIRST_PARTY_EXEMPTIONS at the top of this script."
        ),
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    su_files = find_su_files(repo_root)
    if not su_files:
        if args.warn_only or args.strict:
            # Pre-commit-friendly: a freshly cloned tree has no .su
            # files yet, and forcing a full app build inside the hook
            # would slow every commit by minutes. Skip silently.
            return 0
        print(
            "stack_usage_check: no .su files found under "
            "examples/*/*/build*/.\n  Build at least one app with "
            "-fstack-usage first (e.g. `make blink`).",
            file=sys.stderr,
        )
        return 0

    all_entries = []
    for su in su_files:
        app = app_name_for(su, repo_root)
        all_entries.extend(parse_su(su, app))

    if not all_entries:
        print("stack_usage_check: parsed 0 functions; .su files empty?", file=sys.stderr)
        return 0

    out_dir = repo_root / "build"
    write_csv(out_dir / "stack_usage.csv", all_entries)
    write_per_app(out_dir, all_entries)

    critical, soft = find_violations(all_entries, args.frame_limit)

    if not args.quiet:
        print(
            f"stack_usage_check: parsed {len(all_entries)} functions "
            f"from {len(su_files)} .su files."
        )
        print(f"  CSV:        {out_dir / 'stack_usage.csv'}")
        print(f"  per-app:    {out_dir}/stack_usage_<app>.txt")
        print_top_n(all_entries, args.top)

    if soft:
        print(
            f"\nSOFT violations: {len(soft)} function(s) over {args.frame_limit} bytes or dynamic:"
        )
        for e in sorted(soft, key=lambda r: r.bytes_, reverse=True):
            print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")

    # --- Strict gate for first-party code -----------------------------------
    # Promotes any soft violation in a first-party TU into a hard
    # failure unless the (tu, func) pair is in FIRST_PARTY_EXEMPTIONS.
    if args.strict:
        offenders = []
        for e in soft:
            if not is_first_party(e.tu):
                continue
            allowed = exemption_for(e.tu, e.func)
            if allowed > 0 and e.bytes_ <= allowed:
                continue
            offenders.append(e)
        if offenders:
            print(
                f"\nSTRICT first-party violations: "
                f"{len(offenders)} function(s) exceed "
                f"{args.frame_limit} bytes and are not exempt:"
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

    if critical:
        print(
            f"\nCRITICAL-PATH violations "
            f"(>{CRITICAL_FRAME_LIMIT} bytes or dynamic in "
            f"{','.join(CRITICAL_MODULES)}):"
        )
        for e in sorted(critical, key=lambda r: r.bytes_, reverse=True):
            print(f"  [{e.app}] {e.func} ({e.tu}): {e.bytes_} bytes [{e.qualifier}]")
        if not args.warn_only:
            return 1

    # NASA Power-of-10 Rule 3 hard gate: a `dynamic` qualifier means
    # the function uses a VLA or `alloca()`. Both are forbidden in
    # this firmware regardless of frame size, in any module, and the
    # gate applies even in --warn-only mode (i.e. always-on for
    # pre-commit). No deviation procedure is offered.
    dyn = [e for e in all_entries if e.is_dynamic]
    if dyn:
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

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

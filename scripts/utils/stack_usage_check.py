#!/usr/bin/env python3
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
   (ra_isr, ra_check, ra_err, ra_mpu, ra_cgc, ra_pfs) has any function
   with frame > 256 bytes or any ``dynamic`` qualifier anywhere.

This script is report-only by default for the rest of the codebase --
the per-target ``-Wstack-usage=N`` warning (in cmake/ra_warnings.cmake)
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
    "ra_isr",
    "ra_check",
    "ra_err",
    "ra_mpu",
    "ra_cgc",
    "ra_pfs",
)

_SU_LINE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+):(?P<func>[^\s\t]+)"
    r"[\t ]+(?P<bytes>\d+)[\t ]+(?P<qual>[A-Za-z,]+)\s*$"
)


class StackEntry:
    __slots__ = ("app", "tu", "func", "bytes_", "qualifier", "su_file")

    def __init__(self, app, tu, func, bytes_, qualifier, su_file):
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


def find_su_files(repo_root: Path) -> list:
    out = []
    examples = repo_root / "examples"
    if not examples.is_dir():
        return out
    for tier in sorted(examples.iterdir()):
        if not tier.is_dir():
            continue
        for app in sorted(tier.iterdir()):
            if not app.is_dir():
                continue
            for build in app.glob("build*"):
                if not build.is_dir():
                    continue
                out.extend(build.rglob("*.su"))
    return out


def app_name_for(su_file: Path, repo_root: Path) -> str:
    try:
        rel = su_file.relative_to(repo_root / "examples")
    except ValueError:
        return "unknown"
    parts = rel.parts
    if len(parts) >= 2:
        return parts[1]
    return "unknown"


def parse_su(su_file: Path, app: str) -> list:
    entries = []
    try:
        text = su_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return entries
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        match = _SU_LINE.match(line)
        if not match:
            continue
        try:
            nbytes = int(match.group("bytes"))
        except ValueError:
            continue
        entries.append(StackEntry(
            app,
            match.group("path"),
            match.group("func"),
            nbytes,
            match.group("qual"),
            su_file,
        ))
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
                fh.write(
                    f"{r.bytes_:>8}  {r.qualifier:<16}  {r.func}  {r.tu}\n"
                )


def find_violations(entries: list, frame_limit: int):
    critical = []
    soft = []
    for e in entries:
        if e.is_critical:
            if e.bytes_ > CRITICAL_FRAME_LIMIT or e.is_dynamic:
                critical.append(e)
        else:
            if e.bytes_ > frame_limit or e.is_dynamic:
                soft.append(e)
    return critical, soft


def print_top_n(entries: list, n: int) -> None:
    ranked = sorted(entries, key=lambda r: r.bytes_, reverse=True)[:n]
    print(f"\nTop {len(ranked)} stack-frame offenders across all apps:")
    print(f"{'bytes':>8}  {'qualifier':<16}  app/function  (tu)")
    for r in ranked:
        print(
            f"{r.bytes_:>8}  {r.qualifier:<16}  {r.app}/{r.func}  ({r.tu})"
        )


def main(argv: list) -> int:
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
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    su_files = find_su_files(repo_root)
    if not su_files:
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
        print("stack_usage_check: parsed 0 functions; .su files empty?",
              file=sys.stderr)
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
            f"\nSOFT violations: {len(soft)} function(s) over "
            f"{args.frame_limit} bytes or dynamic:"
        )
        for e in sorted(soft, key=lambda r: r.bytes_, reverse=True):
            print(
                f"  [{e.app}] {e.func} ({e.tu}): "
                f"{e.bytes_} bytes [{e.qualifier}]"
            )

    if critical:
        print(
            f"\nCRITICAL-PATH violations "
            f"(>{CRITICAL_FRAME_LIMIT} bytes or dynamic in "
            f"{','.join(CRITICAL_MODULES)}):"
        )
        for e in sorted(critical, key=lambda r: r.bytes_, reverse=True):
            print(
                f"  [{e.app}] {e.func} ({e.tu}): "
                f"{e.bytes_} bytes [{e.qualifier}]"
            )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

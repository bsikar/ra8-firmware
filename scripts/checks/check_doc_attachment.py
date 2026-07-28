#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a Doxygen block must describe the thing it is actually attached to.

The presence gates (``doxy_audit.py --check`` for functions,
``doxy_audit.py --members --check`` for members/enums/macros) only ask *is a
block there*.  They cannot see a block that is there but describes something
else, and a misattached block actively **satisfies** them: paste one block
twice and measured "coverage" goes up while one symbol silently loses its
documentation and another gains a duplicate.  Every defect this gate finds was
therefore invisible to -- and in some cases rewarded by -- the existing gates.

Real defects that motivated this (both found by eye, in ``tools/rabook_viewer``):

* ``main()``'s block sat immediately above ``viewer_log_sink()``'s block, so
  ``main()`` was undocumented and the sink was documented twice.
* ``viewer_compute_tiles()`` carried its block on a forward declaration while
  the definition below it sat bare.

Why a sibling script rather than a new ``doxy_audit.py`` mode:

* **Different question, different parser.**  ``doxy_audit.py`` is regex-driven
  end to end; its ``FUNC_RE`` + ``strip_comments`` design answers "is a tag
  present".  Attachment needs real parameter names, real return types and real
  declaration-vs-definition identity -- an AST question.  Bolting a libclang
  mode onto a regex tool means two parsers, two scope constants and two notions
  of "a function" in one file, which is precisely the confusion that let
  ``doxy_audit.py``'s own scope hole (no ``tools/``, ``tests/``, ``examples/``)
  survive unnoticed.
* **Different dependency.**  This gate hard-requires libclang and must fail
  loudly without it (see ``docattach_ast._require_libclang``).  ``doxy_audit.py``
  has no third-party dependency and must keep working in environments that
  lack one.
* **Single Responsibility** (CLAUDE.md, SOLID for C): presence and correctness
  are separate concerns with separate failure modes and separate fix
  procedures.

Scope: every first-party ``.c`` / ``.h`` under ``libs/``, ``src/``, ``port/``,
``examples/``, ``tools/`` and ``tests/``.  Vendored SOUP (``libs/third_party``)
and generated data (``libs/ra8_fonts``) are excluded, matching CLAUDE.md.

Module layout
-------------
This file is the driver only.  The checker is split by what each part needs to
do its job (#359), which is also the axis along which the two passes must not
be allowed to disagree:

    :mod:`docattach_scope`     which files are read
    :mod:`docattach_model`     finding codes, tag grammar, and the shared records
    :mod:`docattach_lex`       the findings answerable from text alone
    :mod:`docattach_ast`       libclang setup and the findings needing a parse
    :mod:`docattach_selftest`  both-direction fixtures for every finding code

Run::

    check_doc_attachment.py --check     # CI gate (exit 1 on any finding)
    check_doc_attachment.py             # audit listing, exit 0
    check_doc_attachment.py --selftest  # synthetic both-direction fixtures
    check_doc_attachment.py PATH ...    # restrict to the given files/dirs

Exit 0 when clean, 1 on findings in ``--check`` mode, 2 on a selftest failure,
a missing/unusable libclang, or a whole-tree scan that collapsed below
FILE_FLOOR.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from docattach_ast import _include_args, _require_libclang, check_file
from docattach_model import CODE_HELP, Finding
from docattach_scope import default_targets, iter_sources
from docattach_selftest import selftest

# A tree this size cannot legitimately collapse to a handful of files. If the
# whole-tree enumeration returns less than this, something broke (a renamed
# root, an unreachable repo root) and reporting ``findings=0 (PASS)`` would be
# a lie: a misattached block cannot be found in a file nobody parsed. Measured
# 2026-07-28: 2116 first-party C sources. Same trip-wire as check_ruff.py.
FILE_FLOOR = 1700


def run(targets: list[Path], strict: bool, *, enforce_floor: bool) -> int:
    """Scan ``targets`` and report.

    Args:
        targets: Files and/or directories to sweep.
        strict: When true a finding fails the run (the ``--check`` gate mode);
            otherwise findings are printed advisory-only.
        enforce_floor: Apply FILE_FLOOR to the enumerated source list. Set only
            for the default whole-tree scan -- an explicit argv path list is a
            deliberately narrowed scope, not a collapsed one.

    Returns:
        0 when clean or running advisory-only, 1 on a finding under ``strict``,
        2 when ``enforce_floor`` is set and the enumeration fell below
        FILE_FLOOR.
    """
    cindex = _require_libclang()
    args = ["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *_include_args(cindex)]

    files = iter_sources(targets)
    if enforce_floor and len(files) < FILE_FLOOR:
        print(
            f"check_doc_attachment.py: FATAL -- only {len(files)} source file(s) in "
            f"scope, floor is {FILE_FLOOR}. A collapsed scope reports a clean tree "
            "because it parsed nothing.",
            file=sys.stderr,
        )
        return 2
    findings: list[Finding] = []
    for path in files:
        findings.extend(check_file(path, cindex, args))

    findings.sort(key=lambda f: (f.path, f.line, f.code))
    if not findings:
        print(f"check_doc_attachment: files={len(files)} findings=0 (PASS)")
        return 0

    by_code: dict[str, int] = {}
    for f in findings:
        by_code[f.code] = by_code.get(f.code, 0) + 1

    verdict = "FAIL" if strict else "audit"
    print(f"check_doc_attachment: files={len(files)} findings={len(findings)} ({verdict})")
    for code in sorted(by_code):
        print(f"  {code}  x{by_code[code]:<5} {CODE_HELP[code]}")
    print()
    for f in findings:
        print(f.render())
    return 1 if strict else 0


def main() -> int:
    """Report Doxygen blocks that describe something other than what they precede.

    ``--check`` is what makes this a gate: without it findings are printed and
    the process still exits 0, which is the advisory mode used while a module
    is being cleaned up. CI must pass ``--check`` or the step cannot fail.

    With no positional paths the scan covers every first-party root, so the
    argument list narrows the sweep and never widens it. FILE_FLOOR is applied
    only to that default sweep, and exits 2 below it: a narrowed scope is a
    request, while a collapsed sweep is a broken enumeration reporting a clean
    tree because it parsed nothing.

    Returns 0 when clean, when running advisory-only, or after a passing
    ``--selftest``; 1 on a finding under ``--check`` or a failing selftest;
    2 when the default sweep enumerated too few files to trust.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="CI gate: exit 1 on any finding")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic fixtures")
    ap.add_argument("paths", nargs="*", help="files/dirs to scan (default: every first-party root)")
    ns = ap.parse_args()

    if ns.selftest:
        return selftest()

    targets = [Path(p).resolve() for p in ns.paths] if ns.paths else default_targets()
    return run(targets, strict=ns.check, enforce_floor=not ns.paths)


if __name__ == "__main__":
    sys.exit(main())

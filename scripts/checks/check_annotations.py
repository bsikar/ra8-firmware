#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_annotations.py -- libclang-based annotation enforcement.

This script walks the AST of every C / C++ translation unit under
``libs/``, ``src/``, ``examples/``, ``tests/``, ``port/`` and
``tools/`` -- every first-party source root, per CLAUDE.md ("Scope") --
and applies the project's annotation-enforcement rules. The annotation
macros themselves are defined in ``libs/ra8_core/inc/ra8_attributes.h``
and lower to ``[[clang::annotate("ra8_<rule>:...")]]`` markers that
libclang exposes via ``AnnotateAttr`` cursors.

The enforceable rules are documented in ``docs/ANNOTATIONS.md``; this
script is the canonical implementation of their static checks. Every
rule is fatal -- there is no warn-only mode. A gate that reports a
known gap without failing is a gate that hides the gap.

Two self-checks run before any rule does, because both failure modes
look exactly like success:

* ``annot_rulekeys.check_rule_keys()`` cross-checks every rule key this
  script dispatches on against the annotation strings
  ``ra8_attributes.h`` actually emits. A rule keyed on a string no macro
  produces matches nothing and reports zero violations forever.
* ``annot_clang.check_parse_integrity()`` fails when a header does not
  resolve or when the fraction of call sites libclang could resolve
  drops below ``MIN_CALL_RESOLUTION``. An incomplete parse silently
  starves the call-graph rules, and fewer findings is not better news.

Module layout
-------------
This file is the entry point only: argument parsing, the sweep, and the
report. The checker itself is a pipeline, split one module per stage so
that no stage can quietly redefine another's assumptions:

    :mod:`annot_scope`     which files are in scope, and what module owns each
    :mod:`annot_model`     the records a parse produces and the rules consume
    :mod:`annot_rulekeys`  the rule vocabulary, cross-checked against the header
    :mod:`annot_clang`     libclang setup, compile flags, and the parse floor
    :mod:`annot_source`    source text read back for the two macro-level checks
    :mod:`annot_walk`      AST traversal into a USR-keyed symbol table
    :mod:`annot_rules`     one function per rule, dispatched by key
    :mod:`annot_linkage`   the rule defined over the ABSENCE of an annotation
    :mod:`annot_loopbound` the textual per-loop bound-marker scan (no libclang)
    :mod:`annot_selftest`  synthetic-tree regression tests, both directions

Usage::

    python3 scripts/checks/check_annotations.py            # report
    python3 scripts/checks/check_annotations.py --check    # CI gate
    python3 scripts/checks/check_annotations.py --naming-audit  # full prefix/storage audit
    python3 scripts/checks/check_annotations.py --naming-audit --json  # machine-readable inventory
    python3 scripts/checks/check_annotations.py --list     # dump symbols
    python3 scripts/checks/check_annotations.py --selftest # regression test
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from annot_clang import check_parse_integrity, parse_tu
from annot_loopbound import discover_loopbound_files, enforce_loop_bounds
from annot_model import AnnotatedSymbol, Violation, WalkState
from annot_rulekeys import check_rule_keys
from annot_rules import enforce_rules
from annot_scope import (
    SOURCE_SUFFIXES,
    discover_translation_units,
    relative,
    repo_root,
)
from annot_selftest import run_selftest
from annot_walk import walk_tu


def _parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the command line."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check", action="store_true", help="exit non-zero on any (non-informational) violation"
    )
    ap.add_argument("--list", action="store_true", help="dump every annotated symbol and exit")
    ap.add_argument(
        "--naming-audit",
        action="store_true",
        help="include the full s_/internal_/priv_ naming contract (whole tree only)",
    )
    ap.add_argument(
        "--json", action="store_true", help="emit the findings and parse summary as JSON"
    )
    ap.add_argument("--quiet", action="store_true", help="suppress per-TU progress output")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="run the checker's own regression tests on synthetic TUs and exit",
    )
    ap.add_argument("paths", nargs="*", help="optional explicit file list (else scan everything)")
    return ap.parse_args(argv)


@dataclass
class Sweep(WalkState):
    """Everything one pass over the tree produced.

    The symbol table and the parse evidence travel together on purpose: a
    rule result is only meaningful alongside the ``stats`` proving the parse
    that produced it was complete.
    """

    tu_count: int = 0

    def summary(self) -> str:
        """One line of evidence about what the parse actually saw."""
        seen = self.stats.calls_seen
        resolution = self.stats.calls_resolved / seen if seen else 0.0
        return (
            f"{len(self.symbols)} functions, {len(self.data_symbols)} data definitions, "
            f"{self.stats.calls_resolved}/{seen} call sites resolved "
            f"({resolution:.1%}), {len(self.vector_entries)} vector-table entries, "
            f"{self.tu_count} TUs"
        )


def _sweep(tus: list[pathlib.Path], *, progress: bool) -> Sweep:
    """Parse and walk every TU, returning the symbol table and the evidence."""
    out = Sweep(tu_count=len(tus))
    for tu_path in tus:
        if progress:
            print(f"  parsing {tu_path.relative_to(repo_root())}", file=sys.stderr)
        tu = parse_tu(tu_path, out.stats)
        if tu is None:
            continue
        try:
            walk_tu(tu, out)
        except Exception as exc:  # noqa: BLE001 -- one TU must not abort the sweep; the parse floor catches wholesale failure
            sys.stderr.write(f"  WARN: walk failed for {tu_path}: {exc}\n")
    return out


def _list_symbols(symbols: dict[str, AnnotatedSymbol]) -> int:
    """Dump every annotated symbol, one per line."""
    if not symbols:
        print("(no symbols found)")
        return 0
    print(f"{'symbol':<48} {'file':<60} {'annotations'}")
    for s in sorted(symbols.values(), key=lambda x: (x.file, x.line)):
        if not s.annotations:
            continue
        print(f"{s.name:<48} {relative(s.file):<60} {','.join(s.annotations)}")
    return 0


def _collect_violations(
    sweep: Sweep,
    loopbound_files: list[pathlib.Path],
    *,
    partial: bool,
    naming_contract: bool,
) -> list[Violation]:
    """Run the self-checks and the rules appropriate to the scan's scope."""
    violations = check_rule_keys()
    # An explicit file list parses a fraction of the tree on purpose, so
    # the whole-tree evidence checks cannot say anything about it.
    if partial:
        sys.stderr.write(
            "check_annotations: explicit file list -- parse-integrity and the "
            "linkage rule need the whole tree and are skipped. Run without "
            "arguments for the gate.\n"
        )
        violations.extend(enforce_rules(sweep, whole_tree=False, naming_contract=False))
    else:
        violations.extend(check_parse_integrity(sweep.stats, sweep.tu_count))
        violations.extend(enforce_rules(sweep, naming_contract=naming_contract))
    # The loop-bound marker scan is textual and per-file, so it stands on its
    # own regardless of the parse. It runs in both modes; only the whole-tree
    # gate insists the scan actually saw files (an empty glob there is a broken
    # gate, not a clean tree).
    violations.extend(enforce_loop_bounds(loopbound_files, require_nonempty=not partial))
    return violations


def _report(violations: list[Violation], summary: str, *, quiet: bool, json_output: bool) -> int:
    """Print the findings and return the process exit code."""
    if json_output:
        print(
            json.dumps(
                {
                    "summary": summary,
                    "fatal_count": sum(not violation.warn_only for violation in violations),
                    "informational_count": sum(violation.warn_only for violation in violations),
                    "findings": [
                        {
                            "severity": "informational" if violation.warn_only else "fatal",
                            "rule": violation.rule,
                            "file": relative(violation.file),
                            "line": violation.line,
                            "message": violation.message,
                        }
                        for violation in violations
                    ],
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 1 if any(not violation.warn_only for violation in violations) else 0
    if not violations:
        if not quiet:
            print(f"check_annotations: 0 violations across {summary}")
        return 0

    fatal = [v for v in violations if not v.warn_only]
    informational = [v for v in violations if v.warn_only]

    for v in violations:
        tag = "INFO" if v.warn_only else "FAIL"
        sys.stderr.write(f"[{tag}] {relative(v.file)}:{v.line}: [{v.rule}] {v.message}\n")

    sys.stderr.write(
        f"check_annotations: {len(fatal)} fatal, {len(informational)} informational "
        f"across {summary}\n"
    )
    return 1 if fatal else 0


def main(argv: list[str]) -> int:
    """Run the annotation gate, or one of its three inspection modes.

    Naming paths on the command line deliberately DOWNGRADES the run rather
    than narrowing it: parse-integrity and the linkage rule are both
    whole-tree properties, so with an explicit file list they are skipped and
    a warning goes to stderr. That mode is for iterating on one file. The gate
    proper must be invoked with no paths, or it reports a clean tree having
    parsed a fraction of it -- the exact failure this checker exists to catch
    in other tools.

    Returns 0 when nothing fatal was found; a warn-only violation is printed
    as INFO and still exits 0, so only a real rule breach fails the build.
    """
    args = _parse_args(argv)

    if args.selftest:
        return run_selftest()

    partial = bool(args.paths)
    if partial and args.naming_audit:
        ap_error = "--naming-audit is a whole-tree contract and cannot be combined with paths"
        sys.stderr.write(f"check_annotations: {ap_error}\n")
        return 2
    if partial:
        tus = [
            pathlib.Path(p).resolve()
            for p in args.paths
            if pathlib.Path(p).suffix in SOURCE_SUFFIXES
        ]
        # The loop-bound scan also covers headers, so it takes the raw list
        # (it self-filters by suffix), not the .c/.cpp-only translation units.
        loopbound_files = [pathlib.Path(p).resolve() for p in args.paths]
    else:
        tus = discover_translation_units()
        loopbound_files = discover_loopbound_files()

    sweep = _sweep(tus, progress=not args.quiet and not args.check)

    if args.list:
        return _list_symbols(sweep.symbols)

    violations = _collect_violations(
        sweep,
        loopbound_files,
        partial=partial,
        naming_contract=args.naming_audit,
    )
    return _report(
        violations,
        sweep.summary(),
        quiet=args.quiet,
        json_output=args.json,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

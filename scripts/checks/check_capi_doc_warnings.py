#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the C ABI reference's Doxygen warnings are a ledger that only shrinks.

``scripts/builders/docs_capi.sh`` builds the ADR-0005 ``/api/c/`` slot and
prints how many warning lines Doxygen emitted.  Printing is not a gate: the
count has no floor and nothing fails, so a new public header can arrive with a
``@return`` on a ``void`` function, a broken ``\\ref`` or malformed list markup
and the reference silently ships a page that renders the defect.  Every warning
in the log today is exactly that shape of defect, so the interesting question is
not "how many" but "which ones, and are they the same ones as yesterday".

This gate answers that.  Each warning line is normalised into a stable
``(file, class, key)`` triple and compared against
``.github/capi-doc-warning-baseline.txt``:

* a triple that is not on the ledger, or one whose count grew, is a REGRESSION;
* a triple on the ledger that no longer fires, or fires less, is STALE and must
  be re-emitted with ``--update``, so paying a warning down is recorded rather
  than silently absorbed.

Line numbers are deliberately not part of the key.  Any edit above a warning
shifts them, which would make the ledger churn on unrelated commits and train
everyone to re-emit it without reading the diff.

Two warning classes are environment-dependent rather than defects: Doxygen
drops ``\\dot`` blocks when graphviz is absent and fails formula rendering when
latex is absent.  Baselining those would bake one machine's missing packages
into the repo, so they are excluded from the ledger.  Excluded is not ignored:
if the tool IS installed and the warning still fires, the builder is not
configuring Doxygen correctly and that is a finding.

A message matching no class is also a finding.  A classifier that quietly
skipped what it did not recognise would be a blind spot the size of every
warning shape Doxygen grows in its next release.

Run::

    check_capi_doc_warnings.py --check      # CI gate (exit 1 on any finding)
    check_capi_doc_warnings.py --update     # re-emit the ledger from the log
    check_capi_doc_warnings.py --selftest   # synthetic both-direction fixtures
    check_capi_doc_warnings.py --emit-classes
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG = REPO_ROOT / "build" / "docs" / "doxygen-capi-warnings.log"
DEFAULT_BASELINE = REPO_ROOT / ".github" / "capi-doc-warning-baseline.txt"

NO_FILE = "-"

BASELINE_HEADER = """\
# Doxygen warning ledger for the C ABI reference (ADR-0005 /api/c/ slot).
#
# Emitted by `python3 scripts/checks/check_capi_doc_warnings.py --update` from
# the log that `scripts/builders/docs_capi.sh` writes.  Never hand-edit: every
# field is re-derived from that log, so an edit is either a no-op or a lie the
# gate finds on the next build.
#
# <file>\t<class>\t<key>\t<count>
#
# Frozen debt.  A triple that is missing or whose count grew fails as a
# regression; a triple that fires less often fails as stale, so paying one down
# is recorded here rather than silently absorbed.  Line numbers are not part of
# the key: they churn on every edit above the warning.
#
# Classes are defined in the checker (`--emit-classes`).  Environment-dependent
# warnings (graphviz, latex) are never baselined; the checker fails instead when
# one fires on a machine that HAS the tool.
#
# Columns are TAB-separated.  Rows are sorted.
"""


@dataclass(frozen=True)
class WarningClass:
    """One recognised Doxygen warning shape."""

    ident: str
    pattern: re.Pattern[str]
    env_tool: str | None
    reason: str

    def key_for(self, match: re.Match[str]) -> str:
        """Return the stable key from a match of *this class's* pattern.

        A class whose pattern captures no ``key`` group has nothing to
        distinguish one occurrence from another, so the class id is the key.
        """
        key = match.groupdict().get("key")
        return key if key else self.ident


CLASSES: tuple[WarningClass, ...] = (
    WarningClass(
        "env-dot",
        re.compile(r"^ignoring \\dot command because HAVE_DOT is not set$"),
        "dot",
        "graphviz absent: the authored diagram block is dropped, not broken",
    ),
    WarningClass(
        "env-latex",
        re.compile(r"^Problems running latex\."),
        "latex",
        "latex absent: formula images cannot be rasterised on this machine",
    ),
    WarningClass(
        "void-return",
        re.compile(
            r"^found documented return type for (?P<key>[A-Za-z0-9_]+) "
            r"that does not return anything$"
        ),
        None,
        "a @return documented on a function that returns void",
    ),
    WarningClass(
        "unresolved-ref",
        re.compile(r"^unable to resolve reference to '(?P<key>[^']+)' for \\ref command$"),
        None,
        "a \\ref whose target is outside the C ABI input set",
    ),
    WarningClass(
        "list-markup",
        re.compile(
            r"^(?P<key>Invalid list item found"
            r"|End of list marker found without any preceding list items)$"
        ),
        None,
        "malformed list markup in a documentation block",
    ),
)

CLASS_BY_ID = {cls.ident: cls for cls in CLASSES}
ENV_CLASS_IDS = frozenset(cls.ident for cls in CLASSES if cls.env_tool)

LOCATED_LINE = re.compile(r"^(?P<path>.+?):(?P<line>\d+): (?:warning|error): (?P<msg>.*)$")
BARE_LINE = re.compile(r"^(?:warning|error): (?P<msg>.*)$")
# Doxygen wraps long messages; a continuation is indented and carries no
# severity of its own, so it belongs to the line above rather than being a
# warning in its own right.
CONTINUATION = re.compile(r"^\s+\S")


def relativise(raw: str) -> str:
    """Return *raw* relative to the repo root when it lives under it."""
    candidate = Path(raw)
    try:
        return str(candidate.resolve().relative_to(REPO_ROOT))
    except (ValueError, OSError):
        return raw


def classify(message: str) -> tuple[WarningClass, re.Match[str]] | None:
    """Return the class that recognises *message*, with its own match object.

    The match must come from the class pattern, not from the log-line grammar:
    only the class pattern captures the ``key`` group that makes a warning
    identifiable across edits.
    """
    for cls in CLASSES:
        found = cls.pattern.match(message)
        if found:
            return cls, found
    return None


def parse_log(text: str) -> tuple[Counter[tuple[str, str, str]], list[str]]:
    """Normalise a Doxygen warning log into counted triples plus findings."""
    observed: Counter[tuple[str, str, str]] = Counter()
    findings: list[str] = []
    seen_any = False

    for lineno, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip():
            continue
        if seen_any and CONTINUATION.match(raw):
            continue

        located = LOCATED_LINE.match(raw)
        bare = None if located else BARE_LINE.match(raw)
        if not located and not bare:
            findings.append(
                f"log line {lineno} is not a Doxygen diagnostic this gate can read: {raw!r}"
            )
            continue
        seen_any = True

        match_obj = located or bare
        assert match_obj is not None
        message = match_obj.group("msg").strip()
        path = relativise(located.group("path")) if located else NO_FILE

        classified = classify(message)
        if classified is None:
            findings.append(
                f"log line {lineno} ({path}) is a warning shape this gate does not "
                f"classify: {message!r}. Add a class to CLASSES rather than widening "
                "the ledger."
            )
            continue

        cls, class_match = classified
        observed[(path, cls.ident, cls.key_for(class_match))] += 1

    return observed, findings


def check_environment(
    observed: Counter[tuple[str, str, str]],
    tool_present: Callable[[str], object],
) -> tuple[Counter[tuple[str, str, str]], list[str]]:
    """Drop environment-dependent warnings, or fail when the tool is installed."""
    findings: list[str] = []
    kept: Counter[tuple[str, str, str]] = Counter()

    for triple, count in observed.items():
        _, class_id, _ = triple
        cls = CLASS_BY_ID[class_id]
        if cls.env_tool is None:
            kept[triple] = count
            continue
        if tool_present(cls.env_tool):
            findings.append(
                f"{triple[0]}: {count} '{class_id}' warning(s) although "
                f"{cls.env_tool!r} IS installed, so this is a builder defect, not a "
                "missing package. Fix how docs_capi.sh configures Doxygen."
            )

    return kept, findings


def load_baseline(path: Path) -> tuple[Counter[tuple[str, str, str]], list[str]]:
    baseline: Counter[tuple[str, str, str]] = Counter()
    findings: list[str] = []

    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != 4:
            findings.append(f"baseline line {lineno} needs 4 tab-separated fields: {raw!r}")
            continue
        file_part, class_id, key, count_text = fields
        if class_id not in CLASS_BY_ID:
            findings.append(f"baseline line {lineno} names unknown class {class_id!r}")
            continue
        if class_id in ENV_CLASS_IDS:
            findings.append(
                f"baseline line {lineno} baselines environment-dependent class "
                f"{class_id!r}; those are never frozen into the repo"
            )
            continue
        if not count_text.isdigit() or int(count_text) < 1:
            findings.append(f"baseline line {lineno} has a non-positive count {count_text!r}")
            continue
        triple = (file_part, class_id, key)
        if triple in baseline:
            findings.append(f"baseline line {lineno} duplicates an earlier row: {raw!r}")
            continue
        baseline[triple] = int(count_text)

    return baseline, findings


def compare(
    observed: Counter[tuple[str, str, str]],
    baseline: Counter[tuple[str, str, str]],
) -> list[str]:
    findings: list[str] = []

    for triple in sorted(set(observed) | set(baseline)):
        now = observed.get(triple, 0)
        was = baseline.get(triple, 0)
        if now == was:
            continue
        file_part, class_id, key = triple
        where = "" if file_part == NO_FILE else f"{file_part}: "
        if now > was:
            findings.append(
                f"REGRESSION {where}{class_id} {key!r} fires {now} time(s), "
                f"ledger allows {was}. Fix the documentation block; do not re-emit "
                "the ledger to absorb it."
            )
        else:
            findings.append(
                f"STALE {where}{class_id} {key!r} fires {now} time(s), ledger still "
                f"claims {was}. Re-emit with --update: the ledger only shrinks."
            )

    return findings


def render_baseline(observed: Counter[tuple[str, str, str]]) -> str:
    rows = [
        "\t".join((file_part, class_id, key, str(count)))
        for (file_part, class_id, key), count in sorted(observed.items())
    ]
    total = sum(observed.values())
    summary = f"# rows: {len(rows)}  warnings: {total}\n"
    return BASELINE_HEADER + summary + "\n" + "".join(f"{row}\n" for row in rows)


def emit_classes() -> str:
    lines = ["classes recognised by check_capi_doc_warnings:"]
    for cls in CLASSES:
        scope = f"environment ({cls.env_tool})" if cls.env_tool else "ledgered"
        lines.append(f"  {cls.ident:16s} {scope:22s} {cls.reason}")
    return "\n".join(lines)


def run_check(
    log_path: Path,
    baseline_path: Path,
    tool_present: Callable[[str], object] = shutil.which,
) -> tuple[list[str], Counter[tuple[str, str, str]]]:
    """Return every finding plus the ledgerable triples the log produced."""
    if not log_path.is_file():
        return (
            [
                f"{log_path} not found. Run `bash scripts/builders/docs_capi.sh` "
                "first: this gate reads the log that build writes."
            ],
            Counter(),
        )

    observed, findings = parse_log(log_path.read_text())
    observed, env_findings = check_environment(observed, tool_present)
    findings.extend(env_findings)

    if not baseline_path.is_file():
        findings.append(f"{baseline_path} not found; emit it with --update")
        return findings, observed

    baseline, baseline_findings = load_baseline(baseline_path)
    findings.extend(baseline_findings)
    if not baseline_findings:
        findings.extend(compare(observed, baseline))

    return findings, observed


def _probe(
    name: str,
    log: str,
    baseline: str,
    expect_ok: bool,
    expect_text: str | None = None,
    tools: Iterable[str] = (),
) -> tuple[bool, str]:
    present = set(tools)
    with tempfile.TemporaryDirectory() as tmp:
        log_path = Path(tmp) / "warnings.log"
        log_path.write_text(log)
        baseline_path = Path(tmp) / "baseline.txt"
        baseline_path.write_text(baseline)
        findings, _ = run_check(
            log_path,
            baseline_path,
            tool_present=lambda tool: tool in present,
        )
    ok = not findings
    if ok != expect_ok:
        return False, f"{name}: expected {'no findings' if expect_ok else 'a finding'}, got {findings}"
    if expect_text and not any(expect_text in finding for finding in findings):
        return False, f"{name}: no finding mentioned {expect_text!r}; got {findings}"
    return True, f"{name}: OK"


VOID = (
    "/repo/libs/ra8_core/inc/ra8_log.h:150: warning: found documented return type "
    "for ra8_log_set_byte_sink that does not return anything"
)
DOT = (
    "/repo/libs/ra8_hal/inc/ra8_wdt.h:12: warning: ignoring \\dot command "
    "because HAVE_DOT is not set"
)
LATEX = (
    "error: Problems running latex. Check your installation or look for typos in "
    "_formulas.tex and check _formulas.log!"
)
REF = (
    "/repo/libs/ra8_dfu/inc/ra8_rot.h:73: warning: unable to resolve reference to "
    "'md_docs_2formats_2ROT1' for \\ref command"
)
LIST = "/repo/libs/ra8_hal/inc/ra8_lvd.h:44: warning: Invalid list item found"


def selftest() -> int:
    # The fixtures use /repo paths, which do not resolve under the real root, so
    # relativise() leaves them alone; the ledger rows below match on purpose for
    # the real-root case and are exercised by the repo-relative rows in --check.
    void_row = "/repo/libs/ra8_core/inc/ra8_log.h\tvoid-return\tra8_log_set_byte_sink\t1"
    ref_row = "/repo/libs/ra8_dfu/inc/ra8_rot.h\tunresolved-ref\tmd_docs_2formats_2ROT1\t1"
    list_row = "/repo/libs/ra8_hal/inc/ra8_lvd.h\tlist-markup\tInvalid list item found\t1"

    probes = [
        ("empty log, empty ledger", "", "", True, None, ()),
        ("ledgered void-return matches", VOID + "\n", void_row + "\n", True, None, ()),
        (
            "void-return absent from the ledger",
            VOID + "\n",
            "",
            False,
            "REGRESSION",
            (),
        ),
        (
            "void-return fires twice, ledger allows one",
            VOID + "\n" + VOID + "\n",
            void_row + "\n",
            False,
            "REGRESSION",
            (),
        ),
        (
            "ledger row no longer fires",
            "",
            void_row + "\n",
            False,
            "STALE",
            (),
        ),
        ("unresolved-ref ledgered", REF + "\n", ref_row + "\n", True, None, ()),
        ("list-markup ledgered", LIST + "\n", list_row + "\n", True, None, ()),
        ("dot warning ignored without graphviz", DOT + "\n", "", True, None, ()),
        (
            "dot warning fails when graphviz is installed",
            DOT + "\n",
            "",
            False,
            "builder defect",
            ("dot",),
        ),
        ("latex error ignored without latex", LATEX + "\n", "", True, None, ()),
        (
            "latex error fails when latex is installed",
            LATEX + "\n",
            "",
            False,
            "builder defect",
            ("latex",),
        ),
        (
            "environment class may not be baselined",
            DOT + "\n",
            "libs/ra8_hal/inc/ra8_wdt.h\tenv-dot\tenv-dot\t1\n",
            False,
            "never frozen",
            (),
        ),
        (
            "unknown warning shape fails",
            "/repo/libs/ra8_hal/inc/ra8_lvd.h:9: warning: some future doxygen "
            "complaint\n",
            "",
            False,
            "does not classify",
            (),
        ),
        (
            "unreadable log line fails",
            "this is not a doxygen diagnostic\n",
            "",
            False,
            "not a Doxygen diagnostic",
            (),
        ),
        (
            "wrapped continuation line is not a warning of its own",
            REF + "\n    (continued explanation from doxygen)\n",
            ref_row + "\n",
            True,
            None,
            (),
        ),
        (
            "malformed ledger row fails",
            "",
            "libs/ra8_core/inc/ra8_log.h\tvoid-return\n",
            False,
            "4 tab-separated fields",
            (),
        ),
        (
            "unknown ledger class fails",
            "",
            "libs/ra8_core/inc/ra8_log.h\tmystery\tx\t1\n",
            False,
            "unknown class",
            (),
        ),
        (
            "duplicate ledger row fails",
            VOID + "\n",
            void_row + "\n" + void_row + "\n",
            False,
            "duplicates",
            (),
        ),
        (
            "zero count in the ledger fails",
            "",
            "libs/ra8_core/inc/ra8_log.h\tvoid-return\tx\t0\n",
            False,
            "non-positive",
            (),
        ),
    ]

    failures: list[str] = []
    for name, log, baseline, expect_ok, expect_text, tools in probes:
        ok, detail = _probe(name, log, baseline, expect_ok, expect_text, tools)
        if not ok:
            failures.append(detail)

    if failures:
        print("check_capi_doc_warnings: selftest FAILED")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(f"check_capi_doc_warnings: selftest OK ({len(probes)} probes)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__ and __doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail on any finding")
    parser.add_argument("--update", action="store_true", help="re-emit the ledger")
    parser.add_argument("--selftest", action="store_true", help="run synthetic fixtures")
    parser.add_argument("--emit-classes", action="store_true", help="list warning classes")
    parser.add_argument("--log", default=str(DEFAULT_LOG), help="Doxygen warning log")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE), help="ledger path")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if args.emit_classes:
        print(emit_classes())
        return 0

    log_path = Path(args.log)
    baseline_path = Path(args.baseline)

    if args.update:
        if not log_path.is_file():
            print(
                f"check_capi_doc_warnings: {log_path} not found. Run "
                "`bash scripts/builders/docs_capi.sh` first.",
                file=sys.stderr,
            )
            return 1
        observed, findings = parse_log(log_path.read_text())
        observed, _ = check_environment(observed, lambda _tool: False)
        if findings:
            print("check_capi_doc_warnings: cannot emit a ledger from this log")
            for finding in findings:
                print(f"  - {finding}")
            return 1
        baseline_path.write_text(render_baseline(observed))
        print(
            f"check_capi_doc_warnings: wrote {baseline_path} "
            f"({len(observed)} row(s), {sum(observed.values())} warning(s))"
        )
        return 0

    findings, observed = run_check(log_path, baseline_path)
    if findings:
        print("check_capi_doc_warnings: the C ABI reference's warning ledger is out of date")
        for finding in findings:
            print(f"  - {finding}")
        return 1

    print(
        "check_capi_doc_warnings: OK -- "
        f"{sum(observed.values())} ledgered warning(s) across {len(observed)} triple(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

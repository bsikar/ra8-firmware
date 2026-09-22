#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: register symbols in first-party code must exist in the HUM.

``cite_check.py`` asks "is this citation well-formed, and does it name a real
chapter and a page inside that chapter?". It cannot ask "does the cited text
describe the register being touched?", and it cannot ask "does this register
exist at all?". Three landed defects lived in that gap, every one of them
carrying a citation naming a REAL chapter and a REAL page:

* the ``ra8_rsip`` crypto family addressed registers HUM Ch 52 does not
  publish -- that chapter is six pages long and describes no registers at all;
* ``ra8_ptp_regs.h`` declared a thirteen-register window at ``0x403E_0100``,
  a reserved hole in the GPTP aperture. The demo printed ``gptp: clock PASS``
  because a reserved aperture echoed its own writes back (#498);
* ``ra8_etha_regs.h`` declared ``EASCR`` at ``0x0580``, a symbol absent from
  the whole of Ch 32, and ~26 ETHA citations pointed at real pages describing
  other registers (#539).

This gate closes that gap by connecting a register SYMBOL in our source to the
symbol table the manual publishes. Four rules, all reported per file:

``unknown-cite-symbol``
    A citation names symbol ``S`` in chapter ``C`` and ``S`` appears nowhere
    in chapter ``C``. This is the signal that actually worked by hand: a
    register name that is absent from the cited chapter is almost always
    invented. When ``S`` does exist in some other chapter the diagnostic says
    so, because a mis-attributed real register is a much smaller mistake.

``wrong-cite-page``
    A citation names a real symbol but points outside the pages that symbol's
    description occupies. A description routinely spills onto the following
    page, so the comparison is against a page RANGE (heading page up to the
    next section heading) rather than a single page -- a naive equality check
    would shout about hundreds of correct citations.

``unknown-window-symbol``
    A register-window struct member, or an offset enumerator, in any
    first-party header whose symbol appears in none of the chapters that
    header names. This is the rule that would have caught #498, whose
    citations named no symbol at all.

``wrong-window-offset``
    An offset enumerator whose value disagrees with the offset the manual
    prints for that register.

WHAT THIS DOES NOT CHECK
------------------------
Being precise about the edge is part of not crying wolf:

* **Bit fields.** Only register-level symbols are cross-checked. A driver
  writing the right register with the wrong field -- which is exactly what
  #539's third defect was, ``EATASGL0`` receiving a gate state instead of an
  entry address -- is invisible here and always will be. Nothing mechanical
  substitutes for reading the field table.
* **Citations that name no register.** ``/* HUM Ch 32.4 "Error Interrupt
  Sources" p 1685 */`` carries no symbol, so there is nothing to resolve;
  1665 of the tree's citations name a register and are checked, the rest are
  skipped rather than guessed at.
* **Headers that name no HUM chapter.** ``ra8_touch_gt911_regs.h`` describes
  an external I2C touch controller that is not in this manual at all, so it
  is unattributable by construction and is left alone.
* **Absolute-address enumerators, entirely.** The window scan recognises
  struct members and OFFSET enumerators (``k_ra8_etha_off_eatasgl0``). An
  enumerator holding a whole address instead -- ``k_ra8_wdt_ofs0_addr =
  0x03001E04UL`` -- matches neither shape and is not examined at all. Nor
  would the symbol rule help if it were: ``OFS0`` and ``OFS3`` are REAL
  registers (HUM Ch 7.2.1 p 280 and Ch 7.2.6 p 287), documented by absolute
  address with no offset, so this gate has nothing to compare. The defect in
  that case is that ``0x03001E04`` / ``0x03001E20`` fall inside a window both
  manuals mark ``Reserved area`` -- a wrong ADDRESS, not an invented NAME.
  That needs an address-versus-reserved-window guard, which is a different
  rule and is being added separately under #545. Do not read this gate as
  covering it.

WHAT MAKES THIS GATE ABLE TO FAIL
---------------------------------
Both acceptance properties the #190 audit distilled are designed in, because
they are precisely what a gate like this gets wrong:

* **No check compares a constant to itself.** The authority is always the
  committed manual PDF, re-parsed on every run. The committed
  ``HUM_REGISTERS.csv`` is a reviewable convenience and is byte-compared
  against that fresh parse, so a CSV edited to make a violation disappear
  fails the gate instead of hiding the defect.
* **Every scan has a vacuity floor.** A PDF extraction that silently yielded
  zero symbols would report every register in the tree clean, forever -- the
  single most likely failure mode here. ``hum_regmap.assert_not_vacuous``
  makes that a hard error, and this gate additionally floors the number of
  source claims it found: a scan that stops matching our own headers is just
  as dishonest as one that stops matching the manual.

RATCHET, NOT WARN-ONLY
----------------------
A full-tree sweep starts with a backlog these four rules did not create: they
made it visible. Switching rules off, or running warn-only, is the "gate that
silently does nothing" pattern this tree keeps finding, so instead the debt is
COUNTED per (file, rule) in ``.github/hum-register-baseline.txt`` -- the same
shape ``tidy_ratchet.py`` and ``misra_ratchet.py`` use. A new or increased
count FAILS. Shrinkage passes with a notice to re-baseline, which locks the
progress in. Closing the debt means the baseline reaching zero rows and being
deleted, so ``--update`` refuses to grow a bucket.

USAGE
    check_hum_register_map.py --selftest   # prove it fires AND stays quiet
    check_hum_register_map.py              # the gate
    check_hum_register_map.py --update     # re-baseline (shrink only)
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import Counter
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import hum_regmap
import hum_regmap_scan
from lint_targets import first_party_paths
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_FILE = REPO_ROOT / ".github" / "hum-register-baseline.txt"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

RULES = (
    "unknown-cite-symbol",
    "wrong-cite-page",
    "unknown-window-symbol",
    "wrong-window-offset",
)

# Vacuity floors on OUR side of the comparison. The manual's side is floored
# in hum_regmap. A scan that stopped recognising our citations or our register
# windows would report a clean tree because it looked at nothing.
MIN_FILES_SCANNED = 500
MIN_CITE_CLAIMS = 400
MIN_WINDOW_CLAIMS = 400

MAX_DETAIL_LINES = 25
"""Cap on offending findings echoed before the report truncates."""

BASELINE_COLUMNS = 3
"""Column count of one baseline row: file, rule, count."""

ARRAY_STRIDE_RE = re.compile(r"\+\s*(?P<stride>0[xX][0-9A-Fa-f]+)\s*\*")
"""The stride of an indexed register's offset, e.g. the 0x4 of "0x0040 + 0x4 * q"."""


@dataclass(frozen=True)
class Finding:
    """One rule violation, ready to print and to bucket for the ratchet."""

    path: str
    line: int
    rule: str
    detail: str


def _check_cite(claim: hum_regmap_scan.SymbolClaim, rmap: hum_regmap.RegisterMap) -> Finding | None:
    """Apply the two citation rules to one citation claim."""
    if claim.chapter is None or claim.page is None or claim.page_end is None:
        return None
    rows = rmap.lookup(claim.chapter, claim.symbol)
    if not rows:
        elsewhere = sorted({row.chapter for row in rmap.find_anywhere(claim.symbol)})
        where = f"but Ch {elsewhere} describes it" if elsewhere else "and no chapter describes it"
        return Finding(
            claim.path,
            claim.line,
            "unknown-cite-symbol",
            f"{claim.symbol} does not appear in HUM Ch {claim.chapter} -- {where}",
        )
    if any(row.covers(claim.page, claim.page_end) for row in rows):
        return None
    spans = ", ".join(f"p {row.page}-{row.page_end}" for row in rows)
    return Finding(
        claim.path,
        claim.line,
        "wrong-cite-page",
        f"{claim.symbol} cited at p {claim.page}; HUM describes it at {spans}",
    )


def _array_base_rows(
    symbol: str, chapters: set[int], rmap: hum_regmap.RegisterMap
) -> list[hum_regmap.HumRegister]:
    """Resolve ``EATMFSC0``-style names for the manual's ``EATMFSCq`` array.

    A header names the base element of an indexed register with an explicit
    ``0`` (``k_ra8_etha_off_eatmfsc0`` for what the manual calls
    ``EATMFSCq``). That is a real alias, not an invented register, so it is
    resolved rather than reported -- but only when the manual's own entry is
    genuinely INDEXED, i.e. its offset expression carries a stride. Without
    that condition the rule would also swallow ``EAEIS9``, and the point of
    trailing digits staying significant is that ``EAEIS0`` and ``EAEIS1`` are
    different registers.
    """
    base = symbol.rstrip("0123456789")
    if base == symbol or not base:
        return []
    rows = [row for chapter in sorted(chapters) for row in rmap.lookup(chapter, base)]
    return [row for row in rows if "*" in row.offset_expr]


def _expected_offsets(row: hum_regmap.HumRegister, symbol: str) -> list[int]:
    """Byte offsets the manual allows for a header symbol naming `row`.

    For a plain register that is just the printed offset. For an INDEXED
    register the header may name any element explicitly -- ``IPCSEM15`` for
    the manual's ``IPCSEMn`` at ``0x000 + 0x004 * n`` -- so the element index
    is read off the header's own symbol and the stride applied. Comparing
    element 15 against the array's base offset was reporting sixteen correct
    declarations as wrong.
    """
    if not row.offset:
        return []
    base = int(row.offset, 16)
    stride_match = ARRAY_STRIDE_RE.search(row.offset_expr)
    if stride_match is None:
        return [base]
    digits = symbol[len(symbol.rstrip("0123456789")) :]
    if not digits:
        return [base]
    return [base, base + int(digits) * int(stride_match.group("stride"), 16)]


def _check_window(
    claim: hum_regmap_scan.SymbolClaim, chapters: set[int], rmap: hum_regmap.RegisterMap
) -> Finding | None:
    """Apply the two register-window rules to one struct or offset claim."""
    rows = [row for chapter in sorted(chapters) for row in rmap.lookup(chapter, claim.symbol)]
    if not rows:
        rows = _array_base_rows(claim.symbol, chapters, rmap)
    if not rows:
        elsewhere = sorted({row.chapter for row in rmap.find_anywhere(claim.symbol)})
        where = f"but Ch {elsewhere} describes it" if elsewhere else "and no chapter describes it"
        return Finding(
            claim.path,
            claim.line,
            "unknown-window-symbol",
            f"{claim.symbol} does not appear in the chapters this file cites "
            f"({sorted(chapters)}) -- {where}",
        )
    if claim.offset is None:
        return None
    expected = sorted({addr for row in rows for addr in _expected_offsets(row, claim.symbol)})
    if not expected or claim.offset in expected:
        return None
    printed = ", ".join(f"0x{addr:04X}" for addr in expected)
    return Finding(
        claim.path,
        claim.line,
        "wrong-window-offset",
        f"{claim.symbol} declared at 0x{claim.offset:04X}; HUM puts it at {printed}",
    )


@dataclass
class ScanResult:
    """Everything one full-tree scan produced, findings and vacuity counters."""

    findings: list[Finding]
    files: int
    cite_claims: int
    window_claims: int


def scan_tree(rmap: hum_regmap.RegisterMap, paths: list[str] | None = None) -> ScanResult:
    """Apply every rule to every first-party C file."""
    targets = (
        paths
        if paths is not None
        else first_party_paths(SOURCE_SUFFIXES, respect_language_excludes=True)
    )
    result = ScanResult([], 0, 0, 0)
    for rel in targets:
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        result.files += 1
        cites = hum_regmap_scan.cite_claims(rel, text)
        result.cite_claims += len(cites)
        for claim in cites:
            finding = _check_cite(claim, rmap)
            if finding is not None:
                result.findings.append(finding)
        # Register windows are declared in headers. Scoping this to
        # "*_regs.h" looked tidy and missed ra8_rsip_regs_offsets.h, which
        # declares the whole RSIP offset map under a name the pattern does
        # not match; any header may declare a window, so any header is read.
        if not rel.endswith((".h", ".hpp")):
            continue
        windows = hum_regmap_scan.window_claims(rel, text)
        result.window_claims += len(windows)
        # Every chapter the file cites, NOT only the ones that publish
        # registers. Intersecting with rmap.chapters would skip a header whose
        # only cited chapter documents no registers at all -- which is exactly
        # ra8_rsip_regs.h, citing a six-page Ch 52 that publishes none. A file
        # with no citation whatsoever is genuinely unattributable (the GT911
        # touch controller is an external I2C part, not in this manual) and is
        # the one case that is skipped.
        chapters = hum_regmap_scan.cited_chapters(text)
        if not chapters:
            continue
        for claim in windows:
            finding = _check_window(claim, chapters, rmap)
            if finding is not None:
                result.findings.append(finding)
    return result


def assert_scan_not_vacuous(result: ScanResult) -> None:
    """Fail when the source-side scan found too little to have worked.

    Raises:
        hum_regmap.HumExtractionError: a floor was not met.
    """
    problems = []
    if result.files < MIN_FILES_SCANNED:
        problems.append(f"{result.files} files scanned < floor {MIN_FILES_SCANNED}")
    if result.cite_claims < MIN_CITE_CLAIMS:
        problems.append(f"{result.cite_claims} register citations < floor {MIN_CITE_CLAIMS}")
    if result.window_claims < MIN_WINDOW_CLAIMS:
        problems.append(f"{result.window_claims} window symbols < floor {MIN_WINDOW_CLAIMS}")
    if problems:
        raise hum_regmap.HumExtractionError(
            "source scan is vacuous -- every rule would pass because nothing was "
            "examined: " + "; ".join(problems)
        )


def bucket(findings: list[Finding]) -> Counter[tuple[str, str]]:
    """Reduce findings to the ``(file, rule) -> count`` the ratchet compares.

    Line numbers churn on every unrelated edit above them, so they are not
    part of the key: a count per file per rule is invariant under that and
    still trips the moment a file gains another violation.
    """
    return Counter((finding.path, finding.rule) for finding in findings)


def load_baseline() -> Counter[tuple[str, str]]:
    """Read the committed per-(file, rule) debt, or an empty tally."""
    counts: Counter[tuple[str, str]] = Counter()
    if not BASELINE_FILE.is_file():
        return counts
    for raw in BASELINE_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != BASELINE_COLUMNS:
            continue
        counts[(parts[0], parts[1])] = int(parts[2])
    return counts


def write_baseline(counts: Counter[tuple[str, str]]) -> None:
    """Write the ratchet file in a stable, diffable order."""
    lines = [
        "# HUM register-map debt, per (file, rule). Generated by",
        "# scripts/checks/check_hum_register_map.py --update.",
        "#",
        "# A NEW or INCREASED count fails the gate. Shrinkage passes with a notice",
        "# to re-baseline, which locks the progress in. Closing the debt means this",
        "# file reaching zero rows and being deleted -- --update refuses to grow a",
        "# bucket, so a genuine increase has to be a reviewable hand edit.",
    ]
    for (path, rule), count in sorted(counts.items()):
        if count:
            lines.append(f"{path}\t{rule}\t{count}")
    BASELINE_FILE.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def compare(actual: Counter[tuple[str, str]], baseline: Counter[tuple[str, str]]) -> list[str]:
    """Return one message per bucket that grew above its baseline."""
    regressions = []
    for key, count in sorted(actual.items()):
        allowed = baseline.get(key, 0)
        if count > allowed:
            path, rule = key
            regressions.append(f"{path}: {rule} {count} > baseline {allowed}")
    return regressions


def freshness_error(rows: list[hum_regmap.HumRegister]) -> str | None:
    """Describe how the committed CSV differs from a fresh parse of the PDF."""
    fresh = hum_regmap.to_csv(rows)
    if not hum_regmap.HUM_CSV.is_file():
        return f"{hum_regmap.HUM_CSV} is missing"
    committed = hum_regmap.HUM_CSV.read_text(encoding="utf-8")
    if committed == fresh:
        return None
    return (
        f"{hum_regmap.HUM_CSV.relative_to(REPO_ROOT)} does not match a fresh parse of "
        f"{hum_regmap.HUM_PDF.name} ({len(committed)} vs {len(fresh)} bytes); "
        "run scripts/gen/gen_hum_register_map.py"
    )


def _print_findings(findings: list[Finding]) -> None:
    """Echo up to ``MAX_DETAIL_LINES`` findings, worst rule first."""
    order = {rule: index for index, rule in enumerate(RULES)}
    ranked = sorted(findings, key=lambda f: (order.get(f.rule, 99), f.path, f.line))
    for finding in ranked[:MAX_DETAIL_LINES]:
        print(
            f"  {finding.path}:{finding.line}: [{finding.rule}] {finding.detail}", file=sys.stderr
        )
    if len(ranked) > MAX_DETAIL_LINES:
        print(f"  ... and {len(ranked) - MAX_DETAIL_LINES} more", file=sys.stderr)


def _verdict(actual: Counter[tuple[str, str]], findings: list[Finding]) -> int:
    """Compare against the committed baseline and print the gate's verdict."""
    baseline = load_baseline()
    regressions = compare(actual, baseline)
    if regressions:
        print(
            f"\nFAIL: {len(regressions)} (file, rule) bucket(s) above the committed baseline.",
            file=sys.stderr,
        )
        for message in regressions:
            print(f"  {message}", file=sys.stderr)
        grown = {key for key, count in actual.items() if count > baseline.get(key, 0)}
        print("\nOffending findings:", file=sys.stderr)
        _print_findings([f for f in findings if (f.path, f.rule) in grown])
        return 1

    shrunk = sum(1 for key, count in baseline.items() if actual.get(key, 0) < count)
    if shrunk:
        print(
            f"\nNOTICE: {shrunk} bucket(s) shrank. Re-baseline with "
            "`python3 scripts/checks/check_hum_register_map.py --update` to lock it in."
        )
    print("\nPASS: no register symbol regressed against the manual.")
    return 0


def run_gate(update: bool) -> int:
    """Regenerate the map, scan the tree, and apply the ratchet."""
    try:
        rows = hum_regmap.extract_from_pdf()
    except hum_regmap.HumExtractionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    stale = freshness_error(rows)
    if stale is not None:
        print(f"ERROR: {stale}", file=sys.stderr)
        return 1

    rmap = hum_regmap.RegisterMap(rows)
    result = scan_tree(rmap)
    try:
        assert_scan_not_vacuous(result)
    except hum_regmap.HumExtractionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    actual = bucket(result.findings)
    print(
        f"HUM register map: {len(rows)} registers over {len(rmap.chapters)} chapters; "
        f"scanned {result.files} files, {result.cite_claims} register citations, "
        f"{result.window_claims} window symbols"
    )
    for rule in RULES:
        total = sum(count for (_, name), count in actual.items() if name == rule)
        print(f"  {rule}: {total}")

    if update:
        baseline = load_baseline()
        # Seeding is the one time the file is allowed to appear from nothing:
        # before it exists there is no line to hold. Every later --update may
        # only shrink, which is what makes this a ratchet rather than a
        # rubber stamp.
        seeding = not BASELINE_FILE.is_file()
        grown = [] if seeding else [k for k, c in actual.items() if c > baseline.get(k, 0)]
        if seeding:
            print(f"seeding {BASELINE_FILE.relative_to(REPO_ROOT)} for the first time")
        if grown:
            print("ERROR: --update refuses to grow the baseline:", file=sys.stderr)
            for path, rule in sorted(grown):
                print(f"  {path}: {rule}", file=sys.stderr)
            return 1
        write_baseline(actual)
        print(f"re-baselined {BASELINE_FILE.relative_to(REPO_ROOT)}")
        return 0

    return _verdict(actual, result.findings)


def _selftest_map() -> hum_regmap.RegisterMap:
    """A tiny hand-built register map standing in for the manual."""
    return hum_regmap.RegisterMap(
        [
            hum_regmap.HumRegister(32, "3.1.1", "EAMC", "0x0000", "0x0000", 1630, 1631, "Mode Cfg"),
            hum_regmap.HumRegister(
                32, "3.2.6", "EATMFSCq", "0x0040", "0x0040 + 0x4 * q", 1635, 1636, "Max Frame"
            ),
        ]
        # Pad past the vacuity floor so the floor itself is not what is under
        # test here; the floor gets its own assertions below.
        + [
            hum_regmap.HumRegister(1, "1", f"REG{n:04d}", "0x0000", "0x0000", 100, 100, "pad")
            for n in range(hum_regmap.MIN_TOTAL_ROWS)
        ]
        + [
            hum_regmap.HumRegister(ch, "1", f"CH{ch}REG", "0x0000", "0x0000", 100, 100, "pad")
            for ch in range(2, hum_regmap.MIN_CHAPTERS_WITH_REGISTERS + 2)
        ]
    )


def _selftest_cite_rules(failures: list[str]) -> None:
    """Assert the two citation rules fire and stay quiet as documented."""
    rmap = _selftest_map()

    good = (
        '/* HUM Ch 32.3.1.1 "EAMC : Mode Cfg" p 1630 */\n'
        '/* HUM Ch 32.3.1.1 "EAMC : Mode Cfg" p 1631 */\n'
        '/* HUM Ch 32.3.2.6 "EATMFSCq : Max Frame" p 1635 */\n'
    )
    findings = [
        f for c in hum_regmap_scan.cite_claims("good.c", good) if (f := _check_cite(c, rmap))
    ]
    expect(not findings, "real symbol, real page, spilled page: no finding", failures)

    invented = '/* HUM Ch 32.3 "EASCR : Security Configuration" p 1697 */\n'
    findings = [
        f for c in hum_regmap_scan.cite_claims("bad.c", invented) if (f := _check_cite(c, rmap))
    ]
    expect(
        len(findings) == 1 and findings[0].rule == "unknown-cite-symbol",
        "invented cited symbol fires unknown-cite-symbol",
        failures,
    )

    misplaced = '/* HUM Ch 32.3.1.1 "EAMC : Mode Cfg" p 1690 */\n'
    findings = [
        f for c in hum_regmap_scan.cite_claims("bad.c", misplaced) if (f := _check_cite(c, rmap))
    ]
    expect(
        len(findings) == 1 and findings[0].rule == "wrong-cite-page",
        "real symbol on the wrong page fires wrong-cite-page",
        failures,
    )

    prose = '/* HUM Ch 32.4 "Error Interrupt Sources" p 1685 */\n'
    expect(
        not hum_regmap_scan.cite_claims("p.c", prose),
        "a citation naming no register yields no claim (no false positive)",
        failures,
    )


def _window_findings(source: str, rmap: hum_regmap.RegisterMap) -> list[Finding]:
    """Run the register-window rules over a snippet, for the selftest."""
    return [
        finding
        for claim in hum_regmap_scan.window_claims("regs.h", source)
        if (finding := _check_window(claim, {32}, rmap)) is not None
    ]


def _selftest_window_rules(failures: list[str]) -> None:
    """Assert the two register-window rules fire and stay quiet as documented."""
    rmap = _selftest_map()

    window_ok = (
        "  volatile uint32_t EAMC;\n"
        "  volatile uint32_t reserved08;\n"
        "  volatile uint32_t EATMFSC[8];\n"
        "  k_ra8_etha_off_eamc = 0x0000U,\n"
    )
    claims = hum_regmap_scan.window_claims("regs.h", window_ok)
    findings = _window_findings(window_ok, rmap)
    expect(not findings, "real window symbols and offsets: no finding", failures)
    expect(
        all(claim.symbol != "RESERVED08" for claim in claims),
        "reserved padding is not treated as a register",
        failures,
    )

    window_bad = "  volatile uint32_t EASCR;\n"
    findings = _window_findings(window_bad, rmap)
    expect(
        len(findings) == 1 and findings[0].rule == "unknown-window-symbol",
        "invented struct member fires unknown-window-symbol",
        failures,
    )


def _selftest_array_rules(failures: list[str]) -> None:
    """Assert indexed-register aliases resolve without weakening the rule."""
    rmap = _selftest_map()

    array_base = "  k_ra8_etha_off_eatmfsc0 = 0x0040U,\n"
    findings = _window_findings(array_base, rmap)
    expect(
        not findings,
        "EATMFSC0 resolves to the manual's indexed EATMFSCq (no false positive)",
        failures,
    )

    not_indexed = "  volatile uint32_t EAMC9;\n"
    findings = _window_findings(not_indexed, rmap)
    expect(
        len(findings) == 1 and findings[0].rule == "unknown-window-symbol",
        "a trailing digit on a NON-indexed register is still reported (EAMC9)",
        failures,
    )

    element = "  k_ra8_etha_off_eatmfsc3 = 0x004CU,\n"
    findings = _window_findings(element, rmap)
    expect(
        not findings,
        "an explicitly-named array element uses base + index * stride (EATMFSC3)",
        failures,
    )

    element_bad = "  k_ra8_etha_off_eatmfsc3 = 0x0050U,\n"
    findings = _window_findings(element_bad, rmap)
    expect(
        len(findings) == 1 and findings[0].rule == "wrong-window-offset",
        "an array element at the wrong stride multiple still fires",
        failures,
    )

    offset_bad = "  k_ra8_etha_off_eamc = 0x0580U,\n"
    findings = _window_findings(offset_bad, rmap)
    expect(
        len(findings) == 1 and findings[0].rule == "wrong-window-offset",
        "offset disagreeing with the manual fires wrong-window-offset",
        failures,
    )


def _selftest_guards(failures: list[str]) -> None:
    """Assert the vacuity floors and the ratchet direction both hold."""
    empty = []
    try:
        hum_regmap.assert_not_vacuous(empty)
        fired = False
    except hum_regmap.HumExtractionError:
        fired = True
    expect(fired, "an empty manual extraction is rejected as vacuous", failures)

    try:
        hum_regmap.assert_not_vacuous(_selftest_map().rows)
        quiet = True
    except hum_regmap.HumExtractionError:
        quiet = False
    expect(quiet, "a full manual extraction passes the vacuity floor", failures)

    try:
        assert_scan_not_vacuous(ScanResult([], 0, 0, 0))
        fired = False
    except hum_regmap.HumExtractionError:
        fired = True
    expect(fired, "a source scan that examined nothing is rejected as vacuous", failures)

    base = Counter({("a.c", "unknown-cite-symbol"): 2})
    expect(
        not compare(Counter({("a.c", "unknown-cite-symbol"): 2}), base),
        "a bucket at its baseline passes",
        failures,
    )
    expect(
        not compare(Counter({("a.c", "unknown-cite-symbol"): 1}), base),
        "a bucket below its baseline passes",
        failures,
    )
    expect(
        len(compare(Counter({("a.c", "unknown-cite-symbol"): 3}), base)) == 1,
        "a bucket above its baseline fails",
        failures,
    )
    expect(
        len(compare(Counter({("b.c", "unknown-cite-symbol"): 1}), base)) == 1,
        "a file absent from the baseline fails on its first finding",
        failures,
    )

    expect(
        hum_regmap.canonical_symbol("EATMFSCq") == hum_regmap.canonical_symbol("EATMFSC"),
        "array index letters normalise away (EATMFSCq == EATMFSC)",
        failures,
    )
    expect(
        hum_regmap.canonical_symbol("EAEIS0") != hum_regmap.canonical_symbol("EAEIS1"),
        "trailing digits stay significant (EAEIS0 != EAEIS1)",
        failures,
    )


def selftest() -> int:
    """Prove the detector fires on broken input and stays quiet on good input."""
    print("check_hum_register_map selftest:")
    failures: list[str] = []
    _selftest_cite_rules(failures)
    _selftest_window_rules(failures)
    _selftest_array_rules(failures)
    _selftest_guards(failures)
    return report(failures)


def main(argv: list[str] | None = None) -> int:
    """Entry point."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="assert both directions and exit")
    parser.add_argument("--update", action="store_true", help="re-baseline (shrink only)")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    return run_gate(args.update)


if __name__ == "__main__":
    sys.exit(main())

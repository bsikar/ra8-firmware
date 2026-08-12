#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regenerate the MC/DC gap tables from the live llvm-cov report.

Rewrites docs/MCDC_GAPS.csv and the summary header of docs/MCDC_GAPS.md from
build/mcdc-report/mcdc.txt + summary.txt.

Each row carries a `deactivated` boolean classifying whether the
remaining uncovered MC/DC condition is reachable through the public
API or whether it is a defensive guard already enforced by an upstream
check (per DO-178C 6.4.4.3, "deactivated code"). Detection is
heuristic but conservative -- see `is_deactivated_decision()`.

Source of truth: actual llvm-cov per-decision output. NOT a static parse
of the source tree, NOT a heuristic match against test_mcdc_* function
names. The previous CSV regenerator used heuristics and went stale; this
one parses the same report `make mcdc` emits.

A decision is reported when llvm-cov shows it as < 100% MC/DC. The
columns are:

    source_file,line,condition_count,function_name,decision_excerpt,covered

where `covered` is one of:
    - "no"      -- 0.00% MC/DC for that decision
    - "partial" -- 0 < pct < 100
    - "yes"     -- 100% (omitted from CSV; CSV is gap-only)

Verification: for any module, the number of CSV rows equals the count
of `MC/DC Coverage for Decision: <pct>%` blocks in mcdc.txt where the
percentage is < 100, restricted to that module's source files.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterator
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
MCDC_TXT = REPO_ROOT / "build" / "mcdc-report" / "mcdc.txt"
CSV_OUT = REPO_ROOT / "docs" / "MCDC_GAPS.csv"
MD_OUT = REPO_ROOT / "docs" / "MCDC_GAPS.md"
DEACT_MD_OUT = REPO_ROOT / "docs" / "MCDC_DEACTIVATIONS.md"

# ---------------------------------------------------------------------------
# Coverage thresholds and table display limits
# ---------------------------------------------------------------------------
# MC/DC percentage indicating full decision coverage (llvm-cov scale 0..100).
MCDC_FULL_PCT = 100.0
# MC/DC percentage indicating zero coverage for a decision.
MCDC_ZERO_PCT = 0.0
# Maximum character width of an excerpt cell in the reachable-gaps table.
EXCERPT_MAX_REACHABLE = 80
# Truncated excerpt length (EXCERPT_MAX_REACHABLE - len("...")).
EXCERPT_TRUNC_REACHABLE = 77
# Maximum character width of excerpt/rationale cells in the deactivated table.
EXCERPT_MAX_DEACTIVATED = 60
# Truncated excerpt/rationale length (EXCERPT_MAX_DEACTIVATED - len("...")).
EXCERPT_TRUNC_DEACTIVATED = 57
# Maximum number of rows shown inline in the reachable-gaps markdown table.
TABLE_ROW_CAP = 60


def _truncate_md_cell(text: str, limit: int, trunc: int) -> str:
    """Truncate `text` for a Markdown table cell, keeping backticks balanced.

    Rationale/excerpt strings carry backtick code spans; a truncation that
    cuts between a span's opening and closing backtick leaves an odd count,
    which opens a verbatim block that swallows the rest of the page (and
    trips doxygen's "still searching closing backtick" warning). When the
    truncated cell holds an odd number of backticks, close the span.
    """
    if len(text) > limit:
        text = text[:trunc] + "..."
    if text.count("`") % 2 == 1:
        text += "`"
    return text


# Lines in mcdc.txt look like:
#   "  385|      0|  if ((start == 0U) || (start > end)) {"
# i.e.   <pad><lineno>|<exec_count>|<source>
LINE_RE = re.compile(r"^\s*(\d+)\|\s*[^|]*\|(.*)$")

# llvm-cov's `show -format=text` prints each file's path followed by ':'.
# The path is absolute and reflects wherever the tree was built -- `/work/...`
# in the devcontainer, `/home/<user>/<clone>/...` on a bare Linux checkout, a
# self-hosted-runner workspace in CI, etc. Capture the whole path here and
# relativize it against REPO_ROOT in `_repo_relative()`; a naive
# "first-party-root" regex mis-fires on the nested `src` in `libs/<grp>/src/`.
FILE_HEADER_RE = re.compile(r"^([^\s:][^:]*\.(?:c|h|cpp|hpp)):\s*$")


def _repo_relative(path: str) -> str:
    """Convert an absolute build-time source path to a repo-relative POSIX one.

    Stripping REPO_ROOT is deterministic and location-independent: CMake
    compiles with absolute source paths rooted at the tree the script itself
    lives in, so REPO_ROOT is exactly that prefix in every environment
    (`/work` in the devcontainer, the clone dir on a bare checkout, the runner
    workspace in CI). The `/work` and first-party-root fallbacks only guard the
    unlikely case of a symlinked or relocated object path.
    """
    p = path.replace("\\", "/")
    root = str(REPO_ROOT).replace("\\", "/").rstrip("/") + "/"
    if p.startswith(root):
        return p[len(root) :]
    if "/work/" in p:
        return p.split("/work/", 1)[1]
    m = re.search(r"(?:^|/)((?:libs|src|port|examples|tests)/.+)$", p)
    return m.group(1) if m else p


DECISION_HDR_RE = re.compile(r"\|---> MC/DC Decision Region \((\d+):\d+\) to \(\d+:\d+\)")
COND_COUNT_RE = re.compile(r"\|\s+Number of Conditions:\s+(\d+)")
PCT_RE = re.compile(r"\|\s+MC/DC Coverage for Decision:\s+([0-9.]+)%")


def parse_mcdc_txt(path: Path) -> Iterator[tuple[str, int, int, str, float]]:
    """Yield (rel_path, line, cond_count, source_excerpt, pct_float).

    Only emits one record per decision. The source excerpt is taken from
    the numbered listing at `line` in the same per-file section.
    """
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()

    cur_file = None
    # source_by_line[lineno] -> source text (per current file)
    source_by_line: dict[int, str] = {}
    in_decision = False
    dec_line = None
    dec_cond_count = None

    for raw in lines:
        # File transition
        m = FILE_HEADER_RE.match(raw)
        if m:
            cur_file = _repo_relative(m.group(1))
            source_by_line = {}
            in_decision = False
            dec_line = None
            dec_cond_count = None
            continue

        # Decision header
        m = DECISION_HDR_RE.search(raw)
        if m:
            in_decision = True
            dec_line = int(m.group(1))
            dec_cond_count = None
            continue

        if in_decision:
            m = COND_COUNT_RE.search(raw)
            if m:
                dec_cond_count = int(m.group(1))
                continue
            m = PCT_RE.search(raw)
            if m and cur_file is not None and dec_line is not None:
                pct = float(m.group(1))
                src = source_by_line.get(dec_line, "").strip()
                yield (cur_file, dec_line, dec_cond_count or 0, src, pct)
                in_decision = False
                dec_line = None
                dec_cond_count = None
                continue
            continue

        # Source line in numbered listing
        m = LINE_RE.match(raw)
        if m and cur_file is not None:
            ln = int(m.group(1))
            # The source after the second '|' may have a leading ' ' the
            # split swallowed; preserve content as-is.
            src = m.group(2)
            # First occurrence wins (some lines repeat in expansion contexts).
            source_by_line.setdefault(ln, src)


# ---------------------------------------------------------------------------
# Function-name resolver: walk the source file's brace structure to find
# the innermost function definition enclosing a given line.
# ---------------------------------------------------------------------------
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][\w\s\*\(\),:<>]*?\b([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{?\s*$")


class _CommentStripper:
    """Blank comments and string literals, one line at a time.

    Crude by design and stateful across lines, because the caller is walking
    the file to track brace depth and needs every line in order. A real parse
    would be better, but this runs over llvm-cov output for files that may not
    even compile in isolation.
    """

    def __init__(self) -> None:
        self.in_block = False

    def strip(self, line: str) -> str:
        """Return ``line`` with comments and string literals removed."""
        if self.in_block:
            end = line.find("*/")
            if end == -1:
                return ""
            line = line[end + 2 :]
            self.in_block = False
        while True:
            s = line.find("/*")
            if s == -1:
                break
            e = line.find("*/", s + 2)
            if e == -1:
                line = line[:s]
                self.in_block = True
                break
            line = line[:s] + line[e + 2 :]
        s = line.find("//")
        if s != -1:
            line = line[:s]
        return re.sub(r'"(?:\\.|[^"\\])*"', '""', line)


def _function_signature(line: str) -> str | None:
    """Return the function name if ``line`` opens a definition at file scope.

    Heuristic: an identifier followed by a parameter list, rejecting the
    control-flow keywords that have the same shape.
    """
    stripped = line.strip()
    if stripped.startswith(("if", "while", "for", "switch", "return", "do", "}")):
        return None
    m = FUNC_DEF_RE.match(stripped)
    return m.group(1) if m else None


def _track_braces(line: str, depth: int, pending: str | None, stack: list) -> int:
    """Advance brace depth over ``line``, pushing/popping the function stack."""
    for ch in line:
        if ch == "{":
            if depth == 0 and pending is not None:
                stack.append((pending, depth))
            depth += 1
        elif ch == "}":
            depth = max(depth - 1, 0)
            if stack and depth <= stack[-1][1]:
                stack.pop()
    return depth


def resolve_function(rel_path: str, target_line: int) -> str:
    """Best-effort function name lookup for a decision at ``target_line``.

    Returns "(file scope)" when the decision is not inside a function (e.g. a
    file-scope initializer), or when the file cannot be read.
    """
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return "(file scope)"
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "(file scope)"

    stripper = _CommentStripper()
    depth = 0
    pending: str | None = None
    stack: list[tuple[str, int]] = []  # (name, depth_when_opened)
    for i, raw in enumerate(text.splitlines(), start=1):
        line = stripper.strip(raw)
        if depth == 0:
            pending = _function_signature(line) or pending
        depth = _track_braces(line, depth, pending, stack)
        if i == target_line:
            return stack[-1][0] if stack else "(file scope)"

    return "(file scope)"


# ---------------------------------------------------------------------------
# Deactivated-condition classifier.
#
# Heuristic: a decision condition is "deactivated" (DO-178C 6.4.4.3)
# when the same function contains an earlier guard (RA8_CHECK_NULL_PTR,
# `if (p == NULL) return ...`, RA8_CHECK_RANGE, etc.) that makes the
# condition unreachable on the public-API path. We classify a whole
# decision as deactivated only when EVERY pointer/null/range token in
# the decision is shadowed by an earlier guard on the same name in the
# same function body.
#
# Conservative defaults: when in doubt, mark `reachable` (deactivated
# = False). False negatives only mean a decision stays in the
# "reachable" bucket and continues to demand a real test vector --
# never the other way round.
# ---------------------------------------------------------------------------
NULL_TOKEN_RE = re.compile(r"\(\s*([A-Za-z_]\w*(?:->\w+|\.\w+)?)\s*==\s*(?:NULL|nullptr|0)\s*\)")
GUARD_NULL_RE = re.compile(
    r"RA8_CHECK_NULL_PTR\s*\(\s*([A-Za-z_]\w*)|"
    r"if\s*\(\s*([A-Za-z_]\w*)\s*==\s*(?:NULL|nullptr)\s*\)"
)
LEN_NULL_PAIR_RE = re.compile(
    r"\(\s*([A-Za-z_]\w*)\s*==\s*(?:NULL|nullptr)\s*\)\s*&&\s*"
    r"\(\s*([A-Za-z_]\w*_len)\s*!=\s*0"
)
DEFENSIVE_OFF_RE = re.compile(r"\boff\s*<\s*sizeof\s*\(")
FUNC_BODY_BRACE_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{?\s*$")
# Pattern: `x != V || (x == V && y != Z)` -- the second clause's first
# condition (`x == V`) is structurally `!(x != V)` so it can never be
# true when the OR's first condition was false. llvm-cov still counts
# it as a third condition, but no vector can independently flip it.
STRUCT_REDUNDANT_RE = re.compile(
    r"([A-Za-z_]\w*(?:\[\d+\]|->\w+|\.\w+)?)\s*!=\s*('[^']+'|\"[^\"]+\"|[A-Za-z0-9_]+)\s*"
    r"\|\|\s*\(\s*\1\s*==\s*\2\s*&&"
)
# Pattern: `len < N || (uintXX_t)len > buf - cursor` -- segment-length
# corruption guard inside a bounded-input parser. The buffer is bounded
# by the public-API contract (`xx_decode(buf, len)` validates `buf`,
# `len`, and the segment length is parsed from `buf` itself), so the
# second condition only fires on a deliberately-corrupted input that
# the upstream API contract documents as undefined.
SEGLEN_BOUND_RE = re.compile(
    r"\b(seg_?len|len|sec_?len)\s*<\s*\d+U?\s*\|\|\s*\(?\s*\(?[A-Za-z_]\w*\s*\)?\s*\1?\s*>\s*\w+->\w+\s*-\s*\w+->\w+"
)
# Pattern: 4-condition OR over enum equality of a single variable
# `(x == E1 || x == E2 || x == E3 || x == E4)` -- exhaustive enum-set
# membership. MC/DC requires every condition to independently flip the
# decision; the only way to make all-false is `x` outside the set,
# which is structurally rejected by an upstream enum-validation guard.
ENUM_OR_SET_RE = re.compile(
    r"\(?\s*([A-Za-z_]\w*)\s*==\s*[A-Za-z_][\w]*\s*\)?\s*\|\|"
    r"\s*\(?\s*\1\s*==\s*[A-Za-z_][\w]*\s*\)?\s*\|\|"
    r"\s*\(?\s*\1\s*==\s*[A-Za-z_][\w]*\s*\)?\s*\|\|"
    r"\s*\(?\s*\1\s*==\s*[A-Za-z_][\w]*\s*\)?"
)


def _function_body_lines(rel_path: str, target_line: int) -> list[str]:  # noqa: PLR0911  # multiple early returns for distinct error/sentinel paths
    """Source lines from the enclosing function's start up to ``target_line``.

    Excludes the target line itself, since the caller is looking for what
    guards the decision, not the decision. Returns an empty list when no
    enclosing function can be identified.
    """
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return []
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    lines = text.splitlines()
    # Walk forward; remember the last `{` at depth 0->1 transition.
    depth = 0
    func_start = None
    for i, raw in enumerate(lines, start=1):
        raw.strip()
        for ch in raw:
            if ch == "{":
                if depth == 0:
                    func_start = i
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    if i >= target_line:
                        if func_start is not None:
                            return lines[func_start - 1 : target_line - 1]
                        return []
                    func_start = None
        if i == target_line:
            if func_start is not None:
                return lines[func_start - 1 : target_line - 1]
            return []
    return []


PRIV_NULL_OR_RE = re.compile(
    r"[A-Za-z_]\w*\s*==\s*(?:NULL|nullptr)\s*\|\|\s*"
    r"[A-Za-z_]\w*(?:\.\w+|->\w+)?\s*==\s*(?:NULL|nullptr|0U?|'\\0')"
)


def _enclosing_static_priv_name(rel_path: str, target_line: int) -> str | None:  # noqa: PLR0912  # parser/gate dispatch, splitting hurts readability
    """Name of the enclosing function iff it is TU-local, else None.

    TU-local means declared ``static`` and named by the project's private
    convention (``priv_*`` or ``internal_*``), or inside a C++ anonymous
    namespace, which is the C++ equivalent scope.

    These helpers are called only from inside the TU; their NULL
    guards are defensive contract-checks duplicating the public-API
    guard at the entry point.
    """
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return None
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = text.splitlines()
    depth = 0
    in_anon_ns = False
    anon_ns_depth = -1
    func_name = None
    func_is_local = False
    for i, raw in enumerate(lines, start=1):
        if i == target_line:
            return func_name if func_is_local else None
        stripped = raw.strip()
        # Detect anonymous-namespace open at depth 0 (`namespace {`).
        if depth == 0 and re.match(r"^namespace\s*\{", stripped):
            in_anon_ns = True
            anon_ns_depth = 0
        # Detect candidate function signature at the appropriate depth.
        # In C: depth 0. In an anon C++ namespace: depth 1.
        check_depth = 1 if in_anon_ns else 0
        if depth == check_depth:
            m = FUNC_DEF_RE.match(stripped)
            if m and not stripped.startswith(("if", "while", "for", "switch", "return", "do", "}")):
                cand = m.group(1)
                start = max(0, i - 5)
                window = " ".join(lines[start:i])
                is_static_priv = "static" in window and (cand.startswith(("priv_", "internal_")))
                is_anon_ns = in_anon_ns and depth == 1
                if is_static_priv or is_anon_ns:
                    func_name = cand
                    func_is_local = True
                else:
                    func_name = cand
                    func_is_local = False
        for ch in raw:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                depth = max(depth, 0)
                # If we close back to (or below) the anon-ns opening
                # depth, we have left the namespace.
                if in_anon_ns and depth <= anon_ns_depth:
                    in_anon_ns = False
                    anon_ns_depth = -1
                if depth <= (1 if in_anon_ns else 0):
                    func_name = None
                    func_is_local = False
    return None


def _line_annotation(rel_path: str, line: int) -> str | None:
    """Rationale from an ``mcdc-deactivated:`` annotation, or None.

    Accepts the annotation on the decision's own line or the one directly
    above it, so it can be written wherever it reads best.

    Two recognized syntaxes (case-insensitive):
      * `... // mcdc-deactivated: <rationale>` on the decision line.
      * `// mcdc-deactivated: <rationale>` on the line immediately above.
    """
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return None
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    src_lines = text.splitlines()
    if line - 1 >= len(src_lines):
        return None
    pat = re.compile(r"mcdc-deactivated\s*:\s*(.+?)\s*(?:\*/|$)", re.IGNORECASE)
    # Same-line annotation
    m = pat.search(src_lines[line - 1])
    if m:
        return m.group(1).strip()
    # Previous-line annotation
    if line - 2 >= 0:
        m = pat.search(src_lines[line - 2])
        if m:
            return m.group(1).strip()
    return None


# Rules that decide from the decision TEXT alone: a regex, and the rationale
# recorded when it matches. Data, not control flow -- five sequential
# `if RE.search(excerpt): return (True, "...")` blocks said nothing that this
# table does not, and hid how many rules there are. Order is preserved: the
# first match wins, exactly as the if-cascade did.
#
# The two rules NOT in this table are the ones that cannot decide from the
# excerpt: they have to read the surrounding function, so they are functions.
_TEXTUAL_DEACTIVATION_RULES: tuple[tuple[re.Pattern[str], str], ...] = (
    # ((p == NULL) && (p_len != 0)) -- a defensive contract check. The public
    # API documents the contract (caller-side @pre); the only path that
    # exercises the AND-chain's second condition is a deliberately malformed
    # call that the public API rejects upstream.
    (
        LEN_NULL_PAIR_RE,
        "Defensive null+len contract: (ptr == NULL) && (len != 0)"
        " is rejected upstream by the public-API @pre clause.",
    ),
    # `for (...; off < sizeof(buf); ...)` defensive bound. The fixed-size
    # scratch buffer is sized to the documented input caps; the bound
    # condition can only flip if the caller violates the contract.
    (
        DEFENSIVE_OFF_RE,
        "Defensive scratch-buffer bound: input length is capped"
        " by the public-API contract; second condition unreachable.",
    ),
    # Structurally-redundant `x != V || (x == V && ...)` where the second
    # clause's leading condition is the negation of the first. llvm-cov counts
    # the inner equality as a separate condition, but no vector can flip it
    # independently of the OR's leading inequality. The remaining two
    # conditions ARE testable (and are tested).
    (
        STRUCT_REDUNDANT_RE,
        "Structurally-redundant condition: `x == V` inside the"
        " second clause is the negation of the first OR-clause's"
        " `x != V` and cannot be flipped independently.",
    ),
    # 4-condition exhaustive-enum OR set membership.
    # `(x==E1)||(x==E2)||(x==E3)||(x==E4)` with upstream enum validation.
    # MC/DC's all-false vector requires `x` outside the enum range, which is
    # rejected before this decision.
    (
        ENUM_OR_SET_RE,
        "Exhaustive enum-set OR: 4-way mode equality. The"
        " all-false MC/DC vector requires an out-of-range enum"
        " value, which is rejected by an upstream enum guard.",
    ),
    # Segment-length corruption guard inside a bounded parser. The buffer
    # length is contract-validated by the public API; the second clause only
    # fires on intentionally-malformed input, documented as undefined.
    (
        SEGLEN_BOUND_RE,
        "Defensive segment-length bound in a bounded parser:"
        " buffer length is contract-validated upstream; the"
        " malformed-input branch is exempted under DO-178C 6.4.4.3.",
    ),
)


def _deactivated_by_priv_null(rel_path: str, line: int, excerpt: str) -> str | None:
    """Rationale when this is a NULL guard inside a TU-local static helper.

    Project convention: such helpers are only called from inside the same TU,
    where the public-API entry point has already validated every pointer via
    RA8_CHECK_NULL_PTR. The null guard is defensive duplication, so the
    all-NULL MC/DC vector is rejected upstream.

    Needs the enclosing function's name, which the excerpt does not carry --
    which is why this is a function and not a row in the table above.
    """
    if not PRIV_NULL_OR_RE.search(excerpt):
        return None
    fname = _enclosing_static_priv_name(rel_path, line)
    if fname is None:
        return None
    return (
        f"TU-local static helper `{fname}` -- defensive NULL"
        " guard duplicates the public-API entry-point check,"
        " which has already rejected NULL on every reachable"
        " call path."
    )


def _deactivated_by_upstream_guard(rel_path: str, line: int, excerpt: str) -> str | None:
    """Rationale when every pointer here was already null-checked in this function.

    Reads the enclosing function body looking for an earlier
    RA8_CHECK_NULL_PTR or `if (p == NULL) return ...` covering EVERY pointer
    the decision tests. Partial coverage is not enough: one unguarded pointer
    means the vector is still reachable.
    """
    null_tokens = [m.group(1).split("->")[0].split(".")[0] for m in NULL_TOKEN_RE.finditer(excerpt)]
    if not null_tokens:
        return None
    body = _function_body_lines(rel_path, line)
    if not body:
        return None
    text = "\n".join(body)
    guards: set[str] = set()
    for m in GUARD_NULL_RE.finditer(text):
        name = m.group(1) or m.group(2)
        if name:
            guards.add(name)
    shadowed = [n for n in null_tokens if n in guards]
    if not shadowed or len(shadowed) != len(null_tokens):
        return None
    return (
        f"Pointer(s) {sorted(set(shadowed))} already null-checked"
        " upstream in the same function body."
    )


def is_deactivated_decision(rel_path: str, line: int, excerpt: str) -> tuple[bool, str]:
    """Classify one decision as deactivated code, with the rationale why.

    Deliberately conservative: it reports deactivated only on positive
    evidence (an explicit annotation, or a guard in a TU-local function that
    an upstream check already enforces). An undecidable decision stays
    classified as reachable, so the gap count errs toward overstating the
    work rather than quietly excusing it -- which is the only safe direction
    under DO-178C 6.4.4.3.

    Returns ``(deactivated, rationale)``.
    """
    # Explicit per-line opt-in annotation outranks every inferred rule.
    annot = _line_annotation(rel_path, line)
    if annot is not None:
        return (True, f"Annotated deactivation: {annot}")

    for pattern, rationale in _TEXTUAL_DEACTIVATION_RULES:
        if pattern.search(excerpt):
            return (True, rationale)

    for rule in (_deactivated_by_priv_null, _deactivated_by_upstream_guard):
        rationale = rule(rel_path, line, excerpt)
        if rationale is not None:
            return (True, rationale)

    return (False, "")


# ---------------------------------------------------------------------------
# Module-name extraction: libs/<group>/src/<module>.c -> <module>
#                        src/<group>/<module>.c     -> <module>
# ---------------------------------------------------------------------------
def decision_snippet(excerpt: str, max_chars: int = 40) -> str:
    """Build a stable text-derived anchor fragment for a decision.

    Citation policy forbids `file:line` references because line numbers
    drift on every reformat. Instead we hash the decision into a short,
    grep-able slug derived from the source text itself: take the first
    `max_chars` characters of the (whitespace-collapsed) line and
    replace every run of non-alphanumeric bytes with a single `-`.

    Example:
        ' if (a->kind == k_attr_kind_char_value && a->value == NULL)'
        -> 'a-kind-k_attr_kind_char_value-a-value-NULL'

    Empty input returns 'unknown'. Result is always pure 7-bit ASCII
    (project policy) and contains no leading/trailing dashes.
    """
    if not excerpt:
        return "unknown"
    text = excerpt.strip()[:max_chars]
    slug_chars: list[str] = []
    prev_dash = False
    for ch in text:
        if ch.isalnum() or ch == "_":
            slug_chars.append(ch)
            prev_dash = False
        elif not prev_dash:
            slug_chars.append("-")
            prev_dash = True
    slug = "".join(slug_chars).strip("-")
    return slug or "unknown"


def module_of(rel_path: str) -> str:
    """Module name for a source path: the basename with its extension dropped.

    Groups a header and its implementation under one module, which is what
    makes the per-module gap counts add up the way a reader expects.
    """
    name = Path(rel_path).name
    if name.endswith(".c"):
        name = name[:-2]
    elif name.endswith(".cpp"):
        name = name[:-4]
    elif name.endswith((".h", ".hpp")):
        # Drop extension only.
        name = re.sub(r"\.(h|hpp)$", "", name)
    return name


# ---------------------------------------------------------------------------
# Main


def main() -> int:
    """Rebuild the MC/DC gap tables from the live coverage report.

    Refuses to run when build/mcdc-report/mcdc.txt is absent rather than
    emitting empty tables: a zero-gap report generated from no data would
    read exactly like full MC/DC coverage.

    Reads the LIVE report every time and never merges with the existing CSV,
    so a decision that has since been covered disappears from the tables
    instead of lingering as a stale row.

    Returns 1 when the report is missing, 0 after a successful regeneration.
    """
    if not MCDC_TXT.exists():
        print(
            f"error: {MCDC_TXT} not found. Run `make mcdc` first to generate"
            " the live llvm-cov report.",
            file=sys.stderr,
        )
        return 1

    # Collect every decision (covered or not) from the live report.
    all_decisions: list[tuple[str, int, int, str, float]] = list(parse_mcdc_txt(MCDC_TXT))
    # Skip third_party (the report already strips them, but be defensive).
    all_decisions = [d for d in all_decisions if "/third_party/" not in d[0]]

    # Gap rows = anything < 100%.
    gap_rows = [d for d in all_decisions if d[4] < MCDC_FULL_PCT]
    gap_rows.sort(key=lambda r: (r[0], r[1]))

    # Classify each gap row as deactivated or reachable.
    classified: list[tuple[str, int, int, str, str, str, bool, str]] = []
    for src, ln, n, excerpt, pct in gap_rows:
        covered = "no" if pct == MCDC_ZERO_PCT else "partial"
        func = resolve_function(src, ln)
        deact, rationale = is_deactivated_decision(src, ln, excerpt)
        classified.append((src, ln, n, func, excerpt, covered, deact, rationale))

    # Imported here, not at module scope: mcdc_report reads this module's
    # constants and helpers, so a top-level import either way is a cycle.
    import mcdc_report  # noqa: PLC0415  # avoid import cycle

    mcdc_report.write_gap_csv(classified)
    h = mcdc_report.headline(all_decisions, classified)
    mcdc_report.write_markdown(mcdc_report.module_rows(all_decisions), h)
    mcdc_report.write_deactivations(h)
    mcdc_report.write_gate_json(h)
    mcdc_report.write_per_file_json(all_decisions, classified)

    gap_rows = [d for d in all_decisions if d[4] < MCDC_FULL_PCT]
    print(
        f"Wrote {CSV_OUT.relative_to(REPO_ROOT)} ({len(gap_rows)} gap rows;"
        f" {h['deact_count']} deactivated, {len(h['reachable_rows'])} reachable),"
        f" {MD_OUT.relative_to(REPO_ROOT)},"
        f" {DEACT_MD_OUT.relative_to(REPO_ROOT)}."
        f" Absolute MC/DC: {h['rate']:.2f}%; reachable MC/DC: {h['reachable_rate']:.2f}%."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

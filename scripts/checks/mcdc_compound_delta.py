# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural ancestry model for the new-compound-decision gate.

The public checker owns repository scope, git/index reads, citations, and
reporting.  This companion owns only the source-level comparison that decides
whether a compound decision is pre-existing or newly introduced.  Keeping the
mechanism isolated makes the distinction reviewable and keeps both scripts
within the repository's per-file size limit.
"""

from __future__ import annotations

import re
from collections import Counter
from collections.abc import Callable
from difflib import SequenceMatcher

# Same-symbol fingerprints with the same logical-operator topology above this
# ratio are edited ancestry (casts, redundant parentheses, and equivalent
# bound spelling), not a new compound decision.
DECISION_ANCESTRY_SIMILARITY = 0.55

# Bucket for a decision whose enclosing function cannot be resolved.
NO_ENCLOSING_FUNCTION = "(file-scope)"

# Logical operators are matched outside lexical noise by lexical_code_view().
COMPOUND_OP_RE = re.compile(r"(?:\|\||&&)")

# Tokens retained in the structural fingerprint. Identifiers are alpha-
# normalized; language keywords and operators keep their spelling.
DECISION_TOKEN_RE = re.compile(
    r"[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?|"
    r"&&|\|\||==|!=|<=|>=|<<|>>|->|\+\+|--|[{}()\[\],;?:.~!%^&*+/|<>=-]"
)
DECISION_KEYWORDS: frozenset[str] = frozenset(
    {
        "if",
        "else",
        "while",
        "for",
        "return",
        "true",
        "false",
        "nullptr",
        "sizeof",
        "alignof",
    }
)

# Full-source lexical noise removal, applied before any logical operator is
# looked for. The alternation is ordered so a comment delimiter inside a
# literal stays data and a quote inside a comment stays comment: string, then
# character literal, then block comment, then line comment.
#
# `re.DOTALL` is what makes the block-comment body span lines. A LINE-LOCAL
# version of this rule cannot see that a `&&` or `||` sits on an interior line
# of a multi-line Doxygen block, so it counts prose as a decision -- which is
# exactly how 18 whole buckets of phantom debt entered the compound-decision
# ratchet baseline (issue #790).
#
# `\\.` under DOTALL also consumes a backslash-newline line splice, so a
# spliced string literal is consumed whole; the line-comment alternative
# spells the splice out for the same reason, since `[^\n]` alone would stop at
# the newline and leak the continuation line back into the scan.
#
# C23 has no raw string literal and this scan reads `.c` translation units
# only (see `_path_included`), so there is no raw-string case to handle.
# Extending the decision scan to C++ would need one.
LEXICAL_NOISE_RE = re.compile(
    r'"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|/\*.*?\*/"
    r"|//(?:\\\r?\n|[^\n])*",
    re.DOTALL,
)


def enclosing_function(src_text: str, decision_line: int) -> str | None:
    """Name of the function enclosing a 1-based source line, or None.

    Relies on the repository's clang-format style: a function-definition body
    opens with ``{`` alone, while control blocks keep the brace on their
    header. Comment-only NOLINTNEXTLINE rows inside multiline signatures are
    skipped so their parenthesized rule name cannot be mistaken for a symbol.
    """
    lines = src_text.splitlines()
    idx = decision_line - 1
    if idx < 0 or idx >= len(lines):
        return None
    brace = idx
    while brace >= 0 and lines[brace] != "{":
        brace -= 1
    if brace < 0:
        return None
    sig_parts: list[str] = []
    j = brace - 1
    while j >= 0 and lines[j].strip() not in ("", "}", "};", "*/", "/*"):
        stripped = lines[j].strip()
        if stripped.startswith("//") or (stripped.startswith("/*") and stripped.endswith("*/")):
            j -= 1
            continue
        sig_parts.insert(0, lines[j])
        if "(" in lines[j]:
            break
        j -= 1
    signature = " ".join(part.strip() for part in sig_parts)
    match = re.search(r"([A-Za-z_]\w*)\s*\(", signature)
    return match.group(1) if match else None


def _component(path: str) -> str:
    """Ownership component containing ``path`` (prefix before ``src/``)."""
    marker = "/src/"
    if marker in path:
        return path.partition(marker)[0]
    return path.rpartition("/")[0]


def _blank_lexical_noise(match: re.Match[str]) -> str:
    """Erase one literal/comment while retaining its newline count."""
    token = match.group(0)
    if token.startswith('"'):
        marker = '""'
    elif token.startswith("'"):
        marker = "''"
    else:
        marker = ""
    return marker + ("\n" * token.count("\n"))


def lexical_code_view(text: str) -> str:
    """Source with comments, literals, and preprocessor directives blanked.

    This is the ONE definition of "code, not prose" for the MC/DC
    compound-decision subsystem. The delta modes and the whole-tree ratchet
    measurement both read decisions through it, so what the gate flags and
    what the baseline counts can never disagree -- a second, line-local
    definition living in the detector is what let 18 buckets of comment prose
    be frozen into the ratchet baseline as real debt (issue #790).

    Line numbering is preserved exactly: every blanked construct keeps its
    newline count, so a caller may index the returned text by source line.

    Blanked here, and therefore never a decision:

    * a string or character literal, including one holding ``/*`` or ``//``
      and one spliced across lines with a trailing backslash;
    * a line comment, including a backslash-spliced continuation line;
    * a block or Doxygen comment, including every interior line and any
      ``@code`` example span inside it;
    * a preprocessor directive and every continuation line of it.

    Preprocessor logic is conditional COMPILATION, not a runtime decision, so
    MC/DC -- a runtime coverage criterion -- does not apply to it. The
    canonical case is the fail-closed stub-crypto guard
    ``#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)``,
    whose ``||`` selects a translation unit and is never evaluated at run
    time.
    """
    scrubbed = LEXICAL_NOISE_RE.sub(_blank_lexical_noise, text)
    lines: list[str] = []
    in_directive = False
    for line in scrubbed.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            in_directive = True
        blanked = "\n" if line.endswith("\n") else ""
        lines.append(blanked if in_directive else line)
        if in_directive and not line.rstrip().endswith("\\"):
            in_directive = False
    return "".join(lines)


def _compound_segments(text: str) -> list[tuple[int, str]]:
    """Logical statement/control-header segments containing && or ||."""
    segments: list[tuple[int, str]] = []
    source = lexical_code_view(text)
    buf: list[str] = []
    line = 1
    start_line = 1
    paren_depth = 0
    bracket_depth = 0
    for char in source:
        if not buf and not char.isspace():
            start_line = line
        buf.append(char)
        if char == "(":
            paren_depth += 1
        elif char == ")":
            paren_depth = max(0, paren_depth - 1)
        elif char == "[":
            bracket_depth += 1
        elif char == "]":
            bracket_depth = max(0, bracket_depth - 1)
        boundary = char in ";{}" and paren_depth == 0 and bracket_depth == 0
        if boundary:
            segment = "".join(buf)
            if COMPOUND_OP_RE.search(segment):
                segments.append((start_line, segment))
            buf = []
            start_line = line
        if char == "\n":
            line += 1
    tail = "".join(buf)
    if COMPOUND_OP_RE.search(tail):
        segments.append((start_line, tail))
    return segments


def _drop_atomic_parentheses(tokens: list[str]) -> list[str]:
    """Remove parentheses that cannot affect && / || grouping."""
    result = tokens[:]
    changed = True
    while changed:
        changed = False
        stack: list[int] = []
        for index, value in enumerate(result):
            if value == "(":
                stack.append(index)
            elif value == ")" and stack:
                opening = stack.pop()
                interior = result[opening + 1 : index]
                if "&&" not in interior and "||" not in interior:
                    result = result[:opening] + interior + result[index + 1 :]
                    changed = True
                    break
    return result


def _decision_fingerprint(segment: str) -> str:
    """Alpha-normalized structural token fingerprint for one decision."""
    identifiers: dict[str, str] = {}
    normalized: list[str] = []
    for lexeme in DECISION_TOKEN_RE.findall(segment):
        value = "nullptr" if lexeme == "NULL" else lexeme
        if re.fullmatch(r"[A-Za-z_]\w*", value) and value not in DECISION_KEYWORDS:
            value = identifiers.setdefault(value, f"id{len(identifiers)}")
        normalized.append(value)
    return " ".join(_drop_atomic_parentheses(normalized))


def _logical_decisions(text: str) -> list[tuple[int, str, str, str]]:
    """Return ``(line, symbol, fingerprint, snippet)`` logical decisions."""
    decisions: list[tuple[int, str, str, str]] = []
    for start_line, segment in _compound_segments(text):
        operator = COMPOUND_OP_RE.search(segment)
        if operator is None:
            continue
        line_no = start_line + segment[: operator.start()].count("\n")
        symbol = enclosing_function(text, line_no) or NO_ENCLOSING_FUNCTION
        snippet = re.sub(r"\s+", " ", segment.strip())
        decisions.append((line_no, symbol, _decision_fingerprint(segment), snippet))
    return decisions


def _logical_operators(fingerprint: str) -> tuple[str, ...]:
    """Ordered logical operators retained in one structural fingerprint."""
    return tuple(value for value in fingerprint.split() if value in ("&&", "||"))


def _decision_kind(fingerprint: str) -> str:
    """Control/statement kind anchoring edited-decision ancestry."""
    first = fingerprint.partition(" ")[0]
    return first if first in ("if", "while", "for", "return") else "expression"


def _similar_ancestor(
    old_exact: Counter[tuple[str, str, str]],
    component: str,
    symbol: str,
    fingerprint: str,
) -> tuple[str, str, str] | None:
    """Best same-symbol, same-topology edited ancestor above the policy floor."""
    operators = _logical_operators(fingerprint)
    best_key: tuple[str, str, str] | None = None
    best_ratio = 0.0
    for key, count in old_exact.items():
        old_component, old_symbol, old_fingerprint = key
        if count <= 0 or (old_component, old_symbol) != (component, symbol):
            continue
        if _logical_operators(old_fingerprint) != operators:
            continue
        if _decision_kind(old_fingerprint) != _decision_kind(fingerprint):
            continue
        ratio = SequenceMatcher(
            None, old_fingerprint.split(), fingerprint.split(), autojunk=False
        ).ratio()
        if ratio > best_ratio:
            best_key = key
            best_ratio = ratio
    return best_key if best_ratio >= DECISION_ANCESTRY_SIMILARITY else None


def _collect_decision_ancestry(
    pairs: list[tuple[str | None, str | None]],
    new_text_of: Callable[[str], str],
    base_text_of: Callable[[str], str],
) -> tuple[
    Counter[tuple[str, str, str]],
    Counter[tuple[str, str]],
    Counter[tuple[str, str]],
    list[tuple[str, str, int, str, str, str]],
]:
    """Collect old/new structural inventories with cross-root move ancestry."""
    old_exact: Counter[tuple[str, str, str]] = Counter()
    old_totals: Counter[tuple[str, str]] = Counter()
    new_totals: Counter[tuple[str, str]] = Counter()
    new_items: list[tuple[str, str, int, str, str, str]] = []
    for old_path, new_path in pairs:
        old_decisions = _logical_decisions(base_text_of(old_path)) if old_path else []
        new_decisions = _logical_decisions(new_text_of(new_path)) if new_path else []
        new_component = _component(new_path or old_path or "")
        old_component = _component(old_path or new_path or "")
        if old_component != new_component:
            old_shapes = {(symbol, fingerprint) for _, symbol, fingerprint, _ in old_decisions}
            new_shapes = {(symbol, fingerprint) for _, symbol, fingerprint, _ in new_decisions}
            if old_shapes & new_shapes:
                old_component = new_component
        for _line, symbol, fingerprint, _snippet in old_decisions:
            old_exact[(old_component, symbol, fingerprint)] += 1
            old_totals[(old_component, symbol)] += len(_logical_operators(fingerprint))
        if new_path:
            for line, symbol, fingerprint, snippet in new_decisions:
                new_items.append((new_component, new_path, line, symbol, fingerprint, snippet))
                new_totals[(new_component, symbol)] += len(_logical_operators(fingerprint))
    return old_exact, old_totals, new_totals, new_items


def new_decision_occurrences(
    pairs: list[tuple[str | None, str | None]],
    new_text_of: Callable[[str], str],
    base_text_of: Callable[[str], str],
) -> list[tuple[str, int, str, str]]:
    """New structural decisions after exact-symbol/component-move matching."""
    old_exact, old_totals, new_totals, new_items = _collect_decision_ancestry(
        pairs, new_text_of, base_text_of
    )

    unmatched: list[tuple[str, str, int, str, str, str]] = []
    for component, path, line, symbol, fingerprint, snippet in new_items:
        key = (component, symbol, fingerprint)
        if old_exact[key] > 0:
            old_exact[key] -= 1
        else:
            unmatched.append((component, path, line, symbol, fingerprint, snippet))
    result: list[tuple[str, int, str, str]] = []
    for component, path, line, symbol, fingerprint, snippet in unmatched:
        owner = (component, symbol)
        ancestor = None
        if new_totals[owner] <= old_totals[owner]:
            ancestor = _similar_ancestor(old_exact, component, symbol, fingerprint)
        if ancestor is not None:
            old_exact[ancestor] -= 1
        else:
            result.append((path, line, snippet, symbol))
    return result

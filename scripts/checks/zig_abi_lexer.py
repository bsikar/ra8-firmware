# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Read C and Zig source as tokens, declarations, and digests.

The primitives every ABI rule is built on, and the only part of the policy
checker that touches raw source text. They answer four questions: what the
text says once comments and conditional regions are gone, which symbols a
header or a Zig adapter exports, whether a policy fragment appears as a
contiguous token run, and what a header hashes to once layout is normalised
away.

Nothing here knows about the policy file, so it sits at the bottom of the
import graph and the rules import downward into it.
"""

from __future__ import annotations

import hashlib
import re

# "ra8_unit_symbol" carries a two-part ra8_<unit>_ prefix; anything shorter is
# the whole symbol.
MIN_PREFIXED_SYMBOL_PARTS = 2


def _strip_comments(text: str) -> str:
    """Remove C/Zig comments before inventory extraction."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n\r]*", "", text)


def _strip_conditional_blocks(text: str) -> str:
    """Remove conditional-preprocessor regions from assertion evidence."""
    kept: list[str] = []
    conditional_depth = 0
    for line in text.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", line)
        if directive:
            kind = directive.group(1)
            if kind in {"if", "ifdef", "ifndef"}:
                conditional_depth += 1
                continue
            if conditional_depth and kind == "endif":
                conditional_depth -= 1
                continue
        if not conditional_depth:
            kept.append(line)
    return "".join(kept)


def _lexical_tokens(text: str) -> list[str]:
    """Return assertion-relevant C/Zig tokens while ignoring layout.

    Comments come off FIRST. An apostrophe in prose ("the facade's contract")
    is not a char literal, but the literal-stripping pass cannot tell, so it
    pairs that apostrophe with the next one and swallows every line between
    them, assertions included. ra8_audio's adapter is the case that proved it:
    two doc comments with apostrophes hid a whole comptime block, and the
    policy reported twenty-two assertions missing from a file declaring them.
    """
    clean = _strip_comments(text)
    clean = re.sub(r"(?m)\\\\[^\r\n]*", "", clean)
    clean = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', "", clean)
    clean = _strip_conditional_blocks(clean)
    return re.findall(
        r"@[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*|\d+[A-Za-z]*|==|!=|\S", clean
    )


def _contains_token_sequence(text: str, fragment: str) -> bool:
    """Match one policy fragment as a contiguous lexical token sequence."""
    tokens = _lexical_tokens(text)
    expected = _lexical_tokens(fragment)
    width = len(expected)
    return bool(expected) and any(
        tokens[index : index + width] == expected for index in range(len(tokens))
    )


def _normalized_header_digest(text: str) -> str:
    """Hash representation-bearing header text without comments or whitespace."""
    normalized = re.sub(r"\s+", "", _strip_comments(text))
    return hashlib.sha256(normalized.encode()).hexdigest()


def _symbol_root(symbol: str) -> str:
    """Return the ra8_<unit>_ prefix a retained C symbol is declared under."""
    parts = symbol.split("_")
    return "_".join(parts[:2]) + "_" if len(parts) > MIN_PREFIXED_SYMBOL_PARTS else symbol


def _header_exports(text: str, prefix: str) -> set[str]:
    """Extract namespaced function declarations from a public C header."""
    return set(re.findall(rf"\b({re.escape(prefix)}[A-Za-z0-9_]+)\s*\(", _strip_comments(text)))


def _zig_exports(text: str) -> tuple[set[str], dict[str, str]]:
    """Extract exported Zig names and complete declaration heads."""
    clean = _strip_comments(text)
    matches = list(re.finditer(r"\b(?:pub\s+)?export\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)", clean))
    heads: dict[str, str] = {}
    for match in matches:
        brace = clean.find("{", match.end())
        heads[match.group(1)] = clean[match.start() : brace if brace >= 0 else len(clean)]
    return set(heads), heads

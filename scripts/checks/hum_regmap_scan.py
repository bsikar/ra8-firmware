# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Find the register SYMBOLS first-party C claims exist, and where it claims it.

Two claim sites matter, because the invented-register defects landed through
both of them:

* a **citation** that names a register -- ``/* HUM Ch 32.3 "EASCR : Security
  Configuration" p 1697 */``. The symbol is right there in the quoted section
  name, together with the chapter and page the author believed it lived on.
* a **register window** in a ``*_regs.h`` -- the struct members and the
  ``k_..._off_...`` offset enum. #498's ``ra8_ptp_regs.h`` declared thirteen
  registers this way while its citations named no symbol at all, so a
  citation-only scan would have missed it entirely.

This module only reports what the source CLAIMS. Deciding whether the manual
agrees is ``check_hum_register_map.py``'s job, and the manual's side comes
from ``hum_regmap.py``.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

# A HUM citation, deliberately the same shape cite_check.py accepts so the two
# gates cannot disagree about what a citation is.
CITE_RE = re.compile(
    r"""/\*\s*HUM\s+Ch\s+
    (?P<chapter>\d{1,2})
    (?:\.(?P<sub>\d{1,3}(?:\.\d{1,3})*))?
    \s*
    "(?P<section>[^"]*)"
    \s*,?\s*
    (?:(?:Table|Figure)\s+\d{1,3}(?:\.\d{1,3})*\s*)?
    p\s+
    (?P<start>\d{1,5})
    (?:\s*-\s*(?P<end>\d{1,5}))?
    """,
    re.VERBOSE,
)

# The register name inside a citation's quoted section, e.g. the "EATASGL0" of
# "EATASGL0 : TAS Gate Learn Register 0". A bare symbol with no description is
# accepted too, because a long register name does not always fit the column
# limit. Anything that is not shaped like an abbreviation (prose section names
# such as "Error Interrupt Sources") yields no symbol and is skipped.
CITE_SYMBOL_RE = re.compile(r"^\s*(?P<symbol>[A-Z][A-Z0-9_]{1,23}[a-z]?)\s*(?::|$)")

# A register member of an MMIO window struct: "volatile uint32_t EATASGL0;" or
# "volatile uint32_t EATMFSC[8];". Reserved padding is excluded by name.
MEMBER_RE = re.compile(
    r"^\s*volatile\s+u?int(?:8|16|32|64)_t\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;"
)

# An offset enumerator: "k_ra8_etha_off_eatasgl0 = 0x03C0U,". The trailing
# component after "_off_" is the register symbol in lower case.
OFFSET_RE = re.compile(
    r"^\s*k_(?:ra8_)?(?P<module>[a-z0-9_]+?)_off(?:set)?_(?P<symbol>[a-z0-9_]+)\s*=\s*"
    r"(?P<value>0[xX][0-9A-Fa-f]+)[uUlL]*\s*,"
)

# Any mention of a HUM chapter, wherever it appears -- used only to work out
# which chapters a header's register window belongs to. See cited_chapters().
LOOSE_CHAPTER_RE = re.compile(r"HUM\s+Ch\s+(\d{1,2})\b")

# Padding members, never registers.
RESERVED_PREFIXES = ("reserved", "rsv", "pad", "dummy", "unused")


@dataclass(frozen=True)
class SymbolClaim:
    """One place in first-party source that claims a register exists.

    Attributes:
        path: Repo-relative file the claim was made in.
        line: 1-based line number of the claim.
        symbol: Register symbol as the source spells it.
        chapter: HUM chapter the claim is attributed to, or None when the
            claim site carries no chapter of its own (a struct member relies
            on the chapters its file cites).
        page: Page the citation points at, or None for a struct claim.
        page_end: End of a cited page range, or None.
        offset: Byte offset the source declares, or None.
        kind: ``cite``, ``member`` or ``offset``.
    """

    path: str
    line: int
    symbol: str
    chapter: int | None
    page: int | None
    page_end: int | None
    offset: int | None
    kind: str


def _line_of(text: str, position: int) -> int:
    """Return the 1-based line number containing byte offset `position`."""
    return text.count("\n", 0, position) + 1


def cite_claims(path: str, text: str) -> list[SymbolClaim]:
    """Every citation in `text` that names a register symbol."""
    claims = []
    for match in CITE_RE.finditer(text):
        symbol_match = CITE_SYMBOL_RE.match(match.group("section"))
        if symbol_match is None:
            continue
        start = int(match.group("start"))
        end = int(match.group("end")) if match.group("end") else start
        claims.append(
            SymbolClaim(
                path=path,
                line=_line_of(text, match.start()),
                symbol=symbol_match.group("symbol"),
                chapter=int(match.group("chapter")),
                page=start,
                page_end=end,
                offset=None,
                kind="cite",
            )
        )
    return claims


def is_reserved(name: str) -> bool:
    """True when a struct member is padding rather than a register."""
    lowered = name.lower()
    return any(lowered.startswith(prefix) for prefix in RESERVED_PREFIXES)


def window_claims(path: str, text: str) -> list[SymbolClaim]:
    """Every register-window struct member and offset enumerator in `text`."""
    claims = []
    for number, line in enumerate(text.split("\n"), start=1):
        member = MEMBER_RE.match(line)
        if member is not None:
            name = member.group("name")
            if not is_reserved(name) and name.isupper():
                claims.append(SymbolClaim(path, number, name, None, None, None, None, "member"))
            continue
        offset = OFFSET_RE.match(line)
        if offset is not None:
            symbol = offset.group("symbol").upper()
            claims.append(
                SymbolClaim(
                    path,
                    number,
                    symbol,
                    None,
                    None,
                    None,
                    int(offset.group("value"), 16),
                    "offset",
                )
            )
    return claims


def cited_chapters(text: str) -> set[int]:
    """Every HUM chapter number `text` names anywhere.

    A register window is attributed to the chapters its own file names. That
    is the attribution the #540 issue proposed, and it needs no new
    annotation: a header that declares an MMIO window and names no chapter at
    all cannot be checked, and is skipped rather than guessed at.

    This uses the LOOSE pattern, not ``CITE_RE``, and deliberately so. A
    header often states its chapter only in the ``@details`` prose of its
    file-level Doxygen block -- ``ra8_rsip_regs_offsets.h`` says "HUM Ch 52
    'Renesas Secure IP (RSIP-E50D)' p 3302" there and carries no standalone
    ``/* HUM ... */`` comment at all. Requiring the strict form left that
    whole header unattributed and therefore unchecked, which is the opposite
    of what this gate is for. Attribution only has to know which chapters a
    file is ABOUT; whether an individual citation is well-formed is
    cite_check.py's question.
    """
    return {int(match.group(1)) for match in LOOSE_CHAPTER_RE.finditer(text)}

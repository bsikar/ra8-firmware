# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The checks that need only the source text, not a parse.

Two of this gate's seven findings are answerable lexically, and answering them
without libclang is deliberate: DOC004 (two doc blocks in a row with nothing
between them) and DOC007 (banned definition-site boilerplate) are statements
about the *comment stream*, which a parse discards.

Everything here therefore works on text that has had comments and literals
blanked out but line structure preserved, so a brace inside a string or a
declaration-looking line inside a comment cannot be mistaken for code.
"""

from __future__ import annotations

import re

from docattach_model import (
    _T,
    BANNED_BOILERPLATE_RE,
    COMMENTED_DEFINE_RE,
    EXPLICIT_REF_RE,
    GROUP_MARKER_RE,
    STANDALONE_TAG_RE,
    DocBlock,
    Finding,
)


# ---------------------------------------------------------------------------
# Lexical pass
# ---------------------------------------------------------------------------
def _blank_comments_and_literals(text: str) -> str:
    """Blank every comment and string/char literal, preserving line structure.

    Length and newline positions are preserved so the result can be indexed by
    the same line numbers as the original.  Blanking (rather than deleting) is
    what lets the DOC004 "is there code between these two blocks" test look at
    code only -- an earlier version advanced past comments without clearing
    them, so a member's own trailing ``/**< ... */`` counted as intervening
    code and the check silently never fired on real member runs.
    """
    out = list(text)

    def blank(lo: int, hi: int) -> None:
        for k in range(lo, min(hi, len(text))):
            if text[k] != "\n":
                out[k] = " "

    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in {'"', "'"}:
            quote = ch
            start = i
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            blank(start, i)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            while i < n and text[i] != "\n":
                i += 1
            blank(start, i)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            start = i
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            blank(start, i)
            continue
        i += 1
    return "".join(out)


def _skip_literal(text: str, i: int, line: int) -> tuple[int, int]:
    """Advance past the string/char literal starting at ``i``."""
    quote = text[i]
    n = len(text)
    i += 1
    while i < n:
        if text[i] == "\\" and i + 1 < n:
            i += 2
            continue
        if text[i] == quote:
            i += 1
            break
        if text[i] == "\n":
            line += 1
        i += 1
    return i, line


def _scan_block_comment(text: str, i: int, line: int) -> tuple[int, int, bool]:
    """Advance past the ``/* ... */`` at ``i``; report whether it is a doc block."""
    n = len(text)
    is_doc = i + 2 < n and text[i + 2] in {"*", "!"}
    # `/**/` and `/***/` are terminators, not documentation.
    if is_doc and i + 3 < n and text[i + 2] == "*" and text[i + 3] == "/":
        is_doc = False
    j = i + 2
    while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
        if text[j] == "\n":
            line += 1
        j += 1
    return j + 2, line, is_doc


def extract_doc_blocks(text: str) -> list[DocBlock]:
    """Return every ``/**`` / ``/*!`` block in ``text``, in source order.

    Blocks inside string literals are skipped.  Single-line ``///`` and ``//!``
    comments are captured too so the duplicate check sees them.
    """
    blocks: list[DocBlock] = []
    i, n = 0, len(text)
    line = 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch in {'"', "'"}:
            i, line = _skip_literal(text, i, line)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            start_line = line
            end, line, is_doc = _scan_block_comment(text, i, line)
            if is_doc:
                blocks.append(DocBlock(start_line, line, text[i:end]))
            i = end
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            is_doc = i + 2 < n and text[i + 2] in {"/", "!"}
            chunk_start = i
            while i < n and text[i] != "\n":
                i += 1
            if is_doc:
                blocks.append(DocBlock(line, line, text[chunk_start:i]))
            continue
        i += 1
    for b in blocks:
        b.standalone = bool(STANDALONE_TAG_RE.search(b.text) or GROUP_MARKER_RE.search(b.text))
        b.trailing = bool(re.match(r"/(?:\*[*!]|//|/!)<", b.text))
    return blocks


def check_consecutive_blocks(path: str, text: str) -> list[Finding]:
    """DOC004 -- two doc blocks in a row with nothing declared between them.

    This is the owner's literal complaint and the shape that makes a presence
    gate *reward* the defect.  A ``@file`` / ``@defgroup`` / ``@{`` block
    followed by a real block is normal and is exempted via ``standalone``.
    """
    findings: list[Finding] = []
    blocks = extract_doc_blocks(text)
    lines = _blank_comments_and_literals(text).splitlines()
    raw_lines = text.splitlines()
    for prev, cur in zip(blocks, blocks[1:]):
        if prev.standalone or prev.trailing or cur.trailing:
            continue
        # A block carrying an explicit @def / @fn / @struct / ... tag is
        # resolved by Doxygen *by name*, not by position, so it does not need a
        # following declaration at all.  Config headers rely on this to
        # document deliberately-disabled options:
        #     /** \def MBEDTLS_AES_ROM_TABLES ... */
        #     //#define MBEDTLS_AES_ROM_TABLES
        # The commented-out define is not code, so a purely positional test
        # calls that a duplicate.  Accept the block when the very next non-blank
        # source line mentions the symbol it names -- which still leaves a block
        # whose symbol is declared further down, past another symbol, reported.
        refs = [ref for _, ref in EXPLICIT_REF_RE.findall(prev.text)]
        if refs:
            following = next(
                (ln for ln in raw_lines[prev.end_line : cur.start_line - 1] if ln.strip()),
                "",
            ) or next((ln for ln in raw_lines[prev.end_line :] if ln.strip()), "")
            if any(re.search(r"\b" + re.escape(r) + r"\b", following) for r in refs):
                continue
        # A *commented-out* preprocessor directive is the config-header idiom
        # for "here is an option, documented and deliberately off":
        #     /** Enable the Everest ECDH backend ... */
        #     //#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
        # The block documents that option, so it is attached -- but the
        # directive is lexically a comment, so `_blank_comments_and_literals`
        # erases it and a purely positional test sees an empty gap and calls
        # the block a duplicate.  Consult the raw text for this one shape.
        # Deliberately narrow: only a commented-out #define / #undef counts,
        # so two genuinely adjacent blocks are still reported.
        raw_between = raw_lines[prev.end_line : cur.start_line - 1]
        if any(COMMENTED_DEFINE_RE.match(seg) for seg in raw_between):
            continue
        # Anything but whitespace between the two blocks means the first one is
        # attached to something -- not a duplicate.
        between = lines[prev.end_line : cur.start_line - 1]
        if any(seg.strip() for seg in between):
            continue
        findings.append(
            Finding(
                path,
                prev.start_line,
                "DOC004",
                "(block)",
                f"doc block is followed by another doc block at line {cur.start_line} "
                "with no declaration between them; the first documents nothing",
            )
        )
    return findings


def check_banned_boilerplate(path: str, text: str) -> list[Finding]:
    """DOC007 -- CLAUDE.md's banned pointer-only definition-site comment.

    Only fires on blocks whose *entire* informational content is the pointer.
    A block that says "see the header for the full contract" and then adds a
    real implementation note is allowed by CLAUDE.md, so the presence of any
    other Doxygen tag or a ``--`` note clause clears it.
    """
    findings: list[Finding] = []
    for block in extract_doc_blocks(text):
        if not BANNED_BOILERPLATE_RE.search(block.text):
            continue
        body = re.sub(r"^\s*/\*+<?|\*+/\s*$", " ", block.text)
        body = re.sub(r"^\s*\*", " ", body, flags=re.MULTILINE)
        tags = set(re.findall(_T + r"([a-z]+)", body))
        # @brief alone is still boilerplate; any other tag means real content.
        if tags - {"brief"}:
            continue
        # Strip everything the idiom is *allowed* to contain -- the @brief tag,
        # the "Implementation of <symbol>()" lead-in (backticked or not), the
        # pointer phrase itself, and punctuation.  Whatever survives is the
        # real implementation note CLAUDE.md requires.  If nothing survives,
        # the comment says only "see the header" and is banned.
        residue = BANNED_BOILERPLATE_RE.sub(" ", body)
        residue = re.sub(_T + r"brief", " ", residue)
        residue = re.sub(
            r"[Ii]mplementation of\s+`?[A-Za-z_][A-Za-z_0-9]*`?(?:\s*\(\))?`?", " ", residue
        )
        residue = re.sub(r"[`(){}.,;:*/\s-]", "", residue)
        if residue:
            continue
        findings.append(
            Finding(
                path,
                block.start_line,
                "DOC007",
                "(block)",
                "pointer-only definition-site boilerplate (CLAUDE.md bans it): the comment "
                "carries no information the signature does not. Delete it, or replace it "
                "with the single-line form carrying a real implementation note",
            )
        )
    return findings

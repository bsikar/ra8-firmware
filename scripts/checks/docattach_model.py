# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""What ``check_doc_attachment.py`` finds, and the shapes it reasons over.

The finding codes, the Doxygen tag grammar, and the three records the two
passes exchange: a :class:`Finding`, a :class:`DocBlock` (one comment and the
line it attaches to) and :class:`DocTags` (what that comment claims).

Kept apart from both passes so the lexical pass and the AST pass cannot drift
into disagreeing about what a doc block is -- the entire gate rests on the two
agreeing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Finding codes.  Ordered by signal strength: a DOC001 is near-certain paste
# evidence, a DOC005 is strong, a DOC004 is the owner's literal complaint.
# ---------------------------------------------------------------------------
CODE_HELP = {
    "DOC001": "@param names a parameter the signature does not have",
    "DOC002": "partially documented signature -- some parameters have no @param",
    "DOC003": "@return/@retval on a function returning void",
    "DOC004": "two doc blocks in a row with no declaration between them",
    "DOC005": "block names a different symbol than the one it is attached to",
    "DOC006": "block sits on a forward declaration whose definition is bare",
    "DOC007": "banned pointer-only definition-site boilerplate",
}

# ---------------------------------------------------------------------------
# Doxygen tag scanning.  Doxygen accepts both `@tag` and `\tag`.
# ---------------------------------------------------------------------------
_T = r"[@\\]"

#: ``@param[in] name`` / ``@param[in,out] a,b`` / ``@param name``.
PARAM_RE = re.compile(
    _T + r"param\s*(?:\[[^\]]*\])?\s+"
    r"([A-Za-z_][A-Za-z_0-9]*(?:\s*,\s*[A-Za-z_][A-Za-z_0-9]*)*)"
)
RETURN_RE = re.compile(_T + r"returns?\b")
RETVAL_RE = re.compile(_T + r"retval\b")
COPY_RE = re.compile(_T + r"copy(?:doc|details|brief)\b")

#: Explicit "this block documents symbol X" tags.  These are unambiguous: if
#: the tag names X and the block is attached to Y, one of the two is wrong.
EXPLICIT_REF_RE = re.compile(
    _T + r"(fn|struct|enum|union|def|var|typedef|class)\s+([A-Za-z_][A-Za-z_0-9]*)"
)

#: The CLAUDE.md-sanctioned definition-site single-line form:
#:   /** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
#: The backticked name is a machine-checkable claim about which function this
#: is; a pasted block carries the wrong one.
IMPL_OF_RE = re.compile(r"[Ii]mplementation of\s+`([A-Za-z_][A-Za-z_0-9]*)\s*\(\)`")

#: Blocks that legitimately stand alone (documentation structure, not a
#: symbol).  A block of this kind directly above another block is normal and
#: must never be reported as a duplicate.
STANDALONE_TAG_RE = re.compile(
    _T + r"(file|dir|mainpage|page|subpage|section|subsection|defgroup|addtogroup"
    r"|ingroup|weakgroup|name|cond|endcond|example|internal|endinternal"
    r"|copyright|brief\s*$)"
)

#: Doxygen grouping markers -- ``/** @{ */`` and ``/** @} */`` sit between two
#: real blocks all the time.
GROUP_MARKER_RE = re.compile(r"[@\\][{}]")

#: A commented-out preprocessor directive: the config-header idiom for a
#: documented-but-disabled build option (``//#define MBEDTLS_FOO``).  It is a
#: real subject for the block above it even though it is lexically a comment.
COMMENTED_DEFINE_RE = re.compile(r"\s*(?://+|/\*)\s*#\s*(?:define|undef)\b")

#: An explicit statement that a function yields no value -- either because it
#: returns void ("Nothing.", "None.") or because it never returns at all
#: ("This function never returns.", on the ``[[noreturn]] void`` handlers and
#: park loops).  Doxygen prefers these be omitted, but they are a deliberate,
#: self-consistent house style here and they state the truth about the
#: signature, so they are not a contradiction.  ``@retval`` on a void function
#: is different: it enumerates return *values* that cannot exist.
RETURN_NOTHING_RE = re.compile(
    _T + r"returns?\s+"
    r"(?:nothing|none|void|n/?a|no value"
    r"|(?:this function |the function )?(?:never returns|does not return|no return))"
    r"\b[.\s]*",
    re.IGNORECASE,
)

#: CLAUDE.md "BANNED (rejected in review)" definition-site boilerplate: a
#: comment whose only content is a pointer back at the header.
BANNED_BOILERPLATE_RE = re.compile(
    r"(?:see (?:the )?(?:public )?header for "
    r"(?:the |full )?(?:documented )?(?:contract|description)"
    r"|see header for full contract)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Finding:
    """One gate finding."""

    path: str
    line: int
    code: str
    symbol: str
    detail: str

    def render(self) -> str:
        """One aligned report line for this finding.

        The leading two spaces are part of the format: findings are printed
        under a summary header, and the indent is what visually subordinates
        them to it.
        """
        return f"  {self.path}:{self.line}  {self.code}  {self.symbol}  --  {self.detail}"


@dataclass
class DocBlock:
    """A lexically-extracted ``/** ... */`` or ``/*! ... */`` block."""

    start_line: int
    end_line: int
    text: str
    #: True for blocks that document the file / a group rather than a symbol.
    standalone: bool = False
    #: True for the ``/**<`` "documents the *preceding* member" form.  These
    #: are trailing comments on struct fields and enum values; a run of them on
    #: consecutive lines is the normal shape and must never read as duplicates.
    trailing: bool = False


@dataclass
class DocTags:
    """The claims a doc block makes, extracted once."""

    params: list[str] = field(default_factory=list)
    has_return: bool = False
    has_retval: bool = False
    has_copy: bool = False
    explicit_refs: list[tuple[str, str]] = field(default_factory=list)
    impl_of: str | None = None


def parse_tags(block_text: str) -> DocTags:
    """Extract the claims a block makes.

    ``@code``/``@endcode`` bodies are removed first: a usage example may
    legitimately mention another function's parameters, and reading tags out of
    one produces false positives.
    """
    body = re.sub(r"[@\\]code\b.*?[@\\]endcode\b", " ", block_text, flags=re.DOTALL)
    params: list[str] = []
    for m in PARAM_RE.finditer(body):
        params.extend(p.strip() for p in m.group(1).split(","))
    # A bare "@return Nothing." on a void function states the truth; only a
    # @return that promises an actual value contradicts a void signature.
    return DocTags(
        params=params,
        has_return=bool(RETURN_RE.search(RETURN_NOTHING_RE.sub(" ", body))),
        has_retval=bool(RETVAL_RE.search(body)),
        has_copy=bool(COPY_RE.search(body)),
        explicit_refs=EXPLICIT_REF_RE.findall(body),
        impl_of=(m.group(1) if (m := IMPL_OF_RE.search(body)) else None),
    )

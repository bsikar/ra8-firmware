# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reading source text back, for the checks a parse cannot answer.

Most of this checker reasons over libclang's AST.  Two things cannot be:

* the ``section("...vectors")`` attribute, because ``CursorKind.SECTION_ATTR``
  is absent from the libclang 18.1.x wheels the runners install, so an AST
  lookup silently finds nothing; and
* the NSC range-check idiom, because ``RA8_NSC_CHECK_NS_RANGE_R/_RW`` expands
  to ``cmse_check_address_range()`` only under ``-mcmse``.  This script parses
  without it, so the macro becomes a ``((void)(p),(void)(n))`` no-op and leaves
  no ``CallExpr`` at all.

Both therefore read the pre-preprocessor text.  That is a deliberate exception,
not a general licence to grep instead of parse, and it is confined to this
module so the exception stays countable.
"""

from __future__ import annotations

import pathlib

from annot_model import AnnotatedSymbol

#: Cache of source files read back for textual (macro-level) checks.
_SOURCE_CACHE: dict[str, list[str]] = {}


def source_lines(path: str) -> list[str]:
    """Return ``path``'s lines, cached. Empty list when unreadable."""
    if path not in _SOURCE_CACHE:
        try:
            _SOURCE_CACHE[path] = pathlib.Path(path).read_text(errors="ignore").splitlines()
        except OSError:
            _SOURCE_CACHE[path] = []
    return _SOURCE_CACHE[path]


def definition_text(sym: AnnotatedSymbol) -> str:
    """Return the source text of ``sym``'s definition.

    Used for checks that must see the code *before* preprocessing, because
    the construct being looked for is a macro that this script's parse
    configuration expands away (see the NSC range-check rule).
    """
    lines = source_lines(sym.file)
    if not lines or sym.line <= 0:
        return ""
    end = sym.end_line if sym.end_line >= sym.line else len(lines)
    return "\n".join(lines[sym.line - 1 : end])

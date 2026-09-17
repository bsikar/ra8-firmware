# ADR-0013: Named character reference set for the v1 reflow reader

## Status

Proposed.

## Context

`priv_reflow_tok_decode_entity()` in
`apps/shared_libs/reflow/src/reflow_tokenize_lex.c` recognises exactly five named
character references today:

```c
  } k_named[] = {
    {"amp", (uint32_t)'&'},
    {"lt", (uint32_t)'<'},
    {"gt", (uint32_t)'>'},
    {"quot", (uint32_t)'"'},
    {"apos", (uint32_t)'\''},
  };
```

Numeric references are already complete: `internal_decode_numeric()` in the same
file takes both `&#dec;` and `&#xhex;`, in either letter case, and the layout
walk now steps by decoded code point rather than by byte (issue #686 part 1,
landed on branch `ereader/686-utf8-decode-in-layout`).

So the remaining hole is named references, and it is not an edge case. An
unrecognised reference is deliberately fail-open: the decoder returns `false`,
`internal_stash_one()` falls through, and the literal `&` byte is fed to the
text pool. The consequence is that `caf&eacute;` renders as the seven visible
characters `&eacute;` rather than as `e` with an acute accent, and a paragraph
of ordinary publisher prose (`&nbsp;`, `&mdash;`, `&rsquo;`, `&ldquo;`,
`&hellip;`) renders with its punctuation spelled out. Nothing errors, nothing
is logged, and the book is simply wrong on the page.

Two existing constraints bound the fix, both read from
`apps/shared_libs/reflow/src/reflow_tokenize_internal.h`:

| Constant | Value | What it bounds |
|---|---|---|
| `k_priv_entity_window` | 12 | Bytes scanned for one `&...;` sequence |
| `k_priv_entity_min` | 4 | Shortest sequence accepted (`&lt;`) |

The window is the hard one. It covers `&`, the name, and `;`, so **a reference
name longer than 10 bytes cannot be decoded at all** without widening the
window, and widening it costs a longer bounded scan on every `&` in the
document. Any table this reader adopts has to live inside that limit, and the
limit has to be enforced where the table is written rather than discovered as a
missing glyph months later.

The second constraint is that decoding is not rendering. The shipped face
`libs/ra8_fonts/literata_latin1.ttf` carries 198 code points, declared in
`.github/font-coverage-declaration.txt` on branch
`ereader/687-font-coverage-declaration`: `0020-007E`, `00A0-00FF`, `2013-2014`,
`2018-2019`, `201C-201D`, `2026`. The Latin-1 and general-punctuation half of
any named-entity set lands inside that; the Greek, letterlike, arrow, math and
shape half does not. Decoding `&sum;` correctly and then finding no glyph for
U+2211 is a different failure from not decoding it, and the reader currently has
no defined behaviour for the second one.

## Decision

1. **The set is the XHTML 1.0 entity sets plus XML `apos`**: Latin-1, symbols,
   and special characters, as published by the W3C. Not the HTML5 named
   character reference set: HTML5 carries thousands of names, admits
   semicolon-less legacy forms, and its longest names do not fit the scan
   window. XHTML is also what an EPUB's XHTML content documents are written
   against, so it is the set the corpus actually uses.

2. **Matching is exact and case-sensitive.** `&Eacute;` and `&eacute;` are
   different references, and `&AMP;` is not a reference at all. XML entity names
   are case-sensitive; the HTML5 case-insensitive legacy arm is deliberately not
   adopted.

3. **The table is a sorted static array in its own translation unit**
   (`reflow_tokenize_entities.c`), looked up by binary search over a
   `(name, length)` span, with the sort order and the name-length bound asserted
   by a test rather than assumed. The lookup takes the span between `&` and the
   first `;` inside the window, so the scan stays one forward pass and the hot
   ASCII path is untouched.

4. **The scan window is the table's admission rule.** Every name in the table
   must be at most `k_priv_entity_window - 2` bytes. The implementing PR checks
   this over the whole table in a test, so a future name that does not fit fails
   the build instead of silently never matching.

5. **Fail-open behaviour is preserved.** An unknown or unterminated reference
   still reports "not an entity" and the literal `&` is emitted. This ADR does
   not introduce an error path for malformed markup; the reader's job is to show
   the book, not to validate it.

6. **Decoding a reference does not claim it renders.** Advertising the symbol
   set depends on the explicit `.notdef` / tofu fallback and code-point
   sanitisation of issue #686 part 3. Until that lands, a decoded code point
   outside the declared font coverage is a known gap, recorded here, not a
   feature.

## Consequences

- Ordinary publisher prose renders correctly for the first time: the Latin-1
  accents, the dashes, the curly quotes and the ellipsis all sit inside the
  shipped face's declared coverage, so part 2 alone is a visible correctness
  win with no font work.
- The symbol half of the set decodes to code points the shipped face does not
  carry. That converts a silent wrong-text failure into a visible missing-glyph
  failure, which is an improvement but not a finish; part 3 owns it.
- The table costs flash. The implementing PR must report the measured `.rodata`
  delta from a real build rather than an estimate, per the lane's
  no-unmeasured-numbers rule.
- Widening `k_priv_entity_window` is now a recorded decision rather than an
  incidental edit: it lengthens a bounded scan that runs at every `&`, and it is
  the only way a longer name could ever be added.
- The five-entry table is retired in the same change, so there is one place
  where a reference name maps to a code point.

## Open questions

1. **The exact table contents and count need generating from the primary
   source.** The W3C entity files (`xhtml-lat1.ent`, `xhtml-symbol.ent`,
   `xhtml-special.ent`) could not be fetched during this analysis, so this ADR
   deliberately states no entity count and no longest-name figure. The
   implementing PR generates the table from those files and records both
   numbers, the way `scripts/checks/check_font_coverage.py` records the shipped
   face's coverage.
2. **Does the entity table imply a font-coverage change?** The declaration in
   `.github/font-coverage-declaration.txt` is currently 198 code points. If the
   product decides the symbol set must render rather than tofu, the baked subset
   widens and the two records have to move together.
3. **Attribute values are not entity-decoded.** `priv_reflow_tok_capture_attr()`
   copies a quoted attribute value verbatim into the text pool, so an `&amp;` in
   an `href` or an `alt` stays encoded. Whether link targets need the same
   decode path is a separate question from this one and is not settled here.
4. **No normalisation is performed.** `&Eacute;` and a precomposed U+00C9 in the
   source both reach the pool as U+00C9, but a decomposed `E` plus combining
   acute does not, and this reader does not compose it.

## References

- Issue #686, reflow: decode UTF-8 in the layout path, full entity set, tofu
  fallback. Part 1 (layout decode) landed; this ADR records the contract for
  parts 2 and 3.
- Issue #687 and `.github/font-coverage-declaration.txt`, the shipped face's
  declared 198 code points.
- `apps/shared_libs/reflow/src/reflow_tokenize_lex.c`,
  `priv_reflow_tok_decode_entity()` and `internal_decode_numeric()`.
- `apps/shared_libs/reflow/src/reflow_tokenize_internal.h`,
  `k_priv_entity_window` and `k_priv_entity_min`.
- W3C XHTML 1.0, Appendix A entity sets (Latin-1, symbols, special).

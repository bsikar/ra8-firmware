/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_reflow_css_parse_mcdc.c
 * @brief MC/DC tests for the content-CSS parser / value-scanner decisions.
 *
 * @details
 * Split sibling of test_ra8_reflow_css_select_mcdc.c covering the parse-side
 * decision families of ra8_reflow_css: the whitespace class, the
 * case-insensitive compares, the hex-value ranges, colour parsing (grey /
 * bad hex), the fractional font-size scanner, the font-weight and
 * font-style keyword ORs, the empty-declaration guards, name interning
 * caps, at-rule splitting, unterminated blocks, comment scanning, quote
 * stripping, and url() extraction. Every test reaches the target decision
 * through a public entry point with crafted CSS input; the shared sheet
 * fixture lives in tests/support/reflow_css_test_util.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "support/reflow_css_test_util.h"
#include "unity_minimal.h"

/**
 * @test test_whitespace_variants
 * @brief Every CSS whitespace byte is accepted by the top-level scanner + trim.
 *
 * @par MC/DC:
 * Decision: `priv_is_ws(c)` =
 *   `(c==' ') || (c=='\t') || (c=='\n') || (c=='\r') || (c=='\f')` (5-cond OR;
 * reached from ::ra8_css_parse's inter-rule skip and ::priv_trim). N+1 = 6:
 *  - a space byte  -> cond1 true (others false).
 *  - a tab byte    -> cond2 true (cond1 false).
 *  - a newline     -> cond3 true.
 *  - a CR byte     -> cond4 true.
 *  - a form-feed   -> cond5 true.
 *  - a name byte   -> all five false (the rule body is reached, not skipped).
 * Each whitespace vector, paired with the name-byte vector, isolates one
 * condition: the rule still parses (proving the byte was treated as ws and the
 * selector / value were trimmed), giving exactly one rule with text-align set.
 */
static void test_whitespace_variants(void)
{
  TEST_BEGIN("css whitespace variants (priv_is_ws 5-OR)");
  /* Each delimiter around the selector / value is a distinct ws byte; the name
   * bytes ('p', 't', ...) drive the all-false arm. */
  load(" \t\n\r\fp\t{\n text-align\r:\f center ; }");
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra8_css_set_align) != 0U);
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, s_sheet.rules[0].decl.align);
  TEST_END("css whitespace variants (priv_is_ws 5-OR)");
}

/**
 * @test test_ci_eq_length_tiebreak
 * @brief A value longer than the keyword does NOT match (length tie-break).
 *
 * @par MC/DC:
 * Decision: `priv_ci_eq` return `(k == len) && (lit[k] == '\0')` (2-cond AND;
 * reached via the `color` keyword compare). N+1 = 3:
 *  - "navy"  -> k==len AND lit[k]=='\0' -> true  (both true: navy colour set).
 *  - "navyy" -> the loop ends at lit[k]=='\0' with k<len, so k!=len -> false
 *               (varies cond1: the trailing byte makes the span longer).
 *  - "nav"   -> k==len but lit[k]=='y' != '\0' -> false (varies cond2: the span
 *               is a prefix shorter than the keyword).
 * The false arms leave the `color` set bit clear; the true arm sets navy.
 */
static void test_ci_eq_length_tiebreak(void)
{
  TEST_BEGIN("css priv_ci_eq length tie-break (k==len && lit[k]==0)");
  const uint8_t cset = (uint8_t)k_ra8_css_set_color;
  load("p { color: navy; }"   /* exact -> set                 */
       "h1 { color: navyy; }" /* longer than keyword -> unset */
       "h2 { color: nav; }"); /* shorter prefix -> unset      */
  TEST_ASSERT_EQ(3, s_sheet.rule_count);
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(k_css_color_navy, s_sheet.rules[0].decl.color);
  TEST_ASSERT((s_sheet.rules[1].decl.set & cset) == 0U); /* "navyy" rejected */
  TEST_ASSERT((s_sheet.rules[2].decl.set & cset) == 0U); /* "nav" rejected   */
  TEST_END("css priv_ci_eq length tie-break (k==len && lit[k]==0)");
}

/**
 * @test test_ci_contains_short_span
 * @brief text-decoration shorter than "underline" hits the sub-too-long guard.
 *
 * @par MC/DC:
 * Decision: `priv_ci_contains` early-out `(s==NULL) || (sl==0) || (sl > len)`
 * (3-cond OR; `sub` is the fixed literal "underline", sl=9). The reachable
 * condition is cond3 (`sl > len`):
 *  - value "underline"   -> len=9, sl=9, sl>len false -> scan runs -> underline
 *                           ON (cond3 false: the substring is found).
 *  - value "none"        -> len=4, sl=9, sl>len true  -> immediate false ->
 *                           underline OFF (cond3 true: span shorter than sub).
 * s is never NULL and sl(9) is never 0 here, so cond1/cond2 are held false and
 * cond3 alone flips the outcome.
 */
static void test_ci_contains_short_span(void)
{
  TEST_BEGIN("css priv_ci_contains short-span guard (sl > len)");
  const uint8_t   ubit = (uint8_t)k_ra8_reflow_style_underline;
  ra8_css_style_t on   = inl("text-decoration: underline");
  ra8_css_style_t off  = inl("text-decoration: none");
  TEST_ASSERT((on.set & (uint8_t)k_ra8_css_set_underline) != 0U);
  TEST_ASSERT((on.style & ubit) != 0U); /* "underline" found */
  TEST_ASSERT((off.set & (uint8_t)k_ra8_css_set_underline) != 0U);
  TEST_ASSERT((off.style & ubit) == 0U); /* "none" -> sub too long */
  TEST_END("css priv_ci_contains short-span guard (sl > len)");
}

/**
 * @test test_hex_val_ranges
 * @brief Hex colours exercise the digit, lower a-f, and not-a-digit arms.
 *
 * @par MC/DC:
 * priv_hex_val classifies each colour byte through two ranges:
 *   `(c>='0') && (c<='9')`  -> digit arm,
 *   `(l>='a') && (l<='f')`  -> letter arm (l is the lower-folded byte),
 *   else                    -> k_priv_hex_base sentinel (invalid).
 *  - "#012345" -> all digits   -> digit arm taken (valid 6-digit colour).
 *  - "#ABCDEF" -> all letters  -> letter arm taken (upper-cased, folded valid).
 *  - "#0g0"    -> 'g' > 'f'     -> sentinel arm -> invalid -> color NOT set.
 * Vectors 1/2 isolate the two valid arms; vector 3 isolates the reject arm.
 */
static void test_hex_val_ranges(void)
{
  TEST_BEGIN("css priv_hex_val digit / a-f / invalid arms");
  const uint8_t cset = (uint8_t)k_ra8_css_set_color;
  load("p { color: #012345; }"  /* digits               */
       "h1 { color: #ABCDEF; }" /* letters              */
       "h2 { color: #0g0; }");  /* bad nibble -> reject */
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(0x012345, s_sheet.rules[0].decl.color);
  TEST_ASSERT((s_sheet.rules[1].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(0xABCDEF, s_sheet.rules[1].decl.color);
  TEST_ASSERT((s_sheet.rules[2].decl.set & cset) == 0U); /* invalid -> unset */
  TEST_END("css priv_hex_val digit / a-f / invalid arms");
}

/**
 * @test test_parse_color_grey_and_bad_hex
 * @brief gray/grey both resolve via the keyword OR; a digitless `#` is invalid.
 *
 * @par MC/DC:
 * Two decisions in priv_parse_color / priv_parse_hex_color:
 *  (A) gray/grey OR `ci_eq("gray") || ci_eq("grey")`:
 *      - "gray" -> cond1 true (gray colour set).
 *      - "grey" -> cond1 false, cond2 true (same colour, the British spelling).
 *      The control "red" keeps both false yet still parses (a different arm).
 *  (B) priv_parse_hex_color length dispatch `len == 6` / `len == 3`:
 *      - "#012345" elsewhere -> the 6-digit arm.
 *      - "#" (no digits) -> neither len==6 nor len==3 -> the fall-through
 *        invalid return, so `color` is NOT set (the digitless hex arm).
 */
static void test_parse_color_grey_and_bad_hex(void)
{
  TEST_BEGIN("css priv_parse_color gray/grey OR + digitless hex");
  const uint8_t cset = (uint8_t)k_ra8_css_set_color;
  load("p { color: red; }" /* control: parses, gray/grey OR both false */
       "h1 { color: gray; }"
       "h2 { color: grey; }");
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ(k_css_color_gray, s_sheet.rules[1].decl.color); /* cond1 */
  TEST_ASSERT_EQ(k_css_color_gray, s_sheet.rules[2].decl.color); /* cond2 */
  /* A '#' with no hex digits falls through the 6-/3-digit length dispatch. */
  ra8_css_style_t hash = inl("color: #");
  TEST_ASSERT((hash.set & cset) == 0U); /* "#" with no digits -> invalid */
  TEST_END("css priv_parse_color gray/grey OR + digitless hex");
}

/**
 * @test test_fontsize_fractional_loops
 * @brief Fractional font-size digits beyond the cap are kept-then-skipped.
 *
 * @par MC/DC:
 * Two loops in priv_scan_hundredths:
 *  (A) the kept-fraction loop guard
 *      `(*i<len) && digit && (fd < k_priv_fs_frac)` -- "1.567em" keeps 2 frac
 *      digits then the `fd < k_priv_fs_frac` condition goes false on the 3rd
 *      ('7'), exiting with 156%.
 *  (B) the skip-the-rest loop `(*i<len) && digit` -- the same "1.567em" then
 *      consumes the leftover '7' in the skip loop (loop body runs), whereas
 *      "1.5em" never enters it (no leftover digit -> guard false at entry).
 *  - "1.567em" -> 156% (kept loop exits on fd cap; skip loop runs once).
 *  - "1.5em"   -> 150% (kept loop exits on no-more-digits; skip loop skipped).
 */
static void test_fontsize_fractional_loops(void)
{
  TEST_BEGIN("css font-size fractional cap + skip loops");
  const uint8_t   fset = (uint8_t)k_ra8_css_set_fontsize;
  ra8_css_style_t a    = inl("font-size: 1.567em");
  TEST_ASSERT((a.set & fset) != 0U);
  TEST_ASSERT_EQ(k_css_fs_156, a.font_val); /* 3rd frac digit dropped */
  TEST_ASSERT_EQ(k_ra8_css_font_pct, a.font_unit);
  ra8_css_style_t b = inl("font-size: 1.5em");
  TEST_ASSERT_EQ(k_css_fs_150, b.font_val); /* exact, no skip loop */
  TEST_END("css font-size fractional cap + skip loops");
}

/**
 * @test test_font_weight_keywords
 * @brief All six bold-selecting font-weight keywords flip the bold value bit.
 *
 * @par MC/DC:
 * Decision (priv_apply_emphasis): `bold || bolder || 600 || 700 || 800 || 900`
 * (6-cond OR). Each keyword isolates one condition true (the rest false), and
 * `normal` drives the all-false arm:
 *  - "bold"   -> cond1; "bolder" -> cond2; "600" -> cond3;
 *  - "700"    -> cond4; "800"    -> cond5; "900" -> cond6;
 *  - "normal" -> all six false -> bold OFF (but the set bit is still recorded).
 */
static void test_font_weight_keywords(void)
{
  TEST_BEGIN("css font-weight bold keyword 6-OR");
  const uint8_t      bbit               = (uint8_t)k_ra8_reflow_style_bold;
  const uint8_t      bset               = (uint8_t)k_ra8_css_set_bold;
  static const char* s_k_bold_kw[6]     = {"bold", "bolder", "600", "700", "800", "900"};
  char               buf[k_css_buf_cap] = {};
  for (size_t w = 0U; w < (sizeof(s_k_bold_kw) / sizeof(s_k_bold_kw[0])); ++w) {
    (void)snprintf(buf, sizeof buf, "font-weight: %s", s_k_bold_kw[w]);
    ra8_css_style_t d = inl(buf);
    TEST_ASSERT((d.set & bset) != 0U);
    TEST_ASSERT((d.style & bbit) != 0U); /* this keyword set bold */
  }
  ra8_css_style_t off = inl("font-weight: normal");
  TEST_ASSERT((off.set & bset) != 0U);
  TEST_ASSERT((off.style & bbit) == 0U); /* all-false arm */
  TEST_END("css font-weight bold keyword 6-OR");
}

/**
 * @test test_font_style_oblique
 * @brief font-style accepts both `italic` and `oblique`; `normal` clears it.
 *
 * @par MC/DC:
 * Decision (priv_apply_emphasis): `ci_eq("italic") || ci_eq("oblique")`
 * (2-cond OR; N+1 = 3):
 *  - "italic"  -> cond1 true  (italic ON).
 *  - "oblique" -> cond1 false, cond2 true (italic ON via the slanted keyword).
 *  - "normal"  -> both false  (italic OFF; the set bit stays recorded).
 */
static void test_font_style_oblique(void)
{
  TEST_BEGIN("css font-style italic/oblique OR");
  const uint8_t   ibit = (uint8_t)k_ra8_reflow_style_italic;
  const uint8_t   iset = (uint8_t)k_ra8_css_set_italic;
  ra8_css_style_t it   = inl("font-style: italic");
  ra8_css_style_t ob   = inl("font-style: oblique");
  ra8_css_style_t no   = inl("font-style: normal");
  TEST_ASSERT((it.style & ibit) != 0U); /* cond1 */
  TEST_ASSERT((ob.style & ibit) != 0U); /* cond2 */
  TEST_ASSERT((no.set & iset) != 0U);
  TEST_ASSERT((no.style & ibit) == 0U); /* all-false */
  TEST_END("css font-style italic/oblique OR");
}

/**
 * @test test_empty_decl_guard
 * @brief Declarations missing a property or value name are skipped.
 *
 * @par MC/DC:
 * Decision (priv_parse_decls / priv_for_each_decl): `(plen > 0) && (vlen > 0)`
 * (2-cond AND; N+1 = 3) over an inline body with three pairs:
 *  - "color: red"   -> plen>0 AND vlen>0 -> true  (applied: colour set).
 *  - ": center"     -> plen==0           -> false (cond1: no property name).
 *  - "text-align: " -> vlen==0           -> false (cond2: no value).
 * The applied pair leaves only `color` set; the two skipped pairs add nothing.
 */
static void test_empty_decl_guard(void)
{
  TEST_BEGIN("css empty-decl guard (plen>0 && vlen>0)");
  ra8_css_style_t d = inl("color: red; : center; text-align: ");
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_color) != 0U); /* control applied */
  TEST_ASSERT_EQ(k_css_color_red, d.color);
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_align) == 0U); /* both skips */
  TEST_END("css empty-decl guard (plen>0 && vlen>0)");
}

/**
 * @test test_name_overflow_rejected
 * @brief A class name longer than k_ra8_css_name_max drops the whole rule.
 *
 * @par MC/DC:
 * Decision (priv_intern_name): `(len == 0) || (len > k_ra8_css_name_max)`
 * (2-cond OR), reached via the selector class-name interning:
 *  - a short ".note" class -> len in [1, 64] -> false (rule kept).
 *  - a 70-char class name  -> len > 64       -> true  (cond2: intern fails ->
 *    priv_parse_sel_part returns false -> the rule is dropped).
 * cond1 (len==0) cannot occur on a class part (the caller requires nlen>0), so
 * the kept-rule vector holds it false while cond2 alone flips the outcome.
 */
static void test_name_overflow_rejected(void)
{
  TEST_BEGIN("css over-long class name rejected (intern guard)");
  char buf[k_css_buf_cap]                 = {};
  char longname[k_css_name_overflow + 1U] = {};
  for (size_t c = 0U; c < (size_t)k_css_name_overflow; ++c) {
    longname[c] = 'a';
  }
  (void)snprintf(buf, sizeof buf, ".note { color: red; } .%s { color: blue; }", longname);
  load(buf);
  /* Only `.note` survives; the over-long `.aaaa...` rule is dropped. */
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT(s_sheet.rules[0].class_len > 0U);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_END("css over-long class name rejected (intern guard)");
}

/**
 * @test test_at_rule_split
 * @brief The block dispatcher routes `@`-prefixed selectors to the at-rule path.
 *
 * @par MC/DC:
 * Decision (priv_parse_one_block): `(tlen > 0) && (tsel[0] == '@')` (2-cond AND;
 * N+1 = 3):
 *  - `@font-face{...}`     -> tlen>0 AND first byte '@' -> true (a face, no rule).
 *  - `p {...}` style rule  -> tlen>0 but first byte 'p' -> cond2 false (a rule).
 *  - `@media ... {...}`    -> tlen>0 AND '@' -> true but a non-font-face at-rule
 *    is skipped (no face, no rule): proves the at-rule branch is taken without
 *    adding either kind of entry.
 * After parsing: exactly one rule (the `p`) and one face (the `@font-face`).
 */
static void test_at_rule_split(void)
{
  TEST_BEGIN("css at-rule split ((tlen>0) && first=='@')");
  load("@font-face{font-family:Body;src:url(b.ttf)}" /* at-rule -> face     */
       "p { color: red; }"                           /* style rule          */
       "@media screen { p { color: blue; } }");      /* at-rule -> skipped  */
  TEST_ASSERT_EQ(1, s_sheet.rule_count);             /* only the `p` rule   */
  TEST_ASSERT_EQ(1, s_sheet.face_count);             /* only the @font-face */
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_END("css at-rule split ((tlen>0) && first=='@')");
}

/**
 * @test test_unterminated_block
 * @brief A rule whose `{` has no closing `}` still parses to end-of-input.
 *
 * @par MC/DC:
 * priv_find_block's close-brace scan `(close < len) && (css[close] != '}')`:
 *  - "p { color: red; }" -> the loop stops at '}' (cond2 false): a complete rule.
 *  - "h1 { color: blue"  (no '}') -> the loop runs to `close == len` (cond1
 *    false at the end): the block body is taken to end-of-input and still
 *    parses, so the trailing rule is captured.
 * Both rules end up in the sheet, proving the unterminated path is handled.
 */
static void test_unterminated_block(void)
{
  TEST_BEGIN("css unterminated block (find_block close scan)");
  load("p { color: red; }\n"
       "h1 { color: blue"); /* no closing brace -> runs to EOF */
  TEST_ASSERT_EQ(2, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ(k_css_color_blue, s_sheet.rules[1].decl.color);
  TEST_END("css unterminated block (find_block close scan)");
}

/**
 * @test test_comment_open_scan
 * @brief Comments are skipped; a lone trailing '/' is harmless non-comment bytes.
 *
 * @par MC/DC:
 * Decision (ra8_css_parse): `((i+1) < len) && (css[i]=='/') && (css[i+1]=='*')`
 * (3-cond AND), the comment-open test:
 *  - a real slash-star comment between two rules -> all three true -> the
 *    comment is skipped and both surrounding rules parse (cond1/2/3 true).
 *  - a trailing lone '/' at end-of-input -> `(i+1) < len` false (cond1): not a
 *    comment open, and (having no following '{') it is consumed as the
 *    end-of-input -- no extra rule, no crash.
 * Reaching the real rules through a mid-stream comment exercises the all-true
 * arm; the trailing '/' exercises the cond1-false arm. Net: two `{...}` rules.
 */
static void test_comment_open_scan(void)
{
  TEST_BEGIN("css comment-open scan ((i+1<len) && '/' && '*')");
  load("p { color: red; }\n"
       "/* between the two rules */\n"
       "h1 { color: blue; }\n"
       "/"); /* trailing lone '/' at EOF: (i+1)<len false */
  TEST_ASSERT_EQ(2, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_h1, s_sheet.rules[1].sel_tag);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ(k_css_color_blue, s_sheet.rules[1].decl.color);
  TEST_END("css comment-open scan ((i+1<len) && '/' && '*')");
}

/**
 * @test test_hex_val_below_ranges
 * @brief A hex byte below '0' and below 'a' isolates the lower-range conditions.
 *
 * @par MC/DC:
 * priv_hex_val has two range tests:
 *   `(c >= '0') && (c <= '9')`  (digit arm)
 *   `(l >= 'a') && (l <= 'f')`  (lower-folded letter arm)
 * The existing suite drives the upper sub-conditions false ('g' > '9', 'g' > 'f')
 * and both-true ('0'..'F'). This isolates the LOWER sub-conditions:
 *  - A '/' byte (0x2F, just below '0') -> `c >= '0'` false (digit cond1 false),
 *    then lower-folded '/' (< 'a') -> `l >= 'a'` false (letter cond1 false).
 * The '/' nibble makes the colour invalid, so `color` is NOT set; the
 * surrounding all-digit nibbles keep the rest of the parse on the valid path.
 */
static void test_hex_val_below_ranges(void)
{
  TEST_BEGIN("css priv_hex_val below-'0' / below-'a' arms");
  const uint8_t cset = (uint8_t)k_ra8_css_set_color;
  /* '/' (0x2F) is below '0' and below 'a': both lower-range conditions go false. */
  ra8_css_style_t bad = inl("color: #0/0");
  TEST_ASSERT((bad.set & cset) == 0U); /* '/' nibble -> invalid -> color unset */
  /* Control: a clean 3-digit colour still parses on the same code path. */
  ra8_css_style_t ok = inl("color: #0a0");
  TEST_ASSERT((ok.set & cset) != 0U);
  TEST_END("css priv_hex_val below-'0' / below-'a' arms");
}

/**
 * @test test_fontsize_frac_end_of_input
 * @brief Fractional digits that run to end-of-input exit the loops via `*i<len`.
 *
 * @par MC/DC:
 * Two loops in priv_scan_hundredths exit when the span ends mid-fraction
 * (the `*i < len` condition, not the digit / fd-cap conditions):
 *  (A) kept loop `(*i<len) && digit && (fd < k_priv_fs_frac)` -- "1.5" (no unit)
 *      keeps '5' then `*i < len` goes false at end-of-input (cond1 false).
 *  (B) skip loop `(*i<len) && digit` -- "1.567" keeps two digits, the skip loop
 *      consumes '7' then `*i < len` goes false at end-of-input (cond1 false).
 * Neither span carries a unit suffix, so priv_parse_fontsize returns false and
 * `font-size` is NOT set; the point is that the scan terminated on `*i < len`.
 */
static void test_fontsize_frac_end_of_input(void)
{
  TEST_BEGIN("css font-size fractional *i<len exits");
  const uint8_t   fset = (uint8_t)k_ra8_css_set_fontsize;
  ra8_css_style_t a    = inl("font-size: 1.5");   /* kept loop ends on *i<len */
  ra8_css_style_t b    = inl("font-size: 1.567"); /* skip loop ends on *i<len */
  TEST_ASSERT((a.set & fset) == 0U);              /* no unit -> not applied   */
  TEST_ASSERT((b.set & fset) == 0U);              /* no unit -> not applied   */
  TEST_END("css font-size fractional *i<len exits");
}

/**
 * @test test_strip_quotes_short_and_unclosed
 * @brief A 1-byte family and an unmatched closing quote are NOT quote-stripped.
 *
 * @par MC/DC:
 * Decision (priv_strip_quotes): `(*len >= 2) && ((s[0]=='"') || (s[0]=='\'')) &&
 *   (s[*len-1] == s[0])` (3-cond AND). The existing suite drives the all-true
 * arm (`"Body"` / `'Body'`) and the quote-OR false arm (bare `Body`). This
 * isolates the remaining two AND conditions:
 *  - family `X` (one byte) -> `*len >= 2` false (cond1 false): kept verbatim, a
 *    valid 1-byte family.
 *  - family `"AB'` (open `"`, close `'`) -> cond1 true, quote-OR true, but
 *    `s[*len-1] != s[0]` (cond3 false): the quotes are NOT stripped, so the
 *    interned family keeps all three literal bytes.
 * Matching on the resolved family length proves which bytes were interned.
 */
static void test_strip_quotes_short_and_unclosed(void)
{
  TEST_BEGIN("css strip_quotes short / unmatched-close arms");
  const uint16_t one  = 1U;
  const uint16_t four = 4U;
  load("@font-face{font-family:X;src:url(x.ttf)}"       /* 1-byte: *len<2   */
       "@font-face{font-family:\"AB';src:url(y.ttf)}"); /* mismatched close */
  TEST_ASSERT_EQ(k_css_face_two, s_sheet.face_count);
  TEST_ASSERT_EQ(one, s_sheet.faces[0].family_len);  /* "X" kept        */
  TEST_ASSERT_EQ(four, s_sheet.faces[1].family_len); /* "AB' kept whole */
  TEST_ASSERT_EQ(0, memcmp(&s_sheet.names[s_sheet.faces[1].family_off], "\"AB'", 4U));
  TEST_END("css strip_quotes short / unmatched-close arms");
}

/**
 * @test test_extract_url_unterminated
 * @brief A `url(` with no closing `)` is scanned to end-of-value.
 *
 * @par MC/DC:
 * Loop (priv_extract_url): `while ((b < vlen) && (val[b] != ')'))`. The existing
 * suite drives the `val[b] != ')'`-false exit (a real `url(a.ttf)`). This
 * isolates the `b < vlen`-false exit:
 *  - `src:url(noclose.ttf` (no `)`) -> the scan runs to `b == vlen` (cond1 false)
 *    and still yields a non-empty href, so the face is accepted.
 * The face is kept (family + a non-empty src), proving the unterminated-url path
 * extracted a usable href.
 */
static void test_extract_url_unterminated(void)
{
  TEST_BEGIN("css extract_url unterminated url( (b < vlen)");
  load("@font-face{font-family:U;src:url(noclose.ttf}");
  TEST_ASSERT_EQ(k_css_face_one, s_sheet.face_count);
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "U", 1U, false, false));
  const char* src  = nullptr;
  uint16_t    slen = 0U;
  TEST_ASSERT(ra8_css_face_src(&s_sheet, 0U, &src, &slen));
  TEST_ASSERT(slen > 0U); /* the unterminated url still produced an href */
  TEST_END("css extract_url unterminated url( (b < vlen)");
}

/**
 * @test test_for_each_decl_empty_pairs
 * @brief Empty @font-face descriptor pairs are skipped by the iterator guard.
 *
 * @par MC/DC:
 * Decision (priv_for_each_decl): `(plen > 0U) && (vlen > 0U)` (2-cond AND),
 * driven over a @font-face block (this iterator is the @font-face / font-family
 * one, distinct from priv_parse_decls). N+1 = 3:
 *  - `font-family:F` -> plen>0 AND vlen>0 -> true (applied: the family is set).
 *  - `:bad`          -> plen == 0 -> cond1 false (skipped, no descriptor).
 *  - `font-weight:`  -> vlen == 0 -> cond2 false (skipped, weight stays 0).
 * The face still has family + src (from the valid pairs), so it is accepted; the
 * empty pairs simply add nothing.
 */
static void test_for_each_decl_empty_pairs(void)
{
  TEST_BEGIN("css @font-face decl iterator (plen>0 && vlen>0)");
  load("@font-face{font-family:F; :bad; font-weight:; src:url(f.ttf)}");
  TEST_ASSERT_EQ(k_css_face_one, s_sheet.face_count);
  TEST_ASSERT_EQ(0, s_sheet.faces[0].weight_bold); /* empty weight stayed 0 */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "F", 1U, false, false));
  TEST_END("css @font-face decl iterator (plen>0 && vlen>0)");
}

/**
 * @test test_block_empty_selector
 * @brief A block with an empty (whitespace-only) selector is not an at-rule.
 *
 * @par MC/DC:
 * Decision (priv_parse_one_block): `(tlen > 0U) && (tsel[0] == '@')` (2-cond
 * AND). The existing suite drives the all-true arm (@font-face) and the
 * `tsel[0] != '@'` arm (a `p` rule). This isolates `tlen == 0`:
 *  - a leading `{ color: red; }` block has an empty selector -> tlen == 0
 *    (cond1 false): the at-rule branch is NOT taken, the block falls through to
 *    the style-rule path, but priv_parse_selector rejects an empty selector so
 *    no rule is added.
 *  - the following `p { ... }` proves parsing continues normally afterwards.
 */
static void test_block_empty_selector(void)
{
  TEST_BEGIN("css empty-selector block (tlen == 0) not at-rule");
  load("{ color: red; }\n"    /* empty selector -> tlen == 0 -> no rule */
       "p { color: blue; }"); /* parsing resumes -> one rule            */
  TEST_ASSERT_EQ(k_css_rule_one, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ(k_css_color_blue, s_sheet.rules[0].decl.color);
  TEST_END("css empty-selector block (tlen == 0) not at-rule");
}

/**
 * @test test_comment_scan_slash_not_star
 * @brief A `/` not followed by `*` is not a comment open (cond3 false).
 *
 * @par MC/DC:
 * Decision (ra8_css_parse): `((i+1)<len) && (css[i]=='/') && (css[i+1]=='*')`
 * (3-cond AND). The existing suite drives the all-true arm (a real comment) and
 * the `(i+1) < len`-false arm (a trailing `/`). This isolates cond3:
 *  - a mid-stream `/x` -> (i+1)<len true, css[i]=='/' true, css[i+1]=='x' != '*'
 *    (cond3 false): NOT a comment, so the byte is handled by the block scanner
 *    (which finds no usable selector here and skips it).
 * The leading `p` rule still parses, proving the non-comment `/` did not derail
 * the parser.
 */
static void test_comment_scan_slash_not_star(void)
{
  TEST_BEGIN("css comment scan '/' not '*' (cond3 false)");
  load("p { color: red; }\n"
       "/x\n" /* '/' not followed by '*' -> not a comment open */
       "h1 { color: blue; }");
  TEST_ASSERT(s_sheet.rule_count >= (uint16_t)k_css_rule_one);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_END("css comment scan '/' not '*' (cond3 false)");
}

/**
 * @test test_fontsize_dot_nondigit_mcdc
 *
 * @par MC/DC:
 * Decisions: the two fractional-digit scan loops in priv_scan_hundredths
 * (libs/ra8_reflow/src/ra8_reflow_css.c) --
 *   `while ((*i<len) && (s[*i]>='0') && (s[*i]<='9') && (fd<k_priv_fs_frac))` and
 *   `while ((*i<len) && (s[*i]>='0') && (s[*i]<='9'))`. Existing vectors drive the
 * digit path; a value whose first post-dot byte is a non-digit BELOW '0' isolates
 * the `(s[*i] >= '0')` false arm of BOTH loops:
 *  - "1.-px": after the '.', the first byte is '-' (0x2D < '0'), so the fractional
 *    loop's second condition is false on entry (kept-digit loop) and the skip loop
 *    likewise sees '-' first -> both `(s[*i] >= '0')` conditions take their false
 *    side. The unit suffix "-px" is unrecognised so font-size stays unset.
 *  - control "16px" leaves the fontsize bit set. Length independence pairs with
 *    the existing all-digit vectors.
 */
static void test_fontsize_dot_nondigit_mcdc(void)
{
  TEST_BEGIN("css font-size MC/DC: non-digit right after the decimal point");
  load("p { font-size: 1.-px; } h1 { font-size: 16px; }");
  TEST_ASSERT_EQ(2, s_sheet.rule_count);
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra8_css_set_fontsize) == 0U);
  TEST_ASSERT((s_sheet.rules[1].decl.set & (uint8_t)k_ra8_css_set_fontsize) != 0U);
  TEST_END("css font-size MC/DC: non-digit right after the decimal point");
}

/**
 * @test test_intern_name_too_long_arms_mcdc
 *
 * @par MC/DC:
 * Decisions: the three `intern-succeeds` gates whose second condition
 * (priv_intern_name result) only takes its false side when a name overruns
 * k_ra8_css_name_max (libs/ra8_reflow/src/ra8_reflow_css_rules.c):
 *   - priv_face_apply font-family: `(n > 0U) && priv_intern_name(...)`.
 *   - priv_face_apply src:         `priv_extract_url(...) && priv_intern_name(...)`.
 *   - priv_family_cb:              `(n > 0U) && priv_intern_name(...)`.
 * Each existing vector drives the true side (short name interns). A name longer
 * than k_ra8_css_name_max makes priv_intern_name return false with the first
 * condition still true, completing every second-condition independence pair:
 *  - @font-face family over-long -> family not interned -> the accept guard drops
 *    the face (face_count stays at the control count).
 *  - @font-face src url over-long -> src not interned -> the face is likewise dropped.
 *  - a normal rule `font-family:` over-long -> the family bit is never set on the rule.
 */
static void test_intern_name_too_long_arms_mcdc(void)
{
  TEST_BEGIN("css MC/DC: over-long name defeats intern (face family/src, rule family)");
  /* A name comfortably longer than k_ra8_css_name_max (64) bytes. */
  const char* const k_long =
    "Overlong-css-name-that-comfortably-exceeds-the-sixty-four-byte-name-pool-cap-000";
  char css[k_css_big_buf];
  (void)snprintf(css,
                 sizeof css,
                 "@font-face{font-family:Ok;src:url(o.ttf)}"  /* control face, index 0    */
                 "@font-face{font-family:%s;src:url(g.ttf)}"  /* family over-long -> drop */
                 "@font-face{font-family:Nm;src:url(%s.ttf)}" /* src over-long   -> drop  */
                 "p{font-family:%s}",                         /* rule family over-long    */
                 k_long,
                 k_long,
                 k_long);
  load(css);
  /* Only the single control @font-face survives (the two over-long ones dropped). */
  TEST_ASSERT_EQ(1, s_sheet.face_count);
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Ok", 2U, false, false));
  /* The `p` rule parsed, but its over-long font-family was rejected (no family bit). */
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra8_css_set_family) == 0U);
  TEST_END("css MC/DC: over-long name defeats intern (face family/src, rule family)");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_whitespace_variants,
  test_ci_eq_length_tiebreak,
  test_ci_contains_short_span,
  test_hex_val_ranges,
  test_parse_color_grey_and_bad_hex,
  test_fontsize_fractional_loops,
  test_font_weight_keywords,
  test_font_style_oblique,
  test_empty_decl_guard,
  test_name_overflow_rejected,
  test_at_rule_split,
  test_unterminated_block,
  test_comment_open_scan,
  test_hex_val_below_ranges,
  test_fontsize_frac_end_of_input,
  test_strip_quotes_short_and_unclosed,
  test_extract_url_unterminated,
  test_for_each_decl_empty_pairs,
  test_block_empty_selector,
  test_comment_scan_slash_not_star,
  test_fontsize_dot_nondigit_mcdc,
  test_intern_name_too_long_arms_mcdc,
};

/**
 * @brief Test executable entry point -- runs the parser MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every parse-side decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_css_parse_mcdc.c\n");
  return 0;
}

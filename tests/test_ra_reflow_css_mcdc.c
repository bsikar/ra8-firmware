/**
 * @file test_ra_reflow_css_mcdc.c
 * @brief Supplemental MC/DC host tests for the content-CSS cascade (#111/#140).
 *
 * @details
 * Drives the REAL `ra_reflow_css` parser / matcher / cascade through the
 * compound boolean decisions that `test_ra_css.c` leaves uncovered, so each
 * condition is exercised both true and false. Every test reaches the target
 * decision through a public entry point (::ra_css_parse, ::ra_css_parse_inline,
 * ::ra_css_cascade, ::ra_css_cascade_ctx, ::ra_css_rule_matches,
 * ::ra_css_match_face, ::ra_css_face_src) with crafted CSS / element input;
 * there are no mirror functions, so llvm-cov sees the real source lines.
 *
 * Decisions targeted (one or more vectors each):
 *  - priv_is_ws         : the 5-way whitespace OR (space/tab/newline/CR/FF).
 *  - priv_ci_eq         : the `(k == len) && (lit[k] == 0)` length tie-break.
 *  - priv_ci_contains   : the `sub longer than span` early-out.
 *  - priv_hex_val       : the digit-range and a-f-range arms.
 *  - priv_parse_color   : the empty-span guard and the gray/grey OR.
 *  - priv_scan_hundredths: the fractional-digit cap + skip-the-rest loops.
 *  - priv_apply_emphasis: the 6-way font-weight OR and the italic/oblique OR.
 *  - priv_parse_decls   : the `(plen > 0) && (vlen > 0)` empty-decl guard.
 *  - priv_intern_name   : the over-long-name reject (drops a selector / family).
 *  - priv_parse_sel_part: the empty-name / intern-fail reject.
 *  - priv_parse_selector: the `combinator/pseudo -> drop` part loop.
 *  - priv_strip_quotes  : the matched `'`/`"` quote-strip decision.
 *  - priv_is_bold_kw    : the 6-way @font-face weight OR.
 *  - priv_is_italic_kw  : the @font-face italic/oblique OR.
 *  - priv_face_apply    : the family / src present-and-interned guards.
 *  - priv_parse_fontface: the `family && src` accept guard (both arms).
 *  - priv_parse_one_block: the `(tlen > 0) && (tsel[0] == '@')` at-rule split.
 *  - priv_find_block     : the no-close-brace (unterminated block) arm.
 *  - ra_css_parse        : the `i+1 < len && '/' && '*'` comment-open scan.
 *  - priv_anc_matches    : the ancestor tag / class / id constraint arms.
 *  - priv_resolve        : the winner `rank == best && order >= best_order` arm.
 *  - ra_css_match_face   : the regular-fallback `(!bold && !italic && fb<0)` arm.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_reflow.h"
#include "ra_reflow_css.h"
#include "unity_minimal.h"

/** @brief Shared parsed stylesheet (large -- keep off the test stack). */
static ra_css_sheet_t s_sheet;

/**
 * @enum css_test_consts_t
 * @brief Small named literals for the supplemental CSS MC/DC tests.
 */
typedef enum : uint16_t {
  k_css_name_overflow = 70U,  /**< > k_ra_css_name_max (64) -> intern rejects. */
  k_css_buf_cap       = 256U, /**< Scratch CSS / declaration buffer capacity.  */
  k_css_fs_156        = 156U, /**< 1.567em -> 156% (3rd frac digit dropped).   */
  k_css_fs_150        = 150U, /**< 1.5em -> 150%.                              */
} css_test_consts_t;

/**
 * @enum css_test_color_t
 * @brief Expected 0xRRGGBB results for the named-colour assertions.
 */
typedef enum : uint32_t {
  k_css_color_navy = 0x000080U, /**< Expected RGB of the CSS `navy` keyword.  */
  k_css_color_gray = 0x808080U, /**< Expected RGB of `gray` / `grey`.         */
  k_css_color_red  = 0xFF0000U, /**< Expected RGB of the CSS `red` keyword.   */
  k_css_color_blue = 0x0000FFU, /**< Expected RGB of the CSS `blue` keyword.  */
} css_test_color_t;

/** @brief Parse @p css into the shared sheet (reset first); assert success. */
static void load(const char* css)
{
  TEST_ASSERT_EQ(k_ra_ok, ra_css_sheet_reset(&s_sheet));
  TEST_ASSERT_EQ(k_ra_ok, ra_css_parse(&s_sheet, css, (uint32_t)strlen(css)));
}

/** @brief Build an element identity (NULL id/class allowed). */
static ra_css_element_t elem(ra_reflow_html_tag_t tag, const char* id, const char* cls)
{
  ra_css_element_t e = {};
  e.tag              = (uint8_t)tag;
  e.id               = id;
  e.id_len           = (id != nullptr) ? (uint16_t)strlen(id) : 0U;
  e.class_str        = cls;
  e.class_len        = (cls != nullptr) ? (uint16_t)strlen(cls) : 0U;
  return e;
}

/** @brief The empty inline declaration (nothing set). */
static ra_css_style_t no_inline(void)
{
  return (ra_css_style_t){};
}

/** @brief Parse an inline `prop: value` body and return the declaration. */
static ra_css_style_t inl(const char* decls)
{
  ra_css_style_t d = {};
  (void)ra_css_parse_inline(decls, (uint32_t)strlen(decls), &d);
  return d;
}

/**
 * @test test_whitespace_variants
 * @brief Every CSS whitespace byte is accepted by the top-level scanner + trim.
 *
 * @par MC/DC:
 * Decision: `priv_is_ws(c)` =
 *   `(c==' ') || (c=='\t') || (c=='\n') || (c=='\r') || (c=='\f')` (5-cond OR;
 * reached from ::ra_css_parse's inter-rule skip and ::priv_trim). N+1 = 6:
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
  TEST_ASSERT_EQ(1, (int)s_sheet.rule_count);
  TEST_ASSERT_EQ((int)k_ra_reflow_tag_p, (int)s_sheet.rules[0].sel_tag);
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra_css_set_align) != 0U);
  TEST_ASSERT_EQ((int)k_ra_reflow_align_center, (int)s_sheet.rules[0].decl.align);
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
  const uint8_t cset = (uint8_t)k_ra_css_set_color;
  load("p { color: navy; }"   /* exact -> set */
       "h1 { color: navyy; }" /* longer than keyword -> unset */
       "h2 { color: nav; }"); /* shorter prefix -> unset */
  TEST_ASSERT_EQ(3, (int)s_sheet.rule_count);
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ((int)k_css_color_navy, (int)s_sheet.rules[0].decl.color);
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
  const uint8_t  ubit = (uint8_t)k_ra_reflow_style_underline;
  ra_css_style_t on   = inl("text-decoration: underline");
  ra_css_style_t off  = inl("text-decoration: none");
  TEST_ASSERT((on.set & (uint8_t)k_ra_css_set_underline) != 0U);
  TEST_ASSERT((on.style & ubit) != 0U); /* "underline" found     */
  TEST_ASSERT((off.set & (uint8_t)k_ra_css_set_underline) != 0U);
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
  const uint8_t cset = (uint8_t)k_ra_css_set_color;
  load("p { color: #012345; }"  /* digits   */
       "h1 { color: #ABCDEF; }" /* letters  */
       "h2 { color: #0g0; }");  /* bad nibble -> reject */
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(0x012345, (int)s_sheet.rules[0].decl.color);
  TEST_ASSERT((s_sheet.rules[1].decl.set & cset) != 0U);
  TEST_ASSERT_EQ(0xABCDEF, (int)s_sheet.rules[1].decl.color);
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
  const uint8_t cset = (uint8_t)k_ra_css_set_color;
  load("p { color: red; }" /* control: parses, gray/grey OR both false */
       "h1 { color: gray; }"
       "h2 { color: grey; }");
  TEST_ASSERT((s_sheet.rules[0].decl.set & cset) != 0U);
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ((int)k_css_color_gray, (int)s_sheet.rules[1].decl.color); /* cond1 */
  TEST_ASSERT_EQ((int)k_css_color_gray, (int)s_sheet.rules[2].decl.color); /* cond2 */
  /* A '#' with no hex digits falls through the 6-/3-digit length dispatch. */
  ra_css_style_t hash = inl("color: #");
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
  const uint8_t  fset = (uint8_t)k_ra_css_set_fontsize;
  ra_css_style_t a    = inl("font-size: 1.567em");
  TEST_ASSERT((a.set & fset) != 0U);
  TEST_ASSERT_EQ((int)k_css_fs_156, (int)a.font_val); /* 3rd frac digit dropped */
  TEST_ASSERT_EQ((int)k_ra_css_font_pct, (int)a.font_unit);
  ra_css_style_t b = inl("font-size: 1.5em");
  TEST_ASSERT_EQ((int)k_css_fs_150, (int)b.font_val); /* exact, no skip loop */
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
  const uint8_t      bbit               = (uint8_t)k_ra_reflow_style_bold;
  const uint8_t      bset               = (uint8_t)k_ra_css_set_bold;
  static const char* k_bold_kw[6]       = {"bold", "bolder", "600", "700", "800", "900"};
  char               buf[k_css_buf_cap] = {};
  for (size_t w = 0U; w < (sizeof(k_bold_kw) / sizeof(k_bold_kw[0])); ++w) {
    (void)snprintf(buf, sizeof buf, "font-weight: %s", k_bold_kw[w]);
    ra_css_style_t d = inl(buf);
    TEST_ASSERT((d.set & bset) != 0U);
    TEST_ASSERT((d.style & bbit) != 0U); /* this keyword set bold */
  }
  ra_css_style_t off = inl("font-weight: normal");
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
  const uint8_t  ibit = (uint8_t)k_ra_reflow_style_italic;
  const uint8_t  iset = (uint8_t)k_ra_css_set_italic;
  ra_css_style_t it   = inl("font-style: italic");
  ra_css_style_t ob   = inl("font-style: oblique");
  ra_css_style_t no   = inl("font-style: normal");
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
  ra_css_style_t d = inl("color: red; : center; text-align: ");
  TEST_ASSERT((d.set & (uint8_t)k_ra_css_set_color) != 0U); /* control applied */
  TEST_ASSERT_EQ((int)k_css_color_red, (int)d.color);
  TEST_ASSERT((d.set & (uint8_t)k_ra_css_set_align) == 0U); /* both skips */
  TEST_END("css empty-decl guard (plen>0 && vlen>0)");
}

/**
 * @test test_name_overflow_rejected
 * @brief A class name longer than k_ra_css_name_max drops the whole rule.
 *
 * @par MC/DC:
 * Decision (priv_intern_name): `(len == 0) || (len > k_ra_css_name_max)`
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
  TEST_ASSERT_EQ(1, (int)s_sheet.rule_count);
  TEST_ASSERT(s_sheet.rules[0].class_len > 0U);
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
  TEST_END("css over-long class name rejected (intern guard)");
}

/**
 * @test test_sel_part_combinator_drop
 * @brief A child / sibling combinator selector is unsupported -> dropped.
 *
 * @par MC/DC:
 * Decision (priv_parse_selector part loop):
 *   `((s[i] != '.') && (s[i] != '#')) || !priv_parse_sel_part(...)` (2-cond OR):
 *  - "p.note" -> at the '.' both `s[i]!='.'`/`s[i]!='#'` are false AND the part
 *    parses (intern OK) -> whole disjunction false -> rule kept (control).
 *  - "p>span" -> at '>' both `s[i]!='.'` and `s[i]!='#'` are true -> cond1 true
 *    -> return false -> rule dropped (isolates the combinator condition).
 *  - "p.a.b"  -> the second '.b' is a `.` (cond1 false) but priv_parse_sel_part
 *    fails on the duplicate class -> `!part` true -> rule dropped (isolates the
 *    intern/dup condition).
 */
static void test_sel_part_combinator_drop(void)
{
  TEST_BEGIN("css selector part loop OR (combinator / dup-class drop)");
  load("p.note { color: red; }"   /* kept    */
       "p>span { color: blue; }"  /* '>' combinator -> dropped */
       "p.a.b { color: navy; }"); /* second class -> dropped   */
  TEST_ASSERT_EQ(1, (int)s_sheet.rule_count);
  TEST_ASSERT_EQ((int)k_ra_reflow_tag_p, (int)s_sheet.rules[0].sel_tag);
  TEST_ASSERT(s_sheet.rules[0].class_len > 0U);
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
  TEST_END("css selector part loop OR (combinator / dup-class drop)");
}

/**
 * @test test_fontface_quote_strip
 * @brief @font-face family quotes (both `'` and `"`) are stripped; bare too.
 *
 * @par MC/DC:
 * Decision (priv_strip_quotes):
 *   `(*len>=2) && ((s[0]=='"') || (s[0]=='\'')) && (s[*len-1]==s[0])` (AND of
 * three, with an inner OR). Vectors:
 *  - "\"Body\"" -> len>=2 AND (double-quote) AND closing matches -> stripped to
 *    "Body" (the AND-true / OR-cond1 arm).
 *  - 'Body'     -> the single-quote OR-cond2 arm; stripped to "Body".
 *  - Body       -> s[0]=='B' -> the quote OR is false -> not stripped (still a
 *    valid family name, kept verbatim).
 * Matching on the resolved family proves which bytes were interned.
 */
static void test_fontface_quote_strip(void)
{
  TEST_BEGIN("css @font-face quote strip (\" / ' / bare)");
  load("@font-face{font-family:\"Body\";src:url(a.ttf)}"
       "@font-face{font-family:'Body';src:url(b.ttf)}"
       "@font-face{font-family:Body;src:url(c.ttf)}");
  TEST_ASSERT_EQ(3, (int)s_sheet.face_count);
  /* All three faces resolve to the 4-byte family "Body" (quotes removed). */
  for (uint16_t i = 0U; i < s_sheet.face_count; ++i) {
    TEST_ASSERT_EQ(4, (int)s_sheet.faces[i].family_len);
    TEST_ASSERT_EQ(0, memcmp(&s_sheet.names[s_sheet.faces[i].family_off], "Body", 4U));
  }
  TEST_END("css @font-face quote strip (\" / ' / bare)");
}

/**
 * @test test_fontface_weight_style_keywords
 * @brief @font-face weight (6-OR) and style (2-OR) descriptor keywords.
 *
 * @par MC/DC:
 * priv_is_bold_kw: `bold||bolder||600||700||800||900` and
 * priv_is_italic_kw: `italic||oblique`, reached via @font-face descriptors:
 *  - a face with `font-weight:900`     -> weight_bold=1 (bold OR cond6).
 *  - a face with `font-weight:bolder`  -> weight_bold=1 (bold OR cond2).
 *  - a face with `font-style:oblique`  -> style_italic=1 (italic OR cond2).
 *  - a face with `font-weight:normal`  -> weight_bold=0 (bold OR all-false).
 * ra_css_match_face then proves each face's recorded emphasis selects it.
 */
static void test_fontface_weight_style_keywords(void)
{
  TEST_BEGIN("css @font-face weight/style keyword ORs");
  load("@font-face{font-family:F;font-weight:900;src:url(w9.ttf)}"
       "@font-face{font-family:G;font-weight:bolder;src:url(wb.ttf)}"
       "@font-face{font-family:H;font-style:oblique;src:url(ho.ttf)}"
       "@font-face{font-family:N;font-weight:normal;src:url(nn.ttf)}");
  TEST_ASSERT_EQ(4, (int)s_sheet.face_count);
  TEST_ASSERT_EQ(1, (int)s_sheet.faces[0].weight_bold);  /* 900    */
  TEST_ASSERT_EQ(1, (int)s_sheet.faces[1].weight_bold);  /* bolder */
  TEST_ASSERT_EQ(1, (int)s_sheet.faces[2].style_italic); /* oblique */
  TEST_ASSERT_EQ(0, (int)s_sheet.faces[3].weight_bold);  /* normal -> not bold */
  /* The bold faces are pickable as bold; the normal face is the regular weight. */
  TEST_ASSERT_EQ(0, (int)ra_css_match_face(&s_sheet, "F", 1U, true, false));
  TEST_ASSERT_EQ(2, (int)ra_css_match_face(&s_sheet, "H", 1U, false, true));
  TEST_ASSERT_EQ(3, (int)ra_css_match_face(&s_sheet, "N", 1U, false, false));
  TEST_END("css @font-face weight/style keyword ORs");
}

/**
 * @test test_fontface_accept_guard
 * @brief A @font-face is kept only when BOTH family and src are present.
 *
 * @par MC/DC:
 * Decision (priv_parse_fontface): `(family_len != 0) && (src_len != 0)`
 * (2-cond AND; N+1 = 3):
 *  - family + src present     -> both true  -> face kept (control).
 *  - family present, no src   -> cond2 false -> face dropped.
 *  - src present, no family    -> cond1 false -> face dropped.
 * Only the complete declaration survives in the face table.
 */
static void test_fontface_accept_guard(void)
{
  TEST_BEGIN("css @font-face accept guard (family && src)");
  load("@font-face{font-family:Keep;src:url(k.ttf)}" /* both -> kept   */
       "@font-face{font-family:NoSrc}"               /* no src -> drop */
       "@font-face{src:url(nofam.ttf)}");            /* no family -> drop */
  TEST_ASSERT_EQ(1, (int)s_sheet.face_count);
  TEST_ASSERT_EQ(0, (int)ra_css_match_face(&s_sheet, "Keep", 4U, false, false));
  TEST_ASSERT_EQ((int)k_ra_css_no_face,
                 (int)ra_css_match_face(&s_sheet, "NoSrc", 5U, false, false));
  TEST_END("css @font-face accept guard (family && src)");
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
  load("@font-face{font-family:Body;src:url(b.ttf)}" /* at-rule -> face */
       "p { color: red; }"                           /* style rule      */
       "@media screen { p { color: blue; } }");      /* at-rule -> skipped */
  TEST_ASSERT_EQ(1, (int)s_sheet.rule_count);        /* only the `p` rule  */
  TEST_ASSERT_EQ(1, (int)s_sheet.face_count);        /* only the @font-face */
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
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
  TEST_ASSERT_EQ(2, (int)s_sheet.rule_count);
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ((int)k_css_color_blue, (int)s_sheet.rules[1].decl.color);
  TEST_END("css unterminated block (find_block close scan)");
}

/**
 * @test test_comment_open_scan
 * @brief Comments are skipped; a lone trailing '/' is harmless non-comment bytes.
 *
 * @par MC/DC:
 * Decision (ra_css_parse): `((i+1) < len) && (css[i]=='/') && (css[i+1]=='*')`
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
  TEST_ASSERT_EQ(2, (int)s_sheet.rule_count);
  TEST_ASSERT_EQ((int)k_ra_reflow_tag_p, (int)s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ((int)k_ra_reflow_tag_h1, (int)s_sheet.rules[1].sel_tag);
  TEST_ASSERT_EQ((int)k_css_color_red, (int)s_sheet.rules[0].decl.color);
  TEST_ASSERT_EQ((int)k_css_color_blue, (int)s_sheet.rules[1].decl.color);
  TEST_END("css comment-open scan ((i+1<len) && '/' && '*')");
}

/**
 * @test test_ancestor_constraint_arms
 * @brief Descendant ancestor parts match by tag, by class, and by id.
 *
 * @par MC/DC:
 * priv_anc_matches has three constraint arms, each a 2-condition decision:
 *   tag  : `(anc->tag != unknown) && (el->tag != anc->tag)`
 *   class: `(anc->class_len != 0) && (class-list lacks the name)`
 *   id   : `(anc->id_len != 0) && (id mismatch)`
 * Three rules, each constraining a different ancestor facet, are matched against
 * an ancestor that satisfies it (true outcome) and one that does not (false).
 * `blockquote` is used as the block ancestor tag (the tokenizer classifies it;
 * `div` is not a recognised tag and would drop the rule):
 *  - `blockquote p` : blockquote ancestor -> tag arm passes; h1 -> tag fails.
 *  - `.box p`       : ancestor .box -> class arm passes; ancestor .other fails.
 *  - `#main p`      : ancestor #main -> id arm passes; ancestor #side -> fails.
 */
static void test_ancestor_constraint_arms(void)
{
  TEST_BEGIN("css descendant ancestor tag/class/id arms");
  const ra_css_style_t   inh  = {};
  const ra_css_element_t p    = elem(k_ra_reflow_tag_p, nullptr, nullptr);
  const uint8_t          bset = (uint8_t)k_ra_css_set_bold;

  /* Tag-ancestor `blockquote p` -- blockquote matches (bold), h1 does not. */
  load("blockquote p { font-weight: bold; }");
  const ra_css_element_t a_bq[1] = {elem(k_ra_reflow_tag_blockquote, nullptr, nullptr)};
  const ra_css_element_t a_h1[1] = {elem(k_ra_reflow_tag_h1, nullptr, nullptr)};
  ra_css_style_t         t_ok    = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_bq, 1U);
  ra_css_style_t         t_no    = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_h1, 1U);
  TEST_ASSERT((t_ok.set & bset) != 0U);
  TEST_ASSERT((t_no.set & bset) == 0U);

  /* Class-ancestor `.box p` -- .box matches, .other does not. */
  load(".box p { font-weight: bold; }");
  const ra_css_element_t a_box[1]   = {elem(k_ra_reflow_tag_blockquote, nullptr, "box")};
  const ra_css_element_t a_other[1] = {elem(k_ra_reflow_tag_blockquote, nullptr, "other")};
  ra_css_style_t         c_ok       = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_box, 1U);
  ra_css_style_t         c_no = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_other, 1U);
  TEST_ASSERT((c_ok.set & bset) != 0U);
  TEST_ASSERT((c_no.set & bset) == 0U);

  /* Id-ancestor `blockquote#main p` -- #main matches, #side does not. */
  load("blockquote#main p { font-weight: bold; }");
  const ra_css_element_t a_main[1] = {elem(k_ra_reflow_tag_blockquote, "main", nullptr)};
  const ra_css_element_t a_side[1] = {elem(k_ra_reflow_tag_blockquote, "side", nullptr)};
  ra_css_style_t         i_ok      = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_main, 1U);
  ra_css_style_t         i_no      = ra_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_side, 1U);
  TEST_ASSERT((i_ok.set & bset) != 0U);
  TEST_ASSERT((i_no.set & bset) == 0U);
  TEST_END("css descendant ancestor tag/class/id arms");
}

/**
 * @test test_resolve_order_tiebreak
 * @brief Two equal-specificity rules: the later one wins the order tie-break.
 *
 * @par MC/DC:
 * Decision (priv_resolve winner): `(!have) || (rank > best_rank) ||
 *   ((rank == best_rank) && (order >= best_order))` -- this targets the third
 * disjunct's inner `(rank == best_rank) && (order >= best_order)`:
 *  - first `p { color: red }` -> !have true (seeds best with red, order 0).
 *  - second `p { color: blue }` -> have set, rank == best_rank (both type-only),
 *    order(1) >= best_order(0) -> inner AND true -> blue overrides red.
 * The result is blue: the later same-specificity rule wins, exercising the
 * `rank == best_rank` true arm and the `order >= best_order` true arm together.
 */
static void test_resolve_order_tiebreak(void)
{
  TEST_BEGIN("css resolve order tie-break (rank==best && order>=best)");
  load("p { color: red; } p { color: blue; }");
  ra_css_element_t p = elem(k_ra_reflow_tag_p, nullptr, nullptr);
  ra_css_style_t   r = ra_css_cascade(&s_sheet, &p, no_inline(), no_inline());
  TEST_ASSERT((r.set & (uint8_t)k_ra_css_set_color) != 0U);
  TEST_ASSERT_EQ((int)k_css_color_blue, (int)r.color); /* later same-spec wins */
  TEST_END("css resolve order tie-break (rank==best && order>=best)");
}

/**
 * @test test_match_face_regular_fallback
 * @brief A bold-only family with no regular face yields no regular fallback.
 *
 * @par MC/DC:
 * Decision (ra_css_match_face fallback record):
 *   `(weight_bold == 0) && (style_italic == 0) && (fallback < 0)` (3-cond AND),
 * driven against two families:
 *  (A) "Reg" has a single non-bold non-italic face:
 *      - asking (bold, italic) finds no exact match, but the regular face sets
 *        the fallback (all three true: weight 0, style 0, fallback still <0) ->
 *        returns the regular face index.
 *  (B) "BoldOnly" has a single bold face:
 *      - asking (italic-only) finds no exact match AND the face is bold so
 *        `weight_bold == 0` is false (cond1) -> no fallback recorded -> no-face.
 *      This isolates cond1 against (A)'s all-true control.
 */
static void test_match_face_regular_fallback(void)
{
  TEST_BEGIN("css match_face regular fallback (3-AND record)");
  load("@font-face{font-family:Reg;src:url(r.ttf)}"
       "@font-face{font-family:BoldOnly;font-weight:bold;src:url(b.ttf)}");
  /* (A): bold+italic request on a regular-only family -> regular fallback (0). */
  TEST_ASSERT_EQ(0, (int)ra_css_match_face(&s_sheet, "Reg", 3U, true, true));
  /* (B): italic request on a bold-only family -> no regular face -> no-face. */
  TEST_ASSERT_EQ((int)k_ra_css_no_face,
                 (int)ra_css_match_face(&s_sheet, "BoldOnly", 8U, false, true));
  TEST_END("css match_face regular fallback (3-AND record)");
}

/**
 * @test test_rule_matches_null_arms
 * @brief ra_css_rule_matches rejects each NULL pointer arm independently.
 *
 * @par MC/DC:
 * Decision: `(rule == NULL) || (el == NULL) || (sheet == NULL)` (3-cond OR;
 * N+1 = 4):
 *  - all non-NULL              -> false (control: a real match is computed).
 *  - rule == NULL              -> cond1 true  -> false return.
 *  - el == NULL                -> cond2 true  -> false return.
 *  - sheet == NULL             -> cond3 true  -> false return.
 */
static void test_rule_matches_null_arms(void)
{
  TEST_BEGIN("css rule_matches null guard 3-OR");
  load("p { color: red; }");
  const ra_css_rule_t* r  = &s_sheet.rules[0];
  ra_css_element_t     el = elem(k_ra_reflow_tag_p, nullptr, nullptr);
  TEST_ASSERT(ra_css_rule_matches(r, &el, &s_sheet));        /* control true   */
  TEST_ASSERT(!ra_css_rule_matches(nullptr, &el, &s_sheet)); /* cond1 */
  TEST_ASSERT(!ra_css_rule_matches(r, nullptr, &s_sheet));   /* cond2 */
  TEST_ASSERT(!ra_css_rule_matches(r, &el, nullptr));        /* cond3 */
  TEST_END("css rule_matches null guard 3-OR");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_whitespace_variants();
  test_ci_eq_length_tiebreak();
  test_ci_contains_short_span();
  test_hex_val_ranges();
  test_parse_color_grey_and_bad_hex();
  test_fontsize_fractional_loops();
  test_font_weight_keywords();
  test_font_style_oblique();
  test_empty_decl_guard();
  test_name_overflow_rejected();
  test_sel_part_combinator_drop();
  test_fontface_quote_strip();
  test_fontface_weight_style_keywords();
  test_fontface_accept_guard();
  test_at_rule_split();
  test_unterminated_block();
  test_comment_open_scan();
  test_ancestor_constraint_arms();
  test_resolve_order_tiebreak();
  test_match_face_regular_fallback();
  test_rule_matches_null_arms();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_css_mcdc.c\n");
  return 0;
}

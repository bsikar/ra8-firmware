/**
 * @file test_ra8_reflow_svg_scan_mcdc.c
 * @brief MC/DC tests for the SVG scanner primitives in libs/ra8_reflow.
 *
 * @details
 * Split sibling of test_ra8_reflow_svg_shape_mcdc.c and
 * test_ra8_reflow_svg_paint_mcdc.c covering the byte-level scanner decision
 * families of the SVG parser: the white-space class, the hex nibble and
 * `#rgb` / `#rrggbb` colour scanners, the integer and float number scanners
 * (sign, fraction, and boundary arms), the attribute scanner (quote forms,
 * whitespace before values, boundary rejects), the case-insensitive
 * keyword compare, and the leading-number classifier. Every test drives the
 * real public SVG API with crafted byte strings; the shared framebuffer
 * fixture lives in tests/support/reflow_svg_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "support/reflow_svg_test_util.h"
#include "unity_minimal.h"

/**
 * @test test_ws_class_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_ws` ->
 * `(c==' ')||(c=='\t')||(c=='\n')||(c=='\r')||(c=='\f')` (5 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_ws). Driven through the real number
 * scanner: each character class appears as a coordinate separator inside a
 * `points` list, so `priv_ws` returns true on each in turn; an ordinary digit
 * separator (comma, then a glyph) keeps every condition false.
 *
 * Vectors (N+1 = 6 for N=5):
 *  - V1: space   -> C1 T (others F by short-circuit control).
 *  - V2: tab     -> C1 F, C2 T.
 *  - V3: newline -> C1,C2 F, C3 T.
 *  - V4: CR      -> C1..C3 F, C4 T.
 *  - V5: FF      -> C1..C4 F, C5 T.
 *  - V6: 'z'     -> all F -> decision F (loop stops treating it as whitespace).
 * Each whitespace vector flips exactly one condition true with the lower-index
 * conditions held false (the OR short-circuit control); V6 holds all false.
 */
static void test_ws_class_mcdc(void)
{
  TEST_BEGIN("priv_ws MC/DC: space/tab/nl/cr/ff separators");
  fb_reset();
  /* A filled triangle whose `points` separators span every whitespace class
   * (space, tab, newline, carriage-return, form-feed) plus a comma. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90\t10\n50\r90\f10,10\" "
                        "fill=\"#00aacc\"/></svg>"));
  /* Triangle fb-vertices (20,20)(180,20)(100,180): at fb-y=60 the span is
   * x in [40,160], so the centre is filled with the parsed colour. Reaching
   * it means every whitespace separator was consumed by priv_ws. */
  TEST_ASSERT_EQ(0x00AACC, px(100, 60)); /* inside the filled triangle     */
  TEST_ASSERT_EQ(k_white, px(10, 60));   /* left of the left edge -> white */
  TEST_END("priv_ws MC/DC: space/tab/nl/cr/ff separators");
}

/**
 * @test test_hex_digit_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_hex` digit guard `(c>='0')&&(c<='9')` and the letter guard
 * `(l>='a')&&(l<='f')` (libs/ra8_reflow/src/ra8_reflow_svg.c@priv_hex). A
 * `#rrggbb` colour drives a hex digit ('0'..'9'), a hex letter ('a'..'f',
 * upper-cased through priv_lc), and -- via a malformed `#` colour with a
 * non-hex glyph -- the "not a hex digit" fall-through that returns
 * k_svg_no_paint (so the shape is skipped, leaving the background).
 *
 * Vectors:
 *  - V1: `#0a0b0c` -> digit branch T for the '0'/'b'/'c' nibbles AND letter
 *        branch T for 'a' -> a valid colour fills.
 *  - V2: `#0g0000` -> 'g' fails both guards -> k_svg_no_paint -> skipped.
 * V1 vs V2 vary the digit/letter classification of the second nibble, flipping
 * "valid colour" vs "skip".
 */
static void test_hex_digit_mcdc(void)
{
  TEST_BEGIN("priv_hex MC/DC: digit vs a-f vs invalid nibble");
  fb_reset();
  /* V1: lower-case hex letters + digits -> a concrete colour. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0a0b0c\"/></svg>"));
  TEST_ASSERT_EQ(0x0A0B0C, px(100, 100));

  /* V2: an invalid nibble 'g' -> no_paint -> the rect is skipped (stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0g0000\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("priv_hex MC/DC: digit vs a-f vs invalid nibble");
}

/**
 * @test test_hex3_shorthand
 * @brief A `#rgb` shorthand expands each nibble (priv_hex_color 3-digit arm),
 *        and an invalid shorthand nibble falls through to skip.
 *
 * @par MC/DC:
 * Decision: `if (len == (size_t)k_svg_hex3)` -- the 3-digit shorthand arm of
 * `priv_hex_color` (libs/ra8_reflow/src/ra8_reflow_svg.c) -- combined with
 * the per-nibble validity fall-through.
 * - V1: `#abc` -> shorthand arm TRUE, all nibbles valid -> 0xAABBCC fills.
 * - V2: `#zzz` -> shorthand arm TRUE, first nibble invalid -> no_paint (skip).
 * - V3: `#0a0b0c` (test_hex_digit_mcdc) -> shorthand arm FALSE (6-digit arm).
 * V1 vs V3 vary the length condition; V1 vs V2 vary the nibble validity.
 */
static void test_hex3_shorthand(void)
{
  TEST_BEGIN("svg #rgb shorthand + invalid shorthand");
  fb_reset();
  /* #abc -> 0xAABBCC. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#abc\"/></svg>"));
  TEST_ASSERT_EQ(0xAABBCC, px(100, 100));

  /* #zzz -> invalid -> skip (background stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#zzz\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("svg #rgb shorthand + invalid shorthand");
}

/**
 * @test test_num_sign_fraction_mcdc
 *
 * @par MC/DC:
 * Decision: the integer scanner `priv_num` sign arm
 * `(s[*i]=='-')||(s[*i]=='+')` and its fraction arm `(s[*i]=='.')`
 * (libs/ra8_reflow/src/ra8_reflow_svg.c@priv_num). A `viewBox` whose numbers
 * carry a leading `-` (min-x), a `+` (min-y), and a fraction (truncated width)
 * drives every arm; a sibling viewBox of bare positive ints holds the sign and
 * fraction conditions false.
 *
 * Vectors:
 *  - V1: viewBox `-10 +0 100.5 100` -> sign T ('-'), sign T ('+'),
 *        fraction T ('.') -> the negative/positive/fractional arms execute and
 *        the shifted-origin viewBox maps the shape.
 *  - V2: viewBox `0 0 100 100` -> sign F, fraction F (the bare-integer control).
 * V1 vs V2 flip the sign and fraction conditions; the rendered colour differs
 * because the viewBox origin shifts.
 */
static void test_num_sign_fraction_mcdc(void)
{
  TEST_BEGIN("priv_num MC/DC: sign (- / +) and fraction arms");
  fb_reset();
  /* viewBox min-x = -10, min-y = +0, width = 100 (".5" truncated): a rect at
   * user (0,0) maps shifted right by the negative origin. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"-10 +0 100.5 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  /* user x in [0,20] with min-x=-10 -> framebuffer x in [20,60]; px(40,*) red. */
  TEST_ASSERT_EQ(k_red, px(40, 100));
  TEST_ASSERT_EQ(k_white, px(10, 100)); /* left of the shifted rect */

  /* V2 control: a plain integer viewBox maps user (0,0) to framebuffer (0,0). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(10, 100)); /* now the rect starts at x=0 */
  TEST_END("priv_num MC/DC: sign (- / +) and fraction arms");
}

/**
 * @test test_attr_value_forms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_attr` value delimiter handling --
 * `(j>=len)||((s[j]!='"')&&(s[j]!='\''))` (the open-quote check) and the
 * whitespace-after-`=` skip loop (libs/ra8_reflow/src/ra8_reflow_svg.c@priv_attr).
 * Three attribute spellings drive the arms: a single-quoted value, a value with
 * whitespace between `=` and the opening quote, and a malformed attribute whose
 * value has no quote at all (rejected -> attribute treated as absent).
 *
 * Vectors:
 *  - V1: `fill='#00ff00'` -> single-quote arm (s[j]=='\'') -> parsed -> green.
 *  - V2: `fill =  "#0000ff"` -> the ws-after-`=` loop advances, then the
 *        double-quote arm -> parsed -> blue.
 *  - V3: `fill=none nostop` plus a `width=20x` (unquoted) -> the no-quote reject
 *        path returns false for the malformed attr; the shape still renders with
 *        its default geometry, proving the reject did not crash.
 * V1 vs V2 vary the quote character / whitespace skip; V3 exercises the reject.
 */
static void test_attr_value_forms_mcdc(void)
{
  TEST_BEGIN("priv_attr MC/DC: single-quote / ws-before-value / no-quote reject");
  fb_reset();
  /* V1: single-quoted fill. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox='0 0 100 100'>"
                        "<rect x='0' y='0' width='100' height='100' fill='#00ff00'/></svg>"));
  TEST_ASSERT_EQ(k_green, px(100, 100));

  /* V2: whitespace between `=` and the opening quote on every attribute. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox = \"0 0 100 100\">"
                        "<rect x = \"0\" y = \"0\" width = \"100\" height = \"100\" "
                        "fill = \"#0000ff\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, px(100, 100));

  /* V3: a malformed unquoted attribute value -> priv_attr rejects it; the rect
   * keeps its default (zero) width there, but a well-formed sibling still fills,
   * proving the parser recovered rather than crashed. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=20 height=\"100\" fill=\"#ff0000\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#112233\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(0x112233, px(100, 100));
  TEST_END("priv_attr MC/DC: single-quote / ws-before-value / no-quote reject");
}

/**
 * @test test_named_color_mismatch
 * @brief Named colours fold case + miss late in priv_ci_eq, and an unknown
 *        name returns no_paint (the shape is skipped).
 *
 * @par MC/DC:
 * Decision: `priv_ci_eq` mismatch test `priv_lc(s[k]) != priv_lc(lit[k])`
 * (1 condition, per character; libs/ra8_reflow/src/ra8_reflow_svg.c@priv_ci_eq).
 * - V1: `fill="OrAnGe"` -> every folded char matches -> known colour fills.
 * - V2: `fill="oranje"` -> a late char mismatches every table name -> no match
 *       -> no_paint -> skip.
 * V1 vs V2 vary the mismatch condition (all-match vs a mismatch), flipping
 * "filled" vs "skipped".
 */
static void test_named_color_mismatch(void)
{
  TEST_BEGIN("priv_ci_eq MC/DC: mixed-case match vs late mismatch");
  fb_reset();
  /* V1: mixed-case known colour -> orange (0xFFA500). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"OrAnGe\"/></svg>"));
  TEST_ASSERT_EQ(0xFFA500, px(100, 100));

  /* V2: an unknown name -> no_paint -> skipped (stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"oranje\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("priv_ci_eq MC/DC: mixed-case match vs late mismatch");
}

/**
 * @test test_numf_sign_fraction_mcdc
 *
 * @par MC/DC:
 * Decision: the float scanner `priv_numf` sign arm
 * `(s[*i]=='-')||(s[*i]=='+')` and the fraction arm `(s[*i]=='.')`
 * (libs/ra8_reflow/src/ra8_reflow_svg.c@priv_numf), reached through the float
 * attribute path (transform arguments + gradient coordinates).
 *
 * Vectors:
 *  - V1: `transform="translate(-30.5,+20.25)"` -> sign T ('-'), sign T ('+'),
 *        fraction T ('.') in both arguments -> the shape shifts by the signed,
 *        fractional offset.
 *  - V2: `transform="translate(30,20)"` -> sign F, fraction F (bare integers).
 * V1 vs V2 flip the sign and fraction conditions; the placement differs.
 */
static void test_numf_sign_fraction_mcdc(void)
{
  TEST_BEGIN("priv_numf MC/DC: sign (- / +) and fraction arms");
  fb_reset();
  /* V1: a signed/fractional translate. user x 50..60 shifted by -30.5 -> ~19.5..29.5
   * -> fb ~39..59; y 10..20 shifted by +20.25 -> ~30.25..40.25 -> fb ~60..80. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"50\" y=\"10\" width=\"10\" height=\"10\" "
                        "fill=\"#ff0000\" transform=\"translate(-30.5,+20.25)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(48, 70)); /* inside the negatively-shifted rect */

  /* V2 control: a bare-integer translate places the rect elsewhere. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"50\" y=\"10\" width=\"10\" height=\"10\" "
                        "fill=\"#ff0000\" transform=\"translate(30,20)\"/></svg>"));
  /* user x 80..90 -> fb 160..180; y 30..40 -> fb 60..80. */
  TEST_ASSERT_EQ(k_red, px(170, 70));
  TEST_ASSERT_EQ(k_white, px(48, 70)); /* V1's spot is empty here */
  TEST_END("priv_numf MC/DC: sign (- / +) and fraction arms");
}

/**
 * @test test_is_num_start_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_is_num_start` ->
 * `digit || (c=='-') || (c=='+') || (c=='.')` (4 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_is_num_start), reached from the
 * transform argument reader priv_xform_read which stops at the first non-number.
 * Each leading character class appears as the first transform argument.
 *
 * Vectors (N+1 = 5 for N=4):
 *  - V1: `scale(2)`         -> digit T  -> arg read.
 *  - V2: `translate(-40,0)` -> '-' T    -> arg read.
 *  - V3: `translate(+40,0)` -> '+' T    -> arg read.
 *  - V4: `translate(.5)`    -> '.' T    -> arg read (fractional 0.5).
 *  - V5: `scale()`          -> ')' -> all F -> no arg read (na==0).
 * Each vector flips exactly one condition true with the lower-index conditions
 * held false; V5 holds all false (empty arg list).
 */
static void test_is_num_start_mcdc(void)
{
  TEST_BEGIN("priv_is_num_start MC/DC: digit/-/+/./none leading arg");
  /* V1 digit. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(2)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(60, 60)); /* 10..20 scaled 2x -> fb 40..80 */

  /* V2 leading '-'. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"50\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" transform=\"translate(-40,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_green, px(30, 30)); /* 50..60 - 40 -> 10..20 -> fb 20..40 */

  /* V3 leading '+'. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#0000ff\" transform=\"translate(+40,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, px(110, 30)); /* 10..20 + 40 -> 50..60 -> fb 100..120 */

  /* V4 leading '.': translate(.5) ~= translate(0) -> the rect stays put. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#112233\" transform=\"translate(.5)\"/></svg>"));
  TEST_ASSERT_EQ(0x112233, px(30, 30)); /* ~10..20 -> fb ~20..40 */

  /* V5 empty arg list: scale() reads no args -> the leading-number probe finds
   * none, leaving a degenerate transform, so the rect is not drawn. The parse
   * is exercised without a crash and the pixel stays the background. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#445566\" transform=\"scale()\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(30, 30));
  TEST_END("priv_is_num_start MC/DC: digit/-/+/./none leading arg");
}

/**
 * @test test_priv_hex_below_zero_nibble_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_hex` digit guard `(c>='0')&&(c<='9')` (L242) and letter guard
 * `(l>='a')&&(l<='f')` (L246; libs/ra8_reflow/src/ra8_reflow_svg.c@priv_hex).
 * Existing tests cover the digit branch (both T) and the `>'9'` letter `g`
 * (digit `c<='9'` F; letter `l<='f'` F). This adds a nibble BELOW `'0'` (`/`,
 * 0x2F) so the digit `c>='0'` condition and the letter `l>='a'` condition each
 * flip false.
 *  - V1: `#/00000` -> first nibble `/` -> digit `c>='0'` F (and letter
 *        `l>='a'` F) -> not a hex digit -> k_svg_no_paint -> rect skipped.
 * V1 vs the existing valid-hex and `g` vectors flip the lower-bound conditions
 * of both guards independently.
 */
static void test_priv_hex_below_zero_nibble_mcdc(void)
{
  TEST_BEGIN("priv_hex MC/DC: nibble below '0' flips both lower-bound guards");
  fb_reset();
  /* A '/' (0x2F) is below '0': the digit guard's `c>='0'` and the letter
   * guard's `l>='a'` both evaluate false, so the colour is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#/00000\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("priv_hex MC/DC: nibble below '0' flips both lower-bound guards");
}

/**
 * @test test_priv_num_boundary_arms_mcdc
 *
 * @par MC/DC:
 * Decision: the integer scanner `priv_num` -- the leading-separator skip
 * `(*i<len)&&(priv_ws||==',')` (L258), the sign test `(*i<len)&&(=='-'||=='+')`
 * (L262), the integer-digit loop `(*i<len)&&(>='0')&&(<='9')` (L267), and the
 * fraction-digit loop (L274; libs/ra8_reflow/src/ra8_reflow_svg.c@priv_num). The
 * existing tests cover the in-range arms; this drives the boundary arms where a
 * value ends mid-scan (the `*i<len` F conditions) and where a non-digit follows
 * a fraction.
 *
 * Vectors (each is a `<rect>` attribute slice handed verbatim to priv_num):
 *  - V1: `width="  "` -> the separator loop consumes both spaces then hits the
 *        end -> L258 `*i<len` F; with no digits the rect collapses (skipped).
 *  - V2: `width="100"` ending exactly at the slice end -> the digit loop runs
 *        out of buffer -> L267 `*i<len` F; the sign test sees a digit (no sign).
 *  - V3: `width="20.a"` -> a non-digit (`a`, >'9') after the dot -> L274 `<='9'`
 *        F (the fraction loop body does not run); `height="20.5("` puts a char
 *        below '0' after a fraction digit -> L274 `>='0'` F.
 *  - V4: `width="20."` -> the dot is the last byte -> L274 `*i<len` F.
 * The render result is robust (k_ra8_ok); the scan arms are what matters.
 */
static void test_priv_num_boundary_arms_mcdc(void)
{
  TEST_BEGIN("priv_num MC/DC: separator / sign / digit / fraction end-of-slice arms");
  /* V1: an all-separator width slice -> L258 runs to the slice end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"  \" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));

  /* V2: a digit run that ends exactly at the slice boundary -> L267 `*i<len` F. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 100));

  /* V3: non-digit after a fraction: 'a' (>'9') flips L274 `<='9'` F; '(' (<'0')
   * after a fraction digit flips L274 `>='0'` F. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20.a\" height=\"20.5(\" "
                        "fill=\"#ff0000\"/></svg>"));

  /* V4: a trailing dot -> the fraction loop's `*i<len` becomes false at once. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20.\" height=\"20\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_END("priv_num MC/DC: separator / sign / digit / fraction end-of-slice arms");
}

/**
 * @test test_priv_numf_boundary_arms_mcdc
 *
 * @par MC/DC:
 * Decision: the float scanner `priv_numf` -- the leading-separator skip (L559),
 * the sign test (L563), the integer-digit loop (L568), and the fraction-digit
 * loop (L575; libs/ra8_reflow/src/ra8_reflow_svg.c@priv_numf), reached through a
 * gradient `<stop offset=...>` and gradient coordinate attributes whose value
 * slice ends mid-scan.
 *
 * Vectors:
 *  - V1: `offset="0.5"` -> a fraction running to the slice end -> L575 `*i<len`
 *        F; `offset="0.a"` (a non-digit > '9' after the dot) -> L575 `<='9'` F.
 *  - V2: `offset="1"` -> the integer-digit loop ends at the slice -> L568
 *        `*i<len` F (and no sign -> L563 sign conditions false).
 *  - V3: `offset="  "` -> an all-separator slice -> L559 `*i<len` F.
 * Rendering the gradient is robust (k_ra8_ok); the float-scan arms are the point.
 */
static void test_priv_numf_boundary_arms_mcdc(void)
{
  TEST_BEGIN("priv_numf MC/DC: separator / sign / digit / fraction end-of-slice arms");
  fb_reset();
  /* The stop offsets exercise priv_numf at value-slice boundaries: a fraction
   * to the end (`0.5`), a non-digit after the dot (`0.a`), a bare integer to
   * the end (`1`), and an all-separator slice (`  `). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"  \" stop-color=\"#000000\"/>"
                        "<stop offset=\"0.a\" stop-color=\"#111111\"/>"
                        "<stop offset=\"0.5\" stop-color=\"#888888\"/>"
                        "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"url(#g)\"/></svg>"));
  /* The ramp still runs darker-to-lighter left-to-right. */
  TEST_ASSERT(px(10, 100) < px(190, 100));
  TEST_END("priv_numf MC/DC: separator / sign / digit / fraction end-of-slice arms");
}

/**
 * @test test_attr_boundary_reject_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_attr_at` boundary `(at>0)&&!priv_ws(s[at-1])` (L293) and the
 * delimiter `(s[after]=='=')||priv_ws(s[after])` (L300); `priv_attr`'s
 * scan-to-`=` (L316), ws-after-`=` skip (L320), open-quote reject (L323), and
 * value-scan-to-close-quote (L329; libs/ra8_reflow/src/ra8_reflow_svg.c).
 *
 * Vectors:
 *  - V1: a substring `xfill="..."` makes `priv_attr_at("fill")` match preceded
 *        by a non-ws `x` -> L293 `!priv_ws` T (reject); a `fillx="..."` makes
 *        the name match but the char after is neither `=` nor ws -> L300 (F,F)
 *        reject; the genuine ` fill="#ff0000"` then fills the rect red.
 *  - V2: a bare `fill ` (ws after, no `=` to the span end) -> L316 `j<len` F,
 *        L320 `j<len` F (skip), L323 `j>=len` T (reject) -> default black fill.
 *  - V3: an empty `fill=` (the `=` is the last span byte) -> L320 `j<len` F and
 *        L323 `j>=len` T (reject) -> default black fill.
 *  - V4: an unterminated `fill="#ff0000` (no close quote before the tag end) ->
 *        L329 `j<len` F; the malformed paint -> k_svg_no_paint -> rect skipped.
 */
static void test_attr_boundary_reject_mcdc(void)
{
  TEST_BEGIN("priv_attr MC/DC: name-boundary / delimiter / no-= / empty / unterminated");
  /* V1: a non-ws-preceded `xfill` (L293 reject) and a `fillx` followed by a
   * non-delimiter (L300 all-false reject); the real fill wins. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "xfill=\"#00ff00\" fillx=\"#0000ff\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 100));

  /* V2: a bare `fill` (ws after, no `=` anywhere) -> treated as absent ->
   * default black fill. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill ></svg>"));
  TEST_ASSERT_EQ(k_black, px(100, 100));

  /* V3: an empty `fill=` at the span end -> absent -> default black fill. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=></svg>"));
  TEST_ASSERT_EQ(k_black, px(100, 100));

  /* V4: an unterminated quoted value -> the scan runs to the span end (L329
   * `j<len` F); the garbage slice yields no_paint -> the rect is skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("priv_attr MC/DC: name-boundary / delimiter / no-= / empty / unterminated");
}

/**
 * @test test_ci_eq_length_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_ci_eq` loop guard `(k<len)&&(lit[k]!='\0')` (L355) and the
 * result `(k==len)&&(lit[k]=='\0')` (L360;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_ci_eq). A colour name that is a strict
 * prefix of a table entry exits the loop on `k<len` F (and makes `lit[k]!='\0'`
 * the deciding result condition), while a name longer than every entry exits on
 * `lit[k]!='\0'` F (making `k==len` false).
 *
 * Vectors:
 *  - V1: `fill="blu"` (a prefix of `blue`) -> L355 exits on `k<len` F; L360
 *        `lit[k]=='\0'` F (`lit[3]=='e'`) -> no match -> rect skipped.
 *  - V2: `fill="reddd"` (longer than `red`) -> L355 exits on `lit[k]!='\0'` F;
 *        L360 `k==len` F -> no match -> rect skipped.
 * V1 vs an exact match flip the length relation; the rect stays white.
 */
static void test_ci_eq_length_arms_mcdc(void)
{
  TEST_BEGIN("priv_ci_eq MC/DC: short-prefix vs over-long name");
  /* V1: `blu` is a prefix of `blue` -> runs out of span before the literal. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"blu\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));

  /* V2: `reddd` is longer than `red` -> the literal ends first. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"reddd\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("priv_ci_eq MC/DC: short-prefix vs over-long name");
}

/**
 * @test test_is_num_start_above_nine_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_is_num_start` digit sub-test `(c>='0')&&(c<='9')` within the
 * OR (L611; libs/ra8_reflow/src/ra8_reflow_svg.c@priv_is_num_start). The existing
 * test covers a digit (both T), `-`, `+`, `.`, and the empty `)` (all F, with
 * `c<'0'`). This adds a leading char ABOVE '9' (`z`) so the digit sub-test's
 * `c<='9'` condition flips false while `c>='0'` stays true and the sign/dot
 * conditions stay false.
 *
 * Vector:
 *  - `transform="scale(z)"` -> the first transform argument starts with `z`
 *    (>'9') -> `c>='0'` T, `c<='9'` F, not `-`/`+`/`.` -> no argument read ->
 *    a degenerate scale -> the rect collapses (stays white).
 */
static void test_is_num_start_above_nine_mcdc(void)
{
  TEST_BEGIN("priv_is_num_start MC/DC: leading char above '9'");
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(z)\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(30, 30));
  TEST_END("priv_is_num_start MC/DC: leading char above '9'");
}

/**
 * @brief Test executable entry point -- runs the scanner MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every scanner decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_ws_class_mcdc();
  test_hex_digit_mcdc();
  test_hex3_shorthand();
  test_num_sign_fraction_mcdc();
  test_attr_value_forms_mcdc();
  test_named_color_mismatch();
  test_numf_sign_fraction_mcdc();
  test_is_num_start_mcdc();
  test_priv_hex_below_zero_nibble_mcdc();
  test_priv_num_boundary_arms_mcdc();
  test_priv_numf_boundary_arms_mcdc();
  test_attr_boundary_reject_mcdc();
  test_ci_eq_length_arms_mcdc();
  test_is_num_start_above_nine_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_svg_scan_mcdc.c\n");
  return 0;
}

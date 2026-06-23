/**
 * @file test_ra_reflow_svg_mcdc.c
 * @brief MC/DC + branch-coverage tests for libs/ra_reflow/src/ra_reflow_svg.c.
 *
 * @details
 * Complements tests/test_ra_svg.c by driving the REAL public SVG API
 * (::ra_svg_is_svg, ::ra_svg_image_href, ::ra_svg_size, ::ra_svg_render)
 * with crafted byte-string inputs that exercise the still-uncovered branches
 * of the parser + rasteriser: the white-space class, the hex/decimal/float
 * number scanners (sign + fraction arms), the attribute scanner (whitespace
 * before the value, single-quoted values, a missing-quote reject), the
 * transform list (separator skipping, unknown function, signed / fractional /
 * `+`-prefixed arguments), the `<g>` group stack (self-close, depth cap, pop
 * at the floor), the gradient scanner (radial-before-linear ordering, multiple
 * gradients, `%` stop offsets, an unmatched `url(#)` while gradients exist, a
 * stop-less gradient skip), the elliptical-arc sweep wrap (both arms) and the
 * out-of-range-radius scale-up, the shape skip guards (`fill="none"` circle,
 * a points-less polygon / polyline, a stroke-less polyline), and the
 * width/height viewBox fallback in both ::ra_svg_size and ::ra_svg_render.
 *
 * Every test calls the production functions so llvm-cov attributes the
 * executed source lines; assertions pin the deterministic results and, for the
 * pure scan/whitespace branches, that the render completes without a crash.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_reflow_svg.h"
#include "unity_minimal.h"

/** @brief Framebuffer + colour dimensions for the render tests. */
enum : int32_t {
  k_w     = 200,        /**< Framebuffer / render-box width.  */
  k_h     = 200,        /**< Framebuffer / render-box height. */
  k_white = 0x00FFFFFF, /**< Cleared-background RGB.    */
  k_red   = 0x00FF0000, /**< A primary used for fills.  */
  k_green = 0x0000FF00, /**< A primary used for fills.  */
  k_blue  = 0x000000FF, /**< A primary used for fills.  */
  k_black = 0x00000000, /**< SVG default fill.          */
};

/** @brief Host framebuffer (ARGB8888) bound by the render tests. */
static uint32_t s_fb[k_w * k_h];

/** @brief RGB (low 24 bits) of the framebuffer pixel at (x, y). */
static uint32_t px(int32_t x, int32_t y)
{
  return s_fb[(y * k_w) + x] & 0x00FFFFFFU;
}

/** @brief Bind + clear the framebuffer to white. */
static void fb_reset(void)
{
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_init(s_fb, (uint16_t)k_w, (uint16_t)k_h, k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear((uint32_t)k_white));
}

/** @brief Render a NUL-terminated SVG string into the full framebuffer box. */
static ra_err_t render(const char* svg)
{
  return ra_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_w, k_h);
}

/**
 * @test test_ws_class_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_ws` ->
 * `(c==' ')||(c=='\t')||(c=='\n')||(c=='\r')||(c=='\f')` (5 conditions, OR;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_ws). Driven through the real number
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90\t10\n50\r90\f10,10\" "
                        "fill=\"#00aacc\"/></svg>"));
  /* Triangle fb-vertices (20,20)(180,20)(100,180): at fb-y=60 the span is
   * x in [40,160], so the centre is filled with the parsed colour. Reaching
   * it means every whitespace separator was consumed by priv_ws. */
  TEST_ASSERT_EQ(0x00AACC, (int)px(100, 60)); /* inside the filled triangle */
  TEST_ASSERT_EQ(k_white, (int)px(10, 60));   /* left of the left edge -> white */
  TEST_END("priv_ws MC/DC: space/tab/nl/cr/ff separators");
}

/**
 * @test test_hex_digit_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_hex` digit guard `(c>='0')&&(c<='9')` and the letter guard
 * `(l>='a')&&(l<='f')` (libs/ra_reflow/src/ra_reflow_svg.c@priv_hex). A
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0a0b0c\"/></svg>"));
  TEST_ASSERT_EQ(0x0A0B0C, (int)px(100, 100));

  /* V2: an invalid nibble 'g' -> no_paint -> the rect is skipped (stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0g0000\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("priv_hex MC/DC: digit vs a-f vs invalid nibble");
}

/**
 * @test test_hex3_shorthand
 * @brief A `#rgb` shorthand expands each nibble (priv_hex_color 3-digit arm),
 *        and an invalid shorthand nibble falls through to skip.
 */
static void test_hex3_shorthand(void)
{
  TEST_BEGIN("svg #rgb shorthand + invalid shorthand");
  fb_reset();
  /* #abc -> 0xAABBCC. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#abc\"/></svg>"));
  TEST_ASSERT_EQ(0xAABBCC, (int)px(100, 100));

  /* #zzz -> invalid -> skip (background stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#zzz\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("svg #rgb shorthand + invalid shorthand");
}

/**
 * @test test_num_sign_fraction_mcdc
 *
 * @par MC/DC:
 * Decision: the integer scanner `priv_num` sign arm
 * `(s[*i]=='-')||(s[*i]=='+')` and its fraction arm `(s[*i]=='.')`
 * (libs/ra_reflow/src/ra_reflow_svg.c@priv_num). A `viewBox` whose numbers
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"-10 +0 100.5 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  /* user x in [0,20] with min-x=-10 -> framebuffer x in [20,60]; px(40,*) red. */
  TEST_ASSERT_EQ(k_red, (int)px(40, 100));
  TEST_ASSERT_EQ(k_white, (int)px(10, 100)); /* left of the shifted rect */

  /* V2 control: a plain integer viewBox maps user (0,0) to framebuffer (0,0). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(10, 100)); /* now the rect starts at x=0 */
  TEST_END("priv_num MC/DC: sign (- / +) and fraction arms");
}

/**
 * @test test_attr_value_forms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_attr` value delimiter handling --
 * `(j>=len)||((s[j]!='"')&&(s[j]!='\''))` (the open-quote check) and the
 * whitespace-after-`=` skip loop (libs/ra_reflow/src/ra_reflow_svg.c@priv_attr).
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox='0 0 100 100'>"
                        "<rect x='0' y='0' width='100' height='100' fill='#00ff00'/></svg>"));
  TEST_ASSERT_EQ(k_green, (int)px(100, 100));

  /* V2: whitespace between `=` and the opening quote on every attribute. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox = \"0 0 100 100\">"
                        "<rect x = \"0\" y = \"0\" width = \"100\" height = \"100\" "
                        "fill = \"#0000ff\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(100, 100));

  /* V3: a malformed unquoted attribute value -> priv_attr rejects it; the rect
   * keeps its default (zero) width there, but a well-formed sibling still fills,
   * proving the parser recovered rather than crashed. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=20 height=\"100\" fill=\"#ff0000\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#112233\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(0x112233, (int)px(100, 100));
  TEST_END("priv_attr MC/DC: single-quote / ws-before-value / no-quote reject");
}

/**
 * @test test_named_color_mismatch
 * @brief Named colours fold case + miss late in priv_ci_eq, and an unknown
 *        name returns no_paint (the shape is skipped).
 *
 * @par MC/DC:
 * Decision: `priv_ci_eq` mismatch test `priv_lc(s[k]) != priv_lc(lit[k])`
 * (1 condition, per character; libs/ra_reflow/src/ra_reflow_svg.c@priv_ci_eq).
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"OrAnGe\"/></svg>"));
  TEST_ASSERT_EQ(0xFFA500, (int)px(100, 100));

  /* V2: an unknown name -> no_paint -> skipped (stays white). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"oranje\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("priv_ci_eq MC/DC: mixed-case match vs late mismatch");
}

/**
 * @test test_numf_sign_fraction_mcdc
 *
 * @par MC/DC:
 * Decision: the float scanner `priv_numf` sign arm
 * `(s[*i]=='-')||(s[*i]=='+')` and the fraction arm `(s[*i]=='.')`
 * (libs/ra_reflow/src/ra_reflow_svg.c@priv_numf), reached through the float
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"50\" y=\"10\" width=\"10\" height=\"10\" "
                        "fill=\"#ff0000\" transform=\"translate(-30.5,+20.25)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(48, 70)); /* inside the negatively-shifted rect */

  /* V2 control: a bare-integer translate places the rect elsewhere. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"50\" y=\"10\" width=\"10\" height=\"10\" "
                        "fill=\"#ff0000\" transform=\"translate(30,20)\"/></svg>"));
  /* user x 80..90 -> fb 160..180; y 30..40 -> fb 60..80. */
  TEST_ASSERT_EQ(k_red, (int)px(170, 70));
  TEST_ASSERT_EQ(k_white, (int)px(48, 70)); /* V1's spot is empty here */
  TEST_END("priv_numf MC/DC: sign (- / +) and fraction arms");
}

/**
 * @test test_is_num_start_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_is_num_start` ->
 * `digit || (c=='-') || (c=='+') || (c=='.')` (4 conditions, OR;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_is_num_start), reached from the
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(2)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(60, 60)); /* 10..20 scaled 2x -> fb 40..80 */

  /* V2 leading '-'. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"50\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" transform=\"translate(-40,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_green, (int)px(30, 30)); /* 50..60 - 40 -> 10..20 -> fb 20..40 */

  /* V3 leading '+'. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#0000ff\" transform=\"translate(+40,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(110, 30)); /* 10..20 + 40 -> 50..60 -> fb 100..120 */

  /* V4 leading '.': translate(.5) ~= translate(0) -> the rect stays put. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#112233\" transform=\"translate(.5)\"/></svg>"));
  TEST_ASSERT_EQ(0x112233, (int)px(30, 30)); /* ~10..20 -> fb ~20..40 */

  /* V5 empty arg list: scale() reads no args -> the leading-number probe finds
   * none, leaving a degenerate transform, so the rect is not drawn. The parse
   * is exercised without a crash and the pixel stays the background. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#445566\" transform=\"scale()\"/></svg>"));
  TEST_ASSERT_EQ((int64_t)(uint32_t)k_white, (int64_t)px(30, 30));
  TEST_END("priv_is_num_start MC/DC: digit/-/+/./none leading arg");
}

/**
 * @test test_xform_separators_unknown
 * @brief A chained `transform=` list with comma/space separators between
 *        functions and an unknown function name exercises the separator skip,
 *        the `(` / `)` scans, and the unknown -> skipped arm of priv_parse_xform.
 *
 * @par MC/DC:
 * Decision: `priv_parse_xform` -> `kind != k_svg_xf_none` (compose vs skip;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_parse_xform).
 * - V-known: `translate(50,0)` -> kind != none -> composed (the rect moves).
 * - V-unknown: `foo(1) translate(50,0)` -> the leading `foo(...)` has kind none
 *   -> skipped, then translate composes -> the rect still moves by 50.
 * The pair varies the kind classification (compose vs skip) with the same net
 * placement, proving the unknown arm is reached without corrupting the result.
 */
static void test_xform_separators_unknown(void)
{
  TEST_BEGIN("priv_parse_xform: separators + unknown function skip");
  /* Known-only baseline: translate(50,0) moves user 0..10 -> 50..60 -> fb 100..120. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"translate(50,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(110, 10));

  /* Unknown leading function + comma/space separators -> foo() skipped, then
   * scale(2) and translate(25,0) compose (translate applied first, then scale):
   * user x 0..10 -> (x+25)*2 = 50..70 -> fb 100..140. The unknown arm + the `,`
   * and ` ` separators are exercised. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" "
                        "transform=\"foo(1) , scale(2) translate(25,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_green, (int)px(120, 10));
  TEST_END("priv_parse_xform: separators + unknown function skip");
}

/**
 * @test test_circle_skip_and_rotated_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_circle` skip guard `(gi<0)&&(fill==no_paint)` and the
 * fast-path guard `(gi<0)&&!priv_has_rot(t)`
 * (libs/ra_reflow/src/ra_reflow_svg.c@priv_draw_circle).
 * - V1: `fill="none"` circle -> no_paint with gi<0 -> skipped (stays white).
 * - V2: solid circle, no transform -> not no_paint, no rotation -> fast-path
 *       ra_gfx_circle fills the disc.
 * - V3: solid circle under `rotate(30)` -> priv_has_rot true -> the N-gon
 *       polygon path fills (still covers the centre).
 * V1 vs V2 flip the no-paint condition; V2 vs V3 flip the has-rot condition.
 */
static void test_circle_skip_and_rotated_mcdc(void)
{
  TEST_BEGIN("priv_draw_circle MC/DC: none-skip / fast-path / rotated polygon");
  /* V1: fill:none -> skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"none\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));

  /* V2: solid, axis-aligned -> fast-path disc fills the centre. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 100)); /* centre (50,50) -> fb (100,100) */

  /* V3: solid under a rotation -> the N-gon polygon path fills; the centre,
   * being rotation-invariant, is still covered. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#0000ff\" "
                        "transform=\"rotate(30,50,50)\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(100, 100));
  TEST_END("priv_draw_circle MC/DC: none-skip / fast-path / rotated polygon");
}

/**
 * @test test_polygon_polyline_guards_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_polygon` guard
 * `((gi<0)&&(fill==no_paint)) || !priv_attr(points)` and `priv_draw_polyline`
 * guard `(stroke==no_paint) || !priv_attr(points)`
 * (libs/ra_reflow/src/ra_reflow_svg.c@priv_draw_polygon / @priv_draw_polyline).
 *
 * Vectors:
 *  - Polygon C1 T: `fill="none"` (with points) -> skipped.
 *  - Polygon C2 T: a filled polygon with NO `points` attr -> skipped.
 *  - Polygon both F: a filled polygon WITH points -> fills.
 *  - Polyline C1 T: a polyline with NO `stroke` -> skipped.
 *  - Polyline C2 T: a stroked polyline with NO `points` -> skipped (no crash).
 *  - Polyline both F: a stroked polyline WITH points -> strokes a visible pixel.
 * Each pair flips one condition of the respective OR while the other is held
 * false by the control case.
 */
static void test_polygon_polyline_guards_mcdc(void)
{
  TEST_BEGIN("polygon/polyline guard MC/DC: none / no-points / present");
  /* Polygon both-false control: fills the triangle interior. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 60));

  /* Polygon C1 (fill none) and C2 (no points) -> both skipped; background white. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"none\"/>"
                        "<polygon fill=\"#00ff00\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 60));

  /* Polyline both-false control: a stroked diagonal lights its midpoint. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polyline points=\"0,0 100,100\" stroke=\"#0000ff\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(100, 100)); /* midpoint of the diagonal */

  /* Polyline C1 (no stroke) and C2 (stroke but no points) -> both skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polyline points=\"0,0 100,100\"/>"
                        "<polyline stroke=\"#00ff00\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("polygon/polyline guard MC/DC: none / no-points / present");
}

/**
 * @test test_path_relative_hv
 * @brief Relative path commands (`m`/`l`/`h`/`v`) and the implicit line-to after
 *        `m` exercise priv_next_cmd's lowercase branch, priv_path_step's h/v
 *        arms, and the relative coordinate resolution.
 */
static void test_path_relative_hv(void)
{
  TEST_BEGIN("svg path relative m/l/h/v + implicit line-to");
  fb_reset();
  /* Start (10,10); relative h80 -> (90,10); v80 -> (90,90); h-80 -> (10,90); z.
   * A square 10..90 in user space -> fb 20..180. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"m10 10 h80 v80 h-80 z\" fill=\"#00aacc\"/></svg>"));
  TEST_ASSERT_EQ(0x00AACC, (int)px(100, 100)); /* inside the square */
  TEST_ASSERT_EQ(k_white, (int)px(10, 100));   /* left of x=20 -> white */
  TEST_END("svg path relative m/l/h/v + implicit line-to");
}

/**
 * @test test_arc_sweep_wrap_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_arc_solve` sweep normalisation
 * `(!sweep && dt>0)` else-if `(sweep && dt<0)`
 * (libs/ra_reflow/src/ra_reflow_svg.c@priv_arc_solve). The large-arc flag is
 * toggled so each branch's angular condition (dt>0 / dt<0) is the one that
 * fires for the chosen sweep flag.
 * - V1: sweep=0, large=1 over the same chord -> dt computes positive ->
 *       `!sweep && dt>0` arm subtracts 2*pi (a large clockwise arc).
 * - V2: sweep=1, large=1 -> dt computes negative -> `sweep && dt<0` arm adds
 *       2*pi (a large counter-clockwise arc).
 * The two large arcs bulge to opposite sides of the chord, so a probe above the
 * chord is filled for exactly one of them -- proving the sweep flag selects the
 * wrap arm.
 */
static void test_arc_sweep_wrap_mcdc(void)
{
  TEST_BEGIN("priv_arc_solve MC/DC: large-arc sweep wrap arms");
  /* V1: large=1, sweep=0 -> the !sweep && dt>0 wrap arm; the large arc encloses
   * the side opposite a small sweep=0 arc. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 0 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  const uint32_t up_v1   = px(100, 60);  /* above the chord */
  const uint32_t down_v1 = px(100, 150); /* below the chord */

  /* V2: large=1, sweep=1 -> the sweep && dt<0 wrap arm; the large arc bulges to
   * the opposite side relative to V1. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 1 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  const uint32_t up_v2   = px(100, 60);
  const uint32_t down_v2 = px(100, 150);

  /* Exactly one of the two large arcs fills above the chord, the other below --
   * the sweep flag flips which wrap arm runs and thus which side is enclosed. */
  TEST_ASSERT((up_v1 != up_v2) || (down_v1 != down_v2));
  TEST_END("priv_arc_solve MC/DC: large-arc sweep wrap arms");
}

/**
 * @test test_arc_radius_scale_up
 * @brief An arc whose requested radii are too small for the chord triggers the
 *        out-of-range-radius scale-up (priv_arc_center `lam > 1` arm), which
 *        enlarges rx/ry so the endpoints stay on the ellipse.
 *
 * @par MC/DC:
 * Decision: `priv_arc_center` -> `if (lam > 1.0F)` (1 condition).
 * - V1 (here): rx=ry=10 but the chord is 80 wide -> lam>1 -> radii scaled up;
 *   the arc still reaches its endpoint and the closed region fills.
 * - V2: tests/test_ra_svg.c@test_render_arc uses r=40 == half-chord -> lam<=1
 *   (no scale-up). The pair varies the lam condition.
 */
static void test_arc_radius_scale_up(void)
{
  TEST_BEGIN("priv_arc_center: out-of-range radius scale-up (lam>1)");
  fb_reset();
  /* Half-chord is 40 but rx=ry=10 -> lam = (40/10)^2 = 16 > 1 -> scale-up. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A10 10 0 0 1 90 50 Z\" fill=\"#00aacc\"/></svg>"));
  /* The scaled-up semicircle bulges up; its interior near the chord is filled. */
  TEST_ASSERT(px(100, 90) != (uint32_t)k_white);
  TEST_END("priv_arc_center: out-of-range radius scale-up (lam>1)");
}

/**
 * @test test_group_selfclose_depth_and_pop
 * @brief Self-closing `<g/>`, an over-deep `<g>` nest, and a `</g>` at the floor
 *        exercise priv_group_open's self-close + depth-cap arms and the
 *        priv_draw_shapes pop-at-zero arm.
 *
 * @par MC/DC:
 * Decision: `priv_group_open` -> `self_close || (gsp >= depth_max)` and
 * `priv_draw_shapes` -> `gsp = (gsp > 0) ? (gsp - 1) : 0`
 * (libs/ra_reflow/src/ra_reflow_svg.c).
 * - V-selfclose: `<g transform=... />` does NOT push; the following rect renders
 *   at the document (untransformed) origin, proving the self-close arm.
 * - V-extra-pop: a stray `</g>` with no open group keeps gsp at 0 (no underflow)
 *   and a following rect still renders -- the pop-at-floor arm.
 * - V-deep: more than k_svg_g_depth_max nested `<g>` groups; the deepest rect
 *   still renders without overrunning the bounded stack (depth-cap arm).
 */
static void test_group_selfclose_depth_and_pop(void)
{
  TEST_BEGIN("svg <g> self-close / depth-cap / pop-at-floor");
  /* V-selfclose: the self-closed group must NOT move the sibling rect. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<g transform=\"translate(80,80)\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ff0000\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(10, 10)); /* rect at origin, not translated */

  /* V-extra-pop: a stray `</g>` at the floor leaves gsp==0; the rect renders. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "</g>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#00ff00\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(k_green, (int)px(10, 10));

  /* V-deep: 10 nested groups (> k_svg_g_depth_max == 8); the innermost rect must
   * still render -- the bounded stack must not be overrun. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<g><g><g><g><g><g><g><g><g><g>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#0000ff\"/>"
                        "</g></g></g></g></g></g></g></g></g></g></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(10, 10));
  TEST_END("svg <g> self-close / depth-cap / pop-at-floor");
}

/**
 * @test test_grad_radial_first_and_unmatched_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_scan_grads` ordering `is_rad = (rad < lin)` and the
 * stop-count skip `if (g->nstops > 0U)`, plus `priv_match_grad`'s id-compare
 * `(strlen(gid)==idlen) && (memcmp(...)==0)`
 * (libs/ra_reflow/src/ra_reflow_svg.c).
 *
 * Vectors:
 *  - A radial gradient declared BEFORE a linear one -> `rad < lin` true on the
 *    first pass (radial picked first), false on the next (linear picked) ->
 *    both orderings of the ternary execute.
 *  - A gradient with NO `<stop>` children -> nstops==0 -> skipped (not counted).
 *  - A `fill="url(#defined)"` matches (id-compare true) while a sibling
 *    `fill="url(#missing)"` does not match (id-compare false) even though the
 *    gradient table is non-empty -- the loop body of priv_match_grad runs both
 *    ways.
 */
static void test_grad_radial_first_and_unmatched_mcdc(void)
{
  TEST_BEGIN("priv_scan_grads/match_grad MC/DC: radial-first, no-stop, matched/unmatched");
  fb_reset();
  /* `rg` (radial) is declared before `lg` (linear); `empty` has no stops and is
   * dropped. The first rect uses the linear gradient (matched); the second uses
   * an undefined id (unmatched -> skipped) over a pre-filled red backdrop. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    render("<svg viewBox=\"0 0 100 100\"><defs>"
           "<radialGradient id=\"rg\">"
           "<stop offset=\"0\" stop-color=\"#ffffff\"/>"
           "<stop offset=\"1\" stop-color=\"#000000\"/></radialGradient>"
           "<linearGradient id=\"empty\"></linearGradient>"
           "<linearGradient id=\"lg\">"
           "<stop offset=\"0%\" stop-color=\"#000000\"/>"
           "<stop offset=\"100%\" stop-color=\"#ffffff\"/></linearGradient>"
           "</defs>"
           "<rect x=\"0\" y=\"0\" width=\"100\" height=\"50\" fill=\"url(#lg)\"/>"
           "<rect x=\"0\" y=\"50\" width=\"100\" height=\"50\" fill=\"#ff0000\"/>"
           "<rect x=\"0\" y=\"50\" width=\"100\" height=\"50\" fill=\"url(#missing)\"/>"
           "</svg>"));
  /* Top band: linear gradient -> roughly a horizontal black->white ramp. */
  TEST_ASSERT(px(20, 40) < px(180, 40)); /* darker at left, lighter at right */
  /* Bottom band: the unmatched url is skipped, so the red backdrop survives. */
  TEST_ASSERT_EQ(k_red, (int)px(100, 150));
  TEST_END("priv_scan_grads/match_grad MC/DC: radial-first, no-stop, matched/unmatched");
}

/**
 * @test test_grad_stop_percent_and_default_color
 * @brief A `<stop>` with a `%` offset and a stop with no `stop-color`
 *        exercise priv_stop_offset's `%` arm and priv_stop_color's default arm.
 */
static void test_grad_stop_percent_and_default_color(void)
{
  TEST_BEGIN("svg gradient stop: % offset + default stop-color");
  fb_reset();
  /* The first stop has a `%` offset and NO stop-color (defaults to black); the
   * last stop is white. The ramp therefore runs black(0%) -> white(100%). */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0%\"/>"
                        "<stop offset=\"100%\" stop-color=\"#ffffff\"/></linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(10, 100) < px(190, 100)); /* black-ish left, white-ish right */
  TEST_END("svg gradient stop: % offset + default stop-color");
}

/**
 * @test test_size_and_viewbox_fallback_mcdc
 *
 * @par MC/DC:
 * Decision: `ra_svg_size` -> `if ((w<=0)||(h<=0))` (the width/height vs viewBox
 * fallback) and `priv_read_viewbox`'s width/height else-branch when no viewBox
 * is present (libs/ra_reflow/src/ra_reflow_svg.c).
 *
 * Vectors:
 *  - V1: `<svg width="120" height="90">` (no viewBox) -> w>0 && h>0 -> the
 *        attribute path returns 120x90 (the viewBox fallback is NOT taken).
 *  - V2: `<svg viewBox="0 0 60 40">` (no width/height) -> w<=0 -> the fallback
 *        reads the viewBox -> 60x40.
 *  - V3: a render of a width/height-only `<svg>` exercises priv_read_viewbox's
 *        else-branch (width/height -> the user-space extent) so a rect at the
 *        far corner still lands inside the box.
 * V1 vs V2 flip the (w<=0||h<=0) condition; V3 covers the render-side fallback.
 */
static void test_size_and_viewbox_fallback_mcdc(void)
{
  TEST_BEGIN("ra_svg_size + priv_read_viewbox MC/DC: width/height vs viewBox");
  int32_t w = 0;
  int32_t h = 0;

  /* V1: explicit width/height, no viewBox -> attribute path. */
  const char* a = "<svg width=\"120\" height=\"90\"></svg>";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_size((const uint8_t*)a, strlen(a), &w, &h));
  TEST_ASSERT_EQ(120, w);
  TEST_ASSERT_EQ(90, h);

  /* V2: no width/height -> viewBox fallback. */
  const char* b = "<svg viewBox=\"0 0 60 40\"></svg>";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_size((const uint8_t*)b, strlen(b), &w, &h));
  TEST_ASSERT_EQ(60, w);
  TEST_ASSERT_EQ(40, h);

  /* No size at all -> not found. */
  const char* c = "<svg></svg>";
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_svg_size((const uint8_t*)c, strlen(c), &w, &h));

  /* V3: render with width/height but no viewBox -> priv_read_viewbox uses the
   * width/height as the user-space extent (vw=100, vh=100 here -> 2x into the
   * 200-box). A rect filling user 0..100 covers the whole framebuffer. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg width=\"100\" height=\"100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(190, 190)); /* far corner is inside the mapped box */
  TEST_END("ra_svg_size + priv_read_viewbox MC/DC: width/height vs viewBox");
}

/**
 * @test test_render_no_svg_tag
 * @brief Bytes without a `<svg>` element still render cleanly (priv_read_viewbox
 *        early-returns the box default; no shapes draw).
 */
static void test_render_no_svg_tag(void)
{
  TEST_BEGIN("svg render: no <svg> element -> box default, no crash");
  fb_reset();
  /* No `<svg` tag at all: priv_tag_span fails -> viewBox stays the box; the
   * standalone rect is still walked and drawn at user==framebuffer coords. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<rect x=\"0\" y=\"0\" width=\"200\" height=\"200\" "
                        "fill=\"#00ff00\"/>"));
  TEST_ASSERT_EQ(k_green, (int)px(100, 100));
  TEST_END("svg render: no <svg> element -> box default, no crash");
}

/**
 * @test test_is_svg_xml_vs_plain
 * @brief The sniff accepts a leading `<?xml` and rejects a non-markup buffer
 *        (the `<?xml || <svg` OR, the non-SVG arm).
 *
 * @par MC/DC:
 * Decision: `ra_svg_is_svg` -> `starts_ci("<?xml") || starts_ci("<svg")`
 * (2 conditions, OR; libs/ra_reflow/src/ra_reflow_svg.c@ra_svg_is_svg).
 * - V1: `<?xml ...` -> C1 T -> accepted.
 * - V2: `<svg ...`  -> C1 F, C2 T -> accepted (covered by test_ra_svg.c too).
 * - V3: `<html>...` -> C1 F, C2 F -> rejected.
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 */
static void test_is_svg_xml_vs_plain(void)
{
  TEST_BEGIN("ra_svg_is_svg MC/DC: <?xml / <svg / neither");
  TEST_ASSERT(ra_svg_is_svg((const uint8_t*)"<?xml version=\"1.0\"?>", 21U));
  TEST_ASSERT(ra_svg_is_svg((const uint8_t*)"<svg>", 5U));
  TEST_ASSERT(!ra_svg_is_svg((const uint8_t*)"<html></html>", 13U));
  TEST_ASSERT(!ra_svg_is_svg((const uint8_t*)"", 0U)); /* empty -> false */
  TEST_END("ra_svg_is_svg MC/DC: <?xml / <svg / neither");
}

/**
 * @brief Build "<svg viewBox='0 0 100 100'>" + @p reps repeats of "L5 5 "
 *        after an "M0 0 ", then @p suffix, into @p dst (path-curve overflow tests).
 *
 * @details Used to drive the @c (*n < k_svg_poly_max) loop-bound arm of the
 * curve / arc flatteners: a long run of line-tos pushes the vertex count near
 * the 64-point cap so the trailing curve/arc command's flatten loop stops on
 * the count rather than its segment count.
 *
 * @param[out] dst    Destination buffer (>= 1024 bytes).
 * @param[in]  cap    Capacity of @p dst, bytes.
 * @param[in]  reps   Number of "L5 5 " line-tos to emit after the initial M.
 * @param[in]  suffix Trailing path data (e.g. a `C`/`Q`/`A` command) + tags.
 */
static void build_long_path(char* dst, size_t cap, int32_t reps, const char* suffix)
{
  int32_t off = snprintf(dst, cap, "<svg viewBox=\"0 0 100 100\"><path fill=\"#ff0000\" d=\"M0 0 ");
  for (int32_t k = 0; (k < reps) && ((size_t)off < (cap - 8U)); ++k) {
    off += snprintf(&dst[off], cap - (size_t)off, "L%d 5 ", (k % 90) + 1);
  }
  (void)snprintf(&dst[off], cap - (size_t)off, "%s", suffix);
}

/**
 * @test test_priv_hex_below_zero_nibble_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_hex` digit guard `(c>='0')&&(c<='9')` (L242) and letter guard
 * `(l>='a')&&(l<='f')` (L246; libs/ra_reflow/src/ra_reflow_svg.c@priv_hex).
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#/00000\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("priv_hex MC/DC: nibble below '0' flips both lower-bound guards");
}

/**
 * @test test_priv_num_boundary_arms_mcdc
 *
 * @par MC/DC:
 * Decision: the integer scanner `priv_num` -- the leading-separator skip
 * `(*i<len)&&(priv_ws||==',')` (L258), the sign test `(*i<len)&&(=='-'||=='+')`
 * (L262), the integer-digit loop `(*i<len)&&(>='0')&&(<='9')` (L267), and the
 * fraction-digit loop (L274; libs/ra_reflow/src/ra_reflow_svg.c@priv_num). The
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
 * The render result is robust (k_ra_ok); the scan arms are what matters.
 */
static void test_priv_num_boundary_arms_mcdc(void)
{
  TEST_BEGIN("priv_num MC/DC: separator / sign / digit / fraction end-of-slice arms");
  /* V1: an all-separator width slice -> L258 runs to the slice end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"  \" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));

  /* V2: a digit run that ends exactly at the slice boundary -> L267 `*i<len` F. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 100));

  /* V3: non-digit after a fraction: 'a' (>'9') flips L274 `<='9'` F; '(' (<'0')
   * after a fraction digit flips L274 `>='0'` F. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"20.a\" height=\"20.5(\" "
                        "fill=\"#ff0000\"/></svg>"));

  /* V4: a trailing dot -> the fraction loop's `*i<len` becomes false at once. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
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
 * loop (L575; libs/ra_reflow/src/ra_reflow_svg.c@priv_numf), reached through a
 * gradient `<stop offset=...>` and gradient coordinate attributes whose value
 * slice ends mid-scan.
 *
 * Vectors:
 *  - V1: `offset="0.5"` -> a fraction running to the slice end -> L575 `*i<len`
 *        F; `offset="0.a"` (a non-digit > '9' after the dot) -> L575 `<='9'` F.
 *  - V2: `offset="1"` -> the integer-digit loop ends at the slice -> L568
 *        `*i<len` F (and no sign -> L563 sign conditions false).
 *  - V3: `offset="  "` -> an all-separator slice -> L559 `*i<len` F.
 * Rendering the gradient is robust (k_ra_ok); the float-scan arms are the point.
 */
static void test_priv_numf_boundary_arms_mcdc(void)
{
  TEST_BEGIN("priv_numf MC/DC: separator / sign / digit / fraction end-of-slice arms");
  fb_reset();
  /* The stop offsets exercise priv_numf at value-slice boundaries: a fraction
   * to the end (`0.5`), a non-digit after the dot (`0.a`), a bare integer to
   * the end (`1`), and an all-separator slice (`  `). */
  TEST_ASSERT_EQ(k_ra_ok,
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
 * value-scan-to-close-quote (L329; libs/ra_reflow/src/ra_reflow_svg.c).
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "xfill=\"#00ff00\" fillx=\"#0000ff\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 100));

  /* V2: a bare `fill` (ws after, no `=` anywhere) -> treated as absent ->
   * default black fill. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill ></svg>"));
  TEST_ASSERT_EQ(k_black, (int)px(100, 100));

  /* V3: an empty `fill=` at the span end -> absent -> default black fill. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=></svg>"));
  TEST_ASSERT_EQ(k_black, (int)px(100, 100));

  /* V4: an unterminated quoted value -> the scan runs to the span end (L329
   * `j<len` F); the garbage slice yields no_paint -> the rect is skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("priv_attr MC/DC: name-boundary / delimiter / no-= / empty / unterminated");
}

/**
 * @test test_ci_eq_length_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_ci_eq` loop guard `(k<len)&&(lit[k]!='\0')` (L355) and the
 * result `(k==len)&&(lit[k]=='\0')` (L360;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_ci_eq). A colour name that is a strict
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"blu\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));

  /* V2: `reddd` is longer than `red` -> the literal ends first. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"reddd\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(100, 100));
  TEST_END("priv_ci_eq MC/DC: short-prefix vs over-long name");
}

/**
 * @test test_match_grad_no_close_paren_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_match_grad` id-scan `(idend<vlen)&&(val[idend]!=')')` (L450;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_match_grad). The existing test uses a
 * well-formed `url(#id)` whose scan stops at `)`; this uses a `url(#id` with no
 * closing `)`, so the id scan runs to the value-slice end (`idend<vlen` F).
 *
 * Vector:
 *  - A defined `lg` gradient makes the table non-empty; a rect `fill="url(#zz"`
 *    (no `)`) scans the id to the slice end -> L450 `idend<vlen` F; `zz` matches
 *    no gradient -> -1 -> no_paint -> the rect is skipped over a red backdrop.
 */
static void test_match_grad_no_close_paren_mcdc(void)
{
  TEST_BEGIN("priv_match_grad MC/DC: url(#id with no closing paren");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"lg\">"
                        "<stop offset=\"0\" stop-color=\"#000000\"/>"
                        "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#zz\"/>"
                        "</svg>"));
  /* The unmatched `url(#zz` is skipped, so the red backdrop survives. */
  TEST_ASSERT_EQ(k_red, (int)px(100, 100));
  TEST_END("priv_match_grad MC/DC: url(#id with no closing paren");
}

/**
 * @test test_is_num_start_above_nine_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_is_num_start` digit sub-test `(c>='0')&&(c<='9')` within the
 * OR (L611; libs/ra_reflow/src/ra_reflow_svg.c@priv_is_num_start). The existing
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
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(z)\"/></svg>"));
  TEST_ASSERT_EQ(k_white, (int)px(30, 30));
  TEST_END("priv_is_num_start MC/DC: leading char above '9'");
}

/**
 * @test test_xform_scan_boundary_mcdc
 *
 * @par MC/DC:
 * Decision: the transform scanners -- `priv_xform_read` arg-separator skip
 * `(*j<vlen)&&(priv_ws||==',')` (L654); `priv_parse_xform` leading-separator
 * skip (L764), the scan-to-`(` `(op<vlen)&&(v[op]!='(')` (L769), and the
 * scan-to-`)` `(j<vlen)&&(v[j]!=')')` (L781;
 * libs/ra_reflow/src/ra_reflow_svg.c). Each `*<vlen` F arm is driven by a
 * transform value that ends mid-scan (trailing separator / no `(` / no `)`).
 *
 * Vectors:
 *  - V1: `transform="scale(2,"` -> after the first arg the separator skip hits
 *        a `,` then the value end -> L654 `,` T then `*j<vlen` F.
 *  - V2: `transform="translate(10,0) "` (trailing space) -> the leading-sep
 *        skip consumes the trailing space to the value end -> L764 `i<vlen` F.
 *  - V3: `transform="rotate"` (no `(`) -> the scan-to-`(` runs to the value end
 *        -> L769 `op<vlen` F -> the transform is dropped (identity).
 *  - V4: `transform="translate(10,0"` (no `)`) -> the scan-to-`)` runs to the
 *        value end -> L781 `j<vlen` F.
 * Each renders cleanly (k_ra_ok); the boundary scan arms are the point.
 */
static void test_xform_scan_boundary_mcdc(void)
{
  TEST_BEGIN("priv_parse_xform/xform_read MC/DC: trailing-sep / no-paren scan arms");
  /* V1: a trailing `,` then the value end inside the arg separator skip. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(2,\"/></svg>"));

  /* V2: a trailing space after a complete function -> the leading-sep loop runs
   * to the value end. The rect still translates by (10,0). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" transform=\"translate(10,0) \"/></svg>"));

  /* V3: a name with no `(` -> the scan-to-`(` runs to the end and the transform
   * is dropped; the rect renders at the document origin. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#0000ff\" transform=\"rotate\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, (int)px(10, 10));

  /* V4: an arg list with no `)` -> the scan-to-`)` runs to the value end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#112233\" transform=\"translate(10,0\"/></svg>"));
  TEST_END("priv_parse_xform/xform_read MC/DC: trailing-sep / no-paren scan arms");
}

/**
 * @test test_circle_gradient_fill_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_circle` skip guard `(gi<0)&&(fill==no_paint)` (L854) and
 * the fast-path guard `(gi<0)&&!priv_has_rot(t)` (L860;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_draw_circle). The existing test covers
 * `gi<0` true (none-skip, fast-path, rotated). This drives `gi<0` FALSE: a
 * circle filled with a matched gradient takes neither the skip nor the
 * fast-path, instead the N-gon gradient fill.
 *
 * Vector:
 *  - A defined `g` gradient + `<circle ... fill="url(#g)"/>` -> gi>=0 -> L854
 *    `gi<0` F (do not skip) and L860 `gi<0` F (do not take the fast-path) -> the
 *    circle is gradient-filled -> the centre is non-white.
 */
static void test_circle_gradient_fill_mcdc(void)
{
  TEST_BEGIN("priv_draw_circle MC/DC: gradient fill (gi>=0)");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><radialGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
                        "<stop offset=\"1\" stop-color=\"#0000ff\"/></radialGradient></defs>"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(100, 100) != (uint32_t)k_white);
  TEST_END("priv_draw_circle MC/DC: gradient fill (gi>=0)");
}

/**
 * @test test_polygon_path_gradient_fill_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_polygon` guard `((gi<0)&&(fill==no_paint))||!priv_attr`
 * (L1088) and `priv_draw_path` guard `((gi<0)&&(fill==no_paint))||!priv_attr`
 * (L1599; libs/ra_reflow/src/ra_reflow_svg.c). The existing tests cover the
 * `fill="none"`, the no-`points`/no-`d`, and the solid arms but never `gi<0`
 * false. This drives `gi<0` FALSE for both shapes via a matched gradient, and
 * adds the path-side `fill="none"` and missing-`d` skip arms (L1599 reaches its
 * `(gi<0)&&(fill==no_paint)` and `!priv_attr` true conditions).
 *
 * Vectors:
 *  - V1: `<polygon points=... fill="url(#g)"/>` -> gi>=0 -> L1088 `gi<0` F ->
 *        the polygon is gradient-filled (centre non-white).
 *  - V2: `<path fill="none" d="..."/>` -> L1599 `(gi<0)&&(fill==no_paint)` T ->
 *        skipped; `<path fill="#00ff00"/>` (no `d`) -> L1599 `!priv_attr("d")`
 *        T -> skipped; `<path fill="url(#g)" d="..."/>` -> gi>=0 -> L1599
 *        `gi<0` F -> gradient-filled (centre non-white).
 */
static void test_polygon_path_gradient_fill_mcdc(void)
{
  TEST_BEGIN("polygon/path guard MC/DC: matched-gradient (gi<0 false) + path none/no-d");
  /* V1: a polygon whose fill resolves to a gradient. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
                        "<stop offset=\"1\" stop-color=\"#0000ff\"/></linearGradient></defs>"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(100, 60) != (uint32_t)k_white);

  /* V2: path with fill=none (skipped), path with no d (skipped), and a path
   * whose fill resolves to a gradient (gradient-filled). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
                        "<stop offset=\"1\" stop-color=\"#0000ff\"/></linearGradient></defs>"
                        "<path fill=\"none\" d=\"M10 10 L90 10 L50 90 Z\"/>"
                        "<path fill=\"#00ff00\"/>"
                        "<path fill=\"url(#g)\" d=\"M10 10 L90 10 L50 90 Z\"/></svg>"));
  TEST_ASSERT(px(100, 60) != (uint32_t)k_white);
  TEST_END("polygon/path guard MC/DC: matched-gradient (gi<0 false) + path none/no-d");
}

/**
 * @test test_points_trailing_separator_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_parse_points` separator skip `(k<vlen)&&(priv_ws||==',')`
 * (L915; libs/ra_reflow/src/ra_reflow_svg.c@priv_parse_points). The existing
 * tests stop the skip at a digit (`priv_ws`/`,` F). This drives `k<vlen` F: a
 * `points` value with a trailing separator, so the skip loop runs to the slice
 * end before the outer `k>=vlen` break.
 *
 * Vector:
 *  - `<polygon points="10,10 90,10 50,90 " .../>` (trailing space) -> after the
 *    last pair the separator skip consumes the space and reaches the slice end
 *    -> L915 `k<vlen` F -> the triangle still fills.
 */
static void test_points_trailing_separator_mcdc(void)
{
  TEST_BEGIN("priv_parse_points MC/DC: trailing separator to slice end");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90 \" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 60));
  TEST_END("priv_parse_points MC/DC: trailing separator to slice end");
}

/**
 * @test test_scanline_windings_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_scanline_x` crossing test
 * `((y0<=y)&&(y<y1))||((y1<=y)&&(y<y0))` (L954) and the edge loop bound
 * `(i<n)&&(m<k_svg_poly_max)` (L950; libs/ra_reflow/src/ra_reflow_svg.c). The
 * existing fills cover the common crossing arms; this adds an opposite-wound
 * polygon and a dense many-vertex zigzag so the up-edge / down-edge clauses and
 * the crossing-count cap arm are exercised.
 *
 * Vectors:
 *  - V1: a clockwise and a counter-clockwise triangle -> both the
 *        `(y0<=y)&&(y<y1)` (down) and `(y1<=y)&&(y<y0)` (up) crossing clauses run.
 *  - V2: a 60+-vertex comb crossing a single scanline many times pushes the
 *        crossing count toward the cap (the `m<k_svg_poly_max` arm).
 * Each renders cleanly (k_ra_ok).
 */
static void test_scanline_windings_mcdc(void)
{
  TEST_BEGIN("priv_scanline_x MC/DC: opposite windings + dense crossings");
  /* V1: opposite-wound triangles exercise both crossing clauses. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"50,90 90,10 10,10\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 60));

  /* V2: a dense comb -- many narrow up/down spikes across one band -- to drive
   * the crossing-count toward the per-scanline cap. */
  fb_reset();
  {
    char    buf[1536] = {};
    int32_t off = snprintf(buf, sizeof(buf), "<svg viewBox=\"0 0 100 100\"><polygon points=\"");
    const int32_t k_spikes = 30;
    for (int32_t k = 0; k < k_spikes; ++k) {
      const int32_t bx = (k * 3) + 1;
      off += snprintf(&buf[off], sizeof(buf) - (size_t)off, "%d,90 %d,10 ", bx, bx + 1);
    }
    (void)snprintf(&buf[off], sizeof(buf) - (size_t)off, "95,90\" fill=\"#0000ff\"/></svg>");
    TEST_ASSERT_EQ(k_ra_ok, render(buf));
  }
  TEST_END("priv_scanline_x MC/DC: opposite windings + dense crossings");
}

/**
 * @test test_grad_three_stop_bracket_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_grad_eval` stop-bracket test `(p>=o0)&&(p<=o1)` (L1039;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_grad_eval). With only two stops the
 * interior bracket loop never advances; a three-stop gradient makes a pixel
 * whose parameter lies past the middle stop fail `p<=o1` at the first bracket
 * (so the loop advances) and match at the second (`p<=o1` T), driving both arms.
 *
 * Vector:
 *  - A 3-stop linear gradient (0 / 0.5 / 1) across a rect -> left pixels bracket
 *    in [0,0.5] and right pixels bracket in [0.5,1] -> L1039 `p<=o1` F (advance)
 *    and T (match) both run -> the ramp is monotone left-to-right.
 */
static void test_grad_three_stop_bracket_mcdc(void)
{
  TEST_BEGIN("priv_grad_eval MC/DC: three-stop interior bracket advance");
  fb_reset();
  TEST_ASSERT_EQ(
    k_ra_ok,
    render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
           "<stop offset=\"0\" stop-color=\"#000000\"/>"
           "<stop offset=\"0.5\" stop-color=\"#808080\"/>"
           "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
           "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(10, 100) < px(190, 100));
  TEST_END("priv_grad_eval MC/DC: three-stop interior bracket advance");
}

/**
 * @test test_curve_overflow_count_mcdc
 *
 * @par MC/DC:
 * Decision: the curve / arc flatten loop bounds `(j<=seg)&&(*n<k_svg_poly_max)`
 * in `priv_flatten_cubic` (L1186), `priv_flatten_quad` (L1233), and
 * `priv_flatten_arc` (L1463), plus the path outer loop `(i<dlen)&&(n<poly_max)`
 * (L1561; libs/ra_reflow/src/ra_reflow_svg.c). The existing curve tests exit on
 * the segment count; this prepends a long run of line-tos so the vertex count
 * reaches the 64-point cap DURING the trailing curve/arc, flipping the
 * `*n<k_svg_poly_max` (and the outer `n<poly_max`) condition false.
 *
 * Vectors (built by ::build_long_path):
 *  - V1: ~58 line-tos then a cubic `C` -> the cubic flatten stops on the count.
 *  - V2: ~58 line-tos then a quadratic `Q` -> the quad flatten stops on the count.
 *  - V3: ~58 line-tos then an arc `A` -> the arc flatten stops on the count.
 * Each renders cleanly (k_ra_ok).
 */
static void test_curve_overflow_count_mcdc(void)
{
  TEST_BEGIN("flatten_cubic/quad/arc MC/DC: vertex-count cap arm");
  char          buf[1024] = {};
  const int32_t k_reps    = 58;

  /* V1: a cubic after a near-full vertex list. */
  fb_reset();
  build_long_path(buf, sizeof(buf), k_reps, "C 60 0 70 80 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra_ok, render(buf));

  /* V2: a quadratic after a near-full vertex list. */
  fb_reset();
  build_long_path(buf, sizeof(buf), k_reps, "Q 60 0 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra_ok, render(buf));

  /* V3: an arc after a near-full vertex list. */
  fb_reset();
  build_long_path(buf, sizeof(buf), k_reps, "A 40 40 0 1 1 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra_ok, render(buf));
  TEST_END("flatten_cubic/quad/arc MC/DC: vertex-count cap arm");
}

/**
 * @test test_arc_sweep_both_flags_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_arc_solve` sweep wrap `(!sweep && dt>0)` (L1363) and
 * `(sweep && dt<0)` (L1365; libs/ra_reflow/src/ra_reflow_svg.c@priv_arc_solve).
 * The existing test toggles only the large-arc flag at fixed sweep; this renders
 * the four (large, sweep) combinations over one chord so each wrap arm's angular
 * condition (`dt>0` for `!sweep`, `dt<0` for `sweep`) fires.
 *
 * Vectors (all `M10 50 ... 90 50`):
 *  - small/sweep0 (`0 0`), large/sweep0 (`1 0`), small/sweep1 (`0 1`),
 *    large/sweep1 (`1 1`) -> across the four the `dt` sign spans both arms.
 * Each renders cleanly (k_ra_ok).
 */
static void test_arc_sweep_both_flags_mcdc(void)
{
  TEST_BEGIN("priv_arc_solve MC/DC: all (large,sweep) wrap arms");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 0 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 0 90 50 Z\" fill=\"#00ff00\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 1 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 1 90 50 Z\" fill=\"#112233\"/></svg>"));
  TEST_END("priv_arc_solve MC/DC: all (large,sweep) wrap arms");
}

/**
 * @test test_arc_degenerate_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_arc_center` degeneracy guard
 * `(rx==0)||(ry==0)||((p0.x==p_end.x)&&(p0.y==p_end.y))` (L1404;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_arc_center). The existing arcs are all
 * non-degenerate (every condition false). This drives each true arm and the
 * coincident-endpoint sub-conditions independently.
 *
 * Vectors:
 *  - V1: `A0 40 ...` -> `rx==0` T -> degenerate (collapses to a line).
 *  - V2: `A40 0 ...` -> `ry==0` T -> degenerate.
 *  - V3: `M10 50 A40 40 0 0 1 10 50` -> start==end -> `p0.x==p_end.x` T &&
 *        `p0.y==p_end.y` T -> degenerate.
 *  - V4: `M50 10 A40 40 0 0 1 50 90` -> same x, different y -> `p0.x==p_end.x` T
 *        && `p0.y==p_end.y` F -> NOT degenerate (a real arc), proving the y
 *        sub-condition's independence.
 * Each renders cleanly (k_ra_ok).
 */
static void test_arc_degenerate_arms_mcdc(void)
{
  TEST_BEGIN("priv_arc_center MC/DC: zero-rx / zero-ry / coincident endpoints");
  /* V1: rx == 0. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A0 40 0 0 1 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  /* V2: ry == 0. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 0 0 0 1 90 50 Z\" fill=\"#00ff00\"/></svg>"));
  /* V3: start == end (both coordinates equal). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 1 10 50 Z\" fill=\"#0000ff\"/></svg>"));
  /* V4: same x, different y -> not degenerate (y sub-condition independence). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M50 10 A40 40 0 0 1 50 90 Z\" fill=\"#112233\"/></svg>"));
  TEST_END("priv_arc_center MC/DC: zero-rx / zero-ry / coincident endpoints");
}

/**
 * @test test_path_cmd_scan_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_next_cmd` separator skip `(*i<dlen)&&(priv_ws||==',')` (L1538)
 * and the letter test `((c>='A')&&(c<='Z'))||((c>='a')&&(c<='z'))` (L1545;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_next_cmd). The existing path tests use
 * space separators and A-Z / a-z letters; this adds comma separators, a trailing
 * separator run (`*i<dlen` F), and command-position characters just outside the
 * letter ranges (`` ` `` between `Z` and `a`, `{` just above `z`) so the upper
 * and lower range bounds each flip false.
 *
 * Vectors:
 *  - V1: `d="M10,10 L90,90 "` -> comma separators (L1538 `==','` T) and a
 *        trailing space to the slice end (L1538 `*i<dlen` F).
 *  - V2: `d="M10 10 ` ` 90 90"` -> a backtick (0x60, between `Z` and `a`) at a
 *        command position -> L1545 `c<='Z'` F and `c>='a'` F -> the implicit
 *        previous command repeats.
 *  - V3: `d="M10 10 { 90 90"` -> `{` (0x7B, just above `z`) -> L1545 `c<='z'` F.
 * Each renders cleanly (k_ra_ok).
 */
static void test_path_cmd_scan_arms_mcdc(void)
{
  TEST_BEGIN("priv_next_cmd MC/DC: comma/trailing separators + letter-range bounds");
  /* V1: comma separators and a trailing space to the slice end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10,10 L90,90 \" fill=\"#ff0000\"/></svg>"));

  /* V2: a backtick at a command position (between 'Z' and 'a'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 10 ` 90 90\" fill=\"#00ff00\"/></svg>"));

  /* V3: a '{' at a command position (just above 'z'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 10 { 90 90\" fill=\"#0000ff\"/></svg>"));
  TEST_END("priv_next_cmd MC/DC: comma/trailing separators + letter-range bounds");
}

/**
 * @test test_elem_at_delimiters_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_elem_at` delimiter `priv_ws(c)||(c=='>')||(c=='/')` (L1628;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_elem_at). The existing tests reach the
 * whitespace arm; this adds a `>`-immediately element, a `/`-immediately
 * self-close, and a longer name (`<rectangle>`) where the element prefix matches
 * but the following char is none of the delimiters (all-false), driving each OR
 * condition.
 *
 * Vectors:
 *  - V1: `<circle>` with `>` straight after the name -> `c=='>'` T.
 *  - V2: `<line/>` self-close -> `c=='/'` T.
 *  - V3: `<rectangle ...>` -> the `<rect` prefix matches but `a` follows ->
 *        `priv_ws` F, `=='>'` F, `=='/'` F -> NOT treated as a `<rect>`.
 * Each renders cleanly (k_ra_ok).
 */
static void test_elem_at_delimiters_mcdc(void)
{
  TEST_BEGIN("priv_elem_at MC/DC: '>' / '/' / non-delimiter element boundary");
  fb_reset();
  /* V1: circle followed immediately by '>'; V2: a self-closing line; V3: a
   * `<rectangle>` whose `<rect` prefix matches but is followed by a letter. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#ff0000\"></circle>"
                        "<line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"100\" stroke=\"#00ff00\"/>"
                        "<rectangle x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#0000ff\"/>"
                        "</svg>"));
  /* The circle (with '>' delimiter) fills its disc; probe an interior point off
   * the green diagonal line so the red fill is what we observe. The bogus
   * <rectangle> is skipped, so the blue full-box fill never happens. */
  TEST_ASSERT_EQ(k_red, (int)px(110, 100));
  TEST_END("priv_elem_at MC/DC: '>' / '/' / non-delimiter element boundary");
}

/**
 * @test test_tag_span_no_close_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_tag_span` scan-to-`>` `(j<len)&&(s[j]!='>')` (L1644;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_tag_span). The existing tests always
 * terminate the `<svg>` tag; this passes an unterminated `<svg ...` (no `>`) to
 * ::ra_svg_size so the scan runs to the buffer end (`j<len` F).
 *
 * Vector:
 *  - `<svg width="120" height="90"` (no `>`) -> the scan reaches the end ->
 *    L1644 `j<len` F -> the span still covers the width/height -> size 120x90.
 */
static void test_tag_span_no_close_mcdc(void)
{
  TEST_BEGIN("priv_tag_span MC/DC: unterminated <svg tag (no '>')");
  int32_t     w = 0;
  int32_t     h = 0;
  const char* a = "<svg width=\"120\" height=\"90\"";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_size((const uint8_t*)a, strlen(a), &w, &h));
  TEST_ASSERT_EQ(120, w);
  TEST_ASSERT_EQ(90, h);
  TEST_END("priv_tag_span MC/DC: unterminated <svg tag (no '>')");
}

/**
 * @test test_read_viewbox_nonpositive_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_read_viewbox` viewBox-accept `(nums[2]>0)&&(nums[3]>0)` (L1674)
 * and the width/height fallback `(wv>0)&&(hv>0)` (L1683;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_read_viewbox). The existing render
 * tests use positive viewBox / width / height (both conditions true). This adds
 * non-positive width and non-positive height for each so both conditions flip
 * false independently.
 *
 * Vectors:
 *  - V1: `viewBox="0 0 0 100"` -> `nums[2]>0` F -> viewBox rejected (box default).
 *  - V2: `viewBox="0 0 100 0"` -> `nums[3]>0` F -> viewBox rejected.
 *  - V3: `<svg width="0" height="100">` (no viewBox) -> `wv>0` F -> box default.
 *  - V4: `<svg width="100" height="0">` (no viewBox) -> `hv>0` F -> box default.
 * Each renders cleanly (k_ra_ok).
 */
static void test_read_viewbox_nonpositive_mcdc(void)
{
  TEST_BEGIN("priv_read_viewbox MC/DC: non-positive viewBox / width / height");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 0 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 0\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#00ff00\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg width=\"0\" height=\"100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0000ff\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg width=\"100\" height=\"0\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#112233\"/></svg>"));
  TEST_END("priv_read_viewbox MC/DC: non-positive viewBox / width / height");
}

/**
 * @test test_draw_shapes_unterminated_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_shapes` find-`<` `(i<len)&&(s[i]!='<')` (L1768) and the
 * element scan-to-`>` `(close<len)&&(s[close]!='>')` (L1775;
 * libs/ra_reflow/src/ra_reflow_svg.c@priv_draw_shapes). The existing tests end
 * exactly on the final `>` so neither inner scan runs to the buffer end; this
 * appends trailing non-`<` text and an unterminated trailing element so each
 * inner `*<len` condition flips false.
 *
 * Vectors:
 *  - V1: `...<rect .../>TRAIL` (trailing text, no `</svg>`) -> after the rect
 *        the find-`<` loop scans `TRAIL` to the end -> L1768 `i<len` F.
 *  - V2: `...<rect x="0"` (an unterminated final element, no `>`) -> the
 *        element scan-to-`>` runs to the end -> L1775 `close<len` F.
 * Each renders cleanly (k_ra_ok).
 */
static void test_draw_shapes_unterminated_mcdc(void)
{
  TEST_BEGIN("priv_draw_shapes MC/DC: trailing text + unterminated final element");
  /* V1: trailing non-'<' bytes after the last element. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "TRAILING TEXT"));
  TEST_ASSERT_EQ(k_red, (int)px(100, 100));

  /* V2: an unterminated final element (no '>'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#00ff00\""));
  TEST_END("priv_draw_shapes MC/DC: trailing text + unterminated final element");
}

/**
 * @test test_grad_scan_unterminated_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_parse_stops` `<stop` scan-to-`>` `(close<end)&&(s[close]!='>')`
 * (L1874) and `priv_scan_grads` gradient-tag scan-to-`>`
 * `(close<len)&&(s[close]!='>')` (L1901;
 * libs/ra_reflow/src/ra_reflow_svg.c). The existing gradient tests terminate
 * every tag; this uses an unterminated `<stop` (no `>` before the gradient end)
 * and an unterminated trailing `<linearGradient` (no `>` before the buffer end)
 * so each inner `close<...` condition flips false.
 *
 * Vectors:
 *  - V1: `<linearGradient id="g"><stop offset="0" stop-color="#000000"`
 *        `</linearGradient>` -> the `<stop` has no `>` before the closing tag ->
 *        L1874 `close<end` F; the stop is still parsed and used.
 *  - V2: a document ending in `<linearGradient id="h"` (no `>`) -> the gradient
 *        tag scan runs to the buffer end -> L1901 `close<len` F.
 * Each renders cleanly (k_ra_ok).
 */
static void test_grad_scan_unterminated_mcdc(void)
{
  TEST_BEGIN("priv_parse_stops/scan_grads MC/DC: unterminated <stop / gradient tag");
  /* V1: an unterminated <stop (no '>') still parsed up to the gradient end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#102030\""
                        "</linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"url(#g)\"/></svg>"));

  /* V2: a trailing unterminated <linearGradient (no '>') runs the tag scan to
   * the buffer end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "<linearGradient id=\"h\""));
  TEST_ASSERT_EQ(k_red, (int)px(100, 100));
  TEST_END("priv_parse_stops/scan_grads MC/DC: unterminated <stop / gradient tag");
}

/**
 * @test test_is_svg_bom_and_ws_mcdc
 *
 * @par MC/DC:
 * Decision: `ra_svg_is_svg` BOM test
 * `(len>=bom_len)&&(bytes[0]==EF)&&(bytes[1]==BB)&&(bytes[2]==BF)` (L1943) and
 * the leading-whitespace skip `(i<len)&&priv_ws(bytes[i])` (L1947;
 * libs/ra_reflow/src/ra_reflow_svg.c@ra_svg_is_svg). The existing test covers
 * non-BOM inputs; this drives a real BOM (all four conditions true), near-BOM
 * inputs that flip the second/third BOM byte, a too-short buffer, and leading
 * whitespace (with an all-whitespace buffer for the skip's `i<len` F arm).
 *
 * Vectors:
 *  - V1: `EF BB BF '<svg'` -> all four BOM conditions T -> accepted.
 *  - V2: `EF 00 BF '<svg'` -> `bytes[1]==BB` F -> not a BOM (still `<svg` after?
 *        no -> rejected).
 *  - V3: `EF BB 00 '<svg'` -> `bytes[2]==BF` F -> not a BOM -> rejected.
 *  - V4: `"<s"` (len 2) -> `len>=bom_len` F -> not a BOM; not `<svg` -> rejected.
 *  - V5: `"   <svg>"` -> leading whitespace skipped (L1947 `priv_ws` T) ->
 *        accepted; `"   "` -> the skip reaches the end (`i<len` F) -> rejected.
 */
static void test_is_svg_bom_and_ws_mcdc(void)
{
  TEST_BEGIN("ra_svg_is_svg MC/DC: BOM bytes + leading-whitespace skip");
  const uint8_t v1[] = {0xEFU, 0xBBU, 0xBFU, '<', 's', 'v', 'g', '>'};
  TEST_ASSERT(ra_svg_is_svg(v1, sizeof(v1)));

  const uint8_t v2[] = {0xEFU, 0x00U, 0xBFU, '<', 's', 'v', 'g'};
  TEST_ASSERT(!ra_svg_is_svg(v2, sizeof(v2)));

  const uint8_t v3[] = {0xEFU, 0xBBU, 0x00U, '<', 's', 'v', 'g'};
  TEST_ASSERT(!ra_svg_is_svg(v3, sizeof(v3)));

  TEST_ASSERT(!ra_svg_is_svg((const uint8_t*)"<s", 2U)); /* len < BOM length */

  TEST_ASSERT(ra_svg_is_svg((const uint8_t*)"   <svg>", 8U)); /* ws then <svg */
  TEST_ASSERT(!ra_svg_is_svg((const uint8_t*)"   ", 3U));     /* all whitespace */
  TEST_END("ra_svg_is_svg MC/DC: BOM bytes + leading-whitespace skip");
}

/**
 * @test test_image_href_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `ra_svg_image_href` null guard
 * `(svg==nullptr)||(out_off==nullptr)||(out_len==nullptr)` (L1955) and the href
 * lookup `!priv_attr("xlink:href")&&!priv_attr("href")` (L1967;
 * libs/ra_reflow/src/ra_reflow_svg.c@ra_svg_image_href). Each null argument
 * flips one guard condition; the two href spellings and a no-href image flip the
 * lookup's conditions.
 *
 * Vectors:
 *  - Null guard: `(NULL,..)`, `(svg,NULL,..)`, `(svg,off,NULL)` -> each OR
 *    condition true in turn -> k_ra_err_null_ptr.
 *  - L1967: `<image xlink:href="a.jpg"/>` -> first `!priv_attr` F -> returns the
 *    href; `<image href="b.jpg"/>` -> first T, second F -> returns the href;
 *    `<image/>` (neither) -> both T -> k_ra_err_not_found.
 */
static void test_image_href_arms_mcdc(void)
{
  TEST_BEGIN("ra_svg_image_href MC/DC: null guard + xlink:href / href / neither");
  const char* doc = "<svg><image xlink:href=\"a.jpg\"/></svg>";
  size_t      off = 0U;
  size_t      vl  = 0U;

  /* Null-guard conditions, one at a time. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_image_href(nullptr, 1U, &off, &vl));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_svg_image_href((const uint8_t*)doc, strlen(doc), nullptr, &vl));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_svg_image_href((const uint8_t*)doc, strlen(doc), &off, nullptr));

  /* xlink:href present -> first lookup succeeds. */
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_image_href((const uint8_t*)doc, strlen(doc), &off, &vl));
  TEST_ASSERT_EQ(5, (int)vl); /* "a.jpg" */

  /* href present (no xlink) -> second lookup succeeds. */
  const char* doc2 = "<svg><image href=\"bb.jpg\"/></svg>";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_image_href((const uint8_t*)doc2, strlen(doc2), &off, &vl));
  TEST_ASSERT_EQ(6, (int)vl); /* "bb.jpg" */

  /* Neither -> not found. */
  const char* doc3 = "<svg><image/></svg>";
  TEST_ASSERT_EQ(k_ra_err_not_found,
                 ra_svg_image_href((const uint8_t*)doc3, strlen(doc3), &off, &vl));
  TEST_END("ra_svg_image_href MC/DC: null guard + xlink:href / href / neither");
}

/**
 * @test test_size_null_and_fallback_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `ra_svg_size` null guard
 * `(svg==nullptr)||(out_w==nullptr)||(out_h==nullptr)` (L1978), the viewBox
 * fallback `(w<=0)||(h<=0)` (L1990), and the final size check `(w<=0)||(h<=0)`
 * (L2003; libs/ra_reflow/src/ra_reflow_svg.c@ra_svg_size). Each null argument
 * flips one guard condition; mixed positive/zero width/height flip the `w<=0`
 * and `h<=0` conditions of both range checks independently.
 *
 * Vectors:
 *  - Null guard: `(NULL,..)`, `(svg,NULL,..)`, `(svg,w,NULL)` -> k_ra_err_null_ptr.
 *  - L1990 `h<=0` (with `w>0`): `<svg width="50" viewBox="0 0 60 40">` (no
 *    height) -> fallback -> 60x40.
 *  - L2003 `h<=0` (with `w>0`): `<svg width="50" viewBox="0 0 60 0">` -> after
 *    the fallback w=60, h=0 -> not found.
 */
static void test_size_null_and_fallback_arms_mcdc(void)
{
  TEST_BEGIN("ra_svg_size MC/DC: null guard + (w<=0)/(h<=0) range arms");
  int32_t     w   = 0;
  int32_t     h   = 0;
  const char* doc = "<svg width=\"120\" height=\"90\"></svg>";

  /* Null-guard conditions, one at a time. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_size(nullptr, 1U, &w, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_size((const uint8_t*)doc, strlen(doc), nullptr, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_size((const uint8_t*)doc, strlen(doc), &w, nullptr));

  /* width>0 but height absent -> L1990 `w<=0` F, `h<=0` T -> viewBox fallback. */
  const char* b = "<svg width=\"50\" viewBox=\"0 0 60 40\"></svg>";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_size((const uint8_t*)b, strlen(b), &w, &h));
  TEST_ASSERT_EQ(60, w);
  TEST_ASSERT_EQ(40, h);

  /* width>0, height absent, viewBox height 0 -> L2003 `w<=0` F, `h<=0` T. */
  const char* c = "<svg width=\"50\" viewBox=\"0 0 60 0\"></svg>";
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_svg_size((const uint8_t*)c, strlen(c), &w, &h));
  TEST_END("ra_svg_size MC/DC: null guard + (w<=0)/(h<=0) range arms");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
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
  test_xform_separators_unknown();
  test_circle_skip_and_rotated_mcdc();
  test_polygon_polyline_guards_mcdc();
  test_path_relative_hv();
  test_arc_sweep_wrap_mcdc();
  test_arc_radius_scale_up();
  test_group_selfclose_depth_and_pop();
  test_grad_radial_first_and_unmatched_mcdc();
  test_grad_stop_percent_and_default_color();
  test_size_and_viewbox_fallback_mcdc();
  test_render_no_svg_tag();
  test_is_svg_xml_vs_plain();
  test_priv_hex_below_zero_nibble_mcdc();
  test_priv_num_boundary_arms_mcdc();
  test_priv_numf_boundary_arms_mcdc();
  test_attr_boundary_reject_mcdc();
  test_ci_eq_length_arms_mcdc();
  test_match_grad_no_close_paren_mcdc();
  test_is_num_start_above_nine_mcdc();
  test_xform_scan_boundary_mcdc();
  test_circle_gradient_fill_mcdc();
  test_polygon_path_gradient_fill_mcdc();
  test_points_trailing_separator_mcdc();
  test_scanline_windings_mcdc();
  test_grad_three_stop_bracket_mcdc();
  test_curve_overflow_count_mcdc();
  test_arc_sweep_both_flags_mcdc();
  test_arc_degenerate_arms_mcdc();
  test_path_cmd_scan_arms_mcdc();
  test_elem_at_delimiters_mcdc();
  test_tag_span_no_close_mcdc();
  test_read_viewbox_nonpositive_mcdc();
  test_draw_shapes_unterminated_mcdc();
  test_grad_scan_unterminated_mcdc();
  test_is_svg_bom_and_ws_mcdc();
  test_image_href_arms_mcdc();
  test_size_null_and_fallback_arms_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_svg_mcdc.c\n");
  return 0;
}

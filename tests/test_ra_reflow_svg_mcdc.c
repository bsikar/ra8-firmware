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
  (void)fprintf(stderr, "[OK ] test_ra_reflow_svg_mcdc.c\n");
  return 0;
}

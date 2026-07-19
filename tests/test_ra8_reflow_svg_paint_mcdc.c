/**
 * @file test_ra8_reflow_svg_paint_mcdc.c
 * @brief MC/DC tests for the SVG gradient / document-structure arms.
 *
 * @details
 * Split sibling of test_ra8_reflow_svg_scan_mcdc.c and
 * test_ra8_reflow_svg_shape_mcdc.c covering the paint and document decision
 * families of the SVG parser: the gradient scanner (radial-before-linear
 * ordering, stop offsets and default colours, stop-less skips, unterminated
 * defs), gradient matching by url(#id), gradient fills on circles and
 * polygons, the `<svg>` sniff (XML prolog, BOM, whitespace), element/tag
 * scanning boundaries, the width/height vs viewBox size fallbacks, the
 * image href reader, and the unterminated-document guards. Every test
 * drives the real public SVG API with crafted byte strings; the shared
 * framebuffer fixture lives in tests/support/reflow_svg_test_util.h.
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
 * @test test_grad_radial_first_and_unmatched_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_scan_grads` ordering `is_rad = (rad < lin)` and the
 * stop-count skip `if (g->nstops > 0U)`, plus `priv_match_grad`'s id-compare
 * `(strlen(gid)==idlen) && (memcmp(...)==0)`
 * (libs/ra8_reflow/src/ra8_reflow_svg.c).
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
    k_ra8_ok,
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
  TEST_ASSERT_EQ(k_red, px(100, 150));
  TEST_END("priv_scan_grads/match_grad MC/DC: radial-first, no-stop, matched/unmatched");
}

/**
 * @test test_grad_stop_percent_and_default_color
 * @brief A `<stop>` with a `%` offset and a stop with no `stop-color`
 *        exercise priv_stop_offset's `%` arm and priv_stop_color's default arm.
 *
 * @par MC/DC:
 * Decision: the '%' scan `if (tag[ooff + z] == '%')` in priv_stop_offset and
 * the absent-attribute arm `if (!ra8_svgp_attr(tag, tlen, "stop-color", ...))`
 * in priv_stop_color (libs/ra8_reflow/src/ra8_reflow_svg_doc.c; 1 condition
 * each).
 * - V1: offset "0%" / "100%" -> percent TRUE -> value scaled by 1/100.
 * - V2: bare offsets "0" / "1" -> percent FALSE
 *       (test_grad_radial_first_and_unmatched_mcdc).
 * - V3: the first stop has NO stop-color -> attr-absent TRUE -> default black.
 * - V4: the last stop's explicit stop-color -> attr-absent FALSE.
 * V1/V2 vary the percent condition; V3/V4 vary the stop-color presence; the
 * black-to-white ramp direction pins both outcomes.
 */
static void test_grad_stop_percent_and_default_color(void)
{
  TEST_BEGIN("svg gradient stop: % offset + default stop-color");
  fb_reset();
  /* The first stop has a `%` offset and NO stop-color (defaults to black); the
   * last stop is white. The ramp therefore runs black(0%) -> white(100%). */
  TEST_ASSERT_EQ(k_ra8_ok,
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
 * Decision: `ra8_svg_size` -> `if ((w<=0)||(h<=0))` (the width/height vs viewBox
 * fallback) and `priv_read_viewbox`'s width/height else-branch when no viewBox
 * is present (libs/ra8_reflow/src/ra8_reflow_svg.c).
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
  TEST_BEGIN("ra8_svg_size + priv_read_viewbox MC/DC: width/height vs viewBox");
  int32_t w = 0;
  int32_t h = 0;

  /* V1: explicit width/height, no viewBox -> attribute path. */
  const char* a = "<svg width=\"120\" height=\"90\"></svg>";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_size((const uint8_t*)a, strlen(a), &w, &h));
  TEST_ASSERT_EQ(120, w);
  TEST_ASSERT_EQ(90, h);

  /* V2: no width/height -> viewBox fallback. */
  const char* b = "<svg viewBox=\"0 0 60 40\"></svg>";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_size((const uint8_t*)b, strlen(b), &w, &h));
  TEST_ASSERT_EQ(60, w);
  TEST_ASSERT_EQ(40, h);

  /* No size at all -> not found. */
  const char* c = "<svg></svg>";
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_svg_size((const uint8_t*)c, strlen(c), &w, &h));

  /* V3: render with width/height but no viewBox -> priv_read_viewbox uses the
   * width/height as the user-space extent (vw=100, vh=100 here -> 2x into the
   * 200-box). A rect filling user 0..100 covers the whole framebuffer. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg width=\"100\" height=\"100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(190, 190)); /* far corner is inside the mapped box */
  TEST_END("ra8_svg_size + priv_read_viewbox MC/DC: width/height vs viewBox");
}

/**
 * @test test_render_no_svg_tag
 * @brief Bytes without a `<svg>` element still render cleanly (priv_read_viewbox
 *        early-returns the box default; no shapes draw).
 *
 * @par MC/DC:
 * Decision: `if (!priv_tag_span(s, len, 0U, "<svg", ...))` in
 * priv_read_viewbox (libs/ra8_reflow/src/ra8_reflow_svg_doc.c, 1 condition).
 * - V1: no `<svg` element at all -> TRUE -> the render box becomes the
 *       user space and shapes still draw (this test).
 * - V2: every sibling test with an `<svg>` element -> FALSE.
 * N=1 condition; both outcomes pinned across the suite.
 */
static void test_render_no_svg_tag(void)
{
  TEST_BEGIN("svg render: no <svg> element -> box default, no crash");
  fb_reset();
  /* No `<svg` tag at all: priv_tag_span fails -> viewBox stays the box; the
   * standalone rect is still walked and drawn at user==framebuffer coords. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<rect x=\"0\" y=\"0\" width=\"200\" height=\"200\" "
                        "fill=\"#00ff00\"/>"));
  TEST_ASSERT_EQ(k_green, px(100, 100));
  TEST_END("svg render: no <svg> element -> box default, no crash");
}

/**
 * @test test_is_svg_xml_vs_plain
 * @brief The sniff accepts a leading `<?xml` and rejects a non-markup buffer
 *        (the `<?xml || <svg` OR, the non-SVG arm).
 *
 * @par MC/DC:
 * Decision: `ra8_svg_is_svg` -> `starts_ci("<?xml") || starts_ci("<svg")`
 * (2 conditions, OR; libs/ra8_reflow/src/ra8_reflow_svg.c@ra8_svg_is_svg).
 * - V1: `<?xml ...` -> C1 T -> accepted.
 * - V2: `<svg ...`  -> C1 F, C2 T -> accepted (covered by test_ra8_svg.c too).
 * - V3: `<html>...` -> C1 F, C2 F -> rejected.
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 */
static void test_is_svg_xml_vs_plain(void)
{
  TEST_BEGIN("ra8_svg_is_svg MC/DC: <?xml / <svg / neither");
  TEST_ASSERT(ra8_svg_is_svg((const uint8_t*)"<?xml version=\"1.0\"?>", 21U));
  TEST_ASSERT(ra8_svg_is_svg((const uint8_t*)"<svg>", 5U));
  TEST_ASSERT(!ra8_svg_is_svg((const uint8_t*)"<html></html>", 13U));
  TEST_ASSERT(!ra8_svg_is_svg((const uint8_t*)"", 0U)); /* empty -> false */
  TEST_END("ra8_svg_is_svg MC/DC: <?xml / <svg / neither");
}

/**
 * @test test_match_grad_no_close_paren_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_match_grad` id-scan `(idend<vlen)&&(val[idend]!=')')` (L450;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_match_grad). The existing test uses a
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"lg\">"
                        "<stop offset=\"0\" stop-color=\"#000000\"/>"
                        "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#zz\"/>"
                        "</svg>"));
  /* The unmatched `url(#zz` is skipped, so the red backdrop survives. */
  TEST_ASSERT_EQ(k_red, px(100, 100));
  TEST_END("priv_match_grad MC/DC: url(#id with no closing paren");
}

/**
 * @test test_circle_gradient_fill_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_circle` skip guard `(gi<0)&&(fill==no_paint)` (L854) and
 * the fast-path guard `(gi<0)&&!priv_has_rot(t)` (L860;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_draw_circle). The existing test covers
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
  TEST_ASSERT_EQ(k_ra8_ok,
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
 * (L1599; libs/ra8_reflow/src/ra8_reflow_svg.c). The existing tests cover the
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
                        "<stop offset=\"1\" stop-color=\"#0000ff\"/></linearGradient></defs>"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(100, 60) != (uint32_t)k_white);

  /* V2: path with fill=none (skipped), path with no d (skipped), and a path
   * whose fill resolves to a gradient (gradient-filled). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
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
 * @test test_grad_three_stop_bracket_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_grad_eval` stop-bracket test `(p>=o0)&&(p<=o1)` (L1039;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_grad_eval). With only two stops the
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
    k_ra8_ok,
    render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
           "<stop offset=\"0\" stop-color=\"#000000\"/>"
           "<stop offset=\"0.5\" stop-color=\"#808080\"/>"
           "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
           "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#g)\"/></svg>"));
  TEST_ASSERT(px(10, 100) < px(190, 100));
  TEST_END("priv_grad_eval MC/DC: three-stop interior bracket advance");
}

/**
 * @test test_elem_at_delimiters_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_elem_at` delimiter `priv_ws(c)||(c=='>')||(c=='/')` (L1628;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_elem_at). The existing tests reach the
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
 * Each renders cleanly (k_ra8_ok).
 */
static void test_elem_at_delimiters_mcdc(void)
{
  TEST_BEGIN("priv_elem_at MC/DC: '>' / '/' / non-delimiter element boundary");
  fb_reset();
  /* V1: circle followed immediately by '>'; V2: a self-closing line; V3: a
   * `<rectangle>` whose `<rect` prefix matches but is followed by a letter. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#ff0000\"></circle>"
                        "<line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"100\" stroke=\"#00ff00\"/>"
                        "<rectangle x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#0000ff\"/>"
                        "</svg>"));
  /* The circle (with '>' delimiter) fills its disc; probe an interior point off
   * the green diagonal line so the red fill is what we observe. The bogus
   * <rectangle> is skipped, so the blue full-box fill never happens. */
  TEST_ASSERT_EQ(k_red, px(110, 100));
  TEST_END("priv_elem_at MC/DC: '>' / '/' / non-delimiter element boundary");
}

/**
 * @test test_tag_span_no_close_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_tag_span` scan-to-`>` `(j<len)&&(s[j]!='>')` (L1644;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_tag_span). The existing tests always
 * terminate the `<svg>` tag; this passes an unterminated `<svg ...` (no `>`) to
 * ::ra8_svg_size so the scan runs to the buffer end (`j<len` F).
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_size((const uint8_t*)a, strlen(a), &w, &h));
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
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_read_viewbox). The existing render
 * tests use positive viewBox / width / height (both conditions true). This adds
 * non-positive width and non-positive height for each so both conditions flip
 * false independently.
 *
 * Vectors:
 *  - V1: `viewBox="0 0 0 100"` -> `nums[2]>0` F -> viewBox rejected (box default).
 *  - V2: `viewBox="0 0 100 0"` -> `nums[3]>0` F -> viewBox rejected.
 *  - V3: `<svg width="0" height="100">` (no viewBox) -> `wv>0` F -> box default.
 *  - V4: `<svg width="100" height="0">` (no viewBox) -> `hv>0` F -> box default.
 * Each renders cleanly (k_ra8_ok).
 */
static void test_read_viewbox_nonpositive_mcdc(void)
{
  TEST_BEGIN("priv_read_viewbox MC/DC: non-positive viewBox / width / height");
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 0 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#ff0000\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 0\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#00ff00\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg width=\"0\" height=\"100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"#0000ff\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
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
 * libs/ra8_reflow/src/ra8_reflow_svg.c@priv_draw_shapes). The existing tests end
 * exactly on the final `>` so neither inner scan runs to the buffer end; this
 * appends trailing non-`<` text and an unterminated trailing element so each
 * inner `*<len` condition flips false.
 *
 * Vectors:
 *  - V1: `...<rect .../>TRAIL` (trailing text, no `</svg>`) -> after the rect
 *        the find-`<` loop scans `TRAIL` to the end -> L1768 `i<len` F.
 *  - V2: `...<rect x="0"` (an unterminated final element, no `>`) -> the
 *        element scan-to-`>` runs to the end -> L1775 `close<len` F.
 * Each renders cleanly (k_ra8_ok).
 */
static void test_draw_shapes_unterminated_mcdc(void)
{
  TEST_BEGIN("priv_draw_shapes MC/DC: trailing text + unterminated final element");
  /* V1: trailing non-'<' bytes after the last element. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "TRAILING TEXT"));
  TEST_ASSERT_EQ(k_red, px(100, 100));

  /* V2: an unterminated final element (no '>'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
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
 * libs/ra8_reflow/src/ra8_reflow_svg.c). The existing gradient tests terminate
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
 * Each renders cleanly (k_ra8_ok).
 */
static void test_grad_scan_unterminated_mcdc(void)
{
  TEST_BEGIN("priv_parse_stops/scan_grads MC/DC: unterminated <stop / gradient tag");
  /* V1: an unterminated <stop (no '>') still parsed up to the gradient end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
                        "<stop offset=\"0\" stop-color=\"#102030\""
                        "</linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                        "fill=\"url(#g)\"/></svg>"));

  /* V2: a trailing unterminated <linearGradient (no '>') runs the tag scan to
   * the buffer end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
                        "<linearGradient id=\"h\""));
  TEST_ASSERT_EQ(k_red, px(100, 100));
  TEST_END("priv_parse_stops/scan_grads MC/DC: unterminated <stop / gradient tag");
}

/**
 * @test test_is_svg_bom_and_ws_mcdc
 *
 * @par MC/DC:
 * Decision: `ra8_svg_is_svg` BOM test
 * `(len>=bom_len)&&(bytes[0]==EF)&&(bytes[1]==BB)&&(bytes[2]==BF)` (L1943) and
 * the leading-whitespace skip `(i<len)&&priv_ws(bytes[i])` (L1947;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@ra8_svg_is_svg). The existing test covers
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
  TEST_BEGIN("ra8_svg_is_svg MC/DC: BOM bytes + leading-whitespace skip");
  const uint8_t v1[] = {0xEFU, 0xBBU, 0xBFU, '<', 's', 'v', 'g', '>'};
  TEST_ASSERT(ra8_svg_is_svg(v1, sizeof(v1)));

  const uint8_t v2[] = {0xEFU, 0x00U, 0xBFU, '<', 's', 'v', 'g'};
  TEST_ASSERT(!ra8_svg_is_svg(v2, sizeof(v2)));

  const uint8_t v3[] = {0xEFU, 0xBBU, 0x00U, '<', 's', 'v', 'g'};
  TEST_ASSERT(!ra8_svg_is_svg(v3, sizeof(v3)));

  TEST_ASSERT(!ra8_svg_is_svg((const uint8_t*)"<s", 2U)); /* len < BOM length */

  TEST_ASSERT(ra8_svg_is_svg((const uint8_t*)"   <svg>", 8U)); /* ws then <svg   */
  TEST_ASSERT(!ra8_svg_is_svg((const uint8_t*)"   ", 3U));     /* all whitespace */
  TEST_END("ra8_svg_is_svg MC/DC: BOM bytes + leading-whitespace skip");
}

/**
 * @test test_image_href_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `ra8_svg_image_href` null guard
 * `(svg==nullptr)||(out_off==nullptr)||(out_len==nullptr)` (L1955) and the href
 * lookup `!priv_attr("xlink:href")&&!priv_attr("href")` (L1967;
 * libs/ra8_reflow/src/ra8_reflow_svg.c@ra8_svg_image_href). Each null argument
 * flips one guard condition; the two href spellings and a no-href image flip the
 * lookup's conditions.
 *
 * Vectors:
 *  - Null guard: `(NULL,..)`, `(svg,NULL,..)`, `(svg,off,NULL)` -> each OR
 *    condition true in turn -> k_ra8_err_null_ptr.
 *  - L1967: `<image xlink:href="a.jpg"/>` -> first `!priv_attr` F -> returns the
 *    href; `<image href="b.jpg"/>` -> first T, second F -> returns the href;
 *    `<image/>` (neither) -> both T -> k_ra8_err_not_found.
 */
static void test_image_href_arms_mcdc(void)
{
  TEST_BEGIN("ra8_svg_image_href MC/DC: null guard + xlink:href / href / neither");
  const char* doc = "<svg><image xlink:href=\"a.jpg\"/></svg>";
  size_t      off = 0U;
  size_t      vl  = 0U;

  /* Null-guard conditions, one at a time. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_image_href(nullptr, 1U, &off, &vl));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_svg_image_href((const uint8_t*)doc, strlen(doc), nullptr, &vl));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_svg_image_href((const uint8_t*)doc, strlen(doc), &off, nullptr));

  /* xlink:href present -> first lookup succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_image_href((const uint8_t*)doc, strlen(doc), &off, &vl));
  TEST_ASSERT_EQ(5, vl); /* "a.jpg" */

  /* href present (no xlink) -> second lookup succeeds. */
  const char* doc2 = "<svg><image href=\"bb.jpg\"/></svg>";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_image_href((const uint8_t*)doc2, strlen(doc2), &off, &vl));
  TEST_ASSERT_EQ(6, vl); /* "bb.jpg" */

  /* Neither -> not found. */
  const char* doc3 = "<svg><image/></svg>";
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_svg_image_href((const uint8_t*)doc3, strlen(doc3), &off, &vl));
  TEST_END("ra8_svg_image_href MC/DC: null guard + xlink:href / href / neither");
}

/**
 * @test test_size_null_and_fallback_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `ra8_svg_size` null guard
 * `(svg==nullptr)||(out_w==nullptr)||(out_h==nullptr)` (L1978), the viewBox
 * fallback `(w<=0)||(h<=0)` (L1990), and the final size check `(w<=0)||(h<=0)`
 * (L2003; libs/ra8_reflow/src/ra8_reflow_svg.c@ra8_svg_size). Each null argument
 * flips one guard condition; mixed positive/zero width/height flip the `w<=0`
 * and `h<=0` conditions of both range checks independently.
 *
 * Vectors:
 *  - Null guard: `(NULL,..)`, `(svg,NULL,..)`, `(svg,w,NULL)` -> k_ra8_err_null_ptr.
 *  - L1990 `h<=0` (with `w>0`): `<svg width="50" viewBox="0 0 60 40">` (no
 *    height) -> fallback -> 60x40.
 *  - L2003 `h<=0` (with `w>0`): `<svg width="50" viewBox="0 0 60 0">` -> after
 *    the fallback w=60, h=0 -> not found.
 */
static void test_size_null_and_fallback_arms_mcdc(void)
{
  TEST_BEGIN("ra8_svg_size MC/DC: null guard + (w<=0)/(h<=0) range arms");
  int32_t     w   = 0;
  int32_t     h   = 0;
  const char* doc = "<svg width=\"120\" height=\"90\"></svg>";

  /* Null-guard conditions, one at a time. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_size(nullptr, 1U, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_size((const uint8_t*)doc, strlen(doc), nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_size((const uint8_t*)doc, strlen(doc), &w, nullptr));

  /* width>0 but height absent -> L1990 `w<=0` F, `h<=0` T -> viewBox fallback. */
  const char* b = "<svg width=\"50\" viewBox=\"0 0 60 40\"></svg>";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_size((const uint8_t*)b, strlen(b), &w, &h));
  TEST_ASSERT_EQ(60, w);
  TEST_ASSERT_EQ(40, h);

  /* width>0, height absent, viewBox height 0 -> L2003 `w<=0` F, `h<=0` T. */
  const char* c = "<svg width=\"50\" viewBox=\"0 0 60 0\"></svg>";
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_svg_size((const uint8_t*)c, strlen(c), &w, &h));
  TEST_END("ra8_svg_size MC/DC: null guard + (w<=0)/(h<=0) range arms");
}

/**
 * @brief Test executable entry point -- runs the paint/document MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every paint/document decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_grad_radial_first_and_unmatched_mcdc();
  test_grad_stop_percent_and_default_color();
  test_size_and_viewbox_fallback_mcdc();
  test_render_no_svg_tag();
  test_is_svg_xml_vs_plain();
  test_match_grad_no_close_paren_mcdc();
  test_circle_gradient_fill_mcdc();
  test_polygon_path_gradient_fill_mcdc();
  test_grad_three_stop_bracket_mcdc();
  test_elem_at_delimiters_mcdc();
  test_tag_span_no_close_mcdc();
  test_read_viewbox_nonpositive_mcdc();
  test_draw_shapes_unterminated_mcdc();
  test_grad_scan_unterminated_mcdc();
  test_is_svg_bom_and_ws_mcdc();
  test_image_href_arms_mcdc();
  test_size_null_and_fallback_arms_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_svg_paint_mcdc.c\n");
  return 0;
}

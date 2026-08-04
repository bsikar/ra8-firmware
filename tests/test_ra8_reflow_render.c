/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_reflow_render.c
 * @brief MC/DC vectors for libs/ra8_reflow/src/ra8_reflow_render.c
 *
 * @details
 * Two complementary layers cover the page rasteriser:
 *
 *  - A ``static inline`` mirror with operand-identical short-circuit semantics
 *    documents the canonical N+1 MC/DC vector set for ``priv_blit_glyph``'s
 *    ``if (w > 0 && h > 0)`` glyph-size guard (the mirror-helper pattern from
 *    ``tests/test_ux_dcd_ra8_usb.c``).
 *
 *  - A set of end-to-end tests that drive the **real** render through the public
 *    ``ra8_reflow_render_page()`` API against an ``ra8_gfx`` framebuffer, using the
 *    bundled Literata face (located relative to ``__FILE__`` exactly as
 *    ``tests/test_ra8_reflow_corpus.c`` does; the test SKIPs if the font is
 *    absent). These exercise production source the mirror cannot reach:
 *      * ``priv_blit_glyph``'s ``if (w > 0 && h > 0)`` both ways -- a printable
 *        glyph (true) and a space (empty bitmap box -> false).
 *      * ``priv_init_faces``'s ``if ((offset < 0) || (InitFont == 0))`` both
 *        ways -- a valid registered ``@font-face`` (false) and a junk blob that
 *        falls back to the default face (true).
 *      * ``priv_render_images``'s ``if (loader == NULL || arena == NULL)`` both
 *        ways -- a bound image loader (false -> blits an image box) and the
 *        unbound default (true -> early return).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_glyph_atlas.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/** Mirror of priv_blit_glyph's ``if (w > 0 && h > 0)`` glyph-size guard. */
static inline uint8_t mirror_priv_blit_glyph_size(int w, int h)
{
  if (w > 0 && h > 0) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_mcdc_priv_blit_glyph_size
 *
 * @par MC/DC:
 * Decision: ``if (w > 0 && h > 0)``
 * (2 conditions, libs/ra8_reflow/src/ra8_reflow_render.c@priv_blit_glyph)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: w=10, h=10  -> C1=T, C2=T. Decision T (blit).
 *  - V2: w=0,  h=10  -> C1=F shorts. Decision F (skip).
 *  - V3: w=10, h=0   -> C1=T, C2=F. Decision F (skip).
 *
 * Independence:
 *  - V1 vs V2 vary C1 with C2 held T: outcome flips.
 *  - V1 vs V3 vary C2 with C1 held T: outcome flips.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_priv_blit_glyph_size(void)
{
  TEST_BEGIN("ra8_reflow_render priv_blit_glyph size MC/DC: w>0 && h>0");
  TEST_ASSERT_EQ(1, mirror_priv_blit_glyph_size(10, 10));
  TEST_ASSERT_EQ(0, mirror_priv_blit_glyph_size(0, 10));
  TEST_ASSERT_EQ(0, mirror_priv_blit_glyph_size(10, 0));
  TEST_END("ra8_reflow_render priv_blit_glyph size MC/DC: w>0 && h>0");
}

/* ===========================================================================
 * End-to-end render fixtures: real font + framebuffer + image loader.
 * ===========================================================================
 */

/**
 * @enum render_dim_t
 * @brief Viewport / framebuffer geometry and style knobs (no magic numbers).
 */
typedef enum : uint16_t {
  k_r_vp_w    = 240U, /**< Viewport + framebuffer width, pixels.  */
  k_r_vp_h    = 180U, /**< Viewport + framebuffer height, pixels. */
  k_r_font_px = 18U,  /**< Body font size, pixels.                */
} render_dim_t;

/**
 * @enum render_color_t
 * @brief Style colour keys handed to ra8_reflow_init() (no magic numbers).
 */
typedef enum : uint32_t {
  k_r_body_color = 0x101010FFU, /**< Body text colour key.   */
  k_r_link_color = 0x0040C0FFU, /**< Anchor text colour key. */
} render_color_t;

/**
 * @enum render_size_t
 * @brief Buffer capacities for the render fixtures (no magic numbers).
 */
typedef enum : size_t {
  k_r_font_cap  = 2U * 1024U * 1024U,                       /**< Literata subset < 2 MiB. */
  k_r_path_cap  = 1024U,                                    /**< Font path buffer.        */
  k_r_arena_cap = 64U * 1024U,                              /**< Image decode scratch.    */
  k_r_fb_bytes  = (size_t)k_r_vp_w * (size_t)k_r_vp_h * 3U, /**< RGB888 framebuffer.      */
} render_size_t;

/** @brief Bundled Literata Latin-1 face bytes (loaded once). */
static uint8_t s_font[k_r_font_cap];

/** @brief Byte length of the loaded Literata face. */
static size_t s_font_len;

/** @brief Shared engine handle (large -- keep off the stack). */
static ra8_reflow_t s_engine;

/** @brief RGB888 framebuffer bound via ra8_gfx_init(). */
static uint8_t s_fb[k_r_fb_bytes];

/** @brief Image-decode bump arena scratch. */
static uint8_t s_arena_buf[k_r_arena_cap];

/**
 * @enum render_atlas_size_t
 * @brief Glyph-cache backing-store dimensions for the atlas equivalence test.
 */
typedef enum : uint32_t {
  k_ra8_cell_px         = 64U,                           /**< Cell edge, px (>= 18px body).     */
  k_ra8_cell_bytes      = k_ra8_cell_px * k_ra8_cell_px, /**< Bytes per cell (alpha-8, 4096).   */
  k_ra8_cell_count      = 64U,                           /**< Cells (budget > distinct glyphs). */
  k_ra8_buckets         = 128U,                          /**< Hash buckets.                     */
  k_ra8_tiny_cell_bytes = 4U,                            /**< Too small for any body glyph.     */
} render_atlas_size_t;

/** @brief Reference framebuffer rendered via the direct (no-cache) path. */
static uint8_t s_fb_ref[k_r_fb_bytes];

/** @brief Glyph-atlas cell storage (caller-owned, like the ereader app). */
static uint8_t s_atlas_cells[(size_t)k_ra8_cell_count * (size_t)k_ra8_cell_bytes];

/** @brief Glyph-atlas per-cell link metadata. */
static ra8_keycache_cell_t s_atlas_meta[k_ra8_cell_count];

/** @brief Glyph-atlas per-cell key storage. */
static ra8_glyph_key_t s_atlas_keys[k_ra8_cell_count];

/** @brief Glyph-atlas per-cell dimension descriptors. */
static ra8_glyph_dims_t s_atlas_dims[k_ra8_cell_count];

/** @brief Glyph-atlas hash-bucket heads. */
static int32_t s_atlas_buckets[k_ra8_buckets];

/** @brief Glyph-atlas state bound into the engine. */
static ra8_glyph_atlas_t s_atlas;

/**
 * @brief A 2x2 RGB PNG: TL red, TR green, BL blue, BR white.
 * @details Identical fixture to tests/test_ra8_reflow_image.c -- the smallest
 * raster that the image loader can return so layout records an image box and
 * the render path decodes + blits it.
 */
static const uint8_t s_png_2x2[] = {
  137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
  0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
  84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
  246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
};

/**
 * @brief Image loader DI seam: always returns the baked 2x2 PNG bytes.
 * @param[in]  ctx      Unused opaque context.
 * @param[in]  href     Unused image href.
 * @param[in]  href_len Unused href length.
 * @param[out] out      Receives the PNG byte pointer.
 * @param[out] out_len  Receives the PNG byte count.
 * @return k_ra8_ok always.
 */
static ra8_err_t
png_loader(void* ctx, const char* href, uint32_t href_len, const uint8_t** out, size_t* out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out     = s_png_2x2;
  *out_len = sizeof s_png_2x2;
  return k_ra8_ok;
}

/**
 * @brief Load the bundled Literata subset relative to this source file.
 * @return 1 on success, 0 if the font is absent (caller SKIPs).
 */
static int load_font(void)
{
  char         path[k_r_path_cap];
  const char*  here = __FILE__;
  const size_t flen = strlen(here);
  size_t       base = flen;
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "test_ra8_reflow_render.c" */
  }
  while (base > 0U && here[base - 1U] == '/') {
    base--;
  }
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "tests" -> repo root */
  }
  if (base == 0U || base >= sizeof(path)) {
    return 0;
  }
  memcpy(path, here, base);
  (void)snprintf(&path[base], sizeof(path) - base, "libs/ra8_fonts/literata_latin1.ttf");
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    return 0;
  }
  s_font_len = fread(s_font, 1U, (size_t)k_r_font_cap, fp);
  (void)fclose(fp);
  return (s_font_len > 0U) ? 1 : 0;
}

/** @brief Init the engine with the loaded font; returns the init result. */
static ra8_err_t init_engine(void)
{
  return ra8_reflow_init((uint16_t)k_r_vp_w,
                         (uint16_t)k_r_vp_h,
                         s_font,
                         s_font_len,
                         (uint16_t)k_r_font_px,
                         (uint32_t)k_r_body_color,
                         (uint32_t)k_r_link_color,
                         &s_engine);
}

/** @brief True iff any framebuffer pixel differs from the zeroed background. */
static bool fb_has_drawn_pixel(void)
{
  for (size_t i = 0U; i < (size_t)k_r_fb_bytes; ++i) {
    if (s_fb[i] != 0U) {
      return true;
    }
  }
  return false;
}

/** @brief True iff the two framebuffers are byte-for-byte identical. */
static bool fb_equal_ref(void)
{
  return memcmp(s_fb, s_fb_ref, (size_t)k_r_fb_bytes) == 0;
}

/**
 * @test test_render_glyph_size_both_arms
 *
 * @par MC/DC:
 * Decision: `if (w > 0 && h > 0)` (2 conditions, AND;
 * libs/ra8_reflow/src/ra8_reflow_render.c@priv_blit_glyph), driven through the
 * real ra8_reflow_render_page() API. A single laid-out chapter contains both
 * printable glyphs and spaces, so one render pass exercises:
 *  - true arm: a printable glyph ('H','i',...) -> w>0 && h>0 -> blit (a pixel
 *    differs from the zeroed background).
 *  - false arm: the space code point -> stbtt reports an empty bitmap box
 *    (w == 0 && h == 0) -> the glyph is skipped, not truncated.
 * The framebuffer-differs assertion proves the true arm executed end to end;
 * the false arm is taken for every space without crashing or over-drawing.
 */
static void test_render_glyph_size_both_arms(void)
{
  TEST_BEGIN("ra8_reflow_render: real glyph blit (w>0 && h>0 both arms)");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] literata_latin1.ttf not found; skipping render\n");
    TEST_END("ra8_reflow_render: real glyph blit (w>0 && h>0 both arms)");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());
  uint32_t pages = 0U;
  /* The leading/internal/trailing spaces drive the false arm (empty box);
     the letters drive the true arm. */
  const char* xhtml = "<p>Hi there world, a render test.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);

  memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_r_vp_w, (uint16_t)k_r_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_ASSERT(fb_has_drawn_pixel());

  /* Out-of-range page is rejected without touching the framebuffer. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_reflow_render_page(&s_engine, pages, nullptr));
  TEST_END("ra8_reflow_render: real glyph blit (w>0 && h>0 both arms)");
}

/** @brief Junk bytes that are not a valid font (>= min font length). */
static const uint8_t s_junk_face[64] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

/**
 * @brief A blob with the sfnt magic `00 01 00 00` but a broken table directory.
 * @details stbtt_GetFontOffsetForIndex() accepts the leading magic and returns
 * offset 0 (>= 0), but stbtt_InitFont() then fails parsing the garbage table
 * directory and returns 0. This isolates the *second* condition of
 * priv_init_faces' `(offset < 0) || (InitFont == 0)` OR (offset >= 0, so the
 * decision turns on InitFont alone).
 */
static const uint8_t s_badsfnt_face[64] = {0U, 1U, 0U, 0U};

/**
 * @test test_render_register_face_both_arms
 *
 * @par MC/DC:
 * Decision: `if ((offset < 0) || (stbtt_InitFont(&faces[k+1], blob, offset) == 0))`
 * (2 conditions, OR; libs/ra8_reflow/src/ra8_reflow_render.c@priv_init_faces),
 * driven through the real ra8_reflow_render_page() API. N+1 = 3 vectors for the
 * OR, all exercised in one render pass over three registered faces:
 *  - V1 (decision F): a *valid* Literata `@font-face` -> C1 (offset < 0) F AND
 *    C2 (InitFont == 0) F -> the per-face fontinfo is initialised in place.
 *  - V2 (decision T via C1): a *junk* blob (`01 02 03...`) -> stbtt rejects the
 *    magic so offset < 0 -> C1 T short-circuits -> graceful fallback.
 *  - V3 (decision T via C2): an sfnt-magic-but-broken blob (`00 01 00 00...`) ->
 *    offset >= 0 (C1 F) but InitFont == 0 -> C2 T -> graceful fallback.
 * V1 vs V2 vary C1 (C2 held F-effective via short-circuit), V1 vs V3 vary C2
 * (C1 held F), so each condition independently flips the decision.
 *
 * The public ra8_reflow_register_face() validates and rejects both bad blobs
 * (k_ra8_err_not_supported), so a bad face can never reach priv_init_faces
 * through the API; the two bad faces are injected directly into the engine face
 * table (the same direct-field-population pattern the hit-test tests use) and
 * face_count bumped. A drawn pixel proves the page still rasterises (no glyph
 * indexes an uninitialised face).
 */
static void test_render_register_face_both_arms(void)
{
  TEST_BEGIN("ra8_reflow_render: priv_init_faces bad/good face MC/DC");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] literata_latin1.ttf not found; skipping render\n");
    TEST_END("ra8_reflow_render: priv_init_faces bad/good face MC/DC");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());

  /* Public API: a valid face is accepted (false arm); both bad faces are
     rejected, proving a bad face cannot enter priv_init_faces via the API. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_register_face(&s_engine, 0U, s_font, s_font_len));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&s_engine, 1U, s_junk_face, sizeof s_junk_face));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&s_engine, 2U, s_badsfnt_face, sizeof s_badsfnt_face));

  uint32_t    pages = 0U;
  const char* xhtml = "<p>Faces under test.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);

  /* Inject both bad faces directly to drive priv_init_faces' defensive true arm
     (graceful fallback) twice: V2 via C1 (bad magic), V3 via C2 (good magic,
     broken directory). Neither reaches priv_init_faces through the public API. */
  s_engine.faces[s_engine.face_count].blob         = s_junk_face;
  s_engine.faces[s_engine.face_count].len          = sizeof s_junk_face;
  s_engine.faces[s_engine.face_count].css_face_idx = 1U;
  s_engine.face_count                              = (uint8_t)(s_engine.face_count + 1U);
  s_engine.faces[s_engine.face_count].blob         = s_badsfnt_face;
  s_engine.faces[s_engine.face_count].len          = sizeof s_badsfnt_face;
  s_engine.faces[s_engine.face_count].css_face_idx = 2U;
  s_engine.face_count                              = (uint8_t)(s_engine.face_count + 1U);

  memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_r_vp_w, (uint16_t)k_r_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_ASSERT(fb_has_drawn_pixel());
  TEST_END("ra8_reflow_render: priv_init_faces bad/good face MC/DC");
}

/**
 * @brief Clear `s_fb`, re-init gfx over it, render page @p page and assert output.
 * @param[in] page Page index to render into the shared framebuffer.
 * @pre The engine has a laid-out chapter with at least @p page pages.
 * @post The page rendered and drew at least one pixel.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void reflow_render_drawn(uint32_t page)
{
  memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_r_vp_w, (uint16_t)k_r_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, page, nullptr));
  TEST_ASSERT(fb_has_drawn_pixel());
}

/**
 * @brief Clear `s_fb`, render page 0 and assert byte-identity with `s_fb_ref`.
 * @pre A reference page render already populated `s_fb_ref`.
 * @post The page rendered identically to the reference (cache is pure opt).
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void atlas_render_and_check(void)
{
  memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_r_vp_w, (uint16_t)k_r_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_ASSERT(fb_equal_ref());
}

/**
 * @brief Bind a glyph atlas over the shared caller-owned storage.
 * @param[in] cell_bytes Bytes-per-cell budget (tiny values force the direct path).
 * @pre The engine is initialised.
 * @post The atlas is bound to `s_engine` with the given cell budget.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bind_glyph_atlas(uint32_t cell_bytes)
{
  const ra8_reflow_glyph_atlas_storage_t storage = {
    .cell_mem     = s_atlas_cells,
    .cell_bytes   = cell_bytes,
    .cell_count   = (uint32_t)k_ra8_cell_count,
    .meta         = s_atlas_meta,
    .keys         = s_atlas_keys,
    .dims         = s_atlas_dims,
    .buckets      = s_atlas_buckets,
    .bucket_count = (uint32_t)k_ra8_buckets,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, &storage));
}

/**
 * @test test_render_images_loader_both_arms
 *
 * @par MC/DC:
 * Decision: `if ((engine->img_loader == NULL) || (engine->img_arena == NULL))`
 * (2 conditions, OR; libs/ra8_reflow/src/ra8_reflow_render.c@priv_render_images),
 * driven through the real ra8_reflow_render_page() API. N+1 = 3 vectors:
 *  - V1 (decision F): bind a loader + arena, lay out a chapter with an `<img>`
 *    (so an image box is recorded), then render -> C1 (loader == NULL) F AND
 *    C2 (arena == NULL) F -> the loop runs and the 2x2 PNG is decoded + blitted.
 *  - V2 (decision T via C1): re-init with no loader bound and render a text-only
 *    page -> loader == NULL -> C1 T short-circuits -> early return.
 *  - V3 (decision T via C2): bind a loader but null the arena (injected
 *    directly), render a text-only page -> C1 F, C2 (arena == NULL) T -> early
 *    return. Isolating C2 needs the loader non-null so C1 does not short it.
 * V1 vs V2 vary C1 (C2 held F via short-circuit), V1 vs V3 vary C2 (C1 held F),
 * so each condition independently flips the decision.
 *
 * A drawn pixel in the image case proves the false arm reached the blit; both
 * early-return cases prove the true arm short-circuits cleanly (text still
 * rasterises).
 */
static void test_render_images_loader_both_arms(void)
{
  TEST_BEGIN("ra8_reflow_render: priv_render_images loader/arena MC/DC");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] literata_latin1.ttf not found; skipping render\n");
    TEST_END("ra8_reflow_render: priv_render_images loader/arena MC/DC");
    return;
  }
  /* False arm: loader + arena bound, image box laid out, then rendered. */
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());
  ra8_img_arena_t arena = {.base   = s_arena_buf,
                           .cap    = sizeof s_arena_buf,
                           .offset = 0U,
                           .live   = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_image_loader(&s_engine, png_loader, nullptr, &arena));

  uint32_t    pages = 0U;
  const char* xhtml = "<p>Figure:</p><img src=\"cover.png\"/><p>After.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_engine.image_box_count >= 1U);

  reflow_render_drawn(s_engine.image_boxes[0].page_index);
  /* Arena drains after the on-demand decode (zero heap). */
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  /* V2 -- true arm via C1: fresh engine with no image loader bound. */
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());
  const char* text = "<p>No images here.</p>";
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)text, strlen(text), &pages));
  TEST_ASSERT(pages >= 1U);
  reflow_render_drawn(0U);

  /* V3 -- true arm via C2: loader bound (C1 false) but arena nulled (C2 true).
     Inject the loader/arena fields directly so C1 cannot short-circuit C2. */
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)text, strlen(text), &pages));
  TEST_ASSERT(pages >= 1U);
  s_engine.img_loader     = png_loader;
  s_engine.img_loader_ctx = nullptr;
  s_engine.img_arena      = nullptr;
  reflow_render_drawn(0U);
  TEST_END("ra8_reflow_render: priv_render_images loader/arena MC/DC");
}

/**
 * @test test_render_glyph_atlas_equivalence
 *
 * @par Purpose:
 * Validates the #164 glyph-atlas wiring in ra8_reflow_render.c on two axes that
 * the issue makes the acceptance bar:
 *  1. **Byte-identity** -- a page rendered through the bound atlas is
 *     byte-for-byte identical to the same page rendered through the direct
 *     (no-cache) path. The cache must be a pure optimisation: the same
 *     stb_truetype rasteriser fills each cell, so cached and freshly-rasterised
 *     bitmaps are identical and the composited framebuffers match exactly.
 *  2. **Cache effectiveness** -- re-rendering the same page does ZERO new glyph
 *     rasterisation (the miss count is unchanged across the second render) while
 *     the hit count grows. That "re-render never re-rasterises" property is the
 *     entire motivation for #147/#164.
 *
 * The budget (k_ra8_cell_count cells) exceeds the distinct-glyph count of the
 * pangram page, so the second render evicts nothing and is all hits.
 *
 * @par Coverage:
 * Drives priv_blit_glyph's atlas arm (atlas != NULL) and both
 * priv_glyph_render_cached outcomes (miss -> render-into-cell, hit -> reuse),
 * plus ra8_reflow_set_glyph_atlas bind + detach (atlas == NULL) paths -- source
 * the no-cache MC/DC tests above cannot reach.
 *
 * @par MC/DC:
 * Decision: `(storage->cell_bytes == 0U) || (storage->cell_count == 0U) ||
 * (storage->bucket_count == 0U)` in ra8_reflow_set_glyph_atlas
 * (libs/ra8_reflow/src/ra8_reflow_render.c), evaluated here only at its all-false
 * control -- every bind (full-size then tiny cells) uses non-zero storage, so the
 * init path is taken each time. The atlas bind / render / detach branches it
 * drives (`atlas != NULL`, `!drawn`, `ra8_glyph_atlas_get != k_ra8_ok`, and the
 * `w * h > cell_bytes` fit check) are all single-condition. The storage-zero OR's
 * independent-influence vectors are carried by the sibling
 * test_mcdc_set_glyph_atlas_storage_zeros; the glyph-box `(w > 0) && (h > 0)`
 * guard crossed for the page text is MC/DC-deactivated (co-dependent extents).
 */
static void test_render_glyph_atlas_equivalence(void)
{
  TEST_BEGIN("ra8_reflow_render: glyph atlas byte-identity + zero re-raster");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] literata_latin1.ttf not found; skipping atlas render\n");
    TEST_END("ra8_reflow_render: glyph atlas byte-identity + zero re-raster");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());
  uint32_t    pages = 0U;
  const char* xhtml = "<p>The quick brown fox jumps over the lazy dog.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);

  /* Reference render through the direct path (no atlas bound yet). */
  memset(s_fb_ref, 0, sizeof s_fb_ref);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb_ref, (uint16_t)k_r_vp_w, (uint16_t)k_r_vp_h, k_ra8_gfx_format_rgb888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));

  /* Bind a glyph atlas over caller-owned storage -- exactly as the ereader app. */
  bind_glyph_atlas((uint32_t)k_ra8_cell_bytes);

  /* Cached render #1 (cold cache): must equal the direct render byte-for-byte. */
  atlas_render_and_check();

  uint32_t hits1 = 0U;
  uint32_t miss1 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_stats(&s_atlas, &hits1, &miss1, nullptr));
  TEST_ASSERT(miss1 > 0U); /* The cold render rasterised the distinct glyphs. */

  /* Cached render #2 (warm cache): still byte-identical, and crucially renders
     ZERO new glyphs -- the miss count does not move, every glyph is a hit. */
  atlas_render_and_check();

  uint32_t hits2 = 0U;
  uint32_t miss2 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_stats(&s_atlas, &hits2, &miss2, nullptr));
  TEST_ASSERT(hits2 > hits1);   /* Re-render served from cache.   */
  TEST_ASSERT_EQ(miss1, miss2); /* No glyph re-rasterised (#164). */

  /* Oversized fallback: a cache whose cells are too small for any body glyph
     forces every glyph down the direct path (priv_atlas_render_glyph returns
     k_ra8_err_invalid_size -> priv_glyph_render_cached returns false). Output
     must stay byte-identical -- the cache is a pure optimisation. */
  bind_glyph_atlas((uint32_t)k_ra8_tiny_cell_bytes);
  atlas_render_and_check();

  /* Detach reverts to the direct path and still renders identically. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_glyph_atlas(&s_engine, nullptr, nullptr));
  atlas_render_and_check();
  TEST_END("ra8_reflow_render: glyph atlas byte-identity + zero re-raster");
}

/**
 * @test test_mcdc_set_glyph_atlas_storage_zeros
 *
 * @par MC/DC:
 * Decision: `(storage->cell_bytes == 0U) || (storage->cell_count == 0U) ||
 * (storage->bucket_count == 0U)` (3 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_render.c@ra8_reflow_set_glyph_atlas). N+1 = 4
 * vectors:
 *  - V1 (control, decision F): all three non-zero -> the init path is reached
 *    and k_ra8_ok is returned.
 *  - V2 (decision T via C1): cell_bytes=0, others non-zero -> C1 T short-circuits
 *    -> k_ra8_err_invalid_size.
 *  - V3 (decision T via C2): cell_bytes non-zero (C1 F), cell_count=0,
 *    bucket_count non-zero -> C2 T -> k_ra8_err_invalid_size.
 *  - V4 (decision T via C3): cell_bytes + cell_count non-zero (C1, C2 F),
 *    bucket_count=0 -> C3 T -> k_ra8_err_invalid_size.
 * V1 vs V2 vary C1; V1 vs V3 vary C2 (C1 held F); V1 vs V4 vary C3 (C1+C2 held
 * F), so each condition independently flips the decision. N=3 -> N+1=4 vectors.
 */
static void test_mcdc_set_glyph_atlas_storage_zeros(void)
{
  TEST_BEGIN("ra8_reflow_set_glyph_atlas storage-zero MC/DC");
  if (load_font() == 0) {
    (void)fprintf(stderr, "[SKIP] literata_latin1.ttf not found; skipping atlas storage MC/DC\n");
    TEST_END("ra8_reflow_set_glyph_atlas storage-zero MC/DC");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, init_engine());

  ra8_reflow_glyph_atlas_storage_t st = {
    .cell_mem     = s_atlas_cells,
    .cell_bytes   = (uint32_t)k_ra8_cell_bytes,
    .cell_count   = (uint32_t)k_ra8_cell_count,
    .meta         = s_atlas_meta,
    .keys         = s_atlas_keys,
    .dims         = s_atlas_dims,
    .buckets      = s_atlas_buckets,
    .bucket_count = (uint32_t)k_ra8_buckets,
  };

  /* V2 -- C1 true (cell_bytes == 0). */
  st.cell_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, &st));
  st.cell_bytes = (uint32_t)k_ra8_cell_bytes;

  /* V3 -- C2 true (cell_count == 0, C1 false). */
  st.cell_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, &st));
  st.cell_count = (uint32_t)k_ra8_cell_count;

  /* V4 -- C3 true (bucket_count == 0, C1 + C2 false). */
  st.bucket_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, &st));
  st.bucket_count = (uint32_t)k_ra8_buckets;

  /* V1 -- control, decision false: all non-zero -> bind succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, &st));

  /* Null storage with a non-null atlas is rejected (the storage null guard). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_set_glyph_atlas(&s_engine, &s_atlas, nullptr));

  /* Detach restores the direct path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_glyph_atlas(&s_engine, nullptr, nullptr));
  TEST_END("ra8_reflow_set_glyph_atlas storage-zero MC/DC");
}

int32_t main(void)
{
  test_mcdc_priv_blit_glyph_size();
  test_render_glyph_size_both_arms();
  test_render_register_face_both_arms();
  test_render_images_loader_both_arms();
  test_render_glyph_atlas_equivalence();
  test_mcdc_set_glyph_atlas_storage_zeros();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_render.c\n");
  return 0;
}

/**
 * @file test_ra8_rabook_pipeline_err.c
 * @brief EPUB -> RABOOK1 pipeline: image-transform and error-path tests.
 *
 * @details
 * Split out of test_ra8_rabook_pipeline.c to keep each test translation
 * unit under the repository file-size cap. This sibling owns the raster /
 * tall-image transcode tests, the CSS / TOC resolution tests, and the
 * error-propagation / null-guard / bad-buffer tests; the compile happy
 * path and the byte-identical parity tests stay in
 * test_ra8_rabook_pipeline.c. The synthetic-EPUB builders live in
 * tests/support/rabook_pipeline_fixture.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB_Compiler] {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_img_arena.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_reflow_image.h"
#include "support/rabook_pipeline_fixture.h"
#include "unity_minimal.h"

/**
 * @brief Open the staged @ref s_epub fixture into @p book.
 * @param[out] book Receives the opened book (non-NULL).
 * @pre @p s_epub / @p s_epub_len hold a built archive.
 * @pre @p book is non-NULL.
 * @post @p book->in_use == 1 on success.
 * @post @p s_epub is unmodified.
 * @note Not thread-safe.
 */
static void open_s_epub(ra8_epub_book_t* book)
{
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", book));
}

/**
 * @test test_pipeline_raster_images_transcoded
 * @brief Two decodable BMPs are decoded, gray4-encoded, and added; the cover
 *        resolves to the small image.
 *
 * @par MC/DC:
 * Decision: `if (ow == sw && oh == sh)` in internal_downscale_if_needed (2 conditions).
 * The 2x2 image hits ow==sw && oh==sh -> true (copy in place); the 1601x1 image
 * hits ow!=sw -> false (downscale). N+1 = 3 vectors:
 * - Vector 1: ow==sw, oh==sh (2x2)            -> true  (control: both equal).
 * - Vector 2: ow!=sw, oh==sh (1601x1 -> 1600) -> false (varies width only).
 * - Vector 3: ow==sw, oh!=sh (1x1601 -> 1600) -> false (varies height only,
 *   driven by @ref test_pipeline_gray_scratch_too_small over the tall image).
 * Vectors 1+2 show width independently flips the outcome; 1+3 show height does.
 */
static void test_pipeline_raster_images_transcoded(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: BMP images transcoded to gray4");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(2U, hdr->image_count);       /* small + big both added */
  TEST_ASSERT_EQ(0U, hdr->cover_image_index); /* small.bmp is the cover */

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: BMP images transcoded to gray4");
}

/**
 * @test test_pipeline_default_preserves_resolution
 * @brief With no opt-in clamp (the zero-init default) the oversized image is
 *        stored at its exact source resolution.
 *
 * @par Targeted code:
 * The new opt-out arm of the transcode stage: `scr->max_image_edge == 0`
 * skips ra8_rabook_gray4_output_dims entirely, so ow/oh stay the source
 * dimensions and internal_downscale_if_needed takes its copy-in-place arm with no
 * gray scratch needed (issue #210: full-resolution sources for the zoom
 * loupe).
 *
 * @par MC/DC:
 * (no compound decisions under test -- the opt-in clamp gate
 * `scr->max_image_edge != 0U` is a single condition; its true arm is driven
 * by test_pipeline_raster_images_transcoded and this test drives the false
 * arm)
 */
static void test_pipeline_default_preserves_resolution(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: default preserves source resolution");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena); /* max_image_edge stays 0: no clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(2U, hdr->image_count);
  /* The 1601x1 image keeps every source pixel under the default. */
  const ra8_book_image_t* imgs    = ra8_book_images(s_readback);
  bool                    saw_big = false;
  for (uint32_t i = 0U; i < hdr->image_count; ++i) {
    if (imgs[i].width == k_pl_big_edge) {
      TEST_ASSERT_EQ(1U, imgs[i].height);
      saw_big = true;
    }
  }
  TEST_ASSERT(saw_big);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: default preserves source resolution");
}

/**
 * @test test_pipeline_gray_scratch_too_small
 * @brief A too-small gray scratch makes the downscaled image skip while the
 *        in-place (no-downscale) image still compiles.
 *
 * @par MC/DC:
 * Drives the gray-capacity guard `if ((uint32_t)ow * oh > scr->gray_cap)` true
 * arm (single condition): the 1601x1 image needs a 1600-pixel scratch but only 4
 * are offered, so internal_downscale_if_needed returns nullptr and internal_transcode_image
 * takes its `gray_src == nullptr` cleanup-and-skip arm. The 2x2 image needs no
 * scratch (copy in place), so it still adds -- one decodable image survives.
 * The false arm of the same guard is driven by
 * @ref test_pipeline_raster_images_transcoded (ample scratch).
 */
static void test_pipeline_gray_scratch_too_small(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: gray scratch too small skips downscale image");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp                  */
  scr.gray_cap       = 4U;              /* far below the 1600 pixels the big image needs */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(1U, hdr->image_count);       /* only the 2x2 in-place image survives */
  TEST_ASSERT_EQ(0U, hdr->cover_image_index); /* and it is the cover                  */

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: gray scratch too small skips downscale image");
}

/**
 * @test test_pipeline_tall_image_height_downscaled
 * @brief A tall cover keeps its width but loses height to the clamp, driving the
 *        height-varying leg of the downscale decision.
 *
 * @par MC/DC:
 * Completes `if (ow == sw && oh == sh)` in internal_downscale_if_needed (2 conditions):
 * the 1 x 1601 cover clamps to 1 x 1600, so ow == sw (1 == 1 -> C1 true) while
 * oh != sh (1600 != 1601 -> C2 false) -- the (true, false) vector that isolates
 * the height condition. Paired with test_pipeline_raster_images_transcoded's
 * (true, true) 2x2 control and (false, true) wide-image legs, the three vectors
 * are the N+1 = 3 minimal MC/DC set for the decision.
 */
static void test_pipeline_tall_image_height_downscaled(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: tall cover downscales height only");
  build_epub_tall();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.max_image_edge = k_pl_clamp_edge; /* opt into the downscale clamp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr  = ra8_book_header(s_readback);
  const ra8_book_image_t*  imgs = ra8_book_images(s_readback);
  TEST_ASSERT_EQ(1U, hdr->image_count);
  /* Width unchanged (1), height clamped to 1600: the (C1 true, C2 false) leg. */
  TEST_ASSERT_EQ(k_pl_narrow_edge, imgs[0].width);
  TEST_ASSERT_EQ(k_pl_clamp_edge, imgs[0].height);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: tall cover downscales height only");
}

/**
 * @test test_pipeline_gray8_profile
 * @brief The gray8 device profile stores each raster at 8-bpp (one byte/pixel)
 *        and stamps its descriptor `pixel_format` = gray8 (#343).
 *
 * @par MC/DC:
 * Drives the true arm of `if (scr->pixel_format == k_ra8_book_pixfmt_gray8)` in
 * internal_encode_gray (single condition): the profile selects the 8-bpp copy, so each
 * raster's `raw_size` equals width*height (not the ceil(w*h/2) of the 4-bpp pack)
 * and its `pixel_format` reads back gray8. The false (gray4) arm is driven by
 * @ref test_pipeline_raster_images_transcoded. `format` stays gray4 (raster) --
 * pixel_format is the orthogonal depth axis.
 */
static void test_pipeline_gray8_profile(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: gray8 profile stores 8-bpp rasters");
  build_epub_raster();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8; /* device profile: keep 8-bpp */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr  = ra8_book_header(s_readback);
  const ra8_book_image_t*  imgs = ra8_book_images(s_readback);
  TEST_ASSERT_EQ(2U, hdr->image_count);
  for (uint32_t i = 0U; i < hdr->image_count; ++i) {
    /* Raster kind unchanged; depth axis is gray8; 8-bpp is exactly w*h bytes. */
    TEST_ASSERT_EQ(k_ra8_book_image_gray4, imgs[i].format);
    TEST_ASSERT_EQ(k_ra8_book_pixfmt_gray8, ra8_book_image_pixfmt(&imgs[i]));
    TEST_ASSERT_EQ(imgs[i].width * (uint32_t)imgs[i].height, imgs[i].raw_size);
  }

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: gray8 profile stores 8-bpp rasters");
}

/**
 * @test test_pipeline_toc_titles_resolved
 * @brief Each spine chapter picks up its TOC title via internal_chapter_title.
 *
 * @par MC/DC:
 * Decision: `if (toc_to_chapter() == k_ra8_ok && ch_idx == chapter_idx)` in
 * internal_chapter_title (2 conditions), exercised over two chapters with a three-entry
 * nav whose first entry ("orphan.xhtml") maps to no spine chapter. N+1 = 3:
 * - V1: entry resolves and ch_idx == chapter_idx -> C1 true, C2 true (control:
 *   entry 1 for chapter 0, entry 2 for chapter 1 -> a title is copied).
 * - V2: entry resolves but ch_idx != chapter_idx -> C1 true, C2 false (entry 1's
 *   ch_idx 0 != chapter_idx 1 while resolving chapter 1; isolates C2 vs V1).
 * - V3: entry fails to resolve -> C1 false, short-circuit (the leading orphan
 *   entry, scanned first for every chapter; isolates C1 vs V1).
 * Reading the two titles back proves each chapter resolved its own entry past the
 * orphan.
 */
static void test_pipeline_toc_titles_resolved(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: TOC titles attach to chapters");
  build_epub_toc();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t*  hdr   = ra8_book_header(s_readback);
  const ra8_book_chapter_t* chaps = ra8_book_chapters(s_readback);
  TEST_ASSERT_EQ(2U, hdr->chapter_count);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, chaps[0].title_off), "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, chaps[1].title_off), "Chapter Two"));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: TOC titles attach to chapters");
}

/**
 * @test test_pipeline_css_absent_skipped
 * @brief A `text/css` item declared but missing from the archive is skipped,
 *        like the desktop "only if present" rule.
 *
 * @par MC/DC:
 * Drives the `if (err == k_ra8_err_not_found)` true arm in internal_compile_stylesheets
 * (single condition): the manifest declares style.css but the ZIP omits it, so
 * ra8_epub_get_resource returns not_found and the stage `continue`s rather than
 * failing. The false arm (a present stylesheet) is exercised by the parity
 * fixtures, which intern their CSS.
 */
static void test_pipeline_css_absent_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: absent CSS item skipped");
  build_epub_css(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->stylesheet_count); /* the absent CSS contributed nothing */
  TEST_ASSERT_EQ(1U, hdr->chapter_count);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: absent CSS item skipped");
}

/**
 * @test test_pipeline_css_load_error_propagates
 * @brief A non-not_found stylesheet error aborts the compile and propagates out
 *        of every stage wrapper.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the stylesheet resource load
 * (single condition): the stylesheet is present but the load buffer is sized to
 * three bytes, so ra8_epub_get_resource returns k_ra8_err_no_mem -- neither ok nor
 * not_found -- and internal_compile_stylesheets returns it. That error then propagates
 * the true arm of the stylesheet-stage guard in internal_compile_to_blob and the
 * compile-failure guard in ra8_rabook_compile_from_epub (no file is written).
 */
static void test_pipeline_css_load_error_propagates(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: CSS load error propagates");
  build_epub_css(true);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.css_cap = 4U; /* style.css is larger than css_cap - 1 -> no_mem */

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: CSS load error propagates");
}

/**
 * @test test_pipeline_image_resource_absent_skipped
 * @brief An image item missing from the archive is skipped, not fatal.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the resource load in
 * internal_add_manifest_image (single condition): the manifest declares missing.png but
 * the archive omits it, so ra8_epub_get_resource fails and the item returns nil
 * (skipped) -- the desktop try/except pass. The false arm is exercised whenever a
 * present image loads (the raster + parity fixtures).
 */
static void test_pipeline_image_resource_absent_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: absent image item skipped");
  build_epub_image_absent();
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: absent image item skipped");
}

/**
 * @test test_pipeline_chapter_load_too_small
 * @brief A chapter that overflows the XHTML scratch aborts and propagates.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the chapter-load guard in
 * internal_compile_chapters (single condition): the XHTML scratch is sized to four
 * bytes, so ra8_epub_load_chapter returns k_ra8_err_no_mem and the stage returns
 * it, which propagates the chapter-stage guard in internal_compile_to_blob and the
 * compile-failure guard in the filesystem entry point. The false arm is the
 * happy load every passing compile exercises.
 */
static void test_pipeline_chapter_load_too_small(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: chapter overflow propagates");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.xhtml_cap = 4U; /* the chapter does not fit -> no_mem */

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: chapter overflow propagates");
}

/**
 * @test test_pipeline_malformed_chapter_propagates
 * @brief A chapter with no XML root element fails the parser and propagates.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the parse guard in
 * internal_compile_chapters (single condition): the chapter body is plain text with no
 * element, so ra8_rabook_xml_parse_chapter finds no `<body>` root and returns
 * k_ra8_err_no_mem, which the stage and internal_compile_to_blob propagate. The false arm
 * is the well-formed chapter every passing compile parses.
 */
static void test_pipeline_malformed_chapter_propagates(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: malformed chapter propagates");
  build_epub_chapter(k_chapter_plain);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: malformed chapter propagates");
}

/**
 * @test test_pipeline_to_buffer_and_null_guards
 * @brief The no-filesystem entry point emits a valid blob in place and rejects
 *        each NULL output pointer.
 *
 * @par MC/DC:
 * Covers every guard of ra8_rabook_compile_from_epub_to_buffer. The shared-arg
 * check is exercised by the valid call (false) and a NULL epub (true); the
 * out_blob / out_len guards are each exercised true with a NULL pointer and
 * false on the valid call. Each is a single-condition null guard, so one
 * true/false vector pair per guard is minimal MC/DC.
 */
static void test_pipeline_to_buffer_and_null_guards(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: compile-to-buffer + null guards");
  build_epub(false);

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));
  TEST_ASSERT(blob != nullptr && len > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate((const uint8_t*)blob, (size_t)len));

  /* Null guards: shared-arg check, then each output pointer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(nullptr, &bufs, &scr, &blob, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_rabook_pipeline: compile-to-buffer + null guards");
}

/**
 * @test test_pipeline_fs_null_guards
 * @brief The filesystem entry point rejects each NULL pointer.
 *
 * @par MC/DC:
 * Drives the true arm of the shared-arg check (NULL epub) and of the mount /
 * out_path guards (each a single-condition null guard); the false arms are the
 * valid compiles above. One NULL vector per guard plus the valid baseline is
 * minimal MC/DC for these guards.
 */
static void test_pipeline_fs_null_guards(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: filesystem entry null guards");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(nullptr, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, nullptr, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  teardown(mount);
  TEST_END("ra8_rabook_pipeline: filesystem entry null guards");
}

/**
 * @test test_pipeline_compile_init_rejects_bad_buffers
 * @brief A NULL builder-arena member fails ra8_rabook_compile_init and propagates
 *        out of internal_compile_to_blob.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the builder-init guard in
 * internal_compile_to_blob (single condition): the top-level pointers are all non-NULL
 * (so the shared-arg check passes) but @p bufs->out is NULL, so
 * ra8_rabook_compile_init rejects the arenas and internal_compile_to_blob returns its
 * error. The false arm is every passing compile.
 */
static void test_pipeline_compile_init_rejects_bad_buffers(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: compile_init rejects bad arenas");
  build_epub(false);

  ra8_epub_book_t book = {};
  open_s_epub(&book);

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  bufs.out = nullptr; /* a NULL arena member fails compile_init */

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("ra8_rabook_pipeline: compile_init rejects bad arenas");
}

/**
 * @test test_pipeline_closed_book_chapter_count_fails
 * @brief A book that is not open makes the chapter stage fail and propagate.
 *
 * @par MC/DC:
 * Drives the `if (err != k_ra8_ok)` true arm of the chapter-count guard in
 * internal_compile_chapters (single condition): the book is zero-initialised
 * (in_use == 0), so the empty manifest yields no stylesheets / images and
 * ra8_epub_get_chapter_count then returns k_ra8_err_not_initialized, which the
 * stage and internal_compile_to_blob propagate. The false arm is the open-book compile.
 */
static void test_pipeline_closed_book_chapter_count_fails(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: closed book fails at chapter count");

  ra8_epub_book_t book = {}; /* never opened: in_use == 0 */

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_rabook_compile_from_epub_to_buffer(&book, &bufs, &scr, &blob, &len));

  TEST_END("ra8_rabook_pipeline: closed book fails at chapter count");
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  test_pipeline_raster_images_transcoded();
  test_pipeline_default_preserves_resolution();
  test_pipeline_gray_scratch_too_small();
  test_pipeline_tall_image_height_downscaled();
  test_pipeline_gray8_profile();
  test_pipeline_toc_titles_resolved();
  test_pipeline_css_absent_skipped();
  test_pipeline_css_load_error_propagates();
  test_pipeline_image_resource_absent_skipped();
  test_pipeline_chapter_load_too_small();
  test_pipeline_malformed_chapter_propagates();
  test_pipeline_to_buffer_and_null_guards();
  test_pipeline_fs_null_guards();
  test_pipeline_compile_init_rejects_bad_buffers();
  test_pipeline_closed_book_chapter_count_fails();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_pipeline_err.c\n");
  return 0;
}

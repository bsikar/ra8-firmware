/**
 * @file test_ra8_rabook_pipeline.c
 * @brief End-to-end EPUB -> RABOOK1 pipeline test (ra8_rabook_pipeline, #149).
 *
 * @details
 * Exercises @ref ra8_rabook_compile_from_epub -- the only entry point that wires
 * the metadata, cover, chapter and finalize stages together -- against a tiny
 * synthetic book:
 *
 *  - A text-only `.epub` is assembled in memory with miniz (the build pattern
 *    from tests/test_ra8_epub.c) and opened with @ref ra8_epub_open.
 *  - A real FAT16 volume is formatted over a RAM block backend (the RAM-disk
 *    pattern from tests/test_ra8_epub_fs.c) so the pipeline's
 *    @ref ra8_fs_write_file lands on a genuine filesystem.
 *  - Happy path (no cover): the compile succeeds, the written `.rabook` passes
 *    @ref ra8_book_validate, chapter_count / title / author read back as built,
 *    string-pool offset 0 is the reserved "", and the absent cover leaves the
 *    cover-image index nil.
 *  - Skip path (present-but-undecodable cover): a manifest cover whose bytes are
 *    NOT a valid image makes stb_image fail to decode; matching the desktop
 *    epub_compile.py (try/except pass), the image loop skips it, so the compile
 *    still succeeds and publishes a valid coverless book (cover index nil).
 *  - Byte-identity: the on-device emit equals the desktop golden (parity case).
 *  - Skip-images byte-identity: the same fixture compiled with
 *    @ref ra8_rabook_pipeline_scratch_t::skip_images set equals the desktop
 *    `--no-images` golden (text/CSS-only, the SVG cover dropped).
 *  - Real-book byte-identity (#151): real Standard Ebooks Walden chapters
 *    (carrying the significant `</abbr> <abbr>` inter-element whitespace)
 *    compiled with skip_images equal the desktop `--no-images` golden -- the
 *    proof the device preserves inline whitespace on real content.
 *
 * The branch-coverage tests that follow drive the legs the parity / happy paths
 * never reach: the raster transcode (decode -> downscale -> gray4 encode) over
 * synthesised BMP covers, the TOC-title scan, the stylesheet / image / chapter
 * error legs, the no-filesystem buffer entry point, and every null / bad-arena
 * guard.
 *
 * @par MC/DC:
 * The pipeline's only compound decision is `if (ow == sw && oh == sh)` in
 * s_downscale_if_needed; @ref test_pipeline_raster_images_transcoded drives the
 * true arm (a 2x2 image copied in place) and the width-varying false arm (a
 * 1601x1 image downscaled), and @ref test_pipeline_gray_scratch_too_small drives
 * the height-varying leg, giving the N+1 = 3 vectors. All other branches touched
 * here (image add vs skip, TOC match, and the per-stage error propagation) are
 * single-condition guards, each covered by driving its one condition true or
 * false; the per-test `@par MC/DC:` blocks name the vectors.
 *
 * This sibling owns the compile happy path and the byte-identical parity
 * tests; the image-transform and error-path tests live in
 * test_ra8_rabook_pipeline_err.c and the synthetic-EPUB builders in
 * tests/support/rabook_pipeline_fixture.h.
 *
 *
 * [Ring 4 / EPUB_Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_pipeline.h"
#include "rabook_parity_fixture.h"
#include "rabook_realbook_fixture.h"
#include "support/rabook_pipeline_fixture.h"
#include "unity_minimal.h"

/**
 * @test test_pipeline_text_only_no_cover
 * @brief A cover-less text book compiles, validates, and round-trips its
 *        metadata; the absent cover leaves the cover index nil.
 *
 * @par MC/DC:
 * Drives the false (happy) arm of every `if (err != k_ra8_ok)` stage guard in
 * @ref ra8_rabook_compile_from_epub and the @ref k_ra8_err_not_found arm of
 * s_compile_cover (no cover -> nil index, success). No compound decision is
 * reached on this path.
 */
static void test_pipeline_text_only_no_cover(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: text-only EPUB -> valid .rabook");
  build_epub(false);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  /* Read the emitted blob back off the FAT volume. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(file, &size));
  TEST_ASSERT(size > 0U && (size_t)size <= sizeof(s_readback));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(size, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  /* The on-device reader accepts the blob. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->title_off), "Pipeline Title"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->author_off), "Pipeline Author"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, hdr->language_off), "en"));

  /* String-pool offset 0 is the reserved empty string. */
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(s_readback, 0U), ""));

  /* The single spine chapter has a 'body' root element. */
  const ra8_book_chapter_t* chaps = ra8_book_chapters(s_readback);
  const ra8_book_node_t*    nodes = ra8_book_nodes(s_readback);
  const ra8_book_node_t*    root  = &nodes[chaps[0].root_node];
  TEST_ASSERT_EQ(k_ra8_book_node_element, root->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_name(s_readback, root), "body"));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: text-only EPUB -> valid .rabook");
}

/**
 * @test test_pipeline_undecodable_cover_skipped
 * @brief A present-but-undecodable cover is skipped (not an error), so the book
 *        still compiles to a valid coverless .rabook.
 *
 * @par MC/DC:
 * Drives s_add_manifest_image's decode-failure arm: cover.bin is an image
 * manifest item (media-type image/png), so the image loop attempts a transcode;
 * stb_image rejects the
 * garbage bytes and s_transcode_image returns nil, taking the
 * `if (idx == nil) continue` skip arm -- the same try/except pass the desktop
 * epub_compile.py uses, so one bad image never fails the whole compile. The cover
 * index stays nil because no image was added to match epub->cover_path.
 */
static void test_pipeline_undecodable_cover_skipped(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: present-but-undecodable cover -> skipped");
  build_epub(true);
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_epub, .size = s_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "pipe.epub", &book));
  /* Sanity: the fixture really does declare a cover. */
  TEST_ASSERT(book.cover_path[0] != '\0');

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);

  /* The bad cover is skipped like the desktop tool, so the compile succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  /* The coverless book was published and is valid, with a nil cover index. */
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);
  TEST_ASSERT_EQ(0U, hdr->image_count);

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: present-but-undecodable cover -> skipped");
}

/* -------------------------------------------------------------------------- */
/* #151 byte-identity parity gate */
/* -------------------------------------------------------------------------- */

/**
 * @brief Assert the on-device compiler is byte-identical to the desktop tool.
 *
 * @details
 * The #151 acceptance proof: @ref ra8_rabook_compile_from_epub must emit a
 * RABOOK1 flat blob byte-for-byte identical to tools/epub_compile/epub_compile.py
 * for the text-only slice the pipeline fully supports. Opens the baked fixture
 * @c s_parity_epub from memory, compiles it onto a RAM FAT volume, reads the
 * emitted blob back, and compares it to the baked golden @c s_parity_golden (the
 * desktop output with its RBKC chunked container stripped + inflated). Regenerate
 * both arrays with `make rabook-golden-update` after any format/emitter change.
 *
 * @par MC/DC:
 * Drives the false arm of `if (scr->skip_images)` in s_compile_images (single
 * condition): skip_images is left unset, so the SVG cover IS emitted. Paired
 * with @ref test_pipeline_parity_noimg_byte_identical (which drives the true
 * arm), the two vectors (skip=false image emitted) + (skip=true image dropped)
 * give the guard its MC/DC pair.
 *
 * @pre The generated rabook_parity_fixture.h embeds a matching epub + golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: text-only fixture byte-identical to desktop");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_parity_epub, .size = (size_t)k_parity_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "rabook_parity.epub", &book));

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

  /* The on-device flat blob must equal the desktop golden, byte for byte. On a
   * mismatch, surface the first differing offset so a format/emitter drift is
   * localized to its RABOOK1 field rather than just "not equal". */
  TEST_ASSERT_EQ(k_parity_golden_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_parity_golden[i]) {
      (void)fprintf(stderr,
                    "  parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_parity_golden[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: text-only fixture byte-identical to desktop");
}

/**
 * @brief Assert the skip-images compile equals the desktop --no-images golden.
 *
 * @details
 * The #151 skip-images acceptance proof: compiling the SAME parity fixture with
 * @ref ra8_rabook_pipeline_scratch_t::skip_images set must emit a RABOOK1 blob
 * byte-for-byte identical to tools/epub_compile/epub_compile.py run with its
 * @c --no-images flag. The fixture carries an SVG cover, so the default compile
 * (cover present) and the skip-images compile (no image table, nil cover) differ
 * -- proving both sides agree on dropping images on a case that already parses
 * identically. Opens the baked @c s_parity_epub, compiles it with
 * @c scr.skip_images = true onto a RAM FAT volume, reads the blob back, and
 * compares it to the baked @c s_parity_golden_noimg.
 *
 * @par MC/DC:
 * Drives the true arm of `if (scr->skip_images)` in s_compile_images (single
 * condition); the with-images parity test above drives its false arm. Vectors
 * (skip=false image emitted) + (skip=true image dropped) cover the guard.
 *
 * @pre The generated rabook_parity_fixture.h embeds a matching --no-images golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the --no-images golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_noimg_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: skip-images fixture byte-identical to --no-images");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_parity_epub, .size = (size_t)k_parity_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "rabook_parity.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.skip_images = true; /* text/CSS-only: drop the SVG cover like desktop --no-images */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  /* The skip-images blob has no image table and a nil cover index. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));
  const ra8_book_header_t* hdr = ra8_book_header(s_readback);
  TEST_ASSERT_EQ(0U, hdr->image_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, hdr->cover_image_index);

  /* It must equal the desktop --no-images golden, byte for byte. On a mismatch,
   * surface the first differing offset to localize any drift. */
  TEST_ASSERT_EQ(k_parity_golden_noimg_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_parity_golden_noimg[i]) {
      (void)fprintf(stderr,
                    "  noimg parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_parity_golden_noimg[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_parity_golden_noimg, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: skip-images fixture byte-identical to --no-images");
}

/**
 * @brief Assert a real-book skip-images compile equals the desktop golden.
 *
 * @details
 * The #151 real-book acceptance proof: a trimmed fixture of REAL Standard Ebooks
 * Walden chapters -- @c visitors and @c conclusion, vendored verbatim under
 * tests/fixtures/rabook_realbook/ -- is compiled on-device with
 * @ref ra8_rabook_pipeline_scratch_t::skip_images set and must emit a RABOOK1
 * blob byte-for-byte identical to tools/epub_compile/epub_compile.py run with
 * @c --no-images. Both chapters carry the significant `</abbr> <abbr>`
 * inter-element whitespace (a space between two name-title abbreviations); the
 * SE markup is also indented, so the body subtree is full of inter-element
 * whitespace text runs. The desktop reference (Python @c HTMLParser) keeps every
 * such run, and -- with the on-device tinyxml2 opting into @c PEDANTIC_WHITESPACE
 * (#151) -- so does the device. Byte-identity here is the direct proof that the
 * device preserves inline whitespace on real content, so words like
 * "@c Hon. @c Mr." no longer merge into "@c Hon.Mr.".
 *
 * @par MC/DC:
 * Drives the same single-condition `if (scr->skip_images)` true arm as
 * @ref test_pipeline_parity_noimg_byte_identical, on a larger real-content
 * input; it adds no new compound decision -- its role is content fidelity, not
 * decision coverage.
 *
 * @pre The generated rabook_realbook_fixture.h embeds a matching epub + golden.
 * @pre The RAM backend descriptor @p s_backend is initialised.
 * @post The emitted blob equals the --no-images golden, byte for byte.
 * @post The RAM volume is unmounted and its backing store freed.
 * @note Not thread-safe (writes file-scope fixture buffers).
 */
static void test_pipeline_parity_realbook_byte_identical(void)
{
  TEST_BEGIN("ra8_rabook_pipeline: real Walden chapters byte-identical to --no-images");
  ra8_fs_mount_t* mount = fresh_volume();

  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_realbook_epub, .size = (size_t)k_realbook_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&media, "walden.epub", &book));

  ra8_rabook_buffers_t          bufs  = {};
  ra8_rabook_pipeline_scratch_t scr   = {};
  ra8_img_arena_t               arena = {};
  make_views(&bufs, &scr, &arena);
  scr.skip_images = true; /* text/CSS-only: real chapters + CSS, no images */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_from_epub(&book, &bufs, &scr, mount, "OUT.RAB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "OUT.RAB", k_ra8_fs_mode_read, &file));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, s_readback, (uint32_t)sizeof(s_readback), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_readback, (size_t)got));

  /* The real-book blob must equal the desktop --no-images golden, byte for byte.
   * On a mismatch, surface the first differing offset to localize any drift (a
   * whitespace-fidelity regression would show up as a string-pool divergence). */
  TEST_ASSERT_EQ(k_realbook_golden_noimg_len, got);
  for (uint32_t i = 0U; i < got; i++) {
    if (s_readback[i] != s_realbook_golden_noimg[i]) {
      (void)fprintf(stderr,
                    "  realbook parity diff at offset %u: device=0x%02X golden=0x%02X\n",
                    i,
                    s_readback[i],
                    s_realbook_golden_noimg[i]);
      break;
    }
  }
  TEST_ASSERT_EQ(0, memcmp(s_readback, s_realbook_golden_noimg, (size_t)got));

  teardown(mount);
  TEST_END("ra8_rabook_pipeline: real Walden chapters byte-identical to --no-images");
}

/* -------------------------------------------------------------------------- */
/* Branch-coverage tests (raster transcode, TOC, CSS, error legs) */
/* -------------------------------------------------------------------------- */

int32_t main(void)
{
  ra8_log_set_byte_sink(s_log_sink, nullptr);
  test_pipeline_text_only_no_cover();
  test_pipeline_undecodable_cover_skipped();
  test_pipeline_parity_byte_identical();
  test_pipeline_parity_noimg_byte_identical();
  test_pipeline_parity_realbook_byte_identical();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_pipeline.c\n");
  return 0;
}

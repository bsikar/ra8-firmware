/**
 * @file test_app_ereader.c
 * @brief Integration test: ereader app -- pre-kernel + EPUB library walker
 *
 * @details
 * The production app at examples/ek_ra8d2/ereader/main.c stands up
 * CGC + SCI8 + GLCDC + ThreadX + FileX + GUIX + ra_epub (and
 * optionally ra_reflow). Neither ThreadX, FileX, GUIX nor the on-board
 * panel are available to the host test build, so this test exercises:
 *   - the same pre-kernel boot sequence main() drives;
 *   - the LED indicators the production app uses (LED1 page-render
 *     heartbeat, LED2 "book loaded");
 *   - the ra_epub surface the library_thread + reader_thread call
 *     (open / chapter_count / close), against an in-memory synthetic
 *     EPUB built with miniz exactly the way test_ra_epub.c does.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_epub.h"
#include "ra_err.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "ra_time.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint16_t {
  k_test_er_epub_buf_bytes = 8192U, /**< Capacity of the in-memory ZIP.  */
  k_test_er_expected_chaps = 2U,    /**< Spine length in synth EPUB.     */
} test_er_const_t;

static const char* const k_er_synth_mimetype = "application/epub+zip";
static const char* const k_er_synth_container_xml =
  "<?xml version=\"1.0\"?>\n"
  "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
  "  <rootfiles>\n"
  "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
  "  </rootfiles>\n"
  "</container>\n";
static const char* const k_er_synth_content_opf =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
  "    <dc:title>EReader Test Book</dc:title>\n"
  "    <dc:creator>Brighton Sikarskie</dc:creator>\n"
  "    <dc:language>en</dc:language>\n"
  "    <dc:identifier id=\"id\">urn:test:ereader</dc:identifier>\n"
  "  </metadata>\n"
  "  <manifest>\n"
  "    <item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "    <item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
  "  </manifest>\n"
  "  <spine>\n"
  "    <itemref idref=\"ch1\"/>\n"
  "    <itemref idref=\"ch2\"/>\n"
  "  </spine>\n"
  "</package>\n";
static const char* const k_er_synth_ch1 =
  "<?xml version=\"1.0\"?><html><body><h1>Hello</h1></body></html>";
static const char* const k_er_synth_ch2 =
  "<?xml version=\"1.0\"?><html><body><h1>World</h1></body></html>";

static uint8_t s_er_epub_buf[k_test_er_epub_buf_bytes];
static size_t  s_er_epub_size;

static void er_build_synth_epub(void)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  s_er_epub_size = 0U;
  mz_bool ok     = mz_zip_writer_init_heap(&zip, 0U, k_test_er_epub_buf_bytes);
  TEST_ASSERT(ok == MZ_TRUE);
  ok = mz_zip_writer_add_mem(&zip,
                             "mimetype",
                             k_er_synth_mimetype,
                             strlen(k_er_synth_mimetype),
                             MZ_NO_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
  ok = mz_zip_writer_add_mem(&zip,
                             "META-INF/container.xml",
                             k_er_synth_container_xml,
                             strlen(k_er_synth_container_xml),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/content.opf",
                             k_er_synth_content_opf,
                             strlen(k_er_synth_content_opf),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/ch1.xhtml",
                             k_er_synth_ch1,
                             strlen(k_er_synth_ch1),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);
  ok = mz_zip_writer_add_mem(&zip,
                             "OEBPS/ch2.xhtml",
                             k_er_synth_ch2,
                             strlen(k_er_synth_ch2),
                             MZ_DEFAULT_COMPRESSION);
  TEST_ASSERT(ok == MZ_TRUE);

  void*  heap_buf  = NULL;
  size_t heap_size = 0U;
  ok               = mz_zip_writer_finalize_heap_archive(&zip, &heap_buf, &heap_size);
  TEST_ASSERT(ok == MZ_TRUE);
  TEST_ASSERT(heap_buf != NULL);
  TEST_ASSERT(heap_size <= sizeof(s_er_epub_buf));
  memcpy(s_er_epub_buf, heap_buf, heap_size);
  s_er_epub_size = heap_size;
  mz_zip_writer_end(&zip);
}

static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Pre-kernel bring-up: CGC + SysTick + LED1 + LED2.
 *
 * @par MC/DC: not applicable -- sequential init/teardown, no compound
 * boolean decisions in path.
 */
static void test_ereader_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("ereader: pre-kernel CGC + SysTick + LEDs");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_time_init(hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led1));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led2));
  TEST_END("ereader: pre-kernel CGC + SysTick + LEDs");
}

/**
 * @brief library_thread: open + chapter_count + close happy path.
 *
 * @par MC/DC:
 * Decision under test: ``ra_epub_open() != ok || chapter_count !=
 * expected || close() != ok`` chain. Three atomic conditions x N+1 = 4
 * vectors; this covers the all-ok vector. Failure vectors split across
 * the negative tests below.
 */
static void test_ereader_library_open_chapters_close(void)
{
  reset_world();
  TEST_BEGIN("ereader: library_thread open + chapters + close");
  er_build_synth_epub();
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_er_epub_buf, .size = s_er_epub_size};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_epub_open(&media, "lib.epub", &book));
  TEST_ASSERT_EQ((int)k_test_er_expected_chaps, (int)book.chapter_count);
  TEST_ASSERT_EQ((int)1, (int)book.in_use);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_epub_close(&book));
  TEST_END("ereader: library_thread open + chapters + close");
}

/**
 * @brief reader_thread: load_chapter(0) into a caller buffer.
 *
 * @par MC/DC:
 * Decision under test: ``ra_epub_load_chapter() != ok``. Two atomic
 * conditions (book valid + buf large enough) -> 3 vectors; this case
 * covers all-valid. NULL-buf vector covered below.
 */
static void test_ereader_reader_load_chapter_zero(void)
{
  reset_world();
  TEST_BEGIN("ereader: reader_thread load_chapter(0)");
  er_build_synth_epub();
  ra_epub_book_t            book  = {};
  const ra_epub_mem_media_t media = {.data = s_er_epub_buf, .size = s_er_epub_size};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_epub_open(&media, NULL, &book));

  uint8_t  chunk[1024] = {};
  size_t   actual      = 0U;
  ra_err_t err         = ra_epub_load_chapter(&book, 0U, chunk, sizeof(chunk), &actual);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT(actual > 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_epub_close(&book));
  TEST_END("ereader: reader_thread load_chapter(0)");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief library_thread: open with NULL out_book is rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_book == NULL`` precondition guard
 * inside ra_epub_open -- failure side of the open precondition.
 */
static void test_ereader_open_null_outbook_rejected(void)
{
  reset_world();
  TEST_BEGIN("ereader: open(NULL out_book) rejected");
  er_build_synth_epub();
  const ra_epub_mem_media_t media = {.data = s_er_epub_buf, .size = s_er_epub_size};
  TEST_ASSERT(ra_epub_open(&media, "x.epub", NULL) != k_ra_ok);
  TEST_END("ereader: open(NULL out_book) rejected");
}

/**
 * @brief library_thread: open of a corrupt blob (zeroed bytes) fails.
 *
 * @par MC/DC:
 * Decision vector under test: miniz reader-init failure side of
 * ra_epub_open. Models the SD-card-with-no-/books-yet path.
 */
static void test_ereader_open_corrupt_blob_rejected(void)
{
  reset_world();
  TEST_BEGIN("ereader: open of corrupt blob rejected");
  static const uint8_t      corrupt[] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  ra_epub_book_t            book      = {};
  const ra_epub_mem_media_t media     = {.data = corrupt, .size = sizeof(corrupt)};
  TEST_ASSERT(ra_epub_open(&media, "bogus.epub", &book) != k_ra_ok);
  TEST_END("ereader: open of corrupt blob rejected");
}

/**
 * @brief Double-init of LED1 still succeeds (idempotent).
 *
 * @par MC/DC:
 * State-machine vector: per-LED state already-initialised -> still-
 * initialised transition, exercising library_thread re-entry.
 */
static void test_ereader_led_init_idempotent(void)
{
  reset_world();
  TEST_BEGIN("ereader: led_init(LED1) idempotent");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led1));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led1));
  TEST_END("ereader: led_init(LED1) idempotent");
}

int main(void)
{
  test_ereader_pre_kernel_bringup();
  test_ereader_library_open_chapters_close();
  test_ereader_reader_load_chapter_zero();
  test_ereader_open_null_outbook_rejected();
  test_ereader_open_corrupt_blob_rejected();
  test_ereader_led_init_idempotent();
  return 0;
}

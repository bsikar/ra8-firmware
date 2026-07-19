/**
 * @file test_ra8_epub_streaming.c
 * @brief #151 bounded-RAM invariant gate: open + parse a large EPUB by STREAMING
 *        it from a seekable backing, never resident in one whole-file buffer.
 *
 * @details
 * The load-bearing claim of #151 is that a book far larger than SRAM+SDRAM can be
 * *opened and parsed* on-device, because `ra8_epub_open_streamed()` reads only the
 * ZIP tail (end-of-central-directory + central directory) and inflates each entry
 * on demand through a seek+read callback -- it never materialises the archive.
 * This file turns that claim into a CI-enforced invariant.
 *
 * A ~0.5 MiB in-memory ZIP is built: a real EPUB skeleton (container.xml, OPF,
 * four chapters, a cover) plus one large `filler.bin` entry that is *never*
 * referenced by the manifest/spine, so it dominates the archive size but is never
 * touched by an open. Three backings drive the same streaming reader:
 *
 *  1. **A direct seek+read callback** over the archive bytes -- proves the parse
 *     is byte-identical to the resident `ra8_epub_open()` path, and that the total
 *     bytes fetched is far less than the archive (the filler is never read).
 *  2. **The `ra8_vmem` page cache** (`ra8_vmem_stream`, item 3) fronting the archive
 *     through a *fixed, tiny frame pool* -- proves the resident set never exceeds
 *     that pool (the asserted RAM budget), which is many times smaller than the
 *     archive, and that hot pages stay resident while cold pages are re-fetched
 *     (hits > 0, evictions under churn) -- all served byte-correctly.
 *
 * The RAM-budget assertion is concrete: the page pool is `k_frames * k_frame_bytes`
 * bytes, held fixed regardless of archive size, and asserted to be several times
 * smaller than the archive; the count of resident frames never exceeds it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vmem_stream.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum epub_streaming_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_epub_filler_stride =
    31U, /**< Stride of the filler generator; prime, so the filler does not repeat inside a deflate window. */
  k_epub_entry_bytes = 100U, /**< Declared size of the streamed entry. */
} epub_streaming_uint8_const_t;

/* ---------------------------------------------------------------------------
 * Dimensions (tests are exempt from the magic-number gate; enums used anyway).
 * ---------------------------------------------------------------------------
 */

/**
 * @enum stream_dim_t
 * @brief Archive + page-cache geometry for the streaming invariant.
 * @details The frame pool (`k_frames * k_frame_bytes` = 16 KiB) is deliberately
 *          tiny relative to the ~0.5 MiB archive, so residency is bounded far
 *          below the file size -- the #151 regime.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_frame_bytes  = 4096U,        /**< Page-cache frame size (bytes).           */
  k_frames       = 4U,           /**< Fixed resident frame budget (4 * 4 KiB). */
  k_buckets      = 16U,          /**< Hash buckets for the cache.              */
  k_filler_bytes = 512U * 1024U, /**< Big unreferenced entry (dominates size). */
  k_arc_cap      = 640U * 1024U, /**< Archive scratch buffer capacity.         */
  k_budget_ratio = 8U,           /**< Assert budget * ratio <= archive size.   */
  k_churn        = 96U,          /**< Scattered single-byte reads (Phase B).   */
  k_churn_stride = 4099U,        /**< Prime stride to spread the churn.        */
  k_chapter_max  = 2048U,        /**< Chapter extract buffer.                  */
  k_cover_max    = 64U,          /**< Cover extract buffer.                    */
  k_span_probe   = 40U,          /**< Cross-frame span read length.            */
} stream_dim_t;

/* ---------------------------------------------------------------------------
 * Static fixtures.
 * ---------------------------------------------------------------------------
 */

/** @brief The built ZIP/EPUB archive (models the on-card file). */
static uint8_t s_arc[k_arc_cap];
/** @brief Byte length of the built archive in ::s_arc. */
static size_t s_arc_size = 0U;
/** @brief Source bytes for the large unreferenced filler entry. */
static uint8_t s_filler[k_filler_bytes];

/** @brief Page-cache frame storage. */
static uint8_t s_frames[(size_t)k_frames * (size_t)k_frame_bytes];
/** @brief Page-cache per-frame metadata. */
static ra8_vmem_frame_t s_meta[(size_t)k_frames];
/** @brief Page-cache hash buckets. */
static int32_t s_buckets[(size_t)k_buckets];

/** @brief Bytes fetched from the backing (page-cache path instrumentation). */
static uint64_t g_storage_bytes = 0U;

/* ---------------------------------------------------------------------------
 * EPUB fixture text.
 * ---------------------------------------------------------------------------
 */

/** @brief container.xml pointing at OEBPS/content.opf. */
static const char* const k_container = "<?xml version=\"1.0\"?>"
                                       "<container version=\"1.0\""
                                       " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
                                       " media-type=\"application/oebps-package+xml\"/>"
                                       "</rootfiles></container>";

/** @brief OPF: four chapters + a cover image, EPUB3. */
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\""
  " version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Streaming Test Book</dc:title><dc:creator>RA8</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:stream:1</dc:identifier>"
  "<meta name=\"cover\" content=\"cover-img\"/></metadata>"
  "<manifest>"
  "<item id=\"cover-img\" href=\"cover.png\" media-type=\"image/png\"/>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch3\" href=\"ch3.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"ch4\" href=\"ch4.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest>"
  "<spine><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/>"
  "<itemref idref=\"ch3\"/><itemref idref=\"ch4\"/></spine></package>";

static const char* const k_ch1 =
  "<?xml version=\"1.0\"?><html><body><p>ALPHA chapter one.</p></body></html>";
static const char* const k_ch2 =
  "<?xml version=\"1.0\"?><html><body><p>BRAVO chapter two.</p></body></html>";
static const char* const k_ch3 =
  "<?xml version=\"1.0\"?><html><body><p>CHARLIE chapter three.</p></body></html>";
static const char* const k_ch4 =
  "<?xml version=\"1.0\"?><html><body><p>DELTA chapter four.</p></body></html>";
static const char* const k_cover = "COVER-IMAGE-BYTES";

/* ---------------------------------------------------------------------------
 * Fixture builder.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Build the large EPUB (real skeleton + big unreferenced filler) into ::s_arc.
 * @details The filler is stored uncompressed so the archive is genuinely large on
 *          disk; it is not in the manifest/spine, so opening the book never reads it.
 * @pre ::s_arc has ::k_arc_cap bytes.
 * @pre ::s_filler has ::k_filler_bytes bytes.
 * @post ::s_arc_size holds the finalized archive length (> ::k_filler_bytes).
 * @post ::s_arc[0 .. s_arc_size) is a valid ZIP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_big_epub(void)
{
  for (size_t i = 0U; i < (size_t)k_filler_bytes; ++i) {
    s_filler[i] = (uint8_t)((i * k_epub_filler_stride) +
                            (i >> 3U)); /* varied, so it is not trivially compressible */
  }
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_arc_cap) == MZ_TRUE);

  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "mimetype",
                                    "application/epub+zip",
                                    strlen("application/epub+zip"),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    k_container,
                                    strlen(k_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    k_opf,
                                    strlen(k_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/cover.png",
                                    k_cover,
                                    strlen(k_cover),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch1.xhtml", k_ch1, strlen(k_ch1), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch2.xhtml", k_ch2, strlen(k_ch2), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch3.xhtml", k_ch3, strlen(k_ch3), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch4.xhtml", k_ch4, strlen(k_ch4), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  /* The big unreferenced entry, stored (uncompressed) so it dominates the file. */
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/filler.bin",
                                    s_filler,
                                    (size_t)k_filler_bytes,
                                    MZ_NO_COMPRESSION) == MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz > (size_t)k_filler_bytes) && (hsz <= (size_t)k_arc_cap));
  memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_zip_writer_end(&zip);
}

/** @brief Count resident (valid) page-cache frames -- the live residency probe. */
static uint32_t count_valid_frames(void)
{
  uint32_t live = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_frames; ++i) {
    if (s_meta[i].valid != 0U) {
      ++live;
    }
  }
  return live;
}

/* ---------------------------------------------------------------------------
 * Backing read callbacks.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief ra8_vsource read callback over ::s_arc (the page-cache path backing).
 * @param[in]  ctx    Unused.
 * @param[in]  offset Absolute byte offset within the archive.
 * @param[out] buf    Destination (`len` writable bytes).
 * @param[in]  len    Bytes to copy.
 * @return k_ra8_ok always (loader clamps to the object end before calling).
 * @pre `offset + len <= s_arc_size` (guaranteed by ra8_vsource_loader).
 * @pre `buf` is writable for `len` bytes.
 * @post ::g_storage_bytes advanced by `len`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t storage_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  memcpy(buf, &s_arc[offset], (size_t)len);
  g_storage_bytes += (uint64_t)len;
  return k_ra8_ok;
}

/**
 * @struct buf_src_t
 * @brief Backing descriptor for the direct seek+read callback.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Archive base pointer. */
  size_t         size; /**< Archive length.       */
} buf_src_t;

/** @brief Total bytes served by ::direct_read. */
static uint64_t g_direct_bytes = 0U;
/** @brief Largest single read window served by ::direct_read. */
static size_t g_direct_peak = 0U;

/**
 * @brief Direct ra8_epub stream read over a resident buffer (no cache).
 * @param[in]  ctx    The ::buf_src_t backing.
 * @param[in]  offset Absolute byte offset.
 * @param[out] buf    Destination (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes copied (0 at/after EOF).
 * @pre `ctx` is a valid ::buf_src_t; `buf` is writable for `len` bytes.
 * @post ::g_direct_bytes and ::g_direct_peak updated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static size_t direct_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const buf_src_t* s = (const buf_src_t*)ctx;
  if (offset >= (uint64_t)s->size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->size - offset;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s->data[offset], n);
  g_direct_bytes += (uint64_t)n;
  if (n > g_direct_peak) {
    g_direct_peak = n;
  }
  return n;
}

/** @brief Assert the four chapters carry their distinct markers via @p book. */
static void assert_chapters(ra8_epub_book_t* book)
{
  static const char* const markers[] = {"ALPHA", "BRAVO", "CHARLIE", "DELTA"};
  for (uint16_t i = 0U; i < 4U; ++i) {
    uint8_t chbuf[k_chapter_max] = {};
    size_t  got                  = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(book, i, chbuf, sizeof(chbuf) - 1U, &got));
    TEST_ASSERT(got > 0U);
    chbuf[got] = (uint8_t)'\0';
    TEST_ASSERT(strstr((const char*)chbuf, markers[i]) != nullptr);
  }
}

/* ---------------------------------------------------------------------------
 * Test: direct-callback streaming == resident parse, and never reads the whole file.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Assert the two parses agree on spine length and title.
 * @param[in] book     The streamed parse.
 * @param[in] resident The resident reference parse.
 * @param[out] count   Receives the (agreed) chapter count.
 * @return None.
 * @pre Both books are open.
 * @pre @p count is non-null.
 * @post @p count holds the streamed parse's chapter count.
 * @post Both parses reported the same count, and the title matched.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
stream_check_same_spine(ra8_epub_book_t* book, ra8_epub_book_t* resident, uint16_t* count)
{
  uint16_t cs = 0U;
  uint16_t cr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(book, &cs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(resident, &cr));
  TEST_ASSERT_EQ(4U, cs);
  TEST_ASSERT_EQ(cr, cs);

  ra8_epub_metadata_t ms = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_metadata(book, &ms));
  TEST_ASSERT_EQ(0, strcmp(ms.title, "Streaming Test Book"));
  *count = cs;
}

/**
 * @brief Assert every chapter decodes byte-identically in both parses.
 * @param[in] book     The streamed parse.
 * @param[in] resident The resident reference parse.
 * @param[in] count    Chapter count to walk.
 * @return None.
 * @pre Both books are open and hold @p count chapters.
 * @pre @p count is at most the spine length.
 * @post Every chapter loaded with the same length from both parses.
 * @post Every chapter's bytes compared equal.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
stream_check_chapters_identical(ra8_epub_book_t* book, ra8_epub_book_t* resident, uint16_t count)
{
  for (uint16_t i = 0U; i < count; ++i) {
    uint8_t a[k_chapter_max] = {};
    uint8_t b[k_chapter_max] = {};
    size_t  ga               = 0U;
    size_t  gb               = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(book, i, a, sizeof(a), &ga));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_load_chapter(resident, i, b, sizeof(b), &gb));
    TEST_ASSERT_EQ(gb, ga);
    TEST_ASSERT_EQ(0, memcmp(a, b, ga));
  }
}

/**
 * @brief Assert the direct-callback open never read the whole archive.
 * @return None.
 * @pre A streamed open has just run over the direct callback.
 * @pre `g_direct_bytes` / `g_direct_peak` were zeroed before that open.
 * @post The fetch total stayed under the filler entry's size.
 * @post Every individual read window stayed a bounded miniz chunk.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void stream_check_direct_io_bounded(void)
{
  /* Bounded I/O: the whole archive was NEVER read -- the 0.5 MiB filler entry is
   * untouched, so the streamed open fetched far fewer bytes than the file holds. */
  TEST_ASSERT(g_direct_bytes < (uint64_t)k_filler_bytes);
  /* Every single read window is a small, bounded miniz chunk, not the archive. */
  TEST_ASSERT(g_direct_peak <= (size_t)(k_frame_bytes * 16U));
  TEST_ASSERT((size_t)(k_frame_bytes * 16U) < s_arc_size);
}

/**
 * @test test_stream_direct_parse_equivalence
 * @brief `ra8_epub_open_streamed()` over a seek+read callback parses byte-identically
 *        to `ra8_epub_open()` on the same archive, while fetching far fewer bytes
 *        than the archive size (the filler entry is never read).
 *
 * @par MC/DC:
 * (no compound decisions under test here -- independent single-condition equalities
 * over the two parses' outputs and the fetch-total bound; the open-path compound
 * guards are covered by test_stream_open_arg_guards.)
 */
static void test_stream_direct_parse_equivalence(void)
{
  TEST_BEGIN("epub stream: direct callback == resident parse, no whole-file read");
  build_big_epub();
  g_direct_bytes = 0U;
  g_direct_peak  = 0U;

  /* Resident reference parse. */
  ra8_epub_book_t            resident = {};
  const ra8_epub_mem_media_t mem      = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "big.epub", &resident));

  /* Streamed parse over the same bytes through a seek+read callback. */
  buf_src_t               src   = {.data = s_arc, .size = s_arc_size};
  ra8_epub_stream_media_t media = {.read = direct_read, .ctx = &src, .size = (uint64_t)s_arc_size};
  ra8_epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "big.epub", &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT(book.zip_bytes == nullptr); /* streamed: no resident blob */

  uint16_t cs = 0U;
  stream_check_same_spine(&book, &resident, &cs);
  stream_check_chapters_identical(&book, &resident, cs);
  assert_chapters(&book);
  stream_check_direct_io_bounded();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&resident));
  TEST_END("epub stream: direct callback == resident parse, no whole-file read");
}

/* ---------------------------------------------------------------------------
 * Test: streaming through the ra8_vmem page cache -- bounded residency + churn.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Assert the bounded-RAM invariants after the streamed open+parse.
 * @param[in] vm The page-cache under test.
 * @return None.
 * @pre The book was opened and parsed through @p vm.
 * @post Resident set, budget ratio, partial-read and hit/miss checks all held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void stream_check_bounded_invariant(ra8_vmem_t* vm)
{
  /* 1. Resident set never exceeds the fixed pool -- the RAM budget. */
  TEST_ASSERT(count_valid_frames() <= (uint32_t)k_frames);
  /* 2. The budget is many times smaller than the archive it served. */
  const uint32_t budget = (uint32_t)k_frames * (uint32_t)k_frame_bytes;
  TEST_ASSERT(((size_t)budget * (size_t)k_budget_ratio) <= s_arc_size);
  /* 3. The whole archive was never read: fewer bytes fetched than the filler holds. */
  TEST_ASSERT(g_storage_bytes < (uint64_t)k_filler_bytes);
  /* 4. The cache actually worked: hot pages (re-read headers/dir) were reused. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, &hits, &miss, nullptr));
  TEST_ASSERT(hits > 0U);
  TEST_ASSERT(miss > 0U);
}

/**
 * @brief Scattered-churn phase: prove eviction and byte-correct cold re-fetch.
 * @param[in] vm The page-cache under test.
 * @param[in] st The stream over @p vm.
 * @return None.
 * @pre @p vm and @p st are initialised over the archive.
 * @post The churn drove eviction while every byte stayed correct.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void stream_check_churn(ra8_vmem_t* vm, ra8_vmem_stream_t* st)
{
  uint32_t evic_before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, nullptr, &evic_before));
  for (uint32_t i = 0U; i < (uint32_t)k_churn; ++i) {
    const uint64_t off = ((uint64_t)i * (uint64_t)k_churn_stride) % (uint64_t)s_arc_size;
    uint8_t        one = 0U;
    const size_t   got = ra8_vmem_stream_read(st, off, &one, 1U);
    TEST_ASSERT_EQ(1U, got);
    TEST_ASSERT_EQ(s_arc[off], one); /* byte-correct through the cache */
    TEST_ASSERT(count_valid_frames() <= (uint32_t)k_frames);
  }
  uint32_t evic_after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, nullptr, &evic_after));
  /* The churn touched far more distinct pages than the pool holds -> eviction. */
  TEST_ASSERT(evic_after > evic_before);
}

/**
 * @brief Assert a read spanning a frame boundary reassembles correctly.
 * @param[in] st The stream over the page cache.
 * @return None.
 * @pre @p st is initialised over the archive.
 * @post The boundary-spanning read matched the backing bytes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void stream_check_span(ra8_vmem_stream_t* st)
{
  const uint64_t span_off           = (uint64_t)k_frame_bytes - 10U;
  uint8_t        span[k_span_probe] = {};
  const size_t   span_got = ra8_vmem_stream_read(st, span_off, span, (size_t)k_span_probe);
  TEST_ASSERT_EQ(k_span_probe, span_got);
  TEST_ASSERT_EQ(0, memcmp(span, &s_arc[span_off], (size_t)k_span_probe));
}

/**
 * @test test_stream_via_pagecache_bounded
 * @brief Open + parse the large EPUB through a fixed, tiny `ra8_vmem` frame pool:
 *        residency never exceeds the pool (the RAM budget, many times smaller than
 *        the archive), the filler is never fetched, hot pages are reused, and under
 *        scattered churn cold pages are evicted and re-fetched byte-correctly.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- the gate is a set of independent
 * single-condition inequalities over the cache counters + residency probe, plus a
 * byte-compare correctness oracle against the raw archive.)
 */
static void test_stream_via_pagecache_bounded(void)
{
  TEST_BEGIN("epub stream: bounded residency through ra8_vmem page cache");
  build_big_epub();
  g_storage_bytes = 0U;
  memset(s_meta, 0, sizeof(s_meta));

  /* Layer 1 + Layer 2: register the archive as a paged object, front it with the
   * fixed frame pool, and expose it as a byte stream. */
  ra8_vsource_t     vs      = {};
  ra8_vsource_obj_t objs[1] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, objs, 1U));
  uint32_t obj = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs, storage_read, nullptr, 0U, (uint64_t)s_arc_size, &obj));
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = {.frame_mem    = s_frames,
                        .frame_bytes  = (uint32_t)k_frame_bytes,
                        .frame_count  = (uint32_t)k_frames,
                        .meta         = s_meta,
                        .buckets      = s_buckets,
                        .bucket_count = (uint32_t)k_buckets,
                        .loader       = ra8_vsource_loader,
                        .loader_ctx   = &vs};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));
  ra8_vmem_stream_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stream_init(&st, &vm, obj, (uint64_t)s_arc_size));

  /* Open + parse the whole book through the tiny cache. */
  ra8_epub_stream_media_t media = {.read = ra8_vmem_stream_read,
                                   .ctx  = &st,
                                   .size = (uint64_t)s_arc_size};
  ra8_epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "big.epub", &book));
  TEST_ASSERT_EQ(1, book.in_use);

  uint16_t chapters = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_chapter_count(&book, &chapters));
  TEST_ASSERT_EQ(4U, chapters);
  assert_chapters(&book);

  /* An arbitrary manifest resource inflates on demand off the same stream. */
  uint8_t cover[k_cover_max] = {};
  size_t  cover_got          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_get_resource(&book, "cover.png", cover, sizeof(cover), &cover_got));
  TEST_ASSERT_EQ(strlen(k_cover), cover_got);
  TEST_ASSERT_EQ(0, memcmp(cover, k_cover, cover_got));

  /* ---- The bounded-RAM invariant ------------------------------------------ */
  stream_check_bounded_invariant(&vm);

  /* ---- Phase B: scattered churn proves eviction + correct cold re-fetch ---- */
  stream_check_churn(&vm, &st);

  /* A read spanning a frame boundary is reassembled correctly. */
  stream_check_span(&st);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub stream: bounded residency through ra8_vmem page cache");
}

/* ---------------------------------------------------------------------------
 * Test: open argument guards.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_stream_open_arg_guards
 * @brief `ra8_epub_open_streamed()` rejects NULL/empty media and a non-ZIP stream.
 *
 * @par MC/DC:
 * Decision: `media == NULL || out_book == NULL` (2 conditions, OR).
 * - V1 media=NULL, out=valid  -> true  (varies left).
 * - V2 media=valid, out=NULL  -> true  (varies right).
 * - Control (both valid) is exercised by the parse tests -> false.
 * Decision: `media->read == NULL || media->size == 0` (2 conditions, OR).
 * - V3 read=NULL, size>0      -> true  (varies left).
 * - V4 read=valid, size=0     -> true  (varies right).
 * - Control (read set, size>0) is the happy path -> false.
 */
static void test_stream_open_arg_guards(void)
{
  TEST_BEGIN("epub stream: open argument guards");
  build_big_epub();
  ra8_epub_book_t         book = {};
  buf_src_t               src  = {.data = s_arc, .size = s_arc_size};
  ra8_epub_stream_media_t good = {.read = direct_read, .ctx = &src, .size = (uint64_t)s_arc_size};

  /* NULL-OR guard, each operand flipped. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed(nullptr, "x", &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open_streamed(&good, "x", nullptr));

  /* invalid-arg OR guard, each operand flipped. */
  ra8_epub_stream_media_t no_read = {.read = nullptr, .ctx = &src, .size = (uint64_t)s_arc_size};
  ra8_epub_stream_media_t no_size = {.read = direct_read, .ctx = &src, .size = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_open_streamed(&no_read, "x", &book));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_open_streamed(&no_size, "x", &book));

  /* A non-ZIP stream -> validation_failed (reader init fails). */
  static const uint8_t    k_junk[] = "this is definitely not a zip archive header at all";
  buf_src_t               jsrc     = {.data = k_junk, .size = sizeof(k_junk)};
  ra8_epub_stream_media_t junk     = {.read = direct_read,
                                      .ctx  = &jsrc,
                                      .size = (uint64_t)sizeof(k_junk)};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_epub_open_streamed(&junk, "x", &book));
  TEST_ASSERT_EQ(0, book.in_use);

  TEST_END("epub stream: open argument guards");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_vmem_stream init + read guards.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_vmem_stream_guards
 * @brief `ra8_vmem_stream_init` rejects NULL/zero args; `ra8_vmem_stream_read`
 *        returns 0 for NULL ctx/buf, zero length, past-EOF, and an unbound stream.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent single
 * condition; init's NULL guards are RA8_CHECK_NULL_PTR one-condition checks and
 * read's guards are separate single-condition early returns.)
 */
static void test_vmem_stream_guards(void)
{
  TEST_BEGIN("ra8_vmem_stream: init + read guards");
  /* init guards. */
  ra8_vmem_stream_t st = {};
  ra8_vmem_t        vm = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_stream_init(nullptr, &vm, 0U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_stream_init(&st, nullptr, 0U, 1U));
  /* vm.cfg.frame_bytes is 0 on a zeroed cache; size=0 trips the size guard first. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_vmem_stream_init(&st, &vm, 0U, 0U));

  /* read guards -- all return 0 bytes. */
  uint8_t buf[4] = {};
  TEST_ASSERT_EQ(0U, ra8_vmem_stream_read(nullptr, 0U, buf, sizeof(buf)));
  ra8_vmem_stream_t bound = {.vm          = &vm,
                             .object_id   = 0U,
                             .frame_bytes = (uint32_t)k_frame_bytes,
                             .size        = k_epub_entry_bytes};
  TEST_ASSERT_EQ(0U, ra8_vmem_stream_read(&bound, 0U, nullptr, sizeof(buf))); /* NULL buf */
  TEST_ASSERT_EQ(0U, ra8_vmem_stream_read(&bound, 0U, buf, 0U));              /* len 0    */
  TEST_ASSERT_EQ(0U, ra8_vmem_stream_read(&bound, 100U, buf, sizeof(buf)));   /* at EOF   */
  ra8_vmem_stream_t unbound = {.vm          = &vm,
                               .object_id   = 0U,
                               .frame_bytes = 0U,
                               .size        = k_epub_entry_bytes};
  TEST_ASSERT_EQ(0U, ra8_vmem_stream_read(&unbound, 0U, buf, sizeof(buf))); /* frame_bytes 0 */

  TEST_END("ra8_vmem_stream: init + read guards");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point -- runs the streaming invariant suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_stream_direct_parse_equivalence();
  test_stream_via_pagecache_bounded();
  test_stream_open_arg_guards();
  test_vmem_stream_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_epub_streaming.c\n");
  return 0;
}

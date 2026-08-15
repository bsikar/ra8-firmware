/**
 * @file test_ra8_epub_entry.c
 * @brief #231 bounded-RAM ZIP-entry extraction: forward streaming cursor +
 *        positioned (windowed) read, proven byte-identical to a whole-inflate.
 *
 * @details
 * The load-bearing claim of #231 is that a large in-content image entry (a manga
 * page that inflates to tens of MB) can be paged off an EPUB without ever holding
 * the whole entry resident. This file turns that into a CI-enforced invariant for
 * the two extraction primitives on `ra8_epub`:
 *
 *  1. `ra8_epub_entry_open/read/close` -- a forward inflate cursor. A ~256 KiB
 *     DEFLATE entry is streamed through a *fixed 4 KiB* chunk buffer and every
 *     chunk is byte-compared against the known source: the streamed bytes match a
 *     whole-inflate exactly, and the caller's resident buffer never grows past the
 *     chunk size regardless of the entry's uncompressed size. The same entry is
 *     streamed from a *streamed* book (no resident archive) to prove the backing
 *     read window stays bounded too.
 *  2. `ra8_epub_entry_pread` -- windowed random access into a *stored*
 *     (uncompressed) entry: several windows are read and byte-checked against the
 *     source, a tail window returns short, a past-EOF window returns zero, and a
 *     DEFLATE entry is rejected.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_epub.h"
#include "ra8_epub_entry.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum epub_entry_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus the payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_epub_reread_poison =
    123U, /**< Poison length before a second read, so a silent no-op re-read is detectable. */
  k_epub_pattern_modulus =
    251U, /**< Second generator's modulus: prime, just under 256, so data varies yet compresses. */
  k_epub_pattern_stride =
    37U, /**< Payload stride `i * 37 + 11`; prime, so it does not repeat in a deflate window. */
  k_epub_pattern_shift =
    5U, /**< Shift XORed into that generator, adding a slowly-varying high component. */
  /** Its bias, so index 0 is not byte 0. */
  k_epub_pattern_bias = 11U,
} epub_entry_fixture_t;

/* ---------------------------------------------------------------------------
 * Dimensions (tests are exempt from the magic-number gate; enums used anyway).
 * ---------------------------------------------------------------------------
 */

/**
 * @enum entry_dim_t
 * @brief Archive + entry geometry for the extraction invariant.
 * @details `k_big_bytes` (256 KiB) is the DEFLATE entry streamed in `k_chunk`
 *          (4 KiB) chunks -- the chunk buffer is the whole resident footprint, 64x
 *          smaller than the entry. `k_raw_bytes` is the stored entry pread reads.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_big_bytes  = 256U * 1024U, /**< DEFLATE streaming entry (uncompressed). */
  k_raw_bytes  = 40000U,       /**< Stored entry read by pread.             */
  k_chunk      = 4096U,        /**< Fixed streaming chunk buffer.           */
  k_arc_cap    = 512U * 1024U, /**< Archive scratch capacity.               */
  k_ref_cap    = 300U * 1024U, /**< Whole-inflate reference buffer.         */
  k_io_bound   = 64U * 1024U,  /**< miniz MZ_ZIP_MAX_IO_BUF_SIZE bound.     */
  k_win_off    = 1000U,        /**< A mid-entry pread window offset.        */
  k_win_len    = 500U,         /**< A mid-entry pread window length.        */
  k_tail_extra = 100U,         /**< Over-read past the entry tail.          */
  k_tail_back  = 10U,          /**< Tail window start = size - this.        */
} entry_dim_t;

/* ---------------------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------------------
 */

/** @brief The built ZIP/EPUB archive. */
static uint8_t s_arc[k_arc_cap];
/** @brief Built archive length. */
static size_t s_arc_size = 0U;
/** @brief Source bytes for the DEFLATE streaming entry. */
static uint8_t s_big[k_big_bytes];
/** @brief Source bytes for the stored pread entry. */
static uint8_t s_raw[k_raw_bytes];
/** @brief Whole-inflate reference buffer (parity oracle). */
static uint8_t s_ref[k_ref_cap];

/** @brief container.xml pointing at OEBPS/content.opf. */
static const char* const s_container = "<?xml version=\"1.0\"?>"
                                       "<container version=\"1.0\""
                                       " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
                                       " media-type=\"application/oebps-package+xml\"/>"
                                       "</rootfiles></container>";

/** @brief OPF: one chapter + a big image item + a raw stored item. */
static const char* const s_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\""
  " version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Entry Test</dc:title><dc:creator>RA8</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:entry:1</dc:identifier></metadata>"
  "<manifest>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"big\" href=\"big.dat\" media-type=\"image/png\"/>"
  "<item id=\"raw\" href=\"page.raw\" media-type=\"application/octet-stream\"/>"
  "</manifest>"
  "<spine><itemref idref=\"ch1\"/></spine></package>";

static const char* const s_ch1 =
  "<?xml version=\"1.0\"?><html><body><p>Entry chapter.</p></body></html>";

/**
 * @brief Fill the source arrays with deterministic, semi-compressible patterns.
 * @pre ::s_big and ::s_raw exist.
 * @pre Called before ::internal_build_archive.
 * @post ::s_big and ::s_raw hold reproducible bytes.
 * @post No archive state touched.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the fill sources fixture operation used only by this focused test executable. */
RA8_INTERNAL static void internal_fill_sources(void)
{
  for (size_t i = 0U; i < (size_t)k_big_bytes; ++i) {
    s_big[i] = (uint8_t)((i % k_epub_pattern_modulus) ^
                         (i >> k_epub_pattern_shift)); /* varied but compressible */
  }
  for (size_t i = 0U; i < (size_t)k_raw_bytes; ++i) {
    s_raw[i] = (uint8_t)((i * k_epub_pattern_stride) + k_epub_pattern_bias);
  }
}

/**
 * @brief Build the EPUB (skeleton + DEFLATE big.dat + stored page.raw) into ::s_arc.
 * @pre ::internal_fill_sources has run.
 * @pre ::s_arc has ::k_arc_cap bytes.
 * @post ::s_arc_size holds the finalized archive length.
 * @post ::s_arc[0 .. s_arc_size) is a valid ZIP.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the build archive fixture operation used only by this focused test executable. */
RA8_INTERNAL static void internal_build_archive(void)
{
  internal_fill_sources();
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
                                    s_container,
                                    strlen(s_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    s_opf,
                                    strlen(s_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch1.xhtml", s_ch1, strlen(s_ch1), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  /* Large DEFLATE entry -> exercises the streaming inflate cursor. */
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/big.dat",
                                    s_big,
                                    (size_t)k_big_bytes,
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  /* Stored (uncompressed) entry -> exercises positioned pread. */
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/page.raw", s_raw, (size_t)k_raw_bytes, MZ_NO_COMPRESSION) ==
    MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz <= (size_t)k_arc_cap));
  memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_free(heap);
  mz_zip_writer_end(&zip);
}

/**
 * @struct buf_src_t
 * @brief Backing descriptor for the streamed-book seek+read callback.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Archive base.   */
  size_t         size; /**< Archive length. */
} buf_src_t;

/** @brief Largest single read window served by ::internal_direct_read. */
static size_t s_peak = 0U;

/**
 * @brief ra8_epub streamed-media read over a resident buffer (records peak window).
 * @param[in]  ctx    The ::buf_src_t backing.
 * @param[in]  offset Absolute byte offset.
 * @param[out] buf    Destination.
 * @param[in]  len    Bytes requested.
 * @return Bytes copied (0 past EOF).
 * @pre `ctx` is a ::buf_src_t; `buf` holds `len` bytes.
 * @post ::s_peak tracks the largest window.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the direct read fixture operation used only by this focused test executable. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static size_t internal_direct_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const buf_src_t* s = (const buf_src_t*)ctx;
  if (offset >= (uint64_t)s->size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->size - offset;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s->data[offset], n);
  if (n > s_peak) {
    s_peak = n;
  }
  return n;
}

/**
 * @brief Stream `big.dat` through the cursor and byte-check every chunk vs ::s_big.
 * @param[in] book Open book (resident or streamed).
 * @return Total bytes streamed.
 * @pre @p book is open with `big.dat` present.
 * @pre The chunk buffer is a fixed ::k_chunk bytes.
 * @post Each streamed byte equalled ::s_big; a mismatch fails the test.
 * @post The reader is closed.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the stream and verify big fixture operation used only by this focused test executable. @retval value The computed fixture value for the supplied inputs. */
RA8_INTERNAL static uint64_t internal_stream_and_verify_big(ra8_epub_book_t* book)
{
  ra8_epub_entry_reader_t rd   = {};
  uint64_t                size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_open(book, "big.dat", &rd, &size));
  TEST_ASSERT_EQ(k_big_bytes, size);

  uint8_t  chunk[k_chunk] = {};
  uint64_t off            = 0U;
  uint32_t reads          = 0U;
  for (;;) {
    size_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_read(&rd, chunk, sizeof(chunk), &got));
    if (got == 0U) {
      break;
    }
    TEST_ASSERT(off + (uint64_t)got <= (uint64_t)k_big_bytes);
    TEST_ASSERT_EQ(0, memcmp(chunk, &s_big[(size_t)off], got));
    off += (uint64_t)got;
    ++reads;
    TEST_ASSERT(reads <= ((uint32_t)k_big_bytes / (uint32_t)k_chunk) + 2U); /* bounded loop */
  }
  TEST_ASSERT_EQ(k_big_bytes, off);
  /* Reading past EOF stays at zero, idempotently. */
  size_t again = k_epub_reread_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_read(&rd, chunk, sizeof(chunk), &again));
  TEST_ASSERT_EQ(0U, again);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_close(&rd));
  TEST_ASSERT(rd.iter == nullptr);
  /* Close is idempotent. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_close(&rd));
  return off;
}

/* ---------------------------------------------------------------------------
 * Test: streaming cursor == whole-inflate, in a bounded chunk buffer.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_stream_parity_bounded
 * @brief The forward cursor delivers `big.dat` byte-identically to a whole
 *        `ra8_epub_get_resource` inflate, using only a fixed 4 KiB chunk buffer --
 *        never a buffer sized to the 256 KiB entry.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the cursor's guards are independent
 * single-condition early returns, and its not-ready check reuses the
 * MC/DC-covered `ra8_epub_internal_book_not_ready` helper. The assertions are
 * independent equalities over the streamed bytes, the reported size, and the
 * whole-inflate oracle.) @details Executes the entry stream parity bounded scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_stream_parity_bounded(void)
{
  TEST_BEGIN("epub entry: streaming cursor == whole-inflate in a bounded buffer");
  internal_build_archive();

  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "entry.epub", &book));

  /* Oracle: the whole entry inflated in one shot (the path #231 replaces). */
  size_t ref_got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_resource(&book, "big.dat", s_ref, sizeof(s_ref), &ref_got));
  TEST_ASSERT_EQ(k_big_bytes, ref_got);
  TEST_ASSERT_EQ(0, memcmp(s_ref, s_big, (size_t)k_big_bytes));

  /* Streamed, chunk by chunk, into a fixed 4 KiB buffer -- provably parity. */
  const uint64_t streamed = internal_stream_and_verify_big(&book);
  TEST_ASSERT_EQ(ref_got, streamed);
  /* The resident chunk buffer is 64x smaller than the entry it paged. */
  TEST_ASSERT(((size_t)k_chunk * 64U) == (size_t)k_big_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub entry: streaming cursor == whole-inflate in a bounded buffer");
}

/* ---------------------------------------------------------------------------
 * Test: streaming from a streamed book keeps the backing window bounded.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_stream_over_streamed_book
 * @brief Streaming `big.dat` from a book opened with `ra8_epub_open_streamed()`
 *        (no resident archive) yields the same bytes while the backing read window
 *        stays bounded (<= miniz's 64 KiB IO buffer), never the whole entry.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; see internal_test_entry_stream_parity_bounded.) @details Executes the entry stream over streamed book scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_stream_over_streamed_book(void)
{
  TEST_BEGIN("epub entry: streaming cursor over a streamed book, bounded window");
  internal_build_archive();
  s_peak = 0U;

  buf_src_t               src   = {.data = s_arc, .size = s_arc_size};
  ra8_epub_stream_media_t media = {.read = internal_direct_read,
                                   .ctx  = &src,
                                   .size = (uint64_t)s_arc_size};
  ra8_epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "entry.epub", &book));
  TEST_ASSERT(book.zip_bytes == nullptr);

  const uint64_t streamed = internal_stream_and_verify_big(&book);
  TEST_ASSERT_EQ(k_big_bytes, streamed);
  /* Every backing fetch is a bounded miniz chunk, never the whole entry. */
  TEST_ASSERT(s_peak <= (size_t)k_io_bound);
  TEST_ASSERT((size_t)k_io_bound < (size_t)k_big_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub entry: streaming cursor over a streamed book, bounded window");
}

/* ---------------------------------------------------------------------------
 * Test: positioned read of a stored entry.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_pread_windows
 * @brief `ra8_epub_entry_pread` reads exact windows of the stored `page.raw`,
 *        returns short at the tail, zero past EOF, and rejects the DEFLATE entry.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; pread's guards are independent
 * single-condition checks. Assertions are independent equalities over each window
 * vs the source oracle plus the two rejection codes.) @details Executes the entry pread windows scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_pread_windows(void)
{
  TEST_BEGIN("epub entry: positioned read of a stored entry");
  internal_build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "entry.epub", &book));

  uint8_t win[k_raw_bytes] = {};
  size_t  got              = 0U;

  /* Full window. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_pread(&book, "page.raw", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ(k_raw_bytes, got);
  TEST_ASSERT_EQ(0, memcmp(win, s_raw, (size_t)k_raw_bytes));

  /* Mid window. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_entry_pread(&book, "page.raw", (uint64_t)k_win_off, win, k_win_len, &got));
  TEST_ASSERT_EQ(k_win_len, got);
  TEST_ASSERT_EQ(0, memcmp(win, &s_raw[k_win_off], (size_t)k_win_len));

  /* Tail window over-read -> short read of exactly the remaining bytes. */
  const uint64_t tail_off = (uint64_t)k_raw_bytes - (uint64_t)k_tail_back;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_entry_pread(&book, "page.raw", tail_off, win, k_tail_extra, &got));
  TEST_ASSERT_EQ(k_tail_back, got);
  TEST_ASSERT_EQ(0, memcmp(win, &s_raw[(size_t)tail_off], (size_t)k_tail_back));

  /* At/after EOF -> zero bytes, ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_entry_pread(&book, "page.raw", (uint64_t)k_raw_bytes, win, k_tail_extra, &got));
  TEST_ASSERT_EQ(0U, got);

  /* DEFLATE entry -> not supported (use the cursor). */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_epub_entry_pread(&book, "big.dat", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ(0U, got);

  /* Missing entry -> not found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_epub_entry_pread(&book, "nope.bin", 0U, win, sizeof(win), &got));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub entry: positioned read of a stored entry");
}

/* ---------------------------------------------------------------------------
 * Test: argument + state guards.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_guards
 * @brief NULL / not-open / bad-size / not-found guards on all four entry calls.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; each guard is an independent
 * single-condition early return -- the NULL checks are `RA8_CHECK_NULL_PTR`
 * one-condition macros and the not-ready guard reuses the MC/DC-covered
 * `ra8_epub_internal_book_not_ready` helper, whose two conditions are exercised
 * here by a closed book.) @details Executes the entry guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_guards(void)
{
  TEST_BEGIN("epub entry: argument + state guards");
  internal_build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "entry.epub", &book));

  ra8_epub_entry_reader_t rd     = {};
  uint64_t                sz     = 0U;
  uint8_t                 buf[8] = {};
  size_t                  got    = 0U;

  /* entry_open NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_open(nullptr, "x", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_open(&book, nullptr, &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_open(&book, "x", nullptr, &sz));
  /* entry_open on a missing entry. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epub_entry_open(&book, "nope.bin", &rd, &sz));

  /* entry_read guards on a not-open reader. */
  ra8_epub_entry_reader_t closed = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_read(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_read(&closed, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_read(&closed, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_epub_entry_read(&closed, buf, sizeof(buf), &got));
  /* entry_read with a valid open reader but zero cap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_open(&book, "big.dat", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epub_entry_read(&rd, buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_entry_close(&rd));

  /* entry_close + pread NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_entry_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_entry_pread(nullptr, "x", 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_entry_pread(&book, nullptr, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_entry_pread(&book, "x", 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_entry_pread(&book, "x", 0U, buf, sizeof(buf), nullptr));

  /* Both cursor + pread reject a closed book (not-ready guard, true arm). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_epub_entry_open(&book, "big.dat", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_epub_entry_pread(&book, "page.raw", 0U, buf, sizeof(buf), &got));

  TEST_END("epub entry: argument + state guards");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point -- runs the extraction invariant suite in order.
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
  internal_test_entry_stream_parity_bounded();
  internal_test_entry_stream_over_streamed_book();
  internal_test_entry_pread_windows();
  internal_test_entry_guards();
  return 0;
}

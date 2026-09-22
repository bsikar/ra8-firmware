/**
 * @file test_epub_entry.c
 * @brief #231 bounded-RAM ZIP-entry extraction: forward streaming cursor +
 *        positioned (windowed) read, proven byte-identical to a whole-inflate.
 *
 * @details
 * The load-bearing claim of #231 is that a large in-content image entry (a manga
 * page that inflates to tens of MB) can be paged off an EPUB without ever holding
 * the whole entry resident. This file turns that into a CI-enforced invariant for
 * the two extraction primitives on `epub`:
 *
 *  1. `epub_entry_open/read/close` -- a forward inflate cursor. A ~256 KiB
 *     DEFLATE entry is streamed through a *fixed 4 KiB* chunk buffer and every
 *     chunk is byte-compared against the known source: the streamed bytes match a
 *     whole-inflate exactly, and the caller's resident buffer never grows past the
 *     chunk size regardless of the entry's uncompressed size. The same entry is
 *     streamed from a *streamed* book (no resident archive) to prove the backing
 *     read window stays bounded too.
 *  2. `epub_entry_pread` -- windowed random access into a *stored*
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

#include "epub.h"
#include "epub_entry.h"
#include "miniz.h"
#include "ra8_attributes.h"
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

/**
 * @enum lfh_layout_t
 * @brief The ZIP local-file-header fields the corruption arms reach for.
 *
 * @details
 * Two arms below have to reach INSIDE the archive the miniz writer produced:
 * one flips the local header's signature, the other flips a byte of an
 * entry's stored payload. Both need the same little-endian layout
 * (APPNOTE.TXT 4.3.7), and naming the offsets keeps the arithmetic readable
 * instead of a run of bare integers.
 *
 * @invariant ::k_lfh_size is the fixed part of the header; the variable part
 *            is the file name plus the extra field, whose lengths live at
 *            ::k_lfh_fname_len_ofs and ::k_lfh_extra_len_ofs.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_lfh_size          = 30U,   /**< Fixed part of a local file header.         */
  k_lfh_sig_byte      = 3U,    /**< Index of the last signature byte (0x04).   */
  k_lfh_fname_len_ofs = 26U,   /**< uint16 LE file-name length.                */
  k_lfh_extra_len_ofs = 28U,   /**< uint16 LE extra-field length.              */
  k_lfh_le_lo         = 0U,    /**< Little-endian low byte of a uint16 field.  */
  k_lfh_le_hi         = 1U,    /**< Little-endian high byte of a uint16 field. */
  k_lfh_le_shift      = 8U,    /**< Shift applied to the high byte.            */
  k_corrupt_mask      = 0xFFU, /**< XOR mask that guarantees a changed byte.   */
  k_corrupt_data_ofs  = 7U,    /**< Payload byte flipped by the CRC arm.       */
} lfh_layout_t;

/* ---------------------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------------------
 */

/** @brief Archive bytes, source payloads, and documents shared across entry tests. */
static struct {
  uint8_t     archive[k_arc_cap];   /**< Generated ZIP archive bytes. */
  size_t      archive_size;         /**< Finalized archive length.    */
  uint8_t     big[k_big_bytes];     /**< Oversized member payload.    */
  uint8_t     raw[k_raw_bytes];     /**< Raw entry read destination.  */
  uint8_t     reference[k_ref_cap]; /**< Expected decompressed bytes. */
  const char* container;            /**< Rootfile locator document.   */
  const char* opf;                  /**< Package manifest and spine.  */
  const char* chapter;              /**< XHTML chapter payload.       */
} s_fixture = {
  .container = "<?xml version=\"1.0\"?>"
               "<container version=\"1.0\""
               " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
               "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
               " media-type=\"application/oebps-package+xml\"/>"
               "</rootfiles></container>",

  /** @brief OPF: one chapter + a big image item + a raw stored item. */
  .opf =
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
    "<spine><itemref idref=\"ch1\"/></spine></package>",

  .chapter = "<?xml version=\"1.0\"?><html><body><p>Entry chapter.</p></body></html>",
};

/**
 * @brief Fill the source arrays with deterministic, semi-compressible patterns.
 * @pre ::s_fixture.big and ::s_fixture.raw exist.
 * @pre Called before ::internal_build_archive.
 * @post ::s_fixture.big and ::s_fixture.raw hold reproducible bytes.
 * @post No archive state touched.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the fill sources fixture operation used only by this focused test executable. */
RA8_INTERNAL static void internal_fill_sources(void)
{
  for (size_t i = 0U; i < (size_t)k_big_bytes; ++i) {
    s_fixture.big[i] = (uint8_t)((i % k_epub_pattern_modulus) ^
                                 (i >> k_epub_pattern_shift)); /* varied but compressible */
  }
  for (size_t i = 0U; i < (size_t)k_raw_bytes; ++i) {
    s_fixture.raw[i] = (uint8_t)((i * k_epub_pattern_stride) + k_epub_pattern_bias);
  }
}

/**
 * @brief Build the EPUB (skeleton + DEFLATE big.dat + stored page.raw) into ::s_fixture.archive.
 * @pre ::internal_fill_sources has run.
 * @pre ::s_fixture.archive has ::k_arc_cap bytes.
 * @post ::s_fixture.archive_size holds the finalized archive length.
 * @post ::s_fixture.archive[0 .. s_fixture.archive_size) is a valid ZIP.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the build archive fixture operation used only by this focused test executable. */
RA8_INTERNAL static void internal_build_archive(void)
{
  internal_fill_sources();
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_arc_cap) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "mimetype",
                                    "application/epub+zip",
                                    strlen("application/epub+zip"),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    s_fixture.container,
                                    strlen(s_fixture.container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    s_fixture.opf,
                                    strlen(s_fixture.opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/ch1.xhtml",
                                    s_fixture.chapter,
                                    strlen(s_fixture.chapter),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  /* Large DEFLATE entry -> exercises the streaming inflate cursor. */
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/big.dat",
                                    s_fixture.big,
                                    (size_t)k_big_bytes,
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  /* Stored (uncompressed) entry -> exercises positioned pread. */
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/page.raw",
                                    s_fixture.raw,
                                    (size_t)k_raw_bytes,
                                    MZ_NO_COMPRESSION) == MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz <= (size_t)k_arc_cap));
  (void)memcpy(s_fixture.archive, heap, hsz);
  s_fixture.archive_size = hsz;
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
 * @brief epub streamed-media read over a resident buffer (records peak window).
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
  (void)memcpy(buf, &s->data[offset], n);
  if (n > s_peak) {
    s_peak = n;
  }
  return n;
}

/**
 * @brief Stream `big.dat` through the cursor and byte-check every chunk vs ::s_fixture.big.
 * @param[in] book Open book (resident or streamed).
 * @return Total bytes streamed.
 * @pre @p book is open with `big.dat` present.
 * @pre The chunk buffer is a fixed ::k_chunk bytes.
 * @post Each streamed byte equalled ::s_fixture.big; a mismatch fails the test.
 * @post The reader is closed.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the stream and verify big fixture operation used only by this focused test executable. @retval value The computed fixture value for the supplied inputs. */
RA8_INTERNAL static uint64_t internal_stream_and_verify_big(epub_book_t* book)
{
  epub_entry_reader_t rd   = {};
  uint64_t            size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_open(book, "big.dat", &rd, &size));
  TEST_ASSERT_EQ(k_big_bytes, size);

  uint8_t  chunk[k_chunk] = {};
  uint64_t off            = 0U;
  uint32_t reads          = 0U;
  for (;;) {
    size_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, epub_entry_read(&rd, chunk, sizeof(chunk), &got));
    if (got == 0U) {
      break;
    }
    TEST_ASSERT(off + (uint64_t)got <= (uint64_t)k_big_bytes);
    TEST_ASSERT_EQ(0, memcmp(chunk, &s_fixture.big[(size_t)off], got));
    off += (uint64_t)got;
    ++reads;
    TEST_ASSERT(reads <= ((uint32_t)k_big_bytes / (uint32_t)k_chunk) + 2U); /* bounded loop */
  }
  TEST_ASSERT_EQ(k_big_bytes, off);
  /* Reading past EOF stays at zero, idempotently. */
  size_t again = k_epub_reread_poison;
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_read(&rd, chunk, sizeof(chunk), &again));
  TEST_ASSERT_EQ(0U, again);
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_close(&rd));
  TEST_ASSERT(rd.iter == nullptr);
  /* Close is idempotent. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_close(&rd));
  return off;
}

/* ---------------------------------------------------------------------------
 * Test: streaming cursor == whole-inflate, in a bounded chunk buffer.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_stream_parity_bounded
 * @brief The forward cursor delivers `big.dat` byte-identically to a whole
 *        `epub_get_resource` inflate, using only a fixed 4 KiB chunk buffer --
 *        never a buffer sized to the 256 KiB entry.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the cursor's guards are independent
 * single-condition early returns, and its not-ready check reuses the
 * MC/DC-covered `epub_internal_book_not_ready` helper. The assertions are
 * independent equalities over the streamed bytes, the reported size, and the
 * whole-inflate oracle.) @details Executes the entry stream parity bounded scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_stream_parity_bounded(void)
{
  TEST_BEGIN("epub entry: streaming cursor == whole-inflate in a bounded buffer");
  internal_build_archive();

  epub_book_t            book = {};
  const epub_mem_media_t mem  = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &book));

  /* Oracle: the whole entry inflated in one shot (the path #231 replaces). */
  size_t ref_got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_get_resource(&book,
                                   "big.dat",
                                   s_fixture.reference,
                                   sizeof(s_fixture.reference),
                                   &ref_got));
  TEST_ASSERT_EQ(k_big_bytes, ref_got);
  TEST_ASSERT_EQ(0, memcmp(s_fixture.reference, s_fixture.big, (size_t)k_big_bytes));

  /* Streamed, chunk by chunk, into a fixed 4 KiB buffer -- provably parity. */
  const uint64_t streamed = internal_stream_and_verify_big(&book);
  TEST_ASSERT_EQ(ref_got, streamed);
  /* The resident chunk buffer is 64x smaller than the entry it paged. */
  TEST_ASSERT(((size_t)k_chunk * 64U) == (size_t)k_big_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub entry: streaming cursor == whole-inflate in a bounded buffer");
}

/* ---------------------------------------------------------------------------
 * Test: streaming from a streamed book keeps the backing window bounded.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_stream_over_streamed_book
 * @brief Streaming `big.dat` from a book opened with `epub_open_streamed()`
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

  buf_src_t           src   = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  epub_stream_media_t media = {.read = internal_direct_read,
                               .ctx  = &src,
                               .size = (uint64_t)s_fixture.archive_size};
  epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open_streamed(&media, "entry.epub", &book));
  TEST_ASSERT(book.zip_bytes == nullptr);

  const uint64_t streamed = internal_stream_and_verify_big(&book);
  TEST_ASSERT_EQ(k_big_bytes, streamed);
  /* Every backing fetch is a bounded miniz chunk, never the whole entry. */
  TEST_ASSERT(s_peak <= (size_t)k_io_bound);
  TEST_ASSERT((size_t)k_io_bound < (size_t)k_big_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub entry: streaming cursor over a streamed book, bounded window");
}

/* ---------------------------------------------------------------------------
 * Test: positioned read of a stored entry.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_entry_pread_windows
 * @brief `epub_entry_pread` reads exact windows of the stored `page.raw`,
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
  epub_book_t            book = {};
  const epub_mem_media_t mem  = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &book));

  uint8_t win[k_raw_bytes] = {};
  size_t  got              = 0U;

  /* Full window. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_pread(&book, "page.raw", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ(k_raw_bytes, got);
  TEST_ASSERT_EQ(0, memcmp(win, s_fixture.raw, (size_t)k_raw_bytes));

  /* Mid window. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_entry_pread(&book, "page.raw", (uint64_t)k_win_off, win, k_win_len, &got));
  TEST_ASSERT_EQ(k_win_len, got);
  TEST_ASSERT_EQ(0, memcmp(win, &s_fixture.raw[k_win_off], (size_t)k_win_len));

  /* Tail window over-read -> short read of exactly the remaining bytes. */
  const uint64_t tail_off = (uint64_t)k_raw_bytes - (uint64_t)k_tail_back;
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_pread(&book, "page.raw", tail_off, win, k_tail_extra, &got));
  TEST_ASSERT_EQ(k_tail_back, got);
  TEST_ASSERT_EQ(0, memcmp(win, &s_fixture.raw[(size_t)tail_off], (size_t)k_tail_back));

  /* At/after EOF -> zero bytes, ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    epub_entry_pread(&book, "page.raw", (uint64_t)k_raw_bytes, win, k_tail_extra, &got));
  TEST_ASSERT_EQ(0U, got);

  /* DEFLATE entry -> not supported (use the cursor). */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 epub_entry_pread(&book, "big.dat", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ(0U, got);

  /* Missing entry -> not found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 epub_entry_pread(&book, "nope.bin", 0U, win, sizeof(win), &got));

  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
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
 * `epub_internal_book_not_ready` helper, whose two conditions are exercised
 * here by a closed book.) @details Executes the entry guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_guards(void)
{
  TEST_BEGIN("epub entry: argument + state guards");
  internal_build_archive();
  epub_book_t            book = {};
  const epub_mem_media_t mem  = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &book));

  epub_entry_reader_t rd     = {};
  uint64_t            sz     = 0U;
  uint8_t             buf[8] = {};
  size_t              got    = 0U;

  /* entry_open NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_open(nullptr, "x", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_open(&book, nullptr, &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_open(&book, "x", nullptr, &sz));
  /* entry_open on a missing entry. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_entry_open(&book, "nope.bin", &rd, &sz));

  /* entry_read guards on a not-open reader. */
  epub_entry_reader_t closed = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_read(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_read(&closed, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_read(&closed, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_entry_read(&closed, buf, sizeof(buf), &got));
  /* entry_read with a valid open reader but zero cap. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_open(&book, "big.dat", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_entry_read(&rd, buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_close(&rd));

  /* entry_close + pread NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_pread(nullptr, "x", 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_pread(&book, nullptr, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_pread(&book, "x", 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_entry_pread(&book, "x", 0U, buf, sizeof(buf), nullptr));

  /* Both cursor + pread reject a closed book (not-ready guard, true arm). */
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_entry_open(&book, "big.dat", &rd, &sz));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_entry_pread(&book, "page.raw", 0U, buf, sizeof(buf), &got));

  TEST_END("epub entry: argument + state guards");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Archive offset of an entry's local file header.
 *
 * @details
 * Reads the central directory through a throwaway miniz reader over
 * ::s_fixture.archive and reports the entry's ``m_local_header_ofs``. The
 * corruption arms need it because they mutate the archive BYTES directly, and
 * only the central directory knows where each entry actually starts.
 *
 * @param[in] name Archive-rooted entry name, e.g. "OEBPS/page.raw".
 *
 * @return Byte offset of that entry's local file header.
 *
 * @pre ::internal_build_archive has run, so the archive is finalised.
 * @pre @p name exists in the archive (asserted).
 * @post The archive bytes are unmodified.
 * @post The throwaway reader is closed before returning.
 *
 * @note Not thread-safe; reads file-static fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_local_header_ofs(const char* name)
{
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_reader_init_mem(&zip, s_fixture.archive, s_fixture.archive_size, 0U) ==
              MZ_TRUE);
  const int32_t idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0U);
  TEST_ASSERT(idx >= 0);
  mz_zip_archive_file_stat st;
  TEST_ASSERT(mz_zip_reader_file_stat(&zip, (mz_uint)idx, &st) == MZ_TRUE);
  (void)mz_zip_reader_end(&zip);
  return (size_t)st.m_local_header_ofs;
}

/**
 * @brief Archive offset of an entry's stored payload.
 *
 * @details
 * The payload starts after the fixed 30-byte local header plus the variable
 * file-name and extra-field spans, whose lengths are read out of that header.
 *
 * @param[in] name Archive-rooted entry name.
 *
 * @return Byte offset of the first payload byte.
 *
 * @pre ::internal_build_archive has run.
 * @pre The entry's local header is intact (this is called before any
 *      signature corruption, never after).
 * @post The archive bytes are unmodified.
 * @post The returned offset is inside the archive.
 *
 * @note Not thread-safe; reads file-static fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_entry_data_ofs(const char* name)
{
  const size_t lho = internal_local_header_ofs(name);
  const size_t fname =
    (size_t)s_fixture.archive[lho + (size_t)k_lfh_fname_len_ofs + (size_t)k_lfh_le_lo] |
    ((size_t)s_fixture.archive[lho + (size_t)k_lfh_fname_len_ofs + (size_t)k_lfh_le_hi]
     << (size_t)k_lfh_le_shift);
  const size_t extra =
    (size_t)s_fixture.archive[lho + (size_t)k_lfh_extra_len_ofs + (size_t)k_lfh_le_lo] |
    ((size_t)s_fixture.archive[lho + (size_t)k_lfh_extra_len_ofs + (size_t)k_lfh_le_hi]
     << (size_t)k_lfh_le_shift);
  const size_t data = lho + (size_t)k_lfh_size + fname + extra;
  TEST_ASSERT(data < s_fixture.archive_size);
  return data;
}

/**
 * @test internal_test_entry_corrupt_local_header_signature
 * @brief A stored entry whose LOCAL header signature has been flipped -- while
 *        its central-directory record stays valid -- is refused by
 *        `epub_entry_pread` rather than read from a guessed offset.
 *
 * @details
 * `internal_stored_data_offset` locates and stats the entry through the
 * central directory (both succeed), then reads the 30-byte local header and
 * hands it to `internal_data_offset`, which memcmps the four signature bytes.
 * Flipping the last signature byte makes that memcmp mismatch, which is the
 * ONLY way to reach both the `return false` inside `internal_data_offset` and
 * the `!internal_data_offset(...)` rejection in its caller. Without the guard
 * the driver would compute a data offset from a header that is not a header.
 *
 * @par MC/DC:
 * Two 1-condition decisions, driven by the same pair of vectors.
 * - apps/shared_libs/epub/src/epub_entry.c@internal_data_offset
 *   `memcmp(hdr, sig, sizeof(sig)) != 0`:
 *   V1 intact header -> false, the offset is computed (control; the intact
 *   pread at the end of this case and internal_test_entry_pread_windows);
 *   V2 signature byte flipped -> true, the helper reports false.
 * - apps/shared_libs/epub/src/epub_entry.c@internal_stored_data_offset
 *   `!internal_data_offset(hdr, ...)`:
 *   V1 helper returned true -> false, pread proceeds;
 *   V2 helper returned false -> true, pread reports
 *   k_ra8_err_validation_failed.
 * N = 1 condition each, N+1 = 2 vectors each; V1 and V2 differ only in the
 * signature bytes, so that condition independently affects both decisions.
 *
 * @pre ::internal_build_archive has run.
 * @pre The book is open over the corrupted archive.
 * @post `epub_entry_pread` reported k_ra8_err_validation_failed and wrote no
 *       bytes.
 * @post The book is closed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_entry_corrupt_local_header_signature(void)
{
  TEST_BEGIN("epub entry: corrupt local-header signature is refused");
  internal_build_archive();

  const size_t lho = internal_local_header_ofs("OEBPS/page.raw");
  s_fixture.archive[lho + (size_t)k_lfh_sig_byte] ^= (uint8_t)k_corrupt_mask;

  epub_book_t            book = {};
  const epub_mem_media_t mem  = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  /* The central directory is untouched, so the book still opens. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &book));

  uint8_t win[k_win_len] = {};
  size_t  got            = k_epub_reread_poison;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_entry_pread(&book, "page.raw", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));

  /* Control: repair the byte and the identical read succeeds. */
  s_fixture.archive[lho + (size_t)k_lfh_sig_byte] ^= (uint8_t)k_corrupt_mask;
  epub_book_t good = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &good));
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_pread(&good, "page.raw", 0U, win, sizeof(win), &got));
  TEST_ASSERT_EQ((size_t)k_win_len, got);
  TEST_ASSERT_EQ(0, memcmp(win, s_fixture.raw, (size_t)k_win_len));
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&good));
  TEST_END("epub entry: corrupt local-header signature is refused");
}

/**
 * @brief Drain one entry reader through the bounded fixture buffer.
 * @param[in,out] rd Open entry reader advanced to end-of-stream.
 * @param[out] chunk Caller-owned bounded transfer buffer.
 * @param[in] chunk_size Size of @p chunk in bytes.
 * @return Number of payload bytes consumed.
 * @pre @p rd is open and positioned at the start of an entry.
 * @pre @p chunk addresses at least @p chunk_size writable bytes.
 * @post The reader is positioned at end-of-stream.
 * @post The return value is the sum of all successful reads.
 * @note Test-only helper; assertions stop on an unexpected read failure.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t
internal_drain_entry(epub_entry_reader_t* rd, uint8_t* chunk, size_t chunk_size)
{
  uint64_t off   = 0U;
  uint32_t reads = 0U;
  for (;;) {
    size_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, epub_entry_read(rd, chunk, chunk_size, &got));
    if (got == 0U) {
      break;
    }
    off += (uint64_t)got;
    ++reads;
    TEST_ASSERT(reads <= ((uint32_t)k_raw_bytes / (uint32_t)k_chunk) + 2U); /* bounded loop */
  }
  return off;
}

/**
 * @test internal_test_entry_close_reports_crc_mismatch
 * @brief Closing a fully-consumed reader whose payload was tampered with
 *        reports the CRC failure instead of a clean k_ra8_ok.
 *
 * @details
 * `page.raw` is STORED, so the extract iterator hands the bytes straight
 * through and accumulates CRC32 as it goes; nothing compares that running CRC
 * until `mz_zip_reader_extract_iter_free`. Flipping one payload byte
 * therefore leaves every read succeeding -- `consumed` still reaches `total`,
 * so `epub_entry_close` takes its `full == true` arm -- and only the free()
 * verdict distinguishes a good archive from a tampered one. That is exactly
 * the "silent corruption" this return exists to catch.
 *
 * @par MC/DC:
 * Decision: `if (ok == MZ_FALSE)` inside the `full` arm of
 * apps/shared_libs/epub/src/epub_entry.c@epub_entry_close (1 condition).
 * - V1: intact payload -> free() reports MZ_TRUE -> false -> close returns
 *   k_ra8_ok (control; the intact close at the end of this case, and every
 *   other close in this file).
 * - V2: one payload byte flipped -> the running CRC32 disagrees with the
 *   central directory's -> free() reports MZ_FALSE -> true -> close returns
 *   k_ra8_err_validation_failed (this case).
 * N = 1 condition, N+1 = 2 vectors. Both run the same full-consume sequence
 * and differ only in the free() verdict, so `ok` independently affects the
 * decision. The enclosing `full` condition is held TRUE in both, which is
 * what makes this the CRC arm and not the partial-read arm.
 *
 * @pre ::internal_build_archive has run.
 * @pre The stored entry is read to completion before close.
 * @post `epub_entry_close` reported k_ra8_err_validation_failed once.
 * @post The reader is left closed either way.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_entry_close_reports_crc_mismatch(void)
{
  TEST_BEGIN("epub entry: close reports a CRC mismatch on a fully-read entry");
  internal_build_archive();

  const size_t data_ofs = internal_entry_data_ofs("OEBPS/page.raw");
  s_fixture.archive[data_ofs + (size_t)k_corrupt_data_ofs] ^= (uint8_t)k_corrupt_mask;

  epub_book_t            book = {};
  const epub_mem_media_t mem  = {.data = s_fixture.archive, .size = s_fixture.archive_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &book));

  epub_entry_reader_t rd   = {};
  uint64_t            size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_open(&book, "page.raw", &rd, &size));
  TEST_ASSERT_EQ(k_raw_bytes, size);

  /* Read every byte: a stored entry never fails mid-stream, so the reader
     reaches EOF cleanly and close() is the only place the tamper can show. */
  uint8_t        chunk[k_chunk] = {};
  const uint64_t off            = internal_drain_entry(&rd, chunk, sizeof chunk);
  TEST_ASSERT_EQ(k_raw_bytes, off);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, epub_entry_close(&rd));
  TEST_ASSERT(rd.iter == nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));

  /* Control: repair the byte and the identical sequence closes clean. */
  s_fixture.archive[data_ofs + (size_t)k_corrupt_data_ofs] ^= (uint8_t)k_corrupt_mask;
  epub_book_t good = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&mem, "entry.epub", &good));
  epub_entry_reader_t good_rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_open(&good, "page.raw", &good_rd, &size));
  const uint64_t good_off = internal_drain_entry(&good_rd, chunk, sizeof chunk);
  TEST_ASSERT_EQ(k_raw_bytes, good_off);
  TEST_ASSERT_EQ(k_ra8_ok, epub_entry_close(&good_rd));
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&good));
  TEST_END("epub entry: close reports a CRC mismatch on a fully-read entry");
}

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
int main(void)
{
  internal_test_entry_stream_parity_bounded();
  internal_test_entry_stream_over_streamed_book();
  internal_test_entry_pread_windows();
  internal_test_entry_guards();
  internal_test_entry_corrupt_local_header_signature();
  internal_test_entry_close_reports_crc_mismatch();
  return 0;
}

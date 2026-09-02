/**
 * @file test_book_chunked.c
 * @brief Tests for the demand-paged "RBKC" chunk reader (book_chunked.c).
 *
 * @details
 * Exercises `book_chunked_open()` / `book_chunked_read()` against an
 * in-memory container built with *real* zlib streams (miniz `mz_compress2`,
 * the same RFC 1950 wrapping `tools/epub_compile` emits) and inflated with a
 * tinfl wrapper shaped like the shipping shelf inflater. Covers: the open
 * path's header / table / capacity guards and its bound-fields contract; the
 * read path's alignment + span contract, sequential whole-object reassembly
 * (byte-identical to the source), and every error leg (unbound reader,
 * unaligned offset, past-end chunk, wrong span, file-read error passthrough,
 * corrupt stream, and a valid stream that inflates to the wrong length).
 *
 * The payload here is patterned bytes, not a valid RABOOK1 blob -- the chunk
 * reader is a byte-transport layer; blob validity is the paged accessor
 * equivalence test's concern (test_book_paged.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book_chunked.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Copy an object representation through compatible byte-pointer types. */
static void internal_copy_object(void* dst, const void* src, size_t len)
{
  (void)memcpy(dst, src, len);
}

/**
 * @enum bcx_dim_t
 * @brief Fixture geometry and buffer budgets.
 */
typedef enum : uint32_t {
  k_bcx_blob_len     = 600U,  /**< Patterned source-blob length.              */
  k_bcx_chunk_bytes  = 256U,  /**< Inflated bytes per chunk (3 chunks).       */
  k_bcx_chunk_count  = 3U,    /**< ceil(600 / 256).                           */
  k_bcx_last_span    = 88U,   /**< 600 - 2 * 256: the short final chunk.      */
  k_bcx_short_span   = 60U,   /**< Under-length last stream for the mismatch. */
  k_bcx_file_cap     = 2048U, /**< Container build buffer budget.             */
  k_bcx_table_cap    = 8U,    /**< Chunk-table entry budget (needs 4).        */
  k_bcx_staging_cap  = 512U,  /**< Compressed-chunk staging budget.           */
  k_bcx_tiny_table   = 3U,    /**< One entry short of the needed 4.           */
  k_bcx_tiny_staging = 8U,    /**< Smaller than any real stream.              */
  k_bcx_short_file   = 10U,   /**< File length below the fixed header.        */
  k_bcx_table_cut    = 40U,   /**< File length inside the chunk table.        */
  k_bcx_pattern_mul  = 31U,   /**< Byte-pattern multiplier.                   */
  k_bcx_pattern_add  = 7U,    /**< Byte-pattern offset.                       */
} bcx_dim_t;

/**
 * @enum bcx_validate_dim_t
 * @brief Workspace budgets and 32-bit fault magnitudes used by the
 *        strict-validation guard vectors.
 */
typedef enum : uint32_t {
  k_bcx_validate_scratch = 64U,  /**< Strict-validation scratch budget.       */
  k_bcx_short_chunk_cap  = 255U, /**< One byte under the inflated chunk size. */
  k_bcx_one_chunk_byte   = 1U,   /**< Chunk size that forces a huge count.    */
  k_bcx_table_bad_first  = 1U,   /**< Non-zero first chunk-table entry.       */
} bcx_validate_dim_t;

/**
 * @enum bcx_validate_wide_t
 * @brief 64-bit fault magnitudes for the reader-geometry rejections.
 */
typedef enum : uint64_t {
  k_bcx_total_over_u32  = (uint64_t)UINT32_MAX + 2U, /**< Chunk count over UINT32_MAX. */
  k_bcx_payload_ceiling = UINT64_MAX,                /**< Payload offset at the top.   */
} bcx_validate_wide_t;

/**
 * @enum bcx_validate_addr_t
 * @brief Address-space ceiling gap for the unrepresentable-span vectors.
 */
typedef enum : uintptr_t {
  k_bcx_ceiling_gap = 8U, /**< Bytes between a fabricated base and UINTPTR_MAX. */
} bcx_validate_addr_t;

/**
 * @enum bcx_table_idx_t
 * @brief Chunk-table entry indices mutated by the table-shape vectors.
 */
typedef enum : uint8_t {
  k_bcx_table_first  = 0U, /**< First chunk-table entry.  */
  k_bcx_table_second = 1U, /**< Second chunk-table entry. */
} bcx_table_idx_t;

/**
 * @struct bcx_file_t
 * @brief An in-memory container "file" served through the read callback.
 */
typedef struct {
  const uint8_t* data;    /**< Container bytes.                           */
  uint64_t       len;     /**< Container length.                          */
  uint32_t       fail_at; /**< 1-based call index to fail on (0 = never). */
  uint32_t       calls;   /**< Calls served so far.                       */
} bcx_file_t;

/** @brief ra8_vsource_read_fn over an in-memory container, with fault injection. */
static ra8_err_t bcx_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  bcx_file_t* f = (bcx_file_t*)ctx;
  f->calls++;
  if ((f->fail_at != 0U) && (f->calls == f->fail_at)) {
    return k_ra8_err_hw_timeout;
  }
  if ((offset + (uint64_t)len) > f->len) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &f->data[offset], len);
  return k_ra8_ok;
}

/** @brief zlib inflater matching book_inflate_fn (mirrors the shelf inflater). */
static ra8_err_t
bcx_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  static tinfl_decompressor s_tinfl;
  tinfl_init(&s_tinfl);
  size_t             in_n  = src_len;
  size_t             out_n = dst_cap;
  const tinfl_status st    = tinfl_decompress(
    &s_tinfl,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (st != TINFL_STATUS_DONE) {
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

/** @brief The patterned source blob every container in this file carries. */
static uint8_t s_blob[k_bcx_blob_len];

/** @brief Fill the source blob with a deterministic byte pattern. */
static void bcx_fill_blob(void)
{
  for (uint32_t i = 0U; i < k_bcx_blob_len; ++i) {
    s_blob[i] = (uint8_t)((i * k_bcx_pattern_mul) + k_bcx_pattern_add);
  }
}

/**
 * @brief Deflate every chunk of ::s_blob, recording lengths and offsets.
 *
 * @details
 * @p short_last truncates the final chunk's source span, which is how the
 * caller fabricates a container whose last chunk inflates to fewer bytes than
 * the declared chunk size -- the short-tail case the reader must handle.
 *
 * @param[out] streams    Per-chunk deflate streams.
 * @param[out] stream_len Per-chunk compressed length.
 * @param[out] offs       Running offsets, `count + 1` entries starting at 0.
 * @param[in]  short_last Whether to truncate the last chunk's source span.
 *
 * @pre Every out-parameter has room for ::k_bcx_chunk_count entries.
 * @pre ::s_blob holds ::k_bcx_blob_len bytes.
 * @post `offs[i+1] - offs[i]` is chunk i's compressed length.
 * @post Every compression reported MZ_OK.
 *
 * @note Not thread-safe (reads the shared ::s_blob).
 * @since 0.1.0
 */
static void bcx_compress_chunks(uint8_t   streams[][k_bcx_staging_cap],
                                mz_ulong* stream_len,
                                uint64_t* offs,
                                bool      short_last)
{
  for (uint32_t i = 0U; i < (uint32_t)k_bcx_chunk_count; ++i) {
    const uint32_t at   = i * k_bcx_chunk_bytes;
    uint32_t       span = k_bcx_blob_len - at;
    if (span > k_bcx_chunk_bytes) {
      span = k_bcx_chunk_bytes;
    }
    if (short_last && (i == ((uint32_t)k_bcx_chunk_count - 1U))) {
      span = k_bcx_short_span;
    }
    stream_len[i] = (mz_ulong)k_bcx_staging_cap;
    const int rc =
      mz_compress2(streams[i], &stream_len[i], &s_blob[at], (mz_ulong)span, MZ_BEST_COMPRESSION);
    TEST_ASSERT_EQ(MZ_OK, rc);
    offs[i + 1U] = offs[i] + (uint64_t)stream_len[i];
  }
}

/**
 * @brief Write the RBKC container header; return the offset just past it.
 *
 * @param[out] out   Container buffer.
 * @param[in]  count Chunk count to record.
 * @return Byte offset of the first field after the header.
 *
 * @pre @p out has room for the fixed header.
 * @pre @p count matches the number of chunk streams that follow.
 * @post The magic, chunk size, inflated total, count and reserved word are
 *       written in order.
 * @post The reserved word is zero, as the reader expects.
 *
 * @note Not thread-safe with respect to @p out.
 * @since 0.1.0
 */
static size_t bcx_write_header(uint8_t* out, uint32_t count)
{
  /** @brief RBKC chunked-container magic (raw bytes; no string terminator). */
  static const uint8_t k_bcx_magic_rbkc[] = {'R', 'B', 'K', 'C'};
  size_t               pos                = 0U;
  (void)memcpy(&out[pos], k_bcx_magic_rbkc, sizeof(k_bcx_magic_rbkc));
  pos += 4U;
  const uint32_t chunk_bytes = k_bcx_chunk_bytes;
  internal_copy_object(&out[pos], &chunk_bytes, sizeof(chunk_bytes));
  pos += sizeof(chunk_bytes);
  const uint64_t total = k_bcx_blob_len;
  internal_copy_object(&out[pos], &total, sizeof(total));
  pos += sizeof(total);
  internal_copy_object(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  const uint32_t reserved = 0U;
  internal_copy_object(&out[pos], &reserved, sizeof(reserved));
  pos += sizeof(reserved);
  return pos;
}

static uint64_t bcx_pack(uint8_t* out, bool short_last)
{
  const uint32_t count                        = k_bcx_chunk_count;
  uint64_t       offs[k_bcx_chunk_count + 1U] = {};
  uint8_t        streams[k_bcx_chunk_count][k_bcx_staging_cap];
  mz_ulong       stream_len[k_bcx_chunk_count] = {};

  bcx_compress_chunks(streams, stream_len, offs, short_last);

  size_t pos = bcx_write_header(out, count);
  internal_copy_object(&out[pos], offs, sizeof(offs));
  pos += sizeof(offs);
  for (uint32_t i = 0U; i < count; ++i) {
    (void)memcpy(&out[pos], streams[i], (size_t)stream_len[i]);
    pos += (size_t)stream_len[i];
  }
  return (uint64_t)pos;
}

/** @brief Open a freshly packed container into @p rd; returns the open result. */
static ra8_err_t bcx_open(book_chunked_t* rd,
                          bcx_file_t*     file,
                          const uint8_t*  container,
                          uint64_t        file_len,
                          uint64_t*       table_buf,
                          uint8_t*        staging)
{
  *file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  return book_chunked_open(rd,
                           bcx_file_read,
                           file,
                           file_len,
                           bcx_inflate,
                           table_buf,
                           k_bcx_table_cap,
                           staging,
                           k_bcx_staging_cap);
}

/**
 * @test test_book_chunked_open_happy
 * @brief A well-formed container binds with the parsed geometry exposed.
 *
 * @par Targeted code:
 * `book_chunked_open`'s success path end-to-end: header read + parse,
 * `internal_load_table`'s read, monotonic walk, staging-fit check, and the bound
 * `table` / `payload_off` publication.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the happy path is the all-false leg
 * of every open guard; the header-fields compound is covered by
 * test_mcdc_container_header_fields in test_book_cov.c)
 */
static void test_book_chunked_open_happy(void)
{
  TEST_BEGIN("book_chunked_open happy path");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  book_chunked_t rd                         = {};
  bcx_file_t     file                       = {};
  uint64_t       table_buf[k_bcx_table_cap] = {};
  static uint8_t s_staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  TEST_ASSERT_EQ(k_bcx_chunk_bytes, rd.chunk_bytes);
  TEST_ASSERT_EQ(k_bcx_chunk_count, rd.chunk_count);
  TEST_ASSERT_EQ(k_bcx_blob_len, rd.inflated_total);
  TEST_ASSERT(rd.table == table_buf);
  TEST_ASSERT_EQ(k_book_container_header_len +
                   ((uint64_t)(k_bcx_chunk_count + 1U) * k_book_container_entry_len),
                 rd.payload_off);
  TEST_ASSERT_EQ(0U, rd.table[0]);

  TEST_END("book_chunked_open happy path");
}

/**
 * @struct bcx_guard_ctx_t
 * @brief Shared buffers and container image for the chunked open-guard helpers.
 * @invariant All pointers reference caller-owned storage; @c file_len is the
 *            packed length of @c container.
 * @see test_book_chunked_open_guards
 */
typedef struct {
  uint8_t*  container; /**< Packed container image under test.   */
  uint64_t  file_len;  /**< Byte length of the packed container. */
  uint64_t* table;     /**< Caller chunk-table buffer.           */
  uint8_t*  staging;   /**< Caller staging buffer.               */
} bcx_guard_ctx_t;

/**
 * @brief Assert one null `book_chunked_open` argument yields null_ptr.
 * @param[out] rd       Reader handle (nullptr exercises the rd guard).
 * @param[in]  read     File reader (nullptr exercises the file_read guard).
 * @param[in]  file     File context passed to @p read.
 * @param[in]  inflate  Decompressor (nullptr exercises the inflate guard).
 * @param[out] table    Chunk-table buffer (nullptr exercises the table guard).
 * @param[out] staging  Staging buffer (nullptr exercises the staging guard).
 * @param[in]  file_len Packed container length in bytes.
 * @pre Exactly one pointer argument is nullptr.
 * @post `book_chunked_open` returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_expect_null_open(book_chunked_t*     rd,
                                 ra8_vsource_read_fn read,
                                 void*               file,
                                 book_inflate_fn     inflate,
                                 uint64_t*           table,
                                 uint8_t*            staging,
                                 uint64_t            file_len)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_open(rd,
                                   read,
                                   file,
                                   file_len,
                                   inflate,
                                   table,
                                   k_bcx_table_cap,
                                   staging,
                                   k_bcx_staging_cap));
}

/**
 * @brief Open with explicit capacities and assert the expected error.
 * @param[in]     want      Expected result code.
 * @param[out]    rd        Reader handle under test.
 * @param[in,out] file      File context, reset to the container image.
 * @param[in]     g         Shared buffers and container image.
 * @param[in]     table_cap Chunk-table entry budget.
 * @param[in]     stage_cap Staging-buffer byte budget.
 * @pre @p g fields reference valid buffers.
 * @post `book_chunked_open` returned @p want.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_expect_open_err(ra8_err_t              want,
                                book_chunked_t*        rd,
                                bcx_file_t*            file,
                                const bcx_guard_ctx_t* g,
                                uint32_t               table_cap,
                                uint32_t               stage_cap)
{
  *file = (bcx_file_t){.data = g->container, .len = g->file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(want,
                 book_chunked_open(rd,
                                   bcx_file_read,
                                   file,
                                   g->file_len,
                                   bcx_inflate,
                                   g->table,
                                   table_cap,
                                   g->staging,
                                   stage_cap));
}

/**
 * @brief Exercise the five null-pointer guards of `book_chunked_open`.
 * @param[out]    rd   Reader handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 * @pre @p g fields reference valid buffers.
 * @post Each guarded argument returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_open_guards_null(book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
{
  bcx_expect_null_open(nullptr,
                       bcx_file_read,
                       file,
                       bcx_inflate,
                       g->table,
                       g->staging,
                       g->file_len);
  bcx_expect_null_open(rd, nullptr, file, bcx_inflate, g->table, g->staging, g->file_len);
  bcx_expect_null_open(rd, bcx_file_read, file, nullptr, g->table, g->staging, g->file_len);
  bcx_expect_null_open(rd, bcx_file_read, file, bcx_inflate, nullptr, g->staging, g->file_len);
  bcx_expect_null_open(rd, bcx_file_read, file, bcx_inflate, g->table, nullptr, g->file_len);
}

/**
 * @brief Exercise the short-file, bad-magic and capacity rejections of open.
 * @param[out]    rd   Reader handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 * @pre @p g fields reference valid buffers.
 * @post Every malformed size / magic / budget returned its rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_open_guards_capacity(book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
{
  /* File shorter than the fixed header. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 bcx_open(rd, file, g->container, k_bcx_short_file, g->table, g->staging));
  TEST_ASSERT(rd->table == nullptr);

  /* Bad magic. */
  static uint8_t s_bad_magic[k_bcx_file_cap];
  (void)memcpy(s_bad_magic, g->container, (size_t)g->file_len);
  s_bad_magic[0] = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 bcx_open(rd, file, s_bad_magic, g->file_len, g->table, g->staging));

  /* Table needs more entries than the caller budgeted. */
  bcx_expect_open_err(k_ra8_err_invalid_size, rd, file, g, k_bcx_tiny_table, k_bcx_staging_cap);

  /* File ends inside the chunk table. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 bcx_open(rd, file, g->container, k_bcx_table_cut, g->table, g->staging));

  /* A compressed chunk larger than the staging budget. */
  bcx_expect_open_err(k_ra8_err_invalid_size, rd, file, g, k_bcx_table_cap, k_bcx_tiny_staging);
}

/**
 * @brief Assert a file-read failure on the header read propagates verbatim.
 * @param[out]    rd   Reader handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 * @pre @p g fields reference valid buffers.
 * @post `book_chunked_open` returned k_ra8_err_hw_timeout.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_open_guards_readfail(book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
{
  *file = (bcx_file_t){.data = g->container, .len = g->file_len, .fail_at = 1U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 book_chunked_open(rd,
                                   bcx_file_read,
                                   file,
                                   g->file_len,
                                   bcx_inflate,
                                   g->table,
                                   k_bcx_table_cap,
                                   g->staging,
                                   k_bcx_staging_cap));
}

/**
 * @test test_book_chunked_open_guards
 * @brief Every open-path guard rejects its malformed input.
 *
 * @par Targeted code:
 * The five `RA8_CHECK_NULL_PTR` guards, the short-file return, the bad-magic
 * rejection (via `priv_book_container_header_fields`), `internal_load_table`'s
 * table-capacity return, its header-plus-table file-bound return, its
 * staging-capacity return, and the file-read error passthrough.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every open guard is an independent
 * single-condition check)
 */
static void test_book_chunked_open_guards(void)
{
  TEST_BEGIN("book_chunked_open guards");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  book_chunked_t rd                         = {};
  bcx_file_t     file                       = {};
  uint64_t       table_buf[k_bcx_table_cap] = {};
  static uint8_t s_staging[k_bcx_staging_cap];
  file = (bcx_file_t){.data = s_container, .len = file_len, .fail_at = 0U, .calls = 0U};

  const bcx_guard_ctx_t g = {.container = s_container,
                             .file_len  = file_len,
                             .table     = table_buf,
                             .staging   = s_staging};

  bcx_open_guards_null(&rd, &file, &g);
  bcx_open_guards_capacity(&rd, &file, &g);
  bcx_open_guards_readfail(&rd, &file, &g);

  TEST_END("book_chunked_open guards");
}

/**
 * @test test_book_chunked_read_happy
 * @brief Sequential chunk-aligned reads reassemble the source byte-for-byte.
 *
 * @par Targeted code:
 * `book_chunked_read`'s full-chunk path twice and the short-final-chunk
 * clamp once; each read is one staged file read plus one real zlib inflate.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the read path is a chain of
 * independent single-condition checks)
 */
static void test_book_chunked_read_happy(void)
{
  TEST_BEGIN("book_chunked_read sequential reassembly");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  book_chunked_t rd                         = {};
  bcx_file_t     file                       = {};
  uint64_t       table_buf[k_bcx_table_cap] = {};
  static uint8_t s_staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  static uint8_t s_out[k_bcx_blob_len];
  (void)memset(s_out, 0, sizeof(s_out));
  for (uint32_t at = 0U; at < k_bcx_blob_len; at += k_bcx_chunk_bytes) {
    uint32_t span = k_bcx_blob_len - at;
    if (span > k_bcx_chunk_bytes) {
      span = k_bcx_chunk_bytes;
    }
    TEST_ASSERT_EQ(k_ra8_ok, book_chunked_read(&rd, at, &s_out[at], span));
  }
  TEST_ASSERT_EQ(0, memcmp(s_out, s_blob, k_bcx_blob_len));

  TEST_END("book_chunked_read sequential reassembly");
}

/**
 * @brief Exercise the null and unbound-reader guards of `book_chunked_read`.
 * @param[out] rd  A zero-initialised (unopened) reader handle.
 * @param[out] out Output buffer, at least k_bcx_chunk_bytes long.
 * @pre @p rd is zero-initialised.
 * @post Both null guards and the unbound-state guard returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_read_guards_unbound(book_chunked_t* rd, uint8_t* out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, book_chunked_read(nullptr, 0U, out, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, book_chunked_read(rd, 0U, nullptr, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, book_chunked_read(rd, 0U, out, k_bcx_chunk_bytes));
}

/**
 * @brief Assert a stream that inflates to the wrong span is rejected.
 * @param[out] out     Output buffer, at least k_bcx_chunk_bytes long.
 * @param[in]  staging Shared staging buffer for the open.
 * @pre The chunk blob fixtures are populated (bcx_fill_blob()).
 * @post `book_chunked_read` returned k_ra8_err_invalid_size.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_read_guards_shortspan(uint8_t* out, uint8_t* staging)
{
  static uint8_t s_short_container[k_bcx_file_cap];
  const uint64_t short_len               = bcx_pack(s_short_container, true);
  book_chunked_t srd                     = {};
  bcx_file_t     sfile                   = {};
  uint64_t       stable[k_bcx_table_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&srd, &sfile, s_short_container, short_len, stable, staging));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 book_chunked_read(&srd, (uint64_t)2U * k_bcx_chunk_bytes, out, k_bcx_last_span));
}

/**
 * @test test_book_chunked_read_guards
 * @brief Every read-path guard rejects its malformed request.
 *
 * @par Targeted code:
 * `book_chunked_read`'s null guards, the unbound-reader return, the
 * past-end return, the alignment return, the wrong-span return, the file-read
 * error passthrough, the corrupt-stream inflater passthrough, and the
 * valid-stream-wrong-length return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every read guard is an independent
 * single-condition check)
 */
static void test_book_chunked_read_guards(void)
{
  TEST_BEGIN("book_chunked_read guards");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  book_chunked_t rd                         = {};
  bcx_file_t     file                       = {};
  uint64_t       table_buf[k_bcx_table_cap] = {};
  static uint8_t s_staging[k_bcx_staging_cap];
  static uint8_t s_out[k_bcx_chunk_bytes];

  /* Null + unbound guards. */
  bcx_read_guards_unbound(&rd, s_out);

  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  /* Past the inflated total. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 book_chunked_read(&rd,
                                   (uint64_t)k_bcx_chunk_count * k_bcx_chunk_bytes,
                                   s_out,
                                   k_bcx_chunk_bytes));

  /* Not chunk-aligned. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 book_chunked_read(&rd, k_bcx_chunk_bytes / 2U, s_out, k_bcx_chunk_bytes));

  /* Wrong span: a full chunk asked short, the short final chunk asked full. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes - 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    book_chunked_read(&rd, (uint64_t)2U * k_bcx_chunk_bytes, s_out, k_bcx_chunk_bytes));

  /* File-read failure while staging the stream propagates verbatim. */
  file.fail_at = file.calls + 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes));
  file.fail_at = 0U;

  /* A corrupt stream fails in the inflater and propagates. */
  const uint64_t c0_at        = rd.payload_off + rd.table[0] + 4U;
  const uint8_t  saved        = s_container[c0_at];
  s_container[c0_at]          = (uint8_t)~saved;
  const ra8_err_t corrupt_err = book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes);
  s_container[c0_at]          = saved;
  TEST_ASSERT(corrupt_err != k_ra8_ok);

  /* A valid stream that inflates to the wrong span is rejected. */
  bcx_read_guards_shortspan(s_out, s_staging);

  TEST_END("book_chunked_read guards");
}

/**
 * @struct bcx_validate_ctx_t
 * @brief Opened reader plus the caller workspaces every strict-validation
 *        guard vector shares.
 * @invariant Every pointer references caller-owned storage that out-lives the
 *            vectors, and @c table is a mutable copy of the reader's table.
 * @see test_book_chunked_validate_guards
 */
typedef struct {
  book_chunked_t* rd;      /**< Opened reader under test.        */
  uint8_t*        chunk;   /**< Inflated-chunk workspace.        */
  uint8_t*        scratch; /**< Strict-validation scratch.       */
  uint64_t*       table;   /**< Mutable copy of the chunk table. */
} bcx_validate_ctx_t;

/** @brief Assert one fabricated reader is rejected as an invalid reader state. @details Runs strict validation with valid workspaces, so @p broken is the only defect, then proves the result object was cleared first. @param[in] v Shared reader and workspace fixture. @param[in] broken Reader descriptor carrying the fabricated field(s). @pre @p v references live workspaces sized for the fixture geometry. @pre @p broken differs from the opened reader only in the field under test. @post `book_chunked_validate_strict` returned k_ra8_err_invalid_state. @post The caller's result object was zeroed before the rejection. @note Not thread-safe; single-threaded host-test helper. @since 0.1.0 */
RA8_INTERNAL static void bcx_expect_state_reject(const bcx_validate_ctx_t* v, book_chunked_t broken)
{
  book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 book_chunked_validate_strict(&broken,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
}

/** @brief Exercise the pointer and capacity guards of strict validation. @details Supplies exactly one invalid argument per call so each guard is the only reason the request can fail. @param[in] v Shared reader and workspace fixture. @pre @p v references live workspaces sized for the fixture geometry. @pre The reader in @p v is open and internally consistent. @post Every null argument returned k_ra8_err_null_ptr. @post Every zero or short capacity returned k_ra8_err_invalid_size. @note Not thread-safe; single-threaded host-test helper. @since 0.1.0 */
RA8_INTERNAL static void bcx_validate_arg_guards(const bcx_validate_ctx_t* v)
{
  book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(v->rd,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(nullptr,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(v->rd,
                                              nullptr,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(v->rd,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              nullptr,
                                              k_bcx_validate_scratch,
                                              &hdr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    book_chunked_validate_strict(v->rd, v->chunk, 0U, v->scratch, k_bcx_validate_scratch, &hdr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    book_chunked_validate_strict(v->rd, v->chunk, k_bcx_chunk_bytes, v->scratch, 0U, &hdr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 book_chunked_validate_strict(v->rd,
                                              v->chunk,
                                              k_bcx_short_chunk_cap,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              &hdr));
}

/** @brief Reject a reader whose scalar geometry or callbacks are incomplete. @details Copies the open reader and fabricates exactly one field per vector, covering each independent single-condition reader-state guard. @param[in] v Shared reader and workspace fixture. @pre @p v references live workspaces sized for the fixture geometry. @pre The reader in @p v is open and internally consistent. @post Every fabricated reader returned k_ra8_err_invalid_state. @post The opened reader itself is left unmodified. @note Not thread-safe; single-threaded host-test helper. @since 0.1.0 */
RA8_INTERNAL static void bcx_validate_reader_fields(const bcx_validate_ctx_t* v)
{
  book_chunked_t broken = *v->rd;
  broken.file_read      = nullptr;
  bcx_expect_state_reject(v, broken);
  broken            = *v->rd;
  broken.inflate_cb = nullptr;
  bcx_expect_state_reject(v, broken);
  broken       = *v->rd;
  broken.table = nullptr;
  bcx_expect_state_reject(v, broken);
  broken             = *v->rd;
  broken.chunk_bytes = 0U;
  bcx_expect_state_reject(v, broken);
  broken             = *v->rd;
  broken.chunk_count = 0U;
  bcx_expect_state_reject(v, broken);
  broken                = *v->rd;
  broken.inflated_total = 0U;
  bcx_expect_state_reject(v, broken);
  broken             = *v->rd;
  broken.staging_cap = 0U;
  bcx_expect_state_reject(v, broken);
  broken                = *v->rd;
  broken.chunk_bytes    = k_bcx_one_chunk_byte;
  broken.inflated_total = k_bcx_total_over_u32;
  bcx_expect_state_reject(v, broken);
  broken             = *v->rd;
  broken.payload_off = k_bcx_payload_ceiling;
  bcx_expect_state_reject(v, broken);
}

/** @brief Reject a reader whose chunk table has an illegal shape. @details Restores a pristine table copy before each mutation so exactly one table invariant is broken per vector. @param[in] v Shared reader and workspace fixture, including a table copy. @pre @p v references live workspaces sized for the fixture geometry. @pre `v->table` has room for `chunk_count + 1` entries. @post A non-zero first entry, a non-increasing entry, and an over-staging chunk each returned k_ra8_err_invalid_state. @post The reader's own table is never modified. @note Not thread-safe; single-threaded host-test helper. @since 0.1.0 */
RA8_INTERNAL static void bcx_validate_reader_table(const bcx_validate_ctx_t* v)
{
  book_chunked_t broken      = *v->rd;
  broken.table               = v->table;
  const size_t entries_bytes = ((size_t)k_bcx_chunk_count + 1U) * sizeof(v->table[0]);

  (void)memcpy(v->table, v->rd->table, entries_bytes);
  v->table[k_bcx_table_first] = k_bcx_table_bad_first;
  bcx_expect_state_reject(v, broken);

  (void)memcpy(v->table, v->rd->table, entries_bytes);
  v->table[k_bcx_table_second] = v->table[k_bcx_table_first];
  bcx_expect_state_reject(v, broken);

  (void)memcpy(v->table, v->rd->table, entries_bytes);
  v->table[k_bcx_table_second] = (uint64_t)k_bcx_staging_cap + 1U;
  bcx_expect_state_reject(v, broken);
}

/** @brief Reject spans whose exclusive end address is not representable. @details Supplies a result object and then a reader staging buffer whose declared extent runs past the address ceiling, proving the alias check fails closed instead of comparing wrapped addresses. @param[in] v Shared reader and workspace fixture. @pre @p v references live workspaces sized for the fixture geometry. @pre No fabricated span is ever dereferenced by the implementation. @post Both unrepresentable spans returned k_ra8_err_invalid_arg. @post A detected alias rejection leaves the caller's result untouched. @note Not thread-safe; single-threaded host-test helper. @since 0.1.0 */
RA8_INTERNAL static void bcx_validate_span_overflow(const bcx_validate_ctx_t* v)
{
  book_header_t* const ceiling = (book_header_t*)(UINTPTR_MAX - (uintptr_t)k_bcx_ceiling_gap);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 book_chunked_validate_strict(v->rd,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              ceiling));

  book_header_t  hdr    = {.total_size = UINT32_MAX};
  book_chunked_t broken = *v->rd;
  broken.staging        = (uint8_t*)(UINTPTR_MAX - (uintptr_t)k_bcx_ceiling_gap);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 book_chunked_validate_strict(&broken,
                                              v->chunk,
                                              k_bcx_chunk_bytes,
                                              v->scratch,
                                              k_bcx_validate_scratch,
                                              &hdr));
  TEST_ASSERT_EQ(UINT32_MAX, hdr.total_size);
}

/**
 * @test test_book_chunked_validate_guards
 * @brief Every strict-validation guard rejects its malformed input before I/O.
 *
 * @par Targeted code:
 * `book_chunked_validate_strict`'s argument guards (null result, null
 * reader, null workspace, zero capacity, chunk buffer shorter than the
 * inflated chunk size), `internal_validate_reader`'s callback, geometry,
 * capacity, chunk-count and table-shape rejections, `internal_spans_overlap`'s
 * unrepresentable-end returns, and `internal_output_is_aliased`'s zero-length
 * skip plus its fail-closed return for an unrepresentable span.
 *
 * @par MC/DC:
 * Decision `(chunk == nullptr) || (scratch == nullptr)`:
 * - Vector 1: chunk=valid, scratch=valid -> false (the short-capacity vector
 *   reaches the later capacity guard, so this decision was false)
 * - Vector 2: chunk=NULL, scratch=valid -> true (varies chunk only)
 * - Vector 3: chunk=valid, scratch=NULL -> true (varies scratch only)
 * Vectors 1+2 prove `chunk` independently affects the outcome; 1+3 prove the
 * same for `scratch`. N+1 = 3 vectors for N=2 conditions.
 * Decision `(chunk_cap == 0U) || (scratch_cap == 0U)`:
 * - Vector 4: chunk_cap=256, scratch_cap=64 -> false (control)
 * - Vector 5: chunk_cap=0, scratch_cap=64 -> true (varies chunk_cap only)
 * - Vector 6: chunk_cap=256, scratch_cap=0 -> true (varies scratch_cap only)
 * Vectors 4+5 and 4+6 give each condition an independently determining pair.
 */
static void test_book_chunked_validate_guards(void)
{
  TEST_BEGIN("book_chunked_validate_strict guards");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  book_chunked_t rd                         = {};
  bcx_file_t     file                       = {};
  uint64_t       table_buf[k_bcx_table_cap] = {};
  static uint8_t s_staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  static uint8_t           s_chunk[k_bcx_chunk_bytes];
  static uint8_t           s_scratch[k_bcx_validate_scratch];
  uint64_t                 bad_table[k_bcx_table_cap] = {};
  const bcx_validate_ctx_t v                          = {.rd      = &rd,
                                                         .chunk   = s_chunk,
                                                         .scratch = s_scratch,
                                                         .table   = bad_table};

  bcx_validate_arg_guards(&v);
  bcx_validate_reader_fields(&v);
  bcx_validate_reader_table(&v);
  bcx_validate_span_overflow(&v);

  TEST_END("book_chunked_validate_strict guards");
}

/** @brief Consume expected guard-path logs without touching host ITM MMIO. */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  test_book_chunked_open_happy();
  test_book_chunked_open_guards();
  test_book_chunked_read_happy();
  test_book_chunked_read_guards();
  test_book_chunked_validate_guards();
  return 0;
}

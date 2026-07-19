/**
 * @file test_ra8_book_chunked.c
 * @brief Tests for the demand-paged "RBKC" chunk reader (ra8_book_chunked.c).
 *
 * @details
 * Exercises `ra8_book_chunked_open()` / `ra8_book_chunked_read()` against an
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
 * equivalence test's concern (test_ra8_book_paged.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_book_chunked.h"
#include "ra8_err.h"
#include "unity_minimal.h"

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
 * @struct bcx_file_t
 * @brief An in-memory container "file" served through the read callback.
 */
typedef struct {
  const uint8_t* data;    /**< Container bytes.                           */
  uint64_t       len;     /**< Container length.                          */
  uint32_t       fail_at; /**< 1-based call index to fail on (0 = never). */
  uint32_t       calls;   /**< Calls served so far.                       */
} bcx_file_t;

/** @brief RBKC chunked-container magic (raw bytes; no string terminator). */
static const uint8_t k_bcx_magic_rbkc[] = {'R', 'B', 'K', 'C'};

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
  memcpy(buf, &f->data[offset], len);
  return k_ra8_ok;
}

/** @brief Static tinfl state (~11 KiB) kept off the test stack. */
static tinfl_decompressor s_tinfl;

/** @brief zlib inflater matching ra8_book_inflate_fn (mirrors the shelf inflater). */
static ra8_err_t
bcx_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
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
 * @brief Pack an RBKC container over ::s_blob with real zlib chunk streams.
 * @details Splits the blob into `chunk_bytes` slices, `mz_compress2`s each at
 *          the tool's compression level, and assembles header + table +
 *          streams into @p out. When @p short_last is true the final stream
 *          compresses only ::k_bcx_short_span of the last slice's bytes -- a
 *          *valid* stream that inflates to the wrong span, for the
 *          produced-length mismatch leg.
 * @return Packed container length in bytes.
 */
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
 * @return None.
 *
 * @pre Every out-parameter has room for ::k_bcx_chunk_count entries.
 * @pre ::s_blob holds ::k_bcx_blob_len bytes.
 * @post `offs[i+1] - offs[i]` is chunk i's compressed length.
 * @post Every compression reported MZ_OK.
 *
 * @note Not thread-safe (reads the shared ::s_blob).
 * @since 0.1.0
 */
static void bcx_compress_chunks(uint8_t streams[][k_bcx_staging_cap],
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
  size_t pos = 0U;
  memcpy(&out[pos], k_bcx_magic_rbkc, sizeof(k_bcx_magic_rbkc));
  pos += 4U;
  const uint32_t chunk_bytes = k_bcx_chunk_bytes;
  memcpy(&out[pos], &chunk_bytes, sizeof(chunk_bytes));
  pos += sizeof(chunk_bytes);
  const uint64_t total = k_bcx_blob_len;
  memcpy(&out[pos], &total, sizeof(total));
  pos += sizeof(total);
  memcpy(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  const uint32_t reserved = 0U;
  memcpy(&out[pos], &reserved, sizeof(reserved));
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
  memcpy(&out[pos], offs, sizeof(offs));
  pos += sizeof(offs);
  for (uint32_t i = 0U; i < count; ++i) {
    memcpy(&out[pos], streams[i], (size_t)stream_len[i]);
    pos += (size_t)stream_len[i];
  }
  return (uint64_t)pos;
}

/** @brief Open a freshly packed container into @p rd; returns the open result. */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t bcx_open(ra8_book_chunked_t* rd,
                          bcx_file_t*         file,
                          uint8_t*            container,
                          uint64_t            file_len,
                          uint64_t*           table_buf,
                          uint8_t*            staging)
// NOLINTEND(readability-non-const-parameter)
{
  *file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  return ra8_book_chunked_open(rd,
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
 * @test test_ra8_book_chunked_open_happy
 * @brief A well-formed container binds with the parsed geometry exposed.
 *
 * @par Targeted code:
 * `ra8_book_chunked_open`'s success path end-to-end: header read + parse,
 * `s_load_table`'s read, monotonic walk, staging-fit check, and the bound
 * `table` / `payload_off` publication.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the happy path is the all-false leg
 * of every open guard; the header-fields compound is covered by
 * test_mcdc_container_header_fields in test_ra8_book_cov.c)
 */
static void test_ra8_book_chunked_open_happy(void)
{
  TEST_BEGIN("ra8_book_chunked_open happy path");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  ra8_book_chunked_t rd                         = {};
  bcx_file_t         file                       = {};
  uint64_t           table_buf[k_bcx_table_cap] = {};
  static uint8_t     s_staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  TEST_ASSERT_EQ(k_bcx_chunk_bytes, rd.chunk_bytes);
  TEST_ASSERT_EQ(k_bcx_chunk_count, rd.chunk_count);
  TEST_ASSERT_EQ(k_bcx_blob_len, rd.inflated_total);
  TEST_ASSERT(rd.table == table_buf);
  TEST_ASSERT_EQ(k_ra8_book_container_header_len +
                   ((uint64_t)(k_bcx_chunk_count + 1U) * k_ra8_book_container_entry_len),
                 rd.payload_off);
  TEST_ASSERT_EQ(0U, rd.table[0]);

  TEST_END("ra8_book_chunked_open happy path");
}

/**
 * @struct bcx_guard_ctx_t
 * @brief Shared buffers and container image for the chunked open-guard helpers.
 * @invariant All pointers reference caller-owned storage; @c file_len is the
 *            packed length of @c container.
 * @see test_ra8_book_chunked_open_guards
 */
typedef struct {
  uint8_t*  container; /**< Packed container image under test.   */
  uint64_t  file_len;  /**< Byte length of the packed container. */
  uint64_t* table;     /**< Caller chunk-table buffer.           */
  uint8_t*  staging;   /**< Caller staging buffer.               */
} bcx_guard_ctx_t;

/**
 * @brief Assert one null `ra8_book_chunked_open` argument yields null_ptr.
 * @param[out] rd       Reader handle (nullptr exercises the rd guard).
 * @param[in]  read     File reader (nullptr exercises the file_read guard).
 * @param[in]  file     File context passed to @p read.
 * @param[in]  inflate  Decompressor (nullptr exercises the inflate guard).
 * @param[out] table    Chunk-table buffer (nullptr exercises the table guard).
 * @param[out] staging  Staging buffer (nullptr exercises the staging guard).
 * @param[in]  file_len Packed container length in bytes.
 * @return None.
 * @pre Exactly one pointer argument is nullptr.
 * @post `ra8_book_chunked_open` returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_expect_null_open(ra8_book_chunked_t* rd,
                                 ra8_vsource_read_fn read,
                                 void*               file,
                                 ra8_book_inflate_fn inflate,
                                 uint64_t*           table,
                                 uint8_t*            staging,
                                 uint64_t            file_len)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_chunked_open(rd,
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
 * @return None.
 * @pre @p g fields reference valid buffers.
 * @post `ra8_book_chunked_open` returned @p want.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_expect_open_err(ra8_err_t              want,
                                ra8_book_chunked_t*    rd,
                                bcx_file_t*            file,
                                const bcx_guard_ctx_t* g,
                                uint32_t               table_cap,
                                uint32_t               stage_cap)
{
  *file = (bcx_file_t){.data = g->container, .len = g->file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(want,
                 ra8_book_chunked_open(rd,
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
 * @brief Exercise the five null-pointer guards of `ra8_book_chunked_open`.
 * @param[out]    rd   Reader handle under test.
 * @param[in,out] file File context bound to the container image.
 * @param[in]     g    Shared buffers and container image.
 * @return None.
 * @pre @p g fields reference valid buffers.
 * @post Each guarded argument returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_open_guards_null(ra8_book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
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
 * @return None.
 * @pre @p g fields reference valid buffers.
 * @post Every malformed size / magic / budget returned its rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
bcx_open_guards_capacity(ra8_book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
{
  /* File shorter than the fixed header. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 bcx_open(rd, file, g->container, k_bcx_short_file, g->table, g->staging));
  TEST_ASSERT(rd->table == nullptr);

  /* Bad magic. */
  static uint8_t s_bad_magic[k_bcx_file_cap];
  memcpy(s_bad_magic, g->container, (size_t)g->file_len);
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
 * @return None.
 * @pre @p g fields reference valid buffers.
 * @post `ra8_book_chunked_open` returned k_ra8_err_hw_timeout.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
bcx_open_guards_readfail(ra8_book_chunked_t* rd, bcx_file_t* file, const bcx_guard_ctx_t* g)
{
  *file = (bcx_file_t){.data = g->container, .len = g->file_len, .fail_at = 1U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_book_chunked_open(rd,
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
 * @test test_ra8_book_chunked_open_guards
 * @brief Every open-path guard rejects its malformed input.
 *
 * @par Targeted code:
 * The five `RA8_CHECK_NULL_PTR` guards, the short-file return, the bad-magic
 * rejection (via `ra8_book_container_header_fields`), `s_load_table`'s
 * table-capacity return, its header-plus-table file-bound return, its
 * staging-capacity return, and the file-read error passthrough.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every open guard is an independent
 * single-condition check)
 */
static void test_ra8_book_chunked_open_guards(void)
{
  TEST_BEGIN("ra8_book_chunked_open guards");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  ra8_book_chunked_t rd                         = {};
  bcx_file_t         file                       = {};
  uint64_t           table_buf[k_bcx_table_cap] = {};
  static uint8_t     s_staging[k_bcx_staging_cap];
  file = (bcx_file_t){.data = s_container, .len = file_len, .fail_at = 0U, .calls = 0U};

  const bcx_guard_ctx_t g = {.container = s_container,
                             .file_len  = file_len,
                             .table     = table_buf,
                             .staging   = s_staging};

  bcx_open_guards_null(&rd, &file, &g);
  bcx_open_guards_capacity(&rd, &file, &g);
  bcx_open_guards_readfail(&rd, &file, &g);

  TEST_END("ra8_book_chunked_open guards");
}

/**
 * @test test_ra8_book_chunked_read_happy
 * @brief Sequential chunk-aligned reads reassemble the source byte-for-byte.
 *
 * @par Targeted code:
 * `ra8_book_chunked_read`'s full-chunk path twice and the short-final-chunk
 * clamp once; each read is one staged file read plus one real zlib inflate.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the read path is a chain of
 * independent single-condition checks)
 */
static void test_ra8_book_chunked_read_happy(void)
{
  TEST_BEGIN("ra8_book_chunked_read sequential reassembly");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  ra8_book_chunked_t rd                         = {};
  bcx_file_t         file                       = {};
  uint64_t           table_buf[k_bcx_table_cap] = {};
  static uint8_t     s_staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  static uint8_t s_out[k_bcx_blob_len];
  memset(s_out, 0, sizeof(s_out));
  for (uint32_t at = 0U; at < k_bcx_blob_len; at += k_bcx_chunk_bytes) {
    uint32_t span = k_bcx_blob_len - at;
    if (span > k_bcx_chunk_bytes) {
      span = k_bcx_chunk_bytes;
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chunked_read(&rd, at, &s_out[at], span));
  }
  TEST_ASSERT_EQ(0, memcmp(s_out, s_blob, k_bcx_blob_len));

  TEST_END("ra8_book_chunked_read sequential reassembly");
}

/**
 * @brief Exercise the null and unbound-reader guards of `ra8_book_chunked_read`.
 * @param[out] rd  A zero-initialised (unopened) reader handle.
 * @param[out] out Output buffer, at least k_bcx_chunk_bytes long.
 * @return None.
 * @pre @p rd is zero-initialised.
 * @post Both null guards and the unbound-state guard returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_read_guards_unbound(ra8_book_chunked_t* rd, uint8_t* out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_chunked_read(nullptr, 0U, out, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_chunked_read(rd, 0U, nullptr, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_book_chunked_read(rd, 0U, out, k_bcx_chunk_bytes));
}

/**
 * @brief Assert a stream that inflates to the wrong span is rejected.
 * @param[out] out     Output buffer, at least k_bcx_chunk_bytes long.
 * @param[in]  staging Shared staging buffer for the open.
 * @return None.
 * @pre The chunk blob fixtures are populated (bcx_fill_blob()).
 * @post `ra8_book_chunked_read` returned k_ra8_err_invalid_size.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bcx_read_guards_shortspan(uint8_t* out, uint8_t* staging)
{
  static uint8_t     s_short_container[k_bcx_file_cap];
  const uint64_t     short_len               = bcx_pack(s_short_container, true);
  ra8_book_chunked_t srd                     = {};
  bcx_file_t         sfile                   = {};
  uint64_t           stable[k_bcx_table_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&srd, &sfile, s_short_container, short_len, stable, staging));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_book_chunked_read(&srd, (uint64_t)2U * k_bcx_chunk_bytes, out, k_bcx_last_span));
}

/**
 * @test test_ra8_book_chunked_read_guards
 * @brief Every read-path guard rejects its malformed request.
 *
 * @par Targeted code:
 * `ra8_book_chunked_read`'s null guards, the unbound-reader return, the
 * past-end return, the alignment return, the wrong-span return, the file-read
 * error passthrough, the corrupt-stream inflater passthrough, and the
 * valid-stream-wrong-length return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every read guard is an independent
 * single-condition check)
 */
static void test_ra8_book_chunked_read_guards(void)
{
  TEST_BEGIN("ra8_book_chunked_read guards");
  bcx_fill_blob();
  static uint8_t s_container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(s_container, false);

  ra8_book_chunked_t rd                         = {};
  bcx_file_t         file                       = {};
  uint64_t           table_buf[k_bcx_table_cap] = {};
  static uint8_t     s_staging[k_bcx_staging_cap];
  static uint8_t     s_out[k_bcx_chunk_bytes];

  /* Null + unbound guards. */
  bcx_read_guards_unbound(&rd, s_out);

  TEST_ASSERT_EQ(k_ra8_ok, bcx_open(&rd, &file, s_container, file_len, table_buf, s_staging));

  /* Past the inflated total. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_book_chunked_read(&rd,
                                       (uint64_t)k_bcx_chunk_count * k_bcx_chunk_bytes,
                                       s_out,
                                       k_bcx_chunk_bytes));

  /* Not chunk-aligned. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_read(&rd, k_bcx_chunk_bytes / 2U, s_out, k_bcx_chunk_bytes));

  /* Wrong span: a full chunk asked short, the short final chunk asked full. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes - 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_book_chunked_read(&rd, (uint64_t)2U * k_bcx_chunk_bytes, s_out, k_bcx_chunk_bytes));

  /* File-read failure while staging the stream propagates verbatim. */
  file.fail_at = file.calls + 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes));
  file.fail_at = 0U;

  /* A corrupt stream fails in the inflater and propagates. */
  const uint64_t c0_at        = rd.payload_off + rd.table[0] + 4U;
  const uint8_t  saved        = s_container[c0_at];
  s_container[c0_at]          = (uint8_t)~saved;
  const ra8_err_t corrupt_err = ra8_book_chunked_read(&rd, 0U, s_out, k_bcx_chunk_bytes);
  s_container[c0_at]          = saved;
  TEST_ASSERT(corrupt_err != k_ra8_ok);

  /* A valid stream that inflates to the wrong span is rejected. */
  bcx_read_guards_shortspan(s_out, s_staging);

  TEST_END("ra8_book_chunked_read guards");
}

int main(void)
{
  test_ra8_book_chunked_open_happy();
  test_ra8_book_chunked_open_guards();
  test_ra8_book_chunked_read_happy();
  test_ra8_book_chunked_read_guards();
  return 0;
}

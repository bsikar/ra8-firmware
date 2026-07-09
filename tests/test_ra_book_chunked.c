/**
 * @file test_ra_book_chunked.c
 * @brief Tests for the demand-paged "RBKC" chunk reader (ra_book_chunked.c).
 *
 * @details
 * Exercises `ra_book_chunked_open()` / `ra_book_chunked_read()` against an
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
 * equivalence test's concern (test_ra_book_paged.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra_book_chunked.h"
#include "ra_err.h"
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

/** @brief ra_vsource_read_fn over an in-memory container, with fault injection. */
static ra_err_t bcx_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  bcx_file_t* f = (bcx_file_t*)ctx;
  f->calls++;
  if ((f->fail_at != 0U) && (f->calls == f->fail_at)) {
    return k_ra_err_hw_timeout;
  }
  if ((offset + (uint64_t)len) > f->len) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &f->data[offset], len);
  return k_ra_ok;
}

/** @brief Static tinfl state (~11 KiB) kept off the test stack. */
static tinfl_decompressor s_tinfl;

/** @brief zlib inflater matching ra_book_inflate_fn (mirrors the shelf inflater). */
static ra_err_t
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
    return k_ra_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra_ok;
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
static uint64_t bcx_pack(uint8_t* out, bool short_last)
{
  const uint32_t count                        = k_bcx_chunk_count;
  uint64_t       offs[k_bcx_chunk_count + 1U] = {};
  uint8_t        streams[k_bcx_chunk_count][k_bcx_staging_cap];
  mz_ulong       stream_len[k_bcx_chunk_count] = {};

  for (uint32_t i = 0U; i < count; ++i) {
    const uint32_t at   = i * k_bcx_chunk_bytes;
    uint32_t       span = k_bcx_blob_len - at;
    if (span > k_bcx_chunk_bytes) {
      span = k_bcx_chunk_bytes;
    }
    if (short_last && (i == (count - 1U))) {
      span = k_bcx_short_span;
    }
    stream_len[i] = (mz_ulong)sizeof(streams[i]);
    const int rc =
      mz_compress2(streams[i], &stream_len[i], &s_blob[at], (mz_ulong)span, MZ_BEST_COMPRESSION);
    TEST_ASSERT_EQ(MZ_OK, rc);
    offs[i + 1U] = offs[i] + (uint64_t)stream_len[i];
  }

  size_t pos = 0U;
  memcpy(&out[pos], "RBKC", 4U);
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
  memcpy(&out[pos], offs, sizeof(offs));
  pos += sizeof(offs);
  for (uint32_t i = 0U; i < count; ++i) {
    memcpy(&out[pos], streams[i], (size_t)stream_len[i]);
    pos += (size_t)stream_len[i];
  }
  return (uint64_t)pos;
}

/** @brief Open a freshly packed container into @p rd; returns the open result. */
static ra_err_t bcx_open(ra_book_chunked_t* rd,
                         bcx_file_t*        file,
                         uint8_t*           container,
                         uint64_t           file_len,
                         uint64_t*          table_buf,
                         uint8_t*           staging)
{
  *file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  return ra_book_chunked_open(rd,
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
 * @test test_ra_book_chunked_open_happy
 * @brief A well-formed container binds with the parsed geometry exposed.
 *
 * @par Targeted code:
 * `ra_book_chunked_open`'s success path end-to-end: header read + parse,
 * `s_load_table`'s read, monotonic walk, staging-fit check, and the bound
 * `table` / `payload_off` publication.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the happy path is the all-false leg
 * of every open guard; the header-fields compound is covered by
 * test_mcdc_container_header_fields in test_ra_book_cov.c)
 */
static void test_ra_book_chunked_open_happy(void)
{
  TEST_BEGIN("ra_book_chunked_open happy path");
  bcx_fill_blob();
  static uint8_t container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(container, false);

  ra_book_chunked_t rd                         = {};
  bcx_file_t        file                       = {};
  uint64_t          table_buf[k_bcx_table_cap] = {};
  static uint8_t    staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra_ok, bcx_open(&rd, &file, container, file_len, table_buf, staging));

  TEST_ASSERT_EQ(k_bcx_chunk_bytes, rd.chunk_bytes);
  TEST_ASSERT_EQ(k_bcx_chunk_count, rd.chunk_count);
  TEST_ASSERT_EQ(k_bcx_blob_len, rd.inflated_total);
  TEST_ASSERT(rd.table == table_buf);
  TEST_ASSERT_EQ(k_ra_book_container_header_len +
                   ((uint64_t)(k_bcx_chunk_count + 1U) * k_ra_book_container_entry_len),
                 rd.payload_off);
  TEST_ASSERT_EQ(0U, rd.table[0]);

  TEST_END("ra_book_chunked_open happy path");
}

/**
 * @test test_ra_book_chunked_open_guards
 * @brief Every open-path guard rejects its malformed input.
 *
 * @par Targeted code:
 * The five `RA_CHECK_NULL_PTR` guards, the short-file return, the bad-magic
 * rejection (via `ra_book_container_header_fields`), `s_load_table`'s
 * table-capacity return, its header-plus-table file-bound return, its
 * staging-capacity return, and the file-read error passthrough.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every open guard is an independent
 * single-condition check)
 */
static void test_ra_book_chunked_open_guards(void)
{
  TEST_BEGIN("ra_book_chunked_open guards");
  bcx_fill_blob();
  static uint8_t container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(container, false);

  ra_book_chunked_t rd                         = {};
  bcx_file_t        file                       = {};
  uint64_t          table_buf[k_bcx_table_cap] = {};
  static uint8_t    staging[k_bcx_staging_cap];
  file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};

  /* Null guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_book_chunked_open(nullptr,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_book_chunked_open(&rd,
                                      nullptr,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      nullptr,
                                      table_buf,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      nullptr,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_table_cap,
                                      nullptr,
                                      k_bcx_staging_cap));

  /* File shorter than the fixed header. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 bcx_open(&rd, &file, container, k_bcx_short_file, table_buf, staging));
  TEST_ASSERT(rd.table == nullptr);

  /* Bad magic. */
  static uint8_t bad_magic[k_bcx_file_cap];
  memcpy(bad_magic, container, (size_t)file_len);
  bad_magic[0] = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 bcx_open(&rd, &file, bad_magic, file_len, table_buf, staging));

  /* Table needs more entries than the caller budgeted. */
  file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_tiny_table,
                                      staging,
                                      k_bcx_staging_cap));

  /* File ends inside the chunk table. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 bcx_open(&rd, &file, container, k_bcx_table_cut, table_buf, staging));

  /* A compressed chunk larger than the staging budget. */
  file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_tiny_staging));

  /* File-read failure on the header read propagates verbatim. */
  file = (bcx_file_t){.data = container, .len = file_len, .fail_at = 1U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_book_chunked_open(&rd,
                                      bcx_file_read,
                                      &file,
                                      file_len,
                                      bcx_inflate,
                                      table_buf,
                                      k_bcx_table_cap,
                                      staging,
                                      k_bcx_staging_cap));

  TEST_END("ra_book_chunked_open guards");
}

/**
 * @test test_ra_book_chunked_read_happy
 * @brief Sequential chunk-aligned reads reassemble the source byte-for-byte.
 *
 * @par Targeted code:
 * `ra_book_chunked_read`'s full-chunk path twice and the short-final-chunk
 * clamp once; each read is one staged file read plus one real zlib inflate.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the read path is a chain of
 * independent single-condition checks)
 */
static void test_ra_book_chunked_read_happy(void)
{
  TEST_BEGIN("ra_book_chunked_read sequential reassembly");
  bcx_fill_blob();
  static uint8_t container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(container, false);

  ra_book_chunked_t rd                         = {};
  bcx_file_t        file                       = {};
  uint64_t          table_buf[k_bcx_table_cap] = {};
  static uint8_t    staging[k_bcx_staging_cap];
  TEST_ASSERT_EQ(k_ra_ok, bcx_open(&rd, &file, container, file_len, table_buf, staging));

  static uint8_t out[k_bcx_blob_len];
  memset(out, 0, sizeof(out));
  for (uint32_t at = 0U; at < k_bcx_blob_len; at += k_bcx_chunk_bytes) {
    uint32_t span = k_bcx_blob_len - at;
    if (span > k_bcx_chunk_bytes) {
      span = k_bcx_chunk_bytes;
    }
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chunked_read(&rd, at, &out[at], span));
  }
  TEST_ASSERT_EQ(0, memcmp(out, s_blob, k_bcx_blob_len));

  TEST_END("ra_book_chunked_read sequential reassembly");
}

/**
 * @test test_ra_book_chunked_read_guards
 * @brief Every read-path guard rejects its malformed request.
 *
 * @par Targeted code:
 * `ra_book_chunked_read`'s null guards, the unbound-reader return, the
 * past-end return, the alignment return, the wrong-span return, the file-read
 * error passthrough, the corrupt-stream inflater passthrough, and the
 * valid-stream-wrong-length return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every read guard is an independent
 * single-condition check)
 */
static void test_ra_book_chunked_read_guards(void)
{
  TEST_BEGIN("ra_book_chunked_read guards");
  bcx_fill_blob();
  static uint8_t container[k_bcx_file_cap];
  const uint64_t file_len = bcx_pack(container, false);

  ra_book_chunked_t rd                         = {};
  bcx_file_t        file                       = {};
  uint64_t          table_buf[k_bcx_table_cap] = {};
  static uint8_t    staging[k_bcx_staging_cap];
  static uint8_t    out[k_bcx_chunk_bytes];

  /* Null + unbound guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_chunked_read(nullptr, 0U, out, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_chunked_read(&rd, 0U, nullptr, k_bcx_chunk_bytes));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_book_chunked_read(&rd, 0U, out, k_bcx_chunk_bytes));

  TEST_ASSERT_EQ(k_ra_ok, bcx_open(&rd, &file, container, file_len, table_buf, staging));

  /* Past the inflated total. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_book_chunked_read(&rd,
                                      (uint64_t)k_bcx_chunk_count * k_bcx_chunk_bytes,
                                      out,
                                      k_bcx_chunk_bytes));

  /* Not chunk-aligned. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_book_chunked_read(&rd, k_bcx_chunk_bytes / 2U, out, k_bcx_chunk_bytes));

  /* Wrong span: a full chunk asked short, the short final chunk asked full. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_book_chunked_read(&rd, 0U, out, k_bcx_chunk_bytes - 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_book_chunked_read(&rd, 2U * k_bcx_chunk_bytes, out, k_bcx_chunk_bytes));

  /* File-read failure while staging the stream propagates verbatim. */
  file.fail_at = file.calls + 1U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_book_chunked_read(&rd, 0U, out, k_bcx_chunk_bytes));
  file.fail_at = 0U;

  /* A corrupt stream fails in the inflater and propagates. */
  const uint64_t c0_at       = rd.payload_off + rd.table[0] + 4U;
  const uint8_t  saved       = container[c0_at];
  container[c0_at]           = (uint8_t)~saved;
  const ra_err_t corrupt_err = ra_book_chunked_read(&rd, 0U, out, k_bcx_chunk_bytes);
  container[c0_at]           = saved;
  TEST_ASSERT(corrupt_err != k_ra_ok);

  /* A valid stream that inflates to the wrong span is rejected. */
  static uint8_t    short_container[k_bcx_file_cap];
  const uint64_t    short_len               = bcx_pack(short_container, true);
  ra_book_chunked_t srd                     = {};
  bcx_file_t        sfile                   = {};
  uint64_t          stable[k_bcx_table_cap] = {};
  TEST_ASSERT_EQ(k_ra_ok, bcx_open(&srd, &sfile, short_container, short_len, stable, staging));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_chunked_read(&srd, 2U * k_bcx_chunk_bytes, out, k_bcx_last_span));

  TEST_END("ra_book_chunked_read guards");
}

int main(void)
{
  test_ra_book_chunked_open_happy();
  test_ra_book_chunked_open_guards();
  test_ra_book_chunked_read_happy();
  test_ra_book_chunked_read_guards();
  return 0;
}

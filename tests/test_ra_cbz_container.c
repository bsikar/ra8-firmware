/**
 * @file test_ra_cbz_container.c
 * @brief Tests for the per-page "RCBZ" comic-container reader (ra_cbz_container.c).
 *
 * @details
 * Exercises `ra_cbz_open()` / `ra_cbz_page_info()` / `ra_cbz_page_bind()` /
 * `ra_cbz_page_read()` against an in-memory container built with *real* zlib
 * streams (miniz `mz_compress2`, the same RFC 1950 wrapping the host producer
 * `tools/epub_compile/cbz_container.py` emits) and inflated with a tinfl
 * wrapper shaped like the shipping shelf inflater. The three fixture pages have
 * distinct sizes so the variable-length manifest is stressed.
 *
 * The acceptance bar for #209 is the equivalence test: reading each page a page
 * at a time through the container -- both directly and through the
 * `ra_vsource_add_paged` + `ra_vsource_loader` seam the issue names -- yields
 * bytes identical to the source page rasters (paged == whole-file). The rest
 * covers the open path's header / table / capacity guards, the manifest
 * accessors, and every read-path error leg (unbound, bad offset, wrong span,
 * corrupt stream, wrong-length inflate, file-read passthrough).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniz.h"
#include "ra_book.h"
#include "ra_cbz_container.h"
#include "ra_err.h"
#include "ra_vsource.h"
#include "unity_minimal.h"

/**
 * @enum ccx_dim_t
 * @brief Fixture geometry, page raster sizes, and buffer budgets.
 */
typedef enum : uint32_t {
  k_ccx_page_count  = 3U,    /**< Pages in every fixture container.          */
  k_ccx_p0_w        = 10U,   /**< Page 0 width.                              */
  k_ccx_p0_h        = 20U,   /**< Page 0 height.                             */
  k_ccx_p0_raw      = 100U,  /**< Page 0 raster bytes ((10*20+1)/2).         */
  k_ccx_p1_w        = 25U,   /**< Page 1 width.                              */
  k_ccx_p1_h        = 20U,   /**< Page 1 height.                             */
  k_ccx_p1_raw      = 250U,  /**< Page 1 raster bytes ((25*20+1)/2).         */
  k_ccx_p2_w        = 12U,   /**< Page 2 width.                              */
  k_ccx_p2_h        = 10U,   /**< Page 2 height.                             */
  k_ccx_p2_raw      = 60U,   /**< Page 2 raster bytes ((12*10+1)/2).         */
  k_ccx_max_raw     = 250U,  /**< Largest page raster (page 1).              */
  k_ccx_file_cap    = 4096U, /**< Container build-buffer budget.             */
  k_ccx_offsets_cap = 8U,    /**< Offset-table entry budget (needs 4).       */
  k_ccx_meta_cap    = 64U,   /**< Metadata-table byte budget (needs 36).     */
  k_ccx_staging_cap = 512U,  /**< Compressed-page staging budget.            */
  k_ccx_stream_cap  = 512U,  /**< Per-page compressed-stream scratch budget. */
  k_ccx_tiny_off    = 3U,    /**< One offset entry short of the needed 4.    */
  k_ccx_tiny_meta   = 8U,    /**< Fewer meta bytes than the needed 36.       */
  k_ccx_tiny_stage  = 8U,    /**< Smaller than any real compressed page.     */
  k_ccx_short_file  = 10U,   /**< File length below the fixed header.        */
  k_ccx_table_cut   = 24U,   /**< File length inside the offset table.       */
  k_ccx_frame_bytes = 256U,  /**< ra_vsource frame >= k_ccx_max_raw.         */
  k_ccx_pat_mul0    = 7U,    /**< Page 0 byte-pattern multiplier.            */
  k_ccx_pat_add0    = 1U,    /**< Page 0 byte-pattern offset.                */
  k_ccx_pat_mul1    = 13U,   /**< Page 1 byte-pattern multiplier.            */
  k_ccx_pat_add1    = 3U,    /**< Page 1 byte-pattern offset.                */
  k_ccx_pat_mul2    = 29U,   /**< Page 2 byte-pattern multiplier.            */
  k_ccx_pat_add2    = 5U,    /**< Page 2 byte-pattern offset.                */
} ccx_dim_t;

/**
 * @struct ccx_page_t
 * @brief One fixture page: its source raster plus its manifest metadata.
 */
typedef struct {
  const uint8_t* raw;      /**< Source raster bytes.    */
  uint32_t       raw_size; /**< Raster length in bytes. */
  uint16_t       width;    /**< Manifest pixel width.   */
  uint16_t       height;   /**< Manifest pixel height.  */
  uint8_t        format;   /**< Manifest image format.  */
} ccx_page_t;

/** @brief Page 0 source raster. */
static uint8_t s_p0[k_ccx_p0_raw];
/** @brief Page 1 source raster. */
static uint8_t s_p1[k_ccx_p1_raw];
/** @brief Page 2 source raster. */
static uint8_t s_p2[k_ccx_p2_raw];
/** @brief The three fixture pages, in reading order. */
static ccx_page_t s_pages[k_ccx_page_count];

/** @brief Fill the source rasters with distinct deterministic byte patterns. */
static void ccx_fill_pages(void)
{
  for (uint32_t i = 0U; i < k_ccx_p0_raw; ++i) {
    s_p0[i] = (uint8_t)((i * k_ccx_pat_mul0) + k_ccx_pat_add0);
  }
  for (uint32_t i = 0U; i < k_ccx_p1_raw; ++i) {
    s_p1[i] = (uint8_t)((i * k_ccx_pat_mul1) + k_ccx_pat_add1);
  }
  for (uint32_t i = 0U; i < k_ccx_p2_raw; ++i) {
    s_p2[i] = (uint8_t)((i * k_ccx_pat_mul2) + k_ccx_pat_add2);
  }
  s_pages[0] = (ccx_page_t){s_p0, k_ccx_p0_raw, k_ccx_p0_w, k_ccx_p0_h, k_ra_book_image_gray4};
  s_pages[1] = (ccx_page_t){s_p1, k_ccx_p1_raw, k_ccx_p1_w, k_ccx_p1_h, k_ra_book_image_gray4};
  s_pages[2] = (ccx_page_t){s_p2, k_ccx_p2_raw, k_ccx_p2_w, k_ccx_p2_h, k_ra_book_image_gray4};
}

/**
 * @struct ccx_file_t
 * @brief An in-memory container "file" served through the read callback.
 */
typedef struct {
  const uint8_t* data;    /**< Container bytes.                           */
  uint64_t       len;     /**< Container length.                          */
  uint32_t       fail_at; /**< 1-based call index to fail on (0 = never). */
  uint32_t       calls;   /**< Calls served so far.                       */
} ccx_file_t;

/** @brief ra_vsource_read_fn over an in-memory container, with fault injection. */
static ra_err_t ccx_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  ccx_file_t* f = (ccx_file_t*)ctx;
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
ccx_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
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

/**
 * @brief Pack an RCBZ container over ::s_pages with real zlib page streams.
 * @details Compresses each page raster with `mz_compress2`, then assembles the
 *          header + offset table + metadata table + streams into @p out.
 *          @p flags is stamped into the header. When @p raw_delta is non-zero it
 *          is added to the last page's stored `raw_size`, producing a *valid*
 *          stream that inflates to fewer bytes than the manifest claims -- the
 *          produced-length-mismatch leg.
 * @param[out] out       Destination buffer (>= ::k_ccx_file_cap bytes).
 * @param[in]  flags     Feature-flag word to stamp into the header.
 * @param[in]  raw_delta Amount to inflate the last page's stored raw_size by.
 * @return Packed container length in bytes.
 */
static uint64_t ccx_pack(uint8_t* out, uint32_t flags, uint32_t raw_delta)
{
  const uint32_t count                       = k_ccx_page_count;
  uint64_t       offs[k_ccx_page_count + 1U] = {};
  uint8_t        streams[k_ccx_page_count][k_ccx_stream_cap];
  mz_ulong       slen[k_ccx_page_count] = {};

  for (uint32_t i = 0U; i < count; ++i) {
    slen[i]      = (mz_ulong)sizeof(streams[i]);
    const int rc = mz_compress2(streams[i],
                                &slen[i],
                                s_pages[i].raw,
                                (mz_ulong)s_pages[i].raw_size,
                                MZ_BEST_COMPRESSION);
    TEST_ASSERT_EQ(MZ_OK, rc);
    offs[i + 1U] = offs[i] + (uint64_t)slen[i];
  }

  size_t pos = 0U;
  memcpy(&out[pos], "RCBZ", (size_t)k_ra_cbz_magic_len);
  pos += (size_t)k_ra_cbz_magic_len;
  memcpy(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  memcpy(&out[pos], &flags, sizeof(flags));
  pos += sizeof(flags);
  const uint32_t reserved = 0U;
  memcpy(&out[pos], &reserved, sizeof(reserved));
  pos += sizeof(reserved);

  memcpy(&out[pos], offs, sizeof(offs));
  pos += sizeof(offs);

  for (uint32_t i = 0U; i < count; ++i) {
    uint32_t raw_size = s_pages[i].raw_size;
    if (i == (count - 1U)) {
      raw_size += raw_delta;
    }
    memcpy(&out[pos + k_ra_cbz_meta_off_raw], &raw_size, sizeof(raw_size));
    memcpy(&out[pos + k_ra_cbz_meta_off_width], &s_pages[i].width, sizeof(s_pages[i].width));
    memcpy(&out[pos + k_ra_cbz_meta_off_height], &s_pages[i].height, sizeof(s_pages[i].height));
    out[pos + k_ra_cbz_meta_off_format]      = s_pages[i].format;
    out[pos + k_ra_cbz_meta_off_format + 1U] = 0U;
    out[pos + k_ra_cbz_meta_off_format + 2U] = 0U;
    out[pos + k_ra_cbz_meta_off_format + 3U] = 0U;
    pos += (size_t)k_ra_cbz_meta_len;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    memcpy(&out[pos], streams[i], (size_t)slen[i]);
    pos += (size_t)slen[i];
  }
  return (uint64_t)pos;
}

/** @brief Open a freshly packed container into @p cbz; returns the open result. */
static ra_err_t ccx_open(ra_cbz_t*   cbz,
                         ccx_file_t* file,
                         uint8_t*    container,
                         uint64_t    file_len,
                         uint64_t*   offsets_buf,
                         uint8_t*    meta_buf,
                         uint8_t*    staging)
{
  *file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  return ra_cbz_open(cbz,
                     ccx_file_read,
                     file,
                     file_len,
                     ccx_inflate,
                     offsets_buf,
                     k_ccx_offsets_cap,
                     meta_buf,
                     k_ccx_meta_cap,
                     staging,
                     k_ccx_staging_cap);
}

/**
 * @test test_ra_cbz_open_happy
 * @brief A well-formed container binds with the parsed geometry exposed.
 *
 * @par Targeted code:
 * `ra_cbz_open`'s success path end-to-end: header read + parse, both table
 * reads, offset monotonic walk, staging-fit + raster-length checks, and the
 * bound `offsets` / `meta` / `payload_off` publication; plus the manifest
 * accessors `ra_cbz_page_count` / `ra_cbz_is_rtl`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every guard in the open path is an
 * independent single-condition check)
 */
static void test_ra_cbz_open_happy(void)
{
  TEST_BEGIN("ra_cbz_open happy path");
  ccx_fill_pages();
  static uint8_t container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(container, 0U, 0U);

  ra_cbz_t       cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t meta_buf[k_ccx_meta_cap];
  static uint8_t staging[k_ccx_staging_cap];
  TEST_ASSERT_EQ(k_ra_ok,
                 ccx_open(&cbz, &file, container, file_len, offsets_buf, meta_buf, staging));

  TEST_ASSERT_EQ(k_ccx_page_count, cbz.page_count);
  TEST_ASSERT_EQ(k_ccx_page_count, ra_cbz_page_count(&cbz));
  TEST_ASSERT(!ra_cbz_is_rtl(&cbz));
  TEST_ASSERT(cbz.offsets == offsets_buf);
  TEST_ASSERT(cbz.meta == meta_buf);
  const uint64_t want_payload = (uint64_t)k_ra_cbz_header_len +
                                ((uint64_t)(k_ccx_page_count + 1U) * k_ra_cbz_offset_len) +
                                ((uint64_t)k_ccx_page_count * k_ra_cbz_meta_len);
  TEST_ASSERT_EQ(want_payload, cbz.payload_off);
  TEST_ASSERT_EQ(0U, cbz.offsets[0]);

  /* RTL container: exactly the RTL flag bit is decoded. */
  static uint8_t rtl_container[k_ccx_file_cap];
  const uint64_t rtl_len  = ccx_pack(rtl_container, (uint32_t)k_ra_book_flag_rtl, 0U);
  ra_cbz_t       rtl_cbz  = {};
  ccx_file_t     rtl_file = {};
  uint64_t       rtl_off[k_ccx_offsets_cap] = {};
  static uint8_t rtl_meta[k_ccx_meta_cap];
  static uint8_t rtl_stage[k_ccx_staging_cap];
  TEST_ASSERT_EQ(
    k_ra_ok,
    ccx_open(&rtl_cbz, &rtl_file, rtl_container, rtl_len, rtl_off, rtl_meta, rtl_stage));
  TEST_ASSERT(ra_cbz_is_rtl(&rtl_cbz));

  TEST_END("ra_cbz_open happy path");
}

/**
 * @test test_ra_cbz_open_guards
 * @brief Every open-path guard rejects its malformed input.
 *
 * @par Targeted code:
 * The six `RA_CHECK_NULL_PTR` guards, the short-file return, the bad-magic /
 * zero-page-count / non-zero-reserved / unknown-flag rejections in
 * `s_parse_header`, `s_load_tables`' offset-cap / meta-cap / file-bound
 * returns, the staging-capacity return, a corrupt offset table, a zero raster
 * length, and the two file-read error passthroughs (header + meta reads).
 *
 * @par MC/DC:
 * (no compound decisions under test -- every open guard is an independent
 * single-condition check)
 */
static void test_ra_cbz_open_guards(void)
{
  TEST_BEGIN("ra_cbz_open guards");
  ccx_fill_pages();
  static uint8_t container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(container, 0U, 0U);

  ra_cbz_t       cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t meta_buf[k_ccx_meta_cap];
  static uint8_t staging[k_ccx_staging_cap];
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};

  /* Null guards (cbz, file_read, inflate, offsets_buf, meta_buf, staging). */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(nullptr,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(&cbz,
                             nullptr,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             nullptr,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             nullptr,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             nullptr,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             nullptr,
                             k_ccx_staging_cap));

  /* File shorter than the fixed header. */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ccx_open(&cbz, &file, container, k_ccx_short_file, offsets_buf, meta_buf, staging));
  TEST_ASSERT(cbz.offsets == nullptr);

  /* Bad magic. */
  static uint8_t bad_magic[k_ccx_file_cap];
  memcpy(bad_magic, container, (size_t)file_len);
  bad_magic[0] = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_magic, file_len, offsets_buf, meta_buf, staging));

  /* Zero page count. */
  static uint8_t zero_pages[k_ccx_file_cap];
  memcpy(zero_pages, container, (size_t)file_len);
  const uint32_t zero = 0U;
  memcpy(&zero_pages[k_ra_cbz_hdr_off_page_count], &zero, sizeof(zero));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, zero_pages, file_len, offsets_buf, meta_buf, staging));

  /* Non-zero reserved word. */
  static uint8_t bad_rsv[k_ccx_file_cap];
  memcpy(bad_rsv, container, (size_t)file_len);
  const uint32_t one = 1U;
  memcpy(&bad_rsv[k_ra_cbz_hdr_off_reserved], &one, sizeof(one));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_rsv, file_len, offsets_buf, meta_buf, staging));

  /* Unknown feature-flag bit. */
  static uint8_t bad_flag[k_ccx_file_cap];
  memcpy(bad_flag, container, (size_t)file_len);
  const uint32_t unknown_flag = (uint32_t)k_ra_book_flag_mask_known + 1U;
  memcpy(&bad_flag[k_ra_cbz_hdr_off_flags], &unknown_flag, sizeof(unknown_flag));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_flag, file_len, offsets_buf, meta_buf, staging));

  /* Offset table needs more entries than the caller budgeted. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_tiny_off,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));

  /* Metadata table needs more bytes than the caller budgeted. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_tiny_meta,
                             staging,
                             k_ccx_staging_cap));

  /* File ends inside the offset table. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ccx_open(&cbz, &file, container, k_ccx_table_cut, offsets_buf, meta_buf, staging));

  /* A compressed page larger than the staging budget. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_tiny_stage));

  /* Corrupt offset table: force a non-monotonic entry (offset[1] == offset[0]). */
  static uint8_t bad_offs[k_ccx_file_cap];
  memcpy(bad_offs, container, (size_t)file_len);
  const uint64_t zero64 = 0U;
  memcpy(&bad_offs[k_ra_cbz_header_len + k_ra_cbz_offset_len], &zero64, sizeof(zero64));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_offs, file_len, offsets_buf, meta_buf, staging));

  /* Offset table whose first entry is not zero. */
  static uint8_t bad_off0[k_ccx_file_cap];
  memcpy(bad_off0, container, (size_t)file_len);
  const uint64_t one64 = 1U;
  memcpy(&bad_off0[k_ra_cbz_header_len], &one64, sizeof(one64));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_off0, file_len, offsets_buf, meta_buf, staging));

  /* File one byte short: the offset table's end disagrees with the payload. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, container, file_len - 1U, offsets_buf, meta_buf, staging));

  /* Zero raster length in the metadata table (page 0). */
  static uint8_t bad_raw[k_ccx_file_cap];
  memcpy(bad_raw, container, (size_t)file_len);
  const size_t meta0 =
    (size_t)k_ra_cbz_header_len + ((size_t)(k_ccx_page_count + 1U) * k_ra_cbz_offset_len);
  memcpy(&bad_raw[meta0 + k_ra_cbz_meta_off_raw], &zero, sizeof(zero));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ccx_open(&cbz, &file, bad_raw, file_len, offsets_buf, meta_buf, staging));

  /* File-read failure on the header read propagates verbatim. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 1U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));

  /* File-read failure on the offset-table read (call 2) propagates verbatim. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 2U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));

  /* File-read failure on the metadata read (call 3) propagates verbatim. */
  file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 3U, .calls = 0U};
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_cbz_open(&cbz,
                             ccx_file_read,
                             &file,
                             file_len,
                             ccx_inflate,
                             offsets_buf,
                             k_ccx_offsets_cap,
                             meta_buf,
                             k_ccx_meta_cap,
                             staging,
                             k_ccx_staging_cap));

  TEST_END("ra_cbz_open guards");
}

/**
 * @test test_ra_cbz_page_info
 * @brief The manifest exposes each page's dimensions, format and raster length.
 *
 * @par Targeted code:
 * `ra_cbz_page_info` happy path over all three pages plus its null / unbound /
 * out-of-range guards, and the unbound / null / out-of-range accessors on the
 * inline `ra_cbz_page_count` / `ra_cbz_is_rtl`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each info guard is an independent
 * single-condition check)
 */
static void test_ra_cbz_page_info(void)
{
  TEST_BEGIN("ra_cbz_page_info manifest");
  ccx_fill_pages();
  static uint8_t container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(container, 0U, 0U);

  ra_cbz_t       cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t meta_buf[k_ccx_meta_cap];
  static uint8_t staging[k_ccx_staging_cap];

  /* Unbound / null accessors return their safe defaults. */
  TEST_ASSERT_EQ(0U, ra_cbz_page_count(nullptr));
  TEST_ASSERT_EQ(0U, ra_cbz_page_count(&cbz));
  TEST_ASSERT(!ra_cbz_is_rtl(nullptr));
  TEST_ASSERT(!ra_cbz_is_rtl(&cbz));

  uint16_t w   = 0U;
  uint16_t h   = 0U;
  uint8_t  fmt = 0U;
  uint32_t raw = 0U;
  /* Unbound-container guard (meta == NULL). */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_cbz_page_info(&cbz, 0U, &w, &h, &fmt, &raw));

  TEST_ASSERT_EQ(k_ra_ok,
                 ccx_open(&cbz, &file, container, file_len, offsets_buf, meta_buf, staging));

  for (uint32_t p = 0U; p < k_ccx_page_count; ++p) {
    TEST_ASSERT_EQ(k_ra_ok, ra_cbz_page_info(&cbz, p, &w, &h, &fmt, &raw));
    TEST_ASSERT_EQ(s_pages[p].width, w);
    TEST_ASSERT_EQ(s_pages[p].height, h);
    TEST_ASSERT_EQ(s_pages[p].format, fmt);
    TEST_ASSERT_EQ(s_pages[p].raw_size, raw);
  }

  /* Null-argument guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_info(nullptr, 0U, &w, &h, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_info(&cbz, 0U, nullptr, &h, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_info(&cbz, 0U, &w, nullptr, &fmt, &raw));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_info(&cbz, 0U, &w, &h, nullptr, &raw));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_info(&cbz, 0U, &w, &h, &fmt, nullptr));
  /* Out-of-range page. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_cbz_page_info(&cbz, k_ccx_page_count, &w, &h, &fmt, &raw));

  TEST_END("ra_cbz_page_info manifest");
}

/**
 * @brief Read one page's raster through the ra_vsource paged seam.
 * @details Registers the bound cursor as a paged object and drives one
 *          ::ra_vsource_loader fill (frame >= page raster), exactly as the #147
 *          cache would on a miss; copies the in-range prefix into @p out.
 * @param[in]  cur   Bound page cursor.
 * @param[out] out   Destination for the page raster (>= cur->raw_size bytes).
 * @return ra_err_t The loader result.
 */
static ra_err_t ccx_read_via_vsource(ra_cbz_page_t* cur, uint8_t* out)
{
  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs  = {};
  ra_err_t         err = ra_vsource_init(&vs, objs, 1U);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t oid = 0U;
  err = ra_vsource_add_paged(&vs, ra_cbz_page_read, cur, 0U, (uint64_t)cur->raw_size, &oid);
  if (err != k_ra_ok) {
    return err;
  }
  static uint8_t frame[k_ccx_frame_bytes];
  err = ra_vsource_loader(&vs, oid, 0U, frame, (uint32_t)k_ccx_frame_bytes);
  if (err != k_ra_ok) {
    return err;
  }
  memcpy(out, frame, (size_t)cur->raw_size);
  return k_ra_ok;
}

/**
 * @test test_ra_cbz_page_read_equivalence
 * @brief Per-page paged reads reconstruct the source rasters byte-for-byte.
 *
 * @par Targeted code:
 * The #209 acceptance: `ra_cbz_page_bind` + `ra_cbz_page_read` for every page,
 * read two ways -- directly, and through `ra_vsource_add_paged` +
 * `ra_vsource_loader` (the seam the issue names) -- each byte-identical to the
 * packed source raster. This is the multi-GB per-page read path in miniature.
 *
 * @par MC/DC:
 * (no compound decisions under test -- end-to-end byte-identity through the
 * per-page reader; every read guard is an independent single-condition check)
 */
static void test_ra_cbz_page_read_equivalence(void)
{
  TEST_BEGIN("ra_cbz_page_read paged == whole-file");
  ccx_fill_pages();
  static uint8_t container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(container, (uint32_t)k_ra_book_flag_rtl, 0U);

  ra_cbz_t       cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t meta_buf[k_ccx_meta_cap];
  static uint8_t staging[k_ccx_staging_cap];
  TEST_ASSERT_EQ(k_ra_ok,
                 ccx_open(&cbz, &file, container, file_len, offsets_buf, meta_buf, staging));
  TEST_ASSERT(ra_cbz_is_rtl(&cbz));

  static uint8_t direct[k_ccx_max_raw];
  static uint8_t viasrc[k_ccx_max_raw];
  for (uint32_t p = 0U; p < k_ccx_page_count; ++p) {
    ra_cbz_page_t cur = {};
    TEST_ASSERT_EQ(k_ra_ok, ra_cbz_page_bind(&cbz, p, &cur));
    TEST_ASSERT_EQ(s_pages[p].raw_size, cur.raw_size);

    /* (A) direct paged read. */
    memset(direct, 0, sizeof(direct));
    TEST_ASSERT_EQ(k_ra_ok, ra_cbz_page_read(&cur, 0U, direct, cur.raw_size));
    TEST_ASSERT_EQ(0, memcmp(direct, s_pages[p].raw, s_pages[p].raw_size));

    /* (B) through the ra_vsource paged seam. */
    memset(viasrc, 0, sizeof(viasrc));
    TEST_ASSERT_EQ(k_ra_ok, ccx_read_via_vsource(&cur, viasrc));
    TEST_ASSERT_EQ(0, memcmp(viasrc, s_pages[p].raw, s_pages[p].raw_size));
  }

  TEST_END("ra_cbz_page_read paged == whole-file");
}

/**
 * @test test_ra_cbz_page_read_guards
 * @brief Every bind / read guard rejects its malformed request.
 *
 * @par Targeted code:
 * `ra_cbz_page_bind`'s null / unbound / out-of-range guards, and
 * `ra_cbz_page_read`'s null / unbound-cursor / unbound-container / bad-offset /
 * wrong-span guards, the file-read passthrough, the corrupt-stream inflater
 * passthrough, and the valid-stream-wrong-length return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every bind / read guard is an
 * independent single-condition check)
 */
static void test_ra_cbz_page_read_guards(void)
{
  TEST_BEGIN("ra_cbz_page_read guards");
  ccx_fill_pages();
  static uint8_t container[k_ccx_file_cap];
  const uint64_t file_len = ccx_pack(container, 0U, 0U);

  ra_cbz_t       cbz                            = {};
  ccx_file_t     file                           = {};
  uint64_t       offsets_buf[k_ccx_offsets_cap] = {};
  static uint8_t meta_buf[k_ccx_meta_cap];
  static uint8_t staging[k_ccx_staging_cap];
  static uint8_t out[k_ccx_max_raw];

  /* Bind guards before a valid open. */
  ra_cbz_page_t cur = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_bind(nullptr, 0U, &cur));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_bind(&cbz, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_cbz_page_bind(&cbz, 0U, &cur));

  /* Read guards on an unbound cursor. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_read(nullptr, 0U, out, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cbz_page_read(&cur, 0U, nullptr, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_cbz_page_read(&cur, 0U, out, k_ccx_p0_raw));

  /* Cursor whose container was never bound (offsets == NULL) -> invalid_state. */
  ra_cbz_t      unbound_cbz = {};
  ra_cbz_page_t orphan_cur  = {.cbz = &unbound_cbz, .page = 0U, .raw_size = k_ccx_p0_raw};
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_cbz_page_read(&orphan_cur, 0U, out, k_ccx_p0_raw));

  TEST_ASSERT_EQ(k_ra_ok,
                 ccx_open(&cbz, &file, container, file_len, offsets_buf, meta_buf, staging));

  /* Out-of-range bind. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_cbz_page_bind(&cbz, k_ccx_page_count, &cur));

  TEST_ASSERT_EQ(k_ra_ok, ra_cbz_page_bind(&cbz, 0U, &cur));

  /* Non-zero offset and wrong span are rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_cbz_page_read(&cur, 1U, out, k_ccx_p0_raw));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_cbz_page_read(&cur, 0U, out, k_ccx_p0_raw - 1U));

  /* File-read failure while staging the stream propagates verbatim. */
  file.fail_at = file.calls + 1U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_cbz_page_read(&cur, 0U, out, k_ccx_p0_raw));
  file.fail_at = 0U;

  /* A corrupt stream fails in the inflater and propagates. */
  const uint64_t p0_at   = cbz.payload_off + cbz.offsets[0] + 4U;
  const uint8_t  saved   = container[p0_at];
  container[p0_at]       = (uint8_t)~saved;
  const ra_err_t corrupt = ra_cbz_page_read(&cur, 0U, out, k_ccx_p0_raw);
  container[p0_at]       = saved;
  TEST_ASSERT(corrupt != k_ra_ok);

  /* A valid stream whose manifest raster length is too long is rejected: the
   * last page's stored raw_size is inflated by one, so the produced length no
   * longer matches. */
  static uint8_t badraw_container[k_ccx_file_cap];
  const uint64_t badraw_len              = ccx_pack(badraw_container, 0U, 1U);
  ra_cbz_t       brd                     = {};
  ccx_file_t     bfile                   = {};
  uint64_t       boff[k_ccx_offsets_cap] = {};
  static uint8_t bmeta[k_ccx_meta_cap];
  static uint8_t bstage[k_ccx_staging_cap];
  static uint8_t bout[k_ccx_max_raw + 1U];
  TEST_ASSERT_EQ(k_ra_ok,
                 ccx_open(&brd, &bfile, badraw_container, badraw_len, boff, bmeta, bstage));
  ra_cbz_page_t bcur = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_cbz_page_bind(&brd, k_ccx_page_count - 1U, &bcur));
  TEST_ASSERT_EQ(k_ccx_p2_raw + 1U, bcur.raw_size);
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_cbz_page_read(&bcur, 0U, bout, bcur.raw_size));

  TEST_END("ra_cbz_page_read guards");
}

int32_t main(void)
{
  test_ra_cbz_open_happy();
  test_ra_cbz_open_guards();
  test_ra_cbz_page_info();
  test_ra_cbz_page_read_equivalence();
  test_ra_cbz_page_read_guards();
  (void)fprintf(stderr, "[OK ] test_ra_cbz_container.c\n");
  return 0;
}

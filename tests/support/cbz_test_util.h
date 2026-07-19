/**
 * @file cbz_test_util.h
 * @brief Shared fixture for the test_ra8_cbz_container suite: the three
 *        distinct-size fixture pages, the in-memory container read callback
 *        (with fault injection), the tinfl zlib inflater, the `ccx_pack`
 *        RCBZ builder over real `mz_compress2` streams, and the `ccx_open`
 *        helper.
 *
 * @details Header-only (all definitions `static`) so the test binary carries
 * its own private copy of the fixture state; the tests/CMakeLists.txt auto-glob
 * stays free of non-test .c files. Extracted from test_ra8_cbz_container.c to
 * keep that translation unit under the per-file maintainability cap once the
 * oversized guard tests were decomposed into helpers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_book.h"
#include "ra8_cbz_container.h"
#include "ra8_err.h"
#include "ra8_vsource.h"
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
  k_ccx_frame_bytes = 256U,  /**< ra8_vsource frame >= k_ccx_max_raw.        */
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
static inline void ccx_fill_pages(void)
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
  s_pages[0] = (ccx_page_t){s_p0, k_ccx_p0_raw, k_ccx_p0_w, k_ccx_p0_h, k_ra8_book_image_gray4};
  s_pages[1] = (ccx_page_t){s_p1, k_ccx_p1_raw, k_ccx_p1_w, k_ccx_p1_h, k_ra8_book_image_gray4};
  s_pages[2] = (ccx_page_t){s_p2, k_ccx_p2_raw, k_ccx_p2_w, k_ccx_p2_h, k_ra8_book_image_gray4};
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

/** @brief ra8_vsource_read_fn over an in-memory container, with fault injection. */
static inline ra8_err_t ccx_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  ccx_file_t* f = (ccx_file_t*)ctx;
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
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

/**
 * @brief Write the RCBZ container header; return the offset just past it.
 *
 * @param[out] out   Container buffer.
 * @param[in]  count Page count to record.
 * @param[in]  flags Container flags word to record.
 *
 * @return Byte offset of the first field after the header.
 *
 * @pre @p out has room for the fixed header.
 * @pre @p count matches the number of pages that follow.
 * @post The magic, count, flags and reserved words are written in order.
 * @post The reserved word is zero, as the reader expects.
 *
 * @note Not thread-safe with respect to @p out.
 */
static inline size_t ccx_write_header(uint8_t* out, uint32_t count, uint32_t flags)
{
  size_t pos = 0U;
  memcpy(&out[pos], "RCBZ", (size_t)k_ra8_cbz_magic_len);
  pos += (size_t)k_ra8_cbz_magic_len;
  memcpy(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  memcpy(&out[pos], &flags, sizeof(flags));
  pos += sizeof(flags);
  const uint32_t reserved = 0U;
  memcpy(&out[pos], &reserved, sizeof(reserved));
  pos += sizeof(reserved);
  return pos;
}

/**
 * @brief Write the per-page metadata table; return the offset just past it.
 *
 * @details
 * @p raw_delta is added to the LAST page's recorded raw size only, which is how
 * the caller fabricates a container whose declared size disagrees with its
 * actual stream -- the corruption these tests exercise.
 *
 * @param[out] out       Container buffer.
 * @param[in]  pos       Offset to start writing at.
 * @param[in]  count     Page count.
 * @param[in]  raw_delta Skew applied to the last page's raw size.
 *
 * @return Byte offset just past the metadata table.
 *
 * @pre @p out has room for `count * k_ra8_cbz_meta_len` bytes at @p pos.
 * @pre `s_pages` holds @p count populated entries.
 * @post Exactly @p count fixed-size records are written.
 * @post The format field's three pad bytes are zeroed.
 *
 * @note Not thread-safe; reads the file-scope `s_pages`.
 */
static inline size_t ccx_write_page_meta(uint8_t* out,
                                         size_t   pos,
                                         uint32_t count,
                                         uint32_t raw_delta)
{
  for (uint32_t i = 0U; i < count; ++i) {
    uint32_t raw_size = s_pages[i].raw_size;
    if (i == (count - 1U)) {
      raw_size += raw_delta;
    }
    memcpy(&out[pos + k_ra8_cbz_meta_off_raw], &raw_size, sizeof(raw_size));
    memcpy(&out[pos + k_ra8_cbz_meta_off_width], &s_pages[i].width, sizeof(s_pages[i].width));
    memcpy(&out[pos + k_ra8_cbz_meta_off_height], &s_pages[i].height, sizeof(s_pages[i].height));
    out[pos + k_ra8_cbz_meta_off_format]      = s_pages[i].format;
    out[pos + k_ra8_cbz_meta_off_format + 1U] = 0U;
    out[pos + k_ra8_cbz_meta_off_format + 2U] = 0U;
    out[pos + k_ra8_cbz_meta_off_format + 3U] = 0U;
    pos += (size_t)k_ra8_cbz_meta_len;
  }
  return pos;
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
static inline uint64_t ccx_pack(uint8_t* out, uint32_t flags, uint32_t raw_delta)
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

  size_t pos = ccx_write_header(out, count, flags);
  memcpy(&out[pos], offs, sizeof(offs));
  pos += sizeof(offs);
  pos = ccx_write_page_meta(out, pos, count, raw_delta);

  for (uint32_t i = 0U; i < count; ++i) {
    memcpy(&out[pos], streams[i], (size_t)slen[i]);
    pos += (size_t)slen[i];
  }
  return (uint64_t)pos;
}

/** @brief Open a freshly packed container into @p cbz; returns the open result. */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static inline ra8_err_t ccx_open(ra8_cbz_t*  cbz,
                                 ccx_file_t* file,
                                 uint8_t*    container,
                                 uint64_t    file_len,
                                 uint64_t*   offsets_buf,
                                 uint8_t*    meta_buf,
                                 uint8_t*    staging)
// NOLINTEND(readability-non-const-parameter)
{
  *file = (ccx_file_t){.data = container, .len = file_len, .fail_at = 0U, .calls = 0U};
  return ra8_cbz_open(cbz,
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

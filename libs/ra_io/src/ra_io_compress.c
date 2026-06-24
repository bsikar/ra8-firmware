/**
 * @file ra_io_compress.c
 * @brief Raw-DEFLATE compress / decompress over miniz, heap-free.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Decompression calls `tinfl_decompress_mem_to_mem` (its working state lives on
 * miniz's SOUP stack, no heap). Compression drives the low-level `tdefl` engine
 * with a caller-provided `tdefl_compressor` scratch and a put-buffer callback
 * that appends into the bounded output -- so no allocator is touched.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_compress.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra_check.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_compress";

/**
 * @struct compress_ctx_t
 * @brief Bounded output sink for the tdefl put-buffer callback.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* out;      /**< Destination buffer.        */
  uint32_t cap;      /**< Destination capacity.      */
  uint32_t len;      /**< Bytes written so far.      */
  uint8_t  overflow; /**< Set when `out` filled up.  */
} compress_ctx_t;

/**
 * @brief tdefl put-buffer callback: append `len` bytes to the bounded output.
 *
 * @details Copies into the caller buffer while space remains; on overflow it
 *          flags the context and returns false so tdefl aborts.
 *
 * @param[in] buf  Bytes emitted by the compressor.
 * @param[in] len  Number of bytes emitted (non-negative).
 * @param[in] user The ::compress_ctx_t sink.
 *
 * @return mz_bool MZ_TRUE on success, MZ_FALSE on overflow.
 * @retval MZ_TRUE  Bytes appended.
 * @retval MZ_FALSE Output buffer full.
 *
 * @pre `user` is a populated ::compress_ctx_t.
 * @pre `buf` holds `len` bytes.
 * @post On success the sink grew by `len` bytes.
 * @post On overflow the sink's `overflow` flag is set.
 *
 * @note Not thread-safe with respect to the same sink.
 *
 * @since 0.1.0
 */
static mz_bool compress_put(const void* buf, int len, void* user)
{
  compress_ctx_t* c = (compress_ctx_t*)user;
  const uint32_t  n = (uint32_t)len;
  if (n > (c->cap - c->len)) {
    c->overflow = 1U;
    return MZ_FALSE;
  }
  (void)memcpy(&c->out[c->len], buf, (size_t)n);
  c->len += n;
  return MZ_TRUE;
}

ra_err_t ra_io_compress(const uint8_t* src,
                        uint32_t       src_len,
                        uint8_t*       out,
                        uint32_t       out_cap,
                        void*          scratch,
                        uint32_t       scratch_len,
                        uint32_t*      out_len)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  if (scratch_len < (uint32_t)k_ra_io_compress_scratch_bytes) {
    return k_ra_err_invalid_size;
  }
  tdefl_compressor* d   = (tdefl_compressor*)scratch;
  compress_ctx_t    ctx = {.out = out, .cap = out_cap, .len = 0, .overflow = 0};
  if (tdefl_init(d, compress_put, &ctx, (int)TDEFL_DEFAULT_MAX_PROBES) != TDEFL_STATUS_OKAY) {
    return k_ra_fail;
  }
  const tdefl_status st = tdefl_compress_buffer(d, src, (size_t)src_len, TDEFL_FINISH);
  if (ctx.overflow != 0U) {
    return k_ra_err_no_mem;
  }
  if (st != TDEFL_STATUS_DONE) {
    return k_ra_fail;
  }
  *out_len = ctx.len;
  return k_ra_ok;
}

ra_err_t ra_io_decompress(const uint8_t* src,
                          uint32_t       src_len,
                          uint8_t*       out,
                          uint32_t       out_cap,
                          uint32_t*      out_len)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  const size_t n =
    tinfl_decompress_mem_to_mem((void*)out, (size_t)out_cap, src, (size_t)src_len, 0);
  if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra_err_no_mem;
  }
  *out_len = (uint32_t)n;
  return k_ra_ok;
}

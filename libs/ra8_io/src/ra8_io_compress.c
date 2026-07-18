/**
 * @file ra8_io_compress.c
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

#include "ra8_io_compress.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_compress";

/**
 * @struct compress_ctx_t
 * @brief Bounded output sink for the tdefl put-buffer callback.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* out;      /**< Destination buffer.       */
  uint32_t cap;      /**< Destination capacity.     */
  uint32_t len;      /**< Bytes written so far.     */
  uint8_t  overflow; /**< Set when `out` filled up. */
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
RA8_INTERNAL
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

/**
 * @brief Validate ::ra8_io_compress arguments before driving the compressor.
 *
 * @details Performs the four non-NULL precondition checks and the scratch-size
 *          lower-bound check, returning the matching ::ra8_err_t so the public
 *          entry point stays small. Kept separate from the compress drive so the
 *          validation decisions live in one cohesive block.
 *
 * @param[in] src         Bytes to compress (checked non-NULL).
 * @param[in] out         Destination buffer (checked non-NULL).
 * @param[in] scratch     Caller compressor scratch (checked non-NULL).
 * @param[in] scratch_len Size of `scratch` in bytes (checked >= minimum).
 * @param[in] out_len     Result-length out-pointer (checked non-NULL).
 *
 * @return ra8_err_t Validation result.
 * @retval k_ra8_ok               All arguments are valid.
 * @retval k_ra8_err_null_ptr     Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size `scratch_len` is below the minimum.
 *
 * @pre `s_tag` is initialized for logging.
 * @pre The caller forwards its own arguments unchanged.
 * @post On a non-ok return the caller must propagate the code verbatim.
 * @post On `k_ra8_ok` every pointer argument is non-NULL.
 *
 * @note Pure precondition check; touches no compressor state.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ra8_io_compress_validate(const uint8_t*  src,
                                          const uint8_t*  out,
                                          const void*     scratch,
                                          uint32_t        scratch_len,
                                          const uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA8_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  if (scratch_len < (uint32_t)k_ra8_io_compress_scratch_bytes) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the miniz `tdefl` engine over `src` into the bounded sink.
 *
 * @details Initializes the caller-provided compressor, runs a single finishing
 *          `tdefl_compress_buffer` pass, then maps overflow / engine status onto
 *          ::ra8_err_t. On success the produced length is taken from the sink.
 *
 * @param[in,out] d       Caller compressor scratch, reinitialized here.
 * @param[in,out] ctx     Bounded output sink (`out`/`cap` preset, `len` grows).
 * @param[in]     src     Bytes to compress.
 * @param[in]     src_len Number of input bytes.
 * @param[out]    out_len Compressed byte count on success.
 *
 * @return ra8_err_t Compression result.
 * @retval k_ra8_ok         Compressed; `*out_len` set from the sink.
 * @retval k_ra8_err_no_mem `out` was too small for the result.
 * @retval k_ra8_fail       The compressor reported an error.
 *
 * @pre `d` points to at least ::k_ra8_io_compress_scratch_bytes of scratch.
 * @pre `ctx->out`/`ctx->cap` describe the destination, `ctx->len == 0`.
 * @post On `k_ra8_ok` `*out_len` equals the bytes appended to the sink.
 * @post On any non-ok return `*out_len` is left unchanged.
 *
 * @note Not thread-safe with respect to the same scratch or sink.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ra8_io_compress_drive(tdefl_compressor* d,
                                       compress_ctx_t*   ctx,
                                       const uint8_t*    src,
                                       uint32_t          src_len,
                                       uint32_t*         out_len)
{
  if (tdefl_init(d, compress_put, ctx, (int)TDEFL_DEFAULT_MAX_PROBES) != TDEFL_STATUS_OKAY) {
    return k_ra8_fail;
  }
  const tdefl_status st = tdefl_compress_buffer(d, src, (size_t)src_len, TDEFL_FINISH);
  if (ctx->overflow != 0U) {
    return k_ra8_err_no_mem;
  }
  if (st != TDEFL_STATUS_DONE) {
    return k_ra8_fail;
  }
  *out_len = ctx->len;
  return k_ra8_ok;
}

ra8_err_t ra8_io_compress(const uint8_t* src,
                          uint32_t       src_len,
                          uint8_t*       out,
                          uint32_t       out_cap,
                          void*          scratch,
                          uint32_t       scratch_len,
                          uint32_t*      out_len)
{
  const ra8_err_t vr = ra8_io_compress_validate(src, out, scratch, scratch_len, out_len);
  if (vr != k_ra8_ok) {
    return vr;
  }
  tdefl_compressor* d   = (tdefl_compressor*)scratch;
  compress_ctx_t    ctx = {.out = out, .cap = out_cap, .len = 0, .overflow = 0};
  return ra8_io_compress_drive(d, &ctx, src, src_len, out_len);
}

ra8_err_t ra8_io_decompress(const uint8_t* src,
                            uint32_t       src_len,
                            uint8_t*       out,
                            uint32_t       out_cap,
                            uint32_t*      out_len)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  const size_t n =
    tinfl_decompress_mem_to_mem((void*)out, (size_t)out_cap, src, (size_t)src_len, 0);
  if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_no_mem;
  }
  *out_len = (uint32_t)n;
  return k_ra8_ok;
}

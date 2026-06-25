/**
 * @file ra_io_blockdev_vsource.c
 * @brief Block-device -> ra_vsource read adapter (LBA<->byte translation).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements the read-only bridge declared in `ra_io_blockdev_vsource.h`. A
 * byte-offset read is split into an optional unaligned head sector, a run of
 * whole aligned sectors copied straight into the caller's buffer, and an
 * optional unaligned tail sector. The head and tail are serviced through the
 * context's one-sector bounce buffer so only the bytes actually requested reach
 * the caller. Pure block-device access plus memcpy, so it runs identically on
 * host and target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_blockdev_vsource.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_blockdev.h"
#include "ra_vsource.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_blockdev_vsource";

/**
 * @enum ra_io_bd_vsource_const_t
 * @brief Adapter layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_io_bd_vsource_one_block = 1, /**< Blocks transferred per bounce read. */
} ra_io_bd_vsource_const_t;

/**
 * @brief Read one sector at `lba` into the context bounce buffer.
 *
 * @details
 * Single-block read into `ctx->scratch`, used to service the unaligned head and
 * tail of a byte-offset request. Wrapped here so both call sites share one
 * bounds-checked path.
 *
 * @param[in] ctx Bound adapter context (non-NULL, checked by the caller).
 * @param[in] lba Logical block address of the sector to read.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    Sector copied into `ctx->scratch`.
 * @retval k_ra_err_* Any error reported by the block device's read.
 *
 * @pre `ctx->bd` is a bound block device.
 * @pre `ctx->scratch` is one logical block of writable storage.
 * @post On success `ctx->scratch` holds the sector at `lba`.
 * @post On any non-ok return `ctx->scratch` content is unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
static ra_err_t bd_read_sector(ra_io_blockdev_vsource_ctx_t* ctx, uint32_t lba)
{
  return ra_io_blockdev_read(ctx->bd, lba, (uint32_t)k_ra_io_bd_vsource_one_block, ctx->scratch);
}

/**
 * @brief Copy a slice out of the bounce buffer after a one-sector read.
 *
 * @details
 * Reads the sector at `lba` into `ctx->scratch`, then copies `n` bytes starting
 * at `in_block` of that sector into `dst`. Used for the unaligned head and tail
 * of a byte-offset request, where only part of a sector is wanted.
 *
 * @param[in]  ctx      Bound adapter context (non-NULL, checked by the caller).
 * @param[in]  lba      Logical block address of the sector holding the slice.
 * @param[in]  in_block Byte offset of the slice within the sector (< 512).
 * @param[in]  n        Number of bytes to copy (in_block + n <= 512).
 * @param[out] dst      Destination for the `n` copied bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    `n` bytes copied into `dst`.
 * @retval k_ra_err_* Any error reported by the sector read.
 *
 * @pre `in_block + n <= k_ra_io_block_size_bytes`.
 * @pre `dst` is writable for `n` bytes.
 * @post On success `dst[0 .. n)` mirrors the device bytes of that slice.
 * @post On any non-ok return `dst` content is unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
static ra_err_t bd_copy_slice(ra_io_blockdev_vsource_ctx_t* ctx,
                              uint32_t                      lba,
                              uint32_t                      in_block,
                              uint32_t                      n,
                              uint8_t*                      dst)
{
  const ra_err_t err = bd_read_sector(ctx, lba);
  if (err != k_ra_ok) {
    return err;
  }
  (void)memcpy(dst, &ctx->scratch[in_block], (size_t)n);
  return k_ra_ok;
}

ra_err_t ra_io_blockdev_vsource_init(ra_io_blockdev_vsource_ctx_t* ctx, const ra_io_blockdev_t* bd)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  ctx->bd = bd;
  (void)memset(ctx->scratch, 0, sizeof(ctx->scratch));
  return k_ra_ok;
}

ra_err_t ra_io_blockdev_vsource_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra_io_blockdev_vsource_ctx_t* c = (ra_io_blockdev_vsource_ctx_t*)ctx;

  const uint64_t block_bytes = (uint64_t)k_ra_io_block_size_bytes;
  if (offset > (UINT64_MAX - (uint64_t)len)) {
    return k_ra_err_out_of_range;
  }

  uint64_t cur    = offset;
  uint8_t* out    = buf;
  uint32_t remain = len;

  /* Unaligned head: the byte offset does not start on a sector boundary. */
  const uint32_t head_in_block = (uint32_t)(cur % block_bytes);
  if (head_in_block != 0u) {
    uint32_t head_n = (uint32_t)(block_bytes - (uint64_t)head_in_block);
    if (head_n > remain) {
      head_n = remain;
    }
    const ra_err_t err =
      bd_copy_slice(c, (uint32_t)(cur / block_bytes), head_in_block, head_n, out);
    if (err != k_ra_ok) {
      return err;
    }
    cur += (uint64_t)head_n;
    out += head_n;
    remain -= head_n;
  }

  /* Aligned middle: whole sectors copied straight into the caller buffer. */
  const uint32_t mid_blocks = (uint32_t)((uint64_t)remain / block_bytes);
  if (mid_blocks != 0u) {
    const ra_err_t err = ra_io_blockdev_read(c->bd, (uint32_t)(cur / block_bytes), mid_blocks, out);
    if (err != k_ra_ok) {
      return err;
    }
    const uint32_t mid_bytes = (uint32_t)((uint64_t)mid_blocks * block_bytes);
    cur += (uint64_t)mid_bytes;
    out += mid_bytes;
    remain -= mid_bytes;
  }

  /* Unaligned tail: a partial final sector. */
  if (remain != 0u) {
    const ra_err_t err = bd_copy_slice(c, (uint32_t)(cur / block_bytes), 0u, remain, out);
    if (err != k_ra_ok) {
      return err;
    }
  }

  return k_ra_ok;
}

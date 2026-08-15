/**
 * @file ra8_io_blockdev_vsource.c
 * @brief Block-device -> ra8_vsource read adapter (LBA<->byte translation).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements the read-only bridge declared in `ra8_io_blockdev_vsource.h`. A
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

#include "ra8_io_blockdev_vsource.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_vsource.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_vsource";

/**
 * @enum ra8_io_bd_vsource_const_t
 * @brief Adapter layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_bd_vsource_one_block = 1, /**< Blocks transferred per bounce read. */
} ra8_io_bd_vsource_const_t;

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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Sector copied into `ctx->scratch`.
 * @retval k_ra8_err_* Any error reported by the block device's read.
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_bd_read_sector(ra8_io_blockdev_vsource_ctx_t* ctx,
                                                      uint32_t                       lba)
{
  return ra8_io_blockdev_read(ctx->bd, lba, (uint32_t)k_ra8_io_bd_vsource_one_block, ctx->scratch);
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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    `n` bytes copied into `dst`.
 * @retval k_ra8_err_* Any error reported by the sector read.
 *
 * @pre `in_block + n <= k_ra8_io_block_size_bytes`.
 * @pre `dst` is writable for `n` bytes.
 * @post On success `dst[0 .. n)` mirrors the device bytes of that slice.
 * @post On any non-ok return `dst` content is unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_bd_copy_slice(ra8_io_blockdev_vsource_ctx_t* ctx,
                                                     uint32_t                       lba,
                                                     uint32_t                       in_block,
                                                     uint32_t                       n,
                                                     uint8_t*                       dst)
{
  const ra8_err_t err = internal_bd_read_sector(ctx, lba);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)memcpy(dst, &ctx->scratch[in_block], (size_t)n);
  return k_ra8_ok;
}

/**
 * @struct ra8_io_bd_vsource_cursor_t
 * @brief Mutable walk state shared by the head/middle/tail read segments.
 *
 * @details
 * A byte-offset read is serviced as up to three segments. Each segment advances
 * the same running cursor: the current absolute byte position, the next write
 * position in the caller buffer, and the count of bytes still owed. Bundling the
 * three fields lets each segment live in its own helper without smearing the
 * walk state across separate by-reference out-parameters.
 *
 * @invariant `cur` is the absolute device byte offset of the next unread byte.
 * @invariant `out` points at the next unwritten byte of the caller buffer.
 *
 * @since 0.1.0
 */
typedef struct {
  uint64_t cur;    /**< Absolute device byte offset of the next unread byte.    */
  uint8_t* out;    /**< Next unwritten byte of the caller's destination buffer. */
  uint32_t remain; /**< Bytes still owed to the caller.                         */
} ra8_io_bd_vsource_cursor_t;

/**
 * @brief Service the unaligned head sector of a byte-offset read.
 *
 * @details
 * When the request does not begin on a sector boundary, copies the partial
 * leading sector through the bounce buffer and advances the cursor past it. If
 * the request already starts on a boundary this is a no-op returning success.
 * The copy is clamped so a sub-sector request never reads past `cur->remain`.
 *
 * @param[in]     ctx         Bound adapter context (non-NULL, checked by caller).
 * @param[in,out] cur         Walk state advanced past the head bytes on success.
 * @param[in]     block_bytes Logical block size in bytes (one sector).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Head serviced (possibly a no-op) and cursor advanced.
 * @retval k_ra8_err_* Any error reported by the bounce-buffer read.
 *
 * @pre `block_bytes` equals the device logical block size and is non-zero.
 * @pre `cur->out` is writable for at least `cur->remain` bytes.
 * @post On success the cursor reflects a sector-aligned `cur->cur`, or `remain`
 *       reached zero.
 * @post On any non-ok return the cursor is left unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_bd_read_head(ra8_io_blockdev_vsource_ctx_t* ctx,
                                                    ra8_io_bd_vsource_cursor_t*    cur,
                                                    uint64_t                       block_bytes)
{
  const uint32_t head_in_block = (uint32_t)(cur->cur % block_bytes);
  if (head_in_block == 0U) {
    return k_ra8_ok;
  }
  uint32_t head_n = (uint32_t)(block_bytes - (uint64_t)head_in_block);
  if (head_n > cur->remain) {
    head_n = cur->remain;
  }
  const ra8_err_t err = internal_bd_copy_slice(ctx,
                                               (uint32_t)(cur->cur / block_bytes),
                                               head_in_block,
                                               head_n,
                                               cur->out);
  if (err != k_ra8_ok) {
    return err;
  }
  cur->cur += (uint64_t)head_n;
  cur->out += head_n;
  cur->remain -= head_n;
  return k_ra8_ok;
}

/**
 * @brief Service the aligned middle of a byte-offset read.
 *
 * @details
 * Reads every whole sector that fits in the remaining length straight into the
 * caller's buffer with no intermediate copy, then advances the cursor past them.
 * If fewer than one full sector remains this is a no-op returning success.
 *
 * @param[in]     ctx         Bound adapter context (non-NULL, checked by caller).
 * @param[in,out] cur         Walk state advanced past the whole sectors copied.
 * @param[in]     block_bytes Logical block size in bytes (one sector).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Whole sectors (possibly none) copied and cursor advanced.
 * @retval k_ra8_err_* Any error reported by the block device's read.
 *
 * @pre `block_bytes` equals the device logical block size and is non-zero.
 * @pre `cur->cur` is sector-aligned on entry.
 * @post On success `cur->remain` is strictly less than `block_bytes`.
 * @post On any non-ok return the cursor is left unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_bd_read_middle(ra8_io_blockdev_vsource_ctx_t* ctx,
                                                      ra8_io_bd_vsource_cursor_t*    cur,
                                                      uint64_t                       block_bytes)
{
  const uint32_t mid_blocks = (uint32_t)((uint64_t)cur->remain / block_bytes);
  if (mid_blocks == 0U) {
    return k_ra8_ok;
  }
  const ra8_err_t err =
    ra8_io_blockdev_read(ctx->bd, (uint32_t)(cur->cur / block_bytes), mid_blocks, cur->out);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t mid_bytes = (uint32_t)((uint64_t)mid_blocks * block_bytes);
  cur->cur += (uint64_t)mid_bytes;
  cur->out += mid_bytes;
  cur->remain -= mid_bytes;
  return k_ra8_ok;
}

/**
 * @brief Service the unaligned tail sector of a byte-offset read.
 *
 * @details
 * Copies any sub-sector remainder left after the aligned middle through the
 * bounce buffer. By construction the tail always starts on a sector boundary, so
 * the in-block offset is zero. A zero remainder is a no-op returning success.
 *
 * @param[in]     ctx         Bound adapter context (non-NULL, checked by caller).
 * @param[in,out] cur         Walk state; `cur->remain` reaches zero on success.
 * @param[in]     block_bytes Logical block size in bytes (one sector).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Tail serviced (possibly a no-op); request fully satisfied.
 * @retval k_ra8_err_* Any error reported by the bounce-buffer read.
 *
 * @pre `block_bytes` equals the device logical block size and is non-zero.
 * @pre `cur->cur` is sector-aligned on entry.
 * @post On success `cur->remain` is zero.
 * @post On any non-ok return the cursor is left unspecified.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_bd_read_tail(ra8_io_blockdev_vsource_ctx_t* ctx,
                                                    ra8_io_bd_vsource_cursor_t*    cur,
                                                    uint64_t                       block_bytes)
{
  if (cur->remain == 0U) {
    return k_ra8_ok;
  }
  return internal_bd_copy_slice(ctx, (uint32_t)(cur->cur / block_bytes), 0U, cur->remain, cur->out);
}

ra8_err_t ra8_io_blockdev_vsource_init(ra8_io_blockdev_vsource_ctx_t* ctx,
                                       const ra8_io_blockdev_t*       bd)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  ctx->bd = bd;
  (void)memset(ctx->scratch, 0, sizeof(ctx->scratch));
  return k_ra8_ok;
}

ra8_err_t ra8_io_blockdev_vsource_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_blockdev_vsource_ctx_t* c = (ra8_io_blockdev_vsource_ctx_t*)ctx;

  const uint64_t block_bytes = (uint64_t)k_ra8_io_block_size_bytes;
  if (offset > (UINT64_MAX - (uint64_t)len)) {
    return k_ra8_err_out_of_range;
  }

  ra8_io_bd_vsource_cursor_t cur = {.cur = offset, .out = buf, .remain = len};

  /* Unaligned head: the byte offset does not start on a sector boundary. */
  ra8_err_t err = internal_bd_read_head(c, &cur, block_bytes);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Aligned middle: whole sectors copied straight into the caller buffer. */
  err = internal_bd_read_middle(c, &cur, block_bytes);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Unaligned tail: a partial final sector. */
  return internal_bd_read_tail(c, &cur, block_bytes);
}

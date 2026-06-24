/**
 * @file ra_io_stream_blockdev.c
 * @brief Block-device byte-stream sink -- buffer bytes into 512-byte sectors.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Accumulates stream bytes into a sector buffer; a full sector is committed via
 * ::ra_io_blockdev_write and the LBA advances. A flush zero-pads and commits the
 * partial trailing sector. The sector buffer lives in the caller-owned state, so
 * there is no allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_stream_blockdev.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_blockdev.h"
#include "ra_io_stream_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_stream_blockdev";

/**
 * @enum ra_io_stream_bd_const_t
 * @brief Block-device sink constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_bd_one_block = 1, /**< Single-block transfer count. */
  k_bd_pad_byte  = 0, /**< Pad value for a partial trailing sector. */
} ra_io_stream_bd_const_t;

/**
 * @brief Block-device sink: buffer bytes, auto-committing full sectors.
 *
 * @details
 * Copies bytes into the sector buffer in chunks; whenever the buffer fills it is
 * written as one logical block and the LBA advances. Reports the consumed count.
 *
 * @param[in]  ctx         Block-device sink state (as a void cookie).
 * @param[in]  buf         Source bytes.
 * @param[in]  len         Number of bytes to stream.
 * @param[out] out_written Consumed byte count, or NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           All bytes buffered (full sectors committed).
 * @retval k_ra_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra_err_*        Propagated from ::ra_io_blockdev_write.
 *
 * @pre `ctx` is a populated block-device sink state.
 * @pre `buf` is readable for `len` bytes.
 * @post On success every full sector reached has been written to the device.
 * @post `*out_written` (when provided) holds the consumed count.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
static ra_err_t bdsink_write(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra_io_stream_blockdev_state_t* st   = (ra_io_stream_blockdev_state_t*)ctx;
  uint32_t                       done = 0;
  while (done < len) {
    const uint32_t room  = (uint32_t)k_ra_io_block_size_bytes - st->fill;
    const uint32_t rem   = len - done;
    const uint32_t chunk = (rem < room) ? rem : room;
    (void)memcpy(&st->sector[st->fill], &buf[done], (size_t)chunk);
    st->fill += chunk;
    done += chunk;
    if (st->fill == (uint32_t)k_ra_io_block_size_bytes) {
      const ra_err_t e =
        ra_io_blockdev_write(st->bd, st->lba, (uint32_t)k_bd_one_block, st->sector);
      if (e != k_ra_ok) {
        if (out_written != nullptr) {
          *out_written = done;
        }
        return e;
      }
      st->lba += (uint32_t)k_bd_one_block;
      st->fill = 0;
    }
  }
  if (out_written != nullptr) {
    *out_written = len;
  }
  return k_ra_ok;
}

/**
 * @brief Block-device sink: commit a partial trailing sector (zero-padded).
 *
 * @details
 * If a partial sector is buffered, the remainder is zero-filled and the sector
 * is written as one logical block. A flush with no pending bytes is a no-op.
 *
 * @param[in] ctx Block-device sink state (as a void cookie).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Pending sector committed (or none pending).
 * @retval k_ra_err_null_ptr `ctx` was NULL.
 * @retval k_ra_err_*        Propagated from ::ra_io_blockdev_write.
 *
 * @pre `ctx` is a populated block-device sink state.
 * @pre The stream is idle.
 * @post On success the device holds every streamed byte (last sector padded).
 * @post `fill` is reset to zero.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
static ra_err_t bdsink_flush(void* ctx)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra_io_stream_blockdev_state_t* st = (ra_io_stream_blockdev_state_t*)ctx;
  if (st->fill == 0U) {
    return k_ra_ok;
  }
  const size_t pad = (size_t)((uint32_t)k_ra_io_block_size_bytes - st->fill);
  (void)memset(&st->sector[st->fill], (int)k_bd_pad_byte, pad);
  const ra_err_t e = ra_io_blockdev_write(st->bd, st->lba, (uint32_t)k_bd_one_block, st->sector);
  if (e != k_ra_ok) {
    return e;
  }
  st->lba += (uint32_t)k_bd_one_block;
  st->fill = 0;
  return k_ra_ok;
}

/** @brief Block-device stream sink vtable. */
static const ra_io_stream_iface_t k_bd_iface = {
  .write = bdsink_write,
  .flush = bdsink_flush,
};

ra_err_t ra_io_stream_blockdev_init(ra_io_stream_t*                s,
                                    ra_io_stream_blockdev_state_t* state,
                                    const ra_io_blockdev_t*        bd,
                                    uint32_t                       start_lba)
{
  RA_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  RA_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  state->bd   = bd;
  state->lba  = start_lba;
  state->fill = 0;
  s->iface    = &k_bd_iface;
  s->ctx      = state;
  return k_ra_ok;
}

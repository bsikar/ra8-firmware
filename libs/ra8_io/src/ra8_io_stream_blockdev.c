/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_stream_blockdev.c
 * @brief Block-device byte-stream sink -- buffer bytes into 512-byte sectors.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Accumulates stream bytes into a sector buffer; a full sector is committed via
 * ::ra8_io_blockdev_write and the LBA advances. A flush zero-pads and commits the
 * partial trailing sector. The sector buffer lives in the caller-owned state, so
 * there is no allocation.
 */

#include "ra8_io_stream_blockdev.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_stream_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_stream_blockdev";

/**
 * @enum ra8_io_stream_bd_const_t
 * @brief Block-device sink constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_bd_one_block = 1, /**< Single-block transfer count.             */
  k_bd_pad_byte  = 0, /**< Pad value for a partial trailing sector. */
} ra8_io_stream_bd_const_t;

/**
 * @brief Commit the buffered sector as one logical block and advance the LBA.
 *
 * @details
 * Writes the full `sector` buffer to the device at the current LBA via
 * ::ra8_io_blockdev_write. On success the LBA advances by one block and the fill
 * counter is reset to zero; on failure the error is returned and the state is
 * left unchanged so the caller can decide how to report it.
 *
 * @param[in,out] st Block-device sink state with a sector ready to commit.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Sector written; LBA advanced and `fill` reset.
 * @retval k_ra8_err_* Propagated from ::ra8_io_blockdev_write.
 *
 * @pre `st` is a populated block-device sink state.
 * @pre `st->sector` holds the bytes to commit.
 * @post On success `st->lba` is advanced by one block and `st->fill` is zero.
 * @post On failure `st` is left unmodified.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t bdsink_commit_sector(ra8_io_stream_blockdev_state_t* st)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  RA8_CHECK_NULL_PTR(st->bd, s_tag, "st->bd must not be nullptr");
  const ra8_err_t e = ra8_io_blockdev_write(st->bd, st->lba, (uint32_t)k_bd_one_block, st->sector);
  if (e != k_ra8_ok) {
    return e;
  }
  st->lba += (uint32_t)k_bd_one_block;
  st->fill = 0;
  return k_ra8_ok;
}

/**
 * @brief Copy one bounded chunk from the source into the sector buffer.
 *
 * @details
 * Copies up to a full sector's worth of bytes -- limited by both the bytes that
 * remain in the source (`len - done`) and the room left in the sector buffer --
 * then advances both the fill counter and the consumed count. Performs no device
 * I/O; committing a now-full sector is the caller's responsibility.
 *
 * @param[in,out] st      Block-device sink state receiving the bytes.
 * @param[in]     buf     Source bytes.
 * @param[in]     len     Total number of bytes the caller is streaming.
 * @param[in,out] done    Running consumed-byte count, advanced by the chunk.
 *
 * @return uint32_t Number of bytes copied in this chunk (1..block size).
 * @retval k_ra8_io_block_size_bytes The chunk filled the remaining sector room
 *         (source had at least that many bytes left).
 * @retval other A value in [1, k_ra8_io_block_size_bytes) when the source
 *         (`len - *done`) is exhausted before the sector fills.
 *
 * @pre `st` is a populated block-device sink state with room to spare.
 * @pre `buf` is readable for `len` bytes and `*done < len`.
 * @post `st->fill` and `*done` are advanced by the returned chunk size.
 * @post The returned chunk size never overflows the sector buffer.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t bdsink_fill_chunk(ra8_io_stream_blockdev_state_t* st,
                                  const uint8_t*                  buf,
                                  uint32_t                        len,
                                  uint32_t*                       done)
{
  const uint32_t room  = (uint32_t)k_ra8_io_block_size_bytes - st->fill;
  const uint32_t rem   = len - *done;
  const uint32_t chunk = (rem < room) ? rem : room;
  (void)memcpy(&st->sector[st->fill], &buf[*done], (size_t)chunk);
  st->fill += chunk;
  *done += chunk;
  return chunk;
}

/**
 * @brief Commit the buffered sector iff it is exactly full.
 *
 * @details
 * Helper for ::bdsink_write -- writes the staged sector through
 * ::bdsink_commit_sector only when `fill` has reached one logical block,
 * otherwise it is a no-op. Extracted so ::bdsink_write stays within the
 * nesting-depth limit.
 *
 * @param[in] st Block-device sink state.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Sector committed, or none was full yet.
 * @retval k_ra8_err_* Propagated from ::bdsink_commit_sector.
 *
 * @pre `st` is a populated block-device sink state.
 * @pre `st->fill` is in [0, k_ra8_io_block_size_bytes].
 * @post On a full buffer the sector is written and `fill` is reset.
 * @post On a partial buffer no I/O occurs and state is unchanged.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t bdsink_commit_if_full(ra8_io_stream_blockdev_state_t* st)
{
  if (st->fill == (uint32_t)k_ra8_io_block_size_bytes) {
    return bdsink_commit_sector(st);
  }
  return k_ra8_ok;
}

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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All bytes buffered (full sectors committed).
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_blockdev_write.
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
RA8_INTERNAL
static ra8_err_t bdsink_write(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_stream_blockdev_state_t* st   = (ra8_io_stream_blockdev_state_t*)ctx;
  uint32_t                        done = 0;
  while (done < len) {
    (void)bdsink_fill_chunk(st, buf, len, &done);
    const ra8_err_t e = bdsink_commit_if_full(st);
    if (e != k_ra8_ok) {
      if (out_written != nullptr) {
        *out_written = done;
      }
      return e;
    }
  }
  if (out_written != nullptr) {
    *out_written = len;
  }
  return k_ra8_ok;
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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Pending sector committed (or none pending).
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_blockdev_write.
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
RA8_INTERNAL
static ra8_err_t bdsink_flush(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra8_io_stream_blockdev_state_t* st = (ra8_io_stream_blockdev_state_t*)ctx;
  if (st->fill == 0U) {
    return k_ra8_ok;
  }
  const size_t pad = (size_t)((uint32_t)k_ra8_io_block_size_bytes - st->fill);
  (void)memset(&st->sector[st->fill], (int)k_bd_pad_byte, pad);
  return bdsink_commit_sector(st);
}

/** @brief Block-device stream sink vtable. */
static const ra8_io_stream_iface_t k_bd_iface = {
  .write = bdsink_write,
  .flush = bdsink_flush,
};

ra8_err_t ra8_io_stream_blockdev_init(ra8_io_stream_t*                s,
                                      ra8_io_stream_blockdev_state_t* state,
                                      const ra8_io_blockdev_t*        bd,
                                      uint32_t                        start_lba)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  state->bd   = bd;
  state->lba  = start_lba;
  state->fill = 0;
  s->iface    = &k_bd_iface;
  s->ctx      = state;
  return k_ra8_ok;
}

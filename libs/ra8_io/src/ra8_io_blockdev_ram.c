/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_ram.c
 * @brief RAM block-device backend -- a flat buffer as 512-byte logical blocks.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Reference implementation of ::ra8_io_blockdev_iface. Reads and writes are
 * bounds-checked `memcpy`s into the caller-owned buffer; erase zero-fills.
 * Pure memory access, so it runs identically on host and target.
 */

#include "ra8_io_blockdev_ram.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_ram";

/**
 * @enum ra8_io_ram_const_t
 * @brief RAM-backend layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_ram_erase_unit_blocks = 1, /**< One logical block per erase unit. */
  k_ra8_io_ram_min_blocks        = 1, /**< Smallest legal device size.       */
} ra8_io_ram_const_t;

/**
 * @brief Reject an out-of-range `[lba, lba+count)` block range.
 *
 * @details
 * Split into two single-condition checks (no compound decision) so the bounds
 * test needs no MC/DC vectors and never underflows `block_count - count`.
 *
 * @param[in] st    RAM backend state.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks in the range.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Range lies within the device.
 * @retval k_ra8_err_out_of_range Range extends past the device capacity.
 *
 * @pre `st` is non-NULL.
 * @pre `st->block_count` reflects the backing buffer size.
 * @post No state is mutated.
 * @post The return reflects only the range/capacity comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_bounds(const ra8_io_blockdev_ram_state_t* st, uint32_t lba, uint32_t count)
{
  if (count > st->block_count) {
    return k_ra8_err_out_of_range;
  }
  if (lba > st->block_count - count) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

/**
 * @brief RAM backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * Bounds-checks the range, then copies `count * 512` bytes from the backing
 * buffer.
 *
 * @param[in]  ctx   RAM backend state (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks copied into `buf`.
 * @retval k_ra8_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past capacity.
 *
 * @pre `ctx` is a populated RAM state.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the backing buffer.
 * @post On failure `buf` is untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_ram_state_t* st = (const ra8_io_blockdev_ram_state_t*)ctx;
  const ra8_err_t                    b  = ram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const size_t off = (size_t)lba * (size_t)k_ra8_io_block_size_bytes;
  const size_t n   = (size_t)count * (size_t)k_ra8_io_block_size_bytes;
  (void)memcpy(buf, &st->storage[off], n);
  return k_ra8_ok;
}

/**
 * @brief RAM backend: write `count` blocks from `buf` at `lba`.
 *
 * @details
 * Bounds-checks the range, honours the read-only flag, then copies `count * 512`
 * bytes into the backing buffer.
 *
 * @param[in] ctx   RAM backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks copied into the backing buffer.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 *
 * @pre `ctx` is a populated RAM state.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the backing buffer reflects `buf`.
 * @post On failure the backing buffer is untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_blockdev_ram_state_t* st = (ra8_io_blockdev_ram_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t b = ram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const size_t off = (size_t)lba * (size_t)k_ra8_io_block_size_bytes;
  const size_t n   = (size_t)count * (size_t)k_ra8_io_block_size_bytes;
  (void)memcpy(&st->storage[off], buf, n);
  return k_ra8_ok;
}

/**
 * @brief RAM backend: erase (zero-fill) `count` blocks at `lba`.
 *
 * @details
 * Bounds-checks the range, honours the read-only flag, then zero-fills
 * `count * 512` bytes (the RAM erase value).
 *
 * @param[in] ctx   RAM backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to erase.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Range zero-filled.
 * @retval k_ra8_err_null_ptr      `ctx` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 *
 * @pre `ctx` is a populated RAM state.
 * @pre `count` is non-zero for any observable effect.
 * @post On success the range reads back as 0x00.
 * @post On failure the backing buffer is untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra8_io_blockdev_ram_state_t* st = (ra8_io_blockdev_ram_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t b = ram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const size_t off = (size_t)lba * (size_t)k_ra8_io_block_size_bytes;
  const size_t n   = (size_t)count * (size_t)k_ra8_io_block_size_bytes;
  (void)memset(&st->storage[off], (int)k_ra8_io_erase_value_zero, n);
  return k_ra8_ok;
}

/**
 * @brief RAM backend: report medium capabilities.
 *
 * @details
 * Reports a zero-erase medium that needs no erase-before-write, sized by the
 * backing buffer.
 *
 * @param[in]  ctx RAM backend state (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated.
 * @retval k_ra8_err_null_ptr `ctx` or `out` was NULL.
 *
 * @pre `ctx` is a populated RAM state.
 * @pre `out` is writable.
 * @post On success `*out` describes a zero-erase, no-erase-required medium.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_io_blockdev_ram_state_t* st = (const ra8_io_blockdev_ram_state_t*)ctx;
  out->block_count                      = st->block_count;
  out->erase_unit_blocks                = (uint32_t)k_ra8_io_ram_erase_unit_blocks;
  out->program_size_bytes               = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes              = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value                      = (uint8_t)k_ra8_io_erase_value_zero;
  out->must_erase_before_write          = false;
  out->read_only                        = st->read_only;
  return k_ra8_ok;
}

/** @brief RAM backend vtable. `sync` is NULL: writes hit memory immediately. */
static const ra8_io_blockdev_iface_t k_ram_iface = {
  .read     = ram_read,
  .write    = ram_write,
  .erase    = ram_erase,
  .get_caps = ram_get_caps,
  .sync     = nullptr,
};

ra8_err_t ra8_io_blockdev_ram_init(ra8_io_blockdev_t*           bd,
                                   ra8_io_blockdev_ram_state_t* state,
                                   uint8_t*                     storage,
                                   uint32_t                     block_count,
                                   bool                         read_only)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (block_count < (uint32_t)k_ra8_io_ram_min_blocks) {
    return k_ra8_err_invalid_size;
  }
  state->storage     = storage;
  state->block_count = block_count;
  state->read_only   = read_only;
  bd->iface          = &k_ram_iface;
  bd->ctx            = state;
  return k_ra8_ok;
}

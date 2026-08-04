/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_mram.c
 * @brief MRAM block-device backend -- a hard-fenced data-MRAM window as blocks.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_blockdev_iface over the on-chip extra MRAM region.
 * Reads `memcpy` from the memory-mapped MRAM array; writes route through
 * `ra8_flash_extra_mram_write` chunked into the 32-byte MRAM program unit; erases
 * route through `ra8_flash_extra_mram_erase`, one 32-byte block per call. The
 * window is fenced at bind time to the extra MRAM region and every call is
 * bounds-checked, so no access can reach the code-MRAM `.text` region. This file
 * touches no raw MMIO -- the `ra8_flash` driver carries the HUM citations.
 *
 * @warning The RA8D2 extra MRAM is one-time-programmable option-setting / OTP
 * memory (HUM Ch 59.7.4.5 Table 59.15 p 3592), NOT a rewritable data-flash
 * array -- there is no erase-and-reuse cycle on this silicon. A rewritable
 * block backend therefore belongs on a real rewritable medium (OSPI / SD);
 * retargeting this backend and its demos off the OTP window is tracked by #315.
 */

#include "ra8_io_blockdev_mram.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_regs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_mram";

/**
 * @enum ra8_io_mram_const_t
 * @brief MRAM-backend layout constants.
 *
 * @details
 * The MRAM program/erase unit is 32 bytes, so one 512-byte logical block maps
 * onto exactly sixteen MRAM erase blocks. The medium erases to all-ones and
 * needs an erase before a program can raise a bit, so callers must treat it as
 * erase-before-write.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_mram_erase_block_bytes = (uint32_t)k_ra8_mram_block_size_bytes,
  /**< MRAM erase/program unit in bytes (32). */
  k_ra8_io_mram_program_bytes = (uint32_t)k_ra8_mram_write_size_bytes,
  /**< MRAM minimum program granularity in bytes (32). */
  k_ra8_io_mram_erase_unit_blocks = 1U,
  /**< Logical-block erase granularity: 1 (MRAM erases in 32-byte sub-units). */
  k_ra8_io_mram_min_blocks = 1U, /**< Smallest legal device size.   */
  k_ra8_io_mram_zero_len   = 0U, /**< Sentinel for an empty length. */
} ra8_io_mram_const_t;

/**
 * @brief Reject an out-of-range `[lba, lba+count)` block range.
 *
 * @details
 * Split into two single-condition checks (no compound decision) so the bounds
 * test needs no MC/DC vectors and never underflows `block_count - count`.
 *
 * @param[in] st    MRAM backend state.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks in the range.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Range lies within the device.
 * @retval k_ra8_err_out_of_range Range extends past the device capacity.
 *
 * @pre `st` is non-NULL.
 * @pre `st->block_count` reflects the fenced window size.
 * @post No state is mutated.
 * @post The return reflects only the range/capacity comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_bounds(const ra8_io_blockdev_mram_state_t* st, uint32_t lba, uint32_t count)
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
 * @brief Validate that an MRAM window is erase-aligned and inside data MRAM.
 *
 * @details
 * Enforces the hard fence: the base must be erase-block aligned, the byte span
 * must be a whole number of erase blocks, and the window must lie wholly inside
 * the extra/data MRAM region -- which is disjoint from (and above) code MRAM, so
 * no access can reach the `.text` region. Each predicate is its own
 * single-condition check so the fence needs no MC/DC vectors and cannot underflow.
 *
 * @param[in] base        First MRAM byte address of the window.
 * @param[in] block_count Number of 512-byte logical blocks (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Window is aligned and inside data MRAM.
 * @retval k_ra8_err_invalid_arg Window misaligned, sized wrong, or out of region.
 *
 * @pre `block_count` is non-zero.
 * @pre The extra/data MRAM region constants describe the live silicon.
 * @post No state is mutated.
 * @post On success the window cannot reach code MRAM.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_window_ok(uintptr_t base, uint32_t block_count)
{
  const uintptr_t extra_start = (uintptr_t)k_ra8_flash_extra_start;
  const uintptr_t extra_end   = extra_start + (uintptr_t)k_ra8_flash_extra_size;
  const uintptr_t span        = (uintptr_t)block_count * (uintptr_t)k_ra8_io_block_size_bytes;
  if ((base % (uintptr_t)k_ra8_io_mram_erase_block_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((span % (uintptr_t)k_ra8_io_mram_erase_block_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (base < extra_start) {
    return k_ra8_err_invalid_arg;
  }
  if (base >= extra_end) {
    return k_ra8_err_invalid_arg;
  }
  if (span > extra_end - base) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief MRAM backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * MRAM is memory-mapped, so the read is a bounds-checked `memcpy` from the
 * device window. No `ra8_flash` call is needed for reads.
 *
 * @param[in]  ctx   MRAM backend state (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks copied into `buf`.
 * @retval k_ra8_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past capacity.
 *
 * @pre `ctx` is a populated MRAM state.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the MRAM window contents.
 * @post On failure `buf` is untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_mram_state_t* st = (const ra8_io_blockdev_mram_state_t*)ctx;
  const ra8_err_t                     b  = mram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const size_t off = (size_t)lba * (size_t)k_ra8_io_block_size_bytes;
  const size_t n   = (size_t)count * (size_t)k_ra8_io_block_size_bytes;
  (void)memcpy(buf, (const uint8_t*)(st->base + (uintptr_t)off), n);
  return k_ra8_ok;
}

/**
 * @brief MRAM backend: program `count` blocks from `buf` at `lba`.
 *
 * @details
 * Routes the write through `ra8_flash_extra_mram_write` in 32-byte program-unit
 * chunks (the driver's program granularity). The range is bounds-checked first
 * and the device read-only flag is honoured.
 *
 * @param[in] ctx   MRAM backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks programmed into the MRAM window.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 * @retval k_ra8_err_invalid_arg   The driver rejected the program request.
 * @retval k_ra8_err_hw_error      The controller reported a program error.
 *
 * @pre `ctx` is a populated MRAM state.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the MRAM window reflects `buf`.
 * @post On failure earlier chunks may already be committed (no rollback).
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_blockdev_mram_state_t* st = (ra8_io_blockdev_mram_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t b = mram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const uintptr_t off  = (uintptr_t)lba * (uintptr_t)k_ra8_io_block_size_bytes;
  const uintptr_t span = (uintptr_t)count * (uintptr_t)k_ra8_io_block_size_bytes;
  const uint32_t  step = (uint32_t)k_ra8_io_mram_program_bytes;
  for (uintptr_t done = (uintptr_t)k_ra8_io_mram_zero_len; done < span; done += step) {
    const ra8_err_t w =
      ra8_flash_extra_mram_write((uint32_t)(st->base + off + done), &buf[done], step);
    if (w != k_ra8_ok) {
      return w;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief MRAM backend: erase `count` blocks at `lba` to all-ones.
 *
 * @details
 * Each 512-byte logical block spans sixteen 32-byte MRAM erase blocks, erased
 * one block per `ra8_flash_extra_mram_erase` call across the range. After success
 * the range reads back as `0xFF`.
 *
 * @param[in] ctx   MRAM backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to erase.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Range erased to all-ones.
 * @retval k_ra8_err_null_ptr      `ctx` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 * @retval k_ra8_err_invalid_arg   The driver rejected the erase request.
 * @retval k_ra8_err_hw_error      The controller reported a program error.
 *
 * @pre `ctx` is a populated MRAM state.
 * @pre `count` is non-zero for any observable effect.
 * @post On success the range reads back as 0xFF.
 * @post On failure the window may be partially erased (no rollback).
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra8_io_blockdev_mram_state_t* st = (ra8_io_blockdev_mram_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t b = mram_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const uintptr_t off  = (uintptr_t)lba * (uintptr_t)k_ra8_io_block_size_bytes;
  const uintptr_t span = (uintptr_t)count * (uintptr_t)k_ra8_io_block_size_bytes;
  const uint32_t  step = (uint32_t)k_ra8_io_mram_erase_block_bytes;
  for (uintptr_t done = (uintptr_t)k_ra8_io_mram_zero_len; done < span; done += step) {
    const ra8_err_t e = ra8_flash_extra_mram_erase((uint32_t)(st->base + off + done));
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief MRAM backend: report medium capabilities.
 *
 * @details
 * Reports an all-ones-erase, erase-before-write medium with a 32-byte program
 * unit, sized by the fenced window.
 *
 * @param[in]  ctx MRAM backend state (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated.
 * @retval k_ra8_err_null_ptr `ctx` or `out` was NULL.
 *
 * @pre `ctx` is a populated MRAM state.
 * @pre `out` is writable.
 * @post On success `*out` describes an all-ones-erase, erase-before-write medium.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t mram_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_io_blockdev_mram_state_t* st = (const ra8_io_blockdev_mram_state_t*)ctx;
  out->block_count                       = st->block_count;
  out->erase_unit_blocks                 = (uint32_t)k_ra8_io_mram_erase_unit_blocks;
  out->program_size_bytes                = (uint32_t)k_ra8_io_mram_program_bytes;
  out->logical_block_bytes               = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value                       = (uint8_t)k_ra8_io_erase_value_ones;
  out->must_erase_before_write           = true;
  out->read_only                         = st->read_only;
  return k_ra8_ok;
}

/** @brief MRAM backend vtable. `sync` NULL: `ra8_flash_extra_mram_write` commits inline. */
static const ra8_io_blockdev_iface_t k_mram_iface = {
  .read     = mram_read,
  .write    = mram_write,
  .erase    = mram_erase,
  .get_caps = mram_get_caps,
  .sync     = nullptr,
};

ra8_err_t ra8_io_blockdev_mram_init(ra8_io_blockdev_t*            bd,
                                    ra8_io_blockdev_mram_state_t* state,
                                    uintptr_t                     base_addr,
                                    uint32_t                      block_count,
                                    bool                          read_only)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (block_count < (uint32_t)k_ra8_io_mram_min_blocks) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t w = mram_window_ok(base_addr, block_count);
  if (w != k_ra8_ok) {
    return w;
  }
  state->base        = base_addr;
  state->block_count = block_count;
  state->read_only   = read_only;
  bd->iface          = &k_mram_iface;
  bd->ctx            = state;
  return k_ra8_ok;
}

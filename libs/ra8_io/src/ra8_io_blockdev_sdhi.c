/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_sdhi.c
 * @brief Native-SDHI block-device backend -- an SD card as 512-byte blocks.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_blockdev_iface by forwarding reads and writes to the
 * `ra8_sdcard` HAL driver, which drives the RA8D2 4-bit SDHI controller. The
 * card is a singleton, so the bound context is NULL and this file carries no
 * per-device state. The card exposes no host erase primitive, so the vtable's
 * `erase` slot is NULL; capacity comes from ::ra8_sdcard_get_capacity. This file
 * touches no raw MMIO -- the HUM citations live in the `ra8_sdcard` driver.
 */

#include "ra8_io_blockdev_sdhi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "ra8_sdcard.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_sdhi";

/**
 * @enum ra8_io_sdhi_const_t
 * @brief Native-SDHI backend layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_sdhi_erase_unit_blocks = 1, /**< One logical block per erase unit. */
} ra8_io_sdhi_const_t;

/**
 * @brief Native-SDHI backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * Validates `buf` then forwards straight to ::ra8_sdcard_read_blocks; the card
 * driver enforces capacity bounds and initialisation state. `ctx` is unused
 * (the card is a singleton).
 *
 * @param[in]  ctx   Unused backend context (always NULL for this backend).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks read into `buf`.
 * @retval k_ra8_err_null_ptr      `buf` was NULL.
 * @retval k_ra8_err_invalid_arg   `count` was zero.
 * @retval k_ra8_err_invalid_state The card was never initialised.
 * @retval k_ra8_err_out_of_range  Range past card capacity.
 *
 * @pre ::ra8_sdcard_init has returned ::k_ra8_ok.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` holds the requested card data.
 * @post On failure `buf` is left as the card driver leaves it.
 *
 * @note Blocking, polled; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdhi_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra8_sdcard_read_blocks(lba, buf, count);
}

/**
 * @brief Native-SDHI backend: write `count` blocks from `buf` at `lba`.
 *
 * @details
 * Validates `buf` then forwards straight to ::ra8_sdcard_write_blocks; the card
 * driver enforces capacity bounds and initialisation state. `ctx` is unused
 * (the card is a singleton).
 *
 * @param[in] ctx   Unused backend context (always NULL for this backend).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks written to the card.
 * @retval k_ra8_err_null_ptr      `buf` was NULL.
 * @retval k_ra8_err_invalid_arg   `count` was zero.
 * @retval k_ra8_err_invalid_state The card was never initialised.
 * @retval k_ra8_err_out_of_range  Range past card capacity.
 *
 * @pre ::ra8_sdcard_init has returned ::k_ra8_ok.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the card range `[lba, lba+count)` equals `buf`.
 * @post On failure the card is left as the card driver leaves it.
 *
 * @note Blocking, polled; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdhi_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra8_sdcard_write_blocks(lba, buf, count);
}

/**
 * @brief Native-SDHI backend: report medium capabilities.
 *
 * @details
 * Queries the card block count via ::ra8_sdcard_get_capacity and fills `out` for
 * a zero-erase, no-erase-required, read-write medium with a 512-byte program
 * and erase unit. `ctx` is unused (the card is a singleton).
 *
 * @param[in]  ctx Unused backend context (always NULL for this backend).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                `*out` populated.
 * @retval k_ra8_err_null_ptr      `out` was NULL.
 * @retval k_ra8_err_invalid_state The card was never initialised.
 *
 * @pre ::ra8_sdcard_init has returned ::k_ra8_ok.
 * @pre `out` is writable.
 * @post On success `*out` describes a zero-erase, read-write SD medium.
 * @post No card or backend state is mutated.
 *
 * @note Thread-safe (pure read of card-reported capacity).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdhi_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  uint32_t        blocks = 0;
  const ra8_err_t cap    = ra8_sdcard_get_capacity(&blocks);
  if (cap != k_ra8_ok) {
    return cap;
  }
  out->block_count             = blocks;
  out->erase_unit_blocks       = (uint32_t)k_ra8_io_sdhi_erase_unit_blocks;
  out->program_size_bytes      = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes     = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value             = (uint8_t)k_ra8_io_erase_value_zero;
  out->must_erase_before_write = false;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief Native-SDHI backend vtable. `erase`/`sync` are NULL (no host erase, no buffering). */
static const ra8_io_blockdev_iface_t k_sdhi_iface = {
  .read     = sdhi_read,
  .write    = sdhi_write,
  .erase    = nullptr,
  .get_caps = sdhi_get_caps,
  .sync     = nullptr,
};

ra8_err_t ra8_io_blockdev_sdhi_init(ra8_io_blockdev_t* bd)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  bd->iface = &k_sdhi_iface;
  bd->ctx   = nullptr;
  return k_ra8_ok;
}

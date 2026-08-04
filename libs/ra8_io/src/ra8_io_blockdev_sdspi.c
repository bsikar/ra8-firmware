/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_sdspi.c
 * @brief Micro-SD-over-SPI block-device backend -- thin shim over ra8_sdmmc_spi.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_blockdev_iface by forwarding to the `ra8_sdmmc_spi`
 * singleton driver. The driver owns all card state, so this backend keeps no
 * context: the read primitive forwards to the bulk CMD18 path, the write
 * primitive forwards to the bulk CMD25 path, erase forwards to the driver's
 * verified-zero CMD32/CMD33/CMD38 sequence, and capabilities are derived from
 * the driver's reported block count. No raw MMIO is touched here; the driver
 * carries every HUM citation.
 */

#include "ra8_io_blockdev_sdspi.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "ra8_sdmmc_spi.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_sdspi";

/**
 * @enum ra8_io_sdspi_const_t
 * @brief SD-SPI backend layout constants.
 *
 * @details
 * The SD card erases a logical block at a time from the fabric's point of view
 * and programs in whole 512-byte blocks, so the erase unit and program size are
 * fixed singletons here.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_sdspi_erase_unit_blocks = 1, /**< One logical block per erase unit. */
} ra8_io_sdspi_const_t;

/**
 * @brief SD-SPI backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * Forwards to the driver's bulk CMD18 path, which streams every block in a
 * single READ_MULTIPLE_BLOCK transaction terminated by CMD12 (falling back
 * to CMD17 for a single block).
 *
 * @param[in]  ctx   Unused (the driver is a singleton).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks read into `buf`.
 * @retval k_ra8_err_null_ptr     `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past the card capacity.
 *
 * @pre The card is initialised via `ra8_sdmmc_spi_init`.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the card contents.
 * @post On failure `buf` holds at most the blocks streamed before the error.
 *
 * @note Not thread-safe with respect to the driver singleton.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdspi_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra8_sdmmc_spi_read_blocks(lba, buf, count);
}

/**
 * @brief SD-SPI backend: write `count` blocks from `buf` at `lba`.
 *
 * @details
 * Forwards to the driver's bulk CMD25 path, which streams every block in a
 * single transaction (falling back to CMD24 for a single block).
 *
 * @param[in] ctx   Unused (the driver is a singleton).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks written.
 * @retval k_ra8_err_null_ptr     `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past the card capacity.
 *
 * @pre The card is initialised via `ra8_sdmmc_spi_init`.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the card blocks `[lba, lba+count)` equal `buf`.
 * @post On failure the card is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the driver singleton.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdspi_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  return ra8_sdmmc_spi_write_blocks(lba, buf, count);
}

/**
 * @brief SD-SPI backend: erase `count` blocks at `lba` to all-zero.
 *
 * @details
 * Forwards to the driver's CMD32/CMD33/CMD38 sequence, which verifies the range
 * reads back as `0x00` (returning ::k_ra8_err_not_supported on a ones-erasing
 * card so the caller can fall back to writing zeros).
 *
 * @param[in] ctx   Unused (the driver is a singleton).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to erase.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Range erased and verified to read back zero.
 * @retval k_ra8_err_invalid_arg   `count` was zero.
 * @retval k_ra8_err_out_of_range  Range past the card capacity.
 * @retval k_ra8_err_not_supported Card erases to a non-zero value.
 *
 * @pre The card is initialised via `ra8_sdmmc_spi_init`.
 * @pre `count` is non-zero for any observable effect.
 * @post On success the range reads back as 0x00.
 * @post On failure the card is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the driver singleton.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdspi_erase(void* ctx, uint32_t lba, uint32_t count)
{
  (void)ctx;
  return ra8_sdmmc_spi_erase_blocks(lba, count);
}

/**
 * @brief SD-SPI backend: report card capabilities.
 *
 * @details
 * Queries the driver for the 512-byte block count and fills a fixed capability
 * snapshot: one-block erase unit, 512-byte program size, zero erase value, and
 * no erase-before-write requirement (the card programs blocks directly).
 *
 * @param[in]  ctx Unused (the driver is a singleton).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated.
 * @retval k_ra8_err_null_ptr `out` was NULL.
 *
 * @pre The card is initialised via `ra8_sdmmc_spi_init`.
 * @pre `out` is writable.
 * @post On success `*out` describes a zero-erase 512-byte block device.
 * @post No driver or handle state is mutated.
 *
 * @note Not thread-safe with respect to the driver singleton.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t sdspi_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  uint32_t blocks = 0;
  RA8_RETURN_ON_ERROR(ra8_sdmmc_spi_get_capacity(&blocks), s_tag, "sd capacity");
  out->block_count             = blocks;
  out->erase_unit_blocks       = (uint32_t)k_ra8_io_sdspi_erase_unit_blocks;
  out->program_size_bytes      = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes     = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value             = (uint8_t)k_ra8_io_erase_value_zero;
  out->must_erase_before_write = false;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief SD-SPI backend vtable. `sync` is NULL: the driver writes synchronously. */
static const ra8_io_blockdev_iface_t k_sdspi_iface = {
  .read     = sdspi_read,
  .write    = sdspi_write,
  .erase    = sdspi_erase,
  .get_caps = sdspi_get_caps,
  .sync     = nullptr,
};

ra8_err_t ra8_io_blockdev_sdspi_init(ra8_io_blockdev_t* bd)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  bd->iface = &k_sdspi_iface;
  bd->ctx   = nullptr;
  return k_ra8_ok;
}

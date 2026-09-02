/**
 * @file ra8_io_blockdev_xspi.c
 * @brief OSPI NOR block-device backend -- 512-byte LBAs over ra8_xspi flash.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implementation of ::ra8_io_blockdev_iface over the `ra8_xspi` HAL. Reads chunk
 * `ra8_xspi_flash_read` to the controller's per-transfer limit. Arbitrary writes
 * are correct on NOR's erase-then-program semantics via a whole-sector
 * read-modify-write through a module-static 4 KiB scratch buffer: read the
 * sector, overlay the touched 512-byte blocks, erase the sector, and program
 * the 4 KiB back (chunked). Erases are sector-aligned. The file touches no MMIO;
 * every flash access routes through the HAL, which owns the HUM citations.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_blockdev_xspi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_xspi";

/**
 * @enum ra8_io_xspi_const_t
 * @brief OSPI NOR backend layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xspi_sector_bytes      = 4096, /**< Erase sector size in bytes.            */
  k_xspi_blocks_per_sector = 8,    /**< 512-byte blocks per 4 KiB sector.      */
  k_xspi_program_bytes     = 256,  /**< NOR page-program granularity in bytes. */
  k_xspi_zero_blocks       = 0,    /**< Sentinel for an empty block range.     */
  k_xspi_min_instance      = 0,    /**< Lowest valid xSPI instance index.      */
} ra8_io_xspi_const_t;

/** @brief Whole-sector read-modify-write scratch (zero-malloc, NASA Rule 3). */
static uint8_t s_sector[k_xspi_sector_bytes];

/**
 * @brief Reject an out-of-range `[lba, lba+count)` block range.
 *
 * @details
 * Split into two single-condition checks (no compound decision) so the bounds
 * test needs no MC/DC vectors and never underflows `block_count - count`.
 *
 * @param[in] st    xSPI backend state.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks in the range.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Range lies within the device.
 * @retval k_ra8_err_out_of_range Range extends past the device capacity.
 *
 * @pre `st` is non-NULL.
 * @pre `st->block_count` reflects the flash window size.
 * @post No state is mutated.
 * @post The return reflects only the range/capacity comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_xspi_bounds(const ra8_io_blockdev_xspi_state_t* st, uint32_t lba, uint32_t count)
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
 * @brief Read `len` flash bytes at `flash_addr`, chunked to the HAL limit.
 *
 * @details
 * Splits the request into transfers of at most `k_ra8_xspi_max_xfer` bytes so
 * every `ra8_xspi_flash_read` call stays within the controller's per-transfer
 * window. Stops and returns the first non-ok HAL status.
 *
 * @param[in]  instance   xSPI HAL instance index.
 * @param[in]  flash_addr Absolute flash byte offset to read from.
 * @param[out] buf        Destination buffer (>= `len` bytes).
 * @param[in]  len        Number of bytes to read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               All bytes read.
 * @retval k_ra8_err_null_ptr     `buf` invalid or `instance` out of range.
 * @retval k_ra8_err_invalid_arg  HAL rejected a chunk length.
 *
 * @pre `buf` is writable for `len` bytes.
 * @pre The controller was initialised via `ra8_xspi_init`.
 * @post On success `buf[0 .. len)` mirrors the flash window.
 * @post On failure the read stops at the first failing chunk.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_xspi_read_chunked(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  uint32_t done = k_xspi_zero_blocks;
  while (done < len) {
    const uint32_t remaining = len - done;
    const uint32_t chunk =
      (remaining > (uint32_t)k_ra8_xspi_max_xfer) ? (uint32_t)k_ra8_xspi_max_xfer : remaining;
    const ra8_err_t e = ra8_xspi_flash_read(instance, flash_addr + done, &buf[done], chunk);
    if (e != k_ra8_ok) {
      return e;
    }
    done += chunk;
  }
  return k_ra8_ok;
}

/**
 * @brief Program `len` flash bytes at `flash_addr`, chunked to the HAL limit.
 *
 * @details
 * Splits the request into transfers of at most `k_ra8_xspi_max_xfer` bytes so
 * every `ra8_xspi_flash_program` call stays within the controller window. The
 * target sector must already have been erased to all-ones by the caller.
 *
 * @param[in] instance   xSPI HAL instance index.
 * @param[in] flash_addr Absolute flash byte offset to program.
 * @param[in] data       Source buffer (>= `len` bytes).
 * @param[in] len        Number of bytes to program.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               All bytes programmed.
 * @retval k_ra8_err_null_ptr     `data` invalid or `instance` out of range.
 * @retval k_ra8_err_invalid_arg  HAL rejected a chunk length.
 * @retval k_ra8_err_timeout      Flash WIP bit never cleared.
 *
 * @pre `data` is readable for `len` bytes.
 * @pre The covering sector was erased before this call.
 * @post On success the flash window equals `data`.
 * @post On failure the program stops at the first failing chunk.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_xspi_program_chunked(uint8_t        instance,
                                               uint32_t       flash_addr,
                                               const uint8_t* data,
                                               uint32_t       len)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  uint32_t done = k_xspi_zero_blocks;
  while (done < len) {
    const uint32_t remaining = len - done;
    const uint32_t chunk =
      (remaining > (uint32_t)k_ra8_xspi_max_xfer) ? (uint32_t)k_ra8_xspi_max_xfer : remaining;
    const ra8_err_t e = ra8_xspi_flash_program(instance, flash_addr + done, &data[done], chunk);
    if (e != k_ra8_ok) {
      return e;
    }
    done += chunk;
  }
  return k_ra8_ok;
}

/**
 * @brief Read-modify-write one 4 KiB sector, overlaying the touched blocks.
 *
 * @details
 * Reads the whole sector at `sector_lba` into ::s_sector, overlays the
 * `n_blocks` logical blocks starting at offset `first_blk` within the sector
 * from `src`, erases the sector to all-ones, then programs the 4 KiB back.
 * Keeps NOR's erase-before-program contract correct for arbitrary writes.
 *
 * @param[in] st        xSPI backend state.
 * @param[in] sector_lba First logical block of the sector (multiple of 8).
 * @param[in] first_blk Index of the first touched block within the sector.
 * @param[in] n_blocks  Number of consecutive blocks to overlay.
 * @param[in] src       Source bytes for the overlaid blocks.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Sector rewritten with the overlay applied.
 * @retval k_ra8_err_null_ptr     `st`, `src`, or a HAL pointer was NULL.
 * @retval k_ra8_err_invalid_arg  HAL rejected a transfer length.
 * @retval k_ra8_err_timeout      Flash WIP bit never cleared.
 *
 * @pre `st` is a populated xSPI state.
 * @pre `first_blk + n_blocks <= k_xspi_blocks_per_sector`.
 * @post On success the sector equals its prior contents with the overlay.
 * @post On failure no further chunks are issued past the first error.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_write_one_sector(const ra8_io_blockdev_xspi_state_t* st,
                                                        uint32_t       sector_lba,
                                                        uint32_t       first_blk,
                                                        uint32_t       n_blocks,
                                                        const uint8_t* src)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  const uint32_t  sec_addr = st->base_off + (sector_lba * (uint32_t)k_ra8_io_block_size_bytes);
  const ra8_err_t r =
    internal_xspi_read_chunked(st->instance, sec_addr, s_sector, (uint32_t)k_xspi_sector_bytes);
  if (r != k_ra8_ok) {
    return r;
  }
  const size_t dst_off = (size_t)first_blk * (size_t)k_ra8_io_block_size_bytes;
  const size_t n_bytes = (size_t)n_blocks * (size_t)k_ra8_io_block_size_bytes;
  (void)memcpy(&s_sector[dst_off], src, n_bytes);
  const ra8_err_t e = ra8_xspi_flash_erase_sector(st->instance, sec_addr);
  if (e != k_ra8_ok) {
    return e;
  }
  return internal_xspi_program_chunked(st->instance,
                                       sec_addr,
                                       s_sector,
                                       (uint32_t)k_xspi_sector_bytes);
}

/**
 * @brief xSPI backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * Bounds-checks the range, then reads `count * 512` flash bytes in HAL-sized
 * chunks from `base_off + lba * 512`.
 *
 * @param[in]  ctx   xSPI backend state (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blocks read into `buf`.
 * @retval k_ra8_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range Range past capacity.
 * @retval k_ra8_err_invalid_arg  HAL rejected a chunk length.
 *
 * @pre `ctx` is a populated xSPI state.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` mirrors the flash window.
 * @post On failure `buf` may hold a partial prefix only.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_xspi_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_xspi_state_t* st = (const ra8_io_blockdev_xspi_state_t*)ctx;
  const ra8_err_t                     b  = internal_xspi_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  const uint32_t addr = st->base_off + (lba * (uint32_t)k_ra8_io_block_size_bytes);
  const uint32_t len  = count * (uint32_t)k_ra8_io_block_size_bytes;
  return internal_xspi_read_chunked(st->instance, addr, buf, len);
}

/**
 * @brief xSPI backend: write `count` blocks from `buf` at `lba`.
 *
 * @details
 * Iterates the 4 KiB sectors the `[lba, lba+count)` range spans and rewrites
 * each via ::internal_write_one_sector (whole-sector read-modify-write). Rejects the
 * call up front when the device is read-only or the range exceeds capacity.
 *
 * @param[in] ctx   xSPI backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks committed to flash.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 * @retval k_ra8_err_invalid_arg   HAL rejected a transfer length.
 * @retval k_ra8_err_timeout       Flash WIP bit never cleared.
 *
 * @pre `ctx` is a populated xSPI state.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success flash blocks `[lba, lba+count)` equal `buf`.
 * @post On failure earlier sectors may already be rewritten.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_xspi_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_xspi_state_t* st = (const ra8_io_blockdev_xspi_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t b = internal_xspi_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  uint32_t done = k_xspi_zero_blocks;
  while (done < count) {
    const uint32_t  blk        = lba + done;
    const uint32_t  first_blk  = blk % (uint32_t)k_xspi_blocks_per_sector;
    const uint32_t  sector_lba = blk - first_blk;
    const uint32_t  room       = (uint32_t)k_xspi_blocks_per_sector - first_blk;
    const uint32_t  left       = count - done;
    const uint32_t  n_blocks   = (left < room) ? left : room;
    const uint8_t*  src        = &buf[(size_t)done * (size_t)k_ra8_io_block_size_bytes];
    const ra8_err_t e = internal_write_one_sector(st, sector_lba, first_blk, n_blocks, src);
    if (e != k_ra8_ok) {
      return e;
    }
    done += n_blocks;
  }
  return k_ra8_ok;
}

/**
 * @brief xSPI backend: erase sector-aligned `count` blocks at `lba`.
 *
 * @details
 * Requires `lba` and `count` to be multiples of ::k_xspi_blocks_per_sector
 * (eight blocks per 4 KiB sector); a non-aligned request is rejected. Each
 * spanned sector is erased to all-ones via `ra8_xspi_flash_erase_sector`.
 *
 * @param[in] ctx   xSPI backend state (as a void cookie).
 * @param[in] lba   First logical block address (multiple of 8).
 * @param[in] count Number of blocks to erase (multiple of 8).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Range erased to all-ones.
 * @retval k_ra8_err_null_ptr      `ctx` was NULL.
 * @retval k_ra8_err_not_supported The device is read-only.
 * @retval k_ra8_err_invalid_arg   Range is not sector-aligned.
 * @retval k_ra8_err_out_of_range  Range past capacity.
 * @retval k_ra8_err_timeout       Flash WIP bit never cleared.
 *
 * @pre `ctx` is a populated xSPI state.
 * @pre `lba` and `count` are multiples of eight blocks.
 * @post On success every spanned sector reads back as 0xFF.
 * @post On failure earlier sectors may already be erased.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
/* cppcheck-suppress constParameterCallback -- ra8_io_blockdev_ops_t fixes this callback's context type as void*. */
static ra8_err_t internal_xspi_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  const ra8_io_blockdev_xspi_state_t* st = (const ra8_io_blockdev_xspi_state_t*)ctx;
  if (st->read_only) {
    return k_ra8_err_not_supported;
  }
  if ((lba % (uint32_t)k_xspi_blocks_per_sector) != k_xspi_zero_blocks) {
    return k_ra8_err_invalid_arg;
  }
  if ((count % (uint32_t)k_xspi_blocks_per_sector) != k_xspi_zero_blocks) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t b = internal_xspi_bounds(st, lba, count);
  if (b != k_ra8_ok) {
    return b;
  }
  uint32_t blk = lba;
  while (blk < lba + count) {
    const uint32_t  addr = st->base_off + (blk * (uint32_t)k_ra8_io_block_size_bytes);
    const ra8_err_t e    = ra8_xspi_flash_erase_sector(st->instance, addr);
    if (e != k_ra8_ok) {
      return e;
    }
    blk += (uint32_t)k_xspi_blocks_per_sector;
  }
  return k_ra8_ok;
}

/**
 * @brief xSPI backend: report medium capabilities.
 *
 * @details
 * Reports an all-ones-erase NOR medium that must be erased before programming,
 * with an 8-block (4 KiB) erase unit.
 *
 * @param[in]  ctx xSPI backend state (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated.
 * @retval k_ra8_err_null_ptr `ctx` or `out` was NULL.
 *
 * @pre `ctx` is a populated xSPI state.
 * @pre `out` is writable.
 * @post On success `*out` describes an all-ones-erase, erase-required medium.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_xspi_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_io_blockdev_xspi_state_t* st = (const ra8_io_blockdev_xspi_state_t*)ctx;
  out->block_count                       = st->block_count;
  out->erase_unit_blocks                 = (uint32_t)k_xspi_blocks_per_sector;
  out->program_size_bytes                = (uint32_t)k_xspi_program_bytes;
  out->logical_block_bytes               = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value                       = (uint8_t)k_ra8_io_erase_value_ones;
  out->must_erase_before_write           = true;
  out->read_only                         = st->read_only;
  return k_ra8_ok;
}

/** @brief OSPI NOR backend vtable. `sync` is NULL: programs commit on return. */
static const ra8_io_blockdev_iface_t s_xspi_iface = {
  .read     = internal_xspi_read,
  .write    = internal_xspi_write,
  .erase    = internal_xspi_erase,
  .get_caps = internal_xspi_get_caps,
  .sync     = nullptr,
};

/**
 * @brief Validate the window geometry for ::ra8_io_blockdev_xspi_init.
 *
 * @details
 * Split single-condition checks (no compound decision) covering instance
 * range, sector-aligned base offset, and a non-zero whole-sector block count.
 *
 * @param[in] instance    xSPI HAL instance index.
 * @param[in] base_off    Flash byte offset of logical block 0.
 * @param[in] block_count Number of 512-byte logical blocks.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Geometry is valid.
 * @retval k_ra8_err_invalid_arg Some constraint was violated.
 *
 * @pre `instance`, `base_off`, `block_count` are the caller's raw arguments.
 * @pre No device state has yet been bound.
 * @post No state is mutated.
 * @post The return reflects only the geometry constraints.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_xspi_check_geom(uint8_t instance, uint32_t base_off, uint32_t block_count)
{
  if (instance >= (uint8_t)k_ra8_xspi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  if ((base_off % (uint32_t)k_xspi_sector_bytes) != k_xspi_zero_blocks) {
    return k_ra8_err_invalid_arg;
  }
  if (block_count == k_xspi_zero_blocks) {
    return k_ra8_err_invalid_arg;
  }
  if ((block_count % (uint32_t)k_xspi_blocks_per_sector) != k_xspi_zero_blocks) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_blockdev_xspi_init(ra8_io_blockdev_t*            bd,
                                    ra8_io_blockdev_xspi_state_t* state,
                                    uint8_t                       instance,
                                    uint32_t                      base_off,
                                    uint32_t                      block_count,
                                    bool                          read_only)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  const ra8_err_t g = internal_xspi_check_geom(instance, base_off, block_count);
  if (g != k_ra8_ok) {
    return g;
  }
  state->instance    = instance;
  state->base_off    = base_off;
  state->block_count = block_count;
  state->read_only   = read_only;
  bd->iface          = &s_xspi_iface;
  bd->ctx            = state;
  return k_ra8_ok;
}

static_assert((uint32_t)k_xspi_sector_bytes ==
                ((uint32_t)k_xspi_blocks_per_sector * (uint32_t)k_ra8_io_block_size_bytes),
              "sector must equal blocks_per_sector * logical block size");

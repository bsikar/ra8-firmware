/**
 * @file port/levelx/src/lx_fs_backend.c
 * @brief ra8_fs block-device trampolines onto LevelX wear-levelled NOR storage
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``lx_fs_backend_bind()``: three ::ra8_fs_backend_t callbacks
 * (``read_block`` / ``write_block`` / ``get_capacity``) that forward each
 * 512-byte block request into one ``lx_nor_flash_sector_read`` /
 * ``lx_nor_flash_sector_write`` call against the bound ``LX_NOR_FLASH``.
 *
 * The adapter is intentionally thin: LevelX's 512-byte logical sector is also
 * the smallest ``ra8_fs`` sector size, so each block maps 1:1 onto one LevelX
 * call. No sector cache lives here; ``ra8_fs`` (its FAT cache and sector
 * arena) and LevelX (its mapping cache) bring their own.
 *
 * Two translation details carry the semantics:
 *
 * - ``LX_SECTOR_NOT_FOUND`` on a read means LevelX has never mapped that
 *   logical sector -- it is erased space, not an I/O failure -- so the
 *   caller's buffer is filled with the NOR erased value ``0xFF`` and the read
 *   succeeds, exactly as a raw block device serves erased flash.
 * - LevelX requires ``ULONG``-aligned sector buffers; ``ra8_fs`` hands over
 *   plain byte pointers. Every transfer bounces through one static aligned
 *   sector buffer, which is safe because ``ra8_fs`` serialises all backend
 *   calls (see ``ra8_fs_set_lock()``).
 *
 * @author Brighton Sikarskie
 * @date 2026-08-10
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "lx_fs_backend.h"

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"

/** @brief Module log tag. */
static const char* const s_tag = "lx_fs_be";

/**
 * @enum lx_fs_backend_geometry_t
 * @brief Fixed LevelX <-> ra8_fs sector geometry.
 *
 * @details A LevelX logical sector is ``LX_NOR_SECTOR_SIZE`` ULONG words =
 * 512 bytes on every platform (the word COUNT varies with ``sizeof(ULONG)``
 * -- 128 on the 32-bit target, 64 on an LP64 test host -- the byte size does
 * not), and 512 bytes is also the smallest block size ``ra8_fs`` accepts. The
 * erased byte is what NOR flash reads back after erase, served for
 * never-mapped sectors.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lx_fsbe_sector_bytes = 512U,  /**< LevelX logical sector size in bytes.   */
  k_lx_fsbe_erased_byte  = 0xFFU, /**< NOR erased value for unmapped sectors. */
} lx_fs_backend_geometry_t;

static_assert((uint32_t)LX_NOR_SECTOR_SIZE * (uint32_t)sizeof(ULONG) ==
                (uint32_t)k_lx_fsbe_sector_bytes,
              "LevelX logical sector must be 512 bytes");

/**
 * @brief One-sector ``ULONG``-aligned bounce buffer for LevelX transfers.
 *
 * @details LevelX's sector calls take ``ULONG*`` and perform word accesses;
 * ``ra8_fs`` supplies byte pointers with no alignment promise. Every read and
 * write stages through this buffer instead of casting the caller's pointer.
 *
 * @note Serialised by ``ra8_fs`` itself: backend callbacks only run inside a
 *       public ``ra8_fs`` entry point, which the library serialises (its lock
 *       seam under an RTOS, the single-threaded contract otherwise).
 * @warning Not for use outside the two transfer trampolines in this TU.
 * @since 0.1.0
 */
static ULONG s_lx_fsbe_bounce[LX_NOR_SECTOR_SIZE];

/**
 * @brief FAT-usable 512-byte sector count of an open LevelX partition.
 *
 * @details ``lx_nor_flash_physical_sectors_per_block`` already excludes the
 * one physical sector per block LevelX reserves for its mapping metadata
 * (``lx_nor_flash_open`` computes ``words_per_block / LX_NOR_SECTOR_SIZE -
 * 1``). This helper subtracts ONE MORE sector per block as wear-levelling
 * headroom, so LevelX always has free physical sectors to remap writes into
 * even when the filesystem has touched every logical sector it was offered --
 * the same margin the retired FileX adapter gave ``fx_media_format``. A flash
 * that is not open (or whose driver never programmed the geometry) reports
 * zero.
 *
 * @param[in] nor_flash LevelX control block to size. Must not be NULL.
 *
 * @return Number of usable 512-byte sectors.
 * @retval 0 @p nor_flash is not open or its geometry is not programmed.
 *
 * @pre @p nor_flash is non-NULL (callers have already null-checked it).
 * @pre @p nor_flash geometry fields are stable for the duration of the call.
 * @post No state is mutated; pure computation over the control block.
 * @post The result is 0 unless `physical_sectors_per_block > 1`.
 *
 * @note Thread-safe (pure read).
 * @since 0.1.0
 */
static uint64_t priv_usable_sectors(const LX_NOR_FLASH* nor_flash)
{
  uint64_t phys_per_block = (uint64_t)nor_flash->lx_nor_flash_physical_sectors_per_block;
  if (phys_per_block <= 1U) {
    return 0U;
  }
  uint64_t total_blocks = (uint64_t)nor_flash->lx_nor_flash_total_blocks;
  return total_blocks * (phys_per_block - 1U);
}

/**
 * @brief ``read_block`` trampoline: LevelX sector reads into a byte buffer.
 *
 * @details One ``lx_nor_flash_sector_read`` per block, staged through the
 * aligned bounce buffer. ``LX_SECTOR_NOT_FOUND`` (a logical sector LevelX has
 * never mapped) fills that block with ``0xFF`` and continues -- unmapped
 * wear-levelled NOR is erased space, and a block device serves erased space,
 * it does not error on it. Any other LevelX failure aborts the request.
 *
 * @param[in]  ctx   Bound ``LX_NOR_FLASH*``.
 * @param[in]  lba   First logical 512-byte sector to read.
 * @param[in]  count Number of consecutive sectors to read.
 * @param[out] buf   Destination buffer (>= ``count * 512`` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks read (or served as erased fill).
 * @retval k_ra8_err_null_ptr      @p ctx or @p buf was NULL.
 * @retval k_ra8_err_out_of_range  ``lba + count`` exceeds the usable window.
 * @retval k_ra8_err_hw_init_failed LevelX reported an I/O failure.
 *
 * @pre @p ctx points at an open LevelX flash.
 * @pre @p buf is writable for ``count * 512`` bytes.
 * @post On ::k_ra8_ok, @p buf holds ``count`` sectors of data or erased fill.
 * @post On any non-ok return the flash is unchanged.
 *
 * @note Not thread-safe; serialised by ``ra8_fs`` (see the bounce buffer note).
 * @since 0.1.0
 */
static ra8_err_t priv_read_block(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  LX_NOR_FLASH* nor_flash = ctx;
  if ((lba + (uint64_t)count) > priv_usable_sectors(nor_flash)) {
    return k_ra8_err_out_of_range;
  }
  for (uint32_t i = 0U; i < count; i++) {
    uint8_t* dst = &buf[(size_t)i * (size_t)k_lx_fsbe_sector_bytes];
    UINT     rc = lx_nor_flash_sector_read(nor_flash, (ULONG)(lba + (uint64_t)i), s_lx_fsbe_bounce);
    if (rc == (UINT)LX_SECTOR_NOT_FOUND) {
      (void)memset(dst, (int)k_lx_fsbe_erased_byte, (size_t)k_lx_fsbe_sector_bytes);
    } else if (rc != (UINT)LX_SUCCESS) {
      return k_ra8_err_hw_init_failed;
    } else {
      (void)memcpy(dst, s_lx_fsbe_bounce, (size_t)k_lx_fsbe_sector_bytes);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief ``write_block`` trampoline: byte buffer into LevelX sector writes.
 *
 * @details One ``lx_nor_flash_sector_write`` per block, staged through the
 * aligned bounce buffer. LevelX handles the mark-old-obsolete / pick-new-
 * physical-sector dance internally -- that is the wear-levelling this stack
 * exists for.
 *
 * @param[in] ctx   Bound ``LX_NOR_FLASH*``.
 * @param[in] lba   First logical 512-byte sector to write.
 * @param[in] count Number of consecutive sectors to write.
 * @param[in] buf   Source buffer (>= ``count * 512`` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Blocks written.
 * @retval k_ra8_err_null_ptr       @p ctx or @p buf was NULL.
 * @retval k_ra8_err_out_of_range   ``lba + count`` exceeds the usable window.
 * @retval k_ra8_err_hw_init_failed LevelX reported an I/O failure.
 *
 * @pre @p ctx points at an open LevelX flash.
 * @pre @p buf is readable for ``count * 512`` bytes.
 * @post On ::k_ra8_ok every written sector reads back identically.
 * @post On any non-ok return sectors before the failing one may be written.
 *
 * @note Not thread-safe; serialised by ``ra8_fs`` (see the bounce buffer note).
 * @since 0.1.0
 */
static ra8_err_t priv_write_block(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  LX_NOR_FLASH* nor_flash = ctx;
  if ((lba + (uint64_t)count) > priv_usable_sectors(nor_flash)) {
    return k_ra8_err_out_of_range;
  }
  for (uint32_t i = 0U; i < count; i++) {
    const uint8_t* src = &buf[(size_t)i * (size_t)k_lx_fsbe_sector_bytes];
    (void)memcpy(s_lx_fsbe_bounce, src, (size_t)k_lx_fsbe_sector_bytes);
    UINT rc = lx_nor_flash_sector_write(nor_flash, (ULONG)(lba + (uint64_t)i), s_lx_fsbe_bounce);
    if (rc != (UINT)LX_SUCCESS) {
      return k_ra8_err_hw_init_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief ``get_capacity`` trampoline: report the FAT-usable LevelX window.
 *
 * @details Answers ``ra8_fs``'s size query from the bound control block:
 * ::priv_usable_sectors for the block count (LevelX's usable window minus one
 * spare sector per block of wear-levelling headroom) and the fixed 512-byte
 * LevelX logical sector for the block size. A zero window means the flash was
 * never opened, which is reported as an error rather than a zero-sector disk.
 *
 * @param[in]  ctx         Bound ``LX_NOR_FLASH*``.
 * @param[out] block_count Receives the usable 512-byte sector count.
 * @param[out] block_size  Receives the block size (always 512).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Outputs populated.
 * @retval k_ra8_err_null_ptr        Any argument was NULL.
 * @retval k_ra8_err_not_initialized The flash reports zero usable sectors.
 *
 * @pre @p ctx points at an open LevelX flash.
 * @pre @p block_count and @p block_size are writable.
 * @post On ::k_ra8_ok, ``*block_count > 0`` and ``*block_size == 512``.
 * @post No flash state is mutated.
 *
 * @note Thread-safe (pure read of the control block).
 * @since 0.1.0
 */
static ra8_err_t priv_get_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(block_count, s_tag, "block_count");
  RA8_CHECK_NULL_PTR(block_size, s_tag, "block_size");
  uint64_t usable = priv_usable_sectors(ctx);
  if (usable == 0U) {
    return k_ra8_err_not_initialized;
  }
  *block_count = usable;
  *block_size  = (uint32_t)k_lx_fsbe_sector_bytes;
  return k_ra8_ok;
}

/** @brief Implementation of `lx_fs_backend_bind()` -- fills the three trampolines. */
ra8_err_t lx_fs_backend_bind(LX_NOR_FLASH* nor_flash, ra8_fs_backend_t* out)
{
  RA8_CHECK_NULL_PTR(nor_flash, s_tag, "nor_flash");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  if (priv_usable_sectors(nor_flash) == 0U) {
    return k_ra8_err_not_initialized;
  }
  out->read_block   = priv_read_block;
  out->write_block  = priv_write_block;
  out->get_capacity = priv_get_capacity;
  out->erase_blocks = nullptr; /* NOR erases to ones; ra8_fs wants zero-erase. */
  out->ctx          = nor_flash;
  return k_ra8_ok;
}

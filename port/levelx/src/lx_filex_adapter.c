/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/levelx/src/lx_filex_adapter.c
 * @brief FileX media driver bridging onto LevelX wear-levelled NOR storage
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``fx_media_driver_ra8_levelx`` -- the single FileX
 * media-driver entry point -- by dispatching on
 * ``media->fx_media_driver_request`` into
 * ``lx_nor_flash_sector_read`` / ``lx_nor_flash_sector_write``
 * against a module-private ``LX_NOR_FLASH*`` that the application
 * binds via ``lx_filex_adapter_bind`` before opening the FAT volume.
 *
 * The shim is intentionally thin: each FileX request maps onto a
 * loop of one or more LevelX sector calls. The adapter holds no
 * sector cache of its own; both FileX (its sector cache) and LevelX
 * (its mapping cache) bring their own.
 *
 * Sector size is fixed at 512 bytes -- the LevelX default
 * (``LX_NOR_SECTOR_SIZE`` = 128 ULONGs * 4 = 512 bytes), which is
 * also the FAT12/16/32 sector size FileX uses by default. This 1:1
 * line-up is what makes the shim trivial.
 */

#include "lx_filex_adapter.h"

#include <stdint.h>

/**
 * @brief Compile-time constants for the LevelX <-> FileX bridge.
 */
typedef enum : uint32_t {
  /** @brief LevelX logical sector size in ULONGs (512 bytes / 4). */
  k_lx_filex_sector_words = 128U,

  /** @brief LevelX logical sector size in bytes. */
  k_lx_filex_sector_bytes = 512U,

  /** @brief Boot read / write requests always target sector 0. */
  k_lx_filex_boot_lba = 0U,
} lx_filex_constants_t;

/**
 * @brief Module-private back-end pointer set by ``lx_filex_adapter_bind``.
 *
 * @note Single-threaded init writes this once; FileX serialises
 *       reads via its own ``fx_media_protect`` lock thereafter.
 *
 * @since 0.1.0
 */
static LX_NOR_FLASH* s_nor_flash = LX_NULL;

/**
 * @brief Read ``count`` consecutive 512-byte sectors out of LevelX.
 *
 * @details
 * Walks the destination buffer one sector at a time and issues one
 * ``lx_nor_flash_sector_read`` per sector. Returns early on the first
 * LevelX failure; the partial buffer state is then undefined (FileX
 * treats ``FX_IO_ERROR`` as fatal for the request anyway).
 *
 * @param[in]  start_lba First LevelX logical sector index.
 * @param[in]  count     Number of sectors to read.
 * @param[out] dst       Destination RAM buffer (must be ``ULONG``-aligned).
 *
 * @return ``FX_SUCCESS`` on full success, ``FX_IO_ERROR`` otherwise.
 *
 * @pre ``s_nor_flash != NULL`` and points to an open LevelX flash.
 * @pre ``dst != NULL``.
 *
 * @post On ``FX_SUCCESS`` the destination buffer holds
 *       ``count * 512`` bytes of LevelX-managed data.
 *
 * @note Not thread-safe; FileX serialises driver calls.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 */
static UINT priv_read_sectors(ULONG start_lba, ULONG count, ULONG* dst)
{
  if ((s_nor_flash == LX_NULL) || (dst == LX_NULL)) {
    return FX_IO_ERROR;
  }
  ULONG* cur_buf = dst;
  for (ULONG i = 0U; i < count; i++) {
    UINT lx_status = lx_nor_flash_sector_read(s_nor_flash, start_lba + i, cur_buf);
    if (lx_status != LX_SUCCESS) {
      return FX_IO_ERROR;
    }
    cur_buf += k_lx_filex_sector_words;
  }
  return FX_SUCCESS;
}

/**
 * @brief Write ``count`` consecutive 512-byte sectors into LevelX.
 *
 * @details
 * Walks the source buffer one sector at a time and issues one
 * ``lx_nor_flash_sector_write`` per sector. LevelX handles the
 * write-twice / mark-old-obsolete dance internally and may transparently
 * pick a brand-new physical sector under the hood (this is the whole
 * point of the wear-levelling layer).
 *
 * @param[in]     start_lba First LevelX logical sector index.
 * @param[in]     count     Number of sectors to write.
 * @param[in,out] src       Source RAM buffer (must be ``ULONG``-aligned).
 *                          LevelX takes ``ULONG*`` (non-const) even though
 *                          it treats the buffer as read-only.
 *
 * @return ``FX_SUCCESS`` on full success, ``FX_IO_ERROR`` otherwise.
 *
 * @pre ``s_nor_flash != NULL`` and points to an open LevelX flash.
 * @pre ``src != NULL``.
 *
 * @post On success every written sector reads back identically.
 *
 * @note Not thread-safe; FileX serialises driver calls.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 */
static UINT priv_write_sectors(ULONG start_lba, ULONG count, ULONG* src)
{
  if ((s_nor_flash == LX_NULL) || (src == LX_NULL)) {
    return FX_IO_ERROR;
  }
  ULONG* cur_buf = src;
  for (ULONG i = 0U; i < count; i++) {
    UINT lx_status = lx_nor_flash_sector_write(s_nor_flash, start_lba + i, cur_buf);
    if (lx_status != LX_SUCCESS) {
      return FX_IO_ERROR;
    }
    cur_buf += k_lx_filex_sector_words;
  }
  return FX_SUCCESS;
}

/* Handle ``FX_DRIVER_INIT``: advertise sector geometry to FileX -- see implementation for details. */
static void priv_handle_init(FX_MEDIA* media)
{
  if (s_nor_flash == LX_NULL) {
    media->fx_media_driver_status = FX_IO_ERROR;
    return;
  }
  media->fx_media_driver_write_protect      = FX_FALSE;
  media->fx_media_driver_free_sector_update = FX_FALSE;
  media->fx_media_driver_status             = FX_SUCCESS;
}

/* Handle ``FX_DRIVER_UNINIT``: drop the LevelX binding -- see implementation for details. */
static void priv_handle_uninit(FX_MEDIA* media)
{
  media->fx_media_driver_status = FX_SUCCESS;
}

/* Handle ``FX_DRIVER_READ``: copy ``n`` sectors out of LevelX -- see implementation for details. */
static void priv_handle_read(FX_MEDIA* media)
{
  ULONG lba = (ULONG)media->fx_media_driver_logical_sector;
  ULONG cnt = (ULONG)media->fx_media_driver_sectors;
  /* fx_media_driver_buffer is UCHAR*; FileX guarantees ULONG-aligned
   * sector buffers. Route the cast through uintptr_t so -Wcast-align
   * stays quiet. */
  ULONG* buf                    = (ULONG*)(uintptr_t)media->fx_media_driver_buffer;
  media->fx_media_driver_status = priv_read_sectors(lba, cnt, buf);
}

/* Handle ``FX_DRIVER_WRITE``: push ``n`` sectors into LevelX -- see implementation for details. */
static void priv_handle_write(FX_MEDIA* media)
{
  ULONG  lba                    = (ULONG)media->fx_media_driver_logical_sector;
  ULONG  cnt                    = (ULONG)media->fx_media_driver_sectors;
  ULONG* buf                    = (ULONG*)(uintptr_t)media->fx_media_driver_buffer;
  media->fx_media_driver_status = priv_write_sectors(lba, cnt, buf);
}

/* Handle ``FX_DRIVER_BOOT_READ``: read sector 0 into the FileX buffer -- see implementation for details. */
static void priv_handle_boot_read(FX_MEDIA* media)
{
  ULONG* buf                    = (ULONG*)(uintptr_t)media->fx_media_driver_buffer;
  media->fx_media_driver_status = priv_read_sectors((ULONG)k_lx_filex_boot_lba, 1U, buf);
}

/* Handle ``FX_DRIVER_BOOT_WRITE``: write the FileX buffer to sector 0 -- see implementation for details. */
static void priv_handle_boot_write(FX_MEDIA* media)
{
  ULONG* buf                    = (ULONG*)(uintptr_t)media->fx_media_driver_buffer;
  media->fx_media_driver_status = priv_write_sectors((ULONG)k_lx_filex_boot_lba, 1U, buf);
}

/* Lx filex adapter bind -- see implementation for details. */
UINT lx_filex_adapter_bind(LX_NOR_FLASH* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return FX_PTR_ERROR;
  }
  s_nor_flash = nor_flash;
  return FX_SUCCESS;
}

/* Lx filex adapter get total sectors -- see implementation for details. */
ULONG lx_filex_adapter_get_total_sectors(void)
{
  if (s_nor_flash == LX_NULL) {
    return 0U;
  }
  /* LevelX reserves one physical sector per block for mapping
   * metadata; the rest are user-visible logical sectors. */
  ULONG phys_per_block = s_nor_flash->lx_nor_flash_physical_sectors_per_block;
  if (phys_per_block == 0U) {
    return 0U;
  }
  ULONG total_blocks = s_nor_flash->lx_nor_flash_total_blocks;
  ULONG usable       = (phys_per_block > 1U) ? (phys_per_block - 1U) : 0U;
  return total_blocks * usable;
}

/* Fx media driver ra levelx -- see implementation for details. */
void fx_media_driver_ra8_levelx(FX_MEDIA* media)
{
  /* FileX never invokes the driver with media == NULL, but the FSP
   * media-driver examples still defensively check; we mirror that. */
  if (media == FX_NULL) {
    return;
  }

  switch (media->fx_media_driver_request) {
    case FX_DRIVER_INIT:
      priv_handle_init(media);
      break;

    case FX_DRIVER_UNINIT:
      priv_handle_uninit(media);
      break;

    case FX_DRIVER_READ:
      priv_handle_read(media);
      break;

    case FX_DRIVER_WRITE:
      priv_handle_write(media);
      break;

    case FX_DRIVER_BOOT_READ:
      priv_handle_boot_read(media);
      break;

    case FX_DRIVER_BOOT_WRITE:
      priv_handle_boot_write(media);
      break;

    case FX_DRIVER_FLUSH:
      /* lx_nor_flash_sector_write completes synchronously; nothing to flush. */
      media->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_ABORT:
      /* No outstanding async transfers to cancel in PIO mode. */
      media->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_RELEASE_SECTORS:
      /* TRIM / discard not implemented; report success per FileX
       * convention (the request is purely informational). */
      media->fx_media_driver_status = FX_SUCCESS;
      break;

    default:
      media->fx_media_driver_status = FX_IO_ERROR;
      break;
  }
}

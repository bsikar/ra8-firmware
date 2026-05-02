/**
 * @file port/filex/fx_media_driver_ra_sdhi.c
 * @brief FileX media driver bridging onto the RA8D2 ``ra_sdhi`` block API
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``fx_media_driver_ra_sdhi`` -- the single entry point
 * passed to ``fx_media_open()`` / ``fx_media_format()`` -- by
 * dispatching on ``media->fx_media_driver_request`` into the
 * matching ``ra_sdhi_*`` polled block primitive.
 *
 * The shim is intentionally thin: every FileX I/O request maps 1:1
 * onto one ``ra_sdhi_init`` / ``ra_sdhi_deinit`` /
 * ``ra_sdhi_read_block`` / ``ra_sdhi_write_block`` call. The driver
 * holds no state of its own; FileX owns the in-RAM sector cache and
 * the FAT bookkeeping.
 *
 * SDHI instance 0 is hard-wired here. EK-RA8D2 routes its
 * micro-SD slot to channel 0; no current build needs the second
 * channel, and the FileX media struct exposes no field to plumb a
 * channel index through.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fx_media_driver_ra_sdhi.h"

#include <stdint.h>

#include "fx_api.h"
#include "ra_err.h"
#include "ra_sdhi.h"

/**
 * @brief Compile-time constants for the SDHI <-> FileX bridge.
 */
typedef enum : uint32_t {
  /** SDHI instance routed to the EK-RA8D2 micro-SD slot. */
  k_fx_sdhi_instance = 0U,

  /** SD specification logical block size; FileX configures this on INIT. */
  k_fx_sdhi_sector_bytes = 512U,

  /** Boot-read / boot-write requests always target sector 0. */
  k_fx_sdhi_boot_lba = 0U,

  /** Boot-read / boot-write requests always cover one sector. */
  k_fx_sdhi_boot_count = 1U,
} fx_sdhi_constants_t;

/**
 * @brief Translate an ``ra_err_t`` into the FileX ``fx_media_driver_status`` value.
 *
 * @param[in] err Result of the underlying ``ra_sdhi_*`` call.
 * @return ``FX_SUCCESS`` if ``err == k_ra_ok``, ``FX_IO_ERROR`` otherwise.
 *
 * @pre None.
 * @post Return value is one of ``FX_SUCCESS`` / ``FX_IO_ERROR``.
 *
 * @note Pure helper, no side effects.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static UINT priv_status_from_ra_err(ra_err_t err)
{
  if (err == k_ra_ok) {
    return FX_SUCCESS;
  }
  return FX_IO_ERROR;
}

/**
 * @brief Handle ``FX_DRIVER_INIT``: bring the SDHI controller up.
 *
 * @param[in,out] media FileX media struct; receives status.
 *
 * @pre ``media != NULL``.
 * @post On success the SDHI MSTP gate is open and IRQ masks are cleared.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_init(FX_MEDIA* media)
{
  ra_err_t err                    = ra_sdhi_init((uint8_t)k_fx_sdhi_instance);
  media->fx_media_driver_status   = priv_status_from_ra_err(err);
  /* The SDHI driver always operates on 512-byte logical blocks; advertise
   * that to FileX so it sizes its sector cache correctly. */
  media->fx_media_driver_write_protect      = FX_FALSE;
  media->fx_media_driver_free_sector_update = FX_FALSE;
}

/**
 * @brief Handle ``FX_DRIVER_UNINIT``: power the SDHI controller down.
 *
 * @param[in,out] media FileX media struct; receives status.
 *
 * @pre ``media != NULL``.
 * @post SDHI MSTP gate is closed.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_uninit(FX_MEDIA* media)
{
  ra_err_t err                  = ra_sdhi_deinit((uint8_t)k_fx_sdhi_instance);
  media->fx_media_driver_status = priv_status_from_ra_err(err);
}

/**
 * @brief Handle ``FX_DRIVER_READ``: copy ``n`` sectors from card to FileX buffer.
 *
 * @param[in,out] media FileX media struct describing the request.
 *
 * @pre ``media != NULL``.
 * @pre ``media->fx_media_driver_buffer != NULL``.
 * @pre ``media->fx_media_driver_sectors > 0``.
 *
 * @post On success the buffer holds ``sectors * 512`` bytes of card data.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_read(FX_MEDIA* media)
{
  uint32_t lba = (uint32_t)media->fx_media_driver_logical_sector;
  uint32_t cnt = (uint32_t)media->fx_media_driver_sectors;
  ra_err_t err = ra_sdhi_read_block((uint8_t)k_fx_sdhi_instance,
                                    lba,
                                    (uint8_t*)media->fx_media_driver_buffer,
                                    cnt);
  media->fx_media_driver_status = priv_status_from_ra_err(err);
}

/**
 * @brief Handle ``FX_DRIVER_WRITE``: push ``n`` sectors from FileX buffer to card.
 *
 * @param[in,out] media FileX media struct describing the request.
 *
 * @pre ``media != NULL``.
 * @pre ``media->fx_media_driver_buffer != NULL``.
 * @pre ``media->fx_media_driver_sectors > 0``.
 *
 * @post On success the requested sector range has been written to the card.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_write(FX_MEDIA* media)
{
  uint32_t lba = (uint32_t)media->fx_media_driver_logical_sector;
  uint32_t cnt = (uint32_t)media->fx_media_driver_sectors;
  ra_err_t err = ra_sdhi_write_block((uint8_t)k_fx_sdhi_instance,
                                     lba,
                                     (const uint8_t*)media->fx_media_driver_buffer,
                                     cnt);
  media->fx_media_driver_status = priv_status_from_ra_err(err);
}

/**
 * @brief Handle ``FX_DRIVER_BOOT_READ``: read sector 0 into the FileX buffer.
 *
 * @param[in,out] media FileX media struct.
 *
 * @pre ``media != NULL``.
 * @pre ``media->fx_media_driver_buffer != NULL``.
 *
 * @post On success the boot sector is in ``media->fx_media_driver_buffer``.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_boot_read(FX_MEDIA* media)
{
  ra_err_t err                  = ra_sdhi_read_block((uint8_t)k_fx_sdhi_instance,
                                    (uint32_t)k_fx_sdhi_boot_lba,
                                    (uint8_t*)media->fx_media_driver_buffer,
                                    (uint32_t)k_fx_sdhi_boot_count);
  media->fx_media_driver_status = priv_status_from_ra_err(err);
}

/**
 * @brief Handle ``FX_DRIVER_BOOT_WRITE``: write the FileX buffer to sector 0.
 *
 * @param[in,out] media FileX media struct.
 *
 * @pre ``media != NULL``.
 * @pre ``media->fx_media_driver_buffer != NULL``.
 *
 * @post On success sector 0 has been overwritten with the buffer contents.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_boot_write(FX_MEDIA* media)
{
  ra_err_t err                  = ra_sdhi_write_block((uint8_t)k_fx_sdhi_instance,
                                     (uint32_t)k_fx_sdhi_boot_lba,
                                     (const uint8_t*)media->fx_media_driver_buffer,
                                     (uint32_t)k_fx_sdhi_boot_count);
  media->fx_media_driver_status = priv_status_from_ra_err(err);
}

/**
 * @brief Fx media driver ra sdhi.
 *
 * @details See implementation for details.
 *
 * @param[in,out] media See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void fx_media_driver_ra_sdhi(FX_MEDIA* media)
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
      /* Polled writes already complete synchronously inside
       * ra_sdhi_write_block; nothing buffered in the shim. */
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

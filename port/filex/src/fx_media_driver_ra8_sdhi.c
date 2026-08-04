/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/filex/src/fx_media_driver_ra8_sdhi.c
 * @brief FileX media driver bridging onto the RA8 ``ra8_sdhi`` block API
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``fx_media_driver_ra8_sdhi`` -- the single entry point
 * passed to ``fx_media_open()`` / ``fx_media_format()`` -- by
 * dispatching on ``media->fx_media_driver_request`` into the
 * matching ``ra8_sdhi_*`` polled block primitive.
 *
 * The shim is intentionally thin: every FileX I/O request maps 1:1
 * onto one ``ra8_sdhi_init`` / ``ra8_sdhi_deinit`` /
 * ``ra8_sdhi_read_block`` / ``ra8_sdhi_write_block`` call. The driver
 * holds no state of its own; FileX owns the in-RAM sector cache and
 * the FAT bookkeeping.
 *
 * SDHI instance 0 is hard-wired here. EK-RA8D2 routes its
 * micro-SD slot to channel 0; no current build needs the second
 * channel, and the FileX media struct exposes no field to plumb a
 * channel index through.
 */

#include "fx_media_driver_ra8_sdhi.h"

#include <stdint.h>

#include "fx_api.h"
#include "ra8_err.h"
#include "ra8_sdhi.h"

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

/* Translate an ``ra8_err_t`` into the FileX ``fx_media_driver_status`` value -- see implementation for details. */
static UINT priv_status_from_ra8_err(ra8_err_t err)
{
  if (err == k_ra8_ok) {
    return FX_SUCCESS;
  }
  return FX_IO_ERROR;
}

/* Handle ``FX_DRIVER_INIT``: bring the SDHI controller up -- see implementation for details. */
static void priv_handle_init(FX_MEDIA* media)
{
  ra8_err_t err                 = ra8_sdhi_init((uint8_t)k_fx_sdhi_instance);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
  /* The SDHI driver always operates on 512-byte logical blocks; advertise
   * that to FileX so it sizes its sector cache correctly. */
  media->fx_media_driver_write_protect      = FX_FALSE;
  media->fx_media_driver_free_sector_update = FX_FALSE;
}

/* Handle ``FX_DRIVER_UNINIT``: power the SDHI controller down -- see implementation for details. */
static void priv_handle_uninit(FX_MEDIA* media)
{
  ra8_err_t err                 = ra8_sdhi_deinit((uint8_t)k_fx_sdhi_instance);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
}

/* Handle ``FX_DRIVER_READ``: copy ``n`` sectors from card to FileX buffer -- see implementation for details. */
static void priv_handle_read(FX_MEDIA* media)
{
  uint32_t  lba                 = (uint32_t)media->fx_media_driver_logical_sector;
  uint32_t  cnt                 = (uint32_t)media->fx_media_driver_sectors;
  ra8_err_t err                 = ra8_sdhi_read_block((uint8_t)k_fx_sdhi_instance,
                                                      lba,
                                                      (uint8_t*)media->fx_media_driver_buffer,
                                                      cnt);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
}

/* Handle ``FX_DRIVER_WRITE``: push ``n`` sectors from FileX buffer to card -- see implementation for details. */
static void priv_handle_write(FX_MEDIA* media)
{
  uint32_t  lba = (uint32_t)media->fx_media_driver_logical_sector;
  uint32_t  cnt = (uint32_t)media->fx_media_driver_sectors;
  ra8_err_t err = ra8_sdhi_write_block((uint8_t)k_fx_sdhi_instance,
                                       lba,
                                       (const uint8_t*)media->fx_media_driver_buffer,
                                       cnt);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
}

/* Handle ``FX_DRIVER_BOOT_READ``: read sector 0 into the FileX buffer -- see implementation for details. */
static void priv_handle_boot_read(FX_MEDIA* media)
{
  ra8_err_t err                 = ra8_sdhi_read_block((uint8_t)k_fx_sdhi_instance,
                                                      (uint32_t)k_fx_sdhi_boot_lba,
                                                      (uint8_t*)media->fx_media_driver_buffer,
                                                      (uint32_t)k_fx_sdhi_boot_count);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
}

/* Handle ``FX_DRIVER_BOOT_WRITE``: write the FileX buffer to sector 0 -- see implementation for details. */
static void priv_handle_boot_write(FX_MEDIA* media)
{
  ra8_err_t err = ra8_sdhi_write_block((uint8_t)k_fx_sdhi_instance,
                                       (uint32_t)k_fx_sdhi_boot_lba,
                                       (const uint8_t*)media->fx_media_driver_buffer,
                                       (uint32_t)k_fx_sdhi_boot_count);
  media->fx_media_driver_status = priv_status_from_ra8_err(err);
}

/* Fx media driver ra sdhi -- see implementation for details. */
void fx_media_driver_ra8_sdhi(FX_MEDIA* media)
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
       * ra8_sdhi_write_block; nothing buffered in the shim. */
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

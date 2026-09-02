/**
 * @file
 * examples/ek_ra8d2/hw_pending/manual/usb_msc_sdcard/src/usb_msc_sdcard_steps.c
 * @brief USBX device worker + read/write MSC media callbacks for the live SD card
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of ``main.c`` to keep every translation unit under the line-count
 * cap. Holds the device-side bring-up (USBX system + device stack + a single
 * WRITABLE Mass-Storage LUN + DCD attach) and the media callbacks that stream
 * the live full-capacity Pmod2 SD card:
 *
 *  - ``sdmsc_msc_read`` forwards each SCSI READ(10) run to
 *    ``ra8_sdmmc_spi_read_blocks`` -- one CMD18 (READ_MULTIPLE_BLOCK) streak
 *    per run, CMD17 for single blocks.
 *  - ``sdmsc_msc_write`` forwards each WRITE(10) data chunk to
 *    ``ra8_sdmmc_spi_write_blocks`` -- one CMD25 (WRITE_MULTIPLE_BLOCK) streak
 *    per chunk. The class hands runs of up to
 *    ``UX_SLAVE_CLASS_STORAGE_BUFFER_SIZE`` bytes (4096 = 8 blocks), so a
 *    host file copy passes through as back-to-back CMD25 streaks.
 *
 * WRITE(10) routing: ``_ux_device_class_storage_write`` consults the LUN's
 * ``ux_slave_class_storage_media_read_only_flag`` BEFORE touching the media
 * callback -- the read-only examples (usb_msc_mram, usb_selftest_microsd) set
 * it ``UX_TRUE`` so the class itself answers DATA PROTECT and their
 * media-write hooks never run. This app sets it ``UX_FALSE``: MODE SENSE
 * reports the medium writable (no WP bit) and the class streams the bulk-OUT
 * data phase into ``sdmsc_msc_write``. On ``UX_ERROR`` the class stalls the
 * OUT endpoint, fails the CSW, and serves the sense triple this file stored
 * in ``media_status`` through the host's next REQUEST SENSE.
 *
 * The LUN geometry is latched from ::s_usb_msc_sdcard_blocks (the real card
 * capacity read from the CSD in ``main.c``); no snapshot volume is
 * synthesized anywhere.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-08
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_msc_sdcard_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* -------------------------------------------------------------------------- */
/* USBX pool storage + SCSI INQUIRY strings */
/* -------------------------------------------------------------------------- */

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_sdmsc_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "SDCARD LIVE RW  ";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* J-Link probes (device worker + media-callback owned) */
/* -------------------------------------------------------------------------- */

/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 parked. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;
/** @brief media_read invocations (each is one CMD17/CMD18 streak). */
static volatile uint32_t s_dbg_read_calls;
/** @brief media_write invocations (each is one CMD24/CMD25 streak). */
static volatile uint32_t s_dbg_write_calls;
/** @brief Total 512-B blocks served to the host by media_read. */
static volatile uint32_t s_dbg_read_blocks;
/** @brief Total 512-B blocks committed to the card by media_write. */
static volatile uint32_t s_dbg_write_blocks;
/** @brief First failing SD driver error on the media path (0 = none). */
static volatile uint32_t s_dbg_media_err;

/* -------------------------------------------------------------------------- */
/* USB descriptors (single-interface MSC; SCSI transparent; BBB) */
/* -------------------------------------------------------------------------- */

/* MSC config: bulk-only transport, SCSI command set, EP1 IN + EP2 OUT,
 * 64-byte MPS. PID 0x0019 marks the live-SD read/write MSC identity. */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U,
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x19U, /* PID = 0x0019 (pid.codes test). */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (32 bytes total). */
  0x09U,
  0x02U,
  0x20U,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface descriptor -- MSC, SCSI, BBB. */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x02U,
  0x08U,
  0x06U,
  0x50U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 64-byte MPS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 64-byte MPS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

/**
 * @var s_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Each entry: 2 bytes lang-id, 1 byte string index, 1 byte
 *          length, then ASCII bytes.
 * @since 0.1.0
 */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie". */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "RA8D2 SDCARD RW". */
  0x09U,
  0x04U,
  0x02U,
  0x0FU,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'S',
  'D',
  'C',
  'A',
  'R',
  'D',
  ' ',
  'R',
  'W',
  /* idx 3: serial. */
  0x09U,
  0x04U,
  0x03U,
  0x08U,
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '1',
  '9',
};

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (single writable live-SD LUN) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: stream blocks off the live SD card.
 *
 * @details Bounds-checks the request against the card capacity, then hands
 * the whole run to ``ra8_sdmmc_spi_read_blocks`` -- a single CMD18
 * (READ_MULTIPLE_BLOCK) streak with per-block CRC16 verification, CMD17 for
 * a one-block run. A driver failure reports MEDIUM ERROR / UNRECOVERED READ
 * ERROR so the host retries or surfaces a read error instead of silently
 * corrupting the mount. LED1 toggles per call so traffic is visible.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused; single LUN).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with the sense status word.
 *
 * @return ``UX_SUCCESS`` when the card served the run; else ``UX_ERROR``.
 * @retval UX_SUCCESS Blocks read from the card.
 * @retval UX_ERROR   Out-of-range LBA / count, or the card read failed.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre ``ra8_sdmmc_spi_init`` succeeded at boot (worker gates on capacity).
 * @post Either the blocks were read or media_status carries the sense triple.
 * @post ::s_dbg_read_calls / ::s_dbg_read_blocks advanced.
 *
 * @note Called from the USBX storage class thread -- the only SD-bus user
 *       after boot, so card access needs no extra locking.
 * @since 0.1.0
 */
static UINT sdmsc_msc_read(VOID*  storage,
                           ULONG  lun,
                           UCHAR* data_pointer,
                           ULONG  number_blocks,
                           ULONG  lba,
                           ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)s_usb_msc_sdcard_blocks) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  const ra8_err_t err =
    ra8_sdmmc_spi_read_blocks((uint32_t)lba, data_pointer, (uint32_t)number_blocks);
  if (err != k_ra8_ok) {
    s_dbg_media_err = (uint32_t)err;
    *media_status   = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_medium_error,
                                                           k_scsi_asc_unrecovered_read,
                                                           k_scsi_ascq_none);
    return UX_ERROR;
  }
  s_dbg_read_blocks += (uint32_t)number_blocks;
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: commit host data to the live SD card.
 *
 * @details The LUN is writable, so ``_ux_device_class_storage_write`` streams
 * each WRITE(10) bulk-OUT chunk here after its own read-only-flag and
 * transfer-length checks. Bounds-checks the run against the card capacity,
 * then hands it to ``ra8_sdmmc_spi_write_blocks`` -- one CMD25
 * (WRITE_MULTIPLE_BLOCK) streak per chunk (CMD24 for a one-block run), with
 * the card's data-response token and busy handshake verified per block. On
 * success the class advances its LBA cursor and continues the command; the
 * CSW passes only after every chunk landed. A driver failure reports MEDIUM
 * ERROR / PERIPHERAL DEVICE WRITE FAULT and fails the command so the host
 * knows the sectors did NOT reach the medium. LED2 toggles per call.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused; single LUN).
 * @param[in]     data_pointer USBX-owned source buffer (received OUT data).
 * @param[in]     number_blocks Number of 512-byte blocks in this chunk.
 * @param[in]     lba          Starting LBA of this chunk.
 * @param[out]    media_status Filled with the sense status word.
 *
 * @return ``UX_SUCCESS`` when the card accepted the run; else ``UX_ERROR``.
 * @retval UX_SUCCESS Blocks written to the card.
 * @retval UX_ERROR   Out-of-range LBA / count, or the card write failed.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre The OUT data phase delivered ``number_blocks * 512`` bytes.
 * @post Either the card holds the sectors or media_status carries the triple.
 * @post ::s_dbg_write_calls / ::s_dbg_write_blocks advanced.
 *
 * @note Called from the USBX storage class thread -- the only SD-bus user
 *       after boot, so card access needs no extra locking.
 * @since 0.1.0
 */
static UINT sdmsc_msc_write(VOID*  storage,
                            ULONG  lun,
                            UCHAR* data_pointer,
                            ULONG  number_blocks,
                            ULONG  lba,
                            ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_write_calls++;
  if ((lba + number_blocks) > (ULONG)s_usb_msc_sdcard_blocks) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  const ra8_err_t err =
    ra8_sdmmc_spi_write_blocks((uint32_t)lba, data_pointer, (uint32_t)number_blocks);
  if (err != k_ra8_ok) {
    s_dbg_media_err = (uint32_t)err;
    *media_status   = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_medium_error,
                                                           k_scsi_asc_write_fault,
                                                           k_scsi_ascq_none);
    return UX_ERROR;
  }
  s_dbg_write_blocks += (uint32_t)number_blocks;
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led2);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-status callback. Reports the boot-probed card.
 *
 * @details The card was brought up once at boot and the worker refuses to
 * attach without it, so a live class instance always has its medium.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     media_id     Media id (unused).
 * @param[out]    media_status Filled with 0 (no fault).
 *
 * @return Always ``UX_SUCCESS``.
 * @retval UX_SUCCESS Media is present and ready.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The worker attached only after the card probe succeeded.
 * @post ``*media_status`` is 0.
 * @post No other state changes.
 *
 * @note Card hot-swap is not supported; power-cycle to change cards.
 * @since 0.1.0
 */
static UINT sdmsc_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX MSC with one writable live-SD LUN */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring USBX system + device stack up with the MSC framework.
 *
 * @details One-shot USBX pool + device-stack init (FS-only framework).
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT sdmsc_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_sdmsc_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework_fs,
                                     sizeof(s_device_framework_fs),
                                     s_string_framework,
                                     sizeof(s_string_framework),
                                     s_language_id_framework,
                                     sizeof(s_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Register the Mass-Storage class with one writable live-SD LUN.
 *
 * @details Sets ``number_lun`` = 1 and fills LUN0 with the REAL card
 * geometry: ``last_lba`` = ::s_usb_msc_sdcard_blocks - 1 (CSD-derived
 * capacity, no snapshot), 512-byte blocks, removable, and -- the write
 * enable -- ``media_read_only_flag = UX_FALSE`` so MODE SENSE omits the WP
 * bit and the class routes WRITE(10) into ::sdmsc_msc_write.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::sdmsc_usbx_stack_up has succeeded.
 * @pre ::s_usb_msc_sdcard_blocks is non-zero (worker gated on it).
 * @post MSC class bound to configuration 1, interface 0, with 1 LUN.
 * @post GET_MAX_LUN will report 0.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT sdmsc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)s_usb_msc_sdcard_blocks - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_sdmsc_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_FALSE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    sdmsc_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    sdmsc_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    sdmsc_msc_status;

  static UCHAR s_class_name[] = "ux_slave_class_storage";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Print the READY banner: "usb_msc_sdcard: USB MSC SDCARD READY ...".
 *
 * @details Streams the capacity so the bench log records what the host is
 * about to mount.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The banner is queued.
 *
 * @pre The DCD attach succeeded (DPRPU pulled up).
 * @pre SCI8 init already ran.
 * @post One ASCII banner line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdmsc_print_ready(void)
{
  ra8_err_t err = sdmsc_print("usb_msc_sdcard: USB MSC SDCARD READY (RW, ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = sdmsc_print_dec(s_usb_msc_sdcard_blocks);
  if (err != k_ra8_ok) {
    return err;
  }
  return sdmsc_print(" blocks)\r\n");
}

VOID sdmsc_device_worker(ULONG arg)
{
  (void)arg;

  if (s_usb_msc_sdcard_blocks == 0U) {
    (void)sdmsc_print_fail("no card -- not attaching USB", k_ra8_err_invalid_state);
    return;
  }
  UINT ux = sdmsc_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    (void)sdmsc_print_fail("usbx stack", (ra8_err_t)ux);
    return;
  }
  s_dbg_dev_step = (uint32_t)k_sdmsc_dev_step_stack;
  ux             = sdmsc_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    (void)sdmsc_print_fail("msc class", (ra8_err_t)ux);
    return;
  }
  s_dbg_dev_step = (uint32_t)k_sdmsc_dev_step_class;
  ra8_err_t err  = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)err;
    (void)sdmsc_print_fail("dcd init", err);
    return;
  }
  s_dbg_dev_step = (uint32_t)k_sdmsc_dev_step_dcd;
  err            = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (err != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)err;
    (void)sdmsc_print_fail("device attach", err);
    return;
  }
  s_dbg_dev_step = (uint32_t)k_sdmsc_dev_step_attach;
  (void)sdmsc_print_ready();

  while (1) {
    s_dbg_dev_step = (uint32_t)k_sdmsc_dev_step_parked;
    tx_thread_sleep(k_sdmsc_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */

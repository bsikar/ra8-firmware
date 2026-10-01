/**
 * @file
 * examples/ek_ra8d2/hw_validated/hil/usb_selftest_microsd/src/usb_selftest_microsd_steps.c
 * @brief USBX device worker, read-only MSC media callbacks, and host verify ladder
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of ``main.c`` to keep every translation unit under the
 * line-count cap. Holds the device-side bring-up (USBX system + device stack
 * + single read-only Mass-Storage LUN + DCD attach), the read-only media
 * callbacks that serve the boot-time SD snapshot, and the polled host-side
 * enumerate/READ(10)/verify ladder that byte-checks the looped device against
 * that same snapshot. The two cross-TU probes
 * (::s_usb_selftest_microsd_sd_snapshot, ::s_usb_selftest_microsd_dbg_bootsig)
 * are defined in ``main.c`` (stamped at SD bring-up before the kernel starts)
 * and consumed here; everything else this file needs is owned locally.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_selftest_microsd_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "ra8_usb_hmsc.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* -------------------------------------------------------------------------- */
/* ThreadX device worker + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_microsd_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "MICROSD CARD RO ";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* J-Link probes (device + host owned) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::microsd_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Sectors that read back matching the SD snapshot this pass. */
static volatile uint32_t s_dbg_luns_ok;
/** @brief Device-reported GET_MAX_LUN value (expect 0, single LUN). */
static volatile uint32_t s_dbg_max_lun;
/** @brief First mismatching sector, or ::k_microsd_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_microsd_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Device-side media_read invocations. */
static volatile uint32_t s_dbg_read_calls;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 parked. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* USB descriptors (single-interface MSC; multi-LUN is a BOT-level concept) */
/* -------------------------------------------------------------------------- */

/* The three frameworks below are synthesised at start-up from the config
 * structs by libs/ra8_usb_pal (RA8FW-317) rather than typed out as raw byte
 * arrays. Single-interface MSC: bulk-only transport over the SCSI transparent
 * command set, class 0x08 / subclass 0x06 / protocol 0x50, EP1 IN and EP2 OUT
 * at a 64-byte MPS, per BBB rev 1.0 sec 4 + USB 2.0 sec 9.6. The device
 * descriptor advertises class 0 so the class triple is read off the interface.
 * The configuration block's wTotalLength, 32 bytes a human used to add up by
 * hand, is now derived from what the builder actually emitted.
 */

/**
 * @enum demo_usb_identity_t
 * @brief The device identity this demo publishes.
 *
 * @details ::k_demo_usb_max_power_ma is the real milliamp draw, not the
 * halved bMaxPower encoding; the builder halves it. Bus-powered is the
 * default, because advertising self-powered alongside a 100 mA draw is the
 * contradiction this app's own descriptor comment warned about.
 */
typedef enum : uint16_t {
  k_demo_usb_vid          = 0x1209U, /**< idVendor, pid.codes test range. */
  k_demo_usb_pid          = 0x0015U, /**< idProduct.                      */
  k_demo_usb_bcd_device   = 0x0100U, /**< bcdDevice, release 1.00.        */
  k_demo_usb_max_power_ma = 100U,    /**< Bus draw in mA.                 */
} demo_usb_identity_t;

/**
 * @enum demo_usb_endpoint_t
 * @brief The bulk-only endpoint layout, addresses as they appear on the wire.
 *
 * @details An IN endpoint carries bit 7 set, so EP1 IN is 0x81, while EP2 OUT
 * is 0x02. That is how the byte array this block replaces wrote them, which
 * keeps the two diffable.
 */
typedef enum : uint16_t {
  k_demo_usb_in_ep      = 0x81U, /**< Bulk-IN data pipe.              */
  k_demo_usb_out_ep     = 0x02U, /**< Bulk-OUT data pipe.             */
  k_demo_usb_data_bytes = 64U,   /**< Bulk max packet size, FS.       */
  k_demo_usb_functions  = 1U,    /**< Functions the device publishes. */
} demo_usb_endpoint_t;

/**
 * @var k_demo_usb_device
 * @brief Device identity handed to the framework builders.
 * @note The three strings are string-literal storage with static duration;
 *       the builders copy them and retain no pointer.
 * @since 0.1.0
 */
static const ra8_usb_desc_device_t k_demo_usb_device = {
  .vid           = (uint16_t)k_demo_usb_vid,
  .pid           = (uint16_t)k_demo_usb_pid,
  .bcd_device    = (uint16_t)k_demo_usb_bcd_device,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 MULTILUN",
  .serial        = "00000013",
  .langid        = (uint16_t)k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = (uint16_t)k_demo_usb_max_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/**
 * @var k_demo_usb_msc
 * @brief Bulk-only endpoint layout handed to the framework builder.
 * @since 0.1.0
 */
static const ra8_usb_desc_msc_t k_demo_usb_msc = {
  .in_ep      = (uint8_t)k_demo_usb_in_ep,
  .out_ep     = (uint8_t)k_demo_usb_out_ep,
  .data_bytes = (uint16_t)k_demo_usb_data_bytes,
  .high_speed = false,
};

/**
 * @var s_device_framework_fs
 * @brief Synthesised device framework: device descriptor + configuration.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

/**
 * @var s_string_framework
 * @brief Synthesised string framework: manufacturer, product, serial.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_string_framework[k_ra8_usb_desc_strings_bytes_max];

/**
 * @var s_language_id_framework
 * @brief Synthesised language-id framework -- US English.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_language_id_framework[k_ra8_usb_desc_langid_bytes];

/**
 * @var s_device_framework_len
 * @brief Bytes ::demo_usb_build_frameworks wrote to ::s_device_framework_fs.
 * @since 0.1.0
 */
static uint32_t s_device_framework_len = 0U;

/**
 * @var s_string_framework_len
 * @brief Bytes ::demo_usb_build_frameworks wrote to ::s_string_framework.
 * @since 0.1.0
 */
static uint32_t s_string_framework_len = 0U;

/**
 * @var s_language_id_framework_len
 * @brief Bytes written to ::s_language_id_framework.
 * @since 0.1.0
 */
static uint32_t s_language_id_framework_len = 0U;

/**
 * @brief Synthesise the three USB frameworks this demo enumerates with.
 *
 * @details Replaces the three hand-typed byte arrays this app used to carry.
 * Nothing here touches a controller: a synthesised framework is bytes, not an
 * attached device.
 *
 * @return ra8_err_t Result of the three encodes.
 * @retval k_ra8_ok               All three frameworks written.
 * @retval k_ra8_err_invalid_size A destination buffer is too small.
 * @retval k_ra8_err_invalid_arg  An endpoint address or packet size is wrong.
 *
 * @pre Called from thread context before ``_ux_device_stack_initialize``.
 * @post On success the three buffers hold the frameworks and the three
 *       length variables count them.
 * @post On failure the lengths of the encodes that did not run stay 0.
 *
 * @note Single-call; the builders are pure, so a repeat call is harmless.
 * @since 0.1.0
 */
static ra8_err_t demo_usb_build_frameworks(void)
{
  const ra8_usb_class_t function = {
    .kind = k_ra8_usb_class_msc,
    .msc  = k_demo_usb_msc,
  };
  const ra8_usb_device_cfg_t cfg = {
    .desc        = &k_demo_usb_device,
    .classes     = &function,
    .class_count = (uint8_t)k_demo_usb_functions,
  };
  ra8_usb_device_frameworks_t fw = {
    .device      = s_device_framework_fs,
    .device_cap  = (uint32_t)sizeof(s_device_framework_fs),
    .strings     = s_string_framework,
    .strings_cap = (uint32_t)sizeof(s_string_framework),
    .langid      = s_language_id_framework,
    .langid_cap  = (uint32_t)sizeof(s_language_id_framework),
  };

  const ra8_err_t composed = ra8_usb_device_compose(&cfg, &fw);
  if (composed != k_ra8_ok) {
    return composed;
  }

  s_device_framework_len      = fw.device_len;
  s_string_framework_len      = fw.strings_len;
  s_language_id_framework_len = fw.langid_len;
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (single read-only microSD LUN) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: serve the boot-time SD snapshot.
 *
 * @details Bound-checks the request against the exposed window, then
 * copies the bytes straight out of ::s_usb_selftest_microsd_sd_snapshot (the
 * read-only image of SD LBA 0..63 captured at boot). No live card access
 * happens on the class thread. LED1 toggles per call so loop traffic is
 * visible.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused; single LUN).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the window; else ``UX_ERROR``.
 * @retval UX_SUCCESS Read completed from the snapshot.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre ::s_usb_selftest_microsd_sd_snapshot was populated at boot.
 * @post Either the blocks were copied or media_status is non-zero.
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT microsd_msc_read(VOID*  storage,
                             ULONG  lun,
                             UCHAR* data_pointer,
                             ULONG  number_blocks,
                             ULONG  lba,
                             ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_microsd_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  (void)memcpy(data_pointer,
               &s_usb_selftest_microsd_sd_snapshot[lba * (ULONG)k_microsd_block_size],
               (size_t)(number_blocks * (ULONG)k_microsd_block_size));
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: reject (all LUNs are read-only).
 *
 * @details The single microSD-backed volume is read-only; any WRITE(10)
 * is refused with DATA PROTECT. Hosts honouring the MODE SENSE WP bit
 * never call this.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     data_pointer USBX-owned source buffer (unused).
 * @param[in]     number_blocks Number of blocks the host tried (unused).
 * @param[in]     lba          Starting LBA (unused).
 * @param[out]    media_status Filled with DATA PROTECT sense.
 *
 * @return Always ``UX_ERROR``.
 * @retval UX_ERROR The medium is write-protected.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The LUN reports write-protected via MODE SENSE.
 * @post ``*media_status`` carries the DATA PROTECT sense triple.
 * @post No synthesized content changes.
 *
 * @note Hosts honouring the MODE SENSE WP bit never call this.
 * @since 0.1.0
 */
static UINT microsd_msc_write(VOID*  storage,
                              ULONG  lun,
                              UCHAR* data_pointer,
                              ULONG  number_blocks,
                              ULONG  lba,
                              ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)data_pointer;
  (void)number_blocks;
  (void)lba;
  *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_data_protect,
                                                       k_scsi_asc_write_protected,
                                                       k_scsi_ascq_none);
  return UX_ERROR;
}

/**
 * @brief Storage media-status callback. Always reports media-present.
 *
 * @details Synthesized LUNs never go absent; status is constant 0.
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
 * @pre The class instance is live.
 * @post ``*media_status`` is 0.
 * @post No other state changes.
 *
 * @note Synthesized volumes; never report media-not-present.
 * @since 0.1.0
 */
static UINT microsd_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX MSC with one read-only microSD LUN */
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
static UINT microsd_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_microsd_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  if (demo_usb_build_frameworks() != k_ra8_ok) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     (UCHAR*)s_device_framework_fs,
                                     (ULONG)s_device_framework_len,
                                     (UCHAR*)s_string_framework,
                                     (ULONG)s_string_framework_len,
                                     (UCHAR*)s_language_id_framework,
                                     (ULONG)s_language_id_framework_len,
                                     UX_NULL);
}

/**
 * @brief Populate the LUN parameter slot with geometry + callbacks.
 *
 * @details The single LUN is a read-only FAT-disk-typed,
 * removable, 64-sector microSD-backed volume whose media callbacks
 * serve the boot SD snapshot.
 *
 * @param[in,out] p   The class parameter block.
 * @param[in]     idx LUN slot index (0..1).
 *
 * @pre @p p is zeroed and being filled before class register.
 * @pre @p idx is below ::k_microsd_count.
 * @post Slot @p idx carries the geometry + media callbacks.
 * @post No other slot is touched.
 *
 * @note Helper to keep the register function within the size cap.
 * @since 0.1.0
 */
static void microsd_fill_lun(UX_SLAVE_CLASS_STORAGE_PARAMETER* p, uint32_t idx)
{
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_last_lba =
    (ULONG)k_microsd_sectors - 1UL;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_block_length =
    (ULONG)k_microsd_block_size;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read = microsd_msc_read;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_write =
    microsd_msc_write;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_status =
    microsd_msc_status;
}

/**
 * @brief Register the Mass-Storage class with one read-only LUN.
 *
 * @details Sets ``number_lun`` = 1 and fills the single LUN slot via
 * ::microsd_fill_lun, so the host's GET_MAX_LUN returns 0. One LUN is the
 * minimal read-only microSD exposure.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::microsd_usbx_stack_up has succeeded.
 * @pre Media callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0, with 1 LUN.
 * @post GET_MAX_LUN will report 0.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT microsd_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = (ULONG)k_microsd_count;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;
  for (uint32_t idx = 0U; idx < (uint32_t)k_microsd_count; idx++) {
    microsd_fill_lun(&msc_params, idx);
  }
  static UCHAR s_class_name[] = "ux_slave_class_storage";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

VOID microsd_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = microsd_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_microsd_dev_step_stack;
  ux             = microsd_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_microsd_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_microsd_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_microsd_dev_step_attach;

  while (1) {
    s_dbg_dev_step = (uint32_t)k_microsd_dev_step_parked;
    tx_thread_sleep(k_microsd_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* Host side: ra8_usb_hmsc enumerate + per-LUN read/verify */
/* -------------------------------------------------------------------------- */

/**
 * @brief Read + verify the LUN's sectors against the SD snapshot.
 *
 * @details READ_CAPACITY (must report ::k_microsd_sectors), confirm the SD
 * boot signature, then a raw multi-block READ(10) sweep in 8-block
 * against the boot-time SD snapshot
 * (::s_usb_selftest_microsd_sd_snapshot).
 *
 * @param[in] lun Logical unit to verify (0..1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok            Every sector matched the SD snapshot.
 * @retval k_ra8_err_invalid_size  READ_CAPACITY reported wrong geometry.
 * @retval k_ra8_err_invalid_state A byte differed from the snapshot.
 *
 * @pre The host has enumerated the device.
 * @pre @p lun is below ::k_microsd_count.
 * @post ::s_dbg_mismatch records (lun<<24 | sector) on mismatch.
 * @post Nothing is retained between LUNs.
 *
 * @note Blocking; 32 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_verify_one(uint32_t lun)
{
  static uint8_t s_burst[k_microsd_burst_bytes] = {};

  uint32_t  block_count = 0U;
  uint32_t  block_size  = 0U;
  ra8_err_t err         = ra8_usb_hmsc_read_capacity((uint8_t)lun, &block_count, &block_size);
  if (err != k_ra8_ok) {
    (void)microsd_print_fail("read_capacity", err);
    return err;
  }
  if (block_count != (uint32_t)k_microsd_sectors) {
    (void)microsd_print_fail("capacity mismatch", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  if (s_usb_selftest_microsd_dbg_bootsig == 0U) {
    (void)microsd_print_fail("no 0x55AA boot sig on SD LBA0", k_ra8_err_invalid_state);
    return k_ra8_err_invalid_state;
  }
  for (uint32_t blk = 0U; blk < (uint32_t)k_microsd_sectors;
       blk += (uint32_t)k_microsd_burst_blocks) {
    err = ra8_usb_hmsc_read10((uint8_t)lun, blk, (uint16_t)k_microsd_burst_blocks, s_burst);
    if (err != k_ra8_ok) {
      (void)microsd_print_fail("READ(10)", err);
      return err;
    }
    const uint32_t off = blk * (uint32_t)k_microsd_block_size;
    if (memcmp(s_burst, &s_usb_selftest_microsd_sd_snapshot[off], (size_t)k_microsd_burst_bytes) !=
        0) {
      s_dbg_mismatch = blk;
      (void)microsd_print_fail("SD data mismatch vs snapshot", k_ra8_err_invalid_state);
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Print "LUN n OK (256 sectors)" for a verified logical unit.
 *
 * @param[in] lun The LUN that just verified.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The line is queued.
 *
 * @pre ::microsd_verify_one returned k_ra8_ok for @p lun.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_print_lun_ok(uint32_t lun)
{
  ra8_err_t err = microsd_print("ra8d2 microsd: LUN ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = microsd_print_dec(lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return microsd_print(" OK (64 sectors, SD vs snapshot)\r\n");
}

/**
 * @brief Enumerate the looped device and print its PID + GET_MAX_LUN.
 *
 * @details Sets the enum phase, runs ::ra8_usb_hmsc_enumerate, records
 * the reported max-LUN in ::s_dbg_max_lun, and streams the
 * ``enumerated pid=... GET_MAX_LUN=...`` banner. On enumerate failure
 * the host controller is closed so the next retry re-attaches clean.
 *
 * @param[out] device Receives the enumerated descriptor snapshot.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated and the banner printed.
 *
 * @pre @p device is non-null.
 * @pre ::ra8_usb_hmsc_init has succeeded on this pass.
 * @post ::s_dbg_max_lun mirrors the device's GET_MAX_LUN.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_host_enumerate(ra8_usb_hmsc_device_t* device)
{
  s_dbg_phase   = (uint32_t)k_microsd_phase_enum;
  ra8_err_t err = ra8_usb_hmsc_enumerate(device);
  if (err != k_ra8_ok) {
    (void)microsd_print_fail("enumerate", err);
    (void)ra8_usb_hmsc_close();
    return err;
  }
  s_dbg_max_lun = (uint32_t)device->max_lun;
  err           = microsd_print("ra8d2 microsd: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = microsd_print_hex((uint32_t)device->product_id, (uint8_t)k_microsd_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = microsd_print(", GET_MAX_LUN=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = microsd_print_dec(s_dbg_max_lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return microsd_print("\r\n");
}

/**
 * @brief One full host-side pass: enumerate, verify the SD snapshot.
 *
 * @details Phases mirror ::microsd_phase_t. On any failure the host
 * controller is closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed MICROSD PASS.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_microsd_phase_init;
  ra8_err_t err = microsd_print("ra8d2 microsd: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)microsd_print_fail("host init", err);
    return err;
  }

  ra8_usb_hmsc_device_t device = {};
  err                          = microsd_host_enumerate(&device);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase   = (uint32_t)k_microsd_phase_verify;
  s_dbg_luns_ok = 0U;
  for (uint32_t lun = 0U; lun < (uint32_t)k_microsd_count; lun++) {
    err = microsd_verify_one(lun);
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    s_dbg_luns_ok++;
    err = microsd_print_lun_ok(lun);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  s_dbg_phase = (uint32_t)k_microsd_phase_pass;
  s_dbg_pass_count++;
  err = microsd_print("ra8d2 microsd: USB SELFTEST MICROSD PASS\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

VOID microsd_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_microsd_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = microsd_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_microsd_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_microsd_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */

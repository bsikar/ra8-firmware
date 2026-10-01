/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_mlun/src/main.c
 * @brief USB self-loop: HS host reads a 2-LUN FS device (multi-LUN MSC)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The multi-LUN twin of the MSC self-loop apps. The two USB ports are
 * cabled to EACH OTHER and one firmware image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class that
 *    exposes TWO logical units in one device (``GET_MAX_LUN`` = 1;
 *    UX_MAX_SLAVE_LUN caps the storage class at two). Each LUN is a
 *    small read-only raw block device whose media-read synthesizes a
 *    deterministic per-(LUN,LBA) byte pattern, so every LUN returns
 *    distinct, predictable data. IRQ-driven through the
 *    `port/usbx/ux_dcd_ra8_usb` bridge.
 *  - USBHS (J7) = HOST: the first-party polled host MSC stack
 *    (`ra8_usb_hmsc`). It enumerates the device over the cable, reads
 *    ``GET_MAX_LUN``, then for EACH LUN issues READ_CAPACITY + a full
 *    READ(10) sweep and byte-checks the data against that LUN's pattern
 *    -- proving the host correctly addresses both logical units
 *    and gets the right data from each, end to end on chip.
 *
 * No filesystem is involved (raw SCSI READ(10) per LUN), so this is a
 * pure read-path test that needs no host WRITE support.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "ra8_usb_hmsc.h"
#include "usb_selftest_mlun_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the setup
 *          window is long (U15 expander I2C blocks for ms), so the tick
 *          fires pre-kernel; feeding _tx_timer_interrupt into ThreadX's
 *          zeroed timer state bus-faults. Gate it until the kernel runs.
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra8_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra8_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_mlun_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/*
 * The tunable / formatter / geometry / phase enums (mlun_config_t, mlun_hex_t,
 * mlun_mask_t, mlun_geom_t, mlun_phase_t) are shared with the host-side steps
 * TU and live in usb_selftest_mlun_steps.h. The device-only step + SCSI-sense
 * enums below stay here because only main.c uses them.
 */

/**
 * @enum mlun_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_mlun_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_mlun_dev_step_class  = 2U, /**< MSC class registered.          */
  k_mlun_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_mlun_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_mlun_dev_step_parked = 5U, /**< Bring-up done; worker parked.  */
} mlun_dev_step_t;

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.      */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                 */
  k_scsi_sense_data_protect    = 0x07U, /**< Sense key: DATA PROTECT.    */
  k_scsi_asc_write_protected   = 0x27U, /**< ASC: WRITE PROTECTED.       */
} scsi_sense_code_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_device_thread
 * @brief ThreadX TCB for the USBX device-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_device_thread;

/**
 * @var s_device_stack
 * @brief Stack backing storage for ::s_device_thread.
 * @since 0.1.0
 */
static UCHAR s_device_stack[k_mlun_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread.
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_mlun_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_mlun_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "MULTI-LUN x3 RO ";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* J-Link probes (device-side; host-ladder probes live in the steps TU) */
/* -------------------------------------------------------------------------- */

/** @brief Device-side media_read invocations (all LUNs). */
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
  k_demo_usb_pid          = 0x0013U, /**< idProduct.                      */
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
/* Storage class media callbacks (shared across both LUNs) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: synthesize the per-LUN pattern.
 *
 * @details Bound-checks the request against the LUN geometry, then fills
 * each block via ::mlun_pattern_fill keyed on @p lun. One callback
 * serves both LUNs (USBX passes the LUN index). LED1 toggles per
 * call so loop traffic is visible.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (0..1).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the LUN; else ``UX_ERROR``.
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre @p lun is below ::k_mlun_count.
 * @post Either the blocks were synthesized or media_status is non-zero.
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT mlun_msc_read(VOID*  storage,
                          ULONG  lun,
                          UCHAR* data_pointer,
                          ULONG  number_blocks,
                          ULONG  lba,
                          ULONG* media_status)
{
  (void)storage;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_mlun_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  for (ULONG i = 0UL; i < number_blocks; i++) {
    mlun_pattern_fill((uint32_t)lun,
                      (uint32_t)(lba + i),
                      &data_pointer[i * (ULONG)k_mlun_block_size]);
  }
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: reject (all LUNs are read-only).
 *
 * @details The multi-LUN volumes are synthesized read-only; any WRITE(10)
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
static UINT mlun_msc_write(VOID*  storage,
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
static UINT mlun_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX MSC with two LUNs */
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
static UINT mlun_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_mlun_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief Populate one LUN parameter slot with geometry + callbacks.
 *
 * @details Each of the two LUNs is a read-only FAT-disk-typed,
 * removable, 256-sector volume sharing the same media callbacks (which
 * key on the LUN index).
 *
 * @param[in,out] p   The class parameter block.
 * @param[in]     idx LUN slot index (0..1).
 *
 * @pre @p p is zeroed and being filled before class register.
 * @pre @p idx is below ::k_mlun_count.
 * @post Slot @p idx carries the geometry + media callbacks.
 * @post No other slot is touched.
 *
 * @note Helper to keep the register function within the size cap.
 * @since 0.1.0
 */
static void mlun_fill_lun(UX_SLAVE_CLASS_STORAGE_PARAMETER* p, uint32_t idx)
{
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_last_lba =
    (ULONG)k_mlun_sectors - 1UL;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_block_length =
    (ULONG)k_mlun_block_size;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read  = mlun_msc_read;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_write = mlun_msc_write;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_status =
    mlun_msc_status;
}

/**
 * @brief Register the Mass-Storage class with two logical units.
 *
 * @details Sets ``number_lun`` = 2 and fills both LUN slots via
 * ::mlun_fill_lun, so the host's GET_MAX_LUN returns 1. Two is the
 * ceiling set by ``UX_MAX_SLAVE_LUN`` in the USBX storage class.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::mlun_usbx_stack_up has succeeded.
 * @pre Media callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0, with 2 LUNs.
 * @post GET_MAX_LUN will report 1.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT mlun_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = (ULONG)k_mlun_count;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;
  for (uint32_t idx = 0U; idx < (uint32_t)k_mlun_count; idx++) {
    mlun_fill_lun(&msc_params, idx);
  }
  static UCHAR s_class_name[] = "ux_slave_class_storage";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Device-side worker: bring the 2-LUN FS device up, then park.
 *
 * @details USBX system + device stack + MSC class (2 LUNs) + DCD bridge
 * on the USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB
 * state machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable on both LUNs.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID mlun_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = mlun_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_mlun_dev_step_stack;
  ux             = mlun_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_mlun_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_mlun_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_mlun_dev_step_attach;

  while (1) {
    s_dbg_dev_step = (uint32_t)k_mlun_dev_step_parked;
    tx_thread_sleep(k_mlun_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Device worker at priority 8, host worker at 24 (below the
 * USBX class threads). Sets ::s_tx_kernel_up so SysTick may feed
 * ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post Two auto-start worker threads are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  static CHAR s_device_thread_name[] = "mlun_device";
  static CHAR s_host_thread_name[]   = "mlun_host";

  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         s_device_thread_name,
                         mlun_device_worker,
                         0UL,
                         s_device_stack,
                         k_mlun_thread_stack,
                         (UINT)k_mlun_dev_priority,
                         (UINT)k_mlun_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         s_host_thread_name,
                         mlun_host_worker,
                         0UL,
                         s_host_stack,
                         k_mlun_host_stack,
                         (UINT)k_mlun_host_priority,
                         (UINT)k_mlun_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void mlun_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Open FS in the device role via the board facade, then arm HS as host.
 *
 * @details FS device: opened by ::ra8_board_usb_port_init, which owns the
 * pin identities and the VBUSEN strap. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::mlun_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void mlun_route_usb_or_halt(void)
{
  /* One board call replaces the four-step FS choreography: it routes VBUS,
   * D+ and D- to the USBFS function and keeps VBUSEN a GPIO strapped LOW for
   * the device role. The pin identities are board facts, so they live in
   * libs/ra8_board_ek_ra8d2 rather than being re-declared here. */
  if (ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_board_usbhs_pwr_set(true) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_mlun_pin_hs_vbus, k_ra8_psel_usb_hs, "mlun.hs_vbus") != k_ra8_ok) {
    mlun_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference; USBHS needs its UTMI
 * PLL. SCI8 is the J-Link OB CDC console at 115200.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called once from main.
 * @since 0.1.0
 */
static void mlun_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  const fw_clock_module_t core_module = {.kind = k_fw_clock_module_core, .index = 0U};
  if (ra8_cgc_init() != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (fw_clock_rate_for(ra8_board_clock(), core_module, &cpuclk0_hz) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_mlun_baud) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    mlun_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    mlun_panic_halt();
  }
  mlun_route_usb_or_halt();
}

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  mlun_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  mlun_panic_halt();
}

/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/usb_selftest_wlun/src/main.c
 * @brief USB self-loop: HS host WRITE(10)s + reads back a RAM-backed device
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The WRITABLE MSC self-loop -- it exercises the host->device bulk-OUT
 * data path (SCSI WRITE(10)). The two USB ports are cabled to EACH OTHER
 * and one firmware image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class that
 *    exposes one WRITABLE logical unit (``GET_MAX_LUN`` = 0) backed by a
 *    64-sector (32 KiB) RAM disk. media_write copies host data into the
 *    RAM disk; media_read serves it back. IRQ-driven through the
 *    `port/usbx/ux_dcd_ra8_usb` bridge.
 *  - USBHS (J7) = HOST: the first-party polled host MSC stack
 *    (`ra8_usb_hmsc`). It enumerates the device, WRITE(10)s a
 *    deterministic per-LBA pattern across the whole disk, then READ(10)s
 *    it back and byte-checks every sector -- proving the device bulk-OUT
 *    WRITE data phase round-trips intact, end to end on chip.
 *
 * No filesystem is involved (raw SCSI WRITE(10)/READ(10)); this is the
 * write-path counterpart to the read-only MSC self-loops, and the
 * on-bench validation for the device bulk-OUT WRITE(10) driver fix.
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
#include "usb_selftest_wlun_steps.h"

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
static const ra8_port_pin_t k_wlun_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/* The compile-time tunables (::wlun_config_t), hex/decimal formatter sizing
 * (::wlun_hex_t), text-formatter masks (::wlun_mask_t), LUN geometry
 * (::wlun_geom_t), and host-ladder phase markers (::wlun_phase_t) live in the
 * shared seam usb_selftest_wlun_steps.h alongside the helpers that consume
 * them. */

/**
 * @enum wlun_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_wlun_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_wlun_dev_step_class  = 2U, /**< MSC class registered.          */
  k_wlun_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_wlun_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_wlun_dev_step_parked = 5U, /**< Bring-up done; worker parked.  */
} wlun_dev_step_t;

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
static UCHAR s_device_stack[k_wlun_thread_stack];

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
static UCHAR s_host_stack[k_wlun_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_wlun_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "WRITABLE-RAM RW ";
static UCHAR s_msc_product_rev[] = "0001";
/** @brief Persistent mutable names retained by USBX and ThreadX. */
static UCHAR s_msc_class_name[]     = "ux_slave_class_storage";
static CHAR  s_device_thread_name[] = "wlun_device";
static CHAR  s_host_thread_name[]   = "wlun_host";

/* -------------------------------------------------------------------------- */
/* J-Link probes */
/* -------------------------------------------------------------------------- */

/* The host-ladder probes (s_dbg_phase / s_dbg_luns_ok / s_dbg_max_lun /
 * s_dbg_mismatch / s_dbg_pass_count) live with the host worker in
 * usb_selftest_wlun_host.c. */

/** @brief Device-side media_read invocations. */
static volatile uint32_t s_dbg_read_calls;
/** @brief Device-side media_write invocations (WRITE(10) data received). */
static volatile uint32_t s_dbg_write_calls;
/** @brief Total 512-B blocks the device media_write callback stored. */
static volatile uint32_t s_dbg_write_blocks;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 parked. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/** @brief Writable RAM-backed disk: the device LUN's backing store. */
static UCHAR s_disk[(size_t)k_wlun_sectors * (size_t)k_wlun_block_size];

/* -------------------------------------------------------------------------- */
/* USB descriptors (single-interface MSC; one writable logical unit) */
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
  k_demo_usb_pid          = 0x0014U, /**< idProduct.                      */
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

/* The shared per-(LUN,LBA) pattern generator ::wlun_pattern_fill lives in
 * usb_selftest_wlun_console.c (shared with the host verifier). */

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (single writable RAM-disk LUN) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: synthesize the per-LUN pattern.
 *
 * @details Bound-checks the request against the LUN geometry, then fills
 * each block via ::wlun_pattern_fill keyed on @p lun. One callback
 * serves the single LUN. LED1 toggles per
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
 * @pre @p lun is below ::k_wlun_count.
 * @post Either the blocks were synthesized or media_status is non-zero.
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT wlun_msc_read(VOID*  storage,
                          ULONG  lun,
                          UCHAR* data_pointer,
                          ULONG  number_blocks,
                          ULONG  lba,
                          ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_wlun_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  (void)memcpy(data_pointer,
               &s_disk[lba * (ULONG)k_wlun_block_size],
               (size_t)(number_blocks * (ULONG)k_wlun_block_size));
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: store host data into the RAM disk.
 *
 * @details The LUN is writable; the host's WRITE(10) data-OUT phase lands
 * here. Copies @p number_blocks sectors from the USBX buffer into the
 * RAM-backed ::s_disk so a later READ(10) returns exactly what was
 * written. Bumps ::s_dbg_write_calls / ::s_dbg_write_blocks so a J-Link
 * dump shows the device-side bulk-OUT receive actually completed.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused; single LUN).
 * @param[in]     data_pointer USBX-owned source buffer (received OUT data).
 * @param[in]     number_blocks Number of 512-byte blocks the host wrote.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with the sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the disk; else ``UX_ERROR``.
 * @retval UX_SUCCESS Blocks stored to the RAM disk.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre The OUT data phase delivered ``number_blocks * 512`` bytes.
 * @post ``s_disk`` holds the written sectors; counters advanced.
 * @post ``*media_status`` is 0 on success.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [constParameterCallback] -- USBX's
 * ux_slave_class_storage_media_write function-pointer signature takes
 * non-const UCHAR*; we cannot const-qualify the parameter. */
static UINT wlun_msc_write(VOID*  storage,
                           ULONG  lun,
                           UCHAR* data_pointer,
                           ULONG  number_blocks,
                           ULONG  lba,
                           ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_write_calls++;
  if ((lba + number_blocks) > (ULONG)k_wlun_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  (void)memcpy(&s_disk[lba * (ULONG)k_wlun_block_size],
               data_pointer,
               (size_t)(number_blocks * (ULONG)k_wlun_block_size));
  s_dbg_write_blocks += (uint32_t)number_blocks;
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led2);
  return UX_SUCCESS;
}
/* cppcheck-suppress-end [constParameterCallback] */

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
static UINT wlun_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* The SCI8 polled console helpers (wlun_nibble_to_hex / wlun_str_len /
 * wlun_sci_write / wlun_print / wlun_print_dec / wlun_print_hex /
 * wlun_print_fail) live in usb_selftest_wlun_console.c. */

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
static UINT wlun_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_wlun_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief Populate the writable LUN parameter slot with geometry + callbacks.
 *
 * @details The LUN is a writable FAT-disk-typed,
 * removable, 64-sector RAM-backed volume whose media callbacks store and
 * serve the RAM disk.
 *
 * @param[in,out] p   The class parameter block.
 * @param[in]     idx LUN slot index (0..1).
 *
 * @pre @p p is zeroed and being filled before class register.
 * @pre @p idx is below ::k_wlun_count.
 * @post Slot @p idx carries the geometry + media callbacks.
 * @post No other slot is touched.
 *
 * @note Helper to keep the register function within the size cap.
 * @since 0.1.0
 */
static void wlun_fill_lun(UX_SLAVE_CLASS_STORAGE_PARAMETER* p, uint32_t idx)
{
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_last_lba =
    (ULONG)k_wlun_sectors - 1UL;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_block_length =
    (ULONG)k_wlun_block_size;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read_only_flag =
    UX_FALSE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read  = wlun_msc_read;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_write = wlun_msc_write;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_status =
    wlun_msc_status;
}

/**
 * @brief Register the Mass-Storage class with one writable LUN.
 *
 * @details Sets ``number_lun`` = 1 and fills the LUN slot via
 * ::wlun_fill_lun, so the host's GET_MAX_LUN returns 0. One is the
 * single writable unit this self-loop exercises.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::wlun_usbx_stack_up has succeeded.
 * @pre Media callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0, with 1 LUN.
 * @post GET_MAX_LUN will report 0.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT wlun_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = (ULONG)k_wlun_count;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;
  for (uint32_t idx = 0U; idx < (uint32_t)k_wlun_count; idx++) {
    wlun_fill_lun(&msc_params, idx);
  }
  return _ux_device_stack_class_register(s_msc_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Device-side worker: bring the writable RAM-LUN device up, then park.
 *
 * @details USBX system + device stack + writable MSC class + DCD bridge
 * on the USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB
 * state machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable on its writable LUN.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID wlun_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = wlun_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_wlun_dev_step_stack;
  ux             = wlun_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_wlun_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_wlun_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_wlun_dev_step_attach;

  while (1) {
    s_dbg_dev_step = (uint32_t)k_wlun_dev_step_parked;
    tx_thread_sleep(k_wlun_idle_ticks);
  }
}

/* The host-side MSC ladder (wlun_write_disk / wlun_verify_one /
 * wlun_print_lun_ok / wlun_host_enumerate / wlun_host_pass) and the
 * wlun_host_worker entry point live in usb_selftest_wlun_host.c. */

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
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         s_device_thread_name,
                         wlun_device_worker,
                         0UL,
                         s_device_stack,
                         k_wlun_thread_stack,
                         (UINT)k_wlun_dev_priority,
                         (UINT)k_wlun_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         s_host_thread_name,
                         wlun_host_worker,
                         0UL,
                         s_host_stack,
                         k_wlun_host_stack,
                         (UINT)k_wlun_host_priority,
                         (UINT)k_wlun_host_priority,
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
static void wlun_panic_halt(void)
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
 * @pre Called once from ::wlun_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void wlun_route_usb_or_halt(void)
{
  /* One board call replaces the four-step FS choreography: it routes VBUS,
   * D+ and D- to the USBFS function and keeps VBUSEN a GPIO strapped LOW for
   * the device role. The pin identities are board facts, so they live in
   * libs/ra8_board_ek_ra8d2 rather than being re-declared here. */
  if (ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_board_usbhs_pwr_set(true) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_wlun_pin_hs_vbus, k_ra8_psel_usb_hs, "wlun.hs_vbus") != k_ra8_ok) {
    wlun_panic_halt();
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
static void wlun_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  const fw_clock_module_t core_module = {.kind = k_fw_clock_module_core, .index = 0U};
  if (ra8_cgc_init() != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (fw_clock_rate_for(ra8_board_clock(), core_module, &cpuclk0_hz) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_wlun_baud) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    wlun_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    wlun_panic_halt();
  }
  wlun_route_usb_or_halt();
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
  wlun_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  wlun_panic_halt();
}

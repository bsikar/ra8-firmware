/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_msc_device/main.c
 * @brief ThreadX + USBX Mass-Storage RAM-disk for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual to the on-board USB-FS receptacle, hands
 * control to ThreadX, and brings the Mass-Storage device class up via
 * Eclipse USBX (``_ux_device_class_storage_initialize``). The class
 * sits on top of the project's ``port/usbx/ux_dcd_ra8_usb`` bridge to
 * the hand-written ``ra8_usb`` register-level driver (HUM Ch. 36
 * USBFS, sec. 36.2.x for SYSCFG / DCPCFG / DCPMAXP / PIPECFG /
 * CFIFO). The host actually enumerates the device because USBX's
 * chapter-9 + SCSI/BBB state machines answer SETUP and BOT packets
 * through the DCD bridge.
 *
 * The single LUN is backed by an 8-sector x 512-byte (4 KiB) RAM
 * buffer in DTCM/SRAM. There is no filesystem on it -- the goal is
 * proof-of-enumeration only. The host sees an unformatted removable
 * drive of 4 KiB; macOS will offer to initialize the disk, which is
 * the expected behaviour for this demo.
 *
 * ## Pinout (USB-FS, FSP-aligned, mirrors usb_cdc_echo)
 *
 * P4_07 = VBUS, P5_00 = VBUSEN, P8_14 = D+, P8_15 = D-, all PSEL =
 * ``k_ra8_psel_usb_fs``.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates
 * as a USB Mass-Storage device. ``system_profiler SPUSBDataType``
 * lists it under "USB Bus" with class Mass Storage; Disk Utility
 * shows an unformatted 4 KiB volume.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms) AND ThreadX's timer (for tx_thread_sleep and
 * semaphore timeouts). The default weak ra8_time SysTick handler only advances
 * the ms counter; without _tx_timer_interrupt ThreadX time never advances and
 * tx_thread_sleep / USBX class-thread scheduling stall. The project's
 * tx_initialize_low_level.S configures SysTick but relies on the application
 * to publish the handler. */

extern void _tx_timer_interrupt(void);
void        SysTick_Handler(void);
void        SysTick_Handler(void)
{
  ra8_time_on_tick();
  _tx_timer_interrupt();
  /* Re-enable the USB IRQ at the NVIC level: the bridge's storm guard
   * masks it to break the USBFS event-less interrupt storm, and this
   * 1 ms pulse is its recovery clock -- a masked line is re-enabled
   * within one period so real USB events are never lost. */
  ux_dcd_ra8_usb_irq_reenable();
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB-FS pin identifiers, packed ``ra8_port_pin_t`` (port << 8 | pin).
 * @details Built as a runtime cast so clang-tidy's enum-range check
 * is happy with the otherwise out-of-enum value.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_demo_pin_vbus   = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;
static const ra8_port_pin_t k_demo_pin_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;
static const ra8_port_pin_t k_demo_pin_dp     = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;
static const ra8_port_pin_t k_demo_pin_dm     = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.      */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                 */
} scsi_sense_code_t;

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the worker thread + USBX pool +
 *        RAM-disk geometry.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 4096U,  /**< Worker thread stack (bytes).     */
  k_demo_usbx_pool_bytes = 32768U, /**< USBX memory pool (bytes).        */
  k_demo_block_size      = 512U,   /**< SCSI logical block size (bytes). */
  k_demo_block_count     = 1024U,  /**< Blocks (1024 * 512 = 512 KiB) -- big
                                         enough for a host mkfs.vfat / mount. */
  k_demo_idle_ticks      = 50U,    /**< Heartbeat back-off (ThreadX ticks). */
} demo_config_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_demo_thread
 * @brief ThreadX TCB for the USBX worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_demo_thread;

/**
 * @var s_demo_stack
 * @brief Stack backing storage for ``s_demo_thread``.
 * @since 0.1.0
 */
static UCHAR s_demo_stack[k_demo_thread_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @details Sized larger than CDC/HID because the storage class
 *          allocates 2 x bulk endpoint buffers (~64 KiB-shaped reads).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_demo_usbx_pool_bytes];

/**
 * @var s_ramdisk
 * @brief RAM-disk backing store (4 KiB = 8 x 512-byte sectors).
 * @details Zero-initialized at boot. There is no filesystem on it;
 *          macOS will offer to initialize the volume.
 * @note Single-instance; not thread-safe (USBX serializes class IO).
 * @since 0.1.0
 */
static UCHAR s_ramdisk[k_demo_block_count * k_demo_block_size];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "USBX RAM Disk   ";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints) */
/* -------------------------------------------------------------------------- */

/* Single-interface MSC config: bulk-only transport, SCSI command set.
 * Class = 0x08 (Mass Storage), SubClass = 0x06 (SCSI transparent),
 * Protocol = 0x50 (BBB / Bulk-Only). EP1 IN + EP2 OUT, 64-byte MPS.
 *
 * Layout per USB Mass Storage Class Bulk-Only Transport (BBB) rev 1.0
 * sec 4 + USB 2.0 sec 9.6.
 *
 * Total config-blob length:
 *   9 (config) + 9 (interface) + 7 (EP IN) + 7 (EP OUT) = 32 bytes.
 */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes.
   * bcdUSB = 0x0200 (USB 2.0); hosts may reject USB 1.1 for
   * modern composite/class drivers. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U, /* class      = per-interface */
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x0BU, /* PID = 0x000B (pid.codes test). */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (32 bytes total).
   * bmAttributes = 0x80 (bus-powered); 0xC0 (self-powered)
   * with bMaxPower=100mA is self-contradictory. */
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
  /* idx 2: "EK-RA8D2 RAM Disk". */
  0x09U,
  0x04U,
  0x02U,
  0x11U,
  'E',
  'K',
  '-',
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'R',
  'A',
  'M',
  ' ',
  'D',
  'i',
  's',
  'k',
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
  '0',
  '1',
};

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (read / write / status) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback. Splats from RAM-disk into USBX.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (must be 0).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to copy.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the RAM-disk; otherwise
 *         ``UX_ERROR`` with media_status set to ILLEGAL REQUEST.
 *
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` and ``media_status`` are non-NULL (USBX
 *      guarantee).
 * @pre ``lun`` is 0 (single-LUN device).
 * @post Either ``number_blocks * 512`` bytes were copied or
 *       ``media_status`` is non-zero.
 *
 * @note Called from the USBX storage class thread; no other context
 *       touches ``s_ramdisk``.
 * @since 0.1.0
 */
static UINT demo_msc_read(VOID*  storage,
                          ULONG  lun,
                          UCHAR* data_pointer,
                          ULONG  number_blocks,
                          ULONG  lba,
                          ULONG* media_status)
{
  (void)storage;
  (void)lun;
  if ((lba + number_blocks) > (ULONG)k_demo_block_count) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  (void)memcpy(data_pointer,
               &s_ramdisk[lba * (ULONG)k_demo_block_size],
               number_blocks * (ULONG)k_demo_block_size);
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback. Splats from USBX into RAM-disk.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (must be 0).
 * @param[in]     data_pointer USBX-owned source buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to copy.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the RAM-disk; otherwise
 *         ``UX_ERROR``.
 *
 * @retval UX_SUCCESS Write completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` and ``media_status`` are non-NULL.
 * @pre ``lun`` is 0.
 * @post Either ``number_blocks * 512`` bytes were copied or
 *       ``media_status`` is non-zero.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [constParameterCallback] -- USBX's
 * ux_slave_class_storage_media_write function-pointer signature takes
 * non-const UCHAR*; we cannot const-qualify the parameter. */
static UINT demo_msc_write(VOID*  storage,
                           ULONG  lun,
                           UCHAR* data_pointer,
                           ULONG  number_blocks,
                           ULONG  lba,
                           ULONG* media_status)
{
  (void)storage;
  (void)lun;
  if ((lba + number_blocks) > (ULONG)k_demo_block_count) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  (void)memcpy(&s_ramdisk[lba * (ULONG)k_demo_block_size],
               data_pointer,
               number_blocks * (ULONG)k_demo_block_size);
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}
/* cppcheck-suppress-end [constParameterCallback] */

/**
 * @brief Storage media-status callback. Always reports media-present.
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
 * @post ``*media_status`` is 0.
 *
 * @note RAM-disk is always present; never reports media-not-present.
 * @since 0.1.0
 */
static UINT demo_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + run the storage class */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + FS device stack up.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure, USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT demo_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief Registers the Mass-Storage class with one LUN backed by ``s_ramdisk``.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ``demo_usbx_stack_up`` has succeeded.
 * @pre Media read/write/status callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0.
 * @post LUN0 advertises ``k_demo_block_count`` removable FAT blocks.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT demo_msc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)k_demo_block_count - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_demo_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_FALSE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    demo_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    demo_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    demo_msc_status;

  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_storage",
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

static VOID demo_worker(ULONG arg)
{
  (void)arg;

  if (demo_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (demo_msc_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return;
  }
  if (ra8_usb_device_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    return;
  }

  /* Idle. USBX runs the SCSI/BBB state machine on its own threads. */
  while (1) {
    tx_thread_sleep(k_demo_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker */
/* -------------------------------------------------------------------------- */

/**
 * @brief ThreadX application-define hook. Spawns the demo worker.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @post One auto-start worker thread is queued.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         "usb_msc_device",
                         demo_worker,
                         0UL,
                         s_demo_stack,
                         k_demo_thread_stack,
                         8U, /* priority          */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins to the USBFS controller.
 *
 * @return Error from the first failing route call, or k_ra8_ok.
 * @retval k_ra8_ok All four pins routed.
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_demo_pin_vbus, k_ra8_psel_usb_fs, "usb_msc.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  /* VBUSEN as GPIO output LOW for USB device mode. Peripheral routing
   * forces VBUSEN HIGH (host mode) which blocks device enumeration. */
  err = ra8_gpio_output_init(k_demo_pin_vbusen, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_demo_pin_dp, k_ra8_psel_usb_fs, "usb_msc.dp");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(k_demo_pin_dm, k_ra8_psel_usb_fs, "usb_msc.dm");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS pins + LED1 + ThreadX.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  /* Bring up PLL2 -> USBCKCR / USBCKDIVCR so USBFS sees a spec-compliant
   * 48 MHz reference (PLL2P 240 MHz / 5). Must run BEFORE any caller
   * releases MSTPB11 (USBFS) -- the SREQ -> SRDY handshake silently
   * hangs otherwise (HUM Ch 9 "Clock selection switching procedure"
   * step 1). Without this the SIE never sees a 48 MHz clock and the
   * host never enumerates the device. */
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra8_ok) {
    demo_panic_halt();
  }

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

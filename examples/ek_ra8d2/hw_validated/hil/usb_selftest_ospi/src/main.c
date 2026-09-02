/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_ospi/src/main.c
 * @brief USB self-loop: the onboard OSPI flash exposed as a USB drive
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the **onboard 64 MiB Octo-SPI flash (IS25LX512M, U16)** up as a
 * USB drive and verifies it against itself on-chip -- no PC in the loop.
 * The two USB ports are cabled to EACH OTHER and one firmware image runs
 * both USB stacks PLUS the xSPI flash:
 *
 *  - At boot the device side ERASES + PROGRAMS a 1 MiB region of the
 *    OSPI (offset 0x100000) with a deterministic, sector-derived
 *    pattern via `ra8_xspi` (the same driver flash_journal validated) --
 *    so the flash genuinely holds known content.
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class exposes
 *    that OSPI region as a read-only synthesized FAT16 volume with one
 *    file ``OSPI.BIN``; media-read pulls each sector straight off the
 *    flash with `ra8_xspi_flash_read`.
 *  - USBHS (J7) = HOST: the polled first-party host stack (`ra8_usb_hmsc`
 *    + `ra8_fs`) enumerates the device over the cable, mounts the volume,
 *    streams the data region back with raw multi-block READ(10), and
 *    checks every sector against the SAME deterministic pattern formula
 *    -- so the host never touches the single xSPI controller (no
 *    contention) yet proves the OSPI erase + program + read round-trips
 *    intact over USB. A WRITE(10) into the read-only LUN is rejected.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device, RHST = FS).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * OSPI: OCTA pins routed by `ra8_board_xspi_pins_init` (PSEL 0x1C) on
 * xSPI CS1 (IS25LX512M). FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO
 * LOW (device role), P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to
 * Host via the U15 expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08
 * USBHS_VBUS (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
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
#include "ra8_fs.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "ra8_xspi.h"
#include "usb_selftest_ospi_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer (for tx_thread_sleep and USBX class-thread scheduling).
 * The 1 ms pulse also recovers the DCD's storm-guard NVIC mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick (ra8_time_init) BEFORE tx_kernel_enter,
 *          and this app's setup window is long (the U15 expander I2C
 *          transaction blocks for milliseconds), so the tick WILL fire
 *          pre-kernel. Feeding _tx_timer_interrupt into ThreadX's
 *          still-zeroed timer state walks a bogus expiration list and
 *          bus-faults (observed: IMPRECISERR HardFault from SysTick).
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

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_selftest_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_selftest_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_selftest_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_selftest_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_selftest_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_selftest_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

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
static UCHAR s_device_stack[k_selftest_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the polled host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread (ra8_fs walks live here).
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_selftest_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_selftest_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "SELFTEST OSPI RO";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* J-Link probes */
/* -------------------------------------------------------------------------- */

/** @brief OSPI JEDEC id read at boot (IS25LX512M = 0x009D5A1A). */
static volatile uint32_t s_dbg_ospi_id;
/** @brief OSPI provisioning result (sentinel = pending, 0 = done, else err). */
static volatile uint32_t s_dbg_ospi_prov = (uint32_t)k_selftest_no_mismatch;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints) */
/* -------------------------------------------------------------------------- */

/* Single-interface MSC config: bulk-only transport, SCSI command set.
 * EP1 IN + EP2 OUT, 64-byte MPS. PID 0x000E marks the self-test
 * identity apart from the Mac-facing usb_msc_mram (0x000C). */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
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
  0x10U, /* PID = 0x0010 (pid.codes test). */
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
  /* idx 2: "RA8D2 SELFTEST". */
  0x09U,
  0x04U,
  0x02U,
  0x0EU,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'S',
  'E',
  'L',
  'F',
  'T',
  'E',
  'S',
  'T',
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
  '0',
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
/* Threads */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + FS device stack up.
 *
 * @details One-shot USBX pool + device-stack initialization for the
 * FS-only framework.
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
static UINT selftest_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_selftest_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief Registers the Mass-Storage class with the read-only MRAM LUN.
 *
 * @details Single LUN, write-protected, FAT16 geometry from
 * ::selftest_fat_geom_t, media callbacks above.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::selftest_usbx_stack_up has succeeded.
 * @pre Media read/write/status callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0.
 * @post LUN0 advertises the read-only synthesized FAT16 volume.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT selftest_msc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)k_fat_total_sectors - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_selftest_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    selftest_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    selftest_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    selftest_msc_status;

  static UCHAR s_class_name[] = "ux_slave_class_storage";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Erase the OSPI window and program the deterministic pattern.
 *
 * @details Erases ::k_ospi_erase_count 4 KiB sectors at
 * ::k_ospi_test_offset, then programs the 1 MiB window with the
 * ::selftest_pattern_fill bytes, one 4 KiB page-group per
 * ``ra8_xspi_flash_program`` (the sector pattern packed 8-per-erase).
 *
 * @return First failing flash op's error, or k_ra8_ok.
 * @retval k_ra8_ok The window holds the deterministic pattern.
 *
 * @pre ``ra8_xspi_init`` has succeeded for ::k_ospi_instance.
 * @pre Single caller (the device worker, before USB attach).
 * @post On k_ra8_ok every window sector reads back its pattern.
 * @post Only the window region is touched; the rest of the chip is left.
 *
 * @note Blocking; ~256 sector erases (seconds). Runs once at boot.
 * @since 0.1.0
 */
static ra8_err_t selftest_ospi_write_pattern(void)
{
  static UCHAR   chunk[k_ospi_erase_sector] = {};
  const uint32_t sec_per_erase = (uint32_t)k_ospi_erase_sector / (uint32_t)k_selftest_block_size;
  for (uint32_t e = 0U; e < (uint32_t)k_ospi_erase_count; e++) {
    const uint32_t  addr = (uint32_t)k_ospi_test_offset + (e * (uint32_t)k_ospi_erase_sector);
    const ra8_err_t err  = ra8_xspi_flash_erase_sector((uint8_t)k_ospi_instance, addr);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  for (uint32_t e = 0U; e < (uint32_t)k_ospi_erase_count; e++) {
    for (uint32_t s = 0U; s < sec_per_erase; s++) {
      selftest_pattern_fill((e * sec_per_erase) + s, &chunk[s * (uint32_t)k_selftest_block_size]);
    }
    const uint32_t  addr = (uint32_t)k_ospi_test_offset + (e * (uint32_t)k_ospi_erase_sector);
    const ra8_err_t err =
      ra8_xspi_flash_program((uint8_t)k_ospi_instance, addr, chunk, (uint32_t)k_ospi_erase_sector);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Bring the OSPI flash up and provision the test pattern.
 *
 * @details Mirrors flash_journal's bring-up: U15 expander courtesy
 * write, ``ra8_board_xspi_pins_init`` (OCTA pins + RESET pulse),
 * ``ra8_xspi_init`` in 1S-1S-1S mode, JEDEC-id readback (stamped to
 * ::s_dbg_ospi_id for the bench), then ::selftest_ospi_write_pattern.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok OSPI is up and the window holds the pattern.
 *
 * @pre CGC is initialized (main ran ra8_cgc_init).
 * @pre Single caller (the device worker, before USB attach).
 * @post ::s_dbg_ospi_id / ::s_dbg_ospi_prov reflect the outcome.
 * @post The 1 MiB OSPI window is erased + programmed on success.
 *
 * @note Blocking; runs once at boot before USB attach.
 * @since 0.1.0
 */
static ra8_err_t selftest_ospi_provision(void)
{
  (void)ra8_board_io_expander_set_octospi_active();
  ra8_err_t err = ra8_board_xspi_pins_init();
  if (err != k_ra8_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  err = ra8_xspi_init((uint8_t)k_ospi_instance, k_ra8_xspi_lio_1s1s1s);
  if (err != k_ra8_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  uint32_t id = 0U;
  (void)ra8_xspi_flash_read_id((uint8_t)k_ospi_instance, &id);
  s_dbg_ospi_id   = id;
  err             = selftest_ospi_write_pattern();
  s_dbg_ospi_prov = (uint32_t)err;
  return err;
}

/**
 * @brief Device-side worker: provision OSPI, bring the FS device up.
 *
 * @details First erases + programs the OSPI window with the test
 * pattern (::selftest_ospi_provision), THEN brings USBX + the device
 * stack + MSC class + DCD bridge up on the USBFS controller and
 * DPRPU-attaches. Provisioning runs before attach so the volume is
 * fully populated the moment the host can read it. USBX runs the
 * SCSI/BBB state machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock + CGC are up (main did them).
 * @post The OSPI window is provisioned and the FS device is attached.
 * @post On any bring-up failure the thread exits (probes show where).
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID selftest_device_worker(ULONG arg)
{
  (void)arg;

  if (selftest_ospi_provision() != k_ra8_ok) {
    return;
  }
  if (selftest_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (selftest_msc_class_register() != UX_SUCCESS) {
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
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops
 * ::selftest_host_pass with a retry pause until the whole config A
 * ladder passes; afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread (lower priority than
 *      the USBX device-side threads).
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Polled host stack: blocking calls, ms timeouts via ra8_time.
 * @since 0.1.0
 */
static VOID selftest_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_selftest_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = selftest_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_selftest_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Device worker at priority 8 (above USBX class threads'
 * default), host worker at 16 so the polled host loop can never starve
 * the IRQ-driven device side.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @post Two auto-start worker threads are queued.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  static CHAR s_device_thread_name[] = "selftest_device";
  static CHAR s_host_thread_name[]   = "selftest_host";

  (void)first_unused_memory;
  s_tx_kernel_up = true; /* ThreadX timer state is initialized past here. */
  (void)tx_thread_create(&s_device_thread,
                         s_device_thread_name,
                         selftest_device_worker,
                         0UL,
                         s_device_stack,
                         k_selftest_thread_stack,
                         (UINT)k_selftest_dev_priority,
                         (UINT)k_selftest_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         s_host_thread_name,
                         selftest_host_worker,
                         0UL,
                         s_host_stack,
                         k_selftest_host_stack,
                         (UINT)k_selftest_host_priority,
                         (UINT)k_selftest_host_priority,
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
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal error in boot.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void selftest_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense (PSEL), P5_00 VBUSEN held LOW as
 * GPIO (peripheral routing would force host-style VBUSEN HIGH and block
 * device enumeration), P8_14/P8_15 data. HS host: SW4-8 to Host via the
 * U15 expander, PD07 HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::selftest_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void selftest_route_usb_or_halt(void)
{
  /* FS port: device role. */
  if (ra8_pfs_route_peripheral(k_selftest_pin_fs_vbus, k_ra8_psel_usb_fs, "selftest.fs_vbus") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_gpio_output_init(k_selftest_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_selftest_pin_fs_dp, k_ra8_psel_usb_fs, "selftest.fs_dp") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_selftest_pin_fs_dm, k_ra8_psel_usb_fs, "selftest.fs_dm") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
  /* HS port: host role. */
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_gpio_output_init(k_selftest_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_selftest_pin_hs_vbus, k_ra8_psel_usb_hs, "selftest.hs_vbus") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference before MSTPB11 is
 * released; USBHS needs its 60 MHz UTMI PLL. SCI8 is the J-Link OB CDC
 * console at 115200.
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called exactly once from main.
 * @since 0.1.0
 */
static void selftest_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_selftest_baud) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    selftest_panic_halt();
  }
  selftest_route_usb_or_halt();
}

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the two workers only deal with stack bring-up.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  selftest_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  selftest_panic_halt();
}

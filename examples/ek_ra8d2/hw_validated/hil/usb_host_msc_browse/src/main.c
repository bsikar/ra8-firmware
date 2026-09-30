/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_msc_browse/src/main.c
 * @brief USB host-mode MSC browse over the on-board self-loop (no external drive)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Validates the first-party USB host MSC stack (`ra8_usb_hmsc`) by having the
 * board host AND simulate the peripheral over the loop cable -- no real USB
 * drive needed. The two USB jacks are cabled to each other and one image runs
 * both roles:
 *
 *  - USBFS (J11) = DEVICE (the fake peripheral): a ThreadX + USBX
 *    Mass-Storage class exposing the 1 MiB MRAM window at 0x02000000 as a
 *    read-only synthesized FAT16 volume with one file ``MRAM.BIN``.
 *  - USBHS (J7) = HOST: the polled first-party host stack (`ra8_usb_hmsc` +
 *    `ra8_fs`) on a low-priority ThreadX thread. It enumerates the device,
 *    mounts the FAT16 volume, then BROWSES it -- reads the root directory over
 *    READ(10) and parses the file entry (name + size) -- before a raw
 *    byte-for-byte read-back of the data region and the write-protect check.
 *
 * The browse is the host-side directory walk that distinguishes this app from
 * the raw read-verify self-tests; the original SD-drive version needed a real
 * thumb drive on J7, which the self-loop now stands in for. Verdicts stream
 * over SCI8 (J-Link OB CDC, 115200); ``s_dbg_*`` mirror progress for J-Link.
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN as GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
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
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "ra8_usb_hmsc.h"
#include "usb_host_msc_browse_steps.h"

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
static UCHAR s_msc_product_id[]  = "SELFTEST MRAM RO";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints) */
/* -------------------------------------------------------------------------- */

/** @brief Identity this app publishes. */
typedef enum : uint16_t {
  k_demo_usb_vid          = 0x1209U, /**< idVendor, pid.codes test range. */
  k_demo_usb_pid          = 0x000EU, /**< idProduct.                      */
  k_demo_usb_bcd_device   = 0x0100U, /**< bcdDevice, release 1.00.        */
  k_demo_usb_max_power_ma = 100U,    /**< Bus draw in mA.                 */
} demo_usb_ident_t;

/** @brief Endpoint addresses and packet sizes of the mass-storage function. */
typedef enum : uint16_t {
  k_demo_usb_in_ep         = 0x81U, /**< Bulk-IN data pipe.              */
  k_demo_usb_out_ep        = 0x02U, /**< Bulk-OUT data pipe.             */
  k_demo_usb_data_bytes_fs = 64U,   /**< Bulk max packet size, FS.       */
  k_demo_usb_functions     = 1U,    /**< Functions the device publishes. */
} demo_usb_endpoint_t;

/**
 * @var k_demo_usb_device
 * @brief The identity this app publishes.
 * @since 0.1.0
 */
static const ra8_usb_desc_device_t k_demo_usb_device = {
  .vid           = (uint16_t)k_demo_usb_vid,
  .pid           = (uint16_t)k_demo_usb_pid,
  .bcd_device    = (uint16_t)k_demo_usb_bcd_device,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 SELFTEST",
  .serial        = "00000004",
  .langid        = (uint16_t)k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = (uint16_t)k_demo_usb_max_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/**
 * @var k_demo_usb_msc_fs
 * @brief The mass-storage function at full speed.
 * @since 0.1.0
 */
static const ra8_usb_desc_msc_t k_demo_usb_msc_fs = {
  .in_ep      = (uint8_t)k_demo_usb_in_ep,
  .out_ep     = (uint8_t)k_demo_usb_out_ep,
  .data_bytes = (uint16_t)k_demo_usb_data_bytes_fs,
  .high_speed = false,
};

/**
 * @var s_device_framework_fs
 * @brief Synthesised full-speed framework: device descriptor + configuration.
 * @note Written once by ::selftest_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static UCHAR s_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

/** @brief Bytes written to ::s_device_framework_fs. */
static uint32_t s_device_framework_fs_len = 0U;

/**
 * @var s_string_framework
 * @brief Synthesised string framework: manufacturer, product, serial.
 * @note Written once by ::selftest_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static UCHAR s_string_framework[k_ra8_usb_desc_strings_bytes_max];

/** @brief Bytes written to ::s_string_framework. */
static uint32_t s_string_framework_len = 0U;

/**
 * @var s_language_id_framework
 * @brief Synthesised language-id framework -- US English.
 * @note Written once by ::selftest_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[k_ra8_usb_desc_langid_bytes];

/** @brief Bytes written to ::s_language_id_framework. */
static uint32_t s_language_id_framework_len = 0U;

/* -------------------------------------------------------------------------- */
/* Threads */
/* -------------------------------------------------------------------------- */

/**
 * @brief Synthesise every USB framework this app publishes.
 * @return ra8_err_t ``k_ra8_ok`` on success.
 * @retval k_ra8_ok Every framework written.
 * @retval k_ra8_err_invalid_arg A config the builder cannot encode.
 * @pre File-scope buffers reserved; single-threaded bring-up context.
 * @post The framework buffers and their lengths are set.
 * @note Single-call; the frameworks are read-only afterwards.
 * @since 0.1.0
 */
static ra8_err_t selftest_usb_build_frameworks(void)
{
  const ra8_usb_class_t function = {
    .kind = k_ra8_usb_class_msc,
    .msc  = k_demo_usb_msc_fs,
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

  s_device_framework_fs_len   = fw.device_len;
  s_string_framework_len      = fw.strings_len;
  s_language_id_framework_len = fw.langid_len;
  return k_ra8_ok;
}

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
  if (selftest_usb_build_frameworks() != k_ra8_ok) {
    return UX_ERROR;
  }

  if (_ux_system_initialize(s_usbx_pool, k_selftest_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework_fs,
                                     (ULONG)s_device_framework_fs_len,
                                     s_string_framework,
                                     (ULONG)s_string_framework_len,
                                     s_language_id_framework,
                                     (ULONG)s_language_id_framework_len,
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
 * @brief Device-side worker: bring the FS device stack up, then park.
 *
 * @details USBX system + device stack + MSC class + DCD bridge on the
 * USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB state
 * machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable.
 * @post On any bring-up failure the thread exits (probes show where).
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID selftest_device_worker(ULONG arg)
{
  (void)arg;

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
 * @brief Open FS in the device role via the board facade, then arm HS as host.
 *
 * @details FS device: opened by ::ra8_board_usb_port_init, which owns the
 * pin identities and the VBUSEN strap. HS host: SW4-8 to Host via the
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
  /* One board call replaces the four-step FS choreography: it routes VBUS,
   * D+ and D- to the USBFS function and keeps VBUSEN a GPIO strapped LOW for
   * the device role. The pin identities are board facts, so they live in
   * libs/ra8_board_ek_ra8d2 rather than being re-declared here. */
  if (ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device) != k_ra8_ok) {
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
 * released; USBHS needs its 60 MHz UTMI PLL. The BSP console (SCI8 on
 * PD02/PD03) is the J-Link OB CDC log at 115200.
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
  const fw_clock_module_t core_module = {.kind = k_fw_clock_module_core, .index = 0U};
  if (ra8_cgc_init() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    selftest_panic_halt();
  }
  if (fw_clock_rate_for(ra8_board_clock(), core_module, &cpuclk0_hz) != k_ra8_ok) {
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

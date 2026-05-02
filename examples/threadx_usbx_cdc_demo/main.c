/**
 * @file main.c
 * @brief ThreadX + USBX CDC ACM echo demo for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()``, routes the four USB-FS
 * pins per the EK-RA8D2 v1 User's Manual to the on-board USB-FS
 * receptacle, hands control to ThreadX, and brings the CDC ACM stack
 * up via Eclipse USBX's class layer (``ux_device_class_cdc_acm_initialize``).
 * Same hardware test as ``examples/usb_cdc_echo`` but using USBX's
 * class abstraction instead of the project's hand-rolled
 * ``ra_usb_cdc`` layer.
 *
 * ## Pinout (USB-FS, FSP-aligned)
 *
 * | Net           | Pin    | PFS PSEL                |
 * |---------------|--------|-------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) |
 *
 * ## Sequence
 *
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init`` for any back-off delays.
 *   3. ``ra_pfs_route_peripheral`` for the four USB-FS pins.
 *   4. ``ra_gpio_output_init(k_ra_pin_led1)`` for visual heartbeat.
 *   5. ThreadX ``tx_kernel_enter()`` -- spins the scheduler.
 *   6. ``tx_application_define`` -- spawns one worker thread that:
 *        - Allocates USBX memory pool.
 *        - Calls ``ux_system_initialize`` + ``ux_device_stack_initialize``
 *          with the static device + configuration descriptors.
 *        - Calls ``ux_device_class_cdc_acm_initialize`` to register
 *          the class.
 *        - Calls ``ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs)`` to
 *          plug our DCD bridge into the USBX device stack.
 *        - Calls ``ra_usb_device_attach(true)`` to raise the D+
 *          pull-up so the host begins enumeration.
 *        - Drops into the echo loop:
 *          ``ux_device_class_cdc_acm_read`` -> ``ux_device_class_cdc_acm_write``
 *          -- toggling LED1 once per byte.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates
 * as ``/dev/cu.usbmodem*``. Open it RDWR with picocom or screen and
 * type characters; every byte echoes back and LED1 toggles per byte.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

static const ra_port_pin_t k_demo_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_demo_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_demo_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_demo_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the echo loop and ThreadX worker.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 4096U,  /**< Worker thread stack (bytes).        */
  k_demo_usbx_pool_bytes = 16384U, /**< USBX memory pool (bytes).          */
  k_demo_echo_buf_bytes  = 64U,    /**< One bulk-FS packet per recv/send.   */
  k_demo_idle_ms         = 1U,     /**< Idle back-off when no data queued.  */
} demo_config_t;

#ifndef RA_SIMULATOR_MODE
/* ThreadX worker. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];

/* USBX memory pool (USBX uses tx_byte_pool internally). */
static UCHAR s_usbx_pool[k_demo_usbx_pool_bytes];

/* Active CDC ACM class instance. Filled in by activate-callback. */
static UX_SLAVE_CLASS_CDC_ACM* s_cdc_acm = nullptr;

/* -------------------------------------------------------------------------- */
/* USB descriptors (minimal CDC ACM)                                          */
/* -------------------------------------------------------------------------- */

/* VID/PID matches usb_cdc_echo (pid.codes test range). */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor 18 */
  0x12U,
  0x01U,
  0x10U,
  0x01U,
  0xEFU,
  0x02U,
  0x01U,
  0x40U,
  0x09U,
  0x12U,
  0x0AU,
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor 67 */
  0x09U,
  0x02U,
  0x43U,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0xC0U,
  0x32U,
  /* Interface association (CDC) */
  0x08U,
  0x0BU,
  0x00U,
  0x02U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* Communications interface */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* CDC header */
  0x05U,
  0x24U,
  0x00U,
  0x10U,
  0x01U,
  /* Call management */
  0x05U,
  0x24U,
  0x01U,
  0x00U,
  0x01U,
  /* ACM */
  0x04U,
  0x24U,
  0x02U,
  0x02U,
  /* Union */
  0x05U,
  0x24U,
  0x06U,
  0x00U,
  0x01U,
  /* Interrupt-IN endpoint (EP3 IN) */
  0x07U,
  0x05U,
  0x83U,
  0x03U,
  0x08U,
  0x00U,
  0xFFU,
  /* Data interface */
  0x09U,
  0x04U,
  0x01U,
  0x00U,
  0x02U,
  0x0AU,
  0x00U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT) */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN) */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

/* String descriptors -- vendor / product / serial. */
static UCHAR s_string_framework[] = {
  0x09U, 0x04U, 0x01U, 0x12U, 'B',   'r',   'i', 'g', 'h',   't',   'o',   'n',   ' ', 'S',
  'i',   'k',   'a',   'r',   's',   'k',   'i', 'e', 0x09U, 0x04U, 0x02U, 0x14U, 'E', 'K',
  '-',   'R',   'A',   '8',   'D',   '2',   ' ', 'C', 'D',   'C',   ' ',   'E',   'c', 'h',
  'o',   '!',   0x09U, 0x04U, 0x03U, 0x08U, '0', '0', '0',   '0',   '0',   '0',   '0', '1',
};

static UCHAR s_language_id_framework[] = {0x09U, 0x04U};

/* -------------------------------------------------------------------------- */
/* CDC ACM activate / deactivate callbacks                                    */
/* -------------------------------------------------------------------------- */

static VOID demo_cdc_activate(VOID* cdc_instance)
{
  s_cdc_acm = (UX_SLAVE_CLASS_CDC_ACM*)cdc_instance;
}

static VOID demo_cdc_deactivate(VOID* cdc_instance)
{
  (void)cdc_instance;
  s_cdc_acm = nullptr;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + echo loop                                   */
/* -------------------------------------------------------------------------- */

static VOID demo_worker(ULONG arg)
{
  (void)arg;

  /* Bring USBX up. */
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return;
  }
  if (_ux_device_stack_initialize((UCHAR*)UX_NULL, /* HS framework            */
                                  0,
                                  s_device_framework_fs,
                                  sizeof(s_device_framework_fs),
                                  s_string_framework,
                                  sizeof(s_string_framework),
                                  s_language_id_framework,
                                  sizeof(s_language_id_framework),
                                  UX_NULL) != UX_SUCCESS) {
    return;
  }

  /* Register CDC-ACM class against the configuration. */
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = demo_cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = demo_cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  if (_ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                      _ux_device_class_cdc_acm_entry,
                                      1, /* configuration #  */
                                      0, /* interface #      */
                                      &cdc_params) != UX_SUCCESS) {
    return;
  }

  /* Plug our DCD bridge into the device stack (it stamps
   * _ux_system_slave -> ux_system_slave_dcd) and turn the bus on. */
  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs) != k_ra_ok) {
    return;
  }
  if (ra_usb_device_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    return;
  }

  /* Echo loop. ux_device_class_cdc_acm_read blocks on the bulk-OUT
   * semaphore until the host pushes data; we send it straight back
   * out the bulk-IN pipe. */
  UCHAR buf[k_demo_echo_buf_bytes];
  ULONG n = 0UL;
  while (1) {
    if (s_cdc_acm == UX_NULL) {
      tx_thread_sleep(1);
      continue;
    }
    if (_ux_device_class_cdc_acm_read(s_cdc_acm, buf, sizeof(buf), &n) != UX_SUCCESS) {
      tx_thread_sleep(1);
      continue;
    }
    if (n == 0UL) {
      continue;
    }
    if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
      continue;
    }
    for (ULONG i = 0UL; i < n; i++) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    }
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker                                     */
/* -------------------------------------------------------------------------- */

VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         "usbx_cdc_demo",
                         demo_worker,
                         0UL,
                         s_demo_stack,
                         k_demo_thread_stack,
                         8U, /* priority         */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 *
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
 * @return Error from the first failing route call, or k_ra_ok.
 * @retval k_ra_ok All four pins routed.
 *
 * @pre IOPORT module is reachable.
 * @post On success the pins are in USB peripheral mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_demo_pin_vbus, k_ra_psel_usb_fs, "usbx.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_demo_pin_vbusen, k_ra_psel_usb_fs, "usbx.vbusen");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_demo_pin_dp, k_ra_psel_usb_fs, "usbx.dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_demo_pin_dm, k_ra_psel_usb_fs, "usbx.dm");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS pins + LED1 + ThreadX.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

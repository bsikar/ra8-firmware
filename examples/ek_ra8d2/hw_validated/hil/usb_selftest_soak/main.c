/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_soak/main.c
 * @brief USB self-loop endurance soak + throughput benchmark (board-only)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The robustness/benchmark member of the self-loop matrix: same dual-stack
 * image as config A (HS host + FS device on the J7<->J11 cable), but the host
 * worker REPEATS the 1 MiB MRAM integrity sweep ::k_selftest_soak_iters times
 * back to back instead of once. Every 4 KiB READ(10) burst is still memcmp'd
 * against the real MRAM window, so a single corrupted or dropped transfer
 * anywhere in the soak fails the run -- this is the endurance proof that the
 * USB transport stays byte-perfect under sustained load. The aggregate volume
 * and elapsed time are summed across all iterations for a stable throughput
 * benchmark (more representative than a single short sweep), then the RO
 * write-rejection is confirmed once.
 *
 *  - USBFS (J11) = DEVICE: the USBX MSC class exposing the 1 MiB MRAM window
 *    at 0x02000000 as a read-only synthesized FAT16 volume (``MRAM.BIN``).
 *  - USBHS (J7) = HOST: the polled first-party host stack (`ra8_usb_hmsc` +
 *    `ra8_fs`) on a low-priority ThreadX thread.
 *
 * Entirely on-chip: no Mac/PC host -- the board both hosts and devices itself,
 * so the soak runs anywhere the loop cable is fitted. Verdicts stream over SCI8
 * (J-Link OB CDC, 115200); ::s_dbg_pass_count mirrors the completed-iteration
 * count for a J-Link readout of soak progress.
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
#include "usb_selftest_common.h"
#include "usb_selftest_console.h"
#include "usb_selftest_device.h"
#include "usb_selftest_host.h"

#ifndef RA8_SIMULATOR_MODE
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

#ifndef RA8_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX worker thread storage (device + host TCBs and stacks) */
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
  (void)first_unused_memory;
  s_tx_kernel_up = true; /* ThreadX timer state is initialized past here. */
  (void)tx_thread_create(&s_device_thread,
                         "selftest_device",
                         selftest_device_worker,
                         0UL,
                         s_device_stack,
                         k_selftest_thread_stack,
                         (UINT)k_selftest_dev_priority,
                         (UINT)k_selftest_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "selftest_host",
                         selftest_host_worker,
                         0UL,
                         s_host_stack,
                         k_selftest_host_stack,
                         (UINT)k_selftest_host_priority,
                         (UINT)k_selftest_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the two workers only deal with stack bring-up.
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
  selftest_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  selftest_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

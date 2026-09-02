/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_fs_host/src/main.c
 * @brief USB self-loop config B: FS host reads the HS device's MSC MRAM disk
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The role-flipped twin of `usb_selftest_hs_host`. The board's two USB
 * ports are cabled to EACH OTHER and one firmware image runs BOTH sides
 * of the link, with the host and device roles SWAPPED versus config A:
 *
 *  - USBHS (J7) = DEVICE: the ThreadX + USBX Mass-Storage class from
 *    `usb_msc_mram_hs`, exposing the 1 MiB MRAM window at 0x02000000 as
 *    a read-only synthesized FAT16 volume with one file ``MRAM.BIN``.
 *    IRQ-driven through the `port/usbx/ux_dcd_ra8_usb` bridge on the
 *    USBHS controller; it ships both the HS and the FS-fallback
 *    frameworks so it can serve a full-speed host.
 *  - USBFS (J11) = HOST: the polled first-party host stack from
 *    `usb_host_file_ops` (`ra8_usb_hmsc` + `ra8_fs`), running in a
 *    low-priority ThreadX thread. It enumerates the HS device over the
 *    cable, mounts the FAT16 volume, streams the data region back with
 *    raw multi-block READ(10), and memcmp's every burst against the
 *    SAME MRAM bytes read directly -- a fully on-chip end-to-end proof
 *    that the USB transport returns the truth. A WRITE(10) into the
 *    read-only LUN must come back rejected.
 *
 * The link runs at 12 Mbps: the FS host is the ceiling, so the HS
 * device falls back to full speed (its FS framework, 64-byte bulk MPS)
 * -- the USBHS-device-at-full-speed path, which neither the Mac ladders
 * (HS) nor config A (FS device) exercised.
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * HS device: P4_08 USBHS_VBUS sense (PSEL usb_hs), PD07 driven LOW
 * (J7 role = Device, so U18 does not back-feed VBUS); D+/D- are
 * dedicated PHY balls. FS host: P4_07 VBUS sense, P5_00 VBUSEN
 * peripheral-routed (the USBFS controller sources J11 VBUS), P8_14 D+,
 * P8_15 D- (PSEL usb_fs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
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
#include "usb_selftest_fs_host_steps.h"

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

/** @brief USBFS VBUSEN (P5_00) -- peripheral-routed; the FS host
 *         controller sources J11 VBUS through it (config B host role). */
static const ra8_port_pin_t k_selftest_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_selftest_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_selftest_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_selftest_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 role strap (PD07): LOW = Device, so U18 does not back-feed
 *         VBUS into the FS host's cable (config B device role, UM 6.2). */
static const ra8_port_pin_t k_selftest_pin_hs_role = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX worker TCBs + stacks */
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
 * @brief Route both ports' pins: HS as device, FS as host (config B).
 *
 * @details HS device: P4_08 VBUS sense (PSEL usb_hs), PD07 driven LOW
 * so J7's role is Device and U18 does not back-feed VBUS into the FS
 * host's cable; D+/D- are dedicated PHY balls (no PFS routing). FS
 * host: P4_07 VBUS sense, P5_00 VBUSEN peripheral-routed so the USBFS
 * controller sources J11 VBUS, P8_14/P8_15 data -- all PSEL usb_fs.
 *
 * @pre IOPORT is reachable.
 * @pre Called once from ::selftest_setup_or_halt.
 * @post HS pins carry the device role, FS pins the host role.
 * @post PD07 is LOW (J7 device, not self-powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void selftest_route_usb_or_halt(void)
{
  /* HS port: device role. PD07 LOW so U18 does not back-feed VBUS. */
  if (ra8_pfs_route_peripheral(k_selftest_pin_hs_vbus, k_ra8_psel_usb_hs, "selftest.hs_vbus") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_gpio_output_init(k_selftest_pin_hs_role, k_ra8_level_low) != k_ra8_ok) {
    selftest_panic_halt();
  }
  /* FS port: host role. P5_00 VBUSEN peripheral-routed sources J11. */
  if (ra8_pfs_route_peripheral(k_selftest_pin_fs_vbus, k_ra8_psel_usb_fs, "selftest.fs_vbus") !=
      k_ra8_ok) {
    selftest_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_selftest_pin_fs_vbusen, k_ra8_psel_usb_fs, "selftest.fs_vbusen") !=
      k_ra8_ok) {
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

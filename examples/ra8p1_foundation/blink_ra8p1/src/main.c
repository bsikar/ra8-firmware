/**
 * @file examples/ra8p1_foundation/blink_ra8p1/src/main.c
 * @brief RA8P1 build-foundation blink -- proves ra8_core + ra8_hal compile and
 *        link for the R7KA8P1KFLCAC
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the RA8P1 counterpart of ``examples/ek_ra8d2/.../blink``. Its purpose
 * is NOT hardware validation (there is no RA8P1 board yet) but to prove the
 * multi-chip foundation: built with ``cmake/toolchain-ra8p1.cmake`` it compiles
 * with ``-DRA8_DEVICE_RA8P1``, and the ENTIRE ``ra8_core`` + ``ra8_hal`` plus the
 * dedicated ``ra8_board_ra8p1`` layer (issue #226) compile and link for the
 * R7KA8P1KFLCAC. The peripheral register bases and the memory map are
 * byte-identical to the RA8D2 -- see the difference-analysis issue and
 * ``libs/ra8_core/inc/ra8_device.h`` -- so the HAL sources serve both chips
 * unchanged; only the Arm Ethos-U55 NPU
 * (``libs/ra8_hal/inc/ra8_npu_regs.h``) is RA8P1-specific. The LED pin the blink
 * drives comes from ``ra8_board_ra8p1``, whose LED/switch/console pins are
 * provisional (mirrored from the pin-compatible EK-RA8D2) until an RA8P1 board
 * is defined; see that layer's header for the ``TODO(EK-RA8P1 UM / ra8p1_kicad)``.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}` means --
 * application-layer code that runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-09
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ra8p1.h"
#include "ra8_boot_entry.h"
#include "ra8_device.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/*
 * Compile-time proof that the RA8P1 toolchain selection reached this TU: the
 * NPU feature flag is only defined for RA8_DEVICE_RA8P1. If this app is ever
 * built with the RA8D2 toolchain, the build fails loudly here instead of
 * silently producing an RA8D2 image under an RA8P1 name.
 */
#ifndef RA8_HAS_NPU
#error "blink_ra8p1 must be built with cmake/toolchain-ra8p1.cmake (RA8_DEVICE_RA8P1)."
#endif

/**
 * @brief CPU clock at reset (MOCO ~8.4 MHz -- shared RA8 CGC default).
 */
typedef enum : uint32_t {
  k_blink_cpu_hz_at_reset = 8400000U, /**< Blink CPU Hz at reset.                      */
  k_blink_half_period_ms  = 500U,     /**< 500 ms on / 500 ms off -> 1 Hz square wave. */
} blink_ra8p1_const_t;

/**
 * @var g_blink_ra8p1_tick
 * @brief Liveness counter, incremented each time the LED toggles.
 *
 * @details `volatile` and non-static so an external debugger (J-Link memprobe)
 *          can watch it advance; the firmware never reads it back.
 *
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_blink_ra8p1_tick = 0U;

/**
 * @brief Park the CPU forever in WFI -- used as a panic stop.
 *
 * @details Preserves the failing initialization context while preventing any
 * further timer, LED, or interrupt activity.
 * @pre A mandatory timebase or LED initialization step has failed.
 * @pre No safe application loop can run for this boot attempt.
 * @post The CPU repeatedly enters WFI until debugger intervention or reset.
 * @post The observable tick and peripheral state remain unchanged after entry.
 * @note No console exists in this minimal foundation app, so failure is silent.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_blink_ra8p1_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

void main(void)
{
  if (ra8_time_init(k_blink_cpu_hz_at_reset) != k_ra8_ok) {
    internal_blink_ra8p1_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_blink_ra8p1_panic_halt();
  }

  ra8_isr_globals_enable();

  while (1) {
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    g_blink_ra8p1_tick += 1U;
    ra8_delay_ms(k_blink_half_period_ms);
  }

  internal_blink_ra8p1_panic_halt();
}

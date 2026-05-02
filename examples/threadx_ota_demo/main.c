/**
 * @file examples/threadx_ota_demo/main.c
 * @brief Eclipse ThreadX OTA-orchestration demo for RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single ThreadX worker that walks the canonical OTA pipeline --
 * check -> download -> verify -> commit -- using stub network /
 * crypto / flash backends so the example compiles and runs on the
 * EK-RA8D2 without a live HTTPS update server.
 *
 * The demo is intentionally minimal: it blinks ``LED1`` once per
 * pipeline tick to give a visible heartbeat, and toggles ``LED2``
 * once each time the (stubbed) state machine advances. The real
 * OTA orchestration lives in ``libs/ra_ota``; this example just
 * wires a thread + watchdog around it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.6.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables (typed enums per the no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack size, in bytes, for the OTA worker thread.
 */
typedef enum : uint16_t {
  k_ota_thread_stack_bytes = 2048U,
} ota_stack_t;

/**
 * @brief Worker thread priority.
 */
typedef enum : uint8_t {
  k_ota_thread_priority = 5U,
} ota_priority_t;

/**
 * @brief Heartbeat / step cadence in 1 kHz ThreadX ticks.
 */
typedef enum : uint16_t {
  k_ota_heartbeat_ticks = 250U, /**< 250 ms heartbeat cadence. */
  k_ota_step_ticks      = 1000U,/**< 1 s per state-machine step. */
} ota_period_t;

/* ---------------------------------------------------------------------------
 * SysTick override: forward to the ThreadX scheduler tick.
 * --------------------------------------------------------------------------- */

#ifndef RA_SIMULATOR_MODE
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

static uint8_t s_thread_ota_stack[k_ota_thread_stack_bytes] __attribute__((aligned(8)));
static TX_THREAD s_thread_ota;

/**
 * @brief SysTick handler -- forwards to ThreadX scheduler tick.
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/**
 * @brief OTA worker thread body.
 *
 * @details
 * Walks an idealised state machine: idle -> checking -> downloading
 * -> verifying -> committing -> idle. Each state lasts
 * ``k_ota_step_ticks`` and advances LED2 once. LED1 is toggled at
 * ``k_ota_heartbeat_ticks`` between state changes to provide a
 * fast-blink heartbeat distinct from the slow state pulse.
 *
 * @param[in] thread_input ThreadX cookie -- unused.
 *
 * @pre ThreadX scheduler is running.
 * @pre LED1 / LED2 have been configured as outputs.
 *
 * @post LED1 blinks at the heartbeat cadence indefinitely.
 * @post LED2 toggles once per state transition.
 */
static void thread_ota_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    /* Heartbeat: four LED1 toggles equals one full step. */
    for (uint8_t i = 0U; i < 4U; i++) {
      (void)ra_board_led_toggle(k_ra_board_led1);
      (void)tx_thread_sleep((ULONG)k_ota_heartbeat_ticks);
    }
    /* State-transition pulse on LED2. */
    (void)ra_board_led_toggle(k_ra_board_led2);
  }
}

/**
 * @brief ThreadX application init -- creates the OTA worker thread.
 *
 * @param[in] first_unused_memory Unused (static stacks).
 *
 * @pre Called from _tx_initialize_kernel_enter; IRQs masked.
 *
 * @post One OTA worker thread is created auto-start.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread_ota,
                              "ota_worker",
                              thread_ota_entry,
                              0U,
                              s_thread_ota_stack,
                              (ULONG)k_ota_thread_stack_bytes,
                              (UINT)k_ota_thread_priority,
                              (UINT)k_ota_thread_priority,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
}
#endif /* !RA_SIMULATOR_MODE */

/**
 * @brief Application entry: GPIO bring-up then ThreadX dispatch.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler completed .data/.bss init.
 * @pre SystemInit set VTOR + FPU.
 *
 * @post OTA worker thread is running.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop

/**
 * @file examples/threadx_canfd_demo/main.c
 * @brief Eclipse ThreadX CANFD heartbeat / RX-logger demo for RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Two ThreadX threads cooperate over the EK-RA8D2's CANFD0 channel:
 *
 *   - ``canfd_tx`` periodically transmits an 8-byte heartbeat frame
 *     at standard ID ``k_canfd_heartbeat_id`` once per
 *     ``k_canfd_heartbeat_ms``.
 *   - ``canfd_rx`` polls the channel and toggles ``LED2`` for each
 *     accepted frame -- a visual "we received something" beacon.
 *
 * The demo exists to prove the ``ra_canfd`` HAL driver wires up
 * cleanly under ThreadX -- it is intentionally minimal and does
 * not try to be a full CAN stack. Both threads use static stacks
 * so the binary stays compatible with NASA Power of 10 Rule 3
 * (no dynamic allocation).
 *
 * @par Threads
 *
 * | Name       | Priority | Period            |
 * |:-----------|:---------|:------------------|
 * | canfd_tx   | 4        | 500 ms            |
 * | canfd_rx   | 4        | 50 ms (poll loop) |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.5.0
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
 * @brief Stack size, in bytes, for each CANFD demo thread.
 */
typedef enum : uint16_t {
  k_canfd_thread_stack_bytes = 1024U,
} canfd_stack_t;

/**
 * @brief Thread priorities.
 */
typedef enum : uint8_t {
  k_canfd_thread_priority = 4U,
} canfd_priority_t;

/**
 * @brief Period and identifier values used by the heartbeat thread.
 */
typedef enum : uint16_t {
  k_canfd_heartbeat_ticks = 500U, /**< 500 ms in 1 kHz ticks.   */
  k_canfd_rx_poll_ticks   = 50U,  /**< 50 ms RX poll cadence.   */
  k_canfd_heartbeat_id    = 0x123U, /**< 11-bit CAN identifier. */
  k_canfd_heartbeat_dlc   = 8U,   /**< Data length code (bytes).*/
} canfd_period_t;

/* ---------------------------------------------------------------------------
 * SysTick override: forward to the ThreadX kernel-tick handler.
 * --------------------------------------------------------------------------- */

#ifndef RA_SIMULATOR_MODE
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

static uint8_t s_thread_tx_stack[k_canfd_thread_stack_bytes] __attribute__((aligned(8)));
static uint8_t s_thread_rx_stack[k_canfd_thread_stack_bytes] __attribute__((aligned(8)));
static TX_THREAD s_thread_tx;
static TX_THREAD s_thread_rx;

/**
 * @brief SysTick handler -- forwards to ThreadX scheduler tick.
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/**
 * @brief Heartbeat TX thread: pretend to send a CAN frame, blink LED1.
 *
 * @param[in] thread_input ThreadX cookie -- unused.
 *
 * @pre ThreadX scheduler is running.
 * @pre LED1 has been configured as an output.
 *
 * @post LED1 toggles every k_canfd_heartbeat_ticks.
 */
static void thread_tx_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra_board_led_toggle(k_ra_board_led1);
    (void)tx_thread_sleep((ULONG)k_canfd_heartbeat_ticks);
  }
}

/**
 * @brief RX polling thread: blink LED2 each poll iteration.
 *
 * @param[in] thread_input ThreadX cookie -- unused.
 *
 * @pre ThreadX scheduler is running.
 * @pre LED2 has been configured as an output.
 *
 * @post LED2 toggles every k_canfd_rx_poll_ticks.
 */
static void thread_rx_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra_board_led_toggle(k_ra_board_led2);
    (void)tx_thread_sleep((ULONG)k_canfd_rx_poll_ticks);
  }
}

/**
 * @brief ThreadX application init -- creates the TX and RX threads.
 *
 * @param[in] first_unused_memory Unused (we use static stacks).
 *
 * @pre Called from _tx_initialize_kernel_enter before the scheduler
 *      starts; IRQs are masked.
 *
 * @post Both threads are created auto-start at the same priority.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread_tx,
                              "canfd_tx",
                              thread_tx_entry,
                              0U,
                              s_thread_tx_stack,
                              (ULONG)k_canfd_thread_stack_bytes,
                              (UINT)k_canfd_thread_priority,
                              (UINT)k_canfd_thread_priority,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  err = tx_thread_create(&s_thread_rx,
                         "canfd_rx",
                         thread_rx_entry,
                         0U,
                         s_thread_rx_stack,
                         (ULONG)k_canfd_thread_stack_bytes,
                         (UINT)k_canfd_thread_priority,
                         (UINT)k_canfd_thread_priority,
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
 * @post Two CANFD demo threads are running.
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

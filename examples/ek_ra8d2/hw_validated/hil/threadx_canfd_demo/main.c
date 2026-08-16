/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_canfd_demo/main.c
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
 * The demo exists to prove the ``ra8_canfd`` HAL driver wires up
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
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_canfd.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables (typed enums per the no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack size, in bytes, for each CANFD demo thread.
 */
typedef enum : uint8_t {
  k_canfd_marker_0 = 0xA5U, /**< CANFD marker 0. */
  k_canfd_marker_1 = 0x5AU, /**< CANFD marker 1. */
  k_canfd_marker_2 = 0x12U, /**< CANFD marker 2. */
  k_canfd_marker_3 = 0x34U, /**< CANFD marker 3. */
  k_canfd_marker_4 = 0x56U, /**< CANFD marker 4. */
  k_canfd_marker_5 = 0x78U, /**< CANFD marker 5. */
  k_canfd_marker_6 = 0x9AU, /**< CANFD marker 6. */
} canfd_marker_t;

typedef enum : uint16_t {
  k_canfd_thread_stack_bytes = 1024U, /**< CANFD thread stack bytes. */
} canfd_stack_t;

/**
 * @brief Thread priorities.
 */
typedef enum : uint8_t {
  k_canfd_thread_priority = 4U, /**< CANFD thread priority. */
} canfd_priority_t;

/**
 * @brief Period and identifier values used by the heartbeat thread.
 */
typedef enum : uint16_t {
  k_canfd_heartbeat_ticks = 500U,   /**< 500 ms in 1 kHz ticks.    */
  k_canfd_rx_poll_ticks   = 50U,    /**< 50 ms RX poll cadence.    */
  k_canfd_heartbeat_id    = 0x123U, /**< 11-bit CAN identifier.    */
  k_canfd_heartbeat_dlc   = 8U,     /**< Data length code (bytes). */
} canfd_period_t;

/**
 * @brief CAN-FD channel + bit-rate constants shared by both threads.
 */
typedef enum : uint32_t {
  k_canfd_demo_channel = 0U,      /**< CANFD0 (only channel on this board). */
  k_canfd_demo_bitrate = 500000U, /**< 500 kbit/s nominal + data rate.      */
} canfd_link_t;

/* SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the project's
 * shared weak SysTick_Handler dispatches to ThreadX (via a weak extern
 * to `_tx_timer_interrupt`) so no per-app override is needed. */

#ifndef RA8_OFF_TARGET
[[gnu::aligned(8)]] static uint8_t s_thread_tx_stack[k_canfd_thread_stack_bytes];
[[gnu::aligned(8)]] static uint8_t s_thread_rx_stack[k_canfd_thread_stack_bytes];
static TX_THREAD                   s_thread_tx;
static TX_THREAD                   s_thread_rx;

/**
 * @var g_threadx_canfd_match
 * @brief HIL success counter -- one increment per successful CAN-FD
 *        transmit (TX thread) or per accepted RX frame (RX thread).
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving real CAN-FD frames are flowing on the
 * internal-loopback path under ThreadX.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_canfd_match = 0U;

/**
 * @var g_threadx_canfd_mismatch
 * @brief HIL failure counter -- one increment per TX or RX error.
 *
 * @details
 * Read externally as HIL_PROBE_FAILURE_SYMBOL. Any non-zero reading
 * means a CAN-FD primitive returned non-ok; the gate fails the run.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_canfd_mismatch = 0U;

/**
 * @brief Heartbeat TX thread: send a CAN-FD frame each period.
 *
 * @details
 * Builds an 8-byte frame at ::k_canfd_heartbeat_id and calls
 * ::ra8_canfd_transmit. The chip is in internal-loopback mode (set in
 * ::tx_application_define) so the frame appears on the RX FIFO for
 * ::internal_thread_rx_entry to consume.
 *
 * @param[in] thread_input ThreadX cookie -- unused.
 *
 * @return None.
 *
 * @pre ThreadX scheduler is running.
 * @pre LED1 has been configured as an output.
 *
 * @post LED1 toggles after every successful transmit.
 * @post g_threadx_canfd_match advances after every successful transmit.
 * @post g_threadx_canfd_mismatch advances on every TX failure.
 *
 * @note Not thread-safe; only this thread calls ra8_canfd_transmit on
 *       this channel.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_thread_tx_entry(ULONG thread_input)
{
  (void)thread_input;
  uint8_t seq = 0U;
  while (1) {
    ra8_canfd_frame_t tx = {
      .id          = (uint32_t)k_canfd_heartbeat_id,
      .dlc         = (uint8_t)k_canfd_heartbeat_dlc,
      .is_extended = 0U,
      .is_fd       = 0U,
      .is_brs      = 0U,
      .data        = {seq,
                      k_canfd_marker_0,
                      k_canfd_marker_1,
                      k_canfd_marker_2,
                      k_canfd_marker_3,
                      k_canfd_marker_4,
                      k_canfd_marker_5,
                      k_canfd_marker_6},
    };
    if (ra8_canfd_transmit((uint8_t)k_canfd_demo_channel, &tx) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      g_threadx_canfd_match += 1U;
    } else {
      g_threadx_canfd_mismatch += 1U;
    }
    seq++;
    (void)tx_thread_sleep((ULONG)k_canfd_heartbeat_ticks);
  }
}

/**
 * @brief RX polling thread: drain the loopback-mirrored frame.
 *
 * @details
 * Polls ::ra8_canfd_receive each ::k_canfd_rx_poll_ticks. On a hit
 * (k_ra8_ok), toggles LED2 and bumps ::g_threadx_canfd_match. No-data
 * returns are expected between TX bursts and never increment either
 * counter; only genuine HW errors advance the mismatch counter.
 *
 * @param[in] thread_input ThreadX cookie -- unused.
 *
 * @return None.
 *
 * @pre ThreadX scheduler is running.
 * @pre LED2 has been configured as an output.
 *
 * @post LED2 toggles after every accepted RX frame.
 * @post g_threadx_canfd_match advances after every accepted RX frame.
 * @post g_threadx_canfd_mismatch advances on every receive error.
 *
 * @note Not thread-safe; only this thread calls ra8_canfd_receive on
 *       this channel.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_thread_rx_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    ra8_canfd_frame_t rx  = {};
    const ra8_err_t   err = ra8_canfd_receive((uint8_t)k_canfd_demo_channel, &rx);
    if (err == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led2);
      g_threadx_canfd_match += 1U;
    } else if (err != k_ra8_err_no_data) {
      g_threadx_canfd_mismatch += 1U;
    }
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
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread_tx,
                              "canfd_tx",
                              internal_thread_tx_entry,
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
                         internal_thread_rx_entry,
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
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Application entry: GPIO bring-up then ThreadX dispatch.
 *
 * @pre Reset_Handler completed .data/.bss init.
 * @pre SystemInit set VTOR + FPU.
 *
 * @post Two CANFD demo threads are running.
 */
void main(void)
{
  /* CGC bring-up FIRST so the ThreadX SysTick reload (programmed in
   * tx_initialize_low_level.S assuming RA8_BOOT_CLOCK_HZ = 1 GHz)
   * ticks at the intended 1 ms wallclock cadence. On MOCO (~8.4 MHz)
   * the tick rate would be 1/119 the expected, making
   * tx_thread_sleep(...) ~119x slower than the ms value declared in
   * source. */
  if (ra8_cgc_init() != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (ra8_canfd_init((uint8_t)k_canfd_demo_channel) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  if (ra8_canfd_set_bitrate((uint8_t)k_canfd_demo_channel,
                            (uint32_t)k_canfd_demo_bitrate,
                            (uint32_t)k_canfd_demo_bitrate) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  /* Internal-loopback (HUM Ch 41 "CFDCnCTR" p 2710): TX frames echo
   * into RX so this single-board demo can validate the round trip
   * without a second CAN node on the bus. */
  if (ra8_canfd_set_test_mode((uint8_t)k_canfd_demo_channel, k_ra8_ctms_self_test_1) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}

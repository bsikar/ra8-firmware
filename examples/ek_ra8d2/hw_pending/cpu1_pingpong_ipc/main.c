/**
 * @file examples/ek_ra8d2/hw_pending/cpu1_pingpong_ipc/main.c
 * @brief CPU0 (M85) IPC ping-pong demo against CPU1 (M33), TZ-secure-boot variant
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * CPU0 side: releases CPU1 via ra_cpu1_release, opens IPC channels,
 * sends 0x1234, waits for 0x4321, loops. The CPU1 image lives next to
 * this file as cpu1_main.c (separate ELF target).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_dual_core.h"
#include "ra_err.h"
#include "ra_ipc.h"

extern uint32_t g_ra_ls_cpu1_mram_start;
extern uint32_t g_ra_ls_cpu1_stack_top;

typedef enum : uint32_t {
  k_cpu1_pingpong_magic_ping = 0x1234U,
  k_cpu1_pingpong_magic_pong = 0x4321U,
  k_cpu1_pingpong_poll_max   = 1000000U,
} cpu1_pingpong_const_t;

typedef enum : uint8_t {
  k_cpu1_pingpong_pair_zero = 0U,
} cpu1_pingpong_pair_t;

/**
 * @var g_cpu1_pingpong_match
 * @brief HIL liveness counter -- incremented on every successful
 *        CPU0 -> CPU1 -> CPU0 ping/pong round-trip (got == 0x4321).
 *
 * @details
 * Read externally by scripts/hil_jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving CPU1 (Cortex-M33) was released, booted its
 * vector table, and is servicing the IPC channel pair. If CPU1 never
 * came out of reset, this counter stays at 0 -- alive-mode could not
 * catch that failure because CPU0 keeps iterating its outer loop.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_match = 0U;

/**
 * @var g_cpu1_pingpong_mismatch
 * @brief HIL failure counter -- incremented whenever a round-trip
 *        cannot complete: TX failed, RX timed out after the bounded
 *        poll, or the returned word was not the expected pong magic.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches "CPU1 hung after release", "IPC
 * FIFO wedged", and "CPU1 echoed the wrong magic" -- previously
 * invisible because the outer ``while (1)`` happily ``continue``s on
 * every failure.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_mismatch = 0U;

/**
 * @var g_cpu1_pingpong_step
 * @brief Boot progress tracker.
 * @details
 * 0 = pre-main; 1 = main() entry; 2 = after ra_cpu1_release;
 * 3 = after ra_ipc_init; 4 = first loop iteration.
 * Lets memprobe pinpoint exactly where CPU0 stalls.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_step = 0U;

/**
 * @var g_cpu1_pingpong_release_err
 * @brief Captured ra_cpu1_release return code.
 * @details Stamped right after the call returns so memprobe can read
 * whichever ra_err_t variant the HAL surfaced.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_release_err = 0xFFFFFFFFU;

/**
 * @brief Drain a single message off an IPC channel with bounded poll.
 * @details Loops up to k_cpu1_pingpong_poll_max times.
 * @param[in]  channel IPC channel id.
 * @param[out] out_msg Receives the dequeued 32-bit word.
 * @return ra_err_t Error code.
 * @retval k_ra_ok          Message received.
 * @retval k_ra_err_timeout No message in time.
 * @pre out_msg non-NULL.
 * @pre channel previously initialized via ra_ipc_init.
 * @post On k_ra_ok, *out_msg holds the peer's payload.
 * @post Loop count bounded by k_cpu1_pingpong_poll_max.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t recv_blocking(uint8_t channel, uint32_t* out_msg)
{
  for (uint32_t i = 0U; i < (uint32_t)k_cpu1_pingpong_poll_max; ++i) {
    ra_err_t err = ra_ipc_recv_message(channel, out_msg);
    if (err == k_ra_ok) {
      return k_ra_ok;
    }
    if (err != k_ra_err_no_data) {
      return err;
    }
  }
  return k_ra_err_timeout;
}

/**
 * @brief CPU0 application entry.
 * @details See file header for behaviour summary.
 * @return Never returns.
 * @retval 0 Never returned.
 * @pre Boot init has completed.
 * @pre CPU1 is held in reset.
 * @post CPU1 released and the ping-pong loop running.
 * @post Function never returns.
 * @note Single-threaded entry.
 * @since 0.1.0
 */
int main(void)
{
  g_cpu1_pingpong_step        = 1U;
  ra_err_t err                = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  g_cpu1_pingpong_release_err = (uint32_t)err;
  g_cpu1_pingpong_step        = 2U;
  if (err != k_ra_ok) {
    while (1) {
      __asm volatile("nop");
    }
  }

  uint8_t ch_send = 0U;
  uint8_t ch_recv = 0U;
  (void)ra_ipc_channel_for_send(k_ra_ipc_core_cpu0, (uint8_t)k_cpu1_pingpong_pair_zero, &ch_send);
  (void)ra_ipc_channel_for_recv(k_ra_ipc_core_cpu0, (uint8_t)k_cpu1_pingpong_pair_zero, &ch_recv);

  ra_ipc_config_t cfg_send = {
    .channel      = ch_send,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = 0U,
  };
  ra_ipc_config_t cfg_recv = {
    .channel      = ch_recv,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra_ipc_event_msg_ready,
  };
  (void)ra_ipc_init(&cfg_send);
  (void)ra_ipc_init(&cfg_recv);
  g_cpu1_pingpong_step = 3U;

  while (1) {
    g_cpu1_pingpong_step = 4U;
    if (ra_ipc_send_message(ch_send, (uint32_t)k_cpu1_pingpong_magic_ping) != k_ra_ok) {
      g_cpu1_pingpong_mismatch += 1U;
      continue;
    }
    uint32_t got = 0U;
    if (recv_blocking(ch_recv, &got) != k_ra_ok) {
      g_cpu1_pingpong_mismatch += 1U;
      continue;
    }
    if (got != (uint32_t)k_cpu1_pingpong_magic_pong) {
      g_cpu1_pingpong_mismatch += 1U;
      continue;
    }
    g_cpu1_pingpong_match += 1U;
  }
}

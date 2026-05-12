/**
 * @file examples/ek_ra8d2/cpu1_pingpong/main.c
 * @brief CPU0 (Cortex-M85) ping-pong demo against CPU1 (Cortex-M33)
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
  ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
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

  while (1) {
    if (ra_ipc_send_message(ch_send, (uint32_t)k_cpu1_pingpong_magic_ping) != k_ra_ok) {
      continue;
    }
    uint32_t got = 0U;
    if (recv_blocking(ch_recv, &got) != k_ra_ok) {
      continue;
    }
    if (got != (uint32_t)k_cpu1_pingpong_magic_pong) {
      continue;
    }
  }
}

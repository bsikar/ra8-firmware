/**
 * @file ra_gpt.c
 * @brief General PWM Timer (GPT) driver implementation
 *
 * @details
 * Provides a minimal "free-running 32-bit timer" interface on top of
 * the GPT register block. A full PWM / compare-match driver will
 * land once the motor-control layer needs it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_gpt.h"

#include <stdint.h>

#include "ra8d2_gpt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "GPT";

/**
 * @var s_gpt_mstp_table
 * @brief Channel-index -> MSTP id lookup. GPT4..GPT9 share a single
 *        bit (MSTPE27); the other channels each have their own.
 *        Sized by ``k_ra_gpt_channel_count`` from ``ra8d2_gpt_regs.h``.
 *        HUM Ch 11.2.10 "MSTPCRE", p 449..450.
 */
static const ra_mstp_t s_gpt_mstp_table[k_ra_gpt_channel_count] = {
  k_ra_mstp_gpt0,
  k_ra_mstp_gpt1,
  k_ra_mstp_gpt2,
  k_ra_mstp_gpt3,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt10,
  k_ra_mstp_gpt11,
  k_ra_mstp_gpt12,
  k_ra_mstp_gpt13,
};

/**
 * @enum ra_gtwp_t
 * @brief GTWP write-protect key.
 */
typedef enum : uint32_t {
  k_ra_gtwp_key_unlock = 0xA500U, /**< Password in upper byte, WP=0. */
  k_ra_gtwp_key_lock   = 0xA501U, /**< Password in upper byte, WP=1. */
} ra_gtwp_t;

ra_err_t ra_gpt_start_free_run(uint8_t channel, uint32_t period)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  const ra_err_t mst_err = ra_mstp_enable(s_gpt_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gpt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = 1UL;          /* Stop if running. */
  reg->GTCR  = 0x00000001UL; /* Saw-wave PWM mode. */
  reg->GTPR  = period;
  reg->GTCNT = 0U;
  reg->GTSTR = 1UL; /* Start. */
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_gpt_stop(uint8_t channel)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = 1UL;
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;
  return k_ra_ok;
}

ra_err_t ra_gpt_read(uint8_t channel, uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  *out = reg->GTCNT;
  return k_ra_ok;
}

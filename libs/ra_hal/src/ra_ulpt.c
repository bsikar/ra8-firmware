/**
 * @file ra_ulpt.c
 * @brief Ultra-Low-Power Timer (ULPT) driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ulpt.h"

#include <stdint.h>

#include "ra8d2_ulpt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ULPT";

/**
 * @var s_ulpt_mstp_table
 * @brief Channel-index -> MSTP id lookup. ULPT0/1 share MSTPCRE
 *        bits E9/E8 per HUM Ch 11.2.10 p 449.
 */
static const ra_mstp_t s_ulpt_mstp_table[] = {
  k_ra_mstp_ulpt0,
  k_ra_mstp_ulpt1,
};

[[nodiscard]] ra_err_t ra_ulpt_init(void)
{
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_ulpt_channel_count; ++ch) {
    volatile r_ulpt_regs_t* reg = ra_ulpt(ch);
    if (reg == nullptr) {
      return k_ra_err_hw_init_failed;
    }
    /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
    const ra_err_t mst_err = ra_mstp_enable(s_ulpt_mstp_table[ch]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "ulpt_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */
    reg->ULPTCR  = 0U;
    reg->ULPTMR1 = 0U;
    reg->ULPTMR2 = 0U;
    reg->ULPTMR3 = 0U;
    reg->ULPTIOC = 0U;
    reg->ULPT    = 0U;
  }
  ra_log_info(s_tag, "ulpt_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ulpt_start(uint8_t channel, uint32_t period)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->ULPTCR  = 0U; /* Ensure stopped before reload. */
  reg->ULPTMR1 = 0U; /* Free-running, sub-clock source. */
  reg->ULPTMR2 = 0U;
  reg->ULPTMR3 = 0U;
  reg->ULPT    = period;
  reg->ULPTCR  = (uint8_t)k_ra_ulpt_mask_tstart;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ulpt_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->ULPTCR = (uint8_t)k_ra_ulpt_mask_tstop;
  reg->ULPTCR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * Wave 4.3 -- full build-out
 * =============================================================================
 */

static ra_ulpt_event_fn_t s_ulpt_fn;
static void*              s_ulpt_ctx;

ra_err_t ra_ulpt_deinit(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->ULPTCR = 0U;
  return ra_mstp_disable(s_ulpt_mstp_table[channel]);
}

ra_err_t ra_ulpt_set_period(uint8_t channel, uint32_t period)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");
  reg->ULPT = period;
  return k_ra_ok;
}

ra_err_t ra_ulpt_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_mask = ra_ulpt(channel)->ULPTCR;
  return k_ra_ok;
}

ra_err_t ra_ulpt_attach_handler(ra_ulpt_event_fn_t fn, void* ctx)
{
  s_ulpt_fn  = fn;
  s_ulpt_ctx = ctx;
  return k_ra_ok;
}

void ra_ulpt_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return;
  }
  const ra_ulpt_event_fn_t fn  = s_ulpt_fn;
  void* const              ctx = s_ulpt_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}

ra_err_t ra_ulpt_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_ulpt_mstp_table[channel]);
}

ra_err_t ra_ulpt_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_ulpt_mstp_table[channel]);
}

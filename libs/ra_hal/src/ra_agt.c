/**
 * @file ra_agt.c
 * @brief Low Power Asynchronous General Purpose Timer driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 AGT block (10 channels total; only
 * AGT0 / AGT1 have dedicated MSTPD bits, the rest share the
 * sub-clock path). Programmes an AGT channel as a free-running
 * 16-bit down-counter clocked from PCLKB. Used as a coarse tick
 * source on boards where SysTick is not desirable. Every register
 * access carries a HUM Ch 24 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_agt.h"

#include <stdint.h>

#include "ra8d2_agt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "AGT";

/**
 * @enum ra_agt_mstp_limit_t
 * @brief Number of AGT channels that have dedicated MSTPD bits.
 *
 * @details
 * HUM Ch 11.2.9 p 448 only lists MSTPD4 / MSTPD5 for AGT1 / AGT0.
 * The chip lays out 10 AGT channels total but only the first two
 * are ref-counted through ra_mstp; additional channels inherit
 * their clock from the shared LOCO / sub-clock path.
 */
typedef enum : uint8_t {
  k_ra_agt_mstp_id_count = 2U,
} ra_agt_mstp_limit_t;

/**
 * @var s_agt_mstp_table
 * @brief Channel-index -> MSTP id lookup for AGT0 / AGT1.
 */
static const ra_mstp_t s_agt_mstp_table[k_ra_agt_mstp_id_count] = {
  k_ra_mstp_agt0,
  k_ra_mstp_agt1,
};

[[nodiscard]] ra_err_t ra_agt_start_free_run(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  if (channel < (uint8_t)k_ra_agt_mstp_id_count) {
    /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
    const ra_err_t mst_err = ra_mstp_enable(s_agt_mstp_table[channel]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "agt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */
  }

  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  /* HUM Ch 24.2.2 "AGTMR1 : AGT Mode Register 1" p 1169 -- timer mode,
   * PCLKB source. */
  reg->AGTMR1 = 0U;
  /* HUM Ch 24.2.3 "AGTMR2 : AGT Mode Register 2" p 1170 */
  reg->AGTMR2 = 0U;
  /* HUM Ch 24.2.4 "AGT : AGT Counter" p 1170 */
  reg->AGT = reload;
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  /* TSTART = 1. */
  reg->AGTCR = 0x01U;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_agt_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

static ra_agt_event_fn_t s_agt_fn;
static void*             s_agt_ctx;

ra_err_t ra_agt_deinit(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  if (channel < (uint8_t)k_ra_agt_mstp_id_count) {
    return ra_mstp_disable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}

ra_err_t ra_agt_set_reload(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGT : AGT Counter" p 1170 */
  reg->AGT = reload;
  return k_ra_ok;
}

ra_err_t ra_agt_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  *out_mask = reg->AGTCR;
  return k_ra_ok;
}

ra_err_t ra_agt_attach_handler(ra_agt_event_fn_t fn, void* ctx)
{
  s_agt_fn  = fn;
  s_agt_ctx = ctx;
  return k_ra_ok;
}

void ra_agt_dispatch(uint8_t channel)
{
  if (ra_agt(channel) == nullptr) {
    return;
  }
  const ra_agt_event_fn_t fn  = s_agt_fn;
  void* const             ctx = s_agt_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}

ra_err_t ra_agt_enter_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  if (channel < (uint8_t)k_ra_agt_mstp_id_count) {
    return ra_mstp_disable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}

ra_err_t ra_agt_exit_stop(uint8_t channel)
{
  if (ra_agt(channel) == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (channel < (uint8_t)k_ra_agt_mstp_id_count) {
    return ra_mstp_enable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}

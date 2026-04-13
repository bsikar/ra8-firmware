/**
 * @file ra_agt.c
 * @brief Asynchronous General-Purpose Timer (AGT) driver
 *
 * @details
 * Minimal driver that programmes an AGT channel as a free-running
 * 16-bit down-counter clocked from PCLKB. Used as a coarse tick
 * source on boards where SysTick is not desirable.
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

  reg->AGTCR  = 0U; /* Stop.                      */
  reg->AGTMR1 = 0U; /* Timer mode, PCLKB source.  */
  reg->AGTMR2 = 0U;
  reg->AGT    = reload;
  reg->AGTCR  = 0x01U; /* TSTART.                 */

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_agt_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  reg->AGTCR = 0U;
  return k_ra_ok;
}

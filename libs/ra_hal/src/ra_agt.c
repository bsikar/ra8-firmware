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

static const char* s_tag = "AGT";

[[nodiscard]] ra_err_t ra_agt_start_free_run(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

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

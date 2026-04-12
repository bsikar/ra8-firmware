/**
 * @file ra_canfd.c
 * @brief CANFD Lite driver framework
 *
 * @details
 * Drives `CFDCNCTR.CHMDC` through the documented state transitions
 * (reset -> halt -> operation) per RA8D2 HUM. Full bit-timing,
 * filter configuration, and TX queue handling come with the first
 * real CAN consumer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_canfd.h"

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "CANFD";

static ra_err_t internal_wait_mode(volatile r_canfd_channel_regs_t* reg, uint8_t status_bit)
{
  enum : uint32_t { k_ra_canfd_spin = 200000U };
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {
    if ((reg->CFDCNSTS & (uint32_t)(1UL << status_bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_canfd_init(uint8_t channel)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* Ensure we are in reset. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_reset;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_crstst);

  /* Move to halt. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_halt;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_chltst);

  /* Move to operation. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_operation;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_crstst);

  ra_log_info_val(s_tag, "canfd_init ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_canfd_deinit(uint8_t channel)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_reset;
  return k_ra_ok;
}

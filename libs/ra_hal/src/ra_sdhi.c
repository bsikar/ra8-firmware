/**
 * @file ra_sdhi.c
 * @brief SD/MMC Host Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 SDHI block. Covers lifecycle +
 * status + IRQ + power-transition surface plus the command
 * primitive set: ``ra_sdhi_send_command`` (load SD_ARG + SD_CMD,
 * poll SD_INFO1.RSPEND, copy SD_RSP*) and ``ra_sdhi_set_clock``
 * for runtime bus-rate change. Block data transfer (SD_BUF FIFO
 * + DMA bounce buffer) is left to the first card-stack consumer
 * because the protocol decisions (4-bit / 8-bit, SDR/DDR) belong
 * to the consumer. Every register access carries a HUM Ch 47
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sdhi.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_sdhi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "SDHI";

/**
 * @var s_sdhi_mstp_table
 * @brief Per-instance MSTP id lookup.
 */
static const ra_mstp_t s_sdhi_mstp_table[k_ra_sdhi_instance_count] = {
  k_ra_mstp_sdhi0,
  k_ra_mstp_sdhi1,
};

static ra_sdhi_event_fn_t s_sdhi_fn;
static void*              s_sdhi_ctx;

ra_err_t ra_sdhi_init(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_sdhi_mstp_table[instance]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "sdhi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  /* Clear status + mask registers to a known baseline. */
  reg->SD_INFO1      = 0U;
  reg->SD_INFO2      = 0U;
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  ra_log_info_val(s_tag, "sdhi_init inst", (uint32_t)instance);
  return k_ra_ok;
}

ra_err_t ra_sdhi_deinit(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  reg->SD_INFO1      = 0U;
  reg->SD_INFO2      = 0U;
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  reg->SD_CLK_CTRL   = 0U;
  return ra_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra_err_t ra_sdhi_send_command(uint8_t   instance,
                              uint32_t  cmd,
                              uint32_t  arg,
                              uint32_t* out_rsp) // NOLINT(readability-non-const-parameter)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  /* Load SD_ARG, then SD_CMD kicks the command out on the bus. */
  reg->SD_ARG = arg;
  reg->SD_CMD = cmd;

  /* Poll SD_INFO1.RSPEND (bit 0) with bounded spin budget. */
  enum : uint32_t { k_ra_sdhi_cmd_spin = 200000U };
  for (uint32_t i = 0U; i < k_ra_sdhi_cmd_spin; ++i) {
    if ((reg->SD_INFO1 & 1UL) != 0UL) {
      if (out_rsp != nullptr) {
        out_rsp[0] = reg->SD_RSP10;
        out_rsp[1] = reg->SD_RSP32;
        out_rsp[2] = reg->SD_RSP54;
        out_rsp[3] = reg->SD_RSP76;
      }
      reg->SD_INFO1 = reg->SD_INFO1 & ~1UL;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_sdhi_set_clock(uint8_t instance, uint32_t divider)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  /* SD_CLK_CTRL low byte holds the divider field. */
  reg->SD_CLK_CTRL = divider;
  return k_ra_ok;
}

ra_err_t ra_sdhi_get_status(uint8_t instance, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  *out_mask = reg->SD_INFO1;
  return k_ra_ok;
}

ra_err_t ra_sdhi_clear_status(uint8_t instance, uint32_t mask)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47 "SD/MMC Host Interface (SDHI)" p 3122 */
  reg->SD_INFO1 = reg->SD_INFO1 & ~mask;
  return k_ra_ok;
}

ra_err_t ra_sdhi_attach_handler(ra_sdhi_event_fn_t fn, void* ctx)
{
  s_sdhi_fn  = fn;
  s_sdhi_ctx = ctx;
  return k_ra_ok;
}

void ra_sdhi_dispatch(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  if (reg == nullptr) {
    return;
  }
  const uint32_t           mask = reg->SD_INFO1;
  const ra_sdhi_event_fn_t fn   = s_sdhi_fn;
  void* const              ctx  = s_sdhi_ctx;
  reg->SD_INFO1                 = 0U;
  if (fn != nullptr) {
    fn(ctx, instance, mask);
  }
}

ra_err_t ra_sdhi_enter_stop(uint8_t instance)
{
  if (instance >= k_ra_sdhi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra_err_t ra_sdhi_exit_stop(uint8_t instance)
{
  if (instance >= k_ra_sdhi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_sdhi_mstp_table[instance]);
}

/**
 * @file ra_dmac.c
 * @brief DMAC driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dmac.h"

#include <stdint.h>

#include "ra8d2_dmac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "DMAC";

typedef enum : uint16_t {
  k_ra_dmtmd_sz_shift   = 8U,  /**< DMTMD.SZ[9:8] transfer size. */
  k_ra_dmamd_sm_shift   = 14U, /**< DMAMD.SM[15:14] src inc.     */
  k_ra_dmamd_dm_shift   = 6U,  /**< DMAMD.DM[7:6] dst inc.       */
  k_ra_dmamd_inc_addr   = 2U,  /**< Incrementing address.        */
  k_ra_dmamd_fixed_addr = 0U,  /**< Fixed address.               */
} ra_dmac_mode_t;

typedef enum : uint8_t {
  k_ra_dmcnt_enable = 1U, /**< DMCNT bit 0: channel enable. */
} ra_dmcnt_bit_t;

ra_err_t ra_dmac_start(uint8_t channel, const ra_dmac_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }

  /* DMAC0 + DTC0 share MSTPA22; the ref count tracks how many
   * DMAC channels (or the DTC) currently need the bit cleared.
   * HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dmac_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  reg->DMCNT = 0U;
  reg->DMSAR = cfg->src;
  reg->DMDAR = cfg->dst;
  reg->DMCRA = (uint32_t)cfg->count;

  reg->DMTMD = (uint16_t)((uint16_t)cfg->width << k_ra_dmtmd_sz_shift);

  uint16_t dmamd = 0U;
  if (cfg->src_inc) {
    dmamd |= (uint16_t)(k_ra_dmamd_inc_addr << k_ra_dmamd_sm_shift);
  }
  if (cfg->dst_inc) {
    dmamd |= (uint16_t)(k_ra_dmamd_inc_addr << k_ra_dmamd_dm_shift);
  }
  reg->DMAMD = dmamd;

  reg->DMCNT = k_ra_dmcnt_enable;

  ra_log_info_val(s_tag, "dmac_start ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_dmac_stop(uint8_t channel)
{
  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }
  reg->DMCNT = 0U;
  /* Drop the matching reference acquired in ra_dmac_start. */
  return ra_mstp_disable(k_ra_mstp_dmac0_dtc0);
}

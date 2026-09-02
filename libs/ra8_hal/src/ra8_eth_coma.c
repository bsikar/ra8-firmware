/**
 * @file ra8_eth_coma.c
 * @brief Ethernet Common Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 COMA block. Shares the ESWM MSTP
 * gate with the rest of the ethernet subsystem. Every register
 * access carries a HUM Ch 31 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_eth_coma.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "ETHCMA";

/**
 * @enum ra8_eth_coma_delay_t
 * @brief Busy-wait iteration counts for the COMA bring-up sequence.
 *
 * @details
 * Cortex-M85 at ~1 GHz, ~3 cycles per ``nop`` -> 3,000,000 iters lands
 * around 9 ms. FSP ``r_layer3_switch_reset_coma`` uses
 * ``R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS)`` for the same
 * settle points (~1 ms is sufficient); ~3 ms is kept for margin.
 * ``bpr_poll_max`` bounds the CABPIRM.BPR poll: HUM Ch 31.3.2.7 says BPR
 * sets at clk_period x 512 from the start of buffer-pool init -- well
 * under a microsecond -- so 1,000,000 register-read iterations is a
 * multi-millisecond safety ceiling that satisfies NASA P10 Rule 2.
 */
typedef enum : uint32_t {
  k_ra8_eth_coma_delay_iters  = 3000000UL, /**< ~1-3 ms busy wait between COMA writes. */
  k_ra8_eth_coma_bpr_poll_max = 1000000UL, /**< CABPIRM.BPR poll upper bound.          */
} ra8_eth_coma_delay_t;

static ra8_eth_coma_event_fn_t s_coma_fn;
static void*                   s_coma_ctx;

ra8_err_t ra8_eth_coma_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "coma_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_STS  = 0U;
  reg->COMA_IE   = 0U;
  reg->COMA_ICLR = 0U;
  ra8_log_info(s_tag, "coma_init");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_deinit(void)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_IE   = 0U;
  s_coma_fn      = nullptr;
  s_coma_ctx     = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_coma_bringup(void)
{
  /* Step 1: pulse RRC.RR (1 then 0) to reset the ESWM IP, then settle. */
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *ra8_coma_rrc() = (uint32_t)k_ra8_coma_rrc_rr;
  *ra8_coma_rrc() = 0U;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }

  /* Step 2: enable the switch clock (RCE) alone, then settle. */
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *ra8_coma_rcec() = (uint32_t)k_ra8_coma_rcec_rce;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }

  /* Step 3: kick the buffer-pool init and wait for BPR. Writing BPIOG = 1
   * starts the pool reset; BPR self-sets clk_period x 512 later. On the host
   * build CABPIRM is mmap'd RAM with no hardware to self-set BPR, so the
   * shared bounded waiter consults the ra8_fake_mmio fault seam -- first-poll
   * success unless a test arms a timeout on this register. */
  /* HUM Ch 31.3.2.7 "CABPIRM" p 1599 */
  *ra8_coma_cabpirm() = (uint32_t)k_ra8_coma_cabpirm_bpiog;
  /* HUM Ch 31.3.2.7 "CABPIRM" p 1599 */
  const ra8_err_t bpr_err = ra8_hw_wait_flag_set32(ra8_coma_cabpirm(),
                                                   (uint32_t)k_ra8_coma_cabpirm_bpr,
                                                   (uint32_t)k_ra8_eth_coma_bpr_poll_max);
  if (bpr_err != k_ra8_ok) {
    ra8_log_error(s_tag, "coma_bringup: CABPIRM.BPR timeout");
    return bpr_err;
  }

  /* Step 4: fan out every per-agent clock (RCE | ACE[6:0] = ALL) so
   * RMAC0/1 + ETHA0/1 + GWCA + MFWD + GPTP all become accessible. */
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *ra8_coma_rcec() = (uint32_t)k_ra8_coma_rcec_rce | (uint32_t)k_ra8_coma_rcec_ace_mask;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }
  ra8_log_info(s_tag, "coma_bringup");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *out_mask = ra8_coma()->COMA_STS;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_clear_status(uint32_t mask)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_ICLR = mask;
  reg->COMA_STS  = reg->COMA_STS & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_attach_handler(ra8_eth_coma_event_fn_t fn, void* ctx)
{
  s_coma_fn  = fn;
  s_coma_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_eth_coma_dispatch(void)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  const uint32_t                mask = reg->COMA_STS;
  const ra8_eth_coma_event_fn_t fn   = s_coma_fn;
  void* const                   ctx  = s_coma_ctx;
  reg->COMA_ICLR                     = mask;
  reg->COMA_STS                      = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_eth_coma_enter_stop(void)
{
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  ra8_coma()->COMA_CTRL = 0U;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_coma_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_eswm);
}

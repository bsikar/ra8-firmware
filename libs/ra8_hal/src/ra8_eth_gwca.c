/**
 * @file ra8_eth_gwca.c
 * @brief Ethernet CPU Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. This translation unit holds the
 * lifecycle / status / dispatch surface plus the GWCA state-machine
 * bring-up (set_operation_mode / axi_init / install_linkfix /
 * bring_up); the per-queue descriptor primitives live in
 * src/internal/eth_gwca_queue.zig and the one-call default-state API in
 * ra8_eth_gwca_default.c. Every register access carries a HUM Ch 34
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_eth_gwca.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth_gwca_internal.h"
#include "ra8_ether_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* const s_tag = "ETHGWC";

/* Defined in src/eth_gwca_events_abi.zig (RA8FW-847); init/deinit clear them. */
extern ra8_eth_gwca_event_fn_t s_gwca_fn;
extern void*                   s_gwca_ctx;

/**
 * @enum ra8_eth_gwca_init_layout_t
 * @brief MFWD FWPC10/11/12 offsets used to enable extended descriptors.
 *
 * @details Per FSP r_layer3_switch open path, every agent (GWCA, ETHA0,
 * ETHA1) needs FWPC1n.DDE = 1 BEFORE the GWCA mode transitions, or
 * GWARIRM.ARR never asserts during the AXI init handshake (the chip
 * silently rejects the request). Bench-confirmed on EK-RA8D2.
 */
typedef enum : uint32_t {
  k_ra8_mfwd_off_fwpc10 = 0x104UL, /**< FWPC10 (GWCA agent).  */
  k_ra8_mfwd_off_fwpc11 = 0x114UL, /**< FWPC11 (ETHA0 agent). */
  k_ra8_mfwd_off_fwpc12 = 0x124UL, /**< FWPC12 (ETHA1 agent). */
  k_ra8_mfwd_fwpc_dde   = 0x1UL,   /**< DDE bit position 0.   */
} ra8_eth_gwca_init_layout_t;

ra8_err_t ra8_eth_gwca_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "gwca_init: mstp enable");

  volatile r_gwca_regs_t* reg = ra8_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_STS  = 0U;
  reg->GWCA_IE   = 0U;
  reg->GWCA_ICLR = 0U;

  /* Enable extended descriptor format on each agent before any GWCA
   * mode transition. Without this the AXI init handshake (GWARIRM.ARR)
   * never asserts. Mirrors FSP r_layer3_switch open path. */
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  volatile uint32_t* const fwpc10 =
    (volatile uint32_t*)(k_ra8_mfwd_base_addr + (uintptr_t)k_ra8_mfwd_off_fwpc10);
  volatile uint32_t* const fwpc11 =
    (volatile uint32_t*)(k_ra8_mfwd_base_addr + (uintptr_t)k_ra8_mfwd_off_fwpc11);
  volatile uint32_t* const fwpc12 =
    (volatile uint32_t*)(k_ra8_mfwd_base_addr + (uintptr_t)k_ra8_mfwd_off_fwpc12);
  *fwpc10 = (*fwpc10 & ~(uint32_t)k_ra8_mfwd_fwpc_dde) | (uint32_t)k_ra8_mfwd_fwpc_dde;
  *fwpc11 = (*fwpc11 & ~(uint32_t)k_ra8_mfwd_fwpc_dde) | (uint32_t)k_ra8_mfwd_fwpc_dde;
  *fwpc12 = (*fwpc12 & ~(uint32_t)k_ra8_mfwd_fwpc_dde) | (uint32_t)k_ra8_mfwd_fwpc_dde;

  ra8_log_info(s_tag, "gwca_init");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gwca_deinit(void)
{
  volatile r_gwca_regs_t* reg = ra8_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_IE   = 0U;
  s_gwca_fn      = nullptr;
  s_gwca_ctx     = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_gwca_enter_stop(void)
{
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  ra8_gwca()->GWCA_CTRL = 0U;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

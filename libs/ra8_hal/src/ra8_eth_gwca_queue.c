/**
 * @file ra8_eth_gwca_queue.c
 * @brief Ethernet CPU Agent driver -- per-queue descriptor + ring primitives
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * ra8_eth_gwca_reload_queue only. The rest of the per-queue and
 * per-descriptor surface (configure_queue, init_ring,
 * set_descriptor_buffer, attach_buffers, kick_tx, find_slot, tx_frame
 * and the address-encoding helpers) moved to Zig in RA8FW-749
 * (src/internal/eth_gwca_queue.zig, src/eth_gwca_queue_abi.zig).
 * reload_queue stays C because its BALR poll goes through
 * ra8_hw_wait_flag_clear32, whose host fake-MMIO wait seam the C suite
 * arms. Register access carries a HUM Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth_gwca.h"
#include "ra8_eth_gwca_internal.h"
#include "ra8_ether_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"

static const char* const s_tag = "ETHGWC";

/**
 * @brief Reload a descriptor queue: pulse GWDCC[i].BALR, wait for clear.
 *
 * @details HUM Ch 34.3 "GWDCCi" p 1811 defines BALR (Base Address
 * Load Request) as the request that resets the AXI address RAM
 * current_address field for queue i to the chain base
 * ({GWDCBAC} + i x 8). Until BALR runs, the GWCA never scans the
 * descriptor chain -- every RX descriptor stays FEMPTY and no frame
 * is delivered. BALR self-clears once the reload completes. FSP
 * r_layer3_switch.c::R_LAYER3_SWITCH_StartDescriptorQueue performs
 * this same pulse, with the GWCA in OPERATION mode.
 *
 * @param[in] queue_index GWCA descriptor-queue number 0..63.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             BALR pulsed and self-cleared.
 * @retval k_ra8_err_invalid_arg queue_index has no GWDCC register.
 * @retval k_ra8_err_hw_timeout  BALR never self-cleared.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre ::ra8_eth_gwca_configure_queue ran for queue_index.
 * @post The AXI address RAM current_address for queue_index points at
 *       the chain base.
 * @post GWDCC[queue_index].BALR reads 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_eth_gwca_reload_queue(uint32_t queue_index)
{
  volatile uint32_t* const gwdcc = ra8_gwca_gwdcc(queue_index);
  if (gwdcc == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 34.3 "GWDCCi" p 1811: BALR self-clears once the GWCA has
   * reset the AXI address RAM current_address pointer. */
  *gwdcc |= (uint32_t)k_ra8_gwdcc_balr;
  const ra8_err_t err =
    ra8_hw_wait_flag_clear32(gwdcc, (uint32_t)k_ra8_gwdcc_balr, (uint32_t)k_ra8_eth_gwca_balr_spin);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "reload_queue: GWDCC BALR never cleared");
  }
  return err;
}

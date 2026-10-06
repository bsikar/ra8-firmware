/**
 * @file ra8_eth_gwca_default.c
 * @brief Ethernet CPU Agent driver -- one-call default-state API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The default-state convenience surface of the RA8D2 GWCA block,
 * split out of ra8_eth_gwca.c to stay under the per-file line-count
 * cap: default_open (with its bring-up / ring / queue sub-helpers).
 * default_send, default_recv and rx_frame are Zig now
 * (src/eth_gwca_send_abi.zig, eth_gwca_recv_abi.zig, eth_gwca_rx_abi.zig).
 * Every register access
 * carries a HUM Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth_gwca.h"
#include "ra8_eth_gwca_internal.h"
#include "ra8_ether_regs.h"
#include "ra8_hw_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"

static const char* const s_tag = "ETHGWC";

/**
 * @brief Initialise the extended (16-byte) TX descriptor chain.
 *
 * @details The TX queue uses GWCA extended descriptors (EDE = 1) so
 * every frame carries its INFO1 routing metadata. This helper primes
 * the chain: entries 0..depth-2 become FEMPTY data slots with
 * ds = slot_bytes and PTR = pool + i * slot_bytes; the last entry
 * becomes a LINK terminator wrapping to chain[0]. INFO1 is zeroed
 * here and populated per-frame by ::ra8_eth_gwca_default_send. It is
 * the 16-byte-descriptor analogue of ::ra8_eth_gwca_init_ring +
 * ::ra8_eth_gwca_attach_buffers, which only handle 8-byte basic
 * descriptors.
 *
 * @param[in,out] chain      Caller-owned extended-descriptor array.
 * @param[in]     depth      Number of entries (>= 2).
 * @param[in]     slot_bytes Per-slot buffer size in bytes (<= 2048).
 * @param[in]     pool       Contiguous buffer pool, >= (depth-1)*slot_bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Chain primed.
 * @retval k_ra8_err_null_ptr    chain or pool is null.
 * @retval k_ra8_err_invalid_arg depth < 2 or slot_bytes > 2048.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre chain is 16-byte aligned.
 * @post chain[0..depth-2] have dt = FEMPTY, ds = slot_bytes, PTR set.
 * @post chain[depth-1] has dt = LINK with PTR = &chain[0].
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_tx_ext_init(ra8_gwca_ext_descriptor_t* chain,
                                      uint32_t                   depth,
                                      uint32_t                   slot_bytes,
                                      const uint8_t*             pool)
{
  RA8_CHECK_NULL_PTR(chain, s_tag, "tx_ext_init: chain null");
  RA8_CHECK_NULL_PTR(pool, s_tag, "tx_ext_init: pool null");
  enum : uint32_t {
    k_tx_ext_min_depth = 2U,    /**< One FEMPTY slot + one LINK terminator. */
    k_tx_ext_max_bytes = 2048U, /**< HUM DS field is 12 bits.               */
    k_ds_low_mask      = 0xFFU, /**< ds_l carries 8 bits.                   */
    k_ds_high_shift    = 8U,    /**< ds_h packs the upper 4 bits.           */
    k_ds_high_mask     = 0xFU,  /**< ds_h field width.                      */
  };
  if (depth < k_tx_ext_min_depth) {
    return k_ra8_err_invalid_arg;
  }
  if (slot_bytes > k_tx_ext_max_bytes) {
    return k_ra8_err_invalid_arg;
  }
  enum : uintptr_t {
    k_ptr_hi_shift = 32U,     /**< PTR[39:32] lives 32 bits up. */
    k_ptr_hi_mask  = 0xFFULL, /**< PTR high byte width.         */
  };
  for (uint32_t i = 0U; i < (depth - 1U); ++i) {
    (void)memset(&chain[i], 0, sizeof(ra8_gwca_ext_descriptor_t));
    chain[i].dt         = (uint8_t)k_ra8_gwdcc_dt_fempty;
    chain[i].ds_l       = (uint8_t)(slot_bytes & k_ds_low_mask);
    chain[i].ds_h       = (uint8_t)((slot_bytes >> k_ds_high_shift) & k_ds_high_mask);
    const uintptr_t buf = (uintptr_t)pool + ((uintptr_t)i * (uintptr_t)slot_bytes);
    chain[i].ptr_h      = (uint8_t)(((uint64_t)buf >> k_ptr_hi_shift) & (uint64_t)k_ptr_hi_mask);
    chain[i].ptr_l      = (uint32_t)buf;
  }
  ra8_gwca_ext_descriptor_t* const term = &chain[depth - 1U];
  (void)memset(term, 0, sizeof(ra8_gwca_ext_descriptor_t));
  const uintptr_t head = (uintptr_t)&chain[0];
  term->ptr_h          = (uint8_t)(((uint64_t)head >> k_ptr_hi_shift) & (uint64_t)k_ptr_hi_mask);
  term->ptr_l          = (uint32_t)head;
  term->dt             = (uint8_t)k_ra8_gwdcc_dt_link;
  return k_ra8_ok;
}

/**
 * @brief Set up the RX + TX descriptor rings for the default-state API.
 *
 * @details Helper called by ra8_eth_gwca_default_open. Primes the RX
 * chain (8-byte basic descriptors) via init_ring + attach_buffers and
 * the TX chain (16-byte extended descriptors) via
 * ::internal_tx_ext_init so the top-level function stays under the
 * 60-line / 40-statement budget.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra8_err_t Error code propagated from init_ring/attach_buffers.
 * @retval k_ra8_ok              Both rings primed.
 * @retval k_ra8_err_invalid_arg Depth/slot/pool inconsistent.
 * @retval k_ra8_err_null_ptr    Required pointer field is null.
 *
 * @pre state->rx_chain / tx_chain are 16-byte aligned arrays of
 *      ra8_gwca_basic_descriptor_t.
 * @pre state->rx_pool / tx_pool point to rx_depth * rx_slot_bytes
 *      (resp. tx_*) of payload backing.
 * @post On success every RX/TX descriptor has dt = FEMPTY and PTR
 *       pointing into the matching pool.
 * @post On success the trailing LINK descriptor of each chain wraps
 *       to slot 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_default_open_rings(ra8_eth_gwca_default_state_t* state)
{
  ra8_err_t err = ra8_eth_gwca_init_ring(state->rx_chain, state->rx_depth, state->rx_slot_bytes);
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx init_ring");
  err = ra8_eth_gwca_attach_buffers(state->rx_chain,
                                    state->rx_depth,
                                    state->rx_slot_bytes,
                                    state->rx_pool);
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx attach");
  return internal_tx_ext_init(state->tx_chain,
                              state->tx_depth,
                              state->tx_slot_bytes,
                              state->tx_pool);
}

/**
 * @brief Program the RX + TX per-queue cfgs for the default-state API.
 *
 * @details Helper called by ra8_eth_gwca_default_open after the rings
 * are primed and GWMC.OPC is in CONFIG. Builds the two
 * ra8_eth_gwca_queue_cfg_t structs and calls configure_queue twice.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra8_err_t Error code propagated from configure_queue.
 * @retval k_ra8_ok              Both queues programmed.
 * @retval k_ra8_err_invalid_arg queue_index out of range or chain_head null.
 * @retval k_ra8_err_null_ptr    state field is null.
 *
 * @pre GWMC.OPC == CONFIG.
 * @pre rx_queue_index != tx_queue_index, both < linkfix_count.
 * @post On success both GWDCC[i] cfgs are live.
 * @post On success matching LINKFIX entries point at chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_default_open_queues(ra8_eth_gwca_default_state_t* state)
{
  const ra8_eth_gwca_queue_cfg_t rx_cfg = {.priority     = 0U,
                                           .is_tx        = false,
                                           .stop_on_last = false,
                                           .chain_head   = state->rx_chain};
  ra8_err_t                      err =
    ra8_eth_gwca_configure_queue(state->linkfix_table, state->rx_queue_index, &rx_cfg);
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx config");
  const ra8_eth_gwca_queue_cfg_t tx_cfg = {.priority     = 0U,
                                           .is_tx        = true,
                                           .stop_on_last = false,
                                           .extended     = true,
                                           .chain_head   = state->tx_chain};
  return ra8_eth_gwca_configure_queue(state->linkfix_table, state->tx_queue_index, &tx_cfg);
}

/**
 * @brief Walk the bring-up sub-sequence (init/rings/bring_up/-> CONFIG).
 *
 * @details Helper for ra8_eth_gwca_default_open. Splits the front
 * half of the bring-up so the top-level wrapper stays under the
 * 40-statement budget.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra8_err_t Error code propagated from sub-calls.
 * @retval k_ra8_ok              Hardware in CONFIG mode with rings primed.
 * @retval k_ra8_err_invalid_arg state field invalid.
 * @retval k_ra8_err_hw_timeout  GWMC.OPC transition timed out.
 *
 * @pre state pointer non-null.
 * @pre Power gates and clocks already on (CGC + MSTP for ESWM/GWCA).
 * @post On success GWMC.OPC == CONFIG.
 * @post On success rings have descriptors with PTRs in their pools.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_default_open_pre(ra8_eth_gwca_default_state_t* state)
{
  g_ra8_eth_gwca_pre_step = 0U;
  ra8_err_t err           = ra8_eth_gwca_init();
  if (err != k_ra8_ok) {
    g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_fail_1;
    return err;
  }
  g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_ok_1;
  err                     = internal_default_open_rings(state);
  if (err != k_ra8_ok) {
    g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_fail_2;
    return err;
  }
  g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_ok_2;
  err                     = ra8_eth_gwca_bring_up(state->linkfix_table, state->linkfix_count);
  if (err != k_ra8_ok) {
    g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_fail_3;
    return err;
  }
  g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_ok_3;
  const ra8_err_t cfg_err = ra8_eth_gwca_set_operation_mode(k_ra8_gwmc_opc_config);
  if (cfg_err != k_ra8_ok) {
    g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_fail_4;
    return cfg_err;
  }
  g_ra8_eth_gwca_pre_step = (uint32_t)k_ra8_eth_gwca_step_ok_4;
  return k_ra8_ok;
}

/**
 * @brief One-call GWCA bring-up for the default-state API.
 *
 * @details See header. Brings up RX + TX chains + LINKFIX, walks
 * the GWCA state machine to OPERATION.
 *
 * @param[in,out] state Pre-populated state block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              GWCA live; queues walkable.
 * @retval k_ra8_err_invalid_arg state pointer or fields invalid.
 * @retval k_ra8_err_hw_timeout  Mode transition never converged.
 *
 * @pre state's chain / pool / table pointers are 16-byte aligned.
 * @pre rx_queue_index != tx_queue_index, both < linkfix_count.
 * @post On success GWMC.OPC = OPERATION; both queues live.
 * @post state's rx_head / tx_tail cursors reset to 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_eth_gwca_default_open(ra8_eth_gwca_default_state_t* state)
{
  RA8_CHECK_NULL_PTR(state, s_tag, "default_open: state null");
  g_ra8_eth_gwca_open_step = 0U;
  ra8_err_t err            = internal_default_open_pre(state);
  if (err != k_ra8_ok) {
    g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_fail_1;
    return err;
  }
  g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_ok_1;
  err                      = internal_default_open_queues(state);
  if (err != k_ra8_ok) {
    g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_fail_2;
    return err;
  }
  g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_ok_2;
  state->rx_head           = 0U;
  state->tx_tail           = 0U;
  const ra8_err_t op_err   = ra8_eth_gwca_set_operation_mode(k_ra8_gwmc_opc_operation);
  if (op_err != k_ra8_ok) {
    g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_fail_3;
    return op_err;
  }
  g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_ok_3;

  /* Arm both queues now the GWCA is in OPERATION: BALR loads the
   * chain base into the AXI address RAM so the GWCA starts scanning
   * the descriptor chains (HUM Ch 34.3 "GWDCCi"). */
  const ra8_err_t rx_reload = ra8_eth_gwca_reload_queue(state->rx_queue_index);
  if (rx_reload != k_ra8_ok) {
    g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_fail_3;
    return rx_reload;
  }
  const ra8_err_t tx_reload = ra8_eth_gwca_reload_queue(state->tx_queue_index);
  if (tx_reload != k_ra8_ok) {
    g_ra8_eth_gwca_open_step = (uint32_t)k_ra8_eth_gwca_step_fail_3;
    return tx_reload;
  }
  return k_ra8_ok;
}


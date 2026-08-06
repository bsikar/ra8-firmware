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
 * cap: default_open (with its bring-up / ring / queue sub-helpers),
 * default_send (extended-descriptor TX), default_recv, and rx_frame
 * (plus the RX-drain and queue-rearm helpers). Every register access
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

static const char* s_tag = "ETHGWC";

/**
 * @brief Reconstruct the 12-bit DS (descriptor size) field from a basic descriptor.
 *
 * @details Reverses the ds_l / ds_h split applied when the chain
 * is populated. Used by the RX path to know how many bytes the
 * chip wrote into a buffer before we copy them out.
 *
 * @param[in] desc Descriptor whose ds field to read.
 * @return 12-bit size value.
 * @retval value Reconstructed DS as a uint32_t.
 * @pre desc is non-null and previously initialised.
 * @pre Caller has already validated desc against null.
 * @post Returned value is in [0, 4095].
 * @post No state is modified.
 * @note Pure helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_decode_ds(const ra8_gwca_basic_descriptor_t* desc)
{
  enum : uint32_t {
    k_ra8_ds_low_mask   = 0xFFU, /**< RA8 ds low mask.   */
    k_ra8_ds_high_shift = 8U,    /**< RA8 ds high shift. */
    k_ra8_ds_high_mask  = 0xFU,  /**< RA8 ds high mask.  */
  };
  return ((uint32_t)desc->ds_l & k_ra8_ds_low_mask) |
         (((uint32_t)desc->ds_h & k_ra8_ds_high_mask) << k_ra8_ds_high_shift);
}

/**
 * @brief Copy out a filled RX slot and reset it to FEMPTY.
 *
 * @details Pure helper invoked by ::ra8_eth_gwca_rx_frame after the
 * caller has located an FSINGLE slot. Splits the post-validation
 * memcpy + state reset path out so the top-level function fits
 * under the 60-line cap.
 *
 * When the GWCA writes back an FSINGLE descriptor it overwrites the
 * DS field with the *received* frame length. Re-arming the slot
 * therefore has to restore DS to the buffer capacity (@p slot_bytes)
 * -- otherwise the next frame larger than the last one no longer
 * fits in "one descriptor's area" and the GWCA fragments it into
 * FSTART/FEND, which ::ra8_eth_gwca_rx_frame would then never drain.
 *
 * @param[in,out] desc         The FSINGLE slot to drain.
 * @param[out]    out_frame    Destination buffer.
 * @param[in]     out_capacity Size of ``out_frame``.
 * @param[in]     slot_bytes   Buffer capacity to restore into DS.
 * @param[out]    out_len      Bytes actually written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Slot copied out + reset to FEMPTY.
 * @retval k_ra8_err_invalid_arg buf null or frame > capacity.
 *
 * @pre desc is non-null and currently FSINGLE.
 * @pre out_frame / out_len non-null and out_capacity > 0.
 * @post Slot dt = FEMPTY and DS = slot_bytes on success.
 * @post out_len contains the frame size on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_drain_rx_slot(ra8_gwca_basic_descriptor_t* desc,
                                        uint8_t*                     out_frame,
                                        uint32_t                     out_capacity,
                                        uint32_t                     slot_bytes,
                                        uint32_t*                    out_len)
{
  const uint8_t* const buf      = ra8_eth_gwca_decode_ptr(desc);
  const uint32_t       frame_ds = internal_decode_ds(desc);
  if (buf == nullptr || frame_ds > out_capacity) {
    return k_ra8_err_invalid_arg;
  }
  (void)memcpy(out_frame, buf, (size_t)frame_ds);
  *out_len = frame_ds;
  enum : uint32_t {
    k_ra8_ds_byte_mask  = 0xFFU, /**< ds_l carries 8 bits.         */
    k_ra8_ds_high_shift = 8U,    /**< ds_h packs the upper 4 bits. */
    k_ra8_ds_high_mask  = 0xFU,  /**< ds_h field width 4 bits.     */
  };
  desc->ds_l = (uint8_t)(slot_bytes & k_ra8_ds_byte_mask);
  desc->ds_h = (uint8_t)((slot_bytes >> k_ra8_ds_high_shift) & k_ra8_ds_high_mask);
  desc->dt   = (uint8_t)k_ra8_gwdcc_dt_fempty;
  return k_ra8_ok;
}

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
 * @brief Re-arm the extended TX queue if the GWCA has disabled it.
 *
 * @details The 16-byte-descriptor analogue of
 * ::internal_rearm_queue_if_disabled. When the TX queue runs dry the
 * GWCA rewrites the chain's LINK terminator to LEMPTY and stops
 * scanning; this restores the terminator to LINK (PTR -> chain[0])
 * and re-pulses GWDCC[i].BALR so the GWCA resumes. A no-op while the
 * queue is still live. No software cursor to reset -- the extended
 * TX path always enqueues into slot 0.
 *
 * @param[in,out] chain       Extended TX descriptor chain.
 * @param[in]     depth       Ring depth (data slots + LINK terminator).
 * @param[in]     queue_index GWCA queue number for the BALR reload.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain[depth - 1] is the ring's LINK/LEMPTY terminator.
 * @post If the queue was disabled it is re-armed and scanning again.
 * @post If the queue was already live nothing is changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_tx_ext_rearm(ra8_gwca_ext_descriptor_t* chain, uint32_t depth, uint32_t queue_index)
{
  ra8_gwca_ext_descriptor_t* const term = &chain[depth - 1U];
  if (term->dt != (uint8_t)k_ra8_gwdcc_dt_lempty) {
    return;
  }
  enum : uintptr_t {
    k_ptr_hi_shift = 32U,     /**< Pointer hi shift. */
    k_ptr_hi_mask  = 0xFFULL, /**< Pointer hi mask.  */
  };
  const uintptr_t head = (uintptr_t)&chain[0];
  term->ptr_h          = (uint8_t)(((uint64_t)head >> k_ptr_hi_shift) & (uint64_t)k_ptr_hi_mask);
  term->ptr_l          = (uint32_t)head;
  term->dt             = (uint8_t)k_ra8_gwdcc_dt_link;
  (void)ra8_eth_gwca_reload_queue(queue_index);
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
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx init_ring"); /* GCOVR_EXCL_BR_LINE */
  err = ra8_eth_gwca_attach_buffers(state->rx_chain,
                                    state->rx_depth,
                                    state->rx_slot_bytes,
                                    state->rx_pool);
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx attach"); /* GCOVR_EXCL_BR_LINE */
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
  RA8_RETURN_ON_ERROR(err, s_tag, "default_open: rx config"); /* GCOVR_EXCL_BR_LINE */
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

/**
 * @brief Re-arm a descriptor queue if the GWCA has disabled it.
 *
 * @details When a descriptor ring runs dry the GWCA disables the
 * queue by rewriting the ring's LINK terminator to LEMPTY. Once
 * disabled the GWCA never resumes scanning, so the queue stays dead
 * (RX stops delivering / TX stops sending) even after the
 * application services every data descriptor. This helper detects
 * that state (terminator dt == LEMPTY), restores the terminator to
 * LINK pointing at chain[0], and re-pulses GWDCC[i].BALR so the GWCA
 * reloads the chain base and resumes. It is a no-op while the queue
 * is still live.
 *
 * Critically, the BALR reload resets the GWCA's AXI address-RAM
 * current_address for the queue back to the chain base (chain[0]) --
 * see HUM Ch 34.3 "GWDCCi". The caller's software ring cursor must be
 * snapped back to 0 in lockstep, or the app fills / drains a slot the
 * GWCA is no longer looking at and the frame is silently lost. FSP
 * r_layer3_switch.c::R_LAYER3_SWITCH_StartDescriptorQueue does the
 * same: it pulses BALR and resets head/tail to 0 together. ``cursor``
 * is that software ring index (tx_tail for a TX queue, rx_head for
 * RX); it is zeroed only when an actual re-arm happens.
 *
 * @param[in,out] chain       Descriptor ring (RX or TX).
 * @param[in]     ring_depth  Ring depth (data slots + LINK terminator).
 * @param[in]     queue_index GWCA queue number for the BALR reload.
 * @param[in,out] cursor      Software ring cursor; zeroed on re-arm.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain[ring_depth - 1] is the ring's LINK/LEMPTY terminator.
 * @post If the queue was disabled it is re-armed, scanning again, and
 *       ``*cursor`` is 0 (re-synced with the GWCA scan position).
 * @post If the queue was already live nothing is changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_rearm_queue_if_disabled(ra8_gwca_basic_descriptor_t* chain,
                                             uint32_t                     ring_depth,
                                             uint32_t                     queue_index,
                                             uint32_t*                    cursor)
{
  ra8_gwca_basic_descriptor_t* const term = &chain[ring_depth - 1U];
  if (term->dt != (uint8_t)k_ra8_gwdcc_dt_lempty) {
    return;
  }
  /* Restore the LINK terminator (PTR -> chain[0], dt = LINK), reload
   * the queue so the GWCA resumes from the chain base, and snap the
   * software cursor to 0 so it tracks the GWCA's reset scan position. */
  ra8_eth_gwca_set_linkfix_entry(term, &chain[0]);
  term->dt = (uint8_t)k_ra8_gwdcc_dt_link;
  *cursor  = 0U;
  (void)ra8_eth_gwca_reload_queue(queue_index);
}

/**
 * @brief Compose the INFO1_hi word of a TX extended descriptor.
 *
 * @details Pure helper: places the destination vector (a one-hot
 * port bit, ``1 << mac_port``) into the DV[6:0] field of INFO1.
 * No MMIO, no global state.
 *
 * @param[in] mac_port Destination MAC port index (0..6).
 * @return Packed INFO1_hi word with DV set.
 * @retval value INFO1_hi word.
 * @pre mac_port <= 6 so the one-hot bit stays inside DV[6:0].
 * @pre Caller writes the result to ext-descriptor info1_hi.
 * @post Only the DV field is non-zero.
 * @post No global state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_tx_info1_hi(uint8_t mac_port)
{
  const uint32_t dv = (uint32_t)1U << (uint32_t)mac_port;
  return ((dv << (uint32_t)k_ra8_gwca_info1_tx_dv_shift) & (uint32_t)k_ra8_gwca_info1_tx_dv_mask);
}

/**
 * @brief Block until the GWCA writes TX slot 0 back (FSINGLE -> FEMPTY).
 *
 * @details The single-slot TX path always reuses ``tx_chain[0]``, so a send must
 * observe the GWCA clear ``dt`` from FSINGLE before the next send can overwrite
 * the buffer. This is a bounded spin. The host unit-test build runs the same
 * loop but routes the completion test through the ra8_fake_mmio seam -- keyed on
 * the descriptor base, since ``dt`` is a bitfield with no address of its own --
 * so a test can drive it to completion or to timeout (T1-01); firmware and
 * ra8_emulator take the plain read. Extracted from ::ra8_eth_gwca_default_send to
 * keep that function under the complexity cap.
 *
 * @param[in,out] state Post-default_open state; ``tx_chain[0]`` is in flight.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Slot 0 written back (``dt`` left FSINGLE).
 * @retval k_ra8_err_hw_timeout Spin budget exhausted with slot 0 still FSINGLE.
 *
 * @pre state is non-NULL and post-default_open.
 * @pre A TX descriptor has been kicked into slot 0.
 * @post On success slot 0 is no longer FSINGLE.
 * @post On timeout the error has been logged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_tx0_done(ra8_eth_gwca_default_state_t* state)
{
  for (uint32_t i = 0U; i < k_ra8_eth_gwca_tx_done_spin; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(&state->tx_chain[0],
                                i,
                                (state->tx_chain[0].dt != (uint8_t)k_ra8_gwdcc_dt_fsingle))) {
      return k_ra8_ok;
    }
#else
    if (state->tx_chain[0].dt != (uint8_t)k_ra8_gwdcc_dt_fsingle) {
      return k_ra8_ok;
    }
#endif
  }
  ra8_log_error(s_tag, "default_send: TX completion timeout");
  return k_ra8_err_hw_timeout;
}

/**
 * @brief One-call TX: enqueue an extended descriptor into slot 0 + kick.
 *
 * @details The TX queue uses 16-byte EXTENDED descriptors (EDE = 1):
 * each frame carries its own INFO1 routing metadata, so the GWCA
 * sends it without a forwarding-engine lookup. The frame always goes
 * into slot 0 (deterministic):
 *  1. Re-arm the queue if the GWCA idle-disabled it.
 *  2. memcpy the frame into slot 0's buffer; set DS = len.
 *  3. INFO1: FMT = direct descriptor, DV = 1 << mac_port (the frame's
 *     one destination port). FI stays 0 so the RMAC appends the FCS.
 *  4. dt = FSINGLE, then DSB so the SRAM writes land before the kick.
 *  5. BALR-reload (GWCA scan pointer -> chain[0]) + GWTRC kick.
 *  6. Block until the GWCA writes slot 0 back (transmit complete) so
 *     a back-to-back send cannot overwrite the in-flight buffer.
 *
 * @param[in,out] state Initialized by default_open.
 * @param[in]     frame Frame bytes.
 * @param[in]     len   Frame length (1 .. tx_slot_bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Frame transmitted (slot 0 written back).
 * @retval k_ra8_err_invalid_arg len 0 or > tx_slot_bytes.
 * @retval k_ra8_err_hw_timeout  BALR never self-cleared, or the GWCA
 *                              never wrote slot 0 back.
 * @retval k_ra8_err_null_ptr    state or frame null.
 *
 * @pre default_open returned ok.
 * @pre state remains in its post-default_open layout.
 * @post On success the frame has been fully transmitted and slot 0 is
 *       no longer FSINGLE.
 * @post On success the chip has been signaled.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t
ra8_eth_gwca_default_send(ra8_eth_gwca_default_state_t* state, const uint8_t* frame, uint32_t len)
{
  RA8_CHECK_NULL_PTR(state, s_tag, "default_send: state null");
  RA8_CHECK_NULL_PTR(frame, s_tag, "default_send: frame null");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (len > state->tx_slot_bytes) {
    return k_ra8_err_invalid_arg;
  }
  /* Re-arm if the GWCA idle-disabled the queue, then fill slot 0. */
  internal_tx_ext_rearm(state->tx_chain, state->tx_depth, state->tx_queue_index);
  enum : uint32_t {
    k_ds_low_mask   = 0xFFU, /**< ds_l carries 8 bits.         */
    k_ds_high_shift = 8U,    /**< ds_h packs the upper 4 bits. */
    k_ds_high_mask  = 0xFU,  /**< ds_h field width.            */
  };
  ra8_gwca_ext_descriptor_t* const d = &state->tx_chain[0];
  (void)memcpy(state->tx_pool, frame, (size_t)len);
  d->ds_l        = (uint8_t)(len & k_ds_low_mask);
  d->ds_h        = (uint8_t)((len >> k_ds_high_shift) & k_ds_high_mask);
  d->info1_lo    = (uint32_t)k_ra8_gwca_info1_tx_fmt_direct;
  d->info1_hi    = internal_tx_info1_hi(state->mac_port);
  d->dt          = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  state->tx_tail = 0U;
  /* DSB: the descriptor + frame buffer are Normal (SRAM) writes; the
   * GWCA kick below is a Device write. Armv8-M does not order them
   * without an explicit barrier (host no-op via the ra8_hw_intrinsics seam). */
  ra8_hw_dsb();
  const ra8_err_t reload_err = ra8_eth_gwca_reload_queue(state->tx_queue_index);
  if (reload_err != k_ra8_ok) {
    return reload_err;
  }
  const ra8_err_t kick_err = ra8_eth_gwca_kick_tx(state->tx_queue_index);
  if (kick_err != k_ra8_ok) {
    return kick_err;
  }
  /* Block until the GWCA writes slot 0 back (FSINGLE -> FEMPTY) or the spin
   * budget is exhausted; extracted so this send stays under the complexity cap. */
  return internal_wait_tx0_done(state);
}

/**
 * @brief One-call RX: dequeue next frame.
 *
 * @details See header. Wraps rx_frame using state->rx_chain/head.
 * When no frame is waiting it also self-heals a GWCA-disabled RX
 * queue via ::internal_rearm_rx_if_disabled.
 *
 * @param[in,out] state        Initialized by default_open.
 * @param[out]    out_frame    Destination buffer.
 * @param[in]     out_capacity Size of out_frame.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra8_err_t Error code propagated from rx_frame.
 * @retval k_ra8_ok              Frame copied; slot reset to FEMPTY.
 * @retval k_ra8_err_no_data     No inbound frame waiting.
 * @retval k_ra8_err_invalid_arg capacity 0 or frame too large.
 * @retval k_ra8_err_null_ptr    state, out_frame, or out_len null.
 *
 * @pre default_open returned ok.
 * @pre state remains in its post-default_open layout.
 * @post On success state->rx_head advanced.
 * @post On success *out_len reflects the received frame size.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_eth_gwca_default_recv(ra8_eth_gwca_default_state_t* state,
                                    uint8_t*                      out_frame,
                                    uint32_t                      out_capacity,
                                    uint32_t*                     out_len)
{
  RA8_CHECK_NULL_PTR(state, s_tag, "default_recv: state null");
  const ra8_err_t err = ra8_eth_gwca_rx_frame(state->rx_chain,
                                              state->rx_depth,
                                              &state->rx_head,
                                              out_frame,
                                              out_capacity,
                                              state->rx_slot_bytes,
                                              out_len);
  if (err == k_ra8_err_no_data) {
    internal_rearm_queue_if_disabled(state->rx_chain,
                                     state->rx_depth,
                                     state->rx_queue_index,
                                     &state->rx_head);
  }
  return err;
}

/**
 * @brief Dequeue one frame from an RX queue's descriptor ring.
 *
 * @details See header for the canonical contract. Locates the next
 * FSINGLE slot, delegates the buffer copy + slot reset to
 * ::internal_drain_rx_slot, and advances the head cursor.
 *
 * @param[in,out] chain        RX descriptor ring.
 * @param[in]     ring_depth   Ring depth.
 * @param[in,out] head_idx     Round-robin read cursor.
 * @param[out]    out_frame    Destination buffer for the frame.
 * @param[in]     out_capacity Size of out_frame.
 * @param[in]     slot_bytes   Per-slot buffer capacity restored into DS.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Frame copied; slot reset to FEMPTY.
 * @retval k_ra8_err_no_data     No FSINGLE slot yet.
 * @retval k_ra8_err_invalid_arg out_capacity == 0 or frame > capacity.
 * @retval k_ra8_err_null_ptr    chain / head_idx / out_frame / out_len null.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre RX queue's MFWD forwarding cfg has been programmed.
 * @post On success the chosen slot is FEMPTY with DS = slot_bytes.
 * @post On success ``*head_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_eth_gwca_rx_frame(ra8_gwca_basic_descriptor_t* chain,
                                uint32_t                     ring_depth,
                                uint32_t*                    head_idx,
                                uint8_t*                     out_frame,
                                uint32_t                     out_capacity,
                                uint32_t                     slot_bytes,
                                uint32_t*                    out_len)
{
  RA8_CHECK_NULL_PTR(chain, s_tag, "rx_frame: chain null");
  RA8_CHECK_NULL_PTR(head_idx, s_tag, "rx_frame: head_idx null");
  RA8_CHECK_NULL_PTR(out_frame, s_tag, "rx_frame: out_frame null");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "rx_frame: out_len null");
  if (out_capacity == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t        slot = 0U;
  const ra8_err_t err  = ra8_eth_gwca_find_slot(chain,
                                                ring_depth,
                                                k_ra8_gwdcc_dt_fsingle,
                                                *head_idx % (ring_depth - 1U),
                                                &slot);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_err_t drain_err =
    internal_drain_rx_slot(&chain[slot], out_frame, out_capacity, slot_bytes, out_len);
  if (drain_err != k_ra8_ok) {
    return drain_err;
  }
  *head_idx = (slot + 1U) % (ring_depth - 1U);
  return k_ra8_ok;
}

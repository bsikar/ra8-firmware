/**
 * @file ra_eth_gwca_queue.c
 * @brief Ethernet CPU Agent driver -- per-queue descriptor + ring primitives
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Per-queue and per-descriptor surface of the RA8D2 GWCA block, split
 * out of ra_eth_gwca.c to stay under the per-file line-count cap:
 * configure_queue, reload_queue, init_ring, set_descriptor_buffer,
 * attach_buffers, kick_tx, find_slot, and tx_frame, plus the
 * address-encoding helpers shared with the default-state TU. Every
 * register access carries a HUM Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_ether_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_eth_gwca.h"
#include "ra_eth_gwca_internal.h"
#include "ra_log.h"

static const char* s_tag = "ETHGWC";

/**
 * @brief Compose the GWDCC[i] 32-bit value from a queue config.
 *
 * @details Pure value composition: DQT + DCP[18:16] + SL.
 *
 * GWDCC.SM[1:0] (Synchronization Mode) is left at 00b -- "Normal mode
 * (full descriptor write back)" per HUM Ch 34.3 "GWDCCi" p 9060. SM
 * is NOT a source-MAC field: 01b is "No-write-back mode (no
 * descriptor write back)", which would leave every RX descriptor
 * stuck at FEMPTY because the GWCA never updates it. Bench-confirmed
 * on EK-RA8D2. EDE/ETS stay 0 (basic descriptors), BALR stays 0
 * (default AXI burst), OSID stays 0 (default stream). No MMIO
 * touched.
 *
 * @param[in] cfg Per-queue config.
 * @return Packed GWDCC value with DQT, SL, DCP bits set per cfg.
 * @retval value Packed register word.
 * @pre cfg is non-null with priority <= 7.
 * @pre Caller validated the bounds before invoking.
 * @post Returned value reflects the cfg fields; SM stays 00b.
 * @post No global state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_compose_gwdcc(const ra_eth_gwca_queue_cfg_t* cfg)
{
  /* GWDCC value: DQT + DCP[18:16] + SL + EDE. SM[1:0] stays 00b
   * (Normal mode -- full descriptor write-back). ETS stays 0, BALR
   * stays 0 (default AXI burst), OSID 0. EDE = 1 when the queue uses
   * 16-byte extended descriptors (the TX path). */
  uint32_t value = 0U;
  if (cfg->is_tx) {
    value |= (uint32_t)k_ra_gwdcc_dqt;
  }
  if (cfg->stop_on_last) {
    value |= (uint32_t)k_ra_gwdcc_sl;
  }
  if (cfg->extended) {
    value |= (uint32_t)k_ra_gwdcc_ede;
  }
  value |= (((uint32_t)cfg->priority << k_ra_gwdcc_dcp_shift) & k_ra_gwdcc_dcp_mask);
  return value;
}

void ra_eth_gwca_set_linkfix_entry(ra_gwca_basic_descriptor_t* entry, const void* chain_head)
{
  enum : uintptr_t {
    k_ra_linkfix_ptr_upper_shift = 32U,
    k_ra_linkfix_ptr_upper_mask  = 0xFFULL,
    k_ra_linkfix_ptr_lower_mask  = 0xFFFFFFFFULL,
  };
  const uintptr_t head_addr = (uintptr_t)chain_head;
  entry->dt                 = (uint8_t)k_ra_gwdcc_dt_linkfix;
  entry->ptr_h =
    (uint8_t)((uint64_t)head_addr >> k_ra_linkfix_ptr_upper_shift) & k_ra_linkfix_ptr_upper_mask;
  entry->ptr_l = (uint32_t)head_addr & k_ra_linkfix_ptr_lower_mask;
}

/**
 * @brief Wire a per-queue config into GWDCC[i] + LINKFIX[i].
 *
 * @details See header for the canonical contract. Composes the
 * GWDCC value from cfg's DQT / DCP / SL bits via
 * internal_compose_gwdcc, writes it to GWDCC[queue_index], then
 * promotes the LINKFIX entry to LINKFIX via
 * ra_eth_gwca_set_linkfix_entry.
 *
 * @param[in,out] linkfix_table Same table passed to install_linkfix.
 * @param[in]     queue_index    Queue 0..31.
 * @param[in]     cfg            Per-queue config (priority, dir, head).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCC[i] + LINKFIX[i] wired.
 * @retval k_ra_err_invalid_arg Null pointer or queue out of range.
 * @retval k_ra_err_null_ptr    linkfix_table, cfg, or cfg->chain_head is null.
 *
 * @pre ::ra_eth_gwca_install_linkfix returned ok.
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @post GWDCC[queue_index] reflects cfg's settings.
 * @post linkfix_table[queue_index] has dt = LINKFIX with PTR = chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_configure_queue(ra_gwca_basic_descriptor_t*    linkfix_table,
                                     uint32_t                       queue_index,
                                     const ra_eth_gwca_queue_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(linkfix_table, s_tag, "configure_queue: table null");
  RA_CHECK_NULL_PTR(cfg, s_tag, "configure_queue: cfg null");
  RA_CHECK_NULL_PTR(cfg->chain_head, s_tag, "configure_queue: chain_head null");
  enum : uint8_t {
    k_ra_gwdcc_dcp_max = 7U, /**< DCP field width 3 bits -> max value 7. */
  };
  if (cfg->priority > k_ra_gwdcc_dcp_max) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* const gwdcc = ra_gwca_gwdcc(queue_index);
  if (gwdcc == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *gwdcc = internal_compose_gwdcc(cfg);
  ra_eth_gwca_set_linkfix_entry(&linkfix_table[queue_index], cfg->chain_head);
  return k_ra_ok;
}

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok             BALR pulsed and self-cleared.
 * @retval k_ra_err_invalid_arg queue_index has no GWDCC register.
 * @retval k_ra_err_hw_timeout  BALR never self-cleared.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre ::ra_eth_gwca_configure_queue ran for queue_index.
 * @post The AXI address RAM current_address for queue_index points at
 *       the chain base.
 * @post GWDCC[queue_index].BALR reads 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_reload_queue(uint32_t queue_index)
{
  volatile uint32_t* const gwdcc = ra_gwca_gwdcc(queue_index);
  if (gwdcc == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 34.3 "GWDCCi" p 1811: BALR self-clears once the GWCA has
   * reset the AXI address RAM current_address pointer. */
  *gwdcc |= (uint32_t)k_ra_gwdcc_balr;
#ifndef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_eth_gwca_balr_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*gwdcc & (uint32_t)k_ra_gwdcc_balr) == 0U) {       /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "reload_queue: GWDCC BALR never cleared");
  return k_ra_err_hw_timeout;
#else
  return k_ra_ok;
#endif
}

/**
 * @brief Initialise a descriptor chain as a ring of FEMPTY slots.
 *
 * @details See header for the canonical contract. Walks chain[],
 * setting every entry except the last to FEMPTY with ds = slot_bytes,
 * and the last entry to LINK with PTR pointing back at chain[0].
 *
 * @param[in,out] chain      Caller-owned descriptor array.
 * @param[in]     ring_depth Number of entries (>= 2).
 * @param[in]     slot_bytes Per-slot buffer size in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Ring initialised.
 * @retval k_ra_err_null_ptr    chain is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes out of range.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre chain is 8-byte aligned.
 * @post chain[0..ring_depth-2] have dt = FEMPTY, ds = slot_bytes.
 * @post chain[ring_depth-1] has dt = LINK with PTR = &chain[0].
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t
ra_eth_gwca_init_ring(ra_gwca_basic_descriptor_t* chain, uint32_t ring_depth, uint32_t slot_bytes)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "init_ring: chain null");
  enum : uint32_t {
    k_ra_gwca_ring_min_depth = 2U,    /**< Need at least one FEMPTY + one LINK. */
    k_ra_gwca_ring_max_bytes = 2048U, /**< HUM DS field is 12 bits (max 2048).  */
  };
  if (ring_depth < k_ra_gwca_ring_min_depth || slot_bytes > k_ra_gwca_ring_max_bytes) {
    return k_ra_err_invalid_arg;
  }

  /* FEMPTY data slots: ds carries the buffer size, dt = FEMPTY. */
  enum : uint32_t {
    k_ra_ds_byte_mask  = 0xFFU, /**< ds_l carries 8 bits.         */
    k_ra_ds_high_shift = 8U,    /**< ds_h packs the upper 4 bits. */
    k_ra_ds_high_mask  = 0xFU,  /**< ds_h field width 4 bits.     */
  };
  for (uint32_t i = 0U; i < (ring_depth - 1U); ++i) {
    (void)memset(&chain[i], 0, sizeof(ra_gwca_basic_descriptor_t));
    chain[i].dt   = (uint8_t)k_ra_gwdcc_dt_fempty;
    chain[i].ds_l = (uint8_t)(slot_bytes & k_ra_ds_byte_mask);
    chain[i].ds_h = (uint8_t)((slot_bytes >> k_ra_ds_high_shift) & k_ra_ds_high_mask);
    /* ptr_l left at 0 -- caller fills in the per-slot buffer address. */
  }

  /* Last entry: LINK back to chain[0] so the chip wraps. */
  ra_eth_gwca_set_linkfix_entry(&chain[ring_depth - 1U], &chain[0]);
  /* Override dt: ra_eth_gwca_set_linkfix_entry writes LINKFIX, but for
   * mid-chain wrap we want LINK (interchangeable per HUM Ch
   * 34.5.1.3.2; LINK is the standard chain-continuation type). */
  chain[ring_depth - 1U].dt = (uint8_t)k_ra_gwdcc_dt_link;
  return k_ra_ok;
}

/**
 * @brief Set a descriptor's data-buffer pointer.
 *
 * @details Wraps ::ra_eth_gwca_set_linkfix_entry-style address split
 * for external callers wiring buffer pointers into FEMPTY slots.
 *
 * @param[in,out] desc   Descriptor to update.
 * @param[in]     buffer Buffer address to encode.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           PTR field updated.
 * @retval k_ra_err_null_ptr desc is null.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre desc was previously zeroed (e.g. by init_ring).
 * @post desc->ptr_h / ptr_l encode @p buffer.
 * @post desc->dt / ds / etc unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_set_descriptor_buffer(ra_gwca_basic_descriptor_t* desc, void* buffer)
{
  RA_CHECK_NULL_PTR(desc, s_tag, "set_descriptor_buffer: desc null");
  enum : uintptr_t {
    k_ra_buf_ptr_upper_shift = 32U,
    k_ra_buf_ptr_upper_mask  = 0xFFULL,
    k_ra_buf_ptr_lower_mask  = 0xFFFFFFFFULL,
  };
  const uintptr_t addr = (uintptr_t)buffer;
  desc->ptr_h = (uint8_t)((uint64_t)addr >> k_ra_buf_ptr_upper_shift) & k_ra_buf_ptr_upper_mask;
  desc->ptr_l = (uint32_t)addr & k_ra_buf_ptr_lower_mask;
  return k_ra_ok;
}

/**
 * @brief Walk a ring and attach per-slot buffers from a static pool.
 *
 * @details See header for the canonical contract. Iterates the
 * FEMPTY slots (chain[0..ring_depth-2]) and sets each PTR to
 * pool + i * slot_bytes via ::ra_eth_gwca_set_descriptor_buffer.
 *
 * @param[in,out] chain      Ring from init_ring.
 * @param[in]     ring_depth Same depth as init_ring.
 * @param[in]     slot_bytes Per-slot buffer size.
 * @param[in,out] pool       Contiguous buffer pool.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Every FEMPTY slot has its buffer wired.
 * @retval k_ra_err_null_ptr    chain or pool is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes == 0.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre Pool spans at least (ring_depth - 1) * slot_bytes bytes.
 * @post Each chain[i].ptr_h/ptr_l (i in [0, ring_depth-1)) encodes
 *       pool + i * slot_bytes.
 * @post Chain[ring_depth-1] (the LINK terminator) is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_attach_buffers(ra_gwca_basic_descriptor_t* chain,
                                    uint32_t                    ring_depth,
                                    uint32_t                    slot_bytes,
                                    uint8_t*                    pool)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "attach_buffers: chain null");
  RA_CHECK_NULL_PTR(pool, s_tag, "attach_buffers: pool null");
  if (ring_depth < 2U || slot_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < (ring_depth - 1U); ++i) {
    const size_t   slot_offset = (size_t)i * (size_t)slot_bytes;
    const ra_err_t err         = ra_eth_gwca_set_descriptor_buffer(&chain[i], &pool[slot_offset]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Kick a TX queue via GWTRC.
 *
 * @details See header. Sets the bit for queue_index in GWTRC0
 * (queues 0..31) or GWTRC1 (queues 32..63). Read-modify-write so
 * other already-pending TX requests on the same 32-queue word are
 * preserved.
 *
 * @param[in] queue_index TX queue 0..63.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWTRCi bit set.
 * @retval k_ra_err_invalid_arg queue_index >= 64.
 *
 * @pre Queue's chain has at least one FSINGLE descriptor ready.
 * @pre GWCA is in GWMC.OPC = OPERATION.
 * @post Matching GWTRC bit is 1.
 * @post Other already-pending TX requests on the same word are preserved.
 *
 * @note Not thread-safe across writes to the same GWTRC word.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_kick_tx(uint32_t queue_index)
{
  enum : uint32_t {
    k_ra_gwca_max_tx_queues  = 64U,
    k_ra_gwca_queues_per_reg = 32U,
  };
  if (queue_index >= k_ra_gwca_max_tx_queues) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t          offset = (queue_index < k_ra_gwca_queues_per_reg)
                                      ? (uintptr_t)k_ra_gwca_off_gwtrc0
                                      : (uintptr_t)k_ra_gwca_off_gwtrc1;
  volatile uint32_t* const gwtrc  = (volatile uint32_t*)(k_ra_gwca0_base_addr + offset);
  const uint32_t           bit    = 1UL << (queue_index % k_ra_gwca_queues_per_reg);
  *gwtrc                          = *gwtrc | bit;
  return k_ra_ok;
}

/**
 * @brief Find the next slot in a ring matching a target descriptor type.
 *
 * @details See header for the canonical contract. Walks
 * chain[start_idx .. ring_depth-2] looking for an entry where
 * dt == match_dt, wrapping around to chain[0..start_idx-1] if
 * needed. Skips chain[ring_depth-1] (the LINK terminator).
 *
 * @param[in]  chain      Ring from init_ring.
 * @param[in]  ring_depth Same depth as init_ring.
 * @param[in]  match_dt   Descriptor type to find (FEMPTY / FSINGLE).
 * @param[in]  start_idx  Slot to start scanning from.
 * @param[out] out_index  First matching slot.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              ``*out_index`` set.
 * @retval k_ra_err_no_data     No slot matched ``match_dt``.
 * @retval k_ra_err_invalid_arg null pointer / ring_depth < 2 /
 *                              start_idx out of range.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain has been initialised via init_ring + attach_buffers.
 * @post On success ``*out_index`` < ring_depth - 1.
 * @post On failure ``*out_index`` is unchanged.
 *
 * @note Not thread-safe with concurrent chip-side updates.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_find_slot(const ra_gwca_basic_descriptor_t* chain,
                               uint32_t                          ring_depth,
                               ra_gwdcc_dt_t                     match_dt,
                               uint32_t                          start_idx,
                               uint32_t*                         out_index)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "find_slot: chain null");
  RA_CHECK_NULL_PTR(out_index, s_tag, "find_slot: out_index null");
  if (ring_depth < 2U) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t data_slot_count = ring_depth - 1U;
  if (start_idx >= data_slot_count) {
    return k_ra_err_invalid_arg;
  }
  /* Walk from start_idx forward, wrapping once if needed. */
  for (uint32_t i = 0U; i < data_slot_count; ++i) {
    const uint32_t slot = (start_idx + i) % data_slot_count;
    if (chain[slot].dt == (uint8_t)match_dt) {
      *out_index = slot;
      return k_ra_ok;
    }
  }
  return k_ra_err_no_data;
}

uint8_t* ra_eth_gwca_decode_ptr(const ra_gwca_basic_descriptor_t* desc)
{
  if (desc == nullptr) {
    return nullptr;
  }
  /* PTR is 40 bits across ptr_h (high 8) + ptr_l (low 32). On a
   * 32-bit target uintptr_t is 32 bits, so shifting by 32 is UB --
   * promote to uint64_t for the recompose, then back to uintptr_t.
   * On 32-bit chips ptr_h is always zero so the upper byte is
   * harmlessly truncated when we cast back. */
  enum : uint64_t {
    k_ra_ptr_upper_shift = 32ULL,
    k_ra_ptr_low_mask    = 0xFFFFFFFFULL,
  };
  const uint64_t addr64 =
    ((uint64_t)desc->ptr_h << k_ra_ptr_upper_shift) | ((uint64_t)desc->ptr_l & k_ra_ptr_low_mask);
  return (uint8_t*)(uintptr_t)addr64;
}

/**
 * @brief Enqueue one frame on a TX queue's descriptor ring.
 *
 * @details See header for the canonical contract. Finds the next
 * FEMPTY slot, memcpy's the frame into the slot's pre-attached
 * buffer, sets ds to frame_len, flips dt to FSINGLE, and advances
 * the caller's tail_idx cursor.
 *
 * @param[in,out] chain      TX descriptor ring.
 * @param[in]     ring_depth Ring depth.
 * @param[in,out] tail_idx   Round-robin write cursor.
 * @param[in]     frame      Source bytes.
 * @param[in]     frame_len  Frame length.
 * @param[in]     slot_bytes Per-slot capacity.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame queued; FSINGLE marked.
 * @retval k_ra_err_no_data     All slots already FSINGLE (queue full).
 * @retval k_ra_err_invalid_arg Null pointer or frame_len > slot_bytes.
 * @retval k_ra_err_null_ptr    chain / tail_idx / frame is null.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain initialised via init_ring + attach_buffers.
 * @post On success the chosen slot is FSINGLE with the frame copied.
 * @post On success ``*tail_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_eth_gwca_tx_frame(ra_gwca_basic_descriptor_t* chain,
                              uint32_t                    ring_depth,
                              uint32_t*                   tail_idx,
                              const uint8_t*              frame,
                              uint32_t                    frame_len,
                              uint32_t                    slot_bytes)
{
  RA_CHECK_NULL_PTR(chain, s_tag, "tx_frame: chain null");
  RA_CHECK_NULL_PTR(tail_idx, s_tag, "tx_frame: tail_idx null");
  RA_CHECK_NULL_PTR(frame, s_tag, "tx_frame: frame null");
  if (frame_len == 0U || frame_len > slot_bytes) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       slot = 0U;
  const ra_err_t err  = ra_eth_gwca_find_slot(chain,
                                              ring_depth,
                                              k_ra_gwdcc_dt_fempty,
                                              *tail_idx % (ring_depth - 1U),
                                              &slot);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t* const buf = ra_eth_gwca_decode_ptr(&chain[slot]);
  if (buf == nullptr) {
    return k_ra_err_invalid_arg;
  }
  (void)memcpy(buf, frame, (size_t)frame_len);
  enum : uint32_t {
    k_ra_ds_low_byte_mask = 0xFFU,
    k_ra_ds_high_shift    = 8U,
    k_ra_ds_high_nibble   = 0xFU,
  };
  chain[slot].ds_l = (uint8_t)(frame_len & k_ra_ds_low_byte_mask);
  chain[slot].ds_h = (uint8_t)((frame_len >> k_ra_ds_high_shift) & k_ra_ds_high_nibble);
  chain[slot].dt   = (uint8_t)k_ra_gwdcc_dt_fsingle;
  *tail_idx        = (slot + 1U) % (ring_depth - 1U);
  return k_ra_ok;
}

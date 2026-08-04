/**
 * @file ra8_eth_gwca.h
 * @brief Ethernet CPU Agent (GWCA) driver
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. GWCA is the bridge
 * between MFWD/COMA and CPU memory; it owns the per-channel
 * descriptor rings the host uses for TX/RX staging. This driver
 * covers lifecycle + status + IRQ + power transition; the
 * descriptor-ring programming surface lands with the first
 * NIC consumer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ether_regs.h"

/**
 * @typedef ra8_eth_gwca_event_fn_t
 * @brief GWCA event callback.
 */
typedef void (*ra8_eth_gwca_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise GWCA. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_init(void);

/** @brief Tear down GWCA. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_deinit(void);

/** @brief Read GWCA_STS. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_get_status(uint32_t* out_mask);

/** @brief Clear GWCA_STS bits via GWCA_ICLR. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_attach_handler(ra8_eth_gwca_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a GWCA event. @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_eth_gwca_dispatch(void);

/** @brief Put GWCA into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_exit_stop(void);

/**
 * @brief Transition the GWCA / ESWM state machine to a new OPC mode.
 *
 * @details Writes GWMC.OPC[1:0] = @p mode and polls GWMS.OPS[1:0]
 * until it reflects the new mode (or the bounded poll budget elapses).
 * This is the canonical state-machine transition for GWCA, used by
 * the LINKFIX init sequence (RESET -> DISABLE -> CONFIG -> ... ->
 * OPERATION). Mirrors FSP `r_layer3_switch_update_gwca_operation_mode`.
 *
 * @param[in] mode Target operation mode from ::ra8_gwmc_opc_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             GWMS.OPS now reflects @p mode.
 * @retval k_ra8_err_invalid_arg @p mode is out of range.
 * @retval k_ra8_err_hw_timeout GWMS.OPS never converged.
 *
 * @pre ::ra8_eth_gwca_init has been called (MSTP-gate cleared).
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMC.OPC and GWMS.OPS both equal @p mode.
 * @post On timeout GWMC.OPC may have been written even if OPS
 *       never converged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_set_operation_mode(ra8_gwmc_opc_t mode);

/**
 * @brief Request the GWCA AXI bridge to initialize via GWARIRM.ARIOG.
 *
 * @details Asserts GWARIRM.ARIOG (bit 0) and polls GWARIRM.ARR
 * (bit 1) until it reads 1. Called once from the LINKFIX init flow
 * after entering CONFIG mode. Mirrors the FSP
 * `r_layer3_switch_initialize_gwca` AXI-init step.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              ARR asserted within the budget.
 * @retval k_ra8_err_hw_timeout ARR never asserted.
 *
 * @pre ::ra8_eth_gwca_set_operation_mode(k_ra8_gwmc_opc_config) returned ok.
 * @post On success ARR=1 indicates the AXI manager is ready.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_axi_init(void);

/**
 * @brief Install a fresh LINKFIX table at GWDCBAC0/1.
 *
 * @details Programs the chip with the address of a SW-side LINKFIX
 * table (an array of ::ra8_gwca_basic_descriptor_t indexed by queue
 * number, where each entry's PTR is the head of that queue's
 * descriptor chain). Every entry is initialised to LEMPTY
 * (descriptor type 12 = "queue disabled") so no queue accidentally
 * starts active before its chain is wired up.
 *
 * GWDCBAC0 carries the upper 8 bits of the 40-bit address
 * (PTR[39:32]); GWDCBAC1 carries the lower 32 bits (PTR[31:0]).
 * Caller must already be in CONFIG mode -- LINKFIX address bits
 * are RESET/CONFIG-only-writable per HUM Ch 34.5.1.3.1.
 *
 * @param[in,out] linkfix_table Caller-owned table; written to LEMPTY.
 * @param[in]     entry_count   Number of queues to cover (max 32).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              GWDCBAC0/1 programmed.
 * @retval k_ra8_err_invalid_arg ``linkfix_table`` is null or count > 32.
 *
 * @pre ::ra8_eth_gwca_set_operation_mode(k_ra8_gwmc_opc_config) returned ok.
 * @pre ::ra8_eth_gwca_axi_init returned ok.
 * @pre ``linkfix_table`` is 16-byte aligned (chip requirement).
 * @post Every LINKFIX entry has dt = k_ra8_gwdcc_dt_lempty.
 * @post GWDCBAC0/1 = address of ``linkfix_table``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_install_linkfix(ra8_gwca_basic_descriptor_t* linkfix_table,
                                                     uint32_t                     entry_count);

/**
 * @brief Bring the GWCA from RESET state up to OPERATION with a fresh LINKFIX table.
 *
 * @details Single-call wrapper that ties the three foundation
 * primitives together in the canonical FSP ordering:
 *
 *   1. set_operation_mode(DISABLE)  -- park the state machine
 *   2. set_operation_mode(CONFIG)   -- LINKFIX writable
 *   3. axi_init()                   -- AXI manager ready
 *   4. install_linkfix(table, n)    -- table address + LEMPTY init
 *   5. set_operation_mode(DISABLE)  -- transition back through DISABLE
 *   6. set_operation_mode(OPERATION) -- queues activate
 *
 * If any step fails, the GWCA is left in DISABLE so the chip is in
 * a predictable state for retry.
 *
 * Callers should pass their own (statically allocated, 16-byte
 * aligned) LINKFIX table sized to the queue count they need. The
 * table is left in BSS / app SRAM -- the HAL never owns it.
 *
 * @param[in,out] linkfix_table Caller-owned LINKFIX table.
 * @param[in]     entry_count   Queue count (1..32).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             GWCA in OPERATION mode; LINKFIX live.
 * @retval k_ra8_err_invalid_arg Table is null or count out of range.
 * @retval k_ra8_err_hw_timeout One of the state-machine transitions
 *                              never converged. GWCA left in DISABLE.
 *
 * @pre ::ra8_eth_gwca_init has been called (MSTP-gate cleared).
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMS.OPS == OPERATION and queues are walkable.
 * @post On failure GWMS.OPS == DISABLE.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_bring_up(ra8_gwca_basic_descriptor_t* linkfix_table,
                                              uint32_t                     entry_count);

/**
 * @brief Per-queue configuration descriptor for ::ra8_eth_gwca_configure_queue.
 *
 * @details Populated by the caller and passed to configure_queue;
 * holds the user-facing GWDCC fields plus the chain-head pointer
 * that goes into the matching LINKFIX entry.
 *
 * @note GWDCC.SM (Synchronization Mode) is always programmed to 00b
 * (Normal mode, full descriptor write-back) -- it is NOT a source-MAC
 * selector. Which port's frames land in this queue is decided by the
 * MFWD forwarding fabric (FWPBFCSDC0), not by the GWCA queue config.
 */
typedef struct {
  uint8_t priority;     /**< DCP[2:0]: class priority (0..7).                   */
  bool    is_tx;        /**< DQT: true = TX, false = RX.                        */
  bool    stop_on_last; /**< SL: stop processing on last.                       */
  bool    extended;     /**< EDE: true = 16-byte extended descriptors.          */
  void*   chain_head;   /**< First descriptor in the queue (basic or extended). */
} ra8_eth_gwca_queue_cfg_t;

/**
 * @brief Wire a per-queue config into GWDCC[i] and the matching LINKFIX entry.
 *
 * @details Composes the GWDCC[queue_index] value from @p cfg (DQT /
 * DCP / SL bits; SM is always 00b Normal write-back), writes it,
 * then points the matching LINKFIX entry's PTR at @p cfg->chain_head
 * (transitions that entry out of LEMPTY to LINKFIX so the queue
 * becomes walkable on the next GWMC.OPC transition to OPERATION).
 *
 * Caller must already have invoked ::ra8_eth_gwca_install_linkfix
 * with the table that backs @p linkfix_table[queue_index]. GWDCC[i]
 * is RESET/CONFIG-only-writable, so caller must be in CONFIG mode
 * when invoking this. After the GWCA reaches OPERATION the queue
 * must be armed with ::ra8_eth_gwca_reload_queue.
 *
 * @param[in,out] linkfix_table The same table passed to install_linkfix.
 * @param[in]     queue_index    Queue number 0..31.
 * @param[in]     cfg            Per-queue settings + chain head pointer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              GWDCC[i] + LINKFIX[i] wired.
 * @retval k_ra8_err_invalid_arg ``linkfix_table`` or ``cfg`` null, queue out of range.
 *
 * @pre ::ra8_eth_gwca_install_linkfix returned ok.
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre ``cfg->chain_head`` points at a descriptor array the chip can DMA from.
 * @post GWDCC[queue_index] reflects cfg's settings.
 * @post linkfix_table[queue_index] has dt = LINKFIX with PTR = chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_configure_queue(ra8_gwca_basic_descriptor_t*    linkfix_table,
                                                     uint32_t                        queue_index,
                                                     const ra8_eth_gwca_queue_cfg_t* cfg);

/**
 * @brief Reload (arm) a descriptor queue by pulsing GWDCC[i].BALR.
 *
 * @details Sets GWDCC[queue_index].BALR (Base Address Load Request)
 * and waits for the GWCA to self-clear it. Per HUM Ch 34.3 "GWDCCi",
 * BALR resets the AXI address RAM current_address field for the
 * queue to the chain base ({GWDCBAC} + i x 8). Until BALR runs the
 * GWCA never scans the descriptor chain, so every RX descriptor
 * stays FEMPTY and no frame is delivered. Must be called after the
 * GWCA reaches OPERATION (mirrors FSP
 * R_LAYER3_SWITCH_StartDescriptorQueue).
 *
 * @param[in] queue_index GWCA descriptor-queue number 0..63.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              BALR pulsed and self-cleared.
 * @retval k_ra8_err_invalid_arg queue_index has no GWDCC register.
 * @retval k_ra8_err_hw_timeout  GWDCC[i].BALR never self-cleared.
 *
 * @pre ::ra8_eth_gwca_configure_queue ran for queue_index.
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @post The AXI address RAM current_address for queue_index points at
 *       the chain base.
 * @post GWDCC[queue_index].BALR reads 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_reload_queue(uint32_t queue_index);

/**
 * @brief Initialise a descriptor chain as a ring of FEMPTY slots.
 *
 * @details Populates ``chain[0..ring_depth-1]`` as a circular ring
 * where each entry has dt = FEMPTY (data slot waiting for the chip
 * to fill, on RX queues, or for the app to fill, on TX queues), and
 * ds_l/ds_h = ``slot_bytes`` (per-slot buffer size). The chip walks
 * the ring; on RX queues it converts FEMPTY -> FSINGLE when a frame
 * lands, and on TX queues the app converts FEMPTY -> FSINGLE when
 * enqueuing a frame.
 *
 * The final entry can either be marked LINK back to chain[0]
 * (closed ring) or EOS (end-of-set, terminates processing). This
 * helper marks chain[ring_depth-1] as LINK with PTR = &chain[0].
 *
 * Buffer pointers are NOT set here -- caller fills them in via a
 * subsequent walk that populates each descriptor's ptr_h/ptr_l with
 * the address of a buffer the chip should read from / write to.
 *
 * @param[in,out] chain      Caller-owned descriptor array, 8-byte aligned.
 * @param[in]     ring_depth Number of entries; must be >= 2.
 * @param[in]     slot_bytes Per-slot buffer size in bytes (max 2048).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Ring initialised.
 * @retval k_ra8_err_null_ptr    chain is null.
 * @retval k_ra8_err_invalid_arg ring_depth < 2 or slot_bytes > 2048.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre chain is 8-byte aligned (chip requirement for basic descriptors).
 * @post chain[0..ring_depth-2] have dt = FEMPTY, ds = slot_bytes.
 * @post chain[ring_depth-1] has dt = LINK, PTR = &chain[0].
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_init_ring(ra8_gwca_basic_descriptor_t* chain,
                                               uint32_t                     ring_depth,
                                               uint32_t                     slot_bytes);

/**
 * @brief Set a single descriptor's data-buffer pointer.
 *
 * @details Encodes @p buffer as the 40-bit PTR field of @p desc
 * (high 8 bits in ptr_h, low 32 bits in ptr_l). Used by the caller
 * during chain init to attach a per-slot buffer to each FEMPTY
 * descriptor in a ring. Does not touch dt / ds / err / die fields.
 *
 * @param[in,out] desc   Descriptor to point at @p buffer.
 * @param[in]     buffer Address of the per-slot data buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           PTR field updated.
 * @retval k_ra8_err_null_ptr desc is null.
 *
 * @pre Caller is in GWMC.OPC = CONFIG (descriptor MMIO is RESET/CONFIG-only).
 * @post desc->ptr_h + desc->ptr_l encode @p buffer.
 * @post desc->dt / ds / err / die / info0 are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_set_descriptor_buffer(ra8_gwca_basic_descriptor_t* desc,
                                                           void*                        buffer);

/**
 * @brief Walk a ring and attach per-slot buffers from a static pool.
 *
 * @details Convenience wrapper that calls
 * ::ra8_eth_gwca_set_descriptor_buffer on every FEMPTY slot in the
 * ring (chain[0..ring_depth-2]), pointing each at a distinct slice
 * of the caller's buffer pool. The pool must hold at least
 * ``(ring_depth - 1) * slot_bytes`` bytes of contiguous storage.
 *
 * @param[in,out] chain      The ring previously initialized by
 *                           ::ra8_eth_gwca_init_ring.
 * @param[in]     ring_depth Same depth passed to init_ring.
 * @param[in]     slot_bytes Per-slot buffer size.
 * @param[in,out] pool       Caller-owned buffer pool (contiguous).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Every FEMPTY slot points at its buffer.
 * @retval k_ra8_err_null_ptr    chain or pool is null.
 * @retval k_ra8_err_invalid_arg ring_depth < 2 or slot_bytes is 0.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre Pool spans at least (ring_depth - 1) * slot_bytes bytes.
 * @post Each chain[i].ptr_h/ptr_l (i in [0, ring_depth-1)) encodes
 *       pool + i * slot_bytes.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_attach_buffers(ra8_gwca_basic_descriptor_t* chain,
                                                    uint32_t                     ring_depth,
                                                    uint32_t                     slot_bytes,
                                                    uint8_t*                     pool);

/**
 * @brief Kick a TX queue -- request the chip to start transmitting.
 *
 * @details Sets the matching bit in GWTRC0 (queues 0..31) or
 * GWTRC1 (queues 32..63) per HUM Ch 34.3.6 + FSP
 * `R_LAYER3_SWITCH_StartDescriptorQueue`. Must be called after the
 * app has converted one or more FEMPTY descriptors to FSINGLE
 * (i.e. has data ready to send). The chip walks the queue, sends
 * the FSINGLE frames, and may flip them back to FEMPTY when done.
 *
 * For RX queues the analogous kick is forwarding-configured via
 * MFWD.FWPBFCSDCx; this function does not cover that path.
 *
 * @param[in] queue_index TX queue 0..63 (must have DQT=1 in GWDCC).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              GWTRCi bit set.
 * @retval k_ra8_err_invalid_arg queue_index >= 64.
 *
 * @pre Caller already filled at least one FSINGLE descriptor in the
 *      queue's chain.
 * @pre GWCA is in GWMC.OPC = OPERATION.
 * @post The matching GWTRC bit reflects the request; chip may begin
 *       transmitting on the next bus cycle.
 *
 * @note Not thread-safe with other GWTRC writes on the same word.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_kick_tx(uint32_t queue_index);

/**
 * @brief Find the next FEMPTY slot in a descriptor ring.
 *
 * @details Walks chain[0..ring_depth-2] (the data slots) looking
 * for the first entry with dt == FEMPTY. The last entry is the
 * LINK terminator and is skipped. Used by TX paths to find a slot
 * to fill with the next outgoing frame, and (with a different DT
 * compare) by RX paths to find a slot the chip has filled.
 *
 * @param[in]  chain      Ring previously initialised by init_ring.
 * @param[in]  ring_depth Same depth passed to init_ring.
 * @param[in]  match_dt   Descriptor type to match (FEMPTY for TX
 *                        slot search, FSINGLE for RX completion).
 * @param[in]  start_idx  Slot index to start scanning from (lets
 *                        the caller round-robin instead of always
 *                        starting at 0).
 * @param[out] out_index  Index of the first matching slot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              ``*out_index`` holds the slot index.
 * @retval k_ra8_err_no_data     No slot matched ``match_dt``.
 * @retval k_ra8_err_invalid_arg chain/out_index null, ring_depth < 2,
 *                              or start_idx out of range.
 *
 * @pre Caller is in GWMC.OPC = OPERATION (descriptors are live).
 * @post On success ``*out_index`` is a valid slot index in
 *       [start_idx, ring_depth-1).
 *
 * @note Not thread-safe with concurrent chip-side updates; caller
 *       must drive the search from a single thread.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_find_slot(const ra8_gwca_basic_descriptor_t* chain,
                                               uint32_t                           ring_depth,
                                               ra8_gwdcc_dt_t                     match_dt,
                                               uint32_t                           start_idx,
                                               uint32_t*                          out_index);

/**
 * @brief Enqueue one frame on a TX queue's descriptor ring.
 *
 * @details Finds the next FEMPTY slot via ::ra8_eth_gwca_find_slot,
 * memcpy's the frame into the slot's buffer (already attached via
 * ::ra8_eth_gwca_attach_buffers), sets ``ds`` = frame_len, flips
 * dt to FSINGLE so the chip will send it, and advances the
 * caller's tail-index cursor. Caller is responsible for calling
 * ::ra8_eth_gwca_kick_tx afterward to actually trigger transmission.
 *
 * @param[in,out] chain           TX descriptor ring.
 * @param[in]     ring_depth      Ring depth.
 * @param[in,out] tail_idx        Caller's round-robin write cursor.
 * @param[in]     frame           Frame bytes to send.
 * @param[in]     frame_len       Frame length (must fit in slot_bytes).
 * @param[in]     slot_bytes      Per-slot buffer capacity.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Frame queued; FSINGLE marked.
 * @retval k_ra8_err_no_data     All slots FSINGLE (queue full).
 * @retval k_ra8_err_invalid_arg Null pointer or frame_len > slot_bytes.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre chain initialised via init_ring + attach_buffers.
 * @post On success the chosen slot is FSINGLE with the frame copied.
 * @post On success ``*tail_idx`` advanced past the chosen slot.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_tx_frame(ra8_gwca_basic_descriptor_t* chain,
                                              uint32_t                     ring_depth,
                                              uint32_t*                    tail_idx,
                                              const uint8_t*               frame,
                                              uint32_t                     frame_len,
                                              uint32_t                     slot_bytes);

/**
 * @brief Dequeue one frame from an RX queue's descriptor ring.
 *
 * @details Finds the next FSINGLE slot via ::ra8_eth_gwca_find_slot
 * (the chip filled it), memcpy's the frame out of the slot's
 * buffer into the caller's buffer, flips dt back to FEMPTY and
 * restores the slot's DS field to @p slot_bytes so the chip can
 * refill, and advances the caller's head-index cursor.
 *
 * @p slot_bytes MUST match the value passed to ::ra8_eth_gwca_init_ring:
 * the GWCA overwrites DS with the received length on write-back, so
 * leaving it un-restored would shrink the descriptor's apparent
 * capacity and make the next larger frame fragment into FSTART/FEND.
 *
 * @param[in,out] chain           RX descriptor ring.
 * @param[in]     ring_depth      Ring depth.
 * @param[in,out] head_idx        Caller's round-robin read cursor.
 * @param[out]    out_frame       Destination for the frame bytes.
 * @param[in]     out_capacity    Size of ``out_frame``.
 * @param[in]     slot_bytes      Per-slot buffer capacity (restored into DS).
 * @param[out]    out_len         Number of bytes actually written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Frame copied out; FEMPTY marked.
 * @retval k_ra8_err_no_data     No FSINGLE slot (no inbound frame yet).
 * @retval k_ra8_err_invalid_arg Null pointer or frame > out_capacity.
 *
 * @pre Caller is in GWMC.OPC = OPERATION.
 * @pre RX queue's MFWD forwarding cfg has been programmed.
 * @post On success the chosen slot is FEMPTY with DS = slot_bytes.
 * @post On success ``*head_idx`` advanced past the chosen slot.
 * @post On success ``*out_len`` <= out_capacity.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_rx_frame(ra8_gwca_basic_descriptor_t* chain,
                                              uint32_t                     ring_depth,
                                              uint32_t*                    head_idx,
                                              uint8_t*                     out_frame,
                                              uint32_t                     out_capacity,
                                              uint32_t                     slot_bytes,
                                              uint32_t*                    out_len);

/**
 * @brief State block for ::ra8_eth_gwca_default_open / _send / _recv.
 *
 * @details Caller-owned block holding the LINKFIX table + RX/TX
 * chain heads + per-queue head/tail cursors that the
 * default-path API tracks. Sized for one TX + one RX queue.
 *
 * @par Layout policy:
 * All descriptor + buffer storage is caller-supplied (BSS-resident
 * arrays) so the HAL doesn't pull in any allocator. Caller picks
 * ring depths and slot sizes at static-allocation time.
 */
typedef struct {
  /** LINKFIX table covering at least 2 queues (RX + TX). */
  ra8_gwca_basic_descriptor_t* linkfix_table;
  /** Number of LINKFIX entries. */
  uint32_t linkfix_count;
  /** RX descriptor chain (FEMPTY ring of ``rx_depth`` entries). */
  ra8_gwca_basic_descriptor_t* rx_chain;
  uint32_t                     rx_depth;       /**< RX depth.                       */
  uint8_t*                     rx_pool;        /**< RX pool.                        */
  uint32_t                     rx_slot_bytes;  /**< RX slot bytes.                  */
  uint32_t                     rx_queue_index; /**< LINKFIX entry + GWDCC[i] index. */
  uint32_t                     rx_head;        /**< Round-robin read cursor.        */
  /** TX descriptor chain -- 16-byte EXTENDED descriptors (EDE = 1).
   *  FEMPTY ring of ``tx_depth`` entries; the last is a LINK
   *  terminator. The TX path uses extended descriptors so each frame
   *  carries its INFO1 routing metadata (direct-descriptor format +
   *  destination vector). */
  ra8_gwca_ext_descriptor_t* tx_chain;
  uint32_t                   tx_depth;       /**< TX depth.                 */
  uint8_t*                   tx_pool;        /**< TX pool.                  */
  uint32_t                   tx_slot_bytes;  /**< TX slot bytes.            */
  uint32_t                   tx_queue_index; /**< TX queue index.           */
  uint32_t                   tx_tail;        /**< Round-robin write cursor. */
  /** MAC port (0..3) that frames are sourced from / sent to. */
  uint8_t mac_port;
} ra8_eth_gwca_default_state_t;

/**
 * @brief One-call GWCA bring-up: install LINKFIX, configure RX + TX, OPERATION.
 *
 * @details Wraps the canonical bring-up sequence for an app that
 * just wants "one RX queue + one TX queue going":
 *
 *   ra8_eth_gwca_init();
 *   ra8_eth_gwca_init_ring + attach_buffers on RX and TX chains;
 *   set_operation_mode(CONFIG);
 *   axi_init();
 *   install_linkfix(state->linkfix_table, state->linkfix_count);
 *   configure_queue(RX_QUEUE, rx_cfg);
 *   configure_queue(TX_QUEUE, tx_cfg);
 *   set_operation_mode(DISABLE);
 *   set_operation_mode(OPERATION);
 *
 * After this returns ok, callers can start pushing frames with
 * ::ra8_eth_gwca_default_send and polling with
 * ::ra8_eth_gwca_default_recv.
 *
 * @param[in,out] state Pre-populated state block with caller-owned
 *                      LINKFIX + chains + buffer pools.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              GWCA in OPERATION with queues live.
 * @retval k_ra8_err_invalid_arg state pointer or fields invalid.
 * @retval k_ra8_err_hw_timeout  Mode transition never converged.
 *
 * @pre state's chain/pool/table pointers are all valid + 16-byte aligned.
 * @pre rx_queue_index and tx_queue_index are distinct + < linkfix_count.
 * @post On success, GWMC.OPC = OPERATION; queues walkable.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_default_open(ra8_eth_gwca_default_state_t* state);

/**
 * @brief One-call TX: enqueue frame on state->tx_chain + kick.
 *
 * @details Convenience wrapper around ::ra8_eth_gwca_tx_frame +
 * ::ra8_eth_gwca_kick_tx using state->tx_chain/tail/queue_index.
 *
 * @param[in,out] state Initialized by default_open.
 * @param[in]     frame Frame bytes.
 * @param[in]     len   Frame length.
 *
 * @return ra8_err_t Error code propagated from tx_frame + kick_tx.
 * @retval k_ra8_ok               Frame queued + TX request fired.
 * @retval k_ra8_err_no_data      Queue full.
 * @retval k_ra8_err_invalid_arg  len > tx_slot_bytes or state invalid.
 *
 * @pre default_open returned ok.
 * @post On success state->tx_tail advanced.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_eth_gwca_default_send(ra8_eth_gwca_default_state_t* state, const uint8_t* frame, uint32_t len);

/**
 * @brief One-call RX: dequeue next frame from state->rx_chain.
 *
 * @details Convenience wrapper around ::ra8_eth_gwca_rx_frame.
 *
 * @param[in,out] state        Initialized by default_open.
 * @param[out]    out_frame    Destination for the frame.
 * @param[in]     out_capacity Size of out_frame.
 * @param[out]    out_len      Frame length written.
 *
 * @return ra8_err_t Error code propagated from rx_frame.
 * @retval k_ra8_ok               Frame copied; slot reset to FEMPTY.
 * @retval k_ra8_err_no_data      No inbound frame waiting.
 * @retval k_ra8_err_invalid_arg  capacity 0 or frame too large.
 *
 * @pre default_open returned ok.
 * @post On success state->rx_head advanced.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gwca_default_recv(ra8_eth_gwca_default_state_t* state,
                                                  uint8_t*                      out_frame,
                                                  uint32_t                      out_capacity,
                                                  uint32_t*                     out_len);

#ifdef __cplusplus
}
#endif

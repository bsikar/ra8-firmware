/**
 * @file ra_eth_gwca.h
 * @brief Ethernet CPU Agent (GWCA) driver
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

#include "ra8d2_ether_regs.h"
#include "ra_err.h"

/**
 * @typedef ra_eth_gwca_event_fn_t
 * @brief GWCA event callback.
 */
typedef void (*ra_eth_gwca_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise GWCA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_init(void);

/** @brief Tear down GWCA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_deinit(void);

/** @brief Read GWCA_STS. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask);

/** @brief Clear GWCA_STS bits via GWCA_ICLR. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx);

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
void ra_eth_gwca_dispatch(void);

/** @brief Put GWCA into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_exit_stop(void);

/**
 * @brief Transition the GWCA / ESWM state machine to a new OPC mode.
 *
 * @details Writes GWMC.OPC[1:0] = @p mode and polls GWMS.OPS[1:0]
 * until it reflects the new mode (or the bounded poll budget elapses).
 * This is the canonical state-machine transition for GWCA, used by
 * the LINKFIX init sequence (RESET -> DISABLE -> CONFIG -> ... ->
 * OPERATION). Mirrors FSP `r_layer3_switch_update_gwca_operation_mode`.
 *
 * @param[in] mode Target operation mode from ::ra_gwmc_opc_t.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok             GWMS.OPS now reflects @p mode.
 * @retval k_ra_err_invalid_arg @p mode is out of range.
 * @retval k_ra_err_hw_timeout GWMS.OPS never converged.
 *
 * @pre ::ra_eth_gwca_init has been called (MSTP-gate cleared).
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMC.OPC and GWMS.OPS both equal @p mode.
 * @post On timeout GWMC.OPC may have been written even if OPS
 *       never converged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_set_operation_mode(ra_gwmc_opc_t mode);

/**
 * @brief Request the GWCA AXI bridge to initialize via GWARIRM.ARIOG.
 *
 * @details Asserts GWARIRM.ARIOG (bit 0) and polls GWARIRM.ARR
 * (bit 1) until it reads 1. Called once from the LINKFIX init flow
 * after entering CONFIG mode. Mirrors the FSP
 * `r_layer3_switch_initialize_gwca` AXI-init step.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              ARR asserted within the budget.
 * @retval k_ra_err_hw_timeout ARR never asserted.
 *
 * @pre ::ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config) returned ok.
 * @post On success ARR=1 indicates the AXI manager is ready.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_axi_init(void);

/**
 * @brief Install a fresh LINKFIX table at GWDCBAC0/1.
 *
 * @details Programs the chip with the address of a SW-side LINKFIX
 * table (an array of ::ra_gwca_basic_descriptor_t indexed by queue
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
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCBAC0/1 programmed.
 * @retval k_ra_err_invalid_arg ``linkfix_table`` is null or count > 32.
 *
 * @pre ::ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config) returned ok.
 * @pre ::ra_eth_gwca_axi_init returned ok.
 * @pre ``linkfix_table`` is 16-byte aligned (chip requirement).
 * @post Every LINKFIX entry has dt = k_ra_gwdcc_dt_lempty.
 * @post GWDCBAC0/1 = address of ``linkfix_table``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_install_linkfix(ra_gwca_basic_descriptor_t* linkfix_table,
                                                   uint32_t                    entry_count);

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok             GWCA in OPERATION mode; LINKFIX live.
 * @retval k_ra_err_invalid_arg Table is null or count out of range.
 * @retval k_ra_err_hw_timeout One of the state-machine transitions
 *                              never converged. GWCA left in DISABLE.
 *
 * @pre ::ra_eth_gwca_init has been called (MSTP-gate cleared).
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMS.OPS == OPERATION and queues are walkable.
 * @post On failure GWMS.OPS == DISABLE.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_bring_up(ra_gwca_basic_descriptor_t* linkfix_table,
                                            uint32_t                    entry_count);

/**
 * @brief Per-queue configuration descriptor for ::ra_eth_gwca_configure_queue.
 *
 * @details Populated by the caller and passed to configure_queue;
 * holds the four user-facing GWDCC fields plus the chain-head
 * pointer that goes into the matching LINKFIX entry.
 */
typedef struct {
  uint8_t                     source_mac_port; /**< SM[1:0]: MAC port (0..3). */
  uint8_t                     priority;        /**< DCP[2:0]: class priority (0..7). */
  bool                        is_tx;           /**< DQT: true = TX, false = RX. */
  bool                        stop_on_last;    /**< SL: stop processing on last. */
  ra_gwca_basic_descriptor_t* chain_head;      /**< First descriptor in the queue. */
} ra_eth_gwca_queue_cfg_t;

/**
 * @brief Wire a per-queue config into GWDCC[i] and the matching LINKFIX entry.
 *
 * @details Composes the GWDCC[queue_index] value from @p cfg (SM /
 * DQT / DCP / SL bits), writes it, then points the matching LINKFIX
 * entry's PTR at @p cfg->chain_head (transitions that entry out of
 * LEMPTY to LINKFIX so the queue becomes walkable on the next
 * GWMC.OPC transition to OPERATION).
 *
 * Caller must already have invoked ::ra_eth_gwca_install_linkfix
 * with the table that backs @p linkfix_table[queue_index]. GWDCC[i]
 * is RESET/CONFIG-only-writable, so caller must be in CONFIG mode
 * when invoking this.
 *
 * @param[in,out] linkfix_table The same table passed to install_linkfix.
 * @param[in]     queue_index    Queue number 0..31.
 * @param[in]     cfg            Per-queue settings + chain head pointer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              GWDCC[i] + LINKFIX[i] wired.
 * @retval k_ra_err_invalid_arg ``linkfix_table`` or ``cfg`` null, queue out of range.
 *
 * @pre ::ra_eth_gwca_install_linkfix returned ok.
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre ``cfg->chain_head`` points at a descriptor array the chip can DMA from.
 * @post GWDCC[queue_index] reflects cfg's settings.
 * @post linkfix_table[queue_index] has dt = LINKFIX with PTR = chain_head.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_configure_queue(ra_gwca_basic_descriptor_t*    linkfix_table,
                                                   uint32_t                       queue_index,
                                                   const ra_eth_gwca_queue_cfg_t* cfg);

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Ring initialised.
 * @retval k_ra_err_null_ptr    chain is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes > 2048.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre chain is 8-byte aligned (chip requirement for basic descriptors).
 * @post chain[0..ring_depth-2] have dt = FEMPTY, ds = slot_bytes.
 * @post chain[ring_depth-1] has dt = LINK, PTR = &chain[0].
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_init_ring(ra_gwca_basic_descriptor_t* chain,
                                             uint32_t                    ring_depth,
                                             uint32_t                    slot_bytes);

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok           PTR field updated.
 * @retval k_ra_err_null_ptr desc is null.
 *
 * @pre Caller is in GWMC.OPC = CONFIG (descriptor MMIO is RESET/CONFIG-only).
 * @post desc->ptr_h + desc->ptr_l encode @p buffer.
 * @post desc->dt / ds / err / die / info0 are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_set_descriptor_buffer(ra_gwca_basic_descriptor_t* desc,
                                                         void*                       buffer);

/**
 * @brief Walk a ring and attach per-slot buffers from a static pool.
 *
 * @details Convenience wrapper that calls
 * ::ra_eth_gwca_set_descriptor_buffer on every FEMPTY slot in the
 * ring (chain[0..ring_depth-2]), pointing each at a distinct slice
 * of the caller's buffer pool. The pool must hold at least
 * ``(ring_depth - 1) * slot_bytes`` bytes of contiguous storage.
 *
 * @param[in,out] chain      The ring previously initialized by
 *                           ::ra_eth_gwca_init_ring.
 * @param[in]     ring_depth Same depth passed to init_ring.
 * @param[in]     slot_bytes Per-slot buffer size.
 * @param[in,out] pool       Caller-owned buffer pool (contiguous).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Every FEMPTY slot points at its buffer.
 * @retval k_ra_err_null_ptr    chain or pool is null.
 * @retval k_ra_err_invalid_arg ring_depth < 2 or slot_bytes is 0.
 *
 * @pre Caller is in GWMC.OPC = CONFIG.
 * @pre Pool spans at least (ring_depth - 1) * slot_bytes bytes.
 * @post Each chain[i].ptr_h/ptr_l (i in [0, ring_depth-1)) encodes
 *       pool + i * slot_bytes.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_attach_buffers(ra_gwca_basic_descriptor_t* chain,
                                                  uint32_t                    ring_depth,
                                                  uint32_t                    slot_bytes,
                                                  uint8_t*                    pool);

#ifdef __cplusplus
}
#endif

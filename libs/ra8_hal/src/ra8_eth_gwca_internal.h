/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_eth_gwca_internal.h
 * @brief Cross-TU shared surface for the ra8_eth_gwca driver split.
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The ra8_eth_gwca driver is split across three translation units to
 * stay under the per-file line-count cap: ra8_eth_gwca.c holds the
 * lifecycle / status / dispatch surface plus the GWCA state-machine
 * bring-up (set_operation_mode / axi_init / install_linkfix /
 * bring_up); ra8_eth_gwca_queue.c holds the per-queue descriptor and
 * ring primitives (configure_queue / reload_queue / init_ring /
 * attach_buffers / kick_tx / find_slot / tx_frame); and
 * ra8_eth_gwca_default.c holds the one-call default-state API
 * (default_open / default_send / default_recv / rx_frame). This
 * src/-local header carries only the handful of symbols those TUs
 * share: the poll-budget enum, the bench-side step trail (enum +
 * three debug globals), and the two address-encoding descriptor
 * helpers. It is NOT part of the public ABI.
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_eth_gwca.h"

/**
 * @brief Bounded poll budgets for the GWCA convergence spins.
 *
 * @details ~10 ms ceiling at 1 GHz CPU with ~5 cycles per iter. The
 * state machine transitions in FSP take a handful of CANFDCLK ticks
 * on real silicon. On host (RA8_OFF_TARGET) the loops run against
 * the mmap'd peri region and the bits flip immediately, so the
 * budget is mostly hit-once. Shared because ra8_eth_gwca.c spins on
 * GWMS.OPS / GWARIRM.ARR, ra8_eth_gwca_queue.c spins on GWDCCi.BALR,
 * and ra8_eth_gwca_default.c spins on TX descriptor write-back.
 */
enum : uint32_t {
  k_ra8_eth_gwca_mode_spin    = 2000000UL, /**< GWMS.OPS / GWARIRM.ARR poll budget.   */
  k_ra8_eth_gwca_balr_spin    = 2000000UL, /**< GWDCCi.BALR self-clear poll budget.   */
  k_ra8_eth_gwca_tx_done_spin = 2000000UL, /**< TX descriptor write-back poll budget. */
};

/**
 * @enum ra8_eth_gwca_step_t
 * @brief Step values bumped through ::g_ra8_eth_gwca_open_step,
 *        ::g_ra8_eth_gwca_pre_step, and ::g_ra8_eth_gwca_bring_up_step.
 *
 * @details Successful progression uses values 0..6. Failure codes are
 * ``0x10 | N`` so a JTAG-attached operator can tell at a glance whether
 * the chip parked on a happy-path step or an error path. Shared
 * because ra8_eth_gwca.c bumps the bring-up trail while
 * ra8_eth_gwca_default.c bumps the open / pre trails.
 */
typedef enum : uint32_t {
  k_ra8_eth_gwca_step_ok_1   = 1U,    /**< RA8 Ethernet gwca step ok 1.   */
  k_ra8_eth_gwca_step_ok_2   = 2U,    /**< RA8 Ethernet gwca step ok 2.   */
  k_ra8_eth_gwca_step_ok_3   = 3U,    /**< RA8 Ethernet gwca step ok 3.   */
  k_ra8_eth_gwca_step_ok_4   = 4U,    /**< RA8 Ethernet gwca step ok 4.   */
  k_ra8_eth_gwca_step_ok_5   = 5U,    /**< RA8 Ethernet gwca step ok 5.   */
  k_ra8_eth_gwca_step_ok_6   = 6U,    /**< RA8 Ethernet gwca step ok 6.   */
  k_ra8_eth_gwca_step_fail_1 = 0x11U, /**< RA8 Ethernet gwca step fail 1. */
  k_ra8_eth_gwca_step_fail_2 = 0x12U, /**< RA8 Ethernet gwca step fail 2. */
  k_ra8_eth_gwca_step_fail_3 = 0x13U, /**< RA8 Ethernet gwca step fail 3. */
  k_ra8_eth_gwca_step_fail_4 = 0x14U, /**< RA8 Ethernet gwca step fail 4. */
  k_ra8_eth_gwca_step_fail_5 = 0x15U, /**< RA8 Ethernet gwca step fail 5. */
  k_ra8_eth_gwca_step_fail_6 = 0x16U, /**< RA8 Ethernet gwca step fail 6. */
} ra8_eth_gwca_step_t;

/**
 * @var g_ra8_eth_gwca_open_step
 * @brief Bench-side debug trail bumped by ::ra8_eth_gwca_default_open.
 *
 * @details Read externally via J-Link `mem32` when the chip parks in
 * panic_halt to identify which sub-primitive failed:
 *   1 = internal_default_open_pre ok (init + rings + bring_up + CONFIG).
 *   2 = internal_default_open_queues ok (RX/TX cfgs written).
 *   3 = set_operation_mode(OPERATION) ok (final).
 * Plus the symmetric error codes ``0x10|N`` for failures at step N.
 * Defined in ra8_eth_gwca.c; bumped from ra8_eth_gwca_default.c.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
extern volatile uint32_t g_ra8_eth_gwca_open_step;

/**
 * @var g_ra8_eth_gwca_pre_step
 * @brief Bench-side debug trail bumped inside ::internal_default_open_pre.
 *
 * @details Lets a JTAG-attached operator identify which sub-primitive
 * inside the pre-phase failed when ``g_ra8_eth_gwca_open_step == 0x11``:
 *   1 = ra8_eth_gwca_init ok.
 *   2 = internal_default_open_rings ok.
 *   3 = ra8_eth_gwca_bring_up ok.
 *   4 = set_operation_mode(CONFIG) ok.
 * Error codes ``0x10|N`` for failures at step N. Defined in
 * ra8_eth_gwca.c; bumped from ra8_eth_gwca_default.c.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
extern volatile uint32_t g_ra8_eth_gwca_pre_step;

/**
 * @var g_ra8_eth_gwca_bring_up_step
 * @brief Bench-side debug trail bumped inside ::ra8_eth_gwca_bring_up.
 *
 * @details Pinpoints which of the six sub-steps failed:
 *   1 = first DISABLE ok.
 *   2 = DISABLE -> CONFIG ok.
 *   3 = AXI init ok (GWARIRM handshake).
 *   4 = install_linkfix ok.
 *   5 = second DISABLE ok.
 *   6 = DISABLE -> OPERATION ok.
 * Error codes ``0x10|N`` for failures at step N. Defined in
 * ra8_eth_gwca.c; bumped from ra8_eth_gwca.c (bring-up phase).
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
extern volatile uint32_t g_ra8_eth_gwca_bring_up_step;

/**
 * @brief Promote a LINKFIX entry from LEMPTY to LINKFIX with chain-head PTR.
 *
 * @details Splits the 40-bit chain-head address into ptr_h (high 8
 * bits) + ptr_l (low 32 bits) and writes them with dt = LINKFIX.
 * No MMIO is touched -- caller-owned table memory only. Defined in
 * ra8_eth_gwca_queue.c and shared with ra8_eth_gwca_default.c
 * (the re-arm path restores a LINK terminator through it).
 *
 * @param[in,out] entry      LINKFIX entry to rewrite.
 * @param[in]     chain_head Address to encode into the 40-bit PTR field
 *                           (basic or extended descriptor array head).
 *
 * @return void This helper never fails.
 * @note Always returns normally.
 *
 * @pre entry is non-null and previously initialised by install_linkfix.
 * @pre chain_head is the head of the queue's descriptor array.
 * @post entry->dt == LINKFIX; ptr_h/ptr_l carry the 40-bit address.
 * @post No other LINKFIX entries are modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_eth_gwca_set_linkfix_entry(ra8_gwca_basic_descriptor_t* entry,
                                             const void*                  chain_head);

/**
 * @brief Decode a descriptor's 40-bit PTR back to a host pointer.
 *
 * @details Reverses the ptr_h / ptr_l split applied by
 * ::ra8_eth_gwca_set_descriptor_buffer. On a 32-bit MCU like the
 * RA8D2 the high byte is always zero, but the function handles the
 * 40-bit format generically. Defined in ra8_eth_gwca_queue.c and
 * shared with ra8_eth_gwca_default.c (the RX drain path decodes the
 * filled slot's buffer pointer through it).
 *
 * @param[in] desc Descriptor whose PTR to decode.
 *
 * @return Host-visible buffer pointer or nullptr if desc is null.
 * @retval pointer Valid buffer address.
 * @retval nullptr desc was null.
 *
 * @pre desc was previously initialised via attach_buffers or
 *      set_descriptor_buffer.
 * @pre Caller treats a nullptr return as an invalid descriptor.
 * @post Returned pointer matches the address the descriptor encodes.
 * @post No state is modified.
 *
 * @note Pure helper; no thread-safety concerns.
 * @since 0.1.0
 */
RA8_PRIV uint8_t* ra8_eth_gwca_decode_ptr(const ra8_gwca_basic_descriptor_t* desc);

#ifdef __cplusplus
}
#endif

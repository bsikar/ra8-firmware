/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_dsi_internal.h
 * @brief Module-private link seam shared between the MIPI DSI-2 host
 * @ingroup grp_hal_display
 *        driver translation units.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The MIPI DSI-2 host driver is implemented across more than one
 * translation unit so that no single file exceeds the
 * ``scripts/checks/check_file_size.py`` cap. The configuration / link /
 * sequence-channel path lives in ``ra8_mipi_dsi.c``; the status, IRQ
 * dispatch, video-mode, and convenience surfaces live in
 * ``ra8_mipi_dsi_dispatch.c``. A handful of file-scope mutable state
 * words and one bounded-poll helper are read and written from both TUs.
 *
 * This header is private to the MIPI DSI-2 driver only -- no other
 * module includes it. It exports:
 *  - the four mutable state words shared between the command-submission
 *    path (which arms the pending-RX buffer and registers the IRQ
 *    callback) and the dispatch path (which consumes them);
 *  - the bounded register-poll helper used by both the HS-clock path
 *    and the video-mode path.
 *
 * Read-only constants (the log tag) are intentionally NOT shared: each
 * TU keeps its own ``static`` copy with an identical initialiser.
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_regs.h"

/**
 * @enum ra8_mipi_dsi_internal_t
 * @brief Numeric constants used by the implementation that don't
 *        belong in the public header.
 *
 * @details
 * Following the project's "no magic numbers" rule -- every literal
 * embedded in the driver code path lives here as a typed enum value.
 * Shared between ``ra8_mipi_dsi.c`` and ``ra8_mipi_dsi_dispatch.c``.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_clear_all = 0xFFFFFFFFUL, /**< RW1C all-ones write mask. */
  k_ra8_mipi_dsi_isr_all = k_ra8_mipi_dsi_isr_sq0 | k_ra8_mipi_dsi_isr_sq1 | k_ra8_mipi_dsi_isr_vm |
                           k_ra8_mipi_dsi_isr_rcv | k_ra8_mipi_dsi_isr_ferr |
                           k_ra8_mipi_dsi_isr_ppi, /**< RA8 mipi dsi ISR all.  */
  k_ra8_mipi_dsi_byte_mask = 0xFFUL,               /**< 8-bit byte field.      */
  k_ra8_mipi_dsi_vc_mask   = 0x3UL,                /**< 2-bit virtual-channel. */
  k_ra8_mipi_dsi_dt_mask   = 0x3FUL,               /**< 6-bit DSI Data Type.   */
  k_ra8_mipi_dsi_bta_mask  = 0x3UL,                /**< 2-bit BTA selector.    */
} ra8_mipi_dsi_internal_t;

/**
 * @enum ra8_mipi_dsi_shifts_t
 * @brief Common shift positions used across the MIPI DSI-2 driver.
 *
 * @details
 * Shared between ``ra8_mipi_dsi.c`` and ``ra8_mipi_dsi_dispatch.c`` so
 * both TUs agree on the bit-field positions of the link, ULPS, DSI,
 * receive-result, and ack/error registers.
 */
typedef enum : uint8_t {
  k_shift_8              = 8U,  /**< Shift 8.              */
  k_ulpssetr_wkup_shift  = 0U,  /**< Ulpssetr wkup shift.  */
  k_dsisetr_mrpsz_shift  = 0U,  /**< Dsisetr mrpsz shift.  */
  k_dsisetr_vc_crc_shift = 20U, /**< Dsisetr vc CRC shift. */
  k_rxrss_data1_shift    = 8U,  /**< Rxrss data1 shift.    */
  k_rxrss_dt_shift       = 16U, /**< Rxrss dt shift.       */
  k_rxrss_vc_shift       = 22U, /**< Rxrss vc shift.       */
  k_akep_vc_shift        = 16U, /**< Akep vc shift.        */
} ra8_mipi_dsi_shifts_t;

/**
 * @var s_mipi_dsi_event_fn
 * @brief Currently registered IRQ callback, or `nullptr`.
 *
 * @details
 * Single definition lives in ``ra8_mipi_dsi.c``. Mutated from the
 * single-threaded init / deinit / attach-handler call sites; read from
 * IRQ context inside ``ra8_mipi_dsi_dispatch.c``.
 *
 * @note Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @warning Not thread-safe; treat as IRQ-shared state.
 * @since 0.1.0
 */
extern ra8_mipi_dsi_event_fn_t s_mipi_dsi_event_fn;

/**
 * @var s_mipi_dsi_event_ctx
 * @brief Opaque context handed back to the IRQ callback.
 *
 * @details
 * Single definition lives in ``ra8_mipi_dsi.c``. Lifetime is owned by
 * the caller of ``ra8_mipi_dsi_attach_handler``.
 *
 * @note May be NULL if the user never attached.
 * @warning Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @since 0.1.0
 */
extern void* s_mipi_dsi_event_ctx;

/**
 * @var s_mipi_dsi_pending_rx_buffer
 * @brief Buffer the caller passed to ``ra8_mipi_dsi_read_packet()``.
 *
 * @details
 * Single definition lives in ``ra8_mipi_dsi.c``. The receive ISR in
 * ``ra8_mipi_dsi_dispatch.c`` copies long-packet payload bytes into this
 * buffer once the response arrives and then clears it to avoid
 * stale-pointer use.
 *
 * @note Modified from read_packet / send_command and the RX dispatch.
 * @warning The pointer must remain valid until the IRQ fires.
 * @since 0.1.0
 */
extern uint8_t* s_mipi_dsi_pending_rx_buffer;

/**
 * @var s_mipi_dsi_pending_rx_len
 * @brief Capacity of ``s_mipi_dsi_pending_rx_buffer`` in bytes.
 *
 * @details
 * Single definition lives in ``ra8_mipi_dsi.c``. Cleared to zero once the
 * response is consumed by the RX dispatch.
 *
 * @note Treat as undefined when ``s_mipi_dsi_pending_rx_buffer`` is NULL.
 * @warning Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @since 0.1.0
 */
extern uint16_t s_mipi_dsi_pending_rx_len;

/**
 * @brief Bounded poll waiting for `(reg & mask) == expect`.
 *
 * @details
 * Defined in ``ra8_mipi_dsi.c`` and shared with the video-mode path in
 * ``ra8_mipi_dsi_dispatch.c``. Spins at most
 * ``k_ra8_mipi_dsi_busy_loop_max`` iterations so the loop bound is
 * statically provable (NASA Power of 10 Rule 2).
 *
 * @param[in] reg    Address of the register to poll.
 * @param[in] mask   Bits to test.
 * @param[in] expect Required value of `reg & mask`.
 *
 * @return `k_ra8_ok` if the condition was met within the iteration cap,
 *         otherwise `k_ra8_err_hw_timeout`.
 * @retval k_ra8_ok Condition satisfied before the iteration cap.
 * @retval k_ra8_err_hw_timeout Cap reached without the condition holding.
 *
 * @pre `reg` is a valid MMIO register address.
 * @pre Module clock is ungated.
 * @post No register is modified by this helper.
 * @post `reg` is read at least once.
 *
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_mipi_dsi_internal_wait_eq(volatile const uint32_t* reg,
                                                 uint32_t                 mask,
                                                 uint32_t                 expect);

/**
 * @file ra8_net_pal.h
 * @brief Network Platform Abstraction Layer for the RA8D2 ESWM block
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * scaffold for the ethernet PAL. The PAL sits between the
 * Ring-3 ``ra8_eth_*`` driver and any higher-level network stack
 * (NetX Duo today, possibly TCPDirect or a custom stack later).
 *
 * Responsibilities:
 *
 * - Own the MAC address.
 * - Translate ra8_eth_t error codes into stack-friendly status.
 * - Provide a single send/receive primitive a stack can call.
 * - Track link state and surface it to the stack.
 * - Hide the MSTP / clock-gate dance behind ``ra8_net_pal_init``.
 *
 * The PAL is intentionally stack-agnostic: no NetX Duo types appear in
 * this header, and the same would hold for any future TCPDirect or
 * zero-stack consumer.
 *
 * Its real consumers today are ``libs/ra8_nsc/src/ra8_nsc_eth.c`` (the
 * TrustZone veneer that exposes Ethernet to the Non-Secure world) and the
 * host tests. NetX Duo is NOT one of them: its driver
 * (``port/netxduo/src/nx_ether_driver_ra8_eth.c``) includes ``ra8_eth.h``
 * and calls ``ra8_eth_*`` directly, never this API (#621).
 *
 * ## Layering
 *
 * +------------------------+ ra8_net_pal_send_frame
 * | network stack (NetX)  | ra8_net_pal_recv_frame
 * +-----------+------------+ ra8_net_pal_link_status
 * |
 * v
 * +------------------------+
 * | ra8_net_pal (this file) | wraps Ring-3 ra8_eth_*
 * +-----------+------------+
 * |
 * v
 * +------------------------+
 * | ra8_eth (Ring 3 / HAL) |
 * +------------------------+
 *
 * ## Threading
 *
 * Single-threaded. Init runs from the boot path; send/recv are
 * called from the main loop or a single network task. Frame RX is
 * delivered through ``ra8_net_pal_recv_frame`` polling -- the IRQ
 * path lives inside ``ra8_eth`` and is fanned out via
 * ``ra8_net_pal_set_event_handler``.
 *
 * ## Send/recv backing store
 *
 * The PAL owns a small in-memory ring (``k_ra8_net_pal_ring_slots``
 * slots, each ``k_ra8_net_pal_frame_max`` bytes) the stack writes
 * to with ``ra8_net_pal_send_frame`` and drains with
 * ``ra8_net_pal_recv_frame``. On real hardware the ring is backed
 * by the GWCA descriptor engine; in host tests it is a plain
 * contiguous RAM buffer. The stack-facing contract is identical
 * in either case, so NetX Duo's driver talks to the same API.
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

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_net_pal_limits_t
 * @brief maximums / sizes baked into the PAL contract.
 *
 * @details
 * MTU is the standard Ethernet maximum payload (1500 bytes) plus
 * the 14-byte header. Frame size includes the FCS placeholder so
 * the descriptor ring can route 1518-byte frames without truncation.
 */
typedef enum : uint16_t {
  k_ra8_net_pal_mac_addr_len = 6U,    /**< 48-bit Ethernet MAC.   */
  k_ra8_net_pal_mtu          = 1500U, /**< Standard payload size. */
  k_ra8_net_pal_frame_max    = 1518U, /**< MTU + header + FCS.    */
} ra8_net_pal_limits_t;

/**
 * @enum ra8_net_pal_link_state_t
 * @brief Possible link-up states surfaced to the stack.
 */
typedef enum : uint8_t {
  k_ra8_net_pal_link_down = 0U, /**< Cable unplugged or PHY not up. */
  k_ra8_net_pal_link_up   = 1U, /**< Link up at any speed/duplex.   */
} ra8_net_pal_link_state_t;

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @struct ra8_net_pal_mac_t
 * @brief 48-bit MAC address container.
 *
 * @details
 * Wrapped in a struct so ``ra8_net_pal_set_mac_addr`` can take a
 * pointer to a complete object instead of a raw byte pointer; this
 * makes accidental length mismatches harder.
 */
/* The bytes field is read by ra8_net_pal_set_mac_addr / get_mac_addr
 * which cppcheck does not see if the test build excludes them. */
typedef struct {
  uint8_t bytes[k_ra8_net_pal_mac_addr_len]; /**< Bytes. */
} ra8_net_pal_mac_t;

/**
 * @typedef ra8_net_pal_event_fn_t
 * @brief Async event callback shape (link change, RX ready, error).
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] event_mask OR of ``k_ra8_net_pal_event_*`` bits.
 *
 * @note Invoked from ra8_eth ISR context. Must return quickly and
 * must not call back into ra8_net_pal_init / deinit.
 */
typedef void (*ra8_net_pal_event_fn_t)(void* ctx, uint32_t event_mask);

/**
 * @enum ra8_net_pal_event_t
 * @brief Bits passed to ``ra8_net_pal_event_fn_t``.
 */
typedef enum : uint32_t {
  k_ra8_net_pal_event_none      = 0x00U, /**< RA8 net pal event none. */
  k_ra8_net_pal_event_link_up   = 0x01U, /**< Link came up.           */
  k_ra8_net_pal_event_link_down = 0x02U, /**< Link went down.         */
  k_ra8_net_pal_event_rx_ready  = 0x04U, /**< RX descriptor has data. */
  k_ra8_net_pal_event_tx_done   = 0x08U, /**< TX descriptor freed.    */
  k_ra8_net_pal_event_error     = 0x10U, /**< MAC reported a fault.   */
} ra8_net_pal_event_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the network PAL.
 *
 * @details
 * Powers on the underlying ``ra8_eth`` driver via ``ra8_eth_init``,
 * loads the MAC address from the supplied descriptor (NULL keeps
 * the implementation default of all-zeros), and resets the
 * internal state machine to "link down, no event handler".
 *
 * @param[in] mac MAC address to programme. May be NULL to keep
 * whatever the underlying ESWM block already has.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok PAL ready, link state = down.
 * @retval k_ra8_err_hw_init_failed Underlying ``ra8_eth_init`` failed.
 *
 * @pre ``ra8_mstp_init`` and ``ra8_pwr_init`` have been called.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success, the PAL is ready and ``ra8_net_pal_link_status``
 * returns ``k_ra8_net_pal_link_down``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_net_pal_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_init(const ra8_net_pal_mac_t* mac);

/**
 * @brief Tear down the network PAL.
 *
 * @details
 * Releases any in-flight TX descriptors, detaches the event
 * handler, and calls ``ra8_eth_deinit`` to drop the ESWM MSTP
 * reference.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok PAL released.
 * @retval k_ra8_err_invalid_state PAL was never initialized.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 *
 * @post Link state reads as ``k_ra8_net_pal_link_down``.
 * @post Subsequent send/recv calls return ``k_ra8_err_invalid_state``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_net_pal_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_deinit(void);

/* =============================================================================
 * MAC address
 * =============================================================================
 */

/**
 * @brief Programme the PAL MAC address.
 *
 * @details
 * Updates the in-memory copy of the MAC and (when ra8_eth gains
 * MAC-write support) the ESWM hardware filter. Called by the
 * stack at any time after ``ra8_net_pal_init``.
 *
 * @param[in] mac Non-NULL MAC descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok MAC stored.
 * @retval k_ra8_err_null_ptr ``mac`` was NULL.
 * @retval k_ra8_err_invalid_state ``ra8_net_pal_init`` not called yet.
 *
 * @pre ``mac`` is non-NULL.
 * @pre PAL has been initialized.
 *
 * @post Subsequent ``ra8_net_pal_get_mac_addr`` returns ``mac``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_set_mac_addr(const ra8_net_pal_mac_t* mac);

/**
 * @brief Read the currently programmed MAC address.
 *
 * @param[out] out_mac Receives the MAC.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok MAC copied.
 * @retval k_ra8_err_null_ptr ``out_mac`` was NULL.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre ``out_mac`` is non-NULL.
 * @pre PAL has been initialized.
 *
 * @post No PAL state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_get_mac_addr(ra8_net_pal_mac_t* out_mac);

/* =============================================================================
 * Frame I/O
 * =============================================================================
 */

/**
 * @brief Hand a complete ethernet frame to the MAC for transmit.
 *
 * @details
 * Copies ``frame[0..len-1]`` into the next free TX ring slot. On
 * real hardware the slot is a GWCA descriptor; in the host build
 * it is a plain RAM buffer the PAL also exposes through
 * ``ra8_net_pal_recv_frame`` for loopback tests.
 *
 * @param[in] frame Ethernet frame bytes (header + payload, no FCS).
 * @param[in] len Frame length in bytes; non-zero, <= frame_max.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Frame queued for TX.
 * @retval k_ra8_err_null_ptr ``frame`` was NULL.
 * @retval k_ra8_err_invalid_arg ``len`` zero or out of range.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 * @retval k_ra8_err_no_mem TX ring full; try again later.
 *
 * @pre ``frame`` is non-NULL.
 * @pre PAL has been initialized.
 *
 * @post On success, the frame is queued and the caller may drop
 * ``frame``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_net_pal_recv_frame
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_send_frame(const uint8_t* frame, uint16_t len);

/**
 * @brief Pull the next received ethernet frame, if any, into a buffer.
 *
 * @details
 * If a frame is available it is copied into ``out_buf`` and
 * ``*inout_len`` is updated to the byte count actually written.
 * When no frame is ready the function returns
 * ``k_ra8_err_no_data`` so callers can poll without blocking.
 *
 * @param[out] out_buf Destination buffer.
 * @param[in,out] inout_len On entry: capacity of ``out_buf``.
 * On exit: bytes written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Frame copied.
 * @retval k_ra8_err_no_data No frame ready (poll-friendly).
 * @retval k_ra8_err_null_ptr ``out_buf`` / ``inout_len`` NULL.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 * @retval k_ra8_err_invalid_arg ``*inout_len`` < frame_max capacity.
 *
 * @pre ``out_buf`` and ``inout_len`` are non-NULL.
 * @pre ``*inout_len`` >= ``k_ra8_net_pal_frame_max``.
 * @pre PAL has been initialized.
 *
 * @post On success, ``*inout_len`` holds the actual frame length.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_net_pal_send_frame
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_recv_frame(uint8_t* out_buf, uint16_t* inout_len);

/* =============================================================================
 * Link state
 * =============================================================================
 */

/**
 * @brief Read the current link state.
 *
 * @param[out] out_state Receives link up/down.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Link state copied.
 * @retval k_ra8_err_null_ptr ``out_state`` was NULL.
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre ``out_state`` is non-NULL.
 * @pre PAL has been initialized.
 *
 * @post No PAL state is modified.
 *
 * @note Thread safety: not thread-safe with respect to the event
 * handler which can update link state from ISR context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_link_status(ra8_net_pal_link_state_t* out_state);

/* =============================================================================
 * Async event handler
 * =============================================================================
 */

/**
 * @brief Attach a single event handler for link / RX / TX events.
 *
 * @details
 * Replaces any previously installed handler. The PAL relays
 * ra8_eth ISR events into this callback after translating them
 * into the PAL-level ``k_ra8_net_pal_event_*`` bit set.
 *
 * @param[in] fn Callback. Pass NULL to detach.
 * @param[in] ctx Context passed to the callback.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handler installed (or detached).
 * @retval k_ra8_err_invalid_state PAL not initialized.
 *
 * @pre PAL has been initialized.
 *
 * @post Subsequent ra8_eth events are routed through ``fn``.
 *
 * @note Thread safety: not thread-safe; only call from
 * single-threaded init or with IRQs masked.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_pal_set_event_handler(ra8_net_pal_event_fn_t fn, void* ctx);

#ifdef __cplusplus
}
#endif

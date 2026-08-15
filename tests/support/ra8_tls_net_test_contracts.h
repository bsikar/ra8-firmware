/**
 * @file ra8_tls_net_test_contracts.h
 * @brief Contracts for file-local TLS network-adapter tests.
 * @details Declares only helpers defined by test_ra8_tls_net.c so complete
 *          contracts remain separate from the MC/DC vector bodies.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_tls.h"

/**
 * @brief Fragment TLS bytes into network-PAL frames.
 * @details Sends bounded chunks until input is consumed or the ring rejects one.
 * @param[in,out] ctx Initialized network BIO fixture.
 * @param[in] buf Readable TLS byte span.
 * @param[in] len Requested byte count.
 * @return Number of bytes accepted by the network PAL.
 * @retval 0 The request was empty or no frame could be queued.
 * @pre @p ctx points to an initialized @c np_bio_t.
 * @pre @p buf addresses @p len readable bytes when length is nonzero.
 * @post The return value never exceeds @p len.
 * @post Each accepted frame respects the configured MTU.
 * @note A full ring is reported as positive short progress.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_np_bio_send(void* ctx, const uint8_t* buf, size_t len);

/**
 * @brief Reassemble network-PAL frames into TLS bytes.
 * @details Retains unread frame bytes across successive callback invocations.
 * @param[in,out] ctx Initialized network BIO fixture.
 * @param[out] buf Writable TLS destination span.
 * @param[in] len Maximum bytes requested.
 * @return Number of bytes copied into the destination.
 * @retval 0 No frame was ready or the request length was zero.
 * @pre @p ctx points to an initialized @c np_bio_t.
 * @pre @p buf addresses @p len writable bytes when length is nonzero.
 * @post The return value never exceeds @p len.
 * @post Unreturned frame bytes remain buffered in source order.
 * @note The adapter consumes at most one new frame per call.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_np_bio_recv(void* ctx, uint8_t* buf, size_t len);

/**
 * @brief Reset the network BIO and build its TLS config.
 * @details Clears reassembly state, sets the minimum MTU, and binds callbacks.
 * @return Complete TLS session configuration for the network fixture.
 * @retval ra8_tls_session_cfg_t Value referencing the file-local BIO state.
 * @pre The file-local BIO object has static storage duration.
 * @pre The callback functions are linked into the test target.
 * @post Reassembly length and position both equal zero.
 * @post The returned config has nonnull BIO callbacks and context.
 * @note The helper does not initialize the network PAL itself.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_tls_session_cfg_t internal_make_np_cfg(void);

/**
 * @brief Verify MSS clamp arithmetic and null-output rejection.
 * @details Checks minimum and Ethernet-sized MTUs against fixed overhead.
 * @pre The MSS output objects are writable.
 * @pre Unity accounting is ready.
 * @post Accepted MTUs publish their exact payload sizes.
 * @post A null output pointer reports invalid-argument.
 * @note This vector does not initialize TLS or networking.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mss_clamp_values(void);

/**
 * @brief Cover both MSS rejection conditions independently.
 * @details Applies the documented three-vector MC/DC truth table.
 * @pre The MSS output object is writable.
 * @pre Unity accounting is ready.
 * @post The accepted vector publishes 88 bytes.
 * @post Each rejected vector clears the output and reports invalid-argument.
 * @note No module lifecycle state is changed.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_mss_clamp(void);

/**
 * @brief Verify off-target cipher and peer-verification accessors.
 * @details Handshakes over the PAL fixture and checks deterministic sentinels.
 * @pre TLS and network PAL global states begin inactive.
 * @pre The file-local MAC address is valid for host loopback.
 * @post Cipher name, identifier, and verification flags match expectations.
 * @post Session and both global modules are released before return.
 * @note The off-target cipher name is intentionally deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cipher_and_verify_fake(void);

/**
 * @brief Cover all cipher-accessor argument-guard conditions.
 * @details Varies identifier output, name output, and name capacity independently.
 * @pre TLS and network PAL global states begin inactive.
 * @pre A valid session can be opened over the network fixture.
 * @post The all-valid vector succeeds.
 * @post Each single invalid dependency reports invalid-argument.
 * @note The session need not handshake for the argument checks.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_cipher_arg_guard(void);

/**
 * @brief Cover bounded cipher-name truncation and termination.
 * @details Compares a four-byte output with a destination holding the full name.
 * @pre TLS and network PAL global states begin inactive.
 * @pre A valid off-target session completes its handshake.
 * @post The short output contains a terminated @c off prefix.
 * @post The full output contains the complete deterministic cipher name.
 * @note Both destinations remain within their declared capacities.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_cipher_name_copy(void);

/**
 * @brief Verify cipher and verification accessors reject invalid state.
 * @details Exercises inactive-module and forged-handle guards.
 * @pre TLS global state begins inactive.
 * @pre Local output storage is writable.
 * @post Inactive calls report not-initialized.
 * @post Forged-handle calls report invalid-argument.
 * @note The forged handle is never dereferenced.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_accessor_state_guards(void);

/**
 * @brief Verify TLS record exchange over the network PAL.
 * @details Initializes both modules, handshakes, exchanges a record, and reports state.
 * @pre TLS and network PAL global states begin inactive.
 * @pre The file-local MAC and BIO storage remain valid for the test.
 * @post The received record equals the transmitted bytes exactly.
 * @post Session and both global modules are released before return.
 * @note Host networking is a deterministic in-process frame ring.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_end_to_end_over_net_pal(void);

/**
 * @file ra8_tls_test_contracts.h
 * @brief Contracts for file-local TLS facade test helpers.
 * @details Declares only helpers defined by test_ra8_tls.c so complete
 *          contracts remain separate from the MC/DC vector bodies.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifndef RA8_TLS_TEST_CONTRACTS_H
/** @brief Include guard for TLS facade test contracts. */
#define RA8_TLS_TEST_CONTRACTS_H

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_tls.h"

/**
 * @brief Reset the loopback byte ring.
 * @details Clears indices, count, and backing bytes before a test vector.
 * @pre The file-local loopback object has static storage duration.
 * @pre No BIO callback is executing concurrently.
 * @post The ring contains zero readable bytes.
 * @post Producer and consumer indices both equal zero.
 * @note This helper mutates only the file-local fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_loop_reset(void);

/**
 * @brief Append TLS bytes to the loopback ring.
 * @details Copies as many source bytes as available ring capacity permits.
 * @param[in,out] ctx Unused BIO context retained for signature parity.
 * @param[in] buf Readable TLS byte span.
 * @param[in] len Requested byte count.
 * @return Number of bytes accepted by the ring.
 * @retval 0 The request was empty or the ring was full.
 * @pre @p buf addresses at least @p len readable bytes when length is nonzero.
 * @pre The requested length is representable by the callback return type.
 * @post The return value never exceeds @p len.
 * @post Accepted bytes are readable from the ring in source order.
 * @note The bounded loop advances once per accepted byte.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_loop_bio_send(void* ctx, const uint8_t* buf, size_t len);

/**
 * @brief Drain TLS bytes from the loopback ring.
 * @details Copies up to the requested count without synthesizing bytes.
 * @param[in,out] ctx Unused BIO context retained for signature parity.
 * @param[out] buf Writable destination byte span.
 * @param[in] len Maximum byte count to drain.
 * @return Number of bytes copied from the ring.
 * @retval 0 The request was empty or the ring contained no data.
 * @pre @p buf addresses at least @p len writable bytes when length is nonzero.
 * @pre The requested length is representable by the callback return type.
 * @post The return value never exceeds @p len.
 * @post Returned bytes preserve ring insertion order.
 * @note The bounded loop advances once per drained byte.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_loop_bio_recv(void* ctx, uint8_t* buf, size_t len);

/**
 * @brief Build a session config bound to the loopback callbacks.
 * @details Populates both BIO callbacks, their shared context, and hostname.
 * @return Complete loopback TLS session configuration.
 * @retval ra8_tls_session_cfg_t Value referencing the file-local ring.
 * @pre The file-local ring has static storage duration.
 * @pre The callback functions are linked into the test target.
 * @post Both BIO callback fields are nonnull.
 * @post The returned hostname remains valid for the test lifetime.
 * @note The helper does not reset or otherwise mutate the ring.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_tls_session_cfg_t internal_make_loopback_cfg(void);

/**
 * @brief Verify duplicate TLS global initialization fails closed.
 * @details Initializes once, checks the duplicate error, then deinitializes.
 * @pre TLS global state begins deinitialized.
 * @pre Unity accounting is ready.
 * @post The duplicate call reports the documented exists error.
 * @post TLS global state is deinitialized before return.
 * @note No session slot is opened by this vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_global_init_idempotent_failure(void);

/**
 * @brief Verify deinitialization rejects an inactive TLS module.
 * @details Calls the public deinit entry without a preceding initialization.
 * @pre TLS global state begins deinitialized.
 * @pre Unity accounting is ready.
 * @post The call reports not-initialized.
 * @post TLS global state remains deinitialized.
 * @note The vector owns no session or BIO storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_global_deinit_without_init(void);

/**
 * @brief Verify session-open argument validation.
 * @details Varies the output, config, and BIO callback pointers independently.
 * @pre TLS global state begins deinitialized.
 * @pre The loopback config helper is available.
 * @post Every malformed input reports invalid-argument.
 * @post TLS global state is deinitialized before return.
 * @note Rejected calls publish no live session handle.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_session_open_invalid_args(void);

/**
 * @brief Verify a session cannot open before global initialization.
 * @details Supplies a valid BIO config while leaving module state inactive.
 * @pre TLS global state begins deinitialized.
 * @pre The output session handle is writable.
 * @post The call reports not-initialized.
 * @post The output session handle remains null.
 * @note No global lifecycle call is issued by this vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_session_open_before_init(void);

/**
 * @brief Verify fixed TLS session-pool exhaustion and reuse.
 * @details Fills every slot, rejects one extra open, then proves reuse.
 * @pre TLS global state begins deinitialized.
 * @pre Local storage can retain every public session handle.
 * @post The cap-plus-one open reports no-memory.
 * @post All acquired sessions and global state are released.
 * @note This vector proves bounded capacity rather than allocation behavior.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_session_pool_exhaustion(void);

/**
 * @brief Verify session close rejects forged handles.
 * @details Exercises null and outside-pool pointer-shaped values.
 * @pre TLS global state begins deinitialized.
 * @pre The local bogus-storage object remains live during each call.
 * @post Both forged handles report invalid-argument.
 * @post TLS global state is deinitialized before return.
 * @note The vector never dereferences its forged public handle.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_session_close_invalid_handle(void);

/**
 * @brief Verify loopback handshake and record I/O.
 * @details Opens a session, handshakes, round-trips bytes, and checks zero I/O.
 * @pre TLS global state begins deinitialized.
 * @pre The loopback ring can hold the test payload.
 * @post Sent and received payload bytes match exactly.
 * @post Session and global state are released before return.
 * @note The off-target TLS path uses the injected ring BIO.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_loopback_handshake_and_io(void);

/**
 * @brief Verify send and receive argument guards.
 * @details Varies output-count, handle, and nonzero-buffer dependencies.
 * @pre TLS global state begins deinitialized.
 * @pre A valid loopback session can be opened.
 * @post Every malformed call reports invalid-argument.
 * @post Session and global state are released before return.
 * @note No rejected transfer changes the loopback payload.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_io_arg_validation(void);

/**
 * @brief Cover session-open and send compound decisions.
 * @details Applies the documented N-plus-one MC/DC vectors to both guards.
 * @pre TLS global state begins deinitialized.
 * @pre The loopback ring begins empty.
 * @post Each independent condition affects the expected decision outcome.
 * @post Session and global state are released before return.
 * @note Vector identities are documented beside the implementation body.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_tls(void);

/**
 * @brief Cover the receive null-buffer and nonzero-length decision.
 * @details Applies three vectors that independently establish both conditions.
 * @pre TLS global state begins deinitialized.
 * @pre The loopback ring begins empty.
 * @post Zero-length vectors succeed without reading bytes.
 * @post The nonzero null-buffer vector reports invalid-argument.
 * @note The live session is released after all vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_tls_recv_buf_len(void);

/**
 * @brief Cover lower and upper TLS handle bounds.
 * @details Compares one live pool handle with forged below/above values.
 * @pre TLS global state begins deinitialized.
 * @pre The loopback session config is valid.
 * @post Both forged values report invalid-argument.
 * @post The live handle closes and global state is released.
 * @note Forged pointers are compared only and never dereferenced.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_tls_handle_valid_bounds(void);

#endif /* RA8_TLS_TEST_CONTRACTS_H */

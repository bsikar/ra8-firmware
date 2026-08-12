/**
 * @file ra8_rsip_core.h
 * @brief Renesas Secure IP (RSIP-E50D) HAL -- core lifecycle / TRNG / hash API
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Core surface of the RA8D2 RSIP-E50D HAL split out of the
 * ``ra8_rsip.h`` umbrella. This sub-header owns the engine
 * lifecycle (init / deinit / power transition), the status /
 * IRQ mailbox plumbing, the 128-bit TRNG drain, the one-shot
 * SHA-256 primitive, and the incremental SHA-256 / HMAC-SHA-256
 * streaming contexts.
 *
 * The mailbox-driven peripheral is documented in HUM Ch 52
 * "Renesas Secure IP (RSIP-E50D)" p 3302-3307; cross-references
 * to the broader security feature set live in HUM Ch 51
 * "Security Features" p 3263-3301.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_rsip_config_t
 * @brief Initial configuration for ``ra8_rsip_init``.
 *
 * @details
 * The surface is intentionally tiny: the only knob is
 * whether the BIST should run on init. Production builds always
 * set ``run_bist = true``; bring-up firmware that wants to skip
 * the ~ms self-test on every reset can set it false at its own
 * risk. cppcheck cannot see tests/ so it flags the field as
 * unused; it is read in ``ra8_rsip_init`` in
 * ``libs/ra8_hal/src/ra8_rsip.c``.
 */
typedef struct {
  bool run_bist; /**< true -> arm BIST after MSTP release. */
} ra8_rsip_config_t;

/**
 * @typedef ra8_rsip_event_fn_t
 * @brief RSIP interrupt callback.
 *
 * @param[in] ctx Caller-provided context pointer.
 * @param[in] isr Snapshot of the ``k_ra8_rsip_off_isr`` word at
 * the moment the interrupt fired (see
 * ``k_ra8_rsip_mask_isr_*``).
 */
typedef void (*ra8_rsip_event_fn_t)(void* ctx, uint32_t isr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the RSIP engine and (optionally) run BIST.
 *
 * @details
 * Sequence:
 * 1. Release MSTPC31 via ``ra8_mstp_enable(k_ra8_mstp_rsip)``.
 * HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C"
 * p 446 + HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307.
 * 2. Soft-reset the engine via CTRL.RESET pulse.
 * 3. Set CTRL.ENABLE so the access-management circuit clocks the
 * internal subsystem.
 * 4. If ``cfg->run_bist`` is true, set CTRL.BIST and spin on
 * STATUS.BIST_OK. Returns ``k_ra8_err_hw_init_failed`` if the
 * spin exhausts its budget (BIST failure is a fatal condition
 * -- the engine MUST NOT be used).
 * 5. Clear pending ISR bits.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Engine ready, BIST passed.
 * @retval k_ra8_err_null_ptr ``cfg`` was nullptr.
 * @retval k_ra8_err_hw_init_failed BIST did not pass within budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 *
 * @post On success, the engine is clocked, BIST_OK is set, and
 * no spurious ISR bits are pending.
 * @post On success, MSTPC31 ref count is at least 1.
 *
 * @warning See file-level @warning -- TRNG output is invalid until
 * BIST passes. Do not call ``ra8_rsip_trng_read`` if this
 * function returned anything other than ``k_ra8_ok``.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * const ra8_rsip_config_t cfg = {.run_bist = true };
 * if (ra8_rsip_init(&cfg) != k_ra8_ok) { panic; }
 * @endcode
 *
 * @see ra8_rsip_deinit
 * @see ra8_rsip_trng_read
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_init(const ra8_rsip_config_t* cfg);

/**
 * @brief Power off the RSIP engine.
 *
 * @details
 * Clears CTRL.ENABLE, scrubs any pending ISR bits, releases the
 * shared callback slot, and asks ra8_mstp to gate MSTPC31.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Engine gated.
 * @retval k_ra8_err_invalid_state ra8_mstp had no matching enable.
 * @retval k_ra8_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre Engine is idle (``STATUS.BUSY = 0``); the HAL does not
 * enforce this -- callers must drain in-flight commands.
 * @pre ``ra8_rsip_init`` has been called at least once since reset.
 *
 * @post Engine is gated and the shared callback / context are
 * cleared.
 * @post MSTPC31 ref count is decremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_init
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_deinit(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Snapshot the STATUS mailbox word.
 *
 * @param[out] out Receives the raw STATUS word; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr ``out`` was nullptr.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 *
 * @post ``*out`` reflects the live STATUS word.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently with
 * data-path operations.
 *
 * @see ra8_rsip_clear_status
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_get_status(uint32_t* out);

/**
 * @brief Acknowledge ISR bits via write-1-to-clear.
 *
 * @details
 * Writes ``mask`` to the ISR register; only the bits listed in
 * ``k_ra8_rsip_mask_isr_*`` are valid. The write masks unrelated
 * bits before issuing.
 *
 * @param[in] mask OR of ``k_ra8_rsip_mask_isr_*`` values.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bits cleared.
 * @retval k_ra8_err_invalid_arg ``mask`` had bits outside the
 * ISR field.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre ``mask`` has at least one valid bit set.
 *
 * @post Requested ISR bits read as 0.
 * @post No CTRL bits are touched.
 *
 * @note Thread safety: not thread-safe with concurrent dispatch.
 *
 * @see ra8_rsip_attach_handler
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_clear_status(uint32_t mask);

/**
 * @brief Attach a single shared interrupt callback.
 *
 * @details
 * The RSIP routes a small number of edge-triggered events
 * (DONE / RND / HASH / TAMPER) into one peripheral IRQ line. The
 * driver fans them out by passing the live ISR snapshot to a
 * single callback.
 *
 * @param[in] fn Callback fired by ``ra8_rsip_dispatch``; may be
 * NULL to detach.
 * @param[in] ctx Opaque context forwarded to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Callback installed.
 *
 * @pre IRQs masked while swapping callbacks.
 * @pre Caller owns the lifetime of ``ctx``.
 *
 * @post ``fn`` / ``ctx`` are visible to ``ra8_rsip_dispatch``.
 * @post Previous callback (if any) is no longer invoked.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_dispatch
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_attach_handler(ra8_rsip_event_fn_t fn, void* ctx);

/**
 * @brief Run the attached callback with the current ISR snapshot.
 *
 * @details
 * Called from the secure-world ICU dispatch when the RSIP IRQ
 * fires. Reads the live ISR word, fires the registered callback
 * (if any), and acks the bits the callback observed via
 * ``ra8_rsip_clear_status``.
 *
 * @pre Caller is the IRQ glue layer; runs in IRQ context.
 * @pre ``ra8_rsip_init`` has been called at least once since reset.
 *
 * @post Pending ISR bits that were visible to the callback are
 * cleared.
 * @post No CTRL bits are modified.
 *
 * @note Thread safety: must run with IRQ priority above the RSIP
 * line (default for ICU dispatch). See HUM Ch 39 "Renesas Secure
 * IP (RSIP-E51A)" pp 1839-1859.
 *
 * @see ra8_rsip_attach_handler
 *
 * @since 0.1.0
 */
void ra8_rsip_dispatch(void);

/* =============================================================================
 * TRNG
 * =============================================================================
 */

/**
 * @brief Drain ``len`` bytes from the RSIP true RNG -- fail-closed, no backend.
 *
 * @details
 * The RSIP-E50D TRNG has no documented register interface (HUM Ch 52 is a
 * 6-page feature overview with no register map), so the hand-written ``RND_*``
 * access is invented and non-functional on silicon: ``RND_STATUS.READY`` never
 * asserts. This routine therefore fails closed with ``k_ra8_err_not_supported``
 * rather than spin to a timeout or hand back a deterministic / all-zero value --
 * predictable "entropy" is far more dangerous than an honest error. A working
 * TRNG needs an FSP-derived RSIP primitive sequence; the register path is
 * retained behind ``RA8_RSIP_TRNG_HARDWARE`` (never defined) for a future port.
 * A software PRNG is NOT a substitute here.
 *
 * @param[out] buf Destination buffer (>= ``len`` bytes); never NULL.
 * @param[in] len Bytes to fetch; must be a non-zero multiple of 4.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_err_not_supported No working RSIP TRNG register interface exists
 * (the shipped, fail-closed path).
 * @retval k_ra8_err_null_ptr ``buf`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``len`` is zero or not a multiple
 * of ``k_ra8_rsip_trng_word_bytes``.
 *
 * @pre ``buf`` is non-NULL.
 * @pre ``len`` is a non-zero multiple of ``k_ra8_rsip_trng_word_bytes``.
 *
 * @post ``buf`` is left untouched; the caller must treat it as uninitialized.
 * @post No RSIP register is mutated in the fail-closed build.
 *
 * @warning Returns no entropy on this silicon. Do NOT use for key or nonce
 * generation until a real TRNG backend exists.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * uint8_t  seed[32];
 * ra8_err_t e = ra8_rsip_trng_read(seed, sizeof(seed)); // e == k_ra8_err_not_supported
 * @endcode
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_trng_read(uint8_t* buf, uint32_t len);

/* =============================================================================
 * SHA-256
 * =============================================================================
 */

/**
 * @brief Compute SHA-256 of an in-memory buffer.
 *
 * @details
 * Streams ``msg`` through the HASH engine 32 bits at a time, then
 * reads back the 8-word (32-byte) digest from
 * ``HASH_DIGEST``. Algorithm select is hard-wired to
 * ``k_ra8_rsip_hash_sha256``. Suitable for short messages;
 * incremental update / final API is deferred to a future revision.
 *
 * @param[in] msg Pointer to the input buffer; never NULL.
 * @param[in] msg_len Length of ``msg`` in bytes; may be 0.
 * @param[out] digest 32-byte output buffer; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Digest written.
 * @retval k_ra8_err_null_ptr ``msg`` or ``digest`` was nullptr.
 * @retval k_ra8_err_hw_timeout HASH engine did not signal done.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre ``digest`` is at least ``k_ra8_rsip_sha256_digest_bytes`` wide.
 *
 * @post On success, ``digest[0..31]`` holds the SHA-256 of ``msg``.
 * @post HASH_STATUS.DONE has been observed and acked.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * uint8_t out[k_ra8_rsip_sha256_digest_bytes];
 * (void)ra8_rsip_sha256(buf, len, out);
 * @endcode
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest);

/* =============================================================================
 * SHA-256 incremental (init / update / final)
 * =============================================================================
 */

/**
 * @enum ra8_rsip_sha_buf_t
 * @brief Capacity caps for the incremental SHA / HMAC streaming buffers.
 *
 * @details
 * The incremental SHA / HMAC API buffers all input, then runs a single
 * software SHA-256 pass (``internal_sha256_dispatch``) at ``final()``.
 * There is no RSIP hash-hardware backend on this silicon: HUM Ch 52 is
 * a feature overview with no hash register map (issue #215). The buffer
 * cap is sized for TLS 1.2 / 1.3 handshake transcripts.
 */
typedef enum : uint16_t {
  k_ra8_rsip_inc_buf_bytes  = 8192U, /**< Streaming buffer for incremental hash. */
  k_ra8_rsip_sha256_block   = 64U,   /**< SHA-256 message-block byte length.     */
  k_ra8_rsip_hmac_inner_pad = 0x36U, /**< RFC 2104 inner-pad fill byte.          */
  k_ra8_rsip_hmac_outer_pad = 0x5CU, /**< RFC 2104 outer-pad fill byte.          */
} ra8_rsip_sha_buf_t;

/**
 * @struct ra8_rsip_sha256_ctx_t
 * @brief Streaming state for incremental SHA-256.
 *
 * @invariant ``used`` <= ``k_ra8_rsip_inc_buf_bytes``.
 * @since 0.1.0
 */
typedef struct {
  uint8_t  buf[k_ra8_rsip_inc_buf_bytes]; /**< Accumulated message bytes.  */
  uint32_t used;                          /**< Bytes currently in ``buf``. */
  uint8_t  initialized;                   /**< 1 = ready, 0 = unset.       */
} ra8_rsip_sha256_ctx_t;

/**
 * @brief Initialise a streaming SHA-256 context.
 *
 * @param[out] ctx Streaming state; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Context ready for ``update()``.
 * @retval k_ra8_err_null_ptr      ``ctx`` was nullptr.
 *
 * @pre ``ra8_rsip_init`` has been called at least once since reset.
 * @pre ``ctx`` is non-NULL.
 *
 * @post ``ctx->used == 0`` and ``ctx->initialized == 1``.
 * @post No engine state is modified.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * ra8_rsip_sha256_ctx_t ctx;
 * (void)ra8_rsip_sha256_init(&ctx);
 * (void)ra8_rsip_sha256_update(&ctx, hello, hello_len);
 * uint8_t transcript[k_ra8_rsip_sha256_digest_bytes];
 * (void)ra8_rsip_sha256_final(&ctx, transcript);
 * @endcode
 *
 * @see ra8_rsip_sha256_update
 * @see ra8_rsip_sha256_final
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_sha256_init(ra8_rsip_sha256_ctx_t* ctx);

/**
 * @brief Absorb additional bytes into a streaming SHA-256 context.
 *
 * @param[in,out] ctx  Context primed by ``ra8_rsip_sha256_init``.
 * @param[in]     data Bytes to absorb; may be NULL only if ``len`` is 0.
 * @param[in]     len  Number of bytes in ``data``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bytes accumulated.
 * @retval k_ra8_err_null_ptr      ``ctx`` was nullptr or ``data`` was NULL with non-zero ``len``.
 * @retval k_ra8_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra8_err_invalid_arg   Cumulative input exceeds ``k_ra8_rsip_inc_buf_bytes``.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre Either ``len == 0`` or ``data`` is non-NULL.
 *
 * @post On success ``ctx->used`` grew by ``len``.
 * @post On failure ``ctx`` is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_sha256_update(ra8_rsip_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);

/**
 * @brief Emit the digest of a streaming SHA-256 context.
 *
 * @param[in,out] ctx        Context populated by prior ``update()`` calls.
 * @param[out]    digest_out 32-byte digest output buffer; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Digest written.
 * @retval k_ra8_err_null_ptr      ``ctx`` or ``digest_out`` was nullptr.
 * @retval k_ra8_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra8_err_hw_timeout    Underlying ``ra8_rsip_sha256`` timed out.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre ``digest_out`` is non-NULL.
 *
 * @post On success ``digest_out[0..31]`` holds the SHA-256.
 * @post On any return ``ctx->initialized == 0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_sha256_final(ra8_rsip_sha256_ctx_t* ctx, uint8_t* digest_out);

/* =============================================================================
 * HMAC-SHA-256 incremental (init / update / final)
 * =============================================================================
 */

/**
 * @struct ra8_rsip_hmac_sha256_ctx_t
 * @brief Streaming state for incremental HMAC-SHA-256.
 *
 * @details
 * Implements HMAC-SHA-256 (RFC 2104) on top of the streaming SHA-256
 * primitive. The inner SHA is built incrementally; the outer SHA is
 * issued at ``final()``.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_rsip_sha256_ctx_t inner;                              /**< Running inner-hash state.   */
  uint8_t               key_block[k_ra8_rsip_sha256_block]; /**< Prepared 64-byte key block. */
  uint8_t               initialized;                        /**< 1 = ready, 0 = unset.       */
} ra8_rsip_hmac_sha256_ctx_t;

/**
 * @brief Initialise a streaming HMAC-SHA-256 context.
 *
 * @param[out] ctx     Streaming HMAC state; never NULL.
 * @param[in]  key     HMAC key bytes; never NULL when ``key_len`` > 0.
 * @param[in]  key_len Length of ``key`` in bytes (may be 0).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Context ready.
 * @retval k_ra8_err_null_ptr      ``ctx`` was nullptr or ``key`` was NULL with non-zero ``key_len``.
 * @retval k_ra8_err_hw_timeout    Internal SHA collapse of an oversized key timed out.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre ``ctx`` is non-NULL.
 *
 * @post ``ctx->initialized == 1``.
 * @post No engine state is modified beyond the optional key-collapse.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_hmac_sha256_update
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_hmac_sha256_init(ra8_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* key, uint32_t key_len);

/**
 * @brief Absorb additional bytes into a streaming HMAC-SHA-256 context.
 *
 * @param[in,out] ctx  Context primed by ``ra8_rsip_hmac_sha256_init``.
 * @param[in]     data Bytes to absorb; may be NULL only if ``len`` is 0.
 * @param[in]     len  Number of bytes in ``data``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bytes accumulated.
 * @retval k_ra8_err_null_ptr      ``ctx`` was nullptr or ``data`` NULL with non-zero ``len``.
 * @retval k_ra8_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra8_err_invalid_arg   Cumulative input exceeds the buffer cap.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre Either ``len == 0`` or ``data`` is non-NULL.
 *
 * @post On success ``ctx->inner.used`` grew by ``len``.
 * @post On failure ``ctx`` is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_hmac_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_hmac_sha256_update(ra8_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);

/**
 * @brief Emit the MAC of a streaming HMAC-SHA-256 context.
 *
 * @param[in,out] ctx     Context populated by prior ``update()`` calls.
 * @param[out]    mac_out 32-byte MAC output buffer; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                MAC written.
 * @retval k_ra8_err_null_ptr      ``ctx`` or ``mac_out`` was nullptr.
 * @retval k_ra8_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra8_err_hw_timeout    Either SHA pass timed out.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre ``mac_out`` is non-NULL.
 *
 * @post On success ``mac_out[0..31]`` is the HMAC-SHA-256.
 * @post On any return ``ctx->initialized == 0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_hmac_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_hmac_sha256_final(ra8_rsip_hmac_sha256_ctx_t* ctx,
                                                   uint8_t*                    mac_out);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Park the engine for software-standby entry.
 *
 * @details
 * HUM Ch 52.3.1 p 3307 ("Software Standby Mode") requires the
 * engine to be idle before software-standby; this helper clears
 * CTRL.ENABLE then gates MSTPC31.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Engine parked.
 * @retval k_ra8_err_invalid_state ra8_mstp had no matching enable.
 * @retval k_ra8_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre Engine is idle (caller's responsibility; HUM 52.3.1).
 * @pre ``ra8_rsip_init`` has been called.
 *
 * @post CTRL.ENABLE = 0.
 * @post MSTPC31 ref count is decremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_exit_stop
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_enter_stop(void);

/**
 * @brief Re-enable the engine after software-standby exit.
 *
 * @details
 * Calls ``ra8_mstp_enable`` to clock the block, sets CTRL.ENABLE,
 * and re-runs BIST so a side-channel injection during standby
 * does not silently leak.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Engine ready, BIST passed.
 * @retval k_ra8_err_hw_init_failed BIST did not pass.
 * @retval k_ra8_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre ``ra8_rsip_enter_stop`` was the previous transition.
 * @pre IRQs masked.
 *
 * @post CTRL.ENABLE = 1 and STATUS.BIST_OK = 1.
 * @post MSTPC31 ref count is incremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_enter_stop
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_exit_stop(void);

#ifdef __cplusplus
}
#endif

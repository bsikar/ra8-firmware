/**
 * @file ra_rsip.h
 * @brief Renesas Secure IP (RSIP-E50D) HAL driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Minimum-viable HAL surface for the RA8D2 RSIP-E50D security
 * engine. The engine is a mailbox-driven peripheral (HUM Ch 52
 * "Renesas Secure IP (RSIP-E50D)" p 3302-3307); cross-references
 * to the broader security feature set live in HUM Ch 51
 * "Security Features" p 3263-3301.
 *
 * The driver owns:
 *
 * - lifecycle: power on the engine via MSTPCRC.MSTPC31 and run the
 * built-in self-test (BIST) before any cryptographic operation;
 * - 128-bit TRNG read in 32-byte chunks;
 * - in-memory SHA-256 of a single contiguous buffer;
 * - status / IRQ enable / IRQ ack;
 * - power transition (enter / exit module-stop).
 *
 * The NSC veneer ``libs/ra_nsc/src/ra_nsc_key_vault.c`` calls into
 * the secure-side key vault layer (``src/secure_app/key_vault.c``)
 * which in turn drives this HAL; the veneer is unaware of the
 * register window and that mapping must remain stable.
 *
 * @warning The TRNG MUST pass its self-test (BIST) before any data
 * read. ``ra_rsip_init`` runs the BIST on the way up;
 * callers that hand-build init sequences MUST replicate
 * this. Reading the TRNG before BIST returns "OK" yields
 * biased / predictable output and silently undermines
 * every key derived from it.
 *
 * ## Round-3 coverage (HUM Ch 51 + Ch 52, p 3263-3307)
 *
 * The round-3 expansion brings every documented sub-block in the
 * RSIP-E50D mailbox layer behind a HAL entry point:
 *
 * - **Symmetric key install** (FSP ``r_rsip_key_injection.c``)
 * -- plaintext and OEM (PE5/PE6) flows for AES-128/192/256 and
 * ChaCha20.
 * - **AES block / authenticated modes** -- ECB, CBC, CTR, GCM,
 * CCM, XTS, CMAC, GMAC for both encrypt and decrypt.
 * - **ChaCha20 + Poly1305** -- stream + AEAD modes; standalone
 * Poly1305 MAC.
 * - **Asymmetric crypto** -- RSA-2048/3072/4096 sign/verify and
 * ECDSA / ECDH on every supported curve.
 * - **Hash family** -- SHA-2 (224/256/384/512/512-224/512-256),
 * SHA-3 (224/256/384/512), SHAKE-128/256, and HMAC over each.
 * - **OEM boot loader version** -- read, increment (W1) and lock.
 * - **Wrapped-key vault** -- per-slot read/write/erase + populated
 * slot count.
 * - **Key wrap / unwrap** -- KEK-handle backed wrap engine.
 * - **Key derivation** -- HKDF-SHA-256/384/512 + HUK/UID-bound KDF.
 * - **Device lifecycle** -- read + advance helpers across the six
 * states (CM/SSD/NSECSD/DPL/LCM/RMA).
 * - **Debug authorisation level** -- AL0/AL1/AL2 read+write.
 * - **Tamper subsystem** -- per-source enable, status read, ack,
 * and SPA/DPA arm.
 * - **DOTF key route** -- DOTF0 / DOTF1 enable + slot.
 *
 * Every key-touching API takes opaque ``ra_rsip_key_handle_t``
 * blobs -- raw key bytes never leave secure RAM. Each such API is
 * marked ``[[nodiscard]]`` so a forgotten error check is a build
 * failure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_rsip_regs.h"
#include "ra_err.h"

/**
 * @struct ra_rsip_config_t
 * @brief Initial configuration for ``ra_rsip_init``.
 *
 * @details
 * The surface is intentionally tiny: the only knob is
 * whether the BIST should run on init. Production builds always
 * set ``run_bist = true``; bring-up firmware that wants to skip
 * the ~ms self-test on every reset can set it false at its own
 * risk. cppcheck cannot see tests/ so it flags the field as
 * unused; it is read in ``ra_rsip_init`` in
 * ``libs/ra_hal/src/ra_rsip.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool run_bist; /**< true -> arm BIST after MSTP release. */
} ra_rsip_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_rsip_event_fn_t
 * @brief RSIP interrupt callback.
 *
 * @param[in] ctx Caller-provided context pointer.
 * @param[in] isr Snapshot of the ``k_ra_rsip_off_isr`` word at
 * the moment the interrupt fired (see
 * ``k_ra_rsip_mask_isr_*``).
 */
typedef void (*ra_rsip_event_fn_t)(void* ctx, uint32_t isr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the RSIP engine and (optionally) run BIST.
 *
 * @details
 * Sequence:
 * 1. Release MSTPC31 via ``ra_mstp_enable(k_ra_mstp_rsip)``.
 * HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C"
 * p 446 + HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307.
 * 2. Soft-reset the engine via CTRL.RESET pulse.
 * 3. Set CTRL.ENABLE so the access-management circuit clocks the
 * internal subsystem.
 * 4. If ``cfg->run_bist`` is true, set CTRL.BIST and spin on
 * STATUS.BIST_OK. Returns ``k_ra_err_hw_init_failed`` if the
 * spin exhausts its budget (BIST failure is a fatal condition
 * -- the engine MUST NOT be used).
 * 5. Clear pending ISR bits.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine ready, BIST passed.
 * @retval k_ra_err_null_ptr ``cfg`` was nullptr.
 * @retval k_ra_err_hw_init_failed BIST did not pass within budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 *
 * @post On success, the engine is clocked, BIST_OK is set, and
 * no spurious ISR bits are pending.
 * @post On success, MSTPC31 ref count is at least 1.
 *
 * @warning See file-level @warning -- TRNG output is invalid until
 * BIST passes. Do not call ``ra_rsip_trng_read`` if this
 * function returned anything other than ``k_ra_ok``.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * const ra_rsip_config_t cfg = {.run_bist = true };
 * if (ra_rsip_init(&cfg) != k_ra_ok) { panic; }
 * @endcode
 *
 * @see ra_rsip_deinit
 * @see ra_rsip_trng_read
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_init(const ra_rsip_config_t* cfg);

/**
 * @brief Power off the RSIP engine.
 *
 * @details
 * Clears CTRL.ENABLE, scrubs any pending ISR bits, releases the
 * shared callback slot, and asks ra_mstp to gate MSTPC31.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine gated.
 * @retval k_ra_err_invalid_state ra_mstp had no matching enable.
 * @retval k_ra_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre Engine is idle (``STATUS.BUSY = 0``); the HAL does not
 * enforce this -- callers must drain in-flight commands.
 * @pre ``ra_rsip_init`` has been called at least once since reset.
 *
 * @post Engine is gated and the shared callback / context are
 * cleared.
 * @post MSTPC31 ref count is decremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_rsip_init
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_deinit(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Snapshot the STATUS mailbox word.
 *
 * @param[out] out Receives the raw STATUS word; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr ``out`` was nullptr.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 *
 * @post ``*out`` reflects the live STATUS word.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently with
 * data-path operations.
 *
 * @see ra_rsip_clear_status
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_get_status(uint32_t* out);

/**
 * @brief Acknowledge ISR bits via write-1-to-clear.
 *
 * @details
 * Writes ``mask`` to the ISR register; only the bits listed in
 * ``k_ra_rsip_mask_isr_*`` are valid. The write masks unrelated
 * bits before issuing.
 *
 * @param[in] mask OR of ``k_ra_rsip_mask_isr_*`` values.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bits cleared.
 * @retval k_ra_err_invalid_arg ``mask`` had bits outside the
 * ISR field.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre ``mask`` has at least one valid bit set.
 *
 * @post Requested ISR bits read as 0.
 * @post No CTRL bits are touched.
 *
 * @note Thread safety: not thread-safe with concurrent dispatch.
 *
 * @see ra_rsip_attach_handler
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_clear_status(uint32_t mask);

/**
 * @brief Attach a single shared interrupt callback.
 *
 * @details
 * The RSIP routes a small number of edge-triggered events
 * (DONE / RND / HASH / TAMPER) into one peripheral IRQ line. The
 * driver fans them out by passing the live ISR snapshot to a
 * single callback.
 *
 * @param[in] fn Callback fired by ``ra_rsip_dispatch``; may be
 * NULL to detach.
 * @param[in] ctx Opaque context forwarded to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Callback installed.
 *
 * @pre IRQs masked while swapping callbacks.
 * @pre Caller owns the lifetime of ``ctx``.
 *
 * @post ``fn`` / ``ctx`` are visible to ``ra_rsip_dispatch``.
 * @post Previous callback (if any) is no longer invoked.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_rsip_dispatch
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_attach_handler(ra_rsip_event_fn_t fn, void* ctx);

/**
 * @brief Run the attached callback with the current ISR snapshot.
 *
 * @details
 * Called from the secure-world ICU dispatch when the RSIP IRQ
 * fires. Reads the live ISR word, fires the registered callback
 * (if any), and acks the bits the callback observed via
 * ``ra_rsip_clear_status``.
 *
 * @pre Caller is the IRQ glue layer; runs in IRQ context.
 * @pre ``ra_rsip_init`` has been called at least once since reset.
 *
 * @post Pending ISR bits that were visible to the callback are
 * cleared.
 * @post No CTRL bits are modified.
 *
 * @note Thread safety: must run with IRQ priority above the RSIP
 * line (default for ICU dispatch). See HUM Ch 39 "Renesas Secure
 * IP (RSIP-E51A)" pp 1839-1859.
 *
 * @see ra_rsip_attach_handler
 *
 * @since 0.1.0
 */
void ra_rsip_dispatch(void);

/* =============================================================================
 * TRNG
 * =============================================================================
 */

/**
 * @brief Drain ``len`` bytes from the 128-bit true RNG.
 *
 * @details
 * Reads from ``RND_DATA`` 32 bits at a time, polling
 * ``RND_STATUS.READY`` between fetches. ``len`` must be a multiple
 * of 4 -- partial-word reads are not supported because the
 * underlying engine produces 32-bit lanes.
 *
 * @param[out] buf Destination buffer (>= ``len`` bytes); never NULL.
 * @param[in] len Bytes to fetch; must be a non-zero multiple of 4.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer filled.
 * @retval k_ra_err_null_ptr ``buf`` was nullptr.
 * @retval k_ra_err_invalid_arg ``len`` is zero or not a multiple
 * of ``k_ra_rsip_trng_word_bytes``.
 * @retval k_ra_err_hw_timeout RND_STATUS.READY did not assert.
 *
 * @pre ``ra_rsip_init(..., run_bist=true)`` returned ``k_ra_ok``.
 * @pre ``buf`` is non-NULL.
 *
 * @post On success, ``buf[0..len-1]`` holds fresh random bytes.
 * @post On failure, the caller must treat ``buf`` as uninitialized.
 *
 * @warning See file-level @warning -- the BIST gate is the only
 * safety check between this routine and biased output.
 *
 * @note Thread safety: not thread-safe; the engine has a single
 * output FIFO.
 *
 * @code{.c}
 * uint8_t seed[32];
 * (void)ra_rsip_trng_read(seed, sizeof(seed));
 * @endcode
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_trng_read(uint8_t* buf, uint32_t len);

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
 * ``k_ra_rsip_hash_sha256``. Suitable for short messages;
 * incremental update / final API is deferred to a future revision.
 *
 * @param[in] msg Pointer to the input buffer; never NULL.
 * @param[in] msg_len Length of ``msg`` in bytes; may be 0.
 * @param[out] digest 32-byte output buffer; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Digest written.
 * @retval k_ra_err_null_ptr ``msg`` or ``digest`` was nullptr.
 * @retval k_ra_err_hw_timeout HASH engine did not signal done.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre ``digest`` is at least ``k_ra_rsip_sha256_digest_bytes`` wide.
 *
 * @post On success, ``digest[0..31]`` holds the SHA-256 of ``msg``.
 * @post HASH_STATUS.DONE has been observed and acked.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * uint8_t out[k_ra_rsip_sha256_digest_bytes];
 * (void)ra_rsip_sha256(buf, len, out);
 * @endcode
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest);

/* =============================================================================
 * SHA-256 incremental (init / update / final)
 * =============================================================================
 */

/**
 * @enum ra_rsip_sha_buf_t
 * @brief Capacity caps for the incremental SHA / HMAC streaming buffers.
 *
 * @details
 * The RSIP-E50D hash unit accepts a single contiguous buffer per
 * operation (HUM Ch 52.2.3 "Hash Generator" p 3306). The incremental
 * API therefore buffers all input, then issues a single hardware hash
 * at ``final()``. The buffer cap is sized for TLS 1.2 / 1.3 handshake
 * transcripts.
 */
typedef enum : uint16_t {
  k_ra_rsip_inc_buf_bytes  = 8192U, /**< Streaming buffer for incremental hash. */
  k_ra_rsip_sha256_block   = 64U,   /**< SHA-256 message-block byte length.    */
  k_ra_rsip_hmac_inner_pad = 0x36U, /**< RFC 2104 inner-pad fill byte.         */
  k_ra_rsip_hmac_outer_pad = 0x5CU, /**< RFC 2104 outer-pad fill byte.         */
} ra_rsip_sha_buf_t;

/**
 * @struct ra_rsip_sha256_ctx_t
 * @brief Streaming state for incremental SHA-256.
 *
 * @invariant ``used`` <= ``k_ra_rsip_inc_buf_bytes``.
 * @since 0.1.0
 */
typedef struct {
  uint8_t  buf[k_ra_rsip_inc_buf_bytes]; /**< Accumulated message bytes.   */
  uint32_t used;                         /**< Bytes currently in ``buf``.  */
  uint8_t  initialized;                  /**< 1 = ready, 0 = unset.        */
} ra_rsip_sha256_ctx_t;

/**
 * @brief Initialise a streaming SHA-256 context.
 *
 * @param[out] ctx Streaming state; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Context ready for ``update()``.
 * @retval k_ra_err_null_ptr      ``ctx`` was nullptr.
 *
 * @pre ``ra_rsip_init`` has been called at least once since reset.
 * @pre ``ctx`` is non-NULL.
 *
 * @post ``ctx->used == 0`` and ``ctx->initialized == 1``.
 * @post No engine state is modified.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * ra_rsip_sha256_ctx_t ctx;
 * (void)ra_rsip_sha256_init(&ctx);
 * (void)ra_rsip_sha256_update(&ctx, hello, hello_len);
 * uint8_t transcript[k_ra_rsip_sha256_digest_bytes];
 * (void)ra_rsip_sha256_final(&ctx, transcript);
 * @endcode
 *
 * @see ra_rsip_sha256_update
 * @see ra_rsip_sha256_final
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_sha256_init(ra_rsip_sha256_ctx_t* ctx);

/**
 * @brief Absorb additional bytes into a streaming SHA-256 context.
 *
 * @param[in,out] ctx  Context primed by ``ra_rsip_sha256_init``.
 * @param[in]     data Bytes to absorb; may be NULL only if ``len`` is 0.
 * @param[in]     len  Number of bytes in ``data``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Bytes accumulated.
 * @retval k_ra_err_null_ptr      ``ctx`` was nullptr or ``data`` was NULL with non-zero ``len``.
 * @retval k_ra_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra_err_invalid_arg   Cumulative input exceeds ``k_ra_rsip_inc_buf_bytes``.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre Either ``len == 0`` or ``data`` is non-NULL.
 *
 * @post On success ``ctx->used`` grew by ``len``.
 * @post On failure ``ctx`` is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_rsip_sha256_update(ra_rsip_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);

/**
 * @brief Emit the digest of a streaming SHA-256 context.
 *
 * @param[in,out] ctx        Context populated by prior ``update()`` calls.
 * @param[out]    digest_out 32-byte digest output buffer; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Digest written.
 * @retval k_ra_err_null_ptr      ``ctx`` or ``digest_out`` was nullptr.
 * @retval k_ra_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra_err_hw_timeout    Underlying ``ra_rsip_sha256`` timed out.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre ``digest_out`` is non-NULL.
 *
 * @post On success ``digest_out[0..31]`` holds the SHA-256.
 * @post On any return ``ctx->initialized == 0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_sha256_final(ra_rsip_sha256_ctx_t* ctx, uint8_t* digest_out);

/* =============================================================================
 * HMAC-SHA-256 incremental (init / update / final)
 * =============================================================================
 */

/**
 * @struct ra_rsip_hmac_sha256_ctx_t
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
  ra_rsip_sha256_ctx_t inner;                             /**< Running inner-hash state.   */
  uint8_t              key_block[k_ra_rsip_sha256_block]; /**< Prepared 64-byte key block. */
  uint8_t              initialized;                       /**< 1 = ready, 0 = unset.       */
} ra_rsip_hmac_sha256_ctx_t;

/**
 * @brief Initialise a streaming HMAC-SHA-256 context.
 *
 * @param[out] ctx     Streaming HMAC state; never NULL.
 * @param[in]  key     HMAC key bytes; never NULL when ``key_len`` > 0.
 * @param[in]  key_len Length of ``key`` in bytes (may be 0).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Context ready.
 * @retval k_ra_err_null_ptr      ``ctx`` was nullptr or ``key`` was NULL with non-zero ``key_len``.
 * @retval k_ra_err_hw_timeout    Internal SHA collapse of an oversized key timed out.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre ``ctx`` is non-NULL.
 *
 * @post ``ctx->initialized == 1``.
 * @post No engine state is modified beyond the optional key-collapse.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_hmac_sha256_update
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_rsip_hmac_sha256_init(ra_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* key, uint32_t key_len);

/**
 * @brief Absorb additional bytes into a streaming HMAC-SHA-256 context.
 *
 * @param[in,out] ctx  Context primed by ``ra_rsip_hmac_sha256_init``.
 * @param[in]     data Bytes to absorb; may be NULL only if ``len`` is 0.
 * @param[in]     len  Number of bytes in ``data``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Bytes accumulated.
 * @retval k_ra_err_null_ptr      ``ctx`` was nullptr or ``data`` NULL with non-zero ``len``.
 * @retval k_ra_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra_err_invalid_arg   Cumulative input exceeds the buffer cap.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre Either ``len == 0`` or ``data`` is non-NULL.
 *
 * @post On success ``ctx->inner.used`` grew by ``len``.
 * @post On failure ``ctx`` is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_hmac_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_rsip_hmac_sha256_update(ra_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);

/**
 * @brief Emit the MAC of a streaming HMAC-SHA-256 context.
 *
 * @param[in,out] ctx     Context populated by prior ``update()`` calls.
 * @param[out]    mac_out 32-byte MAC output buffer; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                MAC written.
 * @retval k_ra_err_null_ptr      ``ctx`` or ``mac_out`` was nullptr.
 * @retval k_ra_err_invalid_state ``ctx`` was not initialized.
 * @retval k_ra_err_hw_timeout    Either SHA pass timed out.
 *
 * @pre ``ctx->initialized == 1``.
 * @pre ``mac_out`` is non-NULL.
 *
 * @post On success ``mac_out[0..31]`` is the HMAC-SHA-256.
 * @post On any return ``ctx->initialized == 0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_hmac_sha256_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_hmac_sha256_final(ra_rsip_hmac_sha256_ctx_t* ctx, uint8_t* mac_out);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine parked.
 * @retval k_ra_err_invalid_state ra_mstp had no matching enable.
 * @retval k_ra_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre Engine is idle (caller's responsibility; HUM 52.3.1).
 * @pre ``ra_rsip_init`` has been called.
 *
 * @post CTRL.ENABLE = 0.
 * @post MSTPC31 ref count is decremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_rsip_exit_stop
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_enter_stop(void);

/**
 * @brief Re-enable the engine after software-standby exit.
 *
 * @details
 * Calls ``ra_mstp_enable`` to clock the block, sets CTRL.ENABLE,
 * and re-runs BIST so a side-channel injection during standby
 * does not silently leak.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine ready, BIST passed.
 * @retval k_ra_err_hw_init_failed BIST did not pass.
 * @retval k_ra_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre ``ra_rsip_enter_stop`` was the previous transition.
 * @pre IRQs masked.
 *
 * @post CTRL.ENABLE = 1 and STATUS.BIST_OK = 1.
 * @post MSTPC31 ref count is incremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_rsip_enter_stop
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_exit_stop(void);

/* =============================================================================
 * Wrapped-key handle type (opaque to callers)
 * =============================================================================
 */

/**
 * @struct ra_rsip_key_handle_t
 * @brief Opaque wrapped-key handle.
 *
 * @details
 * The HAL never exposes raw key bytes; every key the engine touches
 * lives behind a wrapped blob. The handle carries the algorithm
 * selector + the body word count so the engine can route the body
 * through the correct command FIFO (FSP ``rsip_wrapped_key_t``).
 *
 * The blob storage is sized to the largest wrapped key the engine
 * accepts (RSA-4096 private = 260 body words + 1 alg word).
 *
 * @invariant ``alg`` is one of ``ra_rsip_oem_cmd_t``.
 * @invariant ``body_words <= k_ra_rsip_handle_words_rsa4096_priv``.
 *
 * @note Handles are populated by ``ra_rsip_aes_*_install_*``,
 * ``ra_rsip_rsa_*_install_*`` etc. and consumed by every
 * cipher / signing / KDF API.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t alg;                                       /**< OEM-cmd algorithm selector. */
  uint32_t body_words;                                /**< Number of body words (1..261). */
  uint32_t body[k_ra_rsip_handle_words_rsa4096_priv]; /**< Wrapped body. */
} ra_rsip_key_handle_t;

/* =============================================================================
 * Key install -- plaintext (development) flow
 * =============================================================================
 */

/**
 * @brief Wrap a 16-byte AES-128 key for use by the engine.
 *
 * @details
 * Streams the plaintext key through the OEM key-install primitive
 * (FSP ``R_RSIP_AES128_InitialKeyWrap`` p ``r_rsip_key_injection.c``)
 * and returns the wrapped handle. The plaintext bytes are pushed
 * directly into the engine input FIFO and never copied into a
 * static buffer.
 *
 * @param[in] key Plaintext AES-128 key (16 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 16-byte readable.
 * @pre ``out`` is non-NULL.
 * @post On success, ``out->alg == k_ra_rsip_oem_cmd_aes128``.
 * @post On success, ``out->body_words == k_ra_rsip_handle_words_aes128``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_aes192_install_plain
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes128_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out);

/**
 * @brief Wrap a 24-byte AES-192 key (see ``ra_rsip_aes128_install_plain``).
 *
 * @param[in] key Plaintext AES-192 key (24 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 24-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra_rsip_oem_cmd_aes192``.
 * @post ``out->body_words == k_ra_rsip_handle_words_aes192``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes192_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out);

/**
 * @brief Wrap a 32-byte AES-256 key (see ``ra_rsip_aes128_install_plain``).
 *
 * @param[in] key Plaintext AES-256 key (32 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 32-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra_rsip_oem_cmd_aes256``.
 * @post ``out->body_words == k_ra_rsip_handle_words_aes256``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes256_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out);

/**
 * @brief Wrap a 32-byte ChaCha20 key.
 *
 * @param[in] key Plaintext ChaCha20 key (32 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 32-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra_rsip_oem_cmd_chacha20``.
 * @post ``out->body_words == k_ra_rsip_handle_words_chacha20``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_chacha20_install_plain(const uint8_t*        key,
                                                      ra_rsip_key_handle_t* out);

/**
 * @brief Wrap an HMAC key for use by the HMAC engine.
 *
 * @details
 * The wrapped-key body size depends on the underlying SHA flavour
 * (HUM Ch 52.1 Table 52.1 p 3302). The selector is derived from
 * ``alg``.
 *
 * @param[in] alg One of ``k_ra_rsip_oem_cmd_hmac_sha*``.
 * @param[in] key Plaintext HMAC key.
 * @param[in] key_len ``key`` length in bytes.
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg ``alg`` not in the HMAC range, or
 * ``key_len`` is zero.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``alg`` is a HMAC opcode.
 * @pre ``key`` is non-NULL and ``key_len`` bytes readable.
 * @post On success ``out->alg == alg``.
 * @post On success ``out->body_words`` matches the algo's handle size.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_hmac_install_plain(ra_rsip_oem_cmd_t     alg,
                                                  const uint8_t*        key,
                                                  uint32_t              key_len,
                                                  ra_rsip_key_handle_t* out);

/* =============================================================================
 * Key install -- OEM (provisioning) flow
 * =============================================================================
 */

/**
 * @brief Install an OEM-encrypted key blob into the wrapped vault.
 *
 * @details
 * Drives the OEM (PE5/PE6) install primitive used during factory
 * provisioning: the plaintext key has already been encrypted under
 * the OEM root key and is delivered as ``oem_blob``; the engine
 * unwraps it inside the secure boundary and returns a vault-wrapped
 * handle (FSP ``r_rsip_key_injection.c`` -- ``InitialKeyWrap`` family).
 *
 * @param[in] cmd OEM opcode (algorithm + key length selector).
 * @param[in] iv 16-byte install IV.
 * @param[in] oem_blob OEM-encrypted body.
 * @param[in] blob_len ``oem_blob`` length in bytes.
 * @param[out] out Wrapped handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Unknown ``cmd`` or ``blob_len`` zero.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Engine reported a verification fail.
 *
 * @pre ``cmd`` != ``k_ra_rsip_oem_cmd_invalid``.
 * @pre ``iv``, ``oem_blob`` and ``out`` are non-NULL.
 * @post On success ``out->alg == cmd``.
 * @post On success ``out->body_words`` matches the algo's handle size.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_oem_install(ra_rsip_oem_cmd_t     cmd,
                                           const uint8_t*        iv,
                                           const uint8_t*        oem_blob,
                                           uint32_t              blob_len,
                                           ra_rsip_key_handle_t* out);

/* =============================================================================
 * AES symmetric cipher -- ECB / CBC / CTR / XTS / CMAC / GMAC
 * =============================================================================
 */

/**
 * @brief Encrypt or decrypt a buffer with AES in a non-AEAD mode.
 *
 * @details
 * Streams the input through ``DATA_IN0..3`` 16 bytes at a time and
 * pulls the result from ``DATA_OUT0..3``. The caller is responsible
 * for padding to a 16-byte boundary in modes that require it (ECB,
 * CBC, CMAC); CTR / XTS / GMAC accept partial trailing bytes.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] mode Block / authenticated mode selector.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv IV / counter / tweak (16 bytes); may be NULL
 * for ECB / CMAC.
 * @param[in] in Input buffer.
 * @param[out] out Output buffer (>= ``len`` bytes).
 * @param[in] len Number of input bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer transformed.
 * @retval k_ra_err_null_ptr Any required pointer was NULL.
 * @retval k_ra_err_invalid_arg ``mode`` is an AEAD mode, or
 * ``len`` is not a multiple of the
 * block size for ECB / CBC / CMAC.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and refers to a wrapped AES handle.
 * @pre ``in`` and ``out`` are non-NULL.
 * @post On success, ``out[0..len-1]`` holds the transformed bytes.
 * @post Engine SYM_STATUS.DONE has been observed and acked.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_aes_gcm
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes_cipher(const ra_rsip_key_handle_t* key,
                                          ra_rsip_aes_mode_t          mode,
                                          ra_rsip_aes_dir_t           dir,
                                          const uint8_t*              iv,
                                          const uint8_t*              in,
                                          uint8_t*                    out,
                                          uint32_t                    len);

/* =============================================================================
 * AES authenticated modes -- GCM / CCM
 * =============================================================================
 */

/**
 * @brief AES-GCM encrypt or decrypt with associated data.
 *
 * @details
 * NIST SP 800-38D (HUM Ch 52.1 Table 52.1 "GCM" p 3302). On encrypt,
 * ``tag`` is filled with the 16-byte authenticator; on decrypt, the
 * tag passed in is compared and the routine returns
 * ``k_ra_err_hw_error`` if the comparison fails.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte tag buffer (output on encrypt,
 * input on decrypt).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_null_ptr ``key``, ``iv``, ``in``, ``out`` or
 * ``tag`` was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Decrypt-side tag comparison failed.
 *
 * @pre ``key->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the authenticator.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes_gcm(const ra_rsip_key_handle_t* key,
                                       ra_rsip_aes_dir_t           dir,
                                       const uint8_t*              iv,
                                       const uint8_t*              aad,
                                       uint32_t                    aad_len,
                                       const uint8_t*              in,
                                       uint8_t*                    out,
                                       uint32_t                    in_len,
                                       uint8_t*                    tag);

/**
 * @brief AES-CCM encrypt or decrypt with associated data.
 *
 * @details
 * NIST SP 800-38C. Same surface as ``ra_rsip_aes_gcm`` -- the
 * engine handles the L / nonce concatenation internally; the caller
 * supplies a 12-byte nonce for compatibility.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte tag buffer.
 *
 * @return ``ra_err_t`` error code (same set as ``ra_rsip_aes_gcm``).
 *
 * @pre ``key->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the authenticator.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_aes_ccm(const ra_rsip_key_handle_t* key,
                                       ra_rsip_aes_dir_t           dir,
                                       const uint8_t*              iv,
                                       const uint8_t*              aad,
                                       uint32_t                    aad_len,
                                       const uint8_t*              in,
                                       uint8_t*                    out,
                                       uint32_t                    in_len,
                                       uint8_t*                    tag);

/* =============================================================================
 * ChaCha20 + Poly1305
 * =============================================================================
 */

/**
 * @brief ChaCha20 stream encrypt or decrypt (RFC 7539, no AEAD).
 *
 * @param[in] key Wrapped ChaCha20 key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] nonce 12-byte nonce.
 * @param[in] counter Initial 32-bit block counter.
 * @param[in] in Input buffer.
 * @param[out] out Output buffer (>= ``len`` bytes).
 * @param[in] len Input length in bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg == k_ra_rsip_oem_cmd_chacha20``.
 * @pre ``nonce`` is non-NULL.
 * @post On success, ``out[0..len-1]`` holds the transformed bytes.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_chacha20(const ra_rsip_key_handle_t* key,
                                        ra_rsip_aes_dir_t           dir,
                                        const uint8_t*              nonce,
                                        uint32_t                    counter,
                                        const uint8_t*              in,
                                        uint8_t*                    out,
                                        uint32_t                    len);

/**
 * @brief ChaCha20-Poly1305 AEAD encrypt or decrypt (RFC 7539).
 *
 * @param[in] key Wrapped ChaCha20 key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] nonce 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length in bytes.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte Poly1305 tag.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_null_ptr Any required pointer was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Decrypt-side tag check failed.
 *
 * @pre ``key->alg == k_ra_rsip_oem_cmd_chacha20``.
 * @pre ``nonce``, ``in``, ``out``, ``tag`` are non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the Poly1305 MAC.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_chacha20_poly1305(const ra_rsip_key_handle_t* key,
                                                 ra_rsip_aes_dir_t           dir,
                                                 const uint8_t*              nonce,
                                                 const uint8_t*              aad,
                                                 uint32_t                    aad_len,
                                                 const uint8_t*              in,
                                                 uint8_t*                    out,
                                                 uint32_t                    in_len,
                                                 uint8_t*                    tag);

/**
 * @brief Poly1305 MAC over a buffer using a 32-byte one-time key.
 *
 * @param[in] one_time_key 32-byte Poly1305 key (derived per message).
 * @param[in] msg Buffer to authenticate.
 * @param[in] msg_len Length of ``msg`` in bytes.
 * @param[out] tag 16-byte tag output.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Tag computed.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``one_time_key`` is non-NULL and 32-byte readable.
 * @pre ``tag`` is non-NULL.
 * @post On success, ``tag[0..15]`` is the Poly1305 MAC.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_rsip_poly1305(const uint8_t* one_time_key, const uint8_t* msg, uint32_t msg_len, uint8_t* tag);

/* =============================================================================
 * Hash family -- generic SHA-2 / SHA-3 / SHAKE
 * =============================================================================
 */

/**
 * @brief Compute a hash of an in-memory buffer using the selected algorithm.
 *
 * @details
 * Generalisation of ``ra_rsip_sha256`` to every algorithm in
 * ``ra_rsip_hash_alg_t``. ``out_len`` must be at least the digest
 * size for the selected algorithm; for SHAKE-128/256 ``out_len`` is
 * the requested XOF length and may be any positive value.
 *
 * @param[in] alg Algorithm selector.
 * @param[in] msg Message to hash; may be NULL only if ``msg_len``
 * is zero.
 * @param[in] msg_len Message length in bytes.
 * @param[out] digest Output buffer.
 * @param[in] digest_len Output buffer length.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Digest written.
 * @retval k_ra_err_null_ptr ``digest`` was NULL, or ``msg`` was
 * NULL with non-zero ``msg_len``.
 * @retval k_ra_err_invalid_arg ``digest_len`` too small for ``alg``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``alg`` is one of ``k_ra_rsip_hash_*``.
 * @pre If ``msg_len`` > 0, ``msg`` is non-NULL.
 * @post On success, ``digest[0..N-1]`` is the digest where N is
 * the algorithm's natural output size (or ``digest_len`` for
 * SHAKE).
 * @post HASH_STATUS.DONE has been acked.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_rsip_sha256
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_hash(ra_rsip_hash_alg_t alg,
                                    const uint8_t*     msg,
                                    uint32_t           msg_len,
                                    uint8_t*           digest,
                                    uint32_t           digest_len);

/**
 * @brief HMAC-SHA-2 / HMAC-SHA-3 over a buffer using a wrapped key.
 *
 * @param[in] key Wrapped HMAC key handle.
 * @param[in] msg Buffer to authenticate.
 * @param[in] msg_len Length of ``msg`` in bytes.
 * @param[out] mac Output MAC buffer (>= digest size of HMAC's
 * underlying hash).
 * @param[in] mac_len ``mac`` buffer length.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok MAC written.
 * @retval k_ra_err_null_ptr Any required pointer was NULL.
 * @retval k_ra_err_invalid_arg ``mac_len`` too small for the algo.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is a HMAC opcode.
 * @pre ``mac`` is non-NULL.
 * @post On success, ``mac[0..N-1]`` is the HMAC.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_hmac(const ra_rsip_key_handle_t* key,
                                    const uint8_t*              msg,
                                    uint32_t                    msg_len,
                                    uint8_t*                    mac,
                                    uint32_t                    mac_len);

/* =============================================================================
 * Asymmetric crypto -- RSA + ECDSA + ECDH
 * =============================================================================
 */

/**
 * @brief RSA sign a digest with a wrapped private key.
 *
 * @param[in] key Wrapped RSA private-key handle.
 * @param[in] size RSA modulus selector (1024 / 2048 / 3072 / 4096).
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length in bytes.
 * @param[out] signature Output signature (modulus / 8 bytes).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature produced.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad ``size``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is an RSA install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, ``signature[0..modulus_bytes-1]`` is the RSA sig.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_rsa_sign(const ra_rsip_key_handle_t* key,
                                        ra_rsip_rsa_size_t          size,
                                        const uint8_t*              digest,
                                        uint32_t                    digest_len,
                                        uint8_t*                    signature);

/**
 * @brief RSA verify a signature against a digest using a wrapped pubkey.
 *
 * @param[in] key Wrapped RSA public-key handle.
 * @param[in] size RSA modulus selector.
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length.
 * @param[in] signature Signature (modulus / 8 bytes).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature valid.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad ``size``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Signature did not verify.
 *
 * @pre ``key->alg`` is an RSA install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, the signature has been validated by the engine.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_rsa_verify(const ra_rsip_key_handle_t* key,
                                          ra_rsip_rsa_size_t          size,
                                          const uint8_t*              digest,
                                          uint32_t                    digest_len,
                                          const uint8_t*              signature);

/**
 * @brief ECDSA sign a digest with a wrapped private key.
 *
 * @param[in] key Wrapped ECC private-key handle.
 * @param[in] curve Curve selector.
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length.
 * @param[out] signature Output (r || s); 64 bytes for P-256, 96 for
 * P-384, 132 for P-521.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature produced.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad ``curve``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is an ECC install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, ``signature`` holds (r || s).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_ecdsa_sign(const ra_rsip_key_handle_t* key,
                                          ra_rsip_curve_t             curve,
                                          const uint8_t*              digest,
                                          uint32_t                    digest_len,
                                          uint8_t*                    signature);

/**
 * @brief ECDSA verify a signature with a peer public key.
 *
 * @param[in] key Wrapped ECC public-key handle (or the peer's
 * raw uncompressed point staged through
 * ``ASYM_PUB_X``/``ASYM_PUB_Y``).
 * @param[in] curve Curve selector.
 * @param[in] digest Pre-computed digest.
 * @param[in] digest_len Digest length.
 * @param[in] signature Signature (r || s).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature valid.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad ``curve``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Signature did not verify.
 *
 * @pre ``key->alg`` is an ECC install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, the engine has validated the signature.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_ecdsa_verify(const ra_rsip_key_handle_t* key,
                                            ra_rsip_curve_t             curve,
                                            const uint8_t*              digest,
                                            uint32_t                    digest_len,
                                            const uint8_t*              signature);

/**
 * @brief ECDH shared-secret derivation.
 *
 * @details
 * The peer public key is supplied as the uncompressed (X || Y)
 * coordinate pair. The shared secret stays inside the wrapped vault
 * and ``out`` receives a wrapped handle suitable for ``ra_rsip_kdf``.
 *
 * @param[in] key Wrapped ECC private-key handle (own).
 * @param[in] curve Curve selector.
 * @param[in] peer_x Peer X coordinate (curve byte length).
 * @param[in] peer_y Peer Y coordinate (curve byte length).
 * @param[out] out Wrapped shared-secret handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Shared secret derived.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad ``curve``.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Peer point off-curve.
 *
 * @pre ``key->alg`` is an ECC private install opcode.
 * @pre ``peer_x`` and ``peer_y`` are non-NULL.
 * @post On success ``out->alg`` matches the curve's HMAC opcode.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_ecdh_compute(const ra_rsip_key_handle_t* key,
                                            ra_rsip_curve_t             curve,
                                            const uint8_t*              peer_x,
                                            const uint8_t*              peer_y,
                                            ra_rsip_key_handle_t*       out);

/* =============================================================================
 * OEM boot loader version (anti-rollback)
 * =============================================================================
 */

/**
 * @brief Read the latched OEM boot loader version counter.
 *
 * @param[out] out Receives the 32-bit counter; never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``*out`` reflects the OEM_BL_VER cell.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_oem_bl_version_get(uint32_t* out);

/**
 * @brief Increment the OEM boot loader version (anti-rollback step).
 *
 * @details
 * Writes the inc-trigger word; the engine increments the counter
 * monotonically. The lock register MUST be set after a successful
 * boot to prevent further increments mid-flight.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Counter advanced.
 * @retval k_ra_err_invalid_state OEM_BL_LOCK already latched.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre ``OEM_BL_LOCK`` is clear.
 * @post Counter has incremented by 1.
 * @post Counter value is observable via ``ra_rsip_oem_bl_version_get``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_oem_bl_version_increment(void);

/**
 * @brief Latch the OEM_BL_LOCK so further increments are rejected.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Lock latched.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre Caller has finished any anti-rollback steps for this boot.
 * @post ``OEM_BL_LOCK`` reads as 1.
 * @post Subsequent ``ra_rsip_oem_bl_version_increment`` returns
 * ``k_ra_err_invalid_state``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_oem_bl_version_lock(void);

/* =============================================================================
 * Wrapped-key vault
 * =============================================================================
 */

/**
 * @brief Read a wrapped-key blob from a vault slot.
 *
 * @param[in] slot Slot index (0..``k_ra_rsip_kv_slot_count``-1).
 * @param[out] out 64-byte buffer to receive the wrapped blob.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob returned.
 * @retval k_ra_err_invalid_arg ``slot`` out of range.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 * @retval k_ra_err_not_found Slot is empty.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra_rsip_kv_slot_count``.
 * @pre ``out`` is non-NULL.
 * @post On success, ``out[0..63]`` holds the slot blob.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_kv_read(uint8_t slot, uint8_t* out);

/**
 * @brief Write a wrapped-key blob into a vault slot.
 *
 * @param[in] slot Slot index.
 * @param[in] in 64-byte wrapped blob.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot written.
 * @retval k_ra_err_invalid_arg ``slot`` out of range.
 * @retval k_ra_err_null_ptr ``in`` was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra_rsip_kv_slot_count``.
 * @pre ``in`` is non-NULL.
 * @post Slot now reads back as ``in``.
 * @post Populated-slot count incremented if the slot was empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_kv_write(uint8_t slot, const uint8_t* in);

/**
 * @brief Zeroise a vault slot.
 *
 * @param[in] slot Slot index.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot erased.
 * @retval k_ra_err_invalid_arg ``slot`` out of range.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra_rsip_kv_slot_count``.
 * @pre ``ra_rsip_init`` has been called.
 * @post Slot reads back as zero.
 * @post Populated-slot count decremented if the slot was non-empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_kv_erase(uint8_t slot);

/**
 * @brief Snapshot the populated-slot count.
 *
 * @param[out] out Receives the count (0..16); never NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Count returned.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``*out`` is in [0..``k_ra_rsip_kv_slot_count``].
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_kv_count(uint32_t* out);

/* =============================================================================
 * Key wrap / unwrap engine
 * =============================================================================
 */

/**
 * @brief Wrap a key handle into a transportable blob using a KEK.
 *
 * @param[in] kek Key-encryption-key handle.
 * @param[in] iv 16-byte wrap IV.
 * @param[in] src Source key handle to wrap.
 * @param[out] blob Wrapped blob output (64 bytes).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob produced.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``kek->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On success, ``blob[0..63]`` is the wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_key_wrap(const ra_rsip_key_handle_t* kek,
                                        const uint8_t*              iv,
                                        const ra_rsip_key_handle_t* src,
                                        uint8_t*                    blob);

/**
 * @brief Unwrap a transportable blob into a key handle.
 *
 * @param[in] kek Key-encryption-key handle.
 * @param[in] iv 16-byte wrap IV used at wrap time.
 * @param[in] blob Wrapped blob (64 bytes).
 * @param[out] dest Destination key handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra_err_hw_error Blob authenticity check failed.
 *
 * @pre ``kek->alg`` is an AES install opcode.
 * @pre ``iv``, ``blob``, ``dest`` are non-NULL.
 * @post On success, ``dest`` carries the unwrapped algorithm + body.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_key_unwrap(const ra_rsip_key_handle_t* kek,
                                          const uint8_t*              iv,
                                          const uint8_t*              blob,
                                          ra_rsip_key_handle_t*       dest);

/* =============================================================================
 * Key Derivation Function
 * =============================================================================
 */

/**
 * @brief Derive a key from input keying material + label/salt/info.
 *
 * @details
 * Implements:
 * - HKDF-SHA-256 / 384 / 512 -- ``op`` selects the hash flavour;
 * ``ikm`` is consumed as the IKM.
 * - HUK / UID bound -- ``op`` selects the source root, ``ikm`` is
 * NULL, and the engine binds the device-unique value.
 *
 * @param[in] op KDF mode selector.
 * @param[in] ikm Input keying material handle (may be NULL for
 * HUK / UID modes).
 * @param[in] label Label / context bytes.
 * @param[in] label_len Label length in bytes.
 * @param[in] salt Salt bytes (may be NULL).
 * @param[in] salt_len Salt length in bytes.
 * @param[in] out_len Bytes of derived material requested.
 * @param[out] out Wrapped derived-key handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Handle filled.
 * @retval k_ra_err_null_ptr ``out`` was NULL or ``label``/``ikm``
 * missing where required.
 * @retval k_ra_err_invalid_arg ``out_len`` zero or larger than the
 * hash's max XOF length.
 * @retval k_ra_err_hw_timeout Engine never signalled KDF_DONE.
 *
 * @pre ``op`` is one of ``k_ra_rsip_kdf_op_*``.
 * @pre ``out`` is non-NULL.
 * @post On success ``out`` carries a wrapped HMAC-SHA-256 handle.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_kdf(ra_rsip_kdf_op_t            op,
                                   const ra_rsip_key_handle_t* ikm,
                                   const uint8_t*              label,
                                   uint32_t                    label_len,
                                   const uint8_t*              salt,
                                   uint32_t                    salt_len,
                                   uint32_t                    out_len,
                                   ra_rsip_key_handle_t*       out);

/* =============================================================================
 * Device lifecycle + debug authorisation
 * =============================================================================
 */

/**
 * @brief Read the device-lifecycle state.
 *
 * @param[out] out Receives the lifecycle word.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``*out`` reflects ``LIFE_STATE``.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_life_get(ra_rsip_life_state_t* out);

/**
 * @brief Advance the device lifecycle to a new state.
 *
 * @details
 * Lifecycle transitions are one-way (HUM Ch 51.1 p 3263). The
 * engine rejects backward moves with ``k_ra_err_invalid_state``.
 *
 * @param[in] state Target lifecycle state.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transition complete.
 * @retval k_ra_err_invalid_arg Unknown ``state``.
 * @retval k_ra_err_invalid_state Transition would move backward.
 * @retval k_ra_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``state`` is one of ``ra_rsip_life_state_t``.
 * @pre ``state`` is forward of the current state.
 * @post Subsequent ``ra_rsip_life_get`` returns ``state``.
 *
 * @note Thread safety: not thread-safe.
 * @warning Lifecycle transitions are irreversible.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_life_advance(ra_rsip_life_state_t state);

/**
 * @brief Read the current debug authorisation level.
 *
 * @param[out] out Receives the level (AL0/AL1/AL2).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``*out`` is one of AL0/AL1/AL2.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_debug_level_get(ra_rsip_debug_level_t* out);

/**
 * @brief Set the debug authorisation level.
 *
 * @param[in] level Target level.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Level applied.
 * @retval k_ra_err_invalid_arg ``level`` out of range.
 *
 * @pre ``level`` is one of ``ra_rsip_debug_level_t``.
 * @pre Lifecycle state allows the requested level.
 * @post Subsequent ``ra_rsip_debug_level_get`` returns ``level``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_debug_level_set(ra_rsip_debug_level_t level);

/* =============================================================================
 * Tamper subsystem
 * =============================================================================
 */

/**
 * @brief Enable or disable a set of tamper sources.
 *
 * @param[in] sources OR of ``k_ra_rsip_tamper_src_*`` bits to enable.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Mask applied.
 * @retval k_ra_err_invalid_arg ``sources`` has bits outside the
 * documented mask.
 *
 * @pre ``sources`` AND-clean against ``k_ra_rsip_tamper_src_all``.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``TAMPER_CTRL`` reads as ``sources``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_tamper_enable(uint32_t sources);

/**
 * @brief Read latched tamper-source flags.
 *
 * @param[out] out Receives the flag word.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra_rsip_init`` has been called.
 * @post ``*out`` reflects ``TAMPER_STATUS``.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_tamper_status(uint32_t* out);

/**
 * @brief Acknowledge tamper-source flags (write-1-to-clear).
 *
 * @param[in] mask Bits to clear.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bits cleared.
 * @retval k_ra_err_invalid_arg ``mask`` had bits outside the field.
 *
 * @pre ``mask`` AND-clean against ``k_ra_rsip_tamper_src_all``.
 * @pre ``mask`` is non-zero.
 * @post Requested bits read as zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_tamper_ack(uint32_t mask);

/**
 * @brief Arm or disarm SPA / DPA countermeasures.
 *
 * @param[in] enable ``true`` to arm, ``false`` to disarm.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Done.
 *
 * @pre ``ra_rsip_init`` has been called.
 * @pre Engine is idle.
 * @post ``CTRL.DPA_ARM`` matches ``enable``.
 * @post Subsequent crypto ops run with countermeasures as requested.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_dpa_arm(bool enable);

/* =============================================================================
 * DOTF key delivery routing
 * =============================================================================
 */

/**
 * @brief Route a wrapped-key vault slot to one of the DOTF instances.
 *
 * @param[in] which DOTF instance (0 or 1).
 * @param[in] slot Vault slot to feed (must contain an AES key).
 * @param[in] on ``true`` to enable the route, ``false`` to disable.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Route applied.
 * @retval k_ra_err_invalid_arg ``which`` not 0/1 or ``slot`` out
 * of range.
 *
 * @pre ``which`` < 2.
 * @pre If ``on`` is ``true``, ``slot`` < ``k_ra_rsip_kv_slot_count``.
 * @post ``DOTFn_CTRL`` reflects the requested route.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rsip_dotf_route(uint8_t which, uint8_t slot, bool on);

#ifdef __cplusplus
}
#endif

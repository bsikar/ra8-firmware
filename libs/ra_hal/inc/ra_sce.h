/**
 * @file ra_sce.h
 * @brief Renesas Secure Crypto Engine (SCE) HAL driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Low-level primitive layer for the RA8D2 Secure Crypto Engine. The
 * SCE silicon block is shared with the RSIP-E50D mailbox layer
 * (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302-3307) and is
 * gated by the same module-stop bit ``MSTPCRC.MSTPC31`` (HUM
 * Ch 11.2.8 "MSTPCRC: Module Stop Control Register C" p 446-447).
 *
 * Where ``ra_rsip`` exposes the high-level mailbox protocol with
 * wrapped-key handles, this driver targets the lower-level FSP
 * ``r_sce`` shape:
 *
 * - AES-128 / 192 / 256 in ECB, CBC, CTR and GCM modes;
 * - SHA-1 / 224 / 256 / 384 / 512 single-shot and streaming;
 * - HMAC over any of the SHA flavours;
 * - hardware true random number generator.
 *
 * The implementation in ``ra_sce.c`` is a host-friendly software
 * stub by design: the FSP ``r_sce`` primitive layer is shipped as
 * compiled-only object files (``hw_sce_*.c`` blobs) under the
 * Renesas-only license, so this tree cannot mirror it byte-for-byte.
 * The stub is deterministic and round-trippable: every
 * ``encrypt`` / ``decrypt`` pair restores the plaintext, and every
 * hash / HMAC computation produces the same output for the same
 * input. A future drop-in real backend (open-source replacement or
 * a sanctioned FSP integration) only has to honour the public API
 * shape declared here.
 *
 * @warning The current backend is a placeholder, NOT cryptographically
 *          secure. It is sufficient for driver-shape validation,
 *          power-management exercise, and bring-up tests. Do not
 *          ship key-bearing firmware against this header until a
 *          real backend is wired up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_sce_regs.h"
#include "ra_err.h"

/**
 * @enum ra_sce_aes_mode_t
 * @brief AES block-cipher mode selector.
 *
 * @details
 * ECB / CBC require block-aligned input (multiple of 16 bytes).
 * CTR and GCM accept arbitrary-length payloads. GCM additionally
 * produces a 16-byte authentication tag through ``ra_sce_aes_finish``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_sce_aes_mode_ecb = 0U, /**< Electronic Codebook (no IV).        */
  k_ra_sce_aes_mode_cbc = 1U, /**< Cipher Block Chaining.              */
  k_ra_sce_aes_mode_ctr = 2U, /**< Counter mode (stream).              */
  k_ra_sce_aes_mode_gcm = 3U, /**< Galois/Counter mode (AEAD).         */
} ra_sce_aes_mode_t;

/**
 * @enum ra_sce_aes_key_bits_t
 * @brief Permitted AES key widths.
 *
 * @details
 * Stored as the literal bit count so existing call sites that say
 * ``128`` / ``192`` / ``256`` for clarity do not need indirection.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_sce_aes_key_bits_128 = 128U, /**< AES-128. */
  k_ra_sce_aes_key_bits_192 = 192U, /**< AES-192. */
  k_ra_sce_aes_key_bits_256 = 256U, /**< AES-256. */
} ra_sce_aes_key_bits_t;

/**
 * @enum ra_sce_sha_mode_t
 * @brief SHA flavour selector for the hash and HMAC engines.
 *
 * @details
 * Mirrors the SHA family supported by FSP's ``r_sce`` shape: SHA-1
 * (legacy) plus SHA-2 (224/256/384/512). The HMAC engine reuses
 * this selector to bind a SHA flavour to a key.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_sce_sha_mode_sha1   = 0U, /**< SHA-1 (legacy, 160-bit digest).  */
  k_ra_sce_sha_mode_sha224 = 1U, /**< SHA-224 (224-bit digest).        */
  k_ra_sce_sha_mode_sha256 = 2U, /**< SHA-256 (256-bit digest).        */
  k_ra_sce_sha_mode_sha384 = 3U, /**< SHA-384 (384-bit digest).        */
  k_ra_sce_sha_mode_sha512 = 4U, /**< SHA-512 (512-bit digest).        */
} ra_sce_sha_mode_t;

/**
 * @struct ra_sce_cfg_t
 * @brief Initial configuration for ``ra_sce_open``.
 *
 * @details
 * Surface is intentionally tiny -- the only knob is whether the
 * driver should run a self-test on bring-up. Production builds set
 * ``run_self_test = true``; bring-up firmware that wants to skip
 * the test on every reset can set it false.
 *
 * @invariant Caller owns the lifetime of the struct.
 *
 * @code{.c}
 * const ra_sce_cfg_t cfg = {.run_self_test = true};
 * (void)ra_sce_open(&cfg);
 * @endcode
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool run_self_test; /**< true -> arm one-shot self-test after MSTP release. */
} ra_sce_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power up the SCE block and (optionally) run the self-test.
 *
 * @details
 * Sequence:
 * 1. Release ``MSTPCRC.MSTPC31`` via ``ra_mstp_enable(k_ra_mstp_rsip)``.
 *    HUM Ch 11.2.8 p 446-447 + HUM Ch 52.3.2 "Module-Stop Function
 *    Setting" p 3307. (The SCE silicon shares the RSIP MSTP bit; the
 *    ra_mstp ref-count keeps the two coexisting.)
 * 2. Pulse CTRL.RESET, wait for STATUS.READY.
 * 3. Set CTRL.ENABLE.
 * 4. If ``cfg->run_self_test`` is true, dispatch a single TRNG read
 *    and verify the resulting word ticked at least once.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine ready.
 * @retval k_ra_err_null_ptr ``cfg`` was nullptr.
 * @retval k_ra_err_hw_init_failed Self-test did not pass.
 * @retval k_ra_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, the engine is clocked and STATUS.READY = 1.
 * @post On success, MSTPC31 ref count is at least 1.
 *
 * @note Thread safety: not thread-safe.
 *
 * @code{.c}
 * const ra_sce_cfg_t cfg = {.run_self_test = true};
 * if (ra_sce_open(&cfg) != k_ra_ok) { panic(); }
 * @endcode
 *
 * @see ra_sce_close
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_open(const ra_sce_cfg_t* cfg);

/**
 * @brief Power down the SCE block.
 *
 * @details
 * Clears CTRL.ENABLE, scrubs any in-progress AES / SHA / HMAC
 * context, and gates ``MSTPCRC.MSTPC31`` via the shared ra_mstp
 * ref-count.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine gated.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_timeout MSTP read-back loop expired.
 *
 * @pre Engine is idle (caller must drain in-flight commands).
 * @pre ``ra_sce_open`` has been called at least once since reset.
 * @post Engine is gated and the cached crypto contexts are wiped.
 * @post MSTPC31 ref count is decremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_open
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_close(void);

/* =============================================================================
 * AES symmetric cipher
 * =============================================================================
 */

/**
 * @brief Initialise an AES context.
 *
 * @details
 * Latches the key, mode and IV inside the driver TU; subsequent
 * ``ra_sce_aes_encrypt`` / ``_decrypt`` calls operate against the
 * cached context until ``ra_sce_aes_finish`` clears it.
 *
 * @param[in] key       Plaintext key buffer (``key_bits / 8`` bytes).
 * @param[in] key_bits  Key width.
 * @param[in] mode      Block-cipher mode.
 * @param[in] iv        16-byte IV; may be NULL only for ECB.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context initialised.
 * @retval k_ra_err_null_ptr ``key`` was NULL or ``iv`` NULL with a
 *                           mode that requires it.
 * @retval k_ra_err_invalid_arg Unknown ``mode`` or ``key_bits``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` has returned ``k_ra_ok``.
 * @pre ``key_bits`` is one of ``ra_sce_aes_key_bits_t``.
 * @post On success, AES context is latched and STATUS.BUSY = 0.
 * @post On success, GCM tag staging area is zeroed.
 *
 * @note Thread safety: not thread-safe; the engine has one context.
 *
 * @see ra_sce_aes_encrypt
 * @see ra_sce_aes_decrypt
 * @see ra_sce_aes_finish
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_aes_init(const uint8_t*        key,
                                       ra_sce_aes_key_bits_t key_bits,
                                       ra_sce_aes_mode_t     mode,
                                       const uint8_t*        iv);

/**
 * @brief AES encrypt a buffer using the latched context.
 *
 * @details
 * For ECB and CBC, ``len`` must be a multiple of
 * ``k_ra_sce_aes_block_bytes`` (16). CTR and GCM accept any
 * non-zero length and stream the keystream over the input.
 *
 * @param[in]  plaintext  Source buffer (>= ``len`` bytes).
 * @param[out] ciphertext Destination buffer (>= ``len`` bytes).
 * @param[in]  len        Number of bytes to encrypt.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer transformed.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 * @retval k_ra_err_invalid_arg ``len`` zero or not block-aligned in
 *                              ECB / CBC.
 * @retval k_ra_err_invalid_state ``ra_sce_aes_init`` had not run.
 *
 * @pre ``ra_sce_aes_init`` has returned ``k_ra_ok``.
 * @pre ``plaintext`` and ``ciphertext`` are non-NULL.
 * @post On success, ``ciphertext[0..len-1]`` holds the encrypted
 *       payload and (GCM) the running tag has been mixed in.
 * @post On success, STATUS.DONE has been observed and acked.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len);

/**
 * @brief AES decrypt a buffer using the latched context.
 *
 * @details
 * Mirror of ``ra_sce_aes_encrypt``: same length rules apply.
 *
 * @param[in]  ciphertext Source buffer (>= ``len`` bytes).
 * @param[out] plaintext  Destination buffer (>= ``len`` bytes).
 * @param[in]  len        Number of bytes to decrypt.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer transformed.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 * @retval k_ra_err_invalid_arg ``len`` zero or not block-aligned in
 *                              ECB / CBC.
 * @retval k_ra_err_invalid_state ``ra_sce_aes_init`` had not run.
 *
 * @pre ``ra_sce_aes_init`` has returned ``k_ra_ok``.
 * @pre ``ciphertext`` and ``plaintext`` are non-NULL.
 * @post On success, ``plaintext[0..len-1]`` holds the recovered data.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len);

/**
 * @brief Finalise the latched AES context (clear + emit GCM tag).
 *
 * @details
 * Clears the cached key, IV and counter state. If the active mode
 * was GCM and ``tag_out`` is non-NULL, copies the running tag (16
 * bytes). After this call the driver requires another
 * ``ra_sce_aes_init`` before any further AES operations.
 *
 * @param[out] tag_out 16-byte tag buffer (GCM only); may be NULL
 *                     for ECB / CBC / CTR or when the caller does
 *                     not need the GCM tag.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context cleared (and GCM tag emitted if requested).
 * @retval k_ra_err_invalid_state ``ra_sce_aes_init`` had not run.
 *
 * @pre ``ra_sce_aes_init`` has returned ``k_ra_ok``.
 * @post Cached AES context is wiped.
 * @post Subsequent ``ra_sce_aes_encrypt`` returns
 *       ``k_ra_err_invalid_state`` until ``ra_sce_aes_init`` re-runs.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_aes_finish(uint8_t* tag_out);

/* =============================================================================
 * SHA family (SHA-1 / 224 / 256 / 384 / 512)
 * =============================================================================
 */

/**
 * @brief Initialise an incremental hash context.
 *
 * @param[in] mode SHA flavour.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context ready.
 * @retval k_ra_err_invalid_arg ``mode`` is not in ``ra_sce_sha_mode_t``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` has returned ``k_ra_ok``.
 * @pre ``mode`` is one of the named values.
 * @post Streaming counters are zero.
 * @post HASH_DIGEST staging area is zero.
 *
 * @note Thread safety: not thread-safe; the engine has one context.
 *
 * @see ra_sce_sha_update
 * @see ra_sce_sha_final
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_sha_init(ra_sce_sha_mode_t mode);

/**
 * @brief Absorb a chunk of message bytes into the hash context.
 *
 * @param[in] data Message bytes (>= ``len`` bytes); may be NULL only
 *                 if ``len`` is zero.
 * @param[in] len  Number of bytes to absorb.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bytes absorbed.
 * @retval k_ra_err_null_ptr ``data`` NULL with non-zero ``len``.
 * @retval k_ra_err_invalid_state ``ra_sce_sha_init`` had not run.
 *
 * @pre ``ra_sce_sha_init`` has returned ``k_ra_ok``.
 * @pre If ``len > 0`` then ``data`` is non-NULL.
 * @post Streaming length counter is incremented by ``len``.
 * @post Running digest mixer state has consumed ``len`` bytes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_sha_update(const uint8_t* data, uint32_t len);

/**
 * @brief Squeeze out the digest and clear the hash context.
 *
 * @param[out] digest_out Output buffer (must be >= the active digest
 *                        size for the chosen SHA flavour).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Digest written.
 * @retval k_ra_err_null_ptr ``digest_out`` was NULL.
 * @retval k_ra_err_invalid_state ``ra_sce_sha_init`` had not run.
 *
 * @pre ``ra_sce_sha_init`` has returned ``k_ra_ok``.
 * @pre ``digest_out`` is non-NULL.
 * @post On success, ``digest_out[0..N-1]`` holds the digest.
 * @post Cached hash context is wiped; another ``ra_sce_sha_init``
 *       is required before further hashing.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_sha_final(uint8_t* digest_out);

/* =============================================================================
 * TRNG
 * =============================================================================
 */

/**
 * @brief Drain ``len`` bytes from the hardware true RNG.
 *
 * @details
 * Writes ``k_ra_sce_cmd_trng_read`` for each ``k_ra_sce_trng_word_bytes``
 * (4-byte) chunk and copies the RND_DATA word into the caller's
 * buffer. ``len`` may be any non-zero value; the tail is filled
 * byte-wise from the last word.
 *
 * @param[out] out_buf Destination (>= ``len`` bytes); never NULL.
 * @param[in]  len     Bytes to fetch; must be non-zero.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer filled.
 * @retval k_ra_err_null_ptr ``out_buf`` was NULL.
 * @retval k_ra_err_invalid_arg ``len`` is zero.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_timeout RND_STATUS.READY did not assert.
 *
 * @pre ``ra_sce_open`` has returned ``k_ra_ok``.
 * @pre ``out_buf`` is non-NULL.
 * @post On success, ``out_buf[0..len-1]`` holds fresh bytes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_random(uint8_t* out_buf, uint32_t len);

/* =============================================================================
 * HMAC (SHA-1 / 224 / 256 / 384 / 512)
 * =============================================================================
 */

/**
 * @brief Initialise an incremental HMAC context.
 *
 * @param[in] key      HMAC key bytes; never NULL when ``key_len > 0``.
 * @param[in] key_len  Length of ``key`` in bytes; must be non-zero.
 * @param[in] sha_mode Underlying SHA flavour.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context ready.
 * @retval k_ra_err_null_ptr ``key`` was NULL.
 * @retval k_ra_err_invalid_arg ``key_len`` zero or ``sha_mode`` not in
 *                              ``ra_sce_sha_mode_t``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` has returned ``k_ra_ok``.
 * @pre ``key`` is non-NULL.
 * @post HMAC streaming counters are zero.
 * @post HMAC_DIGEST staging area is zero.
 *
 * @note Thread safety: not thread-safe; the engine has one context.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_hmac_init(const uint8_t* key, uint32_t key_len, ra_sce_sha_mode_t sha_mode);

/**
 * @brief Absorb a chunk of message bytes into the HMAC context.
 *
 * @param[in] data Message bytes; may be NULL only if ``len`` is zero.
 * @param[in] len  Number of bytes to absorb.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bytes absorbed.
 * @retval k_ra_err_null_ptr ``data`` NULL with non-zero ``len``.
 * @retval k_ra_err_invalid_state ``ra_sce_hmac_init`` had not run.
 *
 * @pre ``ra_sce_hmac_init`` has returned ``k_ra_ok``.
 * @pre If ``len > 0`` then ``data`` is non-NULL.
 * @post HMAC streaming counter is incremented by ``len``.
 * @post Running HMAC mixer state has consumed ``len`` bytes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_hmac_update(const uint8_t* data, uint32_t len);

/**
 * @brief Squeeze out the HMAC tag and clear the HMAC context.
 *
 * @param[out] mac_out Output buffer (must be >= the active digest
 *                     size for the chosen SHA flavour).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok MAC written.
 * @retval k_ra_err_null_ptr ``mac_out`` was NULL.
 * @retval k_ra_err_invalid_state ``ra_sce_hmac_init`` had not run.
 *
 * @pre ``ra_sce_hmac_init`` has returned ``k_ra_ok``.
 * @pre ``mac_out`` is non-NULL.
 * @post On success, ``mac_out[0..N-1]`` holds the MAC.
 * @post Cached HMAC context is wiped.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_hmac_final(uint8_t* mac_out);

#ifdef __cplusplus
}
#endif

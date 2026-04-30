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

/* =============================================================================
 * RSA public-key crypto
 * =============================================================================
 */

/**
 * @enum ra_sce_rsa_key_bits_t
 * @brief Permitted RSA modulus widths (FSP r_sce supports 2048/3072/4096).
 *
 * @details
 * Stored as the literal modulus bit count so the byte count is just
 * ``key_bits / 8``. Applies to both modulus and private-exponent
 * buffers; the public exponent is always carried in
 * ``k_ra_sce_rsa_pub_exp_bytes`` (4) bytes.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_sce_rsa_key_bits_2048 = 2048U, /**< RSA-2048 (256-byte modulus). */
  k_ra_sce_rsa_key_bits_3072 = 3072U, /**< RSA-3072 (384-byte modulus). */
  k_ra_sce_rsa_key_bits_4096 = 4096U, /**< RSA-4096 (512-byte modulus). */
} ra_sce_rsa_key_bits_t;

/**
 * @struct ra_sce_rsa_pub_t
 * @brief Plain (non-wrapped) RSA public key descriptor.
 *
 * @details
 * ``modulus`` is a big-endian byte string of length ``key_bits / 8``;
 * ``exponent`` is a 4-byte big-endian public exponent (typically
 * 0x00010001). The buffer pointers must remain valid for the duration
 * of the call that consumes them.
 *
 * @invariant ``modulus`` is non-NULL.
 * @invariant ``key_bits`` is one of ``ra_sce_rsa_key_bits_t``.
 *
 * @since 0.1.0
 */
typedef struct {
  ra_sce_rsa_key_bits_t key_bits; /**< Modulus width. */
  const uint8_t*        modulus;  /**< Big-endian modulus (key_bits / 8 bytes). */
  const uint8_t*        exponent; /**< Big-endian 4-byte public exponent. */
} ra_sce_rsa_pub_t;

/**
 * @struct ra_sce_rsa_priv_t
 * @brief Plain (non-wrapped) RSA private key descriptor.
 *
 * @details
 * ``private_exp`` is the modular inverse of the public exponent and
 * is the same length as the modulus. CRT components are not exposed
 * here -- the stub backend uses straight modular exponentiation.
 *
 * @invariant ``modulus`` and ``private_exp`` are non-NULL.
 *
 * @since 0.1.0
 */
typedef struct {
  ra_sce_rsa_key_bits_t key_bits;    /**< Modulus width.                         */
  const uint8_t*        modulus;     /**< Big-endian modulus (key_bits / 8).     */
  const uint8_t*        private_exp; /**< Big-endian private exponent.           */
} ra_sce_rsa_priv_t;

/**
 * @brief RSA public-key encrypt (textbook RSAEP).
 *
 * @details
 * Computes ``ciphertext = plaintext^e mod n`` where ``n`` is the
 * modulus and ``e`` the public exponent. The plaintext is treated
 * as a big-endian integer that must be < n. The output buffer must
 * be sized to the modulus byte length and is written big-endian.
 *
 * @warning Stub: software modular-exponentiation backend; NOT a
 *          drop-in for OAEP / PKCS#1 v1.5 padding schemes.
 *
 * @param[in]  pub                 Public key descriptor.
 * @param[in]  plaintext           Big-endian message integer; ``<= modulus``.
 * @param[in]  plaintext_len       Bytes in ``plaintext`` (``<= modulus_len``).
 * @param[out] ciphertext_out      Destination buffer; ``modulus_len`` bytes.
 * @param[in]  ciphertext_out_len  Capacity of ``ciphertext_out``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Ciphertext written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Lengths inconsistent or unsupported key width.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Buffers do not overlap.
 * @post On success, ``ciphertext_out[0..modulus_len-1]`` holds the
 *       big-endian RSA encryption.
 * @post ``ciphertext_out_len >= modulus_len``.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_rsa_private_decrypt
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_rsa_public_encrypt(const ra_sce_rsa_pub_t* pub,
                                                 const uint8_t*          plaintext,
                                                 uint32_t                plaintext_len,
                                                 uint8_t*                ciphertext_out,
                                                 uint32_t                ciphertext_out_len);

/**
 * @brief RSA public-key signature verify (textbook RSAVP1).
 *
 * @details
 * Recovers ``m = signature^e mod n`` and compares it byte-wise against
 * ``message``. Returns ``k_ra_ok`` only if every byte matches.
 *
 * @warning Stub backend. See file-level @warning.
 *
 * @param[in] pub        Public key descriptor.
 * @param[in] message    Expected message bytes (typically a hash).
 * @param[in] msg_len    Length of ``message`` (``<= modulus_len``).
 * @param[in] signature  Big-endian signature value (``modulus_len`` bytes).
 * @param[in] sig_len    Length of ``signature``; must equal ``modulus_len``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature matches.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Length mismatch / unsupported width.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_error Recovered message did not match.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre ``sig_len == modulus_len``.
 * @post No state changes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_rsa_private_sign
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_rsa_public_verify(const ra_sce_rsa_pub_t* pub,
                                                const uint8_t*          message,
                                                uint32_t                msg_len,
                                                const uint8_t*          signature,
                                                uint32_t                sig_len);

/**
 * @brief RSA private-key decrypt (textbook RSADP).
 *
 * @details
 * Computes ``plaintext = ciphertext^d mod n``. Output is written
 * big-endian, then leading zeros are stripped and the actual length
 * returned via ``*plaintext_len_out`` so the caller can recover
 * ``plaintext_len`` < ``modulus_len`` payloads.
 *
 * @warning Stub backend. See file-level @warning.
 *
 * @param[in]      priv             Private key descriptor.
 * @param[in]      ciphertext       Big-endian ciphertext (``modulus_len``).
 * @param[in]      ciphertext_len   Length of ``ciphertext``.
 * @param[out]     plaintext_out    Destination (``>= modulus_len``).
 * @param[in,out]  plaintext_len_out On entry: capacity. On exit: bytes used.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Plaintext written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Length mismatch / unsupported width.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre ``*plaintext_len_out >= modulus_len``.
 * @post On success, ``*plaintext_len_out`` reflects the actual plaintext
 *       length after leading-zero strip.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_rsa_public_encrypt
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_rsa_private_decrypt(const ra_sce_rsa_priv_t* priv,
                                                  const uint8_t*           ciphertext,
                                                  uint32_t                 ciphertext_len,
                                                  uint8_t*                 plaintext_out,
                                                  uint32_t*                plaintext_len_out);

/**
 * @brief RSA private-key sign (textbook RSASP1).
 *
 * @details
 * Computes ``signature = message^d mod n``. The caller is responsible
 * for any digest-info / padding (PKCS#1 v1.5, PSS) -- this entry
 * point performs the raw modular exponentiation only.
 *
 * @warning Stub backend. See file-level @warning.
 *
 * @param[in]  priv          Private key descriptor.
 * @param[in]  message       Message bytes (``<= modulus_len``).
 * @param[in]  msg_len       Length of ``message``.
 * @param[out] signature_out Destination (``modulus_len`` bytes).
 * @param[in]  sig_out_len   Capacity of ``signature_out``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Length mismatch / unsupported width.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre ``sig_out_len >= modulus_len``.
 * @post On success, ``signature_out[0..modulus_len-1]`` is set.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_rsa_public_verify
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_rsa_private_sign(const ra_sce_rsa_priv_t* priv,
                                               const uint8_t*           message,
                                               uint32_t                 msg_len,
                                               uint8_t*                 signature_out,
                                               uint32_t                 sig_out_len);

/* =============================================================================
 * ECC public-key crypto (P-256, P-384)
 * =============================================================================
 */

/**
 * @enum ra_sce_ecc_curve_t
 * @brief Supported elliptic curves.
 *
 * @details
 * Mirrors the FSP r_sce curve list -- the stub backend implements
 * arithmetic for both NIST-P-256 and NIST-P-384 over their natural
 * short-Weierstrass equation ``y^2 = x^3 + a*x + b mod p``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_sce_ecc_curve_p256 = 0U, /**< NIST P-256 / secp256r1.   */
  k_ra_sce_ecc_curve_p384 = 1U, /**< NIST P-384 / secp384r1.   */
} ra_sce_ecc_curve_t;

/**
 * @enum ra_sce_ecc_const_t
 * @brief Sizing constants for ECC byte strings.
 *
 * @details
 * Public keys are uncompressed ``(x || y)`` in big-endian; private
 * keys are big-endian scalars matching the curve order width.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_ecc_p256_priv_bytes = 32U, /**< P-256 private scalar bytes.    */
  k_ra_sce_ecc_p256_pub_bytes  = 64U, /**< P-256 public X || Y bytes.     */
  k_ra_sce_ecc_p256_sig_bytes  = 64U, /**< P-256 ECDSA signature R || S.  */
  k_ra_sce_ecc_p384_priv_bytes = 48U, /**< P-384 private scalar bytes.    */
  k_ra_sce_ecc_p384_pub_bytes  = 96U, /**< P-384 public X || Y bytes.     */
  k_ra_sce_ecc_p384_sig_bytes  = 96U, /**< P-384 ECDSA signature R || S.  */
  k_ra_sce_ecc_max_pub_bytes   = 96U, /**< Largest pub-key buffer needed. */
  k_ra_sce_ecc_max_priv_bytes  = 48U, /**< Largest priv-key buffer.       */
  k_ra_sce_ecc_max_sig_bytes   = 96U, /**< Largest signature buffer.      */
} ra_sce_ecc_const_t;

/**
 * @brief Generate an ECC key pair on the chosen curve.
 *
 * @details
 * Draws a curve-order-width scalar from the TRNG, treats it as the
 * private key, and computes ``Q = d * G`` for the curve-base point
 * ``G``. ``out_priv`` receives the scalar; ``out_pub`` receives the
 * uncompressed ``(x || y)`` form.
 *
 * @warning Stub backend uses double-and-add over a tiny on-host curve
 *          model; NOT cryptographically secure. See file-level
 *          @warning in ra_sce.h.
 *
 * @param[in]  curve     Target curve.
 * @param[out] out_priv  Private scalar (curve-priv-bytes long).
 * @param[out] out_pub   Public X || Y (curve-pub-bytes long).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Key pair written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg ``curve`` not in ``ra_sce_ecc_curve_t``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Buffer sizes match the curve.
 * @post On success, ``out_priv`` and ``out_pub`` are populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_ecc_keygen(ra_sce_ecc_curve_t curve, uint8_t* out_priv, uint8_t* out_pub);

/**
 * @brief ECDSA sign a fixed-length hash.
 *
 * @details
 * Stub backend: produces a deterministic ``(r, s)`` pair from the
 * (priv, hash) tuple so signatures round-trip with verify(). The
 * pair is written as concatenated big-endian ``r || s`` of equal
 * length matching the curve order width.
 *
 * @warning Stub backend. See file-level @warning.
 *
 * @param[in]  curve     Curve identifier.
 * @param[in]  priv_key  Private scalar buffer.
 * @param[in]  hash      Message digest bytes.
 * @param[in]  hash_len  Length of ``hash`` (``<= sig_bytes / 2``).
 * @param[out] sig_out   ``(r || s)`` destination buffer.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Curve unknown.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @post On success, ``sig_out`` is curve-sig-bytes long.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_ecc_ecdsa_verify
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_ecc_ecdsa_sign(ra_sce_ecc_curve_t curve,
                                             const uint8_t*     priv_key,
                                             const uint8_t*     hash,
                                             uint32_t           hash_len,
                                             uint8_t*           sig_out);

/**
 * @brief ECDSA verify a (hash, signature) tuple against a public key.
 *
 * @param[in] curve    Curve identifier.
 * @param[in] pub_key  Uncompressed ``(x || y)`` public key.
 * @param[in] hash     Message digest bytes.
 * @param[in] hash_len Length of ``hash``.
 * @param[in] sig      ``(r || s)`` signature.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature matches the public key.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Curve unknown.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_error Signature did not validate.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @post No state changes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_ecc_ecdsa_sign
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_ecc_ecdsa_verify(ra_sce_ecc_curve_t curve,
                                               const uint8_t*     pub_key,
                                               const uint8_t*     hash,
                                               uint32_t           hash_len,
                                               const uint8_t*     sig);

/**
 * @brief ECDH shared-secret derivation.
 *
 * @details
 * Computes ``Z = priv * peer_pub``. ``shared_secret_out`` receives
 * the X coordinate of the resulting point in big-endian form, which
 * is the standard ECDH output.
 *
 * @warning Stub backend. See file-level @warning.
 *
 * @param[in]  curve              Curve identifier.
 * @param[in]  priv_key           Local private scalar.
 * @param[in]  peer_pub_key       Peer's public key (X || Y).
 * @param[out] shared_secret_out  Destination (curve-priv-bytes long).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Shared secret written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Curve unknown.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @post On success, ``shared_secret_out[0..priv_bytes-1]`` holds X.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_ecc_ecdh(ra_sce_ecc_curve_t curve,
                                       const uint8_t*     priv_key,
                                       const uint8_t*     peer_pub_key,
                                       uint8_t*           shared_secret_out);

#ifdef __cplusplus
}
#endif

/**
 * @file ra_psa_crypto.h
 * @brief Application-level PSA Crypto facade over tf-psa-crypto
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra_psa_crypto`` is a thin, project-shaped wrapper around the
 * vendored TF-PSA-Crypto 1.x library at
 * ``libs/third_party/tf-psa-crypto``. Where ``ra_tls`` provides session-
 * oriented TLS, this module exposes the *application-level* PSA Crypto
 * primitives (key import / destroy, signing, hashing, AEAD) behind a
 * small ``ra_psa_*`` API that returns ``ra_err_t``.
 *
 * The facade has three jobs:
 *
 * 1. **Hide PSA spelling**: callers never include ``psa/crypto.h``
 *    directly. A higher-level OTA verifier, secure-boot ledger, or
 *    BLE pairing flow asks ``ra_psa_*`` for "sign this hash with key
 *    handle 3" and never touches a ``psa_key_id_t``.
 *
 * 2. **Enforce NASA Rule 3**: key handles come from a fixed-size
 *    static pool of ``k_ra_psa_max_keys`` slots. Application code
 *    cannot accidentally leak key material across boots or fragment
 *    the PSA key store -- if the pool is full, ``ra_psa_key_import``
 *    returns ``k_ra_err_no_mem``.
 *
 * 3. **Optional RSIP routing**: where the silicon has a hardware
 *    crypto engine (``ra_rsip``), the facade may transparently route
 *    AES-GCM, SHA-256, and ECDSA primitives through it. The public
 *    surface is identical regardless of whether the operation ran in
 *    software (TF-PSA-Crypto) or hardware (RSIP).
 *
 * ## Layering
 *
 * @verbatim
 *   +---------------------------+ ra_psa_sign_hash, ra_psa_aead_encrypt
 *   | App (OTA / boot / pairing)|
 *   +-------------+-------------+
 *                 |
 *                 v
 *   +---------------------------+ ra_psa_crypto (this header)
 *   | TF-PSA-Crypto 1.x         |
 *   +-------------+-------------+
 *                 |
 *                 v (optional, when present)
 *   +---------------------------+ ra_rsip hardware accelerator
 *   | Silicon AES / SHA / ECDSA |
 *   +---------------------------+
 * @endverbatim
 *
 * ## References
 *
 * - PSA Crypto API specification, ARM IHI 0086 v1.1.0, Sections 9-12
 *   (key management, hash, AEAD, asymmetric signature).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Limits
 * =============================================================================
 */

/**
 * @enum ra_psa_limits_t
 * @brief Static-pool sizing constants for the PSA Crypto facade.
 *
 * @details
 * Sized to comfortably cover the worst-case number of long-lived keys
 * we expect in a single boot: a device identity key, two OTA verify
 * keys (current + rollover), a TLS client key, a BLE LTK, and a
 * handful of ephemeral session keys. Increasing the bound only costs
 * a few bytes of ``.bss`` per slot.
 *
 * @invariant ``k_ra_psa_max_keys`` fits in a ``uint8_t``.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.1 "Key identifiers".
 */
typedef enum : uint8_t {
  /**
   * @brief Maximum simultaneous key handles handed out by the pool.
   * @details NASA Power of 10 Rule 3 cap: any further import returns
   *          ``k_ra_err_no_mem``.
   */
  k_ra_psa_max_keys = 16U,

  /**
   * @brief Maximum imported raw-key length in bytes (P-384 + AES-256).
   */
  k_ra_psa_max_key_bytes = 96U,

  /**
   * @brief SHA-256 digest length (RFC 6234, Section 4.1).
   */
  k_ra_psa_sha256_len = 32U,

  /**
   * @brief AES-GCM nonce length used by ``ra_psa_aead_*`` (NIST SP 800-38D).
   */
  k_ra_psa_gcm_nonce_len = 12U,

  /**
   * @brief AES-GCM authentication tag length (16 octets).
   */
  k_ra_psa_gcm_tag_len = 16U,

  /**
   * @brief Maximum ECDSA signature length we ever emit (P-384 raw r||s).
   */
  k_ra_psa_max_sig_bytes = 96U,
} ra_psa_limits_t;

/* =============================================================================
 * Algorithm + key-type tags
 * =============================================================================
 */

/**
 * @enum ra_psa_key_type_t
 * @brief Project-local enum mirroring the PSA key-type families we care about.
 *
 * @details
 * Mirrors the subset of ``PSA_KEY_TYPE_*`` codes we actually use, mapped
 * back to canonical PSA values inside ``ra_psa_crypto.c``. Keeping the
 * tag in our own namespace lets callers avoid a transitive include of
 * ``psa/crypto_values.h``.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.2 "Key types".
 */
typedef enum : uint8_t {
  k_ra_psa_key_type_raw           = 0U, /**< Raw octet string (HKDF input).        */
  k_ra_psa_key_type_aes           = 1U, /**< Symmetric AES key.                    */
  k_ra_psa_key_type_hmac          = 2U, /**< HMAC key (any hash).                  */
  k_ra_psa_key_type_ecc_p256_priv = 3U, /**< ECDSA P-256 private key.              */
  k_ra_psa_key_type_ecc_p256_pub  = 4U, /**< ECDSA P-256 public key (uncompressed).*/
} ra_psa_key_type_t;

/**
 * @enum ra_psa_alg_t
 * @brief Algorithm selector passed to sign / verify / AEAD operations.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 10 "Algorithms".
 */
typedef enum : uint8_t {
  k_ra_psa_alg_none          = 0U, /**< Sentinel for "unset".                    */
  k_ra_psa_alg_sha_256       = 1U, /**< SHA-256 hash (FIPS 180-4).              */
  k_ra_psa_alg_aes_gcm       = 2U, /**< AES-GCM AEAD (NIST SP 800-38D).         */
  k_ra_psa_alg_ecdsa_sha_256 = 3U, /**< ECDSA over SHA-256 (FIPS 186-4).        */
} ra_psa_alg_t;

/**
 * @enum ra_psa_key_usage_t
 * @brief Bitmask of allowed operations on an imported key.
 *
 * @details
 * Equivalent to PSA's ``psa_key_usage_t`` flags, restricted to the
 * subset this facade exposes.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.4 "Key policies".
 */
typedef enum : uint32_t {
  k_ra_psa_usage_none    = 0x00U, /**< No usage allowed (placeholder).            */
  k_ra_psa_usage_sign    = 0x01U, /**< Allow ``ra_psa_sign_hash``.                */
  k_ra_psa_usage_verify  = 0x02U, /**< Allow ``ra_psa_verify_hash``.              */
  k_ra_psa_usage_encrypt = 0x04U, /**< Allow ``ra_psa_aead_encrypt``.             */
  k_ra_psa_usage_decrypt = 0x08U, /**< Allow ``ra_psa_aead_decrypt``.             */
  k_ra_psa_usage_derive  = 0x10U, /**< Allow KDF-style derivation (future).       */
} ra_psa_key_usage_t;

/* =============================================================================
 * Opaque key handle
 * =============================================================================
 */

/**
 * @struct ra_psa_key_handle
 * @brief Forward declaration of the static-pool slot type.
 *
 * @details
 * Defined inside ``ra_psa_crypto.c``. Callers only ever see the typed
 * pointer; the slot stores the underlying ``psa_key_id_t``, the cached
 * algorithm metadata, and an ``in_use`` flag.
 */
struct ra_psa_key_handle;

/**
 * @brief Opaque PSA key handle (typed pointer into the static pool).
 *
 * @details
 * NULL is the sentinel for "uninitialised handle". The only legal way
 * to obtain a non-NULL value is ``ra_psa_key_import``; passing any
 * other pointer to ``ra_psa_*`` yields ``k_ra_err_invalid_arg``.
 *
 * @since 0.1.0
 */
typedef struct ra_psa_key_handle* ra_psa_key_t;

/**
 * @struct ra_psa_key_attr_t
 * @brief Attributes describing a key being imported.
 *
 * @details
 * Carries everything the facade needs to translate a raw byte buffer
 * into a PSA key slot: type, intended algorithm, and usage flags.
 * Mirrors the ``psa_key_attributes_t`` triple of (type, alg, usage)
 * documented in the PSA spec Section 9.4.
 *
 * @invariant ``type`` is one of ``ra_psa_key_type_t``.
 * @invariant ``usage`` has at least one bit set.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.4 "Key policies".
 * @since 0.1.0
 */
typedef struct ra_psa_key_attr {
  ra_psa_key_type_t  type;  /**< Key type family (AES, ECDSA priv, ...).         */
  ra_psa_alg_t       alg;   /**< Algorithm the key is permitted to drive.        */
  ra_psa_key_usage_t usage; /**< Allowed operations on this key.                 */
} ra_psa_key_attr_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief One-shot facade initialisation.
 *
 * @details
 * Calls ``psa_crypto_init`` (PSA Crypto API spec Sec 8.2), zeroes the
 * key-handle pool, and marks the module ready. Safe to call exactly
 * once per boot; subsequent calls without a matching
 * ``ra_psa_crypto_deinit`` return ``k_ra_err_exists``.
 *
 * Algorithm:
 * 1. If already initialised, return ``k_ra_err_exists``.
 * 2. Invoke ``psa_crypto_init`` (or the simulator stand-in).
 * 3. Zero ``s_key_pool`` so close-without-import paths are well defined.
 * 4. Mark the module initialised.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Facade ready.
 * @retval k_ra_err_exists   Already initialised this boot.
 * @retval k_ra_err_hw_error TF-PSA-Crypto initialisation reported a fault.
 *
 * @pre TF-PSA-Crypto has been built into the firmware image
 *      (``RA_USE_MBEDTLS=ON``) OR ``RA_SIMULATOR_MODE`` is defined.
 * @pre Caller is on the boot thread; not safe to interleave with other
 *      crypto calls.
 * @post Module is in the initialised state on success.
 * @post Key pool is fully reset (no slot held).
 *
 * @note Not re-entrant. Call from the boot path.
 * @warning Do not invoke any ``ra_psa_*`` function before this returns
 *          ``k_ra_ok``; doing so yields ``k_ra_err_not_initialized``.
 *
 * @par Example:
 * @code{.c}
 * ra_err_t err = ra_psa_crypto_init();
 * RA_RETURN_ON_ERROR(err, "ra_psa", "init failed");
 * @endcode
 *
 * @see ra_psa_crypto_deinit()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 8.2 "Library initialisation".
 * @since 0.1.0
 */
ra_err_t ra_psa_crypto_init(void);

/**
 * @brief Symmetric tear-down for ``ra_psa_crypto_init``.
 *
 * @details
 * Destroys every still-imported key, calls ``mbedtls_psa_crypto_free``
 * (or the simulator stand-in), and clears the initialised flag so a
 * subsequent ``ra_psa_crypto_init`` succeeds again.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Facade torn down.
 * @retval k_ra_err_not_initialized ``ra_psa_crypto_init`` was never called.
 *
 * @pre Module was previously initialised.
 * @pre Caller has guaranteed no other thread holds a key handle.
 * @post Pool is empty and module is not initialised.
 * @post All TF-PSA-Crypto resources released.
 *
 * @note Not re-entrant.
 *
 * @see ra_psa_crypto_init()
 * @since 0.1.0
 */
ra_err_t ra_psa_crypto_deinit(void);

/**
 * @brief Import a raw-byte key into the static pool.
 *
 * @details
 * Allocates the first free slot, copies ``data`` into the underlying
 * PSA key store via ``psa_import_key`` (PSA spec Sec 9.5), and returns
 * an opaque handle through ``out_handle``.
 *
 * @param[out] out_handle Receives the new opaque handle on success.
 *                        Set to NULL on any non-success return.
 * @param[in]  attr       Key attributes (type, algorithm, usage).
 * @param[in]  data       Raw key bytes; layout depends on ``attr->type``.
 * @param[in]  data_len   Length of ``data`` in bytes; must be
 *                        ``<= k_ra_psa_max_key_bytes``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Key imported and handle valid.
 * @retval k_ra_err_invalid_arg     NULL pointer or malformed attributes.
 * @retval k_ra_err_invalid_size    ``data_len`` exceeds the static cap.
 * @retval k_ra_err_not_initialized Facade was never initialised.
 * @retval k_ra_err_no_mem          Pool exhausted (``k_ra_psa_max_keys``).
 * @retval k_ra_err_hw_error        Underlying ``psa_import_key`` rejected
 *                                  the key (bad key material).
 *
 * @pre ``ra_psa_crypto_init`` returned ``k_ra_ok`` previously.
 * @pre ``data_len > 0`` and ``data`` is non-NULL.
 * @post On ``k_ra_ok``, ``*out_handle`` is non-NULL and lives until a
 *       matching ``ra_psa_key_destroy``.
 * @post On any error, ``*out_handle`` is set to NULL.
 *
 * @note Not thread-safe; serialise with concurrent destroys.
 *
 * @par Example:
 * @code{.c}
 * ra_psa_key_attr_t attr = {
 *     .type  = k_ra_psa_key_type_aes,
 *     .alg   = k_ra_psa_alg_aes_gcm,
 *     .usage = (ra_psa_key_usage_t)
 *              (k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
 * };
 * ra_psa_key_t k = NULL;
 * (void)ra_psa_key_import(&k, &attr, raw, sizeof(raw));
 * @endcode
 *
 * @see ra_psa_key_destroy()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.5 "Key import / export".
 * @since 0.1.0
 */
ra_err_t ra_psa_key_import(ra_psa_key_t*            out_handle,
                           const ra_psa_key_attr_t* attr,
                           const uint8_t*           data,
                           size_t                   data_len);

/**
 * @brief Destroy a previously-imported key.
 *
 * @details
 * Calls ``psa_destroy_key`` on the underlying PSA key id, zeroes the
 * slot, and clears the in-use bit. Safe to call on any imported key.
 *
 * @param[in,out] handle Key handle previously returned by
 *                       ``ra_psa_key_import``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Key destroyed.
 * @retval k_ra_err_invalid_arg     Handle NULL or not from this pool.
 * @retval k_ra_err_not_initialized Facade not initialised.
 *
 * @pre Handle was returned by ``ra_psa_key_import``.
 * @pre Module is initialised.
 * @post Slot is free and may be re-issued.
 * @post No further use of ``handle`` is permitted.
 *
 * @see ra_psa_key_import()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 9.6 "psa_destroy_key".
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_key_destroy(ra_psa_key_t handle);

/**
 * @brief Compute a one-shot SHA-256 digest.
 *
 * @details
 * Wraps ``psa_hash_compute`` (PSA spec Sec 10.2.1). The output buffer
 * must be at least ``k_ra_psa_sha256_len`` bytes; ``*out_len`` is
 * updated with the bytes actually written (always 32 on success).
 *
 * @param[in]  alg     Hash algorithm; must equal ``k_ra_psa_alg_sha_256``
 *                     for this revision of the facade.
 * @param[in]  input   Input message bytes.
 * @param[in]  input_len Length of ``input`` in bytes.
 * @param[out] out     Buffer that receives the digest.
 * @param[in]  out_cap Capacity of ``out`` in bytes
 *                     (``>= k_ra_psa_sha256_len``).
 * @param[out] out_len Bytes actually written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Digest written.
 * @retval k_ra_err_invalid_arg     Pointer NULL or wrong algorithm.
 * @retval k_ra_err_invalid_size    ``out_cap`` too small.
 * @retval k_ra_err_not_initialized Facade not initialised.
 * @retval k_ra_err_hw_error        PSA reported a fatal error.
 *
 * @pre Facade initialised.
 * @pre ``out`` is non-NULL.
 * @post On ``k_ra_ok``, ``*out_len == k_ra_psa_sha256_len``.
 * @post On any error, ``*out_len == 0``.
 *
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 10.2 "Hash operations".
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_hash_compute(ra_psa_alg_t   alg,
                             const uint8_t* input,
                             size_t         input_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len);

/**
 * @brief Sign a pre-computed hash with a private ECDSA key.
 *
 * @details
 * Wraps ``psa_sign_hash`` (PSA spec Sec 12.6). Only ECDSA over SHA-256
 * is exercised; other algorithms return ``k_ra_err_not_supported``.
 * The caller is responsible for hashing the message first --
 * ``ra_psa_sign_hash`` does *not* hash ``hash``, it signs the bytes
 * verbatim per the PSA contract.
 *
 * @param[in]  handle    Private-key handle with ``k_ra_psa_usage_sign``.
 * @param[in]  alg       Signature algorithm; must be
 *                       ``k_ra_psa_alg_ecdsa_sha_256``.
 * @param[in]  hash      Pre-computed digest bytes.
 * @param[in]  hash_len  Length of ``hash`` (32 for SHA-256).
 * @param[out] sig       Output buffer for the signature.
 * @param[in]  sig_cap   Capacity of ``sig``
 *                       (``>= k_ra_psa_max_sig_bytes``).
 * @param[out] sig_len   Bytes actually written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Signature emitted.
 * @retval k_ra_err_invalid_arg     NULL pointer or alg mismatch.
 * @retval k_ra_err_invalid_size    Output buffer too small.
 * @retval k_ra_err_not_initialized Facade not initialised.
 * @retval k_ra_err_not_supported   Key type / alg not implemented.
 * @retval k_ra_err_hw_error        Underlying ``psa_sign_hash`` failed.
 *
 * @pre Handle was imported with ``k_ra_psa_usage_sign``.
 * @pre ``hash_len == k_ra_psa_sha256_len`` for SHA-256.
 * @post On ``k_ra_ok``, ``*sig_len > 0`` and ``<= sig_cap``.
 * @post On any error, ``*sig_len == 0``.
 *
 * @see ra_psa_verify_hash()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 12.6 "psa_sign_hash".
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_sign_hash(ra_psa_key_t   handle,
                          ra_psa_alg_t   alg,
                          const uint8_t* hash,
                          size_t         hash_len,
                          uint8_t*       sig,
                          size_t         sig_cap,
                          size_t*        sig_len);

/**
 * @brief Verify an ECDSA signature over a pre-computed hash.
 *
 * @details
 * Wraps ``psa_verify_hash`` (PSA spec Sec 12.7). Returns ``k_ra_ok``
 * on a valid signature, ``k_ra_err_crc_mismatch`` on a structurally
 * well-formed but invalid signature, and an ``k_ra_err_*`` value for
 * other failures.
 *
 * @param[in] handle   Public-key handle with ``k_ra_psa_usage_verify``.
 * @param[in] alg      Signature algorithm; must be
 *                     ``k_ra_psa_alg_ecdsa_sha_256``.
 * @param[in] hash     Pre-computed digest bytes.
 * @param[in] hash_len Length of ``hash``.
 * @param[in] sig      Signature bytes.
 * @param[in] sig_len  Length of ``sig``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Signature is valid.
 * @retval k_ra_err_crc_mismatch    Signature is invalid.
 * @retval k_ra_err_invalid_arg     Pointer NULL or alg mismatch.
 * @retval k_ra_err_not_initialized Facade not initialised.
 * @retval k_ra_err_not_supported   Algorithm not implemented.
 * @retval k_ra_err_hw_error        Underlying PSA call reported a fault.
 *
 * @pre Handle was imported with ``k_ra_psa_usage_verify``.
 * @pre ``hash_len == k_ra_psa_sha256_len``.
 * @post No state modified on failure.
 *
 * @see ra_psa_sign_hash()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 12.7 "psa_verify_hash".
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_verify_hash(ra_psa_key_t   handle,
                            ra_psa_alg_t   alg,
                            const uint8_t* hash,
                            size_t         hash_len,
                            const uint8_t* sig,
                            size_t         sig_len);

/**
 * @brief Encrypt + authenticate a buffer with AES-GCM.
 *
 * @details
 * Wraps ``psa_aead_encrypt`` (PSA spec Sec 11.4). Output layout is
 * ciphertext concatenated with the 16-octet authentication tag, so
 * ``out_cap`` must be ``>= plain_len + k_ra_psa_gcm_tag_len``.
 *
 * @param[in]  handle    Key with ``k_ra_psa_usage_encrypt`` set.
 * @param[in]  alg       Must be ``k_ra_psa_alg_aes_gcm``.
 * @param[in]  nonce     12-byte nonce (must be unique per key per call).
 * @param[in]  nonce_len Length of ``nonce`` (``k_ra_psa_gcm_nonce_len``).
 * @param[in]  aad       Additional authenticated data (may be NULL when
 *                       ``aad_len == 0``).
 * @param[in]  aad_len   Length of ``aad``.
 * @param[in]  plain     Plaintext input.
 * @param[in]  plain_len Length of ``plain``.
 * @param[out] out       Buffer for ciphertext || tag.
 * @param[in]  out_cap   Capacity of ``out``.
 * @param[out] out_len   Bytes written
 *                       (== ``plain_len + k_ra_psa_gcm_tag_len`` on
 *                       success).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Ciphertext + tag written.
 * @retval k_ra_err_invalid_arg     NULL pointer / alg mismatch.
 * @retval k_ra_err_invalid_size    Output buffer too small or nonce
 *                                  wrong length.
 * @retval k_ra_err_not_initialized Facade not initialised.
 * @retval k_ra_err_hw_error        Underlying AEAD reported a fault.
 *
 * @pre Handle was imported with ``k_ra_psa_usage_encrypt``.
 * @pre ``nonce_len == k_ra_psa_gcm_nonce_len``.
 * @post On ``k_ra_ok`` ``*out_len == plain_len + k_ra_psa_gcm_tag_len``.
 * @post On error ``*out_len == 0``.
 *
 * @warning Reusing a (key, nonce) pair under AES-GCM catastrophically
 *          breaks confidentiality and integrity (NIST SP 800-38D 8.3).
 *
 * @see ra_psa_aead_decrypt()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 11.4 "psa_aead_encrypt".
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_aead_encrypt(ra_psa_key_t   handle,
                             ra_psa_alg_t   alg,
                             const uint8_t* nonce,
                             size_t         nonce_len,
                             const uint8_t* aad,
                             size_t         aad_len,
                             const uint8_t* plain,
                             size_t         plain_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len);

/**
 * @brief Decrypt + verify-tag an AES-GCM buffer.
 *
 * @details
 * Wraps ``psa_aead_decrypt`` (PSA spec Sec 11.5). ``cipher`` must be
 * ciphertext concatenated with the 16-octet authentication tag, as
 * produced by ``ra_psa_aead_encrypt``.
 *
 * @param[in]  handle     Key with ``k_ra_psa_usage_decrypt`` set.
 * @param[in]  alg        Must be ``k_ra_psa_alg_aes_gcm``.
 * @param[in]  nonce      12-byte nonce.
 * @param[in]  nonce_len  Length of ``nonce``.
 * @param[in]  aad        Additional authenticated data.
 * @param[in]  aad_len    Length of ``aad``.
 * @param[in]  cipher     Ciphertext || tag input.
 * @param[in]  cipher_len Length of ``cipher`` (must be
 *                        ``>= k_ra_psa_gcm_tag_len``).
 * @param[out] out        Buffer for plaintext.
 * @param[in]  out_cap    Capacity of ``out``.
 * @param[out] out_len    Plaintext bytes written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Plaintext recovered, tag valid.
 * @retval k_ra_err_invalid_arg     NULL pointer / alg mismatch.
 * @retval k_ra_err_invalid_size    Output buffer too small or input
 *                                  too short to contain a tag.
 * @retval k_ra_err_not_initialized Facade not initialised.
 * @retval k_ra_err_crc_mismatch    Tag verification failed
 *                                  (ciphertext / aad / nonce / key
 *                                  was tampered with).
 * @retval k_ra_err_hw_error        Underlying AEAD reported a fault.
 *
 * @pre Handle was imported with ``k_ra_psa_usage_decrypt``.
 * @pre ``cipher_len >= k_ra_psa_gcm_tag_len``.
 * @post On ``k_ra_ok``, ``*out_len == cipher_len - k_ra_psa_gcm_tag_len``.
 * @post On any error, ``*out_len == 0``.
 *
 * @see ra_psa_aead_encrypt()
 * @see PSA Crypto API spec (ARM IHI 0086) Sec 11.5 "psa_aead_decrypt".
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_psa_aead_decrypt(ra_psa_key_t   handle,
                             ra_psa_alg_t   alg,
                             const uint8_t* nonce,
                             size_t         nonce_len,
                             const uint8_t* aad,
                             size_t         aad_len,
                             const uint8_t* cipher,
                             size_t         cipher_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len);

#ifdef __cplusplus
}
#endif

/**
 * @file sec_cmac_internal.h
 * @brief Secure-side AES-CMAC seam (real PSA backend / in-tree reference)
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Single-responsibility message-authentication primitive used by the
 * secure-side key importer (``key_import.c``) to authenticate a
 * wrapped-key blob before it is admitted into the key vault. The MAC is
 * AES-CMAC (NIST SP 800-38B), keyed by a secret the caller supplies -- the
 * importer passes the key-authentication key (KAK) it loads from the vault
 * via ``ra8_key_vault_load_mac_key``, so the CMAC key never originates from
 * anything the Non-Secure world can reach.
 *
 * This module is a *dependency-inversion seam* with two interchangeable
 * (Liskov-substitutable) backends selected at compile time. Both compute
 * the identical, standard AES-CMAC, so a blob authenticated by one verifies
 * under the other:
 *
 * - ``RA8_KEY_IMPORT_PSA_CMAC`` defined -- the firmware/HIL image routes to
 *   the vendored, silicon-proven TF-PSA-Crypto (``psa_mac_compute`` /
 *   ``psa_mac_verify`` with ``PSA_ALG_CMAC`` over ``PSA_KEY_TYPE_AES``).
 * - otherwise (the default for host unit tests and every app that does not
 *   link TF-PSA-Crypto) -- a self-contained in-tree AES-128 / AES-256 +
 *   CMAC reference (FIPS 197 + SP 800-38B). It is *real* cryptography, not a
 *   forgeable placeholder: the host suite pins it to the published NIST
 *   SP 800-38B known-answer vectors.
 *
 * Unlike the forgeable length-tagged XOR fold it replaces, neither backend
 * is guarded by the ``RA8_INSECURE_STUB_CRYPTO`` fail-closed fence: an
 * AES-CMAC forgery requires recovering the KAK, so there is no insecure
 * placeholder to hide behind a production stub.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_sec_cmac_limits_t
 * @brief Sizing constants for the secure-side AES-CMAC primitive.
 *
 * @details
 * The tag length is one AES block (128 bits). AES-CMAC accepts a 128- or
 * 256-bit key; the importer keys it with a 256-bit KAK. ``msg`` length is
 * statically capped so every internal loop has a provable bound
 * (NASA Power of 10 Rule 2).
 *
 * @invariant ``k_ra8_sec_cmac_tag_bytes`` equals the AES block size.
 * @invariant ``k_ra8_sec_cmac_key_256 >= k_ra8_sec_cmac_key_128``.
 *
 * @see ra8_sec_cmac_compute()
 * @see NIST SP 800-38B "The CMAC Mode for Authentication".
 */
typedef enum : uint16_t {
  k_ra8_sec_cmac_tag_bytes     = 16U,  /**< CMAC tag length (one AES block). */
  k_ra8_sec_cmac_key_128       = 16U,  /**< AES-128 key length in bytes.     */
  k_ra8_sec_cmac_key_256       = 32U,  /**< AES-256 key length in bytes.     */
  k_ra8_sec_cmac_max_msg_bytes = 256U, /**< Largest authenticated message.   */
} ra8_sec_cmac_limits_t;

/**
 * @brief Compute the AES-CMAC tag of a message under a symmetric key.
 *
 * @details
 * Runs NIST SP 800-38B CMAC over ``msg`` with AES as the block cipher.
 * ``key_len`` selects AES-128 (``k_ra8_sec_cmac_key_128``) or AES-256
 * (``k_ra8_sec_cmac_key_256``). The 16-byte tag is written to ``out_mac``.
 * The active backend (PSA or the in-tree reference) is chosen at compile
 * time and produces the identical standard tag.
 *
 * @param[in]  key     Symmetric CMAC key (secret; caller-owned secure copy).
 * @param[in]  key_len Key length: 16 (AES-128) or 32 (AES-256) bytes.
 * @param[in]  msg     Message to authenticate; NULL only when ``msg_len==0``.
 * @param[in]  msg_len Message length, ``0 .. k_ra8_sec_cmac_max_msg_bytes``.
 * @param[out] out_mac Destination for the ``k_ra8_sec_cmac_tag_bytes`` tag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Tag computed and written.
 * @retval k_ra8_err_null_ptr     ``key`` or ``out_mac`` (or ``msg`` when
 *                                ``msg_len!=0``) was NULL.
 * @retval k_ra8_err_invalid_arg  ``key_len`` was neither 16 nor 32.
 * @retval k_ra8_err_invalid_size ``msg_len`` exceeded the static cap.
 * @retval k_ra8_err_hw_error     PSA backend reported a fault (PSA build).
 *
 * @pre ``key`` and ``out_mac`` are non-NULL.
 * @pre ``key_len`` is 16 or 32.
 * @post On ``k_ra8_ok``, ``out_mac[0..15]`` holds the CMAC tag.
 * @post On any error, no output byte is relied upon by the caller.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 *
 * @par Example:
 * @code
 * uint8_t tag[k_ra8_sec_cmac_tag_bytes];
 * (void)ra8_sec_cmac_compute(kak, k_ra8_sec_cmac_key_256, blob, 32U, tag);
 * @endcode
 *
 * @see ra8_sec_cmac_verify()
 * @see NIST SP 800-38B Sec 6.2 "MAC Generation".
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_sec_cmac_compute(const uint8_t* key,
                                                      uint16_t       key_len,
                                                      const uint8_t* msg,
                                                      uint32_t       msg_len,
                                                      uint8_t*       out_mac);

/**
 * @brief Verify an AES-CMAC tag against a message under a symmetric key.
 *
 * @details
 * Recomputes the CMAC of ``msg`` under ``key`` and compares it, in constant
 * time, to the caller-supplied ``mac``. A single flipped message or tag
 * byte, or a truncated tag (``mac_len != k_ra8_sec_cmac_tag_bytes``), makes
 * the verdict fail. The verdict is the module's security-critical decision.
 *
 * @param[in] key     Symmetric CMAC key (secret; caller-owned secure copy).
 * @param[in] key_len Key length: 16 (AES-128) or 32 (AES-256) bytes.
 * @param[in] msg     Authenticated message; NULL only when ``msg_len==0``.
 * @param[in] msg_len Message length, ``0 .. k_ra8_sec_cmac_max_msg_bytes``.
 * @param[in] mac     Candidate tag to check.
 * @param[in] mac_len Length of ``mac``; must equal ``k_ra8_sec_cmac_tag_bytes``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Tag is authentic.
 * @retval k_ra8_err_invalid_arg  Tag mismatch, wrong ``mac_len``, or bad
 *                                ``key_len``.
 * @retval k_ra8_err_null_ptr     ``key`` or ``mac`` (or ``msg`` when
 *                                ``msg_len!=0``) was NULL.
 * @retval k_ra8_err_invalid_size ``msg_len`` exceeded the static cap.
 * @retval k_ra8_err_hw_error     PSA backend reported a fault (PSA build).
 *
 * @pre ``key`` and ``mac`` are non-NULL.
 * @pre ``key_len`` is 16 or 32.
 * @post No output state is produced; the return code is the whole result.
 * @post ``msg`` and ``mac`` are unmodified.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 *
 * @par MC/DC:
 * Verdict decision `if ((mac_len != k_ra8_sec_cmac_tag_bytes) ||
 * !ra8_ct_equal(computed, mac, k_ra8_sec_cmac_tag_bytes))` (2 conditions):
 *  - C1 = ``mac_len`` is not the expected tag length.
 *  - C2 = the recomputed tag does not equal ``mac``.
 * N=2 -> N+1=3 minimal vectors:
 *  - V1 (C1=F, C2=F): correct length + authentic tag -> decision F -> ok.
 *  - V2 (C1=T): truncated tag -> decision T (short-circuits C2) -> reject.
 *  - V3 (C1=F, C2=T): correct length + one flipped tag byte -> decision T
 *    -> reject.
 * V1+V2 vary C1 (masked C2); V1+V3 vary C2 with C1 held F. Satisfies
 * DO-178C 6.4.4.2 minimal MC/DC.
 *
 * @see ra8_sec_cmac_compute()
 * @see NIST SP 800-38B Sec 6.3 "MAC Verification".
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_sec_cmac_verify(const uint8_t* key,
                                                     uint16_t       key_len,
                                                     const uint8_t* msg,
                                                     uint32_t       msg_len,
                                                     const uint8_t* mac,
                                                     uint16_t       mac_len);

#ifdef __cplusplus
}
#endif

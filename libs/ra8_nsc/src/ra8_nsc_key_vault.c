/**
 * @file ra8_nsc_key_vault.c
 * @brief NSC veneer: secure key vault challenge-response
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * deliverable. Wraps ``ra8_key_vault_sha256_xor_challenge``
 * for Non-Secure callers. The raw key never leaves the secure
 * world; only the 32-byte SHA-256 digest crosses the boundary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "key_vault.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

static const char* s_tag = "NSCKV";

/**
 * @brief NSC veneer: SHA-256(slot_key XOR challenge) for a stored slot.
 *
 * @details
 * Validates that the NS challenge buffer (read) and the NS digest
 * buffer (read/write) lie inside the Non-Secure region, then forwards
 * to ``ra8_key_vault_sha256_xor_challenge``. The raw key never leaves
 * the secure world; only the 32-byte SHA-256 digest crosses the
 * boundary back to NS.
 *
 * The 32-byte slot bound is published by ``k_ra8_key_vault_chal_bytes``
 * and ``k_ra8_key_vault_digest_bytes``; the veneer enforces it on both
 * buffers before calling into the secure key vault.
 *
 * @param[in]  slot      Vault slot index.
 * @param[in]  ns_chal   NS challenge buffer (k_ra8_key_vault_chal_bytes).
 * @param[out] ns_digest NS destination for the SHA-256 digest
 *                       (k_ra8_key_vault_digest_bytes).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                      Digest copied to ``ns_digest``.
 * @retval k_ra8_err_null_ptr            A pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg         Buffer outside NS region or bad slot.
 *
 * @pre TrustZone substrate up.
 * @pre Both buffers lie entirely within the NS data region.
 * @post On success ``ns_digest`` holds the SHA-256(key XOR challenge).
 * @post On failure ``ns_digest`` content is undefined and no key bytes
 *       are exposed.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Both NS pointers are
 *   ``cmse_check_address_range``-validated. The raw slot key is read
 *   only inside the secure world; it is XORed with the challenge and
 *   hashed before any byte crosses back to NS, so a malicious NS caller
 *   cannot recover the key from the digest without breaking SHA-256.
 *
 * @note Thread-safe: serialised by the secure key-vault lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_key_vault_challenge(uint16_t       slot,
                                                     const uint8_t* ns_chal,
                                                     uint8_t*       ns_digest)
{
  RA8_CHECK_NULL_PTR(ns_chal, s_tag, "challenge: ns_chal");
  RA8_CHECK_NULL_PTR(ns_digest, s_tag, "challenge: ns_digest");
  RA8_NSC_CHECK_NS_RANGE_R(ns_chal, (uint32_t)k_ra8_key_vault_chal_bytes);
  RA8_NSC_CHECK_NS_RANGE_RW(ns_digest, (uint32_t)k_ra8_key_vault_digest_bytes);
  return ra8_key_vault_sha256_xor_challenge(slot, ns_chal, ns_digest);
}

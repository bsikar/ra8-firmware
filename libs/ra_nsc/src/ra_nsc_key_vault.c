/**
 * @file ra_nsc_key_vault.c
 * @brief NSC veneer: secure key vault challenge-response
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * deliverable. Wraps ``ra_key_vault_sha256_xor_challenge``
 * for Non-Secure callers. The raw key never leaves the secure
 * world; only the 32-byte SHA-256 digest crosses the boundary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "key_vault.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"

static const char* s_tag = "NSCKV";

/* The 32-byte slot bound is published by k_ra_key_vault_*_bytes;
 * the veneer enforces it on both buffers before calling into the
 * secure key vault. */
RA_NSC_VENEER ra_err_t ra_nsc_key_vault_challenge(uint16_t       slot,
                                                  const uint8_t* ns_chal,
                                                  uint8_t*       ns_digest)
{
  RA_CHECK_NULL_PTR((void*)ns_chal, s_tag, "challenge: ns_chal");
  RA_CHECK_NULL_PTR(ns_digest, s_tag, "challenge: ns_digest");
  RA_NSC_CHECK_NS_RANGE_R(ns_chal, (uint32_t)k_ra_key_vault_chal_bytes);
  RA_NSC_CHECK_NS_RANGE_RW(ns_digest, (uint32_t)k_ra_key_vault_digest_bytes);
  return ra_key_vault_sha256_xor_challenge(slot, ns_chal, ns_digest);
}

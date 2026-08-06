/**
 * @file ra8_app_verify.c
 * @brief Implementation of ThreadX module verification.
 * @ingroup grp_core
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_app_verify.h"

#include "ra8_app_api.h"
#define RA8_ED25519_SIG_LEN 64U
#include <stddef.h>
#include <string.h>

ra8_err_t ra8_app_verify(const uint8_t* binary, uint32_t len, const uint8_t public_key[32])
{
  if ((binary == nullptr) || (public_key == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  if (len < (sizeof(ra8_app_header_t) + RA8_ED25519_SIG_LEN)) {
    return k_ra8_err_invalid_arg;
  }

  /* Copy header to a properly-aligned local to avoid -Wcast-align on ARM. */
  ra8_app_header_t hdr;
  (void)memcpy(&hdr, binary, sizeof(hdr));

  if (hdr.magic != k_ra8_app_magic) {
    return k_ra8_err_invalid_arg;
  }

  if (hdr.version != 1U) {
    return k_ra8_err_invalid_arg;
  }

  if ((hdr.sig_offset + RA8_ED25519_SIG_LEN) > len) {
    return k_ra8_err_invalid_arg;
  }

  /* TODO: wire to actual Ed25519 verification library.
     * Signature is at binary + hdr.sig_offset (64 bytes).
     * Signed region is binary[0 .. hdr.sig_offset). */

  return k_ra8_ok;
}

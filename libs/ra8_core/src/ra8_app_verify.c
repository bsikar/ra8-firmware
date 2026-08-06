/**
 * @file ra8_app_verify.c
 * @brief Implementation of ThreadX module verification.
 * @ingroup grp_app
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_app_verify.h"
#include "ra8_app_api.h"
#include <stddef.h>

ra8_err_t ra8_app_verify(const uint8_t *binary, uint32_t len, const uint8_t public_key[32])
{
    if (binary == NULL || public_key == NULL) {
        return k_ra8_err_invalid_arg;
    }

    if (len < (sizeof(ra8_app_header_t) + 64U)) {
        return k_ra8_err_invalid_arg;
    }

    const ra8_app_header_t *hdr = (const ra8_app_header_t *)binary;

    if (hdr->magic != k_ra8_app_magic) {
        return k_ra8_err_invalid_arg;
    }

    if (hdr->version != 1U) {
        return k_ra8_err_invalid_arg;
    }

    if ((hdr->sig_offset + 64U) > len) {
        return k_ra8_err_invalid_arg;
    }

    /* TODO: wire to actual Ed25519 lib */
    /* Check Ed25519 signature in binary + hdr->sig_offset */

    return k_ra8_ok;
}

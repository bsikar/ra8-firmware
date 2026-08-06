/**
 * @file ra8_app_verify.h
 * @brief Ed25519 signature verification for RA8 ThreadX modules.
 * @ingroup grp_app
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_APP_VERIFY_H
#define RA8_APP_VERIFY_H

#include <stdint.h>
#include "ra8_err.h"

/**
 * @brief Verify a module binary's header and Ed25519 signature.
 *
 * @param binary Pointer to the module binary loaded in memory.
 * @param len Length of the binary in bytes.
 * @param public_key 32-byte Ed25519 public key.
 * @return k_ra8_ok if verification succeeds, error code otherwise.
 */
ra8_err_t ra8_app_verify(const uint8_t *binary, uint32_t len, const uint8_t public_key[32]);

#endif /* RA8_APP_VERIFY_H */

/**
 * @file ra8_rsip.h
 * @brief Renesas Secure IP (RSIP-E50D) HAL driver -- public API
 * @ingroup grp_hal_crypto
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
 * This file is a thin umbrella: the public API is split across
 * cohesive sub-headers, each self-contained and individually
 * includable, that this header pulls in so existing consumers
 * that ``#include "ra8_rsip.h"`` continue to see the full surface
 * unchanged:
 *
 * - ``ra8_rsip_core.h`` -- lifecycle, status / IRQ, TRNG, one-shot
 *   SHA-256, incremental SHA-256 / HMAC-SHA-256, power transition.
 * - ``ra8_rsip_keys.h`` -- wrapped-key handle type, key install
 *   (plaintext + OEM), AES block / authenticated modes, ChaCha20 +
 *   Poly1305, generic SHA-2 / SHA-3 / SHAKE hash family + HMAC,
 *   asymmetric RSA / ECDSA / ECDH.
 * - ``ra8_rsip_mgmt.h`` -- OEM boot loader version, wrapped-key vault,
 *   key wrap / unwrap, key derivation, device lifecycle + debug
 *   authorisation, tamper subsystem, DOTF key routing.
 *
 * The NSC veneer ``libs/ra8_nsc/src/ra8_nsc_key_vault.c`` calls into
 * the secure-side key vault layer (``src/secure_app/key_vault.c``)
 * which in turn drives this HAL; the veneer is unaware of the
 * register window and that mapping must remain stable.
 *
 * @warning The TRNG MUST pass its self-test (BIST) before any data
 *          read. ``ra8_rsip_init`` runs the BIST on the way up;
 *          callers that hand-build init sequences MUST replicate
 *          this. Reading the TRNG before BIST returns "OK" yields
 *          biased / predictable output and silently undermines
 *          every key derived from it.
 *
 * Every key-touching API takes opaque ``ra8_rsip_key_handle_t``
 * blobs -- raw key bytes never leave secure RAM. Each such API is
 * marked ``[[nodiscard]]`` so a forgotten error check is a build
 * failure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *            SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_rsip_core.h"
#include "ra8_rsip_keys.h"
#include "ra8_rsip_mgmt.h"

#ifdef __cplusplus
}
#endif

/**
 * @file ra8_rsip_regs.h
 * @brief Renesas Secure IP (RSIP-E50D) register layout for the RA8D2
 * @ingroup grp_hal_crypto
 *
 * @details
 * The RSIP-E50D engine on the RA8D2 is an isolated security
 * subsystem (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302). It
 * combines an access-management circuit, a symmetric core (AES +
 * ChaCha20-Poly1305), an asymmetric core (RSA + ECC), a hash engine
 * (SHA-2 / SHA-3 / HMAC), a 128-bit true RNG, the Hardware Unique
 * Key (HUK), the Unique ID, the OEM boot loader version counter,
 * and the key data path that feeds DOTF (HUM Ch 52.1 Table 52.1
 * p 3302-3303).
 *
 * Every functional sequence is driven through a small mailbox-style
 * register window starting at ``0x403B0000`` (FSP
 * ``RSIP_PRV_ADDR_BASE`` for the E50D variant). The HUM body is
 * intentionally short (p 3302-3307) because the on-board RSIP
 * firmware interprets opcodes the host pokes through the mailbox.
 *
 * ## Header organization
 *
 * This is a thin umbrella header. The declarations live in two
 * self-contained sub-headers in this directory and are pulled in
 * here so consumers keep including ``ra8_rsip_regs.h`` unchanged:
 *
 *  | Sub-header                     | Contents                              |
 *  |--------------------------------|---------------------------------------|
 *  | ra8_rsip_regs_offsets.h      | base addr, byte offsets, window enums,|
 *  |                                | layout-insurance static_asserts       |
 *  | ra8_rsip_regs_fields.h       | field/bit/mask/command-value enums,   |
 *  |                                | published sizes, ra8_rsip_reg32()      |
 *
 * Field semantics are inferred from the FSP primitives that touch
 * ``REG_xxxxH`` in
 * ``crypto_procedures_protected/src/rsip/ra/primitive/ra8_rsip_e50d/``;
 * the HUM does not publish a register table because the engine is
 * mailbox-driven.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_rsip_regs_fields.h"
#include "ra8_rsip_regs_offsets.h"

#ifdef __cplusplus
}
#endif

/**
 * @file ra_ota_internal.h
 * @brief Test-access surface for ra_ota internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions sitting in TU-private
 * helpers. See CLAUDE.md "Test access to internal symbols
 * (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @brief Parse a JSON-style ``"key": <decimal>`` field into a u32.
 *
 * @details Locates ``key`` in ``json``, skips through the colon /
 *          whitespace / quote run, then reads up to
 *          k_ra_ota_u32_decimal_digits decimal digits into ``*out_v``.
 *          Promoted from TU-private static linkage so tests can drive
 *          its line-403 3-condition OR-chain under -fcoverage-mcdc.
 *
 * @param[in]  json  NUL-terminated JSON document.
 * @param[in]  key   NUL-terminated key string to locate (e.g. ``"size"``).
 * @param[out] out_v Filled with the parsed value on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Parsed.
 * @retval k_ra_err_invalid_arg Key not found / no digits.
 *
 * @pre json, key, out_v are all non-NULL and NUL-terminated.
 * @pre Module is initialised.
 * @post On success ``*out_v`` holds the parsed value.
 * @post On failure ``*out_v`` is unchanged.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Exposes the line-403 ``(*p == ':') || (*p == ' ') || (*p == '"')``
 * skip-loop guard on production source.
 *
 * @since 0.1.0
 */
ra_err_t ra_ota_internal_json_u32(const char* json, const char* key, uint32_t* out_v);

#ifdef __cplusplus
}
#endif

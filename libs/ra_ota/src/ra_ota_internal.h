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
#include "ra_ota.h"

/**
 * @brief Validate the entire OTA configuration descriptor.
 *
 * @details Composes the net/crypto/flash sub-validators and verifies
 *          the manifest URL is non-empty. Promoted from TU-private
 *          static linkage so the parsing TU (``ra_ota_parse.c``) can
 *          own it while ``ra_ota_init`` in ``ra_ota.c`` keeps calling
 *          it. The single gate every public ``ra_ota_init`` call must
 *          pass before the module captures the config.
 *
 * @param[in] cfg Caller configuration (may be NULL -- checked here).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok              Configuration is valid.
 * @retval k_ra_err_null_ptr    ``cfg`` or a sub-pointer is NULL.
 * @retval k_ra_err_invalid_arg Bank size out of range or empty URL.
 *
 * @pre Module init is in progress (no concurrent OTA operation).
 * @pre Caller has not yet committed ``cfg`` to ``s_cfg``.
 * @post Returns k_ra_ok iff every required field is populated.
 * @post No module state mutated.
 *
 * @note Internal cross-TU helper; pure validation function.
 *
 * @par MC/DC:
 * Exposes the ``cfg->manifest_url[0] == '\0'`` empty-URL gate plus the
 * composed net/crypto/flash null-pointer checks on production source.
 *
 * @since 0.1.0
 */
ra_err_t ra_ota_internal_validate_cfg(const ra_ota_cfg_t* cfg);

/**
 * @brief Decode every field of a JSON manifest into an ``ra_ota_manifest_t``.
 *
 * @details Zeroes ``*out`` then pulls ``version``, ``url``, ``size``
 *          and the cryptographic fields. Promoted from TU-private
 *          static linkage so the parsing TU (``ra_ota_parse.c``) can
 *          own it while ``ra_ota_check_for_update`` in ``ra_ota.c``
 *          keeps calling it.
 *
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Destination struct (filled even on partial errors).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Manifest fully decoded.
 * @retval k_ra_err_invalid_arg   Required field missing or zero size.
 * @retval k_ra_err_invalid_size  Image size above firmware-wide cap.
 *
 * @pre Both pointers non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``*out`` is fully populated.
 * @post On failure ``*out`` may hold a partial decode.
 *
 * @note Internal cross-TU helper; pure function.
 *
 * @par MC/DC:
 * Exposes the size-bound gates ``image_size_bytes == 0`` and
 * ``image_size_bytes > k_ra_ota_max_image_bytes`` on production source.
 *
 * @since 0.1.0
 */
ra_err_t ra_ota_internal_manifest_decode(const char* json, ra_ota_manifest_t* out);

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
 * @pre Module is initialized.
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

/**
 * @brief Pure predicate: ASCII char is in inclusive range [lo, hi].
 *
 * @details Reusable for the [0-9] / [a-f] / [A-F] guards in
 *          @c priv_hex_nibble at libs/ra_ota/src/ra_ota.c lines
 *          449, 452, 455.
 *
 * @param[in] c  Character under test.
 * @param[in] lo Inclusive lower bound.
 * @param[in] hi Inclusive upper bound.
 *
 * @return Boolean in-range predicate.
 * @retval true  c is in [lo, hi].
 * @retval false c is outside.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - c<lo,        -> false (varies left from V2)
 *  - lo<=c<=hi,   -> true
 *  - c>hi,        -> false (varies right from V2)
 *
 * @since 0.1.0
 */
bool ra_ota_internal_char_in_range(char c, char lo, char hi);

/**
 * @brief Pure predicate: state is neither IDLE nor DOWNLOADING.
 *
 * @details Promoted from the inline AND at libs/ra_ota/src/ra_ota.c
 *          inside @c ra_ota_download_to_inactive_bank.
 *
 * @param[in] state_idle_val        Numeric value of @c k_ra_ota_state_idle.
 * @param[in] state_downloading_val Numeric value of @c k_ra_ota_state_downloading.
 * @param[in] state                 Candidate state value.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_invalid_state.
 * @retval false State permits the operation.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND of inequalities; N+1 = 3 vectors:
 *  - state=IDLE        -> false (left varies vs V3)
 *  - state=DOWNLOADING -> false (right varies vs V3)
 *  - state=ERROR       -> true  (both true)
 *
 * @since 0.1.0
 */
bool ra_ota_internal_download_state_invalid(uint32_t state_idle_val,
                                            uint32_t state_downloading_val,
                                            uint32_t state);

#ifdef __cplusplus
}
#endif

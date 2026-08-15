/**
 * @file ra8_ota_internal.h
 * @brief Test-access surface for ra8_ota internal helpers (MC/DC).
 * @ingroup grp_security
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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ota.h"

/**
 * @brief Validate the entire OTA configuration descriptor.
 *
 * @details Composes the net/crypto/flash sub-validators and verifies
 *          the manifest URL is non-empty. Promoted from TU-private
 *          static linkage so the parsing TU (``ra8_ota_parse.c``) can
 *          own it while ``ra8_ota_init`` in ``ra8_ota.c`` keeps calling
 *          it. The single gate every public ``ra8_ota_init`` call must
 *          pass before the module captures the config.
 *
 * @param[in] cfg Caller configuration (may be NULL -- checked here).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              Configuration is valid.
 * @retval k_ra8_err_null_ptr    ``cfg`` or a sub-pointer is NULL.
 * @retval k_ra8_err_invalid_arg Bank size out of range or empty URL.
 *
 * @pre Module init is in progress (no concurrent OTA operation).
 * @pre Caller has not yet committed ``cfg`` to ``s_cfg``.
 * @post Returns k_ra8_ok iff every required field is populated.
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
RA8_PRIV
ra8_err_t priv_ota_validate_cfg(const ra8_ota_cfg_t* cfg);

/**
 * @brief Decode every field of a JSON manifest into an ``ra8_ota_manifest_t``.
 *
 * @details Zeroes ``*out`` then pulls ``version``, ``url``, ``size``
 *          and the cryptographic fields. Promoted from TU-private
 *          static linkage so the parsing TU (``ra8_ota_parse.c``) can
 *          own it while ``ra8_ota_check_for_update`` in ``ra8_ota.c``
 *          keeps calling it.
 *
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Destination struct (filled even on partial errors).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                Manifest fully decoded.
 * @retval k_ra8_err_invalid_arg   Required field missing or zero size.
 * @retval k_ra8_err_invalid_size  Image size above firmware-wide cap.
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
 * ``image_size_bytes > k_ra8_ota_max_image_bytes`` on production source.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_ota_manifest_decode(const char* json, ra8_ota_manifest_t* out);

/**
 * @brief Parse a JSON-style ``"key": <decimal>`` field into a u32.
 *
 * @details Locates ``key`` in ``json``, skips through the colon /
 *          whitespace / quote run, then reads up to
 *          k_ra8_ota_u32_decimal_digits decimal digits into ``*out_v``.
 *          Promoted from TU-private static linkage so tests can drive
 *          its line-403 3-condition OR-chain under -fcoverage-mcdc.
 *
 * @param[in]  json  NUL-terminated JSON document.
 * @param[in]  key   NUL-terminated key string to locate (e.g. ``"size"``).
 * @param[out] out_v Filled with the parsed value on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Parsed.
 * @retval k_ra8_err_invalid_arg Key not found / no digits.
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
RA8_PRIV
ra8_err_t priv_ota_json_u32(const char* json, const char* key, uint32_t* out_v);

/**
 * @brief Pure predicate: ASCII char is in inclusive range [lo, hi].
 *
 * @details Reusable for the [0-9] / [a-f] / [A-F] guards in
 *          @c internal_hex_nibble at libs/ra8_ota/src/ra8_ota.c lines
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
RA8_PRIV
bool priv_ota_char_in_range(char c, char lo, char hi);

/**
 * @brief Pure predicate: state is neither IDLE nor DOWNLOADING.
 *
 * @details Promoted from the inline AND at libs/ra8_ota/src/ra8_ota.c
 *          inside @c ra8_ota_download_to_inactive_bank.
 *
 * @param[in] state_idle_val        Numeric value of @c k_ra8_ota_state_idle.
 * @param[in] state_downloading_val Numeric value of @c k_ra8_ota_state_downloading.
 * @param[in] state                 Candidate state value.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_invalid_state.
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
RA8_PRIV
bool priv_ota_download_state_invalid(uint32_t state_idle_val,
                                     uint32_t state_downloading_val,
                                     uint32_t state);

/* =============================================================================
 * Shared mutable module state.
 *
 * Defined exactly once in ra8_ota.c and reached by the verify cluster
 * (ra8_ota_verify.c) through these extern declarations. Each carries the
 * ``s_ra8_ota_`` module infix so the promoted (external-linkage) symbol is
 * link-unique across the codebase (the file-size-split ``s_<module>_`` rule).
 * ============================================================================= */

/**
 * @var g_ra8_ota_cfg
 * @brief Module configuration captured by ``ra8_ota_init``; defined in ``ra8_ota.c``.
 * @details Function-pointer interfaces (net / crypto / flash) plus URLs and bank
 *          metadata. Shared read-only with ``ra8_ota_verify.c`` so the verify
 *          cluster can reach the crypto interface and the public-key handle.
 * @note Internal mutable state; access from the single OTA owner context only.
 * @warning Do not redefine; exactly one definition exists in ``ra8_ota.c``.
 * @since 0.1.0
 */
extern ra8_ota_cfg_t g_ra8_ota_cfg;

/**
 * @var g_ra8_ota_state
 * @brief Current OTA state-machine value; defined in ``ra8_ota.c``.
 * @details Single-byte cooperative state shared with ``ra8_ota_verify.c`` so the
 *          verify entry point can gate on ``k_ra8_ota_state_verifying``.
 * @note Internal mutable state; single-owner access; single-byte reads are torn-free.
 * @warning Do not redefine; exactly one definition exists in ``ra8_ota.c``.
 * @since 0.1.0
 */
extern ra8_ota_state_t g_ra8_ota_state;

/**
 * @var g_ra8_ota_initialized
 * @brief True once ``ra8_ota_init`` has succeeded; defined in ``ra8_ota.c``.
 * @details Shared with ``ra8_ota_verify.c`` so the verify entry point can reject
 *          calls issued before the module is initialized.
 * @note Internal mutable state; single-owner access only.
 * @warning Do not redefine; exactly one definition exists in ``ra8_ota.c``.
 * @since 0.1.0
 */
extern bool g_ra8_ota_initialized;

/**
 * @var g_ra8_ota_buf
 * @brief Streaming chunk buffer reused by the manifest, download and re-hash
 *        paths; defined in ``ra8_ota.c``.
 * @details ``k_ra8_ota_chunk_bytes`` of static scratch shared with
 *          ``ra8_ota_verify.c`` so the re-hash pass reads the inactive bank back
 *          through it.
 * @note Internal mutable state; single-owner access only.
 * @warning Do not redefine; exactly one definition exists in ``ra8_ota.c``.
 * @since 0.1.0
 */
extern uint8_t g_ra8_ota_buf[k_ra8_ota_chunk_bytes];

/**
 * @brief Set the OTA state-machine value and fire the progress callback.
 *
 * @details Updates ``g_ra8_ota_state`` and the latched last-error, then
 *          synthesises a ``ra8_ota_progress_t`` snapshot and forwards it to the
 *          user-registered ``on_progress`` callback when one is present.
 *          Promoted from TU-private static linkage (was ``priv_set_state``) so
 *          the verify cluster (``ra8_ota_verify.c``) can drive state transitions
 *          while the orchestration TU (``ra8_ota.c``) owns the sole definition.
 *
 * @param[in] new_state New state-machine value to latch.
 * @param[in] err       Error to surface (``k_ra8_ok`` on healthy paths).
 *
 * @pre Module is initialized.
 * @pre Caller is the single OTA worker (no concurrent callers).
 * @post ``g_ra8_ota_state`` == ``new_state``.
 * @post The latched last-error equals ``err``.
 *
 * @note Internal cross-TU helper; not thread-safe -- the OTA module assumes a
 *       single owning context.
 * @since 0.1.0
 */
RA8_PRIV
void priv_ota_set_state(ra8_ota_state_t new_state, ra8_err_t err);

#ifdef __cplusplus
}
#endif

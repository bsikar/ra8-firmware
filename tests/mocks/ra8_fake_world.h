/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_world.h
 * @brief Host-side mock of the TrustZone NS / S boundary
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * host mock. Real Cortex-M85 hardware enforces the
 * Secure / Non-Secure split via the SAU; the host test build
 * has no such partition. ``ra8_fake_world`` lets a test pretend
 * the partition exists by tagging address ranges as Secure or
 * Non-Secure and exposing a check-equivalent function the
 * veneer test cases can call.
 *
 * Tests use ``ra8_fake_world_mark_ns(ptr, len)`` to declare a
 * region as "Non-Secure" and ``ra8_fake_world_mark_s(ptr, len)``
 * to declare a region as "Secure". A subsequent
 * ``ra8_fake_world_check_ns_range(ptr, len)`` returns whether the
 * range is fully NS-readable -- the host equivalent of
 * ``cmse_check_address_range``.
 *
 * The mock is ring-buffer backed (16 entries by default) so a
 * test can mark several regions at once and reset between cases.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_fake_world_limits_t
 */
typedef enum : uint8_t {
  k_ra8_fake_world_max_regions = 16U, /**< RA8 fake world maximum regions. */
} ra8_fake_world_limits_t;

/**
 * @enum ra8_fake_world_attr_t
 * @brief Attribute tag for a marked region.
 */
typedef enum : uint8_t {
  k_ra8_fake_world_attr_secure = 0U, /**< RA8 fake world attr secure. */
  k_ra8_fake_world_attr_ns     = 1U, /**< RA8 fake world attr ns.     */
} ra8_fake_world_attr_t;

/**
 * @brief Reset the region table to empty.
 *
 * @post No regions tagged.
 *
 * @note Test-only.
 * @since 0.1.0
 */
void ra8_fake_world_reset(void);

/**
 * @brief Tag ``[ptr, ptr+len)`` as Non-Secure.
 *
 * @param[in] ptr Region base.
 * @param[in] len Region length in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Region tagged.
 * @retval k_ra8_err_no_mem Region table full.
 *
 * @pre Test is running under host build.
 * @post Subsequent ``check_ns_range`` queries see the region.
 *
 * @note Test-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_world_mark_ns(const void* ptr, uint32_t len);

/**
 * @brief Tag ``[ptr, ptr+len)`` as Secure.
 *
 * @param[in] ptr Region base.
 * @param[in] len Region length in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Region tagged.
 * @retval k_ra8_err_no_mem Region table full.
 *
 * @note Test-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_world_mark_s(const void* ptr, uint32_t len);

/**
 * @brief Check whether ``[ptr, ptr+len)`` lies entirely in NS regions.
 *
 * @param[in] ptr Address to check.
 * @param[in] len Range length.
 * @return ``true`` if every byte is in an NS-tagged region,
 * ``false`` otherwise (Secure overlap or untagged).
 *
 * @note This is the host equivalent of
 * ``cmse_check_address_range(ptr, len, CMSE_NONSECURE)``.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_fake_world_check_ns_range(const void* ptr, uint32_t len);

#ifdef __cplusplus
}
#endif

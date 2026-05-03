/**
 * @file ra_modem_at_internal.h
 * @brief Test-access surface for ra_modem_at internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_modem_at facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Result of processing a single completed line.
 *
 * @details Mirror of the TU-local enum in ra_modem_at.c. Exposed so
 *          tests can read the return value of internal_classify().
 */
typedef enum : uint8_t {
  k_ra_modem_line_kind_empty     = 0U,
  k_ra_modem_line_kind_echo      = 1U,
  k_ra_modem_line_kind_urc       = 2U,
  k_ra_modem_line_kind_final_ok  = 3U,
  k_ra_modem_line_kind_final_err = 4U,
  k_ra_modem_line_kind_payload   = 5U,
} ra_modem_line_kind_t;

/**
 * @brief Classify a complete (NUL-terminated) line for the FSM.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-344 compound decision
 *          ``(cmd_echo != nullptr) && (str_eq(line, cmd_echo) != 0U)``
 *          under -fcoverage-mcdc. Production callers MUST keep using
 *          the public ra_modem_at facade.
 *
 * @param[in] line              NUL-terminated input line.
 * @param[in] cmd_echo          Optional expected command echo (NULL = ignore).
 * @param[in] expected_response Optional expected response prefix.
 *
 * @return ra_modem_line_kind_t Classification.
 * @retval k_ra_modem_line_kind_empty Line was empty.
 * @retval k_ra_modem_line_kind_echo  Line matched cmd_echo exactly.
 * @retval other Other classification (URC / final / payload).
 *
 * @pre line is non-NULL and NUL-terminated.
 * @pre Module state is consistent.
 * @post No state mutation outside of the URC dispatch path.
 * @post Return value reflects the input line.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Exposed so tests exercise the line-344 short-circuit AND on the
 * production source.
 *
 * @since 0.1.0
 */
ra_modem_line_kind_t ra_modem_at_internal_classify(const char* line,
                                                   const char* cmd_echo,
                                                   const char* expected_response);

/**
 * @brief NUL-terminated string length, capped at UINT16_MAX.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-124 compound decision under -fcoverage-mcdc.
 *          Production callers must keep using the public facade.
 *
 * @param[in] s NUL-terminated input string (must be non-NULL).
 * @return Length in bytes (0..UINT16_MAX-1).
 * @retval 0 ``s[0]`` is the NUL terminator (empty string).
 *
 * @pre s is non-NULL.
 * @pre s is NUL-terminated within UINT16_MAX bytes.
 * @post No state mutated.
 * @post Return value matches the position of the first NUL byte.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Drives line 124 ``(i < UINT16_MAX) && (s[i] != '\0')``.
 *
 * @since 0.1.0
 */
uint16_t ra_modem_at_internal_str_len(const char* s);

/**
 * @brief Return 1 iff ``hay`` begins with ``needle``.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          the inner compare branch on the production source.
 *
 * @param[in] hay    NUL-terminated haystack.
 * @param[in] needle NUL-terminated prefix.
 * @return 1 on match, 0 otherwise.
 * @retval 1 ``hay`` begins with ``needle`` (or ``needle`` is empty).
 * @retval 0 ``needle`` differs from the leading bytes of ``hay``.
 *
 * @pre hay is non-NULL and NUL-terminated.
 * @pre needle is non-NULL and NUL-terminated.
 * @post No state mutated.
 * @post Return value reflects byte-wise prefix comparison.
 *
 * @note Test-access only.
 *
 * @since 0.1.0
 */
uint8_t ra_modem_at_internal_starts_with(const char* hay, const char* needle);

/**
 * @brief Return 1 iff ``a`` and ``b`` are byte-for-byte equal.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          the line-177 loop AND and the line-184 terminator AND on
 *          the production source under -fcoverage-mcdc.
 *
 * @param[in] a NUL-terminated string.
 * @param[in] b NUL-terminated string.
 * @return 1 if equal, 0 otherwise.
 * @retval 1 Both strings are byte-for-byte equal including length.
 * @retval 0 Strings differ in length or any byte.
 *
 * @pre a is non-NULL and NUL-terminated.
 * @pre b is non-NULL and NUL-terminated.
 * @post No state mutated.
 * @post Return value reflects exact byte-wise equality.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Drives lines 177 ``(a[i] != '\0') && (b[i] != '\0')`` and
 * 184 ``(a[i] == '\0') && (b[i] == '\0')``.
 *
 * @since 0.1.0
 */
uint8_t ra_modem_at_internal_str_eq(const char* a, const char* b);

#ifdef __cplusplus
}
#endif

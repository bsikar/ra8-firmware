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

#ifdef __cplusplus
}
#endif

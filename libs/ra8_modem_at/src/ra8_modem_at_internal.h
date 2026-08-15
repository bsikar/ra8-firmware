/**
 * @file ra8_modem_at_internal.h
 * @brief Test-access surface for ra8_modem_at internal helpers (MC/DC).
 * @ingroup grp_net
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra8_modem_at facade. See CLAUDE.md
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

#include "ra8_attributes.h"

/**
 * @brief Result of processing a single completed line.
 *
 * @details Mirror of the TU-local enum in ra8_modem_at.c. Exposed so
 *          tests can read the return value of internal_classify().
 */
typedef enum : uint8_t {
  k_ra8_modem_line_kind_empty     = 0U, /**< RA8 modem line kind empty.       */
  k_ra8_modem_line_kind_echo      = 1U, /**< RA8 modem line kind echo.        */
  k_ra8_modem_line_kind_urc       = 2U, /**< RA8 modem line kind urc.         */
  k_ra8_modem_line_kind_final_ok  = 3U, /**< RA8 modem line kind final ok.    */
  k_ra8_modem_line_kind_final_err = 4U, /**< RA8 modem line kind final error. */
  k_ra8_modem_line_kind_payload   = 5U, /**< RA8 modem line kind payload.     */
} ra8_modem_line_kind_t;

/**
 * @brief Classify a complete (NUL-terminated) line for the FSM.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-344 compound decision
 *          ``(cmd_echo != nullptr) && (str_eq(line, cmd_echo) != 0U)``
 *          under -fcoverage-mcdc. Production callers MUST keep using
 *          the public ra8_modem_at facade.
 *
 * @param[in] line              NUL-terminated input line.
 * @param[in] cmd_echo          Optional expected command echo (NULL = ignore).
 * @param[in] expected_response Optional expected response prefix.
 *
 * @return ra8_modem_line_kind_t Classification.
 * @retval k_ra8_modem_line_kind_empty Line was empty.
 * @retval k_ra8_modem_line_kind_echo  Line matched cmd_echo exactly.
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
RA8_PRIV
ra8_modem_line_kind_t
priv_modem_classify(const char* line, const char* cmd_echo, const char* expected_response);

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
RA8_PRIV
uint16_t priv_modem_str_len(const char* s);

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
RA8_PRIV
uint8_t priv_modem_starts_with(const char* hay, const char* needle);

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
RA8_PRIV
uint8_t priv_modem_str_eq(const char* a, const char* b);

#include <stddef.h>

/**
 * @brief Append a NUL-terminated line into ``capture`` (with newline sep).
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          the line-469 NULL+len AND-decision under -fcoverage-mcdc.
 *          Production callers must keep using the public facade.
 *
 * @param[in]     line        NUL-terminated line to append (must be non-NULL).
 * @param[in,out] capture     Caller-owned buffer (NULL allowed; no-op).
 * @param[in]     capture_len Capacity of @p capture in bytes.
 * @param[in,out] used        Bytes already populated; updated on append.
 *
 * @pre line is non-NULL and NUL-terminated.
 * @pre used is non-NULL when capture is non-NULL.
 * @post No write occurs when capture is NULL or capture_len is 0.
 * @post Otherwise, *used is monotonically non-decreasing.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Drives line 469 ``(capture == nullptr) || (capture_len == 0U)``.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_modem_capture_line(const char* line, char* capture, size_t capture_len, size_t* used);

/**
 * @brief Pure (state-free) reimplementation of @c internal_reset_line's
 *        line-227 buffer-clear guard.
 *
 * @details Returns true iff @p line_buf is non-NULL and
 *          @p line_buf_len > 0 -- the exact decision exercised by the
 *          production helper @c internal_reset_line at line 227. The
 *          state-reading wrapper passes through @c s_mod.cfg.line_buf /
 *          @c s_mod.cfg.line_buf_len, both of which are gated by the
 *          init validator. This pure sibling exists so tests can drive
 *          the AND under -fcoverage-mcdc with all four input
 *          combinations.
 *
 * @param[in] line_buf     Caller-owned line buffer (NULL when absent).
 * @param[in] line_buf_len Capacity of @p line_buf in bytes.
 *
 * @return Boolean predicate.
 * @retval true  Both arguments are non-NULL / non-zero (would clear).
 * @retval false Either argument signals "no buffer installed".
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives line 227 ``(s_mod.cfg.line_buf != nullptr) &&
 * (s_mod.cfg.line_buf_len > 0U)`` (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_modem_reset_line_should_clear(const void* line_buf, uint16_t line_buf_len);

/**
 * @brief Pure sibling of the line-573 payload-prefix AND-decision.
 *
 * @details Returns true iff @p expected_response is non-NULL, non-empty, and
 *          @p line begins with @p expected_response. This mirrors the
 *          three-condition short-circuit AND in @c internal_handle_line at
 *          line 573 of @c ra8_modem_at.c
 *          (``(expected_response != nullptr) && (expected_response[0] != '\0')
 *          && (priv_modem_starts_with(line, expected_response) !=
 *          0U)``). The production helper consults @c s_mod state to mutate
 *          ``seen_exp``; this pure sibling exposes the boolean predicate
 *          alone so all four short-circuit MC/DC vectors are reachable from
 *          a host test.
 *
 * @param[in] line              NUL-terminated input line (must be non-NULL).
 * @param[in] expected_response Optional NUL-terminated expected prefix.
 *
 * @return Boolean predicate.
 * @retval 1 All three conditions hold (line begins with the prefix).
 * @retval 0 Any one of the three conditions fails (short-circuited).
 *
 * @pre line is non-NULL and NUL-terminated.
 * @pre No state mutation.
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives line 573 of ra8_modem_at.c (3 conditions, AND; N+1 = 4 vectors).
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_modem_payload_prefix_matches(const char* line, const char* expected_response);

/**
 * @brief Pure sibling of the line-664 capture-buffer-init AND-decision.
 *
 * @details Returns true iff both @p capture is non-NULL and @p capture_len is
 *          greater than zero, mirroring the AND-decision in
 *          @c internal_wait_response at line 664 of @c ra8_modem_at.c
 *          (``(capture != nullptr) && (capture_len > 0U)``). The production
 *          decision controls a single byte-clear of ``capture[0] = '\\0'``;
 *          all four input combinations are exposed here so MC/DC can be
 *          driven from a host test (the public ``send_cmd_capture`` API
 *          rejects ``capture_len == 0`` at the entry guard before
 *          @c internal_wait_response is reached).
 *
 * @param[in] capture     Caller-owned capture buffer (NULL allowed).
 * @param[in] capture_len Capacity of @p capture in bytes.
 *
 * @return Boolean predicate.
 * @retval 1 capture is non-NULL AND capture_len > 0.
 * @retval 0 Either condition fails.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives line 664 of ra8_modem_at.c (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_modem_capture_should_clear(const void* capture, size_t capture_len);

#ifdef __cplusplus
}
#endif

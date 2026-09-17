/**
 * @file usb_selftest_console.h
 * @brief Shared SCI8 console + text formatters for the soak self-test
 *
 * @details
 * The host worker prints all its verdicts through these helpers, which wrap
 * polled SCI8 (the J-Link OB CDC bridge) with heap-free decimal / hex / fail
 * formatters. Defined in usb_selftest_console.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @param[in] text NUL-terminated ASCII (bounded by ::k_selftest_print_cap).
 * @return ra8_err_t passthrough from the SCI write.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print(const char* text);

/**
 * @brief Print an unsigned 32-bit value in decimal.
 *
 * @param[in] value Value to print (no leading zeros; "0" for zero).
 * @return ra8_err_t passthrough from the SCI write.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_dec(uint32_t value);

/**
 * @brief Print the low @p digits hex nibbles of @p value (upper-case).
 *
 * @param[in] value  Value to print.
 * @param[in] digits Number of hex digits to emit (e.g. 4 or 8).
 * @return ra8_err_t passthrough from the SCI write.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print a "FAIL <what> err=0x..." diagnostic line.
 *
 * @param[in] what Short ASCII label for the failing step.
 * @param[in] err  Error code to render in hex.
 * @return ra8_err_t passthrough from the SCI writes.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_fail(const char* what, ra8_err_t err);

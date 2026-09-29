/**
 * @file log_fixture.h
 * @brief Test-only declarations for the ra8_wifi ABI log fixture.
 *
 * @details
 * The Zig ABI tests link this fixture instead of the production log sink so
 * rejected-pointer diagnostics can be inspected without hardware or global
 * logging state. Every declaration is test-only and must remain unreachable
 * from production translation units.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_log.h"

/**
 * @brief Clear the captured ra8_wifi diagnostic.
 *
 * @details Resets the recorded length; the backing bytes are ignored afterward.
 *
 * @par MC/DC:
 * Test setup helper; it contains no decision and creates the empty baseline
 * used by each ABI test's diagnostic assertion.
 *
 * @pre Called only by a serialized host test.
 * @pre No concurrent log capture is in progress.
 * @post The captured length is zero.
 * @post The backing buffer remains allocated for the process lifetime.
 * @note Not thread-safe; test-only state is shared by the fixture functions.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_wifi_test_reset_log(void);

/**
 * @brief Return the captured diagnostic bytes.
 *
 * @details The returned pointer addresses static fixture storage and is not
 * NUL-terminated by this accessor; pair it with
 * ra8_wifi_test_last_log_len().
 *
 * @par MC/DC:
 * Test observation helper; it contains no decision and exposes the bytes
 * captured by ra8_log_emit_error().
 *
 * @return Pointer to the captured diagnostic buffer.
 * @retval non-null Always: the fixture buffer has static storage duration.
 *
 * @pre Called only by a serialized host test.
 * @pre The fixture object remains linked into the ABI test executable.
 * @post No fixture state is modified.
 * @post The returned pointer remains valid for the process lifetime.
 * @note Not thread-safe; read only after the facade call returns.
 * @since 0.1.0
 */
RA8_TEST_HELPER const unsigned char* ra8_wifi_test_last_log(void);

/**
 * @brief Return the number of captured diagnostic bytes.
 *
 * @details The count is bounded by the fixture buffer capacity.
 *
 * @par MC/DC:
 * Test observation helper; it contains no decision and pairs with
 * ra8_wifi_test_last_log() to form a bounded byte slice.
 *
 * @return Number of captured bytes, in the inclusive range 0 through 64.
 * @retval 0 No diagnostic has been captured since the last reset.
 * @retval 1..64 A truncated or complete diagnostic is available.
 *
 * @pre Called only by a serialized host test.
 * @pre The fixture object remains linked into the ABI test executable.
 * @post No fixture state is modified.
 * @post The result does not exceed the fixture buffer capacity.
 * @note Not thread-safe; read only after the facade call returns.
 * @since 0.1.0
 */
RA8_TEST_HELPER size_t ra8_wifi_test_last_log_len(void);

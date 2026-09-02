/**
 * @file ra8_usb_cdc_internal.h
 * @brief Test-access surface for the USB CDC line-coding decoder.
 *
 * @details Declares the line-coding decoder so host tests can exercise its
 * compound input guard without manufacturing an asynchronous EP0 OUT packet.
 * Production callers continue to reach it only through the CDC SETUP handler.
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
 * @brief Drive the private SET_LINE_CODING decoder from host tests.
 *
 * @details Forwards the caller-owned payload to the same private decoder used
 * by the CDC SETUP handler. This is a test-access seam only; production code
 * continues to call the private helper directly.
 *
 * @param[in] data Pointer to the line-coding payload in host byte order; may
 *                 be null to exercise the reject path.
 * @param[in] len  Length of @p data in bytes.
 *
 * @pre The CDC state has been initialized by `ra8_usb_cdc_init()`.
 * @pre When @p data is non-null, it references at least @p len readable bytes.
 * @post A valid seven-byte payload replaces every cached line-coding field.
 * @post A null or short payload leaves the cached line coding unchanged.
 *
 * @note Not thread-safe; production calls originate in the USB control path.
 *
 * @par MC/DC:
 * Decision `(data == nullptr) || (len < 7)` uses N+1 = 3 vectors:
 * - non-null data, length 7: false control vector
 * - null data, length 7: first condition independently makes the result true
 * - non-null data, length 6: second condition independently makes it true
 *
 * @since 0.1.0
 */
RA8_TEST_HELPER
void ra8_usb_cdc_test_apply_line_coding(const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif

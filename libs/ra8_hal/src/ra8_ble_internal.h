/**
 * @file ra8_ble_internal.h
 * @brief Test-access surface for the BLE loopback transport.
 * @details Declares the bounded capture and injection hooks used to verify HCI
 *          framing and receive dispatch without exposing them as public HAL
 *          operations. Production code must use the API in `ra8_ble.h`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief Reset the BLE loopback capture and injection cursors.
 * @details Clears the recorded transmit length and discards every unread byte
 *          in the synthetic receive buffer without changing open state or
 *          registered callbacks.
 * @pre The linked BLE implementation provides its loopback test hooks.
 * @pre No concurrent BLE operation is reading or modifying loopback state.
 * @post The transmit capture length is zero.
 * @post The receive injection length and read position are zero.
 * @note Test-only hook; not thread-safe with any other BLE operation.
 * @par MC/DC:
 * This decision-free reset supplies the known-empty control state used by the
 * HCI framing and injection vectors.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER void ra8_ble_test_reset_capture(void);

/**
 * @brief Replace the synthetic BLE receive input with bounded caller bytes.
 * @details Rejects a null source or zero length without changing state. A
 *          longer source is truncated to ::k_ra8_ble_rx_inject_bytes, copied
 *          into module-owned storage, and made readable from position zero.
 * @param[in] bytes Source bytes, or nullptr for the rejected-input vector.
 * @param[in] len Readable source extent in bytes; values above the injection
 *                capacity are truncated.
 * @pre When non-null, @p bytes addresses at least @p len readable bytes.
 * @pre No concurrent BLE operation is reading or modifying loopback state.
 * @post Rejected input leaves the existing receive injection state unchanged.
 * @post Accepted input replaces the receive bytes, publishes the bounded
 *       length, and resets the read position to zero.
 * @note Test-only hook; it copies input and retains no caller-owned pointer.
 * @par MC/DC:
 * The `(bytes == nullptr) || (len == 0)` guard uses three vectors: non-null
 * and nonzero continues, null and nonzero returns, and non-null and zero
 * returns. These demonstrate each condition's independent influence.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER void ra8_ble_test_inject_rx(const uint8_t* bytes, uint16_t len);

/**
 * @brief Borrow the BLE transmit capture and optionally report its length.
 * @details Returns the stable module-owned capture base and writes the number
 *          of valid leading bytes only when @p out_len is non-null.
 * @param[out] out_len Optional destination for the captured byte count.
 * @return Read-only pointer to the module-owned transmit capture.
 * @retval non-null The fixed capture buffer base; ownership remains internal.
 * @pre When non-null, @p out_len addresses writable `uint16_t` storage.
 * @pre No concurrent BLE operation is appending to the capture.
 * @post A non-null @p out_len contains the current capture length.
 * @post The capture bytes and all BLE module state remain unchanged.
 * @note Test-only borrowed view; valid for the module lifetime and mutable by
 *       later BLE transmit or reset operations.
 * @par MC/DC:
 * Tests call with a writable length pointer and with nullptr to cover both
 * outcomes of the optional-output decision.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);

#ifdef __cplusplus
}
#endif

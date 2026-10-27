/**
 * @file uart_stub_smoke.c
 * @brief Cross-target smoke TU for the stub UART adapter's header.
 *
 * @par Tag
 * [Ring 2 / Test] {World: Any}
 *
 * @details
 * Including `fw_if_uart_stub.h` alone is the test: the header must pull in
 * everything it names, and the static assertions pin the facts the stub
 * relies on. Nothing here links; the archive link is the composition root's
 * business.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_uart_stub.h"

_Static_assert(K_FW_UART_DATA_BITS_COUNT == 3U, "data bits enum drifted");
_Static_assert(K_FW_UART_PARITY_COUNT == 3U, "parity enum drifted");
_Static_assert(K_FW_UART_STOP_BITS_COUNT == 3U, "stop bits enum drifted");
_Static_assert(sizeof(fw_uart_config_t) == 8U, "uart config layout drifted");
_Static_assert(offsetof(fw_uart_config_t, baud) == 0U, "uart config layout drifted");
_Static_assert(offsetof(fw_uart_config_t, data_bits) == 4U, "uart config layout drifted");
_Static_assert(offsetof(fw_uart_config_t, parity) == 5U, "uart config layout drifted");
_Static_assert(offsetof(fw_uart_config_t, stop_bits) == 6U, "uart config layout drifted");
_Static_assert(offsetof(fw_uart_t, iface) == 0U, "stub handle layout drifted");
_Static_assert(offsetof(fw_uart_t, ctx) == sizeof(const fw_uart_iface_t *), "stub handle layout drifted");
_Static_assert(offsetof(fw_uart_t, bound) == sizeof(const fw_uart_iface_t *) + sizeof(void *),
    "stub handle layout drifted");

int main(void) { return 0; }

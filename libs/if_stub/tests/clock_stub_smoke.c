/**
 * @file clock_stub_smoke.c
 * @brief Cross-target smoke TU for the stub clock adapter's header.
 *
 * @par Tag
 * [Ring 2 / Test] {World: Any}
 *
 * @details
 * Including `fw_if_clock_stub.h` alone is the test: the header must pull in
 * everything it names, and the static assertions pin the two facts the stub
 * relies on. Nothing here links; the archive link is the composition root's
 * business.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_clock_stub.h"

_Static_assert(K_FW_CLOCK_MODULE_KIND_COUNT == 20U, "module-kind count drifted");
_Static_assert(sizeof(fw_clock_stub_row_t) == 8U, "stub row layout drifted");
_Static_assert(offsetof(fw_clock_stub_row_t, rate_hz) == 0U, "stub row layout drifted");

int main(void) { return 0; }

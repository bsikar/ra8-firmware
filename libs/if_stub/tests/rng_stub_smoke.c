/**
 * @file rng_stub_smoke.c
 * @brief Cross-target smoke TU for the stub random-bytes adapter's header.
 *
 * @par Tag
 * [Ring 2 / Test] {World: Any}
 *
 * @details
 * Including `fw_if_rng_stub.h` alone is the test: the header must pull in
 * everything it names, and the static assertions pin the two facts the stub
 * relies on. Nothing here links; the archive link is the composition root's
 * business.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_rng_stub.h"

_Static_assert(offsetof(fw_rng_t, iface) == 0U, "stub handle layout drifted");
_Static_assert(offsetof(fw_rng_t, ctx) == sizeof(const fw_rng_iface_t *), "stub handle layout drifted");
_Static_assert(offsetof(fw_rng_t, bound) == sizeof(const fw_rng_iface_t *) + sizeof(void *),
    "stub handle layout drifted");

int main(void) { return 0; }

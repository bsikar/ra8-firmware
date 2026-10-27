/**
 * @file gpio_stub_smoke.c
 * @brief Cross-target smoke TU for the stub GPIO adapter's header.
 *
 * @par Tag
 * [Ring 2 / Test] {World: Any}
 *
 * @details
 * Including `fw_if_gpio_stub.h` alone is the test: the header must pull in
 * everything it names, and the static assertions pin the facts the stub
 * relies on. Nothing here links; the archive link is the composition root's
 * business.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_gpio_stub.h"

_Static_assert(K_FW_GPIO_DIRECTION_COUNT == 3U, "direction enum drifted");
_Static_assert(K_FW_GPIO_PULL_COUNT == 3U, "pull enum drifted");
_Static_assert(sizeof(fw_gpio_pin_cfg_t) == 2U, "pin cfg layout drifted");
_Static_assert(offsetof(fw_gpio_pin_cfg_t, direction) == 0U, "pin cfg layout drifted");
_Static_assert(offsetof(fw_gpio_pin_cfg_t, pull) == 1U, "pin cfg layout drifted");
_Static_assert(offsetof(fw_gpio_t, iface) == 0U, "stub handle layout drifted");
_Static_assert(offsetof(fw_gpio_t, ctx) == sizeof(const fw_gpio_iface_t *), "stub handle layout drifted");
_Static_assert(offsetof(fw_gpio_t, bound) == sizeof(const fw_gpio_iface_t *) + sizeof(void *),
    "stub handle layout drifted");

int main(void) { return 0; }

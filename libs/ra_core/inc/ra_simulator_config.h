/**
 * @file ra_simulator_config.h
 * @brief Guards for host-compiled unit tests
 *
 * @details
 * Every firmware .c file that touches a peripheral register block
 * compiles on both the arm-none-eabi cross build (target) and the
 * x86_64 host build (tests). On the host there is no real hardware;
 * the test harness provides mock register pages backed by ordinary
 * SRAM. The switch between the two is a single preprocessor flag
 * added to the compile command for the test build:
 *
 *     -DRA_SIMULATOR_MODE
 *
 * Code under `#ifdef RA_SIMULATOR_MODE` is allowed to use host-only
 * facilities (stdio, malloc via Unity's heap, pthread) because those
 * paths never compile into the real firmware.
 *
 * This header does not define `RA_SIMULATOR_MODE` itself; it only
 * lifts the guards into the `k_ra_*` enum space so clang-tidy's
 * `readability-magic-numbers` check does not flag them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uint8_t {
#ifdef RA_SIMULATOR_MODE
  k_ra_simulator_mode = 1U, /**< Host unit-test build. */
#else
  k_ra_simulator_mode = 0U, /**< Arm-none-eabi firmware build. */
#endif
} ra_simulator_mode_t;

#ifdef __cplusplus
}
#endif

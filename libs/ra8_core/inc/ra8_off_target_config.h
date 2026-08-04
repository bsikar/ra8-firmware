/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_off_target_config.h
 * @brief Guards for host-compiled unit tests
 * @ingroup grp_core
 *
 * @details
 * Every firmware .c file that touches a peripheral register block
 * compiles on both the arm-none-eabi cross build (target) and the
 * x86_64 host build (tests). On the host there is no real hardware;
 * the test harness provides mock register pages backed by ordinary
 * SRAM. The switch between the two is a single preprocessor flag
 * added to the compile command for the test build:
 *
 *     -DRA8_OFF_TARGET
 *
 * Code under `#ifdef RA8_OFF_TARGET` is allowed to use host-only
 * facilities (stdio, malloc via Unity's heap, pthread) because those
 * paths never compile into the real firmware.
 *
 * This header does not define `RA8_OFF_TARGET` itself; it only
 * lifts the guards into the `k_ra8_*` enum space so clang-tidy's
 * `readability-magic-numbers` check does not flag them.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uint8_t {
#ifdef RA8_OFF_TARGET
  k_ra8_off_target = 1U, /**< Host unit-test build. */
#else
  k_ra8_off_target = 0U, /**< Arm-none-eabi firmware build. */
#endif /**< (anon). */
} ra8_off_target_t;

#ifdef __cplusplus
}
#endif

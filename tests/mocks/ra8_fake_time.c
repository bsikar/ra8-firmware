/**
 * @file ra8_fake_time.c
 * @brief Host-test deterministic time source implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details Supplies deterministic monotonic time and explicit advancement to
 * host tests while retaining the production time-provider interface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA8_OFF_TARGET

#include "ra8_fake_time.h"

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_time.h"

/**
 * @enum ra8_fake_time_cpu_hz_t
 * @brief Arbitrary non-zero CPU frequency handed to ``ra8_time_init``.
 *
 * @details
 * ``ra8_time_init`` rejects ``0``. Any real frequency that passes
 * the ``(cpu_hz / 1000) - 1 != 0`` check works; 1 MHz is
 * documented as a sentinel "host test" value.
 */
typedef enum : uint32_t {
  k_ra8_fake_time_cpu_hz = 1000000U, /**< RA8 fake time CPU Hz. */
} ra8_fake_time_cpu_hz_t;

void ra8_fake_time_reset(void)
{
  (void)ra8_time_init((uint32_t)k_ra8_fake_time_cpu_hz);
}

void ra8_fake_time_advance_ms(uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    ra8_time_on_tick();
  }
}

#else
/* On-target build: this translation unit is empty. */
typedef int ra8_fake_time_placeholder_t;
#endif /* RA8_OFF_TARGET */

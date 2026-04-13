/**
 * @file ra_sim_time.c
 * @brief Host-test deterministic time source implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA_SIMULATOR_MODE

#include "ra_sim_time.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_time.h"

/**
 * @enum ra_sim_time_cpu_hz_t
 * @brief Arbitrary non-zero CPU frequency handed to ``ra_time_init``.
 *
 * @details
 * ``ra_time_init`` rejects ``0``. Any real frequency that passes
 * the ``(cpu_hz / 1000) - 1 != 0`` check works; 1 MHz is
 * documented as a sentinel "host test" value.
 */
typedef enum : uint32_t {
  k_ra_sim_time_cpu_hz = 1000000U,
} ra_sim_time_cpu_hz_t;

void ra_sim_time_reset(void)
{
  (void)ra_time_init((uint32_t)k_ra_sim_time_cpu_hz);
}

void ra_sim_time_advance_ms(uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    ra_time_on_tick();
  }
}

#else
/* Non-simulator build: this translation unit is empty. */
typedef int ra_sim_time_placeholder_t;
#endif /* RA_SIMULATOR_MODE */

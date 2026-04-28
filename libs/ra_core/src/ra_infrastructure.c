/**
 * @file ra_infrastructure.c
 * @brief Application-layer bring-up (called from main() after SystemInit)
 *
 * @details
 * Runs after `SystemInit()` -- which already handled VTOR, FPU,
 * lazy stacking, I-cache, D-cache, branch predictor, and priority
 * grouping. This function only covers the *application* layer:
 *
 *  - Log backend initialisation.
 *  - Pin validator reset.
 *  - Stack canary sentinel write (so later overflow checks have a
 *    reference to compare against).
 *
 * Anything that depends on a peripheral clock lives in a driver
 * `_init()` entry point, not here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra_infrastructure.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_log.h"
#include "ra_pin_validator.h"

/* =============================================================================
 * Stack canary sentinel
 * =============================================================================
 */

#ifndef RA_SIMULATOR_MODE
extern uint32_t g_ra_ls_stack_canary_start[];
extern uint32_t g_ra_ls_stack_canary_end[];
#endif

typedef enum : uint32_t {
  k_ra_stack_canary_pattern = 0xDEADBEEFUL,
} ra_stack_canary_pattern_t;

/**
 * @brief Fill `.stack_canary` with the sentinel pattern.
 */
static void internal_write_stack_canary(void)
{
#ifndef RA_SIMULATOR_MODE
  for (uint32_t* w = g_ra_ls_stack_canary_start; w < g_ra_ls_stack_canary_end; w++) {
    *w = (uint32_t)k_ra_stack_canary_pattern;
  }
#endif
}

void ra_infrastructure_init(void)
{
  /* Logging backend first so the rest of the init flow can emit. */
  ra_log_init();

  /* Clear pin-ownership bookkeeping. */
  ra_pin_validator_reset();

  /* Seed the stack overflow sentinel. */
  internal_write_stack_canary();

  ra_log_info("INFRA", "infrastructure ready");
}

ra_err_t ra_stack_canary_check(void)
{
#ifndef RA_SIMULATOR_MODE
  for (uint32_t* w = g_ra_ls_stack_canary_start; w < g_ra_ls_stack_canary_end; w++) {
    if (*w != (uint32_t)k_ra_stack_canary_pattern) {
      return k_ra_err_validation_failed;
    }
  }
#endif
  return k_ra_ok;
}

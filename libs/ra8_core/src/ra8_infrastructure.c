/**
 * @file ra8_infrastructure.c
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
 * @since 0.1.0
 */

#include "ra8_infrastructure.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_pin_validator.h"

/* =============================================================================
 * Stack canary sentinel
 * =============================================================================
 */

#ifndef RA8_OFF_TARGET
extern uint32_t g_ra8_ls_stack_canary_start[];
extern uint32_t g_ra8_ls_stack_canary_end[];
#endif

typedef enum : uint32_t {
  k_ra8_stack_canary_pattern = 0xDEADBEEFUL, /**< RA8 stack canary pattern. */
} ra8_stack_canary_pattern_t;

/**
 * @brief Fill `.stack_canary` with the sentinel pattern.
 *
 * @details Walks the linker-defined region and writes
 *          `k_ra8_stack_canary_pattern` into every word. No-op on the
 *          fake host where the linker symbols are absent.
 *
 * @pre Linker-defined symbols are valid (target build only).
 * @pre Function is called once during early init.
 * @post Every 32-bit word in the canary region equals the sentinel.
 * @post No other state modified.
 *
 * @note Not thread-safe; intended for one-shot init only.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_stack_canary(void)
{
#ifndef RA8_OFF_TARGET
  for (uint32_t* w = g_ra8_ls_stack_canary_start; w < g_ra8_ls_stack_canary_end; w++) {
    *w = (uint32_t)k_ra8_stack_canary_pattern;
  }
#endif
}

/**
 * @brief Implementation of `ra8_infrastructure_init()` -- bring-up.
 *
 * @details Initialises the log backend, resets the pin validator, and
 *          seeds the stack canary region.
 *
 * @pre `SystemInit()` has executed.
 * @pre Function is called from a single-threaded context.
 * @post Log backend, pin validator, and stack canary are initialized.
 * @post Drivers may now be brought up.
 *
 * @note Not thread-safe; intended for one-shot init only.
 *
 * @since 0.1.0
 */
void ra8_infrastructure_init(void)
{
  /* Logging backend first so the rest of the init flow can emit. */
  ra8_log_init();

  /* Clear pin-ownership bookkeeping. */
  ra8_pin_validator_reset();

  /* Seed the stack overflow sentinel. */
  internal_write_stack_canary();

  ra8_log_info("INFRA", "infrastructure ready");
}

/**
 * @brief Implementation of `ra8_stack_canary_check()` -- sentinel check.
 *
 * @details Walks the linker-defined region and compares each word to
 *          `k_ra8_stack_canary_pattern`.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Sentinel intact.
 * @retval k_ra8_err_validation_failed  At least one word was corrupted.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @pre Linker-defined sentinel symbols are valid (target build).
 * @post No state modified.
 * @post Result reflects the canary region at the moment of the call.
 *
 * @note Thread-safe (read-only walk). On the off-target host this is a
 *       no-op that always returns `k_ra8_ok`.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_stack_canary_check(void)
{
#ifndef RA8_OFF_TARGET
  for (uint32_t* w = g_ra8_ls_stack_canary_start; w < g_ra8_ls_stack_canary_end; w++) {
    if (*w != (uint32_t)k_ra8_stack_canary_pattern) {
      return k_ra8_err_validation_failed;
    }
  }
#endif
  return k_ra8_ok;
}

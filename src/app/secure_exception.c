/**
 * @file src/app/secure_exception.c
 * @brief Secure-side fault handler for NS -> S violations
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * deliverable. When a Non-Secure caller tries to read a
 * Secure address (or call a Secure function that has not been
 * exposed via a NSC veneer), the Cortex-M85 raises a SecureFault.
 * The handler in this file:
 *
 * 1. Snapshots SFSR (Secure Fault Status Register) so the
 * operator can see *why* the fault fired.
 * 2. Logs the violation through the ITM.
 * 3. Spins forever with IRQs masked. Real production firmware
 * would jump to a controlled reset path, but for the
 * demo a halt is more diagnostic.
 *
 * The handler runs in the secure world. NS code cannot reach it
 * directly because the SFSR register at 0xE000EDE4 is in the
 * secure-only system control region.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_log.h"

static const char* s_tag = "SECEXC";

typedef enum : uintptr_t {
  k_ra8_sfsr_addr = 0xE000EDE4UL, /**< SecureFault Status Register. */
} ra8_secure_excep_addr_t;

/**
 * @brief Cortex-M85 SecureFault handler (NVIC vector 7).
 *
 * @details
 * Vector_table.c installs this in the SecureFault slot when the
 * firmware is built with RA8_TRUSTZONE_ENABLE. With TZ off the
 * function is dead-stripped because nothing references it.
 *
 * @pre Cortex-M85 has raised a SecureFault.
 *
 * @post Logs the SFSR snapshot and spins.
 *
 * @par TrustZone Safety:
 * - **Validates:** nothing -- this is an exception entry point.
 * - **Trusts:** the SFSR snapshot belongs to the failing access.
 * - **Denies:** any return path. The handler does not RET.
 */
void SecureFault_Handler(void);
void SecureFault_Handler(void)
{
  const uint32_t sfsr = *(volatile uint32_t*)k_ra8_sfsr_addr;
  ra8_log_error_val(s_tag, "SecureFault SFSR", sfsr);
  __asm__ volatile("cpsid i");
  while (1) {
    __asm__ volatile("wfi");
  }
}

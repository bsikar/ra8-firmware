/**
 * @file ra_cgc.c
 * @brief Minimal Clock Generation Circuit driver implementation
 *
 * @details
 * v0.x bring-up keeps this file small. `ra_cgc_init()` is a no-op so
 * the firmware runs on the default MOCO clock (8 MHz) until a full
 * PLL routine lands. `ra_cgc_use_hoco()` demonstrates the PRCR unlock
 * + SCKSCR-write sequence and lifts the system clock to HOCO
 * (20 MHz) for slightly faster bring-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_cgc.h"

#include <stdint.h>

#include "ra8d2_cgc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "CGC";

ra_err_t ra_cgc_init(void)
{
  /* No-op for now: firmware runs on default MOCO. A future version
   * will programme PLL1 for the 1 GHz Cortex-M85 target. */
  ra_log_info(s_tag, "cgc_init (no-op, running on MOCO)");
  return k_ra_ok;
}

ra_err_t ra_cgc_use_hoco(void)
{
  /* Step 1: start HOCO by clearing HCSTP. HOCO stabilises in ~2 us.
   * clang-analyzer flags the deref of a fixed peripheral address --
   * that is the whole point of bare-metal register access. */
  /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference,performance-no-int-to-ptr) */
  volatile uint8_t* hococr = ra_sys_hococr();
  /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
  *hococr = (uint8_t)(*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));

  /* Step 2: wait for HOCO ready. The OSCSF (Oscillation Stabilization
   * Flag) bit lives in OSCSF register at offset 0x03C in the SYSC
   * block. For the v0.x build we just insert a small delay loop;
   * a future version will poll the OSCSF.HOCOSF bit. */
  enum : uint32_t {
    k_ra_hoco_spin_cycles = 4000U,
  };
  for (uint32_t i = 0U; i < k_ra_hoco_spin_cycles; i++) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("nop");
#endif
  }

  /* Step 3: unlock PRCR group 0 (CGC), write SCKSCR, relock. */
  ra_sys_prcr_unlock_cgc();
  *ra_sys_sckscr() = (uint8_t)k_ra_cksel_hoco;
  ra_sys_prcr_lock_all();

  ra_log_info(s_tag, "switched to HOCO");
  return k_ra_ok;
}

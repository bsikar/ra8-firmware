/**
 * @file board_periph_bkup.c
 * @brief VBATT backup-register (VBTBKRn) reset-retained domain model
 *
 * @details
 * Models the RA8D2 VBATT backup registers (ra8d2_bkup_regs.h, ra_bkup.c) as a
 * reset-retained power domain, so @c bkup_survival_demo can prove that backup
 * state survives a CPU reset. The 128 @c VBTBKRn bytes (32 x 32-bit words) live
 * in the R_SYSTEM block at @c 0x4001ED00; on silicon they keep their contents
 * across a reset (and, with a battery on VBATT, across a power cycle). The
 * sparse fallback treated them as ordinary peripheral memory that cleared on
 * every run, so the demo's sentinel was never found and it reported
 * @c survived=N.
 *
 * This block claims the VBTBKRn window plus the access-enable byte (@c VBTBER,
 * @c 0xC40) and holds the 128 backup bytes in a host-static buffer that the
 * block's reset hook deliberately does NOT clear. The board_sim run loop's
 * @c --reboot path re-runs the firmware from its reset vector after resetting
 * the peripheral blocks; because this block preserves the backup bytes across
 * that reset, the second boot finds the sentinel intact -- exactly the
 * reset-survival contract. The bytes clear only at process start (static
 * zero-initialisation), modelling the first-ever boot with a dead battery.
 *
 * The @c VBTBER access-enable byte is shadowed (read back as written) but does
 * not gate VBTBKRn writes in the model: the survival demo writes the backup
 * words directly via @c ra_bkup_write_word without first asserting VBAE, so
 * gating would drop its writes. Modelling the retention (the demo's contract)
 * matters here, not the access-protection bit. Reads always return the retained
 * byte.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief VBATT backup window geometry (ra8d2_bkup_regs.h). */
typedef enum : uint64_t {
  k_bkup_base        = 0x4001EC40UL, /**< VBTBER .. past VBTBKRn window base. */
  k_bkup_span        = 0x180UL,      /**< Covers VBTBER (0x00) .. VBTBKR127.  */
  k_bkup_off_vbtber  = 0x000UL,      /**< VBTBER access-enable (at base).     */
  k_bkup_off_vbtbkr0 = 0x0C0UL,      /**< VBTBKR0 (0x4001ED00 - 0x4001EC40).  */
  k_bkup_reg_count   = 128UL,        /**< 128 byte-wide VBTBKRn slots.        */
} bkup_geom_t;

/** @brief Per-tick order slot for the VBATT-backup block (relative order). */
typedef enum : uint32_t {
  k_bkup_block_order = 175U, /**< After the LVD block; report order. */
} bkup_order_t;

/**
 * @brief Retained VBTBKRn bytes -- the battery-backed domain.
 *
 * @details Survives a peripheral reset (the block's reset hook leaves it
 * untouched), so a @c --reboot re-run finds the prior boot's contents intact.
 * Clears only at process start (static zero-init): the first-ever boot.
 */
static uint8_t s_bkup_vbtbkr[k_bkup_reg_count];

/** @brief VBATT control state (cleared by a reset; data is not). */
typedef struct {
  uint8_t  vbtber; /**< VBTBER access-enable shadow.      */
  uint32_t writes; /**< VBTBKRn writes accepted (report). */
} bkup_ctrl_t;

static bkup_ctrl_t s_bkup;

/** @brief Reset only the VBATT control state; the backup bytes are retained. */
static void bkup_reset(void)
{
  s_bkup = (bkup_ctrl_t){};
  /* s_bkup_vbtbkr is intentionally NOT cleared: it is the reset-retained
   * domain that the survival demo depends on across a --reboot. */
}

/** @brief MMIO read inside the VBATT backup window (width-aware). */
static uint64_t bkup_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_bkup_base;
  if (off == (uint64_t)k_bkup_off_vbtber) {
    return s_bkup.vbtber;
  }
  if (off >= (uint64_t)k_bkup_off_vbtbkr0) {
    const uint64_t idx = off - (uint64_t)k_bkup_off_vbtbkr0;
    uint64_t       v   = 0U;
    for (unsigned i = 0U; (i < size) && ((idx + (uint64_t)i) < (uint64_t)k_bkup_reg_count); ++i) {
      v |= (uint64_t)s_bkup_vbtbkr[idx + (uint64_t)i] << (8U * i);
    }
    return v;
  }
  return 0U;
}

/** @brief MMIO write inside the VBATT backup window (width-aware). */
static void bkup_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_bkup_base;
  if (off == (uint64_t)k_bkup_off_vbtber) {
    s_bkup.vbtber = (uint8_t)value;
    return;
  }
  if (off >= (uint64_t)k_bkup_off_vbtbkr0) {
    const uint64_t idx = off - (uint64_t)k_bkup_off_vbtbkr0;
    for (unsigned i = 0U; (i < size) && ((idx + (uint64_t)i) < (uint64_t)k_bkup_reg_count); ++i) {
      s_bkup_vbtbkr[idx + (uint64_t)i] = (uint8_t)(value >> (8U * i));
    }
    s_bkup.writes++;
  }
}

/** @brief End-of-run VBATT-backup section: writes accepted this run. */
static void bkup_report(void)
{
  if (s_bkup.writes == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr, "  VBATT-BKUP    : VBTBKRn writes=%u (domain retained)\n", s_bkup.writes);
}

/** @brief VBATT-backup block descriptor (self-registered with the core). */
static const board_periph_block_t k_bkup_block = {
  .base   = (uint64_t)k_bkup_base,
  .span   = (uint64_t)k_bkup_span,
  .order  = (uint32_t)k_bkup_block_order,
  .read   = bkup_read,
  .write  = bkup_write,
  .tick   = nullptr,
  .reset  = bkup_reset,
  .report = bkup_report,
  .name   = "VBATT-BKUP",
};

/** @brief Register the VBATT-backup block before main (host constructor). */
[[gnu::constructor]] static void bkup_block_register(void)
{
  board_periph_register_block(&k_bkup_block);
}

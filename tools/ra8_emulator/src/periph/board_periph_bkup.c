/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_bkup.c
 * @brief VBATT backup-register (VBTBKRn) reset-retained domain model
 *
 * @details
 * Models the RA8D2 VBATT backup registers (ra8_bkup_regs.h, ra8_bkup.c) as a
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
 * block's reset hook deliberately does NOT clear. The ra8_emulator run loop's
 * @c --reboot path re-runs the firmware from its reset vector after resetting
 * the peripheral blocks; because this block preserves the backup bytes across
 * that reset, the second boot finds the sentinel intact -- exactly the
 * reset-survival contract. The bytes clear only at process start (static
 * zero-initialisation), modelling the first-ever boot with a dead battery.
 *
 * Two silicon preconditions gate a VBTBKRn write, and both are modelled here:
 *
 * 1. **PRCR.PRC1 must be unlocked.** Every register in this block -- VBTBER,
 *    VBTICTLR, VBTBKRn, VBTBPCR1, VBTBPCR2, VBTBPSR, VBTADSR, VBTADCR1,
 *    VBTADCR2, VBTICTLR2, VBTADCR3, VBTNCWCR -- is named under PRC1 in HUM
 *    Ch 13.1 Table 13.1 "Association between PRCR bits and use of registers to
 *    be protected" p 520-521. A write with PRC1 locked is discarded silently:
 *    no fault, no flag. This is the #131 root cause. Bench-confirmed by J-Link
 *    on an EK-RA8D2: writing VBTBKR0 with PRCR locked reads back
 *    @c 0x00000000; after @c PRCR = 0xA502 the identical write sticks.
 * 2. **VBTBER.VBAE must be 1** -- HUM Ch 12.2.6 p 504, "You must write 1 to
 *    VBAE before accessing VBTBKR".
 *
 * @c VBTBER resets to @c 0x08 (VBAE = 1) per its "Value after reset" row in
 * HUM Ch 12.2.6 p 504, matching a J-Link read of a live board, so VBAE alone
 * never blocks a first write -- which is precisely why modelling only VBAE
 * left the emulator green while the bench was red. Writes to VBTBER itself are
 * PRC1-gated too, so firmware that clears VBAE without unlocking does not
 * change it, exactly as on silicon.
 *
 * A write dropped for either reason is counted in @c dropped and named in the
 * end-of-run report, so the cause is visible rather than inferred from a
 * failing banner. Reads always return the retained byte (PRCR gates writes
 * only, HUM Ch 13.1 p 520).
 *
 * The earlier revision of this file blamed the @c OFS1.PVDAS option byte for
 * the #131 divergence and declared it unmodelable. That was wrong: PVDAS
 * governs the *battery power-supply switch* (HUM Ch 12.3.2 p 514), not
 * register access, and on the EK-RA8D2 -- where VBATT is tied to VCC and the
 * switch is stopped -- the backup registers read and write correctly with the
 * default @c OFS1 once PRC1 is unlocked. Bench-proven: @c rw=ok survived=Y.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_prcr_internal.h"

/** @brief VBATT backup window geometry (ra8_bkup_regs.h). */
typedef enum : uint64_t {
  k_bkup_base        = 0x4001EC40UL, /**< VBTBER .. past VBTBKRn window base. */
  k_bkup_span        = 0x180UL,      /**< Covers VBTBER (0x00) .. VBTBKR127.  */
  k_bkup_off_vbtber  = 0x000UL,      /**< VBTBER access-enable (at base).     */
  k_bkup_off_vbtbkr0 = 0x0C0UL,      /**< VBTBKR0 (0x4001ED00 - 0x4001EC40).  */
  k_bkup_reg_count   = 128UL,        /**< 128 byte-wide VBTBKRn slots.        */
} bkup_geom_t;

/** @brief VBTBER field masks (HUM Ch 12.2.6 p 504). */
typedef enum : uint8_t {
  k_bkup_vbtber_mask_vbae = 0x08U, /**< VBAE @ bit 3: 1 = enable VBTBKRn access. */
  k_bkup_vbtber_reset     = 0x08U, /**< "Value after reset" row: VBAE = 1.       */
} bkup_vbtber_mask_t;

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
  uint8_t  vbtber;       /**< VBTBER access-enable shadow (VBAE @ bit 3). */
  uint32_t writes;       /**< VBTBKRn writes accepted (report).           */
  uint32_t dropped_prc1; /**< VBTBKRn writes dropped: PRCR.PRC1 locked.   */
  uint32_t dropped_vbae; /**< VBTBKRn writes dropped: VBTBER.VBAE = 0.    */
} bkup_ctrl_t;

static bkup_ctrl_t s_bkup;

/** @brief Reset only the VBATT control state; the backup bytes are retained. */
static void bkup_reset(void)
{
  s_bkup = (bkup_ctrl_t){};
  /* HUM Ch 12.2.6 p 504 "Value after reset": VBAE = 1, so the access-enable
   * bit is already armed out of reset (confirmed by a J-Link read of a live
   * board). Modelling it as 0 would make a forgotten VBAE arm look fatal in
   * the emulator when on silicon it is harmless. */
  s_bkup.vbtber = (uint8_t)k_bkup_vbtber_reset;
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
  /* HUM Ch 13.1 Table 13.1 p 521 lists VBTBER and VBTBKRn (with the rest of
   * the VBATT file) under PRC1. Every write below is discarded while that
   * group is locked, with no fault and no flag -- the #131 silicon behaviour.
   * Nested-if (not &&) keeps each decision single-condition. */
  if (!board_prcr_group_unlocked((uint16_t)k_board_prcr_grp1_lpm)) {
    if (off >= (uint64_t)k_bkup_off_vbtbkr0) {
      s_bkup.dropped_prc1++;
    }
    return;
  }
  if (off == (uint64_t)k_bkup_off_vbtber) {
    s_bkup.vbtber = (uint8_t)value;
    return;
  }
  if (off >= (uint64_t)k_bkup_off_vbtbkr0) {
    /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register" p 504: "You must
     * write 1 to VBAE before accessing VBTBKR." Drop the write while VBAE = 0
     * so a cleared-VBAE bug surfaces here (rw=BAD) exactly as on silicon. */
    if ((s_bkup.vbtber & (uint8_t)k_bkup_vbtber_mask_vbae) == 0U) {
      s_bkup.dropped_vbae++;
      return;
    }
    const uint64_t idx = off - (uint64_t)k_bkup_off_vbtbkr0;
    for (unsigned i = 0U; (i < size) && ((idx + (uint64_t)i) < (uint64_t)k_bkup_reg_count); ++i) {
      s_bkup_vbtbkr[idx + (uint64_t)i] = (uint8_t)(value >> (8U * i));
    }
    s_bkup.writes++;
  }
}

/** @brief End-of-run VBATT-backup section: writes accepted / dropped this run. */
static void bkup_report(void)
{
  if (s_bkup.dropped_prc1 != 0U) {
    /* Loud: firmware wrote VBTBKRn without unlocking PRCR.PRC1 (#131). */
    (void)fprintf(
      stderr,
      "  VBATT-BKUP    : VBTBKRn writes=%u dropped=%u (PRCR.PRC1 locked: unlock 0xA502)\n",
      s_bkup.writes,
      s_bkup.dropped_prc1);
    return;
  }
  if (s_bkup.dropped_vbae != 0U) {
    /* Loud: firmware touched VBTBKRn after clearing VBTBER.VBAE. */
    (void)fprintf(stderr,
                  "  VBATT-BKUP    : VBTBKRn writes=%u dropped=%u (VBAE=0: enable VBTBER.VBAE)\n",
                  s_bkup.writes,
                  s_bkup.dropped_vbae);
    return;
  }
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

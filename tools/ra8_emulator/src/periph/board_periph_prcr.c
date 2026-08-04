/**
 * @file board_periph_prcr.c
 * @brief SYSC register-write-protection (PRCR) model
 *
 * @details
 * Models the RA8D2 Protect Register (PRCR) at @c 0x4001_E3FA -- HUM Ch 13
 * "Register Write Protection Function" p 520-523. PRCR gates *writes* to
 * whole families of SYSC-adjacent registers; reads are never gated. A write
 * to a protected register while its group bit is 0 is **silently discarded**
 * by the hardware: no bus fault, no status flag, the register simply keeps
 * its old value.
 *
 * That silence is exactly what makes the protection worth modelling. Issue
 * #131 was a driver (@c ra8_bkup.c) that wrote the entire VBATT backup
 * register file with PRCR locked. On silicon every write vanished and
 * @c bkup_survival_demo reported @c rw=BAD; in the emulator, which modelled
 * no protection at all, the same firmware reported @c rw=ok. The emulator passed
 * where the bench failed, which is the defect this block removes.
 *
 * The block is **observe-only**: it snoops the PRCR window so the sparse
 * fallback continues to serve reads and record writes exactly as before,
 * while the protected blocks consult ::board_prcr_group_unlocked to decide
 * whether to accept a store. Bench-confirmed on an EK-RA8D2 by J-Link:
 * writing VBTBKR0 with PRCR locked reads back @c 0x00000000; after
 * @c PRCR = 0xA502 the identical write reads back intact.
 *
 * Scope: the mask is maintained for every group, but only the blocks that
 * have been silicon-verified against it consult it (currently the VBATT
 * backup block, PRC1). Wiring the remaining protected windows -- CGC under
 * PRC0, the PVD registers under PRC3 -- is deliberately left to the change
 * that verifies each on hardware, so no gate is tightened on theory alone.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_prcr_internal.h"
#include "ra8_attributes.h"

/** @brief PRCR window geometry (HUM Ch 13.2.1 p 522). */
typedef enum : uint64_t {
  k_prcr_base = 0x4001E3FAUL, /**< PRCR_S: SYSC base 0x4001_E000 + 0x3FA. */
  k_prcr_span = 0x2UL,        /**< One 16-bit register.                   */
} prcr_geom_t;

/** @brief PRCR field masks (HUM Ch 13.2.1 p 522). */
typedef enum : uint16_t {
  k_prcr_key_mask   = 0xFF00U, /**< PRKEY[7:0] lives in bits 15:8.      */
  k_prcr_key_value  = 0xA500U, /**< Mandatory 0xA5 write key.           */
  k_prcr_group_mask = 0x003BU, /**< PRC0/1/3/4/5; bit 2 + 7:6 reserved. */
} prcr_mask_t;

/** @brief Per-tick order slot for the PRCR block (relative order). */
typedef enum : uint32_t {
  k_prcr_block_order = 170U, /**< Just before the VBATT-backup block. */
} prcr_order_t;

/**
 * @brief Live PRCR group-unlock mask, and the counters behind the report.
 */
typedef struct {
  uint16_t groups;  /**< Currently write-enabled group bits.        */
  uint32_t unlocks; /**< Keyed writes that enabled >= 1 group.      */
  uint32_t bad_key; /**< Writes discarded for carrying a wrong key. */
} prcr_state_t;

static prcr_state_t s_prcr;

/**
 * @brief Return PRCR to its power-on state: every group locked.
 *
 * @details HUM Ch 13.2.1 p 522 gives PRCR a reset value of 0x0000, so no
 * group is write-enabled until firmware presents the key.
 */
static void prcr_reset(void)
{
  s_prcr = (prcr_state_t){};
}

RA8_PRIV bool board_prcr_group_unlocked(uint16_t group_mask)
{
  return (uint16_t)(s_prcr.groups & group_mask) == group_mask;
}

/**
 * @brief MMIO read stub -- never called for an observe-only block.
 *
 * @details The sparse fallback answers PRCR reads (the block snoops rather
 * than owns its window), so this exists only to satisfy the descriptor's
 * required @c read slot.
 */
static uint64_t prcr_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return 0U;
}

/**
 * @brief Snoop a PRCR write and update the retained group mask.
 *
 * @details
 * HUM Ch 13.2.1 p 522: "PRKEY[7:0] ... Write 0xA5 to this bit field to
 * permit the write to the PRCn bits." A write whose key byte is not 0xA5 is
 * discarded and leaves the mask untouched -- modelled here so firmware that
 * forgets the key gets the silicon result (no unlock) rather than a free one.
 */
static void prcr_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  const uint16_t word = (uint16_t)value;
  if ((uint16_t)(word & (uint16_t)k_prcr_key_mask) != (uint16_t)k_prcr_key_value) {
    s_prcr.bad_key++;
    return;
  }
  s_prcr.groups = (uint16_t)(word & (uint16_t)k_prcr_group_mask);
  if (s_prcr.groups != 0U) {
    s_prcr.unlocks++;
  }
}

/** @brief End-of-run PRCR section: unlock traffic and any key mistakes. */
static void prcr_report(void)
{
  if (s_prcr.bad_key != 0U) {
    /* Loud: a PRCR write without the 0xA5 key unlocks nothing on silicon. */
    (void)fprintf(stderr,
                  "  SYSC-PRCR     : unlocks=%u REJECTED=%u (missing 0xA5 key in PRKEY[7:0])\n",
                  s_prcr.unlocks,
                  s_prcr.bad_key);
    return;
  }
  if (s_prcr.unlocks == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr, "  SYSC-PRCR     : unlocks=%u\n", s_prcr.unlocks);
}

/** @brief PRCR block descriptor (observe-only: snoops, does not own). */
static const board_periph_block_t k_prcr_block = {
  .base    = (uint64_t)k_prcr_base,
  .span    = (uint64_t)k_prcr_span,
  .order   = (uint32_t)k_prcr_block_order,
  .read    = prcr_read,
  .write   = prcr_write,
  .tick    = nullptr,
  .reset   = prcr_reset,
  .report  = prcr_report,
  .name    = "SYSC-PRCR",
  .observe = true,
};

/** @brief Register the PRCR block before main (host constructor). */
[[gnu::constructor]] static void prcr_block_register(void)
{
  board_periph_register_block(&k_prcr_block);
}

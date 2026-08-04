/**
 * @file board_periph_lvd.c
 * @brief LVD / PVD voltage-monitor status model for ra8_emulator
 *
 * @details
 * Models the RA8D2 low-voltage detection status registers (ra8_lvd_regs.h,
 * ra8_lvd.c) so a VCC monitor reports a healthy rail, instead of the status
 * reading back zero. Against the sparse fallback @c lvd_monitor_demo configured
 * PVD1 (threshold 2.80 V) but @c PVD1SR.MON read back 0, so the banner reported
 * @c mon=below @c ok=N -- the opposite of a healthy 3.3 V board. This block
 * makes @c PVD1SR.MON (and @c PVD2SR.MON) read "above threshold" with the
 * latched-crossing flag @c DET clear, so the banner reports @c mon=above
 * @c ok=Y -- the reading a correctly-powered EK-RA8D2 gives.
 *
 * The PVD status registers live inside the R_SYSTEM block; this model claims
 * only the four-byte window holding PVD1CR1 / PVD1SR / PVD2CR1 / PVD2SR
 * (@c 0x4001E0E0..0x4001E0E3), leaving the rest of R_SYSTEM (clock-generator
 * bring-up, etc.) to the sparse fallback. The control bytes (PVD1CR1 / PVD2CR1)
 * shadow their writes so the driver's read-back succeeds; the status bytes
 * (PVD1SR / PVD2SR) always report MON = above, DET = 0. A write to a status
 * byte with DET = 0 is honoured (the driver clears DET write-0-to-clear); MON
 * is read-only live state and stays asserted.
 *
 * The emulator has no analog rail to compare, so reporting "above" is the
 * faithful steady state for a powered board -- the genuine driver path
 * (@c ra8_lvd_get_status reading @c PVD1SR.MON / .DET) sees exactly what a
 * healthy 3.3 V supply yields on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief PVD status-window geometry (ra8_lvd_regs.h). */
typedef enum : uint64_t {
  k_lvd_base        = 0x4001E0E0UL, /**< PVD1CR1 .. PVD2SR window base. */
  k_lvd_span        = 0x04UL,       /**< PVD1CR1/PVD1SR/PVD2CR1/PVD2SR. */
  k_lvd_off_pvd1cr1 = 0x00UL,       /**< PVD1CR1 control (8b).          */
  k_lvd_off_pvd1sr  = 0x01UL,       /**< PVD1SR status (8b).            */
  k_lvd_off_pvd2cr1 = 0x02UL,       /**< PVD2CR1 control (8b).          */
  k_lvd_off_pvd2sr  = 0x03UL,       /**< PVD2SR status (8b).            */
} lvd_geom_t;

/** @brief PVDmSR field masks + the healthy-rail status value (ra8_lvd.c). */
typedef enum : uint8_t {
  k_lvd_sr_det     = 0x01U, /**< DET latched threshold-crossing (bit 0).  */
  k_lvd_sr_mon     = 0x02U, /**< MON live "VCC above Vdetm" flag (bit 1). */
  k_lvd_sr_healthy = 0x02U, /**< MON = above, DET = 0: a healthy rail.    */
} lvd_field_t;

/** @brief Per-tick order slot for the LVD block (relative order only). */
typedef enum : uint32_t {
  k_lvd_block_order = 170U, /**< After the CAC block; report order. */
} lvd_order_t;

/** @brief PVD control-byte shadows (status bytes are synthesised). */
typedef struct {
  uint8_t  pvd1cr1; /**< PVD1CR1 control shadow.       */
  uint8_t  pvd2cr1; /**< PVD2CR1 control shadow.       */
  uint32_t reads;   /**< Status reads served (report). */
} lvd_state_t;

static lvd_state_t s_lvd;

/** @brief Reset the LVD model to power-on state. */
static void lvd_reset(void)
{
  s_lvd = (lvd_state_t){};
}

/** @brief MMIO read inside the PVD status window. */
static uint64_t lvd_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_lvd_base;
  if (off == (uint64_t)k_lvd_off_pvd1cr1) {
    return s_lvd.pvd1cr1;
  }
  if (off == (uint64_t)k_lvd_off_pvd2cr1) {
    return s_lvd.pvd2cr1;
  }
  if ((off == (uint64_t)k_lvd_off_pvd1sr) || (off == (uint64_t)k_lvd_off_pvd2sr)) {
    s_lvd.reads++;
    return (uint64_t)k_lvd_sr_healthy; /* MON = above, DET = 0. */
  }
  return 0U;
}

/** @brief MMIO write inside the PVD status window. */
static void lvd_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_lvd_base;
  if (off == (uint64_t)k_lvd_off_pvd1cr1) {
    s_lvd.pvd1cr1 = (uint8_t)value;
  } else if (off == (uint64_t)k_lvd_off_pvd2cr1) {
    s_lvd.pvd2cr1 = (uint8_t)value;
  }
  /* PVDmSR writes (DET write-0-to-clear) are absorbed: MON stays "above". */
}

/** @brief End-of-run LVD section: status reads served. */
static void lvd_report(void)
{
  if (s_lvd.reads == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr, "  LVD/PVD       : status reads=%u (MON=above, DET=0)\n", s_lvd.reads);
}

/** @brief LVD status-window descriptor (self-registered with the core). */
static const board_periph_block_t k_lvd_block = {
  .base   = (uint64_t)k_lvd_base,
  .span   = (uint64_t)k_lvd_span,
  .order  = (uint32_t)k_lvd_block_order,
  .read   = lvd_read,
  .write  = lvd_write,
  .tick   = nullptr,
  .reset  = lvd_reset,
  .report = lvd_report,
  .name   = "LVD/PVD",
};

/** @brief Register the LVD status window before main (host constructor). */
[[gnu::constructor]] static void lvd_block_register(void)
{
  board_periph_register_block(&k_lvd_block);
}

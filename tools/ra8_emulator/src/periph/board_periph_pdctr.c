/**
 * @file board_periph_pdctr.c
 * @brief Graphics power-domain gating (PDCTRGD) model
 *
 * @details
 * Models the RA8D2 Graphics Power Domain Control Register, @c PDCTRGD at
 * SYSC + 0x110 (@c 0x4001_E110) -- HUM Ch 11.2.14 p 452. The domain it gates
 * holds MIPI DSI, MIPI CSI, VIN, **DRW** and GLCDC (HUM Ch 11.5.1 Table 11.7
 * p 480).
 *
 * The register's reset value is @c 0x81: @c PDPGSF (bit 7) = 1 meaning the
 * domain is gated OFF, and @c PDDE (bit 0) = 1 meaning "power off the target
 * domain". So **every graphics peripheral is unpowered out of reset**, and
 * cancelling a module-stop bit is not enough to make one respond. @c PDDE has
 * inverted polarity: writing 0 powers the domain ON.
 *
 * Modelling this is what closes issue #247. The D/AVE 2D engine had never
 * rasterised a pixel on real silicon because nothing ever cleared @c PDDE;
 * ra8_emulator, which modelled no power domain at all, happily let the firmware
 * drive a DRW that on the bench was simply switched off. Bench evidence on an
 * EK-RA8D2: with @c PDCTRGD at its @c 0x81 reset value the DRW @c HWREVISION
 * register reads @c 0x00000000; after clearing @c PDDE (with PRCR.PRC1
 * unlocked) it reads @c 0x0FBE0107 and the engine rasterises.
 *
 * @c PDCTRGD is PRC1-protected (HUM Ch 13.1 Table 13.1 p 521), so writes are
 * routed through ::priv_board_prcr_group_unlocked and silently discarded while that
 * group is locked -- the same silence the hardware gives.
 *
 * The model completes gating instantly: @c PDCSF (control-in-progress) always
 * reads 0, so a driver polling "PDCSF == 0" makes progress, while @c PDPGSF
 * tracks @c PDDE so the "domain still gated" check is real.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_pdctr_internal.h"
#include "board_periph_prcr_internal.h"
#include "emu_host_io_internal.h"
#include "ra8_attributes.h"

/** @brief PDCTRGD window geometry (HUM Ch 11.2.14 p 452). */
typedef enum : uint64_t {
  k_pdctr_gd_base = 0x4001E110UL, /**< PDCTRGD: SYSC 0x4001_E000 + 0x110. */
  k_pdctr_gd_span = 0x1UL,        /**< One 8-bit register.                */
} pdctr_geom_t;

/** @brief PDCTRGD field masks (HUM Ch 11.2.14 p 452). */
typedef enum : uint8_t {
  k_pdctr_pdde_mask   = 0x01U, /**< PDDE   @ bit 0: 1 = power OFF.  */
  k_pdctr_pdcsf_mask  = 0x40U, /**< PDCSF  @ bit 6: control busy.   */
  k_pdctr_pdpgsf_mask = 0x80U, /**< PDPGSF @ bit 7: 1 = gated off.  */
  k_pdctr_reset_value = 0x81U, /**< "Value after reset": gated off. */
} pdctr_mask_t;

/** @brief Per-tick order slot for the power-domain block. */
typedef enum : uint32_t {
  k_pdctr_block_order = 172U, /**< Between PRCR and the VBATT block. */
} pdctr_order_t;

/** @brief Graphics power-domain state. */
typedef struct {
  uint8_t  pdctrgd;  /**< Live PDCTRGD value (PDDE + status flags).  */
  uint32_t power_on; /**< Times the domain was powered on (report).  */
  uint32_t blocked;  /**< Writes dropped while PRCR.PRC1 was locked. */
} pdctr_state_t;

static pdctr_state_t s_pdctr;

/**
 * @brief Return PDCTRGD to its documented reset value: domain gated OFF.
 * @details Return pdctrgd to its documented reset value: domain gated off; this step is contained within the board periph pdctr model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for pdctr reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph pdctr model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdctr_reset(void)
{
  s_pdctr         = (pdctr_state_t){};
  s_pdctr.pdctrgd = (uint8_t)k_pdctr_reset_value;
}

RA8_PRIV bool priv_board_pdctr_graphics_powered(void)
{
  return (uint8_t)(s_pdctr.pdctrgd & (uint8_t)k_pdctr_pdpgsf_mask) == 0U;
}

/**
 * @brief MMIO read of PDCTRGD: PDDE as written, status flags derived.
 * @details MMIO read of pdctrgd: pdde as written, status flags derived; this step is contained within the board periph pdctr model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The pdctr read result produced by the board periph pdctr model.
 * @retval value The operation-specific pdctr read value.
 * @pre Arguments satisfy the ranges documented for pdctr read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph pdctr model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_pdctr_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return (uint64_t)s_pdctr.pdctrgd;
}

/**
 * @brief MMIO write of PDCTRGD: apply PDDE, settle the gating flags.
 *
 * @details
 * HUM Ch 11.2.14 p 452: PDDE selects the target state (0 = power on,
 * 1 = power off), PDCSF reports a transition in progress and PDPGSF reports
 * the resulting gate state. Gating is modelled as instantaneous, so PDCSF
 * always settles to 0 and PDPGSF simply follows PDDE.
  * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for pdctr write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph pdctr model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_pdctr_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  /* HUM Ch 13.1 Table 13.1 p 521 lists PDCTRGD under PRC1: a write with that
   * group locked is discarded by the hardware with no fault and no flag. */
  if (!priv_board_prcr_group_unlocked((uint16_t)k_board_prcr_grp1_lpm)) {
    s_pdctr.blocked++;
    return;
  }
  const bool power_off = ((uint8_t)value & (uint8_t)k_pdctr_pdde_mask) != 0U;
  if (power_off) {
    s_pdctr.pdctrgd = (uint8_t)(k_pdctr_pdde_mask | k_pdctr_pdpgsf_mask);
    return;
  }
  if (!priv_board_pdctr_graphics_powered()) {
    s_pdctr.power_on++;
  }
  s_pdctr.pdctrgd = 0U; /* PDDE=0, PDCSF=0 (settled), PDPGSF=0 (powered). */
}

/**
 * @brief End-of-run graphics power-domain section.
 * @details End-of-run graphics power-domain section; this step is contained within the board periph pdctr model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for pdctr report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph pdctr model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdctr_report(void)
{
  if (s_pdctr.blocked != 0U) {
    /* Loud: PDCTRGD writes need PRCR.PRC1 unlocked or the domain stays dark. */
    (void)priv_emu_io_errf(
      "  PWR-GRAPHICS  : PDCTRGD writes DROPPED=%u (PRCR.PRC1 locked: unlock 0xA502)\n",
      s_pdctr.blocked);
    return;
  }
  if (s_pdctr.power_on == 0U) {
    return; /* Untouched: stay quiet (domain still gated, as at reset). */
  }
  (void)priv_emu_io_errf("  PWR-GRAPHICS  : graphics power domain on (DRW/GLCDC/MIPI/VIN)\n");
}

/** @brief Graphics power-domain block descriptor (self-registered). */
static const board_periph_block_t s_k_pdctr_block = {
  .base   = (uint64_t)k_pdctr_gd_base,
  .span   = (uint64_t)k_pdctr_gd_span,
  .order  = (uint32_t)k_pdctr_block_order,
  .read   = internal_pdctr_read,
  .write  = internal_pdctr_write,
  .tick   = nullptr,
  .reset  = internal_pdctr_reset,
  .report = internal_pdctr_report,
  .name   = "PWR-GRAPHICS",
};

/** @brief Register the power-domain block before main (host constructor). */
[[gnu::constructor]] RA8_INTERNAL static void internal_pdctr_block_register(void)
{
  board_periph_register_block(&s_k_pdctr_block);
  internal_pdctr_reset();
}

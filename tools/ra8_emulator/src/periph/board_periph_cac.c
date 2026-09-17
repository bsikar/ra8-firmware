/**
 * @file board_periph_cac.c
 * @brief CAC (Clock Frequency Accuracy Measurement) edge-counter model
 *
 * @details
 * Models the RA8D2 CAC (ra8_cac_regs.h, ra8_cac.c) at @c 0x40202400 so a
 * frequency-accuracy measurement actually completes with an in-window edge
 * count, instead of the status flag never asserting. Against the sparse
 * fallback @c cac_accuracy_demo started a measurement (CACR0.CFME = 1) but
 * @c CASTR.MENDF never set, so @c ra8_cac_measure timed out and the banner
 * reported @c meas=TIMEOUT. This block latches MENDF on the start write and
 * returns a count inside the programmed [CALLVR, CAULVR] window, so the
 * measurement completes with FERRF / OVFF clear and the banner reports
 * @c ok=Y.
 *
 * The CAC counts edges of a measurement-target clock during one reference
 * period and sets CASTR.FERRF when the count falls outside the upper / lower
 * limit registers. The board emulator has no real clocks to count, so rather
 * than invent an edge rate it reports the midpoint of the firmware's own
 * programmed window: that is in-band by construction, so the genuine driver
 * path (start -> poll MENDF -> read CACNTBR -> check FERRF) sees a healthy
 * measurement exactly as a correctly-tuned crystal would on silicon.
 *
 * Sequence modelled (HUM Ch 10.2):
 *  - @c CACR0 (+0x00) CFME bit starts a measurement. On a CFME=1 write the
 *    model loads @c CACNTBR with the [CALLVR, CAULVR] midpoint and sets
 *    @c CASTR.MENDF (FERRF / OVFF stay clear). A CFME=0 write stops it.
 *  - @c CASTR (+0x04) exposes MENDF / FERRF / OVFF; the driver's CAICR clear
 *    bits (FERRFCL / MENDFCL / OVFFCL, +4 in CAICR) clear the matching flags.
 *  - @c CAULVR (+0x06) / @c CALLVR (+0x08) are the window limits the driver
 *    programmes; @c CACNTBR (+0x0A) is the latched count it reads back.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "emu_host_io_internal.h"

/** @brief CAC block geometry (ra8_cac_regs.h, r_cac_regs_t). */
typedef enum : uint64_t {
  k_cac_base        = 0x40202400UL, /**< CAC register base (HUM Ch 10.2). */
  k_cac_span        = 0x10UL,       /**< Covers CACR0..CACNTBR.           */
  k_cac_off_cacr0   = 0x00UL,       /**< CACR0 control 0 (CFME).          */
  k_cac_off_caicr   = 0x03UL,       /**< CAICR interrupt / flag-clear.    */
  k_cac_off_castr   = 0x04UL,       /**< CASTR status (MENDF/FERRF/OVFF). */
  k_cac_off_caulvr  = 0x06UL,       /**< CAULVR upper limit (16b).        */
  k_cac_off_callvr  = 0x08UL,       /**< CALLVR lower limit (16b).        */
  k_cac_off_cacntbr = 0x0AUL,       /**< CACNTBR counter buffer (16b).    */
} cac_geom_t;

/** @brief CACR0 / CASTR / CAICR field masks (ra8_cac.c). */
typedef enum : uint8_t {
  k_cac_cacr0_cfme    = 0x01U, /**< CACR0.CFME measurement enable (bit 0). */
  k_cac_castr_ferrf   = 0x01U, /**< CASTR.FERRF frequency-error (bit 0).   */
  k_cac_castr_mendf   = 0x02U, /**< CASTR.MENDF measurement-end (bit 1).   */
  k_cac_castr_ovff    = 0x04U, /**< CASTR.OVFF counter-overflow (bit 2).   */
  k_cac_caicr_ferrfcl = 0x10U, /**< CAICR.FERRFCL clear FERRF (bit 4).     */
  k_cac_caicr_mendfcl = 0x20U, /**< CAICR.MENDFCL clear MENDF (bit 5).     */
  k_cac_caicr_ovffcl  = 0x40U, /**< CAICR.OVFFCL clear OVFF (bit 6).       */
} cac_field_t;

/** @brief Per-tick order slot for the CAC block (relative order only). */
typedef enum : uint32_t {
  k_cac_block_order = 165U, /**< After the DRW block; report order. */
} cac_order_t;

/** @brief CAC register state. */
typedef struct {
  uint8_t  cacr0;   /**< CACR0 shadow (CFME).                 */
  uint8_t  castr;   /**< CASTR status flags.                  */
  uint16_t caulvr;  /**< Upper window limit.                  */
  uint16_t callvr;  /**< Lower window limit.                  */
  uint16_t cacntbr; /**< Latched edge count.                  */
  uint32_t meas;    /**< Measurements completed (for report). */
} cac_state_t;

static cac_state_t s_cac;

/**
 * @brief Complete one measurement: latch an in-window count and set MENDF.
 * @details Complete one measurement: latch an in-window count and set mendf; this step is contained within the board periph cac model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for cac measure. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph cac model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cac_measure(void)
{
  /* Report the midpoint of the firmware's programmed window: in-band by
   * construction, so FERRF stays clear and the measurement reads healthy. */
  s_cac.cacntbr = (uint16_t)(((uint32_t)s_cac.callvr + (uint32_t)s_cac.caulvr) / 2U);
  s_cac.castr   = (uint8_t)k_cac_castr_mendf; /* MENDF set; FERRF / OVFF clear. */
  s_cac.meas++;
}

/**
 * @brief Reset the CAC model to power-on state.
 * @details Reset the cac model to power-on state; this step is contained within the board periph cac model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for cac reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph cac model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cac_reset(void)
{
  s_cac = (cac_state_t){};
}

/**
 * @brief MMIO read inside the CAC window.
 * @details MMIO read inside the cac window; this step is contained within the board periph cac model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The cac read result produced by the board periph cac model.
 * @retval value The operation-specific cac read value.
 * @pre Arguments satisfy the ranges documented for cac read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph cac model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_cac_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_cac_base;
  if (off == (uint64_t)k_cac_off_cacr0) {
    return s_cac.cacr0;
  }
  if (off == (uint64_t)k_cac_off_castr) {
    return s_cac.castr;
  }
  if (off == (uint64_t)k_cac_off_caulvr) {
    return s_cac.caulvr;
  }
  if (off == (uint64_t)k_cac_off_callvr) {
    return s_cac.callvr;
  }
  if (off == (uint64_t)k_cac_off_cacntbr) {
    return s_cac.cacntbr;
  }
  return 0U;
}

/**
 * @brief MMIO write inside the CAC window.
 * @details MMIO write inside the cac window; this step is contained within the board periph cac model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for cac write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph cac model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_cac_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_cac_base;
  if (off == (uint64_t)k_cac_off_cacr0) {
    s_cac.cacr0 = (uint8_t)value;
    if (((uint8_t)value & (uint8_t)k_cac_cacr0_cfme) != 0U) {
      internal_cac_measure(); /* CFME=1 starts a measurement; it completes at once. */
    }
  } else if (off == (uint64_t)k_cac_off_caicr) {
    /* CAICR clear bits are write-1-to-clear of the matching CASTR flags. */
    if (((uint8_t)value & (uint8_t)k_cac_caicr_ferrfcl) != 0U) {
      s_cac.castr &= (uint8_t)~(uint8_t)k_cac_castr_ferrf;
    }
    if (((uint8_t)value & (uint8_t)k_cac_caicr_mendfcl) != 0U) {
      s_cac.castr &= (uint8_t)~(uint8_t)k_cac_castr_mendf;
    }
    if (((uint8_t)value & (uint8_t)k_cac_caicr_ovffcl) != 0U) {
      s_cac.castr &= (uint8_t)~(uint8_t)k_cac_castr_ovff;
    }
  } else if (off == (uint64_t)k_cac_off_caulvr) {
    s_cac.caulvr = (uint16_t)value;
  } else if (off == (uint64_t)k_cac_off_callvr) {
    s_cac.callvr = (uint16_t)value;
  }
}

/**
 * @brief End-of-run CAC section: measurements + last count and window.
 * @details End-of-run cac section: measurements + last count and window; this step is contained within the board periph cac model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for cac report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph cac model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cac_report(void)
{
  if (s_cac.meas == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)priv_emu_io_errf("  CAC           : measurements=%u count=%u window=[%u,%u]\n",
                         s_cac.meas,
                         (unsigned)s_cac.cacntbr,
                         (unsigned)s_cac.callvr,
                         (unsigned)s_cac.caulvr);
}

/** @brief CAC block descriptor (self-registered with the core). */
static const board_periph_block_t s_k_cac_block = {
  .base   = (uint64_t)k_cac_base,
  .span   = (uint64_t)k_cac_span,
  .order  = (uint32_t)k_cac_block_order,
  .read   = internal_cac_read,
  .write  = internal_cac_write,
  .tick   = nullptr,
  .reset  = internal_cac_reset,
  .report = internal_cac_report,
  .name   = "CAC",
};

/** @brief Register the CAC block before main (host constructor). */
[[gnu::constructor]] RA8_INTERNAL static void internal_cac_block_register(void)
{
  board_periph_register_block(&s_k_cac_block);
}

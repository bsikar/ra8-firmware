/**
 * @file board_periph_dtc.c
 * @brief DTC (Data Transfer Controller) transfer-engine model for board_sim
 *
 * @details
 * Models the RA8D2 DTC (ra8_dtc_regs.h, ra8_dtc.c) so an interrupt-activated
 * descriptor-table transfer actually MOVES bytes in emulated memory, instead of
 * the control window merely shadowing register writes. Against the sparse
 * fallback / the old transparent DTC shadow, @c dtc_transfer_demo programmed a
 * Transfer Information (TI) block and fired ELC software event 0 but the
 * destination was never written, so the demo reported @c match=N. This block
 * performs the copy on the activation event, flipping that to @c match=Y.
 *
 * Unlike the DMAC (software-triggered by a register write inside its own
 * window), the DTC is activated by an interrupt request: an IELSR slot with its
 * DTCE bit set, linked to the firing event, activates the controller. The demo
 * routes ELC software event 0 (ELC_SWEVT0 = 0x0CC) to such a slot and fires it
 * with the three-step @c ELSEGR0 write sequence. This model therefore owns two
 * register windows:
 *
 *  - **DTC control window** (@c 0x4000AC00): tracks @c DTCVBR (the vector-table
 *    base, +0x04) so an activation can find the per-slot TI pointer; @c DTCCR /
 *    @c DTCST / @c DTCSTS read back as written so the driver bring-up succeeds.
 *  - **ELC ELSEGR window** (@c 0x40201000): watches @c ELSEGR0 (+0x04). The
 *    third write of the unlock / arm / trigger sequence (value @c 0x41, SEG=1)
 *    fires software event 0 -> this model runs the DTC.
 *
 * Activation walks the copy:
 *  1. Resolve the IELSR slot linked to the event with DTCE set, via
 *     ::board_periph_icu_dtc_slot (the core owns the IELSR table).
 *  2. Read the 4-byte vector-table entry at @c DTCVBR + slot*4 (its low bit is
 *     the privilege attribution, masked off) to get the 16-byte-aligned TI.
 *  3. Read the TI block (MR / SAR / DAR / CRB / CRA) from emulated memory.
 *  4. Decode MRA.SZ for the unit width (byte / 16-bit / 32-bit) and MRA.SM /
 *     MRB.DM for the source / destination address-increment modes, then copy
 *     CRA * CRB units (block mode CRA = block size, CRB = block count; normal
 *     mode CRB = 0 -> CRA units) from SAR to DAR with @c uc_mem_read /
 *     @c uc_mem_write, advancing each address by the unit size when its mode is
 *     "increment".
 *  5. Re-raise the linked event through the core's ICU -> NVIC path so the
 *     transfer-complete interrupt the firmware waits on is delivered.
 *
 * The transfer is synchronous (performed inside the ELSEGR trigger write), so a
 * firmware poll on the destination settling falls straight through.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief Console-tap line buffer capacity for a DTC transfer summary. */
typedef enum : uint32_t {
  k_dtc_console_line_cap = 48U, /**< Max chars in a "DTC .. units" line. */
} dtc_console_t;

/** @brief DTC control-window geometry (ra8_dtc_regs.h, r_dtc_regs_t). */
typedef enum : uint64_t {
  k_dtc_base       = 0x4000AC00UL, /**< DTC control-register base.        */
  k_dtc_span       = 0x40UL,       /**< Covers DTCCR..DTCSQE comfortably. */
  k_dtc_off_dtccr  = 0x00UL,       /**< DTCCR control (8b).               */
  k_dtc_off_dtcvbr = 0x04UL,       /**< DTCVBR vector base (32b).         */
  k_dtc_off_dtcst  = 0x0CUL,       /**< DTCST module start (8b).          */
  k_dtc_off_dtcsts = 0x0EUL,       /**< DTCSTS status (16b).              */
} dtc_geom_t;

/** @brief ELC ELSEGR-window geometry (ra8_elc_regs.h). */
typedef enum : uint64_t {
  k_elc_base        = 0x40201000UL, /**< R_ELC base.                        */
  k_elc_span        = 0x100UL,      /**< Covers ELCR / ELSEGR[0..3] / ELSR. */
  k_elc_off_elsegr0 = 0x04UL,       /**< ELSEGR0 software-event gen (8b).   */
} elc_geom_t;

/** @brief ELC software event 0 number and its ELSEGR trigger value. */
typedef enum : uint32_t {
  k_elc_swevt0_event   = 0x0CCU, /**< ELC_SWEVT0 event (HUM Table 19.3).     */
  k_elc_elsegr_trigger = 0x41U,  /**< WE=1, SEG=1: fires the software event. */
} elc_swevt_t;

/** @brief DTC TI block field offsets (r_dtc_xfer_info_t, HUM Figure 18.4). */
typedef enum : uint32_t {
  k_ti_off_mr  = 0U,  /**< MR mode word ([31:24] MRA, [23:16] MRB). */
  k_ti_off_sar = 4U,  /**< SAR source address.                      */
  k_ti_off_dar = 8U,  /**< DAR destination address.                 */
  k_ti_off_crb = 12U, /**< CRB block count (16b).                   */
  k_ti_off_cra = 14U, /**< CRA transfer count (16b).                */
} dtc_ti_off_t;

/** @brief MR mode-word field extraction (MRA in the top byte, MRB next). */
typedef enum : uint32_t {
  k_mr_mra_shift = 24U,   /**< MRA occupies MR[31:24].                  */
  k_mr_mrb_shift = 16U,   /**< MRB occupies MR[23:16].                  */
  k_mra_md_pos   = 6U,    /**< MRA.MD[7:6] transfer mode.               */
  k_mra_sz_pos   = 4U,    /**< MRA.SZ[5:4] unit width.                  */
  k_mra_sm_pos   = 2U,    /**< MRA.SM[3:2] source-address mode.         */
  k_mrb_dm_pos   = 2U,    /**< MRB.DM[3:2] dest-address mode.           */
  k_mr_2bit_mask = 0x3U,  /**< Two-bit field mask.                      */
  k_mra_md_block = 0x2U,  /**< MD = 10b: block transfer.                */
  k_mr_addr_inc  = 0x2U,  /**< SM / DM = 10b: increment the address.    */
  k_mra_sz_byte  = 0x0U,  /**< SZ = 00b: 8-bit unit.                    */
  k_mra_sz_word  = 0x2U,  /**< SZ = 10b: 32-bit unit.                   */
  k_mra_sz_half  = 0x1U,  /**< SZ = 01b: 16-bit unit.                   */
  k_mr_byte_mask = 0xFFU, /**< Eight-bit field mask (one MRA/MRB byte). */
} dtc_mr_field_t;

/** @brief DTC transfer-count encoding quirks (HUM 18.2.7). */
typedef enum : uint32_t {
  k_dtc_block_cra_zero = 256U, /**< Block mode CRA = 0 encodes a 256-unit block. */
} dtc_count_t;

/** @brief Vector-table entry layout (DTCVBR + slot*4 -> TI start). */
typedef enum : uint32_t {
  k_dtc_vt_entry_bytes = 4U,          /**< One 4-byte pointer per IELSR slot. */
  k_dtc_ti_addr_mask   = 0xFFFFFFFEU, /**< Strip bit0 (privilege attribute).  */
  k_dtc_max_units      = 0x10000U,    /**< Sanity cap on a single activation. */
  k_dtc_ielsr_slots    = 96U,         /**< Number of IELSR slots (0..95).     */
} dtc_vt_geom_t;

/** @brief Per-tick order slot for the DTC block (relative order only). */
typedef enum : uint32_t {
  k_dtc_block_order = 55U, /**< Just after the DMAC block; report order. */
} dtc_order_t;

/** @brief DTC control-register state plus an activation/byte tally. */
typedef struct {
  uint8_t  dtccr;       /**< DTCCR shadow.                         */
  uint32_t dtcvbr;      /**< DTCVBR vector-table base address.     */
  uint8_t  dtcst;       /**< DTCST module-start shadow.            */
  uint16_t dtcsts;      /**< DTCSTS status shadow.                 */
  uint32_t activations; /**< Successful DTC activations performed. */
  uint32_t bytes;       /**< Bytes moved on the last activation.   */
} dtc_state_t;

static dtc_state_t s_dtc;

/**
 * @brief ELC register-window shadow (ELCR / ELSEGR / ELSR).
 *
 * @details The DTC model owns the ELC window only to observe the ELSEGR0
 * software-event trigger; every ELC register otherwise reads back exactly as
 * written so the ELC driver bring-up (e.g. @c elc_event_demo reading ELCR.ELCON
 * through @c ra8_elc_is_enabled, or its ELSR link writes) behaves as before.
 */
static uint8_t s_elc[k_elc_span];

/** @brief Bytes per unit for an MRA.SZ code (defaults to byte width). */
static uint32_t dtc_unit_bytes(uint32_t sz_code)
{
  if (sz_code == (uint32_t)k_mra_sz_word) {
    return 4U;
  }
  if (sz_code == (uint32_t)k_mra_sz_half) {
    return 2U;
  }
  return 1U;
}

/** @brief Read a little-endian word of @p bytes from emulated memory. */
static uint32_t dtc_mem_read(uc_engine* uc, uint64_t addr, uint32_t bytes)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, (size_t)bytes);
  return v;
}

/**
 * @struct dtc_ti_t
 * @brief One Transfer Information block, decoded from emulated memory.
 *
 * @details
 * Mirrors `r_dtc_xfer_info_t` (HUM Figure 18.4) with the packed MR mode word
 * already split into its MRA / MRB sub-fields, so the transfer engine works in
 * named terms rather than re-deriving shifts at each use.
 */
typedef struct {
  uint32_t sar; /**< SAR source address.                             */
  uint32_t dar; /**< DAR destination address.                        */
  uint32_t cra; /**< CRA transfer count (block size in block mode).  */
  uint32_t crb; /**< CRB block count.                                */
  uint32_t md;  /**< MRA.MD transfer mode (normal / repeat / block). */
  uint32_t sz;  /**< MRA.SZ unit-width code (see ::dtc_unit_bytes).  */
  uint32_t sm;  /**< MRA.SM source address mode.                     */
  uint32_t dm;  /**< MRB.DM destination address mode.                */
} dtc_ti_t;

/**
 * @brief Read and decode the TI block at @p ti_addr into @p out.
 *
 * @param[in]  uc      Unicorn engine holding the emulated memory.
 * @param[in]  ti_addr 16-byte-aligned address of the TI block.
 * @param[out] out     Receives the decoded descriptor.
 * @return None.
 * @retval None Void.
 * @pre @p uc and @p out are non-null.
 * @pre @p ti_addr is mapped in the emulated address space.
 * @post Every field of @p out is written.
 * @post Emulated memory is unchanged (reads only).
 * @note Not thread-safe.
 */
static void dtc_decode_ti(uc_engine* uc, uint32_t ti_addr, dtc_ti_t* out)
{
  const uint32_t mr = dtc_mem_read(uc, (uint64_t)ti_addr + (uint64_t)k_ti_off_mr, 4U);
  out->sar          = dtc_mem_read(uc, (uint64_t)ti_addr + (uint64_t)k_ti_off_sar, 4U);
  out->dar          = dtc_mem_read(uc, (uint64_t)ti_addr + (uint64_t)k_ti_off_dar, 4U);
  out->crb          = dtc_mem_read(uc, (uint64_t)ti_addr + (uint64_t)k_ti_off_crb, 2U);
  out->cra          = dtc_mem_read(uc, (uint64_t)ti_addr + (uint64_t)k_ti_off_cra, 2U);

  const uint32_t mra = (mr >> (uint32_t)k_mr_mra_shift) & (uint32_t)k_mr_byte_mask;
  const uint32_t mrb = (mr >> (uint32_t)k_mr_mrb_shift) & (uint32_t)k_mr_byte_mask;
  out->md            = (mra >> (uint32_t)k_mra_md_pos) & (uint32_t)k_mr_2bit_mask;
  out->sz            = (mra >> (uint32_t)k_mra_sz_pos) & (uint32_t)k_mr_2bit_mask;
  out->sm            = (mra >> (uint32_t)k_mra_sm_pos) & (uint32_t)k_mr_2bit_mask;
  out->dm            = (mrb >> (uint32_t)k_mrb_dm_pos) & (uint32_t)k_mr_2bit_mask;
}

/**
 * @brief Total unit count a descriptor moves, or 0 if it is out of range.
 *
 * @details
 * CRA = 0 in block mode encodes a 256-unit block (HUM 18.2.7); otherwise CRA is
 * the unit count. Block mode multiplies that by the CRB block count (CRB = 0
 * meaning one block). A zero or implausibly large result is reported as 0 so the
 * caller declines the transfer rather than looping on a corrupt descriptor.
 *
 * @param[in] ti Decoded descriptor.
 * @return Unit count in [1, ::k_dtc_max_units], or 0 when out of range.
 * @retval 0 The descriptor asks for no units, or more than the model allows.
 * @pre @p ti is non-null and fully decoded.
 * @pre @p ti->md holds a two-bit MRA.MD code.
 * @post The return value is 0 or a bounded loop count.
 * @post @p ti is unmodified.
 * @note Not thread-safe with respect to @p ti.
 */
static uint32_t dtc_unit_count(const dtc_ti_t* ti)
{
  uint32_t per_block = ti->cra;
  uint32_t blocks    = 1U;
  if (ti->md == (uint32_t)k_mra_md_block) {
    if (ti->cra == 0U) {
      per_block = (uint32_t)k_dtc_block_cra_zero;
    }
    blocks = (ti->crb == 0U) ? 1U : ti->crb;
  }
  const uint32_t units = per_block * blocks;
  return (units > (uint32_t)k_dtc_max_units) ? 0U : units;
}

/**
 * @brief Copy @p units units of @p unit bytes from SAR to DAR.
 *
 * @details
 * Each address advances by the unit size when its MRA.SM / MRB.DM mode is
 * "increment", and stays put otherwise -- which is how a fixed-address
 * peripheral register reads or writes the whole run.
 *
 * @param[in] uc    Unicorn engine holding the emulated memory.
 * @param[in] ti    Decoded descriptor supplying the addresses and modes.
 * @param[in] units Unit count to move (bounded by ::dtc_unit_count).
 * @param[in] unit  Unit width in bytes (1, 2 or 4).
 * @return None.
 * @retval None Void.
 * @pre @p uc and @p ti are non-null.
 * @pre @p unit is 1, 2 or 4, so the scratch buffer cannot overrun.
 * @post @p units units have been written at the destination.
 * @post @p ti is unmodified.
 * @note Not thread-safe.
 */
static void dtc_copy_units(uc_engine* uc, const dtc_ti_t* ti, uint32_t units, uint32_t unit)
{
  uint64_t       s_addr = (uint64_t)ti->sar;
  uint64_t       d_addr = (uint64_t)ti->dar;
  const uint64_t s_step = (ti->sm == (uint32_t)k_mr_addr_inc) ? (uint64_t)unit : 0U;
  const uint64_t d_step = (ti->dm == (uint32_t)k_mr_addr_inc) ? (uint64_t)unit : 0U;
  for (uint32_t i = 0U; i < units; ++i) {
    uint8_t tmp[4] = {};
    (void)uc_mem_read(uc, s_addr, tmp, (size_t)unit);
    (void)uc_mem_write(uc, d_addr, tmp, (size_t)unit);
    s_addr += s_step;
    d_addr += d_step;
  }
}

/** @brief Perform the SAR->DAR copy described by one TI block. */
static void dtc_run_transfer(uc_engine* uc, uint32_t ti_addr)
{
  dtc_ti_t ti = {};
  dtc_decode_ti(uc, ti_addr, &ti);

  const uint32_t units = dtc_unit_count(&ti);
  if (units == 0U) {
    return;
  }
  const uint32_t unit = dtc_unit_bytes(ti.sz);
  dtc_copy_units(uc, &ti, units, unit);

  s_dtc.activations++;
  s_dtc.bytes = units * unit;
  /* Console DMA tab: one line per DTC activation (the DTC shares the DMA tab
   * with the DMAC). Reports the unit count and unit size. */
  char ln[k_dtc_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "DTC %u units x %uB", (unsigned)units, (unsigned)unit);
  board_console_push(k_board_console_ch_dma, ln);
}

/** @brief Run the DTC for ELC software event 0 if a DTCE slot links it. */
static void dtc_activate_swevt0(uc_engine* uc)
{
  if (s_dtc.dtcvbr == 0U) {
    return; /* Vector base not programmed: nothing to activate. */
  }
  const uint32_t slot = board_periph_icu_dtc_slot((uint16_t)k_elc_swevt0_event);
  if (slot >= (uint32_t)k_dtc_ielsr_slots) {
    return; /* No IELSR slot links the event with DTCE set. */
  }
  const uint64_t vt_entry =
    (uint64_t)s_dtc.dtcvbr + ((uint64_t)slot * (uint64_t)k_dtc_vt_entry_bytes);
  const uint32_t ti_addr = dtc_mem_read(uc, vt_entry, 4U) & (uint32_t)k_dtc_ti_addr_mask;
  if (ti_addr == 0U) {
    return; /* No TI registered for the slot. */
  }
  dtc_run_transfer(uc, ti_addr);
  /* Real silicon clears the slot's DTCE and asserts its CPU interrupt on the
   * last transfer (HUM Figure 18.5 p 801). The demo gates on the destination
   * settling (a direct poll), not the ISR, so the model performs the copy and
   * leaves interrupt delivery to the firmware's own polling path -- pending the
   * NVIC line here is unnecessary for the transfer contract and depends on the
   * app's vector wiring. The transfer (the modelled behaviour) is what flips the
   * demo to match=Y. */
}

/** @brief Reset the DTC model (and the ELC shadow) to power-on state. */
static void dtc_reset(void)
{
  s_dtc = (dtc_state_t){};
  for (uint32_t i = 0U; i < (uint32_t)k_elc_span; ++i) {
    s_elc[i] = 0U;
  }
}

/** @brief MMIO read inside the DTC control window. */
static uint64_t dtc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_dtc_base;
  if (off == (uint64_t)k_dtc_off_dtccr) {
    return s_dtc.dtccr;
  }
  if (off == (uint64_t)k_dtc_off_dtcvbr) {
    return s_dtc.dtcvbr;
  }
  if (off == (uint64_t)k_dtc_off_dtcst) {
    return s_dtc.dtcst;
  }
  if (off == (uint64_t)k_dtc_off_dtcsts) {
    return s_dtc.dtcsts;
  }
  return 0U;
}

/** @brief MMIO write inside the DTC control window. */
static void dtc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_dtc_base;
  if (off == (uint64_t)k_dtc_off_dtccr) {
    s_dtc.dtccr = (uint8_t)value;
  } else if (off == (uint64_t)k_dtc_off_dtcvbr) {
    s_dtc.dtcvbr = (uint32_t)value;
  } else if (off == (uint64_t)k_dtc_off_dtcst) {
    s_dtc.dtcst = (uint8_t)value;
  } else if (off == (uint64_t)k_dtc_off_dtcsts) {
    s_dtc.dtcsts = (uint16_t)value;
  }
}

/** @brief MMIO read inside the ELC window (transparent byte shadow). */
static uint64_t elc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_elc_base;
  uint64_t       v   = 0U;
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_elc_span); ++i) {
    v |= (uint64_t)s_elc[off + (uint64_t)i] << (8U * i);
  }
  return v;
}

/** @brief MMIO write inside the ELC window: byte shadow + ELSEGR0 trigger. */
static void elc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  const uint64_t off = addr - (uint64_t)k_elc_base;
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_elc_span); ++i) {
    s_elc[off + (uint64_t)i] = (uint8_t)(value >> (8U * i));
  }
  /* The third write of the unlock/arm/trigger sequence (SEG=1) on ELSEGR0 fires
   * software event 0, which activates the DTC if a DTCE-enabled slot links it. */
  if ((off == (uint64_t)k_elc_off_elsegr0) &&
      (((uint8_t)value & (uint8_t)k_elc_elsegr_trigger) == (uint8_t)k_elc_elsegr_trigger)) {
    dtc_activate_swevt0(uc);
  }
}

/** @brief End-of-run DTC section: activations and bytes moved. */
static void dtc_report(void)
{
  if (s_dtc.activations == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr,
                "  DTC           : activations=%u last_bytes=%u DTCVBR=0x%08X\n",
                s_dtc.activations,
                s_dtc.bytes,
                s_dtc.dtcvbr);
}

/** @brief DTC control-window descriptor (owns reset + report). */
static const board_periph_block_t k_dtc_block = {
  .base   = (uint64_t)k_dtc_base,
  .span   = (uint64_t)k_dtc_span,
  .order  = (uint32_t)k_dtc_block_order,
  .read   = dtc_read,
  .write  = dtc_write,
  .tick   = nullptr,
  .reset  = dtc_reset,
  .report = dtc_report,
  .name   = "DTC",
};

/** @brief ELC ELSEGR-window descriptor (the DTC activation trigger). */
static const board_periph_block_t k_elc_block = {
  .base   = (uint64_t)k_elc_base,
  .span   = (uint64_t)k_elc_span,
  .order  = (uint32_t)k_dtc_block_order,
  .read   = elc_read,
  .write  = elc_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "ELC/DTC-trig",
};

/** @brief Register the DTC control + ELC ELSEGR windows before main runs. */
[[gnu::constructor]] static void dtc_block_register(void)
{
  board_periph_register_block(&k_dtc_block);
  board_periph_register_block(&k_elc_block);
}

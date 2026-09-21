/**
 * @file board_periph_doc.c
 * @brief Data Operation Circuit (DOC) peripheral-block model for ra8_emulator
 *
 * @details
 * Models the RA8D2 DOC (ra8_doc_regs.h, ra8_doc.c) at @c 0x40311000 so
 * @c doc_demo's hardware result matches its software reference. The demo chains
 * an 8-entry add through the DOC and compares the hardware sum to a portable
 * software sum, lighting LED1 on a match and latching LED2 on any divergence;
 * against the sparse fallback DODSR0 read back garbage, so LED2 latched. (The
 * @c RA8_OFF_TARGET shortcut in ra8_doc.c that stores the result in software
 * is compiled out of the real cross-target .elf ra8_emulator runs, so the genuine
 * "write DODIR, read DODSR0" hardware path must work.)
 *
 * The unit is one accumulator DODSR0 updated by each DODIR write per DOCR.OMS:
 *  - **Add** (OMS=1): DODSR0 += DODIR, masked to the DOCR.DOBW width (16 / 32);
 *    DOPCF latches on a carry out of the width.
 *  - **Subtract** (OMS=2): DODSR0 -= DODIR, masked; DOPCF latches on a borrow.
 *  - **Compare** (OMS=0): DOPCF latches when the DODIR-vs-DODSR0 relation the
 *    DCSEL field selects holds (match / mismatch implemented; the rest default
 *    to mismatch).
 *
 * DODSR0 is read/write (the driver seeds operand A there); DOSR exposes DOPCF
 * and DOSCR clears it. Only the add path is exercised by an in-tree example
 * (doc_demo); subtract and compare use their documented semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "emu_host_io_internal.h"

/** @brief Result-width masks for the DOC unit (32-bit vs 16-bit mode). */
typedef enum : uint32_t {
  k_u32_all  = 0xFFFFFFFFU, /**< 32-bit all-ones result mask. */
  k_u16_full = 0x0000FFFFU, /**< 16-bit result mask.          */
} doc_lit_t;

/** @brief DOC block geometry (ra8_doc_regs.h). */
typedef enum : uint64_t {
  k_doc_base       = 0x40311000UL, /**< DOC base (HUM Ch 57.2).            */
  k_doc_span       = 0x20UL,       /**< Register window.                   */
  k_doc_off_docr   = 0x00UL,       /**< DOCR control (OMS/DOBW/DCSEL), 8b. */
  k_doc_off_dosr   = 0x04UL,       /**< DOSR status (DOPCF), 8b.           */
  k_doc_off_doscr  = 0x08UL,       /**< DOSCR status clear, 8b.            */
  k_doc_off_dodir  = 0x0CUL,       /**< DODIR data input.                  */
  k_doc_off_dodsr0 = 0x10UL,       /**< DODSR0 reference / result.         */
  k_doc_off_dodsr1 = 0x14UL,       /**< DODSR1 upper threshold.            */
} doc_map_t;

/** @brief DOCR / DOSR field masks (ra8_doc_regs.h). */
typedef enum : uint32_t {
  k_doc_oms_mask    = 0x03U, /**< OMS[1:0] mode select.          */
  k_doc_dobw_mask   = 0x08U, /**< DOBW: 0 = 16-bit, 1 = 32-bit.  */
  k_doc_dcsel_mask  = 0x70U, /**< DCSEL[2:0] compare condition.  */
  k_doc_dcsel_shift = 4U,    /**< DCSEL field position.          */
  k_doc_dopcf       = 0x01U, /**< DOSR DOPCF flag / DOSCR clear. */
} doc_field_t;

/** @brief DOCR.OMS operation modes. */
typedef enum : uint32_t {
  k_doc_mode_compare  = 0U, /**< Compare (set DOPCF on DCSEL hit). */
  k_doc_mode_add      = 1U, /**< DODSR0 += DODIR.                  */
  k_doc_mode_subtract = 2U, /**< DODSR0 -= DODIR.                  */
} doc_mode_t;

/** @brief Per-tick order slot for the DOC block (relative order only). */
typedef enum : uint32_t {
  k_doc_block_order = 141U, /**< After the CRC block; report order. */
} doc_order_t;

/** @brief DOC register state. */
typedef struct {
  uint8_t  docr;   /**< DOCR shadow (OMS/DOBW/DCSEL). */
  uint8_t  dopcf;  /**< DOSR.DOPCF latch.             */
  uint32_t dodsr0; /**< Reference / running result.   */
  uint32_t dodsr1; /**< Upper threshold (compare).    */
  uint32_t ops;    /**< Operations performed.         */
} doc_state_t;

static doc_state_t s_doc;

/**
 * @brief Width mask selected by DOCR.DOBW (0 = 16-bit, 1 = 32-bit).
 * @details Width mask selected by docr.dobw (0 = 16-bit, 1 = 32-bit); this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @return The doc width mask result produced by the board periph doc model.
 * @retval value The operation-specific doc width mask value.
 * @pre Arguments satisfy the ranges documented for doc width mask. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_doc_width_mask(void)
{
  return ((s_doc.docr & (uint8_t)k_doc_dobw_mask) != 0U) ? (uint32_t)k_u32_all
                                                         : (uint32_t)k_u16_full;
}

/**
 * @brief Apply one DODIR operand to DODSR0 per the current DOCR.OMS mode.
 * @details Apply one dodir operand to dodsr0 per the current docr.oms mode; this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @param[in] dodir Dodir input used by the operation.
 * @pre Arguments satisfy the ranges documented for doc apply. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_doc_apply(uint32_t dodir)
{
  const uint32_t mode = (uint32_t)(s_doc.docr & (uint8_t)k_doc_oms_mask);
  const uint32_t mask = internal_doc_width_mask();
  const uint32_t in   = dodir & mask;
  const uint32_t cur  = s_doc.dodsr0 & mask;
  if (mode == (uint32_t)k_doc_mode_add) {
    const uint32_t sum = cur + in;
    s_doc.dopcf        = ((sum & ~mask) != 0U) ? (uint8_t)k_doc_dopcf : s_doc.dopcf;
    s_doc.dodsr0       = sum & mask;
  } else if (mode == (uint32_t)k_doc_mode_subtract) {
    s_doc.dopcf  = (in > cur) ? (uint8_t)k_doc_dopcf : s_doc.dopcf; /* borrow */
    s_doc.dodsr0 = (cur - in) & mask;
  } else {
    /* Compare: DCSEL selects the relation that latches DOPCF. DCSEL=1 matches
     * on equality; everything else (including 0) latches on mismatch. */
    const uint32_t dcsel =
      (uint32_t)((s_doc.docr & (uint8_t)k_doc_dcsel_mask) >> (uint32_t)k_doc_dcsel_shift);
    const bool equal = (in == cur);
    const bool hit   = (dcsel == 1U) ? equal : !equal;
    if (hit) {
      s_doc.dopcf = (uint8_t)k_doc_dopcf;
    }
  }
  s_doc.ops++;
}

/**
 * @brief Reset the DOC unit to power-on state.
 * @details Reset the doc unit to power-on state; this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for doc reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_doc_reset(void)
{
  s_doc.docr   = 0U;
  s_doc.dopcf  = 0U;
  s_doc.dodsr0 = 0U;
  s_doc.dodsr1 = 0U;
  s_doc.ops    = 0U;
}

/**
 * @brief MMIO read inside the DOC window.
 * @details MMIO read inside the doc window; this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The doc read result produced by the board periph doc model.
 * @retval value The operation-specific doc read value.
 * @pre Arguments satisfy the ranges documented for doc read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_doc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_doc_base;
  if (off == (uint64_t)k_doc_off_docr) {
    return s_doc.docr;
  }
  if (off == (uint64_t)k_doc_off_dosr) {
    return s_doc.dopcf;
  }
  if (off == (uint64_t)k_doc_off_dodsr0) {
    return s_doc.dodsr0;
  }
  if (off == (uint64_t)k_doc_off_dodsr1) {
    return s_doc.dodsr1;
  }
  return 0U;
}

/**
 * @brief MMIO write inside the DOC window.
 * @details MMIO write inside the doc window; this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for doc write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_doc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_doc_base;
  if (off == (uint64_t)k_doc_off_docr) {
    s_doc.docr = (uint8_t)value;
  } else if (off == (uint64_t)k_doc_off_doscr) {
    if (((uint8_t)value & (uint8_t)k_doc_dopcf) != 0U) {
      s_doc.dopcf = 0U; /* DOPCFCL clears the flag. */
    }
  } else if (off == (uint64_t)k_doc_off_dodir) {
    internal_doc_apply((uint32_t)value);
  } else if (off == (uint64_t)k_doc_off_dodsr0) {
    s_doc.dodsr0 = (uint32_t)value; /* Seed operand A. */
  } else if (off == (uint64_t)k_doc_off_dodsr1) {
    s_doc.dodsr1 = (uint32_t)value;
  }
}

/**
 * @brief End-of-run DOC section: mode, last result, op count.
 * @details End-of-run doc section: mode, last result, op count; this step is contained within the board periph doc model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for doc report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph doc model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_doc_report(void)
{
  if (s_doc.ops == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)priv_emu_io_errf("  DOC           : OMS=%u DODSR0=0x%08X DOPCF=%u ops=%u\n",
                         (unsigned)(s_doc.docr & (uint8_t)k_doc_oms_mask),
                         s_doc.dodsr0,
                         (unsigned)s_doc.dopcf,
                         s_doc.ops);
}

/** @brief DOC block descriptor (self-registered with the core). */
static const board_periph_block_t s_k_doc_block = {
  .base   = (uint32_t)k_doc_base,
  .span   = (uint32_t)k_doc_span,
  .order  = (uint32_t)k_doc_block_order,
  .read   = internal_doc_read,
  .write  = internal_doc_write,
  .tick   = nullptr,
  .reset  = internal_doc_reset,
  .report = internal_doc_report,
  .name   = "DOC",
};

/** @brief Register the DOC block before main (host constructor). */
[[gnu::constructor]] RA8_INTERNAL static void internal_doc_block_register(void)
{
  board_periph_register_block(&s_k_doc_block);
}

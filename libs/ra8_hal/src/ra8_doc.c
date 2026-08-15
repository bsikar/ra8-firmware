/**
 * @file ra8_doc.c
 * @brief Data Operation Circuit (DOC) driver implementation
 *
 * @details
 * Thin wrapper that drives the DOC_B block through its add / sub /
 * compare modes. Register writes go through the accessor from
 * `ra8_doc_regs.h`, which returns a pointer to host RAM in
 * `RA8_OFF_TARGET` and a real hardware address on target.
 *
 * Per HUM Ch 57.2 p 3521 the DODSR0 register holds the running
 * accumulator AND the operand reference; DODIR is the data input
 * register. The hardware updates DODSR0 in place: writing DODIR
 * triggers the operation, so the sequence is:
 *
 *   1. Programme DOCR with the desired mode (and DOBW=0 for 16-bit).
 *   2. Write the seed to DODSR0.
 *   3. Write the operand to DODIR -- triggers the operation.
 *   4. Read DODSR0 to obtain the result.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_doc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_doc_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "DOC";

/**
 * @brief Write `mode` into `DOCR.OMS[1:0]`, clearing DOBW for 16-bit ops.
 *
 * @details Programmes the operation-mode select bits and forces DOBW=0
 * (16-bit operand width per HUM Ch 57.2.1 p 3519). DCSEL is left at 0
 * since the 16-bit add / subtract APIs don't use compare-mode detection.
 *
 * @param[in] mode Operation mode to programme.
 *
 * @pre Driver state has been initialized by ``ra8_doc_init``.
 * @pre Caller has validated all pointer parameters.
 * @post DOCR.OMS holds `mode`; DOCR.DOBW=0; DOCR.DCSEL=0.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_ra8_doc_set_mode_16(ra8_docr_oms_t mode)
{
  volatile r_doc_regs_t* reg = ra8_doc();
  /* Write OMS field, clear DOBW + DCSEL. */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR = (uint8_t)mode;
}

/**
 * @brief Run the 16-bit DOC operation sequence and return the result.
 *
 * @details Per HUM Ch 57.2.4 / 57.2.5 p 3521 the operation is triggered
 * by writing DODIR. Writes happen at 16-bit width because DOCR.DOBW=0
 * (set by ``internal_ra8_doc_set_mode_16``). The DODSR0 readback is the
 * silicon operation engine's result. The RAM-backed host register file
 * has no operation engine, so on the unit-test build the readback is
 * simply the seed value: host tests assert the register trace
 * (DOCR / DODSR0 / DODIR) instead of the arithmetic, which the
 * ``doc_demo`` HIL app proves on silicon.
 *
 * @param[in]  seed     Initial DODSR0 value (operand A).
 * @param[in]  operand  DODIR value (operand B).
 *
 * @return 16-bit DODSR0 readback.
 * @retval 0..UINT16_MAX The DODSR0 value after the DODIR trigger write.
 *
 * @pre Driver state has been initialized by ``ra8_doc_init``.
 * @pre DOCR.OMS has been programmed for the desired operation.
 * @post DODSR0 holds the operation result.
 * @post DOCR is unchanged by this helper.
 * @note Not thread-safe; caller must serialize.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint16_t internal_ra8_doc_run_16(uint16_t seed, uint16_t operand)
{
  volatile r_doc_regs_t* reg    = ra8_doc();
  volatile uint16_t*     dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t*     dodir  = (volatile uint16_t*)&reg->DODIR;
  /* Seed the accumulator first. */
  /* HUM Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 */
  *dodsr0 = seed;
  /* Writing DODIR triggers the operation. */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  *dodir = operand;
  /* Read back the accumulator the operation just updated. */
  /* HUM Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 */
  return *dodsr0;
}

[[nodiscard]] ra8_err_t ra8_doc_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_doc);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "doc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_doc_regs_t* reg = ra8_doc();
  /* Reset DOCR to default (compare/16-bit). */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR = 0U;
  /* Clear any stale DOPCF via DOSCR.DOPCFCL. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra8_doc_mask_dopcfcl;
  /* Zero the data registers (DODIR, DODSR0, DODSR1). */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  reg->DODIR  = 0U;
  reg->DODSR0 = 0U;
  reg->DODSR1 = 0U;
  ra8_log_info(s_tag, "doc_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum)
{
  RA8_CHECK_NULL_PTR(out_sum, s_tag, "out_sum must not be nullptr");

  internal_ra8_doc_set_mode_16(k_ra8_doc_mode_add);
  *out_sum = internal_ra8_doc_run_16(a, b);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff)
{
  RA8_CHECK_NULL_PTR(out_diff, s_tag, "out_diff must not be nullptr");

  internal_ra8_doc_set_mode_16(k_ra8_doc_mode_subtract);
  *out_diff = internal_ra8_doc_run_16(a, b);
  return k_ra8_ok;
}

/* =============================================================================
 * Window comparison
 * =============================================================================
 */

/** @brief Implementation of `ra8_doc_set_window()` -- programs DOCR/DODSR0/DODSR1. */
[[nodiscard]] ra8_err_t
ra8_doc_set_window(uint16_t lower, uint16_t upper, ra8_doc_window_polarity_t polarity)
{
  if (lower >= upper) {
    ra8_log_error(s_tag, "set_window: lower must be strictly less than upper");
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)polarity > (uint8_t)k_ra8_doc_window_outside) {
    ra8_log_error(s_tag, "set_window: polarity out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_doc_regs_t* reg    = ra8_doc();
  volatile uint16_t*     dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t*     dodsr1 = (volatile uint16_t*)&reg->DODSR1;
  uint8_t                dcsel;

  if (polarity == k_ra8_doc_window_outside) {
    dcsel = (uint8_t)k_ra8_doc_dcsel_outside;
  } else {
    dcsel = (uint8_t)k_ra8_doc_dcsel_inside;
  }

  /* OMS=00 (compare), DOBW=0 (16-bit), DCSEL=inside(4) or outside(5). */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR =
    (uint8_t)((uint8_t)(dcsel << (uint8_t)k_ra8_doc_bit_dcsel) & (uint8_t)k_ra8_doc_mask_dcsel);
  /* Lower threshold; the constraint DODSR1 > DODSR0 is caller-validated. */
  /* HUM Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 */
  *dodsr0 = lower;
  /* Upper threshold; only used in window comparison mode. */
  /* HUM Ch 57.2.6 "DODSR1 : DOC Data Setting Register 1" p 3522 */
  *dodsr1 = upper;
  /* Clear any stale DOPCF so the first compare sees a fresh flag. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra8_doc_mask_dopcfcl;

  ra8_log_info(s_tag, "set_window");
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_doc_window_compare()` -- writes DODIR, reads DOPCF. */
[[nodiscard]] ra8_err_t ra8_doc_window_compare(uint16_t value, bool* out_flag)
{
  RA8_CHECK_NULL_PTR(out_flag, s_tag, "out_flag must not be nullptr");

  volatile r_doc_regs_t* reg = ra8_doc();
  /* Precondition: OMS must be 00 (compare mode); set_window() enforces this. */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  if ((reg->DOCR & (uint8_t)k_ra8_doc_mask_oms) != 0U) {
    ra8_log_error(s_tag, "window_compare: DOC not in compare mode");
    return k_ra8_err_invalid_state;
  }

  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  /* Clear any prior flag before triggering a new comparison. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra8_doc_mask_dopcfcl;
  /* Writing DODIR triggers the hardware comparison; DOSR.DOPCF is set.
   * The RAM-backed host register file has no comparator engine, so on
   * the unit-test build DOSR simply retains whatever the test staged
   * before the call -- both flag legs below are driven that way. */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  *dodir = value;

  /* Read DOPCF -- set by the silicon comparator after the compare. */
  /* HUM Ch 57.2.2 "DOSR : DOC Flag Status Register" p 3520 */
  *out_flag = ((reg->DOSR & (uint8_t)k_ra8_doc_mask_dopcf) != 0U);
  /* Clear DOPCF so the flag reflects only the last comparison. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra8_doc_mask_dopcfcl;

  return k_ra8_ok;
}

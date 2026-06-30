/**
 * @file ra_doc.c
 * @brief Data Operation Circuit (DOC) driver implementation
 *
 * @details
 * Thin wrapper that drives the DOC_B block through its add / sub /
 * compare modes. Register writes go through the accessor from
 * `ra8d2_doc_regs.h`, which returns a pointer to host RAM in
 * `RA_SIMULATOR_MODE` and a real hardware address on target.
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

#include "ra_doc.h"

#include <stdint.h>

#include "ra8d2_doc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

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
 * @pre Driver state has been initialized by ``ra_doc_init``.
 * @pre Caller has validated all pointer parameters.
 * @post DOCR.OMS holds `mode`; DOCR.DOBW=0; DOCR.DCSEL=0.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static inline void internal_ra_doc_set_mode_16(ra_docr_oms_t mode)
{
  volatile r_doc_regs_t* reg = ra_doc();
  /* Write OMS field, clear DOBW + DCSEL. */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR = (uint8_t)mode;
}

/**
 * @brief Run the 16-bit DOC operation sequence and return the result.
 *
 * @details Per HUM Ch 57.2.4 / 57.2.5 p 3521 the operation is triggered
 * by writing DODIR. Writes happen at 16-bit width because DOCR.DOBW=0
 * (set by ``internal_ra_doc_set_mode_16``). In simulator mode the
 * software-modelled DOC has no real operation engine, so the helper
 * stores the result back into DODSR0 explicitly.
 *
 * @param[in]  seed     Initial DODSR0 value (operand A).
 * @param[in]  operand  DODIR value (operand B).
 * @param[in]  sw_model Pre-computed software result used in
 *                      ``RA_SIMULATOR_MODE`` to mirror what the real
 *                      DOC hardware would have produced.
 *
 * @return 16-bit DODSR0 readback.
 * @retval 0..UINT16_MAX The operation result -- caller compares it
 *                       against ``sw_model`` to detect HW != SW.
 *
 * @pre Driver state has been initialized by ``ra_doc_init``.
 * @pre DOCR.OMS has been programmed for the desired operation.
 * @post DODSR0 holds the operation result.
 * @post DOCR is unchanged by this helper.
 * @note Not thread-safe; caller must serialize.
 * @since 0.1.0
 */
static inline uint16_t internal_ra_doc_run_16(uint16_t seed, uint16_t operand, uint16_t sw_model)
{
  volatile r_doc_regs_t* reg    = ra_doc();
  volatile uint16_t*     dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t*     dodir  = (volatile uint16_t*)&reg->DODIR;
  /* Seed the accumulator first. */
  /* HUM Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 */
  *dodsr0 = seed;
  /* Writing DODIR triggers the operation. */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  *dodir = operand;
#ifdef RA_SIMULATOR_MODE
  /* The sim DOC has no operation engine, model the result. */
  *dodsr0 = sw_model;
#else
  (void)sw_model;
#endif
  return *dodsr0;
}

[[nodiscard]] ra_err_t ra_doc_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_doc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "doc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_doc_regs_t* reg = ra_doc();
  /* Reset DOCR to default (compare/16-bit). */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR = 0U;
  /* Clear any stale DOPCF via DOSCR.DOPCFCL. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra_doc_mask_dopcfcl;
  /* Zero the data registers (DODIR, DODSR0, DODSR1). */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  reg->DODIR  = 0U;
  reg->DODSR0 = 0U;
  reg->DODSR1 = 0U;
  ra_log_info(s_tag, "doc_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum)
{
  RA_CHECK_NULL_PTR(out_sum, s_tag, "out_sum must not be nullptr");

  internal_ra_doc_set_mode_16(k_ra_doc_mode_add);
  *out_sum = internal_ra_doc_run_16(a, b, (uint16_t)(a + b));
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff)
{
  RA_CHECK_NULL_PTR(out_diff, s_tag, "out_diff must not be nullptr");

  internal_ra_doc_set_mode_16(k_ra_doc_mode_subtract);
  *out_diff = internal_ra_doc_run_16(a, b, (uint16_t)(a - b));
  return k_ra_ok;
}

/* =============================================================================
 * Window comparison
 * =============================================================================
 */

/** @brief Implementation of `ra_doc_set_window()` -- programs DOCR/DODSR0/DODSR1. */
[[nodiscard]] ra_err_t
ra_doc_set_window(uint16_t lower, uint16_t upper, ra_doc_window_polarity_t polarity)
{
  if (lower >= upper) {
    ra_log_error(s_tag, "set_window: lower must be strictly less than upper");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)polarity > (uint8_t)k_ra_doc_window_outside) {
    ra_log_error(s_tag, "set_window: polarity out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_doc_regs_t* reg    = ra_doc();
  volatile uint16_t*     dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t*     dodsr1 = (volatile uint16_t*)&reg->DODSR1;
  uint8_t                dcsel;

  if (polarity == k_ra_doc_window_outside) {
    dcsel = (uint8_t)k_ra_doc_dcsel_outside;
  } else {
    dcsel = (uint8_t)k_ra_doc_dcsel_inside;
  }

  /* OMS=00 (compare), DOBW=0 (16-bit), DCSEL=inside(4) or outside(5). */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  reg->DOCR =
    (uint8_t)((uint8_t)(dcsel << (uint8_t)k_ra_doc_bit_dcsel) & (uint8_t)k_ra_doc_mask_dcsel);
  /* Lower threshold; the constraint DODSR1 > DODSR0 is caller-validated. */
  /* HUM Ch 57.2.5 "DODSR0 : DOC Data Setting Register 0" p 3521 */
  *dodsr0 = lower;
  /* Upper threshold; only used in window comparison mode. */
  /* HUM Ch 57.2.6 "DODSR1 : DOC Data Setting Register 1" p 3522 */
  *dodsr1 = upper;
  /* Clear any stale DOPCF so the first compare sees a fresh flag. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra_doc_mask_dopcfcl;

  ra_log_info(s_tag, "set_window");
  return k_ra_ok;
}

/** @brief Implementation of `ra_doc_window_compare()` -- writes DODIR, reads DOPCF. */
[[nodiscard]] ra_err_t ra_doc_window_compare(uint16_t value, bool* out_flag)
{
  RA_CHECK_NULL_PTR(out_flag, s_tag, "out_flag must not be nullptr");

  volatile r_doc_regs_t* reg = ra_doc();
  /* Precondition: OMS must be 00 (compare mode); set_window() enforces this. */
  /* HUM Ch 57.2.1 "DOCR : DOC Control Register" p 3519 */
  if ((reg->DOCR & (uint8_t)k_ra_doc_mask_oms) != 0U) {
    ra_log_error(s_tag, "window_compare: DOC not in compare mode");
    return k_ra_err_invalid_state;
  }

  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  /* Clear any prior flag before triggering a new comparison. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra_doc_mask_dopcfcl;
  /* Writing DODIR triggers the hardware comparison; DOSR.DOPCF is set. */
  /* HUM Ch 57.2.4 "DODIR : DOC Data Input Register" p 3521 */
  *dodir = value;

#ifdef RA_SIMULATOR_MODE
  /* Sim has no DOC operation engine; model the window comparison here. */
  {
    volatile uint16_t* dodsr0_ptr = (volatile uint16_t*)&reg->DODSR0;
    volatile uint16_t* dodsr1_ptr = (volatile uint16_t*)&reg->DODSR1;
    const uint16_t     low        = *dodsr0_ptr;
    const uint16_t     high       = *dodsr1_ptr;
    const uint8_t      dcsel =
      (uint8_t)((reg->DOCR >> (uint8_t)k_ra_doc_bit_dcsel) & (uint8_t)k_ra_doc_mask_dcsel_field);
    uint8_t sim_flag = 0U;
    if (dcsel == (uint8_t)k_ra_doc_dcsel_inside) {
      if (value > low) {
        if (value < high) {
          sim_flag = (uint8_t)k_ra_doc_mask_dopcf;
        }
      }
    } else {
      if (value < low) {
        sim_flag = (uint8_t)k_ra_doc_mask_dopcf;
      }
      if (value > high) {
        sim_flag = (uint8_t)k_ra_doc_mask_dopcf;
      }
    }
    /* DOSR is R in hardware; this write is valid only in simulator mode. */
    /* HUM Ch 57.2.2 "DOSR : DOC Flag Status Register" p 3520 */
    reg->DOSR = sim_flag;
  }
#endif /* RA_SIMULATOR_MODE */

  /* Read DOPCF -- set by hardware (or sim model above) after the compare. */
  /* HUM Ch 57.2.2 "DOSR : DOC Flag Status Register" p 3520 */
  *out_flag = ((reg->DOSR & (uint8_t)k_ra_doc_mask_dopcf) != 0U);
  /* Clear DOPCF so the flag reflects only the last comparison. */
  /* HUM Ch 57.2.3 "DOSCR : DOC Status Clear Register" p 3521 */
  reg->DOSCR = (uint8_t)k_ra_doc_mask_dopcfcl;

  return k_ra_ok;
}

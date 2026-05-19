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
  /* HUM Ch 57.2.1 p 3519 -- write OMS field, clear DOBW + DCSEL. */
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
  volatile r_doc_regs_t* reg = ra_doc();
  volatile uint16_t* dodsr0  = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t* dodir   = (volatile uint16_t*)&reg->DODIR;
  /* HUM Ch 57.2.5 p 3521 -- seed the accumulator first. */
  *dodsr0 = seed;
  /* HUM Ch 57.2.4 p 3521 -- writing DODIR triggers the operation. */
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
  /* HUM Ch 57.2.1 p 3519 -- reset DOCR to default (compare/16-bit). */
  reg->DOCR = 0U;
  /* HUM Ch 57.2.3 p 3521 -- clear any stale DOPCF via DOSCR.DOPCFCL. */
  reg->DOSCR = (uint8_t)k_ra_doc_mask_dopcfcl;
  /* HUM Ch 57.2.4 / 57.2.5 / 57.2.6 -- zero data registers. */
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

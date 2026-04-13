/**
 * @file ra_doc.c
 * @brief Data Operation Circuit (DOC) driver implementation
 *
 * @details
 * Thin wrapper that drives the DOC block through its add / sub /
 * compare modes. Register writes go through the accessor from
 * `ra8d2_doc_regs.h`, which returns a pointer to host RAM in
 * `RA_SIMULATOR_MODE` and a real hardware address on target.
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
 * @brief Write `mode` into `DOCR.OMS[1:0]`, preserving other bits.
 *
 * @param[in] mode Operation mode to programme.
 */
static inline void internal_ra_doc_set_mode(ra_docr_oms_t mode)
{
  volatile r_doc_regs_t* reg     = ra_doc();
  const uint8_t          current = reg->DOCR;
  const uint8_t          cleared = (uint8_t)(current & (uint8_t)~(uint8_t)k_ra_doc_mask_oms);
  reg->DOCR                      = (uint8_t)(cleared | (uint8_t)mode);
}

[[nodiscard]] ra_err_t ra_doc_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_doc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "doc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_doc_regs_t* reg = ra_doc();
  reg->DOCR                  = 0U;
  reg->DODIR                 = 0U;
  reg->DODSR                 = 0U;
  ra_log_info(s_tag, "doc_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum)
{
  RA_CHECK_NULL_PTR(out_sum, s_tag, "out_sum must not be nullptr");

  volatile r_doc_regs_t* reg = ra_doc();
  internal_ra_doc_set_mode(k_ra_doc_mode_add);
  reg->DODSR = a;
  reg->DODIR = b;
#ifdef RA_SIMULATOR_MODE
  /* Simulator: DOC hardware not present on the host, model the
   * add in software so the tests see a meaningful result.
   */
  reg->DODSR = (uint16_t)(a + b);
#endif
  *out_sum = reg->DODSR;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff)
{
  RA_CHECK_NULL_PTR(out_diff, s_tag, "out_diff must not be nullptr");

  volatile r_doc_regs_t* reg = ra_doc();
  internal_ra_doc_set_mode(k_ra_doc_mode_subtract);
  reg->DODSR = a;
  reg->DODIR = b;
#ifdef RA_SIMULATOR_MODE
  reg->DODSR = (uint16_t)(a - b);
#endif
  *out_diff = reg->DODSR;
  return k_ra_ok;
}

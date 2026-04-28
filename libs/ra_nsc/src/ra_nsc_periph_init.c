/**
 * @file ra_nsc_periph_init.c
 * @brief NSC veneer: secure peripheral substrate bring-up
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold. Runs the substrate init dance that the NS
 * world cannot do because the MSTP / CGC / ICU register windows
 * live in the secure region partitioning.
 *
 * The function is idempotent: callers can fire it more than once
 * and get ``k_ra_ok`` for every call after the first.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_dma.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"
#include "ra_pwr.h"

static const char* s_tag = "NSCPRH";

static bool s_initialised = false;

RA_NSC_VENEER ra_err_t ra_nsc_periph_init(void)
{
  if (s_initialised) {
    return k_ra_ok;
  }

  ra_err_t err = ra_mstp_init();
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_mstp_init", (uint32_t)err);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra_pwr_init();
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_pwr_init", (uint32_t)err);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra_isr_init();
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_isr_init", (uint32_t)err);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra_dma_init();
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra_log_error_val(s_tag, "ra_dma_init", (uint32_t)err);
    return k_ra_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  s_initialised = true;
  ra_log_info(s_tag, "secure substrate up");
  return k_ra_ok;
}

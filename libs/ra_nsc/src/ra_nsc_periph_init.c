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

static bool s_initialized = false;

/**
 * @brief NSC veneer: bring up the secure-side peripheral substrate.
 *
 * @details
 * Sequences the four substrate init calls that the Non-Secure world
 * cannot perform because the MSTP / CGC / ICU / DMA register windows
 * live in the secure region partitioning:
 * ``ra_mstp_init`` -> ``ra_pwr_init`` -> ``ra_isr_init`` -> ``ra_dma_init``.
 *
 * The function is idempotent: subsequent calls return ``k_ra_ok``
 * without redoing the work. On failure the latched ``s_initialized``
 * flag stays false so a future call may retry from scratch.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Substrate up (or already up).
 * @retval k_ra_err_hw_init_failed  One of the secure init steps failed.
 *
 * @pre Reset vector has handed over to the firmware.
 * @pre TrustZone SAU has been programmed (single-world or NS region carved).
 * @post On success the secure peripheral substrate is fully up.
 * @post On failure the substrate is in an indeterminate partial state;
 *       the caller may retry.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less, scalar
 *   return -- nothing crosses the boundary that needs range-checking.
 *
 * @note Thread-safe: no -- intended to be called once at boot.
 * @since 0.1.0
 */
RA_NSC_VENEER ra_err_t ra_nsc_periph_init(void)
{
  if (s_initialized) {
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

  s_initialized = true;
  ra_log_info(s_tag, "secure substrate up");
  return k_ra_ok;
}

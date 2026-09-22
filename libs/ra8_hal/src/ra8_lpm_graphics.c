/**
 * @file ra8_lpm_graphics.c
 * @brief Graphics power-domain bring-up for the LPM HAL driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implements ``ra8_lpm_graphics_power_on`` (declared in ``ra8_lpm.h``) and its
 * static helpers. Split out of ``ra8_lpm.c`` so neither translation unit
 * exceeds the file-size cap: the graphics power-gating sequence is a distinct
 * responsibility from the sleep-mode / clock-shutdown matrix that remains in
 * the core driver.
 *
 * The sequence follows HUM Ch 11.5.1 (p 480) and HUM Ch 11.2.14 (PDCTRGD,
 * p 452): start MOCO (MOCOCR is PRC0/CGC), wait for the controller to become
 * ready, clear PDCTRGD.PDDE under the PRC1 unlock to request power-on, then
 * confirm the domain is live. Every protected write runs inside a scoped
 * ``RA8_PROTECTED_WRITE`` window so it cannot be silently discarded.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_register_protection.h"

/**
 * @var s_tag
 * @brief Component tag used in every log line emitted by this driver.
 */
static const char* s_tag = "LPM";

/* =============================================================================
 * Graphics power domain
 * =============================================================================
 */

/**
 * @brief Poll a PDCTRGD status flag until it reads the wanted level.
 *
 * @details
 * Busy-polls the read-only graphics power-domain status register for at most
 * @p limit iterations (NASA Rule 2 bound); no PRCR window is needed.
 *
 * @param[in] mask     ``k_ra8_lpm_pdctr_*_mask`` bit to watch.
 * @param[in] want_set ``true`` to wait for set, ``false`` for clear.
 * @param[in] limit    Iteration bound (NASA Rule 2).
 * @return ``k_ra8_ok`` if the flag reached the level, else timeout.
 * @retval k_ra8_ok The watched bit read @p want_set within @p limit polls.
 * @retval k_ra8_err_hw_timeout The bit never reached @p want_set.
 * @pre limit > 0.
 * @pre mask names exactly one PDCTRGD bit.
 * @post No register is written.
 * @post At most @p limit reads of PDCTRGD were issued.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdctrgd_wait(uint8_t mask, bool want_set, uint32_t limit)
{
  for (uint32_t i = 0U; i < limit; ++i) {
    /* HUM Ch 11.2.14 "PDCTRGD : Graphics Power Domain Control Register", p 452 */
    const bool is_set = ((*ra8_lpm_sysc_reg8(k_ra8_lpm_pdctrgd_off) & mask) != 0U);
    if (is_set == want_set) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Start MOCO so the graphics power-gating controller has a clock.
 *
 * @details HUM Ch 11.5.1 p 480 requires MOCOCR.MCSTP = 0 before power gating;
 *          MOCOCR is PRC0 (CGC) so the clear runs in a CGC unlock window.
 * @pre Called on the power-on path with the domain still gated.
 * @pre Single-threaded init context or interrupts masked.
 * @post MOCOCR.MCSTP is 0 (MOCO running).
 * @post PRCR is re-locked on exit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_graphics_enable_moco(void)
{
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.18 "MOCOCR : MOCO Control Register", p 341 */
    volatile uint8_t* mococr = ra8_lpm_sysc_reg8(k_ra8_lpm_mococr_off);
    *mococr                  = (uint8_t)(*mococr & (uint8_t)~k_ra8_lpm_clock_stop_mask);
  }
}

/**
 * @brief Wait until PDCSF = 0 and PDPGSF = 1 (domain ready for the clear).
 *
 * @details HUM Ch 11.2.14 p 452 gates the PDDE clear on PDCSF = 0 then
 *          PDPGSF = 1; split out of ::ra8_lpm_graphics_power_on for size.
 * @param[in] limit Iteration bound handed to ::internal_pdctrgd_wait.
 * @return ``k_ra8_ok`` when both flags reached their level.
 * @retval k_ra8_ok PDCSF == 0 and PDPGSF == 1 within @p limit polls.
 * @retval k_ra8_err_hw_timeout A flag never settled.
 * @pre @p limit > 0.
 * @pre The controller is quiescent enough to make progress.
 * @post No register is written.
 * @post The return value reflects the first flag that failed to settle.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_graphics_confirm_ready(uint32_t limit)
{
  ra8_err_t err = internal_pdctrgd_wait((uint8_t)k_ra8_lpm_pdctr_pdcsf_mask, false, limit);
  RA8_RETURN_ON_ERROR(err, s_tag, "graphics_power_on: PDCSF busy");
  err = internal_pdctrgd_wait((uint8_t)k_ra8_lpm_pdctr_pdpgsf_mask, true, limit);
  return err;
}

/**
 * @brief Wait until PDCSF = 0 and PDPGSF = 0 (gating finished, domain live).
 *
 * @details Gating is complete when the controller is idle and the domain is
 *          no longer gated; split out of ::ra8_lpm_graphics_power_on for size.
 * @param[in] limit Iteration bound handed to ::internal_pdctrgd_wait.
 * @return ``k_ra8_ok`` when both flags reached their level.
 * @retval k_ra8_ok PDCSF == 0 and PDPGSF == 0 within @p limit polls.
 * @retval k_ra8_err_hw_timeout A flag never settled.
 * @pre @p limit > 0.
 * @pre ::internal_graphics_clear_pdde has requested power-on.
 * @post No register is written.
 * @post The return value reflects the first flag that failed to settle.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_graphics_confirm_powered(uint32_t limit)
{
  ra8_err_t err = internal_pdctrgd_wait((uint8_t)k_ra8_lpm_pdctr_pdcsf_mask, false, limit);
  /* GCOVR_EXCL_BR_START -- internal_pdctrgd_wait() error edge; PDCSF always clears off-target */
  RA8_RETURN_ON_ERROR(err, s_tag, "graphics_power_on: PDCSF stuck");
  /* GCOVR_EXCL_BR_STOP */
  err = internal_pdctrgd_wait((uint8_t)k_ra8_lpm_pdctr_pdpgsf_mask, false, limit);
  return err;
}

/**
 * @brief Clear PDDE under the PRC1 unlock so the domain powers on.
 *
 * @details PDCTRGD is PRC1-protected (HUM Ch 13.1 Table 13.1 p 521); without
 *          the unlock the clear is dropped. PDDE = 0 powers the domain ON.
 * @pre The domain was confirmed ready by ::internal_graphics_confirm_ready.
 * @pre Single-threaded init context or interrupts masked.
 * @post PDCTRGD.PDDE is 0 (power-on requested).
 * @post PRCR is re-locked on exit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_graphics_clear_pdde(void)
{
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_lpm)
  {
    /* HUM Ch 11.2.14 "PDCTRGD : Graphics Power Domain Control Register", p 452 */
    *ra8_lpm_sysc_reg8(k_ra8_lpm_pdctrgd_off) = 0U;
  }
}

[[nodiscard]] ra8_err_t ra8_lpm_graphics_power_on(uint32_t timeout_iters)
{
  if (timeout_iters == 0U) {
    ra8_log_error(s_tag, "graphics_power_on: timeout_iters == 0");
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.14 "PDCTRGD : Graphics Power Domain Control Register", p 452 */
  /* Already powered (PDPGSF = 0)? Idempotent no-op for multi-driver init. */
  if ((*ra8_lpm_sysc_reg8(k_ra8_lpm_pdctrgd_off) & (uint8_t)k_ra8_lpm_pdctr_pdpgsf_mask) == 0U) {
    return k_ra8_ok;
  }

  internal_graphics_enable_moco();

  ra8_err_t err = internal_graphics_confirm_ready(timeout_iters);
  RA8_RETURN_ON_ERROR(err, s_tag, "graphics_power_on: not ready");

  internal_graphics_clear_pdde();

  err = internal_graphics_confirm_powered(timeout_iters);
  /* GCOVR_EXCL_BR_START -- internal_graphics_confirm_powered() error edge after clearing PDDE */
  RA8_RETURN_ON_ERROR(err, s_tag, "graphics_power_on: still gated");
  /* GCOVR_EXCL_BR_STOP */

  ra8_log_info(s_tag, "graphics power domain on");
  return k_ra8_ok;
}

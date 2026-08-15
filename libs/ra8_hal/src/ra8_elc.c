/**
 * @file ra8_elc.c
 * @brief Event Link Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * See ``ra8_elc.h`` for the API contract. This file owns every
 * write to the ELC register block (HUM Ch 19, p 817..836). Layout
 * and the ELSEGR three-step write-protect sequence are
 * cross-verified against FSP `r_elc.c` and FSP `R_ELC_Type` in
 * `R7KA8D2KF_core0.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_elc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "ELC";

/* =============================================================================
 * Internal register accessors
 * =============================================================================
 */

/**
 * @brief Pointer to the 8-bit ELCR register.
 *
 * @details HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 819.
 *
 * @return Volatile pointer into the ELC register block.
 */
RA8_HW_REGISTER_ACCESS
RA8_INTERNAL static inline volatile uint8_t* internal_elcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_elc_base_addr + (uintptr_t)k_ra8_elc_off_elcr);
}

/**
 * @brief Pointer to ELSR slot @p index.
 *
 * @details FSP `R_ELC_ELSR_Type` is 16-bit ELS field + 16-bit
 * reserved (4-byte stride); the driver only writes the 16-bit head.
 * HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 820.
 *
 * @param[in] index ELSR slot 0..k_ra8_elc_elsr_count - 1.
 * @return Volatile pointer into the ELC register block.
 */
RA8_HW_REGISTER_ACCESS
RA8_INTERNAL static inline volatile uint16_t* internal_elsr(uint8_t index)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_elc_base_addr + (uintptr_t)k_ra8_elc_off_elsr0 +
                              ((uintptr_t)index * (uintptr_t)k_ra8_elc_elsr_stride));
}

/**
 * @brief Pointer to the 8-bit ELSEGRn (BY) register at index @p group.
 *
 * @details ELSEGR[0..3] live at 0x04/0x08/0x0C/0x10 (8-bit register
 * with 3 bytes reserved -- 4-byte stride). HUM Ch 19.2.2
 * "ELSEGRn : Event Link Software Event Generation Register n",
 * p 819.
 *
 * @param[in] group Software event index 0..k_ra8_elc_segr_count - 1.
 * @return Volatile pointer into the ELC register block.
 */
RA8_HW_REGISTER_ACCESS
RA8_INTERNAL static inline volatile uint8_t* internal_elsegr(uint8_t group)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_elc_base_addr + (uintptr_t)k_ra8_elc_off_elsegr0 +
                             ((uintptr_t)group * (uintptr_t)k_ra8_elc_elsegr_stride));
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_elc_init(void)
{
  ra8_log_info(s_tag, "ra8_elc_init");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_elc);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "elc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Clear every ELSR slot before flipping ELCON so stale routes
   * don't fire spuriously.
   * HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 820 */
  for (uint8_t i = 0U; i < k_ra8_elc_elsr_count; ++i) {
    *internal_elsr(i) = 0U;
  }

  /* HUM Ch 19.2.2 "ELSEGRn : Event Link Software Event Generation",
   * p 819 -- writing 0 clears WI/WE/SEG so subsequent
   * ra8_elc_software_trigger calls start from a clean state. */
  for (uint8_t g = 0U; g < k_ra8_elc_segr_count; ++g) {
    *internal_elsegr(g) = k_ra8_elc_elsegr_step_unlock;
  }

  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 819 */
  *internal_elcr() = (uint8_t)(1U << k_ra8_elcr_bit_elcon);
  return k_ra8_ok;
}

ra8_err_t ra8_elc_deinit(void)
{
  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 819 */
  *internal_elcr() = 0U;
  return ra8_mstp_disable(k_ra8_mstp_elc);
}

ra8_err_t ra8_elc_link(uint8_t elsr_index, ra8_elc_event_t event)
{
  if (elsr_index >= k_ra8_elc_elsr_count) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 820 */
  *internal_elsr(elsr_index) = (uint16_t)event;
  return k_ra8_ok;
}

ra8_err_t ra8_elc_unlink(uint8_t elsr_index)
{
  if (elsr_index >= k_ra8_elc_elsr_count) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 820 */
  *internal_elsr(elsr_index) = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_elc_software_trigger(uint8_t event_index)
{
  if (event_index >= k_ra8_elc_segr_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 19.2.2 "ELSEGRn : Event Link Software Event Generation
   * Register n", p 819. ELSEGRn is write-protected by WI/WE; SEG
   * latches only after the documented three-write sequence. The
   * same sequence is used by FSP R_ELC_SoftwareEventGenerate
   * (`ELC_ELSEGRN_STEP1..3`). */
  volatile uint8_t* elsegr = internal_elsegr(event_index);
  *elsegr                  = k_ra8_elc_elsegr_step_unlock;
  *elsegr                  = k_ra8_elc_elsegr_step_arm;
  *elsegr                  = k_ra8_elc_elsegr_step_trigger;
  return k_ra8_ok;
}

ra8_err_t ra8_elc_is_enabled(bool* out_enabled)
{
  RA8_CHECK_NULL_PTR(out_enabled, s_tag, "is_enabled out");
  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 819 */
  const uint8_t val = *internal_elcr();
  *out_enabled      = ((val & (uint8_t)(1U << k_ra8_elcr_bit_elcon)) != 0U);
  return k_ra8_ok;
}

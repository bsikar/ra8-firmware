/**
 * @file ra_elc.c
 * @brief Event Link Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * rewrite. See ``ra_elc.h`` for the API contract. This
 * file owns every write to the ELC register block (HUM Ch 19,
 * pages 817..836).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_elc.h"

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ELC";

/**
 * @enum ra_elcr_bit_t
 * @brief ELCR bit positions.
 */
typedef enum : uint8_t {
  k_ra_elcr_bit_elcon = 7U, /**< ELCR.ELCON -- global enable. */
} ra_elcr_bit_t;

/* =============================================================================
 * Internal register accessors
 * =============================================================================
 */

static volatile uint8_t* internal_elcr(void)
{
  return (volatile uint8_t*)(k_ra_elc_base_addr + k_ra_elc_off_elcr);
}

static volatile uint16_t* internal_elsr(uint8_t index)
{
  /* HUM Ch 19 R_ELC_ELSR_Type is a 16-bit ELS field + 16-bit reserved
   * (4-byte stride) -- we access the 16-bit head of each slot. */
  return (volatile uint16_t*)(k_ra_elc_base_addr + k_ra_elc_off_elsr0 +
                              ((uintptr_t)index * (uintptr_t)k_ra_elc_elsr_stride));
}

static volatile uint8_t* internal_elsegr(uint8_t group)
{
  /* ELSEGR0..3 live at 0x04/0x08/0x0C/0x10 (8-bit reg, 4-byte stride);
   * the driver today only addresses groups 0 and 1. */
  return (volatile uint8_t*)(k_ra_elc_base_addr + k_ra_elc_off_elsegr0 +
                             ((uintptr_t)group * (uintptr_t)k_ra_elc_elsegr_stride));
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_elc_init(void)
{
  ra_log_info(s_tag, "ra_elc_init");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_elc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "elc_init: mstp enable");

  /* Clear every ELSR slot before flipping ELCON so stale routes
   * don't fire spuriously.
   * HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 817 */
  for (uint8_t i = 0U; i < k_ra_elc_elsr_count; ++i) {
    *internal_elsr(i) = 0U;
  }

  /* HUM Ch 19.2.2 "ELSEGR0..3 : Event Link Software Event Generation", p 817 */
  for (uint8_t g = 0U; g < k_ra_elc_segr_count; ++g) {
    *internal_elsegr(g) = 0U;
  }

  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 817 */
  *internal_elcr() = (uint8_t)(1U << k_ra_elcr_bit_elcon);
  return k_ra_ok;
}

ra_err_t ra_elc_deinit(void)
{
  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 817 */
  *internal_elcr() = 0U;
  return ra_mstp_disable(k_ra_mstp_elc);
}

ra_err_t ra_elc_enable(bool enable)
{
  if (enable) {
    /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
    const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_elc);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "elc_enable: mstp enable");
  }
  /* HUM Ch 19.2.1 "ELCR : Event Link Control Register", p 817 */
  volatile uint8_t* elcr = internal_elcr();
  if (enable) {
    *elcr = (uint8_t)(1U << k_ra_elcr_bit_elcon);
  } else {
    *elcr                  = 0U;
    const ra_err_t mst_err = ra_mstp_disable(k_ra_mstp_elc);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "elc_enable: mstp disable");
  }
  ra_log_info_val(s_tag, "elc_enable", enable ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_elc_link(uint8_t elsr_index, ra_elc_event_t event)
{
  if (elsr_index >= k_ra_elc_elsr_count) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 817 */
  *internal_elsr(elsr_index) = (uint16_t)event;
  return k_ra_ok;
}

ra_err_t ra_elc_unlink(uint8_t elsr_index)
{
  if (elsr_index >= k_ra_elc_elsr_count) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 19.2.3 "ELSRn : Event Link Setting Register n", p 817 */
  *internal_elsr(elsr_index) = 0U;
  return k_ra_ok;
}

ra_err_t ra_elc_software_trigger(uint8_t group, uint8_t value)
{
  if (group >= k_ra_elc_segr_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 19.2.2 "ELSEGRn : Event Link Software Event Generation", p 817 */
  *internal_elsegr(group) = value;
  return k_ra_ok;
}

ra_err_t ra_elc_is_enabled(bool* out_enabled)
{
  RA_CHECK_NULL_PTR(out_enabled, s_tag, "is_enabled out");
  const uint8_t val = *internal_elcr();
  *out_enabled      = ((val & (uint8_t)(1U << k_ra_elcr_bit_elcon)) != 0U);
  return k_ra_ok;
}

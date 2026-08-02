/**
 * @file ra8_pwr.c
 * @brief Low Power Mode + clock-domain wrapper implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. See ``ra8_pwr.h`` for the API contract.
 * Implementation depth is intentionally minimal:
 *
 * - Module request / release forward straight to ra8_mstp.
 * - Wake-source toggling reads / modifies / writes WUPEN0 or
 * WUPEN1 directly. The full WUPEN field map will be added in
 * (RTC, ULPT, AGT bring-up).
 * - ``ra8_pwr_enter_sleep()`` issues a single WFI on the target
 * and is a no-op in ``RA8_OFF_TARGET``.
 * - ``ra8_pwr_enter_software_standby()`` validates that at least
 * one wake source is armed, then sets LPMD and SLEEPDEEP and
 * issues WFI. The WFI is host-no-op so unit tests do not stall.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_pwr.h"

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"
#include "ra8_lpm_regs.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_system_regs.h"

/* =============================================================================
 * Local constants
 * =============================================================================
 */

static const char* s_tag = "PWR";

/** @brief Low-byte mask for register/bit decomposition. */
typedef enum : uint16_t {
  k_pwr_byte_mask = 0xFFU, /**< Pwr byte mask. */
} pwr_mask_t;

/**
 * @enum ra8_pwr_dim_t
 * @brief WUPEN address-decode bounds.
 */
typedef enum : uint8_t {
  k_ra8_pwr_wupen_count = 2U,  /**< WUPEN0 + WUPEN1.      */
  k_ra8_pwr_wupen_bits  = 32U, /**< 32 bits per register. */
} ra8_pwr_dim_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pointer to one of the 32-bit WUPEN registers.
 *
 * @details
 * The WUPEN registers live inside the SYSC block at the offsets
 * defined in ``ra8_lpm_regs.h``.
 */
static volatile uint32_t* internal_wupen_ptr(uint8_t reg)
{
  const uintptr_t base = k_ra8_system_base_addr;
  if (reg == 0U) {
    return (volatile uint32_t*)(base + (uintptr_t)k_ra8_lpm_wupen0_off);
  }
  return (volatile uint32_t*)(base + (uintptr_t)k_ra8_lpm_wupen1_off);
}

/**
 * @brief Decode and validate a packed wake source.
 *
 * @param[in] source Packed identifier.
 * @param[out] out_reg Register index 0 or 1.
 * @param[out] out_bit Bit position 0..31.
 * @return ``true`` on valid decode.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_decode_wake(ra8_pwr_wake_t source, uint8_t* out_reg, uint8_t* out_bit)
{
  const uint8_t reg = (uint8_t)(((uint16_t)source >> 8) & k_pwr_byte_mask);
  const uint8_t bit = (uint8_t)((uint16_t)source & k_pwr_byte_mask);
  if (reg >= k_ra8_pwr_wupen_count) {
    return false;
  }
  if (bit >= k_ra8_pwr_wupen_bits) {
    return false;
  }
  *out_reg = reg;
  *out_bit = bit;
  return true;
}

/**
 * @brief WFI wrapper. No-op on the host build.
 *
 * @details
 * Routes through the ``ra8_hw_intrinsics`` seam: on the Cortex-M85 target it
 * expands to ``wfi``; on the host the stub returns at once so unit tests can
 * call ``ra8_pwr_enter_sleep()`` without stalling.
 *
 * @pre Module state is consistent.
 * @pre The desired sleep-mode registers have been written.
 * @post The core has parked until a wake event (target) or returned (host).
 * @post No module state is mutated by the wait itself.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void internal_wfi(void)
{
  ra8_hw_wfi();
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_pwr_init(void)
{
  ra8_log_info(s_tag, "ra8_pwr_init");

  const ra8_err_t mst_err = ra8_mstp_init();
  if (mst_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "mstp init failed", (uint32_t)mst_err);
    return mst_err;
  }

  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  *internal_wupen_ptr(0U) = 0U;
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  *internal_wupen_ptr(1U) = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_pwr_module_request(ra8_mstp_t id)
{
  return ra8_mstp_enable(id);
}

ra8_err_t ra8_pwr_module_release(ra8_mstp_t id)
{
  return ra8_mstp_disable(id);
}

ra8_err_t ra8_pwr_set_wake_source(ra8_pwr_wake_t source)
{
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode_wake(source, &reg, &bit)) {
    ra8_log_error_val(s_tag, "set_wake: invalid source", (uint32_t)source);
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  volatile uint32_t* p    = internal_wupen_ptr(reg);
  const uint32_t     mask = (uint32_t)1U << bit;
  *p                      = *p | mask;
  return k_ra8_ok;
}

ra8_err_t ra8_pwr_clear_wake_source(ra8_pwr_wake_t source)
{
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode_wake(source, &reg, &bit)) {
    ra8_log_error_val(s_tag, "clear_wake: invalid source", (uint32_t)source);
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  volatile uint32_t* p    = internal_wupen_ptr(reg);
  const uint32_t     mask = (uint32_t)1U << bit;
  *p                      = *p & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_pwr_wake_source_is_enabled(ra8_pwr_wake_t source, bool* out_enabled)
{
  RA8_CHECK_NULL_PTR(out_enabled, s_tag, "wake_source_is_enabled: out_enabled");
  uint8_t reg = 0U;
  uint8_t bit = 0U;
  if (!internal_decode_wake(source, &reg, &bit)) {
    return k_ra8_err_invalid_arg;
  }
  volatile const uint32_t* p    = internal_wupen_ptr(reg);
  const uint32_t           mask = (uint32_t)1U << bit;
  *out_enabled                  = ((*p & mask) != 0U);
  return k_ra8_ok;
}

void ra8_pwr_enter_sleep(void)
{
  internal_wfi();
}

ra8_err_t ra8_pwr_enter_software_standby(void)
{
  /* Validate at least one wake source is armed -- entering Software
   * Standby with WUPEN0 == WUPEN1 == 0 wedges the chip. */
  /* HUM Ch 14.2.19 "WUPEN0", p 550 */
  const uint32_t wupen0 = *internal_wupen_ptr(0U);
  /* HUM Ch 14.2.20 "WUPEN1", p 552 */
  const uint32_t wupen1 = *internal_wupen_ptr(1U);
  if ((wupen0 == 0U) && (wupen1 == 0U)) {
    ra8_log_error(s_tag, "software_standby with no wake source armed");
    return k_ra8_err_invalid_state;
  }

  /* The full LPMD + SLEEPDEEP + SBYCR sequence is implemented in
   * along with the RTC alarm bring-up. For the
   * helper just issues WFI; the chip stays in normal sleep until
   * the wake source fires. This keeps the host build a no-op and
   * lets driver code that calls this function compile and link
   * cleanly. */
  internal_wfi();
  return k_ra8_ok;
}

ra8_err_t ra8_pwr_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz)
{
  return ra8_cgc_get_clock_hz(id, out_hz);
}

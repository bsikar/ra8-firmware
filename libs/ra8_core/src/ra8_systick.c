/**
 * @file ra8_systick.c
 * @brief Cortex-M85 SysTick + DWT cycle-counter timebase implementation.
 *
 * @details
 * Implements ::ra8_systick.h against the Arm v8-M System Control Space (PPB
 * window 0xE000Exxx). SysTick is programmed through SYST_CSR / SYST_RVR /
 * SYST_CVR; the DWT free-running cycle counter is unlocked via DEMCR.TRCENA,
 * started via DWT_CTRL.CYCCNTENA, and sampled from DWT_CYCCNT.
 *
 * These are Arm-architecture registers, so the inline comments reference the
 * Arm v8-M Architecture Reference Manual (the "Arm v8-M ARM") rather than the
 * RA8D2 Hardware User's Manual. On a host build the SCS window is backed by the
 * fake MMIO map, so the writes are observable to unit tests and the reload /
 * range logic runs exactly as on silicon -- there is no `RA8_OFF_TARGET` guard
 * around the accesses here, unlike a driver that reaches a real peripheral bus.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_systick.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_systick";

/**
 * @enum ra8_systick_reg_addr_t
 * @brief Arm v8-M SCS timebase register addresses (PPB 0xE000Exxx).
 * @details Arm v8-M ARM: SysTick registers live in the System Control Block and
 *          DWT registers in the Data Watchpoint and Trace unit. Both are mapped
 *          into the fake core window (0xE0000000) on a host build, real PPB on
 *          silicon.
 */
typedef enum : uintptr_t {
  k_ra8_systick_csr = 0xE000E010UL, /**< SYST_CSR control and status.      */
  k_ra8_systick_rvr = 0xE000E014UL, /**< SYST_RVR reload value.            */
  k_ra8_systick_cvr = 0xE000E018UL, /**< SYST_CVR current value.           */
  k_ra8_dwt_demcr   = 0xE000EDFCUL, /**< DEMCR: bit 24 TRCENA unlocks DWT. */
  k_ra8_dwt_ctrl    = 0xE0001000UL, /**< DWT_CTRL: bit 0 CYCCNTENA.        */
  k_ra8_dwt_cyccnt  = 0xE0001004UL, /**< DWT_CYCCNT free-running counter.  */
} ra8_systick_reg_addr_t;

/**
 * @enum ra8_systick_bit_t
 * @brief Control bits for SYST_CSR, DEMCR, and DWT_CTRL.
 * @details Arm v8-M ARM register descriptions for SYST_CSR, DEMCR, and
 * DWT_CTRL.
 */
typedef enum : uint32_t {
  k_ra8_systick_csr_enable    = 0x00000001UL, /**< SYST_CSR.ENABLE (bit 0).    */
  k_ra8_systick_csr_tickint   = 0x00000002UL, /**< SYST_CSR.TICKINT (bit 1).   */
  k_ra8_systick_csr_clksource = 0x00000004UL, /**< SYST_CSR.CLKSOURCE (bit 2). */
  k_ra8_dwt_demcr_trcena      = 0x01000000UL, /**< DEMCR.TRCENA (bit 24).      */
  k_ra8_dwt_ctrl_cyccntena    = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA (bit 0). */
} ra8_systick_bit_t;

/**
 * @brief Typed pointer to a 32-bit SCS timebase register.
 *
 * @details Trivial address-cast helper shared by every access below, so no
 *          register store or load in this file is written as a raw
 *          pointer-cast dereference.
 *
 * @param[in] addr One of ::ra8_systick_reg_addr_t.
 *
 * @return Volatile pointer for a single read or write.
 * @retval (volatile uint32_t*)addr Always the aliasing register pointer.
 *
 * @pre @p addr is a valid SCS timebase-register address.
 * @pre The PPB / fake core window is accessible.
 * @post No state changed by forming the pointer.
 * @post The returned pointer aliases the live register.
 *
 * @note Arm v8-M ARM B11 "System Control Block". Trivially thread-safe.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS RA8_INTERNAL static inline volatile uint32_t*
internal_systick_reg(ra8_systick_reg_addr_t addr)
{
  return (volatile uint32_t*)addr;
}

ra8_err_t ra8_systick_reload_for(uint32_t cpu_hz, uint32_t tick_hz, uint32_t* out_reload)
{
  RA8_CHECK_NULL_PTR(out_reload, s_tag, "out_reload is NULL");

  /* Reject a zero clock or a zero tick rate: both make the reload arithmetic
   * meaningless (a 0 Hz core, or a division by zero). */
  if ((cpu_hz == 0U) || (tick_hz == 0U)) {
    ra8_log_error(s_tag, "cpu_hz / tick_hz must be non-zero");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t ticks = cpu_hz / tick_hz;
  if (ticks == 0U) {
    /* Clock slower than one tick period -> reload would underflow. */
    ra8_log_error(s_tag, "cpu_hz below one tick period");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t reload = ticks - 1U;
  if (reload > (uint32_t)k_ra8_systick_rvr_max) {
    /* Too high for the 24-bit SYST_RVR field: refuse rather than truncate,
     * which would silently make the tick far too fast. */
    ra8_log_error(s_tag, "reload exceeds 24-bit SysTick range");
    return k_ra8_err_out_of_range;
  }

  *out_reload = reload;
  return k_ra8_ok;
}

ra8_err_t ra8_systick_configure(uint32_t reload, ra8_systick_clock_source_t src, bool tick_irq)
{
  if (reload > (uint32_t)k_ra8_systick_rvr_max) {
    ra8_log_error(s_tag, "reload exceeds 24-bit SysTick range");
    return k_ra8_err_out_of_range;
  }

  /* Arm v8-M ARM: SYST_CSR -- clear ENABLE first so the reload/current writes
   * land on a stopped counter, then re-enable with the requested config. */
  *internal_systick_reg(k_ra8_systick_csr) = 0U;
  *internal_systick_reg(k_ra8_systick_rvr) = reload;
  *internal_systick_reg(k_ra8_systick_cvr) = 0U;

  uint32_t csr_val = (uint32_t)k_ra8_systick_csr_enable;
  if (tick_irq) {
    csr_val |= (uint32_t)k_ra8_systick_csr_tickint;
  }
  if (src == k_ra8_systick_clk_cpu) {
    csr_val |= (uint32_t)k_ra8_systick_csr_clksource;
  }
  *internal_systick_reg(k_ra8_systick_csr) = csr_val;
  return k_ra8_ok;
}

ra8_err_t ra8_systick_set_reload(uint32_t reload)
{
  if (reload > (uint32_t)k_ra8_systick_rvr_max) {
    ra8_log_error(s_tag, "reload exceeds 24-bit SysTick range");
    return k_ra8_err_out_of_range;
  }

  /* Arm v8-M ARM: re-arm the reload, then any write to SYST_CVR clears it so
   * the next count starts from the new reload. The SYST_CSR control bits are
   * deliberately left as-is. */
  *internal_systick_reg(k_ra8_systick_rvr) = reload;
  *internal_systick_reg(k_ra8_systick_cvr) = 0U;
  return k_ra8_ok;
}

uint32_t ra8_systick_current_value(void)
{
  /* Arm v8-M ARM: SYST_CVR -- reading it is side-effect free (only a write
   * clears the counter). */
  return *internal_systick_reg(k_ra8_systick_cvr);
}

void ra8_dwt_cyccnt_enable(void)
{
  /* Arm v8-M ARM: DEMCR.TRCENA unlocks the DWT unit; DWT_CTRL.CYCCNTENA then
   * starts DWT_CYCCNT. Read-modify-write preserves any other trace bits. */
  *internal_systick_reg(k_ra8_dwt_demcr) |= (uint32_t)k_ra8_dwt_demcr_trcena;
  *internal_systick_reg(k_ra8_dwt_ctrl) |= (uint32_t)k_ra8_dwt_ctrl_cyccntena;
}

void ra8_dwt_cyccnt_reset(void)
{
  /* Arm v8-M ARM: DWT_CYCCNT is writable; zero it to start a measured window.
   */
  *internal_systick_reg(k_ra8_dwt_cyccnt) = 0U;
}

uint32_t ra8_dwt_cyccnt_read(void)
{
  /* Arm v8-M ARM: DWT_CYCCNT free-running cycle counter, read side-effect free.
   */
  return *internal_systick_reg(k_ra8_dwt_cyccnt);
}

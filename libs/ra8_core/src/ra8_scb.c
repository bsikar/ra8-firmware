/**
 * @file ra8_scb.c
 * @brief Cortex-M85 System Control Block driver implementation.
 *
 * @details
 * Implements ::ra8_scb.h against the Arm v8-M System Control Block (PPB window
 * 0xE000ED00). The fault-status read loads the eight status / address
 * registers with plain volatile reads; the VTOR helpers write and read the
 * Vector Table Offset Register; the trace helpers read and set DEMCR.TRCENA.
 *
 * These are Arm-architecture registers, so the inline comments reference the
 * Arm v8-M Architecture Reference Manual ("Arm v8-M ARM") rather than the RA8D2
 * Hardware User's Manual -- the HUM defers the Cortex-M85 core registers to the
 * Arm v8-M ARM. Every access goes through ::internal_scb_reg
 * (the pattern ra8_cache.c uses), so on a host build the fake MMIO map backs
 * the window and the reads / writes are observable to unit tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_scb.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/**
 * @enum ra8_scb_reg_addr_t
 * @brief Arm v8-M SCB register addresses (PPB window 0xE000EDxx).
 * @details Arm v8-M ARM B3.2 "System Control Block" and the Debug / Security
 *          Extension register views. Backed by the fake core window
 *          (0xE0000000) on a host build, real PPB on silicon.
 */
typedef enum : uintptr_t {
  k_ra8_scb_vtor  = 0xE000ED08UL, /**< Vector Table Offset Register (VTOR).    */
  k_ra8_scb_cfsr  = 0xE000ED28UL, /**< Configurable Fault Status (CFSR).       */
  k_ra8_scb_hfsr  = 0xE000ED2CUL, /**< HardFault Status Register (HFSR).       */
  k_ra8_scb_dfsr  = 0xE000ED30UL, /**< Debug Fault Status Register (DFSR).     */
  k_ra8_scb_mmfar = 0xE000ED34UL, /**< MemManage Fault Address (MMFAR). */
  k_ra8_scb_bfar  = 0xE000ED38UL, /**< BusFault Address Register (BFAR).       */
  k_ra8_scb_afsr  = 0xE000ED3CUL, /**< Auxiliary Fault Status (AFSR).          */
  k_ra8_scb_sfsr  = 0xE000EDE4UL, /**< SecureFault Status (SFSR, S-banked).    */
  k_ra8_scb_sfar  = 0xE000EDE8UL, /**< SecureFault Address (SFAR, S-banked).   */
  k_ra8_scb_demcr = 0xE000EDFCUL, /**< Debug Exception + Monitor Ctrl (DEMCR). */
} ra8_scb_reg_addr_t;

/**
 * @enum ra8_scb_bits_t
 * @brief Named bit fields within the SCB registers this driver touches.
 * @details Arm v8-M ARM (Debug) DEMCR register description.
 */
typedef enum : uint32_t {
  k_ra8_scb_demcr_trcena = 0x01000000UL, /**< DEMCR.TRCENA -- bit 24. */
} ra8_scb_bits_t;

/**
 * @brief Typed pointer to a 32-bit SCB register.
 *
 * @details Trivial address-cast helper so each access site reads
 *          `*internal_scb_reg(k_...)` instead of an inline cast. Always
 * inlined.
 *
 * @param[in] addr One of ::ra8_scb_reg_addr_t.
 *
 * @return Volatile pointer for a single read or write.
 * @retval (volatile uint32_t*)addr  Alias of the live register.
 *
 * @pre @p addr is a valid SCB register address.
 * @pre The PPB / fake core window is accessible.
 * @post No state changed by forming the pointer.
 * @post The returned pointer aliases the live register.
 *
 * @note Arm v8-M ARM B3.2 "System Control Block". Trivially thread-safe.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS RA8_INTERNAL static inline volatile uint32_t*
internal_scb_reg(ra8_scb_reg_addr_t addr)
{
  return (volatile uint32_t*)addr;
}

ra8_err_t ra8_scb_read_fault_status(ra8_scb_fault_status_t* out)
{
  /** @brief Module log tag -- block scope: this is the only function that logs.
   */
  static const char* const tag = "ra8_scb";

  RA8_CHECK_NULL_PTR(out, tag, "read_fault_status: out");
  /* Arm v8-M ARM B3.2 "System Control Block": configurable / hard / debug
   * fault status, then the MemManage / BusFault / Auxiliary registers. Plain
   * reads -- no side effect, nothing is cleared. */
  out->cfsr  = *internal_scb_reg(k_ra8_scb_cfsr);
  out->hfsr  = *internal_scb_reg(k_ra8_scb_hfsr);
  out->dfsr  = *internal_scb_reg(k_ra8_scb_dfsr);
  out->mmfar = *internal_scb_reg(k_ra8_scb_mmfar);
  out->bfar  = *internal_scb_reg(k_ra8_scb_bfar);
  out->afsr  = *internal_scb_reg(k_ra8_scb_afsr);
  /* Arm v8-M ARM (Security Extension): SFSR / SFAR are banked to the Secure
   * state. Read from Secure they carry the real cause / address; read from
   * Non-secure they are RAZ -- never a fault -- so this is safe in either
   * world without a guard. */
  out->sfsr = *internal_scb_reg(k_ra8_scb_sfsr);
  out->sfar = *internal_scb_reg(k_ra8_scb_sfar);
  return k_ra8_ok;
}

void ra8_scb_set_vtor(uintptr_t base)
{
  /* Arm v8-M ARM B3.2.2 "VTOR, Vector Table Offset Register": point the core
   * at a new vector-table base. Hardware ignores the low alignment bits. */
  *internal_scb_reg(k_ra8_scb_vtor) = (uint32_t)base;
}

uintptr_t ra8_scb_get_vtor(void)
{
  /* Arm v8-M ARM B3.2.2 "VTOR": current vector-table base address. */
  return (uintptr_t)*internal_scb_reg(k_ra8_scb_vtor);
}

bool ra8_scb_trace_enabled(void)
{
  /* Arm v8-M ARM (Debug) DEMCR.TRCENA (bit 24): trace subsystem power. */
  const uint32_t demcr = *internal_scb_reg(k_ra8_scb_demcr);
  return (demcr & (uint32_t)k_ra8_scb_demcr_trcena) != 0U;
}

void ra8_scb_trace_enable(void)
{
  /* Arm v8-M ARM (Debug) DEMCR.TRCENA (bit 24): set it, preserving every
   * other bit -- a read-modify-write, not a blind store. */
  const uint32_t demcr               = *internal_scb_reg(k_ra8_scb_demcr);
  *internal_scb_reg(k_ra8_scb_demcr) = demcr | (uint32_t)k_ra8_scb_demcr_trcena;
}

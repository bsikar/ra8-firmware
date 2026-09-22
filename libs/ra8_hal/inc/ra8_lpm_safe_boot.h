/**
 * @file ra8_lpm_safe_boot.h
 * @brief Self-healing LPM register reset for very early boot
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * SYSRESETREQ on the RA8D2 resets the Cortex-M85 core but NOT the
 * SYSC LPM register block (LPSCR / SBYCR / DPSBYCR live in a
 * separate reset domain). If a previous firmware wrote
 * ``LPSCR.LPMD = 0x4`` (software standby) and executed WFI, the
 * standby configuration survives a debugger-issued SYSRESETREQ.
 *
 * On the next boot, the very first WFI -- including those inside
 * J-Link's RAMCode helper -- re-enters software standby, hanging the
 * flash sequence. The board appears bricked until a true PORST (full
 * power loss) clears LPSCR.
 *
 * ``ra8_lpm_safe_boot()`` is the antidote: it unlocks PRCR.PRC1,
 * restores LPSCR / SBYCR / DPSBYCR to their cold-reset values, then
 * relocks PRCR. Calling it as the first instruction of
 * ``Reset_Handler`` (before ``.data`` copy and before ``SystemInit``)
 * guarantees the chip is in a sane LPM state by the time any other
 * code runs.
 *
 * The helper is intentionally:
 *  - header-only (callable before ``.data`` is copied -- no globals);
 *  - free of dependencies on other HAL headers;
 *  - inlined so a stale ``s_tag`` symbol or logging path cannot reach
 *    it from another translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_lpm_safe_boot_prcr_t
 * @brief PRCR keyed values used by ra8_lpm_safe_boot.
 *
 * @details
 * HUM Ch 11.2.18 p 456: PRCR key = 0xA5 in bits 15:8, PRC1 = bit 1
 * gates the LPM/SYSC register group.
 */
typedef enum : uint16_t {
  k_ra8_lpm_safe_boot_prcr_unlock = 0xA502U, /**< Key + PRC1 = 1.     */
  k_ra8_lpm_safe_boot_prcr_relock = 0xA500U, /**< Key only, PRC1 = 0. */
} ra8_lpm_safe_boot_prcr_t;

/**
 * @enum ra8_lpm_safe_boot_addr_t
 * @brief Absolute addresses of the four SYSC LPM registers this restores.
 *
 * @details
 * uintptr_t, per the project rule for address enums: on the 32-bit RA8D2
 * target it is uint32_t, while on the 64-bit unit-test host a uint32_t enum
 * would truncate the cast and produce a wrong pointer. This header predates
 * ra8_sysc_regs.h and is deliberately self-contained -- it is included from
 * the reset path, before any register-bank header is available.
 *
 * @invariant Each value is the absolute address of the register its name
 * gives; the HUM chapters are cited on the accesses in ra8_lpm_safe_boot().
 * @see ra8_lpm_safe_boot()
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_lpm_safe_boot_prcr_addr    = 0x4001E3FAUL, /**< SYSC.PRCR, 16-bit.   */
  k_ra8_lpm_safe_boot_sbycr_addr   = 0x4001E00CUL, /**< SYSC.SBYCR, 8-bit.   */
  k_ra8_lpm_safe_boot_lpscr_addr   = 0x4001EA90UL, /**< SYSC.LPSCR, 8-bit.   */
  k_ra8_lpm_safe_boot_dpsbycr_addr = 0x4001EA00UL, /**< SYSC.DPSBYCR, 8-bit. */
} ra8_lpm_safe_boot_addr_t;

/**
 * @enum ra8_lpm_safe_boot_val_t
 * @brief Cold-reset values for LPSCR / SBYCR / DPSBYCR.
 *
 * @details
 * Per HUM Ch 11.2.18 / 11.2.20 / 11.2.21. Restoring these guarantees
 * the next WFI is plain Sleep, not Software Standby.
 */
typedef enum : uint8_t {
  k_ra8_lpm_safe_boot_lpscr_sleep   = 0x00U, /**< LPMD = Sleep mode. */
  k_ra8_lpm_safe_boot_sbycr_reset   = 0x40U, /**< OPE = 1, rest 0.   */
  k_ra8_lpm_safe_boot_dpsbycr_reset = 0x14U, /**< DCSSMODE = 01b.    */
} ra8_lpm_safe_boot_val_t;

/**
 * @brief Restore LPSCR / SBYCR / DPSBYCR to cold-reset values.
 *
 * @details
 * HUM Ch 11.2.18 p 456 specifies the PRCR (Protect Register) write
 * sequence for the SYSC LPM group: write 16-bit value ``0xA502``
 * (key ``0xA5`` in bits 15:8 plus PRC1 = 1 in bit 1) to unlock, do
 * the protected writes, then write ``0xA500`` to relock.
 *
 * Cold-reset values per HUM:
 *  - SBYCR   = 0x40 (OPE = 1)
 *  - LPSCR   = 0x00 (LPMD = 0 -- Sleep mode, not standby)
 *  - DPSBYCR = 0x14 (DCSSMODE = 01b, all else 0)
 *
 * @pre Callable from Reset_Handler before ``.data`` / ``.bss`` init.
 * @pre Interrupts are masked (vendor reset state).
 * @post LPSCR = 0; next WFI enters plain Sleep, not Software Standby.
 * @post SBYCR / DPSBYCR restored to documented reset values.
 *
 * @note No-op on host (``RA8_OFF_TARGET``).
 *
 * @see HUM Ch 11.2.18 "SBYCR / PRCR" p 456.
 * @see HUM Ch 11.2.20 "LPSCR" p 457.
 * @see HUM Ch 11.2.21 "DPSBYCR" p 458.
 *
 * @since 0.1.0
 */
[[gnu::always_inline]] static inline void ra8_lpm_safe_boot(void)
{
#ifndef RA8_OFF_TARGET
  /* SYSC.PRCR (16-bit, write-protect key) */
  volatile uint16_t* const prcr = (volatile uint16_t*)k_ra8_lpm_safe_boot_prcr_addr;
  /* SBYCR (8-bit) -- Standby Control Register */
  volatile uint8_t* const sbycr = (volatile uint8_t*)k_ra8_lpm_safe_boot_sbycr_addr;
  /* LPSCR (8-bit) -- Low Power State Control Register */
  volatile uint8_t* const lpscr = (volatile uint8_t*)k_ra8_lpm_safe_boot_lpscr_addr;
  /* DPSBYCR (8-bit) -- Deep Software Standby Ctrl */
  volatile uint8_t* const dpsbycr = (volatile uint8_t*)k_ra8_lpm_safe_boot_dpsbycr_addr;

  *prcr    = (uint16_t)k_ra8_lpm_safe_boot_prcr_unlock;
  *lpscr   = (uint8_t)k_ra8_lpm_safe_boot_lpscr_sleep;
  *sbycr   = (uint8_t)k_ra8_lpm_safe_boot_sbycr_reset;
  *dpsbycr = (uint8_t)k_ra8_lpm_safe_boot_dpsbycr_reset;
  *prcr    = (uint16_t)k_ra8_lpm_safe_boot_prcr_relock;
#endif /* RA8_OFF_TARGET */
}

#ifdef __cplusplus
}
#endif

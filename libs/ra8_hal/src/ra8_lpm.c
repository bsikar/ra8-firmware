/**
 * @file ra8_lpm.c
 * @brief Low Power Mode (LPM) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implements the public API declared in ``ra8_lpm.h``. Every register
 * access carries a HUM Ch 11 (LPM) or HUM Ch 14 (ICU for WUPEN0/1)
 * citation. The driver covers the full set of registers described in
 * HUM Ch 11 (pages 429..497):
 *
 *  - SBYCR / DPSBYCR / LPSCR / SSCR1 (lifecycle + sleep entry).
 *  - DPSIER0..3 / DPSIFR0..3 / DPSIEGR0..2 (deep-standby cancel).
 *  - PDRAMSCR0 / PDRAMSCR1 (RAM retention).
 *  - PLL1LDOCR / PLL2LDOCR / HOCOLDOCR (LDO standby SKEEP).
 *  - OPCCR (operating power control read + transition wait).
 *  - MOSCCR / HOCOCR / MOCOCR / LOCOCR / SOSCCR (clock shutdown
 *    matrix per HUM Ch 11.3 mode tables).
 *  - PRCR (write-protect unlock/relock).
 *  - WUPEN0 / WUPEN1 (per-source wake-up arming).
 *
 * The driver does NOT manipulate SYSTEM.PRCR by side-effect during
 * sleep entry; instead it exposes ``ra8_lpm_prcr_unlock`` and
 * ``ra8_lpm_prcr_relock`` so the caller can scope the unlock to the
 * exact critical section. The legacy contract (caller has unlocked
 * PRC1 before the protected writes) is preserved.
 *
 * On host builds the WFI routes through the ``ra8_hw_intrinsics`` seam,
 * whose host stub returns at once, so unit tests can drive
 * ``ra8_lpm_enter_sleep`` and observe the LPSCR write through the fake mmap
 * without the test binary actually halting. The SCR.SLEEPDEEP read-modify-write runs
 * unchanged on every build: the fake mmap backs the Cortex-M System
 * Control Space window (core region at 0xE0000000), so host tests
 * stage and assert the real toggle sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_lpm.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"
#include "ra8_lpm_regs.h"
#include "ra8_register_protection.h"

/**
 * @var s_tag
 * @brief Component tag used in every log line emitted by this driver.
 */
static const char* s_tag = "LPM";

/** @brief Low-byte mask for the PRCR protect register. */
typedef enum : uint16_t {
  k_lpm_byte_mask = 0xFFU, /**< Lpm byte mask. */
} lpm_byte_mask_t;

/**
 * @enum ra8_lpm_status_shift_t
 * @brief Bit shifts used to pack SBYCR / DPSBYCR / LPSCR / SSCR1.
 */
typedef enum : uint8_t {
  k_ra8_lpm_status_shift_sbycr   = 0U,  /**< SBYCR occupies byte 0.   */
  k_ra8_lpm_status_shift_dpsbycr = 8U,  /**< DPSBYCR occupies byte 1. */
  k_ra8_lpm_status_shift_lpscr   = 16U, /**< LPSCR occupies byte 2.   */
  k_ra8_lpm_status_shift_sscr1   = 24U, /**< SSCR1 occupies byte 3.   */
} ra8_lpm_status_shift_t;

/**
 * @enum ra8_lpm_cause_shift_t
 * @brief Bit shifts used to pack WUPEN0 / WUPEN1 into a 64-bit cause word.
 */
typedef enum : uint8_t {
  k_ra8_lpm_cause_shift_wupen0 = 0U,  /**< WUPEN0 occupies the low 32 bits.  */
  k_ra8_lpm_cause_shift_wupen1 = 32U, /**< WUPEN1 occupies the high 32 bits. */
} ra8_lpm_cause_shift_t;

/**
 * @enum ra8_lpm_dpsi_count_t
 * @brief Buffer counts for ``ra8_lpm_get_dpsi_state``.
 */
typedef enum : uint8_t {
  k_ra8_lpm_dpsier_count = 4U, /**< DPSIER0..3 count.  */
  k_ra8_lpm_dpsifr_count = 4U, /**< DPSIFR0..3 count.  */
  k_ra8_lpm_dpsieg_count = 3U, /**< DPSIEGR0..2 count. */
} ra8_lpm_dpsi_count_t;

/**
 * @brief Issue a Data Synchronization Barrier followed by WFI.
 *
 * @details
 * A DSB retires every prior register write before the core sleeps, per the
 * HUM Ch 11 "Low Power Mode" entry sequence, then WFI parks the core. Both
 * primitives route through the ``ra8_hw_intrinsics`` seam, so the host build
 * substitutes no-ops (defined in ``tests/mocks/ra8_host_asm_stub.c``) and a
 * unit test can drive ``ra8_lpm_enter_sleep`` without blocking.
 *
 * @pre Module state is consistent.
 * @pre Sleep-mode registers have been written.
 * @post The prior register writes have retired before the core sleeps.
 * @post Control resumes on a wake event (target) or at once (host).
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_wait_for_interrupt(void)
{
  ra8_hw_dsb();
  ra8_hw_wfi();
}

/**
 * @brief Toggle SCB.SCR.SLEEPDEEP for non-Sleep mode entry.
 *
 * @details
 * Reads-modify-writes SCR so sibling SCR bits (e.g. SLEEPONEXIT,
 * SEVONPEND) are preserved. SCR is an Arm core register (Armv8-M SCS,
 * not in the HUM); the same sequence runs on every build -- the host
 * unit-test fake mmap backs the SCS window with RAM, so tests stage
 * and assert the toggle directly.
 *
 * @param[in] enable true -- set SLEEPDEEP; false -- clear.
 *
 * @pre The SCS window is accessible (always true on target and in the
 *      host test binary via ``ra8_fake_mmap``).
 * @pre Interrupt-free context is NOT required; the RMW is only racy
 *      against other SCR writers, which the LPM contract forbids.
 * @post SCR.SLEEPDEEP equals ``enable``.
 * @post All other SCR bits are unchanged.
 * @note Not thread-safe against concurrent SCR writers.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_set_sleepdeep(bool enable)
{
  /* Arm Cortex-M85 SCB->SCR (Armv8-M System Control Space; no HUM
   * chapter -- architectural register). */
  volatile uint32_t* scr = ra8_lpm_scb_scr();
  if (enable) {
    *scr |= (uint32_t)k_ra8_lpm_scb_scr_sleepdeep;
  } else {
    *scr &= ~(uint32_t)k_ra8_lpm_scb_scr_sleepdeep;
  }
}

/**
 * @brief Validate ``mode`` against the LPSCR.LPMD encoding table.
 *
 * @details See implementation.
 * @param[in] mode See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_mode(ra8_sleep_mode_t mode)
{
  ra8_err_t result = k_ra8_err_invalid_arg;
  switch (mode) {
    case k_ra8_sleep_mode_sleep:
    case k_ra8_sleep_mode_deep_sleep:
    case k_ra8_sleep_mode_software_std:
    case k_ra8_sleep_mode_deep_standby_1:
    case k_ra8_sleep_mode_deep_standby_2:
    case k_ra8_sleep_mode_deep_standby_3:
      result = k_ra8_ok;
      break;
    default:
      result = k_ra8_err_invalid_arg;
      break;
  }
  return result;
}

/**
 * @brief Resolve ``ra8_lpm_clock_t`` to its OCR offset.
 *
 * @details
 * Returns 0 on invalid input -- callers must validate first.
 *
 * @param[in] clock See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_lpm_off_t internal_clock_offset(ra8_lpm_clock_t clock)
{
  ra8_lpm_off_t off = k_ra8_lpm_mococr_off;
  switch (clock) {
    case k_ra8_lpm_clock_moco:
      /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
      off = k_ra8_lpm_mococr_off;
      break;
    case k_ra8_lpm_clock_hoco:
      /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
      off = k_ra8_lpm_hococr_off;
      break;
    case k_ra8_lpm_clock_loco:
      /* HUM Ch 11.2.21 "DPSBYCR : Deep Software Standby Control", p 458 */
      off = k_ra8_lpm_lococr_off;
      break;
    case k_ra8_lpm_clock_main:
      /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
      off = k_ra8_lpm_mosccr_off;
      break;
    case k_ra8_lpm_clock_sub:
      /* HUM Ch 11.2.21 "DPSBYCR : Deep Software Standby Control", p 458 */
      off = k_ra8_lpm_sosccr_off;
      break;
    case k_ra8_lpm_clock_count:
    default:
      off = k_ra8_lpm_mococr_off;
      break;
  }
  return off;
}

/**
 * @brief Resolve a DPSIER index to its register offset.
 *
 * @details See implementation.
 * @param[in] idx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_lpm_off_t internal_dpsier_offset(ra8_lpm_dpsier_idx_t idx)
{
  ra8_lpm_off_t off = k_ra8_lpm_dpsier3_off;
  switch (idx) {
    case k_ra8_lpm_dpsier_idx_0:
      /* HUM Ch 11.2.22 "DPSIER0", p 459 */
      off = k_ra8_lpm_dpsier0_off;
      break;
    case k_ra8_lpm_dpsier_idx_1:
      /* HUM Ch 11.2.23 "DPSIER1", p 459 */
      off = k_ra8_lpm_dpsier1_off;
      break;
    case k_ra8_lpm_dpsier_idx_2:
      /* HUM Ch 11.2.24 "DPSIER2", p 460 */
      off = k_ra8_lpm_dpsier2_off;
      break;
    case k_ra8_lpm_dpsier_idx_3:
    case k_ra8_lpm_dpsier_idx_count:
    default:
      /* HUM Ch 11.2.25 "DPSIER3", p 461 */
      off = k_ra8_lpm_dpsier3_off;
      break;
  }
  return off;
}

/**
 * @brief Resolve a DPSIFR index to its register offset.
 *
 * @details See implementation.
 * @param[in] idx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_lpm_off_t internal_dpsifr_offset(ra8_lpm_dpsier_idx_t idx)
{
  ra8_lpm_off_t off = k_ra8_lpm_dpsifr3_off;
  switch (idx) {
    case k_ra8_lpm_dpsier_idx_0:
      /* HUM Ch 11.2.22 "DPSIER0", p 459 */
      off = k_ra8_lpm_dpsifr0_off;
      break;
    case k_ra8_lpm_dpsier_idx_1:
      /* HUM Ch 11.2.23 "DPSIER1", p 459 */
      off = k_ra8_lpm_dpsifr1_off;
      break;
    case k_ra8_lpm_dpsier_idx_2:
      /* HUM Ch 11.2.24 "DPSIER2", p 460 */
      off = k_ra8_lpm_dpsifr2_off;
      break;
    case k_ra8_lpm_dpsier_idx_3:
    case k_ra8_lpm_dpsier_idx_count:
    default:
      /* HUM Ch 11.2.25 "DPSIER3", p 461 */
      off = k_ra8_lpm_dpsifr3_off;
      break;
  }
  return off;
}

/**
 * @brief Resolve a DPSIEGR index to its register offset.
 *
 * @details
 * DPSIEGR3 does not exist on RA8D2 (HUM Ch 11.2 stops at DPSIEGR2);
 * passing index 3 returns DPSIEGR2 so the caller observes a
 * harmless re-read.
 *
 * @param[in] idx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_lpm_off_t internal_dpsiegr_offset(ra8_lpm_dpsier_idx_t idx)
{
  ra8_lpm_off_t off = k_ra8_lpm_dpsiegr2_off;
  switch (idx) {
    case k_ra8_lpm_dpsier_idx_0:
      /* HUM Ch 11.2.22 "DPSIER0", p 459 */
      off = k_ra8_lpm_dpsiegr0_off;
      break;
    case k_ra8_lpm_dpsier_idx_1:
      /* HUM Ch 11.2.23 "DPSIER1", p 459 */
      off = k_ra8_lpm_dpsiegr1_off;
      break;
    case k_ra8_lpm_dpsier_idx_2:
    case k_ra8_lpm_dpsier_idx_3:
    case k_ra8_lpm_dpsier_idx_count:
    default:
      /* HUM Ch 11.2.24 "DPSIER2", p 460 */
      off = k_ra8_lpm_dpsiegr2_off;
      break;
  }
  return off;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_init(const ra8_lpm_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* SBYCR: only OPE is software-writable (HUM Ch 11.2.18 p 456). */
  uint8_t sbycr = 0U;
  if (cfg->opa_bus_keep) {
    sbycr |= k_ra8_lpm_sbycr_ope_mask;
  }
  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off) = sbycr;

  /* DPSBYCR: IOKEEP + DCSSMODE (HUM Ch 11.2.21 p 458). */
  uint8_t dpsbycr = (uint8_t)((uint8_t)cfg->dcdc_softstart << k_ra8_lpm_dpsbycr_dcssmode_shift);
  if (cfg->io_port_keep) {
    dpsbycr |= k_ra8_lpm_dpsbycr_iokeep_mask;
  }
  /* HUM Ch 11.2.21 "DPSBYCR : Deep Software Standby Control Register", p 458 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off) = dpsbycr;

  /* SSCR1: SS2FR + SS2LP (HUM Ch 11.2.19 p 456). */
  uint8_t sscr1 = (uint8_t)((uint8_t)cfg->sscr_low_power << k_ra8_lpm_sscr1_ss2lp_shift);
  if (cfg->sscr_fast_return) {
    sscr1 |= k_ra8_lpm_sscr1_ss2fr_mask;
  }
  /* HUM Ch 11.2.19 "SSCR1 : Software Standby Control Register 1", p 456 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off) = sscr1;

  /* LPSCR: clear LPMD so the next plain WFI is just a CPU sleep. */
  /* HUM Ch 11.2.20 "LPSCR : Low Power State Control Register", p 457 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off) = 0U;

  ra8_log_info(s_tag, "lpm_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_deinit(void)
{
  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off) = k_ra8_lpm_sbycr_reset_val;
  /* HUM Ch 11.2.21 "DPSBYCR : Deep Software Standby Control Register", p 458 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off) = k_ra8_lpm_dpsbycr_reset_val;
  /* HUM Ch 11.2.19 "SSCR1 : Software Standby Control Register 1", p 456 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off) = 0U;
  /* HUM Ch 11.2.20 "LPSCR : Low Power State Control Register", p 457 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off) = 0U;

  /* Clear wake-up enables so a stray pending interrupt does not race. */
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  *ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off) = 0U;
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  *ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off) = 0U;

  /* Clear all DPSIER0..3 so a stale per-factor enable does not survive. */
  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier0_off) = 0U;
  /* HUM Ch 11.2.23 "DPSIER1 : Deep Standby Interrupt Enable 1", p 459 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier1_off) = 0U;
  /* HUM Ch 11.2.24 "DPSIER2 : Deep Standby Interrupt Enable 2", p 460 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier2_off) = 0U;
  /* HUM Ch 11.2.25 "DPSIER3 : Deep Standby Interrupt Enable 3", p 461 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off) = 0U;

  ra8_log_info(s_tag, "lpm_deinit");
  return k_ra8_ok;
}

/* =============================================================================
 * PRCR unlock / relock
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_prcr_unlock(void)
{
  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 -- PRCR is
   * the cross-cut write-protect register that gates SBYCR/DPSBYCR/etc. */
  volatile uint16_t* prcr = ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off);
  const uint16_t     cur  = *prcr;
  const uint16_t     mask = (uint16_t)((cur & k_lpm_byte_mask) | k_ra8_lpm_prcr_prc1_msk);
  *prcr                   = (uint16_t)(k_ra8_lpm_prcr_key | mask);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_prcr_relock(void)
{
  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
  volatile uint16_t* prcr = ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off);
  const uint16_t     cur  = *prcr;
  const uint16_t mask = (uint16_t)((cur & k_lpm_byte_mask) & (uint16_t)~k_ra8_lpm_prcr_prc1_msk);
  *prcr               = (uint16_t)(k_ra8_lpm_prcr_key | mask);
  return k_ra8_ok;
}

/* =============================================================================
 * Wake-up source programming
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_set_wakeup_sources(uint32_t wupen0, uint32_t wupen1)
{
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  *ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off) = wupen0;
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  *ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off) = wupen1;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_arm_wupen0_bits(uint32_t bits)
{
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  volatile uint32_t* p = ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off);
  *p                   = *p | bits;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_clear_wupen0_bits(uint32_t bits)
{
  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  volatile uint32_t* p = ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off);
  *p                   = (uint32_t)(*p & ~bits);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_arm_wupen1_bits(uint32_t bits)
{
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  volatile uint32_t* p = ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off);
  *p                   = *p | bits;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_clear_wupen1_bits(uint32_t bits)
{
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  volatile uint32_t* p = ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off);
  *p                   = (uint32_t)(*p & ~bits);
  return k_ra8_ok;
}

/* =============================================================================
 * Deep-standby cancel programming
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_arm_dpsier(ra8_lpm_dpsier_idx_t idx, uint8_t value)
{
  if ((uint8_t)idx >= k_ra8_lpm_dpsier_idx_count) {
    ra8_log_error(s_tag, "arm_dpsier: idx out of range");
    return k_ra8_err_invalid_arg;
  }
  const ra8_lpm_off_t en_off = internal_dpsier_offset(idx);
  const ra8_lpm_off_t fr_off = internal_dpsifr_offset(idx);

  /* HUM Ch 11.2.22 "DPSIER0", p 459 (and siblings 11.2.23..25): the enable
   * register is byte-wide and S-TYPE-4 protected. */
  *ra8_lpm_sysc_reg8(en_off) = value;

  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459
   * mandates a dummy read of DPSIFR before clearing it. */
  (void)*ra8_lpm_sysc_reg8(fr_off);
  *ra8_lpm_sysc_reg8(fr_off) = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_clear_dpsifr(ra8_lpm_dpsier_idx_t idx)
{
  if ((uint8_t)idx >= k_ra8_lpm_dpsier_idx_count) {
    ra8_log_error(s_tag, "clear_dpsifr: idx out of range");
    return k_ra8_err_invalid_arg;
  }
  const ra8_lpm_off_t fr_off = internal_dpsifr_offset(idx);
  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  (void)*ra8_lpm_sysc_reg8(fr_off);
  *ra8_lpm_sysc_reg8(fr_off) = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_set_dpsiegr(ra8_lpm_dpsier_idx_t idx, uint8_t value)
{
  if ((uint8_t)idx >= k_ra8_lpm_dpsier_idx_count) {
    ra8_log_error(s_tag, "set_dpsiegr: idx out of range");
    return k_ra8_err_invalid_arg;
  }
  const ra8_lpm_off_t eg_off = internal_dpsiegr_offset(idx);
  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  *ra8_lpm_sysc_reg8(eg_off) = value;
  return k_ra8_ok;
}

/* =============================================================================
 * Snooze controller (Standby + Snooze pattern on RA8D2)
 * =============================================================================
 */

[[nodiscard]] ra8_err_t
ra8_lpm_snooze_set_request_sources(bool ulpt0_underflow, bool ulpt1_underflow, bool acmphs0)
{
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  volatile uint32_t* w1 = ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off);
  uint32_t           v  = *w1;
  if (ulpt0_underflow) {
    v |= k_ra8_lpm_wupen1_ulpt0u;
  }
  if (ulpt1_underflow) {
    v |= k_ra8_lpm_wupen1_ulpt1u;
  }
  *w1 = v;

  if (acmphs0) {
    /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
    volatile uint32_t* w0 = ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off);
    *w0                   = *w0 | k_ra8_lpm_wupen0_acmphs0;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_lpm_snooze_set_end_sources(bool ulpt0, bool ulpt1, bool usbfs, bool usbhs)
{
  /* HUM Ch 11.2.25 "DPSIER3 : Deep Standby Interrupt Enable 3", p 461 */
  volatile uint8_t* en = ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off);
  uint8_t           v  = *en;
  if (ulpt0) {
    v |= k_ra8_lpm_dpsier3_dulpt0ie_mask;
  }
  if (ulpt1) {
    v |= k_ra8_lpm_dpsier3_dulpt1ie_mask;
  }
  if (usbfs) {
    v |= k_ra8_lpm_dpsier3_dusbfsie_mask;
  }
  if (usbhs) {
    v |= k_ra8_lpm_dpsier3_dusbhsie_mask;
  }
  *en = v;

  /* HUM Ch 11.2.25 "DPSIER3 : Deep Standby Interrupt Enable 3", p 461
   * mandates a dummy read of DPSIFR3 before writing 0 to clear it. */
  (void)*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off);
  *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off) = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * RAM and LDO retention
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_set_ram_retention(const ra8_lpm_ram_retention_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "ram retention cfg null");

  /* HUM Ch 11.2.16 "PDRAMSCR0 : SRAM Power Domain Standby Control 0", p 453 */
  *ra8_lpm_sysc_reg16(k_ra8_lpm_pdramscr0_off) =
    (uint16_t)(cfg->pdramscr0_bits & k_ra8_lpm_pdramscr0_writable);

  uint8_t pdramscr1 = 0U;
  if (cfg->cpu0_tcm_keep) {
    pdramscr1 |= k_ra8_lpm_pdramscr1_rkeep0_mask;
  }
  if (cfg->cpu1_tcm_keep) {
    pdramscr1 |= k_ra8_lpm_pdramscr1_rkeep1_mask;
  }
  /* HUM Ch 11.2.17 "PDRAMSCR1 : SRAM Power Domain Standby Control 1", p 455 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_pdramscr1_off) = pdramscr1;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_set_ldo_standby(const ra8_lpm_ldo_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "ldo cfg null");

  /* Verify OPCCR.OPCM == 0 (High-speed mode) per HUM Ch 11.2.1 p 436 -- *LDOCR
   * writes are only permitted in HSM. */
  /* HUM Ch 11.2.11 "OPCCR : Operating Power Control Register", p 451 */
  const uint8_t opccr = *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off);
  if (((uint8_t)(opccr & k_ra8_lpm_opccr_opcm_mask)) != 0U) {
    ra8_log_error(s_tag, "set_ldo: OPCM!=0");
    return k_ra8_err_invalid_state;
  }

  /* HUM Ch 11.2.1 "LPMSAR : Low Power Mode Security Attribution Register", p 436 */
  volatile uint8_t* pll1 = ra8_lpm_sysc_reg8(k_ra8_lpm_pll1ldocr_off);
  *pll1                  = (uint8_t)((*pll1 & (uint8_t)~k_ra8_lpm_ldocr_skeep_mask) |
                                     (uint8_t)((uint8_t)cfg->pll1 << k_ra8_lpm_ldocr_skeep_shift));

  /* HUM Ch 11.2.1 "LPMSAR : Low Power Mode Security Attribution Register", p 436 */
  volatile uint8_t* pll2 = ra8_lpm_sysc_reg8(k_ra8_lpm_pll2ldocr_off);
  *pll2                  = (uint8_t)((*pll2 & (uint8_t)~k_ra8_lpm_ldocr_skeep_mask) |
                                     (uint8_t)((uint8_t)cfg->pll2 << k_ra8_lpm_ldocr_skeep_shift));

  /* HUM Ch 11.2.1 "LPMSAR : Low Power Mode Security Attribution Register", p 436 */
  volatile uint8_t* hoco = ra8_lpm_sysc_reg8(k_ra8_lpm_hocoldocr_off);
  *hoco                  = (uint8_t)((*hoco & (uint8_t)~k_ra8_lpm_ldocr_skeep_mask) |
                                     (uint8_t)((uint8_t)cfg->hoco << k_ra8_lpm_ldocr_skeep_shift));
  return k_ra8_ok;
}

/* =============================================================================
 * Clock-shutdown matrix
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_set_clock_stop(ra8_lpm_clock_t clock, bool stop)
{
  if ((uint8_t)clock >= k_ra8_lpm_clock_count) {
    ra8_log_error(s_tag, "set_clock_stop: bad clock");
    return k_ra8_err_invalid_arg;
  }
  volatile uint8_t* p = ra8_lpm_sysc_reg8(internal_clock_offset(clock));

  /* The oscillator control registers (MOSCCR/HOCOCR/MOCOCR/LOCOCR/SOSCCR) each
   * carry their STOP bit at bit 0 and belong to the clock-generation circuit,
   * which is PRC0-protected (HUM Ch 13.1 Table 13.1 p 520). A store issued with
   * PRC0 locked is silently discarded, so open a CGC unlock window around the
   * read-modify-write rather than trusting an undocumented caller contract. */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.18 "MOCOCR : MOCO Control Register", p 341 -- representative of
     * the OCR family; internal_clock_offset() selects the register this clock
     * owns and each OCR places its STOP bit at bit 0. */
    if (stop) {
      *p = (uint8_t)(*p | k_ra8_lpm_clock_stop_mask);
    } else {
      *p = (uint8_t)(*p & (uint8_t)~k_ra8_lpm_clock_stop_mask);
    }
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_get_clock_stop(ra8_lpm_clock_t clock, bool* stop)
{
  RA8_CHECK_NULL_PTR(stop, s_tag, "stop must not be nullptr");
  if ((uint8_t)clock >= k_ra8_lpm_clock_count) {
    ra8_log_error(s_tag, "get_clock_stop: bad clock");
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
  const uint8_t v = *ra8_lpm_sysc_reg8(internal_clock_offset(clock));
  *stop           = ((uint8_t)(v & k_ra8_lpm_clock_stop_mask) != 0U);
  return k_ra8_ok;
}

/* =============================================================================
 * OPCCR
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_get_opccr(uint8_t* opccr)
{
  RA8_CHECK_NULL_PTR(opccr, s_tag, "opccr must not be nullptr");
  /* HUM Ch 11.2.11 "OPCCR : Operating Power Control Register", p 451 */
  *opccr = *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_wait_for_opccr(uint32_t poll_limit)
{
  if (poll_limit == 0U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 11.2.11 "OPCCR : Operating Power Control Register", p 451 */
    const uint8_t v = *ra8_lpm_sysc_reg8(k_ra8_lpm_opccr_off);
    if (((uint8_t)(v & k_ra8_lpm_opccr_opcmtsf_msk)) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  ra8_log_error(s_tag, "wait_for_opccr timeout");
  return k_ra8_err_hw_timeout;
}

/* =============================================================================
 * Sleep entry
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_enter_sleep(ra8_sleep_mode_t mode)
{
  const ra8_err_t mode_err = internal_validate_mode(mode);
  RA8_RETURN_ON_ERROR(mode_err, s_tag, "lpm_enter_sleep: bad mode"); /* GCOVR_EXCL_BR_LINE */

  /* For Sleep / Deep-Sleep, LPMD stays at 0 (System Active); only WFI +
   * SCR.SLEEPDEEP toggle differs. For Software Standby and Deep Standby,
   * LPMD encodes the destination per HUM Ch 11.2.20 p 457. */
  uint8_t lpscr_value = 0U;
  bool    sleepdeep   = false;
  switch (mode) {
    case k_ra8_sleep_mode_sleep:
      lpscr_value = 0U;
      sleepdeep   = false;
      break;
    case k_ra8_sleep_mode_deep_sleep:
      lpscr_value = 0U;
      sleepdeep   = true;
      break;
    case k_ra8_sleep_mode_software_std:
    case k_ra8_sleep_mode_deep_standby_1:
    case k_ra8_sleep_mode_deep_standby_2:
    case k_ra8_sleep_mode_deep_standby_3:
    default:
      lpscr_value = (uint8_t)((uint8_t)mode & k_ra8_lpm_lpscr_lpmd_mask);
      sleepdeep   = true;
      break;
  }

  /* HUM Ch 11.2.20 "LPSCR : Low Power State Control Register", p 457 */
  *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off) = lpscr_value;

  /* Toggle SCB.SCR.SLEEPDEEP per HUM Ch 11.3 entry sequence (the bit
   * is owned by the Cortex-M85 SCB, not by SYSC; on host this is a
   * no-op). */
  internal_set_sleepdeep(sleepdeep);

  ra8_log_info_val(s_tag, "lpm_enter_sleep mode", (uint32_t)mode);
  internal_wait_for_interrupt();

  /* On wake (or immediately on host), restore SLEEPDEEP for the next
   * plain Sleep entry, and clear LPSCR back to Sleep mode so the next
   * WFI does not re-enter Software Standby.  Without this, a debugger-
   * issued SYSRESETREQ leaves LPSCR set (SYSC LPM block is in a
   * separate reset domain), and the first WFI in J-Link's RAMCode
   * helper re-enters standby -- bricking the chip until a true PORST. */
  internal_set_sleepdeep(false);
  *ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off) = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_enter_deep_standby(void)
{
  return ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_1);
}

/* =============================================================================
 * Status / diagnostics
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_lpm_get_status(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 11.2.18 "SBYCR : Standby Control Register", p 456 */
  const uint32_t sbycr = (uint32_t)*ra8_lpm_sysc_reg8(k_ra8_lpm_sbycr_off);
  /* HUM Ch 11.2.21 "DPSBYCR : Deep Software Standby Control Register", p 458 */
  const uint32_t dpsbycr = (uint32_t)*ra8_lpm_sysc_reg8(k_ra8_lpm_dpsbycr_off);
  /* HUM Ch 11.2.20 "LPSCR : Low Power State Control Register", p 457 */
  const uint32_t lpscr = (uint32_t)*ra8_lpm_sysc_reg8(k_ra8_lpm_lpscr_off);
  /* HUM Ch 11.2.19 "SSCR1 : Software Standby Control Register 1", p 456 */
  const uint32_t sscr1 = (uint32_t)*ra8_lpm_sysc_reg8(k_ra8_lpm_sscr1_off);

  *out = (sbycr << k_ra8_lpm_status_shift_sbycr) | (dpsbycr << k_ra8_lpm_status_shift_dpsbycr) |
         (lpscr << k_ra8_lpm_status_shift_lpscr) | (sscr1 << k_ra8_lpm_status_shift_sscr1);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_lpm_get_exit_cause(uint64_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 14.2.19 "WUPEN0 : Wake Up Interrupt Enable Register 0", p 550 */
  const uint64_t w0 = (uint64_t)*ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off);
  /* HUM Ch 14.2.20 "WUPEN1 : Wake Up Interrupt Enable Register 1", p 552 */
  const uint64_t w1 = (uint64_t)*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off);

  *out = (w0 << k_ra8_lpm_cause_shift_wupen0) | (w1 << k_ra8_lpm_cause_shift_wupen1);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_lpm_get_dpsi_state(uint8_t enables[4], uint8_t flags[4], uint8_t edges[3])
{
  RA8_CHECK_NULL_PTR(enables, s_tag, "enables nullptr");
  RA8_CHECK_NULL_PTR(flags, s_tag, "flags nullptr");
  RA8_CHECK_NULL_PTR(edges, s_tag, "edges nullptr");

  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  enables[0] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier0_off);
  /* HUM Ch 11.2.23 "DPSIER1 : Deep Standby Interrupt Enable 1", p 459 */
  enables[1] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier1_off);
  /* HUM Ch 11.2.24 "DPSIER2 : Deep Standby Interrupt Enable 2", p 460 */
  enables[2] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier2_off);
  /* HUM Ch 11.2.25 "DPSIER3 : Deep Standby Interrupt Enable 3", p 461 */
  enables[3] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsier3_off);

  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  flags[0] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr0_off);
  /* HUM Ch 11.2.23 "DPSIER1 : Deep Standby Interrupt Enable 1", p 459 */
  flags[1] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr1_off);
  /* HUM Ch 11.2.24 "DPSIER2 : Deep Standby Interrupt Enable 2", p 460 */
  flags[2] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr2_off);
  /* HUM Ch 11.2.25 "DPSIER3 : Deep Standby Interrupt Enable 3", p 461 */
  flags[3] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsifr3_off);

  /* HUM Ch 11.2.22 "DPSIER0 : Deep Standby Interrupt Enable 0", p 459 */
  edges[0] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr0_off);
  /* HUM Ch 11.2.23 "DPSIER1 : Deep Standby Interrupt Enable 1", p 459 */
  edges[1] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr1_off);
  /* HUM Ch 11.2.24 "DPSIER2 : Deep Standby Interrupt Enable 2", p 460 */
  edges[2] = *ra8_lpm_sysc_reg8(k_ra8_lpm_dpsiegr2_off);
  return k_ra8_ok;
}

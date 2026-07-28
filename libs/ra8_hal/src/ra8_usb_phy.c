/**
 * @file ra8_usb_phy.c
 * @brief USBHS embedded-PHY bring-up + shared FS/HS module bring-up
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * USBHS (IP1) embedded-UTMI-PHY bring-up sequence (HUM Ch 37.3.3,
 * Figure 37.2 p 2121) plus the shared post-SYSCFG common-register
 * programming and the USBFS (IP0) module bring-up. Split out of
 * ``ra8_usb.c`` so every translation unit stays under the 1000-line cap;
 * the three externally-called entry points
 * (``internal_usbhs_phy_bringup`` / ``internal_usb_init_common`` /
 * ``internal_usbfs_module_bringup``) are declared in
 * ``ra8_usb_internal.h``. Modelled on FSP ``r_usb_preg_access.c``; no
 * FSP source ships in this tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * USBHS PHY + shared module bring-up
 * =============================================================================
 */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @enum ra8_usbhs_init_local_t
 * @brief Local sentinels used by the USBHS PHY bring-up sequence.
 *
 * @details
 * - ``delay_1us_iters`` -- NOP-loop iteration count for ~1 us at 1 GHz.
 * - ``pll_lock_poll_limit`` -- bounded spin count for PLLSTA.PLLLOCK
 *   wait (~200k iters; covers worst-case PHY analog ramp).
 * - ``scke_poll_limit`` -- same bound for the FS-path SCKE settle wait.
 * - ``probe_hi_shift`` -- shift to pack SYSCFG into the high half of
 *   ``s_usbhs_init_probe``.
 * - ``probe_lo_mask`` -- mask of the low half of the probe word.
 */
typedef enum : uint32_t {
  k_ra8_usbhs_delay_1us_iters = 2000U, /**< RA8 usbhs delay 1us iters. */
  /* PHY analog PLL can take multiple ms to lock at cold start; FSP
   * uses an unbounded while loop. We pick 5 million NOP-spins
   * (~5 ms ceiling at 1 GHz Cortex-M85) which is well above the
   * worst-case observed lock time but still bounded for NASA Rule 2. */
  k_ra8_usbhs_pll_lock_poll_limit = 5000000U, /**< RA8 usbhs pll lock poll limit. */
  /* Per-CLKSEL bisect attempt: ~50 ms ceiling at 1 GHz so the 4-attempt
   * sweep stays under the 250 ms total wall-time budget mandated by the
   * USB-HS bring-up debug spec. */
  k_ra8_usbhs_pll_lock_attempt_limit = 50000000U / 100U, /**< RA8 usbhs pll lock attempt limit. */
  k_ra8_usbhs_scke_poll_limit        = 200000U,          /**< RA8 usbhs scke poll limit.        */
  k_ra8_usbhs_probe_hi_shift         = 16U,              /**< RA8 usbhs probe hi shift.         */
  k_ra8_usbhs_probe_lo_mask          = 0x0000FFFFU,      /**< RA8 usbhs probe lo mask.          */
} ra8_usbhs_init_local_t;

/**
 * @enum ra8_usbhs_clksel_attempt_t
 * @brief Iteration order for the CLKSEL[1:0] bisect harness.
 *
 * @details HUM Ch 37.2.17 PHYSET CLKSEL[1:0] bit-field, p 2080:
 *   - 00b = 12 MHz (canonical UTMI reference; PHY divides USB60CLK
 *     by 5 internally to obtain 12 MHz from a 60 MHz USB60CLK)
 *   - 01b = 48 MHz
 *   - 10b = 20 MHz
 *   - 11b = 24 MHz (PHYSET reset value)
 *
 * USB60CKDIVCR is now /4 so USB60CLK = PLL2P/4 = 240/4 = 60.000 MHz,
 * matching the HUM Ch 37.3.3 named-rate requirement (p 2102: "A
 * 60-MHz clock must be supplied"). With a 60 MHz USB60CLK the
 * canonical CLKSEL is 12 MHz (the standard UTMI+ reference), so the
 * bisect tries CLKSEL=12 first and only falls through to 48 / 20 / 24
 * as a defensive sweep if a CGC regression delivers an unexpected
 * USB60CLK rate. Capture the winner (or 0xFF on full sweep failure)
 * into ::s_clksel_winner.
 */
typedef enum : uint8_t {
  k_ra8_usbhs_clksel_attempts_n = 4U,    /**< RA8 usbhs clksel attempts n. */
  k_ra8_usbhs_clksel_no_winner  = 0xFFU, /**< Sentinel: bisect exhausted.  */
} ra8_usbhs_clksel_attempt_t;

/**
 * @enum ra8_usbhs_phy_step_t
 * @brief Sentinel values for the per-step USBHS PHY bring-up probe.
 *
 * @details Each value marks a completed sub-step of
 * ::internal_usbhs_phy_bringup. A JLink read of ``s_phy_step_probe``
 * pinpoints the furthest-reached step on a stalled boot.
 */
typedef enum : uint8_t {
  k_ra8_usbhs_phy_step_hse_set      = 1U, /**< SYSCFG.HSE asserted.        */
  k_ra8_usbhs_phy_step_clksel_12    = 2U, /**< PHYSET CLKSEL forced to 12. */
  k_ra8_usbhs_phy_step_dirpd_clear  = 3U, /**< PHY analog powered.         */
  k_ra8_usbhs_phy_step_pll_released = 4U, /**< PHY PLLRESET cleared.       */
  k_ra8_usbhs_phy_step_usbe_set     = 5U, /**< SYSCFG DRPD=0 USBE=1.       */
  k_ra8_usbhs_phy_step_suspendm_set = 6U, /**< LPSTS SUSPENDM=1.           */
  k_ra8_usbhs_phy_step_pll_locked   = 7U, /**< PLLSTA.PLLLOCK observed.    */
} ra8_usbhs_phy_step_t;

/**
 * @var s_phy_step_probe
 * @brief Diagnostic step counter for the USBHS PHY bring-up sequence.
 *
 * @details Each step in ::internal_usbhs_phy_bringup writes a fixed
 * sentinel here so a JLink read pinpoints the last completed step:
 * - 1 = SYSCFG.HSE asserted (pre-PHY).
 * - 2 = PHYSET DIRPD|CLKSEL set, CLKSEL field forced to 12 MHz.
 * - 3 = PHYSET DIRPD released (PHY analog powered).
 * - 4 = PHYSET PLLRESET released.
 * - 5 = SYSCFG DRPD cleared, USBE set.
 * - 6 = LPSTS.SUSPENDM set.
 * - 7 = PLLSTA.PLLLOCK observed (PHY ready).
 *
 * @note Read-only from outside; written only by the PHY bring-up.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint8_t s_phy_step_probe = 0U;

/**
 * @var s_usbhs_init_probe
 * @brief Diagnostic capture of staged USBHS bring-up register values.
 *
 * @details Each phase of the HS PHY bring-up writes a distinct slot
 * here so a JLink read of this SRAM word reveals exactly which step
 * was reached. Layout (low half = PHYSET-after-power-up, high half =
 * SYSCFG-after-USBE-write). On a healthy boot the low half ends at
 * ``DIRPD=0, PLLRESET=0, CLKSEL=0`` and the high half ends at
 * ``USBE|HSE = 0x081``.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_init.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint32_t s_usbhs_init_probe = 0U;

/**
 * @var s_usbhs_pllsta_probe
 * @brief Diagnostic capture of PLLSTA at end of PHY bring-up.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_init.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint16_t s_usbhs_pllsta_probe = 0U;

/**
 * @var s_clksel_winner
 * @brief Diagnostic capture of the PHYSET.CLKSEL[1:0] codepoint that
 *        produced PLL lock during the bisect harness.
 *
 * @details Layout: low nibble is the masked PHYSET CLKSEL field
 * (0x00, 0x10, 0x20, or 0x30 corresponding to 12 / 48 / 20 / 24 MHz
 * per HUM Ch 37.2.17 PHYSET p 2080 Table). Sentinel 0xFF means the
 * full 4-codepoint sweep completed without observing PLLSTA.PLLLOCK.
 *
 * @note Read-only from outside; written only by ::internal_usbhs_phy_bringup.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint8_t s_clksel_winner = (uint8_t)k_ra8_usbhs_clksel_no_winner;

/**
 * @var s_clksel_attempt_pllsta
 * @brief Per-attempt PLLSTA capture array for the bisect harness.
 *
 * @details Index = attempt number (0..3). Each slot captures the
 * PLLSTA value observed at the end of that attempt's polling window
 * so a JLink read of all four words tells the operator which CLKSEL
 * codepoints saw any partial lock activity.
 *
 * @note Read-only from outside.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint16_t s_clksel_attempt_pllsta[4] = {0U, 0U, 0U, 0U};

/**
 * @brief Microsecond busy-wait helper for USB PHY bring-up.
 *
 * @details Cortex-M85 at 1 GHz: ~1000 cycles per microsecond. We use
 * a NOP loop calibrated to comfortably exceed 1 us so PHY power-up
 * settling completes. FSP `r_usb_preg_access.c` calls
 * `usb_cpu_delay_1us` -- this is the local equivalent.
 *
 * @pre Caller is in init context (busy-wait blocks the CPU).
 * @pre CPU clock is the one assumed by ``k_ra8_usbhs_delay_1us_iters``.
 * @post No state mutation; loop iteration count is bounded.
 * @post At least 1 us has elapsed.
 * @note Not thread-safe; calibrated for 1 GHz Cortex-M85.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_usb_delay_1us(void)
{
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_usbhs_delay_1us_iters;
       ++i) { /* GCOVR_EXCL_BR_LINE */
    __asm__ volatile("nop");
  }
}

/**
 * @brief Drive USBHS SYSCFG DRPD=0, USBE=1 after PLLRESET is released.
 *
 * @details Splits the SYSCFG mutation off from
 * ::internal_usbhs_phy_bringup so each helper stays under the NASA
 * Rule 4 / clang-tidy size budget. HSE is set separately at the start
 * of the bring-up (mirrors FSP `hw_usb_set_hse` which is called BEFORE
 * `hw_usb_pmodule_init` -- r_usb_basic.c). Captures the
 * SYSCFG-after-USBE value into the probe word's high half.
 *
 * @param[in] reg HS register block pointer.
 *
 * @pre PHYSET DIRPD/PLLRESET are released.
 * @pre SYSCFG.HSE is already 1.
 * @pre s_usbhs_init_probe low half holds PHYSET-after-power-up.
 * @post SYSCFG: DRPD=0, USBE=1, HSE=1.
 * @post s_usbhs_init_probe high half = SYSCFG.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_usbhs_enable_syscfg(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2060 */
  reg->SYSCFG = (uint16_t)(reg->SYSCFG & (uint16_t)~(uint16_t)(1U << k_ra8_syscfg_bit_drpd));
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra8_syscfg_bit_usbe));

  const uint32_t lo  = s_usbhs_init_probe & (uint32_t)k_ra8_usbhs_probe_lo_mask;
  const uint32_t hi  = (uint32_t)reg->SYSCFG << (uint32_t)k_ra8_usbhs_probe_hi_shift;
  s_usbhs_init_probe = lo | hi;
}

/**
 * @brief Bounded PHY PLL-lock poll for a single CLKSEL attempt.
 *
 * @details Polls PLLSTA.PLLLOCK up to a ~50 ms ceiling. Captures the
 * final PLLSTA value into ``s_usbhs_pllsta_probe`` regardless of
 * outcome so JLink reads can see the chip's view of the PLL state.
 *
 * @return ::ra8_err_t
 * @retval k_ra8_ok PLLLOCK observed within the per-attempt window.
 * @retval k_ra8_err_hw_timeout PLLLOCK never asserted.
 *
 * @pre LPSTS.SUSPENDM = 1 and PHYSET is configured for the chosen
 *      CLKSEL value.
 * @pre USB60CLK is running (PLL2P / 4 = 60 MHz).
 * @post Loop iteration count is bounded.
 * @post s_usbhs_pllsta_probe holds the final PLLSTA word.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_usbhs_wait_pll_lock_short(void)
{
  volatile uint16_t* const pllsta = ra8_usbhs_pllsta();
  ra8_err_t                lock   = k_ra8_err_hw_timeout;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usbhs_pll_lock_attempt_limit; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(pllsta, i, ((*pllsta & (uint16_t)k_ra8_pllsta_plllock) != 0U))) {
      lock = k_ra8_ok;
      break;
    }
#else
    if ((*pllsta & (uint16_t)k_ra8_pllsta_plllock) != 0U) {
      lock = k_ra8_ok;
      break;
    }
#endif
  }
  s_usbhs_pllsta_probe = *pllsta;
  return lock;
}

/**
 * @brief Drive PHYSET into "PHY in reset" with the given CLKSEL field.
 *
 * @details Asserts DIRPD=1 and PLLRESET=1, then writes the chosen
 * CLKSEL[1:0] codepoint. HUM Ch 37.2.17 PHYSET, p 2080 Table:
 * 00b=12 MHz, 01b=48 MHz, 10b=20 MHz, 11b=24 MHz. Used by both
 * the canonical bring-up path and the CLKSEL bisect harness.
 *
 * @param[in] physet_reg PHYSET register pointer (::ra8_usbhs_physet()).
 * @param[in] clksel_value Pre-shifted CLKSEL field value (0x00, 0x10,
 *                         0x20, or 0x30).
 *
 * @pre physet_reg non-null.
 * @pre LPSTS.SUSPENDM may be 0 or 1; HUM Ch 37.2.43 LPSTS, p 2111
 *      states only a small whitelist of registers can be written
 *      while SUSPENDM=0 and PHYSET is not on it. The harness clears
 *      SUSPENDM-then-sets it again per attempt to flush latched
 *      writes regardless.
 * @post PHYSET: DIRPD=1, PLLRESET=1, CLKSEL field = clksel_value.
 * @post No other PHYSET fields (REPSEL, HSEB, CDPEN) are touched.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_usbhs_arm_phy_reset(volatile uint16_t* physet_reg, uint16_t clksel_value)
{
  /* HUM Ch 37.2.17 PHYSET, p 2080: assert PHY power-down and PLL
   * reset, mask out CLKSEL[1:0], then OR in the candidate field. */
  uint16_t v  = *physet_reg;
  v           = (uint16_t)(v | (uint16_t)k_ra8_physet_dirpd | (uint16_t)k_ra8_physet_pllreset);
  v           = (uint16_t)(v & (uint16_t)~(uint16_t)k_ra8_physet_clksel);
  v           = (uint16_t)(v | (uint16_t)(clksel_value & (uint16_t)k_ra8_physet_clksel));
  *physet_reg = v;
}

/**
 * @brief One iteration of the CLKSEL bisect: try a single codepoint.
 *
 * @details For the given CLKSEL value:
 *   1. Toggle LPSTS.SUSPENDM 1->0->1 to flush latched PHYSET writes
 *      (HUM Ch 37.2.43 LPSTS p 2111: writes to USBHS regs only take
 *      effect once the PHY clock oscillates, which happens when
 *      SUSPENDM is set to 1).
 *   2. Arm PHYSET in reset with this CLKSEL.
 *   3. Release DIRPD (1 us settle), wait 1 ms, release PLLRESET.
 *   4. Set LPSTS.SUSPENDM = 1.
 *   5. Poll PLLSTA.PLLLOCK with the per-attempt budget.
 *
 * @param[in] physet PHYSET register pointer.
 * @param[in] lpsts  LPSTS register pointer.
 * @param[in] clksel_value PHYSET CLKSEL field value (0x00/10/20/30).
 *
 * @return ::ra8_err_t
 * @retval k_ra8_ok PLL locked under this CLKSEL.
 * @retval k_ra8_err_hw_timeout PLL did not lock within the per-attempt
 *                              window.
 *
 * @pre SYSCFG.HSE = 1.
 * @pre physet and lpsts pointers are non-null and point at the HS
 *      USBHS register block.
 * @post On success: PHY powered, PLLRESET cleared, SUSPENDM=1.
 * @post On failure: SUSPENDM=1 is left asserted; caller may retry the
 *       next CLKSEL value or surface the timeout.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_usbhs_try_clksel(volatile uint16_t* physet,
                                           volatile uint16_t* lpsts,
                                           uint16_t           clksel_value)
{
  /* Drop SUSPENDM so the PHY clock is gated; subsequent PHYSET writes
   * are then sequenced through a single SUSPENDM 0->1 transition. */
  *lpsts = (uint16_t)(*lpsts & (uint16_t)~(uint16_t)k_ra8_lpsts_suspendm);
  internal_usb_delay_1us();

  internal_usbhs_arm_phy_reset(physet, clksel_value);
  internal_usb_delay_1us();

  /* Release DIRPD (PHY analog up), wait 1 ms, release PLLRESET. */
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra8_physet_dirpd);
  ra8_delay_ms(1U);
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra8_physet_pllreset);

  /* HUM Ch 37.2.43 LPSTS, p 2111: SUSPENDM=1 starts the PHY clock,
   * which both commits any previously-latched PHYSET writes and
   * begins the PLL lock countdown. */
  *lpsts = (uint16_t)(*lpsts | (uint16_t)k_ra8_lpsts_suspendm);

  return internal_usbhs_wait_pll_lock_short();
}

/**
 * @brief USBHS embedded-PHY bring-up (HUM Figure 37.2 p 2121).
 *
 * @details Implements the device-mode PHY bring-up flow exactly as
 * documented in HUM Ch 37.3.3 + Figure 37.2. The PHY UTMI internal PLL
 * takes EXTAL directly (24 MHz on EK-RA8D2); USB60CLK = 60 MHz is the
 * separate LINK domain clock supplied by ::ra8_cgc_usbhs_pll_enable.
 *  1. SYSCFG |= HSE                   (high-speed enable, before PHY)
 *  2. PHYSET |= DIRPD | PLLRESET; CLKSEL = 11b (24 MHz)
 *  3. delay 1 us                      (HUM "Wait 1 us" after CLKSEL)
 *  4. PHYSET &= ~DIRPD                (release PHY analog power-down)
 *  5. delay 1 ms                      (HUM "Wait 1 ms" after DIRPD)
 *  6. PHYSET &= ~PLLRESET             (release UTMI PHY PLL reset)
 *  7. LPSTS  |= SUSPENDM              (start the PHY clock)
 *  8. wait PLLSTA.PLLLOCK = 1         (UTMI PLL lock confirmation)
 *  9. SYSCFG &= ~DRPD; |= USBE        (enable module operation)
 * 10. BUSWAIT = 0x0F04                (4-wait + reserved b11-b8)
 * 11. PHYSET |= REPSEL_16             (terminator adjust cycle)
 *
 * The HS instance has no SCKE bit (HUM Ch 37.2.1 "SYSCFG" register
 * layout, p 2060); writing bit 10 there is a no-op.
 *
 * @param[in] reg HS register block pointer (must be ::ra8_usb_hs()).
 *
 * @return ::ra8_err_t
 * @retval k_ra8_ok HS PHY locked and ready for ``DPRPU`` attach.
 * @retval k_ra8_err_hw_timeout PLLSTA.PLLLOCK never asserted.
 *
 * @pre MSTPB12 ungated; USB60CLK = PLL2P / 4 = 60 MHz.
 * @pre Caller is single-threaded init context.
 *
 * @post On success: PHY powered, PLL locked, USBE=1, SUSPENDM=1.
 * @post BUSWAIT programmed.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra8_err_t internal_usbhs_phy_bringup(volatile r_usb_regs_t* reg)
{
  volatile uint16_t* const physet = ra8_usbhs_physet();
  volatile uint16_t* const lpsts  = ra8_usbhs_lpsts();

  /* Step 1: SYSCFG.HSE = 1 (mirrors FSP `hw_usb_set_hse`, called from
   * r_usb_basic.c BEFORE hw_usb_pmodule_init). HUM Ch 37 "SYSCFG"
   * register, p 2060. */
  reg->SYSCFG      = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra8_syscfg_bit_hse));
  s_phy_step_probe = (uint8_t)k_ra8_usbhs_phy_step_hse_set;

  /* Step 2: CLKSEL = 24 MHz (PHYSET[5:4] = 11b, value 0x30).
   *
   * Per HUM Ch 37.3.3 "Supplying the Clock", Table 37.17 (p 2120) the
   * USB-PHY internal PLL takes its reference clock DIRECTLY FROM THE
   * EXTAL PIN -- not from USB60CLK. Allowed EXTAL frequencies are 12,
   * 20, 24 or 48 MHz; PHYSET.CLKSEL[1:0] selects which one.
   *
   * EK-RA8D2 fits a 24 MHz crystal (see ra8_cgc.c::k_ra8_pll2_local_t
   * comments), so CLKSEL must be 11b (k_ra8_physet_clksel_24). USB60CLK
   * (60 MHz, PLL2P/4) is still required -- it is the LINK domain clock
   * for the USBHS module itself -- but it does NOT feed the PHY UTMI
   * PLL. Earlier revisions of this code mis-modeled CLKSEL as
   * "USB60CLK / 5 = 12 MHz" and bisected over {12,48,24,20}; the host
   * never enumerated because the PHY PLL was locking at the wrong
   * frequency, producing malformed HS chirp K signaling. */
  s_phy_step_probe   = (uint8_t)k_ra8_usbhs_phy_step_clksel_12;
  ra8_err_t lock_err = internal_usbhs_try_clksel(physet, lpsts, (uint16_t)k_ra8_physet_clksel_24);
  s_clksel_attempt_pllsta[0] = s_usbhs_pllsta_probe;
  s_usbhs_init_probe         = (uint32_t)*physet;

  if (lock_err != k_ra8_ok) {
    s_clksel_winner = (uint8_t)k_ra8_usbhs_clksel_no_winner;
    ra8_log_error(s_tag, "usbhs: PHY PLL lock timeout (CLKSEL=24)");
    return lock_err;
  }
  s_clksel_winner  = (uint8_t)k_ra8_physet_clksel_24;
  s_phy_step_probe = (uint8_t)k_ra8_usbhs_phy_step_pll_locked;

  /* HUM Figure 37.2 p 2121: USBE/DRPD are programmed AFTER PLLLOCK
   * is observed. Internally splits into a separate helper to keep
   * each function under the NASA Rule 4 budget. */
  internal_usbhs_enable_syscfg(reg);
  s_phy_step_probe = (uint8_t)k_ra8_usbhs_phy_step_usbe_set;

  /* HUM Ch 37.2.2 "BUSWAIT : CPU Bus Wait Register", p 2062 */
  reg->BUSWAIT = (uint16_t)k_ra8_buswait_default;
  *physet      = (uint16_t)(*physet | (uint16_t)k_ra8_physet_repsel_16);
  return k_ra8_ok;
}

/**
 * @brief Programme the post-SYSCFG common registers (FIFO, DCP, INTENB).
 *
 * @details Shared tail of FS and HS device-mode bring-up: CFIFOSEL,
 * DCP defaults, and the device-mode INTENB0 interrupt mask.
 *
 * @param[in] reg Selected USB instance register block.
 *
 * @pre Caller has already enabled SYSCFG.USBE.
 * @pre reg is non-null.
 * @post All listed registers carry deterministic device-mode defaults.
 * @post INTENB1 / BRDYENB / NRDYENB / BEMPENB are cleared.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
void internal_usb_init_common(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  /* HUM Ch 37.2.8 "CFIFOSEL : CFIFO Port Select Register", p 2071 */
  reg->CFIFOSEL  = k_ra8_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra8_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra8_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1990 */
  /* HUM Ch 37.2.30 "DCPMAXP : DCP Max Packet Size Register", p 2092 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra8_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 37.2.18 INTSTS0 Note 3 (p 2082): "To clear the CTRT, DVST,
   * SOFR, RESM, or VBINT flags, write 0 only to the flags to be cleared.
   * Write 1 to the other flags. Do not write 0 to the status flags
   * indicating 0." A wholesale ``INTSTS0 = 0`` write therefore clears
   * any stale CTRT/DVST/SOFR/RESM/VBINT edges that the PHY bring-up
   * sequence latched (mirrors FSP IP1 ``hw_usb_pmodule_init``: see
   * r_usb_preg_access.c::hw_usb_pmodule_init which writes
   * ``INTSTS0 = 0`` immediately before INTENB0 on the HS branch).
   * Without this flush a stale RESM/VBINT bit gets ORed into every
   * polled-dispatch snapshot and clutters the JLink-readable
   * ``s_intsts0_observed_or`` probe (observed value 0xD0D0 with bits
   * 14/15 set even though the host issued no real resume). */
  reg->INTSTS0 = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980.
   * Mirrors FSP r_usb_basic.c::usb_pmodule_init mask: BEMP, BRDY, NRDY,
   * CTRT, DVST, SOFR, RSME, VBSE. Enabling SOFR + RSME is required for
   * HS so the IP latches resume / SOF events in INTSTS0; without RSME
   * a host wake from suspend leaves the device asleep. */
  /* SOFR and RSME are intentionally NOT enabled here:
   *   - SOFR fires every 125us on HS (= 8 kHz);
   *   - RSME stays asserted on USBHS via the PHY's USBR signal
   *     while the host holds the bus in resume signalling;
   * Both starve PendSV and the demo worker thread never gets
   * scheduled. NRDY is still useful (drives the bridge's per-pipe
   * NAK re-arm) and does not cause a storm on its own. RSME / SOFR
   * status is still readable from INTSTS0 for poll-style drivers. */
  reg->INTENB0 = (uint16_t)((1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_brdy) |
                            (1U << k_ra8_int0_bit_nrdy) | (1U << k_ra8_int0_bit_ctrt) |
                            (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse));
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;
}

/**
 * @brief USBFS module bring-up (FSP `hw_usb_pmodule_init` IP0 branch).
 *
 * @details Sets SCKE, polls SCKE-readback, clears DRPD, sets USBE.
 * The FS instance has no PHY-side registers (PHYSET / LPSTS / PLLSTA
 * are HS-only) so this is a pure SYSCFG-driven sequence.
 *
 * @param[in] reg FS register block pointer (must be ::ra8_usb_fs()).
 *
 * @return ::ra8_err_t
 * @retval k_ra8_ok USBE asserted.
 *
 * @pre MSTPB11 ungated; USB48CLK fed from PLL2P/5 = 48 MHz.
 * @pre Caller is single-threaded init context.
 *
 * @post SYSCFG: SCKE=1, DRPD=0, USBE=1.
 * @post Loop iteration count is bounded.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra8_err_t internal_usbfs_module_bringup(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra8_syscfg_bit_scke));
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usbhs_scke_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->SYSCFG & (uint16_t)(1U << k_ra8_syscfg_bit_scke)) != 0U) {  /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }
  reg->SYSCFG = (uint16_t)(reg->SYSCFG & (uint16_t)~(uint16_t)(1U << k_ra8_syscfg_bit_drpd));
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra8_syscfg_bit_usbe));
  return k_ra8_ok;
}

/**
 * @file ra8_cgc_usb.c
 * @brief Clock Generation Circuit driver -- PLL2 + USB-FS / USB-HS clocks.
 *
 * @details
 * Split out of `ra8_cgc.c`. Brings up PLL2 (960 MHz VCO, PLL2P = 240 MHz on
 * the EK-RA8D2 24 MHz crystal) and routes the two USB controller clocks off
 * it:
 *
 *  - USB-FS: USBCKCR sources PLL2P / 5 = 48.000 MHz (0 ppm vs the USB-FS
 *    48 MHz +/- 0.25 % spec).
 *  - USB-HS: USB60CKCR sources PLL2P / 4 = 60.000 MHz, the named operating
 *    clock for the USBHS module (HUM Ch 37.3.3 "Supplying the Clock").
 *
 * Every protected-register write is wrapped in `RA8_PROTECTED_WRITE` so the
 * PRCR re-lock always happens, even on early-return paths. The bounded OSCSF
 * poll helpers and the shared PLL-multiplier scale factor live in
 * `ra8_cgc_internal.h`; `ra8_cgc_ensure_hoco_running_for_usb_ck` is defined
 * here and reused by `ra8_cgc_eswclk.c`.
 *
 * @par Tag ring/world:
 * Ring 1 (HAL), World S (secure-callable). Same ring/world as the parent
 * `ra8_cgc.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_cgc_internal.h"
#include "ra8_cgc_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"
#include "ra8_time_constants.h"

static const char* s_tag = "CGC";

/**
 * @var s_usb60ckcr_probe
 * @brief Last value read back from R_SYSTEM->USB60CKCR at the end of
 *        ::internal_usb60ckcr_switch_to_pll2p_div4.
 *
 * @details
 * JLink-readable witness for the USBHS 60 MHz clock-source SREQ/SRDY
 * handshake. After a successful handshake we expect:
 *   USB60CKCR = 0x06 (USB60CKSEL=PLL2P, USB60CKSREQ=0, USB60CKSRDY=0).
 * On timeout the value captured here pins down which step of the
 * handshake stalled.
 *
 * Resolve the runtime address from the per-app .map file.
 *
 * @note Diagnostic-only; never read by production code paths.
 * @since 0.1.0
 */
static volatile uint8_t s_usb60ckcr_probe = 0U;

/**
 * @var s_pll2_status_probe
 * @brief Last value read back from R_SYSTEM->OSCSF at the end of
 *        ::internal_usb60ckcr_switch_to_pll2p_div4.
 *
 * @details
 * Bit 6 (PLL2SF) must be 1 for the USB60CKCR clock-switch handshake
 * to make forward progress. JLink-readable witness used in tandem
 * with ::s_usb60ckcr_probe to disambiguate "PLL2 never locked" from
 * "SREQ/SRDY ordering wrong".
 *
 * Resolve the runtime address from the per-app .map file.
 *
 * @note Diagnostic-only; never read by production code paths.
 * @since 0.1.0
 */
static volatile uint8_t s_pll2_status_probe = 0U;

/**
 * @enum ra8_usbfs_clock_local_t
 * @brief Local sentinels for the USBCKSRDY handshake and divider codes.
 *
 * @details
 * USBCKDIVCR uses a non-linear code-to-ratio map (FSP `bsp_clocks.c`):
 *   /1=0, /2=1, /3=5, /4=2, /5=6, /6=3, /8=4, /10=7, /16=8.
 * We expose only the divisors this driver actually uses.
 */
typedef enum : uint32_t {
  /* The USBCKSREQ -> USBCKSRDY synchronizer chain crosses from ICLK
   * into the UCK clock domain (currently HOCO @ ~20 MHz before this
   * routine runs). Worst case is a few HOCO cycles plus the M85 spin
   * latency at 1 GHz. FSP polls this unbounded; we cap at ~600 us to
   * stay NASA-Rule-2-compliant while leaving four orders of magnitude
   * of slack over the worst-case real handshake. */
  k_ra8_usbfs_srdy_poll_limit = 200000U, /**< Iterations before timeout.   */
  k_ra8_usbfs_div5_code       = 6U,      /**< USBCKDIVCR codepoint for /5. */
  k_ra8_usbfs_div8_code       = 4U,      /**< USBCKDIVCR codepoint for /8. */
} ra8_usbfs_clock_local_t;

/**
 * @enum ra8_usbfs_mstp_local_t
 * @brief Local sentinels for the USBFS module-stop pre-step.
 *
 * @details
 * HUM Ch 9 -- the entire xCKCR / xCKDIVCR family ("Clock Selection
 * Switching Procedure") requires the dependent module to be in the
 * module-stop state (MSTP = 1) BEFORE asserting xCKSREQ when changing
 * the divider from 1/n where n != 1. Since this routine programmes
 * USBCKDIVCR from reset (1/1) to /5, that precondition applies. Pages
 * 367 (ADCCKDIVCR), 370 (SPICKCR), 373 (GPTCKDIVCR), 378 (ETHPCKDIVCR),
 * and 381 (ESWPCKCR) all give the same instruction. If the caller has
 * already released MSTPB11 (USBFS) before us the SREQ -> SRDY handshake
 * silently hangs -- the symptom we observed on real silicon was
 * USBCKCR == 0x40 (SREQ=1, SRDY=0) and the host never seeing the
 * device. Force the module-stop bit back to 1 inside the PRCR window so
 * the routine is robust to call ordering.
 */
typedef enum : uint32_t {
  k_ra8_mstpb11_usbfs_mask = (uint32_t)(1UL << 11), /**< MSTPCRB.MSTPB11 (USBFS0). */
} ra8_usbfs_mstp_local_t;

/**
 * @enum ra8_pll2_local_t
 * @brief Local constants for the PLL2 bring-up path.
 *
 * @details
 * The EK-RA8D2 board fits a 24 MHz crystal (per the schematic and the
 * existing PLL1 setup). For USB-FS we want a /5 divider downstream so
 * PLL2P needs to be 240 MHz, which means a 960 MHz VCO (the silicon
 * minimum). PL2IDIV = /2 -> 12 MHz pre-scale. PLL2MUL = 80 (integer)
 * with PLL2MULNF = 0 (no quarter steps) -> 12 * 80 = 960 MHz. PL2ODIVP
 * = /4 -> 240 MHz at PLL2P. USBCKDIVCR = /5 -> 48 MHz exactly.
 *
 * Spec compliance: USBCKCR source 240 MHz * (1/5) = 48.000 MHz
 * (0 ppm error vs the USB-FS spec target of 48 MHz +/- 2500 ppm =
 * +/- 0.25 %). PASS.
 */
typedef enum : uint16_t {
  k_ra8_pll2_max_quarters   = 3U,  /**< Max value of PLL2MULNF[7:6].            */
  k_ra8_pll2_usbfs_mul      = 80U, /**< Integer multiplier for PLL2 USBFS path. */
  k_ra8_pll2_usbfs_quarters = 0U,  /**< Fractional quarter-steps (none).        */
} ra8_pll2_local_t;

/**
 * @brief Spin until USBCKCR.USBCKSRDY matches the expected value.
 * @details See implementation.
 * @param[in] expected Expected value for USBCKSRDY (0 or 1).
 * @return Result code.
 * @retval k_ra8_ok USBCKSRDY reached the expected level.
 * @retval k_ra8_err_hw_timeout Polling exceeded the iteration cap.
 * @pre PRCR-CGC must be unlocked at call site.
 * @pre Caller is in single-threaded init context.
 * @post No mutation; readback only.
 * @post Iteration count is bounded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_usbcksrdy(uint8_t expected)
{
  volatile uint8_t* const usbckcr = ra8_sys_usbckcr();
  const uint8_t           mask    = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
  /* Bounded wait through ra8_hw_err.h: on host tests the ra8_fake_mmio
   * seam decides the poll (first-poll success unless a test arms a
   * fault), so the real timeout leg is reachable everywhere. */
  if (expected != 0U) {
    return ra8_hw_wait_flag_set8(usbckcr, mask, (uint32_t)k_ra8_usbfs_srdy_poll_limit);
  }
  return ra8_hw_wait_flag_clear8(usbckcr, mask, (uint32_t)k_ra8_usbfs_srdy_poll_limit);
}

/**
 * @brief PRCR-protected stop+program+restart body of ::ra8_cgc_pll2_enable.
 * @details See implementation; mirrors ::internal_cgc_init_protected.
 * @param[in] pll2ccr  Pre-computed PLL2CCR value.
 * @param[in] pll2ccr2 Pre-computed PLL2CCR2 value.
 * @return ra8_err_t error code.
 * @retval k_ra8_ok PLL2 stopped, re-programmed, and re-locked.
 * @retval k_ra8_err_hw_timeout PLL2SF stop or lock wait exceeded.
 * @pre  Caller is single-threaded init context; CPU not sourced from PLL2.
 * @pre  PLL2 was previously stopped (OSCSF.PLL2SF=0 expected on entry).
 * @post On k_ra8_ok PLL2 is locked at the new mul / divider.
 * @post PRCR re-locked unconditionally (RA8_PROTECTED_WRITE invariant).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_pll2_program_protected(uint32_t pll2ccr, uint16_t pll2ccr2)
{
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.11 "PLL2CR : PLL2 Control Register" p 336 -- stop
     * PLL2 before writing PLL2CCR / PLL2CCR2. */
    *ra8_sys_pll2cr() = (uint8_t)k_ra8_pll2cr_stop;
    err               = ra8_cgc_wait_oscsf_clear(k_ra8_oscsf_bit_pll2sf);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "pll2: stop wait timeout");
      break;
    }
    /* HUM Ch 9.2.10 "PLL2CCR : PLL2 Clock Control Register" p 335 */
    *ra8_sys_pll2ccr() = pll2ccr;
    /* HUM Ch 9.2.12 "PLL2CCR2 : PLL2 Clock Control Register 2" p 335 */
    *ra8_sys_pll2ccr2() = pll2ccr2;
    *ra8_sys_pll2cr()   = (uint8_t)k_ra8_pll2cr_run;
    err                 = ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_pll2sf);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "pll2: lock wait timeout");
      break;
    }
  }
  return err;
}

/**
 * @brief Implementation of `ra8_cgc_pll2_enable()`.
 *
 * @details
 * Idempotent: if OSCSF.PLL2SF is already asserted on entry the
 * function returns ::k_ra8_ok without touching PLL2CCR / PLL2CCR2 /
 * PLL2CR. This lets the FS and HS USB bring-up paths (which both want
 * the same 960 MHz VCO with PLL2P = /4 = 240 MHz) coexist on the same
 * boot without re-programming a live PLL2 (HUM Ch 9 forbids writing
 * PLL2CCR while PLL2 is running, and downstream consumers -- including
 * USBCKCR sourced from PLL2P after the FS path runs -- would lose
 * their clock during the re-program).
 *
 * Stops PLL2 (with bounded poll on OSCSF.PLL2SF clear), programmes
 * PLL2CCR + PLL2CCR2, then starts PLL2 (bounded poll on OSCSF.PLL2SF
 * set). Input divider is hard-wired to /2 and source to main XTAL
 * because that is the only configuration the EK-RA8D2 (24 MHz crystal)
 * needs today; broaden the API once a second board lands.
 *
 * @param[in] mul_int      Integer multiplier (1..255).
 * @param[in] mul_quarters Quarter-step fractional multiplier (0..3).
 * @param[in] p_div_code   ::ra8_plodiv_t code for the P output divider.
 *
 * @return Result code.
 * @retval k_ra8_ok            PLL2 locked.
 * @retval k_ra8_err_invalid_arg ``mul_int`` is 0 or ``mul_quarters > 3``.
 * @retval k_ra8_err_hw_timeout PLL2SF stop or start handshake timed out.
 *
 * @pre  ::ra8_cgc_init has been called and main XTAL is stable.
 * @pre  Single-threaded init context; CPU not sourced from PLL2.
 *
 * @post On k_ra8_ok: PLL2 is locked at the requested multiplier and
 *       output divider; PRCR is re-locked.
 * @post On any error path the PRCR window is closed by RA8_PROTECTED_WRITE.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_cgc_pll2_enable(uint8_t mul_int, uint8_t mul_quarters, ra8_plodiv_t p_div_code)
{
  if (mul_int == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint16_t)mul_quarters > (uint16_t)k_ra8_pll2_max_quarters) {
    return k_ra8_err_invalid_arg;
  }

  ra8_log_info_val(s_tag, "pll2 enable mul_int", (uint32_t)mul_int);

  /* Idempotency (HUM Ch 9): never re-program a running PLL2 -- if FS
   * path already locked it, the HS path's "same VCO" request is a no-op. */
  if ((*ra8_sys_oscsf() & (uint8_t)(1U << k_ra8_oscsf_bit_pll2sf)) != 0U) {
    ra8_log_info(s_tag, "pll2 already locked -- skip re-program");
    return k_ra8_ok;
  }

  /* PLL2CCR (32-bit, HUM Ch 9.2.9): PLL2MULNF[7:6] | PLL2MUL[16:8]. */
  const uint32_t mul_quarters_field =
    ((uint32_t)mul_int * (uint32_t)k_ra8_cgc_quarters_per_unit) + (uint32_t)mul_quarters;
  const uint32_t pll2ccr =
    ((mul_quarters_field & (uint32_t)k_ra8_pll2ccr_mask_quarters) << k_ra8_pllccr_shift_quarters) |
    ((uint32_t)k_ra8_plsrcsel_main << k_ra8_pllccr_shift_plsrcsel) |
    ((uint32_t)k_ra8_plidiv_div2 & (uint32_t)k_ra8_pll2ccr_mask_plidiv);

  /* PLL2CCR2: P from caller; Q/R fixed at /6. Code 0 is prohibited
   * per HUM Ch 9.2.10/9.2.12 (drops the whole 16-bit write). */
  const uint16_t pll2ccr2 =
    (uint16_t)(((uint16_t)k_ra8_plodiv_div6 << k_ra8_pllccr2_shift_plodivr) |
               ((uint16_t)k_ra8_plodiv_div6 << k_ra8_pllccr2_shift_plodivq) |
               ((uint16_t)p_div_code << k_ra8_pllccr2_shift_plodivp));

  const ra8_err_t err = internal_pll2_program_protected(pll2ccr, pll2ccr2);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_log_info(s_tag, "pll2 locked");
  return k_ra8_ok;
}

/**
 * @brief PRCR-protected USBCKCR / USBCKDIVCR handshake body.
 * @details Steps 3..7 of the HUM "Clock selection switching procedure"
 * inside the CGC-PRCR window: SREQ=1, wait SRDY=1, write USBCKDIVCR,
 * write USBCKCR=src|SREQ, write USBCKCR=src, wait SRDY=0.
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok Handshake completed; USBCLK on PLL2P/5 = 48 MHz.
 * @retval k_ra8_err_hw_timeout SRDY=1 or SRDY=0 wait exceeded.
 * @pre PLL2 locked at 240 MHz on PLL2P.
 * @pre MSTPCRB.MSTPB11 = 1 (USBFS module-stopped).
 * @post On k_ra8_ok USBCKCR.USBCKSEL = PLL2P, USBCKDIVCR = /5.
 * @post PRCR re-locked (RA8_PROTECTED_WRITE always re-locks).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra8_err_t internal_usbckcr_switch_to_pll2p_div5(void)
{
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_usbckcr_bit_sreq);
    *ra8_sys_usbckcr()      = sreq_mask;
    err                     = internal_wait_usbcksrdy(1U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "usbfs: SRDY=1 timeout");
      break;
    }
    *ra8_sys_usbckdivcr() = (uint8_t)k_ra8_usbfs_div5_code;
    const uint8_t src     = (uint8_t)((uint8_t)k_ra8_usbcksel_pll2p & k_ra8_usbckcr_mask_sel);
    *ra8_sys_usbckcr()    = (uint8_t)(src | sreq_mask);
    *ra8_sys_usbckcr()    = src;
    err                   = internal_wait_usbcksrdy(0U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "usbfs: SRDY=0 timeout");
      break;
    }
  }
  return err;
}

/**
 * @brief Bring up the 48 MHz USB-FS clock from PLL2P / 5.
 *
 * @details
 * 1. Enable PLL2 at 960 MHz VCO with PLL2P = /4 = 240 MHz.
 * 2. Force MSTPB11 = 1 (USBFS module-stop) before reprogramming
 *    USBCKDIVCR per HUM Ch 9 "Clock selection switching procedure".
 * 3. Run the SREQ / SRDY handshake to swap USBCKCR.SEL to PLL2P
 *    and USBCKDIVCR to /5, landing 48.000 MHz on USBCLK
 *    (0 ppm vs the USB-FS 48 MHz +/- 0.25 % spec).
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok USB-FS clock running at exactly 48 MHz.
 * @retval k_ra8_err_hw_init_failed PLL2 enable failed; see log.
 * @retval k_ra8_err_hw_timeout USBCKCR SREQ/SRDY handshake timed out.
 *
 * @pre  Caller is single-threaded init context.
 * @pre  PLL1 / SCKSCR have already been programmed by ra8_cgc_init.
 * @post On k_ra8_ok PLL2 is locked, MSTPB11 is set, USBCKCR sources
 *       PLL2P / 5 = 48 MHz, and PRCR is re-locked.
 * @post On error the USB-FS clock is left in a quiesced state.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra8_err_t ra8_cgc_usbfs_clock_enable(
  void) // NOLINT(readability-function-size,readability-function-cognitive-complexity)
{
  ra8_log_info(s_tag, "usbfs clock enable");

  /* Step 1: bring up PLL2 to land 240 MHz on PLL2P. */
  const ra8_err_t pll2_err = ra8_cgc_pll2_enable((uint8_t)k_ra8_pll2_usbfs_mul,
                                                 (uint8_t)k_ra8_pll2_usbfs_quarters,
                                                 k_ra8_plodiv_div4);
  if (pll2_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "usbfs: pll2 enable failed", (uint32_t)pll2_err);
    return pll2_err;
  }

  /* Step 2a: Force USBFS into module-stop (MSTPB11 = 1) BEFORE the SREQ
   * handshake. HUM Ch 9 "Clock selection switching procedure" step 1
   * (p367 / p370 / p373 / p378 / p381) is mandatory whenever USBCKDIVCR
   * is moved off 1/1; we are programming /5 below. MSTPCR is NOT
   * PRCR-protected. ra8_usb_device_init() releases MSTPB11 again later
   * via the ref-counted ra8_mstp_enable() path. */
  ra8_mstp()->MSTPCRB |= (uint32_t)k_ra8_mstpb11_usbfs_mask;
  (void)ra8_mstp()->MSTPCRB; /* HUM 11.2.7 Note 2: read-back. */

  /* Step 2a': Ensure HOCO is running. USBCKCR's reset-default source is
   * HOCO; the SREQ -> SRDY synchronizer crosses from ICLK into the
   * CURRENT UCK source's clock domain (HOCO). If HOCO is stopped
   * (HCSTP=1, the chip reset state) the handshake never completes -- we
   * observed USBCKCR=0x40 (SREQ=1, SRDY=0) with HOCOCR=0x01 and
   * OSCSF.HOCOSF=0 on real silicon. ra8_cgc_init() leaves HOCO stopped
   * because it switches SCKSCR to PLL1; we have to start HOCO here so
   * the source clock for USBCKCR is alive when SREQ is asserted.
   * HOCOCR IS PRCR-protected (HUM Ch 9; FSP wraps writes inside its
   * BSP_PRV_PRCR_UNLOCK / BSP_PRV_PRCR_LOCK window -- writes outside
   * the window are silently dropped, which is what we observed when
   * an earlier attempt left HOCOCR at 0x01 after the store). */
  ra8_err_t hoco_err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    volatile uint8_t* const hococr = ra8_sys_hococr();
    if ((*hococr & (uint8_t)(1U << k_ra8_hococr_hcstp)) != 0U) {
      *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t) ~(1U << k_ra8_hococr_hcstp));
    }
  }
  /* OSCSF.HOCOSF poll outside PRCR window (read-only register). */
  hoco_err = ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_hocosf);
  if (hoco_err != k_ra8_ok) {
    ra8_log_error(s_tag, "usbfs: HOCO stabilization timeout");
    return hoco_err;
  }
  ra8_log_info(s_tag, "usbfs: HOCO running for USBCKCR source");

  /* Step 2b: USBCKCR / USBCKDIVCR handshake. */
  const ra8_err_t err = internal_usbckcr_switch_to_pll2p_div5();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_log_info(s_tag, "usbfs clock ready (PLL2P/5 = 48 MHz)");
  return k_ra8_ok;
}

/**
 * @enum ra8_usbhs_clock_local_t
 * @brief Local sentinels for the USB60CKCR SREQ/SRDY handshake.
 *
 * @details
 * USB60CKCR shares the bit layout of USBCKCR (HUM Ch 9; CMSIS
 * `R7KA8D2KF_core0.h` -- `R_SYSTEM->USB60CKCR_b` mirrors
 * `USBCKCR_b` with USB60CKSEL[3:0], USB60CKSREQ@bit6, USB60CKSRDY@bit7).
 * USB60CKDIVCR uses the same non-linear codepoint map as USBCKDIVCR
 * (FSP `bsp_clocks.c`):
 *   /1=0, /2=1, /3=5, /4=2, /5=6, /6=3, /8=4, /10=7, /16=8.
 * For the EK-RA8D2 USBHS PHY we land 60.000 MHz on USB60CLK from
 * PLL2P (240 MHz) / 4 = 60.000 MHz, matching the HUM Ch 37.3.3
 * "Supplying the Clock" requirement: "A 60-MHz clock must be supplied
 * ... USB60CLK is the operating clock for the USBHS module" (HUM
 * p 2102, line 85433). The PHY then internally divides USB60CLK to
 * derive its USBMCLK reference; the PHYSET CLKSEL[1:0] field selects
 * the after-division frequency (12 / 20 / 24 / 48 MHz; HUM Ch 37.2.17
 * PHYSET, p 2080). The earlier "USB60CLK = 48 MHz, CLKSEL=48"
 * configuration violated the named-rate contract on the register and
 * made macOS issue ~43 USB resets without RHST ever advancing past
 * 000 (chirp limbo). PLL2 is configured at 960 MHz VCO with
 * PL2ODIVP=/4 -> PLL2P=240 MHz, the same setup the FS path uses
 * (see ::ra8_cgc_usbfs_clock_enable).
 *
 * Codepoint table from HUM Ch 9 USB60CKDIVCR (matches USBCKDIVCR):
 *   /1=0, /2=1, /3=5, /4=2, /5=6, /6=3, /8=4, /10=7, /16=8.
 */
typedef enum : uint32_t {
  k_ra8_usbhs_srdy_poll_limit = 200000U, /**< Iterations before timeout. */
  k_ra8_usbhs_div4_code       = 2U,      /**< USB60CKDIVCR codepoint /4. */
} ra8_usbhs_clock_local_t;

/**
 * @enum ra8_usbhs_mstp_local_t
 * @brief Local sentinels for the USBHS module-stop pre-step.
 *
 * @details
 * Mirrors the FS path's MSTPB11 pre-step. HUM Ch 9 "Clock Selection
 * Switching Procedure" requires the dependent module to be in module-
 * stop state (MSTP=1) BEFORE asserting SREQ when changing the divider
 * from 1/n with n!=1. We programme USB60CKDIVCR from reset (1/1) to /4,
 * so this precondition applies. If the caller has already released
 * MSTPB12 (USBHS) the SREQ->SRDY handshake silently hangs.
 */
typedef enum : uint32_t {
  k_ra8_mstpb12_usbhs_mask = (uint32_t)(1UL << 12), /**< MSTPCRB.MSTPB12 (USBHS). */
} ra8_usbhs_mstp_local_t;

/**
 * @brief Spin until USB60CKCR.USB60CKSRDY matches the expected value.
 * @details See implementation.
 * @param[in] expected Expected value for USB60CKSRDY (0 or 1).
 * @return Result code.
 * @retval k_ra8_ok      USB60CKSRDY reached the expected level.
 * @retval k_ra8_err_hw_timeout Polling exceeded the iteration cap.
 * @pre PRCR-CGC must be unlocked at call site.
 * @pre Caller is in single-threaded init context.
 * @post No mutation; readback only.
 * @post Iteration count is bounded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_usb60cksrdy(uint8_t expected)
{
  volatile uint8_t* const usb60ckcr = ra8_sys_usb60ckcr();
  const uint8_t           mask      = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
  /* Bounded wait through ra8_hw_err.h: on host tests the ra8_fake_mmio
   * seam decides the poll (first-poll success unless a test arms a
   * fault), so the real timeout leg is reachable everywhere. */
  if (expected != 0U) {
    return ra8_hw_wait_flag_set8(usb60ckcr, mask, (uint32_t)k_ra8_usbhs_srdy_poll_limit);
  }
  return ra8_hw_wait_flag_clear8(usb60ckcr, mask, (uint32_t)k_ra8_usbhs_srdy_poll_limit);
}

/**
 * @brief PRCR-protected USB60CKCR / USB60CKDIVCR handshake body.
 * @details Mirror of ::internal_usbckcr_switch_to_pll2p_div5 for the
 *  USBHS 60 MHz path: SREQ=1, wait SRDY=1, write USB60CKDIVCR=/4,
 *  write USB60CKCR=src|SREQ, write USB60CKCR=src, wait SRDY=0.
 *  HUM Ch 9 "Clock Generation Circuit", USB60CKCR description (USBHS
 *  60 MHz source select). FSP `bsp_clocks.c` ::bsp_peripheral_clock_set
 *  is the canonical reference for the SREQ/SRDY ordering.
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok USBHS 60 MHz clock running on PLL2P/4.
 * @retval k_ra8_err_hw_timeout SRDY=1 or SRDY=0 wait exceeded.
 * @pre PLL2 locked at 240 MHz on PLL2P.
 * @pre MSTPCRB.MSTPB12 = 1 (USBHS module-stopped).
 * @post On k_ra8_ok USB60CKCR.USB60CKSEL = PLL2P, USB60CKDIVCR = /4.
 * @post PRCR re-locked (RA8_PROTECTED_WRITE always re-locks).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra8_err_t internal_usb60ckcr_switch_to_pll2p_div4(void)
{
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_usbckcr_bit_sreq);
    const uint8_t srdy_mask = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
    /* FSP `bsp_clocks.c` ::bsp_peripheral_clock_set sequence (HUM Ch 9
     * "Clock selection switching procedure"):
     *
     *   1. Set SREQ=1 with read-modify-write so the current SEL field
     *      is preserved (the reset-default source is HOCO; clobbering
     *      SEL to 0 also writes "HOCO" but loses the invariant that
     *      SREQ is asserted on top of the EXISTING source -- this
     *      matters when the previous source is no longer running).
     *   2. Spin-wait SRDY=1 (handshake acknowledge).
     *   3. Programme USB60CKDIVCR (peripheral clock now stopped).
     *   4. Write `source | SREQ | SRDY` -- the SRDY bit being set in
     *      the same write is mandatory per FSP (line 2896 of
     *      `bsp_clocks.c`); on RA8 Gen2 silicon, omitting SRDY here
     *      makes the subsequent SREQ-clear hang at step 6.
     *   5. Clear SREQ via read-modify-write (peripheral clock starts).
     *   6. Spin-wait SRDY=0 (start acknowledged).                       */
    volatile uint8_t* const ckcr = ra8_sys_usb60ckcr();
    *ckcr                        = (uint8_t)(*ckcr | sreq_mask);
    err                          = internal_wait_usb60cksrdy(1U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "usbhs: SRDY=1 timeout");
      break;
    }
    *ra8_sys_usb60ckdivcr() = (uint8_t)k_ra8_usbhs_div4_code;
    const uint8_t src       = (uint8_t)((uint8_t)k_ra8_usbcksel_pll2p & k_ra8_usbckcr_mask_sel);
    *ckcr                   = (uint8_t)(src | sreq_mask | srdy_mask);
    *ckcr                   = (uint8_t)(*ckcr & (uint8_t)~sreq_mask);
    err                     = internal_wait_usb60cksrdy(0U);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "usbhs: SRDY=0 timeout");
      break;
    }
  }
  /* Diagnostic witnesses -- read OUTSIDE the PRCR window since
   * USB60CKCR / OSCSF reads are not protected. */
  s_usb60ckcr_probe   = *ra8_sys_usb60ckcr();
  s_pll2_status_probe = *ra8_sys_oscsf();
  return err;
}

ra8_err_t ra8_cgc_ensure_hoco_running_for_usb_ck(void)
{
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    volatile uint8_t* const hococr = ra8_sys_hococr();
    if ((*hococr & (uint8_t)(1U << k_ra8_hococr_hcstp)) != 0U) {
      *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t) ~(1U << k_ra8_hococr_hcstp));
    }
  }
  /* On host tests the OSCSF wait consults the ra8_fake_mmio seam and
   * succeeds on its first poll unless a test arms a fault -- no HOCOSF
   * RAM seeding is needed for callers (USB60CKCR / ESWCKCR SREQ->SRDY
   * handshakes) to make progress. */
  return ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_hocosf);
}

/**
 * @brief Bring up the USBHS PHY 60 MHz reference clock + module clock path.
 *
 * @details
 * The USBHS PHY's internal 480 MHz CDR PLL requires a stable 12 MHz
 * reference, derived inside the PHY from a /5 division of the 60 MHz
 * USB60CLK. This routine programmes USB60CKCR / USB60CKDIVCR (HUM
 * Ch 9 "Clock Generation Circuit") so USB60CLK = PLL2P / 4 = 60.000
 * MHz exactly (PLL2P = 240 MHz from the same configuration the FS
 * path uses, see ::ra8_cgc_usbfs_clock_enable). Mirrors the canonical
 * FSP ``bsp_clocks.c`` ``bsp_peripheral_clock_set`` SREQ/SRDY ordering.
 *
 * Sequence:
 *  1. Wait OSCSF.MOSCSF (main XTAL stable).
 *  2. Enable PLL2 at 240 MHz on PLL2P.
 *  3. Force MSTPCRB.MSTPB12 = 1 (USBHS module-stop) before changing
 *     USB60CKDIVCR off 1/1, per HUM "Clock selection switching
 *     procedure".
 *  4. Ensure HOCO is running (USB60CKCR's reset-default source) so
 *     the SREQ -> SRDY synchronizer chain can drain.
 *  5. SREQ/SRDY handshake on USB60CKCR/USB60CKDIVCR, landing
 *     USB60CLK = PLL2P / 4 = 60 MHz exactly.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok USBHS 60 MHz clock running on PLL2P/4.
 * @retval k_ra8_err_hw_timeout Main XTAL, HOCO, or USB60CKSRDY handshake
 *         timed out.
 * @retval k_ra8_err_invalid_arg PLL2 mul/quarters out of range
 *         (propagated from ::ra8_cgc_pll2_enable).
 *
 * @pre  ::ra8_cgc_init has been called.
 * @pre  Caller is single-threaded init context; CPU not on PLL2.
 *
 * @post On k_ra8_ok: PLL2 locked at 240 MHz on PLL2P, MSTPB12 = 1,
 *       USB60CKCR.USB60CKSEL = PLL2P, USB60CKDIVCR = /4.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe; init context only.
 *
 * @see ra8_cgc_usbfs_clock_enable
 * @see ra8_cgc_pll2_enable
 *
 * @since 0.1.0
 */
ra8_err_t ra8_cgc_usbhs_pll_enable(
  void) // NOLINT(readability-function-size,readability-function-cognitive-complexity)
{
  ra8_log_info(s_tag, "usbhs phy clock enable");

  /* Step 1: HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag
   * Register", p 344. The USBHS PHY's 12 MHz reference is derived
   * from the main XTAL; without MOSCSF=1 the PHY cannot lock. */
  const ra8_err_t osc_err = ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_moscsf);
  if (osc_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "usbhs: main xtal not stable", (uint32_t)osc_err);
    return osc_err;
  }

  /* Step 2: bring up PLL2 to land 240 MHz on PLL2P. Reuse the FS path's
   * configuration -- 24 MHz XTAL / PL2IDIV(/2) * MUL(80) = 960 MHz VCO,
   * PL2ODIVP=/4 -> PLL2P = 240 MHz. PLL2P / 4 = 60.000 MHz on USB60CLK
   * exactly (0 ppm vs the USBHS 60 MHz spec target). ra8_cgc_pll2_enable
   * is idempotent w.r.t. the PRCR window and tolerates being called
   * once per USB controller bring-up (FS path also calls it). */
  const ra8_err_t pll2_err = ra8_cgc_pll2_enable((uint8_t)k_ra8_pll2_usbfs_mul,
                                                 (uint8_t)k_ra8_pll2_usbfs_quarters,
                                                 k_ra8_plodiv_div4);
  if (pll2_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "usbhs: pll2 enable failed", (uint32_t)pll2_err);
    return pll2_err;
  }

  /* Step 3: Force USBHS into module-stop (MSTPB12 = 1) BEFORE the SREQ
   * handshake. HUM Ch 9 "Clock selection switching procedure" step 1 is
   * mandatory whenever USB60CKDIVCR is moved off 1/1 (we are programming
   * /4 below). MSTPCR is NOT PRCR-protected. ra8_usb_device_init() will
   * release MSTPB12 again later via the ref-counted ra8_mstp_enable
   * path for k_ra8_mstp_usbhs. */
  ra8_mstp()->MSTPCRB |= (uint32_t)k_ra8_mstpb12_usbhs_mask;
  (void)ra8_mstp()->MSTPCRB; /* HUM 11.2.7 Note 2: read-back. */

  /* Step 4: Ensure HOCO is running so the SREQ->SRDY synchronizer can
   * drain (USB60CKCR's reset-default source is HOCO). */
  const ra8_err_t hoco_err = ra8_cgc_ensure_hoco_running_for_usb_ck();
  if (hoco_err != k_ra8_ok) {
    ra8_log_error(s_tag, "usbhs: HOCO stabilization timeout");
    return hoco_err;
  }

  /* Step 5: USB60CKCR / USB60CKDIVCR SREQ/SRDY handshake. */
  const ra8_err_t err = internal_usb60ckcr_switch_to_pll2p_div4();
  if (err != k_ra8_ok) {
    return err;
  }

  ra8_log_info(s_tag, "usbhs phy clock ready (PLL2P/4 = 60 MHz)");
  return k_ra8_ok;
}

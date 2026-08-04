/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cgc_eswclk.c
 * @brief Clock Generation Circuit driver -- Ethernet-switch (ESWCLK) path.
 *
 * @details
 * Split out of `ra8_cgc.c`. Powers on the ESWM peripheral domain and routes
 * the Ethernet-switch clocks off PLL1P:
 *
 *  - ESWCLK    = PLL1P / 4 = 250 MHz (ESWM core clock).
 *  - ESWPHYCLK = PLL1P / 2 = 500 MHz (Ethernet-PHY interface clock).
 *
 * Drives the canonical FSP `bsp_peripheral_clock_set` SREQ/SRDY handshake on
 * ESWCKCR then ESWPCKCR. HOCO must be running before the handshake (the
 * SREQ -> SRDY synchronizer crosses into the CURRENT source domain, which is
 * HOCO at reset); this file reuses `ra8_cgc_ensure_hoco_running_for_usb_ck`
 * defined in `ra8_cgc_usb.c`. Every protected-register write is wrapped in
 * `RA8_PROTECTED_WRITE` so the PRCR re-lock always happens.
 *
 * @par Tag ring/world:
 * Ring 1 (HAL), World S (secure-callable). Same ring/world as the parent
 * `ra8_cgc.c`.
 *
 *
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
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"
#include "ra8_time_constants.h"

static const char* s_tag = "CGC";

typedef enum : uint8_t {
  k_ra8_eswckdivcr_div1  = 0U, /**< /1                                          */
  k_ra8_eswckdivcr_div2  = 1U, /**< /2 -- PLL1P/2 = 500 MHz (ESWPHYCLK target). */
  k_ra8_eswckdivcr_div4  = 2U, /**< /4 -- PLL1P/4 = 250 MHz (ESWCLK target).    */
  k_ra8_eswckdivcr_div8  = 3U, /**< /8                                          */
  k_ra8_eswckdivcr_div16 = 4U, /**< /16                                         */
  k_ra8_eswckdivcr_div32 = 5U, /**< /32                                         */
  k_ra8_eswckdivcr_div64 = 6U, /**< /64                                         */
} ra8_cgc_eswckdivcr_t;

/**
 * @enum ra8_cgc_eswclk_const_t
 * @brief Constants used by the ESWCLK bring-up path.
 *
 * @details
 * ESWCKDIVCR.CKDIV is a 4-bit field. Code 0 = /1, code 1 = /2, ...
 * code 6 = /64 (the FSP encoding table). 125 MHz = PLL1P (1 GHz) / 8;
 * the matching code is ::k_ra8_eswckdivcr_div8 (= 3 in the standard
 * FSP "log2 ratio" map).
 */
typedef enum : uint32_t {
  k_ra8_cgc_eswclk_target_hz    = 250000000UL, /**< ESWCLK    = PLL1P / 4. */
  k_ra8_cgc_eswphyclk_target_hz = 500000000UL, /**< ESWPHYCLK = PLL1P / 2. */
  k_ra8_cgc_eswclk_poll_limit   = 200000U,     /**< CKSRDY spin budget.    */
} ra8_cgc_eswclk_const_t;

/**
 * @var s_eswclk_hz
 * @brief Live ESWCLK frequency, published by ::ra8_cgc_eswclk_init.
 *
 * @details
 * Set to ::k_ra8_cgc_eswclk_target_hz on a successful bring-up; remains
 * 0 until then. Callers query the value via ::ra8_cgc_eswclk_hz so
 * ``ra8_rmac_init`` can fill ``ra8_rmac_config_t.eswclk_hz`` without
 * the application having to know the PLL configuration.
 *
 * @note Mutated only under PRCR-CGC unlock from ::ra8_cgc_eswclk_init.
 * @since 0.1.0
 */
static uint32_t s_eswclk_hz = 0U;

/**
 * @brief Spin-wait for a peripheral clock control register's CKSRDY bit
 *        to match ``expected``.
 *
 * @details
 * Both ESWCKCR and ESWPCKCR share the same CKSRDY layout (bit 7), so
 * one helper drains the SREQ/SRDY handshake for both peripherals.
 *
 * @param[in] ckcr_reg  Pointer to the 8-bit peripheral clock control reg
 *                      (ESWCKCR or ESWPCKCR; both have an identical
 *                      CKSRDY layout at bit ::k_ra8_eswckcr_bit_srdy).
 * @param[in] expected  0 or 1.
 *
 * @return ::k_ra8_ok if CKSRDY reaches the expected level; otherwise
 *         ::k_ra8_err_hw_timeout.
 * @retval k_ra8_ok             CKSRDY observed at the expected level.
 * @retval k_ra8_err_hw_timeout Poll budget exhausted.
 *
 * @pre Caller holds the PRCR-CGC unlock window if this is run during
 *      a clock switch.
 * @pre ``ckcr_reg`` points to a live ESWCKCR / ESWPCKCR register.
 * @post No state changes other than the implicit MMIO read.
 * @post The poll count never exceeds ::k_ra8_cgc_eswclk_poll_limit.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_cksrdy(volatile uint8_t* ckcr_reg, uint8_t expected)
{
  const uint8_t mask = (uint8_t)(1U << k_ra8_eswckcr_bit_srdy);
  /* Bounded wait through ra8_hw_err.h: on host tests the ra8_fake_mmio
   * seam decides the poll (first-poll success unless a test arms a
   * fault), so the real timeout leg is reachable everywhere. */
  if (expected != 0U) {
    return ra8_hw_wait_flag_set8(ckcr_reg, mask, (uint32_t)k_ra8_cgc_eswclk_poll_limit);
  }
  return ra8_hw_wait_flag_clear8(ckcr_reg, mask, (uint32_t)k_ra8_cgc_eswclk_poll_limit);
}

/**
 * @brief Switch one ESW-family peripheral clock to PLL1P with the given
 *        divider code.
 *
 * @details
 * Both ESWCKCR (ESWM core clock) and ESWPCKCR (Ethernet-PHY interface
 * clock) share the same SREQ/SRDY handshake protocol -- the canonical
 * FSP ``bsp_peripheral_clock_set`` sequence. Factoring this helper
 * keeps ``ra8_cgc_eswclk_init`` short and lets the same logic configure
 * both clocks without duplicating six MMIO writes per peripheral.
 *
 * @param[in] ckcr_reg  Pointer to the 8-bit CKCR (source-select) reg.
 * @param[in] divcr_reg Pointer to the 8-bit CKDIVCR (divider) reg.
 * @param[in] div_code  Divider code from ::ra8_cgc_eswckdivcr_t.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok            Clock switched.
 * @retval k_ra8_err_hw_timeout CKSRDY handshake exhausted spin budget.
 *
 * @pre PRCR-CGC is unlocked (caller holds the RA8_PROTECTED_WRITE window).
 * @pre HOCO is running (the SREQ->SRDY synchronizer chain crosses into
 *      the CURRENT source domain, which is HOCO at reset).
 *
 * @post On k_ra8_ok, the targeted clock is sourced from PLL1P with the
 *       requested divider; both SREQ and SRDY read 0.
 * @post On failure CKSREQ may be left asserted; caller must reset the
 *       handshake before retrying.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_switch_eswcr_to_pll1p(volatile uint8_t* ckcr_reg,
                                                volatile uint8_t* divcr_reg,
                                                uint8_t           div_code)
{
  const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_eswckcr_bit_sreq);
  const uint8_t srdy_mask = (uint8_t)(1U << k_ra8_eswckcr_bit_srdy);
  /* FSP `bsp_clocks.c` ::bsp_peripheral_clock_set sequence:
   *   1. Set SREQ=1 (preserve current SEL via read-modify-write).
   *   2. Spin-wait SRDY=1.
   *   3. Programme CKDIVCR (peripheral clock now stopped).
   *   4. Write `source | SREQ | SRDY`. Writing SRDY in the same word
   *      is mandatory per FSP (bsp_clocks.c line 2896); without it
   *      the subsequent SREQ-clear hangs.
   *   5. Clear SREQ -> peripheral clock starts.
   *   6. Spin-wait SRDY=0 (start acknowledged).                       */
  *ckcr_reg     = (uint8_t)(*ckcr_reg | sreq_mask);
  ra8_err_t err = internal_wait_cksrdy(ckcr_reg, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  *divcr_reg        = div_code;
  const uint8_t src = (uint8_t)((uint8_t)k_ra8_eswcksel_pll1p & k_ra8_eswckcr_mask_sel);
  *ckcr_reg         = (uint8_t)(src | sreq_mask | srdy_mask);
  *ckcr_reg         = (uint8_t)(*ckcr_reg & (uint8_t)~sreq_mask);
  return internal_wait_cksrdy(ckcr_reg, 0U);
}

/**
 * @brief Wait for a PDCTRESWM status bit to clear within a bounded budget.
 *
 * @details
 * Polls PDCTRESWM at the requested bit position until it reads 0 or
 * the spin budget is exhausted. Used during the ESWM power-domain
 * power-on sequence to confirm PDCSF / PDPGSF have settled.
 *
 * @param[in] bit Bit position (PDCSF or PDPGSF) to poll.
 *
 * @return ::k_ra8_ok if the bit clears, ::k_ra8_err_hw_timeout otherwise.
 * @retval k_ra8_ok            Polled bit observed at 0.
 * @retval k_ra8_err_hw_timeout Poll budget exhausted with bit still 1.
 *
 * @pre Caller is single-threaded init context.
 * @pre ``bit`` is a valid PDCTRESWM read-only flag position.
 * @post No state changes other than the implicit MMIO read.
 * @post The poll count never exceeds ::k_ra8_cgc_eswclk_poll_limit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_pdctreswm_clear(uint8_t bit)
{
  volatile uint8_t* const pd   = ra8_sys_pdctreswm();
  const uint8_t           mask = (uint8_t)(1U << bit);
  /* Bounded wait through ra8_hw_err.h: on host tests the ra8_fake_mmio
   * seam decides the poll, so the stuck-flag timeout leg is testable. */
  return ra8_hw_wait_flag_clear8(pd, mask, (uint32_t)k_ra8_cgc_eswclk_poll_limit);
}

/**
 * @brief Power on the ESWM peripheral domain via PDCTRESWM.
 *
 * @details
 * After power-on reset the ESWM domain is gated -- every per-port
 * RMAC / ETHA register read returns 0 and writes are silently dropped
 * until ``PDCTRESWM.PDDE = 0``. PRCR group 1 (LPC + battery backup)
 * protects this register. Mirrors FSP r_layer3_switch_module_start.
 *
 * @return k_ra8_ok if PDCSF / PDPGSF both clear within the spin budget.
 * @retval k_ra8_err_hw_timeout PDCSF or PDPGSF stuck high after PDDE=0.
 *
 * @pre PRCR-CGC is locked at entry.
 * @pre Single-threaded init context.
 * @post On k_ra8_ok the ESWM peripheral domain is powered.
 * @post PRCR is re-locked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_eswclk_power_on_domain(void)
{
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_lpm)
  {
    volatile uint8_t* const pd = ra8_sys_pdctreswm();
    /* Only kick the power-on path if the domain is currently gated --
     * PDCSF=0 and PDPGSF=1 is the FSP-canonical "power off" signature. */
    const uint8_t state = *pd;
    if (((state & (1U << k_ra8_pdctr_bit_pdcsf)) == 0U) &&
        ((state & (1U << k_ra8_pdctr_bit_pdpgsf)) != 0U)) {
      *pd = (uint8_t)(state & (uint8_t)~(1U << k_ra8_pdctr_bit_pdde));
    }
  }
  /* Wait for power-on outside the PRCR window (the status flags are
   * read-only and not protected). */
  ra8_err_t err = internal_wait_pdctreswm_clear(k_ra8_pdctr_bit_pdcsf);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "eswclk: PDCSF stuck");
    return err;
  }
  err = internal_wait_pdctreswm_clear(k_ra8_pdctr_bit_pdpgsf);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "eswclk: PDPGSF stuck");
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Switch ESWCLK + ESWPHYCLK to PLL1P (250 MHz + 500 MHz).
 *
 * @details
 * Drives the canonical FSP SREQ/SRDY handshake on ESWCKCR
 * (CKSEL=PLL1P, CKDIV=/4 -> 250 MHz) then ESWPCKCR (CKSEL=PLL1P,
 * CKDIV=/2 -> 500 MHz). Matches the EK-RA8D2 FSP example project
 * ethernet_ek_ra8d2_ep.
 *
 * @return k_ra8_ok if both handshakes complete.
 * @retval k_ra8_err_hw_timeout CKSRDY transition stuck on either reg.
 *
 * @pre HOCO is running (caller invoked ``ra8_cgc_ensure_hoco_running_for_usb_ck``).
 * @pre Single-threaded init context.
 * @post On k_ra8_ok ESWCLK = 250 MHz, ESWPHYCLK = 500 MHz.
 * @post PRCR is re-locked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_eswclk_program_cks(void)
{
  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    err = internal_switch_eswcr_to_pll1p(ra8_sys_eswckcr(),
                                         ra8_sys_eswckdivcr(),
                                         (uint8_t)k_ra8_eswckdivcr_div4);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "eswclk: ESWCKCR handshake timeout");
      break;
    }
    err = internal_switch_eswcr_to_pll1p(ra8_sys_eswpckcr(),
                                         ra8_sys_eswpckdivcr(),
                                         (uint8_t)k_ra8_eswckdivcr_div2);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag, "eswclk: ESWPCKCR handshake timeout");
      break;
    }
  }
  return err;
}

ra8_err_t ra8_cgc_eswclk_init(void)
{
  ra8_log_info(s_tag, "eswclk init (PLL1P/4 = 250 MHz, eswphyclk PLL1P/2 = 500 MHz)");

  /* Step 1: ESWCKCR / ESWPCKCR / USBCKCR / USB60CKCR all share the same
   * CKSEL reset-default (HOCO) and the same SREQ -> SRDY handshake
   * plumbing -- HOCO MUST be running before the SREQ is raised. */
  const ra8_err_t hoco_err = ra8_cgc_ensure_hoco_running_for_usb_ck();
  if (hoco_err != k_ra8_ok) {
    ra8_log_error(s_tag, "eswclk: HOCO stabilization timeout");
    return hoco_err;
  }
  /* Step 2: turn on the ESWM peripheral power domain. */
  const ra8_err_t pd_err = internal_eswclk_power_on_domain();
  if (pd_err != k_ra8_ok) {
    return pd_err;
  }
  /* Step 3: release the MSTPC28 ETHPHYCLK module-stop gate. */
  const ra8_err_t ethphy_mst_err = ra8_mstp_enable(k_ra8_mstp_ethphyclk);
  if (ethphy_mst_err != k_ra8_ok) {
    ra8_log_error(s_tag, "eswclk: ethphyclk MSTP release failed");
    return ethphy_mst_err;
  }
  /* Step 4: switch ESWCKCR + ESWPCKCR to PLL1P. */
  const ra8_err_t cks_err = internal_eswclk_program_cks();
  if (cks_err != k_ra8_ok) {
    return cks_err;
  }
  s_eswclk_hz = (uint32_t)k_ra8_cgc_eswclk_target_hz;
  ra8_log_info(s_tag, "eswclk ready");
  return k_ra8_ok;
}

ra8_err_t ra8_cgc_eswclk_hz(uint32_t* out_hz)
{
  if (out_hz == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_hz = s_eswclk_hz;
  return k_ra8_ok;
}

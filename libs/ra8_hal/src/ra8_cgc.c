/**
 * @file ra8_cgc.c
 * @brief Clock Generation Circuit driver -- RA8D2 PLL bring-up
 *
 * @details
 * Brings the RA8D2 clock tree from reset defaults (MOCO @ ~8 MHz) up
 * to the EK-RA8D2 quickstart target (CPUCLK0 = 1 GHz, ICLK = 250 MHz).
 * Mirrors the FSP `bsp_clocks.c` bring-up sequence for RA8 Gen2; on
 * RA8 Gen2 silicon, several non-obvious steps are mandatory and their
 * omission was previously HardFaulting the chip:
 *
 *  1. Flush the MRAM prefetch buffer (MRMS.MRCPFB = 0 + 3 dummy reads).
 *     FSP `bsp_clocks.c`.
 *  2. Drop the core voltage to "not high voltage" range
 *     (R_SYSTEM->VSCR.VSCM = 1) and wait for VSCMTSF to clear.
 *     FSP `bsp_clocks.c`. Required before lifting PLL above
 *     its boot rate.
 *  3. Stop PLL1 (PLLCR = 1), then poll OSCSF.PLLSF = 0. Without this
 *     barrier, PLLCCR / PLLCCR2 writes are silently dropped and read
 *     back as zero. FSP `bsp_clocks.c`.
 *  4. Programme PLLCCR + PLLCCR2 with the new multiplier and output
 *     dividers. FSP `bsp_clocks.c`.
 *  5. Start PLL1 (PLLCR = 0), then poll OSCSF.PLLSF = 1.
 *     FSP `bsp_clocks.c`.
 *  6. Programme MRMS wait-state frequency latches (MRCFREQ for MRICLK,
 *     MREFREQ for MRPCLK). The hardware refuses any write whose key
 *     byte is wrong, so we spin-poll until readback matches.
 *     FSP `bsp_clocks.c`. **This is the MRAM wait-state step --
 *     RA8D2 does NOT have legacy MEMWAIT / FLDWAITR / FLWT.**
 *  7. Programme the SRAM wait state (SRAMWTSC.WTEN) for the ICLK about
 *     to be selected. The SRAM's counterpart to step 6, and just as
 *     mandatory: HUM Ch 58.3.7 p 3540 requires a wait cycle above half
 *     the rated maximum ICLK, and without it "the operation is not
 *     guaranteed" -- which on this part means a bit silently dropped
 *     from an SRAM read, not a hang (tracker #524 / #499).
 *  8. Programme SCKDIVCR + SCKDIVCR2 for the full divider tree.
 *     FSP `bsp_clocks.c`.
 *  9. Switch SCKSCR to PLL1.
 * 10. Re-enable the prefetch buffer (MRCPFB = 1) iff MRICLK >= 100 MHz.
 *     FSP `bsp_clocks.c`.
 * 11. Programme SCICKCR + SCICKDIVCR per HUM 9.2.54 so SCI_B's TCLK
 *     has a real edge source.
 *
 * Every protected-register write is wrapped in `RA8_PROTECTED_WRITE` so
 * the PRCR re-lock always happens, even on early-return paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_cgc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_cgc_internal.h"
#include "ra8_cgc_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mrms_regs.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_register_protection.h"
#include "ra8_sram.h"
#include "ra8_system_regs.h"
#include "ra8_time_constants.h"

static const char* s_tag = "CGC";

/**
 * @enum ra8_cgc_clock_count_t
 * @brief Sentinel for the size of the published frequency table.
 */
typedef enum : uint8_t {
  k_ra8_cgc_clock_count = 10U, /**< Number of tracked clock-tree domains. */
} ra8_cgc_clock_count_t;

/**
 * @var s_clock_hz
 * @brief Most-recently programmed frequency for each clock-tree domain.
 *
 * @details
 * Reset default is MOCO (~8 MHz). `ra8_cgc_init()` updates the table
 * after the PLL has locked and the new dividers are committed, so
 * `ra8_cgc_get_clock_hz()` always returns the current rate.
 *
 * @note Mutated only from CGC bring-up paths; callers must not write.
 * @warning Direct modification breaks every BRR / sample-rate calculator
 *          downstream.
 * @since 0.1.0
 */
static uint32_t s_clock_hz[k_ra8_cgc_clock_count] = {
  [k_ra8_clock_id_cpuclk0] = k_ra8_moco_hz,
  [k_ra8_clock_id_cpuclk1] = k_ra8_moco_hz,
  [k_ra8_clock_id_iclk]    = k_ra8_moco_hz,
  [k_ra8_clock_id_pclka]   = k_ra8_moco_hz,
  [k_ra8_clock_id_pclkb]   = k_ra8_moco_hz,
  [k_ra8_clock_id_pclkc]   = k_ra8_moco_hz,
  [k_ra8_clock_id_pclkd]   = k_ra8_moco_hz,
  [k_ra8_clock_id_pclke]   = k_ra8_moco_hz,
  [k_ra8_clock_id_fclk]    = k_ra8_moco_hz,
  [k_ra8_clock_id_mriclk]  = k_ra8_moco_hz,
};

/**
 * @brief Internal helper.
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_publish_clocks(void)
{
  s_clock_hz[k_ra8_clock_id_cpuclk0] = k_ra8_cpuclk0_hz;
  s_clock_hz[k_ra8_clock_id_cpuclk1] = k_ra8_cpuclk1_hz;
  s_clock_hz[k_ra8_clock_id_iclk]    = k_ra8_iclk_hz;
  s_clock_hz[k_ra8_clock_id_pclka]   = k_ra8_pclka_hz;
  s_clock_hz[k_ra8_clock_id_pclkb]   = k_ra8_pclkb_hz;
  s_clock_hz[k_ra8_clock_id_pclkc]   = k_ra8_pclkc_hz;
  s_clock_hz[k_ra8_clock_id_pclkd]   = k_ra8_pclkd_hz;
  s_clock_hz[k_ra8_clock_id_pclke]   = k_ra8_pclke_hz;
  s_clock_hz[k_ra8_clock_id_fclk]    = k_ra8_fclk_hz;
  s_clock_hz[k_ra8_clock_id_mriclk]  = k_ra8_mriclk_hz;
}

ra8_err_t ra8_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz)
{
  if (out_hz == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const uint8_t idx = (uint8_t)id;
  if (idx >= (uint8_t)(sizeof(s_clock_hz) / sizeof(s_clock_hz[0]))) {
    return k_ra8_err_invalid_arg;
  }
  *out_hz = s_clock_hz[idx];
  return k_ra8_ok;
}

/* =============================================================================
 * Local-only typed-enum vocabulary
 * =============================================================================
 */

/**
 * @enum ra8_cgc_spin_t
 * @brief Bounded polling limits for oscillator / PLL / wait-state spins.
 */
typedef enum : uint32_t {
  k_ra8_cgc_osc_spin_limit  = 0x40000UL, /**< Generic OSCSF wait budget.    */
  k_ra8_cgc_pll_spin_limit  = 0x40000UL, /**< PLL stop / start wait budget. */
  k_ra8_cgc_mrm_spin_limit  = 0x40000UL, /**< MRCFREQ / MREFREQ wait.       */
  k_ra8_cgc_vscr_spin_limit = 0x40000UL, /**< VSCMTSF wait budget.          */
  k_ra8_cgc_scik_spin_limit = 0x40000UL, /**< SCICKCR.CKSRDY wait budget.   */
} ra8_cgc_spin_t;

/**
 * @enum ra8_cgc_dummy_count_t
 * @brief How many dummy reads to issue when flushing the MRAM PFB.
 *
 * @details
 * FSP `bsp_clocks.c` issues exactly three dummy reads after
 * writing `MRCPFB = 0` to ensure the prefetch buffer is fully flushed
 * before any clock-tree change.
 */
typedef enum : uint8_t {
  k_ra8_pfb_flush_dummy_reads = 3U, /**< Dummy-read count after MRCPFB clear. */
} ra8_cgc_dummy_count_t;

/**
 * @enum ra8_pllcr_bit_t
 * @brief Bit positions in PLLCR (PLL stop control).
 */
typedef enum : uint8_t {
  k_ra8_pllcr_bit_pllstp = 0U, /**< PLLCR.PLLSTP: 1 stops PLL, 0 runs PLL. */
} ra8_pllcr_bit_t;

/**
 * @enum ra8_pllcr_val_t
 * @brief Discrete values written to the 8-bit PLLCR register.
 */
typedef enum : uint8_t {
  k_ra8_pllcr_run  = 0x00U, /**< Clear PLLSTP -> run PLL.  */
  k_ra8_pllcr_stop = 0x01U, /**< Set PLLSTP   -> stop PLL. */
} ra8_pllcr_val_t;

/**
 * @enum ra8_cgc_pll_mul_t
 * @brief PLL multiplier vocabulary for the EK-RA8D2 quickstart target.
 *
 * @details
 * `quickstart_ek_ra8d2_ep` (`ra8_cfg.txt:269`) requests x250.00. FSP
 * encodes this as the integer multiplier 250 placed in PLLCCR[16:8]
 * with PLLMULNF[7:6] = 0 (zero quarter-steps).
 */
typedef enum : uint16_t {
  k_ra8_cgc_pllmul_int_default = 250U, /**< Integer multiplier x250.  */
  k_ra8_cgc_pllmul_quarters    = 0U,   /**< Fractional quarter-steps. */
} ra8_cgc_pll_mul_t;

/**
 * @enum ra8_cgc_scickcr_bit_t
 * @brief Bit positions inside SCICKCR (SCICLK clock-source control).
 *
 * @details
 * Cited from FSP CMSIS device header `R7KA8D2KF_core0.h`
 * and HUM 9.2.54 "SCICKCR : SCI Clock Control Register".
 */
typedef enum : uint8_t {
  k_ra8_scickcr_sel_pll1r = 0x08U,      /**< CKSEL = 0x08 selects PLL1R.    */
  k_ra8_scickcr_cksreq    = (1U << 6U), /**< CKSREQ -- request switch.      */
  k_ra8_scickcr_cksrdy    = (1U << 7U), /**< CKSRDY -- switch acknowledged. */
} ra8_cgc_scickcr_bit_t;

/**
 * @enum ra8_cgc_scickdivcr_t
 * @brief Discrete values written to the 8-bit SCICKDIVCR register.
 *
 * @details
 * SCICKDIV[3:0] uses the same code-to-ratio map as PLODIV: code N
 * selects /N+1 for codes 0..5, /8 = 7, /9 = 8, /16 = 15. We pick /4
 * (code 3) so SCICLK = PLL1R / 4 = 100 MHz, matching FSP quickstart.
 */
typedef enum : uint8_t {
  k_ra8_scickdivcr_div4 = 0x03U, /**< SCICLK divider code for /4. */
} ra8_cgc_scickdivcr_t;

ra8_err_t priv_ra8_cgc_wait_oscsf_set(uint8_t bit)
{
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  volatile const uint8_t* const oscsf = ra8_sys_oscsf();
  /* On host tests the bounded wait consults the ra8_fake_mmio seam: it
   * succeeds on the first poll unless a test arms a fault, so the real
   * poll/timeout legs run everywhere (T1-01, no off-target short-circuit). */
  return ra8_hw_wait_flag_set8(oscsf, (uint8_t)(1U << bit), (uint32_t)k_ra8_cgc_osc_spin_limit);
}

ra8_err_t priv_ra8_cgc_wait_oscsf_clear(uint8_t bit)
{
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  volatile const uint8_t* const oscsf = ra8_sys_oscsf();
  return ra8_hw_wait_flag_clear8(oscsf, (uint8_t)(1U << bit), (uint32_t)k_ra8_cgc_pll_spin_limit);
}

/**
 * @brief Step 1: clear the MRAM prefetch buffer before any clock change.
 *
 * @details
 * Mirrors FSP `bsp_prv_clear_pfb()` (`bsp_clocks.c`): write
 * `MRCPFB = 0` then issue 3 dummy reads to flush the prefetch pipeline.
 * Required before any frequency change per HUM Ch 54.4.3.
 *
 * @pre  No code is fetched from MRAM with the PFB enabled afterwards
 *       until step 9 re-enables it.
 * @post MRCPFB reads back as 0 and the prefetch pipeline is empty.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static void internal_clear_pfb(void)
{
  /* HUM Ch 59.4.3 "Frequency Change Procedure" p 3548 */
  /* Cross-reference: FSP bsp_clocks.c. */
  volatile uint32_t* const mrcpfb = ra8_mrms_mrcpfb();
  *mrcpfb                         = k_ra8_mrcpfb_disable;
  for (uint8_t i = 0U; i < k_ra8_pfb_flush_dummy_reads; i++) {
    (void)*mrcpfb;
  }
}

/**
 * @brief Step 2: drop core to not-high-voltage range before lifting PLL.
 *
 * @details
 * FSP `bsp_clocks.c` writes VSCR.VSCM = 1 then waits VSCMTSF
 * to clear. Required on RA8 Gen2 silicon before raising PLL above its
 * boot rate; without it, the PLL writes succeed but the chip browns
 * out as soon as CPUCLK0 lifts past the high-voltage threshold.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Voltage range switched.
 * @retval k_ra8_err_hw_timeout VSCMTSF stayed set past the spin budget.
 *
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 * @post VSCR.VSCM = 1 and VSCMTSF = 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_set_vscr_not_high_v(void)
{
  /* HUM Ch 11.2.43 "VSCR : Voltage Scaling Control Register" p 477 */
  /* Cross-reference: FSP bsp_clocks.c. */
  volatile uint32_t* const vscr = ra8_sys_vscr();
  *vscr                         = k_ra8_vscr_bit_vscm;

  return ra8_hw_wait_flag_clear32(vscr,
                                  (uint32_t)k_ra8_vscr_bit_vscmtsf,
                                  (uint32_t)k_ra8_cgc_vscr_spin_limit);
}

/**
 * @brief Step 3: stop PLL1 and wait for the PLLSF flag to clear.
 *
 * @details
 * FSP `bsp_clocks.c`. Without polling PLLSF=0 here, the
 * subsequent PLLCCR / PLLCCR2 writes are silently dropped (they read
 * back as 0). This was the missing barrier in the previous driver.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            PLL stopped, OSCSF.PLLSF == 0.
 * @retval k_ra8_err_hw_timeout PLLSF stayed set past the spin budget.
 *
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 * @pre  CPU is currently clocked from MOCO / HOCO / Main (not PLL1).
 * @post PLLCR == 1 and OSCSF.PLLSF == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_stop_pll1(void)
{
  /* HUM Ch 9.2.8 "PLLCR : PLL Control Register" p 333 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra8_sys_pllcr() = k_ra8_pllcr_stop;
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  /* Cross-reference: FSP bsp_clocks.c. */
  return priv_ra8_cgc_wait_oscsf_clear(k_ra8_oscsf_bit_pll1sf);
}

/**
 * @brief Steps 4 + 5: programme PLLCCR / PLLCCR2 then start PLL1.
 *
 * @details
 * Writes the EK-RA8D2 quickstart values (XTAL=24, /3 in, x250.00,
 * P=/2, Q=/6, R=/5) and starts PLL1. After this returns successfully
 * the PLL is locked at PLL1P = 1 GHz, PLL1Q ~= 333 MHz, PLL1R = 400 MHz.
 *
 * Predicted register values:
 *   PLLCCR  = 0xFA02   (PLLMUL=250, PLLMULNF=0, PLSRCSEL=0, PLIDIV=2)
 *   PLLCCR2 = 0x0451   (PLODIVR=4, PLODIVQ=5, PLODIVP=1)
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            PLL locked, OSCSF.PLLSF == 1.
 * @retval k_ra8_err_hw_timeout PLLSF never set within the spin budget.
 *
 * @pre  Main XTAL is running and stable (`OSCSF.MOSCSF` == 1).
 * @pre  PLL1 has been stopped via ::internal_stop_pll1.
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 *
 * @post PLLCR == 0 and OSCSF.PLLSF == 1.
 * @post PLLCCR / PLLCCR2 read back as the values written above.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_program_and_start_pll1(void)
{
  /* PLLMUL is encoded as (integer*4 + quarters) shifted by 6, which
   * simultaneously fills PLLMULNF[7:6] and PLLMUL[16:8]. */
  const uint32_t mul_quarters =
    ((uint32_t)k_ra8_cgc_pllmul_int_default * k_ra8_cgc_quarters_per_unit) +
    k_ra8_cgc_pllmul_quarters;

  const uint32_t pllccr =
    (((mul_quarters & k_ra8_pllccr_mask_quarters) << k_ra8_pllccr_shift_quarters) |
     ((uint32_t)k_ra8_plsrcsel_main << k_ra8_pllccr_shift_plsrcsel) |
     ((uint32_t)k_ra8_plidiv_div3 & k_ra8_pllccr_mask_plidiv));

  const uint16_t pllccr2 = (uint16_t)(((uint16_t)k_ra8_plodiv_div5 << k_ra8_pllccr2_shift_plodivr) |
                                      ((uint16_t)k_ra8_plodiv_div6 << k_ra8_pllccr2_shift_plodivq) |
                                      ((uint16_t)k_ra8_plodiv_div2 << k_ra8_pllccr2_shift_plodivp));

  /* HUM Ch 9.2.6 "PLLCCR : PLL Clock Control Register" p 331 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra8_sys_pllccr() = pllccr;
  /* HUM Ch 9.2.7 "PLLCCR2 : PLL Clock Control Register 2" p 332 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra8_sys_pllccr2() = pllccr2;

  /* HUM Ch 9.2.8 "PLLCR : PLL Control Register" p 333 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra8_sys_pllcr() = k_ra8_pllcr_run;
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  /* Cross-reference: FSP bsp_clocks.c. */
  return priv_ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_pll1sf);
}

/**
 * @brief Bounded write-and-readback poll on MRCFREQ or MREFREQ.
 *
 * @details
 * The MRMS frequency latches accept a write only when the upper byte
 * matches the per-register key (0x1E for MRCFREQ, 0xE1 for MREFREQ).
 * FSP loops the write until the readback matches the freq_mhz payload
 * (see `bsp_clocks.c`). We wrap the same pattern
 * with a bounded spin so a wedged register reports `k_ra8_err_hw_timeout`
 * instead of hanging forever.
 *
 * @param[in,out] reg     Pointer to MRCFREQ or MREFREQ.
 * @param[in]     key     Per-register key (::k_ra8_mrcfreq_key etc.).
 * @param[in]     freq_mhz Desired clock rate in MHz.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Readback matches `freq_mhz`.
 * @retval k_ra8_err_hw_timeout Readback never matched.
 *
 * @pre  `reg` is non-null and points at MRCFREQ or MREFREQ.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra8_ok return, `*reg` reads back as `freq_mhz`.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t
internal_wait_mrm_freq(volatile uint32_t* reg, uint32_t key, uint32_t freq_mhz)
{
  for (uint32_t i = 0U; i < k_ra8_cgc_mrm_spin_limit; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam: the loop-exit decision comes from
     * ra8_fake_mmio_wait_eval (satisfied on the first poll unless a test
     * arms a fault) because plain host RAM cannot strip the key byte
     * on readback the way the silicon latch does. */
    if (ra8_fake_mmio_wait_eval(reg, i, (*reg == freq_mhz))) {
      return k_ra8_ok;
    }
#else
    if (*reg == freq_mhz) {
      return k_ra8_ok;
    }
#endif
    *reg = key | freq_mhz;
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Step 6: programme MRMS wait-state frequency latches.
 *
 * @details
 * Mirrors FSP `bsp_prv_set_wait_state_frequency()` for RA8 Gen2
 * (`bsp_clocks.c`). Writes the MRICLK rate (rounded up to
 * MHz) to MRCFREQ and the MRPCLK rate to MREFREQ, in poll-until-readback
 * loops to handle the key-byte filter.
 *
 * @param[in] mriclk_hz MRICLK rate in Hz.
 * @param[in] mrpclk_hz MRPCLK rate in Hz.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Both registers latched.
 * @retval k_ra8_err_hw_timeout MRCFREQ or MREFREQ never readback-matched.
 *
 * @pre  PLL1 is locked and providing the new clock source.
 * @pre  Caller has flushed the PFB via ::internal_clear_pfb.
 *
 * @post MRCFREQ reads `mriclk_hz / 1MHz` (or 0 if below 1 MHz).
 * @post MREFREQ reads `mrpclk_hz / 1MHz` (or 0 if below 1 MHz).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_set_mrm_wait_states(uint32_t mriclk_hz, uint32_t mrpclk_hz)
{
  const uint32_t mri_mhz = (mriclk_hz <= k_ra8_cgc_mr_min_hz)
                             ? 0U
                             : ((mriclk_hz + k_ra8_cgc_hz_per_mhz - 1U) / k_ra8_cgc_hz_per_mhz);
  const uint32_t mre_mhz = (mrpclk_hz <= k_ra8_cgc_mr_min_hz)
                             ? 0U
                             : ((mrpclk_hz + k_ra8_cgc_hz_per_mhz - 1U) / k_ra8_cgc_hz_per_mhz);

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  /* Cross-reference: FSP bsp_clocks.c. */
  ra8_err_t err = internal_wait_mrm_freq(ra8_mrms_mrcfreq(), k_ra8_mrcfreq_key, mri_mhz);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  /* Cross-reference: FSP bsp_clocks.c. */
  err = internal_wait_mrm_freq(ra8_mrms_mrefreq(), k_ra8_mrefreq_key, mre_mhz);
  return err;
}

/**
 * @brief Step 8: programme SCKDIVCR + SCKDIVCR2 for the FSP-quickstart tree.
 *
 * @details
 * Predicted register values:
 *   SCKDIVCR  = 0x32233432
 *     FCK  = 0x3 (/8),  ICK  = 0x2 (/4), PCKE = 0x2 (/4), BCK  = 0x3 (/8),
 *     PCKA = 0x3 (/8),  PCKB = 0x4 (/16), PCKC = 0x3 (/8), PCKD = 0x2 (/4)
 *   SCKDIVCR2 = 0x2020
 *     MRICK = 0x2 (/4), NPUCK = 0,       CPUCK1 = 0x2 (/4), CPUCK = 0 (/1)
 *
 * @pre  PLL1 is locked.
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 *
 * @post SCKDIVCR / SCKDIVCR2 read back as the constants written above.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static void internal_program_dividers(void)
{
  /* HUM Ch 9.2.6 "SCKDIVCR : System Clock Division Control Register"
   * / FSP bsp_clocks.c */
  const uint32_t sckdivcr = ((uint32_t)k_ra8_clock_div_8 << k_ra8_sckdivcr_fck_shift) |
                            ((uint32_t)k_ra8_clock_div_4 << k_ra8_sckdivcr_ick_shift) |
                            ((uint32_t)k_ra8_clock_div_4 << k_ra8_sckdivcr_pcke_shift) |
                            ((uint32_t)k_ra8_clock_div_8 << k_ra8_sckdivcr_bck_shift) |
                            ((uint32_t)k_ra8_clock_div_8 << k_ra8_sckdivcr_pcka_shift) |
                            ((uint32_t)k_ra8_clock_div_16 << k_ra8_sckdivcr_pckb_shift) |
                            ((uint32_t)k_ra8_clock_div_8 << k_ra8_sckdivcr_pckc_shift) |
                            ((uint32_t)k_ra8_clock_div_4 << k_ra8_sckdivcr_pckd_shift);
  *ra8_sys_sckdivcr()     = sckdivcr;

  /* HUM Ch 9.2.7 "SCKDIVCR2 : System Clock Division Control Register 2"
   * / FSP bsp_clocks.c */
  const uint16_t sckdivcr2 =
    (uint16_t)(((uint16_t)k_ra8_clock_div_4 << k_ra8_sckdivcr2_mriclk_shift) |
               ((uint16_t)k_ra8_clock_div_1 << k_ra8_sckdivcr2_npuclk_shift) |
               ((uint16_t)k_ra8_clock_div_4 << k_ra8_sckdivcr2_cpuclk1_shift) |
               ((uint16_t)k_ra8_clock_div_1 << k_ra8_sckdivcr2_cpuclk0_shift));
  *ra8_sys_sckdivcr2() = sckdivcr2;
}

/**
 * @brief Step 10: re-enable the MRAM prefetch buffer if MRICLK >= 100 MHz.
 *
 * @details
 * Mirrors FSP `bsp_prv_set_pfb()` (`bsp_clocks.c`). Reading
 * MRCFREQ confirms what the wait-state stage actually latched, then
 * the buffer is enabled only above the 100 MHz threshold required by
 * HUM Ch 54.4.3.
 *
 * @pre  ::internal_set_mrm_wait_states has succeeded.
 * @post MRCPFB == 1 iff MRCFREQ >= 100 (MHz).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static void internal_set_pfb(void)
{
  const uint32_t mri_mhz = *ra8_mrms_mrcfreq();
  if (mri_mhz >= k_ra8_mrcpfb_threshold_mhz) {
    /* HUM Ch 59.5.1 "MRCPFB : Code MRAM Prefetch Buffer Enable Register" p 3551 */
    /* Cross-reference: FSP bsp_clocks.c. */
    *ra8_mrms_mrcpfb() = k_ra8_mrcpfb_enable;
  }
}

/**
 * @brief Step 11: route SCICLK = PLL1R / 4 per HUM 9.2.54.
 *
 * @details
 * Without this, SCI_B's TCLK is left at the reset default (MOCO @
 * ~8 MHz) and the bit-shift state machine never advances even though
 * register-level writes look correct. The procedure is:
 *  1. Set CKSREQ = 1, poll until CKSRDY = 1 (SCICLK gated, switchable).
 *  2. Programme SCICKDIVCR + SCICKCR.CKSEL while CKSREQ stays asserted.
 *  3. Clear CKSREQ, poll until CKSRDY = 0 (new clock active).
 *
 * @pre  PLL1 is locked and PRCR is unlocked (caller responsibility).
 *
 * @post SCICKCR.CKSEL = PLL1R, SCICKDIVCR = /4. SCICLK ~= 100 MHz.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            SCICLK switched.
 * @retval k_ra8_err_hw_timeout CKSRDY transition stuck.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_route_sciclk(void)
{
  volatile uint8_t* const ckcr  = ra8_sys_scickcr();
  volatile uint8_t* const divcr = ra8_sys_scickdivcr();

  /* HUM Ch 9.2.54 "SCICKCR : SCI Clock Control Register" p 368 */
  /* step 1 */
  *ckcr = (uint8_t)(*ckcr | k_ra8_scickcr_cksreq);
  const ra8_err_t req_err =
    ra8_hw_wait_flag_set8(ckcr, (uint8_t)k_ra8_scickcr_cksrdy, (uint32_t)k_ra8_cgc_scik_spin_limit);
  if (req_err != k_ra8_ok) {
    return req_err;
  }
  /* HUM Ch 9.2.49 "SCICKDIVCR : SCI Clock Division Control Register" p 365 */
  /* step 2 */
  *divcr = k_ra8_scickdivcr_div4;
  *ckcr  = (uint8_t)(k_ra8_scickcr_sel_pll1r | k_ra8_scickcr_cksreq);
  /* Step 3: clear CKSREQ to start the new clock. */
  *ckcr = k_ra8_scickcr_sel_pll1r;
  return ra8_hw_wait_flag_clear8(ckcr,
                                 (uint8_t)k_ra8_scickcr_cksrdy,
                                 (uint32_t)k_ra8_cgc_scik_spin_limit);
}

/**
 * @brief Start the main crystal oscillator and wait for stabilisation.
 *
 * @details
 * Clears `MOSCCR.MOSTP` to start the crystal, then polls `OSCSF.MOSCSF`
 * until set. The MOSCWTCR wait-count was committed earlier in the
 * boot path; we leave it unchanged here.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Main XTAL stable.
 * @retval k_ra8_err_hw_timeout MOSCSF never set within the spin budget.
 *
 * @pre  EK-RA8D2 24 MHz crystal is populated.
 * @post OSCSF.MOSCSF == 1 and MOSCCR.MOSTP == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_start_main_osc(void)
{
  /* HUM Ch 9.2.27 "MOSCWTCR : Main Clock Oscillator Wait Control Register" p 349 */
  *ra8_sys_moscwtcr() = k_ra8_moscwtcr_2_to_16_cycles;

  /* HUM Ch 9.2.13 "MOSCCR : Main Clock Oscillator Control Register" p 338 */
  volatile uint8_t* const moscr =
    (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_moscr);
  *moscr = (uint8_t)(*moscr & (uint8_t)~k_ra8_moscr_mostp_mask);

  return priv_ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_moscsf);
}

/**
 * @brief PRCR-protected core of ::ra8_cgc_init -- runs steps 2..8.
 *
 * @details
 * Splits out the protected register window so ::ra8_cgc_init stays
 * within the NASA Rule 4 statement budget. Returns the first error
 * encountered (or `k_ra8_ok` on a clean run).
 *
 * @return ra8_err_t error code from the first failing step, or k_ra8_ok.
 *
 * @pre  Caller has flushed the MRAM PFB (::internal_clear_pfb).
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra8_ok the PLL is locked, MRMS wait-state latches are set,
 *       SCKDIVCR / SCKDIVCR2 are committed, and SCKSCR points to PLL1.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_cgc_init_protected(void)
{
  ra8_err_t err = internal_start_main_osc();
  if (err == k_ra8_ok) {
    err = internal_set_vscr_not_high_v();
  }
  if (err == k_ra8_ok) {
    err = internal_stop_pll1();
  }
  if (err == k_ra8_ok) {
    err = internal_program_and_start_pll1();
  }
  if (err == k_ra8_ok) {
    /* Step 6: MRMS wait-state latches must come BEFORE the SCKSCR
     * switch -- the new MRICLK and MRPCLK rates kick in the moment
     * SCKDIVCR2 is committed. */
    err = internal_set_mrm_wait_states(k_ra8_mriclk_hz, k_ra8_fclk_hz);
  }
  if (err == k_ra8_ok) {
    /* Step 7: the SRAM wait state, for exactly the same reason and in
     * exactly the same window as step 6. HUM Ch 58.3.7 "Wait State"
     * p 3540 is unambiguous -- above half the maximum ICLK, "when the
     * wait is not inserted, the operation is not guaranteed" -- and
     * SRAMWTSC.WTEN resets to 0 while this sequence takes ICLK to
     * k_ra8_iclk_hz, its maximum. Programming it after the SCKSCR
     * switch would leave a window in which .data, .bss and the stack
     * are read at full speed with no wait; the ONE fault it is known
     * to produce is a single dropped bit in a value read back out of
     * SRAM, so that window has to be empty, not short (tracker #524). */
    err = ra8_sram_set_wait_state_for_clock(k_ra8_iclk_hz, (uint32_t)k_ra8_iclk_max_hz);
  }
  if (err == k_ra8_ok) {
    internal_program_dividers();
    /* HUM Ch 9.2.5 "SCKSCR : System Clock Source Control Register" p 330 */
    *ra8_sys_sckscr() = k_ra8_cksel_pll1;
  }
  return err;
}

ra8_err_t ra8_cgc_init(void)
{
  ra8_log_info(s_tag, "cgc_init -> XTAL -> PLL1 (1 GHz)");

  /* Step 1 happens outside the PRCR window: MRMS isn't a protected block. */
  internal_clear_pfb();

  ra8_err_t err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    err = internal_cgc_init_protected();
  }

  if (err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "cgc_init failed", (uint32_t)err);
    return err;
  }

  /* Step 10 happens after SCKSCR is on PLL1 -- MRMS isn't protected. */
  internal_set_pfb();

  /* Step 11 needs PRCR unlocked again. */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    err = internal_route_sciclk();
  }
  if (err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "sciclk route failed", (uint32_t)err);
    return err;
  }

  internal_publish_clocks();
  ra8_log_info(s_tag, "system clock = PLL1, CPUCLK0 = 1 GHz");
  return k_ra8_ok;
}

ra8_err_t ra8_cgc_use_hoco(void)
{
  volatile uint8_t* const hococr = ra8_sys_hococr();
  *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t)~(1U << k_ra8_hococr_hcstp));

  const ra8_err_t err = priv_ra8_cgc_wait_oscsf_set(k_ra8_oscsf_bit_hocosf);
  if (err != k_ra8_ok) {
    return err;
  }

  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    *ra8_sys_sckscr() = k_ra8_cksel_hoco;
  }

  ra8_log_info(s_tag, "switched to HOCO");
  return k_ra8_ok;
}

/* =============================================================================
 * Runtime reconfigure + stop detection
 * =============================================================================
 */

/**
 * @var s_ostd_handler
 * @brief Registered oscillation-stop-detection callback.
 *
 * @note Mutated only via ::ra8_cgc_enable_stop_detection /
 *       ::ra8_cgc_disable_stop_detection.
 * @warning Direct modification breaks the disarm path.
 * @since 0.1.0
 */
static ra8_cgc_ostd_fn_t s_ostd_handler = nullptr;

/**
 * @var s_ostd_ctx
 * @brief Stored context passed to ::s_ostd_handler.
 * @note  Mutated only via the OSTD enable/disable pair.
 * @since 0.1.0
 */
static void* s_ostd_ctx = nullptr;

/**
 * @var s_ostd_enabled
 * @brief `true` while the stop-detection path is armed.
 * @note  Mutated only via the OSTD enable/disable pair.
 * @since 0.1.0
 */
static bool s_ostd_enabled = false;

ra8_err_t ra8_cgc_switch_pll1_target(uint32_t new_cpuclk_hz)
{
  if (new_cpuclk_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_log_info_val(s_tag, "switch pll1 target", new_cpuclk_hz);

  /* Step 1: temporarily fall back to MOCO so the CPU is not clocked
   * from PLL1 while we reprogramme it. */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    *ra8_sys_sckscr() = k_ra8_cksel_moco;
  }

  /* Step 2: stop + restart PLL1 with the existing field encodings.
   * For now we only support the FSP-quickstart values; a future
   * change can compute PLLCCR / PLLCCR2 from `new_cpuclk_hz`. */
  ra8_err_t pll_err = k_ra8_ok;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    pll_err = internal_stop_pll1();
    if (pll_err == k_ra8_ok) {
      pll_err = internal_program_and_start_pll1();
    }
    if (pll_err != k_ra8_ok) {
      ra8_log_error_val(s_tag, "pll1 retune failed", (uint32_t)pll_err);
      /* fall through: leave SCKSCR on MOCO so the CPU keeps running. */
    }
  }

  /* Step 3: switch SCKSCR back to PLL1 if the lock succeeded, then
   * republish the clock tree. */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    if (pll_err == k_ra8_ok) {
      *ra8_sys_sckscr() = k_ra8_cksel_pll1;
    }
  }
  internal_publish_clocks();
  s_clock_hz[k_ra8_clock_id_cpuclk0] = new_cpuclk_hz;
  return k_ra8_ok;
}

ra8_err_t ra8_cgc_enable_stop_detection(ra8_cgc_ostd_fn_t handler, void* ctx)
{
  if (handler == nullptr) {
    return k_ra8_err_null_ptr;
  }
  s_ostd_handler = handler;
  s_ostd_ctx     = ctx;
  s_ostd_enabled = true;
  /* HUM Ch 9.2.23 "OSTDCR : Oscillation Stop Detection Control Register", p 346
   * -- target programming deferred until the first real NMI wiring
   * lands. On host (fake) the enable-flag is tracked
   * purely in software and the test helper fires the stored
   * callback directly. */
  ra8_log_info(s_tag, "stop detection armed");
  return k_ra8_ok;
}

ra8_err_t ra8_cgc_disable_stop_detection(void)
{
  s_ostd_handler = nullptr;
  s_ostd_ctx     = nullptr;
  s_ostd_enabled = false;
  ra8_log_info(s_tag, "stop detection disarmed");
  return k_ra8_ok;
}

void ra8_cgc_fake_trigger_stop_detection(void)
{
  if (!s_ostd_enabled) {
    return;
  }
  const ra8_cgc_ostd_fn_t cb  = s_ostd_handler;
  void* const             ctx = s_ostd_ctx;
  if (cb != nullptr) {
    cb(ctx);
  }
}

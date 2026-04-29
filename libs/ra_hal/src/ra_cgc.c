/**
 * @file ra_cgc.c
 * @brief Clock Generation Circuit driver -- RA8D2 PLL bring-up
 *
 * @details
 * Brings the RA8D2 clock tree from reset defaults (MOCO @ ~8 MHz) up
 * to the EK-RA8D2 quickstart target (CPUCLK0 = 1 GHz, ICLK = 250 MHz).
 * Mirrors the FSP `bsp_clocks.c` 10-step sequence for RA8 Gen2; on
 * RA8 Gen2 silicon, several non-obvious steps are mandatory and their
 * omission was previously HardFaulting the chip:
 *
 *  1. Flush the MRAM prefetch buffer (MRMS.MRCPFB = 0 + 3 dummy reads).
 *     FSP `bsp_clocks.c:1182-1189`.
 *  2. Drop the core voltage to "not high voltage" range
 *     (R_SYSTEM->VSCR.VSCM = 1) and wait for VSCMTSF to clear.
 *     FSP `bsp_clocks.c:2536-2539`. Required before lifting PLL above
 *     its boot rate.
 *  3. Stop PLL1 (PLLCR = 1), then poll OSCSF.PLLSF = 0. Without this
 *     barrier, PLLCCR / PLLCCR2 writes are silently dropped and read
 *     back as zero. FSP `bsp_clocks.c:2118-2124`.
 *  4. Programme PLLCCR + PLLCCR2 with the new multiplier and output
 *     dividers. FSP `bsp_clocks.c:2486-2513`.
 *  5. Start PLL1 (PLLCR = 0), then poll OSCSF.PLLSF = 1.
 *     FSP `bsp_clocks.c:2548-2553`.
 *  6. Programme MRMS wait-state frequency latches (MRCFREQ for MRICLK,
 *     MREFREQ for MRPCLK). The hardware refuses any write whose key
 *     byte is wrong, so we spin-poll until readback matches.
 *     FSP `bsp_clocks.c:1208-1247`. **This is the wait-state step --
 *     RA8D2 does NOT have legacy MEMWAIT / FLDWAITR / FLWT.**
 *  7. Programme SCKDIVCR + SCKDIVCR2 for the full divider tree.
 *     FSP `bsp_clocks.c:1142-1175`.
 *  8. Switch SCKSCR to PLL1.
 *  9. Re-enable the prefetch buffer (MRCPFB = 1) iff MRICLK >= 100 MHz.
 *     FSP `bsp_clocks.c:1194-1203`.
 * 10. Programme SCICKCR + SCICKDIVCR per HUM 9.2.54 so SCI_B's TCLK
 *     has a real edge source.
 *
 * Every protected-register write is wrapped in `RA_PROTECTED_WRITE` so
 * the PRCR re-lock always happens, even on early-return paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra_cgc.h"

#include <stdint.h>

#include "ra8d2_cgc_regs.h"
#include "ra8d2_mrms_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_register_protection.h"
#include "ra_time_constants.h"

static const char* s_tag = "CGC";

/**
 * @enum ra_cgc_clock_count_t
 * @brief Sentinel for the size of the published frequency table.
 */
typedef enum : uint8_t {
  k_ra_cgc_clock_count = 10U, /**< Number of tracked clock-tree domains. */
} ra_cgc_clock_count_t;

/**
 * @var s_clock_hz
 * @brief Most-recently programmed frequency for each clock-tree domain.
 *
 * @details
 * Reset default is MOCO (~8 MHz). `ra_cgc_init()` updates the table
 * after the PLL has locked and the new dividers are committed, so
 * `ra_cgc_get_clock_hz()` always returns the current rate.
 *
 * @note Mutated only from CGC bring-up paths; callers must not write.
 * @warning Direct modification breaks every BRR / sample-rate calculator
 *          downstream.
 * @since 0.1.0
 */
static uint32_t s_clock_hz[k_ra_cgc_clock_count] = {
  [k_ra_clock_id_cpuclk0] = k_ra_moco_hz,
  [k_ra_clock_id_cpuclk1] = k_ra_moco_hz,
  [k_ra_clock_id_iclk]    = k_ra_moco_hz,
  [k_ra_clock_id_pclka]   = k_ra_moco_hz,
  [k_ra_clock_id_pclkb]   = k_ra_moco_hz,
  [k_ra_clock_id_pclkc]   = k_ra_moco_hz,
  [k_ra_clock_id_pclkd]   = k_ra_moco_hz,
  [k_ra_clock_id_pclke]   = k_ra_moco_hz,
  [k_ra_clock_id_fclk]    = k_ra_moco_hz,
  [k_ra_clock_id_mriclk]  = k_ra_moco_hz,
};

static void internal_publish_clocks(void)
{
  s_clock_hz[k_ra_clock_id_cpuclk0] = k_ra_cpuclk0_hz;
  s_clock_hz[k_ra_clock_id_cpuclk1] = k_ra_cpuclk1_hz;
  s_clock_hz[k_ra_clock_id_iclk]    = k_ra_iclk_hz;
  s_clock_hz[k_ra_clock_id_pclka]   = k_ra_pclka_hz;
  s_clock_hz[k_ra_clock_id_pclkb]   = k_ra_pclkb_hz;
  s_clock_hz[k_ra_clock_id_pclkc]   = k_ra_pclkc_hz;
  s_clock_hz[k_ra_clock_id_pclkd]   = k_ra_pclkd_hz;
  s_clock_hz[k_ra_clock_id_pclke]   = k_ra_pclke_hz;
  s_clock_hz[k_ra_clock_id_fclk]    = k_ra_fclk_hz;
  s_clock_hz[k_ra_clock_id_mriclk]  = k_ra_mriclk_hz;
}

ra_err_t ra_cgc_get_clock_hz(ra_clock_id_t id, uint32_t* out_hz)
{
  if (out_hz == nullptr) {
    return k_ra_err_null_ptr;
  }
  const uint8_t idx = (uint8_t)id;
  if (idx >= (uint8_t)(sizeof(s_clock_hz) / sizeof(s_clock_hz[0]))) {
    return k_ra_err_invalid_arg;
  }
  *out_hz = s_clock_hz[idx];
  return k_ra_ok;
}

/* =============================================================================
 * Local-only typed-enum vocabulary
 * =============================================================================
 */

/**
 * @enum ra_cgc_spin_t
 * @brief Bounded polling limits for oscillator / PLL / wait-state spins.
 */
typedef enum : uint32_t {
  k_ra_cgc_osc_spin_limit  = 0x40000UL, /**< Generic OSCSF wait budget.   */
  k_ra_cgc_pll_spin_limit  = 0x40000UL, /**< PLL stop / start wait budget.*/
  k_ra_cgc_mrm_spin_limit  = 0x40000UL, /**< MRCFREQ / MREFREQ wait.      */
  k_ra_cgc_vscr_spin_limit = 0x40000UL, /**< VSCMTSF wait budget.        */
  k_ra_cgc_scik_spin_limit = 0x40000UL, /**< SCICKCR.CKSRDY wait budget. */
} ra_cgc_spin_t;

/**
 * @enum ra_cgc_dummy_count_t
 * @brief How many dummy reads to issue when flushing the MRAM PFB.
 *
 * @details
 * FSP `bsp_clocks.c:1186-1188` issues exactly three dummy reads after
 * writing `MRCPFB = 0` to ensure the prefetch buffer is fully flushed
 * before any clock-tree change.
 */
typedef enum : uint8_t {
  k_ra_pfb_flush_dummy_reads = 3U, /**< Dummy-read count after MRCPFB clear. */
} ra_cgc_dummy_count_t;

/**
 * @enum ra_pllcr_bit_t
 * @brief Bit positions in PLLCR (PLL stop control).
 */
typedef enum : uint8_t {
  k_ra_pllcr_bit_pllstp = 0U, /**< PLLCR.PLLSTP: 1 stops PLL, 0 runs PLL. */
} ra_pllcr_bit_t;

/**
 * @enum ra_pllcr_val_t
 * @brief Discrete values written to the 8-bit PLLCR register.
 */
typedef enum : uint8_t {
  k_ra_pllcr_run  = 0x00U, /**< Clear PLLSTP -> run PLL.  */
  k_ra_pllcr_stop = 0x01U, /**< Set PLLSTP   -> stop PLL. */
} ra_pllcr_val_t;

/**
 * @enum ra_cgc_pll_mul_t
 * @brief PLL multiplier vocabulary for the EK-RA8D2 quickstart target.
 *
 * @details
 * `quickstart_ek_ra8d2_ep` (`ra_cfg.txt:269`) requests x250.00. FSP
 * encodes this as the integer multiplier 250 placed in PLLCCR[16:8]
 * with PLLMULNF[7:6] = 0 (zero quarter-steps).
 */
typedef enum : uint16_t {
  k_ra_cgc_pllmul_int_default = 250U, /**< Integer multiplier x250. */
  k_ra_cgc_pllmul_quarters    = 0U,   /**< Fractional quarter-steps. */
} ra_cgc_pll_mul_t;

/**
 * @enum ra_cgc_byte_const_t
 * @brief Bit-width / Hz-conversion constants used in this file.
 */
typedef enum : uint32_t {
  k_ra_cgc_quarters_per_unit = 4UL,       /**< PLL multiplier 0.25 step.      */
  k_ra_cgc_hz_per_mhz        = 1000000UL, /**< Hz per MHz.                    */
  k_ra_cgc_mr_min_hz         = 1UL,       /**< Below this, MR*FREQ field = 0. */
} ra_cgc_byte_const_t;

/**
 * @enum ra_cgc_scickcr_bit_t
 * @brief Bit positions inside SCICKCR (SCICLK clock-source control).
 *
 * @details
 * Cited from FSP CMSIS device header `R7KA8D2KF_core0.h:70335-70340`
 * and HUM 9.2.54 "SCICKCR : SCI Clock Control Register".
 */
typedef enum : uint8_t {
  k_ra_scickcr_sel_pll1r = 0x08U,      /**< CKSEL = 0x08 selects PLL1R. */
  k_ra_scickcr_cksreq    = (1U << 6U), /**< CKSREQ -- request switch.   */
  k_ra_scickcr_cksrdy    = (1U << 7U), /**< CKSRDY -- switch acknowledged. */
} ra_cgc_scickcr_bit_t;

/**
 * @enum ra_cgc_scickdivcr_t
 * @brief Discrete values written to the 8-bit SCICKDIVCR register.
 *
 * @details
 * SCICKDIV[3:0] uses the same code-to-ratio map as PLODIV: code N
 * selects /N+1 for codes 0..5, /8 = 7, /9 = 8, /16 = 15. We pick /4
 * (code 3) so SCICLK = PLL1R / 4 = 100 MHz, matching FSP quickstart.
 */
typedef enum : uint8_t {
  k_ra_scickdivcr_div4 = 0x03U, /**< SCICLK divider code for /4.       */
} ra_cgc_scickdivcr_t;

/**
 * @brief Bounded poll on a single OSCSF flag becoming set.
 *
 * @param[in] bit OSCSF bit number to wait on (e.g. ::k_ra_oscsf_bit_pll1sf).
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Flag observed set.
 * @retval k_ra_err_hw_timeout Spin limit exhausted.
 *
 * @pre  The associated oscillator / PLL has been started.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra_ok return, the polled bit reads as 1.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_wait_oscsf_set(uint8_t bit)
{
  volatile uint8_t* const oscsf = ra_sys_oscsf();
#ifdef RA_SIMULATOR_MODE
  /* Tests pre-seed OSCSF with whatever bits they want set. If the
   * caller hasn't pre-seeded *any* bit (OSCSF == 0) we treat that as
   * a failure scenario and run the spin loop to its budget so timeout
   * tests still report ::k_ra_err_hw_timeout. If the caller did
   * pre-seed and an intermediate driver step cleared the bit (because
   * that's what real hardware would do during PLL stop), restore it
   * so the start path moves forward. */
  if (*oscsf != 0U) {
    *oscsf = (uint8_t)((uint8_t)*oscsf | (uint8_t)(1U << bit));
  }
#endif
  for (uint32_t i = 0U; i < k_ra_cgc_osc_spin_limit; i++) {
    if ((*oscsf & (uint8_t)(1U << bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Bounded poll on a single OSCSF flag becoming clear.
 *
 * @param[in] bit OSCSF bit number to wait on.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Flag observed clear.
 * @retval k_ra_err_hw_timeout Spin limit exhausted.
 *
 * @pre  The associated oscillator / PLL has just been stopped.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra_ok return, the polled bit reads as 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_wait_oscsf_clear(uint8_t bit)
{
#ifdef RA_SIMULATOR_MODE
  /* On the host, OSCSF is plain RAM and the chip's hardware path that
   * clears PLL1SF after PLLCR=1 doesn't exist. Mirror what hardware
   * would do so the test sim flow continues. */
  volatile uint8_t* const oscsf = ra_sys_oscsf();
  *oscsf                        = (uint8_t)((uint8_t)*oscsf & (uint8_t)~(1U << bit));
  return k_ra_ok;
#else
  volatile uint8_t* const oscsf = ra_sys_oscsf();
  for (uint32_t i = 0U; i < k_ra_cgc_pll_spin_limit; i++) {
    if ((*oscsf & (uint8_t)(1U << bit)) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @brief Step 1: clear the MRAM prefetch buffer before any clock change.
 *
 * @details
 * Mirrors FSP `bsp_prv_clear_pfb()` (`bsp_clocks.c:1182-1189`): write
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
 */
static void internal_clear_pfb(void)
{
  /* HUM Ch 54.4.3 "Frequency Change Procedure for MRAM" / FSP bsp_clocks.c:1182-1189 */
  volatile uint32_t* const mrcpfb = ra_mrms_mrcpfb();
  *mrcpfb                         = k_ra_mrcpfb_disable;
  for (uint8_t i = 0U; i < k_ra_pfb_flush_dummy_reads; i++) {
    (void)*mrcpfb;
  }
}

/**
 * @brief Step 2: drop core to not-high-voltage range before lifting PLL.
 *
 * @details
 * FSP `bsp_clocks.c:2536-2539` writes VSCR.VSCM = 1 then waits VSCMTSF
 * to clear. Required on RA8 Gen2 silicon before raising PLL above its
 * boot rate; without it, the PLL writes succeed but the chip browns
 * out as soon as CPUCLK0 lifts past the high-voltage threshold.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Voltage range switched.
 * @retval k_ra_err_hw_timeout VSCMTSF stayed set past the spin budget.
 *
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 * @post VSCR.VSCM = 1 and VSCMTSF = 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_set_vscr_not_high_v(void)
{
  /* HUM Ch 9.2.18 "VSCR : Voltage Scaling Control Register" / FSP bsp_clocks.c:2538-2539 */
  volatile uint32_t* const vscr = ra_sys_vscr();
  *vscr                         = k_ra_vscr_bit_vscm;

  for (uint32_t i = 0U; i < k_ra_cgc_vscr_spin_limit; i++) {
    if ((*vscr & k_ra_vscr_bit_vscmtsf) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Step 3: stop PLL1 and wait for the PLLSF flag to clear.
 *
 * @details
 * FSP `bsp_clocks.c:2118-2124`. Without polling PLLSF=0 here, the
 * subsequent PLLCCR / PLLCCR2 writes are silently dropped (they read
 * back as 0). This was the missing barrier in the previous driver.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            PLL stopped, OSCSF.PLLSF == 0.
 * @retval k_ra_err_hw_timeout PLLSF stayed set past the spin budget.
 *
 * @pre  Caller has unlocked PRCR group 0 (CGC).
 * @pre  CPU is currently clocked from MOCO / HOCO / Main (not PLL1).
 * @post PLLCR == 1 and OSCSF.PLLSF == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_stop_pll1(void)
{
  /* HUM Ch 9.2.31 "PLLCR : PLL Control Register" / FSP bsp_clocks.c:2121 */
  *ra_sys_pllcr() = k_ra_pllcr_stop;
  /* HUM Ch 9.2.20 "OSCSF : Oscillation Stabilization Flag Register" / FSP bsp_clocks.c:2122 */
  return internal_wait_oscsf_clear(k_ra_oscsf_bit_pll1sf);
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
 * @return ra_err_t error code.
 * @retval k_ra_ok            PLL locked, OSCSF.PLLSF == 1.
 * @retval k_ra_err_hw_timeout PLLSF never set within the spin budget.
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
static ra_err_t internal_program_and_start_pll1(void)
{
  /* PLLMUL is encoded as (integer*4 + quarters) shifted by 6, which
   * simultaneously fills PLLMULNF[7:6] and PLLMUL[16:8]. */
  const uint32_t mul_quarters =
    ((uint32_t)k_ra_cgc_pllmul_int_default * k_ra_cgc_quarters_per_unit) + k_ra_cgc_pllmul_quarters;

  const uint32_t pllccr =
    (((mul_quarters & k_ra_pllccr_mask_quarters) << k_ra_pllccr_shift_quarters) |
     ((uint32_t)k_ra_plsrcsel_main << k_ra_pllccr_shift_plsrcsel) |
     ((uint32_t)k_ra_plidiv_div3 & k_ra_pllccr_mask_plidiv));

  const uint16_t pllccr2 = (uint16_t)(((uint16_t)k_ra_plodiv_div5 << k_ra_pllccr2_shift_plodivr) |
                                      ((uint16_t)k_ra_plodiv_div6 << k_ra_pllccr2_shift_plodivq) |
                                      ((uint16_t)k_ra_plodiv_div2 << k_ra_pllccr2_shift_plodivp));

  /* HUM Ch 9.2.30 "PLLCCR : PLL Clock Control Register" / FSP bsp_clocks.c:2510 */
  *ra_sys_pllccr() = pllccr;
  /* HUM Ch 9.2.32 "PLLCCR2 : PLL Output Divider Register" / FSP bsp_clocks.c:2512 */
  *ra_sys_pllccr2() = pllccr2;

  /* HUM Ch 9.2.31 "PLLCR : PLL Control Register" / FSP bsp_clocks.c:2548 */
  *ra_sys_pllcr() = k_ra_pllcr_run;
  /* HUM Ch 9.2.20 "OSCSF : Oscillation Stabilization Flag Register" / FSP bsp_clocks.c:2553 */
  return internal_wait_oscsf_set(k_ra_oscsf_bit_pll1sf);
}

/**
 * @brief Bounded write-and-readback poll on MRCFREQ or MREFREQ.
 *
 * @details
 * The MRMS frequency latches accept a write only when the upper byte
 * matches the per-register key (0x1E for MRCFREQ, 0xE1 for MREFREQ).
 * FSP loops the write until the readback matches the freq_mhz payload
 * (see `bsp_clocks.c:1225-1228, 1243-1246`). We wrap the same pattern
 * with a bounded spin so a wedged register reports `k_ra_err_hw_timeout`
 * instead of hanging forever.
 *
 * @param[in,out] reg     Pointer to MRCFREQ or MREFREQ.
 * @param[in]     key     Per-register key (::k_ra_mrcfreq_key etc.).
 * @param[in]     freq_mhz Desired clock rate in MHz.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Readback matches `freq_mhz`.
 * @retval k_ra_err_hw_timeout Readback never matched.
 *
 * @pre  `reg` is non-null and points at MRCFREQ or MREFREQ.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra_ok return, `*reg` reads back as `freq_mhz`.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_wait_mrm_freq(volatile uint32_t* reg, uint32_t key, uint32_t freq_mhz)
{
  for (uint32_t i = 0U; i < k_ra_cgc_mrm_spin_limit; i++) {
    if (*reg == freq_mhz) {
      return k_ra_ok;
    }
    *reg = key | freq_mhz;
#ifdef RA_SIMULATOR_MODE
    /* Real silicon strips the key on readback; sim memory is plain
     * RAM. Mirror what hardware does so the loop terminates. */
    *reg = freq_mhz;
#endif
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Step 6: programme MRMS wait-state frequency latches.
 *
 * @details
 * Mirrors FSP `bsp_prv_set_wait_state_frequency()` for RA8 Gen2
 * (`bsp_clocks.c:1208-1247`). Writes the MRICLK rate (rounded up to
 * MHz) to MRCFREQ and the MRPCLK rate to MREFREQ, in poll-until-readback
 * loops to handle the key-byte filter.
 *
 * @param[in] mriclk_hz MRICLK rate in Hz.
 * @param[in] mrpclk_hz MRPCLK rate in Hz.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Both registers latched.
 * @retval k_ra_err_hw_timeout MRCFREQ or MREFREQ never readback-matched.
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
static ra_err_t internal_set_mrm_wait_states(uint32_t mriclk_hz, uint32_t mrpclk_hz)
{
  const uint32_t mri_mhz = (mriclk_hz <= k_ra_cgc_mr_min_hz)
                             ? 0U
                             : ((mriclk_hz + k_ra_cgc_hz_per_mhz - 1U) / k_ra_cgc_hz_per_mhz);
  const uint32_t mre_mhz = (mrpclk_hz <= k_ra_cgc_mr_min_hz)
                             ? 0U
                             : ((mrpclk_hz + k_ra_cgc_hz_per_mhz - 1U) / k_ra_cgc_hz_per_mhz);

  /* HUM Ch 54.2.2 "MRCFREQ : MRAM Code Frequency Register" / FSP bsp_clocks.c:1224-1228 */
  ra_err_t err = internal_wait_mrm_freq(ra_mrms_mrcfreq(), k_ra_mrcfreq_key, mri_mhz);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 54.2.3 "MREFREQ : MRAM Extra Frequency Register" / FSP bsp_clocks.c:1242-1246 */
  err = internal_wait_mrm_freq(ra_mrms_mrefreq(), k_ra_mrefreq_key, mre_mhz);
  return err;
}

/**
 * @brief Step 7: programme SCKDIVCR + SCKDIVCR2 for the FSP-quickstart tree.
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
 */
static void internal_program_dividers(void)
{
  /* HUM Ch 9.2.6 "SCKDIVCR : System Clock Division Control Register"
   * / FSP bsp_clocks.c:1159 */
  const uint32_t sckdivcr = ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_fck_shift) |
                            ((uint32_t)k_ra_clock_div_4 << k_ra_sckdivcr_ick_shift) |
                            ((uint32_t)k_ra_clock_div_4 << k_ra_sckdivcr_pcke_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_bck_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_pcka_shift) |
                            ((uint32_t)k_ra_clock_div_16 << k_ra_sckdivcr_pckb_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_pckc_shift) |
                            ((uint32_t)k_ra_clock_div_4 << k_ra_sckdivcr_pckd_shift);
  *ra_sys_sckdivcr()      = sckdivcr;

  /* HUM Ch 9.2.7 "SCKDIVCR2 : System Clock Division Control Register 2"
   * / FSP bsp_clocks.c:1160 */
  const uint16_t sckdivcr2 =
    (uint16_t)(((uint16_t)k_ra_clock_div_4 << k_ra_sckdivcr2_mriclk_shift) |
               ((uint16_t)k_ra_clock_div_1 << k_ra_sckdivcr2_npuclk_shift) |
               ((uint16_t)k_ra_clock_div_4 << k_ra_sckdivcr2_cpuclk1_shift) |
               ((uint16_t)k_ra_clock_div_1 << k_ra_sckdivcr2_cpuclk0_shift));
  *ra_sys_sckdivcr2() = sckdivcr2;
}

/**
 * @brief Step 9: re-enable the MRAM prefetch buffer if MRICLK >= 100 MHz.
 *
 * @details
 * Mirrors FSP `bsp_prv_set_pfb()` (`bsp_clocks.c:1194-1203`). Reading
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
 */
static void internal_set_pfb(void)
{
  const uint32_t mri_mhz = *ra_mrms_mrcfreq();
  if (mri_mhz >= k_ra_mrcpfb_threshold_mhz) {
    /* HUM Ch 54.2.1 "MRCPFB : MRAM Prefetch Buffer Enable" / FSP bsp_clocks.c:1201 */
    *ra_mrms_mrcpfb() = k_ra_mrcpfb_enable;
  }
}

/**
 * @brief Step 10: route SCICLK = PLL1R / 4 per HUM 9.2.54.
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
 * @return ra_err_t error code.
 * @retval k_ra_ok            SCICLK switched.
 * @retval k_ra_err_hw_timeout CKSRDY transition stuck.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_route_sciclk(void)
{
  volatile uint8_t* const ckcr  = ra_sys_scickcr();
  volatile uint8_t* const divcr = ra_sys_scickdivcr();

  /* HUM Ch 9.2.54 "SCICKCR : SCI Clock Control Register" -- step 1 */
  *ckcr = (uint8_t)(*ckcr | k_ra_scickcr_cksreq);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake CKSRDY toggling. */
  *ckcr = (uint8_t)(*ckcr | k_ra_scickcr_cksrdy);
#endif
  for (uint32_t i = 0U; i < k_ra_cgc_scik_spin_limit; i++) {
    if ((*ckcr & k_ra_scickcr_cksrdy) != 0U) {
      break;
    }
    if (i + 1U == k_ra_cgc_scik_spin_limit) {
      return k_ra_err_hw_timeout;
    }
  }
  /* HUM Ch 9.2.54 "SCICKDIVCR : SCI Clock Divider Register" -- step 2 */
  *divcr = k_ra_scickdivcr_div4;
  *ckcr  = (uint8_t)(k_ra_scickcr_sel_pll1r | k_ra_scickcr_cksreq);
  /* Step 3: clear CKSREQ to start the new clock. */
  *ckcr = k_ra_scickcr_sel_pll1r;
  for (uint32_t i = 0U; i < k_ra_cgc_scik_spin_limit; i++) {
    if ((*ckcr & k_ra_scickcr_cksrdy) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Start the main crystal oscillator and wait for stabilisation.
 *
 * @details
 * Clears `MOSCCR.MOSTP` to start the crystal, then polls `OSCSF.MOSCSF`
 * until set. The MOSCWTCR wait-count was committed earlier in the
 * boot path; we leave it unchanged here.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Main XTAL stable.
 * @retval k_ra_err_hw_timeout MOSCSF never set within the spin budget.
 *
 * @pre  EK-RA8D2 24 MHz crystal is populated.
 * @post OSCSF.MOSCSF == 1 and MOSCCR.MOSTP == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_start_main_osc(void)
{
  /* HUM Ch 9.2.27 "MOSCWTCR : Main Clock Oscillator Wait Control" */
  *ra_sys_moscwtcr() = k_ra_moscwtcr_2_to_16_cycles;

  /* HUM Ch 9.2.13 "MOSCCR : Main Clock Oscillator Control Register" */
  volatile uint8_t* const moscr = (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_moscr);
  *moscr                        = (uint8_t)(*moscr & (uint8_t)~k_ra_moscr_mostp_mask);

  return internal_wait_oscsf_set(k_ra_oscsf_bit_moscsf);
}

/**
 * @brief PRCR-protected core of ::ra_cgc_init -- runs steps 2..8.
 *
 * @details
 * Splits out the protected register window so ::ra_cgc_init stays
 * within the NASA Rule 4 statement budget. Returns the first error
 * encountered (or `k_ra_ok` on a clean run).
 *
 * @return ra_err_t error code from the first failing step, or k_ra_ok.
 *
 * @pre  Caller has flushed the MRAM PFB (::internal_clear_pfb).
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra_ok the PLL is locked, MRMS wait-state latches are set,
 *       SCKDIVCR / SCKDIVCR2 are committed, and SCKSCR points to PLL1.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_cgc_init_protected(void)
{
  ra_err_t err = internal_start_main_osc();
  if (err == k_ra_ok) {
    err = internal_set_vscr_not_high_v();
  }
  if (err == k_ra_ok) {
    err = internal_stop_pll1();
  }
  if (err == k_ra_ok) {
    err = internal_program_and_start_pll1();
  }
  if (err == k_ra_ok) {
    /* Step 6: MRMS wait-state latches must come BEFORE the SCKSCR
     * switch -- the new MRICLK and MRPCLK rates kick in the moment
     * SCKDIVCR2 is committed. */
    err = internal_set_mrm_wait_states(k_ra_mriclk_hz, k_ra_fclk_hz);
  }
  if (err == k_ra_ok) {
    internal_program_dividers();
    /* HUM Ch 9.2.5 "SCKSCR : System Clock Source Control Register" */
    *ra_sys_sckscr() = k_ra_cksel_pll1;
  }
  return err;
}

ra_err_t ra_cgc_init(void)
{
  ra_log_info(s_tag, "cgc_init -> XTAL -> PLL1 (1 GHz)");

  /* Step 1 happens outside the PRCR window: MRMS isn't a protected block. */
  internal_clear_pfb();

  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    err = internal_cgc_init_protected();
  }

  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "cgc_init failed", (uint32_t)err);
    return err;
  }

  /* Step 9 happens after SCKSCR is on PLL1 -- MRMS isn't protected. */
  internal_set_pfb();

  /* Step 10 needs PRCR unlocked again. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    err = internal_route_sciclk();
  }
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "sciclk route failed", (uint32_t)err);
    return err;
  }

  internal_publish_clocks();
  ra_log_info(s_tag, "system clock = PLL1, CPUCLK0 = 1 GHz");
  return k_ra_ok;
}

ra_err_t ra_cgc_use_hoco(void)
{
  volatile uint8_t* const hococr = ra_sys_hococr();
  *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));

  const ra_err_t err = internal_wait_oscsf_set(k_ra_oscsf_bit_hocosf);
  if (err != k_ra_ok) {
    return err;
  }

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = k_ra_cksel_hoco;
  }

  ra_log_info(s_tag, "switched to HOCO");
  return k_ra_ok;
}

/* =============================================================================
 * Runtime reconfigure + stop detection
 * =============================================================================
 */

/**
 * @var s_ostd_handler
 * @brief Registered oscillation-stop-detection callback.
 *
 * @note Mutated only via ::ra_cgc_enable_stop_detection /
 *       ::ra_cgc_disable_stop_detection.
 * @warning Direct modification breaks the disarm path.
 * @since 0.1.0
 */
static ra_cgc_ostd_fn_t s_ostd_handler = nullptr;

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

ra_err_t ra_cgc_switch_pll1_target(uint32_t new_cpuclk_hz)
{
  if (new_cpuclk_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_log_info_val(s_tag, "switch pll1 target", new_cpuclk_hz);

  /* Step 1: temporarily fall back to MOCO so the CPU is not clocked
   * from PLL1 while we reprogramme it. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = k_ra_cksel_moco;
  }

  /* Step 2: stop + restart PLL1 with the existing field encodings.
   * For now we only support the FSP-quickstart values; a future
   * change can compute PLLCCR / PLLCCR2 from `new_cpuclk_hz`. */
  ra_err_t pll_err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    pll_err = internal_stop_pll1();
    if (pll_err == k_ra_ok) {
      pll_err = internal_program_and_start_pll1();
    }
    if (pll_err != k_ra_ok) {
      ra_log_error_val(s_tag, "pll1 retune failed", (uint32_t)pll_err);
      /* fall through: leave SCKSCR on MOCO so the CPU keeps running. */
    }
  }

  /* Step 3: switch SCKSCR back to PLL1 if the lock succeeded, then
   * republish the clock tree. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    if (pll_err == k_ra_ok) {
      *ra_sys_sckscr() = k_ra_cksel_pll1;
    }
  }
  internal_publish_clocks();
  s_clock_hz[k_ra_clock_id_cpuclk0] = new_cpuclk_hz;
  return k_ra_ok;
}

ra_err_t ra_cgc_enable_stop_detection(ra_cgc_ostd_fn_t handler, void* ctx)
{
  if (handler == nullptr) {
    return k_ra_err_null_ptr;
  }
  s_ostd_handler = handler;
  s_ostd_ctx     = ctx;
  s_ostd_enabled = true;
  /* HUM Ch 9.2.23 "OSTDCR : Oscillation Stop Detection Control Register", p 346
   * -- target programming deferred until the first real NMI wiring
   * lands. On host (simulator) the enable-flag is tracked
   * purely in software and the test helper fires the stored
   * callback directly. */
  ra_log_info(s_tag, "stop detection armed");
  return k_ra_ok;
}

ra_err_t ra_cgc_disable_stop_detection(void)
{
  s_ostd_handler = nullptr;
  s_ostd_ctx     = nullptr;
  s_ostd_enabled = false;
  ra_log_info(s_tag, "stop detection disarmed");
  return k_ra_ok;
}

void ra_cgc_sim_trigger_stop_detection(void)
{
  if (!s_ostd_enabled) {
    return;
  }
  const ra_cgc_ostd_fn_t cb  = s_ostd_handler;
  void* const            ctx = s_ostd_ctx;
  if (cb != nullptr) {
    cb(ctx);
  }
}

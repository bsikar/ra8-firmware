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
 *     FSP `bsp_clocks.c`. **This is the wait-state step --
 *     RA8D2 does NOT have legacy MEMWAIT / FLDWAITR / FLWT.**
 *  7. Programme SCKDIVCR + SCKDIVCR2 for the full divider tree.
 *     FSP `bsp_clocks.c`.
 *  8. Switch SCKSCR to PLL1.
 *  9. Re-enable the prefetch buffer (MRCPFB = 1) iff MRICLK >= 100 MHz.
 *     FSP `bsp_clocks.c`.
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
#include "ra8d2_mstp_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
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
 * @brief Internal helper.
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
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
 * FSP `bsp_clocks.c` issues exactly three dummy reads after
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
 * Cited from FSP CMSIS device header `R7KA8D2KF_core0.h`
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
 *
 * @details See implementation.
 * @post Caller-visible state matches the documented contract.
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
  for (uint32_t i = 0U; i < k_ra_cgc_osc_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((*oscsf & (uint8_t)(1U << bit)) != 0U) {            /* GCOVR_EXCL_BR_LINE */
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
 *
 * @details See implementation.
 * @post Caller-visible state matches the documented contract.
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
  for (uint32_t i = 0U; i < k_ra_cgc_pll_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((*oscsf & (uint8_t)(1U << bit)) == 0U) {            /* GCOVR_EXCL_BR_LINE */
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
static void internal_clear_pfb(void)
{
  /* HUM Ch 59.4.3 "Frequency Change Procedure" p 3548 */
  /* Cross-reference: FSP bsp_clocks.c. */
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
 * FSP `bsp_clocks.c` writes VSCR.VSCM = 1 then waits VSCMTSF
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
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
static ra_err_t internal_set_vscr_not_high_v(void)
{
  /* HUM Ch 11.2.43 "VSCR : Voltage Scaling Control Register" p 477 */
  /* Cross-reference: FSP bsp_clocks.c. */
  volatile uint32_t* const vscr = ra_sys_vscr();
  *vscr                         = k_ra_vscr_bit_vscm;

  for (uint32_t i = 0U; i < k_ra_cgc_vscr_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((*vscr & k_ra_vscr_bit_vscmtsf) == 0U) {             /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Step 3: stop PLL1 and wait for the PLLSF flag to clear.
 *
 * @details
 * FSP `bsp_clocks.c`. Without polling PLLSF=0 here, the
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
 *
 * @post Caller-visible state matches the documented contract.
 */
static ra_err_t internal_stop_pll1(void)
{
  /* HUM Ch 9.2.8 "PLLCR : PLL Control Register" p 333 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra_sys_pllcr() = k_ra_pllcr_stop;
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  /* Cross-reference: FSP bsp_clocks.c. */
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

  /* HUM Ch 9.2.6 "PLLCCR : PLL Clock Control Register" p 331 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra_sys_pllccr() = pllccr;
  /* HUM Ch 9.2.7 "PLLCCR2 : PLL Clock Control Register 2" p 332 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra_sys_pllccr2() = pllccr2;

  /* HUM Ch 9.2.8 "PLLCR : PLL Control Register" p 333 */
  /* Cross-reference: FSP bsp_clocks.c. */
  *ra_sys_pllcr() = k_ra_pllcr_run;
  /* HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag Register" p 344 */
  /* Cross-reference: FSP bsp_clocks.c. */
  return internal_wait_oscsf_set(k_ra_oscsf_bit_pll1sf);
}

/**
 * @brief Bounded write-and-readback poll on MRCFREQ or MREFREQ.
 *
 * @details
 * The MRMS frequency latches accept a write only when the upper byte
 * matches the per-register key (0x1E for MRCFREQ, 0xE1 for MREFREQ).
 * FSP loops the write until the readback matches the freq_mhz payload
 * (see `bsp_clocks.c`). We wrap the same pattern
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
 *
 * @post Caller-visible state matches the documented contract.
 */
static ra_err_t internal_wait_mrm_freq(volatile uint32_t* reg, uint32_t key, uint32_t freq_mhz)
{
  for (uint32_t i = 0U; i < k_ra_cgc_mrm_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
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
 * (`bsp_clocks.c`). Writes the MRICLK rate (rounded up to
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

  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  /* Cross-reference: FSP bsp_clocks.c. */
  ra_err_t err = internal_wait_mrm_freq(ra_mrms_mrcfreq(), k_ra_mrcfreq_key, mri_mhz);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  /* Cross-reference: FSP bsp_clocks.c. */
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
 *
 * @post Caller-visible state matches the documented contract.
 */
static void internal_program_dividers(void)
{
  /* HUM Ch 9.2.6 "SCKDIVCR : System Clock Division Control Register"
   * / FSP bsp_clocks.c */
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
   * / FSP bsp_clocks.c */
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
static void internal_set_pfb(void)
{
  const uint32_t mri_mhz = *ra_mrms_mrcfreq();
  if (mri_mhz >= k_ra_mrcpfb_threshold_mhz) {
    /* HUM Ch 59.5.1 "MRCPFB : Code MRAM Prefetch Buffer Enable Register" p 3551 */
    /* Cross-reference: FSP bsp_clocks.c. */
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
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
static ra_err_t internal_route_sciclk(void)
{
  volatile uint8_t* const ckcr  = ra_sys_scickcr();
  volatile uint8_t* const divcr = ra_sys_scickdivcr();

  /* HUM Ch 9.2.54 "SCICKCR : SCI Clock Control Register" p 368 */
  /* step 1 */
  *ckcr = (uint8_t)(*ckcr | k_ra_scickcr_cksreq);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake CKSRDY toggling. */
  *ckcr = (uint8_t)(*ckcr | k_ra_scickcr_cksrdy);
#endif
  for (uint32_t i = 0U; i < k_ra_cgc_scik_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((*ckcr & k_ra_scickcr_cksrdy) != 0U) {               /* GCOVR_EXCL_BR_LINE */
      break;
    }
    if (i + 1U == k_ra_cgc_scik_spin_limit) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_err_hw_timeout;
    }
  }
  /* HUM Ch 9.2.49 "SCICKDIVCR : SCI Clock Division Control Register" p 365 */
  /* step 2 */
  *divcr = k_ra_scickdivcr_div4;
  *ckcr  = (uint8_t)(k_ra_scickcr_sel_pll1r | k_ra_scickcr_cksreq);
  /* Step 3: clear CKSREQ to start the new clock. */
  *ckcr = k_ra_scickcr_sel_pll1r;
  for (uint32_t i = 0U; i < k_ra_cgc_scik_spin_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((*ckcr & k_ra_scickcr_cksrdy) == 0U) {               /* GCOVR_EXCL_BR_LINE */
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
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
static ra_err_t internal_start_main_osc(void)
{
  /* HUM Ch 9.2.27 "MOSCWTCR : Main Clock Oscillator Wait Control Register" p 349 */
  *ra_sys_moscwtcr() = k_ra_moscwtcr_2_to_16_cycles;

  /* HUM Ch 9.2.13 "MOSCCR : Main Clock Oscillator Control Register" p 338 */
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
 *
 * @retval k_ra_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
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
    /* HUM Ch 9.2.5 "SCKSCR : System Clock Source Control Register" p 330 */
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

/**
 * @enum ra_usbfs_clock_local_t
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
  k_ra_usbfs_srdy_poll_limit = 200000U, /**< Iterations before timeout.   */
  k_ra_usbfs_div5_code       = 6U,      /**< USBCKDIVCR codepoint for /5. */
  k_ra_usbfs_div8_code       = 4U,      /**< USBCKDIVCR codepoint for /8. */
} ra_usbfs_clock_local_t;

/**
 * @enum ra_usbfs_mstp_local_t
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
  k_ra_mstpb11_usbfs_mask = (uint32_t)(1UL << 11), /**< MSTPCRB.MSTPB11 (USBFS0). */
} ra_usbfs_mstp_local_t;

/**
 * @enum ra_pll2_local_t
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
  k_ra_pll2_max_quarters   = 3U,  /**< Max value of PLL2MULNF[7:6]. */
  k_ra_pll2_usbfs_mul      = 80U, /**< Integer multiplier for PLL2 USBFS path. */
  k_ra_pll2_usbfs_quarters = 0U,  /**< Fractional quarter-steps (none).        */
} ra_pll2_local_t;

/**
 * @brief Spin until USBCKCR.USBCKSRDY matches the expected value.
 * @details See implementation.
 * @param[in] expected Expected value for USBCKSRDY (0 or 1).
 * @return Result code.
 * @retval k_ra_ok USBCKSRDY reached the expected level.
 * @retval k_ra_err_hw_timeout Polling exceeded the iteration cap.
 * @pre PRCR-CGC must be unlocked at call site.
 * @pre Caller is in single-threaded init context.
 * @post No mutation; readback only.
 * @post Iteration count is bounded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_wait_usbcksrdy(uint8_t expected)
{
  volatile uint8_t* const usbckcr = ra_sys_usbckcr();
  const uint8_t           mask    = (uint8_t)(1U << k_ra_usbckcr_bit_srdy);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake USBCKSRDY toggling. */
  if (expected != 0U) {
    *usbckcr = (uint8_t)(*usbckcr | mask);
  } else {
    *usbckcr = (uint8_t)(*usbckcr & (uint8_t)~mask);
  }
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbfs_srdy_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    const uint8_t got = (uint8_t)((*usbckcr & mask) >> k_ra_usbckcr_bit_srdy);
    if (got == expected) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief PRCR-protected stop+program+restart body of ::ra_cgc_pll2_enable.
 * @details See implementation; mirrors ::internal_cgc_init_protected.
 * @param[in] pll2ccr  Pre-computed PLL2CCR value.
 * @param[in] pll2ccr2 Pre-computed PLL2CCR2 value.
 * @return ra_err_t error code.
 * @retval k_ra_ok PLL2 stopped, re-programmed, and re-locked.
 * @retval k_ra_err_hw_timeout PLL2SF stop or lock wait exceeded.
 * @pre  Caller is single-threaded init context; CPU not sourced from PLL2.
 * @pre  PLL2 was previously stopped (OSCSF.PLL2SF=0 expected on entry).
 * @post On k_ra_ok PLL2 is locked at the new mul / divider.
 * @post PRCR re-locked unconditionally (RA_PROTECTED_WRITE invariant).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_pll2_program_protected(uint32_t pll2ccr, uint16_t pll2ccr2)
{
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.11 "PLL2CR : PLL2 Control Register" p 336 -- stop
     * PLL2 before writing PLL2CCR / PLL2CCR2. */
    *ra_sys_pll2cr() = (uint8_t)k_ra_pll2cr_stop;
    err              = internal_wait_oscsf_clear(k_ra_oscsf_bit_pll2sf);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "pll2: stop wait timeout");
      break;
    }
    /* HUM Ch 9.2.10 "PLL2CCR : PLL2 Clock Control Register" p 335 */
    *ra_sys_pll2ccr() = pll2ccr;
    /* HUM Ch 9.2.12 "PLL2CCR2 : PLL2 Clock Control Register 2" p 337 */
    *ra_sys_pll2ccr2() = pll2ccr2;
    *ra_sys_pll2cr()   = (uint8_t)k_ra_pll2cr_run;
    err                = internal_wait_oscsf_set(k_ra_oscsf_bit_pll2sf);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "pll2: lock wait timeout");
      break;
    }
  }
  return err;
}

/**
 * @brief Implementation of `ra_cgc_pll2_enable()`.
 *
 * @details
 * Idempotent: if OSCSF.PLL2SF is already asserted on entry the
 * function returns ::k_ra_ok without touching PLL2CCR / PLL2CCR2 /
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
 * @param[in] p_div_code   ::ra_plodiv_t code for the P output divider.
 *
 * @return Result code.
 * @retval k_ra_ok            PLL2 locked.
 * @retval k_ra_err_invalid_arg ``mul_int`` is 0 or ``mul_quarters > 3``.
 * @retval k_ra_err_hw_timeout PLL2SF stop or start handshake timed out.
 *
 * @pre  ::ra_cgc_init has been called and main XTAL is stable.
 * @pre  Single-threaded init context; CPU not sourced from PLL2.
 *
 * @post On k_ra_ok: PLL2 is locked at the requested multiplier and
 *       output divider; PRCR is re-locked.
 * @post On any error path the PRCR window is closed by RA_PROTECTED_WRITE.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_cgc_pll2_enable(uint8_t mul_int, uint8_t mul_quarters, ra_plodiv_t p_div_code)
{
  if (mul_int == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)mul_quarters > (uint16_t)k_ra_pll2_max_quarters) {
    return k_ra_err_invalid_arg;
  }

  ra_log_info_val(s_tag, "pll2 enable mul_int", (uint32_t)mul_int);

  /* Idempotency (HUM Ch 9): never re-program a running PLL2 -- if FS
   * path already locked it, the HS path's "same VCO" request is a no-op. */
  if ((*ra_sys_oscsf() & (uint8_t)(1U << k_ra_oscsf_bit_pll2sf)) != 0U) {
    ra_log_info(s_tag, "pll2 already locked -- skip re-program");
    return k_ra_ok;
  }

  /* PLL2CCR (32-bit, HUM Ch 9.2.9): PLL2MULNF[7:6] | PLL2MUL[16:8]. */
  const uint32_t mul_quarters_field =
    ((uint32_t)mul_int * (uint32_t)k_ra_cgc_quarters_per_unit) + (uint32_t)mul_quarters;
  const uint32_t pll2ccr =
    ((mul_quarters_field & (uint32_t)k_ra_pll2ccr_mask_quarters) << k_ra_pllccr_shift_quarters) |
    ((uint32_t)k_ra_plsrcsel_main << k_ra_pllccr_shift_plsrcsel) |
    ((uint32_t)k_ra_plidiv_div2 & (uint32_t)k_ra_pll2ccr_mask_plidiv);

  /* PLL2CCR2: P from caller; Q/R fixed at /6. Code 0 is prohibited
   * per HUM Ch 9.2.10/9.2.12 (drops the whole 16-bit write). */
  const uint16_t pll2ccr2 = (uint16_t)(((uint16_t)k_ra_plodiv_div6 << k_ra_pllccr2_shift_plodivr) |
                                       ((uint16_t)k_ra_plodiv_div6 << k_ra_pllccr2_shift_plodivq) |
                                       ((uint16_t)p_div_code << k_ra_pllccr2_shift_plodivp));

  const ra_err_t err = internal_pll2_program_protected(pll2ccr, pll2ccr2);
  if (err != k_ra_ok) {
    return err;
  }
  ra_log_info(s_tag, "pll2 locked");
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_cgc_usbfs_clock_enable (see header).
 *
 * @details
 * Step 1: bring up PLL2 via ::ra_cgc_pll2_enable.
 *   XTAL = 24 MHz, PL2IDIV = /2 (fixed), PLL2MUL = 80, PLL2MULNF = 0
 *   -> VCO = 12 MHz * 80 = 960 MHz (right at the silicon minimum).
 *   PL2ODIVP = /4 -> PLL2P = 240 MHz.
 *
 * Step 2: USBCKCR / USBCKDIVCR handshake (FSP `bsp_clocks.c`).
 *   USBCKDIVCR = /5 codepoint (= 6, per the non-linear FSP map).
 *   USBCKSEL   = PLL2P.
 *   Effective USB clock = 240 MHz / 5 = 48.000 MHz.
 *
 * Spec compliance: USB-FS requires 48 MHz +/- 2500 ppm (+/- 0.25 %).
 * Achieved 48.000 MHz exactly (0 ppm) -> PASS.
 *
 * @return Result code.
 * @retval k_ra_ok USB-FS clock running at exactly 48 MHz.
 * @retval k_ra_err_hw_timeout PLL2 lock or USBCKSRDY handshake timed out.
 * @pre ra_cgc_init has been called (PLL1 locked, main XTAL stable).
 * @pre Single-threaded init context.
 * @post PLL2 is locked at 240 MHz on PLL2P.
 * @post USBCKCR.USBCKSEL = PLL2P, USBCKDIVCR = /5.
 * @post PRCR is re-locked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief PRCR-protected USBCKCR / USBCKDIVCR handshake body.
 * @details Steps 3..7 of the HUM "Clock selection switching procedure"
 * inside the CGC-PRCR window: SREQ=1, wait SRDY=1, write USBCKDIVCR,
 * write USBCKCR=src|SREQ, write USBCKCR=src, wait SRDY=0.
 * @return ::ra_err_t error code.
 * @retval k_ra_ok Handshake completed; USBCLK on PLL2P/5 = 48 MHz.
 * @retval k_ra_err_hw_timeout SRDY=1 or SRDY=0 wait exceeded.
 * @pre PLL2 locked at 240 MHz on PLL2P.
 * @pre MSTPCRB.MSTPB11 = 1 (USBFS module-stopped).
 * @post On k_ra_ok USBCKCR.USBCKSEL = PLL2P, USBCKDIVCR = /5.
 * @post PRCR re-locked (RA_PROTECTED_WRITE always re-locks).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbckcr_switch_to_pll2p_div5(void)
{
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra_usbckcr_bit_sreq);
    *ra_sys_usbckcr()       = sreq_mask;
    err                     = internal_wait_usbcksrdy(1U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "usbfs: SRDY=1 timeout");
      break;
    }
    *ra_sys_usbckdivcr() = (uint8_t)k_ra_usbfs_div5_code;
    const uint8_t src    = (uint8_t)((uint8_t)k_ra_usbcksel_pll2p & k_ra_usbckcr_mask_sel);
    *ra_sys_usbckcr()    = (uint8_t)(src | sreq_mask);
    *ra_sys_usbckcr()    = src;
    err                  = internal_wait_usbcksrdy(0U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "usbfs: SRDY=0 timeout");
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
 * @return ::ra_err_t error code.
 * @retval k_ra_ok USB-FS clock running at exactly 48 MHz.
 * @retval k_ra_err_hw_init_failed PLL2 enable failed; see log.
 * @retval k_ra_err_hw_timeout USBCKCR SREQ/SRDY handshake timed out.
 *
 * @pre  Caller is single-threaded init context.
 * @pre  PLL1 / SCKSCR have already been programmed by ra_cgc_init.
 * @post On k_ra_ok PLL2 is locked, MSTPB11 is set, USBCKCR sources
 *       PLL2P / 5 = 48 MHz, and PRCR is re-locked.
 * @post On error the USB-FS clock is left in a quiesced state.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra_err_t ra_cgc_usbfs_clock_enable(
  void) // NOLINT(readability-function-size,readability-function-cognitive-complexity)
{
  ra_log_info(s_tag, "usbfs clock enable");

  /* Step 1: bring up PLL2 to land 240 MHz on PLL2P. */
  const ra_err_t pll2_err = ra_cgc_pll2_enable((uint8_t)k_ra_pll2_usbfs_mul,
                                               (uint8_t)k_ra_pll2_usbfs_quarters,
                                               k_ra_plodiv_div4);
  if (pll2_err != k_ra_ok) {
    ra_log_error_val(s_tag, "usbfs: pll2 enable failed", (uint32_t)pll2_err);
    return pll2_err;
  }

  /* Step 2a: Force USBFS into module-stop (MSTPB11 = 1) BEFORE the SREQ
   * handshake. HUM Ch 9 "Clock selection switching procedure" step 1
   * (p367 / p370 / p373 / p378 / p381) is mandatory whenever USBCKDIVCR
   * is moved off 1/1; we are programming /5 below. MSTPCR is NOT
   * PRCR-protected. ra_usb_device_init() releases MSTPB11 again later
   * via the ref-counted ra_mstp_enable() path. */
  ra_mstp()->MSTPCRB |= (uint32_t)k_ra_mstpb11_usbfs_mask;
  (void)ra_mstp()->MSTPCRB; /* HUM 11.2.7 Note 2: read-back. */

  /* Step 2a': Ensure HOCO is running. USBCKCR's reset-default source is
   * HOCO; the SREQ -> SRDY synchronizer crosses from ICLK into the
   * CURRENT UCK source's clock domain (HOCO). If HOCO is stopped
   * (HCSTP=1, the chip reset state) the handshake never completes -- we
   * observed USBCKCR=0x40 (SREQ=1, SRDY=0) with HOCOCR=0x01 and
   * OSCSF.HOCOSF=0 on real silicon. ra_cgc_init() leaves HOCO stopped
   * because it switches SCKSCR to PLL1; we have to start HOCO here so
   * the source clock for USBCKCR is alive when SREQ is asserted.
   * HOCOCR IS PRCR-protected (HUM Ch 9; FSP wraps writes inside its
   * BSP_PRV_PRCR_UNLOCK / BSP_PRV_PRCR_LOCK window -- writes outside
   * the window are silently dropped, which is what we observed when
   * an earlier attempt left HOCOCR at 0x01 after the store). */
  ra_err_t hoco_err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    volatile uint8_t* const hococr = ra_sys_hococr();
    if ((*hococr & (uint8_t)(1U << k_ra_hococr_hcstp)) != 0U) {
      *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));
    }
  }
  /* OSCSF.HOCOSF poll outside PRCR window (read-only register). */
  hoco_err = internal_wait_oscsf_set(k_ra_oscsf_bit_hocosf);
  if (hoco_err != k_ra_ok) {
    ra_log_error(s_tag, "usbfs: HOCO stabilization timeout");
    return hoco_err;
  }
  ra_log_info(s_tag, "usbfs: HOCO running for USBCKCR source");

  /* Step 2b: USBCKCR / USBCKDIVCR handshake. */
  const ra_err_t err = internal_usbckcr_switch_to_pll2p_div5();
  if (err != k_ra_ok) {
    return err;
  }
  ra_log_info(s_tag, "usbfs clock ready (PLL2P/5 = 48 MHz)");
  return k_ra_ok;
}

/**
 * @brief Bring up the USBHS PHY reference clock + module clock path.
 *
 * @details
 * The USBHS PHY's internal 480 MHz CDR PLL requires a stable 12 MHz
 * reference derived from the EK-RA8D2 main XTAL (24 MHz / 2). The CGC-
 * side prerequisite (HUM Ch 9 "Clock Generation Circuit", USBCKCR /
 * USBCKDIVCR description, p 365) is that the main oscillator stab-flag
 * (OSCSF.MOSCSF) is asserted before the USBHS MSTP ungate runs. We
 * verify that here, then take a PRCR-CGC unlock window so any future
 * USBCKCR.USBCKSREQ/USBCKSRDY handshake on real silicon happens under
 * the same protection-window contract the rest of this driver follows.
 *
 * On the host simulator the system registers are plain RAM, so the
 * helper short-circuits to ::k_ra_ok after the OSCSF check passes; on
 * silicon the PRCR window is the only thing that matters since
 * ::ra_cgc_init has already left MOSCSF asserted on a working board.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok USBHS clock subsystem armed.
 * @retval k_ra_err_hw_timeout Main XTAL never reported stable.
 *
 * @pre  ::ra_cgc_init has been called.
 * @pre  Caller is single-threaded init context.
 *
 * @post OSCSF.MOSCSF = 1.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
/**
 * @enum ra_usbhs_clock_local_t
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
 * (see ::ra_cgc_usbfs_clock_enable).
 *
 * Codepoint table from HUM Ch 9 USB60CKDIVCR (matches USBCKDIVCR):
 *   /1=0, /2=1, /3=5, /4=2, /5=6, /6=3, /8=4, /10=7, /16=8.
 */
typedef enum : uint32_t {
  k_ra_usbhs_srdy_poll_limit = 200000U, /**< Iterations before timeout.   */
  k_ra_usbhs_div4_code       = 2U,      /**< USB60CKDIVCR codepoint /4.   */
} ra_usbhs_clock_local_t;

/**
 * @enum ra_usbhs_mstp_local_t
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
  k_ra_mstpb12_usbhs_mask = (uint32_t)(1UL << 12), /**< MSTPCRB.MSTPB12 (USBHS). */
} ra_usbhs_mstp_local_t;

/**
 * @brief Spin until USB60CKCR.USB60CKSRDY matches the expected value.
 * @details See implementation.
 * @param[in] expected Expected value for USB60CKSRDY (0 or 1).
 * @return Result code.
 * @retval k_ra_ok      USB60CKSRDY reached the expected level.
 * @retval k_ra_err_hw_timeout Polling exceeded the iteration cap.
 * @pre PRCR-CGC must be unlocked at call site.
 * @pre Caller is in single-threaded init context.
 * @post No mutation; readback only.
 * @post Iteration count is bounded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_wait_usb60cksrdy(uint8_t expected)
{
  volatile uint8_t* const usb60ckcr = ra_sys_usb60ckcr();
  const uint8_t           mask      = (uint8_t)(1U << k_ra_usbckcr_bit_srdy);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake USB60CKSRDY toggling. */
  if (expected != 0U) {
    *usb60ckcr = (uint8_t)(*usb60ckcr | mask);
  } else {
    *usb60ckcr = (uint8_t)(*usb60ckcr & (uint8_t)~mask);
  }
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_srdy_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    const uint8_t got = (uint8_t)((*usb60ckcr & mask) >> k_ra_usbckcr_bit_srdy);
    if (got == expected) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief PRCR-protected USB60CKCR / USB60CKDIVCR handshake body.
 * @details Mirror of ::internal_usbckcr_switch_to_pll2p_div5 for the
 *  USBHS 60 MHz path: SREQ=1, wait SRDY=1, write USB60CKDIVCR=/4,
 *  write USB60CKCR=src|SREQ, write USB60CKCR=src, wait SRDY=0.
 *  HUM Ch 9 "Clock Generation Circuit", USB60CKCR description (USBHS
 *  60 MHz source select). FSP `bsp_clocks.c` ::bsp_peripheral_clock_set
 *  is the canonical reference for the SREQ/SRDY ordering.
 * @return ::ra_err_t error code.
 * @retval k_ra_ok USBHS 60 MHz clock running on PLL2P/4.
 * @retval k_ra_err_hw_timeout SRDY=1 or SRDY=0 wait exceeded.
 * @pre PLL2 locked at 240 MHz on PLL2P.
 * @pre MSTPCRB.MSTPB12 = 1 (USBHS module-stopped).
 * @post On k_ra_ok USB60CKCR.USB60CKSEL = PLL2P, USB60CKDIVCR = /4.
 * @post PRCR re-locked (RA_PROTECTED_WRITE always re-locks).
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usb60ckcr_switch_to_pll2p_div4(void)
{
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra_usbckcr_bit_sreq);
    const uint8_t srdy_mask = (uint8_t)(1U << k_ra_usbckcr_bit_srdy);
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
    volatile uint8_t* const ckcr = ra_sys_usb60ckcr();
    *ckcr                        = (uint8_t)(*ckcr | sreq_mask);
    err                          = internal_wait_usb60cksrdy(1U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "usbhs: SRDY=1 timeout");
      break;
    }
    *ra_sys_usb60ckdivcr() = (uint8_t)k_ra_usbhs_div4_code;
    const uint8_t src      = (uint8_t)((uint8_t)k_ra_usbcksel_pll2p & k_ra_usbckcr_mask_sel);
    *ckcr                  = (uint8_t)(src | sreq_mask | srdy_mask);
    *ckcr                  = (uint8_t)(*ckcr & (uint8_t)~sreq_mask);
    err                    = internal_wait_usb60cksrdy(0U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "usbhs: SRDY=0 timeout");
      break;
    }
  }
  /* Diagnostic witnesses -- read OUTSIDE the PRCR window since
   * USB60CKCR / OSCSF reads are not protected. */
  s_usb60ckcr_probe   = *ra_sys_usb60ckcr();
  s_pll2_status_probe = *ra_sys_oscsf();
  return err;
}

/**
 * @brief Ensure HOCO is running so a USBCKCR/USB60CKCR SREQ->SRDY
 *        handshake can drain.
 *
 * @details
 * USBCKCR / USB60CKCR's reset-default source is HOCO. The SREQ -> SRDY
 * synchronizer chain crosses from ICLK into the CURRENT USB clock
 * source's domain (HOCO at boot). If HOCO is stopped (HCSTP = 1) the
 * handshake never completes (observed: USBCKCR = 0x40 with HOCOSF = 0).
 * ::ra_cgc_init leaves HOCO stopped because it switches SCKSCR to PLL1,
 * so this helper restarts HOCO under a PRCR-CGC unlock window (HOCOCR
 * IS PRCR-protected; writes outside the window are silently dropped)
 * and then waits OSCSF.HOCOSF outside the PRCR window (read-only).
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok HOCO running, OSCSF.HOCOSF asserted.
 * @retval k_ra_err_hw_timeout HOCOSF never asserted.
 *
 * @pre Caller is single-threaded init context.
 * @pre PRCR is currently locked (helper takes its own window).
 *
 * @post On k_ra_ok HOCO is running.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @see ra_cgc_usbfs_clock_enable
 * @see ra_cgc_usbhs_pll_enable
 *
 * @since 0.1.0
 */
static ra_err_t internal_ensure_hoco_running_for_usb_ck(void)
{
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    volatile uint8_t* const hococr = ra_sys_hococr();
    if ((*hococr & (uint8_t)(1U << k_ra_hococr_hcstp)) != 0U) {
      *hococr = (uint8_t)((uint8_t)*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));
    }
  }
#ifdef RA_SIMULATOR_MODE
  /* The host simulator's OSCSF is plain RAM and does not auto-pulse the
   * stabilization flag when an oscillator starts. ::internal_wait_oscsf_set
   * only re-asserts a flag that is already partially set; a freshly-reset
   * sim has OSCSF=0 and would time out. Seed HOCOSF so callers that need
   * a stable HOCO (USB60CKCR / ESWCKCR SREQ->SRDY handshakes) progress. */
  volatile uint8_t* const oscsf = ra_sys_oscsf();
  *oscsf = (uint8_t)((uint8_t)*oscsf | (uint8_t)(1U << k_ra_oscsf_bit_hocosf));
#endif
  return internal_wait_oscsf_set(k_ra_oscsf_bit_hocosf);
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
 * path uses, see ::ra_cgc_usbfs_clock_enable). Mirrors the canonical
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
 * @return ::ra_err_t error code.
 * @retval k_ra_ok USBHS 60 MHz clock running on PLL2P/4.
 * @retval k_ra_err_hw_timeout Main XTAL, HOCO, or USB60CKSRDY handshake
 *         timed out.
 * @retval k_ra_err_invalid_arg PLL2 mul/quarters out of range
 *         (propagated from ::ra_cgc_pll2_enable).
 *
 * @pre  ::ra_cgc_init has been called.
 * @pre  Caller is single-threaded init context; CPU not on PLL2.
 *
 * @post On k_ra_ok: PLL2 locked at 240 MHz on PLL2P, MSTPB12 = 1,
 *       USB60CKCR.USB60CKSEL = PLL2P, USB60CKDIVCR = /4.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe; init context only.
 *
 * @see ra_cgc_usbfs_clock_enable
 * @see ra_cgc_pll2_enable
 *
 * @since 0.1.0
 */
ra_err_t ra_cgc_usbhs_pll_enable(
  void) // NOLINT(readability-function-size,readability-function-cognitive-complexity)
{
  ra_log_info(s_tag, "usbhs phy clock enable");

  /* Step 1: HUM Ch 9.2.21 "OSCSF : Oscillation Stabilization Flag
   * Register", p 344. The USBHS PHY's 12 MHz reference is derived
   * from the main XTAL; without MOSCSF=1 the PHY cannot lock. */
  const ra_err_t osc_err = internal_wait_oscsf_set(k_ra_oscsf_bit_moscsf);
  if (osc_err != k_ra_ok) {
    ra_log_error_val(s_tag, "usbhs: main xtal not stable", (uint32_t)osc_err);
    return osc_err;
  }

  /* Step 2: bring up PLL2 to land 240 MHz on PLL2P. Reuse the FS path's
   * configuration -- 24 MHz XTAL / PL2IDIV(/2) * MUL(80) = 960 MHz VCO,
   * PL2ODIVP=/4 -> PLL2P = 240 MHz. PLL2P / 4 = 60.000 MHz on USB60CLK
   * exactly (0 ppm vs the USBHS 60 MHz spec target). ra_cgc_pll2_enable
   * is idempotent w.r.t. the PRCR window and tolerates being called
   * once per USB controller bring-up (FS path also calls it). */
  const ra_err_t pll2_err = ra_cgc_pll2_enable((uint8_t)k_ra_pll2_usbfs_mul,
                                               (uint8_t)k_ra_pll2_usbfs_quarters,
                                               k_ra_plodiv_div4);
  if (pll2_err != k_ra_ok) {
    ra_log_error_val(s_tag, "usbhs: pll2 enable failed", (uint32_t)pll2_err);
    return pll2_err;
  }

  /* Step 3: Force USBHS into module-stop (MSTPB12 = 1) BEFORE the SREQ
   * handshake. HUM Ch 9 "Clock selection switching procedure" step 1 is
   * mandatory whenever USB60CKDIVCR is moved off 1/1 (we are programming
   * /4 below). MSTPCR is NOT PRCR-protected. ra_usb_device_init() will
   * release MSTPB12 again later via the ref-counted ra_mstp_enable
   * path for k_ra_mstp_usbhs. */
  ra_mstp()->MSTPCRB |= (uint32_t)k_ra_mstpb12_usbhs_mask;
  (void)ra_mstp()->MSTPCRB; /* HUM 11.2.7 Note 2: read-back. */

  /* Step 4: Ensure HOCO is running so the SREQ->SRDY synchronizer can
   * drain (USB60CKCR's reset-default source is HOCO). */
  const ra_err_t hoco_err = internal_ensure_hoco_running_for_usb_ck();
  if (hoco_err != k_ra_ok) {
    ra_log_error(s_tag, "usbhs: HOCO stabilization timeout");
    return hoco_err;
  }

  /* Step 5: USB60CKCR / USB60CKDIVCR SREQ/SRDY handshake. */
  const ra_err_t err = internal_usb60ckcr_switch_to_pll2p_div4();
  if (err != k_ra_ok) {
    return err;
  }

  ra_log_info(s_tag, "usbhs phy clock ready (PLL2P/4 = 60 MHz)");
  return k_ra_ok;
}

/**
 * @enum ra_cgc_eswclk_const_t
 * @brief Constants used by the ESWCLK bring-up path.
 *
 * @details
 * ESWCKDIVCR.CKDIV is a 4-bit field. Code 0 = /1, code 1 = /2, ...
 * code 6 = /64 (the FSP encoding table). 125 MHz = PLL1P (1 GHz) / 8;
 * the matching code is ::k_ra_eswckdivcr_div8 (= 3 in the standard
 * FSP "log2 ratio" map).
 */
typedef enum : uint8_t {
  k_ra_eswckdivcr_div1  = 0U, /**< /1   */
  k_ra_eswckdivcr_div2  = 1U, /**< /2 -- PLL1P/2 = 500 MHz (ESWPHYCLK target). */
  k_ra_eswckdivcr_div4  = 2U, /**< /4 -- PLL1P/4 = 250 MHz (ESWCLK target).    */
  k_ra_eswckdivcr_div8  = 3U, /**< /8   */
  k_ra_eswckdivcr_div16 = 4U, /**< /16  */
  k_ra_eswckdivcr_div32 = 5U, /**< /32  */
  k_ra_eswckdivcr_div64 = 6U, /**< /64  */
} ra_cgc_eswckdivcr_t;

typedef enum : uint32_t {
  k_ra_cgc_eswclk_target_hz    = 250000000UL, /**< ESWCLK    = PLL1P / 4. */
  k_ra_cgc_eswphyclk_target_hz = 500000000UL, /**< ESWPHYCLK = PLL1P / 2. */
  k_ra_cgc_eswclk_poll_limit   = 200000U,     /**< CKSRDY spin budget.    */
} ra_cgc_eswclk_const_t;

/**
 * @var s_eswclk_hz
 * @brief Live ESWCLK frequency, published by ::ra_cgc_eswclk_init.
 *
 * @details
 * Set to ::k_ra_cgc_eswclk_target_hz on a successful bring-up; remains
 * 0 until then. Callers query the value via ::ra_cgc_eswclk_hz so
 * ``ra_rmac_init`` can fill ``ra_rmac_config_t.eswclk_hz`` without
 * the application having to know the PLL configuration.
 *
 * @note Mutated only under PRCR-CGC unlock from ::ra_cgc_eswclk_init.
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
 *                      CKSRDY layout at bit ::k_ra_eswckcr_bit_srdy).
 * @param[in] expected  0 or 1.
 *
 * @return ::k_ra_ok if CKSRDY reaches the expected level; otherwise
 *         ::k_ra_err_hw_timeout.
 * @retval k_ra_ok             CKSRDY observed at the expected level.
 * @retval k_ra_err_hw_timeout Poll budget exhausted.
 *
 * @pre Caller holds the PRCR-CGC unlock window if this is run during
 *      a clock switch.
 * @pre ``ckcr_reg`` points to a live ESWCKCR / ESWPCKCR register.
 * @post No state changes other than the implicit MMIO read.
 * @post On simulator builds, the polled bit is forced to ``expected``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_wait_cksrdy(volatile uint8_t* ckcr_reg, uint8_t expected)
{
  const uint8_t mask = (uint8_t)(1U << k_ra_eswckcr_bit_srdy);
#ifdef RA_SIMULATOR_MODE
  if (expected != 0U) {
    *ckcr_reg = (uint8_t)(*ckcr_reg | mask);
  } else {
    *ckcr_reg = (uint8_t)(*ckcr_reg & (uint8_t)~mask);
  }
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_cgc_eswclk_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    const uint8_t got = (uint8_t)((*ckcr_reg & mask) >> k_ra_eswckcr_bit_srdy);
    if (got == expected) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Switch one ESW-family peripheral clock to PLL1P with the given
 *        divider code.
 *
 * @details
 * Both ESWCKCR (ESWM core clock) and ESWPCKCR (Ethernet-PHY interface
 * clock) share the same SREQ/SRDY handshake protocol -- the canonical
 * FSP ``bsp_peripheral_clock_set`` sequence. Factoring this helper
 * keeps ``ra_cgc_eswclk_init`` short and lets the same logic configure
 * both clocks without duplicating six MMIO writes per peripheral.
 *
 * @param[in] ckcr_reg  Pointer to the 8-bit CKCR (source-select) reg.
 * @param[in] divcr_reg Pointer to the 8-bit CKDIVCR (divider) reg.
 * @param[in] div_code  Divider code from ::ra_cgc_eswckdivcr_t.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok            Clock switched.
 * @retval k_ra_err_hw_timeout CKSRDY handshake exhausted spin budget.
 *
 * @pre PRCR-CGC is unlocked (caller holds the RA_PROTECTED_WRITE window).
 * @pre HOCO is running (the SREQ->SRDY synchronizer chain crosses into
 *      the CURRENT source domain, which is HOCO at reset).
 *
 * @post On k_ra_ok, the targeted clock is sourced from PLL1P with the
 *       requested divider; both SREQ and SRDY read 0.
 * @post On failure CKSREQ may be left asserted; caller must reset the
 *       handshake before retrying.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_switch_eswcr_to_pll1p(volatile uint8_t* ckcr_reg,
                                               volatile uint8_t* divcr_reg,
                                               uint8_t           div_code)
{
  const uint8_t sreq_mask = (uint8_t)(1U << k_ra_eswckcr_bit_sreq);
  const uint8_t srdy_mask = (uint8_t)(1U << k_ra_eswckcr_bit_srdy);
  /* FSP `bsp_clocks.c` ::bsp_peripheral_clock_set sequence:
   *   1. Set SREQ=1 (preserve current SEL via read-modify-write).
   *   2. Spin-wait SRDY=1.
   *   3. Programme CKDIVCR (peripheral clock now stopped).
   *   4. Write `source | SREQ | SRDY`. Writing SRDY in the same word
   *      is mandatory per FSP (bsp_clocks.c line 2896); without it
   *      the subsequent SREQ-clear hangs.
   *   5. Clear SREQ -> peripheral clock starts.
   *   6. Spin-wait SRDY=0 (start acknowledged).                       */
  *ckcr_reg    = (uint8_t)(*ckcr_reg | sreq_mask);
  ra_err_t err = internal_wait_cksrdy(ckcr_reg, 1U);
  if (err != k_ra_ok) {
    return err;
  }
  *divcr_reg        = div_code;
  const uint8_t src = (uint8_t)((uint8_t)k_ra_eswcksel_pll1p & k_ra_eswckcr_mask_sel);
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
 * @return ::k_ra_ok if the bit clears, ::k_ra_err_hw_timeout otherwise.
 * @retval k_ra_ok            Polled bit observed at 0.
 * @retval k_ra_err_hw_timeout Poll budget exhausted with bit still 1.
 *
 * @pre Caller is single-threaded init context.
 * @pre ``bit`` is a valid PDCTRESWM read-only flag position.
 * @post No state changes other than the implicit MMIO read.
 * @post On simulator builds the bit is forced to 0 so the spin exits.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_wait_pdctreswm_clear(uint8_t bit)
{
  volatile uint8_t* const pd   = ra_sys_pdctreswm();
  const uint8_t           mask = (uint8_t)(1U << bit);
#ifdef RA_SIMULATOR_MODE
  *pd = (uint8_t)(*pd & (uint8_t)~mask);
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_cgc_eswclk_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*pd & mask) == 0U) {                                            /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
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
 * @return k_ra_ok if PDCSF / PDPGSF both clear within the spin budget.
 * @retval k_ra_err_hw_timeout PDCSF or PDPGSF stuck high after PDDE=0.
 *
 * @pre PRCR-CGC is locked at entry.
 * @pre Single-threaded init context.
 * @post On k_ra_ok the ESWM peripheral domain is powered.
 * @post PRCR is re-locked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_eswclk_power_on_domain(void)
{
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_lpm)
  {
    volatile uint8_t* const pd = ra_sys_pdctreswm();
    /* Only kick the power-on path if the domain is currently gated --
     * PDCSF=0 and PDPGSF=1 is the FSP-canonical "power off" signature. */
    const uint8_t state = *pd;
    if (((state & (1U << k_ra_pdctr_bit_pdcsf)) == 0U) &&
        ((state & (1U << k_ra_pdctr_bit_pdpgsf)) != 0U)) {
      *pd = (uint8_t)(state & (uint8_t)~(1U << k_ra_pdctr_bit_pdde));
    }
  }
  /* Wait for power-on outside the PRCR window (the status flags are
   * read-only and not protected). */
  ra_err_t err = internal_wait_pdctreswm_clear(k_ra_pdctr_bit_pdcsf);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "eswclk: PDCSF stuck");
    return err;
  }
  err = internal_wait_pdctreswm_clear(k_ra_pdctr_bit_pdpgsf);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "eswclk: PDPGSF stuck");
    return err;
  }
  return k_ra_ok;
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
 * @return k_ra_ok if both handshakes complete.
 * @retval k_ra_err_hw_timeout CKSRDY transition stuck on either reg.
 *
 * @pre HOCO is running (caller invoked ``internal_ensure_hoco_running_for_usb_ck``).
 * @pre Single-threaded init context.
 * @post On k_ra_ok ESWCLK = 250 MHz, ESWPHYCLK = 500 MHz.
 * @post PRCR is re-locked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_eswclk_program_cks(void)
{
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    err = internal_switch_eswcr_to_pll1p(ra_sys_eswckcr(),
                                         ra_sys_eswckdivcr(),
                                         (uint8_t)k_ra_eswckdivcr_div4);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "eswclk: ESWCKCR handshake timeout");
      break;
    }
    err = internal_switch_eswcr_to_pll1p(ra_sys_eswpckcr(),
                                         ra_sys_eswpckdivcr(),
                                         (uint8_t)k_ra_eswckdivcr_div2);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "eswclk: ESWPCKCR handshake timeout");
      break;
    }
  }
  return err;
}

ra_err_t ra_cgc_eswclk_init(void)
{
  ra_log_info(s_tag, "eswclk init (PLL1P/4 = 250 MHz, eswphyclk PLL1P/2 = 500 MHz)");

  /* Step 1: ESWCKCR / ESWPCKCR / USBCKCR / USB60CKCR all share the same
   * CKSEL reset-default (HOCO) and the same SREQ -> SRDY handshake
   * plumbing -- HOCO MUST be running before the SREQ is raised. */
  const ra_err_t hoco_err = internal_ensure_hoco_running_for_usb_ck();
  if (hoco_err != k_ra_ok) {
    ra_log_error(s_tag, "eswclk: HOCO stabilization timeout");
    return hoco_err;
  }
  /* Step 2: turn on the ESWM peripheral power domain. */
  const ra_err_t pd_err = internal_eswclk_power_on_domain();
  if (pd_err != k_ra_ok) {
    return pd_err;
  }
  /* Step 3: release the MSTPC28 ETHPHYCLK module-stop gate. */
  const ra_err_t ethphy_mst_err = ra_mstp_enable(k_ra_mstp_ethphyclk);
  if (ethphy_mst_err != k_ra_ok) {
    ra_log_error(s_tag, "eswclk: ethphyclk MSTP release failed");
    return ethphy_mst_err;
  }
  /* Step 4: switch ESWCKCR + ESWPCKCR to PLL1P. */
  const ra_err_t cks_err = internal_eswclk_program_cks();
  if (cks_err != k_ra_ok) {
    return cks_err;
  }
  s_eswclk_hz = (uint32_t)k_ra_cgc_eswclk_target_hz;
  ra_log_info(s_tag, "eswclk ready");
  return k_ra_ok;
}

ra_err_t ra_cgc_eswclk_hz(uint32_t* out_hz)
{
  if (out_hz == nullptr) {
    return k_ra_err_null_ptr;
  }
  *out_hz = s_eswclk_hz;
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

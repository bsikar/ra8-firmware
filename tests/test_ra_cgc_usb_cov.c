/**
 * @file test_ra_cgc_usb_cov.c
 * @brief Coverage-raising tests for ra_cgc_usb.c (PLL2 + USB-FS / USB-HS).
 *
 * @details
 * A dedicated test executable that exercises paths in ra_cgc_usb.c that
 * test_ra_cgc_extended.c leaves uncovered. It is auto-discovered by the
 * GLOB in tests/CMakeLists.txt, linked as its own binary, and its .gcda
 * is merged by gcovr so every covered line adds to repository-wide line
 * coverage.
 *
 * The existing "happy path" tests in test_ra_cgc_extended.c pre-seed
 * OSCSF = 0xFF, which makes ra_cgc_pll2_enable take its idempotency
 * early-return (PLL2SF already set) rather than the full stop / program /
 * lock sequence. This file drives the sequences those tests miss:
 *
 *  - ra_cgc_pll2_enable full program+lock path -- PLL2SF CLEAR on entry but
 *    another OSCSF bit set so the sim wait_oscsf_set() force-sets PLL2SF and
 *    the routine returns k_ra_ok (source lines 213-223, 301-302).
 *  - ra_cgc_usbfs_clock_enable with HOCOCR.HCSTP set -- the inline
 *    HOCO-restart block clears the stop bit (source lines 437-439).
 *  - ra_cgc_usbhs_pll_enable with HOCOCR.HCSTP set -- the shared
 *    ra_cgc_ensure_hoco_running_for_usb_ck helper clears the stop bit
 *    (source lines 644-646).
 *
 * The remaining uncovered lines in ra_cgc_usb.c are the SREQ/SRDY
 * synchronizer-handshake timeout legs and the error returns that only fire
 * when those handshakes fail. In RA_SIMULATOR_MODE the driver's own
 * fake-ack code (and the wait_oscsf_* sim short-circuits in ra_cgc.c)
 * force every handshake to succeed, so those legs are hardware-only and
 * are marked GCOVR_EXCL in the source with per-region justifications.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_cgc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_cgc_usb_cov_bits_t
 * @brief Register seed values used to steer the driver into uncovered legs.
 *
 * @details
 * Tests are exempt from the magic-number gate, but naming these keeps the
 * intent explicit: which OSCSF / HOCOCR bit each seed pokes.
 */
typedef enum : uint8_t {
  k_cov_moscsf_only = (uint8_t)(1U << k_ra_oscsf_bit_moscsf), /**< Main-XTAL stab only.   */
  k_cov_oscsf_all   = 0xFFU,                                  /**< Every OSCSF stab flag. */
  k_cov_hcstp_mask  = (uint8_t)(1U << k_ra_hococr_hcstp),     /**< HOCOCR.HCSTP stop bit. */
  k_cov_pll2sf_mask = (uint8_t)(1U << k_ra_oscsf_bit_pll2sf), /**< OSCSF.PLL2SF flag.     */
} ra_cgc_usb_cov_bits_t;

/* ---------------------------------------------------------------------------
 * ra_cgc_pll2_enable -- full stop / program / lock success path
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify pll2_enable runs the full program+lock path to k_ra_ok.
 *
 * @details
 * OSCSF is seeded with MOSCSF set but PLL2SF CLEAR. The PLL2SF-clear state
 * defeats the idempotency early-return, so ra_cgc_pll2_enable calls
 * internal_pll2_program_protected: it stops PLL2, waits OSCSF.PLL2SF to
 * clear (ra_cgc_wait_oscsf_clear returns k_ra_ok immediately in sim),
 * programmes PLL2CCR/PLL2CCR2, then waits OSCSF.PLL2SF to set. Because at
 * least one OSCSF bit (MOSCSF) is non-zero, the sim wait_oscsf_set()
 * force-sets PLL2SF and returns k_ra_ok. The routine logs "pll2 locked"
 * and returns k_ra_ok. This reaches the program-body and the trailing
 * success return (source lines 213-223, 301-302) that the OSCSF=0xFF
 * happy-path test never touches (it takes the idempotency shortcut).
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre OSCSF = MOSCSF only (PLL2SF clear).
 * @post ra_cgc_pll2_enable returns k_ra_ok.
 * @post OSCSF.PLL2SF is set (the lock wait force-set it).
 *
 * @par MC/DC:
 * Decision: ``(*ra_sys_oscsf() & (1U << k_ra_oscsf_bit_pll2sf)) != 0U``
 * (idempotency guard, one condition).
 * - V1: PLL2SF=1 -> true  -> early k_ra_ok (test_cgc_pll2_enable_already_locked).
 * - V2: PLL2SF=0 -> false -> full program path (this test).
 * Single-condition guard: no compound MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_pll2_enable_full_program_ok(void)
{
  TEST_BEGIN("pll2_enable full program path returns ok");
  ra_sim_mmap_reset();

  /* MOSCSF set, PLL2SF clear: skips idempotency, lets the sim lock wait
   * force-set PLL2SF so the program path completes successfully. */
  *ra_sys_oscsf() = (uint8_t)k_cov_moscsf_only;

  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_pll2_enable(80U, 0U, k_ra_plodiv_div4));

  /* Postcondition witness: the lock wait force-set PLL2SF in OSCSF. */
  TEST_ASSERT((*ra_sys_oscsf() & (uint8_t)k_cov_pll2sf_mask) != 0U);

  TEST_END("pll2_enable full program path returns ok");
}

/* ---------------------------------------------------------------------------
 * ra_cgc_usbfs_clock_enable -- HOCO restart (HCSTP was set)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify usbfs_clock_enable clears HOCOCR.HCSTP when HOCO is stopped.
 *
 * @details
 * The stock happy-path test leaves HOCOCR at its zeroed sim-reset value, so
 * the inline "if HCSTP set, start HOCO" block is skipped. Here HOCOCR.HCSTP
 * is pre-seeded to 1 (HOCO stopped). With OSCSF fully seeded, PLL2 returns
 * via idempotency, the USBFS module-stop pre-step runs, then the HOCO-
 * restart block reads HCSTP=1, enters its body, and clears the stop bit
 * (source lines 437-439). The subsequent OSCSF.HOCOSF wait and the
 * USBCKCR/USBCKDIVCR handshake all succeed in sim, so the routine returns
 * k_ra_ok and HOCOCR.HCSTP reads back 0.
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre OSCSF = 0xFF; HOCOCR.HCSTP = 1.
 * @post ra_cgc_usbfs_clock_enable returns k_ra_ok.
 * @post HOCOCR.HCSTP has been cleared to 0.
 *
 * @par MC/DC:
 * Decision: ``(*hococr & (1U << k_ra_hococr_hcstp)) != 0U`` (one condition).
 * - V1: HCSTP=1 -> true  -> clear the stop bit (this test).
 * - V2: HCSTP=0 -> false -> skip (test_cgc_usbfs_clock_enable_ok).
 * Single-condition guard: no compound MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_usbfs_clock_enable_restarts_hoco(void)
{
  TEST_BEGIN("usbfs_clock_enable clears HCSTP when HOCO stopped");
  ra_sim_mmap_reset();

  *ra_sys_oscsf()  = (uint8_t)k_cov_oscsf_all;
  *ra_sys_hococr() = (uint8_t)k_cov_hcstp_mask; /* HOCO stopped -> exercise restart. */

  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_usbfs_clock_enable());

  /* Postcondition witness: the restart block cleared HCSTP. */
  TEST_ASSERT((*ra_sys_hococr() & (uint8_t)k_cov_hcstp_mask) == 0U);

  TEST_END("usbfs_clock_enable clears HCSTP when HOCO stopped");
}

/* ---------------------------------------------------------------------------
 * ra_cgc_usbhs_pll_enable -- HOCO restart via ensure_hoco helper
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify usbhs_pll_enable clears HOCOCR.HCSTP via ensure_hoco.
 *
 * @details
 * Mirrors the USB-FS restart test for the USB-HS path, which routes the
 * HOCO restart through the shared ra_cgc_ensure_hoco_running_for_usb_ck
 * helper. With OSCSF fully seeded, the main-XTAL wait, the idempotent PLL2
 * enable, and the module-stop pre-step all pass; then ensure_hoco reads
 * HOCOCR.HCSTP=1, enters its body, and clears the stop bit (source lines
 * 644-646). The seeded-HOCOSF sim path and the USB60CKCR handshake then
 * succeed, so the routine returns k_ra_ok and HOCOCR.HCSTP reads back 0.
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre OSCSF = 0xFF; HOCOCR.HCSTP = 1.
 * @post ra_cgc_usbhs_pll_enable returns k_ra_ok.
 * @post HOCOCR.HCSTP has been cleared to 0.
 *
 * @par MC/DC:
 * Decision: ``(*hococr & (1U << k_ra_hococr_hcstp)) != 0U`` inside
 * ra_cgc_ensure_hoco_running_for_usb_ck (one condition).
 * - V1: HCSTP=1 -> true  -> clear the stop bit (this test).
 * - V2: HCSTP=0 -> false -> skip (test_cgc_usbhs_pll_enable_ok).
 * Single-condition guard: no compound MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_usbhs_pll_enable_restarts_hoco(void)
{
  TEST_BEGIN("usbhs_pll_enable clears HCSTP when HOCO stopped");
  ra_sim_mmap_reset();

  *ra_sys_oscsf()  = (uint8_t)k_cov_oscsf_all;
  *ra_sys_hococr() = (uint8_t)k_cov_hcstp_mask; /* HOCO stopped -> exercise restart. */

  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_usbhs_pll_enable());

  /* Postcondition witness: ensure_hoco cleared HCSTP. */
  TEST_ASSERT((*ra_sys_hococr() & (uint8_t)k_cov_hcstp_mask) == 0U);

  TEST_END("usbhs_pll_enable clears HCSTP when HOCO stopped");
}

int32_t main(void)
{
  test_pll2_enable_full_program_ok();
  test_usbfs_clock_enable_restarts_hoco();
  test_usbhs_pll_enable_restarts_hoco();
  (void)fprintf(stderr, "[OK  ] test_ra_cgc_usb_cov.c\n");
  return 0;
}

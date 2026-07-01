/**
 * @file test_ra_usb_host_ctrl_cov2.c
 * @brief Complementary white-box line-coverage tests for the polled
 *        host-mode control-transfer engine (`ra_usb_host_ctrl.c`).
 *
 * @par Tag
 * [Ring 3 / Test] {World: Host}
 *
 * @details
 * This is the structural counterpart to `test_ra_usb_host_ctrl_cov.c`.
 * That sibling redirects the three register transports -- `internal_rmw16`,
 * `internal_wait_frdy`, `internal_dcp_push_chunk` -- through preprocessor
 * renames to deterministic mocks so it can script the SIE interrupt edges
 * (SACK/SIGN/BRDY) the plain-RAM register window cannot raise. Mocking
 * those transports away changes the emitted code of every function that
 * calls them, so in the AGGREGATE gcovr build the mock-diverged copy of
 * the device-side control-OUT helpers (`ra_usb_dcp_out_arm` /
 * `ra_usb_dcp_out_read`), the stage getter, and the wedged-SUREQ busy leg
 * do not merge with the production object -- they stay uncovered.
 *
 * This TU compiles a SECOND instrumented copy of the module that does NOT
 * mock the transports: the source is `#include`d with its five exported
 * symbols renamed to `*_cov2` (so they do not collide with the production
 * copy linked from `ra_core_hal`), but `internal_rmw16` /
 * `internal_wait_frdy` / `internal_dcp_push_chunk` keep their real
 * production bodies. Because no mock `#define` alters the compiled code,
 * this copy is byte-for-byte the same control-flow as the production
 * object, so its line coverage MERGES with production in the aggregate.
 *
 * To drive the real paths the tests pre-seed the `ra_sim_mmap` register
 * window directly (BRDYSTS / BRDYENB / DCPCTR / CFIFOCTR / DTLN) rather
 * than scripting interrupt edges. Under `RA_SIMULATOR_MODE` the real
 * `internal_wait_frdy` returns `k_ra_ok` immediately, so the DCP arm /
 * read ladders run to completion deterministically -- no SIGALRM, no
 * unbounded spin. The single silicon-only leg (`ra_usb_dcp_out_arm`'s
 * BRDYENB read-back timeout) already carries a `GCOVR_EXCL_LINE` marker
 * in the source; no exclusion is added here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_internal.h"
#include "unity_minimal.h"

/**
 * @enum thc2_const_t
 * @brief Named test vectors for the complementary host control-engine suite.
 *
 * @details Tests are exempt from the magic-number gate; these names keep the
 * intent of each seeded argument obvious at the call site.
 */
typedef enum : uint16_t {
  k_thc2_dev_addr    = 1U,  /**< A valid DEVADDn slot to program.      */
  k_thc2_rhst_fs     = 2U,  /**< DVSTCTR0.RHST = 10b (Full-Speed).     */
  k_thc2_cap         = 64U, /**< dcp_out_read destination capacity.    */
  k_thc2_dtln        = 8U,  /**< Non-zero DTLN drained via the CFIFO.  */
  k_thc2_speed_bogus = 9U,  /**< Not FS, not HS -> internal_pick NULL. */
} thc2_const_t;

/**
 * @brief Scrub the simulated USB register window to a known baseline.
 *
 * @details Wraps ::ra_sim_mmap_reset so a prior test's register writes
 * cannot leak into the next. No mock scripts exist in this TU -- the real
 * transports run against the plain-RAM window.
 */
static void prep2(void)
{
  ra_sim_mmap_reset();
}

/* Rename the five exported symbols so this instrumented copy does not clash
 * with the production definitions linked from ra_core_hal. Crucially, the
 * three register transports are NOT redirected: their real bodies run, so
 * this copy is structurally identical to production and its coverage merges
 * with the production object in the aggregate gcovr build. */
#define ra_usb_host_ctrl_stage       ra_usb_host_ctrl_stage_cov2
#define internal_host_program_devadd internal_host_program_devadd_cov2
#define ra_usb_dcp_out_arm           ra_usb_dcp_out_arm_cov2
#define ra_usb_dcp_out_read          ra_usb_dcp_out_read_cov2
#define ra_usb_host_control_xfer     ra_usb_host_control_xfer_cov2

#include "ra_usb_host_ctrl.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * White-box tests: each drives the real production logic on pre-seeded bytes.
 * =============================================================================
 */

/**
 * @test test_stage_getter_reads_diagnostic
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `ra_usb_host_ctrl_stage`
 * is a single-statement read of the bring-up diagnostic; no `&&` or `||`.)
 */
static void test_stage_getter_reads_diagnostic(void)
{
  TEST_BEGIN("stage getter returns the last host control-transfer stage");
  prep2();

  /* The getter is a pure read of the module's stage diagnostic. The first
   * stage code is k_ra_usb_cs_begin (0) until a transfer advances it. */
  TEST_ASSERT_EQ((uint8_t)k_ra_usb_cs_begin, ra_usb_host_ctrl_stage_cov2());

  TEST_END("stage getter returns the last host control-transfer stage");
}

/**
 * @test test_ctrl_setup_wedged_sureq_busy
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the wedged-SUREQ guard
 * and the HS-only SUREQCLR guard are nested single-condition `if`
 * statements; each is driven in isolation, no `&&` or `||`. This exercises
 * the same busy leg as the sibling suite but through the REAL
 * `internal_rmw16`, so the leg merges with the production object.)
 */
static void test_ctrl_setup_wedged_sureq_busy(void)
{
  TEST_BEGIN("ctrl_setup: wedged SUREQ reports busy (HS runs SUREQCLR, FS skips it)");

  const uint16_t sureq = (uint16_t)(1U << k_ra_dcpctr_bit_sureq);
  ra_usb_setup_t setup = {.bm_request_type = 0U,
                          .b_request       = 0U,
                          .w_value         = 0U,
                          .w_index         = 0U,
                          .w_length        = 0U};

  /* HS instance: SUREQ wedged -> internal_is_hs is true so the SUREQCLR
   * read-modify-write runs, but the RAM window does not model the SIE
   * aborting SUREQ, so it stays set and busy is returned. */
  prep2();
  volatile r_usb_regs_t* hs = internal_pick(k_ra_usb_speed_hs);
  hs->DCPCTR                = sureq;
  TEST_ASSERT_EQ(k_ra_err_busy, internal_host_ctrl_setup(hs, &setup));

  /* FS instance: SUREQ wedged -> internal_is_hs is false so SUREQCLR is
   * skipped and busy is returned directly. */
  prep2();
  volatile r_usb_regs_t* fs = internal_pick(k_ra_usb_speed_fs);
  fs->DCPCTR                = sureq;
  TEST_ASSERT_EQ(k_ra_err_busy, internal_host_ctrl_setup(fs, &setup));

  TEST_END("ctrl_setup: wedged SUREQ reports busy (HS runs SUREQCLR, FS skips it)");
}

/**
 * @test test_dcp_out_arm_real_path
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `ra_usb_dcp_out_arm` uses
 * a single-condition speed guard and a single-condition BRDYENB read-back
 * guard; no `&&` or `||`. The read-back timeout leg is silicon-only and is
 * marked GCOVR_EXCL_LINE in the source.)
 */
static void test_dcp_out_arm_real_path(void)
{
  TEST_BEGIN("dcp_out_arm: bad speed rejected; valid speed arms DCP BRDY + PID=BUF");

  /* Bogus speed -> internal_pick returns nullptr -> invalid_arg. */
  prep2();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_usb_dcp_out_arm_cov2((ra_usb_speed_t)k_thc2_speed_bogus));

  /* Valid FS speed -> the real arm ladder runs (dcp_pid, rmw16 CCPL clear,
   * select_cfifo, BCLR, BRDYENB enable, rmw16 SQSET, PID=BUF). The BRDYENB
   * read-back carries the DCP bit the line above OR-set, so the enable
   * succeeds and k_ra_ok is returned. */
  prep2();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_arm_cov2(k_ra_usb_speed_fs));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT((reg->BRDYENB & (uint16_t)k_ra_usb_dcp_pipe0_bit) != 0U);

  TEST_END("dcp_out_arm: bad speed rejected; valid speed arms DCP BRDY + PID=BUF");
}

/**
 * @test test_dcp_out_read_real_path
 *
 * @par MC/DC:
 * Decision: `if ((buf == nullptr) || (out_rx == nullptr))` (2 conditions)
 * - Vector 1: buf=valid, out_rx=valid -> false (control: both false).
 * - Vector 2: buf=NULL,  out_rx=valid -> true  (varies buf only).
 * - Vector 3: buf=valid, out_rx=NULL  -> true  (varies out_rx only).
 * Vectors 1+2 prove buf independently affects the outcome; 1+3 prove the
 * same for out_rx. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_dcp_out_read_real_path(void)
{
  TEST_BEGIN("dcp_out_read: speed/null guards, no-data, ZLP drain, data drain");
  uint8_t  buf[k_thc2_cap] = {};
  uint16_t rx              = 0U;

  /* Bogus speed -> internal_pick returns nullptr -> invalid_arg (this leg
   * short-circuits before the null-pointer guard). */
  prep2();
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_usb_dcp_out_read_cov2((ra_usb_speed_t)k_thc2_speed_bogus, buf, (uint16_t)k_thc2_cap, &rx));

  /* MC/DC vector 2: null destination buffer -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read_cov2(k_ra_usb_speed_fs, nullptr, (uint16_t)k_thc2_cap, &rx));

  /* MC/DC vector 3: null out-count pointer -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read_cov2(k_ra_usb_speed_fs, buf, (uint16_t)k_thc2_cap, nullptr));

  /* Both pointers valid but DCP BRDY not set -> the OUT packet has not
   * landed -> no_data. */
  prep2();
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->BRDYSTS               = 0U;
  TEST_ASSERT_EQ(k_ra_err_no_data,
                 ra_usb_dcp_out_read_cov2(k_ra_usb_speed_fs, buf, (uint16_t)k_thc2_cap, &rx));

  /* MC/DC vector 1 (both pointers valid) + a zero-length packet: BRDY set,
   * DTLN == 0 -> the empty bank is released via BCLR and k_ra_ok is
   * returned with rx == 0. The real internal_wait_frdy returns k_ra_ok
   * under simulation, so the CFIFO drain runs without a mock. */
  prep2();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  reg->CFIFOCTR = 0U;
  rx            = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_dcp_out_read_cov2(k_ra_usb_speed_fs, buf, (uint16_t)k_thc2_cap, &rx));
  TEST_ASSERT_EQ(0U, rx);

  /* BRDY set, non-zero DTLN within capacity -> the payload is drained via
   * the real internal_fifo_read (FS / MBW=16 path) and k_ra_ok is returned
   * with rx == DTLN. */
  prep2();
  reg           = ra_usb_fs();
  reg->BRDYSTS  = (uint16_t)k_ra_usb_dcp_pipe0_bit;
  reg->CFIFOCTR = (uint16_t)k_thc2_dtln;
  rx            = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_dcp_out_read_cov2(k_ra_usb_speed_fs, buf, (uint16_t)k_thc2_cap, &rx));
  TEST_ASSERT_EQ((uint16_t)k_thc2_dtln, rx);

  TEST_END("dcp_out_read: speed/null guards, no-data, ZLP drain, data drain");
}

int32_t main(void)
{
  test_stage_getter_reads_diagnostic();
  test_ctrl_setup_wedged_sureq_busy();
  test_dcp_out_arm_real_path();
  test_dcp_out_read_real_path();
  (void)fprintf(stderr, "[OK ] test_ra_usb_host_ctrl_cov2.c\n");
  return 0;
}

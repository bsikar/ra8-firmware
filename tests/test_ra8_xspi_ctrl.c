/**
 * @file test_ra8_xspi_ctrl.c
 * @brief Unit tests for the xSPI controller lifecycle surface: deinit,
 *        status read/clear, IRQ attach + dispatch, power transitions,
 *        XIP mode, DTR mode, DQS calibration, the OCTACKCR handshake
 *        timeout legs, suspend/resume, and the MC/DC vectors
 *
 * @details Split from test_ra8_xspi.c along the test-group seam.
 * The OCTACKCR handshake timeout legs MUST stay first in the runner:
 * the handshake is a one-shot latch that only sets on success, so its
 * timeout legs are reachable only before the first successful
 * ra8_xspi_init in this process. Shared fixture constants and
 * prep_flash() live in support/xspi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_xspi_flash.h"
#include "ra8_mstp.h"
#include "ra8_ospi_regs.h"
#include "ra8_system_regs.h"
#include "ra8_xspi.h"
#include "ra8_xspi_internal.h"
#include "support/xspi_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xspi_ctrl_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_xspi_ctrl_stamp_comstt = 0x12345678U,  /**< Xspi CTRL stamp comstt. */
  k_xspi_ctrl_stamp_ints   = 0xCAFEBABEUL, /**< Xspi CTRL stamp ints.   */
} xspi_ctrl_test_lit_t;

/* ---- full build-out ---- */

static uint32_t s_xspi_cb_count;
static uint32_t s_xspi_cb_last_mask;

static void stub_xspi_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_xspi_cb_count;
  s_xspi_cb_last_mask = mask;
}

static void prep_w51(void)
{
  prep_flash();
  (void)ra8_mstp_init();
  s_xspi_cb_count     = 0U;
  s_xspi_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("xspi deinit");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_deinit((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_deinit((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("xspi status read + clear");
  prep_w51();
  ra8_xspi((uint8_t)k_test_xspi_valid_inst0)->COMSTT = k_xspi_ctrl_stamp_comstt;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, &mask));
  TEST_ASSERT_EQ(0x12345678U, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_clear_status((uint8_t)k_test_xspi_valid_inst0, 0xFFFFFFFFU));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_clear_status((uint8_t)k_test_xspi_bad_instance, 0U));
  TEST_END("xspi status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("xspi attach + dispatch");
  prep_w51();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_attach_handler((uint8_t)k_test_xspi_valid_inst0, stub_xspi_cb, nullptr));
  /* Dispatch snapshots INTS (command-complete + error flags) and
   * hands it to the callback -- match that in the host test by
   * poking the INTS backing word before dispatching. */
  ra8_xspi((uint8_t)k_test_xspi_valid_inst0)->INTS = k_xspi_ctrl_stamp_ints;
  ra8_xspi_dispatch((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(1, s_xspi_cb_count);
  TEST_ASSERT_EQ(0xCAFEBABEUL, s_xspi_cb_last_mask);

  ra8_xspi_dispatch((uint8_t)k_test_xspi_bad_instance);
  TEST_ASSERT_EQ(1, s_xspi_cb_count);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_attach_handler((uint8_t)k_test_xspi_bad_instance, stub_xspi_cb, nullptr));
  TEST_END("xspi attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("xspi power transition");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_enter_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_exit_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_xspi_enter_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_xspi_exit_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi power transition");
}

/* =============================================================================
 * Sweep 6 extensions: XIP toggle, DTR, DQS calibration, suspend / resume
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_xip_set_mode_enable_then_disable(void)
{
  TEST_BEGIN("xspi set_xip_mode enable+disable");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s8s8s));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, true, 0xEBU, 3U));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_only, reg->BMCTL0);
  TEST_ASSERT_EQ(k_ra8_xspi_cmctlch_xipen_mask, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(k_ra8_xspi_cmctlch_xipen_mask, reg->CMCTLCH[1]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, false, 0xEBU, 3U));
  TEST_ASSERT_EQ(0, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(0, reg->CMCTLCH[1]);
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_write, reg->BMCTL0);
  TEST_END("xspi set_xip_mode enable+disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_xip_set_mode_invalid_args(void)
{
  TEST_BEGIN("xspi set_xip_mode rejects bad args");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_bad_instance, true, 0xEBU, 3U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, true, 0xEBU, 5U));
  TEST_END("xspi set_xip_mode rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dtr_mode(void)
{
  TEST_BEGIN("xspi set_dtr_mode toggles DDREN");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_8d8d8d));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_valid_inst0, true));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(k_ra8_xspi_liocfgcs_mask_ddren,
                 (reg->LIOCFGCS[0] & k_ra8_xspi_liocfgcs_mask_ddren));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_valid_inst0, false));
  TEST_ASSERT_EQ(0, (reg->LIOCFGCS[0] & k_ra8_xspi_liocfgcs_mask_ddren));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_bad_instance, true));
  TEST_END("xspi set_dtr_mode toggles DDREN");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_calibrate_dqs(void)
{
  TEST_BEGIN("xspi calibrate_dqs happy / retry / timeout legs");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_8d8d8d));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);

  /* Happy path: the CAEN auto-clear wait is seam-satisfied on its
   * first poll. The driver's own arm write leaves CAEN set in host RAM
   * (only the real controller clears it on silicon). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_xspi_ccctl0_mask_caen, (reg->CCCTLCS[0] & k_ra8_xspi_ccctl0_mask_caen));

  /* Retry leg: the phase-scan "completes" on the 3rd poll. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after((const volatile void*)&reg->CCCTLCS[0], 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  ra8_fake_mmio_reset();

  /* Timeout leg: CAEN never auto-clears -> bounded poll exhausts. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->CCCTLCS[0]));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  ra8_fake_mmio_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi calibrate_dqs happy / retry / timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the OCTACKCR SREQ/SRDY
 * handshake timeout legs of the one-shot clock-block init, isolated
 * per wait-loop with fail-nth. Must run BEFORE any successful
 * ra8_xspi_init in this binary: the handshake is guarded by a static
 * "already inited" latch that is only set on success.)
 */
static void test_octack_handshake_timeout_legs(void)
{
  TEST_BEGIN("xspi OCTACKCR handshake timeout legs");

  /* Leg 1: OCTACKSRDY=1 never acknowledges (first wait-loop). */
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_octackcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  /* Leg 2: OCTACKSRDY=0 never acknowledges (second wait-loop). */
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_octackcr(), 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  TEST_END("xspi OCTACKCR handshake timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_suspend_resume(void)
{
  TEST_BEGIN("xspi suspend + resume");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_suspend((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_resume((uint8_t)k_test_xspi_valid_inst0));
  TEST_END("xspi suspend + resume");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_suspend_resume_null(void)
{
  TEST_BEGIN("xspi suspend / resume reject bad instance");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_suspend((uint8_t)k_test_xspi_bad_instance));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_resume((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi suspend / resume reject bad instance");
}

/**
 * @enum test_xspi_mcdc_t
 * @brief Numeric vectors used by the MC/DC tests below.
 */
typedef enum : uint8_t {
  k_test_xspi_addr_bytes_3    = 3U,    /**< Valid 24-bit address mode.   */
  k_test_xspi_addr_bytes_4    = 4U,    /**< Valid 32-bit address mode.   */
  k_test_xspi_addr_bytes_bad  = 5U,    /**< Neither 3 nor 4: rejected.   */
  k_test_xspi_reset_bytes_1s  = 1U,    /**< Reset cmd width for 1S mode. */
  k_test_xspi_reset_bytes_8d  = 2U,    /**< Reset cmd width for 8D mode. */
  k_test_xspi_reset_bytes_bad = 3U,    /**< Neither 1 nor 2: rejected.   */
  k_test_xspi_xip_read_cmd    = 0xEBU, /**< Test XSPI xip read cmd.      */
} test_xspi_mcdc_t;

/**
 * @test test_set_xip_mode_mcdc_addr_bytes
 *
 * @par MC/DC:
 * Decision: `if ((addr_bytes != k_ra8_xspi_addr_bytes_3) &&
 *               (addr_bytes != k_ra8_xspi_addr_bytes_4))`
 * (2 conditions, libs/ra8_hal/src/ra8_xspi.c line 889)
 * - Vector 1: addr_bytes=3 -> false (control: C1 short-circuits to F)
 * - Vector 2: addr_bytes=4 -> false (varies C2 only; C1 held T)
 * - Vector 3: addr_bytes=5 -> true  (varies C1 vs vec1; both true)
 * Vectors 1+3 prove `addr_bytes != 3` independently flips outcome
 * (with C2 held implicitly true since 5 != 4 and 3 trivially != 4 is
 * unreachable -- C2 not evaluated when C1=F, so the masking pair is
 * vec1 (decision F) vs vec3 (decision T) varying C1). Vectors 2+3
 * prove `addr_bytes != 4` independently flips outcome with C1 held T.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_set_xip_mode_mcdc_addr_bytes(void)
{
  TEST_BEGIN("xspi set_xip_mode MC/DC: addr_bytes != 3 && addr_bytes != 4");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s8s8s));

  /* Vector 1: addr_bytes=3. C1=(3!=3)=F short-circuits. Decision F.
   * Function proceeds to programme XIP and returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_3));

  /* Vector 2: addr_bytes=4. C1=(4!=3)=T, C2=(4!=4)=F. Decision F.
   * Function returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_4));

  /* Vector 3: addr_bytes=5. C1=(5!=3)=T, C2=(5!=4)=T. Decision T,
   * function returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_bad));
  TEST_END("xspi set_xip_mode MC/DC: addr_bytes != 3 && addr_bytes != 4");
}

/**
 * @test test_software_reset_mcdc_cmd_bytes
 *
 * @par MC/DC:
 * Decision: `if ((cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_1s) &&
 *               (cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_8d))`
 * (2 conditions, libs/ra8_hal/src/ra8_xspi.c line 1026)
 * - Vector 1: cmd_bytes=1 -> false (C1=(1!=1)=F short-circuits)
 * - Vector 2: cmd_bytes=2 -> false (C1=(2!=1)=T, C2=(2!=2)=F)
 * - Vector 3: cmd_bytes=3 -> true  (C1=T, C2=T -> invalid_arg)
 * Vectors 1+3 vary C1 (decision flips); vectors 2+3 vary C2 (decision
 * flips). N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_software_reset_mcdc_cmd_bytes(void)
{
  TEST_BEGIN("xspi software_reset MC/DC: cmd_bytes != 1 && cmd_bytes != 2");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  /* Vector 1: cmd_bytes=1 (1S-1S-1S reset width). Decision F, function
   * proceeds and issues RSTEN + RST opcodes; returns ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0, (uint8_t)k_test_xspi_reset_bytes_1s));

  /* Vector 2: cmd_bytes=2 (8D-8D-8D reset width). Decision F, ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0, (uint8_t)k_test_xspi_reset_bytes_8d));

  /* Vector 3: cmd_bytes=3. Decision T, returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0,
                                         (uint8_t)k_test_xspi_reset_bytes_bad));
  TEST_END("xspi software_reset MC/DC: cmd_bytes != 1 && cmd_bytes != 2");
}

int main(void)
{
  /* Runs first: the OCTACKCR handshake is a one-shot latch that only
   * sets on success, so its timeout legs are reachable only before the
   * first successful ra8_xspi_init in this process. */
  test_octack_handshake_timeout_legs();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_xip_set_mode_enable_then_disable();
  test_xip_set_mode_invalid_args();
  test_set_dtr_mode();
  test_calibrate_dqs();
  test_suspend_resume();
  test_suspend_resume_null();
  test_set_xip_mode_mcdc_addr_bytes();
  test_software_reset_mcdc_cmd_bytes();
  return 0;
}

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_canfd_ctrl.c
 * @brief Unit tests for the ra8_canfd control surface: status + IRQ
 *        dispatch + power transitions, GAFL filters, BRS/ISO mode, the
 *        CANFD clock-handshake and mode-transition timeout legs, and
 *        the driver MC/DC vectors
 *
 * @details Split from test_ra8_canfd.c along the test-group seam; the
 * sibling test_ra8_canfd.c owns init/bitrate/transmit/receive/error
 * state. The clock srdy timeout legs MUST stay first in the runner:
 * the CANFD clock block latches a one-shot static "already inited"
 * guard on the first successful handshake in this process, after which
 * the handshake (and its timeout legs) is skipped.
 */

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum canfd_ctrl_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_canfd_ctrl_stamp_cfdc  = 0xDEADBEEFU, /**< Canfd CTRL stamp cfdc.  */
  k_canfd_ctrl_stamp_cfdc2 = 0xFFFFFFFFU, /**< Canfd CTRL stamp cfdc2. */
  k_canfd_ctrl_stamp_cfdc3 = 0xCAFEU,     /**< Canfd CTRL stamp cfdc3. */
} canfd_ctrl_test_lit_t;

typedef enum : uint8_t {
  k_ra8_canfd_test_channel_0   = 0U, /**< RA8 CANFD test channel 0.   */
  k_ra8_canfd_test_channel_1   = 1U, /**< RA8 CANFD test channel 1.   */
  k_ra8_canfd_test_channel_bad = 2U, /**< RA8 CANFD test channel bad. */
} ra8_canfd_test_channel_t;

typedef enum : uint32_t {
  k_ra8_test_bitrate_500k     = 500000U,     /**< RA8 test bitrate 500k.     */
  k_ra8_test_bitrate_1m       = 1000000U,    /**< RA8 test bitrate 1m.       */
  k_ra8_test_bitrate_250k     = 250000U,     /**< RA8 test bitrate 250k.     */
  k_ra8_test_bitrate_zero     = 0U,          /**< RA8 test bitrate zero.     */
  k_ra8_test_bitrate_invalid  = 999999U,     /**< RA8 test bitrate invalid.  */
  k_ra8_test_bitrate_bad_data = 1234567U,    /**< RA8 test bitrate bad data. */
  k_ra8_test_ext_id           = 0x1FABCDEFU, /**< RA8 test ext ID.           */
  k_ra8_test_std_id           = 0x123U,      /**< RA8 test std ID.           */
  k_ra8_test_oversized_std_id = 0x800U,      /**< RA8 test oversized std ID. */
  k_ra8_test_invalid_ext_id   = 0x40000000U, /**< RA8 test invalid ext ID.   */
  /* TEC = 0x55 in [31:24], REC = 0xAA in [23:16] of CFDC[0].STS. */
  k_ra8_test_sts_encoded  = 0x55AA0000U, /**< RA8 test status encoded. */
  k_ra8_test_expected_tec = 0x55U,       /**< RA8 test expected TEC.   */
  k_ra8_test_expected_rec = 0xAAU,       /**< RA8 test expected REC.   */
} ra8_canfd_test_vals_t;

/* ---- status + IRQ + power ---- */

static uint32_t s_canfd_cb_count;
static uint32_t s_canfd_cb_last_mask;
static uint8_t  s_canfd_cb_last_channel;

static void stub_canfd_cb(void* ctx, uint8_t ch, uint32_t mask)
{
  (void)ctx;
  ++s_canfd_cb_count;
  s_canfd_cb_last_mask    = mask;
  s_canfd_cb_last_channel = ch;
}

static void prep_w53(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  s_canfd_cb_count        = 0U;
  s_canfd_cb_last_mask    = 0U;
  s_canfd_cb_last_channel = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status(void)
{
  TEST_BEGIN("canfd get_status");
  prep_w53();
  ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0)->CFDC[0].STS = k_canfd_ctrl_stamp_cfdc;
  uint32_t mask                                               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_get_status((uint8_t)k_ra8_canfd_test_channel_0, &mask));
  TEST_ASSERT_EQ(0xDEADBEEFU, mask);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_get_status((uint8_t)k_ra8_canfd_test_channel_0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_get_status((uint8_t)k_ra8_canfd_test_channel_bad, &mask));
  TEST_END("canfd get_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status(void)
{
  TEST_BEGIN("canfd clear_status");
  prep_w53();
  ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0)->CFDC[0].ERFL = k_canfd_ctrl_stamp_cfdc2;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_clear_status((uint8_t)k_ra8_canfd_test_channel_0, 0x0000FF00U));
  TEST_ASSERT_EQ(0xFFFF00FFU, ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0)->CFDC[0].ERFL);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_clear_status((uint8_t)k_ra8_canfd_test_channel_bad, 0U));
  TEST_END("canfd clear_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("canfd attach + dispatch");
  prep_w53();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_attach_handler(stub_canfd_cb, (void*)(uintptr_t)0xABU));
  ra8_canfd((uint8_t)k_ra8_canfd_test_channel_1)->CFDC[0].ERFL = k_canfd_ctrl_stamp_cfdc3;
  ra8_canfd_dispatch((uint8_t)k_ra8_canfd_test_channel_1);
  TEST_ASSERT_EQ(1, s_canfd_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_canfd_cb_last_mask);
  TEST_ASSERT_EQ(k_ra8_canfd_test_channel_1, s_canfd_cb_last_channel);

  ra8_canfd_dispatch((uint8_t)k_ra8_canfd_test_channel_bad);
  TEST_ASSERT_EQ(1, s_canfd_cb_count);
  TEST_END("canfd attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("canfd power transition");
  prep_w53();
  /* ra8_canfd_init drives the real clock handshake + mode-transition polls
   * on host now (T1-01); arm the seam so each acknowledges on the first
   * poll. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_enter_stop((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_exit_stop((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_enter_stop((uint8_t)k_ra8_canfd_test_channel_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_exit_stop((uint8_t)k_ra8_canfd_test_channel_bad));
  TEST_END("canfd power transition");
}

/* ---------------------------------------------------------------------------
 * Sweep 15 / Phase 2: GAFL filter, BRS, ISO/non-ISO mode.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

static void test_filter_set_writes_id_mask_dlc(void)
{
  TEST_BEGIN("canfd filter_set programs CFDGAFL slot");
  prep_w53();

  /* filter_set drives GL_RESET -> write AFL -> GL_OPERATION; both the
   * reset-mode ack (GRSTSTS SET) and the operation-mode ack
   * (GRSTSTS|GHLTSTS CLEAR) waits on CFDGSTS run for real on host now
   * (T1-01). satisfy_after(0) makes both directions ack on the first
   * poll. */
  volatile r_canfd_t* reg = ra8_canfd(0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));

  /* Filter 0 -> page 0, slot 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_filter_set(0U, 0x123U, 0x7FFU, 8U));
  TEST_ASSERT_EQ(0x123U, reg->CFDGAFL[0].ID);
  TEST_ASSERT_EQ(0x7FFU | (1UL << 29U), reg->CFDGAFL[0].M);
  /* DLC=8 packed into [31:28] -> 8 << 28 = 0x80000000. P1 bit 0
   * (GAFLFDP0) is also set so the matched frame is routed into RX
   * FIFO 0 instead of being dropped. */
  TEST_ASSERT_EQ(0x80000001U, reg->CFDGAFL[0].P1);
  /* AFLDAE re-locked at exit (CFDGAFLECTR cleared). */
  TEST_ASSERT_EQ(0, reg->CFDGAFLECTR);
  TEST_END("canfd filter_set programs CFDGAFL slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_filter_set_validation(void)
{
  TEST_BEGIN("canfd filter_set rejects out-of-range args");
  prep_w53();

  /* Filter id past the 256-entry total. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_filter_set(0xFFFFU, 0x123U, 0x7FFU, 0U));
  /* DLC > 15. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_filter_set(0U, 0x123U, 0x7FFU, 16U));
  /* ID with stray top bits beyond the 29-bit extended range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_filter_set(0U, 0x40000000U, 0xFFFFU, 0U));
  TEST_END("canfd filter_set rejects out-of-range args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_brs_updates_dcfg(void)
{
  TEST_BEGIN("canfd set_brs reprograms CFDC2[0].DCFG");
  prep_w53();
  /* ra8_canfd_init drives the real clock handshake + mode-transition polls
   * on host now (T1-01); arm the seam so each acknowledges on the first
   * poll. */
  volatile r_canfd_t* reg_init = ra8_canfd(0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg_init->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg_init->CFDC[0].STS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init(0U));
  /* 1 Mbps fast phase resolves cleanly against the fake PCLKA. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_brs(0U, 1000000U));
  volatile r_canfd_t* reg = ra8_canfd(0U);
  TEST_ASSERT(reg->CFDC2[0].DCFG != 0U);
  /* Bad channel -> null_ptr (matches existing convention). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_canfd_set_brs(2U, 1000000U));
  /* Zero rate -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_canfd_set_brs(0U, 0U));
  TEST_END("canfd set_brs reprograms CFDC2[0].DCFG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_iso_mode_toggles_niso(void)
{
  TEST_BEGIN("canfd set_iso_mode toggles CFDGFDCFG.NISO");
  prep_w53();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_iso_mode(true));
  volatile r_canfd_t* reg = ra8_canfd(0U);
  TEST_ASSERT((reg->CFDGFDCFG & (uint32_t)k_ra8_gfdcfg_bit_niso) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_set_iso_mode(false));
  TEST_ASSERT((reg->CFDGFDCFG & (uint32_t)k_ra8_gfdcfg_bit_niso) == 0U);
  TEST_END("canfd set_iso_mode toggles CFDGFDCFG.NISO");
}

/**
 * @test test_mcdc_set_bitrate_data_rate_guard
 *
 * @par MC/DC:
 * Decision: `if ((data_bitrate_bps != 0U) &&
 *               (data_bitrate_bps > bitrate_bps))`
 * (2 conditions, libs/ra8_hal/src/ra8_canfd.c line 313 -- gap row 313 in CSV)
 * - Vector 1: data=0, nominal=250k -> C1=F short-circuits.
 *   Decision F -> classic CAN, ok.
 * - Vector 2: data=500k, nominal=500k -> C1=T, C2=(500k>500k)=F.
 *   Decision F -> CAN-FD with same nominal/data rate, ok (varies C2).
 * - Vector 3: data=1M, nominal=250k -> C1=T, C2=(1M>250k)=T.
 *   Decision T -> programs data-phase timing register, returns ok
 *   (canonical CAN-FD configuration with faster data phase).
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T (decision flips, C2
 * masked in V1 by short-circuit). MC/DC pair for C2: V2(T,F)->F vs
 * V3(T,T)->T (decision flips, C1 held T). N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC.
 */
static void test_mcdc_set_bitrate_data_rate_guard(void)
{
  TEST_BEGIN("canfd set_bitrate MC/DC: data_bitrate!=0 && data>nominal");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* All three vectors drive set_bitrate's real CH_RESET/CH_OPERATION
   * channel-mode acks on host now (T1-01); satisfy_after(0) acks each on
   * the first poll so the MC/DC decision -- not an hw_timeout -- drives
   * each outcome. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_satisfy_after(&ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0)->CFDC[0].STS, 0U));

  /* Vector 1: data=0 -> classic CAN path. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_500k,
                                       0U));

  /* Vector 2: data == nominal (both 500k) -> same rate, decision F. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_500k,
                                       (uint32_t)k_ra8_test_bitrate_500k));

  /* Vector 3: data > nominal -> decision T -> source programs the data-phase
   * timing register (canonical CAN-FD with a faster data phase) and returns
   * k_ra8_ok. The MC/DC obligation is to flip the decision; both legs return
   * k_ra8_ok in this driver because neither is an error path. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_250k,
                                       (uint32_t)k_ra8_test_bitrate_1m));

  TEST_END("canfd set_bitrate MC/DC: data_bitrate!=0 && data>nominal");
}

/**
 * @test test_mcdc_validate_frame_brs_without_fd
 *
 * @par MC/DC:
 * Decision: `if ((frame->is_brs != 0U) && (frame->is_fd == 0U))`
 * (2 conditions, libs/ra8_hal/src/ra8_canfd.c line 485 -- gap row 346 in CSV)
 * - Vector 1: brs=0, fd=0 -> C1=F short-circuit. Decision F -> ok
 *   (classic CAN frame).
 * - Vector 2: brs=1, fd=1 -> C1=T, C2=F. Decision F -> ok
 *   (CAN-FD with bit-rate-switch; varies C2 vs V3).
 * - Vector 3: brs=1, fd=0 -> C1=T, C2=T. Decision T -> invalid_arg
 *   (BRS demanded without enabling FD; varies C1 vs V1).
 * MC/DC pair for C1: V1(F,_)->F vs V3(T,T)->T. MC/DC pair for C2:
 * V2(T,F)->F vs V3(T,T)->T. N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_mcdc_validate_frame_brs_without_fd(void)
{
  TEST_BEGIN("canfd validate_frame MC/DC: is_brs && !is_fd");
  ra8_fake_mmap_reset();

  /* Vector 1: brs=0, fd=0 -> classic CAN frame, ok. */
  ra8_canfd_frame_t f1 = {};
  f1.id                = (uint32_t)k_ra8_test_std_id;
  f1.dlc               = 4U;
  f1.is_extended       = 0U;
  f1.is_fd             = 0U;
  f1.is_brs            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &f1));

  /* Vector 2: brs=1, fd=1 -> CAN-FD with BRS, ok. */
  ra8_canfd_frame_t f2 = {};
  f2.id                = (uint32_t)k_ra8_test_std_id;
  f2.dlc               = 4U;
  f2.is_extended       = 0U;
  f2.is_fd             = 1U;
  f2.is_brs            = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &f2));

  /* Vector 3: brs=1, fd=0 -> rejected. */
  ra8_canfd_frame_t f3 = {};
  f3.is_brs            = 1U;
  f3.is_fd             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &f3));

  TEST_END("canfd validate_frame MC/DC: is_brs && !is_fd");
}

/* ---------------------------------------------------------------------------
 * T1-01 / #177: real timeout + error-propagation legs.
 *
 * The bounded HW polls in ra8_canfd.c now run for real on host via the
 * ra8_hw_wait_flag_* primitives (ra8_hw_err.h) consulting the ra8_fake_mmio
 * fault seam. Each case below arms fail_wait / fail_nth_wait on the EXACT
 * register a specific wait polls, then asserts the enclosing public
 * ra8_canfd_* function returns the propagated k_ra8_err_hw_timeout. Modelled
 * on test_bringup_fail_legs in test_ra8_eth_gwca.c.
 * --------------------------------------------------------------------------- */

/**
 * @test test_canfd_clock_srdy_set_timeout
 *
 * @par MC/DC:
 * (single-condition ``if (err != k_ra8_ok)`` guard -- no compound decision
 * in the code under test. The leg is reached by arming the ra8_fake_mmio
 * seam to time out exactly one bounded poll.)
 *
 * @details Covers the CANFDCKSRDY=1 handshake timeout in
 * ``internal_canfd_clock_block_init`` (ra8_canfd.c "canfd: CANFDCKSRDY=1
 * timeout" log + break) and its propagation through ``ra8_canfd_init``
 * ("clk_err != k_ra8_ok -> return clk_err"). Failing the FIRST bounded
 * wait on CANFDCKCR (the ``internal_wait_canfdcksrdy(1U)`` SRDY-set poll)
 * makes the block handshake time out. This case MUST run before any
 * successful ra8_canfd_init because the clock block latches a one-shot
 * static "already inited" guard on success; a timeout leaves it unlatched.
 */
static void test_canfd_clock_srdy_set_timeout(void)
{
  TEST_BEGIN("canfd init CANFDCKSRDY=1 handshake timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* Fail the first bounded wait on CANFDCKCR (SRDY-set). fail_wait fails
   * every wait on the register, but the driver breaks out after the first
   * one times out, so no later CANFDCKCR wait runs. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_sys_canfdckcr()));

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_END("canfd init CANFDCKSRDY=1 handshake timeout");
}

/**
 * @test test_canfd_clock_srdy_clear_timeout
 *
 * @par MC/DC:
 * (single-condition ``if (err != k_ra8_ok)`` guard -- no compound decision
 * in the code under test. The leg is reached by arming the ra8_fake_mmio
 * seam to time out exactly one bounded poll.)
 *
 * @details Covers the SECOND handshake timeout in
 * ``internal_canfd_clock_block_init`` (the ``internal_wait_canfdcksrdy(0U)``
 * SRDY-clear poll -> "canfd: CANFDCKSRDY=0 timeout" log + break). Both
 * SREQ-set and SREQ-clear waits poll the SAME CANFDCKCR register, so
 * fail_nth_wait(ckcr, 1) lets the first (SRDY=1) wait-loop succeed and
 * times out only the second (SRDY=0) wait-loop -- fail_wait alone would
 * stop at the first. Like the SRDY=1 case this MUST run before any
 * successful init (the one-shot clock-inited guard stays unlatched on a
 * timeout).
 */
static void test_canfd_clock_srdy_clear_timeout(void)
{
  TEST_BEGIN("canfd init CANFDCKSRDY=0 handshake timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* Wait-loop 0 = SRDY-set (succeeds), wait-loop 1 = SRDY-clear (fails). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(ra8_sys_canfdckcr(), 1U));

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_END("canfd init CANFDCKSRDY=0 handshake timeout");
}

/**
 * @test test_init_global_operation_timeout
 *
 * @par MC/DC:
 * (single-condition ``if (gop_err != k_ra8_ok)`` / ``if (open_err !=
 * k_ra8_ok)`` guards -- no compound decision in the code under test.)
 *
 * @details Covers the GL_OPERATION convergence timeout in
 * ``internal_canfd_open_channel`` ("gop_err != k_ra8_ok -> return gop_err")
 * and its propagation through ``ra8_canfd_init`` ("open_err != k_ra8_ok ->
 * return open_err"). ``internal_canfd_open_channel`` polls CFDGSTS twice:
 * wait-loop 0 is the GL_RESET ack, wait-loop 1 is the GL_OPERATION ack.
 * fail_nth_wait(&reg->CFDGSTS, 1) lets the reset ack succeed and times out
 * only the operation ack. The channel-mode acks poll CFDC[0].STS (a
 * different register) so they succeed un-armed. CANFDCKCR is armed to
 * satisfy so the clock block succeeds regardless of the one-shot guard
 * state.
 */
static void test_init_global_operation_timeout(void)
{
  TEST_BEGIN("canfd init GL_OPERATION timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  /* CFDGSTS wait-loop 1 (GL_OPERATION ack) times out; wait-loop 0
   * (GL_RESET ack) succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(&reg->CFDGSTS, 1U));

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0));
  TEST_END("canfd init GL_OPERATION timeout");
}

/**
 * @test test_set_test_mode_happy_and_validation
 *
 * @par MC/DC:
 * (no compound decisions in the exercised code -- the halt/operation
 * transitions and the mode-range / channel guards are single-condition
 * checks; the happy leg drives the CH_HALT status-bit wait for real.)
 *
 * @details First host coverage of ``ra8_canfd_set_test_mode``. The happy
 * call drives CH_HALT then CH_OPERATION: the CH_HALT ack runs the
 * ``internal_wait_status_bit(reg, CHLTSTS)`` leg in
 * ``ra8_canfd_internal_set_channel_mode`` (the ``mode == k_ra8_chmdc_halt``
 * branch), and the return path drives the CH_OPERATION clear-wait. Both
 * poll CFDC[0].STS; satisfy_after(0) acks both on the first poll. The
 * invalid-mode call exercises the "``mode > k_ra8_ctms_self_test_1`` ->
 * invalid_arg" guard, and the bad-channel call the null-ptr guard.
 */
static void test_set_test_mode_happy_and_validation(void)
{
  TEST_BEGIN("canfd set_test_mode happy + validation");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* CH_HALT ack then CH_OPERATION ack both poll CFDC[0].STS. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_canfd_set_test_mode((uint8_t)k_ra8_canfd_test_channel_0, k_ra8_ctms_self_test_1));
  /* CTME (bit 24) latched -> internal-loopback self-test armed. */
  TEST_ASSERT((reg->CFDC[0].CTR & (uint32_t)k_ra8_cnctr_mask_ctme) != 0U);

  /* Mode selector past the 11b maximum -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_test_mode((uint8_t)k_ra8_canfd_test_channel_0, (ra8_ctms_mode_t)4U));

  /* Bad channel -> null_ptr (matches driver convention). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_set_test_mode((uint8_t)k_ra8_canfd_test_channel_bad, k_ra8_ctms_basic));
  TEST_END("canfd set_test_mode happy + validation");
}

/**
 * @test test_set_test_mode_halt_timeout
 *
 * @par MC/DC:
 * (single-condition ``if (halt_err != k_ra8_ok)`` guard -- no compound
 * decision in the code under test.)
 *
 * @details Covers the CH_HALT timeout leg in ``ra8_canfd_set_test_mode``:
 * when the CH_HALT status-bit wait times out the driver best-effort
 * recovers the channel to CH_OPERATION and returns the timeout. Both the
 * failing CH_HALT ack and the recovery CH_OPERATION ack poll CFDC[0].STS,
 * so fail_nth_wait(&reg->CFDC[0].STS, 0) times out only the first
 * wait-loop (CH_HALT) and lets the recovery wait-loop succeed.
 */
static void test_set_test_mode_halt_timeout(void)
{
  TEST_BEGIN("canfd set_test_mode CH_HALT timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* CFDC[0].STS wait-loop 0 (CH_HALT ack) times out; wait-loop 1
   * (recovery CH_OPERATION ack) succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(&reg->CFDC[0].STS, 0U));

  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_canfd_set_test_mode((uint8_t)k_ra8_canfd_test_channel_0, k_ra8_ctms_self_test_1));
  TEST_END("canfd set_test_mode CH_HALT timeout");
}

/**
 * @test test_filter_set_timeout_and_page1_legs
 *
 * @par MC/DC:
 * (single-condition guards -- the ``filter_id >= k_ra8_canfd_afl_per_page``
 * early-return in ``internal_bump_rnc0_locked`` and the two
 * ``if (err != k_ra8_ok)`` GL-mode propagation guards in
 * ``ra8_canfd_filter_set``; no compound decisions.)
 *
 * @details Three legs of ``ra8_canfd_filter_set``, all polling CFDGSTS
 * (wait-loop 0 = GL_RESET ack, wait-loop 1 = GL_OPERATION ack):
 * - page-1 filter (filter_id 16) so ``internal_bump_rnc0_locked`` hits the
 *   ``filter_id >= per_page`` page-0-only early return; both GL-mode acks
 *   succeed (satisfy_after 0) -> k_ra8_ok.
 * - GL_RESET ack times out (fail_nth_wait 0) -> "reset_err != k_ra8_ok ->
 *   return reset_err".
 * - GL_OPERATION ack times out (fail_nth_wait 1) while the GL_RESET ack
 *   succeeds -> "op_err != k_ra8_ok -> return op_err".
 */
static void test_filter_set_timeout_and_page1_legs(void)
{
  TEST_BEGIN("canfd filter_set page-1 bump-skip + GL-mode timeouts");

  /* AFL is global; the driver reaches it via channel 0. */
  volatile r_canfd_t* reg = ra8_canfd(0U);
  TEST_ASSERT_NOT_NULL((void*)reg);

  /* Leg 1: filter_id 16 -> page 1, slot 0; bump_rnc0 page-0-only skip. */
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_filter_set(16U, 0x123U, 0x7FFU, 8U));
  TEST_ASSERT_EQ(0x123U, reg->CFDGAFL[0].ID);

  /* Leg 2: GL_RESET ack (CFDGSTS wait-loop 0) times out. */
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_canfd_filter_set(0U, 0x123U, 0x7FFU, 8U));

  /* Leg 3: GL_OPERATION ack (CFDGSTS wait-loop 1) times out; reset ack ok. */
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(&reg->CFDGSTS, 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_canfd_filter_set(0U, 0x123U, 0x7FFU, 8U));

  TEST_END("canfd filter_set page-1 bump-skip + GL-mode timeouts");
}
int32_t main(void)
{
  /* Clock-handshake timeout legs MUST run before any successful
   * ra8_canfd_init: the CANFD clock block latches a one-shot static
   * "already inited" guard on the first successful handshake, after which
   * the handshake (and its timeout legs) is skipped. A timeout leaves the
   * guard unlatched, so these two cases still exercise the real poll. */
  test_canfd_clock_srdy_set_timeout();
  test_canfd_clock_srdy_clear_timeout();
  test_get_status();
  test_clear_status();
  test_attach_and_dispatch();
  test_power_transition();
  test_filter_set_writes_id_mask_dlc();
  test_filter_set_validation();
  test_set_brs_updates_dcfg();
  test_set_iso_mode_toggles_niso();
  test_mcdc_set_bitrate_data_rate_guard();
  test_mcdc_validate_frame_brs_without_fd();
  test_init_global_operation_timeout();
  test_set_test_mode_happy_and_validation();
  test_set_test_mode_halt_timeout();
  test_filter_set_timeout_and_page1_legs();
  (void)fprintf(stderr, "[OK ] test_ra8_canfd_ctrl.c\n");
  return 0;
}

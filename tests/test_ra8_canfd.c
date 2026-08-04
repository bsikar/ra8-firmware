/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_canfd.c
 * @brief Unit tests for ra8_canfd.c (RA8D2 CANFD driver)
 *
 * @details
 * Tests assert against the FSP-aligned register layout in
 * `ra8_canfd_regs.h` -- per-channel `CFDC[0].NCFG/CTR/STS/ERFL`,
 * `CFDC2[0].DCFG`, `CFDRF[0]` for RX FIFO 0, `CFDTM[0]` for TX MB 0,
 * `CFDTMC[0]` byte transmit-request register, and `CFDRFSTS[0]` /
 * `CFDRFPCTR[0]` for FIFO status.
 *
 * Covers init/bitrate/transmit/receive/error-state; the control
 * surface (status, dispatch, power, filters, BRS/ISO, timeout legs,
 * MC/DC vectors) lives in the sibling test_ra8_canfd_ctrl.c.
 */

#include <string.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum canfd_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_canfd_test_dlc_bad   = 15U,   /**< Canfd test DLC bad.   */
  k_canfd_test_shift_dlc = 28U,   /**< Canfd test shift DLC. */
  k_canfd_test_df0       = 0x11U, /**< Canfd test DF0.       */
  k_canfd_test_df1       = 0x22U, /**< Canfd test df1.       */
  k_canfd_test_df2       = 0x33U, /**< Canfd test df2.       */
  k_canfd_test_df3       = 0x44U, /**< Canfd test df3.       */
} canfd_test_lit_t;

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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_channel0_happy(void)
{
  TEST_BEGIN("canfd init channel 0 happy");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* The CANFD clock handshake + global/channel mode-transition polls run
   * for real on host now that the driver's RA8_OFF_TARGET poll short-
   * circuit is gone (T1-01). Model the block acknowledging each requested
   * state on the first poll: satisfy_after(0) satisfies both the reset-
   * mode ack (flag SET) and the operation-mode ack (flag CLEAR) waits on
   * one status register, and the CANFDCKSRDY set/clear handshake -- which
   * the driver drives AFTER overwriting CANFDCKCR, so a pre-staged bit
   * cannot survive the write. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));

  const ra8_err_t err = ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  /* Final mode latched into CTR.CHMDC[1:0] = 0 (operation). */
  TEST_ASSERT_EQ(k_ra8_chmdc_operation, (reg->CFDC[0].CTR & k_ra8_cnctr_mask_chmdc));
  TEST_END("canfd init channel 0 happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_channel0_timeout(void)
{
  TEST_BEGIN("canfd init channel 0 timeout path");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Same real-poll seam arming as the happy path (T1-01): the block
   * acknowledges each mode transition and the clock handshake on the
   * first poll. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));

  const ra8_err_t err = ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("canfd init channel 0 timeout path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_channel1(void)
{
  TEST_BEGIN("canfd init channel 1");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_1);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Channel-1 register block; arm the same real-poll seam (T1-01) so the
   * clock handshake + mode-transition acks succeed on the first poll. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(ra8_sys_canfdckcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDGSTS, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));
  const ra8_err_t err = ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_1);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("canfd init channel 1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_channel_bad(void)
{
  TEST_BEGIN("canfd init bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_canfd_init((uint8_t)k_ra8_canfd_test_channel_bad));
  TEST_END("canfd init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_happy(void)
{
  TEST_BEGIN("canfd deinit happy");
  ra8_fake_mmap_reset();

  const ra8_err_t err = ra8_canfd_deinit((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_chmdc_reset, (reg->CFDC[0].CTR & k_ra8_cnctr_mask_chmdc));
  TEST_END("canfd deinit happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_bad_channel(void)
{
  TEST_BEGIN("canfd deinit bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_canfd_deinit((uint8_t)k_ra8_canfd_test_channel_bad));
  TEST_END("canfd deinit bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_500k_happy(void)
{
  TEST_BEGIN("canfd set_bitrate 500k happy");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* set_bitrate drives CH_RESET -> program NCFG -> CH_OPERATION; both
   * channel-mode acks (CRSTSTS SET, then CRSTSTS|CHLTSTS CLEAR) on
   * CFDC[0].STS run for real on host now (T1-01). satisfy_after(0) acks
   * both on the first poll. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));

  const ra8_err_t err = ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                              (uint32_t)k_ra8_test_bitrate_500k,
                                              0U);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  TEST_ASSERT(reg->CFDC[0].NCFG != 0U);
  TEST_END("canfd set_bitrate 500k happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_250k_with_fd(void)
{
  TEST_BEGIN("canfd set_bitrate 250k nominal + 1M data");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* set_bitrate drives the real CH_RESET/CH_OPERATION channel-mode acks
   * on CFDC[0].STS now (T1-01); satisfy_after(0) acks both on poll 0. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&reg->CFDC[0].STS, 0U));

  const ra8_err_t err = ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                              (uint32_t)k_ra8_test_bitrate_250k,
                                              (uint32_t)k_ra8_test_bitrate_1m);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  TEST_ASSERT(reg->CFDC[0].NCFG != 0U);
  TEST_ASSERT(reg->CFDC2[0].DCFG != 0U);
  TEST_END("canfd set_bitrate 250k nominal + 1M data");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_zero_rejected(void)
{
  TEST_BEGIN("canfd set_bitrate rejects zero");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_zero,
                                       0U));
  TEST_END("canfd set_bitrate rejects zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_invalid_resolve(void)
{
  TEST_BEGIN("canfd set_bitrate rejects unresolvable rate");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_invalid,
                                       0U));
  TEST_END("canfd set_bitrate rejects unresolvable rate");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_prescaler_too_big(void)
{
  TEST_BEGIN("canfd set_bitrate rejects rate needing prescaler > 1024");
  ra8_fake_mmap_reset();

  /* 1 bps with 8 MHz PCLKA requires a prescaler of ~1 million, well
   * outside the 1..1024 nominal window, so the solver exhausts every
   * tq and returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0, 1U, 0U));
  TEST_END("canfd set_bitrate rejects rate needing prescaler > 1024");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_bad_data_rate(void)
{
  TEST_BEGIN("canfd set_bitrate rejects bad data rate");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  /* Nominal 250k resolves fine, so set_bitrate reaches the real CH_RESET
   * channel-mode ack before the data-phase solve; arm the seam
   * (satisfy_after 0) so that wait passes on host (T1-01) and the
   * rejection comes from the unresolvable data-phase 1234567 (which will
   * not divide evenly into 8 MHz), not an hw_timeout. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_satisfy_after(&ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0)->CFDC[0].STS, 0U));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_0,
                                       (uint32_t)k_ra8_test_bitrate_250k,
                                       (uint32_t)k_ra8_test_bitrate_bad_data));
  TEST_END("canfd set_bitrate rejects bad data rate");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_bitrate_bad_channel(void)
{
  TEST_BEGIN("canfd set_bitrate rejects bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_set_bitrate((uint8_t)k_ra8_canfd_test_channel_bad,
                                       (uint32_t)k_ra8_test_bitrate_500k,
                                       0U));
  TEST_END("canfd set_bitrate rejects bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_standard_frame_happy(void)
{
  TEST_BEGIN("canfd transmit standard frame happy");
  ra8_fake_mmap_reset();

  ra8_canfd_frame_t frame = {};
  frame.id                = (uint32_t)k_ra8_test_std_id;
  frame.dlc               = 8U;
  frame.is_extended       = 0U;
  frame.is_fd             = 0U;
  frame.is_brs            = 0U;
  for (uint8_t i = 0U; i < 8U; i++) {
    frame.data[i] = (uint8_t)(i + 1U);
  }

  const ra8_err_t err = ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_EQ(k_ra8_test_std_id, reg->CFDTM[0].ID);
  /* CFDTMC[0] is a single byte; TMTR = bit 0 -> value 1. */
  TEST_ASSERT_EQ(k_ra8_canfd_tmc_txreq, reg->CFDTMC[0]);
  /* Payload bytes land directly in CFDTM[0].DF[]. */
  TEST_ASSERT_EQ(1, reg->CFDTM[0].DF[0]);
  TEST_ASSERT_EQ(8, reg->CFDTM[0].DF[7]);
  TEST_END("canfd transmit standard frame happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the retry and
 * full-budget legs of the best-effort TMTRF completion spin in
 * ra8_canfd_transmit. The spin is fire-and-forget by design: it never
 * converts exhaustion into an error, so both legs return k_ra8_ok.)
 */
static void test_transmit_tmtrf_spin_legs(void)
{
  TEST_BEGIN("canfd transmit TMTRF spin retry / full-budget legs");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  ra8_canfd_frame_t frame = {};
  frame.id                = (uint32_t)k_ra8_test_std_id;
  frame.dlc               = 8U;
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL(reg);

  /* Retry leg: TMTRF asserts "done" on the 3rd poll. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_satisfy_after((const volatile void*)&reg->CFDTMSTS[0], 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  ra8_fake_mmio_reset();

  /* Full-budget leg: TMTRF never asserts; the bounded spin exhausts
   * and ra8_canfd_transmit still reports success (best-effort wait). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->CFDTMSTS[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  ra8_fake_mmio_reset();

  TEST_END("canfd transmit TMTRF spin retry / full-budget legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_extended_fd_frame(void)
{
  TEST_BEGIN("canfd transmit extended CAN-FD frame");
  ra8_fake_mmap_reset();

  ra8_canfd_frame_t frame = {};
  frame.id                = (uint32_t)k_ra8_test_ext_id;
  frame.dlc               = k_canfd_test_dlc_bad;
  frame.is_extended       = 1U;
  frame.is_fd             = 1U;
  frame.is_brs            = 1U;

  const ra8_err_t err = ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_1, &frame);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_1);
  TEST_ASSERT((reg->CFDTM[0].ID & k_ra8_canfd_id_ide) != 0U);
  TEST_ASSERT((reg->CFDTM[0].FDCTR & k_ra8_canfd_fd_fdf) != 0U);
  TEST_ASSERT((reg->CFDTM[0].FDCTR & k_ra8_canfd_fd_brs) != 0U);
  TEST_END("canfd transmit extended CAN-FD frame");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_null_frame(void)
{
  TEST_BEGIN("canfd transmit rejects NULL frame");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, nullptr));
  TEST_END("canfd transmit rejects NULL frame");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_bad_channel(void)
{
  TEST_BEGIN("canfd transmit rejects bad channel");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t frame = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_bad, &frame));
  TEST_END("canfd transmit rejects bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_bad_dlc(void)
{
  TEST_BEGIN("canfd transmit rejects DLC > 15");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t frame = {};
  frame.dlc               = 16U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects DLC > 15");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_oversized_std_id(void)
{
  TEST_BEGIN("canfd transmit rejects 11-bit overflow");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t frame = {};
  frame.id                = (uint32_t)k_ra8_test_oversized_std_id;
  frame.is_extended       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects 11-bit overflow");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_oversized_ext_id(void)
{
  TEST_BEGIN("canfd transmit rejects 29-bit overflow");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t frame = {};
  frame.id                = (uint32_t)k_ra8_test_invalid_ext_id;
  frame.is_extended       = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects 29-bit overflow");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transmit_brs_without_fd(void)
{
  TEST_BEGIN("canfd transmit rejects BRS without FD");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t frame = {};
  frame.is_fd             = 0U;
  frame.is_brs            = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_canfd_transmit((uint8_t)k_ra8_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects BRS without FD");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_empty_fifo(void)
{
  TEST_BEGIN("canfd receive returns no_data on empty FIFO");
  ra8_fake_mmap_reset();

  /* Seed RFEMP so the driver reads empty. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;

  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_canfd_receive((uint8_t)k_ra8_canfd_test_channel_0, &out));
  TEST_END("canfd receive returns no_data on empty FIFO");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_standard_frame(void)
{
  TEST_BEGIN("canfd receive decodes standard frame");
  ra8_fake_mmap_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  reg->CFDRFSTS[0]        = 0U; /* not empty */
  reg->CFDRF[0].ID        = (uint32_t)k_ra8_test_std_id;
  reg->CFDRF[0].PTR       = (uint32_t)(8UL << k_canfd_test_shift_dlc); /* DLC = 8 */
  reg->CFDRF[0].FDSTS     = 0U;
  reg->CFDRF[0].DF[0]     = k_canfd_test_df0;
  reg->CFDRF[0].DF[1]     = k_canfd_test_df1;
  reg->CFDRF[0].DF[2]     = k_canfd_test_df2;
  reg->CFDRF[0].DF[3]     = k_canfd_test_df3;

  ra8_canfd_frame_t out = {};
  const ra8_err_t   err = ra8_canfd_receive((uint8_t)k_ra8_canfd_test_channel_0, &out);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_ra8_test_std_id, out.id);
  TEST_ASSERT_EQ(0, out.is_extended);
  TEST_ASSERT_EQ(8, out.dlc);
  TEST_ASSERT_EQ(0x11, out.data[0]);
  TEST_ASSERT_EQ(0x22, out.data[1]);
  TEST_ASSERT_EQ(0x33, out.data[2]);
  TEST_ASSERT_EQ(0x44, out.data[3]);
  TEST_END("canfd receive decodes standard frame");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_extended_fd_frame(void)
{
  TEST_BEGIN("canfd receive decodes extended FD frame");
  ra8_fake_mmap_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_1);
  reg->CFDRFSTS[0]        = 0U;
  reg->CFDRF[0].ID        = (uint32_t)k_ra8_test_ext_id | (uint32_t)k_ra8_canfd_id_ide;
  reg->CFDRF[0].PTR       = (uint32_t)(k_canfd_test_dlc_bad << k_canfd_test_shift_dlc);
  reg->CFDRF[0].FDSTS     = (uint32_t)k_ra8_canfd_fd_fdf | (uint32_t)k_ra8_canfd_fd_brs;

  ra8_canfd_frame_t out = {};
  const ra8_err_t   err = ra8_canfd_receive((uint8_t)k_ra8_canfd_test_channel_1, &out);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(1, out.is_extended);
  TEST_ASSERT_EQ(k_ra8_test_ext_id, out.id);
  TEST_ASSERT_EQ(1, out.is_fd);
  TEST_ASSERT_EQ(1, out.is_brs);
  TEST_ASSERT_EQ(15, out.dlc);
  TEST_END("canfd receive decodes extended FD frame");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_null_out(void)
{
  TEST_BEGIN("canfd receive rejects NULL out");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_receive((uint8_t)k_ra8_canfd_test_channel_0, nullptr));
  TEST_END("canfd receive rejects NULL out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_bad_channel(void)
{
  TEST_BEGIN("canfd receive rejects bad channel");
  ra8_fake_mmap_reset();
  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_receive((uint8_t)k_ra8_canfd_test_channel_bad, &out));
  TEST_END("canfd receive rejects bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_error_state_happy(void)
{
  TEST_BEGIN("canfd get_error_state happy");
  ra8_fake_mmap_reset();

  /* TEC[31:24] = 0x55, REC[23:16] = 0xAA in CFDC[0].STS. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_ra8_canfd_test_channel_0);
  reg->CFDC[0].STS        = (uint32_t)k_ra8_test_sts_encoded;

  uint8_t         tx_err = 0U;
  uint8_t         rx_err = 0U;
  const ra8_err_t err =
    ra8_canfd_get_error_state((uint8_t)k_ra8_canfd_test_channel_0, &tx_err, &rx_err);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_ra8_test_expected_tec, tx_err);
  TEST_ASSERT_EQ(k_ra8_test_expected_rec, rx_err);
  TEST_END("canfd get_error_state happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_error_state_null_tx(void)
{
  TEST_BEGIN("canfd get_error_state rejects NULL tx_err");
  ra8_fake_mmap_reset();

  uint8_t rx_err = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_get_error_state((uint8_t)k_ra8_canfd_test_channel_0, nullptr, &rx_err));
  TEST_END("canfd get_error_state rejects NULL tx_err");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_error_state_null_rx(void)
{
  TEST_BEGIN("canfd get_error_state rejects NULL rx_err");
  ra8_fake_mmap_reset();

  uint8_t tx_err = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_canfd_get_error_state((uint8_t)k_ra8_canfd_test_channel_0, &tx_err, nullptr));
  TEST_END("canfd get_error_state rejects NULL rx_err");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_error_state_bad_channel(void)
{
  TEST_BEGIN("canfd get_error_state rejects bad channel");
  ra8_fake_mmap_reset();

  uint8_t tx_err = 0U;
  uint8_t rx_err = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_canfd_get_error_state((uint8_t)k_ra8_canfd_test_channel_bad, &tx_err, &rx_err));
  TEST_END("canfd get_error_state rejects bad channel");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_channel0_happy,
  test_init_channel0_timeout,
  test_init_channel1,
  test_init_channel_bad,
  test_deinit_happy,
  test_deinit_bad_channel,
  test_set_bitrate_500k_happy,
  test_set_bitrate_250k_with_fd,
  test_set_bitrate_zero_rejected,
  test_set_bitrate_invalid_resolve,
  test_set_bitrate_prescaler_too_big,
  test_set_bitrate_bad_data_rate,
  test_set_bitrate_bad_channel,
  test_transmit_standard_frame_happy,
  test_transmit_tmtrf_spin_legs,
  test_transmit_extended_fd_frame,
  test_transmit_null_frame,
  test_transmit_bad_channel,
  test_transmit_bad_dlc,
  test_transmit_oversized_std_id,
  test_transmit_oversized_ext_id,
  test_transmit_brs_without_fd,
  test_receive_empty_fifo,
  test_receive_standard_frame,
  test_receive_extended_fd_frame,
  test_receive_null_out,
  test_receive_bad_channel,
  test_get_error_state_happy,
  test_get_error_state_null_tx,
  test_get_error_state_null_rx,
  test_get_error_state_bad_channel,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_canfd.c\n");
  return 0;
}

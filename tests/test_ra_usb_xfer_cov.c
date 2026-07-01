/**
 * @file test_ra_usb_xfer_cov.c
 * @brief Coverage top-up for the USB device-mode data path (ra_usb_xfer.c)
 *
 * @details
 * Targets the residual uncovered lines in ``libs/ra_hal/src/ra_usb_xfer.c``
 * that the primary suite (``test_ra_usb.c``) does not reach:
 *   - ``ra_usb_dcp_in_data`` bogus-speed rejection (the ``internal_pick``
 *     NULL leg that also stamps the JLink diagnostic ::s_dcp_last_err).
 *   - ``ra_usb_queue_out`` zero-length-packet (ZLP) drain leg: BRDY was
 *     latched but the FIFO holds DTLN == 0 bytes, so the bank is released
 *     via BCLR and ``k_ra_err_no_data`` is returned.
 *   - ``ra_usb_rearm_out_pipe`` and ``ra_usb_park_out_pipe`` in full
 *     (valid re-arm/park plus the speed and pipe-range rejections),
 *     including MC/DC vectors for the pipe-range compound decision.
 *
 * Every leg is driven deterministically by pre-seeding the simulator's
 * register RAM (BRDYSTS / CFIFOCTR / NRDYSTS / PIPECTR); no timing
 * injection (SIGALRM) is used. ``internal_wait_frdy`` returns success
 * unconditionally under RA_SIMULATOR_MODE, so the FRDY-timeout legs are
 * unreachable from the host and are branch-excluded in the source.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "unity_minimal.h"

/*
 * JLink-readable diagnostic latch defined (external linkage) in
 * ra_usb_xfer.c; 2U marks the "NULL arg / bogus speed" rejection leg.
 */
extern volatile uint8_t s_dcp_last_err;

typedef enum : uint16_t {
  k_test_usb_speed_bogus = 9U,   /**< Neither FS nor HS -> internal_pick NULL. */
  k_test_usb_pipe_ok     = 1U,   /**< 1 .. k_ra_usb_max_pipe_num.              */
  k_test_usb_pipe_lo_bad = 0U,   /**< pipe_num == 0 -> rejected.               */
  k_test_usb_pipe_hi_bad = 99U,  /**< pipe_num > k_ra_usb_max_pipe_num.        */
  k_test_usb_dcp_err_arg = 2U,   /**< s_dcp_last_err value for the NULL leg.   */
  k_test_usb_pipe1_bit   = 0x02U /**< 1U << 1 : PIPE1 status-register bit.     */
} test_usb_xfer_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @test test_dcp_in_data_bogus_speed
 *
 * @par MC/DC:
 * (no compound decision on this leg -- ``if (reg == nullptr)`` is a
 * single condition reached by a speed that ``internal_pick`` cannot
 * map to a controller block)
 *
 * @details Drives the ``ra_usb_dcp_in_data`` speed-rejection leg
 * (``internal_pick`` returns NULL), asserting both the return code and
 * the ::s_dcp_last_err diagnostic stamp so line coverage lands on the
 * error-store as well as the return.
 */
static void test_dcp_in_data_bogus_speed(void)
{
  TEST_BEGIN("ra_usb_dcp_in_data rejects bogus speed and stamps s_dcp_last_err");
  prep();

  s_dcp_last_err       = 0U;
  const uint8_t buf[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_in_data((ra_usb_speed_t)k_test_usb_speed_bogus, buf, 4U));
  /* The NULL-arg leg (line 238) stamps the diagnostic latch to 2. */
  TEST_ASSERT_EQ((uint8_t)k_test_usb_dcp_err_arg, s_dcp_last_err);

  TEST_END("ra_usb_dcp_in_data rejects bogus speed and stamps s_dcp_last_err");
}

/**
 * @test test_queue_out_zlp_drain
 *
 * @par MC/DC:
 * (no compound decision exercised here -- ``if (available == 0U)`` is a
 * single condition; the BRDYSTS fast-path guard is pinned true by
 * pre-seeding BRDYSTS so control reaches the FIFO drain)
 *
 * @details Pre-seeds BRDYSTS so the pipe's BRDY latch is set (skipping
 * the no-data fast path), then seeds CFIFOCTR with FRDY asserted but
 * DTLN == 0. The drain therefore observes a zero-length packet: it
 * releases the bank via CFIFOCTR.BCLR, zeroes the caller length, and
 * returns ::k_ra_err_no_data.
 */
static void test_queue_out_zlp_drain(void)
{
  TEST_BEGIN("ra_usb_queue_out ZLP leg releases bank via BCLR and reports no_data");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_device_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg    = ra_usb_fs();
  uint8_t                out[8] = {};
  uint16_t               len    = 8U;

  /* BRDY latched for PIPE1 -> bypass the no_data fast path; FRDY set with
   * DTLN == 0 -> the drain sees an empty (zero-length) bank. */
  reg->BRDYSTS  = (uint16_t)k_test_usb_pipe1_bit;
  reg->CFIFOCTR = (uint16_t)k_ra_fifoctr_frdy;

  TEST_ASSERT_EQ(k_ra_err_no_data,
                 ra_usb_queue_out(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok, out, &len, true));
  TEST_ASSERT_EQ(0U, len);
  /* The ZLP leg writes CFIFOCTR.BCLR to release the empty bank. */
  TEST_ASSERT((reg->CFIFOCTR & (uint16_t)k_ra_fifoctr_bclr) != 0U);

  TEST_END("ra_usb_queue_out ZLP leg releases bank via BCLR and reports no_data");
}

/**
 * @test test_rearm_out_pipe_valid_and_speed
 *
 * @par MC/DC:
 * (no compound decision on these legs -- the valid path and the
 * single-condition ``if (reg == nullptr)`` speed rejection)
 *
 * @details Exercises the happy path (NRDYSTS acked, PID forced to BUF)
 * and the speed-rejection leg. NRDYSTS is pre-seeded all-ones so the
 * post-condition proves the target bit was cleared by the full-register
 * store ``NRDYSTS = ~pipe_bit``.
 */
static void test_rearm_out_pipe_valid_and_speed(void)
{
  TEST_BEGIN("ra_usb_rearm_out_pipe acks NRDYSTS + forces PID=BUF, rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_device_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->NRDYSTS               = (uint16_t)0xFFFFU;
  reg->PIPECTR[0]            = (uint16_t)k_ra_pid_nak;

  TEST_ASSERT_EQ(k_ra_ok, ra_usb_rearm_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* W0C store leaves every bit set except the target PIPE1 bit. */
  TEST_ASSERT_EQ((uint16_t)~(uint16_t)k_test_usb_pipe1_bit, reg->NRDYSTS);
  /* PID field forced to BUF so the next host OUT token is ACKed. */
  TEST_ASSERT_EQ((uint16_t)k_ra_pid_buf, (uint16_t)(reg->PIPECTR[0] & (uint16_t)k_ra_pid_mask));

  /* Speed that internal_pick cannot map -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_usb_rearm_out_pipe((ra_usb_speed_t)k_test_usb_speed_bogus, (uint8_t)k_test_usb_pipe_ok));

  TEST_END("ra_usb_rearm_out_pipe acks NRDYSTS + forces PID=BUF, rejects bogus speed");
}

/**
 * @test test_mcdc_rearm_out_pipe_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num))`
 * (ra_usb_rearm_out_pipe, 2 conditions).
 * - V1: pipe=1  -> C1 false, C2 false -> false (control: accepted).
 * - V2: pipe=0  -> C1 true            -> true  (varies C1 only).
 * - V3: pipe=99 -> C1 false, C2 true  -> true  (varies C2 only).
 * V1+V2 prove C1 (pipe==0) independently flips the decision; V1+V3
 * prove C2 (pipe>max). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_rearm_out_pipe_pipe_num(void)
{
  TEST_BEGIN("mcdc: ra_usb_rearm_out_pipe pipe_num decision");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_device_init(k_ra_usb_speed_fs));

  /* V1: both conditions false -> accepted. */
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_rearm_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* V2: pipe == 0 -> rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_rearm_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_lo_bad));
  /* V3: pipe > max -> rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_rearm_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_hi_bad));

  TEST_END("mcdc: ra_usb_rearm_out_pipe pipe_num decision");
}

/**
 * @test test_park_out_pipe_valid_and_speed
 *
 * @par MC/DC:
 * (no compound decision on these legs -- the valid path and the
 * single-condition ``if (reg == nullptr)`` speed rejection)
 *
 * @details Exercises the happy path (PID forced to NAK) and the
 * speed-rejection leg. PIPECTR is pre-seeded with PID=BUF so the
 * post-condition proves the field was overwritten to NAK.
 */
static void test_park_out_pipe_valid_and_speed(void)
{
  TEST_BEGIN("ra_usb_park_out_pipe forces PID=NAK, rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_device_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->PIPECTR[0]            = (uint16_t)k_ra_pid_buf;

  TEST_ASSERT_EQ(k_ra_ok, ra_usb_park_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* PID field forced to NAK so subsequent host OUT tokens are NAKed. */
  TEST_ASSERT_EQ((uint16_t)k_ra_pid_nak, (uint16_t)(reg->PIPECTR[0] & (uint16_t)k_ra_pid_mask));

  /* Speed that internal_pick cannot map -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_usb_park_out_pipe((ra_usb_speed_t)k_test_usb_speed_bogus, (uint8_t)k_test_usb_pipe_ok));

  TEST_END("ra_usb_park_out_pipe forces PID=NAK, rejects bogus speed");
}

/**
 * @test test_mcdc_park_out_pipe_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num))`
 * (ra_usb_park_out_pipe, 2 conditions).
 * - V1: pipe=1  -> C1 false, C2 false -> false (control: accepted).
 * - V2: pipe=0  -> C1 true            -> true  (varies C1 only).
 * - V3: pipe=99 -> C1 false, C2 true  -> true  (varies C2 only).
 * V1+V2 prove C1 (pipe==0) independently flips the decision; V1+V3
 * prove C2 (pipe>max). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_park_out_pipe_pipe_num(void)
{
  TEST_BEGIN("mcdc: ra_usb_park_out_pipe pipe_num decision");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_device_init(k_ra_usb_speed_fs));

  /* V1: both conditions false -> accepted. */
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_park_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* V2: pipe == 0 -> rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_park_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_lo_bad));
  /* V3: pipe > max -> rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_park_out_pipe(k_ra_usb_speed_fs, (uint8_t)k_test_usb_pipe_hi_bad));

  TEST_END("mcdc: ra_usb_park_out_pipe pipe_num decision");
}

int32_t main(void)
{
  test_dcp_in_data_bogus_speed();
  test_queue_out_zlp_drain();
  test_rearm_out_pipe_valid_and_speed();
  test_mcdc_rearm_out_pipe_pipe_num();
  test_park_out_pipe_valid_and_speed();
  test_mcdc_park_out_pipe_pipe_num();
  (void)fprintf(stderr, "[OK ] test_ra_usb_xfer_cov.c\n");
  return 0;
}

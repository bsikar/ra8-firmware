/**
 * @file test_ra8_usb_xfer_cov.c
 * @brief Coverage top-up for the USB device-mode data path (ra8_usb_xfer.c)
 *
 * @details
 * Targets the residual uncovered lines in ``libs/ra8_hal/src/ra8_usb_xfer.c``
 * that the primary suite (``test_ra8_usb.c``) does not reach:
 *   - ``ra8_usb_dcp_in_data`` bogus-speed rejection (the ``priv_pick``
 *     NULL leg that also stamps the JLink diagnostic ::g_dcp_last_err).
 *   - ``ra8_usb_queue_out`` zero-length-packet (ZLP) drain leg: BRDY was
 *     latched but the FIFO holds DTLN == 0 bytes, so the bank is released
 *     via BCLR and ``k_ra8_err_no_data`` is returned.
 *   - ``ra8_usb_rearm_out_pipe`` and ``ra8_usb_park_out_pipe`` in full
 *     (valid re-arm/park plus the speed and pipe-range rejections),
 *     including MC/DC vectors for the pipe-range compound decision.
 *
 * Every leg is driven deterministically by pre-seeding the fake's
 * register RAM (BRDYSTS / CFIFOCTR / NRDYSTS / PIPECTR); no timing
 * injection (SIGALRM) is used. The bounded FRDY wait in
 * ``priv_wait_frdy`` runs its real poll loop on the host and
 * consults the ra8_fake_mmio fault seam keyed on CFIFOCTR, so the
 * FRDY-timeout and retry legs are driven here by arming that seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_xfer_cov_t
 * @brief NRDYSTS pattern staged before the dispatcher runs.
 */
typedef enum : uint16_t {
  k_t_nrdysts_all = 0xFFFFU, /**< Every pipe's not-ready bit set, so the
                                  dispatcher must service each one.              */
} t_xfer_cov_t;

/*
 * JLink-readable diagnostic latch defined (external linkage) in
 * ra8_usb_xfer.c; 2U marks the "NULL arg / bogus speed" rejection leg.
 */
extern volatile uint8_t g_dcp_last_err;

typedef enum : uint16_t {
  k_test_usb_speed_bogus = 9U,   /**< Neither FS nor HS -> priv_pick NULL.   */
  k_test_usb_pipe_ok     = 1U,   /**< 1 .. k_ra8_usb_max_pipe_num.           */
  k_test_usb_pipe_lo_bad = 0U,   /**< pipe_num == 0 -> rejected.             */
  k_test_usb_pipe_hi_bad = 99U,  /**< pipe_num > k_ra8_usb_max_pipe_num.     */
  k_test_usb_dcp_err_arg = 2U,   /**< g_dcp_last_err value for the NULL leg. */
  k_test_usb_pipe1_bit   = 0x02U /**< 1U << 1 : PIPE1 status-register bit.   */
} test_usb_xfer_const_t;

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @test internal_test_dcp_in_data_bogus_speed
 *
 * @par MC/DC:
 * (no compound decision on this leg -- ``if (reg == nullptr)`` is a
 * single condition reached by a speed that ``priv_pick`` cannot
 * map to a controller block)
 *
 * @details Drives the ``ra8_usb_dcp_in_data`` speed-rejection leg
 * (``priv_pick`` returns NULL), asserting both the return code and
 * the ::g_dcp_last_err diagnostic stamp so line coverage lands on the
 * error-store as well as the return. @brief Verify dcp in data bogus speed behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dcp_in_data_bogus_speed(void)
{
  TEST_BEGIN("ra8_usb_dcp_in_data rejects bogus speed and stamps g_dcp_last_err");
  internal_prep();

  g_dcp_last_err       = 0U;
  const uint8_t buf[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_dcp_in_data((ra8_usb_speed_t)k_test_usb_speed_bogus, buf, 4U));
  /* The NULL-arg leg (line 238) stamps the diagnostic latch to 2. */
  TEST_ASSERT_EQ(k_test_usb_dcp_err_arg, g_dcp_last_err);

  TEST_END("ra8_usb_dcp_in_data rejects bogus speed and stamps g_dcp_last_err");
}

/**
 * @test internal_test_queue_out_zlp_drain
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
 * returns ::k_ra8_err_no_data. @brief Verify queue out zlp drain behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_queue_out_zlp_drain(void)
{
  TEST_BEGIN("ra8_usb_queue_out ZLP leg releases bank via BCLR and reports no_data");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg    = ra8_usb_fs();
  uint8_t                out[8] = {};
  uint16_t               len    = 8U;

  /* BRDY latched for PIPE1 -> bypass the no_data fast path; FRDY set with
   * DTLN == 0 -> the drain sees an empty (zero-length) bank. */
  reg->BRDYSTS  = (uint16_t)k_test_usb_pipe1_bit;
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;

  TEST_ASSERT_EQ(
    k_ra8_err_no_data,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok, out, &len, true));
  TEST_ASSERT_EQ(0U, len);
  /* The ZLP leg writes CFIFOCTR.BCLR to release the empty bank. */
  TEST_ASSERT((reg->CFIFOCTR & (uint16_t)k_ra8_fifoctr_bclr) != 0U);

  TEST_END("ra8_usb_queue_out ZLP leg releases bank via BCLR and reports no_data");
}

/**
 * @test internal_test_rearm_out_pipe_valid_and_speed
 *
 * @par MC/DC:
 * (no compound decision on these legs -- the valid path and the
 * single-condition ``if (reg == nullptr)`` speed rejection)
 *
 * @details Exercises the happy path (NRDYSTS acked, PID forced to BUF)
 * and the speed-rejection leg. NRDYSTS is pre-seeded all-ones so the
 * post-condition proves the target bit was cleared by the full-register
 * store ``NRDYSTS = ~pipe_bit``. @brief Verify rearm out pipe valid and speed behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_rearm_out_pipe_valid_and_speed(void)
{
  TEST_BEGIN("ra8_usb_rearm_out_pipe acks NRDYSTS + forces PID=BUF, rejects bogus speed");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->NRDYSTS               = (uint16_t)k_t_nrdysts_all;
  reg->PIPECTR[0]            = (uint16_t)k_ra8_pid_nak;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_rearm_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* W0C store leaves every bit set except the target PIPE1 bit. The
   * complement is truncated to the 16-bit NRDYSTS register width before
   * the compare (bare ~ would promote to a negative int). */
  const uint16_t nrdysts_expected = (uint16_t)~(uint16_t)k_test_usb_pipe1_bit;
  TEST_ASSERT_EQ(nrdysts_expected, reg->NRDYSTS);
  /* PID field forced to BUF so the next host OUT token is ACKed. */
  TEST_ASSERT_EQ(k_ra8_pid_buf, (reg->PIPECTR[0] & (uint16_t)k_ra8_pid_mask));

  /* Speed that priv_pick cannot map -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_rearm_out_pipe((ra8_usb_speed_t)k_test_usb_speed_bogus, (uint8_t)k_test_usb_pipe_ok));

  TEST_END("ra8_usb_rearm_out_pipe acks NRDYSTS + forces PID=BUF, rejects bogus speed");
}

/**
 * @test internal_test_mcdc_rearm_out_pipe_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num))`
 * (ra8_usb_rearm_out_pipe, 2 conditions).
 * - V1: pipe=1  -> C1 false, C2 false -> false (control: accepted).
 * - V2: pipe=0  -> C1 true            -> true  (varies C1 only).
 * - V3: pipe=99 -> C1 false, C2 true  -> true  (varies C2 only).
 * V1+V2 prove C1 (pipe==0) independently flips the decision; V1+V3
 * prove C2 (pipe>max). N+1 = 3 vectors for N=2. @brief Verify mcdc rearm out pipe pipe num behavior. @details Executes the mcdc rearm out pipe pipe num scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_rearm_out_pipe_pipe_num(void)
{
  TEST_BEGIN("mcdc: ra8_usb_rearm_out_pipe pipe_num decision");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  /* V1: both conditions false -> accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_rearm_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* V2: pipe == 0 -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_rearm_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_lo_bad));
  /* V3: pipe > max -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_rearm_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_hi_bad));

  TEST_END("mcdc: ra8_usb_rearm_out_pipe pipe_num decision");
}

/**
 * @test internal_test_park_out_pipe_valid_and_speed
 *
 * @par MC/DC:
 * (no compound decision on these legs -- the valid path and the
 * single-condition ``if (reg == nullptr)`` speed rejection)
 *
 * @details Exercises the happy path (PID forced to NAK) and the
 * speed-rejection leg. PIPECTR is pre-seeded with PID=BUF so the
 * post-condition proves the field was overwritten to NAK. @brief Verify park out pipe valid and speed behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_park_out_pipe_valid_and_speed(void)
{
  TEST_BEGIN("ra8_usb_park_out_pipe forces PID=NAK, rejects bogus speed");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->PIPECTR[0]            = (uint16_t)k_ra8_pid_buf;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_park_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* PID field forced to NAK so subsequent host OUT tokens are NAKed. */
  TEST_ASSERT_EQ(k_ra8_pid_nak, (reg->PIPECTR[0] & (uint16_t)k_ra8_pid_mask));

  /* Speed that priv_pick cannot map -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_park_out_pipe((ra8_usb_speed_t)k_test_usb_speed_bogus, (uint8_t)k_test_usb_pipe_ok));

  TEST_END("ra8_usb_park_out_pipe forces PID=NAK, rejects bogus speed");
}

/**
 * @test internal_test_mcdc_park_out_pipe_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num))`
 * (ra8_usb_park_out_pipe, 2 conditions).
 * - V1: pipe=1  -> C1 false, C2 false -> false (control: accepted).
 * - V2: pipe=0  -> C1 true            -> true  (varies C1 only).
 * - V3: pipe=99 -> C1 false, C2 true  -> true  (varies C2 only).
 * V1+V2 prove C1 (pipe==0) independently flips the decision; V1+V3
 * prove C2 (pipe>max). N+1 = 3 vectors for N=2. @brief Verify mcdc park out pipe pipe num behavior. @details Executes the mcdc park out pipe pipe num scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_park_out_pipe_pipe_num(void)
{
  TEST_BEGIN("mcdc: ra8_usb_park_out_pipe pipe_num decision");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  /* V1: both conditions false -> accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_park_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok));
  /* V2: pipe == 0 -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_park_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_lo_bad));
  /* V3: pipe > max -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_park_out_pipe(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_hi_bad));

  TEST_END("mcdc: ra8_usb_park_out_pipe pipe_num decision");
}

/**
 * @test internal_test_queue_in_frdy_timeout_and_retry
 *
 * @par MC/DC:
 * (no compound decisions under test -- the FRDY poll in
 * ``priv_wait_frdy`` is a single-condition loop exit; the armed
 * fail drives the timeout leg and the satisfy-after arm drives the
 * loop-continuation leg, each in isolation)
 *
 * @details Arms the ra8_fake_mmio seam on CFIFOCTR so the bounded FRDY
 * wait inside ``ra8_usb_queue_in`` runs to its budget and surfaces
 * ``k_ra8_err_hw_timeout`` -- the leg that was dead while
 * ``priv_wait_frdy`` short-circuited under RA8_OFF_TARGET.
 * Then re-arms with satisfy-after-2 so the wait converges on its third
 * poll, proving the retry path completes the queue (BVAL raised). @brief Verify queue in frdy timeout and retry behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_queue_in_frdy_timeout_and_retry(void)
{
  TEST_BEGIN("ra8_usb_queue_in FRDY timeout leg + satisfy-after retry leg");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  volatile r_usb_regs_t* reg = ra8_usb_fs();

  const uint8_t payload[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->CFIFOCTR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok, payload, 4U));

  /* Retry leg: FRDY "asserts" on the third poll; the queue completes. */
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after((const volatile void*)&reg->CFIFOCTR, 2U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok, payload, 4U));
  /* A completed queue raises BVAL on the staged bank. */
  TEST_ASSERT((reg->CFIFOCTR & (uint16_t)k_ra8_fifoctr_bval) != 0U);

  TEST_END("ra8_usb_queue_in FRDY timeout leg + satisfy-after retry leg");
}

/**
 * @test internal_test_dcp_in_data_frdy_timeouts
 *
 * @par MC/DC:
 * (no compound decisions under test -- both timeout legs are the
 * single-condition FRDY loop exit inside ``priv_wait_frdy``,
 * surfaced through ``priv_dcp_push_chunk`` for the payload leg and
 * ``internal_dcp_in_zlp`` for the zero-length leg)
 *
 * @details Arms the ra8_fake_mmio seam on CFIFOCTR to fail, then drives
 * ``ra8_usb_dcp_in_data`` twice: once with a payload (the chunk-push
 * FRDY timeout propagates through the ``chunk push failed`` leg) and
 * once with ``len == 0`` (the ZLP-path FRDY timeout). Both were dead
 * legs while ``priv_wait_frdy`` short-circuited on host. @brief Verify dcp in data frdy timeouts behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dcp_in_data_frdy_timeouts(void)
{
  TEST_BEGIN("ra8_usb_dcp_in_data FRDY timeout: chunk-push and ZLP legs");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  volatile r_usb_regs_t* reg = ra8_usb_fs();

  const uint8_t payload[4] = {0xA1U, 0xA2U, 0xA3U, 0xA4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->CFIFOCTR));
  /* Payload leg: priv_dcp_push_chunk's FRDY wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, payload, 4U));
  /* ZLP leg: internal_dcp_in_zlp's FRDY wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, nullptr, 0U));

  TEST_END("ra8_usb_dcp_in_data FRDY timeout: chunk-push and ZLP legs");
}

/**
 * @test internal_test_queue_out_frdy_timeout
 *
 * @par MC/DC:
 * (no compound decisions under test -- the BRDYSTS fast-path guard is
 * pinned true by pre-seeding BRDYSTS, and the FRDY poll that then
 * times out is a single-condition loop exit)
 *
 * @details Pre-seeds BRDYSTS so ``ra8_usb_queue_out`` passes its
 * no-data fast path, then arms the ra8_fake_mmio seam on CFIFOCTR to
 * fail so the drain's FRDY wait runs to its budget and returns
 * ``k_ra8_err_hw_timeout`` instead of touching the FIFO. @brief Verify queue out frdy timeout behavior. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_queue_out_frdy_timeout(void)
{
  TEST_BEGIN("ra8_usb_queue_out FRDY timeout leg after BRDY latch");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  volatile r_usb_regs_t* reg = ra8_usb_fs();

  uint8_t  out[8] = {};
  uint16_t len    = 8U;
  reg->BRDYSTS    = (uint16_t)k_test_usb_pipe1_bit;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->CFIFOCTR));
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_test_usb_pipe_ok, out, &len, true));

  TEST_END("ra8_usb_queue_out FRDY timeout leg after BRDY latch");
}

int32_t main(void)
{
  internal_test_dcp_in_data_bogus_speed();
  internal_test_queue_out_zlp_drain();
  internal_test_queue_in_frdy_timeout_and_retry();
  internal_test_dcp_in_data_frdy_timeouts();
  internal_test_queue_out_frdy_timeout();
  internal_test_rearm_out_pipe_valid_and_speed();
  internal_test_mcdc_rearm_out_pipe_pipe_num();
  internal_test_park_out_pipe_valid_and_speed();
  internal_test_mcdc_park_out_pipe_pipe_num();
  return 0;
}

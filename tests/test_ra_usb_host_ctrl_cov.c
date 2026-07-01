/**
 * @file test_ra_usb_host_ctrl_cov.c
 * @brief Coverage tests for the USB host control-transfer engine
 *        (`libs/ra_hal/src/ra_usb_host_ctrl.c`).
 *
 * @details
 * Drives every host-side-reachable path of `ra_usb_host_ctrl.c` against the
 * plain-RAM MMIO backing (`RA_SIMULATOR_MODE` + `ra_sim_mmap`): the
 * device-side DCP control-OUT arm / read helpers, the host control-transfer
 * SETUP-stage engine (argument rejection, SUREQ-busy abort on both
 * controllers, and the SETUP poll running to its spin bound), and the
 * bring-up stage diagnostic.
 *
 * The transfer's DATA / STATUS stages are deliberately NOT asserted here:
 * every stage past SETUP is gated on `internal_host_ctrl_setup` returning
 * `k_ra_ok`, which the polled engine only does after the SIE latches
 * `INTSTS1.SACK` (bit 4) -- a hardware edge the SIE raises when the device
 * ACKs the SETUP on the wire. The host simulator backs the register block
 * with ordinary RAM and models no SIE, and the driver itself W0C-clears
 * `INTSTS1.SACK` immediately before polling (ra_usb_host_ctrl.c), so the
 * bit is never observed set and the SETUP stage always runs to timeout.
 * The unreachable stages are marked `GCOVR_EXCL_*` in the driver with a
 * per-region justification; this suite covers everything the host CAN drive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "unity_minimal.h"

/** @brief Local bit / field constants (tests are magic-number exempt). */
typedef enum : uint16_t {
  k_dcp_pipe0_bit = 0x0001U,              /**< BRDYSTS / BEMPSTS DCP bit.      */
  k_sureq_bit     = (uint16_t)(1U << 14), /**< DCPCTR.SUREQ (host SETUP req).  */
  k_bogus_speed   = 9U,                   /**< Neither FS nor HS.              */
  k_drain_len     = 4U,                   /**< DTLN for the control-OUT drain. */
} test_host_ctrl_const_t;

/**
 * @brief Reset the MMIO backing store and re-init the module-stop map.
 *
 * @note Not thread-safe; single test context.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @test test_dcp_out_arm_arg_and_happy
 *
 * @par MC/DC:
 * Decision: `if (reg == nullptr)` in ra_usb_dcp_out_arm (1 condition).
 * - Vector 1: speed = FS -> reg non-NULL -> false (arm proceeds).
 * - Vector 2: speed = 9  -> reg == NULL  -> true  (rejected).
 * N+1 = 2 vectors for N=1. The HS vector additionally walks the
 * `internal_is_hs` MBW=32 select branch shared by the arm sequence.
 */
static void test_dcp_out_arm_arg_and_happy(void)
{
  TEST_BEGIN("ra_usb_dcp_out_arm: arg rejection + FS/HS arm sequence");
  prep();

  /* Bogus speed -> internal_pick returns NULL -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_usb_dcp_out_arm((ra_usb_speed_t)k_bogus_speed));

  /* FS arm: BRDYENB latches the DCP bit (line 557), so the readback
   * guard passes and PID is parked BUF. */
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_arm(k_ra_usb_speed_fs));
  volatile r_usb_regs_t* fs = ra_usb_fs();
  TEST_ASSERT((fs->BRDYENB & k_dcp_pipe0_bit) != 0U);
  TEST_ASSERT_EQ(k_ra_pid_buf, (uint16_t)(fs->DCPCTR & k_ra_pid_mask));
  /* BRDYSTS DCP latch was W0C-cleared (line 554). */
  TEST_ASSERT_EQ(0U, (uint16_t)(fs->BRDYSTS & k_dcp_pipe0_bit));

  /* HS arm: exercises the internal_is_hs MBW=32 CFIFO select path. */
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_arm(k_ra_usb_speed_hs));
  volatile r_usb_regs_t* hs = ra_usb_hs();
  TEST_ASSERT((hs->BRDYENB & k_dcp_pipe0_bit) != 0U);

  TEST_END("ra_usb_dcp_out_arm: arg rejection + FS/HS arm sequence");
}

/**
 * @test test_dcp_out_read_arg_validation
 *
 * @par MC/DC:
 * Decision: `if ((buf == nullptr) || (out_rx == nullptr))` in
 * ra_usb_dcp_out_read (2 conditions).
 * - Vector 1: buf=valid, out_rx=valid -> false (control: both false).
 * - Vector 2: buf=NULL,  out_rx=valid -> true  (varies C1 only).
 * - Vector 3: buf=valid, out_rx=NULL  -> true  (varies C2 only).
 * Vectors 1+2 prove buf independently flips the decision; 1+3 prove the
 * same for out_rx. N+1 = 3 vectors for N=2. The bogus-speed vector also
 * covers the earlier `if (reg == nullptr)` reject.
 */
static void test_dcp_out_read_arg_validation(void)
{
  TEST_BEGIN("ra_usb_dcp_out_read: speed + null-pointer rejection");
  prep();

  uint8_t  buf[8] = {};
  uint16_t rx     = 0U;

  /* Bogus speed -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read((ra_usb_speed_t)k_bogus_speed, buf, sizeof(buf), &rx));
  /* buf NULL (out_rx valid) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read(k_ra_usb_speed_fs, nullptr, sizeof(buf), &rx));
  /* out_rx NULL (buf valid) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_usb_dcp_out_read(k_ra_usb_speed_fs, buf, sizeof(buf), nullptr));

  TEST_END("ra_usb_dcp_out_read: speed + null-pointer rejection");
}

/**
 * @test test_dcp_out_read_no_data
 *
 * @par MC/DC:
 * Decision: `if ((reg->BRDYSTS & DCP_bit) == 0U)` (1 condition).
 * - Vector 1: BRDYSTS DCP bit clear -> true  -> no_data (this test).
 * - Vector 2: BRDYSTS DCP bit set   -> false -> drains
 *   (covered by test_dcp_out_read_drain_and_zlp).
 * N+1 = 2 vectors for N=1.
 */
static void test_dcp_out_read_no_data(void)
{
  TEST_BEGIN("ra_usb_dcp_out_read: BRDY not set -> no_data");
  prep();

  uint8_t  buf[8] = {};
  uint16_t rx     = 5U;
  /* DCP BRDY latch clear -> the OUT packet has not landed. */
  TEST_ASSERT_EQ(k_ra_err_no_data, ra_usb_dcp_out_read(k_ra_usb_speed_fs, buf, sizeof(buf), &rx));
  TEST_ASSERT_EQ(0U, rx);

  TEST_END("ra_usb_dcp_out_read: BRDY not set -> no_data");
}

/**
 * @test test_dcp_out_read_drain_and_zlp
 *
 * @par MC/DC:
 * Decision: `if (dtln == 0U)` selecting the ZLP vs data-drain leg
 * (1 condition).
 * - Vector 1: CFIFOCTR.DTLN = 4 -> false -> data drain (fifo read).
 * - Vector 2: CFIFOCTR.DTLN = 0 -> true  -> ZLP (BCLR only).
 * N+1 = 2 vectors for N=1. `internal_wait_frdy` returns k_ra_ok under
 * RA_SIMULATOR_MODE so both legs reach the DTLN decision.
 */
static void test_dcp_out_read_drain_and_zlp(void)
{
  TEST_BEGIN("ra_usb_dcp_out_read: data drain + zero-length packet");

  /* Data drain: DCP BRDY set, FRDY set, DTLN=4, CFIFO holds 0xBBAA. */
  prep();
  volatile r_usb_regs_t* fs = ra_usb_fs();
  fs->BRDYSTS               = k_dcp_pipe0_bit;
  fs->CFIFOCTR              = (uint16_t)(k_ra_fifoctr_frdy | k_drain_len);
  fs->CFIFO                 = (uint16_t)0xBBAAU;
  uint8_t  buf[8]           = {};
  uint16_t rx               = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_read(k_ra_usb_speed_fs, buf, sizeof(buf), &rx));
  TEST_ASSERT_EQ(k_drain_len, rx);
  TEST_ASSERT_EQ(0xAAU, buf[0]);
  TEST_ASSERT_EQ(0xBBU, buf[1]);
  /* BRDY was disabled + W0C-cleared before the drain (lines 611-612). */
  TEST_ASSERT_EQ(0U, (uint16_t)(fs->BRDYENB & k_dcp_pipe0_bit));

  /* Zero-length packet: DCP BRDY set, FRDY set, DTLN=0 -> BCLR path. */
  prep();
  fs           = ra_usb_fs();
  fs->BRDYSTS  = k_dcp_pipe0_bit;
  fs->CFIFOCTR = (uint16_t)k_ra_fifoctr_frdy;
  rx           = 7U;
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_dcp_out_read(k_ra_usb_speed_fs, buf, sizeof(buf), &rx));
  TEST_ASSERT_EQ(0U, rx);

  TEST_END("ra_usb_dcp_out_read: data drain + zero-length packet");
}

/**
 * @test test_host_control_xfer_arg_rejection
 *
 * @par MC/DC:
 * Two independent single-condition guards on the control-transfer entry:
 * `RA_CHECK_NULL_PTR(setup)` and `if (reg == nullptr)`.
 * - Vector 1: setup=NULL              -> null_ptr.
 * - Vector 2: setup=valid, speed=9    -> reg NULL -> invalid_arg.
 * The valid-setup / valid-speed false legs are covered by the
 * setup-busy / setup-timeout tests below.
 */
static void test_host_control_xfer_arg_rejection(void)
{
  TEST_BEGIN("ra_usb_host_control_xfer: null setup + bogus speed");
  prep();

  ra_usb_setup_t setup = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_usb_host_control_xfer(k_ra_usb_speed_fs, nullptr, nullptr, 0U, nullptr));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_usb_host_control_xfer((ra_usb_speed_t)k_bogus_speed, &setup, nullptr, 0U, nullptr));

  TEST_END("ra_usb_host_control_xfer: null setup + bogus speed");
}

/**
 * @test test_host_control_xfer_setup_busy
 *
 * @par MC/DC:
 * Decision: `if ((reg->DCPCTR & sureq) != 0U)` guarding the busy abort,
 * plus the nested `if (internal_is_hs(reg))` that selects the USBHS
 * SUREQCLR attempt.
 * - Vector 1: FS, SUREQ preset -> outer true, is_hs false -> busy after
 *   the (skipped) SUREQCLR.
 * - Vector 2: HS, SUREQ preset -> outer true, is_hs true  -> SUREQCLR
 *   write runs, SUREQ stays set in RAM -> busy.
 * Both return k_ra_err_busy because the plain-RAM sim cannot clear the
 * hardware-only SUREQ bit; the not-busy false leg is the timeout test.
 */
static void test_host_control_xfer_setup_busy(void)
{
  TEST_BEGIN("ra_usb_host_control_xfer: SUREQ-busy abort (FS + HS)");

  ra_usb_setup_t setup = {};

  /* FS: SUREQ already asserted -> busy (no USBHS SUREQCLR path). */
  prep();
  volatile r_usb_regs_t* fs = ra_usb_fs();
  fs->DCPCTR                = k_sureq_bit;
  TEST_ASSERT_EQ(k_ra_err_busy,
                 ra_usb_host_control_xfer(k_ra_usb_speed_fs, &setup, nullptr, 0U, nullptr));

  /* HS: SUREQ asserted -> SUREQCLR write executes but the RAM cell keeps
   * SUREQ set, so the abort still reports busy. */
  prep();
  volatile r_usb_regs_t* hs = ra_usb_hs();
  hs->DCPCTR                = k_sureq_bit;
  TEST_ASSERT_EQ(k_ra_err_busy,
                 ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr));

  TEST_END("ra_usb_host_control_xfer: SUREQ-busy abort (FS + HS)");
}

/**
 * @test test_host_control_xfer_setup_timeout
 *
 * @par MC/DC:
 * (no compound decision asserted here) -- drives the full SETUP stage:
 * DEVADD program, USBREQ/VAL/INDX/LENG load, SUREQ assert, and the SACK /
 * SIGN poll loop running to its spin bound. In the host sim the SIE never
 * latches INTSTS1.SACK, so the loop always returns k_ra_err_hw_timeout and
 * the stage diagnostic stays at "begin" (0).
 */
static void test_host_control_xfer_setup_timeout(void)
{
  TEST_BEGIN("ra_usb_host_control_xfer: SETUP stage runs to timeout");
  prep();

  volatile r_usb_regs_t* fs = ra_usb_fs();
  fs->DCPMAXP               = (uint16_t)k_ra_usb_dcp_max_packet_fs;

  ra_usb_setup_t setup  = {};
  setup.bm_request_type = (uint16_t)0x80U; /* device-to-host (control read). */
  setup.b_request       = (uint16_t)0x06U; /* GET_DESCRIPTOR.                */
  setup.w_value         = (uint16_t)0x0100U;
  setup.w_length        = (uint16_t)8U;

  uint8_t  data[8] = {};
  uint16_t got     = 0U;
  /* SETUP never gets a SACK from the (absent) SIE -> hw_timeout. */
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_usb_host_control_xfer(k_ra_usb_speed_fs, &setup, data, sizeof(data), &got));
  /* The transfer aborted in SETUP, so the stage diagnostic is "begin". */
  TEST_ASSERT_EQ(0U, ra_usb_host_ctrl_stage());

  /* USBREQ mirror registers were loaded from the SETUP packet. */
  TEST_ASSERT_EQ(0x0100U, fs->USBVAL);
  TEST_ASSERT_EQ(8U, fs->USBLENG);

  TEST_END("ra_usb_host_control_xfer: SETUP stage runs to timeout");
}

int32_t main(void)
{
  test_dcp_out_arm_arg_and_happy();
  test_dcp_out_read_arg_validation();
  test_dcp_out_read_no_data();
  test_dcp_out_read_drain_and_zlp();
  test_host_control_xfer_arg_rejection();
  test_host_control_xfer_setup_busy();
  test_host_control_xfer_setup_timeout();
  (void)fprintf(stderr, "[OK ] test_ra_usb_host_ctrl_cov.c\n");
  return 0;
}

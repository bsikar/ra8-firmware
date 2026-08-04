/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_usb_host_ctrl_cov.c
 * @brief Black-box line-coverage tests for the USB host control-transfer
 *        engine (libs/ra8_hal/src/ra8_usb_host_ctrl.c).
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Drives the polled host-mode control engine through its PUBLIC entry points
 * -- ``ra8_usb_host_control_xfer``, ``ra8_usb_dcp_out_arm``,
 * ``ra8_usb_dcp_out_read`` and the ``ra8_usb_host_ctrl_stage`` diagnostic getter
 * -- against the RAM-backed peripheral window installed by ``ra8_fake_mmap``.
 * These entry points link from the single production object in ``ra8_core_hal``,
 * so every line they execute is credited to the ONE shared production ``.gcda``
 * and merges cleanly into the aggregate gcovr report (unlike a renamed
 * ``#include`` copy, whose unique-reach lines never union into the production
 * counts).
 *
 * Because the host fake backs the USB register block with plain memory
 * (no write-1-to-clear / no SIE re-latch), the transfer state machine is
 * advanced deterministically by pre-seeding the status words each stage polls
 * BEFORE the call -- exactly the technique the sibling
 * ``test_ra8_usb_host_bulk_cov.c`` uses for the bulk engine:
 *
 *  - ``INTSTS1`` SACK / SIGN / neither selects the SETUP-ACK, transmit-error,
 *    and timeout legs of the seam ``internal_host_setup_wait`` honours under
 *    ``RA8_OFF_TARGET`` (it does NOT W0C-clear INTSTS1, so a pre-loaded
 *    outcome survives the assert-SUREQ read).
 *  - ``DCPCTR.SUREQ`` pre-set drives the FS + HS "a control transfer is already
 *    pending" busy-abort.
 *  - ``NRDYSTS`` DCP bit (with BRDYSTS clear) drives the NRDY re-arm + bounded
 *    timeout leg of the DATA-IN wait.
 *  - ``BRDYSTS`` DCP bit pre-seeded before a control-READ now SURVIVES the SETUP
 *    stage: ``internal_host_ctrl_setup`` models the BEMPSTS/BRDYSTS write-1-to-
 *    clear semantics under ``RA8_OFF_TARGET`` (preserve the DCP pipe bit,
 *    clear the rest), so ``internal_host_dcp_in_wait`` observes the edge and the
 *    DATA-IN receive body runs, the transfer completes, and the control-read
 *    OUT-ZLP status branch executes (stage advances to done).
 *  - ``BRDYSTS`` DCP bit + ``CFIFOCTR.DTLN`` + ``CFIFO`` drive the control-OUT
 *    drain in ``ra8_usb_dcp_out_read`` (no-data / ZLP / non-zero DTLN). That
 *    entry point has NO SETUP stage, so a pre-seeded BRDY edge and a NON-ZERO
 *    DTLN both survive; it is what exercises the ``internal_fifo_read`` copy of
 *    a real payload. The control-READ receive body reads its length from
 *    ``CFIFOCTR.DTLN``, but ``internal_host_ctrl_data_arm`` re-clears CFIFOCTR
 *    to BCLR (DTLN = 0) before the first read, so every control-read drains as
 *    a zero-length packet in the fake -- the non-zero copy / clamp of the same
 *    loop is credited through ``ra8_usb_dcp_out_read`` above.
 *
 * All waits are bounded by the module's ``k_ra8_usb_ctrl_poll_limit`` spin, so
 * every case terminates deterministically without any asynchronous injection
 * (no SIGALRM, no unbounded spin). The silicon-only BRDYENB read-back timeout in
 * ``ra8_usb_dcp_out_arm`` is the module's one GCOVR_EXCL_LINE and is intentionally
 * not driven here.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_hcc_payload_t
 * @brief Control-transfer payload bytes and the receive-count poison.
 *
 * @details
 * The 10-byte OUT payload is the ascending run 1..10, so a byte that lands at
 * the wrong FIFO offset is obvious; only the values the ignore-list does not
 * already cover need naming. The 3-byte payload uses a distinct pattern so the
 * two arms cannot be confused in a dump.
 */
typedef enum : uint16_t {
  k_t_out_b4    = 5U,      /**< Byte 4 of the ascending 10-byte payload. */
  k_t_out_b6    = 7U,      /**< Byte 6.                                  */
  k_t_out_b8    = 9U,      /**< Byte 8.                                  */
  k_t_out_b9    = 10U,     /**< Byte 9, the last.                        */
  k_t_short_b0  = 0xA1U,   /**< Byte 0 of the 3-byte payload.            */
  k_t_short_b1  = 0xB2U,   /**< Byte 1.                                  */
  k_t_short_b2  = 0xC3U,   /**< Byte 2.                                  */
  k_t_rx_poison = 0xFFFFU, /**< Pre-set received-byte count; a transfer that
                                  returns without writing it leaves this.      */
} t_hcc_payload_t;

/**
 * @enum thc_const_t
 * @brief Named register-seed and argument vectors for the host-ctrl suite.
 *
 * @details Tests are exempt from the magic-number gate; naming each seeded
 * value keeps the intent obvious at the call site. Bit vectors are derived
 * from the driver's own field enums so they cannot drift from the code.
 */
typedef enum : uint16_t {
  k_thc_speed_bogus = 9U,                                       /**< Not FS, not HS.              */
  k_thc_sack_bit    = (uint16_t)(1U << k_ra8_int1_bit_sack),    /**< INTSTS1 SETUP-ACK latch.     */
  k_thc_sign_bit    = (uint16_t)(1U << k_ra8_int1_bit_sign),    /**< INTSTS1 SETUP-fail latch.    */
  k_thc_sureq_bit   = (uint16_t)(1U << k_ra8_dcpctr_bit_sureq), /**< DCPCTR SUREQ (pending req).  */
  k_thc_dcp_bit     = (uint16_t)k_ra8_usb_dcp_pipe0_bit,        /**< BRDY/BEMP/NRDY DCP (pipe0).  */
  k_thc_dir_in      = (uint16_t)k_ra8_usb_setup_dir_in,         /**< bmRequestType device-to-host */
  k_thc_dir_out     = 0U,                                       /**< bmRequestType host-to-device */
  k_thc_mps_dcp     = 64U,                                      /**< DCPMAXP MXPS for a read.     */
  k_thc_mps_multi   = 4U,                                       /**< Tiny DCP MPS -> multi packet */
  k_thc_wlen_out    = 10U,                                      /**< DATA-OUT wLength (> mps).    */
  k_thc_wlen_step1  = 3U,                                       /**< DATA-OUT wLength (mxps==0).  */
  k_thc_wlen_in     = 64U,                                      /**< DATA-IN wLength (> buffer).  */
  k_thc_buf_in      = 8U,                                       /**< DATA-IN buffer capacity.     */
  k_thc_dtln        = 4U,                                       /**< CFIFOCTR.DTLN for the drain. */
  k_thc_cap         = 8U,                                       /**< dcp_out_read destination cap */
  k_thc_cfifo_seed  = 0xBBAAU,                                  /**< CFIFO data-port seed word.   */
  k_thc_breq_out    = 0x09U,                                    /**< A control-write bRequest.    */
  k_thc_breq_in     = 0x06U,                                    /**< A control-read bRequest.     */
} thc_const_t;

/**
 * @enum thc_stage_t
 * @brief Expected ::ra8_usb_host_ctrl_stage codes (mirror of the module's
 *        private stage enum, per the public ra8_usb_host.h contract).
 */
typedef enum : uint8_t {
  k_thc_stage_begin    = 0U, /**< Before / at the SETUP stage (setup failed). */
  k_thc_stage_data_in  = 2U, /**< DATA-IN stage entered.                      */
  k_thc_stage_data_pkt = 3U, /**< First DATA packet BRDY observed.            */
  k_thc_stage_status   = 5U, /**< STATUS stage entered.                       */
  k_thc_stage_done     = 6U, /**< STATUS done; the control transfer closed.   */
} thc_stage_t;

/**
 * @brief Reset the fake peripheral window before each test.
 *
 * @details Clears every mapped register region so a prior test's writes cannot
 * leak into the next, and disarms any ``ra8_fake_mmio`` wait faults a prior case
 * armed so an armed timeout cannot bleed into a later stage's bounded wait. The
 * host control engine needs no MSTP or device-init bring-up: ``internal_pick``
 * resolves a fixed controller base and the engine only touches that register
 * block.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
}

/**
 * @test test_control_xfer_guards
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * the ``setup`` NULL guard and the ``internal_pick`` NULL check are separate
 * single-condition guards, each driven in isolation; no ``&&`` or ``||``.)
 */
static void test_control_xfer_guards(void)
{
  TEST_BEGIN("control_xfer rejects NULL setup and a bogus speed");
  prep();

  ra8_usb_setup_t setup             = {};
  uint8_t         buf[k_thc_buf_in] = {};
  uint16_t        rx                = 0U;

  /* NULL setup -> RA8_CHECK_NULL_PTR. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, nullptr, buf, (uint16_t)k_thc_buf_in, &rx));
  /* Bogus speed -> internal_pick returns nullptr. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_control_xfer((ra8_usb_speed_t)k_thc_speed_bogus,
                                           &setup,
                                           buf,
                                           (uint16_t)k_thc_buf_in,
                                           &rx));

  TEST_END("control_xfer rejects NULL setup and a bogus speed");
}

/**
 * @test test_setup_error_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * ``internal_host_setup_wait`` tests the SACK and SIGN bits in separate
 * single-condition ``if`` statements; each leg is selected by pre-loading
 * INTSTS1, no ``&&`` or ``||``.)
 */
static void test_setup_error_legs(void)
{
  TEST_BEGIN("control_xfer surfaces the SETUP SIGN error and timeout legs");

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_buf_in,
  };
  uint8_t  buf[k_thc_buf_in] = {};
  uint16_t rx                = 0U;

  /* SIGN latched: three SETUP transmission attempts failed -> hw_error. Arm the
   * INTSTS1 SETUP-ACK poll to FAIL so the SACK wait does not spuriously succeed
   * on an unarmed fake seam (which would advance past SETUP and time out at a
   * later stage); the driver then reads the pre-seeded SIGN from RAM and reports
   * hw_error. The polled register is exactly the reg->INTSTS1 the SETUP wait
   * spins on. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sign_bit;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_usb_fs()->INTSTS1));
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &setup, buf, (uint16_t)k_thc_buf_in, &rx));
  /* SETUP failed before any stage advanced. */
  TEST_ASSERT_EQ(k_thc_stage_begin, ra8_usb_host_ctrl_stage());

  /* Neither SACK nor SIGN latched: arm the same INTSTS1 poll to fail so the
   * bounded SETUP wait terminates at the SETUP stage with hw_timeout instead of
   * the fake seam succeeding the SACK poll and advancing into a later stage. */
  prep();
  ra8_usb_fs()->INTSTS1 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_usb_fs()->INTSTS1));
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &setup, buf, (uint16_t)k_thc_buf_in, &rx));

  TEST_END("control_xfer surfaces the SETUP SIGN error and timeout legs");
}

/**
 * @test test_setup_sureq_busy_fs_hs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * the pending-SUREQ guard is a single-condition ``if``; the FS vs HS split
 * is a single-condition ``internal_is_hs`` branch that gates the USBHS
 * SUREQCLR abort. No ``&&`` or ``||``.)
 */
static void test_setup_sureq_busy_fs_hs(void)
{
  TEST_BEGIN("control_xfer aborts busy when DCPCTR.SUREQ is already set (FS + HS)");

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_buf_in,
  };
  uint8_t  buf[k_thc_buf_in] = {};
  uint16_t rx                = 0U;

  /* FS: SUREQ wedged, no SUREQCLR path -> busy. */
  prep();
  ra8_usb_fs()->DCPCTR = (uint16_t)k_thc_sureq_bit;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &setup, buf, (uint16_t)k_thc_buf_in, &rx));

  /* HS: the SUREQCLR abort is attempted, SUREQ stays set -> still busy. */
  prep();
  ra8_usb_hs()->DCPCTR = (uint16_t)k_thc_sureq_bit;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, buf, (uint16_t)k_thc_buf_in, &rx));

  TEST_END("control_xfer aborts busy when DCPCTR.SUREQ is already set (FS + HS)");
}

/**
 * @brief MC/DC V1: control-WRITE with a multi-packet data stage.
 * @param[in] data Payload buffer for the DATA-OUT stage.
 * @pre None (prepares its own controller state).
 * @post The transfer timed out at the IN status stage with rx == 0.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void thc_vector_write_data(uint8_t* data)
{
  /* V1: control-WRITE with a data stage -> DATA-OUT drives, IN status times out.
   * mps=4 with wLength=10 forces the multi-packet loop (chunk > step clamp). */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->DCPMAXP = (uint16_t)k_thc_mps_multi;
  ra8_usb_setup_t wr    = {
    .bm_request_type = (uint8_t)k_thc_dir_out,
    .b_request       = (uint8_t)k_thc_breq_out,
    .w_length        = (uint16_t)k_thc_wlen_out,
  };
  uint16_t rx = k_t_rx_poison;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &wr, data, (uint16_t)k_thc_wlen_out, &rx));
  TEST_ASSERT_EQ(0U, rx); /* DATA-OUT reports no received bytes. */
  TEST_ASSERT_EQ(k_thc_stage_status, ra8_usb_host_ctrl_stage());
}

/**
 * @brief MC/DC V3: control-READ entering the DATA-IN stage with a want clamp.
 * @pre None (prepares its own controller state).
 * @post The transfer reached the DATA-IN stage and timed out.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void thc_vector_read(void)
{
  /* V3: control-READ -> DATA-IN entered; NRDYSTS pre-seeded drives the NRDY
   * re-arm; wLength (64) > buffer (8) exercises the want clamp. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->NRDYSTS = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->DCPMAXP = (uint16_t)k_thc_mps_dcp;
  ra8_usb_setup_t rd    = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_wlen_in,
  };
  uint8_t  inbuf[k_thc_buf_in] = {};
  uint16_t rx                  = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &rd, inbuf, (uint16_t)k_thc_buf_in, &rx));
  TEST_ASSERT_EQ(k_thc_stage_data_in, ra8_usb_host_ctrl_stage());
}

/**
 * @test test_mcdc_data_phase_direction
 *
 * @par MC/DC:
 * Decision (``internal_host_data_phase`` DATA-OUT selector, 3 conditions):
 *   ``(setup->w_length > 0U) && !is_read && (data != nullptr)``
 * where ``is_read`` is true only for a device-to-host request with a
 * destination buffer. Short-circuit AND-chain, N+1 = 4 vectors:
 *   - V1 write, wLen>0, data!=NULL : C1=T, C2=T, C3=T -> decision T (DATA-OUT).
 *   - V2 no-data, wLen==0          : C1=F (short)      -> decision F (no stage).
 *   - V3 read,  wLen>0, data!=NULL : C1=T, C2=F (short)-> decision F (DATA-IN).
 *   - V4 write, wLen>0, data==NULL : C1=T, C2=T, C3=F  -> decision F (no stage).
 * V1 vs V2 isolates C1; V1 vs V3 isolates C2 (is_read flips !is_read); V1 vs
 * V4 isolates C3. Each vector is driven end-to-end through the public
 * ``ra8_usb_host_control_xfer`` with a SACK-seeded SETUP.
 *
 * @note V1's DATA-OUT completes (FRDY is forced OK off-target) but its IN status
 * stage then times out (the status stage clears BRDYSTS itself with no fake
 * seam, so no BRDY edge is reproducible there), so a control-write reports
 * hw_timeout at the status stage here; that is the deterministic fake outcome,
 * not a wire failure. V3 pre-seeds NO BRDYSTS edge, so its DATA-IN deliberately
 * takes the arm + NRDY re-arm + bounded-wait timeout leg; the BRDY-satisfied
 * receive body and the OUT-ZLP status stage are driven by
 * ``test_control_read_completes``.
 */
static void test_mcdc_data_phase_direction(void)
{
  TEST_BEGIN("control_xfer DATA-phase direction MC/DC (write / no-data / read / null)");

  uint8_t data[k_thc_wlen_out] =
    {1U, 2U, 3U, 4U, k_t_out_b4, 6U, k_t_out_b6, 8U, k_t_out_b8, k_t_out_b9};
  uint16_t rx = 0U;

  thc_vector_write_data(data);

  /* V2: no-data control (wLength == 0) -> no data stage, IN status times out. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_setup_t nd    = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &nd, data, 0U, &rx));
  TEST_ASSERT_EQ(k_thc_stage_status, ra8_usb_host_ctrl_stage());

  thc_vector_read();

  /* V4: control-WRITE with wLength > 0 but a NULL data pointer -> C3 false, no
   * data stage; IN status times out. Isolates the data!=NULL condition. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_setup_t wn    = {
    .bm_request_type = (uint8_t)k_thc_dir_out,
    .b_request       = (uint8_t)k_thc_breq_out,
    .w_length        = (uint16_t)k_thc_wlen_out,
  };
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &wn, nullptr, 0U, &rx));
  TEST_ASSERT_EQ(k_thc_stage_status, ra8_usb_host_ctrl_stage());

  TEST_END("control_xfer DATA-phase direction MC/DC (write / no-data / read / null)");
}

/**
 * @test test_control_write_mxps_zero
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * ``internal_host_ctrl_data_out``'s ``step = (mxps == 0U) ? 1U : mxps`` is a
 * single-condition ternary and ``chunk > step`` is a single-condition ``if``;
 * no ``&&`` or ``||``.)
 */
static void test_control_write_mxps_zero(void)
{
  TEST_BEGIN("control_xfer DATA-OUT falls back to step==1 when DCPMAXP.MXPS is 0");
  prep();

  /* DCPMAXP == 0 -> mxps == 0 -> the data-out loop uses a 1-byte step, so a
   * 3-byte payload drives three single-byte chunks (chunk > step clamp). */
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->DCPMAXP = 0U;

  ra8_usb_setup_t wr = {
    .bm_request_type = (uint8_t)k_thc_dir_out,
    .b_request       = (uint8_t)k_thc_breq_out,
    .w_length        = (uint16_t)k_thc_wlen_step1,
  };
  uint8_t  data[k_thc_wlen_step1] = {k_t_short_b0, k_t_short_b1, k_t_short_b2};
  uint16_t rx                     = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &wr, data, (uint16_t)k_thc_wlen_step1, &rx));

  TEST_END("control_xfer DATA-OUT falls back to step==1 when DCPMAXP.MXPS is 0");
}

/**
 * @test test_control_read_completes
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches -- the
 * DATA-IN receive loop and the OUT-ZLP status stage use single-condition
 * guards; the two loop-exit conditions ``dtln < mxps`` and ``rx >= want`` are
 * separate single-condition ``if`` statements, no ``&&`` or ``||``.)
 *
 * @details A control-READ with the DCP BRDYSTS bit pre-seeded now runs to
 * completion: ``internal_host_ctrl_setup`` preserves the pre-loaded DCP bit
 * (its RA8_OFF_TARGET write-1-to-clear seam), ``internal_host_dcp_in_wait``
 * observes it, and the receive body executes. ``internal_host_ctrl_data_arm``
 * re-clears CFIFOCTR to BCLR before the first ``CFIFOCTR.DTLN`` read, so DTLN
 * reads 0 and the packet drains as a zero-length read (rx == 0); the non-zero
 * ``internal_fifo_read`` copy of the same loop is credited via
 * ``ra8_usb_dcp_out_read``. Because the data stage completes, the control-read
 * OUT-ZLP status branch (``internal_host_ctrl_status`` with write_zlp == true)
 * runs, so the transfer closes k_ra8_ok and the stage advances to done.
 */
static void test_control_read_completes(void)
{
  TEST_BEGIN("control_xfer control-READ completes through the OUT-ZLP status stage");

  uint8_t  buf[k_thc_buf_in] = {};
  uint16_t rx                = 0U;

  /* Short / ZLP read: the pre-seeded BRDYSTS DCP bit survives SETUP, the
   * receive loop exits on dtln (0) < mxps, and the OUT-ZLP status stage
   * closes the transfer. wLength (8) < mxps (64) so a single packet ends it. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->BRDYSTS = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->DCPMAXP = (uint16_t)k_thc_mps_dcp;
  ra8_usb_setup_t rd    = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_buf_in,
  };
  rx = k_t_rx_poison;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &rd, buf, (uint16_t)k_thc_buf_in, &rx));
  TEST_ASSERT_EQ(0U, rx); /* DTLN forced to 0 by data_arm -> zero-length read. */
  TEST_ASSERT_EQ(k_thc_stage_done, ra8_usb_host_ctrl_stage());

  /* Exact / satisfied read: want == 0 (data_len 0) also drives the rx >= want
   * loop-exit condition; the transfer still closes through the OUT-ZLP status. */
  prep();
  ra8_usb_fs()->INTSTS1 = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->BRDYSTS = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->DCPMAXP = (uint16_t)k_thc_mps_dcp;
  ra8_usb_setup_t rd0   = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_buf_in,
  };
  rx = k_t_rx_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &rd0, buf, 0U, &rx));
  TEST_ASSERT_EQ(0U, rx);
  TEST_ASSERT_EQ(k_thc_stage_done, ra8_usb_host_ctrl_stage());

  TEST_END("control_xfer control-READ completes through the OUT-ZLP status stage");
}

/**
 * @test test_data_in_frdy_timeout
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * the FRDY poll in ``internal_wait_frdy`` is a single-condition loop
 * exit; the armed fail drives the DATA-IN drain's timeout leg)
 *
 * @details A control-READ whose SETUP succeeds (pre-seeded SACK) and whose
 * DCP BRDY edge is pre-seeded so ``internal_host_dcp_in_wait`` passes, but
 * with the ra8_fake_mmio seam armed on CFIFOCTR so the packet drain's FRDY
 * wait runs to its budget. The transfer must surface ``k_ra8_err_hw_timeout``
 * with the DCP parked NAK and the stage latched at the first DATA packet --
 * the leg that was host-dead while ``internal_wait_frdy`` short-circuited
 * under RA8_OFF_TARGET.
 */
static void test_data_in_frdy_timeout(void)
{
  TEST_BEGIN("control_xfer DATA-IN drain surfaces the FRDY timeout leg");
  prep();

  uint8_t  buf[k_thc_buf_in] = {};
  uint16_t rx                = k_t_rx_poison;
  ra8_usb_fs()->INTSTS1      = (uint16_t)k_thc_sack_bit;
  ra8_usb_fs()->BRDYSTS      = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->DCPMAXP      = (uint16_t)k_thc_mps_dcp;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_usb_fs()->CFIFOCTR));

  const ra8_usb_setup_t rd = {
    .bm_request_type = (uint8_t)k_thc_dir_in,
    .b_request       = (uint8_t)k_thc_breq_in,
    .w_length        = (uint16_t)k_thc_buf_in,
  };
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_control_xfer(k_ra8_usb_speed_fs, &rd, buf, (uint16_t)k_thc_buf_in, &rx));
  /* The failure was observed after the DATA packet's BRDY, at the drain. */
  TEST_ASSERT_EQ(k_thc_stage_data_pkt, ra8_usb_host_ctrl_stage());
  /* The DCP was parked NAK on the failure path. */
  TEST_ASSERT_EQ(k_ra8_pid_nak, (ra8_usb_fs()->DCPCTR & (uint16_t)k_ra8_pid_mask));

  TEST_END("control_xfer DATA-IN drain surfaces the FRDY timeout leg");
}

/**
 * @test test_dcp_out_arm_speeds
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * the ``reg == nullptr`` guard and the BRDYENB read-back check are separate
 * single-condition ``if`` statements; no ``&&`` or ``||``.)
 */
static void test_dcp_out_arm_speeds(void)
{
  TEST_BEGIN("dcp_out_arm rejects a bad speed and arms the DCP on FS + HS");

  /* Bogus speed -> internal_pick returns nullptr. */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_dcp_out_arm((ra8_usb_speed_t)k_thc_speed_bogus));

  /* FS: arm succeeds; BRDYENB carries the DCP bit and PID is BUF. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_out_arm(k_ra8_usb_speed_fs));
  TEST_ASSERT((ra8_usb_fs()->BRDYENB & (uint16_t)k_thc_dcp_bit) != 0U);
  TEST_ASSERT_EQ(k_ra8_pid_buf, (ra8_usb_fs()->DCPCTR & (uint16_t)k_ra8_pid_mask));

  /* HS: arm succeeds on the high-speed controller too. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_out_arm(k_ra8_usb_speed_hs));
  TEST_ASSERT((ra8_usb_hs()->BRDYENB & (uint16_t)k_thc_dcp_bit) != 0U);

  TEST_END("dcp_out_arm rejects a bad speed and arms the DCP on FS + HS");
}

/**
 * @test test_mcdc_dcp_out_read
 *
 * @par MC/DC:
 * Decision (``ra8_usb_dcp_out_read`` argument guard, 2 conditions):
 *   ``(buf == nullptr) || (out_rx == nullptr)`` -- short-circuit OR, N+1 = 3:
 *   - W1 buf!=NULL, out_rx!=NULL : C1=F, C2=F -> decision F (drain proceeds).
 *   - W2 buf==NULL              : C1=T (short) -> decision T (invalid_arg).
 *   - W3 buf!=NULL, out_rx==NULL : C1=F, C2=T  -> decision T (invalid_arg).
 * W1 vs W2 proves ``buf`` independently drives the outcome; W1 vs W3 proves
 * the same for ``out_rx``. The W1 (both non-NULL) path is exercised three ways
 * -- BRDY-absent no-data, a zero-length packet, and a non-zero DTLN drain --
 * so the receive-body branches (``dtln == 0`` release vs the FIFO copy) are
 * both reached without a SETUP stage clobbering the pre-seeded BRDY edge.
 */
static void test_mcdc_dcp_out_read(void)
{
  TEST_BEGIN("dcp_out_read: arg-guard MC/DC + no-data / ZLP / DTLN drain legs");

  uint8_t  buf[k_thc_cap] = {};
  uint16_t rx             = 0U;

  /* Bad speed -> internal_pick returns nullptr (before the arg guard). */
  prep();
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_dcp_out_read((ra8_usb_speed_t)k_thc_speed_bogus, buf, (uint16_t)k_thc_cap, &rx));

  /* W2: buf == NULL -> invalid_arg (first OR condition true). */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, nullptr, (uint16_t)k_thc_cap, &rx));

  /* W3: out_rx == NULL -> invalid_arg (second OR condition true). */
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, buf, (uint16_t)k_thc_cap, nullptr));

  /* W1 (a): both pointers valid but no BRDY edge -> the OUT packet has not
   * landed, so the drain reports no-data. */
  prep();
  ra8_usb_fs()->BRDYSTS = 0U;
  rx                    = k_t_rx_poison;
  TEST_ASSERT_EQ(k_ra8_err_no_data,
                 ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  TEST_ASSERT_EQ(0U, rx);

  /* W1 (b): BRDY set, DTLN == 0 -> a zero-length packet is released via BCLR. */
  prep();
  ra8_usb_fs()->BRDYSTS  = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->CFIFOCTR = 0U;
  rx                     = k_t_rx_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  TEST_ASSERT_EQ(0U, rx);

  /* W1 (c): BRDY set, DTLN == 4 (< cap) -> the packet drains through the
   * CFIFO and the host's byte count is reported. */
  prep();
  ra8_usb_fs()->BRDYSTS  = (uint16_t)k_thc_dcp_bit;
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_thc_dtln;
  ra8_usb_fs()->CFIFO    = (uint16_t)k_thc_cfifo_seed;
  rx                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  TEST_ASSERT_EQ(k_thc_dtln, rx);
  /* The one-shot DCP BRDY latch was acknowledged (cleared) after the drain. */
  TEST_ASSERT_EQ(0U, (ra8_usb_fs()->BRDYSTS & (uint16_t)k_thc_dcp_bit));

  TEST_END("dcp_out_read: arg-guard MC/DC + no-data / ZLP / DTLN drain legs");
}

/**
 * @test test_dcp_out_read_frdy_timeout
 *
 * @par MC/DC:
 * (no compound decisions in the code under test that this case touches --
 * the FRDY poll in ``internal_wait_frdy`` is a single-condition loop
 * exit; the armed fail drives the dcp_out_read timeout leg)
 *
 * @details Pre-seeds the DCP BRDY latch so the drain proceeds past the
 * no-data guard, then arms the ra8_fake_mmio seam on CFIFOCTR to fail so
 * the FRDY wait runs to its budget. ``ra8_usb_dcp_out_read`` must report
 * ``k_ra8_err_hw_timeout`` with the DCP parked NAK and a zero byte count.
 */
static void test_dcp_out_read_frdy_timeout(void)
{
  TEST_BEGIN("dcp_out_read surfaces the FRDY timeout leg");
  prep();

  uint8_t  buf[k_thc_cap] = {};
  uint16_t rx             = k_t_rx_poison;
  ra8_usb_fs()->BRDYSTS   = (uint16_t)k_thc_dcp_bit;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_usb_fs()->CFIFOCTR));

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_dcp_out_read(k_ra8_usb_speed_fs, buf, (uint16_t)k_thc_cap, &rx));
  /* The drain never ran: the byte count stays at the cleared entry value. */
  TEST_ASSERT_EQ(0U, rx);
  /* The DCP was parked NAK on the failure path. */
  TEST_ASSERT_EQ(k_ra8_pid_nak, (ra8_usb_fs()->DCPCTR & (uint16_t)k_ra8_pid_mask));

  TEST_END("dcp_out_read surfaces the FRDY timeout leg");
}

int32_t main(void)
{
  test_control_xfer_guards();
  test_setup_error_legs();
  test_setup_sureq_busy_fs_hs();
  test_mcdc_data_phase_direction();
  test_control_write_mxps_zero();
  test_control_read_completes();
  test_data_in_frdy_timeout();
  test_dcp_out_arm_speeds();
  test_mcdc_dcp_out_read();
  test_dcp_out_read_frdy_timeout();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_host_ctrl_cov.c\n");
  return 0;
}

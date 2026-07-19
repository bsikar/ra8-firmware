/**
 * @file test_ra8_sdmmc_spi_cov.c
 * @brief Coverage-completion unit tests for the SPI-mode SD driver core TU.
 *
 * @details
 * The companion ``test_ra8_sdmmc_spi.c`` exercises the block-I/O translation
 * unit (``ra8_sdmmc_spi_io.c``) and the happy identification path. This file
 * targets the error / recovery legs of the protocol core
 * (``ra8_sdmmc_spi.c``) that the block-I/O tests never reach: chip-select
 * failures, per-stage command / response transport faults, the R3/R7 tail
 * per-byte fallback, the ACMD41 exhaustion timeout, the CSD-version /
 * zero-capacity decode legs, and the SDv2 (non-HC) classification branch.
 *
 * Because the ``internal_*`` stage helpers are ``static`` and the core TU is
 * already linked into every host-test executable, the include-the-.c pattern
 * would duplicate symbols. Instead the tests bind a software mock directly
 * into ``s_sdmmc_spi_state.transport`` (exposed through the internal header)
 * and drive either the exposed low-level helpers or
 * ``ra8_sdmmc_spi_run_init_sequence()``. Every fault is injected
 * deterministically by a call-index counter -- no timers, no SIGALRM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_sdmmc_spi_internal.h"
#include "support/sdmmc_spi_cov_test_util.h"

/**
 * @enum sdmmc_spi_cov_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_sdmmc_spi_cov_cfg_cs_fail_at    = 5U,    /**< Sdmmc SPI COV config CS fail at.    */
  k_sdmmc_spi_cov_cfg_xfer_fail_at  = 18U,   /**< Sdmmc SPI COV config xfer fail at.  */
  k_sdmmc_spi_cov_cfg_xfer_fail_at2 = 21U,   /**< Sdmmc SPI COV config xfer fail at2. */
  k_sdmmc_spi_cov_cfg_xfer_fail_at3 = 27U,   /**< Sdmmc SPI COV config xfer fail at3. */
  k_sdmmc_spi_cov_cfg_xfer_fail_at4 = 29U,   /**< Sdmmc SPI COV config xfer fail at4. */
  k_sdmmc_spi_cov_cfg_cs_fail_at2   = 10U,   /**< Sdmmc SPI COV config CS fail at2.   */
  k_sdmmc_spi_cov_cfg_xfer_fail_at5 = 34U,   /**< Sdmmc SPI COV config xfer fail at5. */
  k_sdmmc_spi_cov_cfg_xfer_fail_at6 = 35U,   /**< Sdmmc SPI COV config xfer fail at6. */
  k_sdmmc_spi_cov_lit_x80           = 0x80U, /**< Sdmmc SPI COV literal 0x80.         */
  k_sdmmc_spi_cov_cfg_cs_fail_at3   = 12U,   /**< Sdmmc SPI COV config CS fail at3.   */
  k_sdmmc_spi_cov_cfg_xfer_fail_at7 = 55U,   /**< Sdmmc SPI COV config xfer fail at7. */
} sdmmc_spi_cov_test_lit_t;

/* ===========================================================================
 * Low-level helper error legs (bound transport, direct call)
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` on the ``cs`` return in
 * ``ra8_sdmmc_spi_cs_assert``. The armed cs failure flips it true; the
 * companion true/false control is the many successful cs_assert calls in the
 * init tests below.
 */
static void test_cs_assert_cs_failure(void)
{
  TEST_BEGIN("cs_assert propagates a cs failure");
  cov_bind(&s_tr);
  s_mock.cs_fail_at = 1U; /* cs(true) fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_cs_assert());
  TEST_END("cs_assert propagates a cs failure");
}

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` on the ``cs`` return in
 * ``ra8_sdmmc_spi_cs_release``; the armed cs failure flips it true.
 */
static void test_cs_release_cs_failure(void)
{
  TEST_BEGIN("cs_release propagates a cs failure");
  cov_bind(&s_tr);
  s_mock.cs_fail_at = 1U; /* cs(false) fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_cs_release());
  TEST_END("cs_release propagates a cs failure");
}

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` inside
 * ``internal_read_r1`` (reached through ``ra8_sdmmc_spi_send_command``). Call 1
 * is the frame xfer (ok); call 2 is the R1 read (armed fault), flipping the
 * condition true.
 */
static void test_send_command_r1_read_fault(void)
{
  TEST_BEGIN("send_command R1-read xfer fault");
  cov_bind(&s_tr);
  s_mock.xfer_fail_at = 2U; /* frame ok, R1 read faults. */
  uint8_t r1          = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_send_command(k_sd_cmd_go_idle_state, 0U, &r1));
  TEST_END("send_command R1-read xfer fault");
}

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` after the CMD55 prefix in
 * ``ra8_sdmmc_spi_send_acmd``; the armed fault on the first frame flips it true
 * so the ACMD is never issued.
 */
static void test_send_acmd_cmd55_fault(void)
{
  TEST_BEGIN("send_acmd CMD55-prefix xfer fault");
  cov_bind(&s_tr);
  s_mock.xfer_fail_at = 1U; /* CMD55 frame faults. */
  uint8_t r1          = 0U;
  TEST_ASSERT(ra8_sdmmc_spi_send_acmd(k_sd_acmd_sd_send_op_cond, 0U, &r1) != k_ra8_ok);
  TEST_END("send_acmd CMD55-prefix xfer fault");
}

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` in
 * ``ra8_sdmmc_spi_wait_data_token``; the armed fault on the first poll flips it
 * true.
 */
static void test_wait_data_token_fault(void)
{
  TEST_BEGIN("wait_data_token xfer fault");
  cov_bind(&s_tr);
  s_mock.xfer_fail_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_wait_data_token());
  TEST_END("wait_data_token xfer fault");
}

/**
 * @par MC/DC:
 * Two single-condition decisions in ``ra8_sdmmc_spi_wait_not_busy_bounded``:
 * the ``if (err != k_ra8_ok)`` xfer-fault leg (first sub-case) and the
 * loop-exhaustion timeout return (second sub-case, non-idle bytes so the
 * match never fires within the bound).
 */
static void test_wait_not_busy_bounded_legs(void)
{
  TEST_BEGIN("wait_not_busy_bounded fault + timeout");
  /* Xfer-fault leg. */
  cov_bind(&s_tr);
  s_mock.xfer_fail_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_wait_not_busy_bounded(10U));

  /* Timeout leg: three non-idle (busy) bytes exhaust a 3-poll budget. */
  cov_bind(&s_tr);
  mock_queue_byte(0x00U);
  mock_queue_byte(0x00U);
  mock_queue_byte(0x00U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_wait_not_busy_bounded(3U));
  TEST_END("wait_not_busy_bounded fault + timeout");
}

/**
 * @par MC/DC:
 * Decision ``(cmd == k_sd_cmd_send_if_cond) && (arg == k_sd_cmd8_arg_check_pattern)``
 * (2 conditions) in ``internal_build_frame``, reached through the public
 * ``ra8_sdmmc_spi_send_command``. It selects the spec's pre-baked CMD8 CRC byte
 * (0x87) over the general computed CRC7.
 *   - Control (F,-): any non-CMD8 command       -> C1 false (short-circuit) ->
 *     computed CRC. Exercised by every CMD0/CMD55/... send in the init tests.
 *   - Control (T,T): CMD8 with the canonical 0x1AA check pattern -> pre-baked
 *     CRC. Exercised by the CMD8 init step in the full-init tests.
 *   - Here (T,F): CMD8 (send_if_cond) with an arg != 0x1AA -> C1 true, C2 false
 *     -> computed CRC. The pair with (T,T) proves the arg operand independently
 *     moves the outcome. A ready R1 is queued so the send still succeeds.
 */
static void test_build_frame_cmd8_nonpattern_arg(void)
{
  TEST_BEGIN("build_frame CMD8 non-pattern arg -> computed CRC");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD8-shaped frame shift. */
  mock_queue_byte((uint8_t)k_cov_r1_ready);     /* R1 token (ready).        */
  uint8_t r1 = (uint8_t)k_cov_idle_byte;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sdmmc_spi_send_command(k_sd_cmd_send_if_cond, (uint32_t)k_cov_bad_echo, &r1));
  TEST_ASSERT_EQ(k_cov_r1_ready, r1);
  TEST_END("build_frame CMD8 non-pattern arg -> computed CRC");
}

/* ===========================================================================
 * Wake / CMD0 / recovery legs (probe entry)
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition decision ``if (err != k_ra8_ok)`` on the ``cs`` return in
 * ``internal_wake_card``; the armed cs #1 failure flips it true so the probe
 * enters recovery and ultimately fails.
 */
static void test_init_wake_cs_failure(void)
{
  TEST_BEGIN("init wake-card cs failure");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  s_mock.cs_fail_at = 1U; /* wake cs(false). */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init wake-card cs failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_send_cmd0``; the armed cs #2 failure (CMD0 cs_assert) flips it
 * true, driving the recovery retries.
 */
static void test_init_cmd0_cs_assert_failure(void)
{
  TEST_BEGIN("init CMD0 cs_assert failure");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  s_mock.cs_fail_at = 2U; /* CMD0 cs_assert cs(true). */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD0 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (r1 != idle_state)`` in ``internal_send_cmd0``. CMD0
 * returns a valid R1 (sentinel clear) that is not idle (0x00), flipping the
 * condition true so CMD0 reports a protocol error and the probe recovers /
 * retries.
 */
static void test_init_cmd0_bad_r1(void)
{
  TEST_BEGIN("init CMD0 non-idle R1");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_ready); /* CMD0 R1 == 0x00 (not idle). */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD0 non-idle R1");
}

/**
 * @par MC/DC:
 * Single-condition ``if (cs(true) != k_ra8_ok)`` in
 * ``internal_recover_stuck_card`` phase 2. CMD0 first times out (no R1
 * queued) so the probe enters recovery; cs #5 (recovery phase-2 assert) is
 * armed to fail, flipping the condition true and returning early from
 * recovery.
 */
static void test_init_recover_phase2_cs_failure(void)
{
  TEST_BEGIN("init recovery phase-2 cs failure");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);        /* CMD0 R1 then under-runs -> timeout. */
  s_mock.cs_fail_at = k_sdmmc_spi_cov_cfg_cs_fail_at; /* recovery phase-2 cs(true).          */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init recovery phase-2 cs failure");
}

/* ===========================================================================
 * CMD8 legs
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_send_cmd8`` (and the propagating ``if`` in ``internal_probe_card``).
 * cs #4 (CMD8 cs_assert) is armed to fail.
 */
static void test_init_cmd8_cs_assert_failure(void)
{
  TEST_BEGIN("init CMD8 cs_assert failure");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle); /* CMD0 ok.        */
  s_mock.cs_fail_at = 4U;           /* CMD8 cs_assert. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD8 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``send_command`` in
 * ``internal_send_cmd8`` (its cs_release + return-err leg). The CMD8 frame
 * xfer (call 16) is armed to fault.
 */
static void test_init_cmd8_send_command_fault(void)
{
  TEST_BEGIN("init CMD8 send_command fault");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle); /* CMD0 ok (calls 11..14). */
  s_mock.xfer_fail_at = 16U;        /* CMD8 frame faults.      */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD8 send_command fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if ((echo & mask) != (pattern & mask))`` in
 * ``internal_send_cmd8``. The queued R7 tail echoes a non-canonical word, so
 * the compare is true and CMD8 reports a protocol error.
 */
static void test_init_cmd8_echo_mismatch(void)
{
  TEST_BEGIN("init CMD8 echo mismatch");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle);
  q_cmd_r3r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_bad_echo); /* wrong echo. */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_run_init_sequence());
  TEST_END("init CMD8 echo mismatch");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` (false control) inside
 * ``internal_read_r3_or_r7_tail`` per-byte fallback loop. The NULL-tx bulk
 * read is refused by the transport, so both the CMD8 and CMD58 R3/R7 tails run
 * the fallback to completion; the loop condition and successful exit are
 * exercised and the init still succeeds.
 */
static void test_init_tail_fallback_success(void)
{
  TEST_BEGIN("init R3/R7 tail per-byte fallback success");
  cov_bind(&s_tr_nonulltx);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2(csd);
  q_csd(csd);
  q_cmd_r1((uint8_t)k_cov_r1_ready); /* CMD16. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_run_init_sequence());
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_type_sdhc, s_sdmmc_spi_state.card_type);
  TEST_END("init R3/R7 tail per-byte fallback success");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` (true leg) inside the
 * ``internal_read_r3_or_r7_tail`` per-byte fallback, and the propagating
 * ``if`` in ``internal_send_cmd8``. The CMD8 bulk tail (call 18) is armed to
 * fault; the first fallback byte then faults too, so the tail returns an error.
 */
static void test_init_cmd8_tail_fallback_fault(void)
{
  TEST_BEGIN("init CMD8 tail fallback fault");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle);                       /* CMD0 (calls 11..14).        */
  mock_queue_idle(1U);                                    /* CMD8 cs_assert (15).        */
  mock_queue_idle((uint32_t)k_cov_frame_bytes);           /* CMD8 frame (16).            */
  mock_queue_byte((uint8_t)k_cov_r1_idle);                /* CMD8 R1 (17).               */
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at; /* bulk tail + fallback fault. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD8 tail fallback fault");
}

/* ===========================================================================
 * ACMD41 legs
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_acmd41_loop`` (and the propagating ``if`` in the probe). cs #6
 * (ACMD41 cs_assert) is armed to fail.
 */
static void test_init_acmd41_cs_assert_failure(void)
{
  TEST_BEGIN("init ACMD41 cs_assert failure");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle);
  q_cmd_r3r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_cmd8_echo);
  s_mock.cs_fail_at = 6U; /* ACMD41 cs_assert. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init ACMD41 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``send_acmd`` in
 * ``internal_acmd41_loop``. The CMD55 frame of the first ACMD41 iteration
 * (call 21) is armed to fault.
 */
static void test_init_acmd41_send_acmd_fault(void)
{
  TEST_BEGIN("init ACMD41 send_acmd fault");
  cov_bind(&s_tr);
  mock_queue_idle((uint32_t)k_cov_wake_bytes);
  q_cmd_r1((uint8_t)k_cov_r1_idle);
  q_cmd_r3r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_cmd8_echo);
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at2; /* CMD55 frame of ACMD41. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init ACMD41 send_acmd fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if ((r1 & idle_state) == 0)`` in ``internal_acmd41_loop``
 * held false for the whole attempt budget. The command-aware transport keeps
 * ACMD41's R1 idle, so the loop runs to its ceiling and returns the
 * init-failed timeout (also exercising the loop back-edge).
 */
static void test_init_acmd41_exhausts_attempts(void)
{
  TEST_BEGIN("init ACMD41 attempt exhaustion");
  cov_bind(&s_tr_smart);
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_sdmmc_spi_run_init_sequence());
  TEST_END("init ACMD41 attempt exhaustion");
}

/* ===========================================================================
 * CMD58 (OCR) legs
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_read_ocr``; cs #8 (CMD58 cs_assert) is armed to fail.
 */
static void test_init_read_ocr_cs_assert_failure(void)
{
  TEST_BEGIN("init CMD58 cs_assert failure");
  cov_bind(&s_tr);
  q_prefix_through_acmd41();
  s_mock.cs_fail_at = 8U; /* CMD58 cs_assert. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD58 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``send_command`` in
 * ``internal_read_ocr`` (cs_release + return-err leg). The CMD58 frame
 * (call 27) is armed to fault.
 */
static void test_init_read_ocr_send_command_fault(void)
{
  TEST_BEGIN("init CMD58 send_command fault");
  cov_bind(&s_tr);
  q_prefix_through_acmd41();
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at3; /* CMD58 frame. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD58 send_command fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``read_r3_or_r7_tail`` in
 * ``internal_read_ocr``. The CMD58 bulk tail (call 29) is armed to fault so
 * both the bulk and the first fallback byte fail, and the tail returns error.
 */
static void test_init_read_ocr_tail_fault(void)
{
  TEST_BEGIN("init CMD58 tail fault");
  cov_bind(&s_tr);
  q_prefix_through_acmd41();
  /* CMD58 cs_assert(26), frame(27), R1(28) succeed; tail bulk(29) faults. */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_cov_frame_bytes);
  mock_queue_byte((uint8_t)k_cov_r1_ready);
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at4;
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD58 tail fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (is_v2)`` in ``internal_classify_card`` held true with
 * ``is_hc`` false (SDv2 SC): CMD8 classifies v2 but the OCR CCS bit is clear,
 * so the classifier returns the SDv2 branch rather than SDHC.
 */
static void test_init_ocr_no_ccs_classifies_sdv2(void)
{
  TEST_BEGIN("init OCR without CCS -> SDv2");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_no_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2(csd);
  q_csd(csd);
  q_cmd_r1((uint8_t)k_cov_r1_ready); /* CMD16. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_run_init_sequence());
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_type_sdv2, s_sdmmc_spi_state.card_type);
  TEST_END("init OCR without CCS -> SDv2");
}

/* ===========================================================================
 * CMD9 (CSD) legs
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_read_csd``; cs #10 (CMD9 cs_assert) is armed to fail.
 */
static void test_init_read_csd_cs_assert_failure(void)
{
  TEST_BEGIN("init CMD9 cs_assert failure");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  s_mock.cs_fail_at = k_sdmmc_spi_cov_cfg_cs_fail_at2; /* CMD9 cs_assert. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD9 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``send_command`` in
 * ``internal_read_csd``; the CMD9 frame (call 32) is armed to fault.
 */
static void test_init_read_csd_send_command_fault(void)
{
  TEST_BEGIN("init CMD9 send_command fault");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  s_mock.xfer_fail_at = 32U; /* CMD9 frame. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD9 send_command fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (r1 != 0)`` in ``internal_read_csd``. CMD9 returns a
 * non-zero R1, flipping it true so the read reports a protocol error before
 * the data phase.
 */
static void test_init_read_csd_bad_r1(void)
{
  TEST_BEGIN("init CMD9 non-zero R1");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  mock_queue_idle(1U);                          /* CMD9 cs_assert. */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD9 frame.     */
  mock_queue_byte((uint8_t)k_cov_r1_bad);       /* R1 != 0.        */
  mock_queue_idle(1U);                          /* cs_release.     */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_run_init_sequence());
  TEST_END("init CMD9 non-zero R1");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``wait_data_token`` in
 * ``internal_read_csd``. CMD9 R1 is 0 but the data-token poll (call 34) is
 * armed to fault.
 */
static void test_init_read_csd_data_token_fault(void)
{
  TEST_BEGIN("init CMD9 data-token fault");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  mock_queue_idle(1U);                                     /* CMD9 cs_assert (31).    */
  mock_queue_idle((uint32_t)k_cov_frame_bytes);            /* CMD9 frame (32).        */
  mock_queue_byte((uint8_t)k_cov_r1_ready);                /* R1 == 0 (33).           */
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at5; /* data-token poll faults. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD9 data-token fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` inside the CSD-body read loop of
 * ``internal_read_csd``. The data token arrives (call 34) but the first body
 * byte (call 35) is armed to fault.
 */
static void test_init_read_csd_body_fault(void)
{
  TEST_BEGIN("init CMD9 body fault");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  mock_queue_idle(1U);                                     /* CMD9 cs_assert (31).        */
  mock_queue_idle((uint32_t)k_cov_frame_bytes);            /* CMD9 frame (32).            */
  mock_queue_byte((uint8_t)k_cov_r1_ready);                /* R1 == 0 (33).               */
  mock_queue_byte((uint8_t)k_cov_token_data);              /* data token (34).            */
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at6; /* first CSD body byte faults. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD9 body fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (*out_blocks == 0)`` in ``internal_read_csd`` and the
 * unreachable-version ``return 0`` in ``internal_csd_to_blocks``. A CSD whose
 * version field is 2 decodes to zero blocks, flipping the check true and
 * reporting a protocol error.
 */
static void test_init_csd_bad_version(void)
{
  TEST_BEGIN("init CSD bad version -> zero blocks");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  memset(csd, 0, sizeof(csd));
  csd[0] = k_sdmmc_spi_cov_lit_x80; /* CSD_STRUCTURE bits 7:6 == 0b10 (reserved). */
  q_csd(csd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_run_init_sequence());
  TEST_END("init CSD bad version -> zero blocks");
}

/* ===========================================================================
 * CMD16 (SET_BLOCKLEN) legs
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``cs_assert`` in
 * ``internal_set_block_len``; cs #12 (CMD16 cs_assert) is armed to fail.
 */
static void test_init_set_block_len_cs_assert_failure(void)
{
  TEST_BEGIN("init CMD16 cs_assert failure");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2(csd);
  q_csd(csd);
  s_mock.cs_fail_at = k_sdmmc_spi_cov_cfg_cs_fail_at3; /* CMD16 cs_assert. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD16 cs_assert failure");
}

/**
 * @par MC/DC:
 * Single-condition ``if (err != k_ra8_ok)`` after ``send_command`` in
 * ``internal_set_block_len``; the CMD16 frame (call 55) is armed to fault.
 */
static void test_init_set_block_len_send_command_fault(void)
{
  TEST_BEGIN("init CMD16 send_command fault");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2(csd);
  q_csd(csd);
  s_mock.xfer_fail_at = k_sdmmc_spi_cov_cfg_xfer_fail_at7; /* CMD16 frame. */
  TEST_ASSERT(ra8_sdmmc_spi_run_init_sequence() != k_ra8_ok);
  TEST_END("init CMD16 send_command fault");
}

/**
 * @par MC/DC:
 * Single-condition ``if (r1 != 0)`` in ``internal_set_block_len``. CMD16
 * returns a non-zero R1, flipping it true so init reports a protocol error.
 */
static void test_init_set_block_len_bad_r1(void)
{
  TEST_BEGIN("init CMD16 non-zero R1");
  cov_bind(&s_tr);
  q_prefix_through_ocr((uint32_t)k_cov_ocr_ccs);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2(csd);
  q_csd(csd);
  q_cmd_r1((uint8_t)k_cov_r1_bad); /* CMD16 R1 != 0. */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_run_init_sequence());
  TEST_END("init CMD16 non-zero R1");
}

/* ===========================================================================
 * Main
 * ===========================================================================
 */

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
  test_cs_assert_cs_failure,
  test_cs_release_cs_failure,
  test_send_command_r1_read_fault,
  test_build_frame_cmd8_nonpattern_arg,
  test_send_acmd_cmd55_fault,
  test_wait_data_token_fault,
  test_wait_not_busy_bounded_legs,
  test_init_wake_cs_failure,
  test_init_cmd0_cs_assert_failure,
  test_init_cmd0_bad_r1,
  test_init_recover_phase2_cs_failure,
  test_init_cmd8_cs_assert_failure,
  test_init_cmd8_send_command_fault,
  test_init_cmd8_echo_mismatch,
  test_init_tail_fallback_success,
  test_init_cmd8_tail_fallback_fault,
  test_init_acmd41_cs_assert_failure,
  test_init_acmd41_send_acmd_fault,
  test_init_acmd41_exhausts_attempts,
  test_init_read_ocr_cs_assert_failure,
  test_init_read_ocr_send_command_fault,
  test_init_read_ocr_tail_fault,
  test_init_ocr_no_ccs_classifies_sdv2,
  test_init_read_csd_cs_assert_failure,
  test_init_read_csd_send_command_fault,
  test_init_read_csd_bad_r1,
  test_init_read_csd_data_token_fault,
  test_init_read_csd_body_fault,
  test_init_csd_bad_version,
  test_init_set_block_len_cs_assert_failure,
  test_init_set_block_len_send_command_fault,
  test_init_set_block_len_bad_r1,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] all ra8_sdmmc_spi_cov tests passed\n");
  return 0;
}

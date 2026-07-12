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
#include "unity_minimal.h"

/* ===========================================================================
 * Mock SPI transport
 * ===========================================================================
 *
 * A tiny request/response queue: each test pre-loads the bytes the driver
 * will read back, and the mock hands them out one clock at a time (0xFF on
 * under-run, matching a real idle CIPO line). Two deterministic fault knobs
 * drive the error legs:
 *   - xfer_fail_at: the Nth (and every later) xfer call reports a bus timeout.
 *   - cs_fail_at:   exactly the Nth cs call reports a bus timeout.
 */

/**
 * @enum cov_mock_size_t
 * @brief Sizing / framing constants for the mock queue.
 */
typedef enum : uint32_t {
  k_cov_buf_bytes   = 1024U,        /**< Queue depth (a full init prefix is < 64 bytes). */
  k_cov_idle_byte   = 0xFFU,        /**< Bus-idle / under-run fill byte.                 */
  k_cov_wake_bytes  = 10U,          /**< 80 wake clocks == 10 idle bytes.                */
  k_cov_frame_bytes = 6U,           /**< Bytes shifted for one command frame.            */
  k_cov_cmd8_echo   = 0x000001AAUL, /**< CMD8 canonical check pattern echo.              */
  k_cov_ocr_ccs     = 0xC0FF8000UL, /**< OCR: busy | CCS | voltage window (SDHC).        */
  k_cov_ocr_no_ccs  = 0x80FF8000UL, /**< OCR: busy | voltage window, CCS clear.          */
  k_cov_bad_echo    = 0xDEADBEEFUL, /**< CMD8 echo that fails the 12-bit compare.        */
} cov_mock_size_t;

/**
 * @enum cov_byte_t
 * @brief Response byte fixtures the mock feeds back to the driver.
 */
typedef enum : uint8_t {
  k_cov_r1_idle    = 0x01U, /**< R1 idle-state (CMD0 / CMD55 accept).       */
  k_cov_r1_ready   = 0x00U, /**< R1 all-clear (card ready).                 */
  k_cov_r1_illegal = 0x04U, /**< R1 illegal-command (CMD8 -> v1 classify).  */
  k_cov_r1_bad     = 0x04U, /**< Generic non-zero R1 -> protocol-error leg. */
  k_cov_token_data = 0xFEU, /**< Single-block data-start token.             */
  k_cov_cmd_bit    = 0x40U, /**< Command-frame marker bit (byte 0 bit 6).   */
} cov_byte_t;

/**
 * @struct cov_mock_t
 * @brief Per-test mock state.
 */
typedef struct {
  uint8_t  rx_queue[k_cov_buf_bytes]; /**< Bytes handed back on each clock.         */
  uint32_t rx_len;                    /**< Bytes loaded into rx_queue.              */
  uint32_t rx_pos;                    /**< Next index inside rx_queue.              */
  uint32_t xfer_calls;                /**< Total xfer calls so far.                 */
  uint32_t xfer_fail_at;              /**< 1-based xfer call to start failing (>=). */
  uint32_t cs_calls;                  /**< Total cs calls so far.                   */
  uint32_t cs_fail_at;                /**< 1-based cs call to fail (exact match).   */
  uint8_t  smart_cur_cmd;             /**< Last command byte (smart transport).     */
  bool     smart_pending_r1;          /**< A frame is awaiting its R1 (smart).      */
} cov_mock_t;

/** @brief The single per-test mock instance. */
static cov_mock_t s_mock = {};

/**
 * @brief Reset the mock to a pristine, fault-free state.
 * @pre None.
 * @post The queue is empty and both fault injectors are disarmed.
 * @post All call counters read zero.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
}

/**
 * @brief Append one byte to the response queue (silently drops on overflow).
 * @param[in] b Byte the driver will read on its next clock.
 * @pre None.
 * @post One byte is queued when space remains.
 * @post rx_len grows by at most one.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void mock_queue_byte(uint8_t b)
{
  if (s_mock.rx_len < (uint32_t)k_cov_buf_bytes) {
    s_mock.rx_queue[s_mock.rx_len] = b;
    s_mock.rx_len++;
  }
}

/**
 * @brief Queue @p count copies of the idle byte 0xFF.
 * @param[in] count Number of idle bytes to enqueue.
 * @pre None.
 * @post Up to @p count idle bytes are queued.
 * @post rx_len grows by at most @p count.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void mock_queue_idle(uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    mock_queue_byte((uint8_t)k_cov_idle_byte);
  }
}

/**
 * @brief set_clock stub -- always succeeds (run_init_sequence never calls it).
 * @param[in] ctx Unused mock context.
 * @param[in] hz  Requested clock (ignored).
 * @return Always ::k_ra8_ok.
 * @retval k_ra8_ok Unconditional.
 * @pre None.
 * @post No state mutated.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_set_clock(void* ctx, uint32_t hz)
{
  (void)ctx;
  (void)hz;
  return k_ra8_ok;
}

/**
 * @brief cs driver -- fails exactly on the armed cs call, else succeeds.
 * @param[in] ctx      Unused mock context.
 * @param[in] asserted Requested CS level (ignored).
 * @return ::k_ra8_ok, or ::k_ra8_err_hw_timeout on the armed call.
 * @retval k_ra8_ok           CS toggled.
 * @retval k_ra8_err_hw_timeout The armed cs call fired.
 * @pre None.
 * @post cs_calls grows by one.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_cs(void* ctx, bool asserted)
{
  (void)ctx;
  (void)asserted;
  s_mock.cs_calls++;
  if ((s_mock.cs_fail_at != 0U) && (s_mock.cs_calls == s_mock.cs_fail_at)) {
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}

/**
 * @brief Full-duplex byte exchange with deterministic fault injection.
 * @param[in]  ctx Unused mock context.
 * @param[in]  tx  Bytes clocked out, or NULL for idle (0xFF) fill.
 * @param[out] rx  Received bytes, or NULL to discard.
 * @param[in]  len Byte count.
 * @return ::k_ra8_ok, or ::k_ra8_err_hw_timeout once the fault index is reached.
 * @retval k_ra8_ok            Bytes exchanged from the queue.
 * @retval k_ra8_err_hw_timeout Armed fault fired (this and later calls).
 * @pre @p rx is either NULL or holds @p len bytes.
 * @pre The mock is bound into the driver transport.
 * @post rx_pos advances by @p len on success.
 * @post xfer_calls grows by one.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  (void)tx;
  s_mock.xfer_calls++;
  if ((s_mock.xfer_fail_at != 0U) && (s_mock.xfer_calls >= s_mock.xfer_fail_at)) {
    return k_ra8_err_hw_timeout;
  }
  for (uint32_t i = 0U; i < len; i++) {
    uint8_t in = (uint8_t)k_cov_idle_byte;
    if (s_mock.rx_pos < s_mock.rx_len) {
      in = s_mock.rx_queue[s_mock.rx_pos];
      s_mock.rx_pos++;
    }
    if (rx != nullptr) {
      rx[i] = in;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief xfer shim that refuses NULL-tx transfers to force the tail fallback.
 * @param[in]  ctx Unused mock context.
 * @param[in]  tx  Bytes clocked out; NULL rejects the transfer.
 * @param[out] rx  Received bytes.
 * @param[in]  len Byte count.
 * @return ::k_ra8_err_not_supported on NULL tx, else ::mock_xfer's result.
 * @retval k_ra8_err_not_supported NULL tx (drives read_r3_or_r7_tail fallback).
 * @retval k_ra8_ok               Non-NULL tx exchanged.
 * @pre The mock is bound into the driver transport.
 * @post On the non-NULL path, ::mock_xfer's postconditions hold.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_xfer_no_nulltx(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  if (tx == nullptr) {
    return k_ra8_err_not_supported;
  }
  return mock_xfer(ctx, tx, rx, len);
}

/**
 * @brief Command-aware xfer that keeps ACMD41 forever idle (exhaustion leg).
 *
 * @details Records each 6-byte command frame, then answers the R1 read that
 * follows: CMD8 gets an illegal-command R1 (v1 classify, no R3/R7 tail) and
 * every other command gets idle (0x01). ACMD41's idle R1 keeps the readiness
 * loop iterating until the attempt ceiling trips, exercising the timeout leg
 * with no giant queue and no timer.
 *
 * @param[in]  ctx Unused mock context.
 * @param[in]  tx  Bytes clocked out.
 * @param[out] rx  Received bytes.
 * @param[in]  len Byte count.
 * @return Always ::k_ra8_ok.
 * @retval k_ra8_ok Unconditional.
 * @pre The mock is bound into the driver transport.
 * @post smart_cur_cmd / smart_pending_r1 track the last frame.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t smart_xfer_acmd41(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  if ((len == (uint32_t)k_cov_frame_bytes) && (tx != nullptr) &&
      ((tx[0] & (uint8_t)k_cov_cmd_bit) != 0U)) {
    s_mock.smart_cur_cmd    = tx[0];
    s_mock.smart_pending_r1 = true;
    if (rx != nullptr) {
      memset(rx, (int)k_cov_idle_byte, (size_t)len);
    }
    return k_ra8_ok;
  }
  uint8_t out = (uint8_t)k_cov_idle_byte;
  if (s_mock.smart_pending_r1 && (len == 1U)) {
    s_mock.smart_pending_r1 = false;
    out = (s_mock.smart_cur_cmd == (uint8_t)k_sd_cmd_send_if_cond) ? (uint8_t)k_cov_r1_illegal
                                                                   : (uint8_t)k_cov_r1_idle;
  }
  if (rx != nullptr) {
    memset(rx, (int)out, (size_t)len);
  }
  return k_ra8_ok;
}

/** @brief Standard fault-injecting transport. */
static const ra8_sdmmc_spi_transport_t s_tr = {
  .set_clock = mock_set_clock,
  .cs        = mock_cs,
  .xfer      = mock_xfer,
  .ctx       = nullptr,
};

/** @brief Transport whose NULL-tx bulk read fails (drives the tail fallback). */
static const ra8_sdmmc_spi_transport_t s_tr_nonulltx = {
  .set_clock = mock_set_clock,
  .cs        = mock_cs,
  .xfer      = mock_xfer_no_nulltx,
  .ctx       = nullptr,
};

/** @brief Command-aware transport that never lets ACMD41 report ready. */
static const ra8_sdmmc_spi_transport_t s_tr_smart = {
  .set_clock = mock_set_clock,
  .cs        = mock_cs,
  .xfer      = smart_xfer_acmd41,
  .ctx       = nullptr,
};

/**
 * @brief Reset the driver + mock and bind @p tr as the active transport.
 * @param[in] tr Transport descriptor to install into the driver state.
 * @pre @p tr is non-NULL.
 * @pre The driver may be in any prior state.
 * @post The mock queue is empty and @p tr is the bound transport.
 * @post The driver reports un-initialised.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void cov_bind(const ra8_sdmmc_spi_transport_t* tr)
{
  (void)ra8_sdmmc_spi_deinit();
  mock_reset();
  s_sdmmc_spi_state.transport = *tr;
}

/* ===========================================================================
 * Stage-response queue helpers (v2 SDHC init flow)
 * ===========================================================================
 */

/**
 * @brief Queue one CS-bracketed command whose response is a single R1 byte.
 * @param[in] r1 R1 token the card returns.
 */
static void q_cmd_r1(uint8_t r1)
{
  mock_queue_idle(1U);                          /* cs_assert post-byte.  */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* frame shift.          */
  mock_queue_byte(r1);                          /* R1 token.             */
  mock_queue_idle(1U);                          /* cs_release post-byte. */
}

/**
 * @brief Queue one CS-bracketed command with an R1 + 4-byte R3/R7 tail.
 * @param[in] r1        R1 token the card returns.
 * @param[in] tail_word 32-bit R3/R7 tail (MSB first on the wire).
 */
static void q_cmd_r3r7(uint8_t r1, uint32_t tail_word)
{
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_cov_frame_bytes);
  mock_queue_byte(r1);
  mock_queue_byte((uint8_t)((tail_word >> 24U) & 0xFFU));
  mock_queue_byte((uint8_t)((tail_word >> 16U) & 0xFFU));
  mock_queue_byte((uint8_t)((tail_word >> 8U) & 0xFFU));
  mock_queue_byte((uint8_t)(tail_word & 0xFFU));
  mock_queue_idle(1U);
}

/**
 * @brief Populate a CSD v2 register reporting a 32 GiB SDHC card.
 * @param[out] out 16-byte CSD buffer.
 */
static void build_csd_v2(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0] = 0x40U; /* CSD_STRUCTURE = 1 (version bits 7:6 == 0b01). */
  out[8] = 0xFFU;
  out[9] = 0xFFU;
}

/**
 * @brief Queue the CMD9 CSD read (R1==0, data token, 16 CSD bytes, CRC16).
 * @param[in] csd 16-byte CSD payload to deliver.
 */
static void q_csd(const uint8_t* csd)
{
  mock_queue_idle(1U);                          /* cs_assert post-byte. */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD9 frame.          */
  mock_queue_byte((uint8_t)k_cov_r1_ready);     /* R1 == 0.             */
  mock_queue_byte((uint8_t)k_cov_token_data);   /* data-start token.    */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_csd_response_len; i++) {
    mock_queue_byte(csd[i]);
  }
  mock_queue_byte(0U); /* CRC16 hi (unchecked). */
  mock_queue_byte(0U); /* CRC16 lo (unchecked). */
  mock_queue_idle(1U); /* cs_release post-byte. */
}

/**
 * @brief Queue the wake + CMD0 + CMD8(v2) + ACMD41 prefix (through readiness).
 *
 * @details CMD55 and ACMD41 share one CS bracket (no cs toggle between them),
 * so the ACMD41 stage is queued as a single tight block: one cs_assert idle,
 * the CMD55 frame + R1, the ACMD41 frame + R1, and one cs_release idle. Each
 * R1 token sits immediately after its 6-byte frame so ``internal_read_r1``
 * resolves it in one xfer call, keeping downstream fault indices exact.
 */
static void q_prefix_through_acmd41(void)
{
  mock_queue_idle((uint32_t)k_cov_wake_bytes);                   /* 80 wake clocks.           */
  q_cmd_r1((uint8_t)k_cov_r1_idle);                              /* CMD0 -> idle.             */
  q_cmd_r3r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_cmd8_echo); /* CMD8 v2.                  */
  mock_queue_idle(1U);                                           /* ACMD41 stage cs_assert.   */
  mock_queue_idle((uint32_t)k_cov_frame_bytes);                  /* CMD55 frame.              */
  mock_queue_byte((uint8_t)k_cov_r1_idle);                       /* CMD55 R1 (idle accepted). */
  mock_queue_idle((uint32_t)k_cov_frame_bytes);                  /* ACMD41 frame.             */
  mock_queue_byte((uint8_t)k_cov_r1_ready);                      /* ACMD41 R1 (ready).        */
  mock_queue_idle(1U);                                           /* ACMD41 stage cs_release.  */
}

/**
 * @brief Queue the prefix through the CMD58 OCR read.
 * @param[in] ocr OCR word the card returns (CCS bit selects SDHC vs SDSC).
 */
static void q_prefix_through_ocr(uint32_t ocr)
{
  q_prefix_through_acmd41();
  q_cmd_r3r7((uint8_t)k_cov_r1_ready, ocr); /* CMD58 OCR. */
}

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
  mock_queue_idle((uint32_t)k_cov_wake_bytes); /* CMD0 R1 then under-runs -> timeout. */
  s_mock.cs_fail_at = 5U;                      /* recovery phase-2 cs(true).          */
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
  q_cmd_r1((uint8_t)k_cov_r1_idle);             /* CMD0 (calls 11..14).        */
  mock_queue_idle(1U);                          /* CMD8 cs_assert (15).        */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD8 frame (16).            */
  mock_queue_byte((uint8_t)k_cov_r1_idle);      /* CMD8 R1 (17).               */
  s_mock.xfer_fail_at = 18U;                    /* bulk tail + fallback fault. */
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
  s_mock.xfer_fail_at = 21U; /* CMD55 frame of ACMD41. */
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
  s_mock.xfer_fail_at = 27U; /* CMD58 frame. */
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
  s_mock.xfer_fail_at = 29U;
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
  s_mock.cs_fail_at = 10U; /* CMD9 cs_assert. */
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
  mock_queue_idle(1U);                          /* CMD9 cs_assert (31).    */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD9 frame (32).        */
  mock_queue_byte((uint8_t)k_cov_r1_ready);     /* R1 == 0 (33).           */
  s_mock.xfer_fail_at = 34U;                    /* data-token poll faults. */
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
  mock_queue_idle(1U);                          /* CMD9 cs_assert (31).        */
  mock_queue_idle((uint32_t)k_cov_frame_bytes); /* CMD9 frame (32).            */
  mock_queue_byte((uint8_t)k_cov_r1_ready);     /* R1 == 0 (33).               */
  mock_queue_byte((uint8_t)k_cov_token_data);   /* data token (34).            */
  s_mock.xfer_fail_at = 35U;                    /* first CSD body byte faults. */
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
  csd[0] = 0x80U; /* CSD_STRUCTURE bits 7:6 == 0b10 (reserved). */
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
  s_mock.cs_fail_at = 12U; /* CMD16 cs_assert. */
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
  s_mock.xfer_fail_at = 55U; /* CMD16 frame. */
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

int main(void)
{
  test_cs_assert_cs_failure();
  test_cs_release_cs_failure();
  test_send_command_r1_read_fault();
  test_build_frame_cmd8_nonpattern_arg();
  test_send_acmd_cmd55_fault();
  test_wait_data_token_fault();
  test_wait_not_busy_bounded_legs();
  test_init_wake_cs_failure();
  test_init_cmd0_cs_assert_failure();
  test_init_cmd0_bad_r1();
  test_init_recover_phase2_cs_failure();
  test_init_cmd8_cs_assert_failure();
  test_init_cmd8_send_command_fault();
  test_init_cmd8_echo_mismatch();
  test_init_tail_fallback_success();
  test_init_cmd8_tail_fallback_fault();
  test_init_acmd41_cs_assert_failure();
  test_init_acmd41_send_acmd_fault();
  test_init_acmd41_exhausts_attempts();
  test_init_read_ocr_cs_assert_failure();
  test_init_read_ocr_send_command_fault();
  test_init_read_ocr_tail_fault();
  test_init_ocr_no_ccs_classifies_sdv2();
  test_init_read_csd_cs_assert_failure();
  test_init_read_csd_send_command_fault();
  test_init_read_csd_bad_r1();
  test_init_read_csd_data_token_fault();
  test_init_read_csd_body_fault();
  test_init_csd_bad_version();
  test_init_set_block_len_cs_assert_failure();
  test_init_set_block_len_send_command_fault();
  test_init_set_block_len_bad_r1();
  (void)fprintf(stderr, "[OK ] all ra8_sdmmc_spi_cov tests passed\n");
  return 0;
}

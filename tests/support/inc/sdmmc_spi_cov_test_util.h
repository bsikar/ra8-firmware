/**
 * @file sdmmc_spi_cov_test_util.h
 * @brief Fixture for the SPI-mode SD protocol-core coverage tests: the
 *        deterministic fault-injecting mock transport, the transport
 *        binding helper, and the R1/R3R7/CSD/init-prefix queue scripts
 *
 * @details Header-only (all definitions `static`); split out of
 * test_ra8_sdmmc_spi_cov.c so the test TU stays under the 1000-line
 * file cap while the cohesive coverage suite remains one binary. The
 * mock binds directly into ``g_sdmmc_spi_state.transport`` through the
 * internal header (see the test file's @details for why the
 * include-the-.c pattern cannot be used here). Every fault is injected
 * deterministically by a call-index counter -- no timers, no SIGALRM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_sdmmc_spi_internal.h"
#include "unity_minimal.h"

/**
 * @enum sdmmc_spi_cov_test_util_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_sdmmc_spi_cov_test_util_out_40 =
    0x40U, /**< CSD byte 0: CSD_STRUCTURE = 1 (version bits 7:6 = 0b01). */
  k_sdmmc_spi_cov_test_util_tail_word_24 = 24U, /**< Top-byte shift of little-endian tail word. */
  /** CSD v2 byte offset of the low C_SIZE byte. */
  k_sdmmc_spi_cov_test_util_csd_off_csize_lo = 9,
  k_sdmmc_spi_cov_test_util_byte_mask = 0xFFU, /**< All-ones byte: C_SIZE fill and low-byte mask. */
} sdmmc_spi_cov_test_util_uint8_const_t;

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
  k_cov_buf_bytes   = 1024U,        /**< Queue capacity.  */
  k_cov_idle_byte   = 0xFFU,        /**< Idle byte.       */
  k_cov_wake_bytes  = 10U,          /**< Wake-byte count. */
  k_cov_frame_bytes = 6U,           /**< Frame length.    */
  k_cov_cmd8_echo   = 0x000001AAUL, /**< CMD8 echo.       */
  k_cov_ocr_ccs     = 0xC0FF8000UL, /**< SDHC OCR.        */
  k_cov_ocr_no_ccs  = 0x80FF8000UL, /**< SDSC OCR.        */
  k_cov_bad_echo    = 0xDEADBEEFUL, /**< Invalid echo.    */
} cov_mock_size_t;

/**
 * @enum cov_byte_t
 * @brief Response byte fixtures the mock feeds back to the driver.
 */
typedef enum : uint8_t {
  k_cov_r1_idle    = 0x01U, /**< Idle R1.            */
  k_cov_r1_ready   = 0x00U, /**< Ready R1.           */
  k_cov_r1_illegal = 0x04U, /**< Illegal-command R1. */
  k_cov_r1_bad     = 0x04U, /**< Error R1.           */
  k_cov_token_data = 0xFEU, /**< Data token.         */
  k_cov_cmd_bit    = 0x40U, /**< Command marker.     */
} cov_byte_t;

/**
 * @struct cov_mock_t
 * @brief Per-test mock state.
 */
typedef struct {
  uint8_t  rx_queue[k_cov_buf_bytes]; /**< Queued response bytes.   */
  uint32_t rx_len;                    /**< Queue length.            */
  uint32_t rx_pos;                    /**< Queue cursor.            */
  uint32_t xfer_calls;                /**< Transfer calls.          */
  uint32_t xfer_fail_at;              /**< Transfer fault index.    */
  uint32_t cs_calls;                  /**< Chip-select calls.       */
  uint32_t cs_fail_at;                /**< Chip-select fault index. */
  uint8_t  smart_cur_cmd;             /**< Current command.         */
  bool     smart_pending_r1;          /**< Pending response flag.   */
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 */
RA8_INTERNAL static inline void internal_mock_reset(void)
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 */
RA8_INTERNAL static inline void internal_mock_queue_byte(uint8_t b)
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 */
RA8_INTERNAL static inline void internal_mock_queue_idle(uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    internal_mock_queue_byte((uint8_t)k_cov_idle_byte);
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 * @post Fixture state remains valid for the next scripted step.
 */
RA8_INTERNAL static inline ra8_err_t internal_mock_set_clock(void* ctx, uint32_t hz)
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 * @post Fixture state remains valid for the next scripted step.
 */
RA8_INTERNAL static inline ra8_err_t internal_mock_cs(void* ctx, bool asserted)
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
 * @return ::k_ra8_ok, or ::k_ra8_err_hw_timeout once the fault index is
 * reached.
 * @retval k_ra8_ok            Bytes exchanged from the queue.
 * @retval k_ra8_err_hw_timeout Armed fault fired (this and later calls).
 * @pre @p rx is either NULL or holds @p len bytes.
 * @pre The mock is bound into the driver transport.
 * @post rx_pos advances by @p len on success.
 * @post xfer_calls grows by one.
 * @note Test-only helper.
 * @since 0.1.0
 * @details Supports deterministic SD protocol vectors without hardware access.
 */
RA8_INTERNAL static inline ra8_err_t
internal_mock_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
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
 * @return ::k_ra8_err_not_supported on NULL tx, else ::internal_mock_xfer's
 * result.
 * @retval k_ra8_err_not_supported NULL tx (drives read_r3_or_r7_tail fallback).
 * @retval k_ra8_ok               Non-NULL tx exchanged.
 * @pre The mock is bound into the driver transport.
 * @post On the non-NULL path, ::internal_mock_xfer's postconditions hold.
 * @note Test-only helper.
 * @since 0.1.0
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The caller owns the process-local fixture.
 * @post Fixture state remains valid for the next scripted step.
 */
RA8_INTERNAL static inline ra8_err_t
internal_mock_xfer_no_nulltx(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  if (tx == nullptr) {
    return k_ra8_err_not_supported;
  }
  return internal_mock_xfer(ctx, tx, rx, len);
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
 * @pre The caller owns the process-local fixture.
 * @post Fixture state remains valid for the next scripted step.
 */
RA8_INTERNAL static inline ra8_err_t
internal_smart_xfer_acmd41(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
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
  .set_clock = internal_mock_set_clock,
  .cs        = internal_mock_cs,
  .xfer      = internal_mock_xfer,
  .ctx       = nullptr,
};

/** @brief Transport whose NULL-tx bulk read fails (drives the tail fallback).
 */
static const ra8_sdmmc_spi_transport_t s_tr_nonulltx = {
  .set_clock = internal_mock_set_clock,
  .cs        = internal_mock_cs,
  .xfer      = internal_mock_xfer_no_nulltx,
  .ctx       = nullptr,
};

/** @brief Command-aware transport that never lets ACMD41 report ready. */
static const ra8_sdmmc_spi_transport_t s_tr_smart = {
  .set_clock = internal_mock_set_clock,
  .cs        = internal_mock_cs,
  .xfer      = internal_smart_xfer_acmd41,
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
 * @details Supports deterministic SD protocol vectors without hardware access.
 */
RA8_INTERNAL static inline void internal_cov_bind(const ra8_sdmmc_spi_transport_t* tr)
{
  (void)ra8_sdmmc_spi_deinit();
  internal_mock_reset();
  g_sdmmc_spi_state.transport = *tr;
}

/* ===========================================================================
 * Stage-response queue helpers (v2 SDHC init flow)
 * ===========================================================================
 */

/**
 * @brief Queue one CS-bracketed command whose response is a single R1 byte.
 * @param[in] r1 R1 token the card returns.
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_q_cmd_r1(uint8_t r1)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_frame_bytes);
  internal_mock_queue_byte(r1);
  internal_mock_queue_idle(1U);
}

/**
 * @brief Queue one CS-bracketed command with an R1 + 4-byte R3/R7 tail.
 * @param[in] r1        R1 token the card returns.
 * @param[in] tail_word 32-bit R3/R7 tail (MSB first on the wire).
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_q_cmd_r3r7(uint8_t r1, uint32_t tail_word)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_frame_bytes);
  internal_mock_queue_byte(r1);
  internal_mock_queue_byte((uint8_t)((tail_word >> k_sdmmc_spi_cov_test_util_tail_word_24) &
                                     k_sdmmc_spi_cov_test_util_byte_mask));
  internal_mock_queue_byte((uint8_t)((tail_word >> 16U) & k_sdmmc_spi_cov_test_util_byte_mask));
  internal_mock_queue_byte((uint8_t)((tail_word >> 8U) & k_sdmmc_spi_cov_test_util_byte_mask));
  internal_mock_queue_byte((uint8_t)(tail_word & k_sdmmc_spi_cov_test_util_byte_mask));
  internal_mock_queue_idle(1U);
}

/**
 * @brief Populate a CSD v2 register reporting a 32 GiB SDHC card.
 * @param[out] out 16-byte CSD buffer.
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_build_csd_v2(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0] = k_sdmmc_spi_cov_test_util_out_40; /* CSD_STRUCTURE = 1 (version bits
                                                7:6 == 0b01). */
  out[8] = k_sdmmc_spi_cov_test_util_byte_mask;
  out[k_sdmmc_spi_cov_test_util_csd_off_csize_lo] = k_sdmmc_spi_cov_test_util_byte_mask;
}

/**
 * @brief Queue the CMD9 CSD read (R1==0, data token, 16 CSD bytes, CRC16).
 * @param[in] csd 16-byte CSD payload to deliver.
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_q_csd(const uint8_t* csd)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_frame_bytes);
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);
  internal_mock_queue_byte((uint8_t)k_cov_token_data);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_csd_response_len; i++) {
    internal_mock_queue_byte(csd[i]);
  }
  internal_mock_queue_byte(0U);
  internal_mock_queue_byte(0U);
  internal_mock_queue_idle(1U); /* cs_release post-byte. */
}

/**
 * @brief Queue the wake + CMD0 + CMD8(v2) + ACMD41 prefix (through readiness).
 *
 * @details CMD55 and ACMD41 share one CS bracket (no cs toggle between them),
 * so the ACMD41 stage is queued as a single tight block: one cs_assert idle,
 * the CMD55 frame + R1, the ACMD41 frame + R1, and one cs_release idle. Each
 * R1 token sits immediately after its 6-byte frame so ``internal_read_r1``
 * resolves it in one xfer call, keeping downstream fault indices exact.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_q_prefix_through_acmd41(void)
{
  internal_mock_queue_idle((uint32_t)k_cov_wake_bytes);
  internal_q_cmd_r1((uint8_t)k_cov_r1_idle);
  internal_q_cmd_r3r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_cmd8_echo);
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_frame_bytes);
  internal_mock_queue_byte((uint8_t)k_cov_r1_idle);
  internal_mock_queue_idle((uint32_t)k_cov_frame_bytes);
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);
  internal_mock_queue_idle(1U);
}

/**
 * @brief Queue the prefix through the CMD58 OCR read.
 * @param[in] ocr OCR word the card returns (CCS bit selects SDHC vs SDSC).
 * @details Supports deterministic SD protocol vectors without hardware access.
 * @pre The bounded mock fixture storage is initialized.
 * @pre The caller owns the process-local fixture.
 * @post The requested bounded fixture operation is complete.
 * @post Fixture state remains valid for the next scripted step.
 * @note This helper performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_q_prefix_through_ocr(uint32_t ocr)
{
  internal_q_prefix_through_acmd41();
  internal_q_cmd_r3r7((uint8_t)k_cov_r1_ready, ocr); /* CMD58 OCR. */
}

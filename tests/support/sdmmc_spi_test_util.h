/**
 * @file sdmmc_spi_test_util.h
 * @brief Shared fixture for the test_ra8_sdmmc_spi* suite: the mock SPI
 *        transport (request/response byte queue + TX capture), the
 *        SDHC full-init response scripts, and the common init helper
 *
 * @details Header-only (all definitions `static`) so each split
 * test_ra8_sdmmc_spi* binary carries its own private copy of the mock
 * state; the tests/CMakeLists.txt auto-glob stays free of non-test .c
 * files. Split out of test_ra8_sdmmc_spi.c when the suite was divided
 * into core / read / write binaries.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Mock SPI transport
 * ===========================================================================
 *
 * The mock is a tiny request/response queue: each test pre-loads an
 * array of "response bytes" that the transport hands back, byte for
 * byte, on every full-duplex exchange. The mock also records every
 * outbound byte the driver writes so individual tests can spot-check
 * the command framing.
 */

typedef enum : uint32_t {
  k_mock_buf_bytes      = 4096U, /**< Max captured + queued bytes per test. */
  k_mock_xfer_byte_idle = 0xFFU, /**< Mock xfer byte idle.                  */
} mock_const_t;

/**
 * @struct mock_spi_t
 * @brief Per-test SPI mock state.
 */
typedef struct {
  uint8_t  rx_queue[k_mock_buf_bytes]; /**< Bytes the driver "reads" next.            */
  uint32_t rx_len;                     /**< Bytes loaded into rx_queue.               */
  uint32_t rx_pos;                     /**< Next index inside rx_queue.               */
  uint8_t  tx_log[k_mock_buf_bytes];   /**< Every byte the driver writes.             */
  uint32_t tx_len;                     /**< Outbound bytes recorded.                  */
  uint32_t clock_hz;                   /**< Most recent set_clock call.               */
  bool     cs_asserted;                /**< Current CS line state.                    */
  uint32_t cs_assert_count;            /**< Total CS->low transitions.                */
  uint32_t xfer_calls;                 /**< Total transport.xfer calls.               */
  uint32_t xfer_fail_at;               /**< 1-based xfer call to fail at; 0 disables. */
} mock_spi_t;

static mock_spi_t s_mock = {};

static inline void mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
}

static inline void mock_queue_byte(uint8_t b)
{
  if (s_mock.rx_len < (uint32_t)k_mock_buf_bytes) {
    s_mock.rx_queue[s_mock.rx_len] = b;
    s_mock.rx_len++;
  }
}

static inline void mock_queue_bytes(const uint8_t* data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    mock_queue_byte(data[i]);
  }
}

/**
 * @brief Queue ``count`` copies of the idle byte 0xFF.
 */
static inline void mock_queue_idle(uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    mock_queue_byte((uint8_t)k_mock_xfer_byte_idle);
  }
}

static inline ra8_err_t mock_set_clock(void* ctx, uint32_t hz)
{
  (void)ctx;
  s_mock.clock_hz = hz;
  return k_ra8_ok;
}

static inline ra8_err_t mock_cs(void* ctx, bool asserted)
{
  (void)ctx;
  s_mock.cs_asserted = asserted;
  if (asserted) {
    s_mock.cs_assert_count++;
  }
  return k_ra8_ok;
}

static inline ra8_err_t mock_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  s_mock.xfer_calls++;
  /* Deterministic fault injection: once the call counter reaches the armed
   * index, every subsequent xfer reports a bus timeout. Tests reset the
   * counter via mock_arm_xfer_fail() right before the call under test so the
   * failure lands on a known step (no SIGALRM / timing involved). */
  if ((s_mock.xfer_fail_at != 0U) && (s_mock.xfer_calls >= s_mock.xfer_fail_at)) {
    return k_ra8_err_hw_timeout;
  }
  for (uint32_t i = 0U; i < len; i++) {
    uint8_t out = (uint8_t)k_mock_xfer_byte_idle;
    if (tx != nullptr) {
      out = tx[i];
    }
    if (s_mock.tx_len < (uint32_t)k_mock_buf_bytes) {
      s_mock.tx_log[s_mock.tx_len] = out;
      s_mock.tx_len++;
    }
    uint8_t in = (uint8_t)k_mock_xfer_byte_idle;
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

static const ra8_sdmmc_spi_transport_t s_mock_transport = {
  .set_clock = mock_set_clock,
  .cs        = mock_cs,
  .xfer      = mock_xfer,
  .ctx       = nullptr,
};

/**
 * @enum mock_bulk_const_t
 * @brief Threshold above which the no-bulk transport refuses a transfer.
 */
typedef enum : uint32_t {
  k_mock_bulk_min_len = 256U, /**< len >= this is treated as a block-sized bulk write. */
} mock_bulk_const_t;

/**
 * @brief xfer shim that refuses block-sized bulk writes.
 *
 * @details Models a real transport (some HALs reject a NULL rx on a long
 * write), forcing the driver's per-byte write fallback. Small frames pass
 * straight through to ::mock_xfer so the init sequence is unaffected.
 */
static inline ra8_err_t mock_xfer_no_bulk(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  if (len >= (uint32_t)k_mock_bulk_min_len) {
    return k_ra8_err_not_supported;
  }
  return mock_xfer(ctx, tx, rx, len);
}

static const ra8_sdmmc_spi_transport_t s_mock_transport_no_bulk = {
  .set_clock = mock_set_clock,
  .cs        = mock_cs,
  .xfer      = mock_xfer_no_bulk,
  .ctx       = nullptr,
};

/** @brief Call counter for ::mock_set_clock_fail_second. */
static uint32_t s_setclk_calls = 0U;

/**
 * @brief set_clock shim that succeeds the first time then fails.
 *
 * @details The init clock (call 1, prepare-init) succeeds and the data clock
 * (call 2, finalize-init) fails -- the only place set_clock is invoked twice
 * across one ::ra8_sdmmc_spi_init, so this exercises the finalize-init error leg.
 */
static inline ra8_err_t mock_set_clock_fail_second(void* ctx, uint32_t hz)
{
  (void)ctx;
  s_mock.clock_hz = hz;
  s_setclk_calls++;
  if (s_setclk_calls >= 2U) {
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}

static const ra8_sdmmc_spi_transport_t s_mock_transport_failclk = {
  .set_clock = mock_set_clock_fail_second,
  .cs        = mock_cs,
  .xfer      = mock_xfer,
  .ctx       = nullptr,
};

/**
 * @brief Arm the xfer fault injector to fail starting at the @p nth call.
 * @param[in] nth 1-based xfer call index to begin failing at (0 disables).
 */
static inline void mock_arm_xfer_fail(uint32_t nth)
{
  s_mock.xfer_calls   = 0U;
  s_mock.xfer_fail_at = nth;
}

/* ===========================================================================
 * SD response builder helpers
 * ===========================================================================
 */

typedef enum : uint8_t {
  k_test_r1_idle              = 0x01U, /**< Test r1 idle.                     */
  k_test_r1_ready             = 0x00U, /**< Test r1 ready.                    */
  k_test_r1_illegal_cmd       = 0x05U, /**< Test r1 illegal cmd.              */
  k_test_data_token_start     = 0xFEU, /**< Test data token start.            */
  k_test_data_response_accept = 0x05U, /**< Test data response accept.        */
  k_test_busy_done            = 0xFFU, /**< Not-busy token after CMD38 erase. */
} test_resp_const_t;

typedef enum : uint32_t {
  k_test_cmd8_echo       = 0x000001AAUL, /**< Test cmd8 echo.                 */
  k_test_ocr_ccs_ready   = 0xC0FF8000UL, /**< busy=1 | CCS=1 | voltage window */
  k_test_cmd_frame_bytes = 6U,           /**< Test cmd frame bytes.           */
  /* Allow a generous post-frame N_CR gap so the driver's R1 polling
   * never times out -- the mock queues idle 0xFF bytes for every clock
   * after the command frame until the response token is queued. */
  k_test_r1_padding_bytes = 1U, /**< Test r1 padding bytes. */
} test_resp_int_const_t;

/**
 * @brief Push the bytes the driver will consume for one CS-bracketed command.
 *
 * @details
 * The driver clocks: 1 idle (cs_assert post-byte), 6 cmd bytes, then
 * an R1 wait (1..N idle bytes followed by the R1 token), then 1 idle
 * (cs_release post-byte). We queue all 9 explicitly here -- the
 * cs_release idle has to be a real queue entry, not mock-underflow
 * default-fill, because the mock's xfer always advances rx_pos on
 * every byte regardless of CS state. Without it, the next command's
 * queued bytes get consumed by the current cs_release read and
 * everything shifts by one (off-by-one tracing surfaced this when
 * `internal_send_cmd8` saw a misaligned R7 echo tail).
 */
static inline void queue_command_response_r1(uint8_t r1)
{
  /* cs_assert post-byte. */
  mock_queue_idle(1U);
  /* The 6-byte command itself shifts in 0xFFs (no response there). */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  /* R1 token. */
  mock_queue_byte(r1);
  /* cs_release post-byte: ra8_sdmmc_spi's `internal_cs_release` clocks
   * one trailing idle byte after deasserting CS (SD spec PHY v9
   * section 7.2.4). The mock's xfer reads rx for every transmitted
   * byte, so without an explicit trailing idle here the next command's
   * queued bytes would be misaligned by one position. */
  mock_queue_idle(1U);
}

static inline void queue_command_response_r3_or_r7(uint8_t r1, uint32_t tail_word)
{
  /* Same as queue_command_response_r1 but inserts the 4-byte R3/R7
   * tail BEFORE the cs_release post-byte. */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte(r1);
  mock_queue_byte((uint8_t)((tail_word >> 24U) & 0xFFU));
  mock_queue_byte((uint8_t)((tail_word >> 16U) & 0xFFU));
  mock_queue_byte((uint8_t)((tail_word >> 8U) & 0xFFU));
  mock_queue_byte((uint8_t)(tail_word & 0xFFU));
  /* cs_release post-byte (see queue_command_response_r1 comment). */
  mock_queue_idle(1U);
}

/**
 * @brief Build a CSD v2 register reporting a 32 GiB SDHC card.
 *
 * @details
 * CSD v2 layout (SD spec PHY v9 section 5.3.3):
 *   - byte 0 bits 7:6 = CSD_STRUCTURE = 1.
 *   - bytes 7..9 = C_SIZE (22 bits; top 6 bits of byte 7 are reserved).
 * For a 32 GiB card, capacity = 0x10000 + 1 entries of 512 KiB.
 */
static inline void build_csd_v2_32gib(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0] = 0x40U; /* CSD_STRUCTURE = 1 */
  out[7] = 0x00U;
  out[8] = 0xFFU; /* low 16 bits of C_SIZE = 0xFF FF */
  out[9] = 0xFFU;
}

static inline void queue_csd_read(const uint8_t* csd)
{
  /* Inline R1-phase: 1 cs_assert idle + 6 CMD9 frame idles + 1 R1.
   * NOT the cs_release-padded variant because CMD9 leaves CS asserted
   * across the R1 -> data-token -> CSD body -> CRC sequence (single
   * CS-bracketed read). The driver-side `internal_cs_release` runs
   * once at the very end, after the CRC bytes -- handled by the
   * trailing idle below. */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_data_token_start);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_csd_response_len; i++) {
    mock_queue_byte(csd[i]);
  }
  /* Two trailing CRC16 bytes -- value doesn't matter, driver doesn't
   * verify CRC16 on the CSD read. */
  mock_queue_byte(0U);
  mock_queue_byte(0U);
  /* cs_release post-byte (same rationale as queue_command_response_r1). */
  mock_queue_idle(1U);
}

/**
 * @brief Walk the driver through a successful CMD0..CMD16 init sequence.
 *
 * Helper used by the block-I/O tests that don't care about init coverage. SDHC
 * v2 card with 32 GiB capacity.
 */
static inline void queue_full_init_sdhc_32gib(void)
{
  /* Wake-up dummy clocks (10 idle bytes during the 80-clock kick). */
  mock_queue_idle(10U);
  /* CMD0 -> R1 idle (0x01). */
  queue_command_response_r1((uint8_t)k_test_r1_idle);
  /* CMD8 -> R1 idle + echoed pattern. */
  queue_command_response_r3_or_r7((uint8_t)k_test_r1_idle, (uint32_t)k_test_cmd8_echo);
  /* ACMD41 -> CMD55 R1 idle + ACMD41 R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_idle);  /* CMD55 R1  */
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* ACMD41 R1 */
  /* CMD58 -> R1 ready + OCR with CCS bit set. */
  queue_command_response_r3_or_r7((uint8_t)k_test_r1_ready, (uint32_t)k_test_ocr_ccs_ready);
  /* CMD9 -> CSD v2 (32 GiB). */
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v2_32gib(csd);
  queue_csd_read(csd);
  /* CMD16 (SET_BLOCKLEN) -> R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
}

/* ===========================================================================
 * Setup / teardown
 * ===========================================================================
 */

static inline void per_test_setup(void)
{
  mock_reset();
  (void)ra8_sdmmc_spi_deinit();
}

/**
 * @brief Queue the bytes that follow CMD24 R1 during a write-block
 *        round-trip: N_WR pad, data-token slot, 512 payload echoes,
 *        2 CRC slots, then the data-response token + busy-wait sequence.
 *
 * @details
 * The driver clocks 1 (N_WR) + 1 (data token TX) + 512 (payload TX) +
 * 2 (CRC TX) = 516 bytes BEFORE reading the data-response token byte,
 * and we have to drain those slots from the RX queue (the mock advances
 * its read pointer once per clocked byte regardless of whether the
 * driver discards RX). Filling them all with idle 0xFF matches the
 * real SD card behaviour (CIPO held high while the host shifts data).
 */
static inline void queue_write_block_tail(uint8_t data_response, uint32_t busy_bytes)
{
  /* Note: the preceding `queue_command_response_r1` for the CMD24 R1
   * already leaves one trailing idle (its cs_release post-byte) that
   * the driver's `internal_write_data_phase` consumes as the N_WR pad
   * read, so this helper does NOT queue an additional N_WR pad. */
  mock_queue_idle(1U);                                   /* data-token TX slot.   */
  mock_queue_idle((uint32_t)k_ra8_sdmmc_spi_block_size); /* 512 payload TX slots. */
  mock_queue_idle(2U);                                   /* 2 CRC TX slots.       */
  mock_queue_byte(data_response);                        /* response token byte.  */
  for (uint32_t i = 0U; i < busy_bytes; i++) {
    mock_queue_byte(0x00U); /* card busy 0x00. */
  }
  mock_queue_byte((uint8_t)k_mock_xfer_byte_idle); /* busy released 0xFF. */
}

/**
 * @brief Queue an ra8_sdmmc_spi_read_block response delivering @p block.
 *
 * @details Models the CMD17 read the erase path uses to verify the post-erase
 * value: R1 + data-start token + 512 payload + a correct CRC16 trailer.
 */
static inline void queue_read_back(const uint8_t* block)
{
  /* ra8_sdmmc_spi_read_block holds CS across CMD17 + the data phase (one
   * session: cs_assert -> read_data_phase -> cs_release), so we must NOT
   * use queue_command_response_r1 here -- it injects a cs_release idle byte
   * between R1 and the data, which starves the data-token wait and trips
   * k_ra8_err_hw_timeout. Model the session directly. */
  mock_queue_idle(1U);                               /* cs_assert post-byte.     */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes); /* CMD17 frame shifts 0xFF. */
  mock_queue_byte((uint8_t)k_test_r1_ready);         /* CMD17 R1.                */
  mock_queue_byte((uint8_t)k_test_data_token_start); /* data-start token.        */
  mock_queue_bytes(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  const uint16_t crc = ra8_sdmmc_spi_crc16(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  mock_queue_byte((uint8_t)((crc >> 8U) & 0xFFU));
  mock_queue_byte((uint8_t)(crc & 0xFFU));
  mock_queue_idle(1U); /* cs_release post-byte. */
}

/**
 * @brief Bring the driver up against an SDHC 32 GiB mock and assert success.
 *
 * @details Shorthand used by the block-I/O error tests below: resets the mock,
 * queues the full CMD0..CMD16 identification sequence, and initialises.
 */
static inline void init_sdhc_ok(void)
{
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
}

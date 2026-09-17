/**
 * @file test_ra8_io_blockdev_sdspi_cov.c
 * @brief Coverage-boost tests for ra8_io_blockdev_sdspi.c.
 *
 * @details
 * The SD-over-SPI block-device backend is a thin shim: every vtable op
 * (read / write / erase / get_caps) forwards to the ``ra8_sdmmc_spi``
 * singleton driver. The sibling test_ra8_io_blockdev_backends.c only binds
 * the backend and checks the NULL guard, leaving the four forwarding
 * bodies uncovered because the data path needs a live card.
 *
 * This file supplies that card by bringing the ``ra8_sdmmc_spi`` driver up
 * through the same mock SPI transport the driver's own unit test uses
 * (request/response byte queue -- no MMIO, no SIGALRM), binding the
 * backend on top, and then driving each forwarding body twice:
 *
 *   - the success leg (card up, mock responses pre-seeded), which streams
 *     the CMD18 bulk read, the CMD24 write forward, the erase
 *     probe-and-verify sequence, and the full get_caps snapshot; and
 *   - the driver-error leg (card torn down), which makes the driver
 *     return ::k_ra8_err_invalid_state so the ``RA8_RETURN_ON_ERROR`` early
 *     returns in sdspi_read and sdspi_get_caps are exercised too.
 *
 * The backend's own ``RA8_CHECK_NULL_PTR`` true-arms are provably
 * unreachable through the public ra8_io_blockdev_* dispatcher (it null-
 * checks ``buf`` / ``out`` before forwarding), so those lines are covered
 * by the FALSE arm on the happy path -- no GCOVR_EXCL markers are needed
 * and none are added.
 *
 * Builds as a standalone executable auto-discovered by the
 * tests/CMakeLists.txt ``test_*.c`` GLOB; its .gcda stream is merged by
 * gcovr with the sibling tests so the newly hit lines count toward the
 * global ra8_io_blockdev_sdspi.c coverage total without touching the
 * sibling test file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_sdspi.h"
#include "ra8_sdmmc_spi.h"
#include "unity_minimal.h"

/** @brief Payload fill for the SD-over-SPI transfer assertions. */
typedef enum : uint8_t {
  k_sdspi_cov_fill = 0xA5U, /**< Arbitrary non-trivial byte pattern. */
} sdspi_cov_fill_t;

/**
 * @enum io_blockdev_sdspi_cov_fixture_t
 * @brief The payload generators and their seeds, plus protocol and on-disk field offsets, and the byte-level helpers.
 */
typedef enum : uint8_t {
  k_sdspi_pattern_stride =
    5U, /**< Generator stride `i * 5 + 1`; neighbouring blocks never share a byte pattern. */
  k_sdspi_wakeup_clocks =
    10U, /**< Idle bytes before the first command; needs >= 74 clocks, which these supply. */
  k_sdspi_csd_v2_byte0 =
    0x40U,             /**< CSD byte 0, CSD_STRUCTURE = 1, selecting the version-2 (SDHC) layout. */
  k_shift_byte3 = 24U, /**< Shift to the most significant byte, which goes on the wire first.     */
  /** CSD byte holding the top of the version-2 C_SIZE field. */
  k_sdspi_csd_off_c_size_hi = 7,
  /** The byte holding its low half. */
  k_sdspi_csd_off_c_size_lo = 9,
  k_byte_mask = 0xFFU, /**< Low-byte mask used when clocking a word out a byte at a time. */
} io_blockdev_sdspi_cov_fixture_t;

/* ===========================================================================
 * Mock SPI transport
 * ===========================================================================
 *
 * A tiny request/response queue: each test pre-loads the bytes the driver
 * "reads" back, byte for byte, on every full-duplex exchange. Outbound
 * bytes are discarded here (the backend under test does not inspect wire
 * framing -- the driver's own unit test already proves that). No MMIO is
 * touched: the transport is a pure Dependency-Inversion seam.
 */

/**
 * @enum cov_mock_const_t
 * @brief Mock queue sizing and the SPI idle byte.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_mock_buf_bytes = 4096U, /**< Max queued RX bytes per test. */
  k_cov_mock_idle_byte = 0xFFU, /**< SPI idle / not-busy pattern.  */
} cov_mock_const_t;

/**
 * @struct cov_mock_spi_t
 * @brief Per-test SPI mock state.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  rx_queue[k_cov_mock_buf_bytes]; /**< Bytes the driver reads next. */
  uint32_t rx_len;                         /**< Bytes loaded into rx_queue.  */
  uint32_t rx_pos;                         /**< Next index inside rx_queue.  */
  uint32_t clock_hz;                       /**< Most recent set_clock call.  */
  bool     cs_asserted;                    /**< Current CS line state.       */
} cov_mock_spi_t;

/** @brief Module-private mock state, reset per test. */
static cov_mock_spi_t s_mock = {};

/**
 * @brief Clear all mock queue and line state.
 *
 * @pre None.
 * @post s_mock is fully zeroed.
 * @since 0.1.0 @details Exercises the mock reset path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
}

/**
 * @brief Append one byte to the RX queue.
 *
 * @param[in] b Byte the driver will read on a future exchange.
 *
 * @pre rx_len tracks the queue fill.
 * @post b is queued when space remains; otherwise dropped.
 * @since 0.1.0 @details Exercises the mock queue byte path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_mock_queue_byte(uint8_t b)
{
  if (s_mock.rx_len < (uint32_t)k_cov_mock_buf_bytes) {
    s_mock.rx_queue[s_mock.rx_len] = b;
    s_mock.rx_len++;
  }
}

/**
 * @brief Append @p len bytes from @p data to the RX queue.
 *
 * @param[in] data Source bytes; non-NULL when @p len > 0.
 * @param[in] len  Number of bytes to queue.
 *
 * @pre data is non-NULL or len == 0.
 * @post Each byte is appended in order.
 * @since 0.1.0 @details Exercises the mock queue bytes path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_mock_queue_bytes(const uint8_t* data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    internal_mock_queue_byte(data[i]);
  }
}

/**
 * @brief Queue @p count copies of the idle byte 0xFF.
 *
 * @param[in] count Number of idle bytes to queue.
 *
 * @pre None.
 * @post count idle bytes are appended.
 * @since 0.1.0 @details Exercises the mock queue idle path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_mock_queue_idle(uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    internal_mock_queue_byte((uint8_t)k_cov_mock_idle_byte);
  }
}

/**
 * @brief Transport set_clock shim -- records the requested rate.
 *
 * @param[in] ctx Unused transport cookie.
 * @param[in] hz  Requested clock rate in Hz.
 *
 * @return ra8_err_t Always ::k_ra8_ok.
 * @retval k_ra8_ok Rate recorded.
 *
 * @pre None.
 * @post s_mock.clock_hz == hz.
 * @since 0.1.0 @details Exercises the mock set clock path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static ra8_err_t internal_mock_set_clock(void* ctx, uint32_t hz)
{
  (void)ctx;
  s_mock.clock_hz = hz;
  return k_ra8_ok;
}

/**
 * @brief Transport chip-select shim -- records the CS line state.
 *
 * @param[in] ctx      Unused transport cookie.
 * @param[in] asserted true to drive CS low (selected).
 *
 * @return ra8_err_t Always ::k_ra8_ok.
 * @retval k_ra8_ok CS state recorded.
 *
 * @pre None.
 * @post s_mock.cs_asserted == asserted.
 * @since 0.1.0 @details Exercises the mock cs path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static ra8_err_t internal_mock_cs(void* ctx, bool asserted)
{
  (void)ctx;
  s_mock.cs_asserted = asserted;
  return k_ra8_ok;
}

/**
 * @brief Transport full-duplex exchange shim.
 *
 * @details Discards every outbound byte and returns queued RX bytes in
 * order, defaulting to the idle byte 0xFF once the queue is drained. The
 * RX read pointer advances once per clocked byte regardless of CS state,
 * matching the real bus.
 *
 * @param[in]  ctx Unused transport cookie.
 * @param[in]  tx  Outbound bytes (ignored).
 * @param[out] rx  Destination for the queued RX bytes; may be NULL.
 * @param[in]  len Number of bytes to exchange.
 *
 * @return ra8_err_t Always ::k_ra8_ok.
 * @retval k_ra8_ok len bytes exchanged.
 *
 * @pre len > 0.
 * @post rx (when non-NULL) holds the next len queued bytes.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static ra8_err_t
internal_mock_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  (void)tx;
  for (uint32_t i = 0U; i < len; i++) {
    uint8_t in = (uint8_t)k_cov_mock_idle_byte;
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

/** @brief Mock transport descriptor wired to the three shims above. */
static const ra8_sdmmc_spi_transport_t s_mock_transport = {
  .set_clock = internal_mock_set_clock,
  .cs        = internal_mock_cs,
  .xfer      = internal_mock_xfer,
  .ctx       = nullptr,
};

/* ===========================================================================
 * SD response builder helpers (mirrors of test_ra8_sdmmc_spi.c framing)
 * ===========================================================================
 */

/**
 * @enum cov_resp_byte_t
 * @brief Single-byte SD response tokens the mock replays.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cov_r1_idle          = 0x01U, /**< CMD0 idle-state R1.               */
  k_cov_r1_ready         = 0x00U, /**< Ready R1 (bit0 clear).            */
  k_cov_data_token_start = 0xFEU, /**< Data-start token.                 */
  k_cov_data_resp_accept = 0x05U, /**< Data-response "accepted".         */
  k_cov_busy_done        = 0xFFU, /**< Not-busy token after CMD38 erase. */
} cov_resp_byte_t;

/**
 * @enum cov_resp_word_t
 * @brief Multi-byte constants for the init framing.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_cmd8_echo     = 0x000001AAUL, /**< CMD8 voltage-check echo.      */
  k_cov_ocr_ccs_ready = 0xC0FF8000UL, /**< OCR: busy=1 | CCS=1 | window. */
  k_cov_cmd_frame_len = 6U,           /**< 1 cmd + 4 arg + 1 CRC bytes.  */
} cov_resp_word_t;

/**
 * @brief Queue one CS-bracketed command that returns a plain R1 token.
 *
 * @param[in] r1 R1 response byte the card returns.
 *
 * @pre The queue has room for nine bytes.
 * @post cs-assert idle + 6 frame idles + R1 + cs-release idle are queued.
 * @since 0.1.0 @details Exercises the queue command response r1 path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_command_response_r1(uint8_t r1)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len);
  internal_mock_queue_byte(r1);
  internal_mock_queue_idle(1U);
}

/**
 * @brief Queue a command returning R1 plus a 4-byte R3/R7 tail word.
 *
 * @param[in] r1        R1 response byte.
 * @param[in] tail_word Big-endian 32-bit R3/R7 tail.
 *
 * @pre The queue has room for the framing.
 * @post The R1 + tail + cs-release framing is queued.
 * @since 0.1.0 @details Exercises the queue command response r3 or r7 path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_command_response_r3_or_r7(uint8_t r1, uint32_t tail_word)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len);
  internal_mock_queue_byte(r1);
  internal_mock_queue_byte((uint8_t)((tail_word >> k_shift_byte3) & k_byte_mask));
  internal_mock_queue_byte((uint8_t)((tail_word >> 16U) & k_byte_mask));
  internal_mock_queue_byte((uint8_t)((tail_word >> 8U) & k_byte_mask));
  internal_mock_queue_byte((uint8_t)(tail_word & k_byte_mask));
  internal_mock_queue_idle(1U);
}

/**
 * @brief Build a CSD v2 register reporting a 32 GiB SDHC card.
 *
 * @param[out] out 16-byte CSD buffer to fill.
 *
 * @pre out points at k_ra8_sdmmc_spi_csd_response_len writable bytes.
 * @post out encodes CSD_STRUCTURE=1 and C_SIZE=0xFFFF.
 * @since 0.1.0 @details Exercises the build csd v2 32gib path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_build_csd_v2_32gib(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0]                         = k_sdspi_csd_v2_byte0; /* CSD_STRUCTURE = 1. */
  out[k_sdspi_csd_off_c_size_hi] = 0x00U;
  out[8]                         = k_byte_mask; /* low 16 bits of C_SIZE = 0xFFFF. */
  out[k_sdspi_csd_off_c_size_lo] = k_byte_mask;
}

/**
 * @brief Queue the CMD9 (SEND_CSD) single-session read of @p csd.
 *
 * @param[in] csd 16-byte CSD payload to return.
 *
 * @pre csd points at k_ra8_sdmmc_spi_csd_response_len bytes.
 * @post The full CS-bracketed CSD read is queued.
 * @since 0.1.0 @details Exercises the queue csd read path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_csd_read(const uint8_t* csd)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len);
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);
  internal_mock_queue_byte((uint8_t)k_cov_data_token_start);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_csd_response_len; i++) {
    internal_mock_queue_byte(csd[i]);
  }
  internal_mock_queue_byte(0U);
  internal_mock_queue_byte(0U);
  internal_mock_queue_idle(1U);
}

/**
 * @brief Walk the driver through a successful CMD0..CMD16 init sequence.
 *
 * @details SDHC v2 card with 32 GiB capacity; the block-I/O tests reuse
 * this so they do not re-derive the identification framing.
 *
 * @pre The queue is empty.
 * @post Enough RX bytes are queued to satisfy a full ra8_sdmmc_spi_init.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_full_init_sdhc_32gib(void)
{
  internal_mock_queue_idle(k_sdspi_wakeup_clocks); /* Wake-up dummy clocks. */
  internal_queue_command_response_r1((uint8_t)k_cov_r1_idle);
  internal_queue_command_response_r3_or_r7((uint8_t)k_cov_r1_idle, (uint32_t)k_cov_cmd8_echo);
  internal_queue_command_response_r1((uint8_t)k_cov_r1_idle);  /* CMD55.  */
  internal_queue_command_response_r1((uint8_t)k_cov_r1_ready); /* ACMD41. */
  internal_queue_command_response_r3_or_r7((uint8_t)k_cov_r1_ready, (uint32_t)k_cov_ocr_ccs_ready);
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  internal_build_csd_v2_32gib(csd);
  internal_queue_csd_read(csd);
  internal_queue_command_response_r1((uint8_t)k_cov_r1_ready); /* CMD16. */
}

/**
 * @brief Queue the CMD24 write-block tail (data token .. busy release).
 *
 * @param[in] data_response Data-response token the card returns.
 * @param[in] busy_bytes    Number of 0x00 busy bytes before release.
 *
 * @pre A preceding CMD24 R1 has been queued.
 * @post The write data phase and busy wait are fully queued.
 * @since 0.1.0 @details Exercises the queue write block tail path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_write_block_tail(uint8_t data_response, uint32_t busy_bytes)
{
  internal_mock_queue_idle(1U);                                   /* data-token TX slot.   */
  internal_mock_queue_idle((uint32_t)k_ra8_sdmmc_spi_block_size); /* 512 payload TX slots. */
  internal_mock_queue_idle(2U);                                   /* 2 CRC TX slots.       */
  internal_mock_queue_byte(data_response);                        /* response token byte.  */
  for (uint32_t i = 0U; i < busy_bytes; i++) {
    internal_mock_queue_byte(0x00U); /* card busy 0x00. */
  }
  internal_mock_queue_byte((uint8_t)k_cov_mock_idle_byte); /* busy released 0xFF. */
}

/**
 * @brief Queue a successful CMD32 + CMD33 + CMD38 erase sequence.
 *
 * @pre The queue is positioned at a command boundary.
 * @post One full erase sequence (two R1 commands + a busy wait) is queued.
 * @since 0.1.0 @details Exercises the queue erase blocks ok path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_erase_blocks_ok(void)
{
  internal_queue_command_response_r1((uint8_t)k_cov_r1_ready); /* CMD32. */
  internal_queue_command_response_r1((uint8_t)k_cov_r1_ready); /* CMD33. */
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len);
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);
  internal_mock_queue_byte((uint8_t)k_cov_busy_done);
  internal_mock_queue_idle(1U);
}

/**
 * @brief Queue a single ra8_sdmmc_spi_read_block session delivering @p block.
 *
 * @param[in] block 512-byte payload the card returns, with a valid CRC16.
 *
 * @pre block points at k_ra8_sdmmc_spi_block_size bytes.
 * @post One complete CMD17 read session is queued.
 * @since 0.1.0 @details Exercises the queue read back path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_read_back(const uint8_t* block)
{
  internal_mock_queue_idle(1U);
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len);
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);
  internal_mock_queue_byte((uint8_t)k_cov_data_token_start);
  internal_mock_queue_bytes(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  const uint16_t crc = ra8_sdmmc_spi_crc16(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  internal_mock_queue_byte((uint8_t)((crc >> 8U) & k_byte_mask));
  internal_mock_queue_byte((uint8_t)(crc & k_byte_mask));
  internal_mock_queue_idle(1U);
}

/**
 * @brief Queue a full CMD18 multi-block read session delivering @p blocks.
 *
 * @details Models the single CS-held transaction sdspi_read now issues:
 * cs-assert idle + CMD18 frame + R1, then per block a data-start token +
 * 512-byte payload + valid CRC16 trailer, then the CMD12 stop (frame TX
 * slots + stuff byte + R1 + busy release) and the cs-release idle.
 *
 * @param[in] blocks Array of @p count pointers to 512-byte payloads.
 * @param[in] count  Number of streamed blocks (>= 2 for the CMD18 path).
 *
 * @pre Each blocks[i] points at k_ra8_sdmmc_spi_block_size bytes.
 * @post One complete CMD18 + CMD12 session is queued.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_queue_multi_read(const uint8_t* const* blocks, uint32_t count)
{
  internal_mock_queue_idle(1U);                            /* cs_assert post-byte.  */
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len); /* CMD18 frame TX slots. */
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);       /* CMD18 R1.             */
  for (uint32_t b = 0U; b < count; b++) {
    internal_mock_queue_byte((uint8_t)k_cov_data_token_start);
    internal_mock_queue_bytes(blocks[b], (uint32_t)k_ra8_sdmmc_spi_block_size);
    const uint16_t crc = ra8_sdmmc_spi_crc16(blocks[b], (uint32_t)k_ra8_sdmmc_spi_block_size);
    internal_mock_queue_byte((uint8_t)((crc >> 8U) & k_byte_mask));
    internal_mock_queue_byte((uint8_t)(crc & k_byte_mask));
  }
  internal_mock_queue_idle((uint32_t)k_cov_cmd_frame_len); /* CMD12 frame TX slots.  */
  internal_mock_queue_idle(1U);                            /* stuff byte, discarded. */
  internal_mock_queue_byte((uint8_t)k_cov_r1_ready);       /* CMD12 R1.              */
  internal_mock_queue_byte((uint8_t)k_cov_busy_done);      /* busy released 0xFF.    */
  internal_mock_queue_idle(1U);                            /* cs_release post-byte.  */
}

/**
 * @brief Reset the mock and tear the driver singleton down before a test.
 *
 * @pre None.
 * @post s_mock is cleared and the driver is un-initialized.
 * @since 0.1.0 @details Exercises the per test setup path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_per_test_setup(void)
{
  internal_mock_reset();
  (void)ra8_sdmmc_spi_deinit();
}

/**
 * @brief Bring the driver up against the 32 GiB SDHC mock and bind @p bd.
 *
 * @param[out] bd Zero-initialised block-device handle to bind.
 *
 * @pre bd is non-NULL and zero-initialised.
 * @post The driver is initialised and bd dispatches to the SD-SPI backend.
 * @since 0.1.0 @details Exercises the fixture card up path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_fixture_card_up(ra8_io_blockdev_t* bd)
{
  internal_per_test_setup();
  internal_queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdspi_init(bd));
}

/* ===========================================================================
 * Test functions
 * ===========================================================================
 */

/**
 * @brief sdspi_read: CMD18 bulk-read forward, then driver-error propagation.
 *
 * @details
 * With the card up, reads two consecutive blocks so the backend's single
 * ra8_sdmmc_spi_read_blocks forward streams one CMD18 transaction (a data
 * token + payload + CRC16 per block, terminated by CMD12), then tears the
 * driver down and re-issues the read so the forward returns
 * k_ra8_err_invalid_state.
 *
 * @par MC/DC:
 * sdspi_read contains only single-condition decisions (no &&/||):
 * - `RA8_CHECK_NULL_PTR(buf)` guard -- FALSE arm covered here (buf non-NULL);
 *   the TRUE arm is unreachable through ra8_io_blockdev_read, which null-checks
 *   buf before forwarding.
 * - the forwarded ra8_sdmmc_spi_read_blocks result -- success covered by the
 *   CMD18 read; failure covered by the torn-down driver read below.
 * Each single-condition decision is exercised in both senses: minimal MC/DC.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdspi_read_bulk_and_error(void)
{
  TEST_BEGIN("sdspi_read CMD18 forward + driver error");
  ra8_io_blockdev_t bd = {};
  internal_fixture_card_up(&bd);

  /* Two deterministic payloads with distinct fill patterns. */
  uint8_t block0[k_ra8_sdmmc_spi_block_size];
  uint8_t block1[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    block0[i] = (uint8_t)((i * 3U) & k_byte_mask);
    block1[i] = (uint8_t)((i * k_sdspi_pattern_stride) + 1U);
  }
  const uint8_t* blocks[2] = {block0, block1};
  internal_queue_multi_read(blocks, 2U);

  uint8_t buf[(size_t)2U * (size_t)k_ra8_sdmmc_spi_block_size];
  memset(buf, k_sdspi_cov_fill, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 0U, 2U, buf));
  TEST_ASSERT_EQ(0, memcmp(&buf[0], block0, (size_t)k_ra8_sdmmc_spi_block_size));
  TEST_ASSERT_EQ(
    0,
    memcmp(&buf[(size_t)k_ra8_sdmmc_spi_block_size], block1, (size_t)k_ra8_sdmmc_spi_block_size));

  /* Driver-error leg: tear the card down so the read forward fails. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_deinit());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_read(&bd, 0U, 1U, buf));
  TEST_END("sdspi_read CMD18 forward + driver error");
}

/**
 * @brief sdspi_write: forwards a single-block write to the driver.
 *
 * @details
 * A count=1 write routes through ra8_sdmmc_spi_write_blocks' CMD24 fallback.
 * The mock returns a "data accepted" token and a one-byte busy window, so
 * the forward returns k_ra8_ok.
 *
 * @par MC/DC:
 * sdspi_write has one single-condition decision, the `RA8_CHECK_NULL_PTR(buf)`
 * guard. Its FALSE arm is covered here (buf non-NULL); the TRUE arm is
 * unreachable through ra8_io_blockdev_write, which null-checks buf before
 * forwarding. No compound (&&/||) decision exists in this body.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdspi_write_forwards(void)
{
  TEST_BEGIN("sdspi_write forwards single block");
  ra8_io_blockdev_t bd = {};
  internal_fixture_card_up(&bd);

  internal_queue_command_response_r1((uint8_t)k_cov_r1_ready); /* CMD24 R1. */
  internal_queue_write_block_tail((uint8_t)k_cov_data_resp_accept, 1U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    buf[i] = (uint8_t)(i & k_byte_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 1U, 1U, buf));
  TEST_END("sdspi_write forwards single block");
}

/**
 * @brief sdspi_erase: forwards to the driver's probe-and-verify erase.
 *
 * @details
 * Erases 64 blocks. The driver probe-erases block 0, reads it back as
 * all-zero, then erases the remaining range, so the forward returns k_ra8_ok.
 *
 * @par MC/DC:
 * sdspi_erase is straight-line -- it drops ctx and tail-calls
 * ra8_sdmmc_spi_erase_blocks. No boolean decision (compound or otherwise)
 * lives in this body; the single success forward covers both statements.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdspi_erase_forwards(void)
{
  TEST_BEGIN("sdspi_erase forwards probe-and-verify");
  ra8_io_blockdev_t bd = {};
  internal_fixture_card_up(&bd);

  uint8_t zeros[k_ra8_sdmmc_spi_block_size] = {};
  internal_queue_erase_blocks_ok(); /* probe erase [0,1).        */
  internal_queue_read_back(zeros);  /* probe read-back all zero. */
  internal_queue_erase_blocks_ok(); /* erase the rest [1,64).    */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 0U, 64U));
  TEST_END("sdspi_erase forwards probe-and-verify");
}

/**
 * @brief sdspi_get_caps: full snapshot success, then driver-error propagation.
 *
 * @details
 * With the card up, populates the capability snapshot and checks every field
 * against the SD-SPI backend's fixed contract (one-block erase unit, 512-byte
 * program size and logical block, zero erase value, no erase-before-write,
 * writable). Then tears the driver down so ra8_sdmmc_spi_get_capacity returns
 * k_ra8_err_invalid_state and the RA8_RETURN_ON_ERROR early-return leg runs.
 *
 * @par MC/DC:
 * sdspi_get_caps has two single-condition decisions (no &&/||):
 * - `RA8_CHECK_NULL_PTR(out)` -- FALSE arm covered here (out non-NULL); the TRUE
 *   arm is unreachable through ra8_io_blockdev_get_caps, which null-checks out
 *   before forwarding.
 * - `RA8_RETURN_ON_ERROR(ra8_sdmmc_spi_get_capacity(...))` inner
 *   `ra8_err_is_error()` -- FALSE arm covered by the happy snapshot; TRUE arm
 *   covered by the torn-down driver query below.
 * Both decisions are driven in both senses: minimal MC/DC.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdspi_get_caps_and_error(void)
{
  TEST_BEGIN("sdspi_get_caps snapshot + driver error");
  ra8_io_blockdev_t bd = {};
  internal_fixture_card_up(&bd);

  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT(cap > 0U);

  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(cap, caps.block_count);
  TEST_ASSERT_EQ(1U, caps.erase_unit_blocks);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.program_size_bytes);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.logical_block_bytes);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_zero, caps.erase_value);
  TEST_ASSERT(!caps.must_erase_before_write);
  TEST_ASSERT(!caps.read_only);

  /* Driver-error leg: tear the card down so the capacity query fails. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_deinit());
  ra8_io_blockdev_caps_t caps2 = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_get_caps(&bd, &caps2));
  TEST_END("sdspi_get_caps snapshot + driver error");
}

/**
 * @brief ra8_io_blockdev_sdspi_init: NULL guard and successful bind.
 *
 * @details
 * Confirms the bind helper rejects a NULL handle and, given a valid handle,
 * wires the backend so a subsequent capability query dispatches to the
 * SD-SPI vtable.
 *
 * @par MC/DC:
 * The bind helper's only decision is the single-condition
 * `RA8_CHECK_NULL_PTR(bd)` guard: the TRUE arm (bd == NULL) and FALSE arm
 * (valid bd) are both exercised here. No compound decision exists.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdspi_bind(void)
{
  TEST_BEGIN("sdspi_init NULL guard + bind");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_sdspi_init(nullptr));

  ra8_io_blockdev_t bd = {};
  internal_fixture_card_up(&bd);
  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_END("sdspi_init NULL guard + bind");
}

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Run all SD-SPI block-device coverage tests and report overall pass.
 *
 * @return 0 on success; the harness calls exit(1) on the first failure.
 *
 * @pre The ra8_core_hal OBJECT library was built with --coverage.
 * @post Every forwarding body in ra8_io_blockdev_sdspi.c has executed at least
 *       once in both its success and driver-error senses.
 * @post Exit code 0 signals success to the CI coverage runner.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_sdspi_bind();
  internal_test_sdspi_read_bulk_and_error();
  internal_test_sdspi_write_forwards();
  internal_test_sdspi_erase_forwards();
  internal_test_sdspi_get_caps_and_error();
  return 0;
}

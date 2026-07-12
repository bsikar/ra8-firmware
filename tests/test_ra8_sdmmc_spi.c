/**
 * @file test_ra8_sdmmc_spi.c
 * @brief Host-side unit tests for the SPI-mode SD card driver.
 *
 * @details
 * Drives the driver through a mock transport that records every byte
 * written by the driver and replies with caller-programmed responses.
 * Covers:
 *
 *   - CRC7 polynomial reduction against published vectors.
 *   - CRC16-CCITT against published vectors.
 *   - CMD0 framing (idle response).
 *   - CMD8 voltage-check classification (v1.x vs v2.x cards).
 *   - ACMD41 readiness polling loop.
 *   - CSD v2 capacity decode -> 32 GiB block count.
 *   - CMD17 single-block read with the data token + CRC16 trailer.
 *   - CMD18 multi-block read: one command, streamed blocks, CMD12 stop.
 *   - CMD24 single-block write with the data-accepted token + busy wait.
 *   - ra8_fs backend adapter wiring (read/write forwarding, capacity).
 *
 * Each test re-initializes the mock and the driver -- there is no
 * shared mutable state between cases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_sci_regs.h"
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
  k_mock_xfer_byte_idle = 0xFFU,
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

static void mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
}

static void mock_queue_byte(uint8_t b)
{
  if (s_mock.rx_len < (uint32_t)k_mock_buf_bytes) {
    s_mock.rx_queue[s_mock.rx_len] = b;
    s_mock.rx_len++;
  }
}

static void mock_queue_bytes(const uint8_t* data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    mock_queue_byte(data[i]);
  }
}

/**
 * @brief Queue ``count`` copies of the idle byte 0xFF.
 */
static void mock_queue_idle(uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    mock_queue_byte((uint8_t)k_mock_xfer_byte_idle);
  }
}

static ra8_err_t mock_set_clock(void* ctx, uint32_t hz)
{
  (void)ctx;
  s_mock.clock_hz = hz;
  return k_ra8_ok;
}

static ra8_err_t mock_cs(void* ctx, bool asserted)
{
  (void)ctx;
  s_mock.cs_asserted = asserted;
  if (asserted) {
    s_mock.cs_assert_count++;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
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
static ra8_err_t mock_xfer_no_bulk(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
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
static ra8_err_t mock_set_clock_fail_second(void* ctx, uint32_t hz)
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
static void mock_arm_xfer_fail(uint32_t nth)
{
  s_mock.xfer_calls   = 0U;
  s_mock.xfer_fail_at = nth;
}

/* ===========================================================================
 * SD response builder helpers
 * ===========================================================================
 */

typedef enum : uint8_t {
  k_test_r1_idle              = 0x01U,
  k_test_r1_ready             = 0x00U,
  k_test_r1_illegal_cmd       = 0x05U,
  k_test_data_token_start     = 0xFEU,
  k_test_data_response_accept = 0x05U,
  k_test_busy_done            = 0xFFU, /**< Not-busy token after CMD38 erase. */
} test_resp_const_t;

typedef enum : uint32_t {
  k_test_cmd8_echo       = 0x000001AAUL,
  k_test_ocr_ccs_ready   = 0xC0FF8000UL, /**< busy=1 | CCS=1 | voltage window */
  k_test_cmd_frame_bytes = 6U,
  /* Allow a generous post-frame N_CR gap so the driver's R1 polling
   * never times out -- the mock queues idle 0xFF bytes for every clock
   * after the command frame until the response token is queued. */
  k_test_r1_padding_bytes = 1U,
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
static void queue_command_response_r1(uint8_t r1)
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

static void queue_command_response_r3_or_r7(uint8_t r1, uint32_t tail_word)
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
static void build_csd_v2_32gib(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0] = 0x40U; /* CSD_STRUCTURE = 1 */
  out[7] = 0x00U;
  out[8] = 0xFFU; /* low 16 bits of C_SIZE = 0xFF FF */
  out[9] = 0xFFU;
}

static void queue_csd_read(const uint8_t* csd)
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
static void queue_full_init_sdhc_32gib(void)
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

static void per_test_setup(void)
{
  mock_reset();
  (void)ra8_sdmmc_spi_deinit();
}

/* ===========================================================================
 * CRC primitives
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * CRC reduction has only single-condition decisions; this case proves
 * the spec-published vectors round-trip.
 */
static void test_crc7_published_vectors(void)
{
  TEST_BEGIN("crc7 published vectors");
  /* CMD0 with arg = 0 -> wire byte 0x95 -> CRC7 = 0x4A. */
  const uint8_t cmd0_frame[5] = {0x40U, 0x00U, 0x00U, 0x00U, 0x00U};
  const uint8_t expected_cmd0 = 0x4AU;
  TEST_ASSERT_EQ(expected_cmd0, ra8_sdmmc_spi_crc7(cmd0_frame, 5U));
  /* CMD8 arg = 0x000001AA -> wire byte 0x87 -> CRC7 = 0x43. */
  const uint8_t cmd8_frame[5] = {0x48U, 0x00U, 0x00U, 0x01U, 0xAAU};
  const uint8_t expected_cmd8 = 0x43U;
  TEST_ASSERT_EQ(expected_cmd8, ra8_sdmmc_spi_crc7(cmd8_frame, 5U));
  /* nullptr guard: CRC7 of a nullptr pointer == 0. */
  TEST_ASSERT_EQ(0, ra8_sdmmc_spi_crc7(nullptr, 4U));
  TEST_END("crc7 published vectors");
}

/**
 * @par MC/DC:
 * CRC16 inner-loop XOR has only the single ``top-bit-set`` condition.
 * Vectors prove zero-input -> zero and a sample 512-byte block.
 */
static void test_crc16_published_vectors(void)
{
  TEST_BEGIN("crc16 known vectors");
  /* Zero-length input -> seed 0x0000. */
  TEST_ASSERT_EQ(0, ra8_sdmmc_spi_crc16(nullptr, 0U));
  /* All-zero 512-byte block -> CRC16-CCITT of all-zero is 0x0000. */
  uint8_t zeros[512];
  memset(zeros, 0, sizeof(zeros));
  TEST_ASSERT_EQ(0, ra8_sdmmc_spi_crc16(zeros, sizeof(zeros)));
  /* "123456789" classic vector for CRC16-CCITT seed 0x0000 == 0x31C3. */
  const uint8_t msg[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQ(0x31C3, ra8_sdmmc_spi_crc16(msg, sizeof(msg)));
  TEST_END("crc16 known vectors");
}

/* ===========================================================================
 * Init sequence
 * ===========================================================================
 */

static void test_init_null_transport_rejected(void)
{
  TEST_BEGIN("init nullptr transport");
  per_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_init(nullptr));
  TEST_END("init nullptr transport");
}

/**
 * @par MC/DC:
 * Decision: ``(set_clock == nullptr) || (cs == nullptr) || (xfer == nullptr)``
 * (3 conditions). Vectors:
 *   - all non-nullptr                    -> false (control)
 *   - set_clock nullptr, others non-nullptr -> true  (set_clock independent)
 *   - cs nullptr, others non-nullptr        -> true  (cs independent)
 *   - xfer nullptr, others non-nullptr      -> true  (xfer independent)
 * 4 vectors = N+1 with N=3, minimal MC/DC.
 */
static void test_init_validates_callbacks(void)
{
  TEST_BEGIN("init validates callbacks");
  per_test_setup();
  ra8_sdmmc_spi_transport_t t = s_mock_transport;
  /* control: all set -> sequence will run; we don't queue responses
   * so it must time out. Driver path goes past validate_transport. */
  TEST_ASSERT(ra8_sdmmc_spi_init(&t) != k_ra8_err_invalid_arg);
  per_test_setup();
  t           = s_mock_transport;
  t.set_clock = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  per_test_setup();
  t    = s_mock_transport;
  t.cs = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  per_test_setup();
  t      = s_mock_transport;
  t.xfer = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  TEST_END("init validates callbacks");
}

/**
 * @par MC/DC:
 * End-to-end init flow exercises every compound decision in
 * ``libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi.c`` in its TRUE-control
 * configuration:
 *
 *   - `validate_transport` OR-chain (vectors documented on
 *     ::test_mcdc_validate_transport_or_chain).
 *   - `internal_build_frame` AND-decision ``cmd == CMD8 && arg == 0x1AA``
 *     (libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi.c@internal_build_frame).
 *     The init path issues
 *     CMD8 with the canonical argument (both operands true; on-wire
 *     byte 5 == published constant 0x87) AND issues CMD0 / CMD55 /
 *     CMD17 etc. that vary the first operand false (falling into the
 *     generic CRC7 branch). Pair (V1=CMD8/0x1AA, V2=CMD0/0) flips the
 *     first operand independently; pair (V1, V3=CMD17/lba) flips both.
 *     The V4 vector (CMD8 with non-0x1AA arg) is unreachable from the
 *     public API because the driver never issues CMD8 with anything
 *     else -- coupled-operand exception per DO-178C 6.4.4.3
 *     "where coupled, document and justify".
 *   - `fs_get_capacity` NULL-OR (vectors on
 *     ::test_mcdc_fs_get_capacity_null_or).
 */
static void test_init_full_sdhc_path(void)
{
  TEST_BEGIN("init full SDHC 32GiB path");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  const ra8_err_t err = ra8_sdmmc_spi_init(&s_mock_transport);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  /* 32 GiB SDHC card: ((0xFFFF + 1) * 1024) blocks = 0x4000000 = 64 Mi blocks. */
  TEST_ASSERT_EQ(((0xFFFFUL + 1UL) * 1024UL), cap);

  ra8_sdmmc_spi_card_type_t type = k_ra8_sdmmc_spi_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_card_type(&type));
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_type_sdhc, type);

  /* Final clock should be at the data-speed target (25 MHz). */
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_clock_data_hz, s_mock.clock_hz);

  TEST_END("init full SDHC 32GiB path");
}

/**
 * @par MC/DC:
 * Decision (internal_send_cmd8): ``(r1 & illegal_command) != 0`` (1
 * condition). Two vectors are enough but to also cover the echo-mismatch
 * path we test all three branches:
 *   - illegal cmd set    -> v1 path
 *   - illegal cmd clear, echo matches -> v2 path
 *   - illegal cmd clear, echo mismatch -> protocol error
 */
static void test_init_cmd8_classifies_v1_card(void)
{
  TEST_BEGIN("CMD8 illegal -> v1 classification");
  per_test_setup();
  /* Wake-up clocks. */
  mock_queue_idle(10U);
  /* CMD0 -> R1 idle. */
  queue_command_response_r1((uint8_t)k_test_r1_idle);
  /* CMD8 -> R1 with illegal-cmd bit set (v1 card). No tail bytes drained. */
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd);
  /* ACMD41 (no HCS) -> CMD55 idle + ACMD41 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_idle);
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* For v1 cards we skip CMD58. CMD9 directly. CSD v1 for ~1 GiB card.
   * Construct a minimal v1 CSD: READ_BL_LEN=9 (512 B), C_SIZE=1023,
   * C_SIZE_MULT=7 -> ~1 GiB. */
  uint8_t csd_v1[k_ra8_sdmmc_spi_csd_response_len];
  memset(csd_v1, 0, sizeof(csd_v1));
  csd_v1[0]  = 0x00U; /* CSD_STRUCTURE = 0                                 */
  csd_v1[5]  = 0x09U; /* READ_BL_LEN = 9 (low nibble)                      */
  csd_v1[6]  = 0x03U; /* C_SIZE bits 11:10 = 0b11                          */
  csd_v1[7]  = 0xFFU; /* C_SIZE bits 9:2 = 0xFF                            */
  csd_v1[8]  = 0xC0U; /* C_SIZE bits 1:0 = 0b11 (-> C_SIZE = 0x3FF = 1023) */
  csd_v1[9]  = 0x03U; /* C_SIZE_MULT bits 2:1 = 0b11                       */
  csd_v1[10] = 0x80U; /* C_SIZE_MULT bit 0 = 1 (-> C_SIZE_MULT = 7)        */
  queue_csd_read(csd_v1);
  /* CMD16 -> R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  ra8_sdmmc_spi_card_type_t type = k_ra8_sdmmc_spi_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_card_type(&type));
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_type_sdv1, type);
  TEST_END("CMD8 illegal -> v1 classification");
}

/* ===========================================================================
 * Block I/O
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Decision: ``lba >= capacity`` is single-condition; null-pointer guard
 * is also single-condition. Two vectors prove each.
 */
static void test_read_block_rejects_null(void)
{
  TEST_BEGIN("read_block nullptr buf");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_read_block(0U, nullptr));
  TEST_END("read_block nullptr buf");
}

/**
 * @par MC/DC:
 * Decision: ``lba >= capacity`` (1 condition). V1: lba=0 -> false (covered by happy-path test). V2: lba=capacity -> true. Minimal 2-vector pair flips outcome.
 */
static void test_read_block_rejects_oor(void)
{
  TEST_BEGIN("read_block out-of-range LBA");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  uint8_t  buf[k_ra8_sdmmc_spi_block_size];
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_read_block(cap, buf));
  TEST_END("read_block out-of-range LBA");
}

/**
 * @par MC/DC:
 * Happy path with all decisions in their FALSE-control configuration; companion to *_detects_crc_mismatch which flips the CRC-compare decision.
 */
static void test_read_block_happy_path(void)
{
  TEST_BEGIN("read_block happy path with CRC16");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  /* Build a deterministic 512-byte payload. */
  uint8_t expected[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    expected[i] = (uint8_t)((i * 7U) & 0xFFU);
  }
  const uint16_t crc16 = ra8_sdmmc_spi_crc16(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);

  /* CMD17 -> R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* Data token + payload + CRC16 trailer. */
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);
  mock_queue_byte((uint8_t)((crc16 >> 8U) & 0xFFU));
  mock_queue_byte((uint8_t)(crc16 & 0xFFU));

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  memset(buf, 0xA5U, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_read_block(0U, buf));
  TEST_ASSERT_EQ(0, memcmp(buf, expected, sizeof(buf)));
  TEST_END("read_block happy path with CRC16");
}

/**
 * @par MC/DC:
 * Decision: ``expected != actual`` (1 condition). V1 happy path (false) handled in *_happy_path. V2 (true) handled here -- mock returns wrong CRC trailer.
 */
static void test_read_block_detects_crc_mismatch(void)
{
  TEST_BEGIN("read_block detects bad CRC16");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  uint8_t expected[k_ra8_sdmmc_spi_block_size];
  memset(expected, 0xC5U, sizeof(expected));

  queue_command_response_r1((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);
  /* Wrong CRC bytes. */
  mock_queue_byte(0xDEU);
  mock_queue_byte(0xADU);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_sdmmc_spi_read_block(0U, buf));
  TEST_END("read_block detects bad CRC16");
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
static void queue_write_block_tail(uint8_t data_response, uint32_t busy_bytes)
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
 * @par MC/DC:
 * Happy path -- companion to *_detects_write_error; together they flip the data-response token check decision ``(response & mask) != accepted``.
 */
static void test_write_block_happy_path(void)
{
  TEST_BEGIN("write_block happy path");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  /* CMD24 -> R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* Data-response = accepted, 1 busy byte then idle. */
  queue_write_block_tail((uint8_t)k_test_data_response_accept, 1U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    buf[i] = (uint8_t)(i & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_block(1U, buf));
  TEST_END("write_block happy path");
}

/**
 * @par MC/DC:
 * Decision: ``(response & mask) != accepted`` (1 condition). V1 accepted (false) covered by happy-path test. V2 not-accepted (true) covered here.
 */
static void test_write_block_detects_write_error(void)
{
  TEST_BEGIN("write_block detects write-error token");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* Data-response = write error (0x0D). Card releases busy promptly. */
  queue_write_block_tail(0x0DU, 0U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_write_block(2U, buf));
  TEST_END("write_block detects write-error token");
}

/* ===========================================================================
 * ra8_fs backend adapter
 * ===========================================================================
 */

static void test_bind_fs_backend_populates_struct(void)
{
  TEST_BEGIN("bind_fs_backend populates struct");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_bind_fs_backend(&backend));
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)backend.read_block);
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)backend.write_block);
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)backend.get_capacity);
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)backend.erase_blocks);

  uint32_t blocks = 0U;
  uint32_t bsize  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, backend.get_capacity(backend.ctx, &blocks, &bsize));
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_block_size, bsize);
  TEST_ASSERT(blocks > 0U);
  TEST_END("bind_fs_backend populates struct");
}

/**
 * @par MC/DC:
 * Decision: ``!s_state.initialized`` (1 condition). V1 init succeeded (false) covered by *_populates_struct. V2 not initialized (true) covered here.
 */
static void test_bind_fs_backend_uninitialized_rejected(void)
{
  TEST_BEGIN("bind_fs_backend before init");
  per_test_setup();
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_bind_fs_backend(&backend));
  TEST_END("bind_fs_backend before init");
}

/* ===========================================================================
 * Dedicated MC/DC vector tests (citation gate)
 * ===========================================================================
 */

/**
 * @test test_mcdc_validate_transport_or_chain
 *
 * @par MC/DC:
 * Decision: ``if ((transport->set_clock == nullptr) ||
 *                 (transport->cs == nullptr) ||
 *                 (transport->xfer == nullptr))``
 * (3 conditions; libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi.c@internal_validate_transport)
 * inside ``internal_validate_transport``.
 *
 * Per DO-178C 6.4.4.3 representative-subset, N+1 = 4 vectors. Each
 * sub-test holds two conditions fixed and flips the third so the
 * outcome moves independently:
 *
 *   - V1: all three non-NULL                     -> false (control).
 *   - V2: set_clock NULL, others non-NULL        -> true  (set_clock).
 *   - V3: cs NULL, others non-NULL               -> true  (cs).
 *   - V4: xfer NULL, others non-NULL             -> true  (xfer).
 *
 * Pairs (V1,V2), (V1,V3), (V1,V4) each prove one operand independently
 * affects the OR outcome.
 */
static void test_mcdc_validate_transport_or_chain(void)
{
  TEST_BEGIN("MC/DC: validate_transport OR-chain");
  per_test_setup();
  ra8_sdmmc_spi_transport_t t = s_mock_transport;
  /* V1: all non-NULL -- driver will progress past the gate (no responses queued
   * so it fails later with hw_timeout, but NOT with invalid_arg). */
  TEST_ASSERT(ra8_sdmmc_spi_init(&t) != k_ra8_err_invalid_arg);
  per_test_setup();
  /* V2: set_clock NULL -- gate must trip on first operand. */
  t           = s_mock_transport;
  t.set_clock = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  per_test_setup();
  /* V3: cs NULL -- gate must trip on second operand. */
  t    = s_mock_transport;
  t.cs = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  per_test_setup();
  /* V4: xfer NULL -- gate must trip on third operand. */
  t      = s_mock_transport;
  t.xfer = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_init(&t));
  TEST_END("MC/DC: validate_transport OR-chain");
}

/* test_mcdc_build_frame_cmd8_special_case was removed -- the AND-decision
 * `(cmd == CMD8) && (arg == 0x1AA)` inside `internal_build_frame` is
 * fully exercised by `test_init_full_sdhc_path` below (V1: both
 * operands true; init also issues CMD0 / CMD55 / CMD17 / etc. which
 * vary the first operand false). The removed test re-queued the same
 * full-init mock responses as `test_init_full_sdhc_path` and was
 * functionally a duplicate; the MC/DC justification it carried has
 * been folded into the `@par MC/DC:` block on the test that remains. */

/**
 * @test test_mcdc_fs_get_capacity_null_or
 *
 * @par MC/DC:
 * Decision: ``if ((block_count == nullptr) || (block_size == nullptr))``
 * (2 conditions; libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi.c@internal_fs_get_capacity)
 * inside ``internal_fs_get_capacity``.
 *
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors:
 *   - V1: both non-NULL                    -> false (control).
 *   - V2: block_count NULL, block_size non -> true  (block_count).
 *   - V3: block_count non, block_size NULL -> true  (block_size).
 *
 * Pairs (V1,V2) and (V1,V3) prove independence.
 */
static void test_mcdc_fs_get_capacity_null_or(void)
{
  TEST_BEGIN("MC/DC: fs_get_capacity null OR");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_bind_fs_backend(&backend));
  uint32_t bc = 0U;
  uint32_t bs = 0U;
  /* V1. */
  TEST_ASSERT_EQ(k_ra8_ok, backend.get_capacity(backend.ctx, &bc, &bs));
  /* V2. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, backend.get_capacity(backend.ctx, nullptr, &bs));
  /* V3. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, backend.get_capacity(backend.ctx, &bc, nullptr));
  TEST_END("MC/DC: fs_get_capacity null OR");
}

/**
 * @brief Queue a successful CMD32 + CMD33 + CMD38 erase sequence.
 *
 * @details CMD32 (ERASE_WR_BLK_START) and CMD33 (ERASE_WR_BLK_END) are plain
 * R1 commands; CMD38 (ERASE) is an R1 command followed by a busy wait that the
 * mock clears immediately with a single not-busy (0xFF) byte.
 */
static void queue_erase_blocks_ok(void)
{
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD32 */
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD33 */
  /* CMD38: cs_assert idle + frame echo + R1 + not-busy token + cs_release idle. */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_busy_done);
  mock_queue_idle(1U);
}

/**
 * @brief Queue an ra8_sdmmc_spi_read_block response delivering @p block.
 *
 * @details Models the CMD17 read the erase path uses to verify the post-erase
 * value: R1 + data-start token + 512 payload + a correct CRC16 trailer.
 */
static void queue_read_back(const uint8_t* block)
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
 * @test test_erase_blocks_verifies_zero
 *
 * @par MC/DC:
 * Decision: the read-back verify loop ``if (blk[i] != 0U) return not_supported;``
 * decides success. The SD post-erase value is card-dependent, so a ::k_ra8_ok
 * return must be *measured*, not assumed. The probe erases one block, reads it
 * back, and only erases the rest of the range when that block is zero.
 *   - V1: probe block reads back all-zero -> loop never trips -> erase the rest -> k_ra8_ok.
 *   - V2: probe block reads back non-zero  -> loop trips        -> k_ra8_err_not_supported.
 *
 * Also drives the range guard ``(lba >= capacity) || (count > capacity - lba)``
 * (2 conditions) that precedes the erase:
 *   - RV1: lba < cap, count <= cap - lba -> false (the V1 erase of [0,64) below).
 *   - RV2: lba == cap, count == 1        -> true  (first operand; short-circuits).
 *   - RV3: lba = cap - 1, count == 2     -> true  (second operand: count spills
 *                                          past the last block).
 * Pairs (RV1,RV2) and (RV1,RV3) prove each operand independently moves the
 * outcome. The case also covers the uninitialized and count == 0 guards.
 */
static void test_erase_blocks_verifies_zero(void)
{
  TEST_BEGIN("erase_blocks probes + verifies a zero read-back");

  /* Uninitialized driver -> invalid_state (guard before the erase). */
  per_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  /* V1: probe block reads back all-zero -> erase the rest -> k_ra8_ok. The probe
   * erases [0,1) then, after the zero read-back, the remaining [1,64) -- two
   * CMD32/33/38 sequences bracketing the read-back. */
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_erase_blocks(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_erase_blocks(cap, 1U));
  /* RV3: lba in range but count spills past the last block -> out_of_range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_erase_blocks(cap - 1U, 2U));
  queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t zeros[k_ra8_sdmmc_spi_block_size] = {};
  queue_read_back(zeros);  /* probe read-back: all zero */
  queue_erase_blocks_ok(); /* erase the rest [1,64)     */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  /* V2: probe block reads back 0xFF (card erases to ones) -> not_supported, and
   * the rest of the range is NOT erased (no wasted full-region erase). */
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t ones[k_ra8_sdmmc_spi_block_size];
  memset(ones, 0xFFU, sizeof(ones));
  queue_read_back(ones); /* probe read-back: non-zero -> stop */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  TEST_END("erase_blocks probes + verifies a zero read-back");
}

/* ===========================================================================
 * SCI Simple-SPI transport factory (ra8_sdmmc_spi_io.c)
 * ===========================================================================
 */

/**
 * @brief Reset the simulated MMIO window, MSTP model, and pin validator.
 *
 * @details The factory routes pins and brings up an SCI channel through the
 * real GPIO / SCI HAL, all of which operate on the ``ra8_sim_mmap``-backed
 * register window under ``RA8_SIMULATOR_MODE``. Clearing the pin validator
 * lets each case re-claim the same pins without a "pin already owned" error.
 */
static void sci_factory_prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  ra8_pin_validator_reset();
  (void)ra8_sdmmc_spi_deinit();
}

/**
 * @brief Four valid, distinct Pmod SPI pins on port 1.
 */
static const ra8_sdmmc_spi_sci_pins_t s_factory_pins = {
  .sck  = RA8_PIN(1U, 0U),
  .cipo = RA8_PIN(1U, 1U),
  .copi = RA8_PIN(1U, 2U),
  .cs   = RA8_PIN(1U, 3U),
};

/**
 * @par MC/DC:
 * No compound boolean decision under test here -- the factory body and its
 * three transport shims are straight-line code. The case drives the
 * success leg of ``ra8_sdmmc_spi_transport_sci`` (pin routing + SCI bring-up)
 * and then exercises each returned shim: ``set_clock`` (SCI baud retune),
 * ``cs`` for both asserted and released, and ``xfer`` with the SCI status
 * flags pre-seeded so the TDRE/RDRF poll resolves on its first read (the
 * deterministic pattern from test_ra8_sci_spi.c -- no SIGALRM). The NULL-ctx
 * leg of each shim's ``RA8_CHECK_NULL_PTR`` guard is exercised last.
 */
static void test_transport_sci_factory_and_shims(void)
{
  TEST_BEGIN("transport_sci factory + shims");
  sci_factory_prep();

  ra8_sdmmc_spi_transport_t tr = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_transport_sci(0U, 60000000U, &s_factory_pins, &tr));
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)tr.set_clock);
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)tr.cs);
  TEST_ASSERT_NOT_NULL((void*)(uintptr_t)tr.xfer);
  TEST_ASSERT_NOT_NULL(tr.ctx);

  /* set_clock shim: retune the SCI baud divider. */
  TEST_ASSERT_EQ(k_ra8_ok, tr.set_clock(tr.ctx, 1000000U));

  /* cs shim: assert (CS low) then release (CS high). */
  TEST_ASSERT_EQ(k_ra8_ok, tr.cs(tr.ctx, true));
  TEST_ASSERT_EQ(k_ra8_ok, tr.cs(tr.ctx, false));

  /* xfer shim: pre-seed TDRE + RDRF so the SCI poll resolves immediately. */
  volatile r_sci_regs_t* reg = ra8_sci(0U);
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  reg->RDR = 0x5AU;
  const uint8_t tx[2] = {0xA5U, 0x5AU};
  uint8_t       rx[2] = {0U, 0U};
  TEST_ASSERT_EQ(k_ra8_ok, tr.xfer(tr.ctx, tx, rx, 2U));

  /* NULL-ctx guard on each shim. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, tr.set_clock(nullptr, 1000000U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, tr.cs(nullptr, true));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, tr.xfer(nullptr, tx, rx, 2U));
  TEST_END("transport_sci factory + shims");
}

/**
 * @par MC/DC:
 * Decision (ra8_sdmmc_spi_transport_sci argument guards): the two
 * ``RA8_CHECK_NULL_PTR`` macros and the ``pclk_hz == 0`` check are three
 * independent single-condition guards, exercised one at a time:
 *   - V1: pins NULL                     -> k_ra8_err_null_ptr.
 *   - V2: out NULL                      -> k_ra8_err_null_ptr.
 *   - V3: pclk_hz == 0                  -> k_ra8_err_invalid_arg.
 *   - V4: bad SCK port (bring-up fails) -> propagated HAL error.
 * V4 makes ``ra8_pfs_route_peripheral`` reject the out-of-range SCK port,
 * exercising the bring-up error-return leg the success case skips.
 */
static void test_transport_sci_factory_rejects(void)
{
  TEST_BEGIN("transport_sci factory rejects");
  sci_factory_prep();

  ra8_sdmmc_spi_transport_t tr = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_transport_sci(0U, 60000000U, nullptr, &tr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sdmmc_spi_transport_sci(0U, 60000000U, &s_factory_pins, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_transport_sci(0U, 0U, &s_factory_pins, &tr));

  /* Out-of-range SCK port (> k_ra8_port_max) -> ra8_pfs_route_peripheral fails. */
  const ra8_sdmmc_spi_sci_pins_t bad = {
    .sck  = RA8_PIN(99U, 0U),
    .cipo = RA8_PIN(1U, 1U),
    .copi = RA8_PIN(1U, 2U),
    .cs   = RA8_PIN(1U, 3U),
  };
  TEST_ASSERT(ra8_sdmmc_spi_transport_sci(0U, 60000000U, &bad, &tr) != k_ra8_ok);
  TEST_END("transport_sci factory rejects");
}

/* ===========================================================================
 * Lifecycle error legs
 * ===========================================================================
 */

/**
 * @brief Bring the driver up against an SDHC 32 GiB mock and assert success.
 *
 * @details Shorthand used by the block-I/O error tests below: resets the mock,
 * queues the full CMD0..CMD16 identification sequence, and initialises.
 */
static void init_sdhc_ok(void)
{
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
}

/**
 * @par MC/DC:
 * No compound decision -- ``internal_prepare_init`` guards on the single
 * condition ``s_state.initialized``. V1 (false) is the happy init above;
 * V2 (true) is the second init here, which must report invalid_state.
 */
static void test_init_double_rejected(void)
{
  TEST_BEGIN("init twice -> invalid_state");
  init_sdhc_ok();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_init(&s_mock_transport));
  TEST_END("init twice -> invalid_state");
}

/**
 * @par MC/DC:
 * No compound decision -- ``internal_finalize_init`` guards on the single
 * condition ``err != k_ra8_ok`` from the data-rate set_clock. The counting
 * transport fails that second set_clock, so init must propagate the error
 * and leave the driver un-initialised.
 */
static void test_init_finalize_clock_failure(void)
{
  TEST_BEGIN("init finalize set_clock failure propagates");
  per_test_setup();
  s_setclk_calls = 0U;
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_init(&s_mock_transport_failclk));
  /* Driver must remain un-initialised after a finalize failure. */
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_END("init finalize set_clock failure propagates");
}

/* ===========================================================================
 * Read-block error + byte-addressed paths
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * No compound decision -- ``ra8_sdmmc_spi_read_block`` rejects before init on
 * the single condition ``!s_state.initialized``. The buffer is non-NULL so
 * the preceding NULL guard passes and this guard is the one under test.
 */
static void test_read_block_rejects_uninit(void)
{
  TEST_BEGIN("read_block before init -> invalid_state");
  per_test_setup();
  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_read_block(0U, buf));
  TEST_END("read_block before init -> invalid_state");
}

/**
 * @brief Build a minimal CSD v1 register for a ~1 GiB byte-addressed card.
 *
 * @details READ_BL_LEN=9 (512 B), C_SIZE=1023, C_SIZE_MULT=7. Mirrors the
 * vector used by the v1 classification init test.
 */
static void build_csd_v1_1gib(uint8_t* out)
{
  memset(out, 0, (size_t)k_ra8_sdmmc_spi_csd_response_len);
  out[0]  = 0x00U; /* CSD_STRUCTURE = 0 */
  out[5]  = 0x09U; /* READ_BL_LEN = 9   */
  out[6]  = 0x03U; /* C_SIZE[11:10]     */
  out[7]  = 0xFFU; /* C_SIZE[9:2]       */
  out[8]  = 0xC0U; /* C_SIZE[1:0]       */
  out[9]  = 0x03U; /* C_SIZE_MULT[2:1]  */
  out[10] = 0x80U; /* C_SIZE_MULT[0]    */
}

/**
 * @brief Queue a successful CMD0..CMD16 init for a v1.x (byte-addressed) card.
 */
static void queue_full_init_sdv1_1gib(void)
{
  mock_queue_idle(10U);
  queue_command_response_r1((uint8_t)k_test_r1_idle);        /* CMD0         */
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd); /* CMD8 -> v1   */
  queue_command_response_r1((uint8_t)k_test_r1_idle);        /* CMD55        */
  queue_command_response_r1((uint8_t)k_test_r1_ready);       /* ACMD41 ready */
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len];
  build_csd_v1_1gib(csd);
  queue_csd_read(csd);
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD16 */
}

/**
 * @par MC/DC:
 * No compound decision -- exercises ``internal_lba_to_arg`` on a v1.x card so
 * the byte-address branch (``lba * block_size``) runs instead of the SDHC
 * pass-through. A successful read-back confirms the converted argument framed
 * a valid CMD17.
 */
static void test_read_block_byte_addressed_v1(void)
{
  TEST_BEGIN("read_block byte-addressed (v1) LBA conversion");
  per_test_setup();
  queue_full_init_sdv1_1gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  ra8_sdmmc_spi_card_type_t type = k_ra8_sdmmc_spi_type_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_card_type(&type));
  TEST_ASSERT_EQ(k_ra8_sdmmc_spi_type_sdv1, type);

  uint8_t block[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    block[i] = (uint8_t)((i * 3U) & 0xFFU);
  }
  queue_read_back(block);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_read_block(1U, buf));
  TEST_ASSERT_EQ(0, memcmp(buf, block, sizeof(buf)));
  TEST_END("read_block byte-addressed (v1) LBA conversion");
}

/**
 * @par MC/DC:
 * No compound decision -- drives the single-condition error legs of the read
 * data phase one per sub-case:
 *   - CMD17 R1 non-zero            -> protocol_error (R1 check).
 *   - data token never arrives     -> hw_timeout (wait_data_token budget).
 *   - frame xfer faulted (call 2)  -> propagated bus error (send-command leg).
 *   - payload xfer faulted (call 5)-> propagated bus error (payload drain leg).
 * The last two use the deterministic fault injector, not a timer.
 */
static void test_read_block_error_legs(void)
{
  TEST_BEGIN("read_block protocol / timeout / xfer-fault legs");
  uint8_t buf[k_ra8_sdmmc_spi_block_size];

  /* CMD17 R1 non-zero -> protocol error. CS is held across the read, so model
   * the session directly: cs-assert idle, CMD17 frame, R1, cs-release idle. */
  init_sdhc_ok();
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_illegal_cmd);
  mock_queue_idle(1U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_read_block(0U, buf));

  /* CMD17 R1 ready but no data-start token -> the wait loop times out. */
  init_sdhc_ok();
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  /* No 0xFE queued; the mock under-runs to 0xFF forever, never matching. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_read_block(0U, buf));

  /* Frame xfer faults (call 2: after cs-assert's idle byte). */
  init_sdhc_ok();
  mock_arm_xfer_fail(2U);
  TEST_ASSERT(ra8_sdmmc_spi_read_block(0U, buf) != k_ra8_ok);

  /* Payload xfer faults: cs-assert(1) + frame(2) + R1(3) + token(4) then the
   * first payload byte (5) faults. Queue the bytes calls 1..4 consume. */
  init_sdhc_ok();
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_arm_xfer_fail(5U);
  TEST_ASSERT(ra8_sdmmc_spi_read_block(0U, buf) != k_ra8_ok);
  TEST_END("read_block protocol / timeout / xfer-fault legs");
}

/* ===========================================================================
 * Write-block error + fallback paths
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * Decision: ``lba >= capacity`` is single-condition; the not-initialised
 * guard is single-condition. V1 (init done, lba in range) is the happy-path
 * test; here V2 covers the not-initialised reject and V3 covers lba == cap.
 */
static void test_write_block_rejects_uninit_and_oor(void)
{
  TEST_BEGIN("write_block uninit + out-of-range");
  uint8_t buf[k_ra8_sdmmc_spi_block_size] = {};

  per_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_write_block(0U, buf));

  init_sdhc_ok();
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_write_block(cap, buf));
  TEST_END("write_block uninit + out-of-range");
}

/**
 * @par MC/DC:
 * No new compound decision -- drives the single-condition error legs of the
 * write path:
 *   - CMD24 R1 non-zero          -> protocol_error.
 *   - CMD24 frame xfer faulted   -> propagated bus error (send-command leg).
 * Both leave CS released on the way out.
 */
static void test_write_block_command_errors(void)
{
  TEST_BEGIN("write_block command error legs");
  uint8_t buf[k_ra8_sdmmc_spi_block_size] = {};

  /* CMD24 R1 non-zero -> protocol error. */
  init_sdhc_ok();
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_write_block(1U, buf));

  /* CMD24 frame xfer faults (call 2: after the cs-assert idle byte). */
  init_sdhc_ok();
  mock_arm_xfer_fail(2U);
  TEST_ASSERT(ra8_sdmmc_spi_write_block(1U, buf) != k_ra8_ok);
  TEST_END("write_block command error legs");
}

/**
 * @par MC/DC:
 * No compound decision -- exercises the per-byte write fallback. The no-bulk
 * transport rejects the 512-byte payload xfer, so the driver streams the block
 * one byte at a time instead. The data-response token then reports "accepted"
 * and the busy wait clears, so the whole write still returns success.
 */
static void test_write_block_per_byte_fallback(void)
{
  TEST_BEGIN("write_block per-byte fallback (no bulk xfer)");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport_no_bulk));

  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD24 R1 ready. */
  queue_write_block_tail((uint8_t)k_test_data_response_accept, 1U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    buf[i] = (uint8_t)((i * 5U) & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_block(1U, buf));
  TEST_END("write_block per-byte fallback (no bulk xfer)");
}

/* ===========================================================================
 * Multi-block write (CMD25)
 * ===========================================================================
 */

/**
 * @brief Queue the CS-assert + ACMD23 + CMD25 lead-in of a multi-block write.
 * @param[in] cmd25_r1 R1 byte the card returns for CMD25 (0 = ready).
 *
 * @details CS is held across the whole transaction, so (unlike
 * queue_command_response_r1) no per-command cs-release idle is inserted. The
 * best-effort ACMD23 (CMD55 + ACMD23) responses are ignored by the driver.
 */
static void queue_multi_write_lead(uint8_t cmd25_r1)
{
  mock_queue_idle(1U); /* cs_assert post-byte. */
  /* ACMD23 = CMD55 then ACMD23, each a 6-byte frame + R1. */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  /* CMD25 frame + R1. */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte(cmd25_r1);
}

/**
 * @brief Queue one streamed data block of a multi-block write.
 * @param[in] data_response Data-response token the card returns (0x05 accepts).
 */
static void queue_multi_data_block(uint8_t data_response)
{
  mock_queue_idle(1U);                                   /* N_WR pad.             */
  mock_queue_idle(1U);                                   /* data-token TX slot.   */
  mock_queue_idle((uint32_t)k_ra8_sdmmc_spi_block_size); /* 512 payload TX slots. */
  mock_queue_idle(2U);                                   /* 2 CRC TX slots.       */
  mock_queue_byte(data_response);                        /* data-response token.  */
  mock_queue_byte((uint8_t)k_mock_xfer_byte_idle);       /* busy released 0xFF.   */
}

/**
 * @brief Queue the stop-tran token + final busy wait + cs-release of CMD25.
 */
static void queue_multi_stop(void)
{
  mock_queue_idle(1U);                             /* send_idle pad.        */
  mock_queue_idle(1U);                             /* stop-tran TX slot.    */
  mock_queue_idle(1U);                             /* send_idle pad.        */
  mock_queue_byte((uint8_t)k_mock_xfer_byte_idle); /* busy released 0xFF.   */
  mock_queue_idle(1U);                             /* cs_release post-byte. */
}

/**
 * @par MC/DC:
 * Decision: ``(lba >= capacity) || (count > (capacity - lba))`` (2 conditions)
 * in ``ra8_sdmmc_spi_write_blocks``.
 *   - V1: lba < cap, count <= cap - lba -> false (happy multi-write below).
 *   - V2: lba == cap, count == 1        -> true  (first operand).
 *   - V3: lba = cap - 1, count = 2      -> true  (second operand).
 * Pairs (V1,V2) and (V1,V3) prove each operand independently moves the
 * outcome. The case also covers the count == 0 no-op and the count == 1
 * single-block delegation legs that precede the range check / stream.
 */
static void test_write_blocks_arg_guards(void)
{
  TEST_BEGIN("write_blocks count/range guards");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};

  init_sdhc_ok();
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));

  /* count == 0 -> no-op success, bus untouched. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_blocks(0U, buf, 0U));
  /* V2: lba == cap -> out of range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_write_blocks(cap, buf, 1U));
  /* V3: lba = cap - 1, count = 2 -> count exceeds remaining range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_write_blocks(cap - 1U, buf, 2U));

  /* count == 1 -> single-block delegation. Queue a single CMD24 write. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  queue_write_block_tail((uint8_t)k_test_data_response_accept, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_blocks(5U, buf, 1U));
  TEST_END("write_blocks count/range guards");
}

/**
 * @par MC/DC:
 * No new compound decision -- V1 (false control) of the range check above.
 * Drives the full CMD25 stream: ACMD23 pre-erase hint, CMD25, two streamed
 * blocks each accepted, then the stop-tran token and final busy wait.
 */
static void test_write_blocks_multi_happy(void)
{
  TEST_BEGIN("write_blocks multi-block CMD25 stream");
  init_sdhc_ok();

  queue_multi_write_lead((uint8_t)k_test_r1_ready);
  queue_multi_data_block((uint8_t)k_test_data_response_accept);
  queue_multi_data_block((uint8_t)k_test_data_response_accept);
  queue_multi_stop();

  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U];
  for (uint32_t i = 0U; i < (uint32_t)(k_ra8_sdmmc_spi_block_size * 2U); i++) {
    buf[i] = (uint8_t)((i * 11U) & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_blocks(2U, buf, 2U));
  TEST_END("write_blocks multi-block CMD25 stream");
}

/**
 * @par MC/DC:
 * Decision: ``(err != k_ra8_ok) || (r1 != 0U)`` (2 conditions) after the CMD25
 * send in ``ra8_sdmmc_spi_write_blocks``.
 *   - V1: err == ok, r1 == 0     -> false (the happy CMD25 stream test above).
 *   - V2: err != ok (xfer fault) -> true  (first operand; short-circuits).
 *   - V3: err == ok, r1 != 0     -> true  (second operand).
 * Pairs (V1,V2) and (V1,V3) prove each operand independently moves the
 * outcome. Both reject legs release CS without streaming any data block (the
 * CMD25 stream never opened).
 */
static void test_write_blocks_cmd25_rejected(void)
{
  TEST_BEGIN("write_blocks CMD25 reject legs");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};

  /* V3: CMD25 R1 reports illegal command -> protocol error, no data stream. The
   * best-effort ACMD23 (CMD55 + ACMD23) answers ready and is ignored. */
  init_sdhc_ok();
  queue_multi_write_lead((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_write_blocks(2U, buf, 2U));

  /* V2: the CMD25 frame xfer faults. The cs-assert idle is call 1; every call
   * from 2 onward faults, so the best-effort ACMD23 fails (ignored) and then
   * send_command(CMD25) returns a bus error before any R1 is read. */
  init_sdhc_ok();
  mock_arm_xfer_fail(2U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_write_blocks(2U, buf, 2U));
  TEST_END("write_blocks CMD25 reject legs");
}

/* ===========================================================================
 * Multi-block read (CMD18 + CMD12)
 * ===========================================================================
 */

/**
 * @enum test_wire_cmd_t
 * @brief On-wire command lead bytes spot-checked in the driver's TX log.
 */
typedef enum : uint8_t {
  k_test_wire_cmd18 = 0x52U, /**< 0x40 | 18 -- READ_MULTIPLE_BLOCK. */
  k_test_wire_cmd12 = 0x4CU, /**< 0x40 | 12 -- STOP_TRANSMISSION.   */
} test_wire_cmd_t;

/**
 * @brief Scan the mock TX log for @p byte starting at index @p from.
 * @param[in] byte Byte value to look for.
 * @param[in] from First TX-log index to inspect.
 * @return Index of the first match, or UINT32_MAX when absent.
 */
static uint32_t tx_log_find(uint8_t byte, uint32_t from)
{
  for (uint32_t i = from; i < s_mock.tx_len; i++) {
    if (s_mock.tx_log[i] == byte) {
      return i;
    }
  }
  return UINT32_MAX;
}

/**
 * @brief Queue the CS-assert + CMD18 lead-in of a multi-block read.
 * @param[in] cmd18_r1 R1 byte the card returns for CMD18 (0 = ready).
 *
 * @details CS is held across the whole transaction, so (like
 * queue_multi_write_lead and unlike queue_command_response_r1) no
 * per-command cs-release idle is inserted.
 */
static void queue_multi_read_lead(uint8_t cmd18_r1)
{
  mock_queue_idle(1U);                               /* cs_assert post-byte. */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes); /* CMD18 frame.         */
  mock_queue_byte(cmd18_r1);                         /* CMD18 R1.            */
}

/**
 * @brief Queue one streamed CMD18 block: data token + payload + valid CRC16.
 * @param[in] block 512-byte payload; its CRC16 trailer is computed here.
 */
static void queue_multi_read_block(const uint8_t* block)
{
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  const uint16_t crc = ra8_sdmmc_spi_crc16(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  mock_queue_byte((uint8_t)((crc >> 8U) & 0xFFU));
  mock_queue_byte((uint8_t)(crc & 0xFFU));
}

/**
 * @brief Queue one streamed CMD18 block whose CRC16 trailer is corrupted.
 * @param[in] block 512-byte payload streamed before the bad trailer.
 */
static void queue_multi_read_block_bad_crc(const uint8_t* block)
{
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  const uint16_t crc = ra8_sdmmc_spi_crc16(block, (uint32_t)k_ra8_sdmmc_spi_block_size);
  mock_queue_byte((uint8_t)(~(crc >> 8U) & 0xFFU)); /* inverted -> mismatch. */
  mock_queue_byte((uint8_t)(crc & 0xFFU));
}

/**
 * @brief Queue the CMD12 stop of a CMD18 stream + busy release + cs-release.
 * @param[in] cmd12_r1 R1 byte the card returns for CMD12 (0 = ready).
 *
 * @details The driver clocks the 6-byte CMD12 frame, discards one stuff byte
 * (the undefined tail of the cut-off stream), polls R1, then waits not-busy.
 */
static void queue_multi_read_stop(uint8_t cmd12_r1)
{
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes); /* CMD12 frame TX slots.  */
  mock_queue_idle(1U);                               /* stuff byte, discarded. */
  mock_queue_byte(cmd12_r1);                         /* CMD12 R1.              */
  mock_queue_byte(0x00U);                            /* busy while stopping.   */
  mock_queue_byte((uint8_t)k_test_busy_done);        /* busy released 0xFF.    */
  mock_queue_idle(1U);                               /* cs_release post-byte.  */
}

/**
 * @par MC/DC:
 * Decision: ``(lba >= capacity) || (count > (capacity - lba))`` (2 conditions)
 * in ``ra8_sdmmc_spi_read_blocks`` -- the read-side mirror of the write_blocks
 * range guard.
 *   - V1: lba < cap, count <= cap - lba -> false (happy multi-read test).
 *   - V2: lba == cap, count == 1        -> true  (first operand).
 *   - V3: lba = cap - 1, count = 2      -> true  (second operand).
 * Pairs (V1,V2) and (V1,V3) prove each operand independently moves the
 * outcome. The case also covers the NULL-buf guard, the not-initialised
 * guard, the count == 0 no-op, and the count == 1 single-block (CMD17)
 * delegation legs that precede the range check / stream.
 */
static void test_read_blocks_arg_guards(void)
{
  TEST_BEGIN("read_blocks count/range guards");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};

  /* Not initialised -> invalid_state (guard before any bus traffic). */
  per_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_read_blocks(0U, buf, 1U));

  init_sdhc_ok();
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));

  /* NULL destination -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_read_blocks(0U, nullptr, 1U));
  /* count == 0 -> no-op success, bus untouched. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_read_blocks(0U, buf, 0U));
  /* V2: lba == cap -> out of range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_read_blocks(cap, buf, 1U));
  /* V3: lba = cap - 1, count = 2 -> count exceeds remaining range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_read_blocks(cap - 1U, buf, 2U));

  /* count == 1 -> single-block CMD17 delegation. Queue one CMD17 read. */
  uint8_t block[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    block[i] = (uint8_t)((i * 9U) & 0xFFU);
  }
  queue_read_back(block);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_read_blocks(6U, buf, 1U));
  TEST_ASSERT_EQ(0, memcmp(buf, block, (size_t)k_ra8_sdmmc_spi_block_size));
  TEST_END("read_blocks count/range guards");
}

/**
 * @par MC/DC:
 * No new compound decision -- V1 (false control) of the read_blocks range
 * check above and of the CMD18-reject decision documented on
 * ::test_read_blocks_cmd18_rejected. Drives the full CMD18 stream: one
 * READ_MULTIPLE_BLOCK command, two streamed blocks (data token + payload +
 * verified CRC16 each), then CMD12 STOP_TRANSMISSION with its stuff byte,
 * R1, and busy wait. The TX log is spot-checked to prove a single CMD18
 * frame went out (not per-block CMD17s) and that CMD12 followed the stream.
 */
static void test_read_blocks_multi_happy(void)
{
  TEST_BEGIN("read_blocks multi-block CMD18 stream");
  init_sdhc_ok();

  uint8_t block0[k_ra8_sdmmc_spi_block_size];
  uint8_t block1[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    block0[i] = (uint8_t)((i * 3U) & 0xFFU);
    block1[i] = (uint8_t)((i * 5U) + 1U);
  }
  queue_multi_read_lead((uint8_t)k_test_r1_ready);
  queue_multi_read_block(block0);
  queue_multi_read_block(block1);
  queue_multi_read_stop((uint8_t)k_test_r1_ready);

  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U];
  memset(buf, 0xA5U, sizeof(buf));
  const uint32_t tx_start = s_mock.tx_len; /* skip the recorded init TX. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_read_blocks(3U, buf, 2U));
  TEST_ASSERT_EQ(0, memcmp(&buf[0], block0, (size_t)k_ra8_sdmmc_spi_block_size));
  TEST_ASSERT_EQ(
    0,
    memcmp(&buf[k_ra8_sdmmc_spi_block_size], block1, (size_t)k_ra8_sdmmc_spi_block_size));

  /* Wire framing: the first TX byte of the transaction is the cs-assert
   * idle, the next the CMD18 lead byte; a CMD12 lead byte follows the
   * streamed payload. */
  TEST_ASSERT_EQ(k_test_wire_cmd18, s_mock.tx_log[tx_start + 1U]);
  TEST_ASSERT(tx_log_find((uint8_t)k_test_wire_cmd12, tx_start + 2U) != UINT32_MAX);
  TEST_END("read_blocks multi-block CMD18 stream");
}

/**
 * @par MC/DC:
 * Decision: ``(err != k_ra8_ok) || (r1 != 0U)`` (2 conditions) after the CMD18
 * send in ``ra8_sdmmc_spi_read_blocks``.
 *   - V1: err == ok, r1 == 0     -> false (the happy stream test above).
 *   - V2: err != ok (xfer fault) -> true  (first operand; short-circuits).
 *   - V3: err == ok, r1 != 0     -> true  (second operand).
 * Pairs (V1,V2) and (V1,V3) prove each operand independently moves the
 * outcome. Both reject legs release CS without issuing CMD12 (the stream
 * never opened). The cs-assert RA8_RETURN_ON_ERROR leg is exercised last.
 */
static void test_read_blocks_cmd18_rejected(void)
{
  TEST_BEGIN("read_blocks CMD18 reject legs");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};

  /* V3: CMD18 R1 reports illegal command -> protocol error, no CMD12. */
  init_sdhc_ok();
  queue_multi_read_lead((uint8_t)k_test_r1_illegal_cmd);
  const uint32_t tx_start = s_mock.tx_len; /* skip the recorded init TX. */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));
  TEST_ASSERT_EQ(UINT32_MAX, tx_log_find((uint8_t)k_test_wire_cmd12, tx_start));

  /* V2: the CMD18 frame xfer faults (call 2: after the cs-assert idle). */
  init_sdhc_ok();
  mock_arm_xfer_fail(2U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));

  /* cs-assert leg: the very first idle byte faults -> propagated. */
  init_sdhc_ok();
  mock_arm_xfer_fail(1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));
  TEST_END("read_blocks CMD18 reject legs");
}

/**
 * @par MC/DC:
 * No new compound decision -- exercises the single-condition abort legs of
 * the CMD18 stream and its stop sequence:
 *   - a mid-stream CRC16 mismatch aborts the stream, CMD12 is STILL sent
 *     (the card keeps streaming until STOP_TRANSMISSION) and the stream
 *     error wins over the stop result;
 *   - a missing second data token times out the stream; the CMD12 R1 poll
 *     then times out too and the stream timeout is what propagates;
 *   - a clean stream whose CMD12 R1 is non-zero -> protocol_error from the
 *     stop leg alone (``r1 != 0U`` inside internal_read_multi_stop).
 */
static void test_read_blocks_stream_error_still_stops(void)
{
  TEST_BEGIN("read_blocks stream abort still sends CMD12");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};
  uint8_t block[k_ra8_sdmmc_spi_block_size];
  memset(block, 0xC5U, sizeof(block));

  /* Block 1 streams clean; block 2 delivers a corrupt CRC16 trailer. The
   * driver must abort with crc_mismatch AND still issue CMD12. */
  init_sdhc_ok();
  queue_multi_read_lead((uint8_t)k_test_r1_ready);
  queue_multi_read_block(block);
  queue_multi_read_block_bad_crc(block);
  queue_multi_read_stop((uint8_t)k_test_r1_ready);
  const uint32_t tx_start = s_mock.tx_len; /* skip the recorded init TX. */
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));
  TEST_ASSERT(tx_log_find((uint8_t)k_test_wire_cmd12, tx_start) != UINT32_MAX);

  /* The second data token never arrives: the stream times out; the CMD12
   * R1 poll then times out as well; the stream timeout propagates. */
  init_sdhc_ok();
  queue_multi_read_lead((uint8_t)k_test_r1_ready);
  queue_multi_read_block(block);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));

  /* Clean 2-block stream but CMD12 answers a non-zero R1 -> protocol_error
   * from the stop leg (the stream itself succeeded). */
  init_sdhc_ok();
  queue_multi_read_lead((uint8_t)k_test_r1_ready);
  queue_multi_read_block(block);
  queue_multi_read_block(block);
  queue_multi_read_stop((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_read_blocks(0U, buf, 2U));
  TEST_END("read_blocks stream abort still sends CMD12");
}

/* ===========================================================================
 * Erase error legs
 * ===========================================================================
 */

/**
 * @brief Queue a single CS-bracketed command whose CMD38-style body is
 *        followed by an immediate cs-release (used for the CMD38 R1-error leg).
 */
static void queue_cmd38_r1_error(uint8_t r1)
{
  mock_queue_idle(1U);                               /* cs_assert post-byte.  */
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes); /* CMD38 frame.          */
  mock_queue_byte(r1);                               /* R1 (non-zero).        */
  mock_queue_idle(1U);                               /* cs_release post-byte. */
}

/**
 * @par MC/DC:
 * No new compound decision -- exercises the single-condition error legs that
 * the happy erase test cannot reach:
 *   - CMD32 R1 non-zero            -> probe erase fails (cmd-require-ready).
 *   - CMD33 R1 non-zero            -> erase-range end fails.
 *   - CMD38 R1 non-zero            -> erase command rejected.
 *   - read-back CMD17 R1 non-zero  -> post-erase verify read fails.
 *   - second-range CMD32 non-zero  -> remaining-range erase fails.
 * Each is a fresh init so the mock queue starts clean.
 */
static void test_erase_blocks_error_legs(void)
{
  TEST_BEGIN("erase_blocks error legs");

  /* CMD32 (probe erase start) R1 non-zero -> protocol error. */
  init_sdhc_ok();
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* CMD33 (erase end) R1 non-zero -> protocol error. */
  init_sdhc_ok();
  queue_command_response_r1((uint8_t)k_test_r1_ready);       /* CMD32 ok  */
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd); /* CMD33 bad */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* CMD38 (erase) R1 non-zero -> protocol error. */
  init_sdhc_ok();
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD32 ok */
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD33 ok */
  queue_cmd38_r1_error((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* Probe erase OK, but the post-erase read-back's CMD17 R1 is non-zero. */
  init_sdhc_ok();
  queue_erase_blocks_ok(); /* probe erase [0,1) */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_illegal_cmd); /* CMD17 R1 bad */
  mock_queue_idle(1U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* Probe erase OK, read-back all-zero, but the remaining-range CMD32 fails. */
  init_sdhc_ok();
  queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t zeros[k_ra8_sdmmc_spi_block_size] = {};
  queue_read_back(zeros);                                    /* read-back zero -> erase the rest */
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd); /* second CMD32 bad                 */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));
  TEST_END("erase_blocks error legs");
}

/* ===========================================================================
 * Capacity / type guards + ra8_fs backend shims
 * ===========================================================================
 */

/**
 * @par MC/DC:
 * No compound decision -- both queries reject on the single condition
 * ``!s_state.initialized`` and on their NULL-pointer guards. Exercises the
 * NULL guard and the not-initialised guard of each query.
 */
static void test_capacity_type_query_guards(void)
{
  TEST_BEGIN("get_capacity / get_card_type guards");
  per_test_setup();

  uint32_t                  cap  = 0U;
  ra8_sdmmc_spi_card_type_t type = k_ra8_sdmmc_spi_type_unknown;
  /* NULL-pointer guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_get_capacity(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdmmc_spi_get_card_type(nullptr));
  /* Not-initialised guards. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_get_card_type(&type));
  TEST_END("get_capacity / get_card_type guards");
}

/**
 * @par MC/DC:
 * No new compound decision -- exercises the ra8_fs backend shims through the
 * bound descriptor:
 *   - read_block NULL buf            -> null_ptr.
 *   - read_block over-range forward  -> propagated out_of_range.
 *   - read_block one-block forward   -> success (queued CMD17 read).
 *   - write_block NULL buf           -> null_ptr.
 *   - write_block one-block          -> success via write_blocks delegation.
 *   - erase_blocks count == 0        -> invalid_arg passthrough.
 *   - get_capacity after deinit      -> invalid_state.
 */
static void test_fs_backend_shims(void)
{
  TEST_BEGIN("ra8_fs backend shims");
  init_sdhc_ok();

  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_bind_fs_backend(&backend));

  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));

  uint8_t buf[k_ra8_sdmmc_spi_block_size] = {};

  /* read_block shim: NULL buf, then over-range fan-out error. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, backend.read_block(backend.ctx, 0U, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, backend.read_block(backend.ctx, cap, 1U, buf));

  /* read_block shim: a one-block fan-out that succeeds. */
  uint8_t block[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    block[i] = (uint8_t)((i * 13U) & 0xFFU);
  }
  queue_read_back(block);
  TEST_ASSERT_EQ(k_ra8_ok, backend.read_block(backend.ctx, 0U, 1U, buf));
  TEST_ASSERT_EQ(0, memcmp(buf, block, sizeof(buf)));

  /* write_block shim: NULL buf, then a one-block write that succeeds. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, backend.write_block(backend.ctx, 0U, 1U, nullptr));
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  queue_write_block_tail((uint8_t)k_test_data_response_accept, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, backend.write_block(backend.ctx, 3U, 1U, block));

  /* erase_blocks shim: count == 0 passes through to invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, backend.erase_blocks(backend.ctx, 0U, 0U));

  /* get_capacity shim after deinit -> invalid_state (descriptor outlives init). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_deinit());
  uint32_t bc = 0U;
  uint32_t bs = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, backend.get_capacity(backend.ctx, &bc, &bs));
  TEST_END("ra8_fs backend shims");
}

/* ===========================================================================
 * Main
 * ===========================================================================
 */

int main(void)
{
  test_mcdc_validate_transport_or_chain();
  test_mcdc_fs_get_capacity_null_or();
  test_crc7_published_vectors();
  test_crc16_published_vectors();
  test_init_null_transport_rejected();
  test_init_validates_callbacks();
  test_init_full_sdhc_path();
  test_init_cmd8_classifies_v1_card();
  test_read_block_rejects_null();
  test_read_block_rejects_oor();
  test_read_block_happy_path();
  test_read_block_detects_crc_mismatch();
  test_write_block_happy_path();
  test_write_block_detects_write_error();
  test_bind_fs_backend_populates_struct();
  test_bind_fs_backend_uninitialized_rejected();
  test_erase_blocks_verifies_zero();
  test_transport_sci_factory_and_shims();
  test_transport_sci_factory_rejects();
  test_init_double_rejected();
  test_init_finalize_clock_failure();
  test_read_block_rejects_uninit();
  test_read_block_byte_addressed_v1();
  test_read_block_error_legs();
  test_write_block_rejects_uninit_and_oor();
  test_write_block_command_errors();
  test_write_block_per_byte_fallback();
  test_write_blocks_arg_guards();
  test_write_blocks_multi_happy();
  test_write_blocks_cmd25_rejected();
  test_read_blocks_arg_guards();
  test_read_blocks_multi_happy();
  test_read_blocks_cmd18_rejected();
  test_read_blocks_stream_error_still_stops();
  test_erase_blocks_error_legs();
  test_capacity_type_query_guards();
  test_fs_backend_shims();
  (void)fprintf(stderr, "[OK ] all ra8_sdmmc_spi tests passed\n");
  return 0;
}

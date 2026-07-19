/**
 * @file test_ra8_sdmmc_spi_read.c
 * @brief Unit tests for the ra8_sdmmc_spi read path: single-block reads
 *        (happy, CRC mismatch, byte-addressed V1 cards, error legs) and
 *        multi-block CMD18 reads with CMD12 stop handling
 *
 * @details Split from test_ra8_sdmmc_spi.c along the test-group seam;
 * the sibling test_ra8_sdmmc_spi.c owns CRC/init/fs-backend/factory and
 * test_ra8_sdmmc_spi_write.c the write + erase paths. The shared mock
 * SPI transport lives in support/sdmmc_spi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_sci_regs.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_sim_mmap.h"
#include "support/sdmmc_spi_test_util.h"
#include "unity_minimal.h"

/** @brief Distinguishable payloads for the SD block round-trip. */
typedef enum : uint8_t {
  k_sdspi_rd_fill_poison   = 0xA5U, /**< Poison the read must overwrite. */
  k_sdspi_rd_fill_expected = 0xC5U, /**< Bytes the card is made to return. */
} sdspi_rd_fill_t;

/**
 * @enum sdmmc_spi_read_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_sdmmc_spi_read_lit_7   = 7U,    /**< Sdmmc SPI read literal 7.    */
  k_sdmmc_spi_read_lit_xff = 0xFFU, /**< Sdmmc SPI read literal 0xFF. */
  k_sdmmc_spi_read_lit_xde = 0xDEU, /**< Sdmmc SPI read literal 0xDE. */
  k_sdmmc_spi_read_lit_xad = 0xADU, /**< Sdmmc SPI read literal 0xAD. */
  k_sdmmc_spi_read_lit_5   = 5,     /**< Sdmmc SPI read literal 5.    */
  k_sdmmc_spi_read_lit_x9  = 0x09U, /**< Sdmmc SPI read literal 0x9.  */
  k_sdmmc_spi_read_lit_xc0 = 0xC0U, /**< Sdmmc SPI read literal 0xC0. */
  k_sdmmc_spi_read_lit_9   = 9,     /**< Sdmmc SPI read literal 9.    */
  k_sdmmc_spi_read_lit_10  = 10,    /**< Sdmmc SPI read literal 10.   */
  k_sdmmc_spi_read_lit_x80 = 0x80U, /**< Sdmmc SPI read literal 0x80. */
} sdmmc_spi_read_test_lit_t;

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
    expected[i] = (uint8_t)((i * k_sdmmc_spi_read_lit_7) & k_sdmmc_spi_read_lit_xff);
  }
  const uint16_t crc16 = ra8_sdmmc_spi_crc16(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);

  /* CMD17 -> R1 ready. */
  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* Data token + payload + CRC16 trailer. */
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);
  mock_queue_byte((uint8_t)((crc16 >> 8U) & k_sdmmc_spi_read_lit_xff));
  mock_queue_byte((uint8_t)(crc16 & k_sdmmc_spi_read_lit_xff));

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  memset(buf, k_sdspi_rd_fill_poison, sizeof(buf));
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
  memset(expected, k_sdspi_rd_fill_expected, sizeof(expected));

  queue_command_response_r1((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_data_token_start);
  mock_queue_bytes(expected, (uint32_t)k_ra8_sdmmc_spi_block_size);
  /* Wrong CRC bytes. */
  mock_queue_byte(k_sdmmc_spi_read_lit_xde);
  mock_queue_byte(k_sdmmc_spi_read_lit_xad);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_sdmmc_spi_read_block(0U, buf));
  TEST_END("read_block detects bad CRC16");
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
  out[0]                       = 0x00U;                    /* CSD_STRUCTURE = 0 */
  out[k_sdmmc_spi_read_lit_5]  = k_sdmmc_spi_read_lit_x9;  /* READ_BL_LEN = 9   */
  out[6]                       = 0x03U;                    /* C_SIZE[11:10]     */
  out[k_sdmmc_spi_read_lit_7]  = k_sdmmc_spi_read_lit_xff; /* C_SIZE[9:2]       */
  out[8]                       = k_sdmmc_spi_read_lit_xc0; /* C_SIZE[1:0]       */
  out[k_sdmmc_spi_read_lit_9]  = 0x03U;                    /* C_SIZE_MULT[2:1]  */
  out[k_sdmmc_spi_read_lit_10] = k_sdmmc_spi_read_lit_x80; /* C_SIZE_MULT[0]    */
}

/**
 * @brief Queue a successful CMD0..CMD16 init for a v1.x (byte-addressed) card.
 */
static void queue_full_init_sdv1_1gib(void)
{
  mock_queue_idle(k_sdmmc_spi_read_lit_10);
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
    block[i] = (uint8_t)((i * 3U) & k_sdmmc_spi_read_lit_xff);
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
  mock_arm_xfer_fail(k_sdmmc_spi_read_lit_5);
  TEST_ASSERT(ra8_sdmmc_spi_read_block(0U, buf) != k_ra8_ok);
  TEST_END("read_block protocol / timeout / xfer-fault legs");
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
  mock_queue_byte((uint8_t)((crc >> 8U) & k_sdmmc_spi_read_lit_xff));
  mock_queue_byte((uint8_t)(crc & k_sdmmc_spi_read_lit_xff));
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
  mock_queue_byte((uint8_t)(~(crc >> 8U) & k_sdmmc_spi_read_lit_xff)); /* inverted -> mismatch. */
  mock_queue_byte((uint8_t)(crc & k_sdmmc_spi_read_lit_xff));
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
    block[i] = (uint8_t)((i * k_sdmmc_spi_read_lit_9) & k_sdmmc_spi_read_lit_xff);
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
    block0[i] = (uint8_t)((i * 3U) & k_sdmmc_spi_read_lit_xff);
    block1[i] = (uint8_t)((i * k_sdmmc_spi_read_lit_5) + 1U);
  }
  queue_multi_read_lead((uint8_t)k_test_r1_ready);
  queue_multi_read_block(block0);
  queue_multi_read_block(block1);
  queue_multi_read_stop((uint8_t)k_test_r1_ready);

  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U];
  memset(buf, k_sdspi_rd_fill_poison, sizeof(buf));
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
  memset(block, k_sdspi_rd_fill_expected, sizeof(block));

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
int main(void)
{
  test_read_block_rejects_null();
  test_read_block_rejects_oor();
  test_read_block_happy_path();
  test_read_block_detects_crc_mismatch();
  test_read_block_rejects_uninit();
  test_read_block_byte_addressed_v1();
  test_read_block_error_legs();
  test_read_blocks_arg_guards();
  test_read_blocks_multi_happy();
  test_read_blocks_cmd18_rejected();
  test_read_blocks_stream_error_still_stops();
  (void)fprintf(stderr, "[OK ] all ra8_sdmmc_spi read tests passed\n");
  return 0;
}

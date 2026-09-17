/**
 * @file test_ra8_sdmmc_spi_write.c
 * @brief Unit tests for the ra8_sdmmc_spi write + erase paths:
 *        single-block writes (happy, write error, per-byte fallback),
 *        multi-block CMD25 writes, and CMD32/33/38 erase with
 *        zero-verification and error legs
 *
 * @details Split from test_ra8_sdmmc_spi.c along the test-group seam;
 * the sibling test_ra8_sdmmc_spi.c owns CRC/init/fs-backend/factory and
 * test_ra8_sdmmc_spi_read.c the read paths. The shared mock SPI
 * transport lives in support/inc/sdmmc_spi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_sci_regs.h"
#include "ra8_sdmmc_spi.h"
#include "sdmmc_spi_test_util.h"
#include "unity_minimal.h"

/** @brief All-ones byte: the idle level an SD card holds on CIPO. */
typedef enum : uint8_t {
  k_sdspi_wr_idle_byte = 0xFFU, /**< Bus idle / no response. */
} sdspi_wr_fill_t;

/**
 * @enum sdmmc_spi_write_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_sdmmc_spi_write_lit_xff = 0xFFU, /**< Sdmmc SPI write literal 0xFF. */
  k_sdmmc_spi_write_lit_xd  = 0x0DU, /**< Sdmmc SPI write literal 0xD.  */
  k_sdmmc_spi_write_lit_5   = 5U,    /**< Sdmmc SPI write literal 5.    */
  k_sdmmc_spi_write_lit_11  = 11U,   /**< Sdmmc SPI write literal 11.   */
} sdmmc_spi_write_test_lit_t;

/**
 * @par MC/DC:
 * Happy path -- companion to *_detects_write_error; together they flip the
 * data-response token check decision ``(response & mask) != accepted``.
 * @brief Exercise test write block happy path.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_block_happy_path(void)
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
    buf[i] = (uint8_t)(i & k_sdmmc_spi_write_lit_xff);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_write_block(1U, buf));
  TEST_END("write_block happy path");
}

/**
 * @par MC/DC:
 * Decision: ``(response & mask) != accepted`` (1 condition). V1 accepted
 * (false) covered by happy-path test. V2 not-accepted (true) covered here.
 * @brief Exercise test write block detects write error.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_block_detects_write_error(void)
{
  TEST_BEGIN("write_block detects write-error token");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));

  queue_command_response_r1((uint8_t)k_test_r1_ready);
  /* Data-response = write error (0x0D). Card releases busy promptly. */
  queue_write_block_tail(k_sdmmc_spi_write_lit_xd, 0U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_write_block(2U, buf));
  TEST_END("write_block detects write-error token");
}

/**
 * @brief Queue a successful CMD32 + CMD33 + CMD38 erase sequence.
 *
 * @details CMD32 (ERASE_WR_BLK_START) and CMD33 (ERASE_WR_BLK_END) are plain
 * R1 commands; CMD38 (ERASE) is an R1 command followed by a busy wait that the
 * mock clears immediately with a single not-busy (0xFF) byte.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_queue_erase_blocks_ok(void)
{
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD32 */
  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD33 */
  /* CMD38: cs_assert idle + frame echo + R1 + not-busy token + cs_release idle.
   */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_ready);
  mock_queue_byte((uint8_t)k_test_busy_done);
  mock_queue_idle(1U);
}

/**
 * @test internal_test_erase_blocks_verifies_zero
 *
 * @par MC/DC:
 * Decision: the read-back verify loop ``if (blk[i] != 0U) return
 * not_supported;`` decides success. The SD post-erase value is card-dependent,
 * so a ::k_ra8_ok return must be *measured*, not assumed. The probe erases one
 * block, reads it back, and only erases the rest of the range when that block
 * is zero.
 *   - V1: probe block reads back all-zero -> loop never trips -> erase the rest
 * -> k_ra8_ok.
 *   - V2: probe block reads back non-zero  -> loop trips        ->
 * k_ra8_err_not_supported.
 *
 * Also drives the range guard ``(lba >= capacity) || (count > capacity - lba)``
 * (2 conditions) that precedes the erase:
 *   - RV1: lba < cap, count <= cap - lba -> false (the V1 erase of [0,64)
 * below).
 *   - RV2: lba == cap, count == 1        -> true  (first operand;
 * short-circuits).
 *   - RV3: lba = cap - 1, count == 2     -> true  (second operand: count spills
 *                                          past the last block).
 * Pairs (RV1,RV2) and (RV1,RV3) prove each operand independently moves the
 * outcome. The case also covers the uninitialized and count == 0 guards.
 * @brief Exercise test erase blocks verifies zero.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_erase_blocks_verifies_zero(void)
{
  TEST_BEGIN("erase_blocks probes + verifies a zero read-back");

  /* Uninitialized driver -> invalid_state (guard before the erase). */
  per_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  /* V1: probe block reads back all-zero -> erase the rest -> k_ra8_ok. The
   * probe erases [0,1) then, after the zero read-back, the remaining [1,64) --
   * two CMD32/33/38 sequences bracketing the read-back. */
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  uint32_t cap = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_get_capacity(&cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sdmmc_spi_erase_blocks(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_erase_blocks(cap, 1U));
  /* RV3: lba in range but count spills past the last block -> out_of_range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_sdmmc_spi_erase_blocks(cap - 1U, 2U));
  internal_queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t zeros[k_ra8_sdmmc_spi_block_size] = {};
  queue_read_back(zeros);           /* probe read-back: all zero */
  internal_queue_erase_blocks_ok(); /* erase the rest [1,64)     */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  /* V2: probe block reads back 0xFF (card erases to ones) -> not_supported, and
   * the rest of the range is NOT erased (no wasted full-region erase). */
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport));
  internal_queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t ones[k_ra8_sdmmc_spi_block_size];
  memset(ones, k_sdspi_wr_idle_byte, sizeof(ones));
  queue_read_back(ones); /* probe read-back: non-zero -> stop */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_sdmmc_spi_erase_blocks(0U, 64U));

  TEST_END("erase_blocks probes + verifies a zero read-back");
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
 * @brief Exercise test write block rejects uninit and oor.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_block_rejects_uninit_and_oor(void)
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
 * @brief Exercise test write block command errors.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_block_command_errors(void)
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
 * @brief Exercise test write block per byte fallback.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_block_per_byte_fallback(void)
{
  TEST_BEGIN("write_block per-byte fallback (no bulk xfer)");
  per_test_setup();
  queue_full_init_sdhc_32gib();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&s_mock_transport_no_bulk));

  queue_command_response_r1((uint8_t)k_test_r1_ready); /* CMD24 R1 ready. */
  queue_write_block_tail((uint8_t)k_test_data_response_accept, 1U);

  uint8_t buf[k_ra8_sdmmc_spi_block_size];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    buf[i] = (uint8_t)((i * k_sdmmc_spi_write_lit_5) & k_sdmmc_spi_write_lit_xff);
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
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_queue_multi_write_lead(uint8_t cmd25_r1)
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
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_queue_multi_data_block(uint8_t data_response)
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
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_queue_multi_stop(void)
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
 * @brief Exercise test write blocks arg guards.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_blocks_arg_guards(void)
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
 * @brief Exercise test write blocks multi happy.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_blocks_multi_happy(void)
{
  TEST_BEGIN("write_blocks multi-block CMD25 stream");
  init_sdhc_ok();

  internal_queue_multi_write_lead((uint8_t)k_test_r1_ready);
  internal_queue_multi_data_block((uint8_t)k_test_data_response_accept);
  internal_queue_multi_data_block((uint8_t)k_test_data_response_accept);
  internal_queue_multi_stop();

  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U];
  for (uint32_t i = 0U; i < (uint32_t)(k_ra8_sdmmc_spi_block_size * 2U); i++) {
    buf[i] = (uint8_t)((i * k_sdmmc_spi_write_lit_11) & k_sdmmc_spi_write_lit_xff);
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
 * @brief Exercise test write blocks cmd25 rejected.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_blocks_cmd25_rejected(void)
{
  TEST_BEGIN("write_blocks CMD25 reject legs");
  uint8_t buf[k_ra8_sdmmc_spi_block_size * 2U] = {};

  /* V3: CMD25 R1 reports illegal command -> protocol error, no data stream. The
   * best-effort ACMD23 (CMD55 + ACMD23) answers ready and is ignored. */
  init_sdhc_ok();
  internal_queue_multi_write_lead((uint8_t)k_test_r1_illegal_cmd);
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
 * Erase error legs
 * ===========================================================================
 */

/**
 * @brief Queue a single CS-bracketed command whose CMD38-style body is
 *        followed by an immediate cs-release (used for the CMD38 R1-error leg).
 * @param[in] r1 Nonzero R1 response byte to return after the command frame.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_queue_cmd38_r1_error(uint8_t r1)
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
 * @brief Exercise test erase blocks error legs.
 * @details Uses the bounded mock transport to exercise this SD path.
 * @pre The mock transport storage and SD driver test seam are available.
 * @pre This case has exclusive ownership of the process-local SD fixture.
 * @post Assertions establish the named protocol or fault-handling behavior.
 * @post The fixture remains resettable for the next independent case.
 * @note This deterministic host test performs no physical media access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_erase_blocks_error_legs(void)
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
  internal_queue_cmd38_r1_error((uint8_t)k_test_r1_illegal_cmd);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* Probe erase OK, but the post-erase read-back's CMD17 R1 is non-zero. */
  init_sdhc_ok();
  internal_queue_erase_blocks_ok(); /* probe erase [0,1) */
  mock_queue_idle(1U);
  mock_queue_idle((uint32_t)k_test_cmd_frame_bytes);
  mock_queue_byte((uint8_t)k_test_r1_illegal_cmd); /* CMD17 R1 bad */
  mock_queue_idle(1U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));

  /* Probe erase OK, read-back all-zero, but the remaining-range CMD32 fails. */
  init_sdhc_ok();
  internal_queue_erase_blocks_ok(); /* probe erase [0,1) */
  uint8_t zeros[k_ra8_sdmmc_spi_block_size] = {};
  queue_read_back(zeros);                                    /* read-back zero -> erase the rest */
  queue_command_response_r1((uint8_t)k_test_r1_illegal_cmd); /* second CMD32 bad                 */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_sdmmc_spi_erase_blocks(0U, 4U));
  TEST_END("erase_blocks error legs");
}
/**
 * @brief Consume host-test log bytes without touching target ITM MMIO.
 * @details Provides a no-op injected sink for expected-error test vectors.
 * @param[in] context Unused sink context.
 * @param[in] byte Unused byte emitted by the production logger.
 * @pre The test process owns the logger sink for the suite lifetime.
 * @pre No vector depends on observing diagnostic text.
 * @post No memory, descriptor, or hardware state is modified.
 * @post Control returns to the production logger immediately.
 * @note This keeps sanitizer runs away from the target-only ITM window.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_write_block_happy_path();
  internal_test_write_block_detects_write_error();
  internal_test_erase_blocks_verifies_zero();
  internal_test_write_block_rejects_uninit_and_oor();
  internal_test_write_block_command_errors();
  internal_test_write_block_per_byte_fallback();
  internal_test_write_blocks_arg_guards();
  internal_test_write_blocks_multi_happy();
  internal_test_write_blocks_cmd25_rejected();
  internal_test_erase_blocks_error_legs();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}

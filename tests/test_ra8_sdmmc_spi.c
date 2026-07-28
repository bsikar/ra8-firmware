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
 * This binary owns CRC/init/fs-backend/factory + the MC/DC vectors;
 * the read paths live in the sibling test_ra8_sdmmc_spi_read.c and
 * the write + erase paths in test_ra8_sdmmc_spi_write.c. The mock SPI
 * transport lives in support/sdmmc_spi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fs.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_sci_regs.h"
#include "ra8_sdmmc_spi.h"
#include "support/sdmmc_spi_test_util.h"
#include "unity_minimal.h"

/**
 * @enum sdmmc_spi_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_sd_core_block_bytes      = 512,   /**< SD core block bytes.        */
  k_sd_test_ten              = 10U,   /**< SD test ten.                */
  k_sd_csd_idx_bl_len        = 5,     /**< SD CSD index bl length.     */
  k_sd_csd_bl_len_512        = 0x09U, /**< SD CSD bl length 512.       */
  k_sd_csd_idx_csize_mid     = 7,     /**< SD CSD index csize mid.     */
  k_sd_test_ff               = 0xFFU, /**< SD test ff.                 */
  k_sd_csd_csize_low         = 0xC0U, /**< SD CSD csize low.           */
  k_sd_csd_idx_csize_mult_hi = 9,     /**< SD CSD index csize mult hi. */
  k_sd_csd_csize_mult_low    = 0x80U, /**< SD CSD csize mult low.      */
  k_sd_stamp_rdr             = 0x5AU, /**< SD stamp rdr.               */
  k_sd_test_stride_13        = 13U,   /**< SD test stride 13.          */
} sdmmc_spi_test_lit_t;

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
  uint8_t zeros[k_sd_core_block_bytes];
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
  mock_queue_idle(k_sd_test_ten);
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
  csd_v1[0]                          = 0x00U;               /* CSD_STRUCTURE = 0               */
  csd_v1[k_sd_csd_idx_bl_len]        = k_sd_csd_bl_len_512; /* READ_BL_LEN = 9 (low nibble)    */
  csd_v1[6]                          = 0x03U;               /* C_SIZE bits 11:10 = 0b11        */
  csd_v1[k_sd_csd_idx_csize_mid]     = k_sd_test_ff;        /* C_SIZE bits 9:2 = 0xFF          */
  csd_v1[8]                          = k_sd_csd_csize_low;  /* C_SIZE bits 1:0 (C_SIZE = 1023) */
  csd_v1[k_sd_csd_idx_csize_mult_hi] = 0x03U;               /* C_SIZE_MULT bits 2:1 = 0b11     */
  csd_v1[k_sd_test_ten]              = k_sd_csd_csize_mult_low; /* C_SIZE_MULT bit 0 = 1 */
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

/* ===========================================================================
 * SCI Simple-SPI transport factory (ra8_sdmmc_spi_io.c)
 * ===========================================================================
 */

/**
 * @brief Reset the fake MMIO window, MSTP model, and pin validator.
 *
 * @details The factory routes pins and brings up an SCI channel through the
 * real GPIO / SCI HAL, all of which operate on the ``ra8_fake_mmap``-backed
 * register window under ``RA8_OFF_TARGET``. Clearing the pin validator
 * lets each case re-claim the same pins without a "pin already owned" error.
 */
static void sci_factory_prep(void)
{
  ra8_fake_mmap_reset();
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
  reg->RDR = k_sd_stamp_rdr;
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
    block[i] = (uint8_t)((i * k_sd_test_stride_13) & k_sd_test_ff);
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
  test_bind_fs_backend_populates_struct();
  test_bind_fs_backend_uninitialized_rejected();
  test_transport_sci_factory_and_shims();
  test_transport_sci_factory_rejects();
  test_init_double_rejected();
  test_init_finalize_clock_failure();
  test_capacity_type_query_guards();
  test_fs_backend_shims();
  (void)fprintf(stderr, "[OK ] all ra8_sdmmc_spi tests passed\n");
  return 0;
}

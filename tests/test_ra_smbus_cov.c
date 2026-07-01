/**
 * @file test_ra_smbus_cov.c
 * @brief Supplemental coverage tests for ra_smbus -- targets the branches
 *        left uncovered by test_ra_smbus.c.
 *
 * @details
 * This file is an independent test executable (auto-discovered by the
 * GLOB in tests/CMakeLists.txt).  Its gcov .gcda records are merged with
 * those produced by test_ra_smbus.c when gcovr sweeps the build tree,
 * so the coverage figures for libs/ra_hal/src/ra_smbus.c reflect the
 * union of both binaries.
 *
 * Newly covered branches
 * ----------------------
 * - not-initialized early-return in receive_byte, write_byte_data,
 *   read_byte_data, block_write, block_read (lines 213, 242, 262, 296, 330)
 * - error-propagation return when the underlying ra_i3c call returns
 *   k_ra_err_busy (lines 219, 270, 343, 396); triggered by NOT calling
 *   prime_iic_b() so BCST.BFREF=0, making internal_i3c_i2c_busy_gate
 *   return k_ra_err_busy without any spin-polling.
 * - PEC-mismatch branch in ra_smbus_read_byte_data (lines 273-281)
 * - PEC-mismatch branch in ra_smbus_block_read (lines 354-366)
 * - count-exceeds-cap branch in ra_smbus_block_read (line 348)
 *
 * Hardware-unreachable lines (GCOVR_EXCL_LINE in source)
 * -------------------------------------------------------
 * - Line 163: ra_i3c_init failure path -- ra_i3c_init always succeeds
 *   in the host simulator because RSTCTL self-clears immediately.
 * - Line 229: closing brace of PEC block in receive_byte after a PEC
 *   match -- requires the bus to return the exact matching PEC byte,
 *   which the host simulator (fixed NTDTBP0 RAM) cannot produce.
 * - Line 283: same for read_byte_data.
 * - Line 368: same for block_read.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_i3c_i2c_regs.h"
#include "ra_err.h"
#include "ra_i3c_i2c.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_smbus.h"
#include "unity_minimal.h"

/* =============================================================================
 * Shared test constants
 * =============================================================================
 */

/**
 * @enum ra_smbus_cov_const_t
 * @brief Constants reused across coverage tests.
 */
typedef enum : uint8_t {
  k_cov_target = 0x40U, /**< 7-bit SMBus target address (PMBus-like). */
  k_cov_cmd    = 0x10U, /**< Arbitrary command / register byte.       */
} ra_smbus_cov_const_t;

/**
 * @var k_cfg_no_pec
 * @brief Channel-0 config with PEC disabled (same as sibling test).
 */
static const ra_smbus_cfg_t k_cfg_no_pec = {
  .channel     = 0U,
  .iic_cfg     = {.bus_hz = (uint32_t)k_ra_i3c_i2c_speed_fast, .pclka_hz = 60000000U},
  .pec_enabled = false,
};

/**
 * @var k_cfg_pec
 * @brief Channel-0 config with PEC enabled.
 */
static const ra_smbus_cfg_t k_cfg_pec = {
  .channel     = 0U,
  .iic_cfg     = {.bus_hz = (uint32_t)k_ra_i3c_i2c_speed_fast, .pclka_hz = 60000000U},
  .pec_enabled = true,
};

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Reset the simulator and initialize the SMBus layer.
 *
 * @details Mirrors the prep() helper in test_ra_smbus.c exactly.
 *
 * @param[in] cfg Configuration to apply.
 *
 * @pre cfg is non-NULL.
 * @post SMBus is initialized and ready for transfers.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void prep(const ra_smbus_cfg_t* cfg)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_smbus_deinit();
  TEST_ASSERT_EQ(k_ra_ok, ra_smbus_init(cfg));
}

/**
 * @brief Arm NTST and BCST so IIC_B polling loops exit immediately.
 *
 * @details Sets NTST.TDBEF0 and NTST.RDBFF0 (transfer-buffer ready
 * flags) and BCST.BFREF (bus-free flag) so the underlying transfer
 * helpers do not spin. Identical to prime_iic_b() in test_ra_smbus.c.
 *
 * @pre The simulator memory for channel 0 has been reset.
 * @post NTST holds both ready bits; BCST holds the free bit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void prime_iic_b(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->NTST = (uint32_t)k_ra_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra_i3c_i2c_msk_bcst_bfref;
}

/**
 * @brief Reset state so the driver reports not-initialized.
 *
 * @details Resets the simulator, initializes MSTP, then best-effort
 * deinits (so s_state.initialized becomes false) without calling
 * ra_smbus_init.  Every API call after this should return
 * k_ra_err_not_initialized.
 *
 * @pre None.
 * @post s_state.initialized is false.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void reset_to_uninit(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_smbus_deinit();
}

/* =============================================================================
 * Not-initialized early-return paths (lines 213, 242, 262, 296, 330)
 * =============================================================================
 */

/**
 * @brief Verify ra_smbus_receive_byte returns not-initialized.
 *
 * @details Covers source line 213: the ``if (!s_state.initialized)``
 * guard in ``ra_smbus_receive_byte``.
 *
 * @par MC/DC:
 * No compound boolean decisions touched by this test; the decision
 * ``!s_state.initialized`` is a single condition.
 *
 * @pre reset_to_uninit() has been called.
 * @post No state mutated beyond what reset_to_uninit left.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_byte_not_initialized(void)
{
  TEST_BEGIN("ra_smbus_receive_byte: not_initialized guard (line 213)");
  reset_to_uninit();
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_smbus_receive_byte((uint8_t)k_cov_target, &out));
  TEST_END("ra_smbus_receive_byte: not_initialized guard (line 213)");
}

/**
 * @brief Verify ra_smbus_write_byte_data returns not-initialized.
 *
 * @details Covers source line 242: the ``if (!s_state.initialized)``
 * guard in ``ra_smbus_write_byte_data``.
 *
 * @par MC/DC:
 * No compound boolean decisions; single condition.
 *
 * @pre reset_to_uninit() has been called.
 * @post No state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_write_byte_data_not_initialized(void)
{
  TEST_BEGIN("ra_smbus_write_byte_data: not_initialized guard (line 242)");
  reset_to_uninit();
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_smbus_write_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, 0xA5U));
  TEST_END("ra_smbus_write_byte_data: not_initialized guard (line 242)");
}

/**
 * @brief Verify ra_smbus_read_byte_data returns not-initialized.
 *
 * @details Covers source line 262: the ``if (!s_state.initialized)``
 * guard in ``ra_smbus_read_byte_data``.
 *
 * @par MC/DC:
 * No compound boolean decisions; single condition.
 *
 * @pre reset_to_uninit() has been called.
 * @post No state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_read_byte_data_not_initialized(void)
{
  TEST_BEGIN("ra_smbus_read_byte_data: not_initialized guard (line 262)");
  reset_to_uninit();
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  TEST_END("ra_smbus_read_byte_data: not_initialized guard (line 262)");
}

/**
 * @brief Verify ra_smbus_block_write returns not-initialized.
 *
 * @details Covers source line 296: the ``if (!s_state.initialized)``
 * guard in ``ra_smbus_block_write``.
 *
 * @par MC/DC:
 * No compound boolean decisions; single condition.
 *
 * @pre reset_to_uninit() has been called.
 * @post No state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_write_not_initialized(void)
{
  TEST_BEGIN("ra_smbus_block_write: not_initialized guard (line 296)");
  reset_to_uninit();
  const uint8_t payload[1] = {0xA5U};
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_smbus_block_write((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, payload, 1U));
  TEST_END("ra_smbus_block_write: not_initialized guard (line 296)");
}

/**
 * @brief Verify ra_smbus_block_read returns not-initialized.
 *
 * @details Covers source line 330: the ``if (!s_state.initialized)``
 * guard in ``ra_smbus_block_read``.
 *
 * @par MC/DC:
 * No compound boolean decisions; single condition.
 *
 * @pre reset_to_uninit() has been called.
 * @post No state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_read_not_initialized(void)
{
  TEST_BEGIN("ra_smbus_block_read: not_initialized guard (line 330)");
  reset_to_uninit();
  uint8_t buf[4] = {0U, 0U, 0U, 0U};
  uint8_t got    = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 4U, &got));
  TEST_END("ra_smbus_block_read: not_initialized guard (line 330)");
}

/* =============================================================================
 * Error-propagation paths (lines 219, 270, 343, 396)
 *
 * Strategy: call prep() (which resets simulator registers to zero) but
 * do NOT call prime_iic_b().  With BCST.BFREF=0 the bus-busy gate
 * internal_i3c_i2c_busy_gate() returns k_ra_err_busy immediately
 * (no spin-loop), and that error propagates up through the SMBus layer.
 * =============================================================================
 */

/**
 * @brief Verify receive_byte propagates ra_i3c_read errors.
 *
 * @details Covers source line 219: ``return err`` after a failed
 * ``ra_i3c_read`` inside ``ra_smbus_receive_byte``.  With BCST.BFREF
 * clear the underlying IIC_B controller returns k_ra_err_busy without
 * spinning.
 *
 * @par MC/DC:
 * Decision ``if (err != k_ra_ok)`` is a single condition; no compound
 * boolean is newly exercised.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post The bus remains in the busy-idle state; no bytes were sent.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_byte_xfer_error(void)
{
  TEST_BEGIN("ra_smbus_receive_byte: propagates ra_i3c_read error (line 219)");
  /* prep resets registers (BCST.BFREF=0); do not prime. */
  prep(&k_cfg_no_pec);
  /* BCST.BFREF is 0 after sim reset -> busy gate fires immediately. */
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra_err_busy, ra_smbus_receive_byte((uint8_t)k_cov_target, &out));
  TEST_END("ra_smbus_receive_byte: propagates ra_i3c_read error (line 219)");
}

/**
 * @brief Verify read_byte_data propagates ra_i3c_transfer errors.
 *
 * @details Covers source line 270: ``return err`` after a failed
 * ``ra_i3c_transfer`` inside ``ra_smbus_read_byte_data``.
 *
 * @par MC/DC:
 * Single condition ``if (err != k_ra_ok)``; no compound boolean.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post No bytes transmitted; state unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_read_byte_data_xfer_error(void)
{
  TEST_BEGIN("ra_smbus_read_byte_data: propagates ra_i3c_transfer error (line 270)");
  prep(&k_cfg_no_pec);
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra_err_busy,
                 ra_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  TEST_END("ra_smbus_read_byte_data: propagates ra_i3c_transfer error (line 270)");
}

/**
 * @brief Verify block_read propagates ra_i3c_transfer errors.
 *
 * @details Covers source line 343: ``return err`` after a failed
 * ``ra_i3c_transfer`` inside ``ra_smbus_block_read``.
 *
 * @par MC/DC:
 * Single condition; no compound boolean.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post No bytes received; state unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_read_xfer_error(void)
{
  TEST_BEGIN("ra_smbus_block_read: propagates ra_i3c_transfer error (line 343)");
  prep(&k_cfg_no_pec);
  uint8_t buf[4] = {0U, 0U, 0U, 0U};
  uint8_t got    = 0U;
  TEST_ASSERT_EQ(k_ra_err_busy,
                 ra_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 4U, &got));
  TEST_END("ra_smbus_block_read: propagates ra_i3c_transfer error (line 343)");
}

/**
 * @brief Verify alert_dispatch propagates ra_i3c_read errors.
 *
 * @details Covers source line 396: ``return err`` after a failed
 * ``ra_i3c_read`` inside ``ra_smbus_alert_dispatch``.
 *
 * @par MC/DC:
 * Single condition; no compound boolean.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post No ARA read attempted; callback not fired.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_alert_dispatch_xfer_error(void)
{
  TEST_BEGIN("ra_smbus_alert_dispatch: propagates ra_i3c_read error (line 396)");
  prep(&k_cfg_no_pec);
  TEST_ASSERT_EQ(k_ra_err_busy, ra_smbus_alert_dispatch());
  TEST_END("ra_smbus_alert_dispatch: propagates ra_i3c_read error (line 396)");
}

/* =============================================================================
 * PEC mismatch paths (lines 273-281, 354-366)
 * =============================================================================
 */

/**
 * @brief Verify PEC mismatch detection in ra_smbus_read_byte_data.
 *
 * @details Covers source lines 273-281: the PEC computation block
 * inside ``ra_smbus_read_byte_data`` when ``pec_enabled`` is true.
 *
 * With prime_iic_b() armed, ``ra_i3c_transfer`` succeeds.  The
 * simulator always returns the same value from NTDTBP0 for every read
 * (after the write phase NTDTBP0 holds the read-address byte 0x81),
 * so ``rx[0] == rx[1] == 0x81``.  The computed PEC over the sequence
 * (addr_w, cmd, addr_r, rx[0]) is not 0x81, so the check fails and
 * the function returns k_ra_err_crc_mismatch.
 *
 * @par MC/DC:
 * Decision ``if (pec != rx[1])`` is a single condition; no compound
 * boolean.
 *
 * @pre prep() with PEC config; prime_iic_b() called before transfer.
 * @post Returns k_ra_err_crc_mismatch; out_data not written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_read_byte_data_pec_mismatch(void)
{
  TEST_BEGIN("ra_smbus_read_byte_data: PEC mismatch path (lines 273-281)");
  prep(&k_cfg_pec);
  prime_iic_b();
  uint8_t out = 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_crc_mismatch,
                 ra_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  /* out_data must NOT be written on PEC error. */
  TEST_ASSERT_EQ(0xFF, out);
  TEST_END("ra_smbus_read_byte_data: PEC mismatch path (lines 273-281)");
}

/**
 * @brief Verify PEC mismatch detection in ra_smbus_block_read.
 *
 * @details Covers source lines 354-366: the PEC computation block
 * inside ``ra_smbus_block_read`` when ``pec_enabled`` is true.
 *
 * After prime_iic_b() the write phase writes the read-address byte
 * (0x81 = (0x40<<1)|1 = 129) into NTDTBP0, so all bytes drained from
 * NTDTBP0 read as 0x81.  With cap=255 the transfer reads 257 bytes
 * (cap+1+1 for PEC): rx[0]=count=129, rx[1..129]=payload=0x81 each,
 * rx[130]=PEC_from_bus=0x81.  The locally computed PEC over the
 * sequence (addr_w, cmd, addr_r, count, payload[0..128]) is not 0x81,
 * so the check fails.
 *
 * @par MC/DC:
 * Decision ``if (pec != rx[1U + count])`` is a single condition; no
 * compound boolean.
 *
 * @pre prep() with PEC config; prime_iic_b() called before transfer.
 * @post Returns k_ra_err_crc_mismatch.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_read_pec_mismatch(void)
{
  TEST_BEGIN("ra_smbus_block_read: PEC mismatch path (lines 354-366)");
  prep(&k_cfg_pec);
  prime_iic_b();
  /* Use cap=255 so the count from the bus (129 = 0x81) fits. */
  uint8_t buf[256] = {0U};
  uint8_t out_len  = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_crc_mismatch,
    ra_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 255U, &out_len));
  TEST_END("ra_smbus_block_read: PEC mismatch path (lines 354-366)");
}

/* =============================================================================
 * Count-overflow path (line 348)
 * =============================================================================
 */

/**
 * @brief Verify ra_smbus_block_read detects count > cap.
 *
 * @details Covers source line 348: ``return k_ra_err_invalid_size``
 * when the peripheral's returned byte-count exceeds the caller's
 * buffer capacity.
 *
 * After the write+read-address sequence the simulator leaves the
 * read-address byte (0x81 = 129) latched in NTDTBP0.  With cap=1 the
 * driver reads want=2 bytes; rx[0]=0x81=129 > 1=cap, so it writes
 * out_len=129 and returns k_ra_err_invalid_size.
 *
 * @par MC/DC:
 * Decision ``if (count > cap)`` is a single condition; no compound
 * boolean.
 *
 * @pre prep(); prime_iic_b() arms the transfer registers.
 * @post out_len set to the bus-reported count (129 = 0x81) even on
 *       error, per the API contract in the header.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_read_count_overflow(void)
{
  TEST_BEGIN("ra_smbus_block_read: count > cap returns invalid_size (line 348)");
  prep(&k_cfg_no_pec);
  prime_iic_b();
  /* After the transfer, NTDTBP0 = read-addr byte = (0x40<<1)|1 = 0x81 = 129.
   * With cap=1, count=129 > 1=cap triggers k_ra_err_invalid_size. */
  uint8_t buf     = 0U;
  uint8_t out_len = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &buf, 1U, &out_len));
  /* out_len must be populated even on error (API contract). */
  TEST_ASSERT_EQ(0x81, out_len);
  TEST_END("ra_smbus_block_read: count > cap returns invalid_size (line 348)");
}

/* =============================================================================
 * main
 * =============================================================================
 */

/**
 * @brief Run all supplemental coverage tests.
 *
 * @return 0 on success; exits via abort() on first failure.
 *
 * @pre Host-side simulator is available (RA_SIMULATOR_MODE).
 * @post All listed coverage branches have been exercised.
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  /* not-initialized guards */
  test_receive_byte_not_initialized();
  test_write_byte_data_not_initialized();
  test_read_byte_data_not_initialized();
  test_block_write_not_initialized();
  test_block_read_not_initialized();

  /* error-propagation returns */
  test_receive_byte_xfer_error();
  test_read_byte_data_xfer_error();
  test_block_read_xfer_error();
  test_alert_dispatch_xfer_error();

  /* PEC mismatch blocks */
  test_read_byte_data_pec_mismatch();
  test_block_read_pec_mismatch();

  /* count-overflow path */
  test_block_read_count_overflow();

  (void)fprintf(stderr, "[OK ] test_ra_smbus_cov.c\n");
  return 0;
}

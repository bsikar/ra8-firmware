/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_smbus_cov.c
 * @brief Supplemental coverage tests for ra8_smbus -- targets the branches
 *        left uncovered by test_ra8_smbus.c.
 *
 * @details
 * This file is an independent test executable (auto-discovered by the
 * GLOB in tests/CMakeLists.txt).  Its gcov .gcda records are merged with
 * those produced by test_ra8_smbus.c when gcovr sweeps the build tree,
 * so the coverage figures for libs/ra8_hal/src/ra8_smbus.c reflect the
 * union of both binaries.
 *
 * Newly covered branches
 * ----------------------
 * - not-initialized early-return in receive_byte, write_byte_data,
 *   read_byte_data, block_write, block_read (lines 213, 242, 262, 296, 330)
 * - error-propagation return when the bound i3c-compat backend returns
 *   k_ra8_err_busy (lines 219, 270, 343, 396); triggered by NOT calling
 *   prime_iic_b() so BCST.BFREF=0, making internal_i3c_i2c_busy_gate
 *   return k_ra8_err_busy without any spin-polling.
 * - PEC-mismatch branch in ra8_smbus_read_byte_data (lines 273-281)
 * - PEC-mismatch branch in ra8_smbus_block_read (lines 354-366)
 * - count-exceeds-cap branch in ra8_smbus_block_read (line 348)
 *
 * Hardware-unreachable lines (GCOVR_EXCL_LINE in source)
 * -------------------------------------------------------
 * - Line 229: closing brace of PEC block in receive_byte after a PEC
 *   match -- requires the bus to return the exact matching PEC byte,
 *   which the host fake (fixed NTDTBP0 RAM) cannot produce.
 * - Line 283: same for read_byte_data.
 * - Line 368: same for block_read.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_mstp.h"
#include "ra8_smbus.h"
#include "unity_minimal.h"

/**
 * @enum t_smbus_cov_t
 * @brief Out-parameter seed and the block-read buffer capacity.
 */
typedef enum : uint16_t {
  k_t_byte_unset = 0xFFU, /**< Pre-set byte out-parameter; a transfer that fails
                               must leave it rather than report data.            */
  k_t_block_cap  = 256U,  /**< Block-read destination buffer, bytes. */
} t_smbus_cov_t;

/* =============================================================================
 * Shared test constants
 * =============================================================================
 */

/**
 * @enum ra8_smbus_cov_const_t
 * @brief Constants reused across coverage tests.
 */
typedef enum : uint8_t {
  k_cov_target = 0x40U, /**< 7-bit SMBus target address (PMBus-like). */
  k_cov_cmd    = 0x10U, /**< Arbitrary command / register byte.       */
} ra8_smbus_cov_const_t;

/** @brief Bound I2C bus handle the injected seam's ctx points at. */
static ra8_io_i2c_bus_t s_bus;

/**
 * @brief Play the app's role: initialise IIC_B channel 0 in I2C-compat
 *        mode and bind it through the ra8_io facade into a fresh seam.
 *
 * @details Must re-run after every ``ra8_fake_mmap_reset`` because the
 * reset wipes the registers ``ra8_i3c_init`` programmed. Mirrors the
 * helper in test_ra8_smbus.c exactly.
 *
 * @param[out] out Seam to fill for the SMBus cfg.
 *
 * @pre The fake window is mapped and MSTP is initialised.
 * @pre ``out`` is writable.
 * @post ``*out`` forwards into IIC_B channel 0 via the bound facade.
 * @post ::s_bus is bound to the I3C I2C-compat backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void bind_bus(ra8_i2c_bus_ops_t* out)
{
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_ra8_i3c_i2c_speed_fast,
    .pclka_hz = 60000000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &iic_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_i3c_compat(&s_bus, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_as_ops(&s_bus, out));
}

/**
 * @brief Build an SMBus cfg over a freshly-bound seam.
 *
 * @details Wraps ::bind_bus so each test gets a live seam after its
 * fake reset.
 *
 * @param[in] pec_enabled PEC policy for the returned cfg.
 *
 * @return Config carrying the bound seam and the PEC flag.
 *
 * @pre The fake window is mapped and MSTP is initialised.
 * @pre ::s_bus is safe to (re)bind.
 * @post The returned cfg's seam forwards into IIC_B channel 0.
 * @post ::s_bus is bound.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_smbus_cfg_t make_cfg(bool pec_enabled)
{
  ra8_smbus_cfg_t cfg = {.bus = {}, .pec_enabled = pec_enabled};
  bind_bus(&cfg.bus);
  return cfg;
}

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Reset the fake and initialize the SMBus layer.
 *
 * @details Mirrors the prep() helper in test_ra8_smbus.c exactly.
 *
 * @param[in] pec_enabled Whether the built configuration enables PEC.
 *
 * @pre The SMBus mock MMIO window is mapped.
 * @post SMBus is initialized and ready for transfers.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void prep_pec(bool pec_enabled)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  const ra8_smbus_cfg_t cfg = make_cfg(pec_enabled);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_init(&cfg));
}

/**
 * @brief Arm NTST and BCST so IIC_B polling loops exit immediately.
 *
 * @details Sets NTST.TDBEF0 and NTST.RDBFF0 (transfer-buffer ready
 * flags) and BCST.BFREF (bus-free flag) so the underlying transfer
 * helpers do not spin. Identical to prime_iic_b() in test_ra8_smbus.c.
 *
 * @pre The fake memory for channel 0 has been reset.
 * @post NTST holds both ready bits; BCST holds the free bit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void prime_iic_b(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/**
 * @brief Reset state so the driver reports not-initialized.
 *
 * @details Resets the fake, initializes MSTP, then best-effort
 * deinits (so s_state.initialized becomes false) without calling
 * ra8_smbus_init.  Every API call after this should return
 * k_ra8_err_not_initialized.
 *
 * @pre None.
 * @post s_state.initialized is false.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void reset_to_uninit(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
}

/* =============================================================================
 * Not-initialized early-return paths (lines 213, 242, 262, 296, 330)
 * =============================================================================
 */

/**
 * @brief Verify ra8_smbus_receive_byte returns not-initialized.
 *
 * @details Covers source line 213: the ``if (!s_state.initialized)``
 * guard in ``ra8_smbus_receive_byte``.
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
  TEST_BEGIN("ra8_smbus_receive_byte: not_initialized guard (line 213)");
  reset_to_uninit();
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_smbus_receive_byte((uint8_t)k_cov_target, &out));
  TEST_END("ra8_smbus_receive_byte: not_initialized guard (line 213)");
}

/**
 * @brief Verify ra8_smbus_write_byte_data returns not-initialized.
 *
 * @details Covers source line 242: the ``if (!s_state.initialized)``
 * guard in ``ra8_smbus_write_byte_data``.
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
  TEST_BEGIN("ra8_smbus_write_byte_data: not_initialized guard (line 242)");
  reset_to_uninit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_write_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, 0xA5U));
  TEST_END("ra8_smbus_write_byte_data: not_initialized guard (line 242)");
}

/**
 * @brief Verify ra8_smbus_read_byte_data returns not-initialized.
 *
 * @details Covers source line 262: the ``if (!s_state.initialized)``
 * guard in ``ra8_smbus_read_byte_data``.
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
  TEST_BEGIN("ra8_smbus_read_byte_data: not_initialized guard (line 262)");
  reset_to_uninit();
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  TEST_END("ra8_smbus_read_byte_data: not_initialized guard (line 262)");
}

/**
 * @brief Verify ra8_smbus_block_write returns not-initialized.
 *
 * @details Covers source line 296: the ``if (!s_state.initialized)``
 * guard in ``ra8_smbus_block_write``.
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
  TEST_BEGIN("ra8_smbus_block_write: not_initialized guard (line 296)");
  reset_to_uninit();
  const uint8_t payload[1] = {0xA5U};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_block_write((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, payload, 1U));
  TEST_END("ra8_smbus_block_write: not_initialized guard (line 296)");
}

/**
 * @brief Verify ra8_smbus_block_read returns not-initialized.
 *
 * @details Covers source line 330: the ``if (!s_state.initialized)``
 * guard in ``ra8_smbus_block_read``.
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
  TEST_BEGIN("ra8_smbus_block_read: not_initialized guard (line 330)");
  reset_to_uninit();
  uint8_t buf[4] = {0U, 0U, 0U, 0U};
  uint8_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 4U, &got));
  TEST_END("ra8_smbus_block_read: not_initialized guard (line 330)");
}

/* =============================================================================
 * Error-propagation paths (lines 219, 270, 343, 396)
 *
 * Strategy: call prep() (which resets fake registers to zero) but
 * do NOT call prime_iic_b().  With BCST.BFREF=0 the bus-busy gate
 * internal_i3c_i2c_busy_gate() returns k_ra8_err_busy immediately
 * (no spin-loop), and that error propagates up through the SMBus layer.
 * =============================================================================
 */

/**
 * @brief Verify receive_byte propagates ra8_i3c_read errors.
 *
 * @details Covers source line 219: ``return err`` after a failed
 * ``ra8_i3c_read`` inside ``ra8_smbus_receive_byte``.  With BCST.BFREF
 * clear the underlying IIC_B controller returns k_ra8_err_busy without
 * spinning.
 *
 * @par MC/DC:
 * Decision ``if (err != k_ra8_ok)`` is a single condition; no compound
 * boolean is newly exercised.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post The bus remains in the busy-idle state; no bytes were sent.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_byte_xfer_error(void)
{
  TEST_BEGIN("ra8_smbus_receive_byte: propagates ra8_i3c_read error (line 219)");
  /* prep resets registers (BCST.BFREF=0); do not prime. */
  prep_pec(false);
  /* BCST.BFREF is 0 after fake reset -> busy gate fires immediately. */
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_smbus_receive_byte((uint8_t)k_cov_target, &out));
  TEST_END("ra8_smbus_receive_byte: propagates ra8_i3c_read error (line 219)");
}

/**
 * @brief Verify read_byte_data propagates ra8_i3c_transfer errors.
 *
 * @details Covers source line 270: ``return err`` after a failed
 * ``ra8_i3c_transfer`` inside ``ra8_smbus_read_byte_data``.
 *
 * @par MC/DC:
 * Single condition ``if (err != k_ra8_ok)``; no compound boolean.
 *
 * @pre prep() initializes the layer; prime_iic_b() is NOT called.
 * @post No bytes transmitted; state unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_read_byte_data_xfer_error(void)
{
  TEST_BEGIN("ra8_smbus_read_byte_data: propagates ra8_i3c_transfer error (line 270)");
  prep_pec(false);
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  TEST_END("ra8_smbus_read_byte_data: propagates ra8_i3c_transfer error (line 270)");
}

/**
 * @brief Verify block_read propagates ra8_i3c_transfer errors.
 *
 * @details Covers source line 343: ``return err`` after a failed
 * ``ra8_i3c_transfer`` inside ``ra8_smbus_block_read``.
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
  TEST_BEGIN("ra8_smbus_block_read: propagates ra8_i3c_transfer error (line 343)");
  prep_pec(false);
  uint8_t buf[4] = {0U, 0U, 0U, 0U};
  uint8_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 4U, &got));
  TEST_END("ra8_smbus_block_read: propagates ra8_i3c_transfer error (line 343)");
}

/**
 * @brief Verify alert_dispatch propagates ra8_i3c_read errors.
 *
 * @details Covers source line 396: ``return err`` after a failed
 * ``ra8_i3c_read`` inside ``ra8_smbus_alert_dispatch``.
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
  TEST_BEGIN("ra8_smbus_alert_dispatch: propagates ra8_i3c_read error (line 396)");
  prep_pec(false);
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_smbus_alert_dispatch());
  TEST_END("ra8_smbus_alert_dispatch: propagates ra8_i3c_read error (line 396)");
}

/* =============================================================================
 * PEC mismatch paths (lines 273-281, 354-366)
 * =============================================================================
 */

/**
 * @brief Verify PEC mismatch detection in ra8_smbus_read_byte_data.
 *
 * @details Covers source lines 273-281: the PEC computation block
 * inside ``ra8_smbus_read_byte_data`` when ``pec_enabled`` is true.
 *
 * With prime_iic_b() armed, ``ra8_i3c_transfer`` succeeds.  The
 * fake always returns the same value from NTDTBP0 for every read
 * (after the write phase NTDTBP0 holds the read-address byte 0x81),
 * so ``rx[0] == rx[1] == 0x81``.  The computed PEC over the sequence
 * (addr_w, cmd, addr_r, rx[0]) is not 0x81, so the check fails and
 * the function returns k_ra8_err_crc_mismatch.
 *
 * @par MC/DC:
 * Decision ``if (pec != rx[1])`` is a single condition; no compound
 * boolean.
 *
 * @pre prep() with PEC config; prime_iic_b() called before transfer.
 * @post Returns k_ra8_err_crc_mismatch; out_data not written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_read_byte_data_pec_mismatch(void)
{
  TEST_BEGIN("ra8_smbus_read_byte_data: PEC mismatch path (lines 273-281)");
  prep_pec(true);
  prime_iic_b();
  uint8_t out = k_t_byte_unset;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_smbus_read_byte_data((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &out));
  /* out_data must NOT be written on PEC error. */
  TEST_ASSERT_EQ(0xFF, out);
  TEST_END("ra8_smbus_read_byte_data: PEC mismatch path (lines 273-281)");
}

/**
 * @brief Verify PEC mismatch detection in ra8_smbus_block_read.
 *
 * @details Covers source lines 354-366: the PEC computation block
 * inside ``ra8_smbus_block_read`` when ``pec_enabled`` is true.
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
 * @post Returns k_ra8_err_crc_mismatch.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_block_read_pec_mismatch(void)
{
  TEST_BEGIN("ra8_smbus_block_read: PEC mismatch path (lines 354-366)");
  prep_pec(true);
  prime_iic_b();
  /* Use cap=255 so the count from the bus (129 = 0x81) fits. */
  uint8_t buf[k_t_block_cap] = {0U};
  uint8_t out_len            = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_crc_mismatch,
    ra8_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, buf, 255U, &out_len));
  TEST_END("ra8_smbus_block_read: PEC mismatch path (lines 354-366)");
}

/* =============================================================================
 * Count-overflow path (line 348)
 * =============================================================================
 */

/**
 * @brief Verify ra8_smbus_block_read detects count > cap.
 *
 * @details Covers source line 348: ``return k_ra8_err_invalid_size``
 * when the peripheral's returned byte-count exceeds the caller's
 * buffer capacity.
 *
 * After the write+read-address sequence the fake leaves the
 * read-address byte (0x81 = 129) latched in NTDTBP0.  With cap=1 the
 * driver reads want=2 bytes; rx[0]=0x81=129 > 1=cap, so it writes
 * out_len=129 and returns k_ra8_err_invalid_size.
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
  TEST_BEGIN("ra8_smbus_block_read: count > cap returns invalid_size (line 348)");
  prep_pec(false);
  prime_iic_b();
  /* After the transfer, NTDTBP0 = read-addr byte = (0x40<<1)|1 = 0x81 = 129.
   * With cap=1, count=129 > 1=cap triggers k_ra8_err_invalid_size. */
  uint8_t buf     = 0U;
  uint8_t out_len = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_smbus_block_read((uint8_t)k_cov_target, (uint8_t)k_cov_cmd, &buf, 1U, &out_len));
  /* out_len must be populated even on error (API contract). */
  TEST_ASSERT_EQ(0x81, out_len);
  TEST_END("ra8_smbus_block_read: count > cap returns invalid_size (line 348)");
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
 * @pre Host-side fake is available (RA8_OFF_TARGET).
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

  (void)fprintf(stderr, "[OK ] test_ra8_smbus_cov.c\n");
  return 0;
}

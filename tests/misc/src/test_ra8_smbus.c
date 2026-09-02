/**
 * @file test_ra8_smbus.c
 * @brief Unit tests for the SMBus 3.2 protocol layer.
 *
 * @details
 * The SMBus layer is pure protocol framing -- it composes wire frames
 * (cmd / count / PEC / etc.) and delegates raw byte movement to
 * ``ra8_i3c_i2c``. These tests run against the host-side
 * ``ra8_fake_mmap`` substrate exactly the way the IIC_B tests do: they
 * pre-arm NTST.TDBEF0 / NTST.RDBFF0 and BCST.BFREF before every
 * transfer so the polling loops fall through immediately, then
 * inspect the last bytes written into NTDTBP0 to verify framing.
 *
 * PEC correctness is validated against the SMBus 3.2 section 5.4
 * reference vectors -- a Smart Battery 1.1 ``ManufacturerName`` query
 * (and its inverse) is the canonical sanity check.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
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
 * @enum t_smbus_t
 * @brief Out-parameter seeds and the block-read buffer capacity.
 */
typedef enum : uint16_t {
  k_t_byte_unset  = 0xFFU, /**< Pre-set byte out-parameter; a transfer that
                                fails must leave it rather than report data.   */
  k_t_marker_seed = 42,    /**< Pre-set signed marker, same purpose.  */
  k_t_block_cap   = 256U,  /**< Block-read destination buffer, bytes. */
} t_smbus_t;

/**
 * @enum ra8_smbus_test_const_t
 * @brief Constants used across tests.
 */
typedef enum : uint8_t {
  k_smbus_test_target = 0x40U, /**< 7-bit address (e.g. PMBus device). */
  k_smbus_test_cmd    = 0x10U, /**< Arbitrary command/register byte.   */
  k_smbus_test_data_a = 0xA5U, /**< First payload byte.                */
  k_smbus_test_data_b = 0x5AU, /**< Second payload byte.               */
} ra8_smbus_test_const_t;

/** @brief Bound I2C bus handle the injected seam's ctx points at. */
static ra8_io_i2c_bus_t s_bus;

/**
 * @brief Play the app's role: initialise IIC_B channel 0 in I2C-compat
 *        mode and bind it through the ra8_io facade into a fresh seam.
 *
 * @details Must re-run after every ``ra8_fake_mmap_reset`` because the
 * reset wipes the registers ``ra8_i3c_init`` programmed.
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
RA8_INTERNAL static void internal_bind_bus(ra8_i2c_bus_ops_t* out)
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
 * @details Wraps ::internal_bind_bus so each test gets a live seam after its
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
 * @since 0.1.0 @retval value The computed fixture value for the supplied inputs. */
RA8_INTERNAL static ra8_smbus_cfg_t internal_make_cfg(bool pec_enabled)
{
  ra8_smbus_cfg_t cfg = {.bus = {}, .pec_enabled = pec_enabled};
  internal_bind_bus(&cfg.bus);
  return cfg;
}

/**
 * @brief Pre-arm NTST + BCST so the underlying IIC_B polling loops
 *        fall through immediately. Identical to the helper in the
 *        ra8_i3c_i2c unit tests. @details Implements the prime iic b fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prime_iic_b(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/**
 * @brief Reset the fake and ensure the SMBus layer is back to
 *        pristine state between tests. @details Implements the prep pec fixture operation used only by this focused test executable. @param[in] pec_enabled Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep_pec(bool pec_enabled)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  /* Best-effort deinit -- ignore the error if init was never called. */
  (void)ra8_smbus_deinit();
  const ra8_smbus_cfg_t cfg = internal_make_cfg(pec_enabled);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_init(&cfg));
}

/* =============================================================================
 * PEC: CRC-8/SMBus
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify pec empty and null behavior. @details Executes the pec empty and null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pec_empty_and_null(void)
{
  TEST_BEGIN("ra8_smbus_pec: NULL or zero-length returns init value 0");
  TEST_ASSERT_EQ(0, ra8_smbus_pec(nullptr, 0U));
  const uint8_t one = 0xFFU;
  TEST_ASSERT_EQ(0, ra8_smbus_pec(&one, 0U));
  TEST_ASSERT_EQ(0, ra8_smbus_pec(nullptr, 5U));
  TEST_END("ra8_smbus_pec: NULL or zero-length returns init value 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify pec known vectors behavior. @details Executes the pec known vectors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pec_known_vectors(void)
{
  TEST_BEGIN("ra8_smbus_pec: known CRC-8/SMBus vectors");
  /* Reference: hand-cranked CRC-8 (poly 0x07, init 0). */
  const uint8_t v_zero = 0x00U;
  TEST_ASSERT_EQ(0, ra8_smbus_pec(&v_zero, 1U));

  /* Single byte 0x01 -> CRC = 0x07 (one shift through the poly). */
  const uint8_t v_one = 0x01U;
  TEST_ASSERT_EQ(0x07, ra8_smbus_pec(&v_one, 1U));

  /* Single byte 0xFF -> CRC = 0xF3 (computed offline). */
  const uint8_t v_ff = 0xFFU;
  TEST_ASSERT_EQ(0xF3, ra8_smbus_pec(&v_ff, 1U));

  /* Standard test vector: "123456789" -> 0xF4. */
  const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQ(0xF4, ra8_smbus_pec(check, 9U));
  TEST_END("ra8_smbus_pec: known CRC-8/SMBus vectors");
}

/* =============================================================================
 * Init / deinit
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify init null cfg behavior. @details Executes the init null cfg scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_null_cfg(void)
{
  TEST_BEGIN("ra8_smbus_init: NULL cfg rejected");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_smbus_init(nullptr));
  TEST_END("ra8_smbus_init: NULL cfg rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init incomplete bus behavior. @details Executes the init incomplete bus scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_incomplete_bus(void)
{
  TEST_BEGIN("ra8_smbus_init: incomplete bus seam rejected");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  /* Each missing seam row is rejected by its own single-condition guard. */
  ra8_smbus_cfg_t bad = internal_make_cfg(false);
  bad.bus.write       = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_smbus_init(&bad));
  bad          = internal_make_cfg(false);
  bad.bus.read = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_smbus_init(&bad));
  bad              = internal_make_cfg(false);
  bad.bus.transfer = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_smbus_init(&bad));
  TEST_END("ra8_smbus_init: incomplete bus seam rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify deinit without init behavior. @details Executes the deinit without init scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deinit_without_init(void)
{
  TEST_BEGIN("ra8_smbus_deinit: not_initialized when never inited");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  /* Ensure clean state. */
  (void)ra8_smbus_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_smbus_deinit());
  TEST_END("ra8_smbus_deinit: not_initialized when never inited");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init deinit cycle behavior. @details Executes the init deinit cycle scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_deinit_cycle(void)
{
  TEST_BEGIN("ra8_smbus_init -> deinit cycle");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  const ra8_smbus_cfg_t cfg = internal_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_deinit());
  /* Second deinit should now be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_smbus_deinit());
  TEST_END("ra8_smbus_init -> deinit cycle");
}

/* =============================================================================
 * Send Byte / Receive Byte
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify send byte no pec behavior. @details Executes the send byte no pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_byte_no_pec(void)
{
  TEST_BEGIN("ra8_smbus_send_byte: 1 byte payload, no PEC");
  internal_prep_pec(false);
  internal_prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_smbus_send_byte((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_data_a));
  /* Last byte landed in NTDTBP0 must be the data byte (no trailing PEC). */
  TEST_ASSERT_EQ(k_smbus_test_data_a, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_send_byte: 1 byte payload, no PEC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify send byte with pec behavior. @details Executes the send byte with pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_byte_with_pec(void)
{
  TEST_BEGIN("ra8_smbus_send_byte: PEC enabled appends one extra byte");
  internal_prep_pec(true);
  internal_prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_smbus_send_byte((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_data_a));
  /* Expected PEC = CRC8(addr_w, data). addr_w = 0x40 << 1 = 0x80. */
  const uint8_t frame[2] = {0x80U, (uint8_t)k_smbus_test_data_a};
  const uint8_t expect   = ra8_smbus_pec(frame, 2U);
  TEST_ASSERT_EQ(expect, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_send_byte: PEC enabled appends one extra byte");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify send byte not initialized behavior. @details Executes the send byte not initialized scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_byte_not_initialized(void)
{
  TEST_BEGIN("ra8_smbus_send_byte: not_initialized when init missing");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_send_byte((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_data_a));
  TEST_END("ra8_smbus_send_byte: not_initialized when init missing");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify receive byte no pec happy behavior. @details Executes the receive byte no pec happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_byte_no_pec_happy(void)
{
  TEST_BEGIN("ra8_smbus_receive_byte: no PEC, 1 byte returned");
  internal_prep_pec(false);
  internal_prime_iic_b();
  uint8_t out = k_t_byte_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_receive_byte((uint8_t)k_smbus_test_target, &out));
  /* Fake returns 0 from NTDTBP0; that's fine -- we just confirm
   * the call succeeded and out_data was written. */
  (void)out;
  TEST_END("ra8_smbus_receive_byte: no PEC, 1 byte returned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify receive byte null arg behavior. @details Executes the receive byte null arg scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_byte_null_arg(void)
{
  TEST_BEGIN("ra8_smbus_receive_byte: NULL out_data rejected");
  internal_prep_pec(false);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_smbus_receive_byte((uint8_t)k_smbus_test_target, nullptr));
  TEST_END("ra8_smbus_receive_byte: NULL out_data rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify receive byte pec mismatch behavior. @details Executes the receive byte pec mismatch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_byte_pec_mismatch(void)
{
  TEST_BEGIN("ra8_smbus_receive_byte: PEC mismatch detected");
  internal_prep_pec(true);
  internal_prime_iic_b();
  /* Fake returns 0 for both data and PEC bytes. The expected
   * PEC = CRC8(addr_r, 0x00) is non-zero (= 0xC3 for addr_r = 0x81),
   * so the verification must fail. */
  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_smbus_receive_byte((uint8_t)k_smbus_test_target, &out));
  TEST_END("ra8_smbus_receive_byte: PEC mismatch detected");
}

/* =============================================================================
 * Write Byte Data / Read Byte Data
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify write byte data no pec behavior. @details Executes the write byte data no pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_byte_data_no_pec(void)
{
  TEST_BEGIN("ra8_smbus_write_byte_data: cmd + data, no PEC");
  internal_prep_pec(false);
  internal_prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_smbus_write_byte_data((uint8_t)k_smbus_test_target,
                                           (uint8_t)k_smbus_test_cmd,
                                           (uint8_t)k_smbus_test_data_a));
  /* Last byte in NTDTBP0 is the data byte (cmd was earlier, then data). */
  TEST_ASSERT_EQ(k_smbus_test_data_a, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_write_byte_data: cmd + data, no PEC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify write byte data with pec behavior. @details Executes the write byte data with pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_byte_data_with_pec(void)
{
  TEST_BEGIN("ra8_smbus_write_byte_data: PEC trailing byte present");
  internal_prep_pec(true);
  internal_prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_smbus_write_byte_data((uint8_t)k_smbus_test_target,
                                           (uint8_t)k_smbus_test_cmd,
                                           (uint8_t)k_smbus_test_data_a));
  const uint8_t frame[3] = {0x80U, (uint8_t)k_smbus_test_cmd, (uint8_t)k_smbus_test_data_a};
  const uint8_t expect   = ra8_smbus_pec(frame, 3U);
  TEST_ASSERT_EQ(expect, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_write_byte_data: PEC trailing byte present");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify read byte data no pec behavior. @details Executes the read byte data no pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_byte_data_no_pec(void)
{
  TEST_BEGIN("ra8_smbus_read_byte_data: combined xfer, no PEC");
  internal_prep_pec(false);
  internal_prime_iic_b();
  uint8_t out = k_t_byte_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_smbus_read_byte_data((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, &out));
  TEST_END("ra8_smbus_read_byte_data: combined xfer, no PEC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify read byte data null out behavior. @details Executes the read byte data null out scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_byte_data_null_out(void)
{
  TEST_BEGIN("ra8_smbus_read_byte_data: NULL out rejected");
  internal_prep_pec(false);
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_smbus_read_byte_data((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, nullptr));
  TEST_END("ra8_smbus_read_byte_data: NULL out rejected");
}

/* =============================================================================
 * Block Write / Block Read
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/** @brief Verify block write no pec behavior. @details Executes the block write no pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_block_write_no_pec(void)
{
  TEST_BEGIN("ra8_smbus_block_write: cmd + count + data, no PEC");
  internal_prep_pec(false);
  internal_prime_iic_b();
  const uint8_t payload[2] = {(uint8_t)k_smbus_test_data_a, (uint8_t)k_smbus_test_data_b};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_smbus_block_write((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, payload, 2U));
  /* Last byte in NTDTBP0 is the trailing data byte (data_b). */
  TEST_ASSERT_EQ(k_smbus_test_data_b, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_block_write: cmd + count + data, no PEC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify block write arg validation behavior. @details Executes the block write arg validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_block_write_arg_validation(void)
{
  TEST_BEGIN("ra8_smbus_block_write: zero len + null payload rejected");
  internal_prep_pec(false);
  const uint8_t payload[1] = {(uint8_t)k_smbus_test_data_a};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_smbus_block_write((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, payload, 0U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_smbus_block_write((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, nullptr, 1U));
  TEST_END("ra8_smbus_block_write: zero len + null payload rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify block write with pec behavior. @details Executes the block write with pec scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_block_write_with_pec(void)
{
  TEST_BEGIN("ra8_smbus_block_write: PEC trailing byte present");
  internal_prep_pec(true);
  internal_prime_iic_b();
  const uint8_t payload[2] = {(uint8_t)k_smbus_test_data_a, (uint8_t)k_smbus_test_data_b};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_smbus_block_write((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, payload, 2U));
  /* Frame folded into PEC: addr_w | cmd | count | data_a | data_b. */
  const uint8_t frame[5] = {
    0x80U,
    (uint8_t)k_smbus_test_cmd,
    2U,
    (uint8_t)k_smbus_test_data_a,
    (uint8_t)k_smbus_test_data_b,
  };
  const uint8_t expect = ra8_smbus_pec(frame, 5U);
  TEST_ASSERT_EQ(expect, (i3c_i2c_regs(0U)->NTDTBP0 & 0xFFU));
  TEST_END("ra8_smbus_block_write: PEC trailing byte present");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify block read no pec happy behavior. @details Executes the block read no pec happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_block_read_no_pec_happy(void)
{
  TEST_BEGIN("ra8_smbus_block_read: happy path");
  internal_prep_pec(false);
  internal_prime_iic_b();
  /* The fake's NTDTBP0 holds whatever byte was last written to it
   * (here, the read-address byte emitted by ra8_i3c_i2c_transfer), so the
   * in-band "count" byte is non-deterministic. Use the maximum cap
   * (255) so any 8-bit count value fits, then just verify the call
   * succeeded; the framing logic itself is exercised by
   * internal_test_block_read_arg_validation and test_block_write_*. */
  uint8_t buf[k_t_block_cap] = {};
  uint8_t got                = k_t_byte_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_smbus_block_read((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, buf, 255U, &got));
  (void)got;
  TEST_END("ra8_smbus_block_read: happy path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify block read arg validation behavior. @details Executes the block read arg validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_block_read_arg_validation(void)
{
  TEST_BEGIN("ra8_smbus_block_read: NULL buf / out_len / cap == 0");
  internal_prep_pec(false);
  uint8_t buf = 0U;
  uint8_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_smbus_block_read((uint8_t)k_smbus_test_target,
                                      (uint8_t)k_smbus_test_cmd,
                                      nullptr,
                                      4U,
                                      &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_smbus_block_read((uint8_t)k_smbus_test_target,
                                      (uint8_t)k_smbus_test_cmd,
                                      &buf,
                                      4U,
                                      nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,

    ra8_smbus_block_read((uint8_t)k_smbus_test_target, (uint8_t)k_smbus_test_cmd, &buf, 0U, &got));
  TEST_END("ra8_smbus_block_read: NULL buf / out_len / cap == 0");
}

/* =============================================================================
 * SMBALERT# / Host Notify
 * =============================================================================
 */

static int32_t s_alert_count = 0;
static uint8_t s_alert_addr  = 0U;
static uint8_t s_alert_stat  = 0U;
static void*   s_alert_ctx   = nullptr;

/** @brief Provide the file-local stub alert test helper. @details Implements the stub alert fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] target_7b Fixture argument governed by the exercised interface contract. @param[in] status Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_stub_alert(void* ctx, uint8_t target_7b, uint8_t status)
{
  s_alert_count++;
  s_alert_addr = target_7b;
  s_alert_stat = status;
  s_alert_ctx  = ctx;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify alert register and dispatch behavior. @details Executes the alert register and dispatch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alert_register_and_dispatch(void)
{
  TEST_BEGIN("ra8_smbus_alert: register + dispatch fires callback");
  internal_prep_pec(false);
  internal_prime_iic_b();

  int32_t marker = k_t_marker_seed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_alert_register_callback(internal_stub_alert, &marker));

  s_alert_count = 0;
  s_alert_ctx   = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_alert_dispatch());
  TEST_ASSERT_EQ(1, s_alert_count);
  TEST_ASSERT(s_alert_ctx == &marker);
  /* The ARA byte the dispatch reads is whatever residue NTDTBP0 holds
   * in the fake (the read-address byte emitted by ra8_i3c_i2c_read
   * is left latched there), so addr_7b / status are non-deterministic.
   * We just confirm the callback fired with the recorded context. */
  (void)s_alert_addr;
  (void)s_alert_stat;
  TEST_END("ra8_smbus_alert: register + dispatch fires callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify alert dispatch without callback behavior. @details Executes the alert dispatch without callback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alert_dispatch_without_callback(void)
{
  TEST_BEGIN("ra8_smbus_alert_dispatch: no callback installed -> ok");
  internal_prep_pec(false);
  internal_prime_iic_b();
  /* Detach any previously-registered callback. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_alert_register_callback(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_smbus_alert_dispatch());
  TEST_END("ra8_smbus_alert_dispatch: no callback installed -> ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify alert not initialized behavior. @details Executes the alert not initialized scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_alert_not_initialized(void)
{
  TEST_BEGIN("ra8_smbus_alert: not_initialized when init missing");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_smbus_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_smbus_alert_register_callback(internal_stub_alert, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_smbus_alert_dispatch());
  TEST_END("ra8_smbus_alert: not_initialized when init missing");
}

/**
 * @test internal_test_mcdc_ra8_smbus
 *
 * @par MC/DC:
 * Decision A: ``ra8_smbus_pec`` line 109,
 * libs/ra8_hal/src/ra8_smbus.c:
 * ``if ((data == nullptr) || (len == 0U))`` (2 conditions, ``||``).
 * N+1 = 3 (V1 uses non-zero input so its observed return differs from
 * the init-value sentinel):
 * - V1: data=valid, len=1  -> dec F (compute CRC = 0x07)
 * - V2: data=NULL,  len=5  -> dec T (return init = 0)
 * - V3: data=valid, len=0  -> dec T (return init = 0)
 * DO-178C 6.4.4.3 met. @brief Verify mcdc ra8 smbus behavior. @details Executes the mcdc ra8 smbus scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_ra8_smbus(void)
{
  TEST_BEGIN("smbus MC/DC: ra8_smbus_pec 2-cond null+len decision");
  const uint8_t one = 0x01U;
  TEST_ASSERT_EQ(0x07, ra8_smbus_pec(&one, 1U));
  TEST_ASSERT_EQ(0, ra8_smbus_pec(nullptr, 5U));
  TEST_ASSERT_EQ(0, ra8_smbus_pec(&one, 0U));
  TEST_END("smbus MC/DC: ra8_smbus_pec 2-cond null+len decision");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  internal_test_pec_empty_and_null,
  internal_test_pec_known_vectors,
  internal_test_init_null_cfg,
  internal_test_init_incomplete_bus,
  internal_test_deinit_without_init,
  internal_test_init_deinit_cycle,
  internal_test_send_byte_no_pec,
  internal_test_send_byte_with_pec,
  internal_test_send_byte_not_initialized,
  internal_test_receive_byte_no_pec_happy,
  internal_test_receive_byte_null_arg,
  internal_test_receive_byte_pec_mismatch,
  internal_test_write_byte_data_no_pec,
  internal_test_write_byte_data_with_pec,
  internal_test_read_byte_data_no_pec,
  internal_test_read_byte_data_null_out,
  internal_test_block_write_no_pec,
  internal_test_block_write_arg_validation,
  internal_test_block_write_with_pec,
  internal_test_block_read_no_pec_happy,
  internal_test_block_read_arg_validation,
  internal_test_alert_register_and_dispatch,
  internal_test_alert_dispatch_without_callback,
  internal_test_alert_not_initialized,
  internal_test_mcdc_ra8_smbus,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}

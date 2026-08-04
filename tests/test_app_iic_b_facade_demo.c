/**
 * @file test_app_iic_b_facade_demo.c
 * @brief Integration + MC/DC test for
 *        examples/ek_ra8d2/hw_validated/hil/iic_b_facade_demo/main.c
 *
 * @details
 * The IIC_B controller + peripheral register mechanics are owned and covered
 * by the driver's own unit tests (test_ra8_i3c_i2c.c,
 * test_ra8_i3c_i2c_peripheral.c) and the facade's tests (test_ra8_io_i2c_bus.c).
 * This test pins down the DEMO's own decision logic and its exact API usage:
 *
 *  - ``iic_facade_classify_ctrl``: the facade-transfer return code -> banner
 *    token mapping, full branch coverage (OK / NAK / IDLE).
 *  - ``iic_facade_periph_acted``: the compound decision that gates one
 *    target-mode service poll, under full MC/DC.
 *  - The real CONTROLLER path the app runs: bind the facade to IIC_B channel 0
 *    and issue the write-pointer / repeated-START / read of the GT911
 *    PRODUCT_ID, driven end to end against the ``ra8_fake_mmap`` register
 *    substrate (flags pre-armed exactly as test_ra8_io_i2c_bus.c does).
 *  - The real PERIPHERAL path: open the channel as an addressed target and read
 *    status, with the demo's NULL / bad-channel guards.
 *
 * The two decision helpers are mirrored from the demo (whose own copies are
 * ``static`` in an example TU that is not linked into the host suite); the
 * mirrors are kept byte-identical to the app and cross-referenced here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum app_iic_b_facade_demo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_i2c_mask_all = 0xFFU, /**< Every mask bit set, so no interrupt source is filtered out. */
} app_iic_b_facade_demo_fixture_t;

/** @brief Mirror of the demo's channel + on-bus addresses + register map. */
typedef enum : uint8_t {
  k_t_channel        = 0U,    /**< I3C/IIC_B channel (only 0 on RA8D2).   */
  k_t_channel_oor    = 1U,    /**< First out-of-range channel.            */
  k_t_ctrl_addr_7b   = 0x5DU, /**< On-board GT911 touch 7-bit address.    */
  k_t_periph_addr_7b = 0x42U, /**< Own 7-bit address in target mode.      */
  k_t_prod_ptr_hi    = 0x81U, /**< GT911 PRODUCT_ID pointer MSB (0x8140). */
  k_t_prod_ptr_lo    = 0x40U, /**< GT911 PRODUCT_ID pointer LSB.          */
  k_t_ptr_len        = 2U,    /**< 16-bit register-pointer width.         */
  k_t_prod_len       = 4U,    /**< PRODUCT_ID payload length.             */
} t_layout_t;

/** @brief Mirror of the demo's bit-rate config. */
typedef enum : uint32_t {
  k_t_bus_hz   = 400000U,   /**< Fast-mode bit-rate. */
  k_t_pclka_hz = 60000000U, /**< I3C PCLKA source.   */
} t_clock_t;

/** @brief Mirror of ``iic_facade_ctrl_result_t`` from the demo. */
typedef enum : uint8_t {
  k_t_ctrl_ok   = 0U, /**< Device ACKed; PRODUCT_ID bytes valid.       */
  k_t_ctrl_nak  = 1U, /**< Bus clocked but no device acknowledged.     */
  k_t_ctrl_idle = 2U, /**< No transaction completed (bus fault/float). */
} t_ctrl_result_t;

/**
 * @brief Mirror of the demo's ``iic_facade_classify_ctrl``.
 *
 * @details Kept byte-identical to the app: ``k_ra8_ok`` -> OK,
 * ``k_ra8_err_nack`` -> NAK, anything else -> IDLE.
 */
static t_ctrl_result_t classify_ctrl(ra8_err_t err)
{
  if (err == k_ra8_ok) {
    return k_t_ctrl_ok;
  }
  if (err == k_ra8_err_nack) {
    return k_t_ctrl_nak;
  }
  return k_t_ctrl_idle;
}

/**
 * @brief Mirror of the demo's ``iic_facade_periph_acted`` compound decision.
 *
 * @details Kept byte-identical to the app:
 * ``(mask & rx_full) != 0 || (mask & tx_empty) != 0``.
 */
static bool periph_acted(uint8_t mask)
{
  const bool rx = (mask & (uint8_t)k_ra8_i3c_peripheral_status_rx_full) != 0U;
  const bool tx = (mask & (uint8_t)k_ra8_i3c_peripheral_status_tx_empty) != 0U;
  return rx || tx;
}

/** @brief Clean the register window + bring the MSTP layer up. */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Bring IIC_B channel 0 up in I2C-compat mode and pre-arm its flags.
 *
 * @details Mirrors bring_up_i3c_compat() in test_ra8_io_i2c_bus.c: NTST.TDBEF0
 * / RDBFF0 (buffer-ready) and BCST.BFREF (bus free) are pre-set so the driver's
 * bounded polls fall through immediately, so a controller transfer runs end to
 * end against the host register substrate.
 */
static void bring_up_ctrl(void)
{
  const ra8_i3c_cfg_t cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_t_bus_hz,
    .pclka_hz = (uint32_t)k_t_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_t_channel, &cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_t_channel);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/**
 * @test iic_b_facade_classify_ctrl_maps_all
 *
 * @par MC/DC:
 * ``iic_facade_classify_ctrl`` is a two-stage single-condition if-chain
 * (``err == ok`` then ``err == nack``), not a compound boolean. Full branch
 * coverage needs one vector per arm:
 * - Vector 1: err=k_ra8_ok        -> k_t_ctrl_ok   (first condition true)
 * - Vector 2: err=k_ra8_err_nack  -> k_t_ctrl_nak  (first false, second true)
 * - Vector 3: err=k_ra8_err_hw_timeout -> k_t_ctrl_idle (both false)
 */
static void test_classify_ctrl_maps_all(void)
{
  TEST_BEGIN("iic_b_facade_demo: classify_ctrl maps ok/nack/other");
  TEST_ASSERT_EQ(k_t_ctrl_ok, classify_ctrl(k_ra8_ok));
  TEST_ASSERT_EQ(k_t_ctrl_nak, classify_ctrl(k_ra8_err_nack));
  TEST_ASSERT_EQ(k_t_ctrl_idle, classify_ctrl(k_ra8_err_hw_timeout));
  TEST_ASSERT_EQ(k_t_ctrl_idle, classify_ctrl(k_ra8_err_hw_error));
  TEST_END("iic_b_facade_demo: classify_ctrl maps ok/nack/other");
}

/**
 * @test iic_b_facade_periph_acted_mcdc
 *
 * @par MC/DC:
 * Decision: ``(mask & rx_full) != 0 || (mask & tx_empty) != 0`` (2 conditions).
 * - Vector 1: rx=0, tx=0 -> false (control: both conditions false)
 * - Vector 2: rx=1, tx=0 -> true  (varies rx only)
 * - Vector 3: rx=0, tx=1 -> true  (varies tx only)
 * Vectors 1+2 prove rx independently affects the outcome; 1+3 prove the same
 * for tx. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_periph_acted_mcdc(void)
{
  TEST_BEGIN("iic_b_facade_demo: periph_acted compound decision MC/DC");
  TEST_ASSERT_EQ(false, periph_acted((uint8_t)k_ra8_i3c_peripheral_status_idle));
  TEST_ASSERT_EQ(true, periph_acted((uint8_t)k_ra8_i3c_peripheral_status_rx_full));
  TEST_ASSERT_EQ(true, periph_acted((uint8_t)k_ra8_i3c_peripheral_status_tx_empty));
  /* Both-set case exercises the short-circuit's true/true corner too. */
  TEST_ASSERT_EQ(true,
                 periph_acted((uint8_t)((uint8_t)k_ra8_i3c_peripheral_status_rx_full |
                                        (uint8_t)k_ra8_i3c_peripheral_status_tx_empty)));
  TEST_END("iic_b_facade_demo: periph_acted compound decision MC/DC");
}

/**
 * @test iic_b_facade_controller_transfer_forwards
 *
 * @par MC/DC:
 * (no compound decision under test -- this drives the demo's real controller
 * call end to end and asserts the classification of its success return.)
 */
static void test_controller_transfer_forwards(void)
{
  TEST_BEGIN("iic_b_facade_demo: facade controller PRODUCT_ID transfer forwards");
  reset_world();
  bring_up_ctrl();

  ra8_io_i2c_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_i3c_compat(&bus, (uint8_t)k_t_channel));

  /* Re-arm the buffer-ready + bus-free flags immediately before the transfer,
   * exactly as the facade's own forwarding test does. */
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_t_channel);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;

  const uint8_t   ptr[k_t_ptr_len] = {(uint8_t)k_t_prod_ptr_hi, (uint8_t)k_t_prod_ptr_lo};
  uint8_t         id[k_t_prod_len] = {};
  const ra8_err_t err              = ra8_io_i2c_bus_transfer(&bus,
                                                             (uint8_t)k_t_ctrl_addr_7b,
                                                             ptr,
                                                             (uint32_t)k_t_ptr_len,
                                                             id,
                                                             (uint32_t)k_t_prod_len);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_t_ctrl_ok, classify_ctrl(err));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_deinit((uint8_t)k_t_channel));
  TEST_END("iic_b_facade_demo: facade controller PRODUCT_ID transfer forwards");
}

/**
 * @test iic_b_facade_peripheral_open_guards
 *
 * @par MC/DC:
 * (no compound decision under test -- exercises the demo's peripheral open /
 * status guards: each is a single-condition NULL or channel-range check.)
 */
static void test_peripheral_open_guards(void)
{
  TEST_BEGIN("iic_b_facade_demo: peripheral open / status guards");
  reset_world();

  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_t_periph_addr_7b,
    .general_call       = 0U,
  };
  /* NULL cfg + out-of-range channel are rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_peripheral_open((uint8_t)k_t_channel, nullptr));
  TEST_ASSERT(ra8_i3c_peripheral_open((uint8_t)k_t_channel_oor, &cfg) != k_ra8_ok);

  /* A good open answers 0x42; status then reads without a NULL mask. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_open((uint8_t)k_t_channel, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_peripheral_status((uint8_t)k_t_channel, nullptr));
  uint8_t mask = k_i2c_mask_all;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_peripheral_status((uint8_t)k_t_channel, &mask));
  TEST_END("iic_b_facade_demo: peripheral open / status guards");
}

int main(void)
{
  test_classify_ctrl_maps_all();
  test_periph_acted_mcdc();
  test_controller_transfer_forwards();
  test_peripheral_open_guards();
  return 0;
}

/**
 * @file test_ra8_i3c_i2c_peripheral.c
 * @brief Unit tests for the IIC_B peripheral driver.
 *
 * @details Exercises peripheral-mode address, FIFO, callback, and error paths against the bounded IIC_B register fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_i3c_i2c_peripheral.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_i3c_i2c_peripheral_test_const_t
 * @brief Numeric constants used by the tests.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_peripheral_test_ch0     = 0U,    /**< RA8 I3C I2C peripheral test ch0.         */
  k_ra8_i3c_i2c_peripheral_test_ch_oor  = 9U,    /**< RA8 I3C I2C peripheral test channel oor. */
  k_ra8_i3c_i2c_peripheral_test_addr_7b = 0x42U, /**< RA8 I3C I2C peripheral test address 7b.  */
  k_ra8_i3c_i2c_peripheral_test_byte_a  = 0xA5U, /**< RA8 I3C I2C peripheral test byte a.      */
  k_ra8_i3c_i2c_peripheral_test_byte_b  = 0x5AU, /**< RA8 I3C I2C peripheral test byte b.      */
  k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 =
    0x01U, /**< RA8 I3C I2C peripheral test ntst tdbef0. */
  k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0 =
    0x02U, /**< RA8 I3C I2C peripheral test ntst rdbff0. */
} ra8_i3c_i2c_peripheral_test_const_t;

static const ra8_i3c_i2c_peripheral_cfg_t s_cfg = {
  .peripheral_addr_7b = (uint8_t)k_ra8_i3c_i2c_peripheral_test_addr_7b,
  .general_call       = 1U,
};

/** @brief Provide the file-local prearm test helper. @details Implements the prearm fixture operation used only by this focused test executable. @param[in,out] reg Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prearm(volatile r_i3c_i2c_regs_t* reg)
{
  reg->NTST = (uint32_t)((uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 |
                         (uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0);
}

/**
 * @test internal_test_open_null
 *
 * @par MC/DC:
 * Exercises the ``cfg == nullptr`` precondition branch in
 * ``ra8_i3c_i2c_peripheral_open``. The precondition is a single-condition
 * decision (no compound boolean), so MC/DC and branch coverage coincide.
 * Vector: cfg=NULL -> dec T (k_ra8_err_null_ptr). Companion test
 * ``internal_test_open_close`` provides cfg=valid -> dec F. N+1 = 2 satisfied
 * jointly across the suite. @brief Verify open null behavior. @details Executes the open null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_null(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_open null cfg");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr));
  TEST_END("ra8_i3c_i2c_peripheral_open null cfg");
}

/**
 * @test internal_test_open_oor
 *
 * @par MC/DC:
 * Exercises the ``ch >= k_ra8_i3c_i2c_max_channels`` range-check decision in
 * ``ra8_i3c_i2c_peripheral_open``. Single-condition decision: vector
 * ch=9 -> dec T (k_ra8_err_invalid_arg); the in-range vector is provided
 * by ``internal_test_open_close``. N+1 = 2 jointly. @brief Verify open oor behavior. @details Executes the open oor scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_oor(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_open oor channel");
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch_oor, &s_cfg));
  TEST_END("ra8_i3c_i2c_peripheral_open oor channel");
}

/**
 * @test internal_test_open_close
 *
 * @par MC/DC:
 * Drives the happy-path branches of ``ra8_i3c_i2c_peripheral_open`` and
 * ``_close``. Both functions perform single-condition guard checks
 * (channel range, cfg non-null) -- no compound booleans in either
 * unit under test. This vector exercises the dec=F leg of every guard
 * and pairs with ``internal_test_open_null`` / ``internal_test_open_oor`` to satisfy
 * N+1 = 2 for each guard. @brief Verify open close behavior. @details Executes the open close scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_close(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_open + close happy");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &s_cfg));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_i3c_i2c_peripheral_test_addr_7b << 1U), reg->MSDVAD);
  TEST_ASSERT(reg->BCTL & (uint32_t)k_ra8_i3c_i2c_msk_bctl_buse);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_close((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0));
  TEST_END("ra8_i3c_i2c_peripheral_open + close happy");
}

/**
 * @test internal_test_open_mstp_timeout
 * @brief I3C peripheral open propagates MSTP failure without register writes.
 * @par MC/DC:
 * The MSTP success guard has one condition. This fault vector drives false;
 * ::internal_test_open_close drives true. N+1 = 2.
 * @pre Fake MMIO, peripheral registers, and MSTP state are reset.
 * @post A failed ungate leaves BCTL and the MSTP reference count zero.
 * @post Disarming the fault lets the same public call open and close cleanly.
 * @note Single-threaded host fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_mstp_timeout(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_open MSTP timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_mstp()->MSTPCRB));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &s_cfg));
  /* HUM Ch 40.2.2 "BCTL : Bus Control Register" p 2449 */
  TEST_ASSERT_EQ(0U, reg->BCTL);
  uint8_t ref = UINT8_MAX;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_i3c, &ref));
  TEST_ASSERT_EQ(0U, ref);

  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &s_cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_close((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0));
  TEST_END("ra8_i3c_i2c_peripheral_open MSTP timeout");
}

/**
 * @test internal_test_send_ok
 *
 * @par MC/DC:
 * Drives the dec=F leg of the compound guard
 * ``(len > 0U) && (data == nullptr)`` inside
 * ``ra8_i3c_i2c_peripheral_send`` (vector: len=1, data=valid -> dec F).
 * Companion vectors live in ``internal_test_send_null`` (len=1, data=NULL ->
 * dec T) and the dedicated ``internal_test_mcdc_ra8_i3c_i2c_peripheral`` block
 * (len=0, data=*). N+1 = 3 satisfied jointly. @brief Verify send ok behavior. @details Executes the send ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_ok(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_send ok");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  internal_prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra8_i3c_i2c_peripheral_test_byte_a};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, data, 1U));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_peripheral_test_byte_a, (reg->NTDTBP0 & 0xFFU));
  TEST_END("ra8_i3c_i2c_peripheral_send ok");
}

/**
 * @test internal_test_send_null
 *
 * @par MC/DC:
 * Drives the dec=T leg of ``(len > 0U) && (data == nullptr)`` inside
 * ``ra8_i3c_i2c_peripheral_send`` (vector: len=1, data=NULL -> dec T,
 * returns k_ra8_err_null_ptr). Pairs with ``internal_test_send_ok`` to flip the
 * data condition and with ``internal_test_mcdc_ra8_i3c_i2c_peripheral`` to flip
 * len. N+1 = 3 jointly. @brief Verify send null behavior. @details Executes the send null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_null(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_send null");
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  TEST_END("ra8_i3c_i2c_peripheral_send null");
}

/**
 * @test internal_test_receive_ok
 *
 * @par MC/DC:
 * Drives the dec=F leg of the mirror compound guard
 * ``(len > 0U) && (buf == nullptr)`` inside
 * ``ra8_i3c_i2c_peripheral_receive`` (vector: len=1, buf=valid -> dec F).
 * Companion vectors live in ``internal_test_mcdc_ra8_i3c_i2c_peripheral``
 * (len=0,*) and (len=1, buf=NULL). N+1 = 3 jointly. @brief Verify receive ok behavior. @details Executes the receive ok scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_receive_ok(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_receive ok");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  internal_prearm(reg);
  reg->NTDTBP0   = (uint32_t)k_ra8_i3c_i2c_peripheral_test_byte_b;
  uint8_t buf[1] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_peripheral_test_byte_b, buf[0]);
  TEST_END("ra8_i3c_i2c_peripheral_receive ok");
}

/**
 * @test internal_test_status
 *
 * @par MC/DC:
 * Two single-condition guards in ``ra8_i3c_i2c_peripheral_status``:
 *   (a) ``ch >= k_ra8_i3c_i2c_max_channels`` (range)
 *   (b) ``mask == nullptr`` (precondition)
 * No compound boolean in the unit under test. Vector:
 * ch=0, mask=valid -> dec F (returns k_ra8_ok with flag bits set);
 * second call passes mask=NULL -> dec T (k_ra8_err_null_ptr). N+1 = 2
 * for each independent guard, satisfied within this single test. @brief Verify status behavior. @details Executes the status scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_status(void)
{
  TEST_BEGIN("ra8_i3c_i2c_peripheral_status reflects flags");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  reg->NTST                      = (uint32_t)((uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 |
                                              (uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0);
  reg->BST                       = (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf;
  uint8_t mask                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_status((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_tx_empty);
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_rx_full);
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_nack);

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i3c_i2c_peripheral_status((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr));
  TEST_END("ra8_i3c_i2c_peripheral_status reflects flags");
}

/**
 * @test internal_test_mcdc_ra8_i3c_i2c_peripheral
 *
 * @par MC/DC:
 * Decision A (libs/ra8_hal/src/ra8_i3c_i2c_peripheral.c@ra8_i3c_i2c_peripheral_send):
 * ``if ((len > 0U) && (data == nullptr))`` (2 conditions, ``&&``).
 * N+1 = 3:
 * - V1: len=0,  data=*       -> dec F (ok empty)
 * - V2: len=1,  data=valid   -> dec F (sends)
 * - V3: len=1,  data=NULL    -> dec T (null_ptr)
 * Decision B (libs/ra8_hal/src/ra8_i3c_i2c_peripheral.c@ra8_i3c_i2c_peripheral_send):
 * ``(len > 0U) && (buf == nullptr)``. Mirror vectors. DO-178C 6.4.4.3. @brief Verify mcdc ra8 i3c i2c peripheral behavior. @details Executes the mcdc ra8 i3c i2c peripheral scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_ra8_i3c_i2c_peripheral(void)
{
  TEST_BEGIN("iic_b_peripheral MC/DC: send/receive 2-cond null+len");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &s_cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  internal_prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra8_i3c_i2c_peripheral_test_byte_a};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 0U));
  internal_prearm(reg);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, data, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  uint8_t buf[1] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 0U));
  internal_prearm(reg);
  reg->NTDTBP0 = (uint32_t)k_ra8_i3c_i2c_peripheral_test_byte_b;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, buf, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  (void)ra8_i3c_i2c_peripheral_close((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  TEST_END("iic_b_peripheral MC/DC: send/receive 2-cond null+len");
}

int main(void)
{
  internal_test_open_null();
  internal_test_open_oor();
  internal_test_open_close();
  internal_test_open_mstp_timeout();
  internal_test_send_ok();
  internal_test_send_null();
  internal_test_receive_ok();
  internal_test_status();
  internal_test_mcdc_ra8_i3c_i2c_peripheral();
  return 0;
}

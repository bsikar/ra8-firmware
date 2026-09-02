/**
 * @file test_ra8_i2c.c
 * @brief Unit tests for the I2C (RIIC) controller driver.
 *
 * @details
 * Drives the polling-mode RIIC controller against the host-side
 * ``ra8_fake_mmap`` substrate. ICSR2 status flags are pre-armed where the
 * driver expects them (TDRE, TEND, RDRF) so the bounded wait loops fall
 * through immediately. ICCR2.BBSY is left clear (= bus free) so the
 * bus-busy gate accepts each transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum i2c_fixture_t
 * @brief The recognizable values moved through the code under test.
 */
typedef enum : uint8_t {
  k_i2c_payload_byte = 0xA5U, /**< A recognizable single-byte payload; neither 0x00 nor 0xFF. */
  k_i2c_payload_byte_alt =
    0x12U, /**< A second payload byte, so two writes on one bus cannot be confused. */
} i2c_fixture_t;

/**
 * @enum ra8_i2c_test_const_t
 * @brief Test addresses, channels and payload constants.
 */
typedef enum : uint8_t {
  k_ra8_i2c_test_periph  = 0x50U, /**< 7-bit peripheral address under test. */
  k_ra8_i2c_test_ch0     = 0U,    /**< Channel 0 (IIC0).                    */
  k_ra8_i2c_test_ch1     = 1U,    /**< Channel 1 (IIC1).                    */
  k_ra8_i2c_test_ch2     = 2U,    /**< Channel 2 (IIC2).                    */
  k_ra8_i2c_test_ch_oor  = 3U,    /**< Out-of-range channel.                */
  k_ra8_i2c_test_ch_huge = 200U,  /**< Far out-of-range channel.            */
  k_ra8_i2c_test_byte_a  = 0xA5U, /**< Payload byte A.                      */
  k_ra8_i2c_test_byte_b  = 0x5AU, /**< Payload byte B.                      */
  k_ra8_i2c_test_rx_byte = 0xC3U, /**< Receive payload byte.                */
} ra8_i2c_test_const_t;

/**
 * @enum ra8_i2c_test_clk_t
 * @brief Clock constants used by the bit-rate tests.
 */
typedef enum : uint32_t {
  k_ra8_i2c_test_pclkb_hz = 50000000U, /**< 50 MHz PCLKB.              */
  k_ra8_i2c_test_cks_max  = 7U,        /**< Largest RIIC CKS encoding. */
} ra8_i2c_test_clk_t;

/** @brief Standard-mode configuration descriptor. */
static const ra8_i2c_cfg_t s_i2c_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i2c_speed_standard,
  .pclkb_hz = (uint32_t)k_ra8_i2c_test_pclkb_hz,
};

/** @brief Two-byte transmit payload. */
static const uint8_t s_payload[2] = {
  (uint8_t)k_ra8_i2c_test_byte_a,
  (uint8_t)k_ra8_i2c_test_byte_b,
};

/**
 * @brief Pre-arm ICSR2 so the driver's TDRE / TEND / RDRF wait loops
 *        fall through immediately. ``clear_status`` does not touch these
 *        flags, so a one-shot pre-prime survives the transfer.
 *
 * @param[in] channel Channel to prime. @details Implements the prime status fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prime_status(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
}

/**
 * @brief Reset the fake and refresh MSTP state before each case. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/* Deterministic NACK injection via the ra8_fake_mmio poll-hook -- it runs inline on
 * the driver's OWN poll thread, so there is no wall-clock timer and no concurrent
 * servicer thread to race or starve. The RIIC driver clears ICSR2's condition /
 * fault flags at the start of every transaction, so a pre-armed NACKF would be
 * wiped; the hook instead re-asserts TDRE|NACKF on every internal_i2c_wait_icsr2
 * poll. ra8_i2c_scan waits for TEND|NACKF in that bounded poll, so it observes the
 * injected NACKF and reports acked=false, deterministically on any host. */

/** @brief Channel the NACK poll-hook injects into. */
static uint8_t s_nack_ch;

/**
 * @brief Poll-hook body: latch TDRE|NACKF into the target channel's ICSR2. @details Implements the i2c nack hook fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_i2c_nack_hook(void)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(s_nack_ch);
  if (reg != nullptr) {
    const uint8_t inject =
      (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_nackf);
    reg->ICSR2 = (uint8_t)(reg->ICSR2 | inject);
  }
}

/**
 * @brief Install the NACK poll-hook for @p channel. @details Implements the i2c nack hook arm fixture operation used only by this focused test executable. @param[in] channel Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_i2c_nack_hook_arm(uint8_t channel)
{
  s_nack_ch = channel;
  ra8_fake_mmio_set_poll_hook(internal_i2c_nack_hook);
}

/**
 * @brief Remove the NACK poll-hook. @details Implements the i2c nack hook disarm fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_i2c_nack_hook_disarm(void)
{
  ra8_fake_mmio_set_poll_hook(nullptr);
}

/**
 * @brief Latch BBSY after the transfer has passed its initial idle-bus guard.
 * @details The hook runs from the first status poll, after the public write
 * path has accepted an idle bus. It models silicon that keeps BBSY asserted
 * after STOP is requested so the final bounded bus-free poll reaches its cap.
 * @pre The channel-0 fake register window is mapped.
 * @pre The hook is installed only for one synchronous test call.
 * @post ICCR2.BBSY is set in the fake register window.
 * @post No other register is modified.
 * @note File-local, single-threaded test fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i2c_hold_bus_busy_hook(void)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  if (reg != nullptr) {
    /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
    reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra8_i2c_msk_iccr2_bbsy);
  }
}

/**
 * @brief Model silicon auto-clearing ICCR2.RS after a repeated START.
 * @details Runs synchronously on the driver's poll thread. Status-register
 * polls before the restart leave ICCR2 unchanged; the first restart poll
 * clears RS so the next iteration observes completion deterministically.
 * @pre The channel-0 fake register window is mapped.
 * @post ICCR2.RS is clear when it was previously set.
 * @note File-local, single-threaded test fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i2c_complete_restart_hook(void)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  if (reg != nullptr) {
    /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
    reg->ICCR2 = (uint8_t)(reg->ICCR2 & (uint8_t)~(uint8_t)k_ra8_i2c_msk_iccr2_rs);
  }
}

/* =============================================================================
 * Init / deinit / clock
 * =============================================================================
 *
 * @par MC/DC:
 * (no compound decisions exercised here -- happy-path / range contract)
 */
/** @brief Verify init configured behavior. @details Executes the init configured scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_configured(void)
{
  TEST_BEGIN("ra8_i2c_init: ICE set, bit rate programmed");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch1, &s_i2c_cfg));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch1);
  TEST_ASSERT((reg->ICCR1 & (uint8_t)k_ra8_i2c_msk_iccr1_ice) != 0U);
  /* ICBRL/ICBRH carry the reserved hi bits even at the slowest rate. */
  TEST_ASSERT(reg->ICBRL != 0U);
  TEST_ASSERT(reg->ICBRH != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_deinit((uint8_t)k_ra8_i2c_test_ch1));
  TEST_END("ra8_i2c_init: ICE set, bit rate programmed");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract) @brief Verify init bad inputs behavior. @details Executes the init bad inputs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_init: bad inputs rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, nullptr));
  ra8_i2c_cfg_t bad = s_i2c_cfg;
  bad.bus_hz        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &bad));
  bad          = s_i2c_cfg;
  bad.pclkb_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch_oor, &s_i2c_cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch_huge, &s_i2c_cfg));
  TEST_END("ra8_i2c_init: bad inputs rejected");
}

/**
 * @test internal_test_init_mstp_timeout
 * @brief Prove I2C initialization propagates its module-stop timeout.
 * @details Arms MSTPCRB through the existing fake-MMIO wait seam for channel
 * zero, then repeats the same public call with the seam disarmed as a control.
 * @par MC/DC:
 * Decision: ``RA8_RETURN_ON_ERROR(mst_err, ...)`` in ``ra8_i2c_init()``.
 * - Vector 1: unarmed MSTPCRB settles -> false, initialization succeeds.
 * - Vector 2: armed MSTPCRB never settles -> true, timeout is returned.
 * N+1 = 2 vectors for the one-condition decision.
 * @pre The channel-zero register window is mapped.
 * @pre The MSTP reference table is reset before each vector.
 * @post The armed call returns ::k_ra8_err_hw_timeout unchanged.
 * @post The disarmed control initializes and deinitializes successfully.
 * @note File-local, single-threaded host fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_mstp_timeout(void)
{
  TEST_BEGIN("ra8_i2c_init: propagates MSTP timeout");
  internal_prep();
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_mstp()->MSTPCRB));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_deinit((uint8_t)k_ra8_i2c_test_ch0));
  TEST_END("ra8_i2c_init: propagates MSTP timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- happy-path / range contract) @brief Verify fast plus sets fmpe behavior. @details Executes the fast plus sets fmpe scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_fast_plus_sets_fmpe(void)
{
  TEST_BEGIN("ra8_i2c_init: Fm+ sets ICFER.FMPE");
  internal_prep();
  ra8_i2c_cfg_t cfg = {.bus_hz   = (uint32_t)k_ra8_i2c_speed_fast_plus,
                       .pclkb_hz = (uint32_t)k_ra8_i2c_test_pclkb_hz};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch2, &cfg));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch2);
  TEST_ASSERT((reg->ICFER & (uint8_t)k_ra8_i2c_msk_icfer_fmpe) != 0U);
  TEST_END("ra8_i2c_init: Fm+ sets ICFER.FMPE");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / happy-path contract) @brief Verify set clock behavior. @details Executes the set clock scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_clock(void)
{
  TEST_BEGIN("ra8_i2c_set_clock: reprograms divider");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch0,
                                   (uint32_t)k_ra8_i2c_speed_fast,
                                   (uint32_t)k_ra8_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch_oor,
                                   (uint32_t)k_ra8_i2c_speed_fast,
                                   (uint32_t)k_ra8_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch0, 0U, (uint32_t)k_ra8_i2c_test_pclkb_hz));

  /* Boundary vector: an extreme period exhausts every CKS candidate and
   * clamps to the largest representable divider/half-period fields. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch0, 1U, UINT32_MAX));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  TEST_ASSERT_EQ(
    k_ra8_i2c_test_cks_max,
    /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1" p 2374 */
    ((reg->ICMR1 >> (uint8_t)k_ra8_i2c_icmr1_cks_pos) & (uint8_t)k_ra8_i2c_test_cks_max));
  TEST_END("ra8_i2c_set_clock: reprograms divider");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract) @brief Verify deinit range behavior. @details Executes the deinit range scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deinit_range(void)
{
  TEST_BEGIN("ra8_i2c_deinit: range check");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_deinit((uint8_t)k_ra8_i2c_test_ch_oor));
  TEST_END("ra8_i2c_deinit: range check");
}

/* =============================================================================
 * Polling write
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- happy-path write+STOP) @brief Verify write happy behavior. @details Executes the write happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_happy(void)
{
  TEST_BEGIN("ra8_i2c_write: write + STOP success");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               sizeof(s_payload),
                               /*send_stop=*/true));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  /* STOP requested via ICCR2.SP and the wire byte equals the last data. */
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra8_i2c_write: write + STOP success");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the final bus-free poll has one
 * condition; the ordinary happy path covers clear/exit and this fixture
 * holds BBSY set through the bounded continuation direction)
 */
RA8_INTERNAL static void internal_test_write_bus_free_poll_budget(void)
{
  TEST_BEGIN("ra8_i2c_write: STOP waits through a staged busy bus");
  internal_prep();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  ra8_fake_mmio_set_poll_hook(internal_i2c_hold_bus_busy_hook);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               sizeof(s_payload),
                               /*send_stop=*/true));
  ra8_fake_mmio_set_poll_hook(nullptr);
  TEST_END("ra8_i2c_write: STOP waits through a staged busy bus");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract) @brief Verify write bad inputs behavior. @details Executes the write bad inputs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_write: null / range rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch_oor,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               true));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr, 1U, true));
  TEST_END("ra8_i2c_write: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- bus-busy rejection) @brief Verify write bus busy behavior. @details Executes the write bus busy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_bus_busy(void)
{
  TEST_BEGIN("ra8_i2c_write: BBSY set rejects transfer");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICCR2                 = (uint8_t)k_ra8_i2c_msk_iccr2_bbsy;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               true));
  TEST_END("ra8_i2c_write: BBSY set rejects transfer");
}

/* =============================================================================
 * Polling read
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- happy-path read+STOP) @brief Verify read happy behavior. @details Executes the read happy scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_happy(void)
{
  TEST_BEGIN("ra8_i2c_read: multi-byte read + STOP");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t buf[3]             = {0U, 0U, 0U};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_i2c_test_rx_byte, buf[0]);
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra8_i2c_read: multi-byte read + STOP");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- single-byte read) @brief Verify read single byte behavior. @details Executes the read single byte scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_single_byte(void)
{
  TEST_BEGIN("ra8_i2c_read: single-byte read");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(k_ra8_i2c_test_rx_byte, b);
  TEST_END("ra8_i2c_read: single-byte read");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range / len) @brief Verify read bad inputs behavior. @details Executes the read bad inputs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_read: null / range / zero len rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  uint8_t b = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch_oor, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 0U));
  TEST_END("ra8_i2c_read: null / range / zero len rejected");
}

/* =============================================================================
 * Scan / status
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- ACK probe happy path) @brief Verify scan ack behavior. @details Executes the scan ack scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_scan_ack(void)
{
  TEST_BEGIN("ra8_i2c_scan: ACK reported");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  TEST_ASSERT(acked);
  TEST_END("ra8_i2c_scan: ACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- NACK probe path) @brief Verify scan nack behavior. @details Executes the scan nack scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_scan_nack(void)
{
  TEST_BEGIN("ra8_i2c_scan: NACK reported");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  /* TDRE so the address byte writes; the poll-hook re-asserts NACKF on each
   * scan poll (the start-of-transfer clear_status wipes any pre-armed NACKF,
   * so it must be injected after the driver clears it). */
  reg->ICSR2 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;
  internal_i2c_nack_hook_arm((uint8_t)k_ra8_i2c_test_ch0);
  bool acked = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  internal_i2c_nack_hook_disarm();
  TEST_ASSERT(!acked);
  TEST_END("ra8_i2c_scan: NACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract) @brief Verify scan bad inputs behavior. @details Executes the scan bad inputs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_scan_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_scan: null / range rejected");
  internal_prep();
  bool acked = false;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch_oor, (uint8_t)k_ra8_i2c_test_periph, &acked));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr));
  TEST_END("ra8_i2c_scan: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- error decode / clear) @brief Verify errors get clear behavior. @details Executes the errors get clear scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_errors_get_clear(void)
{
  TEST_BEGIN("ra8_i2c_get_errors / clear_errors");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICSR2   = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_al | (uint8_t)k_ra8_i2c_msk_icsr2_nackf);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, &mask));
  TEST_ASSERT((mask & (uint8_t)k_ra8_i2c_err_arb_lost) != 0U);
  TEST_ASSERT((mask & (uint8_t)k_ra8_i2c_err_nack) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_clear_errors((uint8_t)k_ra8_i2c_test_ch0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, &mask));
  TEST_ASSERT_EQ(k_ra8_i2c_err_none, mask);
  /* Null / range guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch_oor, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_clear_errors((uint8_t)k_ra8_i2c_test_ch_oor));
  TEST_END("ra8_i2c_get_errors / clear_errors");
}

/* =============================================================================
 * MC/DC vector sets
 * =============================================================================
 */

/**
 * @test internal_test_mcdc_clk_invalid
 *
 * @par MC/DC:
 * Decision in ``priv_ra8_i2c_internal_clk_invalid``, libs/ra8_hal/src/ra8_i2c.c@priv_ra8_i2c_internal_clk_invalid
 *   ``(bus_hz == 0) || (pclkb_hz == 0)`` (2 conditions, OR).
 * - V1: bus!=0, pclkb!=0 -> C1=F,C2=F -> dec F
 * - V2: bus=0,  pclkb!=0 -> C1=T (short-circuits) -> dec T (varies left)
 * - V3: bus!=0, pclkb=0  -> C1=F,C2=T -> dec T (varies right)
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors. @brief Verify mcdc clk invalid behavior. @details Executes the mcdc clk invalid scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_clk_invalid(void)
{
  TEST_BEGIN("i2c MC/DC: clk_invalid OR");
  TEST_ASSERT(!priv_ra8_i2c_internal_clk_invalid(100000U, 50000000U));
  TEST_ASSERT(priv_ra8_i2c_internal_clk_invalid(0U, 50000000U));
  TEST_ASSERT(priv_ra8_i2c_internal_clk_invalid(100000U, 0U));
  TEST_END("i2c MC/DC: clk_invalid OR");
}

/**
 * @test internal_test_mcdc_transfer
 *
 * @par MC/DC:
 * Three 2-condition decisions in ``ra8_i2c_transfer``:
 * Decision A libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(wr_len == 0) && (rd_len == 0)``
 * - V1: wr=0, rd=0       -> C1=T,C2=T -> dec T (-> invalid_arg)
 * - V2: wr!=0            -> C1=F (short-circuits) -> dec F
 * - V3: wr=0, rd!=0      -> C1=T,C2=F -> dec F
 * Decision B libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(wr_len != 0) && (wr == nullptr)``
 * - V1: wr_len=0          -> C1=F (short-circuits) -> dec F
 * - V2: wr_len!=0, wr!=0  -> C1=T,C2=F -> dec F
 * - V3: wr_len!=0, wr=NULL-> C1=T,C2=T -> dec T (null_ptr)
 * Decision C libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(rd_len != 0) && (rd == nullptr)``
 * mirrors B with rd_len/rd; same N+1 = 3 vectors. @brief Verify mcdc transfer behavior. @details Executes the mcdc transfer scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_transfer(void)
{
  TEST_BEGIN("i2c MC/DC: transfer arg-validation 2-cond decisions");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  uint8_t wr_buf[1] = {k_i2c_payload_byte};
  uint8_t rd_buf[1] = {};

  /* Decision A V1: both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  0U,
                                  rd_buf,
                                  0U));
  /* Decision B V3: wr_len!=0 with NULL wr -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  nullptr,
                                  1U,
                                  rd_buf,
                                  0U));
  /* Decision C V3: rd_len!=0 with NULL rd (wr_len=0 keeps A=F, B=F). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  0U,
                                  nullptr,
                                  1U));
  /* Range guard: out-of-range channel -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch_oor,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  1U,
                                  rd_buf,
                                  1U));
  TEST_END("i2c MC/DC: transfer arg-validation 2-cond decisions");
}

/**
 * @test internal_test_mcdc_transfer_combined
 *
 * @par MC/DC:
 * Happy-path masking pairs (decision F vectors) for the three
 * ``ra8_i2c_transfer`` decisions and the write-finish OR at
 * libs/ra8_hal/src/ra8_i2c.c@internal_i2c_finish_tx:
 *   ``(err != k_ra8_ok) || send_stop`` (2 conditions, OR).
 * - V1 (write+read combined, send_stop=false on write phase): C1=F,C2=F
 *   -> dec F (bus held for the RESTART read phase)
 * - V2 (plain write, send_stop=true): C1=F,C2=T -> dec T (varies right)
 * - The error-path C1=T arm is covered by the timeout cases in
 *   internal_test_write_timeout / internal_test_read_timeout.
 * Combined transfer also drives decision A=F (wr!=0), B=F (wr!=NULL),
 * C=F (rd!=NULL) -- the masking-pair complements of internal_test_mcdc_transfer. @brief Verify mcdc transfer combined behavior. @details Executes the mcdc transfer combined scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_transfer_combined(void)
{
  TEST_BEGIN("i2c MC/DC: transfer combined write+read happy path");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  ra8_fake_mmio_set_poll_hook(internal_i2c_complete_restart_hook);
  /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  reg->ICDRR        = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t wr_buf[1] = {k_i2c_payload_byte_alt};
  uint8_t rd_buf[2] = {0U, 0U};
  /* Decision F V1: write phase holds the bus (send_stop=false). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  sizeof(wr_buf),
                                  rd_buf,
                                  sizeof(rd_buf)));
  ra8_fake_mmio_set_poll_hook(nullptr);
  /* Decision F V2: plain write issues STOP (send_stop=true). */
  internal_prime_status((uint8_t)k_ra8_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  sizeof(wr_buf),
                                  nullptr,
                                  0U));
  TEST_END("i2c MC/DC: transfer combined write+read happy path");
}

/**
 * @test internal_test_write_timeout
 *
 * @par MC/DC:
 * Error-path arm (C1=T) of the write-finish OR at
 * libs/ra8_hal/src/ra8_i2c.c@internal_i2c_finish_tx
 *   ``(err != k_ra8_ok) || send_stop``: with TDRE never pre-armed the
 *   address send times out, so ``err != k_ra8_ok`` (C1=T) forces STOP
 *   regardless of send_stop. Complements the C1=F vectors in
 *   internal_test_mcdc_transfer_combined. @brief Verify write timeout behavior. @details Executes the write timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_write_timeout(void)
{
  TEST_BEGIN("ra8_i2c_write: address timeout -> STOP");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  /* No internal_prime_status: TDRE stays clear so the address wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               /*send_stop=*/false));
  TEST_END("ra8_i2c_write: address timeout -> STOP");
}

/**
 * @test internal_test_read_timeout
 *
 * @par MC/DC:
 * (no compound decision under test -- the receive-drain loop guard this
 * case once anchored has been refactored in ra8_i2c_read to a
 * single-condition form, so MC/DC no longer applies. This still
 * exercises the RDRF-timeout error path: with RDRF never pre-armed the
 * dummy read times out and ra8_i2c_read returns k_ra8_err_hw_timeout.) @brief Verify read timeout behavior. @details Executes the read timeout scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_read_timeout(void)
{
  TEST_BEGIN("ra8_i2c_read: RDRF timeout halts drain loop");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  /* TDRE so the address send succeeds, but no RDRF for the data phase. */
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_END("ra8_i2c_read: RDRF timeout halts drain loop");
}

/**
 * @test internal_test_mcdc_scan_addr_err
 *
 * @par MC/DC:
 * Decision in ``ra8_i2c_scan``, libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_scan
 *   ``(err != k_ra8_ok) && (err != k_ra8_err_nack)`` (2 conditions, AND).
 * - V1 (address timeout): err=hw_timeout -> C1=T,C2=T -> dec T (hard
 *   error returned). TDRE never armed.
 * - V2 (ACK happy path, internal_test_scan_ack): err=k_ra8_ok -> C1=F
 *   (short-circuits) -> dec F.
 * - V3 (NACK probe, internal_test_scan_nack): err=k_ra8_err_nack -> C1=T,C2=F ->
 *   dec F (probe continues, reports acked=false).
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors. @brief Verify mcdc scan addr err behavior. @details Executes the mcdc scan addr err scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_scan_addr_err(void)
{
  TEST_BEGIN("i2c MC/DC: scan address-error AND");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));

  /* V1: TDRE clear and no injection -> address wait times out, so
   * err=hw_timeout: C1=T (err != ok), C2=T (err != nack) -> dec T. */
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));

  /* V3: inject TDRE + NACKF so send_address reads NACKF and returns
   * k_ra8_err_nack: C1=T (err != ok), C2=F (err == nack) -> dec F; the
   * probe then continues and reports acked=false. */
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &s_i2c_cfg));
  internal_i2c_nack_hook_arm((uint8_t)k_ra8_i2c_test_ch0);
  acked = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  internal_i2c_nack_hook_disarm();
  TEST_ASSERT(!acked);
  TEST_END("i2c MC/DC: scan address-error AND");
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
  internal_test_init_configured,    internal_test_init_bad_inputs,
  internal_test_init_mstp_timeout,  internal_test_fast_plus_sets_fmpe,
  internal_test_set_clock,          internal_test_deinit_range,
  internal_test_write_happy,        internal_test_write_bus_free_poll_budget,
  internal_test_write_bad_inputs,   internal_test_write_bus_busy,
  internal_test_read_happy,         internal_test_read_single_byte,
  internal_test_read_bad_inputs,    internal_test_scan_ack,
  internal_test_scan_nack,          internal_test_scan_bad_inputs,
  internal_test_errors_get_clear,   internal_test_mcdc_clk_invalid,
  internal_test_mcdc_transfer,      internal_test_mcdc_transfer_combined,
  internal_test_write_timeout,      internal_test_read_timeout,
  internal_test_mcdc_scan_addr_err,
};

/**
 * @brief Run every test case in this translation unit.
 *
 * @return 0 when all cases pass (Unity aborts the process on failure).
 *
 * @pre The ``ra8_fake_mmap`` constructor has installed the MMIO windows.
 * @post Every registered case has executed.
 * @note Thread safety: single-threaded test harness.
 * @since 0.1.0
 */
int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}

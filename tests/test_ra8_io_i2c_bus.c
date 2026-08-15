/**
 * @file test_ra8_io_i2c_bus.c
 * @brief Unit tests for the ra8_io I2C-bus facade
 *        (libs/ra8_io/src/ra8_io_i2c_bus*.c).
 *
 * @details
 * Binds both backends of the facade -- classic RIIC
 * (`ra8_io_i2c_bus_bind_riic`) and the I3C block's I2C-compatibility mode
 * (`ra8_io_i2c_bus_bind_i3c_compat`) -- against the ``ra8_fake_mmap``
 * register substrate and drives the vtable end to end:
 *
 *  - dispatcher validation: NULL handle and never-bound handle rejected
 *    on every public entry point;
 *  - binder validation: NULL handle and out-of-range channels rejected;
 *  - forwarding: `write` / `read` / `transfer` land in the real
 *    `ra8_i2c_*` / `ra8_i3c_*` drivers, observed through the same register
 *    effects the drivers' own tests assert (RIIC ICCR2.SP STOP request,
 *    ICDRR capture; I3C ACKCTL/CNDCTL end-state);
 *  - the facade's `send_stop` semantics on both backends (the I3C
 *    backend inverts it into the driver's `restart` flag);
 *  - the `ra8_io_i2c_bus_as_ops` bridge into the Ring-3 seam, including
 *    each trampoline's NULL-cookie guard.
 *
 * Determinism mirrors the sibling driver tests: RIIC ICSR2 flags and I3C
 * NTST/BCST flags are pre-armed so the drivers' bounded polls fall
 * through immediately; no timers and no signal injection are used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i2c_regs.h"
#include "ra8_i3c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_io_i2c_bus_riic.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ra8_io_i2c_bus_test_const_t
 * @brief Channels, clocks, addresses and payload bytes for the tests.
 */
typedef enum : uint32_t {
  k_test_riic_ch     = 0U,        /**< RIIC channel under test.    */
  k_test_riic_ch_oor = 3U,        /**< First invalid RIIC channel. */
  k_test_i3c_ch      = 0U,        /**< I3C channel under test.     */
  k_test_i3c_ch_oor  = 1U,        /**< First invalid I3C channel.  */
  k_test_bus_hz      = 400000U,   /**< Fast-mode bit-rate.         */
  k_test_pclk_hz     = 50000000U, /**< RIIC PCLKB source.          */
  k_test_pclka_hz    = 60000000U, /**< I3C PCLKA source.           */
  k_test_periph_7b   = 0x50U,     /**< 7-bit peripheral address.   */
  k_test_byte_a      = 0xA5U,     /**< First payload byte.         */
  k_test_byte_b      = 0x5AU,     /**< Second payload byte.        */
  k_test_rx_byte     = 0xC3U,     /**< Staged RIIC receive byte.   */
} ra8_io_i2c_bus_test_const_t;

/** @brief Two-byte transmit payload shared by the forwarding tests. */
static const uint8_t s_payload[2] = {(uint8_t)k_test_byte_a, (uint8_t)k_test_byte_b};

/**
 * @brief Reset the fake MMIO window and the MSTP model before a test.
 *
 * @details Mirrors the ``internal_prep`` helper in the wrapped drivers' tests: a
 * clean peripheral RAM image plus an initialized module-stop model.
 *
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All peripheral registers in the window read as zero.
 * @post The MSTP model is initialized.
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Bring RIIC channel 0 up and pre-arm ICSR2 so polls fall through.
 *
 * @details Sets TDRE / TEND / RDRF, the flags the polling controller
 * waits on; the start-of-transfer ``clear_status`` does not touch them,
 * so one pre-arm survives a whole transaction.
 *
 * @pre ``internal_prep`` ran (clean window + MSTP up).
 * @pre Channel 0 registers are mapped.
 * @post The channel is initialised (ICCR1.ICE set by the driver).
 * @post ICSR2 reads back with TDRE, TEND and RDRF set.
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bring_up_riic(void)
{
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_test_bus_hz,
    .pclkb_hz = (uint32_t)k_test_pclk_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_test_riic_ch, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_test_riic_ch);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
  reg->ICDRR = (uint8_t)k_test_rx_byte;
}

/**
 * @brief Bring the I3C block up in I2C-compat mode and pre-arm its flags.
 *
 * @details Sets NTST.TDBEF0 / NTST.RDBFF0 (buffer-ready) and BCST.BFREF
 * (bus free) so the driver's polling loops exit immediately -- the same
 * priming the ra8_i3c_i2c and ra8_touch tests use.
 *
 * @pre ``internal_prep`` ran (clean window + MSTP up).
 * @pre Channel 0 registers are mapped.
 * @post The channel is initialised in I2C-compatibility mode.
 * @post NTST holds both ready bits; BCST holds the free bit.
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bring_up_i3c_compat(void)
{
  const ra8_i3c_cfg_t cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_test_bus_hz,
    .pclka_hz = (uint32_t)k_test_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init((uint8_t)k_test_i3c_ch, &cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_test_i3c_ch);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/* =========================================================================
 * Dispatcher validation
 * =========================================================================
 */

/**
 * @test internal_test_null_and_unbound_rejected
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the dispatcher's
 * ``internal_validate`` is two sequential single-condition ``if`` checks;
 * this case exercises both legs on every public entry point) @brief Verify null and unbound rejected behavior. @details Executes the null and unbound rejected scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_null_and_unbound_rejected(void)
{
  TEST_BEGIN("ra8_io_i2c_bus: NULL handle + never-bound handle rejected");
  internal_prep();

  uint8_t           rx  = 0U;
  ra8_i2c_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_i2c_bus_write(nullptr, (uint8_t)k_test_periph_7b, s_payload, 1U, true));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_i2c_bus_read(nullptr, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_io_i2c_bus_transfer(nullptr, (uint8_t)k_test_periph_7b, s_payload, 1U, &rx, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_i2c_bus_as_ops(nullptr, &ops));

  const ra8_io_i2c_bus_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_i2c_bus_write(&unbound, (uint8_t)k_test_periph_7b, s_payload, 1U, true));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_i2c_bus_read(&unbound, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    ra8_io_i2c_bus_transfer(&unbound, (uint8_t)k_test_periph_7b, s_payload, 1U, &rx, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_i2c_bus_as_ops(&unbound, &ops));
  TEST_END("ra8_io_i2c_bus: NULL handle + never-bound handle rejected");
}

/**
 * @test internal_test_bind_validation
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- each binder runs one
 * NULL guard and one single-condition channel-range check) @brief Verify bind validation behavior. @details Executes the bind validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_bind_validation(void)
{
  TEST_BEGIN("ra8_io_i2c_bus: binder NULL / channel-range rejection");
  internal_prep();

  ra8_io_i2c_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_i2c_bus_bind_riic(nullptr, (uint8_t)k_test_riic_ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_i2c_bus_bind_i3c_compat(nullptr, (uint8_t)k_test_i3c_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_i2c_bus_bind_riic(&bus, (uint8_t)k_test_riic_ch_oor));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_i2c_bus_bind_i3c_compat(&bus, (uint8_t)k_test_i3c_ch_oor));
  /* A failed bind leaves the handle unbound. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_i2c_bus_read(&bus, (uint8_t)k_test_periph_7b, &rx, 1U));
  /* Good binds are accepted for both backends. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_riic(&bus, (uint8_t)k_test_riic_ch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_i3c_compat(&bus, (uint8_t)k_test_i3c_ch));
  TEST_END("ra8_io_i2c_bus: binder NULL / channel-range rejection");
}

/* =========================================================================
 * RIIC backend forwarding
 * =========================================================================
 */

/**
 * @test internal_test_riic_forwarding
 *
 * @par MC/DC:
 * (no compound decisions in the facade code under test -- every backend
 * row is a straight forwarding call; the wrapped driver's own compound
 * decisions carry their MC/DC vectors in test_ra8_i2c.c) @brief Verify riic forwarding behavior. @details Executes the riic forwarding scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_riic_forwarding(void)
{
  TEST_BEGIN("ra8_io_i2c_bus: RIIC write / read / transfer forward");
  internal_prep();
  internal_bring_up_riic();

  ra8_io_i2c_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_riic(&bus, (uint8_t)k_test_riic_ch));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_test_riic_ch);

  /* write with send_stop=true forwards the flag: the driver requests STOP
   * via ICCR2.SP, the exact register effect test_ra8_i2c.c pins. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_i2c_bus_write(&bus, (uint8_t)k_test_periph_7b, s_payload, sizeof(s_payload), true));
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);

  /* read drains the staged ICDRR byte and requests STOP. */
  reg->ICCR2 = 0U;
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_read(&bus, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(k_test_rx_byte, rx);
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);

  /* transfer runs write-then-RESTART-then-read in one transaction. */
  reg->ICDRR  = (uint8_t)k_test_rx_byte;
  uint8_t rd  = 0U;
  uint8_t cmd = (uint8_t)k_test_byte_a;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_i2c_bus_transfer(&bus, (uint8_t)k_test_periph_7b, &cmd, 1U, &rd, 1U));
  TEST_ASSERT_EQ(k_test_rx_byte, rd);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_deinit((uint8_t)k_test_riic_ch));
  TEST_END("ra8_io_i2c_bus: RIIC write / read / transfer forward");
}

/* =========================================================================
 * I3C I2C-compat backend forwarding
 * =========================================================================
 */

/**
 * @test internal_test_i3c_compat_forwarding
 *
 * @par MC/DC:
 * (no compound decisions in the facade code under test -- forwarding
 * rows only, including the single-expression ``!send_stop`` -> `restart`
 * inversion; the wrapped driver's decisions are covered in
 * test_ra8_i3c_i2c.c) @brief Verify i3c compat forwarding behavior. @details Executes the i3c compat forwarding scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_i3c_compat_forwarding(void)
{
  TEST_BEGIN("ra8_io_i2c_bus: I3C-compat write / read / transfer forward");
  internal_prep();
  internal_bring_up_i3c_compat();

  ra8_io_i2c_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_i3c_compat(&bus, (uint8_t)k_test_i3c_ch));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_test_i3c_ch);

  /* write with send_stop=true maps to restart=false: the driver ends the
   * transaction with a STOP condition (CNDCTL last write is SPCND). */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_i2c_bus_write(&bus, (uint8_t)k_test_periph_7b, s_payload, sizeof(s_payload), true));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, reg->CNDCTL);

  /* write with send_stop=false maps to restart=true: no STOP is issued,
   * the bus stays held for a follow-up transfer. */
  reg->CNDCTL = 0U;
  reg->NTST   = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST   = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_i2c_bus_write(&bus, (uint8_t)k_test_periph_7b, s_payload, sizeof(s_payload), false));
  TEST_ASSERT_EQ(0U, (reg->CNDCTL & (uint32_t)k_ra8_i3c_i2c_msk_cndctl_spcnd));

  /* read always releases the bus (restart=false inside the backend). */
  reg->NTST  = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST  = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_read(&bus, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, reg->CNDCTL);

  /* transfer runs the combined write-RESTART-read shape. */
  reg->NTST   = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST   = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  uint8_t cmd = (uint8_t)k_test_byte_a;
  uint8_t rd  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_i2c_bus_transfer(&bus, (uint8_t)k_test_periph_7b, &cmd, 1U, &rd, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_deinit((uint8_t)k_test_i3c_ch));
  TEST_END("ra8_io_i2c_bus: I3C-compat write / read / transfer forward");
}

/* =========================================================================
 * Ring-3 seam bridge
 * =========================================================================
 */

/**
 * @test internal_test_as_ops_bridge
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the bridge validates
 * with the same two sequential single-condition checks as the dispatcher
 * plus one NULL guard on the output, and each trampoline adds a single
 * NULL-cookie guard) @brief Verify as ops bridge behavior. @details Executes the as ops bridge scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_as_ops_bridge(void)
{
  TEST_BEGIN("ra8_io_i2c_bus_as_ops: seam trampolines forward + guard");
  internal_prep();
  internal_bring_up_riic();

  ra8_io_i2c_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_riic(&bus, (uint8_t)k_test_riic_ch));

  ra8_i2c_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_i2c_bus_as_ops(&bus, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_as_ops(&bus, &ops));
  TEST_ASSERT(ops.write != nullptr);
  TEST_ASSERT(ops.read != nullptr);
  TEST_ASSERT(ops.transfer != nullptr);
  TEST_ASSERT(ops.ctx == (void*)&bus);

  /* Drive each row through the seam exactly as a Ring-3 driver would. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ops.write(ops.ctx, (uint8_t)k_test_periph_7b, s_payload, sizeof(s_payload), true));
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ops.read(ops.ctx, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(k_test_rx_byte, rx);
  uint8_t cmd = (uint8_t)k_test_byte_a;
  uint8_t rd  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ops.transfer(ops.ctx, (uint8_t)k_test_periph_7b, &cmd, 1U, &rd, 1U));

  /* Every trampoline rejects a NULL cookie instead of dereferencing it. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ops.write(nullptr, (uint8_t)k_test_periph_7b, s_payload, 1U, true));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ops.read(nullptr, (uint8_t)k_test_periph_7b, &rx, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ops.transfer(nullptr, (uint8_t)k_test_periph_7b, &cmd, 1U, &rd, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_deinit((uint8_t)k_test_riic_ch));
  TEST_END("ra8_io_i2c_bus_as_ops: seam trampolines forward + guard");
}

int32_t main(void)
{
  internal_test_null_and_unbound_rejected();
  internal_test_bind_validation();
  internal_test_riic_forwarding();
  internal_test_i3c_compat_forwarding();
  internal_test_as_ops_bridge();
  return 0;
}

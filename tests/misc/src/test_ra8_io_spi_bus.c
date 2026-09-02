/**
 * @file test_ra8_io_spi_bus.c
 * @brief Unit tests for the ra8_io SPI-bus facade
 *        (libs/ra8_io/src/ra8_io_spi_bus*.c).
 *
 * @details
 * Binds both backends of the facade -- SPI_B (`ra8_io_spi_bus_bind_spi_b`)
 * and SCI Simple-SPI (`ra8_io_spi_bus_bind_sci_spi`) -- against the
 * ``ra8_fake_mmap`` register substrate and drives the vtable end to end:
 *
 *  - dispatcher validation: NULL handle and never-bound handle rejected
 *    on every public entry point;
 *  - binder validation: NULL handle and out-of-range channels rejected;
 *  - forwarding: `xfer8` / `write_read` / `set_clock` land in the real
 *    `ra8_spi_*` / `ra8_sci_spi_*` drivers, observed through the same
 *    register effects the drivers' own tests assert (TDR wire bytes, RDR
 *    capture, SPCR3.SPBR / CCR2 divider writes);
 *  - the SCI backend's 8-bit-only `write_read` width guard;
 *  - the `ra8_io_spi_bus_as_ops` bridge into the Ring-3 seam, including
 *    its trampoline's NULL-cookie guard.
 *
 * Determinism mirrors the sibling driver tests: status flags (SPI_B SPSR,
 * SCI CSR) are pre-seeded so the drivers' bounded polls fall through on
 * the first read; no timers and no signal injection are used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_sci_spi.h"
#include "ra8_io_spi_bus_spi_b.h"
#include "ra8_mstp.h"
#include "ra8_sci_regs.h"
#include "ra8_sci_spi.h"
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_io_spi_bus_test_const_t
 * @brief Channels, rates and wire bytes used across the tests.
 */
typedef enum : uint32_t {
  k_test_spi_b_ch     = 0U,         /**< SPI_B channel under test.         */
  k_test_spi_b_ch_oor = 2U,         /**< First invalid SPI_B channel.      */
  k_test_sci_ch       = 0U,         /**< SCI channel under test.           */
  k_test_sci_ch_oor   = 10U,        /**< First invalid SCI channel.        */
  k_test_baud_hz      = 1000000U,   /**< 1 MHz bit-rate.                   */
  k_test_pclk_hz      = 100000000U, /**< 100 MHz peripheral clock.         */
  k_test_tx_byte      = 0xA5U,      /**< Byte shifted out on COPI.         */
  k_test_rx_byte      = 0x5AU,      /**< Byte the fake returns in SCI RDR. */
  k_test_multi_len    = 3U,         /**< Frames in the bulk transfers.     */
} ra8_io_spi_bus_test_const_t;

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
 * @brief Bring SPI_B channel 0 up and pre-seed SPSR so polls fall through.
 *
 * @details The SPI_B driver's fake wait checks SPSR.SPTEF / SPSR.SPRF; the
 * fake never asserts them, so seed both once (SPSRC clears land in a
 * separate RAM word and do not consume the seed).
 *
 * @pre ``internal_prep`` ran (clean window + MSTP up).
 * @pre Channel 0 registers are mapped.
 * @post The channel is initialised in mode 0 at the test bit-rate.
 * @post SPSR reads back with SPTEF and SPRF set.
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bring_up_spi_b(void)
{
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclk_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_spi_b_ch, &cfg));
  ra8_spi((uint8_t)k_test_spi_b_ch)->SPSR =
    (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
}

/**
 * @brief Bring SCI channel 0 up in Simple-SPI mode and pre-seed CSR + RDR.
 *
 * @details Seeds CSR.TDRE / CSR.RDRF so the per-frame polls fall through,
 * and stages ::k_test_rx_byte in RDR so received bytes are observable.
 *
 * @pre ``internal_prep`` ran (clean window + MSTP up).
 * @pre Channel 0 registers are mapped.
 * @post The channel is enabled (TE+RE) in Simple-SPI mode.
 * @post CSR reads back ready and RDR holds the staged byte.
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bring_up_sci_spi(void)
{
  const ra8_sci_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclk_hz   = (uint32_t)k_test_pclk_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_test_sci_ch, &cfg));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_test_sci_ch);
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  reg->RDR = (uint32_t)k_test_rx_byte;
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
  TEST_BEGIN("ra8_io_spi_bus: NULL handle + never-bound handle rejected");
  internal_prep();

  uint8_t           rx  = 0U;
  ra8_spi_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_spi_bus_xfer8(nullptr, (uint8_t)k_test_tx_byte, &rx));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_spi_bus_write_read(nullptr, &rx, &rx, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_io_spi_bus_set_clock(nullptr, (uint32_t)k_test_baud_hz, (uint32_t)k_test_pclk_hz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_spi_bus_as_ops(nullptr, &ops));

  const ra8_io_spi_bus_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_spi_bus_xfer8(&unbound, (uint8_t)k_test_tx_byte, &rx));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_spi_bus_write_read(&unbound, &rx, &rx, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    ra8_io_spi_bus_set_clock(&unbound, (uint32_t)k_test_baud_hz, (uint32_t)k_test_pclk_hz));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_spi_bus_as_ops(&unbound, &ops));
  TEST_END("ra8_io_spi_bus: NULL handle + never-bound handle rejected");
}

/**
 * @test internal_test_bind_validation
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- each binder runs one
 * NULL guard and one single-condition channel-range check) @brief Verify bind validation behavior. @details Executes the bind validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_bind_validation(void)
{
  TEST_BEGIN("ra8_io_spi_bus: binder NULL / channel-range rejection");
  internal_prep();

  ra8_io_spi_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_spi_bus_bind_spi_b(nullptr, (uint8_t)k_test_spi_b_ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_spi_bus_bind_sci_spi(nullptr, (uint8_t)k_test_sci_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_spi_bus_bind_spi_b(&bus, (uint8_t)k_test_spi_b_ch_oor));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_spi_bus_bind_sci_spi(&bus, (uint8_t)k_test_sci_ch_oor));
  /* A failed bind leaves the handle unbound. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_io_spi_bus_xfer8(&bus, (uint8_t)k_test_tx_byte, &rx));
  /* Good binds are accepted for both backends. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_spi_b(&bus, (uint8_t)k_test_spi_b_ch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_sci_spi(&bus, (uint8_t)k_test_sci_ch));
  TEST_END("ra8_io_spi_bus: binder NULL / channel-range rejection");
}

/* =========================================================================
 * SPI_B backend forwarding
 * =========================================================================
 */

/**
 * @test internal_test_spi_b_forwarding
 *
 * @par MC/DC:
 * (no compound decisions in the facade code under test -- every backend
 * row is a straight forwarding call; the wrapped driver's own compound
 * decisions carry their MC/DC vectors in test_ra8_spi_b.c) @brief Verify spi b forwarding behavior. @details Executes the spi b forwarding scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_spi_b_forwarding(void)
{
  TEST_BEGIN("ra8_io_spi_bus: SPI_B xfer8 / write_read / set_clock forward");
  internal_prep();
  internal_bring_up_spi_b();

  ra8_io_spi_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_spi_b(&bus, (uint8_t)k_test_spi_b_ch));

  /* xfer8: the fake SPDR is one RAM word, so the driver echoes tx as rx --
   * seeing the tx byte back proves the call reached ra8_spi_xfer8. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_xfer8(&bus, (uint8_t)k_test_tx_byte, &rx));
  TEST_ASSERT_EQ(k_test_tx_byte, rx);
  /* rx may be NULL to discard. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_xfer8(&bus, (uint8_t)k_test_tx_byte, nullptr));

  /* write_read: a full-duplex multi-frame exchange completes. */
  ra8_spi((uint8_t)k_test_spi_b_ch)->SPSR =
    (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;
  const uint8_t tx_buf[k_test_multi_len] = {0x11U, 0x22U, 0x33U};
  uint8_t       rx_buf[k_test_multi_len] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_spi_bus_write_read(&bus, tx_buf, rx_buf, (uint32_t)k_test_multi_len, k_ra8_spi_width_8));
  /* Both buffers NULL is the driver's own rejection, forwarded verbatim. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_spi_bus_write_read(&bus, nullptr, nullptr, 1U, k_ra8_spi_width_8));

  /* set_clock: clear SPCR3 so the reprogramme is observable. */
  ra8_spi((uint8_t)k_test_spi_b_ch)->SPCR3 = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_spi_bus_set_clock(&bus, (uint32_t)k_test_baud_hz, (uint32_t)k_test_pclk_hz));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_spi_b_ch)->SPCR3) & k_ra8_spcr3_mask_spbr) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_spi_b_ch));
  TEST_END("ra8_io_spi_bus: SPI_B xfer8 / write_read / set_clock forward");
}

/* =========================================================================
 * SCI Simple-SPI backend forwarding
 * =========================================================================
 */

/**
 * @test internal_test_sci_spi_forwarding
 *
 * @par MC/DC:
 * (no compound decisions in the facade code under test -- forwarding
 * rows only; the wrapped driver's decisions are covered in
 * test_ra8_sci_spi.c) @brief Verify sci spi forwarding behavior. @details Executes the sci spi forwarding scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_sci_spi_forwarding(void)
{
  TEST_BEGIN("ra8_io_spi_bus: SCI xfer8 / write_read / set_clock forward");
  internal_prep();
  internal_bring_up_sci_spi();

  ra8_io_spi_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_sci_spi(&bus, (uint8_t)k_test_sci_ch));

  /* xfer8: the transmitted byte lands in TDR, the staged RDR byte returns. */
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_test_sci_ch);
  uint8_t                rx  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_xfer8(&bus, (uint8_t)k_test_tx_byte, &rx));
  TEST_ASSERT_EQ(k_test_tx_byte, (reg->TDR & 0xFFU));
  TEST_ASSERT_EQ(k_test_rx_byte, rx);

  /* write_read at 8-bit width forwards to ra8_sci_spi_xfer. */
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  reg->RDR = (uint32_t)k_test_rx_byte;
  const uint8_t tx_buf[k_test_multi_len] = {0x44U, 0x55U, 0x66U};
  uint8_t       rx_buf[k_test_multi_len] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_spi_bus_write_read(&bus, tx_buf, rx_buf, (uint32_t)k_test_multi_len, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(0x66U, (reg->TDR & 0xFFU));
  TEST_ASSERT_EQ(k_test_rx_byte, rx_buf[0]);

  /* set_clock: clear CCR2 so the divider write is observable. */
  reg->CCR2 = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_spi_bus_set_clock(&bus, (uint32_t)k_test_baud_hz, (uint32_t)k_test_pclk_hz));
  TEST_ASSERT(reg->CCR2 != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_deinit((uint8_t)k_test_sci_ch));
  TEST_END("ra8_io_spi_bus: SCI xfer8 / write_read / set_clock forward");
}

/**
 * @test internal_test_sci_spi_width_guard
 *
 * @par MC/DC:
 * Decision: ``if (width != k_ra8_spi_width_8)`` in the SCI backend's
 * ``write_read`` row (1 condition). N+1 = 2 vectors:
 * - V1: width=16 -> true  -> k_ra8_err_not_supported (no transfer starts)
 * - V2: width=8  -> false -> forwards to ra8_sci_spi_xfer and succeeds @brief Verify sci spi width guard behavior. @details Executes the sci spi width guard scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_sci_spi_width_guard(void)
{
  TEST_BEGIN("ra8_io_spi_bus: SCI backend rejects non-8-bit widths");
  internal_prep();
  internal_bring_up_sci_spi();

  ra8_io_spi_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_sci_spi(&bus, (uint8_t)k_test_sci_ch));

  uint8_t buf[k_test_multi_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_spi_bus_write_read(&bus, buf, buf, 1U, k_ra8_spi_width_16));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_spi_bus_write_read(&bus, buf, buf, 1U, k_ra8_spi_width_32));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_write_read(&bus, buf, buf, 1U, k_ra8_spi_width_8));
  TEST_END("ra8_io_spi_bus: SCI backend rejects non-8-bit widths");
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
 * plus one NULL guard on the output, and the trampoline adds a single
 * NULL-cookie guard) @brief Verify as ops bridge behavior. @details Executes the as ops bridge scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_as_ops_bridge(void)
{
  TEST_BEGIN("ra8_io_spi_bus_as_ops: seam trampoline forwards + guards");
  internal_prep();
  internal_bring_up_spi_b();

  ra8_io_spi_bus_t bus = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_bind_spi_b(&bus, (uint8_t)k_test_spi_b_ch));

  ra8_spi_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_spi_bus_as_ops(&bus, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_spi_bus_as_ops(&bus, &ops));
  TEST_ASSERT(ops.xfer8 != nullptr);
  TEST_ASSERT(ops.ctx == (void*)&bus);

  /* Drive a byte through the seam exactly as a Ring-3 driver would. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ops.xfer8(ops.ctx, (uint8_t)k_test_tx_byte, &rx));
  TEST_ASSERT_EQ(k_test_tx_byte, rx);

  /* The trampoline rejects a NULL cookie instead of dereferencing it. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ops.xfer8(nullptr, (uint8_t)k_test_tx_byte, &rx));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_spi_b_ch));
  TEST_END("ra8_io_spi_bus_as_ops: seam trampoline forwards + guards");
}

int main(void)
{
  internal_test_null_and_unbound_rejected();
  internal_test_bind_validation();
  internal_test_spi_b_forwarding();
  internal_test_sci_spi_forwarding();
  internal_test_sci_spi_width_guard();
  internal_test_as_ops_bridge();
  return 0;
}

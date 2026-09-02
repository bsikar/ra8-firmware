/**
 * @file test_ra8_board_ek_ra8d2_console_stream.c
 * @brief Unit tests for the EK-RA8D2 console -> ra8_io_stream binding
 *
 * @details
 * Covers every line and both directions of every decision in
 * ``libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_console_stream.c`` plus the
 * console-readiness predicate it consults in the comms
 * translation unit:
 *
 *   - the null-handle guard (``RA8_CHECK_NULL_PTR``), both arms;
 *   - the console-readiness guard, both arms -- which is why the
 *     "console not up" test must run BEFORE any successful console
 *     bring-up: ``s_uart_console_initialized`` is module state that never
 *     goes back to false;
 *   - the bind itself, proved not merely to return ok but to aim at the
 *     board's console channel: a byte pushed through the returned stream is
 *     observed in SCI8's transmit data register.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_console_stream.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_io_stream.h"
#include "ra8_pin_validator.h"
#include "ra8_sci_regs.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum board_console_stream_fixture_t
 * @brief Fixed inputs the console-stream tests need, named so no numeric
 *        literal appears in a test body.
 */
typedef enum : uint32_t {
  k_cs_oscsf_all_ready = 0xFFU,        /**< Every oscillator-stabilisation flag set, so
                                     *   ra8_cgc_init sees each source ready at once. */
  k_cs_csr_all_set     = 0xFFFFFFFFUL, /**< TDRE + TEND (and every other CSR latch) set, so
                                     *   the polled transmit path completes on the host. */
  k_cs_console_baud    = 115200UL,     /**< Console line rate used for bring-up.           */
  k_cs_tdr_data_mask   = 0xFFUL,       /**< TDAT[7:0] field of the transmit data register. */
} board_console_stream_fixture_t;

/**
 * @brief Payload pushed through the bound stream; its last byte is the value
 *        the transmit data register must hold afterwards.
 */
static const char* const s_cs_payload = "ok";

/**
 * @brief Expected transmit-register residue after ::s_cs_payload is written.
 */
typedef enum : uint8_t {
  k_cs_payload_last_byte = (uint8_t)'k', /**< Last character of ::s_cs_payload. */
} board_console_stream_payload_t;

/**
 * @brief Clear every fake peripheral window and free all claimed pins.
 *
 * @details
 * Mirrors the reset helper the sibling BSP tests use: zeroes the fake register
 * windows, disarms any armed MMIO wait fault, and releases pin ownership so
 * one test's pin claims cannot make the next test's routing fail.
 *
 * @pre None.
 * @post Fake register windows are zeroed and the pin bitmap is clear.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Bring the board console up on the fake peripherals.
 *
 * @details
 * Seeds OSCSF so every CGC oscillator spin-loop settles on its first
 * iteration, runs ``ra8_cgc_init`` to publish a post-PLL PCLKA (the console
 * refuses a MOCO-rate clock), then initialises the console itself.
 *
 * @pre ::internal_reset_state has been called.
 * @post ``priv_ra8_board_uart_console_is_up()`` reports true.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bring_console_up(void)
{
  *ra8_sys_oscsf() = (uint8_t)k_cs_oscsf_all_ready;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_init((uint32_t)k_cs_console_baud));
}

/**
 * @test board_console_stream_rejects_null_handle
 *
 * @brief Verify a null stream handle is refused before anything is bound.
 *
 * @return Nothing.
 * @pre The console-stream unit is linked into the test binary.
 * @post No module state is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((out) == nullptr)` inside `RA8_CHECK_NULL_PTR` (1 condition).
 * - Vector 1: out=NULL  -> true  (this test)
 * - Vector 2: out=&stream -> false (the two tests below)
 * N+1 = 2 vectors for N=1: minimal MC/DC, and the pair proves the handle
 * pointer alone controls the return.
 */
RA8_INTERNAL static void internal_test_rejects_null_handle(void)
{
  TEST_BEGIN("board_console_stream: null handle rejected");
  internal_reset_state();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_console_stream(nullptr));
  TEST_END("board_console_stream: null handle rejected");
}

/**
 * @test board_console_stream_requires_console_up
 *
 * @brief Verify binding is refused while the console has never come up.
 *
 * @details
 * MUST run before any test that brings the console up: the readiness flag is
 * module state in the comms unit and is never cleared, so this is the only
 * point in the binary at which the false arm is reachable.
 *
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` has not yet succeeded in this process.
 * @post The caller's handle is left unbound.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if (!priv_ra8_board_uart_console_is_up())` (1 condition).
 * - Vector 1: console down -> true  (this test)
 * - Vector 2: console up   -> false (the bind test below)
 * N+1 = 2 vectors for N=1: minimal MC/DC. The pair proves the readiness
 * predicate alone decides between not_initialized and a bind.
 */
RA8_INTERNAL static void internal_test_requires_console_up(void)
{
  TEST_BEGIN("board_console_stream: refuses to bind a console that is down");
  internal_reset_state();
  ra8_io_stream_t stream = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_console_stream(&stream));
  TEST_ASSERT(stream.iface == nullptr);
  TEST_END("board_console_stream: refuses to bind a console that is down");
}

/**
 * @test board_console_stream_binds_the_board_console
 *
 * @brief Verify a successful bind aims the stream at the board's own console.
 *
 * @details
 * Returning ::k_ra8_ok is not the claim under test -- the claim is that the
 * bound sink is SCI8, the channel the J-Link OB VCOM bridge sits on. So the
 * test writes through the stream with the transmit-status latches pre-seeded
 * (the polled path spins on TDRE then TEND) and reads the byte back out of
 * SCI8's transmit data register. A binding aimed at any other channel would
 * leave SCI8's TDR at zero.
 *
 * @return Nothing.
 * @pre ::internal_test_requires_console_up has already run.
 * @post SCI8's transmit data register holds the payload's last byte.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Supplies the false vector for both of the unit's decisions: a non-null
 * handle for the null guard, and a live console for the readiness guard. Paired
 * with the two tests above, each condition is shown to independently flip the
 * outcome.
 */
RA8_INTERNAL static void internal_test_binds_the_board_console(void)
{
  TEST_BEGIN("board_console_stream: bound sink reaches the console channel");
  internal_reset_state();
  internal_bring_console_up();

  ra8_io_stream_t stream = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_console_stream(&stream));
  TEST_ASSERT(stream.iface != nullptr);

  volatile r_sci_regs_t* console = ra8_sci((uint8_t)k_ra8_board_uart_console_sci_channel);
  /* HUM Ch 38.2.17 "CSR : Common Status Register" p 2225 -- the polled
   * transmit path spins on TDRE then TEND; seed both so it completes. */
  console->CSR = (uint32_t)k_cs_csr_all_set;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_puts(&stream, s_cs_payload));
  /* HUM Ch 38.2.3 "TDR : Transmit Data Register" p 2181 -- TDAT[7:0] holds
   * the last frame the driver launched. */
  TEST_ASSERT_EQ(k_cs_payload_last_byte, console->TDR & (uint32_t)k_cs_tdr_data_mask);
  TEST_END("board_console_stream: bound sink reaches the console channel");
}

/**
 * @brief Test binary entry point.
 *
 * @details
 * Ordering is load-bearing: the "console is down" test must run before the
 * bind test, because the console readiness flag is module state that only ever
 * goes from false to true.
 *
 * @return 0 on success; a failing assertion exits before this returns.
 *
 * @pre The fake register windows are mapped by the test framework.
 * @post Every decision in the console-stream unit has been driven both ways.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_rejects_null_handle();
  internal_test_requires_console_up();
  internal_test_binds_the_board_console();
  return 0;
}

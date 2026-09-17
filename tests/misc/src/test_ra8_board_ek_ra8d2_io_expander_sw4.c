/**
 * @file test_ra8_board_ek_ra8d2_io_expander_sw4.c
 * @brief Host tests for the exact-byte U15 SW4-override entry point (#970)
 *
 * @details
 * ``ra8_board_io_expander_apply_sw4`` was the only one of the five public
 * U15 SW4-override entry points with no consumer anywhere in the tree: no
 * app, no example, no port, no coprocessor and no test ever called it.  Its
 * siblings are all driven -- ``ra8_board_io_expander_apply_sw4_mask`` by the
 * camera routing in ``ra8_board_ek_ra8d2_camera.c``, and the four fixed-byte
 * entry points by ``test_ra8_board_ek_ra8d2_audio_usb_cov.c`` -- so only the
 * caller-supplied-byte path went unexercised.
 *
 * That path is the one worth pinning down, because it is the only entry
 * point whose output byte is *not* a compile-time board constant:
 *
 *   - nothing proved the caller's byte reaches U15's OUTPUT register rather
 *     than one of the fixed layouts (0xFF all-high, 0xF2 project default,
 *     0xF8 octo-SPI, 0x72 USBHS host);
 *   - nothing crossed the all-outputs IODIR path that separates it from
 *     ``ra8_board_io_expander_apply_sw4_mask``, whose only production caller
 *     passes a fixed byte *and* a fixed mask.
 *
 * Both are observable from the host through the RIIC1 transmit register,
 * because every U15 register write ends with its payload byte in ICDRT:
 *
 *   - Byte leg.  ICSR2 is pre-armed with TDRE but not TEND, so the first
 *     U15 write (register 0x05, OUTPUT) drains its address and both payload
 *     bytes into ICDRT and then times out waiting for TEND inside
 *     ``ra8_i2c_write``.  The U15 programming sequence returns at its first
 *     error check, so ICDRT still holds the OUTPUT register value.
 *   - IODIR leg.  With TDRE *and* TEND pre-armed all three U15 writes ACK
 *     (OUTPUT, HIZ, IODIR), so the sequence completes and ICDRT retains the
 *     value of the last write, IODIR.
 *
 * Kept in its own file rather than folded into the audio + USB coverage
 * companion: this is a gap-closing test for one entry point, and the shared
 * file's own header still describes "the four public SW4-override entry
 * points".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i2c_regs.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

/**
 * @brief Local sentinels for the U15 SW4 exact-byte cases.
 *
 * @details
 * ``k_test_u15_sw4_probe_byte`` matches none of the four board constants the
 * fixed-byte entry points hard-code, so a regression that substituted a
 * board default for the caller's byte cannot pass this test.
 * ``k_test_u15_iodir_all_outputs`` mirrors
 * ``k_ra8_board_pi4ioe_iodir_all_outputs``, which is file-local to
 * ra8_board_ek_ra8d2_audio_usb.c and so cannot be referenced by name here.
 */
typedef enum : uint8_t {
  k_test_u15_riic_channel      = 1U,    /**< RIIC1 carries the U15 expander.          */
  k_test_u15_sw4_probe_byte    = 0x5AU, /**< Caller byte; no board constant matches.  */
  k_test_u15_iodir_all_outputs = 0xFFU, /**< IODIR value apply_sw4 hard-codes.        */
} test_u15_sw4_sentinel_t;

/**
 * @brief Reset all fake peripheral state and pin ownership.
 *
 * @details Zeroes every hardware register window (ra8_fake_mmap_reset) and
 * frees all claimed pins (ra8_pin_validator_reset), so the pins the U15
 * bring-up claims on one leg cannot fail the next leg as a conflict.
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @post Register windows cleared; pin-validator bitmap zeroed.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * 1. ra8_board_io_expander_apply_sw4 -- the caller's byte reaches OUTPUT
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify the caller's byte is what gets written to U15's OUTPUT reg.
 *
 * @details
 * Pre-arming RIIC1 ICSR2 with TDRE but not TEND lets the OUTPUT write drain
 * its address byte and both payload bytes into ICDRT, then stall on the
 * TEND wait, so the write returns hw_timeout and the U15 programming
 * sequence stops before the HIZ and IODIR writes.  ICDRT therefore still
 * holds the OUTPUT register value, which must be the caller's byte.
 *
 * @par MC/DC:
 * The decisions crossed are the single-condition ``if (err != k_ra8_ok)``
 * checks in the U15 programming sequence and the shared apply helper; this
 * leg supplies the taken vector for the first of them.  The all-not-taken
 * vector is supplied by the IODIR test below.
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @pre Clean fake state, then RIIC1 ICSR2 = TDRE.
 * @post The expected status and hardware-visible effects are asserted.
 * @post ICDRT holds the OUTPUT byte the caller passed.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_sw4_writes_caller_byte(void)
{
  TEST_BEGIN("apply_sw4 writes the caller's byte to the U15 OUTPUT register");
  internal_reset_state();

  volatile r_i2c_regs_t* riic1 = ra8_i2c_regs((uint8_t)k_test_u15_riic_channel);
  /* TDRE only: the payload drains, then the TEND wait times out. */
  riic1->ICSR2 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;

  const ra8_err_t err = ra8_board_io_expander_apply_sw4((uint8_t)k_test_u15_sw4_probe_byte);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_ASSERT_EQ((uint8_t)k_test_u15_sw4_probe_byte, riic1->ICDRT);

  TEST_END("apply_sw4 writes the caller's byte to the U15 OUTPUT register");
}

/* -------------------------------------------------------------------------
 * 2. ra8_board_io_expander_apply_sw4 -- IODIR is forced all-outputs
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify apply_sw4 closes the sequence with an all-outputs IODIR.
 *
 * @details
 * With TDRE and TEND both pre-armed every U15 register write ACKs, so the
 * programming sequence runs OUTPUT, HIZ and IODIR and the entry point
 * returns k_ra8_ok.  ICDRT then holds the payload of the last write, IODIR,
 * which apply_sw4 hard-codes to all-outputs: that is what distinguishes it
 * from ra8_board_io_expander_apply_sw4_mask, where the mask is the caller's.
 *
 * @par MC/DC:
 * Supplies the all-not-taken (success) vectors for the
 * ``if (err != k_ra8_ok)`` checks in the U15 programming sequence and the
 * shared apply helper; the taken vector is supplied by the test above.
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @pre Clean fake state, then RIIC1 ICSR2 = TDRE | TEND.
 * @post The expected status and hardware-visible effects are asserted.
 * @post ICDRT holds the all-outputs IODIR value.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_sw4_forces_all_outputs(void)
{
  TEST_BEGIN("apply_sw4 programs U15 IODIR to all outputs");
  internal_reset_state();

  volatile r_i2c_regs_t* riic1 = ra8_i2c_regs((uint8_t)k_test_u15_riic_channel);
  /* TDRE + TEND: OUTPUT, HIZ and IODIR all ACK, so IODIR lands last. */
  riic1->ICSR2 =
    (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend);

  const ra8_err_t err = ra8_board_io_expander_apply_sw4((uint8_t)k_test_u15_sw4_probe_byte);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ((uint8_t)k_test_u15_iodir_all_outputs, riic1->ICDRT);

  TEST_END("apply_sw4 programs U15 IODIR to all outputs");
}

/* -------------------------------------------------------------------------
 * 3. ra8_board_io_expander_apply_sw4 -- delegation error propagation
 * -------------------------------------------------------------------------
 */

/**
 * @brief Verify apply_sw4 propagates a U15 write failure to its caller.
 *
 * @details
 * With RIIC1 left un-armed the first U15 write never sees TDRE, so the
 * write times out and the entry point returns hw_timeout rather than
 * reporting success: the delegation leg the four fixed-byte entry points
 * already have covered in the audio + USB companion test.
 *
 * @par MC/DC:
 * (no compound decisions in the delegation line itself)
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @pre Clean fake state; RIIC1 status left un-armed.
 * @post The expected status and hardware-visible effects are asserted.
 * @post The entry point returned the underlying write error.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_sw4_propagates_write_error(void)
{
  TEST_BEGIN("apply_sw4 propagates a U15 write timeout");
  internal_reset_state();
  const ra8_err_t err = ra8_board_io_expander_apply_sw4((uint8_t)k_test_u15_sw4_probe_byte);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("apply_sw4 propagates a U15 write timeout");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 *
 * @details Runs the three exact-byte SW4-override cases in sequence.
 * Returns 0 on success; any TEST_ASSERT failure calls exit(1) before this
 * function returns.  Each case resets the fake state first, so ordering
 * cannot create a false pin conflict.
 *
 * @return 0 on success.
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @post The expected status and hardware-visible effects are asserted.
 *
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_apply_sw4_writes_caller_byte();
  internal_test_apply_sw4_forces_all_outputs();
  internal_test_apply_sw4_propagates_write_error();
  return 0;
}

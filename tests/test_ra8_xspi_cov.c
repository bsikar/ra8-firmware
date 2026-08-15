/**
 * @file test_ra8_xspi_cov.c
 * @brief Supplemental host coverage for the xSPI XIP enter/exit surface.
 *
 * @details
 * The primary suite (``test_ra8_xspi.c``) covers init, direct-command,
 * lifecycle, DTR, DQS-calibration, suspend/resume and software-reset, but
 * never exercises the memory-mapped execute-in-place bring-up entry points
 * ``ra8_xspi_xip_enter()`` / ``ra8_xspi_xip_exit()`` -- leaving those two
 * functions' bodies (the code-word packing, the BMCTL0 read-only/read-write
 * toggle, and the CMCTLCH XIPEN arm/disarm) fully uncovered. This file drives
 * both entry points on their happy path and their null-instance rejection
 * path so every reachable statement in them executes.
 *
 * @note These are pure register-poke functions with no compound decisions:
 *       the only branch is the single-condition ``RA8_CHECK_NULL_PTR`` guard,
 *       which each test exercises from both sides (valid + out-of-range
 *       instance). No ``&&`` / ``||`` appears in the code under test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi.h"
#include "unity_minimal.h"

/**
 * @enum test_xspi_cov_inst_t
 * @brief Instance selectors used by the XIP coverage tests.
 */
typedef enum : uint8_t {
  k_test_xspi_cov_inst0 = 0U,  /**< Valid XSPI0 instance.                        */
  k_test_xspi_cov_bad   = 99U, /**< Out-of-range instance (ra8_xspi -> nullptr). */
} test_xspi_cov_inst_t;

/**
 * @enum test_xspi_cov_code_t
 * @brief XIP enter/exit opcode fixtures written into CMCTLCH.
 *
 * @details
 * Two distinct, non-symmetric bytes so the packed ``code_word`` is
 * position-sensitive: an accidental swap of the XIPENCODE / XIPEXCODE
 * fields would change the expected value and fail the assertion.
 */
typedef enum : uint8_t {
  k_test_xspi_cov_enter_code = 0xA5U, /**< XIPENCODE[7..0]  enter byte. */
  k_test_xspi_cov_exit_code  = 0x5AU, /**< XIPEXCODE[15..8] exit byte.  */
} test_xspi_cov_code_t;

/**
 * @brief Bring the fake mmap + MSTP back to a known state and open XSPI0.
 *
 * @details
 * Mirrors the ``prep_w51`` pattern in ``test_ra8_xspi.c``: reset the fake
 * peripheral RAM, (re)initialise the module-stop shadow, then run
 * ``ra8_xspi_init`` so the register window is gated open and the XIP entry
 * points operate against a realistic post-init state. The clock-block
 * handshake helper is idempotent, so re-running init across tests is safe.
 *
 * @pre The host fake mmap window is available (linked ra8_fake_mmap mock).
 * @pre No other thread touches the xSPI registers (single-threaded test).
 * @post XSPI0 is initialised and its register window is writable.
 * @post ``ra8_xspi(k_test_xspi_cov_inst0)`` returns a non-null pointer.
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
static void prep_xip(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_cov_inst0, k_ra8_xspi_lio_1s8s8s));
}

/**
 * @test test_xip_enter_happy
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- ``ra8_xspi_xip_enter``
 * packs the enter/exit codes and pokes BMCTL0 + CMCTLCH[0/1]; its only
 * branch is the single-condition ``RA8_CHECK_NULL_PTR`` guard, driven
 * false here by a valid instance and true by ::test_xip_enter_bad_instance.)
 */
static void test_xip_enter_happy(void)
{
  TEST_BEGIN("ra8_xspi_xip_enter arms XIPEN + read-only map");
  prep_xip();

  const uint32_t code_word =
    ((uint32_t)k_test_xspi_cov_enter_code << k_ra8_xspi_cmctlch_xipencode_pos) |
    ((uint32_t)k_test_xspi_cov_exit_code << k_ra8_xspi_cmctlch_xipexcode_pos);
  const uint32_t expect_ch = code_word | (uint32_t)k_ra8_xspi_cmctlch_xipen_mask;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_xip_enter((uint8_t)k_test_xspi_cov_inst0,
                                    (uint8_t)k_test_xspi_cov_enter_code,
                                    (uint8_t)k_test_xspi_cov_exit_code));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_cov_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  /* BMCTL0 mapped read-only so the bus path cannot race the enter code. */
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_only, reg->BMCTL0);
  /* Both channels carry the packed enter/exit codes plus the XIPEN arm. */
  TEST_ASSERT_EQ(expect_ch, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(expect_ch, reg->CMCTLCH[1]);
  TEST_END("ra8_xspi_xip_enter arms XIPEN + read-only map");
}

/**
 * @test test_xip_enter_bad_instance
 *
 * @par MC/DC:
 * (single-condition ``RA8_CHECK_NULL_PTR(reg, ...)`` guard -- this vector
 * drives it true, i.e. ``reg == nullptr`` for an out-of-range instance,
 * taking the early null-ptr return; the false side is covered by
 * ::test_xip_enter_happy. No compound decision.)
 */
static void test_xip_enter_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_xip_enter rejects out-of-range instance");
  prep_xip();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_xip_enter((uint8_t)k_test_xspi_cov_bad,
                                    (uint8_t)k_test_xspi_cov_enter_code,
                                    (uint8_t)k_test_xspi_cov_exit_code));
  TEST_END("ra8_xspi_xip_enter rejects out-of-range instance");
}

/**
 * @test test_xip_exit_happy
 *
 * @par MC/DC:
 * (no compound decisions -- ``ra8_xspi_xip_exit`` clears CMCTLCH[0/1] and
 * restores BMCTL0 to read/write; its only branch is the single-condition
 * ``RA8_CHECK_NULL_PTR`` guard, false here for a valid instance and true in
 * ::test_xip_exit_bad_instance.)
 */
static void test_xip_exit_happy(void)
{
  TEST_BEGIN("ra8_xspi_xip_exit disarms XIPEN + restores read/write");
  prep_xip();

  /* Enter first so exit has non-zero XIPEN state to tear down. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_xip_enter((uint8_t)k_test_xspi_cov_inst0,
                                    (uint8_t)k_test_xspi_cov_enter_code,
                                    (uint8_t)k_test_xspi_cov_exit_code));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_xip_exit((uint8_t)k_test_xspi_cov_inst0));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_cov_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(0, reg->CMCTLCH[1]);
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_write, reg->BMCTL0);
  TEST_END("ra8_xspi_xip_exit disarms XIPEN + restores read/write");
}

/**
 * @test test_xip_exit_bad_instance
 *
 * @par MC/DC:
 * (single-condition ``RA8_CHECK_NULL_PTR(reg, ...)`` guard -- this vector
 * drives it true for an out-of-range instance; the false side is covered
 * by ::test_xip_exit_happy. No compound decision.)
 */
static void test_xip_exit_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_xip_exit rejects out-of-range instance");
  prep_xip();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_xip_exit((uint8_t)k_test_xspi_cov_bad));
  TEST_END("ra8_xspi_xip_exit rejects out-of-range instance");
}

int32_t main(void)
{
  test_xip_enter_happy();
  test_xip_enter_bad_instance();
  test_xip_exit_happy();
  test_xip_exit_bad_instance();
  return 0;
}

/**
 * @file test_ra8_nsc_xspi.c
 * @brief MC/DC unit test for libs/ra8_nsc/src/ra8_nsc_xspi.c
 *
 * @details
 * Targets the single 2-condition compound decision identified in
 * docs/MCDC_GAPS.csv at libs/ra8_nsc/src/ra8_nsc_xspi.c
 * (``ra8_nsc_xspi_read`` length validator).
 *
 * The veneer body is short:
 * @code
 *   if ((len == 0U) || (len > k_ra8_nsc_xspi_max_read)) {
 *       return k_ra8_err_invalid_arg;
 *   }
 *   RA8_NSC_CHECK_NS_RANGE_RW(ns_dst, len);
 *   return ra8_xspi_flash_read(k_ra8_nsc_xspi_instance, flash_off, ns_dst, len);
 * @endcode
 *
 * In the host build the CMSE check is a no-op; the tail call lands in
 * ``ra8_xspi_flash_read``.  We do not initialise the XSPI driver here,
 * so the dec=F vector falls through to a non-``invalid_arg`` failure
 * code (proving the validator did not reject the input).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

/**
 * @enum t_xspi_t
 * @brief Destination buffer for the cross-gateway read.
 */
typedef enum : uint16_t {
  k_t_dst_cap = 5000U, /**< Read destination, bytes; larger than one flash page
                            so the veneer must loop rather than single-shot.    */
} t_xspi_t;

typedef enum : uint32_t {
  k_test_xspi_len_ok   = 256U,  /**< Valid read length.            */
  k_test_xspi_len_zero = 0U,    /**< Forces C1=T (short-circuits). */
  k_test_xspi_len_max  = 4096U, /**< Last accepted length.         */
  k_test_xspi_len_over = 4097U, /**< First rejected length.        */
  k_test_xspi_offset   = 0U,    /**< Arbitrary flash offset.       */
} test_xspi_len_t;

/**
 * @test internal_test_mcdc_ra8_nsc_xspi
 * @brief Verify MC/DC coverage of the XSPI read-length validator.
 *
 * @details Drives accepted, zero, maximum, and over-limit lengths through the
 *          veneer and distinguishes validator rejection from driver failure.
 *
 * @par MC/DC:
 * Decision: ``ra8_nsc_xspi_read`` line 67,
 * ``if ((len == 0U) || (len > k_ra8_nsc_xspi_max_read))``
 * (libs/ra8_nsc/src/ra8_nsc_xspi.c). 2 conditions, ``||``.
 * N+1 = 3 vectors:
 * - V1: len=256  -> C1=F, C2=F -> dec F (validator accepts; tail call
 *                                         fails with non-invalid_arg)
 * - V2: len=0    -> C1=T (short-circuits) -> dec T -> invalid_arg
 * - V3: len=4097 -> C1=F, C2=T -> dec T -> invalid_arg
 * V1+V3 vary C2 only; V1+V2 vary C1 only.
 * DO-178C 6.4.4.3 minimal MC/DC subset.
 *
 * @pre The XSPI driver remains intentionally uninitialized.
 * @pre The static destination buffer covers every accepted read length.
 * @post Both invalid lengths return k_ra8_err_invalid_arg.
 * @post Valid and maximum lengths pass the veneer validator.
 *
 * @note A non-invalid-argument driver error proves the forward branch ran.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_mcdc_ra8_nsc_xspi(void)
{
  TEST_BEGIN("ra8_nsc_xspi_read MC/DC: length validator OR");
  static uint8_t s_dst[k_t_dst_cap] = {};

  /* V1: valid length -> dec F.  XSPI driver not initialized in this
   * TU, so the tail call returns a non-invalid_arg error. */
  const ra8_err_t v1 =
    ra8_nsc_xspi_read((uint32_t)k_test_xspi_offset, s_dst, (uint32_t)k_test_xspi_len_ok);
  TEST_ASSERT(v1 != k_ra8_err_invalid_arg);

  /* V2: len=0 -> dec T via C1 -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_nsc_xspi_read((uint32_t)k_test_xspi_offset, s_dst, (uint32_t)k_test_xspi_len_zero));

  /* V3: len=4097 (one past max) -> dec T via C2 -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_nsc_xspi_read((uint32_t)k_test_xspi_offset, s_dst, (uint32_t)k_test_xspi_len_over));

  /* Bonus: maximum still accepted (not part of the N+1 set, but
   * confirms the ``>`` is strict). */
  const ra8_err_t v_max =
    ra8_nsc_xspi_read((uint32_t)k_test_xspi_offset, s_dst, (uint32_t)k_test_xspi_len_max);
  TEST_ASSERT(v_max != k_ra8_err_invalid_arg);

  TEST_END("ra8_nsc_xspi_read MC/DC: length validator OR");
}

int main(void)
{
  internal_test_mcdc_ra8_nsc_xspi();
  return 0;
}

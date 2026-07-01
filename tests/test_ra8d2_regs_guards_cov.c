/**
 * @file test_ra8d2_regs_guards_cov.c
 * @brief Coverage top-up for the out-of-range nullptr guards in the RA8D2
 *        register-accessor inline headers.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Six per-peripheral register headers expose a `static inline` accessor
 * that maps an instance/channel/port index to its memory-mapped register
 * window and returns `nullptr` when the index is out of range. The trailing
 * `return nullptr;` guard leg is undrivable by the peripheral drivers (they
 * only ever pass validated indices), so it is uncovered by the normal
 * suites. This file drives each accessor with an out-of-range index so the
 * guard's true arm (the `return nullptr;` line) executes, and with a valid
 * index so the false arm (the pointer-computing return) executes too. gcovr
 * merges the resulting coverage across every test binary.
 *
 * Accessors exercised:
 *   - ra_acmphs()   (ra8d2_acmphs_regs.h) -- ACMPHS channel 0..5.
 *   - ra_cnecc()    (ra8d2_cnecc_regs.h)  -- CANFD ECC instance 0..1.
 *   - ra_dotf_regs()(ra8d2_dotf_regs.h)   -- DOTF channel 0..1.
 *   - ra_port()     (ra8d2_port_regs.h)   -- IOPORT port 0..14.
 *   - ra_ssie()     (ra8d2_ssie_regs.h)   -- SSIE channel 0..1.
 *   - ra_ulpt()     (ra8d2_ulpt_regs.h)   -- ULPT channel 0..1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_acmphs_regs.h"
#include "ra8d2_cnecc_regs.h"
#include "ra8d2_dotf_regs.h"
#include "ra8d2_port_regs.h"
#include "ra8d2_ssie_regs.h"
#include "ra8d2_ulpt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_regs_guard_const_t
 * @brief Out-of-range index driven into every accessor.
 */
typedef enum : uint8_t {
  k_t_oor_index = 0xFFU, /**< Well past every peripheral's instance count. */
} t_regs_guard_const_t;

/**
 * @brief ra_acmphs(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision `(uint16_t)channel >= k_ra_acmphs_channel_count`:
 * - channel=0xFF -> true  -> nullptr  (guard taken).
 * - channel=0    -> false -> non-null (guard skipped).
 * The two vectors give independent influence for the lone condition.
 */
static void test_acmphs_guard(void)
{
  TEST_BEGIN("acmphs guard");
  TEST_ASSERT_NULL(ra_acmphs((uint8_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_acmphs(0U));
  TEST_END("acmphs guard");
}

/**
 * @brief ra_cnecc(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision `(uint16_t)instance >= k_ra_cnecc_instance_count`:
 * - instance=0xFF -> true  -> nullptr.
 * - instance=0    -> false -> non-null.
 */
static void test_cnecc_guard(void)
{
  TEST_BEGIN("cnecc guard");
  TEST_ASSERT_NULL(ra_cnecc((uint8_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_cnecc(0U));
  TEST_END("cnecc guard");
}

/**
 * @brief ra_dotf_regs(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision
 * `(uint16_t)channel >= (uint16_t)k_ra_dotf_channel_count`:
 * - channel=0xFF -> true  -> nullptr.
 * - channel=0    -> false -> non-null.
 */
static void test_dotf_guard(void)
{
  TEST_BEGIN("dotf guard");
  TEST_ASSERT_NULL(ra_dotf_regs((uint8_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_dotf_regs(0U));
  TEST_END("dotf guard");
}

/**
 * @brief ra_port(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision `(uint8_t)port > k_ra_port_max`:
 * - port=0xFF (255) -> true  -> nullptr.
 * - port=k_ra_port_0 -> false -> non-null.
 */
static void test_port_guard(void)
{
  TEST_BEGIN("port guard");
  TEST_ASSERT_NULL(ra_port((ra_port_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_port(k_ra_port_0));
  TEST_END("port guard");
}

/**
 * @brief ra_ssie(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision `(uint16_t)channel >= k_ra_ssie_channel_count`:
 * - channel=0xFF -> true  -> nullptr.
 * - channel=0    -> false -> non-null.
 */
static void test_ssie_guard(void)
{
  TEST_BEGIN("ssie guard");
  TEST_ASSERT_NULL(ra_ssie((uint8_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_ssie(0U));
  TEST_END("ssie guard");
}

/**
 * @brief ra_ulpt(): out-of-range yields nullptr, valid yields a pointer.
 *
 * @par MC/DC:
 * Single-condition decision `(uint16_t)channel >= k_ra_ulpt_channel_count`:
 * - channel=0xFF -> true  -> nullptr.
 * - channel=0    -> false -> non-null.
 */
static void test_ulpt_guard(void)
{
  TEST_BEGIN("ulpt guard");
  TEST_ASSERT_NULL(ra_ulpt((uint8_t)k_t_oor_index));
  TEST_ASSERT_NOT_NULL(ra_ulpt(0U));
  TEST_END("ulpt guard");
}

int32_t main(void)
{
  test_acmphs_guard();
  test_cnecc_guard();
  test_dotf_guard();
  test_port_guard();
  test_ssie_guard();
  test_ulpt_guard();
  (void)fprintf(stderr, "[OK  ] test_ra8d2_regs_guards_cov.c\n");
  return 0;
}

/**
 * @file test_app_mpu_partition_simple.c
 * @brief Integration test: single-region MPU RO partition demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/mpu_partition_simple/src/main.c bring-up:
 * build a one-entry RO region table covering a 32-byte aligned
 * scratch buffer and feed it to ra8_mpu_configure. The host shim
 * does not actually trap the demo's probe write -- the test
 * exercises the configuration acceptance path and the rejection
 * path for malformed descriptors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mpu.h"
#include "ra8_mpu_regs.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_mpu_simple_size     = 32U,         /**< Test MPU simple size.  */
  k_test_mpu_simple_size_bad = 17U,         /**< Not a power of two.    */
  k_test_mpu_simple_mair0    = 0x000000FFU, /**< Test MPU simple mair0. */
  k_test_mpu_simple_dregion16 =
    0x00001000UL, /**< MPU_TYPE.DREGION = 16 (Cortex-M85 silicon value). */
} test_mpu_simple_const_t;

[[gnu::aligned(32)]] static uint8_t s_buf[k_test_mpu_simple_size] = {};

/**
 * @brief Reset host-side mock MMIO and seed MPU_TYPE.DREGION.
 *
 * @details
 * The host fake zero-fills the SCB/MPU window on each
 * ``ra8_fake_mmap_reset``. The production silicon ships ``MPU_TYPE.DREGION``
 * = 16 (Cortex-M85 TRM, "MPU_TYPE"); without seeding that field the
 * acceptance path in ``ra8_mpu_validate_cfg`` would reject every region
 * configuration as ``region_count > implemented``. We mirror the
 * sibling ``test_app_threadx_mpu_partition_demo`` helper here so the
 * golden bring-up vector can succeed on the host.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_mpu_regs()->TYPE = (uint32_t)k_test_mpu_simple_dregion16;
}

/**
 * @brief Golden bring-up: configure a single RO region.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_mpu_configure != ok``. One atomic condition
 * x 2 vectors -- success (this) + invalid descriptor (below).
 */
static void test_mpu_simple_configure_ok(void)
{
  reset_world();
  TEST_BEGIN("mpu_partition_simple: configure single RO region");
  const ra8_mpu_region_t region = {
    .base       = (uintptr_t)s_buf,
    .size       = (uint32_t)k_test_mpu_simple_size,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_ro,
    .executable = false,
    .shareable  = k_ra8_mpu_share_inner,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {
    .regions      = &region,
    .region_count = 1U,
    .mair0        = (uint32_t)k_test_mpu_simple_mair0,
    .mair1        = 0U,
    .privdefena   = true,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_configure(&cfg));
  TEST_END("mpu_partition_simple: configure single RO region");
}

/**
 * @brief NULL cfg rejected.
 *
 * @par MC/DC:
 * Decision: ``cfg == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_mpu_simple_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("mpu_partition_simple: NULL cfg rejected");
  TEST_ASSERT(ra8_mpu_configure(nullptr) != k_ra8_ok);
  TEST_END("mpu_partition_simple: NULL cfg rejected");
}

/**
 * @brief Non-power-of-two region size rejected.
 *
 * @par MC/DC:
 * Decision: ``size is power of two and >= 32``. Compound condition
 * with two atomic checks (>=32, popcount==1). Vectors: golden
 * (above), size=17 here (popcount fails), size=16 implicit (covered
 * by other tests in the project).
 */
static void test_mpu_simple_bad_size(void)
{
  reset_world();
  TEST_BEGIN("mpu_partition_simple: non-power-of-two size rejected");
  const ra8_mpu_region_t region = {
    .base       = (uintptr_t)s_buf,
    .size       = (uint32_t)k_test_mpu_simple_size_bad,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_ro,
    .executable = false,
    .shareable  = k_ra8_mpu_share_inner,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {
    .regions      = &region,
    .region_count = 1U,
    .mair0        = (uint32_t)k_test_mpu_simple_mair0,
    .mair1        = 0U,
    .privdefena   = true,
    .hfnmiena     = false,
  };
  TEST_ASSERT(ra8_mpu_configure(&cfg) != k_ra8_ok);
  TEST_END("mpu_partition_simple: non-power-of-two size rejected");
}

/**
 * @brief Disable / enable round-trip on the MPU.
 *
 * @par MC/DC:
 * No compound decisions in the helpers under test; both calls are
 * single-condition wrappers. Pair runs each branch once.
 */
static void test_mpu_simple_enable_disable(void)
{
  reset_world();
  TEST_BEGIN("mpu_partition_simple: enable/disable round-trip");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_disable());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_enable());
  TEST_END("mpu_partition_simple: enable/disable round-trip");
}

int main(void)
{
  test_mpu_simple_configure_ok();
  test_mpu_simple_null_cfg();
  test_mpu_simple_bad_size();
  test_mpu_simple_enable_disable();
  return 0;
}

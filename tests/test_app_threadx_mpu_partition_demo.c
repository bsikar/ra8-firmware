/**
 * @file test_app_threadx_mpu_partition_demo.c
 * @brief Integration test: ThreadX + Armv8-M MPU partition table bring-up
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_mpu_partition_demo/
 * main.c programs three MPU regions (MRAM-RX, SRAM-RW, peripheral
 * block-RW), initialises LED1, then hands control to ThreadX which
 * blinks the LED at 1 Hz. ThreadX is not linked into the host test
 * build (RA_SIMULATOR_MODE), so this test exercises the same MPU
 * configuration path main() drives plus the BSP LED surface the
 * worker thread calls inside its blink loop.
 *
 * Exercised modules:
 *   - ra_mpu             (region table programming)
 *   - ra_board_ek_ra8d2  (LED1 init + toggle)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_mpu_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_mpu.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/** @brief Per-test enums -- mirror the demo's static MPU table. */
typedef enum : uintptr_t {
  k_test_mpu_mram_base = 0x02000000UL,
  k_test_mpu_sram_base = 0x22000000UL,
  k_test_mpu_peri_base = 0x40000000UL,
} test_mpu_base_t;

typedef enum : uint32_t {
  k_test_mpu_mram_size = 0x00100000UL, /**< 1 MiB.                 */
  k_test_mpu_sram_size = 0x00100000UL, /**< 1 MiB.                 */
  k_test_mpu_peri_size = 0x10000000UL, /**< 256 MiB.               */
  k_test_mpu_dregion16 = 0x00001000UL, /**< MPU_TYPE.DREGION = 16. */
} test_mpu_size_t;

typedef enum : uint8_t {
  k_test_mpu_mair0_normal = 0xFFU,
  k_test_mpu_mair0_device = 0x04U,
  k_test_mpu_blink_iters  = 4U,
} test_mpu_byte_t;

typedef enum : uint32_t {
  k_test_mpu_mair0_word =
    ((uint32_t)k_test_mpu_mair0_device << 8U) | (uint32_t)k_test_mpu_mair0_normal,
} test_mpu_mair_t;

/** @brief Mock the silicon's region count so the configure() bound check passes. */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  ra_mpu_regs()->TYPE = (uint32_t)k_test_mpu_dregion16;
}

/**
 * @brief Build the production app's three-region table.
 */
static const ra_mpu_region_t s_mpu_test_regions[] = {
  {.base       = k_test_mpu_mram_base,
   .size       = k_test_mpu_mram_size,
   .priv       = k_ra_mpu_perm_ro,
   .unpriv     = k_ra_mpu_perm_ro,
   .executable = true,
   .shareable  = k_ra_mpu_share_non,
   .attr_idx   = k_ra_mpu_attr_idx_0},
  {.base       = k_test_mpu_sram_base,
   .size       = k_test_mpu_sram_size,
   .priv       = k_ra_mpu_perm_rw,
   .unpriv     = k_ra_mpu_perm_rw,
   .executable = false,
   .shareable  = k_ra_mpu_share_inner,
   .attr_idx   = k_ra_mpu_attr_idx_0},
  {.base       = k_test_mpu_peri_base,
   .size       = k_test_mpu_peri_size,
   .priv       = k_ra_mpu_perm_rw,
   .unpriv     = k_ra_mpu_perm_none,
   .executable = false,
   .shareable  = k_ra_mpu_share_outer,
   .attr_idx   = k_ra_mpu_attr_idx_1},
};

static ra_mpu_cfg_t make_demo_cfg(void)
{
  const ra_mpu_cfg_t cfg = {
    .regions      = s_mpu_test_regions,
    .region_count = (uint8_t)(sizeof(s_mpu_test_regions) / sizeof(s_mpu_test_regions[0])),
    .mair0        = (uint32_t)k_test_mpu_mair0_word,
    .mair1        = 0U,
    .privdefena   = true,
    .hfnmiena     = false,
  };
  return cfg;
}

/* -------------------------------------------------------------------------
 * Golden path: full bring-up
 * ------------------------------------------------------------------------- */

/**
 * @brief Demo MPU configure: 3 regions accepted + MPU enabled.
 *
 * @par MC/DC:
 * Decision under test: ``ra_mpu_configure() != ok``. Multiple atomic
 * conditions (cfg!=NULL, regions!=NULL, region_count<=DREGION,
 * per-region power-of-two-size). N+1 vectors; this test covers the
 * all-valid vector. Failure vectors split below.
 */
static void test_mpu_configure_demo_table_ok(void)
{
  reset_world();
  TEST_BEGIN("mpu_demo: configure(3-region table) ok");
  const ra_mpu_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_mpu_configure(&cfg));
  /* main() guarantees MPU_CTRL.ENABLE on success. */
  TEST_ASSERT((ra_mpu_regs()->CTRL & 1U) != 0U);
  TEST_END("mpu_demo: configure(3-region table) ok");
}

/**
 * @brief LED1 init succeeds after MPU partitioning is in place.
 *
 * @par MC/DC:
 * State-machine vector: post-configure peripheral block remains
 * accessible -- LED init must succeed because the peri region
 * permits privileged RW.
 */
static void test_mpu_led_init_after_configure(void)
{
  reset_world();
  TEST_BEGIN("mpu_demo: LED1 init after MPU configure");
  const ra_mpu_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_mpu_configure(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_board_led_init(k_ra_board_led1));
  TEST_END("mpu_demo: LED1 init after MPU configure");
}

/**
 * @brief Blink-loop replay: N toggles of LED1 inside the worker thread.
 *
 * @par MC/DC:
 * Decision under test: per-iteration ``ra_board_led_toggle != ok``
 * inside the demo's blink loop. One atomic condition x 2 vectors:
 * ok-loop (this test) + invalid-id (covered by the negative test).
 */
static void test_mpu_thread_blink_loop(void)
{
  reset_world();
  TEST_BEGIN("mpu_demo: thread_entry blink loop N iterations");
  const ra_mpu_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_mpu_configure(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_board_led_init(k_ra_board_led1));
  for (uint8_t i = 0U; i < (uint8_t)k_test_mpu_blink_iters; i++) {
    TEST_ASSERT_EQ(k_ra_ok, ra_board_led_toggle(k_ra_board_led1));
  }
  TEST_END("mpu_demo: thread_entry blink loop N iterations");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief NULL cfg rejected (panic-halt path in main).
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` precondition guard --
 * failure side of the configure precondition.
 */
static void test_mpu_configure_null_cfg_rejected(void)
{
  reset_world();
  TEST_BEGIN("mpu_demo: configure(NULL) rejected");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_mpu_configure(nullptr));
  TEST_END("mpu_demo: configure(NULL) rejected");
}

/**
 * @brief Region with non-power-of-two size rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``size is power-of-two`` invariant
 * inside ra_mpu_configure -- failure side of the per-region validation.
 */
static void test_mpu_configure_bad_region_size_rejected(void)
{
  reset_world();
  TEST_BEGIN("mpu_demo: configure rejects non-power-of-two region size");
  const ra_mpu_region_t bad = {
    .base       = k_test_mpu_sram_base,
    .size       = 0x00012345UL, /* not a power of two */
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = false,
    .shareable  = k_ra_mpu_share_inner,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  const ra_mpu_cfg_t cfg = {
    .regions      = &bad,
    .region_count = 1U,
    .mair0        = (uint32_t)k_test_mpu_mair0_word,
    .mair1        = 0U,
    .privdefena   = true,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mpu_configure(&cfg));
  TEST_END("mpu_demo: configure rejects non-power-of-two region size");
}

/**
 * @brief Region count exceeding DREGION rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``region_count > DREGION`` failure-side
 * vector pairing with the demo's 3-region (count<=DREGION) ok vector.
 */
static void test_mpu_configure_too_many_regions_rejected(void)
{
  reset_world();
  /* Pretend the silicon only has 2 regions; the demo asks for 3. */
  ra_mpu_regs()->TYPE = (uint32_t)(2UL << 8U);
  TEST_BEGIN("mpu_demo: configure rejects region_count > DREGION");
  const ra_mpu_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mpu_configure(&cfg));
  TEST_END("mpu_demo: configure rejects region_count > DREGION");
}

int main(void)
{
  test_mpu_configure_demo_table_ok();
  test_mpu_led_init_after_configure();
  test_mpu_thread_blink_loop();
  test_mpu_configure_null_cfg_rejected();
  test_mpu_configure_bad_region_size_rejected();
  test_mpu_configure_too_many_regions_rejected();
  return 0;
}

/**
 * @file test_ra_mpu.c
 * @brief Unit tests for the Cortex-M85 MPU helper (ra_mpu.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_mpu_regs.h"
#include "ra_err.h"
#include "ra_mpu.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum test_mpu_layout_t
 * @brief Magic numbers used by the test suite to drive the mock MPU.
 */
typedef enum : uint32_t {
  k_test_mpu_dregion_count = 16U,          /**< Pretend we are an M85.   */
  k_test_mpu_type_dregion  = 0x00001000UL, /**< DREGION = 16 << 8.       */
  k_test_mpu_region_size   = 0x00001000UL, /**< 4 KiB power-of-two size. */
  k_test_mpu_region_base   = 0x20000000UL, /**< Aligned base.            */
  k_test_mpu_mair0         = 0x44440000UL, /**< Arbitrary MAIR pattern.  */
  k_test_mpu_mair1         = 0x00000044UL,
} test_mpu_layout_t;

/**
 * @brief Reset the simulated MPU register block.
 *
 * @details
 * `ra_sim_mmap_reset()` zeros every backing region. Tests then write
 * MPU_TYPE.DREGION = 16 directly so `ra_mpu_dregion_count()` returns
 * a useful value when the helper validates region indices.
 */
static void mpu_test_setup(void)
{
  ra_sim_mmap_reset();
  ra_mpu_regs()->TYPE = (uint32_t)k_test_mpu_type_dregion;
}

static void test_register_layout(void)
{
  TEST_BEGIN("r_mpu_regs_t offsets match Arm Cortex-M85 TRM");
  TEST_ASSERT_EQ((int)0x00, (int)offsetof(r_mpu_regs_t, TYPE));
  TEST_ASSERT_EQ((int)0x04, (int)offsetof(r_mpu_regs_t, CTRL));
  TEST_ASSERT_EQ((int)0x08, (int)offsetof(r_mpu_regs_t, RNR));
  TEST_ASSERT_EQ((int)0x0C, (int)offsetof(r_mpu_regs_t, RBAR));
  TEST_ASSERT_EQ((int)0x10, (int)offsetof(r_mpu_regs_t, RLAR));
  TEST_ASSERT_EQ((int)0x30, (int)offsetof(r_mpu_regs_t, MAIR0));
  TEST_ASSERT_EQ((int)0x34, (int)offsetof(r_mpu_regs_t, MAIR1));
  TEST_ASSERT_EQ((int)0x38, (int)sizeof(r_mpu_regs_t));
  TEST_END("r_mpu_regs_t offsets match Arm Cortex-M85 TRM");
}

static void test_configure_null_cfg(void)
{
  TEST_BEGIN("ra_mpu_configure(NULL) returns null_ptr");
  mpu_test_setup();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mpu_configure(nullptr));
  TEST_END("ra_mpu_configure(NULL) returns null_ptr");
}

static void test_configure_too_many_regions(void)
{
  TEST_BEGIN("ra_mpu_configure rejects region_count > DREGION");
  mpu_test_setup();
  /* Pretend silicon only has 8 regions; ask for 16. */
  ra_mpu_regs()->TYPE = (uint32_t)(8UL << 8U);
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra_mpu_share_inner,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  ra_mpu_region_t regions[16];
  for (uint8_t i = 0U; i < 16U; ++i) {
    regions[i] = r;
  }
  const ra_mpu_cfg_t cfg = {
    .regions      = regions,
    .region_count = 16U,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = false,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mpu_configure(&cfg));
  TEST_END("ra_mpu_configure rejects region_count > DREGION");
}

static void test_configure_invalid_size(void)
{
  TEST_BEGIN("ra_mpu_configure rejects non-power-of-two size");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = 0x1500U, /* not a power of two */
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  const ra_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mpu_configure(&cfg));
  TEST_END("ra_mpu_configure rejects non-power-of-two size");
}

static void test_configure_misaligned_base(void)
{
  TEST_BEGIN("ra_mpu_configure rejects misaligned base");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)(k_test_mpu_region_base + 16U), /* 16 < 32 align */
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  const ra_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mpu_configure(&cfg));
  TEST_END("ra_mpu_configure rejects misaligned base");
}

static void test_configure_unrepresentable_perms(void)
{
  TEST_BEGIN("ra_mpu_configure rejects priv-RO + unpriv-RW");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_ro,
    .unpriv     = k_ra_mpu_perm_rw, /* not encodable in AP[1:0] */
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  const ra_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mpu_configure(&cfg));
  TEST_END("ra_mpu_configure rejects priv-RO + unpriv-RW");
}

static void test_configure_programs_region_zero(void)
{
  TEST_BEGIN("ra_mpu_configure programs region 0 RBAR/RLAR");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_none,
    .executable = false, /* XN = 1 */
    .shareable  = k_ra_mpu_share_inner,
    .attr_idx   = k_ra_mpu_attr_idx_2,
  };
  const ra_mpu_cfg_t cfg = {
    .regions      = &r,
    .region_count = 1U,
    .mair0        = (uint32_t)k_test_mpu_mair0,
    .mair1        = (uint32_t)k_test_mpu_mair1,
    .privdefena   = true,
    .hfnmiena     = false,
  };

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mpu_configure(&cfg));

  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  TEST_ASSERT_EQ((int32_t)k_test_mpu_mair0, (int32_t)mpu->MAIR0);
  TEST_ASSERT_EQ((int32_t)k_test_mpu_mair1, (int32_t)mpu->MAIR1);

  /* RNR ends at the last cleared region (DREGION - 1 = 15). */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_test_mpu_dregion_count - 1U), (int32_t)mpu->RNR);

  /* CTRL must have ENABLE | PRIVDEFENA, no HFNMIENA. */
  const uint32_t expected_ctrl =
    (uint32_t)k_ra_mpu_ctrl_enable | (uint32_t)k_ra_mpu_ctrl_privdefena;
  TEST_ASSERT_EQ((int32_t)expected_ctrl, (int32_t)mpu->CTRL);
  TEST_END("ra_mpu_configure programs region 0 RBAR/RLAR");
}

static void test_configure_clears_unused_regions(void)
{
  TEST_BEGIN("ra_mpu_configure clears regions above region_count");
  mpu_test_setup();
  /* Pre-poison region 5's RLAR via the same RNR-select mechanism the
   * driver uses, then run configure with a 1-region table -- the
   * driver should walk regions 1..15 clearing RLAR. */
  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  mpu->RNR                   = 5U;
  mpu->RLAR                  = 0xFFFFFFFFU;

  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  const ra_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mpu_configure(&cfg));

  /* Re-select region 5 and check its RLAR is back to zero. */
  mpu->RNR = 5U;
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mpu->RLAR);
  TEST_END("ra_mpu_configure clears regions above region_count");
}

static void test_enable_disable(void)
{
  TEST_BEGIN("ra_mpu_enable / ra_mpu_disable toggle CTRL.ENABLE");
  mpu_test_setup();
  ra_mpu_regs()->CTRL = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mpu_enable());
  TEST_ASSERT_EQ((int32_t)k_ra_mpu_ctrl_enable,
                 (int32_t)(ra_mpu_regs()->CTRL & (uint32_t)k_ra_mpu_ctrl_enable));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mpu_disable());
  TEST_ASSERT_EQ((int32_t)0,
                 (int32_t)(ra_mpu_regs()->CTRL & (uint32_t)k_ra_mpu_ctrl_enable));
  TEST_END("ra_mpu_enable / ra_mpu_disable toggle CTRL.ENABLE");
}

static void test_set_region_null(void)
{
  TEST_BEGIN("ra_mpu_set_region(NULL) returns null_ptr");
  mpu_test_setup();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_mpu_set_region(0U, nullptr));
  TEST_END("ra_mpu_set_region(NULL) returns null_ptr");
}

static void test_set_region_out_of_range(void)
{
  TEST_BEGIN("ra_mpu_set_region rejects out-of-range region index");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_rw,
    .unpriv     = k_ra_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  };
  /* DREGION = 16, valid indices 0..15. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_mpu_set_region(16U, &r));
  TEST_END("ra_mpu_set_region rejects out-of-range region index");
}

static void test_set_region_writes_pair(void)
{
  TEST_BEGIN("ra_mpu_set_region writes RBAR + RLAR for selected region");
  mpu_test_setup();
  const ra_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra_mpu_perm_ro,
    .unpriv     = k_ra_mpu_perm_ro,
    .executable = true,
    .shareable  = k_ra_mpu_share_non,
    .attr_idx   = k_ra_mpu_attr_idx_3,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mpu_set_region(7U, &r));

  volatile r_mpu_regs_t* mpu = ra_mpu_regs();
  TEST_ASSERT_EQ((int32_t)7, (int32_t)mpu->RNR);

  /* RLAR.EN must be set; AttrIndx must equal 3 (encoded in bits 3:1). */
  TEST_ASSERT_EQ((int32_t)k_ra_mpu_rlar_en_mask,
                 (int32_t)(mpu->RLAR & (uint32_t)k_ra_mpu_rlar_en_mask));
  const uint32_t attr =
    (mpu->RLAR & (uint32_t)k_ra_mpu_rlar_attridx_mask) >>
    (uint32_t)k_ra_mpu_rlar_attridx_shift;
  TEST_ASSERT_EQ((int32_t)k_ra_mpu_attr_idx_3, (int32_t)attr);

  /* RBAR.BASE field must equal our aligned base. */
  TEST_ASSERT_EQ((int32_t)k_test_mpu_region_base,
                 (int32_t)(mpu->RBAR & (uint32_t)k_ra_mpu_rbar_base_mask));
  /* AP encoding for priv RO + unpriv RO must be 0b11 = 3. */
  const uint32_t ap = (mpu->RBAR & (uint32_t)k_ra_mpu_rbar_ap_mask) >>
                      (uint32_t)k_ra_mpu_rbar_ap_shift;
  TEST_ASSERT_EQ((int32_t)3, (int32_t)ap);
  /* XN = 0 because executable = true. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(mpu->RBAR & (uint32_t)k_ra_mpu_rbar_xn_mask));
  TEST_END("ra_mpu_set_region writes RBAR + RLAR for selected region");
}

int32_t main(void)
{
  test_register_layout();
  test_configure_null_cfg();
  test_configure_too_many_regions();
  test_configure_invalid_size();
  test_configure_misaligned_base();
  test_configure_unrepresentable_perms();
  test_configure_programs_region_zero();
  test_configure_clears_unused_regions();
  test_enable_disable();
  test_set_region_null();
  test_set_region_out_of_range();
  test_set_region_writes_pair();
  (void)fprintf(stderr, "[OK ] test_ra_mpu.c\n");
  return 0;
}

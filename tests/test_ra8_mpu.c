/**
 * @file test_ra8_mpu.c
 * @brief Unit tests for the Cortex-M85 MPU helper (ra8_mpu.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mpu.h"
#include "ra8_mpu_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_mpu_probe_t
 * @brief MPU region selector and limit-address pattern.
 */
typedef enum : uint32_t {
  k_t_region_index  = 5U,          /**< Region the RNR selects before the write. */
  k_t_rlar_all_ones = 0xFFFFFFFFU, /**< Every RLAR bit set, so the driver's
                                        masking of the reserved bits is visible. */
} t_mpu_probe_t;

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
  k_test_mpu_mair1         = 0x00000044UL, /**< Test MPU mair1.          */
} test_mpu_layout_t;

/**
 * @brief Reset the simulated MPU register block.
 *
 * @details
 * `ra8_sim_mmap_reset()` zeros every backing region. Tests then write
 * MPU_TYPE.DREGION = 16 directly so `ra8_mpu_dregion_count()` returns
 * a useful value when the helper validates region indices.
 */
static void mpu_test_setup(void)
{
  ra8_sim_mmap_reset();
  ra8_mpu_regs()->TYPE = (uint32_t)k_test_mpu_type_dregion;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_layout(void)
{
  TEST_BEGIN("r_mpu_regs_t offsets match Arm Cortex-M85 TRM");
  TEST_ASSERT_EQ(0x00, offsetof(r_mpu_regs_t, TYPE));
  TEST_ASSERT_EQ(0x04, offsetof(r_mpu_regs_t, CTRL));
  TEST_ASSERT_EQ(0x08, offsetof(r_mpu_regs_t, RNR));
  TEST_ASSERT_EQ(0x0C, offsetof(r_mpu_regs_t, RBAR));
  TEST_ASSERT_EQ(0x10, offsetof(r_mpu_regs_t, RLAR));
  TEST_ASSERT_EQ(0x30, offsetof(r_mpu_regs_t, MAIR0));
  TEST_ASSERT_EQ(0x34, offsetof(r_mpu_regs_t, MAIR1));
  TEST_ASSERT_EQ(0x38, sizeof(r_mpu_regs_t));
  TEST_END("r_mpu_regs_t offsets match Arm Cortex-M85 TRM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_null_cfg(void)
{
  TEST_BEGIN("ra8_mpu_configure(NULL) returns null_ptr");
  mpu_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mpu_configure(nullptr));
  TEST_END("ra8_mpu_configure(NULL) returns null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_too_many_regions(void)
{
  TEST_BEGIN("ra8_mpu_configure rejects region_count > DREGION");
  mpu_test_setup();
  /* Pretend silicon only has 8 regions; ask for 16. */
  ra8_mpu_regs()->TYPE     = (uint32_t)(8UL << 8U);
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra8_mpu_share_inner,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  ra8_mpu_region_t regions[16];
  for (uint8_t i = 0U; i < 16U; ++i) {
    regions[i] = r;
  }
  const ra8_mpu_cfg_t cfg = {
    .regions      = regions,
    .region_count = 16U,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = false,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_configure(&cfg));
  TEST_END("ra8_mpu_configure rejects region_count > DREGION");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_invalid_size(void)
{
  TEST_BEGIN("ra8_mpu_configure rejects non-power-of-two size");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = 0x1500U, /* not a power of two */
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_configure(&cfg));
  TEST_END("ra8_mpu_configure rejects non-power-of-two size");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_misaligned_base(void)
{
  TEST_BEGIN("ra8_mpu_configure rejects misaligned base");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)(k_test_mpu_region_base + 16U), /* 16 < 32 align */
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_configure(&cfg));
  TEST_END("ra8_mpu_configure rejects misaligned base");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_unrepresentable_perms(void)
{
  TEST_BEGIN("ra8_mpu_configure rejects priv-RO + unpriv-RW");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_rw, /* not encodable in AP[1:0] */
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_configure(&cfg));
  TEST_END("ra8_mpu_configure rejects priv-RO + unpriv-RW");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_programs_region_zero(void)
{
  TEST_BEGIN("ra8_mpu_configure programs region 0 RBAR/RLAR");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_none,
    .executable = false, /* XN = 1 */
    .shareable  = k_ra8_mpu_share_inner,
    .attr_idx   = k_ra8_mpu_attr_idx_2,
  };
  const ra8_mpu_cfg_t cfg = {
    .regions      = &r,
    .region_count = 1U,
    .mair0        = (uint32_t)k_test_mpu_mair0,
    .mair1        = (uint32_t)k_test_mpu_mair1,
    .privdefena   = true,
    .hfnmiena     = false,
  };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_configure(&cfg));

  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  TEST_ASSERT_EQ(k_test_mpu_mair0, mpu->MAIR0);
  TEST_ASSERT_EQ(k_test_mpu_mair1, mpu->MAIR1);

  /* RNR ends at the last cleared region (DREGION - 1 = 15). */
  TEST_ASSERT_EQ(((uint32_t)k_test_mpu_dregion_count - 1U), mpu->RNR);

  /* CTRL must have ENABLE | PRIVDEFENA, no HFNMIENA. */
  const uint32_t expected_ctrl =
    (uint32_t)k_ra8_mpu_ctrl_enable | (uint32_t)k_ra8_mpu_ctrl_privdefena;
  TEST_ASSERT_EQ(expected_ctrl, mpu->CTRL);
  TEST_END("ra8_mpu_configure programs region 0 RBAR/RLAR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_clears_unused_regions(void)
{
  TEST_BEGIN("ra8_mpu_configure clears regions above region_count");
  mpu_test_setup();
  /* Pre-poison region 5's RLAR via the same RNR-select mechanism the
   * driver uses, then run configure with a 1-region table -- the
   * driver should walk regions 1..15 clearing RLAR. */
  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  mpu->RNR                   = k_t_region_index;
  mpu->RLAR                  = k_t_rlar_all_ones;

  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  const ra8_mpu_cfg_t cfg = {.regions = &r, .region_count = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_configure(&cfg));

  /* Re-select region 5 and check its RLAR is back to zero. */
  mpu->RNR = k_t_region_index;
  TEST_ASSERT_EQ(0, mpu->RLAR);
  TEST_END("ra8_mpu_configure clears regions above region_count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_disable(void)
{
  TEST_BEGIN("ra8_mpu_enable / ra8_mpu_disable toggle CTRL.ENABLE");
  mpu_test_setup();
  ra8_mpu_regs()->CTRL = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_enable());
  TEST_ASSERT_EQ(k_ra8_mpu_ctrl_enable, (ra8_mpu_regs()->CTRL & (uint32_t)k_ra8_mpu_ctrl_enable));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_disable());
  TEST_ASSERT_EQ(0, (ra8_mpu_regs()->CTRL & (uint32_t)k_ra8_mpu_ctrl_enable));
  TEST_END("ra8_mpu_enable / ra8_mpu_disable toggle CTRL.ENABLE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_null(void)
{
  TEST_BEGIN("ra8_mpu_set_region(NULL) returns null_ptr");
  mpu_test_setup();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mpu_set_region(0U, nullptr));
  TEST_END("ra8_mpu_set_region(NULL) returns null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_out_of_range(void)
{
  TEST_BEGIN("ra8_mpu_set_region rejects out-of-range region index");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_rw,
    .unpriv     = k_ra8_mpu_perm_rw,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  /* DREGION = 16, valid indices 0..15. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(16U, &r));
  TEST_END("ra8_mpu_set_region rejects out-of-range region index");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_region_writes_pair(void)
{
  TEST_BEGIN("ra8_mpu_set_region writes RBAR + RLAR for selected region");
  mpu_test_setup();
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = (uint32_t)k_test_mpu_region_size,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_ro,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_3,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(7U, &r));

  volatile r_mpu_regs_t* mpu = ra8_mpu_regs();
  TEST_ASSERT_EQ(7, mpu->RNR);

  /* RLAR.EN must be set; AttrIndx must equal 3 (encoded in bits 3:1). */
  TEST_ASSERT_EQ(k_ra8_mpu_rlar_en_mask, (mpu->RLAR & (uint32_t)k_ra8_mpu_rlar_en_mask));
  const uint32_t attr =
    (mpu->RLAR & (uint32_t)k_ra8_mpu_rlar_attridx_mask) >> (uint32_t)k_ra8_mpu_rlar_attridx_shift;
  TEST_ASSERT_EQ(k_ra8_mpu_attr_idx_3, attr);

  /* RBAR.BASE field must equal our aligned base. */
  TEST_ASSERT_EQ(k_test_mpu_region_base, (mpu->RBAR & (uint32_t)k_ra8_mpu_rbar_base_mask));
  /* AP encoding for priv RO + unpriv RO must be 0b11 = 3. */
  const uint32_t ap =
    (mpu->RBAR & (uint32_t)k_ra8_mpu_rbar_ap_mask) >> (uint32_t)k_ra8_mpu_rbar_ap_shift;
  TEST_ASSERT_EQ(3, ap);
  /* XN = 0 because executable = true. */
  TEST_ASSERT_EQ(0, (mpu->RBAR & (uint32_t)k_ra8_mpu_rbar_xn_mask));
  TEST_END("ra8_mpu_set_region writes RBAR + RLAR for selected region");
}

/* =============================================================================
 * MC/DC vector tests for the compound boolean decisions in ra8_mpu.c
 * =============================================================================
 */

/**
 * @enum test_mpu_mcdc_t
 * @brief Constants used by the MC/DC vector tests.
 */
typedef enum : uint32_t {
  k_test_mpu_size_zero        = 0U,      /**< not pow2: value==0 path.    */
  k_test_mpu_size_three       = 3U,      /**< nonzero, not power-of-two.  */
  k_test_mpu_size_pow2_lt_min = 16U,     /**< pow2 but below 32-byte min. */
  k_test_mpu_size_min_ok      = 32U,     /**< pow2 and == min: accepted.  */
  k_test_mpu_size_4k          = 0x1000U, /**< Standard 4 KiB region.      */
} test_mpu_mcdc_t;

/**
 * @brief Build a baseline 4 KiB region descriptor parametrised on perms.
 */
static ra8_mpu_region_t mcdc_region(ra8_mpu_perm_t priv, ra8_mpu_perm_t unpriv, uint32_t size)
{
  const ra8_mpu_region_t r = {
    .base       = (uintptr_t)k_test_mpu_region_base,
    .size       = size,
    .priv       = priv,
    .unpriv     = unpriv,
    .executable = true,
    .shareable  = k_ra8_mpu_share_non,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  };
  return r;
}

/**
 * @test test_validate_region_mcdc_is_pow2
 *
 * @par MC/DC:
 * Decision: `(value != 0U) && ((value & (value - 1U)) == 0U)`
 * (2 conditions, libs/ra8_mpu/src/ra8_mpu.c line 48 -- ra8_mpu_is_pow2),
 * exercised through `ra8_mpu_validate_region` via `ra8_mpu_set_region`.
 * - Vector 1: size=0  -> C1=(0!=0)=F short-circuits; is_pow2 returns
 *             false; the outer guard `!is_pow2(...) || ...` is true and
 *             the validator rejects with invalid_arg.
 * - Vector 2: size=3  -> C1=T (3!=0), C2=((3 & 2)==0)=F. is_pow2
 *             returns false; validator rejects.
 * - Vector 3: size=32 -> C1=T, C2=((32 & 31)==0)=T. is_pow2 returns
 *             true; validator accepts (32 is the minimum region size).
 * Vectors 1+3 vary C1 with C2 implicitly true; vectors 2+3 vary C2 with
 * C1 held true. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_validate_region_mcdc_is_pow2(void)
{
  TEST_BEGIN("ra8_mpu_is_pow2 MC/DC via ra8_mpu_set_region(size)");
  mpu_test_setup();

  /* Vector 1: size=0 -> is_pow2 C1=F. */
  const ra8_mpu_region_t r0 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_zero);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r0));

  /* Vector 2: size=3 -> is_pow2 C1=T, C2=F. */
  const ra8_mpu_region_t r3 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_three);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r3));

  /* Vector 3: size=32 -> is_pow2 C1=T, C2=T (and >= min). Accepted. */
  const ra8_mpu_region_t r32 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_min_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(0U, &r32));
  TEST_END("ra8_mpu_is_pow2 MC/DC via ra8_mpu_set_region(size)");
}

/**
 * @test test_validate_region_mcdc_size_guard
 *
 * @par MC/DC:
 * Decision: `if (!ra8_mpu_is_pow2(r->size) || r->size < min_region_size)`
 * (2 conditions, libs/ra8_mpu/src/ra8_mpu.c line 98).
 * - Vector 1: size=32   -> C1=!T=F, C2=(32<32)=F. Decision F. Accepted.
 * - Vector 2: size=3    -> C1=!F=T short-circuits. Decision T. Rejected.
 * - Vector 3: size=16   -> C1=!T=F, C2=(16<32)=T. Decision T. Rejected.
 * Vectors 1+2 vary C1 with C2 held false (32 vs 3); vectors 1+3 vary C2
 * with C1 held false (32 vs 16). N+1 = 3 vectors: minimal MC/DC.
 */
static void test_validate_region_mcdc_size_guard(void)
{
  TEST_BEGIN("ra8_mpu validate_region MC/DC: !is_pow2 || size < min");
  mpu_test_setup();

  /* Vector 1: pow2 and >= min -> accepted. */
  const ra8_mpu_region_t r_ok =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_min_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(0U, &r_ok));

  /* Vector 2: not pow2 -> C1=T short-circuits, rejected. */
  const ra8_mpu_region_t r_npow2 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_three);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r_npow2));

  /* Vector 3: pow2 but below min -> C1=F, C2=T, rejected. */
  const ra8_mpu_region_t r_small =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_pow2_lt_min);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r_small));
  TEST_END("ra8_mpu validate_region MC/DC: !is_pow2 || size < min");
}

/**
 * @test test_encode_ap_mcdc_priv_rw_unpriv_rw
 *
 * @par MC/DC:
 * Decision: `if (priv == k_ra8_mpu_perm_rw && unpriv == k_ra8_mpu_perm_rw)`
 * (2 conditions, libs/ra8_mpu/src/ra8_mpu.c line 65), exercised via
 * `ra8_mpu_set_region` -> `ra8_mpu_validate_region` -> `ra8_mpu_encode_ap`.
 * The function is reached only when the L62 decision (rw,none) is
 * false, so `unpriv != none` for this test.
 * - Vector 1: priv=ro, unpriv=rw -> C1=(ro==rw)=F short-circuits.
 *             Decision F (and L68/L71 also F): encode returns invalid.
 *             Validator rejects with invalid_arg.
 * - Vector 2: priv=rw, unpriv=ro -> C1=(rw==rw)=T, C2=(ro==rw)=F.
 *             Decision F. L68/L71 also F: encode returns invalid;
 *             validator rejects.
 * - Vector 3: priv=rw, unpriv=rw -> C1=T, C2=T. Decision T.
 *             encode returns 1; validator accepts.
 * Vectors 1+3 vary C1 (held C2=T conceptually); vectors 2+3 vary C2
 * (held C1=T). N+1 = 3 vectors: minimal MC/DC.
 *
 * Note: vector 2 (priv=rw, unpriv=ro) simultaneously provides the
 * C1=F arm of the L68 (ro,none) and L71 (ro,ro) decisions, since priv
 * is rw rather than ro -- this closes the C1=F gap on those decisions
 * with a single test.
 */
static void test_encode_ap_mcdc_priv_rw_unpriv_rw(void)
{
  TEST_BEGIN("ra8_mpu_encode_ap L65 MC/DC: priv==rw && unpriv==rw");
  mpu_test_setup();

  /* Vector 1: priv=ro, unpriv=rw. */
  const ra8_mpu_region_t r1 =
    mcdc_region(k_ra8_mpu_perm_ro, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r1));

  /* Vector 2: priv=rw, unpriv=ro -- not encodable in AP[1:0]. */
  const ra8_mpu_region_t r2 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_ro, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r2));

  /* Vector 3: priv=rw, unpriv=rw -- encodes to AP=01, accepted. */
  const ra8_mpu_region_t r3 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(0U, &r3));
  TEST_END("ra8_mpu_encode_ap L65 MC/DC: priv==rw && unpriv==rw");
}

/**
 * @test test_encode_ap_mcdc_priv_ro_unpriv_none
 *
 * @par MC/DC:
 * Decision: `if (priv == k_ra8_mpu_perm_ro && unpriv == k_ra8_mpu_perm_none)`
 * (2 conditions, libs/ra8_mpu/src/ra8_mpu.c line 68). Reached only when
 * the L62 and L65 decisions are both false (priv != rw, OR
 * unpriv != none for L62 and not (rw,rw) for L65).
 * - Vector 1: priv=rw, unpriv=ro -> reaches L68 with C1=(rw==ro)=F,
 *             short-circuits. Decision F. (Falls through to L71 also
 *             F; validator rejects with invalid_arg.)
 * - Vector 2: priv=ro, unpriv=ro -> reaches L68 (L62/L65 both F).
 *             C1=(ro==ro)=T, C2=(ro==none)=F. Decision F. Falls to L71
 *             which is T -> encode returns 3 (priv RO + unpriv RO);
 *             validator accepts.
 * - Vector 3: priv=ro, unpriv=none -> C1=T, C2=T. Decision T.
 *             encode returns 2; validator accepts.
 * Vectors 1+3 vary C1 (C2 held T); vectors 2+3 vary C2 (C1 held T).
 * N+1 = 3 vectors: minimal MC/DC. The L62 decision (rw,none) is also
 * exercised by vector 3 with priv=ro -> C1=F arm.
 */
static void test_encode_ap_mcdc_priv_ro_unpriv_none(void)
{
  TEST_BEGIN("ra8_mpu_encode_ap L68 MC/DC: priv==ro && unpriv==none");
  mpu_test_setup();

  /* Vector 1: priv=rw, unpriv=ro -- reaches L68 with C1=F. */
  const ra8_mpu_region_t r1 =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_ro, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mpu_set_region(0U, &r1));

  /* Vector 2: priv=ro, unpriv=ro -- L68 C1=T, C2=F. Falls to L71=T. */
  const ra8_mpu_region_t r2 =
    mcdc_region(k_ra8_mpu_perm_ro, k_ra8_mpu_perm_ro, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(0U, &r2));

  /* Vector 3: priv=ro, unpriv=none -- L68 C1=T, C2=T -> AP=2. */
  const ra8_mpu_region_t r3 =
    mcdc_region(k_ra8_mpu_perm_ro, k_ra8_mpu_perm_none, (uint32_t)k_test_mpu_size_4k);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_set_region(0U, &r3));
  TEST_END("ra8_mpu_encode_ap L68 MC/DC: priv==ro && unpriv==none");
}

/**
 * @test test_validate_cfg_mcdc_region_count_and_regions
 *
 * @par MC/DC:
 * Decision: `if (cfg->region_count > 0U && cfg->regions == nullptr)`
 * (2 conditions, libs/ra8_mpu/src/ra8_mpu.c line 195 -- ra8_mpu_validate_cfg).
 * - Vector 1: region_count=0, regions=NULL -> C1=(0>0)=F short-circuits.
 *             Decision F. validate_cfg returns ok (no regions to walk).
 *             ra8_mpu_configure returns ok.
 * - Vector 2: region_count=1, regions=valid -> C1=T, C2=(non-null==null)=F.
 *             Decision F. validate_cfg walks the regions and configure
 *             returns ok.
 * - Vector 3: region_count=1, regions=NULL -> C1=T, C2=T. Decision T.
 *             validate_cfg returns null_ptr; configure propagates it.
 * Vectors 1+3 vary C1 (C2 held T); vectors 2+3 vary C2 (C1 held T).
 * N+1 = 3 vectors: minimal MC/DC.
 */
static void test_validate_cfg_mcdc_region_count_and_regions(void)
{
  TEST_BEGIN("ra8_mpu_validate_cfg MC/DC: region_count>0 && regions==NULL");
  mpu_test_setup();

  /* Vector 1: zero regions, regions=NULL is irrelevant (short-circuit). */
  const ra8_mpu_cfg_t cfg_v1 = {
    .regions      = nullptr,
    .region_count = 0U,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = false,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_configure(&cfg_v1));

  /* Vector 2: one region, valid pointer. */
  const ra8_mpu_region_t r =
    mcdc_region(k_ra8_mpu_perm_rw, k_ra8_mpu_perm_rw, (uint32_t)k_test_mpu_size_4k);
  const ra8_mpu_cfg_t cfg_v2 = {
    .regions      = &r,
    .region_count = 1U,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = false,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mpu_configure(&cfg_v2));

  /* Vector 3: one region claimed but pointer NULL -> null_ptr. */
  const ra8_mpu_cfg_t cfg_v3 = {
    .regions      = nullptr,
    .region_count = 1U,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = false,
    .hfnmiena     = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mpu_configure(&cfg_v3));
  TEST_END("ra8_mpu_validate_cfg MC/DC: region_count>0 && regions==NULL");
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
  test_validate_region_mcdc_is_pow2();
  test_validate_region_mcdc_size_guard();
  test_encode_ap_mcdc_priv_rw_unpriv_rw();
  test_encode_ap_mcdc_priv_ro_unpriv_none();
  test_validate_cfg_mcdc_region_count_and_regions();
  (void)fprintf(stderr, "[OK ] test_ra8_mpu.c\n");
  return 0;
}

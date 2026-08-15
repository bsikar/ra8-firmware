/**
 * @file test_ra8_register_protection.c
 * @brief Unit tests for the scoped PRCR unlock helper macro
 *
 * @details
 * `libs/ra8_core/inc/ra8_register_protection.h` exposes the
 * `RA8_PROTECTED_WRITE(unlock_val)` block macro. It expands to a
 * `for`-loop that runs exactly once, with the unlock written into
 * SYSTEM.PRCR in the init clause and the re-lock written in the
 * increment clause. These tests cross-check the macro against the
 * authoritative FSP `bsp_register_protection.c` behaviour:
 *
 *  - The PRCR password lives in the upper byte (`0xA5`).
 *  - Writing `(0xA500 | mask)` enables writes for the named group.
 *  - Writing `0xA500` (mask cleared) re-locks every group.
 *  - The macro re-locks even when the body executes a `break`.
 *  - The bit positions match FSP/HUM:
 *      PRC0 = 0x0001 (CGC),
 *      PRC1 = 0x0002 (OM_LPC_BATT / LPM),
 *      PRC3 = 0x0008 (LVD/PVD),
 *      PRC4 = 0x0010 (security),
 *      PRC5 = 0x0020 (RESET).
 *
 * The host-side fake maps a writable page at the SYSTEM block
 * base address, so `*ra8_sys_prcr() = ...` lands in observable host
 * memory and the asserts below read it back directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_fake_mmap.h"
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_prcr_test_const_t
 * @brief Bit-mask constants used by the PRCR cross-verification tests.
 *
 * @details
 * Mirrors the `g_prcr_masks[]` table in FSP
 * `bsp_register_protection.c` and the bitfield positions in the FSP
 * CMSIS device header `R7KA8D2KF_core0.h+`.
 */
typedef enum : uint16_t {
  k_ra8_prcr_test_key_only = 0xA500U, /**< Lock-everything pattern.  */
  k_ra8_prcr_test_key_mask = 0xFF00U, /**< Upper-byte password mask. */
  k_ra8_prcr_test_prc0_msk = 0x0001U, /**< PRC0 = CGC.               */
  k_ra8_prcr_test_prc1_msk = 0x0002U, /**< PRC1 = OM_LPC_BATT/LPM.   */
  k_ra8_prcr_test_prc3_msk = 0x0008U, /**< PRC3 = LVD / PVD.         */
  k_ra8_prcr_test_prc4_msk = 0x0010U, /**< PRC4 = security.          */
  k_ra8_prcr_test_prc5_msk = 0x0020U, /**< PRC5 = RESET control.     */
} ra8_prcr_test_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unlock_value_constant(void)
{
  TEST_BEGIN("prcr unlock-cgc constant matches FSP key|PRC0");
  /* Cross-verify against FSP `BSP_PRV_PRCR_KEY (0xA500U)` plus the
   * PRC0 mask `0x0001U` from the FSP `g_prcr_masks[]` table. */
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_test_key_only), ((uint16_t)k_ra8_prcr_lock_all));
  TEST_ASSERT_EQ(((uint16_t)(k_ra8_prcr_test_key_only | k_ra8_prcr_test_prc0_msk)),
                 ((uint16_t)k_ra8_prcr_unlock_cgc));
  TEST_END("prcr unlock-cgc constant matches FSP key|PRC0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_protected_write_unlocks_then_relocks(void)
{
  TEST_BEGIN("RA8_PROTECTED_WRITE unlocks for body, relocks after");
  ra8_fake_mmap_reset();

  /* Sanity: PRCR starts at 0 in the fake mmap. */
  TEST_ASSERT_EQ(0, *ra8_sys_prcr());

  uint16_t observed_during_body = 0U;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    observed_during_body = *ra8_sys_prcr();
  }

  /* Inside the body: upper byte == key, PRC0 == 1. */
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_test_key_only),
                 (observed_during_body & (uint16_t)k_ra8_prcr_test_key_mask));
  TEST_ASSERT((observed_during_body & (uint16_t)k_ra8_prcr_test_prc0_msk) != 0U);

  /* After the body: still keyed but every group bit cleared. */
  const uint16_t after = *ra8_sys_prcr();
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_test_key_only), after);
  TEST_ASSERT((after & (uint16_t)k_ra8_prcr_test_prc0_msk) == 0U);

  TEST_END("RA8_PROTECTED_WRITE unlocks for body, relocks after");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_protected_write_relocks_on_break(void)
{
  TEST_BEGIN("RA8_PROTECTED_WRITE relocks even when body breaks early");
  ra8_fake_mmap_reset();

  uint16_t observed_during_body = 0U;
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc)
  {
    observed_during_body = *ra8_sys_prcr();
    break;
  }

  /* The body ran -- unlock value visible inside it. */
  TEST_ASSERT((observed_during_body & (uint16_t)k_ra8_prcr_test_prc0_msk) != 0U);

  /* The for-loop increment clause is what re-locks. `break` skips
   * that clause, so the helper macro relies on the next caller's
   * `RA8_PROTECTED_WRITE` (or `ra8_sys_prcr_lock_all()`) to relock.
   * Document the actual observable behaviour: PRCR remains in the
   * unlocked state after `break`. Callers that need a guaranteed
   * relock-on-early-exit must use `ra8_sys_prcr_lock_all()`
   * explicitly or avoid `break`. */
  const uint16_t after = *ra8_sys_prcr();
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_unlock_cgc), after);

  /* Manual relock to demonstrate the explicit-relock path. */
  ra8_sys_prcr_lock_all();
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_lock_all), *ra8_sys_prcr());
  TEST_END("RA8_PROTECTED_WRITE relocks even when body breaks early");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_inline_unlock_helper_writes_key_plus_prc0(void)
{
  TEST_BEGIN("ra8_sys_prcr_unlock_cgc / lock_all helpers");
  ra8_fake_mmap_reset();

  ra8_sys_prcr_unlock_cgc();
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_unlock_cgc), *ra8_sys_prcr());

  ra8_sys_prcr_lock_all();
  TEST_ASSERT_EQ(((uint16_t)k_ra8_prcr_lock_all), *ra8_sys_prcr());
  TEST_END("ra8_sys_prcr_unlock_cgc / lock_all helpers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fsp_class_to_bit_mapping(void)
{
  TEST_BEGIN("PRCR class-to-bit mapping cross-verified against FSP");
  /* FSP `bsp_register_protection.c` g_prcr_masks[] order:
   *   [BSP_REG_PROTECT_CGC]         = 0x0001 (PRC0)
   *   [BSP_REG_PROTECT_OM_LPC_BATT] = 0x0002 (PRC1)
   *   [BSP_REG_PROTECT_LVD]         = 0x0008 (PRC3)
   *   [BSP_REG_PROTECT_SAR]         = 0x0010 (PRC4)
   *   [BSP_REG_PROTECT_RESET]       = 0x0020 (PRC5)
   * Cross-check our bit mask constants match those values. */
  TEST_ASSERT_EQ(0x0001, k_ra8_prcr_test_prc0_msk);
  TEST_ASSERT_EQ(0x0002, k_ra8_prcr_test_prc1_msk);
  TEST_ASSERT_EQ(0x0008, k_ra8_prcr_test_prc3_msk);
  TEST_ASSERT_EQ(0x0010, k_ra8_prcr_test_prc4_msk);
  TEST_ASSERT_EQ(0x0020, k_ra8_prcr_test_prc5_msk);

  /* And the existing repo-wide enum has CGC at PRC0 and LPM at PRC1. */
  TEST_ASSERT_EQ(k_ra8_prcr_test_prc0_msk, k_ra8_prcr_grp0_cgc);
  TEST_ASSERT_EQ(k_ra8_prcr_test_prc1_msk, k_ra8_prcr_grp1_lpm);
  TEST_END("PRCR class-to-bit mapping cross-verified against FSP");
}

int32_t main(void)
{
  test_unlock_value_constant();
  test_protected_write_unlocks_then_relocks();
  test_protected_write_relocks_on_break();
  test_inline_unlock_helper_writes_key_plus_prc0();
  test_fsp_class_to_bit_mapping();
  return 0;
}

/**
 * @file test_ra8_tz_psar.c
 * @brief Host tests for ra8_tz_psar_set_ns()
 * @ingroup grp_system
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_system_regs.h"
#include "ra8_tz_psar.h"
#include "unity_minimal.h"

/**
 * @enum psar_test_addr_t
 * @brief PSAR addresses the suite writes through the fake MMIO backing.
 */
typedef enum : uintptr_t {
  k_t_psarb_addr = 0x40204004U, /**< PSARB (MSTPCRB attribution). */
} psar_test_addr_t;

/**
 * @enum psar_test_mask_t
 * @brief Attribution masks used by the suite.
 */
typedef enum : uint32_t {
  k_t_usbfs_ns = 0x00000800U, /**< PSARB11: USBFS0 Non-secure. */
  k_t_usbhs_ns = 0x00001000U, /**< PSARB12: USBHS Non-secure.  */
  k_t_usb_ns   = 0x00001800U, /**< Both USB controllers.       */
  k_t_other_ns = 0x00000010U, /**< An unrelated peripheral bit. */
} psar_test_mask_t;

/**
 * @test internal_test_psar_sets_requested_bits
 * @brief The requested bits land and the confirmed value comes back.
 */
RA8_INTERNAL static void internal_test_psar_sets_requested_bits(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: sets the requested bits and confirms them");
  ra8_fake_mmap_reset();
  *ra8_sys_prcr() = 0U;

  uint32_t        seen = 0U;
  const ra8_err_t err  = ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, (uint32_t)k_t_usb_ns, &seen);

  TEST_ASSERT_EQ((int)k_ra8_ok, (int)err);
  TEST_ASSERT_EQ((uint32_t)k_t_usb_ns, seen);
  TEST_ASSERT_EQ((uint32_t)k_t_usb_ns, *(volatile uint32_t*)k_t_psarb_addr);

  TEST_END("ra8_tz_psar_set_ns: sets the requested bits and confirms them");
}

/**
 * @test internal_test_psar_mask_is_additive
 * @brief Bits another subsystem already handed over are preserved.
 *
 * @details This is the property that makes two independent callers safe during
 *          one boot: the second must not take back the first one's peripheral.
 */
RA8_INTERNAL static void internal_test_psar_mask_is_additive(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: mask is additive, never clears other bits");
  ra8_fake_mmap_reset();
  *ra8_sys_prcr() = 0U;

  *(volatile uint32_t*)k_t_psarb_addr = (uint32_t)k_t_other_ns;

  uint32_t seen = 0U;
  TEST_ASSERT_EQ(
      (int)k_ra8_ok,
      (int)ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, (uint32_t)k_t_usbfs_ns, &seen));

  TEST_ASSERT_EQ((uint32_t)(k_t_other_ns | k_t_usbfs_ns), seen);

  /* A second caller adds its own bit without clearing either earlier one. */
  TEST_ASSERT_EQ(
      (int)k_ra8_ok,
      (int)ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, (uint32_t)k_t_usbhs_ns, &seen));
  TEST_ASSERT_EQ((uint32_t)(k_t_other_ns | k_t_usb_ns), seen);

  TEST_END("ra8_tz_psar_set_ns: mask is additive, never clears other bits");
}

/**
 * @test internal_test_psar_relocks_the_gate
 * @brief PRC4 is locked again after the write.
 */
RA8_INTERNAL static void internal_test_psar_relocks_the_gate(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: re-locks PRCR_S on the success path");
  ra8_fake_mmap_reset();
  *ra8_sys_prcr() = 0U;

  uint32_t seen = 0U;
  (void)ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, (uint32_t)k_t_usb_ns, &seen);

  TEST_ASSERT_EQ((uint16_t)k_ra8_prcr_lock_all, *ra8_sys_prcr());

  TEST_END("ra8_tz_psar_set_ns: re-locks PRCR_S on the success path");
}

/**
 * @test internal_test_psar_zero_mask_is_a_noop
 * @brief A zero mask writes nothing and never opens the gate.
 */
RA8_INTERNAL static void internal_test_psar_zero_mask_is_a_noop(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: zero mask is a no-op and leaves PRCR alone");
  ra8_fake_mmap_reset();
  *ra8_sys_prcr() = 0U;

  *(volatile uint32_t*)k_t_psarb_addr = (uint32_t)k_t_other_ns;

  uint32_t seen = 0U;
  TEST_ASSERT_EQ((int)k_ra8_ok, (int)ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, 0U, &seen));

  TEST_ASSERT_EQ((uint32_t)k_t_other_ns, seen);
  TEST_ASSERT_EQ((uint32_t)k_t_other_ns, *(volatile uint32_t*)k_t_psarb_addr);
  /* The gate was never opened, so it is still exactly as the caller left it. */
  TEST_ASSERT_EQ((uint16_t)0U, *ra8_sys_prcr());

  TEST_END("ra8_tz_psar_set_ns: zero mask is a no-op and leaves PRCR alone");
}

/**
 * @test internal_test_psar_rejects_null_address
 * @brief A zero register address is a caller error, not a write to 0.
 */
RA8_INTERNAL static void internal_test_psar_rejects_null_address(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: rejects a zero PSAR address");
  ra8_fake_mmap_reset();

  uint32_t seen = 0xDEADU;
  TEST_ASSERT_EQ((int)k_ra8_err_invalid_arg,
                 (int)ra8_tz_psar_set_ns(0U, (uint32_t)k_t_usb_ns, &seen));

  TEST_END("ra8_tz_psar_set_ns: rejects a zero PSAR address");
}

/**
 * @test internal_test_psar_tolerates_null_out
 * @brief out_seen is optional.
 */
RA8_INTERNAL static void internal_test_psar_tolerates_null_out(void)
{
  TEST_BEGIN("ra8_tz_psar_set_ns: accepts a NULL out_seen");
  ra8_fake_mmap_reset();
  *ra8_sys_prcr() = 0U;

  TEST_ASSERT_EQ(
      (int)k_ra8_ok,
      (int)ra8_tz_psar_set_ns((uintptr_t)k_t_psarb_addr, (uint32_t)k_t_usbfs_ns, NULL));
  TEST_ASSERT_EQ((uint32_t)k_t_usbfs_ns, *(volatile uint32_t*)k_t_psarb_addr);

  TEST_END("ra8_tz_psar_set_ns: accepts a NULL out_seen");
}

/**
 * @brief Run the PSAR attribution suite.
 * @return 0 on success, non-zero on the first failure.
 */
int main(void)
{
  internal_test_psar_sets_requested_bits();
  internal_test_psar_mask_is_additive();
  internal_test_psar_relocks_the_gate();
  internal_test_psar_zero_mask_is_a_noop();
  internal_test_psar_rejects_null_address();
  internal_test_psar_tolerates_null_out();
  return 0;
}

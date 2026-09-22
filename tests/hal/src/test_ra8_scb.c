/**
 * @file test_ra8_scb.c
 * @brief Unit tests for the ra8_scb Cortex-M85 System Control Block driver.
 *
 * @details
 * The host build maps the core's register window (::ra8_fake_mmap), so each
 * test plants values directly at the SCB addresses and then checks that the
 * driver read the right ones, or writes through the driver and checks the raw
 * register. Covers the fault-status read (field mapping + NULL guard), the
 * VTOR set / get round-trip, and the DEMCR.TRCENA query / enable pair
 * (including bit preservation).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_scb.h"
#include "unity_minimal.h"

/**
 * @enum scb_reg_addr_t
 * @brief Arm v8-M SCB register addresses this test plants values in.
 *
 * @details Addresses are `uintptr_t` because they are addresses; they mirror
 *          the driver's private map and let the test drive the same window.
 */
typedef enum : uintptr_t {
  k_scb_vtor_addr  = 0xE000ED08UL, /**< Vector Table Offset Register.   */
  k_scb_cfsr_addr  = 0xE000ED28UL, /**< Configurable Fault Status.      */
  k_scb_hfsr_addr  = 0xE000ED2CUL, /**< HardFault Status.               */
  k_scb_dfsr_addr  = 0xE000ED30UL, /**< Debug Fault Status.             */
  k_scb_mmfar_addr = 0xE000ED34UL, /**< MemManage Fault Address.        */
  k_scb_bfar_addr  = 0xE000ED38UL, /**< BusFault Address.               */
  k_scb_afsr_addr  = 0xE000ED3CUL, /**< Auxiliary Fault Status.         */
  k_scb_sfsr_addr  = 0xE000EDE4UL, /**< SecureFault Status.             */
  k_scb_sfar_addr  = 0xE000EDE8UL, /**< SecureFault Address.            */
  k_scb_demcr_addr = 0xE000EDFCUL, /**< Debug Exception + Monitor Ctrl. */
} scb_reg_addr_t;

/**
 * @brief Typed pointer to a 32-bit SCB register for direct test planting.
 *
 * @details The same address-accessor pattern the driver (and ra8_cache.c) use
 *          for Arm-core registers, so each plant / probe site reads
 *          `*scb_test_reg(k_...)` instead of a raw `(volatile uint32_t*)` cast.
 *
 * @param[in] addr One of ::scb_reg_addr_t.
 *
 * @return Volatile pointer aliasing the fake-mapped register.
 * @retval (volatile uint32_t*)addr  Alias of the mapped register.
 *
 * @pre The core window is mapped (::ra8_fake_mmap_reset has run).
 * @pre @p addr is one of the SCB addresses under test.
 * @post No state changed by forming the pointer.
 * @post The returned pointer aliases the live fake register.
 * @since 0.1.0
 */
static volatile uint32_t* scb_test_reg(scb_reg_addr_t addr)
{
  return (volatile uint32_t*)addr;
}

/**
 * @enum scb_planted_t
 * @brief Distinct values planted in the fault registers, plus the TRCENA bit.
 *
 * @details A 0x5EC00000 ramp, one step per register, so no two are equal: a
 *          read that landed CFSR into `hfsr` (or left a stale zero) fails a
 *          specific assertion instead of passing.
 */
typedef enum : uint32_t {
  k_plant_cfsr   = 0x5EC00000UL, /**< Planted in CFSR.                        */
  k_plant_hfsr   = 0x5EC00004UL, /**< Planted in HFSR.                        */
  k_plant_dfsr   = 0x5EC00008UL, /**< Planted in DFSR.                        */
  k_plant_mmfar  = 0x5EC0000CUL, /**< Planted in MMFAR.                       */
  k_plant_bfar   = 0x5EC00010UL, /**< Planted in BFAR.                        */
  k_plant_afsr   = 0x5EC00014UL, /**< Planted in AFSR.                        */
  k_plant_sfsr   = 0x5EC00018UL, /**< Planted in SFSR.                        */
  k_plant_sfar   = 0x5EC0001CUL, /**< Planted in SFAR.                        */
  k_demcr_trcena = 0x01000000UL, /**< DEMCR.TRCENA -- bit 24.                 */
  k_demcr_other  = 0x00010000UL, /**< A non-TRCENA DEMCR bit (MON_EN family). */
  k_vtor_base_a  = 0x02000000UL, /**< First VTOR round-trip value.            */
  k_vtor_base_b  = 0x22040000UL, /**< Second VTOR round-trip value.           */
} scb_planted_t;

/**
 * @test scb_read_fault_status_fills
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the read is a straight-line
 * eight-register copy; the only decision, the NULL guard, is exercised by
 * ``test_scb_read_fault_status_null``)
 */
static void test_scb_read_fault_status_fills(void)
{
  TEST_BEGIN("ra8_scb_read_fault_status maps every register");
  ra8_fake_mmap_reset();

  *scb_test_reg(k_scb_cfsr_addr)  = k_plant_cfsr;
  *scb_test_reg(k_scb_hfsr_addr)  = k_plant_hfsr;
  *scb_test_reg(k_scb_dfsr_addr)  = k_plant_dfsr;
  *scb_test_reg(k_scb_mmfar_addr) = k_plant_mmfar;
  *scb_test_reg(k_scb_bfar_addr)  = k_plant_bfar;
  *scb_test_reg(k_scb_afsr_addr)  = k_plant_afsr;
  *scb_test_reg(k_scb_sfsr_addr)  = k_plant_sfsr;
  *scb_test_reg(k_scb_sfar_addr)  = k_plant_sfar;

  ra8_scb_fault_status_t fs = {};
  TEST_ASSERT_EQ((long)k_ra8_ok, (long)ra8_scb_read_fault_status(&fs));

  TEST_ASSERT_EQ((long)k_plant_cfsr, (long)fs.cfsr);
  TEST_ASSERT_EQ((long)k_plant_hfsr, (long)fs.hfsr);
  TEST_ASSERT_EQ((long)k_plant_dfsr, (long)fs.dfsr);
  TEST_ASSERT_EQ((long)k_plant_mmfar, (long)fs.mmfar);
  TEST_ASSERT_EQ((long)k_plant_bfar, (long)fs.bfar);
  TEST_ASSERT_EQ((long)k_plant_afsr, (long)fs.afsr);
  TEST_ASSERT_EQ((long)k_plant_sfsr, (long)fs.sfsr);
  TEST_ASSERT_EQ((long)k_plant_sfar, (long)fs.sfar);

  TEST_END("ra8_scb_read_fault_status maps every register");
}

/**
 * @test scb_read_fault_status_null
 *
 * @par MC/DC:
 * Decision: the ``RA8_CHECK_NULL_PTR(out, ...)`` guard (1 condition).
 * - ``test_scb_read_fault_status_fills`` drives it FALSE (valid pointer -> ok).
 * - This case drives it TRUE (NULL -> k_ra8_err_null_ptr).
 * Both outcomes of the sole condition are exercised across the suite.
 */
static void test_scb_read_fault_status_null(void)
{
  TEST_BEGIN("ra8_scb_read_fault_status rejects NULL");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ((long)k_ra8_err_null_ptr, (long)ra8_scb_read_fault_status(nullptr));
  TEST_END("ra8_scb_read_fault_status rejects NULL");
}

/**
 * @test scb_vtor_roundtrip
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- ::ra8_scb_set_vtor and
 * ::ra8_scb_get_vtor are a straight store and load)
 */
static void test_scb_vtor_roundtrip(void)
{
  TEST_BEGIN("ra8_scb_set_vtor / get_vtor round-trip");
  ra8_fake_mmap_reset();

  ra8_scb_set_vtor((uintptr_t)k_vtor_base_a);
  TEST_ASSERT_EQ((long)k_vtor_base_a, (long)*scb_test_reg(k_scb_vtor_addr));
  TEST_ASSERT_EQ((long)k_vtor_base_a, (long)ra8_scb_get_vtor());

  /* A second, different value proves get_vtor reads live state, not a const. */
  ra8_scb_set_vtor((uintptr_t)k_vtor_base_b);
  TEST_ASSERT_EQ((long)k_vtor_base_b, (long)ra8_scb_get_vtor());

  TEST_END("ra8_scb_set_vtor / get_vtor round-trip");
}

/**
 * @test scb_trace_query_and_enable
 *
 * @par MC/DC:
 * Decision: ``(demcr & TRCENA) != 0`` inside ::ra8_scb_trace_enabled
 * (1 condition). This case drives it FALSE (TRCENA clear -> false) and then
 * TRUE (after ::ra8_scb_trace_enable sets it -> true), covering both outcomes.
 */
static void test_scb_trace_query_and_enable(void)
{
  TEST_BEGIN("ra8_scb_trace_enabled / trace_enable + bit preservation");
  ra8_fake_mmap_reset();

  /* Plant an unrelated DEMCR bit and clear TRCENA. */
  *scb_test_reg(k_scb_demcr_addr) = k_demcr_other;
  TEST_ASSERT(!ra8_scb_trace_enabled());

  ra8_scb_trace_enable();
  TEST_ASSERT(ra8_scb_trace_enabled());

  /* TRCENA now set, and the pre-existing bit must be preserved (RMW, not
   * a blind store). */
  const uint32_t demcr = *scb_test_reg(k_scb_demcr_addr);
  TEST_ASSERT_EQ((long)(k_demcr_trcena | k_demcr_other), (long)demcr);

  TEST_END("ra8_scb_trace_enabled / trace_enable + bit preservation");
}

int main(void)
{
  test_scb_read_fault_status_fills();
  test_scb_read_fault_status_null();
  test_scb_vtor_roundtrip();
  test_scb_trace_query_and_enable();
  return 0;
}

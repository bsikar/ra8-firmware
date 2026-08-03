/**
 * @file test_app_scb_diag_demo.c
 * @brief Integration test: scb_diag_demo example probe + verdict logic.
 *
 * @details
 * Mirrors the per-cycle logic of
 * examples/ek_ra8d2/hw_pending/scb_diag_demo/main.c
 * (``scb_demo_probe_and_report``) against the fake MMIO map: query VTOR, unlock
 * the trace block, dump the fault-status snapshot, and compute the demo's
 * PASS/FAIL verdict. Asserts the VTOR readback and the fault-status field
 * mapping the demo prints, and exercises the demo's compound verdict decision
 * ``(err == ok) && (cfsr == 0) && (hfsr == 0)`` with MC/DC vectors -- the same
 * decision that gates the ``scb: probe PASS`` line the HIL scrape keys on.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_scb.h"
#include "unity_minimal.h"

/**
 * @enum app_scb_reg_addr_t
 * @brief SCB addresses the test plants values at (the demo's read targets).
 */
typedef enum : uintptr_t {
  k_app_scb_vtor_addr  = 0xE000ED08UL, /**< Vector Table Offset Register. */
  k_app_scb_cfsr_addr  = 0xE000ED28UL, /**< Configurable Fault Status.    */
  k_app_scb_hfsr_addr  = 0xE000ED2CUL, /**< HardFault Status.             */
  k_app_scb_dfsr_addr  = 0xE000ED30UL, /**< Debug Fault Status.           */
  k_app_scb_mmfar_addr = 0xE000ED34UL, /**< MemManage Fault Address.      */
  k_app_scb_bfar_addr  = 0xE000ED38UL, /**< BusFault Address.             */
  k_app_scb_afsr_addr  = 0xE000ED3CUL, /**< Auxiliary Fault Status.       */
  k_app_scb_sfsr_addr  = 0xE000EDE4UL, /**< SecureFault Status.           */
  k_app_scb_sfar_addr  = 0xE000EDE8UL, /**< SecureFault Address.          */
} app_scb_reg_addr_t;

/**
 * @enum app_scb_planted_t
 * @brief Distinct values planted per register (a 0xDEB00000 ramp).
 */
typedef enum : uint32_t {
  k_app_plant_vtor  = 0x02000000UL, /**< Planted VTOR base.                  */
  k_app_plant_cfsr  = 0xDEB00000UL, /**< Planted in CFSR.                    */
  k_app_plant_hfsr  = 0xDEB00004UL, /**< Planted in HFSR.                    */
  k_app_plant_dfsr  = 0xDEB00008UL, /**< Planted in DFSR.                    */
  k_app_plant_mmfar = 0xDEB0000CUL, /**< Planted in MMFAR.                   */
  k_app_plant_bfar  = 0xDEB00010UL, /**< Planted in BFAR.                    */
  k_app_plant_afsr  = 0xDEB00014UL, /**< Planted in AFSR.                    */
  k_app_plant_sfsr  = 0xDEB00018UL, /**< Planted in SFSR.                    */
  k_app_plant_sfar  = 0xDEB0001CUL, /**< Planted in SFAR.                    */
  k_app_fault_bit   = 0x00000082UL, /**< A latched fault: nonzero CFSR/HFSR. */
} app_scb_planted_t;

/**
 * @brief Typed pointer to a 32-bit SCB register for direct test planting.
 *
 * @details The same address-accessor pattern the driver uses, so each plant
 *          site reads `*app_scb_reg(k_...)` instead of a raw `(volatile ... *)`
 *          cast (keeps the test out of the raw-cast citation form).
 *
 * @param[in] addr One of ::app_scb_reg_addr_t.
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
static volatile uint32_t* app_scb_reg(app_scb_reg_addr_t addr)
{
  return (volatile uint32_t*)addr;
}

/**
 * @brief Mirror of the demo's PASS verdict predicate.
 *
 * @details Reproduces the return of ``scb_demo_probe_and_report`` in
 *          examples/ek_ra8d2/hw_pending/scb_diag_demo/main.c so the compound
 *          decision that gates ``scb: probe PASS`` can be exercised directly.
 *
 * @param[in] err  Result of the fault-status read.
 * @param[in] cfsr Captured CFSR value.
 * @param[in] hfsr Captured HFSR value.
 *
 * @return Whether the boot is fault-clean.
 * @retval true  ``err == k_ra8_ok`` and both CFSR and HFSR are zero.
 * @retval false The read failed or a fault is latched in CFSR / HFSR.
 *
 * @pre None.
 * @pre Inputs are the values the demo would have read.
 * @post No state is modified (pure predicate).
 * @post The result matches the demo's ``scb: probe PASS`` gate.
 * @since 0.1.0
 */
static bool app_scb_demo_verdict(ra8_err_t err, uint32_t cfsr, uint32_t hfsr)
{
  return (err == k_ra8_ok) && (cfsr == 0U) && (hfsr == 0U);
}

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @test scb_diag_demo_query_and_dump
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the demo's query + dump is
 * a straight-line VTOR read followed by an eight-register snapshot)
 */
static void test_scb_diag_demo_query_and_dump(void)
{
  reset_world();
  TEST_BEGIN("scb_diag_demo: VTOR query + fault-status dump");

  ra8_scb_set_vtor((uintptr_t)k_app_plant_vtor);
  *app_scb_reg(k_app_scb_cfsr_addr)  = k_app_plant_cfsr;
  *app_scb_reg(k_app_scb_hfsr_addr)  = k_app_plant_hfsr;
  *app_scb_reg(k_app_scb_dfsr_addr)  = k_app_plant_dfsr;
  *app_scb_reg(k_app_scb_mmfar_addr) = k_app_plant_mmfar;
  *app_scb_reg(k_app_scb_bfar_addr)  = k_app_plant_bfar;
  *app_scb_reg(k_app_scb_afsr_addr)  = k_app_plant_afsr;
  *app_scb_reg(k_app_scb_sfsr_addr)  = k_app_plant_sfsr;
  *app_scb_reg(k_app_scb_sfar_addr)  = k_app_plant_sfar;

  /* The demo queries VTOR (never relocates it). */
  TEST_ASSERT_EQ((long)k_app_plant_vtor, (long)ra8_scb_get_vtor());

  /* The demo unlocks the trace block and reads it back set. */
  ra8_scb_trace_enable();
  TEST_ASSERT(ra8_scb_trace_enabled());

  /* The demo dumps the whole snapshot -- every field must map. */
  ra8_scb_fault_status_t fs = {};
  TEST_ASSERT_EQ((long)k_ra8_ok, (long)ra8_scb_read_fault_status(&fs));
  TEST_ASSERT_EQ((long)k_app_plant_cfsr, (long)fs.cfsr);
  TEST_ASSERT_EQ((long)k_app_plant_hfsr, (long)fs.hfsr);
  TEST_ASSERT_EQ((long)k_app_plant_dfsr, (long)fs.dfsr);
  TEST_ASSERT_EQ((long)k_app_plant_mmfar, (long)fs.mmfar);
  TEST_ASSERT_EQ((long)k_app_plant_bfar, (long)fs.bfar);
  TEST_ASSERT_EQ((long)k_app_plant_afsr, (long)fs.afsr);
  TEST_ASSERT_EQ((long)k_app_plant_sfsr, (long)fs.sfsr);
  TEST_ASSERT_EQ((long)k_app_plant_sfar, (long)fs.sfar);

  TEST_END("scb_diag_demo: VTOR query + fault-status dump");
}

/**
 * @test scb_diag_demo_verdict_mcdc
 *
 * @par MC/DC:
 * Decision (mirrored from the demo): ``(err == k_ra8_ok) && (cfsr == 0) &&
 * (hfsr == 0)`` -- 3 atomic conditions, N+1 = 4 vectors:
 * - Vector 1: err=ok,   cfsr=0,       hfsr=0       -> true  (control: all true)
 * - Vector 2: err=fault,cfsr=0,       hfsr=0       -> false (varies err only)
 * - Vector 3: err=ok,   cfsr=nonzero, hfsr=0       -> false (varies cfsr only)
 * - Vector 4: err=ok,   cfsr=0,       hfsr=nonzero -> false (varies hfsr only)
 * 1+2 prove err independently decides; 1+3 prove cfsr; 1+4 prove hfsr.
 */
static void test_scb_diag_demo_verdict_mcdc(void)
{
  TEST_BEGIN("scb_diag_demo: verdict decision MC/DC");
  TEST_ASSERT(app_scb_demo_verdict(k_ra8_ok, 0U, 0U));                         /* V1 */
  TEST_ASSERT(!app_scb_demo_verdict(k_ra8_err_null_ptr, 0U, 0U));              /* V2 */
  TEST_ASSERT(!app_scb_demo_verdict(k_ra8_ok, (uint32_t)k_app_fault_bit, 0U)); /* V3 */
  TEST_ASSERT(!app_scb_demo_verdict(k_ra8_ok, 0U, (uint32_t)k_app_fault_bit)); /* V4 */
  TEST_END("scb_diag_demo: verdict decision MC/DC");
}

/**
 * @test scb_diag_demo_clean_boot_passes
 *
 * @par MC/DC:
 * (the compound verdict is covered by ``test_scb_diag_demo_verdict_mcdc``; this
 * case is the end-to-end clean path through the real driver, proving the demo
 * verdict keys ONLY on CFSR + HFSR -- a nonzero DFSR must not fail it)
 */
static void test_scb_diag_demo_clean_boot_passes(void)
{
  reset_world();
  TEST_BEGIN("scb_diag_demo: clean boot -> probe PASS");

  /* Fault-clean: CFSR and HFSR zero. DFSR planted nonzero to prove the demo
   * verdict ignores it (it gates only on CFSR + HFSR). */
  *app_scb_reg(k_app_scb_cfsr_addr) = 0U;
  *app_scb_reg(k_app_scb_hfsr_addr) = 0U;
  *app_scb_reg(k_app_scb_dfsr_addr) = k_app_plant_dfsr;

  ra8_scb_trace_enable();
  ra8_scb_fault_status_t fs  = {};
  const ra8_err_t        err = ra8_scb_read_fault_status(&fs);
  TEST_ASSERT(app_scb_demo_verdict(err, fs.cfsr, fs.hfsr));

  TEST_END("scb_diag_demo: clean boot -> probe PASS");
}

/**
 * @test scb_diag_demo_latched_fault_fails
 *
 * @par MC/DC:
 * (covered by ``test_scb_diag_demo_verdict_mcdc``; this is the end-to-end
 * faulted path through the real driver: a latched CFSR bit must fail the
 * verdict, and separately a latched HFSR bit must too)
 */
static void test_scb_diag_demo_latched_fault_fails(void)
{
  reset_world();
  TEST_BEGIN("scb_diag_demo: latched fault -> probe FAIL");

  /* A CFSR fault bit fails the verdict. */
  *app_scb_reg(k_app_scb_cfsr_addr) = k_app_fault_bit;
  *app_scb_reg(k_app_scb_hfsr_addr) = 0U;
  ra8_scb_fault_status_t fs         = {};
  ra8_err_t              err        = ra8_scb_read_fault_status(&fs);
  TEST_ASSERT(!app_scb_demo_verdict(err, fs.cfsr, fs.hfsr));

  /* An HFSR fault bit alone also fails the verdict. */
  *app_scb_reg(k_app_scb_cfsr_addr) = 0U;
  *app_scb_reg(k_app_scb_hfsr_addr) = k_app_fault_bit;
  fs                                = (ra8_scb_fault_status_t){};
  err                               = ra8_scb_read_fault_status(&fs);
  TEST_ASSERT(!app_scb_demo_verdict(err, fs.cfsr, fs.hfsr));

  TEST_END("scb_diag_demo: latched fault -> probe FAIL");
}

int main(void)
{
  test_scb_diag_demo_query_and_dump();
  test_scb_diag_demo_verdict_mcdc();
  test_scb_diag_demo_clean_boot_passes();
  test_scb_diag_demo_latched_fault_fails();
  return 0;
}

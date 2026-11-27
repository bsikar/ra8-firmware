/**
 * @file test_ra8_nsc_dualcore.c
 * @brief Host unit tests for TrustZone dual-core NSC veneers (`ra8_nsc_dualcore`).
 *
 * @details
 * Exercises ``ra8_nsc_dualcore_start_m33``, ``ra8_nsc_dualcore_park_m85``, and
 * ``ra8_nsc_dualcore_notify_cpu1`` against the ``ra8_fake_mmap`` peripheral,
 * code-MRAM, and core-PPB backing windows. Verifies input bounds checking,
 * blank-vector rejection, CPSCU/SRAM3/IPCSAR/PMSAR register mutations, HOCO
 * clock-stop & restore around Sleep entry, temporary SW1/SW2 NS delegation,
 * and IPC0 IRQ0 wakeup handling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_hw_err.h"
#include "ra8_ipc.h"
#include "ra8_lpm.h"
#include "ra8_nsc_dualcore.h"
#include "unity_minimal.h"

/** @brief Target register and CPU1 image constants mirrored from production map. */
typedef enum : uintptr_t {
  k_t_cpu1_mram_base  = 0x020C0000UL,
  k_t_cpu1_stack_top  = 0x221A0000UL,
  k_t_sramsabar3_addr = 0x4000840CUL,
  k_t_bussara_addr    = 0x40008100UL,
  k_t_ipcsar_addr     = 0x40008610UL,
  k_t_ipcpar_addr     = 0x40008614UL,
  k_t_pmsar0_addr     = 0x40400D30UL,
  k_t_pmsar6_addr     = 0x40400D48UL,
  k_t_ipc0_sta0_addr  = 0x400200C0UL,
  k_t_ipc1_iset0_addr = 0x40020104UL,
  k_t_cpuctrl_actcsr  = 0x4001E584UL,
} t_dc_addr_t;

/** @brief Seed a valid CPU1 vector table at `0x020C0000` in fake code-MRAM. */
RA8_INTERNAL static void seed_valid_cpu1_vectors(void)
{
  volatile uint32_t* const vec = (volatile uint32_t*)k_t_cpu1_mram_base;
  vec[0U]                      = (uint32_t)k_t_cpu1_stack_top;
  vec[1U]                      = (uint32_t)(k_t_cpu1_mram_base + 0x41UL);
}

/** @brief Tear down previous IPC/ISR state and reset fake MMIO for a clean test. */
RA8_INTERNAL static void arm_cpu1_release_ok(void)
{
  (void)ra8_ipc_uninstall_isr(0U);
  (void)ra8_ipc_deinit(0U);
  (void)ra8_ipc_deinit(2U);
  ra8_fake_mmap_reset();
}

/** @brief Ensure invalid address arguments and blank flash vector table fail cleanly. */
RA8_INTERNAL static void test_start_m33_guards(void)
{
  TEST_BEGIN("ra8_nsc_dualcore: start_m33 guards");
  arm_cpu1_release_ok();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base + 4UL, k_t_cpu1_stack_top));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base, k_t_cpu1_stack_top - 4UL));

  /* Blank vector table (all zeroes in fake MRAM) must return not_supported. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base, k_t_cpu1_stack_top));
  TEST_END("ra8_nsc_dualcore: start_m33 guards");
}

/** @brief Verify SRAM3 boundary, IPCSAR, BUSSAR, PMSAR6, and IPC init on start_m33. */
RA8_INTERNAL static void test_start_m33_configures_security_and_ipc(void)
{
  TEST_BEGIN("ra8_nsc_dualcore: start_m33 configures SRAM3, IPC, and LED1");
  arm_cpu1_release_ok();
  seed_valid_cpu1_vectors();

  /* Fake CPU_CTRL actcsr handshake passes (ACT wait defaults to true when unarmed). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base, k_t_cpu1_stack_top));

  TEST_ASSERT_EQ(0x00180000UL, *(const volatile uint32_t*)k_t_sramsabar3_addr);
  TEST_ASSERT_EQ(0x00000001UL, *(const volatile uint32_t*)k_t_bussara_addr);
  TEST_ASSERT_EQ(0x00050000UL, *(const volatile uint32_t*)k_t_ipcsar_addr);
  TEST_ASSERT_EQ(0x00000000UL, *(const volatile uint32_t*)k_t_ipcpar_addr);
  TEST_ASSERT((*(const volatile uint16_t*)k_t_pmsar6_addr & 0x0001U) != 0U);
  TEST_END("ra8_nsc_dualcore: start_m33 configures SRAM3, IPC, and LED1");
}

/** @brief Verify park_m85 preconditions and timeout vs IPC0 IRQ0 wake path. */
RA8_INTERNAL static void test_park_m85_timeout_and_ipc_wake(void)
{
  TEST_BEGIN("ra8_nsc_dualcore: park_m85 timeout and IPC wake");
  arm_cpu1_release_ok();
  seed_valid_cpu1_vectors();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base, k_t_cpu1_stack_top));

  uint32_t woken = 99U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_dualcore_park_m85(4U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_dualcore_park_m85(0U, &woken));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_dualcore_park_m85(5000U, &woken));

  /* Without IPC0 IRQ0 pending, bounded loop exits with woken == 0 and restores HOCO & PMSAR0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_park_m85(3U, &woken));
  TEST_ASSERT_EQ(0U, woken);
  bool hoco_stopped = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_clock_stop(k_ra8_lpm_clock_hoco, &hoco_stopped));
  TEST_ASSERT(!hoco_stopped);
  TEST_ASSERT_EQ(0U, *(const volatile uint16_t*)k_t_pmsar0_addr & 0x0300U);

  /* Latch IPC0 channel 0 IRQ0 and dispatch so s_ipc0_wake_flag triggers immediate exit. */
  *(volatile uint32_t*)k_t_ipc0_sta0_addr = 0x00000001UL;
  ra8_ipc_dispatch(0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_park_m85(4U, &woken));
  TEST_ASSERT_EQ(1U, woken);
  TEST_ASSERT_EQ(0U, *(const volatile uint16_t*)k_t_pmsar0_addr & 0x0300U);

  /* Second park after consumed wake must see woken == 0 again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_park_m85(2U, &woken));
  TEST_ASSERT_EQ(0U, woken);
  TEST_END("ra8_nsc_dualcore: park_m85 timeout and IPC wake");
}

/** @brief Verify notify_cpu1 asserts IPC1 ch0 IRQ0 bit. */
RA8_INTERNAL static void test_notify_cpu1(void)
{
  TEST_BEGIN("ra8_nsc_dualcore: notify_cpu1");
  arm_cpu1_release_ok();
  seed_valid_cpu1_vectors();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_start_m33(k_t_cpu1_mram_base, k_t_cpu1_stack_top));

  *(volatile uint32_t*)k_t_ipc1_iset0_addr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_dualcore_notify_cpu1());
  TEST_ASSERT_EQ(1U, *(const volatile uint32_t*)k_t_ipc1_iset0_addr & 1U);
  TEST_END("ra8_nsc_dualcore: notify_cpu1");
}

int main(void)
{
  test_start_m33_guards();
  test_start_m33_configures_security_and_ipc();
  test_park_m85_timeout_and_ipc_wake();
  test_notify_cpu1();
  return 0;
}

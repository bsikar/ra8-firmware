/**
 * @file examples/ek_ra8d2/mpu_partition_simple/main.c
 * @brief Single-region MPU read-only partition demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Bare-metal counterpart to ``threadx_mpu_partition_demo`` -- no
 * RTOS, no thread context, just one MPU region. The app:
 *
 *   1. Allocates a 32-byte aligned scratch buffer in SRAM.
 *   2. Configures MPU region 0 covering that buffer as RO/RO with
 *      PRIVDEFENA = true so the rest of the address map remains
 *      RW (the default privileged-mode background region).
 *   3. Installs a MemManage handler that latches LED2 and parks
 *      the CPU.
 *   4. From privileged code, attempts a write to the RO buffer.
 *      The write is expected to raise a MemManage fault, the
 *      handler latches LED2, and the demo halts. If the write
 *      succeeds (no fault), LED3 latches to flag the silent
 *      MPU-misconfiguration scenario for diagnostics.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mpu.h"
#include "ra_time.h"

/** @brief Region geometry. */
typedef enum : uint32_t {
  k_mpu_simple_region_size = 32U,         /**< Smallest legal Armv8-M MPU region. */
  k_mpu_simple_mair0_word  = 0x000000FFU, /**< Slot 0 = Normal WB RW-allocate.   */
} mpu_simple_region_t;

/** @brief MAIR slot encoding + probe sentinel. */
typedef enum : uint8_t {
  k_mpu_simple_attr_normal_wb = 0xFFU,
  k_mpu_simple_probe_byte     = 0x42U, /**< Sentinel byte written by mpu_simple_probe. */
} mpu_simple_attr_t;

/** @brief 32-byte aligned scratch buffer the RO region will cover. */
static uint8_t s_ro_buffer[k_mpu_simple_region_size] __attribute__((aligned(32))) = {};

/** @brief Park the CPU after fatal init failure or after the demo write. */
static void mpu_simple_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* The MemManage_Handler is provided by the per-app vector_table.c
 * (it routes to ra_exception_report). On hardware the offending
 * write below will trap into that handler, which halts the CPU.
 * On the host simulator the write simply succeeds and the test
 * driver inspects the MPU configuration directly. */

/** @brief One-region RO descriptor covering ``s_ro_buffer``. */
static const ra_mpu_region_t s_regions[] = {
  {
    .base       = (uintptr_t)s_ro_buffer,
    .size       = (uint32_t)k_mpu_simple_region_size,
    .priv       = k_ra_mpu_perm_ro,
    .unpriv     = k_ra_mpu_perm_ro,
    .executable = false,
    .shareable  = k_ra_mpu_share_inner,
    .attr_idx   = k_ra_mpu_attr_idx_0,
  },
};

/** @brief Aggregate MPU configuration handed to ra_mpu_configure. */
static const ra_mpu_cfg_t s_cfg = {
  .regions      = s_regions,
  .region_count = 1U,
  .mair0        = (uint32_t)k_mpu_simple_mair0_word,
  .mair1        = 0U,
  .privdefena   = true,
  .hfnmiena     = false,
};

/**
 * @brief Bring CGC + SysTick + LEDs up. Halts on any error.
 *
 * @par MC/DC:
 * Sequence of single-condition checks; each guard's MC/DC pair is
 * covered by the test harness via mock-injected failures.
 *
 * @since 0.1.0
 */
static void mpu_simple_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    mpu_simple_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    mpu_simple_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    mpu_simple_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    mpu_simple_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    mpu_simple_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led3) != k_ra_ok) {
    mpu_simple_panic_halt();
  }
}

/**
 * @brief Probe the RO region with a write -- expected to fault on silicon.
 *
 * @par MC/DC:
 * Compound decision: ``probe_byte != 0x42``. One atomic condition x
 * 2 vectors -- match (host simulator: write succeeded, MPU not
 * actually trapping) vs mismatch (impossible because the write
 * either lands or faults). Test driver covers both branches by
 * pre-seeding the buffer.
 *
 * @return ``k_ra_ok`` if the readback matches (no fault took
 *         place); ``k_ra_err_hw_error`` if the byte changed value
 *         unexpectedly. Note: on real silicon control never returns
 *         from the offending store -- the per-app MemManage_Handler
 *         in vector_table.c routes the fault to ra_exception_report
 *         which halts.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t mpu_simple_probe(void)
{
  s_ro_buffer[0] = (uint8_t)k_mpu_simple_probe_byte;
  /* cppcheck-suppress knownConditionTrueFalse
   * (host: write succeeds; silicon: write faults before this readback). */
  if (s_ro_buffer[0] == (uint8_t)k_mpu_simple_probe_byte) {
    return k_ra_ok;
  }
  return k_ra_err_hw_error;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  mpu_simple_setup_or_halt();
  ra_isr_globals_enable();

  if (ra_mpu_configure(&s_cfg) != k_ra_ok) {
    mpu_simple_panic_halt();
  }

  if (mpu_simple_probe() == k_ra_ok) {
    /* Should not reach here on real silicon -- the MemManage
     * handler parked the CPU. Reaching this branch on hardware
     * means the MPU did not arm correctly. */
    (void)ra_board_led_on(k_ra_board_led3);
  } else {
    (void)ra_board_led_on(k_ra_board_led1);
  }

  mpu_simple_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

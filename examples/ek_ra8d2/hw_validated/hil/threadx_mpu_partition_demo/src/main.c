/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_mpu_partition_demo/src/main.c
 * @brief Eclipse ThreadX + Arm v8-M MPU partition demo for RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates partitioning the system address map into a small
 * static MPU region table at boot, then handing control to a
 * single ThreadX worker that blinks ``LED1`` at 1 Hz to prove the
 * MPU configuration did not break ordinary code execution.
 *
 * Region layout (programmed via ``ra8_mpu_configure``):
 *
 *   - Region 0: MRAM (RX, exec)            -- 1 MiB at 0x02000000
 *   - Region 1: SRAM (RW, no-exec)         -- 1 MiB at 0x22000000
 *   - Region 2: Peripheral block (RW, no-exec) -- 256 MiB at 0x40000000
 *
 * Default-on PRIVDEFENA lets privileged accesses outside the
 * declared regions fall through to the architectural defaults so
 * the kernel can still touch its bookkeeping pages.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mpu.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables.
 * --------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mpu_thread_stack_bytes = 1024U, /**< MPU thread stack bytes. */
} mpu_stack_t;

typedef enum : uint8_t {
  k_mpu_thread_priority = 4U, /**< MPU thread priority. */
} mpu_priority_t;

typedef enum : uint16_t {
  k_mpu_blink_ticks = 1000U, /**< MPU blink ticks. */
} mpu_period_t;

/**
 * @brief Region base addresses for the MPU partition table.
 */
typedef enum : uintptr_t {
  k_mpu_region_mram_base = 0x02000000UL, /**< MPU region MRAM base. */
  k_mpu_region_sram_base = 0x22000000UL, /**< MPU region SRAM base. */
  k_mpu_region_peri_base = 0x40000000UL, /**< MPU region peri base. */
} mpu_region_base_t;

/**
 * @brief Region sizes (powers of two, bytes).
 */
typedef enum : uint32_t {
  k_mpu_region_mram_size = 0x00100000UL, /**< 1 MiB.   */
  k_mpu_region_sram_size = 0x00100000UL, /**< 1 MiB.   */
  k_mpu_region_peri_size = 0x10000000UL, /**< 256 MiB. */
} mpu_region_size_t;

/**
 * @brief MAIR attribute encodings used by the region table below.
 *
 * @details
 * Attr 0 = Normal memory, inner+outer write-back, RW-allocate,
 * non-transient (encoding 0xFF per Armv8-M Architecture Reference
 * Manual D1.6.7 "Memory attribute encodings"). Required for the MRAM
 * code region: instruction fetches from device-typed memory are
 * UNPREDICTABLE and most cores HardFault. Attr 1 = device-nGnRnE
 * (encoding 0x04) for the peripheral region.
 */
typedef enum : uint8_t {
  k_mpu_mair_attr0_normal_wb = 0xFFU, /**< MPU mair attr0 normal wb. */
  k_mpu_mair_attr1_device    = 0x04U, /**< MPU mair attr1 device.    */
} mpu_mair_encoding_t;

typedef enum : uint32_t {
  k_mpu_mair0_word = ((uint32_t)k_mpu_mair_attr1_device << 8U) |
                     (uint32_t)k_mpu_mair_attr0_normal_wb, /**< MPU mair0 word. */
} mpu_mair0_t;

/**
 * @brief Static MPU region table installed at boot.
 *
 * @details
 * Three coarse regions cover code, data, and the peripheral block.
 * MRAM + SRAM use attr_idx 0 (Normal write-back); the peripheral
 * region uses attr_idx 1 (device-nGnRnE). A real partitioning policy
 * would split secure / non-secure worlds and apply per-thread
 * sub-regions.
 */
static const ra8_mpu_region_t s_mpu_regions[] = {
  {.base       = k_mpu_region_mram_base,
   .size       = k_mpu_region_mram_size,
   .priv       = k_ra8_mpu_perm_ro,
   .unpriv     = k_ra8_mpu_perm_ro,
   .executable = true,
   .shareable  = k_ra8_mpu_share_non,
   .attr_idx   = k_ra8_mpu_attr_idx_0},
  {.base       = k_mpu_region_sram_base,
   .size       = k_mpu_region_sram_size,
   .priv       = k_ra8_mpu_perm_rw,
   .unpriv     = k_ra8_mpu_perm_rw,
   .executable = false,
   .shareable  = k_ra8_mpu_share_inner,
   .attr_idx   = k_ra8_mpu_attr_idx_0},
  {.base       = k_mpu_region_peri_base,
   .size       = k_mpu_region_peri_size,
   .priv       = k_ra8_mpu_perm_rw,
   .unpriv     = k_ra8_mpu_perm_none,
   .executable = false,
   .shareable  = k_ra8_mpu_share_outer,
   .attr_idx   = k_ra8_mpu_attr_idx_1},
};

typedef enum : uint8_t {
  k_mpu_region_count =
    (uint8_t)(sizeof(s_mpu_regions) / sizeof(s_mpu_regions[0])), /**< MPU region count. */
} mpu_region_count_t;

/**
 * @brief Aggregate MPU configuration handed to ra8_mpu_configure().
 */
static const ra8_mpu_cfg_t s_mpu_cfg = {
  .regions      = s_mpu_regions,
  .region_count = k_mpu_region_count,
  .mair0        = (uint32_t)k_mpu_mair0_word,
  .mair1        = 0U,
  .privdefena   = true,
  .hfnmiena     = false,
};

/* ---------------------------------------------------------------------------
 * ThreadX storage.
 * SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the project's
 * shared weak SysTick_Handler dispatches to ThreadX (via a weak extern
 * to `_tx_timer_interrupt`) so no per-app override is needed.
 * --------------------------------------------------------------------------- */

#ifndef RA8_OFF_TARGET
[[gnu::aligned(8)]] static uint8_t s_thread_stack[k_mpu_thread_stack_bytes];
static TX_THREAD                   s_thread;

/**
 * @var g_threadx_mpu_partition_match
 * @brief HIL liveness counter -- incremented by the worker thread on
 *        every LED-toggle iteration.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving:
 *   1. ra8_mpu_configure(&s_mpu_cfg) returned k_ra8_ok and the three-
 *      region partition table is live without locking the kernel
 *      out of its bookkeeping pages.
 *   2. tx_kernel_enter dispatched the worker thread.
 *   3. The 1 Hz SysTick / _tx_timer_interrupt path is unblocked by
 *      the SRAM and peripheral MPU regions, so tx_thread_sleep wakes
 *      the worker normally.
 *
 * The alive-mode check could only prove the chip didn't HardFault;
 * the counter proves the MPU partition did not silently break the
 * scheduler tick or LED-toggle path.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_mpu_partition_match = 0U;

/**
 * @brief Worker thread: blink LED1 to prove MPU did not wedge us.
 *
 * @details Toggles the board LED, advances the externally probed liveness
 *          counter, and sleeps for the configured ThreadX interval forever.
 *          Continued progress demonstrates that the MPU partition permits the
 *          scheduler and board-control paths required by this thread.
 *
 * @param[in] thread_input ThreadX entry cookie; this demo does not use it.
 *
 * @return None.
 *
 * @pre ThreadX has started the scheduler and dispatched this worker.
 * @pre LED1 and the static worker stack were initialized successfully.
 * @post Each completed iteration toggles LED1 once.
 * @post Each completed iteration advances ::g_threadx_mpu_partition_match.
 *
 * @note This is a permanent, single-instance ThreadX worker.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    g_threadx_mpu_partition_match += 1U;
    (void)tx_thread_sleep((ULONG)k_mpu_blink_ticks);
  }
}

void tx_application_define(void* first_unused_memory)
{
  static CHAR s_thread_name[] = "mpu_blink";

  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread,
                              s_thread_name,
                              internal_thread_entry,
                              0U,
                              s_thread_stack,
                              (ULONG)k_mpu_thread_stack_bytes,
                              (UINT)k_mpu_thread_priority,
                              (UINT)k_mpu_thread_priority,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
}
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Application entry: program MPU, init GPIO, dispatch ThreadX.
 *
 * @pre Reset_Handler completed .data/.bss init.
 *
 * @post MPU is enabled with the s_mpu_cfg partition table.
 * @post Worker thread is running and blinking LED1.
 */
void main(void)
{
  /* CGC bring-up FIRST. tx_initialize_low_level.S programs SysTick
   * with a reload sized for the post-PLL CPUCLK0 = 1 GHz (see the
   * threadx_blink/main.c rationale). Skipping ra8_cgc_init leaves the
   * chip on the MOCO (~8.4 MHz) so the SysTick reload takes ~119 ms
   * wallclock per tick -- the worker's tx_thread_sleep(1000) would
   * then sleep for ~2 minutes and the HIL counter window would see
   * zero advance. Bring up the PLL before tx_kernel_enter so the
   * scheduler tick rate matches what tx_user.h declared. */
  if (ra8_cgc_init() != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (ra8_mpu_configure(&s_mpu_cfg) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}

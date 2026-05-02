/**
 * @file examples/threadx_mpu_partition_demo/main.c
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
 * Region layout (programmed via ``ra_mpu_configure``):
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
 * @since 0.7.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mpu.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables.
 * --------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mpu_thread_stack_bytes = 1024U,
} mpu_stack_t;

typedef enum : uint8_t {
  k_mpu_thread_priority = 4U,
} mpu_priority_t;

typedef enum : uint16_t {
  k_mpu_blink_ticks = 1000U,
} mpu_period_t;

/**
 * @brief Region base addresses for the MPU partition table.
 */
typedef enum : uintptr_t {
  k_mpu_region_mram_base = 0x02000000UL,
  k_mpu_region_sram_base = 0x22000000UL,
  k_mpu_region_peri_base = 0x40000000UL,
} mpu_region_base_t;

/**
 * @brief Region sizes (powers of two, bytes).
 */
typedef enum : uint32_t {
  k_mpu_region_mram_size = 0x00100000UL, /**< 1 MiB. */
  k_mpu_region_sram_size = 0x00100000UL, /**< 1 MiB. */
  k_mpu_region_peri_size = 0x10000000UL, /**< 256 MiB. */
} mpu_region_size_t;

/**
 * @brief Static MPU region table installed at boot.
 *
 * @details
 * Three coarse regions cover code, data, and the peripheral block.
 * MAIR slots default to 0 (device-nGnRnE) -- adequate for a smoke
 * test. A real partitioning policy would split secure / non-secure
 * worlds and apply per-thread sub-regions.
 */
static const ra_mpu_region_t s_mpu_regions[] = {
    {.base       = k_mpu_region_mram_base,
     .size       = k_mpu_region_mram_size,
     .priv       = k_ra_mpu_perm_ro,
     .unpriv     = k_ra_mpu_perm_ro,
     .executable = true,
     .shareable  = k_ra_mpu_share_non,
     .attr_idx   = k_ra_mpu_attr_idx_0},
    {.base       = k_mpu_region_sram_base,
     .size       = k_mpu_region_sram_size,
     .priv       = k_ra_mpu_perm_rw,
     .unpriv     = k_ra_mpu_perm_rw,
     .executable = false,
     .shareable  = k_ra_mpu_share_inner,
     .attr_idx   = k_ra_mpu_attr_idx_0},
    {.base       = k_mpu_region_peri_base,
     .size       = k_mpu_region_peri_size,
     .priv       = k_ra_mpu_perm_rw,
     .unpriv     = k_ra_mpu_perm_none,
     .executable = false,
     .shareable  = k_ra_mpu_share_outer,
     .attr_idx   = k_ra_mpu_attr_idx_0},
};

typedef enum : uint8_t {
  k_mpu_region_count = (uint8_t)(sizeof(s_mpu_regions) / sizeof(s_mpu_regions[0])),
} mpu_region_count_t;

/**
 * @brief Aggregate MPU configuration handed to ra_mpu_configure().
 */
static const ra_mpu_cfg_t s_mpu_cfg = {
    .regions      = s_mpu_regions,
    .region_count = k_mpu_region_count,
    .mair0        = 0U,
    .mair1        = 0U,
    .privdefena   = true,
    .hfnmiena     = false,
};

/* ---------------------------------------------------------------------------
 * SysTick override + ThreadX storage.
 * --------------------------------------------------------------------------- */

#ifndef RA_SIMULATOR_MODE
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

static uint8_t s_thread_stack[k_mpu_thread_stack_bytes] __attribute__((aligned(8)));
static TX_THREAD s_thread;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/**
 * @brief Worker thread: blink LED1 to prove MPU did not wedge us.
 */
static void thread_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra_gpio_toggle(k_ra_pin_led1);
    (void)tx_thread_sleep((ULONG)k_mpu_blink_ticks);
  }
}

/* NOLINTNEXTLINE(misc-use-internal-linkage) */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread,
                              "mpu_blink",
                              thread_entry,
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
#endif /* !RA_SIMULATOR_MODE */

/**
 * @brief Application entry: program MPU, init GPIO, dispatch ThreadX.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler completed .data/.bss init.
 *
 * @post MPU is enabled with the s_mpu_cfg partition table.
 * @post Worker thread is running and blinking LED1.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_mpu_configure(&s_mpu_cfg) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop

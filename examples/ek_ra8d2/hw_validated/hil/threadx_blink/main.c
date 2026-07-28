/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_blink/main.c
 * @brief Eclipse ThreadX bring-up HIL test on the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Two ThreadX threads, each blinking a different LED at a different
 * rate, sharing the Cortex-M85 via cooperative-plus-preemptive
 * scheduling. This is the canonical "is the RTOS alive?" demo and the
 * first piece of code that exercises:
 *
 *   - The project's `port/threadx/src/cortex_m85/tx_initialize_low_level.S`
 *     bring-up (SysTick at 1 ms, system stack pointer save, priority
 *     fix-up).
 *   - SysTick_Handler routed to `_tx_timer_interrupt` (override of the
 *     vector_table.c default).
 *   - The upstream port's `PendSV_Handler` and `SVC_Handler` strong
 *     symbols replacing the weak aliases in `vector_table.c`.
 *   - The HAL's `ra8_gpio_output_init` + `ra8_gpio_toggle` -- the same
 *     paths `examples/blink_hal/main.c` uses, but driven by RTOS
 *     threads instead of a busy `ra8_delay_ms` loop.
 *
 * Like `examples/blink_hal`, this app deliberately skips
 * `ra8_cgc_init()` -- it runs on the boot-default MOCO (~8.4 MHz) so
 * the SysTick reload programmed in `tx_initialize_low_level.S` stays
 * accurate.
 *
 * @par Threads
 *
 * | Name        | Priority | Period            | LED       |
 * |:------------|:---------|:------------------|:----------|
 * | `blink_a`   | 4        | 1000 ms (1 Hz)    | LED1 P6_00 |
 * | `blink_b`   | 4        | 2000 ms (0.5 Hz)  | LED2 P3_03 |
 *
 * Both threads run at the same priority; ThreadX round-robins them on
 * each `tx_thread_sleep` wake.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"

/*
 * The host unit-test build (RA8_OFF_TARGET) does not link the
 * ThreadX vendor tree, so `tx_api.h` is unreachable when clang-tidy
 * walks this file. Pull it in only on the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Timer-thread tunables (typed enums per the project's no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack size, in bytes, for each user blink thread.
 *
 * @details
 * 1 KiB is comfortably above `TX_MINIMUM_STACK` (set to 512 in
 * `port/threadx/inc/tx_user.h`) and accommodates the FPU + Helium register
 * save area on a context switch. Static allocation keeps the demo
 * compatible with NASA Power of 10 Rule 3 (no dynamic memory).
 */
typedef enum : uint16_t {
  k_blink_thread_stack_bytes = 1024U, /**< Blink thread stack bytes. */
} blink_stack_t;

/**
 * @brief Thread priority for both blink threads.
 */
typedef enum : uint8_t {
  k_blink_thread_priority = 4U, /**< Blink thread priority. */
} blink_priority_t;

/**
 * @brief Sleep durations expressed in ThreadX ticks.
 *
 * @details
 * `port/threadx/inc/tx_user.h` pins the tick rate to 1000 Hz, so a tick is
 * 1 ms. `k_blink_a_ticks` therefore lights LED1 at 1 Hz and
 * `k_blink_b_ticks` lights LED2 at 0.5 Hz.
 */
typedef enum : uint16_t {
  k_blink_a_ticks = 500U,  /**< Blink a ticks. */
  k_blink_b_ticks = 1000U, /**< Blink b ticks. */
} blink_period_t;

#ifndef RA8_OFF_TARGET
/* ---------------------------------------------------------------------------
 * Static thread + stack storage. Only meaningful on the cross build,
 * where TX_THREAD is defined by the ThreadX vendor headers.
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack for thread A (LED1 blinker).
 * @note 32-bit aligned per ARMv8-M AAPCS.
 */
[[gnu::aligned(8)]] static uint8_t s_thread_a_stack[k_blink_thread_stack_bytes];

/**
 * @brief Stack for thread B (LED2 blinker).
 */
[[gnu::aligned(8)]] static uint8_t s_thread_b_stack[k_blink_thread_stack_bytes];

/**
 * @brief Thread A control block. ThreadX zeroes it on tx_thread_create.
 */
static TX_THREAD s_thread_a;

/**
 * @brief Thread B control block.
 */
static TX_THREAD s_thread_b;

/**
 * @var g_threadx_blink_tick
 * @brief HIL liveness counter -- incremented by thread A on each
 *        LED-A toggle. Read externally by hil_jlink_memprobe.sh.
 *
 * @details
 * If ThreadX schedules thread A correctly, the counter advances at
 * 1/k_blink_a_ticks Hz. The memprobe HIL mode asserts the counter
 * has advanced over a sample window -- catches "scheduler wedged"
 * and "thread crashed before first iteration" failure modes that
 * the plain HIL_MODE=alive check misses.
 *
 * `volatile` keeps the increment alive under optimization; the
 * global (non-static) keeps the symbol linker-visible without
 * --gc-sections culling.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_blink_tick = 0U;

/* SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the project's
 * shared weak SysTick_Handler dispatches to ThreadX (via a weak extern
 * to `_tx_timer_interrupt`) so no per-app override is needed. Closes
 * Issue #8. */

/* ---------------------------------------------------------------------------
 * Thread bodies.
 * --------------------------------------------------------------------------- */

/**
 * @brief Thread A entry point: toggle LED1 every `k_blink_a_ticks` ms.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre `ra8_gpio_output_init` has succeeded for `k_ra8_pin_led1`.
 * @pre ThreadX scheduler is running (`tx_kernel_enter` has dispatched
 *      a thread).
 *
 * @post LED1 toggles on each loop iteration.
 * @post The thread blocks at `tx_thread_sleep`, yielding the CPU.
 */
static void thread_a_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    g_threadx_blink_tick += 1U;
    (void)tx_thread_sleep((ULONG)k_blink_a_ticks);
  }
}

/**
 * @brief Thread B entry point: toggle LED2 every `k_blink_b_ticks` ms.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre `ra8_gpio_output_init` has succeeded for `k_ra8_pin_led2`.
 * @pre ThreadX scheduler is running.
 *
 * @post LED2 toggles on each loop iteration.
 * @post Thread sleeps via tx_thread_sleep so the scheduler can pick A.
 */
static void thread_b_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led2);
    (void)tx_thread_sleep((ULONG)k_blink_b_ticks);
  }
}

/* ---------------------------------------------------------------------------
 * tx_application_define -- ThreadX calls this once, just before the first
 * scheduling decision, with the address of the first byte of unused RAM.
 * Threads are created here, NOT in main(), because the kernel needs them
 * registered before tx_kernel_enter dispatches.
 * --------------------------------------------------------------------------- */

/**
 * @brief ThreadX application initialization callback.
 *
 * @param[in] first_unused_memory Pointer to the first byte of free RAM
 *            after the kernel's internal allocations. Unused here --
 *            both threads use static stacks declared at file scope.
 *
 * @pre Called from `_tx_initialize_kernel_enter`, before the scheduler
 *      starts. IRQs are masked at this point.
 * @pre `ra8_gpio_output_init` has already configured LED1 + LED2 (done
 *      from `main()` before `tx_kernel_enter()`).
 *
 * @post Thread A is created at priority `k_blink_thread_priority` and
 *       set to auto-start.
 * @post Thread B is created at the same priority.
 *
 * @note `tx_thread_create` returns `TX_SUCCESS` on a clean register --
 *       on failure we busy-loop here because there is no log channel
 *       up yet and the kernel is about to take over.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_thread_a,
                              "blink_a",
                              thread_a_entry,
                              0U,
                              s_thread_a_stack,
                              (ULONG)k_blink_thread_stack_bytes,
                              (UINT)k_blink_thread_priority,
                              (UINT)k_blink_thread_priority,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  err = tx_thread_create(&s_thread_b,
                         "blink_b",
                         thread_b_entry,
                         0U,
                         s_thread_b_stack,
                         (ULONG)k_blink_thread_stack_bytes,
                         (UINT)k_blink_thread_priority,
                         (UINT)k_blink_thread_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  if (err != TX_SUCCESS) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
}
#endif /* !RA8_OFF_TARGET */

/* ---------------------------------------------------------------------------
 * main() -- driver init, then drop into the ThreadX scheduler.
 * --------------------------------------------------------------------------- */

/**
 * @brief Application entry. Configures GPIO, then launches ThreadX.
 *
 * @return Never returns -- `tx_kernel_enter` does not.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post The two LED threads are running and SysTick is generating
 *       1 ms ticks for the scheduler.
 * @post On any HAL init failure the function halts in `__WFI`.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  /* CGC bring-up FIRST. tx_initialize_low_level.S programs SysTick
   * with a reload of (1 GHz / 1000 - 1) = 999999 cycles per tick
   * (chip HUM Ch 11 CGC, plus our port/threadx/src/cortex_m85/
   * tx_initialize_low_level.S RA8_BOOT_CLOCK_HZ value). If the chip
   * is still on MOCO (~8.4 MHz) at scheduler-enter time, that
   * 999999-cycle reload takes ~119 ms wallclock instead of 1 ms --
   * a 119x slowdown, which makes tx_thread_sleep(500 ticks) sleep
   * for ~60 s and the LED toggle look frozen in any reasonable
   * HIL window. Bring up the PLL before tx_kernel_enter so the
   * scheduler tick rate matches what tx_user.h declared. */
  if (ra8_cgc_init() != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  /* GPIO init runs with PRIMASK set; ThreadX will re-enable IRQs once
   * its scheduler is up. */
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  /* Drop into ThreadX. Returns only on internal scheduler error.
   * The host build has no kernel, so it just falls into the WFI loop. */
#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  /* Belt + suspenders. */
  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop

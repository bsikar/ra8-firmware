/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_systick_retune/main.c
 * @brief Eclipse ThreadX SysTick retune-to-live-CPUCLK0 demo (issue #287)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Proves that ::ra8_threadx_systick_retune reprograms SysTick.LOAD from
 * the *live* CPUCLK0 rate so the ThreadX kernel tick is accurate no
 * matter which clock the app raised. Unlike `threadx_blink` -- which
 * relies on `ra8_cgc_init()` bringing CPUCLK0 to exactly the 1 GHz the
 * `tx_initialize_low_level.S` reload assumes -- this app additionally
 * calls the retune hook from `tx_application_define` and self-checks the
 * result:
 *
 *   1. `main()` raises CPUCLK0 to the PLL1 target with `ra8_cgc_init()`.
 *   2. `tx_application_define` (which ThreadX runs after
 *      `_tx_initialize_low_level` but before scheduling) calls
 *      ::ra8_threadx_systick_retune.
 *   3. It then independently recomputes the expected reload with
 *      ::ra8_threadx_systick_reload_for from the live CPUCLK0 and reads
 *      SYST_RVR back. Any mismatch (or a retune error) bumps
 *      ::g_threadx_retune_bad; the computed reload is published in
 *      ::g_threadx_retune_reload for observability.
 *   4. One worker thread toggles LED1 and advances
 *      ::g_threadx_retune_tick every wake -- the scheduler-liveness
 *      counter the HIL / ra8_emulator probe samples.
 *
 * @par HIL / EIL contract
 * `hil.conf` uses `HIL_MODE=jlink_memprobe`:
 *   - primary  ::g_threadx_retune_tick   must advance >= 3 (scheduler alive),
 *   - failure  ::g_threadx_retune_bad     must stay 0 (retune matched the
 *     live clock).
 * ra8_emulator runs the identical ARM retune path -- the SYST_RVR write lands
 * in the emulated System Control Space and reads back -- so EIL == HIL.
 *
 * @par Threads
 *
 * | Name       | Priority | Period          | Action                     |
 * |:-----------|:---------|:----------------|:---------------------------|
 * | `retune`   | 4        | 250 ms loop     | Toggle LED1, bump tick     |
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

/*
 * The host unit-test build (RA8_OFF_TARGET) does not link the
 * ThreadX vendor tree, so `tx_api.h` (and the ThreadX-port header
 * `ra8_threadx.h`) are pulled in only on the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "ra8_threadx.h"
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Tunables (typed enums per the project's no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack size, in bytes, for the worker thread.
 */
typedef enum : uint16_t {
  k_retune_thread_stack_bytes = 1024U, /**< Retune thread stack bytes. */
} retune_stack_t;

/**
 * @brief Thread priority for the worker thread.
 */
typedef enum : uint8_t {
  k_retune_thread_priority = 4U, /**< Retune thread priority. */
} retune_priority_t;

/**
 * @brief Worker sleep period in ThreadX ticks (tx_user.h pins a tick to 1 ms).
 */
typedef enum : uint16_t {
  k_retune_loop_ticks = 250U, /**< Retune loop ticks. */
} retune_period_t;

/**
 * @brief SysTick reload register address (Cortex-M System Control Space).
 *
 * @details
 * SYST_RVR is architectural (Arm), not a Renesas peripheral, so it needs
 * no HUM citation. Read back here to confirm what the retune actually
 * programmed.
 */
typedef enum : uintptr_t {
  k_retune_syst_rvr_addr = 0xE000E014UL, /**< Retune syst rvr address. */
} retune_syst_addr_t;

/* ---------------------------------------------------------------------------
 * HIL / EIL observables. Global (non-static) + volatile so the symbols stay
 * linker-visible for J-Link / ra8_emulator --dump-sym and survive optimization.
 * --------------------------------------------------------------------------- */

/**
 * @var g_threadx_retune_tick
 * @brief Scheduler-liveness counter -- bumped by the worker on each wake.
 * @note Read externally by the memprobe; firmware never reads it back.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_retune_tick = 0U;

/**
 * @var g_threadx_retune_reload
 * @brief SysTick reload the retune computed for the live CPUCLK0.
 * @details Published for observability (999999 at CPUCLK0 = 1 GHz).
 * @note Written once from `tx_application_define`; read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_retune_reload = 0U;

/**
 * @var g_threadx_retune_bad
 * @brief Non-zero iff the retune failed or SYST_RVR did not match the
 *        independently-computed reload for the live clock.
 * @note The memprobe asserts this stays 0. Written once at init.
 * @warning A non-zero value means the kernel tick would drift.
 * @since 0.1.0
 */
volatile uint32_t g_threadx_retune_bad = 0U;

#ifndef RA8_OFF_TARGET
/* ---------------------------------------------------------------------------
 * Static thread + stack storage (cross build only -- TX_THREAD is a vendor
 * type unavailable on the host clang-tidy pass).
 * --------------------------------------------------------------------------- */

/**
 * @brief Stack for the worker thread (32-bit aligned per ARMv8-M AAPCS).
 */
[[gnu::aligned(8)]] static uint8_t s_retune_stack[k_retune_thread_stack_bytes];

/**
 * @brief Worker thread control block. ThreadX zeroes it on tx_thread_create.
 */
static TX_THREAD s_retune_thread;

/* SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the shared weak
 * SysTick_Handler dispatches to ThreadX via a weak extern to
 * `_tx_timer_interrupt`, so no per-app override is needed (issue #8). */

/**
 * @brief Read back the SysTick reload register (SYST_RVR).
 *
 * @details Performs one volatile read from the architected Cortex-M System
 *          Control Space address used to verify the ThreadX clock retune.
 *
 * @return Current SYST_RVR value (the reload the kernel tick uses).
 * @retval 0..k_ra8_systick_reload_max Whatever was last programmed.
 *
 * @pre The System Control Space is mapped (always true on Cortex-M).
 * @pre `_tx_initialize_low_level` / the retune has run.
 * @post No state modified (pure read).
 * @post Returned value reflects the live SYST_RVR.
 *
 * @note Not called on the host build; SCS is unmapped there.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_retune_read_syst_rvr(void)
{
  volatile const uint32_t* rvr = (volatile const uint32_t*)k_retune_syst_rvr_addr;
  return *rvr;
}

/* ---------------------------------------------------------------------------
 * Thread body.
 * --------------------------------------------------------------------------- */

/**
 * @brief Worker entry: toggle LED1 and advance the liveness counter.
 *
 * @details Repeats the visible LED toggle and externally observed counter
 *          increment at the configured ThreadX sleep cadence.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @return None.
 *
 * @pre The ThreadX scheduler is running.
 * @pre LED1 has been configured by `main()`.
 * @post LED1 toggles once per loop; ::g_threadx_retune_tick advances.
 * @post The thread yields the CPU via `tx_thread_sleep`.
 *
 * @note Permanent worker used only by this retune validation image.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_retune_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    g_threadx_retune_tick += 1U;
    (void)tx_thread_sleep((ULONG)k_retune_loop_ticks);
  }
}

/* ---------------------------------------------------------------------------
 * tx_application_define -- ThreadX calls this once, after
 * _tx_initialize_low_level but before the first scheduling decision.
 * --------------------------------------------------------------------------- */

/**
 * @brief ThreadX application-define callback: retune SysTick, then spawn.
 *
 * @param[in] first_unused_memory Unused -- the demo uses a static stack.
 *
 * @pre Called from `_tx_initialize_kernel_enter` with IRQs masked.
 * @pre `ra8_cgc_init()` has published the live CPUCLK0 rate.
 * @post SysTick is retuned to the live clock (or ::g_threadx_retune_bad set).
 * @post The worker thread is created and auto-started.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* Reprogram SysTick.LOAD from the live CPUCLK0 (issue #287). */
  const ra8_err_t retune_err = ra8_threadx_systick_retune();

  /* Independently recompute the expected reload from the live clock and
   * compare against what retune actually left in SYST_RVR. Any deviation
   * means the kernel tick would drift, so flag it for the probe. */
  uint32_t        cpuclk_hz = 0U;
  uint32_t        expected  = 0U;
  const ra8_err_t clk_err   = ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk_hz);
  const ra8_err_t rel_err =
    ra8_threadx_systick_reload_for(cpuclk_hz, (uint32_t)k_ra8_threadx_tick_hz, &expected);
  const uint32_t live_rvr = internal_retune_read_syst_rvr();

  g_threadx_retune_reload = expected;
  if ((retune_err != k_ra8_ok) || (clk_err != k_ra8_ok) || (rel_err != k_ra8_ok) ||
      (live_rvr != expected)) {
    g_threadx_retune_bad = 1U;
  }

  const UINT err = tx_thread_create(&s_retune_thread,
                                    "retune",
                                    internal_retune_thread_entry,
                                    0U,
                                    s_retune_stack,
                                    (ULONG)k_retune_thread_stack_bytes,
                                    (UINT)k_retune_thread_priority,
                                    (UINT)k_retune_thread_priority,
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
 * main() -- raise the clock, bring up the LED, then drop into ThreadX.
 * --------------------------------------------------------------------------- */

/**
 * @brief Application entry. Raises CPUCLK0, configures LED1, launches ThreadX.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post CPUCLK0 is at the PLL1 target before the kernel starts.
 * @post On any HAL init failure the function halts in `__WFI`.
 *
 * @since 0.1.0
 */
void main(void)
{
  /* Raise CPUCLK0 to the PLL1 target so the retune has a real
   * high-frequency clock to program SysTick against. */
  if (ra8_cgc_init() != k_ra8_ok) {
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
  ra8_isr_globals_enable();
  tx_kernel_enter();
#endif

  while (1) {
    __asm__ volatile("wfi");
  }
}

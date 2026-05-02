/**
 * @file ra_time.c
 * @brief SysTick tick counter implementation
 *
 * @details
 * SysTick control registers are architectural (part of the Cortex-M
 * System Control Block). Addresses:
 *
 *   SYST_CSR   @ 0xE000E010  -- Control and status
 *   SYST_RVR   @ 0xE000E014  -- Reload value
 *   SYST_CVR   @ 0xE000E018  -- Current value
 *   SYST_CALIB @ 0xE000E01C  -- Calibration
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_time.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_log.h"
#include "ra_time_constants.h"

static const char* s_tag = "TIME";

static volatile uint32_t s_tick_ms = 0U;

typedef enum : uintptr_t {
  k_ra_systick_csr_addr = 0xE000E010UL,
  k_ra_systick_rvr_addr = 0xE000E014UL,
  k_ra_systick_cvr_addr = 0xE000E018UL,
} ra_systick_addr_t;

typedef enum : uint32_t {
  k_ra_systick_csr_enable    = 0x00000001UL,
  k_ra_systick_csr_tickint   = 0x00000002UL,
  k_ra_systick_csr_clksource = 0x00000004UL, /**< 1 = CPU clock, 0 = ext ref. */
} ra_systick_csr_bit_t;

/**
 * @brief Get the SysTick Control & Status Register pointer.
 *
 * @details Trivial address-cast helper for SYST_CSR.
 *
 * @return Volatile pointer to SYST_CSR.
 * @retval (volatile uint32_t*)k_ra_systick_csr_addr
 *
 * @pre None.
 * @pre SCS region is mapped (always true on Cortex-M).
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_csr(void)
{
  return (volatile uint32_t*)k_ra_systick_csr_addr;
}

/**
 * @brief Get the SysTick Reload Value Register pointer.
 *
 * @details Trivial address-cast helper for SYST_RVR.
 *
 * @return Volatile pointer to SYST_RVR.
 * @retval (volatile uint32_t*)k_ra_systick_rvr_addr
 *
 * @pre None.
 * @pre SCS region is mapped.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_rvr(void)
{
  return (volatile uint32_t*)k_ra_systick_rvr_addr;
}

/**
 * @brief Get the SysTick Current Value Register pointer.
 *
 * @details Trivial address-cast helper for SYST_CVR.
 *
 * @return Volatile pointer to SYST_CVR.
 * @retval (volatile uint32_t*)k_ra_systick_cvr_addr
 *
 * @pre None.
 * @pre SCS region is mapped.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_cvr(void)
{
  return (volatile uint32_t*)k_ra_systick_cvr_addr;
}

/**
 * @brief Implementation of `ra_time_init()` -- programme SysTick.
 *
 * @details Computes the reload as `cpu_hz / 1000 - 1`, programmes
 *          SYST_RVR/CVR/CSR. On the simulator host the SCS writes are
 *          skipped.
 *
 * @param[in] cpu_hz Current CPU clock in Hz.
 *
 * @return Error code.
 * @retval k_ra_ok                SysTick programmed; tick interrupt active.
 * @retval k_ra_err_invalid_arg   `cpu_hz` is zero or yields a zero reload.
 *
 * @pre `ra_cgc_init()` has run -- CPU clock is stable.
 * @pre Function is called from a single-threaded context.
 * @post On success, SysTick fires every 1 ms.
 * @post `s_tick_ms` is reset to zero.
 *
 * @note Not thread-safe; intended for one-shot init only.
 *
 * @since 0.1.0
 */
ra_err_t ra_time_init(uint32_t cpu_hz)
{
  if (cpu_hz == 0U) {
    ra_log_error(s_tag, "cpu_hz must be non-zero");
    return k_ra_err_invalid_arg;
  }

  const uint32_t reload = (cpu_hz / k_ra_ms_per_sec) - 1U;
  if (reload == 0U) {
    ra_log_error(s_tag, "cpu_hz too low for 1kHz tick");
    return k_ra_err_invalid_arg;
  }

#ifndef RA_SIMULATOR_MODE
  /* Disable, programme reload, clear current count, re-enable with
   * IRQ and CPU clock source. The SysTick registers live in the
   * Cortex-M System Control Space and are not mapped on the host
   * test build. */
  *internal_csr() = 0U;
  *internal_rvr() = reload;
  *internal_cvr() = 0U;
  *internal_csr() = k_ra_systick_csr_clksource | k_ra_systick_csr_tickint | k_ra_systick_csr_enable;
#endif

  s_tick_ms = 0U;
  ra_log_info_val(s_tag, "systick reload", reload);
  return k_ra_ok;
}

/**
 * @brief Implementation of `ra_time_ms()` -- read SysTick counter.
 *
 * @details Returns the SysTick-incremented `s_tick_ms` counter.
 *
 * @return Milliseconds since `ra_time_init()`, modulo 2^32.
 * @retval 0..UINT32_MAX  Current tick count.
 *
 * @pre `ra_time_init()` has been called.
 * @pre Reader is OK with single-word atomicity.
 * @post No state modified.
 * @post Successive calls are non-decreasing modulo 2^32.
 *
 * @note Thread-safe (atomic single-word read on Cortex-M).
 *
 * @since 0.1.0
 */
uint32_t ra_time_ms(void)
{
  return s_tick_ms;
}

/**
 * @brief Implementation of `ra_delay_ms()` -- busy-wait with `wfi`.
 *
 * @details Loops on `s_tick_ms` and issues `wfi` between checks.
 *
 * @param[in] ms Milliseconds to wait. Zero returns immediately.
 *
 * @return None.
 * @retval None
 *
 * @pre `ra_time_init()` has been called.
 * @pre IRQs are NOT globally masked.
 * @post At least `ms` milliseconds have elapsed.
 * @post No internal state modified.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
void ra_delay_ms(uint32_t ms)
{
  const uint32_t start = s_tick_ms;
  while ((uint32_t)(s_tick_ms - start) < ms) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("wfi");
#endif
  }
}

/**
 * @brief Implementation of `ra_time_on_tick()` -- SysTick IRQ tick.
 *
 * @details Increments `s_tick_ms`. Invoked from SysTick IRQ.
 *
 * @return None.
 * @retval None
 *
 * @pre Invoked from SysTick IRQ context (or test equivalent).
 * @pre `ra_time_init()` has set up the SysTick reload.
 * @post `s_tick_ms` is incremented by exactly one.
 * @post No other state is modified.
 *
 * @note IRQ-safe; SysTick cannot pre-empt itself.
 *
 * @since 0.1.0
 */
void ra_time_on_tick(void)
{
  s_tick_ms++;
}

/*
 * Default SysTick_Handler -- weak so that an app linking an RTOS (e.g.
 * Eclipse ThreadX with `_tx_timer_interrupt`) can supply a non-weak
 * override and steer the tick into its own scheduler. When no override
 * is provided the linker keeps this body and the ms counter advances
 * exactly as the bare-metal apps expect.
 *
 * The vector_table.c weak alias to Default_Handler is overridden by
 * this stronger weak symbol; an even stronger non-weak one in the
 * application file wins above both.
 */
void SysTick_Handler(void);

/* NOLINTNEXTLINE(misc-use-internal-linkage) -- linker symbol for vector table. */
__attribute__((weak)) void SysTick_Handler(void)
{
  ra_time_on_tick();
}

/**
 * @file ra8_time.c
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

#include "ra8_time.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_time_constants.h"

static const char* s_tag = "TIME";

static volatile uint32_t s_tick_ms = 0U;

typedef enum : uintptr_t {
  k_ra8_systick_csr_addr = 0xE000E010UL, /**< RA8 systick csr address. */
  k_ra8_systick_rvr_addr = 0xE000E014UL, /**< RA8 systick rvr address. */
  k_ra8_systick_cvr_addr = 0xE000E018UL, /**< RA8 systick cvr address. */
  /* DWT (Data Watchpoint and Trace) gives a free-running cycle counter
   * that ticks every CPU cycle regardless of PRIMASK -- needed by
   * ra8_delay_ms when SysTick IRQ can't dispatch (early boot, IRQ-off
   * critical sections). Cortex-M85 documents these addresses. */
  k_ra8_dwt_demcr_addr  = 0xE000EDFCUL, /**< DEMCR: bit 24 TRCENA.            */
  k_ra8_dwt_ctrl_addr   = 0xE0001000UL, /**< DWT_CTRL: bit 0 CYCCNTENA.       */
  k_ra8_dwt_cyccnt_addr = 0xE0001004UL, /**< DWT_CYCCNT free-running counter. */
} ra8_systick_addr_t;

typedef enum : uint32_t {
  k_ra8_systick_csr_enable    = 0x00000001UL, /**< RA8 systick csr enable.     */
  k_ra8_systick_csr_tickint   = 0x00000002UL, /**< RA8 systick csr tickint.    */
  k_ra8_systick_csr_clksource = 0x00000004UL, /**< 1 = CPU clock, 0 = ext ref. */
  k_ra8_dwt_demcr_trcena      = 0x01000000UL, /**< DEMCR.TRCENA (bit 24).      */
  k_ra8_dwt_ctrl_cyccntena    = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA (bit 0). */
} ra8_systick_csr_bit_t;

/** @brief CPU cycles per millisecond, stamped by ra8_time_init. */
static volatile uint32_t s_cycles_per_ms = 0U;

/**
 * @brief Get the SysTick Control & Status Register pointer.
 *
 * @details Trivial address-cast helper for SYST_CSR.
 *
 * @return Volatile pointer to SYST_CSR.
 * @retval (volatile uint32_t*)k_ra8_systick_csr_addr
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
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_csr(void)
{
  return (volatile uint32_t*)k_ra8_systick_csr_addr;
}

/**
 * @brief Get the SysTick Reload Value Register pointer.
 *
 * @details Trivial address-cast helper for SYST_RVR.
 *
 * @return Volatile pointer to SYST_RVR.
 * @retval (volatile uint32_t*)k_ra8_systick_rvr_addr
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
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_rvr(void)
{
  return (volatile uint32_t*)k_ra8_systick_rvr_addr;
}

/**
 * @brief Get the SysTick Current Value Register pointer.
 *
 * @details Trivial address-cast helper for SYST_CVR.
 *
 * @return Volatile pointer to SYST_CVR.
 * @retval (volatile uint32_t*)k_ra8_systick_cvr_addr
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
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_cvr(void)
{
  return (volatile uint32_t*)k_ra8_systick_cvr_addr;
}

/**
 * @brief Implementation of `ra8_time_init()` -- programme SysTick.
 *
 * @details Computes the reload as `cpu_hz / 1000 - 1`, programmes
 *          SYST_RVR/CVR/CSR. On the off-target host the SCS writes are
 *          skipped.
 *
 * @param[in] cpu_hz Current CPU clock in Hz.
 *
 * @return Error code.
 * @retval k_ra8_ok                SysTick programmed; tick interrupt active.
 * @retval k_ra8_err_invalid_arg   `cpu_hz` is zero or yields a zero reload.
 *
 * @pre `ra8_cgc_init()` has run -- CPU clock is stable.
 * @pre Function is called from a single-threaded context.
 * @post On success, SysTick fires every 1 ms.
 * @post `s_tick_ms` is reset to zero.
 *
 * @note Not thread-safe; intended for one-shot init only.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_time_init(uint32_t cpu_hz)
{
  if (cpu_hz == 0U) {
    ra8_log_error(s_tag, "cpu_hz must be non-zero");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t reload = (cpu_hz / k_ra8_ms_per_sec) - 1U;
  if (reload == 0U) {
    ra8_log_error(s_tag, "cpu_hz too low for 1kHz tick");
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  /* Disable, programme reload, clear current count, re-enable with
   * IRQ and CPU clock source. The SysTick registers live in the
   * Cortex-M System Control Space and are not mapped on the host
   * test build. */
  *internal_csr() = 0U;
  *internal_rvr() = reload;
  *internal_cvr() = 0U;
  *internal_csr() =
    k_ra8_systick_csr_clksource | k_ra8_systick_csr_tickint | k_ra8_systick_csr_enable;

  /* Enable DWT cycle counter as a PRIMASK-immune fallback for
   * ra8_delay_ms. DEMCR.TRCENA unlocks DWT; DWT_CTRL.CYCCNTENA starts
   * the free-running counter. */
  volatile uint32_t* demcr   = (volatile uint32_t*)k_ra8_dwt_demcr_addr;
  volatile uint32_t* dwtctrl = (volatile uint32_t*)k_ra8_dwt_ctrl_addr;
  *demcr |= (uint32_t)k_ra8_dwt_demcr_trcena;
  *dwtctrl |= (uint32_t)k_ra8_dwt_ctrl_cyccntena;
#endif

  s_cycles_per_ms = cpu_hz / k_ra8_ms_per_sec;
  s_tick_ms       = 0U;
  ra8_log_info_val(s_tag, "systick reload", reload);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_time_ms()` -- read SysTick counter.
 *
 * @details Returns the SysTick-incremented `s_tick_ms` counter.
 *
 * @return Milliseconds since `ra8_time_init()`, modulo 2^32.
 * @retval 0..UINT32_MAX  Current tick count.
 *
 * @pre `ra8_time_init()` has been called.
 * @pre Reader is OK with single-word atomicity.
 * @post No state modified.
 * @post Successive calls are non-decreasing modulo 2^32.
 *
 * @note Thread-safe (atomic single-word read on Cortex-M).
 *
 * @since 0.1.0
 */
uint32_t ra8_time_ms(void)
{
  return s_tick_ms;
}

/**
 * @brief Implementation of `ra8_delay_ms()` -- busy-wait with `wfi`.
 *
 * @details Loops on `s_tick_ms` and issues `wfi` between checks.
 *
 * @param[in] ms Milliseconds to wait. Zero returns immediately.
 *
 * @pre `ra8_time_init()` has been called.
 * @pre IRQs are NOT globally masked.
 * @post At least `ms` milliseconds have elapsed.
 * @post No internal state modified.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
void ra8_delay_ms(uint32_t ms)
{
#ifdef RA8_OFF_TARGET
  (void)ms; /* No SysTick off-target -- s_tick_ms never advances. */
#else
  /* If PRIMASK is set the SysTick IRQ cannot dispatch and s_tick_ms */
  /* never advances -- a wfi-loop on s_tick_ms would hang forever. */
  /* Fall back to DWT_CYCCNT, which ticks every CPU cycle regardless */
  /* of PRIMASK. Once IRQs are globally enabled, the cheaper */
  /* SysTick path is used. */
  uint32_t primask;
  __asm__ volatile("mrs %0, primask" : "=r"(primask));
  if ((primask & 1U) != 0U) {
    volatile const uint32_t* cyccnt = (volatile const uint32_t*)k_ra8_dwt_cyccnt_addr;
    const uint32_t           target = ms * s_cycles_per_ms;
    const uint32_t           start  = *cyccnt;
    while ((uint32_t)(*cyccnt - start) < target) {
      __asm__ volatile("nop");
    }
  } else {
    const uint32_t start = s_tick_ms;
    while ((uint32_t)(s_tick_ms - start) < ms) {
      __asm__ volatile("wfi");
    }
  }
#endif
}

/**
 * @brief Implementation of `ra8_time_on_tick()` -- SysTick IRQ tick.
 *
 * @details Increments `s_tick_ms`. Invoked from SysTick IRQ.
 *
 * @pre Invoked from SysTick IRQ context (or test equivalent).
 * @pre `ra8_time_init()` has set up the SysTick reload.
 * @post `s_tick_ms` is incremented by exactly one.
 * @post No other state is modified.
 *
 * @note IRQ-safe; SysTick cannot pre-empt itself.
 *
 * @since 0.1.0
 */
void ra8_time_on_tick(void)
{
  s_tick_ms++;
}

/*
 * Per-subsystem tick callouts. These are declared as WEAK EXTERNS: if
 * the firmware image links a subsystem that defines the symbol, the
 * weak reference resolves to that strong defn and the function pointer
 * is non-null; if the subsystem isn't linked the reference remains a
 * null pointer and the call is skipped.
 *
 *   - `_tx_timer_interrupt`         lives in libthreadx.a. Advances
 *                                   the ThreadX time bases so
 *                                   tx_thread_sleep / TX_TIMER /
 *                                   semaphore-wait timeouts fire.
 *                                   GUARDED by `g_ra8_threadx_systick_ready`
 *                                   (also in libthreadx.a) -- the kernel
 *                                   timer state is not safe to call
 *                                   into until the project's
 *                                   `_tx_initialize_low_level` has run.
 *   - `ux_dcd_ra8_usb_irq_reenable`  lives in port/usbx/src/ux_dcd_ra8_usb.c.
 *                                   Re-arms the USBFS NVIC line that
 *                                   the bridge's storm guard masks.
 *
 * Apps that need additional tick work can still publish a strong
 * SysTick_Handler in their own main.c -- it wins over the weak default
 * below. The weak-extern trick is what lets a per-app handler go away
 * once its work is just "tick threadx and tick usb".
 */
/* The default SysTick_Handler below and its weak externs are firmware-only.
 * The host unit-test build (RA8_OFF_TARGET) has no SysTick ISR, never calls
 * this handler, and must not drag in the ThreadX / USB weak externs: the macOS
 * test linker (ld64) does NOT resolve an undefined `weak` reference to NULL the
 * way the ELF firmware linker does, so leaving them visible breaks every host
 * test that links ra8_time.c. Compile the whole ISR + its externs for the
 * firmware target only; on ELF the `weak` references still fold to NULL when a
 * subsystem is absent and to its strong defn when present. */
#ifndef RA8_OFF_TARGET

/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) -- vendor (ThreadX) symbol; we must use Eclipse ThreadX's actual entry-point name. */
[[gnu::weak]] extern void _tx_timer_interrupt(void);
[[gnu::weak]] extern void ux_dcd_ra8_usb_irq_reenable(void);

/* When a non-ThreadX app is being built nothing pulls in the storage
 * for this flag and the weak reference resolves to a null pointer at
 * link time; the runtime check below short-circuits the conditional.
 * When ThreadX IS linked the pointer is non-null and we read the
 * underlying byte every SysTick. */
[[gnu::weak]] extern volatile uint32_t g_ra8_threadx_systick_ready;

/*
 * Default SysTick_Handler -- weak so an app can still supply a strong
 * override when needed. The default already routes ticks into ThreadX
 * and USB via the weak externs above, so most apps no longer need a
 * per-app SysTick_Handler at all.
 *
 * The vector_table.c weak alias to Default_Handler is overridden by
 * this stronger weak symbol; an even stronger non-weak one in the
 * application file wins above both.
 */
void SysTick_Handler(void);

/* NOLINTNEXTLINE(misc-use-internal-linkage) -- linker symbol for vector table. */
[[gnu::weak]] void SysTick_Handler(void)
{
  ra8_time_on_tick();
  /* Only dispatch into the ThreadX timer ISR after the project's
   * `_tx_initialize_low_level` has flagged the kernel as ready.
   * Between `ra8_time_init()` (which arms SysTick at 1 kHz from C)
   * and `tx_kernel_enter()` (which runs ThreadX init), the kernel
   * timer state is uninitialised; an early call into
   * `_tx_timer_interrupt` here HardFaults with UFSR.INVPC=1 -- the
   * bench symptom that bricks `threadx_netx_tcp_echo` boot when its
   * intervening `ra8_board_ethernet_init` runs long enough for
   * SysTick to fire. Non-ThreadX apps see both externs as NULL
   * weak references and the nested-if folds away. Written as
   * nested ifs (no compound boolean operator) so the project's
   * MC/DC gate does not demand an extra test vector for this
   * ISR-only code path. */
  if (&g_ra8_threadx_systick_ready != ((void*)0)) {
    if (g_ra8_threadx_systick_ready != 0U) {
      if (_tx_timer_interrupt != ((void*)0)) {
        _tx_timer_interrupt();
      }
    }
  }
  if (ux_dcd_ra8_usb_irq_reenable != ((void*)0)) {
    ux_dcd_ra8_usb_irq_reenable();
  }
}

#else /* RA8_OFF_TARGET */

/* Host unit-test build: test_ra8_time exercises SysTick_Handler directly, so the
 * symbol must exist -- but there is no ThreadX kernel or USB bridge to tick,
 * and the macOS test linker (ld64) cannot resolve the firmware handler's
 * undefined weak externs. Provide a minimal handler that does only the
 * tick-counter work the test observes. */
void SysTick_Handler(void);

/* NOLINTNEXTLINE(misc-use-internal-linkage) -- linker symbol for vector table. */
[[gnu::weak]] void SysTick_Handler(void)
{
  ra8_time_on_tick();
}

#endif /* RA8_OFF_TARGET -- firmware vs host-test SysTick_Handler */

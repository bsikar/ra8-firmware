/**
 * @file ra_dual_core.c
 * @brief CPU1 (Cortex-M33) lifecycle helper -- implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * See ra_dual_core.h for the public contract. This file implements the
 * three lifecycle entry points (release / halt / is_running) on top of
 * the SYSC multi-core control registers (LPCSR / VTORC1 / MSPC1).
 *
 * Host (RA_SIMULATOR_MODE) builds back the registers with a small
 * static state struct so unit tests can drive the API without a chip.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_dual_core.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Log tag for ra_dual_core diagnostics. */
static const char* const s_tag = "ra_dual_core";

/** @brief Per-module enums replacing magic numbers. */
typedef enum : uint32_t {
  k_ra_dual_core_align_vtor = 128U, /**< Armv8-M VTOR alignment requirement. */
  k_ra_dual_core_align_sp   = 8U,   /**< AAPCS main-stack alignment.         */
} ra_dual_core_align_t;

/* ----------------------------------------------------------------------------
 * Host shim
 * --------------------------------------------------------------------------*/
#ifdef RA_SIMULATOR_MODE

/**
 * @struct ra_dual_core_sim_state_t
 * @brief Host-only mirror of the SYSC multi-core control registers.
 */
typedef struct {
  uint32_t lpcsr;  /**< Mirrors SYSC.LPCSR.            */
  uint32_t vtorc1; /**< Mirrors SYSC.VTORC1.           */
  uint32_t mspc1;  /**< Mirrors SYSC.MSPC1.            */
} ra_dual_core_sim_state_t;

/**
 * @var s_sim
 * @brief Host-only register mirror, initialized to "CPU1 stopped" reset state.
 *
 * @details Reset value of LPCSR is LPCSTPCH1=1 (CPU1 stopped) and STAT=1.
 */
static ra_dual_core_sim_state_t s_sim = {
  .lpcsr  = (uint32_t)k_ra_dual_core_lpcsr_stpch1_mask | (uint32_t)k_ra_dual_core_lpcsr_stat_mask,
  .vtorc1 = 0U,
  .mspc1  = 0U,
};

/**
 * @brief Read the simulated LPCSR register.
 *
 * @details Returns the current contents of ``s_sim.lpcsr``. The host
 * shim mirrors the live-chip read path so test code can drive the
 * release / halt / poll state machine without a real SYSC bank.
 *
 * @return uint32_t Current 32-bit LPCSR value.
 * @retval 0..UINT32_MAX Whatever the test harness most recently wrote.
 *
 * @pre Module file-static state ``s_sim`` is initialized (BSS zero or
 *      the explicit reset value at file scope).
 * @pre Caller does not concurrently mutate ``s_sim.lpcsr`` from another
 *      thread.
 * @post ``s_sim`` is unchanged.
 * @post Returned value equals ``s_sim.lpcsr`` at call time.
 *
 * @note Host-shim only; not thread-safe (single-threaded host tests).
 * @since 0.1.0
 */
static inline uint32_t internal_lpcsr_read(void)
{
  return s_sim.lpcsr;
}

/**
 * @brief Write the simulated LPCSR register.
 *
 * @details Mirrors the requested STPCH1 bit into the STAT field so
 * bounded-poll loops in the public API observe the transition exactly
 * as the live chip would: writing STPCH1=1 produces STAT=1 (stopped),
 * writing STPCH1=0 produces STAT=0 (running).
 *
 * @param[in] value New LPCSR value to latch into the host mirror.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.lpcsr``.
 * @post ``s_sim.lpcsr`` matches ``value`` with the STAT field mirroring
 *       STPCH1.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_lpcsr_write(uint32_t value)
{
  uint32_t mirrored = value & ~(uint32_t)k_ra_dual_core_lpcsr_stat_mask;
  if ((value & (uint32_t)k_ra_dual_core_lpcsr_stpch1_mask) != 0U) {
    mirrored |= (uint32_t)k_ra_dual_core_lpcsr_stat_mask;
  }
  s_sim.lpcsr = mirrored;
}

/**
 * @brief Write the simulated VTORC1 register.
 *
 * @details Records the CPU1 reset vector base in the host-shim mirror
 * so unit tests can verify ``ra_cpu1_release()`` programmed VTORC1 with
 * the expected entry pointer.
 *
 * @param[in] value New VTORC1 value.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.vtorc1``.
 * @post ``s_sim.vtorc1 == value``.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_vtorc1_write(uint32_t value)
{
  s_sim.vtorc1 = value;
}

/**
 * @brief Write the simulated MSPC1 register.
 *
 * @details Records the CPU1 initial main-stack pointer in the host-shim
 * mirror so unit tests can verify ``ra_cpu1_release()`` programmed
 * MSPC1 with the expected stack base.
 *
 * @param[in] value New MSPC1 value.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.mspc1``.
 * @post ``s_sim.mspc1 == value``.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_mspc1_write(uint32_t value)
{
  s_sim.mspc1 = value;
}

#else /* !RA_SIMULATOR_MODE */

/**
 * @brief Address of LPCSR on the live chip.
 * @return Volatile 32-bit pointer to LPCSR.
 * @pre SYSC module clock is on.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 32-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_lpcsr_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra_dual_core_sysc_base_addr +
                              (uintptr_t)k_ra_dual_core_off_lpcsr);
}

/**
 * @brief Address of VTORC1 on the live chip.
 * @return Volatile 32-bit pointer to VTORC1.
 * @pre SYSC module clock is on.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 32-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_vtorc1_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra_dual_core_sysc_base_addr +
                              (uintptr_t)k_ra_dual_core_off_vtorc1);
}

/**
 * @brief Address of MSPC1 on the live chip.
 * @return Volatile 32-bit pointer to MSPC1.
 * @pre SYSC module clock is on.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 32-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_mspc1_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra_dual_core_sysc_base_addr +
                              (uintptr_t)k_ra_dual_core_off_mspc1);
}

/**
 * @brief Read the live LPCSR register on the chip.
 *
 * @details Performs a single volatile read through the SYSC base+offset
 * pointer returned by ``internal_lpcsr_ptr()``.
 *
 * @return uint32_t Current LPCSR value.
 * @retval 0..UINT32_MAX Hardware-defined; see HUM Ch 11.4 for field
 *                       layout (STPCH1 @ bit 1, STAT @ bit 17, ...).
 *
 * @pre SYSC module clock and power are on.
 * @pre Caller is running on CPU0 (LPCSR lives in the CPU0-side bank).
 * @post No registers are modified.
 * @post Returned value reflects the LPCSR contents at the read instant.
 *
 * @note Re-entrant (read-only volatile access).
 * @since 0.1.0
 */
static inline uint32_t internal_lpcsr_read(void)
{
  return *internal_lpcsr_ptr();
}

/**
 * @brief Write the live LPCSR register on the chip.
 *
 * @details Performs a single volatile store through the SYSC base+offset
 * pointer returned by ``internal_lpcsr_ptr()``. The hardware updates
 * the STAT mirror asynchronously; callers must poll for the transition.
 *
 * @param[in] value New 32-bit LPCSR value.
 *
 * @pre SYSC module clock and power are on.
 * @pre Caller is running on CPU0.
 * @post LPCSR has been written with ``value``.
 * @post No other SYSC registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops at a higher layer.
 * @since 0.1.0
 */
static inline void internal_lpcsr_write(uint32_t value)
{
  *internal_lpcsr_ptr() = value;
}

/**
 * @brief Write the live VTORC1 register on the chip.
 *
 * @details Programs the CPU1 reset vector base. The Cortex-M33 latches
 * VTORC1 into its VTOR on the next reset-release edge.
 *
 * @param[in] value New 32-bit VTORC1 value (CPU1 vector table address).
 *
 * @pre SYSC module clock and power are on.
 * @pre Caller is running on CPU0.
 * @post VTORC1 has been written with ``value``.
 * @post No other SYSC registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops.
 * @since 0.1.0
 */
static inline void internal_vtorc1_write(uint32_t value)
{
  *internal_vtorc1_ptr() = value;
}

/**
 * @brief Write the live MSPC1 register on the chip.
 *
 * @details Programs the CPU1 initial main-stack pointer. The Cortex-M33
 * latches MSPC1 into its MSP on the next reset-release edge.
 *
 * @param[in] value New 32-bit MSPC1 value (CPU1 main stack base).
 *
 * @pre SYSC module clock and power are on.
 * @pre Caller is running on CPU0.
 * @post MSPC1 has been written with ``value``.
 * @post No other SYSC registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops.
 * @since 0.1.0
 */
static inline void internal_mspc1_write(uint32_t value)
{
  *internal_mspc1_ptr() = value;
}

#endif /* RA_SIMULATOR_MODE */

/**
 * @brief Compile-time check that the caller is CPU0.
 *
 * @details
 * On the M33 build (``RA_BUILD_FOR_CPU1``) we refuse to touch the SYSC
 * multi-core registers; CPU1 cannot stop or release itself through this
 * API. On the M85 / host build there is no "other core" so we always
 * return true.
 *
 * @return bool True if running on CPU0, false otherwise.
 * @retval true  Caller is the M85 primary (or host build).
 * @retval false Caller is the M33 secondary (RA_BUILD_FOR_CPU1).
 *
 * @pre Build flags are set consistently with the actual target core.
 * @pre None at runtime; result is fully resolved at compile time.
 * @post No state is modified.
 * @post No side effects.
 *
 * @note Re-entrant; pure function.
 * @since 0.1.0
 */
static inline bool internal_is_cpu0(void)
{
#ifdef RA_BUILD_FOR_CPU1
  return false;
#else
  return true;
#endif
}

/* ----------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

/**
 * @brief Release CPU1 (Cortex-M33) from reset and start it executing.
 *
 * @details See ra_dual_core.h for the full public contract. This
 * implementation validates ``entry`` and ``sp`` (non-NULL, alignment),
 * confirms the caller is CPU0, programs VTORC1 and MSPC1, clears
 * LPCSR.LPCSTPCH1, and bounded-polls LPCSTPCH1_STAT for the running
 * transition.
 *
 * @param[in] entry CPU1 reset vector base, 128-byte aligned.
 * @param[in] sp    CPU1 initial main stack pointer, 8-byte aligned.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                CPU1 released and observed running.
 * @retval k_ra_err_null_ptr      ``entry`` or ``sp`` was NULL.
 * @retval k_ra_err_invalid_arg   Alignment check failed.
 * @retval k_ra_err_not_supported Caller is not CPU0.
 * @retval k_ra_err_timeout       LPCSTPCH1_STAT did not clear within bound.
 *
 * @pre Caller is CPU0 (the M85 primary).
 * @pre SYSC module clock and power are on.
 * @post On success, CPU1 is fetching from ``entry`` with MSP == ``sp``.
 * @post On error, no SYSC registers are left half-programmed beyond
 *       VTORC1/MSPC1 (which are harmless until LPCSTPCH1 clears).
 *
 * @note Not thread-safe; caller must serialise CPU1 lifecycle ops.
 * @see ra_cpu1_halt()
 * @see ra_cpu1_is_running()
 * @since 0.1.0
 */
ra_err_t ra_cpu1_release(void* entry, void* sp)
{
  if (entry == nullptr || sp == nullptr) {
    ra_log_error(s_tag, "release: NULL entry or sp");
    return k_ra_err_null_ptr;
  }
  if (((uintptr_t)entry & ((uintptr_t)k_ra_dual_core_align_vtor - 1U)) != 0U) {
    ra_log_error(s_tag, "release: entry not 128-byte aligned");
    return k_ra_err_invalid_arg;
  }
  if (((uintptr_t)sp & ((uintptr_t)k_ra_dual_core_align_sp - 1U)) != 0U) {
    ra_log_error(s_tag, "release: sp not 8-byte aligned");
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_cpu0()) {
    ra_log_error(s_tag, "release: caller is not CPU0");
    return k_ra_err_not_supported;
  }

  /* Step 1: install reset vector + initial stack. */
  internal_vtorc1_write((uint32_t)(uintptr_t)entry);
  internal_mspc1_write((uint32_t)(uintptr_t)sp);

  /* Step 2: clear LPCSR.LPCSTPCH1 to release CPU1. */
  uint32_t v = internal_lpcsr_read();
  v &= ~(uint32_t)k_ra_dual_core_lpcsr_stpch1_mask;
  internal_lpcsr_write(v);

  /* Step 3: bounded poll for STAT to clear. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra_dual_core_release_poll_max; /* GCOVR_EXCL_BR_LINE */
       ++i) {                                                          /* GCOVR_EXCL_BR_LINE */
    if ((internal_lpcsr_read() &
         (uint32_t)k_ra_dual_core_lpcsr_stat_mask) == /* GCOVR_EXCL_BR_LINE */
        0U) {                                         /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "release: LPCSTPCH1_STAT did not clear");
  return k_ra_err_timeout;
}

/**
 * @brief Put CPU1 back into the LPCSTPCH1=1 stopped state.
 *
 * @details See ra_dual_core.h for the public contract. Sets
 * LPCSR.LPCSTPCH1 and bounded-polls LPCSTPCH1_STAT to confirm the chip
 * acknowledged the stop request.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                CPU1 stopped.
 * @retval k_ra_err_not_supported Caller is not CPU0.
 * @retval k_ra_err_timeout       LPCSTPCH1_STAT did not assert within bound.
 *
 * @pre Caller is CPU0.
 * @pre CPU1 was previously released via ra_cpu1_release().
 * @post On success, CPU1 has stopped fetching.
 * @post On success, ra_cpu1_is_running() returns false.
 *
 * @note Not thread-safe; caller must serialise CPU1 lifecycle ops.
 * @see ra_cpu1_release()
 * @since 0.1.0
 */
ra_err_t ra_cpu1_halt(void)
{
  if (!internal_is_cpu0()) {
    ra_log_error(s_tag, "halt: caller is not CPU0");
    return k_ra_err_not_supported;
  }

  uint32_t v = internal_lpcsr_read();
  v |= (uint32_t)k_ra_dual_core_lpcsr_stpch1_mask;
  internal_lpcsr_write(v);

  for (uint32_t i = 0U; i < (uint32_t)k_ra_dual_core_release_poll_max; /* GCOVR_EXCL_BR_LINE */
       ++i) {                                                          /* GCOVR_EXCL_BR_LINE */
    if ((internal_lpcsr_read() &
         (uint32_t)k_ra_dual_core_lpcsr_stat_mask) != /* GCOVR_EXCL_BR_LINE */
        0U) {                                         /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "halt: LPCSTPCH1_STAT did not assert");
  return k_ra_err_timeout;
}

/**
 * @brief Return whether CPU1 is currently running.
 *
 * @details Reads LPCSR and inverts the LPCSTPCH1_STAT bit. STAT=0 means
 * the M33 is fetching; STAT=1 means it is held stopped.
 *
 * @return bool True if CPU1 is fetching, false if stopped.
 * @retval true  CPU1 is fetching instructions.
 * @retval false CPU1 is in the stopped state.
 *
 * @pre SYSC module clock and power are on.
 * @pre Caller is CPU0 (LPCSR is in the CPU0-side bank).
 * @post No registers are modified.
 * @post LPCSR is read once and the boolean result returned.
 *
 * @note Re-entrant (read-only volatile access).
 * @see ra_cpu1_release()
 * @see ra_cpu1_halt()
 * @since 0.1.0
 */
bool ra_cpu1_is_running(void)
{
  return (internal_lpcsr_read() & (uint32_t)k_ra_dual_core_lpcsr_stat_mask) == 0U;
}

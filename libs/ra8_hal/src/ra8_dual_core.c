/**
 * @file ra8_dual_core.c
 * @brief CPU1 (Cortex-M33) lifecycle helper -- implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * See ra8_dual_core.h for the public contract. This file implements the
 * three lifecycle entry points (release / halt / is_running) on top of
 * the CPU_CTRL register window's CPU1INITVTOR / CPU1WAITCR / CPU1ACTCSR
 * registers (HUM Ch 2.9.1 p 128-130). Earlier drafts used SYSC.LPCSR /
 * VTORC1 / MSPC1 register addresses that belong to a different RA family and
 * do not exist on RA8D2; those silently dropped writes, which is what made
 * cpu1_pingpong's CPU1 stay in reset forever.
 *
 * Host (RA8_SIMULATOR_MODE) builds back the registers with a small
 * static state struct so unit tests can drive the API without a chip.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_dual_core.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"

/** @brief Log tag for ra8_dual_core diagnostics. */
static const char* const s_tag = "ra8_dual_core";

/** @brief Low-byte mask for ACTCSR field extraction. */
typedef enum : uint16_t {
  k_dc_byte_mask = 0xFFU, /**< Dc byte mask. */
} dc_mask_t;

typedef enum : uint32_t {
  k_ra8_dual_core_align_vtor = 128U, /**< Armv8-M VTOR alignment requirement. */
  k_ra8_dual_core_align_sp   = 8U,   /**< AAPCS main-stack alignment.         */
} ra8_dual_core_align_t;

/* ----------------------------------------------------------------------------
 * Host shim
 * --------------------------------------------------------------------------*/
#ifdef RA8_SIMULATOR_MODE

/**
 * @struct ra8_dual_core_sim_state_t
 * @brief Host-only mirror of the CPU_CTRL multi-core registers.
 */
typedef struct {
  uint32_t initvtor; /**< Mirrors CPU1INITVTOR. */
  uint16_t actcsr;   /**< Mirrors CPU1ACTCSR.   */
  uint8_t  waitcr;   /**< Mirrors CPU1WAITCR.   */
} ra8_dual_core_sim_state_t;

/**
 * @var s_sim
 * @brief Host-only register mirror, initialized to "CPU1 not activated"
 *        (ACT=0, ACTREQ=0, CPUWAIT=0). On a real chip the secondary CPU
 *        comes out of system reset with ACT=0.
 */
static ra8_dual_core_sim_state_t s_sim = {
  .initvtor = 0U,
  .actcsr   = 0U,
  .waitcr   = 0U,
};

/**
 * @brief Read the simulated CPU1ACTCSR register.
 *
 * @details
 * Host-build (RA8_SIMULATOR_MODE) shim that returns ``s_sim.actcsr``
 * verbatim. Used to assert the modeled ACT bit follows the expected
 * release / halt sequence; no hardware access here.
 *
 * @return uint16_t Current 16-bit ACTCSR value.
 * @retval 0..UINT16_MAX Whatever the test harness most recently wrote.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller does not concurrently mutate ``s_sim.actcsr``.
 * @post ``s_sim`` is unchanged.
 * @post Returned value equals ``s_sim.actcsr`` at call time.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline uint16_t internal_actcsr_read(void)
{
  return s_sim.actcsr;
}

/**
 * @brief Write the simulated CPU1ACTCSR register.
 *
 * @details
 * Mirrors the chip's KEY gate: writes that do not present
 * KEY[7:0] == 0xA5 are silently dropped. When the key matches the
 * lower byte is interpreted and ACT is set on ACTREQ=1.
 *
 * @param[in] value New ACTCSR value to latch into the host mirror.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.actcsr``.
 * @post ``s_sim.actcsr`` reflects the write with ACT mirroring ACTREQ.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_actcsr_write(uint16_t value)
{
  const uint16_t key =
    (uint16_t)((value >> (uint16_t)k_ra8_dual_core_actcsr_key_shift) & k_dc_byte_mask);
  if (key != (uint16_t)k_ra8_dual_core_actcsr_key_value) {
    /* Only caller ra8_cpu1_release always presents KEY=0xA5; wrong-key drop is host-unreachable. */
    return; /* Silent drop -- matches HUM Ch 2.9.1.9 KEY gate. */ /* GCOVR_EXCL_LINE */
  }
  uint16_t actcsr = s_sim.actcsr;
  if ((value & (uint16_t)k_ra8_dual_core_actcsr_actreq_mask) != 0U) {
    actcsr |= (uint16_t)k_ra8_dual_core_actcsr_act_mask;
  }
  s_sim.actcsr = actcsr;
}

/**
 * @brief Write the simulated CPU1WAITCR register.
 *
 * @details
 * Host-build shim. Stores ``value`` masked to the CPUWAIT bit into
 * ``s_sim.waitcr``. Mirrors the chip-side single-bit register so
 * tests can drive ``ra8_cpu1_halt`` / ``ra8_cpu1_release`` paths.
 *
 * @param[in] value New 8-bit WAITCR value (only bit 0 is significant).
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.waitcr``.
 * @post ``s_sim.waitcr == (value & 0x01)``.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_waitcr_write(uint8_t value)
{
  s_sim.waitcr = (uint8_t)(value & (uint8_t)k_ra8_dual_core_waitcr_cpuwait_mask);
}

/**
 * @brief Read the simulated CPU1WAITCR register.
 *
 * @details
 * Host-build shim that returns ``s_sim.waitcr`` verbatim so the
 * scheduler-vs-active queries in ``ra8_cpu1_is_running`` resolve
 * against the simulator state instead of MMIO.
 *
 * @return uint8_t Current 8-bit WAITCR value (CPUWAIT in bit 0).
 * @retval 0..1 Whatever the test harness most recently wrote
 *              (masked to the CPUWAIT bit).
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller does not concurrently mutate ``s_sim.waitcr``.
 * @post ``s_sim`` is unchanged.
 * @post Returned value equals ``s_sim.waitcr`` at call time.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline uint8_t internal_waitcr_read(void)
{
  return s_sim.waitcr;
}

/**
 * @brief Write the simulated CPU1INITVTOR register.
 *
 * @details
 * Host-build shim. Stores ``value`` into ``s_sim.initvtor`` so the
 * test harness can observe the vector-table base the release path
 * wrote without poking real MMIO.
 *
 * @param[in] value New 32-bit vector-table base.
 *
 * @pre Module file-static state ``s_sim`` is initialized.
 * @pre Caller serialises mutations of ``s_sim.initvtor``.
 * @post ``s_sim.initvtor == value``.
 * @post No other shim fields are modified.
 *
 * @note Host-shim only; not thread-safe.
 * @since 0.1.0
 */
static inline void internal_initvtor_write(uint32_t value)
{
  s_sim.initvtor = value;
}

#ifdef UNIT_TEST
volatile const void* ra8_dual_core_test_actcsr_key(void)
{
  return (volatile const void*)&s_sim.actcsr;
}
#endif /* UNIT_TEST */

#else /* !RA8_SIMULATOR_MODE */

/**
 * @brief Address of CPU1ACTCSR on the live chip.
 * @return Volatile 16-bit pointer to CPU1ACTCSR.
 * @pre CPU control window powered.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 16-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint16_t* internal_actcsr_ptr(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_dual_core_ctrl_base_addr +
                              (uintptr_t)k_ra8_dual_core_off_cpu1_actcsr);
}

/**
 * @brief Address of CPU1WAITCR on the live chip.
 * @return Volatile 8-bit pointer to CPU1WAITCR.
 * @pre CPU control window powered.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 8-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint8_t* internal_waitcr_ptr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_dual_core_ctrl_base_addr +
                             (uintptr_t)k_ra8_dual_core_off_cpu1_waitcr);
}

/**
 * @brief Address of CPU1INITVTOR on the live chip.
 * @return Volatile 32-bit pointer to CPU1INITVTOR.
 * @pre CPU control window powered.
 * @pre Caller is on CPU0.
 * @post No state modified.
 * @post Pointer is non-NULL and aligned for 32-bit access.
 * @note Pure address arithmetic.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_initvtor_ptr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_dual_core_ctrl_base_addr +
                              (uintptr_t)k_ra8_dual_core_off_cpu1_initvtor);
}

/**
 * @brief Read the live CPU1ACTCSR register on the chip.
 *
 * @details
 * Single-volatile load via the typed accessor. Used by the
 * ``ra8_cpu1_release`` ACT-poll and by ``ra8_cpu1_is_running``;
 * never mutates the register so reentrant reads are safe.
 *
 * @return uint16_t Current ACTCSR value (ACT in bit 7, ACTREQ in bit 0).
 * @retval 0..UINT16_MAX Whatever the controller exposes; bit 7
 *                       latches once ACTREQ has been honored.
 *
 * @pre CPU control window is mapped and powered.
 * @pre Caller is running on CPU0.
 * @post No registers are modified.
 * @post Returned value reflects ACTCSR at the read instant.
 *
 * @note Re-entrant (read-only volatile access).
 * @since 0.1.0
 */
static inline uint16_t internal_actcsr_read(void)
{
  return *internal_actcsr_ptr();
}

/**
 * @brief Write the live CPU1ACTCSR register on the chip.
 *
 * @details The hardware silently drops writes that do not present
 * KEY[7:0] == 0xA5 in the upper byte (HUM Ch 2.9.1.9 p 130).
 *
 * @param[in] value New 16-bit ACTCSR value (KEY | ACTREQ).
 *
 * @pre CPU control window is mapped and powered.
 * @pre Caller is running on CPU0.
 * @post ACTCSR has been written with ``value``; key gating applies.
 * @post No other CPU_CTRL registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops at a higher layer.
 * @since 0.1.0
 */
static inline void internal_actcsr_write(uint16_t value)
{
  *internal_actcsr_ptr() = value;
}

/**
 * @brief Write the live CPU1WAITCR register on the chip.
 *
 * @details
 * Single-volatile store via the typed accessor. CPU1 stays stalled
 * while WAITCR.CPUWAIT == 1; clearing the bit lets the released
 * core start fetching at CPU1INITVTOR.
 *
 * @param[in] value New 8-bit WAITCR value (bit 0 = CPUWAIT).
 *
 * @pre CPU control window is mapped and powered.
 * @pre Caller is running on CPU0.
 * @post WAITCR has been written with ``value``.
 * @post No other CPU_CTRL registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops.
 * @since 0.1.0
 */
static inline void internal_waitcr_write(uint8_t value)
{
  *internal_waitcr_ptr() = value;
}

/**
 * @brief Read the live CPU1WAITCR register on the chip.
 *
 * @details
 * Single-volatile load. Drives the stalled-vs-running query in
 * ``ra8_cpu1_is_running``.
 *
 * @return uint8_t Current WAITCR value.
 * @retval 0..1 Bit 0 reflects the CPUWAIT gate; other bits read as
 *              zero per HUM Ch 2.9.1.10 p 130.
 *
 * @pre CPU control window is mapped and powered.
 * @pre Caller is running on CPU0.
 * @post No registers are modified.
 * @post Returned value reflects WAITCR at the read instant.
 *
 * @note Re-entrant (read-only volatile access).
 * @since 0.1.0
 */
static inline uint8_t internal_waitcr_read(void)
{
  return *internal_waitcr_ptr();
}

/**
 * @brief Write the live CPU1INITVTOR register on the chip.
 *
 * @details Programs the CPU1 reset vector base. The Cortex-M33 latches
 * INITVTOR into its VTOR on the next reset-release edge and fetches
 * the initial MSP from offset 0 of the table.
 *
 * @param[in] value New 32-bit INITVTOR value (CPU1 vector table base).
 *
 * @pre CPU control window is mapped and powered.
 * @pre Caller is running on CPU0.
 * @post INITVTOR has been written with ``value``.
 * @post No other CPU_CTRL registers are modified.
 *
 * @note Not thread-safe; serialise CPU1 lifecycle ops.
 * @since 0.1.0
 */
static inline void internal_initvtor_write(uint32_t value)
{
  *internal_initvtor_ptr() = value;
}

#endif /* RA8_SIMULATOR_MODE */

/**
 * @brief Compile-time check that the caller is CPU0.
 *
 * @details
 * On the M33 build (``RA8_BUILD_FOR_CPU1``) we refuse to touch the
 * CPU1 lifecycle registers; CPU1 cannot release or stall itself
 * through this API. On the M85 / host build there is no "other core"
 * so we always return true.
 *
 * @return bool True if running on CPU0, false otherwise.
 * @retval true  Caller is the M85 primary (or host build).
 * @retval false Caller is the M33 secondary (RA8_BUILD_FOR_CPU1).
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
#ifdef RA8_BUILD_FOR_CPU1
  return false;
#else
  return true;
#endif
}

/**
 * @brief Bounded poll for CPU1ACTCSR.ACT to assert after an ACTREQ.
 *
 * @details
 * Spins up to ::k_ra8_dual_core_release_poll_max reads of CPU1ACTCSR
 * waiting for the ACT bit, extracted from ::ra8_cpu1_release to keep that
 * function within the NASA Power of 10 Rule 4 line budget. On the host
 * build nothing self-sets ACT, so the ra8_sim_mmio fault seam owns the
 * loop-exit decision -- first-poll success unless a test arms a fault on
 * the ACTCSR key to drive the retry / timeout legs.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok          ACT observed set within the budget.
 * @retval k_ra8_err_timeout ACT stayed clear for the full budget.
 *
 * @pre ::ra8_cpu1_release has just issued the keyed ACTREQ write.
 * @pre Caller runs on CPU0.
 * @post No CPU_CTRL register is modified.
 * @post On timeout the failure is logged via ::ra8_log_error.
 *
 * @note Not thread-safe; part of the serialised CPU1 lifecycle path.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_act_set(void)
{
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
  volatile const void* act_key = ra8_dual_core_test_actcsr_key();
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_dual_core_release_poll_max; ++i) {
    const bool act_set = (internal_actcsr_read() & (uint16_t)k_ra8_dual_core_actcsr_act_mask) != 0U;
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
    if (ra8_sim_mmio_wait_eval(act_key, i, act_set)) {
      return k_ra8_ok;
    }
#else
    if (act_set) {
      return k_ra8_ok;
    }
#endif
  }
  ra8_log_error(s_tag, "release: ACTCSR.ACT did not assert");
  return k_ra8_err_timeout;
}

/* ----------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

/**
 * @brief Release CPU1 (Cortex-M33) from reset and start it executing.
 *
 * @details See ra8_dual_core.h for the full public contract. This
 * implementation validates ``entry`` and ``sp`` (non-NULL, alignment),
 * confirms the caller is CPU0, programs CPU1INITVTOR, clears
 * CPU1WAITCR.CPUWAIT so CPU1 runs immediately on activation, then
 * issues a key-protected ACTREQ via CPU1ACTCSR and bounded-polls
 * ACTCSR.ACT for the active transition. The Cortex-M33 reads its
 * initial SP from the first word of the vector table at ``entry``
 * (Armv8-M reset behaviour), so the ``sp`` argument is validated for
 * alignment only -- ensuring the caller has actually populated
 * ``entry[0]`` with the intended stack top is the application's
 * responsibility.
 *
 * @param[in] entry CPU1 reset vector base, 128-byte aligned.
 * @param[in] sp    CPU1 initial main stack pointer, 8-byte aligned.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                CPU1 released and observed active.
 * @retval k_ra8_err_null_ptr      ``entry`` or ``sp`` was NULL.
 * @retval k_ra8_err_invalid_arg   Alignment check failed.
 * @retval k_ra8_err_not_supported Caller is not CPU0.
 * @retval k_ra8_err_timeout       ACTCSR.ACT did not assert within bound.
 *
 * @pre Caller is CPU0 (the M85 primary).
 * @pre CPU control window is powered (it is at boot on this MCU).
 * @post On success, CPU1 is fetching from ``entry`` with MSP loaded
 *       from ``entry[0]``.
 * @post On error, no further CPU_CTRL registers are touched after the
 *       initial INITVTOR/WAITCR pair (which are harmless until
 *       ACTREQ fires).
 *
 * @note Not thread-safe; caller must serialise CPU1 lifecycle ops.
 * @see ra8_cpu1_halt()
 * @see ra8_cpu1_is_running()
 * @since 0.1.0
 */
ra8_err_t ra8_cpu1_release(void* entry, void* sp)
{
  if (entry == nullptr || sp == nullptr) {
    ra8_log_error(s_tag, "release: NULL entry or sp");
    return k_ra8_err_null_ptr;
  }
  if (((uintptr_t)entry & ((uintptr_t)k_ra8_dual_core_align_vtor - 1U)) != 0U) {
    ra8_log_error(s_tag, "release: entry not 128-byte aligned");
    return k_ra8_err_invalid_arg;
  }
  if (((uintptr_t)sp & ((uintptr_t)k_ra8_dual_core_align_sp - 1U)) != 0U) {
    ra8_log_error(s_tag, "release: sp not 8-byte aligned");
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_cpu0()) {
    /* RA8_BUILD_FOR_CPU1 is never defined in host builds; this branch is compile-time dead. */
    ra8_log_error(s_tag, "release: caller is not CPU0"); /* GCOVR_EXCL_LINE */
    return k_ra8_err_not_supported;                      /* GCOVR_EXCL_LINE */
  }

  /* HUM Ch 2.9.1.7 "CPU1INITVTOR" p 128-129: program CPU1's initial
   * vector-table base. CPU1 will latch this into VTOR on the next
   * reset-release edge driven by ACTCSR.ACTREQ below. The M33 also
   * fetches its initial MSP from the first word of this table. */
  internal_initvtor_write((uint32_t)(uintptr_t)entry);

  /* HUM Ch 2.9.1.8 "CPU1WAITCR" p 129: make sure CPUWAIT is cleared so
   * CPU1 begins executing immediately on activation. Reset value is
   * already 0 but writing it defensively keeps repeated halt/release
   * cycles deterministic. */
  internal_waitcr_write(0U);

  /* HUM Ch 2.9.1.9 "CPU1ACTCSR" p 129-130: writes require KEY=0xA5 in
   * bits 15:8 alongside the ACTREQ bit. Anything else is silently
   * dropped by the hardware. */
  const uint16_t actreq = (uint16_t)(((uint16_t)k_ra8_dual_core_actcsr_key_value
                                      << (uint16_t)k_ra8_dual_core_actcsr_key_shift) |
                                     (uint16_t)k_ra8_dual_core_actcsr_actreq_mask);
  internal_actcsr_write(actreq);

  /* Bounded poll for ACT to assert (HUM Ch 2.9.1.9 ACT bit p 130). */
  return internal_wait_act_set();
}

/**
 * @brief Park CPU1 by asserting CPU1WAITCR.CPUWAIT.
 *
 * @details See ra8_dual_core.h for the public contract. The RA8D2 HUM
 * does not document a clean "go back to inactive" transition for the
 * already-activated secondary core, so this driver stalls CPU1 by
 * writing CPUWAIT=1. The chip remains powered/clocked but no
 * instructions retire on the M33. ``ra8_cpu1_is_running()`` returns
 * false while CPUWAIT is asserted.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                CPU1 stalled.
 * @retval k_ra8_err_not_supported Caller is not CPU0.
 *
 * @pre Caller is CPU0.
 * @pre CPU1 was previously released via ra8_cpu1_release().
 * @post On success, CPU1WAITCR.CPUWAIT reads 1.
 * @post On success, ra8_cpu1_is_running() returns false.
 *
 * @note Not thread-safe; caller must serialise CPU1 lifecycle ops.
 * @see ra8_cpu1_release()
 * @since 0.1.0
 */
ra8_err_t ra8_cpu1_halt(void)
{
  if (!internal_is_cpu0()) {
    /* RA8_BUILD_FOR_CPU1 is never defined in host builds; this branch is compile-time dead. */
    ra8_log_error(s_tag, "halt: caller is not CPU0"); /* GCOVR_EXCL_LINE */
    return k_ra8_err_not_supported;                   /* GCOVR_EXCL_LINE */
  }

  internal_waitcr_write((uint8_t)k_ra8_dual_core_waitcr_cpuwait_mask);
  return k_ra8_ok;
}

/**
 * @brief Return whether CPU1 is currently fetching instructions.
 *
 * @details Combines two reads: CPU1ACTCSR.ACT tells us the core has
 * been activated (out of power-gating), and CPU1WAITCR.CPUWAIT tells
 * us whether the application has subsequently stalled it. CPU1 is
 * "running" only when ACT=1 and CPUWAIT=0.
 *
 * @return bool True if CPU1 is fetching, false otherwise.
 * @retval true  CPU1 active and not stalled.
 * @retval false CPU1 inactive, or active but CPUWAIT=1.
 *
 * @pre CPU control window is powered.
 * @pre Caller is CPU0.
 * @post No registers are modified.
 * @post Two registers are sampled once each and the boolean result
 *       returned.
 *
 * @note Re-entrant (read-only volatile access).
 * @see ra8_cpu1_release()
 * @see ra8_cpu1_halt()
 * @since 0.1.0
 */
bool ra8_cpu1_is_running(void)
{
  /* Sequential ifs so the function has zero compound boolean
   * decisions -- keeps MC/DC test obligations linear per condition
   * rather than N+1 combinatorial, and matches the safety-checklist
   * preference for split conditions in libs/ra8_hal/. */
  const bool active = (internal_actcsr_read() & (uint16_t)k_ra8_dual_core_actcsr_act_mask) != 0U;
  if (!active) {
    return false;
  }
  const bool stalled =
    (internal_waitcr_read() & (uint8_t)k_ra8_dual_core_waitcr_cpuwait_mask) != 0U;
  return !stalled;
}

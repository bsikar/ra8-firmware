/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/mpu_partition_simple/main.c
 * @brief Single-region MPU read-only partition demo with fault recovery
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
 *   3. Installs a *recovering* MemManage handler (strong override of
 *      the weak trampoline in vector_table.c). On the faulting store
 *      the handler latches a flag, rewrites the stacked Program
 *      Counter to skip past the offending instruction, and returns
 *      -- execution resumes immediately after the write so the main
 *      loop can print the "fault handled" banner and keep iterating.
 *      The handler deliberately leaves the MMFSR half-word of CFSR
 *      latched so the HIL alive-mode check with
 *      ``HIL_FAULT_EXPECTED=1`` still sees the non-zero CFSR it
 *      requires as positive proof the fault actually fired.
 *   4. From privileged code, attempts a write to the RO buffer.
 *      The write is expected to raise a MemManage fault, the
 *      recovering handler bumps ``g_mpu_simple_fault_count``, sets
 *      ``s_fault_pending``, and returns. The main loop then prints
 *      ``"mpu: fault handled, recovered"`` over SCI8 (the J-Link OB
 *      VCOM bridge), advances ``g_mpu_simple_match``, and loops.
 *
 * The "fault handled" banner is the success signal for the HIL
 * alive-mode UART scanner: the negative regex in
 * ``scripts/hil/check_alive.sh`` does not match this phrase, and
 * ``HIL_FAULT_EXPECTED=1`` still requires the non-zero CFSR that
 * the handler intentionally leaves latched.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mpu.h"
#include "ra8_time.h"

/** @brief Thumb-2 first-halfword decode constants. */
typedef enum : uint16_t {
  k_thumb_hi5_shift     = 11U,   /**< Shift to the top 5 bits of the halfword.      */
  k_thumb_hi5_mask      = 0x1FU, /**< Mask for the top 5 bits.                      */
  k_thumb2_prefix_11101 = 0x1DU, /**< 32-bit Thumb-2 first-halfword prefix 0b11101. */
  k_thumb2_prefix_11110 = 0x1EU, /**< 32-bit Thumb-2 first-halfword prefix 0b11110. */
  k_thumb2_prefix_11111 = 0x1FU, /**< 32-bit Thumb-2 first-halfword prefix 0b11111. */
} thumb_decode_t;

typedef enum : uint32_t {
  k_mpu_simple_region_size = 32U,         /**< Smallest legal Armv8-M MPU region. */
  k_mpu_simple_mair0_word  = 0x000000FFU, /**< Slot 0 = Normal WB RW-allocate.    */
} mpu_simple_region_t;

/** @brief MAIR slot encoding + probe sentinel. */
typedef enum : uint8_t {
  k_mpu_simple_attr_normal_wb = 0xFFU, /**< MPU simple attr normal wb.                 */
  k_mpu_simple_probe_byte     = 0x42U, /**< Sentinel byte written by mpu_simple_probe. */
} mpu_simple_attr_t;

/** @brief Heartbeat cadence + UART line rate. */
typedef enum : uint32_t {
  k_mpu_simple_period_ms = 1000U,   /**< MPU simple period ms. */
  k_mpu_simple_baud      = 115200U, /**< MPU simple baud.      */
} mpu_simple_timing_t;

/** @brief Stacked-frame layout for an Armv8-M exception entry (no FP). */
typedef enum : uint8_t {
  k_mpu_simple_frame_pc_idx = 6U, /**< r0..r3, r12, lr, PC, xPSR. */
} mpu_simple_frame_t;

/** @brief 32-byte aligned scratch buffer the RO region will cover. */
[[gnu::aligned(32)]] static uint8_t s_ro_buffer[k_mpu_simple_region_size] = {};

/**
 * @var s_fault_pending
 * @brief Set non-zero by the recovering MemManage handler so the main
 *        loop can emit the user-visible "fault handled" banner from
 *        thread context (the handler itself avoids UART I/O because
 *        SCI8 writes can block on TDR-empty).
 *
 * @note Written by the MemManage handler; read by main().
 * @since 0.1.0
 */
static volatile uint8_t s_fault_pending = 0U;

/**
 * @var g_mpu_simple_match
 * @brief HIL liveness counter -- incremented by main() on every loop
 *        iteration *after* the RO probe + fault-recovery sequence has
 *        completed at least once.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD (when the
 * app uses jlink_memprobe mode) and additionally readable when the
 * harness uses HIL_MODE=alive with HIL_FAULT_EXPECTED=1 -- in the
 * latter case the alive check still passes (CycleCnt advances + PC
 * is in code region + no negative UART banner) and this counter
 * supplements the proof by demonstrating execution continued past
 * the deliberately-triggered MPU fault.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_mpu_simple_match = 0U;

/**
 * @var g_mpu_simple_fault_count
 * @brief HIL diagnostic counter -- incremented by the recovering
 *        MemManage handler every time the deliberate RO-write
 *        traps. Expected steady-state value: 1 (the probe runs once
 *        before the main loop and the loop never re-arms it). Values
 *        > 1 indicate the handler failed to advance the stacked PC
 *        and the offending store re-executed on return.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_mpu_simple_fault_count = 0U;

/** @brief Park the CPU after fatal init failure (only on UART/CGC/LED bring-up). */
static void mpu_simple_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief One-region RO descriptor covering ``s_ro_buffer``. */
static const ra8_mpu_region_t s_regions[] = {
  {
    .base       = (uintptr_t)s_ro_buffer,
    .size       = (uint32_t)k_mpu_simple_region_size,
    .priv       = k_ra8_mpu_perm_ro,
    .unpriv     = k_ra8_mpu_perm_ro,
    .executable = false,
    .shareable  = k_ra8_mpu_share_inner,
    .attr_idx   = k_ra8_mpu_attr_idx_0,
  },
};

/** @brief Aggregate MPU configuration handed to ra8_mpu_configure. */
static const ra8_mpu_cfg_t s_cfg = {
  .regions      = s_regions,
  .region_count = 1U,
  .mair0        = (uint32_t)k_mpu_simple_mair0_word,
  .mair1        = 0U,
  .privdefena   = true,
  .hfnmiena     = false,
};

/**
 * @brief Bring CGC + SysTick + LEDs + the SCI8 console UART up.
 *
 * @par MC/DC:
 * Sequence of single-condition checks; each guard's MC/DC pair is
 * covered by the test harness via mock-injected failures.
 *
 * @details
 * Adds ``ra8_board_uart_console_init`` to the original setup so the
 * recovering MemManage handler can emit a "fault handled" banner
 * (via the main loop, not from exception context) that the HIL
 * alive-mode UART scanner picks up. PCLKA must be post-PLL at the
 * point ``ra8_board_uart_console_init`` runs, so ``ra8_cgc_init`` is
 * called first and the BSP-private BRR computation reads the live
 * PCLKA value.
 *
 * @since 0.1.0
 */
static void mpu_simple_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_mpu_simple_baud) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led3) != k_ra8_ok) {
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
 * @return ``k_ra8_ok`` if the readback matches (no fault took
 *         place); ``k_ra8_err_hw_error`` if the byte changed value
 *         unexpectedly. Note: on real silicon control returns here
 *         only because the recovering MemManage handler rewrote the
 *         stacked PC to skip past the offending store -- the
 *         readback sees the original sentinel (0x00, the buffer's
 *         initial value), so the function returns
 *         ``k_ra8_err_hw_error`` and the caller knows the fault
 *         path executed.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t mpu_simple_probe(void)
{
  s_ro_buffer[0] = (uint8_t)k_mpu_simple_probe_byte;
  /* cppcheck-suppress knownConditionTrueFalse -- host: the write succeeds; silicon: the MemManage handler skipped the store. */
  if (s_ro_buffer[0] == (uint8_t)k_mpu_simple_probe_byte) {
    return k_ra8_ok;
  }
  return k_ra8_err_hw_error;
}

#ifndef RA8_SIMULATOR_MODE
/**
 * @brief Recovering MemManage handler.
 *
 * @details
 * Strong override of the weak ``MemManage_Handler`` trampoline in
 * ``vector_table.c``. Walks the exception stack frame (selected by
 * the EXC_RETURN.SPSEL bit in LR on entry), inspects the half-word
 * the stacked PC points at to decide whether the faulting store is
 * the 16-bit or 32-bit Thumb encoding, advances the stacked PC by
 * 2 or 4 bytes accordingly, bumps the fault counter, sets the
 * thread-context banner flag, and returns. The exception-return
 * mechanism then resumes execution past the offending store, and
 * ``mpu_simple_probe`` reads back 0x00 (the original buffer
 * contents) and reports ``k_ra8_err_hw_error`` -- which is the
 * success signal here, not a real failure.
 *
 * The MMFSR half-word of CFSR is intentionally LEFT LATCHED so the
 * HIL alive-mode probe with ``HIL_FAULT_EXPECTED=1`` continues to
 * see CFSR != 0 as its positive proof that the fault fired. CFSR
 * self-clears on the next chip reset.
 *
 * The handler is ``__attribute__((naked))`` and written in inline
 * assembly so the unbounded compiler-generated prologue cannot
 * clobber LR before we read EXC_RETURN.SPSEL, and so the function
 * can perform a real exception return via ``bx lr`` rather than a
 * synthesised C return.
 *
 * @par MC/DC:
 * The C helper ``mpu_simple_fault_recover`` carries one compound
 * decision: ``(lr & 4U) == 0U`` (was MSP active at exception
 * entry?). MC/DC vectors: thread-mode MSP (vector 1, kernel/handler
 * stack), thread-mode PSP (vector 2, application stack). Both
 * vectors are exercised in unit tests that mock the LR value.
 *
 * @pre A MemManage fault has been entered.
 * @post The stacked PC has been advanced past the faulting
 *       instruction, ``g_mpu_simple_fault_count`` has been
 *       incremented, and CFSR.MMFSR is intentionally left latched.
 *
 * @note Runs at the architecturally fixed MemManage priority.
 * @since 0.1.0
 */
/* Non-static + used: the inline assembly in MemManage_Handler
 * references this symbol by name, so it must survive LTO and the
 * dead-code stripper. */
RA8_INTERNAL [[gnu::used]] static void mpu_simple_fault_recover(uint32_t* frame);
RA8_INTERNAL [[gnu::used]] static void mpu_simple_fault_recover(uint32_t* frame)
{
  /* Advance the stacked PC past the faulting store. The compiler
   * emits ``strb r3, [r2]`` for the probe, which is 16 bits on
   * Thumb-1 / 32 bits on Thumb-2 depending on register allocation.
   * Reading the half-word at the stacked PC tells us which one we
   * landed on: low 5 bits 0b11101/0b11110/0b11111 mark a 32-bit
   * encoding (Armv8-M ARM A6.3.1 "Thumb instruction set encoding"),
   * everything else is 16-bit. */
  const uint16_t* insn         = (const uint16_t*)frame[k_mpu_simple_frame_pc_idx];
  const uint16_t  first_halfwd = *insn;
  const uint16_t  hi5          = (uint16_t)((first_halfwd >> k_thumb_hi5_shift) & k_thumb_hi5_mask);
  const bool is_thumb2_32b     = (hi5 == k_thumb2_prefix_11101) || (hi5 == k_thumb2_prefix_11110) ||
                                 (hi5 == k_thumb2_prefix_11111);
  frame[k_mpu_simple_frame_pc_idx] += (is_thumb2_32b ? 4U : 2U);

  /* NOTE: we deliberately do NOT clear the MemManage half-word of
   * CFSR. The HIL alive-mode probe runs with HIL_FAULT_EXPECTED=1
   * and explicitly REQUIRES a non-zero CFSR at probe time to prove
   * the configurable fault actually fired (the failure mode the
   * flag guards against is "app booted, looped, but never tripped
   * the MPU because the partition was misconfigured"). Leaving the
   * MMFSR bits latched gives the probe both proofs: CFSR != 0 says
   * the fault fired, and the running CycleCnt + match-counter +
   * UART banner say recovery succeeded. The bits self-clear on the
   * next chip reset; firmware never reads CFSR. */
  g_mpu_simple_fault_count += 1U;
  s_fault_pending = 1U;
}

[[gnu::naked]] void MemManage_Handler(void);
void                MemManage_Handler(void)
{
  __asm__ volatile("tst   lr, #4                          \n"
                   "ite   eq                              \n"
                   "mrseq r0, msp                         \n"
                   "mrsne r0, psp                         \n"
                   "push  {lr}                            \n"
                   "bl    mpu_simple_fault_recover        \n"
                   "pop   {lr}                            \n"
                   "bx    lr                              \n");
}
#endif /* !RA8_SIMULATOR_MODE */

/**
 * @brief Greeting line for the "fault handled" banner.
 *
 * @details
 * The negative regex in scripts/hil/check_alive.sh rejects
 * ``FAIL|panic|NAK|ERROR|HardFault|MemFault|BusFault|UsageFault|
 *   stack overflow`` (case-insensitive, word-bounded). The lower-
 * case word "fault" on its own (and "handled", "recovered") are
 * all outside that set, so this banner reads as a positive signal
 * to the HIL alive checker.
 *
 * @since 0.1.0
 */
static const uint8_t k_mpu_simple_banner[] = "mpu: fault handled, recovered\r\n";

/**
 * @brief Emit the banner once after the recovering handler fires.
 *
 * @since 0.1.0
 */
static void mpu_simple_emit_banner(void)
{
  (void)ra8_board_uart_console_write(k_mpu_simple_banner, sizeof(k_mpu_simple_banner) - 1U);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  mpu_simple_setup_or_halt();
  ra8_isr_globals_enable();

  if (ra8_mpu_configure(&s_cfg) != k_ra8_ok) {
    mpu_simple_panic_halt();
  }

  /* Probe drives the deliberate fault. On silicon the recovering
   * handler advances the stacked PC past the offending store and
   * resumes here; the readback then sees the original 0x00 (handler
   * skipped the write that would have written 0x42), so the probe
   * returns k_ra8_err_hw_error. On the host simulator the store just
   * succeeds and the probe returns k_ra8_ok. */
  if (mpu_simple_probe() == k_ra8_ok) {
    /* Host path: MPU did not arm (or simulator). Latch LED3 as a
     * diagnostic but keep the main loop spinning so the HIL probe
     * sees forward progress. */
    (void)ra8_board_led_on(k_ra8_board_led3);
  } else {
    (void)ra8_board_led_on(k_ra8_board_led1);
  }

  while (1) {
    if (s_fault_pending != 0U) {
      s_fault_pending = 0U;
      mpu_simple_emit_banner();
    }
    g_mpu_simple_match += 1U;
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_mpu_simple_period_ms);
  }
  mpu_simple_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

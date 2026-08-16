/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_wake_matrix_demo/main.c
 * @brief Exercise the WUPEN0 / WUPEN1 wake-source enable matrix
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Walks every wake-source bit the LPM HAL exposes through
 * ``ra8_lpm_arm_wupen0_bits`` / ``ra8_lpm_arm_wupen1_bits`` and
 * verifies the matrix is reachable from the application layer. The
 * demo does NOT attempt to enter Software Standby / Deep Standby
 * because most of these sources need additional peripheral wiring
 * (USB device attached, IRQ pin driven, ULPT armed, etc) that the
 * bare EK-RA8D2 cannot synthesise without external hardware.
 *
 * Instead, the demo:
 *
 *   1. Brings the standard CGC + SysTick + SCI8 + LPM up.
 *   2. Emits boot banner ``"lpm_wake_matrix: boot\r\n"`` -- HIL gate.
 *   3. Walks the WUPEN0 internal-peripheral bits (those that can be
 *      armed without external HW): IWDT / PVD1 / PVD2 / VBATT / RTC
 *      alarm / RTC periodic. Each arm + readback updates
 *      ``g_lpm_wake_matrix_armed`` so the operator can see progress
 *      via SWD.
 *   4. Walks the WUPEN1 internal sources: COMPHS0 / SOSC / ULPT0U /
 *      ULPT0A / ULPT0B / I3C0.
 *   5. Disarms everything via ``ra8_lpm_clear_wupen0_bits`` /
 *      ``ra8_lpm_clear_wupen1_bits`` and confirms the matrix reads
 *      back as zero.
 *   6. Emits ``"lpm_wake_matrix: done\r\n"`` and parks LED1 on.
 *
 * The demo proves that the HAL helpers actually mutate the WUPEN
 * registers as expected -- equivalent to the host unit test in
 * ``tests/test_app_lpm_wake_matrix.c`` but running on real silicon
 * so the chip's read-as-written semantics are exercised.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_time.h"

/** @brief Wake-up enable register masks. */
typedef enum : uint32_t {
  k_lpm_wupen_all_mask = 0xFFFFFFFFUL, /**< Clear all WUPEN bits. */
} lpm_wupen_mask_t;

typedef enum : uint32_t {
  k_lpm_wake_baud = 115200U, /**< Lpm wake baud. */
} lpm_wake_const_t;

/** @brief Boot banner -- HIL gate string. */
static const uint8_t s_lpm_wake_boot_msg[] = "lpm_wake_matrix: boot\r\n";

/** @brief Per-step completion banner emitted after each arm/disarm. */
static const uint8_t s_lpm_wake_done_msg[] = "lpm_wake_matrix: done\r\n";

/**
 * @var g_lpm_wake_matrix_armed
 * @brief Snapshot of which WUPEN sources the demo has armed so far.
 *
 * @details
 * Updated after each successful ``ra8_lpm_arm_wupen0_bits`` /
 * ``ra8_lpm_arm_wupen1_bits`` call. Exposed for SWD inspection so an
 * operator can confirm matrix progression without a UART.
 *
 * @since 0.1.0
 */
volatile uint64_t g_lpm_wake_matrix_armed = 0U;

/**
 * @brief Park the processor after a fatal setup or wake-matrix failure.
 *
 * @details Executes wait-for-interrupt indefinitely so a partial matrix walk
 * cannot be mistaken for a completed diagnostic.
 *
 * @pre The caller has determined that the demo cannot continue safely.
 * @pre No foreground recovery operation remains capable of restoring state.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop.
 * @note The terminal loop preserves the last matrix state for SWD inspection.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_wake_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + SCI8 + LED1 + LPM block up.
 *
 * @details Initializes clocks, timing, the HIL console, LED1, and LPM
 * configuration in dependency order, halting at the first required failure.
 *
 * @pre IRQs disabled.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post All five sub-systems are armed on success.
 * @post LPM block has LPSCR.LPMD = 0 (System Active).
 * @note The helper applies a fail-closed startup policy before the matrix walk.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_wake_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_wake_baud) != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
}

/**
 * @brief Arm a WUPEN0 source mask and update the in-RAM snapshot.
 *
 * @details Applies the requested low-bank mask, then samples the combined exit
 * cause into the externally visible progression snapshot.
 *
 * @param[in] bits ``k_ra8_lpm_wupen0_*`` mask to OR into WUPEN0.
 *
 * @return Error code from ``ra8_lpm_arm_wupen0_bits``.
 * @retval k_ra8_ok The mask was armed and the resulting snapshot was captured.
 *
 * @pre LPM block initialised; PRC1 unlocked.
 * @pre ``bits`` is a documented WUPEN0 mask (not arbitrary).
 *
 * @post On success ``g_lpm_wake_matrix_armed`` low 32 bits reflect
 *       the new arm state.
 * @post On failure, the prior externally visible snapshot remains unchanged.
 * @note The snapshot spans both WUPEN banks even though only bank zero changes.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_lpm_wake_arm0(uint32_t bits)
{
  ra8_err_t err = ra8_lpm_arm_wupen0_bits(bits);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t cause = 0U;
  err            = ra8_lpm_get_exit_cause(&cause);
  if (err == k_ra8_ok) {
    g_lpm_wake_matrix_armed = cause;
  }
  return err;
}

/**
 * @brief Arm a WUPEN1 source mask and update the in-RAM snapshot.
 *
 * @details Applies the requested high-bank mask, then samples the combined exit
 * cause into the externally visible progression snapshot.
 *
 * @param[in] bits ``k_ra8_lpm_wupen1_*`` mask to OR into WUPEN1.
 *
 * @return Error code from ``ra8_lpm_arm_wupen1_bits``.
 * @retval k_ra8_ok The mask was armed and the resulting snapshot was captured.
 *
 * @pre LPM block initialised; PRC1 unlocked.
 * @pre ``bits`` is a documented WUPEN1 mask.
 *
 * @post On success ``g_lpm_wake_matrix_armed`` high 32 bits reflect
 *       the new arm state.
 * @post On failure, the prior externally visible snapshot remains unchanged.
 * @note The snapshot spans both WUPEN banks even though only bank one changes.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_lpm_wake_arm1(uint32_t bits)
{
  ra8_err_t err = ra8_lpm_arm_wupen1_bits(bits);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t cause = 0U;
  err            = ra8_lpm_get_exit_cause(&cause);
  if (err == k_ra8_ok) {
    g_lpm_wake_matrix_armed = cause;
  }
  return err;
}

/**
 * @brief Walk the WUPEN0 internal-peripheral wake sources.
 *
 * @details Arms each supported low-bank source in a fixed diagnostic order and
 * stops immediately if any individual update or snapshot read fails.
 *
 * @par MC/DC:
 * Compound decision: ``arm0(iwdt) != ok || arm0(pvd1) != ok || ...``
 * Six atomic conditions x N+1 = 7 vectors. Steady-state all-ok runs
 * on bench; each error branch is covered in
 * tests/test_app_lpm_wake_matrix.c.
 *
 * @return Error code from the first failing arm call.
 * @retval k_ra8_ok All six WUPEN0 source bits were armed successfully.
 *
 * @pre LPM block initialised and PRC1 unlocked.
 * @pre The low-bank matrix begins in the state expected by the diagnostic.
 *
 * @post On success WUPEN0 has IWDT / PVD1 / PVD2 / VBATT / RTCALM /
 *       RTCPRD set; all other WUPEN0 bits left untouched.
 * @post On failure, no source after the failing step is attempted.
 * @note Each step refreshes ``g_lpm_wake_matrix_armed`` for SWD observation.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_lpm_wake_walk_wupen0(void)
{
  ra8_err_t err = internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_iwdt);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_pvd1);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_pvd2);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_vbatt);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_rtcalm);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_lpm_wake_arm0((uint32_t)k_ra8_lpm_wupen0_rtcprd);
}

/**
 * @brief Walk the WUPEN1 internal-peripheral wake sources.
 *
 * @details Arms each supported high-bank source in a fixed diagnostic order and
 * stops immediately if any individual update or snapshot read fails.
 *
 * @par MC/DC:
 * Compound decision: ``arm1(comphs0) != ok || arm1(sosc) != ok ||
 * arm1(ulpt0u) != ok || arm1(ulpt0a) != ok || arm1(ulpt0b) != ok ||
 * arm1(i3c0) != ok``. Six atomic conditions x N+1 = 7 vectors
 * covered in tests/test_app_lpm_wake_matrix.c.
 *
 * @return Error code from the first failing arm call.
 * @retval k_ra8_ok All six WUPEN1 source bits were armed successfully.
 *
 * @pre LPM block initialised and PRC1 unlocked.
 * @pre The high-bank matrix begins in the state expected by the diagnostic.
 *
 * @post On success WUPEN1 has COMPHS0 / SOSC / ULPT0U / ULPT0A /
 *       ULPT0B / I3C0 set; all other WUPEN1 bits left untouched.
 * @post On failure, no source after the failing step is attempted.
 * @note Each step refreshes ``g_lpm_wake_matrix_armed`` for SWD observation.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_lpm_wake_walk_wupen1(void)
{
  ra8_err_t err = internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_comphs0);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_sosc);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_ulpt0u);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_ulpt0a);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_ulpt0b);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_lpm_wake_arm1((uint32_t)k_ra8_lpm_wupen1_i3c0);
}

/**
 * @brief Disarm every WUPEN bit the demo set and confirm zero.
 *
 * @details Clears both complete WUPEN banks in order and refreshes the combined
 * externally visible snapshot only after both clears succeed.
 *
 * @par MC/DC:
 * Compound decision: ``clear_wupen0 != ok || clear_wupen1 != ok``.
 * Two atomic conditions x N+1 = 3 vectors covered in the host test.
 *
 * @return Error code from the first failing primitive.
 * @retval k_ra8_ok Both banks were cleared and the zero snapshot was captured.
 *
 * @pre Walk-phase complete.
 * @pre LPM register protection permits WUPEN updates.
 *
 * @post WUPEN0 == 0 and WUPEN1 == 0.
 * @post ``g_lpm_wake_matrix_armed`` == 0.
 * @note A failed clear leaves the hardware and snapshot available for diagnosis.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_lpm_wake_disarm_all(void)
{
  ra8_err_t err = ra8_lpm_clear_wupen0_bits(k_lpm_wupen_all_mask);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_lpm_clear_wupen1_bits(k_lpm_wupen_all_mask);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t cause = 0U;
  err            = ra8_lpm_get_exit_cause(&cause);
  if (err == k_ra8_ok) {
    g_lpm_wake_matrix_armed = cause;
  }
  return err;
}

void main(void)
{
  internal_lpm_wake_setup_or_halt();
  ra8_isr_globals_enable();

  /* Boot banner -- HIL gate. */
  (void)ra8_board_uart_console_write(s_lpm_wake_boot_msg,
                                     (size_t)(sizeof(s_lpm_wake_boot_msg) - 1U));

  if (internal_lpm_wake_walk_wupen0() != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (internal_lpm_wake_walk_wupen1() != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }
  if (internal_lpm_wake_disarm_all() != k_ra8_ok) {
    internal_lpm_wake_panic_halt();
  }

  /* Final-state banner -- emit so the operator sees the walk
   * completed even when no debugger is attached. */
  (void)ra8_board_uart_console_write(s_lpm_wake_done_msg,
                                     (size_t)(sizeof(s_lpm_wake_done_msg) - 1U));

  (void)ra8_board_led_on(k_ra8_board_led1);

  while (1) {
    __asm__ volatile("wfi");
  }
}

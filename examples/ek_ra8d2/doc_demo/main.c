/**
 * @file examples/ek_ra8d2/doc_demo/main.c
 * @brief Data Operation Circuit (DOC) hardware vs software check
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The on-chip Data Operation Circuit accepts 16-bit add / subtract /
 * compare operations and stores the result in DODSR (HUM Ch "DOC").
 * This app sums an 8-entry constant table both via the hardware
 * `ra_doc_add16` API (chained accumulator) and via a portable
 * software reference, then compares the two checksums every second.
 * LED1 toggles on a match; LED2 latches if the two ever diverge.
 *
 * The HAL only exposes 16-bit add and sub (no XOR), so we exercise
 * the add path -- it is sufficient to prove DOC bring-up + register
 * access + result readback. Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_doc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_doc_demo_period_ms = 1000U,
} doc_demo_const_t;

/** @brief Operand-table layout. */
typedef enum : uint8_t {
  k_doc_demo_table_len = 8U,
} doc_demo_layout_t;

/** @brief Eight 16-bit operands chained through DOC.add. */
static const uint16_t k_doc_demo_operands[k_doc_demo_table_len] = {
  0x1111U,
  0x2222U,
  0x3333U,
  0x0F0FU,
  0xF0F0U,
  0x00FFU,
  0xFF00U,
  0xDEADU,
};

/** @brief Park the CPU forever after fatal init failure. */
static void doc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Software reference: sum every operand modulo 2^16.
 *
 * @par MC/DC:
 * Loop bound is statically known; no compound boolean decisions in
 * this helper. (MC/DC vector pattern: trivial; covered by any call.)
 *
 * @return Wrap-around sum.
 *
 * @pre None.
 * @post Return value is reproducible for the fixed operand table.
 *
 * @since 0.1.0
 */
static uint16_t doc_demo_sw_sum(void)
{
  uint16_t acc = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_doc_demo_table_len; ++i) {
    acc = (uint16_t)(acc + k_doc_demo_operands[i]);
  }
  return acc;
}

/**
 * @brief Hardware DOC sum: chain seven add16 calls into the accumulator.
 *
 * @par MC/DC:
 * Compound decision: ``ra_doc_add16 != ok``. One atomic condition x
 * 2 vectors; covered by test_app_doc_demo.c (success path + bad
 * pointer rejection).
 *
 * @param[out] out_sum Receives the chained DOC sum.
 *
 * @pre out_sum non-NULL.
 * @post On success *out_sum == doc_demo_sw_sum().
 *
 * @return Error code from the first failing ra_doc_add16.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t doc_demo_hw_sum(uint16_t* out_sum)
{
  uint16_t acc = k_doc_demo_operands[0];
  for (uint8_t i = 1U; i < (uint8_t)k_doc_demo_table_len; ++i) {
    uint16_t       partial = 0U;
    const ra_err_t err     = ra_doc_add16(acc, k_doc_demo_operands[i], &partial);
    if (err != k_ra_ok) {
      return err;
    }
    acc = partial;
  }
  *out_sum = acc;
  return k_ra_ok;
}

/** @brief Bring CGC + SysTick + LEDs + DOC up. Halts on any error. */
static void doc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    doc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    doc_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    doc_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    doc_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    doc_demo_panic_halt();
  }
  if (ra_doc_init() != k_ra_ok) {
    doc_demo_panic_halt();
  }
}

/**
 * @brief One iteration: HW sum + SW sum + compare.
 *
 * @par MC/DC:
 * Compound decision: ``hw_err != ok || hw_sum != sw_sum``. Two
 * atomic conditions x N+1 = 3 vectors -- match path (steady state),
 * hw_err branch (driver-failure mock), mismatch branch (mock seeds
 * a wrong DODSR value).
 *
 * @param[out] out_match Receives 1 if hw == sw, 0 otherwise.
 * @return ra_err_t from doc_demo_hw_sum.
 *
 * @pre out_match non-NULL.
 * @post On success *out_match is 0 or 1.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t doc_demo_one_iter(uint8_t* out_match)
{
  uint16_t       hw  = 0U;
  const ra_err_t err = doc_demo_hw_sum(&hw);
  if (err != k_ra_ok) {
    return err;
  }
  const uint16_t sw = doc_demo_sw_sum();
  *out_match        = (hw == sw) ? 1U : 0U;
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  doc_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    uint8_t        match = 0U;
    const ra_err_t err   = doc_demo_one_iter(&match);
    if (err != k_ra_ok) {
      break;
    }
    if (match != 0U) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ra_delay_ms((uint32_t)k_doc_demo_period_ms);
  }
  doc_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop

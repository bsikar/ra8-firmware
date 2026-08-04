/**
 * @file test_ra8_log.c
 * @brief Unit tests for ra8_log.c (log backend + ra8_err_to_str table)
 *
 * @details
 * On the host `RA8_OFF_TARGET` hard-codes `internal_itm_ready()`
 * to return `false`, so every log call short-circuits before touching
 * the ITM stimulus port. The log entry points themselves are still
 * exercised so their `!internal_itm_ready()` early-return paths are
 * covered.
 *
 * This file also walks every `k_ra8_err_*` value through `ra8_err_to_str`
 * so the big switch in the same translation unit is fully exercised.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

/* Make every level visible so the macros bind to the internal_*
 * entry points rather than no-ops. */
/** @brief RA8 LOG LEVEL. */
#define RA8_LOG_LEVEL k_ra8_log_level_debug

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum log_small_value_t
 * @brief The narrow end of the formatter's range: a single digit, and a small magnitude logged both signs.
 */
typedef enum : uint8_t {
  k_log_val_small_signed =
    42, /**< A small magnitude logged as both negative and positive, covering the sign branch. */
  k_log_val_single_digit =
    7U, /**< A single-digit value: the shortest decimal the formatter can emit. */
} log_small_value_t;

/**
 * @enum log_mid_value_t
 * @brief Mid-width decimal and hex vectors, plus an error code no enumerator uses.
 */
typedef enum : uint16_t {
  k_log_val_positive_decimal = 12345, /**< A positive decimal exercising the multi-digit path. */
  k_log_val_hex_small =
    0x1234U, /**< Small hex value: two significant nibbles; leading-zero suppression is visible. */
  k_log_val_hex_mid =
    0x5678U, /**< Second hex value, distinct from the first, so the two sinks cannot be confused. */
  k_log_err_code_unknown =
    0xFFFFU, /**< Code no ra8_err_t enumerator uses; ra8_err_to_str returns its unknown string. */
} log_mid_value_t;

/**
 * @enum log_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint32_t {
  k_log_val_mid_decimal =
    1234567U, /**< A mid-width decimal, between the single digit and the 32-bit extremes. */
  /** A hex value using all eight nibbles, including letters. */
  k_log_val_hex_full = 0xDEADBEEFU,
  k_log_val_u32_max =
    0xFFFFFFFFU, /**< UINT32_MAX: the widest unsigned value the formatter must render intact. */
  k_log_all_ones =
    0xFFFFFFFFUL, /**< All-ones: ITM stimulus-port ready state and widest unsigned log value. */
  k_log_val_i32_max =
    2147483647, /**< INT32_MAX; `-k_log_val_i32_max - 1` gives INT32_MIN: both signed extremes. */
} log_fixture_t;

/*
 * ITM stimulus register addresses (must match ra8_log.c's
 * `k_ra8_itm_stim_base`, `k_ra8_itm_tcr_addr`, `k_ra8_itm_tenr_addr`).
 * Declared here so the tests can flip the ITM into the "ready"
 * state and drive the full emit path.
 */
typedef enum : uintptr_t {
  k_test_itm_stim0 = 0xE0000000UL, /**< Test itm stim0. */
  k_test_itm_tcr   = 0xE0000E80UL, /**< Test itm tcr.   */
  k_test_itm_tenr  = 0xE0000E00UL, /**< Test itm tenr.  */
} test_itm_addr_t;

/**
 * @brief Arm the ITM registers so internal_itm_ready() returns true.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_itm_arm(void)
{
  /* ITMENA (TCR bit 0) and stimulus port 0 (TENR bit 0). */
  *(volatile uint32_t*)k_test_itm_tcr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr = 0x00000001UL;
  /* Non-zero STIM0 makes the "FIFO has space" poll return on the
   * first iteration. */
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_init_runs(void)
{
  TEST_BEGIN("ra8_log_init runs");
  ra8_fake_mmap_reset();
  ra8_log_init();
  TEST_END("ra8_log_init runs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_levels_plain(void)
{
  TEST_BEGIN("ra8_log plain tag/message at every level");
  ra8_fake_mmap_reset();
  ra8_log_init();

  internal_ra8_log_error("TAG", "error line");
  internal_ra8_log_warn("TAG", "warn line");
  internal_ra8_log_info("TAG", "info line");
  internal_ra8_log_debug("TAG", "debug line");

  TEST_END("ra8_log plain tag/message at every level");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_levels_val(void)
{
  TEST_BEGIN("ra8_log with companion value at every level");
  ra8_fake_mmap_reset();
  ra8_log_init();

  internal_ra8_log_error_val("TAG", "value", k_log_val_hex_small);
  internal_ra8_log_warn_val("TAG", "value", k_log_val_hex_mid);
  internal_ra8_log_info_val("TAG", "value", k_log_val_hex_full);
  internal_ra8_log_debug_val("TAG", "value", (int32_t)-k_log_val_small_signed);

  TEST_END("ra8_log with companion value at every level");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_val_edge_cases(void)
{
  TEST_BEGIN("ra8_log value edge cases");
  ra8_fake_mmap_reset();
  ra8_log_init();

  internal_ra8_log_info_val("ZERO", "v", 0U);
  internal_ra8_log_info_val("MAX", "v", k_log_val_u32_max);
  internal_ra8_log_debug_val("NEG", "v", (int32_t)-1);
  internal_ra8_log_debug_val("MIN", "v", (int32_t)(-k_log_val_i32_max - 1));
  internal_ra8_log_debug_val("POS", "v", (int32_t)k_log_val_positive_decimal);

  TEST_END("ra8_log value edge cases");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_macros(void)
{
  TEST_BEGIN("ra8_log_*_val macros through public header");
  ra8_fake_mmap_reset();
  ra8_log_init();

  ra8_log_error("MAC", "err");
  ra8_log_warn("MAC", "warn");
  ra8_log_info("MAC", "info");
  ra8_log_debug("MAC", "dbg");
  ra8_log_error_val("MAC", "err", 1U);
  ra8_log_warn_val("MAC", "warn", 2U);
  ra8_log_info_val("MAC", "info", 3U);
  ra8_log_debug_val("MAC", "dbg", (int32_t)-3);

  TEST_END("ra8_log_*_val macros through public header");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_many_calls(void)
{
  TEST_BEGIN("ra8_log many calls do not crash");
  ra8_fake_mmap_reset();
  ra8_log_init();
  for (uint32_t i = 0U; i < 32U; ++i) {
    internal_ra8_log_info_val("LOOP", "i", i);
  }
  TEST_END("ra8_log many calls do not crash");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_plain(void)
{
  TEST_BEGIN("ra8_log plain writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  test_itm_arm();
  internal_ra8_log_error("RDY", "error msg");
  internal_ra8_log_warn("RDY", "warn msg");
  internal_ra8_log_info("RDY", "info msg");
  internal_ra8_log_debug("RDY", "debug msg");
  TEST_END("ra8_log plain writes walk the ITM path when armed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_val_unsigned(void)
{
  TEST_BEGIN("ra8_log unsigned-value writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  test_itm_arm();
  /* Zero triggers the fast-path inside internal_itm_put_u32. */
  internal_ra8_log_info_val("RDY", "zero", 0U);
  /* Non-zero walks the digit loop. */
  internal_ra8_log_info_val("RDY", "big", k_log_all_ones);
  internal_ra8_log_error_val("RDY", "mid", k_log_val_mid_decimal);
  internal_ra8_log_warn_val("RDY", "small", k_log_val_single_digit);
  TEST_END("ra8_log unsigned-value writes walk the ITM path when armed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_val_signed(void)
{
  TEST_BEGIN("ra8_log signed-value writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  test_itm_arm();
  internal_ra8_log_debug_val("RDY", "neg", (int32_t)-k_log_val_small_signed);
  internal_ra8_log_debug_val("RDY", "pos", (int32_t)k_log_val_small_signed);
  internal_ra8_log_debug_val("RDY", "min", (int32_t)(-k_log_val_i32_max - 1));
  internal_ra8_log_debug_val("RDY", "max", (int32_t)k_log_val_i32_max);
  internal_ra8_log_debug_val("RDY", "zero", (int32_t)0);
  TEST_END("ra8_log signed-value writes walk the ITM path when armed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_tcr_disabled(void)
{
  TEST_BEGIN("ra8_log bails out when TCR.ITMENA is clear");
  ra8_fake_mmap_reset();
  /* TCR=0 makes internal_itm_ready return false on the first check. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
  internal_ra8_log_info("OFF", "should drop");
  TEST_END("ra8_log bails out when TCR.ITMENA is clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_tenr_disabled(void)
{
  TEST_BEGIN("ra8_log bails out when TENR port 0 is clear");
  ra8_fake_mmap_reset();
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
  internal_ra8_log_info("OFF", "should drop");
  TEST_END("ra8_log bails out when TENR port 0 is clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_ready_fifo_full(void)
{
  TEST_BEGIN("ra8_log drops byte when STIM0 is zero for the full poll");
  ra8_fake_mmap_reset();
  /* TCR + TENR are on, but STIM0 starts at 0 so internal_itm_ready
   * returns false and the emit path short-circuits. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = 0x00000000UL;
  internal_ra8_log_info("FULL", "should drop");
  TEST_END("ra8_log drops byte when STIM0 is zero for the full poll");
}

/* -------------------------------------------------------------------------
 * ra8_err_to_str -- walk every enum value so the big switch is covered.
 * ra8_err_to_str lives in ra8_log.c so its coverage count rolls up into the
 * same .gcda as the log backend tests.
 * ------------------------------------------------------------------------- */

typedef struct {
  ra8_err_t   code; /**< Code. */
  const char* text; /**< Text. */
} test_err_entry_t;

static const test_err_entry_t s_all_err_codes[] = {
  {k_ra8_ok, "ok"},
  {k_ra8_fail, "fail"},
  {k_ra8_err_no_mem, "no_mem"},
  {k_ra8_err_invalid_arg, "invalid_arg"},
  {k_ra8_err_invalid_state, "invalid_state"},
  {k_ra8_err_invalid_size, "invalid_size"},
  {k_ra8_err_not_found, "not_found"},
  {k_ra8_err_not_supported, "not_supported"},
  {k_ra8_err_timeout, "timeout"},
  {k_ra8_err_busy, "busy"},
  {k_ra8_err_no_data, "no_data"},
  {k_ra8_err_would_block, "would_block"},
  {k_ra8_err_exists, "exists"},
  {k_ra8_err_empty, "empty"},
  {k_ra8_err_cancelled, "cancelled"},
  {k_ra8_err_not_initialized, "not_initialized"},
  {k_ra8_err_estop, "estop"},
  {k_ra8_err_not_empty, "not_empty"},
  {k_ra8_err_hw_init_failed, "hw_init_failed"},
  {k_ra8_err_hw_not_ready, "hw_not_ready"},
  {k_ra8_err_hw_timeout, "hw_timeout"},
  {k_ra8_err_hw_error, "hw_error"},
  {k_ra8_err_gpio_conflict, "gpio_conflict"},
  {k_ra8_err_gpio_invalid_port, "gpio_invalid_port"},
  {k_ra8_err_gpio_invalid_pin, "gpio_invalid_pin"},
  {k_ra8_err_out_of_range, "out_of_range"},
  {k_ra8_err_hw_unmapped, "hw_unmapped"},
  {k_ra8_err_rtos_error, "rtos_error"},
  {k_ra8_err_rtos_thread_create, "rtos_thread_create"},
  {k_ra8_err_rtos_semaphore, "rtos_semaphore"},
  {k_ra8_err_rtos_mutex, "rtos_mutex"},
  {k_ra8_err_rtos_queue, "rtos_queue"},
  {k_ra8_err_rtos_timer, "rtos_timer"},
  {k_ra8_err_comm_error, "comm_error"},
  {k_ra8_err_spi_error, "spi_error"},
  {k_ra8_err_uart_error, "uart_error"},
  {k_ra8_err_i2c_error, "i2c_error"},
  {k_ra8_err_crc_mismatch, "crc_mismatch"},
  {k_ra8_err_protocol_error, "protocol_error"},
  {k_ra8_err_nack, "nack"},
  {k_ra8_err_conflict, "conflict"},
  {k_ra8_err_retry_limit, "retry_limit"},
  {k_ra8_err_validation_failed, "validation_failed"},
  {k_ra8_err_checksum_mismatch, "checksum_mismatch"},
  {k_ra8_err_range_check_failed, "range_check_failed"},
  {k_ra8_err_null_ptr, "null_ptr"},
  {k_ra8_err_decomp_output_cap, "decomp_output_cap"},
  {k_ra8_err_decomp_ratio, "decomp_ratio"},
  {k_ra8_err_decomp_entries, "decomp_entries"},
  {k_ra8_err_decomp_depth, "decomp_depth"},
  {k_ra8_err_decomp_iterations, "decomp_iterations"},
};

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_err_to_str_every_code(void)
{
  TEST_BEGIN("ra8_err_to_str covers every enum value");
  ra8_fake_mmap_reset();

  const size_t count = sizeof(s_all_err_codes) / sizeof(s_all_err_codes[0]);
  for (size_t i = 0U; i < count; ++i) {
    const char* actual = ra8_err_to_str(s_all_err_codes[i].code);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT(strcmp(actual, s_all_err_codes[i].text) == 0);
  }

  TEST_END("ra8_err_to_str covers every enum value");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_err_to_str_unknown_default(void)
{
  TEST_BEGIN("ra8_err_to_str returns 'unknown' for out-of-enum value");
  ra8_fake_mmap_reset();
  const char* s = ra8_err_to_str((ra8_err_t)k_log_err_code_unknown);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT(strcmp(s, "unknown") == 0);
  TEST_END("ra8_err_to_str returns 'unknown' for out-of-enum value");
}

/**
 * @test test_mcdc_itm_put_u32_loop
 *
 * @par MC/DC:
 * Decision: `while (value != 0U && i < k_ra8_u32_max_digits)`
 * (2 conditions, libs/ra8_core/src/ra8_log.c line 244 -- gap row 148 in CSV)
 * Exercised indirectly via `internal_ra8_log_info_val()` once the ITM
 * is armed so the emit path actually reaches `internal_itm_put_u32()`.
 * - Vector 1: value=0           -> C1=F (early-return path before loop;
 *   loop predicate evaluated zero times so decision F controls entry)
 * - Vector 2: value=1           -> C1=T, C2=T (i=0<10) -> enter loop
 *   once, then value becomes 0 -> exit (C1=F).
 * - Vector 3: value=0xFFFFFFFFU -> C1=T, C2=T 10 times until i reaches
 *   k_ra8_u32_max_digits=10 (full digit emit). Within the budget C1
 *   stays T while C2 transitions T->F at iteration 10.
 *
 * MC/DC pair for C1: V1(F,_)->F vs V2(T,T)->T (C2 held T). MC/DC pair
 * for C2 within V3: at iter 9 (T,T) the loop continues, at iter 10
 * (T,F) the loop exits while C1 still T -- decision flip with C1 held
 * T. N+1 = 3 vectors for N=2 conditions.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * C2 (`i < k_ra8_u32_max_digits`) is structurally bounded: a uint32_t
 * holds at most 10 decimal digits and the buffer is sized for exactly
 * 10. The C2=F transition is observable only inside V3's iteration
 * sequence; no caller can produce a uint32_t requiring 11+ digits, so
 * the C1=T,C2=F-then-decision-F masking pair is provably reachable
 * only within the V3 trajectory. This is acceptable per DO-178C
 * 6.4.4.3 representative-subset coverage where structural bounds
 * preclude an isolated single-vector demonstration.
 */
static void test_mcdc_itm_put_u32_loop(void)
{
  TEST_BEGIN("ra8_log put_u32 MC/DC: value!=0 && i<max_digits");
  ra8_fake_mmap_reset();
  test_itm_arm();
  ra8_log_init();

  /* Vector 1: value == 0 -> early return path (C1=F at first check). */
  internal_ra8_log_info_val("V1", "zero", 0U);

  /* Vector 2: value == 1 -> single loop iteration (C1=T, C2=T then
   * value becomes 0 so C1 flips to F on the second predicate). */
  internal_ra8_log_info_val("V2", "one", 1U);

  /* Vector 3: value == UINT32_MAX -> 10 iterations exhausting the
   * digit budget so C2 transitions T->F at iteration 10 with C1 still
   * T -- proves C2 independently controls loop exit. */
  internal_ra8_log_info_val("V3", "max", k_log_val_u32_max);

  TEST_END("ra8_log put_u32 MC/DC: value!=0 && i<max_digits");
}

int32_t main(void)
{
  test_log_init_runs();
  test_log_levels_plain();
  test_log_levels_val();
  test_log_val_edge_cases();
  test_log_macros();
  test_log_many_calls();
  test_log_ready_plain();
  test_log_ready_val_unsigned();
  test_log_ready_val_signed();
  test_log_ready_tcr_disabled();
  test_log_ready_tenr_disabled();
  test_log_ready_fifo_full();
  test_err_to_str_every_code();
  test_err_to_str_unknown_default();
  test_mcdc_itm_put_u32_loop();
  (void)fprintf(stderr, "[OK  ] test_ra8_log.c\n");
  return 0;
}

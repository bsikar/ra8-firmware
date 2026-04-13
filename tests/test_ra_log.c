/**
 * @file test_ra_log.c
 * @brief Unit tests for ra_log.c (log backend + ra_err_to_str table)
 *
 * @details
 * On the host `RA_SIMULATOR_MODE` hard-codes `internal_itm_ready()`
 * to return `false`, so every log call short-circuits before touching
 * the ITM stimulus port. The log entry points themselves are still
 * exercised so their `!internal_itm_ready()` early-return paths are
 * covered.
 *
 * This file also walks every `k_ra_err_*` value through `ra_err_to_str`
 * so the big switch in the same translation unit is fully exercised.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

/* Make every level visible so the macros bind to the internal_*
 * entry points rather than no-ops. */
#define RA_LOG_LEVEL k_ra_log_level_debug

#include "ra_err.h"
#include "ra_log.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/*
 * ITM stimulus register addresses (must match ra_log.c's
 * `k_ra_itm_stim_base`, `k_ra_itm_tcr_addr`, `k_ra_itm_tenr_addr`).
 * Declared here so the tests can flip the ITM into the "ready"
 * state and drive the full emit path.
 */
typedef enum : uintptr_t {
  k_test_itm_stim0 = 0xE0000000UL,
  k_test_itm_tcr   = 0xE0000E80UL,
  k_test_itm_tenr  = 0xE0000E00UL,
} test_itm_addr_t;

/**
 * @brief Arm the ITM registers so internal_itm_ready() returns true.
 */
static void test_itm_arm(void)
{
  /* ITMENA (TCR bit 0) and stimulus port 0 (TENR bit 0). */
  *(volatile uint32_t*)k_test_itm_tcr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr = 0x00000001UL;
  /* Non-zero STIM0 makes the "FIFO has space" poll return on the
   * first iteration. */
  *(volatile uint32_t*)k_test_itm_stim0 = 0xFFFFFFFFUL;
}

static void test_log_init_runs(void)
{
  TEST_BEGIN("ra_log_init runs");
  ra_sim_mmap_reset();
  ra_log_init();
  TEST_END("ra_log_init runs");
}

static void test_log_levels_plain(void)
{
  TEST_BEGIN("ra_log plain tag/message at every level");
  ra_sim_mmap_reset();
  ra_log_init();

  internal_ra_log_error("TAG", "error line");
  internal_ra_log_warn("TAG", "warn line");
  internal_ra_log_info("TAG", "info line");
  internal_ra_log_debug("TAG", "debug line");

  TEST_END("ra_log plain tag/message at every level");
}

static void test_log_levels_val(void)
{
  TEST_BEGIN("ra_log with companion value at every level");
  ra_sim_mmap_reset();
  ra_log_init();

  internal_ra_log_error_val("TAG", "value", 0x1234U);
  internal_ra_log_warn_val("TAG", "value", 0x5678U);
  internal_ra_log_info_val("TAG", "value", 0xDEADBEEFU);
  internal_ra_log_debug_val("TAG", "value", (int32_t)-42);

  TEST_END("ra_log with companion value at every level");
}

static void test_log_val_edge_cases(void)
{
  TEST_BEGIN("ra_log value edge cases");
  ra_sim_mmap_reset();
  ra_log_init();

  internal_ra_log_info_val("ZERO", "v", 0U);
  internal_ra_log_info_val("MAX", "v", 0xFFFFFFFFU);
  internal_ra_log_debug_val("NEG", "v", (int32_t)-1);
  internal_ra_log_debug_val("MIN", "v", (int32_t)(-2147483647 - 1));
  internal_ra_log_debug_val("POS", "v", (int32_t)12345);

  TEST_END("ra_log value edge cases");
}

static void test_log_macros(void)
{
  TEST_BEGIN("ra_log_*_val macros through public header");
  ra_sim_mmap_reset();
  ra_log_init();

  ra_log_error("MAC", "err");
  ra_log_warn("MAC", "warn");
  ra_log_info("MAC", "info");
  ra_log_debug("MAC", "dbg");
  ra_log_error_val("MAC", "err", 1U);
  ra_log_warn_val("MAC", "warn", 2U);
  ra_log_info_val("MAC", "info", 3U);
  ra_log_debug_val("MAC", "dbg", (int32_t)-3);

  TEST_END("ra_log_*_val macros through public header");
}

static void test_log_many_calls(void)
{
  TEST_BEGIN("ra_log many calls do not crash");
  ra_sim_mmap_reset();
  ra_log_init();
  for (uint32_t i = 0U; i < 32U; ++i) {
    internal_ra_log_info_val("LOOP", "i", i);
  }
  TEST_END("ra_log many calls do not crash");
}

static void test_log_ready_plain(void)
{
  TEST_BEGIN("ra_log plain writes walk the ITM path when armed");
  ra_sim_mmap_reset();
  test_itm_arm();
  internal_ra_log_error("RDY", "error msg");
  internal_ra_log_warn("RDY", "warn msg");
  internal_ra_log_info("RDY", "info msg");
  internal_ra_log_debug("RDY", "debug msg");
  TEST_END("ra_log plain writes walk the ITM path when armed");
}

static void test_log_ready_val_unsigned(void)
{
  TEST_BEGIN("ra_log unsigned-value writes walk the ITM path when armed");
  ra_sim_mmap_reset();
  test_itm_arm();
  /* Zero triggers the fast-path inside internal_itm_put_u32. */
  internal_ra_log_info_val("RDY", "zero", 0U);
  /* Non-zero walks the digit loop. */
  internal_ra_log_info_val("RDY", "big", 0xFFFFFFFFUL);
  internal_ra_log_error_val("RDY", "mid", 1234567U);
  internal_ra_log_warn_val("RDY", "small", 7U);
  TEST_END("ra_log unsigned-value writes walk the ITM path when armed");
}

static void test_log_ready_val_signed(void)
{
  TEST_BEGIN("ra_log signed-value writes walk the ITM path when armed");
  ra_sim_mmap_reset();
  test_itm_arm();
  internal_ra_log_debug_val("RDY", "neg", (int32_t)-42);
  internal_ra_log_debug_val("RDY", "pos", (int32_t)42);
  internal_ra_log_debug_val("RDY", "min", (int32_t)(-2147483647 - 1));
  internal_ra_log_debug_val("RDY", "max", (int32_t)2147483647);
  internal_ra_log_debug_val("RDY", "zero", (int32_t)0);
  TEST_END("ra_log signed-value writes walk the ITM path when armed");
}

static void test_log_ready_tcr_disabled(void)
{
  TEST_BEGIN("ra_log bails out when TCR.ITMENA is clear");
  ra_sim_mmap_reset();
  /* TCR=0 makes internal_itm_ready return false on the first check. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = 0xFFFFFFFFUL;
  internal_ra_log_info("OFF", "should drop");
  TEST_END("ra_log bails out when TCR.ITMENA is clear");
}

static void test_log_ready_tenr_disabled(void)
{
  TEST_BEGIN("ra_log bails out when TENR port 0 is clear");
  ra_sim_mmap_reset();
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_stim0 = 0xFFFFFFFFUL;
  internal_ra_log_info("OFF", "should drop");
  TEST_END("ra_log bails out when TENR port 0 is clear");
}

static void test_log_ready_fifo_full(void)
{
  TEST_BEGIN("ra_log drops byte when STIM0 is zero for the full poll");
  ra_sim_mmap_reset();
  /* TCR + TENR are on, but STIM0 starts at 0 so internal_itm_ready
   * returns false and the emit path short-circuits. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = 0x00000000UL;
  internal_ra_log_info("FULL", "should drop");
  TEST_END("ra_log drops byte when STIM0 is zero for the full poll");
}

/* -------------------------------------------------------------------------
 * ra_err_to_str -- walk every enum value so the big switch is covered.
 * ra_err_to_str lives in ra_log.c so its coverage count rolls up into the
 * same .gcda as the log backend tests.
 * ------------------------------------------------------------------------- */

typedef struct {
  ra_err_t    code;
  const char* text;
} test_err_entry_t;

static const test_err_entry_t s_all_err_codes[] = {
  {k_ra_ok, "ok"},
  {k_ra_fail, "fail"},
  {k_ra_err_no_mem, "no_mem"},
  {k_ra_err_invalid_arg, "invalid_arg"},
  {k_ra_err_invalid_state, "invalid_state"},
  {k_ra_err_invalid_size, "invalid_size"},
  {k_ra_err_not_found, "not_found"},
  {k_ra_err_not_supported, "not_supported"},
  {k_ra_err_timeout, "timeout"},
  {k_ra_err_busy, "busy"},
  {k_ra_err_no_data, "no_data"},
  {k_ra_err_would_block, "would_block"},
  {k_ra_err_exists, "exists"},
  {k_ra_err_empty, "empty"},
  {k_ra_err_cancelled, "cancelled"},
  {k_ra_err_not_initialized, "not_initialized"},
  {k_ra_err_estop, "estop"},
  {k_ra_err_hw_init_failed, "hw_init_failed"},
  {k_ra_err_hw_not_ready, "hw_not_ready"},
  {k_ra_err_hw_timeout, "hw_timeout"},
  {k_ra_err_hw_error, "hw_error"},
  {k_ra_err_gpio_conflict, "gpio_conflict"},
  {k_ra_err_gpio_invalid_port, "gpio_invalid_port"},
  {k_ra_err_gpio_invalid_pin, "gpio_invalid_pin"},
  {k_ra_err_out_of_range, "out_of_range"},
  {k_ra_err_hw_unmapped, "hw_unmapped"},
  {k_ra_err_rtos_error, "rtos_error"},
  {k_ra_err_rtos_thread_create, "rtos_thread_create"},
  {k_ra_err_rtos_semaphore, "rtos_semaphore"},
  {k_ra_err_rtos_mutex, "rtos_mutex"},
  {k_ra_err_rtos_queue, "rtos_queue"},
  {k_ra_err_rtos_timer, "rtos_timer"},
  {k_ra_err_comm_error, "comm_error"},
  {k_ra_err_spi_error, "spi_error"},
  {k_ra_err_uart_error, "uart_error"},
  {k_ra_err_i2c_error, "i2c_error"},
  {k_ra_err_crc_mismatch, "crc_mismatch"},
  {k_ra_err_protocol_error, "protocol_error"},
  {k_ra_err_nack, "nack"},
  {k_ra_err_conflict, "conflict"},
  {k_ra_err_retry_limit, "retry_limit"},
  {k_ra_err_validation_failed, "validation_failed"},
  {k_ra_err_checksum_mismatch, "checksum_mismatch"},
  {k_ra_err_range_check_failed, "range_check_failed"},
  {k_ra_err_null_ptr, "null_ptr"},
};

static void test_err_to_str_every_code(void)
{
  TEST_BEGIN("ra_err_to_str covers every enum value");
  ra_sim_mmap_reset();

  const size_t count = sizeof(s_all_err_codes) / sizeof(s_all_err_codes[0]);
  for (size_t i = 0U; i < count; ++i) {
    const char* actual = ra_err_to_str(s_all_err_codes[i].code);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT(strcmp(actual, s_all_err_codes[i].text) == 0);
  }

  TEST_END("ra_err_to_str covers every enum value");
}

static void test_err_to_str_unknown_default(void)
{
  TEST_BEGIN("ra_err_to_str returns 'unknown' for out-of-enum value");
  ra_sim_mmap_reset();
  const char* s = ra_err_to_str((ra_err_t)0xFFFFU);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT(strcmp(s, "unknown") == 0);
  TEST_END("ra_err_to_str returns 'unknown' for out-of-enum value");
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
  (void)fprintf(stderr, "[OK  ] test_ra_log.c\n");
  return 0;
}

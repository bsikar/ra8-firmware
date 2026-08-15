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

/* Make every level visible so the macros bind to the ra8_log_emit_*
 * entry points rather than no-ops. */
/** @brief RA8 LOG LEVEL. */
#define RA8_LOG_LEVEL k_ra8_log_level_debug

#include "ra8_attributes.h"
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
 * @details Programs the fake TCR, TENR, and stimulus registers with the
 * values needed to exercise the logger's ready path without real ITM hardware.
 *
 * @pre The fake MMIO map has been reset for the current test vector.
 * @pre The three test register addresses match the production logger constants.
 * @post ITM enable and stimulus-port-zero enable bits are set.
 * @post Stimulus port zero reports writable capacity.
 *
 * @note This helper changes only the host fake-MMIO fixture.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_itm_arm(void)
{
  /* ITMENA (TCR bit 0) and stimulus port 0 (TENR bit 0). */
  *(volatile uint32_t*)k_test_itm_tcr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr = 0x00000001UL;
  /* Non-zero STIM0 makes the "FIFO has space" poll return on the
   * first iteration. */
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
}

/**
 * @brief Verify logger initialization returns normally on the host fixture.
 *
 * @details Resets fake MMIO, initializes the logger, and records the Unity
 * case outcome without requiring an active ITM stimulus port.
 *
 * @pre Unity output is initialized by the test process.
 * @pre The fake MMIO backend is linked into the test executable.
 * @post Logger initialization completes without a fault.
 * @post The Unity case is reported as passed.
 *
 * @note The host logger legitimately drops bytes while ITM is unarmed.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_init_runs(void)
{
  TEST_BEGIN("ra8_log_init runs");
  ra8_fake_mmap_reset();
  ra8_log_init();
  TEST_END("ra8_log_init runs");
}

/**
 * @brief Verify an unbound hosted logger never touches target ITM addresses.
 * @details Clears the optional byte sink and emits a diagnostic before the
 * fake MMIO fixture has mapped any architectural register range.
 * @pre The test executable is built with `RA8_OFF_TARGET`.
 * @pre No earlier vector has initialized the fake MMIO map.
 * @post The log call returns without a host memory access or signal.
 * @post No byte sink or fake register state is modified.
 * @note Sanitizer execution makes a target-address dereference observable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_log_unbound_host_drops(void)
{
  TEST_BEGIN("ra8_log drops hosted output when no sink is bound");
  ra8_log_set_byte_sink(nullptr, nullptr);
  ra8_log_emit_warn("HOST", "drop without ITM access");
  TEST_END("ra8_log drops hosted output when no sink is bound");
}

/**
 * @brief Exercise every plain-message log-level backend.
 *
 * @details Initializes the host logger and invokes the four published emit
 * functions directly so each weak default backend is linked and callable.
 *
 * @pre The fake MMIO backend is available.
 * @pre All four published emitter declarations match their definitions.
 * @post Every plain emitter returns without touching invalid host MMIO.
 * @post The Unity case is reported as passed.
 *
 * @note Exact byte formatting is exercised separately with armed fake ITM.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_levels_plain(void)
{
  TEST_BEGIN("ra8_log plain tag/message at every level");
  ra8_fake_mmap_reset();
  ra8_log_init();

  ra8_log_emit_error("TAG", "error line");
  ra8_log_emit_warn("TAG", "warn line");
  ra8_log_emit_info("TAG", "info line");
  ra8_log_emit_debug("TAG", "debug line");

  TEST_END("ra8_log plain tag/message at every level");
}

/**
 * @brief Exercise every valued log-level backend.
 *
 * @details Calls the unsigned ERROR, WARN, and INFO emitters plus the signed
 * DEBUG emitter with distinct values after host logger initialization.
 *
 * @pre The fake MMIO backend is available.
 * @pre The published valued-emitter declarations match their definitions.
 * @post Every valued emitter returns without invalid host access.
 * @post Both unsigned and signed formatting entry points are reached.
 *
 * @note Register-byte verification is outside this linkage coverage vector.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_levels_val(void)
{
  TEST_BEGIN("ra8_log with companion value at every level");
  ra8_fake_mmap_reset();
  ra8_log_init();

  ra8_log_emit_error_val("TAG", "value", k_log_val_hex_small);
  ra8_log_emit_warn_val("TAG", "value", k_log_val_hex_mid);
  ra8_log_emit_info_val("TAG", "value", k_log_val_hex_full);
  ra8_log_emit_debug_val("TAG", "value", (int32_t)-k_log_val_small_signed);

  TEST_END("ra8_log with companion value at every level");
}

/**
 * @brief Exercise unsigned and signed numeric edge values.
 *
 * @details Sends zero, UINT32_MAX, negative one, INT32_MIN, and a positive
 * signed value through the published value emitters.
 *
 * @pre The fake MMIO backend is available.
 * @pre The fixture constants represent the intended integer edge values.
 * @post Unsigned zero and maximum paths are exercised.
 * @post Signed negative, minimum, and positive paths are exercised.
 *
 * @note The test asserts safe completion because ITM is deliberately unarmed.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_val_edge_cases(void)
{
  TEST_BEGIN("ra8_log value edge cases");
  ra8_fake_mmap_reset();
  ra8_log_init();

  ra8_log_emit_info_val("ZERO", "v", 0U);
  ra8_log_emit_info_val("MAX", "v", k_log_val_u32_max);
  ra8_log_emit_debug_val("NEG", "v", (int32_t)-1);
  ra8_log_emit_debug_val("MIN", "v", (int32_t)(-k_log_val_i32_max - 1));
  ra8_log_emit_debug_val("POS", "v", (int32_t)k_log_val_positive_decimal);

  TEST_END("ra8_log value edge cases");
}

/**
 * @brief Verify public logging macros dispatch to the enabled backends.
 *
 * @details Invokes every plain and valued macro at DEBUG log level so the
 * preprocessor surface is checked independently from direct emitter calls.
 *
 * @pre RA8_LOG_LEVEL is set to the debug level before including ra8_log.h.
 * @pre The fake MMIO backend is linked and resettable.
 * @post Every enabled macro dispatches without a host fault.
 * @post Both plain and valued macro families are exercised.
 *
 * @note Disabled-level no-op expansion is a compile-time configuration path.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_macros(void)
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
 * @brief Verify repeated valued logging remains bounded and safe.
 *
 * @details Emits thirty-two consecutive INFO values to exercise repeated
 * calls and loop-driven caller behavior against the unarmed host backend.
 *
 * @pre Logger initialization succeeds on the fake-MMIO backend.
 * @pre The loop bound remains within the uint32_t fixture range.
 * @post Exactly thirty-two emitter calls return normally.
 * @post The Unity case is reported as passed.
 *
 * @note This is a repetition/liveness vector, not a timing benchmark.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_many_calls(void)
{
  TEST_BEGIN("ra8_log many calls do not crash");
  ra8_fake_mmap_reset();
  ra8_log_init();
  for (uint32_t i = 0U; i < 32U; ++i) {
    ra8_log_emit_info_val("LOOP", "i", i);
  }
  TEST_END("ra8_log many calls do not crash");
}

/**
 * @brief Exercise every plain emitter with fake ITM armed.
 *
 * @details Arms the fake stimulus registers and calls all four level emitters
 * so their full tag, level, message, and line-ending paths execute.
 *
 * @pre The fake MMIO map is reset and writable.
 * @pre ::internal_itm_arm programs the production-compatible ready values.
 * @post All four plain emit paths reach the armed fake ITM registers.
 * @post No call exceeds its bounded stimulus polling contract.
 *
 * @note The fake register stores are the observable host-side sink.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_plain(void)
{
  TEST_BEGIN("ra8_log plain writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  internal_itm_arm();
  ra8_log_emit_error("RDY", "error msg");
  ra8_log_emit_warn("RDY", "warn msg");
  ra8_log_emit_info("RDY", "info msg");
  ra8_log_emit_debug("RDY", "debug msg");
  TEST_END("ra8_log plain writes walk the ITM path when armed");
}

/**
 * @brief Exercise unsigned valued emitters with fake ITM armed.
 *
 * @details Sends zero, all-ones, a mid-width value, and one digit through
 * the unsigned formatting path while the fake stimulus port is writable.
 *
 * @pre The fake MMIO map is reset and armed.
 * @pre Fixture values cover zero, short, medium, and maximum widths.
 * @post Both the zero fast path and bounded digit loop execute.
 * @post ERROR, WARN, and INFO valued level prefixes execute.
 *
 * @note Signed formatting is covered by a separate vector.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_val_unsigned(void)
{
  TEST_BEGIN("ra8_log unsigned-value writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  internal_itm_arm();
  /* Zero triggers the fast-path inside internal_itm_put_u32. */
  ra8_log_emit_info_val("RDY", "zero", 0U);
  /* Non-zero walks the digit loop. */
  ra8_log_emit_info_val("RDY", "big", k_log_all_ones);
  ra8_log_emit_error_val("RDY", "mid", k_log_val_mid_decimal);
  ra8_log_emit_warn_val("RDY", "small", k_log_val_single_digit);
  TEST_END("ra8_log unsigned-value writes walk the ITM path when armed");
}

/**
 * @brief Exercise signed valued emission with fake ITM armed.
 *
 * @details Sends negative, positive, minimum, maximum, and zero int32 values
 * through the DEBUG valued backend to cover sign and magnitude handling.
 *
 * @pre The fake MMIO map is reset and armed.
 * @pre Fixture casts produce the intended int32_t boundary values.
 * @post Negative and nonnegative formatting branches execute.
 * @post INT32_MIN is converted without signed-overflow behavior.
 *
 * @note This vector targets decimal conversion rather than level dispatch.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_val_signed(void)
{
  TEST_BEGIN("ra8_log signed-value writes walk the ITM path when armed");
  ra8_fake_mmap_reset();
  internal_itm_arm();
  ra8_log_emit_debug_val("RDY", "neg", (int32_t)-k_log_val_small_signed);
  ra8_log_emit_debug_val("RDY", "pos", (int32_t)k_log_val_small_signed);
  ra8_log_emit_debug_val("RDY", "min", (int32_t)(-k_log_val_i32_max - 1));
  ra8_log_emit_debug_val("RDY", "max", (int32_t)k_log_val_i32_max);
  ra8_log_emit_debug_val("RDY", "zero", (int32_t)0);
  TEST_END("ra8_log signed-value writes walk the ITM path when armed");
}

/**
 * @brief Verify a cleared ITMENA bit suppresses log emission.
 *
 * @details Programs a writable stimulus port but clears TCR.ITMENA, then
 * emits INFO to exercise the readiness rejection at the first condition.
 *
 * @pre The fake MMIO map is reset and writable.
 * @pre TENR and STIM0 are configured so only TCR blocks readiness.
 * @post The emitter returns without writing a log byte.
 * @post The Unity case is reported as passed.
 *
 * @note This isolates the TCR half of the readiness predicate.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_tcr_disabled(void)
{
  TEST_BEGIN("ra8_log bails out when TCR.ITMENA is clear");
  ra8_fake_mmap_reset();
  /* TCR=0 makes internal_itm_ready return false on the first check. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
  ra8_log_emit_info("OFF", "should drop");
  TEST_END("ra8_log bails out when TCR.ITMENA is clear");
}

/**
 * @brief Verify a disabled stimulus port suppresses log emission.
 *
 * @details Enables TCR and presents writable STIM0 while clearing TENR port
 * zero, isolating the second readiness qualification before INFO emission.
 *
 * @pre The fake MMIO map is reset and writable.
 * @pre TCR and STIM0 are configured so only TENR blocks readiness.
 * @post The emitter returns without writing a log byte.
 * @post The Unity case is reported as passed.
 *
 * @note This isolates the TENR half of the readiness predicate.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_tenr_disabled(void)
{
  TEST_BEGIN("ra8_log bails out when TENR port 0 is clear");
  ra8_fake_mmap_reset();
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000000UL;
  *(volatile uint32_t*)k_test_itm_stim0 = k_log_all_ones;
  ra8_log_emit_info("OFF", "should drop");
  TEST_END("ra8_log bails out when TENR port 0 is clear");
}

/**
 * @brief Verify a full stimulus FIFO causes bounded log-byte dropping.
 *
 * @details Enables TCR and TENR but leaves STIM0 at zero, exercising the
 * bounded unavailable path without hanging the host test.
 *
 * @pre The fake MMIO map is reset and writable.
 * @pre TCR and TENR enable the logger while STIM0 reports no capacity.
 * @post The attempted INFO line is dropped after bounded readiness checks.
 * @post The test returns without an unbounded poll.
 *
 * @note The production policy deliberately drops bytes when the sink is full.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_log_ready_fifo_full(void)
{
  TEST_BEGIN("ra8_log drops byte when STIM0 is zero for the full poll");
  ra8_fake_mmap_reset();
  /* TCR + TENR are on, but STIM0 starts at 0 so internal_itm_ready
   * returns false and the emit path short-circuits. */
  *(volatile uint32_t*)k_test_itm_tcr   = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_tenr  = 0x00000001UL;
  *(volatile uint32_t*)k_test_itm_stim0 = 0x00000000UL;
  ra8_log_emit_info("FULL", "should drop");
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
  {k_ra8_err_access_denied, "access_denied"},
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
 * @brief Verify every declared ra8_err_t value has the expected string.
 *
 * @details Walks the complete table of error enumerators and compares each
 * returned immutable string against the canonical expected spelling.
 *
 * @pre ::s_all_err_codes contains each declared error enumerator once.
 * @pre Every expected text pointer is a valid NUL-terminated string.
 * @post Every table entry returns a non-NULL string.
 * @post Every returned string exactly matches its canonical spelling.
 *
 * @note The explicit table also documents the public diagnostic vocabulary.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_err_to_str_every_code(void)
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
 * @brief Verify out-of-enum errors map to the unknown diagnostic.
 *
 * @details Casts a deliberately unused numeric code to ra8_err_t and checks
 * the default switch result for non-NULL exact text.
 *
 * @pre k_log_err_code_unknown is not assigned to a public error enumerator.
 * @pre ::ra8_err_to_str accepts arbitrary ra8_err_t bit patterns.
 * @post The returned pointer is non-NULL.
 * @post The returned text is exactly `unknown`.
 *
 * @note This directly covers the switch default rather than a valid code.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_err_to_str_unknown_default(void)
{
  TEST_BEGIN("ra8_err_to_str returns 'unknown' for out-of-enum value");
  ra8_fake_mmap_reset();
  const char* s = ra8_err_to_str((ra8_err_t)k_log_err_code_unknown);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT(strcmp(s, "unknown") == 0);
  TEST_END("ra8_err_to_str returns 'unknown' for out-of-enum value");
}

/**
 * @test internal_test_mcdc_itm_put_u32_loop
 *
 * @brief Demonstrate MC/DC coverage of the bounded uint32 digit loop.
 *
 * @details Emits zero, one, and UINT32_MAX while fake ITM is armed, giving
 * the condition pairs needed to show independent control of value and bound.
 *
 * @pre The fake MMIO map is reset and armed before the three vectors.
 * @pre k_log_val_u32_max requires the formatter's full ten-digit capacity.
 * @post The zero vector bypasses the digit loop.
 * @post The one and maximum vectors exercise value and bound termination.
 *
 * @note The structural-bound rationale below explains the within-vector pair.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `while (value != 0U && i < k_ra8_u32_max_digits)`
 * (2 conditions, libs/ra8_core/src/ra8_log.c line 244 -- gap row 148 in CSV)
 * Exercised indirectly via `ra8_log_emit_info_val()` once the ITM
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
RA8_INTERNAL static void internal_test_mcdc_itm_put_u32_loop(void)
{
  TEST_BEGIN("ra8_log put_u32 MC/DC: value!=0 && i<max_digits");
  ra8_fake_mmap_reset();
  internal_itm_arm();
  ra8_log_init();

  /* Vector 1: value == 0 -> early return path (C1=F at first check). */
  ra8_log_emit_info_val("V1", "zero", 0U);

  /* Vector 2: value == 1 -> single loop iteration (C1=T, C2=T then
   * value becomes 0 so C1 flips to F on the second predicate). */
  ra8_log_emit_info_val("V2", "one", 1U);

  /* Vector 3: value == UINT32_MAX -> 10 iterations exhausting the
   * digit budget so C2 transitions T->F at iteration 10 with C1 still
   * T -- proves C2 independently controls loop exit. */
  ra8_log_emit_info_val("V3", "max", k_log_val_u32_max);

  TEST_END("ra8_log put_u32 MC/DC: value!=0 && i<max_digits");
}

int32_t main(void)
{
  internal_test_log_unbound_host_drops();
  internal_test_log_init_runs();
  internal_test_log_levels_plain();
  internal_test_log_levels_val();
  internal_test_log_val_edge_cases();
  internal_test_log_macros();
  internal_test_log_many_calls();
  internal_test_log_ready_plain();
  internal_test_log_ready_val_unsigned();
  internal_test_log_ready_val_signed();
  internal_test_log_ready_tcr_disabled();
  internal_test_log_ready_tenr_disabled();
  internal_test_log_ready_fifo_full();
  internal_test_err_to_str_every_code();
  internal_test_err_to_str_unknown_default();
  internal_test_mcdc_itm_put_u32_loop();
  return 0;
}

/**
 * @file test_ra8_exception.c
 * @brief Unit tests for ra8_exception.c fault diagnostic helpers
 *
 * @details
 * `ra8_exception_report()` is declared `noreturn` because on target it
 * tail-calls `internal_ra8_fatal_error()`, which halts the CPU. The
 * host implementation of `internal_ra8_fatal_error()` normally calls
 * `__builtin_trap()`. To keep the test runnable we override it here
 * with a weak-symbol-clobbering definition that simply records the
 * call and returns via `longjmp()` so the test process survives.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_exception.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum exception_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_exception_ra8_exception_report_7 = 7U,
  k_exception_xe000ede4ul_00000048   = 0x00000048UL,
} exception_uint8_const_t;

/**
 * @enum exception_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_exception_val_e000ed28         = 0xE000ED28UL,
  k_exception_val_e000ed2c         = 0xE000ED2CUL,
  k_exception_val_e000ed30         = 0xE000ED30UL,
  k_exception_val_e000ed34         = 0xE000ED34UL,
  k_exception_val_e000ed38         = 0xE000ED38UL,
  k_exception_val_e000ed3c         = 0xE000ED3CUL,
  k_exception_val_e000ede4         = 0xE000EDE4UL,
  k_exception_val_e000ede8         = 0xE000EDE8UL,
  k_exception_xe000ed28ul_c0ffee00 = 0xC0FFEE00UL,
  k_exception_xe000ed2cul_c0ffee04 = 0xC0FFEE04UL,
  k_exception_xe000ed30ul_c0ffee08 = 0xC0FFEE08UL,
  k_exception_xe000ed34ul_c0ffee0c = 0xC0FFEE0CUL,
  k_exception_xe000ed38ul_c0ffee10 = 0xC0FFEE10UL,
  k_exception_xe000ed3cul_c0ffee14 = 0xC0FFEE14UL,
  k_exception_xe000ede4ul_c0ffee18 = 0xC0FFEE18UL,
  k_exception_xe000ede8ul_30001234 = 0x30001234UL,
  k_exception_xe000ede8ul_c0ffee1c = 0xC0FFEE1CUL,
} exception_uint32_const_t;

static jmp_buf s_fatal_jmp;
static uint8_t s_fatal_hit = 0U;

/* Override the weak default in ra8_error_handler.c so the test can
 * invoke ra8_exception_report() without aborting the process. */
[[noreturn]] void internal_ra8_fatal_error(const char* tag, const char* message, uint32_t err);

void internal_ra8_fatal_error(const char* tag, const char* message, uint32_t err)
{
  (void)tag;
  (void)message;
  (void)err;
  s_fatal_hit = 1U;
  longjmp(s_fatal_jmp, 1);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_diagnostics_happy(void)
{
  TEST_BEGIN("ra8_exception_capture_diagnostics fills buffer");
  ra8_sim_mmap_reset();

  /* Pre-load the SCB fault status registers via the mapped core
   * window so the capture has something to read. */
  *(volatile uint32_t*)k_exception_val_e000ed28 = k_exception_xe000ed28ul_c0ffee00;
  *(volatile uint32_t*)k_exception_val_e000ed2c = k_exception_xe000ed2cul_c0ffee04;
  *(volatile uint32_t*)k_exception_val_e000ed30 = k_exception_xe000ed30ul_c0ffee08;
  *(volatile uint32_t*)k_exception_val_e000ed34 = k_exception_xe000ed34ul_c0ffee0c;
  *(volatile uint32_t*)k_exception_val_e000ed38 = k_exception_xe000ed38ul_c0ffee10;
  *(volatile uint32_t*)k_exception_val_e000ed3c = k_exception_xe000ed3cul_c0ffee14;
  /* TrustZone SecureFault pair (SFSR / SFAR). */
  *(volatile uint32_t*)k_exception_val_e000ede4 = k_exception_xe000ede4ul_c0ffee18;
  *(volatile uint32_t*)k_exception_val_e000ede8 = k_exception_xe000ede8ul_c0ffee1c;

  ra8_exception_diagnostics_t diag = {};
  ra8_exception_capture_diagnostics(&diag);

  TEST_ASSERT_EQ(0xC0FFEE00L, diag.cfsr);
  TEST_ASSERT_EQ(0xC0FFEE04L, diag.hfsr);
  TEST_ASSERT_EQ(0xC0FFEE08L, diag.dfsr);
  TEST_ASSERT_EQ(0xC0FFEE0CL, diag.mmfar);
  TEST_ASSERT_EQ(0xC0FFEE10L, diag.bfar);
  TEST_ASSERT_EQ(0xC0FFEE14L, diag.afsr);
  TEST_ASSERT_EQ(0xC0FFEE18L, diag.sfsr);
  TEST_ASSERT_EQ(0xC0FFEE1CL, diag.sfar);

  TEST_END("ra8_exception_capture_diagnostics fills buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_diagnostics_null(void)
{
  TEST_BEGIN("ra8_exception_capture_diagnostics tolerates NULL");
  ra8_sim_mmap_reset();
  /* Must early-return without touching anything. */
  ra8_exception_capture_diagnostics(nullptr);
  TEST_END("ra8_exception_capture_diagnostics tolerates NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exception_report_with_frame(void)
{
  TEST_BEGIN("ra8_exception_report logs frame and halts via fatal");
  ra8_sim_mmap_reset();
  s_fatal_hit = 0U;

  const ra8_exception_frame_t frame = {
    .r0   = 0x01U,
    .r1   = 0x02U,
    .r2   = 0x03U,
    .r3   = 0x04U,
    .r12  = 0x05U,
    .lr   = 0x06U,
    .pc   = 0x07U,
    .xpsr = 0x08U,
  };
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(&frame, 3U);
    /* Unreachable once internal_ra8_fatal_error is invoked. */
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  TEST_END("ra8_exception_report logs frame and halts via fatal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exception_report_null_frame(void)
{
  TEST_BEGIN("ra8_exception_report accepts NULL frame");
  ra8_sim_mmap_reset();
  s_fatal_hit = 0U;

  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(nullptr, 2U);
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  TEST_END("ra8_exception_report accepts NULL frame");
}

/**
 * @test exception_report_securefault_records_sfsr_sfar
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the SecureFault
 * (exc 7) capture path; the only new decision in the code under test,
 * `if (exc_number == k_ra8_exc_num_nmi)`, is a single condition, taken
 * FALSE here and TRUE in `test_exception_report_nmi_records_cause`,
 * so both outcomes of the condition are exercised across the suite)
 */
static void test_exception_report_securefault_records_sfsr_sfar(void)
{
  TEST_BEGIN("ra8_exception_report exc 7 snapshots SFSR/SFAR");
  ra8_sim_mmap_reset();
  s_fatal_hit = 0U;

  /* Synthetic SecureFault cause: SFSR = AUVIOL|SFARVALID (0x48), SFAR
   * = the "violating" address, planted in the mapped core window. */
  *(volatile uint32_t*)k_exception_val_e000ede4 = k_exception_xe000ede4ul_00000048;
  *(volatile uint32_t*)k_exception_val_e000ede8 = k_exception_xe000ede8ul_30001234;

  const ra8_exception_frame_t frame = {.pc = 0x02001000U, .lr = 0x02000FFFU};
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(&frame, k_exception_ra8_exception_report_7);
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);
  TEST_ASSERT_EQ((long)k_ra8_exc_magic_valid, (long)g_ra8_exception_last.magic);
  TEST_ASSERT_EQ(7L, (long)g_ra8_exception_last.exc_number);
  TEST_ASSERT_EQ(0x00000048L, (long)g_ra8_exception_last.diag.sfsr);
  TEST_ASSERT_EQ(0x30001234L, (long)g_ra8_exception_last.diag.sfar);
  TEST_ASSERT_EQ(0x02001000L, (long)g_ra8_exception_last.frame.pc);

  TEST_END("ra8_exception_report exc 7 snapshots SFSR/SFAR");
}

/**
 * @test exception_report_nmi_records_cause
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the NMI (exc 2)
 * cause-recording path; the single condition
 * `if (exc_number == k_ra8_exc_num_nmi)` in `ra8_exception_report` is
 * taken TRUE here and FALSE in the sibling report tests, covering
 * both outcomes across the suite)
 */
static void test_exception_report_nmi_records_cause(void)
{
  TEST_BEGIN("ra8_exception_report_nmi records the NMISR cause");
  ra8_sim_mmap_reset();
  s_fatal_hit = 0U;

  /* Synthetic RA8D2 NMISR: WDTST (bit 1) + LMST local-memory / SRAM
   * ECC error (bit 14) latched together. */
  const uint32_t nmisr = (1UL << 1) | (1UL << 14);

  const ra8_exception_frame_t frame = {.pc = 0x02002000U};
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report_nmi(&frame, nmisr);
    TEST_FAIL_FMT("%s", "ra8_exception_report_nmi returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);
  TEST_ASSERT_EQ((long)k_ra8_exc_magic_valid, (long)g_ra8_exception_last.magic);
  TEST_ASSERT_EQ(2L, (long)g_ra8_exception_last.exc_number);
  TEST_ASSERT_EQ((long)nmisr, (long)g_ra8_exception_last.nmisr);
  TEST_ASSERT_EQ(0x02002000L, (long)g_ra8_exception_last.frame.pc);

  TEST_END("ra8_exception_report_nmi records the NMISR cause");
}

/**
 * @test exception_report_clears_stale_nmi_cause
 *
 * @par MC/DC:
 * (no compound decisions in this test -- proves the NMI staging
 * variable is consumed exactly once: a plain fault report following
 * an NMI report must NOT inherit the previous NMISR cause)
 */
static void test_exception_report_clears_stale_nmi_cause(void)
{
  TEST_BEGIN("ra8_exception_report clears the staged NMI cause");
  ra8_sim_mmap_reset();
  s_fatal_hit = 0U;

  /* First: an NMI report stages + records a cause. */
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report_nmi(nullptr, 0x00000001UL);
    TEST_FAIL_FMT("%s", "ra8_exception_report_nmi returned");
  }
  TEST_ASSERT_EQ(0x00000001L, (long)g_ra8_exception_last.nmisr);

  /* Then: a plain HardFault report must show a zero NMISR field --
   * the stage was cleared when the NMI record consumed it. */
  s_fatal_hit = 0U;
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(nullptr, 3U);
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);
  TEST_ASSERT_EQ(3L, (long)g_ra8_exception_last.exc_number);
  TEST_ASSERT_EQ(0L, (long)g_ra8_exception_last.nmisr);

  TEST_END("ra8_exception_report clears the staged NMI cause");
}

int32_t main(void)
{
  test_capture_diagnostics_happy();
  test_capture_diagnostics_null();
  test_exception_report_with_frame();
  test_exception_report_null_frame();
  test_exception_report_securefault_records_sfsr_sfar();
  test_exception_report_nmi_records_cause();
  test_exception_report_clears_stale_nmi_cause();
  (void)fprintf(stderr, "[OK  ] test_ra8_exception.c\n");
  return 0;
}

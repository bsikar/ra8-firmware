/**
 * @file test_ra8_crashlog.c
 * @brief Unit tests for the cross-reset crash-log + reset-loop guard.
 *
 * @details
 * Exercises the ra8_crashlog record lifecycle entirely in-process (there is
 * no reset to survive in a single host test): fill via
 * `ra8_crashlog_record_fault`, validate + copy via `ra8_crashlog_peek`,
 * consume + guard-reset via `ra8_crashlog_claim`, the monotonic + saturating
 * `boot_loops` counter, the safe-mode threshold, and corrupted-record
 * rejection. The `.noinit` record is a plain zero-init static on the host
 * (`RA8_SIMULATOR_MODE`), so the white-box `ra8_crashlog_test_*` accessors
 * (ra8_crashlog_internal.h) plant specific `magic` / `crc` byte patterns to
 * drive the validation decision through its MC/DC vectors. One test also
 * proves the genuine `ra8_exception_report` -> persist-hook -> record path,
 * using the same longjmp-based fatal-hook override as test_ra8_exception.c
 * so the `noreturn` reporter can be called without aborting the process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdint.h>

#include "ra8_crashlog.h"
#include "ra8_crashlog_internal.h"
#include "ra8_exception.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum crashlog_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_crashlog_cfsr_usage_fault =
    0x02000000UL, /**< A CFSR value with a usage-fault bit set, so the decoded record names a specific cause rather than an empty one. */
  k_crashlog_crc_flip_mask =
    0xFFFFFFFFUL, /**< XORed into a valid CRC to corrupt every bit of it, so the integrity check cannot pass by chance. */
  k_crashlog_magic_wrong =
    0xDEADBEEFUL, /**< A magic value the reader does not recognise, so a stale or foreign record is rejected. */
} crashlog_uint32_const_t;

static jmp_buf s_fatal_jmp;
static uint8_t s_fatal_hit = 0U;

/* Override the weak default in ra8_error_handler.c so a test can invoke the
 * noreturn ra8_exception_report() without aborting the process. */
[[noreturn]] void internal_ra8_fatal_error(const char* tag, const char* message, uint32_t err);

void internal_ra8_fatal_error(const char* tag, const char* message, uint32_t err)
{
  (void)tag;
  (void)message;
  (void)err;
  s_fatal_hit = 1U;
  longjmp(s_fatal_jmp, 1);
}

/* Build a fully-populated decoded snapshot with distinguishable fields. */
static ra8_exception_last_t make_synth(uint32_t exc, uint32_t pc)
{
  ra8_exception_last_t s = {};
  s.magic                = (uint32_t)k_ra8_exc_magic_valid;
  s.exc_number           = exc;
  s.frame.pc             = pc;
  s.frame.lr             = pc - 4U;
  s.diag.cfsr            = k_crashlog_cfsr_usage_fault;
  s.nmisr                = 0U;
  return s;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- after a wipe the record is empty,
 * so ra8_crashlog_peek()/safe_mode_requested() must both report "none")
 */
static void test_crashlog_empty_after_wipe(void)
{
  TEST_BEGIN("ra8_crashlog reads empty after wipe");
  ra8_crashlog_test_wipe();

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(0, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  TEST_END("ra8_crashlog reads empty after wipe");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- records one decoded snapshot and
 * asserts peek returns it verbatim with boot_loops == 1)
 */
static void test_crashlog_record_then_peek(void)
{
  TEST_BEGIN("ra8_crashlog records and reads back a fault");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(6U, 0x0200ABCDUL);
  ra8_crashlog_record_fault(&synth);

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(6L, (long)rec.fault.exc_number);
  TEST_ASSERT_EQ(0x0200ABCDL, (long)rec.fault.frame.pc);
  TEST_ASSERT_EQ((long)k_ra8_exc_magic_valid, (long)rec.fault.magic);
  TEST_ASSERT_EQ(1L, (long)rec.boot_loops);
  TEST_ASSERT_EQ((long)k_ra8_crashlog_magic_valid, (long)rec.magic);

  TEST_END("ra8_crashlog records and reads back a fault");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- null inputs must be rejected:
 * record_fault(NULL) leaves the log empty and peek(NULL) returns false)
 */
static void test_crashlog_null_args(void)
{
  TEST_BEGIN("ra8_crashlog tolerates NULL arguments");
  ra8_crashlog_test_wipe();

  ra8_crashlog_record_fault(nullptr); /* no-op */
  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0); /* still empty */
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(nullptr) ? 1 : 0);

  TEST_END("ra8_crashlog tolerates NULL arguments");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- three recorded faults without a
 * claim must accumulate boot_loops to 3)
 */
static void test_crashlog_loop_counter_climbs(void)
{
  TEST_BEGIN("ra8_crashlog boot_loops climbs across records");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(3U, 0x02000100UL);
  ra8_crashlog_record_fault(&synth);
  ra8_crashlog_record_fault(&synth);
  ra8_crashlog_record_fault(&synth);

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(3L, (long)rec.boot_loops);

  TEST_END("ra8_crashlog boot_loops climbs across records");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- recording far past the ceiling
 * must saturate boot_loops instead of wrapping back below the threshold)
 */
static void test_crashlog_counter_saturates(void)
{
  TEST_BEGIN("ra8_crashlog boot_loops saturates at the ceiling");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(5U, 0x02000200UL);
  /* k_ra8_crashlog_loops_max is 255 in ra8_crashlog.c; overshoot it. */
  enum : uint32_t {
    k_overshoot = 300U, /**< Overshoot. */
  };
  for (uint32_t i = 0U; i < (uint32_t)k_overshoot; i++) {
    ra8_crashlog_record_fault(&synth);
  }

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(255L, (long)rec.boot_loops);

  TEST_END("ra8_crashlog boot_loops saturates at the ceiling");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a claim clears the record and
 * zeroes the guard so the next record restarts boot_loops at 1)
 */
static void test_crashlog_claim_resets_guard(void)
{
  TEST_BEGIN("ra8_crashlog claim clears record and resets the guard");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(4U, 0x02000300UL);
  ra8_crashlog_record_fault(&synth);
  ra8_crashlog_record_fault(&synth); /* boot_loops == 2 */

  ra8_crashlog_claim();
  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0); /* consumed */

  ra8_crashlog_record_fault(&synth); /* guard reset -> restarts at 1 */
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(1L, (long)rec.boot_loops);

  TEST_END("ra8_crashlog claim clears record and resets the guard");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- safe mode must stay clear at the
 * threshold and latch strictly above it; k_ra8_crashlog_loop_threshold is 3)
 */
static void test_crashlog_safe_mode_threshold(void)
{
  TEST_BEGIN("ra8_crashlog safe mode latches above the threshold");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(3U, 0x02000400UL);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_crashlog_loop_threshold; i++) {
    ra8_crashlog_record_fault(&synth); /* boot_loops -> 3 (== threshold) */
  }
  TEST_ASSERT_EQ(0, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  ra8_crashlog_record_fault(&synth); /* boot_loops -> 4 (> threshold) */
  TEST_ASSERT_EQ(1, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  TEST_END("ra8_crashlog safe mode latches above the threshold");
}

/**
 * @test test_mcdc_crashlog_is_valid
 *
 * @par MC/DC:
 * Decision: `magic == k_ra8_crashlog_magic_valid && payload_crc == crc`
 * (2 conditions) in `libs/ra8_core/src/ra8_crashlog.c@ra8_crashlog_is_valid`,
 * reached through ra8_crashlog_peek(). `magic` is not covered by the CRC, so
 * the two conditions vary independently.
 * - Vector 1: magic=valid, crc=correct -> true  (control: both true)
 * - Vector 2: magic=BAD,   crc=correct -> false (varies magic only)
 * - Vector 3: magic=valid, crc=BAD     -> false (varies crc only)
 * Vectors 1+2 prove `magic` independently affects the outcome; 1+3 prove
 * the same for the CRC. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_mcdc_crashlog_is_valid(void)
{
  TEST_BEGIN("ra8_crashlog_is_valid MC/DC (magic && crc)");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = make_synth(6U, 0x02000500UL);
  ra8_crashlog_record_fault(&synth); /* writes valid magic + correct crc */

  volatile ra8_crashlog_record_t* raw = ra8_crashlog_test_record();
  ra8_crashlog_record_t           rec = {};

  /* Vector 1: both conditions true -> valid. */
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);

  /* Vector 2: magic wrong, crc still matches payload (magic is not
   * CRC-covered) -> reject. Isolates the magic condition. */
  const uint32_t good_magic = raw->magic;
  raw->magic                = k_crashlog_magic_wrong;
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0);

  /* Vector 3: restore magic, corrupt the stored crc -> reject. Isolates
   * the crc condition. */
  raw->magic = good_magic;
  raw->crc   = raw->crc ^ k_crashlog_crc_flip_mask;
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0);

  TEST_END("ra8_crashlog_is_valid MC/DC (magic && crc)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- proves the production wiring:
 * ra8_exception_report -> installed persist hook -> record. The single
 * condition inside ra8_exception_report, `if (s_ra8_exception_persist !=
 * nullptr)`, is taken TRUE here; its FALSE outcome is covered by the
 * unhooked reports in test_ra8_exception.c)
 */
static void test_crashlog_hook_persists_report(void)
{
  TEST_BEGIN("ra8_exception_report persists through the installed hook");
  ra8_sim_mmap_reset();
  ra8_crashlog_test_wipe();
  s_fatal_hit = 0U;

  ra8_crashlog_install(); /* arm the persist hook */

  const ra8_exception_frame_t frame = {.pc = 0x0200BEEFUL, .lr = 0x0200BEE0UL};
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(&frame, 3U); /* HardFault */
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(3L, (long)rec.fault.exc_number);
  TEST_ASSERT_EQ(0x0200BEEFL, (long)rec.fault.frame.pc);
  TEST_ASSERT_EQ(1L, (long)rec.boot_loops);

  ra8_exception_set_persist_hook(nullptr); /* disarm for sibling tests */

  TEST_END("ra8_exception_report persists through the installed hook");
}

int32_t main(void)
{
  test_crashlog_empty_after_wipe();
  test_crashlog_record_then_peek();
  test_crashlog_null_args();
  test_crashlog_loop_counter_climbs();
  test_crashlog_counter_saturates();
  test_crashlog_claim_resets_guard();
  test_crashlog_safe_mode_threshold();
  test_mcdc_crashlog_is_valid();
  test_crashlog_hook_persists_report();
  (void)fprintf(stderr, "[OK  ] test_ra8_crashlog.c\n");
  return 0;
}

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
 * (`RA8_OFF_TARGET`), so the white-box `ra8_crashlog_test_*` accessors
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

#include "ra8_attributes.h"
#include "ra8_crashlog.h"
#include "ra8_crashlog_internal.h"
#include "ra8_exception.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum crashlog_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_crashlog_cfsr_usage_fault =
    0x02000000UL, /**< A CFSR value with a usage-fault bit, naming a specific cause, not empty. */
  k_crashlog_crc_flip_mask =
    0xFFFFFFFFUL, /**< XORed into a valid CRC to corrupt every bit; check cannot pass by chance. */
  k_crashlog_magic_wrong =
    0xDEADBEEFUL, /**< Magic value the reader rejects; a stale or foreign record won't pass. */
} crashlog_fixture_t;

static jmp_buf s_fatal_jmp;
static uint8_t s_fatal_hit = 0U;

/* Override the weak default in ra8_error_handler.c so a test can invoke the
 * noreturn ra8_exception_report() without aborting the process. */
[[noreturn]] void ra8_fatal_error(const char* tag, const char* message, uint32_t err);

void ra8_fatal_error(const char* tag, const char* message, uint32_t err)
{
  (void)tag;
  (void)message;
  (void)err;
  s_fatal_hit = 1U;
  longjmp(s_fatal_jmp, 1);
}

/**
 * @brief Build a distinguishable decoded exception snapshot.
 * @details Populates the fields consumed by the crash-log record path while
 * deriving a nearby link-register value from the supplied program counter.
 * @param[in] exc Exception number to store in the synthetic snapshot.
 * @param[in] pc Program-counter value to store in the synthetic frame.
 * @return A fully initialized exception snapshot for one record operation.
 * @retval ra8_exception_last_t Snapshot containing the supplied identifiers.
 * @pre `pc` is at least four so the synthetic link-register subtraction holds.
 * @pre Both arguments are representable by their uint32_t destination fields.
 * @post The returned magic field identifies a valid exception snapshot.
 * @post No shared crash-log or fatal-hook state is modified.
 * @note The fixed CFSR fixture makes stale and misrouted fields visible.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_exception_last_t internal_make_synth(uint32_t exc, uint32_t pc)
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
 * @brief Verify a wiped crash log reports no retained fault.
 * @details Wipes the host fixture, then checks both the record query and safe-mode request.
 * @pre The white-box crash-log wipe hook is linked into the host test.
 * @pre The output record object is writable.
 * @post Peek reports false and does not publish a valid retained record.
 * @post Safe mode remains unrequested.
 * @note This establishes the empty-state baseline for the remaining vectors.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- after a wipe the record is empty,
 * so ra8_crashlog_peek()/safe_mode_requested() must both report "none")
 */
RA8_INTERNAL static void internal_test_crashlog_empty_after_wipe(void)
{
  TEST_BEGIN("ra8_crashlog reads empty after wipe");
  ra8_crashlog_test_wipe();

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(0, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(0, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  TEST_END("ra8_crashlog reads empty after wipe");
}

/**
 * @brief Verify one recorded exception round-trips through peek.
 * @details Records a synthetic HardFault-style snapshot and checks identity fields plus the first loop count.
 * @pre The crash-log fixture can be wiped and written.
 * @pre The synthetic PC fixture is valid for ::internal_make_synth.
 * @post Peek returns the recorded exception and valid crash-log magic.
 * @post The boot-loop counter is exactly one.
 * @note The distinct PC and exception values expose field-routing mistakes.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- records one decoded snapshot and
 * asserts peek returns it verbatim with boot_loops == 1)
 */
RA8_INTERNAL static void internal_test_crashlog_record_then_peek(void)
{
  TEST_BEGIN("ra8_crashlog records and reads back a fault");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(6U, 0x0200ABCDUL);
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
 * @brief Verify null crash-log arguments fail safely.
 * @details Exercises the no-op record call and the rejecting null output query from an empty state.
 * @pre The crash-log fixture can be wiped.
 * @pre No valid record is present after the wipe.
 * @post Recording a null snapshot leaves the log empty.
 * @post Peeking through a null output pointer returns false.
 * @note Both public null guards are covered without dereferencing invalid memory.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- null inputs must be rejected:
 * record_fault(NULL) leaves the log empty and peek(NULL) returns false)
 */
RA8_INTERNAL static void internal_test_crashlog_null_args(void)
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
 * @brief Verify repeated unclaimed faults increment the loop counter.
 * @details Records the same snapshot three times and observes the accumulated guard count.
 * @pre The crash-log fixture starts wiped.
 * @pre The synthetic fixture is valid for repeated recording.
 * @post Peek reports a valid retained record.
 * @post The boot-loop counter equals three.
 * @note No claim occurs between records, so the guard must remain continuous.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- three recorded faults without a
 * claim must accumulate boot_loops to 3)
 */
RA8_INTERNAL static void internal_test_crashlog_loop_counter_climbs(void)
{
  TEST_BEGIN("ra8_crashlog boot_loops climbs across records");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(3U, 0x02000100UL);
  ra8_crashlog_record_fault(&synth);
  ra8_crashlog_record_fault(&synth);
  ra8_crashlog_record_fault(&synth);

  ra8_crashlog_record_t rec = {};
  TEST_ASSERT_EQ(1, ra8_crashlog_peek(&rec) ? 1 : 0);
  TEST_ASSERT_EQ(3L, (long)rec.boot_loops);

  TEST_END("ra8_crashlog boot_loops climbs across records");
}

/**
 * @brief Verify the boot-loop counter saturates instead of wrapping.
 * @details Records beyond the uint8 policy ceiling and reads the final bounded count.
 * @pre The crash-log fixture starts wiped.
 * @pre The overshoot count exceeds the implementation ceiling.
 * @post Peek reports a valid record after all writes.
 * @post The loop counter remains at 255 rather than wrapping.
 * @note Saturation preserves safe-mode evidence across a long reset storm.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- recording far past the ceiling
 * must saturate boot_loops instead of wrapping back below the threshold)
 */
RA8_INTERNAL static void internal_test_crashlog_counter_saturates(void)
{
  TEST_BEGIN("ra8_crashlog boot_loops saturates at the ceiling");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(5U, 0x02000200UL);
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
 * @brief Verify claiming a record resets retained state and the loop guard.
 * @details Builds a two-fault history, claims it, then records once more to prove a fresh sequence.
 * @pre The crash-log fixture starts wiped.
 * @pre The synthetic exception fixture remains stable across all records.
 * @post The claimed record is no longer visible through peek.
 * @post The next recorded fault starts with a loop count of one.
 * @note This covers both consumption and guard-reset responsibilities of claim.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- a claim clears the record and
 * zeroes the guard so the next record restarts boot_loops at 1)
 */
RA8_INTERNAL static void internal_test_crashlog_claim_resets_guard(void)
{
  TEST_BEGIN("ra8_crashlog claim clears record and resets the guard");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(4U, 0x02000300UL);
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
 * @brief Verify safe mode activates strictly above the loop threshold.
 * @details Tests the exact threshold and threshold-plus-one boundary with consecutive records.
 * @pre The crash-log fixture starts wiped.
 * @pre The published threshold is nonzero and below the counter ceiling.
 * @post Safe mode is false when the count equals the threshold.
 * @post Safe mode is true after one additional fault.
 * @note The vector pins the policy's strict greater-than comparison.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- safe mode must stay clear at the
 * threshold and latch strictly above it; k_ra8_crashlog_loop_threshold is 3)
 */
RA8_INTERNAL static void internal_test_crashlog_safe_mode_threshold(void)
{
  TEST_BEGIN("ra8_crashlog safe mode latches above the threshold");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(3U, 0x02000400UL);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_crashlog_loop_threshold; i++) {
    ra8_crashlog_record_fault(&synth); /* boot_loops -> 3 (== threshold) */
  }
  TEST_ASSERT_EQ(0, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  ra8_crashlog_record_fault(&synth); /* boot_loops -> 4 (> threshold) */
  TEST_ASSERT_EQ(1, ra8_crashlog_safe_mode_requested() ? 1 : 0);

  TEST_END("ra8_crashlog safe mode latches above the threshold");
}

/**
 * @test internal_test_mcdc_crashlog_is_valid
 * @brief Prove magic and CRC independently control record validity.
 * @details Plants one valid record, then independently corrupts its uncovered magic and stored CRC.
 * @pre The crash-log fixture exposes its retained raw record to this white-box test.
 * @pre Recording the synthetic fixture produces a valid baseline CRC.
 * @post Only the vector with valid magic and CRC is accepted.
 * @post Each single-condition corruption is rejected.
 * @note The three vectors are the minimal MC/DC set for the two-condition conjunction.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `magic == k_ra8_crashlog_magic_valid && payload_crc == crc`
 * (2 conditions) in `libs/ra8_core/src/ra8_crashlog.c@internal_crashlog_is_valid`,
 * reached through ra8_crashlog_peek(). `magic` is not covered by the CRC, so
 * the two conditions vary independently.
 * - Vector 1: magic=valid, crc=correct -> true  (control: both true)
 * - Vector 2: magic=BAD,   crc=correct -> false (varies magic only)
 * - Vector 3: magic=valid, crc=BAD     -> false (varies crc only)
 * Vectors 1+2 prove `magic` independently affects the outcome; 1+3 prove
 * the same for the CRC. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
RA8_INTERNAL static void internal_test_mcdc_crashlog_is_valid(void)
{
  TEST_BEGIN("ra8_crashlog_is_valid MC/DC (magic && crc)");
  ra8_crashlog_test_wipe();

  const ra8_exception_last_t synth = internal_make_synth(6U, 0x02000500UL);
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
 * @brief Verify exception reporting persists through the installed crash hook.
 * @details Installs the production persist callback, intercepts the fatal tail, and checks the retained fault.
 * @pre Fake core MMIO and the longjmp fatal override are active.
 * @pre The crash-log fixture starts wiped.
 * @post The fatal override is reached and the reported frame is retained.
 * @post The persistence hook is cleared before the test returns.
 * @note This is the suite's end-to-end exception-to-crashlog wiring vector.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- proves the production wiring:
 * ra8_exception_report -> installed persist hook -> record. The single
 * condition inside ra8_exception_report, `if (s_ra8_exception_persist !=
 * nullptr)`, is taken TRUE here; its FALSE outcome is covered by the
 * unhooked reports in test_ra8_exception.c)
 */
RA8_INTERNAL static void internal_test_crashlog_hook_persists_report(void)
{
  TEST_BEGIN("ra8_exception_report persists through the installed hook");
  ra8_fake_mmap_reset();
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
  internal_test_crashlog_empty_after_wipe();
  internal_test_crashlog_record_then_peek();
  internal_test_crashlog_null_args();
  internal_test_crashlog_loop_counter_climbs();
  internal_test_crashlog_counter_saturates();
  internal_test_crashlog_claim_resets_guard();
  internal_test_crashlog_safe_mode_threshold();
  internal_test_mcdc_crashlog_is_valid();
  internal_test_crashlog_hook_persists_report();
  return 0;
}

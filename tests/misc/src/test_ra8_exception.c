/**
 * @file test_ra8_exception.c
 * @brief Unit tests for ra8_exception.c fault diagnostic helpers
 *
 * @details
 * `ra8_exception_report()` is declared `noreturn` because on target it
 * tail-calls `ra8_fatal_error()`, which halts the CPU. The
 * host implementation of `ra8_fatal_error()` normally calls
 * `__builtin_trap()`. To keep the test runnable we override it here
 * with a weak-symbol-clobbering definition that simply records the
 * call and returns via `longjmp()` so the test process survives.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_exception.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"
/**
 * @enum exc_scb_reg_t
 * @brief Armv8-M System Control Block fault registers this test plants values in.
 *
 * @details
 * The host build maps the core's register window (::ra8_fake_mmap), so the test
 * writes each fault-status register directly and then checks that the capture
 * read the right one. Addresses are `uintptr_t` because they are addresses.
 */
typedef enum : uintptr_t {
  k_scb_cfsr_addr  = 0xE000ED28UL, /**< SCB->CFSR:  Configurable Fault Status. */
  k_scb_hfsr_addr  = 0xE000ED2CUL, /**< SCB->HFSR:  HardFault Status.          */
  k_scb_dfsr_addr  = 0xE000ED30UL, /**< SCB->DFSR:  Debug Fault Status.        */
  k_scb_mmfar_addr = 0xE000ED34UL, /**< SCB->MMFAR: MemManage Fault Address.   */
  k_scb_bfar_addr  = 0xE000ED38UL, /**< SCB->BFAR:  BusFault Address.          */
  k_scb_afsr_addr  = 0xE000ED3CUL, /**< SCB->AFSR:  Auxiliary Fault Status.    */
  k_scb_sfsr_addr  = 0xE000EDE4UL, /**< SAU->SFSR:  SecureFault Status.        */
  k_scb_sfar_addr  = 0xE000EDE8UL, /**< SAU->SFAR:  SecureFault Address.       */
} exc_scb_reg_t;

/**
 * @enum exc_planted_t
 * @brief The values planted in those registers, and the SecureFault cause.
 *
 * @details
 * The `k_poison_*` values are a 0xC0FFEE00 ramp, one step per register. Any
 * recognizable value would do; what matters is that no two are equal, so a
 * capture that read CFSR into `hfsr` (or skipped a register and left a stale
 * zero) fails a specific assertion instead of passing.
 */
typedef enum : uint32_t {
  k_poison_cfsr  = 0xC0FFEE00UL, /**< Planted in CFSR.  */
  k_poison_hfsr  = 0xC0FFEE04UL, /**< Planted in HFSR.  */
  k_poison_dfsr  = 0xC0FFEE08UL, /**< Planted in DFSR.  */
  k_poison_mmfar = 0xC0FFEE0CUL, /**< Planted in MMFAR. */
  k_poison_bfar  = 0xC0FFEE10UL, /**< Planted in BFAR.  */
  k_poison_afsr  = 0xC0FFEE14UL, /**< Planted in AFSR.  */
  k_poison_sfsr  = 0xC0FFEE18UL, /**< Planted in SFSR.  */
  k_poison_sfar  = 0xC0FFEE1CUL, /**< Planted in SFAR.  */
  k_sfsr_auviol_sfarvalid =
    0x00000048UL,                       /**< A realistic SFSR cause: AUVIOL together with SFARVALID,
                       so the report must treat SFAR as meaningful.            */
  k_sfar_violating_addr = 0x30001234UL, /**< The faulting address SFSR declares valid. */
} exc_planted_t;

/** @brief Exception number 7: SecureFault. */
typedef enum : uint8_t {
  k_exc_num_securefault = 7U, /**< Passed to ra8_exception_report(). */
} exc_number_t;

static jmp_buf s_fatal_jmp;
static uint8_t s_fatal_hit = 0U;

/* Override the weak default in ra8_error_handler.c so the test can
 * invoke ra8_exception_report() without aborting the process. */
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
 * @brief Verify diagnostic capture reads every mapped fault register.
 * @details Plants distinct values across the fake SCB window and compares every published field.
 * @pre The fake core-register mapping is installed and writable.
 * @pre Each poison constant is distinct from the other register fixtures.
 * @post Every diagnostics member equals its corresponding planted register.
 * @post The fake mapping remains available to sibling test vectors.
 * @note Distinct fixtures make register swaps and omissions individually visible.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_capture_diagnostics_happy(void)
{
  TEST_BEGIN("ra8_exception_capture_diagnostics fills buffer");
  ra8_fake_mmap_reset();

  /* Pre-load the SCB fault status registers via the mapped core
   * window so the capture has something to read. */
  *(volatile uint32_t*)k_scb_cfsr_addr  = k_poison_cfsr;
  *(volatile uint32_t*)k_scb_hfsr_addr  = k_poison_hfsr;
  *(volatile uint32_t*)k_scb_dfsr_addr  = k_poison_dfsr;
  *(volatile uint32_t*)k_scb_mmfar_addr = k_poison_mmfar;
  *(volatile uint32_t*)k_scb_bfar_addr  = k_poison_bfar;
  *(volatile uint32_t*)k_scb_afsr_addr  = k_poison_afsr;
  /* TrustZone SecureFault pair (SFSR / SFAR). */
  *(volatile uint32_t*)k_scb_sfsr_addr = k_poison_sfsr;
  *(volatile uint32_t*)k_scb_sfar_addr = k_poison_sfar;

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
 * @brief Verify diagnostic capture accepts a null destination safely.
 * @details Calls the public capture routine without writable output after resetting fake MMIO.
 * @pre The host fake-MMIO fixture can be reset.
 * @pre No output storage is supplied to the call.
 * @post The function returns without a host fault.
 * @post No caller-visible diagnostic object is published.
 * @note This isolates the public null guard from register-reading behavior.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_capture_diagnostics_null(void)
{
  TEST_BEGIN("ra8_exception_capture_diagnostics tolerates NULL");
  ra8_fake_mmap_reset();
  /* Must early-return without touching anything. */
  ra8_exception_capture_diagnostics(nullptr);
  TEST_END("ra8_exception_capture_diagnostics tolerates NULL");
}

/**
 * @brief Verify framed exception reporting reaches the fatal backend.
 * @details Supplies all stacked-register fields and intercepts the reporter's non-returning fatal tail.
 * @pre Fake MMIO and the longjmp fatal override are active.
 * @pre The supplied frame remains valid until the reporter transfers control.
 * @post The ordinary report call site is not reached again.
 * @post The fatal override records exactly one observation.
 * @note The reporter snapshots the frame before invoking the fatal policy.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_exception_report_with_frame(void)
{
  TEST_BEGIN("ra8_exception_report logs frame and halts via fatal");
  ra8_fake_mmap_reset();
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
    /* Unreachable once ra8_fatal_error is invoked. */
    TEST_FAIL_FMT("%s", "ra8_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  TEST_END("ra8_exception_report logs frame and halts via fatal");
}

/**
 * @brief Verify exception reporting accepts a null stacked frame.
 * @details Invokes the same fatal reporting path without an exception-frame pointer.
 * @pre Fake MMIO and the longjmp fatal override are active.
 * @pre The reporter has no staged NMI cause from this vector.
 * @post The reporter reaches the fatal override without dereferencing null.
 * @post Exactly one fatal observation is recorded.
 * @note A missing hardware frame must not suppress the fault report.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_exception_report_null_frame(void)
{
  TEST_BEGIN("ra8_exception_report accepts NULL frame");
  ra8_fake_mmap_reset();
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
 * @brief Verify a SecureFault retains its secure status and address.
 * @details Plants SFSR and SFAR, reports exception seven, and inspects the published last-fault snapshot.
 * @pre The secure fault registers are writable through fake MMIO.
 * @pre The longjmp fatal override is armed.
 * @post The retained exception number, status, address, and frame PC match the fixtures.
 * @post The fatal override records one completed report.
 * @note The chosen SFSR marks SFAR as meaningful to the fault consumer.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the SecureFault
 * (exc 7) capture path; the only new decision in the code under test,
 * `if (exc_number == k_ra8_exc_num_nmi)`, is a single condition, taken
 * FALSE here and TRUE in `test_exception_report_nmi_records_cause`,
 * so both outcomes of the condition are exercised across the suite)
 */
RA8_INTERNAL static void internal_test_exception_report_securefault_records_sfsr_sfar(void)
{
  TEST_BEGIN("ra8_exception_report exc 7 snapshots SFSR/SFAR");
  ra8_fake_mmap_reset();
  s_fatal_hit = 0U;

  /* Synthetic SecureFault cause: SFSR = AUVIOL|SFARVALID (0x48), SFAR
   * = the "violating" address, planted in the mapped core window. */
  *(volatile uint32_t*)k_scb_sfsr_addr = k_sfsr_auviol_sfarvalid;
  *(volatile uint32_t*)k_scb_sfar_addr = k_sfar_violating_addr;

  const ra8_exception_frame_t frame = {.pc = 0x02001000U, .lr = 0x02000FFFU};
  if (setjmp(s_fatal_jmp) == 0) {
    ra8_exception_report(&frame, k_exc_num_securefault);
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
 * @brief Verify NMI reporting retains the supplied cause mask.
 * @details Reports a watchdog-plus-memory NMI and examines the staged exception snapshot.
 * @pre The longjmp fatal override is armed.
 * @pre The supplied frame remains valid during the report call.
 * @post The retained exception number is NMI and its NMISR equals the supplied mask.
 * @post The retained program counter matches the frame fixture.
 * @note Sibling plain-fault vectors cover the false branch of NMI classification.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the NMI (exc 2)
 * cause-recording path; the single condition
 * `if (exc_number == k_ra8_exc_num_nmi)` in `ra8_exception_report` is
 * taken TRUE here and FALSE in the sibling report tests, covering
 * both outcomes across the suite)
 */
RA8_INTERNAL static void internal_test_exception_report_nmi_records_cause(void)
{
  TEST_BEGIN("ra8_exception_report_nmi records the NMISR cause");
  ra8_fake_mmap_reset();
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
 * @brief Verify a consumed NMI cause cannot leak into a later fault.
 * @details Reports an NMI, then a plain HardFault, checking the second snapshot's staged cause.
 * @pre The longjmp fatal override can be reused for two sequential reports.
 * @pre Fake MMIO is reset before the first report.
 * @post The first snapshot retains the supplied NMI cause.
 * @post The second snapshot reports a zero NMI cause and the new exception number.
 * @note This pins the one-shot lifetime of the internal NMI staging value.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- proves the NMI staging
 * variable is consumed exactly once: a plain fault report following
 * an NMI report must NOT inherit the previous NMISR cause)
 */
RA8_INTERNAL static void internal_test_exception_report_clears_stale_nmi_cause(void)
{
  TEST_BEGIN("ra8_exception_report clears the staged NMI cause");
  ra8_fake_mmap_reset();
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

int main(void)
{
  internal_test_capture_diagnostics_happy();
  internal_test_capture_diagnostics_null();
  internal_test_exception_report_with_frame();
  internal_test_exception_report_null_frame();
  internal_test_exception_report_securefault_records_sfsr_sfar();
  internal_test_exception_report_nmi_records_cause();
  internal_test_exception_report_clears_stale_nmi_cause();
  return 0;
}
